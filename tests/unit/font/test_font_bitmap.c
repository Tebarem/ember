#include "font_fixture.h"

#include "image/png.h"

/* Bitmap colour glyphs (W4i): CBLC/CBDT and sbix, over tests/fonts/bitmap (built by tools/fonts/make_fixtures.py).
 *
 * EmberBitmap-cbdt.ttf, glyph 1 ('A'), three strikes:
 *   16 ppem: index format 1, image format 17 (small metrics): 16x16 red PNG, bearing (0, 14), advance 16
 *   32 ppem: index format 3, image format 18 (big metrics): 32x32 green PNG, bearing (0, 28)
 *   64 ppem: index format 2 (shared big metrics), image format 19: 64x64 yellow PNG, bearing (1, 56)
 * EmberBitmap-sbix.ttf, glyph 1: one strike of 16 ppem, a 16x16 blue PNG, origin (2, -3).
 * EmberBitmap-colr.ttf: the CBDT strikes plus a COLR v0 entry for glyph 1. */

static bool decode(const embi_bitmap_glyph *g, embi_pixels *out)
{
    return embi_png_decode(g->png, g->len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, out);
}

static bool all_pixels(const embi_pixels *p, uint8_t r, uint8_t gr, uint8_t b, uint8_t a)
{
    for (uint32_t i = 0; i < p->w * p->h; i++) {
        const uint8_t *q = p->px + 4u * i;
        if (q[0] != r || q[1] != gr || q[2] != b || q[3] != a) {
            return false;
        }
    }
    return true;
}

EMB_TEST(cbdt_strike_16_format_17)
{
    size_t base = embi_mem_live();
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    EMB_CHECK(embi_font_has_bitmaps(&f));
    EMB_CHECK(!f.metrics.has_colr);
    embi_bitmap_glyph g;
    EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, 16.0f, &g));
    EMB_CHECK_EQ_INT(g.ppem, 16);
    EMB_CHECK_EQ_INT(g.width, 16);
    EMB_CHECK_EQ_INT(g.height, 16);
    EMB_CHECK_NEAR(g.left, 0.0, 1e-6);
    EMB_CHECK_NEAR(g.top, 14.0, 1e-6);
    EMB_CHECK(!g.top_from_bottom);
    EMB_REQUIRE(g.png != nullptr && g.len > 8);
    EMB_CHECK(g.png >= ff.p && g.png + g.len <= ff.p + ff.n); /* a slice of the font, not a copy */
    EMB_CHECK_EQ_INT(g.png[1], 'P');
    embi_pixels px = {0};
    EMB_REQUIRE(decode(&g, &px));
    EMB_CHECK_EQ_INT(px.w, 16);
    EMB_CHECK_EQ_INT(px.h, 16);
    EMB_CHECK(all_pixels(&px, 255, 0, 0, 255));
    embi_pixels_free(&px);
    embi_font_close(&f);
    fx_free(&ff);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(cbdt_strike_32_format_18)
{
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    embi_bitmap_glyph g;
    EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, 32.0f, &g));
    EMB_CHECK_EQ_INT(g.ppem, 32);
    EMB_CHECK_EQ_INT(g.width, 32);
    EMB_CHECK_EQ_INT(g.height, 32);
    EMB_CHECK_NEAR(g.left, 0.0, 1e-6);
    EMB_CHECK_NEAR(g.top, 28.0, 1e-6);
    embi_pixels px = {0};
    EMB_REQUIRE(decode(&g, &px));
    EMB_CHECK_EQ_INT(px.w, 32);
    EMB_CHECK(all_pixels(&px, 0, 255, 0, 255));
    embi_pixels_free(&px);
    embi_font_close(&f);
    fx_free(&ff);
}

EMB_TEST(cbdt_strike_64_format_19_with_shared_metrics)
{
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    embi_bitmap_glyph g;
    EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, 64.0f, &g));
    EMB_CHECK_EQ_INT(g.ppem, 64);
    EMB_CHECK_EQ_INT(g.width, 64);
    EMB_CHECK_EQ_INT(g.height, 64);
    EMB_CHECK_NEAR(g.left, 1.0, 1e-6); /* from the index subtable's bigMetrics */
    EMB_CHECK_NEAR(g.top, 56.0, 1e-6);
    embi_pixels px = {0};
    EMB_REQUIRE(decode(&g, &px));
    EMB_CHECK_EQ_INT(px.w, 64);
    EMB_CHECK(all_pixels(&px, 255, 255, 0, 255));
    embi_pixels_free(&px);
    embi_font_close(&f);
    fx_free(&ff);
}

