/*
 * Planes of 3D transforms and 3D rendering contexts (R3c, see plane3d.h).
 *
 * Painting. embi_p3_paint opens a context. Its first plane is the box itself; paint_ctx (paint.c) defers every stacking
 * context that is a child of a plane that preserves 3D (embi_p3_defer), and the context paints the deferred planes after
 * the plane that found them, in tree order, so each plane's items are one contiguous block of the list.
 *
 * Ordering. When all blocks are painted, the planes are ordered. Each plane's content bounds are a rectangle in its own
 * space; the matrix t of the plane (its own transform and those of the planes above it in the context, with the
 * perspective of each parent) maps it to the space the context is composed into. After the perspective division that
 * space is a box in which the viewer looks along -z, so a plane in front of another has larger z. A BSP tree over the
 * polygons orders them back to front: the first polygon splits the rest into its front and back half spaces
 * (polygons that cross it are cut in two along the line of intersection, which is a straight line in the plane's own
 * space as well), and the traversal draws the half space away from the viewer, then the coplanar polygons in tree
 * order, then the near half space. A cut is recorded as a line in the plane's space; the piece composes under a clip
 * that is the half plane of that line in the space of the composite.
 */
#include "paint/plane3d.h"

#include "paint/layer.h"

#include <math.h>
#include <string.h>

#define P3_MAXV 24
#define P3_MAXCUT 6
#define P3_MAXPOLY 1024u
#define P3_EPS 1e-3
#define P3_WEPS 1e-4
#define P3_BIG 100000.0f
#define P3_MAX_DEPTH 48u
#define P3_BSP_DEPTH 64u /* the deepest BSP recursion: a chain of parallel planes nests one level per plane */

typedef struct v2 {
    double x, y;
} v2;

/* A cut line in the plane's space: two points on it and a point on the side that is kept. */
typedef struct p3cut {
    v2 a, b, in;
} p3cut;

typedef struct p3rec {
    const embi_frag *f;
    float kx, ky;
    embi_m4 rel;
    uint32_t parent_ctx;
} p3rec;

/* A painted plane: its items [b, e) and the contexts [cb, ce) created while painting it. */
typedef struct p3blk {
    uint32_t b, e;
    uint32_t cb, ce;
    embi_m4 t;
    float bounds[4];
    uint32_t seq;
    bool drop;
    bool moved;
} p3blk;

typedef struct p3poly {
    uint32_t blk;
    int n;
    v2 v[P3_MAXV];
    int ncut;
    p3cut cuts[P3_MAXCUT];
} p3poly;

typedef struct p3piece {
    uint32_t blk;
    int ncut;
    p3cut cuts[P3_MAXCUT];
} p3piece;

struct embi_p3ctx {
    struct embi_p3ctx *prev;
    uint32_t start;                 /* the first item of the context */
    uint32_t xf_parent, clip_parent;
    double a2d[9];                  /* xf_parent as a 3x3 */
    EMBI_VEC(p3blk) blks;
    EMBI_VEC(p3rec) recs;
    const embi_element *cur_el;     /* the element of the plane being painted */
    bool cur_pres;                  /* and whether it preserves 3D */
    uint32_t nseq;
    uint32_t depth;
};

/* ------------------------------------------------------------------ participants */

bool embi_p3_participant(const embi_pst *p, const embi_frag *f)
{
    const struct embi_p3ctx *c = p->c3;
    return c != nullptr && c->cur_pres && c->depth < P3_MAX_DEPTH && f->box != nullptr &&
           c->cur_el == embi_lay_parent_el(f->box) && c->cur_el != nullptr;
}

void embi_p3_defer(embi_pst *p, const embi_frag *f, float kx, float ky, const embi_m4 *rel, uint32_t parent_ctx)
{
    p3rec r = {f, kx, ky, *rel, parent_ctx};
    embi_vec_push(&p->c3->recs, r);
}

/* ------------------------------------------------------------------ bounds */

