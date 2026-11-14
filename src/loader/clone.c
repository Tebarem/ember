/*
 * Structured serialization (W7b). See clone.h for the format.
 *
 * Writer and reader walk the graph in the same order and register objects in the same order, so a
 * backreference index means the same object on both sides. A memory object is registered when its
 * record starts, before its children: for containers that is before the children are written, and
 * for an ArrayBuffer view it is after its buffer record (the buffer is written first, as the HTML
 * algorithm does). The reader pushes each object into a JS array as it is created, which keeps it
 * alive for the GC; the writer does the same with its memory table.
 *
 * Intrinsics: Map, Set, RegExp, TypedArray and Error constructors come from the realm (the engine
 * exposes them). Date has no exported constructor, so the reader calls the global Date. A script
 * that replaces globalThis.Date changes what deserialization creates. The RegExp source and flags
 * are read through their getters, so a script that overrides them changes what is written.
 */
#include "loader/clone.h"

#include "js/bigint.h"
#include "js/collections.h"
#include "js/typedarray.h"

#include <SDL3/SDL_stdinc.h>

#include <string.h>

#define CL_MAGIC "EMCL"
#define CL_NO_LENGTH UINT64_MAX /* length-tracking view marker in the byte stream */

enum cl_tag {
    CL_UNDEFINED = 0x01,
    CL_NULL,
    CL_TRUE,
    CL_FALSE,
    CL_INT32,
    CL_DOUBLE,
    CL_STRING,
    CL_BIGINT,
    CL_BOOL_OBJ,
    CL_NUMBER_OBJ,
    CL_STRING_OBJ,
    CL_BIGINT_OBJ,
    CL_DATE,
    CL_REGEXP,
    CL_ARRAYBUFFER,
    CL_VIEW,
    CL_MAP,
    CL_SET,
    CL_ERROR,
    CL_OBJECT,
    CL_ARRAY,
    CL_BACKREF,
    CL_PLATFORM, /* a host object (a Blob, a File): u32 length, then the host's payload */
    CL_XFER_TABLE, /* right after the header, only when the transfer list holds platform objects: u32 n, then n x (u32 length, payload) */
    CL_XFERRED,    /* an object of the transfer list: u32 index into the table */
};

/* Error names by code, with their prototype slots. Code 0 (Error) is also the fallback. */
static const char *const cl_error_names[7] = {"Error",          "EvalError",  "RangeError",
                                              "ReferenceError", "SyntaxError", "TypeError",
                                              "URIError"};
static const enum embi_js_proto cl_error_protos[7] = {
    EMBI_JS_PROTO_ERROR,          EMBI_JS_PROTO_EVAL_ERROR,      EMBI_JS_PROTO_RANGE_ERROR,
    EMBI_JS_PROTO_REFERENCE_ERROR, EMBI_JS_PROTO_SYNTAX_ERROR,   EMBI_JS_PROTO_TYPE_ERROR,
    EMBI_JS_PROTO_URI_ERROR};

/* ------------------------------------------------------------------ shared helpers */

static bool cl_is_string(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_STRING;
}

static bool cl_is_object(embi_js_value v)
{
    return embi_js_is_object(v);
}

/* Pushes v at index idx of a JS array the engine keeps alive. */
static bool cl_push(embi_js_runtime *rt, embi_js_object *a, uint32_t idx, embi_js_value v)
{
    return embi_js_create_data_property(rt, a, embi_js_key_from_index(idx), v) == EMBI_JS_YES;
}

static void cl_set_hidden(embi_js_runtime *rt, embi_js_object *o, const char *name,
                          embi_js_value v)
{
    embi_js_define_raw(rt, o, embi_js_key_c(rt, name), v, EMBI_JS_ATTR_HIDDEN);
}

/* True when the string value equals the ASCII literal. */
static bool cl_str_is(embi_js_runtime *rt, embi_js_value sv, const char *lit)
{
    embi_js_string *s = embi_js_str_flat(rt, sv);
    size_t n = strlen(lit);
    if (s == nullptr || embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint8_t)lit[i]) {
            return false;
        }
    }
    return true;
}

static const embi_clone_host *g_host;

void embi_clone_set_host(const embi_clone_host *h)
{
    g_host = h;
}

/* A DataCloneError with the message "<what> could not be cloned.". Always returns false. */
static bool cl_clone_fail(embi_js_runtime *rt, const char *what)
{
    char msg[160];
    SDL_snprintf(msg, sizeof msg, "%s could not be cloned.", what);
    embi_js_object *e =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_ERROR], &embi_js_class_error);
    embi_js_string *name = embi_js_str_new_utf8(rt, "DataCloneError", 14);
    embi_js_string *text = embi_js_str_new_utf8(rt, msg, strlen(msg));
    if (e != nullptr && name != nullptr && text != nullptr) {
        cl_set_hidden(rt, e, "name", embi_js_from_string(name));
        cl_set_hidden(rt, e, "message", embi_js_from_string(text));
        (void)embi_js_throw_value(rt, embi_js_from_object(e));
    }
    return false;
}

/* ------------------------------------------------------------------ serializer */

typedef struct cl_ser {
    embi_js_realm *r;
    embi_js_runtime *rt;
    embi_sb *out;
    embi_hashmap memo;    /* object address -> memory index */
    embi_js_object *keep; /* JS array of every registered object, so the GC keeps them alive */
    uint32_t count;       /* registered objects */
    uint32_t depth;       /* open objects, see EMBI_CLONE_MAX_DEPTH */
    bool overflow;        /* the output would pass UINT32_MAX bytes; checked once at the end */
    const embi_js_value *xobj; /* the platform objects of the transfer list (nxobj of them) */
    uint32_t nxobj;
} cl_ser;

/* Appends n bytes. The output length is a u32, so a write that would pass UINT32_MAX sets the
 * overflow flag (the serialization then fails) instead of wrapping. */
static void cl_w_bytes(cl_ser *s, const void *p, size_t n)
{
    if (n == 0 || s->overflow) {
        return;
    }
    if (n > (size_t)(UINT32_MAX - s->out->len)) {
        s->overflow = true;
        return;
    }
    embi_sb_append(s->out, (embi_str){(const char *)p, (uint32_t)n});
}

static void cl_w_u8(cl_ser *s, uint8_t v)
{
    cl_w_bytes(s, &v, 1);
}

