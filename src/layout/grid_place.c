/*
 * Grid item placement (CSS Grid 2 section 8, W3f): the placement values of an item (line numbers,
 * spans, named lines and areas) resolved against the explicit grid, then the auto-placement
 * algorithm of section 8.5 with its cursor, sparse and dense packing, and the implicit tracks that
 * definite placements and auto-placed items create.
 *
 * The steps follow section 8.5. Step 1 places the items with both lines definite. Step 2 places the
 * items locked to a given major line (definite major axis, auto minor axis). Step 3 fixes the implicit
 * minor tracks. Step 4 is one loop in order-modified document order over the rest, sharing one cursor:
 * an item with a definite minor line moves the cursor to that line (and to the next major line when the
 * cursor is already past it), and an item auto on both axes is searched from the cursor. The column
 * flow swaps the axes.
 *
 * Occupancy is kept in a dense cell array that grows with the implicit grid. A grid too large to hold
 * that array (over 16M cells), or one that cannot be allocated, places the remaining items without the
 * occupancy check, so pathological pages cannot run out of memory.
 *
 * Named lines. A name with no matching line resolves to the implicit lines past the explicit grid (or
 * before its start for negative values), all of which carry the name: the first one past the end for
 * "foo" alone, and the n-th one for "n foo" once the explicit lines run out.
 */
#include "grid_int.h"

#include <string.h>

#define GP_CLAMP_LINE 10000
#define GP_OCC_MAX (1u << 24)

typedef struct occ {
    uint8_t *d;
    uint32_t r, c;
    bool off; /* no occupancy array: every cell is free */
} occ;

static bool occ_at(const occ *o, uint32_t r, uint32_t c)
{
    return r < o->r && c < o->c && o->d[(size_t)r * o->c + c] != 0;
}

/* Grows the array to at least r by c cells. False when it would be too large or cannot be allocated. */
static bool occ_reserve(occ *o, uint32_t r, uint32_t c)
{
    if (o->off) {
        return false;
    }
    if (r <= o->r && c <= o->c) {
        return true;
    }
    uint32_t nr = r > o->r ? r : o->r;
    uint32_t nc = c > o->c ? c : o->c;
    if ((uint64_t)nr * nc > GP_OCC_MAX) {
        return false;
    }
    /* Grow the dimension that was exceeded geometrically, so auto-placing N items does not copy the array
     * for every new row or column; the allocated extent is the stride, and cells beyond what was marked
     * stay free. */
    if (r > o->r && (uint64_t)(2 * o->r > nr ? 2 * o->r : nr) * nc <= GP_OCC_MAX) {
        nr = 2 * o->r > nr ? 2 * o->r : nr;
    }
    if (c > o->c && (uint64_t)nr * (2 * o->c > nc ? 2 * o->c : nc) <= GP_OCC_MAX) {
        nc = 2 * o->c > nc ? 2 * o->c : nc;
    }
    uint8_t *nd = embi_calloc((size_t)nr * nc, 1);
    if (nd == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < o->r; i++) {
        memcpy(nd + (size_t)i * nc, o->d + (size_t)i * o->c, o->c);
    }
    embi_free(o->d, (size_t)o->r * o->c);
    o->d = nd;
    o->r = nr;
    o->c = nc;
    return true;
}

static void occ_drop(occ *o)
{
    embi_free(o->d, (size_t)o->r * o->c);
    o->d = nullptr;
    o->r = 0;
    o->c = 0;
}

static bool rect_free(const occ *o, uint32_t r, uint32_t c, uint32_t rs, uint32_t cs)
{
    if (o->off) {
        return true;
    }
    /* Cells beyond the allocated extent are free: clip the scan to it. */
    uint32_t re = (uint64_t)r + rs < o->r ? r + rs : o->r;
    uint32_t ce = (uint64_t)c + cs < o->c ? c + cs : o->c;
    for (uint32_t i = r; i < re; i++) {
        for (uint32_t j = c; j < ce; j++) {
            if (occ_at(o, i, j)) {
                return false;
            }
        }
    }
    return true;
}

