/*
 * Canvas paths (R4b): the subpaths of the current default path and of Path2D, in the space the script drew them in (the
 * current default path stores device-space points: the callers pass the CTM). Curves are kept as written (moves, lines,
 * quadratics and cubics); arcs become cubics. The replay converts to the GPU path (cgpu.c).
 */
#include "canvas/canvas_int.h"

#include <string.h>

#define CP_PI 3.14159265358979323846
#define CP_TAU (2.0 * CP_PI)

/* ------------------------------------------------------------------ lifetime */

embi_cpath *embi_cpath_new(void)
{
    embi_cpath *p = embi_calloc(1, sizeof *p);
    p->rc = 1;
    return p;
}

embi_cpath *embi_cpath_retain(embi_cpath *p)
{
    if (p != nullptr) {
        p->rc++;
    }
    return p;
}

void embi_cpath_release(embi_cpath *p)
{
    if (p == nullptr || --p->rc != 0) {
        return;
    }
    embi_vec_free(&p->ops);
    embi_vec_free(&p->pts);
    embi_free(p, sizeof *p);
}

void embi_cpath_reset(embi_cpath *p)
{
    p->ops.len = 0;
    p->pts.len = 0;
    p->has_cur = false;
    p->closed = false;
}

embi_cpath *embi_cpath_clone(const embi_cpath *p)
{
    embi_cpath *c = embi_cpath_new();
    for (uint32_t i = 0; i < p->ops.len; i++) {
        embi_vec_push(&c->ops, p->ops.data[i]);
    }
    for (uint32_t i = 0; i < p->pts.len; i++) {
        embi_vec_push(&c->pts, p->pts.data[i]);
    }
    memcpy(c->cur, p->cur, sizeof c->cur);
    memcpy(c->start, p->start, sizeof c->start);
    c->has_cur = p->has_cur;
    c->closed = p->closed;
    return c;
}

bool embi_cpath_empty(const embi_cpath *p)
{
    return p->ops.len == 0;
}

bool embi_cpath_current(const embi_cpath *p, double *x, double *y)
{
    if (!p->has_cur) {
        return false;
    }
    *x = p->cur[0];
    *y = p->cur[1];
    return true;
}

/* ------------------------------------------------------------------ segments */

static void tp(const double *m, double x, double y, double *ox, double *oy)
{
    if (m == nullptr) {
        *ox = x;
        *oy = y;
    } else {
        cm_apply(m, x, y, ox, oy);
    }
}

static void push_pt(embi_cpath *p, double x, double y)
{
    embi_vec_push(&p->pts, x);
    embi_vec_push(&p->pts, y);
}

/* Starts a new subpath at (x, y) in the path's own space. */
static void raw_move(embi_cpath *p, double x, double y)
{
    embi_vec_push(&p->ops, (uint8_t)CP_M);
    push_pt(p, x, y);
    p->cur[0] = p->start[0] = x;
    p->cur[1] = p->start[1] = y;
    p->has_cur = true;
    p->closed = false;
}

/* After a close the next segment starts a new subpath at the start point. */
static void ensure_open(embi_cpath *p)
{
    if (p->closed) {
        raw_move(p, p->start[0], p->start[1]);
    }
}

void embi_cpath_move_to(embi_cpath *p, const double *m, double x, double y)
{
    if (!isfinite(x) || !isfinite(y)) {
        return;
    }
    double dx, dy;
    tp(m, x, y, &dx, &dy);
    raw_move(p, dx, dy);
}

void embi_cpath_line_to(embi_cpath *p, const double *m, double x, double y)
{
    if (!isfinite(x) || !isfinite(y)) {
        return;
    }
    double dx, dy;
    tp(m, x, y, &dx, &dy);
    if (!p->has_cur) {
        raw_move(p, dx, dy);
        return;
    }
    ensure_open(p);
    embi_vec_push(&p->ops, (uint8_t)CP_L);
    push_pt(p, dx, dy);
    p->cur[0] = dx;
    p->cur[1] = dy;
}

void embi_cpath_quad_to(embi_cpath *p, const double *m, double cx, double cy, double x, double y)
{
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(x) || !isfinite(y)) {
        return;
    }
    double c0, c1, d0, d1;
    tp(m, cx, cy, &c0, &c1);
    tp(m, x, y, &d0, &d1);
    if (!p->has_cur) {
        raw_move(p, c0, c1);
    }
    ensure_open(p);
    embi_vec_push(&p->ops, (uint8_t)CP_Q);
    push_pt(p, c0, c1);
    push_pt(p, d0, d1);
    p->cur[0] = d0;
    p->cur[1] = d1;
}

