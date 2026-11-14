/*
 * Image cache without a GPU: states (none, pending, ready, failed, unsupported), decoding through
 * the built-in PNG path and the embedder callback, the URL table, LRU trimming against the byte
 * budget (including the oversize exemption), the encoded-byte counter, forget and retry, re-decoding
 * after eviction, the pixel cap, and the upload refusal without a GPU hook.
 */
#include "png_suite_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/png.h"

/* basn6a08 is a 32 x 32 RGBA8 PngSuite image: 4096 decoded bytes. */
#define CACHE_IMG_BYTES (32u * 32u * 4u)

static uint8_t *load_basn(size_t *len)
{
    return ps_load("tests/data/pngsuite/basn6a08.png", len);
}

static bool fake_decoder(void *user, const uint8_t *in, size_t len, embi_pixels *out)
{
    (void)user;
    if (len < 4 || in[0] != 'B') {
        return false;
    }
    out->w = 2;
    out->h = 1;
    out->px = embi_alloc(8);
    for (size_t i = 0; i < 8; i++) {
        out->px[i] = (uint8_t)(in[1] + i);
    }
    return true;
}

EMB_TEST(cache_png_states_size_and_pixels)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    uint32_t pw = 0;
    uint32_t ph = 0;
    EMB_REQUIRE(embi_png_probe(png, len, &pw, &ph));

    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str url = EMBI_STR("img/a.png");
    uint32_t w = 99;
    uint32_t h = 99;
    EMB_CHECK(embi_image_cache_query(c, url, &w, &h) == EMBI_IMAGE_NONE);
    EMB_CHECK_EQ_INT(w, 0);
    EMB_CHECK_EQ_INT(h, 0);

    embi_image_cache_begin(c, url);
    EMB_CHECK(embi_image_cache_query(c, url, &w, &h) == EMBI_IMAGE_PENDING);

    EMB_CHECK(embi_image_cache_finish(c, url, png, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_query(c, url, &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, pw);
    EMB_CHECK_EQ_INT(h, ph);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), (size_t)pw * ph * 4);

    /* begin on a ready image does not reset it. */
    embi_image_cache_begin(c, url);
    EMB_CHECK(embi_image_cache_query(c, url, nullptr, nullptr) == EMBI_IMAGE_READY);

    const uint8_t *px = nullptr;
    uint32_t gw = 0;
    uint32_t gh = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &gw, &gh));
    EMB_CHECK_EQ_INT(gw, pw);
    EMB_CHECK_EQ_INT(gh, ph);
    embi_pixels ref = {0};
    EMB_REQUIRE(embi_png_decode(png, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &ref));
    EMB_CHECK(SDL_memcmp(px, ref.px, (size_t)pw * ph * 4) == 0);
    embi_pixels_free(&ref);

    /* Unknown URLs and a missing GPU hook refuse cleanly. */
    EMB_CHECK(!embi_image_cache_pixels(c, EMBI_STR("img/none.png"), &px, &gw, &gh));
    uint64_t handle = 0;
    EMB_CHECK(!embi_image_cache_upload(c, url, nullptr, &handle));

    embi_image_cache_destroy(c);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_failed_and_fail_marks_are_terminal)
{
    size_t live0 = embi_mem_live();
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    /* A PNG signature followed by junk: a known format that fails to decode. */
    static const uint8_t junk[] = {137, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 'j', 'u', 'n', 'k'};

    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("bad.png"), junk, sizeof junk) == EMBI_IMAGE_FAILED);
    uint32_t w = 5;
    uint32_t h = 5;
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("bad.png"), &w, &h) == EMBI_IMAGE_FAILED);
    EMB_CHECK_EQ_INT(w, 0);
    EMB_CHECK_EQ_INT(h, 0);

    embi_image_cache_begin(c, EMBI_STR("bad.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("bad.png"), nullptr, nullptr) == EMBI_IMAGE_FAILED);

    embi_image_cache_fail(c, EMBI_STR("gone.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("gone.png"), nullptr, nullptr) == EMBI_IMAGE_FAILED);

    /* GIF is a built-in format, so a GIF header with bad bytes is FAILED, not UNSUPPORTED. */
    static const uint8_t gif[] = "GIF89a....";
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a.gif"), gif, sizeof gif - 1) == EMBI_IMAGE_FAILED);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("a.gif"), &w, &h) == EMBI_IMAGE_FAILED);

    /* Formats we do not know are unsupported without a callback (BMP is not built in). */
    static const uint8_t bmp[] = "BM......";
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a.bmp"), bmp, sizeof bmp - 1) == EMBI_IMAGE_UNSUPPORTED);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("a.bmp"), &w, &h) == EMBI_IMAGE_UNSUPPORTED);

    embi_image_cache_destroy(c);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_embedder_decoder_handles_other_formats)
{
    size_t live0 = embi_mem_live();
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_image_cache_set_decoder(c, fake_decoder, nullptr);

    static const uint8_t good[] = {'B', 10, 0, 0};
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("x.fmt"), good, sizeof good) == EMBI_IMAGE_READY);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("x.fmt"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 2);
    EMB_CHECK_EQ_INT(h, 1);
    const uint8_t *px = nullptr;
    uint32_t gw = 0;
    uint32_t gh = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, EMBI_STR("x.fmt"), &px, &gw, &gh));
    EMB_CHECK_EQ_INT(px[0], 10);
    EMB_CHECK_EQ_INT(px[7], 17);

    static const uint8_t other[] = {'Z', 0, 0, 0};
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("y.fmt"), other, sizeof other) == EMBI_IMAGE_FAILED);

    embi_image_cache_destroy(c);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_lru_trim_evicts_least_recent_and_redecodes)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);

    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("b"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("c"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 3 * (size_t)CACHE_IMG_BYTES);

    /* Touch a: now b is the least recently used. */
    const uint8_t *px = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, EMBI_STR("a"), &px, &w, &h));

    embi_image_cache_set_budget(c, 2 * (size_t)CACHE_IMG_BYTES);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 2 * (size_t)CACHE_IMG_BYTES);

    /* The evicted image keeps its size and comes back on demand, then the next trim settles. */
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("b"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 32);
    EMB_REQUIRE(embi_image_cache_pixels(c, EMBI_STR("b"), &px, &w, &h));
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 3 * (size_t)CACHE_IMG_BYTES);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 2 * (size_t)CACHE_IMG_BYTES);

    /* Budget zero drops every decoded image, and the cache still serves them. */
    embi_image_cache_set_budget(c, 0);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("c"), nullptr, nullptr) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_pixels(c, EMBI_STR("c"), &px, &w, &h));

    embi_image_cache_destroy(c);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

