/*
 * GIF through SDL_image's GIF decoder: the first frame of each fixture matches the ImageMagick
 * reference exactly (every channel; for transparent pixels only alpha, since the color under full
 * transparency is undefined). Covers GIF87a, interlaced, transparent, odd size, a local color
 * table, animation (first frame only), and truncation. Header caps refuse oversized images before
 * any decoding. Every test checks that the live allocation count returns to its baseline.
 */
#include "fmt_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

typedef struct gif_case {
    const char *name;   /* tests/image/<name>.gif and tests/image/ref/<name>.png */
    bool transparent;   /* color is undefined where the reference alpha is 0 */
} gif_case;

static const gif_case kGifFixtures[] = {
    {"gif87a", false},
    {"gif89_interlaced", false},
    {"gif89_transparent", true},
    {"gif89_odd_17x13", false},
    {"gif89_anim", false},
    {"gif89_local_ct", false},
};

static bool check_gif(const gif_case *c)
{
    char rel[256];
    SDL_snprintf(rel, sizeof rel, "tests/image/%s.gif", c->name);
    size_t len = 0;
    uint8_t *bytes = fx_load(rel, &len);
    if (!bytes) {
        return false;
    }
    embi_pixels got = {0};
    bool ok = embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr,
                                &got);
    SDL_free(bytes);
    if (!ok) {
        SDL_Log("gif %s: decode refused", c->name);
        return false;
    }
    embi_pixels ref = {0};
    if (!fx_ref(c->name, &ref)) {
        embi_pixels_free(&got);
        return false;
    }
    fx_diff d;
    bool same = fx_compare(&got, &ref, c->transparent, &d);
    SDL_Log("gif %-18s %ux%u max_err=%d over3=%llu of %llu channels", c->name, got.w, got.h,
            d.max_err, (unsigned long long)d.over3, (unsigned long long)d.channels);
    bool exact = same && d.max_err == 0;
    embi_pixels_free(&got);
    embi_pixels_free(&ref);
    return exact;
}

