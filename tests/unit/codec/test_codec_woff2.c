#include "codec_test_util.h"

/* ------------------------------------------------------------------ glyph outline comparison */

/* Locates glyph gid in an sfnt through its head, maxp, loca and glyf tables. */
static bool glyph_at(const uint8_t *f, size_t fl, uint32_t gid, const uint8_t **g, size_t *gl)
{
    const uint8_t *head = nullptr;
    const uint8_t *maxp = nullptr;
    const uint8_t *loca = nullptr;
    const uint8_t *glyf = nullptr;
    size_t hl = 0;
    size_t ml = 0;
    size_t ll = 0;
    size_t gfl = 0;
    if (!tu_sfnt_table(f, fl, tu_tag("head"), &head, &hl) || hl < 54) {
        return false;
    }
    if (!tu_sfnt_table(f, fl, tu_tag("maxp"), &maxp, &ml) || ml < 6) {
        return false;
    }
    if (!tu_sfnt_table(f, fl, tu_tag("loca"), &loca, &ll) || !tu_sfnt_table(f, fl, tu_tag("glyf"), &glyf, &gfl)) {
        return false;
    }
    if (gid >= tu_be16(maxp + 4)) {
        return false;
    }
    uint32_t o0 = 0;
    uint32_t o1 = 0;
    if ((int16_t)tu_be16(head + 50) == 0) {
        if ((size_t)(gid + 2) * 2 > ll) {
            return false;
        }
        o0 = tu_be16(loca + 2 * (size_t)gid) * 2u;
        o1 = tu_be16(loca + 2 * (size_t)gid + 2) * 2u;
    } else {
        if ((size_t)(gid + 2) * 4 > ll) {
            return false;
        }
        o0 = tu_be32(loca + 4 * (size_t)gid);
        o1 = tu_be32(loca + 4 * (size_t)gid + 4);
    }
    if (o0 > o1 || o1 > gfl) {
        return false;
    }
    *g = glyf + o0;
    *gl = o1 - o0;
    return true;
}

/* Decodes the simple-glyph point data starting at p. Coordinates are absolute. */
static bool decode_points(const uint8_t *g, size_t gl, size_t p, uint32_t n, int32_t *xs, int32_t *ys,
                          uint8_t *on)
{
    uint8_t *fl = embi_alloc(n ? n : 1);
    bool ok = true;
    uint32_t i = 0;
    while (ok && i < n) {
        if (p >= gl) {
            ok = false;
            break;
        }
        uint8_t f = g[p++];
        fl[i++] = f;
        if (f & 8) {
            if (p >= gl) {
                ok = false;
                break;
            }
            uint8_t rep = g[p++];
            for (uint32_t r = 0; r < rep && i < n; r++) {
                fl[i++] = f;
            }
        }
    }
    int32_t x = 0;
    for (uint32_t k = 0; ok && k < n; k++) {
        uint8_t f = fl[k];
        if (f & 2) {
            if (p >= gl) {
                ok = false;
                break;
            }
            int32_t d = g[p++];
            x += (f & 16) ? d : -d;
        } else if (!(f & 16)) {
            if (p + 2 > gl) {
                ok = false;
                break;
            }
            x += (int16_t)tu_be16(g + p);
            p += 2;
        }
        xs[k] = x;
    }
    int32_t y = 0;
    for (uint32_t k = 0; ok && k < n; k++) {
        uint8_t f = fl[k];
        if (f & 4) {
            if (p >= gl) {
                ok = false;
                break;
            }
            int32_t d = g[p++];
            y += (f & 32) ? d : -d;
        } else if (!(f & 32)) {
            if (p + 2 > gl) {
                ok = false;
                break;
            }
            y += (int16_t)tu_be16(g + p);
            p += 2;
        }
        ys[k] = y;
        on[k] = f & 1;
    }
    embi_free(fl, n ? n : 1);
    return ok;
}

/* Compares two glyph records by outline: contours, instructions and point coordinates with their
 * on-curve bits. Flag encoding (repeat, short form) and trailing padding are ignored. Composite
 * glyphs are compared byte for byte, allowing zero padding at the end. */
static bool outline_equal(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    if (al == 0 || bl == 0) {
        return al == bl;
    }
    if (al < 10 || bl < 10) {
        return false;
    }
    int16_t nca = (int16_t)tu_be16(a);
    if (nca != (int16_t)tu_be16(b)) {
        return false;
    }
    if (nca < 0) {
        size_t m = al < bl ? al : bl;
        if (SDL_memcmp(a, b, m) != 0) {
            return false;
        }
        const uint8_t *rest = al < bl ? b : a;
        size_t rl = al < bl ? bl - m : al - m;
        for (size_t i = 0; i < rl; i++) {
            if (rest[i] != 0) {
                return false;
            }
        }
        return true;
    }
    uint32_t nc = (uint32_t)nca;
    size_t ea = 10 + 2 * (size_t)nc;
    if (al < ea + 2 || bl < ea + 2) {
        return false;
    }
    for (uint32_t k = 0; k < nc; k++) {
        if (tu_be16(a + 10 + 2 * (size_t)k) != tu_be16(b + 10 + 2 * (size_t)k)) {
            return false;
        }
    }
    uint32_t ila = tu_be16(a + ea);
    if (ila != tu_be16(b + ea)) {
        return false;
    }
    if (al < ea + 2 + ila || bl < ea + 2 + ila) {
        return false;
    }
    if (SDL_memcmp(a + ea + 2, b + ea + 2, ila) != 0) {
        return false;
    }
    uint32_t n = nc ? tu_be16(a + 10 + 2 * (size_t)(nc - 1)) + 1u : 0u;
    if (n == 0) {
        return true;
    }
    int32_t *axs = embi_alloc(sizeof(int32_t) * n);
    int32_t *ays = embi_alloc(sizeof(int32_t) * n);
    uint8_t *aon = embi_alloc(n);
    int32_t *bxs = embi_alloc(sizeof(int32_t) * n);
    int32_t *bys = embi_alloc(sizeof(int32_t) * n);
    uint8_t *bon = embi_alloc(n);
    bool eq = decode_points(a, al, ea + 2 + ila, n, axs, ays, aon) &&
              decode_points(b, bl, ea + 2 + ila, n, bxs, bys, bon);
    if (eq) {
        for (uint32_t k = 0; k < n; k++) {
            if (axs[k] != bxs[k] || ays[k] != bys[k] || aon[k] != bon[k]) {
                eq = false;
                break;
            }
        }
    }
    embi_free(axs, sizeof(int32_t) * n);
    embi_free(ays, sizeof(int32_t) * n);
    embi_free(aon, n);
    embi_free(bxs, sizeof(int32_t) * n);
    embi_free(bys, sizeof(int32_t) * n);
    embi_free(bon, n);
    return eq;
}

