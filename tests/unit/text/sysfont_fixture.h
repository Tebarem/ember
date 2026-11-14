#pragma once
/*
 * Helpers of the system font tests (test_text_sysfont_*.c): a font registry whose system index scans the fixture tree
 * tests/fonts/sysfonts (see tools/fonts/make_fixtures.py for what is in it), built on the calling thread.
 *
 * The tree: sans/EmberSans-{Regular,Bold}.ttf, deep/er/EmberSerif-Italic.ttf (typographic family "Ember Serif Display",
 * a Japanese localized family name), cjk/EmberCJK.otf (U+4E00..4E03), collection.ttc (Ember Collection A and B),
 * woff/EmberWoff.woff, script/NotoSansCJK{JP,SC,TC,KR}.ttf (U+4E00..4E03) and script/NotoSansArabic.ttf
 * (U+0627..062A), plus ignored/ (a .fon and a .txt that must not be indexed). 11 font files, 12 faces.
 */

#include "text_fixture.h"

#define SYSFX_FILES 11u
#define SYSFX_FACES 12u

#define SYSFX_DIR EMBER_SOURCE_DIR "/tests/fonts/sysfonts"

/* Starts the index on db over dirs (default: the fixture tree). cache_name nullptr: no cache. */
static inline bool sysfx_start(embi_fontdb *db, const char *const *dirs, uint32_t ndirs, const char *cache_name,
                               bool sync)
{
    static const char *const def[] = {SYSFX_DIR};
    embi_sys_opts o;
    SDL_memset(&o, 0, sizeof o);
    o.dirs = dirs != nullptr ? dirs : def;
    o.ndirs = dirs != nullptr ? ndirs : 1;
    o.sync = sync;
    o.cache = cache_name != nullptr;
    o.cache_name = cache_name;
    return embi_sys_start(db, &o);
}

/* The entry whose full name or PostScript name is name, or UINT32_MAX. */
static inline uint32_t sysfx_find(const embi_fontdb *db, const char *name)
{
    uint32_t ids[8];
    uint32_t n = embi_sys_local(db->sys, tx_s(name), ids, 8);
    return n >= 1 ? ids[0] : UINT32_MAX;
}

/* The number of faces whose family is name (typographic, then legacy, then localized). */
static inline uint32_t sysfx_family_count(const embi_fontdb *db, const char *name)
{
    uint32_t ids[32];
    return embi_sys_family(db->sys, tx_s(name), ids, 32);
}

static inline bool sysfx_family_is(embi_face *f, const char *name)
{
    return f != nullptr && embi_str_eq(embi_face_family(f), tx_s(name));
}
