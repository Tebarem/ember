/*
 * Minimal Intl (ECMA-402 subset). Every request resolves to en-US, and
 * the data is built in: no CLDR tables are vendored.
 *
 *   Intl.getCanonicalLocales, and supportedLocalesOf on each constructor (a tag is supported when
 *   it is en-US, with any subtags).
 *   Intl.NumberFormat: decimal, percent, currency (built-in symbols for USD, EUR, GBP, JPY and a few
 *   more, the ISO code otherwise), unit (a table of common units), grouping, digit options (integer,
 *   fraction and significant), rounding modes, signDisplay, trailingZeroDisplay, formatToParts,
 *   resolvedOptions, BigInt input.
 *   Intl.DateTimeFormat: en-US patterns for the component options and the dateStyle and timeStyle
 *   presets, the time zones UTC, the local zone and fixed offsets, formatToParts, resolvedOptions.
 *   Intl.Collator: code point order of the NFD form, with case folding through the engine's simple
 *   lowercase table, sensitivity, numeric, ignorePunctuation and caseFirst.
 *   Intl.PluralRules: en cardinal and ordinal rules.
 *
 * Options that this build does not implement are a RangeError, never a silently different value:
 * notation other than standard, era, dayPeriod, roundingIncrement other than 1 and roundingPriority
 * other than auto. A well-formed numberingSystem or calendar falls back to the only supported one
 * (latn, gregory), which is what ECMA-402 specifies for an unsupported value.
 *
 * The toLocaleString family (Number, BigInt, Date, Array, String.prototype.localeCompare) is
 * installed here, last, so it replaces the toString-based versions of the other builtins.
 *
 * Numbers are formatted from their shortest round-trip decimal digits (the digits that Number
 * toString prints), and every rounding mode works on that digit string, so 1.005 rounds to 1.01 as
 * in browsers. Date fields are computed from the time value: the local offset comes from the
 * Date.prototype.getTimezoneOffset captured at init, so the test pin hook applies, and user patches
 * of Date.prototype have no effect on the output.
 */
#include "base/base.h"
#include "js/bigint.h"
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/unicode_norm.h"
#include "num/num.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The generated simple case mappings (the table builtins_string.c uses). Only the lowercase tables
 * are read here, so the unused ones are not warned about. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "js/unicode_case.inc"
#pragma GCC diagnostic pop

#define ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_DEF EMBI_JS_ATTR_DEFAULT

#define NBSP "\xC2\xA0"
#define INFINITY_UTF8 "\xE2\x88\x9E"

/* ------------------------------------------------------------------ root record */

/* Values that the native code keeps alive: the Date constructor and its getTime, the prototypes
 * of the four constructors, and the native targets of the bound getters. */
enum {
    R_DATE,
    R_GETTIME,
    R_GETTZOFF,
    R_NF_PROTO,
    R_DTF_PROTO,
    R_COLL_PROTO,
    R_PR_PROTO,
    R_NF_FMT,
    R_DTF_FMT,
    R_COLL_CMP,
    R_COUNT
};

static void root_mark(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, user);
}

static embi_js_varray *record(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == root_mark) {
            return rt->roots.data[i].user;
        }
    }
    EMBI_ASSERT(false);
    return nullptr;
}

static embi_js_value slot(embi_js_runtime *rt, int i)
{
    return record(rt)->elems[i];
}

/* ------------------------------------------------------------------ small helpers */

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value str_val(embi_js_runtime *rt, const char *s, size_t n)
{
    embi_js_string *p = embi_js_str_new_utf8(rt, s, n);
    return p ? embi_js_from_string(p) : EXC;
}

static embi_js_value cstr(embi_js_runtime *rt, const char *s)
{
    return str_val(rt, s, strlen(s));
}

static bool str_is(const embi_js_string *s, const char *lit)
{
    size_t n = strlen(lit);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (unsigned char)lit[i]) {
            return false;
        }
    }
    return true;
}

static bool str_is_prefix(const embi_js_string *s, const char *lit)
{
    size_t n = strlen(lit);
    if (embi_js_str_len(s) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (unsigned char)lit[i]) {
            return false;
        }
    }
    return true;
}

static void put_val(embi_js_runtime *rt, embi_js_object *o, const char *k, embi_js_value v)
{
    embi_js_define_raw(rt, o, embi_js_key_c(rt, k), v, ATTR_DEF);
}

static void put_str(embi_js_runtime *rt, embi_js_object *o, const char *k, const char *v)
{
    put_val(rt, o, k, cstr(rt, v));
}

static void put_num(embi_js_runtime *rt, embi_js_object *o, const char *k, double v)
{
    put_val(rt, o, k, embi_js_number(v));
}

static void put_bool(embi_js_runtime *rt, embi_js_object *o, const char *k, bool v)
{
    put_val(rt, o, k, v ? EMBI_JS_TRUE : EMBI_JS_FALSE);
}

/* Brand check for a method: the internal state of an object of class k. */
static void *brand(embi_js_runtime *rt, embi_js_value v, const embi_js_class *k, const char *m)
{
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == k) {
        return embi_js_as_object(v)->internal;
    }
    (void)embi_js_throw_type_error(rt, "Method %s called on incompatible receiver", m);
    return nullptr;
}

/* GetPrototypeFromConstructor for the Intl constructors: new.target's prototype, else the
 * intrinsic one. */
static embi_js_value proto_from_nt(embi_js_runtime *rt, embi_js_value nt, embi_js_value fallback)
{
    if (!embi_js_is_object(nt)) {
        return fallback;
    }
    embi_js_value p = embi_js_get(rt, embi_js_as_object(nt), embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
    if (embi_js_is_exception(p)) {
        return EXC;
    }
    return embi_js_is_object(p) ? p : fallback;
}

/* ------------------------------------------------------------------ options */

/* Options object: nullptr for undefined. Returns false when ToObject throws. */
static bool options_obj(embi_js_runtime *rt, embi_js_value v, embi_js_object **out)
{
    *out = nullptr;
    if (embi_js_is_undefined(v)) {
        return true;
    }
    *out = embi_js_to_object(rt, v);
    return *out != nullptr;
}

/* GetOption as a string: 1 present (*out set), 0 absent, -1 thrown. */
static int opt_get_str(embi_js_runtime *rt, embi_js_object *o, const char *name,
                       embi_js_string **out)
{
    if (!o) {
        return 0;
    }
    embi_js_value v = embi_js_get(rt, o, embi_js_key_c(rt, name));
    if (embi_js_is_exception(v)) {
        return -1;
    }
    if (embi_js_is_undefined(v)) {
        return 0;
    }
    *out = embi_js_to_string(rt, v);
    return *out ? 1 : -1;
}

/* GetOption over a list of allowed strings: the index, -1 absent, -2 thrown (RangeError). */
static int opt_choice(embi_js_runtime *rt, embi_js_object *o, const char *name,
                      const char *const *list, int n)
{
    embi_js_string *s = nullptr;
    int r = opt_get_str(rt, o, name, &s);
    if (r < 0) {
        return -2;
    }
    if (r == 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (str_is(s, list[i])) {
            return i;
        }
    }
    (void)embi_js_throw_range_error(rt, "Value out of range for option %s.", name);
    return -2;
}

/* GetBooleanOption: 1 or 0, -1 absent, -2 thrown. */
static int opt_bool(embi_js_runtime *rt, embi_js_object *o, const char *name)
{
    if (!o) {
        return -1;
    }
    embi_js_value v = embi_js_get(rt, o, embi_js_key_c(rt, name));
    if (embi_js_is_exception(v)) {
        return -2;
    }
    if (embi_js_is_undefined(v)) {
        return -1;
    }
    return embi_js_to_boolean(v) ? 1 : 0;
}

/* GetNumberOption: returns false when it throws or the value is out of range. */
static bool opt_int(embi_js_runtime *rt, embi_js_object *o, const char *name, int lo, int hi,
                    int *out, bool *present)
{
    *present = false;
    if (!o) {
        return true;
    }
    embi_js_value v = embi_js_get(rt, o, embi_js_key_c(rt, name));
    if (embi_js_is_exception(v)) {
        return false;
    }
    if (embi_js_is_undefined(v)) {
        return true;
    }
    double d = 0.0;
    if (!embi_js_to_number(rt, v, &d)) {
        return false;
    }
    if (d != d || d < lo || d > hi) {
        (void)embi_js_throw_range_error(rt, "%s value is out of range.", name);
        return false;
    }
    *out = (int)floor(d);
    *present = true;
    return true;
}

/* ------------------------------------------------------------------ locales */

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Validates a BCP 47 tag (unicode_locale_id subset: language, script, region, variants and
 * extensions with no duplicate singletons) and writes its canonical case to out. */
static bool canon_tag(const embi_js_string *s, char *out, size_t *outlen)
{
    uint32_t n = embi_js_str_len(s);
    if (n == 0 || n > 200) {
        return false;
    }
    char a[256];
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        if (c >= 0x80) {
            return false;
        }
        a[i] = (char)c;
    }
    size_t o = 0;
    uint32_t start = 0;
    int idx = 0;
    uint64_t singles = 0;
    bool in_base = true;
    bool priv = false;
    bool region_seen = false;
    bool script_seen = false;
    bool expect = false;
    size_t vstart[16];
    size_t vlen[16];
    int nvar = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i < n && a[i] != '-') {
            continue;
        }
        size_t len = i - start;
        char *p = a + start;
        if (len == 0 || len > 8) {
            return false;
        }
        bool alpha = true;
        bool digit = true;
        for (size_t j = 0; j < len; j++) {
            if (!is_alpha(p[j]) && !is_digit(p[j])) {
                return false;
            }
            alpha = alpha && is_alpha(p[j]);
            digit = digit && is_digit(p[j]);
        }
        for (size_t j = 0; j < len; j++) {
            p[j] = lower(p[j]);
        }
        /* A singleton needs a subtag after it, and another singleton is not one. */
        if (len != 1 || priv) {
            expect = false;
        }
        if (idx == 0) {
            if (!alpha || !(len == 2 || len == 3 || (len >= 5 && len <= 8))) {
                return false;
            }
        } else if (priv) {
            /* Private use subtags: any length 1 .. 8. */
        } else if (len == 1) {
            if (expect) {
                return false;
            }
            uint64_t bit = 1ull << (is_digit(p[0]) ? p[0] - 48 : p[0] - 97 + 10);
            if (!is_alpha(p[0]) && !is_digit(p[0])) {
                return false;
            }
            if (singles & bit) {
                return false;
            }
            singles |= bit;
            in_base = false;
            expect = true;
            if (p[0] == 'x') {
                priv = true;
            }
        } else if (!in_base) {
            if (len < 2) {
                return false;
            }
        } else if (idx <= 2 && len == 4 && alpha && !script_seen && !region_seen) {
            p[0] = upper(p[0]);
            script_seen = true;
        } else if (!region_seen && ((len == 2 && alpha) || (len == 3 && digit))) {
            for (size_t j = 0; j < len; j++) {
                p[j] = upper(p[j]);
            }
            region_seen = true;
        } else if ((len >= 5 && len <= 8) || (len == 4 && is_digit(p[0]))) {
            /* A variant: no repeated variant (case-insensitive). */
            for (int k = 0; k < nvar; k++) {
                if (vlen[k] == len && memcmp(out + vstart[k], p, len) == 0) {
                    return false;
                }
            }
            if (nvar == 16) {
                return false;
            }
            vstart[nvar] = (size_t)(o + (idx > 0 ? 1 : 0));
            vlen[nvar++] = len;
        } else {
            return false;
        }
        if (idx > 0) {
            out[o++] = '-';
        }
        memcpy(out + o, p, len);
        o += len;
        start = i + 1;
        idx++;
    }
    if (expect) {
        return false;
    }
    out[o] = '\0';
    *outlen = o;
    return true;
}

/* Appends one locale element (a string or an object) to the canonical list. */
static bool add_locale(embi_js_runtime *rt, embi_js_object *arr, embi_js_value v)
{
    if (!embi_js_is_object(v) && !embi_js_is_string(v)) {
        (void)embi_js_throw_type_error(rt, "Language ID should be string or object.");
        return false;
    }
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    char buf[256];
    size_t len = 0;
    if (!canon_tag(s, buf, &len)) {
        (void)embi_js_throw_range_error(rt, "Incorrect locale information provided");
        return false;
    }
    embi_js_string *c = embi_js_str_new_utf8(rt, buf, len);
    if (!c) {
        return false;
    }
    uint32_t count = 0;
    double dlen = 0.0;
    if (!embi_js_length_of_array_like(rt, arr, &dlen)) {
        return false;
    }
    count = (uint32_t)dlen;
    for (uint32_t i = 0; i < count; i++) {
        embi_js_value e = embi_js_get(rt, arr, embi_js_key_from_index(i));
        if (embi_js_is_exception(e)) {
            return false;
        }
        if (embi_js_is_string(e) && embi_js_str_equal(embi_js_as_string(e), c)) {
            return true;
        }
    }
    return embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(count),
                                                 embi_js_from_string(c)) != EMBI_JS_THREW;
}