void embi_cpath_cubic_to(embi_cpath *p, const double *m, double c1x, double c1y, double c2x, double c2y, double x,
                         double y)
{
    if (!isfinite(c1x) || !isfinite(c1y) || !isfinite(c2x) || !isfinite(c2y) || !isfinite(x) || !isfinite(y)) {
        return;
    }
    double a0, a1, b0, b1, d0, d1;
    tp(m, c1x, c1y, &a0, &a1);
    tp(m, c2x, c2y, &b0, &b1);
    tp(m, x, y, &d0, &d1);
    if (!p->has_cur) {
        raw_move(p, a0, a1);
    }
    ensure_open(p);
    embi_vec_push(&p->ops, (uint8_t)CP_C);
    push_pt(p, a0, a1);
    push_pt(p, b0, b1);
    push_pt(p, d0, d1);
    p->cur[0] = d0;
    p->cur[1] = d1;
}

void embi_cpath_close(embi_cpath *p)
{
    if (!p->has_cur || p->closed) {
        return;
    }
    embi_vec_push(&p->ops, (uint8_t)CP_Z);
    p->cur[0] = p->start[0];
    p->cur[1] = p->start[1];
    p->closed = true;
}

/* ------------------------------------------------------------------ arcs */

/* Appends the cubics of the elliptical arc of (cx, cy, rx, ry, rot) from angle a0 through the signed sweep, as segments of
 * at most a quarter turn. The current point is assumed to be the arc's start. The points go through m. */
static void arc_segments(embi_cpath *p, const double *m, double cx, double cy, double rx, double ry, double rot,
                         double a0, double sweep)
{
    int n = (int)ceil(fabs(sweep) / (CP_PI / 2.0) - 1e-9);
    if (n < 1) {
        n = 1;
    }
    double da = sweep / n;
    double k = 4.0 / 3.0 * tan(da / 4.0);
    double cr = cos(rot), sr = sin(rot);
    for (int i = 0; i < n; i++) {
        double t0 = a0 + da * i, t1 = t0 + da;
        double c0 = cos(t0), s0 = sin(t0), c1 = cos(t1), s1 = sin(t1);
        /* Points on the unit circle and the tangent handles, then scaled, rotated and moved. */
        double pt[6][2] = {
            {c0 - k * s0, s0 + k * c0},
            {c1 + k * s1, s1 - k * c1},
            {c1, s1},
        };
        double out[3][2];
        for (int j = 0; j < 3; j++) {
            double ux = pt[j][0] * rx, uy = pt[j][1] * ry;
            out[j][0] = cx + ux * cr - uy * sr;
            out[j][1] = cy + ux * sr + uy * cr;
        }
        embi_cpath_cubic_to(p, m, out[0][0], out[0][1], out[1][0], out[1][1], out[2][0], out[2][1]);
    }
}

int embi_cpath_ellipse(embi_cpath *p, const double *m, double x, double y, double rx, double ry, double rot,
                       double a0, double a1, bool ccw)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(rx) || !isfinite(ry) || !isfinite(rot) || !isfinite(a0) ||
        !isfinite(a1)) {
        return EMBI_CV_OK;
    }
    if (rx < 0 || ry < 0) {
        return EMBI_CV_INDEX_SIZE;
    }
    double sweep;
    if (!ccw && a1 - a0 >= CP_TAU) {
        sweep = CP_TAU;
    } else if (ccw && a0 - a1 >= CP_TAU) {
        sweep = -CP_TAU;
    } else {
        double d = fmod(a1 - a0, CP_TAU);
        if (!ccw) {
            if (d < 0) {
                d += CP_TAU;
            }
            sweep = d;
        } else {
            if (d > 0) {
                d -= CP_TAU;
            }
            sweep = d;
        }
    }
    double cr = cos(rot), sr = sin(rot);
    double ux = rx * cos(a0), uy = ry * sin(a0);
    double sx = x + ux * cr - uy * sr, sy = y + ux * sr + uy * cr;
    embi_cpath_line_to(p, m, sx, sy); /* a move when the path has no subpath */
    if (sweep != 0.0 && (rx != 0.0 || ry != 0.0)) {
        arc_segments(p, m, x, y, rx, ry, rot, a0, sweep);
    }
    return EMBI_CV_OK;
}