static void cl_w_u32(cl_ser *s, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    cl_w_bytes(s, b, sizeof b);
}

static void cl_w_u64(cl_ser *s, uint64_t v)
{
    unsigned char b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (unsigned char)(v >> (8 * i));
    }
    cl_w_bytes(s, b, sizeof b);
}

static void cl_w_f64(cl_ser *s, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    cl_w_u64(s, bits);
}

/* A string as a u32 unit count and the UTF-16LE units (lone surrogates are kept). The units are
 * packed into a stack buffer and appended in chunks, not one unit at a time. */
static void cl_w_string(cl_ser *s, embi_js_value sv)
{
    embi_js_string *str = embi_js_str_flat(s->rt, sv);
    uint32_t n = embi_js_str_len(str);
    cl_w_u32(s, n);
    enum { CHUNK_UNITS = 1024 };
    unsigned char buf[CHUNK_UNITS * 2];
    for (uint32_t i = 0; i < n;) {
        uint32_t take = n - i < CHUNK_UNITS ? n - i : CHUNK_UNITS;
        for (uint32_t j = 0; j < take; j++) {
            uint16_t u = embi_js_str_at(str, i + j);
            buf[2 * j] = (unsigned char)(u & 0xFFu);
            buf[2 * j + 1] = (unsigned char)(u >> 8);
        }
        cl_w_bytes(s, buf, (size_t)take * 2u);
        i += take;
    }
}

static bool cl_w_bigint(cl_ser *s, embi_js_value big)
{
    embi_js_string *txt = embi_js_bigint_to_string(s->rt, embi_js_as_bigint(big), 10);
    if (txt == nullptr) {
        return false;
    }
    cl_w_string(s, embi_js_from_string(txt));
    return true;
}

static double cl_number_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

static bool cl_memo_find(cl_ser *s, const embi_js_object *o, uint32_t *idx)
{
    uint64_t v = 0;
    if (embi_hashmap_get(&s->memo, (uint64_t)(uintptr_t)o, &v)) {
        *idx = (uint32_t)v;
        return true;
    }
    return false;
}

/* Registers an object (its index is the next count) and keeps it alive. */
static bool cl_memo_add(cl_ser *s, embi_js_value ov)
{
    if (!cl_push(s->rt, s->keep, s->count, ov)) {
        return false;
    }
    (void)embi_hashmap_put(&s->memo, (uint64_t)(uintptr_t)embi_js_as_object(ov), s->count);
    s->count++;
    return true;
}

static bool cl_ser_value(cl_ser *s, embi_js_value v);

/* Writes an own enumerable string-keyed property list: a count, then key and value pairs. The key
 * list is taken first and held in a JS array, so getters cannot change which keys are considered.
 * As the algorithm requires, a key that is no longer an own property when its turn comes (a getter
 * deleted it) is skipped, so the count is only known at the end and is patched in afterwards. */
static bool cl_ser_props(cl_ser *s, embi_js_value ov)
{
    embi_js_runtime *rt = s->rt;
    embi_js_object *o = embi_js_as_object(ov);
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_enumerable_own_keys(rt, o, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return false;
    }
    uint32_t n = keys.len;
    embi_js_object *names = embi_js_array_create_default(rt, 0);
    for (uint32_t i = 0; i < n; i++) {
        embi_js_string *ks = embi_js_to_string(rt, embi_js_key_to_value(rt, keys.data[i]));
        if (ks == nullptr || !cl_push(rt, names, i, embi_js_from_string(ks))) {
            embi_vec_free(&keys);
            return false;
        }
    }
    embi_vec_free(&keys);

    size_t count_at = s->out->len;
    cl_w_u32(s, 0);
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value name =
            embi_js_obj_get(rt, names, embi_js_key_from_index(i), embi_js_from_object(names));
        if (name == EMBI_JS_EXCEPTION) {
            return false;
        }
        embi_js_key key = embi_js_to_property_key(rt, name);
        embi_js_pdesc pd;
        memset(&pd, 0, sizeof pd);
        int own = embi_js_obj_get_own_property(rt, o, key, &pd);
        if (own == EMBI_JS_THREW) {
            return false;
        }
        if (own != EMBI_JS_YES) {
            continue;
        }
        embi_js_value val = embi_js_obj_get(rt, o, key, ov);
        if (val == EMBI_JS_EXCEPTION) {
            return false;
        }
        cl_w_string(s, name);
        if (!cl_ser_value(s, val)) {
            return false;
        }
        written++;
    }
    if (!s->overflow) {
        /* The placeholder is inside the output: appends never move earlier bytes. */
        for (int i = 0; i < 4; i++) {
            s->out->data[count_at + (size_t)i] = (char)((written >> (8 * i)) & 0xFFu);
        }
    }
    return true;
}

/* Reads an own data property value. Returns EMBI_JS_THREW on a failure, EMBI_JS_NO when the
 * property is absent or an accessor, and EMBI_JS_YES with the value in *out. */
static int cl_own_data(embi_js_runtime *rt, embi_js_object *o, const char *name, embi_js_value *out)
{
    embi_js_pdesc pd;
    memset(&pd, 0, sizeof pd);
    int r = embi_js_obj_get_own_property(rt, o, embi_js_key_c(rt, name), &pd);
    if (r != EMBI_JS_YES || (pd.has & EMBI_JS_PD_VALUE) == 0) {
        return r == EMBI_JS_THREW ? EMBI_JS_THREW : EMBI_JS_NO;
    }
    *out = pd.value;
    return EMBI_JS_YES;
}