EMB_TEST(strike_choice_prefers_the_larger_then_the_largest)
{
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    static const struct {
        float target;
        uint32_t ppem;
    } cases[] = {
        {1.0f, 16},   {8.0f, 16},   {16.0f, 16},  /* the smallest strike that is at least the target */
        {16.5f, 32},  {20.0f, 32},  {32.0f, 32},  /* prefer larger, then scale down */
        {33.0f, 64},  {48.0f, 64},  {64.0f, 64},
        {65.0f, 64},  {128.0f, 64}, {4000.0f, 64}, /* nothing larger: the largest strike, scaled up */
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_bitmap_glyph g;
        EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, cases[i].target, &g));
        if (g.ppem != cases[i].ppem) {
            SDL_Log("target %.1f: got strike %u, want %u", cases[i].target, g.ppem, cases[i].ppem);
            EMB_CHECK(false);
        }
    }
    embi_font_close(&f);
    fx_free(&ff);
}

EMB_TEST(glyphs_without_a_bitmap_and_bad_arguments)
{
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    embi_bitmap_glyph g;
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 0, 16.0f, &g));      /* .notdef has none */
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 2, 16.0f, &g));      /* 'B' has an outline only */
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 3, 16.0f, &g));      /* beyond the strikes' glyph range */
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 100000, 16.0f, &g)); /* beyond numGlyphs */
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 1, 0.0f, &g) || g.ppem == 16);
    embi_font_close(&f);
    fx_free(&ff);

    /* A font with no bitmap table. */
    fx_file nf;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &nf));
    embi_font n;
    EMB_REQUIRE(fx_open(&nf, 0, &n));
    EMB_CHECK(!embi_font_has_bitmaps(&n));
    EMB_CHECK(!embi_font_bitmap_glyph(&n, 36, 16.0f, &g));
    embi_font_close(&n);
    fx_free(&nf);
}

EMB_TEST(sbix_strike_and_origin)
{
    size_t base = embi_mem_live();
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-sbix.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    EMB_CHECK(embi_font_has_bitmaps(&f));
    embi_bitmap_glyph g;
    EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, 16.0f, &g));
    EMB_CHECK_EQ_INT(g.ppem, 16);
    EMB_CHECK_NEAR(g.left, 2.0, 1e-6);       /* originOffsetX */
    EMB_CHECK_NEAR(g.origin_y, -3.0, 1e-6);  /* originOffsetY: the bottom edge relative to the baseline */
    EMB_CHECK(g.top_from_bottom);            /* top = origin_y + the decoded height */
    embi_pixels px = {0};
    EMB_REQUIRE(decode(&g, &px));
    EMB_CHECK_EQ_INT(px.w, 16);
    EMB_CHECK_EQ_INT(px.h, 16);
    EMB_CHECK(all_pixels(&px, 0, 0, 255, 255));
    embi_pixels_free(&px);
    /* The only strike serves every target size. */
    EMB_REQUIRE(embi_font_bitmap_glyph(&f, 1, 100.0f, &g));
    EMB_CHECK_EQ_INT(g.ppem, 16);
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 0, 16.0f, &g));
    EMB_CHECK(!embi_font_bitmap_glyph(&f, 2, 16.0f, &g));
    embi_font_close(&f);
    fx_free(&ff);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(colr_is_detected_next_to_the_bitmaps)
{
    /* The face has COLR and CBDT: has_colr is set, so the draw path takes COLR (src/text/draw.c: a bitmap run needs
     * !has_colr), while the bitmap tables stay readable. */
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-colr.ttf", &ff));
    embi_font f;
    EMB_REQUIRE(fx_open(&ff, 0, &f));
    EMB_CHECK(f.metrics.has_colr);
    EMB_CHECK(embi_font_has_bitmaps(&f));
    embi_bitmap_glyph g;
    EMB_CHECK(embi_font_bitmap_glyph(&f, 1, 16.0f, &g));
    embi_font_close(&f);
    fx_free(&ff);
}

EMB_TEST(truncated_tables_never_read_outside)
{
    /* Every prefix of the CBDT and sbix fonts either opens and answers, or fails to open: no crash, no overread. The
     * sanitizers catch an overread. */
    static const char *const files[] = {"bitmap/EmberBitmap-cbdt.ttf", "bitmap/EmberBitmap-sbix.ttf"};
    for (uint32_t k = 0; k < 2; k++) {
        fx_file ff;
        EMB_REQUIRE(fx_load(files[k], &ff));
        for (size_t cut = 0; cut < ff.n; cut += 7) {
            uint8_t *copy = SDL_malloc(cut > 0 ? cut : 1);
            EMB_REQUIRE(copy != nullptr);
            SDL_memcpy(copy, ff.p, cut);
            embi_font f;
            if (embi_font_open(copy, cut, 0, &f)) {
                embi_bitmap_glyph g;
                (void)embi_font_has_bitmaps(&f);
                for (uint32_t gid = 0; gid < 4; gid++) {
                    (void)embi_font_bitmap_glyph(&f, gid, 16.0f, &g);
                    (void)embi_font_bitmap_glyph(&f, gid, 64.0f, &g);
                }
                embi_font_close(&f);
            }
            SDL_free(copy);
        }
        fx_free(&ff);
    }
}

