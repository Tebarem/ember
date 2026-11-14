/*
 * RegExp objects (ECMA-262 section 22.2): the RegExp constructor and its species getter,
 * RegExp.escape, RegExp.prototype (exec, test, toString, compile, the flag and source getters) and
 * the Symbol methods @@match, @@matchAll, @@replace, @@search and @@split, with
 * %RegExpStringIteratorPrototype%. The matching itself is the engine of js/regexp.h; this file
 * holds the JS-visible algorithms of the spec.
 *
 * A RegExp object's internal slot is an rx_data: the compiled program, the flags bits and the
 * source text. rx_data is shared by reference count: a literal's template and every object cloned
 * from it hold one reference, and compile() replaces the reference of its object. The object trace
 * marks the source string; the finalizer drops the reference.
 *
 * RegExp literals (J4c): the parser compiles the pattern once (early errors) and stores a template
 * object in the code's constants. NEW_REGEXP clones it, so each evaluation makes a fresh object
 * that shares the program.
 *
 * Fast paths: test, @@match, @@replace and @@split run the engine directly, without result objects,
 * when the receiver is pristine (see "pristine RegExp objects" below): none of the steps they skip
 * can run user code or be observed, so the results and the lastIndex writes are the same as the
 * spec's generic algorithm, which every other case still runs.
 */
#include "js/builtins.h"
#include "js/heap.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/ops.h"
#include "js/regexp.h"
#include "js/string.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define KEY(rt, s) embi_js_key_c((rt), (s))

typedef EMBI_VEC(uint16_t) u16buf;
typedef EMBI_VEC(uint32_t) u32buf;

/* The most captures @@replace reads from one exec result. A script-supplied length above this throws
 * a RangeError, so that a hostile result cannot make the replace build an unbounded argument list.
 * The engine's own patterns have far fewer groups. */
#define RX_MAX_CAPTURES 65535u

/* ------------------------------------------------------------------ internal slots */

typedef struct rx_data {
    uint32_t refs;
    uint32_t flags;        /* EMBI_RE_FLAG_* bits */
    embi_re_prog *prog;
    embi_js_value source;  /* the pattern text, a string */
} rx_data;

static void rx_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_value(t, ((rx_data *)o->internal)->source);
    }
}

static void rx_release(rx_data *d)
{
    if (--d->refs == 0) {
        embi_re_free(d->prog);
        embi_free(d, sizeof *d);
    }
}

static void rx_finalize(embi_js_object *o)
{
    if (o->internal) {
        rx_release(o->internal);
        o->internal = nullptr;
    }
}

static const embi_js_class class_regexp = {
    .name = "RegExp",
    .trace = rx_trace,
    .finalize = rx_finalize,
};

/* Class of the iterator returned by @@matchAll. Its internal is a varray of
 * [regexp, string, global, unicode, done]. */
enum { RI_R, RI_S, RI_GLOBAL, RI_UNICODE, RI_DONE, RI_N };

static void rx_iter_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, &((embi_js_varray *)o->internal)->hdr);
    }
}

static const embi_js_class class_rx_iter = {
    .name = "RegExp String Iterator",
    .trace = rx_iter_trace,
};

/* The embedder's interrupt, polled by the matcher (the interpreter's own check, see interp.c). */
static bool re_interrupt(void *ctx)
{
    embi_js_runtime *rt = ctx;
    embi_js_vm *vm = rt->vm;
    return vm && vm->interrupt && vm->interrupt(rt, vm->interrupt_user);
}

static bool is_rx_obj(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == &class_regexp &&
           embi_js_as_object(v)->internal != nullptr;
}

bool embi_js_is_regexp(embi_js_value v)
{
    return is_rx_obj(v);
}

static rx_data *rx_of(embi_js_value v)
{
    return (rx_data *)embi_js_as_object(v)->internal;
}

/* ------------------------------------------------------------------ helpers */

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value str_val(embi_js_string *s)
{
    return s ? embi_js_from_string(s) : EXC;
}

static embi_js_value ascii_str(embi_js_runtime *rt, const char *p)
{
    return str_val(embi_js_str_new_latin1(rt, (const uint8_t *)p, strlen(p)));
}

static bool has_char(const embi_js_string *s, uint16_t c)
{
    uint32_t n = embi_js_str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, i) == c) {
            return true;
        }
    }
    return false;
}

/* A UTF-16 view of a string: wide strings in place, Latin-1 strings copied into an owned buffer. */
typedef struct units {
    const uint16_t *p;
    uint16_t *own;
    uint32_t n;
} units;

static void units_of(const embi_js_string *s, units *u)
{
    u->n = embi_js_str_len(s);
    u->own = nullptr;
    if (embi_js_str_is_wide(s)) {
        u->p = embi_js_str_wide(s);
        return;
    }
    u->own = embi_alloc((size_t)u->n * sizeof(uint16_t) + sizeof(uint16_t));
    const uint8_t *b = embi_js_str_latin1(s);
    for (uint32_t i = 0; i < u->n; i++) {
        u->own[i] = b[i];
    }
    u->p = u->own;
}

static void units_free(units *u)
{
    if (u->own) {
        embi_free(u->own, (size_t)u->n * sizeof(uint16_t) + sizeof(uint16_t));
        u->own = nullptr;
    }
}

static void ub_str(u16buf *b, const embi_js_string *s, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++) {
        embi_vec_push(b, embi_js_str_at(s, i));
    }
}

static void ub_ascii(u16buf *b, const char *p)
{
    for (; *p; p++) {
        embi_vec_push(b, (uint16_t)(unsigned char)*p);
    }
}

static void ub_hex(u16buf *b, uint32_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        embi_vec_push(b, (uint16_t)hex[(v >> (4 * i)) & 0xFu]);
    }
}

/* Turns the buffer into a string and frees it. */
static embi_js_value ub_finish(embi_js_runtime *rt, u16buf *b)
{
    embi_js_string *s = embi_js_str_new_utf16(rt, b->data, b->len);
    embi_vec_free(b);
    return str_val(s);
}

static bool cdp(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v)
{
    return embi_js_create_data_property_or_throw(rt, o, k, v) == EMBI_JS_YES;
}

static embi_js_value obj_get(embi_js_runtime *rt, embi_js_value o, const char *name)
{
    return embi_js_get(rt, embi_js_as_object(o), KEY(rt, name));
}

/* Set(O, "lastIndex", v, true). */
static bool set_last_index(embi_js_runtime *rt, embi_js_object *o, double v)
{
    return embi_js_put(rt, o, KEY(rt, "lastIndex"), embi_js_number(v), true) == EMBI_JS_YES;
}

/* AdvanceStringIndex(S, index, unicode). */
static double advance_index(const embi_js_string *s, double index, bool unicode)
{
    if (!unicode) {
        return index + 1;
    }
    uint32_t n = embi_js_str_len(s);
    if (index + 1 >= n) {
        return index + 1;
    }
    uint16_t c = embi_js_str_at(s, (uint32_t)index);
    if (c < 0xD800u || c > 0xDBFFu) {
        return index + 1;
    }
    uint16_t d = embi_js_str_at(s, (uint32_t)index + 1u);
    if (d < 0xDC00u || d > 0xDFFFu) {
        return index + 1;
    }
    return index + 2;
}

/* ToString(Get(R, "flags")) for the flag tests of the Symbol methods. */
static embi_js_string *flags_of(embi_js_runtime *rt, embi_js_object *r)
{
    embi_js_value f = embi_js_get(rt, r, KEY(rt, "flags"));
    if (f == EXC) {
        return nullptr;
    }
    return embi_js_to_string(rt, f);
}

/* The canonical flags string of a set of flag bits ("dgimsuvy" order). */
static embi_js_value flags_from_bits(embi_js_runtime *rt, uint32_t bits)
{
    static const char order[] = "dgimsuvy";
    static const uint32_t vals[] = {
        EMBI_RE_FLAG_D, EMBI_RE_FLAG_G, EMBI_RE_FLAG_I, EMBI_RE_FLAG_M,
        EMBI_RE_FLAG_S, EMBI_RE_FLAG_U, EMBI_RE_FLAG_V, EMBI_RE_FLAG_Y,
    };
    uint8_t buf[8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (bits & vals[i]) {
            buf[n++] = (uint8_t)order[i];
        }
    }
    return str_val(embi_js_str_new_latin1(rt, buf, n));
}

/* IsRegExp(argument): YES, NO or THREW. */
static int is_regexp_arg(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        return EMBI_JS_NO;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(v), embi_js_wk_sym(rt, EMBI_JS_WK_MATCH));
    if (m == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_undefined(m)) {
        return embi_js_to_boolean(m) ? EMBI_JS_YES : EMBI_JS_NO;
    }
    return is_rx_obj(v) ? EMBI_JS_YES : EMBI_JS_NO;
}

/* ------------------------------------------------------------------ compile and initialize */

static embi_js_value throw_bad_pattern(embi_js_runtime *rt, const embi_js_string *src,
                                       const embi_js_string *flags, const char *msg)
{
    embi_sb a = {nullptr, 0, 0};
    embi_sb b = {nullptr, 0, 0};
    embi_js_str_append_utf8(src, &a);
    embi_js_str_append_utf8(flags, &b);
    embi_js_value v = embi_js_throw_syntax_error(rt, "Invalid regular expression: /%s/%s: %s",
                                                 embi_sb_cstr(&a), embi_sb_cstr(&b), msg);
    embi_sb_free(&a);
    embi_sb_free(&b);
    return v;
}

/* RegExpInitialize(obj, pattern, flags): compiles, replaces the object's program and sets
 * lastIndex to 0. Returns false with an exception pending. */
static bool regexp_initialize(embi_js_runtime *rt, embi_js_object *o, embi_js_value pattern,
                              embi_js_value flags)
{
    embi_js_string *src = embi_js_is_undefined(pattern) ? embi_js_str_new_latin1(rt, nullptr, 0)
                                                     : embi_js_to_string(rt, pattern);
    if (!src) {
        return false;
    }
    embi_js_string *fl = embi_js_is_undefined(flags) ? embi_js_str_new_latin1(rt, nullptr, 0)
                                                     : embi_js_to_string(rt, flags);
    if (!fl) {
        return false;
    }
    units fu;
    units_of(fl, &fu);
    uint32_t bits = 0;
    bool flags_ok = embi_re_parse_flags(fu.p, fu.n, &bits);
    units_free(&fu);
    if (!flags_ok) {
        embi_js_throw_syntax_error(rt, "Invalid flags supplied to RegExp constructor");
        return false;
    }
    units pu;
    units_of(src, &pu);
    embi_re_error err = {0, nullptr};
    embi_re_prog *prog = embi_re_compile(pu.p, pu.n, bits, &err);
    if (!prog) {
        units_free(&pu);
        throw_bad_pattern(rt, src, fl, err.msg ? err.msg : "invalid pattern");
        return false;
    }
    units_free(&pu);
    rx_data *d = embi_alloc(sizeof *d);
    d->refs = 1;
    d->flags = bits;
    d->prog = prog;
    EMBI_JS_STORE(o, d->source, embi_js_from_string(src));
    if (o->internal) {
        rx_release(o->internal);
    }
    o->internal = d;
    EMBI_JS_WB_STRUCT(o);
    return set_last_index(rt, o, 0);
}