/* ------------------------------------------------------------------ table comparison */

/* Compares every table of the reference sfnt with the decoded font, byte for byte. glyf and loca
 * are compared by outline instead (WOFF 2 re-encodes them). head skips checkSumAdjustment, which
 * the decoder recomputes. Tables whose tag is in skip_tag are not required to be present. */
static void compare_font(const uint8_t *dec, size_t dl, const uint8_t *ref, size_t rl, uint32_t skip_tag)
{
    uint32_t nt = tu_be16(ref + 4);
    uint32_t expect = nt;
    if (skip_tag != 0) {
        expect--;
    }
    EMB_CHECK(tu_be16(dec + 4) == expect);
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t tag = tu_be32(ref + 12 + 16 * i);
        if (tag == skip_tag) {
            continue;
        }
        if (tag == tu_tag("glyf") || tag == tu_tag("loca")) {
            continue;
        }
        const uint8_t *rd = nullptr;
        const uint8_t *od = nullptr;
        size_t rlen = 0;
        size_t olen = 0;
        bool have_ref = tu_sfnt_table(ref, rl, tag, &rd, &rlen);
        bool have_out = tu_sfnt_table(dec, dl, tag, &od, &olen);
        EMB_CHECK(have_ref && have_out);
        if (!(have_ref && have_out)) {
            continue;
        }
        EMB_CHECK(rlen == olen);
        if (rlen != olen) {
            continue;
        }
        /* Bytes 8..11 of head (checkSumAdjustment) are recomputed, so they are skipped. */
        size_t from = tag == tu_tag("head") ? 12 : 0;
        if (tag == tu_tag("head")) {
            EMB_CHECK(rlen >= 54);
            EMB_CHECK(SDL_memcmp(rd, od, 8) == 0);
        }
        for (size_t k = from; k < rlen; k++) {
            /* head.flags bit 11 (0x0800, byte 16 mask 0x08) is set by WOFF 2.0 encoders on every
             * font, while the TTF/OTF originals leave it clear. It is masked here; see the report. */
            uint8_t mask = (tag == tu_tag("head") && k == 16) ? 0xF7u : 0xFFu;
            if (tag == tu_tag("head") && k >= 28 && k < 36) {
                continue; /* head.modified differs between the WOFF 2.0 and TTF/OTF builds */
            }
            if ((rd[k] & mask) != (od[k] & mask)) {
                SDL_Log("table %c%c%c%c differs at byte %llu of %llu (ref %u, out %u)", (char)(tag >> 24),
                        (char)(tag >> 16), (char)(tag >> 8), (char)tag, (unsigned long long)k,
                        (unsigned long long)rlen, (unsigned)rd[k], (unsigned)od[k]);
                EMB_CHECK(false);
                break;
            }
        }
    }
}

static void check_font(const char *woff2_rel, const char *ref_rel, uint32_t skip_tag, bool outlines)
{
    size_t wl = 0;
    size_t rl = 0;
    uint8_t *w = tu_load(woff2_rel, &wl);
    uint8_t *r = tu_load(ref_rel, &rl);
    EMB_REQUIRE(w != nullptr && r != nullptr);

    embi_buf out = {0};
    bool ok = embi_woff2_decode(w, wl, rl * 2, &out);
    if (!ok) {
        SDL_Log("woff2 decode failed: %s", woff2_rel);
    }
    EMB_CHECK(ok);
    if (ok) {
        compare_font(out.data, out.len, r, rl, skip_tag);
        EMB_CHECK(embi_sfnt_checksum(out.data, out.len) == 0xB1B0AFBAu);
        if (outlines) {
            uint32_t ng = 0;
            const uint8_t *maxp = nullptr;
            size_t ml = 0;
            if (tu_sfnt_table(r, rl, tu_tag("maxp"), &maxp, &ml) && ml >= 6) {
                ng = tu_be16(maxp + 4);
            }
            EMB_CHECK(ng > 0);
            for (uint32_t g = 0; g < ng; g++) {
                const uint8_t *ga = nullptr;
                const uint8_t *gb = nullptr;
                size_t gal = 0;
                size_t gbl = 0;
                bool ha = glyph_at(r, rl, g, &ga, &gal);
                bool hb = glyph_at(out.data, out.len, g, &gb, &gbl);
                EMB_CHECK(ha && hb);
                if (ha && hb) {
                    EMB_CHECK(outline_equal(ga, gal, gb, gbl));
                }
            }
        }
    }
    embi_buf_free(&out);
    SDL_free(w);
    SDL_free(r);
}