static bool cl_ser_error(cl_ser *s, embi_js_value ov)
{
    embi_js_runtime *rt = s->rt;
    embi_js_object *o = embi_js_as_object(ov);
    cl_w_u8(s, CL_ERROR);

    embi_js_value nv = embi_js_obj_get(rt, o, embi_js_key_c(rt, "name"), ov);
    if (nv == EMBI_JS_EXCEPTION) {
        return false;
    }
    uint8_t code = 0;
    if (cl_is_string(nv)) {
        for (uint8_t i = 0; i < 7; i++) {
            if (cl_str_is(rt, nv, cl_error_names[i])) {
                code = i;
                break;
            }
        }
    }
    cl_w_u8(s, code);

    embi_js_value mv = EMBI_JS_UNDEFINED;
    int mr = cl_own_data(rt, o, "message", &mv);
    if (mr == EMBI_JS_THREW) {
        return false;
    }
    if (mr == EMBI_JS_YES) {
        embi_js_string *ms = embi_js_to_string(rt, mv);
        if (ms == nullptr) {
            return false;
        }
        cl_w_u8(s, 1);
        cl_w_string(s, embi_js_from_string(ms));
    } else {
        cl_w_u8(s, 0);
    }

    embi_js_value cv = EMBI_JS_UNDEFINED;
    int cr = cl_own_data(rt, o, "cause", &cv);
    if (cr == EMBI_JS_THREW) {
        return false;
    }
    if (cr == EMBI_JS_YES) {
        cl_w_u8(s, 1);
        if (!cl_ser_value(s, cv)) {
            return false;
        }
    } else {
        cl_w_u8(s, 0);
    }

    embi_js_value sv = EMBI_JS_UNDEFINED;
    int sr = cl_own_data(rt, o, "stack", &sv);
    if (sr == EMBI_JS_THREW) {
        return false;
    }
    if (sr == EMBI_JS_YES && cl_is_string(sv)) {
        cl_w_u8(s, 1);
        cl_w_string(s, sv);
    } else {
        cl_w_u8(s, 0);
    }
    return true;
}

static bool cl_ser_abuf(cl_ser *s, embi_js_object *o)
{
    embi_js_abuf *b = embi_js_abuf_of(o);
    if ((b->flags & EMBI_JS_ABUF_DETACHED) != 0) {
        return cl_clone_fail(s->rt, "A detached ArrayBuffer");
    }
    if ((uint64_t)b->len > (uint64_t)(UINT32_MAX - s->out->len)) {
        return cl_clone_fail(s->rt, "An ArrayBuffer this large");
    }
    cl_w_u8(s, CL_ARRAYBUFFER);
    cl_w_u8(s, (b->flags & EMBI_JS_ABUF_RESIZABLE) != 0 ? 1 : 0);
    cl_w_u64(s, (uint64_t)b->max_len);
    cl_w_u64(s, (uint64_t)b->len);
    if (b->len != 0 && b->data != nullptr) {
        cl_w_bytes(s, b->data, b->len);
    }
    return true;
}

/* A view record. Its buffer is written first (and registered), then the view itself is
 * registered, which the reader mirrors. */
static bool cl_ser_view(cl_ser *s, embi_js_value ov, embi_js_object *o)
{
    embi_js_runtime *rt = s->rt;
    embi_js_varray *vw = embi_js_view_of(o);
    bool dv = o->klass == &embi_js_class_dataview;
    uint8_t kind = dv ? 0 : (uint8_t)embi_js_as_int32(vw->elems[EMBI_JS_VIEW_KIND]);
    if (!dv && kind >= EMBI_JS_TA_KINDS) {
        return cl_clone_fail(rt, "A TypedArray");
    }
    embi_js_value bufv = vw->elems[EMBI_JS_VIEW_BUFFER];
    if (!cl_is_object(bufv) ||
        (embi_js_abuf_of(embi_js_as_object(bufv))->flags & EMBI_JS_ABUF_DETACHED) != 0) {
        return cl_clone_fail(rt, "A view on a detached ArrayBuffer");
    }
    size_t esz = dv ? 1 : embi_js_bin_size((enum embi_js_ta_kind)kind);
    size_t len = 0;
    if (!embi_js_view_len(vw, esz, &len)) {
        return cl_clone_fail(rt, "An out-of-bounds view");
    }
    bool tracking = vw->elems[EMBI_JS_VIEW_LENGTH] == EMBI_JS_UNDEFINED;
    uint64_t off = (uint64_t)embi_js_view_offset(vw);
    cl_w_u8(s, CL_VIEW);
    cl_w_u8(s, dv ? 1 : 0);
    cl_w_u8(s, kind);
    cl_w_u64(s, off);
    cl_w_u64(s, tracking ? CL_NO_LENGTH : (uint64_t)len);
    if (!cl_ser_value(s, bufv)) {
        return false;
    }
    return cl_memo_add(s, ov);
}

/* Map and Set: the entries are collected into a JS array first (a snapshot, as the algorithm
 * requires), then written with their count. */
static bool cl_ser_coll(cl_ser *s, embi_js_value ov, bool is_map)
{
    embi_js_runtime *rt = s->rt;
    embi_js_object *it = embi_js_coll_iter_new(
        rt, embi_js_as_object(ov),
        is_map ? EMBI_JS_COLL_ITER_ENTRIES : EMBI_JS_COLL_ITER_VALUES);
    if (it == nullptr) {
        return false;
    }
    embi_js_object *flat = embi_js_array_create_default(rt, 0);
    uint32_t n = 0;
    for (;;) {
        embi_js_value item = EMBI_JS_UNDEFINED;
        int st = embi_js_coll_iter_step(rt, it, &item);
        if (st == EMBI_JS_THREW) {
            return false;
        }
        if (st == EMBI_JS_NO) {
            break;
        }
        if (is_map) {
            if (!cl_is_object(item)) {
                return false;
            }
            embi_js_object *ent = embi_js_as_object(item);
            embi_js_value k = embi_js_obj_get(rt, ent, embi_js_key_from_index(0), item);
            embi_js_value val = embi_js_obj_get(rt, ent, embi_js_key_from_index(1), item);
            if (k == EMBI_JS_EXCEPTION || val == EMBI_JS_EXCEPTION ||
                !cl_push(rt, flat, n++, k) || !cl_push(rt, flat, n++, val)) {
                return false;
            }
        } else {
            if (!cl_push(rt, flat, n++, item)) {
                return false;
            }
        }
    }
    uint32_t count = is_map ? n / 2 : n;
    cl_w_u32(s, count);
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value item = embi_js_obj_get(rt, flat, embi_js_key_from_index(i),
                                             embi_js_from_object(flat));
        if (item == EMBI_JS_EXCEPTION || !cl_ser_value(s, item)) {
            return false;
        }
    }
    return true;
}