/* RegExpAlloc(newTarget): an object with the prototype of new_target and its own lastIndex. */
static embi_js_value regexp_alloc(embi_js_runtime *rt, embi_js_value new_target)
{
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_REGEXP);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, &class_regexp);
    if (new_target == rt->protos[EMBI_JS_PROTO_REGEXP_CTOR]) {
        o->hdr.flags |= EMBI_JS_OBJ_REGEXP_LEGACY;
    }
    embi_js_define_raw(rt, o, KEY(rt, "lastIndex"), embi_js_from_int32(0), ATTR_W);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ literals (J4c, parser and interpreter) */

embi_js_value embi_js_regexp_template_new(embi_js_runtime *rt, embi_re_prog *prog, uint32_t flags,
                                          const uint16_t *src, uint32_t len)
{
    embi_js_string *s = embi_js_str_new_utf16(rt, src, len);
    if (!s) {
        embi_re_free(prog);
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_regexp);
    rx_data *d = embi_alloc(sizeof *d);
    d->refs = 1;
    d->flags = flags;
    d->prog = prog;
    EMBI_JS_STORE(o, d->source, embi_js_from_string(s));
    o->internal = d;
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

embi_js_value embi_js_regexp_from_template(embi_js_runtime *rt, embi_js_value tmpl)
{
    embi_js_object *t = embi_js_as_object(tmpl);
    embi_js_object *o = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_REGEXP], &class_regexp);
    o->hdr.flags |= EMBI_JS_OBJ_REGEXP_LEGACY;
    rx_data *d = t->internal;
    d->refs++;
    o->internal = d;
    EMBI_JS_WB_STRUCT(o);
    embi_js_define_raw(rt, o, KEY(rt, "lastIndex"), embi_js_from_int32(0), ATTR_W);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ RegExp constructor */

/* The RegExp(pattern, flags) constructor (22.2.4.1). */
static embi_js_value regexp_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value pattern = arg_at(argc, argv, 0);
    embi_js_value flags = arg_at(argc, argv, 1);
    int pattern_is_re = is_regexp_arg(rt, pattern);
    if (pattern_is_re == EMBI_JS_THREW) {
        return EXC;
    }
    if (embi_js_is_undefined(new_target)) {
        new_target = rt->protos[EMBI_JS_PROTO_REGEXP_CTOR];
        if (pattern_is_re == EMBI_JS_YES && embi_js_is_undefined(flags)) {
            embi_js_value pc = embi_js_get(rt, embi_js_as_object(pattern), KEY(rt, "constructor"));
            if (pc == EXC) {
                return EXC;
            }
            if (embi_js_same_value(new_target, pc)) {
                return pattern;
            }
        }
    }
    embi_js_value p;
    embi_js_value f;
    if (is_rx_obj(pattern)) {
        rx_data *d = rx_of(pattern);
        p = d->source;
        if (embi_js_is_undefined(flags)) {
            f = flags_from_bits(rt, d->flags);
            if (f == EXC) {
                return EXC;
            }
        } else {
            f = flags;
        }
    } else if (pattern_is_re == EMBI_JS_YES) {
        p = obj_get(rt, pattern, "source");
        if (p == EXC) {
            return EXC;
        }
        if (embi_js_is_undefined(flags)) {
            f = obj_get(rt, pattern, "flags");
            if (f == EXC) {
                return EXC;
            }
        } else {
            f = flags;
        }
    } else {
        p = pattern;
        f = flags;
    }
    embi_js_value o = regexp_alloc(rt, new_target);
    if (o == EXC) {
        return EXC;
    }
    if (!regexp_initialize(rt, embi_js_as_object(o), p, f)) {
        return EXC;
    }
    return o;
}

/* RegExp.prototype.compile(pattern, flags) (Annex B.2.4.1). */
static embi_js_value proto_compile(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_rx_obj(this_v) || !(embi_js_as_object(this_v)->hdr.flags & EMBI_JS_OBJ_REGEXP_LEGACY)) {
        return embi_js_throw_type_error(rt, "RegExp.prototype.compile called on incompatible receiver");
    }
    embi_js_value p = arg_at(argc, argv, 0);
    embi_js_value f = arg_at(argc, argv, 1);
    if (is_rx_obj(p)) {
        if (!embi_js_is_undefined(f)) {
            return embi_js_throw_type_error(rt, "Cannot supply flags when constructing one RegExp from another");
        }
        rx_data *d = rx_of(p);
        f = flags_from_bits(rt, d->flags);
        if (f == EXC) {
            return EXC;
        }
        p = d->source;
    }
    if (!regexp_initialize(rt, embi_js_as_object(this_v), p, f)) {
        return EXC;
    }
    return this_v;
}

/* RegExp.escape(S) (ES2025 22.2.5.1). */
static embi_js_value regexp_escape(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value sv = arg_at(argc, argv, 0);
    if (!embi_js_is_string(sv)) {
        return embi_js_throw_type_error(rt, "RegExp.escape requires a string");
    }
    const embi_js_string *s = embi_js_str_flat(rt, sv);
    uint32_t n = embi_js_str_len(s);
    u16buf b = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = embi_js_str_at(s, i);
        uint32_t units_n = 1;
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < n) {
            uint32_t lo = embi_js_str_at(s, i + 1);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                units_n = 2;
            }
        }
        bool ascii_alnum = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
                           (cp >= 'A' && cp <= 'Z');
        if (i == 0 && ascii_alnum) {
            ub_ascii(&b, "\\x");
            ub_hex(&b, cp, 2);
        } else if (cp < 0x80u && strchr("^$\\.*+?()[]{}|/", (int)cp) != nullptr && cp != 0) {
            embi_vec_push(&b, (uint16_t)'\\');
            embi_vec_push(&b, (uint16_t)cp);
        } else if (cp == '\t' || cp == '\n' || cp == 0x0Bu || cp == '\f' || cp == '\r') {
            embi_vec_push(&b, (uint16_t)'\\');
            static const char ctl[] = "tnvfr";
            embi_vec_push(&b, (uint16_t)ctl[cp == '\t' ? 0 : cp == '\n' ? 1 : cp == 0x0Bu ? 2 : cp == '\f' ? 3 : 4]);
        } else if (cp < 0x80u && strchr(",-=<>#&!%:;@~'`\"", (int)cp) != nullptr && cp != 0) {
            ub_ascii(&b, "\\x");
            ub_hex(&b, cp, 2);
        } else if (cp == 0x20u || cp == 0xA0u || cp == 0x1680u || (cp >= 0x2000u && cp <= 0x200Au) ||
                   cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu || cp == 0x205Fu ||
                   cp == 0x3000u || cp == 0xFEFFu) {
            if (cp <= 0xFFu) {
                ub_ascii(&b, "\\x");
                ub_hex(&b, cp, 2);
            } else {
                ub_ascii(&b, "\\u");
                ub_hex(&b, cp, 4);
            }
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            /* A lone surrogate: UnicodeEscape of its code unit. */
            ub_ascii(&b, "\\u");
            ub_hex(&b, cp, 4);
        } else {
            for (uint32_t k = 0; k < units_n; k++) {
                embi_vec_push(&b, embi_js_str_at(s, i + k));
            }
        }
        i += units_n - 1;
    }
    return ub_finish(rt, &b);
}

/* ------------------------------------------------------------------ exec */

/* Results of builtin_exec_raw. */
enum { RX_NO_MATCH = 0, RX_MATCH = 1, RX_THREW = -1 };

/* The captures of one match. The engine writes them into p, which is the stack array unless the
 * pattern has more groups than fit. */
typedef struct capbuf {
    uint32_t *p;
    uint32_t cap;
    uint32_t small[32];
} capbuf;

static void capbuf_init(capbuf *c)
{
    c->p = c->small;
    c->cap = 32;
}

static void capbuf_free(capbuf *c)
{
    if (c->p != c->small) {
        embi_free(c->p, (size_t)c->cap * sizeof(uint32_t));
    }
    capbuf_init(c);
}

/* Makes room for 'slots' captures (the old contents are not kept). */
static void capbuf_reserve(capbuf *c, uint32_t slots)
{
    if (slots <= c->cap) {
        return;
    }
    uint32_t *p = embi_alloc((size_t)slots * sizeof(uint32_t));
    capbuf_free(c);
    c->p = p;
    c->cap = slots;
}

static embi_re_limits re_limits(embi_js_runtime *rt)
{
    embi_re_limits lim = {0};
    lim.interrupt = re_interrupt;
    lim.ctx = rt;
    return lim;
}

/* Throws the error of an engine status that is neither a match nor a no-match. */
static embi_js_value re_error(embi_js_runtime *rt, embi_re_status st)
{
    if (st == EMBI_RE_INTERRUPTED) {
        if (rt->vm) {
            rt->vm->terminating = true;
        }
        return embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
    }
    return embi_js_throw_range_error(rt, "regular expression too complex");
}

/* Whether the pattern has any named group. */
static bool prog_has_names(const embi_re_prog *prog)
{
    uint32_t n = embi_re_group_count(prog);
    for (uint32_t g = 1; g <= n; g++) {
        if (embi_re_group_name(prog, g, nullptr)) {
            return true;
        }
    }
    return false;
}

/* groups[name] = v for a named group. A duplicated name keeps the participating group's value
 * (an undefined value does not replace a defined one). */
static bool set_group(embi_js_runtime *rt, embi_js_object *groups, const uint16_t *nm, uint32_t nlen,
                      embi_js_value v)
{
    embi_js_string *ns = embi_js_str_new_utf16(rt, nm, nlen);
    if (!ns) {
        return false;
    }
    embi_js_key k = embi_js_key_from_str(rt, ns);
    if (embi_js_is_undefined(v) && embi_js_has_own(rt, groups, k) == EMBI_JS_YES) {
        return true;
    }
    return cdp(rt, groups, k, v);
}

