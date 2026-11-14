/*
 * Value parsers of the SVG module (R4c): numbers, lengths, the transform list, viewBox and preserveAspectRatio, and path
 * data (SVG 2 section 9.3, with the error handling of its "Error handling" part: the path is rendered up to the first error).
 */
#include "svg/svg_int.h"

#include <SDL3/SDL_stdinc.h>

const char *svg_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) {
        p++;
    }
    return p;
}

const char *svg_skip_wsp_comma(const char *p, const char *end)
{
    p = svg_skip_ws(p, end);
    if (p < end && *p == ',') {
        p = svg_skip_ws(p + 1, end);
    }
    return p;
}

bool svg_number(const char **pp, const char *end, double *out)
{
    const char *p = *pp;
    const char *s = p;
    if (p < end && (*p == '+' || *p == '-')) {
        p++;
    }
    const char *digits = p;
    while (p < end && *p >= '0' && *p <= '9') {
        p++;
    }
    bool any = p > digits;
    if (p < end && *p == '.') {
        const char *f = p + 1;
        while (f < end && *f >= '0' && *f <= '9') {
            f++;
        }
        if (f > p + 1) {
            any = true;
            p = f;
        } else if (any) {
            p = f; /* "5." is a number */
        }
    }
    if (!any) {
        return false;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        if (q < end && (*q == '+' || *q == '-')) {
            q++;
        }
        const char *ed = q;
        while (q < end && *q >= '0' && *q <= '9') {
            q++;
        }
        /* an exponent needs digits; "1em" keeps the e as the unit's */
        if (q > ed) {
            p = q;
        }
    }
    char tmp[64];
    size_t n = (size_t)(p - s);
    if (n >= sizeof tmp) {
        /* absurdly long number: parse a clipped copy */
        n = sizeof tmp - 1;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    double v = SDL_strtod(tmp, nullptr);
    if (!isfinite(v)) {
        v = v > 0 ? 3.4e38 : -3.4e38;
    }
    *out = v;
    *pp = p;
    return true;
}

uint32_t svg_number_list(const char *s, uint32_t len, double *out, uint32_t cap, bool *ok)
{
    const char *p = s, *end = s + len;
    uint32_t n = 0;
    *ok = true;
    p = svg_skip_ws(p, end);
    while (p < end) {
        double v;
        if (!svg_number(&p, end, &v)) {
            *ok = false;
            break;
        }
        if (n < cap) {
            out[n++] = v;
        } else {
            *ok = false;
            break;
        }
        p = svg_skip_wsp_comma(p, end);
    }
    return n;
}

bool svg_parse_length(const char *s, uint32_t len, svg_len *out)
{
    const char *p = svg_skip_ws(s, s + len), *end = s + len;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\f')) {
        end--;
    }
    double v;
    if (!svg_number(&p, end, &v)) {
        return false;
    }
    uint8_t unit = SVGL_NONE;
    size_t rest = (size_t)(end - p);
    if (rest == 0) {
        unit = SVGL_NONE;
    } else if (rest == 1 && p[0] == '%') {
        unit = SVGL_PCT;
    } else if (rest == 2) {
        char a = (char)SDL_tolower((unsigned char)p[0]), b = (char)SDL_tolower((unsigned char)p[1]);
        if (a == 'p' && b == 'x') {
            unit = SVGL_PX;
        } else if (a == 'e' && b == 'm') {
            unit = SVGL_EM;
        } else if (a == 'e' && b == 'x') {
            unit = SVGL_EX;
        } else if (a == 'i' && b == 'n') {
            unit = SVGL_IN;
        } else if (a == 'c' && b == 'm') {
            unit = SVGL_CM;
        } else if (a == 'm' && b == 'm') {
            unit = SVGL_MM;
        } else if (a == 'p' && b == 't') {
            unit = SVGL_PT;
        } else if (a == 'p' && b == 'c') {
            unit = SVGL_PC;
        } else {
            return false;
        }
    } else {
        return false;
    }
    out->v = (float)v;
    out->unit = unit;
    return true;
}

