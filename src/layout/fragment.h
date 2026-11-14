#pragma once
/*
 * The fragment tree (W3a). A fragment is one placed piece of a box: a block or atomic box, an
 * inline box piece, a line box, or a run of text. Fragments are allocated in the per-document
 * fragment arena and freed as a whole with the next layout or with embi_layout_forget_document.
 *
 * Positions (x, y) are relative to the parent fragment's border-box origin. w and h are the border
 * box size. The used border widths, paddings and margins are in a shared metrics record (embi_fmet,
 * read through embi_frag_bdr, embi_frag_pad and embi_frag_mg). ov is the scrollable overflow rectangle as
 * x0, y0, x1, y1 relative to this fragment's border-box origin (read through embi_frag_ov). bl_first and
 * bl_last are the first and last baselines relative to the border-box top (NaN when there are none).
 */
#include "base/base.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

struct embi_box;

enum {
    EMBI_FK_BOX = 1,  /* a box: block, atomic inline, replaced, inline box piece */
    EMBI_FK_LINE = 2, /* a line box of an inline formatting context */
    EMBI_FK_TEXT = 3, /* a run of text (text is set) */
};

/* Fragment.cut: the box continues before this piece (its top edge is a cut), and after it (its bottom edge is a cut). */
#define EMBI_CUT_BEFORE 0x01u
#define EMBI_CUT_AFTER 0x02u

/* Fragment flags. */
#define EMBI_FF_STACKING 0x01u /* a stacking context (W3d sets it, W4 reads it) */
#define EMBI_FF_SCROLL 0x02u   /* a scroll container */
#define EMBI_FF_CB_FIXED 0x04u /* containing block for fixed-position descendants */
#define EMBI_FF_DEF_H 0x10u    /* its used height is definite (a stretched flex item): percentage bases */
#define EMBI_FF_CLIP 0x08u     /* clips its contents (overflow other than visible; scroll implies it) */
#define EMBI_FF_TABLE 0x40u    /* the fragment of a table box: allocated as an embi_tfrag (embi_frag_coll) */
#define EMBI_FF_EXT 0x80u      /* met points into an embi_fext (a private metrics record with a scroll offset,
                                * gutters, z-index or sticky data); see embi_lay_frag_ext */
#define EMBI_FF_HIDE_EMPTY 0x20u /* an empty table cell with empty-cells: hide (separate model): W4 paints no
                                  * background, border or shadow for it (table.c, W3g) */

/* Collapsed borders of a table with border-collapse: collapse (table.c, W3g), kept on the table's
 * fragment (embi_tfrag.coll, read through embi_frag_coll) for W4. h holds the (rows + 1) * cols horizontal edges, row-major (index
 * r * cols + j: the edge above row r in column j), and v the rows * (cols + 1) vertical edges (index
 * i * (cols + 1) + c: the edge left of column c in row i). A winner has the width it paints (0 when
 * hidden or none), the style keyword (EMBI_KW_*), the side of its owner (0 top, 1 right, 2 bottom,
 * 3 left), the element priority (5 cell, 4 row, 3 row group, 2 column, 1 column group, 0 table) and
 * the owner box, whose computed style gives the color. */
typedef struct embi_cborder {
    float w;
    uint16_t style;
    uint8_t side;
    uint8_t prio;
    const struct embi_box *owner;
} embi_cborder;

typedef struct embi_cgrid {
    uint32_t rows, cols;
    embi_cborder *h; /* (rows + 1) * cols */
    embi_cborder *v; /* rows * (cols + 1) */
    /* The grid lines in the frame of the table fragment, left to right and top to bottom (cols + 1 and rows + 1 of them;
     * nullptr when the model has no such lines). Rows and columns without size share their lines, which is why paint
     * cannot get them back from the cells. */
    float *xb;
    float *yb;
    bool rtl; /* the table runs right to left: an edge's before side is its physical right */
} embi_cgrid;

/* The used border widths, paddings and margins of a box fragment: left, top, right, bottom. The records
 * are interned per layout (embi_lay_met, block.c): fragments with the same numbers share one, which is
 * most of them, so the 48 bytes cost a pointer per fragment. A shared record is never written. */
typedef struct embi_fmet {
    float bdr[4];
    float pad[4];
    float mg[4]; /* the vertical ones as the parent collapses them */
} embi_fmet;

