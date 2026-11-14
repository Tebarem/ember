#pragma once
/*
 * Shared helpers for the renderer tests (tests/unit/gpu/test_render_*.c). Not a test itself:
 * the test glob only picks up test_*.c, so this header is never built as an executable.
 */

#include "emb_gold.h"

#include <stdint.h>

#include <SDL3/SDL_init.h>

/* Packs RGBA8 with r in the low byte, the layout the renderer uses for colors. */
#define RGBA(r, g, b, a)                                                                      \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

/* Checks one pixel per channel within +-tol. Use it where the expected value comes from
 * rounding a partial coverage, which can differ by one between GPUs. */
#define CHECK_PX_NEAR(px, w, x, y, r, g, b, a, tol)                                            \
    do {                                                                                       \
        const uint8_t *emb_cpn_p_ = (px) + ((size_t)(y) * (size_t)(w) + (size_t)(x)) * 4u;     \
        const int emb_cpn_want_[4] = {(r), (g), (b), (a)};                                     \
        for (int emb_cpn_c_ = 0; emb_cpn_c_ < 4; emb_cpn_c_++) {                               \
            int emb_cpn_d_ = (int)emb_cpn_p_[emb_cpn_c_] - emb_cpn_want_[emb_cpn_c_];          \
            if (emb_cpn_d_ < 0) {                                                              \
                emb_cpn_d_ = -emb_cpn_d_;                                                      \
            }                                                                                  \
            if (emb_cpn_d_ > (tol)) {                                                          \
                char emb_cpn_msg_[200];                                                        \
                SDL_snprintf(emb_cpn_msg_, sizeof emb_cpn_msg_,                                \
                             "pixel (%d,%d) channel %d is %u, want %d (tol %d)", (int)(x),     \
                             (int)(y), emb_cpn_c_, (unsigned)emb_cpn_p_[emb_cpn_c_],           \
                             emb_cpn_want_[emb_cpn_c_], (int)(tol));                           \
                emb_test_report_fail(__FILE__, __LINE__, emb_cpn_msg_);                        \
                break;                                                                         \
            }                                                                                  \
        }                                                                                      \
    } while (0)

#define TRANSPARENT RGBA(0, 0, 0, 0)

/* Test main for renderer tests. SDL_CreateGPUDevice needs the video subsystem, so it is
 * initialized here once, around the whole run. If it fails, every device-backed test skips. */
static inline int emb_render_test_run(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}

#define EMB_RENDER_TEST_MAIN()                                                                 \
    int main(int argc, char **argv)                                                            \
    {                                                                                          \
        (void)argc;                                                                            \
        (void)argv;                                                                            \
        return emb_render_test_run();                                                          \
    }
