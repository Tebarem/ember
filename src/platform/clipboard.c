/*
 * The text clipboard (W5d). The system clipboard goes through SDL_GetClipboardText and
 * SDL_SetClipboardText, which need SDL's video subsystem (a window, or the offscreen or dummy driver). A process that
 * never started video (an offscreen embedder) gets a process-local buffer instead, so copy, cut, paste and
 * navigator.clipboard still work among its views; the buffer is not shared with other processes.
 */
#include "platform/platform.h"

#include "base/base.h"

#include <stdatomic.h>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static char *g_text;     /* the local clipboard (SDL_malloc'd, NUL-terminated), nullptr when empty */
static size_t g_len;

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

bool embi_pf_clipboard_native(void)
{
    return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

bool embi_pf_clipboard_get(embi_sb *out)
{
    if (embi_pf_clipboard_native()) {
        char *t = SDL_GetClipboardText();
        if (t == nullptr) {
            return false;
        }
        size_t n = SDL_strlen(t);
        if (n > 0) {
            embi_sb_append(out, (embi_str){t, (uint32_t)n});
        }
        SDL_free(t);
        return true;
    }
    lock();
    if (g_len > 0) {
        embi_sb_append(out, (embi_str){g_text, (uint32_t)g_len});
    }
    unlock();
    return true;
}

bool embi_pf_clipboard_set(embi_str text)
{
    char *copy = SDL_malloc((size_t)text.len + 1);
    if (copy == nullptr) {
        return false;
    }
    if (text.len > 0) {
        SDL_memcpy(copy, text.ptr, text.len);
    }
    copy[text.len] = '\0';
    if (embi_pf_clipboard_native()) {
        bool ok = SDL_SetClipboardText(copy);
        SDL_free(copy);
        return ok;
    }
    lock();
    char *old = g_text;
    g_text = copy;
    g_len = text.len;
    unlock();
    SDL_free(old);
    return true;
}
