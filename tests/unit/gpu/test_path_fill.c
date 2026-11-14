#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <SDL3/SDL_stdinc.h>

#include "gpu/path.h"

/* Filling paths: open subpaths close for the fill, overlapping subpaths follow the fill rule, and a
 * path that is cleared and refilled shows its new geometry (the blob cache must not serve the old
 * outline). */

#define W 48
#define H 48

static void add_square(embi_path *p, float x0, float y0, float x1, float y1, bool ccw)
{
    embi_path_move_to(p, x0, y0);
    if (ccw) {
        embi_path_line_to(p, x0, y1);
        embi_path_line_to(p, x1, y1);
        embi_path_line_to(p, x1, y0);
    } else {
        embi_path_line_to(p, x1, y0);
        embi_path_line_to(p, x1, y1);
        embi_path_line_to(p, x0, y1);
    }
    embi_path_close(p);
}

/* An open subpath (no close) must fill as if closed. */
static void scene_open(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, 8.0f, 8.0f);
    embi_path_line_to(p, 40.0f, 8.0f);
    embi_path_line_to(p, 40.0f, 40.0f);
    embi_path_line_to(p, 8.0f, 40.0f);
    /* No close: the fill adds the closing edge from (8, 40) back to (8, 8). */
    const embi_paint paint = embi_paint_solid(RGBA(255, 0, 0, 255));
    embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(path_open_subpath_is_filled_as_closed)
{
    uint8_t *px = emb_test_render(W, H, scene_open, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 9, 9, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 30, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 39, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 41, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 41, 0, 0, 0, 0);
    SDL_free(px);
}

/* Two overlapping squares of the same orientation: winding 2 in the overlap. Nonzero fills the
 * overlap, evenodd leaves it empty. */
static void scene_overlap(embi_frame *f, void *user)
{
    const embi_fill_rule rule = *(const embi_fill_rule *)user;
    embi_path *p = embi_path_create(0.0f);
    add_square(p, 4.0f, 4.0f, 28.0f, 28.0f, true);
    add_square(p, 12.0f, 12.0f, 40.0f, 40.0f, true);
    const embi_paint paint = embi_paint_solid(RGBA(0, 0, 255, 255));
    embi_frame_fill_path(f, p, rule, &paint, 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(path_overlapping_subpaths_follow_the_fill_rule)
{
    embi_fill_rule nz = EMBI_FILL_NONZERO;
    embi_fill_rule eo = EMBI_FILL_EVENODD;
    uint8_t *a = emb_test_render(W, H, scene_overlap, &nz);
    EMB_RENDER_OR_SKIP(a);
    uint8_t *b = emb_test_render(W, H, scene_overlap, &eo);
    EMB_REQUIRE(b != nullptr);
    EMB_CHECK_PIXEL(a, W, 16, 16, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(b, W, 16, 16, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(a, W, 6, 6, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(b, W, 6, 6, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(b, W, 36, 36, 0, 0, 255, 255);
    SDL_free(a);
    SDL_free(b);
}

/* Draws whatever path user points at, in blue. */
static void scene_path_blue(embi_frame *f, void *user)
{
    embi_path *p = user;
    const embi_paint paint = embi_paint_solid(RGBA(0, 0, 255, 255));
    embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, 0, 0);
}

/* Clear and refill one path object: the second outline must be what shows, not the cached first. */
EMB_TEST(path_clear_and_refill_shows_new_geometry)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, W, H)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }

    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    add_square(p, 4.0f, 4.0f, 20.0f, 20.0f, true);
    slug_rig_frame(&rg, 1.0f, scene_path_blue, p, true, RGBA(0, 0, 0, 0));

    /* The same object, cleared and refilled somewhere else. */
    embi_path_clear(p);
    add_square(p, 24.0f, 24.0f, 44.0f, 44.0f, true);
    slug_rig_frame(&rg, 1.0f, scene_path_blue, p, true, RGBA(0, 0, 0, 0));

    uint8_t *px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 255, 255);
    SDL_free(px);

    embi_path_destroy(p);
    slug_rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
