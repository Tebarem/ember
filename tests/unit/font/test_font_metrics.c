#include "font_build.h"

/* Font-wide metrics, names, flags and the cmap lookups. Expected values are read from the raw
 * tables (head, hhea, OS/2, post, name, maxp, cmap, hmtx) of the fixtures in tests/fonts. */

/* NotoSans-Regular.ttf (glyf, version 4 OS/2):
 *   head: unitsPerEm 1000, bbox -621 -389 2800 1067.
 *   maxp: numGlyphs 3884.
 *   hhea: ascender 1069, descender -293, lineGap 0.
 *   OS/2: fsSelection 0x00C0 (REGULAR and USE_TYPO_METRICS), usWeightClass 400, usWidthClass 5,
 *         sTypoAscender 1069 / sTypoDescender -293 / sTypoLineGap 0,
 *         usWinAscent 1124 / usWinDescent 395, sxHeight 536, sCapHeight 714,
 *         sStrikeoutSize 50, sStrikeoutPosition 322.
 *   post: italicAngle 0.
 *   name (platform 3, language 0x409): 1 "Noto Sans", 2 "Regular", 4 "Noto Sans Regular".
 *   cmap: U+0041 -> gid 36, U+0020 -> gid 3, U+10FFFF unmapped.
 *   hmtx: gid 36 advance 639; gid 3 advance 260. */
EMB_TEST(noto_sans_metrics_and_names)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_font_metrics m;
    EMB_REQUIRE(embi_font_get_metrics(&f, &m));
    EMB_CHECK_EQ_INT(m.units_per_em, 1000);
    EMB_CHECK_EQ_INT(m.num_glyphs, 3884);
    EMB_CHECK_EQ_INT(m.x_min, -621);
    EMB_CHECK_EQ_INT(m.y_min, -389);
    EMB_CHECK_EQ_INT(m.x_max, 2800);
    EMB_CHECK_EQ_INT(m.y_max, 1067);
    EMB_CHECK_EQ_INT(m.hhea_ascender, 1069);
    EMB_CHECK_EQ_INT(m.hhea_descender, -293);
    EMB_CHECK_EQ_INT(m.hhea_line_gap, 0);
    EMB_CHECK_EQ_INT(m.typo_ascender, 1069);
    EMB_CHECK_EQ_INT(m.typo_descender, -293);
    EMB_CHECK_EQ_INT(m.typo_line_gap, 0);
    EMB_CHECK_EQ_INT(m.win_ascent, 1124);
    EMB_CHECK_EQ_INT(m.win_descent, 395);
    EMB_CHECK_EQ_INT(m.x_height, 536);
    EMB_CHECK_EQ_INT(m.cap_height, 714);
    EMB_CHECK_EQ_INT(m.strikeout_size, 50);
    EMB_CHECK_EQ_INT(m.strikeout_position, 322);
    EMB_CHECK_NEAR(m.italic_angle, 0.0, 1e-6);
    EMB_CHECK_EQ_INT(m.weight_class, 400);
    EMB_CHECK_EQ_INT(m.width_class, 5);
    EMB_CHECK(m.use_typo_metrics);
    EMB_CHECK(!m.italic);
    EMB_CHECK(!m.bold);
    EMB_CHECK(!m.is_cff);
    EMB_CHECK(!m.is_cff2);
    EMB_CHECK(!m.is_variable);
    EMB_CHECK(!m.has_colr);
    EMB_CHECK(fx_str_is(m.family, "Noto Sans"));
    EMB_CHECK(fx_str_is(m.subfamily, "Regular"));
    EMB_CHECK(fx_str_is(m.full_name, "Noto Sans Regular"));

    embi_font_close(&f);
    fx_free(&src);
}

/* Glyph lookup and advances, same font:
 *   cmap(U+0041) = 36, hmtx advance 639 (no variations, so coords nullptr).
 *   cmap(U+0020) = 3, hmtx advance 260.
 *   cmap(U+10FFFF) = 0, and has_codepoint is false. */
