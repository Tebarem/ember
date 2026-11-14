#include "font_build.h"

/* Lenient opening, like FreeType and HarfBuzz: table bounds are checked per table when the table
 * is used, so a directory record outside the file for an unused table never fails the font; a
 * broken optional table (name, OS/2, post, COLR, CPAL, fvar and the variation tables) degrades
 * that feature only; the required ones (head, maxp, hhea/hmtx, a listed cmap, glyf/loca) still
 * fail the open. Every font here is built byte by byte. */

#define LEN_NG 4u

/* Builds head, hhea (numberOfHMetrics = nhm_field), maxp, hmtx (LEN_NG * 4 bytes plus pad extra
 * zero bytes) and the extra tables, in that order. Table i of the extras is directory record
 * 4 + i. */
static bool len_font(uint32_t nhm_field, size_t hmtx_pad, const fb_table *extra, uint32_t nextra,
                     fx_file *out)
{
    fb_buf head = {0}, hhea = {0}, maxp = {0}, hmtx = {0};
    fb_put_head(&head, 1000);
    fb_put_hhea(&hhea, nhm_field);
    fb_put_maxp(&maxp, LEN_NG);
    fb_put_hmtx(&hmtx, LEN_NG);
    for (size_t i = 0; i < hmtx_pad; i++) {
        fb_u8(&hmtx, 0);
    }
    fb_table t[4 + 8];
    t[0] = (fb_table){EMBI_TAG('h', 'e', 'a', 'd'), head.p, head.n};
    t[1] = (fb_table){EMBI_TAG('h', 'h', 'e', 'a'), hhea.p, hhea.n};
    t[2] = (fb_table){EMBI_TAG('m', 'a', 'x', 'p'), maxp.p, maxp.n};
    t[3] = (fb_table){EMBI_TAG('h', 'm', 't', 'x'), hmtx.p, hmtx.n};
    for (uint32_t i = 0; i < nextra && i < 8; i++) {
        t[4 + i] = extra[i];
    }
    const bool ok = fb_sfnt(0x00010000u, t, 4 + (nextra < 8 ? nextra : 8), out);
    fb_free(&head);
    fb_free(&hhea);
    fb_free(&maxp);
    fb_free(&hmtx);
    return ok;
}

/* Points directory record `rec` at offset/length that lie outside the file. */
static void len_break_record(fx_file *f, uint32_t rec)
{
    fb_buf v = {f->p, f->n, f->n};
    fb_patch32(&v, 12 + 16 * (size_t)rec + 8, 0x7FFF0000u);
    fb_patch32(&v, 12 + 16 * (size_t)rec + 12, 0x1000u);
}

static bool len_open(const fx_file *f, embi_font *out)
{
    SDL_memset(out, 0, sizeof *out);
    return embi_font_open(f->p, f->n, 0, out);
}

static const uint8_t len_junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};

/* An unused table (DSIG) whose record points past the end of the file must not fail the open. */
EMB_TEST(lenient_unused_table_outside_file_opens)
{
    const fb_table ex[1] = {{EMBI_TAG('D', 'S', 'I', 'G'), len_junk, sizeof len_junk}};
    fx_file src;
    EMB_REQUIRE(len_font(LEN_NG, 0, ex, 1, &src));
    len_break_record(&src, 4);
    embi_font f;
    EMB_REQUIRE(len_open(&src, &f));
    EMB_CHECK_EQ_INT(f.metrics.num_glyphs, LEN_NG);
    embi_font_close(&f);
    fx_free(&src);
}

