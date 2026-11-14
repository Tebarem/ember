#include "sysfont_fixture.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_storage.h>
#include <SDL3/SDL_timer.h>

/* Smoke test against the machine's real fonts (W4i): the platform's own font directories, with no fixture. It asserts
 * only what holds on any machine (a machine without fonts gives an empty index), and logs the numbers the notes report:
 * files, faces, the cold and warm index times, the index memory, and what loading the largest CJK face costs.
 * On a typical Linux system it indexes /usr/share/fonts and the fontconfig directories. */

static void remove_cache(const char *name)
{
    SDL_Storage *st = SDL_OpenUserStorage("ember", "fontcache", 0);
    for (int i = 0; st != nullptr && i < 200 && !SDL_StorageReady(st); i++) {
        SDL_Delay(10);
    }
    if (st != nullptr) {
        SDL_RemoveStoragePath(st, name);
        SDL_CloseStorage(st);
    }
}

EMB_TEST(index_the_platform_font_directories)
{
    static const char *const name = "w4i-test-real.bin";
    remove_cache(name);
    size_t base = embi_mem_live();
    embi_sys_stats cold, warm;
    {
        embi_fontdb *db = embi_fontdb_create();
        embi_sys_opts o;
        SDL_memset(&o, 0, sizeof o);
        o.cache = true;
        o.cache_name = name;
        o.sync = true; /* dirs nullptr: the platform's directories */
        EMB_REQUIRE(embi_sys_start(db, &o));
        embi_sys_stats_get(db, &cold);
        EMB_CHECK(cold.ready);
        EMB_CHECK(!cold.warm);
        EMB_CHECK(cold.entries >= cold.files || cold.files == 0 || cold.entries > 0);
        for (uint32_t i = 0; i < embi_sys_count(db->sys); i++) {
            uint32_t fi = 0;
            EMB_CHECK(embi_sys_path(db->sys, i, &fi) != nullptr);
        }
        /* The largest face that covers a Han ideograph (a CJK face): load it and report what it costs. */
        uint32_t big = UINT32_MAX;
        uint64_t big_bytes = 0;
        for (uint32_t i = 0; i < embi_sys_count(db->sys); i++) {
            if (!embi_sys_block(db->sys, i, 0x4E00)) {
                continue;
            }
            uint32_t fi = 0;
            const char *p = embi_sys_path(db->sys, i, &fi);
            SDL_PathInfo pi;
            if (p != nullptr && SDL_GetPathInfo(p, &pi) && pi.size > big_bytes) {
                big_bytes = pi.size;
                big = i;
            }
        }
        if (big != UINT32_MAX) {
            uint64_t before = (uint64_t)embi_mem_live();
            embi_face *f = embi_fontdb_system_face(db, big);
            if (f != nullptr) {
                uint32_t fi = 0;
                SDL_Log("largest Han face: %s (file %llu bytes), loaded whole: %llu bytes of font data, heap delta %lld "
                        "bytes, covers U+4E00: %d",
                        embi_sys_path(db->sys, big, &fi), (unsigned long long)big_bytes,
                        (unsigned long long)f->data_len, (long long)((uint64_t)embi_mem_live() - before),
                        (int)embi_face_covers(f, 0x4E00));
                embi_sys_stats s;
                embi_sys_stats_get(db, &s);
                EMB_CHECK_EQ_INT(s.faces_loaded, 1);
                EMB_CHECK_EQ_INT(s.font_bytes, f->data_len);
            }
        }
        embi_fontdb_destroy(db);
    }
    {
        embi_fontdb *db = embi_fontdb_create();
        embi_sys_opts o;
        SDL_memset(&o, 0, sizeof o);
        o.cache = true;
        o.cache_name = name;
        o.sync = true;
        EMB_REQUIRE(embi_sys_start(db, &o));
        embi_sys_stats_get(db, &warm);
        EMB_CHECK(warm.ready);
        EMB_CHECK_EQ_INT(warm.entries, cold.entries);
        if (cold.files > 0) {
            EMB_CHECK(warm.warm);
            EMB_CHECK_EQ_INT(warm.reparsed, 0);
        }
        embi_fontdb_destroy(db);
    }
    SDL_Log("real fonts (%s): %u files, %u faces; cold index %.2f ms (%u files parsed), warm index %.2f ms "
            "(%u parsed); index memory %llu bytes",
            SDL_GetPlatform(), cold.files, cold.entries, cold.index_ms, cold.reparsed, warm.index_ms, warm.reparsed,
            (unsigned long long)cold.index_bytes);
    remove_cache(name);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST_MAIN()
