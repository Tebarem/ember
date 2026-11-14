/*
 * Math: the global Math object with its constants and functions, and @@toStringTag.
 *
 * Exact where ECMA-262 requires exactness: round, floor, ceil, trunc, sign, abs, min and max
 * (with -0 and NaN), fround, clz32 and imul are computed directly, which gives the correctly
 * rounded result. The other functions use SDL's math functions where SDL has them (sin, cos, tan,
 * asin, acos, atan, atan2, exp, log, log10, sqrt, pow, floor, ceil, trunc, fabs) and the C
 * library for the rest (sinh, cosh, tanh, asinh, acosh, atanh, cbrt, expm1, log1p, log2), which
 * SDL does not provide.
 *
 * Math.pow follows Number::exponentiate: pow(x, NaN) is NaN, and pow(+-1, +-Infinity) is NaN
 * (the C pow gives 1 there).
 */
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE

enum math_unary_op {
    M_ABS,
    M_ACOS,
    M_ACOSH,
    M_ASIN,
    M_ASINH,
    M_ATAN,
    M_ATANH,
    M_CBRT,
    M_CEIL,
    M_COS,
    M_COSH,
    M_EXP,
    M_EXPM1,
    M_F16ROUND,
    M_FLOOR,
    M_FROUND,
    M_LOG,
    M_LOG1P,
    M_LOG10,
    M_LOG2,
    M_ROUND,
    M_SIGN,
    M_SIN,
    M_SINH,
    M_SQRT,
    M_TAN,
    M_TANH,
    M_TRUNC,
};

static bool is_finite(double d)
{
    return d - d == 0.0;
}

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

/* Math.round: ties go toward +Infinity, and results in [-0.5, -0] are -0. x - floor(x) is exact
 * for every double, so the comparison with 0.5 is exact too. */
static double js_round(double x)
{
    if (!is_finite(x) || x == 0.0) {
        return x;
    }
    double r = SDL_floor(x);
    if (x - r >= 0.5) {
        r += 1.0;
    }
    if (r == 0.0 && (x < 0.0 || signbit(x))) {
        return -0.0;
    }
    return r;
}

/* Math.f16round: rounds to the nearest IEEE binary16 value, ties to even. Values from 2^15 *
 * (2 - 2^-11) up round to infinity. The step is 2^(e - 10) for a normal result with exponent e,
 * and 2^-24 in the subnormal range. */
static double js_f16round(double x)
{
    if (x != x || x == 0.0 || !is_finite(x)) {
        return x;
    }
    double a = fabs(x);
    if (a >= 65520.0) {
        return x < 0.0 ? -INFINITY : INFINITY;
    }
    int k;
    frexp(a, &k); /* a = m * 2^k with m in [0.5, 1), so floor(log2(a)) = k - 1 */
    int e = k - 1;
    if (e < -14) {
        e = -14;
    }
    double q = ldexp(1.0, e - 10);
    double v = a / q; /* exact: a power of two */
    double r = floor(v);
    double d = v - r;  /* exact fraction */
    if (d > 0.5 || (d == 0.5 && fmod(r, 2.0) != 0.0)) {
        r += 1.0;
    }
    double res = r * q;
    return x < 0.0 ? -res : res;
}

/* Math.fround: rounds to the nearest binary32 value, ties to even. Magnitudes from FLT_MAX up to
 * 2^128 - 2^103 round to FLT_MAX, and the halfway point and above round to infinity. The guard
 * keeps the cast from converting an out-of-range double, which is undefined in C. */
static double js_fround(double x)
{
    if (fabs(x) >= 3.4028235677973366e38) {
        return copysign(INFINITY, x);
    }
    return (double)(float)x;
}

static double js_sign(double x)
{
    if (x != x || x == 0.0) {
        return x;
    }
    return x > 0.0 ? 1.0 : -1.0;
}

/* Number::exponentiate: unlike C pow, a NaN exponent gives NaN, and so does a base of +-1 with
 * an infinite exponent. */
static double js_pow(double base, double e)
{
    if (e != e) {
        return NAN;
    }
    if (e == 0.0) {
        return 1.0;
    }
    if ((base == 1.0 || base == -1.0) && !is_finite(e)) {
        return NAN;
    }
    return SDL_pow(base, e);
}

