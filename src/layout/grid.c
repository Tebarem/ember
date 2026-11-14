/*
 * Grid containers (CSS Grid 2, W3f section 7). The container is a block box whose content is laid out
 * here: block_pass (block.c) sizes the container and calls embi_grid_content with the content box
 * geometry, which places the items into the container's fragment. Intrinsic widths come from
 * embi_grid_intrinsic (size.c).
 *
 * The steps of embi_grid_content:
 *   - the explicit templates (grid-template-rows and -columns, with repeat() and the line names, and
 *     the areas of grid-template-areas), and the implicit track sizes (grid-auto-rows and -columns);
 *   - the items (in-flow children in order-modified document order), placed by grid_place.c;
 *   - the column tracks (section 12) from the items' contributions, then the row tracks from the items'
 *     heights at their column widths (baseline-aligned items add their shim), then the columns again
 *     and the rows once more when a column contribution depends on the row sizes (aspect ratios,
 *     section 12.1 step 3);
 *   - the items are laid out in their areas, aligned (justify-self, align-self, auto margins and
 *     baselines, safe and unsafe), and the tracks are distributed by justify-content and align-content;
 *   - the out-of-flow children get their containing block (a grid area, section 9) or the content box
 *     as their static position, and are placed by position.c.
 *
 * Subgrid. A grid item that is a grid container with subgrid in an axis takes its tracks in that axis
 * from the parent's tracks it spans. For the sizing of the parent, its items contribute to the
 * parent's tracks as if they were the parent's own (embi grid "gather"), with the subgrid's margin,
 * border and padding at its edges, and the difference of the gaps, added to the margins of the items at
 * the edges (the xm fields). When the subgrid is laid out, the parent hands it its sized tracks through
 * c->gsub, and the subgrid positions its items on them. Baseline groups are formed over all the
 * items of the nested grids that share a track of the root.
 *
 * Coordinates. Positions of tracks and items are logical (inline start, block start) from the content
 * box; the inline axis is mirrored for rtl when a fragment is placed. Axis 0 is rows (block), 1 columns
 * (inline). Track sizes are the bases of grid_track.c, which are the used sizes at the end.
 *
 * Auto-fit collapse: an empty auto-fit track has no size, and its gutters collapse with it (only the gaps
 * between live tracks count, see embi_grid_gutters).
 *
 * Known gaps. Writing modes other than horizontal-tb are out of scope for W3. The intrinsic
 * contributions of an item and its measured height are recomputed whenever they are asked for (the
 * item layout is cached by neither): W3h can cache them per (width, height). A subgrid in an axis
 * whose direction differs from its parent's is laid out as if the directions were equal.
 */
#include "grid_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

/* Alignment kinds of an item along an axis. NORMAL stretches like STRETCH, except for replaced boxes
 * and boxes with an aspect ratio, which start. */
enum {
    AK_NORMAL = 0,
    AK_STRETCH,
    AK_START,
    AK_END,
    AK_CENTER,
    AK_BASELINE,
    AK_LBASELINE,
};

/* Content distribution kinds of justify-content and align-content. */
enum {
    CD_STRETCH = 0,
    CD_START,
    CD_END,
    CD_CENTER,
    CD_BETWEEN,
    CD_AROUND,
    CD_EVENLY,
};

/* One axis of a grid. */
typedef struct gax {
    gtmpl tm;
    uint32_t pre, nt;   /* implicit tracks before the explicit ones, and the track count */
    uint32_t nex;       /* explicit tracks */
    gtrack *tr;         /* nt + 1 */
    float *pos;         /* the start of each track, logical, from the content box (nt + 1) */
    uint8_t *cov;       /* tracks covered by an item (nt + 1) */
    float gap;          /* the gap used */
    float tgap;         /* the gap between the tracks of the grid that owns them (the root's) */
    bool sub;           /* the axis is a subgrid axis: tracks come from the parent */
    uint32_t off;       /* sub: the index of the first parent track */
    float mbp[2];       /* a subgrid's margin, border and padding at the start and end of this axis */
} gax;

typedef struct gst gst;

struct gst {
    embi_lctx *c;
    embi_box *b;
    gst *parent;        /* the grid this one is a subgrid of */
    gitem *pit;         /* the parent's item that is this grid */
    gst *root;
    uint32_t goff[2];   /* index of track 0 in the root's tracks, per axis */
    float cw, chd;      /* the content box width, and its height when definite (NaN otherwise) */
    float minw, maxw, minh, maxh;
    bool rtl;
    bool cols_ready;    /* the columns of a subgrid (sizing phase) are known */
    gax ax[2];
    garea *areas;
    uint32_t narea, acap;
    gplace_ctx pc;
    const embi_grid_tracks *autol[2];
    gitem *items;
    uint32_t ni, icap;
    float *bf, *bl;     /* root: the largest first and last baseline distance of the baseline group at each row */
};

/* The request a parent hands to the layout of one of its subgrid items (embi_lctx.gsub). */
typedef struct gsub_req {
    gst *parent;
    gitem *item;
    float origin[2];    /* the child's content box origin in the parent's logical coordinates */
} gsub_req;

/* A contribution record: the track item plus the data for baseline groups. */
typedef struct gcon {
    gsitem s;
    uint8_t bk;         /* 0 none, 1 first baseline, 2 last baseline */
    float asc;          /* first: the distance from the margin-box start to the baseline */
    float desc;         /* last: the distance from the baseline to the margin-box end */
    bool has_lim;       /* the minimum contribution was limited by a fixed track maximum (rows) */
    float lim, mfull, mfloor; /* that limit, the unlimited margin-box height, and the margin-box edges */
} gcon;

typedef struct gcv {
    gcon *d;
    uint32_t len, cap;
} gcv;

/* ------------------------------------------------------------------ style helpers */

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

typedef struct galign {
    uint16_t kw;
    bool last, safe, unsafe;
} galign;

static galign al_of(const embi_value *v)
{
    galign r = {0, false, false, false};
    if (v == nullptr) {
        return r;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        r.kw = v->kw;
    } else if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            const embi_value *k = &v->list.items[i];
            if (k->kind != EMBI_V_KEYWORD) {
                continue;
            }
            if (k->kw == EMBI_KW_last) {
                r.last = true;
            } else if (k->kw == EMBI_KW_safe) {
                r.safe = true;
            } else if (k->kw == EMBI_KW_unsafe) {
                r.unsafe = true;
            } else if (k->kw != EMBI_KW_first) {
                r.kw = k->kw;
            }
        }
    }
    return r;
}

/* The alignment kind of an alignment value. left and right are physical: flip makes them the logical end
 * and start in a rtl inline axis. */
static int ak_of(galign a, bool flip)
{
    if (a.kw == EMBI_KW_baseline) {
        return a.last ? AK_LBASELINE : AK_BASELINE;
    }
    switch (a.kw) {
    case EMBI_KW_start:
    case EMBI_KW_self_start:
    case EMBI_KW_flex_start:
        return AK_START;
    case EMBI_KW_left:
        return flip ? AK_END : AK_START;
    case EMBI_KW_end:
    case EMBI_KW_self_end:
    case EMBI_KW_flex_end:
        return AK_END;
    case EMBI_KW_right:
        return flip ? AK_START : AK_END;
    case EMBI_KW_center:
        return AK_CENTER;
    case EMBI_KW_stretch:
        return AK_STRETCH;
    default:
        return AK_NORMAL;
    }
}

/* The content distribution kind of justify-content or align-content (CD_*), and whether it is safe: the
 * default is unsafe (WPT grid-content-alignment-overflow), safe only when written. */
static int cd_of(const embi_value *v, bool flip, bool *safe)
{
    galign a = al_of(v);
    *safe = a.safe;
    switch (a.kw) {
    case EMBI_KW_start:
    case EMBI_KW_flex_start:
        return CD_START;
    case EMBI_KW_left:
        return flip ? CD_END : CD_START;
    case EMBI_KW_end:
    case EMBI_KW_flex_end:
        return CD_END;
    case EMBI_KW_right:
        return flip ? CD_START : CD_END;
    case EMBI_KW_center:
        return CD_CENTER;
    case EMBI_KW_space_between:
        return CD_BETWEEN;
    case EMBI_KW_space_around:
        return CD_AROUND;
    case EMBI_KW_space_evenly:
        return CD_EVENLY;
    default:
        return CD_STRETCH;
    }
}

/* The alignment of an item along an axis: its own value, or the container's default when auto. */
static int self_kind(embi_lctx *c, embi_box *b, embi_box *k, embi_prop_id self_p, embi_prop_id items_p,
                     bool flip, bool *safe)
{
    galign s = al_of(embi_lay_prop(c, k, self_p));
    if (s.kw == EMBI_KW_auto || (s.kw == 0 && !s.last)) {
        s = al_of(embi_lay_prop(c, b, items_p));
    }
    *safe = s.safe;
    return ak_of(s, flip);
}

/* The gap of a gap property: normal is the inherited gap (zero for a root), a percentage of an
 * indefinite base is zero. */
static float gap_len(embi_lctx *c, embi_box *b, embi_prop_id p, float pct, float normal)
{
    const embi_value *v = embi_lay_prop(c, b, p);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return normal;
    }
    return clamp0(embi_lay_len(c, b, v, pct));
}

static bool is_replaced(embi_lctx *c, embi_box *k)
{
    float r = 0;
    return (k->flags & EMBI_BF_REPLACED) != 0 || k->kind == EMBI_BK_REPLACED || embi_lay_ratio(c, k, &r);
}

static bool box_sizing_bb(embi_lctx *c, embi_box *k)
{
    const embi_value *bs = embi_lay_prop(c, k, EMBI_PROP_box_sizing);
    return bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
}

/* A min or max property of an item as a border-box length (NaN: auto, none, a keyword, a percentage of an indefinite
 * size). A calc-size() with a length basis applies its calc-sum to the basis (CSS Values 5 section 10.1). */
