#include "gpu/stroke.h"

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_STROKE_PI 3.14159265358979323846f
#define EMBI_STROKE_TOL_DEFAULT (1.0f / 64.0f)
#define EMBI_STROKE_MAX_DEPTH 10
#define EMBI_STROKE_MAX_OUT (1u << 21)
#define EMBI_STROKE_MAX_DASHES 1e5f
#define EMBI_STROKE_MAX_FLAT 256u
#define EMBI_STROKE_MAX_HW 1e8f
/* A piece turns by at most acos(0.92), about 23 degrees, to be offset as one quadratic. */
#define EMBI_STROKE_DOT_OK 0.92f
/* A piece that turns by less than this is not split any further for being tight. */
#define EMBI_STROKE_DOT_SMALL 0.99875f
/* The radius of curvature must stay above 1 / TIGHT half-widths for a piece to be a ribbon. */
#define EMBI_STROKE_TIGHT 0.8f

/* One segment of a subpath: a line or a quadratic. rjoin: the join after it is round whatever the
 * style says (the cusp of a curve that doubled back). */
typedef struct embi_seg {
    float x0, y0, cx, cy, x1, y1;
    bool line;
    bool rjoin;
} embi_seg;

typedef struct embi_dash_run {
    uint32_t first;
    uint32_t n;
    bool dot; /* a zero-length dash: a point with a tangent */
    float x, y, tx, ty;
} embi_dash_run;

enum { EMBI_ELEM_NONE = 0, EMBI_ELEM_RIBBON, EMBI_ELEM_FLAT };

typedef struct embi_stroker {
    embi_path *out;
    float hw;
    float tol;
    embi_stroke_cap cap;
    embi_stroke_join join;
    float miter_min; /* a miter is drawn when 1 + cos(turn) >= this */
    float arc_step;  /* the largest angle of one quadratic of a circular arc */
    uint32_t budget;
    EMBI_VEC(embi_qcurve) pc; /* scratch: the piece being built */
    EMBI_VEC(embi_qcurve) rl; /* ribbon, left side, forward */
    EMBI_VEC(embi_qcurve) rr; /* ribbon, right side, forward */
    EMBI_VEC(embi_seg) segs;
    EMBI_VEC(embi_seg) dsegs;
    EMBI_VEC(embi_dash_run) runs;
    EMBI_VEC(embi_seg) merged;
    EMBI_VEC(float) pat;
    /* The curve being drawn: what the last element of it ended with, and how it began. */
    int elem;
    float qx, qy, qdx, qdy;
    bool fset;
    float fdx, fdy;
} embi_stroker;

/* Small helpers ------------------------------------------------------------ */

static bool embi_stroke_finite(float x)
{
    return x - x == 0.0f;
}

static embi_qcurve embi_stroke_line(float x0, float y0, float x1, float y1)
{
    return (embi_qcurve){.x0 = x0,
                         .y0 = y0,
                         .cx = (x0 + x1) * 0.5f,
                         .cy = (y0 + y1) * 0.5f,
                         .x1 = x1,
                         .y1 = y1};
}

static void embi_stroke_pline(embi_stroker *S, float x0, float y0, float x1, float y1)
{
    embi_vec_push(&S->pc, embi_stroke_line(x0, y0, x1, y1));
}

/* Normalizes (x, y) in place. False when it has no usable length. */
static bool embi_stroke_norm(float *x, float *y)
{
    const float l = SDL_sqrtf(*x * *x + *y * *y);
    if (!(l > 1e-30f) || !embi_stroke_finite(l)) {
        return false;
    }
    *x /= l;
    *y /= l;
    return true;
}

/* Emission ----------------------------------------------------------------- */

/* Writes one closed chain of quadratics (each starts where the last ended, the last ends where the
 * first started) to the output, wound so that its signed area is negative: every piece of a stroke
 * winds the same way, so the nonzero rule yields their union. */
static void embi_stroke_emit(embi_stroker *S, const embi_qcurve *q, uint32_t n)
{
    if (n == 0 || S->budget == 0) {
        return;
    }
    if (n > S->budget) {
        S->budget = 0;
        return;
    }
    /* Signed area: the polygon of the end points plus 2/3 of each control triangle, relative to
     * the first point to keep cancellation small. */
    const double ox = q[0].x0;
    const double oy = q[0].y0;
    double a = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const double x0 = q[i].x0 - ox, y0 = q[i].y0 - oy;
        const double x1 = q[i].x1 - ox, y1 = q[i].y1 - oy;
        const double cx = q[i].cx - ox, cy = q[i].cy - oy;
        a += 0.5 * (x0 * y1 - x1 * y0);
        a += (1.0 / 3.0) * ((cx - x0) * (y1 - y0) - (cy - y0) * (x1 - x0));
    }
    if (!(a > 1e-12 || a < -1e-12) || a - a != 0.0) {
        return; /* no area, NaN or infinite */
    }
    S->budget -= n;
    if (a < 0.0) {
        embi_path_move_to(S->out, q[0].x0, q[0].y0);
        for (uint32_t i = 0; i < n; i++) {
            embi_path_quad_to(S->out, q[i].cx, q[i].cy, q[i].x1, q[i].y1);
        }
    } else {
        embi_path_move_to(S->out, q[n - 1u].x1, q[n - 1u].y1);
        for (uint32_t i = n; i-- > 0;) {
            embi_path_quad_to(S->out, q[i].cx, q[i].cy, q[i].x0, q[i].y0);
        }
    }
    embi_path_close(S->out);
}

static void embi_stroke_emit_pc(embi_stroker *S)
{
    embi_stroke_emit(S, S->pc.data, S->pc.len);
    S->pc.len = 0;
}

/* Appends the arc of radius hw around (cx, cy) that starts at unit vector (ux, uy) and turns by
 * sweep radians (any size up to a full turn) to pc, as quadratics whose control points sit at the
 * miter points of the tangents. */
static void embi_stroke_arc(embi_stroker *S, float cx, float cy, float ux, float uy, float sweep)
{
    const float a = SDL_fabsf(sweep);
    uint32_t n = (uint32_t)SDL_ceilf(a / S->arc_step);
    if (n < 1u) {
        n = 1u;
    }
    const float step = sweep / (float)n;
    const float ang0 = SDL_atan2f(uy, ux);
    float ax = ux, ay = uy;
    float px = cx + S->hw * ux, py = cy + S->hw * uy;
    for (uint32_t k = 1; k <= n; k++) {
        const float ang = ang0 + step * (float)k;
        const float bx = SDL_cosf(ang), by = SDL_sinf(ang);
        const float f = S->hw / (1.0f + ax * bx + ay * by);
        const embi_qcurve q = {.x0 = px,
                               .y0 = py,
                               .cx = cx + f * (ax + bx),
                               .cy = cy + f * (ay + by),
                               .x1 = cx + S->hw * bx,
                               .y1 = cy + S->hw * by};
        embi_vec_push(&S->pc, q);
        px = q.x1;
        py = q.y1;
        ax = bx;
        ay = by;
    }
}

