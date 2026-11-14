#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <SDL3/SDL_stdinc.h>

#include "gpu/path.h"
#include "gpu/stroke.h"

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

/* Draws the path of the request in green: filled, or stroked with its width (miter joins). */
typedef struct shape_req {
    embi_path *p;
    bool stroke;
    float width;
} shape_req;

static void scene_shape(embi_frame *f, void *user)
{
    const shape_req *q = user;
    const embi_paint paint = embi_paint_solid(RGBA(0, 255, 0, 255));
    if (q->stroke) {
        embi_stroke_style st;
        SDL_memset(&st, 0, sizeof st);
        st.width = q->width;
        st.join = EMBI_JOIN_MITER;
        st.miter_limit = 4.0f;
        embi_frame_stroke_path(f, q->p, &st, 0.0f, &paint, 0, 0);
    } else {
        embi_frame_fill_path(f, q->p, EMBI_FILL_NONZERO, &paint, 0, 0);
    }
}

/* A display list is built again on each paint with new path objects. Outlines are cached by what they are, so a new path
 * object of the same shape (a fill, or a stroke with the same style) uses the blob that is there instead of building one,
 * and a different shape or style builds its own. */
EMB_TEST(path_outlines_are_cached_by_their_shape)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, W, H)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    const uint32_t b0 = embi_renderer_outline_blobs(rg.r);
    shape_req q = {0};
    for (int i = 0; i < 3; i++) {
        q.p = embi_path_create(0.0f);
        add_square(q.p, 4.0f, 4.0f, 20.0f, 20.0f, true);
        q.stroke = false;
        slug_rig_frame(&rg, 1.0f, scene_shape, &q, true, RGBA(0, 0, 0, 0));
        EMB_CHECK_EQ_INT((int)embi_renderer_outline_blobs(rg.r), (int)b0 + 1);
        embi_path_destroy(q.p);
    }
    uint8_t *px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 0, 0);
    SDL_free(px);

    /* Strokes of new copies of a square with the same style: one blob, and the cached one draws the stroke. */
    for (int i = 0; i < 3; i++) {
        q.p = embi_path_create(0.0f);
        add_square(q.p, 24.0f, 24.0f, 40.0f, 40.0f, true);
        q.stroke = true;
        q.width = 4.0f;
        slug_rig_frame(&rg, 1.0f, scene_shape, &q, true, RGBA(0, 0, 0, 0));
        EMB_CHECK_EQ_INT((int)embi_renderer_outline_blobs(rg.r), (int)b0 + 2);
        embi_path_destroy(q.p);
    }
    px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, W, 24, 32, 0, 255, 0, 255); /* on the left edge of the stroke */
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 0, 0);     /* inside the square: not filled */
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);     /* the fill of the first frames is gone */
    SDL_free(px);

    /* Another width is another outline; another shape too. */
    q.p = embi_path_create(0.0f);
    add_square(q.p, 24.0f, 24.0f, 40.0f, 40.0f, true);
    q.width = 2.0f;
    slug_rig_frame(&rg, 1.0f, scene_shape, &q, true, RGBA(0, 0, 0, 0));
    EMB_CHECK_EQ_INT((int)embi_renderer_outline_blobs(rg.r), (int)b0 + 3);
    px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, W, 24, 32, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 22, 32, 0, 0, 0, 0); /* a 4 px stroke covers this pixel, a 2 px one does not */
    SDL_free(px);
    embi_path_clear(q.p);
    add_square(q.p, 4.0f, 4.0f, 21.0f, 20.0f, true);
    q.stroke = false;
    slug_rig_frame(&rg, 1.0f, scene_shape, &q, true, RGBA(0, 0, 0, 0));
    EMB_CHECK_EQ_INT((int)embi_renderer_outline_blobs(rg.r), (int)b0 + 4);
    embi_path_destroy(q.p);
    slug_rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
