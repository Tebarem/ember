/*
 * Global object contents: Object (installed by embi_js_builtins_object_install), print, globalThis,
 * the value properties undefined, NaN and Infinity, the function properties parseInt, parseFloat,
 * isNaN, isFinite, encodeURI, encodeURIComponent, decodeURI and decodeURIComponent. eval is
 * installed by realm creation (interp.c). Runs after the global object exists.
 */
#include "js/builtins.h"
#include "js/js.h"
#include "num/num.h"

#include <SDL3/SDL_log.h>
#include <math.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

typedef EMBI_VEC(uint16_t) u16_vec;

/* ------------------------------------------------------------------ print (test helper) */

static embi_js_value print_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_sb sb = {nullptr, 0, 0};
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_js_string *s = embi_js_to_string(rt, argv[i]);
        if (!s) {
            embi_sb_free(&sb);
            return EXC;
        }
        embi_js_str_append_utf8(s, &sb);
    }
    /* Output goes through SDL_Log (stderr, or the platform log): the engine does no libc I/O. */
    SDL_Log("%s", embi_sb_cstr(&sb));
    embi_sb_free(&sb);
    return EMBI_JS_UNDEFINED;
}

/* ------------------------------------------------------------------ parseInt and parseFloat */

/* WhiteSpace and LineTerminator (ECMA-262 12.2, 12.3). */
static bool is_js_space(uint32_t c)
{
    switch (c) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

/* Digit value of an ASCII alphanumeric, or 99 for anything else. */
static uint32_t digit_value(uint32_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10u;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10u;
    }
    return 99u;
}

static embi_js_value parse_int(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!s) {
        return EXC;
    }
    int32_t radix = 0;
    if (argc > 1 && !embi_js_to_int32(rt, argv[1], &radix)) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t i = 0;
    while (i < len && is_js_space(embi_js_str_at(s, i))) {
        i++;
    }
    bool neg = false;
    if (i < len && (embi_js_str_at(s, i) == '+' || embi_js_str_at(s, i) == '-')) {
        neg = embi_js_str_at(s, i) == '-';
        i++;
    }
    bool strip = true;
    uint32_t R = 10;
    if (radix != 0) {
        if (radix < 2 || radix > 36) {
            return embi_js_number(NAN);
        }
        R = (uint32_t)radix;
        if (R != 16) {
            strip = false;
        }
    }
    if (strip && i + 1 < len && embi_js_str_at(s, i) == '0' &&
        (embi_js_str_at(s, i + 1) == 'x' || embi_js_str_at(s, i + 1) == 'X')) {
        i += 2;
        R = 16;
    }
    embi_sb digits = {nullptr, 0, 0};
    while (i < len) {
        uint32_t c = embi_js_str_at(s, i);
        if (c > 0x7F || digit_value(c) >= R) {
            break;
        }
        embi_sb_append_char(&digits, (char)c);
        i++;
    }
    if (digits.len == 0) {
        embi_sb_free(&digits);
        return embi_js_number(NAN);
    }
    size_t used = 0;
    double v = embi_num_parse_radix(digits.data, digits.len, R, &used);
    embi_sb_free(&digits);
    return embi_js_number(neg ? -v : v);
}

static embi_js_value parse_float(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!s) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t i = 0;
    while (i < len && is_js_space(embi_js_str_at(s, i))) {
        i++;
    }
    /* Only ASCII can take part in a StrDecimalLiteral, so the scan stops at the first other unit. */
    embi_sb buf = {nullptr, 0, 0};
    for (; i < len && embi_js_str_at(s, i) < 0x80; i++) {
        embi_sb_append_char(&buf, (char)embi_js_str_at(s, i));
    }
    double result = NAN;
    const char *p = buf.data ? buf.data : "";
    size_t n = buf.len;
    size_t k = 0;
    bool neg = false;
    if (k < n && (p[k] == '+' || p[k] == '-')) {
        neg = p[k] == '-';
        k++;
    }
    if (n - k >= 8 && memcmp(p + k, "Infinity", 8) == 0) {
        result = neg ? -INFINITY : INFINITY;
    } else {
        size_t used = 0;
        double v = embi_num_parse_decimal(p, n, &used);
        if (used > 0) {
            result = v;
        }
    }
    embi_sb_free(&buf);
    return embi_js_number(result);
}

