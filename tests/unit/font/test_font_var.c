#include "font_build.h"

#include <math.h>

/* Variation support: fvar axes and named instances, avar-mapped normalization, MVAR deltas,
 * HVAR advances and gvar-driven outline bounds. Expected values are from the raw tables of
 * NotoSans-VF.ttf (fvar, avar, gvar, HVAR, MVAR) and SourceSans3VF-Upright.otf.
 *
 * Reference derivation (Python over the raw tables):
 *   NotoSans-VF avar wght pairs: (-1,-1) (-0.6667,-0.7969) (-0.3333,-0.5) (0,0)
 *     (0.2000,0.1800) (0.4000,0.3800) (0.6000,0.6100) (0.8000,0.7900) (1,1), as F2DOT14.
 *   user 650 -> normalized 0.5 -> avar 0.4949951171875 -> 8110.0 exactly.
 *   user 550 -> normalized 0.3 -> avar 0.27998046875 -> 4587.2 -> 4587.
 *   user 700 -> normalized 0.6 -> avar 0.6100073 -> 9994.36 -> 9994.
 *   MVAR 'xhgt' record: VarIdx (outer 0, inner 0). Its IVS delta is -8 at wght 100 and +17 at
 *     wght 900, and 0 at the default.
 *   HVAR for 'A' (gid 36): hmtx 639 plus delta; advances 577 (wght 100), 639 (400), 726 (900).
 *   gvar outline bbox of 'A': (0, 0, 577, 716) at wght 100, (0, 0, 726, 717) at wght 900.
 *   fvar instance 0 is (wght 100, wdth 100) and there are 9 named instances.
 *   SourceSans3VF: one wght axis (200, 200, 900); user 500 -> coord 7963. */

#define NOTO_WGHT_100 (-16384)
#define NOTO_WGHT_900 16384

EMB_TEST(noto_vf_axes_and_instances)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    EMB_REQUIRE(f.axis_count == 2);
    EMB_CHECK_EQ_INT(f.axes[0].tag, EMBI_TAG('w', 'g', 'h', 't'));
    EMB_CHECK_NEAR(f.axes[0].min, 100.0, 1e-4);
    EMB_CHECK_NEAR(f.axes[0].def, 400.0, 1e-4);
    EMB_CHECK_NEAR(f.axes[0].max, 900.0, 1e-4);
    EMB_CHECK_EQ_INT(f.axes[1].tag, EMBI_TAG('w', 'd', 't', 'h'));
    EMB_CHECK_NEAR(f.axes[1].min, 62.5, 1e-4);
    EMB_CHECK_NEAR(f.axes[1].def, 100.0, 1e-4);
    EMB_CHECK_NEAR(f.axes[1].max, 100.0, 1e-4);

    EMB_CHECK_EQ_INT(embi_font_axis_index(&f, EMBI_TAG('w', 'g', 'h', 't')), 0);
    EMB_CHECK_EQ_INT(embi_font_axis_index(&f, EMBI_TAG('w', 'd', 't', 'h')), 1);
    EMB_CHECK_EQ_INT(embi_font_axis_index(&f, EMBI_TAG('o', 'p', 's', 'z')), -1);

    /* fvar: instanceCount 9; instance 0 is (wght 100, wdth 100) per the raw instance records. */
    EMB_CHECK_EQ_INT(embi_font_instance_count(&f), 9);
    float user[2] = {0.0f, 0.0f};
    EMB_REQUIRE(embi_font_instance_coords(&f, 0, user));
    EMB_CHECK_NEAR(user[0], 100.0, 1e-4);
    EMB_CHECK_NEAR(user[1], 100.0, 1e-4);
    EMB_CHECK(!embi_font_instance_coords(&f, 9, user));

    embi_font_close(&f);
    fx_free(&src);
}

/* User values map through the normalization and avar to F2DOT14 (see the reference above).
 * Out-of-range values clamp to the axis. The wdth axis stays at its default (100) so its coord
 * is 0. */
EMB_TEST(noto_vf_normalize_with_avar)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    struct {
        float wght;
        int16_t want;
    } cases[] = {
        {100.0f, NOTO_WGHT_100}, {400.0f, 0},          {900.0f, NOTO_WGHT_900},
        {2000.0f, NOTO_WGHT_900}, /* clamps to 900 */  {650.0f, 8110},
        {550.0f, 4587},          {700.0f, 9994},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const float user[2] = {cases[i].wght, 100.0f};
        int16_t coords[2] = {12345, 12345};
        EMB_REQUIRE(embi_font_normalize(&f, user, coords));
        EMB_CHECK_EQ_INT(coords[0], cases[i].want);
        EMB_CHECK_EQ_INT(coords[1], 0);
    }

    embi_font_close(&f);
    fx_free(&src);
}

