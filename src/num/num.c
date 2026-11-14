#include "num/num.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/*
 * Design notes.
 *  - Shortest digits: Burger and Dybvig's free-format algorithm on a small fixed-capacity
 *    bignum, with round-half-even boundary handling and the spec's "closest, then even" tie
 *    rule. This is exact but slower than Ryu or Schubfach; it is the reference path.
 *  - Fixed/exponential/precision: the exact decimal expansion of the double (x = D * 10^P, D
 *    with no trailing zeros) is rounded half-up as a string operation.
 *  - Parsing: a Clinger fast path for short inputs, otherwise an exact bignum quotient with
 *    a sticky bit, rounded half-even to 53 bits (or fewer for subnormals).
 *  - Radix output mirrors V8's DoubleToRadixCString so that results match engines.
 */

/* ------------------------------------------------------------------ bit helpers */

#define DBL_SIGN_BIT 0x8000000000000000ull
#define DBL_EXP_MASK 0x7FF0000000000000ull
#define DBL_MANT_MASK 0x000FFFFFFFFFFFFFull
#define DBL_HIDDEN 0x0010000000000000ull

static uint64_t dbl_to_bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static double bits_to_dbl(uint64_t b)
{
    double v;
    memcpy(&v, &b, sizeof v);
    return v;
}

static bool dbl_is_nan(double v)
{
    return (dbl_to_bits(v) & ~DBL_SIGN_BIT) > DBL_EXP_MASK;
}

static bool dbl_is_inf(double v)
{
    return (dbl_to_bits(v) & ~DBL_SIGN_BIT) == DBL_EXP_MASK;
}

static double dbl_inf(bool neg)
{
    return bits_to_dbl((neg ? DBL_SIGN_BIT : 0) | DBL_EXP_MASK);
}

/* For finite positive x: x == f * 2^e, with f < 2^53. Subnormals use e = -1074. */
static void dbl_decompose(double x, uint64_t *f, int *e)
{
    uint64_t b = dbl_to_bits(x) & ~DBL_SIGN_BIT;
    uint32_t biased = (uint32_t)(b >> 52);
    uint64_t mant = b & DBL_MANT_MASK;
    if (biased == 0) {
        *f = mant;
        *e = -1074;
    } else {
        *f = mant | DBL_HIDDEN;
        *e = (int)biased - 1075;
    }
}

/* ------------------------------------------------------------------ fixed-capacity bignum */

/* 128 limbs = 4096 bits. The largest value handled is about 3.8 Kbit (decimal parsing of a
 * 801-digit mantissa against 10^1131, shifted for the quotient). */
#define BN_LIMBS 128

typedef struct bn {
    uint32_t n; /* used limbs; the top used limb is nonzero, so zero has n == 0 */
    uint32_t d[BN_LIMBS];
} bn;

static void bn_trim(bn *a)
{
    while (a->n && a->d[a->n - 1] == 0) {
        a->n--;
    }
}

static void bn_set_u64(bn *a, uint64_t v)
{
    a->n = 0;
    if (v) {
        a->d[0] = (uint32_t)v;
        a->d[1] = (uint32_t)(v >> 32);
        a->n = a->d[1] ? 2 : 1;
    }
}

static bool bn_is_zero(const bn *a)
{
    return a->n == 0;
}

static int bn_cmp(const bn *a, const bn *b)
{
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (uint32_t i = a->n; i-- > 0;) {
        if (a->d[i] != b->d[i]) {
            return a->d[i] < b->d[i] ? -1 : 1;
        }
    }
    return 0;
}

/* a = a * m + add. */
static void bn_mul_add(bn *a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        assert(a->n < BN_LIMBS);
        a->d[a->n++] = (uint32_t)carry;
    }
}