static bool cl_ser_regexp(cl_ser *s, embi_js_value ov)
{
    embi_js_runtime *rt = s->rt;
    embi_js_object *o = embi_js_as_object(ov);
    embi_js_value src = embi_js_obj_get(rt, o, embi_js_key_c(rt, "source"), ov);
    if (src == EMBI_JS_EXCEPTION) {
        return false;
    }
    embi_js_value flags = embi_js_obj_get(rt, o, embi_js_key_c(rt, "flags"), ov);
    if (flags == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (!cl_is_string(src) || !cl_is_string(flags)) {
        return cl_clone_fail(rt, "A RegExp");
    }
    cl_w_u8(s, CL_REGEXP);
    cl_w_string(s, src);
    cl_w_string(s, flags);
    return true;
}

/* Object kinds other than the plain ones. Each registers itself before its contents. */
static bool cl_ser_object_body(cl_ser *s, embi_js_value ov, embi_js_object *o)
{
    embi_js_runtime *rt = s->rt;
    const embi_js_class *k = o->klass;

    if (k == &embi_js_class_object) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_OBJECT);
        return cl_ser_props(s, ov);
    }
    if (k == &embi_js_class_array) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        double len = 0;
        if (!embi_js_length_of_array_like(rt, o, &len)) {
            return false;
        }
        cl_w_u8(s, CL_ARRAY);
        cl_w_u32(s, (uint32_t)len);
        return cl_ser_props(s, ov);
    }
    if (k == &embi_js_class_date) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_DATE);
        cl_w_f64(s, embi_js_as_double((embi_js_value)(uintptr_t)o->internal));
        return true;
    }
    if (k == &embi_js_class_number) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_NUMBER_OBJ);
        cl_w_f64(s, cl_number_of(embi_js_wrapper_value(o)));
        return true;
    }
    if (k == &embi_js_class_boolean) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_BOOL_OBJ);
        cl_w_u8(s, embi_js_as_bool(embi_js_wrapper_value(o)) ? 1 : 0);
        return true;
    }
    if (k == &embi_js_class_string) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_STRING_OBJ);
        cl_w_string(s, embi_js_wrapper_value(o));
        return true;
    }
    if (k == &embi_js_class_bigint) {
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, CL_BIGINT_OBJ);
        return cl_w_bigint(s, embi_js_bigint_wrapper_value(o));
    }
    if (k == &embi_js_class_error) {
        return cl_memo_add(s, ov) && cl_ser_error(s, ov);
    }
    if (k == &embi_js_class_arraybuffer) {
        return cl_memo_add(s, ov) && cl_ser_abuf(s, o);
    }
    if (k == &embi_js_class_typedarray || k == &embi_js_class_dataview) {
        return cl_ser_view(s, ov, o);
    }
    if (embi_js_coll_is_map(ov) || embi_js_coll_is_set(ov)) {
        bool is_map = embi_js_coll_is_map(ov);
        if (!cl_memo_add(s, ov)) {
            return false;
        }
        cl_w_u8(s, is_map ? CL_MAP : CL_SET);
        return cl_ser_coll(s, ov, is_map);
    }
    if (k->name != nullptr && strcmp(k->name, "RegExp") == 0 && o->internal != nullptr) {
        return cl_memo_add(s, ov) && cl_ser_regexp(s, ov);
    }
    for (uint32_t i = 0; i < s->nxobj; i++) {
        if (s->xobj[i] == ov) {
            if (!cl_memo_add(s, ov)) {
                return false;
            }
            cl_w_u8(s, CL_XFERRED);
            cl_w_u32(s, i);
            return true;
        }
    }
    if (g_host != nullptr && g_host->serialize != nullptr) {
        embi_sb payload = {nullptr, 0, 0};
        int r = g_host->serialize(s->r, ov, &payload);
        if (r < 0) {
            embi_sb_free(&payload);
            return false;
        }
        if (r > 0) {
            bool ok = cl_memo_add(s, ov);
            if (ok) {
                cl_w_u8(s, CL_PLATFORM);
                cl_w_u32(s, payload.len);
                cl_w_bytes(s, payload.data, payload.len);
            }
            embi_sb_free(&payload);
            return ok;
        }
    }
    return cl_clone_fail(rt, "An object");
}

static bool cl_ser_object(cl_ser *s, embi_js_value ov)
{
    embi_js_object *o = embi_js_as_object(ov);
    uint32_t idx = 0;
    if (cl_memo_find(s, o, &idx)) {
        cl_w_u8(s, CL_BACKREF);
        cl_w_u32(s, idx);
        return true;
    }
    if (embi_js_obj_is_proxy(o)) {
        return cl_clone_fail(s->rt, "A Proxy object");
    }
    if (embi_js_is_callable(ov)) {
        return cl_clone_fail(s->rt, "A function");
    }
    if (s->depth >= EMBI_CLONE_MAX_DEPTH) {
        return cl_clone_fail(s->rt, "An object nested too deeply");
    }
    s->depth++;
    bool ok = cl_ser_object_body(s, ov, o);
    s->depth--;
    return ok;
}

static bool cl_ser_value(cl_ser *s, embi_js_value v)
{
    if (v == EMBI_JS_UNDEFINED) {
        cl_w_u8(s, CL_UNDEFINED);
    } else if (v == EMBI_JS_NULL) {
        cl_w_u8(s, CL_NULL);
    } else if (v == EMBI_JS_TRUE) {
        cl_w_u8(s, CL_TRUE);
    } else if (v == EMBI_JS_FALSE) {
        cl_w_u8(s, CL_FALSE);
    } else if (embi_js_is_int32(v)) {
        cl_w_u8(s, CL_INT32);
        cl_w_u32(s, (uint32_t)embi_js_as_int32(v));
    } else if (embi_js_is_double(v)) {
        cl_w_u8(s, CL_DOUBLE);
        cl_w_f64(s, embi_js_as_double(v));
    } else if (cl_is_string(v)) {
        cl_w_u8(s, CL_STRING);
        cl_w_string(s, v);
    } else if (embi_js_is_bigint(v)) {
        cl_w_u8(s, CL_BIGINT);
        return cl_w_bigint(s, v);
    } else if (cl_is_object(v)) {
        return cl_ser_object(s, v);
    } else if (embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_SYMBOL) {
        return cl_clone_fail(s->rt, "A Symbol");
    } else {
        return cl_clone_fail(s->rt, "This value");
    }
    return true;
}

