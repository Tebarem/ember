#include "image/internal.h"

#include <SDL3/SDL_stdinc.h>

static uint16_t rd16(const uint8_t *p, bool le)
{
    return le ? (uint16_t)(p[0] | ((uint32_t)p[1] << 8)) : (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p, bool le)
{
    if (le) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Reads IFD0 of a TIFF block (the Exif payload after "Exif\0\0") of n bytes. Every offset is
 * checked against n, so a bad IFD offset or entry count stops the walk instead of reading past
 * the segment. */
static int tiff_orientation(const uint8_t *t, size_t n)
{
    if (n < 8) {
        return 1;
    }
    bool le;
    if (t[0] == 'I' && t[1] == 'I') {
        le = true;
    } else if (t[0] == 'M' && t[1] == 'M') {
        le = false;
    } else {
        return 1;
    }
    if (rd16(t + 2, le) != 42) {
        return 1;
    }
    uint32_t ifd = rd32(t + 4, le);
    if (ifd > n || n - ifd < 2) {
        return 1;
    }
    uint16_t count = rd16(t + ifd, le);
    size_t p = (size_t)ifd + 2;
    for (uint32_t i = 0; i < count && n - p >= 12; i++, p += 12) {
        const uint8_t *e = t + p;
        if (rd16(e, le) != 0x0112) {
            continue; /* Orientation */
        }
        /* Type 3 (SHORT), one value stored in the first two bytes of the value field. */
        if (rd16(e + 2, le) != 3 || rd32(e + 4, le) < 1) {
            return 1;
        }
        uint16_t v = rd16(e + 8, le);
        return (v >= 1 && v <= 8) ? v : 1;
    }
    return 1;
}

int embi_jpeg_orientation(const uint8_t *in, size_t len)
{
    if (len < 4 || in[0] != 0xFF || in[1] != 0xD8) {
        return 1;
    }
    size_t pos = 2;
    for (;;) {
        uint8_t m = 0;
        const uint8_t *d = nullptr;
        size_t dl = 0;
        if (!embi_jpeg_next_segment(in, len, &pos, &m, &d, &dl)) {
            return 1;
        }
        if (m == 0xE1 && dl >= 6 && SDL_memcmp(d, "Exif\0\0", 6) == 0) {
            return tiff_orientation(d + 6, dl - 6);
        }
    }
}

/* The orientation of a PNG: the EXIF block of an eXIf chunk that comes before the first IDAT (a later one is ignored, as in
 * the PNG 3 specification's reading order for decoders). 1 when there is none. */
int embi_png_orientation(const uint8_t *in, size_t len)
{
    size_t pos = 8; /* after the signature */
    while (pos + 12 <= len) {
        uint32_t n = rd32(in + pos, false);
        const uint8_t *type = in + pos + 4;
        if (SDL_memcmp(type, "IDAT", 4) == 0 || SDL_memcmp(type, "IEND", 4) == 0) {
            return 1;
        }
        if ((size_t)n > len - pos - 12) {
            return 1;
        }
        if (SDL_memcmp(type, "eXIf", 4) == 0) {
            return tiff_orientation(in + pos + 8, n);
        }
        pos += 12 + (size_t)n;
    }
    return 1;
}

/* Swaps two 4-byte RGBA pixels. */
static void swap_px(uint8_t *a, uint8_t *b)
{
    for (int i = 0; i < 4; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* Orientations 2-4 keep the size, so they run in place: no second buffer at the pixel cap. */
static void flip_in_place(embi_pixels *p, int o)
{
    uint32_t w = p->w;
    uint32_t h = p->h;
    uint8_t *px = p->px;
    if (o == 2) { /* mirror horizontal: reverse each row */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = px + (size_t)y * w * 4;
            for (uint32_t i = 0, j = w - 1; i < j; i++, j--) {
                swap_px(row + (size_t)i * 4, row + (size_t)j * 4);
            }
        }
    } else if (o == 4) { /* mirror vertical: swap rows top and bottom */
        for (uint32_t y = 0, z = h - 1; y < z; y++, z--) {
            uint8_t *a = px + (size_t)y * w * 4;
            uint8_t *b = px + (size_t)z * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                swap_px(a + (size_t)x * 4, b + (size_t)x * 4);
            }
        }
    } else { /* 3, rotate 180: reverse the whole pixel array */
        size_t n = (size_t)w * h;
        for (size_t i = 0, j = n - 1; i < j; i++, j--) {
            swap_px(px + i * 4, px + j * 4);
        }
    }
}

bool embi_orient_pixels(embi_pixels *p, int o)
{
    if (o <= 1 || o > 8) {
        return true;
    }
    if (o <= 4) {
        flip_in_place(p, o);
        return true;
    }
    uint32_t w = p->w;
    uint32_t h = p->h;
    /* Orientations 5-8 transpose, so the output has the sides swapped and needs a new buffer. */
    uint32_t ow = h;
    uint32_t oh = w;
    uint8_t *dst = embi_alloc((size_t)ow * oh * 4);
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            /* Output pixel (x, y) reads source pixel (sx, sy). */
            uint32_t sx = 0;
            uint32_t sy = 0;
            switch (o) {
            case 5: sx = y; sy = x; break;                 /* transpose */
            case 6: sx = y; sy = h - 1 - x; break;         /* rotate 90 clockwise */
            case 7: sx = w - 1 - y; sy = h - 1 - x; break; /* transverse */
            default: sx = w - 1 - y; sy = x; break;        /* 8: rotate 270 clockwise */
            }
            const uint8_t *s = p->px + ((size_t)sy * w + sx) * 4;
            uint8_t *d = dst + ((size_t)y * ow + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    embi_pixels_free(p);
    p->px = dst;
    p->w = ow;
    p->h = oh;
    return true;
}
