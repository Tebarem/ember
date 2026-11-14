#include "font_build.h"

/* Robustness: every query must survive malformed input. The fixtures are truncated at many
 * offsets and have bytes flipped with a fixed-seed xorshift PRNG (so runs repeat exactly). A
 * run passes when nothing crashes, nothing reads out of bounds (checked by the sanitizer build
 * and by the allocator's live-byte count, which must return to its baseline), and a truncation
 * below the table directory's end never opens. */

static const char *const fuzz_fonts[] = {
    "NotoSans-Regular.ttf", "NotoSans-VF.ttf",       "SourceSans3-Regular.otf",
    "SourceSans3VF-Upright.otf", "Noto-COLRv1.ttf",
};

/* Glyph ids touched on each input: the synthetic glyphs (0..11, which cover the flex, hintmask,
 * subr and operator paths of the CFF1 test font; 5 and 6 are also the COLR base glyphs of the
 * synthetic COLR font), 'A' in each fixture, the Aacute composite, and COLR glyph 1849. Ids past the end are fine: the queries must reject them. */
static const uint32_t fuzz_gids[] = {0,  1,  2,  3,  4,   5,    6,   7,   8,   9,   10,
                                     11, 12, 28, 36, 131, 1849, 0xFFFFu};

/* Synthetic cmap: (3,1) format 4, (3,10) format 12 and (0,5) format 14. Codes 'A'..'C' map to
 * glyphs 10..12, and U+FE0F after 'A' selects glyph 12. */
static void fuzz_put_cmap(fb_buf *b)
{
    fb_buf f4 = {0}, f12 = {0}, f14 = {0};
    fb_u16(&f4, 4);
    fb_u16(&f4, 32);
    fb_u16(&f4, 0);
    fb_u16(&f4, 4); /* segCountX2 */
    fb_u16(&f4, 0);
    fb_u16(&f4, 0);
    fb_u16(&f4, 0);
    fb_u16(&f4, 0x43); /* end codes */
    fb_u16(&f4, 0xFFFF);
    fb_u16(&f4, 0);
    fb_u16(&f4, 0x41); /* start codes */
    fb_u16(&f4, 0xFFFF);
    fb_u16(&f4, 0xFFC9); /* deltas */
    fb_u16(&f4, 1);
    fb_u16(&f4, 0); /* range offsets */
    fb_u16(&f4, 0);
    fb_u16(&f12, 12);
    fb_u16(&f12, 0);
    fb_u32(&f12, 28);
    fb_u32(&f12, 0);
    fb_u32(&f12, 1);
    fb_u32(&f12, 0x41);
    fb_u32(&f12, 0x43);
    fb_u32(&f12, 10);
    fb_u16(&f14, 14);
    fb_u32(&f14, 32);
    fb_u32(&f14, 1);
    fb_u24(&f14, 0xFE0F);
    fb_u32(&f14, 0);
    fb_u32(&f14, 21);
    fb_u32(&f14, 1);
    fb_u24(&f14, 0x41);
    fb_u16(&f14, 12);
    fb_u16(b, 0);
    fb_u16(b, 3);
    uint32_t off = 4 + 8 * 3;
    fb_u16(b, 3);
    fb_u16(b, 1);
    fb_u32(b, off);
    off += (uint32_t)f4.n;
    fb_u16(b, 3);
    fb_u16(b, 10);
    fb_u32(b, off);
    off += (uint32_t)f12.n;
    fb_u16(b, 0);
    fb_u16(b, 5);
    fb_u32(b, off);
    fb_bytes(b, f4.p, f4.n);
    fb_bytes(b, f12.p, f12.n);
    fb_bytes(b, f14.p, f14.n);
    fb_free(&f4);
    fb_free(&f12);
    fb_free(&f14);
}

/* Synthetic name table: Windows and Mac records for IDs 1, 2, 4, 16 and 17. */
static void fuzz_put_name(fb_buf *b)
{
    static const struct {
        uint16_t pid, eid, nid;
        const char *s;
    } r[6] = {
        {3, 1, 1, "Family"},    {1, 0, 1, "MacFam"},     {3, 1, 2, "Style"},
        {3, 1, 4, "Full Name"}, {3, 1, 16, "TypoFam"}, {3, 1, 17, "TypoSty"},
    };
    fb_u16(b, 0);
    fb_u16(b, 6);
    fb_u16(b, 6 + 12 * 6);
    uint32_t off = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t len = (uint32_t)SDL_strlen(r[i].s) * (r[i].pid == 1 ? 1u : 2u);
        fb_u16(b, r[i].pid);
        fb_u16(b, r[i].eid);
        fb_u16(b, r[i].pid == 1 ? 0 : 0x409);
        fb_u16(b, r[i].nid);
        fb_u16(b, len);
        fb_u16(b, off);
        off += len;
    }
    for (int i = 0; i < 6; i++) {
        for (const char *c = r[i].s; *c != 0; c++) {
            if (r[i].pid != 1) {
                fb_u8(b, 0);
            }
            fb_u8(b, (uint8_t)*c);
        }
    }
}

