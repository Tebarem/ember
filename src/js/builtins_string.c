/*
 * String: the String constructor (called as a function it converts, with new it makes a String
 * wrapper), String.fromCharCode, String.fromCodePoint and String.raw, and String.prototype (a
 * String wrapper of ""). The prototype has the ES5 methods, the ES2015-2022 methods (includes,
 * startsWith, endsWith, repeat, padStart, padEnd, trimStart/trimEnd, at, codePointAt, normalize,
 * isWellFormed, toWellFormed), String.prototype[@@iterator] (J2c),
 * the regular-expression entry points (match, matchAll, replace, replaceAll, search, split, which
 * dispatch through the Symbol methods and need RegExp for string arguments), and the Annex B
 * methods (substr, trimLeft, trimRight and the HTML methods).
 *
 * Case mapping is full Unicode default case conversion (SpecialCasing, unconditional entries,
 * plus Final_Sigma), not locale-sensitive. The tables are generated into js/unicode_case.inc by
 * tools/unicodegen/casegen.c. String.prototype.normalize runs the UAX #15 algorithms of
 * js/unicode_norm.c over the tables generated into js/unicode_norm.inc by
 * tools/unicodegen/normgen.c.
 *
 * The constructors use their own class with call and construct hooks: a native function receives
 * no new.target, and String(x) must differ from new String(x).
 */
#include "js/builtins.h"
#include "js/heap.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/string.h"
#include "js/unicode_norm.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define STR_MAX EMBI_JS_STR_MAX_LEN
#define ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))

#include "js/unicode_case.inc"

typedef EMBI_VEC(uint16_t) u16_buf;

/* ------------------------------------------------------------------ helpers */

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static bool is_finite(double d)
{
    return d - d == 0.0;
}

static embi_js_value str_val(embi_js_string *s)
{
    return s ? embi_js_from_string(s) : EXC;
}

static embi_js_value ascii_val(embi_js_runtime *rt, const char *p)
{
    return str_val(embi_js_str_new_latin1(rt, (const uint8_t *)p, strlen(p)));
}

static embi_js_value sub_val(embi_js_runtime *rt, const embi_js_string *s, uint32_t a, uint32_t b)
{
    return str_val(embi_js_str_substring(rt, s, a, b));
}

/* Appends code point cp as one or two UTF-16 units. */
static void buf_push_cp(u16_buf *b, uint32_t cp)
{
    if (cp < 0x10000u) {
        embi_vec_push(b, (uint16_t)cp);
    } else {
        cp -= 0x10000u;
        embi_vec_push(b, (uint16_t)(0xD800u + (cp >> 10)));
        embi_vec_push(b, (uint16_t)(0xDC00u + (cp & 0x3FFu)));
    }
}

static void buf_push_str(u16_buf *b, const embi_js_string *s, uint32_t from, uint32_t to)
{
    embi_vec_reserve(b, b->len + (to - from));
    for (uint32_t i = from; i < to; i++) {
        b->data[b->len++] = embi_js_str_at(s, i);
    }
}

static void buf_push_ascii(u16_buf *b, const char *p)
{
    for (; *p; p++) {
        embi_vec_push(b, (uint16_t)(unsigned char)*p);
    }
}

/* Turns the buffer into a string and frees it. */
static embi_js_value buf_finish(embi_js_runtime *rt, u16_buf *b)
{
    embi_js_string *s = embi_js_str_new_utf16(rt, b->data, b->len);
    embi_vec_free(b);
    return str_val(s);
}

/* RequireObjectCoercible(this) followed by ToString. Returns nullptr when it throws. */
static embi_js_string *this_string(embi_js_runtime *rt, embi_js_value this_v, const char *name)
{
    if (embi_js_is_nullish(this_v)) {
        embi_js_throw_type_error(rt, "String.prototype.%s called on null or undefined", name);
        return nullptr;
    }
    return embi_js_to_string(rt, this_v);
}

/* The index of the first occurrence of needle in hay at or after from, or -1. */
static int64_t str_index_of(const embi_js_string *hay, uint32_t from, const embi_js_string *nd)
{
    uint32_t hl = embi_js_str_len(hay);
    uint32_t nl = embi_js_str_len(nd);
    if (nl == 0) {
        return from <= hl ? (int64_t)from : -1;
    }
    if (nl > hl) {
        return -1;
    }
    uint16_t first = embi_js_str_at(nd, 0);
    for (uint32_t i = from; i <= hl - nl; i++) {
        if (embi_js_str_at(hay, i) != first) {
            continue;
        }
        uint32_t k = 1;
        while (k < nl && embi_js_str_at(hay, i + k) == embi_js_str_at(nd, k)) {
            k++;
        }
        if (k == nl) {
            return i;
        }
    }
    return -1;
}

/* The last occurrence of nd that starts at or before start, or -1. */
static int64_t str_last_index_of(const embi_js_string *hay, uint32_t start,
                                 const embi_js_string *nd)
{
    uint32_t hl = embi_js_str_len(hay);
    uint32_t nl = embi_js_str_len(nd);
    if (nl > hl) {
        return -1;
    }
    uint32_t i = start < hl - nl ? start : hl - nl;
    for (;;) {
        uint32_t k = 0;
        while (k < nl && embi_js_str_at(hay, i + k) == embi_js_str_at(nd, k)) {
            k++;
        }
        if (k == nl) {
            return i;
        }
        if (i == 0) {
            return -1;
        }
        i--;
    }
}

/* True when nd matches hay at pos (the caller checks the bounds). */
static bool match_at(const embi_js_string *hay, uint32_t pos, const embi_js_string *nd)
{
    uint32_t nl = embi_js_str_len(nd);
    for (uint32_t k = 0; k < nl; k++) {
        if (embi_js_str_at(hay, pos + k) != embi_js_str_at(nd, k)) {
            return false;
        }
    }
    return true;
}

/* ToIntegerOrInfinity clamped to [0, len]. Returns false when it throws. */
static bool clamp_int(embi_js_runtime *rt, embi_js_value v, uint32_t len, uint32_t *out)
{
    double d;
    if (!embi_js_to_integer_or_infinity(rt, v, &d)) {
        return false;
    }
    *out = d <= 0 ? 0 : (d >= len ? len : (uint32_t)d);
    return true;
}

/* A relative position (slice, at): negative counts from the end. Result in [0, len]. */
static uint32_t rel_pos(double p, uint32_t len)
{
    if (p < 0) {
        double v = (double)len + p;
        return v < 0 ? 0 : (uint32_t)v;
    }
    return p > len ? len : (uint32_t)p;
}

/* True when s is exactly the ASCII literal lit. */
static bool str_is_ascii(const embi_js_string *s, const char *lit)
{
    size_t n = strlen(lit);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint8_t)lit[i]) {
            return false;
        }
    }
    return true;
}

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

/* IsRegExp(argument): YES, NO or THREW. A RegExp object without a @@match property is recognized
 * by its internal slot (builtins_regexp.c). */
static int is_regexp(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        return EMBI_JS_NO;
    }
    embi_js_value m = embi_js_obj_get(rt, embi_js_as_object(v),
                                      embi_js_wk_sym(rt, EMBI_JS_WK_MATCH), v);
    if (m == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_undefined(m)) {
        return embi_js_to_boolean(m) ? EMBI_JS_YES : EMBI_JS_NO;
    }
    return embi_js_is_regexp(v) ? EMBI_JS_YES : EMBI_JS_NO;
}

