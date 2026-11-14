/*
 * Abstract operations (ECMA-262 7.1 to 7.3, 7.2, 6.2.6 and 10.4/10.2 helpers). See ops.h for the
 * error conventions.
 */
#include "js/ops.h"
#include "js/typedarray.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define EMBI_JS_MAX_ARG_LIST (1u << 24)

#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE

#define UNDEF EMBI_JS_UNDEFINED
#define EXC EMBI_JS_EXCEPTION

/* ------------------------------------------------------------------ errors */

embi_js_value embi_js_throw_error(embi_js_runtime *rt, enum embi_js_proto which, const char *fmt,
                                  ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    embi_js_value proto = rt->protos[which];
    if (embi_js_is_empty(proto)) {
        proto = EMBI_JS_NULL;
    }
    embi_js_object *e = embi_js_object_new_class(rt, proto, &embi_js_class_error);
    embi_js_string *msg = embi_js_str_new_utf8(rt, buf, strlen(buf));
    embi_js_define_raw(rt, e, embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE), embi_js_from_string(msg),
                       ATTR_W | ATTR_C);
    embi_js_exception_set(rt, embi_js_from_object(e));
    return EXC;
}

embi_js_value embi_js_throw_value(embi_js_runtime *rt, embi_js_value v)
{
    embi_js_exception_set(rt, v);
    return EXC;
}

/* ------------------------------------------------------------------ number helpers */

/* ToUint32 without libm: decodes the double and reduces it modulo 2^32 with integer arithmetic. */
static uint32_t double_to_uint32(double d)
{
    if (!(d >= -DBL_MAX && d <= DBL_MAX)) {
        return 0; /* NaN and infinities */
    }
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    bool neg = (bits >> 63) != 0;
    int exp = (int)((bits >> 52) & 0x7FFu);
    uint64_t mant = bits & ((1ull << 52) - 1u);
    if (exp == 0) {
        exp = 1; /* subnormal: below 1 in magnitude, truncates to zero */
    } else {
        mant |= 1ull << 52;
    }
    int e = exp - 1075; /* |d| == mant * 2^e */
    uint32_t r;
    if (e >= 0) {
        r = e >= 32 ? 0u : (uint32_t)mant << e;
    } else {
        int s = -e;
        r = s >= 64 ? 0u : (uint32_t)(mant >> s);
    }
    if (neg) {
        r = 0u - r;
    }
    return r;
}

static double truncate_toward_zero(double n)
{
    if (n != n || n == 0.0) {
        return 0.0;
    }
    if (!(n >= -9.0e18 && n <= 9.0e18)) {
        return n; /* infinities, and large values that are already integers */
    }
    return (double)(int64_t)n;
}

/* ------------------------------------------------------------------ type conversion */

embi_js_value embi_js_to_primitive(embi_js_runtime *rt, embi_js_value v, embi_js_hint hint);

static embi_js_value ordinary_to_primitive(embi_js_runtime *rt, embi_js_value v, bool string_hint)
{
    embi_js_key first =
        embi_js_name_val(rt, string_hint ? EMBI_JS_NAME_TO_STRING : EMBI_JS_NAME_VALUE_OF);
    embi_js_key second =
        embi_js_name_val(rt, string_hint ? EMBI_JS_NAME_VALUE_OF : EMBI_JS_NAME_TO_STRING);
    embi_js_key order[2] = {first, second};
    for (int i = 0; i < 2; i++) {
        embi_js_value f = embi_js_get_v(rt, v, order[i]);
        if (embi_js_is_exception(f)) {
            return EXC;
        }
        if (embi_js_is_callable(f)) {
            embi_js_value r = embi_js_call(rt, f, v, 0, nullptr);
            if (embi_js_is_exception(r)) {
                return EXC;
            }
            if (!embi_js_is_object(r)) {
                return r;
            }
        }
    }
    return embi_js_throw_type_error(rt, "Cannot convert object to primitive value");
}

embi_js_value embi_js_to_primitive(embi_js_runtime *rt, embi_js_value v, embi_js_hint hint)
{
    if (!embi_js_is_object(v)) {
        return v;
    }
    embi_js_key tp = embi_js_wk_sym(rt, EMBI_JS_WK_TO_PRIMITIVE);
    embi_js_value ex = embi_js_get_method(rt, v, tp);
    if (embi_js_is_exception(ex)) {
        return EXC;
    }
    if (!embi_js_is_undefined(ex)) {
        embi_js_value hv = embi_js_name_val(
            rt, hint == EMBI_JS_HINT_STRING   ? EMBI_JS_NAME_STRING
                : hint == EMBI_JS_HINT_NUMBER ? EMBI_JS_NAME_NUMBER
                                              : EMBI_JS_NAME_DEFAULT);
        embi_js_value r = embi_js_call(rt, ex, v, 1, &hv);
        if (embi_js_is_exception(r)) {
            return EXC;
        }
        if (embi_js_is_object(r)) {
            return embi_js_throw_type_error(rt, "Cannot convert object to primitive value");
        }
        return r;
    }
    return ordinary_to_primitive(rt, v, hint == EMBI_JS_HINT_STRING);
}

