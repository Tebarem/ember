#pragma once
/*
 * Golden-image harness (header-only, builds on emb_test.h).
 *
 *     EMB_CHECK(emb_gold_check("box_fill", px, 64, 64));
 *
 * px is tightly packed RGBA8, row-major, top row first. It is compared with the golden
 * <golden dir>/<name>.bmp. A pixel matches when every channel is within +-EMB_GOLD_TOL.
 * The check fails when more than 0.5% of the pixels mismatch, or when any pixel is off by more
 * than EMB_GOLD_HARD in some channel (a real bug, not antialiasing noise).
 *
 * Environment:
 *   EMBER_UPDATE_GOLDENS=1  write (or overwrite) the golden and pass.
 *   EMBER_GOLD_DIR=<dir>    golden directory. Default: the tests/gold folder next to this header.
 *   EMBER_GOLD_OUT=<dir>    failure output directory. Default: gold-out (relative to the working
 *                           directory, which ctest sets to the build directory of the test).
 *
 * On failure the actual image is written to <out>/<name>.actual.bmp and the diff image to
 * <out>/<name>.diff.bmp. In the diff image, matching pixels show the actual pixel at a quarter
 * of its brightness, pixels outside +-EMB_GOLD_TOL are magenta, and pixels outside
 * +-EMB_GOLD_HARD are red.
 *
 * Header-only: every function is static, so unused helpers cost nothing.
 */

#include "emb_test.h"

#include <stdint.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_surface.h>

#define EMB_GOLD_TOL 3
#define EMB_GOLD_HARD 64
/* Allowed mismatch fraction in parts per thousand: 5 means 0.5%. */
#define EMB_GOLD_BAD_PER_MILLE 5
#define EMB_GOLD_PATH_MAX 1024

typedef struct emb_gold_report {
    uint64_t total;      /* pixel count */
    uint64_t mismatched; /* pixels with a channel off by more than EMB_GOLD_TOL */
    int max_diff;        /* largest channel difference seen */
    int hard_x;          /* first pixel off by more than EMB_GOLD_HARD, or -1 */
    int hard_y;
} emb_gold_report;

static inline int emb_gold_channel_diff(const uint8_t *a, const uint8_t *b)
{
    int m = 0;
    for (int c = 0; c < 4; c++) {
        int d = (int)a[c] - (int)b[c];
        if (d < 0) {
            d = -d;
        }
        if (d > m) {
            m = d;
        }
    }
    return m;
}

/* Pure comparison of two RGBA8 buffers of the same size. No I/O. */
static inline emb_gold_report emb_gold_compare(const uint8_t *actual, const uint8_t *golden,
                                               int w, int h)
{
    emb_gold_report r = {0};
    r.hard_x = -1;
    r.hard_y = -1;
    r.total = (uint64_t)w * (uint64_t)h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            int d = emb_gold_channel_diff(actual + i, golden + i);
            if (d > r.max_diff) {
                r.max_diff = d;
            }
            if (d > EMB_GOLD_TOL) {
                r.mismatched++;
            }
            if (d > EMB_GOLD_HARD && r.hard_x < 0) {
                r.hard_x = x;
                r.hard_y = y;
            }
        }
    }
    return r;
}

static inline bool emb_gold_report_pass(const emb_gold_report *r)
{
    return r->hard_x < 0 && r->mismatched * 1000u <= r->total * EMB_GOLD_BAD_PER_MILLE;
}

/* Diff image (RGBA8, w*h*4 bytes) as described in the header comment. */
static inline void emb_gold_make_diff(const uint8_t *actual, const uint8_t *golden, int w, int h,
                                      uint8_t *out)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            int d = emb_gold_channel_diff(actual + i, golden + i);
            uint8_t *o = out + i;
            if (d > EMB_GOLD_HARD) {
                o[0] = 255;
                o[1] = 0;
                o[2] = 0;
            } else if (d > EMB_GOLD_TOL) {
                o[0] = 255;
                o[1] = 0;
                o[2] = 255;
            } else {
                o[0] = (uint8_t)(actual[i] / 4);
                o[1] = (uint8_t)(actual[i + 1] / 4);
                o[2] = (uint8_t)(actual[i + 2] / 4);
            }
            o[3] = 255;
        }
    }
}