static bool cl_serialize(embi_js_realm *r, embi_js_value v, const embi_js_value *transfer,
                         uint32_t ntransfer, embi_sb *out)
{
    embi_js_runtime *rt = r->rt;
    uint32_t len0 = out->len;
    /* The platform objects of the transfer list, with the payloads the host prepared for them. */
    embi_js_value *xobj = nullptr;
    embi_sb *xpay = nullptr;
    uint32_t nx = 0;
    bool ok = true;
    for (uint32_t i = 0; i < ntransfer && ok; i++) {
        embi_js_value t = transfer[i];
        bool is_ab = cl_is_object(t) && embi_js_as_object(t)->klass == &embi_js_class_arraybuffer;
        for (uint32_t j = 0; j < i && ok; j++) {
            if (transfer[j] == t) {
                ok = cl_clone_fail(rt, is_ab ? "A duplicate ArrayBuffer in the transfer list"
                                             : "A duplicate object in the transfer list");
            }
        }
        if (!ok) {
            break;
        }
        if (is_ab) {
            if ((embi_js_abuf_of(embi_js_as_object(t))->flags & EMBI_JS_ABUF_DETACHED) != 0) {
                ok = cl_clone_fail(rt, "A detached ArrayBuffer in the transfer list");
            }
            continue;
        }
        if (!cl_is_object(t) || g_host == nullptr || g_host->transfer_prepare == nullptr) {
            ok = cl_clone_fail(rt, "A transfer list entry that is not an ArrayBuffer");
            break;
        }
        if (xobj == nullptr) {
            xobj = embi_alloc(ntransfer * sizeof *xobj);
            xpay = embi_alloc(ntransfer * sizeof *xpay);
        }
        embi_sb pay = {nullptr, 0, 0};
        int rc = g_host->transfer_prepare(r, t, &pay);
        if (rc < 0) {
            embi_sb_free(&pay);
            ok = false;
            break;
        }
        if (rc == 0) {
            embi_sb_free(&pay);
            ok = cl_clone_fail(rt, "A non-transferable object in the transfer list");
            break;
        }
        xobj[nx] = t;
        xpay[nx] = pay;
        nx++;
    }

    cl_ser s;
    memset(&s, 0, sizeof s);
    s.r = r;
    s.rt = rt;
    s.out = out;
    s.xobj = xobj;
    s.nxobj = nx;
    embi_hashmap_init(&s.memo);
    s.keep = embi_js_array_create_default(rt, 0);

    if (ok) {
        cl_w_bytes(&s, CL_MAGIC, 4);
        cl_w_u8(&s, EMBI_CLONE_VERSION);
        if (nx > 0) {
            cl_w_u8(&s, CL_XFER_TABLE);
            cl_w_u32(&s, nx);
            for (uint32_t i = 0; i < nx; i++) {
                cl_w_u32(&s, xpay[i].len);
                cl_w_bytes(&s, xpay[i].data, xpay[i].len);
            }
        }
        ok = cl_ser_value(&s, v);
        if (ok && s.overflow) {
            ok = cl_clone_fail(rt, "Data this large");
        }
    }
    if (ok) {
        for (uint32_t i = 0; i < ntransfer; i++) {
            if (cl_is_object(transfer[i]) && embi_js_as_object(transfer[i])->klass == &embi_js_class_arraybuffer) {
                (void)embi_js_arraybuffer_detach(transfer[i]);
            }
        }
        for (uint32_t i = 0; i < nx; i++) {
            if (g_host->transfer_commit != nullptr) {
                g_host->transfer_commit(r, xobj[i]);
            }
        }
    } else {
        for (uint32_t i = 0; i < nx; i++) {
            if (g_host->transfer_rollback != nullptr) {
                g_host->transfer_rollback(r, xobj[i]);
            }
        }
        out->len = len0;
    }
    for (uint32_t i = 0; i < nx; i++) {
        embi_sb_free(&xpay[i]);
    }
    if (xobj != nullptr) {
        embi_free(xobj, ntransfer * sizeof *xobj);
        embi_free(xpay, ntransfer * sizeof *xpay);
    }
    embi_hashmap_free(&s.memo);
    return ok;
}

/* ------------------------------------------------------------------ deserializer */

typedef struct cl_de {
    embi_js_realm *r;
    embi_js_runtime *rt;
    const unsigned char *p;
    size_t end;
    size_t pos;
    embi_js_object *objs; /* JS array of registered objects, in registration order */
    uint32_t count;
    uint32_t depth;
    embi_js_object *xfer; /* JS array of the objects rebuilt from the transfer table */
    uint32_t nxfer;
} cl_de;

static bool cl_de_fail(embi_js_runtime *rt)
{
    (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "invalid structured clone data");
    return false;
}

static bool cl_de_take(cl_de *d, size_t n, const unsigned char **out)
{
    if (d->end - d->pos < n) {
        return false;
    }
    *out = d->p + d->pos;
    d->pos += n;
    return true;
}

static bool cl_de_u8(cl_de *d, uint8_t *out)
{
    const unsigned char *b = nullptr;
    if (!cl_de_take(d, 1, &b)) {
        return false;
    }
    *out = b[0];
    return true;
}