/* Throws a TypeError when v is a RegExp whose flags lack "g" (matchAll and replaceAll). Returns
 * false when it throws. */
static bool require_global_flag(embi_js_runtime *rt, embi_js_value v, const char *name)
{
    int re = is_regexp(rt, v);
    if (re == EMBI_JS_THREW) {
        return false;
    }
    if (re != EMBI_JS_YES) {
        return true;
    }
    embi_js_value flags = embi_js_get(rt, embi_js_as_object(v), embi_js_key_c(rt, "flags"));
    if (flags == EXC) {
        return false;
    }
    if (embi_js_is_nullish(flags)) {
        embi_js_throw_type_error(rt, "%s: flags is null or undefined", name);
        return false;
    }
    embi_js_string *fs = embi_js_to_string(rt, flags);
    if (!fs) {
        return false;
    }
    for (uint32_t i = 0; i < embi_js_str_len(fs); i++) {
        if (embi_js_str_at(fs, i) == 'g') {
            return true;
        }
    }
    embi_js_throw_type_error(rt, "%s must be called with a global RegExp", name);
    return false;
}

/* RegExpCreate(P, F) through the %RegExp% intrinsic. */
static embi_js_value regexp_create(embi_js_realm *r, embi_js_value pattern, embi_js_value flags)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value ctor = rt->protos[EMBI_JS_PROTO_REGEXP_CTOR];
    embi_js_value args[2] = {pattern, flags};
    return embi_js_construct(rt, ctor, 2, args, ctor);
}

/* Appends GetSubstitution(matched, str, position, no captures, replacement) to b. */
static void append_substitution(u16_buf *b, const embi_js_string *matched,
                                const embi_js_string *str, uint32_t pos,
                                const embi_js_string *rep)
{
    uint32_t rl = embi_js_str_len(rep);
    uint32_t sl = embi_js_str_len(str);
    uint32_t ml = embi_js_str_len(matched);
    for (uint32_t i = 0; i < rl; i++) {
        uint16_t c = embi_js_str_at(rep, i);
        if (c != '$' || i + 1 >= rl) {
            embi_vec_push(b, c);
            continue;
        }
        uint16_t n = embi_js_str_at(rep, i + 1);
        switch (n) {
        case '$':
            embi_vec_push(b, '$');
            i++;
            break;
        case '&':
            buf_push_str(b, matched, 0, ml);
            i++;
            break;
        case '`':
            buf_push_str(b, str, 0, pos);
            i++;
            break;
        case '\'': {
            uint32_t tail = pos + ml;
            if (tail < sl) {
                buf_push_str(b, str, tail, sl);
            }
            i++;
            break;
        }
        default:
            /* No captures and no named groups: "$n" and "$<" stay literal. */
            embi_vec_push(b, c);
            break;
        }
    }
}

/* The property key of an integer index, possibly above 2^32 - 2 (a string key then). */
static embi_js_key key_of_index(embi_js_runtime *rt, double i)
{
    if (i <= (double)EMBI_JS_MAX_ARRAY_INDEX) {
        return embi_js_key_from_index((uint32_t)i);
    }
    return embi_js_to_property_key(rt, embi_js_number(i));
}

/* ------------------------------------------------------------------ case mapping */

static const embi_case_special *special_find(const embi_case_special *tab, size_t n, uint32_t cp)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tab[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < n && tab[lo].cp == cp ? &tab[lo] : nullptr;
}

/* The simple mapping of cp in the runs, when cp has one. */
static bool run_lookup(const embi_case_run *runs, size_t n, uint32_t cp, uint32_t *out)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (runs[mid].start <= cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const embi_case_run *r = &runs[lo - 1];
    uint32_t d = cp - r->start;
    if (d % r->stride != 0 || d / r->stride >= r->count) {
        return false;
    }
    *out = cp + (uint32_t)r->delta; /* wraps back to the mapped value */
    return true;
}

static bool in_ranges(const embi_case_range *tab, size_t n, uint32_t cp)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tab[mid].hi < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < n && tab[lo].lo <= cp;
}

static bool is_cased(uint32_t cp)
{
    return in_ranges(embi_case_cased, ARR_LEN(embi_case_cased), cp);
}

static bool is_case_ignorable(uint32_t cp)
{
    return in_ranges(embi_case_case_ignorable, ARR_LEN(embi_case_case_ignorable), cp);
}

/* Maps one code point with the full default mapping. Writes up to three code points and returns
 * how many. */
static int map_cp(uint32_t cp, bool upper, uint32_t out[3])
{
    if (cp < 0x80u) {
        if (upper && cp >= 'a' && cp <= 'z') {
            out[0] = cp - 32u;
        } else if (!upper && cp >= 'A' && cp <= 'Z') {
            out[0] = cp + 32u;
        } else {
            out[0] = cp;
        }
        return 1;
    }
    const embi_case_special *sp =
        upper ? special_find(embi_case_upper_special, ARR_LEN(embi_case_upper_special), cp)
              : special_find(embi_case_lower_special, ARR_LEN(embi_case_lower_special), cp);
    if (sp) {
        int n = 0;
        while (n < 3 && sp->out[n] != 0) {
            out[n] = sp->out[n];
            n++;
        }
        return n;
    }
    bool hit = upper ? run_lookup(embi_case_upper_runs, ARR_LEN(embi_case_upper_runs), cp, out)
                     : run_lookup(embi_case_lower_runs, ARR_LEN(embi_case_lower_runs), cp, out);
    if (!hit) {
        out[0] = cp;
    }
    return 1;
}

/* The code point ending just before index j (j > 0), and the index where it starts. */
static uint32_t cp_before(const embi_js_string *s, uint32_t j, uint32_t *start)
{
    uint32_t u = embi_js_str_at(s, j - 1);
    if (u >= 0xDC00u && u <= 0xDFFFu && j >= 2) {
        uint32_t lead = embi_js_str_at(s, j - 2);
        if (lead >= 0xD800u && lead <= 0xDBFFu) {
            *start = j - 2;
            return 0x10000u + ((lead - 0xD800u) << 10) + (u - 0xDC00u);
        }
    }
    *start = j - 1;
    return u;
}

/* The code point starting at index j (j < len), and the index after it. */
static uint32_t cp_at(const embi_js_string *s, uint32_t j, uint32_t *next)
{
    uint32_t u = embi_js_str_at(s, j);
    if (u >= 0xD800u && u <= 0xDBFFu && j + 1 < embi_js_str_len(s)) {
        uint32_t trail = embi_js_str_at(s, j + 1);
        if (trail >= 0xDC00u && trail <= 0xDFFFu) {
            *next = j + 2;
            return 0x10000u + ((u - 0xD800u) << 10) + (trail - 0xDC00u);
        }
    }
    *next = j + 1;
    return u;
}

/* Final_Sigma (Unicode Table 3-17): preceded by a cased letter with only case-ignorable
 * characters between, and not followed by such a sequence. */
