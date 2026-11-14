#include "emb_gold.h"

/* File-level behaviour of the golden harness: BMP round trip, update mode, missing or
 * mismatched goldens, and the failure outputs. Everything is written under the scratch
 * directories selftest-gold and selftest-out, relative to the working directory. */

static void use_scratch_dirs(void)
{
    SDL_setenv_unsafe("EMBER_GOLD_DIR", "selftest-gold", 1);
    SDL_setenv_unsafe("EMBER_GOLD_OUT", "selftest-out", 1);
    SDL_unsetenv_unsafe("EMBER_UPDATE_GOLDENS");
    SDL_CreateDirectory("selftest-gold");
    SDL_CreateDirectory("selftest-out");
}

static bool file_exists(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* A gradient in R (by x) and G (by y), constant B and A. */
static void make_gradient(uint8_t *px, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            p[0] = (uint8_t)(x * 8);
            p[1] = (uint8_t)(y * 8);
            p[2] = 200;
            p[3] = 255;
        }
    }
}

EMB_TEST(gold_default_root_is_tests_gold)
{
    char buf[EMB_GOLD_PATH_MAX];
    SDL_unsetenv_unsafe("EMBER_GOLD_DIR");
    emb_gold_root(buf, sizeof buf);
    size_t n = SDL_strlen(buf);
    EMB_REQUIRE(n >= 6);
    EMB_CHECK(SDL_strcmp(buf + n - 4, "gold") == 0);
    EMB_CHECK(buf[n - 5] == '/' || buf[n - 5] == '\\');
    EMB_CHECK(SDL_strstr(buf, "tests") != nullptr);
    use_scratch_dirs();
}

EMB_TEST(gold_bmp_round_trip_keeps_pixels)
{
    use_scratch_dirs();
    /* Varied alpha, so a loader or writer that drops alpha shows up here. */
    uint8_t px[8 * 4 * 4];
    for (int i = 0; i < 8 * 4; i++) {
        px[i * 4 + 0] = (uint8_t)(i * 7);
        px[i * 4 + 1] = (uint8_t)(i * 3);
        px[i * 4 + 2] = (uint8_t)(255 - i * 5);
        px[i * 4 + 3] = (uint8_t)((i % 3 == 0) ? 0 : (i % 3 == 1) ? 128 : 255);
    }
    EMB_REQUIRE(emb_gold_save("selftest-gold/roundtrip.bmp", px, 8, 4));

    int w = 0;
    int h = 0;
    uint8_t *back = emb_gold_load("selftest-gold/roundtrip.bmp", &w, &h);
    EMB_REQUIRE(back != nullptr);
    EMB_CHECK_EQ_INT(w, 8);
    EMB_CHECK_EQ_INT(h, 4);
    EMB_CHECK(SDL_memcmp(back, px, sizeof px) == 0);
    SDL_free(back);
}

EMB_TEST(gold_update_writes_then_matches)
{
    use_scratch_dirs();
    uint8_t px[16 * 16 * 4];
    make_gradient(px, 16, 16);

    SDL_setenv_unsafe("EMBER_UPDATE_GOLDENS", "1", 1);
    EMB_CHECK(emb_gold_check("selftest_update", px, 16, 16));
    SDL_unsetenv_unsafe("EMBER_UPDATE_GOLDENS");
    EMB_CHECK(file_exists("selftest-gold/selftest_update.bmp"));

    /* Same pixels now match the golden that was just written. */
    EMB_CHECK(emb_gold_check("selftest_update", px, 16, 16));

    /* One pixel off by 100 is a hard failure and must write both outputs. */
    uint8_t bad[16 * 16 * 4];
    SDL_memcpy(bad, px, sizeof bad);
    bad[(5 * 16 + 5) * 4 + 0] = (uint8_t)(bad[(5 * 16 + 5) * 4 + 0] + 100);
    EMB_CHECK(!emb_gold_check("selftest_update", bad, 16, 16));
    EMB_CHECK(file_exists("selftest-out/selftest_update.actual.bmp"));
    EMB_CHECK(file_exists("selftest-out/selftest_update.diff.bmp"));

    int dw = 0;
    int dh = 0;
    uint8_t *diff = emb_gold_load("selftest-out/selftest_update.diff.bmp", &dw, &dh);
    EMB_REQUIRE(diff != nullptr);
    EMB_CHECK_EQ_INT(dw, 16);
    EMB_CHECK_EQ_INT(dh, 16);
    EMB_CHECK_PIXEL(diff, 16, 5, 5, 255, 0, 0, 255);      /* hard mismatch: red */
    EMB_CHECK_PIXEL(diff, 16, 4, 5, px[(5 * 16 + 4) * 4] / 4, px[(5 * 16 + 4) * 4 + 1] / 4,
                    px[(5 * 16 + 4) * 4 + 2] / 4, 255); /* match: dimmed actual */
    SDL_free(diff);
}

EMB_TEST(gold_update_mode_overwrites_old_golden)
{
    use_scratch_dirs();
    uint8_t a[4 * 4 * 4];
    uint8_t b[4 * 4 * 4];
    make_gradient(a, 4, 4);
    SDL_memcpy(b, a, sizeof b);
    b[0] = (uint8_t)(b[0] + 50);

    SDL_setenv_unsafe("EMBER_UPDATE_GOLDENS", "1", 1);
    EMB_CHECK(emb_gold_check("selftest_overwrite", a, 4, 4));
    EMB_CHECK(emb_gold_check("selftest_overwrite", b, 4, 4));
    SDL_unsetenv_unsafe("EMBER_UPDATE_GOLDENS");
    EMB_CHECK(emb_gold_check("selftest_overwrite", b, 4, 4));
    EMB_CHECK(!emb_gold_check("selftest_overwrite", a, 4, 4));
}

EMB_TEST(gold_missing_golden_fails_and_writes_actual)
{
    use_scratch_dirs();
    uint8_t px[4 * 4 * 4];
    make_gradient(px, 4, 4);
    (void)SDL_RemovePath("selftest-gold/selftest_missing.bmp");
    (void)SDL_RemovePath("selftest-out/selftest_missing.actual.bmp");

    EMB_CHECK(!emb_gold_check("selftest_missing", px, 4, 4));
    EMB_CHECK(file_exists("selftest-out/selftest_missing.actual.bmp"));
    EMB_CHECK(!file_exists("selftest-gold/selftest_missing.bmp"));
}

EMB_TEST(gold_size_mismatch_fails)
{
    use_scratch_dirs();
    uint8_t small[4 * 4 * 4];
    uint8_t tall[4 * 5 * 4];
    make_gradient(small, 4, 4);
    make_gradient(tall, 4, 5);

    SDL_setenv_unsafe("EMBER_UPDATE_GOLDENS", "1", 1);
    EMB_CHECK(emb_gold_check("selftest_size", small, 4, 4));
    SDL_unsetenv_unsafe("EMBER_UPDATE_GOLDENS");
    EMB_CHECK(!emb_gold_check("selftest_size", tall, 4, 5));
}

EMB_TEST(gold_invalid_arguments_fail)
{
    use_scratch_dirs();
    uint8_t px[4 * 4 * 4];
    make_gradient(px, 4, 4);
    EMB_CHECK(!emb_gold_check(nullptr, px, 4, 4));
    EMB_CHECK(!emb_gold_check("selftest_invalid", nullptr, 4, 4));
    EMB_CHECK(!emb_gold_check("selftest_invalid", px, 0, 4));
}

EMB_TEST_MAIN()
