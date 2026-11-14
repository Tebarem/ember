/*
 * Format dispatch and the cache around it: magic-byte sniffing (the URL never matters), the
 * embedder fallback for unknown bytes, size caps refusing oversized headers before allocation,
 * JPEG and GIF through embi_image_cache (natural size, orientation switch, eviction and
 * re-decoding), and FAILED versus UNSUPPORTED states. Every test checks that the live allocation
 * count returns to its baseline.
 */
#include "fmt_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

static const uint8_t kPngSig[8] = {137, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

EMB_TEST(sniff_uses_magic_bytes_only)
{
    size_t live0 = embi_mem_live();
    static const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    static const uint8_t gif87[] = "GIF87a..";
    static const uint8_t gif89[] = "GIF89a..";
    static const uint8_t bmp[] = "BM......";
    static const uint8_t webp[] = "RIFF....WEBP";
    EMB_CHECK(embi_image_sniff(kPngSig, sizeof kPngSig) == EMBI_IMAGE_FMT_PNG);
    EMB_CHECK(embi_image_sniff(jpg, sizeof jpg) == EMBI_IMAGE_FMT_JPEG);
    EMB_CHECK(embi_image_sniff(gif87, 6) == EMBI_IMAGE_FMT_GIF);
    EMB_CHECK(embi_image_sniff(gif89, 6) == EMBI_IMAGE_FMT_GIF);
    /* Too short to be sure: never a guess. */
    EMB_CHECK(embi_image_sniff(jpg, 2) == EMBI_IMAGE_FMT_OTHER);
    EMB_CHECK(embi_image_sniff(gif89, 5) == EMBI_IMAGE_FMT_OTHER);
    EMB_CHECK(embi_image_sniff(kPngSig, 7) == EMBI_IMAGE_FMT_OTHER);
    /* Other formats (not built in) are OTHER. */
    EMB_CHECK(embi_image_sniff(bmp, sizeof bmp - 1) == EMBI_IMAGE_FMT_OTHER);
    EMB_CHECK(embi_image_sniff(webp, sizeof webp - 1) == EMBI_IMAGE_FMT_OTHER);
    /* A JPEG SOI without the third marker byte is not a JPEG by this rule. */
    static const uint8_t soi_only[] = {0xFF, 0xD8, 0x00, 0x00};
    EMB_CHECK(embi_image_sniff(soi_only, sizeof soi_only) == EMBI_IMAGE_FMT_OTHER);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Counts calls and fills a 2x1 image, so the fallback path is visible. */
typedef struct counting_decoder {
    int calls;
    bool fail;
    uint32_t w;
    uint32_t h;
} counting_decoder;

static bool counting_fn(void *user, const uint8_t *in, size_t len, embi_pixels *out)
{
    counting_decoder *d = user;
    d->calls++;
    if (d->fail || len < 4 || in[0] != 'B') {
        return false;
    }
    out->w = d->w;
    out->h = d->h;
    out->px = embi_alloc((size_t)d->w * d->h * 4);
    for (size_t i = 0; i < (size_t)d->w * d->h * 4; i++) {
        out->px[i] = (uint8_t)i;
    }
    return true;
}

EMB_TEST(unknown_bytes_go_to_embedder_or_unsupported)
{
    size_t live0 = embi_mem_live();
    static const uint8_t bmp[] = "BM......";
    counting_decoder d = {.calls = 0, .fail = false, .w = 2, .h = 1};

    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(bmp, sizeof bmp - 1, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr,
                                 nullptr, &p));
    EMB_CHECK(p.px == nullptr);

    EMB_CHECK(embi_image_decode(bmp, sizeof bmp - 1, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true,
                                counting_fn, &d, &p));
    EMB_CHECK_EQ_INT(d.calls, 1);
    EMB_CHECK_EQ_INT(p.w, 2);
    EMB_CHECK_EQ_INT(p.h, 1);
    embi_pixels_free(&p);

    /* Built-in formats never reach the embedder, even when it is set. */
    size_t len = 0;
    uint8_t *jpg = fx_load("tests/image/jpg_1x1.jpg", &len);
    EMB_REQUIRE(jpg != nullptr);
    EMB_CHECK(embi_image_decode(jpg, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, counting_fn, &d,
                                &p));
    EMB_CHECK_EQ_INT(d.calls, 1);
    embi_pixels_free(&p);
    SDL_free(jpg);

    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* An embedder result over the area limit, or with no pixels, is refused and freed. */
EMB_TEST(embedder_results_are_validated)
{
    size_t live0 = embi_mem_live();
    static const uint8_t bmp[] = "BM......";
    counting_decoder big = {.calls = 0, .fail = false, .w = 100, .h = 100};
    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(bmp, sizeof bmp - 1, 99, true, counting_fn, &big, &p));
    EMB_CHECK(p.px == nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);

    counting_decoder bad = {.calls = 0, .fail = true, .w = 1, .h = 1};
    EMB_CHECK(!embi_image_decode(bmp, sizeof bmp - 1, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true,
                                 counting_fn, &bad, &p));
    EMB_CHECK_EQ_INT(bad.calls, 1);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* A JPEG whose SOF claims 65535 x 65535 is refused by the probe; the decoder never runs. The
 * allocation count stays put. */
EMB_TEST(jpeg_oversize_sof_refused_without_allocating)
{
    size_t live0 = embi_mem_live();
    static const uint8_t sof[] = {
        0xFF, 0xD8,                   /* SOI */
        0xFF, 0xC0, 0x00, 0x11,       /* SOF0, length 17 */
        0x08,                         /* precision */
        0xFF, 0xFF,                   /* height 65535 */
        0xFF, 0xFF,                   /* width 65535 */
        0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, /* components */
        0xFF, 0xD9,
    };
    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(sof, sizeof sof, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr,
                                 nullptr, &p));
    EMB_CHECK(p.px == nullptr);

    /* A SOF whose area is just over the limit (96x64 against 96*64-1), with the real fixture. */
    size_t len = 0;
    uint8_t *jpg = fx_load("tests/image/jpg_baseline_444.jpg", &len);
    EMB_REQUIRE(jpg != nullptr);
    EMB_CHECK(!embi_image_decode(jpg, len, 96 * 64 - 1, true, nullptr, nullptr, &p));
    EMB_CHECK(embi_image_decode(jpg, len, 96 * 64, true, nullptr, nullptr, &p));
    embi_pixels_free(&p);
    SDL_free(jpg);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Magic bytes beat the URL: PNG bytes under a .jpg name and JPEG bytes under a .png name are both
 * READY, and the natural size comes from the bytes. */
EMB_TEST(cache_magic_bytes_beat_url)
{
    size_t live0 = embi_mem_live();
    size_t jlen = 0;
    uint8_t *jpg = fx_load("tests/image/jpg_17x13.jpg", &jlen);
    size_t plen = 0;
    uint8_t *png = fx_load("tests/data/pngsuite/basn6a08.png", &plen);
    EMB_REQUIRE(jpg != nullptr);
    EMB_REQUIRE(png != nullptr);

    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("pic.png"), jpg, jlen) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("pic.png"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 17);
    EMB_CHECK_EQ_INT(h, 13);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("pic.jpg"), png, plen) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("pic.jpg"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 32);
    EMB_CHECK_EQ_INT(h, 32);
    SDL_free(png);
    embi_image_cache_destroy(c);
    SDL_free(jpg);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* JPEG and GIF through the cache: READY with the natural size and pixels; a JPEG with EXIF 6 is
 * oriented by default and stored-size with orientation off; a huge header goes to FAILED. */
EMB_TEST(cache_jpeg_and_gif_ready_sizes_and_orientation)
{
    size_t live0 = embi_mem_live();
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);

    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("a.gif"), gif, len) == EMBI_IMAGE_READY);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("a.gif"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 32);
    EMB_CHECK_EQ_INT(h, 24);
    const uint8_t *px = nullptr;
    EMB_CHECK(embi_image_cache_pixels(c, EMBI_STR("a.gif"), &px, &w, &h));
    EMB_CHECK(px != nullptr);
    SDL_free(gif);

    uint8_t *jpg = fx_load("tests/image/jpg_exif_o6.jpg", &len);
    EMB_REQUIRE(jpg != nullptr);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("o6.jpg"), jpg, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("o6.jpg"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 24);
    EMB_CHECK_EQ_INT(h, 40);
    SDL_free(jpg);

    /* Orientation off (image-orientation: none) for the next image: stored size. */
    embi_image_cache_set_apply_orientation(c, false);
    jpg = fx_load("tests/image/jpg_exif_o6.jpg", &len);
    EMB_REQUIRE(jpg != nullptr);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("raw6.jpg"), jpg, len) == EMBI_IMAGE_READY);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("raw6.jpg"), &w, &h) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(w, 40);
    EMB_CHECK_EQ_INT(h, 24);
    SDL_free(jpg);
    embi_image_cache_set_apply_orientation(c, true);

    /* A header whose SOF exceeds the limits is FAILED (the format is known), and keeps nothing. */
    static const uint8_t sof[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9};
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("huge.jpg"), sof, sizeof sof) ==
              EMBI_IMAGE_FAILED);
    EMB_CHECK(embi_image_cache_query(c, EMBI_STR("huge.jpg"), nullptr, nullptr) ==
              EMBI_IMAGE_FAILED);

    embi_image_cache_destroy(c);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* JPEG pixels are evicted under a zero budget and decoded again on demand, with the same oriented
 * size and the same pixels. */