/* MVAR 'xhgt' is -8 at wght 100, 0 at the default and +17 at wght 900. */
EMB_TEST(noto_vf_mvar_delta)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t lo[2] = {NOTO_WGHT_100, 0};
    const int16_t def[2] = {0, 0};
    const int16_t hi[2] = {NOTO_WGHT_900, 0};
    float d = 99.0f;
    EMB_REQUIRE(embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), lo, &d));
    EMB_CHECK_NEAR(d, -8.0, 1e-4);
    EMB_REQUIRE(embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), def, &d));
    EMB_CHECK_NEAR(d, 0.0, 1e-4);
    EMB_REQUIRE(embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), hi, &d));
    EMB_CHECK_NEAR(d, 17.0, 1e-4);

    embi_font_close(&f);
    fx_free(&src);
}

/* 'A' (gid 36) advance through HVAR at the three weights. Default is hmtx 639. */
EMB_TEST(noto_vf_advance_follows_hvar)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t lo[2] = {NOTO_WGHT_100, 0};
    const int16_t def[2] = {0, 0};
    const int16_t hi[2] = {NOTO_WGHT_900, 0};
    float adv = 0.0f;
    EMB_REQUIRE(embi_font_glyph_advance(&f, 36, lo, &adv));
    EMB_CHECK_NEAR(adv, 577.0, 1e-3);
    EMB_REQUIRE(embi_font_glyph_advance(&f, 36, def, &adv));
    EMB_CHECK_NEAR(adv, 639.0, 1e-3);
    EMB_REQUIRE(embi_font_glyph_advance(&f, 36, hi, &adv));
    EMB_CHECK_NEAR(adv, 726.0, 1e-3);

    embi_font_close(&f);
    fx_free(&src);
}

/* gvar moves the 'A' outline. The bbox follows the reference above, and the default instance
 * (nullptr coords) is the header bbox. */
EMB_TEST(noto_vf_outline_bounds_follow_gvar)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t lo[2] = {NOTO_WGHT_100, 0};
    const int16_t hi[2] = {NOTO_WGHT_900, 0};
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, lo, &o));
    EMB_CHECK_NEAR(o.xmax, 577.0, 0.5);
    EMB_CHECK_NEAR(o.ymax, 716.0, 0.5);
    EMB_CHECK_EQ_INT(o.contours.len, 2);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, hi, &o));
    EMB_CHECK_NEAR(o.xmax, 726.0, 0.5);
    EMB_CHECK_NEAR(o.ymax, 717.0, 0.5);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, nullptr, &o));
    EMB_CHECK_NEAR(o.xmax, 638.0, 0.01);
    EMB_CHECK_NEAR(o.ymax, 717.0, 0.01);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* SourceSans3VF-Upright.otf: one wght axis, default 200. The user 500 maps to coord 7963
 * (avar segment (0.2857, 0.3680) to (0.4286, 0.4860), see test_font_cff for the advances). */
EMB_TEST(source_vf_normalize)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3VF-Upright.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_REQUIRE(f.axis_count == 1);

    struct {
        float wght;
        int16_t want;
    } cases[] = {{200.0f, 0}, {500.0f, 7963}, {900.0f, 16384}, {1000.0f, 16384}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const float user[1] = {cases[i].wght};
        int16_t coords[1] = {12345};
        EMB_REQUIRE(embi_font_normalize(&f, &user[0], coords));
        EMB_CHECK_EQ_INT(coords[0], cases[i].want);
    }

    embi_font_close(&f);
    fx_free(&src);
}


/* NaN user values take the axis default; infinities clamp to the axis ends. */
EMB_TEST(noto_vf_normalize_nan_is_default_and_inf_clamps)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    int16_t coords[2];
    const float nan_user[2] = {NAN, 100.0f};
    coords[0] = 999;
    coords[1] = 999;
    EMB_REQUIRE(embi_font_normalize(&f, nan_user, coords));
    EMB_CHECK_EQ_INT(coords[0], 0);
    EMB_CHECK_EQ_INT(coords[1], 0);

    const float inf_user[2] = {INFINITY, 100.0f};
    EMB_REQUIRE(embi_font_normalize(&f, inf_user, coords));
    EMB_CHECK_EQ_INT(coords[0], 16384);

    const float ninf_user[2] = {-INFINITY, 100.0f};
    EMB_REQUIRE(embi_font_normalize(&f, ninf_user, coords));
    EMB_CHECK_EQ_INT(coords[0], -16384);

    embi_font_close(&f);
    fx_free(&src);
}

/* Curved glyph 'a' (gid 68) of NotoSans-VF at wght 100/400/900. Reference: fontTools 4.68
 * BoundsPen on getGlyphSet(location)['a'] and the glyph set widths:
 *   wght 100: (48, -10, 428, 537) advance 511
 *   wght 400: (46, -10, 480, 545) advance 561
 *   wght 900: (36, -10, 559, 563) advance 623 */