static void unite(float acc[4], bool *any, const float b[4])
{
    if (!*any) {
        memcpy(acc, b, 4 * sizeof(float));
        *any = true;
        return;
    }
    acc[0] = fminf(acc[0], b[0]);
    acc[1] = fminf(acc[1], b[1]);
    acc[2] = fmaxf(acc[2], b[2]);
    acc[3] = fmaxf(acc[3], b[3]);
}

/* The bounds of the items of the block [b, e) (its two markers excluded), in the space of its items. A nested plane
 * counts as the box of its projected bounds. Returns false when nothing is drawn. */
static bool block_bounds(const embi_dl *d, uint32_t b, uint32_t e, float out[4])
{
    bool any = false;
    for (uint32_t i = b + 1; i + 1 < e; i++) {
        const embi_ditem *it = &d->items.data[i];
        float bx[4];
        if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_PLANE) {
            if (it->aux2 < d->planes.len) {
                const embi_dplane *pl = &d->planes.data[it->aux2];
                float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
                bool ok = false;
                for (int k = 0; k < 4; k++) {
                    float x = pl->bounds[(k & 1) ? 2 : 0], y = pl->bounds[(k & 2) ? 3 : 1];
                    float w = pl->h[6] * x + pl->h[7] * y + pl->h[8];
                    if (!(w > 1e-6f)) {
                        continue;
                    }
                    float px = (pl->h[0] * x + pl->h[1] * y + pl->h[2]) / w;
                    float py = (pl->h[3] * x + pl->h[4] * y + pl->h[5]) / w;
                    mn[0] = fminf(mn[0], px);
                    mn[1] = fminf(mn[1], py);
                    mx[0] = fmaxf(mx[0], px);
                    mx[1] = fmaxf(mx[1], py);
                    ok = true;
                }
                if (ok) {
                    bx[0] = mn[0] - 1.0f;
                    bx[1] = mn[1] - 1.0f;
                    bx[2] = mx[0] + 1.0f;
                    bx[3] = mx[1] + 1.0f;
                    unite(out, &any, bx);
                }
            }
            if (it->aux > i && it->aux < e) {
                i = it->aux;
            }
            continue;
        }
        if (!embi_layer_drawable(it->kind)) {
            continue;
        }
        float pad = 2.0f;
        if (it->kind == EMBI_DI_TEXT) {
            pad += 0.25f * it->rect[3];
        }
        if (embi_layer_item_bounds_pad(d, it, true, pad, bx)) {
            unite(out, &any, bx);
        }
    }
    return any && out[2] > out[0] && out[3] > out[1];
}

/* ------------------------------------------------------------------ painting */

static void process(embi_pst *p, struct embi_p3ctx *c, const p3rec *r, const embi_m4 *t)
{
    embi_dl *d = p->d;
    const embi_style *s = r->f->box != nullptr ? r->f->box->style : nullptr;
    const embi_element *saved_el = c->cur_el;
    bool saved_pres = c->cur_pres;
    c->cur_el = r->f->box != nullptr ? r->f->box->el : nullptr;
    c->cur_pres = embi_lay_preserve3d_used(s);
    c->depth++;
    uint32_t rmark = c->recs.len;

    p3blk blk;
    memset(&blk, 0, sizeof blk);
    blk.t = *t;
    blk.seq = c->nseq++;
    blk.b = d->items.len;
    blk.cb = d->ctxs.len;
    uint32_t planes0 = d->planes.len;
    double h[9];
    embi_m4_plane(t, h);
    bool hidden = embi_lay_backface_hidden(s) && embi_h3_det(h) < 0.0;

    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = EMBI_LS_PLANE;
    open.f[0] = embi_pb_opacity(s);
    embi_vec_push(&d->items, open);
    float bb[4] = {r->kx, r->ky, r->f->w, r->f->h};
    embi_pb_paint_body(p, r->f, r->kx, r->ky, bb, 0, 0, r->parent_ctx, 1.0f);
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = blk.b;
    embi_vec_push(&d->items, end);
    d->items.data[blk.b].aux = d->items.len - 1;
    blk.e = d->items.len;
    blk.ce = d->ctxs.len;
    blk.drop = hidden || !block_bounds(d, blk.b, blk.e, blk.bounds);
    if (blk.drop) {
        /* The block is not drawn: the planes of the contexts nested in it are not referenced either. */
        d->planes.len = planes0;
    }
    embi_vec_push(&c->blks, blk);

    /* The planes this one found are painted next, each after the one before it (their own children first). */
    uint32_t n = c->recs.len - rmark;
    p3rec *kids = nullptr;
    if (n > 0) {
        kids = embi_alloc(n * sizeof *kids);
        memcpy(kids, c->recs.data + rmark, n * sizeof *kids);
        c->recs.len = rmark;
    }
    for (uint32_t i = 0; i < n; i++) {
        embi_m4 tc;
        embi_m4_mul(t, &kids[i].rel, &tc);
        process(p, c, &kids[i], &tc);
    }
    if (kids != nullptr) {
        embi_free(kids, n * sizeof *kids);
    }
    c->depth--;
    c->cur_el = saved_el;
    c->cur_pres = saved_pres;
}