/* CanonicalizeLocaleList: a fresh array of the canonical tags, or nullptr when it throws. */
static embi_js_object *canon_locales(embi_js_runtime *rt, embi_js_value locales)
{
    embi_js_object *arr = embi_js_array_create_default(rt, 0);
    if (!arr || embi_js_is_undefined(locales)) {
        return arr;
    }
    if (embi_js_is_string(locales)) {
        return add_locale(rt, arr, locales) ? arr : nullptr;
    }
    embi_js_object *o = embi_js_to_object(rt, locales);
    if (!o) {
        return nullptr;
    }
    double len = 0.0;
    if (!embi_js_length_of_array_like(rt, o, &len)) {
        return nullptr;
    }
    for (double k = 0.0; k < len; k++) {
        embi_js_value kv = embi_js_number(k);
        embi_js_key key = embi_js_to_property_key(rt, kv);
        if (embi_js_is_exception(key)) {
            return nullptr;
        }
        int has = embi_js_obj_has_property(rt, o, key);
        if (has == EMBI_JS_THREW) {
            return nullptr;
        }
        if (has != EMBI_JS_YES) {
            continue;
        }
        embi_js_value e = embi_js_get(rt, o, key);
        if (embi_js_is_exception(e) || !add_locale(rt, arr, e)) {
            return nullptr;
        }
    }
    return arr;
}

/* The only available locale is en-US (with any extensions): the lookup of ECMA-402 finds no other
 * tag, so supportedLocalesOf keeps exactly these. */
static bool tag_is_supported(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_str_flat(rt, v);
    if (embi_js_str_len(s) < 5 || !str_is_prefix(s, "en-US")) {
        return false;
    }
    return embi_js_str_len(s) == 5 || embi_js_str_at(s, 5) == '-';
}

/* ------------------------------------------------------------------ decimal digits */

/* A finite decimal: the digits d (no leading zero, no trailing zero unless zero itself) and the
 * point position p, so that the value is 0.d x 10^p. Zero is "0" with p = 1. */
typedef struct dec {
    embi_sb d;
    int32_t p;
    bool neg;
} dec;

static void dec_norm(dec *x)
{
    while (x->d.len > 0 && x->d.data[x->d.len - 1] == '0') {
        x->d.len--;
    }
    if (x->d.len == 0) {
        embi_sb_clear(&x->d);
        embi_sb_append_char(&x->d, '0');
        x->p = 1;
    }
}

static bool dec_is_zero(const dec *x)
{
    return x->d.len == 1 && x->d.data[0] == '0';
}

static void dec_free(dec *x)
{
    embi_sb_free(&x->d);
}

/* The shortest round-trip decimal digits of a finite double (embi_num_to_exponential with a negative
 * digit count, which is what Number toString prints), so 1.005 is the digits 1005 at 10^-3 and rounds
 * the way its text does. */
static void dec_from_double(double v, dec *x)
{
    memset(x, 0, sizeof *x);
    x->neg = signbit(v) != 0;
    double a = fabs(v);
    if (a == 0.0) {
        embi_sb_append_char(&x->d, '0');
        x->p = 1;
        return;
    }
    char buf[EMBI_NUM_STR_MAX];
    (void)embi_num_to_exponential(a, -1, buf);
    const char *e = strchr(buf, 'e');
    for (const char *q = buf; q < e; q++) {
        if (is_digit(*q)) {
            embi_sb_append_char(&x->d, *q);
        }
    }
    const char *q = e + 1;
    bool eneg = false;
    if (*q == '+' || *q == '-') {
        eneg = *q == '-';
        q++;
    }
    int32_t ex = 0;
    while (is_digit(*q)) {
        ex = ex * 10 + (*q - '0');
        q++;
    }
    x->p = (eneg ? -ex : ex) + 1;
    dec_norm(x);
}

static bool dec_from_bigint(embi_js_runtime *rt, const embi_js_bigint *b, dec *x)
{
    memset(x, 0, sizeof *x);
    x->neg = embi_js_bigint_is_neg(b);
    embi_js_string *s = embi_js_bigint_to_string(rt, b, 10);
    if (!s) {
        return false;
    }
    uint32_t n = embi_js_str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        if (c >= '0' && c <= '9') {
            embi_sb_append_char(&x->d, (char)c);
        }
    }
    x->p = (int32_t)x->d.len;
    dec_norm(x);
    return true;
}

static char dig(const dec *x, int64_t i)
{
    return (i >= 0 && i < (int64_t)x->d.len) ? x->d.data[i] : '0';
}

/* Rounding modes, in the order of RM_NAMES. */
enum { RM_CEIL, RM_FLOOR, RM_EXPAND, RM_TRUNC, RM_HCEIL, RM_HFLOOR, RM_HEXPAND, RM_HTRUNC, RM_HEVEN };
static const char *const RM_NAMES[9] = {"ceil",       "floor",     "expand",    "trunc", "halfCeil",
                                        "halfFloor",  "halfExpand", "halfTrunc", "halfEven"};

/* Keeps the first keep digits (the unit is 10^(p - keep)) and rounds by mode. */
static void dec_round(dec *x, int64_t keep, int mode)
{
    uint32_t n = x->d.len;
    if (keep >= (int64_t)n) {
        return;
    }
    char *d = x->d.data;
    char first = keep >= 0 ? d[keep] : '0';
    bool sticky = false;
    for (uint32_t i = keep >= 0 ? (uint32_t)keep + 1 : 0; i < n; i++) {
        if (d[i] != '0') {
            sticky = true;
            break;
        }
    }
    char last = keep > 0 ? d[keep - 1] : '0';
    bool any = first != '0' || sticky;
    bool up = false;
    switch (mode) {
    case RM_CEIL:
        up = any && !x->neg;
        break;
    case RM_FLOOR:
        up = any && x->neg;
        break;
    case RM_EXPAND:
        up = any;
        break;
    case RM_TRUNC:
        up = false;
        break;
    case RM_HCEIL:
        up = x->neg ? (first > '5' || (first == '5' && sticky)) : first >= '5';
        break;
    case RM_HFLOOR:
        up = x->neg ? first >= '5' : (first > '5' || (first == '5' && sticky));
        break;
    case RM_HEXPAND:
        up = first >= '5';
        break;
    case RM_HTRUNC:
        up = first > '5' || (first == '5' && sticky);
        break;
    default:
        up = first > '5' || (first == '5' && (sticky || ((last - '0') & 1)));
        break;
    }
    if (keep <= 0) {
        x->d.len = 0;
        if (up) {
            embi_sb_append_char(&x->d, '1');
            x->p = x->p - (int32_t)keep + 1;
        }
        dec_norm(x);
        return;
    }
    x->d.len = (uint32_t)keep;
    if (up) {
        int64_t i = keep - 1;
        while (i >= 0 && d[i] == '9') {
            d[i] = '0';
            i--;
        }
        if (i >= 0) {
            d[i]++;
        } else {
            d[0] = '1';
            x->d.len = 1;
            x->p += 1;
        }
    }
    dec_norm(x);
}

/* ------------------------------------------------------------------ output parts */

enum {
    PT_LITERAL,
    PT_INTEGER,
    PT_GROUP,
    PT_DECIMAL,
    PT_FRACTION,
    PT_MINUS,
    PT_PLUS,
    PT_CURRENCY,
    PT_PERCENT,
    PT_UNIT,
    PT_NAN,
    PT_INF,
    PT_WEEKDAY,
    PT_YEAR,
    PT_MONTH,
    PT_DAY,
    PT_HOUR,
    PT_MINUTE,
    PT_SECOND,
    PT_FSECOND,
    PT_DAYPERIOD,
    PT_TZNAME
};
static const char *const PT_NAMES[] = {
    "literal", "integer", "group", "decimal", "fraction", "minusSign", "plusSign", "currency",
    "percentSign", "unit", "nan", "infinity", "weekday", "year", "month", "day", "hour", "minute",
    "second", "fractionalSecond", "dayPeriod", "timeZoneName"};

typedef struct part {
    uint8_t type;
    uint32_t start;
    uint32_t len;
} part;

/* The formatted text, as one string builder, with the parts as ranges of it. */
typedef struct fmt {
    embi_sb sb;
    EMBI_VEC(part) parts;
} fmt;

static void fmt_free(fmt *f)
{
    embi_sb_free(&f->sb);
    embi_vec_free(&f->parts);
}

static void fpart(fmt *f, uint8_t type, uint32_t start)
{
    uint32_t len = f->sb.len - start;
    if (len == 0) {
        return;
    }
    part p = {type, start, len};
    embi_vec_push(&f->parts, p);
}

static void fadd_n(fmt *f, uint8_t type, const char *s, size_t n)
{
    uint32_t start = f->sb.len;
    embi_sb_append(&f->sb, (embi_str){s, (uint32_t)n});
    fpart(f, type, start);
}

static void fmt_part(fmt *f, uint8_t type, const char *s)
{
    fadd_n(f, type, s, strlen(s));
}

/* The decimal digits of an integer, two or more when two is set. */
static void emit_num(fmt *f, uint8_t type, int32_t v, bool two)
{
    char b[16];
    int n = 0;
    bool neg = v < 0;
    uint32_t u = neg ? 0u - (uint32_t)v : (uint32_t)v;
    char tmp[16];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    while (k < (two ? 2 : 1)) {
        tmp[k++] = '0';
    }
    if (neg) {
        b[n++] = '-';
    }
    while (k > 0) {
        b[n++] = tmp[--k];
    }
    fadd_n(f, type, b, (size_t)n);
}

/* Grouping modes. */
enum { G_FALSE, G_AUTO, G_ALWAYS, G_MIN2 };

/* The integer digits of x with the minimum count and en-US grouping. */
static void emit_int_digits(fmt *f, const dec *x, int minid, int group)
{
    int32_t L = x->p > 0 ? x->p : 1;
    int32_t pad = minid > L ? minid - L : 0;
    int32_t N = L + pad;
    bool grp = (group == G_ALWAYS && N > 3) || (group == G_AUTO && N >= 4) ||
               (group == G_MIN2 && N >= 5);
    uint32_t seg = f->sb.len;
    for (int32_t i = 0; i < N; i++) {
        if (grp && i > 0 && (N - i) % 3 == 0) {
            fpart(f, PT_INTEGER, seg);
            uint32_t g = f->sb.len;
            embi_sb_append_char(&f->sb, ',');
            fpart(f, PT_GROUP, g);
            seg = f->sb.len;
        }
        char c = '0';
        if (i >= pad && x->p > 0) {
            c = dig(x, i - pad);
        }
        embi_sb_append_char(&f->sb, c);
    }
    fpart(f, PT_INTEGER, seg);
}

/* ------------------------------------------------------------------ number format */

/* The bound getters cache their function in the first word of the state. */
typedef struct bound_hdr {
    embi_js_value bound;
} bound_hdr;

/* Digit options (SetNumberFormatDigitOptions). sd: significant digits are used. strip: the
 * trailingZeroDisplay stripIfInteger. rmode: an RM_* rounding mode. */
typedef struct digits {
    uint8_t minid, minfd, maxfd, minsd, maxsd, rmode;
    bool sd, strip;
} digits;

enum { S_DECIMAL, S_PERCENT, S_CURRENCY, S_UNIT };
static const char *const STYLE_NAMES[4] = {"decimal", "percent", "currency", "unit"};
enum { CD_SYMBOL, CD_NARROW, CD_CODE, CD_NAME };
static const char *const CD_NAMES[4] = {"symbol", "narrowSymbol", "code", "name"};
enum { CS_STANDARD, CS_ACCOUNTING };
static const char *const CS_NAMES[2] = {"standard", "accounting"};
enum { UD_SHORT, UD_NARROW, UD_LONG };
static const char *const UD_NAMES[3] = {"short", "narrow", "long"};
enum { SD_AUTO, SD_ALWAYS, SD_EXCEPT, SD_NEG, SD_NEVER };
static const char *const SD_NAMES[5] = {"auto", "always", "exceptZero", "negative", "never"};
static const char *const GROUP_NAMES[4] = {"false", "auto", "always", "min2"};

typedef struct nf_state {
    bound_hdr hdr;
    digits dg;
    char currency[4];
    uint8_t style, cdisp, csign, udisp, unit, sign, group;
} nf_state;

/* Built-in currency data: symbol, narrow symbol, singular and plural names, fraction digits. */
typedef struct curr_info {
    const char *code, *sym, *narrow, *n1, *nn;
    uint8_t digits;
} curr_info;
static const curr_info CURRENCIES[] = {
    {"USD", "$", "$", "US dollar", "US dollars", 2},
    {"EUR", "\xE2\x82\xAC", "\xE2\x82\xAC", "euro", "euros", 2},
    {"GBP", "\xC2\xA3", "\xC2\xA3", "British pound", "British pounds", 2},
    {"JPY", "\xC2\xA5", "\xC2\xA5", "Japanese yen", "Japanese yen", 0},
    {"INR", "\xE2\x82\xB9", "\xE2\x82\xB9", "Indian rupee", "Indian rupees", 2},
    {"CNY", "CN\xC2\xA5", "\xC2\xA5", "Chinese yuan", "Chinese yuan", 2},
    {"KRW", "\xE2\x82\xA9", "\xE2\x82\xA9", "South Korean won", "South Korean won", 0},
    {"CAD", "CA$", "$", "Canadian dollar", "Canadian dollars", 2},
    {"AUD", "A$", "$", "Australian dollar", "Australian dollars", 2},
};