/* A join at p between a segment that ends going d0 and one that starts going d1 (unit vectors). */
static void embi_stroke_do_join(embi_stroker *S, float px, float py, float d0x, float d0y,
                                float d1x, float d1y, embi_stroke_join type)
{
    const float cross = d0x * d1y - d0y * d1x;
    const float dot = d0x * d1x + d0y * d1y;
    /* The gap a small turn leaves is a triangle hw * phi wide at its far side: below a fiftieth of
     * the tolerance it is not worth drawing. */
    if (dot > 0.0f && S->hw * SDL_fabsf(cross) < S->tol * 0.02f) {
        return;
    }
    const float side = cross > 0.0f ? -1.0f : 1.0f;
    const float n0x = -d0y, n0y = d0x;
    const float n1x = -d1y, n1y = d1x;
    const float hw = S->hw;
    const float ax = px + side * hw * n0x, ay = py + side * hw * n0y;
    const float bx = px + side * hw * n1x, by = py + side * hw * n1y;
    S->pc.len = 0;
    if (type == EMBI_JOIN_MITER && 1.0f + dot >= S->miter_min) {
        const float k = side * hw / (1.0f + dot);
        const float mx = px + k * (n0x + n1x), my = py + k * (n0y + n1y);
        embi_stroke_pline(S, px, py, ax, ay);
        embi_stroke_pline(S, ax, ay, mx, my);
        embi_stroke_pline(S, mx, my, bx, by);
        embi_stroke_pline(S, bx, by, px, py);
    } else if (type == EMBI_JOIN_ROUND) {
        float sweep;
        if (SDL_fabsf(cross) < 1e-7f && dot < 0.0f) {
            sweep = side > 0.0f ? -EMBI_STROKE_PI : EMBI_STROKE_PI; /* the half disc ahead */
        } else {
            sweep = SDL_atan2f(cross, dot);
        }
        embi_stroke_pline(S, px, py, ax, ay);
        embi_stroke_arc(S, px, py, side * n0x, side * n0y, sweep);
        embi_stroke_pline(S, bx, by, px, py);
    } else {
        embi_stroke_pline(S, px, py, ax, ay);
        embi_stroke_pline(S, ax, ay, bx, by);
        embi_stroke_pline(S, bx, by, px, py);
    }
    embi_stroke_emit_pc(S);
}

/* A cap at p; (ex, ey) is the unit direction pointing away from the line. */
static void embi_stroke_do_cap(embi_stroker *S, float px, float py, float ex, float ey)
{
    if (S->cap == EMBI_CAP_BUTT) {
        return;
    }
    const float hw = S->hw;
    const float nx = -ey, ny = ex;
    const float ax = px + hw * nx, ay = py + hw * ny;
    const float bx = px - hw * nx, by = py - hw * ny;
    S->pc.len = 0;
    if (S->cap == EMBI_CAP_SQUARE) {
        const float fx = hw * ex, fy = hw * ey;
        embi_stroke_pline(S, ax, ay, ax + fx, ay + fy);
        embi_stroke_pline(S, ax + fx, ay + fy, bx + fx, by + fy);
        embi_stroke_pline(S, bx + fx, by + fy, bx, by);
        embi_stroke_pline(S, bx, by, ax, ay);
    } else {
        embi_stroke_arc(S, px, py, nx, ny, -EMBI_STROKE_PI);
        embi_stroke_pline(S, bx, by, ax, ay);
    }
    embi_stroke_emit_pc(S);
}

/* A zero-length subpath or dash: a disc for round caps, a square along (tx, ty) for square. */
static void embi_stroke_do_dot(embi_stroker *S, float px, float py, float tx, float ty)
{
    if (S->cap == EMBI_CAP_BUTT) {
        return;
    }
    if (!embi_stroke_norm(&tx, &ty)) {
        tx = 1.0f;
        ty = 0.0f;
    }
    const float hw = S->hw;
    S->pc.len = 0;
    if (S->cap == EMBI_CAP_SQUARE) {
        const float nx = -ty, ny = tx;
        const float ex = hw * tx, ey = hw * ty, fx = hw * nx, fy = hw * ny;
        embi_stroke_pline(S, px - ex - fx, py - ey - fy, px + ex - fx, py + ey - fy);
        embi_stroke_pline(S, px + ex - fx, py + ey - fy, px + ex + fx, py + ey + fy);
        embi_stroke_pline(S, px + ex + fx, py + ey + fy, px - ex + fx, py - ey + fy);
        embi_stroke_pline(S, px - ex + fx, py - ey + fy, px - ex - fx, py - ey - fy);
    } else {
        embi_stroke_arc(S, px, py, 1.0f, 0.0f, 2.0f * EMBI_STROKE_PI);
        /* Close the circle exactly. */
        S->pc.data[S->pc.len - 1u].x1 = S->pc.data[0].x0;
        S->pc.data[S->pc.len - 1u].y1 = S->pc.data[0].y0;
    }
    embi_stroke_emit_pc(S);
}

/* Bodies ------------------------------------------------------------------- */

static void embi_stroke_body_line(embi_stroker *S, float x0, float y0, float x1, float y1, float dx,
                                  float dy)
{
    const float hx = -dy * S->hw, hy = dx * S->hw;
    S->pc.len = 0;
    embi_stroke_pline(S, x0 + hx, y0 + hy, x1 + hx, y1 + hy);
    embi_stroke_pline(S, x1 + hx, y1 + hy, x1 - hx, y1 - hy);
    embi_stroke_pline(S, x1 - hx, y1 - hy, x0 - hx, y0 - hy);
    embi_stroke_pline(S, x0 - hx, y0 - hy, x0 + hx, y0 + hy);
    embi_stroke_emit_pc(S);
}

/* Draws the ribbon collected so far as one closed piece. */
static void embi_stroke_ribbon_flush(embi_stroker *S)
{
    if (S->rl.len == 0) {
        return;
    }
    S->pc.len = 0;
    for (uint32_t i = 0; i < S->rl.len; i++) {
        embi_vec_push(&S->pc, S->rl.data[i]);
    }
    const embi_qcurve *le = &S->rl.data[S->rl.len - 1u];
    const embi_qcurve *re = &S->rr.data[S->rr.len - 1u];
    embi_stroke_pline(S, le->x1, le->y1, re->x1, re->y1);
    for (uint32_t i = S->rr.len; i-- > 0;) {
        const embi_qcurve *r = &S->rr.data[i];
        const embi_qcurve q = {.x0 = r->x1, .y0 = r->y1, .cx = r->cx, .cy = r->cy, .x1 = r->x0,
                               .y1 = r->y0};
        embi_vec_push(&S->pc, q);
    }
    embi_stroke_pline(S, S->rr.data[0].x0, S->rr.data[0].y0, S->rl.data[0].x0, S->rl.data[0].y0);
    embi_stroke_emit_pc(S);
    S->rl.len = 0;
    S->rr.len = 0;
}

/* A quadratic in double precision, by parameter: P(t) = p + 2 l t + e t^2, velocity P'(t) / 2 =
 * l + e t. Pieces of a curve are intervals [ta, tb] of this one parametrization, so that tangents
 * stay exact however small a piece gets (chopping the float control points would leave the
 * tangents at the tip of a hairpin as noise). */
typedef struct embi_dq {
    double px, py, lx, ly, ex, ey;
} embi_dq;

static void embi_dq_at(const embi_dq *q, double t, double *x, double *y)
{
    *x = q->px + t * (2.0 * q->lx + t * q->ex);
    *y = q->py + t * (2.0 * q->ly + t * q->ey);
}

/* Unit tangent at t; false at a zero of the velocity. */
static bool embi_dq_dir(const embi_dq *q, double t, double *dx, double *dy)
{
    const double vx = q->lx + t * q->ex, vy = q->ly + t * q->ey;
    const double l = SDL_sqrt(vx * vx + vy * vy);
    if (!(l > 1e-300)) {
        return false;
    }
    *dx = vx / l;
    *dy = vy / l;
    return true;
}

