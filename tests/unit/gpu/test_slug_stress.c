#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <SDL3/SDL_stdinc.h>

#include "gpu/path.h"

/* 100k glyph-like fills on one 1024 px target. Every fill is one of 256 distinct paths, so the
 * slug cache holds 256 blobs (about 1 MB, past the initial 64 KB buffer, so growth runs too). Path
 * k is the 2x2 square with k extra collinear vertices on its bottom edge: same area, distinct
 * curves, distinct key. Each fill sits on an integer grid with 3 px spacing and covers exactly
 * four full pixels, so the total alpha is exact. The whole frame must go out in at most 8 draw
 * calls. */

#define STRESS_N 100000u
#define STRESS_PATHS 256u
#define STRESS_EXTRA_MAX 255u
#define STRESS_SIDE 1024u
#define STRESS_COLS 317u /* 317 * 317 = 100489 >= STRESS_N, and 317 * 3 = 951 < 1024 */

typedef struct stress_ctx {
    embi_path **paths;
    uint32_t n;
} stress_ctx;

static void stress_scene(embi_frame *f, void *user)
{
    const stress_ctx *c = user;
    for (uint32_t i = 0; i < c->n; i++) {
        const float x = (float)((i % STRESS_COLS) * 3u);
        const float y = (float)((i / STRESS_COLS) * 3u);
        const float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, x, y};
        const uint32_t xf = embi_frame_xform(f, m);
        const embi_paint paint = embi_paint_solid(RGBA(255, 0, 0, 255));
        embi_frame_fill_path(f, c->paths[i % STRESS_PATHS], EMBI_FILL_NONZERO, &paint, xf, 0);
    }
}

/* The square with `extra` collinear points on its bottom edge. */
static embi_path *stress_path(uint32_t extra)
{
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, 0.0f, 0.0f);
    for (uint32_t j = 1; j <= extra; j++) {
        embi_path_line_to(p, 2.0f * (float)j / (float)(extra + 1u), 0.0f);
    }
    embi_path_line_to(p, 2.0f, 0.0f);
    embi_path_line_to(p, 2.0f, 2.0f);
    embi_path_line_to(p, 0.0f, 2.0f);
    embi_path_close(p);
    return p;
}

EMB_TEST(slug_100k_glyph_stress_draws_at_most_eight)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, STRESS_SIDE, STRESS_SIDE)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }

    embi_path *paths[STRESS_PATHS];
    for (uint32_t k = 0; k < STRESS_PATHS; k++) {
        paths[k] = stress_path(k % (STRESS_EXTRA_MAX + 1u));
        EMB_REQUIRE(paths[k] != nullptr);
    }

    stress_ctx ctx = {.paths = paths, .n = STRESS_N};
    const uint32_t draws = slug_rig_frame(&rg, 1.0f, stress_scene, &ctx, true, RGBA(0, 0, 0, 0));
    EMB_CHECK(draws <= 8u);

    uint8_t *px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* 100000 squares of 4 full pixels each: 400000 units of full coverage. */
    const double want = (double)STRESS_N * 4.0;
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, STRESS_SIDE, STRESS_SIDE) - want) < 0.5);
    /* The first square is at the origin, the second one grid step to the right. */
    EMB_CHECK_PIXEL(px, STRESS_SIDE, 0, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, STRESS_SIDE, 2, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, STRESS_SIDE, 3, 0, 255, 0, 0, 255);
    SDL_free(px);

    for (uint32_t k = 0; k < STRESS_PATHS; k++) {
        embi_path_destroy(paths[k]);
    }
    slug_rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
