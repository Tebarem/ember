/*
 * The text clipboard (W5d, platform/clipboard.c): the process-local buffer used while SDL's video subsystem is not
 * initialized, and SDL's own clipboard once it is (the offscreen video driver gives a working clipboard headless; the test
 * skips with 77 when it cannot be started). Memory returns to its baseline.
 */
#include "emb_test.h"

#include "platform/platform.h"

#include <string.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

static bool get_text(embi_sb *sb, const char *want)
{
    sb->len = 0;
    return embi_pf_clipboard_get(sb) && sb->len == strlen(want) && (sb->len == 0 || memcmp(sb->data, want, sb->len) == 0);
}

EMB_TEST(local_buffer_round_trip_without_video)
{
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        EMB_SKIP("the video subsystem is already up");
    }
    size_t before = embi_mem_live();
    EMB_CHECK(!embi_pf_clipboard_native());
    embi_sb sb = {0};
    EMB_CHECK(get_text(&sb, "")); /* empty at first */
    EMB_CHECK(embi_pf_clipboard_set(EMBI_STR("hello \xC3\xA9")));
    EMB_CHECK(get_text(&sb, "hello \xC3\xA9"));
    EMB_CHECK(embi_pf_clipboard_set(EMBI_STR("")));
    EMB_CHECK(get_text(&sb, ""));
    EMB_CHECK(embi_pf_clipboard_set(EMBI_STR("kept")));
    embi_sb_free(&sb);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
}

EMB_TEST(sdl_clipboard_round_trip)
{
    (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        EMB_SKIP("SDL video (the offscreen driver) is not available");
    }
    EMB_CHECK(embi_pf_clipboard_native());
    embi_sb sb = {0};
    if (!embi_pf_clipboard_set(EMBI_STR("via sdl \xE4\xB8\xAD"))) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        EMB_SKIP("the SDL clipboard is not available");
    }
    EMB_CHECK(get_text(&sb, "via sdl \xE4\xB8\xAD"));
    EMB_CHECK(embi_pf_clipboard_set(EMBI_STR("second")));
    EMB_CHECK(get_text(&sb, "second"));
    embi_sb_free(&sb);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    EMB_CHECK(!embi_pf_clipboard_native());
}

EMB_TEST_MAIN()