float svg_len_px(svg_len l, float fs, float pct_base)
{
    switch (l.unit) {
    case SVGL_EM:
        return l.v * fs;
    case SVGL_EX:
        return l.v * fs * 0.5f;
    case SVGL_PCT:
        return l.v * pct_base / 100.0f;
    case SVGL_IN:
        return l.v * 96.0f;
    case SVGL_CM:
        return l.v * 96.0f / 2.54f;
    case SVGL_MM:
        return l.v * 96.0f / 25.4f;
    case SVGL_PT:
        return l.v * 96.0f / 72.0f;
    case SVGL_PC:
        return l.v * 16.0f;
    default:
        return l.v;
    }
}

/* ------------------------------------------------------------------ transform list */

bool svg_parse_transform(const char *s, uint32_t len, float m[6])
{
    m_identity(m);
    const char *p = s, *end = s + len;
    float acc[6];
    m_identity(acc);
    p = svg_skip_ws(p, end);
    while (p < end) {
        const char *name = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            p++;
        }
        size_t nl = (size_t)(p - name);
        p = svg_skip_ws(p, end);
        if (nl == 0 || p >= end || *p != '(') {
            return false;
        }
        p = svg_skip_ws(p + 1, end);
        double a[6];
        uint32_t n = 0;
        while (p < end && *p != ')') {
            double v;
            if (n >= 6 || !svg_number(&p, end, &v)) {
                return false;
            }
            a[n++] = v;
            p = svg_skip_wsp_comma(p, end);
        }
        if (p >= end) {
            return false;
        }
        p++; /* ) */
        float t[6];
        m_identity(t);
        if (nl == 6 && memcmp(name, "matrix", 6) == 0) {
            if (n != 6) {
                return false;
            }
            for (int i = 0; i < 6; i++) {
                t[i] = (float)a[i];
            }
        } else if (nl == 9 && memcmp(name, "translate", 9) == 0) {
            if (n != 1 && n != 2) {
                return false;
            }
            t[4] = (float)a[0];
            t[5] = n == 2 ? (float)a[1] : 0.0f;
        } else if (nl == 5 && memcmp(name, "scale", 5) == 0) {
            if (n != 1 && n != 2) {
                return false;
            }
            t[0] = (float)a[0];
            t[3] = n == 2 ? (float)a[1] : (float)a[0];
        } else if (nl == 6 && memcmp(name, "rotate", 6) == 0) {
            if (n != 1 && n != 3) {
                return false;
            }
            double r = a[0] * 3.14159265358979323846 / 180.0;
            float c = (float)cos(r), sn = (float)sin(r);
            t[0] = c;
            t[1] = sn;
            t[2] = -sn;
            t[3] = c;
            if (n == 3) {
                float cx = (float)a[1], cy = (float)a[2];
                t[4] = cx - c * cx + sn * cy;
                t[5] = cy - sn * cx - c * cy;
            }
        } else if (nl == 5 && memcmp(name, "skewX", 5) == 0) {
            if (n != 1) {
                return false;
            }
            t[2] = (float)tan(a[0] * 3.14159265358979323846 / 180.0);
        } else if (nl == 5 && memcmp(name, "skewY", 5) == 0) {
            if (n != 1) {
                return false;
            }
            t[1] = (float)tan(a[0] * 3.14159265358979323846 / 180.0);
        } else {
            return false;
        }
        m_mul(acc, t, acc);
        p = svg_skip_wsp_comma(p, end);
    }
    memcpy(m, acc, sizeof acc);
    return true;
}

/* ------------------------------------------------------------------ viewBox and preserveAspectRatio */

