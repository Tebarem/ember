#include "gpu/path.h"

#include "base/base.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>

#include "gpu/slug.h"

#define EMBI_PATH_TOL_DEFAULT (1.0f / 64.0f)
#define EMBI_PATH_MAX_DEPTH 12u /* at most 4096 quadratics per cubic */
/* sqrt(3) / 36: the error of a quadratic that replaces a cubic, per unit of |P0 - 3P1 + 3P2 - P3|.
 * Derived from D(t) = t(1-t)(1/2 - t) (3P1 - 3P2 - P0 + P3) and max |t(1-t)(1/2-t)| = sqrt(3)/36. */
#define EMBI_PATH_QUAD_ERR 0.04811252243f
#define EMBI_PATH_PI 3.14159265358979323846f
#define EMBI_PATH_KEY_BIT (1ull << 63)

typedef struct embi_path_contour {
    uint32_t first; /* index of the first curve of the subpath */
    float sx;       /* start point */
    float sy;
    bool closed;    /* ended by embi_path_close */
} embi_path_contour;

struct embi_path {
    EMBI_VEC(embi_qcurve) curves;
    EMBI_VEC(embi_path_contour) contours;
    EMBI_VEC(embi_qcurve) fill; /* curves plus the closing lines of open subpaths, when dirty */
    float tol;
    float cur[2];   /* current point */
    float start[2]; /* start of the current subpath */
    bool has_cur;
    bool open;      /* a subpath has been started and not closed */
    bool moved;     /* the last command was move_to (a close then makes a point subpath) */
    bool fill_dirty;
    uint32_t id;
    uint32_t version;
    uint64_t fill_hash; /* of fill, made with it */
};

static SDL_AtomicInt embi_path_ids;

static bool embi_path_finite(float x)
{
    return x - x == 0.0f;
}

static bool embi_path_finite2(float x, float y)
{
    return embi_path_finite(x) && embi_path_finite(y);
}

embi_path *embi_path_create(float tolerance)
{
    embi_path *p = embi_calloc(1, sizeof *p);
    p->tol = (embi_path_finite(tolerance) && tolerance > 0.0f) ? tolerance : EMBI_PATH_TOL_DEFAULT;
    p->id = (uint32_t)SDL_AddAtomicInt(&embi_path_ids, 1) + 1u;
    p->fill_dirty = true;
    return p;
}

void embi_path_destroy(embi_path *p)
{
    if (p == nullptr) {
        return;
    }
    embi_vec_free(&p->curves);
    embi_vec_free(&p->contours);
    embi_vec_free(&p->fill);
    embi_free(p, sizeof *p);
}

void embi_path_clear(embi_path *p)
{
    if (p == nullptr) {
        return;
    }
    p->curves.len = 0;
    p->contours.len = 0;
    p->has_cur = false;
    p->open = false;
    p->moved = false;
    p->version++;
    p->fill_dirty = true;
}

bool embi_path_is_empty(const embi_path *p)
{
    return p == nullptr || p->curves.len == 0;
}

const embi_qcurve *embi_path_segments(const embi_path *p, uint32_t *count)
{
    if (p == nullptr || count == nullptr) {
        if (count != nullptr) {
            *count = 0;
        }
        return nullptr;
    }
    *count = p->curves.len;
    return p->curves.data;
}

uint32_t embi_path_subpath_count(const embi_path *p)
{
    return p == nullptr ? 0u : p->contours.len;
}

bool embi_path_subpath(const embi_path *p, uint32_t k, embi_path_subpath_info *out)
{
    if (p == nullptr || out == nullptr || k >= p->contours.len) {
        return false;
    }
    const embi_path_contour *c = &p->contours.data[k];
    const uint32_t end = (k + 1u < p->contours.len) ? p->contours.data[k + 1u].first : p->curves.len;
    out->first = c->first;
    out->count = end - c->first;
    out->sx = c->sx;
    out->sy = c->sy;
    out->closed = c->closed;
    return true;
}

/* Segments ----------------------------------------------------------------- */

static void embi_path_changed(embi_path *p)
{
    p->version++;
    p->fill_dirty = true;
}

/* Appends one quadratic from (x0, y0) through the control point to (x1, y1), which becomes the
 * current point. A segment whose three points coincide adds nothing. */
static void embi_path_add(embi_path *p, float x0, float y0, float cx, float cy, float x1, float y1)
{
    p->cur[0] = x1;
    p->cur[1] = y1;
    if (x0 == x1 && y0 == y1 && cx == x0 && cy == y0) {
        return;
    }
    const embi_qcurve q = {.x0 = x0, .y0 = y0, .cx = cx, .cy = cy, .x1 = x1, .y1 = y1};
    embi_vec_push(&p->curves, q);
    embi_path_changed(p);
}