static double math_unary(int op, double x)
{
    switch (op) {
    case M_ABS:
        return SDL_fabs(x);
    case M_ACOS:
        return SDL_acos(x);
    case M_ACOSH:
        return acosh(x);
    case M_ASIN:
        return SDL_asin(x);
    case M_ASINH:
        return asinh(x);
    case M_ATAN:
        return SDL_atan(x);
    case M_ATANH:
        return atanh(x);
    case M_CBRT:
        return cbrt(x);
    case M_CEIL:
        return SDL_ceil(x);
    case M_COS:
        return SDL_cos(x);
    case M_COSH:
        return cosh(x);
    case M_EXP:
        return SDL_exp(x);
    case M_EXPM1:
        return expm1(x);
    case M_F16ROUND:
        return js_f16round(x);
    case M_FLOOR:
        return SDL_floor(x);
    case M_FROUND:
        return js_fround(x);
    case M_LOG:
        return SDL_log(x);
    case M_LOG1P:
        return log1p(x);
    case M_LOG10:
        return SDL_log10(x);
    case M_LOG2:
        return log2(x);
    case M_ROUND:
        return js_round(x);
    case M_SIGN:
        return js_sign(x);
    case M_SIN:
        return SDL_sin(x);
    case M_SINH:
        return sinh(x);
    case M_SQRT:
        return SDL_sqrt(x);
    case M_TAN:
        return SDL_tan(x);
    case M_TANH:
        return tanh(x);
    case M_TRUNC:
        return SDL_trunc(x);
    default:
        return x;
    }
}

/* The one-argument functions (magic is the operation). */
static embi_js_value math_unary_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    double x;
    if (!embi_js_to_number(r->rt, arg_at(argc, argv, 0), &x)) {
        return EXC;
    }
    return embi_js_number(math_unary(magic, x));
}

/* Leading zero bits of u, by binary search (no compiler builtin, which the toolchain need not
 * provide). Returns 32 for zero. */
static uint32_t count_leading_zeros(uint32_t u)
{
    if (u == 0) {
        return 32;
    }
    uint32_t n = 0;
    if (!(u & 0xFFFF0000u)) {
        n += 16;
        u <<= 16;
    }
    if (!(u & 0xFF000000u)) {
        n += 8;
        u <<= 8;
    }
    if (!(u & 0xF0000000u)) {
        n += 4;
        u <<= 4;
    }
    if (!(u & 0xC0000000u)) {
        n += 2;
        u <<= 2;
    }
    if (!(u & 0x80000000u)) {
        n += 1;
    }
    return n;
}

static embi_js_value math_clz32(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    uint32_t u;
    if (!embi_js_to_uint32(r->rt, arg_at(argc, argv, 0), &u)) {
        return EXC;
    }
    uint32_t n = count_leading_zeros(u);
    return embi_js_from_int32((int32_t)n);
}

static embi_js_value math_imul(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    uint32_t a;
    uint32_t b;
    if (!embi_js_to_uint32(rt, arg_at(argc, argv, 0), &a)) {
        return EXC;
    }
    if (!embi_js_to_uint32(rt, arg_at(argc, argv, 1), &b)) {
        return EXC;
    }
    return embi_js_from_int32((int32_t)(a * b));
}

static embi_js_value math_atan2(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double y;
    double x;
    if (!embi_js_to_number(rt, arg_at(argc, argv, 0), &y)) {
        return EXC;
    }
    if (!embi_js_to_number(rt, arg_at(argc, argv, 1), &x)) {
        return EXC;
    }
    return embi_js_number(SDL_atan2(y, x));
}

static embi_js_value math_pow(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double b;
    double e;
    if (!embi_js_to_number(rt, arg_at(argc, argv, 0), &b)) {
        return EXC;
    }
    if (!embi_js_to_number(rt, arg_at(argc, argv, 1), &e)) {
        return EXC;
    }
    return embi_js_number(js_pow(b, e));
}

/* Coerces every argument to a number before anything else happens (ECMA-262 requires all of
 * them to be converted even when one is NaN). Returns a C-heap array that the caller frees with
 * embi_free(buf, (argc + 1) * sizeof *buf), or nullptr when a conversion throws. */