EMB_TEST(noto_sans_cmap_and_advances)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 36);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x20), 3);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x10FFFF), 0);
    EMB_CHECK(embi_font_has_codepoint(&f, 0x41));
    EMB_CHECK(!embi_font_has_codepoint(&f, 0x10FFFF));

    float adv = -1.0f;
    EMB_CHECK(embi_font_glyph_advance(&f, 36, nullptr, &adv));
    EMB_CHECK_NEAR(adv, 639.0, 1e-4);
    EMB_CHECK(embi_font_glyph_advance(&f, 3, nullptr, &adv));
    EMB_CHECK_NEAR(adv, 260.0, 1e-4);

    embi_font_close(&f);
    fx_free(&src);
}

/* SourceSans3-Regular.otf (CFF, version 0 OS/2 metrics as present in the file):
 *   head: unitsPerEm 1000. maxp numGlyphs 2478.
 *   hhea: ascender 1000, descender -326, lineGap 0. OS/2 typo 1000 / -326 / 0, win 1000 / 326.
 *   name: 1 "Source Sans 3", 2 "Regular", 4 "Source Sans 3".
 *   cmap(U+0041) = gid 2, hmtx advance 544, lsb 3. */
EMB_TEST(source_sans_cff_metrics)
{
    fx_file src;
    EMB_REQUIRE(fx_load("SourceSans3-Regular.otf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_font_metrics m;
    EMB_REQUIRE(embi_font_get_metrics(&f, &m));
    EMB_CHECK_EQ_INT(m.units_per_em, 1000);
    EMB_CHECK_EQ_INT(m.num_glyphs, 2478);
    EMB_CHECK_EQ_INT(m.hhea_ascender, 1000);
    EMB_CHECK_EQ_INT(m.hhea_descender, -326);
    EMB_CHECK_EQ_INT(m.typo_ascender, 1000);
    EMB_CHECK_EQ_INT(m.typo_descender, -326);
    EMB_CHECK_EQ_INT(m.win_ascent, 1000);
    EMB_CHECK_EQ_INT(m.win_descent, 326);
    EMB_CHECK(m.is_cff);
    EMB_CHECK(!m.is_cff2);
    EMB_CHECK(!m.is_variable);
    EMB_CHECK(fx_str_is(m.family, "Source Sans 3"));
    EMB_CHECK(fx_str_is(m.subfamily, "Regular"));
    EMB_CHECK(fx_str_is(m.full_name, "Source Sans 3"));

    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 2);
    float adv = -1.0f;
    EMB_CHECK(embi_font_glyph_advance(&f, 2, nullptr, &adv));
    EMB_CHECK_NEAR(adv, 544.0, 1e-4);

    embi_font_close(&f);
    fx_free(&src);
}

/* Noto-COLRv1.ttf has COLR and CPAL, and no fvar. */
EMB_TEST(colr_font_reports_color_flag)
{
    fx_file src;
    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_font_metrics m;
    EMB_REQUIRE(embi_font_get_metrics(&f, &m));
    EMB_CHECK(m.has_colr);
    EMB_CHECK(!m.is_variable);
    EMB_CHECK_EQ_INT(embi_font_palette_count(&f), 1);

    embi_font_close(&f);
    fx_free(&src);
}

/* NotoSans-VF.ttf: fvar with two axes (wght, wdth), so is_variable is set and the CFF flags are
 * clear. */
EMB_TEST(variable_font_flags)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));

    embi_font_metrics m;
    EMB_REQUIRE(embi_font_get_metrics(&f, &m));
    EMB_CHECK(m.is_variable);
    EMB_CHECK(!m.is_cff);
    EMB_CHECK(!m.is_cff2);
    EMB_CHECK_EQ_INT(f.axis_count, 2);

    embi_font_close(&f);
    fx_free(&src);
}

/* A font that is not a face (no tables, bad tag) still answers the metrics query: the output
 * is zeroed and the function returns false for a closed font. */
EMB_TEST(get_metrics_on_closed_font_fails)
{
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    embi_font_metrics m;
    SDL_memset(&m, 0, sizeof m);
    EMB_CHECK(!embi_font_get_metrics(&f, &m));
}


/* ------------------------------------------------------------------ synthetic cmap and name */

/* One format 4 segment. delta is the raw idDelta; ro is the raw idRangeOffset. */
typedef struct cm4_seg {
    uint16_t start, end, delta, ro;
} cm4_seg;