static const curr_info *curr_find(const char *code)
{
    for (size_t i = 0; i < sizeof CURRENCIES / sizeof CURRENCIES[0]; i++) {
        if (strcmp(CURRENCIES[i].code, code) == 0) {
            return &CURRENCIES[i];
        }
    }
    return nullptr;
}

/* Sanctioned units: short singular and plural, narrow, long singular and plural, and whether the
 * short and narrow forms take no space before the unit. */
typedef struct unit_info {
    const char *id, *s1, *sn, *nar, *l1, *ln;
    bool nospace;
} unit_info;
static const unit_info UNITS[] = {
    {"acre", "ac", "ac", "ac", "acre", "acres", false},
    {"bit", "bit", "bit", "bit", "bit", "bits", false},
    {"byte", "byte", "byte", "B", "byte", "bytes", false},
    {"celsius", "\xC2\xB0" "C", "\xC2\xB0" "C", "\xC2\xB0" "C", "degree Celsius", "degrees Celsius", true},
    {"centimeter", "cm", "cm", "cm", "centimeter", "centimeters", false},
    {"day", "day", "days", "d", "day", "days", false},
    {"degree", "deg", "deg", "\xC2\xB0", "degree", "degrees", false},
    {"fahrenheit", "\xC2\xB0" "F", "\xC2\xB0" "F", "\xC2\xB0", "degree Fahrenheit", "degrees Fahrenheit", true},
    {"foot", "ft", "ft", "\xE2\x80\xB2", "foot", "feet", false},
    {"gigabyte", "GB", "GB", "G", "gigabyte", "gigabytes", false},
    {"gram", "g", "g", "g", "gram", "grams", false},
    {"hour", "hr", "hr", "h", "hour", "hours", false},
    {"inch", "in", "in", "\xE2\x80\xB3", "inch", "inches", false},
    {"kilobyte", "kB", "kB", "kB", "kilobyte", "kilobytes", false},
    {"kilogram", "kg", "kg", "kg", "kilogram", "kilograms", false},
    {"kilometer", "km", "km", "km", "kilometer", "kilometers", false},
    {"kilometer-per-hour", "km/h", "km/h", "km/h", "kilometer per hour", "kilometers per hour", false},
    {"liter", "L", "L", "L", "liter", "liters", false},
    {"megabyte", "MB", "MB", "M", "megabyte", "megabytes", false},
    {"meter", "m", "m", "m", "meter", "meters", false},
    {"mile", "mi", "mi", "mi", "mile", "miles", false},
    {"mile-per-hour", "mph", "mph", "mph", "mile per hour", "miles per hour", false},
    {"millisecond", "ms", "ms", "ms", "millisecond", "milliseconds", false},
    {"minute", "min", "min", "m", "minute", "minutes", false},
    {"percent", "%", "%", "%", "percent", "percent", true},
    {"pound", "lb", "lb", "lb", "pound", "pounds", false},
    {"second", "sec", "sec", "s", "second", "seconds", false},
    {"week", "wk", "wks", "w", "week", "weeks", false},
    {"year", "yr", "yrs", "y", "year", "years", false},
};

static int unit_find(const embi_js_string *s)
{
    for (size_t i = 0; i < sizeof UNITS / sizeof UNITS[0]; i++) {
        if (str_is(s, UNITS[i].id)) {
            return (int)i;
        }
    }
    return -1;
}

/* ISO 4217 code: three ASCII letters, uppercased into out. */
static bool currency_code(const embi_js_string *s, char *out)
{
    if (embi_js_str_len(s) != 3) {
        return false;
    }
    for (uint32_t i = 0; i < 3; i++) {
        uint16_t c = embi_js_str_at(s, i);
        if (c >= 0x80 || !is_alpha((char)c)) {
            return false;
        }
        out[i] = upper((char)c);
    }
    out[3] = '\0';
    return true;
}

static int currency_digits(const char *code)
{
    const curr_info *ci = curr_find(code);
    return ci ? ci->digits : 2;
}

/* SetNumberFormatDigitOptions with the style defaults mnfd_def and mxfd_def, then the rounding
 * options. roundingIncrement other than 1 and roundingPriority other than auto are a RangeError, as
 * they are not implemented. */
static bool read_digits(embi_js_runtime *rt, embi_js_object *o, int mnfd_def, int mxfd_def,
                        digits *g)
{
    static const char *const PRIORITY[3] = {"auto", "morePrecision", "lessPrecision"};
    static const char *const TZD[2] = {"auto", "stripIfInteger"};
    int mnid = 1, mnfd = 0, mxfd = 0, mnsd = 0, mxsd = 0, inc = 1;
    bool pid = false, pmnfd = false, pmxfd = false, pmnsd = false, pmxsd = false, pinc = false;
    if (!opt_int(rt, o, "minimumIntegerDigits", 1, 21, &mnid, &pid) ||
        !opt_int(rt, o, "minimumFractionDigits", 0, 100, &mnfd, &pmnfd) ||
        !opt_int(rt, o, "maximumFractionDigits", 0, 100, &mxfd, &pmxfd) ||
        !opt_int(rt, o, "minimumSignificantDigits", 1, 21, &mnsd, &pmnsd) ||
        !opt_int(rt, o, "maximumSignificantDigits", 1, 21, &mxsd, &pmxsd) ||
        !opt_int(rt, o, "roundingIncrement", 1, 5000, &inc, &pinc)) {
        return false;
    }
    if (pinc && inc != 1) {
        (void)embi_js_throw_range_error(rt, "roundingIncrement is not supported.");
        return false;
    }
    int rm = opt_choice(rt, o, "roundingMode", RM_NAMES, 9);
    if (rm == -2) {
        return false;
    }
    int pr = opt_choice(rt, o, "roundingPriority", PRIORITY, 3);
    if (pr == -2) {
        return false;
    }
    if (pr > 0) {
        (void)embi_js_throw_range_error(rt, "roundingPriority is not supported.");
        return false;
    }
    int tz = opt_choice(rt, o, "trailingZeroDisplay", TZD, 2);
    if (tz == -2) {
        return false;
    }
    g->rmode = rm < 0 ? RM_HEXPAND : (uint8_t)rm;
    g->strip = tz == 1;
    g->minid = (uint8_t)mnid;
    /* roundingPriority "auto": significant digits win when either of their options is set. */
    g->sd = pmnsd || pmxsd;
    if (g->sd) {
        int a = pmnsd ? mnsd : 1;
        int b = pmxsd ? mxsd : 21;
        if (a > b) {
            (void)embi_js_throw_range_error(rt, "maximumSignificantDigits value is out of range.");
            return false;
        }
        g->minsd = (uint8_t)a;
        g->maxsd = (uint8_t)b;
        return true;
    }
    if (!pmnfd && !pmxfd) {
        mnfd = mnfd_def;
        mxfd = mxfd_def;
    } else if (pmnfd && !pmxfd) {
        mxfd = mxfd_def > mnfd ? mxfd_def : mnfd;
    } else if (!pmnfd && pmxfd) {
        mnfd = mnfd_def < mxfd ? mnfd_def : mxfd;
    } else if (mnfd > mxfd) {
        (void)embi_js_throw_range_error(rt, "maximumFractionDigits value is out of range.");
        return false;
    }
    g->minfd = (uint8_t)mnfd;
    g->maxfd = (uint8_t)mxfd;
    return true;
}

/* Fraction digits shown for x under g (after rounding). With stripIfInteger an integer shows none. */
static int32_t frac_count(const digits *g, const dec *x)
{
    int32_t len = (int32_t)x->d.len;
    if (g->sd) {
        if (g->strip && len <= x->p) {
            return 0;
        }
        int32_t sig = len > g->minsd ? len : g->minsd;
        int32_t f = sig - x->p;
        return f > 0 ? f : 0;
    }
    int32_t f = len - x->p;
    if (g->strip && f <= 0) {
        return 0;
    }
    return f < g->minfd ? g->minfd : f;
}

static void emit_number(fmt *f, const digits *g, const dec *x, int group)
{
    emit_int_digits(f, x, g->minid, group);
    int32_t frac = frac_count(g, x);
    if (frac <= 0) {
        return;
    }
    uint32_t st = f->sb.len;
    embi_sb_append_char(&f->sb, '.');
    fpart(f, PT_DECIMAL, st);
    st = f->sb.len;
    for (int32_t j = 0; j < frac; j++) {
        embi_sb_append_char(&f->sb, dig(x, x->p + j));
    }
    fpart(f, PT_FRACTION, st);
}

/* Formats a value under the number format state. Returns false when a conversion throws. */
static bool nf_run(embi_js_runtime *rt, const nf_state *s, embi_js_value v, fmt *f)
{
    embi_js_value n = embi_js_to_numeric(rt, v);
    if (embi_js_is_exception(n)) {
        return false;
    }
    dec x;
    memset(&x, 0, sizeof x);
    int kind = 0; /* 0 finite, 1 NaN, 2 infinity */
    if (embi_js_is_bigint(n)) {
        if (!dec_from_bigint(rt, embi_js_as_bigint(n), &x)) {
            return false;
        }
    } else {
        double d = embi_js_as_double(n);
        if (d != d) {
            kind = 1;
        } else if (isinf(d)) {
            kind = 2;
            x.neg = d < 0;
        } else {
            dec_from_double(d, &x);
        }
    }
    if (kind == 0) {
        if (s->style == S_PERCENT) {
            x.p += 2;
        }
        int64_t keep = s->dg.sd ? (int64_t)s->dg.maxsd : (int64_t)x.p + s->dg.maxfd;
        dec_round(&x, keep, s->dg.rmode);
    }
    bool zero = kind == 0 && dec_is_zero(&x);
    bool neg = x.neg;
    int show = 0; /* 0 none, 1 minus, 2 plus */
    if (kind == 1) {
        /* NaN has no sign except under signDisplay always. */
        show = s->sign == SD_ALWAYS ? 2 : 0;
    } else {
        switch (s->sign) {
        case SD_AUTO:
            show = neg ? 1 : 0;
            break;
        case SD_ALWAYS:
            show = neg ? 1 : 2;
            break;
        case SD_EXCEPT:
            show = zero ? 0 : (neg ? 1 : 2);
            break;
        case SD_NEG:
            show = (neg && !zero) ? 1 : 0;
            break;
        default:
            show = 0;
            break;
        }
    }
    bool currency = s->style == S_CURRENCY;
    bool paren = show == 1 && currency && s->csign == CS_ACCOUNTING;
    if (paren) {
        show = 0;
    }
    const curr_info *ci = currency ? curr_find(s->currency) : nullptr;
    /* The singular form needs the value 1 with no fraction digits shown ("1.00" is plural). */
    bool one = kind == 0 && x.p == 1 && x.d.len == 1 && x.d.data[0] == '1' &&
               frac_count(&s->dg, &x) == 0;
    if (paren) {
        fmt_part(f, PT_LITERAL, "(");
    }
    if (show == 1) {
        fmt_part(f, PT_MINUS, "-");
    } else if (show == 2) {
        fmt_part(f, PT_PLUS, "+");
    }
    if (currency && s->cdisp != CD_NAME) {
        if (ci && s->cdisp != CD_CODE) {
            fmt_part(f, PT_CURRENCY, s->cdisp == CD_NARROW ? ci->narrow : ci->sym);
        } else {
            fmt_part(f, PT_CURRENCY, s->currency);
            fmt_part(f, PT_LITERAL, NBSP);
        }
    }
    if (kind == 1) {
        fmt_part(f, PT_NAN, "NaN");
    } else if (kind == 2) {
        fmt_part(f, PT_INF, INFINITY_UTF8);
    } else {
        emit_number(f, &s->dg, &x, s->group);
    }
    if (currency && s->cdisp == CD_NAME) {
        fmt_part(f, PT_LITERAL, " ");
        fmt_part(f, PT_CURRENCY, ci ? (one ? ci->n1 : ci->nn) : s->currency);
    }
    if (s->style == S_PERCENT) {
        fmt_part(f, PT_PERCENT, "%");
    }
    if (s->style == S_UNIT) {
        const unit_info *u = &UNITS[s->unit];
        if (s->udisp != UD_NARROW && !u->nospace) {
            fmt_part(f, PT_LITERAL, " ");
        }
        const char *name = s->udisp == UD_LONG    ? (one ? u->l1 : u->ln)
                           : s->udisp == UD_NARROW ? u->nar
                                                   : (one ? u->s1 : u->sn);
        fmt_part(f, PT_UNIT, name);
    }
    if (paren) {
        fmt_part(f, PT_LITERAL, ")");
    }
    dec_free(&x);
    return true;
}

/* ------------------------------------------------------------------ date time format */

enum { TZ_UTC, TZ_LOCAL, TZ_FIXED };
enum { HC_H11, HC_H12, HC_H23, HC_H24, HC_NONE };
static const char *const HC_NAMES[4] = {"h11", "h12", "h23", "h24"};

/* Component values: 0 is absent, and the others name the entry of each option's list (see the
 * option tables in dtf_init). */