/* The required tables still fail the font when they are missing or out of the file. */
EMB_TEST(required_tables_failing_still_fail)
{
    for (uint32_t rec = 0; rec < 4; rec++) { /* head, hhea, maxp, hmtx */
        fx_file src;
        EMB_REQUIRE(len_font(LEN_NG, 0, nullptr, 0, &src));
        len_break_record(&src, rec);
        embi_font f;
        EMB_CHECK(!len_open(&src, &f));
        embi_font_close(&f);
        fx_free(&src);
    }
    /* A listed cmap that lies outside the file, or that is too short to hold a header. */
    const fb_table cm[1] = {{EMBI_TAG('c', 'm', 'a', 'p'), len_junk, sizeof len_junk}};
    fx_file src;
    EMB_REQUIRE(len_font(LEN_NG, 0, cm, 1, &src));
    embi_font f;
    EMB_CHECK(len_open(&src, &f));
    embi_font_close(&f);
    len_break_record(&src, 4);
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);
    const fb_table cm2[1] = {{EMBI_TAG('c', 'm', 'a', 'p'), len_junk, 2}};
    EMB_REQUIRE(len_font(LEN_NG, 0, cm2, 1, &src));
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);

    /* glyf without loca, and glyf outside the file. */
    const fb_table gl[1] = {{EMBI_TAG('g', 'l', 'y', 'f'), len_junk, sizeof len_junk}};
    EMB_REQUIRE(len_font(LEN_NG, 0, gl, 1, &src));
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);
    const fb_table gl2[2] = {{EMBI_TAG('g', 'l', 'y', 'f'), len_junk, sizeof len_junk},
                             {EMBI_TAG('l', 'o', 'c', 'a'), len_junk, sizeof len_junk}};
    EMB_REQUIRE(len_font(LEN_NG, 0, gl2, 2, &src));
    len_break_record(&src, 4);
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);

    /* A CFF table outside the file fails; so does one that is not CFF data. */
    const fb_table cf[1] = {{EMBI_TAG('C', 'F', 'F', ' '), len_junk, sizeof len_junk}};
    EMB_REQUIRE(len_font(LEN_NG, 0, cf, 1, &src));
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    len_break_record(&src, 4);
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);
}

/* hmtx with extra trailing bytes opens; so does a hhea whose numberOfHMetrics exceeds the glyph
 * count (the surplus metrics have no glyph). */
EMB_TEST(lenient_hmtx_trailing_bytes_and_oversized_count)
{
    fx_file src;
    EMB_REQUIRE(len_font(LEN_NG, 37, nullptr, 0, &src));
    embi_font f;
    EMB_REQUIRE(len_open(&src, &f));
    float adv = 0.0f;
    EMB_REQUIRE(embi_font_glyph_advance(&f, 3, nullptr, &adv));
    EMB_CHECK_NEAR(adv, 500.0, 0.001);
    embi_font_close(&f);
    fx_free(&src);

    EMB_REQUIRE(len_font(LEN_NG + 3, 5, nullptr, 0, &src));
    EMB_REQUIRE(len_open(&src, &f));
    EMB_REQUIRE(embi_font_glyph_advance(&f, 2, nullptr, &adv));
    EMB_CHECK_NEAR(adv, 500.0, 0.001);
    embi_font_close(&f);
    fx_free(&src);

    /* Too short stays an error. */
    EMB_REQUIRE(len_font(LEN_NG, 0, nullptr, 0, &src));
    fb_buf v = {src.p, src.n, src.n};
    fb_patch32(&v, 12 + 16 * 3 + 12, 4u * LEN_NG - 4u); /* hmtx length minus one record */
    EMB_CHECK(!len_open(&src, &f));
    embi_font_close(&f);
    fx_free(&src);
}

/* ------------------------------------------------------------------ fvar */

/* fvar with one axis: min, def, max as 16.16 values. */
static void len_put_fvar(fb_buf *b, int32_t mn, int32_t df, int32_t mx, uint32_t axis_size)
{
    fb_u32(b, 0x00010000u);
    fb_u16(b, 16);        /* axesArrayOffset */
    fb_u16(b, 2);         /* reserved */
    fb_u16(b, 1);         /* axisCount */
    fb_u16(b, axis_size); /* axisSize */
    fb_u16(b, 0);         /* instanceCount */
    fb_u16(b, 8);         /* instanceSize */
    fb_u32(b, EMBI_TAG('w', 'g', 'h', 't'));
    fb_u32(b, (uint32_t)mn);
    fb_u32(b, (uint32_t)df);
    fb_u32(b, (uint32_t)mx);
    fb_u16(b, 0);
    fb_u16(b, 256);
}

static bool len_open_fvar(int32_t mn, int32_t df, int32_t mx, uint32_t axis_size, fx_file *src,
                          embi_font *f)
{
    fb_buf fv = {0};
    len_put_fvar(&fv, mn, df, mx, axis_size);
    const fb_table ex[1] = {{EMBI_TAG('f', 'v', 'a', 'r'), fv.p, fv.n}};
    const bool ok = len_font(LEN_NG, 0, ex, 1, src);
    fb_free(&fv);
    return ok && len_open(src, f);
}