/* r = a + b. r may alias a or b. */
static void bn_add(bn *r, const bn *a, const bn *b)
{
    uint32_t n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = carry + (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    r->n = n;
    if (carry) {
        assert(n < BN_LIMBS);
        r->d[r->n++] = (uint32_t)carry;
    }
}

/* r = a - b. Requires a >= b. r may alias a or b. */
static void bn_sub(bn *r, const bn *a, const bn *b)
{
    uint64_t borrow = 0;
    uint32_t an = a->n;
    for (uint32_t i = 0; i < an; i++) {
        uint64_t bi = (i < b->n ? b->d[i] : 0) + borrow;
        uint64_t ai = a->d[i];
        if (ai >= bi) {
            r->d[i] = (uint32_t)(ai - bi);
            borrow = 0;
        } else {
            r->d[i] = (uint32_t)(ai + (1ull << 32) - bi);
            borrow = 1;
        }
    }
    assert(borrow == 0);
    r->n = an;
    bn_trim(r);
}

/* r = a << bits. r must not alias a. */
static void bn_shl(bn *r, const bn *a, uint32_t bits)
{
    if (a->n == 0) {
        r->n = 0;
        return;
    }
    uint32_t limbs = bits / 32;
    uint32_t sh = bits % 32;
    assert(a->n + limbs + 1 <= BN_LIMBS);
    for (uint32_t i = 0; i < limbs; i++) {
        r->d[i] = 0;
    }
    uint32_t carry = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] << sh;
        r->d[limbs + i] = (uint32_t)t | carry;
        carry = (uint32_t)(t >> 32);
    }
    r->n = limbs + a->n;
    if (carry) {
        r->d[r->n++] = carry;
    }
    bn_trim(r);
}

/* a >>= bits, in place. */
static void bn_shr(bn *a, uint32_t bits)
{
    uint32_t limbs = bits / 32;
    uint32_t sh = bits % 32;
    if (limbs >= a->n) {
        a->n = 0;
        return;
    }
    uint32_t nn = a->n - limbs;
    for (uint32_t i = 0; i < nn; i++) {
        uint64_t lo = a->d[i + limbs];
        uint64_t hi = (i + limbs + 1 < a->n) ? a->d[i + limbs + 1] : 0;
        a->d[i] = (uint32_t)(((hi << 32) | lo) >> sh);
    }
    a->n = nn;
    bn_trim(a);
}

/* r = 2^k. */
static void bn_pow2(bn *r, uint32_t k)
{
    uint32_t l = k / 32;
    assert(l < BN_LIMBS);
    for (uint32_t i = 0; i < l; i++) {
        r->d[i] = 0;
    }
    r->d[l] = 1u << (k % 32);
    r->n = l + 1;
}

/* r = v << sh. */
static void bn_u64_shl(bn *r, uint64_t v, uint32_t sh)
{
    bn t;
    bn_set_u64(&t, v);
    bn_shl(r, &t, sh);
}

static uint32_t bn_bitlen(const bn *a)
{
    if (a->n == 0) {
        return 0;
    }
    return 32 * (a->n - 1) + (uint32_t)(32 - __builtin_clz(a->d[a->n - 1]));
}

static bool bn_bit(const bn *a, uint32_t i)
{
    if (i >= 32 * a->n) {
        return false;
    }
    return (a->d[i / 32] >> (i % 32)) & 1u;
}

/* True when any bit with index < i is set. */
static bool bn_low_nonzero(const bn *a, uint32_t i)
{
    uint32_t full = i / 32;
    for (uint32_t k = 0; k < full && k < a->n; k++) {
        if (a->d[k]) {
            return true;
        }
    }
    if (full < a->n && (i % 32) != 0) {
        return (a->d[full] & ((1u << (i % 32)) - 1)) != 0;
    }
    return false;
}

static uint64_t bn_low64(const bn *a)
{
    uint64_t v = a->n > 0 ? a->d[0] : 0;
    if (a->n > 1) {
        v |= (uint64_t)a->d[1] << 32;
    }
    return v;
}

/* a /= div in place, returns the remainder. */
static uint32_t bn_divmod_small(bn *a, uint32_t div)
{
    uint64_t rem = 0;
    for (uint32_t i = a->n; i-- > 0;) {
        uint64_t cur = (rem << 32) | a->d[i];
        a->d[i] = (uint32_t)(cur / div);
        rem = cur % div;
    }
    bn_trim(a);
    return (uint32_t)rem;
}

static void bn_mul_pow10(bn *a, uint32_t k)
{
    while (k >= 9) {
        bn_mul_add(a, 1000000000u, 0);
        k -= 9;
    }
    if (k) {
        uint32_t m = 1;
        while (k--) {
            m *= 10;
        }
        bn_mul_add(a, m, 0);
    }
}

