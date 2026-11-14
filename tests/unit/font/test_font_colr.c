#include "font_build.h"

#include <math.h>

/* CPAL palettes and the COLRv1 paint graph of Noto-COLRv1.ttf.
 *
 * Reference (Python over the raw tables, same DFS order the visitor uses):
 *   CPAL: 1 palette, 5081 entries, records at offset 14. Entry BGRA bytes: entry 0 (0,0,0,255),
 *     entry 1 (13,0,0,255), entry 2 (0,2,0,255), entry 3 (2,2,0,255), all as B,G,R,A. So RGBA is
 *     entry 0 (0,0,0,255), entry 1 (0,0,13,255), entry 2 (0,2,0,255), entry 3 (0,2,2,255).
 *   COLR base glyph 1849: paint format 1 (PaintColrLayers) with 10 leaf paints (9 PaintSolid,
 *     1 PaintRadialGradient), 10 PaintGlyph clips, 5 PaintTransform (format 12). No composite
 *     groups. The first transform (format 12, Fixed/65536) is
 *     [72437, 0, 0, 72437, -4516499, 1273332] / 65536 = [1.10530, 0, 0, 1.10530, -68.9163,
 *     19.4295]. */

typedef struct colr_count {
    uint32_t total;
    uint32_t xf_push, xf_pop;
    uint32_t clip_push, clip_pop;
    uint32_t group_push, group_pop;
    uint32_t paint;
    uint32_t solid, radial, linear, sweep;
    int32_t depth_xf, depth_clip, depth_group;
    bool depth_negative;
    bool have_xf;
    float first_xf[6];
    bool have_paint;
    float first_paint_rgba[4];
    uint32_t stop_after; /* 0 never stops early */
} colr_count;

static bool count_op(void *ctx, const embi_colr_op *op)
{
    colr_count *c = (colr_count *)ctx;
    c->total++;
    if (op->kind == EMBI_COLR_TRANSFORM_PUSH) {
        c->xf_push++;
        c->depth_xf++;
        if (!c->have_xf) {
            c->have_xf = true;
            for (int i = 0; i < 6; i++) {
                c->first_xf[i] = op->xf[i];
            }
        }
    } else if (op->kind == EMBI_COLR_TRANSFORM_POP) {
        c->xf_pop++;
        c->depth_xf--;
    } else if (op->kind == EMBI_COLR_CLIP_PUSH) {
        c->clip_push++;
        c->depth_clip++;
    } else if (op->kind == EMBI_COLR_CLIP_POP) {
        c->clip_pop++;
        c->depth_clip--;
    } else if (op->kind == EMBI_COLR_GROUP_PUSH) {
        c->group_push++;
        c->depth_group++;
    } else if (op->kind == EMBI_COLR_GROUP_POP) {
        c->group_pop++;
        c->depth_group--;
    } else {
        c->paint++;
        if (op->paint.kind == EMBI_PAINT_SOLID) {
            c->solid++;
        } else if (op->paint.kind == EMBI_PAINT_RADIAL) {
            c->radial++;
        } else if (op->paint.kind == EMBI_PAINT_LINEAR) {
            c->linear++;
        } else {
            c->sweep++;
        }
        if (!c->have_paint) {
            c->have_paint = true;
            for (int i = 0; i < 4; i++) {
                c->first_paint_rgba[i] = op->paint.rgba[i];
            }
        }
    }
    if (c->depth_xf < 0 || c->depth_clip < 0 || c->depth_group < 0) {
        c->depth_negative = true;
    }
    if (c->stop_after != 0 && c->total == c->stop_after) {
        return false;
    }
    return true;
}

static void count_init(colr_count *c, uint32_t stop_after)
{
    SDL_memset(c, 0, sizeof *c);
    c->stop_after = stop_after;
}

EMB_TEST(cpal_entries_match_raw_bgra)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    EMB_CHECK_EQ_INT(embi_font_palette_count(&f), 1);

    uint8_t c[4] = {0, 0, 0, 0};
    const uint8_t want[4][4] = {
        {0, 0, 0, 255}, {0, 0, 13, 255}, {0, 2, 0, 255}, {0, 2, 2, 255}};
    for (uint32_t e = 0; e < 4; e++) {
        EMB_REQUIRE(embi_font_cpal_color(&f, 0, e, c));
        for (int k = 0; k < 4; k++) {
            EMB_CHECK_EQ_INT(c[k], want[e][k]);
        }
    }
    /* Past the 5081 entries, and an unknown palette, both fail. */
    EMB_CHECK(!embi_font_cpal_color(&f, 0, 5081, c));
    EMB_CHECK(!embi_font_cpal_color(&f, 9, 0, c));

    embi_font_close(&f);
    fx_free(&src);
}

/* Glyph 1849 counts from the reference above: 5 transform push/pop pairs, 10 clip push/pop pairs,
 * no groups, 10 paints (9 solid, 1 radial). The depth counters end at zero. */
EMB_TEST(colr_glyph_1849_op_counts)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    colr_count c;
    count_init(&c, 0);
    const embi_colr_visitor v = {&c, count_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 1849, nullptr, 0, &v));

    EMB_CHECK_EQ_INT(c.xf_push, 5);
    EMB_CHECK_EQ_INT(c.xf_pop, 5);
    EMB_CHECK_EQ_INT(c.clip_push, 10);
    EMB_CHECK_EQ_INT(c.clip_pop, 10);
    EMB_CHECK_EQ_INT(c.group_push, 0);
    EMB_CHECK_EQ_INT(c.group_pop, 0);
    EMB_CHECK_EQ_INT(c.paint, 10);
    EMB_CHECK_EQ_INT(c.solid, 9);
    EMB_CHECK_EQ_INT(c.radial, 1);
    EMB_CHECK_EQ_INT(c.linear, 0);
    EMB_CHECK_EQ_INT(c.sweep, 0);
    EMB_CHECK(!c.depth_negative);
    EMB_CHECK_EQ_INT(c.depth_xf, 0);
    EMB_CHECK_EQ_INT(c.depth_clip, 0);
    EMB_CHECK_EQ_INT(c.depth_group, 0);

    /* The first transform is the raw Fixed values divided by 65536. */
    EMB_CHECK(c.have_xf);
    EMB_CHECK_NEAR(c.first_xf[0], 1.1053009033203125, 1e-6);
    EMB_CHECK_NEAR(c.first_xf[1], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.first_xf[2], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.first_xf[3], 1.1053009033203125, 1e-6);
    EMB_CHECK_NEAR(c.first_xf[4], -68.91630554199219, 1e-4);
    EMB_CHECK_NEAR(c.first_xf[5], 19.42950439453125, 1e-4);

    embi_font_close(&f);
    fx_free(&src);
}