static void rect_mark(occ *o, uint32_t r, uint32_t c, uint32_t rs, uint32_t cs)
{
    if (o->off) {
        return;
    }
    if (!occ_reserve(o, r + rs, c + cs)) {
        occ_drop(o);
        o->off = true;
        return;
    }
    for (uint32_t i = r; i < r + rs; i++) {
        for (uint32_t j = c; j < c + cs; j++) {
            o->d[(size_t)i * o->c + j] = 1;
        }
    }
}

/* Whether an item at major position maj and minor position min, of extent sM along the major axis and
 * sm along the minor axis, is free. M is 0 for the row flow (the major axis is the row) and 1 for the
 * column flow. */
static bool fits(const occ *o, int M, uint32_t maj, uint32_t min, uint32_t sM, uint32_t sm)
{
    if (M == 0) {
        return rect_free(o, maj, min, sM, sm);
    }
    return rect_free(o, min, maj, sm, sM);
}

static void mark(occ *o, int M, uint32_t maj, uint32_t min, uint32_t sM, uint32_t sm)
{
    if (M == 0) {
        rect_mark(o, maj, min, sM, sm);
    } else {
        rect_mark(o, min, maj, sm, sM);
    }
}

/* ------------------------------------------------------------------ reading the placement */

static void read_one(const embi_value *v, gpl *out)
{
    memset(out, 0, sizeof *out);
    if (v == nullptr || v->kind != EMBI_V_GRID_LINE || v->gline == nullptr) {
        return;
    }
    const embi_grid_line *g = v->gline;
    if (g->span) {
        out->set = true;
        out->span = true;
        int32_t n = g->num > 0 ? g->num : 1;
        out->num = n > GP_CLAMP_LINE ? GP_CLAMP_LINE : n;
        out->name = g->name;
        return;
    }
    int32_t n = g->num;
    if (n > GP_CLAMP_LINE) {
        n = GP_CLAMP_LINE;
    } else if (n < -GP_CLAMP_LINE) {
        n = -GP_CLAMP_LINE;
    }
    if (n == 0 && g->name == 0) {
        return;
    }
    out->set = true;
    out->num = n;
    out->name = g->name;
}

void embi_grid_read_placement(embi_lctx *c, embi_box *k, gpl pl[2][2])
{
    read_one(embi_lay_prop(c, k, EMBI_PROP_grid_row_start), &pl[0][0]);
    read_one(embi_lay_prop(c, k, EMBI_PROP_grid_row_end), &pl[0][1]);
    read_one(embi_lay_prop(c, k, EMBI_PROP_grid_column_start), &pl[1][0]);
    read_one(embi_lay_prop(c, k, EMBI_PROP_grid_column_end), &pl[1][1]);
}

/* Whether the explicit line with 0-based index line0 on an axis carries name. */
static bool line_has(const gplace_ctx *p, uint32_t ax, uint32_t line0, embi_atom name)
{
    const gname_vec *nv = &p->names[ax];
    for (uint32_t i = 0; i < nv->len; i++) {
        if (nv->data[i].line == line0 && nv->data[i].name == name) {
            return true;
        }
    }
    return false;
}

/* The 1-based line of the n-th line carrying name (n counted from the start when positive, from the end
 * when negative). With has_bound the search covers only the lines after bound (positive n) or before it
 * (negative n). When the explicit lines run out, the implicit lines past the grid (or before its start)
 * carry the name, so the result continues beyond the explicit lines (or from the bound when it lies
 * beyond them). */
static int32_t nth_named(const gplace_ctx *p, uint32_t ax, embi_atom name, int32_t n, bool has_bound,
                         int32_t bound)
{
    int32_t want = n > 0 ? n : -n;
    int32_t seen = 0;
    int32_t N = (int32_t)p->nex[ax];
    int32_t nl = N + 1; /* explicit lines 1 .. N+1 */
    if (n > 0) {
        if (has_bound && bound >= nl) {
            return bound + want;
        }
        for (int32_t l = 1; l <= nl; l++) {
            if (has_bound && l <= bound) {
                continue;
            }
            if (line_has(p, ax, (uint32_t)(l - 1), name) && ++seen == want) {
                return l;
            }
        }
        return N + 1 + (want - seen);
    }
    if (has_bound && bound <= 1) {
        return bound - want;
    }
    for (int32_t l = nl; l >= 1; l--) {
        if (has_bound && l >= bound) {
            continue;
        }
        if (line_has(p, ax, (uint32_t)(l - 1), name) && ++seen == want) {
            return l;
        }
    }
    return 1 - (want - seen);
}

