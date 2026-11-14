/*
 * The exclusion space of a BFC (W3b, exclusion.c): the band queries that inline and block layout
 * use, the clearance and next-position queries, and the memory it takes. Calls the module
 * functions directly, without a document.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

EMB_TEST(excl_empty_space_leaves_the_whole_width)
{
    embi_excl e;
    embi_excl_init(&e);
    float l = -1, r = -1;
    embi_excl_band(&e, 0, 200, 0, 10, &l, &r);
    EMB_CHECK(near(l, 0) && near(r, 200));
    EMB_CHECK(embi_excl_next_y(&e, 0, 10) == INFINITY);
    EMB_CHECK(embi_excl_clear_y(&e, EMBI_CLR_LEFT | EMBI_CLR_RIGHT) == -INFINITY);
    embi_excl_free(&e);
}

EMB_TEST(excl_band_is_bounded_by_the_floats_that_overlap_it)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 0, 100, 50, true);    /* left float 100 wide, 50 tall */
    embi_excl_add(&e, 150, 0, 200, 50, false); /* right float 50 wide, 50 tall */
    float l, r;
    embi_excl_band(&e, 0, 200, 10, 5, &l, &r);
    EMB_CHECK(near(l, 100) && near(r, 150));
    /* Below both floats the whole width is free again. */
    embi_excl_band(&e, 0, 200, 60, 5, &l, &r);
    EMB_CHECK(near(l, 0) && near(r, 200));
    embi_excl_free(&e);
}

EMB_TEST(excl_band_of_a_point_takes_the_floats_that_contain_it)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 0, 100, 50, true);
    embi_excl_add(&e, 100, 0, 200, 50, false);
    float l, r;
    embi_excl_band(&e, 0, 200, 0, 0, &l, &r);
    EMB_CHECK(near(l, 100) && near(r, 100));
    /* The band [-10, 0) ends where the floats start: no overlap, the whole width is free. */
    embi_excl_band(&e, 0, 200, -10, 10, &l, &r);
    EMB_CHECK(near(l, 0) && near(r, 200));
    embi_excl_free(&e);
}

EMB_TEST(excl_zero_height_float_never_overlaps)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 10, 100, 10, true);
    float l, r;
    embi_excl_band(&e, 0, 200, 10, 20, &l, &r);
    EMB_CHECK(near(l, 0) && near(r, 200));
    embi_excl_free(&e);
}

EMB_TEST(excl_next_y_is_the_lowest_bottom_of_the_blocking_floats)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 0, 100, 50, true);
    embi_excl_add(&e, 100, 0, 200, 30, false);
    embi_excl_add(&e, 0, 80, 50, 100, true); /* below the band: not a blocker */
    EMB_CHECK(near(embi_excl_next_y(&e, 0, 10), 30));
    EMB_CHECK(near(embi_excl_next_y(&e, 30, 10), 50));
    EMB_CHECK(embi_excl_next_y(&e, 50, 10) == INFINITY);
    embi_excl_free(&e);
}

EMB_TEST(excl_clear_y_separates_the_sides)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 0, 100, 50, true);
    embi_excl_add(&e, 100, 0, 200, 80, false);
    EMB_CHECK(near(embi_excl_clear_y(&e, EMBI_CLR_LEFT), 50));
    EMB_CHECK(near(embi_excl_clear_y(&e, EMBI_CLR_RIGHT), 80));
    EMB_CHECK(near(embi_excl_clear_y(&e, EMBI_CLR_LEFT | EMBI_CLR_RIGHT), 80));
    EMB_CHECK(near(embi_excl_max_top(&e), 0));
    EMB_CHECK(near(embi_excl_bottom(&e), 80));
    embi_excl_free(&e);
}

EMB_TEST(excl_max_top_follows_the_lowest_float_start)
{
    embi_excl e;
    embi_excl_init(&e);
    embi_excl_add(&e, 0, 5, 100, 50, true);
    embi_excl_add(&e, 100, 20, 200, 60, false);
    EMB_CHECK(near(embi_excl_max_top(&e), 20));
    embi_excl_free(&e);
}

EMB_TEST(excl_memory_returns_to_the_baseline)
{
    size_t before = embi_mem_live();
    embi_excl e;
    embi_excl_init(&e);
    for (int i = 0; i < 100; i++) {
        embi_excl_add(&e, (float)i, 0, (float)i + 1, 1, (i & 1) != 0);
    }
    EMB_CHECK(e.rects.len == 100);
    embi_excl_free(&e);
    EMB_CHECK(embi_mem_live() == before);
}

/* W3q: the queries stop at the newest rect whose prefix maximum (ymax) lies at or above the band; floats in no particular
 * y order (the list is in placement order, not y order) must still give what a scan of all of them gives. */
static void brute_band(const embi_excl *e, float cl, float cr, float y, float h, float *l, float *r)
{
    float lo = cl, hi = cr;
    for (uint32_t i = 0; i < e->rects.len; i++) {
        const embi_excl_rect *q = &e->rects.data[i];
        bool sp = q->y1 > q->y0 && ((q->y0 <= y && y < q->y1) || (y <= q->y0 && q->y0 < y + h));
        if (!sp) {
            continue;
        }
        if (q->left) {
            lo = q->x1 > lo ? q->x1 : lo;
        } else {
            hi = q->x0 < hi ? q->x0 : hi;
        }
    }
    *l = lo;
    *r = hi;
}

EMB_TEST(excl_band_and_next_y_equal_a_full_scan_for_floats_in_any_order)
{
    embi_excl e;
    embi_excl_init(&e);
    uint32_t seed = 12345u;
    for (int i = 0; i < 60; i++) {
        seed = seed * 1664525u + 1013904223u;
        float y0 = (float)((seed >> 8) % 400u);
        seed = seed * 1664525u + 1013904223u;
        float hh = (float)((seed >> 8) % 40u); /* 0 included: a zero-height float */
        seed = seed * 1664525u + 1013904223u;
        float x0 = (float)((seed >> 8) % 150u);
        embi_excl_add(&e, x0, y0, x0 + 30.0f, y0 + hh, (seed & 0x10000u) != 0);
        for (int q = 0; q < 12; q++) {
            seed = seed * 1664525u + 1013904223u;
            float y = (float)((seed >> 8) % 460u) - 10.0f;
            seed = seed * 1664525u + 1013904223u;
            float h = (float)((seed >> 8) % 30u);
            float l = 0, r = 0, bl = 0, br = 0;
            embi_excl_band(&e, 0, 200, y, h, &l, &r);
            brute_band(&e, 0, 200, y, h, &bl, &br);
            EMB_CHECK(l == bl && r == br);
            float nb = INFINITY;
            for (uint32_t k = 0; k < e.rects.len; k++) {
                const embi_excl_rect *rq = &e.rects.data[k];
                bool sp = rq->y1 > rq->y0 && ((rq->y0 <= y && y < rq->y1) || (y <= rq->y0 && rq->y0 < y + h));
                if (sp && rq->y1 > y && rq->y1 < nb) {
                    nb = rq->y1;
                }
            }
            EMB_CHECK(embi_excl_next_y(&e, y, h) == nb);
        }
        if ((i % 7) == 6) {
            e.rects.len -= 3; /* the list is cut back to an earlier state (block.c rolls floats back) */
        }
    }
    embi_excl_free(&e);
}

EMB_TEST_MAIN()