static float limit_len_bb(embi_lctx *c, embi_box *k, embi_prop_id p, float cb, float edges, bool bb)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, k, p, &cx);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return NAN;
    }
    float l = embi_lay_len(c, k, v, cb);
    if (isnan(l)) {
        return NAN;
    }
    if (cx != nullptr) {
        float r = embi_lay_size_eval(c, k, cx, l, cb);
        if (!isnan(r)) {
            l = r;
        }
    }
    return clamp0(bb ? l : l + edges);
}

/* A min or max size (border-box) of a box along one axis, applied to v. cb is the percentage base. */
static float clamp_bb(embi_lctx *c, embi_box *k, bool horiz, float cb, float edges, float v)
{
    bool bb = box_sizing_bb(c, k);
    embi_prop_id mnp = horiz ? EMBI_PROP_min_width : EMBI_PROP_min_height;
    embi_prop_id mxp = horiz ? EMBI_PROP_max_width : EMBI_PROP_max_height;
    float l = limit_len_bb(c, k, mxp, cb, edges, bb);
    if (!isnan(l) && v > l) {
        v = l;
    }
    l = limit_len_bb(c, k, mnp, cb, edges, bb);
    if (!isnan(l) && v < l) {
        v = l;
    }
    return v < edges ? edges : v;
}

/* The used min size of an item along an axis as a border-box length, or NaN when it is auto (or a
 * keyword, or a percentage of an indefinite size). */
static float min_len_bb(embi_lctx *c, embi_box *k, bool horiz, float cb, float edges)
{
    float l = limit_len_bb(c, k, horiz ? EMBI_PROP_min_width : EMBI_PROP_min_height, cb, edges, box_sizing_bb(c, k));
    return isnan(l) ? NAN : (l < edges ? edges : l);
}

static float max_len_bb(embi_lctx *c, embi_box *k, bool horiz, float cb, float edges)
{
    float l = limit_len_bb(c, k, horiz ? EMBI_PROP_max_width : EMBI_PROP_max_height, cb, edges, box_sizing_bb(c, k));
    return isnan(l) ? NAN : (l < edges ? edges : l);
}

/* The scroll container test of one axis (overflow hidden, scroll or auto; clip does not count). */
static bool scrolls_in(embi_lctx *c, embi_box *k, int ax)
{
    const embi_value *v = embi_lay_prop(c, k, ax == 1 ? EMBI_PROP_overflow_x : EMBI_PROP_overflow_y);
    uint16_t kw = v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
    return kw == EMBI_KW_hidden || kw == EMBI_KW_scroll || kw == EMBI_KW_auto;
}

/* ------------------------------------------------------------------ the grid state */

static void free_gst(gst *g)
{
    for (int a = 0; a < 2; a++) {
        gax *x = &g->ax[a];
        embi_grid_free_tmpl(&x->tm);
        embi_free(x->tr, ((size_t)x->nt + 1) * sizeof(gtrack));
        embi_free(x->pos, ((size_t)x->nt + 1) * sizeof(float));
        embi_free(x->cov, (size_t)x->nt + 1);
    }
    if (g->items != nullptr) {
        for (uint32_t i = 0; i < g->ni; i++) {
            if (g->items[i].sub != nullptr) {
                free_gst(g->items[i].sub);
                embi_free(g->items[i].sub, sizeof(gst));
            }
        }
    }
    embi_free(g->areas, (size_t)g->acap * sizeof(garea));
    embi_free(g->items, (size_t)g->icap * sizeof(gitem));
}

static bool item_is_subgrid(embi_lctx *c, embi_box *k, int ax)
{
    if (k->kind != EMBI_BK_GRID) {
        return false;
    }
    const embi_value *v = embi_lay_prop(c, k, ax == 0 ? EMBI_PROP_grid_template_rows : EMBI_PROP_grid_template_columns);
    return v != nullptr && v->kind == EMBI_V_TRACKS && v->tracks != nullptr && v->tracks->subgrid;
}

/* Collects the in-flow children, in order-modified document order (a stable insertion sort on the
 * order property), with their placements. */
static bool collect_items(embi_lctx *c, embi_box *b, gst *g)
{
    uint32_t n = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) == 0) {
            n++;
        }
    }
    g->items = embi_calloc(n + 1, sizeof(gitem));
    g->icap = n + 1;
    if (g->items == nullptr) {
        return false;
    }
    uint32_t m = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) != 0) {
            continue;
        }
        gitem *it = &g->items[m++];
        it->box = k;
        const embi_value *ov = embi_lay_prop(c, k, EMBI_PROP_order);
        it->order = ov != nullptr && ov->kind == EMBI_V_INTEGER ? (int32_t)ov->num : 0;
        embi_grid_read_placement(c, k, it->pl);
        it->subax[0] = item_is_subgrid(c, k, 0);
        it->subax[1] = item_is_subgrid(c, k, 1);
    }
    for (uint32_t i = 1; i < m; i++) {
        gitem tmp = g->items[i];
        uint32_t j = i;
        while (j > 0 && g->items[j - 1].order > tmp.order) {
            g->items[j] = g->items[j - 1];
            j--;
        }
        g->items[j] = tmp;
    }
    g->ni = m;
    return true;
}

/* Registers the implicit line names X-start and X-end of the areas on both axes. */
static void area_names(gst *g)
{
    for (uint32_t i = 0; i < g->narea; i++) {
        const garea *a = &g->areas[i];
        embi_str s = embi_atom_str(a->name);
        if (s.len + 6 > 250) {
            continue;
        }
        char buf[256];
        memcpy(buf, s.ptr, s.len);
        memcpy(buf + s.len, "-start", 6);
        embi_atom st = embi_atom_intern(buf, s.len + 6);
        memcpy(buf + s.len, "-end", 4);
        embi_atom en = embi_atom_intern(buf, s.len + 4);
        for (int ax = 0; ax < 2; ax++) {
            uint32_t l0 = ax == 0 ? a->r0 : a->c0, l1 = (ax == 0 ? a->r1 : a->c1) + 1;
            embi_grid_push_name(&g->ax[ax].tm.names, l0, st, &g->ax[ax].tm.err);
            embi_grid_push_name(&g->ax[ax].tm.names, l1, en, &g->ax[ax].tm.err);
        }
    }
}

/* Builds the tracks of an axis from the definitions (and the auto lists) against a size basis. */
static void build_tracks(gst *g, int a, float basis)
{
    gax *x = &g->ax[a];
    for (uint32_t i = 0; i < x->nt; i++) {
        gtrack *t = &x->tr[i];
        memset(t, 0, sizeof *t);
        if (x->sub) {
            t->mink = t->maxk = GTK_FIXED;
            continue;
        }
        int64_t e = (int64_t)i - (int64_t)x->pre;
        const embi_track_size *ts;
        if (e >= 0 && e < (int64_t)x->tm.defs.len) {
            ts = x->tm.defs.data[e].ts;
        } else {
            ts = embi_grid_auto_size(g->autol[a], e, x->tm.defs.len);
        }
        embi_grid_track_from_def(g->c, g->b, ts, basis, t);
        if (e >= 0 && e < (int64_t)x->tm.defs.len && x->tm.defs.data[e].autofit) {
            /* An auto-fit track collapses when no item covers it (section 7.2.3.2). */
            t->collapsed = !x->cov[i];
        } else {
            t->collapsed = false;
        }
    }
}

/* A parent's line names [lo, hi] (explicit line indices), rebased to 0, appended to names. */
static void inherit_names(const gname_vec *from, uint32_t lo, uint32_t hi, gname_vec *to, bool *err)
{
    for (uint32_t i = 0; i < from->len; i++) {
        uint32_t l = from->data[i].line;
        if (l >= lo && l <= hi) {
            embi_grid_push_name(to, l - lo, from->data[i].name, err);
        }
    }
}

