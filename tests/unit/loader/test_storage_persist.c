/*
 * Storage backend, persisted: the store survives closing and reopening its directory, origins keep
 * separate files, a flush writes the dirty areas, and a damaged file starts its origin empty instead
 * of failing. The storage root is a directory under the test executable's folder (the build tree),
 * never the user's storage location, so the user-storage opener is not called here.
 */
#include "loader_test.h"

#include "loader/storage.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

static embi_str origin_of(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static embi_u16view units_of(uint16_t *buf, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint16_t)(unsigned char)s[i];
    }
    return (embi_u16view){buf, (uint32_t)n};
}

static bool value_is(embi_u16view v, const char *s)
{
    size_t n = strlen(s);
    if (v.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (v.p[i] != (uint16_t)(unsigned char)s[i]) {
            return false;
        }
    }
    return true;
}

/* Deletes every file in dir and then dir itself, ignoring what is missing. */
static SDL_EnumerationResult remove_entry(void *userdata, const char *dirname, const char *fname)
{
    (void)userdata;
    char full[1024];
    SDL_snprintf(full, sizeof full, "%s/%s", dirname, fname);
    (void)SDL_RemovePath(full);
    return SDL_ENUM_CONTINUE;
}

static void purge_dir(const char *dir)
{
    (void)SDL_EnumerateDirectory(dir, remove_entry, nullptr);
    (void)SDL_RemovePath(dir);
}

/* The directory for this test, under the executable's folder. Returns false when SDL has none. */
static bool test_dir(const char *name, char *out, size_t cap)
{
    const char *base = SDL_GetBasePath();
    if (base == nullptr) {
        return false;
    }
    SDL_snprintf(out, cap, "%sstorage_test_%s", base, name);
    return true;
}

/* Finds the first file in dir (for the damage test). */
static SDL_EnumerationResult take_first(void *userdata, const char *dirname, const char *fname)
{
    char *found = userdata;
    if (found[0] == '\0') {
        SDL_snprintf(found, 1024, "%s/%s", dirname, fname);
    }
    return SDL_ENUM_CONTINUE;
}

EMB_TEST(storage_persists_across_reopen)
{
    char dir[1024];
    if (!test_dir("persist", dir, sizeof dir)) {
        EMB_SKIP("no base path");
    }
    purge_dir(dir);

    uint16_t kb[16], vb[16];
    embi_u16view out = {nullptr, 0};

    embi_storage *st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    EMB_CHECK(embi_storage_is_persistent(st));
    embi_storage_area *site = embi_storage_local(st, origin_of("app://site"));
    embi_storage_area *other = embi_storage_local(st, origin_of("app://other"));
    EMB_CHECK(embi_storage_area_set(site, units_of(kb, "theme"), units_of(vb, "dark")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_set(site, units_of(kb, "count"), units_of(vb, "41")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_set(other, units_of(kb, "x"), units_of(vb, "1")) ==
              EMBI_STORAGE_OK);
    /* Closing flushes every dirty area. */
    embi_storage_close(st);

    st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    site = embi_storage_local(st, origin_of("app://site"));
    other = embi_storage_local(st, origin_of("app://other"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(site), 2);
    EMB_CHECK(embi_storage_area_get(site, units_of(kb, "theme"), &out));
    EMB_CHECK(value_is(out, "dark"));
    EMB_CHECK(embi_storage_area_key(site, 1, &out));
    EMB_CHECK(value_is(out, "count"));
    EMB_CHECK(embi_storage_area_get(other, units_of(kb, "x"), &out));
    EMB_CHECK(value_is(out, "1"));
    EMB_CHECK(!embi_storage_area_get(other, units_of(kb, "theme"), &out));

    /* An explicit flush writes the change; a removal and a clear persist too. */
    EMB_CHECK(embi_storage_area_set(site, units_of(kb, "count"), units_of(vb, "42")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_flush(st));
    embi_storage_area_remove(site, units_of(kb, "theme"));
    embi_storage_area_clear(other);
    embi_storage_close(st);

    st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    site = embi_storage_local(st, origin_of("app://site"));
    other = embi_storage_local(st, origin_of("app://other"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(site), 1);
    EMB_CHECK(embi_storage_area_get(site, units_of(kb, "count"), &out));
    EMB_CHECK(value_is(out, "42"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(other), 0);
    embi_storage_close(st);
    purge_dir(dir);
}

EMB_TEST(storage_damaged_file_starts_origin_empty)
{
    char dir[1024];
    if (!test_dir("damage", dir, sizeof dir)) {
        EMB_SKIP("no base path");
    }
    purge_dir(dir);

    uint16_t kb[8], vb[8];
    embi_storage *st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://broken"));
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "k"), units_of(vb, "v")) == EMBI_STORAGE_OK);
    embi_storage_close(st);

    /* Overwrite the one file in the directory with bytes that are not a store. */
    char file[1024] = {0};
    EMB_CHECK(SDL_EnumerateDirectory(dir, take_first, file));
    EMB_REQUIRE(file[0] != '\0');
    static const char junk[] = "this is not a store file at all";
    EMB_CHECK(SDL_SaveFile(file, junk, sizeof junk - 1));

    st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    a = embi_storage_local(st, origin_of("app://broken"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 0);
    /* The origin still works after the damage: a new write is saved over the bad file. */
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "n"), units_of(vb, "w")) == EMBI_STORAGE_OK);
    embi_storage_close(st);

    st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    a = embi_storage_local(st, origin_of("app://broken"));
    embi_u16view out = {nullptr, 0};
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 1);
    EMB_CHECK(embi_storage_area_get(a, units_of(kb, "n"), &out));
    EMB_CHECK(value_is(out, "w"));
    embi_storage_close(st);
    purge_dir(dir);
}

