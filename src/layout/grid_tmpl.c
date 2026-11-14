/*
 * Grid templates (CSS Grid 2 section 7, W3f): the explicit track list of an axis with repeat(),
 * auto-fill and auto-fit counts, line names, the rectangles of grid-template-areas, and the sizing
 * functions of a track definition resolved against the container's size.
 *
 * auto-fill and auto-fit (section 7.2.3.2). The number of repetitions is the largest one that does not
 * overflow the container: the tracks outside the repeat() and the gaps between all the tracks count
 * against the space, and each track counts as its maximum sizing function when that is a length, else
 * its minimum (floored at 1px). With an indefinite size, the container's max size is used; without one,
 * the smallest count that reaches the min size; without either, one.
 */
#include "grid_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

/* A track length resolved against basis: NaN when it is a percentage of an indefinite basis (the caller
 * treats that as auto), else the length clamped to zero. */
float embi_grid_track_len(embi_lctx *c, embi_box *b, const embi_value *v, float basis)
{
    float l = embi_lay_len(c, b, v, basis);
    return isnan(l) ? NAN : clamp0(l);
}

void embi_grid_free_tmpl(gtmpl *t)
{
    embi_free(t->defs.data, (size_t)t->defs.cap * sizeof(gdef));
    embi_free(t->names.data, (size_t)t->names.cap * sizeof(gname));
    memset(t, 0, sizeof *t);
}

static void push_def(gtmpl *t, const embi_track_size *ts, bool autofit)
{
    if (t->err || t->defs.len >= GRID_MAX_TRACKS) {
        return;
    }
    if (t->defs.len == t->defs.cap) {
        uint32_t cap = t->defs.cap ? t->defs.cap * 2 : 8;
        gdef *d = embi_realloc(t->defs.data, (size_t)t->defs.cap * sizeof *d, (size_t)cap * sizeof *d);
        if (d == nullptr) {
            t->err = true;
            return;
        }
        t->defs.data = d;
        t->defs.cap = cap;
    }
    t->defs.data[t->defs.len].ts = ts;
    t->defs.data[t->defs.len].autofit = autofit;
    t->defs.len++;
}

void embi_grid_push_name(gname_vec *v, uint32_t line, embi_atom name, bool *err)
{
    if (*err) {
        return;
    }
    if (v->len == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 8;
        gname *d = embi_realloc(v->data, (size_t)v->cap * sizeof *d, (size_t)cap * sizeof *d);
        if (d == nullptr) {
            *err = true;
            return;
        }
        v->data = d;
        v->cap = cap;
    }
    v->data[v->len].line = line;
    v->data[v->len].name = name;
    v->len++;
}

/* The px length a track counts as for the repetitions of auto-fill and auto-fit: the maximum sizing
 * function when it is a length, else the minimum one, floored by the minimum when both are lengths.
 * Zero when neither is a length. */
static float fixed_of(embi_lctx *c, embi_box *b, const embi_track_size *ts, float basis)
{
    float mn = NAN, mx = NAN;
    if (ts->kind == EMBI_TS_MINMAX) {
        mn = ts->min.kind == EMBI_TB_LP ? embi_grid_track_len(c, b, &ts->min.v, basis) : NAN;
        mx = ts->max.kind == EMBI_TB_LP ? embi_grid_track_len(c, b, &ts->max.v, basis) : NAN;
    } else if (ts->kind == EMBI_TS_BREADTH) {
        mn = ts->min.kind == EMBI_TB_LP ? embi_grid_track_len(c, b, &ts->min.v, basis) : NAN;
        mx = mn;
    }
    float r;
    if (!isnan(mx)) {
        r = !isnan(mn) && mn > mx ? mn : mx;
    } else if (!isnan(mn)) {
        r = mn;
    } else {
        r = 0.0f;
    }
    return r;
}

