#include "font_build.h"

#include "font/font_internal.h"

/* CFF (Type 2 charstring) outlines: CFF1 from SourceSans3-Regular.otf, CFF2 from
 * SourceSans3VF-Upright.otf, and synthetic CFF1 fonts built byte by byte (font_build.h) for the
 * operator paths the real fixtures do not reach. */

/* SourceSans3-Regular.otf glyph 2 ('A'). The tight bbox (3, 0, 541, 656) and the two moveto
 * operators (two contours) come from a dense sampling of the charstring path (cubic segments
 * evaluated at 2001 parameter values), computed from the raw CharString INDEX. The advance 544
 * is hmtx (see test_font_metrics). The cubic-to-quad conversion may differ from the curve by its
 * tolerance (about one unit in a 1000 unit em), so the bbox check allows 1.5 units. */
EMB_TEST(cff1_a_outline_bbox_and_contours)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 2, nullptr, &o));
    EMB_CHECK(o.quads.len > 0);
    EMB_CHECK_EQ_INT(o.contours.len, 2);
    EMB_CHECK_NEAR(o.xmin, 3.0, 1.5);
    EMB_CHECK_NEAR(o.ymin, 0.0, 1.5);
    EMB_CHECK_NEAR(o.xmax, 541.0, 1.5);
    EMB_CHECK_NEAR(o.ymax, 656.0, 1.5);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Every mapped ASCII glyph of the CFF font yields closed contours, and a glyph id past the end
 * of the CharStrings fails. */
EMB_TEST(cff1_closed_contours_and_bad_gid)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    for (uint32_t cp = 0x41; cp <= 0x5A; cp++) {
        const uint32_t gid = embi_font_glyph_index(&f, cp);
        EMB_REQUIRE(gid != 0);
        EMB_REQUIRE(embi_font_glyph_outline(&f, gid, nullptr, &o));
        EMB_CHECK(o.quads.len > 0);
        EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
    }
    EMB_CHECK(!embi_font_glyph_outline(&f, 2478u, nullptr, &o));

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* SourceSans3VF-Upright.otf (CFF2, one wght axis 200..900, default 200). Advances are hmtx plus
 * the HVAR delta, through the advance map (outer/inner per glyph). Reference, from the raw
 * tables with the ItemVariationStore at the CFF2 VarStore offset (+2 for the length prefix):
 *   gid 28: map (1, 30). Coords 0 (wght 200), 7963 (wght 500), 16384 (wght 900)
 *           give 486, 510.3021, 536.
 *   gid 2 ('A'): map (1, 42). The same coords give 520, 550.7827, 584.
 * The coords 7963 is the avar-mapped normalized value of wght 500 (see test_font_var). */
EMB_TEST(cff2_variable_advances_via_hvar)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3VF-Upright.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK(f.metrics.is_cff2);
    EMB_CHECK(f.metrics.is_variable);
    EMB_CHECK_EQ_INT(f.axis_count, 1);

    const int16_t coords_200[1] = {0};
    const int16_t coords_500[1] = {7963};
    const int16_t coords_900[1] = {16384};
    const int16_t *all[3] = {coords_200, coords_500, coords_900};

    const double want28[3] = {486.0, 510.3021363173957, 536.0};
    const double want2[3] = {520.0, 550.7827060020346, 584.0};
    for (int i = 0; i < 3; i++) {
        float adv = -1.0f;
        EMB_REQUIRE(embi_font_glyph_advance(&f, 28, all[i], &adv));
        EMB_CHECK_NEAR(adv, want28[i], 0.01);
        EMB_REQUIRE(embi_font_glyph_advance(&f, 2, all[i], &adv));
        EMB_CHECK_NEAR(adv, want2[i], 0.01);
    }

    embi_font_close(&f);
    fx_free(&src);
}

/* A CFF2 outline at the default instance is closed and non-empty. */
EMB_TEST(cff2_outline_default_instance_is_closed)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3VF-Upright.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 2, nullptr, &o));
    EMB_CHECK(o.quads.len > 0);
    EMB_CHECK(o.contours.len >= 2);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* CFF2 blend at non-default instances: the bbox of 'A' (gid 2) moves with the blended
 * coordinates. Reference: fontTools 4.68 BoundsPen on the instantiated glyph set (getGlyphSet
 * with location wght = 200, 500, 900; the 500 location goes through avar to coord 7963):
 *   coord 0     : (10, 0, 510, 660)
 *   coord 7963  : (0.2794, 0, 550.5024, 655.1397)
 *   coord 16384 : (-10, 0, 594, 650)
 * The outline is a cubic path converted to quads, so the check allows 1.5 units. */