typedef struct dtf_state {
    bound_hdr hdr;
    uint8_t wd;         /* 1 narrow, 2 short, 3 long */
    uint8_t yr;         /* 1 numeric, 2 2-digit */
    uint8_t mo;         /* 1 numeric, 2 2-digit, 3 narrow, 4 short, 5 long */
    uint8_t dy;         /* 1 numeric, 2 2-digit */
    uint8_t hr, mi, se; /* 1 numeric, 2 2-digit */
    uint8_t fsd;        /* fractional second digits, 0 absent */
    uint8_t tzn;        /* timeZoneName index + 1 (see TZN_LIST) */
    uint8_t hc;         /* HC_* */
    uint8_t tz;         /* TZ_* */
    int16_t off;        /* TZ_FIXED: minutes east of UTC */
    uint8_t dstyle;     /* 1 full, 2 long, 3 medium, 4 short; 0 absent */
    uint8_t tstyle;     /* the same, for timeStyle */
} dtf_state;

static const char *const WEEKDAY_LONG[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                             "Thursday", "Friday", "Saturday"};
static const char *const MONTH_LONG[12] = {"January", "February", "March",     "April",
                                            "May",     "June",     "July",      "August",
                                            "September", "October", "November", "December"};

/* The fields of a time value in a zone: off is the offset in minutes east of UTC. */
typedef struct tf {
    int32_t y, mo, d, wd, h, mi, s, ms, off;
} tf;

/* Splits ms (a time value shifted to the zone, in ms since the epoch) into the fields of the
 * proleptic Gregorian calendar (the civil-from-days algorithm, H. Hinnant). No property lookups. */
static void tf_from_ms(int64_t ms, int32_t off, tf *t)
{
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    t->y = (int32_t)(yoe + era * 400 + (m <= 2 ? 1 : 0));
    t->mo = (int32_t)(m - 1);
    t->d = (int32_t)d;
    t->wd = (int32_t)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday (4) */
    t->h = (int32_t)(rem / 3600000);
    t->mi = (int32_t)(rem / 60000 % 60);
    t->s = (int32_t)(rem / 1000 % 60);
    t->ms = (int32_t)(rem % 1000);
    t->off = off;
}

/* A name in long form, or its first width characters (3 short, 1 narrow). */
static void name_part(fmt *f, uint8_t type, const char *full, int style)
{
    size_t n = strlen(full);
    if (style == 2) {
        n = 3;
    } else if (style == 1) {
        n = 1;
    }
    fadd_n(f, type, full, n);
}

static void dtf_date(fmt *f, const dtf_state *s, const tf *t)
{
    bool textual = s->mo >= 3;
    bool any = s->mo || s->dy || s->yr;
    if (s->wd) {
        name_part(f, PT_WEEKDAY, WEEKDAY_LONG[t->wd], s->wd == 3 ? 0 : s->wd == 2 ? 2 : 1);
        if (any) {
            fmt_part(f, PT_LITERAL, ", ");
        }
    }
    if (textual) {
        name_part(f, PT_MONTH, MONTH_LONG[t->mo], s->mo == 5 ? 0 : s->mo == 4 ? 2 : 1);
        if (s->dy) {
            fmt_part(f, PT_LITERAL, " ");
            emit_num(f, PT_DAY, t->d, s->dy == 2);
        }
        if (s->yr) {
            fmt_part(f, PT_LITERAL, s->dy ? ", " : " ");
            emit_num(f, PT_YEAR, s->yr == 2 ? (t->y < 0 ? -t->y : t->y) % 100 : t->y, s->yr == 2);
        }
        return;
    }
    bool sep = false;
    if (s->mo) {
        emit_num(f, PT_MONTH, t->mo + 1, s->mo == 2);
        sep = true;
    }
    if (s->dy) {
        if (sep) {
            fmt_part(f, PT_LITERAL, "/");
        }
        emit_num(f, PT_DAY, t->d, s->dy == 2);
        sep = true;
    }
    if (s->yr) {
        if (sep) {
            fmt_part(f, PT_LITERAL, "/");
        }
        emit_num(f, PT_YEAR, s->yr == 2 ? (t->y < 0 ? -t->y : t->y) % 100 : t->y, s->yr == 2);
    }
}

static void dtf_time(fmt *f, const dtf_state *s, const tf *t)
{
    bool h12 = s->hc == HC_H11 || s->hc == HC_H12;
    if (s->hr) {
        int32_t h = t->h;
        if (h12) {
            h %= 12;
            if (h == 0 && s->hc == HC_H12) {
                h = 12;
            }
        } else if (s->hc == HC_H24 && h == 0) {
            h = 24;
        }
        emit_num(f, PT_HOUR, h, s->hr == 2 || !h12);
    }
    if (s->mi) {
        if (s->hr) {
            fmt_part(f, PT_LITERAL, ":");
        }
        emit_num(f, PT_MINUTE, t->mi, s->hr != 0 || s->se != 0 || s->mi == 2);
    }
    if (s->se) {
        if (s->hr || s->mi) {
            fmt_part(f, PT_LITERAL, ":");
        }
        emit_num(f, PT_SECOND, t->s, s->hr != 0 || s->mi != 0 || s->se == 2);
    }
    if (s->fsd) {
        char b[3] = {(char)('0' + t->ms / 100), (char)('0' + (t->ms / 10) % 10),
                     (char)('0' + t->ms % 10)};
        if (s->se || s->mi) {
            fmt_part(f, PT_LITERAL, ".");
        }
        fadd_n(f, PT_FSECOND, b, s->fsd);
    }
    if (s->hr && h12) {
        fmt_part(f, PT_LITERAL, " ");
        fmt_part(f, PT_DAYPERIOD, t->h < 12 ? "AM" : "PM");
    }
}

/* The time zone name: UTC for the zero offset (GMT for the offset forms), else GMT with the offset. */
static void dtf_tzname(fmt *f, const dtf_state *s, const tf *t)
{
    /* timeZoneName index + 1: short 1, long 2, shortOffset 3, longOffset 4, shortGeneric 5,
     * longGeneric 6. */
    bool lng = s->tzn == 2 || s->tzn == 4 || s->tzn == 6;
    bool offset_form = s->tzn >= 3; /* the generic forms print GMT for zero too, as ICU does */
    if (t->off == 0) {
        if (offset_form) {
            fmt_part(f, PT_TZNAME, "GMT");
        } else {
            fmt_part(f, PT_TZNAME, lng ? "Coordinated Universal Time" : "UTC");
        }
        return;
    }
    char b[24];
    int n = 0;
    b[n++] = 'G';
    b[n++] = 'M';
    b[n++] = 'T';
    b[n++] = t->off < 0 ? '-' : '+';
    int32_t a = t->off < 0 ? -t->off : t->off;
    int32_t hours = a / 60;
    int32_t mins = a % 60;
    if (hours >= 10) {
        b[n++] = (char)('0' + hours / 10);
    }
    b[n++] = (char)('0' + hours % 10);
    if (mins) {
        b[n++] = ':';
        b[n++] = (char)('0' + mins / 10);
        b[n++] = (char)('0' + mins % 10);
    }
    fadd_n(f, PT_TZNAME, b, (size_t)n);
}

/* The parts of a formatted time value. */
static void dtf_emit(fmt *f, const dtf_state *s, const tf *t)
{
    bool has_date = s->wd || s->yr || s->mo || s->dy;
    bool has_time = s->hr || s->mi || s->se || s->fsd;
    if (has_date) {
        dtf_date(f, s, t);
    }
    if (has_time) {
        if (has_date) {
            fmt_part(f, PT_LITERAL, (s->dstyle == 1 || s->dstyle == 2) ? " at " : ", ");
        }
        dtf_time(f, s, t);
    }
    if (s->tzn) {
        if (has_time) {
            fmt_part(f, PT_LITERAL, " ");
        } else if (has_date) {
            fmt_part(f, PT_LITERAL, ", ");
        }
        dtf_tzname(f, s, t);
    }
}

/* Formats the Date dv in the zone of the format. The time value comes from the captured getTime and
 * the local offset from the captured getTimezoneOffset, so patches of Date.prototype do not change
 * the output, and the test pin hook still applies. */
static bool dtf_run(embi_js_runtime *rt, const dtf_state *s, embi_js_value dv, fmt *f)
{
    embi_js_value tv = embi_js_call(rt, slot(rt, R_GETTIME), dv, 0, nullptr);
    if (embi_js_is_exception(tv)) {
        return false;
    }
    int32_t off = s->tz == TZ_FIXED ? s->off : 0;
    if (s->tz == TZ_LOCAL) {
        embi_js_value zv = embi_js_call(rt, slot(rt, R_GETTZOFF), dv, 0, nullptr);
        if (embi_js_is_exception(zv)) {
            return false;
        }
        off = -(int32_t)embi_js_as_double(zv);
    }
    tf t;
    tf_from_ms((int64_t)embi_js_as_double(tv) + (int64_t)off * 60000, off, &t);
    dtf_emit(f, s, &t);
    return true;
}

/* Formats a value given to format(): undefined is now, otherwise a time value. */
static bool dtf_format_value(embi_js_runtime *rt, const dtf_state *s, embi_js_value x, fmt *f)
{
    embi_js_value ctor = slot(rt, R_DATE);
    embi_js_value dv;
    if (embi_js_is_undefined(x)) {
        dv = embi_js_construct(rt, ctor, 0, nullptr, ctor);
    } else {
        double d = 0.0;
        if (!embi_js_to_number(rt, x, &d)) {
            return false;
        }
        if (d != d || fabs(d) > 8.64e15) {
            (void)embi_js_throw_range_error(rt, "Invalid time value");
            return false;
        }
        embi_js_value a = embi_js_number(d);
        dv = embi_js_construct(rt, ctor, 1, &a, ctor);
    }
    if (embi_js_is_exception(dv)) {
        return false;
    }
    return dtf_run(rt, s, dv, f);
}

enum { REQ_ANY, REQ_DATE, REQ_TIME };
enum { DEF_DATE, DEF_TIME, DEF_ALL };

static const char *const WD_LIST[3] = {"narrow", "short", "long"};
static const char *const YR_LIST[2] = {"numeric", "2-digit"};
static const char *const MO_LIST[5] = {"numeric", "2-digit", "narrow", "short", "long"};
static const char *const TZN_LIST[6] = {"short", "long", "shortOffset", "longOffset",
                                        "shortGeneric", "longGeneric"};
static const char *const HC_LIST[4] = {"h11", "h12", "h23", "h24"};
static const char *const STYLE_LIST[4] = {"full", "long", "medium", "short"};

/* ASCII case-insensitive test of one code unit against a character. */
static bool ch_is(const embi_js_string *s, uint32_t i, char c)
{
    uint16_t u = embi_js_str_at(s, i);
    return u < 0x80 && lower((char)u) == lower(c);
}

/* ASCII case-insensitive equality with a literal, and with its prefix. */
static bool ieq_lit(const embi_js_string *s, const char *lit, bool prefix)
{
    size_t n = strlen(lit);
    if (embi_js_str_len(s) < n || (!prefix && embi_js_str_len(s) != n)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!ch_is(s, (uint32_t)i, lit[i])) {
            return false;
        }
    }
    return true;
}

/* The value of a decimal digit at index i, or -1. */
static int dig_val(const embi_js_string *s, uint32_t i)
{
    uint16_t u = embi_js_str_at(s, i);
    return (u >= '0' && u <= '9') ? u - '0' : -1;
}

/* A time zone name: UTC, GMT, Etc/UTC or Etc/GMT (any case), Etc/GMT+N or Etc/GMT-N with N in 0..14
 * (IANA sign: Etc/GMT-1 is UTC+1), or an offset +HH:MM or -HH:MM. Writes the offset in minutes east
 * (0 for the UTC names). False for any other name. */
static bool tz_parse(const embi_js_string *z, int32_t *off)
{
    static const char *const NAMES[4] = {"UTC", "GMT", "Etc/UTC", "Etc/GMT"};
    *off = 0;
    for (int i = 0; i < 4; i++) {
        if (ieq_lit(z, NAMES[i], false)) {
            return true;
        }
    }
    uint32_t n = embi_js_str_len(z);
    if (n >= 9 && n <= 10 && ieq_lit(z, "Etc/GMT", true) && (ch_is(z, 7, '+') || ch_is(z, 7, '-'))) {
        int32_t v = 0;
        for (uint32_t i = 8; i < n; i++) {
            int d = dig_val(z, i);
            if (d < 0) {
                return false;
            }
            v = v * 10 + d;
        }
        if ((n == 10 && dig_val(z, 8) == 0) || v > 14) { /* no zero padding */
            return false;
        }
        /* Etc/GMT-0 and Etc/GMT+0 are UTC: v is 0 and the offset is 0 for them */
        *off = (ch_is(z, 7, '-') ? 60 : -60) * v;
        return true;
    }
    if (n == 6 && (ch_is(z, 0, '+') || ch_is(z, 0, '-')) && ch_is(z, 3, ':')) {
        int h1 = dig_val(z, 1), h2 = dig_val(z, 2), m1 = dig_val(z, 4), m2 = dig_val(z, 5);
        if (h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0) {
            return false;
        }
        int32_t hh = h1 * 10 + h2, mm = m1 * 10 + m2;
        if (hh > 23 || mm > 59) {
            return false;
        }
        int32_t v = hh * 60 + mm;
        *off = ch_is(z, 0, '-') ? -v : v;
        return true;
    }
    return false;
}

/* A Unicode type sequence (a calendar or numberingSystem value): subtags of 3 to 8 ASCII
 * alphanumerics joined by '-'. */