/* The repetitions of the auto repeat() of the list tl. */
static uint32_t auto_count(embi_lctx *c, embi_box *b, const embi_grid_tracks *tl, const embi_grid_item *rep,
                           float basis, float minb, float maxb, float gap)
{
    float space;
    bool atleast = false;
    if (!isnan(basis) && !isinf(basis)) {
        space = basis;
    } else if (!isinf(maxb) && !isnan(maxb)) {
        space = maxb;
    } else if (minb > 0.0f) {
        space = minb;
        atleast = true;
    } else {
        return 1;
    }
    /* The tracks outside the repeat(). */
    float fixed = 0.0f;
    uint32_t m = 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        const embi_grid_item *it = &tl->items[i];
        if (it == rep) {
            continue;
        }
        if (it->kind == EMBI_GI_TRACK) {
            fixed += fixed_of(c, b, &it->size, space);
            m++;
        } else if (it->kind == EMBI_GI_REPEAT && it->rep == EMBI_GR_COUNT) {
            for (uint32_t q = 0; q < it->n; q++) {
                if (it->items[q].kind == EMBI_GI_TRACK) {
                    fixed += (float)it->count * fixed_of(c, b, &it->items[q].size, space);
                    m += it->count;
                }
            }
        }
    }
    float sum = 0.0f;
    uint32_t r = 0;
    for (uint32_t i = 0; i < rep->n; i++) {
        const embi_grid_item *s = &rep->items[i];
        if (s->kind != EMBI_GI_TRACK) {
            continue;
        }
        float v = fixed_of(c, b, &s->size, space);
        sum += v < 1.0f ? 1.0f : v;
        r++;
    }
    if (r == 0) {
        return 1;
    }
    float per = sum + (float)r * gap;
    if (!(per > 0.0f)) {
        return 1;
    }
    /* fixed + n * sum + (m + n * r - 1) * gap versus the space. */
    float rest = space - fixed - (m > 0 ? (float)(m - 1) * gap : -gap);
    if (atleast) {
        float n = ceilf(rest / per);
        if (!(n >= 1.0f)) {
            return 1;
        }
        return n > 10000.0f ? 10000u : (uint32_t)n;
    }
    float n = floorf(rest / per);
    if (!(n >= 1.0f)) {
        return 1;
    }
    return n > 10000.0f ? 10000u : (uint32_t)n;
}

void embi_grid_build_tmpl(embi_lctx *c, embi_box *b, const embi_value *v, float basis, float minb,
                          float maxb, float gap, gtmpl *t)
{
    if (v == nullptr || v->kind != EMBI_V_TRACKS || v->tracks == nullptr || v->tracks->subgrid) {
        return;
    }
    const embi_grid_tracks *tl = v->tracks;
    for (uint32_t i = 0; i < tl->n && !t->err; i++) {
        const embi_grid_item *it = &tl->items[i];
        uint32_t line = t->defs.len;
        if (it->kind == EMBI_GI_NAMES) {
            for (uint32_t k = 0; k < it->n; k++) {
                embi_grid_push_name(&t->names, line, it->names[k], &t->err);
            }
        } else if (it->kind == EMBI_GI_TRACK) {
            push_def(t, &it->size, false);
        } else if (it->kind == EMBI_GI_REPEAT) {
            uint32_t reps = it->rep == EMBI_GR_COUNT ? it->count : auto_count(c, b, tl, it, basis, minb, maxb, gap);
            for (uint32_t r = 0; r < reps && t->defs.len < GRID_MAX_TRACKS && !t->err; r++) {
                for (uint32_t k = 0; k < it->n; k++) {
                    const embi_grid_item *s = &it->items[k];
                    uint32_t ln = t->defs.len;
                    if (s->kind == EMBI_GI_NAMES) {
                        for (uint32_t q = 0; q < s->n; q++) {
                            embi_grid_push_name(&t->names, ln, s->names[q], &t->err);
                        }
                    } else if (s->kind == EMBI_GI_TRACK) {
                        push_def(t, &s->size, it->rep == EMBI_GR_AUTO_FIT);
                    }
                }
            }
        }
    }
}