/* A default outside [min, max] is clamped into the range rather than failing the font. */
EMB_TEST(lenient_fvar_default_is_clamped)
{
    fx_file src;
    embi_font f;
    /* default 50 below min 100 */
    EMB_REQUIRE(len_open_fvar(100 << 16, 50 << 16, 900 << 16, 20, &src, &f));
    EMB_REQUIRE(f.axis_count == 1);
    EMB_CHECK_NEAR(f.axes[0].min, 100.0, 0.001);
    EMB_CHECK_NEAR(f.axes[0].def, 100.0, 0.001);
    EMB_CHECK_NEAR(f.axes[0].max, 900.0, 0.001);
    EMB_CHECK(f.metrics.is_variable);
    embi_font_close(&f);
    fx_free(&src);

    /* default 1000 above max 900 */
    EMB_REQUIRE(len_open_fvar(100 << 16, 1000 << 16, 900 << 16, 20, &src, &f));
    EMB_REQUIRE(f.axis_count == 1);
    EMB_CHECK_NEAR(f.axes[0].def, 900.0, 0.001);
    embi_font_close(&f);
    fx_free(&src);

    /* swapped bounds are put in order */
    EMB_REQUIRE(len_open_fvar(900 << 16, 400 << 16, 100 << 16, 20, &src, &f));
    EMB_REQUIRE(f.axis_count == 1);
    EMB_CHECK_NEAR(f.axes[0].min, 100.0, 0.001);
    EMB_CHECK_NEAR(f.axes[0].max, 900.0, 0.001);
    EMB_CHECK_NEAR(f.axes[0].def, 400.0, 0.001);
    embi_font_close(&f);
    fx_free(&src);
}

/* A broken fvar (axis size below 20, a table cut short) leaves a static font. */
EMB_TEST(broken_fvar_degrades_to_static_font)
{
    fx_file src;
    embi_font f;
    EMB_REQUIRE(len_open_fvar(0, 0, 0, 4, &src, &f));
    EMB_CHECK_EQ_INT(f.axis_count, 0);
    EMB_CHECK(!f.metrics.is_variable);
    EMB_CHECK_EQ_INT(embi_font_instance_count(&f), 0);
    embi_font_close(&f);
    fx_free(&src);

    /* Header only: the axis record is past the end of the table. */
    fb_buf fv = {0};
    len_put_fvar(&fv, 0, 0, 0, 20);
    const fb_table ex[1] = {{EMBI_TAG('f', 'v', 'a', 'r'), fv.p, 20}};
    EMB_REQUIRE(len_font(LEN_NG, 0, ex, 1, &src));
    EMB_REQUIRE(len_open(&src, &f));
    EMB_CHECK_EQ_INT(f.axis_count, 0);
    EMB_CHECK(!f.metrics.is_variable);
    embi_font_close(&f);
    fx_free(&src);
    fb_free(&fv);
}

/* ------------------------------------------------------------------ optional tables */

/* name, OS/2, post, COLR, CPAL, gvar, HVAR and MVAR all listed but out of the file, or holding
 * garbage: the font opens and the features are off. */
EMB_TEST(broken_optional_tables_degrade_feature_only)
{
    static const uint32_t tags[8] = {
        EMBI_TAG('n', 'a', 'm', 'e'), EMBI_TAG('O', 'S', '/', '2'), EMBI_TAG('p', 'o', 's', 't'),
        EMBI_TAG('C', 'O', 'L', 'R'), EMBI_TAG('C', 'P', 'A', 'L'), EMBI_TAG('g', 'v', 'a', 'r'),
        EMBI_TAG('H', 'V', 'A', 'R'), EMBI_TAG('M', 'V', 'A', 'R'),
    };
    fb_table ex[8];
    for (int i = 0; i < 8; i++) {
        ex[i] = (fb_table){tags[i], len_junk, sizeof len_junk};
    }
    /* Garbage contents. */
    fx_file src;
    embi_font f;
    EMB_REQUIRE(len_font(LEN_NG, 0, ex, 8, &src));
    EMB_REQUIRE(len_open(&src, &f));
    EMB_CHECK(!f.metrics.has_colr);
    EMB_CHECK_EQ_INT(f.metrics.family.len, 0);
    EMB_CHECK_EQ_INT(f.metrics.weight_class, 0);
    EMB_CHECK_EQ_INT(f.gvar.n, 0);
    EMB_CHECK_EQ_INT(f.hvar_ivs.n, 0);
    EMB_CHECK_EQ_INT(f.mvar_ivs.n, 0);
    float adv = 0.0f;
    EMB_CHECK(embi_font_glyph_advance(&f, 1, nullptr, &adv));
    embi_font_close(&f);

    /* Each one outside the file, on its own. */
    for (uint32_t i = 0; i < 8; i++) {
        fx_file one;
        EMB_REQUIRE(len_font(LEN_NG, 0, &ex[i], 1, &one));
        len_break_record(&one, 4);
        EMB_REQUIRE(len_open(&one, &f));
        EMB_CHECK(!f.metrics.has_colr);
        EMB_CHECK_EQ_INT(f.metrics.family.len, 0);
        embi_font_close(&f);
        fx_free(&one);
    }
    fx_free(&src);

    /* COLR with an unknown version is dropped, the font opens. */
    fb_buf colr = {0};
    fb_u16(&colr, 7);
    for (int i = 0; i < 32; i++) {
        fb_u8(&colr, 0);
    }
    const fb_table cx[1] = {{EMBI_TAG('C', 'O', 'L', 'R'), colr.p, colr.n}};
    EMB_REQUIRE(len_font(LEN_NG, 0, cx, 1, &src));
    EMB_REQUIRE(len_open(&src, &f));
    EMB_CHECK(!f.metrics.has_colr);
    embi_font_close(&f);
    fx_free(&src);
    fb_free(&colr);
}