static double *to_number_list(embi_js_runtime *rt, int argc, const embi_js_value *argv)
{
    double *vals = embi_alloc((size_t)(argc + 1) * sizeof *vals);
    for (int i = 0; i < argc; i++) {
        if (!embi_js_to_number(rt, argv[i], &vals[i])) {
            embi_free(vals, (size_t)(argc + 1) * sizeof *vals);
            return nullptr;
        }
    }
    return vals;
}

static embi_js_value math_max_min(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    double *vals = to_number_list(rt, argc, argv);
    if (!vals) {
        return EXC;
    }
    bool is_max = magic == 0;
    double res = is_max ? -INFINITY : INFINITY;
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        double x = vals[i];
        if (x != x) {
            nan = true;
            continue;
        }
        if (is_max) {
            /* +0 beats -0 for max, and -0 beats +0 for min. */
            if (x > res || (x == 0.0 && res == 0.0 && !signbit(x))) {
                res = x;
            }
        } else {
            if (x < res || (x == 0.0 && res == 0.0 && signbit(x))) {
                res = x;
            }
        }
    }
    embi_free(vals, (size_t)(argc + 1) * sizeof *vals);
    return embi_js_number(nan ? NAN : res);
}

/* ------------------------------------------------------------------ Math.sumPrecise (ES2026)
 *
 * The sum is exact: every finite double is an integer multiple of 2^-1074, so the sum of any
 * number of them is an integer multiple of 2^-1074 and fits in a fixed-size big integer (units of
 * 2^-1074, 2098 bits for the largest double, plus room for the count). Positive and negative parts
 * accumulate separately and are subtracted at the end. The result is rounded once, to nearest with
 * ties to even, so overflow turns into an infinity exactly as the rounded value does. */
#define SP_LIMBS 40u /* 2560 bits: 2098 for the magnitude range, the rest for the carries */

typedef struct sp_big {
    uint64_t w[SP_LIMBS];
} sp_big;

static void sp_add_limb(sp_big *a, uint32_t i, uint64_t v)
{
    while (v != 0 && i < SP_LIMBS) {
        uint64_t s = a->w[i] + v;
        v = s < v ? 1u : 0u;
        a->w[i] = s;
        i++;
    }
}

/* Adds m * 2^shift (in units of 2^-1074) to a. m is at most 53 bits. */
static void sp_add_shifted(sp_big *a, uint64_t m, uint32_t shift)
{
    uint32_t li = shift / 64u;
    uint32_t bi = shift % 64u;
    sp_add_limb(a, li, m << bi);
    if (bi != 0) {
        sp_add_limb(a, li + 1u, m >> (64u - bi));
    }
}

/* a -= b, for a >= b. */
static void sp_sub(sp_big *a, const sp_big *b)
{
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < SP_LIMBS; i++) {
        uint64_t x = a->w[i];
        uint64_t y = b->w[i];
        uint64_t d = x - y;
        uint64_t b1 = x < y ? 1u : 0u;
        uint64_t d2 = d - borrow;
        uint64_t b2 = d < borrow ? 1u : 0u;
        a->w[i] = d2;
        borrow = b1 | b2;
    }
}

static int sp_cmp(const sp_big *a, const sp_big *b)
{
    for (uint32_t i = SP_LIMBS; i-- > 0;) {
        if (a->w[i] != b->w[i]) {
            return a->w[i] < b->w[i] ? -1 : 1;
        }
    }
    return 0;
}

static bool sp_bit(const sp_big *a, uint32_t i)
{
    return ((a->w[i / 64u] >> (i % 64u)) & 1u) != 0;
}

/* Adds a finite double to the accumulator for its sign. */
static void sp_add_double(sp_big *a, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    uint64_t e = (bits >> 52) & 0x7FFu;
    uint64_t f = bits & ((1ull << 52) - 1u);
    if (e == 0) {
        sp_add_shifted(a, f, 0);
    } else {
        sp_add_shifted(a, f | (1ull << 52), (uint32_t)(e - 1u));
    }
}

