/*
 * clip-path basic shapes (R3b, see clip_path.h): the resolution of the shape functions to a rounded rect or an outline of
 * quadratic curves, an SVG path data parser (M L H V C S Q T A Z, absolute and relative; cubics and arcs become quadratics
 * that stay within a fraction of a px), and the point-in-shape test hit testing uses.
 */
#include "layout/clip_path.h"

#include "layout/xform.h"

#include <math.h>
#include <string.h>

static const double k_pi_cp = 3.14159265358979323846;

/* ------------------------------------------------------------------ outline building */

typedef struct outline {
    embi_lay_curves *cv;
    uint32_t first;
    float mn[2], mx[2];
} outline;

static void ol_init(outline *o, embi_lay_curves *cv)
{
    o->cv = cv;
    o->first = cv->len;
    o->mn[0] = o->mn[1] = 1e30f;
    o->mx[0] = o->mx[1] = -1e30f;
}

static void ol_bounds(outline *o, float x, float y)
{
    o->mn[0] = fminf(o->mn[0], x);
    o->mn[1] = fminf(o->mn[1], y);
    o->mx[0] = fmaxf(o->mx[0], x);
    o->mx[1] = fmaxf(o->mx[1], y);
}

static void ol_quad(outline *o, float x0, float y0, float cx, float cy, float x1, float y1)
{
    embi_lay_curve c = {{x0, y0, cx, cy, x1, y1}};
    embi_vec_push(o->cv, c);
    ol_bounds(o, x0, y0);
    ol_bounds(o, cx, cy);
    ol_bounds(o, x1, y1);
}

static void ol_line(outline *o, float x0, float y0, float x1, float y1)
{
    if (x0 == x1 && y0 == y1) {
        return;
    }
    ol_quad(o, x0, y0, 0.5f * (x0 + x1), 0.5f * (y0 + y1), x1, y1);
}

/* A cubic as eight quadratics (each one a piece of the cubic with the control point of the standard approximation). */
static void ol_cubic(outline *o, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3)
{
    enum { N = 8 };
    for (int i = 0; i < N; i++) {
        double t0 = (double)i / N, t1 = (double)(i + 1) / N, h = t1 - t0;
        double pt[2][2], dv[2][2];
        for (int k = 0; k < 2; k++) {
            double t = k == 0 ? t0 : t1, u = 1.0 - t;
            pt[k][0] = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            pt[k][1] = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            dv[k][0] = 3 * (u * u * (x1 - x0) + 2 * u * t * (x2 - x1) + t * t * (x3 - x2));
            dv[k][1] = 3 * (u * u * (y1 - y0) + 2 * u * t * (y2 - y1) + t * t * (y3 - y2));
        }
        double q1x = pt[0][0] + h / 3.0 * dv[0][0], q1y = pt[0][1] + h / 3.0 * dv[0][1];
        double q2x = pt[1][0] - h / 3.0 * dv[1][0], q2y = pt[1][1] - h / 3.0 * dv[1][1];
        double cx = (3.0 * (q1x + q2x) - (pt[0][0] + pt[1][0])) / 4.0;
        double cy = (3.0 * (q1y + q2y) - (pt[0][1] + pt[1][1])) / 4.0;
        ol_quad(o, (float)pt[0][0], (float)pt[0][1], (float)cx, (float)cy, (float)pt[1][0], (float)pt[1][1]);
    }
}

/* An arc of the ellipse centered (cx, cy) with radii rx, ry, rotated by phi, from angle a0 over da (radians), as quadratics
 * of at most 30 degrees each. */
