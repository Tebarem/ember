#pragma once
/*
 * Shared helpers for the PngSuite tests: directory listing (SDL_EnumerateDirectory, sorted by
 * nothing in particular), file loading relative to EMBER_SOURCE_DIR, and name checks.
 */
#include "emb_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#define PS_MAX_NAMES 512
#define PS_NAME_LEN 64

typedef struct ps_list {
    char names[PS_MAX_NAMES][PS_NAME_LEN];
    int n;
} ps_list;

static inline SDL_EnumerationResult ps_collect(void *ud, const char *dirname, const char *fname)
{
    (void)dirname;
    ps_list *l = ud;
    if (fname[0] != '.' && l->n < PS_MAX_NAMES) {
        SDL_strlcpy(l->names[l->n], fname, PS_NAME_LEN);
        l->n++;
    }
    return SDL_ENUM_CONTINUE;
}

/* Lists the entries of a directory relative to the source root. */
static inline void ps_dir(const char *rel, ps_list *l)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    l->n = 0;
    SDL_EnumerateDirectory(path, ps_collect, l);
}

static inline bool ps_has_suffix(const char *s, const char *suf)
{
    size_t n = SDL_strlen(s);
    size_t m = SDL_strlen(suf);
    return n >= m && SDL_strcmp(s + n - m, suf) == 0;
}

/* Copies the file name without its extension (the suffix must match). */
static inline void ps_stem(const char *fname, const char *suf, char *out, size_t cap)
{
    size_t n = SDL_strlen(fname) - SDL_strlen(suf);
    if (n >= cap) {
        n = cap - 1;
    }
    SDL_memcpy(out, fname, n);
    out[n] = '\0';
}

static inline bool ps_listed(const ps_list *l, const char *name)
{
    for (int i = 0; i < l->n; i++) {
        if (SDL_strcmp(l->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Loads a file relative to the source root. Returns nullptr on failure. */
static inline uint8_t *ps_load(const char *rel, size_t *len)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    *len = 0;
    return (uint8_t *)SDL_LoadFile(path, len);
}