EMB_TEST(woff2_noto_sans_matches_ttf_outlines)
{
    check_font("tests/fonts/NotoSans-Regular.woff2", "tests/fonts/NotoSans-Regular.ttf", 0, true);
}

EMB_TEST(woff2_source_sans_matches_otf_minus_dsig)
{
    check_font("tests/fonts/SourceSans3-Regular.woff2", "tests/fonts/SourceSans3-Regular.otf",
               tu_tag("DSIG"), false);
}

/* ------------------------------------------------------------------ synthetic WOFF 2.0 */

static size_t b128_put(uint8_t *p, uint32_t v)
{
    uint8_t tmp[5];
    size_t n = 0;
    do {
        tmp[n++] = (uint8_t)(v & 0x7f);
        v >>= 7;
    } while (v != 0);
    for (size_t i = 0; i < n; i++) {
        p[i] = tmp[n - 1 - i] | (i + 1 < n ? 0x80 : 0);
    }
    return n;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Wraps data in one uncompressed Brotli metablock (at most 65536 bytes) and writes the stream to
 * out, which has room for at least n + 16 bytes. Returns the stream length. */
static size_t brotli_uncompressed(const uint8_t *data, size_t n, uint8_t *out)
{
    tu_bits w = {0};
    tu_put_bits(&w, 0, 1);            /* WBITS 16 */
    tu_put_bits(&w, 0, 1);            /* ISLAST = 0 */
    tu_put_bits(&w, 0, 2);            /* MNIBBLES = 4 */
    tu_put_bits(&w, (uint32_t)(n - 1), 16);
    tu_put_bits(&w, 1, 1);            /* ISUNCOMPRESSED */
    tu_bits_finish(&w);
    for (size_t i = 0; i < n; i++) {
        tu_put_bits(&w, data[i], 8);
    }
    tu_put_bits(&w, 1, 1);            /* ISLAST */
    tu_put_bits(&w, 1, 1);            /* ISLASTEMPTY */
    size_t len = tu_bits_finish(&w);
    SDL_memcpy(out, w.buf, len);
    return len;
}

/* Fixed tables used by the synthetic files: head (null transform, known index 1), maxp (known
 * index 4) and a private tag 'ABCD' (index 63 with the tag written out). */
static uint8_t g_head[54];
static const uint8_t g_maxp[6] = {0x00, 0x00, 0x50, 0x00, 0x00, 0x03};
static const uint8_t g_abcd[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

static void init_head(void)
{
    SDL_memset(g_head, 0, sizeof g_head);
    put_be32(g_head, 0x00010000u);
    for (size_t i = 12; i < sizeof g_head; i++) {
        g_head[i] = (uint8_t)(i * 3 + 1);
    }
}

EMB_TEST(woff2_synthetic_uncompressed_brotli_font)
{
    init_head();
    uint8_t data[sizeof g_head + sizeof g_maxp + sizeof g_abcd];
    SDL_memcpy(data, g_head, sizeof g_head);
    SDL_memcpy(data + sizeof g_head, g_maxp, sizeof g_maxp);
    SDL_memcpy(data + sizeof g_head + sizeof g_maxp, g_abcd, sizeof g_abcd);

    uint8_t comp[256];
    size_t cl = brotli_uncompressed(data, sizeof data, comp);

    uint8_t dir[64];
    size_t dl = 0;
    dir[dl++] = 1;                         /* head, null transform */
    dl += b128_put(dir + dl, sizeof g_head);
    dir[dl++] = 4;                         /* maxp, null transform */
    dl += b128_put(dir + dl, sizeof g_maxp);
    dir[dl++] = 63;                        /* private tag */
    SDL_memcpy(dir + dl, "ABCD", 4);
    dl += 4;
    dl += b128_put(dir + dl, sizeof g_abcd);

    uint8_t file[512];
    SDL_memset(file, 0, sizeof file);
    put_be32(file, 0x774F4632u);           /* 'wOF2' */
    put_be32(file + 4, 0x00010000u);
    size_t total = 48 + dl + cl;
    put_be32(file + 8, (uint32_t)total);
    file[12] = 0;
    file[13] = 3;                          /* numTables */
    put_be32(file + 16, (uint32_t)(12 + 16 * 3 + sizeof data + 12));
    put_be32(file + 20, (uint32_t)cl);
    file[24] = 0;
    file[25] = 1;                          /* majorVersion */
    SDL_memcpy(file + 48, dir, dl);
    SDL_memcpy(file + 48 + dl, comp, cl);

    embi_buf out = {0};
    EMB_CHECK(embi_woff2_decode(file, total, 4096, &out));
    if (out.len > 0) {
        const uint8_t *d = nullptr;
        size_t dl2 = 0;
        EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("ABCD"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_abcd && SDL_memcmp(d, g_abcd, dl2) == 0);
        EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("maxp"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_maxp && SDL_memcmp(d, g_maxp, dl2) == 0);
        EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("head"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_head && SDL_memcmp(d + 12, g_head + 12, dl2 - 12) == 0);
        EMB_CHECK(embi_sfnt_checksum(out.data, out.len) == 0xB1B0AFBAu);
    }
    /* The declared output must fit the limit. */
    embi_buf small = {0};
    EMB_CHECK(!embi_woff2_decode(file, total, 16, &small));
    EMB_CHECK(!embi_woff2_decode(file, total - 1, 4096, &small));
    embi_buf_free(&out);
    embi_buf_free(&small);
}

/* Finds a table of the font whose directory starts at font_off. TTC table offsets are absolute
 * from the start of the file, so the lookup is done on the whole buffer. */
static bool ttc_find(const uint8_t *f, size_t n, size_t font_off, uint32_t tag, const uint8_t **data, size_t *len)
{
    if (font_off > n || n - font_off < 12) {
        return false;
    }
    uint32_t nt = tu_be16(f + font_off + 4);
    for (uint32_t i = 0; i < nt; i++) {
        size_t e = font_off + 12 + 16 * (size_t)i;
        if (e > n || n - e < 16) {
            return false;
        }
        if (tu_be32(f + e) == tag) {
            size_t off = tu_be32(f + e + 8);
            size_t l = tu_be32(f + e + 12);
            if (off > n || l > n - off) {
                return false;
            }
            *data = f + off;
            *len = l;
            return true;
        }
    }
    return false;
}

EMB_TEST(woff2_synthetic_ttc_shares_head)
{
    init_head();
    uint8_t data[sizeof g_head + sizeof g_abcd];
    SDL_memcpy(data, g_head, sizeof g_head);
    SDL_memcpy(data + sizeof g_head, g_abcd, sizeof g_abcd);

    uint8_t comp[256];
    size_t cl = brotli_uncompressed(data, sizeof data, comp);

    uint8_t dir[64];
    size_t dl = 0;
    dir[dl++] = 1;                         /* head */
    dl += b128_put(dir + dl, sizeof g_head);
    dir[dl++] = 63;                        /* ABCD */
    SDL_memcpy(dir + dl, "ABCD", 4);
    dl += 4;
    dl += b128_put(dir + dl, sizeof g_abcd);

    /* Collection directory: version, numFonts = 2, font 0 = {head, ABCD}, font 1 = {head}. */
    uint8_t coll[32];
    size_t cdl = 0;
    put_be32(coll, 0x00020000u);
    cdl = 4;
    coll[cdl++] = 2;                       /* numFonts */
    coll[cdl++] = 2;                       /* font 0: numTables */
    put_be32(coll + cdl, 0x00010000u);
    cdl += 4;
    coll[cdl++] = 0;                       /* head */
    coll[cdl++] = 1;                       /* ABCD */
    coll[cdl++] = 1;                       /* font 1: numTables */
    put_be32(coll + cdl, 0x00010000u);
    cdl += 4;
    coll[cdl++] = 0;                       /* head */

    uint8_t file[512];
    SDL_memset(file, 0, sizeof file);
    size_t total = 48 + dl + cdl + cl;
    put_be32(file, 0x774F4632u);
    put_be32(file + 4, 0x74746366u);       /* flavor 'ttcf' */
    put_be32(file + 8, (uint32_t)total);
    file[13] = 2;                          /* numTables */
    put_be32(file + 20, (uint32_t)cl);
    file[25] = 1;
    SDL_memcpy(file + 48, dir, dl);
    SDL_memcpy(file + 48 + dl, coll, cdl);
    SDL_memcpy(file + 48 + dl + cdl, comp, cl);

    embi_buf out = {0};
    EMB_CHECK(embi_woff2_decode(file, total, 4096, &out));
    if (out.len >= 16) {
        EMB_CHECK(tu_be32(out.data) == 0x74746366u);
        EMB_CHECK(tu_be32(out.data + 8) == 2u);
        size_t o0 = tu_be32(out.data + 12);
        size_t o1 = tu_be32(out.data + 16);
        const uint8_t *d = nullptr;
        size_t dl2 = 0;
        EMB_CHECK(ttc_find(out.data, out.len, o0, tu_tag("ABCD"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_abcd && SDL_memcmp(d, g_abcd, dl2) == 0);
        EMB_CHECK(ttc_find(out.data, out.len, o0, tu_tag("head"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_head);
        EMB_CHECK(o1 + 6 <= out.len && tu_be16(out.data + o1 + 4) == 1);
        EMB_CHECK(ttc_find(out.data, out.len, o1, tu_tag("head"), &d, &dl2));
        EMB_CHECK(dl2 == sizeof g_head);
        /* Font 0 owns the adjustment of the shared head: its header, directory and table sums
         * (with the adjustment in place) must add up to 0xB1B0AFBA. */
        size_t nt0 = tu_be16(out.data + o0 + 4);
        uint32_t sum0 = embi_sfnt_checksum(out.data + o0, 12 + 16 * nt0);
        for (size_t k = 0; k < nt0; k++) {
            size_t e = o0 + 12 + 16 * k;
            sum0 += embi_sfnt_checksum(out.data + tu_be32(out.data + e + 8), tu_be32(out.data + e + 12));
        }
        EMB_CHECK(sum0 == 0xB1B0AFBAu);
    }
    embi_buf_free(&out);
}

/* A transformed glyf table (WOFF2 5.1) whose first nContour is 0xFFFE (-2): not a valid count. The
 * decoder must reject it without allocating for the count. */
EMB_TEST(woff2_rejects_contour_count_below_minus_one)
{
    uint8_t glyf[42];
    SDL_memset(glyf, 0, sizeof glyf);
    glyf[5] = 1;                           /* numGlyphs */
    put_be32(glyf + 8, 2);                 /* nContourStreamSize */
    put_be32(glyf + 28, 4);                /* bboxStreamSize: one bitmap word for one glyph */
    glyf[36] = 0xFF;
    glyf[37] = 0xFE;                       /* nContour = -2 */

    uint8_t comp[128];
    size_t cl = brotli_uncompressed(glyf, sizeof glyf, comp);

    uint8_t dir[32];
    size_t dl = 0;
    dir[dl++] = 10;                        /* glyf, transformed (version 0) */
    dl += b128_put(dir + dl, 16);          /* origLength */
    dl += b128_put(dir + dl, sizeof glyf); /* transformLength */
    dir[dl++] = 11;                        /* loca, transformed (version 0) */
    dl += b128_put(dir + dl, 4);           /* origLength: (1 + 1) short offsets */
    dir[dl++] = 0;                         /* transformLength 0 */

    uint8_t file[256];
    SDL_memset(file, 0, sizeof file);
    size_t total = 48 + dl + cl;
    put_be32(file, 0x774F4632u);           /* 'wOF2' */
    put_be32(file + 4, 0x00010000u);
    put_be32(file + 8, (uint32_t)total);
    file[13] = 2;                          /* numTables */
    put_be32(file + 20, (uint32_t)cl);
    file[25] = 1;
    SDL_memcpy(file + 48, dir, dl);
    SDL_memcpy(file + 48 + dl, comp, cl);

    embi_buf out = {0};
    EMB_CHECK(!embi_woff2_decode(file, total, 1u << 20, &out));
    embi_buf_free(&out);
}

/* Decodes a file, then checks that the limit equal to the output size succeeds and one byte less
 * fails. This covers the glyf rebuild and the final sfnt size (both fixtures store hmtx with the
 * null transform; the hmtx rebuild is covered by the synthetic hmtx tests below). */
static void check_exact_output_limit(const char *rel)
{
    size_t wl = 0;
    uint8_t *w = tu_load(rel, &wl);
    EMB_REQUIRE(w != nullptr);
    embi_buf full = {0};
    bool ok = embi_woff2_decode(w, wl, 1u << 26, &full);
    EMB_CHECK(ok);
    if (ok) {
        size_t n = full.len;
        embi_buf out = {0};
        EMB_CHECK(embi_woff2_decode(w, wl, n, &out));
        EMB_CHECK(out.len == n);
        embi_buf_free(&out);
        EMB_CHECK(!embi_woff2_decode(w, wl, n - 1, &out));
        EMB_CHECK(out.len == 0);
    }
    embi_buf_free(&full);
    SDL_free(w);
}

EMB_TEST(woff2_output_limit_is_exact_for_noto_sans)
{
    check_exact_output_limit("tests/fonts/NotoSans-Regular.woff2");
}

EMB_TEST(woff2_output_limit_is_exact_for_source_sans)
{
    check_exact_output_limit("tests/fonts/SourceSans3-Regular.woff2");
}

/* A collection whose first font lists the same table twice must be rejected. */
EMB_TEST(woff2_rejects_duplicate_tag_inside_one_font)
{
    init_head();
    uint8_t data[sizeof g_head];
    SDL_memcpy(data, g_head, sizeof g_head);
    uint8_t comp[128];
    size_t cl = brotli_uncompressed(data, sizeof data, comp);

    uint8_t dir[16];
    size_t dl = 0;
    dir[dl++] = 1;                         /* head */
    dl += b128_put(dir + dl, sizeof g_head);

    uint8_t coll[32];
    size_t cdl = 0;
    put_be32(coll, 0x00020000u);
    cdl = 4;
    coll[cdl++] = 1;                       /* numFonts */
    coll[cdl++] = 2;                       /* font 0: two entries, both the same table */
    put_be32(coll + cdl, 0x00010000u);
    cdl += 4;
    coll[cdl++] = 0;
    coll[cdl++] = 0;

    uint8_t file[256];
    SDL_memset(file, 0, sizeof file);
    size_t total = 48 + dl + cdl + cl;
    put_be32(file, 0x774F4632u);
    put_be32(file + 4, 0x74746366u);       /* 'ttcf' */
    put_be32(file + 8, (uint32_t)total);
    file[13] = 1;
    put_be32(file + 20, (uint32_t)cl);
    file[25] = 1;
    SDL_memcpy(file + 48, dir, dl);
    SDL_memcpy(file + 48 + dl, coll, cdl);
    SDL_memcpy(file + 48 + dl + cdl, comp, cl);

    embi_buf out = {0};
    EMB_CHECK(!embi_woff2_decode(file, total, 4096, &out));
    embi_buf_free(&out);
}

/* ------------------------------------------------------------------ generic file builder */

typedef struct w2t {
    uint8_t known;       /* known-tag index, or 63 with tag */
    const char *tag;
    uint8_t ver;         /* transform version, directory flag bits 6..7 */
    uint32_t orig;       /* origLength */
    bool has_tl;         /* a transformLength follows */
    uint32_t tl;
    const uint8_t *data; /* bytes stored in the compressed block */
    size_t dlen;
} w2t;

/* Wraps data in uncompressed Brotli metablocks of at most 65536 bytes. Returns a SDL_malloc buffer. */
static uint8_t *brotli_big(const uint8_t *data, size_t n, size_t *out_len)
{
    size_t pieces = n / 65536 + 1;
    uint8_t *o = SDL_malloc(n + 3 * pieces + 8);
    size_t pos = 0;
    size_t at = 0;
    bool first = true;
    while (at < n) {
        size_t c = n - at > 65536 ? 65536 : n - at;
        uint32_t v = ((uint32_t)(c - 1) << 3) | (1u << 19); /* ISLAST 0, MNIBBLES 4, MLEN-1, UNCOMPRESSED */
        if (first) {
            v <<= 1; /* WBITS = 16 is the single bit 0 */
            first = false;
        }
        o[pos++] = (uint8_t)v;
        o[pos++] = (uint8_t)(v >> 8);
        o[pos++] = (uint8_t)(v >> 16);
        SDL_memcpy(o + pos, data + at, c);
        pos += c;
        at += c;
    }
    if (pos == 0) {
        o[pos++] = 0x06; /* WBITS 16, ISLAST, ISLASTEMPTY */
    } else {
        o[pos++] = 0x03; /* ISLAST, ISLASTEMPTY */
    }
    *out_len = pos;
    return o;
}

static uint8_t *w2_make(const w2t *t, size_t nt, size_t *out_len)
{
    size_t total_data = 0;
    for (size_t i = 0; i < nt; i++) {
        total_data += t[i].dlen;
    }
    uint8_t *block = SDL_malloc(total_data + 1);
    size_t bl = 0;
    uint8_t *dir = SDL_malloc(nt * 20 + 16);
    size_t dl = 0;
    for (size_t i = 0; i < nt; i++) {
        dir[dl++] = (uint8_t)(t[i].known | (t[i].ver << 6));
        if (t[i].known == 63) {
            SDL_memcpy(dir + dl, t[i].tag, 4);
            dl += 4;
        }
        dl += b128_put(dir + dl, t[i].orig);
        if (t[i].has_tl) {
            dl += b128_put(dir + dl, t[i].tl);
        }
        if (t[i].dlen) {
            SDL_memcpy(block + bl, t[i].data, t[i].dlen);
        }
        bl += t[i].dlen;
    }
    size_t cl = 0;
    uint8_t *comp = brotli_big(block, bl, &cl);
    size_t total = 48 + dl + cl;
    uint8_t *file = SDL_calloc(1, total);
    put_be32(file, 0x774F4632u);
    put_be32(file + 4, 0x00010000u);
    put_be32(file + 8, (uint32_t)total);
    file[12] = (uint8_t)(nt >> 8);
    file[13] = (uint8_t)nt;
    put_be32(file + 16, (uint32_t)(12 + 16 * nt + total_data));
    put_be32(file + 20, (uint32_t)cl);
    file[25] = 1;
    SDL_memcpy(file + 48, dir, dl);
    SDL_memcpy(file + 48 + dl, comp, cl);
    SDL_free(block);
    SDL_free(dir);
    SDL_free(comp);
    *out_len = total;
    return file;
}

/* ------------------------------------------------------------------ hmtx transform (version 1) */

typedef struct hm_src {
    uint8_t *ttf;
    size_t ttf_len;
    const uint8_t *head, *hhea, *maxp, *hmtx, *loca, *glyf;
    size_t head_l, hhea_l, maxp_l, hmtx_l, loca_l, glyf_l;
    uint32_t ng, nhm;
    int32_t *xmin; /* xMin of every glyph (0 when empty) */
    int32_t *lsb;  /* the original left side bearing of every glyph */
    uint32_t *adv; /* advance width of every glyph (the last long metric repeated) */
} hm_src;

static bool hm_load(hm_src *h)
{
    h->ttf = tu_load("tests/fonts/NotoSans-Regular.ttf", &h->ttf_len);
    if (!h->ttf) {
        return false;
    }
    bool ok = tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("head"), &h->head, &h->head_l) &&
              tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("hhea"), &h->hhea, &h->hhea_l) &&
              tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("maxp"), &h->maxp, &h->maxp_l) &&
              tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("hmtx"), &h->hmtx, &h->hmtx_l) &&
              tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("loca"), &h->loca, &h->loca_l) &&
              tu_sfnt_table(h->ttf, h->ttf_len, tu_tag("glyf"), &h->glyf, &h->glyf_l);
    if (!ok) {
        return false;
    }
    h->ng = tu_be16(h->maxp + 4);
    h->nhm = tu_be16(h->hhea + 34);
    if (h->nhm == 0 || h->nhm > h->ng || h->hmtx_l < 4ull * h->nhm + 2ull * (h->ng - h->nhm)) {
        return false;
    }
    h->xmin = SDL_calloc(h->ng, sizeof(int32_t));
    h->lsb = SDL_calloc(h->ng, sizeof(int32_t));
    h->adv = SDL_calloc(h->ng, sizeof(uint32_t));
    for (uint32_t i = 0; i < h->ng; i++) {
        const uint8_t *g = nullptr;
        size_t gl = 0;
        if (!glyph_at(h->ttf, h->ttf_len, i, &g, &gl)) {
            return false;
        }
        h->xmin[i] = gl >= 10 ? (int16_t)tu_be16(g + 2) : 0;
        if (i < h->nhm) {
            h->adv[i] = tu_be16(h->hmtx + 4 * (size_t)i);
            h->lsb[i] = (int16_t)tu_be16(h->hmtx + 4 * (size_t)i + 2);
        } else {
            h->adv[i] = h->adv[h->nhm - 1];
            h->lsb[i] = (int16_t)tu_be16(h->hmtx + 4 * (size_t)h->nhm + 2 * (size_t)(i - h->nhm));
        }
    }
    return true;
}

static void hm_free(hm_src *h)
{
    SDL_free(h->xmin);
    SDL_free(h->lsb);
    SDL_free(h->adv);
    SDL_free(h->ttf);
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Builds a WOFF 2.0 file whose hmtx uses the transform with the given flags, a split at nhm long
 * metrics, and stored-size / origLength deviations. glyf and loca use the null transform, so the
 * decoder reads xMin from the original glyph data. expect_hmtx receives the table the decoder must
 * produce when the file is valid. hhea_nhm is the value written to hhea. */
static bool hm_case(const hm_src *h, uint8_t flags, uint32_t nhm, uint32_t hhea_nhm, int tl_delta,
                    int orig_delta, uint8_t **expect_hmtx, size_t *expect_len, embi_buf *out)
{
    uint32_t ng = h->ng;
    size_t tl = 1 + 2ull * nhm + ((flags & 1) ? 0 : 2ull * nhm) + ((flags & 2) ? 0 : 2ull * (ng - nhm));
    uint8_t *t = SDL_calloc(1, tl + 2);
    uint8_t *e = SDL_calloc(1, 2ull * ng + 2ull * nhm + 2);
    size_t el = 0;
    size_t p = 1;
    t[0] = flags;
    for (uint32_t i = 0; i < nhm; i++) {
        put_be16(t + p, h->adv[i]);
        p += 2;
    }
    if (!(flags & 1)) {
        for (uint32_t i = 0; i < nhm; i++) {
            put_be16(t + p, (uint32_t)h->lsb[i]);
            p += 2;
        }
    }
    if (!(flags & 2)) {
        for (uint32_t i = nhm; i < ng; i++) {
            put_be16(t + p, (uint32_t)h->lsb[i]);
            p += 2;
        }
    }
    for (uint32_t i = 0; i < ng; i++) {
        bool omitted = i < nhm ? (flags & 1) != 0 : (flags & 2) != 0;
        int32_t l = omitted ? h->xmin[i] : h->lsb[i];
        if (i < nhm) {
            put_be16(e + el, h->adv[i]);
            el += 2;
        }
        put_be16(e + el, (uint32_t)l);
        el += 2;
    }
    size_t stored = tl;
    if (tl_delta > 0) {
        stored = tl + 1;
    } else if (tl_delta < 0) {
        stored = tl - 1;
    }
    uint8_t *hhea = SDL_malloc(h->hhea_l);
    SDL_memcpy(hhea, h->hhea, h->hhea_l);
    put_be16(hhea + 34, hhea_nhm);
    w2t tabs[6] = {
        {1, nullptr, 0, (uint32_t)h->head_l, false, 0, h->head, h->head_l},
        {2, nullptr, 0, (uint32_t)h->hhea_l, false, 0, hhea, h->hhea_l},
        {4, nullptr, 0, (uint32_t)h->maxp_l, false, 0, h->maxp, h->maxp_l},
        {3, nullptr, 1, (uint32_t)((int)el + orig_delta), true, (uint32_t)stored, t, stored},
        {11, nullptr, 3, (uint32_t)h->loca_l, false, 0, h->loca, h->loca_l},
        {10, nullptr, 3, (uint32_t)h->glyf_l, false, 0, h->glyf, h->glyf_l},
    };
    size_t fl = 0;
    uint8_t *file = w2_make(tabs, 6, &fl);
    bool ok = embi_woff2_decode(file, fl, 1u << 24, out);
    SDL_free(file);
    SDL_free(hhea);
    SDL_free(t);
    if (expect_hmtx) {
        *expect_hmtx = e;
        *expect_len = el;
    } else {
        SDL_free(e);
    }
    return ok;
}

EMB_TEST(woff2_hmtx_transform_rebuilds_all_flag_combinations)
{
    hm_src h = {0};
    EMB_REQUIRE(hm_load(&h));
    uint32_t splits[3] = {h.nhm, 3, h.ng};
    for (unsigned si = 0; si < 3; si++) {
        for (uint8_t flags = 0; flags < 4; flags++) {
            embi_buf out = {0};
            uint8_t *exp = nullptr;
            size_t exp_len = 0;
            bool ok = hm_case(&h, flags, splits[si], splits[si], 0, 0, &exp, &exp_len, &out);
            if (!ok) {
                SDL_Log("hmtx case failed: split %u flags %u", (unsigned)splits[si], (unsigned)flags);
            }
            EMB_CHECK(ok);
            if (ok) {
                const uint8_t *d = nullptr;
                size_t dl = 0;
                EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("hmtx"), &d, &dl));
                EMB_CHECK(dl == exp_len && SDL_memcmp(d, exp, exp_len) == 0);
                EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("glyf"), &d, &dl));
                EMB_CHECK(dl == h.glyf_l && SDL_memcmp(d, h.glyf, dl) == 0);
                EMB_CHECK(embi_sfnt_checksum(out.data, out.len) == 0xB1B0AFBAu);
            }
            SDL_free(exp);
            embi_buf_free(&out);
        }
    }
    hm_free(&h);
}