EMB_TEST(gif_fixtures_match_reference_exactly)
{
    size_t live0 = embi_mem_live();
    for (size_t i = 0; i < SDL_arraysize(kGifFixtures); i++) {
        EMB_CHECK(check_gif(&kGifFixtures[i]));
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(gif_sniff_and_probe_sizes)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(bytes != nullptr);
    EMB_CHECK(embi_image_sniff(bytes, len) == EMBI_IMAGE_FMT_GIF);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_gif_probe(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK_EQ_INT(w, 32);
    EMB_CHECK_EQ_INT(h, 24);
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Truncated files: the first 150 bytes cut through the first frame's data. The result may be a
 * refusal or a partial frame (SDL_image's GIF reader pads); it must return cleanly and keep the
 * allocation count. */
EMB_TEST(gif_truncated_does_not_crash)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/gif_truncated.gif", &len);
    EMB_REQUIRE(bytes != nullptr);
    embi_pixels p = {0};
    if (embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p)) {
        EMB_CHECK_EQ_INT(p.w, 32);
        EMB_CHECK_EQ_INT(p.h, 24);
        embi_pixels_free(&p);
    }
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Builds a GIF89a header with a logical screen and one image descriptor of the given sizes, with
 * no color tables and a trailer. Returns the length. */
static size_t make_gif_header(uint8_t *b, uint16_t screen_w, uint16_t screen_h, uint16_t img_w,
                              uint16_t img_h)
{
    size_t n = 0;
    SDL_memcpy(b, "GIF89a", 6);
    n = 6;
    b[n++] = (uint8_t)screen_w;
    b[n++] = (uint8_t)(screen_w >> 8);
    b[n++] = (uint8_t)screen_h;
    b[n++] = (uint8_t)(screen_h >> 8);
    b[n++] = 0x00; /* no global color table */
    b[n++] = 0;
    b[n++] = 0;
    b[n++] = 0x2C;
    b[n++] = 0;
    b[n++] = 0; /* left */
    b[n++] = 0;
    b[n++] = 0; /* top */
    b[n++] = (uint8_t)img_w;
    b[n++] = (uint8_t)(img_w >> 8);
    b[n++] = (uint8_t)img_h;
    b[n++] = (uint8_t)(img_h >> 8);
    b[n++] = 0x00; /* no local color table, not interlaced */
    b[n++] = 0x02; /* LZW minimum code size */
    b[n++] = 0x00; /* no data sub-blocks: refused by the header walk only if sizes fail */
    b[n++] = 0x3B;
    return n;
}

/* Headers claiming huge logical screens or image descriptors are refused by the probe before
 * SDL_image is called, so nothing is allocated. */
EMB_TEST(gif_oversize_headers_refused_without_allocating)
{
    size_t live0 = embi_mem_live();
    uint8_t b[64];

    size_t n = make_gif_header(b, 65535, 65535, 4, 4);
    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(b, n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p));
    EMB_CHECK(p.px == nullptr);

    /* Screen within the side limit but over the area limit (16384 x 16384 is 2^28 pixels). */
    n = make_gif_header(b, 16384, 16384, 4, 4);
    EMB_CHECK(!embi_image_decode(b, n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p));

    /* Small screen, huge first frame: the frame buffer is the cost, so the probe checks it too. */
    n = make_gif_header(b, 8, 8, 65535, 65535);
    EMB_CHECK(!embi_image_decode(b, n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p));

    /* A zero-sized screen is refused: SDL_image cannot create the canvas. */
    n = make_gif_header(b, 0, 0, 4, 4);
    EMB_CHECK(!embi_image_decode(b, n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p));

    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* The area limit applies to GIFs too: the 96x64 fixture is refused when the limit is 96 * 63. */
EMB_TEST(gif_area_limit_is_enforced)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/gif89_interlaced.gif", &len);
    EMB_REQUIRE(bytes != nullptr);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_gif_probe(bytes, len, 96 * 64, &w, &h));
    EMB_CHECK(!embi_gif_probe(bytes, len, 96 * 64 - 1, &w, &h));
    EMB_CHECK_EQ_INT(w, 0);
    EMB_CHECK_EQ_INT(h, 0);
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* Regression: a fuzz-found GIF (tests/image/gif_sdl_fallback.gif, from test_image_decode_fuzz) whose
 * header SDL_image's GIF animation decoder refuses. SDL_image then falls back to a single-frame
 * decoder that calls back into its GIF loader on the same stream, which recursed until the stack
 * overflowed. The decode must now refuse it cleanly, without allocating pixels or recursing. */
EMB_TEST(gif_sdl_refused_header_does_not_recurse)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/gif_sdl_fallback.gif", &len);
    EMB_REQUIRE(bytes != nullptr);
    embi_pixels p = {0};
    EMB_CHECK(!embi_image_decode(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &p));
    EMB_CHECK(p.px == nullptr);
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

/* The probe mirrors SDL_image's header rules, so headers SDL_image refuses (whose fallback leaks
 * and recurses) never reach it. The fuzzed fixture is one of them, as is a NETSCAPE2.0 block with
 * more than one sub-block. One sub-block is fine. */
EMB_TEST(gif_probe_refuses_headers_sdl_image_refuses)
{
    size_t len = 0;
    uint8_t *bytes = fx_load("tests/image/gif_sdl_fallback.gif", &len);
    EMB_REQUIRE(bytes != nullptr);
    uint32_t w = 0, h = 0;
    EMB_CHECK(!embi_gif_probe(bytes, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    SDL_free(bytes);

    /* GIF89a, 1x1, no global color table, NETSCAPE2.0 app block, sub-blocks, image descriptor. */
    static const uint8_t head[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x00, 0, 0,
                                   0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2',
                                   '.', '0'};
    static const uint8_t desc[] = {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0};
    uint8_t buf[64];
    size_t n = 0;
    SDL_memcpy(buf, head, sizeof head);
    n = sizeof head;
    static const uint8_t one[] = {3, 1, 0, 0, 0}; /* loop forever, then the terminator */
    SDL_memcpy(buf + n, one, sizeof one);
    SDL_memcpy(buf + n + sizeof one, desc, sizeof desc);
    EMB_CHECK(embi_gif_probe(buf, n + sizeof one + sizeof desc, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK_EQ_INT(w, 1);

    static const uint8_t two[] = {3, 1, 0, 0, 2, 7, 7, 0}; /* a second sub-block: SDL_image refuses */
    SDL_memcpy(buf + n, two, sizeof two);
    SDL_memcpy(buf + n + sizeof two, desc, sizeof desc);
    EMB_CHECK(!embi_gif_probe(buf, n + sizeof two + sizeof desc, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
}

EMB_TEST_MAIN()