/* Appends a format 4 subtable. The segments must be sorted by end code and end with 0xFFFF.
 * `garr` is the glyphIdArray (garr_n entries) that follows the idRangeOffset array. */
static void cm4_build(fb_buf *b, const cm4_seg *s, uint32_t n, const uint16_t *garr,
                      uint32_t garr_n)
{
    fb_u16(b, 4);
    fb_u16(b, 16 + 8 * n + 2 * garr_n); /* length: 14 header + pad 2 + 4 arrays of n */
    fb_u16(b, 0);                       /* language */
    fb_u16(b, 2 * n);                   /* segCountX2 */
    fb_u16(b, 0);                       /* searchRange, entrySelector, rangeShift: not read */
    fb_u16(b, 0);
    fb_u16(b, 0);
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(b, s[i].end);
    }
    fb_u16(b, 0); /* reservedPad */
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(b, s[i].start);
    }
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(b, s[i].delta);
    }
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(b, s[i].ro);
    }
    for (uint32_t i = 0; i < garr_n; i++) {
        fb_u16(b, garr[i]);
    }
}

typedef struct cm_rec {
    uint16_t pid, eid;
    const fb_buf *sub;
} cm_rec;

/* Wraps subtables in a cmap table and builds a 64-glyph font around it. */
static bool cm_font(const cm_rec *r, uint32_t n, fx_file *src, embi_font *f)
{
    fb_buf cm = {0};
    fb_u16(&cm, 0);
    fb_u16(&cm, n);
    uint32_t off = 4 + 8 * n;
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(&cm, r[i].pid);
        fb_u16(&cm, r[i].eid);
        fb_u32(&cm, off);
        off += (uint32_t)r[i].sub->n;
    }
    for (uint32_t i = 0; i < n; i++) {
        fb_bytes(&cm, r[i].sub->p, r[i].sub->n);
    }
    fb_table ex[1] = {{EMBI_TAG('c', 'm', 'a', 'p'), cm.p, cm.n}};
    const bool ok = fb_font(0x00010000u, 1000, 64, ex, 1, src);
    fb_free(&cm);
    if (!ok) {
        return false;
    }
    SDL_memset(f, 0, sizeof *f);
    return embi_font_open(src->p, src->n, 0, f);
}

/* Format 4 only (no format 12), (3,1). Segments, sorted by end code:
 *   0: 'A'..'C', idDelta 0xFFC9 (-55), idRangeOffset 0: gid = (cp + delta) & 0xFFFF = cp - 55,
 *      so 'A' (0x41) -> 10, 'B' -> 11, 'C' -> 12.
 *   1: 'a'..'c', idDelta 5, idRangeOffset 6 (the glyphIdArray follows the 4 idRangeOffset entries,
 *      so it starts 6 bytes after this entry) with array {30, 0, 31}: 'a' -> 35, 'b' -> 0
 *      (an array entry of 0 stays unmapped, the delta is not added), 'c' -> 36.
 *   2: 0xFF00..0xFF02, idDelta 0x0114, range offset 0: (0xFF00 + 0x0114) & 0xFFFF = 0x0014 = 20
 *      (the sum wraps past 16 bits), so 0xFF01 -> 21.
 *   3: the 0xFFFF sentinel, idDelta 1: (0xFFFF + 1) & 0xFFFF = 0, unmapped. */
EMB_TEST(cmap4_only_lookups)
{
    const cm4_seg segs[4] = {{0x41, 0x43, 0xFFC9, 0},
                             {0x61, 0x63, 5, 6},
                             {0xFF00, 0xFF02, 0x0114, 0},
                             {0xFFFF, 0xFFFF, 1, 0}};
    const uint16_t garr[3] = {30, 0, 31};
    fb_buf sub = {0};
    cm4_build(&sub, segs, 4, garr, 3);
    const cm_rec r[1] = {{3, 1, &sub}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(cm_font(r, 1, &src, &f));
    EMB_CHECK(f.cmap4.n != 0);
    EMB_CHECK(f.cmap12.n == 0);

    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 10);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x42), 11);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x43), 12);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x61), 35);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x62), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x63), 36);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xFF00), 20);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xFF01), 21);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xFF02), 22);
    /* Unmapped: below the first segment, in a gap, past a segment's end, the sentinel. */
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x20), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x50), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x44), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xFF03), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xFFFF), 0);
    /* Beyond the BMP a format 4 cmap cannot map anything; 0x10041 must not alias 0x0041. */
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x10041), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x10FFFF), 0);
    EMB_CHECK(embi_font_has_codepoint(&f, 0x41));
    EMB_CHECK(!embi_font_has_codepoint(&f, 0x62));

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&sub);
}