svg_par svg_parse_par(const char *s, uint32_t len)
{
    svg_par r = {5, false};
    const char *p = svg_skip_ws(s, s + len), *end = s + len;
    if (end - p >= 5 && memcmp(p, "defer", 5) == 0 && (end - p == 5 || p[5] == ' ')) {
        p = svg_skip_ws(p + 5, end);
    }
    const char *a = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n') {
        p++;
    }
    size_t al = (size_t)(p - a);
    if (al == 4 && memcmp(a, "none", 4) == 0) {
        r.align = 0;
    } else if (al == 8 && a[0] == 'x' && a[4] == 'Y') {
        int xi = -1, yi = -1;
        if (memcmp(a + 1, "Min", 3) == 0) {
            xi = 0;
        } else if (memcmp(a + 1, "Mid", 3) == 0) {
            xi = 1;
        } else if (memcmp(a + 1, "Max", 3) == 0) {
            xi = 2;
        }
        if (memcmp(a + 5, "Min", 3) == 0) {
            yi = 0;
        } else if (memcmp(a + 5, "Mid", 3) == 0) {
            yi = 1;
        } else if (memcmp(a + 5, "Max", 3) == 0) {
            yi = 2;
        }
        if (xi < 0 || yi < 0) {
            return (svg_par){5, false};
        }
        r.align = (uint8_t)(1 + yi * 3 + xi);
    } else {
        return (svg_par){5, false};
    }
    p = svg_skip_ws(p, end);
    if (end - p == 5 && memcmp(p, "slice", 5) == 0) {
        r.slice = true;
    } else if (end - p != 0 && !(end - p == 4 && memcmp(p, "meet", 4) == 0)) {
        return (svg_par){5, false};
    }
    return r;
}

void svg_viewbox_matrix(const float vb[4], float vw, float vh, svg_par par, float m[6])
{
    float sx = vb[2] > 0 ? vw / vb[2] : 1.0f, sy = vb[3] > 0 ? vh / vb[3] : 1.0f;
    if (par.align == 0) {
        m[0] = sx;
        m[1] = 0;
        m[2] = 0;
        m[3] = sy;
        m[4] = -vb[0] * sx;
        m[5] = -vb[1] * sy;
        return;
    }
    float s = par.slice ? fmaxf(sx, sy) : fminf(sx, sy);
    int xi = (par.align - 1) % 3, yi = (par.align - 1) / 3;
    float tx = -vb[0] * s + (vw - vb[2] * s) * (0.5f * (float)xi);
    float ty = -vb[1] * s + (vh - vb[3] * s) * (0.5f * (float)yi);
    m[0] = s;
    m[1] = 0;
    m[2] = 0;
    m[3] = s;
    m[4] = tx;
    m[5] = ty;
}

/* ------------------------------------------------------------------ path data */

void svg_verts_free(svg_verts *vs)
{
    if (vs->v != nullptr) {
        embi_free(vs->v, (size_t)vs->cap * sizeof *vs->v);
    }
    memset(vs, 0, sizeof *vs);
}

static void verts_push(svg_verts *vs, svg_mvert v)
{
    if (vs->n == vs->cap) {
        uint32_t nc = vs->cap != 0 ? vs->cap * 2 : 16;
        vs->v = embi_realloc(vs->v, (size_t)vs->cap * sizeof *vs->v, (size_t)nc * sizeof *vs->v);
        vs->cap = nc;
    }
    vs->v[vs->n++] = v;
}

/* A path segment as the marker code sees it: the tangents at both ends. */
typedef struct pseg {
    float x0, y0, x1, y1;
    float t0x, t0y, t1x, t1y;
} pseg;

typedef struct msub {
    float sx, sy;
    bool closed;
    EMBI_VEC(pseg) segs;
} msub;

static bool nz(float x, float y)
{
    return x != 0.0f || y != 0.0f;
}

