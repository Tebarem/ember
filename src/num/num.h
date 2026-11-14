#pragma once
/*
 * Number <-> string conversion for the JS engine and CSS.
 *
 * Formatting functions write a NUL-terminated result into a caller buffer of at least
 * EMBI_NUM_STR_MAX bytes and return the length excluding the NUL. Output is exact:
 *   - shortest output is the shortest decimal that round-trips (ECMA-262 Number::toString);
 *   - toFixed / toExponential / toPrecision round the exact binary value, ties away from zero
 *     (the spec's "pick the larger n" rule for non-negative values);
 *   - parsing is correctly rounded (ties to even).
 * Range checks that throw in JS (digits, precision, radix) are the caller's job.
 */
#include <stddef.h>
#include <stdint.h>

/* Largest possible result, including the NUL. Radix 2 output of DBL_MAX needs about 2.1 KB. */
#define EMBI_NUM_STR_MAX 2208

/* Number::toString(10). Handles NaN, Infinity, -0 ("0"). */
size_t embi_num_to_str(double v, char *out);

/* Number.prototype.toString(radix) for radix in 2..36. Radix 10 is embi_num_to_str. */
size_t embi_num_to_radix_str(double v, unsigned radix, char *out);

/* Number.prototype.toFixed(digits) with digits in 0..100. Values with |v| >= 1e21 use
 * embi_num_to_str. */
size_t embi_num_to_fixed(double v, unsigned digits, char *out);

/* Number.prototype.toExponential(fractionDigits). Pass digits < 0 for "undefined" (shortest
 * round-trip digits). Otherwise digits is in 0..100. */
size_t embi_num_to_exponential(double v, int digits, char *out);

/* Number.prototype.toPrecision(p) with p in 1..100. Caller handles the undefined case by
 * calling embi_num_to_str. */
size_t embi_num_to_precision(double v, unsigned precision, char *out);

/* Parses the longest prefix of p[0..len) that matches
 *   [+-]? (digits ('.' digits?)? | '.' digits) ([eE] [+-]? digits)?
 * with no whitespace and no Infinity. *consumed receives the number of bytes used. Returns
 * NaN and sets *consumed to 0 when no digits are present. Overflow gives +/-Infinity and
 * underflow gives +/-0. The result is correctly rounded. */
double embi_num_parse_decimal(const char *p, size_t len, size_t *consumed);

/* Parses the longest prefix of unsigned digits in the given radix (2..36, case-insensitive).
 * Returns NaN and sets *consumed to 0 when there are no digits. The result is correctly
 * rounded, so it is exact for hex/octal/binary StringToNumber and for parseInt. */
double embi_num_parse_radix(const char *p, size_t len, unsigned radix, size_t *consumed);
