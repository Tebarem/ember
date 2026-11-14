/*
 * Grid grammars (W2c): <track-list> / <auto-track-list> / subgrid with named lines and repeat(),
 * <track-size>, <grid-line>, grid-template-areas validation and grid-auto-rows / -columns.
 */
#include "css/value_int.h"

/* ------------------------------------------------------------------ track sizes */

static bool fr_value(const embi_css_cv *cv, double *fr)
{
    if (cv->tok.type == EMBI_CSS_DIMENSION && embi_str_is(cv->tok.value, "fr") && cv->tok.num >= 0) {
        *fr = cv->tok.num;
        return true;
    }
    return false;
}

/* <track-breadth> (allow_fr) or <inflexible-breadth>. */
static bool parse_breadth(embi_arena *a, const embi_css_cv *cv, bool allow_fr, embi_track_breadth *tb)
{
    *tb = (embi_track_breadth){0};
    if (cv_ident(cv, "auto")) {
        tb->kind = EMBI_TB_AUTO;
        return true;
    }
    if (cv_ident(cv, "min-content")) {
        tb->kind = EMBI_TB_MIN_CONTENT;
        return true;
    }
    if (cv_ident(cv, "max-content")) {
        tb->kind = EMBI_TB_MAX_CONTENT;
        return true;
    }
    double fr;
    if (fr_value(cv, &fr)) {
        if (!allow_fr) {
            return false;
        }
        tb->kind = EMBI_TB_FR;
        tb->v = v_num(fr);
        return true;
    }
    if (embi_value_parse_types(a, EMBI_LP_NN, cv, &tb->v)) {
        tb->kind = EMBI_TB_LP;
        return true;
    }
    return false;
}

bool embi_parse_track_size(embi_arena *a, const embi_css_cv *cv, embi_track_size *out)
{
    *out = (embi_track_size){0};
    if (cv && cv_fn(cv, "minmax")) {
        embi_cur args;
        embi_cur_kids(a, cv, &args);
        uint32_t n = 0;
        embi_cur *segs = embi_cur_split(a, &args, &n);
        if (n != 2 || cur_left(&segs[0]) != 1 || cur_left(&segs[1]) != 1) {
            return false;
        }
        out->kind = EMBI_TS_MINMAX;
        if (!parse_breadth(a, cur_peek(&segs[0]), false, &out->min) ||
            !parse_breadth(a, cur_peek(&segs[1]), true, &out->max)) {
            return false;
        }
        return true;
    }
    if (cv && cv_fn(cv, "fit-content")) {
        embi_cur args;
        embi_cur_kids(a, cv, &args);
        if (cur_left(&args) != 1) {
            return false;
        }
        out->kind = EMBI_TS_FIT_CONTENT;
        out->max.kind = EMBI_TB_LP;
        return embi_value_parse_types(a, EMBI_LP_NN, cur_peek(&args), &out->max.v);
    }
    if (!cv || !parse_breadth(a, cv, true, &out->min)) {
        return false;
    }
    out->kind = EMBI_TS_BREADTH;
    out->max = out->min;
    return true;
}

/* A fixed-size track (usable in auto-repeat and auto-track-list). */
static bool fixed_size(const embi_track_size *ts)
{
    if (ts->kind == EMBI_TS_BREADTH) {
        return ts->min.kind == EMBI_TB_LP;
    }
    if (ts->kind == EMBI_TS_MINMAX) {
        return ts->min.kind == EMBI_TB_LP || ts->max.kind == EMBI_TB_LP;
    }
    return false;
}

/* ------------------------------------------------------------------ line names */

static bool line_name_ok(embi_str s)
{
    return !embi_ident_reserved(s) && !embi_str_is(s, "span") && !embi_str_is(s, "auto");
}

