#pragma once
/*
 * Reference rasterizer for the text quality tests: the exact box-filter coverage of an outline made of quadratic curves.
 *
 * Each quad is flattened into line segments (tolerance 1/256 px). The signed area and cover of every segment is
 * accumulated per pixel exactly (the "signed area accumulation" method of font-rs and stb_truetype v2) and a prefix sum
 * along each row gives the winding-weighted coverage. coverage = min(1, |sum|).
 *
 * Limitations: this equals the nonzero fill only where contours do not overlap with the same winding. Where two contours
 * of the same direction overlap (the overlapping contours of some variable fonts), the sum exceeds 1 and is clamped, which
 * is right for the union as long as no pixel is covered by opposite windings of different shapes. Pixels where opposite
 * windings partly overlap come out too light. Segments outside the grid are clipped (rows) or folded onto the border
 * columns (columns), so ink beyond the grid does not leak in.
 */

#include <stdint.h>

#include <SDL3/SDL_stdinc.h>

#define COVREF_TOL (1.0f / 256.0f)

typedef struct covref_acc {
    float *a;     /* (w + 2) * h accumulation cells */
    uint32_t w, h;
    uint32_t stride;
} covref_acc;

static inline void covref_cell(covref_acc *c, int x, int y, float v)
{
    c->a[(size_t)y * c->stride + (size_t)x] += v;
}

/* One line segment fully inside the columns [0, w]. */
static void covref_line_in(covref_acc *c, float x0, float y0, float x1, float y1)
{
    if (y0 == y1) {
        return;
    }
    float dir = 1.0f;
    if (y0 > y1) {
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        dir = -1.0f;
    }
    const float h = (float)c->h;
    if (y1 <= 0.0f || y0 >= h) {
        return;
    }
    const float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0.0f) {
        x -= y0 * dxdy;
        y0 = 0.0f;
    }
    if (y1 > h) {
        y1 = h;
    }
    const int ys = (int)y0;
    const int ye = (int)SDL_ceilf(y1);
    const float wf = (float)c->w;
    for (int y = ys; y < ye && y < (int)c->h; y++) {
        const float top = (float)y > y0 ? (float)y : y0;
        const float bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        const float dy = bot - top;
        float xnext = x + dxdy * dy;
        xnext = xnext < 0.0f ? 0.0f : (xnext > wf ? wf : xnext);
        const float d = dy * dir;
        const float xa = x < xnext ? x : xnext;
        const float xb = x < xnext ? xnext : x;
        const float xaf = SDL_floorf(xa);
        const int xai = (int)xaf;
        const int xbi = (int)SDL_ceilf(xb);
        if (xbi <= xai + 1) {
            const float xmf = 0.5f * (x + xnext) - xaf;
            covref_cell(c, xai, y, d - d * xmf);
            covref_cell(c, xai + 1, y, d * xmf);
        } else {
            const float s = 1.0f / (xb - xa);
            const float xaf_ = xa - xaf;
            const float a0 = 0.5f * s * (1.0f - xaf_) * (1.0f - xaf_);
            const float xbf = xb - (float)xbi + 1.0f;
            const float am = 0.5f * s * xbf * xbf;
            covref_cell(c, xai, y, d * a0);
            if (xbi == xai + 2) {
                covref_cell(c, xai + 1, y, d * (1.0f - a0 - am));
            } else {
                const float a1 = s * (1.5f - xaf_);
                covref_cell(c, xai + 1, y, d * (a1 - a0));
                for (int xi = xai + 2; xi < xbi - 1; xi++) {
                    covref_cell(c, xi, y, d * s);
                }
                const float a2 = a1 + (float)(xbi - xai - 3) * s;
                covref_cell(c, xbi - 1, y, d * (1.0f - a2 - am));
            }
            covref_cell(c, xbi, y, d * am);
        }
        x = xnext;
    }
}

/* A line segment anywhere: cut at the columns 0 and w, and the parts outside are put on the border column. */
static void covref_line(covref_acc *c, float x0, float y0, float x1, float y1)
{
    const float wf = (float)c->w;
    float cuts[4];
    int n = 0;
    cuts[n++] = 0.0f;
    if (x0 != x1) {
        const float t0 = (0.0f - x0) / (x1 - x0), t1 = (wf - x0) / (x1 - x0);
        const float ta = t0 < t1 ? t0 : t1, tb = t0 < t1 ? t1 : t0;
        if (ta > 0.0f && ta < 1.0f) {
            cuts[n++] = ta;
        }
        if (tb > 0.0f && tb < 1.0f) {
            cuts[n++] = tb;
        }
    }
    cuts[n++] = 1.0f;
    for (int i = 0; i + 1 < n; i++) {
        float ax = x0 + (x1 - x0) * cuts[i], ay = y0 + (y1 - y0) * cuts[i];
        float bx = x0 + (x1 - x0) * cuts[i + 1], by = y0 + (y1 - y0) * cuts[i + 1];
        const float mx = 0.5f * (ax + bx);
        if (mx <= 0.0f) {
            ax = bx = 0.0f;
        } else if (mx >= wf) {
            ax = bx = wf;
        } else {
            ax = ax < 0.0f ? 0.0f : (ax > wf ? wf : ax);
            bx = bx < 0.0f ? 0.0f : (bx > wf ? wf : bx);
        }
        covref_line_in(c, ax, ay, bx, by);
    }
}

/* Flattens one quad: splits at the middle until the control point is within tolerance of the chord. */
static void covref_quad(covref_acc *c, float x0, float y0, float cx, float cy, float x1, float y1, int depth)
{
    /* The curve deviates from its chord by half the distance of the control point from the chord midpoint (at most). */
    const float dx = cx - 0.5f * (x0 + x1), dy = cy - 0.5f * (y0 + y1);
    const float dev = 0.5f * SDL_sqrtf(dx * dx + dy * dy);
    if (dev <= COVREF_TOL || depth >= 18) {
        covref_line(c, x0, y0, x1, y1);
        return;
    }
    const float ax = 0.5f * (x0 + cx), ay = 0.5f * (y0 + cy);
    const float bx = 0.5f * (cx + x1), by = 0.5f * (cy + y1);
    const float mx = 0.5f * (ax + bx), my = 0.5f * (ay + by);
    covref_quad(c, x0, y0, ax, ay, mx, my, depth + 1);
    covref_quad(c, mx, my, bx, by, x1, y1, depth + 1);
}

/* Exact area coverage of a nonzero-filled outline made of quadratic curves, per pixel of a w x h grid.
 * curves: n quads (x0,y0, cx,cy, x1,y1) already in device px (y down). out: w*h floats in [0,1]. */
static void covref_raster(const float (*q)[6], uint32_t n, uint32_t w, uint32_t h, float *out)
{
    covref_acc c = {nullptr, w, h, w + 2};
    c.a = SDL_calloc((size_t)c.stride * (size_t)h, sizeof(float));
    if (c.a == nullptr) {
        SDL_memset(out, 0, (size_t)w * (size_t)h * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        covref_quad(&c, q[i][0], q[i][1], q[i][2], q[i][3], q[i][4], q[i][5], 0);
    }
    for (uint32_t y = 0; y < h; y++) {
        float acc = 0.0f;
        const float *row = c.a + (size_t)y * c.stride;
        for (uint32_t x = 0; x < w; x++) {
            acc += row[x];
            const float v = SDL_fabsf(acc);
            out[(size_t)y * w + x] = v > 1.0f ? 1.0f : v;
        }
    }
    SDL_free(c.a);
}