EMB_TEST(woff2_hmtx_transform_rejections)
{
    hm_src h = {0};
    EMB_REQUIRE(hm_load(&h));
    embi_buf out = {0};
    uint32_t n = h.nhm;
    EMB_CHECK(hm_case(&h, 3, n, n, 0, 0, nullptr, nullptr, &out)); /* the baseline decodes */
    EMB_CHECK(!hm_case(&h, 0x04, n, n, 0, 0, nullptr, nullptr, &out));  /* reserved bit 2 */
    EMB_CHECK(!hm_case(&h, 0x80, n, n, 0, 0, nullptr, nullptr, &out));  /* reserved bit 7 */
    EMB_CHECK(!hm_case(&h, 0xfc | 3, n, n, 0, 0, nullptr, nullptr, &out));
    EMB_CHECK(!hm_case(&h, 0, n, n, 1, 0, nullptr, nullptr, &out));     /* one byte too long */
    EMB_CHECK(!hm_case(&h, 0, n, n, -1, 0, nullptr, nullptr, &out));    /* one byte too short */
    EMB_CHECK(!hm_case(&h, 3, n, n, 1, 0, nullptr, nullptr, &out));
    EMB_CHECK(!hm_case(&h, 1, n, n, -1, 0, nullptr, nullptr, &out));
    EMB_CHECK(!hm_case(&h, 3, n, n, 0, 2, nullptr, nullptr, &out));     /* wrong origLength */
    EMB_CHECK(!hm_case(&h, 3, n, n, 0, -2, nullptr, nullptr, &out));
    EMB_CHECK(!hm_case(&h, 3, n, h.ng + 1, 0, 0, nullptr, nullptr, &out)); /* numberOfHMetrics > numGlyphs */
    EMB_CHECK(!hm_case(&h, 3, n, 0, 0, 0, nullptr, nullptr, &out));        /* numberOfHMetrics == 0 */
    /* hhea says one split, the table was encoded for another: the size equation catches it. */
    EMB_CHECK(!hm_case(&h, 0, 3, 4, 0, 0, nullptr, nullptr, &out));
    EMB_CHECK(out.len == 0);
    embi_buf_free(&out);
    hm_free(&h);
}

