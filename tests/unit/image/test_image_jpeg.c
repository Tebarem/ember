/*
 * JPEG through SDL_image (stb_image): every fixture decodes to the ImageMagick reference within
 * the tolerance below, EXIF orientations 1-8 give the oriented size and pixels, the orientation
 * can be turned off, and truncated files fail or decode without crashing. Each test checks that
 * the live allocation count returns to its baseline.
 *
 * Tolerance. JPEG decoders differ in IDCT and upsampling rounding, so the reference is not
 * bit-exact. Every channel value must be within JPEG_MAX_ERR of the reference, and at most
 * JPEG_MAX_OVER3_PPM of the channel values may differ by more than 3. The measured maximum is
 * printed for each fixture (2 with stb_image against ImageMagick at the time of writing).
 */
#include "fmt_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

#define JPEG_MAX_ERR 3
#define JPEG_MAX_OVER3_PPM 1000u /* 0.1 percent of channel values */

static const char *kJpegFixtures[] = {
    "jpg_baseline_420", "jpg_baseline_444", "jpg_progressive", "jpg_gray",
    "jpg_cmyk_adobe",   "jpg_restart",      "jpg_1x1",         "jpg_17x13",
};

/* Decodes tests/image/<name>.jpg and compares it with tests/image/ref/<name>.png. */
static bool check_against_ref(const char *name, bool apply_orientation)
{
    char rel[256];
    SDL_snprintf(rel, sizeof rel, "tests/image/%s.jpg", name);
    size_t len = 0;
    uint8_t *bytes = fx_load(rel, &len);
    if (!bytes) {
        return false;
    }

    embi_pixels got = {0};
    bool ok = embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, apply_orientation,
                                nullptr, nullptr, &got);
    SDL_free(bytes);
    if (!ok) {
        SDL_Log("jpeg %s: decode refused", name);
        return false;
    }

    embi_pixels ref = {0};
    if (!fx_ref(name, &ref)) {
        embi_pixels_free(&got);
        return false;
    }
    fx_diff d;
    bool same = fx_compare(&got, &ref, false, &d);
    SDL_Log("jpeg %-18s %ux%u max_err=%d over3=%llu of %llu channels", name, got.w, got.h, d.max_err,
            (unsigned long long)d.over3, (unsigned long long)d.channels);
    bool within = same && d.max_err <= JPEG_MAX_ERR &&
                  d.over3 * 1000000u <= d.channels * JPEG_MAX_OVER3_PPM;
    embi_pixels_free(&got);
    embi_pixels_free(&ref);
    return within;
}

EMB_TEST(jpeg_fixtures_match_reference)
{
    size_t live0 = embi_mem_live();
    for (size_t i = 0; i < SDL_arraysize(kJpegFixtures); i++) {
        EMB_CHECK(check_against_ref(kJpegFixtures[i], true));
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(jpeg_sniff_and_header_size)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/jpg_progressive.jpg", &len);
    EMB_REQUIRE(bytes != nullptr);
    EMB_CHECK(embi_image_sniff(bytes, len) == EMBI_IMAGE_FMT_JPEG);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_jpeg_probe(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK_EQ_INT(w, 96);
    EMB_CHECK_EQ_INT(h, 64);
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Orientations 1-8 on a 40x24 image. Each one must give the reference orientation (ImageMagick
 * -auto-orient) and the swapped size for 5-8. */
EMB_TEST(jpeg_exif_orientations_1_to_8)
{
    size_t live0 = embi_mem_live();
    for (int o = 1; o <= 8; o++) {
        char name[32];
        SDL_snprintf(name, sizeof name, "jpg_exif_o%d", o);
        char rel[256];
        SDL_snprintf(rel, sizeof rel, "tests/image/%s.jpg", name);
        size_t len = 0;
        uint8_t *bytes = fx_load(rel, &len);
        EMB_REQUIRE(bytes != nullptr);
        EMB_CHECK_EQ_INT(embi_jpeg_orientation(bytes, len), o);

        embi_pixels raw = {0};
        EMB_CHECK(embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, nullptr,
                                    nullptr, &raw));
        EMB_CHECK_EQ_INT(raw.w, 40);
        EMB_CHECK_EQ_INT(raw.h, 24);
        embi_pixels_free(&raw);

        EMB_CHECK(check_against_ref(name, true));
        embi_pixels got = {0};
        EMB_CHECK(embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr,
                                    nullptr, &got));
        bool swapped = o >= 5;
        EMB_CHECK_EQ_INT(got.w, swapped ? 24 : 40);
        EMB_CHECK_EQ_INT(got.h, swapped ? 40 : 24);
        embi_pixels ref = {0};
        EMB_REQUIRE(fx_ref(name, &ref));
        fx_diff d;
        EMB_CHECK(fx_compare(&got, &ref, false, &d));
        EMB_CHECK(d.max_err <= JPEG_MAX_ERR);
        embi_pixels_free(&got);
        embi_pixels_free(&ref);
        SDL_free(bytes);
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* The natural size reported for orientation 6 is the oriented one, and turning orientation off
 * keeps the stored size. */
EMB_TEST(jpeg_orientation_off_keeps_stored_size)
{
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/jpg_exif_o6.jpg", &len);
    EMB_REQUIRE(bytes != nullptr);
    embi_pixels p = {0};
    EMB_CHECK(embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, nullptr, nullptr,
                                &p));
    EMB_CHECK_EQ_INT(p.w, 40);
    EMB_CHECK_EQ_INT(p.h, 24);
    embi_pixels_free(&p);
    SDL_free(bytes);
}

/* Truncated files: the header cut must fail; the cut through the scan data may fail or decode
 * (stb_image pads missing data). Either way it must return and keep the allocation count. */
EMB_TEST(jpeg_truncated_files_do_not_crash)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/jpg_header_cut.jpg", &len);
    EMB_REQUIRE(bytes != nullptr);
    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr,
                                 &p));
    SDL_free(bytes);

    bytes = fx_load("tests/image/jpg_truncated.jpg", &len);
    EMB_REQUIRE(bytes != nullptr);
    if (embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p)) {
        uint32_t w = 0;
        uint32_t h = 0;
        EMB_CHECK(embi_jpeg_probe(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
        EMB_CHECK_EQ_INT(p.w, w);
        EMB_CHECK_EQ_INT(p.h, h);
        embi_pixels_free(&p);
    }
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