/* Side data of the fragments that need it (W3h): the scroll offset (set by W4/W5), the scrollbar gutters,
 * the z-index of a stacking context and the sticky data. A fragment that has it (EMBI_FF_EXT) points with
 * `met` at the metrics record at the start of its own embi_fext, a private copy that is not shared;
 * ordinary fragments do not pay for the 32 bytes. */
typedef struct embi_fext {
    embi_fmet m;                /* first: the fragment's met points here */
    float sx, sy;               /* scroll offset (0 until W4/W5 set one) */
    float scr[2];               /* scrollbar gutters: inline end (vertical bar), block end (horizontal bar) */
    int32_t z_index;            /* z-index of a stacking context (0 when auto); set by position.c */
    struct embi_sticky *sticky; /* position: sticky boxes (position.c), else nullptr */
    struct embi_mcol *mc;       /* a multi-column container: its column sets, for the column rules (multicol.c) */
} embi_fext;

/* The fragment is allocated in tiers (W3p): only the prefix a kind needs is allocated, in the order of
 * the fields below. A TEXT fragment has the first 48 bytes (through the text range), a LINE fragment adds
 * the children and the baselines, an inline box piece has the line fields and the metrics, and a
 * block-level or atomic box has everything. A field outside the tier of its fragment must not be read
 * or written: use the accessors (embi_frag_first, embi_frag_bl_first, embi_frag_bdr, embi_frag_ov ...), or
 * embi_fl(f) and embi_fb(f) for the checked access to the fields of lines and boxes. Sanitizer builds
 * allocate the whole record and poison the part past the tier, so a stray access is reported. */
typedef struct embi_frag {
    struct embi_box *box;        /* the box this fragment belongs to */
    struct embi_frag *parent;
    struct embi_frag *next;      /* next sibling fragment */
    float x, y, w, h;            /* border box, relative to the parent's border-box origin */
    uint8_t kind;                /* EMBI_FK_* */
    uint8_t flags;               /* EMBI_FF_* */
    union {
        uint8_t trange[6];       /* TEXT: byte range [begin, end) of the run in box->lt, 24 bits each
                                  * (embi_frag_tbegin, embi_frag_tend, embi_frag_set_trange) */
        struct {
            uint8_t inl;         /* BOX: 1 for the piece of an inline box (allocated without ov) */
            uint8_t cut;         /* BOX, not inl: EMBI_CUT_*, the sides where a multi-column container cut the box (multicol.c) */
        };
    };
    /* ---- end of the TEXT tier (48 bytes) ---- */
    struct embi_frag *first;     /* first child fragment (not for TEXT: use embi_frag_first) */
    struct embi_frag *last;      /* last child fragment (kept by embi_lay_frag_append, O(1) appends) */
    float bl_first;              /* first baseline relative to the border-box top, NaN when none */
    float bl_last;               /* last baseline */
    /* ---- end of the LINE tier (72 bytes) ---- */
    const embi_fmet *met;        /* border, padding and margins; nullptr when all are zero */
    /* ---- end of the INLINE tier (80 bytes) ---- */
    float ov[4];                 /* scrollable overflow: x0, y0, x1, y1 (not for lines and inline pieces: embi_frag_ov) */
} embi_frag;

/* Checked access to the fields past the TEXT tier (W3p). A debug build asserts that the fragment has the
 * field; Release builds compile the checks out. Use embi_fl(f)->first, ->last, ->bl_first, ->bl_last for
 * lines and boxes, embi_fb(f)->met for box fragments (inline pieces included) and embi_fo(f)->ov for the
 * box fragments that store their overflow rectangle (not the pieces of inline boxes). */
static inline embi_frag *embi_fl_chk(embi_frag *f)
{
    EMBI_ASSERT(f->kind != EMBI_FK_TEXT);
    return f;
}

static inline const embi_frag *embi_fl_chkc(const embi_frag *f)
{
    EMBI_ASSERT(f->kind != EMBI_FK_TEXT);
    return f;
}

static inline embi_frag *embi_fb_chk(embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX);
    return f;
}

static inline const embi_frag *embi_fb_chkc(const embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX);
    return f;
}

static inline embi_frag *embi_fo_chk(embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX && f->inl == 0);
    return f;
}

static inline const embi_frag *embi_fo_chkc(const embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX && f->inl == 0);
    return f;
}

#define embi_fl(f) _Generic((f), const embi_frag *: embi_fl_chkc, embi_frag *: embi_fl_chk)(f)
#define embi_fb(f) _Generic((f), const embi_frag *: embi_fb_chkc, embi_frag *: embi_fb_chk)(f)
#define embi_fo(f) _Generic((f), const embi_frag *: embi_fo_chkc, embi_frag *: embi_fo_chk)(f)

