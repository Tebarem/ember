/*
 * EXIF orientation: the APP1 walk and the IFD0 read on hand-built streams (both byte orders;
 * out-of-range values, wrong types, offsets and counts that run past the segment, truncated
 * segments, missing tags), and the pixel transforms for orientations 2-8 checked against forward
 * coordinate maps written separately from the implementation.
 */
#include "base/base.h"
#include "emb_test.h"
#include "image/image.h"
#include "image/internal.h"

#include <SDL3/SDL_stdinc.h>

/* Writes a big- or little-endian 16-bit value. */
static void put16(uint8_t *p, bool le, uint16_t v)
{
    if (le) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    } else {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    }
}

static void put32(uint8_t *p, bool le, uint32_t v)
{
    if (le) {
        for (int i = 0; i < 4; i++) {
            p[i] = (uint8_t)(v >> (8 * i));
        }
    } else {
        for (int i = 0; i < 4; i++) {
            p[i] = (uint8_t)(v >> (8 * (3 - i)));
        }
    }
}

/* Builds SOI, an APP1 segment with "Exif\0\0" and the given TIFF block, then EOI. The segment
 * length field is set from the block. Returns the total length. */
static size_t wrap_exif(uint8_t *out, size_t cap, const uint8_t *tiff, size_t tlen)
{
    size_t total = 2 + 2 + 2 + 6 + tlen + 2;
    if (total > cap) {
        return 0;
    }
    size_t n = 0;
    out[n++] = 0xFF;
    out[n++] = 0xD8;
    out[n++] = 0xFF;
    out[n++] = 0xE1;
    put16(out + n, false, (uint16_t)(2 + 6 + tlen));
    n += 2;
    SDL_memcpy(out + n, "Exif\0\0", 6);
    n += 6;
    SDL_memcpy(out + n, tiff, tlen);
    n += tlen;
    out[n++] = 0xFF;
    out[n++] = 0xD9;
    return n;
}

/* A TIFF block with IFD0 at offset 8 holding one entry (tag, type, count, value). */
static size_t one_entry_tiff(uint8_t *t, bool le, uint16_t tag, uint16_t type, uint32_t count,
                             uint16_t value)
{
    t[0] = le ? 'I' : 'M';
    t[1] = le ? 'I' : 'M';
    put16(t + 2, le, 42);
    put32(t + 4, le, 8);
    put16(t + 8, le, 1);
    put16(t + 10, le, tag);
    put16(t + 12, le, type);
    put32(t + 14, le, count);
    put16(t + 18, le, value);
    put16(t + 20, le, 0);
    put32(t + 22, le, 0); /* next IFD */
    return 26;
}

EMB_TEST(exif_orientation_both_byte_orders)
{
    size_t live0 = embi_mem_live();
    uint8_t t[64];
    uint8_t j[128];
    for (int le = 0; le < 2; le++) {
        for (uint16_t v = 1; v <= 8; v++) {
            size_t tl = one_entry_tiff(t, le != 0, 0x0112, 3, 1, v);
            size_t n = wrap_exif(j, sizeof j, t, tl);
            EMB_CHECK(n > 0);
            EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), v);
        }
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(exif_missing_or_out_of_range_is_identity)
{
    size_t live0 = embi_mem_live();
    uint8_t t[64];
    uint8_t j[128];
    size_t tl;
    size_t n;

    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 0);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 9);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* Another tag than Orientation (0x010F, Make) carries no rotation. */
    tl = one_entry_tiff(t, true, 0x010F, 3, 1, 6);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* Wrong type (LONG instead of SHORT). */
    tl = one_entry_tiff(t, true, 0x0112, 4, 1, 6);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* No count. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 0, 6);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* Not a JPEG, or no Exif segment at all. */
    static const uint8_t plain[] = {0xFF, 0xD8, 0xFF, 0xD9};
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(plain, sizeof plain), 1);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation((const uint8_t *)"GIF89a", 6), 1);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(exif_malformed_blocks_are_bounded)
{
    size_t live0 = embi_mem_live();
    uint8_t t[64];
    uint8_t j[128];
    size_t tl;
    size_t n;

    /* Bad byte order and bad magic. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    t[0] = 'X';
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    put16(t + 2, true, 43);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* IFD offset past the end of the block. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    put32(t + 4, true, 0x10000);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* IFD offset at the very end (no room for the count). */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    put32(t + 4, true, (uint32_t)tl - 1);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* An entry count of 0xFFFF with one entry present: the walk stops at the block end. */
    tl = one_entry_tiff(t, true, 0x010F, 3, 1, 6);
    put16(t + 8, true, 0xFFFF);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* The Orientation entry itself is cut off by the end of the block. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    n = wrap_exif(j, sizeof j, t, tl - 14);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* A segment length that runs past the file ends the walk. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    n = wrap_exif(j, sizeof j, t, tl);
    put16(j + 4, false, 0xFFF0);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n), 1);

    /* Truncated stream: the file ends inside the APP1 segment. */
    tl = one_entry_tiff(t, true, 0x0112, 3, 1, 6);
    n = wrap_exif(j, sizeof j, t, tl);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, n - 10), 1);
    EMB_CHECK_EQ_INT(embi_jpeg_orientation(j, 3), 1);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Forward maps for orientations 2-8 on a w x h source, as coordinates of where source pixel
 * (sx, sy) lands in the oriented image. These are independent of the inverse maps used by
 * embi_orient_pixels. */