/* ------------------------------------------------------------------ glyf/loca consistency */

/* One empty glyph. fmt is the transform's indexFormat, head_fmt is head.indexToLocFormat. */
static bool glyf_fmt_case(uint16_t fmt, uint16_t head_fmt, int glyf_extra, bool with_head, uint32_t *loca_len)
{
    uint8_t glyf[44];
    SDL_memset(glyf, 0, sizeof glyf);
    glyf[5] = 1;            /* numGlyphs */
    glyf[7] = (uint8_t)fmt; /* indexFormat */
    put_be32(glyf + 8, 2);  /* nContourStreamSize */
    put_be32(glyf + 28, 4); /* bboxStreamSize */
    uint8_t head[54];
    SDL_memset(head, 0, sizeof head);
    put_be32(head, 0x00010000u);
    head[51] = (uint8_t)head_fmt;
    size_t gl = 42 + (size_t)glyf_extra;
    w2t tabs[3] = {
        {10, nullptr, 0, 0, true, (uint32_t)gl, glyf, gl},
        {11, nullptr, 0, fmt ? 8u : 4u, true, 0, nullptr, 0},
        {1, nullptr, 0, sizeof head, false, 0, head, sizeof head},
    };
    size_t fl = 0;
    uint8_t *file = w2_make(tabs, with_head ? 3 : 2, &fl);
    embi_buf out = {0};
    bool ok = embi_woff2_decode(file, fl, 1u << 16, &out);
    if (ok && loca_len) {
        const uint8_t *d = nullptr;
        size_t dl = 0;
        ok = tu_sfnt_table(out.data, out.len, tu_tag("loca"), &d, &dl);
        *loca_len = (uint32_t)dl;
    }
    embi_buf_free(&out);
    SDL_free(file);
    return ok;
}