static bool type_ok(const embi_js_string *s)
{
    uint32_t n = embi_js_str_len(s);
    uint32_t run = 0;
    if (n == 0) {
        return false;
    }
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || ch_is(s, i, '-')) {
            if (run < 3 || run > 8) {
                return false;
            }
            run = 0;
        } else {
            uint16_t u = embi_js_str_at(s, i);
            if (u >= 0x80 || !(is_alpha((char)u) || is_digit((char)u))) {
                return false;
            }
            run++;
        }
    }
    return true;
}

/* A calendar or numberingSystem option: must be a well-formed type. The value is not used, since
 * the only calendar and numbering system are gregory and latn. */
static bool opt_type(embi_js_runtime *rt, embi_js_object *o, const char *name)
{
    embi_js_string *s = nullptr;
    int r = opt_get_str(rt, o, name, &s);
    if (r < 0) {
        return false;
    }
    if (r == 1 && !type_ok(s)) {
        (void)embi_js_throw_range_error(rt, "Invalid %s.", name);
        return false;
    }
    return true;
}

/* An option whose valid values this build does not implement: false when the read throws or the
 * option is set to any of its values (a RangeError). */
static bool no_opt(embi_js_runtime *rt, embi_js_object *o, const char *name,
                   const char *const *list, int n)
{
    int c = opt_choice(rt, o, name, list, n);
    if (c == -2) {
        return false;
    }
    if (c >= 0) {
        (void)embi_js_throw_range_error(rt, "%s is not supported.", name);
        return false;
    }
    return true;
}

/* A component option: sets *out to the list index + 1, or 0 when absent. */
static bool comp_opt(embi_js_runtime *rt, embi_js_object *o, const char *name,
                     const char *const *list, int n, uint8_t *out)
{
    int c = opt_choice(rt, o, name, list, n);
    if (c == -2) {
        return false;
    }
    *out = c < 0 ? 0 : (uint8_t)(c + 1);
    return true;
}

/* ToDateTimeOptions and CreateDateTimeFormat for the constructor and the toLocale* methods. The
 * options are read in the ECMA-402 order, and each enumerated value is validated even when this
 * build then ignores it. */
static bool dtf_init(embi_js_runtime *rt, dtf_state *s, embi_js_value locales,
                     embi_js_value options, int req, int def)
{
    static const char *const MATCHER[2] = {"lookup", "best fit"};
    static const char *const FORMAT_MATCHER[2] = {"basic", "best fit"};
    static const char *const ERA_LIST[3] = {"narrow", "short", "long"};
    static const char *const DAYPERIOD_LIST[3] = {"narrow", "short", "long"};
    s->hdr.bound = UNDEF;
    if (!canon_locales(rt, locales)) {
        return false;
    }
    embi_js_object *o = nullptr;
    if (!options_obj(rt, options, &o)) {
        return false;
    }
    if (opt_choice(rt, o, "localeMatcher", MATCHER, 2) == -2 || !opt_type(rt, o, "calendar") ||
        !opt_type(rt, o, "numberingSystem")) {
        return false;
    }
    int h12 = opt_bool(rt, o, "hour12");
    if (h12 == -2) {
        return false;
    }
    int c = opt_choice(rt, o, "hourCycle", HC_LIST, 4);
    if (c == -2) {
        return false;
    }
    s->hc = c < 0 ? HC_NONE : (uint8_t)c;
    embi_js_string *tzs = nullptr;
    int tzr = opt_get_str(rt, o, "timeZone", &tzs);
    if (tzr < 0) {
        return false;
    }
    s->tz = TZ_LOCAL;
    s->off = 0;
    if (tzr == 1) {
        int32_t off = 0;
        if (!tz_parse(tzs, &off)) {
            (void)embi_js_throw_range_error(rt, "Invalid time zone specified");
            return false;
        }
        s->tz = off == 0 ? TZ_UTC : TZ_FIXED;
        s->off = (int16_t)off;
    }
    if (!comp_opt(rt, o, "weekday", WD_LIST, 3, &s->wd) ||
        !no_opt(rt, o, "era", ERA_LIST, 3) ||
        !comp_opt(rt, o, "year", YR_LIST, 2, &s->yr) ||
        !comp_opt(rt, o, "month", MO_LIST, 5, &s->mo) ||
        !comp_opt(rt, o, "day", YR_LIST, 2, &s->dy) ||
        !no_opt(rt, o, "dayPeriod", DAYPERIOD_LIST, 3) ||
        !comp_opt(rt, o, "hour", YR_LIST, 2, &s->hr) ||
        !comp_opt(rt, o, "minute", YR_LIST, 2, &s->mi) ||
        !comp_opt(rt, o, "second", YR_LIST, 2, &s->se)) {
        return false;
    }
    int fsd = 0;
    bool pfsd = false;
    if (!opt_int(rt, o, "fractionalSecondDigits", 1, 3, &fsd, &pfsd)) {
        return false;
    }
    s->fsd = pfsd ? (uint8_t)fsd : 0;
    if (!comp_opt(rt, o, "timeZoneName", TZN_LIST, 6, &s->tzn) ||
        opt_choice(rt, o, "formatMatcher", FORMAT_MATCHER, 2) == -2 ||
        !comp_opt(rt, o, "dateStyle", STYLE_LIST, 4, &s->dstyle) ||
        !comp_opt(rt, o, "timeStyle", STYLE_LIST, 4, &s->tstyle)) {
        return false;
    }

    bool comps = s->wd || s->yr || s->mo || s->dy || s->hr || s->mi || s->se || s->fsd || s->tzn;
    if ((s->dstyle || s->tstyle) && comps) {
        (void)embi_js_throw_type_error(rt, "dateStyle and timeStyle cannot be combined with date-time components.");
        return false;
    }
    if ((req == REQ_DATE && s->tstyle) || (req == REQ_TIME && s->dstyle)) {
        (void)embi_js_throw_type_error(rt, "timeStyle is not valid for a date format, and dateStyle is not valid for a time format.");
        return false;
    }
    if (s->dstyle) {
        /* full: weekday long, long: month long, medium: month short, short: 2-digit year */
        s->wd = s->dstyle == 1 ? 3 : 0;
        s->mo = s->dstyle <= 2 ? 5 : s->dstyle == 3 ? 4 : 1;
        s->dy = 1;
        s->yr = s->dstyle == 4 ? 2 : 1;
    }
    if (s->tstyle) {
        s->hr = 1;
        s->mi = 2;
        s->se = s->tstyle == 4 ? 0 : 2;
        s->tzn = s->tstyle == 1 ? 2 : s->tstyle == 2 ? 1 : 0;
    }
    if (!s->dstyle && !s->tstyle) {
        bool need = true;
        if ((req == REQ_DATE || req == REQ_ANY) && (s->wd || s->yr || s->mo || s->dy)) {
            need = false;
        }
        if ((req == REQ_TIME || req == REQ_ANY) && (s->hr || s->mi || s->se || s->fsd)) {
            need = false;
        }
        if (need && (def == DEF_DATE || def == DEF_ALL)) {
            s->yr = s->mo = s->dy = 1;
        }
        if (need && (def == DEF_TIME || def == DEF_ALL)) {
            s->hr = s->mi = s->se = 1;
        }
    }
    if (s->hr) {
        if (h12 >= 0) {
            s->hc = h12 ? HC_H12 : HC_H23;
        } else if (s->hc == HC_NONE) {
            s->hc = HC_H12;
        }
    } else {
        s->hc = HC_NONE;
    }
    return true;
}

/* ------------------------------------------------------------------ collator */

enum { SENS_BASE, SENS_ACCENT, SENS_CASE, SENS_VARIANT };
static const char *const SENS_LIST[4] = {"base", "accent", "case", "variant"};

static const char *const CASE_FIRST_NAMES[3] = {"upper", "lower", "false"};

typedef struct coll_state {
    bound_hdr hdr;
    uint8_t sens;
    uint8_t case_first; /* index into CASE_FIRST_NAMES */
    bool numeric;
    bool search;
    bool ignore_punct;
} coll_state;

/* CreateCollator option reads, in the ECMA-402 order. */
static bool coll_init(embi_js_runtime *rt, coll_state *s, embi_js_value locales, embi_js_value options)
{
    static const char *const USAGE[2] = {"sort", "search"};
    static const char *const MATCHER[2] = {"lookup", "best fit"};
    s->hdr.bound = UNDEF;
    if (!canon_locales(rt, locales)) {
        return false;
    }
    embi_js_object *o = nullptr;
    if (!options_obj(rt, options, &o)) {
        return false;
    }
    int c = opt_choice(rt, o, "usage", USAGE, 2);
    if (c == -2) {
        return false;
    }
    s->search = c == 1;
    if (opt_choice(rt, o, "localeMatcher", MATCHER, 2) == -2 || !opt_type(rt, o, "collation")) {
        return false;
    }
    c = opt_bool(rt, o, "numeric");
    if (c == -2) {
        return false;
    }
    s->numeric = c == 1;
    c = opt_choice(rt, o, "caseFirst", CASE_FIRST_NAMES, 3);
    if (c == -2) {
        return false;
    }
    s->case_first = c < 0 ? 2 : (uint8_t)c;
    c = opt_choice(rt, o, "sensitivity", SENS_LIST, 4);
    if (c == -2) {
        return false;
    }
    s->sens = c < 0 ? SENS_VARIANT : (uint8_t)c;
    c = opt_bool(rt, o, "ignorePunctuation");
    if (c == -2) {
        return false;
    }
    s->ignore_punct = c == 1;
    return true;
}

/* The code points of the collator, in a vector. */
typedef EMBI_VEC(uint32_t) cps;

/* Punctuation, spaces and controls: ignored at every level when ignorePunctuation is set. */
static bool is_ignorable(uint32_t c)
{
    if (c < 0x80u) {
        return c < 0x30u || (c >= 0x3Au && c <= 0x40u) || (c >= 0x5Bu && c <= 0x60u) || c >= 0x7Bu;
    }
    return c == 0xA0u || c == 0x3000u || (c >= 0x2000u && c <= 0x206Fu);
}

/* Binary search in a sorted special-case table (the full mappings with several code points). */
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

/* Looks cp up in the run table of simple mappings: writes the mapped code point and returns true. */
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
    *out = cp + (uint32_t)r->delta;
    return true;
}

/* The simple lowercase of a code point, which is the case fold of the collator. A special mapping
 * with several code points contributes its first one (U+0130 folds to i). */
static uint32_t lower_cp(uint32_t c)
{
    if (c < 0x80u) {
        return (c >= 'A' && c <= 'Z') ? c + 32u : c;
    }
    const embi_case_special *sp =
        special_find(embi_case_lower_special, ARR_LEN(embi_case_lower_special), c);
    if (sp) {
        return sp->out[0];
    }
    uint32_t mapped = c;
    if (run_lookup(embi_case_lower_runs, ARR_LEN(embi_case_lower_runs), c, &mapped)) {
        return mapped;
    }
    return c;
}

static bool is_mark(uint32_t c)
{
    return c >= 0x300u && c <= 0x36Fu;
}

static bool is_upper_cp(uint32_t c)
{
    return lower_cp(c) != c;
}

/* Appends the code points of s: its NFD form, or the code units as they are for ASCII (already NFD,
 * so the normalizer is skipped). Ignorable characters are dropped when ignore is set. */
static void coll_cps(embi_js_string *s, bool ignore, cps *out)
{
    uint32_t n = embi_js_str_len(s);
    bool ascii = true;
    for (uint32_t i = 0; i < n && ascii; i++) {
        ascii = embi_js_str_at(s, i) < 0x80u;
    }
    /* Reserves the usual size up front, so most strings take one allocation. Pushes grow the
     * vector anyway, so a string whose NFD form is longer is still correct. */
    embi_vec_reserve(out, out->len + n);
    if (ascii) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = embi_js_str_at(s, i);
            if (!ignore || !is_ignorable(c)) {
                embi_vec_push(out, c);
            }
        }
        return;
    }
    embi_norm_units src = {nullptr, 0, 0};
    embi_norm_units nfd = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&src, embi_js_str_at(s, i));
    }
    if (!embi_norm_run(src.data, n, EMBI_NORM_NFD, &nfd)) {
        /* Allocation failure: compare the code units as they are. */
        nfd.len = 0;
        for (uint32_t i = 0; i < n; i++) {
            embi_vec_push(&nfd, src.data[i]);
        }
    }
    for (uint32_t i = 0; i < nfd.len; i++) {
        uint32_t c = nfd.data[i];
        if (c >= 0xD800u && c <= 0xDBFFu && i + 1 < nfd.len && nfd.data[i + 1] >= 0xDC00u &&
            nfd.data[i + 1] <= 0xDFFFu) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (nfd.data[i + 1] - 0xDC00u);
            i++;
        }
        if (!ignore || !is_ignorable(c)) {
            embi_vec_push(out, c);
        }
    }
    embi_vec_free(&src);
    embi_vec_free(&nfd);
}