EMB_TEST(cff2_outline_bbox_follows_blend)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3VF-Upright.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t coords[3][1] = {{0}, {7963}, {16384}};
    const double want[3][4] = {
        {10.0, 0.0, 510.0, 660.0},
        {0.2794336, 0.0, 550.5023600, 655.1397168},
        {-10.0, 0.0, 594.0, 650.0},
    };
    for (int i = 0; i < 3; i++) {
        embi_outline o;
        SDL_memset(&o, 0, sizeof o);
        EMB_REQUIRE(embi_font_glyph_outline(&f, 2, coords[i], &o));
        EMB_CHECK_NEAR(o.xmin, want[i][0], 1.5);
        EMB_CHECK_NEAR(o.ymin, want[i][1], 1.5);
        EMB_CHECK_NEAR(o.xmax, want[i][2], 1.5);
        EMB_CHECK_NEAR(o.ymax, want[i][3], 1.5);
        EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
        embi_outline_free(&o);
    }

    embi_font_close(&f);
    fx_free(&src);
}

/* seac in SourceSans3-Regular.otf. Glyphs are rewritten in a copy, by patching the bytes of
 * their own charstring (found through the parsed INDEX, so the offsets are exact). Most letters
 * are 3 byte subroutine calls in this font, so the seac tests use 'D' (15 bytes) and 'a' (27
 * bytes) as the patched glyphs. Standard encoding codes: 'a' 97 (236), 'b' 98 (237), 'D' 68
 * (207), as 139 + code. */
static bool patch_charstring(const fx_file *orig, const embi_font *f, uint32_t gid,
                             const uint8_t *bytes, size_t n, fx_file *io)
{
    embi_bytes cs;
    if (!embi_cffidx_get(&f->cff_data->charstrings, gid, &cs) || cs.n < n || io->p == nullptr ||
        io->n != orig->n) {
        return false;
    }
    /* The offset is taken from the original file, which f was opened on, and applied to io. */
    const size_t at = (size_t)(cs.p - orig->p);
    SDL_memcpy(io->p + at, bytes, n);
    return true;
}

/* 'D' rewritten as seac(base 'D', accent 'D'): the base resolves back to the glyph itself, so it
 * must fail, not recurse until the stack runs out. */
EMB_TEST(cff1_seac_self_reference_fails)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    const uint32_t gid_d = embi_font_glyph_index(&f, 'D');
    EMB_REQUIRE(gid_d != 0);

    const uint8_t seac_self[] = {139, 139, 207, 207, 14}; /* 0 0 'D' 'D' endchar */
    fx_file m = fb_copy(&src);
    EMB_REQUIRE(patch_charstring(&src, &f, gid_d, seac_self, sizeof seac_self, &m));

    embi_font g;
    SDL_memset(&g, 0, sizeof g);
    EMB_REQUIRE(fx_open(&m, 0, &g));
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_CHECK(!embi_font_glyph_outline(&g, gid_d, nullptr, &o));
    EMB_CHECK_EQ_INT(o.quads.len, 0);

    embi_outline_free(&o);
    embi_font_close(&g);
    fx_free(&m);
    embi_font_close(&f);
    fx_free(&src);
}

/* A two-glyph cycle: 'D' is seac(base 'a', accent 'D') and 'a' is seac(base 'D', accent 'D').
 * Following the base from 'D' reaches 'a', whose seac is nested inside a seac, so it fails. */
EMB_TEST(cff1_seac_cycle_fails)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    const uint32_t gid_d = embi_font_glyph_index(&f, 'D');
    const uint32_t gid_a = embi_font_glyph_index(&f, 'a');
    EMB_REQUIRE(gid_d != 0 && gid_a != 0 && gid_d != gid_a);

    const uint8_t d_seac_a[] = {139, 139, 236, 207, 14}; /* base 'a' (97), accent 'D' */
    const uint8_t a_seac_d[] = {139, 139, 207, 207, 14}; /* base 'D', accent 'D' */
    fx_file m2 = fb_copy(&src);
    EMB_REQUIRE(patch_charstring(&src, &f, gid_d, d_seac_a, sizeof d_seac_a, &m2));
    EMB_REQUIRE(patch_charstring(&src, &f, gid_a, a_seac_d, sizeof a_seac_d, &m2));

    embi_font g;
    SDL_memset(&g, 0, sizeof g);
    EMB_REQUIRE(fx_open(&m2, 0, &g));
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_CHECK(!embi_font_glyph_outline(&g, gid_d, nullptr, &o));
    EMB_CHECK(!embi_font_glyph_outline(&g, gid_a, nullptr, &o));

    embi_outline_free(&o);
    embi_font_close(&g);
    fx_free(&m2);
    embi_font_close(&f);
    fx_free(&src);
}