bool embi_js_to_number(embi_js_runtime *rt, embi_js_value v, double *out)
{
    if (embi_js_is_number(v)) {
        *out = embi_js_as_double(v);
        return true;
    }
    if (embi_js_is_undefined(v)) {
        *out = NAN;
        return true;
    }
    if (embi_js_is_null(v)) {
        *out = 0.0;
        return true;
    }
    if (embi_js_is_bool(v)) {
        *out = v == EMBI_JS_TRUE ? 1.0 : 0.0;
        return true;
    }
    if (embi_js_is_string(v)) {
        *out = embi_js_str_to_number(embi_js_as_string(v));
        return true;
    }
    if (embi_js_is_symbol(v)) {
        (void)embi_js_throw_type_error(rt, "Cannot convert a Symbol value to a number");
        return false;
    }
    if (embi_js_is_bigint(v)) {
        (void)embi_js_throw_type_error(rt, "Cannot convert a BigInt value to a number");
        return false;
    }
    embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_NUMBER);
    if (embi_js_is_exception(p)) {
        return false;
    }
    return embi_js_to_number(rt, p, out);
}

embi_js_value embi_js_to_numeric(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_number(v) || embi_js_is_bigint(v)) {
        return v;
    }
    embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_NUMBER);
    if (embi_js_is_exception(p)) {
        return EXC;
    }
    if (embi_js_is_bigint(p)) {
        return p;
    }
    double d = 0.0;
    if (!embi_js_to_number(rt, p, &d)) {
        return EXC;
    }
    return embi_js_number(d);
}

bool embi_js_to_integer_or_infinity(embi_js_runtime *rt, embi_js_value v, double *out)
{
    double n = 0.0;
    if (!embi_js_to_number(rt, v, &n)) {
        return false;
    }
    *out = truncate_toward_zero(n);
    return true;
}

bool embi_js_to_int32(embi_js_runtime *rt, embi_js_value v, int32_t *out)
{
    double n = 0.0;
    if (!embi_js_to_number(rt, v, &n)) {
        return false;
    }
    *out = (int32_t)double_to_uint32(n);
    return true;
}

bool embi_js_to_uint32(embi_js_runtime *rt, embi_js_value v, uint32_t *out)
{
    double n = 0.0;
    if (!embi_js_to_number(rt, v, &n)) {
        return false;
    }
    *out = double_to_uint32(n);
    return true;
}

bool embi_js_to_length(embi_js_runtime *rt, embi_js_value v, double *out)
{
    double n = 0.0;
    if (!embi_js_to_integer_or_infinity(rt, v, &n)) {
        return false;
    }
    const double max_safe = 9007199254740991.0;
    *out = n <= 0.0 ? 0.0 : (n > max_safe ? max_safe : n);
    return true;
}

embi_js_string *embi_js_to_string(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_string(v)) {
        /* The flat form: callers read characters from the result (J5g). */
        return embi_js_str_flat(rt, v);
    }
    if (embi_js_is_number(v)) {
        return embi_js_number_to_str(rt, embi_js_as_double(v));
    }
    if (embi_js_is_undefined(v)) {
        return embi_js_as_string(embi_js_name_val(rt, EMBI_JS_NAME_UNDEFINED));
    }
    if (embi_js_is_null(v)) {
        return embi_js_as_string(embi_js_name_val(rt, EMBI_JS_NAME_NULL));
    }
    if (embi_js_is_bool(v)) {
        return embi_js_as_string(
            embi_js_name_val(rt, v == EMBI_JS_TRUE ? EMBI_JS_NAME_TRUE : EMBI_JS_NAME_FALSE));
    }
    if (embi_js_is_symbol(v)) {
        (void)embi_js_throw_type_error(rt, "Cannot convert a Symbol value to a string");
        return nullptr;
    }
    if (embi_js_is_bigint(v)) {
        return embi_js_bigint_to_string(rt, embi_js_as_bigint(v), 10);
    }
    embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_STRING);
    if (embi_js_is_exception(p)) {
        return nullptr;
    }
    return embi_js_to_string(rt, p);
}

embi_js_key embi_js_to_property_key(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_int32(v) && embi_js_as_int32(v) >= 0) {
        return embi_js_key_from_index((uint32_t)embi_js_as_int32(v));
    }
    if (embi_js_is_string(v)) {
        return embi_js_key_from_str(rt, embi_js_as_string(v));
    }
    if (embi_js_is_symbol(v)) {
        return v;
    }
    embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_STRING);
    if (embi_js_is_exception(p)) {
        return EXC;
    }
    if (embi_js_is_symbol(p)) {
        return p;
    }
    embi_js_string *s = embi_js_to_string(rt, p);
    if (!s) {
        return EXC;
    }
    return embi_js_key_from_str(rt, s);
}

embi_js_value embi_js_key_to_value(embi_js_runtime *rt, embi_js_key k)
{
    if (embi_js_key_is_index(k)) {
        return embi_js_from_string(
            embi_js_number_to_str(rt, (double)embi_js_key_index(k)));
    }
    return k;
}

embi_js_object *embi_js_to_object(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_nullish(v)) {
        (void)embi_js_throw_type_error(rt, "Cannot convert undefined or null to object");
        return nullptr;
    }
    if (embi_js_is_object(v)) {
        return embi_js_as_object(v);
    }
    if (embi_js_is_bigint(v)) {
        return embi_js_bigint_wrapper_new(rt, v);
    }
    return embi_js_primitive_wrapper_new(rt, v);
}