static bool is_final_sigma(const embi_js_string *s, uint32_t i)
{
    bool before = false;
    uint32_t j = i;
    while (j > 0) {
        uint32_t k;
        uint32_t c = cp_before(s, j, &k);
        j = k;
        if (is_case_ignorable(c)) {
            continue;
        }
        before = is_cased(c);
        break;
    }
    if (!before) {
        return false;
    }
    uint32_t n = embi_js_str_len(s);
    uint32_t next;
    j = i + 1;
    while (j < n) {
        uint32_t c = cp_at(s, j, &next);
        j = next;
        if (is_case_ignorable(c)) {
            continue;
        }
        return !is_cased(c);
    }
    return true;
}

/* toLowerCase / toUpperCase of the whole string. */
static embi_js_value change_case(embi_js_runtime *rt, const embi_js_string *s, bool upper)
{
    u16_buf b = {nullptr, 0, 0};
    uint32_t n = embi_js_str_len(s);
    uint32_t i = 0;
    while (i < n) {
        uint32_t next;
        uint32_t cp = cp_at(s, i, &next);
        uint32_t out[3];
        int m;
        if (!upper && cp == 0x03A3u) {
            out[0] = is_final_sigma(s, i) ? 0x03C2u : 0x03C3u;
            m = 1;
        } else {
            m = map_cp(cp, upper, out);
        }
        for (int k = 0; k < m; k++) {
            buf_push_cp(&b, out[k]);
        }
        i = next;
    }
    return buf_finish(rt, &b);
}

/* ------------------------------------------------------------------ constructors */

static embi_js_value make_wrapper(embi_js_runtime *rt, embi_js_value proto,
                                  const embi_js_class *klass, embi_js_value prim)
{
    embi_js_object *o = embi_js_object_new_class(rt, proto, klass);
    o->internal = (void *)(uintptr_t)prim;
    EMBI_JS_WB_STRUCT(o);
    if (embi_js_is_string(prim)) {
        embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                           embi_js_number((double)embi_js_str_len(embi_js_as_string(prim))), 0);
    }
    return embi_js_from_object(o);
}

/* Symbol.prototype.toString semantics for String(symbol): "Symbol(description)". */
static embi_js_value symbol_descriptive(embi_js_runtime *rt, embi_js_value sym)
{
    u16_buf b = {nullptr, 0, 0};
    buf_push_ascii(&b, "Symbol(");
    embi_js_value d = embi_js_as_symbol(sym)->description;
    if (embi_js_is_string(d)) {
        const embi_js_string *ds = embi_js_str_flat(rt, d);
        buf_push_str(&b, ds, 0, embi_js_str_len(ds));
    }
    buf_push_ascii(&b, ")");
    return buf_finish(rt, &b);
}

static int string_call_to(embi_js_runtime *rt, embi_js_value v, embi_js_string **out)
{
    *out = embi_js_to_string(rt, v);
    return *out ? EMBI_JS_YES : EMBI_JS_THREW;
}

static embi_js_value string_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    if (argc == 0) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    if (embi_js_is_symbol(argv[0])) {
        return symbol_descriptive(rt, argv[0]);
    }
    embi_js_string *s;
    if (string_call_to(rt, argv[0], &s) != EMBI_JS_YES) {
        return EXC;
    }
    return embi_js_from_string(s);
}

static embi_js_value string_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    embi_js_string *s;
    if (argc == 0) {
        s = embi_js_atom_intern_utf8(rt, "", 0);
    } else if (string_call_to(rt, argv[0], &s) != EMBI_JS_YES) {
        return EXC;
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_STRING);
    if (proto == EXC) {
        return EXC;
    }
    return make_wrapper(rt, proto, &embi_js_class_string, embi_js_from_string(s));
}

static const embi_js_class string_ctor_class = {
    .name = "Function",
    .call = string_call,
    .construct = string_construct,
};

static embi_js_value string_from_char_code(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    u16_buf b = {nullptr, 0, 0};
    for (int i = 0; i < argc; i++) {
        uint32_t u;
        if (!embi_js_to_uint32(rt, argv[i], &u)) {
            embi_vec_free(&b);
            return EXC;
        }
        embi_vec_push(&b, (uint16_t)(u & 0xFFFFu));
    }
    return buf_finish(rt, &b);
}

static embi_js_value string_from_code_point(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    u16_buf b = {nullptr, 0, 0};
    for (int i = 0; i < argc; i++) {
        double d;
        if (!embi_js_to_number(rt, argv[i], &d)) {
            embi_vec_free(&b);
            return EXC;
        }
        if (d != trunc(d) || !is_finite(d) || d < 0 || d > 0x10FFFF) {
            embi_vec_free(&b);
            return embi_js_throw_range_error(rt, "Invalid code point");
        }
        buf_push_cp(&b, (uint32_t)d);
    }
    return buf_finish(rt, &b);
}

/* String.raw(template, ...substitutions). */
static embi_js_value string_raw(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *cooked = embi_js_to_object(rt, arg_at(argc, argv, 0));
    if (!cooked) {
        return EXC;
    }
    embi_js_value rawv = embi_js_get(rt, cooked, embi_js_key_c(rt, "raw"));
    if (rawv == EXC) {
        return EXC;
    }
    embi_js_object *raw = embi_js_to_object(rt, rawv);
    if (!raw) {
        return EXC;
    }
    double count;
    if (!embi_js_length_of_array_like(rt, raw, &count)) {
        return EXC;
    }
    if (count <= 0) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    u16_buf b = {nullptr, 0, 0};
    for (double i = 0;; i++) {
        embi_js_value lit = embi_js_obj_get(rt, raw, key_of_index(rt, i), embi_js_from_object(raw));
        embi_js_string *ls = lit == EXC ? nullptr : embi_js_to_string(rt, lit);
        if (!ls) {
            embi_vec_free(&b);
            return EXC;
        }
        buf_push_str(&b, ls, 0, embi_js_str_len(ls));
        if (i + 1 == count) {
            break;
        }
        if (i + 1 < argc) {
            embi_js_string *ss = embi_js_to_string(rt, argv[(int)i + 1]);
            if (!ss) {
                embi_vec_free(&b);
                return EXC;
            }
            buf_push_str(&b, ss, 0, embi_js_str_len(ss));
        }
    }
    return buf_finish(rt, &b);
}

/* ------------------------------------------------------------------ String.prototype: access */

static embi_js_value str_at(embi_js_realm *r, embi_js_value this_v, int argc,
                            const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "at");
    if (!s) {
        return EXC;
    }
    double rel;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &rel)) {
        return EXC;
    }
    double len = embi_js_str_len(s);
    double k = rel >= 0 ? rel : len + rel;
    if (k < 0 || k >= len) {
        return UNDEF;
    }
    return sub_val(rt, s, (uint32_t)k, (uint32_t)k + 1);
}

static embi_js_value str_char_at(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "charAt");
    if (!s) {
        return EXC;
    }
    double p;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &p)) {
        return EXC;
    }
    if (p < 0 || p >= embi_js_str_len(s)) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    return sub_val(rt, s, (uint32_t)p, (uint32_t)p + 1);
}

static embi_js_value str_char_code_at(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "charCodeAt");
    if (!s) {
        return EXC;
    }
    double p;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &p)) {
        return EXC;
    }
    if (p < 0 || p >= embi_js_str_len(s)) {
        return embi_js_number(NAN);
    }
    return embi_js_from_int32(embi_js_str_at(s, (uint32_t)p));
}

