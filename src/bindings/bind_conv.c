/*
 * Binding runtime (W5a): strings, WebIDL conversions, overload resolution, errors, arrays and
 * promises built from C, reflected attributes and the generic iterable machinery.
 */
#include "bindings/bind.h"
#include "bindings/gen/types.h"
#include "js/heap.h"
#include "js/object.h"
#include "js/promise.h"
#include "js/runtime.h"

#include <stdint.h>

/* ------------------------------------------------------------------ strings */

embi_dstr embi_bind_tmp_units(embi_bind_ctx *c, const uint16_t *u, uint32_t n)
{
    if (n == 0) {
        return embi_dstr_empty();
    }
    bool wide = false;
    for (uint32_t i = 0; i < n; i++) {
        if (u[i] > 0xFF) {
            wide = true;
            break;
        }
    }
    if (wide) {
        uint16_t *m = embi_arena_alloc(&c->arena, (size_t)n * 2, 2);
        SDL_memcpy(m, u, (size_t)n * 2);
        return (embi_dstr){m, n, true};
    }
    uint8_t *m = embi_arena_alloc(&c->arena, n, 1);
    for (uint32_t i = 0; i < n; i++) {
        m[i] = (uint8_t)u[i];
    }
    return (embi_dstr){m, n, false};
}

embi_bind_union *embi_bind_union_array(embi_bind_ctx *c, int n)
{
    if (n <= 0) {
        return nullptr;
    }
    return embi_arena_alloc(&c->arena, sizeof(embi_bind_union) * (size_t)n, 8);
}

embi_dstr embi_bind_tmp_utf8(embi_bind_ctx *c, embi_str text)
{
    bool wide = false;
    uint32_t n = embi_utf8_units(text, &wide);
    if (n == 0) {
        return embi_dstr_empty();
    }
    size_t bytes = (size_t)n * (wide ? 2u : 1u);
    void *mem = embi_arena_alloc(&c->arena, bytes, 2);
    embi_utf8_write(mem, wide, text);
    return (embi_dstr){mem, n, wide};
}

embi_dstr embi_bind_tmp_copy(embi_bind_ctx *c, embi_dstr d)
{
    if (d.len == 0) {
        return embi_dstr_empty();
    }
    size_t bytes = (size_t)d.len * (d.wide ? 2u : 1u);
    void *mem = embi_arena_alloc(&c->arena, bytes, 2);
    embi_copy_bytes(mem, d.ptr, bytes);
    return (embi_dstr){mem, d.len, d.wide};
}

embi_str embi_bind_utf8(embi_bind_ctx *c, embi_dstr d)
{
    if (d.len == 0) {
        return (embi_str){"", 0};
    }
    size_t cap = (size_t)d.len * 3u + 1u;
    char *buf = embi_arena_alloc(&c->arena, cap, 1);
    size_t n = embi_dstr_to_utf8(d, buf, cap);
    return (embi_str){buf, (uint32_t)n};
}

static embi_js_value str_value(embi_js_runtime *rt, const void *ptr, uint32_t len, bool wide)
{
    if (len == 0) {
        return embi_js_from_string(embi_js_str_new_utf8(rt, "", 0));
    }
    if (wide) {
        return embi_js_from_string(embi_js_str_new_utf16(rt, ptr, len));
    }
    return embi_js_from_string(embi_js_str_new_latin1(rt, ptr, len));
}

embi_js_value embi_bind_dstr_value(embi_bind_ctx *c, embi_dstr d)
{
    return str_value(c->rt, d.ptr, d.len, d.wide);
}

embi_js_value embi_bind_utf8_value(embi_bind_ctx *c, embi_str s)
{
    return embi_js_from_string(embi_js_str_new_utf8(c->rt, s.ptr ? s.ptr : "", s.len));
}

/* The DOMString of a JS value (ToString). The flat string is pinned, so its storage stays valid
 * for the rest of the scope. */
bool embi_bind_to_dstr(embi_bind_ctx *c, embi_js_value v, embi_dstr *out)
{
    embi_js_runtime *rt = c->rt;
    if (embi_js_is_symbol(v)) {
        embi_js_throw_type_error(rt, "Cannot convert a Symbol value to a string");
        return false;
    }
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    s = embi_js_str_flat(rt, embi_js_from_string(s));
    if (!s) {
        return false;
    }
    embi_bind_pin(c, embi_js_from_string(s));
    uint32_t n = embi_js_str_len(s);
    bool wide = embi_js_str_is_wide(s);
    out->len = n;
    out->wide = wide;
    if (n == 0) {
        out->ptr = nullptr;
    } else {
        out->ptr = wide ? (const void *)embi_js_str_wide(s) : (const void *)embi_js_str_latin1(s);
    }
    return true;
}

/* ------------------------------------------------------------------ errors */

static const struct {
    const char *name;
    uint16_t code;
} k_legacy[] = {
    {"IndexSizeError", 1},          {"HierarchyRequestError", 3},
    {"WrongDocumentError", 4},      {"InvalidCharacterError", 5},
    {"NoModificationAllowedError", 7}, {"NotFoundError", 8},
    {"NotSupportedError", 9},       {"InUseAttributeError", 10},
    {"InvalidStateError", 11},      {"SyntaxError", 12},
    {"InvalidModificationError", 13}, {"NamespaceError", 14},
    {"InvalidAccessError", 15},     {"TypeMismatchError", 17},
    {"SecurityError", 18},          {"NetworkError", 19},
    {"AbortError", 20},             {"URLMismatchError", 21},
    {"QuotaExceededError", 22},     {"TimeoutError", 23},
    {"InvalidNodeTypeError", 24},   {"DataCloneError", 25},
};

uint16_t embi_bind_legacy_code(embi_dstr name)
{
    for (size_t i = 0; i < sizeof k_legacy / sizeof k_legacy[0]; i++) {
        if (name.len == SDL_strlen(k_legacy[i].name) && name.len) {
            bool same = true;
            for (uint32_t k = 0; k < name.len && same; k++) {
                same = embi_dstr_unit(name, k) == (uint8_t)k_legacy[i].name[k];
            }
            if (same) {
                return k_legacy[i].code;
            }
        }
    }
    return 0;
}

embi_js_value embi_bind_dom_exception(embi_bind_ctx *c, uint16_t code, embi_dstr name,
                                      embi_dstr message)
{
    size_t name_bytes = (size_t)name.len * (name.wide ? 2u : 1u);
    size_t msg_off = (sizeof(embi_bind_dom_exc) + name_bytes + 7u) & ~(size_t)7u;
    size_t msg_bytes = (size_t)message.len * (message.wide ? 2u : 1u);
    size_t total = msg_off + msg_bytes;
    uint8_t *block = embi_calloc(1, total);
    embi_bind_dom_exc *h = (embi_bind_dom_exc *)block;
    h->code = code;
    if (name.len) {
        SDL_memcpy(block + sizeof *h, name.ptr, name_bytes);
    }
    if (message.len) {
        SDL_memcpy(block + msg_off, message.ptr, msg_bytes);
    }
    h->name = (embi_dstr){name.len ? block + sizeof *h : nullptr, name.len, name.wide};
    h->message = (embi_dstr){message.len ? block + msg_off : nullptr, message.len, message.wide};
    return embi_bind_other_value(c, EMBI_IF_DOMException, h, block, (uint32_t)total);
}

