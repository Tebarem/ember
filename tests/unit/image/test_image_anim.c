/*
 * Animated GIF playback through the frame clock (W6c, image.h embi_image_cache_tick): the frame follows the delays on a
 * fake clock, a change reports the URL and drops the GPU copy, a long pause skips whole plays, a finite loop count stops
 * on the last frame, and a still GIF never animates. tests/image/gif89_anim.gif has three frames of 100 ms that loop
 * forever.
 */
#include "fmt_util.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

typedef struct hook_state {
    uint32_t uploads;
    uint32_t releases;
} hook_state;

static uint64_t hook_upload(void *user, const uint8_t *px, uint32_t w, uint32_t h, SDL_GPUCommandBuffer *cmd)
{
    (void)px;
    (void)w;
    (void)h;
    (void)cmd;
    hook_state *s = user;
    s->uploads++;
    return 100u + s->uploads;
}

static void hook_release(void *user, uint64_t handle)
{
    (void)handle;
    ((hook_state *)user)->releases++;
}

typedef struct note {
    uint32_t n;
    char last[64];
} note;

static void on_frame(void *user, embi_str url)
{
    note *t = user;
    t->n++;
    SDL_snprintf(t->last, sizeof t->last, "%.*s", (int)url.len, url.ptr);
}

static uint32_t first_word(const uint8_t *px)
{
    uint32_t v;
    SDL_memcpy(&v, px, 4);
    return v;
}

EMB_TEST(gif_animates_on_the_frame_clock)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    hook_state hs = {0};
    embi_image_gpu hook = {&hs, hook_upload, hook_release};
    embi_image_cache_set_gpu(c, &hook);
    embi_str url = EMBI_STR("a.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    SDL_free(gif);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, url), 3);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 0);
    EMB_CHECK(embi_image_cache_anim_bytes(c) > 0);

    note nt = {0};
    uint64_t handle = 0;
    EMB_REQUIRE(embi_image_cache_upload(c, url, nullptr, &handle));
    EMB_CHECK_EQ_INT(hs.uploads, 1);
    const uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    uint32_t f0 = first_word(px);

    /* The first tick starts the clock; nothing changes before the delay is over. */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1000.0, on_frame, &nt), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1050.0, on_frame, &nt), 0);
    EMB_CHECK_EQ_INT(nt.n, 0);
    EMB_CHECK(embi_image_cache_animating(c, 1050.0));

    /* Frame 1 at 100 ms: the URL is reported, the GPU copy of frame 0 is gone and the next upload makes a new one. */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1100.0, on_frame, &nt), 1);
    EMB_CHECK_EQ_INT(nt.n, 1);
    EMB_CHECK_EQ_STR(nt.last, "a.gif");
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 1);
    EMB_CHECK_EQ_INT(hs.releases, 1);
    EMB_REQUIRE(embi_image_cache_upload(c, url, nullptr, &handle));
    EMB_CHECK_EQ_INT(hs.uploads, 2);

    /* A tick with no change reports nothing and does not drop the GPU copy. */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1150.0, on_frame, &nt), 0);
    EMB_CHECK_EQ_INT(hs.releases, 1);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1200.0, on_frame, &nt), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 2);
    /* Frame 2 ends at 300 ms: the loop is forever, so it wraps to frame 0. */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 1300.0, on_frame, &nt), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 0);
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    EMB_CHECK_EQ_INT(first_word(px), f0);

    /* A long pause lands on the right frame without walking: 5 s + 150 ms later is 150 ms into a play (1300 + 17 plays + 150) (frame 1). */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 6550.0, on_frame, &nt), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 1);

    /* Drawn lately: wants frames. Not drawn for a long time: it stops asking. */
    EMB_REQUIRE(embi_image_cache_upload(c, url, nullptr, &handle));
    EMB_CHECK(embi_image_cache_animating(c, 6560.0));
    EMB_CHECK(!embi_image_cache_animating(c, 6550.0 + 5000.0));

    embi_image_cache_destroy(c);
    EMB_CHECK_EQ_INT(hs.uploads, hs.releases); /* every GPU copy was released */
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(gif_frames_differ)
{
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str url = EMBI_STR("a.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    SDL_free(gif);
    const uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    size_t n = (size_t)w * h * 4;
    uint8_t *first = SDL_malloc(n);
    SDL_memcpy(first, px, n);
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 100.0, nullptr, nullptr), 1);
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    EMB_CHECK(SDL_memcmp(first, px, n) != 0);
    SDL_free(first);
    embi_image_cache_destroy(c);
}

EMB_TEST(evicted_frame_comes_back_as_the_current_one)
{
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(1); /* every trim evicts */
    embi_str url = EMBI_STR("a.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    SDL_free(gif);
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 100.0, nullptr, nullptr), 1);
    const uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    uint32_t f1 = first_word(px);
    embi_image_cache_trim(c);
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_bytes(c), 0);
    EMB_REQUIRE(embi_image_cache_pixels(c, url, &px, &w, &h));
    EMB_CHECK_EQ_INT(first_word(px), f1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 1);
    embi_image_cache_destroy(c);
}

