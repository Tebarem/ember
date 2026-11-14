/*
 * Tables (CSS Tables 3 with CSS 2.1 section 17 where Tables 3 is silent), W3g.
 *
 * The table box is its own wrapper: its margins, border and padding are the table element's, and
 * block_pass lays out its content here (embi_table_content). The content is the captions (above or
 * below the grid), the grid of row groups, rows and cells, and the spacing between them.
 *
 * The model (model_build). The rows are in display order: header groups, then the bodies and the
 * rows that sit directly in the table, then the footer groups. Each cell is placed in the first column
 * its row does not already cover by a cell that started above (rowspan), with colspan and rowspan
 * clamped as HTML does (rowspan 0 covers the rest of its row group). Each cell has a border-box
 * contribution: the minimum is its min-content width, and the maximum is its max-content width, or
 * its width when that is larger (CSS Tables 3 section 3.5, as Chromium computes it). A spanning cell
 * that needs more width than its columns give shares the excess among them.
 *
 * Collapsed borders (build_collapse, CSS 2.1 section 17.6.2). Every edge unit of the grid (a horizontal
 * unit between two rows and one column, a vertical unit between two columns and one row, the outer
 * ones included) has candidates from the cells, rows, row groups, columns, column groups and the
 * table on that edge. The winner follows the conflict rules: hidden wins, then the widest border, then
 * the style (double, solid, dashed, dotted, ridge, outset, groove, inset, none), then the element
 * (cell, row, row group, column, column group, table), then the one that is first (top, then left).
 * A cell's half borders are the halves of its winners on its own sides: the start half (top, left) is
 * floor(w / 2) and the end half (bottom, right) is the rest, so two halves add up to the border. The
 * outer halves belong to the table, which has half of its outer winners and no padding. The winners
 * are kept on the table fragment (embi_tfrag.coll) for W4 to paint.
 *
 * The layout. Columns: auto layout distributes the content width W between the columns' minimum and
 * maximum (table_col.c); fixed layout (table-layout: fixed) takes the first row's widths. Each cell
 * is laid out once at its column span width to measure its height, which gives the row heights
 * (a cell that spans rows shares its extra height among them, and baseline cells line up on their
 * first baselines). Percentage row and cell heights resolve against the table's definite height. A
 * table taller than its rows gets the extra height distributed among the rows. Each cell is then laid
 * out at its final height, with vertical-align applied to its content.
 *
 * Visibility: a row or row group with visibility: collapse has no height, and a column or column group
 * with it has no width (the table shrinks by them). empty-cells: hide (separate model) flags the cells
 * that have no content so that W4 paints no background or border for them (EMBI_FF_HIDE_EMPTY).
 *
 * Not yet (see tests/layout/expectations/css-tables.txt): the collapsed-column table size
 * (col-collapse-table-size.html has no checks). The outer collapsed borders are the winners of the full
 * model (cells, rows, columns and the table). The decisions
 * on percentages, fixed layout and spacing are explained in the code below.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

/* The vertical-align of a cell. */
enum {
    VA_TOP = 0,
    VA_MIDDLE,
    VA_BOTTOM,
    VA_BASELINE,
};

/* The element priorities of the collapsed border conflicts (CSS 2.1 section 17.6.2.1). */
enum {
    PRIO_TABLE = 0,
    PRIO_COLGROUP,
    PRIO_COL,
    PRIO_ROWGROUP,
    PRIO_ROW,
    PRIO_CELL,
};

/* One candidate or winner border of an edge unit. w is 0 when hidden or none. */
typedef struct tbord {
    float w;
    uint16_t kw;    /* the style keyword (EMBI_KW_*) */
    uint8_t rank;   /* style precedence: double 8 .. inset 1, none 0 */
    uint8_t prio;   /* PRIO_* */
    uint8_t side;   /* the side of the owner: 0 top, 1 right, 2 bottom, 3 left */
    bool hidden;
    const embi_box *owner;
} tbord;

/* One cell of the grid. The measured values belong to the layout of the content. */
typedef struct tcell {
    embi_box *box;
    uint32_t row; /* the row it starts in */
    uint32_t col; /* its first column */
    uint32_t cspan; /* at least 1; after the track merging, the columns left under the cell */
    uint32_t ospan; /* the colspan it was placed with: a cell that spanned several columns does not fix a column's width */
    uint32_t rspan; /* at least 1, rowspan 0 resolved to the rest of its row group */
    float eh, ev;   /* border and padding, horizontal and vertical */
    float bd[4];    /* the collapsed half borders: top, right, bottom, left (zero when separate) */
    float cmin;     /* border-box contribution to a column's minimum */
    float cmax;     /* border-box contribution to a column's maximum */
    float spec;     /* the width property as a border-box length, or NaN */
    float pct;      /* the width property as a fraction, or NaN */
    float hspec;    /* the height property as a border-box length (a minimum), or NaN */
    float sw;       /* the border-box width over its columns */
    float mh;       /* the measured border-box height */
    float nat;      /* the measured content height */
    float bl;       /* the measured first baseline from the border-box top, or NaN */
    uint8_t valign; /* VA_* */
    float hpct;     /* the height property as a fraction of the table's height, or NaN */
    embi_frag *mf; /* size_rows' layout of the cell (rspan 1), reused by place_cells_final at the same height */
    float mw;      /* the column width size_rows laid the cell out at */
    bool bb;        /* box-sizing: border-box */
    bool hscroll;   /* a scroll container: its height is its height property, not its content's */
    bool empty;     /* no content (only collapsible white space) */
    bool hide;      /* empty and empty-cells: hide in the separate model */
} tcell;

/* A table has at most this many columns; cells and spans past it are ignored (Chromium's limit). */
#define TABLE_MAX_COLS 1000u
/* The collapsed-border model is built only for grids up to this many units (R * C). */
#define TABLE_MAX_UNITS (1u << 18)

/* One column: its contributions from the cells and column elements, and its used width. */
typedef struct tcol {
    float min, max; /* contributions (border box) of the cells, and the columns' own widths */
    float spec;     /* the width of a column element or a fixed cell, px, or NaN */
    float pct;      /* a percentage width as a fraction, or NaN */
    float pe;       /* border and padding a content-box single-span percentage cell adds to the width it asks for */
    float x, w;     /* the used position (content coordinates) and width */
    uint8_t fixedc; /* a fixed width (spec) applies */
    bool vcol;      /* visibility: collapse: the column has no width */
    embi_box *colbox; /* the column element, or nullptr (generated columns, column groups) */
    embi_box *grpbox; /* the column group, or nullptr */
} tcol;

/* One row. The row's frame is set in embi_table_content. */
typedef struct trow {
    embi_box *box;
    uint32_t group; /* the row group index, or UINT32_MAX for a row directly in the table */
    float hspec;    /* the height property as a minimum, or NaN */
    float hpct;     /* the height property as a fraction of the table's height, or NaN */
    float h;        /* the used height */
    float top;      /* the top in content coordinates */
    float bl_base;  /* the highest baseline of the baseline cells that start here, or NaN */
    float bl_any;   /* the highest baseline of any cell that starts here, or NaN */
    bool bl_real;   /* some baseline cell has content: with none, the row's baseline is its bottom edge */
    float bl_fin;   /* the same as bl_base, from the final layout of the cells (percentages resolved) */
    bool vcol;      /* visibility: collapse: the row has no height */
    embi_frag *frag;
} trow;

/* One row group: the rows [first, end). */
typedef struct tgroup {
    embi_box *box;
    uint32_t first, end;
    float hspec;   /* the height property as a minimum, or NaN */
    float top, h;
    embi_frag *frag;
} tgroup;

/* One caption: its margins and size after its layout. */
typedef struct tcap {
    embi_box *box;
    bool bottom;
    float ml, mt, mb, h;
    embi_frag *frag;
} tcap;

typedef struct tmodel {
    EMBI_VEC(tcell) cells;
    EMBI_VEC(tcol) cols;
    EMBI_VEC(trow) rows;
    EMBI_VEC(tgroup) groups;
    EMBI_VEC(tcap) caps;
    EMBI_VEC(tbord) hb; /* the horizontal edge winners, (R + 1) * C (collapse only) */
    EMBI_VEC(tbord) vb; /* the vertical edge winners, R * (C + 1) (collapse only) */
    EMBI_VEC(int32_t) grid; /* the cell covering each unit, R * C (collapse only), -1 for none */
    embi_box *table;
    float cq_w, cq_h; /* the container size (cq units) the cell contributions were computed under, NaN for none */
    uint32_t R, C;  /* the rows and the columns of the grid */
    float sph, spv; /* the horizontal and vertical border-spacing (0 when collapsed) */
    bool fixed;     /* table-layout: fixed */
    bool nested;    /* the table is inside a cell: its cells' percentage widths are ignored */
    bool collapse;  /* border-collapse: collapse */
    bool rtl;       /* direction: rtl: column 0 is at the right; the grid and the edges are logical */
    float ch_def;   /* the table's definite content height (percentage heights of cells), or NaN */
    bool restricted; /* the table's height is not auto or its max-height not none, as specified (even a percentage
                      * that behaves as auto): the cells' content resolves percentage heights against the cells */
    float grid_h;   /* the height of the grid, set by place_rows */
} tmodel;

static void model_free(tmodel *m)
{
    embi_vec_free(&m->cells);
    embi_vec_free(&m->cols);
    embi_vec_free(&m->rows);
    embi_vec_free(&m->groups);
    embi_vec_free(&m->caps);
    embi_vec_free(&m->hb);
    embi_vec_free(&m->vb);
    embi_vec_free(&m->grid);
}

/* The border-box height a cell needs in its rows: a scroll container with a height keeps that height,
 * and otherwise its content height, or its height property when that is larger. */
static float cell_need(const tcell *t)
{
    float h = t->mh;
    if (!isnan(t->hspec) && t->hspec > h) {
        h = t->hspec;
    }
    return h;
}

/* True when a table box lies inside a table cell (its box tree has a cell ancestor). */
bool embi_table_in_cell(const embi_box *b)
{
    for (const embi_box *p = b->parent; p != nullptr; p = p->parent) {
        if (p->tr == EMBI_TR_CELL) {
            return true;
        }
    }
    return false;
}

static bool border_box_sizing(const embi_lctx *c, const embi_box *b)
{
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_border_box;
}

static bool keyword_is(const embi_lctx *c, const embi_box *b, embi_prop_id p, uint16_t kw)
{
    const embi_value *v = embi_lay_prop(c, b, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == kw;
}

static bool collapse_of(const embi_lctx *c, const embi_box *b)
{
    return keyword_is(c, b, EMBI_PROP_border_collapse, EMBI_KW_collapse);
}

/* The table's own direction: its columns run from the inline-start side. */
static bool table_rtl(const embi_lctx *c, const embi_box *b)
{
    return keyword_is(c, b, EMBI_PROP_direction, EMBI_KW_rtl);
}

static bool vis_collapse(const embi_lctx *c, const embi_box *b)
{
    return b != nullptr && keyword_is(c, b, EMBI_PROP_visibility, EMBI_KW_collapse);
}

/* A length property of a cell as a border-box size: NaN for auto, keywords and percentages. */
static float cell_len(embi_lctx *c, embi_box *cb, embi_prop_id prop, float edges, bool bb)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, cb, prop, &cx);
    if (v == nullptr || embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD ||
        v->kind == EMBI_V_PERCENT) {
        return NAN;
    }
    float w = embi_lay_len(c, cb, v, NAN);
    if (isnan(w)) {
        return NAN;
    }
    if (cx != nullptr) {
        /* calc-size(<length>, <calc>): the calc-sum sees the length basis (CSS Values 5 section 10.1). */
        float r = embi_lay_size_eval(c, cb, cx, w, NAN);
        if (!isnan(r)) {
            w = r;
        }
    }
    w = bb ? w : w + edges;
    if (w < edges) {
        w = edges; /* a border-box size is never below the border and padding */
    }
    return w < 0 ? 0.0f : w;
}

/* The fraction a width or height stands for when it is a percentage or a calc() that is exactly a percentage
 * (calc(50% + 0px) is calc(50%)); NaN otherwise. */
static float cell_fraction(embi_lctx *c, embi_box *cb, embi_prop_id prop)
{
    const embi_value *v = embi_lay_prop(c, cb, prop);
    if (v == nullptr) {
        return NAN;
    }
    if (v->kind == EMBI_V_PERCENT) {
        return v->num < 0 ? 0.0f : (float)(v->num / 100.0);
    }
    if (v->kind == EMBI_V_CALC) {
        float a = embi_lay_len(c, cb, v, 0.0f);
        float b = embi_lay_len(c, cb, v, 100.0f);
        float d = embi_lay_len(c, cb, v, 300.0f);
        if (!isnan(a) && !isnan(b) && !isnan(d) && fabsf(a) < 0.001f && fabsf(d - 3 * b) < 0.01f) {
            return b < 0 ? 0.0f : b / 100.0f;
        }
    }
    return NAN;
}