/* Format 12 only (3,10), 64 glyphs. Groups: 0x41..0x43 -> 5, 0x2000..0x2010 -> 60,
 * 0x1F600..0x1F602 -> 60. The 0x2000 group runs past numGlyphs: 0x2003 -> 63 is the last valid
 * glyph, 0x2004 would be glyph 64 (== numGlyphs) and 0x2010 glyph 76, both out of range, so 0. */
EMB_TEST(cmap12_group_past_num_glyphs)
{
    fb_buf sub = {0};
    fb_u16(&sub, 12);
    fb_u16(&sub, 0);
    fb_u32(&sub, 16 + 12 * 3);
    fb_u32(&sub, 0);
    fb_u32(&sub, 3);
    const uint32_t g[3][3] = {{0x41, 0x43, 5}, {0x2000, 0x2010, 60}, {0x1F600, 0x1F602, 60}};
    for (int i = 0; i < 3; i++) {
        fb_u32(&sub, g[i][0]);
        fb_u32(&sub, g[i][1]);
        fb_u32(&sub, g[i][2]);
    }
    const cm_rec r[1] = {{3, 10, &sub}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(cm_font(r, 1, &src, &f));
    EMB_CHECK(f.cmap12.n != 0);

    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 5);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x43), 7);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x2003), 63);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x2004), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x2010), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x1F600), 60);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x1F601), 61);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x1F602), 62);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x1F603), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x44), 0);
    EMB_CHECK(!embi_font_has_codepoint(&f, 0x2004));

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&sub);
}

/* Format 14 (0,5), variation selectors. Layout (offsets from the subtable start):
 *   header 10 bytes, 2 records of 11 bytes (to 32), then
 *   32: default UVS table for U+FE00 (1 range: U+0045, 0 additional) -- 8 bytes
 *   40: non-default UVS table for U+FE00 (4 mappings) -- 24 bytes
 *   64: default UVS table for U+FE0F (1 range: U+2764) -- 8 bytes, no non-default table.
 *   Records (sorted by selector): FE00 {def 32, nondef 40}, FE0F {def 64, nondef 0}.
 *   Non-default mappings: U+41 -> 7, U+4E00 -> 8, U+4E01 -> 200 (>= numGlyphs 64), U+20000 -> 9. */
EMB_TEST(cmap14_variation_selectors)
{
    fb_buf sub = {0};
    fb_u16(&sub, 14);
    fb_u32(&sub, 0); /* length: not read */
    fb_u32(&sub, 2);
    fb_u24(&sub, 0xFE00);
    fb_u32(&sub, 32);
    fb_u32(&sub, 40);
    fb_u24(&sub, 0xFE0F);
    fb_u32(&sub, 64);
    fb_u32(&sub, 0);
    EMB_REQUIRE(sub.n == 32);
    fb_u32(&sub, 1); /* default table for FE00 */
    fb_u24(&sub, 0x45);
    fb_u8(&sub, 0);
    fb_u32(&sub, 4); /* non-default table for FE00 */
    const uint32_t m[4][2] = {{0x41, 7}, {0x4E00, 8}, {0x4E01, 200}, {0x20000, 9}};
    for (int i = 0; i < 4; i++) {
        fb_u24(&sub, m[i][0]);
        fb_u16(&sub, m[i][1]);
    }
    EMB_REQUIRE(sub.n == 64);
    fb_u32(&sub, 1); /* default table for FE0F */
    fb_u24(&sub, 0x2764);
    fb_u8(&sub, 0);
    const cm_rec r[1] = {{0, 5, &sub}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(cm_font(r, 1, &src, &f));
    EMB_CHECK(f.cmap14.n != 0);

    /* Non-default mapping hits, including a code point beyond the BMP. */
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xFE00), 7);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x4E00, 0xFE00), 8);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x20000, 0xFE00), 9);
    /* Mapped glyph past numGlyphs, code point missing from the mapping table. */
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x4E01, 0xFE00), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x42, 0xFE00), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x4DFF, 0xFE00), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x20001, 0xFE00), 0);
    /* Default-UVS-only entries return 0 (documented: callers use the plain cmap). */
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x45, 0xFE00), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x2764, 0xFE0F), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xFE0F), 0);
    /* Selector absent from the table (before, between and after the records). */
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xFE01), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xFDFF), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xE0100), 0);
    /* The plain lookup knows nothing of format 14. */
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 0);

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&sub);
}

