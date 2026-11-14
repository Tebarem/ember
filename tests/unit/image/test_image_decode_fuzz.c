/*
 * Seeded mutation fuzz over the JPEG and GIF fixtures. Each iteration takes a fixture, applies one
 * mutation (bit flips, truncation, header-field corruption, byte insertion or overwrite), and
 * decodes it through embi_image_decode, then runs the header probes and the EXIF walk on the same
 * bytes. Every decode must return cleanly, any image it returns must have sane sides and pixels,
 * and the live allocation count must return to its baseline.
 *
 * Iterations per fixture: EMBER_IMAGE_FUZZ_ITERS if set (for the longer local runs), else 200
 * (60 under EMB_TEST_ASAN). The seed is fixed so a failure reproduces. Run under the ASan build
 * with the environment variable set higher for a longer pass.
 */
#include "fmt_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

#include <SDL3/SDL_stdinc.h>

#define FUZZ_SEED 0x5EED1234u
#define FUZZ_DEFAULT_ITERS 200
#define FUZZ_ASAN_ITERS 60
#define FUZZ_MAX_PIXELS ((uint64_t)1 << 22)
#define FUZZ_MAX_LEN ((size_t)1 << 20)

static const char *kFuzzFixtures[] = {
    "jpg_baseline_420", "jpg_baseline_444", "jpg_progressive", "jpg_gray", "jpg_cmyk_adobe",
    "jpg_restart",      "jpg_1x1",          "jpg_17x13",       "jpg_exif_o1", "jpg_exif_o6",
    "jpg_exif_o8",      "gif87a",           "gif89_interlaced", "gif89_transparent",
    "gif89_odd_17x13",  "gif89_anim",       "gif89_local_ct",
};

typedef struct fz_src {
    uint8_t *bytes;
    size_t len;
} fz_src;

static uint32_t fz_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static size_t fz_below(uint32_t *s, size_t n)
{
    return n ? (size_t)(fz_next(s) % n) : 0;
}

static int fz_iters(void)
{
    const char *env = SDL_getenv("EMBER_IMAGE_FUZZ_ITERS");
    if (env && *env) {
        long v = SDL_strtol(env, nullptr, 10);
        if (v > 0 && v < 100000000L) {
            return (int)v;
        }
    }
    return EMB_TEST_ASAN ? FUZZ_ASAN_ITERS : FUZZ_DEFAULT_ITERS;
}

/* Applies one mutation to buf (capacity cap, current length *len). */
static void fz_mutate(uint32_t *rng, uint8_t *buf, size_t cap, size_t *len)
{
    switch (fz_next(rng) % 5) {
    case 0: { /* 1-8 bit flips anywhere */
        size_t flips = 1 + fz_below(rng, 8);
        for (size_t i = 0; i < flips && *len > 0; i++) {
            buf[fz_below(rng, *len)] ^= (uint8_t)(1u << fz_below(rng, 8));
        }
        break;
    }
    case 1: /* truncation, possibly to nothing */
        *len = fz_below(rng, *len + 1);
        break;
    case 2: { /* header-field corruption: 2 bytes in the first 64, set to 0xFF or random */
        size_t span = *len < 64 ? *len : 64;
        if (span >= 2) {
            size_t at = fz_below(rng, span - 1);
            uint8_t v = (fz_next(rng) & 1) ? 0xFF : (uint8_t)fz_next(rng);
            buf[at] = v;
            buf[at + 1] = (fz_next(rng) & 1) ? 0xFF : (uint8_t)fz_next(rng);
        }
        break;
    }
    case 3: { /* insert 1-16 random bytes */
        size_t n = 1 + fz_below(rng, 16);
        if (*len + n > cap) {
            break;
        }
        size_t at = fz_below(rng, *len + 1);
        SDL_memmove(buf + at + n, buf + at, *len - at);
        for (size_t i = 0; i < n; i++) {
            buf[at + i] = (uint8_t)fz_next(rng);
        }
        *len += n;
        break;
    }
    default: { /* overwrite 1-16 random bytes */
        size_t n = 1 + fz_below(rng, 16);
        if (*len == 0) {
            break;
        }
        size_t at = fz_below(rng, *len);
        for (size_t i = 0; i < n && at + i < *len; i++) {
            buf[at + i] = (uint8_t)fz_next(rng);
        }
        break;
    }
    }
}

EMB_TEST(fuzz_fixtures_mutations_decode_cleanly)
{
    size_t live0 = embi_mem_live();
    const size_t nfix = SDL_arraysize(kFuzzFixtures);
    fz_src src[SDL_arraysize(kFuzzFixtures)];
    for (size_t i = 0; i < nfix; i++) {
        char rel[256];
        const char *name = kFuzzFixtures[i];
        bool is_gif = name[0] == 'g';
        SDL_snprintf(rel, sizeof rel, "tests/image/%s.%s", name, is_gif ? "gif" : "jpg");
        src[i].bytes = fx_load(rel, &src[i].len);
        EMB_REQUIRE(src[i].bytes != nullptr);
    }

    const int iters = fz_iters();
    uint32_t rng = FUZZ_SEED;
    uint8_t *buf = embi_alloc(FUZZ_MAX_LEN);
    size_t decoded = 0;
    size_t refused = 0;
    size_t probed = 0;
    for (size_t i = 0; i < nfix; i++) {
        for (int it = 0; it < iters; it++) {
            size_t len = src[i].len;
            SDL_memcpy(buf, src[i].bytes, len);
            /* Up to three stacked mutations per input. */
            int rounds = 1 + (int)fz_below(&rng, 3);
            for (int r = 0; r < rounds; r++) {
                fz_mutate(&rng, buf, FUZZ_MAX_LEN, &len);
            }

            bool apply = (it & 1) != 0;
            embi_pixels p = {0};
            if (embi_image_decode(buf, len, FUZZ_MAX_PIXELS, apply, nullptr, nullptr, &p)) {
                EMB_CHECK(p.px != nullptr);
                EMB_CHECK(p.w > 0 && p.h > 0 && p.w <= EMBI_IMAGE_MAX_DIM &&
                          p.h <= EMBI_IMAGE_MAX_DIM);
                EMB_CHECK((uint64_t)p.w * p.h <= FUZZ_MAX_PIXELS);
                /* Touch the first and last pixel so an undersized buffer shows under ASan. */
                volatile uint8_t sink = p.px[0];
                sink ^= p.px[(size_t)p.w * p.h * 4 - 1];
                (void)sink;
                embi_pixels_free(&p);
                decoded++;
            } else {
                refused++;
            }

            /* The header and EXIF walks on the same bytes. */
            uint32_t w = 0;
            uint32_t h = 0;
            if (embi_jpeg_probe(buf, len, FUZZ_MAX_PIXELS, &w, &h) ||
                embi_gif_probe(buf, len, FUZZ_MAX_PIXELS, &w, &h)) {
                probed++;
            }
            (void)embi_jpeg_orientation(buf, len);
            (void)embi_image_sniff(buf, len);
        }
    }
    embi_free(buf, FUZZ_MAX_LEN);
    for (size_t i = 0; i < nfix; i++) {
        SDL_free(src[i].bytes);
    }

    SDL_Log("image fuzz: %llu fixtures x %d iterations: %llu decoded, %llu refused, %llu headers ok",
            (unsigned long long)nfix, iters, (unsigned long long)decoded, (unsigned long long)refused,
            (unsigned long long)probed);
    EMB_CHECK(decoded > 0);
    EMB_CHECK(refused > 0);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