static bool cl_de_u32(cl_de *d, uint32_t *out)
{
    const unsigned char *b = nullptr;
    if (!cl_de_take(d, 4, &b)) {
        return false;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool cl_de_u64(cl_de *d, uint64_t *out)
{
    const unsigned char *b = nullptr;
    if (!cl_de_take(d, 8, &b)) {
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)b[i] << (8 * i);
    }
    *out = v;
    return true;
}

static bool cl_de_f64(cl_de *d, double *out)
{
    uint64_t bits = 0;
    if (!cl_de_u64(d, &bits)) {
        return false;
    }
    memcpy(out, &bits, sizeof bits);
    return true;
}

/* Reads a string record into a new string value. */
static bool cl_de_string(cl_de *d, embi_js_value *out)
{
    uint32_t n = 0;
    if (!cl_de_u32(d, &n) || n > (d->end - d->pos) / 2) {
        return cl_de_fail(d->rt);
    }
    const unsigned char *b = d->p + d->pos;
    uint16_t *units = embi_alloc((size_t)(n != 0 ? n : 1) * 2u);
    for (uint32_t i = 0; i < n; i++) {
        units[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    }
    d->pos += (size_t)n * 2u;
    embi_js_string *str = embi_js_str_new_utf16(d->rt, units, n);
    embi_free(units, (size_t)(n != 0 ? n : 1) * 2u);
    if (str == nullptr) {
        return false;
    }
    *out = embi_js_from_string(str);
    return true;
}

static bool cl_de_value(cl_de *d, embi_js_value *out);

static bool cl_de_register(cl_de *d, embi_js_value v)
{
    if (!cl_push(d->rt, d->objs, d->count, v)) {
        return false;
    }
    d->count++;
    return true;
}

/* Reads the property list written by cl_ser_props into object o (created and registered). */
static bool cl_de_props(cl_de *d, embi_js_object *o)
{
    uint32_t n = 0;
    if (!cl_de_u32(d, &n) || n > d->end - d->pos) {
        return cl_de_fail(d->rt);
    }
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value name = EMBI_JS_UNDEFINED;
        embi_js_value val = EMBI_JS_UNDEFINED;
        if (!cl_de_string(d, &name) || !cl_de_value(d, &val)) {
            return false;
        }
        if (embi_js_create_data_property(d->rt, o, embi_js_to_property_key(d->rt, name), val) !=
            EMBI_JS_YES) {
            return false;
        }
    }
    return true;
}

static bool cl_de_date_ctor(cl_de *d, embi_js_value *out)
{
    embi_js_value c = embi_js_obj_get(d->rt, d->r->global, embi_js_key_c(d->rt, "Date"),
                                      embi_js_from_object(d->r->global));
    if (c == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (!embi_js_is_constructor(c)) {
        return cl_de_fail(d->rt);
    }
    *out = c;
    return true;
}

static bool cl_de_error(cl_de *d, embi_js_value *out)
{
    embi_js_runtime *rt = d->rt;
    uint8_t code = 0;
    if (!cl_de_u8(d, &code) || code >= 7) {
        return cl_de_fail(rt);
    }
    embi_js_object *e = embi_js_object_new_class(rt, rt->protos[cl_error_protos[code]],
                                                 &embi_js_class_error);
    if (!cl_de_register(d, embi_js_from_object(e))) {
        return false;
    }
    uint8_t flag = 0;
    embi_js_value v = EMBI_JS_UNDEFINED;
    if (!cl_de_u8(d, &flag)) {
        return cl_de_fail(rt);
    }
    if (flag != 0) {
        if (!cl_de_string(d, &v)) {
            return false;
        }
        cl_set_hidden(rt, e, "message", v);
    }
    if (!cl_de_u8(d, &flag)) {
        return cl_de_fail(rt);
    }
    if (flag != 0) {
        if (!cl_de_value(d, &v)) {
            return false;
        }
        cl_set_hidden(rt, e, "cause", v);
    }
    if (!cl_de_u8(d, &flag)) {
        return cl_de_fail(rt);
    }
    if (flag != 0) {
        if (!cl_de_string(d, &v)) {
            return false;
        }
        cl_set_hidden(rt, e, "stack", v);
    }
    *out = embi_js_from_object(e);
    return true;
}

static bool cl_de_view(cl_de *d, embi_js_value *out)
{
    embi_js_runtime *rt = d->rt;
    uint8_t dv = 0;
    uint8_t kind = 0;
    uint64_t off = 0;
    uint64_t len = 0;
    if (!cl_de_u8(d, &dv) || !cl_de_u8(d, &kind) || !cl_de_u64(d, &off) || !cl_de_u64(d, &len)) {
        return cl_de_fail(rt);
    }
    embi_js_value bufv = EMBI_JS_UNDEFINED;
    if (!cl_de_value(d, &bufv)) {
        return false;
    }
    if (!cl_is_object(bufv) || embi_js_as_object(bufv)->klass != &embi_js_class_arraybuffer) {
        return cl_de_fail(rt);
    }
    bool tracking = len == CL_NO_LENGTH;
    if (dv > 1 || off > UINT32_MAX || (!tracking && len > UINT32_MAX)) {
        return cl_de_fail(rt);
    }
    if (dv == 1) {
        size_t buflen = embi_js_abuf_of(embi_js_as_object(bufv))->len;
        if (off > buflen || (!tracking && len > buflen - off)) {
            return cl_de_fail(rt);
        }
        embi_js_varray *vw = embi_js_varray_new(rt, EMBI_JS_VIEW_SLOTS);
        EMBI_JS_STORE(vw, vw->elems[EMBI_JS_VIEW_BUFFER], bufv);
        EMBI_JS_STORE(vw, vw->elems[EMBI_JS_VIEW_OFFSET], embi_js_number((double)off));
        EMBI_JS_STORE(vw, vw->elems[EMBI_JS_VIEW_LENGTH],
                      tracking ? EMBI_JS_UNDEFINED : embi_js_number((double)len));
        EMBI_JS_STORE(vw, vw->elems[EMBI_JS_VIEW_KIND], embi_js_from_int32(-1));
        embi_js_object *o = embi_js_object_new_class(
            rt, embi_js_binary_intrinsic(rt, EMBI_JS_BI_DATAVIEW_PROTO), &embi_js_class_dataview);
        o->internal = vw;
        EMBI_JS_WB_STRUCT(o);
        *out = embi_js_from_object(o);
    } else {
        if (kind >= EMBI_JS_TA_KINDS) {
            return cl_de_fail(rt);
        }
        embi_js_value ctor = embi_js_binary_intrinsic(
            rt, (enum embi_js_bin_intr)(EMBI_JS_BI_TA_CTOR_BASE + kind));
        embi_js_value argv[3] = {bufv, embi_js_number((double)off), embi_js_number((double)len)};
        int argc = tracking ? 2 : 3;
        embi_js_value result = embi_js_construct(rt, ctor, argc, argv, ctor);
        if (result == EMBI_JS_EXCEPTION) {
            return false;
        }
        *out = result;
    }
    return cl_de_register(d, *out);
}

static bool cl_de_coll(cl_de *d, bool is_map, embi_js_value *out)
{
    embi_js_runtime *rt = d->rt;
    embi_js_value ctor = embi_js_coll_intrinsic(
        d->r, is_map ? EMBI_JS_COLL_MAP_CTOR : EMBI_JS_COLL_SET_CTOR);
    embi_js_value adder = embi_js_coll_intrinsic(
        d->r, is_map ? EMBI_JS_COLL_MAP_SET : EMBI_JS_COLL_SET_ADD);
    embi_js_value coll = embi_js_construct(rt, ctor, 0, nullptr, ctor);
    if (coll == EMBI_JS_EXCEPTION || !cl_de_register(d, coll)) {
        return false;
    }
    uint32_t n = 0;
    if (!cl_de_u32(d, &n) || n > d->end - d->pos) {
        return cl_de_fail(rt);
    }
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value args[2] = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED};
        if (!cl_de_value(d, &args[0])) {
            return false;
        }
        int argc = 1;
        if (is_map) {
            if (!cl_de_value(d, &args[1])) {
                return false;
            }
            argc = 2;
        }
        if (embi_js_call(rt, adder, coll, argc, args) == EMBI_JS_EXCEPTION) {
            return false;
        }
    }
    *out = coll;
    return true;
}