/* The 1-based line of a bare name used as a start or end (section 8.3): the first line called
 * name-start or name-end (the implicit lines of an area are registered under those names too), else the
 * first line called name, else the implicit line past the grid. */
static int32_t bare_name_line(const gplace_ctx *p, uint32_t ax, bool start, embi_atom name)
{
    embi_str s = embi_atom_str(name);
    if (s.len + 6 <= 250) {
        char buf[256];
        memcpy(buf, s.ptr, s.len);
        memcpy(buf + s.len, start ? "-start" : "-end", start ? 6 : 4);
        embi_atom want = embi_atom_intern(buf, s.len + (start ? 6u : 4u));
        int64_t best = -1;
        for (uint32_t i = 0; i < p->names[ax].len; i++) {
            if (p->names[ax].data[i].name == want && (best < 0 || p->names[ax].data[i].line < best)) {
                best = p->names[ax].data[i].line;
            }
        }
        if (best >= 0) {
            return (int32_t)best + 1;
        }
    }
    return nth_named(p, ax, name, 1, false, 0);
}

bool embi_grid_line_of(const gplace_ctx *p, uint32_t ax, bool start, const gpl *pl, int32_t *line)
{
    *line = 0;
    if (!pl->set || pl->span) {
        return false;
    }
    if (pl->name != 0) {
        *line = pl->num == 0 ? bare_name_line(p, ax, start, pl->name) : nth_named(p, ax, pl->name, pl->num, false, 0);
        return true;
    }
    int32_t n = (int32_t)p->nex[ax];
    /* A negative index that reaches past the first explicit line is a legitimate implicit line (0 and
     * below), so the result is not a sentinel. */
    *line = pl->num > 0 ? pl->num : n + 2 + pl->num;
    return true;
}

/* Resolves the two values of an item on one axis (section 8.3): both lines, a line and a span, or spans
 * alone. Sets kind (0 both lines, 1 start line, 2 end line, 3 auto), the raw line numbers and the span. */
static void item_axis(const gplace_ctx *p, uint32_t ax, const gpl *ps, const gpl *pe, gitem *g)
{
    int32_t sl = 0, el = 0;
    bool sd = embi_grid_line_of(p, ax, true, ps, &sl);
    bool ed = embi_grid_line_of(p, ax, false, pe, &el);
    uint32_t ss = (ps->set && ps->span) ? (uint32_t)ps->num : 0;
    uint32_t es = (pe->set && pe->span) ? (uint32_t)pe->num : 0;
    if (ss != 0 && es != 0) {
        es = 0; /* two spans: the end span is removed (section 8.3) and the start span is kept */
    }
    /* A named span counts the named lines from the line on its other side. */
    if (ss != 0 && ps->name != 0 && ed && !sd) {
        sl = nth_named(p, ax, ps->name, -(int32_t)ss, true, el);
        sd = true;
        ss = 0;
    } else if (es != 0 && pe->name != 0 && sd && !ed) {
        el = nth_named(p, ax, pe->name, (int32_t)es, true, sl);
        ed = true;
        es = 0;
    }
    /* A span for a named line with no definite line at the other end counts as one track. */
    if (ss != 0 && ps->name != 0) {
        ss = 1;
    }
    if (es != 0 && pe->name != 0) {
        es = 1;
    }
    if (sd && ed) {
        if (el < sl) {
            int32_t t = sl;
            sl = el;
            el = t;
        }
        if (el == sl) {
            el = sl + 1;
        }
        g->kind[ax] = 0;
        g->raw_s[ax] = sl;
        g->raw_e[ax] = el;
        g->span[ax] = (uint32_t)(el - sl);
    } else if (sd) {
        uint32_t span = es != 0 ? es : 1;
        g->kind[ax] = 1;
        g->raw_s[ax] = sl;
        g->raw_e[ax] = sl + (int32_t)span;
        g->span[ax] = span;
    } else if (ed) {
        uint32_t span = ss != 0 ? ss : 1;
        g->kind[ax] = 2;
        g->raw_e[ax] = el;
        g->raw_s[ax] = el - (int32_t)span;
        g->span[ax] = span;
    } else {
        g->kind[ax] = 3;
        g->span[ax] = ss != 0 ? ss : (es != 0 ? es : 1);
    }
    if (p->clamp[ax] && p->nex[ax] > 0) {
        /* A subgridded axis has no implicit tracks: items stay inside the lines of the subgrid. */
        uint32_t n = p->nex[ax];
        if (g->span[ax] > n) {
            g->span[ax] = n;
        }
        if (g->kind[ax] != 3) {
            int32_t s0 = g->kind[ax] == 2 ? g->raw_e[ax] - (int32_t)g->span[ax] : g->raw_s[ax];
            if (s0 < 1) {
                s0 = 1;
            }
            if (s0 + (int32_t)g->span[ax] > (int32_t)n + 1) {
                s0 = (int32_t)n + 1 - (int32_t)g->span[ax];
            }
            g->raw_s[ax] = s0;
            g->raw_e[ax] = s0 + (int32_t)g->span[ax];
            g->kind[ax] = 0;
        }
    }
}

