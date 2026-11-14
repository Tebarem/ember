/*
 * BigInt arithmetic (ECMA-262 6.1.6.2).
 *
 * Values are sign and magnitude with 32-bit limbs. Intermediate results live in mbufs. A buffer of
 * up to MBUF_INL limbs (and the small stack buffers of division) sits inside the mbuf itself, so an
 * operation on small values makes no heap allocation beyond the result cell. A heap cell is created
 * only at the end of an operation: big_make copies the magnitude after the allocation, so no input
 * cell is read after a collection point. Multiplication is schoolbook below KARA_MIN limbs and
 * Karatsuba above it. Division is Knuth's algorithm D. Printing in radix r splits the value by
 * powers of r with divide and conquer.
 *
 * Size limit: every result whose magnitude exceeds EMBI_JS_BIGINT_MAX_BITS bits throws a RangeError
 * before its cell is allocated.
 *
 * Errors follow ops.h: a function that throws returns EMBI_JS_EXCEPTION (or nullptr / false) with
 * the exception pending.
 */
#include <stdatomic.h>
#include "js/bigint.h"
#include "js/ops.h"
#include "num/num.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define KARA_MIN 40u
#define TWO32 ((uint64_t)1 << 32)
#define MBUF_INL 8u   /* limbs held inside an mbuf */
#define DIV_VS_INL 8u /* divisor limbs held on the stack in mag_divmod */
#define DIV_US_INL 10u /* dividend limbs held on the stack in mag_divmod */

/* Heap limb buffers allocated so far. The arithmetic tests check that small operations add none. */
static _Atomic uint64_t g_temp_allocs;

uint64_t embi_js_bigint_temp_allocs(void)
{
    return atomic_load_explicit(&g_temp_allocs, memory_order_relaxed);
}

/* ------------------------------------------------------------------ limb buffers */

static uint32_t *limbs_new(size_t n)
{
    if (n == 0) {
        return nullptr;
    }
    atomic_fetch_add_explicit(&g_temp_allocs, 1, memory_order_relaxed);
    return embi_calloc(n, sizeof(uint32_t));
}

static void limbs_free(uint32_t *p, size_t n)
{
    if (p) {
        embi_free(p, n * sizeof(uint32_t));
    }
}

/* A zeroed limb buffer. Its storage is inside the struct when cap <= MBUF_INL, so an mbuf must
 * never be copied: move one with mbuf_move. */
typedef struct mbuf {
    uint32_t *d;
    size_t cap; /* limbs allocated */
    size_t n;   /* limbs in use (normalized where the code says so) */
    uint32_t inl[MBUF_INL];
} mbuf;

static void mbuf_init(mbuf *m, size_t cap)
{
    m->cap = cap;
    m->n = 0;
    if (cap <= MBUF_INL) {
        memset(m->inl, 0, sizeof m->inl);
        m->d = m->inl;
    } else {
        m->d = limbs_new(cap);
    }
}

static void mbuf_free(mbuf *m)
{
    if (m->d != m->inl) {
        limbs_free(m->d, m->cap);
    }
    m->d = nullptr;
    m->cap = 0;
    m->n = 0;
}

/* dst takes the contents of src (dst is freed first); src is left empty. */
static void mbuf_move(mbuf *dst, mbuf *src)
{
    bool inl = src->d == src->inl;
    uint32_t *heap = inl ? nullptr : src->d;
    size_t cap = src->cap;
    size_t n = src->n;
    mbuf_free(dst);
    if (inl) {
        memcpy(dst->inl, src->inl, sizeof dst->inl);
        dst->d = dst->inl;
    } else {
        dst->d = heap;
    }
    dst->cap = cap;
    dst->n = n;
    src->d = nullptr;
    src->cap = 0;
    src->n = 0;
}

/* A magnitude-and-sign view of a value. d is valid for n limbs and may point into a cell, so it is
 * used only before the next allocation. */
typedef struct bview {
    const uint32_t *d;
    size_t n;
    bool neg;
} bview;

static bview view_of(const embi_js_bigint *b)
{
    bview v = {b->limbs, b->hdr.aux, embi_js_bigint_is_neg(b)};
    return v;
}

static size_t mag_len(const uint32_t *a, size_t n)
{
    while (n > 0 && a[n - 1] == 0) {
        n--;
    }
    return n;
}

static size_t mag_bits(const uint32_t *a, size_t n)
{
    n = mag_len(a, n);
    if (n == 0) {
        return 0;
    }
    return (n - 1) * 32u + (size_t)(32 - __builtin_clz(a[n - 1]));
}