embi_js_value embi_js_typeof(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_undefined(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_UNDEFINED);
    }
    if (embi_js_is_null(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_OBJECT);
    }
    if (embi_js_is_bool(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_BOOLEAN);
    }
    if (embi_js_is_number(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_NUMBER);
    }
    if (embi_js_is_string(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_STRING);
    }
    if (embi_js_is_symbol(v)) {
        return embi_js_name_val(rt, EMBI_JS_NAME_SYMBOL);
    }
    if (embi_js_is_bigint(v)) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "bigint", 6));
    }
    return embi_js_name_val(rt, embi_js_is_callable(v) ? EMBI_JS_NAME_FUNCTION : EMBI_JS_NAME_OBJECT);
}

/* ------------------------------------------------------------------ equality and comparison */

bool embi_js_same_value(embi_js_value a, embi_js_value b)
{
    if (embi_js_is_number(a) && embi_js_is_number(b)) {
        double x = embi_js_as_double(a);
        double y = embi_js_as_double(b);
        if (x != x || y != y) {
            return x != x && y != y;
        }
        if (x == 0.0 && y == 0.0) {
            return signbit(x) == signbit(y); /* +0 and -0 differ */
        }
        return x == y;
    }
    if (embi_js_is_string(a) && embi_js_is_string(b)) {
        return embi_js_str_equal(embi_js_as_string(a), embi_js_as_string(b));
    }
    if (embi_js_is_bigint(a) && embi_js_is_bigint(b)) {
        return embi_js_bigint_compare(embi_js_as_bigint(a), embi_js_as_bigint(b)) == 0;
    }
    return a == b;
}

bool embi_js_same_value_zero(embi_js_value a, embi_js_value b)
{
    if (embi_js_is_number(a) && embi_js_is_number(b)) {
        double x = embi_js_as_double(a);
        double y = embi_js_as_double(b);
        if (x != x || y != y) {
            return x != x && y != y;
        }
        return x == y;
    }
    return embi_js_strict_equal(a, b);
}

bool embi_js_strict_equal(embi_js_value a, embi_js_value b)
{
    if (embi_js_is_number(a) && embi_js_is_number(b)) {
        return embi_js_as_double(a) == embi_js_as_double(b);
    }
    if (embi_js_is_string(a) && embi_js_is_string(b)) {
        return embi_js_str_equal(embi_js_as_string(a), embi_js_as_string(b));
    }
    if (embi_js_is_bigint(a) && embi_js_is_bigint(b)) {
        return embi_js_bigint_compare(embi_js_as_bigint(a), embi_js_as_bigint(b)) == 0;
    }
    return a == b;
}

/* Coarse kind used by IsLooselyEqual. */
enum loose_kind {
    KIND_UNDEFINED,
    KIND_NULL,
    KIND_BOOL,
    KIND_NUMBER,
    KIND_STRING,
    KIND_SYMBOL,
    KIND_BIGINT,
    KIND_OBJECT,
};

static enum loose_kind loose_kind_of(embi_js_value v)
{
    if (embi_js_is_undefined(v)) {
        return KIND_UNDEFINED;
    }
    if (embi_js_is_null(v)) {
        return KIND_NULL;
    }
    if (embi_js_is_bool(v)) {
        return KIND_BOOL;
    }
    if (embi_js_is_number(v)) {
        return KIND_NUMBER;
    }
    if (embi_js_is_string(v)) {
        return KIND_STRING;
    }
    if (embi_js_is_symbol(v)) {
        return KIND_SYMBOL;
    }
    if (embi_js_is_bigint(v)) {
        return KIND_BIGINT;
    }
    return KIND_OBJECT;
}

