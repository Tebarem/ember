/*
 * A minimal PNG writer (R4b): RGBA8, filter 0, zlib stored blocks. There is no deflate encoder in the tree, and a canvas
 * export (toDataURL, toBlob) is not on a hot path; the files are larger than a deflated PNG but every decoder reads them.
 */
#include "canvas/canvas_int.h"

#include <string.h>

static uint32_t crc_table[256];
static bool crc_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[i] = c;
        }
        crc_ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

static void put32(embi_sb *sb, uint32_t v)
{
    embi_sb_append_char(sb, (char)(v >> 24));
    embi_sb_append_char(sb, (char)(v >> 16));
    embi_sb_append_char(sb, (char)(v >> 8));
    embi_sb_append_char(sb, (char)v);
}

/* Writes a chunk whose data is already in sb after the 8 header bytes at `start` (length and type). */
static void chunk_end(embi_sb *sb, size_t start)
{
    uint32_t len = (uint32_t)(sb->len - start - 8);
    sb->data[start] = (char)(len >> 24);
    sb->data[start + 1] = (char)(len >> 16);
    sb->data[start + 2] = (char)(len >> 8);
    sb->data[start + 3] = (char)len;
    uint32_t crc = crc32_update(0, (const uint8_t *)sb->data + start + 4, sb->len - start - 4);
    put32(sb, crc);
}

static size_t chunk_begin(embi_sb *sb, const char type[4])
{
    size_t start = sb->len;
    put32(sb, 0);
    embi_sb_append(sb, (embi_str){type, 4});
    return start;
}

void embi_png_encode_rgba(const uint8_t *px, uint32_t w, uint32_t h, embi_sb *out)
{
    static const char sig[8] = {(char)0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    embi_sb_append(out, (embi_str){sig, 8});
    size_t c = chunk_begin(out, "IHDR");
    put32(out, w);
    put32(out, h);
    embi_sb_append_char(out, 8);
    embi_sb_append_char(out, 6);
    embi_sb_append_char(out, 0);
    embi_sb_append_char(out, 0);
    embi_sb_append_char(out, 0);
    chunk_end(out, c);

    c = chunk_begin(out, "IDAT");
    size_t row = (size_t)w * 4 + 1;
    size_t total = row * h;
    embi_sb_append_char(out, 0x78);
    embi_sb_append_char(out, 0x01);
    uint32_t a = 1, b = 0; /* Adler-32 */
    size_t pos = 0;
    uint8_t *blk = embi_alloc(65535);
    while (pos < total || total == 0) {
        size_t n = total - pos;
        if (n > 65535) {
            n = 65535;
        }
        bool last = pos + n >= total;
        const char hdr[5] = {last ? 1 : 0, (char)(n & 0xFF), (char)(n >> 8), (char)(~n & 0xFF), (char)((~n >> 8) & 0xFF)};
        embi_sb_append(out, (embi_str){hdr, 5});
        /* The stored block: filter byte 0 and the row's pixels, copied a row segment at a time. */
        size_t filled = 0;
        while (filled < n) {
            size_t o = pos + filled;
            size_t col = o % row;
            if (col == 0) {
                blk[filled++] = 0;
                continue;
            }
            size_t take = row - col;
            if (take > n - filled) {
                take = n - filled;
            }
            memcpy(blk + filled, px + (o / row) * (size_t)w * 4 + (col - 1), take);
            filled += take;
        }
        for (size_t i = 0; i < n;) {
            size_t m = n - i < 5552 ? n - i : 5552; /* the most bytes before the sums can overflow 32 bits */
            for (size_t k = 0; k < m; k++) {
                a += blk[i + k];
                b += a;
            }
            a %= 65521u;
            b %= 65521u;
            i += m;
        }
        embi_sb_append(out, (embi_str){(const char *)blk, (uint32_t)n});
        pos += n;
        if (total == 0) {
            break;
        }
    }
    embi_free(blk, 65535);
    put32(out, (b << 16) | a);
    chunk_end(out, c);
    c = chunk_begin(out, "IEND");
    chunk_end(out, c);
}