/* ------------------------------------------------------------------ name */

typedef struct nm_rec {
    uint16_t pid, eid, lang, nid;
    const char *s;   /* ASCII; stored as UTF-16BE for platforms 0 and 3, raw bytes for platform 1 */
    bool outside;    /* offset points past the table */
} nm_rec;

static void nm_build(fb_buf *b, const nm_rec *r, uint32_t n)
{
    fb_u16(b, 0);
    fb_u16(b, n);
    fb_u16(b, 6 + 12 * n);
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t len = (uint32_t)SDL_strlen(r[i].s) * (r[i].pid == 1 ? 1u : 2u);
        fb_u16(b, r[i].pid);
        fb_u16(b, r[i].eid);
        fb_u16(b, r[i].lang);
        fb_u16(b, r[i].nid);
        fb_u16(b, len);
        fb_u16(b, r[i].outside ? 0xFF00u : off);
        off += len;
    }
    for (uint32_t i = 0; i < n; i++) {
        for (const char *c = r[i].s; *c != 0; c++) {
            if (r[i].pid != 1) {
                fb_u8(b, 0);
            }
            fb_u8(b, (uint8_t)*c);
        }
    }
}

static bool nm_open(const nm_rec *r, uint32_t n, fx_file *src, embi_font *f)
{
    fb_buf nm = {0};
    nm_build(&nm, r, n);
    const fb_table ex[1] = {{EMBI_TAG('n', 'a', 'm', 'e'), nm.p, nm.n}};
    const bool ok = len_font(LEN_NG, 0, ex, 1, src);
    fb_free(&nm);
    return ok && len_open(src, f);
}

/* The best record for an ID lies outside the table: it is skipped before scoring, so the valid
 * lower-ranked Mac record wins instead of leaving the name empty. */
EMB_TEST(name_skips_out_of_table_records_before_scoring)
{
    const nm_rec r[3] = {
        {3, 1, 0x409, 1, "WinFamily", true},
        {1, 0, 0, 1, "MacFamily", false},
        {3, 1, 0x409, 2, "WinStyle", false},
    };
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_open(r, 3, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "MacFamily"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "WinStyle"));
    embi_font_close(&f);
    fx_free(&src);
}

/* Only the typographic IDs 16/17 present: family and subfamily fall back to them. */
EMB_TEST(name_falls_back_from_1_2_to_16_17)
{
    const nm_rec r[2] = {
        {3, 1, 0x409, 16, "TypoFam", false},
        {3, 1, 0x409, 17, "TypoSub", false},
    };
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_open(r, 2, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "TypoFam"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "TypoSub"));
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "TypoFam"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "TypoSub"));
    embi_font_close(&f);
    fx_free(&src);
}

/* Only the legacy IDs 1/2 present: the typographic names fall back to them. */
EMB_TEST(name_falls_back_from_16_17_to_1_2)
{
    const nm_rec r[2] = {
        {3, 1, 0x409, 1, "LegacyFam", false},
        {3, 1, 0x409, 2, "LegacySub", false},
    };
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_open(r, 2, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "LegacyFam"));
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "LegacyFam"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "LegacySub"));
    embi_font_close(&f);
    fx_free(&src);
}

/* When both sets exist, each keeps its own value. A 16 whose only record is outside the table
 * falls back to 1. */
EMB_TEST(name_both_sets_stay_separate_and_bad_16_falls_back)
{
    const nm_rec r[4] = {
        {3, 1, 0x409, 1, "Fam", false},
        {3, 1, 0x409, 16, "TypoFam", false},
        {3, 1, 0x409, 2, "Sub", false},
        {3, 1, 0x409, 17, "TypoSub", true},
    };
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_open(r, 4, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "Fam"));
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "TypoFam"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "Sub"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "Sub"));
    embi_font_close(&f);
    fx_free(&src);
}

EMB_TEST_MAIN()