static embi_js_value str_code_point_at(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "codePointAt");
    if (!s) {
        return EXC;
    }
    double p;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &p)) {
        return EXC;
    }
    if (p < 0 || p >= embi_js_str_len(s)) {
        return UNDEF;
    }
    uint32_t next;
    return embi_js_from_int32((int32_t)cp_at(s, (uint32_t)p, &next));
}

static embi_js_value str_concat(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    /* J5g: the pieces are joined as values (embi_js_str_concat_v), so a long result is a rope and
     * no piece is copied here. this_string is the RequireObjectCoercible + ToString of `this`. */
    embi_js_value acc;
    if (embi_js_is_string(this_v)) {
        acc = this_v;
    } else {
        embi_js_string *s = this_string(rt, this_v, "concat");
        if (!s) {
            return EXC;
        }
        acc = embi_js_from_string(s);
    }
    for (int i = 0; i < argc; i++) {
        embi_js_value part = argv[i];
        if (!embi_js_is_string(part)) {
            embi_js_string *a = embi_js_to_string(rt, part);
            if (!a) {
                return EXC;
            }
            part = embi_js_from_string(a);
        }
        acc = embi_js_str_concat_v(rt, acc, part);
    }
    return acc;
}

static embi_js_value str_slice(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "slice");
    if (!s) {
        return EXC;
    }
    double from;
    double to;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &from)) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    embi_js_value endv = arg_at(argc, argv, 1);
    if (embi_js_is_undefined(endv)) {
        to = len;
    } else if (!embi_js_to_integer_or_infinity(rt, endv, &to)) {
        return EXC;
    }
    uint32_t a = rel_pos(from, len);
    uint32_t b = rel_pos(to, len);
    if (a >= b) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    return sub_val(rt, s, a, b);
}

static embi_js_value str_substring(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "substring");
    if (!s) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t a;
    uint32_t b;
    if (!clamp_int(rt, arg_at(argc, argv, 0), len, &a)) {
        return EXC;
    }
    embi_js_value endv = arg_at(argc, argv, 1);
    if (embi_js_is_undefined(endv)) {
        b = len;
    } else if (!clamp_int(rt, endv, len, &b)) {
        return EXC;
    }
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return sub_val(rt, s, a, b);
}

/* Annex B String.prototype.substr(start, length). */
static embi_js_value str_substr(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "substr");
    if (!s) {
        return EXC;
    }
    double size = embi_js_str_len(s);
    double start;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &start)) {
        return EXC;
    }
    if (start == -INFINITY) {
        start = 0;
    } else if (start < 0) {
        start = size + start < 0 ? 0 : size + start;
    } else if (start > size) {
        start = size;
    }
    double length = size;
    embi_js_value lenv = arg_at(argc, argv, 1);
    if (!embi_js_is_undefined(lenv) && !embi_js_to_integer_or_infinity(rt, lenv, &length)) {
        return EXC;
    }
    if (length < 0) {
        length = 0;
    } else if (length > size) {
        length = size;
    }
    double end = start + length;
    if (end > size) {
        end = size;
    }
    if (end <= start) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    return sub_val(rt, s, (uint32_t)start, (uint32_t)end);
}

/* ------------------------------------------------------------------ search */

static embi_js_value str_index_of_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "indexOf");
    if (!s) {
        return EXC;
    }
    embi_js_string *needle = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!needle) {
        return EXC;
    }
    uint32_t start;
    if (!clamp_int(rt, arg_at(argc, argv, 1), embi_js_str_len(s), &start)) {
        return EXC;
    }
    return embi_js_number((double)str_index_of(s, start, needle));
}

static embi_js_value str_last_index_of_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "lastIndexOf");
    if (!s) {
        return EXC;
    }
    embi_js_string *needle = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!needle) {
        return EXC;
    }
    double num;
    if (!embi_js_to_number(rt, arg_at(argc, argv, 1), &num)) {
        return EXC;
    }
    double pos;
    if (num != num) {
        pos = INFINITY;
    } else {
        pos = trunc(num);
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t start = pos <= 0 ? 0 : (pos >= len ? len : (uint32_t)pos);
    return embi_js_number((double)str_last_index_of(s, start, needle));
}

static embi_js_value str_includes(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "includes");
    if (!s) {
        return EXC;
    }
    int re = is_regexp(rt, arg_at(argc, argv, 0));
    if (re == EMBI_JS_THREW) {
        return EXC;
    }
    if (re == EMBI_JS_YES) {
        return embi_js_throw_type_error(
            rt, "First argument to String.prototype.includes must not be a regular expression");
    }
    embi_js_string *needle = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!needle) {
        return EXC;
    }
    uint32_t start;
    if (!clamp_int(rt, arg_at(argc, argv, 1), embi_js_str_len(s), &start)) {
        return EXC;
    }
    return embi_js_from_bool(str_index_of(s, start, needle) >= 0);
}

static embi_js_value str_starts_with(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "startsWith");
    if (!s) {
        return EXC;
    }
    int re = is_regexp(rt, arg_at(argc, argv, 0));
    if (re == EMBI_JS_THREW) {
        return EXC;
    }
    if (re == EMBI_JS_YES) {
        return embi_js_throw_type_error(
            rt, "First argument to String.prototype.startsWith must not be a regular expression");
    }
    embi_js_string *needle = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!needle) {
        return EXC;
    }
    uint32_t start;
    if (!clamp_int(rt, arg_at(argc, argv, 1), embi_js_str_len(s), &start)) {
        return EXC;
    }
    uint32_t nl = embi_js_str_len(needle);
    if (nl > embi_js_str_len(s) - start) {
        return EMBI_JS_FALSE;
    }
    return embi_js_from_bool(match_at(s, start, needle));
}

static embi_js_value str_ends_with(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "endsWith");
    if (!s) {
        return EXC;
    }
    int re = is_regexp(rt, arg_at(argc, argv, 0));
    if (re == EMBI_JS_THREW) {
        return EXC;
    }
    if (re == EMBI_JS_YES) {
        return embi_js_throw_type_error(
            rt, "First argument to String.prototype.endsWith must not be a regular expression");
    }
    embi_js_string *needle = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!needle) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t end = len;
    embi_js_value endv = arg_at(argc, argv, 1);
    if (!embi_js_is_undefined(endv) && !clamp_int(rt, endv, len, &end)) {
        return EXC;
    }
    uint32_t nl = embi_js_str_len(needle);
    if (nl > end) {
        return EMBI_JS_FALSE;
    }
    return embi_js_from_bool(match_at(s, end - nl, needle));
}

/* localeCompare: code-unit order, reported as -1, 0 or 1 (no locale data yet). */
static embi_js_string *normalize_str(embi_js_runtime *rt, embi_js_string *s, enum embi_norm_form nf);

static embi_js_value str_locale_compare(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "localeCompare");
    if (!s) {
        return EXC;
    }
    embi_js_string *that = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!that) {
        return EXC;
    }
    int c = embi_js_str_compare(s, that);
    if (c != 0) {
        /* Canonically equivalent strings compare equal (ECMA-262 22.1.3.12): compare their NFC
         * forms when the code units differ. */
        embi_js_string *a = normalize_str(rt, s, EMBI_NORM_NFC);
        embi_js_string *b = normalize_str(rt, that, EMBI_NORM_NFC);
        c = embi_js_str_compare(a, b);
    }
    return embi_js_from_int32(c < 0 ? -1 : (c > 0 ? 1 : 0));
}