static void bn_mul_pow5(bn *a, uint32_t k)
{
    while (k >= 13) {
        bn_mul_add(a, 1220703125u, 0); /* 5^13 */
        k -= 13;
    }
    if (k) {
        uint32_t m = 1;
        while (k--) {
            m *= 5;
        }
        bn_mul_add(a, m, 0);
    }
}

/* Writes the decimal digits of a (no leading zeros; "0" for zero). Returns the length. */
static int bn_to_dec(const bn *a, char *out)
{
    bn t = *a;
    uint32_t chunks[2 * BN_LIMBS];
    int nc = 0;
    while (t.n) {
        chunks[nc++] = bn_divmod_small(&t, 1000000000u);
    }
    if (nc == 0) {
        out[0] = '0';
        return 1;
    }
    int len = 0;
    char tmp[10];
    uint32_t c = chunks[nc - 1];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + c % 10);
        c /= 10;
    } while (c);
    while (k) {
        out[len++] = tmp[--k];
    }
    for (int i = nc - 2; i >= 0; i--) {
        c = chunks[i];
        for (int j = 8; j >= 0; j--) {
            out[len + j] = (char)('0' + c % 10);
            c /= 10;
        }
        len += 9;
    }
    return len;
}

/* ------------------------------------------------------------------ decimal rounding */

/* Returns the double nearest to (N + frac) * 2^E, where frac is in (0,1) when sticky is set
 * and 0 otherwise. Ties go to even. Requires N to have at least 55 bits when sticky is set. */
static double make_double(uint64_t kept, int target);

static double round_to_double(const bn *N, bool sticky, int E)
{
    uint32_t L = bn_bitlen(N);
    if (L == 0) {
        assert(!sticky);
        return 0.0;
    }
    int emsb = E + (int)L - 1;
    if (emsb > 1023) {
        return dbl_inf(false);
    }
    int target = emsb - 52 > -1074 ? emsb - 52 : -1074;
    int shift = target - E;
    uint64_t kept;
    if (shift <= 0) {
        assert(!sticky);
        kept = bn_low64(N) << (-shift);
    } else {
        bn t = *N;
        bn_shr(&t, (uint32_t)shift);
        kept = bn_low64(&t);
        bool half = bn_bit(N, (uint32_t)shift - 1);
        bool rest = bn_low_nonzero(N, (uint32_t)shift - 1) || sticky;
        if (half && (rest || (kept & 1))) {
            kept++;
        }
    }
    return make_double(kept, target);
}

/* Returns kept * 2^target, where kept <= 2^53 and the result is representable up to rounding
 * already applied by the caller. */
static double make_double(uint64_t kept, int target)
{
    if (kept == 0) {
        return 0.0;
    }
    while (kept >> 53) {
        kept >>= 1;
        target++;
    }
    int L2 = 64 - __builtin_clzll(kept);
    int E2 = target + L2 - 1;
    if (E2 > 1023) {
        return dbl_inf(false);
    }
    uint64_t bits;
    if (E2 >= -1022) {
        uint64_t mant = kept << (53 - L2);
        bits = ((uint64_t)(E2 + 1023) << 52) | (mant & DBL_MANT_MASK);
    } else {
        assert(target == -1074);
        bits = kept;
    }
    return bits_to_dbl(bits);
}

/* Exact decimal expansion of finite x > 0: x == D * 10^P with D of L digits and no trailing
 * zeros. D must hold at least 800 bytes. */
static void exact_decimal(double x, char *D, int *L, int *P)
{
    uint64_t f;
    int e;
    dbl_decompose(x, &f, &e);
    bn N;
    bn_set_u64(&N, f);
    if (e >= 0) {
        bn tmp;
        bn_shl(&tmp, &N, (uint32_t)e);
        N = tmp;
    } else {
        bn_mul_pow5(&N, (uint32_t)(-e));
    }
    int len = bn_to_dec(&N, D);
    int p = e < 0 ? e : 0;
    while (len > 0 && D[len - 1] == '0') {
        len--;
        p++;
    }
    *L = len;
    *P = p;
}

/* Writes nsig significant digits of finite x > 0, rounded half up from the exact value.
 * Returns the decimal exponent of the first digit. */