/* The base letters: marks skipped, case folded, digit runs compared by value when numeric. */
static int cmp_base(const cps *A, const cps *B, bool numeric)
{
    const uint32_t *a = A->data;
    const uint32_t *b = B->data;
    uint32_t na = A->len, nb = B->len;
    uint32_t i = 0, j = 0;
    for (;;) {
        while (i < na && is_mark(a[i])) {
            i++;
        }
        while (j < nb && is_mark(b[j])) {
            j++;
        }
        if (i >= na || j >= nb) {
            break;
        }
        if (numeric && a[i] >= '0' && a[i] <= '9' && b[j] >= '0' && b[j] <= '9') {
            uint32_t ie = i, je = j;
            while (ie < na && a[ie] >= '0' && a[ie] <= '9') {
                ie++;
            }
            while (je < nb && b[je] >= '0' && b[je] <= '9') {
                je++;
            }
            uint32_t ia = i, jb = j;
            while (ia + 1 < ie && a[ia] == '0') {
                ia++;
            }
            while (jb + 1 < je && b[jb] == '0') {
                jb++;
            }
            uint32_t la = ie - ia, lb = je - jb;
            if (la != lb) {
                return la < lb ? -1 : 1;
            }
            for (uint32_t k = 0; k < la; k++) {
                if (a[ia + k] != b[jb + k]) {
                    return a[ia + k] < b[jb + k] ? -1 : 1;
                }
            }
            i = ie;
            j = je;
            continue;
        }
        uint32_t ca = lower_cp(a[i]), cb = lower_cp(b[j]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        i++;
        j++;
    }
    while (i < na && is_mark(a[i])) {
        i++;
    }
    while (j < nb && is_mark(b[j])) {
        j++;
    }
    bool ra = i < na, rb = j < nb;
    if (ra == rb) {
        return 0;
    }
    return ra ? 1 : -1;
}

/* All code points, case folded: accents count. */
static int cmp_folded(const cps *A, const cps *B)
{
    uint32_t n = A->len < B->len ? A->len : B->len;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ca = lower_cp(A->data[k]), cb = lower_cp(B->data[k]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (A->len == B->len) {
        return 0;
    }
    return A->len < B->len ? -1 : 1;
}

/* The case of the non-mark code points. Lowercase sorts first unless upper_first. */
static int cmp_case(const cps *A, const cps *B, bool upper_first)
{
    uint32_t i = 0, j = 0;
    for (;;) {
        while (i < A->len && is_mark(A->data[i])) {
            i++;
        }
        while (j < B->len && is_mark(B->data[j])) {
            j++;
        }
        if (i >= A->len || j >= B->len) {
            break;
        }
        int ua = is_upper_cp(A->data[i]), ub = is_upper_cp(B->data[j]);
        if (ua != ub) {
            int r = ua < ub ? -1 : 1;
            return upper_first ? -r : r;
        }
        i++;
        j++;
    }
    bool ra = i < A->len, rb = j < B->len;
    if (ra == rb) {
        return 0;
    }
    return ra ? 1 : -1;
}

/* Compares two strings at the collator's sensitivity: -1, 0 or 1. */
static int coll_compare(const coll_state *s, embi_js_string *a, embi_js_string *b)
{
    cps A = {nullptr, 0, 0}, B = {nullptr, 0, 0};
    coll_cps(a, s->ignore_punct, &A);
    coll_cps(b, s->ignore_punct, &B);
    int c = cmp_base(&A, &B, s->numeric);
    if (c == 0 && (s->sens == SENS_ACCENT || s->sens == SENS_VARIANT)) {
        c = cmp_folded(&A, &B);
    }
    if (c == 0 && (s->sens == SENS_CASE || s->sens == SENS_VARIANT)) {
        c = cmp_case(&A, &B, s->case_first == 0);
    }
    embi_vec_free(&A);
    embi_vec_free(&B);
    return c;
}

/* ------------------------------------------------------------------ plural rules */

enum { PR_CARDINAL, PR_ORDINAL };
static const char *const PR_TYPE_NAMES[2] = {"cardinal", "ordinal"};

typedef struct pr_state {
    bound_hdr hdr;
    digits dg;
    uint8_t type;
} pr_state;

static bool pr_init(embi_js_runtime *rt, pr_state *s, embi_js_value locales, embi_js_value options)
{
    static const char *const MATCHER[2] = {"lookup", "best fit"};
    s->hdr.bound = UNDEF;
    if (!canon_locales(rt, locales)) {
        return false;
    }
    embi_js_object *o = nullptr;
    if (!options_obj(rt, options, &o)) {
        return false;
    }
    if (opt_choice(rt, o, "localeMatcher", MATCHER, 2) == -2) {
        return false;
    }
    int c = opt_choice(rt, o, "type", PR_TYPE_NAMES, 2);
    if (c == -2) {
        return false;
    }
    s->type = c < 0 ? PR_CARDINAL : (uint8_t)c;
    static const char *const NOTATION[4] = {"standard", "scientific", "engineering", "compact"};
    c = opt_choice(rt, o, "notation", NOTATION, 4);
    if (c == -2) {
        return false;
    }
    if (c > 0) {
        (void)embi_js_throw_range_error(rt, "notation other than standard is not supported.");
        return false;
    }
    return read_digits(rt, o, 0, 3, &s->dg);
}

/* The plural category of n: one, two, few or other (en rules). A value with visible fraction digits
 * (after rounding, "1.0" for minimumFractionDigits 1) is always other. */
static const char *pr_select(embi_js_runtime *rt, const pr_state *s, embi_js_value v)
{
    double d = 0.0;
    if (!embi_js_to_number(rt, v, &d)) {
        return nullptr;
    }
    if (d != d || isinf(d)) {
        return "other";
    }
    dec x;
    dec_from_double(d, &x);
    int64_t keep = s->dg.sd ? (int64_t)s->dg.maxsd : (int64_t)x.p + s->dg.maxfd;
    dec_round(&x, keep, s->dg.rmode);
    const char *r = "other";
    if (frac_count(&s->dg, &x) == 0) {
        if (s->type == PR_CARDINAL) {
            if (x.p == 1 && x.d.len == 1 && x.d.data[0] == '1') {
                r = "one";
            }
        } else if (x.d.len <= (uint32_t)(x.p > 0 ? x.p : 0)) {
            int m10 = x.p >= 1 ? dig(&x, x.p - 1) - '0' : 0;
            int m100 = m10 + 10 * (x.p >= 2 ? dig(&x, x.p - 2) - '0' : 0);
            if (m10 == 1 && m100 != 11) {
                r = "one";
            } else if (m10 == 2 && m100 != 12) {
                r = "two";
            } else if (m10 == 3 && m100 != 13) {
                r = "few";
            }
        }
    }
    dec_free(&x);
    return r;
}

/* ------------------------------------------------------------------ option setup */

/* NumberFormat option reads (SetNumberFormatUnitOptions, then the digit options, then grouping and
 * sign), in the ECMA-402 order. notation other than standard is a RangeError. */
static bool nf_init(embi_js_runtime *rt, nf_state *s, embi_js_value locales, embi_js_value options)
{
    static const char *const MATCHER[2] = {"lookup", "best fit"};
    static const char *const NOTATION[4] = {"standard", "scientific", "engineering", "compact"};
    s->hdr.bound = UNDEF;
    if (!canon_locales(rt, locales)) {
        return false;
    }
    embi_js_object *o = nullptr;
    if (!options_obj(rt, options, &o)) {
        return false;
    }
    if (opt_choice(rt, o, "localeMatcher", MATCHER, 2) == -2 || !opt_type(rt, o, "numberingSystem")) {
        return false;
    }
    int c = opt_choice(rt, o, "style", STYLE_NAMES, 4);
    if (c == -2) {
        return false;
    }
    s->style = c < 0 ? S_DECIMAL : (uint8_t)c;
    embi_js_string *cs = nullptr;
    int r = opt_get_str(rt, o, "currency", &cs);
    if (r < 0) {
        return false;
    }
    s->currency[0] = '\0';
    if (r == 1 && !currency_code(cs, s->currency)) {
        (void)embi_js_throw_range_error(rt, "Invalid currency code");
        return false;
    }
    if (s->style == S_CURRENCY && r != 1) {
        (void)embi_js_throw_type_error(rt, "Currency code is required with currency style.");
        return false;
    }
    c = opt_choice(rt, o, "currencyDisplay", CD_NAMES, 4);
    if (c == -2) {
        return false;
    }
    s->cdisp = c < 0 ? CD_SYMBOL : (uint8_t)c;
    c = opt_choice(rt, o, "currencySign", CS_NAMES, 2);
    if (c == -2) {
        return false;
    }
    s->csign = c < 0 ? CS_STANDARD : (uint8_t)c;
    embi_js_string *us = nullptr;
    r = opt_get_str(rt, o, "unit", &us);
    if (r < 0) {
        return false;
    }
    s->unit = 0;
    if (r == 1) {
        int u = unit_find(us);
        if (u < 0) {
            (void)embi_js_throw_range_error(rt, "Invalid unit argument");
            return false;
        }
        s->unit = (uint8_t)u;
    }
    if (s->style == S_UNIT && r != 1) {
        (void)embi_js_throw_type_error(rt, "Unit is required with unit style.");
        return false;
    }
    c = opt_choice(rt, o, "unitDisplay", UD_NAMES, 3);
    if (c == -2) {
        return false;
    }
    s->udisp = c < 0 ? UD_SHORT : (uint8_t)c;
    c = opt_choice(rt, o, "notation", NOTATION, 4);
    if (c == -2) {
        return false;
    }
    if (c > 0) {
        (void)embi_js_throw_range_error(rt, "notation other than standard is not supported.");
        return false;
    }
    int mnfd = 0, mxfd = 3;
    if (s->style == S_CURRENCY) {
        mnfd = mxfd = currency_digits(s->currency);
    } else if (s->style == S_PERCENT) {
        mxfd = 0;
    }
    if (!read_digits(rt, o, mnfd, mxfd, &s->dg)) {
        return false;
    }
    s->group = G_AUTO;
    embi_js_value gv = o ? embi_js_get(rt, o, embi_js_key_c(rt, "useGrouping")) : UNDEF;
    if (embi_js_is_exception(gv)) {
        return false;
    }
    if (embi_js_is_bool(gv) && gv == EMBI_JS_TRUE) {
        s->group = G_ALWAYS;
    } else if (!embi_js_is_undefined(gv)) {
        if (!embi_js_to_boolean(gv)) {
            s->group = G_FALSE;
        } else {
            /* The strings "true" and "false" fall back to the default, as ECMA-402 specifies. */
            embi_js_string *g = embi_js_to_string(rt, gv);
            if (!g) {
                return false;
            }
            if (str_is(g, "always")) {
                s->group = G_ALWAYS;
            } else if (str_is(g, "min2")) {
                s->group = G_MIN2;
            } else if (!str_is(g, "auto") && !str_is(g, "true") && !str_is(g, "false")) {
                (void)embi_js_throw_range_error(rt, "Value out of range for option useGrouping.");
                return false;
            }
        }
    }
    c = opt_choice(rt, o, "signDisplay", SD_NAMES, 5);
    if (c == -2) {
        return false;
    }
    s->sign = c < 0 ? SD_AUTO : (uint8_t)c;
    return true;
}

/* ------------------------------------------------------------------ results */

/* formatToParts: an array of { type, value } objects. */
static embi_js_value parts_array(embi_js_runtime *rt, const fmt *f)
{
    embi_js_object *arr = embi_js_array_create_default(rt, 0);
    if (!arr) {
        return EXC;
    }
    for (uint32_t i = 0; i < f->parts.len; i++) {
        const part *p = &f->parts.data[i];
        embi_js_object *o = embi_js_object_new_plain(rt);
        put_str(rt, o, "type", PT_NAMES[p->type]);
        put_val(rt, o, "value", str_val(rt, f->sb.data + p->start, p->len));
        if (embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(i),
                                                  embi_js_from_object(o)) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(arr);
}

static embi_js_value nf_resolved(embi_js_runtime *rt, const nf_state *s)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_str(rt, o, "locale", "en-US");
    put_str(rt, o, "numberingSystem", "latn");
    put_str(rt, o, "style", STYLE_NAMES[s->style]);
    if (s->style == S_CURRENCY) {
        put_str(rt, o, "currency", s->currency);
        put_str(rt, o, "currencyDisplay", CD_NAMES[s->cdisp]);
        put_str(rt, o, "currencySign", CS_NAMES[s->csign]);
    }
    if (s->style == S_UNIT) {
        put_str(rt, o, "unit", UNITS[s->unit].id);
        put_str(rt, o, "unitDisplay", UD_NAMES[s->udisp]);
    }
    put_num(rt, o, "minimumIntegerDigits", s->dg.minid);
    if (s->dg.sd) {
        put_num(rt, o, "minimumSignificantDigits", s->dg.minsd);
        put_num(rt, o, "maximumSignificantDigits", s->dg.maxsd);
    } else {
        put_num(rt, o, "minimumFractionDigits", s->dg.minfd);
        put_num(rt, o, "maximumFractionDigits", s->dg.maxfd);
    }
    if (s->group == G_FALSE) {
        put_bool(rt, o, "useGrouping", false);
    } else {
        put_str(rt, o, "useGrouping", GROUP_NAMES[s->group]);
    }
    put_str(rt, o, "notation", "standard");
    put_str(rt, o, "signDisplay", SD_NAMES[s->sign]);
    put_num(rt, o, "roundingIncrement", 1);
    put_str(rt, o, "roundingMode", RM_NAMES[s->dg.rmode]);
    put_str(rt, o, "roundingPriority", "auto");
    put_str(rt, o, "trailingZeroDisplay", "auto");
    return embi_js_from_object(o);
}

/* The current offset of the local zone in minutes east, from a fresh Date. */
static bool local_offset(embi_js_runtime *rt, int32_t *off)
{
    embi_js_value ctor = slot(rt, R_DATE);
    embi_js_value dv = embi_js_construct(rt, ctor, 0, nullptr, ctor);
    if (embi_js_is_exception(dv)) {
        return false;
    }
    embi_js_value zv = embi_js_call(rt, slot(rt, R_GETTZOFF), dv, 0, nullptr);
    if (embi_js_is_exception(zv)) {
        return false;
    }
    *off = -(int32_t)embi_js_as_double(zv);
    return true;
}

/* The name of a fixed offset zone (minutes east): Etc/GMT-N for whole hours, else +HH:MM. These
 * names parse back through tz_parse, so resolvedOptions().timeZone can be passed to the constructor.
 * For the local zone the name is a label: it is the offset in effect when the options are resolved,
 * and the zone it names does not follow daylight saving changes. */
static embi_js_value offset_zone_name(embi_js_runtime *rt, int32_t off)
{
    if (off == 0) {
        return cstr(rt, "UTC");
    }
    char b[24];
    int n = 0;
    int32_t a = off < 0 ? -off : off;
    if (a % 60 == 0) {
        const char *base = "Etc/GMT";
        while (*base) {
            b[n++] = *base++;
        }
        b[n++] = off > 0 ? '-' : '+';
        int32_t h = a / 60;
        if (h >= 10) {
            b[n++] = (char)('0' + h / 10);
        }
        b[n++] = (char)('0' + h % 10);
    } else {
        int32_t h = a / 60, m = a % 60;
        b[n++] = off < 0 ? '-' : '+';
        b[n++] = (char)('0' + h / 10);
        b[n++] = (char)('0' + h % 10);
        b[n++] = ':';
        b[n++] = (char)('0' + m / 10);
        b[n++] = (char)('0' + m % 10);
    }
    return str_val(rt, b, (size_t)n);
}

static embi_js_value dtf_resolved(embi_js_runtime *rt, const dtf_state *s)
{
    int32_t off = s->tz == TZ_FIXED ? s->off : 0;
    if (s->tz == TZ_LOCAL && !local_offset(rt, &off)) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_str(rt, o, "locale", "en-US");
    put_str(rt, o, "calendar", "gregory");
    put_str(rt, o, "numberingSystem", "latn");
    put_val(rt, o, "timeZone", offset_zone_name(rt, off));
    if (s->dstyle || s->tstyle) {
        if (s->tstyle) {
            if (s->hc != HC_NONE) {
                put_str(rt, o, "hourCycle", HC_NAMES[s->hc]);
                put_bool(rt, o, "hour12", s->hc == HC_H11 || s->hc == HC_H12);
            }
        }
        if (s->dstyle) {
            put_str(rt, o, "dateStyle", STYLE_LIST[s->dstyle - 1]);
        }
        if (s->tstyle) {
            put_str(rt, o, "timeStyle", STYLE_LIST[s->tstyle - 1]);
        }
        return embi_js_from_object(o);
    }
    if (s->hc != HC_NONE) {
        put_str(rt, o, "hourCycle", HC_NAMES[s->hc]);
        put_bool(rt, o, "hour12", s->hc == HC_H11 || s->hc == HC_H12);
    }
    if (s->wd) {
        put_str(rt, o, "weekday", WD_LIST[s->wd - 1]);
    }
    if (s->yr) {
        put_str(rt, o, "year", YR_LIST[s->yr - 1]);
    }
    if (s->mo) {
        put_str(rt, o, "month", MO_LIST[s->mo - 1]);
    }
    if (s->dy) {
        put_str(rt, o, "day", YR_LIST[s->dy - 1]);
    }
    if (s->hr) {
        put_str(rt, o, "hour", YR_LIST[s->hr - 1]);
    }
    if (s->mi) {
        put_str(rt, o, "minute", YR_LIST[s->mi - 1]);
    }
    if (s->se) {
        put_str(rt, o, "second", YR_LIST[s->se - 1]);
    }
    if (s->fsd) {
        put_num(rt, o, "fractionalSecondDigits", s->fsd);
    }
    if (s->tzn) {
        put_str(rt, o, "timeZoneName", TZN_LIST[s->tzn - 1]);
    }
    return embi_js_from_object(o);
}

static embi_js_value coll_resolved(embi_js_runtime *rt, const coll_state *s)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_str(rt, o, "locale", "en-US");
    put_str(rt, o, "usage", s->search ? "search" : "sort");
    put_str(rt, o, "sensitivity", SENS_LIST[s->sens]);
    put_bool(rt, o, "ignorePunctuation", s->ignore_punct);
    put_str(rt, o, "collation", "default");
    put_bool(rt, o, "numeric", s->numeric);
    put_str(rt, o, "caseFirst", CASE_FIRST_NAMES[s->case_first]);
    return embi_js_from_object(o);
}

static embi_js_value pr_resolved(embi_js_runtime *rt, const pr_state *s)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_str(rt, o, "locale", "en-US");
    put_str(rt, o, "type", PR_TYPE_NAMES[s->type]);
    put_str(rt, o, "notation", "standard");
    put_num(rt, o, "minimumIntegerDigits", s->dg.minid);
    if (s->dg.sd) {
        put_num(rt, o, "minimumSignificantDigits", s->dg.minsd);
        put_num(rt, o, "maximumSignificantDigits", s->dg.maxsd);
    } else {
        put_num(rt, o, "minimumFractionDigits", s->dg.minfd);
        put_num(rt, o, "maximumFractionDigits", s->dg.maxfd);
    }
    embi_js_object *cats = embi_js_array_create_default(rt, 0);
    static const char *const CARD[2] = {"one", "other"};
    static const char *const ORD[4] = {"few", "one", "two", "other"};
    int n = s->type == PR_CARDINAL ? 2 : 4;
    for (int i = 0; i < n; i++) {
        const char *cat = s->type == PR_CARDINAL ? CARD[i] : ORD[i];
        (void)embi_js_create_data_property_or_throw(rt, cats, embi_js_key_from_index((uint32_t)i),
                                                    cstr(rt, cat));
    }
    put_val(rt, o, "pluralCategories", embi_js_from_object(cats));
    put_num(rt, o, "roundingIncrement", 1);
    put_str(rt, o, "roundingMode", RM_NAMES[s->dg.rmode]);
    put_str(rt, o, "roundingPriority", "auto");
    put_str(rt, o, "trailingZeroDisplay", "auto");
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ objects */

static void state_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_value(t, ((bound_hdr *)o->internal)->bound);
    }
}