static uint8_t valign_of(embi_lctx *c, embi_box *cb)
{
    const embi_value *v = embi_lay_prop(c, cb, EMBI_PROP_vertical_align);
    if (v == nullptr || v->kind != EMBI_V_KEYWORD) {
        return VA_BASELINE;
    }
    switch (v->kw) {
    case EMBI_KW_top:
        return VA_TOP;
    case EMBI_KW_middle:
        return VA_MIDDLE;
    case EMBI_KW_bottom:
        return VA_BOTTOM;
    default:
        return VA_BASELINE;
    }
}

/* The border-spacing of the table (CSS 2.1 section 17.6): one value for both, or two (horizontal,
 * vertical). Negative values are invalid and count as 0. */
static void read_spacing(embi_lctx *c, embi_box *b, float *h, float *v)
{
    *h = 0;
    *v = 0;
    const embi_value *sv = embi_lay_prop(c, b, EMBI_PROP_border_spacing);
    if (sv == nullptr) {
        return;
    }
    uint32_t n = 0;
    const embi_value *it = embi_value_items(sv, &n);
    if (n == 0) {
        return;
    }
    float a = embi_lay_len(c, b, &it[0], NAN);
    if (isnan(a) || a < 0) {
        a = 0;
    }
    float bb = a;
    if (n > 1) {
        bb = embi_lay_len(c, b, &it[1], NAN);
        if (isnan(bb) || bb < 0) {
            bb = 0;
        }
    }
    *h = a;
    *v = bb;
}

static bool caption_bottom(embi_lctx *c, embi_box *k)
{
    const embi_value *v = embi_lay_prop(c, k, EMBI_PROP_caption_side);
    return v != nullptr && v->kind == EMBI_V_KEYWORD &&
           (v->kw == EMBI_KW_bottom || v->kw == EMBI_KW_block_end);
}

/* ------------------------------------------------------------------ collapsed borders */

/* The precedence of a border style in a conflict (CSS 2.1 section 17.6.2.1), hidden excluded. */
static uint8_t style_rank(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_double:
        return 8;
    case EMBI_KW_solid:
        return 7;
    case EMBI_KW_dashed:
        return 6;
    case EMBI_KW_dotted:
        return 5;
    case EMBI_KW_ridge:
        return 4;
    case EMBI_KW_outset:
        return 3;
    case EMBI_KW_groove:
        return 2;
    case EMBI_KW_inset:
        return 1;
    default:
        return 0;
    }
}

/* The border of one side of box b as a candidate for an edge (prio is its element priority). */
static tbord side_bord(const embi_lctx *c, const embi_box *b, int side, uint8_t prio)
{
    static const embi_prop_id sp[4] = {EMBI_PROP_border_top_style, EMBI_PROP_border_right_style,
                                       EMBI_PROP_border_bottom_style, EMBI_PROP_border_left_style};
    tbord t;
    memset(&t, 0, sizeof t);
    const embi_value *st = embi_lay_prop(c, b, sp[side]);
    t.kw = (st != nullptr && st->kind == EMBI_V_KEYWORD) ? st->kw : EMBI_KW_none;
    t.hidden = t.kw == EMBI_KW_hidden;
    t.rank = t.hidden ? 0 : style_rank(t.kw);
    t.w = t.hidden ? 0.0f : embi_lay_border_side(c, b, side);
    /* A collapsed border is as wide as its whole device pixels (a width below one pixel counts as one). */
    t.w = t.w >= 1.0f ? floorf(t.w) : (t.w > 0.0f ? 1.0f : 0.0f);
    t.prio = prio;
    t.side = (uint8_t)side;
    t.owner = b;
    return t;
}

/* True when border a wins over border b (CSS 2.1 section 17.6.2.1): hidden, then the widest, then the
 * style, then the element priority. Ties keep the earlier candidate (the top or left one). */
static bool bord_wins(const tbord *a, const tbord *b)
{
    if (a->hidden != b->hidden) {
        return a->hidden;
    }
    if (a->w != b->w) {
        return a->w > b->w;
    }
    if (a->rank != b->rank) {
        return a->rank > b->rank;
    }
    return a->prio > b->prio;
}

static tbord pick_border(const tbord *cs, int n)
{
    tbord best;
    memset(&best, 0, sizeof best);
    best.kw = EMBI_KW_none;
    for (int i = 0; i < n; i++) {
        if (i == 0 || bord_wins(&cs[i], &best)) {
            best = cs[i];
        }
    }
    return best;
}

/* The start half of a width (top and left: floor) and the end half (bottom and right: the rest). */
static float start_half(float w)
{
    return floorf(w / 2.0f);
}

static float end_half(float w)
{
    return w - floorf(w / 2.0f);
}

/* ------------------------------------------------------------------ model building */

static void add_cols(embi_lctx *c, tmodel *m, embi_box *k, embi_box *grp, float gw, float gp)
{
    uint32_t span = embi_table_col_span(k);
    float w = embi_table_col_width_clamped(c, k);
    float p = embi_table_col_pct(c, k);
    if (isnan(w)) {
        w = gw;
    }
    if (isnan(p)) {
        p = gp;
    }
    bool is_col = k->tr == EMBI_TR_COL;
    if (m->cols.len >= TABLE_MAX_COLS) {
        return;
    }
    if (span > TABLE_MAX_COLS - m->cols.len) {
        span = TABLE_MAX_COLS - m->cols.len;
    }
    for (uint32_t i = 0; i < span; i++) {
        tcol col;
        memset(&col, 0, sizeof col);
        col.spec = w;
        col.pct = p;
        col.fixedc = isnan(w) ? 0 : 1;
        col.colbox = is_col ? k : nullptr;
        col.grpbox = grp;
        col.vcol = vis_collapse(c, k) || (grp != nullptr && vis_collapse(c, grp));
        embi_vec_push(&m->cols, col);
    }
}

static float row_height(embi_lctx *c, embi_box *rb)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, rb, EMBI_PROP_height, &cx);
    if (v == nullptr || embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD ||
        v->kind == EMBI_V_PERCENT) {
        return NAN;
    }
    float h = embi_lay_len(c, rb, v, NAN);
    if (isnan(h)) {
        return NAN;
    }
    if (cx != nullptr) {
        float r = embi_lay_size_eval(c, rb, cx, h, NAN);
        if (!isnan(r)) {
            h = r;
        }
    }
    return h < 0 ? 0.0f : h;
}

static float row_pct(embi_lctx *c, embi_box *rb)
{
    const embi_value *v = embi_lay_prop(c, rb, EMBI_PROP_height);
    if (v == nullptr || v->kind != EMBI_V_PERCENT) {
        return NAN;
    }
    return v->num < 0 ? 0.0f : (float)(v->num / 100.0);
}

static void add_row(embi_lctx *c, tmodel *m, embi_box *rb, uint32_t group, embi_box *gbox)
{
    trow r;
    memset(&r, 0, sizeof r);
    r.box = rb;
    r.group = group;
    r.hspec = row_height(c, rb);
    r.hpct = row_pct(c, rb);
    r.bl_base = NAN;
    r.bl_any = NAN;
    r.vcol = vis_collapse(c, rb) || vis_collapse(c, gbox);
    embi_vec_push(&m->rows, r);
}

static void add_group(embi_lctx *c, tmodel *m, embi_box *k)
{
    uint32_t gi = m->groups.len;
    tgroup g;
    memset(&g, 0, sizeof g);
    g.box = k;
    g.first = m->rows.len;
    g.end = m->rows.len;
    g.hspec = row_height(c, k);
    embi_vec_push(&m->groups, g);
    for (embi_box *r = k->first; r != nullptr; r = r->next) {
        if (r->tr == EMBI_TR_ROW) {
            add_row(c, m, r, gi, k);
        }
    }
    m->groups.data[gi].end = m->rows.len;
}

/* The last row that a cell starting in row r covers when its rowspan is 0: the end of its group. */
static uint32_t row_last(const tmodel *m, uint32_t r)
{
    uint32_t g = m->rows.data[r].group;
    if (g != UINT32_MAX) {
        return m->groups.data[g].end - 1;
    }
    uint32_t e = r;
    while (e + 1 < m->rows.len && m->rows.data[e + 1].group == UINT32_MAX) {
        e++;
    }
    return e;
}

/* Places the cells of every row (CSS 2.1 section 17.5, the table model). cov[j] is the last row that
 * a cell covers in column j, from a cell that started above or in this row. */
static void place_cells(embi_lctx *c, tmodel *m)
{
    (void)c;
    EMBI_VEC(int32_t) cov = {nullptr, 0, 0};
    for (uint32_t r = 0; r < m->rows.len; r++) {
        uint32_t col = 0;
        for (embi_box *cb = m->rows.data[r].box->first; cb != nullptr; cb = cb->next) {
            if (cb->tr != EMBI_TR_CELL) {
                continue;
            }
            uint32_t cs = embi_table_attr_uint(cb, "colspan", 1, 1, TABLE_MAX_COLS);
            uint32_t rs = embi_table_attr_uint(cb, "rowspan", 1, 0, 65534);
            /* A span is cut at the end of its row group (Chromium); rowspan=0 spans to that end. */
            uint32_t room = row_last(m, r) - r + 1;
            if (rs == 0 || rs > room) {
                rs = room;
            }
            while (col < cov.len && cov.data[col] >= (int32_t)r) {
                col++;
            }
            if (col >= TABLE_MAX_COLS) {
                continue; /* past the column cap: the cell takes no part in the grid */
            }
            if (cs > TABLE_MAX_COLS - col) {
                cs = TABLE_MAX_COLS - col;
            }
            embi_vec_reserve(&cov, col + cs);
            while (cov.len < col + cs) {
                embi_vec_push(&cov, -1);
            }
            for (uint32_t j = col; j < col + cs; j++) {
                if (cov.data[j] < (int32_t)(r + rs - 1)) {
                    cov.data[j] = (int32_t)(r + rs - 1);
                }
            }
            tcell t;
            memset(&t, 0, sizeof t);
            t.box = cb;
            t.row = r;
            t.col = col;
            t.cspan = cs;
            t.ospan = cs;
            t.rspan = rs;
            embi_vec_push(&m->cells, t);
            col += cs;
        }
    }
    embi_vec_free(&cov);
    uint32_t n = m->cols.len;
    for (uint32_t i = 0; i < m->cells.len; i++) {
        uint32_t end = m->cells.data[i].col + m->cells.data[i].cspan;
        if (end > n) {
            n = end;
        }
    }
    while (m->cols.len < n) {
        tcol col;
        memset(&col, 0, sizeof col);
        col.spec = NAN;
        col.pct = NAN;
        embi_vec_push(&m->cols, col);
    }
}

/* Track merging (CSS Tables 3 section 3.3, "dimensioning the row/column grid", step 2): the columns in
 * which no cell starts are empty, and they merge into the track before them, so they add neither a width
 * nor a border-spacing. A column with a specified width stays a track, unless that width is zero (WPT
 * column-track-merging.html: <col span=10> merges, <col span=10 width=30px> and a percentage do not).
 * The columns are removed from the model and the cells' columns are renumbered, so the widths, the
 * spans, the spacing and the collapsed borders all see the merged grid. */
static bool col_is_pct(const tcol *c);

static void merge_empty_cols(tmodel *m)
{
    uint32_t n = m->cols.len;
    if (n == 0) {
        return;
    }
    EMBI_VEC(uint32_t) keep = {nullptr, 0, 0}; /* keep[j]: the number of columns before j that stay */
    embi_vec_reserve(&keep, n + 1);
    keep.len = n + 1;
    EMBI_VEC(uint8_t) used = {nullptr, 0, 0};
    embi_vec_reserve(&used, n);
    used.len = n;
    if (keep.data == nullptr || used.data == nullptr) {
        embi_vec_free(&keep);
        embi_vec_free(&used);
        return;
    }
    memset(used.data, 0, n);
    for (uint32_t i = 0; i < m->cells.len; i++) {
        used.data[m->cells.data[i].col] = 1;
    }
    uint32_t stay = 0;
    float pct_used = 0.0f; /* the percentages of the columns before (they cannot add up to more than 100%) */
    for (uint32_t j = 0; j < n; j++) {
        const tcol *col = &m->cols.data[j];
        float cp = col_is_pct(col) ? col->pct : 0.0f;
        for (uint32_t i = 0; i < m->cells.len; i++) {
            const tcell *t = &m->cells.data[i];
            if (t->col == j && t->cspan == 1 && !isnan(t->pct) && t->pct > cp) {
                cp = t->pct;
            }
        }
        /* A percentage column that no percentage is left for takes no width, so an empty one of them merges. */
        bool pct_w = cp > 0.0f && pct_used < 0.9999f;
        pct_used += cp;
        bool has_w = (!isnan(col->spec) && col->spec != 0.0f) || pct_w;
        keep.data[j] = stay;
        if (used.data[j] || has_w || col->vcol) {
            stay++;
        }
    }
    keep.data[n] = stay;
    if (stay != n) {
        for (uint32_t i = 0; i < m->cells.len; i++) {
            tcell *t = &m->cells.data[i];
            uint32_t a = keep.data[t->col], e = keep.data[t->col + t->cspan];
            t->col = a;
            t->cspan = e > a ? e - a : 1;
        }
        uint32_t q = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (keep.data[j + 1] != keep.data[j]) {
                m->cols.data[q++] = m->cols.data[j];
            }
        }
        m->cols.len = q;
    }
    embi_vec_free(&keep);
    embi_vec_free(&used);
}