int embi_js_loose_equal(embi_js_runtime *rt, embi_js_value a, embi_js_value b)
{
    for (;;) {
        enum loose_kind ka = loose_kind_of(a);
        enum loose_kind kb = loose_kind_of(b);
        if (ka == kb) {
            return embi_js_strict_equal(a, b) ? EMBI_JS_YES : EMBI_JS_NO;
        }
        bool na = ka == KIND_UNDEFINED || ka == KIND_NULL;
        bool nb = kb == KIND_UNDEFINED || kb == KIND_NULL;
        if (na && nb) {
            return EMBI_JS_YES;
        }
        if (ka == KIND_NUMBER && kb == KIND_STRING) {
            double d = 0.0;
            if (!embi_js_to_number(rt, b, &d)) {
                return EMBI_JS_THREW;
            }
            b = embi_js_number(d);
            continue;
        }
        if (ka == KIND_STRING && kb == KIND_NUMBER) {
            double d = 0.0;
            if (!embi_js_to_number(rt, a, &d)) {
                return EMBI_JS_THREW;
            }
            a = embi_js_number(d);
            continue;
        }
        if (ka == KIND_BOOL) {
            a = embi_js_from_int32(a == EMBI_JS_TRUE ? 1 : 0);
            continue;
        }
        if (kb == KIND_BOOL) {
            b = embi_js_from_int32(b == EMBI_JS_TRUE ? 1 : 0);
            continue;
        }
        bool prim_b = kb == KIND_NUMBER || kb == KIND_STRING || kb == KIND_SYMBOL ||
                      kb == KIND_BIGINT;
        bool prim_a = ka == KIND_NUMBER || ka == KIND_STRING || ka == KIND_SYMBOL ||
                      ka == KIND_BIGINT;
        if (ka == KIND_BIGINT && kb == KIND_STRING) {
            embi_js_bigint *n = nullptr;
            if (!embi_js_string_to_bigint(rt, embi_js_as_string(b), &n)) {
                return EMBI_JS_NO;
            }
            if (!n) {
                return EMBI_JS_THREW;
            }
            return embi_js_bigint_compare(embi_js_as_bigint(a), n) == 0 ? EMBI_JS_YES : EMBI_JS_NO;
        }
        if (ka == KIND_STRING && kb == KIND_BIGINT) {
            embi_js_bigint *n = nullptr;
            if (!embi_js_string_to_bigint(rt, embi_js_as_string(a), &n)) {
                return EMBI_JS_NO;
            }
            if (!n) {
                return EMBI_JS_THREW;
            }
            return embi_js_bigint_compare(n, embi_js_as_bigint(b)) == 0 ? EMBI_JS_YES : EMBI_JS_NO;
        }
        if (ka == KIND_BIGINT && kb == KIND_NUMBER) {
            return embi_js_bigint_compare_double(embi_js_as_bigint(a), embi_js_as_double(b)) == 0
                       ? EMBI_JS_YES
                       : EMBI_JS_NO;
        }
        if (ka == KIND_NUMBER && kb == KIND_BIGINT) {
            return embi_js_bigint_compare_double(embi_js_as_bigint(b), embi_js_as_double(a)) == 0
                       ? EMBI_JS_YES
                       : EMBI_JS_NO;
        }
        if (ka == KIND_OBJECT && prim_b) {
            a = embi_js_to_primitive(rt, a, EMBI_JS_HINT_DEFAULT);
            if (embi_js_is_exception(a)) {
                return EMBI_JS_THREW;
            }
            continue;
        }
        if (kb == KIND_OBJECT && prim_a) {
            b = embi_js_to_primitive(rt, b, EMBI_JS_HINT_DEFAULT);
            if (embi_js_is_exception(b)) {
                return EMBI_JS_THREW;
            }
            continue;
        }
        return EMBI_JS_NO;
    }
}

int embi_js_less_than(embi_js_runtime *rt, embi_js_value x, embi_js_value y, bool left_first)
{
    embi_js_value px;
    embi_js_value py;
    if (left_first) {
        px = embi_js_to_primitive(rt, x, EMBI_JS_HINT_NUMBER);
        if (embi_js_is_exception(px)) {
            return EMBI_JS_CMP_THREW;
        }
        py = embi_js_to_primitive(rt, y, EMBI_JS_HINT_NUMBER);
        if (embi_js_is_exception(py)) {
            return EMBI_JS_CMP_THREW;
        }
    } else {
        py = embi_js_to_primitive(rt, y, EMBI_JS_HINT_NUMBER);
        if (embi_js_is_exception(py)) {
            return EMBI_JS_CMP_THREW;
        }
        px = embi_js_to_primitive(rt, x, EMBI_JS_HINT_NUMBER);
        if (embi_js_is_exception(px)) {
            return EMBI_JS_CMP_THREW;
        }
    }
    if (embi_js_is_string(px) && embi_js_is_string(py)) {
        return embi_js_str_compare(embi_js_as_string(px), embi_js_as_string(py)) < 0
                   ? EMBI_JS_CMP_TRUE
                   : EMBI_JS_CMP_FALSE;
    }
    /* A BigInt against a string compares with StringToBigInt of the string (undefined when it is
     * not a BigInt). */
    if (embi_js_is_bigint(px) && embi_js_is_string(py)) {
        embi_js_bigint *ny = nullptr;
        if (!embi_js_string_to_bigint(rt, embi_js_as_string(py), &ny)) {
            return EMBI_JS_CMP_UNDEFINED;
        }
        if (!ny) {
            return EMBI_JS_CMP_THREW;
        }
        return embi_js_bigint_compare(embi_js_as_bigint(px), ny) < 0 ? EMBI_JS_CMP_TRUE
                                                                     : EMBI_JS_CMP_FALSE;
    }
    if (embi_js_is_string(px) && embi_js_is_bigint(py)) {
        embi_js_bigint *nx = nullptr;
        if (!embi_js_string_to_bigint(rt, embi_js_as_string(px), &nx)) {
            return EMBI_JS_CMP_UNDEFINED;
        }
        if (!nx) {
            return EMBI_JS_CMP_THREW;
        }
        return embi_js_bigint_compare(nx, embi_js_as_bigint(py)) < 0 ? EMBI_JS_CMP_TRUE
                                                                     : EMBI_JS_CMP_FALSE;
    }
    /* ToNumeric on both sides; then BigInt against BigInt or Number. */
    embi_js_value vx = embi_js_to_numeric(rt, px);
    if (embi_js_is_exception(vx)) {
        return EMBI_JS_CMP_THREW;
    }
    embi_js_value vy = embi_js_to_numeric(rt, py);
    if (embi_js_is_exception(vy)) {
        return EMBI_JS_CMP_THREW;
    }
    if (embi_js_is_bigint(vx) && embi_js_is_bigint(vy)) {
        return embi_js_bigint_compare(embi_js_as_bigint(vx), embi_js_as_bigint(vy)) < 0
                   ? EMBI_JS_CMP_TRUE
                   : EMBI_JS_CMP_FALSE;
    }
    if (embi_js_is_bigint(vx)) {
        int c = embi_js_bigint_compare_double(embi_js_as_bigint(vx), embi_js_as_double(vy));
        if (c == 2) {
            return EMBI_JS_CMP_UNDEFINED;
        }
        return c < 0 ? EMBI_JS_CMP_TRUE : EMBI_JS_CMP_FALSE;
    }
    if (embi_js_is_bigint(vy)) {
        int c = embi_js_bigint_compare_double(embi_js_as_bigint(vy), embi_js_as_double(vx));
        if (c == 2) {
            return EMBI_JS_CMP_UNDEFINED;
        }
        return c > 0 ? EMBI_JS_CMP_TRUE : EMBI_JS_CMP_FALSE;
    }
    double nx = embi_js_as_double(vx);
    double ny = embi_js_as_double(vy);
    if (nx != nx || ny != ny) {
        return EMBI_JS_CMP_UNDEFINED;
    }
    return nx < ny ? EMBI_JS_CMP_TRUE : EMBI_JS_CMP_FALSE;
}

