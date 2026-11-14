#pragma once
/*
 * Unicode normalization (UAX #15) for String.prototype.normalize. The tables are generated from
 * UCD 17.0.0 by tools/unicodegen/normgen.c into js/unicode_norm.inc; see unicode_norm.c for the
 * algorithms.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>

enum embi_norm_form {
    EMBI_NORM_NFC,
    EMBI_NORM_NFD,
    EMBI_NORM_NFKC,
    EMBI_NORM_NFKD,
};

typedef EMBI_VEC(uint16_t) embi_norm_units;

/* Normalizes n UTF-16 code units in form. Lone surrogates pass through unchanged. The result is
 * written to out (an empty vector is valid; the caller frees it with embi_vec_free). Always
 * returns true: allocation failure aborts inside the base allocator (the bool is kept for the
 * call-site shape). */
bool embi_norm_run(const uint16_t *in, uint32_t n, enum embi_norm_form form, embi_norm_units *out);
