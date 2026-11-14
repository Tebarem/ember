/*
 * idlgen: writes the C glue for the bindings (see idl.h for what is supported).
 *
 * For every interface the generator writes:
 *   - a class (embi_gen_class_<I>) when it needs one of its own (index/named property hooks, Window);
 *     the others share embi_gen_class_plain. Trace and finalize are the binding runtime's;
 *   - a table of member rows (embi_bind_member, bind.h): one row per attribute, operation and
 *     constant, in IDL order, and an embi_bind_iface descriptor (embi_gen_iface_<I>) that the generic
 *     installer (embi_bind_install_iface) turns into the prototype (chained to the parent's), the
 *     interface object (Illegal constructor unless the IDL has a constructor), the members with
 *     their WebIDL property attributes, the constants, @@toStringTag, iterable members and the
 *     global binding;
 *   - the natives that cannot be shared (operation overloads and their dispatchers, toString, the CSS
 *     property accessors, constructors).
 * The native function of every other member is a "shape": its body is generated as text without any
 * mention of the member (the implementation is called through the row, the names in error messages
 * come from the row), and members whose bodies are the same text share one function. The shapes of
 * all files are written to gen/shapes.c. Each does the brand check, the argument conversions with
 * WebIDL messages, the call of the hand-written implementation, and the return conversion.
 * Reflected attributes ([Reflect]) have no native of their own at all: bind_conv.c has one getter
 * and one setter per type, and the row holds the content attribute name.
 * The hand-written implementations are declared in gen/<file>.h as embi_impl_<I>_<member>.
 * [SameObject] caches are generated inline.
 *
 * Shared output (gen/types.h, gen/ifaces.c, gen/cssprops.c, gen/shapes.c) holds the interface ids, the
 * descriptors and classes of every interface, the namespace descriptors, the shared natives, the enums, the dictionary structs and
 * converters, the tag-to-interface table and the CSS property names. Everything is emitted in
 * sorted order, so regenerating after an IDL change touches only what changed.
 */
#include "idl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum {
    CONV_CLAMP = 1,
    CONV_ENFORCE = 2,
    CONV_LEGACY_NTE = 4,
};

typedef struct gen {
    idl_db *db;
    int errors;
    int tmp; /* counter for generated local names */
} gen;

static int g_vtmp;          /* temporaries of statement expressions */
static char **g_props;      /* property names of src/css/props.txt, sorted */
static int g_nprops;

static void gen_error(gen *g, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void gen_error(gen *g, const char *file, int line, const char *fmt, ...)
{
    char buf[IDL_MAX_ERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    idl_report(g->db, file, line, "%s", buf);
    g->errors++;
}

/* Appends formatted text to a fixed buffer without overflowing it. */
static void sbf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void sbf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (k > 0) {
        *len = (*len + (size_t)k < cap) ? *len + (size_t)k : cap - 1;
    }
}

/* ------------------------------------------------------------------ output sinks and shapes */

/* Where generated text goes: a file, or memory (the body of a shared native). */
typedef struct sink {
    FILE *f;
    char *buf;
    size_t len, cap;
} sink;

static void sp(sink *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sp(sink *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s->f) {
        vfprintf(s->f, fmt, ap);
        va_end(ap);
        return;
    }
    for (;;) {
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(s->buf ? s->buf + s->len : nullptr, s->cap - s->len, fmt, cp);
        va_end(cp);
        if (n < 0) {
            exit(2);
        }
        if (s->buf && (size_t)n < s->cap - s->len) {
            s->len += (size_t)n;
            break;
        }
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        while (ncap < s->len + (size_t)n + 1) {
            ncap *= 2;
        }
        char *nb = realloc(s->buf, ncap);
        if (!nb) {
            exit(2);
        }
        s->buf = nb;
        s->cap = ncap;
    }
    va_end(ap);
}

static void sink_reset(sink *s)
{
    s->len = 0;
    if (s->buf) {
        s->buf[0] = '\0';
    }
}

/* A native function shared by every member whose body is the same text (the member itself, its
 * implementation and the names in its error messages are data in its row, see bind.h). They are
 * written to shapes.c by idl_gen_shapes once every file has been generated. */
typedef struct shape {
    char name[32];
    char *text;
    char *example;
    bool shipped; /* used outside testing.webidl, whose gen/testing.c only builds with tests compile */
} shape_ent;

static shape_ent *g_shapes;
static size_t g_nshapes, g_capshapes;

static uint32_t fnv32(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (unsigned char)*s) * 16777619u;
    }
    return h;
}

/* Registers a native body and returns the name of its function in name. */
static void shape_add(const char *text, const char *example, bool shipped, char name[32])
{
    snprintf(name, 32, "embi_shape_%08x", (unsigned)fnv32(text));
    for (size_t i = 0; i < g_nshapes; i++) {
        if (strcmp(g_shapes[i].name, name) == 0) {
            if (strcmp(g_shapes[i].text, text) != 0) {
                fprintf(stderr, "idlgen: shape hash collision between %s and %s\n", g_shapes[i].example, example);
                exit(2);
            }
            g_shapes[i].shipped |= shipped;
            return;
        }
    }
    if (g_nshapes == g_capshapes) {
        g_capshapes = g_capshapes ? g_capshapes * 2 : 256;
        g_shapes = realloc(g_shapes, g_capshapes * sizeof *g_shapes);
        if (!g_shapes) {
            exit(2);
        }
    }
    shape_ent *s = &g_shapes[g_nshapes++];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->text = strdup(text);
    s->example = strdup(example);
    s->shipped = shipped;
}

/* A C string literal with escapes, for text used in a message. */
static void cstr(char *buf, size_t cap, const char *s)
{
    size_t n = 0;
    if (cap < 3) {
        if (cap) {
            buf[0] = '\0';
        }
        return;
    }
    buf[n++] = '"';
    for (const char *p = s; *p && n + 5 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = (char)c;
        } else if (c < 0x20 || c >= 0x7f) {
            n += (size_t)snprintf(buf + n, cap - n, "\\%03o", c);
        } else {
            buf[n++] = (char)c;
        }
    }
    buf[n++] = '"';
    buf[n] = '\0';
}

/* The identifier of an enum value: EMBI_ENUM_<Enum>_<value>, non-alphanumerics replaced. */
static void enum_const(char *buf, size_t cap, const char *en, const char *value)
{
    size_t n = (size_t)snprintf(buf, cap, "EMBI_ENUM_%s_", en);
    for (const char *p = value; *p && n + 1 < cap; p++) {
        buf[n++] = isalnum((unsigned char)*p) ? *p : '_';
    }
    buf[n] = '\0';
}

/* ------------------------------------------------------------------ type helpers */

static bool is_str(const idl_type *t)
{
    return t->kind == IDL_T_DOMSTRING || t->kind == IDL_T_USVSTRING || t->kind == IDL_T_BYTESTRING;
}

static bool is_int(const idl_type *t)
{
    return t->kind >= IDL_T_OCTET && t->kind <= IDL_T_ULONGLONG;
}

static bool is_num(const idl_type *t)
{
    return is_int(t) || t->kind == IDL_T_FLOAT || t->kind == IDL_T_DOUBLE;
}

static bool is_node(const idl_type *t)
{
    return t->kind == IDL_T_INTERFACE && idl_is_node((const idl_iface *)t->target);
}

static const char *int_ctype(uint8_t k)
{
    switch (k) {
    case IDL_T_OCTET: return "uint8_t";
    case IDL_T_BYTE: return "int8_t";
    case IDL_T_SHORT: return "int16_t";
    case IDL_T_USHORT: return "uint16_t";
    case IDL_T_LONG: return "int32_t";
    case IDL_T_ULONG: return "uint32_t";
    case IDL_T_LONGLONG: return "int64_t";
    default: return "uint64_t";
    }
}

/* The EMBI_BIND_* integer kind of the converter (bind.h). */
static int int_kind(uint8_t k)
{
    switch (k) {
    case IDL_T_BYTE: return 0;
    case IDL_T_SHORT: return 1;
    case IDL_T_LONG: return 2;
    case IDL_T_LONGLONG: return 3;
    case IDL_T_OCTET: return 4;
    case IDL_T_USHORT: return 5;
    case IDL_T_ULONG: return 6;
    default: return 7;
    }
}

static bool is_signed_int(uint8_t k)
{
    return k == IDL_T_BYTE || k == IDL_T_SHORT || k == IDL_T_LONG || k == IDL_T_LONGLONG;
}

/* The C type of a value of IDL type t, as an argument of an implementation. */
static void ctype_of(const idl_type *t, char *buf, size_t cap)
{
    const char *s = "void";
    switch (t->kind) {
    case IDL_T_ANY:
    case IDL_T_OBJECT:
    case IDL_T_CALLBACK:
    case IDL_T_PROMISE:
        s = "embi_js_value";
        break;
    case IDL_T_BOOLEAN:
        s = "bool";
        break;
    case IDL_T_FLOAT:
    case IDL_T_DOUBLE:
        s = "double";
        break;
    case IDL_T_ENUM:
        s = "int32_t";
        break;
    case IDL_T_UNION:
        s = "embi_bind_union";
        break;
    case IDL_T_SEQUENCE:
        s = "embi_bind_seq";
        break;
    case IDL_T_RECORD:
        s = "embi_bind_rec";
        break;
    case IDL_T_INTERFACE:
        s = is_node(t) ? "embi_node *" : "void *";
        break;
    case IDL_T_DICT:
        snprintf(buf, cap, "embi_dict_%s", ((const idl_dict *)t->target)->name);
        return;
    default:
        if (is_str(t)) {
            s = t->nullable ? "embi_bind_nstr" : "embi_dstr";
        } else if (is_int(t)) {
            s = int_ctype(t->kind);
        }
        break;
    }
    snprintf(buf, cap, "%s", s);
}

/* True when the implementation returns a finished JS value for this type. */
static bool ret_is_js(const idl_type *t)
{
    switch (t->kind) {
    case IDL_T_ANY:
    case IDL_T_OBJECT:
    case IDL_T_CALLBACK:
    case IDL_T_PROMISE:
    case IDL_T_UNION:
        return true;
    case IDL_T_INTERFACE:
        return !is_node(t);
    case IDL_T_ENUM:
        return false;
    default:
        break;
    }
    if (t->nullable) {
        return !(is_node(t));
    }
    return false;
}

/* The C type a getter or operation returns to its native function. */
static void ret_ctype(const idl_type *t, char *buf, size_t cap)
{
    if (t->kind == IDL_T_VOID) {
        snprintf(buf, cap, "void");
    } else if (ret_is_js(t)) {
        snprintf(buf, cap, "embi_js_value");
    } else if (is_str(t)) {
        snprintf(buf, cap, "embi_dstr");
    } else {
        ctype_of(t, buf, cap);
    }
}

/* Why a type cannot be an argument or an attribute value, or nullptr. */
static const char *unsupported_arg(const idl_type *t)
{
    if (t->nullable && (is_num(t) || t->kind == IDL_T_BOOLEAN)) {
        return "nullable numbers and booleans are not supported as arguments";
    }
    if (t->nullable && (t->kind == IDL_T_SEQUENCE || t->kind == IDL_T_RECORD)) {
        return "nullable sequences and records are not supported as arguments";
    }
    if (t->kind == IDL_T_SEQUENCE) {
        const char *w = unsupported_arg(t->sub[0]);
        return w ? w : nullptr;
    }
    if (t->kind == IDL_T_RECORD) {
        return is_str(t->sub[0]) ? unsupported_arg(t->sub[1]) : "record keys must be strings";
    }
    return nullptr;
}

/* The name a member has in IDL and C (an unnamed indexed getter is called "indexed"). */
static const char *mname(const idl_member *m)
{
    return m->name ? m->name : "indexed";
}

/* ------------------------------------------------------------------ generated symbol names */

/* The implementation a member calls: embi_impl_<I>_<name>, with _<k> for overloads. tag is "get"
 * or "set" for attributes. */
static void impl_name(char *buf, size_t cap, const char *iface, const idl_member *m, const char *tag)
{
    if (m->kind == IDL_M_CTOR) {
        snprintf(buf, cap, "embi_impl_%s_constructor", iface);
    } else if (m->kind == IDL_M_ATTR) {
        snprintf(buf, cap, "embi_impl_%s_%s_%s", iface, tag, m->name);
    } else if (m->kind == IDL_M_STRINGIFIER) {
        snprintf(buf, cap, "embi_impl_%s_stringifier", iface);
    } else if (m->noverloads > 1) {
        snprintf(buf, cap, "embi_impl_%s_%s_%d", iface, m->name, m->overload);
    } else if (m->static_clash) {
        snprintf(buf, cap, "embi_impl_%s_%s_static", iface, m->name);
    } else {
        snprintf(buf, cap, "embi_impl_%s_%s", iface, mname(m));
    }
}

/* The native function of a member (the same pattern, with gen_ in place of embi_impl_). */
static void native_name(char *buf, size_t cap, const char *iface, const idl_member *m, const char *tag)
{
    if (m->kind == IDL_M_CTOR) {
        snprintf(buf, cap, "gen_%s_ctor", iface);
    } else if (m->kind == IDL_M_ATTR) {
        snprintf(buf, cap, "gen_%s_%s_%s", iface, tag, m->name);
    } else if (m->noverloads > 1) {
        snprintf(buf, cap, "gen_%s_%s_%d", iface, m->name, m->overload);
    } else if (m->static_clash) {
        snprintf(buf, cap, "gen_%s_%s_static", iface, m->name);
    } else {
        snprintf(buf, cap, "gen_%s_%s", iface, mname(m));
    }
}

/* The dispatcher of an overloaded operation name. */
static void dispatch_name(char *buf, size_t cap, const char *iface, const char *name)
{
    snprintf(buf, cap, "gen_%s_%s", iface, name);
}

/* ------------------------------------------------------------------ type categories */

static const char *const k_um_names[] = {"EMBI_UM_NODE",   "EMBI_UM_IFACE", "EMBI_UM_CALLBACK",
                                         "EMBI_UM_SEQ",    "EMBI_UM_OBJECT", "EMBI_UM_BOOL",
                                         "EMBI_UM_NUM",    "EMBI_UM_STR",    "EMBI_UM_ANY"};

/* The text of one embi_bind_um initializer for a type. */
static void um_init(const idl_type *t, char *buf, size_t cap)
{
    int iface = 0;
    bool nullable = false;
    int cat = idl_type_cat(t, &iface, &nullable);
    if (t->kind == IDL_T_CALLBACK && t->cb_iface) {
        cat = 4;
    }
    int sub = 0;
    if (cat == 6) {
        sub = is_int(t) ? int_kind(t->kind) : 8;
    }
    char ifs[96] = "0";
    if ((cat == 0 || cat == 1) && t->target) {
        snprintf(ifs, sizeof ifs, "EMBI_IF_%s", ((const idl_iface *)t->target)->name);
    }
    snprintf(buf, cap, "{%s, %d, %s, %s}", k_um_names[cat], sub, nullable ? "true" : "false", ifs);
}

/* ------------------------------------------------------------------ conversions */

/* Emits the conversion of the JS value src into the C lvalue dst of IDL type t. fail is the
 * statement run when the conversion throws (inside a dictionary converter: return false). */