static int g_upload_calls;
static int g_release_calls;

static uint64_t fake_upload(void *user, const uint8_t *px, uint32_t w, uint32_t h,
                            SDL_GPUCommandBuffer *cmd)
{
    (void)user;
    (void)cmd;
    if (px == nullptr || w == 0 || h == 0) {
        return 0;
    }
    g_upload_calls++;
    return 1000u + (uint64_t)g_upload_calls;
}

static void fake_release(void *user, uint64_t handle)
{
    (void)user;
    (void)handle;
    g_release_calls++;
}

EMB_TEST(cache_gpu_hook_uploads_once_and_releases_on_eviction)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    g_upload_calls = 0;
    g_release_calls = 0;

    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("b"), png, len) == EMBI_IMAGE_READY);
    embi_image_gpu hook = {nullptr, fake_upload, fake_release};
    embi_image_cache_set_gpu(c, &hook);

    uint64_t h1 = 0;
    uint64_t h2 = 0;
    uint64_t h3 = 0;
    EMB_CHECK(!embi_image_cache_upload(c, EMBI_STR("missing"), nullptr, &h3));
    EMB_REQUIRE(embi_image_cache_upload(c, EMBI_STR("a"), nullptr, &h1));
    EMB_REQUIRE(embi_image_cache_upload(c, EMBI_STR("a"), nullptr, &h2));
    EMB_CHECK_EQ_INT(h1, h2);
    EMB_CHECK_EQ_INT(g_upload_calls, 1);

    /* The first trim only marks the frame boundary: a is an oversize image for a zero budget and
     * was used since the cache was made, so it stays. The next trim evicts it and its handle. */
    embi_image_cache_trim(c);
    embi_image_cache_set_budget(c, 0);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(g_release_calls, 1);

    /* Uploading again after eviction makes a fresh handle. */
    EMB_REQUIRE(embi_image_cache_upload(c, EMBI_STR("a"), nullptr, &h3));
    EMB_CHECK_EQ_INT(g_upload_calls, 2);
    EMB_CHECK(h3 != h1);

    embi_image_cache_destroy(c);
    EMB_CHECK_EQ_INT(g_release_calls, 2);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_pixel_cap_refuses_large_images)
{
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_image_cache_set_max_pixels(c, 100); /* the image has 1024 pixels */
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("big"), png, len) == EMBI_IMAGE_FAILED);
    embi_image_cache_set_max_pixels(c, EMBI_IMAGE_DEFAULT_MAX_PIXELS);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("big"), png, len) == EMBI_IMAGE_READY);
    embi_image_cache_destroy(c);
    SDL_free(png);
}