/* The tangent angle at the ends of an elliptical arc (endpoint parameterization, SVG 2 appendix B.2.4). */
static void arc_tangents(float x0, float y0, float rx, float ry, float phi_deg, bool fa, bool fs, float x1, float y1, float *t0x,
                         float *t0y, float *t1x, float *t1y)
{
    *t0x = x1 - x0;
    *t0y = y1 - y0;
    *t1x = *t0x;
    *t1y = *t0y;
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx == 0 || ry == 0 || (x0 == x1 && y0 == y1)) {
        return;
    }
    double phi = phi_deg * 3.14159265358979323846 / 180.0, cp = cos(phi), sp = sin(phi);
    double dx = (x0 - x1) / 2.0, dy = (y0 - y1) / 2.0;
    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = (x1p * x1p) / ((double)rx * rx) + (y1p * y1p) / ((double)ry * ry);
    double rxd = rx, ryd = ry;
    if (lam > 1) {
        double sl = sqrt(lam);
        rxd *= sl;
        ryd *= sl;
    }
    double num = rxd * rxd * ryd * ryd - rxd * rxd * y1p * y1p - ryd * ryd * x1p * x1p;
    double den = rxd * rxd * y1p * y1p + ryd * ryd * x1p * x1p;
    double co = den > 0 ? sqrt(fmax(0.0, num / den)) : 0.0;
    if (fa == fs) {
        co = -co;
    }
    double cxp = co * rxd * y1p / ryd, cyp = -co * ryd * x1p / rxd;
    double th1 = atan2((y1p - cyp) / ryd, (x1p - cxp) / rxd);
    double th2 = atan2((-y1p - cyp) / ryd, (-x1p - cxp) / rxd);
    /* derivative at theta (unrotated): (-rx sin, ry cos) * direction */
    double dir = fs ? 1.0 : -1.0;
    double ux0 = -rxd * sin(th1) * dir, uy0 = ryd * cos(th1) * dir;
    double ux1 = -rxd * sin(th2) * dir, uy1 = ryd * cos(th2) * dir;
    *t0x = (float)(cp * ux0 - sp * uy0);
    *t0y = (float)(sp * ux0 + cp * uy0);
    *t1x = (float)(cp * ux1 - sp * uy1);
    *t1y = (float)(sp * ux1 + cp * uy1);
}

static void flush_sub(msub *sb, svg_verts *vs, bool *first_vertex_done, uint32_t *start_index)
{
    (void)first_vertex_done;
    (void)start_index;
    uint32_t n = sb->segs.len;
    /* vertex 0 is the subpath start; vertex j is the end of segment j */
    for (uint32_t j = 0; j <= n; j++) {
        svg_mvert v;
        memset(&v, 0, sizeof v);
        v.seg = 1;
        if (j == 0) {
            v.x = sb->sx;
            v.y = sb->sy;
        } else {
            v.x = sb->segs.data[j - 1].x1;
            v.y = sb->segs.data[j - 1].y1;
        }
        /* incoming */
        if (j > 0) {
            const pseg *s = &sb->segs.data[j - 1];
            if (nz(s->t1x, s->t1y)) {
                v.ain = atan2f(s->t1y, s->t1x);
                v.has_in = true;
            }
        } else if (sb->closed && n > 0) {
            const pseg *s = &sb->segs.data[n - 1];
            if (nz(s->t1x, s->t1y)) {
                v.ain = atan2f(s->t1y, s->t1x);
                v.has_in = true;
            }
        }
        /* outgoing */
        if (j < n) {
            const pseg *s = &sb->segs.data[j];
            if (nz(s->t0x, s->t0y)) {
                v.aout = atan2f(s->t0y, s->t0x);
                v.has_out = true;
            }
        } else if (sb->closed && n > 0) {
            const pseg *s = &sb->segs.data[0];
            if (nz(s->t0x, s->t0y)) {
                v.aout = atan2f(s->t0y, s->t0x);
                v.has_out = true;
            }
        }
        verts_push(vs, v);
    }
    embi_vec_free(&sb->segs);
}

