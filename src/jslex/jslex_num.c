/*
 * Correctly rounded numeric conversion for the lexer.
 *
 * Decimal literals: digits and a power of ten form the exact rational num/den. The quotient of
 * num*2^s/den is computed with 56 shift-and-subtract steps, which leaves at least 55 bits and a
 * sticky remainder. Rounding to 53 bits (or fewer for subnormals) then uses round-half-even.
 * Literals whose binary value fits in an int64 with at most 15 digits and |exponent| <= 22 take
 * the exact fast path (Clinger), where a single IEEE operation rounds once.
 */
#include "jslex/jslex.h"
#include "jslex/jslex_num.h"

static const double pow10_tab[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static const uint32_t pow10_u32[9] = {1u,      10u,      100u,      1000u,      10000u,
                                      100000u, 1000000u, 10000000u, 100000000u};

static void big_norm(embi_jsbig *b)
{
    while (b->n != 0 && b->w[b->n - 1] == 0) {
        b->n--;
    }
}

/* b = b * m + add. Callers keep the result within EMBI_JSNUM_LIMBS. */
static void big_mul_add(embi_jsbig *b, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    for (uint32_t i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry != 0) {
        if (b->n >= EMBI_JSNUM_LIMBS) {
            __builtin_trap();
        }
        b->w[b->n++] = (uint32_t)carry;
    }
    big_norm(b);
}

static void big_shl(embi_jsbig *b, uint32_t bits)
{
    if (b->n == 0 || bits == 0) {
        return;
    }
    uint32_t limbs = bits / 32;
    uint32_t r = bits % 32;
    if (b->n + limbs + 1 > EMBI_JSNUM_LIMBS) {
        __builtin_trap();
    }
    if (r != 0) {
        uint32_t carry = 0;
        for (uint32_t i = 0; i < b->n; i++) {
            uint32_t v = b->w[i];
            b->w[i] = (v << r) | carry;
            carry = v >> (32 - r);
        }
        if (carry != 0) {
            b->w[b->n++] = carry;
        }
    }
    if (limbs != 0) {
        for (uint32_t i = b->n; i-- > 0;) {
            b->w[i + limbs] = b->w[i];
        }
        for (uint32_t i = 0; i < limbs; i++) {
            b->w[i] = 0;
        }
        b->n += limbs;
    }
}

static int big_cmp(const embi_jsbig *a, const embi_jsbig *b)
{
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (uint32_t i = a->n; i-- > 0;) {
        if (a->w[i] != b->w[i]) {
            return a->w[i] < b->w[i] ? -1 : 1;
        }
    }
    return 0;
}

/* a -= b, requires a >= b. */
static void big_sub(embi_jsbig *a, const embi_jsbig *b)
{
    int64_t borrow = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - borrow - (i < b->n ? (int64_t)b->w[i] : 0);
        if (t < 0) {
            t += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->w[i] = (uint32_t)t;
    }
    big_norm(a);
}

static uint32_t big_bitlen(const embi_jsbig *b)
{
    if (b->n == 0) {
        return 0;
    }
    return (b->n - 1) * 32 + (32 - (uint32_t)__builtin_clz(b->w[b->n - 1]));
}

/* 2^k as a double, for k in [-1022, 1023]. */
static double pow2d(int k)
{
    union {
        uint64_t u;
        double d;
    } x;
    x.u = (uint64_t)(k + 1023) << 52;
    return x.d;
}

/* m * 2^k with a single rounding. m is an exact integer up to 2^53. */
static double scale2(double m, int64_t k)
{
    while (k > 1000) {
        m *= pow2d(1000);
        k -= 1000;
    }
    while (k < -1000) {
        m *= pow2d(-1000);
        k += 1000;
    }
    return m * pow2d((int)k);
}

/* Correctly rounded num/den. Both are consumed. */
static double big_ratio_to_double(embi_jsbig *num, embi_jsbig *den)
{
    if (num->n == 0) {
        return 0.0;
    }
    int64_t t = (int64_t)big_bitlen(num) - (int64_t)big_bitlen(den);
    int64_t s = 55 - t;
    if (s >= 0) {
        big_shl(num, (uint32_t)s);
    } else {
        big_shl(den, (uint32_t)-s);
    }

    /* q = floor(num / den) lies in [2^54, 2^56). */
    uint64_t q = 0;
    for (int k = 56; k >= 0; k--) {
        embi_jsbig tmp = *den;
        big_shl(&tmp, (uint32_t)k);
        if (big_cmp(num, &tmp) >= 0) {
            big_sub(num, &tmp);
            q |= (uint64_t)1 << k;
        }
    }
    bool sticky = num->n != 0;

    int64_t len = 0;
    for (uint64_t x = q; x != 0; x >>= 1) {
        len++;
    }
    /* The value is q * 2^-s plus a fraction, so its leading bit has exponent e. */
    int64_t e = len - 1 - s;
    if (e >= 1024) {
        return __builtin_inf();
    }
    if (e < -1075) {
        return 0.0;
    }
    /* Bits kept: 53 for normals, fewer below 2^-1022. */
    int64_t keep = e >= -1022 ? 53 : 1075 + e;
    int64_t drop = len - keep;
    uint64_t m = q >> drop;
    bool guard = ((q >> (drop - 1)) & 1) != 0;
    bool low = (q & ((((uint64_t)1) << (drop - 1)) - 1)) != 0 || sticky;
    if (guard && (low || (m & 1) != 0)) {
        m++;
    }
    return scale2((double)m, drop - s);
}

void embi_jsnum_init(embi_jsnum *a, unsigned radix)
{
    a->radix = radix;
    a->seen_point = false;
    a->overflow = false;
    a->dropped_nonzero = false;
    a->nd = 0;
    a->dropped = 0;
    a->frac = 0;
    a->big.n = 0;
}

void embi_jsnum_digit(embi_jsnum *a, unsigned d)
{
    if (a->radix != 10) {
        if (!a->overflow) {
            big_mul_add(&a->big, a->radix, d);
            if (a->big.n > 35) {
                a->overflow = true;
            }
        }
        return;
    }
    if (a->seen_point) {
        a->frac++;
    }
    if (a->nd == 0 && d == 0) {
        return;
    }
    if (a->nd < EMBI_JSNUM_MAX_DIGITS) {
        a->d[a->nd++] = (uint8_t)d;
    } else {
        a->dropped++;
        if (d != 0) {
            a->dropped_nonzero = true;
        }
    }
}

void embi_jsnum_point(embi_jsnum *a)
{
    a->seen_point = true;
}

double embi_jsnum_finish(embi_jsnum *a, int64_t exp10)
{
    if (a->radix != 10) {
        if (a->overflow) {
            return __builtin_inf();
        }
        embi_jsbig one;
        one.n = 1;
        one.w[0] = 1;
        return big_ratio_to_double(&a->big, &one);
    }
    if (a->nd == 0) {
        return 0.0;
    }
    int64_t e = exp10 - (int64_t)a->frac + (int64_t)a->dropped;
    int64_t est = (int64_t)a->nd + e; /* position of the leading digit, roughly */
    if (est > 310) {
        return __builtin_inf();
    }
    if (est < -330) {
        return 0.0;
    }
    uint32_t nd = a->nd;
    if (a->dropped_nonzero) {
        /* Sticky digit: the true value lies strictly between D and D+1 at this scale, and
         * no rounding boundary (at most 767 significant digits) falls in that gap. */
        a->d[nd++] = 1;
        e -= 1;
    }

    if (nd <= 15 && e >= -22 && e <= 22) {
        uint64_t m = 0;
        for (uint32_t i = 0; i < nd; i++) {
            m = m * 10 + a->d[i];
        }
        double md = (double)m;
        return e >= 0 ? md * pow10_tab[e] : md / pow10_tab[-e];
    }

    embi_jsbig num;
    embi_jsbig den;
    num.n = 0;
    den.n = 1;
    den.w[0] = 1;
    for (uint32_t i = 0; i < nd; i++) {
        big_mul_add(&num, 10, a->d[i]);
    }
    if (e >= 0) {
        int64_t k = e;
        while (k >= 9) {
            big_mul_add(&num, 1000000000u, 0);
            k -= 9;
        }
        if (k > 0) {
            big_mul_add(&num, pow10_u32[k], 0);
        }
    } else {
        int64_t k = -e;
        while (k >= 9) {
            big_mul_add(&den, 1000000000u, 0);
            k -= 9;
        }
        if (k > 0) {
            big_mul_add(&den, pow10_u32[k], 0);
        }
    }
    return big_ratio_to_double(&num, &den);
}

double embi_jslex_decimal_to_double(const char *s, uint32_t len)
{
    embi_jsnum a;
    embi_jsnum_init(&a, 10);
    uint32_t i = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            embi_jsnum_digit(&a, (unsigned)(c - '0'));
        } else if (c == '.') {
            embi_jsnum_point(&a);
        } else {
            break;
        }
    }
    int64_t ev = 0;
    bool neg = false;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            neg = s[i] == '-';
            i++;
        }
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
            if (ev < 100000000000LL) {
                ev = ev * 10 + (s[i] - '0');
            }
        }
    }
    return embi_jsnum_finish(&a, neg ? -ev : ev);
}

double embi_jslex_radix_to_double(const char *s, uint32_t len, unsigned radix)
{
    embi_jsnum a;
    embi_jsnum_init(&a, radix);
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A') + 10u;
        } else {
            continue;
        }
        if (d < radix) {
            embi_jsnum_digit(&a, d);
        }
    }
    return embi_jsnum_finish(&a, 0);
}