/* A curve piece whose flattening is drawn as rectangles with round wedges between them. The wedges
 * also join the true tangents at its two ends (d0, d1) to the first and last chord: a piece that
 * turns a lot (the tip of a hairpin) is mostly such wedges. Pieces of one curve share their end
 * tangents, so there is nothing to join between pieces. */
static void embi_stroke_flat_piece(embi_stroker *S, const embi_dq *q, double ta, double tb,
                                   const double d0[2], const double d1[2])
{
    embi_stroke_ribbon_flush(S);
    if (S->elem == EMBI_ELEM_NONE) {
        S->fset = true;
        S->fdx = (float)d0[0];
        S->fdy = (float)d0[1];
    }
    const double h = tb - ta;
    const double dl = h * h * SDL_sqrt(q->ex * q->ex + q->ey * q->ey);
    double nf = SDL_ceil(SDL_sqrt(dl / (2.0 * (double)S->tol)));
    if (!(nf >= 1.0)) {
        nf = 1.0;
    }
    if (nf > (double)EMBI_STROKE_MAX_FLAT) {
        nf = (double)EMBI_STROKE_MAX_FLAT;
    }
    const uint32_t n = (uint32_t)nf;
    double px, py;
    embi_dq_at(q, ta, &px, &py);
    double ldx = d0[0], ldy = d0[1];
    const double skip = (double)S->tol * 0.01;
    for (uint32_t k = 1; k <= n; k++) {
        double x, y;
        embi_dq_at(q, k == n ? tb : ta + h * (double)k / nf, &x, &y);
        double dx = x - px, dy = y - py;
        const double len = SDL_sqrt(dx * dx + dy * dy);
        if (len <= skip) {
            continue;
        }
        dx /= len;
        dy /= len;
        embi_stroke_do_join(S, (float)px, (float)py, (float)ldx, (float)ldy, (float)dx, (float)dy,
                            EMBI_JOIN_ROUND);
        embi_stroke_body_line(S, (float)px, (float)py, (float)x, (float)y, (float)dx, (float)dy);
        ldx = dx;
        ldy = dy;
        px = x;
        py = y;
    }
    embi_stroke_do_join(S, (float)px, (float)py, (float)ldx, (float)ldy, (float)d1[0],
                        (float)d1[1], EMBI_JOIN_ROUND);
    S->elem = EMBI_ELEM_FLAT;
    S->qdx = (float)d1[0];
    S->qdy = (float)d1[1];
}

/* The piece [ta, tb] of a quadratic: a ribbon when its offsets are good quadratics, else split,
 * else flattened. */
static void embi_stroke_quad_piece(embi_stroker *S, const embi_dq *q, double ta, double tb,
                                   int depth)
{
    const double hw = (double)S->hw;
    const double tol = (double)S->tol;
    double d0[2], d1[2];
    const bool ok0 = embi_dq_dir(q, ta, &d0[0], &d0[1]);
    const bool ok1 = embi_dq_dir(q, tb, &d1[0], &d1[1]);
    if (!ok0 && !ok1) {
        return;
    }
    if (!ok0) {
        d0[0] = d1[0];
        d0[1] = d1[1];
    }
    if (!ok1) {
        d1[0] = d0[0];
        d1[1] = d0[1];
    }
    const double dot = d0[0] * d1[0] + d0[1] * d1[1];
    const double h = tb - ta;
    /* Speed |v(t)| is smallest at some t in the piece; the curvature there is the largest:
     * kappa = cross(l, e) / (2 |v|^3). */
    const double kk = SDL_fabs(q->lx * q->ey - q->ly * q->ex);
    const double ee = q->ex * q->ex + q->ey * q->ey;
    double tm = ta;
    if (ee > 0.0) {
        tm = -(q->lx * q->ex + q->ly * q->ey) / ee;
        tm = tm < ta ? ta : (tm > tb ? tb : tm);
    }
    const double vmx = q->lx + tm * q->ex, vmy = q->ly + tm * q->ey;
    const double m = SDL_sqrt(vmx * vmx + vmy * vmy);
    const bool tight = !(kk * hw < (double)EMBI_STROKE_TIGHT * 2.0 * m * m * m);
    const bool angle_ok = dot >= (double)EMBI_STROKE_DOT_OK;

    if (angle_ok && !tight) {
        const double n0x = -d0[1], n0y = d0[0], n1x = -d1[1], n1y = d1[0];
        const double f = hw / (1.0 + dot);
        double pax, pay, pbx, pby;
        embi_dq_at(q, ta, &pax, &pay);
        embi_dq_at(q, tb, &pbx, &pby);
        const double cx = pax + h * (q->lx + ta * q->ex);
        const double cy = pay + h * (q->ly + ta * q->ey);
        double o[2][6]; /* per side: start, control, end */
        for (int sd = 0; sd < 2; sd++) {
            const double s = sd == 0 ? 1.0 : -1.0;
            o[sd][0] = pax + s * hw * n0x;
            o[sd][1] = pay + s * hw * n0y;
            o[sd][2] = cx + s * f * (n0x + n1x);
            o[sd][3] = cy + s * f * (n0y + n1y);
            o[sd][4] = pbx + s * hw * n1x;
            o[sd][5] = pby + s * hw * n1y;
        }
        bool good = true;
        static const double us[3] = {0.25, 0.5, 0.75};
        for (int i = 0; i < 3 && good; i++) {
            const double u = us[i], mu = 1.0 - u;
            const double t = ta + h * u;
            double bx, by, tx, ty;
            embi_dq_at(q, t, &bx, &by);
            if (!embi_dq_dir(q, t, &tx, &ty)) {
                good = false;
                break;
            }
            for (int sd = 0; sd < 2; sd++) {
                const double s = sd == 0 ? 1.0 : -1.0;
                const double qx = mu * mu * o[sd][0] + 2.0 * mu * u * o[sd][2] + u * u * o[sd][4];
                const double qy = mu * mu * o[sd][1] + 2.0 * mu * u * o[sd][3] + u * u * o[sd][5];
                const double dx = qx - (bx + s * hw * -ty), dy = qy - (by + s * hw * tx);
                if (dx * dx + dy * dy > tol * tol * 0.25) {
                    good = false;
                    break;
                }
            }
        }
        if (good) {
            if (S->elem == EMBI_ELEM_NONE) {
                S->fset = true;
                S->fdx = (float)d0[0];
                S->fdy = (float)d0[1];
            }
            /* Left side forward; the start of a piece is the end of the last one. */
            embi_qcurve ql = {.x0 = (float)o[0][0], .y0 = (float)o[0][1], .cx = (float)o[0][2],
                              .cy = (float)o[0][3], .x1 = (float)o[0][4], .y1 = (float)o[0][5]};
            embi_qcurve qr = {.x0 = (float)o[1][0], .y0 = (float)o[1][1], .cx = (float)o[1][2],
                              .cy = (float)o[1][3], .x1 = (float)o[1][4], .y1 = (float)o[1][5]};
            if (S->rl.len > 0) {
                ql.x0 = S->rl.data[S->rl.len - 1u].x1;
                ql.y0 = S->rl.data[S->rl.len - 1u].y1;
                qr.x0 = S->rr.data[S->rr.len - 1u].x1;
                qr.y0 = S->rr.data[S->rr.len - 1u].y1;
            }
            embi_vec_push(&S->rl, ql);
            embi_vec_push(&S->rr, qr);
            S->elem = EMBI_ELEM_RIBBON;
            S->qdx = (float)d1[0];
            S->qdy = (float)d1[1];
            return;
        }
    }
    const bool small = dot >= (double)EMBI_STROKE_DOT_SMALL;
    if (depth < EMBI_STROKE_MAX_DEPTH && !(tight && small)) {
        const double tmid = (ta + tb) * 0.5;
        embi_stroke_quad_piece(S, q, ta, tmid, depth + 1);
        embi_stroke_quad_piece(S, q, tmid, tb, depth + 1);
        return;
    }
    embi_stroke_flat_piece(S, q, ta, tb, d0, d1);
}