static bool cl_de_value_body(cl_de *d, uint8_t tag, embi_js_value *out)
{
    embi_js_runtime *rt = d->rt;
    switch (tag) {
    case CL_UNDEFINED:
        *out = EMBI_JS_UNDEFINED;
        return true;
    case CL_NULL:
        *out = EMBI_JS_NULL;
        return true;
    case CL_TRUE:
        *out = EMBI_JS_TRUE;
        return true;
    case CL_FALSE:
        *out = EMBI_JS_FALSE;
        return true;
    case CL_INT32: {
        uint32_t u = 0;
        if (!cl_de_u32(d, &u)) {
            return cl_de_fail(rt);
        }
        int32_t iv = 0;
        memcpy(&iv, &u, sizeof iv);
        *out = embi_js_from_int32(iv);
        return true;
    }
    case CL_DOUBLE: {
        double dv = 0;
        if (!cl_de_f64(d, &dv)) {
            return cl_de_fail(rt);
        }
        *out = embi_js_number(dv);
        return true;
    }
    case CL_STRING:
        return cl_de_string(d, out);
    case CL_BIGINT: {
        embi_js_value txt = EMBI_JS_UNDEFINED;
        if (!cl_de_string(d, &txt)) {
            return false;
        }
        embi_js_bigint *b = nullptr;
        if (!embi_js_string_to_bigint(rt, embi_js_as_string(txt), &b)) {
            return cl_de_fail(rt);
        }
        *out = embi_js_from_bigint(b);
        return true;
    }
    case CL_BOOL_OBJ: {
        uint8_t b = 0;
        if (!cl_de_u8(d, &b) || b > 1) {
            return cl_de_fail(rt);
        }
        embi_js_object *w = embi_js_primitive_wrapper_new(rt, embi_js_from_bool(b != 0));
        *out = embi_js_from_object(w);
        return cl_de_register(d, *out);
    }
    case CL_NUMBER_OBJ: {
        double dv = 0;
        if (!cl_de_f64(d, &dv)) {
            return cl_de_fail(rt);
        }
        embi_js_object *w = embi_js_primitive_wrapper_new(rt, embi_js_number(dv));
        *out = embi_js_from_object(w);
        return cl_de_register(d, *out);
    }
    case CL_STRING_OBJ: {
        embi_js_value str = EMBI_JS_UNDEFINED;
        if (!cl_de_string(d, &str)) {
            return false;
        }
        embi_js_object *w = embi_js_primitive_wrapper_new(rt, str);
        *out = embi_js_from_object(w);
        return cl_de_register(d, *out);
    }
    case CL_BIGINT_OBJ: {
        embi_js_value txt = EMBI_JS_UNDEFINED;
        if (!cl_de_string(d, &txt)) {
            return false;
        }
        embi_js_bigint *b = nullptr;
        if (!embi_js_string_to_bigint(rt, embi_js_as_string(txt), &b)) {
            return cl_de_fail(rt);
        }
        embi_js_object *w = embi_js_bigint_wrapper_new(rt, embi_js_from_bigint(b));
        *out = embi_js_from_object(w);
        return cl_de_register(d, *out);
    }
    case CL_PLATFORM: {
        uint32_t n = 0;
        const unsigned char *b = nullptr;
        if (!cl_de_u32(d, &n) || !cl_de_take(d, n, &b) || g_host == nullptr || g_host->deserialize == nullptr) {
            return cl_de_fail(rt);
        }
        if (!g_host->deserialize(d->r, (embi_str){(const char *)b, n}, out)) {
            return false;
        }
        return cl_de_register(d, *out);
    }
    case CL_DATE: {
        double tv = 0;
        if (!cl_de_f64(d, &tv)) {
            return cl_de_fail(rt);
        }
        embi_js_value ctor = EMBI_JS_UNDEFINED;
        if (!cl_de_date_ctor(d, &ctor)) {
            return false;
        }
        embi_js_value arg = embi_js_from_double(tv);
        *out = embi_js_construct(rt, ctor, 1, &arg, ctor);
        if (*out == EMBI_JS_EXCEPTION) {
            return false;
        }
        return cl_de_register(d, *out);
    }
    case CL_REGEXP: {
        embi_js_value args[2] = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED};
        if (!cl_de_string(d, &args[0]) || !cl_de_string(d, &args[1])) {
            return false;
        }
        embi_js_value ctor = rt->protos[EMBI_JS_PROTO_REGEXP_CTOR];
        *out = embi_js_construct(rt, ctor, 2, args, ctor);
        if (*out == EMBI_JS_EXCEPTION) {
            return false;
        }
        return cl_de_register(d, *out);
    }
    case CL_ARRAYBUFFER: {
        uint8_t resizable = 0;
        uint64_t max = 0;
        uint64_t len = 0;
        if (!cl_de_u8(d, &resizable) || !cl_de_u64(d, &max) || !cl_de_u64(d, &len)) {
            return cl_de_fail(rt);
        }
        if (resizable > 1 || len > max || max > EMBI_JS_ABUF_MAX_BYTES ||
            (resizable == 0 && max != len) || len > d->end - d->pos) {
            return cl_de_fail(rt);
        }
        embi_js_object *ab = embi_js_arraybuffer_alloc(
            rt, embi_js_binary_intrinsic(rt, EMBI_JS_BI_ARRAYBUFFER_PROTO), (size_t)len,
            (size_t)max, resizable != 0 ? EMBI_JS_ABUF_RESIZABLE : 0);
        if (ab == nullptr) {
            return false;
        }
        if (len != 0) {
            embi_js_abuf *b = embi_js_abuf_of(ab);
            if (b->data == nullptr) {
                return cl_de_fail(rt);
            }
            memcpy(b->data, d->p + d->pos, (size_t)len);
            d->pos += (size_t)len;
        }
        *out = embi_js_from_object(ab);
        return cl_de_register(d, *out);
    }
    case CL_VIEW:
        return cl_de_view(d, out);
    case CL_MAP:
        return cl_de_coll(d, true, out);
    case CL_SET:
        return cl_de_coll(d, false, out);
    case CL_ERROR:
        return cl_de_error(d, out);
    case CL_OBJECT: {
        embi_js_object *o = embi_js_object_new_plain(rt);
        *out = embi_js_from_object(o);
        return cl_de_register(d, *out) && cl_de_props(d, o);
    }
    case CL_ARRAY: {
        uint32_t len = 0;
        if (!cl_de_u32(d, &len)) {
            return cl_de_fail(rt);
        }
        embi_js_object *a = embi_js_array_create(rt, (double)len, rt->protos[EMBI_JS_PROTO_ARRAY]);
        *out = embi_js_from_object(a);
        return cl_de_register(d, *out) && cl_de_props(d, a);
    }
    case CL_XFERRED: {
        uint32_t idx = 0;
        if (!cl_de_u32(d, &idx) || idx >= d->nxfer) {
            return cl_de_fail(rt);
        }
        *out = embi_js_obj_get(rt, d->xfer, embi_js_key_from_index(idx), embi_js_from_object(d->xfer));
        return *out != EMBI_JS_EXCEPTION && cl_de_register(d, *out);
    }
    case CL_BACKREF: {
        uint32_t idx = 0;
        if (!cl_de_u32(d, &idx) || idx >= d->count) {
            return cl_de_fail(rt);
        }
        *out = embi_js_obj_get(rt, d->objs, embi_js_key_from_index(idx),
                               embi_js_from_object(d->objs));
        return *out != EMBI_JS_EXCEPTION;
    }
    default:
        return cl_de_fail(rt);
    }
}

