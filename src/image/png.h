#pragma once
/*
 * PNG decoder (module image). Covers every color type and bit depth, PLTE and tRNS, Adam7
 * interlacing, all five filters, and CRC checks. IDAT streams through the codec's inflater one
 * scanline at a time, so memory is the output plus one row. Ancillary chunks are ignored (gAMA,
 * cHRM, iCCP, sRGB, APNG chunks: the default image only). A bad CRC on a critical chunk fails the
 * decode. A bad CRC on an ancillary chunk skips that chunk.
 *
 * Samples scale to 8 bits as (v * 255 + max / 2) / max, where max = 2^depth - 1. Premultiplied
 * output uses (c * a + 127) / 255 for each color channel.
 */
#include "image/image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads the signature and IHDR (with its CRC) and reports the size. Does not decode pixels. */
[[nodiscard]] bool embi_png_probe(const uint8_t *in, size_t len, uint32_t *w, uint32_t *h);

/* Decodes a complete PNG held in memory. Fails when w * h exceeds max_pixels or either side
 * exceeds EMBI_IMAGE_MAX_DIM. premultiply selects premultiplied or straight RGBA8 output. On
 * success *out owns the pixels. On failure *out is untouched and nothing is leaked. */
[[nodiscard]] bool embi_png_decode(const uint8_t *in, size_t len, uint64_t max_pixels,
                                   bool premultiply, embi_pixels *out);