/* The winners of every edge unit of the grid, and the collapsed half borders of the cells. The
 * candidates of a horizontal unit (above row r, column j) are the cells that end or start on it,
 * the rows above and below, the row groups that end or start there, and the table when it is an outer
 * edge. A vertical unit (left of column c, row i) has the same kinds for columns, column groups and
 * rows, and the table on the outer edges. */
static void build_collapse(embi_lctx *c, tmodel *m)
{
    uint32_t R = m->rows.len, C = m->cols.len;
    if (R == 0 || C == 0 || (size_t)R * C > TABLE_MAX_UNITS) {
        return;
    }
    m->R = R;
    m->C = C;
    for (size_t i = 0; i < (size_t)R * C; i++) {
        embi_vec_push(&m->grid, -1);
    }
    for (uint32_t k = 0; k < m->cells.len; k++) {
        const tcell *t = &m->cells.data[k];
        for (uint32_t i = t->row; i < t->row + t->rspan && i < R; i++) {
            for (uint32_t j = t->col; j < t->col + t->cspan && j < C; j++) {
                m->grid.data[i * C + j] = (int32_t)k;
            }
        }
    }
    for (uint32_t r = 0; r <= R; r++) {
        for (uint32_t j = 0; j < C; j++) {
            tbord cs[8];
            int n = 0;
            if (r > 0) {
                int32_t k = m->grid.data[(r - 1) * C + j];
                if (k >= 0 && m->cells.data[k].row + m->cells.data[k].rspan == r) {
                    cs[n++] = side_bord(c, m->cells.data[k].box, 2, PRIO_CELL);
                }
                cs[n++] = side_bord(c, m->rows.data[r - 1].box, 2, PRIO_ROW);
                uint32_t g = m->rows.data[r - 1].group;
                if (g != UINT32_MAX && m->groups.data[g].end == r) {
                    cs[n++] = side_bord(c, m->groups.data[g].box, 2, PRIO_ROWGROUP);
                }
            }
            if (r < R) {
                int32_t k = m->grid.data[r * C + j];
                if (k >= 0 && m->cells.data[k].row == r) {
                    cs[n++] = side_bord(c, m->cells.data[k].box, 0, PRIO_CELL);
                }
                cs[n++] = side_bord(c, m->rows.data[r].box, 0, PRIO_ROW);
                uint32_t g = m->rows.data[r].group;
                if (g != UINT32_MAX && m->groups.data[g].first == r) {
                    cs[n++] = side_bord(c, m->groups.data[g].box, 0, PRIO_ROWGROUP);
                }
            }
            /* A column and its group run the whole height of the grid: their top and bottom borders are on its outer
             * edges (CSS 2.1 section 17.6.2.1). */
            if (r == 0 || r == R) {
                int side = r == 0 ? 0 : 2;
                const tcol *cl = &m->cols.data[j];
                if (cl->colbox != nullptr) {
                    cs[n++] = side_bord(c, cl->colbox, side, PRIO_COL);
                }
                if (cl->grpbox != nullptr) {
                    cs[n++] = side_bord(c, cl->grpbox, side, PRIO_COLGROUP);
                }
                cs[n++] = side_bord(c, m->table, side, PRIO_TABLE);
            }
            embi_vec_push(&m->hb, pick_border(cs, n));
        }
    }
    /* The columns are in logical order (column 0 at the inline start). Edge cc lies between column
     * cc - 1 (the "before" side) and column cc (the "after" side). In an rtl table the before side is
     * physically on the right, so it contributes its left border. The candidates are listed in
     * logical order, before first: a tie keeps the left one in ltr and the right one in rtl (CSS 2.1
     * 17.6.2.1), and the before side is the physically right one in rtl. */
    int sb = m->rtl ? 3 : 1; /* the physical side of a before-box that touches the edge */
    int sa = m->rtl ? 1 : 3; /* the same for an after-box */
    for (uint32_t i = 0; i < R; i++) {
        for (uint32_t cc = 0; cc <= C; cc++) {
            tbord before[7], after[7];
            int nb = 0, na = 0;
            if (cc > 0) {
                int32_t k = m->grid.data[i * C + cc - 1];
                if (k >= 0 && m->cells.data[k].col + m->cells.data[k].cspan == cc) {
                    before[nb++] = side_bord(c, m->cells.data[k].box, sb, PRIO_CELL);
                }
            }
            if (cc < C) {
                int32_t k = m->grid.data[i * C + cc];
                if (k >= 0 && m->cells.data[k].col == cc) {
                    after[na++] = side_bord(c, m->cells.data[k].box, sa, PRIO_CELL);
                }
            }
            const tcol *lc = cc > 0 ? &m->cols.data[cc - 1] : nullptr;
            const tcol *rc = cc < C ? &m->cols.data[cc] : nullptr;
            if (lc != nullptr && lc->colbox != nullptr && (rc == nullptr || rc->colbox != lc->colbox)) {
                before[nb++] = side_bord(c, lc->colbox, sb, PRIO_COL);
            }
            if (rc != nullptr && rc->colbox != nullptr && (lc == nullptr || lc->colbox != rc->colbox)) {
                after[na++] = side_bord(c, rc->colbox, sa, PRIO_COL);
            }
            if (lc != nullptr && lc->grpbox != nullptr && (rc == nullptr || rc->grpbox != lc->grpbox)) {
                before[nb++] = side_bord(c, lc->grpbox, sb, PRIO_COLGROUP);
            }
            if (rc != nullptr && rc->grpbox != nullptr && (lc == nullptr || lc->grpbox != rc->grpbox)) {
                after[na++] = side_bord(c, rc->grpbox, sa, PRIO_COLGROUP);
            }
            /* The row and its row group touch the outer edges of the grid (the group's left and right borders run the
             * height of its rows). */
            uint32_t rg = m->rows.data[i].group;
            const embi_box *gbox = rg != UINT32_MAX ? m->groups.data[rg].box : nullptr;
            if (cc == 0) {
                after[na++] = side_bord(c, m->rows.data[i].box, sa, PRIO_ROW);
                if (gbox != nullptr) {
                    after[na++] = side_bord(c, gbox, sa, PRIO_ROWGROUP);
                }
                after[na++] = side_bord(c, m->table, sa, PRIO_TABLE);
            }
            if (cc == C) {
                before[nb++] = side_bord(c, m->rows.data[i].box, sb, PRIO_ROW);
                if (gbox != nullptr) {
                    before[nb++] = side_bord(c, gbox, sb, PRIO_ROWGROUP);
                }
                before[nb++] = side_bord(c, m->table, sb, PRIO_TABLE);
            }
            tbord cs[14];
            int n = 0;
            for (int q = 0; q < nb; q++) {
                cs[n++] = before[q];
            }
            for (int q = 0; q < na; q++) {
                cs[n++] = after[q];
            }
            embi_vec_push(&m->vb, pick_border(cs, n));
        }
    }
    /* The half borders of each cell: the maximum winner over its sides. */
    for (uint32_t k = 0; k < m->cells.len; k++) {
        tcell *t = &m->cells.data[k];
        uint32_t r2 = t->row + t->rspan, c2 = t->col + t->cspan;
        float top = 0, bot = 0, left = 0, right = 0;
        for (uint32_t j = t->col; j < c2 && j < C; j++) {
            top = fmaxf(top, m->hb.data[t->row * C + j].w);
            bot = fmaxf(bot, m->hb.data[r2 * C + j].w);
        }
        for (uint32_t i = t->row; i < r2 && i < R; i++) {
            left = fmaxf(left, m->vb.data[i * (C + 1) + t->col].w);
            right = fmaxf(right, m->vb.data[i * (C + 1) + c2].w);
        }
        /* Each edge is shared by the two boxes on its sides: the box after it (below, or at the inline end of it) takes
         * the start half (floor) and the box before it the rest (Chromium's NGTableBorders). The cells at the outer
         * edges of the grid take their half too; the table has the other half (embi_table_collapse_edges). left and
         * right are the logical before and after edges. */
        t->bd[0] = start_half(top);
        t->bd[2] = end_half(bot);
        if (m->rtl) {
            /* Mirrored: the logical before edge (left) is the physical right one and takes the same half as in ltr. */
            t->bd[1] = start_half(left);
            t->bd[3] = end_half(right);
        } else {
            t->bd[1] = end_half(right);
            t->bd[3] = start_half(left);
        }
    }
}

/* Whether a cell has no content: only white space that collapses away (CSS 2.1 section 17.6.1.1). */
static bool cell_empty(const embi_box *cb)
{
    for (const embi_box *k = cb->first; k != nullptr; k = k->next) {
        if (k->kind != EMBI_BK_TEXT) {
            return false;
        }
        for (uint32_t i = 0; i < k->text_len; i++) {
            char ch = k->text[i];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f') {
                return false;
            }
        }
    }
    return true;
}

/* The border-box sizes and the widths of the cells, from their boxes (no layout). */
static void cell_props(embi_lctx *c, tmodel *m)
{
    for (uint32_t i = 0; i < m->cells.len; i++) {
        tcell *t = &m->cells.data[i];
        embi_box *cb = t->box;
        embi_edges bdr, pad, mar;
        embi_lay_edges(c, cb, 0, &bdr, &pad, &mar);
        if (m->collapse) {
            t->eh = t->bd[1] + t->bd[3] + pad.l + pad.r;
            t->ev = t->bd[0] + t->bd[2] + pad.t + pad.b;
        } else {
            t->eh = bdr.l + bdr.r + pad.l + pad.r;
            t->ev = bdr.t + bdr.b + pad.t + pad.b;
        }
        bool bb = border_box_sizing(c, cb);
        float imn = 0, imx = 0;
        embi_lay_intrinsic(c, cb, &imn, &imx);
        t->pct = NAN;
        t->pct = cell_fraction(c, cb, EMBI_PROP_width);
        t->spec = cell_len(c, cb, EMBI_PROP_width, t->eh, bb);
        /* min-width and max-width of a cell: browsers apply them as a clamp of its width and of its contributions. */
        float cminw = cell_len(c, cb, EMBI_PROP_min_width, t->eh, bb);
        float cmaxw = cell_len(c, cb, EMBI_PROP_max_width, t->eh, bb);
        if (!isnan(cmaxw) && !isnan(t->spec) && t->spec > cmaxw) {
            t->spec = cmaxw;
        }
        if (!isnan(cminw) && !isnan(t->spec) && t->spec < cminw) {
            t->spec = cminw;
        }
        t->hspec = cell_len(c, cb, EMBI_PROP_height, t->ev, bb);
        t->hpct = NAN;
        t->hpct = cell_fraction(c, cb, EMBI_PROP_height);
        t->bb = bb;
        t->hscroll = (cb->flags & EMBI_BF_SCROLL) != 0;
        t->cmin = imn + t->eh;
        t->cmax = isnan(t->spec) ? imx + t->eh : fmaxf(imn + t->eh, t->spec);
        if (!isnan(cminw)) {
            t->cmin = fmaxf(t->cmin, cminw);
            t->cmax = fmaxf(t->cmax, cminw);
        }
        if (!isnan(cmaxw)) {
            t->cmax = fminf(t->cmax, fmaxf(cmaxw, t->cmin));
            if (cmaxw < t->cmin) {
                /* max-width clamps the min-content contribution too, below the content (min-width wins), as Chromium's
                 * contribution clamp does (baseline-empty-cell-001's reference: a non-breaking space in a max-width: 0
                 * cell takes no width). */
                t->cmin = fmaxf(cmaxw, isnan(cminw) ? 0.0f : cminw);
                t->cmax = fmaxf(t->cmin, fminf(t->cmax, cmaxw));
            }
        }
        t->valign = valign_of(c, cb);
        t->empty = cell_empty(cb);
        t->hide = !m->collapse && t->empty && keyword_is(c, cb, EMBI_PROP_empty_cells, EMBI_KW_hide);
    }
}

