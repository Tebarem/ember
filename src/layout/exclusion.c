/*
 * The exclusion space of a block formatting context (CSS 2.1 section 9.5, W3b). The space is the
 * list of the margin boxes of the floats placed in the BFC, in placement order. The queries are
 * linear scans: a page has few floats per BFC, and W3h decides whether shelves are needed.
 *
 * A rectangle overlaps the band [y, y + h) when it contains y, or when its top lies inside the band.
 * A zero-height float never overlaps a band, and a zero-height band (h = 0) overlaps the floats that
 * contain y, so a zero-width box can sit between floats at the same height.
 */
#include "layout_int.h"

#include <math.h>

void embi_excl_init(embi_excl *e)
{
    e->rects.data = nullptr;
    e->rects.len = 0;
    e->rects.cap = 0;
}

void embi_excl_free(embi_excl *e)
{
    embi_vec_free(&e->rects);
}

void embi_excl_add(embi_excl *e, float x0, float y0, float x1, float y1, bool left)
{
    float prev = e->rects.len > 0 ? e->rects.data[e->rects.len - 1].ymax : -INFINITY;
    embi_excl_rect q = {x0, y0, x1, y1, y1 > prev ? y1 : prev, left};
    embi_vec_push(&e->rects, q);
}

static bool spans(const embi_excl_rect *q, float y, float h)
{
    if (!(q->y1 > q->y0)) {
        return false;
    }
    return (q->y0 <= y && y < q->y1) || (y <= q->y0 && q->y0 < y + h);
}

void embi_excl_band(const embi_excl *e, float cl, float cr, float y, float h, float *l, float *r)
{
    float lo = cl, hi = cr;
    /* From the newest float back: the floats of a page lie in document order, so a line is usually below all but the last few
     * (a rect whose ymax is at or above y ends the search: it and the ones before it end above the band). */
    for (uint32_t i = e->rects.len; i > 0; i--) {
        const embi_excl_rect *q = &e->rects.data[i - 1];
        if (y >= q->ymax) {
            break;
        }
        if (!spans(q, y, h)) {
            continue;
        }
        if (q->left) {
            if (q->x1 > lo) {
                lo = q->x1;
            }
        } else if (q->x0 < hi) {
            hi = q->x0;
        }
    }
    *l = lo;
    *r = hi;
}

float embi_excl_next_y(const embi_excl *e, float y, float h)
{
    float best = INFINITY;
    for (uint32_t i = e->rects.len; i > 0; i--) {
        const embi_excl_rect *q = &e->rects.data[i - 1];
        if (y >= q->ymax) {
            break;
        }
        if (spans(q, y, h) && q->y1 > y && q->y1 < best) {
            best = q->y1;
        }
    }
    return best;
}

float embi_excl_clear_y(const embi_excl *e, uint8_t sides)
{
    float best = -INFINITY;
    for (uint32_t i = 0; i < e->rects.len; i++) {
        const embi_excl_rect *q = &e->rects.data[i];
        uint8_t side = q->left ? EMBI_CLR_LEFT : EMBI_CLR_RIGHT;
        if ((sides & side) != 0 && q->y1 > best) {
            best = q->y1;
        }
    }
    return best;
}

float embi_excl_max_top(const embi_excl *e)
{
    float best = -INFINITY;
    for (uint32_t i = 0; i < e->rects.len; i++) {
        if (e->rects.data[i].y0 > best) {
            best = e->rects.data[i].y0;
        }
    }
    return best;
}

float embi_excl_bottom(const embi_excl *e)
{
    float best = -INFINITY;
    for (uint32_t i = 0; i < e->rects.len; i++) {
        if (e->rects.data[i].y1 > best) {
            best = e->rects.data[i].y1;
        }
    }
    return best;
}