/* ------------------------------------------------------------------ isNaN and isFinite */

static embi_js_value is_nan_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    double d = 0.0;
    if (!embi_js_to_number(r->rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, &d)) {
        return EXC;
    }
    return embi_js_from_bool(isnan(d));
}

static embi_js_value is_finite_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    double d = 0.0;
    if (!embi_js_to_number(r->rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, &d)) {
        return EXC;
    }
    return embi_js_from_bool(isfinite(d));
}

/* ------------------------------------------------------------------ URI functions */

static bool is_unreserved(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '-':
    case '_':
    case '.':
    case '!':
    case '~':
    case '*':
    case '\'':
    case '(':
    case ')':
        return true;
    default:
        return false;
    }
}

/* uriReserved: the characters that encodeURI leaves alone and decodeURI keeps escaped. */
static bool is_reserved(uint32_t c)
{
    switch (c) {
    case ';':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case ',':
    case '#':
        return true;
    default:
        return false;
    }
}

static embi_js_value uri_error(embi_js_runtime *rt)
{
    return embi_js_throw_error(rt, EMBI_JS_PROTO_URI_ERROR, "URI malformed");
}

static void put_pct(embi_sb *sb, uint32_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    embi_sb_append_char(sb, '%');
    embi_sb_append_char(sb, hex[(byte >> 4) & 15u]);
    embi_sb_append_char(sb, hex[byte & 15u]);
}

/* encodeURI (magic 0) and encodeURIComponent (magic 1). Lone surrogates throw URIError. */
static embi_js_value uri_encode(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    bool component = magic == 1;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!s) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    embi_sb out = {nullptr, 0, 0};
    embi_js_value result = EXC;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t c = embi_js_str_at(s, i);
        if (c < 0x80 && (is_unreserved(c) || (!component && is_reserved(c)))) {
            embi_sb_append_char(&out, (char)c);
            continue;
        }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            uri_error(rt);
            goto done;
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= len) {
                uri_error(rt);
                goto done;
            }
            uint32_t d = embi_js_str_at(s, i + 1);
            if (d < 0xDC00 || d > 0xDFFF) {
                uri_error(rt);
                goto done;
            }
            cp = 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u);
            i++;
        }
        if (cp < 0x80) {
            put_pct(&out, cp);
        } else if (cp < 0x800) {
            put_pct(&out, 0xC0u | (cp >> 6));
            put_pct(&out, 0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000) {
            put_pct(&out, 0xE0u | (cp >> 12));
            put_pct(&out, 0x80u | ((cp >> 6) & 0x3Fu));
            put_pct(&out, 0x80u | (cp & 0x3Fu));
        } else {
            put_pct(&out, 0xF0u | (cp >> 18));
            put_pct(&out, 0x80u | ((cp >> 12) & 0x3Fu));
            put_pct(&out, 0x80u | ((cp >> 6) & 0x3Fu));
            put_pct(&out, 0x80u | (cp & 0x3Fu));
        }
    }
    embi_js_string *res = embi_js_str_new_latin1(rt, (const uint8_t *)out.data, out.len);
    if (res) {
        result = embi_js_from_string(res);
    }
done:
    embi_sb_free(&out);
    return result;
}

static int hex_value(uint32_t c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A' + 10);
    }
    return -1;
}