EMB_TEST(woff2_loca_format_must_match_head)
{
    uint32_t ll = 0;
    EMB_CHECK(glyf_fmt_case(0, 0, 0, true, &ll));
    EMB_CHECK(ll == 4);
    EMB_CHECK(glyf_fmt_case(1, 1, 0, true, &ll));
    EMB_CHECK(ll == 8);
    EMB_CHECK(!glyf_fmt_case(0, 1, 0, true, nullptr));  /* short loca, head says long */
    EMB_CHECK(!glyf_fmt_case(1, 0, 0, true, nullptr));  /* long loca, head says short */
    EMB_CHECK(!glyf_fmt_case(0, 0, 0, false, nullptr)); /* no head table at all */
}

EMB_TEST(woff2_glyf_transform_rejects_trailing_bytes)
{
    EMB_CHECK(!glyf_fmt_case(0, 0, 1, true, nullptr));
}

EMB_TEST(woff2_rejects_excess_table_count)
{
    uint8_t file[64];
    SDL_memset(file, 0, sizeof file);
    put_be32(file, 0x774F4632u);
    put_be32(file + 8, sizeof file);
    file[12] = 0xff;
    file[13] = 0xff; /* 65535 tables in a 64-byte file */
    embi_buf out = {0};
    EMB_CHECK(!embi_woff2_decode(file, sizeof file, 1u << 20, &out));
    embi_buf_free(&out);
}