/* A valid seac is still accepted: 'D' becomes seac(base 'a', accent 'b'), two plain glyphs
 * drawn at the origin. The result is non-empty and every contour is closed. */
EMB_TEST(cff1_valid_seac_still_composes)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    const uint32_t gid_d = embi_font_glyph_index(&f, 'D');
    EMB_REQUIRE(gid_d != 0 && embi_font_glyph_index(&f, 'a') != 0 &&
                embi_font_glyph_index(&f, 'b') != 0);

    const uint8_t d_seac_ab[] = {139, 139, 236, 237, 14}; /* base 'a', accent 'b' */
    fx_file m = fb_copy(&src);
    EMB_REQUIRE(patch_charstring(&src, &f, gid_d, d_seac_ab, sizeof d_seac_ab, &m));
    embi_font g;
    SDL_memset(&g, 0, sizeof g);
    EMB_REQUIRE(fx_open(&m, 0, &g));
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&g, gid_d, nullptr, &o));
    EMB_CHECK(o.quads.len > 0);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&g);
    fx_free(&m);
    embi_font_close(&f);
    fx_free(&src);
}

/* ------------------------------------------------------------------ synthetic CFF1 */

/* Outline of glyph gid of a synthetic font, with the bbox checked against the expected box. */
static void check_synth_bbox(const embi_font *f, uint32_t gid, double x0, double y0, double x1,
                             double y1)
{
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(f, gid, nullptr, &o));
    EMB_CHECK(o.quads.len > 0);
    EMB_CHECK_NEAR(o.xmin, x0, 1.5);
    EMB_CHECK_NEAR(o.ymin, y0, 1.5);
    EMB_CHECK_NEAR(o.xmax, x1, 1.5);
    EMB_CHECK_NEAR(o.ymax, y1, 1.5);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
    embi_outline_free(&o);
}

/* Each synthetic charstring draws the shape its comment in font_build.h describes: the operator
 * paths hintmask, flex, flex1, hflex, hflex1, subr calls with bias, and roll. The font also has
 * a 60-operand Private DICT, which must open (CFF1 allows 48, the CFF2 limit is 513). */