/* The byte of "%XY" at position p of s, or -1 when it is not a valid escape. */
static int escape_at(const embi_js_string *s, uint32_t len, uint32_t p)
{
    if (p + 2 >= len || embi_js_str_at(s, p) != '%') {
        return -1;
    }
    int hi = hex_value(embi_js_str_at(s, p + 1));
    int lo = hex_value(embi_js_str_at(s, p + 2));
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return hi * 16 + lo;
}

static void push_unit(u16_vec *out, uint32_t u)
{
    embi_vec_push(out, (uint16_t)u);
}

/* decodeURI (magic 0, keeps escapes of reserved characters) and decodeURIComponent (magic 1). */
static embi_js_value uri_decode(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    bool component = magic == 1;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!s) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    u16_vec out = {nullptr, 0, 0};
    embi_js_value result = EXC;
    for (uint32_t k = 0; k < len; k++) {
        uint32_t c = embi_js_str_at(s, k);
        if (c != '%') {
            push_unit(&out, c);
            continue;
        }
        uint32_t start = k;
        int b = escape_at(s, len, k);
        if (b < 0) {
            uri_error(rt);
            goto done;
        }
        k += 2;
        if (b < 0x80) {
            if (!component && is_reserved((uint32_t)b)) {
                for (uint32_t j = start; j <= k; j++) {
                    push_unit(&out, embi_js_str_at(s, j));
                }
            } else {
                push_unit(&out, (uint32_t)b);
            }
            continue;
        }
        int n = 0;
        if ((b & 0xE0) == 0xC0) {
            n = 2;
        } else if ((b & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4;
        } else {
            uri_error(rt);
            goto done;
        }
        uint32_t cp = (uint32_t)b & (0x7Fu >> n);
        for (int j = 1; j < n; j++) {
            int cb = escape_at(s, len, k + 1);
            if (cb < 0 || (cb & 0xC0) != 0x80) {
                uri_error(rt);
                goto done;
            }
            cp = (cp << 6) | ((uint32_t)cb & 0x3Fu);
            k += 3;
        }
        static const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_cp[n] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            uri_error(rt);
            goto done;
        }
        if (cp >= 0x10000u) {
            push_unit(&out, 0xD800u + ((cp - 0x10000u) >> 10));
            push_unit(&out, 0xDC00u + ((cp - 0x10000u) & 0x3FFu));
        } else {
            push_unit(&out, cp);
        }
    }
    embi_js_string *res = embi_js_str_new_utf16(rt, out.data, out.len);
    if (res) {
        result = embi_js_from_string(res);
    }
done:
    embi_vec_free(&out);
    return result;
}

/* ------------------------------------------------------------------ installation */

void embi_js_init_builtins_global(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *g = r->global;

    embi_js_builtins_object_install(r);

    embi_js_define_method(r, g, "print", print_fn, 1, 0);
    embi_js_define_raw(rt, g, embi_js_key_c(rt, "globalThis"), embi_js_from_object(g), ATTR_WC);
    embi_js_define_raw(rt, g, embi_js_key_c(rt, "undefined"), EMBI_JS_UNDEFINED, 0);
    embi_js_define_raw(rt, g, embi_js_key_c(rt, "NaN"), embi_js_number(NAN), 0);
    embi_js_define_raw(rt, g, embi_js_key_c(rt, "Infinity"), embi_js_number(INFINITY), 0);

    embi_js_define_method(r, g, "parseInt", parse_int, 2, 0);
    embi_js_define_method(r, g, "parseFloat", parse_float, 1, 0);
    embi_js_define_method(r, g, "isNaN", is_nan_fn, 1, 0);
    embi_js_define_method(r, g, "isFinite", is_finite_fn, 1, 0);
    embi_js_define_method(r, g, "encodeURI", uri_encode, 1, 0);
    embi_js_define_method(r, g, "encodeURIComponent", uri_encode, 1, 1);
    embi_js_define_method(r, g, "decodeURI", uri_decode, 1, 0);
    embi_js_define_method(r, g, "decodeURIComponent", uri_decode, 1, 1);
}