/* Merges a cell's border-box minimum and maximum into a column (auto layout). A constrained cell or
 * column (a fixed width) keeps its maximum; an unconstrained one only raises the minimum of a
 * constrained column, and its maximum is dropped when a constrained one arrives (Chromium, the rule of
 * WPT tentative/column-widths.html). A percentage keeps the largest. */
static void col_merge(tcol *col, float mn, float mx, bool cons, float pct)
{
    if (!isnan(pct) && pct > 0 && (isnan(col->pct) || pct > col->pct)) {
        col->pct = pct;
    }
    col->min = fmaxf(col->min, mn);
    if (col->fixedc) {
        if (cons) {
            col->max = fmaxf(col->max, mx);
        }
    } else if (cons) {
        col->max = fmaxf(mx, col->min);
        col->fixedc = 1;
    } else {
        col->max = fmaxf(col->max, mx);
    }
    if (col->max < col->min) {
        col->max = col->min;
    }
}

static bool col_is_pct(const tcol *c)
{
    return !isnan(c->pct) && c->pct > 0;
}

/* Gives the extra width e to the minima of the columns idx[0..k): the constrained columns first, up to
 * their maximum, in proportion to the room they have; then the unconstrained ones in proportion to
 * their maximum (equally when they are all 0), or all of them when there is no unconstrained one. */
static void span_fill_min(tcol *cs, const uint32_t *idx, uint32_t k, float e)
{
    if (e <= 0 || k == 0) {
        return;
    }
    float cost = 0;
    uint32_t nu = 0;
    for (uint32_t i = 0; i < k; i++) {
        const tcol *c = &cs[idx[i]];
        if (c->fixedc) {
            cost += fmaxf(0.0f, c->max - c->min);
        } else {
            nu++;
        }
    }
    if (cost > 0) {
        if (e <= cost) {
            for (uint32_t i = 0; i < k; i++) {
                tcol *c = &cs[idx[i]];
                if (c->fixedc) {
                    c->min += e * fmaxf(0.0f, c->max - c->min) / cost;
                }
            }
            return;
        }
        for (uint32_t i = 0; i < k; i++) {
            tcol *c = &cs[idx[i]];
            if (c->fixedc && c->max > c->min) {
                c->min = c->max;
            }
        }
        e -= cost;
    }
    float wsum = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < k; i++) {
        const tcol *c = &cs[idx[i]];
        if (nu == 0 || !c->fixedc) {
            wsum += c->max;
            cnt++;
        }
    }
    for (uint32_t i = 0; i < k; i++) {
        tcol *c = &cs[idx[i]];
        if (nu == 0 || !c->fixedc) {
            c->min += wsum > 0 ? e * c->max / wsum : e / (float)cnt;
        }
    }
}

/* A cell that spans columns cs[0..n) (CSS Tables 3 section 3.5, Chromium's order): its percentage goes to
 * the columns that have none and are not constrained, then its minimum and its maximum are shared out. */
static void span_cell(tmodel *m, const tcell *t)
{
    uint32_t n = t->cspan;
    tcol *cs = &m->cols.data[t->col];
    float gap = m->sph * (float)(n - 1);
    if (!isnan(t->pct) && t->pct > 0) {
        float sumpct = 0, sw = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (col_is_pct(&cs[j])) {
                sumpct += cs[j].pct;
            }
            sw += cs[j].max;
        }
        if (t->pct > sumpct) {
            for (uint32_t j = 0; j < n; j++) {
                if (col_is_pct(&cs[j]) || cs[j].fixedc) {
                    continue;
                }
                float portion = t->pct * (sw > 0 ? cs[j].max / sw : 1.0f / (float)n);
                if (portion > 0) {
                    cs[j].pct = portion;
                }
            }
        }
    }
    /* The minimum. */
    EMBI_VEC(uint32_t) idx = {nullptr, 0, 0};
    float etot = t->cmin - gap;
    bool anypct = false;
    for (uint32_t j = 0; j < n; j++) {
        anypct = anypct || col_is_pct(&cs[j]);
    }
    if (anypct) {
        float sum_s = 0, sum_np = 0, sumpct = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (col_is_pct(&cs[j])) {
                float s = fmaxf(cs[j].min, cs[j].pct * etot);
                cs[j].min = s;
                sum_s += s;
                sumpct += cs[j].pct;
            } else {
                sum_np += cs[j].min;
                embi_vec_push(&idx, j);
            }
        }
        float rem = etot - sum_s - sum_np;
        if (rem > 0) {
            if (idx.len > 0) {
                span_fill_min(cs, idx.data, idx.len, rem);
            } else {
                for (uint32_t j = 0; j < n; j++) {
                    cs[j].min += rem * cs[j].pct / sumpct;
                }
            }
        }
    } else {
        float have = 0;
        for (uint32_t j = 0; j < n; j++) {
            embi_vec_push(&idx, j);
            have += cs[j].min;
        }
        span_fill_min(cs, idx.data, idx.len, etot - have);
    }
    embi_vec_free(&idx);
    for (uint32_t j = 0; j < n; j++) {
        if (cs[j].max < cs[j].min) {
            cs[j].max = cs[j].min;
        }
    }
    /* The maximum: a percentage column takes what the others leave; otherwise only unconstrained
     * columns grow, or every column when the cell itself has a fixed width. */
    float emax = t->cmax - gap;
    float np_max = 0, pc_max = 0, all_max = 0, psum = 0;
    for (uint32_t j = 0; j < n; j++) {
        all_max += cs[j].max;
        if (col_is_pct(&cs[j])) {
            pc_max += cs[j].max;
            psum += cs[j].pct;
        } else {
            np_max += cs[j].max;
        }
    }
    if (psum > 0) {
        float extra = emax - np_max - pc_max;
        if (extra > 0) {
            for (uint32_t j = 0; j < n; j++) {
                if (col_is_pct(&cs[j])) {
                    cs[j].max += extra * cs[j].pct / psum;
                }
            }
        }
        return;
    }
    float e = emax - all_max;
    if (e <= 0) {
        return;
    }
    bool cell_cons = !isnan(t->spec);
    bool unc = false;
    for (uint32_t j = 0; j < n; j++) {
        unc = unc || !cs[j].fixedc;
    }
    if (!unc && !cell_cons) {
        return;
    }
    float wsum = 0;
    uint32_t cnt = 0;
    for (uint32_t j = 0; j < n; j++) {
        if (!unc || !cs[j].fixedc) {
            wsum += cs[j].max;
            cnt++;
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        if (!unc || !cs[j].fixedc) {
            cs[j].max += wsum > 0 ? e * cs[j].max / wsum : e / (float)cnt;
        }
    }
}

/* The column contributions of the cells (CSS Tables 3 section 3.5, auto layout), or the first row's
 * widths and percentages (section 3.4, fixed layout). */
static void contributions(tmodel *m)
{
    if (m->fixed) {
        for (uint32_t i = 0; i < m->cells.len; i++) {
            const tcell *t = &m->cells.data[i];
            if (t->row != 0) {
                continue;
            }
            tcol *col = &m->cols.data[t->col];
            if (t->cspan == 1 && t->ospan == 1) {
                if (isnan(col->spec) && !col_is_pct(col) && !isnan(t->spec)) {
                    col->spec = t->spec;
                    col->fixedc = 1;
                }
                if (isnan(col->pct) && !isnan(t->pct)) {
                    col->pct = t->pct;
                    col->pe = t->bb ? 0.0f : t->eh;
                }
            } else if (!isnan(t->spec)) {
                /* A px width over several columns gives each column without a width an equal share. */
                float share = fmaxf(0.0f, t->spec - m->sph * (float)(t->cspan - 1)) / (float)t->cspan;
                for (uint32_t j = t->col; j < t->col + t->cspan; j++) {
                    tcol *q = &m->cols.data[j];
                    if (isnan(q->spec) && !col_is_pct(q)) {
                        q->spec = share;
                        q->fixedc = 1;
                    }
                }
            } else if (!isnan(t->pct) && t->pct > 0) {
                /* The percentage goes to the spanned auto columns, an equal share each. */
                float sumpct = 0;
                for (uint32_t j = t->col; j < t->col + t->cspan; j++) {
                    if (col_is_pct(&m->cols.data[j])) {
                        sumpct += m->cols.data[j].pct;
                    }
                }
                if (t->pct > sumpct) {
                    for (uint32_t j = t->col; j < t->col + t->cspan; j++) {
                        tcol *q = &m->cols.data[j];
                        if (!col_is_pct(q) && isnan(q->spec)) {
                            q->pct = t->pct / (float)t->cspan;
                        }
                    }
                }
            }
        }
        return;
    }
    for (uint32_t j = 0; j < m->cols.len; j++) {
        tcol *col = &m->cols.data[j];
        if (!isnan(col->spec)) {
            col->max = col->spec;
        }
    }
    uint32_t maxspan = 1;
    for (uint32_t i = 0; i < m->cells.len; i++) {
        const tcell *t = &m->cells.data[i];
        if (t->cspan == 1) {
            col_merge(&m->cols.data[t->col], t->cmin, t->cmax, !isnan(t->spec) && t->ospan == 1, t->pct);
        } else if (t->cspan > maxspan) {
            maxspan = t->cspan;
        }
    }
    for (uint32_t sp = 2; sp <= maxspan; sp++) {
        for (uint32_t i = 0; i < m->cells.len; i++) {
            if (m->cells.data[i].cspan == sp) {
                span_cell(m, &m->cells.data[i]);
            }
        }
    }
}

/* A column with visibility: collapse has no width and no contribution. */
static void drop_collapsed_cols(tmodel *m)
{
    if (!m->fixed) {
        /* Percentages beyond 100% are truncated, left to right (WPT tentative/table-width-redistribution.html). */
        float sum = 0;
        for (uint32_t j = 0; j < m->cols.len; j++) {
            tcol *col = &m->cols.data[j];
            if (col->vcol || !col_is_pct(col)) {
                continue;
            }
            if (sum + col->pct > 1.0f) {
                col->pct = 1.0f - sum;
                if (col->pct <= 0.0001f) {
                    col->pct = NAN;
                }
            }
            sum += isnan(col->pct) ? 0.0f : col->pct;
        }
    }
    for (uint32_t j = 0; j < m->cols.len; j++) {
        tcol *col = &m->cols.data[j];
        if (col->vcol) {
            col->min = 0;
            col->max = 0;
            col->spec = NAN;
            col->pct = NAN;
            col->fixedc = 0;
        }
    }
}

static void model_structure(embi_lctx *c, embi_box *b, tmodel *m);

/* Builds the model of table b: the captions, the columns, the rows and cells, and the contributions. */
static void model_build(embi_lctx *c, embi_box *b, tmodel *m)
{
    memset(m, 0, sizeof *m);
    m->table = b;
    m->cq_w = c->ncq > 0 ? c->cq[c->ncq - 1].w : NAN;
    m->cq_h = c->ncq > 0 ? c->cq[c->ncq - 1].h : NAN;
    m->nested = embi_table_in_cell(b);
    m->collapse = collapse_of(c, b);
    m->rtl = table_rtl(c, b);
    const embi_value *lv = embi_lay_prop(c, b, EMBI_PROP_table_layout);
    /* table-layout: fixed only applies to a table whose width is not auto (CSS Tables 3 section 3.4;
     * WPT tentative/table-width-redistribution-fixed.html): min-content, max-content and lengths count. */
    const embi_value *tw = embi_lay_prop(c, b, EMBI_PROP_width);
    m->fixed = lv != nullptr && lv->kind == EMBI_V_KEYWORD && lv->kw == EMBI_KW_fixed && tw != nullptr &&
               !embi_lay_is_auto(tw) && !(tw->kind == EMBI_V_KEYWORD && tw->kw == EMBI_KW_stretch);
    if (!m->collapse) {
        read_spacing(c, b, &m->sph, &m->spv);
    }
    model_structure(c, b, m);
    if (m->cols.len == 0) {
        /* A table without columns has no horizontal spacing; the vertical spacing only shows when the
         * table has a definite height (embi_table_content). */
        m->sph = 0;
    }
    if (m->collapse) {
        build_collapse(c, m);
    }
    cell_props(c, m);
    contributions(m);
    drop_collapsed_cols(m);
}

/* The model of a table is built once per layout (W3h): the intrinsic queries, the border queries and the
 * content layout of a table all start from the same cached master, which is read-only; the content layout
 * works on a copy, since it sets the widths and heights. */
