/*
 * PNG decoder: synthetic streams that exercise each feature in isolation (filters, Adam7, bit
 * depths and scaling, PLTE/tRNS, CRC rules, IDAT splitting, truncation, limits, premultiplication).
 * The streams are built here: stored (uncompressed) deflate blocks, with the filters applied by
 * the encoder below. PngSuite (test_image_png_suite.c) covers the real files.
 */
#include "emb_test.h"

#include "base/base.h"
#include "image/png.h"

#include <SDL3/SDL_stdinc.h>

/* ------------------------------------------------------------------ byte buffer (test only) */

typedef struct tb {
    uint8_t *p;
    size_t n;
    size_t cap;
} tb;

static void tb_put(tb *b, const void *src, size_t n)
{
    if (n == 0) {
        return;
    }
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->n + n) {
            nc *= 2;
        }
        b->p = SDL_realloc(b->p, nc);
        b->cap = nc;
    }
    SDL_memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void tb_u8(tb *b, uint8_t v)
{
    tb_put(b, &v, 1);
}

static void tb_be32(tb *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    tb_put(b, t, 4);
}

static void tb_free(tb *b)
{
    SDL_free(b->p);
    *b = (tb){0};
}

static tb tb_copy(const uint8_t *p, size_t n)
{
    tb b = {0};
    tb_put(&b, p, n);
    return b;
}

/* ------------------------------------------------------------------ encoder (test only) */

static uint32_t t_crc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void t_chunk(tb *o, const char *type, const uint8_t *data, size_t n)
{
    tb_be32(o, (uint32_t)n);
    size_t start = o->n;
    tb_put(o, type, 4);
    tb_put(o, data, n);
    uint32_t crc = t_crc(o->p + start, 4 + n);
    tb_be32(o, crc);
}

/* zlib stream made of stored blocks (RFC 1950 header, RFC 1951 BTYPE=00, Adler-32 trailer). */
static void t_zlib(tb *z, const uint8_t *raw, size_t n)
{
    uint8_t hdr[2] = {0x78, 0x01};
    tb_put(z, hdr, 2);
    size_t pos = 0;
    do {
        size_t take = n - pos > 65535 ? 65535 : n - pos;
        bool last = pos + take == n;
        uint8_t bh[5] = {last ? 1u : 0u, (uint8_t)take, (uint8_t)(take >> 8),
                         (uint8_t)~take, (uint8_t)((~take) >> 8)};
        tb_put(z, bh, 5);
        tb_put(z, raw + pos, take);
        pos += take;
    } while (pos < n);
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    tb_be32(z, (b << 16) | a);
}

static uint8_t t_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    return (uint8_t)(pb <= pc ? b : c);
}