EMB_TEST(noto_vf_curved_a_bounds_and_advance)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 'a'), 68);

    static const int16_t w[3] = {NOTO_WGHT_100, 0, NOTO_WGHT_900};
    static const double ref[3][5] = {{48, -10, 428, 537, 511},
                                     {46, -10, 480, 545, 561},
                                     {36, -10, 559, 563, 623}};
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    for (int i = 0; i < 3; i++) {
        const int16_t c[2] = {w[i], 0};
        EMB_REQUIRE(embi_font_glyph_outline(&f, 68, c, &o));
        EMB_CHECK_NEAR(o.xmin, ref[i][0], 0.6);
        EMB_CHECK_NEAR(o.ymin, ref[i][1], 0.6);
        EMB_CHECK_NEAR(o.xmax, ref[i][2], 0.6);
        EMB_CHECK_NEAR(o.ymax, ref[i][3], 0.6);
        float adv = 0.0f;
        EMB_REQUIRE(embi_font_glyph_advance(&f, 68, c, &adv));
        EMB_CHECK_NEAR(adv, ref[i][4], 1e-3);
    }
    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Region axis spanning zero. Synthetic MVAR with one axis ('wght', 0..1) and two regions:
 *   region 0: start -1.0, peak 0.5, end 1.0 (start < 0 < end: HarfBuzz and FreeType ignore
 *             such an axis, so the region scalar is 1 at every coordinate);
 *   region 1: start 0, peak 0.5, end 1.0 (an ordinary tent).
 * VarData rows (two regions each): row 0 = [100, 0] for 'xhgt', row 1 = [0, 100] for 'cpht'.
 * Coordinates are F2DOT14. xhgt is 100 at 0.5 and at -0.5 (a non-skipping implementation would
 * give 100 at 0.5 but 100 * 1/3 at -0.5), and at 0 and 1 too. cpht follows the tent: 100 at
 * 0.5, 50 at 0.25, 0 at -0.5, 0 at 1.0. */
EMB_TEST(mvar_region_spanning_zero_is_ignored)
{
    fb_buf mv = {0}, fvar = {0};
    fb_u16(&mv, 1); /* majorVersion */
    fb_u16(&mv, 0);
    fb_u16(&mv, 0);  /* reserved */
    fb_u16(&mv, 8);  /* valueRecordSize */
    fb_u16(&mv, 2);  /* valueRecordCount */
    fb_u16(&mv, 28); /* itemVariationStoreOffset: 12 header + 16 records */
    fb_u32(&mv, EMBI_TAG('x', 'h', 'g', 't'));
    fb_u16(&mv, 0);
    fb_u16(&mv, 0);
    fb_u32(&mv, EMBI_TAG('c', 'p', 'h', 't'));
    fb_u16(&mv, 0);
    fb_u16(&mv, 1);
    /* ItemVariationStore at 28 */
    fb_u16(&mv, 1);  /* format */
    fb_u32(&mv, 12); /* regionList */
    fb_u16(&mv, 1);
    fb_u32(&mv, 28); /* VarData at store + 28 */
    fb_u16(&mv, 1);  /* axisCount */
    fb_u16(&mv, 2);  /* regionCount */
    fb_u16(&mv, (uint16_t)-16384);
    fb_u16(&mv, 8192);
    fb_u16(&mv, 16384);
    fb_u16(&mv, 0);
    fb_u16(&mv, 8192);
    fb_u16(&mv, 16384);
    fb_u16(&mv, 2); /* itemCount */
    fb_u16(&mv, 2); /* wordDeltaCount */
    fb_u16(&mv, 2); /* regionIndexCount */
    fb_u16(&mv, 0);
    fb_u16(&mv, 1);
    fb_u16(&mv, 100);
    fb_u16(&mv, 0);
    fb_u16(&mv, 0);
    fb_u16(&mv, 100);

    fb_u16(&fvar, 1);
    fb_u16(&fvar, 0);
    fb_u16(&fvar, 16);
    fb_u16(&fvar, 2);
    fb_u16(&fvar, 1);
    fb_u16(&fvar, 20);
    fb_u16(&fvar, 0);
    fb_u16(&fvar, 0);
    fb_u32(&fvar, EMBI_TAG('w', 'g', 'h', 't'));
    fb_u32(&fvar, 0);
    fb_u32(&fvar, 0);
    fb_u32(&fvar, 0x10000);
    fb_u16(&fvar, 0);
    fb_u16(&fvar, 0);
    fb_table ex[2] = {{EMBI_TAG('f', 'v', 'a', 'r'), fvar.p, fvar.n},
                      {EMBI_TAG('M', 'V', 'A', 'R'), mv.p, mv.n}};
    fx_file src;
    EMB_REQUIRE(fb_font(0x00010000u, 1000, 4, ex, 2, &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t at[6] = {8192, -8192, 0, 16384, 4096, -16384};
    const double xhgt[6] = {100, 100, 100, 100, 100, 100};
    const double cpht[6] = {100, 0, 0, 0, 50, 0};
    for (int i = 0; i < 6; i++) {
        float d = -1.0f;
        EMB_REQUIRE(embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), &at[i], &d));
        EMB_CHECK_NEAR(d, xhgt[i], 1e-3);
        d = -1.0f;
        EMB_REQUIRE(embi_font_mvar_delta(&f, EMBI_TAG('c', 'p', 'h', 't'), &at[i], &d));
        EMB_CHECK_NEAR(d, cpht[i], 1e-3);
    }

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&mv);
    fb_free(&fvar);
}

EMB_TEST_MAIN()