static void nf_fin(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(nf_state));
    }
}

static void dtf_fin(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(dtf_state));
    }
}

static void coll_fin(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(coll_state));
    }
}

static void pr_fin(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(pr_state));
    }
}

static const embi_js_class nf_class = {
    .name = "Intl.NumberFormat", .trace = state_trace, .finalize = nf_fin};
static const embi_js_class dtf_class = {
    .name = "Intl.DateTimeFormat", .trace = state_trace, .finalize = dtf_fin};
static const embi_js_class coll_class = {
    .name = "Intl.Collator", .trace = state_trace, .finalize = coll_fin};
static const embi_js_class pr_class = {
    .name = "Intl.PluralRules", .trace = state_trace, .finalize = pr_fin};

/* The new object of a constructor. The state is owned by the object from here on, and freed
 * here when the prototype lookup throws. */
static embi_js_value make_obj(embi_js_runtime *rt, const embi_js_class *k, int proto_slot,
                              embi_js_value nt, void *state, size_t size)
{
    embi_js_value proto = proto_from_nt(rt, nt, slot(rt, proto_slot));
    if (embi_js_is_exception(proto)) {
        embi_free(state, size);
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, k);
    embi_js_obj_set_internal(o, state);
    return embi_js_from_object(o);
}

static embi_js_value nf_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    nf_state *s = embi_calloc(1, sizeof *s);
    if (!nf_init(rt, s, arg_at(argc, argv, 0), arg_at(argc, argv, 1))) {
        embi_free(s, sizeof *s);
        return EXC;
    }
    return make_obj(rt, &nf_class, R_NF_PROTO, nt, s, sizeof *s);
}

static embi_js_value dtf_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    dtf_state *s = embi_calloc(1, sizeof *s);
    if (!dtf_init(rt, s, arg_at(argc, argv, 0), arg_at(argc, argv, 1), REQ_ANY, DEF_DATE)) {
        embi_free(s, sizeof *s);
        return EXC;
    }
    return make_obj(rt, &dtf_class, R_DTF_PROTO, nt, s, sizeof *s);
}

static embi_js_value coll_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    coll_state *s = embi_calloc(1, sizeof *s);
    if (!coll_init(rt, s, arg_at(argc, argv, 0), arg_at(argc, argv, 1))) {
        embi_free(s, sizeof *s);
        return EXC;
    }
    return make_obj(rt, &coll_class, R_COLL_PROTO, nt, s, sizeof *s);
}

static embi_js_value pr_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(nt)) {
        return embi_js_throw_type_error(rt, "Constructor Intl.PluralRules requires 'new'");
    }
    pr_state *s = embi_calloc(1, sizeof *s);
    if (!pr_init(rt, s, arg_at(argc, argv, 0), arg_at(argc, argv, 1))) {
        embi_free(s, sizeof *s);
        return EXC;
    }
    return make_obj(rt, &pr_class, R_PR_PROTO, nt, s, sizeof *s);
}

/* ------------------------------------------------------------------ prototype methods */

/* The bound getter: the function bound to the receiver, created once per object. Like the built-in
 * functions of ECMA-402 it is anonymous with own length (1, or 2 for compare) and name "", in that
 * order, and it is not a constructor (a bound function of a non-constructor). */
static embi_js_value bound_get(embi_js_realm *r, embi_js_value this_v, bound_hdr *h, int target)
{
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(h->bound)) {
        embi_js_object *b = embi_js_bound_function_new(rt, embi_js_as_object(slot(rt, target)),
                                                       this_v, 0, nullptr);
        if (!b) {
            return EXC;
        }
        /* Stored first, so the object is reachable while the properties are added. */
        h->bound = embi_js_from_object(b);
        EMBI_JS_WB(embi_js_as_object(this_v), h->bound);
        embi_js_define_raw(rt, b, embi_js_key_c(rt, "length"),
                           embi_js_number(target == R_COLL_CMP ? 2 : 1), ATTR_C);
        embi_js_define_raw(rt, b, embi_js_key_c(rt, "name"), cstr(rt, ""), ATTR_C);
    }
    return h->bound;
}

static embi_js_value nf_format_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    nf_state *s = brand(r->rt, this_v, &nf_class, "get Intl.NumberFormat.prototype.format");
    return s ? bound_get(r, this_v, &s->hdr, R_NF_FMT) : EXC;
}

static embi_js_value nf_format_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    nf_state *s = brand(r->rt, this_v, &nf_class, "Intl.NumberFormat.prototype.format");
    if (!s) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!nf_run(r->rt, s, arg_at(argc, argv, 0), &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = str_val(r->rt, embi_sb_cstr(&f.sb), f.sb.len);
    fmt_free(&f);
    return out;
}

static embi_js_value nf_to_parts(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    nf_state *s = brand(r->rt, this_v, &nf_class, "Intl.NumberFormat.prototype.formatToParts");
    if (!s) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!nf_run(r->rt, s, arg_at(argc, argv, 0), &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = parts_array(r->rt, &f);
    fmt_free(&f);
    return out;
}

static embi_js_value nf_resolved_options(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    nf_state *s = brand(r->rt, this_v, &nf_class, "Intl.NumberFormat.prototype.resolvedOptions");
    return s ? nf_resolved(r->rt, s) : EXC;
}

static embi_js_value dtf_format_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    dtf_state *s = brand(r->rt, this_v, &dtf_class, "get Intl.DateTimeFormat.prototype.format");
    return s ? bound_get(r, this_v, &s->hdr, R_DTF_FMT) : EXC;
}

static embi_js_value dtf_format_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)magic;
    dtf_state *s = brand(r->rt, this_v, &dtf_class, "Intl.DateTimeFormat.prototype.format");
    if (!s) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!dtf_format_value(r->rt, s, arg_at(argc, argv, 0), &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = str_val(r->rt, embi_sb_cstr(&f.sb), f.sb.len);
    fmt_free(&f);
    return out;
}

static embi_js_value dtf_to_parts(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    dtf_state *s = brand(r->rt, this_v, &dtf_class, "Intl.DateTimeFormat.prototype.formatToParts");
    if (!s) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!dtf_format_value(r->rt, s, arg_at(argc, argv, 0), &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = parts_array(r->rt, &f);
    fmt_free(&f);
    return out;
}

static embi_js_value dtf_resolved_options(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    dtf_state *s = brand(r->rt, this_v, &dtf_class, "Intl.DateTimeFormat.prototype.resolvedOptions");
    return s ? dtf_resolved(r->rt, s) : EXC;
}

static embi_js_value coll_compare_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    coll_state *s = brand(r->rt, this_v, &coll_class, "get Intl.Collator.prototype.compare");
    return s ? bound_get(r, this_v, &s->hdr, R_COLL_CMP) : EXC;
}