/* The visitor can stop the traversal: the call then returns false after the op that asked. */
EMB_TEST(colr_visitor_can_stop_early)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    colr_count c;
    count_init(&c, 3);
    const embi_colr_visitor v = {&c, count_op};
    EMB_CHECK(!embi_font_colr_paint(&f, 1849, nullptr, 0, &v));
    EMB_CHECK_EQ_INT(c.total, 3);

    embi_font_close(&f);
    fx_free(&src);
}

/* A palette index past the palette count falls back to palette 0: the same first paint colour
 * and op count. */
EMB_TEST(colr_palette_out_of_range_uses_palette_zero)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    colr_count p0, p7;
    count_init(&p0, 0);
    count_init(&p7, 0);
    const embi_colr_visitor v0 = {&p0, count_op};
    const embi_colr_visitor v7 = {&p7, count_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 1849, nullptr, 0, &v0));
    EMB_REQUIRE(embi_font_colr_paint(&f, 1849, nullptr, 7, &v7));
    EMB_CHECK_EQ_INT(p7.total, p0.total);
    for (int i = 0; i < 4; i++) {
        EMB_CHECK_NEAR(p7.first_paint_rgba[i], p0.first_paint_rgba[i], 1e-6);
    }

    embi_font_close(&f);
    fx_free(&src);
}

/* Glyph 1 is not in the COLR base glyph list (gid 36 is), so it gives false and emits
 * nothing. */
EMB_TEST(colr_glyph_without_record_fails)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    colr_count c;
    count_init(&c, 0);
    const embi_colr_visitor v = {&c, count_op};
    EMB_CHECK(!embi_font_colr_paint(&f, 1, nullptr, 0, &v));
    EMB_CHECK_EQ_INT(c.total, 0);

    embi_font_close(&f);
    fx_free(&src);
}


/* ------------------------------------------------------------------ paint capture */

/* Keeps the first paints (with their stops), transforms, clips and composite modes of a walk. */
typedef struct cap_paint {
    embi_colr_paint_kind kind;
    uint8_t extend;
    float p[6];
    float rgba[4];
    bool fg;
    uint32_t nstops;
    embi_colr_stop stops[8];
} cap_paint;

typedef struct cap {
    embi_colr_op_kind kinds[32];
    uint32_t nops;
    cap_paint paints[4];
    uint32_t npaints;
    float xf[4][6];
    uint32_t nxf;
    uint32_t clip_gid[4];
    uint32_t nclip;
    uint8_t group_mode[4];
    uint32_t ngroup;
} cap;

static bool cap_op(void *ctx, const embi_colr_op *op)
{
    cap *c = (cap *)ctx;
    if (c->nops < 32) {
        c->kinds[c->nops] = op->kind;
    }
    c->nops++;
    if (op->kind == EMBI_COLR_TRANSFORM_PUSH) {
        if (c->nxf < 4) {
            for (int i = 0; i < 6; i++) {
                c->xf[c->nxf][i] = op->xf[i];
            }
        }
        c->nxf++;
    } else if (op->kind == EMBI_COLR_CLIP_PUSH) {
        if (c->nclip < 4) {
            c->clip_gid[c->nclip] = op->gid;
        }
        c->nclip++;
    } else if (op->kind == EMBI_COLR_GROUP_PUSH) {
        if (c->ngroup < 4) {
            c->group_mode[c->ngroup] = op->composite;
        }
        c->ngroup++;
    } else if (op->kind == EMBI_COLR_PAINT) {
        if (c->npaints < 4) {
            cap_paint *q = &c->paints[c->npaints];
            q->kind = op->paint.kind;
            q->extend = op->paint.extend;
            q->fg = op->paint.foreground;
            for (int i = 0; i < 6; i++) {
                q->p[i] = op->paint.p[i];
            }
            for (int i = 0; i < 4; i++) {
                q->rgba[i] = op->paint.rgba[i];
            }
            q->nstops = op->paint.num_stops < 8 ? op->paint.num_stops : 8;
            for (uint32_t i = 0; i < q->nstops; i++) {
                q->stops[i] = op->paint.stops[i];
            }
        }
        c->npaints++;
    }
    return true;
}

static void cap_init(cap *c)
{
    SDL_memset(c, 0, sizeof *c);
}

/* ------------------------------------------------------------------ synthetic COLR fonts */