static void emit_conv(gen *g, sink *o, const idl_type *t, unsigned flags, const char *dst,
                      const char *src, const char *ctx, const char *fail, const char *file,
                      int line)
{
    bool nullable = t->nullable;
    const char *why = unsupported_arg(t);
    if (why) {
        gen_error(g, file, line, "%s", why);
        return;
    }
    if (is_int(t)) {
        int kind = int_kind(t->kind);
        if (flags & CONV_CLAMP) {
            kind |= 0x10;
        }
        if (flags & CONV_ENFORCE) {
            kind |= 0x20;
        }
        int k = ++g->tmp;
        sp(o, "    { int64_t t%d = 0;\n      if (!embi_bind_conv_int(c, %s, %d, %s, &t%d)) %s\n", k, src,
                kind, ctx, k, fail);
        sp(o, "      %s = (%s)t%d; }\n", dst, int_ctype(t->kind), k);
    } else if (t->kind == IDL_T_BOOLEAN) {
        sp(o, "    %s = embi_js_to_boolean(%s);\n", dst, src);
    } else if (t->kind == IDL_T_FLOAT || t->kind == IDL_T_DOUBLE) {
        sp(o, "    if (!embi_bind_conv_double(c, %s, %s, %s, &%s)) %s\n", src,
                t->unrestricted ? "true" : "false", ctx, dst, fail);
    } else if (t->kind == IDL_T_ANY) {
        sp(o, "    %s = %s;\n", dst, src);
    } else if (t->kind == IDL_T_PROMISE) {
        sp(o, "    %s = %s;\n", dst, src);
    } else if (t->kind == IDL_T_OBJECT) {
        sp(o, "    if (!embi_bind_conv_object(c, %s, %s, %s, &%s)) %s\n", src,
                nullable ? "true" : "false", ctx, dst, fail);
    } else if (t->kind == IDL_T_CALLBACK) {
        bool iface_obj = t->cb_iface;
        sp(o, "    if (!embi_bind_conv_callback(c, %s, %s, %s, %s, &%s)) %s\n", src,
                nullable ? "true" : "false", iface_obj ? "true" : "false", ctx, dst, fail);
    } else if (is_str(t)) {
        sp(o, "    if (!embi_bind_conv_%s(c, %s, %s, %s, &%s%s)) %s\n", nullable ? "nstr" : "str", src,
                ctx, (flags & CONV_LEGACY_NTE) ? "true" : "false", dst, "", fail);
    } else if (t->kind == IDL_T_ENUM) {
        const idl_enum *e = (const idl_enum *)t->target;
        sp(o,
                "    if (!embi_bind_conv_enum(c, %s, %s, embi_enum_names_%s, EMBI_ENUM_COUNT_%s, %s, &%s)) %s\n",
                src, nullable ? "true" : "false", e->name, e->name, ctx, dst, fail);
    } else if (t->kind == IDL_T_INTERFACE) {
        const idl_iface *i = (const idl_iface *)t->target;
        if (i->id < 0) {
            gen_error(g, file, line, "interface '%s' has no binding id", i->name);
            return;
        }
        if (is_node(t)) {
            sp(o, "    if (!embi_bind_conv_node(c, %s, EMBI_IF_%s, %s, %s, &%s)) %s\n", src,
                    i->name, nullable ? "true" : "false", ctx, dst, fail);
        } else {
            sp(o, "    if (!embi_bind_conv_other(c, %s, EMBI_IF_%s, %s, %s, &%s)) %s\n", src,
                    i->name, nullable ? "true" : "false", ctx, dst, fail);
        }
    } else if (t->kind == IDL_T_DICT) {
        const idl_dict *d = (const idl_dict *)t->target;
        sp(o, "    if (!embi_gen_dict_conv_%s(c, %s, &%s, %s)) %s\n", d->name, src, dst, ctx, fail);
    } else if (t->kind == IDL_T_SEQUENCE) {
        const idl_type *et = t->sub[0];
        char ect[160];
        ctype_of(et, ect, sizeof ect);
        int k = ++g->tmp;
        sp(o, "    { embi_js_value *it%d = nullptr;\n      uint32_t n%d = 0;\n", k, k);
        sp(o, "      if (!embi_bind_seq_open(c, %s, %s, &it%d, &n%d)) %s\n", src, ctx, k, k, fail);
        sp(o, "      %s *arr%d = (%s *)embi_bind_alloc(c, (size_t)n%d * sizeof(%s));\n", ect, k, ect, k, ect);
        sp(o, "      for (uint32_t i%d = 0; i%d < n%d; i%d++) {\n", k, k, k, k);
        sp(o, "      arr%d[i%d] = (%s){0};\n", k, k, ect);
        char d2[64], s2[64];
        snprintf(d2, sizeof d2, "arr%d[i%d]", k, k);
        snprintf(s2, sizeof s2, "it%d[i%d]", k, k);
        emit_conv(g, o, et, 0, d2, s2, ctx, fail, file, line);
        sp(o, "      }\n      %s.data = arr%d;\n      %s.len = n%d; }\n", dst, k, dst, k);
    } else if (t->kind == IDL_T_RECORD) {
        const idl_type *vt = t->sub[1];
        char vct[160];
        ctype_of(vt, vct, sizeof vct);
        int k = ++g->tmp;
        sp(o, "    { embi_dstr *rk%d = nullptr;\n      embi_js_value *rv%d = nullptr;\n      uint32_t n%d = 0;\n", k, k, k);
        sp(o, "      if (!embi_bind_rec_open(c, %s, %s, &rk%d, &rv%d, &n%d)) %s\n", src, ctx, k, k, k, fail);
        sp(o, "      %s *vals%d = (%s *)embi_bind_alloc(c, (size_t)n%d * sizeof(%s));\n", vct, k, vct, k, vct);
        sp(o, "      for (uint32_t i%d = 0; i%d < n%d; i%d++) {\n", k, k, k, k);
        sp(o, "      vals%d[i%d] = (%s){0};\n", k, k, vct);
        char d2[64], s2[64];
        snprintf(d2, sizeof d2, "vals%d[i%d]", k, k);
        snprintf(s2, sizeof s2, "rv%d[i%d]", k, k);
        emit_conv(g, o, vt, 0, d2, s2, ctx, fail, file, line);
        sp(o, "      }\n      %s.keys = rk%d;\n      %s.vals = vals%d;\n      %s.len = n%d; }\n", dst, k, dst, k,
                dst, k);
    } else if (t->kind == IDL_T_UNION) {
        int k = ++g->tmp;
        sp(o, "    { static const embi_bind_um um%d[] = {", k);
        for (int i = 0; i < t->nsub; i++) {
            char u[200];
            um_init(t->sub[i], u, sizeof u);
            sp(o, "%s%s", i ? ", " : "", u);
        }
        sp(o, "};\n      if (!embi_bind_conv_union(c, %s, um%d, %d, %s, %s, &%s)) %s\n", src, k, t->nsub,
                nullable ? "true" : "false", ctx, dst, fail);
        for (int i = 0; i < t->nsub; i++) {
            const idl_type *m = t->sub[i];
            if (m->kind == IDL_T_DICT) {
                const idl_dict *d = (const idl_dict *)m->target;
                sp(o, "      if (%s.kind == %d) {\n", dst, i + 1);
                sp(o, "        embi_dict_%s *dd = (embi_dict_%s *)embi_bind_alloc(c, sizeof *dd);\n", d->name, d->name);
                sp(o, "        SDL_memset(dd, 0, sizeof *dd);\n");
                sp(o, "        if (!embi_gen_dict_conv_%s(c, %s.u.v, dd, %s)) %s\n", d->name, dst, ctx, fail);
                sp(o, "        %s.u.ptr = dd;\n      }\n", dst);
            }
        }
        sp(o, "    }\n");
    } else {
        gen_error(g, file, line, "cannot convert this type");
    }
}

/* The C expression of an optional argument's default, or of the zero value of an absent one.
 * Dictionaries are not in it: an absent dictionary converts as undefined. */
static void default_expr(const idl_type *t, const char *def, char *buf, size_t cap)
{
    if (def && strcmp(def, "null") == 0) {
        if (is_str(t)) {
            snprintf(buf, cap, "(embi_bind_nstr){{nullptr, 0, false}, true}");
        } else if (t->kind == IDL_T_ANY || t->kind == IDL_T_OBJECT || t->kind == IDL_T_CALLBACK) {
            snprintf(buf, cap, "EMBI_JS_NULL");
        } else if (t->kind == IDL_T_ENUM) {
            snprintf(buf, cap, "-1");
        } else {
            snprintf(buf, cap, "nullptr");
        }
        return;
    }
    if (def && (strcmp(def, "true") == 0 || strcmp(def, "false") == 0)) {
        snprintf(buf, cap, "%s", def);
        return;
    }
    if (def && def[0] == '"') {
        size_t n = strlen(def) - 2;
        if (t->kind == IDL_T_ENUM) {
            const idl_enum *e = (const idl_enum *)t->target;
            for (int i = 0; i < e->values.n; i++) {
                if (strlen(e->values.items[i]) == n && strncmp(e->values.items[i], def + 1, n) == 0) {
                    char cname[256];
                    enum_const(cname, sizeof cname, e->name, e->values.items[i]);
                    snprintf(buf, cap, "%s", cname);
                    return;
                }
            }
            snprintf(buf, cap, "0");
            return;
        }
        if (t->nullable && is_str(t)) {
            snprintf(buf, cap, "(embi_bind_nstr){{(const void *)\"%.*s\", %zu, false}, false}", (int)n, def + 1, n);
        } else {
            snprintf(buf, cap, "(embi_dstr){(const void *)\"%.*s\", %zu, false}", (int)n, def + 1, n);
        }
        return;
    }
    if (def && (is_int(t) || t->kind == IDL_T_FLOAT || t->kind == IDL_T_DOUBLE)) {
        if (strcmp(def, "Infinity") == 0) {
            snprintf(buf, cap, "(1.0 / 0.0)");
        } else if (strcmp(def, "-Infinity") == 0) {
            snprintf(buf, cap, "(-1.0 / 0.0)");
        } else if (strcmp(def, "NaN") == 0) {
            snprintf(buf, cap, "(0.0 / 0.0)");
        } else {
            snprintf(buf, cap, "%s", def);
        }
        return;
    }
    if (def && strcmp(def, "[]") == 0 && t->kind == IDL_T_SEQUENCE) {
        snprintf(buf, cap, "(embi_bind_seq){nullptr, 0}");
        return;
    }
    /* No default: the zero value of the type. */
    switch (t->kind) {
    case IDL_T_ANY:
    case IDL_T_OBJECT:
    case IDL_T_CALLBACK:
    case IDL_T_PROMISE:
        snprintf(buf, cap, "EMBI_JS_UNDEFINED");
        break;
    case IDL_T_BOOLEAN:
        snprintf(buf, cap, "false");
        break;
    case IDL_T_FLOAT:
    case IDL_T_DOUBLE:
        snprintf(buf, cap, "0.0");
        break;
    case IDL_T_UNION:
        snprintf(buf, cap, "(embi_bind_union){0}");
        break;
    case IDL_T_SEQUENCE:
        snprintf(buf, cap, "(embi_bind_seq){nullptr, 0}");
        break;
    case IDL_T_RECORD:
        snprintf(buf, cap, "(embi_bind_rec){nullptr, nullptr, 0}");
        break;
    case IDL_T_INTERFACE:
        snprintf(buf, cap, "nullptr");
        break;
    case IDL_T_DICT:
        snprintf(buf, cap, "(embi_dict_%s){0}", ((const idl_dict *)t->target)->name);
        break;
    default:
        if (is_str(t)) {
            snprintf(buf, cap, t->nullable ? "(embi_bind_nstr){{nullptr, 0, false}, false}"
                                           : "(embi_dstr){nullptr, 0, false}");
        } else {
            snprintf(buf, cap, "0");
        }
        break;
    }
}

/* The C expression that turns a finished value into a JS value (the return conversion). */
static void value_expr(const idl_type *t, const char *ret, char *buf, size_t cap)
{
    if (t->kind == IDL_T_VOID) {
        snprintf(buf, cap, "EMBI_JS_UNDEFINED");
    } else if (ret_is_js(t)) {
        snprintf(buf, cap, "%s", ret);
    } else if (t->kind == IDL_T_BOOLEAN) {
        snprintf(buf, cap, "embi_js_from_bool(%s)", ret);
    } else if (t->kind == IDL_T_FLOAT || t->kind == IDL_T_DOUBLE || is_int(t)) {
        snprintf(buf, cap, "embi_bind_num((double)%s)", ret);
    } else if (is_str(t)) {
        snprintf(buf, cap, "embi_bind_dstr_value(c, %s)", ret);
    } else if (t->kind == IDL_T_ENUM) {
        const idl_enum *e = (const idl_enum *)t->target;
        snprintf(buf, cap, "embi_bind_enum_value(c, embi_enum_names_%s, EMBI_ENUM_COUNT_%s, %s)", e->name,
                 e->name, ret);
    } else if (t->kind == IDL_T_INTERFACE) {
        snprintf(buf, cap, "embi_bind_node_value(c, %s)", ret);
    } else if (t->kind == IDL_T_SEQUENCE) {
        int k = ++g_vtmp;
        char ect[160], el[64], ev[512];
        ctype_of(t->sub[0], ect, sizeof ect);
        snprintf(el, sizeof el, "se%d_", k);
        value_expr(t->sub[0], el, ev, sizeof ev);
        int w = snprintf(buf, cap,
                         "({ embi_bind_seq sq%d_ = %s; embi_js_value ar%d_ = embi_bind_array_new(c, sq%d_.len); "
                         "for (uint32_t si%d_ = 0; si%d_ < sq%d_.len; si%d_++) { %s se%d_ = ((%s *)sq%d_.data)[si%d_]; "
                         "embi_bind_array_set(c, ar%d_, si%d_, %s); } ar%d_; })",
                         k, ret, k, k, k, k, k, k, ect[0] ? ect : "int", k, ect, k, k, k, k, ev, k);
        if (w < 0 || (size_t)w >= cap) {
            exit(2); /* the generated expression does not fit: a generator limit, not an IDL error */
        }
    } else if (t->kind == IDL_T_RECORD) {
        int k = ++g_vtmp;
        char vct[160], vv[64], ve[512];
        ctype_of(t->sub[1], vct, sizeof vct);
        snprintf(vv, sizeof vv, "rv%d_", k);
        value_expr(t->sub[1], vv, ve, sizeof ve);
        int w = snprintf(buf, cap,
                         "({ embi_bind_rec rc%d_ = %s; embi_js_value ro%d_ = embi_bind_object_new(c); "
                         "for (uint32_t ri%d_ = 0; ri%d_ < rc%d_.len; ri%d_++) { %s rv%d_ = ((%s *)rc%d_.vals)[ri%d_]; "
                         "embi_bind_object_set(c, ro%d_, rc%d_.keys[ri%d_], %s); } ro%d_; })",
                         k, ret, k, k, k, k, k, vct, k, vct, k, k, k, k, k, ve, k);
        if (w < 0 || (size_t)w >= cap) {
            exit(2); /* the generated expression does not fit: a generator limit, not an IDL error */
        }
    } else if (t->kind == IDL_T_DICT) {
        const idl_dict *d = (const idl_dict *)t->target;
        int k = ++g_vtmp;
        snprintf(buf, cap, "({ embi_dict_%s dv%d_ = %s; embi_gen_dict_to_value_%s(c, &dv%d_); })", d->name, k,
                 ret, d->name, k);
    } else {
        snprintf(buf, cap, "EMBI_JS_UNDEFINED");
    }
}

/* ------------------------------------------------------------------ member bodies */

typedef struct body_ctx {
    const char *file;
    const char *iface; /* the interface (or namespace) name */
    bool is_ns;
    bool is_static;
    bool bindobj;      /* the implementation takes the wrapper record */
    bool node_self;    /* the implementation takes an embi_node * */
    bool global;       /* [Global]: a missing this means the global object */
    bool direct;       /* the native is written under its own name, not shared (overloads, stringifiers) */
} body_ctx;

/* The self argument of an implementation (none for static members and namespaces). */
static const char *self_ctype(const body_ctx *b)
{
    if (b->bindobj) {
        return "embi_bind_obj *";
    }
    return b->node_self ? "embi_node *" : "void *";
}

/* The start of a native's body: finds the context, opens the scope, names the member (site is a C
 * expression for its row: the data of the running function in a shared native, a static row in
 * a direct one), brand-checks the receiver and declares self. */