/* COLRv1 with two base glyphs: 5 is a linear gradient with three stops, 6 a solid fill. */
static void fuzz_put_colr(fb_buf *b)
{
    fb_u16(b, 1);
    fb_u16(b, 0);
    fb_u32(b, 0);
    fb_u32(b, 0);
    fb_u16(b, 0);
    fb_u32(b, 34); /* baseGlyphListOffset */
    fb_u32(b, 0);
    fb_u32(b, 0);
    fb_u32(b, 0);
    fb_u32(b, 0);
    fb_u32(b, 2); /* BaseGlyphList at 34: two records, paints at +16 and +53 */
    fb_u16(b, 5);
    fb_u32(b, 16);
    fb_u16(b, 6);
    fb_u32(b, 53);
    fb_u8(b, 4); /* PaintLinearGradient, colorLine at +16 */
    fb_u24(b, 16);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 100);
    fb_u8(b, 2); /* ColorLine: reflect, three stops */
    fb_u16(b, 3);
    fb_u16(b, 0);
    fb_u16(b, 0);
    fb_u16(b, 16384);
    fb_u16(b, 8192);
    fb_u16(b, 1);
    fb_u16(b, 8192);
    fb_u16(b, 16384);
    fb_u16(b, 2);
    fb_u16(b, 8192);
    fb_u8(b, 2); /* PaintSolid: palette entry 1, alpha 1 */
    fb_u16(b, 1);
    fb_u16(b, 16384);
}

static void fuzz_put_cpal(fb_buf *b)
{
    fb_u16(b, 0);
    fb_u16(b, 3);
    fb_u16(b, 1);
    fb_u16(b, 3);
    fb_u32(b, 14);
    fb_u16(b, 0);
    const uint8_t rec[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    fb_bytes(b, &rec[0][0], sizeof rec);
}

/* Inputs are the fixtures, then the synthetic CFF1 font (index FUZZ_NFILES), then the synthetic
 * cmap, name and COLR fonts. */
#define FUZZ_NFILES (sizeof fuzz_fonts / sizeof fuzz_fonts[0])
#define FUZZ_NINPUTS (FUZZ_NFILES + 4)

static bool fuzz_load(size_t fi, fx_file *out)
{
    if (fi == FUZZ_NFILES) {
        return fb_synth_cff_font(1000, out);
    }
    if (fi > FUZZ_NFILES) {
        fb_buf t = {0}, cpal = {0};
        uint32_t tag = EMBI_TAG('c', 'm', 'a', 'p');
        if (fi == FUZZ_NFILES + 1) {
            fuzz_put_cmap(&t);
        } else if (fi == FUZZ_NFILES + 2) {
            tag = EMBI_TAG('n', 'a', 'm', 'e');
            fuzz_put_name(&t);
        } else {
            tag = EMBI_TAG('C', 'O', 'L', 'R');
            fuzz_put_colr(&t);
        }
        fuzz_put_cpal(&cpal);
        const fb_table ex[2] = {{tag, t.p, t.n}, {EMBI_TAG('C', 'P', 'A', 'L'), cpal.p, cpal.n}};
        const bool ok = fb_font(0x00010000u, 1000, 64, ex, 2, out);
        fb_free(&t);
        fb_free(&cpal);
        return ok;
    }
    return fx_load(fuzz_fonts[fi], out);
}

static uint32_t xs_state;

static uint32_t xs_next(void)
{
    uint32_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs_state = x;
    return x;
}

static bool fuzz_colr_visit(void *ctx, const embi_colr_op *op)
{
    (void)op;
    (*(uint32_t *)ctx)++;
    return true;
}

/* Opens the bytes and runs every query. The return value says only whether the face opened;
 * the test cares that nothing crashes and the allocator balances. */
static bool exercise(const uint8_t *p, size_t n, embi_outline *ol)
{
    embi_font f;
    SDL_memset(&f, 0, sizeof f);
    if (!embi_font_open(p, n, 0, &f)) {
        embi_font_close(&f);
        return false;
    }

    int16_t coords[64];
    float user[64];
    uint32_t na = f.axis_count;
    if (na > 64) {
        na = 64;
    }
    for (uint32_t a = 0; a < na; a++) {
        coords[a] = 0;
        user[a] = f.axes[a].def;
    }
    (void)embi_font_normalize(&f, user, coords);

    for (size_t g = 0; g < sizeof fuzz_gids / sizeof fuzz_gids[0]; g++) {
        float adv = 0.0f;
        (void)embi_font_glyph_advance(&f, fuzz_gids[g], coords, &adv);
        (void)embi_font_glyph_outline(&f, fuzz_gids[g], coords, ol);
        (void)embi_font_glyph_outline(&f, fuzz_gids[g], nullptr, ol);

        uint32_t ops = 0;
        const embi_colr_visitor v = {&ops, fuzz_colr_visit};
        (void)embi_font_colr_paint(&f, fuzz_gids[g], coords, 0, &v);
    }

    static const uint32_t cps[] = {0x20, 0x41, 0x42, 0x43, 0xFF01u, 0x1F600u, 0x10FFFFu};
    for (size_t i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        (void)embi_font_glyph_index(&f, cps[i]);
        (void)embi_font_has_codepoint(&f, cps[i]);
        (void)embi_font_glyph_variant(&f, cps[i], 0xFE0Fu);
        (void)embi_font_glyph_variant(&f, cps[i], 0xFE00u);
    }

    /* Read every name byte, so an out-of-bounds string is caught by the sanitizer. */
    const embi_str names[5] = {f.metrics.family, f.metrics.subfamily, f.metrics.full_name,
                               f.metrics.typo_family, f.metrics.typo_subfamily};
    uint32_t sum = 0;
    for (int i = 0; i < 5; i++) {
        for (uint32_t k = 0; k < names[i].len; k++) {
            sum += (uint8_t)names[i].ptr[k];
        }
    }
    if (sum == 0xFFFFFFFFu) {
        SDL_Log("unlikely");
    }

    float delta = 0.0f;
    (void)embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), coords, &delta);

    uint32_t insts = embi_font_instance_count(&f);
    if (insts > 4) {
        insts = 4;
    }
    float ins[64];
    for (uint32_t i = 0; i < insts; i++) {
        (void)embi_font_instance_coords(&f, i, ins);
    }

    uint8_t rgba[4];
    (void)embi_font_cpal_color(&f, 0, 0, rgba);

    embi_font_close(&f);
    return true;
}

