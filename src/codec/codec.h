#pragma once
/*
 * Internal codec API (module codec): DEFLATE/zlib, Brotli (RFC 7932), WOFF 1.0 and WOFF 2.0.
 *
 * Every decoder takes an output limit (max_out bytes). Malformed, truncated or oversized input is
 * rejected with false (or EMBI_INFLATE_ERROR / NEED_INPUT): no decoder reads or writes out of
 * bounds, and the produced output never exceeds max_out bytes. Peak allocation can be a small
 * multiple of max_out (buffers grow by doubling, and WOFF 2.0 holds the decompressed block, the
 * rebuilt tables and the output at the same time).
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ byte buffer */

/* Growable byte buffer. A zeroed embi_buf is empty. Release with embi_buf_free. */
typedef struct embi_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} embi_buf;

void embi_buf_free(embi_buf *b);
/* Makes room for extra bytes past len. Returns false when len + extra overflows size_t. */
[[nodiscard]] bool embi_buf_reserve(embi_buf *b, size_t extra);
/* Appends n bytes. Returns false on overflow. */
[[nodiscard]] bool embi_buf_append(embi_buf *b, const void *src, size_t n);

/* ------------------------------------------------------------------ DEFLATE (RFC 1951, zlib RFC 1950) */

typedef enum embi_inflate_status {
    EMBI_INFLATE_OK,         /* output space filled; call again with more output space */
    EMBI_INFLATE_NEED_INPUT, /* input exhausted before the stream ended */
    EMBI_INFLATE_DONE,       /* stream complete; trailing input bytes are not consumed */
    EMBI_INFLATE_ERROR,      /* malformed data, bad checksum, or the output limit was exceeded */
} embi_inflate_status;

/* Streaming inflater. Heap allocated; state is kept between calls, so input and output can be
 * supplied in pieces. Output never exceeds max_out bytes in total. */
typedef struct embi_inflater embi_inflater;

[[nodiscard]] embi_inflater *embi_inflater_new(bool zlib_wrapped, size_t max_out);
void embi_inflater_free(embi_inflater *z);
/* Consumes input from in into *in_used bytes and writes at most out_cap bytes into out, reporting
 * the count in *out_len. */
[[nodiscard]] embi_inflate_status embi_inflate(embi_inflater *z, const uint8_t *in, size_t in_len,
                                               size_t *in_used, uint8_t *out, size_t out_cap,
                                               size_t *out_len);
/* One-shot: clears out, then decodes the whole input. Returns true only when the stream ends
 * cleanly (checksum verified for zlib) within max_out bytes. */
[[nodiscard]] bool embi_inflate_all(const uint8_t *in, size_t in_len, bool zlib_wrapped,
                                    size_t max_out, embi_buf *out);

/* ------------------------------------------------------------------ Brotli (RFC 7932) */

/* Decodes a complete Brotli stream. Clears out first. The static dictionary is the RFC 7932
 * Appendix A data (brotli_tables.c). */
[[nodiscard]] bool embi_brotli_decode(const uint8_t *in, size_t in_len, size_t max_out,
                                      embi_buf *out);

#define EMBI_BROTLI_DICT_SIZE 122784
#define EMBI_BROTLI_NUM_TRANSFORMS 121

typedef struct embi_brotli_transform {
    const char *prefix;
    uint8_t type; /* 0 Identity, 1 FermentFirst, 2 FermentAll, 3..11 OmitFirst1..9, 12..20 OmitLast1..9 */
    const char *suffix;
} embi_brotli_transform;

/* The static dictionary (EMBI_BROTLI_DICT_SIZE bytes). With EMBER_COMPRESS_ASSETS it is stored LZ-compressed (SZ3a): open
 * expands it into a fresh allocation, close frees it (a decode opens it on its first dictionary reference). Without, open
 * returns the array in the library and close does nothing. */
const uint8_t *embi_brotli_dict_open(void);
void embi_brotli_dict_close(const uint8_t *dict);
extern const uint8_t embi_brotli_lut0[256];
extern const uint8_t embi_brotli_lut1[256];
extern const uint8_t embi_brotli_lut2[256];
extern const embi_brotli_transform embi_brotli_transforms[EMBI_BROTLI_NUM_TRANSFORMS];
extern const uint8_t embi_brotli_ndbits[25];
extern const uint32_t embi_brotli_doffset[25];

/* ------------------------------------------------------------------ sfnt helpers */

typedef struct embi_sfnt_table {
    uint32_t tag;
    const uint8_t *data;
    size_t len;
} embi_sfnt_table;

/* Big-endian sum of 32-bit words, with the tail zero-padded (the OpenType table checksum). */
[[nodiscard]] uint32_t embi_sfnt_checksum(const uint8_t *p, size_t len);
/* Writes a single-font sfnt (offset table, sorted directory, 4-byte padded tables) into out, which
 * must be empty. Recomputes every checksum and head.checkSumAdjustment. Duplicate tags fail. */
[[nodiscard]] bool embi_sfnt_build(uint32_t flavor, const embi_sfnt_table *tables, size_t n,
                                   embi_buf *out);

/* ------------------------------------------------------------------ WOFF 1.0 and WOFF 2.0 */

/* Each returns an sfnt in out (cleared first). Fails when the decoded font would exceed max_out. */
[[nodiscard]] bool embi_woff1_decode(const uint8_t *in, size_t in_len, size_t max_out,
                                     embi_buf *out);
[[nodiscard]] bool embi_woff2_decode(const uint8_t *in, size_t in_len, size_t max_out,
                                     embi_buf *out);