/* The magnitude a (units of 2^-1074) correctly rounded to a double. a is not zero. */
static double sp_round(const sp_big *a)
{
    uint32_t top = 0;
    for (uint32_t i = SP_LIMBS; i-- > 0;) {
        if (a->w[i] != 0) {
            top = i * 64u + (63u - (uint32_t)__builtin_clzll(a->w[i]));
            break;
        }
    }
    if (top < 53u) {
        /* Fits in 53 bits: exact, and every such multiple of 2^-1074 is a double. */
        return ldexp((double)a->w[0], -1074);
    }
    uint32_t q = top - 52u; /* the lowest bit kept */
    uint64_t mant = 0;
    for (uint32_t j = top + 1u; j-- > q;) {
        mant = (mant << 1) | (sp_bit(a, j) ? 1u : 0u);
    }
    bool round = sp_bit(a, q - 1u);
    bool sticky = false;
    for (uint32_t j = 0; j + 1u < q && !sticky; j++) {
        sticky = sp_bit(a, j);
    }
    if (round && (sticky || (mant & 1u))) {
        mant++;
        if (mant == (1ull << 53)) {
            mant >>= 1;
            q++;
        }
    }
    return ldexp((double)mant, (int)q - 1074);
}

/* Math.sumPrecise(items) (ES2026). Every item must be a Number; a value that is not closes the
 * iterator and throws a TypeError. */
static embi_js_value math_sum_precise(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value rec = embi_js_ir_open(rt, arg_at(argc, argv, 0));
    if (rec == EXC) {
        return EXC;
    }
    sp_big *pos = embi_alloc(sizeof *pos);
    sp_big *neg = embi_alloc(sizeof *neg);
    memset(pos, 0, sizeof *pos);
    memset(neg, 0, sizeof *neg);
    bool nan = false;
    bool pinf = false;
    bool ninf = false;
    bool nonnegzero = false; /* a value other than -0 was seen: an exact zero sum is then +0 */
    embi_js_value result = UNDEF;
    for (;;) {
        embi_js_value v = UNDEF;
        int c = embi_js_ir_next(rt, rec, &v);
        if (c < 0) {
            result = EXC;
            break;
        }
        if (c == 0) {
            break;
        }
        if (!embi_js_is_number(v)) {
            embi_js_ir_close_throw(rt, rec);
            (void)embi_js_throw_type_error(rt, "Math.sumPrecise: every item must be a Number");
            result = EXC;
            break;
        }
        double d = embi_js_as_double(v);
        if (d != d) {
            nan = true;
        } else if (d == INFINITY) {
            pinf = true;
        } else if (d == -INFINITY) {
            ninf = true;
        } else {
            if (!(d == 0.0 && signbit(d))) {
                nonnegzero = true;
            }
            if (d < 0.0) {
                sp_add_double(neg, -d);
            } else {
                sp_add_double(pos, d);
            }
        }
    }
    if (result == UNDEF) {
        if (nan || (pinf && ninf)) {
            result = embi_js_number(NAN);
        } else if (pinf) {
            result = embi_js_number(INFINITY);
        } else if (ninf) {
            result = embi_js_number(-INFINITY);
        } else {
            int cmp = sp_cmp(pos, neg);
            if (cmp == 0) {
                result = embi_js_number(nonnegzero ? 0.0 : -0.0);
            } else {
                sp_big *mag = embi_alloc(sizeof *mag);
                if (cmp > 0) {
                    *mag = *pos;
                    sp_sub(mag, neg);
                } else {
                    *mag = *neg;
                    sp_sub(mag, pos);
                }
                double x = sp_round(mag);
                embi_free(mag, sizeof *mag);
                result = embi_js_number(cmp > 0 ? x : -x);
            }
        }
    }
    embi_free(pos, sizeof *pos);
    embi_free(neg, sizeof *neg);
    return result;
}

static embi_js_value math_hypot(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double *vals = to_number_list(rt, argc, argv);
    if (!vals) {
        return EXC;
    }
    bool inf = false;
    bool nan = false;
    double big = 0.0;
    for (int i = 0; i < argc; i++) {
        double x = vals[i];
        if (!is_finite(x)) {
            if (x != x) {
                nan = true;
            } else {
                inf = true;
            }
        } else if (fabs(x) > big) {
            big = fabs(x);
        }
    }
    double res;
    if (inf) {
        res = INFINITY;
    } else if (nan) {
        res = NAN;
    } else if (big == 0.0) {
        res = 0.0;
    } else {
        /* Scaled sum of squares with Kahan compensation; the largest term is exactly 1. */
        double sum = 0.0;
        double comp = 0.0;
        for (int i = 0; i < argc; i++) {
            double t = vals[i] / big;
            double y = t * t - comp;
            double s = sum + y;
            comp = (s - sum) - y;
            sum = s;
        }
        res = SDL_sqrt(sum) * big;
    }
    embi_free(vals, (size_t)(argc + 1) * sizeof *vals);
    return embi_js_number(res);
}