/* Golden directory, written into buf. */
static inline void emb_gold_root(char *buf, size_t cap)
{
    const char *env = SDL_getenv("EMBER_GOLD_DIR");
    if (env != nullptr && env[0] != '\0') {
        SDL_snprintf(buf, cap, "%s", env);
        return;
    }
    /* __FILE__ is this header, tests/emb_gold.h: keep its directory and append gold. */
    const char *f = __FILE__;
    size_t n = SDL_strlen(f);
    while (n > 0 && f[n - 1] != '/' && f[n - 1] != '\\') {
        n--;
    }
    SDL_snprintf(buf, cap, "%.*sgold", (int)n, f);
}

static inline void emb_gold_out_root(char *buf, size_t cap)
{
    const char *env = SDL_getenv("EMBER_GOLD_OUT");
    if (env != nullptr && env[0] != '\0') {
        SDL_snprintf(buf, cap, "%s", env);
    } else {
        SDL_snprintf(buf, cap, "gold-out");
    }
}

static inline bool emb_gold_join(char *buf, size_t cap, const char *dir, const char *name,
                                 const char *suffix)
{
    int n = SDL_snprintf(buf, cap, "%s/%s%s", dir, name, suffix);
    return n > 0 && (size_t)n < cap;
}

static inline bool emb_gold_update_requested(void)
{
    const char *v = SDL_getenv("EMBER_UPDATE_GOLDENS");
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

/* Writes RGBA8 pixels as a 32-bit BMP. */
static inline bool emb_gold_save(const char *path, const uint8_t *px, int w, int h)
{
    /* SDL_CreateSurfaceFrom takes a non-const pointer, but SDL_SaveBMP only reads it. */
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void *)px, w * 4);
    if (s == nullptr) {
        return false;
    }
    bool ok = SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    return ok;
}