/* CPAL with one palette of 3 entries: 0 blue, 1 green, 2 red (all opaque), stored as BGRA. */
static void colr_put_cpal(fb_buf *b)
{
    fb_u16(b, 0);  /* version */
    fb_u16(b, 3);  /* numPaletteEntries */
    fb_u16(b, 1);  /* numPalettes */
    fb_u16(b, 3);  /* numColorRecords */
    fb_u32(b, 14); /* colorRecordsArray, after the palette index list */
    fb_u16(b, 0);  /* colorRecordIndices[0] */
    const uint8_t rec[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    fb_bytes(b, &rec[0][0], sizeof rec);
}

/* Builds the sfnt (head, hhea, maxp, hmtx for 16 glyphs, CPAL, COLR) and opens face 0. */
static bool colr_open_font(const fb_buf *colr, fx_file *src, embi_font *f)
{
    fb_buf cpal = {0};
    colr_put_cpal(&cpal);
    fb_table ex[2] = {{EMBI_TAG('C', 'P', 'A', 'L'), cpal.p, cpal.n},
                      {EMBI_TAG('C', 'O', 'L', 'R'), colr->p, colr->n}};
    const bool ok = fb_font(0x00010000u, 1000, 16, ex, 2, src);
    fb_free(&cpal);
    if (!ok) {
        return false;
    }
    SDL_memset(f, 0, sizeof *f);
    return embi_font_open(src->p, src->n, 0, f);
}

/* COLRv1 header (34 bytes) with the base glyph list at 34 and the layer list at layer_off. */
static void colr_header_v1(fb_buf *b, uint32_t layer_off)
{
    fb_u16(b, 1);         /* version */
    fb_u16(b, 0);         /* numBaseGlyphRecords (v0) */
    fb_u32(b, 0);         /* baseGlyphRecordsOffset */
    fb_u32(b, 0);         /* layerRecordsOffset */
    fb_u16(b, 0);         /* numLayerRecords */
    fb_u32(b, 34);        /* baseGlyphListOffset */
    fb_u32(b, layer_off); /* layerListOffset */
    fb_u32(b, 0);         /* clipListOffset */
    fb_u32(b, 0);         /* varIdxMapOffset */
    fb_u32(b, 0);         /* itemVariationStoreOffset */
}

/* A base glyph list with one record (gid, paint offset relative to the list). */
static void colr_one_base(fb_buf *b, uint32_t gid, uint32_t paint_off_rel)
{
    fb_u32(b, 1);
    fb_u16(b, gid);
    fb_u32(b, paint_off_rel);
}

#define COLR_NO_VAR 0xFFFFFFFFu

/* Linear gradient (format 4) on glyph 5: reflect extend, three stops, alpha on two of them. */
static void colr_build_linear(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 5, 10); /* paint at 34 + 10 = 44 */
    /* PaintLinearGradient at 44: colorLine at +16 (60), then x0 y0 x1 y1 x2 y2. */
    fb_u8(b, 4);
    fb_u24(b, 16);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    /* ColorLine at 60: extend reflect, three stops. */
    fb_u8(b, 2);
    fb_u16(b, 3);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 16384); /* offset 0, palette 0, alpha 1 */
    fb_u16(b, 8192);
    fb_u16(b, 1);
    fb_u16(b, 8192); /* offset 0.5, palette 1, alpha 0.5 */
    fb_u16(b, 16384);
    fb_u16(b, 2);
    fb_u16(b, 8192); /* offset 1, palette 2, alpha 0.5 */
}

/* PaintVarLinearGradient (format 5) on glyph 6: VarColorLine with 10-byte stops, no deltas. */
static void colr_build_var_linear(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 6, 10); /* paint at 44 */
    /* PaintVarLinearGradient at 44: colorLine at +20 (64), geometry, varIndexBase. */
    fb_u8(b, 5);
    fb_u24(b, 20);
    fb_u16(b, 10);
    fb_u16(b, 20);
    fb_u16(b, 30);
    fb_u16(b, 40);
    fb_u16(b, 50);
    fb_u16(b, 60);
    fb_u32(b, COLR_NO_VAR);
    /* VarColorLine at 64: two stops of 10 bytes. */
    fb_u8(b, 0);
    fb_u16(b, 2);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 16384);
    fb_u32(b, COLR_NO_VAR);
    fb_u16(b, 16384);
    fb_u16(b, 2);
    fb_u16(b, 8192);
    fb_u32(b, COLR_NO_VAR);
}

/* Sweep gradient (format 8) on glyph 7: centre (10, 20), angles 90 and 180 degrees, repeat. */
static void colr_build_sweep(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 7, 10); /* paint at 44 */
    /* PaintSweepGradient at 44: colorLine at +12 (56), centre, start and end angles. */
    fb_u8(b, 8);
    fb_u24(b, 12);
    fb_u16(b, 10);
    fb_u16(b, 20);
    fb_u16(b, 8192);  /* 0.5 semicircle: 90 degrees */
    fb_u16(b, 16384); /* 1 semicircle: 180 degrees */
    /* ColorLine at 56. */
    fb_u8(b, 1);
    fb_u16(b, 2);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 16384);
    fb_u16(b, 16384);
    fb_u16(b, 1);
    fb_u16(b, 16384);
}

/* Composite (format 32) on glyph 11: source green, backdrop blue, mode 4. Layout: composite at
 * 44 (8 bytes), source solid at 52, backdrop solid at 57. */
static void colr_build_composite(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 11, 10);
    fb_u8(b, 32);
    fb_u24(b, 8);  /* source, relative to the composite */
    fb_u8(b, 4);   /* compositeMode */
    fb_u24(b, 13); /* backdrop */
    fb_u8(b, 2);   /* PaintSolid: palette 1 (green), alpha 1 */
    fb_u16(b, 1);
    fb_u16(b, 16384);
    fb_u8(b, 2); /* PaintSolid: palette 2 (red) here, used as the backdrop in the test */
    fb_u16(b, 2);
    fb_u16(b, 16384);
}

/* PaintSkew (format 28) on glyph 12 with xSkew and ySkew of 2048 (0.125 semicircle, 22.5 degrees)
 * over a solid child. */
static void colr_build_skew(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 12, 10);
    fb_u8(b, 28);
    fb_u24(b, 8); /* child at 52 */
    fb_u16(b, 2048);
    fb_u16(b, 2048);
    fb_u8(b, 2); /* PaintSolid: palette 0, alpha 1 */
    fb_u16(b, 0);
    fb_u16(b, 16384);
}

/* COLRv0 on glyph 8: two layers (9 with palette 0, 10 with the foreground index 0xFFFF). */
static void colr_build_v0(fb_buf *b)
{
    fb_u16(b, 0);  /* version */
    fb_u16(b, 1);  /* numBaseGlyphRecords */
    fb_u32(b, 14); /* baseGlyphRecordsOffset */
    fb_u32(b, 20); /* layerRecordsOffset */
    fb_u16(b, 2);  /* numLayerRecords */
    fb_u16(b, 8);  /* base glyph 8: first layer 0, two layers */
    fb_u16(b, 0);
    fb_u16(b, 2);
    fb_u16(b, 9);
    fb_u16(b, 0);
    fb_u16(b, 10);
    fb_u16(b, 0xFFFFu);
}