/* ------------------------------------------------------------------ legacy static properties
 * (Annex B proposal: RegExp.$1-$9, input/$_, lastMatch/$&, lastParen/$+, leftContext/$`,
 * rightContext/$'). The state is rt->rx_legacy (runtime.h): src is the string of the last legacy
 * match and caps its index pairs, input is [[RegExpInput]] (assigned by RegExp.input, and set to
 * the match string by every legacy match). A regexp without legacy features (a subclass instance)
 * that matches invalidates them: state 0 and input NULL, so reads throw until input is assigned. */

/* A successful RegExpBuiltinExec of a regexp with legacy features: the legacy values follow the
 * match. Plain stores only; this is on the path of every match. */
static inline void legacy_set(embi_js_runtime *rt, embi_js_string *S, const uint32_t *caps,
                              uint32_t slots)
{
    uint32_t n = slots / 2u < 10u ? slots / 2u : 10u;
    rt->rx_legacy.state = 1;
    rt->rx_legacy.src = S;
    rt->rx_legacy.input = S;
    rt->rx_legacy.n = n;
    memcpy(rt->rx_legacy.caps, caps, 2u * n * sizeof caps[0]);
}

static inline void legacy_update(embi_js_runtime *rt, const embi_js_object *R, embi_js_string *S,
                                 const uint32_t *caps, uint32_t slots)
{
    if (!(R->hdr.flags & EMBI_JS_OBJ_REGEXP_LEGACY)) {
        rt->rx_legacy.state = 0;
        rt->rx_legacy.input = nullptr;
        return;
    }
    legacy_set(rt, S, caps, slots);
}

/* The substring [from, to) of the recorded match string, "" when from is UNDEF or the range is empty. */
static embi_js_value legacy_slice(embi_js_runtime *rt, uint32_t from, uint32_t to)
{
    if (from == EMBI_RE_UNDEF || to == EMBI_RE_UNDEF || to < from) {
        return ascii_str(rt, "");
    }
    return str_val(embi_js_str_substring(rt, rt->rx_legacy.src, from, to));
}

/* magic: 0-8 $1-$9, 9 input, 10 lastMatch, 11 lastParen, 12 leftContext, 13 rightContext. */
static embi_js_value legacy_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    if (this_v != rt->protos[EMBI_JS_PROTO_REGEXP_CTOR]) {
        return embi_js_throw_type_error(rt, "RegExp legacy static property read on a receiver other than RegExp");
    }
    if (!rt->rx_legacy.input || (magic != 9 && rt->rx_legacy.state == 0)) {
        return embi_js_throw_type_error(rt, "RegExp legacy static property is not available");
    }
    if (magic == 9) {
        return embi_js_from_string(rt->rx_legacy.input);
    }
    const uint32_t *c = rt->rx_legacy.caps;
    uint32_t n = rt->rx_legacy.n;
    if (magic < 9) {
        uint32_t g = (uint32_t)magic + 1u;
        return g < n ? legacy_slice(rt, c[2u * g], c[2u * g + 1u]) : ascii_str(rt, "");
    }
    if (n == 0) {
        return ascii_str(rt, "");
    }
    switch (magic) {
    case 10:
        return legacy_slice(rt, c[0], c[1]);
    case 11:
        return n > 1 ? legacy_slice(rt, c[2u * (n - 1u)], c[2u * (n - 1u) + 1u]) : ascii_str(rt, "");
    case 12:
        return legacy_slice(rt, 0, c[0]);
    default:
        return legacy_slice(rt, c[1], embi_js_str_len(rt->rx_legacy.src));
    }
}

static embi_js_value legacy_input_set(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (this_v != rt->protos[EMBI_JS_PROTO_REGEXP_CTOR]) {
        return embi_js_throw_type_error(rt, "RegExp legacy static property set on a receiver other than RegExp");
    }
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : UNDEF);
    if (!s) {
        return EXC;
    }
    rt->rx_legacy.input = s;
    return UNDEF;
}

static void legacy_init(embi_js_realm *r, embi_js_object *ctor)
{
    embi_js_runtime *rt = r->rt;
    if (!rt->rx_legacy.src) {
        /* One state per runtime: a later realm only installs its accessors. */
        rt->rx_legacy.state = 1;
        embi_js_value e = ascii_str(rt, "");
        rt->rx_legacy.src = embi_js_as_string(e);
        rt->rx_legacy.input = rt->rx_legacy.src;
    }
    /* An alias ($_, $&, $+, $`, $') shares the accessor functions of its long name: each function
     * costs heap that every collection marks, and nothing but identity tells them apart. */
    static const struct {
        const char *name;
        int magic;
        bool alias;
    } props[] = {
        {"$1", 0, false}, {"$2", 1, false}, {"$3", 2, false}, {"$4", 3, false}, {"$5", 4, false},
        {"$6", 5, false}, {"$7", 6, false}, {"$8", 7, false}, {"$9", 8, false},
        {"input", 9, false}, {"$_", 9, true}, {"lastMatch", 10, false}, {"$&", 10, true},
        {"lastParen", 11, false}, {"$+", 11, true}, {"leftContext", 12, false}, {"$`", 12, true},
        {"rightContext", 13, false}, {"$'", 13, true},
    };
    embi_js_obj_make_dict(rt, ctor);
    embi_js_value get = UNDEF;
    embi_js_value set = UNDEF;
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (!props[i].alias) {
            char buf[32];
            SDL_snprintf(buf, sizeof buf, "get %s", props[i].name);
            get = embi_js_from_object(embi_js_native_function_new(r, buf, 0, legacy_get, props[i].magic));
            set = UNDEF;
            if (props[i].magic == 9) {
                SDL_snprintf(buf, sizeof buf, "set %s", props[i].name);
                set = embi_js_from_object(embi_js_native_function_new(r, buf, 1, legacy_input_set, 9));
            }
        }
        embi_js_define_accessor_raw(rt, ctor, embi_js_key_c(rt, props[i].name), get, set,
                                    EMBI_JS_ATTR_CONFIGURABLE);
    }
}

/* RegExpBuiltinExec steps 1-13 (22.2.7.2) without the result object. R is a RegExp object. lastIndex
 * is read and converted here, the engine runs, and lastIndex is written as the spec says. On a match
 * the captures are in cb. Returns RX_MATCH, RX_NO_MATCH, or RX_THREW with an exception pending. */
static int builtin_exec_raw(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S, capbuf *cb)
{
    embi_js_value li = embi_js_get(rt, R, KEY(rt, "lastIndex"));
    if (li == EXC) {
        return RX_THREW;
    }
    double last_index;
    if (!embi_js_to_length(rt, li, &last_index)) {
        return RX_THREW;
    }
    /* ToLength may have run user code that recompiled R, so the program is read after it. */
    rx_data *d = R->internal;
    capbuf_reserve(cb, embi_re_capture_slots(d->prog));
    bool global = (d->flags & EMBI_RE_FLAG_G) != 0;
    bool sticky = (d->flags & EMBI_RE_FLAG_Y) != 0;
    if (!global && !sticky) {
        last_index = 0;
    }
    uint32_t len = embi_js_str_len(S);
    if (last_index > len) {
        if ((global || sticky) && !set_last_index(rt, R, 0)) {
            return RX_THREW;
        }
        return RX_NO_MATCH;
    }
    embi_re_input in;
    in.wide = embi_js_str_is_wide(S);
    in.units = in.wide ? (const void *)embi_js_str_wide(S) : (const void *)embi_js_str_latin1(S);
    in.len = len;
    embi_re_limits lim = re_limits(rt);
    embi_re_status st = sticky ? embi_re_match_at(d->prog, in, (uint32_t)last_index, cb->p, &lim)
                               : embi_re_search(d->prog, in, (uint32_t)last_index, cb->p, &lim);
    switch (st) {
    case EMBI_RE_MATCH:
        legacy_update(rt, R, S, cb->p, embi_re_capture_slots(d->prog));
        if ((global || sticky) && !set_last_index(rt, R, cb->p[1])) {
            return RX_THREW;
        }
        return RX_MATCH;
    case EMBI_RE_NO_MATCH:
        if ((global || sticky) && !set_last_index(rt, R, 0)) {
            return RX_THREW;
        }
        return RX_NO_MATCH;
    default:
        (void)re_error(rt, st);
        return RX_THREW;
    }
}

/* The groups object of a match (null prototype, one property per named group), or undefined when
 * the pattern has no named group. */
static embi_js_value groups_of(embi_js_runtime *rt, const embi_re_prog *prog, embi_js_string *S,
                               const uint32_t *caps)
{
    if (!prog_has_names(prog)) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_object *g = embi_js_object_new(rt, EMBI_JS_NULL);
    uint32_t ngroups = embi_re_group_count(prog);
    for (uint32_t i = 1; i <= ngroups; i++) {
        uint32_t nlen = 0;
        const uint16_t *nm = embi_re_group_name(prog, i, &nlen);
        if (!nm) {
            continue;
        }
        embi_js_value v = EMBI_JS_UNDEFINED;
        if (caps[2 * i] != EMBI_RE_UNDEF) {
            embi_js_string *sub = embi_js_str_substring(rt, S, caps[2 * i], caps[2 * i + 1]);
            if (!sub) {
                return EXC;
            }
            v = embi_js_from_string(sub);
        }
        if (!set_group(rt, g, nm, nlen, v)) {
            return EXC;
        }
    }
    return embi_js_from_object(g);
}

/* Appends v as element `index` of a fresh array A that is still packed (J5m2): the dense store
 * takes it when the intrinsic protectors hold, and the generic define does the rest otherwise. */
static bool append_elem(embi_js_runtime *rt, embi_js_object *A, uint32_t index, embi_js_value v)
{
    if (embi_js_array_push_fast(rt, A, v)) {
        return true;
    }
    return cdp(rt, A, embi_js_key_from_index(index), v);
}

/* The match array of a successful RegExpBuiltinExec (22.2.7.2 steps 15-34): index, input, the
 * matched text and the captures, groups, and indices when the d flag is set. J5m2: the result and
 * the groups objects are fresh, so their named properties are appended to the cached shape
 * transitions (embi_js_define_fresh) and the elements are pushed onto the dense store. */