EMB_TEST(still_gif_never_animates)
{
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif87a.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str url = EMBI_STR("s.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    SDL_free(gif);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, url), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_anim_bytes(c), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 0.0, nullptr, nullptr), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 10000.0, nullptr, nullptr), 0);
    EMB_CHECK(!embi_image_cache_animating(c, 10000.0));
    embi_image_cache_destroy(c);
}

EMB_TEST(finite_loop_count_stops_on_the_last_frame)
{
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    /* NETSCAPE2.0 block: the loop count is two bytes after "NETSCAPE2.0" 03 01. */
    size_t at = 0;
    for (; at + 16 < len; at++) {
        if (SDL_memcmp(gif + at, "NETSCAPE2.0", 11) == 0) {
            break;
        }
    }
    EMB_REQUIRE(at + 16 < len);
    gif[at + 13] = 1; /* NETSCAPE counts repeats after the first play: 1 is two plays */
    gif[at + 14] = 0;
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str url = EMBI_STR("l.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, url), 3);
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    /* Walk: 3 frames x 100 ms per play, two plays. */
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 100.0, nullptr, nullptr), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 200.0, nullptr, nullptr), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 300.0, nullptr, nullptr), 1); /* wraps to 0 for the second play */
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 0);
    EMB_CHECK(embi_image_cache_animating(c, 300.0));
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 400.0, nullptr, nullptr), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 500.0, nullptr, nullptr), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 2);
    /* The second play ends: the last frame stays and the image stops asking for frames. */
    (void)embi_image_cache_tick(c, 600.0, nullptr, nullptr);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 2);
    EMB_CHECK(!embi_image_cache_animating(c, 600.0));
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 5000.0, nullptr, nullptr), 0);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 2);
    embi_image_cache_destroy(c);

    /* A long pause lands on the last frame at once. */
    c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    EMB_REQUIRE(embi_image_cache_finish(c, url, gif, len) == EMBI_IMAGE_READY);
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 60000.0, nullptr, nullptr), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, url), 2);
    EMB_CHECK(!embi_image_cache_animating(c, 60000.0));
    EMB_CHECK_EQ_INT(embi_image_cache_tick(c, 120000.0, nullptr, nullptr), 0);
    embi_image_cache_destroy(c);
    SDL_free(gif);
}

EMB_TEST(frame_memory_is_capped_across_the_cache)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str a = EMBI_STR("a.gif"), b = EMBI_STR("b.gif"), d = EMBI_STR("c.gif"), e = EMBI_STR("d.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, a, gif, len) == EMBI_IMAGE_READY);
    size_t one = embi_image_cache_anim_bytes(c);
    EMB_REQUIRE(one > 0);
    embi_image_cache_destroy(c);

    /* Room for two images' frames: the third and fourth are READY stills. */
    c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_image_cache_set_anim_caps(c, one, one * 2 + one / 2);
    EMB_REQUIRE(embi_image_cache_finish(c, a, gif, len) == EMBI_IMAGE_READY);
    EMB_REQUIRE(embi_image_cache_finish(c, b, gif, len) == EMBI_IMAGE_READY);
    EMB_REQUIRE(embi_image_cache_finish(c, d, gif, len) == EMBI_IMAGE_READY);
    EMB_REQUIRE(embi_image_cache_finish(c, e, gif, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_anim_bytes(c), one * 2);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, a), 3);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, b), 3);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, d), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, e), 1);
    EMB_CHECK(embi_image_cache_anim_bytes(c) <= one * 2 + one / 2);

    /* A per-image cap under one image's frames makes it a still. */
    embi_image_cache_set_anim_caps(c, one - 1, one * 10);
    embi_str f = EMBI_STR("e.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, f, gif, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, f), 1);

    /* Trim drops the frames of images not drawn lately; the stills stay. b is drawn, a is not. */
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    (void)embi_image_cache_tick(c, 10000.0, nullptr, nullptr);
    const uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, b, &px, &w, &h));
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, a), 1);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, b), 3);
    EMB_CHECK_EQ_INT(embi_image_cache_anim_bytes(c), one);
    EMB_REQUIRE(embi_image_cache_pixels(c, a, &px, &w, &h));
    embi_image_cache_destroy(c);
    SDL_free(gif);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST(trimmed_frames_decode_again_when_drawn)
{
    size_t live0 = embi_mem_live();
    size_t len = 0;
    uint8_t *gif = fx_load("tests/image/gif89_anim.gif", &len);
    EMB_REQUIRE(gif != nullptr);
    embi_image_cache *c = embi_image_cache_create(EMBI_IMAGE_DEFAULT_BUDGET);
    embi_str u = EMBI_STR("r.gif");
    EMB_REQUIRE(embi_image_cache_finish(c, u, gif, len) == EMBI_IMAGE_READY);
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, u), 3);
    (void)embi_image_cache_tick(c, 0.0, nullptr, nullptr);
    (void)embi_image_cache_tick(c, 10000.0, nullptr, nullptr); /* off screen for ten seconds */
    embi_image_cache_trim(c);
    EMB_CHECK_EQ_INT(embi_image_cache_anim_bytes(c), 0);
    EMB_CHECK(!embi_image_cache_animating(c, 10000.0));

    /* A draw brings the frames back and the clock restarts. */
    const uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    EMB_REQUIRE(embi_image_cache_pixels(c, u, &px, &w, &h));
    EMB_CHECK_EQ_INT(embi_image_cache_frame_count(c, u), 3);
    EMB_CHECK(embi_image_cache_anim_bytes(c) > 0);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, u), 0);
    EMB_CHECK(embi_image_cache_animating(c, 10000.0));
    note t = {0};
    (void)embi_image_cache_tick(c, 10000.0, on_frame, &t); /* starts the clock */
    (void)embi_image_cache_tick(c, 10150.0, on_frame, &t);
    EMB_CHECK_EQ_INT(embi_image_cache_frame(c, u), 1);
    EMB_CHECK(t.n >= 1);

    /* A still never takes this path. */
    embi_image_cache_destroy(c);
    SDL_free(gif);
    EMB_CHECK_EQ_INT(embi_mem_live(), live0);
}

EMB_TEST_MAIN()
