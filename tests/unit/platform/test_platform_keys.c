/*
 * SDL keys to DOM key values, codes and locations (W5d): the case and the US shifted symbols, the named keys, the
 * function keys, the scancode codes, and the modifier locations.
 */
#include "emb_test.h"

#include "platform/platform.h"

#include <string.h>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

static bool key_is(SDL_Keycode k, uint16_t mods, const char *want)
{
    char buf[32];
    embi_pf_key_name(k, mods, buf, sizeof buf);
    return strcmp(buf, want) == 0;
}

static bool code_is(SDL_Scancode sc, const char *want)
{
    char buf[32];
    embi_pf_code_name(sc, buf, sizeof buf);
    return strcmp(buf, want) == 0;
}

EMB_TEST(key_values_follow_shift)
{
    EMB_CHECK(key_is(((SDL_Keycode)'a'), 0, "a"));
    EMB_CHECK(key_is(((SDL_Keycode)'a'), SDL_KMOD_SHIFT, "A"));
    EMB_CHECK(key_is(((SDL_Keycode)'1'), SDL_KMOD_SHIFT, "!"));
    EMB_CHECK(key_is(((SDL_Keycode)'1'), 0, "1"));
    EMB_CHECK(key_is(((SDL_Keycode)'/'), SDL_KMOD_SHIFT, "?"));
    EMB_CHECK(key_is(SDLK_SPACE, 0, " "));
    EMB_CHECK(key_is(SDLK_RETURN, 0, "Enter"));
    EMB_CHECK(key_is(SDLK_TAB, SDL_KMOD_SHIFT, "Tab"));
    EMB_CHECK(key_is(SDLK_BACKSPACE, 0, "Backspace"));
    EMB_CHECK(key_is(SDLK_LEFT, 0, "ArrowLeft"));
    EMB_CHECK(key_is(SDLK_F5, 0, "F5"));
    EMB_CHECK(key_is(SDLK_F12, 0, "F12"));
    EMB_CHECK(key_is(SDLK_LSHIFT, 0, "Shift"));
    EMB_CHECK(key_is(SDLK_UNKNOWN, 0, "Unidentified"));
}

EMB_TEST(codes_and_locations)
{
    EMB_CHECK(code_is(SDL_SCANCODE_A, "KeyA"));
    EMB_CHECK(code_is(SDL_SCANCODE_Z, "KeyZ"));
    EMB_CHECK(code_is(SDL_SCANCODE_1, "Digit1"));
    EMB_CHECK(code_is(SDL_SCANCODE_0, "Digit0"));
    EMB_CHECK(code_is(SDL_SCANCODE_RETURN, "Enter"));
    EMB_CHECK(code_is(SDL_SCANCODE_LSHIFT, "ShiftLeft"));
    EMB_CHECK(code_is(SDL_SCANCODE_KP_1, "Numpad1"));
    EMB_CHECK(code_is(SDL_SCANCODE_UNKNOWN, ""));

    EMB_CHECK_EQ_INT(embi_pf_key_location(SDL_SCANCODE_LSHIFT), 1);
    EMB_CHECK_EQ_INT(embi_pf_key_location(SDL_SCANCODE_RCTRL), 2);
    EMB_CHECK_EQ_INT(embi_pf_key_location(SDL_SCANCODE_KP_5), 3);
    EMB_CHECK_EQ_INT(embi_pf_key_location(SDL_SCANCODE_A), 0);
}

EMB_TEST(small_buffers_truncate_and_terminate)
{
    char buf[3];
    uint32_t n = embi_pf_key_name(SDLK_LEFT, 0, buf, sizeof buf); /* "ArrowLeft" does not fit */
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(strlen(buf) == 2);
}

EMB_TEST_MAIN()