/* Draws the body of a quadratic segment. Returns false when nothing was drawn (a degenerate
 * segment). sd and ed receive the directions it starts and ends going. */
static bool embi_stroke_body_quad(embi_stroker *S, const embi_seg *s, float sd[2], float ed[2])
{
    S->elem = EMBI_ELEM_NONE;
    S->fset = false;
    S->rl.len = 0;
    S->rr.len = 0;
    const embi_dq q = {.px = s->x0,
                       .py = s->y0,
                       .lx = (double)s->cx - (double)s->x0,
                       .ly = (double)s->cy - (double)s->y0,
                       .ex = (double)s->x0 - 2.0 * (double)s->cx + (double)s->x1,
                       .ey = (double)s->y0 - 2.0 * (double)s->cy + (double)s->y1};
    embi_stroke_quad_piece(S, &q, 0.0, 1.0, 0);
    embi_stroke_ribbon_flush(S);
    if (!S->fset) {
        return false;
    }
    sd[0] = S->fdx;
    sd[1] = S->fdy;
    ed[0] = S->qdx;
    ed[1] = S->qdy;
    return true;
}

static bool embi_stroke_seg_tiny(const embi_stroker *S, const embi_seg *s)
{
    const float a = SDL_fabsf(s->cx - s->x0) + SDL_fabsf(s->cy - s->y0);
    const float b = SDL_fabsf(s->x1 - s->cx) + SDL_fabsf(s->y1 - s->cy);
    return !(a + b >= S->tol * 0.05f);
}

/* Collapses runs of tiny segments into chords. A run is closed once its chord reaches
 * tol * 0.05, so the dropped length per gap stays below that, however many tiny segments a
 * dense polyline has. */
static void embi_stroke_merge_tiny(embi_stroker *S)
{
    const uint32_t n = S->segs.len;
    if (n == 0) {
        return;
    }
    const float thr = S->tol * 0.05f;
    const embi_seg first = S->segs.data[0];
    uint32_t w = 0;
    bool pend = false;
    float px = 0.0f, py = 0.0f, ex = 0.0f, ey = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const embi_seg s = S->segs.data[i];
        if (embi_stroke_seg_tiny(S, &s)) {
            if (!pend) {
                px = s.x0;
                py = s.y0;
                pend = true;
            }
            ex = s.x1;
            ey = s.y1;
            if (SDL_fabsf(ex - px) + SDL_fabsf(ey - py) >= thr) {
                const embi_seg c = {.x0 = px, .y0 = py, .cx = (px + ex) * 0.5f,
                                    .cy = (py + ey) * 0.5f, .x1 = ex, .y1 = ey, .line = true,
                                    .rjoin = false};
                S->segs.data[w++] = c;
                pend = false;
            }
            continue;
        }
        if (pend) {
            if (SDL_fabsf(ex - px) + SDL_fabsf(ey - py) > 0.0f) {
                const embi_seg c = {.x0 = px, .y0 = py, .cx = (px + ex) * 0.5f,
                                    .cy = (py + ey) * 0.5f, .x1 = ex, .y1 = ey, .line = true,
                                    .rjoin = false};
                S->segs.data[w++] = c;
            }
            pend = false;
        }
        S->segs.data[w++] = s;
    }
    if (pend && SDL_fabsf(ex - px) + SDL_fabsf(ey - py) > 0.0f) {
        const embi_seg c = {.x0 = px, .y0 = py, .cx = (px + ex) * 0.5f, .cy = (py + ey) * 0.5f,
                            .x1 = ex, .y1 = ey, .line = true, .rjoin = false};
        S->segs.data[w++] = c;
    }
    if (w == 0) {
        S->segs.data[w++] = first; /* a zero-length subpath keeps its dot */
    }
    S->segs.len = w;
}

/* Collinear runs ------------------------------------------------------------ */

/* The direction of a step of v along the line; a step below eps is rounding noise, not a direction. */
static float embi_stroke_sgn(float v, float eps)
{
    return v > eps ? 1.0f : (v < -eps ? -1.0f : 0.0f);
}

/* A half disc at a turn at s (moving cur along the line before it) only adds to the bar where it reaches past it. */
static bool embi_stroke_pokes(const embi_stroker *S, float s, float cur, float lo, float hi)
{
    return cur > 0.0f ? s + S->hw > hi : s - S->hw < lo;
}

/* Walks segments [i, j) that lie on the line through (ax, ay) along u, in 1-D (s = position along u).
 * Counts the turns (the curve reverses along the line), finds the extent [lo, hi] and the directions the run
 * starts and ends going. With emit, a round join's half disc is drawn at each turn that can reach past the bar. */
static uint32_t embi_stroke_walk_run(embi_stroker *S, const embi_seg *sg, uint32_t i, uint32_t j, float ax, float ay,
                                     float ux, float uy, bool emit, float *lo, float *hi, float *first,
                                     float *last)
{
    float cur = 0.0f, mn = 0.0f, mx = 0.0f;
    uint32_t turns = 0;
    const float eps = S->tol * 0.05f;
    bool prj = false; /* the segment before this one ended in a forced round join (a curve's cusp) */
    *first = 0.0f;
    for (uint32_t k = i; k < j; k++) {
        const embi_seg *s = &sg[k];
        if (embi_stroke_seg_tiny(S, s)) {
            continue; /* the chain skips these too */
        }
        const float p0 = (s->x0 - ax) * ux + (s->y0 - ay) * uy;
        const float pc = (s->cx - ax) * ux + (s->cy - ay) * uy;
        const float p1 = (s->x1 - ax) * ux + (s->y1 - ay) * uy;
        float d0 = pc - p0, d1 = p1 - pc;
        float s0 = embi_stroke_sgn(d0, eps), s1 = embi_stroke_sgn(d1, eps);
        if (s0 == 0.0f) {
            s0 = embi_stroke_sgn(p1 - p0, eps);
        }
        if (s1 == 0.0f) {
            s1 = s0;
        }
        mn = SDL_min(mn, SDL_min(p0, p1));
        mx = SDL_max(mx, SDL_max(p0, p1));
        if (s0 != 0.0f && cur != 0.0f && s0 != cur) {
            turns++;
            if (emit && (S->join == EMBI_JOIN_ROUND || prj) && embi_stroke_pokes(S, p0, cur, *lo, *hi)) {
                embi_stroke_do_join(S, ax + p0 * ux, ay + p0 * uy, cur * ux, cur * uy, -cur * ux, -cur * uy,
                                    EMBI_JOIN_ROUND);
            }
        }
        if (s0 != 0.0f) {
            if (*first == 0.0f) {
                *first = s0;
            }
            cur = s0;
        }
        if (d0 > eps ? d1 < -eps : (d0 < -eps && d1 > eps)) {
            const float t = d0 / (d0 - d1);
            const float e = (1.0f - t) * (1.0f - t) * p0 + 2.0f * t * (1.0f - t) * pc + t * t * p1;
            mn = SDL_min(mn, e);
            mx = SDL_max(mx, e);
            turns++;
            if (emit && embi_stroke_pokes(S, e, cur, *lo, *hi)) { /* a curve that doubles back has a round cusp */
                embi_stroke_do_join(S, ax + e * ux, ay + e * uy, cur * ux, cur * uy, -cur * ux, -cur * uy,
                                    EMBI_JOIN_ROUND);
            }
            cur = -cur;
        }
        if (s1 != 0.0f) {
            cur = s1;
        }
        prj = s->rjoin;
    }
    *lo = mn;
    *hi = mx;
    *last = cur;
    return turns;
}