void embi_grid_build_subgrid_names(const embi_value *v, uint32_t n, gtmpl *t)
{
    if (v == nullptr || v->kind != EMBI_V_TRACKS || v->tracks == nullptr || !v->tracks->subgrid) {
        return;
    }
    const embi_grid_tracks *tl = v->tracks;
    /* The lines the explicit names occupy, to size an auto repeat(). */
    uint32_t fixed = 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        const embi_grid_item *it = &tl->items[i];
        if (it->kind == EMBI_GI_NAMES) {
            fixed++;
        } else if (it->kind == EMBI_GI_REPEAT && it->rep == EMBI_GR_COUNT) {
            uint32_t per = 0;
            for (uint32_t q = 0; q < it->n; q++) {
                per += it->items[q].kind == EMBI_GI_NAMES ? 1u : 0u;
            }
            fixed += per * it->count;
        }
    }
    uint32_t line = 0;
    for (uint32_t i = 0; i < tl->n && !t->err; i++) {
        const embi_grid_item *it = &tl->items[i];
        if (it->kind == EMBI_GI_NAMES) {
            for (uint32_t k = 0; k < it->n && line <= n; k++) {
                embi_grid_push_name(&t->names, line, it->names[k], &t->err);
            }
            line++;
        } else if (it->kind == EMBI_GI_REPEAT) {
            uint32_t per = 0;
            for (uint32_t q = 0; q < it->n; q++) {
                per += it->items[q].kind == EMBI_GI_NAMES ? 1u : 0u;
            }
            uint32_t reps = it->count;
            if (it->rep != EMBI_GR_COUNT) {
                reps = per > 0 && n + 1 > fixed ? (n + 1 - fixed) / per : 0;
                if (reps == 0) {
                    reps = 1;
                }
            }
            for (uint32_t r = 0; r < reps && line <= n; r++) {
                for (uint32_t q = 0; q < it->n && line <= n; q++) {
                    const embi_grid_item *s = &it->items[q];
                    if (s->kind != EMBI_GI_NAMES) {
                        continue;
                    }
                    for (uint32_t k = 0; k < s->n; k++) {
                        embi_grid_push_name(&t->names, line, s->names[k], &t->err);
                    }
                    line++;
                }
            }
        }
    }
}

const embi_track_size *embi_grid_auto_size(const embi_grid_tracks *al, int64_t e, uint32_t ntmpl)
{
    static const embi_track_size AUTO_TS = {
        .kind = EMBI_TS_BREADTH,
        .min = {.kind = EMBI_TB_AUTO},
        .max = {.kind = EMBI_TB_AUTO},
    };
    if (al == nullptr || al->n == 0) {
        return &AUTO_TS;
    }
    /* The list is flat: tracks only (a repeat() of tracks expands to its tracks, once per count). */
    uint32_t len = 0;
    for (uint32_t i = 0; i < al->n; i++) {
        const embi_grid_item *it = &al->items[i];
        if (it->kind == EMBI_GI_TRACK) {
            len++;
        } else if (it->kind == EMBI_GI_REPEAT && it->rep == EMBI_GR_COUNT) {
            for (uint32_t q = 0; q < it->n; q++) {
                len += it->items[q].kind == EMBI_GI_TRACK ? it->count : 0u;
            }
        }
    }
    if (len == 0) {
        return &AUTO_TS;
    }
    uint32_t idx;
    if (e >= (int64_t)ntmpl) {
        idx = (uint32_t)((e - (int64_t)ntmpl) % len);
    } else {
        int64_t j = -e - 1;
        idx = len - 1 - (uint32_t)(j % len);
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < al->n; i++) {
        const embi_grid_item *it = &al->items[i];
        if (it->kind == EMBI_GI_TRACK) {
            if (seen == idx) {
                return &it->size;
            }
            seen++;
        } else if (it->kind == EMBI_GI_REPEAT && it->rep == EMBI_GR_COUNT) {
            uint32_t per = 0;
            for (uint32_t q = 0; q < it->n; q++) {
                per += it->items[q].kind == EMBI_GI_TRACK ? 1u : 0u;
            }
            uint32_t tot = per * it->count;
            if (idx < seen + tot) {
                uint32_t k = (idx - seen) % per;
                for (uint32_t q = 0; q < it->n; q++) {
                    if (it->items[q].kind == EMBI_GI_TRACK) {
                        if (k == 0) {
                            return &it->items[q].size;
                        }
                        k--;
                    }
                }
            }
            seen += tot;
        }
    }
    return &AUTO_TS;
}

/* A breadth as a sizing function: the kind, and its px length or flex factor. */
static void breadth_fn(embi_lctx *c, embi_box *b, const embi_track_breadth *bb, float basis, bool is_max,
                       uint8_t *kind, float *px, float *fr, bool *pct)
{
    switch (bb->kind) {
    case EMBI_TB_LP: {
        float v = embi_grid_track_len(c, b, &bb->v, basis);
        if (isnan(v)) {
            *kind = GTK_AUTO;
            if (pct != nullptr) {
                *pct = true;
            }
        } else {
            *kind = GTK_FIXED;
            *px = v;
        }
        break;
    }
    case EMBI_TB_FR:
        *kind = is_max ? GTK_FR : GTK_AUTO;
        *fr = (float)bb->v.num;
        break;
    case EMBI_TB_MIN_CONTENT:
        *kind = GTK_MIN;
        break;
    case EMBI_TB_MAX_CONTENT:
        *kind = GTK_MAX;
        break;
    default:
        *kind = GTK_AUTO;
        break;
    }
}

