#include "font_build.h"

/* Container and table-directory checks: sfnt, TTC, rejected formats and malformed directories.
 * The TTC is synthesized from NotoSans-Regular.ttf: a 'ttcf' header, two table directories
 * whose records point at the original bytes (shifted), then the original file. */

static uint32_t rd16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Builds a two-face TTC from a single sfnt. Caller frees with SDL_free. */
static uint8_t *build_ttc2(const uint8_t *src, size_t n, size_t *out_n)
{
    const uint32_t nt = rd16(src + 4);
    const size_t dir = 12 + 16 * (size_t)nt;
    const size_t shift = 20 + 2 * dir; /* header (12 + 2 offsets), two directories */
    uint8_t *buf = (uint8_t *)SDL_malloc(shift + n);
    if (buf == nullptr) {
        return nullptr;
    }
    wr32(buf + 0, 0x74746366u); /* 'ttcf' */
    wr32(buf + 4, 0x00010000u);
    wr32(buf + 8, 2);
    wr32(buf + 12, 20);
    wr32(buf + 16, (uint32_t)(20 + dir));
    for (uint32_t face = 0; face < 2; face++) {
        uint8_t *d = buf + 20 + face * dir;
        SDL_memcpy(d, src, dir);
        for (uint32_t i = 0; i < nt; i++) {
            uint8_t *rec = d + 12 + 16 * i;
            wr32(rec + 8, rd32(rec + 8) + (uint32_t)shift);
        }
    }
    SDL_memcpy(buf + shift, src, n);
    *out_n = shift + n;
    return buf;
}

EMB_TEST(sfnt_count_is_one)
{
    fx_file f;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &f));
    EMB_CHECK_EQ_INT(embi_font_count(f.p, f.n), 1);
    fx_free(&f);
}

EMB_TEST(unknown_bytes_are_rejected)
{
    uint8_t junk[256];
    SDL_memset(junk, 0xAB, sizeof junk);
    EMB_CHECK_EQ_INT(embi_font_count(junk, sizeof junk), 0);
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK(!embi_font_open(junk, sizeof junk, 0, &f));
    EMB_CHECK_EQ_INT(f.metrics.units_per_em, 0);
    embi_font_close(&f);
}

EMB_TEST(zero_length_input_is_rejected)
{
    uint8_t one = 0;
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK_EQ_INT(embi_font_count(&one, 0), 0);
    EMB_CHECK(!embi_font_open(&one, 0, 0, &f));
    embi_font_close(&f);
}

EMB_TEST(truncated_header_is_rejected)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    /* The offset table is 12 bytes; 11 bytes cannot hold it. */
    EMB_CHECK(!embi_font_open(src.p, 11, 0, &f));
    embi_font_close(&f);
    fx_free(&src);
}

EMB_TEST(table_count_overflow_is_rejected)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    /* numTables = 0xFFFF: the 16-byte records run far past the end of the file. */
    wr16(src.p + 4, 0xFFFFu);
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK(!embi_font_open(src.p, src.n, 0, &f));
    embi_font_close(&f);
    fx_free(&src);
}

EMB_TEST(woff_and_woff2_are_rejected)
{
    /* WOFF and WOFF2 are decoded in R2d, so R2a fails them without touching the data. */
    fx_file woff, woff2;
    EMB_REQUIRE(fx_load("NotoSans-Regular.woff", &woff));
    EMB_REQUIRE(fx_load("NotoSans-Regular.woff2", &woff2));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK(!embi_font_open(woff.p, woff.n, 0, &f));
    EMB_CHECK(!embi_font_open(woff2.p, woff2.n, 0, &f));
    EMB_CHECK_EQ_INT(f.metrics.units_per_em, 0);
    embi_font_close(&f);
    fx_free(&woff);
    fx_free(&woff2);
}

EMB_TEST(face_index_past_sfnt_is_rejected)
{
    fx_file f0;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &f0));
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK(!embi_font_open(f0.p, f0.n, 1, &f));
    embi_font_close(&f);
    fx_free(&f0);
}