static int mag_cmp(const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    if (an != bn) {
        return an < bn ? -1 : 1;
    }
    for (size_t i = an; i-- > 0;) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* r += x; r has rn limbs and xn <= rn; the sum fits by construction. */
static void add_into(uint32_t *r, size_t rn, const uint32_t *x, size_t xn)
{
    uint64_t carry = 0;
    size_t i = 0;
    for (; i < xn; i++) {
        uint64_t s = (uint64_t)r[i] + x[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    for (; carry != 0 && i < rn; i++) {
        uint64_t s = (uint64_t)r[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

/* x -= y; requires x >= y and yn <= xn. */
static void sub_in(uint32_t *x, size_t xn, const uint32_t *y, size_t yn)
{
    uint64_t br = 0;
    for (size_t i = 0; i < xn; i++) {
        uint64_t t = (i < yn ? (uint64_t)y[i] : 0u) + br;
        br = (uint64_t)x[i] < t ? 1u : 0u;
        x[i] = (uint32_t)((uint64_t)x[i] - t);
    }
}

/* r = a + b with room for max(an, bn) + 1 limbs. Returns the normalized length. */
static size_t mag_add(uint32_t *r, const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    if (an < bn) {
        const uint32_t *tp = a;
        a = b;
        b = tp;
        size_t tn = an;
        an = bn;
        bn = tn;
    }
    uint64_t carry = 0;
    size_t i = 0;
    for (; i < bn; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    for (; i < an; i++) {
        uint64_t s = (uint64_t)a[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    r[an] = (uint32_t)carry;
    return mag_len(r, an + 1);
}

/* r = a - b with a >= b; r has room for an limbs. Returns the normalized length. */
static size_t mag_sub(uint32_t *r, const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    for (size_t i = 0; i < an; i++) {
        r[i] = a[i];
    }
    sub_in(r, an, b, bn);
    return mag_len(r, an);
}

/* r += schoolbook product; r is zeroed and holds an + bn limbs. */
static void mag_mul_school(uint32_t *r, const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    for (size_t i = 0; i < an; i++) {
        uint64_t ai = a[i];
        if (ai == 0) {
            continue;
        }
        uint64_t carry = 0;
        for (size_t j = 0; j < bn; j++) {
            uint64_t t = ai * b[j] + r[i + j] + carry;
            r[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        r[i + bn] = (uint32_t)carry;
    }
}

static void mag_mul(uint32_t *r, const uint32_t *a, size_t an, const uint32_t *b, size_t bn);

/* r = a * b for r zeroed with an + bn limbs. Operands may have leading zero limbs. */
static void mag_mul(uint32_t *r, const uint32_t *a, size_t an, const uint32_t *b, size_t bn)
{
    if (an == 0 || bn == 0) {
        return;
    }
    if (an < bn) {
        const uint32_t *tp = a;
        a = b;
        b = tp;
        size_t tn = an;
        an = bn;
        bn = tn;
    }
    if (bn < KARA_MIN) {
        mag_mul_school(r, a, an, b, bn);
        return;
    }
    if (2 * bn <= an) {
        /* Unbalanced: multiply the longer operand in slices of bn limbs. */
        for (size_t off = 0; off < an; off += bn) {
            size_t cl = an - off < bn ? an - off : bn;
            uint32_t *t = limbs_new(cl + bn);
            mag_mul(t, a + off, cl, b, bn);
            add_into(r + off, an + bn - off, t, cl + bn);
            limbs_free(t, cl + bn);
        }
        return;
    }
    /* Balanced Karatsuba: a = a1 B^m + a0, b = b1 B^m + b0, with bn > m. */
    size_t m = an / 2;
    const uint32_t *a0 = a;
    const uint32_t *a1 = a + m;
    size_t a1n = an - m;
    const uint32_t *b0 = b;
    const uint32_t *b1 = b + m;
    size_t b1n = bn - m;

    uint32_t *z0 = limbs_new(2 * m);
    mag_mul(z0, a0, m, b0, m);

    size_t z2cap = a1n + b1n;
    uint32_t *z2 = limbs_new(z2cap);
    mag_mul(z2, a1, a1n, b1, b1n);

    size_t sacap = (m > a1n ? m : a1n) + 1;
    uint32_t *sa = limbs_new(sacap);
    size_t san = mag_add(sa, a0, m, a1, a1n);

    size_t sbcap = (m > b1n ? m : b1n) + 1;
    uint32_t *sb = limbs_new(sbcap);
    size_t sbn = mag_add(sb, b0, m, b1, b1n);

    size_t z1cap = san + sbn;
    uint32_t *z1 = limbs_new(z1cap);
    mag_mul(z1, sa, san, sb, sbn);

    size_t z0n = mag_len(z0, 2 * m);
    size_t z2n = mag_len(z2, z2cap);
    sub_in(z1, z1cap, z0, z0n);
    sub_in(z1, z1cap, z2, z2n);
    size_t z1n = mag_len(z1, z1cap);

    add_into(r, an + bn, z0, z0n);
    add_into(r + m, an + bn - m, z1, z1n);
    add_into(r + 2 * m, an + bn - 2 * m, z2, z2n);

    limbs_free(z0, 2 * m);
    limbs_free(z2, z2cap);
    limbs_free(sa, sacap);
    limbs_free(sb, sbcap);
    limbs_free(z1, z1cap);
}

/* Knuth algorithm D. u has un limbs, v is normalized with vn >= 1 limbs, un >= vn. q has
 * un - vn + 1 limbs and r has vn limbs; both are zeroed by the caller. */
static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, size_t un, const uint32_t *v,
                       size_t vn)
{
    if (vn == 1) {
        uint64_t rem = 0;
        uint64_t dv = v[0];
        for (size_t i = un; i-- > 0;) {
            uint64_t cur = (rem << 32) | u[i];
            q[i] = (uint32_t)(cur / dv);
            rem = cur % dv;
        }
        if (r) {
            r[0] = (uint32_t)rem;
        }
        return;
    }
    unsigned s = (unsigned)__builtin_clz(v[vn - 1]);
    uint32_t vs_buf[DIV_VS_INL];
    uint32_t us_buf[DIV_US_INL];
    uint32_t *vs = vn <= DIV_VS_INL ? vs_buf : limbs_new(vn);
    uint32_t *us = un + 1 <= DIV_US_INL ? us_buf : limbs_new(un + 1);
    for (size_t i = vn; i-- > 0;) {
        uint64_t lo = (uint64_t)v[i] << s;
        uint64_t hi = i ? (uint64_t)v[i - 1] >> (32 - s) : 0;
        vs[i] = (uint32_t)(lo | hi);
    }
    us[un] = (uint32_t)((uint64_t)u[un - 1] >> (32 - s));
    for (size_t i = un; i-- > 0;) {
        uint64_t lo = (uint64_t)u[i] << s;
        uint64_t hi = i ? (uint64_t)u[i - 1] >> (32 - s) : 0;
        us[i] = (uint32_t)(lo | hi);
    }

    uint64_t vtop = vs[vn - 1];
    uint64_t vnext = vs[vn - 2];
    for (size_t j = un - vn + 1; j-- > 0;) {
        uint64_t num = ((uint64_t)us[j + vn] << 32) | us[j + vn - 1];
        uint64_t qhat = num / vtop;
        uint64_t rhat = num % vtop;
        for (;;) {
            if (qhat >= TWO32 || qhat * vnext > ((rhat << 32) | us[j + vn - 2])) {
                qhat--;
                rhat += vtop;
                if (rhat < TWO32) {
                    continue;
                }
            }
            break;
        }

        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < vn; i++) {
            uint64_t p = qhat * vs[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)us[i + j] - (int64_t)(p & 0xFFFFFFFFu) + borrow;
            us[i + j] = (uint32_t)t;
            borrow = t < 0 ? -1 : 0;
        }
        int64_t t = (int64_t)us[j + vn] - (int64_t)carry + borrow;
        us[j + vn] = (uint32_t)t;
        if (t < 0) {
            /* The estimate was one too large: add the divisor back. */
            qhat--;
            uint64_t c = 0;
            for (size_t i = 0; i < vn; i++) {
                uint64_t sum = (uint64_t)us[i + j] + vs[i] + c;
                us[i + j] = (uint32_t)sum;
                c = sum >> 32;
            }
            us[j + vn] = (uint32_t)((uint64_t)us[j + vn] + c);
        }
        q[j] = (uint32_t)qhat;
    }

    if (r) {
        for (size_t i = 0; i < vn; i++) {
            uint64_t hi = (i + 1 < vn) ? us[i + 1] : 0;
            r[i] = (uint32_t)(((uint64_t)us[i] >> s) | (hi << (32 - s)));
        }
    }
    if (vs != vs_buf) {
        limbs_free(vs, vn);
    }
    if (us != us_buf) {
        limbs_free(us, un + 1);
    }
}

/* r = a << s; r is zeroed with an + s / 32 + 1 limbs. Returns the normalized length. */
static size_t mag_shl(uint32_t *r, const uint32_t *a, size_t an, size_t s)
{
    size_t ls = s / 32;
    unsigned bs = (unsigned)(s % 32);
    for (size_t i = 0; i < an; i++) {
        uint64_t v = (uint64_t)a[i] << bs;
        r[i + ls] |= (uint32_t)v;
        r[i + ls + 1] |= (uint32_t)(v >> 32);
    }
    return mag_len(r, an + ls + 1);
}

/* r = a >> s for an limbs in a and room for an - s / 32 limbs in r. Sets *lost when a nonzero bit
 * was shifted out. Returns the normalized length. */
static size_t mag_shr(uint32_t *r, const uint32_t *a, size_t an, size_t s, bool *lost)
{
    size_t ls = s / 32;
    unsigned bs = (unsigned)(s % 32);
    *lost = false;
    if (ls >= an) {
        *lost = mag_len(a, an) != 0;
        return 0;
    }
    for (size_t i = 0; i < ls; i++) {
        if (a[i] != 0) {
            *lost = true;
        }
    }
    if (bs != 0 && (a[ls] & ((1u << bs) - 1u)) != 0) {
        *lost = true;
    }
    size_t rn = an - ls;
    for (size_t i = 0; i < rn; i++) {
        uint64_t lo = a[i + ls];
        uint64_t hi = (i + ls + 1 < an) ? a[i + ls + 1] : 0;
        r[i] = (uint32_t)(((hi << 32) | lo) >> bs);
    }
    return mag_len(r, rn);
}

/* ------------------------------------------------------------------ cell construction */

static const uint32_t k_one_limb[1] = {1};

/* Creates a BigInt from a magnitude that is not a cell's own storage (the allocation may collect).
 * Normalizes the input; zero is never negative. */
static embi_js_bigint *big_make(embi_js_runtime *rt, const uint32_t *d, size_t n, bool neg)
{
    n = mag_len(d, n);
    if (n == 0) {
        neg = false;
    }
    size_t bytes = sizeof(embi_js_cell) + n * sizeof(uint32_t);
    embi_js_cell *c = embi_js_alloc_cell(rt, bytes, EMBI_JS_CELL_BIGINT);
    c->aux = (uint32_t)n;
    c->flags = neg ? EMBI_JS_FLAG_BIG_NEG : 0;
    embi_js_bigint *b = (embi_js_bigint *)c;
    if (n) {
        memcpy(b->limbs, d, n * sizeof(uint32_t));
    }
    return b;
}

static embi_js_value big_val(embi_js_runtime *rt, const uint32_t *d, size_t n, bool neg)
{
    return embi_js_from_bigint(big_make(rt, d, n, neg));
}

static embi_js_value big_zero(embi_js_runtime *rt)
{
    return big_val(rt, nullptr, 0, false);
}

static embi_js_value size_error(embi_js_runtime *rt)
{
    return embi_js_throw_range_error(rt, "Maximum BigInt size exceeded");
}

/* The value of the magnitude in r, with sign neg. Frees r. Throws a RangeError (before any cell is
 * allocated) when the magnitude is above EMBI_JS_BIGINT_MAX_BITS. */
static embi_js_value finish(embi_js_runtime *rt, mbuf *r, bool neg)
{
    if (mag_bits(r->d, r->n) > EMBI_JS_BIGINT_MAX_BITS) {
        mbuf_free(r);
        return size_error(rt);
    }
    embi_js_value v = big_val(rt, r->d, r->n, neg);
    mbuf_free(r);
    return v;
}

/* A copy of the view's magnitude in a buffer (no cell is allocated here). */
static void mag_copy(mbuf *m, bview v)
{
    mbuf_init(m, v.n);
    if (v.n) {
        memcpy(m->d, v.d, v.n * sizeof(uint32_t));
    }
    m->n = v.n;
}

/* ------------------------------------------------------------------ signed arithmetic */

static int cmp_view(bview a, bview b)
{
    if (a.neg != b.neg) {
        return a.neg ? -1 : 1;
    }
    int m = mag_cmp(a.d, mag_len(a.d, a.n), b.d, mag_len(b.d, b.n));
    return a.neg ? -m : m;
}

/* a + b where the signs of a and b are already the effective signs. */
static embi_js_value signed_add(embi_js_runtime *rt, bview a, bview b)
{
    mbuf r;
    if (a.neg == b.neg) {
        mbuf_init(&r, (a.n > b.n ? a.n : b.n) + 1);
        r.n = mag_add(r.d, a.d, a.n, b.d, b.n);
        return finish(rt, &r, a.neg);
    }
    if (mag_cmp(a.d, a.n, b.d, b.n) >= 0) {
        mbuf_init(&r, a.n);
        r.n = mag_sub(r.d, a.d, a.n, b.d, b.n);
        return finish(rt, &r, a.neg);
    }
    mbuf_init(&r, b.n);
    r.n = mag_sub(r.d, b.d, b.n, a.d, a.n);
    return finish(rt, &r, b.neg);
}

static embi_js_value signed_mul(embi_js_runtime *rt, bview a, bview b)
{
    if (a.n == 0 || b.n == 0) {
        return big_zero(rt);
    }
    /* The product has at least bits(a) + bits(b) - 1 bits: reject before multiplying. */
    if ((uint64_t)mag_bits(a.d, a.n) + mag_bits(b.d, b.n) - 1 > EMBI_JS_BIGINT_MAX_BITS) {
        return size_error(rt);
    }
    mbuf p;
    mbuf_init(&p, a.n + b.n);
    mag_mul(p.d, a.d, a.n, b.d, b.n);
    p.n = mag_len(p.d, p.cap);
    return finish(rt, &p, a.neg != b.neg);
}

/* Truncating division: quotient (want_rem false) or remainder (want_rem true). */
static embi_js_value signed_divmod(embi_js_runtime *rt, bview a, bview b, bool want_rem)
{
    if (b.n == 0) {
        return embi_js_throw_range_error(rt, "Division by zero");
    }
    if (mag_cmp(a.d, a.n, b.d, b.n) < 0) {
        if (!want_rem) {
            return big_zero(rt);
        }
        mbuf c;
        mag_copy(&c, a);
        return finish(rt, &c, a.neg);
    }
    mbuf q;
    mbuf r;
    mbuf_init(&q, a.n - b.n + 1);
    mbuf_init(&r, b.n);
    mag_divmod(q.d, r.d, a.d, a.n, b.d, b.n);
    q.n = mag_len(q.d, q.cap);
    r.n = mag_len(r.d, r.cap);
    if (want_rem) {
        mbuf_free(&q);
        return finish(rt, &r, a.neg);
    }
    mbuf_free(&r);
    return finish(rt, &q, a.neg != b.neg);
}

/* Bit length of a view's magnitude. */
static uint64_t bits_of(bview v)
{
    return mag_bits(v.d, v.n);
}

static embi_js_value bigint_pow(embi_js_runtime *rt, bview x, bview y)
{
    if (y.neg) {
        return embi_js_throw_range_error(rt, "Exponent must be non-negative");
    }
    if (y.n == 0) {
        return big_val(rt, k_one_limb, 1, false);
    }
    if (x.n == 0) {
        return big_zero(rt);
    }
    if (x.n == 1 && x.d[0] == 1) {
        bool neg = x.neg && (y.d[0] & 1u) != 0;
        return big_val(rt, k_one_limb, 1, neg);
    }
    if (y.n > 1) {
        return size_error(rt);
    }
    uint64_t e = y.d[0];
    /* |x| >= 2 here, so x^e has at least (bits(x) - 1) * e + 1 bits. */
    if (e > EMBI_JS_BIGINT_MAX_BITS || (bits_of(x) - 1) * e + 1 > EMBI_JS_BIGINT_MAX_BITS) {
        return size_error(rt);
    }
    bool neg = x.neg && (e & 1u) != 0;

    /* Square and multiply from the low bit. Both buffers hold normalized values. */
    mbuf res;
    mbuf base;
    mbuf_init(&res, 1);
    res.d[0] = 1;
    res.n = 1;
    mag_copy(&base, x);
    for (;;) {
        if (e & 1u) {
            mbuf t;
            mbuf_init(&t, res.n + base.n);
            mag_mul(t.d, res.d, res.n, base.d, base.n);
            t.n = mag_len(t.d, t.cap);
            mbuf_move(&res, &t);
        }
        e >>= 1;
        if (e == 0) {
            break;
        }
        mbuf sq;
        mbuf_init(&sq, 2 * base.n);
        mag_mul(sq.d, base.d, base.n, base.d, base.n);
        sq.n = mag_len(sq.d, sq.cap);
        mbuf_move(&base, &sq);
    }
    mbuf_free(&base);
    return finish(rt, &res, neg);
}

/* Shifts x left (left = true) or right by a signed count. Right shifts round toward negative
 * infinity, as >> does on two's complement values. */
static embi_js_value shift_by(embi_js_runtime *rt, bview x, bview cnt, bool left)
{
    bool go_left = left != cnt.neg;
    if (x.n == 0) {
        return big_zero(rt);
    }
    if (cnt.n == 0) {
        mbuf c;
        mag_copy(&c, x);
        return finish(rt, &c, x.neg);
    }
    bool huge = cnt.n > 2;
    uint64_t c = 0;
    if (!huge) {
        c = cnt.d[0] | (cnt.n == 2 ? (uint64_t)cnt.d[1] << 32 : 0u);
    }
    if (go_left) {
        if (huge || c > EMBI_JS_BIGINT_MAX_BITS || bits_of(x) + c > EMBI_JS_BIGINT_MAX_BITS) {
            return size_error(rt);
        }
        size_t s = (size_t)c;
        mbuf r;
        mbuf_init(&r, x.n + s / 32 + 1);
        r.n = mag_shl(r.d, x.d, x.n, s);
        return finish(rt, &r, x.neg);
    }
    if (huge || c >= bits_of(x)) {
        /* Every bit is shifted out: 0 for a non-negative value, -1 for a negative one. */
        return x.neg ? big_val(rt, k_one_limb, 1, true) : big_zero(rt);
    }
    size_t s = (size_t)c;
    bool lost = false;
    if (!x.neg) {
        mbuf r;
        mbuf_init(&r, x.n);
        r.n = mag_shr(r.d, x.d, x.n, s, &lost);
        return finish(rt, &r, false);
    }
    /* Negative: x >> s == -(((|x| - 1) >> s) + 1). */
    mbuf t;
    mbuf q;
    mbuf_init(&t, x.n);
    t.n = mag_sub(t.d, x.d, x.n, k_one_limb, 1);
    mbuf_init(&q, t.n + 1);
    q.n = mag_shr(q.d, t.d, t.n, s, &lost);
    add_into(q.d, q.cap, k_one_limb, 1);
    q.n = mag_len(q.d, q.cap);
    mbuf_free(&t);
    return finish(rt, &q, true);
}

/* Fills out (L limbs) with the two's complement of x, sign-extended. */
static void twos_fill(uint32_t *out, size_t L, bview x)
{
    if (!x.neg) {
        for (size_t i = 0; i < L; i++) {
            out[i] = i < x.n ? x.d[i] : 0u;
        }
        return;
    }
    /* -|x| == ~(|x| - 1) on an infinite sign-extended vector. */
    mbuf t;
    mbuf_init(&t, x.n);
    t.n = mag_sub(t.d, x.d, x.n, k_one_limb, 1);
    for (size_t i = 0; i < L; i++) {
        out[i] = i < x.n ? ~t.d[i] : 0xFFFFFFFFu;
    }
    mbuf_free(&t);
}

static embi_js_value bitwise(embi_js_runtime *rt, enum embi_js_bigop op, bview x, bview y)
{
    size_t L = (x.n > y.n ? x.n : y.n) + 1;
    mbuf a;
    mbuf b;
    mbuf r;
    mbuf_init(&a, L);
    mbuf_init(&b, L);
    mbuf_init(&r, L);
    twos_fill(a.d, L, x);
    twos_fill(b.d, L, y);
    for (size_t i = 0; i < L; i++) {
        switch (op) {
        case EMBI_JS_BIG_AND:
            r.d[i] = a.d[i] & b.d[i];
            break;
        case EMBI_JS_BIG_OR:
            r.d[i] = a.d[i] | b.d[i];
            break;
        default:
            r.d[i] = a.d[i] ^ b.d[i];
            break;
        }
    }
    bool neg = (r.d[L - 1] >> 31) != 0;
    if (neg) {
        /* The magnitude of a negative two's complement value is ~r + 1. */
        uint64_t carry = 1;
        for (size_t i = 0; i < L; i++) {
            uint64_t s = (uint64_t)(uint32_t)~r.d[i] + carry;
            r.d[i] = (uint32_t)s;
            carry = s >> 32;
        }
    }
    r.n = L;
    mbuf_free(&a);
    mbuf_free(&b);
    return finish(rt, &r, neg);
}

embi_js_value embi_js_bigint_binop(embi_js_runtime *rt, enum embi_js_bigop op, embi_js_value a,
                                   embi_js_value b)
{
    bview x = view_of(embi_js_as_bigint(a));
    bview y = view_of(embi_js_as_bigint(b));
    switch (op) {
    case EMBI_JS_BIG_ADD:
        return signed_add(rt, x, y);
    case EMBI_JS_BIG_SUB:
        if (y.n != 0) {
            y.neg = !y.neg;
        }
        return signed_add(rt, x, y);
    case EMBI_JS_BIG_MUL:
        return signed_mul(rt, x, y);
    case EMBI_JS_BIG_DIV:
        return signed_divmod(rt, x, y, false);
    case EMBI_JS_BIG_MOD:
        return signed_divmod(rt, x, y, true);
    case EMBI_JS_BIG_EXP:
        return bigint_pow(rt, x, y);
    case EMBI_JS_BIG_SHL:
        return shift_by(rt, x, y, true);
    case EMBI_JS_BIG_SAR:
        return shift_by(rt, x, y, false);
    case EMBI_JS_BIG_SHR:
        return embi_js_throw_type_error(rt, "BigInts have no unsigned right shift, use >> instead");
    default:
        return bitwise(rt, op, x, y);
    }
}

embi_js_value embi_js_bigint_neg(embi_js_runtime *rt, embi_js_value a)
{
    bview x = view_of(embi_js_as_bigint(a));
    if (x.n == 0) {
        return a;
    }
    mbuf c;
    mag_copy(&c, x);
    embi_js_value res = big_val(rt, c.d, c.n, !x.neg);
    mbuf_free(&c);
    return res;
}

embi_js_value embi_js_bigint_not(embi_js_runtime *rt, embi_js_value a)
{
    /* ~x == -x - 1. For x < 0 the result is |x| - 1 >= 0; for x >= 0 it is -(x + 1). */
    bview x = view_of(embi_js_as_bigint(a));
    mbuf t;
    if (x.neg) {
        mbuf_init(&t, x.n);
        t.n = mag_sub(t.d, x.d, x.n, k_one_limb, 1);
        return finish(rt, &t, false);
    }
    mbuf_init(&t, (x.n > 1 ? x.n : 1) + 1);
    t.n = mag_add(t.d, x.d, x.n, k_one_limb, 1);
    return finish(rt, &t, true);
}

embi_js_value embi_js_bigint_inc(embi_js_runtime *rt, embi_js_value a, int delta)
{
    bview x = view_of(embi_js_as_bigint(a));
    bview d = {k_one_limb, 1, delta < 0};
    return signed_add(rt, x, d);
}

/* Clears the bits of the two's complement vector d (L limbs) at positions >= bits. */
static void mask_low_bits(uint32_t *d, size_t L, uint64_t bits)
{
    size_t w = (size_t)(bits / 32);
    unsigned rb = (unsigned)(bits % 32);
    for (size_t i = w; i < L; i++) {
        if (i > w || rb == 0) {
            d[i] = 0;
        } else {
            d[i] &= (1u << rb) - 1u;
        }
    }
}

embi_js_value embi_js_bigint_as_n(embi_js_runtime *rt, embi_js_value a, uint64_t bits,
                                  bool is_signed)
{
    if (bits == 0) {
        return big_zero(rt);
    }
    bview x = view_of(embi_js_as_bigint(a));
    uint64_t xb = bits_of(x);
    if (is_signed ? xb < bits : (!x.neg && xb <= bits)) {
        return a; /* already in range */
    }
    /* Signed: not in range means bits <= xb <= MAX. Unsigned with negative x: the result has `bits`
     * bits, so it is too large when bits exceeds the limit. */
    if (bits > EMBI_JS_BIGINT_MAX_BITS) {
        return size_error(rt);
    }
    size_t L = (size_t)(bits / 32) + 2;
    mbuf r;
    mbuf_init(&r, L);
    twos_fill(r.d, L, x);
    mask_low_bits(r.d, L, bits);
    if (is_signed) {
        size_t top = (size_t)(bits - 1);
        if ((r.d[top / 32] >> (top % 32)) & 1u) {
            /* At or above 2^(bits-1): the value is r - 2^bits == -(2^bits - r). Negate over L
             * limbs, then keep the low bits, which leaves 2^bits - r. */
            uint64_t carry = 1;
            for (size_t i = 0; i < L; i++) {
                uint64_t s = (uint64_t)(uint32_t)~r.d[i] + carry;
                r.d[i] = (uint32_t)s;
                carry = s >> 32;
            }
            mask_low_bits(r.d, L, bits);
            r.n = mag_len(r.d, L);
            return finish(rt, &r, true);
        }
    }
    r.n = mag_len(r.d, L);
    return finish(rt, &r, false);
}

/* ------------------------------------------------------------------ construction from numbers */

/* Limbs of a non-negative integral double. out holds 48 limbs; returns the normalized length. */
static size_t limbs_from_integral(double at, uint32_t *out)
{
    memset(out, 0, 48 * sizeof(uint32_t));
    if (at < 18446744073709551616.0) {
        uint64_t u = (uint64_t)at;
        out[0] = (uint32_t)u;
        out[1] = (uint32_t)(u >> 32);
        return mag_len(out, 2);
    }
    int e = 0;
    double m = frexp(at, &e);
    uint64_t mant = (uint64_t)ldexp(m, 53);
    uint32_t tmp[2] = {(uint32_t)mant, (uint32_t)(mant >> 32)};
    size_t sh = (size_t)(e - 53);
    uint32_t shifted[48];
    memset(shifted, 0, sizeof shifted);
    size_t n = mag_shl(shifted, tmp, 2, sh);
    memcpy(out, shifted, n * sizeof(uint32_t));
    return n;
}

embi_js_bigint *embi_js_bigint_from_i64(embi_js_runtime *rt, int64_t v)
{
    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    uint32_t l[2] = {(uint32_t)mag, (uint32_t)(mag >> 32)};
    return big_make(rt, l, 2, v < 0);
}

embi_js_bigint *embi_js_bigint_from_double(embi_js_runtime *rt, double d)
{
    uint32_t l[48];
    size_t n = limbs_from_integral(fabs(d), l);
    return big_make(rt, l, n, d < 0);
}

static unsigned digit_of(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'z') {
        return (unsigned)(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'Z') {
        return (unsigned)(c - 'A') + 10u;
    }
    return 99u;
}

embi_js_bigint *embi_js_bigint_from_digits(embi_js_runtime *rt, const char *digits, size_t n,
                                           unsigned radix, bool neg)
{
    size_t lead = 0;
    while (lead < n && digits[lead] == '0') {
        lead++;
    }
    digits += lead;
    n -= lead;
    if (n == 0) {
        return big_make(rt, nullptr, 0, false);
    }
    /* A leading nonzero digit makes the value at least radix^(n-1) >= 2^(n-1). */
    if (n > EMBI_JS_BIGINT_MAX_BITS) {
        (void)embi_js_throw_range_error(rt, "Maximum BigInt size exceeded");
        return nullptr;
    }
    size_t cap = n * 6 / 32 + 3; /* radix <= 36 needs under 6 bits per digit */
    mbuf r;
    mbuf_init(&r, cap);
    size_t i = 0;
    while (i < n) {
        /* Gather as many digits as fit in one limb, then r = r * radix^k + chunk. */
        uint64_t m = 1;
        uint64_t val = 0;
        while (i < n && m * radix <= 0xFFFFFFFFu) {
            val = val * radix + digit_of(digits[i]);
            m *= radix;
            i++;
        }
        uint64_t carry = val;
        for (size_t j = 0; j < r.n; j++) {
            uint64_t t = (uint64_t)r.d[j] * m + carry;
            r.d[j] = (uint32_t)t;
            carry = t >> 32;
        }
        if (carry != 0) {
            r.d[r.n++] = (uint32_t)carry;
        }
    }
    if (mag_bits(r.d, r.n) > EMBI_JS_BIGINT_MAX_BITS) {
        mbuf_free(&r);
        (void)embi_js_throw_range_error(rt, "Maximum BigInt size exceeded");
        return nullptr;
    }
    embi_js_bigint *b = big_make(rt, r.d, r.n, neg);
    mbuf_free(&r);
    return b;
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

bool embi_js_string_to_bigint(embi_js_runtime *rt, const embi_js_string *s0, embi_js_bigint **out)
{
    /* J5g: the characters are read below, so a rope is flattened first (it may allocate). */
    const embi_js_string *s = embi_js_str_flat(rt, embi_js_from_string(s0));
    uint32_t i = 0;
    uint32_t end = embi_js_str_len(s);
    while (i < end && is_js_space(embi_js_str_at(s, i))) {
        i++;
    }
    while (end > i && is_js_space(embi_js_str_at(s, end - 1))) {
        end--;
    }
    if (i == end) {
        *out = big_make(rt, nullptr, 0, false);
        return true;
    }
    unsigned radix = 10;
    bool neg = false;
    if (end - i > 2 && embi_js_str_at(s, i) == '0') {
        uint32_t p = embi_js_str_at(s, i + 1) | 0x20u; /* lowercase the prefix letter */
        if (p == 'x') {
            radix = 16;
        } else if (p == 'o') {
            radix = 8;
        } else if (p == 'b') {
            radix = 2;
        }
        if (radix != 10) {
            i += 2;
        }
    }
    if (radix == 10) {
        uint32_t c = embi_js_str_at(s, i);
        if (c == '+' || c == '-') {
            neg = c == '-';
            i++;
        }
    }
    if (i == end) {
        return false;
    }
    size_t n = end - i;
    char *buf = embi_alloc(n);
    for (size_t k = 0; k < n; k++) {
        uint32_t u = embi_js_str_at(s, i + (uint32_t)k);
        if (u > 0x7F || digit_of((char)u) >= radix) {
            embi_free(buf, n);
            return false;
        }
        buf[k] = (char)u;
    }
    *out = embi_js_bigint_from_digits(rt, buf, n, radix, neg);
    embi_free(buf, n);
    return true;
}

embi_js_value embi_js_to_bigint(embi_js_runtime *rt, embi_js_value v)
{
    embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_NUMBER);
    if (embi_js_is_exception(p)) {
        return EXC;
    }
    if (embi_js_is_bigint(p)) {
        return p;
    }
    if (embi_js_is_bool(p)) {
        return embi_js_from_bigint(embi_js_bigint_from_i64(rt, p == EMBI_JS_TRUE ? 1 : 0));
    }
    if (embi_js_is_string(p)) {
        embi_js_bigint *b = nullptr;
        if (!embi_js_string_to_bigint(rt, embi_js_as_string(p), &b)) {
            return embi_js_throw_syntax_error(rt, "Cannot convert string to a BigInt");
        }
        return b ? embi_js_from_bigint(b) : EXC;
    }
    if (embi_js_is_number(p)) {
        char buf[EMBI_NUM_STR_MAX];
        (void)embi_num_to_str(embi_js_as_double(p), buf);
        return embi_js_throw_type_error(rt, "Cannot convert %s to a BigInt", buf);
    }
    if (embi_js_is_symbol(p)) {
        return embi_js_throw_type_error(rt, "Cannot convert a Symbol value to a BigInt");
    }
    return embi_js_throw_type_error(rt, "Cannot convert %s to a BigInt",
                                    p == EMBI_JS_NULL ? "null" : "undefined");
}

/* ------------------------------------------------------------------ comparison and conversion */

int embi_js_bigint_compare(const embi_js_bigint *a, const embi_js_bigint *b)
{
    return cmp_view(view_of(a), view_of(b));
}

int embi_js_bigint_compare_double(const embi_js_bigint *b, double d)
{
    if (d != d) {
        return 2;
    }
    if (d == INFINITY) {
        return -1;
    }
    if (d == -INFINITY) {
        return 1;
    }
    double t = trunc(d);
    double frac = d - t; /* exact */
    uint32_t tl[48];
    size_t tn = limbs_from_integral(fabs(t), tl);
    bview tv = {tl, tn, t < 0};
    int c = cmp_view(view_of(b), tv);
    if (c != 0) {
        return c;
    }
    if (frac > 0) {
        return -1;
    }
    if (frac < 0) {
        return 1;
    }
    return 0;
}

double embi_js_bigint_to_double(const embi_js_bigint *b)
{
    bview v = view_of(b);
    if (v.n == 0) {
        return 0.0;
    }
    size_t bits = mag_bits(v.d, v.n);
    double r;
    if (bits <= 64) {
        uint64_t u = v.d[0] | (v.n > 1 ? (uint64_t)v.d[1] << 32 : 0u);
        r = (double)u;
    } else {
        /* Keep the top 64 bits and fold everything below them into bit 0, which lies under the
         * rounding bit of the 53-bit result, so the single conversion rounds correctly. */
        size_t sh = bits - 64;
        size_t li = sh / 32;
        unsigned bo = (unsigned)(sh % 32);
        uint64_t lo64 = (uint64_t)v.d[li] | ((li + 1 < v.n ? (uint64_t)v.d[li + 1] : 0u) << 32);
        uint64_t hi32 = li + 2 < v.n ? v.d[li + 2] : 0u;
        uint64_t top = bo == 0 ? lo64 : (lo64 >> bo) | (hi32 << (64 - bo));
        bool sticky = false;
        for (size_t i = 0; i < li && !sticky; i++) {
            sticky = v.d[i] != 0;
        }
        if (bo != 0 && (v.d[li] & ((1u << bo) - 1u)) != 0) {
            sticky = true;
        }
        if (sticky) {
            top |= 1u;
        }
        r = ldexp((double)top, (int)sh);
    }
    return v.neg ? -r : r;
}

uint64_t embi_js_bigint_hash(const embi_js_bigint *b)
{
    uint64_t h = 1469598103934665603ull ^ (embi_js_bigint_is_neg(b) ? 0x9E3779B9ull : 0u);
    for (uint32_t i = 0; i < b->hdr.aux; i++) {
        h ^= b->limbs[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* ------------------------------------------------------------------ printing */

static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* Powers radix^(k * 2^i) with their digit widths, all at most the value being printed. */
typedef struct pw_entry {
    uint32_t *d;
    size_t cap;
    size_t n;
    size_t w;
} pw_entry;

typedef struct pw_table {
    pw_entry e[64];
    size_t count;
    unsigned radix;
    unsigned chunk_k;   /* digits per chunk: radix^chunk_k fits in a limb */
    uint32_t chunk_div; /* radix^chunk_k */
} pw_table;

static void pw_free(pw_table *t)
{
    for (size_t i = 0; i < t->count; i++) {
        limbs_free(t->e[i].d, t->e[i].cap);
    }
    t->count = 0;
}

static void pw_build(pw_table *t, const uint32_t *v, size_t vn)
{
    t->count = 0;
    uint32_t one_cd[1] = {t->chunk_div};
    if (mag_cmp(one_cd, 1, v, vn) > 0) {
        return; /* the whole value is one chunk or less: the base case prints it */
    }
    t->e[0].d = limbs_new(1);
    t->e[0].d[0] = t->chunk_div;
    t->e[0].cap = 1;
    t->e[0].n = 1;
    t->e[0].w = t->chunk_k;
    t->count = 1;
    while (t->count < 64) {
        pw_entry *last = &t->e[t->count - 1];
        size_t cap = 2 * last->n;
        uint32_t *sq = limbs_new(cap);
        mag_mul(sq, last->d, last->n, last->d, last->n);
        size_t n = mag_len(sq, cap);
        if (mag_cmp(sq, n, v, vn) > 0) {
            limbs_free(sq, cap);
            break;
        }
        t->e[t->count].d = sq;
        t->e[t->count].cap = cap;
        t->e[t->count].n = n;
        t->e[t->count].w = 2 * last->w;
        t->count++;
    }
}

/* Writes exactly W digits of v (which is below radix^W) into out, zero-padded on the left. */
static void emit_digits(const pw_table *t, const uint32_t *v, size_t vn, size_t W, char *out)
{
    size_t best = (size_t)-1;
    for (size_t i = 0; i < t->count; i++) {
        if (mag_cmp(t->e[i].d, t->e[i].n, v, vn) > 0) {
            break;
        }
        best = i;
    }
    if (best == (size_t)-1) {
        /* Base case: peel chunk_k digits at a time off a copy of v. */
        mbuf tmp;
        mbuf_init(&tmp, vn);
        if (vn) {
            memcpy(tmp.d, v, vn * sizeof(uint32_t));
        }
        tmp.n = mag_len(tmp.d, vn);
        size_t pos = W;
        while (tmp.n > 0) {
            uint64_t rem = 0;
            for (size_t i = tmp.n; i-- > 0;) {
                uint64_t cur = (rem << 32) | tmp.d[i];
                tmp.d[i] = (uint32_t)(cur / t->chunk_div);
                rem = cur % t->chunk_div;
            }
            tmp.n = mag_len(tmp.d, tmp.n);
            for (unsigned k = 0; k < t->chunk_k && pos > 0; k++) {
                out[--pos] = k_digits[rem % t->radix];
                rem /= t->radix;
            }
        }
        while (pos > 0) {
            out[--pos] = '0';
        }
        mbuf_free(&tmp);
        return;
    }
    const pw_entry *p = &t->e[best];
    mbuf q;
    mbuf r;
    mbuf_init(&q, vn - p->n + 1);
    mbuf_init(&r, p->n);
    mag_divmod(q.d, r.d, v, vn, p->d, p->n);
    q.n = mag_len(q.d, q.cap);
    r.n = mag_len(r.d, r.cap);
    emit_digits(t, q.d, q.n, W - p->w, out);
    emit_digits(t, r.d, r.n, p->w, out + (W - p->w));
    mbuf_free(&q);
    mbuf_free(&r);
}

embi_js_string *embi_js_bigint_to_string(embi_js_runtime *rt, const embi_js_bigint *b, unsigned radix)
{
    bview v = view_of(b);
    if (v.n == 0) {
        return embi_js_str_new_latin1(rt, (const uint8_t *)"0", 1);
    }
    pw_table t;
    memset(&t, 0, sizeof t);
    t.radix = radix;
    t.chunk_k = 0;
    t.chunk_div = 1;
    while ((uint64_t)t.chunk_div * radix <= 0xFFFFFFFFu) {
        t.chunk_div *= radix;
        t.chunk_k++;
    }
    size_t bits = mag_bits(v.d, v.n);
    size_t W = (size_t)((double)bits / log2((double)radix)) + 2;
    pw_build(&t, v.d, v.n);
    char *digits = embi_alloc(W);
    emit_digits(&t, v.d, v.n, W, digits);
    pw_free(&t);
    size_t start = 0;
    while (start + 1 < W && digits[start] == '0') {
        start++;
    }
    size_t len = W - start;
    size_t total = len + (v.neg ? 1u : 0u);
    char *text = embi_alloc(total);
    size_t o = 0;
    if (v.neg) {
        text[o++] = '-';
    }
    memcpy(text + o, digits + start, len);
    embi_js_string *s = embi_js_str_new_latin1(rt, (const uint8_t *)text, total);
    embi_free(text, total);
    embi_free(digits, W);
    return s;
}

/* ------------------------------------------------------------------ wrapper objects */

embi_js_value embi_js_bigint_wrapper_value(const embi_js_object *o)
{
    return (embi_js_value)(uintptr_t)o->internal;
}

static void bigint_wrapper_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_js_mark_value(t, embi_js_bigint_wrapper_value(o));
}

const embi_js_class embi_js_class_bigint = {
    .name = "BigInt",
    .trace = bigint_wrapper_trace,
};

embi_js_object *embi_js_bigint_wrapper_new(embi_js_runtime *rt, embi_js_value v)
{
    embi_js_object *o = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_BIGINT],
                                                 &embi_js_class_bigint);
    EMBI_JS_WB(o, v);
    o->internal = (void *)(uintptr_t)v;
    EMBI_JS_WB_STRUCT(o);
    return o;
}