/* ------------------------------------------------------------------ regular-expression entry points */

/* match, search and matchAll with a Symbol method on the argument, otherwise a RegExp made from
 * it. magic selects the symbol: 0 @@match, 1 @@search, 2 @@matchAll. */
static embi_js_value str_regexp_dispatch(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    static const char *const names[] = {"match", "search", "matchAll"};
    static const enum embi_js_wk syms[] = {EMBI_JS_WK_MATCH, EMBI_JS_WK_SEARCH,
                                           EMBI_JS_WK_MATCH_ALL};
    embi_js_runtime *rt = r->rt;
    embi_js_value regexp = arg_at(argc, argv, 0);
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "String.prototype.%s called on null or undefined",
                                        names[magic]);
    }
    embi_js_value sym = embi_js_wk_sym(rt, syms[magic]);
    if (magic == 2 && !embi_js_is_nullish(regexp)) {
        if (!require_global_flag(rt, regexp, "String.prototype.matchAll")) {
            return EXC;
        }
    }
    if (embi_js_is_object(regexp)) { /* ES2025: the Symbol method is looked up on objects only */
        embi_js_value m = embi_js_get_method(rt, regexp, sym);
        if (m == EXC) {
            return EXC;
        }
        if (!embi_js_is_undefined(m)) {
            embi_js_value args[1] = {this_v};
            return embi_js_call(rt, m, regexp, 1, args);
        }
    }
    embi_js_string *s = embi_js_to_string(rt, this_v);
    if (!s) {
        return EXC;
    }
    embi_js_value flags = UNDEF;
    if (magic == 2) {
        flags = ascii_val(rt, "g");
        if (flags == EXC) {
            return EXC;
        }
    }
    embi_js_value rx = regexp_create(r, regexp, flags);
    if (rx == EXC) {
        return EXC;
    }
    embi_js_value sv = embi_js_from_string(s);
    return embi_js_invoke(rt, rx, sym, 1, &sv);
}

/* ------------------------------------------------------------------ replace and split */

/* replace (magic 0) and replaceAll (magic 1). */
static embi_js_value str_replace_common(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    bool all = magic == 1;
    const char *name = all ? "replaceAll" : "replace";
    embi_js_value search = arg_at(argc, argv, 0);
    embi_js_value replace = arg_at(argc, argv, 1);
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "String.prototype.%s called on null or undefined",
                                        name);
    }
    if (embi_js_is_object(search)) { /* ES2025: primitives never reach the Symbol.replace lookup */
        if (all && !require_global_flag(rt, search, "String.prototype.replaceAll")) {
            return EXC;
        }
        embi_js_value m = embi_js_get_method(rt, search, embi_js_wk_sym(rt, EMBI_JS_WK_REPLACE));
        if (m == EXC) {
            return EXC;
        }
        if (!embi_js_is_undefined(m)) {
            embi_js_value args[2] = {this_v, replace};
            return embi_js_call(rt, m, search, 2, args);
        }
    }
    embi_js_string *s = embi_js_to_string(rt, this_v);
    if (!s) {
        return EXC;
    }
    embi_js_string *needle = embi_js_to_string(rt, search);
    if (!needle) {
        return EXC;
    }
    bool functional = embi_js_is_callable(replace);
    embi_js_string *rep = nullptr;
    if (!functional) {
        rep = embi_js_to_string(rt, replace);
        if (!rep) {
            return EXC;
        }
    }
    uint32_t len = embi_js_str_len(s);
    uint32_t nl = embi_js_str_len(needle);
    uint32_t advance = nl > 1 ? nl : 1;

    /* Match positions are plain integers, so they can live in a C buffer. */
    EMBI_VEC(uint32_t) pos = {nullptr, 0, 0};
    int64_t p = str_index_of(s, 0, needle);
    while (p >= 0) {
        embi_vec_push(&pos, (uint32_t)p);
        if (!all) {
            break;
        }
        uint64_t from = (uint64_t)p + advance;
        if (from > len) {
            break;
        }
        p = str_index_of(s, (uint32_t)from, needle);
    }
    if (pos.len == 0) {
        embi_vec_free(&pos);
        return embi_js_from_string(s);
    }

    u16_buf b = {nullptr, 0, 0};
    uint32_t last = 0;
    for (uint32_t k = 0; k < pos.len; k++) {
        uint32_t at = pos.data[k];
        buf_push_str(&b, s, last, at);
        if (functional) {
            embi_js_value args[3] = {embi_js_from_string(needle), embi_js_number((double)at),
                                     embi_js_from_string(s)};
            embi_js_value res = embi_js_call(rt, replace, UNDEF, 3, args);
            embi_js_string *rs = res == EXC ? nullptr : embi_js_to_string(rt, res);
            if (!rs) {
                embi_vec_free(&pos);
                embi_vec_free(&b);
                return EXC;
            }
            buf_push_str(&b, rs, 0, embi_js_str_len(rs));
        } else {
            append_substitution(&b, needle, s, at, rep);
        }
        last = at + nl;
    }
    buf_push_str(&b, s, last, len);
    embi_vec_free(&pos);
    return buf_finish(rt, &b);
}

/* split(separator, limit) */
static embi_js_value str_split(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value sep = arg_at(argc, argv, 0);
    embi_js_value limv = arg_at(argc, argv, 1);
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "String.prototype.split called on null or undefined");
    }
    if (embi_js_is_object(sep)) { /* ES2025: primitives never reach the Symbol.split lookup */
        embi_js_value m = embi_js_get_method(rt, sep, embi_js_wk_sym(rt, EMBI_JS_WK_SPLIT));
        if (m == EXC) {
            return EXC;
        }
        if (!embi_js_is_undefined(m)) {
            embi_js_value args[2] = {this_v, limv};
            return embi_js_call(rt, m, sep, 2, args);
        }
    }
    embi_js_string *s = embi_js_to_string(rt, this_v);
    if (!s) {
        return EXC;
    }
    uint32_t lim = UINT32_MAX;
    if (!embi_js_is_undefined(limv) && !embi_js_to_uint32(rt, limv, &lim)) {
        return EXC;
    }
    embi_js_string *r_sep = embi_js_to_string(rt, sep);
    if (!r_sep) {
        return EXC;
    }
    embi_js_object *arr = embi_js_array_create_default(rt, 0.0);
    if (lim == 0) {
        return embi_js_from_object(arr);
    }
    uint32_t len = embi_js_str_len(s);
    if (embi_js_is_undefined(sep)) {
        if (embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(0),
                                                  embi_js_from_string(s)) == EMBI_JS_THREW) {
            return EXC;
        }
        return embi_js_from_object(arr);
    }
    uint32_t sl = embi_js_str_len(r_sep);
    uint32_t count = 0;
    if (sl == 0) {
        uint32_t n = len < lim ? len : lim;
        for (uint32_t i = 0; i < n; i++) {
            embi_js_value unit = sub_val(rt, s, i, i + 1);
            if (unit == EXC ||
                embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(count++),
                                                      unit) == EMBI_JS_THREW) {
                return EXC;
            }
        }
        return embi_js_from_object(arr);
    }
    if (len == 0) {
        if (embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(0),
                                                  embi_js_from_string(s)) == EMBI_JS_THREW) {
            return EXC;
        }
        return embi_js_from_object(arr);
    }
    uint32_t p = 0;
    int64_t q = str_index_of(s, 0, r_sep);
    while (q >= 0) {
        embi_js_value part = sub_val(rt, s, p, (uint32_t)q);
        if (part == EXC ||
            embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(count++),
                                                  part) == EMBI_JS_THREW) {
            return EXC;
        }
        if (count == lim) {
            return embi_js_from_object(arr);
        }
        p = (uint32_t)q + sl;
        q = str_index_of(s, p, r_sep);
    }
    embi_js_value tail = sub_val(rt, s, p, len);
    if (tail == EXC ||
        embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(count), tail) ==
            EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(arr);
}