static void ol_arc(outline *o, double cx, double cy, double rx, double ry, double phi, double a0, double da)
{
    int n = (int)ceil(fabs(da) / (k_pi_cp / 6.0));
    if (n < 1) {
        n = 1;
    }
    double cp = cos(phi), sp = sin(phi);
    double step = da / n;
    for (int i = 0; i < n; i++) {
        double t0 = a0 + step * i, t1 = t0 + step, tm = 0.5 * (t0 + t1);
        double k = 1.0 / cos(0.5 * step);
        double p[3][2] = {{rx * cos(t0), ry * sin(t0)}, {rx * cos(tm) * k, ry * sin(tm) * k}, {rx * cos(t1), ry * sin(t1)}};
        float f[6];
        for (int j = 0; j < 3; j++) {
            f[2 * j] = (float)(cx + cp * p[j][0] - sp * p[j][1]);
            f[2 * j + 1] = (float)(cy + sp * p[j][0] + cp * p[j][1]);
        }
        ol_quad(o, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
}

/* An SVG elliptical arc (implementation notes F.6.5) from (x0, y0) to (x1, y1). */
static void ol_svg_arc(outline *o, double x0, double y0, double rx, double ry, double rot_deg, bool large, bool sweep, double x1,
                       double y1)
{
    if (x0 == x1 && y0 == y1) {
        return;
    }
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx == 0.0 || ry == 0.0) {
        ol_line(o, (float)x0, (float)y0, (float)x1, (float)y1);
        return;
    }
    double phi = rot_deg * k_pi_cp / 180.0, cp = cos(phi), sp = sin(phi);
    double dx = (x0 - x1) / 2.0, dy = (y0 - y1) / 2.0;
    double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
    double lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry);
    if (lam > 1.0) {
        double s = sqrt(lam);
        rx *= s;
        ry *= s;
    }
    double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    double den = rx * rx * yp * yp + ry * ry * xp * xp;
    double co = den > 0.0 ? sqrt(fmax(0.0, num / den)) : 0.0;
    if (large == sweep) {
        co = -co;
    }
    double cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x1) / 2.0, cy = sp * cxp + cp * cyp + (y0 + y1) / 2.0;
    double ux = (xp - cxp) / rx, uy = (yp - cyp) / ry;
    double vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
    double a0 = atan2(uy, ux);
    double da = atan2(vx * uy - vy * ux, vx * ux + vy * uy);
    if (!sweep && da > 0.0) {
        da -= 2.0 * k_pi_cp;
    } else if (sweep && da < 0.0) {
        da += 2.0 * k_pi_cp;
    }
    ol_arc(o, cx, cy, rx, ry, phi, a0, da);
}

/* ------------------------------------------------------------------ SVG path data */

typedef struct pscan {
    const char *s;
    uint32_t n, i;
} pscan;

static void ps_skip(pscan *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r' ||
                           p->s[p->i] == '\f' || p->s[p->i] == ',')) {
        p->i++;
    }
}

static bool ps_num(pscan *p, double *out)
{
    ps_skip(p);
    uint32_t i = p->i;
    if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) {
        i++;
    }
    uint32_t digits = 0;
    while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') {
        i++;
        digits++;
    }
    if (i < p->n && p->s[i] == '.') {
        i++;
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
        uint32_t j = i + 1;
        if (j < p->n && (p->s[j] == '+' || p->s[j] == '-')) {
            j++;
        }
        if (j < p->n && p->s[j] >= '0' && p->s[j] <= '9') {
            while (j < p->n && p->s[j] >= '0' && p->s[j] <= '9') {
                j++;
            }
            i = j;
        }
    }
    char buf[64];
    uint32_t len = i - p->i;
    if (len >= sizeof buf) {
        return false;
    }
    memcpy(buf, p->s + p->i, len);
    buf[len] = 0;
    char *end = nullptr;
    *out = SDL_strtod(buf, &end);
    p->i = i;
    return true;
}

static bool ps_flag(pscan *p, bool *out)
{
    ps_skip(p);
    if (p->i < p->n && (p->s[p->i] == '0' || p->s[p->i] == '1')) {
        *out = p->s[p->i] == '1';
        p->i++;
        return true;
    }
    return false;
}