embi_js_value embi_bind_throw_named(embi_bind_ctx *c, const char *name, const char *msg)
{
    embi_dstr nd = {name, (uint32_t)SDL_strlen(name), false};
    embi_dstr md = {msg, (uint32_t)SDL_strlen(msg), false};
    return embi_js_throw_value(c->rt, embi_bind_dom_exception(c, embi_bind_legacy_code(nd), nd, md));
}

embi_js_value embi_bind_throw_dom(embi_bind_ctx *c, embi_dom_err err)
{
    if ((int)err < 0) {
        return EMBI_JS_EXCEPTION;
    }
    switch (err) {
    case EMBI_DOM_ERR_HIERARCHY:
        return embi_bind_throw_named(c, "HierarchyRequestError",
                                     "The operation would yield an incorrect node tree.");
    case EMBI_DOM_ERR_NOT_FOUND:
        return embi_bind_throw_named(c, "NotFoundError", "The object can not be found here.");
    case EMBI_DOM_ERR_INVALID_CHAR:
        return embi_bind_throw_named(c, "InvalidCharacterError",
                                     "The string contains invalid characters.");
    case EMBI_DOM_ERR_NAMESPACE:
        return embi_bind_throw_named(c, "NamespaceError",
                                     "The operation is not allowed by Namespaces in XML.");
    case EMBI_DOM_ERR_NOT_SUPPORTED:
        return embi_bind_throw_named(c, "NotSupportedError", "The operation is not supported.");
    default:
        return embi_bind_throw_named(c, "InvalidStateError", "The object is in an invalid state.");
    }
}

const char *embi_bind_ctx_text(embi_bind_ctx *c, const char *ctx)
{
    if (ctx[0] != 1) {
        return ctx;
    }
    const embi_bind_member *m = c->site;
    if (!m) {
        return "Failed to execute";
    }
    const char *iface = (m->flags & EMBI_MF_NS) ? embi_bind_ns_names[m->iface] : embi_bind_names[m->iface];
    if (ctx[1] == 's') {
        SDL_snprintf(c->ctx_buf, sizeof c->ctx_buf, "Failed to set the '%s' property on '%s'", m->name, iface);
    } else if (ctx[1] == 'c') {
        int arg = SDL_atoi(ctx + 2);
        if (arg > 0) {
            SDL_snprintf(c->ctx_buf, sizeof c->ctx_buf, "Failed to construct '%s': parameter %d", iface, arg);
        } else {
            SDL_snprintf(c->ctx_buf, sizeof c->ctx_buf, "Failed to construct '%s'", iface);
        }
    } else {
        int arg = SDL_atoi(ctx + 2);
        if (arg > 0) {
            SDL_snprintf(c->ctx_buf, sizeof c->ctx_buf, "Failed to execute '%s' on '%s': parameter %d", m->name, iface,
                         arg);
        } else {
            SDL_snprintf(c->ctx_buf, sizeof c->ctx_buf, "Failed to execute '%s' on '%s'", m->name, iface);
        }
    }
    return c->ctx_buf;
}

embi_js_value embi_bind_throw_args(embi_bind_ctx *c, const char *ctx, int required, int argc)
{
    return embi_js_throw_type_error(c->rt, "%s: %d argument%s required, but only %d present.", embi_bind_ctx_text(c, ctx),
                                    required, required == 1 ? "" : "s", argc);
}

embi_js_value embi_bind_enum_value(embi_bind_ctx *c, const char *const *names, int count, int32_t v)
{
    if (v < 0 || v >= count) {
        return EMBI_JS_NULL;
    }
    return embi_js_from_string(embi_js_str_new_utf8(c->rt, names[v], SDL_strlen(names[v])));
}

/* ------------------------------------------------------------------ arrays, objects, promises */

embi_js_value embi_bind_array_new(embi_bind_ctx *c, uint32_t len)
{
    embi_js_object *a = embi_js_array_create_default(c->rt, (double)len);
    return embi_js_from_object(a);
}

void embi_bind_array_set(embi_bind_ctx *c, embi_js_value arr, uint32_t i, embi_js_value v)
{
    if (embi_js_obj_create_index_fast(c->rt, embi_js_as_object(arr), i, v)) {
        return;
    }
    (void)embi_js_create_data_property(c->rt, embi_js_as_object(arr), embi_js_key_from_index(i), v);
}

embi_js_value embi_bind_array_get(embi_bind_ctx *c, embi_js_value arr, uint32_t i)
{
    embi_js_value v;
    if (embi_js_obj_index_get_fast(c->rt, embi_js_as_object(arr), i, &v)) {
        return v;
    }
    v = embi_js_get(c->rt, embi_js_as_object(arr), embi_js_key_from_index(i));
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_UNDEFINED;
    }
    return v;
}

uint32_t embi_bind_array_len(embi_bind_ctx *c, embi_js_value arr)
{
    uint32_t n = 0;
    if (embi_js_is_object(arr) && embi_js_array_length_fast(c->rt, embi_js_as_object(arr), &n)) {
        return n;
    }
    double d = 0;
    if (!embi_js_is_object(arr) || !embi_js_length_of_array_like(c->rt, embi_js_as_object(arr), &d)) {
        embi_js_exception_clear(c->rt);
        return 0;
    }
    return d > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)d;
}

embi_js_value embi_bind_object_new(embi_bind_ctx *c)
{
    return embi_js_from_object(embi_js_object_new_plain(c->rt));
}

void embi_bind_object_set_c(embi_bind_ctx *c, embi_js_value obj, const char *key, embi_js_value v)
{
    embi_js_runtime *rt = c->rt;
    (void)embi_js_create_data_property(rt, embi_js_as_object(obj), embi_js_key_c(rt, key), v);
}

void embi_bind_object_set(embi_bind_ctx *c, embi_js_value obj, embi_dstr key, embi_js_value v)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value k = embi_bind_dstr_value(c, key);
    embi_js_key kk = embi_js_to_property_key(rt, k);
    (void)embi_js_create_data_property(rt, embi_js_as_object(obj), kk, v);
}

embi_js_value embi_bind_promise_resolved(embi_bind_ctx *c, embi_js_value v)
{
    embi_js_object *p = embi_js_promise_new(c->rt);
    embi_js_value pv = embi_js_from_object(p);
    embi_bind_pin(c, pv);
    (void)embi_js_promise_resolve_with(c->rt, p, v);
    return pv;
}

embi_js_value embi_bind_promise_rejected(embi_bind_ctx *c, embi_js_value reason)
{
    embi_js_object *p = embi_js_promise_new(c->rt);
    embi_js_value pv = embi_js_from_object(p);
    embi_bind_pin(c, pv);
    (void)embi_js_promise_reject_with(c->rt, p, reason);
    return pv;
}

embi_js_value embi_bind_promise_from_pending(embi_bind_ctx *c)
{
    embi_js_value ex = embi_js_exception_get(c->rt);
    embi_bind_pin(c, ex);
    embi_js_exception_clear(c->rt);
    return embi_bind_promise_rejected(c, ex);
}