/* ------------------------------------------------------------------ transforms */

static embi_js_value str_to_case(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    /* magic: 0 toLowerCase, 1 toUpperCase; the locale variants share them. */
    static const char *const names[] = {"toLowerCase", "toUpperCase"};
    embi_js_string *s = this_string(rt, this_v, names[magic & 1]);
    if (!s) {
        return EXC;
    }
    return change_case(rt, s, (magic & 1) != 0);
}

/* trim (magic 0), trimStart (1), trimEnd (2). */
static embi_js_value str_trim(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    static const char *const names[] = {"trim", "trimStart", "trimEnd"};
    embi_js_string *s = this_string(rt, this_v, names[magic]);
    if (!s) {
        return EXC;
    }
    uint32_t a = 0;
    uint32_t b = embi_js_str_len(s);
    if (magic != 2) {
        while (a < b && is_js_space(embi_js_str_at(s, a))) {
            a++;
        }
    }
    if (magic != 1) {
        while (b > a && is_js_space(embi_js_str_at(s, b - 1))) {
            b--;
        }
    }
    return sub_val(rt, s, a, b);
}

/* The normal form of s in form nf. A string of ASCII is its own normal form in every form, so it
 * is returned as it is. */
static embi_js_string *normalize_str(embi_js_runtime *rt, embi_js_string *s, enum embi_norm_form nf)
{
    uint32_t n = embi_js_str_len(s);
    bool ascii = true;
    for (uint32_t i = 0; i < n && ascii; i++) {
        ascii = embi_js_str_at(s, i) < 0x80u;
    }
    if (ascii) {
        return s;
    }
    /* A Latin-1 source is widened into a unit buffer; a UTF-16 one is read in place. */
    u16_buf units = {nullptr, 0, 0};
    const uint16_t *src = nullptr;
    if (embi_js_str_is_wide(s)) {
        src = embi_js_str_wide(s);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            embi_vec_push(&units, (uint16_t)embi_js_str_at(s, i));
        }
        src = units.data;
    }
    embi_norm_units out = {nullptr, 0, 0};
    embi_norm_run(src, n, nf, &out);
    embi_js_string *res = embi_js_str_new_utf16(rt, out.data, out.len);
    embi_vec_free(&out);
    embi_vec_free(&units);
    return res;
}

/* String.prototype.normalize (ECMA-262 22.1.3.15). The form defaults to NFC. */
static embi_js_value str_normalize(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "normalize");
    if (!s) {
        return EXC;
    }
    enum embi_norm_form nf = EMBI_NORM_NFC;
    embi_js_value fv = arg_at(argc, argv, 0);
    if (!embi_js_is_undefined(fv)) {
        embi_js_string *form = embi_js_to_string(rt, fv);
        if (!form) {
            return EXC;
        }
        static const char *const forms[] = {"NFC", "NFD", "NFKC", "NFKD"};
        bool ok = false;
        for (size_t i = 0; i < ARR_LEN(forms) && !ok; i++) {
            if (str_is_ascii(form, forms[i])) {
                nf = (enum embi_norm_form)i;
                ok = true;
            }
        }
        if (!ok) {
            return embi_js_throw_range_error(
                rt, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
        }
    }
    return str_val(normalize_str(rt, s, nf));
}

/* String.prototype.isWellFormed and toWellFormed (ES2024): a lone surrogate is a code unit in
 * D800..DFFF that is not part of a pair. magic 0 is isWellFormed, 1 is toWellFormed. */
static embi_js_value str_well_formed(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, magic == 0 ? "isWellFormed" : "toWellFormed");
    if (!s) {
        return EXC;
    }
    uint32_t n = embi_js_str_len(s);
    if (!embi_js_str_is_wide(s)) {
        return magic == 0 ? EMBI_JS_TRUE : embi_js_from_string(s);
    }
    const uint16_t *w = embi_js_str_wide(s);
    u16_buf out = {nullptr, 0, 0};
    bool lone = false;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t u = w[i];
        bool high = u >= 0xD800u && u <= 0xDBFFu;
        bool low = u >= 0xDC00u && u <= 0xDFFFu;
        if (high && i + 1 < n && w[i + 1] >= 0xDC00u && w[i + 1] <= 0xDFFFu) {
            if (magic == 1) {
                embi_vec_push(&out, u);
                embi_vec_push(&out, w[i + 1]);
            }
            i++;
            continue;
        }
        if (high || low) {
            lone = true;
            if (magic == 0) {
                break;
            }
            embi_vec_push(&out, 0xFFFDu);
            continue;
        }
        if (magic == 1) {
            embi_vec_push(&out, u);
        }
    }
    embi_js_value result;
    if (magic == 0) {
        result = embi_js_from_bool(!lone);
    } else if (!lone) {
        result = embi_js_from_string(s);
    } else {
        result = str_val(embi_js_str_new_utf16(rt, out.data, out.len));
    }
    embi_vec_free(&out);
    return result;
}

/* Result strings are allocated at their final length and filled in place, so a repeat or pad does
 * not build a second copy in a C-heap buffer. The width (Latin-1 or UTF-16) is fixed when the
 * string is allocated, so the caller must decide it first. */
static embi_js_string *str_alloc(embi_js_runtime *rt, uint32_t len, bool wide)
{
    size_t bytes = sizeof(embi_js_string) + (wide ? 2u : 1u) * (size_t)len;
    embi_js_string *o = (embi_js_string *)embi_js_alloc_cell(rt, bytes, EMBI_JS_CELL_STRING);
    o->len_flags = len | (wide ? EMBI_JS_STR_WIDE_BIT : 0u);
    o->hash = 0;
    return o;
}

static uint8_t *str_bytes(embi_js_string *o)
{
    return (uint8_t *)o + sizeof(embi_js_string);
}

static void str_set(embi_js_string *o, uint32_t i, uint16_t u)
{
    if (embi_js_str_is_wide(o)) {
        ((uint16_t *)str_bytes(o))[i] = u;
    } else {
        str_bytes(o)[i] = (uint8_t)u;
    }
}

/* Writes count units of src, starting at from, into o at off. */
static void str_write(embi_js_string *o, uint32_t off, const embi_js_string *src, uint32_t from,
                      uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        str_set(o, off + i, embi_js_str_at(src, from + i));
    }
}