static void emit_begin(sink *o, const body_ctx *b, const char *site)
{
    bool none = b->is_ns || b->is_static;
    sp(o, "    (void)this_v;\n    (void)argc;\n    (void)argv;\n    (void)magic;\n");
    sp(o, "    const embi_bind_member *m = %s;\n    embi_bind_scope sc;\n", site);
    if (!none) {
        sp(o, "    embi_bind_obj *bo = nullptr;\n");
    }
    const char *how = none ? "EMBI_SELF_NONE" : b->bindobj ? "EMBI_SELF_OBJ" : b->node_self ? "EMBI_SELF_NODE" : "EMBI_SELF_PTR";
    sp(o, "    embi_bind_ctx *c = embi_bind_begin(r, this_v, m, %s%s, &sc, %s);\n", how,
       (!none && b->global) ? " | EMBI_SELF_GLOBAL" : "", none ? "nullptr" : "&bo");
    sp(o, "    if (!c) return sc.early;\n");
    if (none) {
        return;
    }
    if (b->bindobj) {
        sp(o, "    embi_bind_obj *self = bo;\n    (void)self;\n");
    } else if (b->node_self) {
        sp(o, "    embi_node *self = (embi_node *)bo->ptr;\n    (void)self;\n");
    } else {
        sp(o, "    void *self = bo->ptr;\n    (void)self;\n");
    }
}

static void emit_native_head(sink *o, const char *fname, bool is_static)
{
    sp(o, "%sembi_js_value %s(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)\n{\n",
       is_static ? "static " : "", fname);
}

/* A static row describing a member for a direct native (one that is not shared): the name and the
 * interface its error messages and brand check use. */
static void emit_site_row(sink *o, const body_ctx *b, const char *rowname, const char *jsname)
{
    sp(o, "static const embi_bind_member %s = {.name = \"%s\", .iface = %s_%s%s};\n", rowname, jsname,
       b->is_ns ? "EMBI_NSID" : "EMBI_IF", b->iface, b->is_ns ? ", .flags = EMBI_MF_NS" : "");
}

/* Finishes a native whose body is in body. Direct natives are written to o under fname. Shared
 * ones are registered; their function name is returned in shape. */
static void finish_native(sink *o, const body_ctx *b, sink *body, const char *fname, const char *example,
                          char shape[32])
{
    if (b->direct) {
        emit_native_head(o, fname, true);
        sp(o, "%s}\n\n", body->buf ? body->buf : "");
        shape[0] = '\0';
        return;
    }
    /* src/bindings/module.cmake leaves gen/testing.c out of builds without tests, so a shape only testing.webidl
     * uses may call converters that do not exist there: idl_gen_shapes compiles it only in test builds. */
    bool testing = strcmp(b->file, "testing") == 0; /* file names are stems, see idlgen.c */
    shape_add(body->buf ? body->buf : "", example, !testing, shape);
}

/* The number of required arguments of a member: those before the first optional or variadic one. */
static int required_count(const idl_member *m)
{
    int required = 0;
    bool seen_opt = false;
    for (int i = 0; i < m->args.n; i++) {
        const idl_arg *a = &m->args.items[i];
        if (a->optional || a->variadic) {
            seen_opt = true;
        }
        if (!seen_opt) {
            required = i + 1;
        }
    }
    return required;
}

/* The result type and the parameter list of the implementation of a member (tag "get" or "set" for
 * attributes); with names the parameters are named as in the prototype, otherwise only their types
 * are written (the type of the function pointer a shared native calls through). */
static void impl_parts(const body_ctx *b, const idl_member *m, const char *tag, bool names, char *ret,
                       size_t retcap, char *params, size_t pcap)
{
    size_t len = 0;
    params[0] = '\0';
    if (m->kind == IDL_M_ATTR) {
        ret_ctype(m->type, ret, retcap);
        if (strcmp(tag, "set") == 0) {
            snprintf(ret, retcap, "%s", m->throws ? "embi_dom_err" : "void");
        } else if (m->throws) {
            snprintf(ret, retcap, "embi_dom_err");
        }
    } else if (m->kind == IDL_M_CTOR) {
        snprintf(ret, retcap, "embi_js_value");
    } else if (m->kind == IDL_M_STRINGIFIER) {
        snprintf(ret, retcap, "embi_dstr");
    } else if (m->getter) {
        /* An indexed getter returns the finished JS value (undefined when there is no item). */
        snprintf(ret, retcap, "embi_js_value");
    } else {
        ret_ctype(m->type, ret, retcap);
        if (m->throws) {
            snprintf(ret, retcap, "embi_dom_err");
        }
    }
    sbf(params, pcap, &len, "embi_bind_ctx *%s", names ? "c" : "");
    if (m->kind == IDL_M_CTOR) {
        sbf(params, pcap, &len, ", embi_js_value %s", names ? "new_target" : "");
    } else if (!b->is_ns && !b->is_static) {
        sbf(params, pcap, &len, ", %s%s", self_ctype(b), names ? " self" : "");
    }
    if (strcmp(tag, "set") == 0) {
        char t[160];
        ctype_of(m->type, t, sizeof t);
        sbf(params, pcap, &len, ", %s%s%s", t, names ? " " : "", names ? "a0" : "");
    } else {
        for (int i = 0; i < m->args.n; i++) {
            const idl_arg *a = &m->args.items[i];
            char t[160];
            if (a->variadic) {
                if (a->type->kind == IDL_T_ANY) {
                    if (names) {
                        sbf(params, pcap, &len, ", int a%dn, const embi_js_value *a%dp", i, i);
                    } else {
                        sbf(params, pcap, &len, ", int, const embi_js_value *");
                    }
                } else {
                    ctype_of(a->type, t, sizeof t);
                    if (names) {
                        sbf(params, pcap, &len, ", int a%dn, const %s *a%dp", i, t, i);
                    } else {
                        sbf(params, pcap, &len, ", int, const %s *", t);
                    }
                }
                continue;
            }
            ctype_of(a->type, t, sizeof t);
            if (names) {
                sbf(params, pcap, &len, ", %s a%d", t, i);
            } else {
                sbf(params, pcap, &len, ", %s", t);
            }
        }
    }
    if (m->throws && m->type && m->type->kind != IDL_T_VOID && strcmp(tag, "set") != 0 &&
        (m->kind == IDL_M_ATTR || m->kind == IDL_M_OP)) {
        char rt[160];
        ret_ctype(m->type, rt, sizeof rt);
        sbf(params, pcap, &len, ", %s *%s", rt, names ? "out" : "");
    }
}

static void emit_impl_proto(FILE *h, const body_ctx *b, const idl_member *m, const char *tag)
{
    char ret[160], impl[256], params[2048];
    impl_parts(b, m, tag, true, ret, sizeof ret, params, sizeof params);
    impl_name(impl, sizeof impl, b->iface, m, tag);
    fprintf(h, "%s %s(%s);\n", (strcmp(tag, "set") == 0) ? ((m->throws) ? "embi_dom_err" : "void") : ret, impl,
            params);
}

/* The expression a native calls an implementation through: its name in a direct native, the row's
 * function pointer (which = "impl" or "impl2") cast to the implementation's type in a shared one. */
static void callee_expr(char *buf, size_t cap, const body_ctx *b, const idl_member *m, const char *tag,
                        const char *which)
{
    if (b->direct) {
        impl_name(buf, cap, b->iface, m, tag);
        return;
    }
    char ret[160], params[2048];
    impl_parts(b, m, tag, false, ret, sizeof ret, params, sizeof params);
    snprintf(buf, cap, "((%s (*)(%s))m->%s)", (strcmp(tag, "set") == 0) ? ((m->throws) ? "embi_dom_err" : "void") : ret,
             params, which);
}

static bool is_variadic(const idl_member *m)
{
    return m->args.n > 0 && m->args.items[m->args.n - 1].variadic;
}

/* The C type of one element of a variadic argument. */
static void variadic_elem_ctype(const idl_type *t, char *buf, size_t cap)
{
    ctype_of(t, buf, cap);
}

/* Writes the argument conversions for an overload. The arguments are named a<i>. fail is the
 * statement run on a conversion error. ctx_macro is the macro that makes the error context of a parameter. */
static void emit_args(gen *g, sink *o, const body_ctx *b, const idl_member *m, const char *ctx_macro,
                      const char *fail)
{
    for (int i = 0; i < m->args.n; i++) {
        const idl_arg *a = &m->args.items[i];
        char cty[160], ctx[512], def[256];
        /* the message is built from the member row when a conversion fails (embi_bind_ctx_text) */
        snprintf(ctx, sizeof ctx, "%s(%d)", ctx_macro, i + 1);
        if (a->variadic) {
            sp(o, "    int a%dn = argc > %d ? argc - %d : 0;\n", i, i, i);
            if (a->type->kind == IDL_T_ANY) {
                sp(o, "    const embi_js_value *a%dp = argc > %d ? argv + %d : nullptr;\n", i, i, i);
            } else {
                char ect[160];
                variadic_elem_ctype(a->type, ect, sizeof ect);
                sp(o, "    %s *a%dp = (%s *)embi_bind_alloc(c, (size_t)a%dn * sizeof(%s));\n", ect, i, ect, i, ect);
                sp(o, "    for (int q = 0; q < a%dn; q++) {\n", i);
                sp(o, "        a%dp[q] = (%s){0};\n", i, ect);
                char dst[64], src[64];
                snprintf(dst, sizeof dst, "a%dp[q]", i);
                snprintf(src, sizeof src, "argv[%d + q]", i);
                emit_conv(g, o, a->type, 0, dst, src, ctx, fail, b->file, a->line);
                sp(o, "    }\n");
            }
            continue;
        }
        const char *why = unsupported_arg(a->type);
        if (why) {
            gen_error(g, b->file, a->line, "%s", why);
            continue;
        }
        ctype_of(a->type, cty, sizeof cty);
        unsigned flags = 0;
        if (idl_has_ext(&a->ext, "Clamp")) {
            flags |= CONV_CLAMP;
        }
        if (idl_has_ext(&a->ext, "EnforceRange")) {
            flags |= CONV_ENFORCE;
        }
        if (idl_has_ext(&a->ext, "LegacyNullToEmptyString")) {
            flags |= CONV_LEGACY_NTE;
        }
        sp(o, "    %s a%d = {0};\n", cty, i);
        char dst[64], src[96];
        snprintf(dst, sizeof dst, "a%d", i);
        if (a->optional && a->type->kind != IDL_T_DICT) {
            /* An absent optional argument takes its default (or the zero value), not a conversion. */
            default_expr(a->type, a->def, def, sizeof def);
            if (a->def != nullptr) {
                /* WebIDL: undefined stands for an absent value when the argument has a default. */
                sp(o, "    if (argc <= %d || embi_js_is_undefined(argv[%d])) {\n        a%d = %s;\n    } else {\n", i, i, i, def);
            } else {
                sp(o, "    if (argc <= %d) {\n        a%d = %s;\n    } else {\n", i, i, def);
            }
            snprintf(src, sizeof src, "argv[%d]", i);
            emit_conv(g, o, a->type, flags, dst, src, ctx, fail, b->file, a->line);
            sp(o, "    }\n");
        } else if (a->optional) {
            /* A dictionary: an absent value converts as undefined, which gives the defaults. */
            snprintf(src, sizeof src, "(argc > %d ? argv[%d] : EMBI_JS_UNDEFINED)", i, i);
            emit_conv(g, o, a->type, flags, dst, src, ctx, fail, b->file, a->line);
        } else {
            snprintf(src, sizeof src, "argv[%d]", i);
            emit_conv(g, o, a->type, flags, dst, src, ctx, fail, b->file, a->line);
        }
    }
}

/* The call of the implementation: c, self (when the member has one), then the arguments. */
static void call_list(char *buf, size_t cap, const idl_member *m, bool with_self, bool with_out)
{
    size_t len = 0;
    buf[0] = '\0';
    sbf(buf, cap, &len, "c");
    if (with_self) {
        sbf(buf, cap, &len, ", self");
    }
    for (int i = 0; i < m->args.n; i++) {
        const idl_arg *a = &m->args.items[i];
        if (a->variadic) {
            sbf(buf, cap, &len, ", a%dn, a%dp", i, i);
        } else {
            sbf(buf, cap, &len, ", a%d", i);
        }
    }
    if (with_out) {
        sbf(buf, cap, &len, ", &out");
    }
}

/* Writes the call and the result of a member whose native function has already converted its
 * arguments. impl is the implementation name, ret the C type of its result. */
static void emit_call_and_return(sink *o, const idl_member *m, const idl_type *t, const char *impl,
                                 const char *ret, bool with_self, bool getter_value)
{
    char args[1024];
    bool promise = t->kind == IDL_T_PROMISE;
    if (m->throws) {
        call_list(args, sizeof args, m, with_self, t->kind != IDL_T_VOID);
        if (t->kind != IDL_T_VOID) {
            sp(o, "    %s out = {0};\n", ret);
        }
        sp(o, "    embi_dom_err err = %s(%s);\n", impl, args);
        sp(o, "    if (err != EMBI_DOM_OK) return embi_bind_end(&sc, embi_bind_throw_dom(c, err));\n");
        if (t->kind == IDL_T_VOID) {
            sp(o, "    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        } else {
            char v[768];
            value_expr(t, "out", v, sizeof v);
            sp(o, "    return embi_bind_end(&sc, %s);\n", v);
        }
        return;
    }
    call_list(args, sizeof args, m, with_self, false);
    if (t->kind == IDL_T_VOID) {
        sp(o, "    %s(%s);\n", impl, args);
        sp(o, "    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        return;
    }
    if (getter_value) {
        /* An item() style operation: the implementation returns undefined for no item. */
        sp(o, "    embi_js_value gv = %s(%s);\n", impl, args);
        sp(o, "    if (embi_js_is_undefined(gv)) gv = %s;\n", t->nullable ? "EMBI_JS_NULL" : "EMBI_JS_UNDEFINED");
        sp(o, "    return embi_bind_end(&sc, gv);\n");
        return;
    }
    sp(o, "    %s res = %s(%s);\n", ret, impl, args);
    if (promise) {
        sp(o, "    if (res == EMBI_JS_EXCEPTION) res = embi_bind_promise_from_pending(c);\n");
    }
    char v[768];
    value_expr(t, "res", v, sizeof v);
    sp(o, "    return embi_bind_end(&sc, %s);\n", v);
}

/* The native function of an operation or a static or namespace member. A shared native is only
 * registered (its function name is returned in shape); a direct one (b->direct) is written to o
 * under fname, after the static row that names it in messages. */
static void emit_op_native(gen *g, sink *o, const body_ctx *b, const idl_member *m, const char *fname,
                           char shape[32])
{
    char ret[160];
    shape[0] = '\0';
    ret_ctype(m->type, ret, sizeof ret);
    if (m->getter && m->args.n != 1) {
        gen_error(g, b->file, m->line, "an indexed getter takes exactly one argument");
        return;
    }
    bool promise = m->type->kind == IDL_T_PROMISE;
    g->tmp = 0;
    g_vtmp = 0;
    char site[300], example[300];
    snprintf(example, sizeof example, "%s.%s", b->iface, mname(m));
    snprintf(site, sizeof site, "&%s_site", fname);
    if (b->direct) {
        char rowname[300];
        snprintf(rowname, sizeof rowname, "%s_site", fname);
        emit_site_row(o, b, rowname, mname(m));
    }
    sink body = {0};
    emit_begin(&body, b, b->direct ? site : "r->cur_data");
    int required = required_count(m);
    const char *fail = promise ? "return embi_bind_end(&sc, embi_bind_promise_from_pending(c));"
                               : "return embi_bind_end(&sc, EMBI_JS_EXCEPTION);";
    if (required > 0) {
        if (promise) {
            sp(&body, "    if (argc < %d) { embi_bind_throw_args(c, EMBI_CTX_ARG(0), %d, argc); %s }\n", required,
               required, fail);
        } else {
            sp(&body, "    if (argc < %d) return embi_bind_end(&sc, embi_bind_throw_args(c, EMBI_CTX_ARG(0), %d, argc));\n",
               required, required);
        }
    }
    emit_args(g, &body, b, m, "EMBI_CTX_ARG", fail);
    char callee[2400];
    callee_expr(callee, sizeof callee, b, m, "", "impl");
    bool with_self = !(b->is_ns || b->is_static);
    emit_call_and_return(&body, m, m->type, callee, ret, with_self, m->getter);
    finish_native(o, b, &body, fname, example, shape);
    free(body.buf);
}

/* A dispatcher for an overloaded operation name. */
static void emit_dispatcher(gen *g, sink *o, const idl_iface *ifc, const idl_member *first)
{
    (void)g;
    char dname[256], fn[256];
    dispatch_name(dname, sizeof dname, ifc->name, first->name);
    sp(o, "static const embi_bind_overload %s_ov[] = {\n", dname);
    int n = 0;
    for (int k = 0; k < first->noverloads; k++) {
        const idl_member *m = nullptr;
        for (int i = 0; i < ifc->members.n; i++) {
            const idl_member *x = &ifc->members.items[i];
            if (x->kind == IDL_M_OP && !x->getter && x->name && strcmp(x->name, first->name) == 0 &&
                x->is_static == first->is_static && x->overload == k) {
                m = x;
                break;
            }
        }
        if (!m) {
            continue;
        }
        native_name(fn, sizeof fn, ifc->name, m, "");
        int ncat = m->args.n < 6 ? m->args.n : 6;
        sp(o, "    {%s, %d, %d, %d, {", fn, required_count(m), is_variadic(m) ? 255 : m->args.n, ncat);
        for (int q = 0; q < ncat; q++) {
            char u[200];
            um_init(m->args.items[q].type, u, sizeof u);
            sp(o, "%s%s", q ? ", " : "", u);
        }
        if (ncat == 0) {
            sp(o, "{EMBI_UM_ANY, 0, false, 0}");
        }
        sp(o, "}},\n");
        n++;
    }
    sp(o, "};\n\n");
    emit_native_head(o, dname, true);
    sp(o, "    (void)magic;\n");
    sp(o, "    return embi_bind_dispatch(r, this_v, argc, argv, %s_ov, %d, \"%s\");\n}\n\n", dname, n,
            first->name);
}

static void lower_name(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    for (size_t i = 0; i <= n && i < cap - 1; i++) {
        out[i] = (char)tolower((unsigned char)in[i]);
    }
    out[cap - 1] = '\0';
}

/* What the installer needs of a member: a row of the member table of its interface, or (for
 * natives that are not shared) a statement that defines it. */
typedef struct mrow {
    char *row;
    char *stmt;
} mrow;

static char *dupf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *dupf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) {
        exit(2);
    }
    return strdup(buf);
}

