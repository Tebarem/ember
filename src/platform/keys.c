/*
 * SDL keys to DOM key values and codes (W5d). The tables cover the keys a page sees: the
 * printable ASCII keys (with the US shifted symbols), the control keys, the navigation keys, the modifiers, the function
 * keys and the keypad. Other keys are "Unidentified" with an empty code.
 */
#include "platform/platform.h"

#include "base/base.h"

#include <string.h>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

static uint32_t put(const char *s, char *buf, uint32_t cap)
{
    uint32_t n = (uint32_t)strlen(s);
    if (cap == 0) {
        return 0;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    return n;
}

/* The US layout's shifted symbol of a printable ASCII key, or 0 when the key has none. */
static char shifted(char c)
{
    static const char plain[] = "1234567890-=[]\\;',./`";
    static const char shift[] = "!@#$%^&*()_+{}|:\"<>?~";
    for (uint32_t i = 0; plain[i] != '\0'; i++) {
        if (plain[i] == c) {
            return shift[i];
        }
    }
    return 0;
}


/* A keypad digit: the digit with NumLock on (SDL_KMOD_NUM in mods), else the navigation key printed on the keycap. */
static uint32_t keypad_digit(SDL_Keycode key, uint16_t mods, char *buf, uint32_t cap)
{
    static const char *const nav[10] = {"Insert", "End", "ArrowDown", "PageDown", "ArrowLeft",
                                        "Clear",  "ArrowRight", "Home", "ArrowUp", "PageUp"};
    uint32_t d = (uint32_t)(key == SDLK_KP_0 ? 0 : key - SDLK_KP_1 + 1);
    if ((mods & SDL_KMOD_NUM) != 0) {
        char tmp[2] = {(char)('0' + d), '\0'};
        return put(tmp, buf, cap);
    }
    return put(nav[d], buf, cap);
}

uint32_t embi_pf_key_name(SDL_Keycode key, uint16_t mods, char *buf, uint32_t cap)
{
    bool shift = (mods & SDL_KMOD_SHIFT) != 0;
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return put("Enter", buf, cap);
    case SDLK_TAB:
        return put("Tab", buf, cap);
    case SDLK_BACKSPACE:
        return put("Backspace", buf, cap);
    case SDLK_ESCAPE:
        return put("Escape", buf, cap);
    case SDLK_DELETE:
        return put("Delete", buf, cap);
    case SDLK_SPACE:
        return put(" ", buf, cap);
    case SDLK_LEFT:
        return put("ArrowLeft", buf, cap);
    case SDLK_RIGHT:
        return put("ArrowRight", buf, cap);
    case SDLK_UP:
        return put("ArrowUp", buf, cap);
    case SDLK_DOWN:
        return put("ArrowDown", buf, cap);
    case SDLK_HOME:
        return put("Home", buf, cap);
    case SDLK_END:
        return put("End", buf, cap);
    case SDLK_PAGEUP:
        return put("PageUp", buf, cap);
    case SDLK_PAGEDOWN:
        return put("PageDown", buf, cap);
    case SDLK_INSERT:
        return put("Insert", buf, cap);
    case SDLK_CAPSLOCK:
        return put("CapsLock", buf, cap);
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return put("Shift", buf, cap);
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return put("Control", buf, cap);
    case SDLK_LALT:
    case SDLK_RALT:
        return put("Alt", buf, cap);
    case SDLK_LGUI:
    case SDLK_RGUI:
        return put("Meta", buf, cap);
    case SDLK_KP_0:
    case SDLK_KP_1:
    case SDLK_KP_2:
    case SDLK_KP_3:
    case SDLK_KP_4:
    case SDLK_KP_5:
    case SDLK_KP_6:
    case SDLK_KP_7:
    case SDLK_KP_8:
    case SDLK_KP_9:
        return keypad_digit(key, mods, buf, cap);
    case SDLK_KP_PERIOD:
        return put((mods & SDL_KMOD_NUM) != 0 ? "." : "Delete", buf, cap);
    case SDLK_KP_PLUS:
        return put("+", buf, cap);
    case SDLK_KP_MINUS:
        return put("-", buf, cap);
    case SDLK_KP_MULTIPLY:
        return put("*", buf, cap);
    case SDLK_KP_DIVIDE:
        return put("/", buf, cap);
    case SDLK_KP_EQUALS:
        return put("=", buf, cap);
    case SDLK_KP_COMMA:
        return put(",", buf, cap);
    case SDLK_PRINTSCREEN:
        return put("PrintScreen", buf, cap);
    case SDLK_SCROLLLOCK:
        return put("ScrollLock", buf, cap);
    case SDLK_PAUSE:
        return put("Pause", buf, cap);
    case SDLK_NUMLOCKCLEAR:
        return put("NumLock", buf, cap);
    case SDLK_APPLICATION:
        return put("ContextMenu", buf, cap);
    default:
        break;
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        char tmp[4];
        SDL_snprintf(tmp, sizeof tmp, "F%d", (int)(key - SDLK_F1 + 1));
        return put(tmp, buf, cap);
    }
    static const SDL_Keycode f_high[] = {SDLK_F13, SDLK_F14, SDLK_F15, SDLK_F16, SDLK_F17, SDLK_F18,
                                         SDLK_F19, SDLK_F20, SDLK_F21, SDLK_F22, SDLK_F23, SDLK_F24};
    for (uint32_t i = 0; i < sizeof f_high / sizeof f_high[0]; i++) {
        if (key == f_high[i]) {
            char tmp[4];
            SDL_snprintf(tmp, sizeof tmp, "F%u", (unsigned)(13 + i));
            return put(tmp, buf, cap);
        }
    }
    if (key >= 0x20 && key < 0x7f) {
        char c = (char)key;
        if (c >= 'a' && c <= 'z') {
            c = shift ? (char)(c - 'a' + 'A') : c;
        } else if (shift && shifted(c) != 0) {
            c = shifted(c);
        }
        char tmp[2] = {c, '\0'};
        return put(tmp, buf, cap);
    }
    /* A printable character outside ASCII: SDL uses its code point as the keycode (the special keys carry the scancode
     * mask). Encoded as UTF-8; surrogates and code points past U+10FFFF are not characters. */
    if (key >= 0x80 && key < 0x110000 && (key & SDLK_SCANCODE_MASK) == 0 && (key < 0xD800 || key > 0xDFFF)) {
        char tmp[5];
        uint32_t cp = (uint32_t)key;
        uint32_t n = 0;
        if (cp < 0x800) {
            tmp[n++] = (char)(0xC0 | (cp >> 6));
            tmp[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            tmp[n++] = (char)(0xE0 | (cp >> 12));
            tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            tmp[n++] = (char)(0xF0 | (cp >> 18));
            tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = (char)(0x80 | (cp & 0x3F));
        }
        tmp[n] = '\0';
        return put(tmp, buf, cap);
    }
    return put("Unidentified", buf, cap);
}

/* UTF-8 encoding of a code point (cp is a scalar value). Returns the byte count; out holds at least 5 bytes. */
static uint32_t encode_cp(uint32_t cp, char *out)
{
    uint32_t n = 0;
    if (cp < 0x80) {
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xF0 | (cp >> 18));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
    return n;
}

uint32_t embi_pf_key_name_layout(SDL_Keycode base, SDL_Keycode resolved, uint16_t mods, char *buf, uint32_t cap)
{
    bool printable = resolved >= 0x20 && resolved != 0x7f && resolved < 0x110000 && (resolved & SDLK_SCANCODE_MASK) == 0 &&
                     (resolved < 0xD800 || resolved > 0xDFFF);
    if (!printable) {
        return embi_pf_key_name(base, mods, buf, cap);
    }
    uint32_t cp = (uint32_t)resolved;
    /* The layout already applied Shift and AltGr to the character, and CapsLock to the letters it covers; the case of an
     * ASCII letter is settled here whichever case the keymap stored. Other scripts keep the keymap's character. */
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
        bool upper = ((mods & SDL_KMOD_SHIFT) != 0) != ((mods & SDL_KMOD_CAPS) != 0);
        cp = (cp >= 'a' && cp <= 'z') ? (upper ? cp - 32 : cp) : (upper ? cp : cp + 32);
    }
    char tmp[5];
    (void)encode_cp(cp, tmp);
    return put(tmp, buf, cap);
}