static void model_vec_copy(void *dst_v, const void *src_v, size_t esz)
{
    EMBI_VEC(char) *dst = dst_v;
    const EMBI_VEC(char) *src = src_v;
    dst->data = nullptr;
    dst->len = dst->cap = 0;
    if (src->len == 0) {
        return;
    }
    char *p = embi_realloc(nullptr, 0, (size_t)src->len * esz);
    if (p == nullptr) {
        return;
    }
    memcpy(p, src->data, (size_t)src->len * esz);
    dst->data = p;
    dst->len = dst->cap = src->len;
}

void embi_table_models_free(embi_lctx *c)
{
    for (uint32_t i = 0; i < c->tmods.len; i++) {
        tmodel *m = c->tmods.data[i];
        model_free(m);
        embi_free(m, sizeof *m);
    }
    embi_vec_free(&c->tmods);
    embi_hashmap_free(&c->tmodels);
}

/* The master model of table b for this layout, or nullptr when it cannot be allocated. */
static const tmodel *model_master(embi_lctx *c, embi_box *b)
{
    uint64_t v = 0;
    float qw = c->ncq > 0 ? c->cq[c->ncq - 1].w : NAN;
    float qh = c->ncq > 0 ? c->cq[c->ncq - 1].h : NAN;
    if (embi_hashmap_get(&c->tmodels, (uint64_t)(uintptr_t)b, &v)) {
        const tmodel *cached = (const tmodel *)(uintptr_t)v;
        /* The cell contributions depend on the container size of the query (cq units): a master built
         * under another one is rebuilt (the old one stays in tmods until the layout ends). */
        if ((cached->cq_w == qw || (isnan(cached->cq_w) && isnan(qw))) &&
            (cached->cq_h == qh || (isnan(cached->cq_h) && isnan(qh)))) {
            return cached;
        }
    }
    tmodel *m = embi_realloc(nullptr, 0, sizeof *m);
    if (m == nullptr) {
        return nullptr;
    }
    model_build(c, b, m);
    /* Building the model may have laid out table cells (a nested table builds its own and caches it), so
     * the map is looked up again by put, not assumed unchanged. */
    embi_vec_push(&c->tmods, m);
    if (!embi_hashmap_put(&c->tmodels, (uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)m)) {
        return m; /* not cached: it is freed with the others, and rebuilt on the next query */
    }
    return m;
}

/* A private copy of the model of b (to lay the table out with), built on the spot when the cache fails. */
static void model_copy(embi_lctx *c, embi_box *b, tmodel *out)
{
    const tmodel *src = model_master(c, b);
    if (src == nullptr) {
        model_build(c, b, out);
        return;
    }
    *out = *src;
    model_vec_copy(&out->cells, &src->cells, sizeof(tcell));
    model_vec_copy(&out->cols, &src->cols, sizeof(tcol));
    model_vec_copy(&out->rows, &src->rows, sizeof(trow));
    model_vec_copy(&out->groups, &src->groups, sizeof(tgroup));
    model_vec_copy(&out->caps, &src->caps, sizeof(tcap));
    model_vec_copy(&out->hb, &src->hb, sizeof(tbord));
    model_vec_copy(&out->vb, &src->vb, sizeof(tbord));
    model_vec_copy(&out->grid, &src->grid, sizeof(int32_t));
}

/* The structure of table b: the captions, the columns, the rows and the placed cells. */
static void model_structure(embi_lctx *c, embi_box *b, tmodel *m)
{
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->tr == EMBI_TR_CAPTION) {
            tcap cp;
            memset(&cp, 0, sizeof cp);
            cp.box = k;
            cp.bottom = caption_bottom(c, k);
            embi_vec_push(&m->caps, cp);
        } else if (k->tr == EMBI_TR_COL) {
            add_cols(c, m, k, nullptr, NAN, NAN);
        } else if (k->tr == EMBI_TR_COLGROUP) {
            float gw = embi_table_col_width(c, k);
            float gp = embi_table_col_pct(c, k);
            bool has = false;
            for (embi_box *q = k->first; q != nullptr; q = q->next) {
                if (q->tr == EMBI_TR_COL) {
                    has = true;
                    add_cols(c, m, q, k, gw, gp);
                }
            }
            if (!has) {
                add_cols(c, m, k, k, NAN, NAN);
            }
        }
    }
    /* The rows: header groups, then the bodies and the rows directly in the table, then the footers. */
    /* Only the first header group and the first footer group are the header and the footer; the others are
     * row groups in document order (CSS 2.1 section 17.2.1; WPT css-tables/row-group-order.html). */
    const embi_box *first_head = nullptr, *first_foot = nullptr;
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->tr == EMBI_TR_THEAD && first_head == nullptr) {
            first_head = k;
        } else if (k->tr == EMBI_TR_TFOOT && first_foot == nullptr) {
            first_foot = k;
        }
    }
    for (int pass = 0; pass < 3; pass++) {
        for (embi_box *k = b->first; k != nullptr; k = k->next) {
            uint8_t tr = k->tr;
            if (pass == 0 && k == first_head) {
                add_group(c, m, k);
            } else if (pass == 2 && k == first_foot) {
                add_group(c, m, k);
            } else if (pass == 1 && (tr == EMBI_TR_ROWGROUP || (tr == EMBI_TR_THEAD && k != first_head) ||
                                     (tr == EMBI_TR_TFOOT && k != first_foot))) {
                add_group(c, m, k);
            } else if (pass == 1 && tr == EMBI_TR_ROW) {
                add_row(c, m, k, UINT32_MAX, nullptr);
            }
        }
    }
    place_cells(c, m);
    if (!m->fixed) {
        merge_empty_cols(m); /* a fixed table keeps its columns: Chrome, Safari and the WPT fixed tests do not merge there */
    }
}

/* The half borders the table itself takes from the winners of its outer edges (the same winners that
 * build_collapse gives the grid: cells, rows, row groups, columns, column groups and the table). */
bool embi_table_collapse_edges(const embi_lctx *c, const embi_box *b, float h[4])
{
    if (!collapse_of(c, b)) {
        return false;
    }
    h[0] = h[1] = h[2] = h[3] = 0;
    embi_lctx *lc = (embi_lctx *)c;
    const tmodel *mp = model_master(lc, (embi_box *)b);
    tmodel local;
    if (mp == nullptr) {
        model_build(lc, (embi_box *)b, &local);
        mp = &local;
    }
    const tmodel m = *mp;
    /* A table without cells has no grid to border: it has no outer borders (Chromium). */
    if (m.R > 0 && m.C > 0) {
        float top = 0, bot = 0, left = 0, right = 0;
        size_t C = m.C;
        size_t le = m.rtl ? C : 0, re = m.rtl ? 0 : C; /* the physical left and right edge indices */
        for (size_t j = 0; j < C; j++) {
            top = fmaxf(top, m.hb.data[j].w);
            bot = fmaxf(bot, m.hb.data[(size_t)m.R * C + j].w);
        }
        for (size_t i = 0; i < m.R; i++) {
            left = fmaxf(left, m.vb.data[i * (C + 1) + le].w);
            right = fmaxf(right, m.vb.data[i * (C + 1) + re].w);
        }
        /* The table has the half of each outer edge that the cells at it do not take: the larger half at its top and its
         * inline start, the smaller at its bottom and its inline end. */
        h[0] = end_half(top);
        h[1] = m.rtl ? end_half(right) : start_half(right);
        h[2] = start_half(bot);
        h[3] = m.rtl ? start_half(left) : end_half(left);
    }
    if (mp == &local) {
        model_free(&local);
    }
    return true;
}

/* The used widths of the columns for the content width W (CSS Tables 3 sections 3.4 and 3.5). The
 * border-spacing takes its share first. Collapsed columns get 0 and take no share. */
static void column_widths(tmodel *m, float W)
{
    uint32_t n = m->cols.len;
    if (n == 0) {
        return;
    }
    float gap = m->sph * (float)(n + 1);
    float avail = W - gap;
    EMBI_VEC(float) mn = {nullptr, 0, 0};
    EMBI_VEC(float) mx = {nullptr, 0, 0};
    EMBI_VEC(float) pc = {nullptr, 0, 0};
    EMBI_VEC(float) sp = {nullptr, 0, 0};
    EMBI_VEC(float) pe = {nullptr, 0, 0};
    EMBI_VEC(float) out = {nullptr, 0, 0};
    EMBI_VEC(uint8_t) fx = {nullptr, 0, 0};
    EMBI_VEC(uint32_t) idx = {nullptr, 0, 0};
    for (uint32_t j = 0; j < n; j++) {
        const tcol *col = &m->cols.data[j];
        if (col->vcol) {
            continue;
        }
        embi_vec_push(&mn, col->min);
        embi_vec_push(&mx, col->max);
        embi_vec_push(&pc, col->pct);
        embi_vec_push(&sp, col->spec);
        embi_vec_push(&pe, col->pe);
        embi_vec_push(&out, 0.0f);
        embi_vec_push(&fx, col->fixedc);
        embi_vec_push(&idx, j);
    }
    uint32_t k = idx.len;
    if (k > 0) {
        if (m->fixed) {
            embi_table_distribute_fixed(k, sp.data, pc.data, pe.data, avail, out.data);
        } else {
            embi_table_distribute_auto(k, mn.data, mx.data, pc.data, fx.data, avail, out.data);
        }
    }
    float x = m->sph;
    uint32_t q = 0;
    for (uint32_t j = 0; j < n; j++) {
        tcol *col = &m->cols.data[j];
        col->w = 0;
        if (!col->vcol) {
            col->w = out.data[q] < 0 ? 0.0f : out.data[q];
            q++;
        }
        col->x = x;
        if (!col->vcol) {
            x += col->w + m->sph;
        }
    }
    embi_vec_free(&mn);
    embi_vec_free(&mx);
    embi_vec_free(&pc);
    embi_vec_free(&sp);
    embi_vec_free(&pe);
    embi_vec_free(&out);
    embi_vec_free(&fx);
    embi_vec_free(&idx);
}

/* The border-box width of a cell over its columns, with the spacing between them. */
static void cell_widths(tmodel *m)
{
    for (uint32_t i = 0; i < m->cells.len; i++) {
        tcell *t = &m->cells.data[i];
        float w = 0;
        for (uint32_t j = t->col; j < t->col + t->cspan; j++) {
            w += m->cols.data[j].w;
        }
        t->sw = w + m->sph * (float)(t->cspan - 1);
    }
}

/* The collapsed half borders a cell is laid out with, or nullptr when the table is separate. */
static const float *cell_bd(const tmodel *m, const tcell *t)
{
    return m->collapse ? t->bd : nullptr;
}

/* The baseline of a laid-out cell from its border-box top: its first line's baseline, or, for a cell
 * without line boxes, the bottom of its in-flow content (the last child's bottom edge). */
static float cell_baseline(const embi_frag *cf)
{
    if (!isnan(cf->bl_first)) {
        return cf->bl_first;
    }
    /* A cell whose first content is a table (an anonymous one around a row or table inside a row) takes that table's first
     * row baseline (CSS Tables 3 section 3.3: the baseline of a table is its first row's; table-anonymous-objects-089). */
    const embi_frag *fk = cf->first;
    if (fk != nullptr && fk->kind == EMBI_FK_BOX && fk->box != nullptr && embi_lay_is_table(fk->box) &&
        !isnan(fk->bl_first)) {
        return fk->y + fk->bl_first;
    }
    float b = embi_frag_bdr(cf, 1) + embi_frag_pad(cf, 1);
    for (const embi_frag *k = cf->first; k != nullptr; k = k->next) {
        if (k->y + k->h > b) {
            b = k->y + k->h;
        }
    }
    return b;
}

/* Lays out a cell (a block container) at the border-box width sw, its content width cw, and the
 * border-box height hset (NaN for its natural height). bd, when set, are its collapsed half borders
 * (top, right, bottom, left). The fragment is not placed. The forced sizes of flex items (lctx force_*)
 * are saved and restored around the call. */
static embi_frag *lay_cell(embi_lctx *c, embi_box *cell, float cw, float sw, float hset,
                           const float *bd, bool soft_h, embi_lres *out)
{
    embi_box *sbx = c->force_box;
    float sw0 = c->force_w, sh0 = c->force_h;
    bool sww = c->force_wset, shs = c->force_hset, shn = c->force_hnone, shf = c->force_hsoft;
    const embi_box *eb0 = c->edge_box;
    float ebd0[4];
    memcpy(ebd0, c->edge_bdr, sizeof ebd0);
    embi_lay_clear_frags(c, cell);
    c->force_box = cell;
    c->force_w = sw;
    c->force_wset = true;
    c->force_hset = !isnan(hset);
    c->force_h = isnan(hset) ? sh0 : hset;
    c->force_hnone = isnan(hset);
    c->force_hsoft = soft_h;
    if (bd != nullptr) {
        c->edge_box = cell;
        memcpy(c->edge_bdr, bd, sizeof ebd0);
    }
    embi_cs ccs;
    memset(&ccs, 0, sizeof ccs);
    ccs.avail_w = sw;
    ccs.cb_w = cw;
    ccs.cb_h = NAN;
    ccs.fit = EMBI_FIT_FILL;
    memset(out, 0, sizeof *out);
    embi_lay_box(c, cell, &ccs, out);
    c->force_box = sbx;
    c->force_w = sw0;
    c->force_h = sh0;
    c->force_wset = sww;
    c->force_hset = shs;
    c->force_hnone = shn;
    c->force_hsoft = shf;
    c->edge_box = eb0;
    memcpy(c->edge_bdr, ebd0, sizeof ebd0);
    return out->frag;
}