/* The member-table fields that name the interface (or namespace) of the member and flag it. */
static void row_iface(char *buf, size_t cap, const body_ctx *b, bool unforgeable)
{
    const char *f1 = b->is_ns ? "EMBI_MF_NS" : "";
    const char *f2 = unforgeable ? "EMBI_MF_UNFORGEABLE" : "";
    snprintf(buf, cap, ".iface = %s_%s%s%s%s%s", b->is_ns ? "EMBI_NSID" : "EMBI_IF", b->iface,
             (f1[0] || f2[0]) ? ", .flags = " : "", f1, (f1[0] && f2[0]) ? " | " : "", f2);
}

/* The row of a reflected attribute: the content attribute is read and written by one of the
 * natives of bind_conv.c, so the attribute costs no code at all. */
static void emit_reflect_row(gen *g, const body_ctx *b, const idl_member *m, mrow *out)
{
    char lower[128], lit[256], iface[160];
    lower_name(m->reflect_name ? m->reflect_name : m->name, lower, sizeof lower);
    cstr(lit, sizeof lit, lower);
    row_iface(iface, sizeof iface, b, m->unforgeable);
    const idl_type *t = m->type;
    const idl_ext *dx = idl_ext_get(&m->ext, "ReflectDefault");
    const char *def = dx && dx->value ? dx->value : "0";
    const char *kind = "str";
    char extra[160] = "";
    if (idl_has_ext(&m->ext, "Clamp") || idl_has_ext(&m->ext, "EnforceRange") ||
        idl_has_ext(&m->ext, "LegacyNullToEmptyString")) {
        gen_error(g, b->file, m->line, "[Reflect] does not combine with [Clamp], [EnforceRange] or [LegacyNullToEmptyString]");
        return;
    }
    if (m->put_forwards || m->replaceable) {
        /* The shared reflect setter would write the content attribute, not forward or replace. */
        gen_error(g, b->file, m->line, "[Reflect] does not combine with [PutForwards] or [Replaceable]");
        return;
    }
    if (t->kind == IDL_T_BOOLEAN) {
        kind = "bool";
    } else if (t->kind == IDL_T_FLOAT || t->kind == IDL_T_DOUBLE) {
        kind = "double";
        snprintf(extra, sizeof extra, ", .num = %s, .len = %d", def, t->unrestricted ? 1 : 0);
    } else if (is_int(t) && is_signed_int(t->kind)) {
        kind = "long";
        snprintf(extra, sizeof extra, ", .num = %s, .len = %d", def, int_kind(t->kind));
    } else if (is_int(t)) {
        kind = "ulong";
        snprintf(extra, sizeof extra, ", .len = %d", int_kind(t->kind));
    } else if (t->nullable) {
        kind = "nstr";
    }
    bool setter = !m->readonly;
    char setf[100] = "";
    if (setter) {
        snprintf(setf, sizeof setf, ", .set = embi_bind_refl_set_%s", kind);
    }
    out->row = dupf("{.name = \"%s\", .attr = %s, .fn = embi_bind_refl_get_%s%s%s, %s, .kind = EMBI_MEMBER_ATTR}",
                    m->name, lit, kind, setf, extra, iface);
}

/* The natives of an attribute: a getter and, when the attribute is writable, a setter. Reflected
 * attributes need none. out gets the member-table row. */
