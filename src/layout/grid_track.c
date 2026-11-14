/*
 * Grid track sizing (CSS Grid 2 section 12, W3f). The algorithm works on arrays of tracks and item
 * contributions, so it has no knowledge of boxes: grid.c resolves the sizing functions and measures
 * the items, then calls embi_grid_size_tracks (section 12.3: initialize tracks, resolve intrinsic track
 * sizes, maximize tracks, expand flexible tracks, stretch auto tracks).
 *
 * Track sizes are kept in base: a track's used size is its base size after the last step. The growth
 * limit is grow; inf marks a growth limit that is still infinite (it becomes the base size before the
 * maximize step, as section 12.5 says), and infg marks a track whose growth limit went from infinite to
 * finite in step 3.5 ("infinitely growable").
 *
 * Resolve intrinsic track sizes (section 12.5). The items that span no flexible track are taken in
 * groups of equal span, smallest first. For each group the six sub-steps run in order (minimums from the
 * minimum contributions, from the min-content contributions, from the limited max-content ones under a
 * max-content constraint, the growth limit repair, then the intrinsic and the max-content maximums). In
 * each sub-step every item of the group computes its item-incurred increase against the sizes from
 * before the group, a track's planned increase is the maximum over the items, and the planned
 * increases are applied once at the end of the sub-step. A span of one is the same procedure with a
 * single track. The items that span a flexible track are then taken together, and distribute to the
 * flexible tracks only, in proportion to their flex factors.
 *
 * Collapsed tracks take no size and no gutters (embi_grid_gutters counts the gaps between live tracks
 * only).
 */
#include "grid_int.h"

#include <math.h>

#define GT_EPS 1e-4f

static bool min_intrinsic(const gtrack *t)
{
    return t->mink == GTK_AUTO || t->mink == GTK_MIN || t->mink == GTK_MAX;
}

static bool max_intrinsic(const gtrack *t)
{
    return t->maxk == GTK_AUTO || t->maxk == GTK_MIN || t->maxk == GTK_MAX || t->maxk == GTK_FIT;
}

static bool is_flex(const gtrack *t)
{
    return !t->collapsed && t->maxk == GTK_FR;
}

float embi_grid_fr_size(float free_space, float flex_sum)
{
    if (!(flex_sum > 0.0f)) {
        return 0.0f;
    }
    if (free_space <= 0.0f) {
        return 0.0f;
    }
    return free_space / (flex_sum < 1.0f ? 1.0f : flex_sum);
}

/* The gutters of the tracks [a, b): one gap between each pair of neighbouring tracks that are not
 * collapsed (a collapsed track's gutters collapse with it, section 7.2.3.2). */
float embi_grid_gutters(const gtrack *t, uint32_t a, uint32_t b, float gap)
{
    uint32_t live = 0;
    for (uint32_t i = a; i < b; i++) {
        live += t[i].collapsed ? 0u : 1u;
    }
    return live > 1 ? gap * (float)(live - 1) : 0.0f;
}

float embi_grid_span_size(const gtrack *t, uint32_t a, uint32_t b, float gap)
{
    float s = 0.0f;
    for (uint32_t i = a; i < b; i++) {
        s += t[i].collapsed ? 0.0f : t[i].base;
    }
    return s + embi_grid_gutters(t, a, b, gap);
}

void embi_grid_tracks_init(gtrack *t, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        gtrack *k = &t[i];
        k->frz = false;
        k->infg = false;
        k->plan = 0.0f;
        k->inc = 0.0f;
        if (k->collapsed) {
            k->mink = GTK_FIXED;
            k->maxk = GTK_FIXED;
            k->minpx = 0.0f;
            k->maxpx = 0.0f;
        }
        k->base = k->mink == GTK_FIXED ? k->minpx : 0.0f;
        if (k->maxk == GTK_FIXED) {
            k->grow = k->maxpx < k->base ? k->base : k->maxpx;
            k->inf = false;
        } else if (k->maxk == GTK_FR) {
            k->grow = k->base;
            k->inf = false;
        } else {
            k->grow = INFINITY;
            k->inf = true;
        }
    }
}

/* ------------------------------------------------------------------ equal distribution */