/* Moves the content of a cell down by dy (vertical-align) and recomputes its overflow. */
static void shift_content(embi_frag *cf, float dy)
{
    if (dy == 0) {
        return;
    }
    for (embi_frag *k = cf->first; k != nullptr; k = k->next) {
        k->y += dy;
    }
    if (!isnan(cf->bl_first)) {
        cf->bl_first += dy;
    }
    if (!isnan(embi_fl(cf)->bl_last)) {
        embi_fl(cf)->bl_last += dy;
    }
    embi_fo(cf)->ov[0] = embi_frag_bdr(cf, 0);
    embi_fo(cf)->ov[1] = embi_frag_bdr(cf, 1);
    embi_fo(cf)->ov[2] = cf->w - embi_frag_bdr(cf, 2);
    embi_fo(cf)->ov[3] = cf->h - embi_frag_bdr(cf, 3);
    for (embi_frag *k = cf->first; k != nullptr; k = k->next) {
        embi_lay_ov_add_child(cf, k);
    }
}

/* Lays out the captions at the content width W: their fragments and margins. */
static void lay_captions(embi_lctx *c, tmodel *m, float W, float *top_h, float *bot_h)
{
    *top_h = 0;
    *bot_h = 0;
    for (uint32_t i = 0; i < m->caps.len; i++) {
        tcap *cp = &m->caps.data[i];
        embi_lay_clear_frags(c, cp->box);
        embi_cs ccs;
        memset(&ccs, 0, sizeof ccs);
        ccs.avail_w = W;
        ccs.cb_w = W;
        ccs.cb_h = NAN;
        ccs.fit = EMBI_FIT_FILL;
        embi_lres r;
        memset(&r, 0, sizeof r);
        embi_lay_box(c, cp->box, &ccs, &r);
        cp->frag = r.frag;
        if (r.frag != nullptr) {
            cp->ml = embi_frag_mg(r.frag, 0);
            cp->mt = embi_frag_mg(r.frag, 1);
            cp->mb = embi_frag_mg(r.frag, 3);
            cp->h = r.frag->h;
        } else {
            cp->ml = cp->mt = cp->mb = cp->h = 0;
        }
        float tot = cp->mt + cp->h + cp->mb;
        if (cp->bottom) {
            *bot_h += tot;
        } else {
            *top_h += tot;
        }
    }
}

/* A table whose height is restricted as specified (Chromium's restricted block size table: a height other than auto,
 * a percentage that behaves as auto included, or a max-height other than none; percentage-sizing-of-table-cell-007). */
static bool restricted_height(const embi_lctx *c, const embi_box *table)
{
    const embi_value *h = embi_lay_prop(c, table, EMBI_PROP_height);
    if (h != nullptr && !embi_lay_is_auto(h) && h->kind != EMBI_V_KEYWORD) {
        return true;
    }
    const embi_value *mh = embi_lay_prop(c, table, EMBI_PROP_max_height);
    return mh != nullptr && !(mh->kind == EMBI_V_KEYWORD && mh->kw == EMBI_KW_none);
}

static bool cell_is_pct_scroll_only(const embi_lctx *c, const embi_box *cell);

/* In a restricted table (restricted_height), the percentage heights of a cell whose children are all scroll containers
 * resolve against the row height in the final layout (percentage-sizing-of-table-cell-007); the percentages of other
 * children of a cell without a height stay unresolved (percent-height-table-cell-child). */
static bool pct_scroll_resolves(const embi_lctx *c, const tmodel *m, const tcell *t)
{
    return m->restricted && cell_is_pct_scroll_only(c, t->box);
}

/* An anonymous cell whose children are all block scroll containers with a percentage height, in a
 * table with a definite height (WPT percent-height-overflow-auto-in-restricted-block-size-cell.html,
 * CSS Tables 3 "row layout"): the percentage cannot resolve while the cell is measured, and the content
 * of a scroll container must not size the row, so the cell asks for no height of its own and the scroll
 * boxes take the row's height in the final layout. */
static bool cell_is_pct_scroll_only(const embi_lctx *c, const embi_box *cell)
{
    if (cell->first == nullptr) {
        return false;
    }
    for (const embi_box *k = cell->first; k != nullptr; k = k->next) {
        if (k->kind != EMBI_BK_BLOCK || (k->flags & EMBI_BF_SCROLL) == 0) {
            return false;
        }
        const embi_value *hv = embi_lay_prop(c, k, EMBI_PROP_height);
        if (hv == nullptr || hv->kind != EMBI_V_PERCENT) {
            return false;
        }
    }
    return true;
}

/* True when the layout of the content of a cell can depend on the height the cell is given: a descendant
 * with a percentage (or calc) height, min-height or max-height, or an out-of-flow descendant (its
 * insets resolve against the height of its containing block). size_rows lays a cell out at its natural
 * height; place_cells_final lays it out at the row's height, so the two trees agree only for content
 * where this is false (W3h: the measured layout is reused only then). */
static bool cell_height_dependent(const embi_lctx *c, const embi_box *cell)
{
    static const embi_prop_id props[3] = {EMBI_PROP_height, EMBI_PROP_min_height, EMBI_PROP_max_height};
    const embi_box *b = cell->first;
    while (b != nullptr) {
        if ((b->flags & EMBI_BF_ABSPOS) != 0) {
            return true;
        }
        if (b->kind != EMBI_BK_TEXT && b->kind != EMBI_BK_BR) {
            for (int i = 0; i < 3; i++) {
                const embi_value *v = embi_lay_prop(c, b, props[i]);
                if (v != nullptr && (v->kind == EMBI_V_PERCENT || v->kind == EMBI_V_CALC)) {
                    return true;
                }
            }
        }
        if (!embi_box_is_text(b) && b->first != nullptr) {
            b = b->first;
            continue;
        }
        while (b != cell && b->next == nullptr) {
            b = b->parent;
        }
        if (b == cell) {
            break;
        }
        b = b->next;
    }
    return false;
}

/* Lays out every cell at its column width, measures it, and derives the row heights (CSS Tables 3
 * section 3.6): single-row cells first, then cells that span rows share their extra height among
 * the rows they cover. */
static void size_rows(embi_lctx *c, tmodel *m, float W)
{
    for (uint32_t i = 0; i < m->cells.len; i++) {
        tcell *t = &m->cells.data[i];
        embi_lres r;
        embi_frag *f = lay_cell(c, t->box, W, t->sw, NAN, cell_bd(m, t), false, &r);
        t->mf = f;
        t->mw = W;
        if (f != nullptr && t->rspan == 1 && cell_height_dependent(c, t->box)) {
            t->mf = nullptr; /* the final layout at the row height can differ: lay it out again */
        }
        if (f != nullptr) {
            t->mh = f->h;
            t->bl = cell_baseline(f);
            t->nat = f->h - t->ev;
        } else {
            t->mh = t->ev;
            t->nat = 0;
            t->bl = NAN;
        }
        if (t->nat < 0) {
            t->nat = 0;
        }
        if (f != nullptr && (m->restricted || (t->box->flags & EMBI_BF_ANON) == 0) && cell_is_pct_scroll_only(c, t->box)) {
            t->nat = 0;
            t->mh = t->ev;
            t->mf = nullptr; /* the final layout lays it out again at the row height */
        }
    }
    for (uint32_t r = 0; r < m->rows.len; r++) {
        trow *row = &m->rows.data[r];
        row->h = isnan(row->hspec) ? 0.0f : row->hspec;
        row->bl_base = NAN;
        row->bl_any = NAN;
        row->bl_fin = NAN;
    }
    for (uint32_t i = 0; i < m->cells.len; i++) {
        const tcell *t = &m->cells.data[i];
        if (t->rspan != 1 || isnan(t->bl)) {
            continue;
        }
        trow *row = &m->rows.data[t->row];
        if (isnan(row->bl_any) || t->bl > row->bl_any) {
            row->bl_any = t->bl;
        }
        if (t->valign == VA_BASELINE && (isnan(row->bl_base) || t->bl > row->bl_base)) {
            row->bl_base = t->bl;
        }
    }
    for (uint32_t i = 0; i < m->cells.len; i++) {
        const tcell *t = &m->cells.data[i];
        if (t->rspan != 1) {
            continue;
        }
        trow *row = &m->rows.data[t->row];
        float h = cell_need(t);
        float ext = h;
        if (t->valign == VA_BASELINE && !isnan(t->bl) && !isnan(row->bl_base)) {
            /* The cell's own height property is a minimum of the row, it does not extend below the
             * baseline (WPT tentative/baseline-td.html). */
            float natural = t->mh;
            ext = (row->bl_base - t->bl) + natural;
            if (!isnan(t->hspec) && t->hspec > ext) {
                ext = t->hspec;
            }
        }
        if (ext > row->h) {
            row->h = ext;
        }
    }
    uint32_t maxrs = 1;
    for (uint32_t i = 0; i < m->cells.len; i++) {
        if (m->cells.data[i].rspan > maxrs) {
            maxrs = m->cells.data[i].rspan;
        }
    }
    for (uint32_t rs = 2; rs <= maxrs; rs++) {
        for (uint32_t i = 0; i < m->cells.len; i++) {
            const tcell *t = &m->cells.data[i];
            if (t->rspan != rs) {
                continue;
            }
            /* A cell that spans rows asks its rows for its content height with its border and padding, but
             * its height property counts as a border-box height whatever its box-sizing (Chromium; WPT
             * tentative/baseline-table.html). */
            float need = t->mh;
            if (!isnan(t->hspec)) {
                float hs = t->bb ? t->hspec : t->hspec - t->ev;
                if (hs > need) {
                    need = hs;
                }
            }
            float have = m->spv * (float)(rs - 1);
            for (uint32_t r = t->row; r < t->row + rs; r++) {
                have += m->rows.data[r].h;
            }
            if (need <= have) {
                continue;
            }
            float extra = need - have;
            /* The extra goes to the unconstrained rows (no height; a percentage that cannot resolve counts)
             * in proportion to their heights, all to the last one when they are all empty; with no such row
             * to the others in proportion (WPT tentative/rowspan-height-redistribution.html). */
            bool any_u = false;
            for (uint32_t r = t->row; r < t->row + rs; r++) {
                const trow *row = &m->rows.data[r];
                any_u = any_u || (!row->vcol && isnan(row->hspec));
            }
            float sum = 0;
            uint32_t n = 0, last = UINT32_MAX;
            for (uint32_t r = t->row; r < t->row + rs; r++) {
                const trow *row = &m->rows.data[r];
                if (row->vcol || (any_u && !isnan(row->hspec))) {
                    continue;
                }
                sum += row->h;
                n++;
                last = r;
            }
            for (uint32_t r = t->row; r < t->row + rs && n > 0; r++) {
                trow *row = &m->rows.data[r];
                if (row->vcol || (any_u && !isnan(row->hspec))) {
                    continue;
                }
                if (sum > 0) {
                    row->h += extra * row->h / sum;
                } else if (any_u) {
                    row->h += r == last ? extra : 0.0f;
                } else {
                    row->h += extra / (float)n;
                }
            }
        }
    }
    /* A collapsed row has no height. */
    for (uint32_t r = 0; r < m->rows.len; r++) {
        if (m->rows.data[r].vcol) {
            m->rows.data[r].h = 0;
        }
    }
    /* A row group taller than its rows (its height property) gives the extra to its unconstrained rows (no
     * height property), in proportion to their heights, equally when they have none; with no such row,
     * to all of its rows (CSS Tables 3 section 3.6, Chromium; WPT tentative/tbody-height-redistribution.html). */
    for (uint32_t g = 0; g < m->groups.len; g++) {
        const tgroup *gr = &m->groups.data[g];
        if (isnan(gr->hspec) || gr->first >= gr->end) {
            continue;
        }
        float have = m->spv * (float)(gr->end - gr->first - 1);
        float sum_u = 0, sum_a = 0;
        uint32_t nu = 0, na = 0;
        for (uint32_t r = gr->first; r < gr->end; r++) {
            const trow *row = &m->rows.data[r];
            have += row->h;
            if (row->vcol) {
                continue;
            }
            na++;
            sum_a += row->h;
            if (isnan(row->hspec) && isnan(row->hpct)) {
                nu++;
                sum_u += row->h;
            }
        }
        if (gr->hspec <= have) {
            continue;
        }
        float extra = gr->hspec - have;
        bool only_u = nu > 0;
        float sum = only_u ? sum_u : sum_a;
        uint32_t n = only_u ? nu : na;
        for (uint32_t r = gr->first; r < gr->end && n > 0; r++) {
            trow *row = &m->rows.data[r];
            if (row->vcol || (only_u && !(isnan(row->hspec) && isnan(row->hpct)))) {
                continue;
            }
            row->h += sum > 0 ? extra * row->h / sum : extra / (float)n;
        }
    }
}