/* Starts a subpath at the current point when none is open. With no current point yet, the
 * subpath starts at (fx, fy), the first point of the command. */
static void embi_path_open(embi_path *p, float fx, float fy)
{
    if (p->open) {
        return;
    }
    if (!p->has_cur) {
        p->cur[0] = fx;
        p->cur[1] = fy;
        p->has_cur = true;
    }
    const embi_path_contour c = {
        .first = p->curves.len, .sx = p->cur[0], .sy = p->cur[1], .closed = false};
    embi_vec_push(&p->contours, c);
    p->start[0] = p->cur[0];
    p->start[1] = p->cur[1];
    p->open = true;
    p->moved = false;
    embi_path_changed(p);
}

/* Cubic to quadratics: a cubic is split at t = 1/2 until one quadratic is within tolerance. The
 * quadratic that replaces a piece has control point (3 (P1 + P2) - P0 - P3) / 4. */
static void embi_path_cubic_rec(embi_path *p, const float P[8], uint32_t depth)
{
    const float dx = P[0] - 3.0f * P[2] + 3.0f * P[4] - P[6];
    const float dy = P[1] - 3.0f * P[3] + 3.0f * P[5] - P[7];
    const float err = EMBI_PATH_QUAD_ERR * SDL_sqrtf(dx * dx + dy * dy);
    if (err <= p->tol || depth >= EMBI_PATH_MAX_DEPTH) {
        const float qx = (3.0f * (P[2] + P[4]) - P[0] - P[6]) * 0.25f;
        const float qy = (3.0f * (P[3] + P[5]) - P[1] - P[7]) * 0.25f;
        embi_path_add(p, P[0], P[1], qx, qy, P[6], P[7]);
        return;
    }
    /* de Casteljau at t = 1/2. */
    const float m01x = (P[0] + P[2]) * 0.5f, m01y = (P[1] + P[3]) * 0.5f;
    const float m12x = (P[2] + P[4]) * 0.5f, m12y = (P[3] + P[5]) * 0.5f;
    const float m23x = (P[4] + P[6]) * 0.5f, m23y = (P[5] + P[7]) * 0.5f;
    const float m012x = (m01x + m12x) * 0.5f, m012y = (m01y + m12y) * 0.5f;
    const float m123x = (m12x + m23x) * 0.5f, m123y = (m12y + m23y) * 0.5f;
    const float mx = (m012x + m123x) * 0.5f, my = (m012y + m123y) * 0.5f;
    const float left[8] = {P[0], P[1], m01x, m01y, m012x, m012y, mx, my};
    const float right[8] = {mx, my, m123x, m123y, m23x, m23y, P[6], P[7]};
    embi_path_cubic_rec(p, left, depth + 1u);
    embi_path_cubic_rec(p, right, depth + 1u);
}

/* Builds the fill outline: every curve, plus the closing line of each subpath that does not end
 * where it started. */
static void embi_path_build_fill(embi_path *p)
{
    p->fill.len = 0;
    for (uint32_t k = 0; k < p->contours.len; k++) {
        const uint32_t first = embi_vec_at(&p->contours, k).first;
        const uint32_t end =
            (k + 1u < p->contours.len) ? embi_vec_at(&p->contours, k + 1u).first : p->curves.len;
        if (end <= first) {
            continue;
        }
        for (uint32_t i = first; i < end; i++) {
            embi_vec_push(&p->fill, embi_vec_at(&p->curves, i));
        }
        const embi_qcurve last = embi_vec_at(&p->curves, end - 1u);
        const float sx = embi_vec_at(&p->contours, k).sx;
        const float sy = embi_vec_at(&p->contours, k).sy;
        if (last.x1 != sx || last.y1 != sy) {
            const embi_qcurve close = {.x0 = last.x1,
                                       .y0 = last.y1,
                                       .cx = (last.x1 + sx) * 0.5f,
                                       .cy = (last.y1 + sy) * 0.5f,
                                       .x1 = sx,
                                       .y1 = sy};
            embi_vec_push(&p->fill, close);
        }
    }
    p->fill_hash = embi_hash_bytes(p->fill.data, (size_t)p->fill.len * sizeof(embi_qcurve), 0x9e3779b97f4a7c15ull);
    p->fill_dirty = false;
}

/* Public commands ---------------------------------------------------------- */