/* Spreads extra over the tracks idx[0..k) equally. When limited, a track takes at most its room below
 * its growth limit (none when the growth limit is infinite); the increases are left in plan, and the
 * tracks that reached their limit freeze. */
static void spread(gtrack *t, const uint32_t *idx, uint32_t k, float extra, bool limited)
{
    for (uint32_t j = 0; j < k; j++) {
        t[idx[j]].plan = 0.0f;
        t[idx[j]].frz = false;
    }
    float rem = extra;
    for (uint32_t it = 0; it < k + 2 && rem > GT_EPS; it++) {
        uint32_t act = 0;
        for (uint32_t j = 0; j < k; j++) {
            if (!t[idx[j]].frz) {
                act++;
            }
        }
        if (act == 0) {
            break;
        }
        float share = rem / (float)act;
        float given = 0.0f;
        for (uint32_t j = 0; j < k; j++) {
            gtrack *tr = &t[idx[j]];
            if (tr->frz) {
                continue;
            }
            float room = INFINITY;
            if (limited && !tr->inf) {
                room = tr->grow - tr->base - tr->plan;
                if (room < 0.0f) {
                    room = 0.0f;
                }
            }
            float inc = share < room ? share : room;
            tr->plan += inc;
            given += inc;
            if (limited && !tr->inf && inc >= room - GT_EPS) {
                tr->frz = true;
            }
        }
        if (given <= 0.0f) {
            break;
        }
        rem -= given;
    }
}

/* ------------------------------------------------------------------ distributing extra space */

typedef struct dist_ctx {
    gtrack *t;
    uint32_t *idx; /* scratch: n entries */
} dist_ctx;

/* Distributes extra over the affected tracks idx[0..k) for one item (section 12.5, "distributing extra
 * space across spanned tracks"), leaving the item-incurred increase in inc. base_mode distributes to
 * base sizes (limited by growth limits, then beyond them); otherwise to growth limits (no limit).
 * weighted shares by flex factor instead of equally. */
static void item_increase(gtrack *t, const uint32_t *idx, uint32_t k, float extra, bool base_mode,
                          bool weighted)
{
    for (uint32_t j = 0; j < k; j++) {
        t[idx[j]].inc = 0.0f;
        t[idx[j]].frz = false;
    }
    float rem = extra;
    for (uint32_t it = 0; it < k + 2 && rem > GT_EPS; it++) {
        float wsum = 0.0f;
        for (uint32_t j = 0; j < k; j++) {
            if (!t[idx[j]].frz) {
                wsum += weighted ? t[idx[j]].fr : 1.0f;
            }
        }
        if (!(wsum > 0.0f)) {
            break;
        }
        float given = 0.0f;
        for (uint32_t j = 0; j < k; j++) {
            gtrack *tr = &t[idx[j]];
            if (tr->frz) {
                continue;
            }
            float share = rem * (weighted ? tr->fr : 1.0f) / wsum;
            float room = INFINITY;
            if (base_mode && !weighted && !tr->inf && !tr->infg) {
                room = tr->grow - tr->base - tr->inc;
                if (room < 0.0f) {
                    room = 0.0f;
                }
            }
            float inc = share < room ? share : room;
            tr->inc += inc;
            given += inc;
            if (inc >= room - GT_EPS) {
                tr->frz = true;
            }
        }
        if (given <= GT_EPS * 0.01f) {
            break;
        }
        rem -= given;
    }
    if (base_mode && !weighted && rem > GT_EPS) {
        /* Beyond the limits: to the affected tracks with an intrinsic maximum, else to all of them. */
        uint32_t tgt = 0;
        for (uint32_t j = 0; j < k; j++) {
            tgt += max_intrinsic(&t[idx[j]]) ? 1u : 0u;
        }
        bool all = tgt == 0;
        if (all) {
            tgt = k;
        }
        for (uint32_t j = 0; j < k; j++) {
            if (all || max_intrinsic(&t[idx[j]])) {
                t[idx[j]].inc += rem / (float)tgt;
            }
        }
    }
    for (uint32_t j = 0; j < k; j++) {
        gtrack *tr = &t[idx[j]];
        if (tr->inc > tr->plan) {
            tr->plan = tr->inc;
        }
    }
}