static bool gst_init(gst *g, embi_lctx *c, embi_box *b, gst *parent, gitem *pit, float cw, float chd)
{
    g->c = c;
    g->b = b;
    g->parent = parent;
    g->pit = pit;
    g->root = parent != nullptr ? parent->root : g;
    g->cw = cw;
    g->chd = chd;
    const embi_value *dv = embi_lay_prop(c, b, EMBI_PROP_direction);
    g->rtl = dv != nullptr && dv->kind == EMBI_V_KEYWORD && dv->kw == EMBI_KW_rtl;

    static const embi_prop_id tprop[2] = {EMBI_PROP_grid_template_rows, EMBI_PROP_grid_template_columns};
    static const embi_prop_id gprop[2] = {EMBI_PROP_row_gap, EMBI_PROP_column_gap};
    float basis[2] = {chd, cw};
    float lo[2] = {g->minh, g->minw}, hi[2] = {g->maxh, g->maxw};
    for (int a = 0; a < 2; a++) {
        gax *x = &g->ax[a];
        x->sub = parent != nullptr && pit->subax[a];
        float inh = x->sub ? parent->ax[a].gap : 0.0f;
        x->gap = gap_len(c, b, gprop[a], basis[a], inh);
        x->tgap = x->sub ? parent->ax[a].tgap : x->gap;
        const embi_value *tv = embi_lay_prop(c, b, tprop[a]);
        if (x->sub) {
            x->off = pit->st[a];
            x->nex = pit->en[a] - pit->st[a];
            g->goff[a] = parent->goff[a] + pit->st[a];
            embi_grid_build_subgrid_names(tv, x->nex, &x->tm);
            const gax *px = &parent->ax[a];
            if (pit->st[a] >= px->pre) {
                int64_t pl0 = (int64_t)pit->st[a] - px->pre, pl1 = (int64_t)pit->en[a] - px->pre;
                inherit_names(&px->tm.names, (uint32_t)pl0, (uint32_t)pl1, &x->tm.names, &x->tm.err);
            }
        } else {
            embi_grid_build_tmpl(c, b, tv, basis[a], lo[a], hi[a], x->gap, &x->tm);
        }
    }
    if (g->ax[0].tm.err || g->ax[1].tm.err) {
        return false;
    }

    uint32_t nrows = 0, ncols = 0;
    if (!embi_grid_parse_areas(embi_lay_prop(c, b, EMBI_PROP_grid_template_areas), &g->areas, &g->narea,
                               &g->acap, &nrows, &ncols)) {
        return false;
    }
    area_names(g);
    if (!g->ax[0].sub) {
        g->ax[0].nex = g->ax[0].tm.defs.len > nrows ? g->ax[0].tm.defs.len : nrows;
    }
    if (!g->ax[1].sub) {
        g->ax[1].nex = g->ax[1].tm.defs.len > ncols ? g->ax[1].tm.defs.len : ncols;
    }
    for (int a = 0; a < 2; a++) {
        g->pc.nex[a] = g->ax[a].nex;
        g->pc.names[a] = g->ax[a].tm.names;
        g->pc.clamp[a] = g->ax[a].sub;
    }
    g->pc.areas = g->areas;
    g->pc.narea = g->narea;

    /* grid-auto-flow: row (the default) or column, and dense. */
    g->pc.row_flow = true;
    g->pc.dense = false;
    uint32_t nf = 0;
    const embi_value *fv = embi_lay_prop(c, b, EMBI_PROP_grid_auto_flow);
    if (fv != nullptr) {
        const embi_value *fl = embi_value_items(fv, &nf);
        for (uint32_t i = 0; i < nf; i++) {
            if (fl[i].kind != EMBI_V_KEYWORD) {
                continue;
            }
            if (fl[i].kw == EMBI_KW_column) {
                g->pc.row_flow = false;
            } else if (fl[i].kw == EMBI_KW_dense) {
                g->pc.dense = true;
            }
        }
    }
    for (int a = 0; a < 2; a++) {
        const embi_value *av = embi_lay_prop(c, b, a == 0 ? EMBI_PROP_grid_auto_rows : EMBI_PROP_grid_auto_columns);
        g->autol[a] = (av != nullptr && av->kind == EMBI_V_TRACKS && av->tracks != nullptr && !av->tracks->subgrid)
                          ? av->tracks
                          : nullptr;
    }

    if (!collect_items(c, b, g)) {
        return false;
    }
    uint32_t pre[2], nt[2];
    if (!embi_grid_place(&g->pc, g->items, g->ni, pre, nt)) {
        return false;
    }
    for (int a = 0; a < 2; a++) {
        gax *x = &g->ax[a];
        x->pre = pre[a];
        x->nt = nt[a];
        x->tr = embi_calloc((size_t)x->nt + 1, sizeof(gtrack));
        x->pos = embi_calloc((size_t)x->nt + 1, sizeof(float));
        x->cov = embi_calloc((size_t)x->nt + 1, 1);
        if (x->tr == nullptr || x->pos == nullptr || x->cov == nullptr) {
            return false;
        }
        /* The tracks covered by an item, in one pass (a difference array). */
        int32_t *d = embi_calloc((size_t)x->nt + 2, sizeof *d);
        if (d == nullptr) {
            return false;
        }
        for (uint32_t i = 0; i < g->ni; i++) {
            d[g->items[i].st[a]]++;
            d[g->items[i].en[a]]--;
        }
        int32_t run = 0;
        for (uint32_t i = 0; i < x->nt; i++) {
            run += d[i];
            x->cov[i] = run > 0;
        }
        embi_free(d, ((size_t)x->nt + 2) * sizeof *d);
        build_tracks(g, a, basis[a]);
    }
    for (int a = 0; a < 2; a++) {
        embi_grid_tracks_init(g->ax[a].tr, g->ax[a].nt);
    }
    return true;
}

/* ------------------------------------------------------------------ items */

static bool is_stretch(int k);

/* The alignment kinds of an item: block (align-self) and inline (justify-self). */
static void item_kinds(embi_lctx *c, gst *g, gitem *it)
{
    embi_box *k = it->box;
    it->al[0] = self_kind(c, g->b, k, EMBI_PROP_align_self, EMBI_PROP_align_items, false, &it->ovf_safe[0]);
    it->al[1] = self_kind(c, g->b, k, EMBI_PROP_justify_self, EMBI_PROP_justify_items, g->rtl, &it->ovf_safe[1]);
    /* A form control is a block-like box that stretches under `normal` (WPT
     * grid-align-baseline-nested-intrinsic-sizing-001); other replaced boxes start. */
    if (is_replaced(c, k) && (k->flags2 & EMBI_BF2_CONTROL) == 0) {
        /* A box with only a preferred aspect ratio (not replaced) is sized like a block-level box in the inline axis, so
         * its auto width stretches; its height then follows the ratio (CSS Grid 1 section 6.2; grid-aspect-ratio-038). */
        bool truly = (k->flags & EMBI_BF_REPLACED) != 0 || k->kind == EMBI_BK_REPLACED;
        /* Stretching the width of an aspect-ratio item under justify-self: normal is against the text of CSS Grid 1
         * (normal behaves as start for such items), but it is what WPT grid-aspect-ratio-038 expects and what Chromium
         * does, so we follow them.
         * The width stretches only while the height is auto and does not stretch itself: a definite or stretched
         * height gives the width through the ratio (grid-aspect-ratio-001, -028). */
        bool h_auto = embi_lay_is_auto(embi_lay_prop(c, k, EMBI_PROP_height));
        /* Only over fixed-size columns: an intrinsic column takes its size from the item's own contribution. */
        bool cols_fixed = it->st[1] < it->en[1] && it->en[1] <= g->ax[1].nt && g->ax[1].tr != nullptr;
        for (uint32_t t = it->st[1]; cols_fixed && t < it->en[1]; t++) {
            cols_fixed = g->ax[1].tr[t].maxk == GTK_FIXED && g->ax[1].tr[t].mink == GTK_FIXED;
        }
        for (int a = 0; a < 2; a++) {
            if (it->al[a] == AK_NORMAL) {
                it->al[a] = a == 1 && !truly && h_auto && cols_fixed && !is_stretch(it->al[0]) ? AK_STRETCH : AK_START;
            }
        }
    }
    if (it->al[1] == AK_BASELINE || it->al[1] == AK_LBASELINE) {
        it->al[1] = AK_START;
    }
    for (int a = 0; a < 2; a++) {
        if (it->subax[a] && (it->al[a] == AK_BASELINE || it->al[a] == AK_LBASELINE)) {
            it->al[a] = AK_STRETCH; /* a subgridded axis has no self alignment of its own */
        }
    }
}

static bool is_stretch(int k)
{
    return k == AK_NORMAL || k == AK_STRETCH;
}

static bool lay_at(embi_lctx *c, embi_box *k, float avail, float cbw, float cbh, bool fw, float w, bool fh,
                   float h, bool hnone, embi_lres *r)
{
    embi_cs cs;
    memset(&cs, 0, sizeof cs);
    cs.avail_w = avail;
    cs.cb_w = cbw;
    cs.cb_h = cbh;
    cs.fit = EMBI_FIT_SHRINK;
    embi_box *sb = c->force_box;
    float sw = c->force_w, sh = c->force_h;
    bool swset = c->force_wset, shset = c->force_hset, shn = c->force_hnone;
    c->force_box = k;
    c->force_wset = fw;
    c->force_w = w;
    c->force_hset = fh;
    c->force_h = h;
    c->force_hnone = hnone;
    embi_lay_clear_frags(c, k);
    memset(r, 0, sizeof *r);
    embi_lay_box(c, k, &cs, r);
    c->force_box = sb;
    c->force_w = sw;
    c->force_h = sh;
    c->force_wset = swset;
    c->force_hset = shset;
    c->force_hnone = shn;
    return r->frag != nullptr;
}

/* The size of the grid area of tracks [a, b) of axis ax. */
static float area_len(gst *g, int ax, uint32_t a, uint32_t b)
{
    /* A span can end beyond the tracks that exist (a subgrid item in implicit tracks the root did not create). */
    uint32_t nt = g->ax[ax].nt;
    b = b < nt ? b : nt;
    a = a < b ? a : b;
    return embi_grid_span_size(g->ax[ax].tr, a, b, g->ax[ax].tgap);
}

/* The margins, borders and paddings of an item against the inline size cbw: the margins as numbers
 * (auto is zero), logical per axis (start, end), with the auto flags. */
static void item_edges(gst *g, gitem *it, float cbw, embi_edges *bdr, embi_edges *pad)
{
    embi_edges mar;
    embi_lay_edges(g->c, it->box, cbw, bdr, pad, &mar);
    bool rtl = g->rtl;
    float ml = mar.l, mr = mar.r;
    it->am[1][0] = isnan(rtl ? mr : ml);
    it->am[1][1] = isnan(rtl ? ml : mr);
    it->m[1][0] = it->am[1][0] ? 0.0f : (rtl ? mr : ml);
    it->m[1][1] = it->am[1][1] ? 0.0f : (rtl ? ml : mr);
    it->am[0][0] = isnan(mar.t);
    it->am[0][1] = isnan(mar.b);
    it->m[0][0] = it->am[0][0] ? 0.0f : mar.t;
    it->m[0][1] = it->am[0][1] ? 0.0f : mar.b;
}

/* The stretched size v (border-box) of an item whose width or height is calc-size(auto, ...): the calc-sum sees the
 * stretched size as `size` (CSS Values 5 section 10.1). v when there is no expression. */
static float stretch_calc(embi_lctx *c, embi_box *k, const embi_value *expr, float cb, float edges, float v)
{
    if (expr == nullptr) {
        return v;
    }
    bool bb = box_sizing_bb(c, k);
    float r = embi_lay_size_eval(c, k, expr, bb ? v : v - edges, cb);
    return isnan(r) ? v : clamp0(bb ? r : r + edges);
}

/* Lays out an item in an area of aw by ah (ah NaN when the row is not known yet). The inline size
 * stretches when justify-self is stretch and the width is auto; the block size likewise when ah is
 * known. The fragment is left in it->fr. */