embi_path *svg_parse_path(const char *d, uint32_t len, float tol, svg_verts *verts, bool *err)
{
    embi_path *path = svgp_new(tol);
    const char *p = d, *end = d + len;
    float cx = 0, cy = 0, sx = 0, sy = 0; /* current point and subpath start */
    float lcx = 0, lcy = 0;               /* the last control point (S, T) */
    char prev = 0;                        /* the last command letter, uppercased */
    char cmd = 0;
    bool bad = false;
    bool first = true;
    msub sub;
    memset(&sub, 0, sizeof sub);
    bool in_sub = false;
    bool z_cont = false; /* the current subpath is the empty one a closepath leaves behind */
    svg_verts local;
    memset(&local, 0, sizeof local);
    bool fvd = false;
    uint32_t si = 0;

    p = svg_skip_ws(p, end);
    while (p < end && !bad) {
        char ch = *p;
        bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (letter) {
            if (!SDL_strchr("MmLlHhVvCcSsQqTtAaZz", ch)) {
                bad = true;
                break;
            }
            cmd = ch;
            p++;
        } else if (cmd != 0 && (prev != 'Z') && ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-')) {
            /* implicit repetition; moveto repeats as lineto */
            if (cmd == 'M') {
                cmd = 'L';
            } else if (cmd == 'm') {
                cmd = 'l';
            }
        } else {
            bad = true;
            break;
        }
        if (first && cmd != 'M' && cmd != 'm') {
            bad = true;
            break;
        }
        first = false;
        bool rel = cmd >= 'a';
        char up = (char)(rel ? cmd - 'a' + 'A' : cmd);
        double a[7];
        int na = 0;
        switch (up) {
        case 'M': case 'L': case 'T': na = 2; break;
        case 'H': case 'V': na = 1; break;
        case 'C': na = 6; break;
        case 'S': case 'Q': na = 4; break;
        case 'A': na = 7; break;
        default: na = 0; break;
        }
        p = svg_skip_ws(p, end);
        bool ok = true;
        for (int i = 0; i < na && ok; i++) {
            if (up == 'A' && (i == 3 || i == 4)) {
                if (p < end && (*p == '0' || *p == '1')) {
                    a[i] = *p - '0';
                    p++;
                } else {
                    ok = false;
                }
            } else if (!svg_number(&p, end, &a[i])) {
                ok = false;
            }
            if (ok) {
                p = svg_skip_wsp_comma(p, end);
            }
        }
        if (!ok) {
            bad = true;
            break;
        }
        float ox = rel ? cx : 0.0f, oy = rel ? cy : 0.0f;
        pseg sg;
        memset(&sg, 0, sizeof sg);
        bool have_seg = false;
        switch (up) {
        case 'M': {
            float x = ox + (float)a[0], y = oy + (float)a[1];
            if (in_sub && verts != nullptr && !(z_cont && sub.segs.len == 0)) {
                flush_sub(&sub, &local, &fvd, &si);
            } else if (in_sub) {
                embi_vec_free(&sub.segs);
            }
            z_cont = false;
            memset(&sub, 0, sizeof sub);
            sub.sx = x;
            sub.sy = y;
            in_sub = true;
            (void)svgp_move(path, x, y);
            cx = sx = x;
            cy = sy = y;
            lcx = cx;
            lcy = cy;
            break;
        }
        case 'L': {
            float x = ox + (float)a[0], y = oy + (float)a[1];
            sg = (pseg){cx, cy, x, y, x - cx, y - cy, x - cx, y - cy};
            have_seg = true;
            (void)svgp_line(path, x, y);
            cx = x;
            cy = y;
            lcx = cx;
            lcy = cy;
            break;
        }
        case 'H': {
            float x = ox + (float)a[0];
            sg = (pseg){cx, cy, x, cy, x - cx, 0, x - cx, 0};
            have_seg = true;
            (void)svgp_line(path, x, cy);
            cx = x;
            lcx = cx;
            lcy = cy;
            break;
        }
        case 'V': {
            float y = (rel ? cy : 0.0f) + (float)a[0];
            sg = (pseg){cx, cy, cx, y, 0, y - cy, 0, y - cy};
            have_seg = true;
            (void)svgp_line(path, cx, y);
            cy = y;
            lcx = cx;
            lcy = cy;
            break;
        }
        case 'C': case 'S': {
            float c1x, c1y, c2x, c2y, x, y;
            if (up == 'C') {
                c1x = ox + (float)a[0];
                c1y = oy + (float)a[1];
                c2x = ox + (float)a[2];
                c2y = oy + (float)a[3];
                x = ox + (float)a[4];
                y = oy + (float)a[5];
            } else {
                if (prev == 'C' || prev == 'S') {
                    c1x = 2 * cx - lcx;
                    c1y = 2 * cy - lcy;
                } else {
                    c1x = cx;
                    c1y = cy;
                }
                c2x = ox + (float)a[0];
                c2y = oy + (float)a[1];
                x = ox + (float)a[2];
                y = oy + (float)a[3];
            }
            float t0x = c1x - cx, t0y = c1y - cy;
            if (!nz(t0x, t0y)) {
                t0x = c2x - cx;
                t0y = c2y - cy;
            }
            if (!nz(t0x, t0y)) {
                t0x = x - cx;
                t0y = y - cy;
            }
            float t1x = x - c2x, t1y = y - c2y;
            if (!nz(t1x, t1y)) {
                t1x = x - c1x;
                t1y = y - c1y;
            }
            if (!nz(t1x, t1y)) {
                t1x = x - cx;
                t1y = y - cy;
            }
            sg = (pseg){cx, cy, x, y, t0x, t0y, t1x, t1y};
            have_seg = true;
            (void)svgp_cubic(path, c1x, c1y, c2x, c2y, x, y);
            lcx = c2x;
            lcy = c2y;
            cx = x;
            cy = y;
            break;
        }
        case 'Q': case 'T': {
            float qx, qy, x, y;
            if (up == 'Q') {
                qx = ox + (float)a[0];
                qy = oy + (float)a[1];
                x = ox + (float)a[2];
                y = oy + (float)a[3];
            } else {
                if (prev == 'Q' || prev == 'T') {
                    qx = 2 * cx - lcx;
                    qy = 2 * cy - lcy;
                } else {
                    qx = cx;
                    qy = cy;
                }
                x = ox + (float)a[0];
                y = oy + (float)a[1];
            }
            float t0x = qx - cx, t0y = qy - cy;
            if (!nz(t0x, t0y)) {
                t0x = x - cx;
                t0y = y - cy;
            }
            float t1x = x - qx, t1y = y - qy;
            if (!nz(t1x, t1y)) {
                t1x = x - cx;
                t1y = y - cy;
            }
            sg = (pseg){cx, cy, x, y, t0x, t0y, t1x, t1y};
            have_seg = true;
            (void)svgp_quad(path, qx, qy, x, y);
            lcx = qx;
            lcy = qy;
            cx = x;
            cy = y;
            break;
        }
        case 'A': {
            float rx = (float)a[0], ry = (float)a[1], rot = (float)a[2];
            bool fa = a[3] != 0, fs = a[4] != 0;
            float x = ox + (float)a[5], y = oy + (float)a[6];
            float t0x, t0y, t1x, t1y;
            arc_tangents(cx, cy, rx, ry, rot, fa, fs, x, y, &t0x, &t0y, &t1x, &t1y);
            sg = (pseg){cx, cy, x, y, t0x, t0y, t1x, t1y};
            have_seg = true;
            (void)svgp_arc(path, rx, ry, rot * 3.14159265358979323846f / 180.0f, fa, fs, x, y);
            cx = x;
            cy = y;
            lcx = cx;
            lcy = cy;
            break;
        }
        case 'Z': {
            sg = (pseg){cx, cy, sx, sy, sx - cx, sy - cy, sx - cx, sy - cy};
            /* a closepath that is already at the start is no segment of its own: the vertex that ends the last segment is
             * also the end of the subpath, so it gets no extra marker */
            have_seg = !(cx == sx && cy == sy && sub.segs.len > 0);
            (void)svgp_close(path);
            if (in_sub) {
                sub.closed = true;
            }
            cx = sx;
            cy = sy;
            lcx = cx;
            lcy = cy;
            break;
        }
        default:
            break;
        }
        if (have_seg && in_sub && verts != nullptr) {
            embi_vec_push(&sub.segs, sg);
            z_cont = false;
        }
        prev = up;
        if (up == 'Z') {
            /* the next subpath starts where this one did; a new M or any drawing command is a new subpath there */
            if (verts != nullptr && in_sub) {
                flush_sub(&sub, &local, &fvd, &si);
                memset(&sub, 0, sizeof sub);
                sub.sx = sx;
                sub.sy = sy;
                /* the continuation starts a subpath at the same point */
            } else if (in_sub) {
                embi_vec_free(&sub.segs);
                memset(&sub, 0, sizeof sub);
                sub.sx = sx;
                sub.sy = sy;
            }
            in_sub = true;
            z_cont = true;
            sub.closed = false;
            sub.sx = sx;
            sub.sy = sy;
            sub.segs.len = 0;
            cmd = 'Z';
        }
        p = svg_skip_ws(p, end);
        if (up != 'Z' && p < end) {
            /* repetition is decided at the top of the loop */
        }
    }
    if (in_sub) {
        if (verts != nullptr) {
            /* a trailing subpath that only holds the point left by a closepath adds no vertices */
            if (!(z_cont && sub.segs.len == 0)) {
                flush_sub(&sub, &local, &fvd, &si);
            } else {
                embi_vec_free(&sub.segs);
            }
        } else {
            embi_vec_free(&sub.segs);
        }
    }
    if (verts != nullptr) {
        if (local.n > 0) {
            local.v[0].seg = 0;
            local.v[local.n - 1].seg = 2;
        }
        *verts = local;
    } else {
        svg_verts_free(&local);
    }
    if (err != nullptr) {
        *err = bad;
    }
    return path;
}