int embi_cpath_arc(embi_cpath *p, const double *m, double x, double y, double r, double a0, double a1, bool ccw)
{
    return embi_cpath_ellipse(p, m, x, y, r, r, 0.0, a0, a1, ccw);
}

int embi_cpath_arc_to(embi_cpath *p, const double *m, double x1, double y1, double x2, double y2, double r)
{
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) || !isfinite(r)) {
        return EMBI_CV_OK;
    }
    if (r < 0) {
        return EMBI_CV_INDEX_SIZE;
    }
    if (!p->has_cur) {
        embi_cpath_move_to(p, m, x1, y1);
    }
    /* The current point in the script's (user) space. */
    double x0 = p->cur[0], y0 = p->cur[1];
    if (m != nullptr) {
        double inv[6];
        if (!cm_invert(m, inv)) {
            return EMBI_CV_OK;
        }
        cm_apply(inv, p->cur[0], p->cur[1], &x0, &y0);
    }
    double ax = x0 - x1, ay = y0 - y1, bx = x2 - x1, by = y2 - y1;
    double la = hypot(ax, ay), lb = hypot(bx, by);
    double cross = ax * by - ay * bx;
    if ((x0 == x1 && y0 == y1) || (x1 == x2 && y1 == y2) || r == 0.0 || cross == 0.0 || la == 0.0 || lb == 0.0) {
        embi_cpath_line_to(p, m, x1, y1);
        return EMBI_CV_OK;
    }
    double ux = ax / la, uy = ay / la, vx = bx / lb, vy = by / lb;
    double cosb = ux * vx + uy * vy;
    if (cosb > 1.0) {
        cosb = 1.0;
    } else if (cosb < -1.0) {
        cosb = -1.0;
    }
    double theta = acos(cosb); /* the angle between the rays */
    double sinh_ = sin(theta / 2.0);
    double tanh_ = tan(theta / 2.0);
    if (sinh_ == 0.0 || tanh_ == 0.0 || !isfinite(tanh_)) {
        embi_cpath_line_to(p, m, x1, y1);
        return EMBI_CV_OK;
    }
    double d = r / tanh_;
    double t1x = x1 + ux * d, t1y = y1 + uy * d;
    double t2x = x1 + vx * d, t2y = y1 + vy * d;
    double bxs = ux + vx, bys = uy + vy;
    double bl = hypot(bxs, bys);
    double h = r / sinh_;
    double cx = x1 + bxs / bl * h, cy = y1 + bys / bl * h;
    double s = atan2(t1y - cy, t1x - cx), e = atan2(t2y - cy, t2x - cx);
    double sweep = e - s;
    while (sweep > CP_PI) {
        sweep -= CP_TAU;
    }
    while (sweep < -CP_PI) {
        sweep += CP_TAU;
    }
    embi_cpath_line_to(p, m, t1x, t1y);
    arc_segments(p, m, cx, cy, r, r, 0.0, s, sweep);
    /* Land exactly on the tangent point. */
    embi_cpath_line_to(p, m, t2x, t2y);
    return EMBI_CV_OK;
}

void embi_cpath_rect(embi_cpath *p, const double *m, double x, double y, double w, double h)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;
    }
    embi_cpath_move_to(p, m, x, y);
    embi_cpath_line_to(p, m, x + w, y);
    embi_cpath_line_to(p, m, x + w, y + h);
    embi_cpath_line_to(p, m, x, y + h);
    embi_cpath_close(p);
}