/* Filters one row with filter type ft. prev is nullptr for the first row of a pass. */
static void t_filter_row(uint8_t ft, const uint8_t *cur, const uint8_t *prev, size_t n, size_t bpp,
                         uint8_t *out)
{
    out[0] = ft;
    for (size_t i = 0; i < n; i++) {
        int a = i >= bpp ? cur[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        int x = cur[i];
        int v = x;
        switch (ft) {
        case 0:
            v = x;
            break;
        case 1:
            v = x - a;
            break;
        case 2:
            v = x - b;
            break;
        case 3:
            v = x - ((a + b) >> 1);
            break;
        default:
            v = x - t_paeth(a, b, c);
            break;
        }
        out[1 + i] = (uint8_t)v;
    }
}

static const uint8_t kT_Pass[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                      {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

/* Filtered scanlines for a w x h image of 8-bit samples (ch per pixel), Adam7 when il. The
 * filter cycles through all five types so every decoder path runs. */
static tb t_scanlines_8(const uint8_t *px, uint32_t w, uint32_t h, unsigned ch, bool il)
{
    tb raw = {0};
    int np = il ? 7 : 1;
    for (int p = 0; p < np; p++) {
        uint32_t x0 = il ? kT_Pass[p][0] : 0;
        uint32_t y0 = il ? kT_Pass[p][1] : 0;
        uint32_t dx = il ? kT_Pass[p][2] : 1;
        uint32_t dy = il ? kT_Pass[p][3] : 1;
        uint32_t pw = w > x0 ? (w - x0 + dx - 1) / dx : 0;
        uint32_t ph = h > y0 ? (h - y0 + dy - 1) / dy : 0;
        if (pw == 0 || ph == 0) {
            continue;
        }
        size_t rb = (size_t)pw * ch;
        uint8_t *cur = SDL_malloc(rb);
        uint8_t *prev = SDL_malloc(rb);
        uint8_t *enc = SDL_malloc(rb + 1);
        for (uint32_t r = 0; r < ph; r++) {
            uint32_t y = y0 + r * dy;
            for (uint32_t j = 0; j < pw; j++) {
                uint32_t x = x0 + j * dx;
                SDL_memcpy(cur + (size_t)j * ch, px + ((size_t)y * w + x) * ch, ch);
            }
            uint8_t ft = (uint8_t)((r + (uint32_t)p * 3u) % 5u);
            t_filter_row(ft, cur, r ? prev : nullptr, rb, ch, enc);
            tb_put(&raw, enc, rb + 1);
            SDL_memcpy(prev, cur, rb);
        }
        SDL_free(cur);
        SDL_free(prev);
        SDL_free(enc);
    }
    return raw;
}

typedef struct t_spec {
    uint32_t w;
    uint32_t h;
    uint8_t depth;
    uint8_t ctype;
    uint8_t il;
} t_spec;

static void t_put_sig(tb *o)
{
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    tb_put(o, sig, 8);
}

static void t_put_ihdr(tb *o, const t_spec *s)
{
    uint8_t d[13];
    for (int i = 0; i < 4; i++) {
        d[i] = (uint8_t)(s->w >> (24 - 8 * i));
        d[4 + i] = (uint8_t)(s->h >> (24 - 8 * i));
    }
    d[8] = s->depth;
    d[9] = s->ctype;
    d[10] = 0;
    d[11] = 0;
    d[12] = s->il;
    t_chunk(o, "IHDR", d, 13);
}

static void t_put_idat(tb *o, const uint8_t *z, size_t zn, size_t split)
{
    if (split == 0) {
        split = zn ? zn : 1;
    }
    size_t pos = 0;
    do {
        size_t take = zn - pos > split ? split : zn - pos;
        t_chunk(o, "IDAT", z + pos, take);
        pos += take;
    } while (pos < zn);
}

/* A complete file: signature, IHDR, optional PLTE and tRNS, IDAT (split into pieces when split is
 * nonzero), IEND. */
static tb t_file(const t_spec *s, const tb *raw, size_t split, const tb *plte, const tb *trns)
{
    tb o = {0};
    tb z = {0};
    t_put_sig(&o);
    t_put_ihdr(&o, s);
    if (plte) {
        t_chunk(&o, "PLTE", plte->p, plte->n);
    }
    if (trns) {
        t_chunk(&o, "tRNS", trns->p, trns->n);
    }
    t_zlib(&z, raw->p, raw->n);
    t_put_idat(&o, z.p, z.n, split);
    t_chunk(&o, "IEND", nullptr, 0);
    tb_free(&z);
    return o;
}

/* Packs n samples of the given depth into one row (MSB first; 16-bit big endian). */
static tb t_pack_row(const uint32_t *v, size_t n, unsigned depth)
{
    tb row = {0};
    size_t nb = (n * depth + 7) / 8;
    uint8_t *tmp = SDL_calloc(1, nb ? nb : 1);
    for (size_t i = 0; i < n; i++) {
        if (depth == 16) {
            tmp[2 * i] = (uint8_t)(v[i] >> 8);
            tmp[2 * i + 1] = (uint8_t)v[i];
        } else {
            size_t bit = i * depth;
            unsigned shift = 8u - depth - (unsigned)(bit & 7u);
            tmp[bit >> 3] |= (uint8_t)((v[i] & ((1u << depth) - 1u)) << shift);
        }
    }
    tb_put(&row, tmp, nb);
    SDL_free(tmp);
    return row;
}

/* Finds the first chunk of this type. Returns its offset, or SIZE_MAX. */
static size_t t_find(const tb *f, const char *type)
{
    size_t pos = 8;
    while (pos + 12 <= f->n) {
        uint32_t len = ((uint32_t)f->p[pos] << 24) | ((uint32_t)f->p[pos + 1] << 16) |
                       ((uint32_t)f->p[pos + 2] << 8) | f->p[pos + 3];
        if (SDL_memcmp(f->p + pos + 4, type, 4) == 0) {
            return pos;
        }
        pos += 12 + len;
    }
    return SIZE_MAX;
}

static bool t_decode(const tb *f, bool premul, embi_pixels *p)
{
    return embi_png_decode(f->p, f->n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, premul, p);
}

static const uint8_t *t_px(const embi_pixels *p, uint32_t x, uint32_t y)
{
    return p->px + ((size_t)y * p->w + x) * 4;
}

static uint8_t t_premul(uint8_t c, uint8_t a)
{
    return (uint8_t)(((uint32_t)c * a + 127u) / 255u);
}

static uint32_t t_rng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* ------------------------------------------------------------------ tests */

EMB_TEST(png_probe_and_rgba8_round_trip)
{
    uint8_t px[3 * 2 * 4];
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)(i * 37 + 11);
    }
    t_spec s = {3, 2, 8, 6, 0};
    tb raw = t_scanlines_8(px, 3, 2, 4, false);
    tb f = t_file(&s, &raw, 0, nullptr, nullptr);

    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_png_probe(f.p, f.n, &w, &h));
    EMB_CHECK_EQ_INT(w, 3);
    EMB_CHECK_EQ_INT(h, 2);

    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    EMB_CHECK_EQ_INT(p.w, 3);
    EMB_CHECK_EQ_INT(p.h, 2);
    EMB_CHECK(SDL_memcmp(p.px, px, sizeof px) == 0);
    embi_pixels_free(&p);

    EMB_REQUIRE(t_decode(&f, true, &p));
    for (uint32_t i = 0; i < 6; i++) {
        const uint8_t *q = p.px + i * 4;
        const uint8_t *s0 = px + i * 4;
        EMB_CHECK_EQ_INT(q[0], t_premul(s0[0], s0[3]));
        EMB_CHECK_EQ_INT(q[1], t_premul(s0[1], s0[3]));
        EMB_CHECK_EQ_INT(q[2], t_premul(s0[2], s0[3]));
        EMB_CHECK_EQ_INT(q[3], s0[3]);
    }
    embi_pixels_free(&p);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_premultiply_rounding_is_documented_formula)
{
    uint8_t px[4] = {255, 128, 0, 128};
    t_spec s = {1, 1, 8, 6, 0};
    tb raw = t_scanlines_8(px, 1, 1, 4, false);
    tb f = t_file(&s, &raw, 0, nullptr, nullptr);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, true, &p));
    /* (255 * 128 + 127) / 255 = 128 and (128 * 128 + 127) / 255 = 64. */
    EMB_CHECK_EQ_INT(p.px[0], 128);
    EMB_CHECK_EQ_INT(p.px[1], 64);
    EMB_CHECK_EQ_INT(p.px[2], 0);
    EMB_CHECK_EQ_INT(p.px[3], 128);
    embi_pixels_free(&p);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_all_five_filters_round_trip)
{
    uint8_t px[9 * 7 * 3];
    uint32_t seed = 0x1234567u;
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)t_rng(&seed);
    }
    t_spec s = {9, 7, 8, 2, 0};
    tb raw = t_scanlines_8(px, 9, 7, 3, false);
    tb f = t_file(&s, &raw, 0, nullptr, nullptr);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    for (uint32_t y = 0; y < 7; y++) {
        for (uint32_t x = 0; x < 9; x++) {
            const uint8_t *q = t_px(&p, x, y);
            const uint8_t *src = px + ((size_t)y * 9 + x) * 3;
            EMB_CHECK_EQ_INT(q[0], src[0]);
            EMB_CHECK_EQ_INT(q[1], src[1]);
            EMB_CHECK_EQ_INT(q[2], src[2]);
            EMB_CHECK_EQ_INT(q[3], 255);
        }
    }
    embi_pixels_free(&p);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_adam7_interlace_round_trip)
{
    static const uint32_t sizes[5][2] = {{1, 1}, {2, 3}, {8, 8}, {13, 11}, {9, 17}};
    for (size_t k = 0; k < 5; k++) {
        uint32_t w = sizes[k][0];
        uint32_t h = sizes[k][1];
        for (unsigned ch = 3; ch <= 4; ch++) {
            uint8_t *px = SDL_malloc((size_t)w * h * ch);
            uint32_t seed = 77u + (uint32_t)k * 13u + ch;
            for (size_t i = 0; i < (size_t)w * h * ch; i++) {
                px[i] = (uint8_t)t_rng(&seed);
            }
            t_spec s = {w, h, 8, (uint8_t)(ch == 3 ? 2 : 6), 1};
            tb raw = t_scanlines_8(px, w, h, ch, true);
            tb f = t_file(&s, &raw, 0, nullptr, nullptr);
            embi_pixels p = {0};
            EMB_REQUIRE(t_decode(&f, false, &p));
            EMB_REQUIRE(p.w == w && p.h == h);
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t *q = t_px(&p, x, y);
                    const uint8_t *src = px + ((size_t)y * w + x) * ch;
                    EMB_CHECK_EQ_INT(q[0], src[0]);
                    EMB_CHECK_EQ_INT(q[1], src[1]);
                    EMB_CHECK_EQ_INT(q[2], src[2]);
                    EMB_CHECK_EQ_INT(q[3], ch == 4 ? src[3] : 255);
                }
            }
            embi_pixels_free(&p);
            tb_free(&raw);
            tb_free(&f);
            SDL_free(px);
        }
    }
}