/* A run of segments from sg[i] on one straight line, usually one that reverses along it (a cubic whose control points are
 * collinear with its ends, a line out and back): its stroke is one bar from the lowest to the highest point the
 * run reaches, plus round join discs at the turns. Drawn as overlapping pieces the doubled antialiased edges would
 * add up to a full row. A run of several segments that does not reverse (a straight curve in pieces) is one bar as
 * well: the seams of abutting pieces would show in the edge rows. Returns the end of the run (i when it is not one); the run's ends and the directions it
 * starts and ends going are in out. */
typedef struct embi_fold {
    float x0, y0, x1, y1;
    float sd[2], ed[2];
    uint32_t scanned; /* end of the collinear stretch examined, whether or not it reversed */
} embi_fold;

static uint32_t embi_stroke_fold_run(embi_stroker *S, const embi_seg *sg, uint32_t i, uint32_t n, embi_fold *out)
{
    const embi_seg *a = &sg[i];
    out->scanned = i + 1;
    const float eps = S->tol * 0.25f;
    float ux = a->x1 - a->x0, uy = a->y1 - a->y0;
    if (!embi_stroke_norm(&ux, &uy)) {
        ux = a->cx - a->x0;
        uy = a->cy - a->y0;
        if (!embi_stroke_norm(&ux, &uy)) {
            return i;
        }
    }
    uint32_t j = i;
    while (j < n) {
        const embi_seg *s = &sg[j];
        const float px[3] = {s->x0, s->cx, s->x1}, py[3] = {s->y0, s->cy, s->y1};
        bool on = true;
        for (int q = 0; q < 3; q++) {
            if (SDL_fabsf((px[q] - a->x0) * -uy + (py[q] - a->y0) * ux) > eps) {
                on = false;
            }
        }
        if (!on) {
            break;
        }
        j++;
    }
    if (j - i < 1) {
        return i;
    }
    out->scanned = j;
    float lo, hi, first, last;
    const uint32_t turns = embi_stroke_walk_run(S, sg, i, j, a->x0, a->y0, ux, uy, false, &lo, &hi, &first, &last);
    /* A single straight segment is drawn as it is; so is any other that neither reverses nor has company. */
    if ((turns == 0 && j - i < 2) || first == 0.0f || last == 0.0f) {
        return i;
    }
    embi_stroke_walk_run(S, sg, i, j, a->x0, a->y0, ux, uy, true, &lo, &hi, &first, &last);
    if (hi - lo > 0.0f) {
        embi_stroke_body_line(S, a->x0 + lo * ux, a->y0 + lo * uy, a->x0 + hi * ux, a->y0 + hi * uy, ux, uy);
    }
    out->x0 = a->x0;
    out->y0 = a->y0;
    out->x1 = sg[j - 1].x1;
    out->y1 = sg[j - 1].y1;
    out->sd[0] = first * ux;
    out->sd[1] = first * uy;
    out->ed[0] = last * ux;
    out->ed[1] = last * uy;
    return j;
}

/* Strokes a chain of segments as one subpath: bodies, joins, caps. */
static void embi_stroke_chain(embi_stroker *S, const embi_seg *sg, uint32_t n, bool closed)
{
    bool have = false;
    float fx = 0.0f, fy = 0.0f, fd[2] = {1.0f, 0.0f};
    float lx = 0.0f, ly = 0.0f, ld[2] = {1.0f, 0.0f};
    bool lr = false;
    uint32_t nofold = 0; /* segments before this index are known not to start a reversing run */
    for (uint32_t i = 0; i < n; i++) {
        const embi_seg *s = &sg[i];
        if (embi_stroke_seg_tiny(S, s)) {
            continue;
        }
        float sd[2], ed[2];
        float x0 = s->x0, y0 = s->y0, x1 = s->x1, y1 = s->y1;
        bool rj = s->rjoin;
        embi_fold fo = {0};
        /* A collinear run is not merged across a closed subpath's closing point: the run starts and ends there. */
        uint32_t j = i >= nofold ? embi_stroke_fold_run(S, sg, i, n, &fo) : i;
        if (j > i) {
            x0 = fo.x0;
            y0 = fo.y0;
            x1 = fo.x1;
            y1 = fo.y1;
            sd[0] = fo.sd[0];
            sd[1] = fo.sd[1];
            ed[0] = fo.ed[0];
            ed[1] = fo.ed[1];
            rj = sg[j - 1].rjoin;
            i = j - 1;
        } else {
            if (i >= nofold) {
                /* a straight stretch without a reversal is not tried again from each of its segments */
                nofold = fo.scanned > i ? fo.scanned : i + 1;
            }
            if (s->line) {
                float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
                if (!embi_stroke_norm(&dx, &dy)) {
                    continue;
                }
                embi_stroke_body_line(S, s->x0, s->y0, s->x1, s->y1, dx, dy);
                sd[0] = ed[0] = dx;
                sd[1] = ed[1] = dy;
            } else if (!embi_stroke_body_quad(S, s, sd, ed)) {
                continue;
            }
        }
        if (have) {
            embi_stroke_do_join(S, x0, y0, ld[0], ld[1], sd[0], sd[1], lr ? EMBI_JOIN_ROUND : S->join);
        } else {
            fx = x0;
            fy = y0;
            fd[0] = sd[0];
            fd[1] = sd[1];
            have = true;
        }
        ld[0] = ed[0];
        ld[1] = ed[1];
        lx = x1;
        ly = y1;
        lr = rj;
    }
    if (!have) {
        if (n > 0) {
            embi_stroke_do_dot(S, sg[0].x0, sg[0].y0, 1.0f, 0.0f);
        }
        return;
    }
    if (closed) {
        embi_stroke_do_join(S, fx, fy, ld[0], ld[1], fd[0], fd[1], lr ? EMBI_JOIN_ROUND : S->join);
    } else {
        embi_stroke_do_cap(S, fx, fy, -fd[0], -fd[1]);
        embi_stroke_do_cap(S, lx, ly, ld[0], ld[1]);
    }
}

/* Segments of a subpath ---------------------------------------------------- */

static void embi_stroke_add_line(embi_stroker *S, float x0, float y0, float x1, float y1,
                                 bool rjoin)
{
    const embi_seg s = {.x0 = x0, .y0 = y0, .cx = (x0 + x1) * 0.5f, .cy = (y0 + y1) * 0.5f,
                        .x1 = x1, .y1 = y1, .line = true, .rjoin = rjoin};
    embi_vec_push(&S->segs, s);
}

/* Turns the curves of one subpath into segments: lines stay lines, a quadratic whose control
 * points are collinear becomes a line, or two lines with a round join where it doubles back. */