/* ------------------------------------------------------------------ sampled fuzz */

EMB_TEST(woff2_truncation_is_sampled_and_fails)
{
    size_t wl = 0;
    uint8_t *w = tu_load("tests/fonts/SourceSans3-Regular.woff2", &wl);
    EMB_REQUIRE(w != nullptr);
    embi_buf out = {0};
    size_t step = wl / 64 + 1;
    for (size_t k = 0; k < wl; k += step) {
        EMB_CHECK(!embi_woff2_decode(w, k, 1u << 22, &out));
    }
    embi_buf_free(&out);
    SDL_free(w);
}

EMB_TEST(woff2_random_byte_flips_never_crash)
{
    size_t wl = 0;
    uint8_t *w = tu_load("tests/fonts/SourceSans3-Regular.woff2", &wl);
    EMB_REQUIRE(w != nullptr);
    tu_rng rng = {0xBADC0DEu};
    embi_buf out = {0};
    for (int iter = 0; iter < 300; iter++) {
        uint8_t *copy = embi_alloc(wl);
        SDL_memcpy(copy, w, wl);
        int flips = 1 + (int)(tu_rand(&rng) % 4);
        for (int f = 0; f < flips; f++) {
            size_t at = tu_rand(&rng) % wl;
            copy[at] ^= (uint8_t)(1u + tu_rand(&rng) % 255u);
        }
        bool ok = embi_woff2_decode(copy, wl, 1u << 22, &out);
        EMB_CHECK(!ok || out.len <= (1u << 22));
        embi_free(copy, wl);
    }
    embi_buf_free(&out);
    SDL_free(w);
}

EMB_TEST_MAIN()