void embi_cpath_round_rect(embi_cpath *p, const double *m, double x, double y, double w, double h,
                           const double radii[8])
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;
    }
    double r[4][2];
    memcpy(r, radii, sizeof r);
    /* Scale the radii down so that the corners fit (CSS Backgrounds 3 section 5.5). */
    double aw = fabs(w), ah = fabs(h);
    double sc = 1.0;
    double sums[4] = {r[0][0] + r[1][0], r[1][1] + r[2][1], r[2][0] + r[3][0], r[3][1] + r[0][1]};
    double lens[4] = {aw, ah, aw, ah};
    for (int i = 0; i < 4; i++) {
        if (sums[i] > lens[i] && sums[i] > 0) {
            double f = lens[i] / sums[i];
            if (f < sc) {
                sc = f;
            }
        }
    }
    if (sc < 1.0) {
        for (int i = 0; i < 4; i++) {
            r[i][0] *= sc;
            r[i][1] *= sc;
        }
    }
    double sx = w < 0 ? -1 : 1, sy = h < 0 ? -1 : 1;
    /* A negative size mirrors the shape about the origin corner: the first radius stays with the corner at (x, y). */
    double ul[2], ur[2], lr[2], ll[2];
    memcpy(ul, r[0], sizeof ul);
    memcpy(ur, r[1], sizeof ur);
    memcpy(lr, r[2], sizeof lr);
    memcpy(ll, r[3], sizeof ll);
    /* Corner arcs in the unmirrored angle convention, then mirrored per axis. */
#define MIRROR(a) ((sy < 0) ? -((sx < 0) ? CP_PI - (a) : (a)) : ((sx < 0) ? CP_PI - (a) : (a)))
    embi_cpath_move_to(p, m, x + sx * ul[0], y);
    embi_cpath_line_to(p, m, x + w - sx * ur[0], y);
    arc_segments(p, m, x + w - sx * ur[0], y + sy * ur[1], ur[0], ur[1], 0.0, MIRROR(1.5 * CP_PI),
                 (sx * sy) * (0.5 * CP_PI));
    embi_cpath_line_to(p, m, x + w, y + h - sy * lr[1]);
    arc_segments(p, m, x + w - sx * lr[0], y + h - sy * lr[1], lr[0], lr[1], 0.0, MIRROR(0.0),
                 (sx * sy) * (0.5 * CP_PI));
    embi_cpath_line_to(p, m, x + sx * ll[0], y + h);
    arc_segments(p, m, x + sx * ll[0], y + h - sy * ll[1], ll[0], ll[1], 0.0, MIRROR(0.5 * CP_PI),
                 (sx * sy) * (0.5 * CP_PI));
    embi_cpath_line_to(p, m, x, y + sy * ul[1]);
    arc_segments(p, m, x + sx * ul[0], y + sy * ul[1], ul[0], ul[1], 0.0, MIRROR(CP_PI), (sx * sy) * (0.5 * CP_PI));
#undef MIRROR
    embi_cpath_close(p);
}

/* ------------------------------------------------------------------ appending */

void embi_cpath_add(embi_cpath *dst, const embi_cpath *src, const double *m)
{
    if (m != nullptr && !cm_finite(m, 6)) {
        return;
    }
    const double *q = src->pts.data;
    for (uint32_t i = 0; i < src->ops.len; i++) {
        switch (src->ops.data[i]) {
        case CP_M:
            embi_cpath_move_to(dst, m, q[0], q[1]);
            q += 2;
            break;
        case CP_L:
            embi_cpath_line_to(dst, m, q[0], q[1]);
            q += 2;
            break;
        case CP_Q:
            embi_cpath_quad_to(dst, m, q[0], q[1], q[2], q[3]);
            q += 4;
            break;
        case CP_C:
            embi_cpath_cubic_to(dst, m, q[0], q[1], q[2], q[3], q[4], q[5]);
            q += 6;
            break;
        default:
            embi_cpath_close(dst);
            break;
        }
    }
}

/* ------------------------------------------------------------------ SVG path data */

typedef struct svgp {
    const char *s, *e;
} svgp;

static void sp_ws(svgp *p)
{
    while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r' || *p->s == '\f')) {
        p->s++;
    }
}

static void sp_sep(svgp *p)
{
    sp_ws(p);
    if (p->s < p->e && *p->s == ',') {
        p->s++;
        sp_ws(p);
    }
}