/* ------------------------------------------------------------------ property access */

embi_js_value embi_js_get_v(embi_js_runtime *rt, embi_js_value v, embi_js_key k)
{
    if (embi_js_is_object(v)) {
        return embi_js_obj_get(rt, embi_js_as_object(v), k, v);
    }
    embi_js_object *o = embi_js_to_object(rt, v);
    if (!o) {
        return EXC;
    }
    return embi_js_obj_get(rt, o, k, v);
}

int embi_js_put(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                bool throw_on_fail)
{
    int r = embi_js_obj_set(rt, o, k, v, embi_js_from_object(o));
    if (r == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (r == EMBI_JS_NO && throw_on_fail) {
        (void)embi_js_throw_type_error(rt, "Cannot assign to read only property");
        return EMBI_JS_THREW;
    }
    return r;
}

embi_js_value embi_js_get_method(embi_js_runtime *rt, embi_js_value v, embi_js_key k)
{
    embi_js_value f = embi_js_get_v(rt, v, k);
    if (embi_js_is_exception(f)) {
        return EXC;
    }
    if (embi_js_is_nullish(f)) {
        return UNDEF;
    }
    if (!embi_js_is_callable(f)) {
        return embi_js_throw_type_error(rt, "property is not a function");
    }
    return f;
}

int embi_js_create_data_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_value v)
{
    embi_js_pdesc d = {v, UNDEF, UNDEF,
                       EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                           EMBI_JS_PD_CONFIGURABLE,
                       ATTR_W | ATTR_E | ATTR_C};
    return embi_js_obj_define_own_property(rt, o, k, &d);
}

int embi_js_create_data_property_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                          embi_js_value v)
{
    int r = embi_js_create_data_property(rt, o, k, v);
    if (r == EMBI_JS_NO) {
        (void)embi_js_throw_type_error(rt, "Cannot define property");
        return EMBI_JS_THREW;
    }
    return r;
}

int embi_js_define_property_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                     const embi_js_pdesc *d)
{
    int r = embi_js_obj_define_own_property(rt, o, k, d);
    if (r == EMBI_JS_NO) {
        (void)embi_js_throw_type_error(rt, "Cannot redefine property");
        return EMBI_JS_THREW;
    }
    return r;
}

int embi_js_delete_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    int r = embi_js_obj_delete(rt, o, k);
    if (r == EMBI_JS_NO) {
        (void)embi_js_throw_type_error(rt, "Cannot delete property");
        return EMBI_JS_THREW;
    }
    return r;
}

int embi_js_has_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    return embi_js_obj_has_own_property(rt, o, k);
}

bool embi_js_length_of_array_like(embi_js_runtime *rt, embi_js_object *o, double *out)
{
    embi_js_value len = embi_js_get(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH));
    if (embi_js_is_exception(len)) {
        return false;
    }
    return embi_js_to_length(rt, len, out);
}

embi_js_varray *embi_js_list_from_array_like(embi_js_runtime *rt, embi_js_value v, uint32_t *len)
{
    if (!embi_js_is_object(v)) {
        (void)embi_js_throw_type_error(rt, "CreateListFromArrayLike called on non-object");
        return nullptr;
    }
    embi_js_object *o = embi_js_as_object(v);
    double n = 0.0;
    if (!embi_js_length_of_array_like(rt, o, &n)) {
        return nullptr;
    }
    if (n > (double)EMBI_JS_MAX_ARG_LIST) {
        (void)embi_js_throw_range_error(rt, "too many arguments in array-like");
        return nullptr;
    }
    uint32_t count = (uint32_t)n;
    embi_js_varray *a = embi_js_varray_new(rt, count);
    for (uint32_t i = 0; i < count; i++) {
        embi_js_value x = embi_js_obj_get(rt, o, embi_js_key_from_index(i), v);
        if (embi_js_is_exception(x)) {
            return nullptr;
        }
        EMBI_JS_STORE(a, a->elems[i], x);
    }
    *len = count;
    return a;
}

int embi_js_enumerable_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, o, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return EMBI_JS_THREW;
    }
    int result = EMBI_JS_YES;
    for (uint32_t i = 0; i < keys.len; i++) {
        embi_js_key k = keys.data[i];
        if (embi_js_is_symbol(k)) {
            continue;
        }
        embi_js_pdesc d;
        int r = embi_js_obj_get_own_property(rt, o, k, &d);
        if (r == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
            break;
        }
        if (r == EMBI_JS_YES && (d.attrs & ATTR_E)) {
            embi_vec_push(out, k);
        }
    }
    embi_vec_free(&keys);
    return result;
}

