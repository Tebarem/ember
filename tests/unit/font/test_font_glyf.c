#include "font_build.h"

/* TrueType (glyf) outlines of NotoSans-Regular.ttf. For simple glyphs the glyf header bbox is the
 * tight bbox of the outline (font compilers compute it from the curves), so the header is the
 * reference for the outline bounds. */

/* Glyph 36 ('A'): header bbox (0, 0, 638, 717), numberOfContours 2 (outer and counter).
 * Advance 639. */
EMB_TEST(glyph_a_outline_matches_header_bbox)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, nullptr, &o));
    EMB_CHECK_EQ_INT(o.contours.len, 2);
    EMB_CHECK(o.quads.len > 0);
    EMB_CHECK_NEAR(o.xmin, 0.0, 0.01);
    EMB_CHECK_NEAR(o.ymin, 0.0, 0.01);
    EMB_CHECK_NEAR(o.xmax, 638.0, 0.01);
    EMB_CHECK_NEAR(o.ymax, 717.0, 0.01);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Glyph 131 ('Aacute') is a composite: header numberOfContours is -1 and bbox (0, 0, 638, 944).
 * It is the 'A' outline (2 contours) plus an accent, so it flattens to at least 3 contours. */
EMB_TEST(composite_aacute_flattens_with_header_bbox)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xC1), 131);

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 131, nullptr, &o));
    EMB_CHECK(o.contours.len >= 3);
    EMB_CHECK_NEAR(o.xmin, 0.0, 0.01);
    EMB_CHECK_NEAR(o.ymin, 0.0, 0.01);
    EMB_CHECK_NEAR(o.xmax, 638.0, 0.01);
    EMB_CHECK_NEAR(o.ymax, 944.0, 0.01);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Every printable ASCII glyph: contours are closed, and every quad endpoint lies inside the
 * outline bbox (control points may lie outside, since the bbox is the tight curve bound). */
EMB_TEST(ascii_outlines_are_closed_and_bounded)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    int glyphs = 0;
    for (uint32_t cp = 0x21; cp <= 0x7E; cp++) {
        const uint32_t gid = embi_font_glyph_index(&f, cp);
        EMB_REQUIRE(gid != 0);
        EMB_REQUIRE(embi_font_glyph_outline(&f, gid, nullptr, &o));
        EMB_CHECK(o.quads.len > 0);
        EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
        for (uint32_t q = 0; q < o.quads.len; q++) {
            const embi_pt ends[2] = {o.quads.data[q].p0, o.quads.data[q].p2};
            for (int k = 0; k < 2; k++) {
                const double eps = 0.01;
                if (ends[k].x < o.xmin - eps || ends[k].x > o.xmax + eps ||
                    ends[k].y < o.ymin - eps || ends[k].y > o.ymax + eps) {
                    EMB_CHECK(false);
                }
            }
        }
        glyphs++;
    }
    EMB_CHECK_EQ_INT(glyphs, 94); /* 0x21..0x7E */

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* Glyph 50 ('O') of NotoSans-Regular. Raw glyf data (read with fontTools getCoordinates): 2
 * contours, 28 points of which 8 are on-curve, so 20 off-curve points. Every off-curve point
 * ends exactly one quad and this outline has no standalone line segments, so 20 quads. If the
 * on-curve bit were mishandled (any flag bit counted as on-curve) the glyph would degrade to 28
 * straight lines, so also require that every quad is a real curve (control point off the chord
 * midpoint). */
EMB_TEST(curved_glyph_o_emits_curves)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 'O'), 50);

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 50, nullptr, &o));
    EMB_CHECK_EQ_INT(o.contours.len, 2);
    EMB_CHECK_EQ_INT(o.quads.len, 20);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
    uint32_t curved = 0;
    for (uint32_t i = 0; i < o.quads.len; i++) {
        const embi_quad *q = &o.quads.data[i];
        const float mx = (q->p0.x + q->p2.x) * 0.5f, my = (q->p0.y + q->p2.y) * 0.5f;
        if (SDL_fabsf(q->p1.x - mx) > 0.01f || SDL_fabsf(q->p1.y - my) > 0.01f) {
            curved++;
        }
    }
    EMB_CHECK_EQ_INT(curved, 20);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

static bool pt_is(embi_pt p, float x, float y)
{
    return SDL_fabsf(p.x - x) < 1e-4f && SDL_fabsf(p.y - y) < 1e-4f;
}

static bool quad_is(const embi_quad *q, float x0, float y0, float x1, float y1, float x2, float y2)
{
    return pt_is(q->p0, x0, y0) && pt_is(q->p1, x1, y1) && pt_is(q->p2, x2, y2);
}

