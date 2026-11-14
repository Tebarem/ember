#pragma once
/*
 * The box tree (W3a, section 1). Boxes are built from the DOM and the computed styles of one
 * document by embi_lay_build (box.c). The tree is a pure function of (DOM subtree, styles), and it
 * lives in the document's box arena until the next layout. Each element with a box points to its
 * principal box through its `box` slot.
 */
#include "layout/fragment.h"

#include <stddef.h>
#include <stdint.h>

struct embi_element;
struct embi_style;
struct embi_frag;
struct embi_lay_text;

enum embi_box_kind {
    EMBI_BK_BLOCK = 1,      /* block container (flow, flow-root, list item, anonymous block) */
    EMBI_BK_INLINE,         /* inline box (one piece of an inline element) */
    EMBI_BK_TEXT,           /* a run of text */
    EMBI_BK_REPLACED,       /* replaced element, or a form control with a fixed size */
    EMBI_BK_FLEX,           /* flex container (laid out as a block until W3e) */
    EMBI_BK_GRID,           /* grid container (laid out as a block until W3f) */
    EMBI_BK_TABLE,          /* table, table-row-group, row, cell, caption, columns: all laid out
                             * as blocks until W3g; kind only tells them apart */
    EMBI_BK_BR,             /* <br>: a forced line break */
    EMBI_BK_MARKER,         /* ::marker of a list item (an inline piece, see BF_OUTSIDE) */
};

/* Box flags. */
#define EMBI_BF_ANON 0x0001u         /* anonymous: non-inherited properties are the initial values */
#define EMBI_BF_IFC 0x0002u          /* block container whose children are all inline-level */
#define EMBI_BF_BFC 0x0004u          /* establishes a new block formatting context */
#define EMBI_BF_INLINE_LEVEL 0x0008u /* the outer display is inline */
#define EMBI_BF_FLOAT 0x0010u        /* float (out of flow until W3b) */
#define EMBI_BF_ABSPOS 0x0020u       /* absolutely positioned or fixed (out of flow until W3d) */
#define EMBI_BF_OUTSIDE 0x0040u      /* MARKER: list-style-position outside (hangs before the line) */
#define EMBI_BF_ROOT 0x0080u         /* the root element */
#define EMBI_BF_INTR 0x0100u         /* min_c and max_c are valid */
#define EMBI_BF_REPLACED 0x0200u     /* the box has a replaced size (rw, rh) */
#define EMBI_BF_HAS_RATIO 0x0400u    /* the replaced intrinsic size has a ratio */
#define EMBI_BF_SCROLL 0x0800u       /* scroll container (overflow other than visible/clip) */
#define EMBI_BF_ATOMIC 0x1000u       /* atomic inline (inline-block, inline-flex, ...) */
#define EMBI_BF_BLOCKIFIED 0x2000u   /* an inline-level box that became block-level (flex items) */
#define EMBI_BF_CLIP 0x4000u         /* overflow other than visible (hidden, clip, or a scroll container) */
#define EMBI_BF_CENTER 0x8000u       /* button: the content is centered vertically in the used height */

/* Table roles (CSS Tables 3), the tr field. A table box is the table element's own box and wrapper
 * in one (captions are its children). Anonymous table objects use the same roles (box.c). */
enum {
    EMBI_TR_NONE = 0,
    EMBI_TR_TABLE,     /* table, inline-table, or an anonymous table */
    EMBI_TR_CAPTION,
    EMBI_TR_COLGROUP,
    EMBI_TR_COL,
    EMBI_TR_THEAD,     /* header row group */
    EMBI_TR_TFOOT,     /* footer row group */
    EMBI_TR_ROWGROUP,  /* table-row-group */
    EMBI_TR_ROW,
    EMBI_TR_CELL,
};

/* flags2. CONTROL: a form control whose (rw, rh) is a border-box size (see the table in box.c). */
#define EMBI_BF2_CONTROL 0x0001u
/* overflow: auto remembers (from the last layout) that the box needed a vertical or horizontal
 * scrollbar, so the next layout starts with the right gutter and needs one pass (block.c). */
#define EMBI_BF2_VGUT 0x0002u
#define EMBI_BF2_HGUT 0x0004u
/* W3p: the height-independence of the box's layout was computed (HIKNOWN) and is true (HINDEP); see
 * embi_lay_height_independent (block.c). */
#define EMBI_BF2_HIKNOWN 0x0008u
#define EMBI_BF2_HINDEP 0x0010u
/* W3p, position.c: FLAGS0 - the stacking and containing-block flags of the box's fragments are all clear
 * and it has no z-index (a plain box: the final pass skips its ~20 style reads); SHIFT0 - no box on the
 * way up through inline ancestors is position: relative, so the box's fragments never shift. Both are
 * functions of the styles alone, so they persist with the box tree. */
#define EMBI_BF2_PPFLAGS0 0x0020u
/* The box's element is an HTML button (set by box.c when the element is assigned). */
#define EMBI_BF2_BUTTON 0x0080u
#define EMBI_BF2_PPSHIFT0 0x0040u
/* A piece of an inline box that was split around block-level children (CSS 2.1 section 9.2.1.1): NOSTART - it is not
 * the first piece, so the box's start margin, border and padding are not here; NOEND - it is not the last. */