void embi_grid_track_from_def(embi_lctx *c, embi_box *b, const embi_track_size *ts, float basis, gtrack *t)
{
    uint8_t col_unused = 0;
    (void)col_unused;
    float fr = 0;
    if (ts->kind == EMBI_TS_MINMAX) {
        breadth_fn(c, b, &ts->min, basis, false, &t->mink, &t->minpx, &fr, &t->pct);
        breadth_fn(c, b, &ts->max, basis, true, &t->maxk, &t->maxpx, &t->fr, &t->pct);
        /* A flexible minimum is invalid; an fr maximum keeps its factor. */
    } else if (ts->kind == EMBI_TS_FIT_CONTENT) {
        t->mink = GTK_AUTO;
        float v = embi_grid_track_len(c, b, &ts->max.v, basis);
        if (isnan(v)) {
            t->maxk = GTK_MAX;
            t->pct = true;
        } else {
            t->maxk = GTK_FIT;
            t->maxpx = v;
        }
    } else {
        const embi_track_breadth *bb = &ts->min;
        if (bb->kind == EMBI_TB_FR) {
            t->mink = GTK_AUTO;
            t->maxk = GTK_FR;
            t->fr = (float)bb->v.num;
        } else {
            uint8_t k = GTK_AUTO;
            float v = 0, unused = 0;
            breadth_fn(c, b, bb, basis, false, &k, &v, &unused, &t->pct);
            t->mink = k;
            t->maxk = k;
            t->minpx = v;
            t->maxpx = v;
        }
    }
    (void)fr;
}

/* The rectangles of grid-template-areas, and the row and column counts. */
bool embi_grid_parse_areas(const embi_value *v, garea **out, uint32_t *nout, uint32_t *ncap,
                           uint32_t *nrows, uint32_t *ncols)
{
    *out = nullptr;
    *nout = 0;
    *ncap = 0;
    *nrows = 0;
    *ncols = 0;
    if (v == nullptr || v->kind != EMBI_V_LIST) {
        return true;
    }
    uint32_t nr = v->list.n;
    if (nr == 0) {
        return true;
    }
    /* At most one area per token: an upper bound from the string lengths. */
    uint32_t cap = 0;
    for (uint32_t r = 0; r < nr; r++) {
        if (v->list.items[r].kind == EMBI_V_STRING) {
            cap += v->list.items[r].str.len / 2 + 1;
        }
    }
    if (cap > GRID_MAX_TRACKS) {
        cap = GRID_MAX_TRACKS;
    }
    garea *ar = embi_calloc(cap + 1, sizeof *ar);
    if (ar == nullptr) {
        return false;
    }
    *ncap = cap + 1;
    uint32_t na = 0;
    uint32_t cols = 0;
    for (uint32_t r = 0; r < nr; r++) {
        const embi_value *row = &v->list.items[r];
        if (row->kind != EMBI_V_STRING) {
            continue;
        }
        const char *s = row->str.ptr;
        uint32_t n = row->str.len;
        uint32_t i = 0, cidx = 0;
        while (i < n) {
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) {
                i++;
            }
            if (i >= n) {
                break;
            }
            uint32_t st = i;
            while (i < n && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) {
                i++;
            }
            bool dots = true;
            for (uint32_t k = st; k < i; k++) {
                if (s[k] != '.') {
                    dots = false;
                    break;
                }
            }
            if (!dots) {
                embi_atom a = embi_atom_intern(s + st, i - st);
                garea *f = nullptr;
                for (uint32_t q = 0; q < na; q++) {
                    if (ar[q].name == a) {
                        f = &ar[q];
                        break;
                    }
                }
                if (f == nullptr && na < cap) {
                    f = &ar[na++];
                    f->name = a;
                    f->r0 = r;
                    f->r1 = r;
                    f->c0 = cidx;
                    f->c1 = cidx;
                }
                if (f != nullptr) {
                    if (r < f->r0) f->r0 = r;
                    if (r > f->r1) f->r1 = r;
                    if (cidx < f->c0) f->c0 = cidx;
                    if (cidx > f->c1) f->c1 = cidx;
                }
            }
            cidx++;
        }
        if (cidx > cols) {
            cols = cidx;
        }
    }
    *out = ar;
    *nout = na;
    *nrows = nr;
    *ncols = cols;
    return true;
}