static int round_sig(double x, int nsig, char *out)
{
    char D[800];
    int L;
    int P;
    exact_decimal(x, D, &L, &P);
    int e10 = L + P - 1;
    if (nsig >= L) {
        memcpy(out, D, (size_t)L);
        memset(out + L, '0', (size_t)(nsig - L));
        return e10;
    }
    memcpy(out, D, (size_t)nsig);
    if (D[nsig] >= '5') {
        int i = nsig - 1;
        while (i >= 0 && out[i] == '9') {
            out[i] = '0';
            i--;
        }
        if (i >= 0) {
            out[i]++;
        } else {
            out[0] = '1';
            memset(out + 1, '0', (size_t)(nsig - 1));
            return e10 + 1;
        }
    }
    return e10;
}

/* ------------------------------------------------------------------ shortest digits */

/* Burger and Dybvig free-format digit generation for finite x > 0. Writes the shortest digit
 * string (at most 17 digits, no trailing zeros) to dg and sets *n_out so that
 * x ~= 0.d1d2... * 10^n. Returns the digit count. */
static int shortest_digits(double x, char *dg, int *n_out)
{
    uint64_t f;
    int e;
    dbl_decompose(x, &f, &e);
    /* The lower neighbour is closer when f is a power of two (and not the smallest normal). */
    bool asym = (f == DBL_HIDDEN) && e > -1074;
    /* Round-half-even on parse means boundary values round-trip exactly when f is even. */
    bool even = (f & 1) == 0;

    bn R, S, Mp, Mm, T;
    if (e >= 0) {
        if (!asym) {
            bn_u64_shl(&R, f, (uint32_t)e + 1);
            bn_set_u64(&S, 2);
            bn_pow2(&Mp, (uint32_t)e);
            bn_pow2(&Mm, (uint32_t)e);
        } else {
            bn_u64_shl(&R, f, (uint32_t)e + 2);
            bn_set_u64(&S, 4);
            bn_pow2(&Mp, (uint32_t)e + 1);
            bn_pow2(&Mm, (uint32_t)e);
        }
    } else {
        if (!asym) {
            bn_u64_shl(&R, f, 1);
            bn_pow2(&S, (uint32_t)(1 - e));
            bn_set_u64(&Mp, 1);
            bn_set_u64(&Mm, 1);
        } else {
            bn_u64_shl(&R, f, 2);
            bn_pow2(&S, (uint32_t)(2 - e));
            bn_set_u64(&Mp, 2);
            bn_set_u64(&Mm, 1);
        }
    }

    /* Estimate k = ceil(log10(x)) from floor(log2(x)), then correct it exactly. */
    int lg2 = e + (63 - __builtin_clzll(f));
    double t = (double)lg2 * 0.30102999566398120;
    int k = (int)t;
    if ((double)k > t) {
        k--;
    }
    k += 1;

    /* Scale so that the value (in units of 10^k) is in [0.1, 1) with the high boundary
     * accounted for. */
    if (k >= 0) {
        bn_mul_pow10(&S, (uint32_t)k);
    } else {
        bn_mul_pow10(&R, (uint32_t)-k);
        bn_mul_pow10(&Mp, (uint32_t)-k);
        bn_mul_pow10(&Mm, (uint32_t)-k);
    }
    for (;;) {
        bn_add(&T, &R, &Mp);
        int c = bn_cmp(&T, &S);
        if (even ? c >= 0 : c > 0) {
            bn_mul_add(&S, 10, 0);
            k++;
        } else {
            break;
        }
    }
    for (;;) {
        bn_add(&T, &R, &Mp);
        bn_mul_add(&T, 10, 0);
        int c = bn_cmp(&T, &S);
        if (even ? c >= 0 : c > 0) {
            break;
        }
        bn_mul_add(&R, 10, 0);
        bn_mul_add(&Mp, 10, 0);
        bn_mul_add(&Mm, 10, 0);
        k--;
    }

    int nd = 0;
    for (;;) {
        bn_mul_add(&R, 10, 0);
        bn_mul_add(&Mp, 10, 0);
        bn_mul_add(&Mm, 10, 0);
        int d = 0;
        while (bn_cmp(&R, &S) >= 0) {
            bn_sub(&R, &R, &S);
            d++;
        }
        bn_add(&T, &R, &Mp);
        int c1 = bn_cmp(&R, &Mm);
        int c2 = bn_cmp(&T, &S);
        bool tc1 = even ? c1 <= 0 : c1 < 0;
        bool tc2 = even ? c2 >= 0 : c2 > 0;
        if (!tc1 && !tc2) {
            dg[nd++] = (char)('0' + d);
            continue;
        }
        int last;
        if (tc1 && !tc2) {
            last = d;
        } else if (!tc1 && tc2) {
            last = d + 1;
        } else {
            /* Both candidates are inside the interval: take the closer one, then the even one. */
            bn_add(&T, &R, &R);
            int c = bn_cmp(&T, &S);
            if (c < 0) {
                last = d;
            } else if (c > 0) {
                last = d + 1;
            } else {
                last = (d & 1) ? d + 1 : d;
            }
        }
        assert(last <= 9);
        dg[nd++] = (char)('0' + last);
        break;
    }
    while (nd > 1 && dg[nd - 1] == '0') {
        nd--;
    }
    *n_out = k;
    return nd;
}