bool embi_path_move_to(embi_path *p, float x, float y)
{
    if (p == nullptr || !embi_path_finite2(x, y)) {
        return false;
    }
    p->cur[0] = x;
    p->cur[1] = y;
    p->has_cur = true;
    p->open = false;
    p->moved = true;
    embi_path_changed(p);
    return true;
}

bool embi_path_line_to(embi_path *p, float x, float y)
{
    if (p == nullptr || !embi_path_finite2(x, y)) {
        return false;
    }
    embi_path_open(p, x, y);
    const float x0 = p->cur[0];
    const float y0 = p->cur[1];
    embi_path_add(p, x0, y0, (x0 + x) * 0.5f, (y0 + y) * 0.5f, x, y);
    return true;
}

bool embi_path_quad_to(embi_path *p, float cx, float cy, float x, float y)
{
    if (p == nullptr || !embi_path_finite2(cx, cy) || !embi_path_finite2(x, y)) {
        return false;
    }
    embi_path_open(p, cx, cy);
    embi_path_add(p, p->cur[0], p->cur[1], cx, cy, x, y);
    return true;
}

bool embi_path_cubic_to(embi_path *p, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    if (p == nullptr || !embi_path_finite2(c1x, c1y) || !embi_path_finite2(c2x, c2y) ||
        !embi_path_finite2(x, y)) {
        return false;
    }
    embi_path_open(p, c1x, c1y);
    const float P[8] = {p->cur[0], p->cur[1], c1x, c1y, c2x, c2y, x, y};
    embi_path_cubic_rec(p, P, 0u);
    return true;
}

bool embi_path_close(embi_path *p)
{
    if (p == nullptr) {
        return false;
    }
    if (!p->open && p->moved && p->has_cur) {
        /* "M x y Z": a point subpath (it draws nothing when filled, a dot when stroked). */
        embi_path_open(p, p->cur[0], p->cur[1]);
    }
    if (p->open) {
        p->contours.data[p->contours.len - 1u].closed = true;
        if (p->cur[0] != p->start[0] || p->cur[1] != p->start[1]) {
            embi_path_add(p, p->cur[0], p->cur[1], (p->cur[0] + p->start[0]) * 0.5f,
                          (p->cur[1] + p->start[1]) * 0.5f, p->start[0], p->start[1]);
        }
        p->cur[0] = p->start[0];
        p->cur[1] = p->start[1];
        p->open = false;
        embi_path_changed(p);
    }
    return true;
}

/* The endpoint form of an elliptical arc (SVG F.6.5 and F.6.6) becomes the center form. The
 * center form is split into pieces (see thmax), and each piece becomes a cubic, which is then
 * flattened like any other cubic. */