/* A linear gradient with one stop (a solid fill) on glyph 14, and one with no stops (nothing)
 * on glyph 15. Layout: list at 34 (2 records), paint A at 50 (colorLine at 66), paint B at 75
 * (colorLine at 91). */
static void colr_build_degenerate(fb_buf *b)
{
    colr_header_v1(b, 0);
    fb_u32(b, 2);
    fb_u16(b, 14);
    fb_u32(b, 16); /* 34 + 16 = 50 */
    fb_u16(b, 15);
    fb_u32(b, 41); /* 34 + 41 = 75 */
    fb_u8(b, 4);   /* paint A */
    fb_u24(b, 16);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u8(b, 0); /* ColorLine A: pad, one stop */
    fb_u16(b, 1);
    fb_u16(b, 0);
    fb_u16(b, 1);
    fb_u16(b, 16384);
    fb_u8(b, 4); /* paint B */
    fb_u24(b, 16);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u8(b, 0); /* ColorLine B: no stops */
    fb_u16(b, 0);
}

/* PaintColrLayers that refers to itself through the layer list: the walk has no emitting paint,
 * so only the evaluation budget stops it. Layer list at 44 (two entries pointing at the paint at
 * 56), paint at 56 (format 1, two layers from index 0). */
static void colr_build_self_layers(fb_buf *b)
{
    colr_header_v1(b, 44);
    colr_one_base(b, 13, 22); /* paint at 34 + 22 = 56 */
    fb_u32(b, 2);             /* LayerList at 44 */
    fb_u32(b, 12);            /* 44 + 12 = 56 */
    fb_u32(b, 12);
    fb_u8(b, 1); /* PaintColrLayers at 56 */
    fb_u8(b, 2);
    fb_u32(b, 0);
}

/* ------------------------------------------------------------------ tests */

/* The radial gradient of glyph 1849 (the one in the op count test). Reference from fontTools
 * 4.68 on the raw COLR and CPAL tables: 3 stops, extend pad, geometry (638, 299) r0 0 to
 * (638, 299) r1 483, with
 *   offset 0.5 (palette 4332 = 253, 224, 48, alpha 1),
 *   offset 0.91998291015625 (palette 4034 = 247, 192, 43, alpha 1),
 *   offset 1.0 (palette 3924 = 244, 162, 35, alpha 1). */
EMB_TEST(colr_glyph_1849_radial_stops)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 1849, nullptr, 0, &v));

    /* The first paint op is the radial gradient (the first leaf of the first layer). */
    EMB_REQUIRE(c.npaints > 0);
    const cap_paint *g = &c.paints[0];
    EMB_CHECK(g->kind == EMBI_PAINT_RADIAL);
    EMB_CHECK_EQ_INT(g->extend, 0);
    EMB_CHECK_NEAR(g->p[0], 638.0, 1e-4);
    EMB_CHECK_NEAR(g->p[1], 299.0, 1e-4);
    EMB_CHECK_NEAR(g->p[2], 0.0, 1e-4);
    EMB_CHECK_NEAR(g->p[3], 638.0, 1e-4);
    EMB_CHECK_NEAR(g->p[4], 299.0, 1e-4);
    EMB_CHECK_NEAR(g->p[5], 483.0, 1e-4);

    EMB_REQUIRE(g->nstops == 3);
    EMB_CHECK_NEAR(g->stops[0].offset, 0.5, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].offset, 0.91998291015625, 1e-6);
    EMB_CHECK_NEAR(g->stops[2].offset, 1.0, 1e-6);
    const double want[3][3] = {{253, 224, 48}, {247, 192, 43}, {244, 162, 35}};
    for (int i = 0; i < 3; i++) {
        EMB_CHECK(!g->stops[i].foreground);
        EMB_CHECK_NEAR(g->stops[i].rgba[0], want[i][0] / 255.0, 1e-6);
        EMB_CHECK_NEAR(g->stops[i].rgba[1], want[i][1] / 255.0, 1e-6);
        EMB_CHECK_NEAR(g->stops[i].rgba[2], want[i][2] / 255.0, 1e-6);
        EMB_CHECK_NEAR(g->stops[i].rgba[3], 1.0, 1e-6);
    }

    embi_font_close(&f);
    fx_free(&src);
}

/* Linear gradient with reflect extend, three stops: offsets, palette colours with alpha, and
 * the geometry. Stops are 6 bytes each in a ColorLine. */