EMB_TEST(png_gray_bit_depths_scale_to_eight_bits)
{
    /* depth 1: {0, 1} -> {0, 255}; depth 2: {0..3} -> {0, 85, 170, 255}; depth 4: v -> 17 v;
     * depth 16: {0, 32768, 65535} -> {0, 128, 255}. */
    static const uint32_t v1[2] = {0, 1};
    static const uint8_t e1[2] = {0, 255};
    static const uint32_t v2[4] = {0, 1, 2, 3};
    static const uint8_t e2[4] = {0, 85, 170, 255};
    uint32_t v4[16];
    for (uint32_t i = 0; i < 16; i++) {
        v4[i] = i;
    }
    static const uint32_t v16[3] = {0, 32768, 65535};
    static const uint8_t e16[3] = {0, 128, 255};

    struct {
        unsigned depth;
        const uint32_t *v;
        size_t n;
        const uint8_t *expect; /* nullptr: expect 17 * v */
    } cases[4] = {
        {1, v1, 2, e1},
        {2, v2, 4, e2},
        {4, v4, 16, nullptr},
        {16, v16, 3, e16},
    };
    for (size_t k = 0; k < 4; k++) {
        tb row = t_pack_row(cases[k].v, cases[k].n, cases[k].depth);
        tb raw = {0};
        tb_u8(&raw, 0);
        tb_put(&raw, row.p, row.n);
        t_spec s = {(uint32_t)cases[k].n, 1, (uint8_t)cases[k].depth, 0, 0};
        tb f = t_file(&s, &raw, 0, nullptr, nullptr);
        embi_pixels p = {0};
        EMB_REQUIRE(t_decode(&f, false, &p));
        for (size_t x = 0; x < cases[k].n; x++) {
            uint8_t want = cases[k].expect ? cases[k].expect[x] : (uint8_t)(17 * cases[k].v[x]);
            const uint8_t *q = t_px(&p, (uint32_t)x, 0);
            EMB_CHECK_EQ_INT(q[0], want);
            EMB_CHECK_EQ_INT(q[1], want);
            EMB_CHECK_EQ_INT(q[2], want);
            EMB_CHECK_EQ_INT(q[3], 255);
        }
        embi_pixels_free(&p);
        tb_free(&row);
        tb_free(&raw);
        tb_free(&f);
    }
}