EMB_TEST(corrupted_bytes_never_read_outside)
{
    /* Flip each byte of the bitmap tables in turn (the fixtures are small). */
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-cbdt.ttf", &ff));
    uint8_t *copy = SDL_malloc(ff.n);
    EMB_REQUIRE(copy != nullptr);
    for (size_t i = 0; i < ff.n; i++) {
        SDL_memcpy(copy, ff.p, ff.n);
        copy[i] ^= 0xFF;
        embi_font f;
        if (embi_font_open(copy, ff.n, 0, &f)) {
            embi_bitmap_glyph g;
            for (uint32_t gid = 0; gid < 4; gid++) {
                (void)embi_font_bitmap_glyph(&f, gid, 16.0f, &g);
                (void)embi_font_bitmap_glyph(&f, gid, 64.0f, &g);
            }
            embi_font_close(&f);
        }
    }
    SDL_free(copy);
    fx_free(&ff);
}

EMB_TEST(sbix_offsets_past_the_table_are_rejected)
{
    /* A corrupt glyph record: o1 (and then o0) point past the end of the sbix table. The glyph must be refused, never
     * read (the sanitizers catch an overread of the graphicType bytes or of the PNG slice). */
    fx_file ff;
    EMB_REQUIRE(fx_load("bitmap/EmberBitmap-sbix.ttf", &ff));
    uint8_t *copy = SDL_malloc(ff.n);
    EMB_REQUIRE(copy != nullptr);
    uint32_t nt = (uint32_t)(ff.p[4] << 8 | ff.p[5]);
    size_t sbix_at = 0, sbix_len = 0;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = ff.p + 12 + 16u * i;
        if (r[0] == 's' && r[1] == 'b' && r[2] == 'i' && r[3] == 'x') {
            sbix_at = (size_t)r[8] << 24 | (size_t)r[9] << 16 | (size_t)r[10] << 8 | r[11];
            sbix_len = (size_t)r[12] << 24 | (size_t)r[13] << 16 | (size_t)r[14] << 8 | r[15];
        }
    }
    EMB_REQUIRE(sbix_at > 0 && sbix_at + sbix_len <= ff.n);
    const uint8_t *sb = ff.p + sbix_at;
    size_t base = (size_t)sb[8] << 24 | (size_t)sb[9] << 16 | (size_t)sb[10] << 8 | sb[11];
    size_t oa = sbix_at + base + 4 + 4 * 1; /* glyph 1's offset pair */
    for (uint32_t variant = 0; variant < 3; variant++) {
        SDL_memcpy(copy, ff.p, ff.n);
        uint32_t o0 = (uint32_t)copy[oa] << 24 | (uint32_t)copy[oa + 1] << 16 | (uint32_t)copy[oa + 2] << 8 | copy[oa + 3];
        uint32_t v0 = o0, v1 = 0x7fffff00u;
        if (variant == 1) {
            v0 = (uint32_t)sbix_len + 100u;
            v1 = v0 + 64u;
        } else if (variant == 2) {
            v1 = (uint32_t)sbix_len - (uint32_t)base + 4u; /* ends 4 bytes... past the table */
        }
        uint32_t vals[2] = {v0, v1};
        for (uint32_t k = 0; k < 2; k++) {
            copy[oa + 4 * k] = (uint8_t)(vals[k] >> 24);
            copy[oa + 4 * k + 1] = (uint8_t)(vals[k] >> 16);
            copy[oa + 4 * k + 2] = (uint8_t)(vals[k] >> 8);
            copy[oa + 4 * k + 3] = (uint8_t)vals[k];
        }
        embi_font f;
        EMB_REQUIRE(embi_font_open(copy, ff.n, 0, &f));
        embi_bitmap_glyph g;
        EMB_CHECK(!embi_font_bitmap_glyph(&f, 1, 16.0f, &g));
        embi_font_close(&f);
    }
    SDL_free(copy);
    fx_free(&ff);
}

EMB_TEST_MAIN()