/* ------------------------------------------------------------------ output helpers */

static size_t put_uint(char *o, uint32_t v)
{
    char tmp[12];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    size_t n = 0;
    while (k) {
        o[n++] = tmp[--k];
    }
    return n;
}

static size_t put_cstr(char *o, const char *s)
{
    size_t n = strlen(s);
    memcpy(o, s, n);
    o[n] = 0;
    return n;
}

/* Formats the ECMA-262 Number::toString layout for digits dg (k of them) with exponent n. */
static size_t format_js(char *o, const char *dg, int k, int n)
{
    size_t p = 0;
    if (k <= n && n <= 21) {
        memcpy(o, dg, (size_t)k);
        p = (size_t)k;
        for (int i = 0; i < n - k; i++) {
            o[p++] = '0';
        }
    } else if (0 < n && n <= 21) {
        memcpy(o, dg, (size_t)n);
        p = (size_t)n;
        o[p++] = '.';
        memcpy(o + p, dg + n, (size_t)(k - n));
        p += (size_t)(k - n);
    } else if (-6 < n && n <= 0) {
        o[p++] = '0';
        o[p++] = '.';
        for (int i = 0; i < -n; i++) {
            o[p++] = '0';
        }
        memcpy(o + p, dg, (size_t)k);
        p += (size_t)k;
    } else {
        int e = n - 1;
        o[p++] = dg[0];
        if (k > 1) {
            o[p++] = '.';
            memcpy(o + p, dg + 1, (size_t)(k - 1));
            p += (size_t)(k - 1);
        }
        o[p++] = 'e';
        o[p++] = e < 0 ? '-' : '+';
        p += put_uint(o + p, (uint32_t)(e < 0 ? -e : e));
    }
    o[p] = 0;
    return p;
}

/* ------------------------------------------------------------------ public formatting */

size_t embi_num_to_str(double v, char *out)
{
    if (dbl_is_nan(v)) {
        return put_cstr(out, "NaN");
    }
    if (v == 0) {
        return put_cstr(out, "0");
    }
    if (dbl_is_inf(v)) {
        return put_cstr(out, v < 0 ? "-Infinity" : "Infinity");
    }
    size_t pos = 0;
    if (v < 0) {
        out[pos++] = '-';
        v = -v;
    }
    char dg[24];
    int n;
    int k = shortest_digits(v, dg, &n);
    return pos + format_js(out + pos, dg, k, n);
}

static double floor_nonneg(double v)
{
    if (v >= 4503599627370496.0) { /* 2^52: already integral */
        return v;
    }
    return (double)(uint64_t)v;
}

/* V8 Double::Exponent() > 0, i.e. x >= 2^53. */
static bool exponent_above_zero(double x)
{
    return ((dbl_to_bits(x) >> 52) & 0x7FF) > 1075;
}