#define EMBI_FSZ_TEXT offsetof(embi_frag, first)
#define EMBI_FSZ_LINE offsetof(embi_frag, met)
#define EMBI_FSZ_INLINE offsetof(embi_frag, ov)
#define EMBI_FSZ_FULL sizeof(embi_frag)

/* The size budget of a fragment (W3p). */
static_assert(sizeof(embi_frag) == 96, "embi_frag must stay at the 96-byte budget");
static_assert(offsetof(embi_frag, first) == 48, "the TEXT tier is 48 bytes");
static_assert(offsetof(embi_frag, met) == 72, "the LINE tier is 72 bytes");
static_assert(offsetof(embi_frag, ov) == 80, "the INLINE tier is 80 bytes");
static_assert(sizeof(embi_fmet) == 48, "embi_fmet layout changed");
static_assert(sizeof(embi_fext) == 88, "embi_fext layout changed");

/* The byte range of a TEXT fragment in the shaped run of its box. A text box is never longer than
 * EMBI_TEXT_MAX bytes (box.c splits longer text), so 24 bits hold both ends. */
#define EMBI_TEXT_MAX 0xFFFFFFu

static inline uint32_t embi_frag_tbegin(const embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_TEXT);
    return (uint32_t)f->trange[0] | ((uint32_t)f->trange[1] << 8) | ((uint32_t)f->trange[2] << 16);
}

static inline uint32_t embi_frag_tend(const embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_TEXT);
    return (uint32_t)f->trange[3] | ((uint32_t)f->trange[4] << 8) | ((uint32_t)f->trange[5] << 16);
}

static inline void embi_frag_set_trange(embi_frag *f, uint32_t b, uint32_t e)
{
    EMBI_ASSERT(f->kind == EMBI_FK_TEXT && b <= EMBI_TEXT_MAX && e <= EMBI_TEXT_MAX);
    f->trange[0] = (uint8_t)b;
    f->trange[1] = (uint8_t)(b >> 8);
    f->trange[2] = (uint8_t)(b >> 16);
    f->trange[3] = (uint8_t)e;
    f->trange[4] = (uint8_t)(e >> 8);
    f->trange[5] = (uint8_t)(e >> 16);
}

/* The first child fragment (a TEXT fragment has none). */
static inline embi_frag *embi_frag_first(const embi_frag *f)
{
    return f->kind != EMBI_FK_TEXT ? f->first : nullptr;
}

/* The baselines (NaN for a text run). */
static inline float embi_frag_bl_first(const embi_frag *f)
{
    return f->kind != EMBI_FK_TEXT ? f->bl_first : NAN;
}

static inline float embi_frag_bl_last(const embi_frag *f)
{
    return f->kind != EMBI_FK_TEXT ? f->bl_last : NAN;
}

/* The used border width, padding or margin of side i (0 left, 1 top, 2 right, 3 bottom), 0 for the
 * fragments that have none (text, lines, boxes without any). */
static inline float embi_frag_bdr(const embi_frag *f, int i)
{
    return f->kind == EMBI_FK_BOX && f->met != nullptr ? f->met->bdr[i] : 0.0f;
}

static inline float embi_frag_pad(const embi_frag *f, int i)
{
    return f->kind == EMBI_FK_BOX && f->met != nullptr ? f->met->pad[i] : 0.0f;
}

static inline float embi_frag_mg(const embi_frag *f, int i)
{
    return f->kind == EMBI_FK_BOX && f->met != nullptr ? f->met->mg[i] : 0.0f;
}

/* The scrollable overflow rectangle of any fragment. It is stored for block-level and atomic boxes; a
 * TEXT fragment and the piece of an inline box have their border box, and a line the union of its border box
 * and its children's, computed on the call (block.c). */
void embi_frag_ov(const embi_frag *f, float out[4]);

/* The side record of f when it has one (the scroll offset, gutters, z-index, sticky data), else nullptr. */
static inline embi_fext *embi_frag_ext(const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && (f->flags & EMBI_FF_EXT) != 0 ? (embi_fext *)(uintptr_t)f->met : nullptr;
}

static inline float embi_frag_sx(const embi_frag *f)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->sx : 0.0f;
}

static inline float embi_frag_sy(const embi_frag *f)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->sy : 0.0f;
}