/* The marker vertices of a polyline or polygon (n points in xy pairs): straight segments. */
void svg_verts_from_points(svg_verts *vs, const double *xy, uint32_t n, bool closed)
{
    /* a polygon is the path M p0 L p1 ... L pn-1 Z: its end vertex is the closepath's, back at p0 */
    uint32_t total = closed && n > 0 ? n + 1 : n;
    for (uint32_t k = 0; k < total; k++) {
        uint32_t i = k % (n > 0 ? n : 1);
        svg_mvert v;
        memset(&v, 0, sizeof v);
        v.x = (float)xy[2 * i];
        v.y = (float)xy[2 * i + 1];
        v.seg = k == 0 ? 0 : k + 1 == total ? 2 : 1;
        bool has_pi = closed || k > 0, has_ni = closed || k + 1 < total;
        uint32_t pi = closed ? (i + n - 1) % n : (k > 0 ? k - 1 : 0);
        uint32_t ni = closed ? (i + 1) % n : (k + 1 < total ? k + 1 : 0);
        if (has_pi && (xy[2 * i] != xy[2 * pi] || xy[2 * i + 1] != xy[2 * pi + 1])) {
            v.ain = atan2f((float)(xy[2 * i + 1] - xy[2 * pi + 1]), (float)(xy[2 * i] - xy[2 * pi]));
            v.has_in = true;
        }
        if (has_ni && (xy[2 * ni] != xy[2 * i] || xy[2 * ni + 1] != xy[2 * i + 1])) {
            v.aout = atan2f((float)(xy[2 * ni + 1] - xy[2 * i + 1]), (float)(xy[2 * ni] - xy[2 * i]));
            v.has_out = true;
        }
        verts_push(vs, v);
    }
}