enum {
    P_MIN_INTR = 0,  /* tracks with an intrinsic minimum */
    P_MIN_CONTENT,   /* tracks with a min-content or max-content minimum */
    P_MAX_AUTO,      /* tracks with an auto or max-content minimum */
    P_MAX_INTR,      /* tracks with an intrinsic maximum */
    P_MAX_MAXC,      /* tracks with an auto, max-content or fit-content maximum */
    P_MAXC_MIN,      /* tracks with a max-content minimum */
};

static bool in_set(const gtrack *t, int set)
{
    if (t->collapsed) {
        return false;
    }
    switch (set) {
    case P_MIN_INTR:
        return min_intrinsic(t);
    case P_MIN_CONTENT:
        return t->mink == GTK_MIN || t->mink == GTK_MAX;
    case P_MAX_AUTO:
        return t->mink == GTK_AUTO || t->mink == GTK_MAX;
    case P_MAXC_MIN:
        return t->mink == GTK_MAX;
    case P_MAX_INTR:
        return max_intrinsic(t);
    default:
        return t->maxk == GTK_AUTO || t->maxk == GTK_MAX || t->maxk == GTK_FIT;
    }
}

/* The limited contribution of an item to the intrinsic sizing (section 12.5 step 3): its contribution
 * limited by the sum of the fixed maximum sizing functions of the tracks it spans, when it spans only
 * such tracks, and floored by its minimum contribution. */
static float limited(const gtrack *t, const gsitem *g, float contrib, float gap)
{
    float lim = 0.0f;
    uint32_t live = 0;
    for (uint32_t j = g->a; j < g->b; j++) {
        if (t[j].collapsed) {
            continue;
        }
        live++;
        if (t[j].maxk == GTK_FIXED || t[j].maxk == GTK_FIT) {
            lim += t[j].maxpx;
        } else {
            return contrib;
        }
    }
    if (live == 0) {
        return contrib;
    }
    lim += embi_grid_gutters(t, g->a, g->b, gap);
    float v = contrib < lim ? contrib : lim;
    return v < g->mi ? g->mi : v;
}

/* One sub-step for the items it[order[lo..hi)]: item-incurred increases against the current sizes,
 * then the planned increases are applied. */
static void substep(gtrack *t, uint32_t n, const gsitem *it, const uint32_t *order, uint32_t lo, uint32_t hi,
                    uint32_t *idx, float gap, int set, int which, bool base_mode, bool weighted, bool lim)
{
    for (uint32_t i = 0; i < n; i++) {
        t[i].plan = 0.0f;
    }
    bool any = false;
    for (uint32_t q = lo; q < hi; q++) {
        const gsitem *g = &it[order[q]];
        float contrib = which == 0 ? g->mi : (which == 1 ? g->mn : g->mx);
        if (isnan(contrib)) {
            continue;
        }
        if (lim && which != 0) {
            contrib = limited(t, g, contrib, gap);
        } else if (lim && which == 0) {
            float m = limited(t, g, g->mn, gap);
            contrib = m;
        }
        uint32_t k = 0;
        float cur = 0.0f;
        for (uint32_t j = g->a; j < g->b; j++) {
            if (t[j].collapsed) {
                continue;
            }
            if (in_set(&t[j], set) && (!weighted || is_flex(&t[j]))) {
                idx[k++] = j;
            }
            if (base_mode) {
                cur += t[j].base;
            } else {
                cur += (t[j].inf || t[j].infg) ? t[j].base : t[j].grow;
            }
        }
        if (k == 0) {
            continue;
        }
        cur += embi_grid_gutters(t, g->a, g->b, gap);
        float extra = contrib - cur;
        if (!(extra > 0.0f)) {
            continue;
        }
        item_increase(t, idx, k, extra, base_mode, weighted);
        any = true;
    }
    if (!any) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        gtrack *tr = &t[i];
        if (tr->collapsed || !in_set(tr, set)) {
            continue;
        }
        if (weighted && !is_flex(tr)) {
            continue;
        }
        if (base_mode) {
            tr->base += tr->plan;
        } else {
            tr->grow = ((tr->inf || tr->infg) ? tr->base : tr->grow) + tr->plan;
            if (tr->inf) {
                tr->inf = false;
                tr->infg = true;
            }
        }
    }
}