/* Repeats the first period units of the region at dst, which has unit_size bytes per unit, until
 * total units are filled. period must divide every prefix length the doubling reaches (it does,
 * because the filled length starts at period and only doubles or ends). */
static void str_tile(uint8_t *dst, size_t unit_size, uint32_t period, uint32_t total)
{
    uint32_t filled = period;
    while (filled < total) {
        uint32_t n = filled < total - filled ? filled : total - filled;
        memcpy(dst + (size_t)filled * unit_size, dst, (size_t)n * unit_size);
        filled += n;
    }
}

/* padStart (magic 0) and padEnd (magic 1). */
static embi_js_value str_pad(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    static const char *const names[] = {"padStart", "padEnd"};
    embi_js_string *s = this_string(rt, this_v, names[magic]);
    if (!s) {
        return EXC;
    }
    double max;
    if (!embi_js_to_length(rt, arg_at(argc, argv, 0), &max)) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    if (max <= len) {
        return embi_js_from_string(s);
    }
    embi_js_string *fill;
    embi_js_value fv = arg_at(argc, argv, 1);
    if (embi_js_is_undefined(fv)) {
        fill = embi_js_str_new_latin1(rt, (const uint8_t *)" ", 1);
    } else {
        fill = embi_js_to_string(rt, fv);
    }
    if (!fill) {
        return EXC;
    }
    if (embi_js_str_len(fill) == 0) {
        return embi_js_from_string(s);
    }
    if (max > STR_MAX) {
        return embi_js_throw_range_error(rt, "Invalid string length");
    }
    uint32_t total = (uint32_t)max;
    uint32_t pad_len = total - len;
    uint32_t fl = embi_js_str_len(fill);
    /* The pattern uses the first min(pad_len, fl) fill units; the result is wide when the source
     * is or when one of those units is above 0xFF (the canonical representation). */
    uint32_t used = pad_len < fl ? pad_len : fl;
    bool wide = embi_js_str_is_wide(s);
    for (uint32_t i = 0; !wide && i < used; i++) {
        wide = embi_js_str_at(fill, i) > 0xFF;
    }
    embi_js_string *out = str_alloc(rt, total, wide);
    uint32_t pad_off = magic == 0 ? 0 : len;
    uint32_t src_off = magic == 0 ? pad_len : 0;
    str_write(out, src_off, s, 0, len);
    str_write(out, pad_off, fill, 0, used);
    str_tile(str_bytes(out) + (size_t)pad_off * (wide ? 2u : 1u), wide ? 2u : 1u, used,
             pad_len);
    return str_val(out);
}

static embi_js_value str_repeat(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = this_string(rt, this_v, "repeat");
    if (!s) {
        return EXC;
    }
    double n;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &n)) {
        return EXC;
    }
    if (n < 0 || !is_finite(n)) {
        return embi_js_throw_range_error(rt, "Invalid count value");
    }
    uint32_t len = embi_js_str_len(s);
    if (n == 0 || len == 0) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    if (n * (double)len > (double)STR_MAX) {
        return embi_js_throw_range_error(rt, "Invalid string length");
    }
    uint32_t total = (uint32_t)((uint64_t)n * len);
    bool wide = embi_js_str_is_wide(s);
    embi_js_string *out = str_alloc(rt, total, wide);
    str_write(out, 0, s, 0, len);
    str_tile(str_bytes(out), wide ? 2u : 1u, len, total);
    return str_val(out);
}

/* Annex B HTML methods: CreateHTML(string, tag, attribute, value). */
struct html_def {
    const char *tag;
    const char *attr; /* nullptr when the method takes no attribute */
};

static const struct html_def html_defs[] = {
    {"a", "name"},  {"big", nullptr},   {"blink", nullptr}, {"b", nullptr},
    {"tt", nullptr}, {"font", "color"}, {"font", "size"},    {"i", nullptr},
    {"a", "href"},  {"small", nullptr}, {"strike", nullptr}, {"sub", nullptr},
    {"sup", nullptr},
};

static embi_js_value str_html(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    static const char *const names[] = {"anchor",   "big",  "blink",  "bold", "fixed",
                                        "fontcolor", "fontsize", "italics", "link", "small",
                                        "strike",   "sub",  "sup"};
    const struct html_def *d = &html_defs[magic];
    embi_js_string *s = this_string(rt, this_v, names[magic]);
    if (!s) {
        return EXC;
    }
    embi_js_string *value = nullptr;
    if (d->attr) {
        value = embi_js_to_string(rt, arg_at(argc, argv, 0));
        if (!value) {
            return EXC;
        }
    }
    u16_buf b = {nullptr, 0, 0};
    buf_push_ascii(&b, "<");
    buf_push_ascii(&b, d->tag);
    if (d->attr) {
        buf_push_ascii(&b, " ");
        buf_push_ascii(&b, d->attr);
        buf_push_ascii(&b, "=\"");
        for (uint32_t i = 0; i < embi_js_str_len(value); i++) {
            uint16_t c = embi_js_str_at(value, i);
            if (c == '"') {
                buf_push_ascii(&b, "&quot;");
            } else {
                embi_vec_push(&b, c);
            }
        }
        buf_push_ascii(&b, "\"");
    }
    buf_push_ascii(&b, ">");
    buf_push_str(&b, s, 0, embi_js_str_len(s));
    buf_push_ascii(&b, "</");
    buf_push_ascii(&b, d->tag);
    buf_push_ascii(&b, ">");
    return buf_finish(rt, &b);
}

/* toString (magic 0) and valueOf (magic 1). */
static embi_js_value str_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_string(this_v)) {
        return this_v;
    }
    if (embi_js_is_object(this_v) && embi_js_as_object(this_v)->klass == &embi_js_class_string) {
        return embi_js_wrapper_value(embi_js_as_object(this_v));
    }
    return embi_js_throw_type_error(rt, "String.prototype.%s requires that 'this' be a String",
                                    magic ? "valueOf" : "toString");
}

/* String.prototype.at, charAt and so on use the entry points above. The iterator method
 * (String.prototype[@@iterator]) belongs to J2. */

/* ------------------------------------------------------------------ installation */

struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
    int magic;
};

static const struct method_def proto_methods[] = {
    {"at", str_at, 1, 0},
    {"charAt", str_char_at, 1, 0},
    {"charCodeAt", str_char_code_at, 1, 0},
    {"codePointAt", str_code_point_at, 1, 0},
    {"concat", str_concat, 1, 0},
    {"endsWith", str_ends_with, 1, 0},
    {"includes", str_includes, 1, 0},
    {"indexOf", str_index_of_fn, 1, 0},
    {"lastIndexOf", str_last_index_of_fn, 1, 0},
    {"localeCompare", str_locale_compare, 1, 0},
    {"match", str_regexp_dispatch, 1, 0},
    {"matchAll", str_regexp_dispatch, 1, 2},
    {"isWellFormed", str_well_formed, 0, 0},
    {"normalize", str_normalize, 0, 0},
    {"padEnd", str_pad, 1, 1},
    {"padStart", str_pad, 1, 0},
    {"repeat", str_repeat, 1, 0},
    {"replace", str_replace_common, 2, 0},
    {"replaceAll", str_replace_common, 2, 1},
    {"search", str_regexp_dispatch, 1, 1},
    {"slice", str_slice, 2, 0},
    {"split", str_split, 2, 0},
    {"startsWith", str_starts_with, 1, 0},
    {"substr", str_substr, 2, 0},
    {"substring", str_substring, 2, 0},
    {"toLocaleLowerCase", str_to_case, 0, 0},
    {"toLocaleUpperCase", str_to_case, 0, 1},
    {"toWellFormed", str_well_formed, 0, 1},
    {"toLowerCase", str_to_case, 0, 0},
    {"toString", str_to_string, 0, 0},
    {"toUpperCase", str_to_case, 0, 1},
    {"trim", str_trim, 0, 0},
    {"trimEnd", str_trim, 0, 2},
    {"valueOf", str_to_string, 0, 1},
    {"trimStart", str_trim, 0, 1},
};

