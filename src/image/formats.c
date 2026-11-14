#include "image/internal.h"

#include <SDL3_image/SDL_image.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint32_t)p[1] << 8));
}

bool embi_size_ok(uint32_t w, uint32_t h, uint64_t max_pixels)
{
    return w != 0 && h != 0 && w <= EMBI_IMAGE_MAX_DIM && h <= EMBI_IMAGE_MAX_DIM &&
           (uint64_t)w * h <= max_pixels;
}

/* Start-of-frame markers: C0-CF except C4 (DHT), C8 (JPG) and CC (DAC). */
static bool is_sof(uint8_t m)
{
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

bool embi_jpeg_next_segment(const uint8_t *in, size_t len, size_t *pos, uint8_t *marker,
                            const uint8_t **data, size_t *dlen)
{
    size_t p = *pos;
    for (;;) {
        if (p >= len || in[p] != 0xFF) {
            return false;
        }
        while (p < len && in[p] == 0xFF) {
            p++; /* fill bytes before a marker */
        }
        if (p >= len) {
            return false;
        }
        uint8_t m = in[p++];
        if (m == 0x00) {
            return false; /* a stuffed byte outside entropy-coded data */
        }
        if (m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) {
            continue; /* standalone markers have no length */
        }
        if (m == 0xD9 || m == 0xDA) {
            return false; /* EOI or SOS: the header is over */
        }
        if (len - p < 2) {
            return false;
        }
        size_t seg = be16(in + p);
        if (seg < 2 || seg > len - p) {
            return false;
        }
        *marker = m;
        *data = in + p + 2;
        *dlen = seg - 2;
        *pos = p + seg;
        return true;
    }
}

bool embi_jpeg_probe(const uint8_t *in, size_t len, uint64_t max_pixels, uint32_t *w, uint32_t *h)
{
    *w = 0;
    *h = 0;
    if (len < 4 || in[0] != 0xFF || in[1] != 0xD8) {
        return false;
    }
    size_t pos = 2;
    for (;;) {
        uint8_t m = 0;
        const uint8_t *d = nullptr;
        size_t dl = 0;
        if (!embi_jpeg_next_segment(in, len, &pos, &m, &d, &dl)) {
            return false;
        }
        if (is_sof(m)) {
            /* Payload: precision (1 byte), height (2), width (2). */
            if (dl < 5) {
                return false;
            }
            uint32_t hh = be16(d + 1);
            uint32_t ww = be16(d + 3);
            if (!embi_size_ok(ww, hh, max_pixels)) {
                return false;
            }
            *w = ww;
            *h = hh;
            return true;
        }
    }
}

bool embi_gif_probe(const uint8_t *in, size_t len, uint64_t max_pixels, uint32_t *w, uint32_t *h)
{
    *w = 0;
    *h = 0;
    if (len < 13 || (SDL_memcmp(in, "GIF87a", 6) != 0 && SDL_memcmp(in, "GIF89a", 6) != 0)) {
        return false;
    }
    uint32_t sw = le16(in + 6);
    uint32_t sh = le16(in + 8);
    uint8_t packed = in[10];
    size_t pos = 13;
    if (packed & 0x80) {
        size_t gct = (size_t)3 * ((size_t)2 << (packed & 7));
        if (len - pos < gct) {
            return false;
        }
        pos += gct;
    }
    /* Walk the blocks to the first image descriptor. Extensions are skipped by their sub-blocks. */
    for (;;) {
        if (pos >= len) {
            return false;
        }
        uint8_t b = in[pos++];
        if (b == 0x2C) {
            /* Image descriptor: left, top, width, height (2 bytes each). */
            if (len - pos < 9) {
                return false;
            }
            uint32_t iw = le16(in + pos + 4);
            uint32_t ih = le16(in + pos + 6);
            if (!embi_size_ok(sw, sh, max_pixels) || !embi_size_ok(iw, ih, max_pixels)) {
                return false;
            }
            *w = sw;
            *h = sh;
            return true;
        }
        if (b != 0x21) {
            return false; /* trailer or garbage before any image */
        }
        if (pos >= len) {
            return false;
        }
        uint8_t label = in[pos++];
        /* SDL_image's GIF header parser takes a NETSCAPE2.0 application block followed by exactly one
         * data sub-block and a terminator, and refuses anything else. Mirror that, so a header it
         * refuses never reaches it (see decode_gif). */
        if (label == 0xFF && len - pos >= 12 && in[pos] == 11 &&
            SDL_memcmp(in + pos + 1, "NETSCAPE2.0", 11) == 0) {
            pos += 12;
            if (pos >= len) {
                return false;
            }
            size_t n = in[pos++];
            if (len - pos < n + 1) {
                return false;
            }
            pos += n;
            if (in[pos++] != 0) {
                return false;
            }
            continue;
        }
        /* A graphic control extension has a fixed 4 byte block (GIF89a, 23.c.iii). */
        if (label == 0xF9 && (pos >= len || in[pos] != 4)) {
            return false;
        }
        /* An application extension has a fixed 11 byte block (GIF89a, 26.c.iii). With a block size of 0
         * SDL_image's header walker reads the byte after it as the first sub-block size while its frame
         * reader takes the 0 as the terminator, so the two disagree about where the extension ends
         * (the header walker then fails, and the fallback leaks, or the frame reader lands on another
         * image descriptor). Refuse it. */
        if (label == 0xFF && pos < len && in[pos] == 0) {
            return false;
        }
        for (;;) {
            if (pos >= len) {
                return false;
            }
            uint8_t n = in[pos++];
            if (n == 0) {
                break;
            }
            if (len - pos < n) {
                return false;
            }
            pos += n;
        }
    }
}

/* Converts an SDL surface to straight RGBA8 in out, and destroys the surface on every path. */
static bool surface_to_rgba(SDL_Surface *src, const char *type, embi_pixels *out)
{
    SDL_Surface *rgba = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(src);
    if (!rgba) {
        embi_log_debug("image: %s convert to RGBA32 failed: %s", type, SDL_GetError());
        return false;
    }
    uint32_t w = (uint32_t)rgba->w;
    uint32_t h = (uint32_t)rgba->h;
    if (!embi_size_ok(w, h, (uint64_t)EMBI_IMAGE_MAX_DIM * EMBI_IMAGE_MAX_DIM)) {
        SDL_DestroySurface(rgba);
        embi_log_debug("image: %s decoded to an unusable size %ux%u", type, w, h);
        return false;
    }
    uint8_t *px = embi_alloc((size_t)w * h * 4);
    const uint8_t *srcpx = rgba->pixels;
    for (uint32_t y = 0; y < h; y++) {
        SDL_memcpy(px + (size_t)y * w * 4, srcpx + (size_t)y * (size_t)rgba->pitch, (size_t)w * 4);
    }
    SDL_DestroySurface(rgba);
    out->w = w;
    out->h = h;
    out->px = px;
    return true;
}

/* GIF, first frame only, through SDL_image's animation decoder. IMG_LoadTyped_IO is not used for
 * GIF: when the GIF animation decoder cannot parse a header, SDL_image falls back to a
 * single-frame decoder whose first frame calls IMG_LoadTyped_IO("gif") on the same stream, which
 * creates the same decoder again and falls back again, without end (a stack overflow). The same
 * fallback also leaks the GIF decoder's context (about 66 KB), because the single-frame decoder
 * overwrites it (SDL_image 3.4.8 and main as of 2026-11-08). So a header SDL_image would refuse
 * must never reach it: embi_gif_probe mirrors its header checks (signature, screen descriptor,
 * global color table, and the extension walk up to the first image descriptor, including its
 * one-sub-block rule for NETSCAPE2.0). The only refusal left is a failed canvas allocation
 * (out of memory). As a backstop against the recursion, the GIF decoder sets the loop-count
 * property only after its header parses, and this path never sets the ignore-properties flag, so
 * a decoder without that property is the fallback and is refused before any frame is requested. */
static bool decode_gif(const uint8_t *in, size_t len, embi_pixels *out)
{
    SDL_IOStream *io = SDL_IOFromConstMem(in, len);
    if (!io) {
        embi_log_debug("image: SDL_IOFromConstMem failed: %s", SDL_GetError());
        return false;
    }
    /* closeio: the decoder closes io when it is closed, and on a failed creation. */
    IMG_AnimationDecoder *dec = IMG_CreateAnimationDecoder_IO(io, true, "gif");
    if (!dec) {
        embi_log_debug("image: GIF decoder creation failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_HasProperty(IMG_GetAnimationDecoderProperties(dec), IMG_PROP_METADATA_LOOP_COUNT_NUMBER)) {
        embi_log_debug("image: GIF header refused by SDL_image");
        IMG_CloseAnimationDecoder(dec);
        return false;
    }
    SDL_Surface *frame = nullptr;
    bool got = IMG_GetAnimationDecoderFrame(dec, &frame, nullptr);
    IMG_CloseAnimationDecoder(dec);
    if (!got || !frame) {
        embi_log_debug("image: GIF decode failed: %s", SDL_GetError());
        if (frame) {
            SDL_DestroySurface(frame);
        }
        return false;
    }
    return surface_to_rgba(frame, "GIF", out);
}

bool embi_sdl_decode(const uint8_t *in, size_t len, embi_image_format fmt, embi_pixels *out)
{
    if (fmt == EMBI_IMAGE_FMT_GIF) {
        return decode_gif(in, len, out);
    }
    SDL_IOStream *io = SDL_IOFromConstMem(in, len);
    if (!io) {
        embi_log_debug("image: SDL_IOFromConstMem failed: %s", SDL_GetError());
        return false;
    }
    /* IMG_LoadTyped_IO closes io (closeio is true). JPEG does not fall back into itself. */
    SDL_Surface *src = IMG_LoadTyped_IO(io, true, "JPG");
    if (!src) {
        embi_log_debug("image: JPG decode failed: %s", SDL_GetError());
        return false;
    }
    return surface_to_rgba(src, "JPG", out);
}

/* All the frames of an animated GIF through the same animation decoder as decode_gif (same header guard against the
 * SDL_image recursion: embi_gif_probe has run on these bytes before). Frames are full-canvas composited surfaces. A
 * delay of 10 ms or less becomes 100 ms, as browsers do. Returns false (nothing allocated) for a single-frame image, an
 * error, frames of different sizes, or more than max_bytes of frames: the caller keeps the first frame as a still. */
bool embi_gif_decode_frames(const uint8_t *in, size_t len, uint64_t max_pixels, size_t max_bytes, embi_gif_frames *out)
{
    SDL_IOStream *io = SDL_IOFromConstMem(in, len);
    if (!io) {
        return false;
    }
    IMG_AnimationDecoder *dec = IMG_CreateAnimationDecoder_IO(io, true, "gif");
    if (!dec) {
        return false;
    }
    SDL_PropertiesID props = IMG_GetAnimationDecoderProperties(dec);
    if (!SDL_HasProperty(props, IMG_PROP_METADATA_LOOP_COUNT_NUMBER)) {
        IMG_CloseAnimationDecoder(dec);
        return false;
    }
    int64_t loops = SDL_GetNumberProperty(props, IMG_PROP_METADATA_LOOP_COUNT_NUMBER, 1);
    embi_gif_frames f;
    SDL_memset(&f, 0, sizeof f);
    size_t cap = 0;
    bool ok = true;
    for (;;) {
        SDL_Surface *s = nullptr;
        Uint64 dur = 0;
        SDL_ClearError();
        if (!IMG_GetAnimationDecoderFrame(dec, &s, &dur) || !s) {
            const char *err = SDL_GetError();
            ok = err == nullptr || err[0] == '\0'; /* an empty error: no more frames */
            break;
        }
        embi_pixels p = {0};
        if (!surface_to_rgba(s, "GIF", &p)) { /* destroys s */
            ok = false;
            break;
        }
        size_t fb = (size_t)p.w * p.h * 4;
        if ((f.n > 0 && (p.w != f.w || p.h != f.h)) || !embi_size_ok(p.w, p.h, max_pixels) ||
            (size_t)(f.n + 1) * fb > max_bytes || f.n >= 4096) {
            embi_pixels_free(&p);
            ok = false;
            break;
        }
        if (f.n == cap) {
            size_t nc = cap ? cap * 2 : 4;
            f.px = embi_realloc(f.px, cap * fb, nc * fb);
            f.delay_ms = embi_realloc(f.delay_ms, cap * sizeof *f.delay_ms, nc * sizeof *f.delay_ms);
            cap = nc;
        }
        f.w = p.w;
        f.h = p.h;
        SDL_memcpy(f.px + (size_t)f.n * fb, p.px, fb);
        f.delay_ms[f.n] = dur <= 10 ? 100u : (dur > 3600000u ? 3600000u : (uint32_t)dur);
        f.n++;
        embi_pixels_free(&p);
    }
    IMG_CloseAnimationDecoder(dec);
    if (!ok || f.n < 2) {
        if (f.px) {
            embi_free(f.px, cap * (size_t)f.w * f.h * 4);
            embi_free(f.delay_ms, cap * sizeof *f.delay_ms);
        }
        return false;
    }
    f.cap = (uint32_t)cap;
    f.loops = loops < 0 ? 0u : (loops > 65535 ? 65535u : (uint32_t)loops);
    *out = f;
    return true;
}

void embi_gif_frames_free(embi_gif_frames *f)
{
    if (f->px) {
        embi_free(f->px, (size_t)f->cap * f->w * f->h * 4);
        embi_free(f->delay_ms, (size_t)f->cap * sizeof *f->delay_ms);
    }
    SDL_memset(f, 0, sizeof *f);
}