/* A font without a format 14 subtable answers 0. Noto-COLRv1.ttf has a (0,5,14) subtable (checked
 * with fontTools); it is picked up and a query for an absent selector is 0. */
EMB_TEST(cmap14_absent_and_real_font)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0xFE0F), 0);
    embi_font_close(&f);
    fx_free(&src);

    EMB_REQUIRE(fx_load("Noto-COLRv1.ttf", &src));
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK(f.cmap14.n != 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_variant(&f, 0x41, 0x0F0000), 0);
    embi_font_close(&f);
    fx_free(&src);
}

/* A symbol font: only a (3,0) format 4 subtable, with its characters at U+F000 + code.
 * Segment 0xF041..0xF043 with idDelta 0x0FCB: (0xF041 + 0x0FCB) & 0xFFFF = 0x000C = 12, so
 * 0x41 -> 12 (via 0xF041) and 0x42 -> 13. The raw code 0xF041 is found too. */
EMB_TEST(cmap_symbol_fallback)
{
    const cm4_seg segs[2] = {{0xF041, 0xF043, 0x0FCB, 0}, {0xFFFF, 0xFFFF, 1, 0}};
    fb_buf sym = {0};
    cm4_build(&sym, segs, 2, nullptr, 0);
    const cm_rec r[1] = {{3, 0, &sym}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(cm_font(r, 1, &src, &f));
    EMB_CHECK(f.cmap_symbol);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 12);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x42), 13);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x44), 0);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0xF041), 12);
    EMB_CHECK(embi_font_has_codepoint(&f, 0x43));
    embi_font_close(&f);
    fx_free(&src);

    /* With a Unicode subtable present the symbol one is ignored: (3,1) maps 'A' -> 20
     * (idDelta 0xFFD3 = 20 - 0x41 mod 65536). */
    const cm4_seg uni[2] = {{0x41, 0x41, 0xFFD3, 0}, {0xFFFF, 0xFFFF, 1, 0}};
    fb_buf u = {0};
    cm4_build(&u, uni, 2, nullptr, 0);
    const cm_rec r2[2] = {{3, 0, &sym}, {3, 1, &u}};
    EMB_REQUIRE(cm_font(r2, 2, &src, &f));
    EMB_CHECK(!f.cmap_symbol);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x41), 20);
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f, 0x42), 0);
    embi_font_close(&f);
    fx_free(&src);
    fb_free(&u);
    fb_free(&sym);
}

/* ------------------------------------------------------------------ name selection */

typedef struct nm_rec {
    uint16_t pid, eid, lang, nid;
    const char *s; /* ASCII, or raw Mac Roman bytes when pid is 1 */
} nm_rec;

/* Builds a name table (format 0). Windows and Unicode strings are written as UTF-16BE from the
 * ASCII text; Mac strings are written byte for byte. */