int embi_js_set_integrity(embi_js_runtime *rt, embi_js_object *o, bool frozen)
{
    /* A TypedArray that is not fixed-length refuses [[PreventExtensions]] (ta_prevent_extensions),
     * so sealing or freezing it throws below. */
    int pe = embi_js_obj_prevent_extensions(rt, o);
    if (pe == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (pe == EMBI_JS_NO) {
        (void)embi_js_throw_type_error(rt, "Cannot prevent extensions");
        return EMBI_JS_THREW;
    }
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, o, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return EMBI_JS_THREW;
    }
    int result = EMBI_JS_YES;
    for (uint32_t i = 0; i < keys.len && result == EMBI_JS_YES; i++) {
        embi_js_key k = keys.data[i];
        embi_js_pdesc d = {UNDEF, UNDEF, UNDEF, EMBI_JS_PD_CONFIGURABLE, 0};
        if (frozen) {
            embi_js_pdesc cur = {};
            int r = embi_js_obj_get_own_property(rt, o, k, &cur);
            if (r == EMBI_JS_THREW) {
                result = EMBI_JS_THREW;
                break;
            }
            if (r == EMBI_JS_NO) {
                continue;
            }
            if (!embi_js_pd_is_accessor(&cur)) {
                d.has |= EMBI_JS_PD_WRITABLE;
            }
        }
        if (embi_js_define_property_or_throw(rt, o, k, &d) == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
        }
    }
    embi_vec_free(&keys);
    return result;
}

int embi_js_test_integrity(embi_js_runtime *rt, embi_js_object *o, bool frozen)
{
    int ext = embi_js_obj_extensible(rt, o);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (ext == EMBI_JS_YES) {
        return EMBI_JS_NO;
    }
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, o, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return EMBI_JS_THREW;
    }
    int result = EMBI_JS_YES;
    for (uint32_t i = 0; i < keys.len; i++) {
        embi_js_pdesc d;
        int r = embi_js_obj_get_own_property(rt, o, keys.data[i], &d);
        if (r == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
            break;
        }
        if (r == EMBI_JS_NO) {
            continue;
        }
        if (d.attrs & ATTR_C) {
            result = EMBI_JS_NO;
            break;
        }
        if (frozen && !embi_js_pd_is_accessor(&d) && (d.attrs & ATTR_W)) {
            result = EMBI_JS_NO;
            break;
        }
    }
    embi_vec_free(&keys);
    return result;
}

/* ------------------------------------------------------------------ property descriptors */

/* Reads one optional field of a descriptor object. Returns EMBI_JS_THREW on error. */
static int read_field(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, bool *present,
                      embi_js_value *out)
{
    int r = embi_js_obj_has_property(rt, o, k);
    if (r != EMBI_JS_YES) {
        *present = false;
        return r;
    }
    embi_js_value v = embi_js_get(rt, o, k);
    if (embi_js_is_exception(v)) {
        return EMBI_JS_THREW;
    }
    *present = true;
    *out = v;
    return EMBI_JS_YES;
}

bool embi_js_to_property_descriptor(embi_js_runtime *rt, embi_js_value v, embi_js_pdesc *out)
{
    if (!embi_js_is_object(v)) {
        (void)embi_js_throw_type_error(rt, "Property description must be an object");
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    *out = (embi_js_pdesc){UNDEF, UNDEF, UNDEF, 0, 0};
    bool present = false;
    embi_js_value f = UNDEF;

    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_ENUMERABLE), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        out->has |= EMBI_JS_PD_ENUMERABLE;
        if (embi_js_to_boolean(f)) {
            out->attrs |= ATTR_E;
        }
    }
    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_CONFIGURABLE), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        out->has |= EMBI_JS_PD_CONFIGURABLE;
        if (embi_js_to_boolean(f)) {
            out->attrs |= ATTR_C;
        }
    }
    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_VALUE), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        out->has |= EMBI_JS_PD_VALUE;
        out->value = f;
    }
    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_WRITABLE), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        out->has |= EMBI_JS_PD_WRITABLE;
        if (embi_js_to_boolean(f)) {
            out->attrs |= ATTR_W;
        }
    }
    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_GET), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        if (!embi_js_is_undefined(f) && !embi_js_is_callable(f)) {
            (void)embi_js_throw_type_error(rt, "Getter must be a function");
            return false;
        }
        out->has |= EMBI_JS_PD_GET;
        out->get = f;
    }
    if (read_field(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_SET), &present, &f) == EMBI_JS_THREW) {
        return false;
    }
    if (present) {
        if (!embi_js_is_undefined(f) && !embi_js_is_callable(f)) {
            (void)embi_js_throw_type_error(rt, "Setter must be a function");
            return false;
        }
        out->has |= EMBI_JS_PD_SET;
        out->set = f;
    }
    if (embi_js_pd_is_accessor(out) && embi_js_pd_is_data(out)) {
        (void)embi_js_throw_type_error(rt, "Invalid property descriptor. Cannot both specify "
                                           "accessors and a value or writable attribute");
        return false;
    }
    return true;
}