/* ------------------------------------------------------------------ integer conversion */

#define TWO63 9223372036854775808.0
#define TWO64 18446744073709551616.0

static double round_half_even(double x)
{
    double r = SDL_floor(x + 0.5);
    if (r - x == 0.5 && SDL_fmod(r, 2.0) != 0.0) {
        r -= 1.0;
    }
    return r;
}

bool embi_bind_conv_int(embi_bind_ctx *c, embi_js_value v, int kind, const char *ctx, int64_t *out)
{
    double x = 0;
    if (!embi_js_to_number(c->rt, v, &x)) {
        return false;
    }
    static const int bits[8] = {8, 16, 32, 64, 8, 16, 32, 64};
    static const bool sgn[8] = {true, true, true, true, false, false, false, false};
    bool clamp = (kind & 0x10) != 0;
    bool enforce = (kind & 0x20) != 0;
    int k = kind & 0x0F;
    int b = bits[k];
    bool s = sgn[k];
    bool finite = (x - x) == 0.0;
    /* The range of the type as doubles. The 64-bit upper bounds (2^63 - 1, 2^64 - 1) are not
     * doubles, so they are tested as "below 2^63" and "below 2^64". */
    double lo, hi_excl;
    if (b < 64) {
        lo = s ? -(double)(1ll << (b - 1)) : 0.0;
        hi_excl = s ? (double)(1ll << (b - 1)) : (double)(1ll << b);
    } else {
        lo = s ? -TWO63 : 0.0;
        hi_excl = s ? TWO63 : TWO64;
    }
    if (clamp) {
        if (x != x) {
            x = 0;
        }
        if (x < 0 && !((x - x) == 0.0)) { /* -Infinity */
            x = lo;
        } else if (x >= hi_excl) {
            x = hi_excl;
        } else {
            x = round_half_even(x);
        }
        if (x < lo) {
            x = lo;
        }
        if (x >= hi_excl) {
            /* The maximum of the type. */
            if (b == 64) {
                *out = s ? INT64_MAX : (int64_t)UINT64_MAX;
            } else {
                *out = (int64_t)hi_excl - 1;
            }
            return true;
        }
        *out = (b == 64 && !s && x >= TWO63) ? (int64_t)(uint64_t)x : (int64_t)x;
        return true;
    }
    if (enforce) {
        if (!finite) {
            embi_js_throw_type_error(c->rt, "%s is outside the accepted range.", embi_bind_ctx_text(c, ctx));
            return false;
        }
        x = SDL_trunc(x);
        if (x < lo || x >= hi_excl) {
            embi_js_throw_type_error(c->rt, "%s is outside the accepted range.", embi_bind_ctx_text(c, ctx));
            return false;
        }
        *out = (b == 64 && !s && x >= TWO63) ? (int64_t)(uint64_t)x : (int64_t)x;
        return true;
    }
    /* ConvertToInt: x modulo 2^b, exactly. Below 2^63 the integer part is an exact int64 and the
     * two's complement wrap gives the residue; beyond, fmod(x, 2^64) is exact. */
    uint64_t u = 0;
    if (finite && x != 0.0) {
        x = SDL_trunc(x);
        double ax = x < 0 ? -x : x;
        if (ax < TWO63) {
            u = (uint64_t)(int64_t)x;
        } else {
            uint64_t r = (uint64_t)SDL_fmod(ax, TWO64);
            u = x < 0 ? (uint64_t)0 - r : r;
        }
    }
    if (b < 64) {
        uint64_t mask = (1ull << b) - 1;
        u &= mask;
        if (s && u >= (1ull << (b - 1))) {
            *out = (int64_t)u - (int64_t)(1ull << b);
        } else {
            *out = (int64_t)u;
        }
    } else {
        *out = (int64_t)u;
    }
    return true;
}