bool embi_parse_line_names(embi_arena *a, const embi_css_cv *cv, embi_grid_item *out)
{
    if (!cv || cv->tok.type != EMBI_CSS_LBRACKET) {
        return false;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < cv->nkids; i++) {
        const embi_css_cv *k = &cv->kids[i];
        if (k->tok.type == EMBI_CSS_WS) {
            continue;
        }
        if (k->tok.type != EMBI_CSS_IDENT || !line_name_ok(k->tok.value)) {
            return false;
        }
        n++;
    }
    embi_atom *names = EMBI_NEW_ARRAY(a, embi_atom, n);
    uint32_t j = 0;
    for (uint32_t i = 0; i < cv->nkids; i++) {
        const embi_css_cv *k = &cv->kids[i];
        if (k->tok.type == EMBI_CSS_IDENT) {
            names[j++] = embi_atom_intern(k->tok.value.ptr, k->tok.value.len);
        }
    }
    *out = (embi_grid_item){0};
    out->kind = EMBI_GI_NAMES;
    out->n = n;
    out->names = names;
    return true;
}

/* ------------------------------------------------------------------ track lists */

typedef struct gvec {
    embi_grid_item *items;
    uint32_t n;
} gvec;

static void push_names(embi_arena *a, gvec *v, const embi_grid_item *names)
{
    if (v->n > 0 && v->items[v->n - 1].kind == EMBI_GI_NAMES) {
        embi_grid_item *last = &v->items[v->n - 1];
        embi_atom *merged = EMBI_NEW_ARRAY(a, embi_atom, last->n + names->n);
        memcpy(merged, last->names, sizeof(embi_atom) * last->n);
        memcpy(merged + last->n, names->names, sizeof(embi_atom) * names->n);
        last->names = merged;
        last->n += names->n;
        return;
    }
    v->items[v->n++] = *names;
}

/* repeat( <count> | auto-fill | auto-fit , [line-names? track-size]+ line-names? ) */
static bool parse_repeat(embi_arena *a, const embi_css_cv *fn, bool names_only, embi_grid_item *out)
{
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    if (nseg != 2 || cur_left(&segs[0]) != 1) {
        return false;
    }
    *out = (embi_grid_item){0};
    out->kind = EMBI_GI_REPEAT;
    const embi_css_cv *cnt = cur_peek(&segs[0]);
    if (cv_ident(cnt, "auto-fill")) {
        out->rep = EMBI_GR_AUTO_FILL;
    } else if (cv_ident(cnt, "auto-fit")) {
        if (names_only) {
            return false;
        }
        out->rep = EMBI_GR_AUTO_FIT;
    } else if (cnt->tok.type == EMBI_CSS_NUMBER && cnt->tok.flag && cnt->tok.num >= 1 &&
               cnt->tok.num <= 1000000) {
        out->rep = EMBI_GR_COUNT;
        out->count = (uint32_t)cnt->tok.num;
    } else {
        return false;
    }
    embi_cur *body = &segs[1];
    gvec v = {EMBI_NEW_ARRAY(a, embi_grid_item, cur_left(body) + 1), 0};
    uint32_t ntracks = 0;
    while (!cur_done(body)) {
        const embi_css_cv *t = cur_take(body);
        if (t->tok.type == EMBI_CSS_LBRACKET) {
            embi_grid_item names;
            if (!embi_parse_line_names(a, t, &names)) {
                return false;
            }
            push_names(a, &v, &names);
        } else if (!names_only) {
            embi_grid_item it = {0};
            it.kind = EMBI_GI_TRACK;
            if (!embi_parse_track_size(a, t, &it.size)) {
                return false;
            }
            if (out->rep != EMBI_GR_COUNT && !fixed_size(&it.size)) {
                return false;
            }
            v.items[v.n++] = it;
            ntracks++;
        } else {
            return false;
        }
    }
    if (!names_only && ntracks == 0) {
        return false;
    }
    out->n = v.n;
    out->items = v.items;
    return true;
}