/* Math.random state. It is per thread (C23 thread_local), so runtimes on different threads never
 * share a generator and need no lock. Each thread seeds itself on first use from the performance
 * counter mixed with the address of its own state, so two threads that start together still get
 * different sequences. The generator is splitmix64, which has a 2^64 period. */
static thread_local uint64_t rng_state;
static thread_local bool rng_seeded;

static uint64_t rng_next(void)
{
    if (!rng_seeded) {
        uint64_t seed = (uint64_t)SDL_GetPerformanceCounter() ^ (uint64_t)(uintptr_t)&rng_state;
        rng_state = seed;
        rng_seeded = true;
    }
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static embi_js_value math_random(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    /* 53 random bits scaled into [0, 1). */
    uint64_t bits = rng_next() & ((1ull << 53) - 1);
    return embi_js_number((double)bits * (1.0 / 9007199254740992.0));
}

/* ------------------------------------------------------------------ installation */

struct unary_def {
    const char *name;
    int op;
};

static const struct unary_def unary_fns[] = {
    {"abs", M_ABS},     {"acos", M_ACOS},   {"acosh", M_ACOSH}, {"asin", M_ASIN},
    {"asinh", M_ASINH}, {"atan", M_ATAN},   {"atanh", M_ATANH}, {"cbrt", M_CBRT},
    {"ceil", M_CEIL},   {"cos", M_COS},     {"cosh", M_COSH},   {"exp", M_EXP},
    {"expm1", M_EXPM1}, {"f16round", M_F16ROUND}, {"floor", M_FLOOR}, {"fround", M_FROUND},
    {"log", M_LOG},
    {"log1p", M_LOG1P}, {"log10", M_LOG10}, {"log2", M_LOG2},   {"round", M_ROUND},
    {"sign", M_SIGN},   {"sin", M_SIN},     {"sinh", M_SINH},   {"sqrt", M_SQRT},
    {"tan", M_TAN},     {"tanh", M_TANH},   {"trunc", M_TRUNC},
};

struct const_def {
    const char *name;
    double value;
};

static const struct const_def math_consts[] = {
    {"E", 2.718281828459045},
    {"LN10", 2.302585092994046},
    {"LN2", 0.6931471805599453},
    {"LOG10E", 0.4342944819032518},
    {"LOG2E", 1.4426950408889634},
    {"PI", 3.141592653589793},
    {"SQRT1_2", 0.7071067811865476},
    {"SQRT2", 1.4142135623730951},
};

void embi_js_init_builtins_math(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *m = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Math"), embi_js_from_object(m),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);

    for (size_t i = 0; i < sizeof math_consts / sizeof math_consts[0]; i++) {
        embi_js_define_raw(rt, m, embi_js_key_c(rt, math_consts[i].name),
                           embi_js_number(math_consts[i].value), 0);
    }
    for (size_t i = 0; i < sizeof unary_fns / sizeof unary_fns[0]; i++) {
        embi_js_define_method(r, m, unary_fns[i].name, math_unary_fn, 1, unary_fns[i].op);
    }
    embi_js_define_method(r, m, "atan2", math_atan2, 2, 0);
    embi_js_define_method(r, m, "clz32", math_clz32, 1, 0);
    embi_js_define_method(r, m, "hypot", math_hypot, 2, 0);
    embi_js_define_method(r, m, "imul", math_imul, 2, 0);
    embi_js_define_method(r, m, "max", math_max_min, 2, 0);
    embi_js_define_method(r, m, "min", math_max_min, 2, 1);
    embi_js_define_method(r, m, "pow", math_pow, 2, 0);
    embi_js_define_method(r, m, "random", math_random, 0, 0);
    embi_js_define_method(r, m, "sumPrecise", math_sum_precise, 1, 0);

    embi_js_value tag = embi_js_from_string(embi_js_atom_intern_utf8(rt, "Math", 4));
    embi_js_define_raw(rt, m, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), tag, ATTR_C);
}