static bool lay_item(gst *g, gitem *it, float aw, float ah, bool hnone)
{
    embi_lctx *c = g->c;
    embi_box *k = it->box;
    /* W3p: the measuring layout (no row height) is reused when the item is laid out again at the same
     * inline size: by the next measurement, or by the final layout, which then only sets the stretched
     * height when the item's content does not read a definite height. */
    bool plain = !it->subax[0] && !it->subax[1];
    bool cached = plain && it->mvalid && it->maw == aw && it->fr != nullptr && !it->mselfc && !hnone;
    if (cached && isnan(ah)) {
        return true;
    }
    if (cached && it->fr->first != nullptr && embi_lay_height_independent(c, k)) {
        embi_edges bd0, pd0;
        item_edges(g, it, aw, &bd0, &pd0);
        float xb0 = it->xm[0][0] + it->xm[0][1];
        const embi_value *hx0;
        bool sy0 = (is_stretch(it->al[0])) && embi_lay_is_auto(embi_lay_prop_calc(c, k, EMBI_PROP_height, &hx0)) &&
                   !it->am[0][0] && !it->am[0][1];
        if (sy0) {
            float edv0 = bd0.t + bd0.b + pd0.t + pd0.b;
            it->fr->h = clamp_bb(c, k, false, ah, edv0,
                                 stretch_calc(c, k, hx0, ah, edv0, ah - xb0 - it->m[0][0] - it->m[0][1]));
        }
        it->mvalid = false;
        return true;
    }
    it->mvalid = false;
    embi_edges bdr, pad;
    item_edges(g, it, aw, &bdr, &pad);
    float xi = it->xm[1][0] + it->xm[1][1], xb = it->xm[0][0] + it->xm[0][1];
    float awn = aw - xi;
    bool sub1 = it->subax[1], sub0 = it->subax[0];
    const embi_value *wx, *hx;
    bool sx = (is_stretch(it->al[1]) || sub1) && embi_lay_is_auto(embi_lay_prop_calc(c, k, EMBI_PROP_width, &wx)) &&
              !it->am[1][0] && !it->am[1][1];
    bool sy = !isnan(ah) && (is_stretch(it->al[0]) || sub0) &&
              embi_lay_is_auto(embi_lay_prop_calc(c, k, EMBI_PROP_height, &hx)) && !it->am[0][0] && !it->am[0][1];
    float edh = bdr.l + bdr.r + pad.l + pad.r;
    float edv = bdr.t + bdr.b + pad.t + pad.b;
    float fw = NAN, fh = NAN;
    if (sx) {
        fw = clamp_bb(c, k, true, aw, edh, stretch_calc(c, k, wx, aw, edh, awn - it->m[1][0] - it->m[1][1]));
    }
    if (sy) {
        fh = clamp_bb(c, k, false, ah, edv, stretch_calc(c, k, hx, ah, edv, ah - xb - it->m[0][0] - it->m[0][1]));
    }
    embi_lres r;
    bool ok = lay_at(c, k, awn, awn, isnan(ah) ? NAN : ah - xb, sx, fw, sy, fh, hnone, &r) &&
              ((it->fr = r.frag) != nullptr);
    if (ok && plain && isnan(ah) && !hnone) {
        it->mvalid = true;
        it->maw = aw;
        it->mselfc = r.selfc;
    }
    return ok;
}

/* ------------------------------------------------------------------ contributions */

static bool gcv_push(gcv *v, const gcon *e)
{
    if (v->len == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 16;
        gcon *d = embi_realloc(v->d, (size_t)v->cap * sizeof *d, (size_t)cap * sizeof *d);
        if (d == nullptr) {
            return false;
        }
        v->d = d;
        v->cap = cap;
    }
    v->d[v->len++] = *e;
    return true;
}

static bool spans_auto_min(const gtrack *t, uint32_t a, uint32_t b)
{
    for (uint32_t i = a; i < b; i++) {
        if (!t[i].collapsed && t[i].mink == GTK_AUTO) {
            return true;
        }
    }
    return false;
}

/* The sum of the fixed maximum sizing functions of the tracks [a, b) with their gutters, when every live
 * track has one (a fit-content() limit counts); NaN otherwise. It clamps a content-based minimum size
 * (section 6.6). */
static float fixed_max_limit(const gtrack *t, uint32_t a, uint32_t b, float gap)
{
    float lim = 0.0f;
    uint32_t live = 0;
    for (uint32_t i = a; i < b; i++) {
        if (t[i].collapsed) {
            continue;
        }
        live++;
        if (t[i].maxk != GTK_FIXED) {
            return NAN;
        }
        lim += t[i].maxpx;
    }
    return live > 0 ? lim + embi_grid_gutters(t, a, b, gap) : NAN;
}

/* How an item's preferred size on an axis constrains its automatic minimum: 0 auto (or a percentage,
 * which is cyclic here), 1 a definite length, 2 a content keyword (min-content, max-content, fit-content). */
static int pref_kind(embi_lctx *c, embi_box *k, bool horiz)
{
    const embi_value *v = embi_lay_prop(c, k, horiz ? EMBI_PROP_width : EMBI_PROP_height);
    if (embi_lay_is_auto(v)) {
        return 0;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        return v->kw == EMBI_KW_min_content || v->kw == EMBI_KW_max_content || v->kw == EMBI_KW_fit_content ? 2 : 0;
    }
    if (v->kind == EMBI_V_PERCENT) {
        return 0;
    }
    return isnan(embi_lay_len(c, k, v, NAN)) ? 2 : 1;
}

/* The width keywords of an item: min-content, max-content, fit-content(). */
static void width_keyword(embi_lctx *c, embi_box *k, float edges, float cmn, float cmx, float *mn, float *mx)
{
    const embi_value *wv = embi_lay_prop(c, k, EMBI_PROP_width);
    if (wv == nullptr || wv->kind != EMBI_V_KEYWORD) {
        return;
    }
    if (wv->kw == EMBI_KW_min_content) {
        *mn = *mx = cmn + edges;
    } else if (wv->kw == EMBI_KW_max_content) {
        *mn = *mx = cmx + edges;
    }
}

/* The inline contribution of an item with a preferred aspect ratio and an auto width whose block size is
 * known: a specified height, or the stretched row area once the rows are sized (CSS Sizing 4 section 5.1,
 * the transferred size). An auto height of a replaced box takes its natural height under the min and max
 * heights. mnb and mxb (border-box) are replaced when the ratio decides the width. */
static void ratio_width(gst *g, gitem *it, float edh, float natw, float *mnb, float *mxb)
{
    embi_lctx *c = g->c;
    embi_box *k = it->box;
    float ratio = 0;
    if (!embi_lay_ratio(c, k, &ratio) || !(ratio > 0) || !embi_lay_is_auto(embi_lay_prop(c, k, EMBI_PROP_width))) {
        return;
    }
    embi_edges bdr, pad;
    item_edges(g, it, NAN, &bdr, &pad);
    float edv = bdr.t + bdr.b + pad.t + pad.b;
    float xb = it->xm[0][0] + it->xm[0][1];
    float area = it->rowk ? it->rowh - xb : NAN; /* the grid area, the base of percentages */
    const embi_value *hv = embi_lay_prop(c, k, EMBI_PROP_height);
    bool bb = box_sizing_bb(c, k);
    float h = NAN; /* border-box height */
    if (embi_lay_is_auto(hv)) {
        if (it->rowk && is_stretch(it->al[0]) && !it->am[0][0] && !it->am[0][1]) {
            h = area - it->m[0][0] - it->m[0][1];
        } else if (!isnan(natw) && (k->flags & EMBI_BF_REPLACED) != 0 && natw > 0) {
            /* auto height: the natural height, moved only by min-height and max-height */
            float h0 = natw / ratio + (bb ? 0.0f : edv);
            float hc = clamp_bb(c, k, false, area, edv, h0);
            if (hc != h0) {
                h = hc;
            }
        }
    } else if (hv->kind != EMBI_V_KEYWORD) {
        float l = embi_lay_len(c, k, hv, area);
        if (!isnan(l)) {
            h = clamp0(bb ? l : l + edv);
        }
    }
    if (isnan(h)) {
        return;
    }
    h = clamp_bb(c, k, false, area, edv, h);
    float w = bb ? h * ratio : (h - edv) * ratio + edh;
    w = clamp_bb(c, k, true, NAN, edh, w);
    *mnb = *mxb = w;
}

/* The contribution of an item to the column tracks: min-content, max-content and minimum (margin
 * box). */