bool embi_parse_track_list(embi_arena *a, embi_cur *c, embi_value *out)
{
    embi_grid_tracks *tr = EMBI_NEW(a, embi_grid_tracks);
    gvec v = {EMBI_NEW_ARRAY(a, embi_grid_item, cur_left(c) + 1), 0};
    bool names_only = false;
    if (cv_ident(cur_peek(c), "subgrid")) {
        c->i++;
        tr->subgrid = true;
        names_only = true;
    }
    uint32_t ntracks = 0;
    uint32_t nauto = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (cv_delim(t, '/')) {
            break;
        }
        c->i++;
        if (t->tok.type == EMBI_CSS_LBRACKET) {
            embi_grid_item names;
            if (!embi_parse_line_names(a, t, &names)) {
                return false;
            }
            push_names(a, &v, &names);
        } else if (cv_fn(t, "repeat")) {
            embi_grid_item rep;
            if (!parse_repeat(a, t, names_only, &rep)) {
                return false;
            }
            if (rep.rep != EMBI_GR_COUNT) {
                nauto++;
            }
            v.items[v.n++] = rep;
            ntracks++;
        } else if (!names_only) {
            embi_grid_item it = {0};
            it.kind = EMBI_GI_TRACK;
            if (!embi_parse_track_size(a, t, &it.size)) {
                return false;
            }
            v.items[v.n++] = it;
            ntracks++;
        } else {
            return false;
        }
    }
    if (!names_only && ntracks == 0) {
        return false;
    }
    if (nauto > 1) {
        return false;
    }
    if (nauto == 1) {
        /* auto-track-list: every other track must be a fixed size. */
        for (uint32_t i = 0; i < v.n; i++) {
            const embi_grid_item *it = &v.items[i];
            if (it->kind == EMBI_GI_TRACK && !fixed_size(&it->size)) {
                return false;
            }
            if (it->kind == EMBI_GI_REPEAT && it->rep == EMBI_GR_COUNT) {
                for (uint32_t k = 0; k < it->n; k++) {
                    if (it->items[k].kind == EMBI_GI_TRACK && !fixed_size(&it->items[k].size)) {
                        return false;
                    }
                }
            }
        }
        tr->has_auto_repeat = true;
    }
    tr->n = v.n;
    tr->items = v.items;
    *out = (embi_value){0};
    out->kind = EMBI_V_TRACKS;
    out->tracks = tr;
    return true;
}

bool embi_gram_grid_tracks(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_track_list(g->a, c, out);
}

bool embi_gram_grid_auto(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_grid_tracks *tr = EMBI_NEW(g->a, embi_grid_tracks);
    embi_grid_item *items = EMBI_NEW_ARRAY(g->a, embi_grid_item, cur_left(c));
    uint32_t n = 0;
    while (!cur_done(c) && !cv_delim(cur_peek(c), '/')) {
        items[n].kind = EMBI_GI_TRACK;
        if (!embi_parse_track_size(g->a, cur_peek(c), &items[n].size)) {
            return false;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    tr->n = n;
    tr->items = items;
    *out = (embi_value){0};
    out->kind = EMBI_V_TRACKS;
    out->tracks = tr;
    return true;
}

/* ------------------------------------------------------------------ grid lines */

bool embi_parse_grid_line(embi_arena *a, embi_cur *c, embi_value *out)
{
    if (cv_ident(cur_peek(c), "auto")) {
        c->i++;
        *out = v_kw(EMBI_KW_auto);
        return true;
    }
    bool span = false, have_int = false;
    int32_t num = 0;
    embi_atom name = 0;
    uint32_t taken = 0;
    while (taken < 3 && !cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (cv_ident(t, "span") && !span) {
            span = true;
        } else if (t->tok.type == EMBI_CSS_NUMBER && t->tok.flag && !have_int) {
            if (t->tok.num < -1e9 || t->tok.num > 1e9) {
                return false;
            }
            num = (int32_t)t->tok.num;
            have_int = true;
        } else if (t->tok.type == EMBI_CSS_IDENT && name == 0 && line_name_ok(t->tok.value)) {
            name = embi_atom_intern(t->tok.value.ptr, t->tok.value.len);
        } else {
            break;
        }
        c->i++;
        taken++;
    }
    if (taken == 0) {
        return false;
    }
    if (span) {
        if (!have_int && name == 0) {
            return false;
        }
        if (have_int && num < 1) {
            return false;
        }
        if (!have_int) {
            num = 1;
        }
    } else {
        if (have_int && num == 0) {
            return false;
        }
        if (!have_int && name == 0) {
            return false;
        }
    }
    embi_grid_line *gl = EMBI_NEW(a, embi_grid_line);
    gl->span = span;
    gl->num = num;
    gl->name = name;
    *out = (embi_value){0};
    out->kind = EMBI_V_GRID_LINE;
    out->gline = gl;
    return true;
}

bool embi_gram_grid_line(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_grid_line(g->a, c, out);
}

/* ------------------------------------------------------------------ grid-template-areas */

static bool area_name_ok(const char *s, uint32_t n)
{
    if (n == 0) {
        return false;
    }
    unsigned char c0 = (unsigned char)s[0];
    uint32_t i = 0;
    if (c0 == '-') {
        if (n < 2) {
            return false;
        }
        c0 = (unsigned char)s[1];
        i = 1;
    }
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_' || c0 >= 0x80)) {
        return false;
    }
    for (i++; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch >= 0x80)) {
            return false;
        }
    }
    return true;
}

