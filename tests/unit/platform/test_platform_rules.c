/*
 * The input rules of the platform layer (W5d) that the other platform tests do not cover: hidden inputs and invalid
 * tabindex values (focus), maxlength and minlength values that HTML ignores (state), and the names of the keys beyond
 * ASCII and of the keypad and the high function keys (keys).
 */
#include "emb_test.h"

#include "dom/dom.h"
#include "platform/platform.h"
#include "pf_test.h"

#include <string.h>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

EMB_TEST(hidden_input_is_never_focusable)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *hidden = pf_el(doc, &body->base, "input");
    pf_attr(hidden, "type", "HIDDEN");
    embi_element *hidden_tab = pf_el(doc, &body->base, "input");
    pf_attr(hidden_tab, "type", "hidden");
    pf_attr(hidden_tab, "tabindex", "0");
    embi_element *text = pf_el(doc, &body->base, "input");
    pf_attr(text, "type", "text");
    EMB_CHECK(!embi_pf_focusable(hidden));
    EMB_CHECK(!embi_pf_focusable(hidden_tab));
    EMB_CHECK(embi_pf_focusable(text));
    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, false) == text);
    embi_document_free(doc);
}

EMB_TEST(invalid_tabindex_counts_as_absent)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *empty = pf_el(doc, &body->base, "div");
    pf_attr(empty, "tabindex", "");
    embi_element *word = pf_el(doc, &body->base, "div");
    pf_attr(word, "tabindex", "abc");
    embi_element *btn = pf_el(doc, &body->base, "button");
    pf_attr(btn, "tabindex", "x");
    embi_element *spaced = pf_el(doc, &body->base, "div");
    pf_attr(spaced, "tabindex", "  3");
    embi_element *plus = pf_el(doc, &body->base, "div");
    pf_attr(plus, "tabindex", "+4x");

    EMB_CHECK(!embi_pf_focusable(empty));
    EMB_CHECK(!embi_pf_focusable(word));
    EMB_CHECK(embi_pf_tabindex(empty) == -1);
    EMB_CHECK(embi_pf_tabindex(word) == -1);
    /* A button keeps its default: focusable and in the Tab order at 0, the invalid attribute notwithstanding. */
    EMB_CHECK(embi_pf_focusable(btn));
    EMB_CHECK(embi_pf_tabindex(btn) == 0);
    /* Leading ASCII whitespace is skipped, and the digits after a sign are read. */
    EMB_CHECK(embi_pf_tabindex(spaced) == 3);
    EMB_CHECK(embi_pf_tabindex(plus) == 4);
    EMB_CHECK(embi_pf_focusable(spaced));
    embi_document_free(doc);
}

EMB_TEST(maxlength_ignores_invalid_values)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *neg = pf_el(doc, &body->base, "input");
    pf_attr(neg, "maxlength", "-1");
    pf_attr(neg, "value", "abcdef");
    embi_element *huge = pf_el(doc, &body->base, "input");
    pf_attr(huge, "maxlength", "1e20");
    pf_attr(huge, "value", "abcdef");
    embi_element *ok = pf_el(doc, &body->base, "input");
    pf_attr(ok, "maxlength", "2");
    pf_attr(ok, "value", "abcdef");
    embi_element *min = pf_el(doc, &body->base, "input");
    pf_attr(min, "minlength", "3x");
    pf_attr(min, "value", "a");
    EMB_CHECK(embi_pf_control_valid(neg));  /* ignored: no limit */
    EMB_CHECK(embi_pf_control_valid(huge)); /* ignored: no limit */
    EMB_CHECK(!embi_pf_control_valid(ok));  /* a real limit of 2 */
    EMB_CHECK(embi_pf_control_valid(min));  /* "3x" is not a number: no minimum */
    embi_document_free(doc);
}