/* Loads a BMP as tightly packed RGBA8 (SDL_free the result). Returns nullptr on failure. */
static inline uint8_t *emb_gold_load(const char *path, int *w, int *h)
{
    SDL_Surface *s = SDL_LoadBMP(path);
    if (s == nullptr) {
        return nullptr;
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (c == nullptr) {
        return nullptr;
    }
    size_t row = (size_t)c->w * 4u;
    uint8_t *out = SDL_malloc(row * (size_t)c->h);
    if (out != nullptr) {
        for (int y = 0; y < c->h; y++) {
            SDL_memcpy(out + (size_t)y * row, (const uint8_t *)c->pixels + (size_t)y * (size_t)c->pitch,
                       row);
        }
        *w = c->w;
        *h = c->h;
    }
    SDL_DestroySurface(c);
    return out;
}

static inline void emb_gold_save_failure(const char *name, const uint8_t *actual,
                                         const uint8_t *golden, int w, int h)
{
    char out[EMB_GOLD_PATH_MAX];
    char path[EMB_GOLD_PATH_MAX];
    emb_gold_out_root(out, sizeof out);
    SDL_CreateDirectory(out);

    if (emb_gold_join(path, sizeof path, out, name, ".actual.bmp") &&
        emb_gold_save(path, actual, w, h)) {
        SDL_Log("gold %s: actual image written to %s", name, path);
    } else {
        SDL_Log("gold %s: could not write %s", name, path);
    }

    if (golden == nullptr) {
        return;
    }
    uint8_t *diff = SDL_malloc((size_t)w * (size_t)h * 4u);
    if (diff == nullptr) {
        return;
    }
    emb_gold_make_diff(actual, golden, w, h, diff);
    if (emb_gold_join(path, sizeof path, out, name, ".diff.bmp") &&
        emb_gold_save(path, diff, w, h)) {
        SDL_Log("gold %s: diff image written to %s", name, path);
    } else {
        SDL_Log("gold %s: could not write %s", name, path);
    }
    SDL_free(diff);
}

/* Compares px (w x h RGBA8) with the golden named name. Returns true on a match, or when
 * EMBER_UPDATE_GOLDENS=1 wrote the golden. Failures are logged; callers use EMB_CHECK(). */
static inline bool emb_gold_check(const char *name, const uint8_t *px, int w, int h)
{
    char root[EMB_GOLD_PATH_MAX];
    char path[EMB_GOLD_PATH_MAX];
    if (name == nullptr || px == nullptr || w <= 0 || h <= 0) {
        SDL_Log("gold: invalid arguments");
        return false;
    }
    emb_gold_root(root, sizeof root);
    if (!emb_gold_join(path, sizeof path, root, name, ".bmp")) {
        SDL_Log("gold %s: path too long", name);
        return false;
    }

    if (emb_gold_update_requested()) {
        SDL_CreateDirectory(root);
        if (!emb_gold_save(path, px, w, h)) {
            SDL_Log("gold %s: cannot write %s: %s", name, path, SDL_GetError());
            return false;
        }
        SDL_Log("gold %s: golden updated at %s", name, path);
        return true;
    }

    int gw = 0;
    int gh = 0;
    uint8_t *want = emb_gold_load(path, &gw, &gh);
    if (want == nullptr) {
        SDL_Log("gold %s: no golden at %s (rerun with EMBER_UPDATE_GOLDENS=1 to create it)", name,
                path);
        emb_gold_save_failure(name, px, nullptr, w, h);
        return false;
    }
    if (gw != w || gh != h) {
        SDL_Log("gold %s: image is %dx%d but golden is %dx%d", name, w, h, gw, gh);
        SDL_free(want);
        emb_gold_save_failure(name, px, nullptr, w, h);
        return false;
    }

    emb_gold_report r = emb_gold_compare(px, want, w, h);
    bool ok = emb_gold_report_pass(&r);
    if (!ok) {
        if (r.hard_x >= 0) {
            SDL_Log("gold %s: pixel (%d,%d) is off by more than %d", name, r.hard_x, r.hard_y,
                    EMB_GOLD_HARD);
        }
        SDL_Log("gold %s: %llu of %llu pixels differ by more than %d (max channel diff %d)", name,
                (unsigned long long)r.mismatched, (unsigned long long)r.total, EMB_GOLD_TOL,
                r.max_diff);
        emb_gold_save_failure(name, px, want, w, h);
    }
    SDL_free(want);
    return ok;
}

/* Reports a failure, with the actual value, for one pixel that is not exactly rgba. */
static inline void emb_gold_report_pixel(const char *file, int line, const uint8_t *px, int w,
                                         int x, int y, const uint8_t want[4])
{
    char msg[256];
    const uint8_t *got = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
    SDL_snprintf(msg, sizeof msg, "pixel (%d,%d) is (%u,%u,%u,%u), want (%u,%u,%u,%u)", x, y,
                 got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
    emb_test_report_fail(file, line, msg);
}

/* Exact check of one pixel: EMB_CHECK_PIXEL(px, w, x, y, r, g, b, a). Use it for interior
 * samples whose expected colors are computed by hand, so a golden cannot encode a bug. */
#define EMB_CHECK_PIXEL(px, w, x, y, r, g, b, a)                                               \
    do {                                                                                       \
        const uint8_t emb_want_px_[4] = {(uint8_t)(r), (uint8_t)(g), (uint8_t)(b),             \
                                         (uint8_t)(a)};                                        \
        const int emb_px_x_ = (x);                                                             \
        const int emb_px_y_ = (y);                                                             \
        const uint8_t *emb_got_px_ =                                                           \
            (px) + ((size_t)emb_px_y_ * (size_t)(w) + (size_t)emb_px_x_) * 4u;                 \
        if (SDL_memcmp(emb_got_px_, emb_want_px_, 4) != 0) {                                   \
            emb_gold_report_pixel(__FILE__, __LINE__, (px), (w), emb_px_x_, emb_px_y_,         \
                                  emb_want_px_);                                               \
        }                                                                                      \
    } while (0)