EMB_TEST(colr_linear_gradient_stops_and_extend)
{
    fb_buf colr = {0};
    colr_build_linear(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 5, nullptr, 0, &v));
    EMB_REQUIRE(c.npaints == 1);
    const cap_paint *g = &c.paints[0];
    EMB_CHECK(g->kind == EMBI_PAINT_LINEAR);
    EMB_CHECK_EQ_INT(g->extend, 2);
    EMB_CHECK_NEAR(g->p[2], 100.0, 1e-4);
    EMB_CHECK_NEAR(g->p[5], 100.0, 1e-4);
    EMB_REQUIRE(g->nstops == 3);
    EMB_CHECK_NEAR(g->stops[0].offset, 0.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].offset, 0.5, 1e-6);
    EMB_CHECK_NEAR(g->stops[2].offset, 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[0].rgba[2], 1.0, 1e-6); /* palette 0 is blue */
    EMB_CHECK_NEAR(g->stops[0].rgba[3], 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].rgba[1], 1.0, 1e-6); /* palette 1 is green */
    EMB_CHECK_NEAR(g->stops[1].rgba[3], 0.5, 1e-6);
    EMB_CHECK_NEAR(g->stops[2].rgba[0], 1.0, 1e-6); /* palette 2 is red */
    EMB_CHECK_NEAR(g->stops[2].rgba[3], 0.5, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* PaintVarLinearGradient: its VarColorLine stops are 10 bytes, so the stride must be 10. With
 * no variation store the deltas are zero and the stops are the static values. */
EMB_TEST(colr_var_linear_gradient_uses_var_stop_stride)
{
    fb_buf colr = {0};
    colr_build_var_linear(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 6, nullptr, 0, &v));
    EMB_REQUIRE(c.npaints == 1);
    const cap_paint *g = &c.paints[0];
    EMB_CHECK(g->kind == EMBI_PAINT_LINEAR);
    EMB_CHECK_EQ_INT(g->extend, 0);
    EMB_CHECK_NEAR(g->p[0], 10.0, 1e-4);
    EMB_CHECK_NEAR(g->p[1], 20.0, 1e-4);
    EMB_CHECK_NEAR(g->p[2], 30.0, 1e-4);
    EMB_CHECK_NEAR(g->p[3], 40.0, 1e-4);
    EMB_CHECK_NEAR(g->p[4], 50.0, 1e-4);
    EMB_CHECK_NEAR(g->p[5], 60.0, 1e-4);
    EMB_REQUIRE(g->nstops == 2);
    EMB_CHECK_NEAR(g->stops[0].offset, 0.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].offset, 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[0].rgba[2], 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].rgba[0], 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].rgba[3], 0.5, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* Sweep gradient: the angles are stored in semicircles and come out in degrees. */