/* ------------------------------------------------------------------ ordering */

/* The point (x, y) of a plane through t: out = (X, Y, Z, W). */
static void map4(const embi_m4 *t, v2 p, double out[4])
{
    embi_m4_point(t, p.x, p.y, 0.0, out);
}

/* Keeps the part of the polygon where fv >= 0 (fv is the value of a linear function at each vertex). Returns the vertex
 * count; ia/ib receive the two points where the boundary crosses zero. */
static int clip_keep(const v2 *in, int n, const double *fv, v2 *out, v2 *ia, v2 *ib, int *ncross)
{
    int m = 0;
    *ncross = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        double fi = fv[i], fj = fv[j];
        if (fi >= 0.0 && m < P3_MAXV) {
            out[m++] = in[i];
        }
        if ((fi > 0.0 && fj < 0.0) || (fi < 0.0 && fj > 0.0)) {
            double tt = fi / (fi - fj);
            v2 q = {in[i].x + (in[j].x - in[i].x) * tt, in[i].y + (in[j].y - in[i].y) * tt};
            if (m < P3_MAXV) {
                out[m++] = q;
            }
            if (*ncross == 0) {
                *ia = q;
            } else {
                *ib = q;
            }
            (*ncross)++;
        }
    }
    return m;
}

static v2 centroid(const v2 *v, int n)
{
    v2 c = {0, 0};
    for (int i = 0; i < n; i++) {
        c.x += v[i].x;
        c.y += v[i].y;
    }
    c.x /= n > 0 ? n : 1;
    c.y /= n > 0 ? n : 1;
    return c;
}

typedef struct bsp {
    const struct embi_p3ctx *c;
    EMBI_VEC(p3poly) all;
    EMBI_VEC(p3piece) out;
} bsp;

/* The polygon's plane in the divided space: unit normal (nz >= 0) and offset. False for a polygon that is edge on. */
static bool plane_of(const bsp *B, const p3poly *q, double nrm[3], double *dist)
{
    const embi_m4 *t = &B->c->blks.data[q->blk].t;
    double pt[P3_MAXV][3];
    for (int i = 0; i < q->n; i++) {
        double o[4];
        map4(t, q->v[i], o);
        double w = o[3] > P3_WEPS ? o[3] : P3_WEPS;
        pt[i][0] = o[0] / w;
        pt[i][1] = o[1] / w;
        pt[i][2] = o[2] / w;
    }
    double nx = 0, ny = 0, nz = 0, cx = 0, cy = 0, cz = 0;
    for (int i = 0; i < q->n; i++) {
        int j = (i + 1) % q->n;
        nx += (pt[i][1] - pt[j][1]) * (pt[i][2] + pt[j][2]);
        ny += (pt[i][2] - pt[j][2]) * (pt[i][0] + pt[j][0]);
        nz += (pt[i][0] - pt[j][0]) * (pt[i][1] + pt[j][1]);
        cx += pt[i][0];
        cy += pt[i][1];
        cz += pt[i][2];
    }
    double len = sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 1e-9)) {
        return false;
    }
    nx /= len;
    ny /= len;
    nz /= len;
    if (nz < 0.0) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    cx /= q->n;
    cy /= q->n;
    cz /= q->n;
    nrm[0] = nx;
    nrm[1] = ny;
    nrm[2] = nz;
    *dist = -(nx * cx + ny * cy + nz * cz);
    return true;
}