static void embi_stroke_build(embi_stroker *S, const embi_qcurve *q, uint32_t n)
{
    S->segs.len = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *c = &q[i];
        const float l0x = c->cx - c->x0, l0y = c->cy - c->y0;
        const float l1x = c->x1 - c->cx, l1y = c->y1 - c->cy;
        const float len0 = SDL_sqrtf(l0x * l0x + l0y * l0y);
        const float len1 = SDL_sqrtf(l1x * l1x + l1y * l1y);
        const float cross = l0x * l1y - l0y * l1x;
        const float dot = l0x * l1x + l0y * l1y;
        if (SDL_fabsf(cross) <= 1e-6f * len0 * len1 || !(len0 > 0.0f) || !(len1 > 0.0f)) {
            if (dot >= 0.0f || !(len0 > 0.0f) || !(len1 > 0.0f)) {
                embi_stroke_add_line(S, c->x0, c->y0, c->x1, c->y1, false);
            } else {
                const float t = len0 / (len0 + len1);
                const float mt = 1.0f - t;
                const float xx = mt * mt * c->x0 + 2.0f * mt * t * c->cx + t * t * c->x1;
                const float yy = mt * mt * c->y0 + 2.0f * mt * t * c->cy + t * t * c->y1;
                embi_stroke_add_line(S, c->x0, c->y0, xx, yy, true);
                embi_stroke_add_line(S, xx, yy, c->x1, c->y1, false);
            }
            continue;
        }
        const embi_seg s = {.x0 = c->x0, .y0 = c->y0, .cx = c->cx, .cy = c->cy, .x1 = c->x1,
                            .y1 = c->y1, .line = false, .rjoin = false};
        embi_vec_push(&S->segs, s);
    }
}

/* Dashes ------------------------------------------------------------------- */

static const float embi_gl_x[3] = {0.0f, 0.5384693101056831f, 0.9061798459386640f};
static const float embi_gl_w[3] = {0.5688888888888889f, 0.4786286704993665f, 0.2369268850561891f};

static float embi_stroke_speed(const embi_seg *s, float t)
{
    const float l0x = s->cx - s->x0, l0y = s->cy - s->y0;
    const float l1x = s->x1 - s->cx, l1y = s->y1 - s->cy;
    const float vx = l0x + t * (l1x - l0x), vy = l0y + t * (l1y - l0y);
    return 2.0f * SDL_sqrtf(vx * vx + vy * vy);
}

/* Arc length between parameters t0 and t1. */
static float embi_stroke_len(const embi_seg *s, float t0, float t1)
{
    if (s->line) {
        const float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
        return SDL_sqrtf(dx * dx + dy * dy) * (t1 - t0);
    }
    float sum = 0.0f;
    const int parts = 4;
    for (int p = 0; p < parts; p++) {
        const float a = t0 + (t1 - t0) * (float)p / (float)parts;
        const float b = t0 + (t1 - t0) * (float)(p + 1) / (float)parts;
        const float mid = (a + b) * 0.5f, half = (b - a) * 0.5f;
        float acc = embi_gl_w[0] * embi_stroke_speed(s, mid);
        for (int k = 1; k < 3; k++) {
            acc += embi_gl_w[k] * (embi_stroke_speed(s, mid - half * embi_gl_x[k]) +
                                   embi_stroke_speed(s, mid + half * embi_gl_x[k]));
        }
        sum += acc * half;
    }
    return sum;
}

/* The parameter at which the arc length from t_from reaches dist (dist < the length left). */
static float embi_stroke_param(const embi_seg *s, float t_from, float dist, float total_len)
{
    if (s->line) {
        return t_from + dist / total_len;
    }
    float lo = t_from, hi = 1.0f;
    const float rest = embi_stroke_len(s, t_from, 1.0f);
    float t = rest > 0.0f ? t_from + (1.0f - t_from) * dist / rest : t_from;
    for (int it = 0; it < 30; it++) {
        const float f = embi_stroke_len(s, t_from, t) - dist;
        if (SDL_fabsf(f) < 1e-5f * (1.0f + dist)) {
            break;
        }
        if (f > 0.0f) {
            hi = t;
        } else {
            lo = t;
        }
        const float sp = embi_stroke_speed(s, t);
        float nt = sp > 1e-12f ? t - f / sp : -1.0f;
        if (!(nt > lo && nt < hi)) {
            nt = (lo + hi) * 0.5f;
        }
        t = nt;
    }
    return t;
}

static float embi_stroke_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* The part of s between parameters t0 and t1 (0 <= t0 < t1 <= 1). */
static embi_seg embi_stroke_sub(const embi_seg *s, float t0, float t1)
{
    embi_seg r = *s;
    r.rjoin = t1 >= 1.0f ? s->rjoin : false;
    if (t0 <= 0.0f && t1 >= 1.0f) {
        return r;
    }
    if (s->line) {
        r.x0 = embi_stroke_lerp(s->x0, s->x1, t0);
        r.y0 = embi_stroke_lerp(s->y0, s->y1, t0);
        r.x1 = embi_stroke_lerp(s->x0, s->x1, t1);
        r.y1 = embi_stroke_lerp(s->y0, s->y1, t1);
        r.cx = (r.x0 + r.x1) * 0.5f;
        r.cy = (r.y0 + r.y1) * 0.5f;
        return r;
    }
    float ax = s->x0, ay = s->y0, bx = s->cx, by = s->cy, ex = s->x1, ey = s->y1;
    if (t1 < 1.0f) { /* keep [0, t1] */
        const float a1x = embi_stroke_lerp(ax, bx, t1), a1y = embi_stroke_lerp(ay, by, t1);
        const float b1x = embi_stroke_lerp(bx, ex, t1), b1y = embi_stroke_lerp(by, ey, t1);
        bx = a1x;
        by = a1y;
        ex = embi_stroke_lerp(a1x, b1x, t1);
        ey = embi_stroke_lerp(a1y, b1y, t1);
    }
    if (t0 > 0.0f) { /* keep [t0 / t1, 1] of what is left */
        const float u = t1 < 1.0f ? t0 / t1 : t0;
        const float q0x = embi_stroke_lerp(ax, bx, u), q0y = embi_stroke_lerp(ay, by, u);
        const float q1x = embi_stroke_lerp(bx, ex, u), q1y = embi_stroke_lerp(by, ey, u);
        ax = embi_stroke_lerp(q0x, q1x, u);
        ay = embi_stroke_lerp(q0y, q1y, u);
        bx = q1x;
        by = q1y;
    }
    r.x0 = ax;
    r.y0 = ay;
    r.cx = bx;
    r.cy = by;
    r.x1 = ex;
    r.y1 = ey;
    return r;
}

/* Point and unit tangent at parameter t. */
static void embi_stroke_at(const embi_seg *s, float t, float *x, float *y, float *tx, float *ty)
{
    const float mt = 1.0f - t;
    if (s->line) {
        *x = embi_stroke_lerp(s->x0, s->x1, t);
        *y = embi_stroke_lerp(s->y0, s->y1, t);
        *tx = s->x1 - s->x0;
        *ty = s->y1 - s->y0;
    } else {
        *x = mt * mt * s->x0 + 2.0f * mt * t * s->cx + t * t * s->x1;
        *y = mt * mt * s->y0 + 2.0f * mt * t * s->cy + t * t * s->y1;
        *tx = mt * (s->cx - s->x0) + t * (s->x1 - s->cx);
        *ty = mt * (s->cy - s->y0) + t * (s->y1 - s->cy);
        if (!(*tx * *tx + *ty * *ty > 0.0f)) {
            *tx = s->x1 - s->x0;
            *ty = s->y1 - s->y0;
        }
    }
    if (!embi_stroke_norm(tx, ty)) {
        *tx = 1.0f;
        *ty = 0.0f;
    }
}