/* Orders the items by span (a stable merge sort over indices), skipping the ones with no live track. */
static void sort_by_span(const gsitem *it, uint32_t *a, uint32_t *tmp, uint32_t n)
{
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n;
            uint32_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                uint32_t si = it[a[i]].b - it[a[i]].a;
                uint32_t sj = it[a[j]].b - it[a[j]].a;
                tmp[k++] = sj < si ? a[j++] : a[i++];
            }
            while (i < mid) {
                tmp[k++] = a[i++];
            }
            while (j < hi) {
                tmp[k++] = a[j++];
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            a[i] = tmp[i];
        }
    }
}

void embi_grid_resolve_intrinsic(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap, int mode)
{
    if (n == 0) {
        return;
    }
    uint32_t *idx = embi_calloc(n + 1, sizeof *idx);
    uint32_t *ord = embi_calloc(ni + 1, sizeof *ord);
    uint32_t *tmp = embi_calloc(ni + 1, sizeof *tmp);
    uint32_t *flx = embi_calloc(ni + 1, sizeof *flx);
    if (idx == nullptr || ord == nullptr || tmp == nullptr || flx == nullptr) {
        goto done;
    }
    uint32_t na = 0, nf = 0;
    for (uint32_t i = 0; i < ni; i++) {
        const gsitem *g = &it[i];
        if (g->b <= g->a || g->b > n) {
            continue;
        }
        bool live = false, flex = false;
        for (uint32_t j = g->a; j < g->b; j++) {
            if (!t[j].collapsed) {
                live = true;
                flex = flex || t[j].maxk == GTK_FR;
            }
        }
        if (!live) {
            continue;
        }
        if (flex) {
            flx[nf++] = i;
        } else {
            ord[na++] = i;
        }
    }
    sort_by_span(it, ord, tmp, na);

    bool lim = mode != GSZ_NORMAL;
    for (uint32_t lo = 0; lo < na;) {
        uint32_t span = it[ord[lo]].b - it[ord[lo]].a;
        uint32_t hi = lo;
        while (hi < na && it[ord[hi]].b - it[ord[hi]].a == span) {
            hi++;
        }
        /* 1: minimums from the minimum contributions (limited ones under an intrinsic constraint). */
        substep(t, n, it, ord, lo, hi, idx, gap, P_MIN_INTR, 0, true, false, lim);
        /* 2: min-content and max-content minimums from the min-content contributions. */
        substep(t, n, it, ord, lo, hi, idx, gap, P_MIN_CONTENT, 1, true, false, false);
        /* 3: under a max-content constraint, auto and max-content minimums from the limited max-content
         * contributions. */
        if (mode == GSZ_MAXC) {
            substep(t, n, it, ord, lo, hi, idx, gap, P_MAX_AUTO, 2, true, false, true);
        } else {
            /* 3: max-content minimums from the max-content contributions (a `max-content` track keeps the
             * room of an item spanning it and a track with a fixed minimum). */
            substep(t, n, it, ord, lo, hi, idx, gap, P_MAXC_MIN, 2, true, false, false);
        }
        /* 4: a growth limit below its base size is raised. */
        for (uint32_t i = 0; i < n; i++) {
            if (!t[i].inf && t[i].grow < t[i].base) {
                t[i].grow = t[i].base;
            }
        }
        /* 5: intrinsic maximums from the min-content contributions; 6: auto and max-content maximums from
         * the max-content contributions. */
        substep(t, n, it, ord, lo, hi, idx, gap, P_MAX_INTR, 1, false, false, false);
        substep(t, n, it, ord, lo, hi, idx, gap, P_MAX_MAXC, 2, false, false, false);
        lo = hi;
    }

    /* The items that span a flexible track, together, distribute to the flexible tracks. */
    if (nf > 0) {
        substep(t, n, it, flx, 0, nf, idx, gap, P_MIN_INTR, 0, true, true, lim);
        substep(t, n, it, flx, 0, nf, idx, gap, P_MIN_CONTENT, 1, true, true, false);
        if (mode == GSZ_MAXC) {
            substep(t, n, it, flx, 0, nf, idx, gap, P_MAX_AUTO, 2, true, true, true);
        } else {
            substep(t, n, it, flx, 0, nf, idx, gap, P_MAXC_MIN, 2, true, true, false);
        }
    }

    /* Tracks whose growth limit is still infinite take their base size, and fit-content limits apply. */
    for (uint32_t i = 0; i < n; i++) {
        gtrack *tr = &t[i];
        if (tr->inf) {
            tr->grow = tr->base;
            tr->inf = false;
        }
        if (tr->maxk == GTK_FIT) {
            float l = tr->maxpx < tr->base ? tr->base : tr->maxpx;
            if (tr->grow > l) {
                tr->grow = l;
            }
        }
        if (tr->grow < tr->base) {
            tr->grow = tr->base;
        }
        tr->infg = false;
    }
done:
    embi_free(idx, (n + 1) * sizeof *idx);
    embi_free(ord, (ni + 1) * sizeof *ord);
    embi_free(tmp, (ni + 1) * sizeof *tmp);
    embi_free(flx, (ni + 1) * sizeof *flx);
}