static void emit_piece(bsp *B, const p3poly *q)
{
    p3piece pc;
    memset(&pc, 0, sizeof pc);
    pc.blk = q->blk;
    pc.ncut = q->ncut;
    memcpy(pc.cuts, q->cuts, sizeof pc.cuts);
    embi_vec_push(&B->out, pc);
}

/* Sorts the polygon indices idx[0..n) by the sequence of their block (insertion sort: the lists are short). */
static void sort_by_seq(const bsp *B, uint32_t *idx, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = idx[i];
        uint32_t sv = B->c->blks.data[B->all.data[v].blk].seq;
        uint32_t j = i;
        while (j > 0 && B->c->blks.data[B->all.data[idx[j - 1]].blk].seq > sv) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
}

/* The z of the polygon's centroid in the divided space (larger is nearer the viewer). */
static double centroid_z(const bsp *B, const p3poly *q)
{
    const embi_m4 *t = &B->c->blks.data[q->blk].t;
    double o[4];
    map4(t, centroid(q->v, q->n), o);
    return o[2] / (o[3] > P3_WEPS ? o[3] : P3_WEPS);
}

/* Emits the polygons idx[0..n) back to front by the z of their centroids (ties in tree order), without splitting. Used
 * below the deepest BSP level. */
static void emit_sorted(bsp *B, const uint32_t *idx, uint32_t n)
{
    if (n == 0) {
        return;
    }
    double *key = embi_alloc(n * sizeof *key);
    uint32_t *ord = embi_alloc(n * sizeof *ord);
    for (uint32_t i = 0; i < n; i++) {
        key[i] = centroid_z(B, &B->all.data[idx[i]]);
        ord[i] = i;
    }
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = ord[i];
        uint32_t j = i;
        while (j > 0) {
            uint32_t u = ord[j - 1];
            bool later = key[u] > key[v] + P3_EPS ||
                         (fabs(key[u] - key[v]) <= P3_EPS &&
                          B->c->blks.data[B->all.data[idx[u]].blk].seq > B->c->blks.data[B->all.data[idx[v]].blk].seq);
            if (!later) {
                break;
            }
            ord[j] = u;
            j--;
        }
        ord[j] = v;
    }
    for (uint32_t i = 0; i < n; i++) {
        emit_piece(B, &B->all.data[idx[ord[i]]]);
    }
    embi_free(key, n * sizeof *key);
    embi_free(ord, n * sizeof *ord);
}

typedef struct bsp_lists {
    uint32_t *front, *back, *cop;
    uint32_t nf, nb, nc;
} bsp_lists;

/* Places the polygon at index qi of B->all against the splitting plane (nrm, dist): in the front list, the back list, the
 * coplanar list, or cut in two. Its own function so that the recursion of bsp_run keeps a small frame. */