static bool sp_num(svgp *p, double *out)
{
    sp_ws(p);
    const char *s = p->s;
    char buf[64];
    size_t n = 0;
    const char *q = s;
    if (q < p->e && (*q == '+' || *q == '-')) {
        buf[n++] = *q++;
    }
    bool digits = false;
    while (q < p->e && *q >= '0' && *q <= '9' && n < 60) {
        buf[n++] = *q++;
        digits = true;
    }
    if (q < p->e && *q == '.') {
        buf[n++] = *q++;
        while (q < p->e && *q >= '0' && *q <= '9' && n < 60) {
            buf[n++] = *q++;
            digits = true;
        }
    }
    if (!digits) {
        return false;
    }
    if (q < p->e && (*q == 'e' || *q == 'E')) {
        const char *r = q + 1;
        size_t save = n;
        buf[n++] = 'e';
        if (r < p->e && (*r == '+' || *r == '-')) {
            buf[n++] = *r++;
        }
        bool ed = false;
        while (r < p->e && *r >= '0' && *r <= '9' && n < 62) {
            buf[n++] = *r++;
            ed = true;
        }
        if (ed) {
            q = r;
        } else {
            n = save;
        }
    }
    buf[n] = '\0';
    char *end = nullptr;
    double v = SDL_strtod(buf, &end);
    if (end == buf || !isfinite(v)) {
        return false;
    }
    p->s = q;
    *out = v;
    return true;
}

static bool sp_flag(svgp *p, int *out)
{
    sp_ws(p);
    if (p->s < p->e && (*p->s == '0' || *p->s == '1')) {
        *out = *p->s - '0';
        p->s++;
        return true;
    }
    return false;
}

/* The SVG endpoint arc as cubics from (x0, y0). */
static void svg_arc(embi_cpath *p, double x0, double y0, double rx, double ry, double rot_deg, int large, int sweep,
                    double x, double y)
{
    if (x0 == x && y0 == y) {
        return;
    }
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx == 0.0 || ry == 0.0) {
        embi_cpath_line_to(p, nullptr, x, y);
        return;
    }
    double phi = rot_deg * CP_PI / 180.0;
    double cp = cos(phi), sp = sin(phi);
    double dx2 = (x0 - x) / 2.0, dy2 = (y0 - y) / 2.0;
    double x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0) {
        double s = sqrt(lam);
        rx *= s;
        ry *= s;
    }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = den == 0.0 ? 0.0 : sqrt(num < 0 ? 0 : num / den);
    if (large == sweep) {
        co = -co;
    }
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2.0, cy = sp * cxp + cp * cyp + (y0 + y) / 2.0;
    double th1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double th2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dth = th2 - th1;
    if (sweep && dth < 0) {
        dth += CP_TAU;
    } else if (!sweep && dth > 0) {
        dth -= CP_TAU;
    }
    arc_segments(p, nullptr, cx, cy, rx, ry, phi, th1, dth);
    embi_cpath_line_to(p, nullptr, x, y);
}

