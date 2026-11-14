/*
 * The curated UA stylesheet (src/style/ua/html.css), embedded at build time. The text is parsed
 * once per process with the declaration hook of the CSS module, so invalid UA declarations are
 * dropped the same way author declarations are, and the parsed sheet (immutable, atomically
 * refcounted) is shared by every style engine (FP2b). The cache keeps its own reference only
 * while someone uses it (an engine or a runtime that prefetches it): when the last user goes the
 * reference is dropped, so a process that created and destroyed everything is back at zero bytes.
 */
#include "style_int.h"

#include <stdatomic.h>

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>

#ifdef EMBER_LZ_ASSETS
/* EMBER_COMPRESS_ASSETS: stored as a tools/lz stream and expanded for the parse (the sheet copies what it keeps). */
#include "ember_lz/ua/html.css.lz.h"
#define ua_css_bytes embi_lz_ua_html_css
#else
static const unsigned char ua_css_bytes[] = {
#embed "ua/html.css"
    , 0
};
#endif

static embi_css_sheet *ua_parse(void)
{
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
#ifdef EMBER_LZ_ASSETS
    size_t raw = 0;
    uint8_t *buf = embi_lz_unpack(ua_css_bytes, sizeof ua_css_bytes, &raw);
    embi_str text = {(const char *)buf, (uint32_t)raw};
    embi_css_sheet *sheet = buf != nullptr ? embi_css_sheet_parse(text, &opts) : nullptr;
    if (buf != nullptr) {
        embi_free(buf, raw + 1);
    }
    return sheet;
#else
    embi_str text = {(const char *)ua_css_bytes, (uint32_t)(sizeof ua_css_bytes - 1)};
    return embi_css_sheet_parse(text, &opts);
#endif
}

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static embi_css_sheet *g_sheet; /* the cache's reference; guarded by g_lock */
static uint32_t g_users;        /* guarded by g_lock */

static void ua_lock(void)
{
    uint32_t spins = 0;
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
        if (++spins > 64) {
            SDL_DelayNS(20000); /* the holder is parsing: about a millisecond */
        } else {
            SDL_CPUPauseInstruction();
        }
    }
}

static void ua_unlock(void)
{
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

embi_css_sheet *embi_style_ua_sheet(void)
{
    ua_lock();
    if (g_sheet == nullptr) {
        g_sheet = ua_parse();
    }
    if (g_sheet == nullptr) { /* the compressed sheet would not unpack (a damaged build): no UA rules, no user counted */
        ua_unlock();
        return nullptr;
    }
    g_users++;
    embi_css_sheet_ref(g_sheet);
    embi_css_sheet *s = g_sheet;
    ua_unlock();
    return s;
}

void embi_style_ua_unuse(void)
{
    ua_lock();
    EMBI_ASSERT(g_users > 0);
    embi_css_sheet *drop = nullptr;
    if (g_users > 0 && --g_users == 0) {
        drop = g_sheet;
        g_sheet = nullptr;
    }
    ua_unlock();
    embi_css_sheet_unref(drop);
}

void embi_style_ua_release(embi_css_sheet *s)
{
    if (s == nullptr) { /* embi_style_ua_sheet failed: it counted no user */
        return;
    }
    embi_css_sheet_unref(s);
    embi_style_ua_unuse();
}

uint32_t embi_style_ua_users(void)
{
    ua_lock();
    uint32_t n = g_users;
    ua_unlock();
    return n;
}