[[gnu::noinline]] static void bsp_place(bsp *B, uint32_t qi, const double nrm[3], double dist, bsp_lists *L)
{
    const p3poly q = B->all.data[qi];
    const embi_m4 *t = &B->c->blks.data[q.blk].t;
    double fv[P3_MAXV];
    double smin = 1e300, smax = -1e300;
    for (int k = 0; k < q.n; k++) {
        double o[4];
        map4(t, q.v[k], o);
        double w = o[3] > P3_WEPS ? o[3] : P3_WEPS;
        double sdist = nrm[0] * o[0] / w + nrm[1] * o[1] / w + nrm[2] * o[2] / w + dist;
        smin = fmin(smin, sdist);
        smax = fmax(smax, sdist);
        fv[k] = nrm[0] * o[0] + nrm[1] * o[1] + nrm[2] * o[2] + dist * o[3];
    }
    if (smax <= P3_EPS && smin >= -P3_EPS) {
        L->cop[L->nc++] = qi;
    } else if (smin >= -P3_EPS) {
        L->front[L->nf++] = qi;
    } else if (smax <= P3_EPS) {
        L->back[L->nb++] = qi;
    } else if (B->all.len + 2 > P3_MAXPOLY || q.ncut >= P3_MAXCUT) {
        /* Too many pieces or cuts: keep the polygon whole on the side of its centroid. */
        v2 cc = centroid(q.v, q.n);
        double o[4];
        map4(t, cc, o);
        double w = o[3] > P3_WEPS ? o[3] : P3_WEPS;
        double sc = nrm[0] * o[0] / w + nrm[1] * o[1] / w + nrm[2] * o[2] / w + dist;
        if (sc >= 0.0) {
            L->front[L->nf++] = qi;
        } else {
            L->back[L->nb++] = qi;
        }
    } else {
        p3poly pf, pb;
        memcpy(&pf, &q, sizeof pf);
        memcpy(&pb, &q, sizeof pb);
        double fb[P3_MAXV];
        for (int k = 0; k < q.n; k++) {
            fb[k] = -fv[k];
        }
        v2 a1 = {0, 0}, b1 = {0, 0}, a2 = {0, 0}, b2 = {0, 0};
        int c1 = 0, c2 = 0;
        pf.n = clip_keep(q.v, q.n, fv, pf.v, &a1, &b1, &c1);
        pb.n = clip_keep(q.v, q.n, fb, pb.v, &a2, &b2, &c2);
        if (pf.n >= 3 && c1 >= 2) {
            pf.cuts[pf.ncut++] = (p3cut){a1, b1, centroid(pf.v, pf.n)};
        }
        if (pb.n >= 3 && c2 >= 2) {
            pb.cuts[pb.ncut++] = (p3cut){a2, b2, centroid(pb.v, pb.n)};
        }
        if (pf.n >= 3) {
            embi_vec_push(&B->all, pf);
            L->front[L->nf++] = B->all.len - 1;
        }
        if (pb.n >= 3) {
            embi_vec_push(&B->all, pb);
            L->back[L->nb++] = B->all.len - 1;
        }
    }
}

static void bsp_run(bsp *B, const uint32_t *idx, uint32_t n, uint32_t depth)
{
    if (n == 0) {
        return;
    }
    if (depth >= P3_BSP_DEPTH) {
        emit_sorted(B, idx, n);
        return;
    }
    double nrm[3], dist = 0.0;
    bool ok;
    {
        p3poly sp = B->all.data[idx[0]];
        ok = plane_of(B, &sp, nrm, &dist);
        if (!ok) {
            emit_piece(B, &sp);
        }
    }
    if (!ok) {
        bsp_run(B, idx + 1, n - 1, depth + 1);
        return;
    }
    bsp_lists L;
    L.front = embi_alloc(n * sizeof *L.front * 3);
    L.back = L.front + n;
    L.cop = L.back + n;
    L.nf = L.nb = L.nc = 0;
    L.cop[L.nc++] = idx[0];
    for (uint32_t i = 1; i < n; i++) {
        bsp_place(B, idx[i], nrm, dist, &L);
    }
    sort_by_seq(B, L.cop, L.nc);
    bsp_run(B, L.back, L.nb, depth + 1);
    for (uint32_t i = 0; i < L.nc; i++) {
        emit_piece(B, &B->all.data[L.cop[i]]);
    }
    bsp_run(B, L.front, L.nf, depth + 1);
    embi_free(L.front, n * sizeof *L.front * 3);
}