static embi_js_value build_match(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S,
                                 const uint32_t *caps)
{
    rx_data *d = R->internal;
    uint32_t ngroups = embi_re_group_count(d->prog);
    bool has_names = prog_has_names(d->prog);
    embi_js_object *A = embi_js_array_create_default(rt, 0);
    if (!A) {
        return EXC;
    }
    /* A GC is possible from here on: A, S and R are locals, and d stays valid because R holds it. */
    embi_js_define_fresh(rt, A, embi_js_name_val(rt, EMBI_JS_NAME_INDEX),
                         embi_js_number((double)caps[0]), EMBI_JS_ATTR_DEFAULT);
    embi_js_define_fresh(rt, A, embi_js_name_val(rt, EMBI_JS_NAME_INPUT), embi_js_from_string(S),
                         EMBI_JS_ATTR_DEFAULT);
    embi_js_string *m0 = embi_js_str_substring(rt, S, caps[0], caps[1]);
    if (!m0 || !append_elem(rt, A, 0, embi_js_from_string(m0))) {
        return EXC;
    }
    embi_js_value groups = EMBI_JS_UNDEFINED;
    if (has_names) {
        groups = embi_js_from_object(embi_js_object_new(rt, EMBI_JS_NULL));
    }
    embi_js_define_fresh(rt, A, embi_js_name_val(rt, EMBI_JS_NAME_GROUPS), groups,
                         EMBI_JS_ATTR_DEFAULT);
    for (uint32_t g = 1; g <= ngroups; g++) {
        embi_js_value v = EMBI_JS_UNDEFINED;
        if (caps[2 * g] != EMBI_RE_UNDEF) {
            embi_js_string *sub = embi_js_str_substring(rt, S, caps[2 * g], caps[2 * g + 1]);
            if (!sub) {
                return EXC;
            }
            v = embi_js_from_string(sub);
        }
        if (!append_elem(rt, A, g, v)) {
            return EXC;
        }
        uint32_t nlen = 0;
        const uint16_t *nm = embi_re_group_name(d->prog, g, &nlen);
        if (nm && !set_group(rt, embi_js_as_object(groups), nm, nlen, v)) {
            return EXC;
        }
    }
    if (d->flags & EMBI_RE_FLAG_D) {
        embi_js_object *IA = embi_js_array_create_default(rt, 0);
        if (!IA) {
            return EXC;
        }
        embi_js_value igroups = EMBI_JS_UNDEFINED;
        if (has_names) {
            igroups = embi_js_from_object(embi_js_object_new(rt, EMBI_JS_NULL));
        }
        embi_js_define_fresh(rt, IA, embi_js_name_val(rt, EMBI_JS_NAME_GROUPS), igroups,
                             EMBI_JS_ATTR_DEFAULT);
        for (uint32_t g = 0; g <= ngroups; g++) {
            embi_js_value pair = EMBI_JS_UNDEFINED;
            if (caps[2 * g] != EMBI_RE_UNDEF) {
                embi_js_object *pa = embi_js_array_create_default(rt, 0);
                if (!pa || !append_elem(rt, pa, 0, embi_js_number((double)caps[2 * g])) ||
                    !append_elem(rt, pa, 1, embi_js_number((double)caps[2 * g + 1]))) {
                    return EXC;
                }
                pair = embi_js_from_object(pa);
            }
            if (!append_elem(rt, IA, g, pair)) {
                return EXC;
            }
            uint32_t nlen = 0;
            const uint16_t *nm = g == 0 ? nullptr : embi_re_group_name(d->prog, g, &nlen);
            if (nm && !set_group(rt, embi_js_as_object(igroups), nm, nlen, pair)) {
                return EXC;
            }
        }
        embi_js_define_fresh(rt, A, embi_js_name_val(rt, EMBI_JS_NAME_INDICES),
                             embi_js_from_object(IA), EMBI_JS_ATTR_DEFAULT);
    }
    return embi_js_from_object(A);
}

/* RegExpBuiltinExec(R, S) (22.2.7.2): the match array, or null. */
static embi_js_value builtin_exec(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S)
{
    capbuf cb;
    capbuf_init(&cb);
    int m = builtin_exec_raw(rt, R, S, &cb);
    embi_js_value out = EXC;
    if (m == RX_MATCH) {
        out = build_match(rt, R, S, cb.p);
    } else if (m == RX_NO_MATCH) {
        out = EMBI_JS_NULL;
    }
    capbuf_free(&cb);
    return out;
}


/* RegExpExec(R, S) (22.2.7.1). R is an object, S a string. */
static embi_js_value regexp_exec(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S)
{
    embi_js_value exec = embi_js_get(rt, R, KEY(rt, "exec"));
    if (exec == EXC) {
        return EXC;
    }
    if (exec == rt->protos[EMBI_JS_PROTO_REGEXP_EXEC] && is_rx_obj(embi_js_from_object(R))) {
        return builtin_exec(rt, R, S);
    }
    if (embi_js_is_callable(exec)) {
        embi_js_value sv = embi_js_from_string(S);
        embi_js_value res = embi_js_call(rt, exec, embi_js_from_object(R), 1, &sv);
        if (res == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(res) && !embi_js_is_null(res)) {
            return embi_js_throw_type_error(rt, "RegExp exec method returned something other than an Object or null");
        }
        return res;
    }
    if (!is_rx_obj(embi_js_from_object(R))) {
        return embi_js_throw_type_error(rt, "RegExp.prototype.exec called on incompatible receiver");
    }
    return builtin_exec(rt, R, S);
}

static embi_js_value check_rx_receiver(embi_js_runtime *rt, embi_js_value this_v, const char *name)
{
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "%s called on non-object", name);
    }
    return this_v;
}

/* RegExp.prototype.exec(string). */
static embi_js_value proto_exec(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_rx_obj(this_v)) {
        return embi_js_throw_type_error(rt, "RegExp.prototype.exec called on incompatible receiver");
    }
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    return builtin_exec(rt, embi_js_as_object(this_v), S);
}

/* ------------------------------------------------------------------ source, flags, toString */

/* EscapeRegExpPattern: "/" and line terminators outside a class, and the line terminators inside
 * one, are escaped; an empty pattern reads as "(?:)". */
static embi_js_value escape_source(embi_js_runtime *rt, embi_js_value src)
{
    embi_js_string *s = embi_js_str_flat(rt, src);
    uint32_t n = embi_js_str_len(s);
    if (n == 0) {
        return ascii_str(rt, "(?:)");
    }
    u16buf b = {nullptr, 0, 0};
    bool in_class = false;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        if (c == '\\' && i + 1 < n) {
            embi_vec_push(&b, c);
            i++;
            c = embi_js_str_at(s, i);
            if (c == '\n') {
                ub_ascii(&b, "n");
            } else if (c == '\r') {
                ub_ascii(&b, "r");
            } else if (c == 0x2028u) {
                ub_ascii(&b, "u2028");
            } else if (c == 0x2029u) {
                ub_ascii(&b, "u2029");
            } else {
                embi_vec_push(&b, c);
            }
            continue;
        }
        if (c == '[') {
            in_class = true;
        } else if (c == ']') {
            in_class = false;
        }
        if (c == '/' && !in_class) {
            ub_ascii(&b, "\\/");
        } else if (c == '\n') {
            ub_ascii(&b, "\\n");
        } else if (c == '\r') {
            ub_ascii(&b, "\\r");
        } else if (c == 0x2028u) {
            ub_ascii(&b, "\\u2028");
        } else if (c == 0x2029u) {
            ub_ascii(&b, "\\u2029");
        } else {
            embi_vec_push(&b, c);
        }
    }
    return ub_finish(rt, &b);
}

struct flag_prop {
    const char *name;
    const char *getter;
    uint32_t bit;
    char ch;
};

/* In the order of the flags getter (hasIndices, global, ignoreCase, multiline, dotAll, unicode,
 * unicodeSets, sticky), which is the order of the letters "dgimsuvy". */
static const struct flag_prop flag_props[] = {
    {"hasIndices", "get hasIndices", EMBI_RE_FLAG_D, 'd'},
    {"global", "get global", EMBI_RE_FLAG_G, 'g'},
    {"ignoreCase", "get ignoreCase", EMBI_RE_FLAG_I, 'i'},
    {"multiline", "get multiline", EMBI_RE_FLAG_M, 'm'},
    {"dotAll", "get dotAll", EMBI_RE_FLAG_S, 's'},
    {"unicode", "get unicode", EMBI_RE_FLAG_U, 'u'},
    {"unicodeSets", "get unicodeSets", EMBI_RE_FLAG_V, 'v'},
    {"sticky", "get sticky", EMBI_RE_FLAG_Y, 'y'},
};

/* The flag getters (hasIndices ... sticky): magic indexes flag_props. */
static embi_js_value flag_getter(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    const struct flag_prop *fp = &flag_props[magic];
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "%s called on non-object", fp->getter);
    }
    if (!is_rx_obj(this_v)) {
        if (this_v == rt->protos[EMBI_JS_PROTO_REGEXP]) {
            return UNDEF;
        }
        return embi_js_throw_type_error(rt, "%s called on incompatible receiver", fp->getter);
    }
    return embi_js_from_bool((rx_of(this_v)->flags & fp->bit) != 0);
}

/* get RegExp.prototype.flags: the letters of the flags whose getters answer true. */
static embi_js_value proto_flags_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "RegExp.prototype.flags getter called on non-object");
    }
    u16buf b = {nullptr, 0, 0};
    for (size_t i = 0; i < sizeof flag_props / sizeof flag_props[0]; i++) {
        embi_js_value v = obj_get(rt, this_v, flag_props[i].name);
        if (v == EXC) {
            embi_vec_free(&b);
            return EXC;
        }
        if (embi_js_to_boolean(v)) {
            embi_vec_push(&b, (uint16_t)flag_props[i].ch);
        }
    }
    return ub_finish(rt, &b);
}

/* get RegExp.prototype.source. */
static embi_js_value proto_source_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "RegExp.prototype.source getter called on non-object");
    }
    if (!is_rx_obj(this_v)) {
        if (this_v == rt->protos[EMBI_JS_PROTO_REGEXP]) {
            return ascii_str(rt, "(?:)");
        }
        return embi_js_throw_type_error(rt, "RegExp.prototype.source getter called on incompatible receiver");
    }
    return escape_source(rt, rx_of(this_v)->source);
}

/* RegExp.prototype.toString(). */
static embi_js_value proto_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype.toString") == EXC) {
        return EXC;
    }
    embi_js_string *P = embi_js_to_string(rt, obj_get(rt, this_v, "source"));
    if (!P) {
        return EXC;
    }
    embi_js_string *F = embi_js_to_string(rt, obj_get(rt, this_v, "flags"));
    if (!F) {
        return EXC;
    }
    u16buf b = {nullptr, 0, 0};
    embi_vec_push(&b, (uint16_t)'/');
    ub_str(&b, P, 0, embi_js_str_len(P));
    embi_vec_push(&b, (uint16_t)'/');
    ub_str(&b, F, 0, embi_js_str_len(F));
    return ub_finish(rt, &b);
}

/* ------------------------------------------------------------------ pristine RegExp objects (fast paths) */