EMB_TEST(cache_jpeg_redecodes_after_eviction)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *jpg = fx_load("tests/image/jpg_exif_o8.jpg", &len);
    EMB_REQUIRE(jpg != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("e.jpg"), jpg, len) == EMBI_IMAGE_READY);

    const uint8_t *px = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_image_cache_pixels(c, EMBI_STR("e.jpg"), &px, &w, &h));
    EMB_CHECK_EQ_INT(w, 24);
    EMB_CHECK_EQ_INT(h, 40);
    /* Keep a copy of the first pixel to compare after the re-decode. */
    uint8_t first[4] = {px[0], px[1], px[2], px[3]};
    size_t bytes_before = embi_image_cache_bytes(c);
    EMB_CHECK(bytes_before > 0);

    embi_image_cache_trim(c);
    embi_image_cache_set_budget(c, 0);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_encoded_bytes(c), len);

    EMB_CHECK(embi_image_cache_pixels(c, EMBI_STR("e.jpg"), &px, &w, &h));
    EMB_CHECK_EQ_INT(w, 24);
    EMB_CHECK_EQ_INT(h, 40);
    EMB_CHECK(px[0] == first[0] && px[1] == first[1] && px[2] == first[2] && px[3] == first[3]);

    embi_image_cache_destroy(c);
    SDL_free(jpg);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* The area limit set on the cache applies to JPEG and GIF as well as PNG. */
EMB_TEST(cache_pixel_cap_applies_to_all_formats)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_interlaced.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_image_cache_set_max_pixels(c, 96 * 64 - 1);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("g.gif"), gif, len) == EMBI_IMAGE_FAILED);
    embi_image_cache_set_max_pixels(c, 96 * 64);
    EMB_CHECK(embi_image_cache_finish(c, EMBI_STR("g2.gif"), gif, len) == EMBI_IMAGE_READY);
    embi_image_cache_destroy(c);
    SDL_free(gif);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