/* One simple glyph with a single contour, explicit 16-bit deltas (flags hold only the on-curve
 * bit). Padded to an even length for the short loca. */
static void gl_simple(fb_buf *g, const int *x, const int *y, const uint8_t *on, uint32_t n)
{
    fb_u16(g, 1);
    fb_u16(g, 0);
    fb_u16(g, 0);
    fb_u16(g, 100);
    fb_u16(g, 100);
    fb_u16(g, n - 1);
    fb_u16(g, 0); /* instructionLength */
    for (uint32_t i = 0; i < n; i++) {
        fb_u8(g, on[i] ? 1 : 0);
    }
    int px = 0;
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(g, (uint32_t)(x[i] - px) & 0xFFFFu);
        px = x[i];
    }
    int py = 0;
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(g, (uint32_t)(y[i] - py) & 0xFFFFu);
        py = y[i];
    }
    while (g->n & 1) {
        fb_u8(g, 0);
    }
}

/* Synthetic glyf font, 4 glyphs, short loca, known contours:
 *  gid 1: four off-curve points (0,0) (100,0) (100,100) (0,100): no on-curve point, so the start
 *         is the midpoint of the last and first points, (0,50). 4 quads, neighbouring control
 *         points joined at their midpoints.
 *  gid 2: (0,0) off, (100,0) on, (100,100) on, (0,100) on: starts at the first on-curve point
 *         (100,0), lines to (100,100) and (0,100), then a quad with control (0,0) back to start.
 *  gid 3: on (0,0), off (100,0), on (100,100), encoded with X_SHORT/Y_SHORT/SAME flag bits set
 *         on the off-curve point, which must not make it on-curve. */
EMB_TEST(synthetic_glyf_implied_points)
{
    fb_buf glyf = {0}, loca = {0};
    fb_u16(&loca, 0); /* gid 0: empty */
    fb_u16(&loca, 0);
    {
        const int x[4] = {0, 100, 100, 0}, y[4] = {0, 0, 100, 100};
        const uint8_t on[4] = {0, 0, 0, 0};
        gl_simple(&glyf, x, y, on, 4);
        fb_u16(&loca, (uint32_t)glyf.n / 2);
    }
    {
        const int x[4] = {0, 100, 100, 0}, y[4] = {0, 0, 100, 100};
        const uint8_t on[4] = {0, 1, 1, 1};
        gl_simple(&glyf, x, y, on, 4);
        fb_u16(&loca, (uint32_t)glyf.n / 2);
    }
    {
        /* header, endPts {2}, no instructions, flags, x bytes, y bytes. Flag bits: 1 on-curve,
         * 2 X_SHORT, 4 Y_SHORT, 0x10 X_SAME/positive, 0x20 Y_SAME/positive. */
        static const uint8_t g3[] = {
            0, 1, 0, 0, 0, 0, 0, 100, 0, 100, 0, 2, 0, 0,
            0x31, /* on, x same, y same: (0,0) */
            0x32, /* off, x short positive, y same: (100,0) */
            0x35, /* on, x same, y short positive: (100,100) */
            100,  /* x delta of point 1 */
            100,  /* y delta of point 2 */
        };
        fb_bytes(&glyf, g3, sizeof g3);
        while (glyf.n & 1) {
            fb_u8(&glyf, 0);
        }
        fb_u16(&loca, (uint32_t)glyf.n / 2);
    }
    fb_table extra[2] = {{EMBI_TAG('g', 'l', 'y', 'f'), glyf.p, glyf.n},
                         {EMBI_TAG('l', 'o', 'c', 'a'), loca.p, loca.n}};
    fx_file src = {0};
    EMB_REQUIRE(fb_font(0x00010000u, 1000, 4, extra, 2, &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 1, nullptr, &o));
    EMB_CHECK_EQ_INT(o.contours.len, 1);
    EMB_REQUIRE(o.quads.len == 4);
    EMB_CHECK(quad_is(&o.quads.data[0], 0, 50, 0, 0, 50, 0));
    EMB_CHECK(quad_is(&o.quads.data[1], 50, 0, 100, 0, 100, 50));
    EMB_CHECK(quad_is(&o.quads.data[2], 100, 50, 100, 100, 50, 100));
    EMB_CHECK(quad_is(&o.quads.data[3], 50, 100, 0, 100, 0, 50));

    EMB_REQUIRE(embi_font_glyph_outline(&f, 2, nullptr, &o));
    EMB_REQUIRE(o.quads.len == 3);
    EMB_CHECK(quad_is(&o.quads.data[0], 100, 0, 100, 50, 100, 100));
    EMB_CHECK(quad_is(&o.quads.data[1], 100, 100, 50, 100, 0, 100));
    EMB_CHECK(quad_is(&o.quads.data[2], 0, 100, 0, 0, 100, 0));

    EMB_REQUIRE(embi_font_glyph_outline(&f, 3, nullptr, &o));
    EMB_REQUIRE(o.quads.len == 2);
    EMB_CHECK(quad_is(&o.quads.data[0], 0, 0, 100, 0, 100, 100));
    EMB_CHECK(quad_is(&o.quads.data[1], 100, 100, 50, 50, 0, 0));

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
    fb_free(&glyf);
    fb_free(&loca);
}