/* The Annex B HTML methods, in the order of html_defs (their magic values). */
static const char *const html_names[] = {"anchor",    "big",   "blink",  "bold",  "fixed",
                                         "fontcolor", "fontsize", "italics", "link", "small",
                                         "strike",    "sub",   "sup"};
static const int html_lengths[] = {1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0};

static embi_js_object *make_ctor(embi_js_realm *r, const char *name, int length,
                                 const embi_js_class *klass)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *f = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], klass);
    f->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                       embi_js_number((double)length), ATTR_C);
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, name, strlen(name))),
                       ATTR_C);
    return f;
}

/* ------------------------------------------------------------------ string iterators (J2c) */

/* A string iterator's state is a varray [string, next index]. The string slot is undefined once
 * the iteration is over. Steps are by code point (a surrogate pair is one step). */
enum { SI_STR, SI_INDEX, SI_N };

static void string_iter_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, &((embi_js_varray *)o->internal)->hdr);
    }
}

static const embi_js_class class_string_iter = {.name = "String Iterator",
                                                .trace = string_iter_trace};

/* String.prototype[@@iterator]. */
static embi_js_value string_iter_make(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "String.prototype[Symbol.iterator] called on null or undefined");
    }
    embi_js_string *s = embi_js_to_string(rt, this_v);
    if (!s) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_varray *st = embi_js_varray_new(rt, SI_N);
    EMBI_JS_STORE(st, st->elems[SI_STR], embi_js_from_string(s));
    EMBI_JS_WB(st, st->elems[SI_STR]);
    EMBI_JS_STORE(st, st->elems[SI_INDEX], embi_js_from_int32(0));
    embi_js_object *it = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_STRING_ITERATOR],
                                                  &class_string_iter);
    it->internal = st;
    EMBI_JS_WB_STRUCT(it);
    return embi_js_from_object(it);
}

/* %StringIteratorPrototype%.next. */
static embi_js_value string_iter_next(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v) || embi_js_as_object(this_v)->klass != &class_string_iter) {
        return embi_js_throw_type_error(rt, "String Iterator next called on incompatible receiver");
    }
    embi_js_varray *st = embi_js_as_object(this_v)->internal;
    if (!embi_js_is_string(st->elems[SI_STR])) {
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    embi_js_string *s = embi_js_str_flat(rt, st->elems[SI_STR]);
    uint32_t i = (uint32_t)embi_js_as_int32(st->elems[SI_INDEX]);
    uint32_t n = embi_js_str_len(s);
    if (i >= n) {
        EMBI_JS_STORE(st, st->elems[SI_STR], UNDEF);
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    uint16_t units[2];
    uint32_t w = 1;
    units[0] = embi_js_str_at(s, i);
    if (units[0] >= 0xD800u && units[0] <= 0xDBFFu && i + 1u < n) {
        uint16_t lo = embi_js_str_at(s, i + 1u);
        if (lo >= 0xDC00u && lo <= 0xDFFFu) {
            units[1] = lo;
            w = 2;
        }
    }
    /* A one-unit step below 256 is a cached atom (no allocation). */
    embi_js_string *cp = w == 1 ? embi_js_unit_string(rt, units[0])
                                : embi_js_str_new_utf16(rt, units, w);
    if (!cp) {
        return EMBI_JS_EXCEPTION;
    }
    EMBI_JS_STORE(st, st->elems[SI_INDEX], embi_js_from_int32((int32_t)(i + w)));
    return embi_js_iter_result_new(rt, embi_js_from_string(cp), false);
}

/* %StringIteratorPrototype%, the next function the for-of fast path compares with, and
 * String.prototype[@@iterator]. */
static void string_iterators_init(embi_js_realm *r, embi_js_object *string_proto)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_ITERATOR]);
    rt->protos[EMBI_JS_PROTO_STRING_ITERATOR] = embi_js_from_object(proto);
    embi_js_object *next = embi_js_native_function_new(r, "next", 0, string_iter_next, 0);
    rt->protos[EMBI_JS_PROTO_STRING_ITER_NEXT] = embi_js_from_object(next);
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "next"), embi_js_from_object(next), ATTR_WC);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "String Iterator", 15)),
                       ATTR_C);

    embi_js_object *iter = embi_js_native_function_new(r, "[Symbol.iterator]", 0, string_iter_make, 0);
    rt->protos[EMBI_JS_PROTO_STRING_ITER] = embi_js_from_object(iter);
    embi_js_define_raw(rt, string_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(iter), ATTR_WC);
}

void embi_js_init_builtins_string(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];

    /* String.prototype is itself a String wrapper whose value is "". */
    embi_js_object *proto =
        embi_js_object_new_class(rt, obj_proto, &embi_js_class_string);
    proto->internal = (void *)(uintptr_t)embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    EMBI_JS_WB_STRUCT(proto);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(0.0),
                       0);
    rt->protos[EMBI_JS_PROTO_STRING] = embi_js_from_object(proto);

    /* Symbol.prototype is created here only when the Symbol init has not run yet (it runs first). */
    if (embi_js_is_empty(rt->protos[EMBI_JS_PROTO_SYMBOL])) {
        embi_js_object *sym = embi_js_object_new(rt, obj_proto);
        rt->protos[EMBI_JS_PROTO_SYMBOL] = embi_js_from_object(sym);
    }

    embi_js_object *ctor = make_ctor(r, "String", 1, &string_ctor_class);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "String"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, ctor, "fromCharCode", string_from_char_code, 1, 0);
    embi_js_define_method(r, ctor, "fromCodePoint", string_from_code_point, 1, 0);
    embi_js_define_method(r, ctor, "raw", string_raw, 1, 0);

    for (size_t i = 0; i < ARR_LEN(proto_methods); i++) {
        const struct method_def *m = &proto_methods[i];
        embi_js_define_method(r, proto, m->name, m->fn, m->length, m->magic);
    }

    /* Annex B aliases: trimLeft and trimRight are the same function objects as trimStart and
     * trimEnd. */
    embi_js_value ts = embi_js_get(rt, proto, embi_js_key_c(rt, "trimStart"));
    embi_js_value te = embi_js_get(rt, proto, embi_js_key_c(rt, "trimEnd"));
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "trimLeft"), ts, ATTR_WC);
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "trimRight"), te, ATTR_WC);

    for (size_t i = 0; i < ARR_LEN(html_names); i++) {
        embi_js_define_method(r, proto, html_names[i], str_html, html_lengths[i], (int)i);
    }
    string_iterators_init(r, proto);
}
