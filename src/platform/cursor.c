/*
 * CSS cursor keywords to SDL system cursors (W5d). The style engine reads the cursor
 * property of the element under the pointer (W5d2) and the window sets the cursor with SDL_SetCursor (window.c). Only the
 * system cursors exist: a url() cursor falls back to the keyword after it, which the caller resolves.
 */
#include "platform/platform.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>

typedef struct cursor_map {
    const char *name;
    int32_t cursor;
} cursor_map;

int32_t embi_pf_cursor_from_css(embi_str kw)
{
    static const cursor_map map[] = {
        {"pointer", SDL_SYSTEM_CURSOR_POINTER},
        {"text", SDL_SYSTEM_CURSOR_TEXT},
        {"vertical-text", SDL_SYSTEM_CURSOR_TEXT},
        {"wait", SDL_SYSTEM_CURSOR_WAIT},
        {"progress", SDL_SYSTEM_CURSOR_PROGRESS},
        {"crosshair", SDL_SYSTEM_CURSOR_CROSSHAIR},
        {"cell", SDL_SYSTEM_CURSOR_CROSSHAIR},
        {"move", SDL_SYSTEM_CURSOR_MOVE},
        {"all-scroll", SDL_SYSTEM_CURSOR_MOVE},
        {"grab", SDL_SYSTEM_CURSOR_MOVE},
        {"grabbing", SDL_SYSTEM_CURSOR_MOVE},
        {"not-allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED},
        {"no-drop", SDL_SYSTEM_CURSOR_NOT_ALLOWED},
        {"col-resize", SDL_SYSTEM_CURSOR_EW_RESIZE},
        {"ew-resize", SDL_SYSTEM_CURSOR_EW_RESIZE},
        {"row-resize", SDL_SYSTEM_CURSOR_NS_RESIZE},
        {"ns-resize", SDL_SYSTEM_CURSOR_NS_RESIZE},
        {"nesw-resize", SDL_SYSTEM_CURSOR_NESW_RESIZE},
        {"nwse-resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE},
        {"n-resize", SDL_SYSTEM_CURSOR_N_RESIZE},
        {"s-resize", SDL_SYSTEM_CURSOR_S_RESIZE},
        {"e-resize", SDL_SYSTEM_CURSOR_E_RESIZE},
        {"w-resize", SDL_SYSTEM_CURSOR_W_RESIZE},
        {"ne-resize", SDL_SYSTEM_CURSOR_NE_RESIZE},
        {"nw-resize", SDL_SYSTEM_CURSOR_NW_RESIZE},
        {"se-resize", SDL_SYSTEM_CURSOR_SE_RESIZE},
        {"sw-resize", SDL_SYSTEM_CURSOR_SW_RESIZE},
    };
    char buf[24];
    if (kw.len == 0 || kw.len >= sizeof buf) {
        return -1;
    }
    for (uint32_t i = 0; i < kw.len; i++) {
        char c = kw.ptr[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[kw.len] = '\0';
    if (SDL_strcmp(buf, "none") == 0) {
        return -2;
    }
    for (uint32_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (SDL_strcmp(buf, map[i].name) == 0) {
            return map[i].cursor;
        }
    }
    return -1;
}