static void col_contrib(gst *g, gitem *it, float *mn, float *mx, float *mi)
{
    embi_lctx *c = g->c;
    embi_box *k = it->box;
    embi_edges bdr, pad;
    item_edges(g, it, NAN, &bdr, &pad);
    float edges = bdr.l + bdr.r + pad.l + pad.r;
    float outer = it->m[1][0] + it->m[1][1] + it->xm[1][0] + it->xm[1][1];
    /* The item's percentage height resolves against its grid area once the rows are sized (the second pass of Grid 2
     * section 12.1 step 3; aspect-ratio/grid-aspect-ratio-041): a contribution that looked at it asks for that pass. */
    uint32_t need0 = c->ih_need;
    embi_ih ih = embi_lay_ih_push(c, it->rowk ? it->rowh - (it->xm[0][0] + it->xm[0][1]) : NAN);
    embi_lay_intrinsic_outer(c, k, mn, mx);
    /* intrinsic_outer reads the margins against zero, as item_edges does; add the extra margins. */
    float cmn = 0, cmx = 0;
    embi_lay_intrinsic(c, k, &cmn, &cmx);
    embi_lay_ih_pop(c, ih);
    it->pctdep = it->pctdep || c->ih_need != need0;
    c->ih_need = need0;
    float mnb = *mn - (it->m[1][0] + it->m[1][1]);
    float mxb = *mx - (it->m[1][0] + it->m[1][1]);
    width_keyword(c, k, edges, cmn, cmx, &mnb, &mxb);
    ratio_width(g, it, edges, cmx, &mnb, &mxb);
    *mn = mnb + outer;
    *mx = mxb + outer;
    /* The minimum contribution (section 6.6): the used min-width, which for auto is the content-based
     * minimum when the item spans an auto-minimum track and is not a scroll container. */
    float minw = min_len_bb(c, k, true, NAN, edges);
    float used;
    bool kwmin = false;
    const embi_value *mnv = embi_lay_prop(c, k, EMBI_PROP_min_width);
    if (mnv != nullptr && mnv->kind == EMBI_V_KEYWORD && (mnv->kw == EMBI_KW_min_content || mnv->kw == EMBI_KW_max_content)) {
        /* min-width: min-content or max-content is the contribution itself, uncapped. */
        used = (mnv->kw == EMBI_KW_min_content ? cmn : cmx) + edges;
        kwmin = true;
    } else if (!isnan(minw)) {
        used = minw;
    } else if (!scrolls_in(c, k, 1) && spans_auto_min(g->ax[1].tr, it->st[1], it->en[1])) {
        /* The content-based minimum: the min-content contribution, which is the width when it is
         * definite (the lesser of the specified and the content size suggestions). */
        used = mnb;
        int pk = pref_kind(c, k, true);
        float lim = pk == 0 ? fixed_max_limit(g->ax[1].tr, it->st[1], it->en[1], g->ax[1].tgap) : NAN;
        if (!isnan(lim)) {
            float room = lim - outer;
            used = fminf(used, room < edges ? edges : room);
        }
        float maxw = max_len_bb(c, k, true, NAN, edges);
        if (!isnan(maxw) && maxw < used) {
            used = maxw;
        }
    } else {
        used = edges;
    }
    if (!kwmin && used > *mn - outer) {
        used = *mn - outer;
    }
    *mi = used + outer;
}

/* The inline size an item is laid out at for its row measurement: the area width of its columns. */
static float col_area(gst *g, gitem *it)
{
    return area_len(g, 1, it->st[1], it->en[1]);
}

/* The first and last baseline of an item's fragment for alignment: the synthesized border-box bottom when it
 * has none, and clamped to its border box when the item is a scroll container (CSS Align 3 section 9.1:
 * content scrolled far from the box must not move the baseline outside it). */
static void item_baselines(const gitem *it, float *bf, float *bl)
{
    const embi_frag *cf = it->fr;
    float f = isnan(cf->bl_first) ? cf->h : cf->bl_first;
    float l = isnan(embi_fl(cf)->bl_last) ? f : embi_fl(cf)->bl_last;
    if ((it->box->flags & EMBI_BF_SCROLL) != 0) {
        f = fminf(fmaxf(f, 0.0f), cf->h);
        l = fminf(fmaxf(l, 0.0f), cf->h);
    }
    *bf = f;
    *bl = l;
}

/* The contribution of an item to the row tracks: its margin-box height at its column area's width, and
 * its minimum contribution. Also the baseline data. */
static void row_contrib(gst *g, gitem *it, gcon *out)
{
    embi_lctx *c = g->c;
    embi_box *k = it->box;
    float aw = col_area(g, it);
    /* An item that is a subgrid in the column axis measures against the columns of this grid (their
     * sizes are final here; their positions are not needed for a height), as it will be laid out. */
    gsub_req rq;
    memset(&rq, 0, sizeof rq);
    rq.parent = g;
    rq.item = it;
    void *prev_sub = c->gsub;
    if (it->subax[1]) {
        c->gsub = &rq;
    }
    bool laid = lay_item(g, it, aw, NAN, false);
    c->gsub = prev_sub;
    if (!laid || it->fr == nullptr) {
        out->s.mn = out->s.mx = out->s.mi = 0;
        return;
    }
    embi_edges bdr, pad;
    item_edges(g, it, aw, &bdr, &pad);
    float edv = bdr.t + bdr.b + pad.t + pad.b;
    float outer = it->m[0][0] + it->m[0][1] + it->xm[0][0] + it->xm[0][1];
    float h = it->fr->h;
    float mn = h + outer;
    float used;
    float minh = min_len_bb(c, k, false, NAN, edv);
    if (!isnan(minh)) {
        used = minh;
    } else if (!scrolls_in(c, k, 0) && spans_auto_min(g->ax[0].tr, it->st[0], it->en[0])) {
        used = h;
        float lim = pref_kind(c, k, false) == 0 ? fixed_max_limit(g->ax[0].tr, it->st[0], it->en[0], g->ax[0].tgap)
                                                : NAN;
        if (!isnan(lim)) {
            float room = lim - outer;
            used = fminf(used, room < edv ? edv : room);
            out->has_lim = true;
            out->lim = lim;
            out->mfull = h + outer;
            out->mfloor = edv + outer;
        }
        float maxh = max_len_bb(c, k, false, NAN, edv);
        if (!isnan(maxh) && maxh < used) {
            used = maxh;
        }
    } else {
        used = edv;
    }
    if (used > h) {
        used = h;
    }
    out->s.mn = mn;
    out->s.mx = mn;
    out->s.mi = used + outer;
    /* Baselines for the shim. */
    out->bk = 0;
    if (it->fr != nullptr && (it->al[0] == AK_BASELINE || it->al[0] == AK_LBASELINE)) {
        float bf, bl;
        item_baselines(it, &bf, &bl);
        if (it->al[0] == AK_BASELINE) {
            out->bk = 1;
            out->asc = it->m[0][0] + it->xm[0][0] + bf;
        } else {
            out->bk = 2;
            out->desc = (it->fr->h - bl) + it->m[0][1] + it->xm[0][1];
        }
    }
}

static gst *ensure_sub(gst *g, gitem *it);

/* Appends the contributions of the items of g to axis a of the root, with g's track 0 at root track
 * base. A subgrid item contributes through its own items. */
static bool gather(gst *g, int a, gcv *out, uint32_t base);

static bool ensure_cols(gst *g);

static bool gather_item(gst *g, gitem *it, int a, gcv *out, uint32_t base)
{
    if (it->subax[a]) {
        gst *ch = ensure_sub(g, it);
        if (ch != nullptr) {
            if (!gather(ch, a, out, base + it->st[a])) {
                return false;
            }
            /* The subgrid's edges count even when no item reaches them. */
            for (int e = 0; e < 2; e++) {
                float v = ch->ax[a].mbp[e] + it->xm[a][e];
                if (v > 0.0f) {
                    gcon pe;
                    memset(&pe, 0, sizeof pe);
                    uint32_t t = e == 0 ? it->st[a] : it->en[a] - 1;
                    pe.s.a = base + t;
                    pe.s.b = base + t + 1;
                    pe.s.mn = pe.s.mx = pe.s.mi = v;
                    if (!gcv_push(out, &pe)) {
                        return false;
                    }
                }
            }
            return true;
        }
    }
    gcon e;
    memset(&e, 0, sizeof e);
    e.s.a = base + it->st[a];
    e.s.b = base + it->en[a];
    if (a == 1) {
        col_contrib(g, it, &e.s.mn, &e.s.mx, &e.s.mi);
    } else {
        row_contrib(g, it, &e);
    }
    return gcv_push(out, &e);
}

static bool gather(gst *g, int a, gcv *out, uint32_t base)
{
    if (a == 0 && !ensure_cols(g)) {
        return false;
    }
    for (uint32_t i = 0; i < g->ni; i++) {
        if (!gather_item(g, &g->items[i], a, out, base)) {
            return false;
        }
    }
    return true;
}

/* Converts the records to track items after applying the baseline shims (section 12.5 step 1): the
 * items of a first-baseline group share a baseline, so each contributes the distance its baseline is
 * below the start of its cell; those of a last-baseline group likewise at the end. */
static gsitem *to_items(gst *root, gcv *v, int a)
{
    gsitem *si = embi_calloc(v->len + 1, sizeof *si);
    if (si == nullptr) {
        return nullptr;
    }
    if (a == 0 && root->bf != nullptr && root->bl != nullptr) {
        uint32_t n = root->ax[0].nt;
        for (uint32_t i = 0; i < n; i++) {
            root->bf[i] = 0.0f;
            root->bl[i] = 0.0f;
        }
        for (uint32_t i = 0; i < v->len; i++) {
            gcon *e = &v->d[i];
            if (e->bk == 1 && e->s.a < n && e->asc > root->bf[e->s.a]) {
                root->bf[e->s.a] = e->asc;
            } else if (e->bk == 2 && e->s.b >= 1 && e->s.b - 1 < n && e->desc > root->bl[e->s.b - 1]) {
                root->bl[e->s.b - 1] = e->desc;
            }
        }
        for (uint32_t i = 0; i < v->len; i++) {
            gcon *e = &v->d[i];
            float shim = 0.0f;
            if (e->bk == 1 && e->s.a < n) {
                shim = root->bf[e->s.a] - e->asc;
            } else if (e->bk == 2 && e->s.b >= 1 && e->s.b - 1 < n) {
                shim = root->bl[e->s.b - 1] - e->desc;
            }
            e->s.mn += shim;
            e->s.mx += shim;
            e->s.mi += shim;
            if (e->has_lim && shim != 0.0f) {
                /* The limit by the track's fixed maximum applies to the contribution with its shim: the
                 * item may not push the track past the limit, except by its own edges (WPT
                 * grid-minimum-contribution-baseline-shim). */
                e->s.mi = fminf(e->mfull + shim, fmaxf(e->lim, e->mfloor + shim));
            }
        }
    }
    for (uint32_t i = 0; i < v->len; i++) {
        si[i] = v->d[i].s;
    }
    return si;
}

/* ------------------------------------------------------------------ subgrids */