EMB_TEST(png_palette_with_alpha_entries)
{
    static const uint8_t plte[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    static const uint8_t trns[2] = {0, 128};
    static const uint32_t idx[4] = {0, 1, 2, 3};
    tb row = t_pack_row(idx, 4, 2);
    tb raw = {0};
    tb_u8(&raw, 0);
    tb_put(&raw, row.p, row.n);
    t_spec s = {4, 1, 2, 3, 0};
    tb pl = tb_copy(plte, sizeof plte);
    tb tr = tb_copy(trns, sizeof trns);
    tb f = t_file(&s, &raw, 0, &pl, &tr);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    EMB_CHECK(SDL_memcmp(t_px(&p, 0, 0), (uint8_t[]){10, 20, 30, 0}, 4) == 0);
    EMB_CHECK(SDL_memcmp(t_px(&p, 1, 0), (uint8_t[]){40, 50, 60, 128}, 4) == 0);
    EMB_CHECK(SDL_memcmp(t_px(&p, 2, 0), (uint8_t[]){70, 80, 90, 255}, 4) == 0);
    EMB_CHECK(SDL_memcmp(t_px(&p, 3, 0), (uint8_t[]){100, 110, 120, 255}, 4) == 0);
    embi_pixels_free(&p);

    EMB_REQUIRE(t_decode(&f, true, &p));
    EMB_CHECK_EQ_INT(t_px(&p, 1, 0)[0], t_premul(40, 128));
    embi_pixels_free(&p);

    tb_free(&pl);
    tb_free(&tr);
    tb_free(&row);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_color_key_for_gray_and_rgb)
{
    /* Gray key 5: the samples 5 become transparent. */
    uint32_t gv[3] = {5, 6, 5};
    tb row = t_pack_row(gv, 3, 8);
    tb raw = {0};
    tb_u8(&raw, 0);
    tb_put(&raw, row.p, row.n);
    t_spec s = {3, 1, 8, 0, 0};
    uint8_t key_g[2] = {0, 5};
    tb tg = tb_copy(key_g, 2);
    tb f = t_file(&s, &raw, 0, nullptr, &tg);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    EMB_CHECK_EQ_INT(t_px(&p, 0, 0)[3], 0);
    EMB_CHECK_EQ_INT(t_px(&p, 1, 0)[3], 255);
    EMB_CHECK_EQ_INT(t_px(&p, 2, 0)[3], 0);
    EMB_CHECK_EQ_INT(t_px(&p, 1, 0)[0], 6);
    embi_pixels_free(&p);
    tb_free(&tg);
    tb_free(&row);
    tb_free(&raw);
    tb_free(&f);

    /* RGB key (1, 2, 3): only the exact triple is transparent. */
    uint8_t rgb[6] = {1, 2, 3, 1, 2, 4};
    tb raw2 = {0};
    tb_u8(&raw2, 0);
    tb_put(&raw2, rgb, 6);
    t_spec s2 = {2, 1, 8, 2, 0};
    uint8_t key_c[6] = {0, 1, 0, 2, 0, 3};
    tb tc = tb_copy(key_c, 6);
    tb f2 = t_file(&s2, &raw2, 0, nullptr, &tc);
    EMB_REQUIRE(t_decode(&f2, false, &p));
    EMB_CHECK_EQ_INT(t_px(&p, 0, 0)[3], 0);
    EMB_CHECK_EQ_INT(t_px(&p, 1, 0)[3], 255);
    embi_pixels_free(&p);
    tb_free(&tc);
    tb_free(&raw2);
    tb_free(&f2);
}

EMB_TEST(png_palette_index_past_palette_fails)
{
    static const uint8_t plte[6] = {1, 1, 1, 2, 2, 2};
    static const uint32_t idx[4] = {0, 1, 2, 0};
    tb row = t_pack_row(idx, 4, 2);
    tb raw = {0};
    tb_u8(&raw, 0);
    tb_put(&raw, row.p, row.n);
    t_spec s = {4, 1, 2, 3, 0};
    tb pl = tb_copy(plte, sizeof plte);
    tb f = t_file(&s, &raw, 0, &pl, nullptr);
    embi_pixels p = {0};
    EMB_CHECK(!t_decode(&f, false, &p));
    tb_free(&pl);
    tb_free(&row);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_critical_crc_mismatch_fails_and_ancillary_is_skipped)
{
    uint32_t gv[3] = {5, 6, 5};
    tb row = t_pack_row(gv, 3, 8);
    tb raw = {0};
    tb_u8(&raw, 0);
    tb_put(&raw, row.p, row.n);
    t_spec s = {3, 1, 8, 0, 0};
    uint8_t key_g[2] = {0, 5};
    tb tg = tb_copy(key_g, 2);

    /* Corrupt tRNS CRC: the chunk is ancillary, so it is skipped and the key is ignored. */
    tb f = t_file(&s, &raw, 0, nullptr, &tg);
    size_t off = t_find(&f, "tRNS");
    EMB_REQUIRE(off != SIZE_MAX);
    f.p[off + 8 + 2 + 3] ^= 0xFF;
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    EMB_CHECK_EQ_INT(t_px(&p, 0, 0)[3], 255);
    embi_pixels_free(&p);
    tb_free(&f);

    /* Corrupt IDAT CRC: critical, so the decode fails. */
    tb g = t_file(&s, &raw, 0, nullptr, nullptr);
    size_t doff = t_find(&g, "IDAT");
    EMB_REQUIRE(doff != SIZE_MAX);
    uint32_t dlen = ((uint32_t)g.p[doff] << 24) | ((uint32_t)g.p[doff + 1] << 16) |
                    ((uint32_t)g.p[doff + 2] << 8) | g.p[doff + 3];
    g.p[doff + 8 + dlen] ^= 0x01;
    EMB_CHECK(!t_decode(&g, false, &p));
    tb_free(&g);

    /* Corrupt IHDR CRC: critical. */
    tb h = t_file(&s, &raw, 0, nullptr, nullptr);
    h.p[8 + 8 + 13] ^= 0x80;
    EMB_CHECK(!t_decode(&h, false, &p));
    tb_free(&h);

    tb_free(&tg);
    tb_free(&row);
    tb_free(&raw);
}

EMB_TEST(png_unknown_chunks_ancillary_ignored_critical_fails)
{
    uint8_t px[4] = {9, 8, 7, 6};
    t_spec s = {1, 1, 8, 6, 0};
    tb raw = t_scanlines_8(px, 1, 1, 4, false);
    tb z = {0};
    t_zlib(&z, raw.p, raw.n);

    uint8_t junk[5] = {1, 2, 3, 4, 5};
    tb ok = {0};
    t_put_sig(&ok);
    t_put_ihdr(&ok, &s);
    t_chunk(&ok, "teSt", junk, sizeof junk); /* ancillary (lowercase first letter) */
    t_put_idat(&ok, z.p, z.n, 0);
    t_chunk(&ok, "IEND", nullptr, 0);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&ok, false, &p));
    EMB_CHECK(SDL_memcmp(p.px, px, 4) == 0);
    embi_pixels_free(&p);

    tb bad = {0};
    t_put_sig(&bad);
    t_put_ihdr(&bad, &s);
    t_chunk(&bad, "TeSt", junk, sizeof junk); /* critical (uppercase first letter), unknown */
    t_put_idat(&bad, z.p, z.n, 0);
    t_chunk(&bad, "IEND", nullptr, 0);
    EMB_CHECK(!t_decode(&bad, false, &p));

    tb_free(&bad);
    tb_free(&ok);
    tb_free(&z);
    tb_free(&raw);
}