bool embi_bind_conv_double(embi_bind_ctx *c, embi_js_value v, bool unrestricted, const char *ctx,
                           double *out)
{
    double x = 0;
    if (!embi_js_to_number(c->rt, v, &x)) {
        return false;
    }
    if (!unrestricted && (x - x) != 0.0) {
        embi_js_throw_type_error(c->rt, "%s is not a finite floating-point value.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    *out = x;
    return true;
}

bool embi_bind_conv_object(embi_bind_ctx *c, embi_js_value v, bool nullable, const char *ctx,
                           embi_js_value *out)
{
    if (nullable && embi_js_is_nullish(v)) {
        *out = EMBI_JS_NULL;
        return true;
    }
    if (!embi_js_is_object(v)) {
        embi_js_throw_type_error(c->rt, "%s is not an object.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    *out = v;
    return true;
}

bool embi_bind_conv_callback(embi_bind_ctx *c, embi_js_value v, bool nullable, bool iface_obj,
                             const char *ctx, embi_js_value *out)
{
    if (nullable && embi_js_is_nullish(v)) {
        *out = EMBI_JS_NULL;
        return true;
    }
    if (embi_js_is_callable(v) || (iface_obj && embi_js_is_object(v))) {
        *out = v;
        return true;
    }
    embi_js_throw_type_error(c->rt, "%s is not a function.", embi_bind_ctx_text(c, ctx));
    return false;
}

bool embi_bind_conv_str(embi_bind_ctx *c, embi_js_value v, const char *ctx, bool legacy_nte,
                        embi_dstr *out)
{
    (void)ctx;
    if (legacy_nte && v == EMBI_JS_NULL) {
        *out = embi_dstr_empty();
        return true;
    }
    return embi_bind_to_dstr(c, v, out);
}

bool embi_bind_conv_nstr(embi_bind_ctx *c, embi_js_value v, const char *ctx, bool legacy_nte,
                         embi_bind_nstr *out)
{
    out->is_null = false;
    if (embi_js_is_nullish(v)) {
        out->is_null = true;
        out->s = embi_dstr_empty();
        return true;
    }
    return embi_bind_conv_str(c, v, ctx, legacy_nte, &out->s);
}

bool embi_bind_conv_enum(embi_bind_ctx *c, embi_js_value v, bool nullable,
                         const char *const *names, int count, const char *ctx, int32_t *out)
{
    if (nullable && embi_js_is_nullish(v)) {
        *out = -1;
        return true;
    }
    embi_dstr d = embi_dstr_empty();
    if (!embi_bind_to_dstr(c, v, &d)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (embi_dstr_eq_utf8(d, (embi_str){names[i], (uint32_t)SDL_strlen(names[i])})) {
            *out = i;
            return true;
        }
    }
    embi_str got = embi_bind_utf8(c, d);
    embi_js_throw_type_error(c->rt, "%s: '%.*s' is not a valid enumeration value.", embi_bind_ctx_text(c, ctx),
                             (int)(got.len > 64 ? 64 : got.len), got.ptr);
    return false;
}

bool embi_bind_conv_node(embi_bind_ctx *c, embi_js_value v, int iface, bool nullable,
                         const char *ctx, embi_node **out)
{
    if (nullable && embi_js_is_nullish(v)) {
        *out = nullptr;
        return true;
    }
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (!bo || !bo->is_node || !bo->ptr || !embi_bind_iface_is(bo->iface, iface)) {
        embi_js_throw_type_error(c->rt, "%s is not of type '%s'.", embi_bind_ctx_text(c, ctx), embi_bind_names[iface]);
        return false;
    }
    *out = bo->ptr;
    return true;
}

bool embi_bind_conv_other(embi_bind_ctx *c, embi_js_value v, int iface, bool nullable,
                          const char *ctx, void **out)
{
    if (nullable && embi_js_is_nullish(v)) {
        *out = nullptr;
        return true;
    }
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (!bo || !bo->ptr || !embi_bind_iface_is(bo->iface, iface)) {
        embi_js_throw_type_error(c->rt, "%s is not of type '%s'.", embi_bind_ctx_text(c, ctx), embi_bind_names[iface]);
        return false;
    }
    *out = bo->ptr;
    return true;
}

/* ------------------------------------------------------------------ sequences and records */

static bool is_iterable_obj(embi_bind_ctx *c, embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        return false;
    }
    embi_js_runtime *rt = c->rt;
    embi_js_value m = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (m == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        return false;
    }
    return !embi_js_is_undefined(m);
}

bool embi_bind_seq_open(embi_bind_ctx *c, embi_js_value v, const char *ctx, embi_js_value **items,
                        uint32_t *n)
{
    embi_js_runtime *rt = c->rt;
    *items = nullptr;
    *n = 0;
    if (!embi_js_is_object(v)) {
        embi_js_throw_type_error(rt, "%s: The provided value cannot be converted to a sequence.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    embi_js_value m = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (m == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (embi_js_is_undefined(m)) {
        embi_js_throw_type_error(rt, "%s: The object must have a callable @@iterator property.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    embi_js_value it = embi_js_call(rt, m, v, 0, nullptr);
    if (it == EMBI_JS_EXCEPTION) {
        return false;
    }
    embi_bind_pin(c, it);
    if (!embi_js_is_object(it)) {
        embi_js_throw_type_error(rt, "%s: The iterator is not an object.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    embi_js_value next = embi_js_get_v(rt, it, embi_js_key_c(rt, "next"));
    if (next == EMBI_JS_EXCEPTION) {
        return false;
    }
    embi_bind_pin(c, next);
    uint32_t cap = 8, len = 0;
    embi_js_value *buf = (embi_js_value *)embi_bind_alloc(c, cap * sizeof *buf);
    for (;;) {
        embi_js_value r = embi_js_call(rt, next, it, 0, nullptr);
        if (r == EMBI_JS_EXCEPTION) {
            return false;
        }
        embi_bind_pin(c, r);
        if (!embi_js_is_object(r)) {
            embi_js_throw_type_error(rt, "%s: The iterator result is not an object.", embi_bind_ctx_text(c, ctx));
            return false;
        }
        embi_js_value done = embi_js_get_v(rt, r, embi_js_key_c(rt, "done"));
        if (done == EMBI_JS_EXCEPTION) {
            return false;
        }
        if (embi_js_to_boolean(done)) {
            break;
        }
        embi_js_value val = embi_js_get_v(rt, r, embi_js_key_c(rt, "value"));
        if (val == EMBI_JS_EXCEPTION) {
            return false;
        }
        embi_bind_pin(c, val);
        if (len == cap) {
            embi_js_value *nb = (embi_js_value *)embi_bind_alloc(c, cap * 2 * sizeof *nb);
            SDL_memcpy(nb, buf, len * sizeof *buf);
            buf = nb;
            cap *= 2;
        }
        buf[len++] = val;
    }
    *items = buf;
    *n = len;
    return true;
}

bool embi_bind_rec_open(embi_bind_ctx *c, embi_js_value v, const char *ctx, embi_dstr **keys,
                        embi_js_value **vals, uint32_t *n)
{
    embi_js_runtime *rt = c->rt;
    *keys = nullptr;
    *vals = nullptr;
    *n = 0;
    if (!embi_js_is_object(v)) {
        embi_js_throw_type_error(rt, "%s: The provided value is not of type 'record'.", embi_bind_ctx_text(c, ctx));
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    embi_js_key_vec ks = {0};
    if (embi_js_obj_own_keys(rt, o, &ks) != EMBI_JS_YES) {
        embi_vec_free(&ks);
        return false;
    }
    embi_dstr *kd = (embi_dstr *)embi_bind_alloc(c, (size_t)ks.len * sizeof *kd + 1);
    embi_js_value *vd = (embi_js_value *)embi_bind_alloc(c, (size_t)ks.len * sizeof *vd + 1);
    uint32_t cnt = 0;
    bool ok = true;
    for (uint32_t i = 0; i < ks.len && ok; i++) {
        embi_js_key k = ks.data[i];
        if (embi_js_is_symbol(k)) {
            continue;
        }
        embi_js_pdesc d;
        int r = embi_js_obj_get_own_property(rt, o, k, &d);
        if (r == EMBI_JS_THREW) {
            ok = false;
            break;
        }
        if (r != EMBI_JS_YES || !(d.attrs & EMBI_JS_ATTR_ENUMERABLE)) {
            continue;
        }
        embi_js_value kv = embi_bind_pin(c, embi_js_key_to_value(rt, k));
        embi_js_value val = embi_js_obj_get(rt, o, k, v);
        if (val == EMBI_JS_EXCEPTION) {
            ok = false;
            break;
        }
        embi_bind_pin(c, val);
        if (!embi_bind_to_dstr(c, kv, &kd[cnt])) {
            ok = false;
            break;
        }
        vd[cnt++] = val;
    }
    embi_vec_free(&ks);
    if (!ok) {
        return false;
    }
    *keys = kd;
    *vals = vd;
    *n = cnt;
    return true;
}

/* ------------------------------------------------------------------ unions and overloads */

bool embi_bind_arg_is(embi_bind_ctx *c, embi_js_value v, const embi_bind_um *m)
{
    if (embi_js_is_nullish(v) && m->nullable) {
        return true;
    }
    switch (m->cat) {
    case EMBI_UM_NODE: {
        embi_bind_obj *bo = embi_bind_obj_of(v);
        return bo && bo->is_node && embi_bind_iface_is(bo->iface, m->iface);
    }
    case EMBI_UM_IFACE: {
        embi_bind_obj *bo = embi_bind_obj_of(v);
        return bo && embi_bind_iface_is(bo->iface, m->iface);
    }
    case EMBI_UM_CALLBACK:
        return embi_js_is_callable(v);
    case EMBI_UM_SEQ:
        return is_iterable_obj(c, v);
    case EMBI_UM_OBJECT:
        return embi_js_is_object(v);
    case EMBI_UM_BOOL:
        return embi_js_is_bool(v);
    case EMBI_UM_NUM:
        return embi_js_is_number(v);
    default:
        return true;
    }
}

bool embi_bind_conv_union(embi_bind_ctx *c, embi_js_value v, const embi_bind_um *mem, int n,
                          bool nullable, const char *ctx, embi_bind_union *out)
{
    embi_js_runtime *rt = c->rt;
    SDL_memset(out, 0, sizeof *out);
    if (embi_js_is_nullish(v)) {
        if (nullable) {
            return true;
        }
        for (int i = 0; i < n; i++) {
            if (mem[i].nullable) { /* a dictionary member takes undefined and null */
                out->kind = (uint8_t)(i + 1);
                out->u.v = EMBI_JS_UNDEFINED;
                return true;
            }
        }
    }
    if (embi_js_is_object(v)) {
        embi_bind_obj *bo = embi_bind_obj_of(v);
        for (int i = 0; i < n; i++) {
            if (bo && (mem[i].cat == EMBI_UM_NODE || mem[i].cat == EMBI_UM_IFACE) &&
                embi_bind_iface_is(bo->iface, mem[i].iface) &&
                (mem[i].cat == EMBI_UM_IFACE || bo->is_node)) {
                out->kind = (uint8_t)(i + 1);
                if (mem[i].cat == EMBI_UM_NODE) {
                    out->u.node = bo->ptr;
                } else {
                    out->u.ptr = bo->ptr;
                }
                return true;
            }
        }
        static const uint8_t order[] = {EMBI_UM_CALLBACK, EMBI_UM_SEQ, EMBI_UM_OBJECT};
        for (size_t o = 0; o < sizeof order; o++) {
            for (int i = 0; i < n; i++) {
                if (mem[i].cat == order[o] && embi_bind_arg_is(c, v, &mem[i])) {
                    out->kind = (uint8_t)(i + 1);
                    out->u.v = v;
                    return true;
                }
            }
        }
    }
    int boolean = -1, numeric = -1, str = -1;
    for (int i = 0; i < n; i++) {
        if (mem[i].cat == EMBI_UM_BOOL && boolean < 0) {
            boolean = i;
        } else if (mem[i].cat == EMBI_UM_NUM && numeric < 0) {
            numeric = i;
        } else if (mem[i].cat == EMBI_UM_STR && str < 0) {
            str = i;
        }
    }
    if (boolean >= 0 && embi_js_is_bool(v)) {
        out->kind = (uint8_t)(boolean + 1);
        out->u.b = v == EMBI_JS_TRUE;
        return true;
    }
    bool use_num = numeric >= 0 && (embi_js_is_number(v) || str < 0);
    if (use_num) {
        out->kind = (uint8_t)(numeric + 1);
        if (mem[numeric].sub < 8) {
            int64_t t = 0;
            if (!embi_bind_conv_int(c, v, mem[numeric].sub, ctx, &t)) {
                return false;
            }
            out->u.i = t;
            out->u.num = (double)t;
            return true;
        }
        return embi_bind_conv_double(c, v, true, ctx, &out->u.num);
    }
    if (str >= 0) {
        out->kind = (uint8_t)(str + 1);
        return embi_bind_to_dstr(c, v, &out->u.str);
    }
    if (boolean >= 0) {
        out->kind = (uint8_t)(boolean + 1);
        out->u.b = embi_js_to_boolean(v);
        return true;
    }
    embi_js_throw_type_error(rt, "%s: The provided value is not of a permitted type.", embi_bind_ctx_text(c, ctx));
    return false;
}

embi_js_value embi_bind_dispatch(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, const embi_bind_overload *ov, int nov,
                                 const char *name)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_runtime *rt = r->rt;
    int cand[16];
    int nc = 0;
    for (int i = 0; i < nov && nc < 16; i++) {
        if (argc >= ov[i].min_args && (ov[i].max_args == 255 || argc <= ov[i].max_args)) {
            cand[nc++] = i;
        }
    }
    if (nc == 0) {
        return embi_js_throw_type_error(rt, "Failed to execute '%s': %d argument%s not valid.", name,
                                        argc, argc == 1 ? " is" : "s are");
    }
    if (nc == 1 || !c) {
        return ov[cand[0]].fn(r, this_v, argc, argv, 0);
    }
    /* The distinguishing argument: the first one whose category differs between candidates. */
    int d = -1;
    for (int a = 0; a < argc && a < 6 && d < 0; a++) {
        for (int k = 1; k < nc; k++) {
            const embi_bind_overload *x = &ov[cand[0]];
            const embi_bind_overload *y = &ov[cand[k]];
            embi_bind_um ux = a < x->ncat ? x->cats[a] : (embi_bind_um){EMBI_UM_ANY, 0, false, 0};
            embi_bind_um uy = a < y->ncat ? y->cats[a] : (embi_bind_um){EMBI_UM_ANY, 0, false, 0};
            if (ux.cat != uy.cat || ux.iface != uy.iface) {
                d = a;
                break;
            }
        }
    }
    if (d < 0) {
        return ov[cand[0]].fn(r, this_v, argc, argv, 0);
    }
    for (int rank = EMBI_UM_NODE; rank <= EMBI_UM_ANY; rank++) {
        for (int k = 0; k < nc; k++) {
            const embi_bind_overload *x = &ov[cand[k]];
            embi_bind_um u = d < x->ncat ? x->cats[d] : (embi_bind_um){EMBI_UM_ANY, 0, false, 0};
            if (u.cat == rank && embi_bind_arg_is(c, argv[d], &u)) {
                return x->fn(r, this_v, argc, argv, 0);
            }
        }
    }
    return embi_js_throw_type_error(
        rt, "Failed to execute '%s': No function was found that matched the signature provided.", name);
}

/* ------------------------------------------------------------------ dictionaries, forwarding */

bool embi_bind_dict_check(embi_bind_ctx *c, embi_js_value v, const char *ctx)
{
    if (embi_js_is_nullish(v) || embi_js_is_object(v)) {
        return true;
    }
    embi_js_throw_type_error(c->rt, "%s: The value is not of type 'dictionary'.", embi_bind_ctx_text(c, ctx));
    return false;
}

bool embi_bind_dict_get(embi_bind_ctx *c, embi_js_value v, const char *name, embi_js_value *out)
{
    embi_js_runtime *rt = c->rt;
    if (!embi_js_is_object(v)) {
        *out = EMBI_JS_UNDEFINED;
        return true;
    }
    embi_js_value r = embi_js_obj_get(rt, embi_js_as_object(v), embi_js_key_c(rt, name), v);
    if (r == EMBI_JS_EXCEPTION) {
        return false;
    }
    *out = embi_bind_pin(c, r);
    return true;
}

bool embi_bind_dict_required(embi_bind_ctx *c, const char *name, const char *ctx)
{
    embi_js_throw_type_error(c->rt, "%s: required member '%s' is undefined.", embi_bind_ctx_text(c, ctx), name);
    return false;
}

bool embi_bind_replace_prop(embi_bind_ctx *c, embi_js_value this_v, const char *name,
                            embi_js_value v)
{
    embi_js_runtime *rt = c->rt;
    if (!embi_js_is_object(this_v)) {
        embi_js_throw_type_error(rt, "Illegal invocation");
        return false;
    }
    return embi_js_create_data_property(rt, embi_js_as_object(this_v), embi_js_key_c(rt, name), v) !=
           EMBI_JS_THREW;
}

bool embi_bind_put_forwards(embi_bind_ctx *c, embi_js_value this_v, const char *attr,
                            const char *forward, embi_js_value v)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value tgt = embi_js_get_v(rt, this_v, embi_js_key_c(rt, attr));
    if (tgt == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (!embi_js_is_object(tgt)) {
        embi_js_throw_type_error(rt, "The '%s' property is not an object.", attr);
        return false;
    }
    return embi_js_put(rt, embi_js_as_object(tgt), embi_js_key_c(rt, forward), v, true) !=
           EMBI_JS_THREW;
}

/* ------------------------------------------------------------------ reflection */

static bool parse_int(embi_dstr d, int64_t *out)
{
    uint32_t i = 0;
    while (i < d.len) {
        uint16_t u = embi_dstr_unit(d, i);
        if (u == ' ' || u == '\t' || u == '\n' || u == '\f' || u == '\r') {
            i++;
        } else {
            break;
        }
    }
    bool neg = false;
    if (i < d.len && (embi_dstr_unit(d, i) == '-' || embi_dstr_unit(d, i) == '+')) {
        neg = embi_dstr_unit(d, i) == '-';
        i++;
    }
    if (i >= d.len || embi_dstr_unit(d, i) < '0' || embi_dstr_unit(d, i) > '9') {
        return false;
    }
    int64_t v = 0;
    while (i < d.len && embi_dstr_unit(d, i) >= '0' && embi_dstr_unit(d, i) <= '9') {
        v = v * 10 + (embi_dstr_unit(d, i) - '0');
        if (v > 0x7FFFFFFFFFFFll) {
            v = 0x7FFFFFFFFFFFll;
        }
        i++;
    }
    *out = neg ? -v : v;
    return true;
}

bool embi_bind_reflect_get_bool(embi_element *el, embi_str name)
{
    return embi_el_has_attribute(el, name);
}

uint32_t embi_bind_reflect_get_ulong(embi_element *el, embi_str name)
{
    const embi_attr *a = embi_el_get_attribute(el, name);
    int64_t v = 0;
    if (!a || !parse_int(a->value, &v) || v < 0) {
        return 0;
    }
    return v > 0x7FFFFFFF ? 0 : (uint32_t)v;
}

int32_t embi_bind_reflect_get_long(embi_element *el, embi_str name, int32_t def)
{
    const embi_attr *a = embi_el_get_attribute(el, name);
    int64_t v = 0;
    if (!a || !parse_int(a->value, &v) || v > INT32_MAX || v < INT32_MIN) {
        return def;
    }
    return (int32_t)v;
}

double embi_bind_reflect_get_double(embi_element *el, embi_str name, double def)
{
    const embi_attr *a = embi_el_get_attribute(el, name);
    if (!a || a->value.len == 0) {
        return def;
    }
    char buf[64];
    uint32_t n = a->value.len < 63 ? a->value.len : 63;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (char)embi_dstr_unit(a->value, i);
    }
    buf[n] = '\0';
    char *end = nullptr;
    double d = SDL_strtod(buf, &end);
    return (end == buf) ? def : d;
}

embi_js_value embi_bind_reflect_get_str(embi_bind_ctx *c, embi_element *el, embi_str name)
{
    const embi_attr *a = embi_el_get_attribute(el, name);
    return embi_bind_dstr_value(c, a ? a->value : embi_dstr_empty());
}

void embi_bind_reflect_set_bool(embi_element *el, embi_str name, bool v)
{
    if (v) {
        if (!embi_el_has_attribute(el, name)) {
            (void)embi_el_set_attribute(el, name, embi_dstr_empty());
        }
    } else {
        (void)embi_el_remove_attribute(el, name);
    }
}

static void set_number_attr(embi_element *el, embi_str name, const char *text)
{
    embi_dstr d = {text, (uint32_t)SDL_strlen(text), false};
    (void)embi_el_set_attribute(el, name, d);
}

void embi_bind_reflect_set_ulong(embi_element *el, embi_str name, uint32_t v)
{
    char buf[16];
    SDL_snprintf(buf, sizeof buf, "%u", v);
    set_number_attr(el, name, buf);
}

void embi_bind_reflect_set_long(embi_element *el, embi_str name, int32_t v)
{
    char buf[16];
    SDL_snprintf(buf, sizeof buf, "%d", v);
    set_number_attr(el, name, buf);
}

void embi_bind_reflect_set_double(embi_element *el, embi_str name, double v)
{
    embi_sb sb = {0};
    embi_sb_append_f64(&sb, v);
    embi_dstr d = {embi_sb_cstr(&sb), sb.len, false};
    (void)embi_el_set_attribute(el, name, d);
    embi_sb_free(&sb);
}

void embi_bind_reflect_set_str(embi_bind_ctx *c, embi_element *el, embi_str name, embi_dstr v)
{
    (void)c;
    (void)embi_el_set_attribute(el, name, v);
}

/* The natives of reflected attributes. Each is the getter or setter of a row whose attr is the
 * content attribute name; the receiver is an element wrapper (EMBI_SELF_NODE). */

static embi_str refl_name(const embi_bind_member *m)
{
    return (embi_str){m->attr, (uint32_t)SDL_strlen(m->attr)};
}

#define REFL_GET_BEGIN \
    (void)argc; \
    (void)argv; \
    (void)magic; \
    const embi_bind_member *m = r->cur_data; \
    embi_bind_scope sc; \
    embi_bind_obj *bo = nullptr; \
    embi_bind_ctx *c = embi_bind_begin(r, this_v, m, EMBI_SELF_NODE, &sc, &bo); \
    if (!c) return sc.early; \
    embi_element *el = bo->ptr; \
    embi_str name = refl_name(m); \
    (void)name;

#define REFL_SET_BEGIN \
    (void)magic; \
    const embi_bind_member *m = r->cur_data; \
    embi_bind_scope sc; \
    embi_bind_obj *bo = nullptr; \
    embi_bind_ctx *c = embi_bind_begin(r, this_v, m, EMBI_SELF_NODE, &sc, &bo); \
    if (!c) return sc.early; \
    embi_element *el = bo->ptr; \
    embi_str name = refl_name(m); \
    embi_js_value v0 = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;

embi_js_value embi_bind_refl_get_str(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    return embi_bind_end(&sc, embi_bind_reflect_get_str(c, el, name));
}

embi_js_value embi_bind_refl_set_str(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    embi_dstr a0 = {0};
    if (!embi_bind_conv_str(c, v0, EMBI_CTX_SET, false, &a0)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
    embi_bind_reflect_set_str(c, el, name, a0);
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_bind_refl_get_nstr(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    const embi_attr *at = embi_el_get_attribute(el, name);
    return embi_bind_end(&sc, at ? embi_bind_dstr_value(c, at->value) : EMBI_JS_NULL);
}

embi_js_value embi_bind_refl_set_nstr(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    embi_bind_nstr a0 = {0};
    if (!embi_bind_conv_nstr(c, v0, EMBI_CTX_SET, false, &a0)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
    if (a0.is_null) (void)embi_el_remove_attribute(el, name);
    else embi_bind_reflect_set_str(c, el, name, a0.s);
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_bind_refl_get_bool(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    return embi_bind_end(&sc, embi_js_from_bool(embi_bind_reflect_get_bool(el, name)));
}

embi_js_value embi_bind_refl_set_bool(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    embi_bind_reflect_set_bool(el, name, embi_js_to_boolean(v0));
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_bind_refl_get_long(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    return embi_bind_end(&sc, embi_bind_num((double)embi_bind_reflect_get_long(el, name, (int32_t)m->num)));
}

embi_js_value embi_bind_refl_set_long(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    int64_t t = 0;
    if (!embi_bind_conv_int(c, v0, m->len, EMBI_CTX_SET, &t)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
    embi_bind_reflect_set_long(el, name, (int32_t)t);
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_bind_refl_get_ulong(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    return embi_bind_end(&sc, embi_bind_num((double)embi_bind_reflect_get_ulong(el, name)));
}

embi_js_value embi_bind_refl_set_ulong(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    int64_t t = 0;
    if (!embi_bind_conv_int(c, v0, m->len, EMBI_CTX_SET, &t)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
    embi_bind_reflect_set_ulong(el, name, (uint32_t)t);
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_bind_refl_get_double(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    REFL_GET_BEGIN
    return embi_bind_end(&sc, embi_bind_num(embi_bind_reflect_get_double(el, name, m->num)));
}

embi_js_value embi_bind_refl_set_double(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    REFL_SET_BEGIN
    double a0 = 0;
    if (!embi_bind_conv_double(c, v0, m->len != 0, EMBI_CTX_SET, &a0)) return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
    embi_bind_reflect_set_double(el, name, a0);
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ class hooks */

int embi_bind_own_named(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    /* The class hook that called us replaces [[GetOwnProperty]]: run the ordinary one with the
     * ordinary class in place. */
    const embi_js_class *saved = o->klass;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_get_own_property(rt, o, k, out);
    o->klass = saved;
    return r;
}

int embi_bind_own_keys_ordinary(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    const embi_js_class *saved = o->klass;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_own_keys(rt, o, out);
    o->klass = saved;
    return r;
}

/* ------------------------------------------------------------------ interface objects */

void embi_bind_finish_iface(embi_bind_ctx *c, embi_js_object *proto, embi_js_object *ctor,
                            const char *name, embi_js_value parent_proto)
{
    embi_js_runtime *rt = c->rt;
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_str_new_utf8(rt, name, SDL_strlen(name))),
                       EMBI_JS_ATTR_CONFIGURABLE);
    if (embi_js_is_object(parent_proto)) {
        embi_js_value pc = embi_js_obj_get(rt, embi_js_as_object(parent_proto),
                                           embi_js_key_c(rt, "constructor"), parent_proto);
        if (pc != EMBI_JS_EXCEPTION && embi_js_is_object(pc)) {
            (void)embi_js_obj_set_prototype_of(rt, ctor, pc);
        } else {
            embi_js_exception_clear(rt);
        }
    }
}

void embi_bind_finish_ns(embi_bind_ctx *c, embi_js_object *ns, const char *name)
{
    embi_js_runtime *rt = c->rt;
    embi_js_define_raw(rt, ns, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_str_new_utf8(rt, name, SDL_strlen(name))),
                       EMBI_JS_ATTR_CONFIGURABLE);
}

void embi_bind_define_css_prop(embi_bind_ctx *c, embi_js_object *proto, const char *dashed,
                               const char *camel, int idx, embi_js_native getter,
                               embi_js_native setter)
{
    embi_js_realm *r = c->realm;
    embi_js_runtime *rt = c->rt;
    char buf[200];
    SDL_snprintf(buf, sizeof buf, "get %s", camel);
    embi_js_value g = embi_js_from_object(embi_js_native_function_new(r, buf, 0, getter, idx));
    embi_bind_pin(c, g);
    SDL_snprintf(buf, sizeof buf, "set %s", camel);
    embi_js_value s = embi_js_from_object(embi_js_native_function_new(r, buf, 1, setter, idx));
    embi_bind_pin(c, s);
    uint8_t attrs = EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
    embi_js_define_accessor_raw(rt, proto, embi_js_key_c(rt, camel), g, s, attrs);
    if (SDL_strcmp(dashed, camel) != 0) {
        embi_js_define_accessor_raw(rt, proto, embi_js_key_c(rt, dashed), g, s, attrs);
    }
}

/* ------------------------------------------------------------------ iterables */

typedef struct iterobj {
    embi_js_value target;
    const embi_bind_iter_ops *ops;
    uint32_t idx;
    int kind;
    bool done;
} iterobj;

static void iter_trace(embi_js_object *o, embi_js_tracer *t)
{
    iterobj *it = o->internal;
    if (it) {
        embi_js_mark_value(t, it->target);
    }
}

static void iter_finalize(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(iterobj));
        o->internal = nullptr;
    }
}

static const embi_js_class k_iter_class = {
    .name = "Iterator",
    .trace = iter_trace,
    .finalize = iter_finalize,
};

static embi_js_value iter_result(embi_bind_ctx *c, embi_js_value value, bool done)
{
    embi_js_value o = embi_bind_object_new(c);
    embi_bind_pin(c, o);
    embi_bind_object_set_c(c, o, "value", value);
    embi_bind_object_set_c(c, o, "done", embi_js_from_bool(done));
    return o;
}

static embi_js_value iter_next(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (!c) {
        return embi_js_throw_type_error(r->rt, "Illegal invocation");
    }
    if (!embi_js_is_object(this_v) || embi_js_as_object(this_v)->klass != &k_iter_class) {
        return embi_js_throw_type_error(r->rt, "next method called on incompatible receiver");
    }
    iterobj *it = embi_js_as_object(this_v)->internal;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *self = embi_bind_obj_of(it->target);
    if (it->done || !self || embi_bind_stale(self) || it->idx >= it->ops->size(c, self)) {
        it->done = true;
        return embi_bind_end(&sc, iter_result(c, EMBI_JS_UNDEFINED, true));
    }
    uint32_t i = it->idx++;
    embi_js_value key = it->ops->key ? it->ops->key(c, self, i) : embi_bind_num((double)i);
    embi_bind_pin(c, key);
    embi_js_value val = it->kind == 0 ? EMBI_JS_UNDEFINED : it->ops->value(c, self, i);
    embi_bind_pin(c, val);
    embi_js_value v = key;
    if (it->kind == 1) {
        v = val;
    } else if (it->kind == 2) {
        v = embi_bind_array_new(c, 2);
        embi_bind_pin(c, v);
        embi_bind_array_set(c, v, 0, key);
        embi_bind_array_set(c, v, 1, val);
    }
    return embi_bind_end(&sc, iter_result(c, v, false));
}

embi_js_value embi_bind_iter_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    const embi_bind_iter_ops *ops = embi_js_native_data_current(r);
    embi_bind_obj *bo = nullptr;
    if (!c || !embi_bind_this(c, this_v, ops->iface, &bo)) {
        return c ? EMBI_JS_EXCEPTION : embi_js_throw_type_error(r->rt, "Illegal invocation");
    }
    iterobj *it = embi_calloc(1, sizeof *it);
    it->target = this_v;
    it->ops = ops;
    it->kind = magic;
    embi_js_object *o = embi_js_object_new_class(c->rt, c->iter_proto, &k_iter_class);
    o->internal = it;
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

embi_js_value embi_bind_foreach_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    const embi_bind_iter_ops *ops = embi_js_native_data_current(r);
    embi_bind_obj *bo = nullptr;
    if (!c || !embi_bind_this(c, this_v, ops->iface, &bo)) {
        return c ? EMBI_JS_EXCEPTION : embi_js_throw_type_error(r->rt, "Illegal invocation");
    }
    if (argc < 1) {
        return embi_bind_throw_args(c, "Failed to execute 'forEach'", 1, argc);
    }
    if (!embi_js_is_callable(argv[0])) {
        return embi_js_throw_type_error(r->rt,
                                        "Failed to execute 'forEach': parameter 1 is not a function.");
    }
    embi_js_value this_arg = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    embi_bind_scope sc = embi_bind_enter(c);
    for (uint32_t i = 0; !embi_bind_stale(bo) && i < ops->size(c, bo); i++) {
        embi_js_value key = ops->key ? ops->key(c, bo, i) : embi_bind_num((double)i);
        embi_bind_pin(c, key);
        embi_js_value val = embi_bind_pin(c, ops->value(c, bo, i));
        embi_js_value args[3] = {val, key, this_v};
        if (embi_js_call(r->rt, argv[0], this_arg, 3, args) == EMBI_JS_EXCEPTION) {
            return embi_bind_end(&sc, EMBI_JS_EXCEPTION);
        }
    }
    return embi_bind_end(&sc, EMBI_JS_UNDEFINED);
}

static void define_op_data(embi_bind_ctx *c, embi_js_object *proto, embi_js_key key,
                           const char *name, embi_js_native fn, int len, int magic, void *data,
                           uint8_t attrs)
{
    embi_js_object *f = embi_js_native_function_new_data(c->realm, name, len, fn, magic, data);
    embi_js_define_raw(c->rt, proto, key, embi_js_from_object(f), attrs);
}

void embi_bind_install_iterable(embi_bind_ctx *c, embi_js_object *proto,
                                const embi_bind_iter_ops *ops)
{
    embi_js_runtime *rt = c->rt;
    if (embi_js_is_empty(c->iter_proto)) {
        embi_js_object *ip = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_ITERATOR]);
        c->iter_proto = embi_js_from_object(ip);
        /* WebIDL 3.7.10: the iterator prototype's next is writable, enumerable and configurable. */
        define_op_data(c, ip, embi_js_key_c(rt, "next"), "next", iter_next, 0, 0, nullptr,
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE);
        embi_js_define_raw(rt, ip, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                           embi_js_from_string(embi_js_str_new_utf8(rt, "Iterator", 8)),
                           EMBI_JS_ATTR_CONFIGURABLE);
    }
    const uint8_t wec = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
    define_op_data(c, proto, embi_js_key_c(rt, "keys"), "keys", embi_bind_iter_native, 0, 0,
                   (void *)ops, wec);
    define_op_data(c, proto, embi_js_key_c(rt, "values"), "values", embi_bind_iter_native, 0, 1,
                   (void *)ops, wec);
    define_op_data(c, proto, embi_js_key_c(rt, "entries"), "entries", embi_bind_iter_native, 0, 2,
                   (void *)ops, wec);
    define_op_data(c, proto, embi_js_key_c(rt, "forEach"), "forEach", embi_bind_foreach_native, 1,
                   0, (void *)ops, wec);
    define_op_data(c, proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR), "[Symbol.iterator]",
                   embi_bind_iter_native, 0, ops->pairs ? 2 : 1, (void *)ops,
                   EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

void embi_bind_install_array_iterable(embi_bind_ctx *c, embi_js_object *proto, bool symbol_only)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value ap = rt->protos[EMBI_JS_PROTO_ARRAY];
    static const char *const names[] = {"keys", "values", "entries", "forEach"};
    const uint8_t wec = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
    for (int i = 0; i < 4 && !symbol_only; i++) {
        embi_js_value f = embi_js_obj_get(rt, embi_js_as_object(ap), embi_js_key_c(rt, names[i]), ap);
        if (f == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
            continue;
        }
        embi_js_define_raw(rt, proto, embi_js_key_c(rt, names[i]), f, wec);
    }
    embi_js_key sk = embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR);
    embi_js_value f = embi_js_obj_get(rt, embi_js_as_object(ap), sk, ap);
    if (f != EMBI_JS_EXCEPTION) {
        embi_js_define_raw(rt, proto, sk, f, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    } else {
        embi_js_exception_clear(rt);
    }
}

void embi_bind_define_op(embi_bind_ctx *c, embi_js_object *obj, const char *name, embi_js_native fn,
                         int length, bool configurable)
{
    embi_js_object *f = embi_js_native_function_new(c->realm, name, length, fn, 0);
    uint8_t attrs = EMBI_JS_ATTR_ENUMERABLE;
    if (configurable) {
        attrs |= EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE;
    }
    embi_js_define_raw(c->rt, obj, embi_js_key_c(c->rt, name), embi_js_from_object(f), attrs);
}

embi_str embi_bind_key_utf8(embi_bind_ctx *c, embi_js_key k)
{
    if (embi_js_key_is_index(k)) {
        char *buf = embi_bind_alloc(c, 16);
        int n = SDL_snprintf(buf, 16, "%u", (unsigned)embi_js_key_index(k));
        return (embi_str){buf, (uint32_t)n};
    }
    if (!embi_js_is_string(k)) {
        return (embi_str){"", 0};
    }
    embi_sb sb = {0};
    embi_js_str_append_utf8(embi_js_as_string(k), &sb);
    char *buf = embi_bind_alloc(c, (size_t)sb.len + 1);
    if (sb.len) {
        SDL_memcpy(buf, sb.data, sb.len);
    }
    embi_str r = {buf, (uint32_t)sb.len};
    embi_sb_free(&sb);
    return r;
}

embi_js_key embi_bind_key_from_utf8(embi_bind_ctx *c, embi_str s)
{
    return embi_js_key_from_str(c->rt, embi_js_str_new_utf8(c->rt, s.ptr, s.len));
}

void embi_bind_adopt_new_target(embi_bind_ctx *c, embi_js_value v, embi_js_value new_target)
{
    embi_js_runtime *rt = c->rt;
    if (!embi_js_is_object(new_target) || !embi_js_is_object(v)) {
        return;
    }
    embi_js_value p = embi_js_obj_get(rt, embi_js_as_object(new_target), embi_js_key_c(rt, "prototype"),
                                      new_target);
    if (p == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        return;
    }
    if (embi_js_is_object(p)) {
        (void)embi_js_obj_set_prototype_of(rt, embi_js_as_object(v), p);
    }
}

bool embi_bind_proto_has(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_value proto = o->shape->proto;
    if (!embi_js_is_object(proto)) {
        return false;
    }
    return embi_js_obj_has_property(rt, embi_js_as_object(proto), k) == EMBI_JS_YES;
}