EMB_TEST(colr_sweep_gradient_angles_in_degrees)
{
    fb_buf colr = {0};
    colr_build_sweep(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 7, nullptr, 0, &v));
    EMB_REQUIRE(c.npaints == 1);
    const cap_paint *g = &c.paints[0];
    EMB_CHECK(g->kind == EMBI_PAINT_SWEEP);
    EMB_CHECK_EQ_INT(g->extend, 1);
    EMB_CHECK_NEAR(g->p[0], 10.0, 1e-4);
    EMB_CHECK_NEAR(g->p[1], 20.0, 1e-4);
    EMB_CHECK_NEAR(g->p[2], 90.0, 1e-4);
    EMB_CHECK_NEAR(g->p[3], 180.0, 1e-4);
    EMB_REQUIRE(g->nstops == 2);
    EMB_CHECK_NEAR(g->stops[1].offset, 1.0, 1e-6);
    EMB_CHECK_NEAR(g->stops[1].rgba[1], 1.0, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* A composite paints the backdrop first, then GROUP_PUSH with the mode, the source, and
 * GROUP_POP. */
EMB_TEST(colr_composite_orders_backdrop_group_source)
{
    fb_buf colr = {0};
    colr_build_composite(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 11, nullptr, 0, &v));
    EMB_REQUIRE(c.nops == 4);
    EMB_CHECK(c.kinds[0] == EMBI_COLR_PAINT);
    EMB_CHECK(c.kinds[1] == EMBI_COLR_GROUP_PUSH);
    EMB_CHECK(c.kinds[2] == EMBI_COLR_PAINT);
    EMB_CHECK(c.kinds[3] == EMBI_COLR_GROUP_POP);
    EMB_CHECK_EQ_INT(c.ngroup, 1);
    EMB_CHECK_EQ_INT(c.group_mode[0], 4);
    EMB_REQUIRE(c.npaints == 2);
    /* Backdrop is palette 2 (red), source is palette 1 (green). */
    EMB_CHECK_NEAR(c.paints[0].rgba[0], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.paints[0].rgba[1], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.paints[1].rgba[1], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.paints[1].rgba[0], 0.0, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* PaintSkew: x' = x - tan(xSkew) y and y' = y + tan(ySkew) x (counter-clockwise angles, as the
 * fontTools skew(-xSkew, ySkew) convention). With both angles 22.5 degrees the matrix is
 * [1, t, -t, 1, 0, 0] with t = tan(22.5 degrees) = sqrt(2) - 1. */
EMB_TEST(colr_skew_matrix_sign_convention)
{
    fb_buf colr = {0};
    colr_build_skew(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 12, nullptr, 0, &v));
    EMB_REQUIRE(c.nxf == 1);
    const double t = (double)sqrtf(2.0f) - 1.0;
    EMB_CHECK_NEAR(c.xf[0][0], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.xf[0][1], t, 1e-5);  /* b: y' gets tan(ySkew) x */
    EMB_CHECK_NEAR(c.xf[0][2], -t, 1e-5); /* c: x' gets -tan(xSkew) y */
    EMB_CHECK_NEAR(c.xf[0][3], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.xf[0][4], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.xf[0][5], 0.0, 1e-6);
    EMB_CHECK(c.kinds[0] == EMBI_COLR_TRANSFORM_PUSH);
    EMB_CHECK(c.kinds[2] == EMBI_COLR_TRANSFORM_POP);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* COLRv0 layers: clip to the layer glyph, a solid paint of the palette colour (alpha 1), and
 * the foreground index 0xFFFF as a foreground paint. */
EMB_TEST(colr_v0_layers_clip_and_foreground)
{
    fb_buf colr = {0};
    colr_build_v0(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 8, nullptr, 0, &v));
    EMB_CHECK_EQ_INT(c.nops, 6);
    EMB_CHECK(c.kinds[0] == EMBI_COLR_CLIP_PUSH);
    EMB_CHECK(c.kinds[1] == EMBI_COLR_PAINT);
    EMB_CHECK(c.kinds[2] == EMBI_COLR_CLIP_POP);
    EMB_CHECK(c.kinds[3] == EMBI_COLR_CLIP_PUSH);
    EMB_CHECK(c.kinds[4] == EMBI_COLR_PAINT);
    EMB_CHECK(c.kinds[5] == EMBI_COLR_CLIP_POP);
    EMB_CHECK_EQ_INT(c.clip_gid[0], 9);
    EMB_CHECK_EQ_INT(c.clip_gid[1], 10);
    EMB_REQUIRE(c.npaints == 2);
    EMB_CHECK(c.paints[0].kind == EMBI_PAINT_SOLID);
    EMB_CHECK(!c.paints[0].fg);
    EMB_CHECK_NEAR(c.paints[0].rgba[2], 1.0, 1e-6); /* palette 0 is blue */
    EMB_CHECK_NEAR(c.paints[0].rgba[3], 1.0, 1e-6);
    EMB_CHECK(c.paints[1].fg);
    EMB_CHECK_NEAR(c.paints[1].rgba[3], 1.0, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* A gradient with one stop becomes a solid fill; one with no stops emits nothing and succeeds. */
EMB_TEST(colr_gradient_with_one_or_no_stops)
{
    fb_buf colr = {0};
    colr_build_degenerate(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_REQUIRE(embi_font_colr_paint(&f, 14, nullptr, 0, &v));
    EMB_REQUIRE(c.npaints == 1);
    EMB_CHECK(c.paints[0].kind == EMBI_PAINT_SOLID);
    EMB_CHECK_NEAR(c.paints[0].rgba[1], 1.0, 1e-6); /* palette 1 is green */
    EMB_CHECK_NEAR(c.paints[0].rgba[3], 1.0, 1e-6);

    cap e;
    cap_init(&e);
    const embi_colr_visitor ve = {&e, cap_op};
    EMB_CHECK(embi_font_colr_paint(&f, 15, nullptr, 0, &ve));
    EMB_CHECK_EQ_INT(e.nops, 0);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* Layers that refer back to themselves, with no emitting paint, are stopped by the evaluation
 * budget: the call fails and emits nothing. Without the budget this is 2^64 visits. */
EMB_TEST(colr_self_referencing_layers_hit_the_budget)
{
    fb_buf colr = {0};
    colr_build_self_layers(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    cap c;
    cap_init(&c);
    const embi_colr_visitor v = {&c, cap_op};
    EMB_CHECK(!embi_font_colr_paint(&f, 13, nullptr, 0, &v));
    EMB_CHECK_EQ_INT(c.nops, 0);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* ------------------------------------------------------------------ ColorLine contract */

/* The embi_colr_stop contract (font.h) that R2c's gradient code relies on:
 *   - offsets are not clamped (F2DOT14 values outside 0..1 come through as they are),
 *   - a line of at most 256 stops is stable-sorted by offset,
 *   - a longer line keeps font order,
 *   - variation deltas move the offset (VarColorStop),
 *   - a call reads at most 1,000,000 stops; past that the walk fails.
 * The tests below build the COLR bytes by hand, so each expected value follows from them. */

typedef struct stop_cap {
    uint32_t npaints;
    uint32_t n; /* stops of the first paint, up to 300 */
    float off[300];
    float rgba[300][4];
    bool fg[300];
} stop_cap;

static bool stop_cap_op(void *ctx, const embi_colr_op *op)
{
    stop_cap *c = (stop_cap *)ctx;
    if (op->kind != EMBI_COLR_PAINT) {
        return true;
    }
    if (c->npaints++ == 0) {
        c->n = op->paint.num_stops < 300 ? op->paint.num_stops : 300;
        for (uint32_t i = 0; i < c->n; i++) {
            c->off[i] = op->paint.stops[i].offset;
            c->fg[i] = op->paint.stops[i].foreground;
            for (int k = 0; k < 4; k++) {
                c->rgba[i][k] = op->paint.stops[i].rgba[k];
            }
        }
    }
    return true;
}

/* A raw ColorStop: F2DOT14 offset, palette index, F2DOT14 alpha. */
typedef struct raw_stop {
    int16_t offset;
    uint16_t palette;
    int16_t alpha;
} raw_stop;

/* COLRv1 with glyph 5 painted by a PaintLinearGradient (format 4) whose ColorLine holds the given
 * stops. Layout: header 0..34, base list 34..44, paint at 44 (colorLine at +16 = 60). */
static void colr_build_stops(fb_buf *b, const raw_stop *s, uint32_t n)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 5, 10);
    fb_u8(b, 4);
    fb_u24(b, 16);
    for (int i = 0; i < 6; i++) {
        fb_u16(b, 0);
    }
    fb_u8(b, 0); /* extend pad */
    fb_u16(b, n);
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(b, (uint16_t)s[i].offset);
        fb_u16(b, s[i].palette);
        fb_u16(b, (uint16_t)s[i].alpha);
    }
}

/* Palette 0 entries: 0 blue (0,0,1), 1 green (0,1,0), 2 red (1,0,0). Each stop below names its
 * colour through the palette index, so the sorted order is visible in the rgba. */
static bool stop_walk(const fb_buf *colr, stop_cap *c)
{
    fx_file src;
    embi_font f;
    if (!colr_open_font(colr, &src, &f)) {
        return false;
    }
    SDL_memset(c, 0, sizeof *c);
    const embi_colr_visitor v = {c, stop_cap_op};
    const bool ok = embi_font_colr_paint(&f, 5, nullptr, 0, &v);
    embi_font_close(&f);
    fx_free(&src);
    return ok;
}

/* Out-of-order offsets with a tie. Font order (offset F2DOT14 -> value, palette):
 *   0: 8192 (0.5) blue    1: 0 (0.0) green    2: 8192 (0.5) red    3: 4096 (0.25) green.
 * Sorted by offset: 0.0 green, 0.25 green, then the two 0.5 stops in font order: blue, red. */
EMB_TEST(colr_stops_sorted_stably)
{
    const raw_stop s[4] = {{8192, 0, 16384}, {0, 1, 16384}, {8192, 2, 16384}, {4096, 1, 16384}};
    fb_buf colr = {0};
    colr_build_stops(&colr, s, 4);
    stop_cap c;
    EMB_REQUIRE(stop_walk(&colr, &c));
    EMB_REQUIRE(c.npaints == 1 && c.n == 4);
    EMB_CHECK_NEAR(c.off[0], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.off[1], 0.25, 1e-6);
    EMB_CHECK_NEAR(c.off[2], 0.5, 1e-6);
    EMB_CHECK_NEAR(c.off[3], 0.5, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][1], 1.0, 1e-6); /* green */
    EMB_CHECK_NEAR(c.rgba[1][1], 1.0, 1e-6); /* green */
    EMB_CHECK_NEAR(c.rgba[2][2], 1.0, 1e-6); /* blue first (font order) */
    EMB_CHECK_NEAR(c.rgba[2][0], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.rgba[3][0], 1.0, 1e-6); /* red second */
    EMB_CHECK_NEAR(c.rgba[3][2], 0.0, 1e-6);
    fb_free(&colr);
}

/* Offsets outside 0..1 are not clamped: F2DOT14 -8192 is -0.5 and 24576 is 1.5. They also sort
 * (the 1.5 stop comes first in the font). */
EMB_TEST(colr_stop_offsets_not_clamped)
{
    const raw_stop s[2] = {{24576, 2, 16384}, {-8192, 0, 16384}};
    fb_buf colr = {0};
    colr_build_stops(&colr, s, 2);
    stop_cap c;
    EMB_REQUIRE(stop_walk(&colr, &c));
    EMB_REQUIRE(c.npaints == 1 && c.n == 2);
    EMB_CHECK_NEAR(c.off[0], -0.5, 1e-6);
    EMB_CHECK_NEAR(c.off[1], 1.5, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][2], 1.0, 1e-6); /* blue */
    EMB_CHECK_NEAR(c.rgba[1][0], 1.0, 1e-6); /* red */
    fb_free(&colr);
}

/* 257 stops, offsets descending 1.0 - i/256 (F2DOT14 16384 - 64 i), palette i % 3. A line longer
 * than 256 stops keeps font order: stop 0 is offset 1.0 and the last is 0.0. At exactly 256 stops
 * the line still sorts, so the same data cut to 256 stops comes out ascending. */
EMB_TEST(colr_long_stop_line_keeps_font_order)
{
    raw_stop s[257];
    for (int i = 0; i < 257; i++) {
        s[i] = (raw_stop){(int16_t)(16384 - 64 * i), (uint16_t)(i % 3), 16384};
    }
    fb_buf colr = {0};
    colr_build_stops(&colr, s, 257);
    stop_cap c;
    EMB_REQUIRE(stop_walk(&colr, &c));
    EMB_REQUIRE(c.npaints == 1 && c.n == 257);
    for (int i = 0; i < 257; i++) {
        EMB_CHECK_NEAR(c.off[i], 1.0 - i / 256.0, 1e-6);
    }
    fb_free(&colr);

    colr = (fb_buf){0};
    colr_build_stops(&colr, s, 256);
    EMB_REQUIRE(stop_walk(&colr, &c));
    EMB_REQUIRE(c.npaints == 1 && c.n == 256);
    for (int i = 0; i < 256; i++) {
        EMB_CHECK_NEAR(c.off[i], (i + 1) / 256.0, 1e-6);
    }
    fb_free(&colr);
}

/* COLRv1 with an ItemVariationStore. Glyph 6 is a PaintVarLinearGradient (format 5) at 44 with a
 * VarColorLine at 64 (stops of 10 bytes):
 *   stop 0: offset 0.0, palette 0 (blue), alpha 1.0, varIndexBase 0
 *   stop 1: offset 1.0, palette 2 (red), alpha 1.0, varIndexBase 0xFFFFFFFF (no variation).
 * The store (at 88, no DeltaSetIndexMap so the index is the row) has one axis and one region
 * (start 0, peak 1.0, end 1.0) and a VarData of two rows with one 16-bit delta each:
 *   row 0 (stop 0 offset): +4096 (0.25 in F2DOT14), row 1 (stop 0 alpha): -8192 (-0.5).
 * At coordinate F2DOT14 16384 the region scalar is 1, at 8192 it is 0.5 and at 0 it is 0. */
static void colr_build_var_stops(fb_buf *b)
{
    colr_header_v1(b, 0);
    colr_one_base(b, 6, 10);
    fb_u8(b, 5);
    fb_u24(b, 20);
    for (int i = 0; i < 6; i++) {
        fb_u16(b, 0);
    }
    fb_u32(b, COLR_NO_VAR); /* the gradient geometry has no variation */
    fb_u8(b, 0); /* VarColorLine at 64 */
    fb_u16(b, 2);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 16384);
    fb_u32(b, 0);
    fb_u16(b, 16384);
    fb_u16(b, 2);
    fb_u16(b, 16384);
    fb_u32(b, COLR_NO_VAR);
    while (b->n < 88) {
        fb_u8(b, 0);
    }
    /* ItemVariationStore at 88 */
    fb_u16(b, 1);  /* format */
    fb_u32(b, 12); /* variationRegionListOffset */
    fb_u16(b, 1);  /* itemVariationDataCount */
    fb_u32(b, 22); /* itemVariationDataOffsets[0] */
    fb_u16(b, 1);  /* region list: axisCount */
    fb_u16(b, 1);  /* regionCount */
    fb_u16(b, 0);  /* start */
    fb_u16(b, 16384); /* peak */
    fb_u16(b, 16384); /* end */
    fb_u16(b, 2);  /* VarData: itemCount */
    fb_u16(b, 1);  /* wordDeltaCount (all deltas are 16-bit) */
    fb_u16(b, 1);  /* regionIndexCount */
    fb_u16(b, 0);  /* regionIndexes[0] */
    fb_u16(b, 4096);
    fb_u16(b, (uint16_t)-8192);
    fb_patch32(b, 30, 88); /* itemVariationStoreOffset */
}

/* Font with one fvar axis ('wght', 0..1) so that the variation coordinates have an axis. */
static bool colr_open_var_font(const fb_buf *colr, fx_file *src, embi_font *f)
{
    fb_buf cpal = {0}, fvar = {0};
    colr_put_cpal(&cpal);
    fb_u16(&fvar, 1); /* majorVersion */
    fb_u16(&fvar, 0);
    fb_u16(&fvar, 16); /* axesArrayOffset */
    fb_u16(&fvar, 2);
    fb_u16(&fvar, 1);  /* axisCount */
    fb_u16(&fvar, 20); /* axisSize */
    fb_u16(&fvar, 0);  /* instanceCount */
    fb_u16(&fvar, 0);
    fb_u32(&fvar, EMBI_TAG('w', 'g', 'h', 't'));
    fb_u32(&fvar, 0);       /* min 0 */
    fb_u32(&fvar, 0);       /* default 0 */
    fb_u32(&fvar, 0x10000); /* max 1 */
    fb_u16(&fvar, 0);
    fb_u16(&fvar, 0);
    fb_table ex[3] = {{EMBI_TAG('C', 'P', 'A', 'L'), cpal.p, cpal.n},
                      {EMBI_TAG('C', 'O', 'L', 'R'), colr->p, colr->n},
                      {EMBI_TAG('f', 'v', 'a', 'r'), fvar.p, fvar.n}};
    const bool ok = fb_font(0x00010000u, 1000, 16, ex, 3, src);
    fb_free(&cpal);
    fb_free(&fvar);
    if (!ok) {
        return false;
    }
    SDL_memset(f, 0, sizeof *f);
    return embi_font_open(src->p, src->n, 0, f);
}

/* A PaintVarLinearGradient stop offset delta moves the offset (no clamping, no re-sorting
 * surprises) and an alpha delta scales the colour alpha. */
EMB_TEST(colr_var_stop_offset_delta_applies)
{
    fb_buf colr = {0};
    colr_build_var_stops(&colr);
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_var_font(&colr, &src, &f));
    EMB_REQUIRE(f.axis_count == 1);

    /* coordinate 1.0: scalar 1: offset 0 + 0.25, alpha 1 - 0.5; stop 1 has no variation. */
    stop_cap c;
    int16_t coords[1] = {16384};
    embi_colr_visitor v = {&c, stop_cap_op};
    SDL_memset(&c, 0, sizeof c);
    EMB_REQUIRE(embi_font_colr_paint(&f, 6, coords, 0, &v));
    EMB_REQUIRE(c.npaints == 1 && c.n == 2);
    EMB_CHECK_NEAR(c.off[0], 0.25, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][3], 0.5, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][2], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.off[1], 1.0, 1e-6);
    EMB_CHECK_NEAR(c.rgba[1][3], 1.0, 1e-6);

    /* coordinate 0.5: scalar 0.5. */
    coords[0] = 8192;
    SDL_memset(&c, 0, sizeof c);
    EMB_REQUIRE(embi_font_colr_paint(&f, 6, coords, 0, &v));
    EMB_REQUIRE(c.n == 2);
    EMB_CHECK_NEAR(c.off[0], 0.125, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][3], 0.75, 1e-6);

    /* coordinate 0 and the default instance (no coords): no change. */
    coords[0] = 0;
    SDL_memset(&c, 0, sizeof c);
    EMB_REQUIRE(embi_font_colr_paint(&f, 6, coords, 0, &v));
    EMB_CHECK_NEAR(c.off[0], 0.0, 1e-6);
    EMB_CHECK_NEAR(c.rgba[0][3], 1.0, 1e-6);
    SDL_memset(&c, 0, sizeof c);
    EMB_REQUIRE(embi_font_colr_paint(&f, 6, nullptr, 0, &v));
    EMB_CHECK_NEAR(c.off[0], 0.0, 1e-6);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* Hostile file: a ColorLine of 65535 stops (a 393 KB table) referenced by 20 layers of a
 * PaintColrLayers. Each walk of the line costs 65535 stops of the 1,000,000 budget, so the
 * sixteenth gradient (16 * 65535 = 1,048,560) must fail the call. The first 15 have been emitted
 * by then. Layout: header 0..34, base list 34..44, layer list at 44 (count + 20 offsets = 84
 * bytes, to 128), PaintColrLayers at 128 (6 bytes), PaintLinearGradient at 134 (colorLine at
 * +16 = 150). Layer offsets are relative to the layer list: 134 - 44 = 90. */