/* Validates a list of row strings: same cell count, every named area a filled rectangle. */
bool embi_grid_areas_valid(embi_arena *a, const embi_str *rows, uint32_t nrows);
bool embi_grid_areas_valid(embi_arena *a, const embi_str *rows, uint32_t nrows)
{
    if (nrows == 0) {
        return false;
    }
    uint32_t ncols = 0;
    /* cell names as (ptr, len); null cells have len 0 */
    embi_str *cells = nullptr;
    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t cols = 0;
        const char *s = rows[r].ptr;
        uint32_t n = rows[r].len;
        uint32_t i = 0;
        embi_str *rowcells = EMBI_NEW_ARRAY(a, embi_str, n + 1);
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
                }
            }
            if (dots) {
                rowcells[cols++] = (embi_str){nullptr, 0};
            } else if (area_name_ok(s + st, i - st)) {
                rowcells[cols++] = (embi_str){s + st, i - st};
            } else {
                return false;
            }
        }
        if (cols == 0) {
            return false;
        }
        if (r == 0) {
            ncols = cols;
            cells = EMBI_NEW_ARRAY(a, embi_str, (size_t)nrows * ncols);
        } else if (cols != ncols) {
            return false;
        }
        memcpy(cells + (size_t)r * ncols, rowcells, sizeof(embi_str) * cols);
    }
    /* Each name: bounding box must be completely filled with that name. */
    for (uint32_t r = 0; r < nrows; r++) {
        for (uint32_t cidx = 0; cidx < ncols; cidx++) {
            embi_str nm = cells[(size_t)r * ncols + cidx];
            if (nm.len == 0) {
                continue;
            }
            uint32_t r0 = nrows, r1 = 0, c0 = ncols, c1 = 0, count = 0;
            for (uint32_t rr = 0; rr < nrows; rr++) {
                for (uint32_t cc = 0; cc < ncols; cc++) {
                    embi_str o = cells[(size_t)rr * ncols + cc];
                    if (o.len == nm.len && memcmp(o.ptr, nm.ptr, nm.len) == 0) {
                        count++;
                        if (rr < r0) r0 = rr;
                        if (rr > r1) r1 = rr;
                        if (cc < c0) c0 = cc;
                        if (cc > c1) c1 = cc;
                    }
                }
            }
            if (count != (r1 - r0 + 1) * (c1 - c0 + 1)) {
                return false;
            }
        }
    }
    return true;
}

bool embi_gram_grid_areas(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *rows = EMBI_NEW_ARRAY(g->a, embi_value, n);
    embi_str *strs = EMBI_NEW_ARRAY(g->a, embi_str, n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_cv *t = cur_take(c);
        if (!cv_string(t)) {
            return false;
        }
        rows[i] = (embi_value){0};
        rows[i].kind = EMBI_V_STRING;
        rows[i].str = t->tok.value;
        strs[i] = t->tok.value;
    }
    if (!embi_grid_areas_valid(g->a, strs, n)) {
        return false;
    }
    return embi_make_list(g->a, rows, n, EMBI_SEP_SPACE, out);
}