EMB_TEST(png_idat_split_across_many_chunks)
{
    uint8_t px[11 * 5 * 4];
    uint32_t seed = 99u;
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)t_rng(&seed);
    }
    t_spec s = {11, 5, 8, 6, 0};
    tb raw = t_scanlines_8(px, 11, 5, 4, false);
    tb f = t_file(&s, &raw, 3, nullptr, nullptr);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    EMB_CHECK(SDL_memcmp(p.px, px, sizeof px) == 0);
    embi_pixels_free(&p);
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_every_truncation_fails)
{
    uint8_t px[4 * 3 * 4];
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)(i * 5);
    }
    t_spec s = {4, 3, 8, 6, 1};
    tb raw = t_scanlines_8(px, 4, 3, 4, true);
    tb f = t_file(&s, &raw, 0, nullptr, nullptr);
    embi_pixels p = {0};
    EMB_REQUIRE(t_decode(&f, false, &p));
    embi_pixels_free(&p);
    for (size_t n = 0; n < f.n; n++) {
        if (embi_png_decode(f.p, n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &p)) {
            embi_pixels_free(&p);
            EMB_CHECK(false);
            break;
        }
    }
    tb_free(&raw);
    tb_free(&f);
}

EMB_TEST(png_zlib_checksum_and_missing_parts_fail)
{
    uint8_t px[4] = {1, 2, 3, 4};
    t_spec s = {1, 1, 8, 6, 0};
    tb raw = t_scanlines_8(px, 1, 1, 4, false);

    /* Adler-32 wrong. */
    tb z = {0};
    t_zlib(&z, raw.p, raw.n);
    z.p[z.n - 1] ^= 0x55;
    tb f = {0};
    t_put_sig(&f);
    t_put_ihdr(&f, &s);
    t_put_idat(&f, z.p, z.n, 0);
    t_chunk(&f, "IEND", nullptr, 0);
    embi_pixels p = {0};
    EMB_CHECK(!t_decode(&f, false, &p));
    tb_free(&f);
    tb_free(&z);

    /* No IDAT at all. */
    tb none = {0};
    t_put_sig(&none);
    t_put_ihdr(&none, &s);
    t_chunk(&none, "IEND", nullptr, 0);
    EMB_CHECK(!t_decode(&none, false, &p));
    tb_free(&none);

    /* Stream that ends before the image is complete: the raw data holds only one row of two. */
    t_spec s2 = {1, 2, 8, 6, 0};
    tb f2 = t_file(&s2, &raw, 0, nullptr, nullptr);
    EMB_CHECK(!t_decode(&f2, false, &p));
    tb_free(&f2);

    /* Extra bytes past the image: rejected by the inflater's output limit. */
    tb extra = {0};
    tb_put(&extra, raw.p, raw.n);
    tb_u8(&extra, 0);
    tb f3 = t_file(&s, &extra, 0, nullptr, nullptr);
    EMB_CHECK(!t_decode(&f3, false, &p));
    tb_free(&f3);
    tb_free(&extra);

    tb_free(&raw);
}