/* ------------------------------------------------------------------ placement */

bool embi_grid_place(const gplace_ctx *p, gitem *items, uint32_t ni, uint32_t pre[2],
                     uint32_t total[2])
{
    int M = p->row_flow ? 0 : 1;
    int m = 1 - M;

    /* Resolve every item's axes against the explicit grid. The implicit tracks before the explicit grid
     * are counted per axis from the lowest definite line. */
    int64_t minl[2] = {1, 1};
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        item_axis(p, 0, &g->pl[0][0], &g->pl[0][1], g);
        item_axis(p, 1, &g->pl[1][0], &g->pl[1][1], g);
        for (int ax = 0; ax < 2; ax++) {
            if (g->kind[ax] != 3 && g->raw_s[ax] < minl[ax]) {
                minl[ax] = g->raw_s[ax];
            }
        }
    }
    for (int ax = 0; ax < 2; ax++) {
        pre[ax] = (uint32_t)(1 - minl[ax]);
        total[ax] = pre[ax] + p->nex[ax];
    }
    /* Definite lines fix the indices of the items and the size of the implicit grid. */
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        for (int ax = 0; ax < 2; ax++) {
            if (g->kind[ax] == 3) {
                continue;
            }
            g->st[ax] = (uint32_t)(g->raw_s[ax] - 1 + (int32_t)pre[ax]);
            g->en[ax] = g->st[ax] + g->span[ax];
            if (g->en[ax] > total[ax]) {
                total[ax] = g->en[ax];
            }
        }
    }

    occ o = {nullptr, 0, 0, false};
    if (!occ_reserve(&o, total[0], total[1])) {
        /* Too large or no memory: place without the occupancy check (the header's fallback). */
        occ_drop(&o);
        o.off = true;
    }

    /* Step 1: items with both lines definite. */
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        if (g->kind[0] != 3 && g->kind[1] != 3) {
            mark(&o, M, g->st[M], g->st[m], g->span[M], g->span[m]);
        }
    }

    /* Step 2: items locked to a given major line (definite major, auto minor). Sparse packing keeps a
     * cursor for each major line that only moves forward; dense packing starts each item at the start. */
    uint32_t *cur2 = embi_calloc((size_t)total[M] + 1, sizeof *cur2);
    if (cur2 == nullptr) {
        occ_drop(&o);
        return false;
    }
    for (uint32_t j = 0; j <= total[M]; j++) {
        cur2[j] = 0;
    }
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        if (g->kind[M] == 3 || g->kind[m] != 3) {
            continue;
        }
        uint32_t maj = g->st[M];
        uint32_t cur = p->dense ? 0 : cur2[maj];
        while (!fits(&o, M, maj, cur, g->span[M], g->span[m])) {
            cur++;
        }
        g->st[m] = cur;
        g->en[m] = cur + g->span[m];
        if (g->en[m] > total[m]) {
            total[m] = g->en[m];
        }
        cur2[maj] = cur + g->span[m];
        mark(&o, M, maj, cur, g->span[M], g->span[m]);
    }
    embi_free(cur2, ((size_t)total[M] + 1) * sizeof *cur2);

    /* Step 3: the implicit minor tracks are fixed now: the explicit and definite extent, at least one,
     * and wide enough for the widest item that is auto on both axes. */
    uint32_t lim = total[m] > 0 ? total[m] : 1;
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        if (g->kind[M] == 3 && g->kind[m] == 3 && g->span[m] > lim) {
            lim = g->span[m];
        }
    }

    /* Step 4: the remaining items, in order, with one cursor (major cM, minor cm). An item with a definite
     * minor line moves the cursor to that line, and to the next major line when the cursor is already past
     * it. An item auto on both axes is searched from the cursor, wrapping at the last minor track. */
    uint32_t cM = 0, cm = 0;
    for (uint32_t i = 0; i < ni; i++) {
        gitem *g = &items[i];
        if (g->kind[M] != 3) {
            continue;
        }
        uint32_t sM = g->span[M], sm = g->span[m];
        if (g->kind[m] != 3) {
            uint32_t target = g->st[m];
            if (p->dense) {
                cM = 0;
            } else if (target < cm) {
                cM++;
            }
            cm = target;
            while (!fits(&o, M, cM, cm, sM, sm)) {
                cM++;
            }
            g->st[M] = cM;
            g->en[M] = cM + sM;
        } else {
            if (p->dense) {
                cM = 0;
                cm = 0;
            }
            for (;;) {
                if (cm + sm > lim) {
                    cM++;
                    cm = 0;
                    continue;
                }
                if (fits(&o, M, cM, cm, sM, sm)) {
                    break;
                }
                cm++;
            }
            g->st[M] = cM;
            g->en[M] = cM + sM;
            g->st[m] = cm;
            g->en[m] = cm + sm;
            cm += sm;
        }
        if (g->en[M] > total[M]) {
            total[M] = g->en[M];
        }
        if (g->en[m] > total[m]) {
            total[m] = g->en[m];
        }
        mark(&o, M, g->st[M], g->st[m], sM, sm);
    }

    occ_drop(&o);
    return true;
}