static gst *ensure_sub(gst *g, gitem *it)
{
    if (it->sub != nullptr) {
        return it->sub;
    }
    gst *ch = embi_calloc(1, sizeof *ch);
    if (ch == nullptr) {
        return nullptr;
    }
    ch->minw = ch->minh = 0.0f;
    ch->maxw = ch->maxh = INFINITY;
    /* The child's own width is only used when its columns are not subgridded: the stretched area. */
    embi_edges bdr, pad;
    float aw = it->subax[1] ? NAN : col_area(g, it);
    item_edges(g, it, isnan(aw) ? NAN : aw, &bdr, &pad);
    float edh = bdr.l + bdr.r + pad.l + pad.r;
    float cwc = isnan(aw) ? NAN : clamp0(aw - it->xm[1][0] - it->xm[1][1] - it->m[1][0] - it->m[1][1] - edh);
    if (!gst_init(ch, g->c, it->box, g, it, cwc, NAN)) {
        free_gst(ch);
        embi_free(ch, sizeof *ch);
        return nullptr;
    }
    /* The subgrid's margin, border and padding at the edges of each of its subgridded axes. */
    for (int a = 0; a < 2; a++) {
        float s, e;
        embi_edges m2;
        embi_lay_edges(g->c, it->box, NAN, &bdr, &pad, &m2);
        if (a == 0) {
            s = bdr.t + pad.t + (isnan(m2.t) ? 0.0f : m2.t);
            e = bdr.b + pad.b + (isnan(m2.b) ? 0.0f : m2.b);
        } else {
            float ls = bdr.l + pad.l + (isnan(m2.l) ? 0.0f : m2.l);
            float le = bdr.r + pad.r + (isnan(m2.r) ? 0.0f : m2.r);
            s = g->rtl ? le : ls;
            e = g->rtl ? ls : le;
        }
        ch->ax[a].mbp[0] = s;
        ch->ax[a].mbp[1] = e;
    }
    for (int a = 0; a < 2; a++) {
        gax *x = &ch->ax[a];
        if (!x->sub) {
            continue;
        }
        float es = it->xm[a][0] + x->mbp[0], ee = it->xm[a][1] + x->mbp[1];
        float hg = (x->gap - x->tgap) / 2.0f;
        for (uint32_t i = 0; i < ch->ni; i++) {
            gitem *ci = &ch->items[i];
            ci->xm[a][0] = ci->st[a] == 0 ? es : hg;
            ci->xm[a][1] = ci->en[a] == x->nex ? ee : hg;
        }
    }
    for (uint32_t i = 0; i < ch->ni; i++) {
        item_kinds(g->c, ch, &ch->items[i]);
    }
    it->sub = ch;
    return ch;
}

/* Copies the tracks of a subgrid's subgridded axes from the parent (whose tracks are sized). */
static void fill_sub_tracks(gst *g)
{
    for (int a = 0; a < 2; a++) {
        gax *x = &g->ax[a];
        if (!x->sub) {
            continue;
        }
        for (uint32_t i = 0; i < x->nt; i++) {
            if (i < x->nex) {
                x->tr[i] = g->parent->ax[a].tr[x->off + i];
            } else {
                memset(&x->tr[i], 0, sizeof x->tr[i]);
                x->tr[i].mink = x->tr[i].maxk = GTK_FIXED;
            }
        }
    }
}

/* Marks the sized rows on the items of g and its subgrids that follow the root's rows, for the column
 * contributions that depend on the row sizes, and resets the columns of nested grids that size their own.
 * *any is set when an item has a preferred aspect ratio with an auto width. */
static void mark_rows(gst *g, bool *any)
{
    gst *root = g->root;
    if (g->parent != nullptr && !g->ax[0].sub) {
        return;
    }
    for (uint32_t i = 0; i < g->ni; i++) {
        gitem *it = &g->items[i];
        float r = 0;
        if ((embi_lay_ratio(g->c, it->box, &r) && r > 0 && embi_lay_is_auto(embi_lay_prop(g->c, it->box, EMBI_PROP_width))) ||
            it->pctdep) {
            *any = true;
        }
        it->rowk = true;
        uint32_t r1 = g->goff[0] + it->en[0], r0 = g->goff[0] + it->st[0];
        r1 = r1 < root->ax[0].nt ? r1 : root->ax[0].nt;
        r0 = r0 < r1 ? r0 : r1;
        it->rowh = embi_grid_span_size(root->ax[0].tr, r0, r1, root->ax[0].tgap);
        if (it->sub != nullptr) {
            it->sub->cols_ready = false;
            mark_rows(it->sub, any);
        }
    }
}

/* The columns of a subgrid in the sizing phase: copied from the parent's sized columns, or sized by the
 * subgrid itself against its own width. */
static bool ensure_cols(gst *g)
{
    if (g->parent == nullptr || g->cols_ready) {
        return true;
    }
    g->cols_ready = true;
    if (g->ax[1].sub) {
        fill_sub_tracks(g);
        return true;
    }
    gcv cv = {nullptr, 0, 0};
    bool ok = true;
    for (uint32_t i = 0; i < g->ni && ok; i++) {
        ok = gather_item(g, &g->items[i], 1, &cv, 0);
    }
    gsitem *si = ok ? to_items(g->root, &cv, 1) : nullptr;
    if (si != nullptr) {
        gsz z = {g->cw, 0.0f, INFINITY, GSZ_NORMAL, true};
        embi_grid_size_tracks(g->ax[1].tr, g->ax[1].nt, si, cv.len, g->ax[1].gap, &z);
        embi_free(si, ((size_t)cv.len + 1) * sizeof *si);
    }
    embi_free(cv.d, (size_t)cv.cap * sizeof *cv.d);
    return true;
}

/* ------------------------------------------------------------------ track distribution */

/* The start offset and extra spacing between tracks for a content distribution, with the tracks laid
 * out from pos[0] (the logical start). Returns the sum of the track sizes and gaps. */
static float distribute(const gtrack *t, uint32_t n, float gap, float avail, int cd, bool safe, float *pos)
{
    float sum = embi_grid_span_size(t, 0, n, gap);
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; i++) {
        live += t[i].collapsed ? 0u : 1u;
    }
    float off = 0.0f, extra = 0.0f;
    if (!isnan(avail)) {
        float freeS = avail - sum;
        if (freeS < 0.0f && cd >= CD_BETWEEN) {
            /* The fallback alignments for distributions with no space to share. */
            cd = cd == CD_BETWEEN ? CD_START : CD_CENTER;
            safe = true;
        }
        switch (cd) {
        case CD_END:
            off = freeS;
            break;
        case CD_CENTER:
            off = freeS / 2.0f;
            break;
        case CD_BETWEEN:
            if (live > 1) {
                extra = freeS / (float)(live - 1);
            }
            break;
        case CD_AROUND:
            if (live > 0) {
                extra = freeS / (float)live;
                off = extra / 2.0f;
            }
            break;
        case CD_EVENLY:
            extra = freeS / (float)(live + 1);
            off = extra;
            break;
        default:
            break;
        }
        if (safe && off < 0.0f) {
            off = 0.0f;
        }
    }
    float p = off;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (t[i].collapsed) {
            pos[i] = p;
            continue;
        }
        if (seen > 0) {
            p += gap + extra;
        }
        pos[i] = p;
        p += t[i].base;
        seen++;
    }
    pos[n] = p;
    return sum;
}

/* The grid area of a track range on one axis: its logical start and end. */
static void area_of(const gax *x, uint32_t a, uint32_t b, float *s, float *e)
{
    *s = x->pos[a];
    *e = x->pos[b - 1] + (x->tr[b - 1].collapsed ? 0.0f : x->tr[b - 1].base);
}

/* ------------------------------------------------------------------ the sizing driver */

/* Sizes the tracks of axis a of the root. */
static bool size_axis(gst *g, int a, const gsz *z)
{
    gcv cv = {nullptr, 0, 0};
    bool ok = gather(g, a, &cv, 0);
    gsitem *si = ok ? to_items(g, &cv, a) : nullptr;
    if (si == nullptr) {
        embi_free(cv.d, (size_t)cv.cap * sizeof *cv.d);
        return false;
    }
    embi_grid_size_tracks(g->ax[a].tr, g->ax[a].nt, si, cv.len, g->ax[a].gap, z);
    embi_free(si, ((size_t)cv.len + 1) * sizeof *si);
    embi_free(cv.d, (size_t)cv.cap * sizeof *cv.d);
    return true;
}

static bool any_pct(const gst *g, int a)
{
    for (uint32_t i = 0; i < g->ax[a].nt; i++) {
        if (g->ax[a].tr[i].pct) {
            return true;
        }
    }
    return false;
}

/* The content-box min and max sizes of the container along an axis (CSS Grid 2 section 7.2.3.2 uses
 * them for the repetitions of an indefinite container). */
static void container_limits(embi_lctx *c, embi_box *b, bool horiz, float *mn, float *mx)
{
    embi_edges bdr, pad;
    embi_lay_edges(c, b, NAN, &bdr, &pad, nullptr);
    float ed = horiz ? bdr.l + bdr.r + pad.l + pad.r : bdr.t + bdr.b + pad.t + pad.b;
    float lo = min_len_bb(c, b, horiz, NAN, ed);
    float hi = max_len_bb(c, b, horiz, NAN, ed);
    *mn = isnan(lo) ? 0.0f : lo - ed;
    *mx = isnan(hi) ? INFINITY : hi - ed;
    if (*mx < *mn) {
        *mx = *mn; /* min wins over max */
    }
}

/* The baselines of the container from its items (Grid 1 section 9.1). The first baseline comes from the
 * items that intersect the first row: those that are first-baseline aligned (they share a baseline), else
 * the first of them in grid order, which is the order-modified document order of the item array. The last
 * baseline likewise from the items that intersect the last row, the last in grid order. NaN when that item
 * has no baseline (the caller synthesizes one). */