static void emit_attr_natives(gen *g, sink *o, const body_ctx *b, const idl_member *m, mrow *out)
{
    const char *why = unsupported_arg(m->type);
    if (why && !m->readonly) {
        gen_error(g, b->file, m->line, "%s", why);
        return;
    }
    if (m->reflect) {
        bool ok = (m->type->kind == IDL_T_BOOLEAN) || is_num(m->type) || is_str(m->type);
        if (!ok) {
            gen_error(g, b->file, m->line, "[Reflect] supports boolean, number and DOMString attributes");
            return;
        }
        emit_reflect_row(g, b, m, out);
        return;
    }
    char ret[160], ctyp[160];
    ret_ctype(m->type, ret, sizeof ret);
    ctype_of(m->type, ctyp, sizeof ctyp);
    char getn[256], setn[256];
    native_name(getn, sizeof getn, b->iface, m, "get");
    native_name(setn, sizeof setn, b->iface, m, "set");
    bool has_set = !(m->readonly && !m->put_forwards && !m->replaceable);
    char example[300];
    snprintf(example, sizeof example, "%s.%s", b->iface, m->name);
    char getshape[32] = "", setshape[32] = "";
    bool with_self = !(b->is_ns || b->is_static);
    char callee[2400];
    sink body = {0};
    /* getter */
    g->tmp = 0;
    g_vtmp = 0;
    emit_begin(&body, b, "r->cur_data");
    if (m->sameobject) {
        sp(&body, "    embi_js_value *cache = embi_bind_same(bo, %d);\n", m->slot);
        sp(&body, "    if (!embi_js_is_empty(*cache)) return embi_bind_end(&sc, *cache);\n");
        char args[256];
        call_list(args, sizeof args, m, with_self, m->throws);
        char v[768];
        callee_expr(callee, sizeof callee, b, m, "get", "impl");
        if (m->throws) {
            sp(&body, "    %s out = {0};\n", ret);
            sp(&body, "    embi_dom_err err = %s(%s);\n", callee, args);
            sp(&body, "    if (err != EMBI_DOM_OK) return embi_bind_end(&sc, embi_bind_throw_dom(c, err));\n");
            value_expr(m->type, "out", v, sizeof v);
        } else {
            sp(&body, "    %s res = %s(%s);\n", ret, callee, args);
            value_expr(m->type, "res", v, sizeof v);
        }
        sp(&body, "    embi_js_value v = %s;\n", v);
        sp(&body, "    cache = embi_bind_same(bo, %d);\n", m->slot);
        sp(&body, "    *cache = v;\n    EMBI_JS_WB_STRUCT(embi_js_as_cell(this_v));\n");
        sp(&body, "    return embi_bind_end(&sc, v);\n");
    } else {
        callee_expr(callee, sizeof callee, b, m, "get", "impl");
        emit_call_and_return(&body, m, m->type, callee, ret, with_self, false);
    }
    finish_native(o, b, &body, getn, example, getshape);
    sink_reset(&body);
    if (has_set) {
        /* setter */
        g->tmp = 0;
        g_vtmp = 0;
        emit_begin(&body, b, "r->cur_data");
        sp(&body, "    embi_js_value v0 = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;\n");
        if (m->put_forwards) {
            sp(&body, "    if (!embi_bind_put_forwards(c, this_v, m->name, \"%s\", v0)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);\n",
               m->put_forwards);
            sp(&body, "    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        } else if (m->replaceable) {
            sp(&body, "    if (!embi_bind_replace_prop(c, this_v, m->name, v0)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);\n");
            sp(&body, "    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        } else {
            unsigned flags = 0;
            if (idl_has_ext(&m->ext, "Clamp")) {
                flags |= CONV_CLAMP;
            }
            if (idl_has_ext(&m->ext, "EnforceRange")) {
                flags |= CONV_ENFORCE;
            }
            if (idl_has_ext(&m->ext, "LegacyNullToEmptyString")) {
                flags |= CONV_LEGACY_NTE;
            }
            sp(&body, "    %s a0 = {0};\n", ctyp);
            emit_conv(g, &body, m->type, flags, "a0", "v0", "EMBI_CTX_SET", "return embi_bind_end(&sc, EMBI_JS_EXCEPTION);",
                      b->file, m->line);
            char args[256];
            size_t len = 0;
            args[0] = '\0';
            sbf(args, sizeof args, &len, "c");
            if (with_self) {
                sbf(args, sizeof args, &len, ", self");
            }
            sbf(args, sizeof args, &len, ", a0");
            callee_expr(callee, sizeof callee, b, m, "set", "impl2");
            if (m->throws) {
                sp(&body, "    embi_dom_err err = %s(%s);\n", callee, args);
                sp(&body, "    if (err != EMBI_DOM_OK) return embi_bind_end(&sc, embi_bind_throw_dom(c, err));\n");
            } else {
                sp(&body, "    %s(%s);\n", callee, args);
            }
            sp(&body, "    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        }
        finish_native(o, b, &body, setn, example, setshape);
    }
    free(body.buf);
    char impl[256], impl2[256], iface[160];
    impl_name(impl, sizeof impl, b->iface, m, "get");
    impl_name(impl2, sizeof impl2, b->iface, m, "set");
    row_iface(iface, sizeof iface, b, m->unforgeable);
    char setpart[700] = "";
    if (has_set) {
        bool via_impl = !m->put_forwards && !m->replaceable;
        snprintf(setpart, sizeof setpart, "%s%s%s, .set = %s", via_impl ? ", .impl2 = (embi_gen_fn)" : "",
                 via_impl ? impl2 : "", "", setshape);
    }
    out->row = dupf("{.name = \"%s\", .impl = (embi_gen_fn)%s%s, .fn = %s, %s, .kind = EMBI_MEMBER_ATTR}", m->name, impl,
                    setpart, getshape, iface);
}

/* The indexed getter of an interface, when it has one (an unnamed or named getter taking an index). */
static const idl_member *indexed_getter(const idl_iface *i)
{
    for (int k = 0; k < i->members.n; k++) {
        if (i->members.items[k].getter) {
            return &i->members.items[k];
        }
    }
    return nullptr;
}

static const idl_member *find_member(const idl_iface *i, int kind)
{
    for (int k = 0; k < i->members.n; k++) {
        if (i->members.items[k].kind == kind) {
            return &i->members.items[k];
        }
    }
    return nullptr;
}

static bool has_length_attr(const idl_iface *i)
{
    for (int k = 0; k < i->members.n; k++) {
        const idl_member *m = &i->members.items[k];
        if (m->kind == IDL_M_ATTR && !m->is_static && strcmp(m->name, "length") == 0) {
            return true;
        }
    }
    return false;
}

static const char *self_expr_of(const idl_iface *i)
{
    if (idl_has_ext(&i->ext, "BindObj")) {
        return "bo";
    }
    return i->node_derived ? "(embi_node *)bo->ptr" : "bo->ptr";
}

/* The get_own and own_keys hooks of an interface with an indexed getter. Named keys go to the
 * ordinary lookup. */
static void emit_index_hooks(gen *g, sink *o, const idl_iface *i, const idl_member *getter)
{
    if (getter->args.n != 1 || getter->args.items[0].type->kind != IDL_T_ULONG) {
        gen_error(g, i->file, getter->line, "an indexed getter takes one unsigned long argument");
        return;
    }
    char impl[256];
    impl_name(impl, sizeof impl, i->name, getter, "");
    bool stale = idl_has_ext(&i->ext, "BindObj");
    /* [IndexedSetter]: the interface has an indexed property setter (WebIDL 3.9): its indexes are writable and
     * [[DefineOwnProperty]] of an index (so [[Set]]) goes to embi_impl_<I>_indexed_set. */
    bool isetter = idl_has_ext(&i->ext, "IndexedSetter");
    sp(o, "static int gen_%s_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)\n{\n",
            i->name);
    sp(o, "    if (embi_js_key_is_index(k)) {\n");
    sp(o, "        embi_bind_obj *bo = (embi_bind_obj *)o->internal;\n");
    sp(o, "        embi_bind_ctx *c = bo->ctx;\n");
    sp(o, "        if (!c || %s) return EMBI_JS_NO;\n", stale ? "embi_bind_stale(bo)" : "bo->ptr == nullptr");
    sp(o, "        embi_bind_scope sc = embi_bind_enter(c);\n");
    sp(o, "        embi_js_value v = %s(c, %s, embi_js_key_index(k));\n", impl, self_expr_of(i));
    sp(o, "        embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
    sp(o, "        if (embi_js_is_undefined(v)) return EMBI_JS_NO;\n");
    sp(o, "        out->value = v;\n        out->get = EMBI_JS_UNDEFINED;\n        out->set = EMBI_JS_UNDEFINED;\n");
    sp(o, "        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;\n");
    sp(o, "        out->attrs = %s;\n        return EMBI_JS_YES;\n    }\n",
            isetter ? "EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE"
                    : "EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE");
    if (idl_has_ext(&i->ext, "NamedLookup")) {
        /* Named properties (a name that no own property or prototype member shadows): embi_impl_<I>_named. */
        sp(o, "    int own = embi_bind_own_named(rt, o, k, out);\n    if (own != EMBI_JS_NO) return own;\n");
        sp(o, "    if (embi_js_is_string(k)) {\n");
        sp(o, "        embi_bind_obj *bo = (embi_bind_obj *)o->internal;\n");
        sp(o, "        embi_bind_ctx *c = bo->ctx;\n");
        sp(o, "        if (!c || %s || embi_bind_proto_has(rt, o, k)) return EMBI_JS_NO;\n", stale ? "embi_bind_stale(bo)" : "bo->ptr == nullptr");
        sp(o, "        embi_bind_scope sc = embi_bind_enter(c);\n");
        sp(o, "        embi_js_value v = embi_impl_%s_named(c, %s, embi_bind_key_utf8(c, k));\n", i->name, self_expr_of(i));
        sp(o, "        embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n");
        sp(o, "        if (embi_js_is_undefined(v)) return EMBI_JS_NO;\n");
        sp(o, "        out->value = v;\n        out->get = EMBI_JS_UNDEFINED;\n        out->set = EMBI_JS_UNDEFINED;\n");
        sp(o, "        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;\n");
        sp(o, "        out->attrs = %s;\n        return EMBI_JS_YES;\n    }\n",
                idl_has_ext(&i->ext, "LegacyUnenumerableNamedProperties") ? "EMBI_JS_ATTR_CONFIGURABLE"
                                                                           : "EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE");
        sp(o, "    return EMBI_JS_NO;\n}\n\n");
    } else {
        sp(o, "    return embi_bind_own_named(rt, o, k, out);\n}\n\n");
    }
    /* [[DefineOwnProperty]] and [[Delete]] of a legacy platform object without setters (WebIDL 3.9): indexes and named
     * properties can be neither defined nor deleted. */
    sp(o, "static int gen_%s_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, const embi_js_pdesc *d)\n{\n", i->name);
    if (isetter) {
        sp(o, "    if (embi_js_key_is_index(k)) return embi_impl_%s_indexed_set(rt, o, embi_js_key_index(k), d);\n", i->name);
    } else {
        sp(o, "    if (embi_js_key_is_index(k)) return EMBI_JS_NO;\n");
    }
    if (idl_has_ext(&i->ext, "NamedLookup")) {
        sp(o, "    if (embi_js_is_string(k)) {\n        embi_js_pdesc cur;\n        if (embi_bind_own_named(rt, o, k, &cur) == EMBI_JS_NO && gen_%s_get_own(rt, o, k, &cur) == EMBI_JS_YES) return EMBI_JS_NO;\n    }\n", i->name);
    }
    sp(o, "    const embi_js_class *saved = o->klass;\n    o->klass = &embi_js_class_object;\n");
    sp(o, "    int r = embi_js_obj_define_own_property(rt, o, k, d);\n    o->klass = saved;\n    return r;\n}\n\n");
    sp(o, "static int gen_%s_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)\n{\n", i->name);
    sp(o, "    if (embi_js_key_is_index(k)) {\n        embi_js_pdesc cur;\n        return gen_%s_get_own(rt, o, k, &cur) == EMBI_JS_YES ? EMBI_JS_NO : EMBI_JS_YES;\n    }\n", i->name);
    if (idl_has_ext(&i->ext, "NamedLookup")) {
        sp(o, "    if (embi_js_is_string(k)) {\n        embi_js_pdesc cur;\n        if (embi_bind_own_named(rt, o, k, &cur) == EMBI_JS_NO && gen_%s_get_own(rt, o, k, &cur) == EMBI_JS_YES) return EMBI_JS_NO;\n    }\n", i->name);
    }
    sp(o, "    const embi_js_class *saved = o->klass;\n    o->klass = &embi_js_class_object;\n");
    sp(o, "    int r = embi_js_obj_delete(rt, o, k);\n    o->klass = saved;\n    return r;\n}\n\n");
    if (has_length_attr(i)) {
        sp(o, "static int gen_%s_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)\n{\n", i->name);
        sp(o, "    embi_bind_obj *bo = (embi_bind_obj *)o->internal;\n");
        sp(o, "    if (bo && bo->ctx && !(%s)) {\n", stale ? "embi_bind_stale(bo)" : "bo->ptr == nullptr");
        sp(o, "        embi_bind_ctx *c = bo->ctx;\n");
        sp(o, "        embi_bind_scope sc = embi_bind_enter(c);\n");
        sp(o, "        uint32_t n = embi_impl_%s_get_length(c, %s);\n", i->name, self_expr_of(i));
        sp(o, "        for (uint32_t q = 0; q < n; q++) embi_vec_push(out, embi_js_key_from_index(q));\n");
        if (idl_has_ext(&i->ext, "NamedLookup")) {
            /* the supported property names that no prototype member shadows, after the indexes */
            sp(o, "        embi_impl_%s_named_keys(rt, o, c, %s, out);\n", i->name, self_expr_of(i));
        }
        sp(o, "        embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n    }\n");
        sp(o, "    return embi_bind_own_keys_ordinary(rt, o, out);\n}\n\n");
    }
}

/* ------------------------------------------------------------------ per interface and namespace */

static int cmp_iface_ptr(const void *a, const void *b)
{
    const idl_iface *x = *(idl_iface *const *)a;
    const idl_iface *y = *(idl_iface *const *)b;
    return strcmp(x->name, y->name);
}

static int cmp_dict_ptr(const void *a, const void *b)
{
    const idl_dict *x = *(idl_dict *const *)a;
    const idl_dict *y = *(idl_dict *const *)b;
    return strcmp(x->name, y->name);
}

static int cmp_member_name(const void *a, const void *b)
{
    const idl_dmember *x = (const idl_dmember *)a;
    const idl_dmember *y = (const idl_dmember *)b;
    return strcmp(x->name, y->name);
}

/* The main records of a file (or of all files when stem is nullptr), sorted by name. */
static int file_ifaces(const idl_db *db, const char *stem, idl_iface **out, int cap)
{
    int n = 0;
    for (int i = 0; i < db->ifaces.n && n < cap; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (!r->has_main || r->mixin) {
            continue;
        }
        if (stem && strcmp(r->file, stem) != 0) {
            continue;
        }
        out[n++] = r;
    }
    qsort(out, (size_t)n, sizeof *out, cmp_iface_ptr);
    return n;
}

static int file_dicts(const idl_db *db, const char *stem, idl_dict **out, int cap)
{
    int n = 0;
    for (int i = 0; i < db->dicts.n && n < cap; i++) {
        idl_dict *d = db->dicts.items[i];
        if (d->is_partial) {
            continue;
        }
        if (stem && strcmp(d->file, stem) != 0) {
            continue;
        }
        out[n++] = d;
    }
    qsort(out, (size_t)n, sizeof *out, cmp_dict_ptr);
    return n;
}

static int file_enums(const idl_db *db, const char *stem, idl_enum **out, int cap)
{
    int n = 0;
    for (int i = 0; i < db->enums.n && n < cap; i++) {
        idl_enum *e = db->enums.items[i];
        if (stem && strcmp(e->file, stem) != 0) {
            continue;
        }
        out[n++] = e;
    }
    return n;
}

/* Marks the dictionaries that appear as return or attribute types (they get a to-value converter). */
static void mark_result_dicts(idl_db *db)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        const idl_iface *r = db->ifaces.items[i];
        if (!r->has_main || r->mixin) {
            continue;
        }
        for (int k = 0; k < r->members.n; k++) {
            const idl_member *m = &r->members.items[k];
            if ((m->kind == IDL_M_ATTR || m->kind == IDL_M_OP) && m->type) {
                const idl_type *t = m->type;
                while ((t->kind == IDL_T_SEQUENCE || t->kind == IDL_T_PROMISE) && t->nsub) {
                    t = t->sub[0];
                }
                if (t->kind == IDL_T_DICT) {
                    ((idl_dict *)t->target)->result = true;
                }
            }
        }
    }
}

static void emit_stringifier(gen *g, sink *o, const body_ctx *b0, const idl_iface *ifc, const idl_member *m)
{
    (void)g;
    body_ctx b = *b0;
    b.direct = true;
    char fname[200], site[220], shape[32], row[240];
    snprintf(fname, sizeof fname, "gen_%s_toString", ifc->name);
    snprintf(site, sizeof site, "&%s_site", fname);
    snprintf(row, sizeof row, "%s_site", fname);
    emit_site_row(o, &b, row, "toString");
    sink body = {0};
    emit_begin(&body, &b, site);
    if (m->kind == IDL_M_STRINGIFIER) {
        sp(&body, "    return embi_bind_end(&sc, embi_bind_dstr_value(c, embi_impl_%s_stringifier(c, self)));\n", ifc->name);
    } else if (m->reflect) {
        char lower[128], lit[200];
        lower_name(m->reflect_name ? m->reflect_name : m->name, lower, sizeof lower);
        cstr(lit, sizeof lit, lower);
        sp(&body, "    return embi_bind_end(&sc, embi_bind_reflect_get_str(c, (embi_element *)self, EMBI_STR(%s)));\n", lit);
    } else {
        sp(&body, "    return embi_bind_end(&sc, embi_bind_dstr_value(c, embi_impl_%s_get_%s(c, self)));\n", ifc->name,
           m->name);
    }
    finish_native(o, &b, &body, fname, "toString", shape);
    free(body.buf);
}

/* The natives of an interface's members. rows[k] gets what the installer needs of member k. */
static void emit_natives_for(gen *g, sink *o, FILE *h, const body_ctx *base, const idl_iface *ifc, mrow *rows)
{
    for (int k = 0; k < ifc->members.n; k++) {
        const idl_member *m = &ifc->members.items[k];
        body_ctx b = *base;
        b.is_static = m->is_static;
        switch (m->kind) {
        case IDL_M_ATTR:
            if (m->is_static) {
                gen_error(g, base->file, m->line, "static attributes are not supported");
                break;
            }
            if (!m->reflect) {
                emit_impl_proto(h, &b, m, "get");
                if (!m->readonly && !m->put_forwards && !m->replaceable) {
                    emit_impl_proto(h, &b, m, "set");
                }
            }
            emit_attr_natives(g, o, &b, m, &rows[k]);
            if (m->stringifier) {
                emit_stringifier(g, o, &b, ifc, m);
            }
            break;
        case IDL_M_OP: {
            emit_impl_proto(h, &b, m, "");
            if (!m->name) {
                break;
            }
            char fn[256];
            native_name(fn, sizeof fn, base->iface, m, "");
            if (m->noverloads > 1) {
                /* an overload is called by its dispatcher, which has no row to pass: direct natives */
                char shape[32];
                b.direct = true;
                emit_op_native(g, o, &b, m, fn, shape);
                if (m->overload == m->noverloads - 1) {
                    /* The dispatcher comes after every overload it names. */
                    const idl_member *first = nullptr;
                    for (int q = 0; q < ifc->members.n; q++) {
                        const idl_member *x = &ifc->members.items[q];
                        if (x->kind == IDL_M_OP && x->name && !x->getter && x->overload == 0 &&
                            strcmp(x->name, m->name) == 0 && x->is_static == m->is_static) {
                            first = x;
                            break;
                        }
                    }
                    if (first) {
                        emit_dispatcher(g, o, ifc, first);
                    }
                }
                break;
            }
            char shape[32];
            emit_op_native(g, o, &b, m, fn, shape);
            char impl[256], iface[160];
            impl_name(impl, sizeof impl, b.iface, m, "");
            row_iface(iface, sizeof iface, &b, m->unforgeable);
            rows[k].row = dupf("{.name = \"%s\", .impl = (embi_gen_fn)%s, .fn = %s, %s, .kind = EMBI_MEMBER_%s, .len = %d}",
                               m->name, impl, shape, iface, m->is_static && !b.is_ns ? "SOP" : "OP",
                               required_count(m));
            break;
        }
        case IDL_M_CTOR:
            emit_impl_proto(h, &b, m, "");
            break;
        case IDL_M_STRINGIFIER:
            emit_impl_proto(h, &b, m, "");
            emit_stringifier(g, o, &b, ifc, m);
            break;
        case IDL_M_ITERABLE: {
            if (indexed_getter(ifc)) {
                break;
            }
            fprintf(h, "uint32_t embi_impl_%s_iter_size(embi_bind_ctx *c, embi_bind_obj *self);\n", ifc->name);
            if (m->key) {
                fprintf(h, "embi_js_value embi_impl_%s_iter_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i);\n",
                        ifc->name);
            }
            fprintf(h, "embi_js_value embi_impl_%s_iter_value(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i);\n",
                    ifc->name);
            char keyname[200];
            snprintf(keyname, sizeof keyname, "embi_impl_%s_iter_key", ifc->name);
            sp(o, "static const embi_bind_iter_ops gen_%s_iter_ops = {EMBI_IF_%s, %s, embi_impl_%s_iter_size, %s, embi_impl_%s_iter_value};\n\n",
                    ifc->name, ifc->name, m->key ? "true" : "false", ifc->name,
                    m->key ? keyname : "nullptr", ifc->name);
            break;
        }
        default:
            break;
        }
    }
    if (idl_has_ext(&ifc->ext, "CSSProperties")) {
        fprintf(h, "embi_dstr embi_impl_%s_cssprop_get(embi_bind_ctx *c, embi_bind_obj *self, int idx);\n", ifc->name);
        fprintf(h, "void embi_impl_%s_cssprop_set(embi_bind_ctx *c, embi_bind_obj *self, int idx, embi_dstr v);\n", ifc->name);
        body_ctx b = *base;
        b.direct = true;
        char shape[32];
        char fname[200], site[220], row[240];
        snprintf(fname, sizeof fname, "gen_%s_cssprop_get", ifc->name);
        snprintf(site, sizeof site, "&%s_site", fname);
        snprintf(row, sizeof row, "%s_site", fname);
        emit_site_row(o, &b, row, "cssprop");
        sink body = {0};
        emit_begin(&body, &b, site);
        sp(&body, "    return embi_bind_end(&sc, embi_bind_dstr_value(c, embi_impl_%s_cssprop_get(c, self, magic)));\n", ifc->name);
        finish_native(o, &b, &body, fname, "cssprop_get", shape);
        sink_reset(&body);
        snprintf(fname, sizeof fname, "gen_%s_cssprop_set", ifc->name);
        snprintf(site, sizeof site, "&%s_site", fname);
        snprintf(row, sizeof row, "%s_site", fname);
        emit_site_row(o, &b, row, "cssprop");
        emit_begin(&body, &b, site);
        sp(&body, "    embi_dstr v = embi_dstr_empty();\n");
        sp(&body, "    if (!embi_bind_conv_str(c, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, \"Failed to set a CSS property\", true, &v)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);\n");
        sp(&body, "    embi_impl_%s_cssprop_set(c, self, magic, v);\n    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);\n", ifc->name);
        finish_native(o, &b, &body, fname, "cssprop_set", shape);
        free(body.buf);
    }
}

static void emit_ctor_native(gen *g, sink *o, const idl_iface *ifc, const idl_member *ctor, const char *file)
{
    if (!ctor) {
        return; /* the installer uses embi_bind_illegal_ctor */
    }
    body_ctx b = {0};
    b.file = file;
    b.iface = ifc->name;
    b.is_static = true; /* no receiver */
    g->tmp = 0;
    g_vtmp = 0;
    sp(o, "static const embi_bind_member gen_%s_ctor_site = {.iface = EMBI_IF_%s};\n", ifc->name, ifc->name);
    sp(o, "static embi_js_value gen_%s_ctor(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic, embi_js_value new_target)\n{\n",
       ifc->name);
    sp(o, "    (void)this_v;\n    (void)argc;\n    (void)argv;\n    (void)magic;\n");
    sp(o, "    if (embi_js_is_undefined(new_target)) return embi_bind_ctor_needs_new(r, &gen_%s_ctor_site);\n", ifc->name);
    sp(o, "    embi_bind_scope sc;\n");
    sp(o, "    embi_bind_ctx *c = embi_bind_begin(r, this_v, &gen_%s_ctor_site, EMBI_SELF_NONE, &sc, nullptr);\n", ifc->name);
    sp(o, "    if (!c) return sc.early;\n");
    int required = required_count(ctor);
    if (required > 0) {
        sp(o, "    if (argc < %d) return embi_bind_end(&sc, embi_bind_throw_args(c, EMBI_CTX_CTOR(0), %d, argc));\n",
           required, required);
    }
    emit_args(g, o, &b, ctor, "EMBI_CTX_CTOR", "return embi_bind_end(&sc, EMBI_JS_EXCEPTION);");
    char args[1024];
    call_list(args, sizeof args, ctor, false, false);
    sp(o, "    return embi_bind_end(&sc, embi_impl_%s_constructor(c, new_target%s%s));\n}\n\n", ifc->name,
       ctor->args.n ? ", " : "", ctor->args.n ? args + 3 : "");
}

/* The member rows of one interface (or namespace), in definition order, written as one const
 * table that the generic installer (embi_bind_install_iface) walks. Natives that are not shared
 * (dispatchers, toString) have rows too, with their function in .fn. */
typedef struct inst {
    const char *owner; /* the interface or namespace name */
    const char *tag;   /* the table name suffix */
    char **rows;
    const char **feat; /* per row: the build option macro that guards it (SZ3b), or nullptr */
    int n, cap;
    bool has_feat;     /* some row is guarded: the count in the table is then a sizeof expression */
} inst;

/* The build option macro of a [Feature=Name] extended attribute (Canvas -> EMBER_CANVAS), or nullptr. */
static const char *feature_macro(const idl_exts *e)
{
    const idl_ext *f = idl_ext_get(e, "Feature");
    if (!f || !f->value) {
        return nullptr;
    }
    if (strcmp(f->value, "Canvas") == 0) {
        return "EMBER_CANVAS";
    }
    if (strcmp(f->value, "SVG") == 0) {
        return "EMBER_SVG";
    }
    return nullptr;
}

static void inst_row_f(inst *in, char *row, const char *feat)
{
    if (!row) {
        return;
    }
    if (in->n == in->cap) {
        in->cap = in->cap ? in->cap * 2 : 64;
        in->rows = realloc(in->rows, (size_t)in->cap * sizeof *in->rows);
        in->feat = realloc((void *)in->feat, (size_t)in->cap * sizeof *in->feat);
        if (!in->rows || !in->feat) {
            exit(2);
        }
    }
    in->feat[in->n] = feat;
    if (feat) {
        in->has_feat = true;
    }
    in->rows[in->n++] = row;
}

static void inst_row(inst *in, char *row)
{
    inst_row_f(in, row, nullptr);
}

/* The row of an overloaded operation: its dispatcher. */
static void inst_overloaded(inst *in, const body_ctx *b, const idl_iface *ifc, const idl_member *m, bool maxlen)
{
    char fn[256], iface[160];
    dispatch_name(fn, sizeof fn, ifc->name, m->name);
    int len = required_count(m);
    for (int q = 0; q < ifc->members.n; q++) {
        const idl_member *x = &ifc->members.items[q];
        if (maxlen && x->kind == IDL_M_OP && x->name && strcmp(x->name, m->name) == 0 && x->is_static == m->is_static) {
            int xl = required_count(x);
            if (xl > len) {
                len = xl;
            }
        }
    }
    row_iface(iface, sizeof iface, b, m->unforgeable);
    inst_row(in, dupf("{.name = \"%s\", .fn = %s, %s, .kind = EMBI_MEMBER_%s, .len = %d}", m->name, fn, iface,
                      m->is_static && !b->is_ns ? "SOP" : "OP", len));
}

/* Writes the table; returns the number of rows (nothing is written for none). */
static int inst_table(inst *in, sink *o)
{
    if (in->n == 0) {
        return 0;
    }
    sp(o, "static const embi_bind_member gen_%s_%s[] = {\n", in->owner, in->tag);
    for (int i = 0; i < in->n; i++) {
        if (in->feat[i]) {
            sp(o, "#if %s\n    %s,\n#endif\n", in->feat[i], in->rows[i]);
        } else {
            sp(o, "    %s,\n", in->rows[i]);
        }
        free(in->rows[i]);
    }
    sp(o, "};\n\n");
    free(in->rows);
    free((void *)in->feat);
    return in->n;
}

static void emit_installer(gen *g, sink *o, const idl_iface *ifc, mrow *rows)
{
    (void)g;
    const idl_member *ctor = find_member(ifc, IDL_M_CTOR);
    body_ctx bi = {0};
    bi.iface = ifc->name;
    char tsrow[240];
    row_iface(tsrow, sizeof tsrow, &bi, false);
    inst in = {.owner = ifc->name, .tag = "m"};
    inst un = {.owner = ifc->name, .tag = "mu"};
    for (int k = 0; k < ifc->members.n; k++) {
        const idl_member *m = &ifc->members.items[k];
        inst *target = m->unforgeable ? &un : &in;
        switch (m->kind) {
        case IDL_M_CONST:
            inst_row(&in, dupf("{.name = \"%s\", .num = (double)(%s), .kind = EMBI_MEMBER_CONST}", m->name, m->const_text));
            break;
        case IDL_M_ATTR:
            if (!m->is_static) {
                inst_row_f(target, rows[k].row, feature_macro(&m->ext));
            }
            break;
        case IDL_M_OP:
            if (m->name) {
                if (m->noverloads > 1) {
                    if (m->overload == 0) {
                        inst_overloaded(target, &bi, ifc, m, true);
                    }
                } else {
                    inst_row_f(target, rows[k].row, feature_macro(&m->ext));
                }
            }
            break;
        case IDL_M_STRINGIFIER:
            inst_row(&in, dupf("{.name = \"toString\", .fn = gen_%s_toString, .kind = EMBI_MEMBER_OP}", ifc->name));
            break;
        case IDL_M_ITERABLE:
            if (indexed_getter(ifc)) {
                /* [SymbolIteratorOnly]: only @@iterator (HTMLCollection has no iterable declaration in the spec). */
                inst_row(&in, dupf("{.kind = EMBI_MEMBER_ARRAY_ITER%s}",
                                   idl_has_ext(&m->ext, "SymbolIteratorOnly") ? ", .flags = EMBI_MF_SYMBOL_ONLY" : ""));
            } else {
                inst_row(&in, dupf("{.kind = EMBI_MEMBER_ITER, .data = &gen_%s_iter_ops}", ifc->name));
            }
            break;
        default:
            break;
        }
        if (m->kind == IDL_M_ATTR && m->stringifier) {
            inst_row(&in, dupf("{.name = \"toString\", .fn = gen_%s_toString, .kind = EMBI_MEMBER_OP}", ifc->name));
        }
    }
    if (idl_has_ext(&ifc->ext, "CSSProperties")) {
        if (g_nprops == 0) {
            gen_error(g, ifc->file, ifc->line, "[CSSProperties] needs --props src/css/props.txt");
        }
        inst_row(&in, dupf("{.fn = gen_%s_cssprop_get, .set = gen_%s_cssprop_set, .kind = EMBI_MEMBER_CSSPROPS}",
                           ifc->name, ifc->name));
    }
    bool nm_feat = in.has_feat, nu_feat = un.has_feat;
    int nm = inst_table(&in, o);
    int nu = inst_table(&un, o);
    sp(o, "const embi_bind_iface embi_gen_iface_%s = {\n", ifc->name);
    if (nm && nm_feat) {
        sp(o, "    .members = gen_%s_m,\n    .nmembers = (int)(sizeof gen_%s_m / sizeof gen_%s_m[0]),\n", ifc->name,
           ifc->name, ifc->name);
    } else if (nm) {
        sp(o, "    .members = gen_%s_m,\n    .nmembers = %d,\n", ifc->name, nm);
    }
    if (nu && nu_feat) {
        sp(o, "    .unforge = gen_%s_mu,\n    .nunforge = (int)(sizeof gen_%s_mu / sizeof gen_%s_mu[0]),\n", ifc->name,
           ifc->name, ifc->name);
    } else if (nu) {
        sp(o, "    .unforge = gen_%s_mu,\n    .nunforge = %d,\n", ifc->name, nu);
    }
    if (ctor) {
        sp(o, "    .ctor = gen_%s_ctor,\n    .ctor_len = %d,\n", ifc->name, required_count(ctor));
    }
    bool global = idl_has_ext(&ifc->ext, "Global");
    bool error = idl_has_ext(&ifc->ext, "Error") && !ifc->parent_decl;
    if (global || error) {
        sp(o, "    .flags = %s%s%s,\n", error ? "EMBI_IFACE_ERROR" : "", (error && global) ? " | " : "",
           global ? "EMBI_IFACE_GLOBAL" : "");
    }
    sp(o, "};\n\n");
}

/* True when the interface needs a class of its own: property hooks (indexed or named getters), or
 * the identity that hand-written code tests (the Window class, see embi_bind_ctx_of). The others
 * share embi_gen_class_plain: nothing reads the name of a class. */
static bool own_class(const idl_iface *ifc)
{
    return indexed_getter(ifc) || idl_has_ext(&ifc->ext, "CustomNamed") || idl_has_ext(&ifc->ext, "Global");
}

static void emit_class(sink *o, const idl_iface *ifc)
{
    if (!own_class(ifc)) {
        return;
    }
    const idl_member *idx = indexed_getter(ifc);
    bool named = idl_has_ext(&ifc->ext, "CustomNamed");
    sp(o, "const embi_js_class embi_gen_class_%s = {\n    .name = \"%s\",\n", ifc->name, ifc->name);
    if (named) {
        sp(o, "    .get_own = embi_impl_%s_named_get_own,\n    .define_own = embi_impl_%s_named_define_own,\n    .delete = embi_impl_%s_named_delete,\n    .own_keys = embi_impl_%s_named_own_keys,\n",
                ifc->name, ifc->name, ifc->name, ifc->name);
    if (idl_has_ext(&ifc->ext, "CustomNamedSet")) {
        sp(o, "    .set = embi_impl_%s_named_set,\n", ifc->name);
    }
    } else if (idx) {
        sp(o, "    .get_own = gen_%s_get_own,\n    .define_own = gen_%s_define_own,\n    .delete = gen_%s_delete,\n", ifc->name,
                ifc->name, ifc->name);
        if (has_length_attr(ifc)) {
            sp(o, "    .own_keys = gen_%s_own_keys,\n", ifc->name);
        }
    }
    sp(o, "    .trace = embi_bind_trace,\n    .finalize = embi_bind_finalize,\n};\n\n");
}

/* True when a [Global] interface inherits from ifc: its members also accept a null or undefined this
 * (a bare call such as addEventListener(...) uses the global object). */
static bool is_global_ancestor(const idl_db *db, const idl_iface *ifc)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        const idl_iface *gi = db->ifaces.items[i];
        if (!idl_has_ext(&gi->ext, "Global")) {
            continue;
        }
        for (const idl_iface *p = gi->parent_decl; p; p = p->parent_decl) {
            if (strcmp(p->name, ifc->name) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void emit_iface(gen *g, sink *o, FILE *h, const idl_iface *ifc)
{
    body_ctx b = {0};
    b.file = ifc->file;
    b.iface = ifc->name;
    b.bindobj = idl_has_ext(&ifc->ext, "BindObj");
    b.node_self = ifc->node_derived;
    b.global = idl_has_ext(&ifc->ext, "Global") || is_global_ancestor(g->db, ifc);
    fprintf(h, "\n/* %s */\n", ifc->name);
    if (idl_has_ext(&ifc->ext, "CustomNamed")) {
        fprintf(h, "int embi_impl_%s_named_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out);\n", ifc->name);
        fprintf(h, "int embi_impl_%s_named_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, const embi_js_pdesc *d);\n", ifc->name);
        fprintf(h, "int embi_impl_%s_named_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);\n", ifc->name);
        if (idl_has_ext(&ifc->ext, "CustomNamedSet")) {
            fprintf(h, "int embi_impl_%s_named_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v, embi_js_value receiver);\n", ifc->name);
        }
        fprintf(h, "int embi_impl_%s_named_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out);\n", ifc->name);
    }
    if (idl_has_ext(&ifc->ext, "IndexedSetter") && indexed_getter(ifc) && !idl_has_ext(&ifc->ext, "CustomNamed")) {
        /* The indexed property setter: a data descriptor for an index. EMBI_JS_YES, EMBI_JS_NO (false) or EMBI_JS_THREW. */
        fprintf(h, "int embi_impl_%s_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d);\n", ifc->name);
    }
    if (idl_has_ext(&ifc->ext, "NamedLookup")) {
        fprintf(h, "void embi_impl_%s_named_keys(embi_js_runtime *rt, embi_js_object *o, embi_bind_ctx *c, %s self, embi_js_key_vec *out);\n",
                ifc->name, ifc->node_derived ? "embi_node *" : "embi_bind_obj *");
        fprintf(h, "embi_js_value embi_impl_%s_named(embi_bind_ctx *c, %s self, embi_str name);\n", ifc->name,
                ifc->node_derived ? "embi_node *" : "embi_bind_obj *");
    }
    mrow *rows = calloc(ifc->members.n ? (size_t)ifc->members.n : 1, sizeof *rows);
    if (!rows) {
        exit(2);
    }
    emit_natives_for(g, o, h, &b, ifc, rows);
    const idl_member *ctor = find_member(ifc, IDL_M_CTOR);
    emit_ctor_native(g, o, ifc, ctor, ifc->file);
    const idl_member *idx = indexed_getter(ifc);
    if (idx && !idl_has_ext(&ifc->ext, "CustomNamed")) {
        emit_index_hooks(g, o, ifc, idx);
    }
    emit_class(o, ifc);
    emit_installer(g, o, ifc, rows);
    free(rows);
}

static void emit_namespace(gen *g, sink *o, FILE *h, const idl_iface *ns)
{
    body_ctx b = {0};
    b.file = ns->file;
    b.iface = ns->name;
    b.is_ns = true;
    fprintf(h, "\n/* namespace %s */\n", ns->name);
    mrow *rows = calloc(ns->members.n ? (size_t)ns->members.n : 1, sizeof *rows);
    if (!rows) {
        exit(2);
    }
    for (int k = 0; k < ns->members.n; k++) {
        const idl_member *m = &ns->members.items[k];
        if (m->kind == IDL_M_OP && m->name) {
            emit_impl_proto(h, &b, m, "");
            char fn[256], shape[32];
            native_name(fn, sizeof fn, ns->name, m, "");
            body_ctx bb = b;
            bb.direct = m->noverloads > 1;
            emit_op_native(g, o, &bb, m, fn, shape);
            if (!bb.direct) {
                char impl[256], iface[160];
                impl_name(impl, sizeof impl, b.iface, m, "");
                row_iface(iface, sizeof iface, &b, false);
                rows[k].row = dupf("{.name = \"%s\", .impl = (embi_gen_fn)%s, .fn = %s, %s, .kind = EMBI_MEMBER_OP, .len = %d}",
                                   m->name, impl, shape, iface, required_count(m));
            } else if (m->overload == m->noverloads - 1) {
                for (int q = 0; q < ns->members.n; q++) {
                    const idl_member *x = &ns->members.items[q];
                    if (x->kind == IDL_M_OP && x->name && x->overload == 0 && strcmp(x->name, m->name) == 0) {
                        emit_dispatcher(g, o, ns, x);
                        break;
                    }
                }
            }
        } else if (m->kind == IDL_M_ATTR) {
            gen_error(g, ns->file, m->line, "namespace attributes are not supported");
        }
    }
    inst in = {.owner = ns->name, .tag = "m"};
    for (int k = 0; k < ns->members.n; k++) {
        const idl_member *m = &ns->members.items[k];
        if (m->kind == IDL_M_OP && m->name && m->overload == 0) {
            if (m->noverloads > 1) {
                inst_overloaded(&in, &b, ns, m, false);
            } else {
                inst_row(&in, rows[k].row);
            }
        }
    }
    free(rows);
    int nm = inst_table(&in, o);
    sp(o, "const embi_bind_iface embi_gen_nsdesc_%s = {\n", ns->name);
    if (nm) {
        sp(o, "    .members = gen_%s_m,\n    .nmembers = %d,\n", ns->name, nm);
    }
    sp(o, "};\n\n");
}

/* ------------------------------------------------------------------ enums and dictionaries */

static void emit_enum_names(sink *o, const idl_enum *e)
{
    sp(o, "const char *const embi_enum_names_%s[] = {", e->name);
    for (int i = 0; i < e->values.n; i++) {
        char lit[256];
        cstr(lit, sizeof lit, e->values.items[i]);
        sp(o, "%s%s", i ? ", " : "", lit);
    }
    sp(o, "};\n\n");
}

static idl_dmember *sorted_members(const idl_dict *d)
{
    int n = d->members.n;
    if (n == 0) {
        return nullptr;
    }
    idl_dmember *sorted = malloc(sizeof *sorted * (size_t)n);
    if (!sorted) {
        exit(2);
    }
    memcpy(sorted, d->members.items, sizeof *sorted * (size_t)n);
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_member_name);
    return sorted;
}

static void emit_dict_conv(gen *g, sink *o, const idl_dict *d)
{
    idl_dmember *sorted = sorted_members(d);
    int n = d->members.n;
    sp(o, "bool embi_gen_dict_conv_%s(embi_bind_ctx *c, embi_js_value v, embi_dict_%s *out, const char *ctx)\n{\n",
            d->name, d->name);
    sp(o, "    embi_js_runtime *rt = c->rt;\n    (void)rt;\n");
    if (d->parent_decl) {
        sp(o, "    if (!embi_gen_dict_conv_%s(c, v, &out->base, ctx)) return false;\n", d->parent_decl->name);
    }
    sp(o, "    if (!embi_bind_dict_check(c, v, ctx)) return false;\n");
    for (int k = 0; k < n; k++) {
        const idl_dmember *m = &sorted[k];
        const char *why = unsupported_arg(m->type);
        if (why) {
            gen_error(g, d->file, m->line, "%s", why);
            continue;
        }
        sp(o, "    {\n        embi_js_value mv = EMBI_JS_UNDEFINED;\n");
        sp(o, "        if (!embi_bind_dict_get(c, v, \"%s\", &mv)) return false;\n", m->name);
        sp(o, "        if (embi_js_is_undefined(mv)) {\n");
        if (m->required) {
            sp(o, "            return embi_bind_dict_required(c, \"%s\", ctx);\n", m->name);
        } else {
            char def[256];
            default_expr(m->type, m->def, def, sizeof def);
            sp(o, "            out->%s = %s;\n", m->name, def);
        }
        sp(o, "        } else {\n");
        char dst[160];
        snprintf(dst, sizeof dst, "out->%s", m->name);
        emit_conv(g, o, m->type, 0, dst, "mv", "ctx", "return false;", d->file, m->line);
        sp(o, "        }\n    }\n");
    }
    sp(o, "    return true;\n}\n\n");
    free(sorted);
}

/* The converter from a dictionary struct to a JS object (for dictionaries used as results). */
static void emit_dict_to_value(gen *g, sink *o, const idl_dict *d)
{
    idl_dmember *sorted = sorted_members(d);
    int n = d->members.n;
    sp(o, "embi_js_value embi_gen_dict_to_value_%s(embi_bind_ctx *c, const embi_dict_%s *in)\n{\n", d->name, d->name);
    sp(o, "    embi_js_value obj = embi_bind_object_new(c);\n    embi_bind_pin(c, obj);\n");
    if (d->parent_decl) {
        sp(o, "    {\n        embi_js_value base = embi_gen_dict_to_value_%s(c, &in->base);\n", d->parent_decl->name);
        sp(o, "        (void)base;\n    }\n");
    }
    for (int k = 0; k < n; k++) {
        const idl_dmember *m = &sorted[k];
        char ret[200], v[768];
        snprintf(ret, sizeof ret, "in->%s", m->name);
        if (m->type->kind == IDL_T_UNION) {
            gen_error(g, d->file, m->line, "union members of result dictionaries are not supported");
            continue;
        }
        value_expr(m->type, ret, v, sizeof v);
        sp(o, "    embi_bind_object_set_c(c, obj, \"%s\", %s);\n", m->name, v);
    }
    sp(o, "    return obj;\n}\n\n");
    free(sorted);
}

/* The struct of a dictionary (its parent first) and the converter's declaration in types.h. */
static void emit_dict_struct(gen *g, FILE *h, const idl_dict *d)
{
    fprintf(h, "typedef struct embi_dict_%s {\n", d->name);
    if (d->parent_decl) {
        fprintf(h, "    embi_dict_%s base;\n", d->parent_decl->name);
    }
    idl_dmember *sorted = sorted_members(d);
    for (int k = 0; k < d->members.n; k++) {
        const idl_dmember *m = &sorted[k];
        const char *why = unsupported_arg(m->type);
        if (why) {
            gen_error(g, d->file, m->line, "%s", why);
            continue;
        }
        char cty[160];
        ctype_of(m->type, cty, sizeof cty);
        fprintf(h, "    %s %s;\n", cty, m->name);
    }
    free(sorted);
    if (d->members.n == 0 && !d->parent_decl) {
        fprintf(h, "    char unused_;\n");
    }
    fprintf(h, "} embi_dict_%s;\n", d->name);
    fprintf(h, "bool embi_gen_dict_conv_%s(embi_bind_ctx *c, embi_js_value v, embi_dict_%s *out, const char *ctx);\n",
            d->name, d->name);
    if (d->result) {
        fprintf(h, "embi_js_value embi_gen_dict_to_value_%s(embi_bind_ctx *c, const embi_dict_%s *in);\n", d->name,
                d->name);
    }
    fprintf(h, "\n");
}

/* Dictionaries in an order where the structs a dictionary embeds (its parent, nested dictionary
 * members) come first. */
static int dict_order(const idl_db *db, idl_dict **list, int n, idl_dict **out)
{
    int k = 0;
    bool *done = calloc((size_t)n + 1, sizeof *done);
    if (!done) {
        exit(2);
    }
    for (int round = 0; round < n + 1; round++) {
        for (int i = 0; i < n; i++) {
            if (done[i]) {
                continue;
            }
            bool ready = true;
            const idl_dict *deps[64];
            int nd = 0;
            if (list[i]->parent_decl) {
                deps[nd++] = list[i]->parent_decl;
            }
            for (int q = 0; q < list[i]->members.n && nd < 63; q++) {
                const idl_type *t = list[i]->members.items[q].type;
                while (t && (t->kind == IDL_T_SEQUENCE || t->kind == IDL_T_RECORD) && t->nsub) {
                    t = t->sub[t->nsub - 1];
                }
                if (t && t->kind == IDL_T_DICT) {
                    deps[nd++] = t->target;
                }
            }
            for (int q = 0; q < nd && ready; q++) {
                bool found = false;
                for (int j = 0; j < n; j++) {
                    if (list[j] == deps[q] && done[j]) {
                        found = true;
                    }
                }
                ready = found;
            }
            if (ready) {
                done[i] = true;
                out[k++] = list[i];
            }
        }
    }
    (void)db;
    free(done);
    return k;
}

/* ------------------------------------------------------------------ CSS properties */

static int cmp_cstr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void idl_set_cssprops(const char *props_txt)
{
    /* Property names: the first '|' separated field of each non-comment line. */
    const char *p = props_txt;
    int cap = 512;
    g_props = malloc(sizeof *g_props * (size_t)cap);
    g_nprops = 0;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && p[0] != '#' && p[0] != ' ' && p[0] != '\r') {
            size_t n = 0;
            while (n < len && p[n] != '|' && p[n] != ' ' && p[n] != '\r') {
                n++;
            }
            /* "-internal-" longhands (see tools/cssgen) are not author-visible: no CSSStyleDeclaration name. */
            if (n && !(n >= 10 && strncmp(p, "-internal-", 10) == 0)) {
                char *s = malloc(n + 1);
                memcpy(s, p, n);
                s[n] = '\0';
                if (g_nprops == cap) {
                    cap *= 2;
                    g_props = realloc(g_props, sizeof *g_props * (size_t)cap);
                }
                g_props[g_nprops++] = s;
            }
        }
        p = e ? e + 1 : nullptr;
    }
    qsort(g_props, (size_t)g_nprops, sizeof *g_props, cmp_cstr);
}

static void camel_of(const char *dashed, char *out, size_t cap)
{
    if (strcmp(dashed, "float") == 0) {
        snprintf(out, cap, "cssFloat");
        return;
    }
    size_t n = 0;
    bool up = false;
    const char *s = dashed;
    if (s[0] == '-') {
        s++; /* -webkit-x -> webkitX */
    }
    for (; *s && n + 1 < cap; s++) {
        if (*s == '-') {
            up = true;
        } else {
            out[n++] = up ? (char)toupper((unsigned char)*s) : *s;
            up = false;
        }
    }
    out[n] = '\0';
}

int idl_gen_cssprops(idl_db *db, const char *props_txt, FILE *out_c)
{
    (void)db;
    if (!g_props) {
        idl_set_cssprops(props_txt);
    }
    fprintf(out_c, "/* Generated by tools/idlgen from src/css/props.txt. Do not edit. */\n");
    fprintf(out_c, "#include \"bindings/gen/types.h\"\n\n");
    fprintf(out_c, "const int embi_css_prop_count = %d;\n", g_nprops);
    fprintf(out_c, "const char *const embi_css_prop_names[%d] = {\n", g_nprops ? g_nprops : 1);
    for (int i = 0; i < g_nprops; i++) {
        fprintf(out_c, "    \"%s\",\n", g_props[i]);
    }
    if (!g_nprops) {
        fprintf(out_c, "    nullptr,\n");
    }
    fprintf(out_c, "};\nconst char *const embi_css_prop_camel[%d] = {\n", g_nprops ? g_nprops : 1);
    for (int i = 0; i < g_nprops; i++) {
        char camel[200];
        camel_of(g_props[i], camel, sizeof camel);
        fprintf(out_c, "    \"%s\",\n", camel);
    }
    if (!g_nprops) {
        fprintf(out_c, "    nullptr,\n");
    }
    fprintf(out_c, "};\n");
    return 0;
}

/* ------------------------------------------------------------------ file and shared output */

#define GEN_MAX 4096

int idl_gen_file(idl_db *db, const char *stem, FILE *out_c, FILE *out_h)
{
    g_vtmp = 0;
    gen g = {0};
    g.db = db;
    static idl_iface *ifaces[GEN_MAX];
    static idl_dict *dicts[GEN_MAX];
    static idl_enum *enums[GEN_MAX];
    sink so = {.f = out_c};
    mark_result_dicts(db);
    int ni = file_ifaces(db, stem, ifaces, GEN_MAX);
    int nd = file_dicts(db, stem, dicts, GEN_MAX);
    int ne = file_enums(db, stem, enums, GEN_MAX);

    fprintf(out_h, "/* Generated by tools/idlgen from src/bindings/idl/%s.webidl. Do not edit. */\n#pragma once\n",
            stem);
    fprintf(out_h, "#include \"bindings/gen/types.h\"\n\n");
    fprintf(out_h, "/* The implementations of this file's members (src/bindings/impl_*.c). */\n");

    fprintf(out_c, "/* Generated by tools/idlgen from src/bindings/idl/%s.webidl. Do not edit. */\n", stem);
    fprintf(out_c, "#include \"bindings/bind.h\"\n#include \"bindings/gen/%s.h\"\n#include \"bindings/gen/shapes.h\"\n#include \"js/object.h\"\n#include \"js/ops.h\"\n#include \"js/runtime.h\"\n#include \"js/string.h\"\n\n", stem);
    for (int i = 0; i < ne; i++) {
        emit_enum_names(&so, enums[i]);
    }
    for (int i = 0; i < ni; i++) {
        if (ifaces[i]->ns || ifaces[i]->callback_iface) {
            continue;
        }
        if (ifaces[i]->id < 0) {
            gen_error(&g, stem, ifaces[i]->line, "interface without an id");
        }
        emit_iface(&g, &so, out_h, ifaces[i]);
    }
    for (int i = 0; i < ni; i++) {
        if (ifaces[i]->ns) {
            emit_namespace(&g, &so, out_h, ifaces[i]);
        }
    }
    for (int i = 0; i < nd; i++) {
        emit_dict_conv(&g, &so, dicts[i]);
        if (dicts[i]->result) {
            emit_dict_to_value(&g, &so, dicts[i]);
        }
    }
    return g.errors;
}

/* The tag names of an interface ([HTMLTag="a"] or [HTMLTags="h1 h2"]), split on spaces. */
typedef struct tag_ent {
    char *tag;
    const char *iface;
} tag_ent;

static int cmp_tag(const void *a, const void *b)
{
    return strcmp(((const tag_ent *)a)->tag, ((const tag_ent *)b)->tag);
}

int idl_gen_shared(idl_db *db, FILE *out_types_h, FILE *out_ifaces_c)
{
    gen g = {0};
    g.db = db;
    static idl_iface *all[GEN_MAX];
    static idl_dict *dicts[GEN_MAX], *dord[GEN_MAX];
    static idl_enum *enums[GEN_MAX];
    mark_result_dicts(db);
    int n = file_ifaces(db, nullptr, all, GEN_MAX);
    int nd = file_dicts(db, nullptr, dicts, GEN_MAX);
    int dn = dict_order(db, dicts, nd, dord);
    int ne = file_enums(db, nullptr, enums, GEN_MAX);
    int nids = 0, nns = 0;
    static idl_iface *ids[GEN_MAX], *nss[GEN_MAX];
    const char *global = nullptr;
    for (int i = 0; i < n; i++) {
        if (all[i]->ns) {
            nss[nns++] = all[i];
        } else if (!all[i]->callback_iface) {
            ids[nids++] = all[i];
            if (idl_has_ext(&all[i]->ext, "Global")) {
                global = all[i]->name;
            }
        }
    }
    /* ids were built sorted by name, so the enum order is the id order. */
    fprintf(out_types_h, "/* Generated by tools/idlgen from the WebIDL files in src/bindings/idl. Do not edit. */\n#pragma once\n");
    fprintf(out_types_h, "#include \"bindings/bind.h\"\n#include \"js/object.h\"\n\n");
    fprintf(out_types_h, "enum embi_iface_id {\n");
    for (int i = 0; i < nids; i++) {
        fprintf(out_types_h, "    EMBI_IF_%s = %d,\n", ids[i]->name, i);
    }
    fprintf(out_types_h, "    EMBI_IF_COUNT = %d,\n};\n", nids);
    fprintf(out_types_h, "#define EMBI_IF_NONE 0xFFFFu\n");
    fprintf(out_types_h, "#define EMBI_NS_COUNT %d\n#define EMBI_NS_SLOTS %d\n", nns, nns ? nns : 1);
    fprintf(out_types_h, "enum embi_ns_id {\n");
    for (int i = 0; i < nns; i++) {
        fprintf(out_types_h, "    EMBI_NSID_%s = %d,\n", nss[i]->name, i);
    }
    fprintf(out_types_h, "    EMBI_NSID_COUNT_ = %d,\n};\n", nns);
    if (global) {
        fprintf(out_types_h, "#define EMBI_IF_GLOBAL EMBI_IF_%s\n", global);
    } else {
        fprintf(out_types_h, "#define EMBI_IF_GLOBAL EMBI_IF_NONE\n");
    }
    fprintf(out_types_h, "\nextern const embi_js_class *const embi_bind_classes[EMBI_IF_COUNT];\n");
    fprintf(out_types_h, "extern const embi_js_class embi_gen_class_plain;\n");
    fprintf(out_types_h, "extern const embi_bind_iface *const embi_bind_ifaces[EMBI_IF_COUNT];\n");
    fprintf(out_types_h, "extern const bool embi_bind_testing[EMBI_IF_COUNT];\n");
    fprintf(out_types_h, "extern const char *const embi_bind_names[EMBI_IF_COUNT];\n");
    fprintf(out_types_h, "extern const uint16_t embi_bind_parent[EMBI_IF_COUNT];\n");
    fprintf(out_types_h, "extern const char *const embi_bind_ns_names[EMBI_NS_SLOTS];\n");
    fprintf(out_types_h, "extern const embi_bind_iface *const embi_bind_nsdescs[EMBI_NS_SLOTS];\n");
    fprintf(out_types_h, "extern const int embi_css_prop_count;\n");
    fprintf(out_types_h, "extern const char *const embi_css_prop_names[];\n");
    fprintf(out_types_h, "extern const char *const embi_css_prop_camel[];\n");
    fprintf(out_types_h, "/* Defines the interface object of a [Testing] interface; used when it is exposed. */\n\n");
    for (int i = 0; i < nids; i++) {
        if (own_class(ids[i])) {
            fprintf(out_types_h, "extern const embi_js_class embi_gen_class_%s;\n", ids[i]->name);
        }
        fprintf(out_types_h, "extern const embi_bind_iface embi_gen_iface_%s;\n", ids[i]->name);
    }
    for (int i = 0; i < nns; i++) {
        fprintf(out_types_h, "extern const embi_bind_iface embi_gen_nsdesc_%s;\n", nss[i]->name);
    }
    fprintf(out_types_h, "\n");
    for (int i = 0; i < ne; i++) {
        const idl_enum *e = enums[i];
        fprintf(out_types_h, "#define EMBI_ENUM_COUNT_%s %d\n", e->name, e->values.n);
        fprintf(out_types_h, "enum {\n");
        for (int k = 0; k < e->values.n; k++) {
            char cname[256];
            enum_const(cname, sizeof cname, e->name, e->values.items[k]);
            fprintf(out_types_h, "    %s = %d,\n", cname, k);
        }
        fprintf(out_types_h, "};\nextern const char *const embi_enum_names_%s[];\n\n", e->name);
    }
    for (int i = 0; i < dn; i++) {
        emit_dict_struct(&g, out_types_h, dord[i]);
    }
    /* The tables. */
    fprintf(out_ifaces_c, "/* Generated by tools/idlgen from the WebIDL files in src/bindings/idl. Do not edit. */\n");
    fprintf(out_ifaces_c, "#include \"bindings/gen/types.h\"\n\n");
    fprintf(out_ifaces_c, "/* The [Testing] interfaces are left out of builds without the unit tests. */\n#ifdef EMBI_BIND_NO_TESTING\n#define EMBI_T(x) nullptr\n#else\n#define EMBI_T(x) x\n#endif\n\n");
    fprintf(out_ifaces_c, "/* The interfaces of a build option ([Feature=Canvas], [Feature=SVG]) are nullptr when it is off (SZ3b). */\n"
                          "#if EMBER_CANVAS\n#define EMBI_F_CANVAS(x) x\n#else\n#define EMBI_F_CANVAS(x) nullptr\n#endif\n"
                          "#if EMBER_SVG\n#define EMBI_F_SVG(x) x\n#else\n#define EMBI_F_SVG(x) nullptr\n#endif\n\n");
    fprintf(out_ifaces_c, "/* The class of the interfaces without property hooks. */\nconst embi_js_class embi_gen_class_plain = {\n    .name = \"BindObject\",    .trace = embi_bind_trace,\n    .finalize = embi_bind_finalize,\n};\n");
    fprintf(out_ifaces_c, "\nconst embi_js_class *const embi_bind_classes[EMBI_IF_COUNT] = {\n");
    for (int i = 0; i < nids; i++) {
        if (feature_macro(&ids[i]->ext) && own_class(ids[i])) {
            fprintf(out_ifaces_c, "    EMBI_F_%s(&embi_gen_class_%s),\n", feature_macro(&ids[i]->ext) + 6, ids[i]->name);
            continue;
        }
        fprintf(out_ifaces_c, idl_has_ext(&ids[i]->ext, "Testing") ? "    EMBI_T(&embi_gen_class_%s),\n" : "    &embi_gen_class_%s,\n", own_class(ids[i]) ? ids[i]->name : "plain");
    }
    fprintf(out_ifaces_c, "};\n\n");
    fprintf(out_ifaces_c, "const embi_bind_iface *const embi_bind_ifaces[EMBI_IF_COUNT] = {\n");
    for (int i = 0; i < nids; i++) {
        if (feature_macro(&ids[i]->ext)) {
            fprintf(out_ifaces_c, "    EMBI_F_%s(&embi_gen_iface_%s),\n", feature_macro(&ids[i]->ext) + 6, ids[i]->name);
            continue;
        }
        fprintf(out_ifaces_c, idl_has_ext(&ids[i]->ext, "Testing") ? "    EMBI_T(&embi_gen_iface_%s),\n" : "    &embi_gen_iface_%s,\n", ids[i]->name);
    }
    fprintf(out_ifaces_c, "};\n\nconst bool embi_bind_testing[EMBI_IF_COUNT] = {\n");
    for (int i = 0; i < nids; i++) {
        fprintf(out_ifaces_c, "    %s,\n", idl_has_ext(&ids[i]->ext, "Testing") ? "true" : "false");
    }
    fprintf(out_ifaces_c, "};\n\n");
    fprintf(out_ifaces_c, "const char *const embi_bind_names[EMBI_IF_COUNT] = {\n");
    for (int i = 0; i < nids; i++) {
        fprintf(out_ifaces_c, "    \"%s\",\n", ids[i]->name);
    }
    fprintf(out_ifaces_c, "};\n\n");
    fprintf(out_ifaces_c, "const uint16_t embi_bind_parent[EMBI_IF_COUNT] = {\n");
    for (int i = 0; i < nids; i++) {
        const idl_iface *p = ids[i]->parent_decl;
        if (p && p->id >= 0) {
            fprintf(out_ifaces_c, "    EMBI_IF_%s,\n", p->name);
        } else {
            fprintf(out_ifaces_c, "    EMBI_IF_NONE,\n");
        }
    }
    fprintf(out_ifaces_c, "};\n\n");
    fprintf(out_ifaces_c, "const char *const embi_bind_ns_names[EMBI_NS_SLOTS] = {");
    for (int i = 0; i < nns; i++) {
        fprintf(out_ifaces_c, "\"%s\", ", nss[i]->name);
    }
    fprintf(out_ifaces_c, "%s};\n", nns ? "" : "nullptr");
    fprintf(out_ifaces_c, "const embi_bind_iface *const embi_bind_nsdescs[EMBI_NS_SLOTS] = {");
    for (int i = 0; i < nns; i++) {
        fprintf(out_ifaces_c, "&embi_gen_nsdesc_%s, ", nss[i]->name);
    }
    fprintf(out_ifaces_c, "%s};\n\n", nns ? "" : "nullptr");

    /* The tag-to-interface table of the HTML elements, sorted by tag name. */
    static tag_ent tags[GEN_MAX];
    int ntags = 0;
    for (int i = 0; i < nids; i++) {
        const idl_ext *t = idl_ext_get(&ids[i]->ext, "HTMLTag");
        if (!t) {
            t = idl_ext_get(&ids[i]->ext, "HTMLTags");
        }
        if (!t || !t->value) {
            continue;
        }
        const char *p = t->value;
        while (*p && ntags < GEN_MAX) {
            while (*p == ' ') {
                p++;
            }
            const char *e = p;
            while (*e && *e != ' ') {
                e++;
            }
            if (e == p) {
                break;
            }
            size_t tl = (size_t)(e - p);
            char *tg = malloc(tl + 1);
            memcpy(tg, p, tl);
            tg[tl] = 0;
            p = e;
            tags[ntags].tag = tg;
            tags[ntags].iface = ids[i]->name;
            ntags++;
        }
    }
    qsort(tags, (size_t)ntags, sizeof tags[0], cmp_tag);
    for (int i = 1; i < ntags; i++) {
        if (strcmp(tags[i].tag, tags[i - 1].tag) == 0) {
            gen_error(&g, "idl", 0, "tag '%s' maps to two interfaces", tags[i].tag);
        }
    }
    fprintf(out_ifaces_c, "static const struct {\n    const char *tag;\n    uint16_t len;\n    uint16_t iface;\n} embi_bind_tags[%d] = {\n",
            ntags ? ntags : 1);
    for (int i = 0; i < ntags; i++) {
        fprintf(out_ifaces_c, "    {\"%s\", %zu, EMBI_IF_%s},\n", tags[i].tag, strlen(tags[i].tag), tags[i].iface);
    }
    if (!ntags) {
        fprintf(out_ifaces_c, "    {\"\", 0, 0},\n");
    }
    fprintf(out_ifaces_c, "};\n\n");
    /* The SVG-namespace table ([SVGTag="g"] or [SVGTags="..."]): case-sensitive names (linearGradient), with its own
     * default (SVGElement). The same names may map differently in the HTML namespace (a, style, title). */
    static tag_ent svg_tags[GEN_MAX];
    int nsvg = 0;
    for (int i = 0; i < nids; i++) {
        const idl_ext *t = idl_ext_get(&ids[i]->ext, "SVGTag");
        if (!t) {
            t = idl_ext_get(&ids[i]->ext, "SVGTags");
        }
        if (!t || !t->value) {
            continue;
        }
        const char *p = t->value;
        while (*p && nsvg < GEN_MAX) {
            while (*p == ' ') {
                p++;
            }
            const char *e = p;
            while (*e && *e != ' ') {
                e++;
            }
            if (e == p) {
                break;
            }
            size_t tl = (size_t)(e - p);
            char *tg = malloc(tl + 1);
            memcpy(tg, p, tl);
            tg[tl] = 0;
            p = e;
            svg_tags[nsvg].tag = tg;
            svg_tags[nsvg].iface = ids[i]->name;
            nsvg++;
        }
    }
    qsort(svg_tags, (size_t)nsvg, sizeof svg_tags[0], cmp_tag);
    for (int i = 1; i < nsvg; i++) {
        if (strcmp(svg_tags[i].tag, svg_tags[i - 1].tag) == 0) {
            gen_error(&g, "idl", 0, "SVG tag '%s' maps to two interfaces", svg_tags[i].tag);
        }
    }
    fprintf(out_ifaces_c, "#if EMBER_SVG\n");
    fprintf(out_ifaces_c, "static const struct {\n    const char *tag;\n    uint16_t len;\n    uint16_t iface;\n} embi_bind_svg_tags[%d] = {\n",
            nsvg ? nsvg : 1);
    for (int i = 0; i < nsvg; i++) {
        fprintf(out_ifaces_c, "    {\"%s\", %zu, EMBI_IF_%s},\n", svg_tags[i].tag, strlen(svg_tags[i].tag), svg_tags[i].iface);
    }
    if (!nsvg) {
        fprintf(out_ifaces_c, "    {\"\", 0, 0},\n");
    }
    fprintf(out_ifaces_c, "};\n\n");
    fprintf(out_ifaces_c, "static uint16_t embi_bind_svg_iface_for_tag(embi_atom local)\n{\n");
    fprintf(out_ifaces_c, "    embi_str s = embi_atom_str(local);\n");
    fprintf(out_ifaces_c, "    int lo = 0, hi = %d - 1;\n", nsvg);
    fprintf(out_ifaces_c, "    while (lo <= hi) {\n        int mid = (lo + hi) / 2;\n");
    fprintf(out_ifaces_c, "        uint32_t n = embi_bind_svg_tags[mid].len < s.len ? embi_bind_svg_tags[mid].len : s.len;\n");
    fprintf(out_ifaces_c, "        int cmp = SDL_memcmp(s.ptr, embi_bind_svg_tags[mid].tag, n);\n");
    fprintf(out_ifaces_c, "        if (cmp == 0) cmp = (s.len > embi_bind_svg_tags[mid].len) - (s.len < embi_bind_svg_tags[mid].len);\n");
    fprintf(out_ifaces_c, "        if (cmp == 0) return embi_bind_svg_tags[mid].iface;\n");
    fprintf(out_ifaces_c, "        if (cmp < 0) hi = mid - 1; else lo = mid + 1;\n    }\n");
    fprintf(out_ifaces_c, "    return EMBI_IF_SVGElement;\n}\n#endif\n\n");
    for (int i = 0; i < nsvg; i++) {
        free(svg_tags[i].tag);
    }
    fprintf(out_ifaces_c, "uint16_t embi_bind_iface_for_tag(uint8_t ns, embi_atom local)\n{\n");
    fprintf(out_ifaces_c, "#if EMBER_SVG\n    if (ns == EMBI_NS_SVG) return embi_bind_svg_iface_for_tag(local);\n#else\n    if (ns == EMBI_NS_SVG) return EMBI_IF_SVGElement;\n#endif\n");
    fprintf(out_ifaces_c, "    if (ns != EMBI_NS_HTML) return EMBI_IF_Element;\n");
    fprintf(out_ifaces_c, "    embi_str s = embi_atom_str(local);\n");
    fprintf(out_ifaces_c, "    int lo = 0, hi = %d - 1;\n", ntags);
    fprintf(out_ifaces_c, "    while (lo <= hi) {\n        int mid = (lo + hi) / 2;\n");
    fprintf(out_ifaces_c, "        uint32_t n = embi_bind_tags[mid].len < s.len ? embi_bind_tags[mid].len : s.len;\n");
    fprintf(out_ifaces_c, "        int cmp = SDL_memcmp(s.ptr, embi_bind_tags[mid].tag, n);\n");
    fprintf(out_ifaces_c, "        if (cmp == 0) cmp = (s.len > embi_bind_tags[mid].len) - (s.len < embi_bind_tags[mid].len);\n");
    fprintf(out_ifaces_c, "        if (cmp == 0) return embi_bind_tags[mid].iface;\n");
    fprintf(out_ifaces_c, "        if (cmp < 0) hi = mid - 1; else lo = mid + 1;\n    }\n");
    fprintf(out_ifaces_c, "    for (uint32_t i = 0; i < s.len; i++) if (s.ptr[i] == '-') return EMBI_IF_HTMLElement;\n");
    fprintf(out_ifaces_c, "    return EMBI_IF_HTMLUnknownElement;\n}\n");
    for (int i = 0; i < ntags; i++) {
        free(tags[i].tag);
    }
    return g.errors;
}

/* ------------------------------------------------------------------ shared natives */

static int cmp_shape(const void *a, const void *b)
{
    return strcmp(((const shape_ent *)a)->name, ((const shape_ent *)b)->name);
}

void idl_gen_shapes(FILE *out_c, FILE *out_h)
{
    if (g_nshapes > 0) { /* g_shapes is nullptr when nothing was registered (qsort wants a pointer even for 0) */
        qsort(g_shapes, g_nshapes, sizeof *g_shapes, cmp_shape);
    }
    fprintf(out_h, "/* Generated by tools/idlgen from the WebIDL files in src/bindings/idl. Do not edit. */\n#pragma once\n");
    fprintf(out_h, "#include \"bindings/gen/types.h\"\n\n");
    fprintf(out_h, "/* The native functions the member tables share (see gen/shapes.c). */\n");
    fprintf(out_c, "/* Generated by tools/idlgen from the WebIDL files in src/bindings/idl. Do not edit.\n");
    fprintf(out_c, " *\n * One native function per distinct body: a member's implementation, its name and its interface\n");
    fprintf(out_c, " * are data in its row of the member table (embi_bind_member), which the function reads from the\n");
    fprintf(out_c, " * data of the function being called. */\n");
    fprintf(out_c, "#include \"bindings/bind.h\"\n#include \"bindings/gen/shapes.h\"\n#include \"js/object.h\"\n");
    fprintf(out_c, "#include \"js/ops.h\"\n#include \"js/runtime.h\"\n#include \"js/string.h\"\n\n");
    for (size_t i = 0; i < g_nshapes; i++) {
        const shape_ent *s = &g_shapes[i];
        fprintf(out_h, "embi_js_value %s(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);\n",
                s->name);
        fprintf(out_c, "%s/* %s */\nembi_js_value %s(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)\n{\n%s}\n%s\n",
                s->shipped ? "" : "#ifndef EMBI_BIND_NO_TESTING\n", s->example, s->name, s->text,
                s->shipped ? "" : "#endif\n");
        free(s->text);
        free(s->example);
    }
    free(g_shapes);
    g_shapes = nullptr;
    g_nshapes = g_capshapes = 0;
}