/* The column sets of a multi-column container (multicol.c): each set is a run of columns between two column-span: all
 * boxes (or the whole content). Column j of a set starts at x0 + j * stride (stride is negative in a right-to-left
 * container) in the container fragment's frame, with the set's y and h; cols is the number of columns that have content. */
typedef struct embi_mcset {
    float y, h;
    uint32_t cols;
} embi_mcset;

typedef struct embi_mcol {
    float colw, gap, x0, stride;
    uint32_t nsets;
    embi_mcset sets[];
} embi_mcol;

static inline const embi_mcol *embi_frag_mc(const embi_frag *f);

/* The scrollbar gutter of f: i 0 is the inline end (a vertical bar), i 1 the block end. */
static inline float embi_frag_scr(const embi_frag *f, int i)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->scr[i] : 0.0f;
}

static inline const embi_mcol *embi_frag_mc(const embi_frag *f)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->mc : nullptr;
}

static inline int32_t embi_frag_z_index(const embi_frag *f)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->z_index : 0;
}

static inline const struct embi_sticky *embi_frag_sticky(const embi_frag *f)
{
    const embi_fext *x = embi_frag_ext(f);
    return x != nullptr ? x->sticky : nullptr;
}

/* The side record of f, made on first use from the document's recycled side records or its fragment
 * arena (nullptr when the arena cannot grow). Its metrics start as a copy of the fragment's. Fragments of
 * the document are freed with its next layout. */
struct embi_lay_doc;
embi_fext *embi_lay_frag_ext(struct embi_lay_doc *ld, embi_frag *f);

/* The offset a sticky fragment takes at the current scroll offset of its scroll container (CSS
 * Position 3 section 3.4), to add to x and y when painting. Zero for other fragments. */
void embi_lay_sticky_offset(const embi_frag *f, float *dx, float *dy);

/* Whether f is the box of a position: fixed element whose containing block is the viewport (a child of the initial
 * containing block). The viewport's scroll offset does not move it (CSS Position 3 section 2.2): paint, the hit test and
 * the painted geometry (embi_layout_painted_box) add the offset back for it and its content. */
bool embi_lay_frag_vp_fixed(const embi_frag *f);

/* The fragment of a table box is allocated with room for its collapsed borders after the common
 * fragment, so the 8 bytes cost only tables, not every fragment (the 160 B per box budget). */
/* The column and column group boxes of a table, for W4 to paint their backgrounds (CSS 2.1 17.5.1: column groups,
 * then columns, below the row groups). x and w are in the table fragment's frame (the columns' physical extent);
 * the row bands rows (top, height pairs, also in the table fragment's frame) are where the backgrounds show: the
 * border-spacing between rows stays the table's. */
typedef struct embi_tcol {
    const struct embi_box *box;
    float x, w;
} embi_tcol;

typedef struct embi_tcols {
    uint32_t n;      /* items: the column groups first, then the columns */
    uint32_t nrows;  /* row bands */
    embi_tcol *items;
    float *rows;     /* 2 * nrows floats: top, height */
} embi_tcols;

typedef struct embi_tfrag {
    embi_frag f;
    embi_cgrid *coll; /* border-collapse: collapse, the winning borders (table.c), else nullptr */
    embi_tcols *cols; /* column and column group backgrounds (table.c), else nullptr */
    float cap_t, cap_b; /* the heights of the top and bottom caption stacks: the table box (background and borders) lies
                         * between them, the captions are outside it (CSS 2.1 17.4) */
} embi_tfrag;

/* The collapsed borders of a table fragment, or nullptr (a separate-borders table, or any other fragment). */
static inline embi_cgrid *embi_frag_coll(const embi_frag *f)
{
    return (f->flags & EMBI_FF_TABLE) != 0 ? ((const embi_tfrag *)f)->coll : nullptr;
}

/* The heights of the captions above and below the table box of table fragment f (0 for other fragments). */
static inline void embi_frag_caps(const embi_frag *f, float *top, float *bot)
{
    *top = (f->flags & EMBI_FF_TABLE) != 0 ? ((const embi_tfrag *)f)->cap_t : 0.0f;
    *bot = (f->flags & EMBI_FF_TABLE) != 0 ? ((const embi_tfrag *)f)->cap_b : 0.0f;
}

/* The column boxes of a table fragment, or nullptr. */
static inline const embi_tcols *embi_frag_cols(const embi_frag *f)
{
    return (f->flags & EMBI_FF_TABLE) != 0 ? ((const embi_tfrag *)f)->cols : nullptr;
}