typedef struct embi_dash_state {
    uint32_t idx;
    float rem;
    bool on;
    bool run_open;
} embi_dash_state;

static void embi_dash_close(embi_stroker *S, embi_dash_state *ds)
{
    if (ds->run_open) {
        S->runs.data[S->runs.len - 1u].n = S->dsegs.len - S->runs.data[S->runs.len - 1u].first;
        ds->run_open = false;
    }
}

/* Moves to the next entry of the pattern at the point (x, y) going (tx, ty), emitting a dot for a
 * zero-length dash on the way. */
static void embi_dash_advance(embi_stroker *S, embi_dash_state *ds, float x, float y, float tx,
                              float ty)
{
    const uint32_t m = S->pat.len;
    for (;;) {
        ds->idx = (ds->idx + 1u) % m;
        ds->rem = S->pat.data[ds->idx];
        ds->on = !ds->on;
        if (ds->on && ds->rem <= 0.0f) {
            const embi_dash_run r = {.first = S->dsegs.len, .n = 0, .dot = true, .x = x, .y = y,
                                     .tx = tx, .ty = ty};
            embi_vec_push(&S->runs, r);
        }
        if (ds->rem > 0.0f) {
            return;
        }
    }
}

/* Strokes one subpath with the dash pattern in S->pat. */
static void embi_stroke_dashed(embi_stroker *S, float offset, bool closed)
{
    const uint32_t m = S->pat.len;
    float sum = 0.0f;
    float ctrl = 0.0f;
    for (uint32_t i = 0; i < m; i++) {
        sum += S->pat.data[i];
    }
    for (uint32_t i = 0; i < S->segs.len; i++) {
        const embi_seg *s = &S->segs.data[i];
        ctrl += SDL_fabsf(s->cx - s->x0) + SDL_fabsf(s->cy - s->y0) + SDL_fabsf(s->x1 - s->cx) +
                SDL_fabsf(s->y1 - s->cy);
    }
    if (ctrl / sum * (float)m > EMBI_STROKE_MAX_DASHES) {
        embi_stroke_chain(S, S->segs.data, S->segs.len, closed);
        return;
    }
    S->dsegs.len = 0;
    S->runs.len = 0;
    embi_dash_state ds = {.idx = 0, .rem = 0.0f, .on = true, .run_open = false};
    float off = SDL_fmodf(offset, sum);
    if (off < 0.0f) {
        off += sum;
    }
    while (off > S->pat.data[ds.idx] || (off == S->pat.data[ds.idx] && off > 0.0f)) {
        off -= S->pat.data[ds.idx];
        ds.idx = (ds.idx + 1u) % m;
        ds.on = !ds.on;
    }
    ds.rem = S->pat.data[ds.idx] - off;
    const bool start_on = ds.on && ds.rem > 0.0f;
    if (ds.on && ds.rem <= 0.0f && S->segs.len > 0) {
        const embi_seg *s0 = &S->segs.data[0];
        float x, y, tx, ty;
        embi_stroke_at(s0, 0.0f, &x, &y, &tx, &ty);
        const embi_dash_run r = {.first = 0, .n = 0, .dot = true, .x = x, .y = y, .tx = tx, .ty = ty};
        embi_vec_push(&S->runs, r);
        embi_dash_advance(S, &ds, x, y, tx, ty);
    }
    const bool first_at_start = start_on;

    for (uint32_t i = 0; i < S->segs.len; i++) {
        const embi_seg *s = &S->segs.data[i];
        if (embi_stroke_seg_tiny(S, s)) {
            continue;
        }
        const float total = embi_stroke_len(s, 0.0f, 1.0f);
        if (!(total > 0.0f)) {
            continue;
        }
        float t = 0.0f, left = total;
        while (left > 0.0f) {
            const float step = ds.rem < left ? ds.rem : left;
            const float tb = step >= left ? 1.0f : embi_stroke_param(s, t, step, total);
            if (ds.on && tb > t) {
                if (!ds.run_open) {
                    const embi_dash_run r = {.first = S->dsegs.len, .n = 0, .dot = false};
                    embi_vec_push(&S->runs, r);
                    ds.run_open = true;
                }
                const embi_seg sub = embi_stroke_sub(s, t, tb);
                embi_vec_push(&S->dsegs, sub);
            }
            ds.rem -= step;
            left -= step;
            t = tb;
            if (ds.rem <= 0.0f) {
                if (ds.on) {
                    embi_dash_close(S, &ds);
                }
                float x, y, tx, ty;
                embi_stroke_at(s, t, &x, &y, &tx, &ty);
                embi_dash_advance(S, &ds, x, y, tx, ty);
            }
            if (S->runs.len > (uint32_t)EMBI_STROKE_MAX_DASHES * 2u) {
                break;
            }
        }
    }
    const bool end_open = ds.run_open;
    embi_dash_close(S, &ds);

    uint32_t first_run = 0;
    uint32_t last_run = S->runs.len; /* one past */
    if (closed && first_at_start && end_open && S->runs.len > 0 && !S->runs.data[0].dot) {
        if (S->runs.len == 1u) {
            embi_stroke_chain(S, S->segs.data, S->segs.len, true);
            return;
        }
        /* The last dash continues into the first one across the start of the subpath. */
        const embi_dash_run lr = S->runs.data[S->runs.len - 1u];
        const embi_dash_run fr = S->runs.data[0];
        S->merged.len = 0;
        for (uint32_t i = 0; i < lr.n; i++) {
            embi_vec_push(&S->merged, S->dsegs.data[lr.first + i]);
        }
        for (uint32_t i = 0; i < fr.n; i++) {
            embi_vec_push(&S->merged, S->dsegs.data[fr.first + i]);
        }
        embi_stroke_chain(S, S->merged.data, S->merged.len, false);
        first_run = 1;
        last_run = S->runs.len - 1u;
    }
    for (uint32_t k = first_run; k < last_run; k++) {
        const embi_dash_run r = S->runs.data[k];
        if (r.dot) {
            embi_stroke_do_dot(S, r.x, r.y, r.tx, r.ty);
        } else if (r.n > 0) {
            embi_stroke_chain(S, &S->dsegs.data[r.first], r.n, false);
        }
    }
}

/* True when the dash pattern, after the offset, starts on an "on" entry of zero length: the only
 * case where a zero-length subpath draws a dot. */
static bool embi_dash_starts_on_zero(const embi_stroker *S, float offset)
{
    const uint32_t m = S->pat.len;
    float sum = 0.0f;
    for (uint32_t i = 0; i < m; i++) {
        sum += S->pat.data[i];
    }
    float off = SDL_fmodf(offset, sum);
    if (off < 0.0f) {
        off += sum;
    }
    uint32_t idx = 0;
    bool on = true;
    while (off > S->pat.data[idx] || (off == S->pat.data[idx] && off > 0.0f)) {
        off -= S->pat.data[idx];
        idx = (idx + 1u) % m;
        on = !on;
    }
    return on && S->pat.data[idx] - off <= 0.0f;
}

/* Entry points ------------------------------------------------------------- */

/* The width the stroke is made with (0 when it draws nothing): the style's, raised to the hairline floor. *coverage gets the
 * alpha that makes up for the raise (1 without one). */