/* The fast paths of test, @@match, @@replace and @@split skip the object-level steps of the spec
 * (the result arrays, the property reads and the splitter object), so they run only when none of the
 * skipped steps could run user code or be observed. A RegExp object R is pristine when:
 *   - its only own property is lastIndex, and lastIndex holds a primitive (ToLength runs no user code);
 *   - its prototype is %RegExp.prototype%;
 *   - the prototype's exec is the intrinsic function (and, for the Symbol methods that read flags,
 *     the flags getter and the eight flag getters are the intrinsics, and @@match is the intrinsic
 *     that IsRegExp reads).
 * An intrinsic native function is recognized by its implementation and magic, so no table of the
 * intrinsic values is kept. Reads of lastIndex and of the prototype's data are pure. */

static bool is_intrinsic_fn(embi_js_value f, embi_js_native fn, int magic)
{
    if (!embi_js_is_object(f)) {
        return false;
    }
    embi_js_object *o = embi_js_as_object(f);
    if (o->klass != &embi_js_class_native || o->internal == nullptr) {
        return false;
    }
    const embi_js_native_data *nd = o->internal;
    return nd->fn == fn && nd->magic == magic;
}

/* The own property 'key' of %RegExp.prototype%. */
static bool proto_own(embi_js_runtime *rt, embi_js_key key, embi_js_pdesc *d)
{
    embi_js_object *proto = embi_js_as_object(rt->protos[EMBI_JS_PROTO_REGEXP]);
    return embi_js_obj_get_own_property(rt, proto, key, d) == EMBI_JS_YES;
}

/* True when %RegExp.prototype%[key] is an accessor whose getter is the intrinsic (accessor true), or a
 * data property holding the intrinsic function (accessor false). */
static bool proto_intrinsic(embi_js_runtime *rt, embi_js_key key, bool accessor, embi_js_native fn,
                            int magic)
{
    embi_js_pdesc d;
    if (!proto_own(rt, key, &d)) {
        return false;
    }
    if (((d.has & EMBI_JS_PD_GET) != 0) != accessor) {
        return false;
    }
    return is_intrinsic_fn(accessor ? d.get : d.value, fn, magic);
}

/* RegExp.prototype[@@match], declared here because the pristine check names it. */
static embi_js_value proto_match(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic);

/* Only lastIndex is an own property, its value is a primitive, and the prototype is intrinsic. */
static bool rx_plain(embi_js_runtime *rt, embi_js_object *R)
{
    if (!is_rx_obj(embi_js_from_object(R)) || embi_js_obj_dict(R) != nullptr || embi_js_obj_sparse(R) != nullptr ||
        embi_js_obj_elems_len(R) != 0 || R->shape->count != 1 ||
        embi_js_obj_proto(R) != rt->protos[EMBI_JS_PROTO_REGEXP]) {
        return false;
    }
    embi_js_pdesc d;
    if (embi_js_obj_get_own_property(rt, R, KEY(rt, "lastIndex"), &d) != EMBI_JS_YES) {
        return false;
    }
    return !embi_js_is_object(d.value);
}

/* The exec step of RegExpExec is the intrinsic, so RegExpBuiltinExec runs without user code. */
static bool rx_exec_pristine(embi_js_runtime *rt, embi_js_object *R)
{
    if (!rx_plain(rt, R)) {
        return false;
    }
    embi_js_pdesc d;
    return proto_own(rt, KEY(rt, "exec"), &d) && (d.has & EMBI_JS_PD_GET) == 0 &&
           d.value == rt->protos[EMBI_JS_PROTO_REGEXP_EXEC];
}

/* Everything rx_exec_pristine checks, plus the flag reads of the Symbol methods. */
static bool rx_pristine(embi_js_runtime *rt, embi_js_object *R)
{
    if (!rx_exec_pristine(rt, R) || !proto_intrinsic(rt, KEY(rt, "flags"), true, proto_flags_get, 0) ||
        !proto_intrinsic(rt, embi_js_wk_sym(rt, EMBI_JS_WK_MATCH), false, proto_match, 0)) {
        return false;
    }
    for (size_t i = 0; i < sizeof flag_props / sizeof flag_props[0]; i++) {
        if (!proto_intrinsic(rt, KEY(rt, flag_props[i].name), true, flag_getter, (int)i)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ test and @@match */

/* RegExp.prototype.test(string) (22.2.6.16). */
static embi_js_value proto_test(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype.test") == EXC) {
        return EXC;
    }
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    if (rx_exec_pristine(rt, R)) {
        capbuf cb;
        capbuf_init(&cb);
        int m = builtin_exec_raw(rt, R, S, &cb);
        capbuf_free(&cb);
        if (m == RX_THREW) {
            return EXC;
        }
        return embi_js_from_bool(m == RX_MATCH);
    }
    embi_js_value res = regexp_exec(rt, R, S);
    if (res == EXC) {
        return EXC;
    }
    return embi_js_from_bool(!embi_js_is_null(res));
}

/* The global case of @@match for a pristine R: the matched substrings, without match arrays. The
 * lastIndex writes are the ones the spec makes after each match. */
static embi_js_value match_global_fast(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S)
{
    bool full_unicode = (rx_of(embi_js_from_object(R))->flags & (EMBI_RE_FLAG_U | EMBI_RE_FLAG_V)) != 0;
    if (!set_last_index(rt, R, 0)) {
        return EXC;
    }
    embi_js_object *A = embi_js_array_create_default(rt, 0);
    if (!A) {
        return EXC;
    }
    capbuf cb;
    capbuf_init(&cb);
    embi_js_value result = EXC;
    uint32_t n = 0;
    for (;;) {
        int m = builtin_exec_raw(rt, R, S, &cb);
        if (m == RX_THREW) {
            break;
        }
        if (m == RX_NO_MATCH) {
            result = n == 0 ? EMBI_JS_NULL : embi_js_from_object(A);
            break;
        }
        embi_js_string *ms = embi_js_str_substring(rt, S, cb.p[0], cb.p[1]);
        if (!ms || !cdp(rt, A, embi_js_key_from_index(n), embi_js_from_string(ms))) {
            break;
        }
        n++;
        if (cb.p[0] == cb.p[1] &&
            !set_last_index(rt, R, advance_index(S, (double)cb.p[1], full_unicode))) {
            break;
        }
    }
    capbuf_free(&cb);
    return result;
}

/* RegExp.prototype[@@match](string) (22.2.6.8). */
static embi_js_value proto_match(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype[Symbol.match]") == EXC) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    if (rx_pristine(rt, R)) {
        if (!(rx_of(this_v)->flags & EMBI_RE_FLAG_G)) {
            return builtin_exec(rt, R, S);
        }
        return match_global_fast(rt, R, S);
    }
    embi_js_string *flags = flags_of(rt, R);
    if (!flags) {
        return EXC;
    }
    if (!has_char(flags, 'g')) {
        return regexp_exec(rt, R, S);
    }
    bool full_unicode = has_char(flags, 'u') || has_char(flags, 'v');
    if (!set_last_index(rt, R, 0)) {
        return EXC;
    }
    embi_js_object *A = embi_js_array_create_default(rt, 0);
    if (!A) {
        return EXC;
    }
    uint32_t n = 0;
    for (;;) {
        embi_js_value res = regexp_exec(rt, R, S);
        if (res == EXC) {
            return EXC;
        }
        if (embi_js_is_null(res)) {
            return n == 0 ? EMBI_JS_NULL : embi_js_from_object(A);
        }
        embi_js_string *ms = embi_js_to_string(rt, obj_get(rt, res, "0"));
        if (!ms) {
            return EXC;
        }
        if (!cdp(rt, A, embi_js_key_from_index(n), embi_js_from_string(ms))) {
            return EXC;
        }
        n++;
        if (embi_js_str_len(ms) == 0) {
            embi_js_value li = embi_js_get(rt, R, KEY(rt, "lastIndex"));
            double this_index;
            if (li == EXC || !embi_js_to_length(rt, li, &this_index)) {
                return EXC;
            }
            if (!set_last_index(rt, R, advance_index(S, this_index, full_unicode))) {
                return EXC;
            }
        }
    }
}

/* ------------------------------------------------------------------ Symbol.matchAll and its iterator */

/* RegExp.prototype[@@matchAll](string). */
static embi_js_value proto_match_all(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype[Symbol.matchAll]") == EXC) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    embi_js_value C = embi_js_species_constructor(rt, R, rt->protos[EMBI_JS_PROTO_REGEXP_CTOR]);
    if (C == EXC) {
        return EXC;
    }
    embi_js_string *flags = flags_of(rt, R);
    if (!flags) {
        return EXC;
    }
    embi_js_value args[2] = {this_v, embi_js_from_string(flags)};
    embi_js_value matcher = embi_js_construct(rt, C, 2, args, C);
    if (matcher == EXC) {
        return EXC;
    }
    embi_js_value li = embi_js_get(rt, R, KEY(rt, "lastIndex"));
    double last_index;
    if (li == EXC || !embi_js_to_length(rt, li, &last_index)) {
        return EXC;
    }
    if (!set_last_index(rt, embi_js_as_object(matcher), last_index)) {
        return EXC;
    }
    embi_js_varray *st = embi_js_varray_new(rt, RI_N);
    EMBI_JS_STORE(st, st->elems[RI_R], matcher);
    EMBI_JS_STORE(st, st->elems[RI_S], embi_js_from_string(S));
    EMBI_JS_STORE(st, st->elems[RI_GLOBAL], embi_js_from_bool(has_char(flags, 'g')));
    EMBI_JS_STORE(st, st->elems[RI_UNICODE], embi_js_from_bool(has_char(flags, 'u') || has_char(flags, 'v')));
    EMBI_JS_STORE(st, st->elems[RI_DONE], EMBI_JS_FALSE);
    embi_js_object *it = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_REGEXP_STRING_ITER],
                                                  &class_rx_iter);
    it->internal = st;
    EMBI_JS_WB_STRUCT(it);
    return embi_js_from_object(it);
}