typedef struct big_count {
    uint32_t paints;
    uint32_t stops_total;
} big_count;

static bool big_op(void *ctx, const embi_colr_op *op)
{
    big_count *c = (big_count *)ctx;
    if (op->kind == EMBI_COLR_PAINT) {
        c->paints++;
        c->stops_total += op->paint.num_stops;
    }
    return true;
}

EMB_TEST(colr_stop_budget_fails_the_walk)
{
    fb_buf colr = {0};
    colr_header_v1(&colr, 44);
    colr_one_base(&colr, 5, 94); /* paint at 34 + 94 = 128 */
    fb_u32(&colr, 20);           /* LayerList at 44 */
    for (int i = 0; i < 20; i++) {
        fb_u32(&colr, 90);
    }
    EMB_REQUIRE(colr.n == 128);
    fb_u8(&colr, 1); /* PaintColrLayers at 128 */
    fb_u8(&colr, 20);
    fb_u32(&colr, 0);
    fb_u8(&colr, 4); /* PaintLinearGradient at 134 */
    fb_u24(&colr, 16);
    for (int i = 0; i < 6; i++) {
        fb_u16(&colr, 0);
    }
    fb_u8(&colr, 0); /* ColorLine at 150 */
    fb_u16(&colr, 65535);
    for (uint32_t i = 0; i < 65535; i++) {
        fb_u16(&colr, i & 0x3FFFu);
        fb_u16(&colr, i % 3);
        fb_u16(&colr, 16384);
    }
    fx_file src;
    embi_font f;
    EMB_REQUIRE(colr_open_font(&colr, &src, &f));

    big_count c = {0, 0};
    const embi_colr_visitor v = {&c, big_op};
    EMB_CHECK(!embi_font_colr_paint(&f, 5, nullptr, 0, &v));
    EMB_CHECK_EQ_INT(c.paints, 15);
    EMB_CHECK_EQ_INT(c.stops_total, 15u * 65535u);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}


EMB_TEST_MAIN()