static embi_js_value coll_compare_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    coll_state *s = brand(rt, this_v, &coll_class, "Intl.Collator.prototype.compare");
    if (!s) {
        return EXC;
    }
    embi_js_string *a = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!a) {
        return EXC;
    }
    embi_js_string *b = embi_js_to_string(rt, arg_at(argc, argv, 1));
    if (!b) {
        return EXC;
    }
    return embi_js_from_int32(coll_compare(s, a, b));
}

static embi_js_value coll_resolved_options(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    coll_state *s = brand(r->rt, this_v, &coll_class, "Intl.Collator.prototype.resolvedOptions");
    return s ? coll_resolved(r->rt, s) : EXC;
}

static embi_js_value pr_select_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    pr_state *s = brand(r->rt, this_v, &pr_class, "Intl.PluralRules.prototype.select");
    if (!s) {
        return EXC;
    }
    const char *cat = pr_select(r->rt, s, arg_at(argc, argv, 0));
    return cat ? cstr(r->rt, cat) : EXC;
}

static embi_js_value pr_resolved_options(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    pr_state *s = brand(r->rt, this_v, &pr_class, "Intl.PluralRules.prototype.resolvedOptions");
    return s ? pr_resolved(r->rt, s) : EXC;
}

/* ------------------------------------------------------------------ Intl statics */

static embi_js_value intl_get_canonical(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_object *a = canon_locales(r->rt, arg_at(argc, argv, 0));
    return a ? embi_js_from_object(a) : EXC;
}

static embi_js_value supported_locales_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *all = canon_locales(rt, arg_at(argc, argv, 0));
    if (!all) {
        return EXC;
    }
    embi_js_object *o = nullptr;
    if (!options_obj(rt, arg_at(argc, argv, 1), &o)) {
        return EXC;
    }
    static const char *const MATCHER[2] = {"lookup", "best fit"};
    if (opt_choice(rt, o, "localeMatcher", MATCHER, 2) == -2) {
        return EXC;
    }
    double len = 0.0;
    if (!embi_js_length_of_array_like(rt, all, &len)) {
        return EXC;
    }
    embi_js_object *out = embi_js_array_create_default(rt, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < (uint32_t)len; i++) {
        embi_js_value e = embi_js_get(rt, all, embi_js_key_from_index(i));
        if (embi_js_is_exception(e)) {
            return EXC;
        }
        if (tag_is_supported(rt, e) &&
            embi_js_create_data_property_or_throw(rt, out, embi_js_key_from_index(n++), e) ==
                EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(out);
}

/* ------------------------------------------------------------------ toLocaleString family */

static embi_js_value num_to_locale(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value x = this_v;
    if (!embi_js_is_number(x)) {
        if (!(embi_js_is_object(x) && embi_js_as_object(x)->klass == &embi_js_class_number)) {
            return embi_js_throw_type_error(
                rt, "Number.prototype.toLocaleString requires that 'this' be a Number");
        }
        x = embi_js_wrapper_value(embi_js_as_object(x));
    }
    nf_state st;
    if (!nf_init(rt, &st, arg_at(argc, argv, 0), arg_at(argc, argv, 1))) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!nf_run(rt, &st, x, &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = str_val(rt, embi_sb_cstr(&f.sb), f.sb.len);
    fmt_free(&f);
    return out;
}

static embi_js_value bigint_to_locale(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value x = this_v;
    if (!embi_js_is_bigint(x)) {
        if (!(embi_js_is_object(x) && embi_js_as_object(x)->klass == &embi_js_class_bigint)) {
            return embi_js_throw_type_error(
                rt, "BigInt.prototype.toLocaleString requires that 'this' be a BigInt");
        }
        x = embi_js_bigint_wrapper_value(embi_js_as_object(x));
    }
    nf_state st;
    if (!nf_init(rt, &st, arg_at(argc, argv, 0), arg_at(argc, argv, 1))) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!nf_run(rt, &st, x, &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = str_val(rt, embi_sb_cstr(&f.sb), f.sb.len);
    fmt_free(&f);
    return out;
}

/* Date.prototype.toLocaleString (magic 0), toLocaleDateString (1) and toLocaleTimeString (2). */
static embi_js_value date_to_locale(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value tv = embi_js_call(rt, slot(rt, R_GETTIME), this_v, 0, nullptr);
    if (embi_js_is_exception(tv)) {
        return EXC;
    }
    if (embi_js_as_double(tv) != embi_js_as_double(tv)) {
        return cstr(rt, "Invalid Date");
    }
    dtf_state st;
    int req = magic == 0 ? REQ_ANY : magic == 1 ? REQ_DATE : REQ_TIME;
    int def = magic == 0 ? DEF_ALL : magic == 1 ? DEF_DATE : DEF_TIME;
    if (!dtf_init(rt, &st, arg_at(argc, argv, 0), arg_at(argc, argv, 1), req, def)) {
        return EXC;
    }
    fmt f;
    memset(&f, 0, sizeof f);
    if (!dtf_run(rt, &st, this_v, &f)) {
        fmt_free(&f);
        return EXC;
    }
    embi_js_value out = str_val(rt, embi_sb_cstr(&f.sb), f.sb.len);
    fmt_free(&f);
    return out;
}

static embi_js_value str_locale_compare(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "String.prototype.localeCompare called on null or undefined");
    }
    embi_js_string *a = embi_js_to_string(rt, this_v);
    if (!a) {
        return EXC;
    }
    embi_js_string *b = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!b) {
        return EXC;
    }
    coll_state st;
    if (!coll_init(rt, &st, arg_at(argc, argv, 1), arg_at(argc, argv, 2))) {
        return EXC;
    }
    return embi_js_from_int32(coll_compare(&st, a, b));
}

/* Array.prototype.toLocaleString: each element's toLocaleString gets the locales and options. The
 * pieces go into one UTF-16 vector, so the cost is linear, and the native stack is checked like
 * join, so a cyclic array throws a RangeError instead of overflowing the stack. */
static embi_js_value arr_to_locale(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_native_stack_ok(rt)) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len = 0.0;
    if (!embi_js_length_of_array_like(rt, o, &len)) {
        return EXC;
    }
    EMBI_VEC(uint16_t) out = {nullptr, 0, 0};
    embi_js_value result = EXC;
    for (double k = 0.0; k < len; k++) {
        if (k > 0.0) {
            embi_vec_push(&out, (uint16_t)',');
        }
        embi_js_key key = embi_js_to_property_key(rt, embi_js_number(k));
        if (embi_js_is_exception(key)) {
            goto done;
        }
        embi_js_value e = embi_js_get(rt, o, key);
        if (embi_js_is_exception(e)) {
            goto done;
        }
        if (embi_js_is_nullish(e)) {
            continue;
        }
        embi_js_value av[2] = {arg_at(argc, argv, 0), arg_at(argc, argv, 1)};
        embi_js_value s = embi_js_invoke(rt, e, embi_js_key_c(rt, "toLocaleString"), 2, av);
        if (embi_js_is_exception(s)) {
            goto done;
        }
        embi_js_string *ss = embi_js_to_string(rt, s);
        if (!ss) {
            goto done;
        }
        uint32_t n = embi_js_str_len(ss);
        for (uint32_t i = 0; i < n; i++) {
            embi_vec_push(&out, embi_js_str_at(ss, i));
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

/* Defines an Intl constructor with its prototype (kept in the root record) and supportedLocalesOf.
 * Returns the prototype. */
static embi_js_object *install_ctor(embi_js_realm *r, embi_js_object *intl, const char *name,
                                    embi_js_native_ctor fn, int proto_slot, const char *tag)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *ctor = embi_js_native_constructor_new(r, name, 0, fn, 0);
    embi_js_object *proto = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), cstr(rt, tag),
                       ATTR_C);
    embi_js_define_raw(rt, intl, embi_js_key_c(rt, name), embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_method(r, ctor, "supportedLocalesOf", supported_locales_of, 1, 0);
    EMBI_JS_STORE(record(rt), record(rt)->elems[proto_slot], embi_js_from_object(proto));
    return proto;
}

static void install_getter(embi_js_realm *r, embi_js_object *proto, const char *name,
                           embi_js_native fn, int target_slot, const char *target_name,
                           int target_length, embi_js_native target_fn)
{
    embi_js_runtime *rt = r->rt;
    char getter_name[64];
    size_t n = strlen(name);
    if (n > 40) {
        n = 40;
    }
    memcpy(getter_name, "get ", 4);
    memcpy(getter_name + 4, name, n);
    getter_name[4 + n] = '\0';
    embi_js_object *g = embi_js_native_function_new(r, getter_name, 0, fn, 0);
    embi_js_define_accessor_raw(rt, proto, embi_js_key_c(rt, name), embi_js_from_object(g), UNDEF,
                                ATTR_C);
    embi_js_object *t = embi_js_native_function_new(r, target_name, target_length, target_fn, 0);
    EMBI_JS_STORE(record(rt), record(rt)->elems[target_slot], embi_js_from_object(t));
}

void embi_js_init_builtins_intl(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *rec = embi_js_varray_new(rt, R_COUNT);
    for (int i = 0; i < R_COUNT; i++) {
        EMBI_JS_STORE(rec, rec->elems[i], UNDEF);
    }
    embi_js_root_add(rt, root_mark, rec);

    /* The Date constructor and Date.prototype.getTime, for the Date formats. */
    embi_js_value dctor = embi_js_get(rt, r->global, embi_js_key_c(rt, "Date"));
    EMBI_JS_WB(rec, dctor);
    EMBI_JS_STORE(rec, rec->elems[R_DATE], dctor);
    embi_js_value dproto = embi_js_get(rt, embi_js_as_object(dctor),
                                       embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
    EMBI_JS_STORE(rec, rec->elems[R_GETTIME], embi_js_get(rt, embi_js_as_object(dproto), embi_js_key_c(rt, "getTime")));
    EMBI_JS_STORE(rec, rec->elems[R_GETTZOFF],
                  embi_js_get(rt, embi_js_as_object(dproto), embi_js_key_c(rt, "getTimezoneOffset")));

    embi_js_object *intl = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Intl"), embi_js_from_object(intl), ATTR_WC);
    embi_js_define_raw(rt, intl, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), cstr(rt, "Intl"),
                       ATTR_C);
    embi_js_define_method(r, intl, "getCanonicalLocales", intl_get_canonical, 1, 0);

    embi_js_object *nf = install_ctor(r, intl, "NumberFormat", nf_ctor, R_NF_PROTO,
                                      "Intl.NumberFormat");
    install_getter(r, nf, "format", nf_format_get, R_NF_FMT, "", 1, nf_format_native);
    embi_js_define_method(r, nf, "formatToParts", nf_to_parts, 1, 0);
    embi_js_define_method(r, nf, "resolvedOptions", nf_resolved_options, 0, 0);

    embi_js_object *dtf = install_ctor(r, intl, "DateTimeFormat", dtf_ctor, R_DTF_PROTO,
                                       "Intl.DateTimeFormat");
    install_getter(r, dtf, "format", dtf_format_get, R_DTF_FMT, "", 1, dtf_format_native);
    embi_js_define_method(r, dtf, "formatToParts", dtf_to_parts, 1, 0);
    embi_js_define_method(r, dtf, "resolvedOptions", dtf_resolved_options, 0, 0);

    embi_js_object *coll = install_ctor(r, intl, "Collator", coll_ctor, R_COLL_PROTO,
                                        "Intl.Collator");
    install_getter(r, coll, "compare", coll_compare_get, R_COLL_CMP, "", 2, coll_compare_native);
    embi_js_define_method(r, coll, "resolvedOptions", coll_resolved_options, 0, 0);

    embi_js_object *pr = install_ctor(r, intl, "PluralRules", pr_ctor, R_PR_PROTO,
                                      "Intl.PluralRules");
    embi_js_define_method(r, pr, "select", pr_select_method, 1, 0);
    embi_js_define_method(r, pr, "resolvedOptions", pr_resolved_options, 0, 0);

    /* The toLocaleString family. These replace the toString-based methods of the other builtins. */
    embi_js_define_method(r, embi_js_as_object(rt->protos[EMBI_JS_PROTO_NUMBER]),
                          "toLocaleString", num_to_locale, 0, 0);
    embi_js_define_method(r, embi_js_as_object(rt->protos[EMBI_JS_PROTO_BIGINT]),
                          "toLocaleString", bigint_to_locale, 0, 0);
    embi_js_define_method(r, embi_js_as_object(rt->protos[EMBI_JS_PROTO_ARRAY]),
                          "toLocaleString", arr_to_locale, 0, 0);
    embi_js_define_method(r, embi_js_as_object(rt->protos[EMBI_JS_PROTO_STRING]),
                          "localeCompare", str_locale_compare, 1, 0);
    embi_js_object *dp = embi_js_as_object(dproto);
    embi_js_define_method(r, dp, "toLocaleString", date_to_locale, 0, 0);
    embi_js_define_method(r, dp, "toLocaleDateString", date_to_locale, 0, 1);
    embi_js_define_method(r, dp, "toLocaleTimeString", date_to_locale, 0, 2);
}