void embi_cpath_parse_svg(embi_cpath *p, embi_str d)
{
    svgp ps = {d.ptr, d.ptr + d.len};
    svgp *s = &ps;
    char cmd = 0;
    bool first = true;
    double cx = 0, cy = 0, sx = 0, sy = 0; /* current point and subpath start (user space) */
    double lcx = 0, lcy = 0;               /* last control point for S and T */
    char prev = 0;
    while (true) {
        sp_ws(s);
        if (s->s >= s->e) {
            break;
        }
        char c = *s->s;
        bool is_cmd = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (is_cmd) {
            if (!strchr("MmZzLlHhVvCcSsQqTtAa", c)) {
                return;
            }
            cmd = c;
            s->s++;
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            return;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        if (first && cmd != 'M' && cmd != 'm') {
            return;
        }
        first = false;
        bool rel = cmd >= 'a';
        double a[7];
        int flags[2];
        switch (cmd) {
        case 'M':
        case 'm':
            if (!sp_num(s, &a[0]) || (sp_sep(s), !sp_num(s, &a[1]))) {
                return;
            }
            sp_sep(s);
            cx = rel ? cx + a[0] : a[0];
            cy = rel ? cy + a[1] : a[1];
            embi_cpath_move_to(p, nullptr, cx, cy);
            sx = cx;
            sy = cy;
            prev = 'M';
            break;
        case 'Z':
        case 'z':
            embi_cpath_close(p);
            cx = sx;
            cy = sy;
            prev = 'Z';
            break;
        case 'L':
        case 'l':
            if (!sp_num(s, &a[0]) || (sp_sep(s), !sp_num(s, &a[1]))) {
                return;
            }
            sp_sep(s);
            cx = rel ? cx + a[0] : a[0];
            cy = rel ? cy + a[1] : a[1];
            embi_cpath_line_to(p, nullptr, cx, cy);
            prev = 'L';
            break;
        case 'H':
        case 'h':
            if (!sp_num(s, &a[0])) {
                return;
            }
            sp_sep(s);
            cx = rel ? cx + a[0] : a[0];
            embi_cpath_line_to(p, nullptr, cx, cy);
            prev = 'L';
            break;
        case 'V':
        case 'v':
            if (!sp_num(s, &a[0])) {
                return;
            }
            sp_sep(s);
            cy = rel ? cy + a[0] : a[0];
            embi_cpath_line_to(p, nullptr, cx, cy);
            prev = 'L';
            break;
        case 'C':
        case 'c':
            for (int i = 0; i < 6; i++) {
                if (!sp_num(s, &a[i])) {
                    return;
                }
                sp_sep(s);
            }
            if (rel) {
                a[0] += cx; a[2] += cx; a[4] += cx;
                a[1] += cy; a[3] += cy; a[5] += cy;
            }
            embi_cpath_cubic_to(p, nullptr, a[0], a[1], a[2], a[3], a[4], a[5]);
            lcx = a[2];
            lcy = a[3];
            cx = a[4];
            cy = a[5];
            prev = 'C';
            break;
        case 'S':
        case 's':
            for (int i = 0; i < 4; i++) {
                if (!sp_num(s, &a[i])) {
                    return;
                }
                sp_sep(s);
            }
            if (rel) {
                a[0] += cx; a[2] += cx;
                a[1] += cy; a[3] += cy;
            }
            {
                double c1x = cx, c1y = cy;
                if (prev == 'C') {
                    c1x = 2 * cx - lcx;
                    c1y = 2 * cy - lcy;
                }
                embi_cpath_cubic_to(p, nullptr, c1x, c1y, a[0], a[1], a[2], a[3]);
            }
            lcx = a[0];
            lcy = a[1];
            cx = a[2];
            cy = a[3];
            prev = 'C';
            break;
        case 'Q':
        case 'q':
            for (int i = 0; i < 4; i++) {
                if (!sp_num(s, &a[i])) {
                    return;
                }
                sp_sep(s);
            }
            if (rel) {
                a[0] += cx; a[2] += cx;
                a[1] += cy; a[3] += cy;
            }
            embi_cpath_quad_to(p, nullptr, a[0], a[1], a[2], a[3]);
            lcx = a[0];
            lcy = a[1];
            cx = a[2];
            cy = a[3];
            prev = 'Q';
            break;
        case 'T':
        case 't':
            for (int i = 0; i < 2; i++) {
                if (!sp_num(s, &a[i])) {
                    return;
                }
                sp_sep(s);
            }
            if (rel) {
                a[0] += cx;
                a[1] += cy;
            }
            {
                double qx = cx, qy = cy;
                if (prev == 'Q') {
                    qx = 2 * cx - lcx;
                    qy = 2 * cy - lcy;
                }
                embi_cpath_quad_to(p, nullptr, qx, qy, a[0], a[1]);
                lcx = qx;
                lcy = qy;
            }
            cx = a[0];
            cy = a[1];
            prev = 'Q';
            break;
        default: /* A, a */
            if (!sp_num(s, &a[0]) || (sp_sep(s), !sp_num(s, &a[1])) || (sp_sep(s), !sp_num(s, &a[2])) ||
                (sp_sep(s), !sp_flag(s, &flags[0])) || (sp_sep(s), !sp_flag(s, &flags[1])) ||
                (sp_sep(s), !sp_num(s, &a[3])) || (sp_sep(s), !sp_num(s, &a[4]))) {
                return;
            }
            sp_sep(s);
            if (rel) {
                a[3] += cx;
                a[4] += cy;
            }
            svg_arc(p, cx, cy, a[0], a[1], a[2], flags[0], flags[1], a[3], a[4]);
            cx = a[3];
            cy = a[4];
            prev = 'A';
            break;
        }
    }
}

/* ------------------------------------------------------------------ flattening and hit testing */

typedef EMBI_VEC(double) dvec;

static void flat_cubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3,
                       double tol, dvec *out)
{
    double d = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2);
    int n = (int)ceil(sqrt(d / (tol > 0 ? tol : 0.25)));
    if (n < 4) {
        n = 4;
    } else if (n > 128) {
        n = 128;
    }
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
        embi_vec_push(out, a * x0 + b * x1 + c * x2 + e * x3);
        embi_vec_push(out, a * y0 + b * y1 + c * y2 + e * y3);
    }
}