/* Reusing an outline resets it: a second fill of the same glyph gives the same counts. An
 * out-of-range glyph id fails. */
EMB_TEST(outline_reuse_and_bad_gid)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, nullptr, &o));
    const uint32_t quads = o.quads.len;
    const uint32_t contours = o.contours.len;
    EMB_REQUIRE(embi_font_glyph_outline(&f, 36, nullptr, &o));
    EMB_CHECK_EQ_INT(o.quads.len, quads);
    EMB_CHECK_EQ_INT(o.contours.len, contours);

    EMB_CHECK(!embi_font_glyph_outline(&f, 1000000u, nullptr, &o));

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}


/* Byte offset of glyph gid's data inside the glyf table. */
static size_t glyf_data_offset(const embi_font *f, uint32_t gid)
{
    const uint8_t *p = f->loca.p;
    if (f->loca_long) {
        return ((size_t)p[4 * gid] << 24) | ((size_t)p[4 * gid + 1] << 16) |
               ((size_t)p[4 * gid + 2] << 8) | p[4 * gid + 3];
    }
    return (((size_t)p[2 * gid] << 8) | p[2 * gid + 1]) * 2;
}

/* Aacute (131) is a composite of A and an accent, and its components move under gvar. Reference
 * from fontTools 4.68 BoundsPen on getGlyphSet(location) of NotoSans-VF.ttf:
 *   wght 100: (0, 0, 577, 952),   wght 900: (0, 0, 726, 927).
 * The composite offsets come from gvar as well, so this checks the gvar composite path. */
EMB_TEST(composite_aacute_gvar_bbox_at_weight_extremes)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    const int16_t lo[2] = {-16384, 0};
    const int16_t hi[2] = {16384, 0};
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 131, lo, &o));
    EMB_CHECK(o.contours.len >= 3);
    EMB_CHECK_NEAR(o.xmin, 0.0, 0.5);
    EMB_CHECK_NEAR(o.ymin, 0.0, 0.5);
    EMB_CHECK_NEAR(o.xmax, 577.0, 0.5);
    EMB_CHECK_NEAR(o.ymax, 952.0, 0.5);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    EMB_REQUIRE(embi_font_glyph_outline(&f, 131, hi, &o));
    EMB_CHECK_NEAR(o.xmin, 0.0, 0.5);
    EMB_CHECK_NEAR(o.xmax, 726.0, 0.5);
    EMB_CHECK_NEAR(o.ymax, 927.0, 0.5);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);

    embi_outline_free(&o);
    embi_font_close(&f);
    fx_free(&src);
}

/* A composite whose first component refers to itself is a cycle. The depth limit and the
 * component budget stop it, so the glyph fails without exhausting time or memory. The copy
 * patches the component glyph index of Aacute (offset 12 of the glyph, after the 10 byte header
 * and the 2 byte flags). */
EMB_TEST(composite_self_reference_fails_within_budget)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_REQUIRE(f.metrics.num_glyphs > 131);

    fx_file m = fb_copy(&src);
    EMB_REQUIRE(m.p != nullptr);
    const size_t at = (size_t)(f.glyf.p - src.p) + glyf_data_offset(&f, 131) + 12;
    m.p[at] = 0x00;
    m.p[at + 1] = 131;

    embi_font g;
    SDL_memset(&g, 0, sizeof g);
    EMB_REQUIRE(fx_open(&m, 0, &g));
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    EMB_CHECK(!embi_font_glyph_outline(&g, 131, nullptr, &o));
    EMB_CHECK_EQ_INT(o.quads.len, 0);

    embi_outline_free(&o);
    embi_font_close(&g);
    fx_free(&m);
    embi_font_close(&f);
    fx_free(&src);
}

EMB_TEST_MAIN()