EMB_TEST(png_header_fields_are_validated)
{
    /* Each variant is a complete file with one bad IHDR field. */
    /* Each variant is a complete 1 x 1 file with one invalid IHDR field, so all must fail. */
    struct {
        uint8_t depth;
        uint8_t ctype;
        uint8_t comp;
        uint8_t filt;
        uint8_t il;
    } cases[] = {
        {3, 2, 0, 0, 0},  /* RGB at depth 3 */
        {8, 1, 0, 0, 0},  /* color type 1 is not defined */
        {8, 2, 1, 0, 0},  /* compression method 1 */
        {8, 2, 0, 1, 0},  /* filter method 1 */
        {8, 2, 0, 0, 2},  /* interlace method 2 */
    };
    uint8_t px[3] = {1, 2, 3};
    tb raw = t_scanlines_8(px, 1, 1, 3, false);
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        tb f = {0};
        t_put_sig(&f);
        uint8_t d[13] = {0, 0, 0, 1, 0, 0, 0, 1, cases[k].depth, cases[k].ctype,
                         cases[k].comp, cases[k].filt, cases[k].il};
        t_chunk(&f, "IHDR", d, 13);
        tb z = {0};
        t_zlib(&z, raw.p, raw.n);
        t_put_idat(&f, z.p, z.n, 0);
        t_chunk(&f, "IEND", nullptr, 0);
        embi_pixels p = {0};
        EMB_CHECK(!t_decode(&f, false, &p));
        tb_free(&z);
        tb_free(&f);
    }
    tb_free(&raw);
}