/* Positions the rows from the top of the grid (gbase in content coordinates), and the groups. A group
 * without rows takes its height property as its height. m->grid_h is the height of the grid (the rows,
 * the empty groups and the spacing around them). */
static void place_rows(tmodel *m, float gbase)
{
    float rel = m->spv;
    uint32_t eg = 0; /* the next empty group to place */
    for (uint32_t r = 0; r <= m->rows.len; r++) {
        for (; eg < m->groups.len; eg++) {
            tgroup *gr = &m->groups.data[eg];
            if (gr->first < gr->end) {
                continue;
            }
            if (gr->first > r) {
                break;
            }
            gr->top = gbase + rel;
            gr->h = isnan(gr->hspec) ? 0.0f : gr->hspec;
            if (gr->h > 0) {
                rel += gr->h + m->spv;
            }
        }
        if (r == m->rows.len) {
            break;
        }
        trow *row = &m->rows.data[r];
        row->top = gbase + rel;
        if (!row->vcol) {
            rel += row->h + m->spv; /* a collapsed row has no height and no spacing (CSS Tables 3 section 3.9.1) */
        }
    }
    for (uint32_t g = 0; g < m->groups.len; g++) {
        tgroup *gr = &m->groups.data[g];
        if (gr->first < gr->end) {
            gr->top = m->rows.data[gr->first].top;
            const trow *last = &m->rows.data[gr->end - 1];
            gr->h = last->top + last->h - gr->top;
        }
    }
    m->grid_h = (m->rows.len == 0 && rel == m->spv) ? 0.0f : rel;
}

/* The height of the grid (the rows and the spacing around them). */
static float grid_height(const tmodel *m, float gbase)
{
    (void)gbase;
    return m->grid_h;
}

/* The out-of-flow children of a row or row group (position: absolute with a table display value is blockified, but it
 * stays a child of the row): their static position is the origin of the row's fragment. */
static void row_abspos(embi_lctx *c, embi_box *rb)
{
    for (embi_box *k = rb->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) != 0) {
            embi_lay_abspos_at(c, k, rb, 0.0f, 0.0f, nullptr);
        }
    }
}

/* The rows and groups get their fragments in the table fragment f; the cells are laid out later. */
static void make_row_frags(embi_lctx *c, tmodel *m, embi_frag *f, float cw, float ox, float oy)
{
    for (uint32_t r = 0; r < m->rows.len; r++) {
        trow *row = &m->rows.data[r];
        bool first = row->group != UINT32_MAX && (r == 0 || m->rows.data[r - 1].group != row->group);
        if (first) {
            tgroup *gr = &m->groups.data[row->group];
            gr->frag = embi_lay_new_frag(c, gr->box, EMBI_FK_BOX);
            embi_lay_frag_set_size(gr->frag, cw, gr->h);
            gr->frag->x = ox;
            gr->frag->y = oy + gr->top;
            embi_lay_frag_append(f, gr->frag);
            row_abspos(c, gr->box);
        }
        row->frag = embi_lay_new_frag(c, row->box, EMBI_FK_BOX);
        embi_lay_frag_set_size(row->frag, cw, row->h);
        row_abspos(c, row->box);
        if (row->group != UINT32_MAX) {
            const tgroup *gr = &m->groups.data[row->group];
            row->frag->x = 0;
            row->frag->y = row->top - gr->top;
            embi_lay_frag_append(gr->frag, row->frag);
        } else {
            row->frag->x = ox;
            row->frag->y = oy + row->top;
            embi_lay_frag_append(f, row->frag);
        }
    }
    for (uint32_t g = 0; g < m->groups.len; g++) {
        tgroup *gr = &m->groups.data[g];
        if (gr->frag == nullptr) {
            gr->frag = embi_lay_new_frag(c, gr->box, EMBI_FK_BOX);
            embi_lay_frag_set_size(gr->frag, cw, gr->h);
            gr->frag->x = ox;
            gr->frag->y = oy + gr->top;
            embi_lay_frag_append(f, gr->frag);
        }
    }
}

/* True when a spanning cell covers a collapsed column: the cell keeps the width of the columns that remain and its
 * content is clipped to it (CSS Tables 3 section 3.9.1, Chromium; visibility-collapse-colspan-003). */
static bool spans_collapsed(const tmodel *m, const tcell *t)
{
    for (uint32_t j = t->col; t->cspan > 1 && j < t->col + t->cspan && j < m->cols.len; j++) {
        if (m->cols.data[j].vcol) {
            return true;
        }
    }
    return false;
}

/* Lays out the cells at their final sizes (the spanned rows' height), aligns their content, and places
 * their fragments in their rows. */
static void place_cells_final(embi_lctx *c, tmodel *m, float W, float gx0)
{
    /* Pass 1: every cell at its final size. The baselines are read from this layout, since percentage
     * heights of the cell's content resolve now (WPT tentative/baseline-td.html). */
    EMBI_VEC(embi_frag *) frags = {nullptr, 0, 0};
    for (uint32_t i = 0; i < m->cells.len; i++) {
        tcell *t = &m->cells.data[i];
        float span_h = m->spv * (float)(t->rspan - 1);
        for (uint32_t r = t->row; r < t->row + t->rspan; r++) {
            span_h += m->rows.data[r].h;
        }
        /* A one-row cell whose row is exactly its measured height gets the measured layout back (W3h):
         * laying it out again at the same height gives the same tree, and nested tables would otherwise
         * lay out every level twice (2^depth). */
        embi_frag *cf = nullptr;
        if (t->rspan == 1 && t->mf != nullptr && t->mw == W && span_h == t->mh) {
            cf = t->mf;
        } else {
            embi_lres r;
            bool soft = isnan(t->hspec) && isnan(m->ch_def) && !pct_scroll_resolves(c, m, t);
            cf = lay_cell(c, t->box, W, t->sw, span_h, cell_bd(m, t), soft, &r);
        }
        t->mf = nullptr;
        embi_vec_push(&frags, cf);
        if (cf != nullptr && t->rspan == 1 && t->valign == VA_BASELINE) {
            trow *row = &m->rows.data[t->row];
            float bl = cell_baseline(cf);
            if (isnan(row->bl_fin) || bl > row->bl_fin) {
                row->bl_fin = bl;
            }
            row->bl_real = row->bl_real || cf->first != nullptr;
        }
    }
    for (uint32_t i = 0; i < m->cells.len; i++) {
        tcell *t = &m->cells.data[i];
        trow *row = &m->rows.data[t->row];
        embi_frag *cf = frags.data[i];
        if (cf == nullptr) {
            continue;
        }
        float span_h = m->spv * (float)(t->rspan - 1);
        for (uint32_t r = t->row; r < t->row + t->rspan; r++) {
            span_h += m->rows.data[r].h;
        }
        if (t->hide) {
            cf->flags |= EMBI_FF_HIDE_EMPTY;
        }
        float dy = 0;
        float slack = span_h - t->ev - t->nat;
        if (slack < 0) {
            slack = 0;
        }
        switch (t->valign) {
        case VA_MIDDLE:
            dy = slack / 2;
            break;
        case VA_BOTTOM:
            dy = slack;
            break;
        case VA_BASELINE:
            if (!isnan(row->bl_fin)) {
                dy = row->bl_fin - cell_baseline(cf);
            } else if (!isnan(row->bl_base) && !isnan(t->bl)) {
                dy = row->bl_base - t->bl;
            }
            break;
        default:
            break;
        }
        shift_content(cf, dy);
        cf->x = m->cols.data[t->col].x - gx0;
        if (m->rtl) {
            cf->x = W - cf->x - t->sw; /* the grid is mirrored: column 0 is at the right */
        }
        cf->y = 0;
        if (spans_collapsed(m, t)) {
            cf->flags |= EMBI_FF_CLIP;
        }
        embi_lay_frag_append(row->frag, cf);
        embi_lay_ov_add_child(row->frag, cf);
    }
    embi_vec_free(&frags);
}

/* The column and column group backgrounds of the model: the groups first, then the columns (a column element
 * that spans several columns is one item), and the row bands they show in. */
static embi_tcols *frag_cols(embi_lctx *c, const tmodel *m, float ox, float oy, float gx0, float gw)
{
    uint32_t ng = 0, nc = 0;
    for (uint32_t j = 0; j < m->cols.len; j++) {
        const tcol *cl = &m->cols.data[j];
        if (cl->grpbox != nullptr && (j == 0 || m->cols.data[j - 1].grpbox != cl->grpbox)) {
            ng++;
        }
        if (cl->colbox != nullptr && (j == 0 || m->cols.data[j - 1].colbox != cl->colbox)) {
            nc++;
        }
    }
    if (ng + nc == 0 || m->rows.len == 0) {
        return nullptr;
    }
    embi_tcols *t = (embi_tcols *)embi_lay_alloc(&c->ld->fa, sizeof *t, alignof(embi_tcols));
    t->n = ng + nc;
    t->nrows = m->rows.len;
    t->items = (embi_tcol *)embi_lay_alloc(&c->ld->fa, sizeof(embi_tcol) * t->n, alignof(embi_tcol));
    t->rows = (float *)embi_lay_alloc(&c->ld->fa, sizeof(float) * 2 * t->nrows, alignof(float));
    for (uint32_t r = 0; r < m->rows.len; r++) {
        t->rows[2 * r] = oy + m->rows.data[r].top;
        t->rows[2 * r + 1] = m->rows.data[r].h;
    }
    uint32_t ig = 0, ic = ng;
    for (uint32_t j = 0; j < m->cols.len; j++) {
        const tcol *cl = &m->cols.data[j];
        /* The physical left edge: an rtl grid is mirrored (column 0 is at the right). */
        float cx = m->rtl ? ox + gx0 + gw - (cl->x - gx0) - cl->w : ox + cl->x;
        if (cl->grpbox != nullptr) {
            if (j == 0 || m->cols.data[j - 1].grpbox != cl->grpbox) {
                t->items[ig].box = cl->grpbox;
                t->items[ig].x = cx;
                t->items[ig].w = 0;
                ig++;
            }
            embi_tcol *it = &t->items[ig - 1];
            float x0 = it->x, x1 = it->x + it->w;
            float nx0 = cx, nx1 = cx + cl->w;
            if (it->w == 0) {
                x0 = nx0;
                x1 = nx1;
            } else {
                x0 = nx0 < x0 ? nx0 : x0;
                x1 = nx1 > x1 ? nx1 : x1;
            }
            it->x = x0;
            it->w = x1 - x0;
        }
        if (cl->colbox != nullptr) {
            if (j == 0 || m->cols.data[j - 1].colbox != cl->colbox) {
                t->items[ic].box = cl->colbox;
                t->items[ic].x = cx;
                t->items[ic].w = 0;
                ic++;
            }
            embi_tcol *it = &t->items[ic - 1];
            float x0 = it->w == 0 ? cx : (cx < it->x ? cx : it->x);
            float x1 = cx + cl->w;
            if (it->w != 0 && it->x + it->w > x1) {
                x1 = it->x + it->w;
            }
            it->x = x0;
            it->w = x1 - x0;
        }
    }
    return t;
}

static embi_cborder to_cborder(const tbord *t)
{
    embi_cborder o;
    o.w = t->w;
    o.style = t->kw;
    o.side = t->side;
    o.prio = t->prio;
    o.owner = t->owner;
    return o;
}