static bool nm_font(const nm_rec *r, uint32_t n, fx_file *src, embi_font *f)
{
    fb_buf st = {0}, nt = {0};
    uint16_t off[16], len[16];
    for (uint32_t i = 0; i < n; i++) {
        off[i] = (uint16_t)st.n;
        const size_t l = strlen(r[i].s);
        for (size_t k = 0; k < l; k++) {
            if (r[i].pid != 1) {
                fb_u8(&st, 0);
            }
            fb_u8(&st, (uint8_t)r[i].s[k]);
        }
        len[i] = (uint16_t)(st.n - off[i]);
    }
    fb_u16(&nt, 0);
    fb_u16(&nt, n);
    fb_u16(&nt, 6 + 12 * n);
    for (uint32_t i = 0; i < n; i++) {
        fb_u16(&nt, r[i].pid);
        fb_u16(&nt, r[i].eid);
        fb_u16(&nt, r[i].lang);
        fb_u16(&nt, r[i].nid);
        fb_u16(&nt, len[i]);
        fb_u16(&nt, off[i]);
    }
    fb_bytes(&nt, st.p, st.n);
    fb_table ex[1] = {{EMBI_TAG('n', 'a', 'm', 'e'), nt.p, nt.n}};
    const bool ok = fb_font(0x00010000u, 1000, 4, ex, 1, src);
    fb_free(&st);
    fb_free(&nt);
    if (!ok) {
        return false;
    }
    SDL_memset(f, 0, sizeof *f);
    return embi_font_open(src->p, src->n, 0, f);
}

/* Which encodings are read: Unicode platform (0) and Windows encodings 1, 10 and 0; Windows
 * encoding 2 (ShiftJIS) is skipped.
 *   ID 1 (family): platform 0 -> "Uni".  ID 2: platform 3 enc 10 -> "Ten".
 *   ID 4: only platform 3 enc 2 -> empty.  ID 16: only platform 3 enc 2 -> skipped, so it falls
 *   back to ID 1 -> "Uni".  ID 17: platform 3 enc 1 -> "Sub17". */
EMB_TEST(name_encoding_selection)
{
    const nm_rec r[] = {{0, 3, 0, 1, "Uni"},      {3, 10, 0x409, 2, "Ten"},
                        {3, 2, 0x411, 4, "Sjis"}, {3, 2, 0x411, 16, "Sjis16"},
                        {3, 1, 0x409, 17, "Sub17"}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_font(r, 5, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "Uni"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "Ten"));
    EMB_CHECK_EQ_INT(f.metrics.full_name.len, 0);
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "Uni"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "Sub17"));
    embi_font_close(&f);
    fx_free(&src);
}

/* Mac Roman is read only for language 0. ID 1 (Mac, lang 0) holds "Caf" + 0x8E (e acute in Mac
 * Roman) -> "Caf\xC3\xA9". ID 2 is Mac with language 1 (French): skipped, subfamily empty. */
EMB_TEST(name_mac_roman_only_english)
{
    const nm_rec r[] = {{1, 0, 0, 1, "Caf\x8e"}, {1, 0, 1, 2, "Fr"}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_font(r, 2, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "Caf\xC3\xA9"));
    EMB_CHECK_EQ_INT(f.metrics.subfamily.len, 0);
    embi_font_close(&f);
    fx_free(&src);
}

/* Typographic names: with IDs 1/2 and 16/17 all present, family stays the legacy ID 1 and
 * typo_family reports ID 16 (what CSS matching needs for "Noto Sans Light"). When only 16/17
 * exist, family/subfamily fall back to them. */
EMB_TEST(name_typographic_family)
{
    const nm_rec both[] = {{3, 1, 0x409, 1, "Noto Sans Light"}, {3, 1, 0x409, 2, "Regular"},
                           {3, 1, 0x409, 16, "Noto Sans"},      {3, 1, 0x409, 17, "Light"}};
    fx_file src;
    embi_font f;
    EMB_REQUIRE(nm_font(both, 4, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "Noto Sans Light"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "Regular"));
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "Noto Sans"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "Light"));
    embi_font_close(&f);
    fx_free(&src);

    const nm_rec only[] = {{3, 1, 0x409, 16, "Typo"}, {3, 1, 0x409, 17, "Bold"}};
    EMB_REQUIRE(nm_font(only, 2, &src, &f));
    EMB_CHECK(fx_str_is(f.metrics.family, "Typo"));
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "Bold"));
    EMB_CHECK(fx_str_is(f.metrics.typo_family, "Typo"));
    EMB_CHECK(fx_str_is(f.metrics.typo_subfamily, "Bold"));
    embi_font_close(&f);
    fx_free(&src);
}

EMB_TEST_MAIN()