/* ------------------------------------------------------------------ assembly */

static bool map_pt(const double h[9], v2 p, v2 *out)
{
    double w = h[6] * p.x + h[7] * p.y + h[8];
    if (!(w > 1e-9)) {
        return false;
    }
    out->x = (h[0] * p.x + h[1] * p.y + h[2]) / w;
    out->y = (h[3] * p.x + h[4] * p.y + h[5]) / w;
    return true;
}

/* The clip that keeps one side of a cut line, in the space of the composite: a rectangle of half a plane, rotated so that
 * its edge lies on the line. Returns parent when the line cannot be mapped. */
static uint32_t cut_clip(embi_pst *p, const double h[9], const p3cut *ct, uint32_t parent)
{
    v2 a, b, q;
    if (!map_pt(h, ct->a, &a) || !map_pt(h, ct->b, &b) || !map_pt(h, ct->in, &q)) {
        return parent;
    }
    double dx = b.x - a.x, dy = b.y - a.y;
    double len = sqrt(dx * dx + dy * dy);
    if (!(len > 1e-6)) {
        return parent;
    }
    double ux = dx / len, uy = dy / len;
    double vx = -uy, vy = ux;
    if ((q.x - a.x) * vx + (q.y - a.y) * vy < 0.0) {
        vx = -vx;
        vy = -vy;
    }
    float m[6] = {(float)ux, (float)uy, (float)vx, (float)vy, (float)a.x, (float)a.y};
    uint32_t xf = embi_pb_xform(p, m);
    float rect[4] = {-P3_BIG, 0.0f, 2.0f * P3_BIG, P3_BIG};
    float zero[4] = {0, 0, 0, 0};
    return embi_pb_clip(p, rect, zero, xf, parent);
}