/* ------------------------------------------------------------------ maximize, flex, stretch */

void embi_grid_maximize(gtrack *t, uint32_t n, float free_space)
{
    if (!(free_space > 0.0f) || n == 0) {
        return;
    }
    if (isinf(free_space)) {
        for (uint32_t i = 0; i < n; i++) {
            if (!t[i].collapsed && t[i].grow > t[i].base) {
                t[i].base = t[i].grow;
            }
        }
        return;
    }
    uint32_t *idx = embi_calloc(n, sizeof *idx);
    if (idx == nullptr) {
        return;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!t[i].collapsed && t[i].grow > t[i].base + GT_EPS) {
            idx[k++] = i;
        }
    }
    if (k > 0) {
        spread(t, idx, k, free_space, true);
        for (uint32_t j = 0; j < k; j++) {
            t[idx[j]].base += t[idx[j]].plan;
            if (t[idx[j]].base > t[idx[j]].grow) {
                t[idx[j]].base = t[idx[j]].grow;
            }
        }
    }
    embi_free(idx, n * sizeof *idx);
}

void embi_grid_stretch_auto(gtrack *t, uint32_t n, float free_space)
{
    if (!(free_space > 0.0f) || n == 0) {
        return;
    }
    uint32_t *idx = embi_calloc(n, sizeof *idx);
    if (idx == nullptr) {
        return;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!t[i].collapsed && t[i].maxk == GTK_AUTO) {
            idx[k++] = i;
        }
    }
    if (k > 0) {
        spread(t, idx, k, free_space, false);
        for (uint32_t j = 0; j < k; j++) {
            t[idx[j]].base += t[idx[j]].plan;
        }
    }
    embi_free(idx, n * sizeof *idx);
}

/* The fr size of the tracks [a, b) for a leftover space (the loop of section 12.7.1 that treats a track
 * whose share is below its base size as inflexible). */
static float find_fr_range(gtrack *t, uint32_t a, uint32_t b, float leftover)
{
    for (uint32_t i = a; i < b; i++) {
        t[i].frz = !is_flex(&t[i]);
    }
    float lo = leftover;
    for (;;) {
        float fsum = 0.0f;
        for (uint32_t i = a; i < b; i++) {
            if (!t[i].frz) {
                fsum += t[i].fr;
            }
        }
        if (!(fsum > 0.0f)) {
            return 0.0f;
        }
        float size = lo > 0.0f ? lo / (fsum < 1.0f ? 1.0f : fsum) : 0.0f;
        bool changed = false;
        for (uint32_t i = a; i < b; i++) {
            if (!t[i].frz && t[i].fr * size < t[i].base) {
                t[i].frz = true;
                lo -= t[i].base;
                changed = true;
            }
        }
        if (!changed) {
            return size;
        }
    }
}

float embi_grid_find_fr(gtrack *t, uint32_t n, float leftover)
{
    return find_fr_range(t, 0, n, leftover);
}