/* The collapsed winners of the model, copied to the table's fragment for W4. */
static embi_cgrid *frag_grid(embi_lctx *c, const tmodel *m, float ox, float oy, float gx0, float gw)
{
    embi_cgrid *g = (embi_cgrid *)embi_lay_alloc(&c->ld->fa, sizeof(embi_cgrid), alignof(embi_cgrid));
    g->rows = m->R;
    g->cols = m->C;
    size_t nh = (size_t)(m->R + 1) * m->C;
    size_t nv = (size_t)m->R * (m->C + 1);
    g->h = (embi_cborder *)embi_lay_alloc(&c->ld->fa, sizeof(embi_cborder) * nh, alignof(embi_cborder));
    g->v = (embi_cborder *)embi_lay_alloc(&c->ld->fa, sizeof(embi_cborder) * nv, alignof(embi_cborder));
    for (size_t i = 0; i < nh; i++) {
        g->h[i] = to_cborder(&m->hb.data[i]);
    }
    for (size_t i = 0; i < nv; i++) {
        /* The fragment's edges are physical, left to right: an rtl table stores them reversed. */
        size_t src = i;
        if (m->rtl) {
            size_t row = i / (m->C + 1), cc = i % (m->C + 1);
            src = row * (m->C + 1) + (m->C - cc);
        }
        g->v[i] = to_cborder(&m->vb.data[src]);
    }
    g->rtl = m->rtl;
    g->xb = nullptr;
    g->yb = nullptr;
    if (m->cols.len == m->C && m->rows.len == m->R) {
        g->xb = (float *)embi_lay_alloc(&c->ld->fa, sizeof(float) * (m->C + 1), alignof(float));
        g->yb = (float *)embi_lay_alloc(&c->ld->fa, sizeof(float) * (m->R + 1), alignof(float));
        /* The physical lines, left to right: an rtl grid is mirrored within the columns' span (gx0, gw). */
        for (uint32_t j = 0; j < m->C; j++) {
            const tcol *cl = &m->cols.data[m->rtl ? m->C - 1 - j : j];
            g->xb[j] = ox + (m->rtl ? 2.0f * gx0 + gw - cl->x - cl->w : cl->x);
        }
        const tcol *lastc = &m->cols.data[m->rtl ? 0 : m->C - 1];
        g->xb[m->C] = ox + (m->rtl ? 2.0f * gx0 + gw - lastc->x : lastc->x + lastc->w);
        for (uint32_t r = 0; r < m->R; r++) {
            g->yb[r] = oy + m->rows.data[r].top;
        }
        const trow *lastr = &m->rows.data[m->R - 1];
        g->yb[m->R] = oy + lastr->top + lastr->h;
    }
    return g;
}

void embi_table_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in,
                        embi_flex_out *out)
{
    tmodel m;
    model_copy(c, b, &m);
    if (m.cols.len == 0 && isnan(in->ch_def)) {
        m.spv = 0; /* rows of a column-less table with an auto height take no spacing */
    }
    float W = in->cw;
    float ox = in->ox, oy = in->oy;
    /* An absolutely positioned child of the table is out of flow: its static position is the content start. */
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) != 0) {
            embi_lay_abspos_at(c, k, b, ox, oy, nullptr);
        }
    }
    column_widths(&m, W);
    cell_widths(&m);
    /* The percentage heights of the rows and cells are of the table's definite height (CSS Tables 3
     * section 3.6). */
    m.ch_def = in->ch_def;
    m.restricted = !isnan(m.ch_def) || restricted_height(c, b);
    for (uint32_t i = 0; i < m.cells.len; i++) {
        tcell *t = &m.cells.data[i];
        if (!isnan(t->hpct) && !isnan(m.ch_def)) {
            t->hspec = t->hpct * m.ch_def + (t->bb ? 0.0f : t->ev);
        }
    }
    for (uint32_t r = 0; r < m.rows.len; r++) {
        trow *row = &m.rows.data[r];
        /* A percentage row height is of its row group's height when that has one, else of the table's. */
        float base = m.ch_def;
        if (row->group != UINT32_MAX && !isnan(m.groups.data[row->group].hspec)) {
            base = m.groups.data[row->group].hspec;
        }
        if (!isnan(row->hpct) && !isnan(base)) {
            row->hspec = row->hpct * base;
        }
    }

    float top_h = 0, bot_h = 0;
    lay_captions(c, &m, W, &top_h, &bot_h);
    ((embi_tfrag *)f)->cap_t = top_h;
    ((embi_tfrag *)f)->cap_b = bot_h;
    /* The top captions come first in the fragment tree. */
    float y = 0;
    for (uint32_t i = 0; i < m.caps.len; i++) {
        tcap *cp = &m.caps.data[i];
        if (cp->bottom || cp->frag == nullptr) {
            continue;
        }
        cp->frag->x = ox + cp->ml;
        cp->frag->y = oy + y + cp->mt;
        embi_lay_frag_append(f, cp->frag);
        embi_lay_ov_add_child(f, cp->frag);
        y += cp->mt + cp->h + cp->mb;
    }
    /* The rows span the column grid: their width is what the percentages of the cells (padding) resolve
     * against (WPT tentative/element-sizing.html). */
    float gx0 = 0, gw = W;
    if (m.cols.len > 0) {
        const tcol *first = &m.cols.data[0], *last = &m.cols.data[m.cols.len - 1];
        gx0 = first->x;
        gw = last->x + last->w - gx0;
    }
    size_rows(c, &m, gw);

    /* A table taller than its rows gets the extra height in its rows (CSS Tables 3 section 3.6). */
    float gbase = top_h;
    place_rows(&m, gbase);
    float G = grid_height(&m, gbase);
    /* Browsers also stretch the rows to a min-height of the table. */
    float want = in->ch_def;
    if (in->minh > 0.0f && (isnan(want) || in->minh > want)) {
        want = in->minh;
    }
    if (!isnan(want) && m.rows.len > 0 && want > gbase + G + bot_h) {
        float extra = want - (gbase + G + bot_h);
        float sum = 0;
        for (uint32_t r = 0; r < m.rows.len; r++) {
            sum += m.rows.data[r].h;
        }
        for (uint32_t r = 0; r < m.rows.len; r++) {
            trow *row = &m.rows.data[r];
            if (!row->vcol) {
                row->h += sum > 0 ? extra * row->h / sum : extra / (float)m.rows.len;
            }
        }
        place_rows(&m, gbase);
        G = grid_height(&m, gbase);
    }

    make_row_frags(c, &m, f, gw, ox + gx0, oy);
    place_cells_final(c, &m, gw, gx0);
    for (uint32_t r = 0; r < m.rows.len; r++) {
        trow *row = &m.rows.data[r];
        if (row->group != UINT32_MAX) {
            embi_lay_ov_add_child(m.groups.data[row->group].frag, row->frag);
        } else {
            embi_lay_ov_add_child(f, row->frag);
        }
    }
    for (uint32_t g = 0; g < m.groups.len; g++) {
        embi_lay_ov_add_child(f, m.groups.data[g].frag);
    }
    ((embi_tfrag *)f)->coll = (m.collapse && m.R > 0 && m.C > 0) ? frag_grid(c, &m, ox, oy, gx0, gw) : nullptr;
    ((embi_tfrag *)f)->cols = frag_cols(c, &m, ox, oy, gx0, gw);

    /* The bottom captions follow the grid. */
    float yb = gbase + G;
    for (uint32_t i = 0; i < m.caps.len; i++) {
        tcap *cp = &m.caps.data[i];
        if (!cp->bottom || cp->frag == nullptr) {
            continue;
        }
        cp->frag->x = ox + cp->ml;
        cp->frag->y = oy + yb + cp->mt;
        embi_lay_frag_append(f, cp->frag);
        embi_lay_ov_add_child(f, cp->frag);
        yb += cp->mt + cp->h + cp->mb;
    }

    out->ch = yb;
    /* The table's baseline is its first row's (CSS Tables 3 section 3.6): the baseline cells' common
     * baseline; the bottom of the row when it has cells but none is baseline-aligned; the top of an
     * empty row (WPT tentative/baseline-table.html). A table without rows has none (its parent
     * synthesizes the bottom edge). */
    out->bl1 = NAN;
    out->bl2 = NAN;
    if (m.rows.len > 0) {
        const trow *r0 = &m.rows.data[0];
        float base = 0;
        if (!isnan(r0->bl_fin) && r0->bl_real) {
            base = r0->bl_fin;
        } else {
            for (uint32_t i = 0; i < m.cells.len; i++) {
                if (m.cells.data[i].row == 0) {
                    base = r0->h;
                    break;
                }
            }
        }
        out->bl1 = oy + r0->top + base;
        /* The last baseline is the last row's, found the same way. */
        uint32_t rl = m.rows.len - 1;
        const trow *rn = &m.rows.data[rl];
        float lbase = 0;
        if (!isnan(rn->bl_fin) && rn->bl_real) {
            lbase = rn->bl_fin;
        } else {
            for (uint32_t i = 0; i < m.cells.len; i++) {
                if (m.cells.data[i].row == rl) {
                    lbase = rn->h;
                    break;
                }
            }
        }
        out->bl2 = oy + rn->top + lbase;
    }
    model_free(&m);
}

/* The min-content and max-content content widths of a table (CSS Tables 3 section 3.5). The minimum is
 * the sum of the column minima and the spacing. The maximum is the largest of: the sum of the column
 * maxima; each percentage column's maximum over its percentage; and the maximum of the other columns
 * over what the percentages leave (CSS Tables 3 "rule 2"). When the percentages add up to 100% or more
 * and the other columns have content, that last bound is unbounded: the finite maximum is reported (a
 * parent's contribution, a flex basis), and EMBI_TF_FILL tells the table's own width resolution to use
 * the available width (WPT tentative/table-minmax.html). */
static void table_sizes(embi_lctx *c, embi_box *b, float *mn, float *mx, float *plain, bool own)
{
    tmodel local;
    model_build(c, b, &local);
    const tmodel *mp = &local;
    const tmodel *const pm_ = mp;
#define m (*pm_)
    if (own || !m.nested) {
        b->tflags &= (uint8_t)~EMBI_TF_FILL;
    }
    if (m.nested && !own) {
        /* The percentage widths of a table inside a cell do not count in its intrinsic sizes (what the
         * cell contributes to its table), only when the table is laid out itself (WPT
         * percent-width-ignored-001). */
        for (uint32_t j = 0; j < m.cols.len; j++) {
            m.cols.data[j].pct = NAN;
        }
    }
    uint32_t n = m.cols.len;
    float smin = 0, smax = 0;
    float P = 0, pct_need = 0, nonpct_max = 0;
    for (uint32_t j = 0; j < n; j++) {
        const tcol *col = &m.cols.data[j];
        if (m.fixed) {
            float w = isnan(col->spec) ? 0.0f : col->spec;
            smin += w;
            smax += w;
            continue;
        }
        smin += col->min;
        smax += col->max;
        if (col_is_pct(col)) {
            P += col->pct;
            float need = col->max / col->pct;
            pct_need = need > pct_need ? need : pct_need;
        } else {
            nonpct_max += col->max;
        }
    }
    if (!m.fixed && P > 0) {
        if (P < 0.9999f) {
            float rest = nonpct_max / (1.0f - P);
            pct_need = rest > pct_need ? rest : pct_need;
        } else if (nonpct_max > 0) {
            if (own || !m.nested) {
                b->tflags |= EMBI_TF_FILL;
            }
        }
        if (pct_need > smax) {
            smax = pct_need;
        }
    }
    if (n > 0) {
        float gap = m.sph * (float)(n + 1);
        smin += gap;
        smax += gap;
    }
    if (smax < smin) {
        smax = smin;
    }
    for (uint32_t i = 0; i < m.caps.len; i++) {
        float cmn = 0, cmx = 0;
        embi_lay_intrinsic_outer(c, m.caps.data[i].box, &cmn, &cmx);
        if (cmn > smin) {
            smin = cmn;
        }
        if (cmx > smax) {
            smax = cmx;
        }
    }
    *mn = smin;
    *mx = smax;
    if (plain != nullptr) {
        /* width: max-content: every column at its maximum, percentages do not widen the table. */
        float pm = 0;
        for (uint32_t j = 0; j < n; j++) {
            pm += m.fixed ? (isnan(m.cols.data[j].spec) ? 0.0f : m.cols.data[j].spec) : m.cols.data[j].max;
        }
        if (n > 0) {
            pm += m.sph * (float)(n + 1);
        }
        *plain = fmaxf(pm, *mn);
    }
#undef m
    if (mp == &local) {
        model_free(&local);
    }
}

void embi_table_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    table_sizes(c, b, mn, mx, nullptr, false);
}

void embi_table_intrinsic_own(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    table_sizes(c, b, mn, mx, nullptr, true);
}

float embi_table_max_content_plain(embi_lctx *c, embi_box *b)
{
    float mn = 0, mx = 0, plain = 0;
    table_sizes(c, b, &mn, &mx, &plain, true);
    return plain;
}

void embi_lay_table(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    /* A table box is laid out as a block box (block_pass calls embi_table_content for its content). The
     * cells, rows and groups never reach this point through the box dispatch. */
    embi_lay_block(c, b, cs, out);
}