static void assemble(embi_pst *p, struct embi_p3ctx *c)
{
    embi_dl *d = p->d;
    uint32_t total = d->items.len - c->start;
    if (total == 0 || c->blks.len == 0) {
        return;
    }
    embi_ditem *scratch = embi_alloc(total * sizeof *scratch);
    memcpy(scratch, d->items.data + c->start, total * sizeof *scratch);
    d->items.len = c->start;

    uint32_t nlive = 0;
    for (uint32_t i = 0; i < c->blks.len; i++) {
        if (!c->blks.data[i].drop) {
            nlive++;
        }
    }
    bsp B;
    memset(&B, 0, sizeof B);
    B.c = c;
    if (nlive == 1) {
        for (uint32_t i = 0; i < c->blks.len; i++) {
            if (!c->blks.data[i].drop) {
                p3piece pc;
                memset(&pc, 0, sizeof pc);
                pc.blk = i;
                embi_vec_push(&B.out, pc);
            }
        }
    } else if (nlive > 1) {
        EMBI_VEC(uint32_t) idx = {0};
        for (uint32_t i = 0; i < c->blks.len; i++) {
            const p3blk *k = &c->blks.data[i];
            if (k->drop) {
                continue;
            }
            /* The bounds of the plane, clipped to the half space in front of the viewer (W > 0). */
            v2 quad[4] = {{k->bounds[0], k->bounds[1]}, {k->bounds[2], k->bounds[1]}, {k->bounds[2], k->bounds[3]},
                          {k->bounds[0], k->bounds[3]}};
            double fw[4];
            for (int j = 0; j < 4; j++) {
                double o[4];
                map4(&k->t, quad[j], o);
                fw[j] = o[3] - P3_WEPS;
            }
            p3poly q;
            memset(&q, 0, sizeof q);
            q.blk = i;
            v2 ia, ib;
            int nc2 = 0;
            q.n = clip_keep(quad, 4, fw, q.v, &ia, &ib, &nc2);
            if (q.n < 3) {
                continue;
            }
            /* A plane seen edge on draws nothing. */
            double nrm[3], dist;
            if (!plane_of(&B, &q, nrm, &dist)) {
                continue;
            }
            embi_vec_push(&B.all, q);
            embi_vec_push(&idx, B.all.len - 1);
        }
        uint32_t n0 = idx.len;
        if (n0 > 0) {
            uint32_t *copy = embi_alloc(n0 * sizeof *copy);
            memcpy(copy, idx.data, n0 * sizeof *copy);
            bsp_run(&B, copy, n0, 0);
            embi_free(copy, n0 * sizeof *copy);
        }
        embi_vec_free(&idx);
    }

    for (uint32_t pi = 0; pi < B.out.len; pi++) {
        const p3piece *pc = &B.out.data[pi];
        p3blk *blk = &c->blks.data[pc->blk];
        double ph[9], h[9];
        embi_m4_plane(&blk->t, ph);
        embi_h3_mul(c->a2d, ph, h);
        uint32_t clip = c->clip_parent;
        for (int k = 0; k < pc->ncut; k++) {
            clip = cut_clip(p, h, &pc->cuts[k], clip);
        }
        uint32_t base = d->items.len;
        int64_t delta = (int64_t)base - (int64_t)blk->b;
        for (uint32_t i = blk->b; i < blk->e; i++) {
            embi_ditem it = scratch[i - c->start];
            if (it.kind == EMBI_DI_LAYER_BEGIN || it.kind == EMBI_DI_LAYER_END) {
                it.aux = (uint32_t)((int64_t)it.aux + delta);
            }
            embi_vec_push(&d->items, it);
        }
        embi_dplane pl;
        for (int k = 0; k < 9; k++) {
            pl.h[k] = (float)h[k];
        }
        memcpy(pl.bounds, blk->bounds, sizeof pl.bounds);
        d->items.data[base].aux2 = d->planes.len;
        d->items.data[base].clip = clip;
        embi_vec_push(&d->planes, pl);
        if (!blk->moved) {
            blk->moved = true;
            for (uint32_t k = blk->cb; k < blk->ce && k < d->ctxs.len; k++) {
                d->ctxs.data[k].first = (uint32_t)((int64_t)d->ctxs.data[k].first + delta);
                d->ctxs.data[k].end = (uint32_t)((int64_t)d->ctxs.data[k].end + delta);
            }
        }
    }
    /* The contexts of a plane that is not drawn are empty. */
    for (uint32_t i = 0; i < c->blks.len; i++) {
        const p3blk *k = &c->blks.data[i];
        if (!k->moved) {
            for (uint32_t j = k->cb; j < k->ce && j < d->ctxs.len; j++) {
                d->ctxs.data[j].first = d->ctxs.data[j].end = c->start;
            }
        }
    }
    embi_vec_free(&B.all);
    embi_vec_free(&B.out);
    embi_free(scratch, total * sizeof *scratch);
}

void embi_p3_paint(embi_pst *p, const embi_frag *f, float kx, float ky, const embi_m4 *rel, uint32_t xf_parent,
                   uint32_t clip_parent, uint32_t parent_ctx)
{
    embi_dl *d = p->d;
    struct embi_p3ctx c;
    memset(&c, 0, sizeof c);
    c.prev = p->c3;
    c.start = d->items.len;
    c.xf_parent = xf_parent;
    c.clip_parent = clip_parent;
    const float *m = d->xforms.data[xf_parent].m;
    c.a2d[0] = m[0];
    c.a2d[1] = m[2];
    c.a2d[2] = m[4];
    c.a2d[3] = m[1];
    c.a2d[4] = m[3];
    c.a2d[5] = m[5];
    c.a2d[8] = 1.0;
    p->c3 = &c;
    p3rec r = {f, kx, ky, *rel, parent_ctx};
    process(p, &c, &r, rel);
    p->c3 = c.prev;
    assemble(p, &c);
    embi_vec_free(&c.blks);
    embi_vec_free(&c.recs);
}