/* End of the sfnt table directory. Opening is lenient per table (an unused table cut off by the
 * truncation does not fail the font), but a file cut inside the directory itself loses records,
 * so it must not open. Zero when the header is too short to read. */
static size_t sfnt_table_end(const uint8_t *p, size_t n)
{
    if (n < 12) {
        return 0;
    }
    return 12 + 16 * (((size_t)p[4] << 8) | p[5]);
}

EMB_TEST(fuzz_truncations_never_crash)
{
    for (size_t fi = 0; fi < FUZZ_NINPUTS; fi++) {
        fx_file src;
        EMB_REQUIRE(fuzz_load(fi, &src));
        const size_t table_end = sfnt_table_end(src.p, src.n);
        const size_t base = embi_mem_live();

        embi_outline ol;
        SDL_memset(&ol, 0, sizeof ol);
        size_t step = src.n / 512;
        if (step == 0) {
            step = 1;
        }
        for (size_t cut = 0; cut < src.n; cut += step) {
            uint8_t *buf = (uint8_t *)SDL_malloc(cut + 1);
            EMB_REQUIRE(buf != nullptr);
            SDL_memcpy(buf, src.p, cut);
            const bool ok = exercise(buf, cut, &ol);
            /* A file cut inside its own table directory must not open. */
            if (ok && cut < table_end) {
                EMB_CHECK(!ok);
            }
            SDL_free(buf);
        }
        embi_outline_free(&ol);
        EMB_CHECK_EQ_INT(embi_mem_live(), base);
        fx_free(&src);
    }
}

EMB_TEST(fuzz_byte_flips_never_crash)
{
    for (size_t fi = 0; fi < FUZZ_NINPUTS; fi++) {
        fx_file src;
        EMB_REQUIRE(fuzz_load(fi, &src));
        uint8_t *buf = (uint8_t *)SDL_malloc(src.n);
        EMB_REQUIRE(buf != nullptr);
        SDL_memcpy(buf, src.p, src.n);

        xs_state = 0x9E3779B9u ^ (uint32_t)(fi * 7919u + 1u);
        const size_t base = embi_mem_live();
        embi_outline ol;
        SDL_memset(&ol, 0, sizeof ol);

        for (int it = 0; it < 1000; it++) {
            /* Two flips per round: a single bit flip, or a random byte. The old values are
             * restored in reverse order so a repeated position also restores. */
            const size_t p1 = xs_next() % src.n;
            const size_t p2 = xs_next() % src.n;
            const uint32_t r1 = xs_next();
            const uint32_t r2 = xs_next();
            const uint8_t old1 = buf[p1];
            buf[p1] = (r1 & 1u) ? (uint8_t)(old1 ^ (1u << ((r1 >> 1) & 7u))) : (uint8_t)(r1 >> 8);
            const uint8_t old2 = buf[p2];
            buf[p2] = (r2 & 1u) ? (uint8_t)(old2 ^ (1u << ((r2 >> 1) & 7u))) : (uint8_t)(r2 >> 8);

            (void)exercise(buf, src.n, &ol);

            buf[p2] = old2;
            buf[p1] = old1;
        }

        embi_outline_free(&ol);
        EMB_CHECK_EQ_INT(embi_mem_live(), base);
        EMB_CHECK(SDL_memcmp(buf, src.p, src.n) == 0);
        SDL_free(buf);
        fx_free(&src);
    }
}

EMB_TEST_MAIN()
