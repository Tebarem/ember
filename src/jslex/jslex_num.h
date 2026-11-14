#pragma once
/*
 * Streaming number accumulator behind the lexer's numeric literals. Internal to jslex.
 *
 * A decimal accumulator keeps up to EMBI_JSNUM_MAX_DIGITS significant digits. Further digits
 * only count, and a nonzero one sets a sticky digit, which keeps the rounding exact. A radix
 * accumulator is a bignum. embi_jsnum_finish rounds correctly to a double.
 */
#include <stdbool.h>
#include <stdint.h>

#define EMBI_JSNUM_LIMBS 160u
#define EMBI_JSNUM_MAX_DIGITS 800u

typedef struct embi_jsbig {
    uint32_t n;                    /* significant limbs, 0 for zero */
    uint32_t w[EMBI_JSNUM_LIMBS];  /* little endian 32-bit limbs */
} embi_jsbig;

typedef struct embi_jsnum {
    unsigned radix;                /* 2, 8, 10 or 16 */
    bool seen_point;               /* decimal: a '.' was read */
    bool overflow;                 /* radix: the value is above 2^1100, so it is +Infinity */
    bool dropped_nonzero;          /* decimal: a nonzero digit was dropped */
    uint32_t nd;                   /* decimal: kept significant digits */
    uint64_t dropped;              /* decimal: digits dropped after the kept ones */
    uint64_t frac;                 /* decimal: digits after the point, zeros included */
    uint8_t d[EMBI_JSNUM_MAX_DIGITS + 1];
    embi_jsbig big;                /* radix: the value so far */
} embi_jsnum;

void embi_jsnum_init(embi_jsnum *a, unsigned radix);
void embi_jsnum_digit(embi_jsnum *a, unsigned d);
void embi_jsnum_point(embi_jsnum *a);
/* exp10 is the signed value of the exponent part and is ignored for radix != 10. Finishing
 * consumes the accumulator. */
double embi_jsnum_finish(embi_jsnum *a, int64_t exp10);