bool embi_grid_place_abs(const gplace_ctx *p, uint32_t ax, const gpl pl[2], uint32_t pre,
                         uint32_t total, uint32_t *s, uint32_t *e)
{
    *s = UINT32_MAX;
    *e = UINT32_MAX;
    int32_t sl = 0, el = 0;
    bool sd = embi_grid_line_of(p, ax, true, &pl[0], &sl);
    bool ed = embi_grid_line_of(p, ax, false, &pl[1], &el);
    uint32_t ss = (pl[0].set && pl[0].span) ? (uint32_t)pl[0].num : 0;
    uint32_t es = (pl[1].set && pl[1].span) ? (uint32_t)pl[1].num : 0;
    if (ss != 0 && es != 0) {
        es = 0;
    }
    if (ss != 0 && pl[0].name != 0 && ed && !sd) {
        sl = nth_named(p, ax, pl[0].name, -(int32_t)ss, true, el);
        sd = true;
        ss = 0;
    } else if (es != 0 && pl[1].name != 0 && sd && !ed) {
        el = nth_named(p, ax, pl[1].name, (int32_t)es, true, sl);
        ed = true;
        es = 0;
    }
    if (sd && ed) {
        if (el < sl) {
            int32_t t = sl;
            sl = el;
            el = t;
        }
        if (el == sl) {
            el = sl + 1;
        }
    } else if (sd) {
        if (es != 0) {
            el = sl + (int32_t)es;
            ed = true;
        }
    } else if (ed) {
        if (ss != 0) {
            sl = el - (int32_t)ss;
            sd = true;
        }
    } else {
        return false;
    }
    /* A line outside the implicit grid is treated as auto (section 9.2). */
    if (sd) {
        int64_t a = (int64_t)sl - 1 + (int64_t)pre;
        if (a >= 0 && a <= (int64_t)total) {
            *s = (uint32_t)a;
        }
    }
    if (ed) {
        int64_t b = (int64_t)el - 1 + (int64_t)pre;
        if (b >= 0 && b <= (int64_t)total) {
            *e = (uint32_t)b;
        }
    }
    return *s != UINT32_MAX || *e != UINT32_MAX;
}