static void forward_map(int o, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy, uint32_t *dx,
                        uint32_t *dy)
{
    switch (o) {
    case 2: *dx = w - 1 - sx; *dy = sy; break;
    case 3: *dx = w - 1 - sx; *dy = h - 1 - sy; break;
    case 4: *dx = sx; *dy = h - 1 - sy; break;
    case 5: *dx = sy; *dy = sx; break;
    case 6: *dx = h - 1 - sy; *dy = sx; break;
    case 7: *dx = h - 1 - sy; *dy = w - 1 - sx; break;
    default: *dx = sy; *dy = w - 1 - sx; break; /* 8 */
    }
}

EMB_TEST(exif_orient_pixels_match_forward_maps)
{
    size_t live0 = embi_mem_live();
    const uint32_t w = 5;
    const uint32_t h = 3;
    for (int o = 2; o <= 8; o++) {
        embi_pixels p = {0};
        p.w = w;
        p.h = h;
        p.px = embi_alloc((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *q = p.px + ((size_t)y * w + x) * 4;
                q[0] = (uint8_t)x;
                q[1] = (uint8_t)y;
                q[2] = (uint8_t)(o * 16 + x + y);
                q[3] = 255;
            }
        }
        EMB_CHECK(embi_orient_pixels(&p, o));
        bool swap = o >= 5;
        EMB_CHECK_EQ_INT(p.w, swap ? h : w);
        EMB_CHECK_EQ_INT(p.h, swap ? w : h);
        for (uint32_t sy = 0; sy < h; sy++) {
            for (uint32_t sx = 0; sx < w; sx++) {
                uint32_t dx = 0;
                uint32_t dy = 0;
                forward_map(o, w, h, sx, sy, &dx, &dy);
                const uint8_t *q = p.px + ((size_t)dy * p.w + dx) * 4;
                EMB_CHECK_EQ_INT(q[0], sx);
                EMB_CHECK_EQ_INT(q[1], sy);
                EMB_CHECK_EQ_INT(q[2], (uint8_t)(o * 16 + sx + sy));
            }
        }
        embi_pixels_free(&p);
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Orientation 1 and values outside 1-8 change nothing (same size and same pixels). */
EMB_TEST(exif_orient_identity_leaves_pixels)
{
    size_t live0 = embi_mem_live();
    embi_pixels p = {0};
    p.w = 2;
    p.h = 2;
    p.px = embi_alloc(16);
    for (int i = 0; i < 16; i++) {
        p.px[i] = (uint8_t)(i * 7);
    }
    EMB_CHECK(embi_orient_pixels(&p, 1));
    EMB_CHECK(embi_orient_pixels(&p, 0));
    EMB_CHECK(embi_orient_pixels(&p, 9));
    EMB_CHECK_EQ_INT(p.w, 2);
    EMB_CHECK_EQ_INT(p.h, 2);
    EMB_CHECK_EQ_INT(p.px[5], 35);
    embi_pixels_free(&p);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