embi_js_value embi_js_from_property_descriptor(embi_js_runtime *rt, const embi_js_pdesc *d)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    if (d->has & EMBI_JS_PD_VALUE) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_VALUE), d->value) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    if (d->has & EMBI_JS_PD_WRITABLE) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_WRITABLE),
                                         embi_js_from_bool((d->attrs & ATTR_W) != 0)) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    if (d->has & EMBI_JS_PD_GET) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_GET), d->get) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    if (d->has & EMBI_JS_PD_SET) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_SET), d->set) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    if (d->has & EMBI_JS_PD_ENUMERABLE) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_ENUMERABLE),
                                         embi_js_from_bool((d->attrs & ATTR_E) != 0)) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    if (d->has & EMBI_JS_PD_CONFIGURABLE) {
        if (embi_js_create_data_property(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_CONFIGURABLE),
                                         embi_js_from_bool((d->attrs & ATTR_C) != 0)) !=
            EMBI_JS_YES) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ calls and construction */

embi_js_value embi_js_call(embi_js_runtime *rt, embi_js_value f, embi_js_value this_v, int argc,
                           const embi_js_value *argv)
{
    if (!embi_js_is_callable(f)) {
        return embi_js_throw_type_error(rt, "not a function");
    }
    embi_js_object *o = embi_js_as_object(f);
    return o->klass->call(rt, o, this_v, argc, argv);
}

embi_js_value embi_js_construct(embi_js_runtime *rt, embi_js_value f, int argc,
                                const embi_js_value *argv, embi_js_value new_target)
{
    if (!embi_js_is_constructor(f)) {
        return embi_js_throw_type_error(rt, "not a constructor");
    }
    embi_js_object *o = embi_js_as_object(f);
    if (!o->klass->construct) {
        return embi_js_throw_type_error(rt, "not a constructor");
    }
    return o->klass->construct(rt, o, argc, argv, new_target);
}

embi_js_value embi_js_invoke(embi_js_runtime *rt, embi_js_value v, embi_js_key k, int argc,
                             const embi_js_value *argv)
{
    embi_js_value f = embi_js_get_v(rt, v, k);
    if (embi_js_is_exception(f)) {
        return EXC;
    }
    return embi_js_call(rt, f, v, argc, argv);
}

int embi_js_ordinary_has_instance(embi_js_runtime *rt, embi_js_value c, embi_js_value v)
{
    if (!embi_js_is_callable(c)) {
        return EMBI_JS_NO;
    }
    embi_js_value target = UNDEF;
    if (embi_js_bound_target(embi_js_as_object(c), &target)) {
        return embi_js_instance_of(rt, v, target);
    }
    if (!embi_js_is_object(v)) {
        return EMBI_JS_NO;
    }
    embi_js_value p = embi_js_obj_get(rt, embi_js_as_object(c),
                                      embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE), c);
    if (embi_js_is_exception(p)) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_object(p)) {
        (void)embi_js_throw_type_error(rt, "Function has non-object prototype in instanceof check");
        return EMBI_JS_THREW;
    }
    embi_js_object *cur = embi_js_as_object(v);
    for (;;) {
        embi_js_value pv;
        if (embi_js_obj_get_prototype_of(rt, cur, &pv) == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (!embi_js_is_object(pv)) {
            return EMBI_JS_NO;
        }
        if (pv == p) {
            return EMBI_JS_YES;
        }
        cur = embi_js_as_object(pv);
    }
}

int embi_js_instance_of(embi_js_runtime *rt, embi_js_value v, embi_js_value target)
{
    if (!embi_js_is_object(target)) {
        (void)embi_js_throw_type_error(rt, "Right-hand side of 'instanceof' is not an object");
        return EMBI_JS_THREW;
    }
    embi_js_value h = embi_js_get_method(rt, target, embi_js_wk_sym(rt, EMBI_JS_WK_HAS_INSTANCE));
    if (embi_js_is_exception(h)) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_undefined(h)) {
        embi_js_value r = embi_js_call(rt, h, target, 1, &v);
        if (embi_js_is_exception(r)) {
            return EMBI_JS_THREW;
        }
        return embi_js_to_boolean(r) ? EMBI_JS_YES : EMBI_JS_NO;
    }
    if (!embi_js_is_callable(target)) {
        (void)embi_js_throw_type_error(rt, "Right-hand side of 'instanceof' is not callable");
        return EMBI_JS_THREW;
    }
    return embi_js_ordinary_has_instance(rt, target, v);
}

embi_js_value embi_js_proto_from_ctor(embi_js_runtime *rt, embi_js_value new_target,
                                      enum embi_js_proto which)
{
    embi_js_value fallback = rt->protos[which];
    if (embi_js_is_empty(fallback)) {
        fallback = EMBI_JS_NULL;
    }
    if (!embi_js_is_object(new_target)) {
        return fallback;
    }
    embi_js_value p = embi_js_obj_get(rt, embi_js_as_object(new_target),
                                      embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE), new_target);
    if (embi_js_is_exception(p)) {
        return EXC;
    }
    if (embi_js_is_object(p)) {
        return p;
    }
    /* GetFunctionRealm(constructor): a revoked proxy throws (there is one realm per runtime, so
     * nothing else is needed from it). */
    embi_js_value v = new_target;
    while (embi_js_is_object(v) && embi_js_obj_is_proxy(embi_js_as_object(v))) {
        embi_js_value handler;
        if (!embi_js_proxy_parts(embi_js_as_object(v), &v, &handler)) {
            break;
        }
        if (embi_js_is_null(handler)) {
            return embi_js_throw_type_error(rt, "Cannot perform 'GetFunctionRealm' on a proxy that has been revoked");
        }
    }
    return fallback;
}