EMB_TEST(png_size_limits_and_pixel_cap)
{
    uint8_t px[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}; /* 2 x 2 RGBA */
    t_spec s = {2, 2, 8, 6, 0};
    tb raw = t_scanlines_8(px, 2, 2, 4, false);
    tb f = t_file(&s, &raw, 0, nullptr, nullptr);
    embi_pixels p = {0};
    /* The image is 2 x 2 = 4 pixels; a cap of 3 refuses it before any allocation. */
    EMB_CHECK(!embi_png_decode(f.p, f.n, 3, false, &p));
    EMB_REQUIRE(embi_png_decode(f.p, f.n, 4, false, &p));
    embi_pixels_free(&p);
    tb_free(&raw);
    tb_free(&f);

    /* Width above the decoder's dimension limit is refused from the header alone. */
    t_spec big = {16385, 1, 8, 6, 0};
    tb none = {0};
    t_put_sig(&none);
    t_put_ihdr(&none, &big);
    t_chunk(&none, "IEND", nullptr, 0);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(!embi_png_probe(none.p, none.n, &w, &h));
    EMB_CHECK(!t_decode(&none, false, &p));
    tb_free(&none);
}

EMB_TEST(png_huge_header_with_tiny_idat_fails_before_allocating)
{
    /* An 8000 x 8000 RGBA8 image needs 256 MB of pixels, yet its IDAT is a few dozen bytes, far
     * below what DEFLATE can expand (1032:1). The decoder refuses it before allocating the pixel
     * buffer, so the live and peak byte counts barely move. */
    uint8_t px[4] = {1, 2, 3, 4};
    tb raw = t_scanlines_8(px, 1, 1, 4, false);
    t_spec s = {8000, 8000, 8, 6, 0};
    tb f = {0};
    t_put_sig(&f);
    t_put_ihdr(&f, &s);
    tb z = {0};
    t_zlib(&z, raw.p, raw.n);
    t_put_idat(&f, z.p, z.n, 0);
    t_chunk(&f, "IEND", nullptr, 0);

    size_t live0 = embi_mem_live();
    size_t peak0 = embi_mem_peak();
    embi_pixels p = {0};
    EMB_CHECK(!embi_png_decode(f.p, f.n, UINT64_C(1) << 30, false, &p));
    EMB_CHECK(embi_mem_peak() - peak0 < ((size_t)1 << 20));
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
    tb_free(&z);
    tb_free(&f);
    tb_free(&raw);
}

EMB_TEST_MAIN()