/* %RegExpStringIteratorPrototype%.next(). */
static embi_js_value rx_iter_next(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v) || embi_js_as_object(this_v)->klass != &class_rx_iter) {
        return embi_js_throw_type_error(rt, "%%RegExpStringIterator%%.next called on incompatible receiver");
    }
    embi_js_varray *st = embi_js_as_object(this_v)->internal;
    if (embi_js_as_bool(st->elems[RI_DONE])) {
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    embi_js_object *R = embi_js_as_object(st->elems[RI_R]);
    embi_js_string *S = embi_js_str_flat(rt, st->elems[RI_S]);
    embi_js_value m = regexp_exec(rt, R, S);
    if (m == EXC) {
        return EXC;
    }
    if (embi_js_is_null(m)) {
        EMBI_JS_STORE(st, st->elems[RI_DONE], EMBI_JS_TRUE);
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    if (!embi_js_as_bool(st->elems[RI_GLOBAL])) {
        EMBI_JS_STORE(st, st->elems[RI_DONE], EMBI_JS_TRUE);
        return embi_js_iter_result_new(rt, m, false);
    }
    embi_js_string *ms = embi_js_to_string(rt, obj_get(rt, m, "0"));
    if (!ms) {
        return EXC;
    }
    if (embi_js_str_len(ms) == 0) {
        embi_js_value li = embi_js_get(rt, R, KEY(rt, "lastIndex"));
        double this_index;
        if (li == EXC || !embi_js_to_length(rt, li, &this_index)) {
            return EXC;
        }
        if (!set_last_index(rt, R, advance_index(S, this_index, embi_js_as_bool(st->elems[RI_UNICODE])))) {
            return EXC;
        }
    }
    return embi_js_iter_result_new(rt, m, false);
}

/* ------------------------------------------------------------------ Symbol.replace */

/* The captures of one match for GetSubstitution. A fast-path match is the engine's captures over the
 * subject; a generic match is its matched string and a list of groups 1..n (undefined or strings). */
typedef struct subst_caps {
    const embi_js_string *matched;  /* generic path: the matched text; nullptr on the fast path */
    const embi_js_string *S;        /* fast path: the subject */
    const uint32_t *caps;           /* fast path: the engine's captures; nullptr on the generic path */
    embi_js_varray *list;           /* generic path: groups 1..n */
    uint32_t n;                     /* number of groups */
} subst_caps;

/* A growable list of values, kept in a varray (which the collector traces). */
typedef struct vlist {
    embi_js_varray *a;
    uint32_t n;
    uint32_t cap;
} vlist;

static void vlist_push(embi_js_runtime *rt, vlist *l, embi_js_value v)
{
    if (l->n == l->cap) {
        uint32_t ncap = l->cap == 0 ? 8 : l->cap * 2;
        l->a = l->a ? embi_js_varray_grow(rt, l->a, ncap) : embi_js_varray_new(rt, ncap);
        l->cap = ncap;
    }
    EMBI_JS_STORE(l->a, l->a->elems[l->n++], v);
}

static uint32_t matched_len(const subst_caps *sc)
{
    return sc->matched ? embi_js_str_len(sc->matched) : sc->caps[1] - sc->caps[0];
}

static void append_matched(u16buf *out, const subst_caps *sc)
{
    if (sc->matched) {
        ub_str(out, sc->matched, 0, embi_js_str_len(sc->matched));
    } else {
        ub_str(out, sc->S, sc->caps[0], sc->caps[1]);
    }
}

/* Appends group i (1-based, i <= sc->n): nothing when it did not participate. */
static void append_group(u16buf *out, const subst_caps *sc, uint32_t i)
{
    if (sc->caps) {
        if (sc->caps[2 * i] != EMBI_RE_UNDEF) {
            ub_str(out, sc->S, sc->caps[2 * i], sc->caps[2 * i + 1]);
        }
        return;
    }
    embi_js_value cv = sc->list->elems[i - 1];
    if (embi_js_is_string(cv)) {
        ub_str(out, embi_js_as_string(cv), 0, embi_js_str_len(embi_js_as_string(cv)));
    }
}

/* GetSubstitution (22.1.3.19.1), appending the expansion of repl to out. str is the subject, and
 * position the match's start. named is undefined or an object. */
static bool get_substitution(embi_js_runtime *rt, const embi_js_string *str, uint32_t position,
                             const subst_caps *sc, embi_js_value named, const embi_js_string *repl,
                             u16buf *out)
{
    uint32_t rl = embi_js_str_len(repl);
    uint32_t sl = embi_js_str_len(str);
    uint32_t tail = position + matched_len(sc);
    if (tail > sl) {
        tail = sl;
    }
    uint32_t i = 0;
    while (i < rl) {
        uint16_t c = embi_js_str_at(repl, i);
        if (c != '$' || i + 1 >= rl) {
            embi_vec_push(out, c);
            i++;
            continue;
        }
        uint16_t c2 = embi_js_str_at(repl, i + 1);
        if (c2 == '$') {
            embi_vec_push(out, '$');
            i += 2;
        } else if (c2 == '&') {
            append_matched(out, sc);
            i += 2;
        } else if (c2 == '`') {
            ub_str(out, str, 0, position);
            i += 2;
        } else if (c2 == '\'') {
            if (tail < sl) {
                ub_str(out, str, tail, sl);
            }
            i += 2;
        } else if (c2 >= '0' && c2 <= '9') {
            uint32_t idx = c2 - '0';
            uint32_t dig = 1;
            if (i + 2 < rl) {
                uint16_t c3 = embi_js_str_at(repl, i + 2);
                if (c3 >= '0' && c3 <= '9') {
                    uint32_t two = idx * 10u + (c3 - '0');
                    if (two >= 1 && two <= sc->n) {
                        idx = two;
                        dig = 2;
                    }
                }
            }
            if (idx >= 1 && idx <= sc->n) {
                append_group(out, sc, idx);
                i += 1 + dig;
            } else {
                embi_vec_push(out, '$');
                i++;
            }
        } else if (c2 == '<') {
            if (embi_js_is_undefined(named)) {
                embi_vec_push(out, '$');
                embi_vec_push(out, '<');
                i += 2;
            } else {
                uint32_t gt = i + 2;
                while (gt < rl && embi_js_str_at(repl, gt) != '>') {
                    gt++;
                }
                if (gt >= rl) {
                    embi_vec_push(out, '$');
                    embi_vec_push(out, '<');
                    i += 2;
                } else {
                    embi_js_string *gname = embi_js_str_substring(rt, repl, i + 2, gt);
                    if (!gname) {
                        return false;
                    }
                    embi_js_value cv = embi_js_get(rt, embi_js_as_object(named),
                                                   embi_js_key_from_str(rt, gname));
                    if (cv == EXC) {
                        return false;
                    }
                    if (!embi_js_is_undefined(cv)) {
                        embi_js_string *cs = embi_js_to_string(rt, cv);
                        if (!cs) {
                            return false;
                        }
                        ub_str(out, cs, 0, embi_js_str_len(cs));
                    }
                    i = gt + 1;
                }
            }
        } else {
            embi_vec_push(out, '$');
            i++;
        }
    }
    return true;
}

/* The replacement of a pristine R (@@replace): the engine finds the matches, and the substitutions
 * are built from its captures, with no result arrays. A functional replacer runs after all matches
 * are found (as in the spec), with the captures of each match in turn. rv is the replacement
 * function or string (already converted). */
static embi_js_value replace_fast(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S,
                                  embi_js_value rv, bool functional)
{
    /* The replacer can run user code, which may recompile R: hold the program across it. */
    rx_data *d = rx_of(embi_js_from_object(R));
    d->refs++;
    bool global = (d->flags & EMBI_RE_FLAG_G) != 0;
    bool full_unicode = (d->flags & (EMBI_RE_FLAG_U | EMBI_RE_FLAG_V)) != 0;
    bool has_names = prog_has_names(d->prog);
    uint32_t ng = embi_re_group_count(d->prog);
    uint32_t nslots = embi_re_capture_slots(d->prog);
    uint32_t sl = embi_js_str_len(S);
    embi_js_string *repl = functional ? nullptr : embi_js_str_flat(rt, rv);
    bool groups_in_repl = !functional && has_names && has_char(repl, '<');
    capbuf cb;
    capbuf_init(&cb);
    u32buf recs = {nullptr, 0, 0};
    u16buf acc = {nullptr, 0, 0};
    embi_js_value out = EXC;
    uint32_t next_pos = 0;
    uint32_t nrec = 0;
    if (global && !set_last_index(rt, R, 0)) {
        goto done;
    }
    for (;;) {
        int m = builtin_exec_raw(rt, R, S, &cb);
        if (m == RX_THREW) {
            goto done;
        }
        if (m == RX_NO_MATCH) {
            break;
        }
        const uint32_t *c = cb.p;
        if (functional) {
            for (uint32_t k = 0; k < nslots; k++) {
                embi_vec_push(&recs, c[k]);
            }
            nrec++;
        } else if (c[0] >= next_pos) {
            ub_str(&acc, S, next_pos, c[0]);
            embi_js_value named = EMBI_JS_UNDEFINED;
            if (groups_in_repl) {
                named = groups_of(rt, d->prog, S, c);
                if (named == EXC) {
                    goto done;
                }
            }
            subst_caps sc = {nullptr, S, c, nullptr, ng};
            if (!get_substitution(rt, S, c[0], &sc, named, repl, &acc)) {
                goto done;
            }
            next_pos = c[1];
        }
        if (!global) {
            break;
        }
        if (c[0] == c[1] &&
            !set_last_index(rt, R, advance_index(S, (double)c[1], full_unicode))) {
            goto done;
        }
    }
    for (uint32_t r = 0; functional && r < nrec; r++) {
        const uint32_t *c = recs.data + (size_t)r * nslots;
        embi_js_string *ms = embi_js_str_substring(rt, S, c[0], c[1]);
        if (!ms) {
            goto done;
        }
        uint32_t nargs = ng + 3 + (has_names ? 1 : 0);
        embi_js_varray *av = embi_js_varray_new(rt, nargs);
        uint32_t ai = 0;
        EMBI_JS_STORE(av, av->elems[ai++], embi_js_from_string(ms));
        for (uint32_t g = 1; g <= ng; g++) {
            embi_js_value cv = EMBI_JS_UNDEFINED;
            if (c[2 * g] != EMBI_RE_UNDEF) {
                embi_js_string *cs = embi_js_str_substring(rt, S, c[2 * g], c[2 * g + 1]);
                if (!cs) {
                    goto done;
                }
                cv = embi_js_from_string(cs);
            }
            EMBI_JS_STORE(av, av->elems[ai++], cv);
        }
        EMBI_JS_STORE(av, av->elems[ai++], embi_js_number((double)c[0]));
        EMBI_JS_STORE(av, av->elems[ai++], embi_js_from_string(S));
        if (has_names) {
            embi_js_value groups = groups_of(rt, d->prog, S, c);
            if (groups == EXC) {
                goto done;
            }
            EMBI_JS_STORE(av, av->elems[ai++], groups);
        }
        embi_js_value repv = embi_js_call(rt, rv, EMBI_JS_UNDEFINED, (int)ai, av->elems);
        EMBI_KEEP_ALIVE(av); /* argv points into av */
        if (repv == EXC) {
            goto done;
        }
        embi_js_string *rs = embi_js_to_string(rt, repv);
        if (!rs) {
            goto done;
        }
        if (c[0] >= next_pos) {
            ub_str(&acc, S, next_pos, c[0]);
            ub_str(&acc, rs, 0, embi_js_str_len(rs));
            next_pos = c[1];
        }
    }
    if (next_pos < sl) {
        ub_str(&acc, S, next_pos, sl);
    }
    out = str_val(embi_js_str_new_utf16(rt, acc.data, acc.len));
done:
    embi_vec_free(&acc);
    embi_vec_free(&recs);
    capbuf_free(&cb);
    rx_release(d);
    return out;
}

/* RegExp.prototype[@@replace](string, replaceValue) (22.2.6.11). */
static embi_js_value proto_replace(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype[Symbol.replace]") == EXC) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    uint32_t sl = embi_js_str_len(S);
    embi_js_value rv = arg_at(argc, argv, 1);
    bool functional = embi_js_is_callable(rv);
    if (!functional) {
        embi_js_string *rs = embi_js_to_string(rt, rv);
        if (!rs) {
            return EXC;
        }
        rv = embi_js_from_string(rs);
    }
    if (rx_pristine(rt, R)) {
        return replace_fast(rt, R, S, rv, functional);
    }
    embi_js_string *flags = flags_of(rt, R);
    if (!flags) {
        return EXC;
    }
    bool global = has_char(flags, 'g');
    bool full_unicode = has_char(flags, 'u') || has_char(flags, 'v');
    if (global && !set_last_index(rt, R, 0)) {
        return EXC;
    }
    embi_js_object *results = embi_js_array_create_default(rt, 0);
    if (!results) {
        return EXC;
    }
    uint32_t nres = 0;
    for (;;) {
        embi_js_value res = regexp_exec(rt, R, S);
        if (res == EXC) {
            return EXC;
        }
        if (embi_js_is_null(res)) {
            break;
        }
        if (!cdp(rt, results, embi_js_key_from_index(nres), res)) {
            return EXC;
        }
        nres++;
        if (!global) {
            break;
        }
        embi_js_string *ms = embi_js_to_string(rt, obj_get(rt, res, "0"));
        if (!ms) {
            return EXC;
        }
        if (embi_js_str_len(ms) == 0) {
            embi_js_value li = embi_js_get(rt, R, KEY(rt, "lastIndex"));
            double this_index;
            if (li == EXC || !embi_js_to_length(rt, li, &this_index)) {
                return EXC;
            }
            if (!set_last_index(rt, R, advance_index(S, this_index, full_unicode))) {
                return EXC;
            }
        }
    }

    u16buf acc = {nullptr, 0, 0};
    u16buf rep = {nullptr, 0, 0};
    uint32_t next_pos = 0;
    embi_js_value out = EXC;
    for (uint32_t k = 0; k < nres; k++) {
        embi_js_value resv = embi_js_get(rt, results, embi_js_key_from_index(k));
        if (resv == EXC) {
            goto done;
        }
        embi_js_object *res = embi_js_as_object(resv);
        double rlen;
        if (!embi_js_length_of_array_like(rt, res, &rlen)) {
            goto done;
        }
        /* nCaptures comes from a script-controlled length: it is bounded, and the captures are
         * collected one read at a time. */
        uint64_t ncap64 = rlen > 1 ? (uint64_t)(rlen - 1) : 0;
        if (ncap64 > RX_MAX_CAPTURES) {
            (void)embi_js_throw_range_error(rt, "too many captures in a match result");
            goto done;
        }
        uint32_t ncaps = (uint32_t)ncap64;
        embi_js_value mv = obj_get(rt, resv, "0");
        if (mv == EXC) {
            goto done;
        }
        embi_js_string *matched = embi_js_to_string(rt, mv);
        if (!matched) {
            goto done;
        }
        uint32_t mlen = embi_js_str_len(matched);
        embi_js_value idx = obj_get(rt, resv, "index");
        if (idx == EXC) {
            goto done;
        }
        double pos;
        if (!embi_js_to_integer_or_infinity(rt, idx, &pos)) {
            goto done;
        }
        if (pos < 0) {
            pos = 0;
        }
        if (pos > sl) {
            pos = sl;
        }
        uint32_t position = (uint32_t)pos;
        vlist capl = {nullptr, 0, 0};
        for (uint32_t n = 1; n <= ncaps; n++) {
            embi_js_value cv = embi_js_get(rt, res, embi_js_key_from_index(n));
            if (cv == EXC) {
                goto done;
            }
            if (!embi_js_is_undefined(cv)) {
                embi_js_string *cs = embi_js_to_string(rt, cv);
                if (!cs) {
                    goto done;
                }
                cv = embi_js_from_string(cs);
            }
            vlist_push(rt, &capl, cv);
        }
        embi_js_value named = obj_get(rt, resv, "groups");
        if (named == EXC) {
            goto done;
        }
        rep.len = 0;
        if (functional) {
            bool has_named = !embi_js_is_undefined(named);
            uint32_t nargs = ncaps + 3 + (has_named ? 1 : 0);
            embi_js_varray *av = embi_js_varray_new(rt, nargs);
            uint32_t ai = 0;
            EMBI_JS_STORE(av, av->elems[ai++], embi_js_from_string(matched));
            for (uint32_t n = 0; n < ncaps; n++) {
                EMBI_JS_STORE(av, av->elems[ai++], capl.a->elems[n]);
            }
            EMBI_JS_STORE(av, av->elems[ai++], embi_js_number((double)position));
            EMBI_JS_STORE(av, av->elems[ai++], embi_js_from_string(S));
            if (has_named) {
                EMBI_JS_STORE(av, av->elems[ai++], named);
            }
            embi_js_value repv = embi_js_call(rt, rv, EMBI_JS_UNDEFINED, (int)nargs, av->elems);
            EMBI_KEEP_ALIVE(av); /* argv points into av */
            if (repv == EXC) {
                goto done;
            }
            embi_js_string *rs = embi_js_to_string(rt, repv);
            if (!rs) {
                goto done;
            }
            ub_str(&rep, rs, 0, embi_js_str_len(rs));
        } else {
            embi_js_value named_v = named;
            if (!embi_js_is_undefined(named)) {
                embi_js_object *no = embi_js_to_object(rt, named);
                if (!no) {
                    goto done;
                }
                named_v = embi_js_from_object(no);
            }
            subst_caps sc = {matched, nullptr, nullptr, capl.a, ncaps};
            if (!get_substitution(rt, S, position, &sc, named_v, embi_js_str_flat(rt, rv), &rep)) {
                goto done;
            }
        }
        if (position >= next_pos) {
            ub_str(&acc, S, next_pos, position);
            for (uint32_t j = 0; j < rep.len; j++) {
                embi_vec_push(&acc, rep.data[j]);
            }
            next_pos = position + mlen;
        }
    }
    if (next_pos < sl) {
        ub_str(&acc, S, next_pos, sl);
    }
    out = str_val(embi_js_str_new_utf16(rt, acc.data, acc.len));
done:
    embi_vec_free(&acc);
    embi_vec_free(&rep);
    return out;
}


/* ------------------------------------------------------------------ Symbol.search */

/* RegExp.prototype[@@search](string). */
static embi_js_value proto_search(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype[Symbol.search]") == EXC) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    embi_js_value prev = embi_js_get(rt, R, KEY(rt, "lastIndex"));
    if (prev == EXC) {
        return EXC;
    }
    if (!embi_js_same_value(prev, embi_js_from_int32(0)) && !set_last_index(rt, R, 0)) {
        return EXC;
    }
    embi_js_value res = regexp_exec(rt, R, S);
    if (res == EXC) {
        return EXC;
    }
    embi_js_value cur = embi_js_get(rt, R, KEY(rt, "lastIndex"));
    if (cur == EXC) {
        return EXC;
    }
    if (!embi_js_same_value(cur, prev) &&
        embi_js_put(rt, R, KEY(rt, "lastIndex"), prev, true) != EMBI_JS_YES) {
        return EXC;
    }
    if (embi_js_is_null(res)) {
        return embi_js_from_int32(-1);
    }
    return obj_get(rt, res, "index");
}

/* ------------------------------------------------------------------ Symbol.split */

/* @@split for a pristine R whose species is %RegExp%. The spec's splitter is a sticky copy of R,
 * and nothing can observe it here, so the engine matches at each position directly. limit is
 * undefined or a primitive (its conversion runs no user code). */
static embi_js_value split_fast(embi_js_runtime *rt, embi_js_object *R, embi_js_string *S,
                                embi_js_value limit)
{
    rx_data *d = rx_of(embi_js_from_object(R));
    bool unicode = (d->flags & (EMBI_RE_FLAG_U | EMBI_RE_FLAG_V)) != 0;
    uint32_t lim = 0xFFFFFFFFu;
    if (!embi_js_is_undefined(limit) && !embi_js_to_uint32(rt, limit, &lim)) {
        return EXC;
    }
    embi_js_object *A = embi_js_array_create_default(rt, 0);
    if (!A) {
        return EXC;
    }
    embi_js_value result = EXC;
    if (lim == 0) {
        return embi_js_from_object(A);
    }
    uint32_t size = embi_js_str_len(S);
    uint32_t ng = embi_re_group_count(d->prog);
    embi_re_input in;
    in.wide = embi_js_str_is_wide(S);
    in.units = in.wide ? (const void *)embi_js_str_wide(S) : (const void *)embi_js_str_latin1(S);
    in.len = size;
    embi_re_limits hook = re_limits(rt);
    capbuf cb;
    capbuf_init(&cb);
    uint32_t slots = embi_re_capture_slots(d->prog);
    capbuf_reserve(&cb, slots);
    if (size == 0) {
        embi_re_status st = embi_re_match_at(d->prog, in, 0, cb.p, &hook);
        if (st == EMBI_RE_MATCH) {
            legacy_set(rt, S, cb.p, slots);
            result = embi_js_from_object(A);
        } else if (st == EMBI_RE_NO_MATCH) {
            result = cdp(rt, A, embi_js_key_from_index(0), embi_js_from_string(S)) ?
                         embi_js_from_object(A) : EXC;
        } else {
            (void)re_error(rt, st);
        }
        goto done;
    }
    uint32_t p = 0;
    uint32_t q = 0;
    uint32_t len_a = 0;
    while (q < size) {
        embi_re_status st = embi_re_match_at(d->prog, in, q, cb.p, &hook);
        if (st == EMBI_RE_NO_MATCH) {
            q = (uint32_t)advance_index(S, q, unicode);
            continue;
        }
        if (st != EMBI_RE_MATCH) {
            (void)re_error(rt, st);
            goto done;
        }
        /* The splitter that the spec constructs is a %RegExp% instance (legacy features on), and
         * each of its successful execs updates the legacy statics. */
        legacy_set(rt, S, cb.p, slots);
        uint32_t e = cb.p[1] > size ? size : cb.p[1];
        if (e == p) {
            q = (uint32_t)advance_index(S, q, unicode);
            continue;
        }
        embi_js_string *t = embi_js_str_substring(rt, S, p, q);
        if (!t || !cdp(rt, A, embi_js_key_from_index(len_a), embi_js_from_string(t))) {
            goto done;
        }
        len_a++;
        if (len_a == lim) {
            result = embi_js_from_object(A);
            goto done;
        }
        p = e;
        for (uint32_t i = 1; i <= ng; i++) {
            embi_js_value cv = EMBI_JS_UNDEFINED;
            if (cb.p[2 * i] != EMBI_RE_UNDEF) {
                embi_js_string *cs = embi_js_str_substring(rt, S, cb.p[2 * i], cb.p[2 * i + 1]);
                if (!cs) {
                    goto done;
                }
                cv = embi_js_from_string(cs);
            }
            if (!cdp(rt, A, embi_js_key_from_index(len_a), cv)) {
                goto done;
            }
            len_a++;
            if (len_a == lim) {
                result = embi_js_from_object(A);
                goto done;
            }
        }
        q = p;
    }
    {
        embi_js_string *tail = embi_js_str_substring(rt, S, p, size);
        if (tail && cdp(rt, A, embi_js_key_from_index(len_a), embi_js_from_string(tail))) {
            result = embi_js_from_object(A);
        }
    }
done:
    capbuf_free(&cb);
    return result;
}

/* RegExp.prototype[@@split](string, limit) (22.2.6.14). */
static embi_js_value proto_split(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (check_rx_receiver(rt, this_v, "RegExp.prototype[Symbol.split]") == EXC) {
        return EXC;
    }
    embi_js_object *R = embi_js_as_object(this_v);
    embi_js_string *S = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!S) {
        return EXC;
    }
    embi_js_value limit = arg_at(argc, argv, 1);
    embi_js_value C = embi_js_species_constructor(rt, R, rt->protos[EMBI_JS_PROTO_REGEXP_CTOR]);
    if (C == EXC) {
        return EXC;
    }
    if (C == rt->protos[EMBI_JS_PROTO_REGEXP_CTOR] &&
        (embi_js_is_undefined(limit) || !embi_js_is_object(limit)) && rx_pristine(rt, R)) {
        return split_fast(rt, R, S, limit);
    }
    embi_js_string *flags = flags_of(rt, R);
    if (!flags) {
        return EXC;
    }
    bool unicode = has_char(flags, 'u') || has_char(flags, 'v');
    embi_js_string *new_flags = flags;
    if (!has_char(flags, 'y')) {
        embi_js_string *y = embi_js_str_new_latin1(rt, (const uint8_t *)"y", 1);
        if (!y) {
            return EXC;
        }
        new_flags = embi_js_str_concat(rt, flags, y);
        if (!new_flags) {
            return EXC;
        }
    }
    embi_js_value args[2] = {this_v, embi_js_from_string(new_flags)};
    embi_js_value splitter_v = embi_js_construct(rt, C, 2, args, C);
    if (splitter_v == EXC) {
        return EXC;
    }
    embi_js_object *splitter = embi_js_as_object(splitter_v);
    embi_js_object *A = embi_js_array_create_default(rt, 0);
    if (!A) {
        return EXC;
    }
    uint32_t lim = 0xFFFFFFFFu;
    if (!embi_js_is_undefined(limit)) {
        if (!embi_js_to_uint32(rt, limit, &lim)) {
            return EXC;
        }
    }
    if (lim == 0) {
        return embi_js_from_object(A);
    }
    uint32_t size = embi_js_str_len(S);
    if (size == 0) {
        embi_js_value z = regexp_exec(rt, splitter, S);
        if (z == EXC) {
            return EXC;
        }
        if (!embi_js_is_null(z)) {
            return embi_js_from_object(A);
        }
        if (!cdp(rt, A, embi_js_key_from_index(0), embi_js_from_string(S))) {
            return EXC;
        }
        return embi_js_from_object(A);
    }
    uint32_t p = 0;
    uint32_t q = 0;
    uint32_t len_a = 0;
    while (q < size) {
        if (!set_last_index(rt, splitter, q)) {
            return EXC;
        }
        embi_js_value z = regexp_exec(rt, splitter, S);
        if (z == EXC) {
            return EXC;
        }
        if (embi_js_is_null(z)) {
            q = (uint32_t)advance_index(S, q, unicode);
            continue;
        }
        double e_d;
        embi_js_value li = embi_js_get(rt, splitter, KEY(rt, "lastIndex"));
        if (li == EXC || !embi_js_to_length(rt, li, &e_d)) {
            return EXC;
        }
        uint32_t e = e_d > size ? size : (uint32_t)e_d;
        if (e == p) {
            q = (uint32_t)advance_index(S, q, unicode);
            continue;
        }
        embi_js_string *t = embi_js_str_substring(rt, S, p, q);
        if (!t || !cdp(rt, A, embi_js_key_from_index(len_a), embi_js_from_string(t))) {
            return EXC;
        }
        len_a++;
        if (len_a == lim) {
            return embi_js_from_object(A);
        }
        p = e;
        double ncap_d;
        embi_js_object *zo = embi_js_as_object(z);
        if (!embi_js_length_of_array_like(rt, zo, &ncap_d)) {
            return EXC;
        }
        /* The capture count is a script-controlled length (up to 2^53). The loop stops at lim
         * results, and each capture is read as the loop reaches it. */
        uint64_t ncap = ncap_d > 1 ? (uint64_t)(ncap_d - 1) : 0;
        for (uint64_t i = 1; i <= ncap; i++) {
            embi_js_value cap = embi_js_get(rt, zo, embi_js_key_from_index((uint32_t)i));
            if (cap == EXC || !cdp(rt, A, embi_js_key_from_index(len_a), cap)) {
                return EXC;
            }
            len_a++;
            if (len_a == lim) {
                return embi_js_from_object(A);
            }
        }
        q = p;
    }
    embi_js_string *tail = embi_js_str_substring(rt, S, p, size);
    if (!tail || !cdp(rt, A, embi_js_key_from_index(len_a), embi_js_from_string(tail))) {
        return EXC;
    }
    return embi_js_from_object(A);
}