size_t embi_num_to_radix_str(double v, unsigned radix, char *out)
{
    if (radix == 10) {
        return embi_num_to_str(v, out);
    }
    if (dbl_is_nan(v)) {
        return put_cstr(out, "NaN");
    }
    if (dbl_is_inf(v)) {
        return put_cstr(out, v < 0 ? "-Infinity" : "Infinity");
    }
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    enum { HALF = 1100, BUF = 2200 };
    char buf[BUF];
    int ic = HALF; /* integer cursor: digits are written downward from here */
    int fc = HALF; /* fraction cursor: digits are written upward from here */

    bool negative = v < 0;
    if (negative) {
        v = -v;
    }
    double integer = floor_nonneg(v);
    double fraction = v - integer;
    /* Only produce fraction digits up to the precision of the input. */
    double next = bits_to_dbl(dbl_to_bits(v) + 1);
    double delta = 0.5 * (next - v);
    double min_sub = bits_to_dbl(1);
    if (delta < min_sub) {
        delta = min_sub;
    }
    if (fraction >= delta) {
        buf[fc++] = '.';
        do {
            fraction *= radix;
            delta *= radix;
            int digit = (int)fraction;
            buf[fc++] = chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    /* Round up, propagating carries back through the written digits. */
                    for (;;) {
                        fc--;
                        if (fc == HALF) {
                            integer += 1;
                            break;
                        }
                        char c = buf[fc];
                        int dd = c > '9' ? c - 'a' + 10 : c - '0';
                        if (dd + 1 < (int)radix) {
                            buf[fc++] = chars[dd + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }

    /* Integer digits. Digits below the representable precision are written as zeros. */
    while (exponent_above_zero(integer / radix)) {
        integer /= radix;
        buf[--ic] = '0';
    }
    do {
        double remainder = fmod(integer, (double)radix);
        buf[--ic] = chars[(int)remainder];
        integer = (integer - remainder) / radix;
    } while (integer > 0);
    if (negative) {
        buf[--ic] = '-';
    }

    size_t len = (size_t)(fc - ic);
    memcpy(out, buf + ic, len);
    out[len] = 0;
    return len;
}

size_t embi_num_to_fixed(double v, unsigned f, char *out)
{
    if (dbl_is_nan(v)) {
        return put_cstr(out, "NaN");
    }
    if (!(v < 1e21 && v > -1e21)) {
        return embi_num_to_str(v, out);
    }
    bool neg = v < 0;
    double x = neg ? -v : v;

    /* n = round_half_up(x * 10^f) as a digit string without leading zeros. */
    char D[800];
    int L = 0;
    int P = 0;
    if (x != 0) {
        exact_decimal(x, D, &L, &P);
    }
    int keep = L + P + (int)f;
    char nd[256];
    int nlen = 0;
    if (x == 0 || keep < 0) {
        nlen = 0;
    } else if (keep >= L) {
        memcpy(nd, D, (size_t)L);
        nlen = L;
        for (int i = 0; i < P + (int)f; i++) {
            nd[nlen++] = '0';
        }
    } else {
        memcpy(nd, D, (size_t)keep);
        nlen = keep;
        if (D[keep] >= '5') {
            int i = nlen - 1;
            while (i >= 0 && nd[i] == '9') {
                nd[i] = '0';
                i--;
            }
            if (i >= 0) {
                nd[i]++;
            } else {
                memmove(nd + 1, nd, (size_t)nlen);
                nd[0] = '1';
                nlen++;
            }
        }
    }

    /* Left-pad to at least f + 1 digits so there is an integer part. */
    char s[300];
    int m = nlen > (int)f + 1 ? nlen : (int)f + 1;
    int pad = m - nlen;
    for (int i = 0; i < pad; i++) {
        s[i] = '0';
    }
    memcpy(s + pad, nd, (size_t)nlen);

    size_t p = 0;
    if (neg) {
        out[p++] = '-';
    }
    int int_len = m - (int)f;
    memcpy(out + p, s, (size_t)int_len);
    p += (size_t)int_len;
    if (f > 0) {
        out[p++] = '.';
        memcpy(out + p, s + int_len, f);
        p += f;
    }
    out[p] = 0;
    return p;
}

/* Writes digit string dg (nsig digits) as d.ddd[e±x]. */
static size_t put_exponential(char *o, const char *dg, int nsig, int e)
{
    size_t p = 0;
    o[p++] = dg[0];
    if (nsig > 1) {
        o[p++] = '.';
        memcpy(o + p, dg + 1, (size_t)(nsig - 1));
        p += (size_t)(nsig - 1);
    }
    o[p++] = 'e';
    o[p++] = e < 0 ? '-' : '+';
    p += put_uint(o + p, (uint32_t)(e < 0 ? -e : e));
    o[p] = 0;
    return p;
}

size_t embi_num_to_exponential(double v, int digits, char *out)
{
    if (dbl_is_nan(v) || dbl_is_inf(v)) {
        return embi_num_to_str(v, out);
    }
    bool neg = v < 0;
    double x = neg ? -v : v;
    char dg[128];
    int nsig;
    int e;
    if (x == 0) {
        nsig = digits < 0 ? 1 : digits + 1;
        memset(dg, '0', (size_t)nsig);
        e = 0;
    } else if (digits < 0) {
        int n;
        nsig = shortest_digits(x, dg, &n);
        e = n - 1;
    } else {
        nsig = digits + 1;
        e = round_sig(x, nsig, dg);
    }
    size_t p = 0;
    if (neg) {
        out[p++] = '-';
    }
    p += put_exponential(out + p, dg, nsig, e);
    return p;
}

size_t embi_num_to_precision(double v, unsigned precision, char *out)
{
    if (dbl_is_nan(v) || dbl_is_inf(v)) {
        return embi_num_to_str(v, out);
    }
    bool neg = v < 0;
    double x = neg ? -v : v;
    int p_ = (int)precision;
    char dg[128];
    int e;
    if (x == 0) {
        memset(dg, '0', (size_t)p_);
        e = 0;
    } else {
        e = round_sig(x, p_, dg);
    }
    size_t p = 0;
    if (neg) {
        out[p++] = '-';
    }
    if (e < -6 || e >= p_) {
        p += put_exponential(out + p, dg, p_, e);
        return p;
    }
    if (e == p_ - 1) {
        memcpy(out + p, dg, (size_t)p_);
        p += (size_t)p_;
    } else if (e >= 0) {
        memcpy(out + p, dg, (size_t)(e + 1));
        p += (size_t)(e + 1);
        out[p++] = '.';
        memcpy(out + p, dg + e + 1, (size_t)(p_ - e - 1));
        p += (size_t)(p_ - e - 1);
    } else {
        out[p++] = '0';
        out[p++] = '.';
        for (int i = 0; i < -e - 1; i++) {
            out[p++] = '0';
        }
        memcpy(out + p, dg, (size_t)p_);
        p += (size_t)p_;
    }
    out[p] = 0;
    return p;
}

/* ------------------------------------------------------------------ parsing */

static const double POW10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/* Correctly rounded value of D[0..nd) * 10^P. D has no trailing zeros and at most 801
 * digits. The caller has already bounded the decimal exponent, so -P <= 1131. */
static double decimal_to_double(const char *D, int nd, int P)
{
    if (nd <= 15 && P >= -22 && P <= 22) {
        uint64_t m = 0;
        for (int i = 0; i < nd; i++) {
            m = m * 10 + (uint64_t)(D[i] - '0');
        }
        double dv = (double)m;
        return P >= 0 ? dv * POW10[P] : dv / POW10[-P];
    }
    bn M;
    bn_set_u64(&M, 0);
    for (int i = 0; i < nd; i++) {
        bn_mul_add(&M, 10, (uint32_t)(D[i] - '0'));
    }
    if (P >= 0) {
        bn_mul_pow10(&M, (uint32_t)P);
        return round_to_double(&M, false, 0);
    }
    bn Den;
    bn_set_u64(&Den, 1);
    bn_mul_pow10(&Den, (uint32_t)(-P));

    /* Quotient M / Den with about 57 to 58 significant bits, then a sticky bit. Let
     * sh = 57 + bd - bm. When sh >= 0 scale the numerator up by 2^sh; otherwise scale the
     * divisor up by 2^-sh. Either way the value is (R / Dv) * 2^-sh. */
    int bm = (int)bn_bitlen(&M);
    int bd = (int)bn_bitlen(&Den);
    int sh = 57 + bd - bm;
    bn R;
    bn Dv;
    if (sh >= 0) {
        bn_shl(&R, &M, (uint32_t)sh);
        Dv = Den;
    } else {
        R = M;
        bn_shl(&Dv, &Den, (uint32_t)(-sh));
    }
    bn T;
    bn_shl(&T, &Dv, 59);
    uint64_t q = 0;
    for (int i = 59; i >= 0; i--) {
        if (bn_cmp(&R, &T) >= 0) {
            bn_sub(&R, &R, &T);
            q |= 1ull << i;
        }
        bn_shr(&T, 1);
    }
    bn Q;
    bn_set_u64(&Q, q);
    return round_to_double(&Q, !bn_is_zero(&R), -sh);
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Digit collection state for decimal parsing. */
typedef struct decimal_acc {
    char D[802];
    int nd;
    int64_t dropped;
    bool sticky;
} decimal_acc;

static void acc_push(decimal_acc *a, char c)
{
    if (a->nd == 0 && c == '0') {
        return; /* leading zeros do not change the value */
    }
    if (a->nd < 800) {
        a->D[a->nd++] = c;
    } else {
        a->dropped++;
        if (c != '0') {
            a->sticky = true;
        }
    }
}

double embi_num_parse_decimal(const char *p, size_t len, size_t *consumed)
{
    size_t i = 0;
    bool neg = false;
    if (i < len && (p[i] == '+' || p[i] == '-')) {
        neg = p[i] == '-';
        i++;
    }
    decimal_acc acc;
    acc.nd = 0;
    acc.dropped = 0;
    acc.sticky = false;
    int64_t nfrac = 0;
    bool any = false;

    while (i < len && is_digit(p[i])) {
        any = true;
        acc_push(&acc, p[i]);
        i++;
    }
    if (i < len && p[i] == '.') {
        size_t j = i + 1;
        bool frac = false;
        while (j < len && is_digit(p[j])) {
            frac = true;
            acc_push(&acc, p[j]);
            nfrac++;
            j++;
        }
        if (any || frac) {
            any = true;
            i = j;
        }
    }
    if (!any) {
        *consumed = 0;
        return NAN;
    }

    int64_t ex = 0;
    if (i < len && (p[i] == 'e' || p[i] == 'E')) {
        size_t j = i + 1;
        bool eneg = false;
        if (j < len && (p[j] == '+' || p[j] == '-')) {
            eneg = p[j] == '-';
            j++;
        }
        if (j < len && is_digit(p[j])) {
            while (j < len && is_digit(p[j])) {
                if (ex < 100000000) {
                    ex = ex * 10 + (p[j] - '0');
                }
                j++;
            }
            if (eneg) {
                ex = -ex;
            }
            i = j;
        }
    }
    *consumed = i;

    if (acc.nd == 0) {
        return neg ? -0.0 : 0.0;
    }
    int64_t P = ex - nfrac + acc.dropped;
    if (acc.sticky) {
        /* Append a nonzero digit below the kept ones: the value stays strictly between the
         * truncated decimal and its successor, so rounding is unchanged. */
        acc.D[acc.nd++] = '1';
        P -= 1;
    }
    while (acc.nd > 0 && acc.D[acc.nd - 1] == '0') {
        acc.nd--;
        P++;
    }
    int64_t M = acc.nd + P; /* value < 10^M */
    double r;
    if (M > 310) {
        r = dbl_inf(false);
    } else if (M < -330) {
        r = 0.0;
    } else {
        r = decimal_to_double(acc.D, acc.nd, (int)P);
    }
    return neg ? -r : r;
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

double embi_num_parse_radix(const char *p, size_t len, unsigned radix, size_t *consumed)
{
    bn acc;
    bn_set_u64(&acc, 0);
    size_t i = 0;
    bool big = false;
    while (i < len) {
        int dv = digit_value(p[i]);
        if (dv >= (int)radix) {
            break;
        }
        if (!big) {
            bn_mul_add(&acc, radix, (uint32_t)dv);
            /* Anything above 2^1100 is beyond DBL_MAX; further digits cannot change that. */
            if (bn_bitlen(&acc) > 1100) {
                big = true;
            }
        }
        i++;
    }
    *consumed = i;
    if (i == 0) {
        return NAN;
    }
    if (big) {
        return dbl_inf(false);
    }
    return round_to_double(&acc, false, 0);
}
