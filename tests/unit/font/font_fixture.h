#pragma once
/*
 * Shared helpers for the font tests (tests/unit/font/test_font_*.c). Not a test itself: the test
 * glob only picks up test_*.c, so this header is never built as an executable.
 *
 * Fixtures live in tests/fonts and are found through EMBER_SOURCE_DIR. Expected values in the
 * tests come from the raw tables (read with a small Python table parser) and are commented
 * with their source.
 */

#include "emb_test.h"

#include "font/font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

/* A fixture file loaded into memory. Free with fx_free. */
typedef struct fx_file {
    uint8_t *p;
    size_t n;
} fx_file;

/* Loads tests/fonts/<name>. Logs and returns false when the file cannot be read. */
static inline bool fx_load(const char *name, fx_file *out)
{
    char path[512];
    size_t n = 0;
    SDL_snprintf(path, sizeof path, "%s/tests/fonts/%s", EMBER_SOURCE_DIR, name);
    out->p = (uint8_t *)SDL_LoadFile(path, &n);
    out->n = n;
    if (out->p == nullptr) {
        SDL_Log("cannot read fixture %s: %s", path, SDL_GetError());
        return false;
    }
    return true;
}

static inline void fx_free(fx_file *f)
{
    SDL_free(f->p);
    f->p = nullptr;
    f->n = 0;
}

static inline double fx_abs(double v)
{
    return v < 0.0 ? -v : v;
}

/* Opens face `index` of a loaded file. Returns false when the face does not open. */
static inline bool fx_open(const fx_file *f, uint32_t index, embi_font *out)
{
    return embi_font_open(f->p, f->n, index, out);
}

/* Checks |a - b| <= tol for floating values, reporting both numbers on failure. */
#define EMB_CHECK_NEAR(a, b, tol)                                                              \
    do {                                                                                       \
        const double emb_fx_a_ = (double)(a);                                                  \
        const double emb_fx_b_ = (double)(b);                                                  \
        const double emb_fx_t_ = (double)(tol);                                                \
        if (!(fx_abs(emb_fx_a_ - emb_fx_b_) <= emb_fx_t_)) {                                   \
            char emb_fx_msg_[256];                                                             \
            SDL_snprintf(emb_fx_msg_, sizeof emb_fx_msg_, "%s = %.6f, want %.6f (tol %g)",     \
                         #a, emb_fx_a_, emb_fx_b_, emb_fx_t_);                                 \
            emb_test_report_fail(__FILE__, __LINE__, emb_fx_msg_);                             \
        }                                                                                      \
    } while (0)

/* Checks that an embi_str equals a C string. */
static inline bool fx_str_is(embi_str s, const char *want)
{
    const size_t n = strlen(want);
    return s.len == n && s.ptr != nullptr && memcmp(s.ptr, want, n) == 0;
}

/* Reports a failure when the outline's contours are not closed: the last quad of each contour
 * must end at the first point of that contour. Returns the number of open contours. */
static inline int fx_count_open_contours(const embi_outline *o)
{
    int open = 0;
    for (uint32_t c = 0; c < o->contours.len; c++) {
        const uint32_t first = o->contours.data[c];
        const uint32_t end = c + 1 < o->contours.len ? o->contours.data[c + 1] : o->quads.len;
        if (end <= first) {
            open++;
            continue;
        }
        const embi_pt a = o->quads.data[first].p0;
        const embi_pt b = o->quads.data[end - 1].p2;
        if (fx_abs((double)a.x - b.x) > 1e-3 || fx_abs((double)a.y - b.y) > 1e-3) {
            open++;
        }
    }
    return open;
}
