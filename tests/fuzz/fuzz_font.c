// fuzz-modules: font base
/* sfnt / TTC open, then every query: cmap lookups, advances, outlines for a few glyph ids (glyf,
 * CFF, CFF2, variations), COLR v0/v1 paint graphs and CPAL colours, names, MVAR, instances.
 * Input: a font file as it is. The last byte (not removed) picks the face index of a collection and an
 * extra glyph id, so the fixtures in tests/fonts work as seeds unchanged. */
#include "fuzz_common.h"

#include "font/font.h"

static bool colr_visit(void *ctx, const embi_colr_op *op)
{
    (void)op;
    uint32_t *n = ctx;
    return ++*n < 4096;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t tail = size ? data[size - 1] : 0;
    uint32_t face = tail & 3u;
    uint32_t extra_gid = (uint32_t)tail * 7u;
    (void)embi_font_count(in.p, in.n);
    embi_font f;
    memset(&f, 0, sizeof f);
    if (embi_font_open(in.p, in.n, face, &f)) {
        int16_t coords[64] = {0};
        float user[64] = {0};
        uint32_t na = f.axis_count > 64 ? 64 : f.axis_count;
        for (uint32_t a = 0; a < na; a++) {
            user[a] = f.axes[a].def;
        }
        (void)embi_font_normalize(&f, user, coords);
        /* Extreme axis positions too, which stress gvar/HVAR/avar interpolation. */
        int16_t hi[64];
        int16_t lo[64];
        for (uint32_t a = 0; a < 64; a++) {
            hi[a] = 16384;
            lo[a] = -16384;
        }

        const uint32_t gids[] = {0, 1, 2, 3, 5, 6, 10, 12, 36, 100, 1849, 0xFFFFu, extra_gid};
        embi_outline ol = {0};
        for (size_t g = 0; g < sizeof gids / sizeof gids[0]; g++) {
            float adv = 0.0f;
            (void)embi_font_glyph_advance(&f, gids[g], coords, &adv);
            (void)embi_font_glyph_advance(&f, gids[g], hi, &adv);
            (void)embi_font_glyph_outline(&f, gids[g], coords, &ol);
            (void)embi_font_glyph_outline(&f, gids[g], nullptr, &ol);
            if (na > 0) {
                (void)embi_font_glyph_outline(&f, gids[g], hi, &ol);
                (void)embi_font_glyph_outline(&f, gids[g], lo, &ol);
            }
            uint32_t ops = 0;
            const embi_colr_visitor v = {&ops, colr_visit};
            (void)embi_font_colr_paint(&f, gids[g], coords, 0, &v);
        }
        embi_outline_free(&ol);

        static const uint32_t cps[] = {0x20, 0x41, 0x42, 0xE9, 0x627, 0x915, 0xFF01u, 0x1F600u, 0x10FFFFu};
        for (size_t i = 0; i < sizeof cps / sizeof cps[0]; i++) {
            (void)embi_font_glyph_index(&f, cps[i]);
            (void)embi_font_has_codepoint(&f, cps[i]);
            (void)embi_font_glyph_variant(&f, cps[i], 0xFE0Fu);
        }
        const embi_str names[5] = {f.metrics.family, f.metrics.subfamily, f.metrics.full_name,
                                   f.metrics.typo_family, f.metrics.typo_subfamily};
        uint32_t sum = 0;
        for (int i = 0; i < 5; i++) {
            for (uint32_t k = 0; k < names[i].len; k++) {
                sum += (uint8_t)names[i].ptr[k];
            }
        }
        float delta = 0.0f;
        (void)embi_font_mvar_delta(&f, EMBI_TAG('x', 'h', 'g', 't'), coords, &delta);
        uint32_t insts = embi_font_instance_count(&f);
        float ins[64];
        for (uint32_t i = 0; i < insts && i < 4; i++) {
            (void)embi_font_instance_coords(&f, i, ins);
        }
        uint32_t pals = embi_font_palette_count(&f);
        uint8_t rgba[4];
        for (uint32_t p = 0; p < pals && p < 2; p++) {
            for (uint32_t e = 0; e < 4; e++) {
                (void)embi_font_cpal_color(&f, p, e, rgba);
            }
        }
        if (sum == 0xFFFFFFFFu) {
            fputs("", stderr);
        }
    }
    embi_font_close(&f);
    FUZZ_GUARD_END();
    return 0;
}