uint32_t embi_pf_code_name(SDL_Scancode sc, char *buf, uint32_t cap)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char tmp[8];
        SDL_snprintf(tmp, sizeof tmp, "Key%c", (char)('A' + (sc - SDL_SCANCODE_A)));
        return put(tmp, buf, cap);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        char tmp[8];
        SDL_snprintf(tmp, sizeof tmp, "Digit%c", (char)('1' + (sc - SDL_SCANCODE_1)));
        return put(tmp, buf, cap);
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        char tmp[8];
        SDL_snprintf(tmp, sizeof tmp, "F%d", (int)(sc - SDL_SCANCODE_F1 + 1));
        return put(tmp, buf, cap);
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        char tmp[10];
        SDL_snprintf(tmp, sizeof tmp, "Numpad%c", (char)('1' + (sc - SDL_SCANCODE_KP_1)));
        return put(tmp, buf, cap);
    }
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) {
        char tmp[8];
        SDL_snprintf(tmp, sizeof tmp, "F%d", (int)(sc - SDL_SCANCODE_F13 + 13));
        return put(tmp, buf, cap);
    }
    switch (sc) {
    case SDL_SCANCODE_0:
        return put("Digit0", buf, cap);
    case SDL_SCANCODE_NONUSBACKSLASH:
        return put("IntlBackslash", buf, cap);
    case SDL_SCANCODE_APPLICATION:
        return put("ContextMenu", buf, cap);
    case SDL_SCANCODE_PRINTSCREEN:
        return put("PrintScreen", buf, cap);
    case SDL_SCANCODE_SCROLLLOCK:
        return put("ScrollLock", buf, cap);
    case SDL_SCANCODE_PAUSE:
        return put("Pause", buf, cap);
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return put("NumLock", buf, cap);
    case SDL_SCANCODE_KP_PLUS:
        return put("NumpadAdd", buf, cap);
    case SDL_SCANCODE_KP_MINUS:
        return put("NumpadSubtract", buf, cap);
    case SDL_SCANCODE_KP_MULTIPLY:
        return put("NumpadMultiply", buf, cap);
    case SDL_SCANCODE_KP_DIVIDE:
        return put("NumpadDivide", buf, cap);
    case SDL_SCANCODE_KP_PERIOD:
        return put("NumpadDecimal", buf, cap);
    case SDL_SCANCODE_KP_EQUALS:
        return put("NumpadEqual", buf, cap);
    case SDL_SCANCODE_KP_COMMA:
        return put("NumpadComma", buf, cap);
    case SDL_SCANCODE_KP_0:
        return put("Numpad0", buf, cap);
    case SDL_SCANCODE_RETURN:
        return put("Enter", buf, cap);
    case SDL_SCANCODE_KP_ENTER:
        return put("NumpadEnter", buf, cap);
    case SDL_SCANCODE_ESCAPE:
        return put("Escape", buf, cap);
    case SDL_SCANCODE_BACKSPACE:
        return put("Backspace", buf, cap);
    case SDL_SCANCODE_TAB:
        return put("Tab", buf, cap);
    case SDL_SCANCODE_SPACE:
        return put("Space", buf, cap);
    case SDL_SCANCODE_MINUS:
        return put("Minus", buf, cap);
    case SDL_SCANCODE_EQUALS:
        return put("Equal", buf, cap);
    case SDL_SCANCODE_LEFTBRACKET:
        return put("BracketLeft", buf, cap);
    case SDL_SCANCODE_RIGHTBRACKET:
        return put("BracketRight", buf, cap);
    case SDL_SCANCODE_BACKSLASH:
        return put("Backslash", buf, cap);
    case SDL_SCANCODE_SEMICOLON:
        return put("Semicolon", buf, cap);
    case SDL_SCANCODE_APOSTROPHE:
        return put("Quote", buf, cap);
    case SDL_SCANCODE_GRAVE:
        return put("Backquote", buf, cap);
    case SDL_SCANCODE_COMMA:
        return put("Comma", buf, cap);
    case SDL_SCANCODE_PERIOD:
        return put("Period", buf, cap);
    case SDL_SCANCODE_SLASH:
        return put("Slash", buf, cap);
    case SDL_SCANCODE_CAPSLOCK:
        return put("CapsLock", buf, cap);
    case SDL_SCANCODE_RIGHT:
        return put("ArrowRight", buf, cap);
    case SDL_SCANCODE_LEFT:
        return put("ArrowLeft", buf, cap);
    case SDL_SCANCODE_DOWN:
        return put("ArrowDown", buf, cap);
    case SDL_SCANCODE_UP:
        return put("ArrowUp", buf, cap);
    case SDL_SCANCODE_HOME:
        return put("Home", buf, cap);
    case SDL_SCANCODE_END:
        return put("End", buf, cap);
    case SDL_SCANCODE_PAGEUP:
        return put("PageUp", buf, cap);
    case SDL_SCANCODE_PAGEDOWN:
        return put("PageDown", buf, cap);
    case SDL_SCANCODE_DELETE:
        return put("Delete", buf, cap);
    case SDL_SCANCODE_INSERT:
        return put("Insert", buf, cap);
    case SDL_SCANCODE_LSHIFT:
        return put("ShiftLeft", buf, cap);
    case SDL_SCANCODE_RSHIFT:
        return put("ShiftRight", buf, cap);
    case SDL_SCANCODE_LCTRL:
        return put("ControlLeft", buf, cap);
    case SDL_SCANCODE_RCTRL:
        return put("ControlRight", buf, cap);
    case SDL_SCANCODE_LALT:
        return put("AltLeft", buf, cap);
    case SDL_SCANCODE_RALT:
        return put("AltRight", buf, cap);
    case SDL_SCANCODE_LGUI:
        return put("MetaLeft", buf, cap);
    case SDL_SCANCODE_RGUI:
        return put("MetaRight", buf, cap);
    default:
        return put("", buf, cap);
    }
}

uint8_t embi_pf_key_location(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_LGUI:
        return 1;
    case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_RALT:
    case SDL_SCANCODE_RGUI:
        return 2;
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_KP_0:
    case SDL_SCANCODE_KP_1:
    case SDL_SCANCODE_KP_2:
    case SDL_SCANCODE_KP_3:
    case SDL_SCANCODE_KP_4:
    case SDL_SCANCODE_KP_5:
    case SDL_SCANCODE_KP_6:
    case SDL_SCANCODE_KP_7:
    case SDL_SCANCODE_KP_8:
    case SDL_SCANCODE_KP_9:
    case SDL_SCANCODE_KP_PERIOD:
    case SDL_SCANCODE_KP_PLUS:
    case SDL_SCANCODE_KP_MINUS:
    case SDL_SCANCODE_KP_MULTIPLY:
    case SDL_SCANCODE_KP_DIVIDE:
        return 3;
    default:
        return 0;
    }
}