void cpath_flatten(const embi_cpath *p, double tol, cp_poly_fn fn, void *user)
{
    dvec poly = {0};
    const double *q = p->pts.data;
    double cx = 0, cy = 0;
    for (uint32_t i = 0; i < p->ops.len; i++) {
        switch (p->ops.data[i]) {
        case CP_M:
            if (poly.len >= 2) {
                fn(user, poly.data, poly.len / 2, false);
            }
            poly.len = 0;
            cx = q[0];
            cy = q[1];
            embi_vec_push(&poly, cx);
            embi_vec_push(&poly, cy);
            q += 2;
            break;
        case CP_L:
            cx = q[0];
            cy = q[1];
            embi_vec_push(&poly, cx);
            embi_vec_push(&poly, cy);
            q += 2;
            break;
        case CP_Q:
            flat_cubic(cx, cy, cx + (q[0] - cx) * 2.0 / 3.0, cy + (q[1] - cy) * 2.0 / 3.0,
                       q[2] + (q[0] - q[2]) * 2.0 / 3.0, q[3] + (q[1] - q[3]) * 2.0 / 3.0, q[2], q[3], tol, &poly);
            cx = q[2];
            cy = q[3];
            q += 4;
            break;
        case CP_C:
            flat_cubic(cx, cy, q[0], q[1], q[2], q[3], q[4], q[5], tol, &poly);
            cx = q[4];
            cy = q[5];
            q += 6;
            break;
        default:
            if (poly.len >= 2) {
                double sx = poly.data[0], sy = poly.data[1];
                fn(user, poly.data, poly.len / 2, true);
                poly.len = 0;
                cx = sx;
                cy = sy;
                embi_vec_push(&poly, cx);
                embi_vec_push(&poly, cy);
            }
            break;
        }
    }
    if (poly.len >= 2) {
        fn(user, poly.data, poly.len / 2, false);
    }
    embi_vec_free(&poly);
}

typedef struct wind {
    double x, y;
    int w;
    int crossings;
    bool on; /* the point lies on an edge: the boundary counts as inside */
} wind;

static void wind_edge(wind *k, double x0, double y0, double x1, double y1)
{
    double ex = x1 - x0, ey = y1 - y0;
    double cr = ex * (k->y - y0) - ey * (k->x - x0);
    double len2 = ex * ex + ey * ey;
    if (len2 > 0.0 && cr * cr <= 1e-15 * len2) {
        double dt = ex * (k->x - x0) + ey * (k->y - y0);
        if (dt >= -1e-12 && dt <= len2 + 1e-12) {
            k->on = true;
        }
    }
    if ((y0 <= k->y) != (y1 <= k->y)) {
        double t = (k->y - y0) / (y1 - y0);
        double xi = x0 + t * (x1 - x0);
        if (xi > k->x) {
            k->w += (y1 > y0) ? 1 : -1;
            k->crossings++;
        }
    }
}

static void wind_poly(void *user, const double *xy, uint32_t n, bool closed)
{
    (void)closed;
    wind *k = user;
    if (n < 2) {
        return;
    }
    for (uint32_t i = 0; i + 1 < n; i++) {
        wind_edge(k, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3]);
    }
    /* Every subpath is filled as if closed. */
    wind_edge(k, xy[2 * (n - 1)], xy[2 * (n - 1) + 1], xy[0], xy[1]);
}

bool embi_cpath_contains(const embi_cpath *p, double x, double y, bool evenodd)
{
    wind k = {x, y, 0, 0, false};
    cpath_flatten(p, 0.05, wind_poly, &k);
    return k.on || (evenodd ? (k.crossings & 1) != 0 : k.w != 0);
}

bool embi_cpath_bounds(const embi_cpath *p, double b[4])
{
    if (p->pts.len < 2) {
        return false;
    }
    b[0] = b[2] = p->pts.data[0];
    b[1] = b[3] = p->pts.data[1];
    for (uint32_t i = 2; i + 1 < p->pts.len; i += 2) {
        double x = p->pts.data[i], y = p->pts.data[i + 1];
        b[0] = x < b[0] ? x : b[0];
        b[1] = y < b[1] ? y : b[1];
        b[2] = x > b[2] ? x : b[2];
        b[3] = y > b[3] ? y : b[3];
    }
    return true;
}