/* ------------------------------------------------------------------ species and installation */

static embi_js_value species_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
};

static const struct method_def proto_methods[] = {
    {"test", proto_test, 1},
    {"toString", proto_to_string, 0},
    {"compile", proto_compile, 2},
};

/* The Symbol-keyed methods: the symbol, the function name and the length. */
static const struct {
    enum embi_js_wk sym;
    const char *name;
    embi_js_native fn;
    int length;
} symbol_methods[] = {
    {EMBI_JS_WK_MATCH, "[Symbol.match]", proto_match, 1},
    {EMBI_JS_WK_MATCH_ALL, "[Symbol.matchAll]", proto_match_all, 1},
    {EMBI_JS_WK_REPLACE, "[Symbol.replace]", proto_replace, 2},
    {EMBI_JS_WK_SEARCH, "[Symbol.search]", proto_search, 1},
    {EMBI_JS_WK_SPLIT, "[Symbol.split]", proto_split, 2},
};

void embi_js_init_builtins_regexp(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    rt->protos[EMBI_JS_PROTO_REGEXP] = embi_js_from_object(proto);

    embi_js_object *ctor = embi_js_native_constructor_new(r, "RegExp", 2, regexp_ctor, 0);
    rt->protos[EMBI_JS_PROTO_REGEXP_CTOR] = embi_js_from_object(ctor);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, KEY(rt, "RegExp"), embi_js_from_object(ctor), ATTR_WC);

    embi_js_object *species = embi_js_native_function_new(r, "get [Symbol.species]", 0, species_get, 0);
    embi_js_define_accessor_raw(rt, ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES),
                                embi_js_from_object(species), UNDEF, ATTR_C);
    embi_js_define_method(r, ctor, "escape", regexp_escape, 1, 0);
    legacy_init(r, ctor);

    for (size_t i = 0; i < sizeof proto_methods / sizeof proto_methods[0]; i++) {
        embi_js_define_method(r, proto, proto_methods[i].name, proto_methods[i].fn,
                              proto_methods[i].length, 0);
    }
    embi_js_object *exec = embi_js_native_function_new(r, "exec", 1, proto_exec, 0);
    embi_js_define_raw(rt, proto, KEY(rt, "exec"), embi_js_from_object(exec), ATTR_WC);
    rt->protos[EMBI_JS_PROTO_REGEXP_EXEC] = embi_js_from_object(exec);

    for (size_t i = 0; i < sizeof flag_props / sizeof flag_props[0]; i++) {
        embi_js_object *g = embi_js_native_function_new(r, flag_props[i].getter, 0, flag_getter, (int)i);
        embi_js_define_accessor_raw(rt, proto, KEY(rt, flag_props[i].name), embi_js_from_object(g),
                                    UNDEF, ATTR_C);
    }
    embi_js_object *fg = embi_js_native_function_new(r, "get flags", 0, proto_flags_get, 0);
    embi_js_define_accessor_raw(rt, proto, KEY(rt, "flags"), embi_js_from_object(fg), UNDEF, ATTR_C);
    embi_js_object *sg = embi_js_native_function_new(r, "get source", 0, proto_source_get, 0);
    embi_js_define_accessor_raw(rt, proto, KEY(rt, "source"), embi_js_from_object(sg), UNDEF, ATTR_C);

    for (size_t i = 0; i < sizeof symbol_methods / sizeof symbol_methods[0]; i++) {
        embi_js_object *f = embi_js_native_function_new(r, symbol_methods[i].name,
                                                        symbol_methods[i].length,
                                                        symbol_methods[i].fn, 0);
        embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, symbol_methods[i].sym),
                           embi_js_from_object(f), ATTR_WC);
    }

    embi_js_object *iter_proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_ITERATOR]);
    rt->protos[EMBI_JS_PROTO_REGEXP_STRING_ITER] = embi_js_from_object(iter_proto);
    embi_js_define_method(r, iter_proto, "next", rx_iter_next, 0, 0);
    embi_js_define_raw(rt, iter_proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       ascii_str(rt, "RegExp String Iterator"), ATTR_C);
}