/* Counts the entries of a directory (the store files and any temp file). */
static SDL_EnumerationResult count_entry(void *userdata, const char *dirname, const char *fname)
{
    (void)dirname;
    (void)fname;
    (*(int *)userdata)++;
    return SDL_ENUM_CONTINUE;
}

static int count_files(const char *dir)
{
    int n = 0;
    (void)SDL_EnumerateDirectory(dir, count_entry, &n);
    return n;
}

/* Write-behind: a tick inside the delay writes nothing, a tick after EMBI_STORAGE_FLUSH_MS writes
 * the dirty area, and the written file holds the value for the next backend. */
EMB_TEST(storage_tick_flushes_only_after_the_delay)
{
    char dir[1024];
    if (!test_dir("tick_delay", dir, sizeof dir)) {
        EMB_SKIP("no base path");
    }
    purge_dir(dir);

    uint16_t kb[16], vb[16];
    embi_u16view out = {nullptr, 0};

    embi_storage *st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://timer"));
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "k"), units_of(vb, "v")) == EMBI_STORAGE_OK);

    /* The first unsaved change was just now: the delay has not passed, so nothing is written. */
    embi_storage_tick(st);
    EMB_CHECK_EQ_INT(count_files(dir), 0);

    /* Past the delay, the tick flushes the dirty area. */
    SDL_Delay(EMBI_STORAGE_FLUSH_MS + 100u);
    embi_storage_tick(st);
    EMB_CHECK(count_files(dir) > 0);

    /* A reopened backend loads the flushed value. */
    embi_storage_close(st);
    st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    a = embi_storage_local(st, origin_of("app://timer"));
    EMB_CHECK(embi_storage_area_get(a, units_of(kb, "k"), &out));
    EMB_CHECK(value_is(out, "v"));
    embi_storage_close(st);
    purge_dir(dir);
}

EMB_TEST(storage_tick_without_changes_is_a_no_op)
{
    char dir[1024];
    if (!test_dir("tick", dir, sizeof dir)) {
        EMB_SKIP("no base path");
    }
    purge_dir(dir);
    embi_storage *st = embi_storage_open_dir(dir);
    EMB_REQUIRE(st != nullptr);
    embi_storage_tick(st);
    EMB_CHECK(embi_storage_flush(st));
    embi_storage_close(st);
    /* Nothing was written, so the directory holds no store files. */
    char file[1024] = {0};
    EMB_CHECK(SDL_EnumerateDirectory(dir, take_first, file));
    EMB_CHECK(file[0] == '\0');
    purge_dir(dir);
}

LOADER_TEST_MAIN()