EMB_TEST(cache_forget_allows_retry_after_failure)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);

    embi_image_cache_fail(c, EMBI_STR("net.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("net.png"), nullptr, nullptr) == EMBI_IMAGE_FAILED);
    /* A failed url stays failed until it is forgotten. */
    embi_image_cache_begin(c, EMBI_STR("net.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("net.png"), nullptr, nullptr) == EMBI_IMAGE_FAILED);
    embi_image_cache_forget(c, EMBI_STR("net.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("net.png"), nullptr, nullptr) == EMBI_IMAGE_NONE);
    embi_image_cache_begin(c, EMBI_STR("net.png"));
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("net.png"), png, len) == EMBI_IMAGE_READY);

    /* Forgetting a ready image releases its bytes and pixels; forgetting an unknown url is a no-op. */
    embi_image_cache_forget(c, EMBI_STR("net.png"));
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("net.png"), nullptr, nullptr) == EMBI_IMAGE_NONE);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_encoded_bytes(c), 0);
    embi_image_cache_forget(c, EMBI_STR("never.png"));

    embi_image_cache_destroy(c);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_encoded_bytes_are_counted_outside_the_budget)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("b"), png, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_encoded_bytes(c), 2 * len);

    /* A refused body keeps no encoded bytes. */
    static const uint8_t junk[] = "junk";
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("j"), junk, sizeof junk - 1) == EMBI_IMAGE_UNSUPPORTED);
    EMB_CHECK_EQ_INT(embi_image_cache_encoded_bytes(c), 2 * len);

    /* Trimming drops pixels only: the encoded bytes stay so the image can be decoded again. The
     * first trim marks the frame boundary, so the images used before it are evictable by the next. */
    embi_image_cache_trim(c);
    embi_image_cache_set_budget(c, 0);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_encoded_bytes(c), 2 * len);
    const uint8_t *px = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_image_cache_pixels(c, EMBI_STR("a"), &px, &w, &h));
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), (size_t)CACHE_IMG_BYTES);

    embi_image_cache_destroy(c);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(cache_trim_keeps_oversize_image_used_between_trims)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *png = load_basn(&len);
    EMB_REQUIRE(png != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_image_cache_set_budget(c, 1000); /* smaller than the 4096-byte image */
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("big"), png, len) == EMBI_IMAGE_READY);

    /* Used since the cache was made: kept across the first trim. */
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), (size_t)CACHE_IMG_BYTES);

    /* Used again between trims: kept, and not decoded again. */
    const uint8_t *px = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, EMBI_STR("big"), &px, &w, &h));
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), (size_t)CACHE_IMG_BYTES);

    /* Not used since the previous trim: evicted, and it comes back on demand. */
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_REQUIRE(embi_image_cache_pixels(c, EMBI_STR("big"), &px, &w, &h));
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), (size_t)CACHE_IMG_BYTES);

    embi_image_cache_destroy(c);
    SDL_free(png);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