static void container_baselines(const gst *g, float *bl1, float *bl2)
{
    *bl1 = NAN;
    *bl2 = NAN;
    /* The first row with an item in it, and the last (the items of the first and last rows may all lie
     * further in: WPT grid-baseline-004). */
    uint32_t r1 = UINT32_MAX, r2 = 0;
    for (uint32_t i = 0; i < g->ni; i++) {
        const gitem *it = &g->items[i];
        if (it->fr != nullptr) {
            r1 = it->st[0] < r1 ? it->st[0] : r1;
            r2 = it->en[0] > r2 ? it->en[0] : r2;
        }
    }
    const gitem *f1 = nullptr, *f2 = nullptr;
    for (uint32_t i = 0; i < g->ni; i++) {
        const gitem *it = &g->items[i];
        if (it->fr == nullptr) {
            continue;
        }
        /* Grid order within a row: by start column, then by the order of the item array (document order
         * after `order`): the first in grid order is the one at the lowest column (WPT
         * grid-baseline-004), and the last the one at the highest. */
        if (it->st[0] == r1) {
            bool better = f1 == nullptr;
            if (!better) {
                /* An item takes part in baseline alignment of the first row when it is first-baseline
                 * aligned (its start row), or last-baseline aligned within that single row. */
                bool ib = it->al[0] == AK_BASELINE || (it->al[0] == AK_LBASELINE && it->en[0] - it->st[0] == 1);
                bool fb = f1->al[0] == AK_BASELINE || (f1->al[0] == AK_LBASELINE && f1->en[0] - f1->st[0] == 1);
                better = (ib && !fb) || (ib == fb && it->st[1] < f1->st[1]);
            }
            if (better) {
                f1 = it;
            }
        }
        if (it->en[0] == r2) {
            bool better = f2 == nullptr;
            if (!better) {
                bool ib = it->al[0] == AK_LBASELINE || (it->al[0] == AK_BASELINE && it->en[0] - it->st[0] == 1);
                bool fb = f2->al[0] == AK_LBASELINE || (f2->al[0] == AK_BASELINE && f2->en[0] - f2->st[0] == 1);
                better = (ib && !fb) || (ib == fb && it->st[1] >= f2->st[1]);
            }
            if (better) {
                f2 = it;
            }
        }
    }
    /* An item without a baseline gets one synthesized from its border box (the bottom edge). */
    if (f1 != nullptr) {
        /* A last-baseline member of the group contributes its own last baseline. */
        float bf, bl;
        item_baselines(f1, &bf, &bl);
        *bl1 = f1->fr->y + (f1->al[0] == AK_LBASELINE ? bl : bf);
    }
    if (f2 != nullptr) {
        float bf, bl;
        item_baselines(f2, &bf, &bl);
        *bl2 = f2->fr->y + (f2->al[0] == AK_BASELINE ? bf : bl);
    }
}

/* The position of an item along an axis inside its area (logical, from the area start), for an
 * alignment kind. ms, me are the margins (xm and the item's own), size the border-box size. */
static float align_pos(int kind, bool safe, float area, float size, float ms, float me, bool as, bool ae,
                       float *rms, float *rme)
{
    float fx = area - size - ms - me;
    *rms = ms;
    *rme = me;
    if (as || ae) {
        int na = (as ? 1 : 0) + (ae ? 1 : 0);
        float each = fx > 0.0f ? fx / (float)na : 0.0f;
        if (as) {
            *rms = ms + each;
        }
        if (ae) {
            *rme = me + each;
        }
        return *rms;
    }
    if (safe && fx < 0.0f) {
        return ms;
    }
    switch (kind) {
    case AK_END:
        return area - me - size;
    case AK_CENTER:
        return ms + fx / 2.0f;
    default:
        return ms;
    }
}

