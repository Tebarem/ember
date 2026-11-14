/*
 * 3D rendering contexts on the display list (R3c): planes with their homographies, ordering, splitting and
 * backface-visibility.
 */
#include "paint_harness.h"

#include <math.h>
#include <stdio.h>

static uint32_t count_planes(const embi_dl *d)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_LAYER_BEGIN && d->items.data[i].sub == EMBI_LS_PLANE) {
            n++;
        }
    }
    return n;
}

EMB_TEST(flat_affine_transform_makes_no_plane)
{
    ph h = ph_make("<div style='width:50px;height:50px;background:#f00;transform:rotate(10deg)'></div>", "");
    EMB_CHECK(count_planes(&h.dl) == 0);
    ph_free(&h);
}

EMB_TEST(perspective_child_makes_a_plane)
{
    ph h = ph_make("<div style='perspective:300px'><div style='width:100px;height:100px;background:#f00;"
                   "transform:rotateY(40deg)'></div></div>",
                   "");
    EMB_CHECK(count_planes(&h.dl) == 1);
    EMB_CHECK(h.dl.planes.len == 1);
    EMB_CHECK(ph_fill_of(&h.dl, PH_RED) >= 0);
    ph_free(&h);
}

EMB_TEST(backface_hidden_drops_the_plane)
{
    ph h = ph_make("<div style='perspective:300px'><div style='width:100px;height:100px;background:#f00;"
                   "backface-visibility:hidden;transform:rotateY(180deg)'></div></div>",
                   "");
    EMB_CHECK(ph_fill_of(&h.dl, PH_RED) < 0);
    ph_free(&h);
}

EMB_TEST(singular_transform_paints_nothing)
{
    ph h = ph_make("<div style='width:100px;height:100px;background:#f00;transform:scale3d(2,2,0)'></div>", "");
    EMB_CHECK(ph_fill_of(&h.dl, PH_RED) < 0);
    ph_free(&h);
}

EMB_TEST(preserve_3d_orders_by_depth)
{
    ph h = ph_make("<div style='perspective:500px'><div style='transform-style:preserve-3d;width:200px;height:200px'>"
                   "<div style='position:absolute;width:200px;height:200px;background:#00f;transform:translateZ(50px)'></div>"
                   "<div style='position:absolute;width:200px;height:200px;background:#f00;transform:translateZ(-50px)'></div>"
                   "</div></div>",
                   "");
    int r = ph_fill_of(&h.dl, PH_RED), b = ph_fill_of(&h.dl, PH_BLUE);
    EMB_REQUIRE(r >= 0 && b >= 0);
    EMB_CHECK(r < b); /* the far red plane paints first although it is later in the DOM */
    ph_free(&h);
}

/* The colors of the first FILL of each plane, in list order; n planes at most. Returns the count. */
static uint32_t plane_fills(const embi_dl *d, uint32_t *out, uint32_t *plane_idx, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len && n < cap; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind != EMBI_DI_LAYER_BEGIN || it->sub != EMBI_LS_PLANE) {
            continue;
        }
        out[n] = 0;
        for (uint32_t j = i + 1; j < it->aux && j < d->items.len; j++) {
            if (d->items.data[j].kind == EMBI_DI_FILL) {
                out[n] = d->items.data[j].colors[0];
                break;
            }
        }
        plane_idx[n] = i;
        n++;
    }
    return n;
}