/* IsArray (ECMA-262 7.2.2): looks through proxies to their targets. A revoked proxy throws. */
int embi_js_is_array_checked(embi_js_runtime *rt, embi_js_value v)
{
    for (;;) {
        if (!embi_js_is_object(v)) {
            return EMBI_JS_NO;
        }
        embi_js_object *o = embi_js_as_object(v);
        if (o->klass == &embi_js_class_array) {
            return EMBI_JS_YES;
        }
        if (!embi_js_obj_is_proxy(o)) {
            return EMBI_JS_NO;
        }
        embi_js_value handler;
        if (!embi_js_proxy_parts(o, &v, &handler)) {
            return EMBI_JS_NO;
        }
        if (embi_js_is_null(handler)) {
            (void)embi_js_throw_type_error(rt, "Cannot perform 'IsArray' on a proxy that has been revoked");
            return EMBI_JS_THREW;
        }
    }
}

/* Non-throwing IsArray: a revoked proxy reads as not an array. */
bool embi_js_is_array(embi_js_value v)
{
    for (;;) {
        if (!embi_js_is_object(v)) {
            return false;
        }
        embi_js_object *o = embi_js_as_object(v);
        if (o->klass == &embi_js_class_array) {
            return true;
        }
        embi_js_value handler;
        if (!embi_js_proxy_parts(o, &v, &handler) || embi_js_is_null(handler)) {
            return false;
        }
    }
}

embi_js_value embi_js_species_constructor(embi_js_runtime *rt, embi_js_object *o,
                                          embi_js_value default_ctor)
{
    embi_js_value c = embi_js_get(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR));
    if (embi_js_is_exception(c)) {
        return EXC;
    }
    if (embi_js_is_undefined(c)) {
        return default_ctor;
    }
    if (!embi_js_is_object(c)) {
        return embi_js_throw_type_error(rt, "object.constructor is not an object");
    }
    embi_js_value s = embi_js_get(rt, embi_js_as_object(c), embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES));
    if (embi_js_is_exception(s)) {
        return EXC;
    }
    if (embi_js_is_nullish(s)) {
        return default_ctor;
    }
    if (embi_js_is_constructor(s)) {
        return s;
    }
    return embi_js_throw_type_error(rt, "object.constructor[Symbol.species] is not a constructor");
}

embi_js_object *embi_js_array_species_create(embi_js_runtime *rt, embi_js_object *original,
                                             double len)
{
    int orig_is_arr = embi_js_is_array_checked(rt, embi_js_from_object(original));
    if (orig_is_arr == EMBI_JS_THREW) {
        return nullptr;
    }
    if (orig_is_arr == EMBI_JS_NO) {
        return embi_js_array_create_default(rt, len);
    }
    /* J5e: an Array that inherits its constructor from the intrinsic Array.prototype, with the
     * ARRAY_SPECIES protector intact, gets C = %Array% and %Array%[@@species] = %Array%, so the
     * species create is ArrayCreate(len) with the intrinsic prototype (no user code runs). */
    if ((rt->protect & EMBI_JS_PROTECT_ARRAY_SPECIES) && original->klass == &embi_js_class_array &&
        original->shape->proto == rt->protos[EMBI_JS_PROTO_ARRAY]) {
        uint32_t own_slot = 0;
        uint8_t own_attrs = 0;
        if (!embi_js_obj_ic_own(original, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR), &own_slot,
                                &own_attrs)) {
            return embi_js_array_create_default(rt, len);
        }
    }
    embi_js_value c = embi_js_get(rt, original, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR));
    if (embi_js_is_exception(c)) {
        return nullptr;
    }
    if (embi_js_is_object(c)) {
        c = embi_js_get(rt, embi_js_as_object(c), embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES));
        if (embi_js_is_exception(c)) {
            return nullptr;
        }
        if (embi_js_is_null(c)) {
            c = UNDEF;
        }
    }
    if (embi_js_is_undefined(c)) {
        return embi_js_array_create_default(rt, len);
    }
    if (!embi_js_is_constructor(c)) {
        (void)embi_js_throw_type_error(rt, "species is not a constructor");
        return nullptr;
    }
    embi_js_value arg = embi_js_number(len);
    embi_js_value r = embi_js_construct(rt, c, 1, &arg, c);
    if (embi_js_is_exception(r)) {
        return nullptr;
    }
    return embi_js_as_object(r);
}

/* ------------------------------------------------------------------ symbols (J2e) */

int embi_js_with_has_binding(embi_js_runtime *rt, embi_js_object *obj, embi_js_key k)
{
    int r = embi_js_obj_has_property(rt, obj, k);
    if (r != EMBI_JS_YES) {
        return r;
    }
    embi_js_value uns = embi_js_get(rt, obj, embi_js_wk_sym(rt, EMBI_JS_WK_UNSCOPABLES));
    if (uns == EXC) {
        return EMBI_JS_THREW;
    }
    if (embi_js_is_object(uns)) {
        embi_js_value blocked = embi_js_obj_get(rt, embi_js_as_object(uns), k, uns);
        if (blocked == EXC) {
            return EMBI_JS_THREW;
        }
        if (embi_js_to_boolean(blocked)) {
            return EMBI_JS_NO;
        }
    }
    return EMBI_JS_YES;
}