/* True for the tags the serializer writes for an object (the ones that open a level). Primitives
 * and backreferences do not, which is what cl_ser_object counts. */
static bool cl_tag_opens_level(uint8_t tag)
{
    switch (tag) {
    case CL_BOOL_OBJ:
    case CL_NUMBER_OBJ:
    case CL_STRING_OBJ:
    case CL_BIGINT_OBJ:
    case CL_DATE:
    case CL_REGEXP:
    case CL_ARRAYBUFFER:
    case CL_VIEW:
    case CL_MAP:
    case CL_SET:
    case CL_ERROR:
    case CL_OBJECT:
    case CL_ARRAY:
    case CL_PLATFORM:
        return true;
    default:
        return false;
    }
}

static bool cl_de_value(cl_de *d, embi_js_value *out)
{
    uint8_t tag = 0;
    if (!cl_de_u8(d, &tag)) {
        return cl_de_fail(d->rt);
    }
    if (!cl_tag_opens_level(tag)) {
        return cl_de_value_body(d, tag, out);
    }
    if (d->depth >= EMBI_CLONE_MAX_DEPTH) {
        return cl_de_fail(d->rt);
    }
    d->depth++;
    bool ok = cl_de_value_body(d, tag, out);
    d->depth--;
    return ok;
}

static bool cl_deserialize(embi_js_realm *r, embi_str bytes, embi_js_value *out, embi_js_value *transferred)
{
    embi_js_runtime *rt = r->rt;
    if (bytes.len < 5 || memcmp(bytes.ptr, CL_MAGIC, 4) != 0 ||
        (uint8_t)bytes.ptr[4] != EMBI_CLONE_VERSION) {
        return cl_de_fail(rt);
    }
    cl_de d;
    memset(&d, 0, sizeof d);
    d.r = r;
    d.rt = rt;
    d.p = (const unsigned char *)bytes.ptr;
    d.end = bytes.len;
    d.pos = 5;
    d.objs = embi_js_array_create_default(rt, 0);
    if (d.pos < d.end && d.p[d.pos] == CL_XFER_TABLE) {
        uint32_t n = 0;
        d.pos++;
        if (!cl_de_u32(&d, &n) || g_host == nullptr || g_host->transfer_receive == nullptr || n > d.end - d.pos) {
            return cl_de_fail(rt);
        }
        d.xfer = embi_js_array_create_default(rt, 0);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t len = 0;
            const unsigned char *b = nullptr;
            embi_js_value obj = EMBI_JS_UNDEFINED;
            if (!cl_de_u32(&d, &len) || !cl_de_take(&d, len, &b)) {
                return cl_de_fail(rt);
            }
            if (!g_host->transfer_receive(r, (embi_str){(const char *)b, len}, &obj)) {
                return false;
            }
            if (!cl_push(rt, d.xfer, d.nxfer, obj)) {
                return false;
            }
            d.nxfer++;
        }
    }
    embi_js_value v = EMBI_JS_UNDEFINED;
    if (!cl_de_value(&d, &v)) {
        return false;
    }
    if (d.pos != d.end) {
        return cl_de_fail(rt);
    }
    *out = v;
    if (transferred != nullptr) {
        *transferred = d.xfer != nullptr ? embi_js_from_object(d.xfer) : EMBI_JS_UNDEFINED;
    }
    return true;
}

/* The engine entry points run inside embi_js_enter/embi_js_leave, as host.c does, so the collector
 * knows the native stack range while the clone runs. Nested calls from script are fine. */
bool embi_clone_serialize(embi_js_realm *r, embi_js_value v, const embi_js_value *transfer,
                          uint32_t ntransfer, embi_sb *out)
{
    embi_js_enter(r->rt);
    bool ok = cl_serialize(r, v, transfer, ntransfer, out);
    embi_js_leave(r->rt);
    return ok;
}

bool embi_clone_deserialize_ex(embi_js_realm *r, embi_str bytes, embi_js_value *out, embi_js_value *transferred)
{
    embi_js_enter(r->rt);
    bool ok = cl_deserialize(r, bytes, out, transferred);
    embi_js_leave(r->rt);
    return ok;
}

bool embi_clone_deserialize(embi_js_realm *r, embi_str bytes, embi_js_value *out)
{
    return embi_clone_deserialize_ex(r, bytes, out, nullptr);
}