EMB_TEST(intersecting_planes_are_split)
{
    ph h = ph_make("<div style='perspective:600px'><div style='transform-style:preserve-3d;width:200px;height:200px'>"
                   "<div style='position:absolute;width:200px;height:200px;background:#00f;transform:rotateY(30deg)'></div>"
                   "<div style='position:absolute;width:200px;height:200px;background:#f00;transform:rotateY(-30deg)'></div>"
                   "</div></div>",
                   "");
    uint32_t col[8], at[8];
    uint32_t n = plane_fills(&h.dl, col, at, 8);
    /* Blue (the first plane) is the splitter and stays whole; red is cut along the line where they cross into two
     * pieces: the half behind blue first, blue, then the half in front of it. */
    EMB_REQUIRE(n == 3);
    EMB_CHECK(col[0] == PH_RED && col[1] == PH_BLUE && col[2] == PH_RED);
    EMB_CHECK(h.dl.planes.len == 3);
    /* Blue draws under the clip it was given (none); the red pieces under the rotated half planes of the cut. */
    const embi_ditem *b0 = &h.dl.items.data[at[0]], *b1 = &h.dl.items.data[at[1]], *b2 = &h.dl.items.data[at[2]];
    EMB_REQUIRE(b0->clip != b1->clip && b2->clip != b1->clip && b0->clip < h.dl.clips.len && b2->clip < h.dl.clips.len);
    EMB_CHECK(h.dl.clips.data[b0->clip].parent == b1->clip && h.dl.clips.data[b2->clip].parent == b1->clip);
    const float *m0 = h.dl.xforms.data[h.dl.clips.data[b0->clip].xform].m;
    const float *m2 = h.dl.xforms.data[h.dl.clips.data[b2->clip].xform].m;
    /* The cut line is vertical (x = 100): the clip's x axis runs along y, and the kept side is -x for the piece that is
     * behind and +x for the one in front (the right half of red comes toward the viewer). */
    EMB_CHECK(fabsf(m0[0]) < 1e-3f && fabsf(m0[1]) > 0.99f);
    EMB_CHECK(fabsf(m2[0]) < 1e-3f && fabsf(m2[1]) > 0.99f);
    EMB_CHECK(m0[2] < -0.99f && fabsf(m0[3]) < 1e-3f);
    EMB_CHECK(m2[2] > 0.99f && fabsf(m2[3]) < 1e-3f);
    /* The line passes through the center of the composite (x = 100). */
    EMB_CHECK(fabsf(m0[4] - 100.0f) < 1.0f && fabsf(m2[4] - 100.0f) < 1.0f);
    ph_free(&h);
}

EMB_TEST(backface_hidden_inside_a_context_uses_the_accumulated_transform)
{
    /* The container is turned around; a hidden-back child turned around again faces the viewer, a hidden-back child that
     * is not turned shows its back. */
    ph h = ph_make("<div style='perspective:600px'><div style='transform-style:preserve-3d;width:200px;height:200px;"
                   "transform:rotateY(180deg)'>"
                   "<div style='position:absolute;width:200px;height:100px;background:#f00;backface-visibility:hidden;"
                   "transform:rotateY(180deg)'></div>"
                   "<div style='position:absolute;top:100px;width:200px;height:100px;background:#00f;"
                   "backface-visibility:hidden'></div>"
                   "</div></div>",
                   "");
    EMB_CHECK(ph_fill_of(&h.dl, PH_RED) >= 0);
    EMB_CHECK(ph_fill_of(&h.dl, PH_BLUE) < 0);
    ph_free(&h);
}

EMB_TEST(many_parallel_planes_keep_their_order)
{
    /* A carousel of 600 planes at increasing z, in the opposite order in the DOM: the BSP chain is as deep as the
     * plane count, so the recursion has to be bounded and the order must survive its fallback. */
    enum { N = 600 };
    static char html[N * 220 + 400];
    size_t len = 0;
    len += (size_t)snprintf(html + len, sizeof html - len,
                            "<div style='perspective:3000px'><div style='transform-style:preserve-3d;width:100px;height:100px'>");
    for (int i = N - 1; i >= 0; i--) {
        len += (size_t)snprintf(html + len, sizeof html - len,
                                "<div style='position:absolute;width:100px;height:100px;background:rgb(%d,%d,1);"
                                "transform:translateZ(%dpx)'></div>",
                                i & 255, i >> 8, i);
    }
    snprintf(html + len, sizeof html - len, "</div></div>");
    ph h = ph_make(html, "");
    static uint32_t col[N + 8], at[N + 8];
    uint32_t n = plane_fills(&h.dl, col, at, N + 8);
    EMB_REQUIRE(n == N);
    bool ordered = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t want = 0xFF010000u | ((uint32_t)(i >> 8) << 8) | (uint32_t)(i & 255);
        ordered = ordered && col[i] == want;
    }
    EMB_CHECK(ordered);
    ph_free(&h);
}

EMB_TEST(dropped_planes_leave_no_orphans)
{
    /* A hidden-back plane with a plane nested in it: neither is drawn and no plane record is left for them. */
    ph h = ph_make("<div style='perspective:600px'><div style='transform-style:preserve-3d;width:200px;height:200px'>"
                   "<div style='position:absolute;width:200px;height:200px;backface-visibility:hidden;"
                   "transform:rotateY(180deg);transform-style:preserve-3d'>"
                   "<div style='position:absolute;width:100px;height:100px;background:#f00;transform:rotateX(20deg)'></div>"
                   "</div>"
                   "<div style='position:absolute;width:100px;height:100px;background:#00f;transform:rotateX(20deg)'></div>"
                   "</div></div>",
                   "");
    EMB_CHECK(ph_fill_of(&h.dl, PH_BLUE) >= 0);
    EMB_CHECK(h.dl.planes.len == count_planes(&h.dl));
    ph_free(&h);
}

EMB_TEST_MAIN()
