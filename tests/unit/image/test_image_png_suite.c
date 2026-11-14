/*
 * PngSuite conformance. Every valid image (every *.png that does not start with x) must decode to
 * its reference pixels: ref/<name>.rgba holds a little-endian u32 width, u32 height, then
 * non-premultiplied RGBA8. The straight-alpha decode must equal the reference exactly. The
 * premultiplied decode must equal the reference premultiplied with (c * a + 127) / 255, which is
 * the decoder's own rounding. Every x* file is deliberately corrupt and must fail cleanly.
 */
#include "png_suite_util.h"

#include "base/base.h"
#include "image/png.h"

static uint32_t suite_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t suite_premul(uint8_t c, uint8_t a)
{
    return (uint8_t)(((uint32_t)c * a + 127u) / 255u);
}

static ps_list g_pngs;
static ps_list g_refs;

EMB_TEST(pngsuite_valid_images_match_reference_pixels)
{
    ps_dir("tests/data/pngsuite", &g_pngs);
    ps_dir("tests/data/pngsuite/ref", &g_refs);
    EMB_REQUIRE(g_pngs.n > 0 && g_refs.n > 0);

    int checked = 0;
    int bad = 0;
    for (int i = 0; i < g_refs.n; i++) {
        const char *fname = g_refs.names[i];
        if (!ps_has_suffix(fname, ".rgba")) {
            continue;
        }
        char stem[PS_NAME_LEN];
        ps_stem(fname, ".rgba", stem, sizeof stem);

        char png_rel[256];
        char ref_rel[256];
        SDL_snprintf(png_rel, sizeof png_rel, "tests/data/pngsuite/%s.png", stem);
        SDL_snprintf(ref_rel, sizeof ref_rel, "tests/data/pngsuite/ref/%s.rgba", stem);

        size_t rl = 0;
        uint8_t *ref = ps_load(ref_rel, &rl);
        size_t pl = 0;
        uint8_t *png = ps_load(png_rel, &pl);
        if (!ref || rl < 8 || !png) {
            SDL_Log("pngsuite: cannot load %s", stem);
            bad++;
            SDL_free(ref);
            SDL_free(png);
            continue;
        }
        uint32_t w = suite_le32(ref);
        uint32_t h = suite_le32(ref + 4);
        size_t npx = (size_t)w * h * 4;
        if (rl != 8 + npx) {
            SDL_Log("pngsuite: %s reference has the wrong size", stem);
            bad++;
            SDL_free(ref);
            SDL_free(png);
            continue;
        }
        const uint8_t *rgba = ref + 8;

        bool fail = false;
        embi_pixels p = {0};
        if (!embi_png_decode(png, pl, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &p)) {
            SDL_Log("pngsuite: %s did not decode", stem);
            fail = true;
        } else {
            if (p.w != w || p.h != h || SDL_memcmp(p.px, rgba, npx) != 0) {
                SDL_Log("pngsuite: %s straight pixels differ", stem);
                fail = true;
            }
            embi_pixels_free(&p);
        }

        if (!fail) {
            if (!embi_png_decode(png, pl, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, &p)) {
                SDL_Log("pngsuite: %s did not decode premultiplied", stem);
                fail = true;
            } else {
                for (size_t k = 0; k < npx; k += 4) {
                    uint8_t a = rgba[k + 3];
                    if (p.px[k] != suite_premul(rgba[k], a) ||
                        p.px[k + 1] != suite_premul(rgba[k + 1], a) ||
                        p.px[k + 2] != suite_premul(rgba[k + 2], a) || p.px[k + 3] != a) {
                        SDL_Log("pngsuite: %s premultiplied pixels differ", stem);
                        fail = true;
                        break;
                    }
                }
                embi_pixels_free(&p);
            }
        }

        checked++;
        if (fail) {
            bad++;
        }
        SDL_free(ref);
        SDL_free(png);
    }

    /* The suite has 161 valid images plus PngSuite.png; a short list would hide a broken listing. */
    EMB_CHECK(checked >= 150);
    EMB_CHECK_EQ_INT(bad, 0);
}

EMB_TEST(pngsuite_every_valid_file_has_a_reference)
{
    ps_dir("tests/data/pngsuite", &g_pngs);
    ps_dir("tests/data/pngsuite/ref", &g_refs);
    int missing = 0;
    for (int i = 0; i < g_pngs.n; i++) {
        const char *fname = g_pngs.names[i];
        if (!ps_has_suffix(fname, ".png") || fname[0] == 'x') {
            continue;
        }
        char stem[PS_NAME_LEN];
        ps_stem(fname, ".png", stem, sizeof stem);
        char want[PS_NAME_LEN + 8];
        SDL_snprintf(want, sizeof want, "%s.rgba", stem);
        if (!ps_listed(&g_refs, want)) {
            SDL_Log("pngsuite: no reference for %s", fname);
            missing++;
        }
    }
    EMB_CHECK_EQ_INT(missing, 0);
}

EMB_TEST(pngsuite_corrupt_files_fail_cleanly)
{
    ps_dir("tests/data/pngsuite", &g_pngs);
    int corrupt = 0;
    int accepted = 0;
    for (int i = 0; i < g_pngs.n; i++) {
        const char *fname = g_pngs.names[i];
        if (!ps_has_suffix(fname, ".png") || fname[0] != 'x') {
            continue;
        }
        char rel[256];
        SDL_snprintf(rel, sizeof rel, "tests/data/pngsuite/%s", fname);
        size_t len = 0;
        uint8_t *data = ps_load(rel, &len);
        if (!data) {
            SDL_Log("pngsuite: cannot load %s", fname);
            accepted++;
            continue;
        }
        corrupt++;
        embi_pixels p = {0};
        if (embi_png_decode(data, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &p)) {
            SDL_Log("pngsuite: corrupt %s was accepted", fname);
            embi_pixels_free(&p);
            accepted++;
        }
        SDL_free(data);
    }
    /* The suite ships 14 x* files. */
    EMB_CHECK(corrupt >= 14);
    EMB_CHECK_EQ_INT(accepted, 0);
}

EMB_TEST_MAIN()