static float embi_stroke_width(const embi_stroke_style *st, float *coverage)
{
    if (coverage != nullptr) {
        *coverage = 1.0f;
    }
    float w = st->width;
    if (!embi_stroke_finite(w) || !(w > 0.0f)) {
        return 0.0f;
    }
    const float scale = (embi_stroke_finite(st->scale) && st->scale > 0.0f) ? st->scale : 1.0f;
    const float minw = (embi_stroke_finite(st->min_device_width) && st->min_device_width > 0.0f)
                           ? st->min_device_width
                           : 0.0f;
    if (minw > 0.0f && w * scale < minw) {
        if (coverage != nullptr) {
            *coverage = w * scale / minw;
        }
        w = minw / scale;
    }
    return w;
}

static uint64_t embi_stroke_mix_f(uint64_t h, float v)
{
    uint32_t b;
    SDL_memcpy(&b, &v, sizeof b);
    return embi_hash64(h ^ b);
}

/* The key of the outline embi_path_stroke_ex makes of in with st, w (embi_stroke_width) and tolerance: everything the
 * outline depends on. */
static uint64_t embi_stroke_key(const embi_path *in, const embi_stroke_style *st, float w, float tolerance)
{
    uint64_t h = embi_hash64(embi_path_hash(in) ^ 0x2545f4914f6cdd1dull);
    h = embi_stroke_mix_f(h, w);
    h = embi_stroke_mix_f(h, tolerance);
    h = embi_stroke_mix_f(h, st->miter_limit);
    h = embi_hash64(h ^ (((uint64_t)st->cap << 8) | (uint64_t)st->join));
    if (st->dash != nullptr && st->dash_count > 0) {
        h = embi_hash64(h ^ st->dash_count);
        for (uint32_t i = 0; i < st->dash_count; i++) {
            h = embi_stroke_mix_f(h, st->dash[i]);
        }
        h = embi_stroke_mix_f(h, st->dash_offset);
    }
    return h;
}

embi_path *embi_path_stroke_ex(const embi_path *in, const embi_stroke_style *st, float tolerance,
                               float *coverage)
{
    if (coverage != nullptr) {
        *coverage = 1.0f;
    }
    if (in == nullptr || st == nullptr) {
        return nullptr;
    }
    const float tol =
        (embi_stroke_finite(tolerance) && tolerance > 0.0f) ? tolerance : EMBI_STROKE_TOL_DEFAULT;
    embi_path *out = embi_path_create(tol);
    if (out == nullptr) {
        return nullptr;
    }
    const float w = embi_stroke_width(st, coverage);
    if (!(w > 0.0f)) {
        return out;
    }
    embi_stroker S = {0};
    S.out = out;
    S.hw = w * 0.5f;
    if (!(S.hw <= EMBI_STROKE_MAX_HW)) {
        S.hw = EMBI_STROKE_MAX_HW;
    }
    S.tol = tol;
    S.cap = st->cap;
    S.join = st->join;
    const float ml = (embi_stroke_finite(st->miter_limit) && st->miter_limit >= 1.0f)
                         ? st->miter_limit
                         : 4.0f;
    S.miter_min = 2.0f / (ml * ml);
    float step = 2.0f * SDL_powf(4.0f * tol / S.hw, 0.25f);
    if (!(step <= EMBI_STROKE_PI * 0.5f)) {
        step = EMBI_STROKE_PI * 0.5f;
    }
    if (step < EMBI_STROKE_PI / 128.0f) {
        step = EMBI_STROKE_PI / 128.0f;
    }
    S.arc_step = step;
    S.budget = EMBI_STROKE_MAX_OUT;

    /* The dash pattern, doubled when odd; off when invalid. */
    if (st->dash != nullptr && st->dash_count > 0) {
        bool ok = true;
        float sum = 0.0f;
        for (uint32_t i = 0; i < st->dash_count; i++) {
            const float v = st->dash[i];
            if (!embi_stroke_finite(v) || v < 0.0f) {
                ok = false;
                break;
            }
            sum += v;
        }
        if (ok && sum > 0.0f && embi_stroke_finite(sum)) {
            const uint32_t reps = (st->dash_count & 1u) ? 2u : 1u;
            for (uint32_t r = 0; r < reps; r++) {
                for (uint32_t i = 0; i < st->dash_count; i++) {
                    embi_vec_push(&S.pat, st->dash[i]);
                }
            }
        }
    }
    const float doff = embi_stroke_finite(st->dash_offset) ? st->dash_offset : 0.0f;

    const embi_qcurve *curves = nullptr;
    uint32_t ncurves = 0;
    curves = embi_path_segments(in, &ncurves);
    const uint32_t nsub = embi_path_subpath_count(in);
    for (uint32_t k = 0; k < nsub && S.budget > 0; k++) {
        embi_path_subpath_info si;
        if (!embi_path_subpath(in, k, &si)) {
            break;
        }
        if (si.count == 0 || curves == nullptr) {
            if (S.pat.len == 0 || embi_dash_starts_on_zero(&S, doff)) {
                embi_stroke_do_dot(&S, si.sx, si.sy, 1.0f, 0.0f);
            }
            continue;
        }
        embi_stroke_build(&S, curves + si.first, si.count);
        embi_stroke_merge_tiny(&S);
        if (S.pat.len > 0) {
            embi_stroke_dashed(&S, doff, si.closed);
        } else {
            embi_stroke_chain(&S, S.segs.data, S.segs.len, si.closed);
        }
    }
    embi_vec_free(&S.pc);
    embi_vec_free(&S.rl);
    embi_vec_free(&S.rr);
    embi_vec_free(&S.segs);
    embi_vec_free(&S.dsegs);
    embi_vec_free(&S.runs);
    embi_vec_free(&S.merged);
    embi_vec_free(&S.pat);
    return out;
}

embi_path *embi_path_stroke(const embi_path *in, const embi_stroke_style *st, float tolerance)
{
    return embi_path_stroke_ex(in, st, tolerance, nullptr);
}

void embi_frame_stroke_path(embi_frame *f, const embi_path *p, const embi_stroke_style *st,
                            float tolerance, const embi_paint *paint, uint32_t xform,
                            uint32_t clip)
{
    if (f == nullptr || p == nullptr || st == nullptr || paint == nullptr) {
        return;
    }
    float cov = 1.0f;
    const float w = embi_stroke_width(st, &cov);
    if (!(w > 0.0f)) {
        return;
    }
    embi_paint pt = *paint;
    if (cov < 1.0f && pt.kind == EMBI_PRIM_FILL) {
        const uint32_t a = pt.color >> 24;
        const uint32_t na = (uint32_t)((float)a * cov + 0.5f);
        pt.color = (pt.color & 0x00ffffffu) | (na << 24);
    }
    /* The outline is cached under a key of the path and the style, so a stroke drawn again (a list painted again makes new
     * paths of the same shapes) is not made again: stroking every SVG icon in each frame cost about 10 us an icon. */
    const uint64_t key = embi_stroke_key(p, st, w, tolerance);
    const float identity[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (embi_frame_fill_cached(f, key | (1ull << 63), identity, &pt, EMBI_FILL_NONZERO, xform, clip)) {
        return;
    }
    embi_path *s = embi_path_stroke_ex(p, st, tolerance, nullptr);
    if (s == nullptr) {
        return;
    }
    embi_frame_fill_path_keyed(f, s, key, EMBI_FILL_NONZERO, &pt, xform, clip);
    embi_path_destroy(s);
}