EMB_TEST(synth_cff_operator_paths_draw_expected_bboxes)
{
    fx_file src;
    EMB_REQUIRE(fb_synth_cff_font(1000, &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_REQUIRE(f.cff_data != nullptr);
    EMB_CHECK_EQ_INT(f.metrics.num_glyphs, FB_NSYNTH_GLYPHS);

    check_synth_bbox(&f, 1, 0, 0, 100, 100);   /* square */
    check_synth_bbox(&f, 2, 0, 0, 100, 100);   /* hintmask skips its mask byte */
    check_synth_bbox(&f, 3, 0, 0, 100, 100);   /* flex */
    check_synth_bbox(&f, 4, 0, 0, 300, 50);    /* hflex */
    check_synth_bbox(&f, 5, 0, 0, 300, 50);    /* hflex1 */
    check_synth_bbox(&f, 6, 0, 0, 100, 100);   /* flex1 */
    check_synth_bbox(&f, 7, 0, 0, 100, 100);   /* -107 callsubr: bias 107 selects subr 0 */

    /* Dotsection (escape 0) is ignored, so the glyph is empty but valid. */
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 10, nullptr, &o));
    EMB_CHECK_EQ_INT(o.quads.len, 0);

    /* 0 100 2 1 roll gives 100 0, so the line ends at (100, 0). */
    EMB_REQUIRE(embi_font_glyph_outline(&f, 11, nullptr, &o));
    EMB_CHECK_NEAR(o.xmax, 100.0, 0.01);
    EMB_CHECK_NEAR(o.ymax, 0.0, 0.01);
    EMB_CHECK_NEAR(o.ymin, 0.0, 0.01);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* A value that overflows to infinity must fail at the operator that needs an integer (vsindex,
 * index) and not convert it: the glyph gives false and the font stays usable. */
EMB_TEST(synth_cff_overflowing_operands_fail)
{
    fx_file src;
    EMB_REQUIRE(fb_synth_cff_font(1000, &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_CHECK(!embi_font_glyph_outline(&f, 8, nullptr, &o));
    EMB_CHECK(!embi_font_glyph_outline(&f, 9, nullptr, &o));
    /* The good glyph after them still works. */
    EMB_REQUIRE(embi_font_glyph_outline(&f, 1, nullptr, &o));
    EMB_CHECK(o.quads.len > 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Without a FontMatrix the charstring units are head.unitsPerEm units. A 2048 em font with the
 * same square then has a 100 unit square, not 204.8. */
EMB_TEST(synth_cff_default_fontmatrix_follows_units_per_em)
{
    fx_file src;
    EMB_REQUIRE(fb_synth_cff_font(2048, &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(f.metrics.units_per_em, 2048);

    check_synth_bbox(&f, 1, 0, 0, 100, 100);

    embi_font_close(&f);
    fx_free(&src);
}

/* ------------------------------------------------------------------ CFF2 FDSelect */

/* CFF2 INDEX (u32 count) with 1-byte offsets. */
static void cff2_index(fb_buf *b, const fb_seg *items, uint32_t n)
{
    fb_u32(b, n);
    if (n == 0) {
        return;
    }
    fb_u8(b, 1);
    uint32_t off = 1;
    for (uint32_t i = 0; i <= n; i++) {
        fb_u8(b, off);
        if (i < n) {
            off += (uint32_t)items[i].n;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        fb_bytes(b, items[i].p, items[i].n);
    }
}

/* Every glyph is rmoveto 0 0 then -107 callsubr (subr 0 of its font dict). */
static const uint8_t cff2_cs[] = {139, 139, 21, 32, 10};
/* FD 1's subr 0: a 200 unit square (FD 0 uses fb_subr0, a 100 unit square). */
static const uint8_t cff2_subr200[] = {28, 0, 200, 139, 139, 28, 0, 200, 28, 255, 56, 139, 5, 11};

#define CFF2_NGLYPHS 6u

/* Builds a CFF2 table with two font dicts, each with its own Private DICT and Subrs, and an
 * FDSelect of the given format (3 or 4) mapping glyphs 0-1 to FD 0, 2-3 to FD 1 and 4-5 to FD 0.
 * Layout: header, top DICT, global subrs, CharStrings, FDArray, FDSelect, then Private 0 + its
 * Subrs and Private 1 + its Subrs. */
static void cff2_build(fb_buf *out, uint32_t fmt)
{
    fb_seg cs[CFF2_NGLYPHS];
    for (uint32_t i = 0; i < CFF2_NGLYPHS; i++) {
        cs[i] = (fb_seg)FB_SEG(cff2_cs);
    }
    fb_u8(out, 2); /* major, minor, hdrSize, topDictLength */
    fb_u8(out, 0);
    fb_u8(out, 5);
    fb_u16(out, 20);
    /* Top DICT: CharStrings (17), FDArray (12 36), FDSelect (12 37); offsets patched below. */
    fb_dict_i32(out, 0);
    fb_u8(out, 17);
    fb_dict_i32(out, 0);
    fb_u8(out, 12);
    fb_u8(out, 36);
    fb_dict_i32(out, 0);
    fb_u8(out, 12);
    fb_u8(out, 37);
    fb_u32(out, 0); /* global subr INDEX: empty */

    fb_patch32(out, 6, (uint32_t)out->n);
    cff2_index(out, cs, CFF2_NGLYPHS);

    /* FDArray: two dicts "size offset Private(18)", 11 bytes each, offsets patched later. */
    fb_patch32(out, 12, (uint32_t)out->n);
    fb_u32(out, 2);
    fb_u8(out, 1);
    fb_u8(out, 1);
    fb_u8(out, 12);
    fb_u8(out, 23);
    size_t fd_pos[2];
    for (int i = 0; i < 2; i++) {
        fd_pos[i] = out->n;
        fb_dict_i32(out, 6);
        fb_dict_i32(out, 0);
        fb_u8(out, 18);
    }
    /* Each Private DICT is 6 bytes: the Subrs offset operand (5) and operator 19. */

    fb_patch32(out, 19, (uint32_t)out->n);
    if (fmt == 4) {
        fb_u8(out, 4);
        fb_u32(out, 3);
        fb_u32(out, 0);
        fb_u16(out, 0);
        fb_u32(out, 2);
        fb_u16(out, 1);
        fb_u32(out, 4);
        fb_u16(out, 0);
        fb_u32(out, CFF2_NGLYPHS);
    } else {
        fb_u8(out, 3);
        fb_u16(out, 3);
        fb_u16(out, 0);
        fb_u8(out, 0);
        fb_u16(out, 2);
        fb_u8(out, 1);
        fb_u16(out, 4);
        fb_u8(out, 0);
        fb_u16(out, CFF2_NGLYPHS);
    }

    const fb_seg subrs[2] = {{fb_subr0, sizeof fb_subr0}, {cff2_subr200, sizeof cff2_subr200}};
    for (int i = 0; i < 2; i++) {
        fb_patch32(out, fd_pos[i] + 6, (uint32_t)out->n);
        fb_dict_i32(out, 6); /* Subrs offset: right after this 6 byte Private DICT */
        fb_u8(out, 19);
        cff2_index(out, &subrs[i], 1);
    }
}

static bool cff2_font(uint32_t fmt, fx_file *out)
{
    fb_buf cff = {0};
    cff2_build(&cff, fmt);
    const fb_table extra[1] = {{EMBI_TAG('C', 'F', 'F', '2'), cff.p, cff.n}};
    const bool ok = fb_font(EMBI_TAG('O', 'T', 'T', 'O'), 1000, CFF2_NGLYPHS, extra, 1, out);
    fb_free(&cff);
    return ok;
}

/* FDSelect format 3 and format 4 (CFF2 only) pick the font dict per glyph range, and the Subrs
 * of that dict are the ones called: glyphs 0-1 and 4-5 draw the 100 unit square (FD 0), glyphs
 * 2-3 the 200 unit square (FD 1). */
EMB_TEST(cff2_fdselect_formats_3_and_4_pick_the_right_fd)
{
    for (uint32_t fmt = 3; fmt <= 4; fmt++) {
        fx_file src;
        EMB_REQUIRE(cff2_font(fmt, &src));
        embi_font f;
        SDL_memset(&f, 0, sizeof f);
        EMB_REQUIRE(fx_open(&src, 0, &f));
        EMB_REQUIRE(f.cff_data != nullptr);
        EMB_CHECK(f.metrics.is_cff2);
        EMB_CHECK_EQ_INT(f.cff_data->nfds, 2);
        EMB_CHECK_EQ_INT(f.cff_data->fdselect_fmt, fmt);
        for (uint32_t g = 0; g < CFF2_NGLYPHS; g++) {
            const double side = (g == 2 || g == 3) ? 200.0 : 100.0;
            check_synth_bbox(&f, g, 0, 0, side, side);
        }
        embi_outline o;
        SDL_memset(&o, 0, sizeof o);
        EMB_CHECK(!embi_font_glyph_outline(&f, CFF2_NGLYPHS, nullptr, &o));
        embi_outline_free(&o);
        embi_font_close(&f);
        fx_free(&src);
    }
}

/* A glyph past the last range (the sentinel) has no font dict and fails, for both formats. */
EMB_TEST(cff2_fdselect_gid_past_sentinel_fails)
{
    for (uint32_t fmt = 3; fmt <= 4; fmt++) {
        fx_file src;
        EMB_REQUIRE(cff2_font(fmt, &src));
        /* Shrink the sentinel to 5: glyph 5 is then outside every range. */
        embi_font probe;
        SDL_memset(&probe, 0, sizeof probe);
        EMB_REQUIRE(fx_open(&src, 0, &probe));
        const embi_bytes fds = probe.cff_data->fdselect;
        const size_t sent = (size_t)(fds.p - src.p) + (fmt == 4 ? 5 + 6 * 3 : 3 + 3 * 3);
        embi_font_close(&probe);
        if (fmt == 4) {
            src.p[sent + 3] = 5; /* low byte of the u32 sentinel (its high bytes are 0) */
        } else {
            src.p[sent + 1] = 5;
        }
        embi_font f;
        SDL_memset(&f, 0, sizeof f);
        EMB_REQUIRE(fx_open(&src, 0, &f));
        embi_outline o;
        SDL_memset(&o, 0, sizeof o);
        EMB_CHECK(embi_font_glyph_outline(&f, 4, nullptr, &o));
        EMB_CHECK(!embi_font_glyph_outline(&f, 5, nullptr, &o));
        embi_outline_free(&o);
        embi_font_close(&f);
        fx_free(&src);
    }
}

EMB_TEST_MAIN()