#define EMBI_BF2_NOSTART 0x0100u
#define EMBI_BF2_NOEND 0x0200u
/* W5e: a button (button element or input button/submit/reset): the painter draws its face (control.c). */
#define EMBI_BF2_CTR 0x0800u    /* the box takes part in the counters (a list item, counter properties, counter() content) */
#define EMBI_BF2_CHROME 0x0400u
/* A text box whose last character gets no letter-spacing: it ends the element that specifies the spacing (set by inline.c add_text; CSS Text 4 section 8.2). */
#define EMBI_BF2_LSTRIM 0x1000u
/* R4c: a replaced box with a natural ratio but no natural size (an inline svg with a viewBox and no width or height): an auto
 * size is the available width, and the height follows from the ratio (CSS 2.1 10.3.2, CSS Sizing 3 section 5.1). (rw, rh) hold
 * a 300-wide size of that ratio for the intrinsic-size queries. */
#define EMBI_BF2_NOSIZE 0x4000u
/* R4c: the box of a foreignObject element, a child of its svg root's replaced box (see embi_layout_set_svg_fo): a block that
 * is a formatting context, a stacking context and a containing block for all its descendants. Paint and hit testing
 * reach it only through the SVG walk, never through the fragment tree walks. */
#define EMBI_BF2_SVGFO 0x8000u
/* W3q: the box's style has calc-size() in one of width, min-width, max-width, height, min-height, max-height (set by
 * finish_tree from the style summary). Without it embi_lay_size_expr answers nullptr without reading the style. */
#define EMBI_BF2_CALCSIZE 0x2000u

/* tflags. FILL: the table's max-content width is unbounded (percentage columns that add up to 100% or
 * more next to columns with content, CSS Tables 3 section 3.5), so a table with an auto width in a
 * block or float takes the whole available width. Its max-content contribution to a parent stays
 * finite. Set by embi_table_intrinsic. */
#define EMBI_TF_FILL 0x01u
/* A line-clamp container (continue: collapse or discard on a block container): paint clips its content at its block-end
 * content edge, in the block direction only (CSS Overflow 4 section 5.1: the content left out is visually suppressed). */
#define EMBI_TF_CLAMP 0x02u
/* A multi-column container (column-count or column-width is not auto, on a block container): multicol.c. */
#define EMBI_TF_MC 0x04u
/* A box that multicol.c split in fragments (its box->frag is the first one): geometry queries walk the tree for the others. */
#define EMBI_TF_SPLIT 0x08u

/* The box record is 72 bytes, and a text box 64 (W3p). The fields of a text run (text, lt, text_len, in
 * TEXT and MARKER boxes) share the bytes of the fields of the other boxes (first, frag, min_c, max_c), which
 * a text run does not have: it has no children, no fragment pointer and no intrinsic size cache. Read the
 * text fields only of TEXT and MARKER boxes (embi_box_is_text) and the others only of the rest. Text boxes
 * are allocated without the last 8 bytes (rw, rh, cq_w, cq_h), which sanitizer builds poison. */
typedef struct embi_box {
    uint8_t kind;               /* embi_box_kind */
    uint8_t pe;                 /* EMBI_PE_* for generated boxes (::before, ::after, ::marker), else 0 */
    uint16_t flags;             /* EMBI_BF_* */
    uint16_t flags2;            /* EMBI_BF2_* (fits in the padding after flags) */
    uint8_t tr;                 /* EMBI_TR_*: the table role of a table-internal or table box (W3g) */
    uint8_t tflags;             /* EMBI_TF_*: table sizing facts (fits in the padding after tr) */
    struct embi_box *parent;    /* box tree parent (anonymous boxes included) */
    struct embi_box *next;      /* next sibling */
    const struct embi_style *style; /* element or pseudo style; for anonymous and text boxes the
                                     * nearest element style (inherited properties only) */
    struct embi_element *el;    /* the element (the owner for generated boxes) */
    union {
        struct {                /* every box but TEXT and MARKER */
            struct embi_box *first;     /* first child */
            struct embi_frag *frag;     /* the first fragment of this box, nullptr when none */
            float min_c, max_c;         /* intrinsic content widths, valid with EMBI_BF_INTR */
        };
        struct {                /* TEXT, MARKER */
            const char *text;           /* processed UTF-8: in the box arena, or in the DOM text node when
                                         * the processing left it unchanged */
            struct embi_lay_text *lt;   /* the shaped run, created by the first layout */
            uint32_t text_len;
            float adv;                  /* the advance of the whole run once measured (tw, inline.c), else 0 */
        };
    };
    /* ---- end of the text tier (64 bytes) ---- */
    union {
        struct {
            float rw, rh;       /* replaced: the natural size (0 when unknown) */
        };
        struct {
            float cq_w, cq_h;   /* other boxes: the container size (cq units) min_c and max_c were
                                 * computed under, NaN for none; see embi_lay_intrinsic */
        };
    };
} embi_box;

#define EMBI_BSZ_TEXT offsetof(embi_box, rw)

static inline bool embi_box_is_text(const embi_box *b)
{
    return b->kind == EMBI_BK_TEXT || b->kind == EMBI_BK_MARKER;
}

/* The first child of a box (a text run has none). */
static inline embi_box *embi_box_first(const embi_box *b)
{
    return embi_box_is_text(b) ? nullptr : b->first;
}

static_assert(sizeof(embi_box) == 72, "embi_box exceeds the 72-byte budget");
static_assert(offsetof(embi_box, rw) == 64, "the text tier of embi_box is 64 bytes");