void embi_grid_expand_flex(gtrack *t, uint32_t n, float leftover)
{
    float size = embi_grid_find_fr(t, n, leftover);
    for (uint32_t i = 0; i < n; i++) {
        if (is_flex(&t[i])) {
            float v = t[i].fr * size;
            if (v > t[i].base) {
                t[i].base = v;
            }
        }
    }
}

float embi_grid_fr_indef(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap)
{
    float best = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        if (is_flex(&t[i]) && t[i].fr > 0.0f) {
            float v = t[i].fr > 1.0f ? t[i].base / t[i].fr : t[i].base;
            if (v > best) {
                best = v;
            }
        }
    }
    for (uint32_t i = 0; i < ni; i++) {
        const gsitem *g = &it[i];
        if (g->b <= g->a || g->b > n || isnan(g->mx)) {
            continue;
        }
        bool crosses = false;
        float nonflex = 0.0f;
        for (uint32_t j = g->a; j < g->b; j++) {
            if (t[j].collapsed) {
                continue;
            }
            if (t[j].maxk == GTK_FR) {
                crosses = true;
            } else {
                nonflex += t[j].base;
            }
        }
        if (!crosses) {
            continue;
        }
        float left = g->mx - nonflex - embi_grid_gutters(t, g->a, g->b, gap);
        float v = find_fr_range(t, g->a, g->b, left);
        if (v > best) {
            best = v;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ the whole algorithm */

static float nonflex_sum(const gtrack *t, uint32_t n, float gap)
{
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        if (!t[i].collapsed && t[i].maxk != GTK_FR) {
            s += t[i].base;
        }
    }
    return s + embi_grid_gutters(t, 0, n, gap);
}

/* Maximize, expand and stretch against a definite space. */
static void finish_definite(gtrack *t, uint32_t n, float gap, float avail, bool stretch)
{
    float sum = embi_grid_span_size(t, 0, n, gap);
    embi_grid_maximize(t, n, avail - sum);
    embi_grid_expand_flex(t, n, avail - nonflex_sum(t, n, gap));
    if (stretch) {
        sum = embi_grid_span_size(t, 0, n, gap);
        embi_grid_stretch_auto(t, n, avail - sum);
    }
}

float embi_grid_size_tracks(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap, const gsz *z)
{
    if (n == 0) {
        return 0.0f;
    }
    embi_grid_tracks_init(t, n);
    embi_grid_resolve_intrinsic(t, n, it, ni, gap, z->mode);
    if (z->mode == GSZ_NORMAL && !isnan(z->avail)) {
        finish_definite(t, n, gap, z->avail, z->stretch);
        return embi_grid_span_size(t, 0, n, gap);
    }
    float *snap = embi_calloc(n, sizeof *snap);
    if (snap != nullptr) {
        for (uint32_t i = 0; i < n; i++) {
            snap[i] = t[i].base;
        }
    }
    /* An indefinite space: the free space of the maximize step is infinite (zero under a min-content
     * constraint) and the fr size comes from the items. */
    if (z->mode != GSZ_MINC) {
        embi_grid_maximize(t, n, INFINITY);
    }
    if (z->mode != GSZ_MINC) {
        /* 12.7.1: under a min-content constraint the used flex fraction is zero, so flexible tracks
         * stay at their base sizes. */
        float s = embi_grid_fr_indef(t, n, it, ni, gap);
        for (uint32_t i = 0; i < n; i++) {
            if (is_flex(&t[i]) && t[i].fr * s > t[i].base) {
                t[i].base = t[i].fr * s;
            }
        }
    }
    float total = embi_grid_span_size(t, 0, n, gap);
    if (z->mode == GSZ_NORMAL && snap != nullptr) {
        /* The container's min and max sizes: redo the steps with that size as the space. */
        float target = NAN;
        if (total < z->minsz) {
            target = z->minsz;
        } else if (total > z->maxsz) {
            target = z->maxsz;
        }
        if (!isnan(target)) {
            for (uint32_t i = 0; i < n; i++) {
                t[i].base = snap[i];
            }
            finish_definite(t, n, gap, target, z->stretch);
            total = embi_grid_span_size(t, 0, n, gap);
        }
    }
    embi_free(snap, (size_t)n * sizeof *snap);
    return total;
}