bool embi_lay_svg_path(embi_str d, float ox, float oy, embi_lay_curves *cv)
{
    outline o;
    ol_init(&o, cv);
    pscan p = {d.ptr, d.len, 0};
    double cx = 0, cy = 0, sx = 0, sy = 0;      /* the current point, the subpath start */
    double lcx = 0, lcy = 0;                    /* the last control point (S, T) */
    char prev = 0;                              /* the previous command, upper case */
    char cmd = 0;
    bool open = false;
    uint32_t start_len = cv->len;
    ps_skip(&p);
    while (p.i < p.n) {
        ps_skip(&p);
        if (p.i >= p.n) {
            break;
        }
        char ch = p.s[p.i];
        bool is_cmd = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (is_cmd) {
            cmd = ch;
            p.i++;
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            goto fail;
        } else if (cmd == 'M') {
            cmd = 'L'; /* the pairs after a moveto are linetos */
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        if (start_len == cv->len && !open && cmd != 'M' && cmd != 'm' && prev == 0) {
            goto fail; /* data must start with a moveto */
        }
        bool rel = cmd >= 'a';
        char up = (char)(rel ? cmd - 32 : cmd);
        double a[7];
        switch (up) {
        case 'Z':
            if (open) {
                ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
            }
            cx = sx;
            cy = sy;
            open = false;
            break;
        case 'M':
            if (!ps_num(&p, &a[0]) || !ps_num(&p, &a[1])) {
                goto fail;
            }
            if (open) {
                ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
            }
            cx = sx = rel && prev != 0 ? cx + a[0] : a[0];
            cy = sy = rel && prev != 0 ? cy + a[1] : a[1];
            open = true;
            break;
        case 'L':
        case 'H':
        case 'V': {
            double nx = cx, ny = cy;
            if (up == 'L') {
                if (!ps_num(&p, &a[0]) || !ps_num(&p, &a[1])) {
                    goto fail;
                }
                nx = rel ? cx + a[0] : a[0];
                ny = rel ? cy + a[1] : a[1];
            } else if (up == 'H') {
                if (!ps_num(&p, &a[0])) {
                    goto fail;
                }
                nx = rel ? cx + a[0] : a[0];
            } else {
                if (!ps_num(&p, &a[0])) {
                    goto fail;
                }
                ny = rel ? cy + a[0] : a[0];
            }
            ol_line(&o, (float)cx, (float)cy, (float)nx, (float)ny);
            cx = nx;
            cy = ny;
            open = true;
            break;
        }
        case 'C':
        case 'S': {
            double x1, y1;
            int skip = 0;
            if (up == 'C') {
                for (int k = 0; k < 6; k++) {
                    if (!ps_num(&p, &a[k])) {
                        goto fail;
                    }
                }
                x1 = a[0];
                y1 = a[1];
                skip = 2;
                if (rel) {
                    x1 += cx;
                    y1 += cy;
                }
            } else {
                for (int k = 0; k < 4; k++) {
                    if (!ps_num(&p, &a[k + 2])) {
                        goto fail;
                    }
                }
                x1 = (prev == 'C' || prev == 'S') ? 2 * cx - lcx : cx;
                y1 = (prev == 'C' || prev == 'S') ? 2 * cy - lcy : cy;
            }
            double x2 = a[2], y2 = a[3], x3 = a[4], y3 = a[5];
            (void)skip;
            if (rel) {
                x2 += cx;
                y2 += cy;
                x3 += cx;
                y3 += cy;
            }
            ol_cubic(&o, cx, cy, x1, y1, x2, y2, x3, y3);
            lcx = x2;
            lcy = y2;
            cx = x3;
            cy = y3;
            open = true;
            break;
        }
        case 'Q':
        case 'T': {
            double x1, y1, x2, y2;
            if (up == 'Q') {
                for (int k = 0; k < 4; k++) {
                    if (!ps_num(&p, &a[k])) {
                        goto fail;
                    }
                }
                x1 = rel ? cx + a[0] : a[0];
                y1 = rel ? cy + a[1] : a[1];
                x2 = rel ? cx + a[2] : a[2];
                y2 = rel ? cy + a[3] : a[3];
            } else {
                if (!ps_num(&p, &a[0]) || !ps_num(&p, &a[1])) {
                    goto fail;
                }
                x1 = (prev == 'Q' || prev == 'T') ? 2 * cx - lcx : cx;
                y1 = (prev == 'Q' || prev == 'T') ? 2 * cy - lcy : cy;
                x2 = rel ? cx + a[0] : a[0];
                y2 = rel ? cy + a[1] : a[1];
            }
            ol_quad(&o, (float)cx, (float)cy, (float)x1, (float)y1, (float)x2, (float)y2);
            lcx = x1;
            lcy = y1;
            cx = x2;
            cy = y2;
            open = true;
            break;
        }
        case 'A': {
            bool large = false, sweep = false;
            if (!ps_num(&p, &a[0]) || !ps_num(&p, &a[1]) || !ps_num(&p, &a[2]) || !ps_flag(&p, &large) ||
                !ps_flag(&p, &sweep) || !ps_num(&p, &a[3]) || !ps_num(&p, &a[4])) {
                goto fail;
            }
            double nx = rel ? cx + a[3] : a[3], ny = rel ? cy + a[4] : a[4];
            ol_svg_arc(&o, cx, cy, a[0], a[1], a[2], large, sweep, nx, ny);
            cx = nx;
            cy = ny;
            open = true;
            break;
        }
        default:
            goto fail;
        }
        prev = up == 'Z' ? 'Z' : up;
        if (up == 'M' || up == 'Z') {
            /* A lineto must follow the moveto with its own command letter or more pairs; Z takes no operands. */
        }
    }
    if (open) {
        ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
    }
    /* Move the outline to the origin. */
    for (uint32_t i = start_len; i < cv->len; i++) {
        for (int k = 0; k < 3; k++) {
            cv->data[i].c[2 * k] += ox;
            cv->data[i].c[2 * k + 1] += oy;
        }
    }
    return true;
fail:
    cv->len = start_len;
    return false;
}

/* ------------------------------------------------------------------ rounded rects */

void embi_lay_cp_rrect(embi_lay_cp *out, embi_lay_curves *cv, const float rect[4], const float rx[4], const float ry[4])
{
    memset(out, 0, sizeof *out);
    float x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    if (!(w > 0.0f) || !(h > 0.0f)) {
        out->kind = EMBI_LCP_EMPTY;
        return;
    }
    out->bounds[0] = x;
    out->bounds[1] = y;
    out->bounds[2] = x + w;
    out->bounds[3] = y + h;
    bool circular = true;
    for (int i = 0; i < 4; i++) {
        if (fabsf(rx[i] - ry[i]) > 1e-4f) {
            circular = false;
        }
    }
    if (circular) {
        out->kind = EMBI_LCP_RRECT;
        memcpy(out->rect, rect, sizeof out->rect);
        for (int i = 0; i < 4; i++) {
            out->radii[i] = rx[i];
        }
        return;
    }
    /* Corners are elliptical arcs between four straight edges. */
    outline o;
    ol_init(&o, cv);
    float r0x = rx[0], r0y = ry[0], r1x = rx[1], r1y = ry[1], r2x = rx[2], r2y = ry[2], r3x = rx[3], r3y = ry[3];
    ol_line(&o, x + r0x, y, x + w - r1x, y);
    if (r1x > 0 && r1y > 0) {
        ol_arc(&o, x + w - r1x, y + r1y, r1x, r1y, 0.0, -k_pi_cp / 2.0, k_pi_cp / 2.0);
    }
    ol_line(&o, x + w, y + r1y, x + w, y + h - r2y);
    if (r2x > 0 && r2y > 0) {
        ol_arc(&o, x + w - r2x, y + h - r2y, r2x, r2y, 0.0, 0.0, k_pi_cp / 2.0);
    }
    ol_line(&o, x + w - r2x, y + h, x + r3x, y + h);
    if (r3x > 0 && r3y > 0) {
        ol_arc(&o, x + r3x, y + h - r3y, r3x, r3y, 0.0, k_pi_cp / 2.0, k_pi_cp / 2.0);
    }
    ol_line(&o, x, y + h - r3y, x, y + r0y);
    if (r0x > 0 && r0y > 0) {
        ol_arc(&o, x + r0x, y + r0y, r0x, r0y, 0.0, k_pi_cp, k_pi_cp / 2.0);
    }
    out->kind = EMBI_LCP_PATH;
    out->first = o.first;
    out->n = cv->len - o.first;
    out->evenodd = false;
    out->bounds[0] = x;
    out->bounds[1] = y;
    out->bounds[2] = x + w;
    out->bounds[3] = y + h;
}

/* ------------------------------------------------------------------ the shape functions */

typedef struct env {
    float fs, vw, vh, rem;
} env;

static bool lp(const env *e, const embi_value *v, double ref, double *out)
{
    bool ok = false;
    *out = embi_lay_len_px(v, ref, e->fs, e->vw, e->vh, e->rem, &ok);
    return ok;
}

/* The border-radius of a shape rect (CSS Backgrounds 3 section 5): the radii resolved against the rect, overlapping radii
 * scaled down by one common factor. */
static bool shape_radii(const env *e, const embi_shape *sh, const float r[4], double refw, double refh, float rx[4],
                        float ry[4])
{
    for (int i = 0; i < 4; i++) {
        rx[i] = ry[i] = 0.0f;
    }
    if (!sh->has_round) {
        return true;
    }
    for (int i = 0; i < 4; i++) {
        double hx, hy;
        if (!lp(e, &sh->radii[i], refw, &hx) || !lp(e, &sh->radii[4 + i], refh, &hy)) {
            return false;
        }
        rx[i] = hx > 0.0 ? (float)hx : 0.0f;
        ry[i] = hy > 0.0 ? (float)hy : 0.0f;
    }
    double f = 1.0;
    double sx[4] = {rx[0] + rx[1], rx[3] + rx[2], 0, 0}; /* top, bottom */
    double sy[4] = {ry[0] + ry[3], ry[1] + ry[2], 0, 0}; /* left, right */
    if (sx[0] > 0.0) {
        f = fmin(f, r[2] / sx[0]);
    }
    if (sx[1] > 0.0) {
        f = fmin(f, r[2] / sx[1]);
    }
    if (sy[0] > 0.0) {
        f = fmin(f, r[3] / sy[0]);
    }
    if (sy[1] > 0.0) {
        f = fmin(f, r[3] / sy[1]);
    }
    if (f < 1.0) {
        for (int i = 0; i < 4; i++) {
            rx[i] = (float)(rx[i] * f);
            ry[i] = (float)(ry[i] * f);
        }
    }
    return true;
}

static bool axis_pos(const env *e, const embi_pos_axis *ax, double size, double *out)
{
    double off;
    if (!lp(e, &ax->off, size, &off)) {
        return false;
    }
    *out = ax->anchor != 0 ? size - off : off;
    return true;
}

/* Percentages of the radii are of the reference box (refw, refh), not of the shape's rectangle r. */
static void rect_shape(const env *e, const embi_shape *sh, const float r[4], double refw, double refh, embi_lay_cp *out,
                       embi_lay_curves *cv, bool *ok)
{
    float rx[4], ry[4];
    if (!shape_radii(e, sh, r, refw, refh, rx, ry)) {
        *ok = false;
        return;
    }
    embi_lay_cp_rrect(out, cv, r, rx, ry);
}

/* The PATH result for the curves [first, cv->len): the bounds are those of the control polygons. */
static void finish_path(embi_lay_cp *out, const embi_lay_curves *cv, uint32_t first, bool evenodd)
{
    out->kind = EMBI_LCP_PATH;
    out->evenodd = evenodd;
    out->first = first;
    out->n = cv->len - first;
    out->bounds[0] = out->bounds[1] = 1e30f;
    out->bounds[2] = out->bounds[3] = -1e30f;
    for (uint32_t i = first; i < cv->len; i++) {
        for (int k = 0; k < 3; k++) {
            out->bounds[0] = fminf(out->bounds[0], cv->data[i].c[2 * k]);
            out->bounds[1] = fminf(out->bounds[1], cv->data[i].c[2 * k + 1]);
            out->bounds[2] = fmaxf(out->bounds[2], cv->data[i].c[2 * k]);
            out->bounds[3] = fmaxf(out->bounds[3], cv->data[i].c[2 * k + 1]);
        }
    }
    if (out->n == 0) {
        out->kind = EMBI_LCP_EMPTY;
    }
}

/* shape() (CSS Shapes 2): the commands as an outline, in the page frame (ref is the reference box). Absolute points are
 * positions in the box; relative points and control points are offsets from the current point (control points: from the
 * start of their command). */
static bool resolve_shape_fn(const env *e, const embi_shape *sh, const float ref[4], embi_lay_cp *out, embi_lay_curves *cv)
{
    (void)out;
    double w = ref[2], h = ref[3];
    double diag = sqrt((w * w + h * h) / 2.0);
    outline o;
    ol_init(&o, cv);
    double cx, cy;
    if (!axis_pos(e, &sh->from.x, w, &cx) || !axis_pos(e, &sh->from.y, h, &cy)) {
        return false;
    }
    cx += ref[0];
    cy += ref[1];
    double sx = cx, sy = cy;
    double lcx = cx, lcy = cy;
    int prev = 0; /* 1 after a cubic (lc is its second control point), 2 after a quadratic (lc is its control point) */
    bool open = true;
    for (uint32_t i = 0; i < sh->ncmds; i++) {
        const embi_shape_cmd *c = &sh->cmds[i];
        double nx = cx, ny = cy, px[2], py[2];
        if (c->op == EMBI_SOP_CLOSE) {
            if (open) {
                ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
            }
            cx = sx;
            cy = sy;
            open = false;
            prev = 0;
            continue;
        }
        /* The end point. */
        if (c->op == EMBI_SOP_HLINE || c->op == EMBI_SOP_VLINE) {
            bool hz = c->op == EMBI_SOP_HLINE;
            double v;
            if (c->rel) {
                if (!lp(e, &c->p[0].off, hz ? w : h, &v)) {
                    return false;
                }
                v += hz ? cx : cy;
            } else {
                if (!axis_pos(e, &c->p[0], hz ? w : h, &v)) {
                    return false;
                }
                v += hz ? ref[0] : ref[1];
            }
            if (hz) {
                nx = v;
            } else {
                ny = v;
            }
        } else {
            double dx, dy;
            if (c->rel) {
                if (!lp(e, &c->p[0].off, w, &dx) || !lp(e, &c->p[1].off, h, &dy)) {
                    return false;
                }
                nx = cx + dx;
                ny = cy + dy;
            } else {
                if (!axis_pos(e, &c->p[0], w, &dx) || !axis_pos(e, &c->p[1], h, &dy)) {
                    return false;
                }
                nx = ref[0] + dx;
                ny = ref[1] + dy;
            }
        }
        for (uint32_t k = 0; k < c->ncp; k++) {
            double dx, dy;
            if (c->rel) {
                if (!lp(e, &c->c[k][0].off, w, &dx) || !lp(e, &c->c[k][1].off, h, &dy)) {
                    return false;
                }
                px[k] = cx + dx;
                py[k] = cy + dy;
            } else {
                if (!axis_pos(e, &c->c[k][0], w, &dx) || !axis_pos(e, &c->c[k][1], h, &dy)) {
                    return false;
                }
                px[k] = ref[0] + dx;
                py[k] = ref[1] + dy;
            }
        }
        switch (c->op) {
        case EMBI_SOP_MOVE:
            if (open) {
                ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
            }
            sx = nx;
            sy = ny;
            open = true;
            prev = 0;
            break;
        case EMBI_SOP_LINE:
        case EMBI_SOP_HLINE:
        case EMBI_SOP_VLINE:
            ol_line(&o, (float)cx, (float)cy, (float)nx, (float)ny);
            prev = 0;
            break;
        case EMBI_SOP_CURVE:
            if (c->ncp == 2) {
                ol_cubic(&o, cx, cy, px[0], py[0], px[1], py[1], nx, ny);
                lcx = px[1];
                lcy = py[1];
                prev = 1;
            } else {
                ol_quad(&o, (float)cx, (float)cy, (float)px[0], (float)py[0], (float)nx, (float)ny);
                lcx = px[0];
                lcy = py[0];
                prev = 2;
            }
            break;
        case EMBI_SOP_SMOOTH:
            if (c->ncp >= 1) {
                double x1 = prev == 1 ? 2 * cx - lcx : cx, y1 = prev == 1 ? 2 * cy - lcy : cy;
                ol_cubic(&o, cx, cy, x1, y1, px[0], py[0], nx, ny);
                lcx = px[0];
                lcy = py[0];
                prev = 1;
            } else {
                double x1 = prev == 2 ? 2 * cx - lcx : cx, y1 = prev == 2 ? 2 * cy - lcy : cy;
                ol_quad(&o, (float)cx, (float)cy, (float)x1, (float)y1, (float)nx, (float)ny);
                lcx = x1;
                lcy = y1;
                prev = 2;
            }
            break;
        case EMBI_SOP_ARC: {
            double rx, ry;
            if (c->nr >= 2) {
                if (!lp(e, &c->r[0], w, &rx) || !lp(e, &c->r[1], h, &ry)) {
                    return false;
                }
            } else {
                if (!lp(e, &c->r[0], diag, &rx)) {
                    return false;
                }
                ry = rx;
            }
            ol_svg_arc(&o, cx, cy, rx, ry, c->rot, c->large, c->cw, nx, ny);
            prev = 0;
            break;
        }
        default:
            return false;
        }
        cx = nx;
        cy = ny;
        if (c->op == EMBI_SOP_MOVE) {
            open = true;
        } else if (!open) {
            /* A drawing command after close starts from the subpath start (done through cx, cy = start above) */
            open = true;
        }
    }
    if (open) {
        ol_line(&o, (float)cx, (float)cy, (float)sx, (float)sy);
    }
    return true;
}

bool embi_lay_clip_shape(const embi_shape *sh, const float ref[4], float fs, float vw, float vh, float rem, embi_lay_cp *out,
                         embi_lay_curves *cv)
{
    memset(out, 0, sizeof *out);
    env e = {fs, vw, vh, rem};
    double w = ref[2], h = ref[3];
    bool ok = true;
    switch (sh->kind) {
    case EMBI_SHAPE_INSET: {
        double d[4]; /* top right bottom left */
        for (int i = 0; i < 4; i++) {
            if (!lp(&e, &sh->box[i], (i & 1) ? w : h, &d[i])) {
                return false;
            }
        }
        float r[4] = {(float)(ref[0] + d[3]), (float)(ref[1] + d[0]), (float)(w - d[3] - d[1]), (float)(h - d[0] - d[2])};
        rect_shape(&e, sh, r, w, h, out, cv, &ok);
        return ok;
    }
    case EMBI_SHAPE_XYWH: {
        double d[4];
        for (int i = 0; i < 4; i++) {
            if (!lp(&e, &sh->box[i], (i & 1) ? h : w, &d[i])) {
                return false;
            }
        }
        float r[4] = {(float)(ref[0] + d[0]), (float)(ref[1] + d[1]), (float)d[2], (float)d[3]};
        rect_shape(&e, sh, r, w, h, out, cv, &ok);
        return ok;
    }
    case EMBI_SHAPE_RECT: {
        /* rect(top right bottom left): the distances of the four edges from the top and left sides of the reference box;
         * auto is the box edge itself. */
        double d[4];
        for (int i = 0; i < 4; i++) {
            if (sh->box[i].kind == EMBI_V_KEYWORD) {
                d[i] = (i == 1) ? w : (i == 2 ? h : 0.0);
            } else if (!lp(&e, &sh->box[i], (i & 1) ? w : h, &d[i])) {
                return false;
            }
        }
        double x0 = d[3], x1 = d[1], y0 = d[0], y1 = d[2];
        float r[4] = {(float)(ref[0] + x0), (float)(ref[1] + y0), (float)fmax(0.0, x1 - x0), (float)fmax(0.0, y1 - y0)};
        rect_shape(&e, sh, r, w, h, out, cv, &ok);
        return ok;
    }
    case EMBI_SHAPE_CIRCLE:
    case EMBI_SHAPE_ELLIPSE: {
        double cx = w / 2.0, cy = h / 2.0;
        if (sh->has_at) {
            if (!axis_pos(&e, &sh->at.x, w, &cx) || !axis_pos(&e, &sh->at.y, h, &cy)) {
                return false;
            }
        }
        double rr[2];
        bool ell = sh->kind == EMBI_SHAPE_ELLIPSE;
        for (int i = 0; i < (ell ? 2 : 1); i++) {
            const embi_value *rv = &sh->r[i];
            double side_lo = i == 0 ? cx : cy, side_hi = (i == 0 ? w : h) - side_lo;
            double sx_lo = cx, sx_hi = w - cx, sy_lo = cy, sy_hi = h - cy;
            if (rv->kind == EMBI_V_KEYWORD) {
                bool far = rv->kw == EMBI_KW_farthest_side;
                if (ell) {
                    double a = fabs(side_lo), b = fabs(side_hi);
                    rr[i] = far ? fmax(a, b) : fmin(a, b);
                } else {
                    double m_lo = fmin(fabs(sx_lo), fabs(sx_hi)), m_hi = fmin(fabs(sy_lo), fabs(sy_hi));
                    double f_lo = fmax(fabs(sx_lo), fabs(sx_hi)), f_hi = fmax(fabs(sy_lo), fabs(sy_hi));
                    rr[i] = far ? fmax(f_lo, f_hi) : fmin(m_lo, m_hi);
                }
            } else {
                double refl = ell ? (i == 0 ? w : h) : sqrt((w * w + h * h) / 2.0);
                if (!lp(&e, rv, refl, &rr[i])) {
                    return false;
                }
            }
        }
        double rx = rr[0], ry = ell ? rr[1] : rr[0];
        if (!(rx > 0.0) || !(ry > 0.0)) {
            out->kind = EMBI_LCP_EMPTY;
            return true;
        }
        float r[4] = {(float)(ref[0] + cx - rx), (float)(ref[1] + cy - ry), (float)(2 * rx), (float)(2 * ry)};
        float rxs[4] = {(float)rx, (float)rx, (float)rx, (float)rx}, rys[4] = {(float)ry, (float)ry, (float)ry, (float)ry};
        embi_lay_cp_rrect(out, cv, r, rxs, rys);
        return true;
    }
    case EMBI_SHAPE_POLYGON: {
        if (sh->npts < 3 || sh->pts == nullptr) {
            out->kind = EMBI_LCP_EMPTY;
            return true;
        }
        outline o;
        ol_init(&o, cv);
        double px0 = 0, py0 = 0, ppx = 0, ppy = 0;
        for (uint32_t i = 0; i < sh->npts; i++) {
            double x, y;
            if (!lp(&e, &sh->pts[2 * i], w, &x) || !lp(&e, &sh->pts[2 * i + 1], h, &y)) {
                cv->len = o.first;
                return false;
            }
            x += ref[0];
            y += ref[1];
            if (i == 0) {
                px0 = x;
                py0 = y;
            } else {
                ol_line(&o, (float)ppx, (float)ppy, (float)x, (float)y);
            }
            ppx = x;
            ppy = y;
        }
        ol_line(&o, (float)ppx, (float)ppy, (float)px0, (float)py0);
        out->kind = EMBI_LCP_PATH;
        out->evenodd = sh->evenodd;
        out->first = o.first;
        out->n = cv->len - o.first;
        out->bounds[0] = o.mn[0];
        out->bounds[1] = o.mn[1];
        out->bounds[2] = o.mx[0];
        out->bounds[3] = o.mx[1];
        if (out->n == 0) {
            out->kind = EMBI_LCP_EMPTY;
        }
        return true;
    }
    case EMBI_SHAPE_PATH: {
        uint32_t first = cv->len;
        if (!embi_lay_svg_path(sh->path, ref[0], ref[1], cv)) {
            return false;
        }
        finish_path(out, cv, first, sh->evenodd);
        return true;
    }
    case EMBI_SHAPE_SHAPE: {
        uint32_t first = cv->len;
        if (!resolve_shape_fn(&e, sh, ref, out, cv)) {
            cv->len = first;
            return false;
        }
        finish_path(out, cv, first, sh->evenodd);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ point test */

static bool in_rrect(const embi_lay_cp *cp, float x, float y)
{
    float x0 = cp->rect[0], y0 = cp->rect[1], x1 = x0 + cp->rect[2], y1 = y0 + cp->rect[3];
    if (x < x0 || x >= x1 || y < y0 || y >= y1) {
        return false;
    }
    /* The corner circle whose quarter contains the point. */
    float r;
    float cx, cy;
    if (x < x0 + cp->radii[0] && y < y0 + cp->radii[0]) {
        r = cp->radii[0];
        cx = x0 + r;
        cy = y0 + r;
    } else if (x > x1 - cp->radii[1] && y < y0 + cp->radii[1]) {
        r = cp->radii[1];
        cx = x1 - r;
        cy = y0 + r;
    } else if (x > x1 - cp->radii[2] && y > y1 - cp->radii[2]) {
        r = cp->radii[2];
        cx = x1 - r;
        cy = y1 - r;
    } else if (x < x0 + cp->radii[3] && y > y1 - cp->radii[3]) {
        r = cp->radii[3];
        cx = x0 + r;
        cy = y1 - r;
    } else {
        return true;
    }
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

bool embi_lay_cp_contains(const embi_lay_cp *cp, const embi_lay_curve *curves, float x, float y)
{
    if (cp->kind == EMBI_LCP_EMPTY) {
        return false;
    }
    if (cp->kind == EMBI_LCP_RRECT) {
        return in_rrect(cp, x, y);
    }
    if (x < cp->bounds[0] || x > cp->bounds[2] || y < cp->bounds[1] || y > cp->bounds[3]) {
        return false;
    }
    int winding = 0;
    int crossings = 0;
    enum { SEG = 12 };
    for (uint32_t i = 0; i < cp->n; i++) {
        const float *c = curves[cp->first + i].c;
        float px = c[0], py = c[1];
        for (int k = 1; k <= SEG; k++) {
            float t = (float)k / SEG, u = 1.0f - t;
            float qx = u * u * c[0] + 2 * u * t * c[2] + t * t * c[4];
            float qy = u * u * c[1] + 2 * u * t * c[3] + t * t * c[5];
            if ((py <= y) != (qy <= y)) {
                float xi = px + (y - py) * (qx - px) / (qy - py);
                if (xi > x) {
                    crossings++;
                    winding += qy > py ? 1 : -1;
                }
            }
            px = qx;
            py = qy;
        }
    }
    return cp->evenodd ? (crossings & 1) != 0 : winding != 0;
}