bool embi_path_arc_to(embi_path *p, float rx, float ry, float rotation, bool large_arc,
                      bool sweep, float x, float y)
{
    if (p == nullptr || !embi_path_finite(rx) || !embi_path_finite(ry) ||
        !embi_path_finite(rotation) || !embi_path_finite2(x, y)) {
        return false;
    }
    embi_path_open(p, x, y);
    const float x1 = p->cur[0];
    const float y1 = p->cur[1];
    if (x1 == x && y1 == y) {
        return true;
    }
    float rxa = SDL_fabsf(rx);
    float rya = SDL_fabsf(ry);
    if (rxa == 0.0f || rya == 0.0f) {
        return embi_path_line_to(p, x, y);
    }

    const float c = SDL_cosf(rotation);
    const float s = SDL_sinf(rotation);
    const float dx2 = (x1 - x) * 0.5f;
    const float dy2 = (y1 - y) * 0.5f;
    const float x1p = c * dx2 + s * dy2;
    const float y1p = -s * dx2 + c * dy2;
    const float lam = (x1p * x1p) / (rxa * rxa) + (y1p * y1p) / (rya * rya);
    if (lam > 1.0f) {
        const float grow = SDL_sqrtf(lam);
        rxa *= grow;
        rya *= grow;
    }
    const float rx2 = rxa * rxa;
    const float ry2 = rya * rya;
    const float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float coef = (den > 0.0f && num > 0.0f) ? SDL_sqrtf(num / den) : 0.0f;
    if (large_arc == sweep) {
        coef = -coef;
    }
    const float cxp = coef * rxa * y1p / rya;
    const float cyp = -coef * rya * x1p / rxa;
    const float cx = c * cxp - s * cyp + (x1 + x) * 0.5f;
    const float cy = s * cxp + c * cyp + (y1 + y) * 0.5f;

    const float th1 = SDL_atan2f((y1p - cyp) / rya, (x1p - cxp) / rxa);
    const float th2 = SDL_atan2f((-y1p - cyp) / rya, (-x1p - cxp) / rxa);
    float dth = th2 - th1;
    if (!sweep && dth > 0.0f) {
        dth -= 2.0f * EMBI_PATH_PI;
    } else if (sweep && dth < 0.0f) {
        dth += 2.0f * EMBI_PATH_PI;
    }

    /* A cubic of angle t is off the circle by about 1.8e-5 r t^6. Pieces are cut so that this stays
     * under the tolerance, and never longer than a quarter turn. */
    const float rmax = rxa > rya ? rxa : rya;
    float thmax = EMBI_PATH_PI * 0.25f;
    const float fromtol = SDL_powf(p->tol / (2e-5f * rmax), 1.0f / 6.0f);
    if (fromtol < thmax) {
        thmax = fromtol;
    }
    if (thmax < EMBI_PATH_PI / 64.0f) {
        thmax = EMBI_PATH_PI / 64.0f;
    }
    uint32_t n = (uint32_t)SDL_ceilf(SDL_fabsf(dth) / thmax);
    if (n < 1u) {
        n = 1u;
    }

    for (uint32_t k = 0; k < n; k++) {
        const float a0 = th1 + dth * (float)k / (float)n;
        const float a1 = th1 + dth * (float)(k + 1u) / (float)n;
        const float alpha = 4.0f / 3.0f * SDL_tanf((a1 - a0) * 0.25f);
        const float ca0 = SDL_cosf(a0), sa0 = SDL_sinf(a0);
        const float ca1 = SDL_cosf(a1), sa1 = SDL_sinf(a1);
        const float P0x = (k == 0) ? x1 : cx + rxa * ca0 * c - rya * sa0 * s;
        const float P0y = (k == 0) ? y1 : cy + rxa * ca0 * s + rya * sa0 * c;
        const float P3x = (k + 1u == n) ? x : cx + rxa * ca1 * c - rya * sa1 * s;
        const float P3y = (k + 1u == n) ? y : cy + rxa * ca1 * s + rya * sa1 * c;
        const float d0x = -rxa * sa0 * c - rya * ca0 * s;
        const float d0y = -rxa * sa0 * s + rya * ca0 * c;
        const float d1x = -rxa * sa1 * c - rya * ca1 * s;
        const float d1y = -rxa * sa1 * s + rya * ca1 * c;
        const float P[8] = {P0x,
                            P0y,
                            P0x + alpha * d0x,
                            P0y + alpha * d0y,
                            P3x - alpha * d1x,
                            P3y - alpha * d1y,
                            P3x,
                            P3y};
        embi_path_cubic_rec(p, P, 0u);
    }
    p->cur[0] = x;
    p->cur[1] = y;
    return true;
}

/* Fill --------------------------------------------------------------------- */

uint64_t embi_path_hash(const embi_path *p)
{
    if (p == nullptr) {
        return 0;
    }
    uint64_t h = embi_hash_bytes(p->curves.data, (size_t)p->curves.len * sizeof(embi_qcurve), 0x51ed2701a1b2c3d4ull);
    for (uint32_t k = 0; k < p->contours.len; k++) {
        const embi_path_contour *c = &embi_vec_at(&p->contours, k);
        uint32_t sx, sy;
        SDL_memcpy(&sx, &c->sx, sizeof sx);
        SDL_memcpy(&sy, &c->sy, sizeof sy);
        h = embi_hash64(h ^ (((uint64_t)c->first << 1) | (c->closed ? 1u : 0u)));
        h = embi_hash64(h ^ (((uint64_t)sx << 32) | sy));
    }
    return h;
}

void embi_frame_fill_path_keyed(embi_frame *f, embi_path *p, uint64_t key, embi_fill_rule rule,
                                const embi_paint *paint, uint32_t xform, uint32_t clip)
{
    if (f == nullptr || p == nullptr || paint == nullptr) {
        return;
    }
    if (p->fill_dirty) {
        embi_path_build_fill(p);
    }
    if (p->fill.len == 0) {
        return;
    }
    const float identity[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_frame_fill_quads(f, key | EMBI_PATH_KEY_BIT, p->fill.data, p->fill.len, identity, paint, rule, xform, clip);
}

void embi_frame_fill_path(embi_frame *f, embi_path *p, embi_fill_rule rule,
                          const embi_paint *paint, uint32_t xform, uint32_t clip)
{
    if (f == nullptr || p == nullptr || paint == nullptr) {
        return;
    }
    if (p->fill_dirty) {
        embi_path_build_fill(p);
    }
    /* Keys with bit 63 set are paths' (glyph keys leave it clear). */
    embi_frame_fill_path_keyed(f, p, p->fill_hash, rule, paint, xform, clip);
}