EMB_TEST(ttc_two_faces_open_independently)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-Regular.ttf", &src));
    size_t n = 0;
    uint8_t *ttc = build_ttc2(src.p, src.n, &n);
    EMB_REQUIRE(ttc != nullptr);

    EMB_CHECK_EQ_INT(embi_font_count(ttc, n), 2);
    for (uint32_t face = 0; face < 2; face++) {
        embi_font f;
        SDL_memset(&f, 0, sizeof f);
        EMB_REQUIRE(embi_font_open(ttc, n, face, &f));
        EMB_CHECK_EQ_INT(f.metrics.units_per_em, 1000);
        EMB_CHECK_EQ_INT(f.metrics.num_glyphs, 3884);
        EMB_CHECK(fx_str_is(f.metrics.family, "Noto Sans"));
        embi_font_close(&f);
    }
    embi_font extra;
    SDL_memset(&extra, 0, sizeof extra);
    EMB_CHECK(!embi_font_open(ttc, n, 2, &extra));
    embi_font_close(&extra);
    SDL_free(ttc);
    fx_free(&src);
}

EMB_TEST(close_is_idempotent_and_frees_everything)
{
    fx_file src;
    EMB_REQUIRE(fx_load("NotoSans-VF.ttf", &src));
    const size_t before = embi_mem_live();
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK(embi_mem_live() > before);
    embi_font_close(&f);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
    embi_font_close(&f); /* second close on a closed font */
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
    fx_free(&src);
}

EMB_TEST(failed_open_leaves_no_allocations)
{
    uint8_t junk[64];
    SDL_memset(junk, 0x55, sizeof junk);
    const size_t before = embi_mem_live();
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_CHECK(!embi_font_open(junk, sizeof junk, 0, &f));
    embi_font_close(&f);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
}


/* A 'name' table built here: format 0 with three records and the string storage after them.
 * Record 0 (Windows family) points outside the table, so the family stays empty and the font
 * still opens. Record 1 is a Mac Roman subfamily with bytes 0x80 0x41, which must decode through
 * the Mac Roman table to U+00C4 then 'A'. Record 2 is a Windows full name in UTF-16BE, "AB". */
static void cont_put_name(fb_buf *b)
{
    fb_u16(b, 0);  /* format */
    fb_u16(b, 3);  /* count */
    fb_u16(b, 42); /* stringOffset: 6 + 3 * 12 */
    /* record 0: platform 3, encoding 1, language 0x409, nameID 1, length 4, offset 0xFFF0 */
    fb_u16(b, 3); fb_u16(b, 1); fb_u16(b, 0x409); fb_u16(b, 1); fb_u16(b, 4); fb_u16(b, 0xFFF0);
    /* record 1: platform 1 (Mac), encoding 0, nameID 2, length 2, offset 0 */
    fb_u16(b, 1); fb_u16(b, 0); fb_u16(b, 0); fb_u16(b, 2); fb_u16(b, 2); fb_u16(b, 0);
    /* record 2: platform 3, encoding 1, nameID 4, length 4, offset 2 */
    fb_u16(b, 3); fb_u16(b, 1); fb_u16(b, 0x409); fb_u16(b, 4); fb_u16(b, 4); fb_u16(b, 2);
    fb_u8(b, 0x80); /* storage at 42: record 1 */
    fb_u8(b, 0x41);
    fb_u8(b, 0x00); /* storage at 44: record 2, UTF-16BE "AB" */
    fb_u8(b, 0x41);
    fb_u8(b, 0x00);
    fb_u8(b, 0x42);
}

/* Name strings are optional: a bad offset empties that name only, and Mac Roman bytes decode
 * through the table. */
EMB_TEST(name_bad_offset_is_empty_and_mac_roman_decodes)
{
    fb_buf nm = {0};
    cont_put_name(&nm);
    fb_table ex[1] = {{EMBI_TAG('n', 'a', 'm', 'e'), nm.p, nm.n}};
    fx_file src;
    EMB_REQUIRE(fb_font(0x00010000u, 1000, 1, ex, 1, &src));

    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    EMB_REQUIRE(fx_open(&src, 0, &f));
    EMB_CHECK_EQ_INT(f.metrics.family.len, 0);
    EMB_CHECK(fx_str_is(f.metrics.subfamily, "\xC3\x84" "A"));
    EMB_CHECK(fx_str_is(f.metrics.full_name, "AB"));

    embi_font_close(&f);
    fx_free(&src);
    fb_free(&nm);
}

EMB_TEST_MAIN()