void embi_grid_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in, embi_flex_out *out)
{
    out->ch = 0;
    out->bl1 = NAN;
    out->bl2 = NAN;

    gsub_req *req = (gsub_req *)c->gsub;
    if (req == nullptr || req->item == nullptr || req->item->box != b) {
        req = nullptr;
    }

    gst *g = embi_calloc(1, sizeof *g);
    if (g == nullptr) {
        return;
    }
    container_limits(c, b, true, &g->minw, &g->maxw);
    g->minh = in->minh;
    g->maxh = in->maxh;
    if (g->maxh < g->minh) {
        g->maxh = g->minh;
    }
    if (!gst_init(g, c, b, req != nullptr ? req->parent : nullptr, req != nullptr ? req->item : nullptr, in->cw,
                  in->ch_def)) {
        free_gst(g);
        embi_free(g, sizeof *g);
        return;
    }
    uint32_t ni = g->ni;
    gitem *items = g->items;
    uint32_t nc = g->ax[1].nt, nr = g->ax[0].nt;
    gax *cx = &g->ax[1], *rx = &g->ax[0];
    /* A grid whose rows are not subgridded starts its own baseline context, whatever it does in columns. */
    bool own_root = req == nullptr || !rx->sub;
    g->root = own_root ? g : req->parent->root;
    float *rbf = nullptr, *rbl = nullptr;
    if (own_root) {
        g->bf = rbf = embi_calloc(nr + 2, sizeof(float));
        g->bl = rbl = embi_calloc(nr + 2, sizeof(float));
    }
    g->cols_ready = true;
    gst *root = g->root;

    if (req != nullptr) {
        /* A subgrid: the items' edge margins from the parent, then the tracks. */
        for (int a = 0; a < 2; a++) {
            gax *x = &g->ax[a];
            if (!x->sub) {
                continue;
            }
            embi_edges bdr, pad, m2;
            embi_lay_edges(c, b, req->parent->cw, &bdr, &pad, &m2);
            float s, e;
            if (a == 0) {
                s = bdr.t + pad.t + (isnan(m2.t) ? 0.0f : m2.t);
                e = bdr.b + pad.b + (isnan(m2.b) ? 0.0f : m2.b);
            } else {
                float ls = bdr.l + pad.l + (isnan(m2.l) ? 0.0f : m2.l);
                float le = bdr.r + pad.r + (isnan(m2.r) ? 0.0f : m2.r);
                s = g->rtl ? le : ls;
                e = g->rtl ? ls : le;
            }
            x->mbp[0] = s;
            x->mbp[1] = e;
            float es = req->item->xm[a][0] + s, ee = req->item->xm[a][1] + e;
            float hg = (x->gap - x->tgap) / 2.0f;
            for (uint32_t i = 0; i < ni; i++) {
                items[i].xm[a][0] = items[i].st[a] == 0 ? es : hg;
                items[i].xm[a][1] = items[i].en[a] == x->nex ? ee : hg;
            }
        }
        fill_sub_tracks(g);
    }
    for (uint32_t i = 0; i < ni; i++) {
        item_kinds(c, g, &items[i]);
    }

    bool sdum;
    int cdj = cd_of(embi_lay_prop(c, b, EMBI_PROP_justify_content), g->rtl, &sdum);
    bool safe_j = sdum;
    int cda = cd_of(embi_lay_prop(c, b, EMBI_PROP_align_content), false, &sdum);
    bool safe_a = sdum;

    float ch_avail = NAN; /* the block size the rows are distributed in */
    bool had_pct = false;
    /* Pass 0 sizes the columns and then the rows. When an item's column contribution depends on its row
     * (an aspect ratio, Grid 2 section 12.1 step 3), pass 1 sizes the columns again with the row sizes and
     * then the rows once more. */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && !rx->sub && isnan(g->chd)) {
            /* Pass 0 may have resolved a percentage row gap against the height; pass 1 sizes the rows
             * with the zero gap again (section 11.1). */
            rx->gap = gap_len(c, b, EMBI_PROP_row_gap, NAN, 0.0f);
            rx->tgap = rx->gap;
        }
        /* Columns: the inline sizes. */
        if (!cx->sub) {
            gsz z = {g->cw, 0.0f, INFINITY, GSZ_NORMAL, cdj == CD_STRETCH};
            (void)size_axis(g, 1, &z);
        }
        /* Rows: the block sizes at the items' column widths. */
        if (!rx->sub) {
            gsz z = {g->chd, g->minh, g->maxh, GSZ_NORMAL, cda == CD_STRETCH};
            (void)size_axis(g, 0, &z);
            float used = embi_grid_span_size(rx->tr, 0, nr, rx->gap);
            ch_avail = isnan(g->chd) ? fminf(fmaxf(used, g->minh), g->maxh) : g->chd;
            if (isnan(g->chd) && any_pct(g, 0)) {
                had_pct = true;
                /* Percentage rows against an indefinite height count as auto for the container's size, and
                 * resolve against the size it ended up with (section 7.2.1). */
                build_tracks(g, 0, ch_avail);
                gsz z2 = {ch_avail, 0.0f, INFINITY, GSZ_NORMAL, cda == CD_STRETCH};
                (void)size_axis(g, 0, &z2);
            }
            if (isnan(g->chd)) {
                /* A percentage row gap against an indefinite height resolves against the height the rows
                 * gave, without changing it (section 11.1). */
                const embi_value *rg = embi_lay_prop(c, b, EMBI_PROP_row_gap);
                if (rg != nullptr && rg->kind != EMBI_V_KEYWORD && isnan(embi_lay_len(c, b, rg, NAN))) {
                    rx->gap = gap_len(c, b, EMBI_PROP_row_gap, ch_avail, 0.0f);
                    rx->tgap = rx->gap;
                }
            }
        } else {
            ch_avail = g->chd;
        }
        if (pass == 0) {
            bool again = false;
            if (!cx->sub && !rx->sub) {
                mark_rows(g, &again);
            }
            if (!again) {
                break;
            }
            if (had_pct) {
                build_tracks(g, 0, NAN);
            }
        }
    }

    distribute(cx->tr, nc, cx->gap, g->cw, cdj, safe_j, cx->pos);
    float rowh = distribute(rx->tr, nr, rx->gap, ch_avail, cda, safe_a, rx->pos);
    float ch = rowh;
    if (!rx->sub && isnan(g->chd) && !isnan(ch_avail)) {
        ch = ch_avail; /* percentage rows resolved after the container's height is known */
    }
    if (rx->sub) {
        /* A subgrid's rows are the parent's: positions relative to the content box origin. */
        gax *px = &req->parent->ax[0];
        for (uint32_t i = 0; i < nr; i++) {
            rx->pos[i] = i < rx->nex ? px->pos[rx->off + i] - req->origin[0] : 0.0f;
        }
        rx->pos[nr] = nr > 0 ? rx->pos[nr - 1] + rx->tr[nr - 1].base : 0.0f;
        ch = in->ch_def;
        if (isnan(ch)) {
            ch = rowh;
        }
    }
    if (cx->sub) {
        gax *px = &req->parent->ax[1];
        for (uint32_t i = 0; i < nc; i++) {
            cx->pos[i] = i < cx->nex ? px->pos[cx->off + i] - req->origin[1] : 0.0f;
        }
        cx->pos[nc] = nc > 0 ? cx->pos[nc - 1] + cx->tr[nc - 1].base : 0.0f;
    }
    out->ch = ch;

    /* The baseline groups: the largest shared baseline per row (computed with the contributions for the
     * root, read from it for a subgrid). */
    /* The items: their areas, their sizes and their alignment. */
    for (uint32_t i = 0; i < ni; i++) {
        gitem *it = &items[i];
        float xs, xe, ys, ye;
        area_of(cx, it->st[1], it->en[1], &xs, &xe);
        area_of(rx, it->st[0], it->en[0], &ys, &ye);
        it->cell[1] = xe - xs;
        it->cell[0] = ye - ys;
        if (it->subax[0] || it->subax[1]) {
            /* The subgrid's tracks and origin are handed over through the request. */
            gsub_req r2;
            memset(&r2, 0, sizeof r2);
            r2.parent = g;
            r2.item = it;
            embi_edges bdr, pad;
            item_edges(g, it, it->cell[1], &bdr, &pad);
            r2.origin[1] = xs + it->xm[1][0] + it->m[1][0] + (g->rtl ? bdr.r + pad.r : bdr.l + pad.l);
            r2.origin[0] = ys + it->xm[0][0] + it->m[0][0] + bdr.t + pad.t;
            void *prev_sub = c->gsub;
            c->gsub = &r2;
            (void)lay_item(g, it, it->cell[1], it->cell[0], false);
            c->gsub = prev_sub;
            continue;
        }
        (void)lay_item(g, it, it->cell[1], it->cell[0], false);
    }

    /* Positions. Logical offsets inside the area, then the physical position (rtl mirrors the inline
     * axis). */
    for (uint32_t i = 0; i < ni; i++) {
        gitem *it = &items[i];
        embi_frag *cf = it->fr;
        if (cf == nullptr) {
            continue;
        }
        float xs, xe, ys, ye;
        area_of(cx, it->st[1], it->en[1], &xs, &xe);
        area_of(rx, it->st[0], it->en[0], &ys, &ye);
        float bw = cf->w, bh = cf->h;
        float inl_area = it->cell[1] - it->xm[1][0] - it->xm[1][1];
        float blk_area = it->cell[0] - it->xm[0][0] - it->xm[0][1];
        float rms, rme, lx, ly;
        lx = align_pos(it->al[1], it->ovf_safe[1], inl_area, bw, it->m[1][0], it->m[1][1], it->am[1][0],
                       it->am[1][1], &rms, &rme);
        float ims = rms, ime = rme;
        if (it->al[0] == AK_BASELINE || it->al[0] == AK_LBASELINE) {
            float bf, bl;
            item_baselines(it, &bf, &bl);
            if (it->al[0] == AK_BASELINE) {
                float asc = it->m[0][0] + it->xm[0][0] + bf;
                float gm = root->bf[g->goff[0] + it->st[0]];
                ly = (gm - asc) + it->xm[0][0] + it->m[0][0];
            } else {
                float desc = (cf->h - bl) + it->m[0][1] + it->xm[0][1];
                float gm = root->bl[g->goff[0] + it->en[0] - 1];
                ly = it->cell[0] - (gm - desc) - it->xm[0][1] - it->m[0][1] - bh;
            }
            rms = it->m[0][0];
            rme = it->m[0][1];
        } else {
            ly = it->xm[0][0] + align_pos(it->al[0], it->ovf_safe[0], blk_area, bh, it->m[0][0], it->m[0][1],
                                          it->am[0][0], it->am[0][1], &rms, &rme);
        }
        lx += it->xm[1][0];
        float xl = xs + lx;
        float yl = ys + ly;
        float px = g->rtl ? g->cw - xl - bw : xl;
        cf->x = in->ox + px;
        cf->y = in->oy + yl;
        embi_lay_frag_set_mg(c->ld, cf, g->rtl ? ime : ims, rms, g->rtl ? ims : ime, rme);
        embi_lay_frag_append(f, cf);
        embi_lay_ov_add_child(f, cf);
    }
    container_baselines(g, &out->bl1, &out->bl2);

    /* Out-of-flow children (position.c places them). */
    float ch_eff = isnan(g->chd) ? ch : g->chd;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) == 0) {
            continue;
        }
        float cont[4] = {in->ox, in->oy, in->ox + in->cw, in->oy + ch_eff};
        bool sdum2;
        int jk = self_kind(c, b, k, EMBI_PROP_justify_self, EMBI_PROP_justify_items, g->rtl, &sdum2);
        int ak = self_kind(c, b, k, EMBI_PROP_align_self, EMBI_PROP_align_items, false, &sdum2);
        /* An out-of-flow box takes part in no baseline group: first baseline falls back to start, last baseline to end
         * (CSS Align 3 section 9.3; grid-abspos-staticpos-align-self-last-baseline-002). */
        float frac[2] = {jk == AK_END || jk == AK_LBASELINE ? 1.0f : (jk == AK_CENTER ? 0.5f : 0.0f),
                         ak == AK_END || ak == AK_LBASELINE ? 1.0f : (ak == AK_CENTER ? 0.5f : 0.0f)};
        bool flip[2] = {g->rtl, false};
        if (embi_lay_abspos_cb(c, k) != b) {
            embi_lay_abspos_flex(c, k, b, cont, frac, flip);
            continue;
        }
        gpl pl[2][2];
        embi_grid_read_placement(c, k, pl);
        uint32_t s0, e0, s1, e1;
        bool p0 = embi_grid_place_abs(&g->pc, 0, pl[0], rx->pre, nr, &s0, &e0);
        bool p1 = embi_grid_place_abs(&g->pc, 1, pl[1], cx->pre, nc, &s1, &e1);
        /* The containing block: the area between the lines, with the padding edge for an auto side.
         * NaN marks a padding-box edge that position.c fills in from the final fragment. */
        float cb[4] = {NAN, NAN, NAN, NAN};
        if (p1) {
            float ls = NAN, le = NAN;
            if (s1 != UINT32_MAX) {
                ls = cx->pos[s1];
            }
            if (e1 != UINT32_MAX) {
                le = e1 > 0 ? cx->pos[e1 - 1] + (cx->tr[e1 - 1].collapsed ? 0.0f : cx->tr[e1 - 1].base)
                            : cx->pos[0];
            }
            float x0, x1;
            if (g->rtl) {
                x0 = isnan(le) ? NAN : in->ox + (g->cw - le);
                x1 = isnan(ls) ? NAN : in->ox + (g->cw - ls);
            } else {
                x0 = isnan(ls) ? NAN : in->ox + ls;
                x1 = isnan(le) ? NAN : in->ox + le;
            }
            cb[0] = x0;
            cb[2] = x1;
        }
        if (p0) {
            if (s0 != UINT32_MAX) {
                cb[1] = in->oy + rx->pos[s0];
            }
            if (e0 != UINT32_MAX) {
                float le = e0 > 0 ? rx->pos[e0 - 1] + (rx->tr[e0 - 1].collapsed ? 0.0f : rx->tr[e0 - 1].base)
                                  : rx->pos[0];
                cb[3] = in->oy + le;
            }
        }
        embi_lay_abspos_grid(c, k, b, cb);
    }

    if (rbf != nullptr) {
        embi_free(rbf, (nr + 2) * sizeof(float));
        embi_free(rbl, (nr + 2) * sizeof(float));
        g->bf = g->bl = nullptr;
    }
    free_gst(g);
    embi_free(g, sizeof *g);
}

/* The intrinsic inline sizes of a grid container (CSS Grid 2 section 12.1): the columns sized under a
 * min-content and under a max-content constraint. */
void embi_grid_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    *mn = 0;
    *mx = 0;
    gst *g = embi_calloc(1, sizeof *g);
    if (g == nullptr) {
        return;
    }
    container_limits(c, b, true, &g->minw, &g->maxw);
    container_limits(c, b, false, &g->minh, &g->maxh);
    if (!gst_init(g, c, b, nullptr, nullptr, NAN, NAN)) {
        free_gst(g);
        embi_free(g, sizeof *g);
        return;
    }
    for (uint32_t i = 0; i < g->ni; i++) {
        item_kinds(c, g, &g->items[i]);
    }
    gcv cv = {nullptr, 0, 0};
    bool ok = gather(g, 1, &cv, 0);
    gsitem *si = ok ? to_items(g, &cv, 1) : nullptr;
    if (si != nullptr) {
        gax *x = &g->ax[1];
        for (int m = 1; m <= 2; m++) {
            gsz z = {NAN, 0.0f, INFINITY, m, false};
            float total = embi_grid_size_tracks(x->tr, x->nt, si, cv.len, x->gap, &z);
            if (m == 1) {
                *mn = total;
            } else {
                *mx = total;
            }
        }
        embi_free(si, ((size_t)cv.len + 1) * sizeof *si);
    }
    embi_free(cv.d, (size_t)cv.cap * sizeof *cv.d);
    free_gst(g);
    embi_free(g, sizeof *g);
}

/* A grid container's box is laid out as a block box whose content is the grid (block_pass calls
 * embi_grid_content). */
void embi_lay_grid(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    embi_lay_block(c, b, cs, out);
}
