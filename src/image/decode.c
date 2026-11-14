#include "image/image.h"

#include "image/internal.h"
#include "image/png.h"

#include <SDL3/SDL_stdinc.h>

static const uint8_t kPngSig[8] = {137, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

embi_image_format embi_image_sniff(const uint8_t *in, size_t len)
{
    if (len >= 8 && SDL_memcmp(in, kPngSig, 8) == 0) {
        return EMBI_IMAGE_FMT_PNG;
    }
    if (len >= 3 && in[0] == 0xFF && in[1] == 0xD8 && in[2] == 0xFF) {
        return EMBI_IMAGE_FMT_JPEG;
    }
    if (len >= 6 && (SDL_memcmp(in, "GIF87a", 6) == 0 || SDL_memcmp(in, "GIF89a", 6) == 0)) {
        return EMBI_IMAGE_FMT_GIF;
    }
    return EMBI_IMAGE_FMT_OTHER;
}

/* Decodes the image with the decoder the magic bytes select. Each built-in path checks the size
 * from the header before it decodes, so a header that claims a huge image is refused without any
 * pixels being allocated. */
bool embi_image_decode(const uint8_t *in, size_t len, uint64_t max_pixels, bool apply_orientation,
                       embi_image_decoder_fn fallback, void *user, embi_pixels *out)
{
    switch (embi_image_sniff(in, len)) {
    case EMBI_IMAGE_FMT_PNG: {
        embi_pixels p = {0};
        if (!embi_png_decode(in, len, max_pixels, false, &p)) {
            return false;
        }
        if (apply_orientation && !embi_orient_pixels(&p, embi_png_orientation(in, len))) {
            embi_pixels_free(&p);
            return false;
        }
        *out = p;
        return true;
    }
    case EMBI_IMAGE_FMT_JPEG: {
        uint32_t w = 0;
        uint32_t h = 0;
        if (!embi_jpeg_probe(in, len, max_pixels, &w, &h)) {
            return false;
        }
        embi_pixels p = {0};
        if (!embi_sdl_decode(in, len, EMBI_IMAGE_FMT_JPEG, &p)) {
            return false;
        }
        if (p.w != w || p.h != h) {
            embi_log_debug("image: JPEG header says %ux%u, decoded %ux%u", w, h, p.w, p.h);
            embi_pixels_free(&p);
            return false;
        }
        /* The EXIF rotation runs after the size check: orientations 5-8 swap the sides. */
        if (apply_orientation && !embi_orient_pixels(&p, embi_jpeg_orientation(in, len))) {
            embi_pixels_free(&p);
            return false;
        }
        *out = p;
        return true;
    }
    case EMBI_IMAGE_FMT_GIF: {
        uint32_t w = 0;
        uint32_t h = 0;
        if (!embi_gif_probe(in, len, max_pixels, &w, &h)) {
            return false;
        }
        embi_pixels p = {0};
        if (!embi_sdl_decode(in, len, EMBI_IMAGE_FMT_GIF, &p)) {
            return false;
        }
        if (p.w != w || p.h != h) {
            embi_log_debug("image: GIF screen is %ux%u, decoded %ux%u", w, h, p.w, p.h);
            embi_pixels_free(&p);
            return false;
        }
        *out = p;
        return true;
    }
    default:
        break;
    }
    if (!fallback) {
        return false;
    }
    embi_pixels p = {0};
    if (!fallback(user, in, len, &p) || !p.px) {
        embi_pixels_free(&p);
        return false;
    }
    if (!embi_size_ok(p.w, p.h, max_pixels)) {
        embi_pixels_free(&p);
        return false;
    }
    *out = p;
    return true;
}
