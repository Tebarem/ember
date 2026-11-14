#pragma once
/*
 * Internal to module image: the JPEG and GIF path (SDL_image glue, header checks before any
 * decoding, EXIF orientation). Public entry points are in image.h. The tests use these directly.
 */
#include "image/image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Non-zero sides within EMBI_IMAGE_MAX_DIM whose product is at most max_pixels. */
[[nodiscard]] bool embi_size_ok(uint32_t w, uint32_t h, uint64_t max_pixels);

/* JPEG marker walk. pos starts at 2 (after SOI) and advances past each segment. Fill bytes and
 * standalone markers are skipped. Returns false at SOS, EOI or a malformed stream. On success
 * *marker is the marker code, *data the payload (after the length field), *dlen its length. */
[[nodiscard]] bool embi_jpeg_next_segment(const uint8_t *in, size_t len, size_t *pos,
                                          uint8_t *marker, const uint8_t **data, size_t *dlen);

/* Reads the first SOFn header and checks its size against the limits, without decoding. */
[[nodiscard]] bool embi_jpeg_probe(const uint8_t *in, size_t len, uint64_t max_pixels, uint32_t *w,
                                   uint32_t *h);
/* Reads the logical screen and the first image descriptor and checks both sizes. The reported
 * size is the logical screen, which is the size SDL_image returns for the first frame. */
[[nodiscard]] bool embi_gif_probe(const uint8_t *in, size_t len, uint64_t max_pixels, uint32_t *w,
                                  uint32_t *h);

/* Decodes a JPEG (fmt EMBI_IMAGE_FMT_JPEG) or the first frame of a GIF (EMBI_IMAGE_FMT_GIF) with
 * SDL_image's stb_image or GIF backend, to straight RGBA8. On success *out owns the pixels. On
 * failure *out is untouched. Callers probe the size first. */
[[nodiscard]] bool embi_sdl_decode(const uint8_t *in, size_t len, embi_image_format fmt,
                                   embi_pixels *out);

/* EXIF Orientation (1-8) from the first APP1 Exif segment of a JPEG, read from IFD0 with bounds
 * checks for both byte orders. Returns 1 (no rotation) when the tag is missing or malformed. */
[[nodiscard]] int embi_jpeg_orientation(const uint8_t *in, size_t len);
/* The EXIF orientation of a PNG (an eXIf chunk before the first IDAT), 1 when there is none. */
[[nodiscard]] int embi_png_orientation(const uint8_t *in, size_t len);

/* Applies an EXIF orientation to decoded pixels. 1 and out-of-range values change nothing.
 * Orientations 2-4 run in place; 5-8 swap the width and height and replace the buffer. Always
 * returns true: embi_alloc aborts on out-of-memory instead of failing. */
[[nodiscard]] bool embi_orient_pixels(embi_pixels *p, int orientation);

/* The frames of an animated GIF: n full-canvas RGBA8 frames of w x h in px (frame i at i * w * h * 4), the delay of each in
 * ms (at least 100 for a source delay of 10 ms or less), and the number of plays (0: forever). cap is the allocated
 * count. */
typedef struct embi_gif_frames {
    uint32_t w, h, n, cap;
    uint8_t *px;
    uint32_t *delay_ms;
    uint32_t loops;
} embi_gif_frames;

/* Decodes every frame (see formats.c). Fails for a still GIF (fewer than two frames), a decoding error, frames of
 * different sizes, or frames over max_bytes together. embi_gif_probe must have accepted the bytes. */
[[nodiscard]] bool embi_gif_decode_frames(const uint8_t *in, size_t len, uint64_t max_pixels, size_t max_bytes,
                                          embi_gif_frames *out);
void embi_gif_frames_free(embi_gif_frames *f);