EMB_TEST(key_names_beyond_ascii_and_keypad)
{
    char buf[32];
    /* The printable non-ASCII keys: the code point, UTF-8 encoded. */
    EMB_CHECK(embi_pf_key_name(0xE9, 0, buf, sizeof buf) == 2);
    EMB_CHECK(strcmp(buf, "\xC3\xA9") == 0);
    EMB_CHECK(embi_pf_key_name(0x44F, 0, buf, sizeof buf) == 2); /* Cyrillic small letter ya is U+044F */
    EMB_CHECK(strcmp(buf, "\xD1\x8F") == 0);
    EMB_CHECK(embi_pf_key_name(0x4E2D, 0, buf, sizeof buf) == 3); /* a CJK character */
    EMB_CHECK(strcmp(buf, "\xE4\xB8\xAD") == 0);
    /* The keypad digits and operators have their DOM values. */
    EMB_CHECK(embi_pf_key_name(SDLK_KP_1, SDL_KMOD_NUM, buf, sizeof buf) == 1 && strcmp(buf, "1") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_PLUS, 0, buf, sizeof buf) == 1 && strcmp(buf, "+") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_PLUS, 0, buf, sizeof buf) == 1 && strcmp(buf, "+") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_F13, 0, buf, sizeof buf) == 3 && strcmp(buf, "F13") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_F24, 0, buf, sizeof buf) == 3 && strcmp(buf, "F24") == 0);
    /* Codes. */
    EMB_CHECK(embi_pf_code_name(SDL_SCANCODE_NONUSBACKSLASH, buf, sizeof buf) > 0 &&
              strcmp(buf, "IntlBackslash") == 0);
    EMB_CHECK(embi_pf_code_name(SDL_SCANCODE_APPLICATION, buf, sizeof buf) > 0 && strcmp(buf, "ContextMenu") == 0);
    EMB_CHECK(embi_pf_code_name(SDL_SCANCODE_KP_PLUS, buf, sizeof buf) > 0 && strcmp(buf, "NumpadAdd") == 0);
    EMB_CHECK(embi_pf_code_name(SDL_SCANCODE_F13, buf, sizeof buf) > 0 && strcmp(buf, "F13") == 0);
    EMB_CHECK(embi_pf_code_name(SDL_SCANCODE_PAUSE, buf, sizeof buf) > 0 && strcmp(buf, "Pause") == 0);
    /* A surrogate code point is not a character. */
    EMB_CHECK(embi_pf_key_name(0xD800, 0, buf, sizeof buf) == 12 && strcmp(buf, "Unidentified") == 0);
}

EMB_TEST(keypad_digits_follow_numlock)
{
    char buf[32];
    /* With NumLock off the keypad digits are the navigation keys printed on them. */
    EMB_CHECK(embi_pf_key_name(SDLK_KP_1, 0, buf, sizeof buf) > 0 && strcmp(buf, "End") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_8, 0, buf, sizeof buf) > 0 && strcmp(buf, "ArrowUp") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_0, 0, buf, sizeof buf) > 0 && strcmp(buf, "Insert") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_PERIOD, 0, buf, sizeof buf) > 0 && strcmp(buf, "Delete") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_5, SDL_KMOD_NUM, buf, sizeof buf) == 1 && strcmp(buf, "5") == 0);
    EMB_CHECK(embi_pf_key_name(SDLK_KP_PERIOD, SDL_KMOD_NUM, buf, sizeof buf) == 1 && strcmp(buf, ".") == 0);
}

EMB_TEST(key_names_follow_the_layout_and_capslock)
{
    char buf[32];
    /* Unshifted, Shift, CapsLock and both: the case of a letter is Shift xor CapsLock. */
    EMB_CHECK(embi_pf_key_name_layout('a', 'a', 0, buf, sizeof buf) == 1 && strcmp(buf, "a") == 0);
    EMB_CHECK(embi_pf_key_name_layout('a', 'a', SDL_KMOD_SHIFT, buf, sizeof buf) == 1 && strcmp(buf, "A") == 0);
    EMB_CHECK(embi_pf_key_name_layout('a', 'a', SDL_KMOD_CAPS, buf, sizeof buf) == 1 && strcmp(buf, "A") == 0);
    EMB_CHECK(embi_pf_key_name_layout('a', 'A', SDL_KMOD_SHIFT | SDL_KMOD_CAPS, buf, sizeof buf) == 1 &&
              strcmp(buf, "a") == 0);
    /* The layout's own character is used as it is: a German Shift+7 is "/", a French unshifted 1 key is "&". */
    EMB_CHECK(embi_pf_key_name_layout('7', '/', SDL_KMOD_SHIFT, buf, sizeof buf) == 1 && strcmp(buf, "/") == 0);
    EMB_CHECK(embi_pf_key_name_layout('1', '&', 0, buf, sizeof buf) == 1 && strcmp(buf, "&") == 0);
    EMB_CHECK(embi_pf_key_name_layout('2', '"', SDL_KMOD_SHIFT, buf, sizeof buf) == 1 && strcmp(buf, "\"") == 0);
    /* A non-ASCII layout character is UTF-8 encoded; a letter of another script keeps its keymap case. */
    EMB_CHECK(embi_pf_key_name_layout(0x44F, 0x44F, 0, buf, sizeof buf) == 2 && strcmp(buf, "\xD1\x8F") == 0);
    /* A non-character key is named by the event's keycode. */
    EMB_CHECK(embi_pf_key_name_layout(SDLK_RETURN, SDLK_RETURN, 0, buf, sizeof buf) == 5 && strcmp(buf, "Enter") == 0);
    EMB_CHECK(embi_pf_key_name_layout(SDLK_LEFT, SDLK_LEFT, SDL_KMOD_SHIFT, buf, sizeof buf) > 0 &&
              strcmp(buf, "ArrowLeft") == 0);
}

EMB_TEST_MAIN()
