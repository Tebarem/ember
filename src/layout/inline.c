/*
 * Inline formatting context (CSS 2.1 sections 9.4.2, 9.5, 10.8; CSS Text 3; CSS Writing Modes 4 for
 * the bidi isolates; W3 section 5, W3c).
 *
 * Paragraph. The inline content of a block container is one paragraph: the text bytes of its text
 * boxes, one U+FFFC for each atomic inline (with the break and bidi class of an atomic inline), a line
 * feed for each <br> and preserved newline, and the bidi embedding and isolate controls of the inline
 * boxes whose unicode-bidi asks for them. Inline box edges and floats take no bytes: they are items at
 * a position of the paragraph. The paragraph is shaped once (R2c) with the container's style, for its
 * break opportunities (UAX 14 under the CSS break modes) and its bidi levels. The advances come from
 * each text box's own shaping, so every box keeps its font.
 *
 * Items. The paragraph is a list of items in logical order: text pieces, inline box edges, atomic
 * inlines, floats and line feeds. An item at paragraph position p covers the bytes [p, p + n). A line
 * is the item range [j0, j1) between two breaks: split_of gives the index where a break at a position
 * splits the items (the closing edges that end at the break stay on its earlier side).
 *
 * Breaks. A candidate is a break opportunity with its flags after the CSS modes. The greedy breaker
 * takes the last candidate whose content fits (CSS Text 3 section 5): a forced break always ends the
 * line; without a fitting opportunity the first one overflows; with overflow-wrap the emergency
 * breaks are used when no normal opportunity fits. Collapsible spaces at the end of a line hang (they
 * take no room) and those at its start are removed. text-wrap: balance picks the narrowest width that
 * keeps the line count (up to six lines, no floats). Floats met in a line are placed at the line's top
 * (CSS 2.1 9.5.1); the line is laid out again when they shorten it, and moved below them when it fits
 * nowhere beside them.
 *
 * Line content. The units of a line are in logical order: text runs (split where the bidi level
 * changes, and at the spaces when justifying), inline box edges and atomic inlines. Units are placed
 * in visual order (UAX 9 rule L2 over their levels), then text-align places the line in the band the
 * floats leave free (CSS Text 3 section 7.1), and justification adds the free space to the spaces.
 *
 * Vertical alignment. Each inline box and atomic inline has a baseline shift relative to its parent
 * (vertical-align: baseline, sub, super, text-top, text-bottom, middle, lengths and percentages). top
 * and bottom align to the line box after the others (CSS 2.1 10.8.1). The line box is the union of the
 * strut and the extent of every unit (each with the half-leading of its line-height); padding and
 * borders do not change it. text-top, text-bottom and the BOX fragment of an inline box use the
 * content area (the font's ascent and descent, ca and cd), not the line-height box.
 *
 * Fragments. The line box is a LINE fragment with its pieces as children in logical order. A text
 * run is a TEXT fragment that draws the bytes [tbegin, tend) of its box's shaped run at x, on the
 * line's baseline. An inline box has a BOX fragment on each line it occupies: its extent is the union
 * of its units there, and its border box includes its vertical padding and border.
 *
 * text-wrap: pretty is the greedy breaker with orphan avoidance on the last line: when that line would
 * hold one word, the line before it breaks earlier so the last line gets two (pretty_ends, no floats).
 * Justification of text without spaces (ideographic text, text-justify auto) spreads the space between
 * its characters; other scripts without spaces are not justified.
 *
 * Intrinsic sizes. The minimum is the widest run between soft breaks or the widest float's own
 * minimum; the maximum is the widest run between forced breaks, with the floats of a run added.
 *
 * Ellipsis. A line that overflows under text-overflow: ellipsis ends before the next line starts; the
 * items between are clipped but still processed (their boxes open or close, their floats are placed).
 *
 * Not done (for later tasks or other modules): line-clamp (W2 has no property for it); text-indent
 * with hanging and each-line (W2's grammar has no keywords for text-indent); ::first-line and
 * ::first-letter (deferred, not wired, see the W3c notes); ascent-override, descent-override and
 * line-gap-override (the font registry keeps no descriptor for them, so normal uses the face's
 * metrics); box-decoration-break: clone (slice is used); hyphens: auto (it acts as manual, with no
 * dictionaries); the lang attribute reaches shaping through the element's ancestors (embi_lay_lang).
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

#include <SDL3/SDL_timer.h>

/* Break kinds (the EMBI_BREAK_* bits of text.h, which layout does not include), and the end of the
 * paragraph, which is a soft break (it fits or it overflows, like any other). */
#define IFC_BK_LINE 1u
#define IFC_BK_HARD 2u
#define IFC_BK_GRAPHEME 4u
#define IFC_BK_WORD 8u
#define IFC_BK_MINC 16u /* an overflow-wrap: anywhere break: soft only for min-content (CSS Text 3 section 5.5) */

/* Item kinds. */
enum {
    IK_TEXT = 1,
    IK_OPEN,
    IK_CLOSE,
    IK_ATOM,
    IK_FLOAT,
    IK_BR,
};

/* vertical-align classes of an inline box or an atomic inline. */
enum {
    VA_BASE = 0,
    VA_TOP,
    VA_BOTTOM,
};

/* Unit kinds of a line. */
enum {
    UK_TEXT = 1,
    UK_OPEN,
    UK_CLOSE,
    UK_ATOM,
};

/* The advance of a text item from its start to a break candidate inside it. */
typedef struct ifc_ptab {
    uint32_t pos;      /* the paragraph position */
    float w;
} ifc_ptab;

typedef struct ifc_item {
    uint8_t kind;      /* IK_* */
    uint8_t va;        /* VA_* (OPEN and ATOM) */
    uint8_t bm;        /* OPEN and CLOSE: the unicode-bidi kind (BIDI_*), whose control characters are in the bytes */
    bool outside;      /* a hanging marker: no bytes, no room in the line */
    bool placed;       /* FLOAT: placed in the exclusion space */
    embi_box *box;
    embi_lay_text *lt; /* TEXT: the shaped run of the box */
    uint32_t p, n;     /* paragraph bytes [p, p + n) */
    uint32_t b0;       /* TEXT: the box byte of paragraph byte p */
    uint64_t k1, k2;   /* TEXT: the 128-bit content key of its shaped run */
    float wmin, wmax;  /* intrinsic contributions (the prefix sums of the paragraph) */
    float kern;        /* TEXT: the kerning with the next text box of the same font (it ends this item), see pair_kern */
    uint32_t tab0, tabn; /* TEXT: its entries in ptab (the advance up to each candidate inside it) */
    float w;           /* line width: OPEN: start margin, border and padding; CLOSE: the end ones;
                        * ATOM: the margin box; TEXT outside: the hanging width */
    float ml, mr;      /* OPEN and CLOSE: the start and end margins (by the box's direction, so
                        * ml is the left margin in ltr and the right one in rtl); ATOM: the
                        * physical left and right margins as laid out */
    float bl, br;      /* OPEN and CLOSE: border and padding on the start and end sides */
    bool rtl;          /* OPEN and CLOSE: the box's direction is rtl */
    float a, d;        /* ascent and descent: with the half-leading of line-height (OPEN, TEXT), the
                        * margin box (ATOM) */
    float ca, cd;      /* OPEN: the content area (the font's ascent and descent without leading), which
                        * text-top and text-bottom align to and the box fragment spans */
    float xh;          /* OPEN: the x-height of its font (its children's middle alignment) */
    float fa;          /* ATOM: the baseline offset from the border box top */
    float rel;         /* OPEN and ATOM: the baseline shift relative to the parent's baseline */
    embi_lres lr;      /* ATOM: the layout result */
    embi_frag *frag;   /* ATOM: its fragment, laid out with the box */
    int32_t close;     /* OPEN: the index of its CLOSE item */
    int32_t parent;    /* the enclosing OPEN item, -1 for the container */
} ifc_item;

typedef struct ifc_cand {
    uint32_t pos;      /* a line may end before byte pos */
    uint8_t flags;     /* IFC_BK_* after the CSS modes */
    float hw;          /* after a soft hyphen: the width of the hyphen shown when the line ends here */
    const embi_style *hs; /* after a soft hyphen: the style of the hyphen */
    /* The width up to the candidate, and the collapsible spaces that hang there (computed once per
     * paragraph, kept with the box tree): cand_w then needs no measuring (W3p). */
    float pw, hang;
    uint32_t hs0;      /* where the hanging spaces start (pos when there are none) */
    bool okc;          /* pw, hang and hs0 are valid */
} ifc_cand;

static inline ifc_cand mk_cand(uint32_t pos, uint8_t flags, float hw, const embi_style *hs)
{
    ifc_cand c;
    memset(&c, 0, sizeof c);
    c.pos = pos;
    c.flags = flags;
    c.hw = hw;
    c.hs = hs;
    return c;
}

/* One unit of a line, in logical order. */
typedef struct ifc_unit {
    uint8_t kind;      /* UK_* */
    uint8_t level;     /* bidi level */
    bool space;        /* TEXT: a single space (a justification opportunity) */
    bool tab;          /* TEXT: a preserved tab, whose width the line gives it (resolve_tabs) */
    bool bare;         /* TEXT: the block overflow ellipsis: inside no inline box piece of the line */
    uint8_t mirror;    /* OPEN, CLOSE: an edge of a box whose direction has the other parity than its text: 1 both edges on the line, 2 one (mirror_edges) */
    int32_t item;      /* the item (-1 for the ellipsis) */
    embi_box *box;
    embi_lay_text *lt;
    uint32_t b0, b1;   /* TEXT: the box bytes */
    float a, d;        /* TEXT: the ascent and descent of its font, with the half-leading */
    float ca, cd;      /* TEXT: the content area (the font's rounded ascent and descent) its fragment spans */
    float w;           /* the advance (ATOM: the margin box) */
    float dx;          /* TEXT: where its glyphs start relative to x (text-spacing-trim of a line's opening punctuation) */
    float x;           /* the position in the line (content box x), set by the placement */
    float B;           /* the baseline shift (up is positive), set by the vertical placement */
    int32_t rec;       /* OPEN: its piece record */
} ifc_unit;

/* A piece of an inline box on the line: one BOX fragment, from its extent on the line. The records
 * of the boxes open at the line's start come first (ifc_line.ncont of them). */
typedef struct ifc_rec {
    int32_t item;      /* the OPEN item */
    float B, a, d;     /* the baseline shift and the content area (the font's, without leading) */
    float el, er;      /* the extent in the content box (a line in logical order: its one run) */
    uint32_t r0, rn;   /* its visual runs: ln.runs[r0, r0 + rn), one BOX fragment each */
    bool has;          /* some unit lies inside it on this line */
    bool closed;       /* its end edge is on this line */
} ifc_rec;

/* A visual run of a piece: the content-box extent of units that are next to each other on the line. */
typedef struct ifc_run {
    float l, r;
} ifc_run;

/* The extent a unit gives to a piece, with the unit's place in the visual order. */
typedef struct ifc_mem {
    uint32_t rec, v;
    float l, r;
} ifc_mem;

/* A line under construction. */
typedef struct ifc_line {
    uint32_t s, e;     /* the paragraph range */
    uint32_t j0, j1;   /* the item range */
    uint32_t jn;       /* the end of the items the line is responsible for: j1, and with an ellipsis
                        * also the clipped items up to the next line's first item */
    bool forced;       /* ends at a forced break */
    bool last;         /* the paragraph ends here */
    bool bell_only;    /* the block overflow ellipsis displaces the whole content: the line has the root's strut only */
    bool any;          /* the line has content (it is created) */
    bool justify;      /* text-align resolves to justify here */
    uint8_t align;     /* the resolved alignment: 0 start-left, 1 right, 2 center */
    float lb, rb;      /* the band the line may use, in the content box */
    float lb0, rb0;    /* the band before the floats met in this line were placed (outside markers hang from it) */
    float indent;      /* text-indent of this line */
    float cwc;         /* the content width (after justification) */
    float hw_s, hw_t;  /* what hangs at the end (line_hang): separators, and conditionally hanging spaces */
    bool tab_hangs;    /* a preserved tab hangs at the end: nothing of the line is justified */
    float H;           /* the line box height */
    float base;        /* the baseline, from the line top */
    uint32_t ncont;    /* the first records are the boxes open at the start */
    EMBI_VEC(ifc_unit) u;
    EMBI_VEC(uint32_t) ord;
    EMBI_VEC(ifc_rec) recs;
    EMBI_VEC(ifc_run) runs;
    EMBI_VEC(ifc_mem) mem;
} ifc_line;

/* The font metrics of a style, kept per style pointer: the text, the boxes and the hyphens of a
 * paragraph share a few styles, and the lookups go through the font database. */
typedef struct ifc_met {
    const embi_style *s;  /* the key (nullptr: empty) */
    float a, d;           /* ascent and descent with the half-leading of line-height */
    float ca, cd;         /* the content area: the font's rounded ascent and descent */
    float xh, hw;         /* the x-height and the width of a hyphen, valid with has_xh and has_hw */
    bool has_xh, has_hw, has_ad;
} ifc_met;

#define IFC_MET_N 16

/* An out-of-flow child of the inline content with its paragraph position. Its static position is
 * set by the line it falls in (abs_static, W3d). */
typedef struct ifc_abs {
    embi_box *box;
    uint32_t p;
    bool done;
} ifc_abs;

typedef struct ifc_prep ifc_prep;

/* A float laid out but not yet placed: it did not fit on the line of its position (CSS 2.1 section 9.5.1: a float
 * that does not fit beside the line's content goes below the line). */
typedef struct ifc_defer {
    embi_box *box;
    embi_frag *frag;
    embi_float_geom g;
} ifc_defer;

typedef struct ifc_st {
    embi_lctx *c;
    ifc_met met[IFC_MET_N];
    embi_box *root;
    const embi_style *rs;  /* the container's style */
    bool layout;           /* false: only the intrinsic widths are needed */
    float cb_h;            /* the height of the block container (percentage base of atomic inlines), or NaN */
    float cw;              /* the containing block's content width */
    uint8_t base;          /* the paragraph's base level: 0 ltr, 1 rtl */
    bool any_float;
    bool after_forced;     /* the line being laid out follows a forced break (text-indent: each-line) */
    float a0, d0, xh0;     /* the container's strut */
    float ca0, cd0;        /* the container's content area (its font's ascent and descent) */
    embi_sb buf;           /* the paragraph bytes */
    embi_lay_text *para;   /* the paragraph's analysis (nullptr without bytes) */
    embi_lay_tent *para_ent; /* its cache entry */
    embi_lay_text *para_alt[4]; /* with spans of other line-break modes: the analysis of each effective mode (loose, normal, strict), else nullptr */
    ifc_prep *pc;            /* the cached analysis in use (nullptr when the paragraph is not cached) */
    uint32_t lv_lo, lv_hi; /* the byte range of the run level_at looked up last, and its level */
    uint8_t lv_v;
    embi_box *ell_box;     /* the ellipsis of text-overflow, made when needed */
    embi_box *bell_box;    /* the block overflow ellipsis of a line clamp (block-ellipsis), made when needed */
    bool clamp_last;       /* the line being laid out is the last one a line clamp keeps: it takes the block overflow ellipsis */
    embi_frag *root_frag_bf; /* the fragment that takes the line boxes and the floats */
    float balance_w;         /* the width the lines may take (text-wrap: balance narrows it) */
    bool use_ends;           /* text-wrap: pretty: lay the lines out at the ends in ends */
    EMBI_VEC(uint32_t) ends; /* the line ends of the pretty layout, the last one the paragraph end */
    uint32_t nlines;         /* the lines created so far */
    uint32_t li;             /* the lines laid out so far, including those without content (index of ends) */
    EMBI_VEC(ifc_item) it;
    EMBI_VEC(ifc_cand) cand;
    EMBI_VEC(float) cmn;    /* the prefix sums of the minimum widths: cmn[i] sums the items before i */
    EMBI_VEC(float) cmx;    /* the prefix sums of the maximum widths */
    EMBI_VEC(ifc_ptab) ptab; /* the in-item prefix advances at the candidates, per text item */
    EMBI_VEC(int32_t) open; /* the OPEN items of the inline boxes open at the next line's start */
    EMBI_VEC(ifc_abs) abs;  /* the out-of-flow children (layout only) */
    EMBI_VEC(ifc_defer) defer; /* floats that did not fit on their line: placed at the top of the next one */
    ifc_line ln;
    embi_ifc_slot *slots;    /* the scratch buffers taken from the pool of the layout, or nullptr */
    bool has_tab;            /* a text box has a preserved tab: widths of ranges are measured with the tab stops (tabbed_w) */
    bool nocache;            /* the analysis depends on the layout (atomic inlines, floats, out-of-flow boxes,
                              * percentages, soft hyphens): it is not kept between layouts */
    uint8_t fwp;             /* W3q: 0 not scanned, 1 the paragraph has no byte 0xE3 or 0xEF (so no fullwidth punctuation for
                              * text-spacing-trim), 2 it has one */
    const embi_box *ti_box;  /* W3q: the box whose text-indent line_indent last read, and what it read */
    float ti_v;
    bool ti_hang, ti_each, ti_apply;
} ifc_st;

/* The width-independent analysis of a simple paragraph (W3p, W3q): the break candidates, the advance tables
 * and the paragraph's shaped analysis. The items themselves are built again by every layout (cheap); the
 * numbers that need the shaper's break data are not. An entry is keyed by the content of the paragraph (k1
 * and k2, see prep_sig), not by box, so it survives a rebuild of the box tree: the layout cache `pmap` owns
 * it and drops the ones a rebuilding layout did not use. */
struct ifc_prep {
    struct ifc_prep *chain; /* the entries with the same k1 */
    embi_lay_tent *pt;      /* the cache entry of `para` (its generation is refreshed when this entry is used) */
    uint64_t k1, k2;
    size_t sz;
    uint32_t gen;           /* the layout that used it last */
    uint32_t nit, ncand, nptab, buflen;
    bool lv_whole;       /* the paragraph is a single bidi run: lv is the level of every byte */
    uint8_t lv;
    bool have_int;       /* i_mn and i_mx are valid: the widest segments, computed by the first intrinsic query */
    float i_mn0, i_mx0;  /* the first run and the first segment (the only ones text-indent widens) */
    float i_mn, i_mx;    /* the minimum and maximum widths before text-indent and the clamps (embi_lay_ifc_intrinsic) */
    embi_lay_text *para;
    ifc_cand *cand;      /* the candidates as ifc_cand (no soft hyphens: hw 0, hs null; okc set) */
    float *cum;          /* 2 * (nit + 1): cmn then cmx */
    ifc_ptab *ptab;
    uint32_t *tab;       /* 2 * nit: tab0, tabn of each item */
};

#define SCR_LIST(X)                                                                            \
    X(0, x->it) X(1, x->cand) X(2, x->cmn) X(3, x->cmx) X(4, x->ptab) X(5, x->open)            \
    X(6, x->ends) X(7, x->abs) X(8, x->ln.u) X(9, x->ln.ord) X(10, x->ln.recs)                 \
    X(11, x->ln.runs) X(12, x->ln.mem)

/* Gives x the scratch buffers of the next free pool depth (empty, with the capacity they grew to). */
static void scr_take(ifc_st *x)
{
    embi_lctx *c = x->c;
    if (c->ld->ifc_depth >= EMBI_IFC_POOL_DEPTH) {
        return;
    }
    embi_ifc_slot *s = c->ld->ifc_pool[c->ld->ifc_depth++];
    x->slots = s;
#define SCR_T(i, v) (v).data = s[i].data; (v).cap = s[i].cap; (v).len = 0;
    SCR_LIST(SCR_T)
#undef SCR_T
    x->buf.data = s[13].data;
    x->buf.cap = s[13].cap;
    x->buf.len = 0;
}

/* Gives the buffers back to the pool, or frees them when x took none. */
static void scr_give(ifc_st *x)
{
    if (x->slots == nullptr) {
#define SCR_F(i, v) embi_vec_free(&(v));
        SCR_LIST(SCR_F)
#undef SCR_F
        embi_sb_free(&x->buf);
        return;
    }
    embi_ifc_slot *s = x->slots;
#define SCR_B(i, v) s[i].data = (v).data; s[i].cap = (v).cap;
    SCR_LIST(SCR_B)
#undef SCR_B
    s[13].data = x->buf.data;
    s[13].cap = x->buf.cap;
    x->c->ld->ifc_depth--;
}

void embi_lay_ifc_pool_free(embi_lay_doc *ld)
{
    for (uint32_t d = 0; d < EMBI_IFC_POOL_DEPTH; d++) {
        ifc_st t;
        memset(&t, 0, sizeof t);
        ifc_st *x = &t;
        embi_ifc_slot *s = ld->ifc_pool[d];
#define SCR_T2(i, v) (v).data = s[i].data; (v).cap = s[i].cap; (v).len = 0; embi_vec_free(&(v));
        SCR_LIST(SCR_T2)
#undef SCR_T2
        x->buf.data = s[13].data;
        x->buf.cap = s[13].cap;
        x->buf.len = 0;
        embi_sb_free(&x->buf);
        memset(s, 0, sizeof(embi_ifc_slot) * EMBI_IFC_SLOTS);
    }
    ld->ifc_depth = 0;
}

/* The bytes the scratch pool holds (the capacities the vectors grew to), for embi_layout_cache_bytes. */
size_t embi_lay_ifc_pool_bytes(const embi_lay_doc *ld)
{
    size_t n = 0;
    for (uint32_t d = 0; d < EMBI_IFC_POOL_DEPTH; d++) {
        ifc_st t;
        memset(&t, 0, sizeof t);
        ifc_st *x = &t;
        const embi_ifc_slot *s = ld->ifc_pool[d];
#define SCR_T3(i, v) (v).data = s[i].data; n += (size_t)s[i].cap * sizeof(*(v).data);
        SCR_LIST(SCR_T3)
#undef SCR_T3
        n += s[13].cap;
    }
    return n;
}

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

/* The cache slot of a style, without the line metrics. */
static ifc_met *met_slot(ifc_st *x, const embi_style *s)
{
    ifc_met *m = &x->met[(((uintptr_t)s >> 4) * 2654435761u >> 7) % IFC_MET_N];
    if (m->s != s) {
        memset(m, 0, sizeof *m);
        m->s = s;
    }
    return m;
}

/* The metrics of a style (cached; computed on first use, so the intrinsic-size path skips them). */
static ifc_met *metrics(ifc_st *x, const embi_style *s)
{
    ifc_met *m = met_slot(x, s);
    if (m->has_ad) {
        return m;
    }
    m->has_ad = true;
    const embi_lay_tkey *k = embi_lay_tkey_of(x->c, s);
    if (k != nullptr && k->has_lm) {
        m->a = k->lm_a;
        m->d = k->lm_d;
        m->ca = k->lm_ca;
        m->cd = k->lm_cd;
        return m;
    }
    embi_lay_line_metrics(x->c, s, &m->a, &m->d);
    /* The content area of CSS 2.1 10.6.1 is the font's ascent and descent, rounded as the normal
     * line-height computation does (line.c), without the half-leading. */
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(s, &ts, fams, 16);
    float asc = 0.8f * ts.size, desc = 0.2f * ts.size, gap = 0;
    (void)embi_tshim_face_metrics(x->c->fonts, &ts, &asc, &desc, &gap);
    m->ca = roundf(asc);
    m->cd = roundf(desc);
    if (k != nullptr && embi_lay_lh_static(s)) {
        embi_lay_tkey *wk = (embi_lay_tkey *)(uintptr_t)k; /* the cache slot of this layout */
        wk->has_lm = true;
        wk->lm_a = m->a;
        wk->lm_d = m->d;
        wk->lm_ca = m->ca;
        wk->lm_cd = m->cd;
    }
    return m;
}

/* The x-height of a style's font (cached). */
static float met_xh(ifc_st *x, const embi_style *s)
{
    ifc_met *m = met_slot(x, s);
    if (!m->has_xh) {
        embi_lay_tkey *k = (embi_lay_tkey *)(uintptr_t)embi_lay_tkey_of(x->c, s);
        if (k != nullptr && k->has_xh) {
            m->xh = k->xh;
        } else {
            embi_str fams[16];
            embi_lay_tstyle ts;
            embi_lay_text_style(s, &ts, fams, 16);
            m->xh = embi_tshim_x_height(x->c->fonts, &ts);
            if (k != nullptr) {
                k->xh = m->xh;
                k->has_xh = true;
            }
        }
        m->has_xh = true;
    }
    return m->xh;
}

/* The keyword of a property of a style, or 0. */
static uint16_t kwv(const embi_style *s, embi_prop_id p)
{
    const embi_value *v = embi_style_value(s, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* The break modes of a style, as the values of text.h. */
static uint8_t word_break_mode(const embi_style *s)
{
    uint16_t k = kwv(s, EMBI_PROP_word_break);
    return k == EMBI_KW_break_all ? 1 : (k == EMBI_KW_keep_all ? 2 : 0);
}

static uint8_t overflow_wrap_mode(const embi_style *s)
{
    uint16_t k = kwv(s, EMBI_PROP_overflow_wrap);
    if (k == EMBI_KW_anywhere) {
        return 2;
    }
    if (k == EMBI_KW_break_word) {
        return 1;
    }
    /* word-break: break-word is word-break: normal with overflow-wrap: anywhere (CSS Text 3 section 5.2). */
    return kwv(s, EMBI_PROP_word_break) == EMBI_KW_break_word ? 2 : 0;
}

static uint8_t line_break_mode(const embi_style *s);

/* The effective Japanese line-break style of a line_break_mode: 0 normal (auto, normal, anywhere), 1 loose, 2 strict. */
static uint8_t lb_eff(uint8_t m)
{
    return m == 1 ? 1 : m == 3 ? 2 : 0;
}

static uint8_t line_break_mode(const embi_style *s)
{
    switch (kwv(s, EMBI_PROP_line_break)) {
    case EMBI_KW_loose:
        return 1;
    case EMBI_KW_normal:
        return 2;
    case EMBI_KW_strict:
        return 3;
    case EMBI_KW_anywhere:
        return 4;
    default:
        return 0;
    }
}

static bool no_wrap(const embi_style *s)
{
    return kwv(s, EMBI_PROP_text_wrap_mode) == EMBI_KW_nowrap;
}

/* Whether the collapsible spaces that end a line of this style hang (CSS Text 3 section 4.1.3). */
static bool hangs(const embi_style *s)
{
    uint8_t m = embi_lay_ws_mode(s);
    if (m == EMBI_WS_BREAK_SPACES) {
        return false;
    }
    if (m == EMBI_WS_PRESERVE || m == EMBI_WS_PRESERVE_SPACES) {
        return !no_wrap(s);
    }
    return true;
}

/* Whether the collapsible spaces at the start of a line of this style are removed. */
static bool strips_start(const embi_style *s)
{
    uint8_t m = embi_lay_ws_mode(s);
    return m == EMBI_WS_COLLAPSE || m == EMBI_WS_DISCARD || m == EMBI_WS_PRESERVE_BREAKS;
}

/* ------------------------------------------------------------------ measuring */

/* The advance of the box bytes [a, b) of a shaped run. The line feeds and soft hyphens in them take
 * no room (a soft hyphen shows only when a line breaks at it). */
static float tw_raw(const embi_lay_text *lt, const embi_box *k, uint32_t a, uint32_t b)
{
    if (lt == nullptr || b <= a) {
        return 0.0f;
    }
    if (a == 0 && b == k->text_len && k->adv > 0.0f) {
        return k->adv; /* measured by an earlier layout of the kept box tree */
    }
    const char *t = k->text;
    float sum = 0.0f;
    uint32_t seg = a;
    if (a == 0 && b == k->text_len && b == embi_tshim_len(lt) && b < 4096) {
        /* The whole run: the shaped advance, unless line feeds or soft hyphens must be left out. */
        uint32_t i = 0;
        while (i < b && t[i] != '\n' && (uint8_t)t[i] != 0xC2) {
            i++;
        }
        if (i == b) {
            return embi_tshim_advance(lt);
        }
    }
    for (uint32_t i = a; i < b; i++) {
        if (t[i] == '\n') {
            sum += embi_tshim_measure(lt, seg, i);
            seg = i + 1;
        } else if ((uint8_t)t[i] == 0xC2 && i + 1 < b && (uint8_t)t[i + 1] == 0xAD) {
            sum += embi_tshim_measure(lt, seg, i);
            seg = i + 2;
            i++;
        }
    }
    sum += embi_tshim_measure(lt, seg, b);
    return sum;
}

/* The letter-spacing of a text box in px (0 when none). */
static float ls_px(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_letter_spacing);
    return embi_lay_spacing_px(v, embi_lay_font_px_style(s));
}

/* Whether a box has something on a line: text, an atomic inline or a replaced box (a float or an out-of-flow box has not). */
static bool has_content(const embi_box *s)
{
    if ((s->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) != 0 || s->kind == EMBI_BK_BR) {
        return false;
    }
    if (embi_box_is_text(s)) {
        return s->text_len > 0;
    }
    if (s->kind != EMBI_BK_INLINE) {
        return true;
    }
    for (const embi_box *c = s->first; c != nullptr; c = c->next) {
        if (has_content(c)) {
            return true;
        }
    }
    return false;
}

/* The letter-spacing after the last character of a text box: the spacing is given by the closest common ancestor
 * of the character and the next one on the line (CSS Text 4 section 8.2), and none at the end of a line. */
static float ls_tail(const embi_box *k)
{
    for (const embi_box *n = k; n->parent != nullptr; n = n->parent) {
        for (const embi_box *s = n->next; s != nullptr; s = s->next) {
            if (s->kind == EMBI_BK_BR) {
                return 0.0f;
            }
            if (has_content(s)) {
                return ls_px(n->parent->style);
            }
        }
        if (n->parent->kind != EMBI_BK_INLINE) {
            return 0.0f;
        }
    }
    return 0.0f;
}

/* Whether the character of a text box that ends at byte b is in a cursive script (no letter-spacing there). */
static bool cursive_before(const embi_box *k, uint32_t b)
{
    if (b == 0 || b > k->text_len || (uint8_t)k->text[b - 1] < 0x80) {
        return false; /* ASCII (the last byte of a longer character is a continuation byte, 0x80 and up) */
    }
    uint32_t a = b - 1;
    while (a > 0 && ((uint8_t)k->text[a] & 0xC0) == 0x80) {
        a--;
    }
    const char *p = k->text + a;
    return embi_tshim_no_ls(embi_utf8_decode(&p, k->text + b));
}

/* The letter-spacing after the character of a text box that ends at byte b. */
static float ls_end(const embi_box *k, uint32_t b)
{
    if (cursive_before(k, b)) {
        return 0.0f;
    }
    return b == k->text_len && (k->flags2 & EMBI_BF2_LSTRIM) != 0 ? ls_tail(k) : ls_px(k->style);
}

/* tw_raw with the letter-spacing after the last character of the box replaced by its tail spacing. */
static float tw(const embi_lay_text *lt, const embi_box *k, uint32_t a, uint32_t b)
{
    float w = tw_raw(lt, k, a, b);
    if (w != 0.0f && b == k->text_len && (k->flags2 & EMBI_BF2_LSTRIM) != 0 && !cursive_before(k, b)) {
        w += ls_tail(k) - ls_px(k->style);
    }
    return w;
}

/* The UTF-8 length of the character that starts with byte c. */
static uint32_t utf8_len(uint8_t c)
{
    return c < 0x80 ? 1u : (c < 0xE0 ? 2u : (c < 0xF0 ? 3u : 4u));
}

/* The advance of the box bytes [a, b) of a text item: tw, and the kerning with the next text box when the range
 * ends the box (the font kerns the last character of this box with the first of the next, as it does inside one
 * text run: CSS Text 3 section 7.3, shaping across inline box boundaries; css-content/quotes-001). */
static float twi(const ifc_item *t, uint32_t a, uint32_t b)
{
    float w = tw(t->lt, t->box, a, b);
    if (t->kern != 0.0f && b > a && b == t->box->text_len) {
        w += t->kern;
    }
    return w;
}

/* The kerning between the last character of text item p and the first of box k, when they have the same shaping
 * style: advance(AB) - advance(A) - advance(B), 0 when the pair does not kern. Only scripts without complex
 * shaping (below U+0590) take part. */
static float pair_kern(ifc_st *x, const ifc_item *p, const embi_box *k)
{
    const embi_box *pb = p->box;
    if (pb->text == nullptr || k->text == nullptr || pb->text_len == 0 || k->text_len == 0) {
        return 0.0f;
    }
    const embi_lay_tkey *k1 = embi_lay_tkey_of(x->c, pb->style);
    const embi_lay_tkey *k2 = embi_lay_tkey_of(x->c, k->style);
    if (k1 == nullptr || k2 == nullptr || (k1 != k2 && (k1->h != k2->h || k1->h2 != k2->h2))) {
        return 0.0f;
    }
    embi_str l1 = embi_lay_lang(x->c, pb), l2 = embi_lay_lang(x->c, k);
    /* A plain text box carries no language of its own (embi_lay_lang: no element), a generated one does: only two
     * different languages stop the pairing. */
    if (l1.len != 0 && l2.len != 0 && (l1.len != l2.len || memcmp(l1.ptr, l2.ptr, l1.len) != 0)) {
        return 0.0f;
    }
    if (l1.len == 0) {
        l1 = l2;
    }
    uint32_t i = pb->text_len;
    while (i > 0 && ((uint8_t)pb->text[i - 1] & 0xC0u) == 0x80u) {
        i--;
    }
    if (i == 0) {
        return 0.0f;
    }
    i--;
    uint32_t al = pb->text_len - i;
    uint32_t bl = utf8_len((uint8_t)k->text[0]);
    if (al > 4 || bl > k->text_len) {
        return 0.0f;
    }
    const char *ap = pb->text + i, *aend = pb->text + pb->text_len;
    const char *bp = k->text, *bend = k->text + bl;
    uint32_t ca = embi_utf8_decode(&ap, aend), cb = embi_utf8_decode(&bp, bend);
    /* Line feeds, tabs, soft hyphens, scripts that shape in context (from U+0590 up) except general punctuation. */
    bool a_ok = ca >= 0x20 && ca != 0xAD && (ca < 0x590 || (ca >= 0x2000 && ca <= 0x206F));
    bool b_ok = cb >= 0x20 && cb != 0xAD && (cb < 0x590 || (cb >= 0x2000 && cb <= 0x206F));
    if (!a_ok || !b_ok) {
        return 0.0f;
    }
    /* A small cache of the pairs seen (a document repeats the same few boundary pairs thousands of times). The key holds the
     * two style hashes, the two characters and the language length and first bytes; the kerning is a pure function of them. */
    typedef struct kcache {
        uint64_t h, h2;
        uint32_t ca, cb, lang;
        float kern;
        bool used;
    } kcache;
    static thread_local kcache kc[128];
    uint32_t lang = (uint32_t)l1.len << 16 | (l1.len > 0 ? (uint32_t)(uint8_t)l1.ptr[0] << 8 : 0u) |
                    (l1.len > 1 ? (uint32_t)(uint8_t)l1.ptr[1] : 0u);
    uint32_t slot = (uint32_t)((k1->h ^ (k1->h2 * 31u) ^ ((uint64_t)ca * 0x9E3779B1u) ^ ((uint64_t)cb * 0x85EBCA6Bu) ^ lang) & 127u);
    kcache *ke = &kc[slot];
    if (ke->used && ke->h == k1->h && ke->h2 == k1->h2 && ke->ca == ca && ke->cb == cb && ke->lang == lang) {
        return ke->kern;
    }
    char ab[8];
    memcpy(ab, pb->text + i, al);
    memcpy(ab + al, k->text, bl);
    embi_lay_text *ta = embi_lay_shape_text(x->c, k1, l1, 0, ab, al);
    embi_lay_text *tb = embi_lay_shape_text(x->c, k1, l1, 0, ab + al, bl);
    embi_lay_text *tab = embi_lay_shape_text(x->c, k1, l1, 0, ab, al + bl);
    if (ta == nullptr || tb == nullptr || tab == nullptr) {
        return 0.0f;
    }
    float aa = embi_tshim_advance(ta), bb = embi_tshim_advance(tb);
    float kern = embi_tshim_advance(tab) - aa - bb;
    if (fabsf(kern) < 0.005f || fabsf(kern) > 0.5f * fmaxf(aa, bb)) {
        kern = 0.0f;
    }
    *ke = (kcache){k1->h, k1->h2, ca, cb, lang, kern, true};
    return kern;
}

/* ------------------------------------------------------------------ building the paragraph */

static uint32_t plen(const ifc_st *x)
{
    return (uint32_t)x->buf.len;
}

static void put(ifc_st *x, const char *s, uint32_t n)
{
    embi_sb_append(&x->buf, (embi_str){s, n});
}

static int32_t push_item(ifc_st *x, const ifc_item *v)
{
    embi_vec_push(&x->it, *v);
    return (int32_t)(x->it.len - 1);
}

static void build_children(ifc_st *x, embi_box *p, int32_t parent);

/* The unicode-bidi kinds of an inline box (CSS Writing Modes 4 section 2.4). */
enum {
    BIDI_NONE = 0,
    BIDI_EMBED,
    BIDI_OVERRIDE,
    BIDI_ISOLATE,
    BIDI_ISOLATE_OVERRIDE,
    BIDI_PLAINTEXT,
};

/* A text box, or an outside marker (which has no bytes in the paragraph). */
/* Marks a text box whose last character gets a spacing other than its own letter-spacing (ls_tail). */
static void ls_mark(ifc_st *x, embi_box *k)
{
    k->flags2 = (uint16_t)(k->flags2 & ~EMBI_BF2_LSTRIM);
    if (k->kind != EMBI_BK_TEXT || k->parent == nullptr) {
        return;
    }
    if (ls_tail(k) != ls_px(k->style)) {
        k->flags2 |= EMBI_BF2_LSTRIM;
        x->nocache = true;
    }
}

static void add_text(ifc_st *x, embi_box *k, int32_t parent, bool outside)
{
    uint64_t r1 = 0, r2 = 0;
    /* Inside an inline box with unicode-bidi: bidi-override (or a block container with it) every character of the
     * text is strong in the override's direction, which the text's own shaping must know. */
    bool ovr = false;
    for (int32_t pi = parent; pi >= 0 && !ovr; pi = x->it.data[pi].parent) {
        ovr = x->it.data[pi].bm == BIDI_OVERRIDE || x->it.data[pi].bm == BIDI_ISOLATE_OVERRIDE;
    }
    embi_lay_text *lt = embi_lay_shape_k_ovr(x->c, k, &r1, &r2, ovr);
    if (lt == nullptr) {
        return;
    }
    if (outside) {
        x->nocache = true;
    }
    ifc_item v;
    memset(&v, 0, sizeof v);
    v.k1 = r1;
    v.k2 = r2;
    v.kind = IK_TEXT;
    v.box = k;
    v.lt = lt;
    v.parent = parent;
    if (x->layout) {
        const ifc_met *km = metrics(x, k->style);
        v.a = km->a;
        v.d = km->d;
        v.ca = km->ca;
        v.cd = km->cd;
    }
    ls_mark(x, k);
    k->adv = tw_raw(lt, k, 0, k->text_len);
    float w = tw(lt, k, 0, k->text_len);
    if (!outside) {
        /* The previous text item, when only inline box edges without room lie between: its last character kerns with
         * this box's first. */
        for (uint32_t j = x->it.len; j > 0; j--) {
            ifc_item *pt = &x->it.data[j - 1];
            if ((pt->kind == IK_OPEN || pt->kind == IK_CLOSE) && pt->w == 0.0f && pt->bm == 0) {
                /* A box that is not on the baseline breaks the shaping (CSS Text 3 section 7.3). */
                const embi_value *va = embi_style_value(pt->box->style, EMBI_PROP_vertical_align);
                if (va == nullptr || (va->kind == EMBI_V_KEYWORD && va->kw == EMBI_KW_baseline)) {
                    continue;
                }
                break;
            }
            if (pt->kind == IK_TEXT && !pt->outside && pt->n > 0 && pt->kern == 0.0f) {
                float kn = pair_kern(x, pt, k);
                if (kn != 0.0f) {
                    pt->kern = kn;
                    pt->wmin += kn;
                    pt->wmax += kn;
                }
            }
            break;
        }
    }
    v.p = plen(x);
    if (outside) {
        v.outside = true;
        v.n = 0;
        v.w = w;
        push_item(x, &v);
        return;
    }
    v.n = k->text_len;
    v.b0 = 0;
    v.wmin = v.wmax = w;
    if (memchr(k->text, 9, k->text_len) != nullptr) {
        x->has_tab = true;
        x->nocache = true;
    }
    put(x, k->text, k->text_len);
    push_item(x, &v);
}

/* A line feed for <br>: a forced break in the paragraph. */
static void add_br(ifc_st *x, embi_box *k, int32_t parent)
{
    ifc_item v;
    memset(&v, 0, sizeof v);
    v.kind = IK_BR;
    v.box = k;
    v.parent = parent;
    v.p = plen(x);
    v.n = 1;
    put(x, "\n", 1);
    push_item(x, &v);
}

/* An atomic inline: laid out at its shrink-to-fit width, or measured for the intrinsic sizes. A
 * hanging marker image has no bytes and no room in the line. */
static void add_atomic(ifc_st *x, embi_box *k, int32_t parent)
{
    x->nocache = true;
    ifc_item v;
    memset(&v, 0, sizeof v);
    v.kind = IK_ATOM;
    v.box = k;
    v.parent = parent;
    v.outside = (k->flags & EMBI_BF_OUTSIDE) != 0;
    if (x->layout) {
        embi_cs cs = {x->cw, x->cw, x->cb_h, EMBI_FIT_SHRINK, nullptr, 0, 0, false, 0, 0};
        embi_lres r;
        memset(&r, 0, sizeof r);
        embi_lay_box(x->c, k, &cs, &r);
        embi_frag *f = r.frag;
        if (f == nullptr) {
            return;
        }
        /* The baseline (CSS 2.1 10.8.1): the last line box's, or the bottom margin edge when there is
         * none or the box clips its overflow. */
        /* Negative vertical margins shrink the margin box that the line box has to contain (CSS 2.1 10.8.1, 10.6.2;
         * line-height-129). */
        float mt = isnan(r.mt) ? 0.0f : r.mt, mb = isnan(r.mb) ? 0.0f : r.mb;
        /* Inline flex, grid and table containers align on their FIRST baseline (CSS Align 3 section 9.1);
         * an inline-block on its last line box. */
        bool first_bl = k->kind == EMBI_BK_FLEX || k->kind == EMBI_BK_GRID || k->kind == EMBI_BK_TABLE;
        float kbl = first_bl ? f->bl_first : embi_fl(f)->bl_last;
        bool own = !isnan(kbl) && ((k->flags & EMBI_BF_CLIP) == 0 || (k->tflags & EMBI_TF_CTLBASE) != 0);
        float bl = own ? kbl : f->h + mb;
        v.lr = r;
        v.frag = f;
        v.fa = bl;
        v.ml = isnan(r.ml) ? 0.0f : r.ml; /* a negative margin pulls the next item closer */
        v.mr = isnan(r.mr) ? 0.0f : r.mr;
        v.a = bl + mt;
        v.d = f->h - bl + mb;
        v.w = v.ml + f->w + v.mr;
        if (!v.outside) {
            v.wmin = v.wmax = v.w;
        }
    } else {
        float mn = 0, mx = 0;
        embi_lay_intrinsic_outer(x->c, k, &mn, &mx);
        if (!v.outside) {
            v.wmin = mn;
            v.wmax = mx;
        }
    }
    v.p = plen(x);
    if (!v.outside) {
        v.n = 3;
        put(x, "\xEF\xBF\xBC", 3); /* U+FFFC */
    }
    push_item(x, &v);
}

/* An inside marker's text: with unicode-bidi: isolate (the UA style of ::marker, CSS Lists 3 section 3.2) it is isolated
 * from the item's text by an LRI or RLI and a PDI around it, in the marker's direction (list-style-type-string-006). */
static void add_inside_marker(ifc_st *x, embi_box *k, int32_t parent)
{
    const embi_lay_ssum *sm = embi_lay_ssum_of(x->c, k->style);
    bool iso = sm->ub == EMBI_KW_isolate;
    if (iso) {
        put(x, sm->rtl ? "\xE2\x81\xA7" : "\xE2\x81\xA6", 3);
    }
    add_text(x, k, parent, false);
    if (iso) {
        put(x, "\xE2\x81\xA9", 3);
    }
}

static void add_inline(ifc_st *x, embi_box *k, int32_t parent)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(x->c, k, x->cw, &bdr, &pad, &mar);
    /* The style-only facts of the box (position.c): the same for every box of the style. The box has its own
     * style, or the initial one when anonymous (it is an inline box, never anonymous). */
    const embi_lay_ssum sm = *embi_lay_ssum_of(x->c, k->style);
    if (!x->nocache && sm.edges_vary) {
        x->nocache = true;
    }

    uint16_t ub = sm.ub;
    bool rtl = sm.rtl;
    int mode = BIDI_NONE;
    switch (ub) {
    case EMBI_KW_embed:
        mode = BIDI_EMBED;
        break;
    case EMBI_KW_bidi_override:
        mode = BIDI_OVERRIDE;
        break;
    case EMBI_KW_isolate:
        mode = BIDI_ISOLATE;
        break;
    case EMBI_KW_plaintext:
        mode = BIDI_PLAINTEXT;
        break;
    case EMBI_KW_isolate_override:
        mode = BIDI_ISOLATE_OVERRIDE;
        break;
    default:
        break;
    }
    /* A box that opens no embedding (unicode-bidi: normal) does not reverse its text: an rtl box in ltr text keeps its
     * physical left edge on the left (an ltr box in rtl text keeps its own direction, its text being a run of its own). */
    if (mode == BIDI_NONE && rtl) {
        rtl = parent >= 0 ? x->it.data[parent].rtl : x->base == 1;
    }
    /* LRE U+202A, RLE U+202B, PDF U+202C, LRO U+202D, RLO U+202E, LRI U+2066, RLI U+2067, PDI
     * U+2069 (UTF-8 as written here). */
    const char *open_c = nullptr, *close_c = nullptr;
    switch (mode) {
    case BIDI_EMBED:
        open_c = rtl ? "\xE2\x80\xAB" : "\xE2\x80\xAA";
        close_c = "\xE2\x80\xAC";
        break;
    case BIDI_OVERRIDE:
        open_c = rtl ? "\xE2\x80\xAE" : "\xE2\x80\xAD";
        close_c = "\xE2\x80\xAC";
        break;
    case BIDI_ISOLATE:
    case BIDI_ISOLATE_OVERRIDE:
        open_c = rtl ? "\xE2\x81\xA7" : "\xE2\x81\xA6";
        close_c = "\xE2\x81\xA9";
        break;
    case BIDI_PLAINTEXT:
        /* FSI U+2068: the direction comes from the first strong character. */
        open_c = "\xE2\x81\xA8";
        close_c = "\xE2\x81\xA9";
        break;
    default:
        break;
    }
    if (open_c != nullptr) {
        put(x, open_c, 3);
    }
    if (mode == BIDI_ISOLATE_OVERRIDE) {
        put(x, rtl ? "\xE2\x80\xAE" : "\xE2\x80\xAD", 3);
    }

    /* The edges are the start and end ones of the box's direction: the start side is the left one
     * in ltr and the right one in rtl. The OPEN item holds the start edges, the CLOSE item the end. */
    ifc_item o;
    memset(&o, 0, sizeof o);
    o.kind = IK_OPEN;
    o.box = k;
    o.parent = parent;
    o.p = plen(x);
    o.rtl = rtl;
    o.bm = (uint8_t)mode;
    /* Inline box margins may be negative (CSS 2.1 10.3.1): they are kept, and move the box's content. An auto margin
     * (NaN in mar) is 0 on an inline box. */
    o.ml = rtl ? mar.r : mar.l;
    o.mr = rtl ? mar.l : mar.r;
    o.ml = isnan(o.ml) ? 0.0f : o.ml;
    o.mr = isnan(o.mr) ? 0.0f : o.mr;
    o.bl = rtl ? bdr.r + pad.r : bdr.l + pad.l;
    o.br = rtl ? bdr.l + pad.l : bdr.r + pad.r;
    if ((k->flags2 & EMBI_BF2_NOSTART) != 0) {
        o.ml = o.bl = 0.0f;
    }
    if ((k->flags2 & EMBI_BF2_NOEND) != 0) {
        o.mr = o.br = 0.0f;
    }
    o.w = o.ml + o.bl;
    o.wmin = o.wmax = o.w;
    if (x->layout) {
        o.xh = met_xh(x, k->style);
    }
    if (x->layout) {
        const ifc_met *om = metrics(x, k->style);
        o.a = om->a;
        o.d = om->d;
        o.ca = om->ca;
        o.cd = om->cd;
    }
    uint16_t va = sm.va;
    if (va == EMBI_KW_top) {
        o.va = VA_TOP;
    } else if (va == EMBI_KW_bottom) {
        o.va = VA_BOTTOM;
    }
    int32_t oi = push_item(x, &o);
    build_children(x, k, oi);

    ifc_item cl;
    memset(&cl, 0, sizeof cl);
    cl.kind = IK_CLOSE;
    cl.box = k;
    cl.parent = oi;
    cl.p = plen(x);
    cl.rtl = rtl;
    cl.bm = (uint8_t)mode;
    cl.ml = o.ml;
    cl.mr = o.mr;
    cl.bl = o.bl;
    cl.br = o.br;
    cl.w = cl.br + cl.mr;
    cl.wmin = cl.wmax = cl.w;
    int32_t ci = push_item(x, &cl);
    x->it.data[oi].close = ci;

    if (mode == BIDI_ISOLATE_OVERRIDE) {
        put(x, "\xE2\x80\xAC", 3);
    }
    if (close_c != nullptr) {
        put(x, close_c, 3);
    }
}

static void build_children(ifc_st *x, embi_box *p, int32_t parent)
{
    for (embi_box *k = p->first; k != nullptr; k = k->next) {
        if (k->next != nullptr && k->next->kind == EMBI_BK_TEXT) {
            __builtin_prefetch(k->next->text); /* the text of the next run: the first read of it is a cache miss */
        }
        if (k->flags & EMBI_BF_ABSPOS) {
            /* Out of flow: it takes no bytes and no item, so the analysis does not see it; its static
             * position is set by the line it falls in (abs_static). */
            if (x->layout) {
                ifc_abs a = {k, plen(x), false};
                embi_vec_push(&x->abs, a);
            }
            continue;
        }
        if (k->flags & EMBI_BF_FLOAT) {
            /* A float takes no bytes. Its intrinsic contribution counts in the intrinsic sizes (it
             * adds to the line it sits in); in layout it is placed beside the line. */
            x->nocache = true;
            ifc_item v;
            memset(&v, 0, sizeof v);
            v.kind = IK_FLOAT;
            v.box = k;
            v.parent = parent;
            v.p = plen(x);
            if (x->layout) {
                x->any_float = true;
            } else {
                embi_lay_intrinsic_outer(x->c, k, &v.wmin, &v.wmax);
            }
            push_item(x, &v);
            continue;
        }
        switch (k->kind) {
        case EMBI_BK_TEXT:
            add_text(x, k, parent, false);
            break;
        case EMBI_BK_MARKER:
            if (k->flags & EMBI_BF_OUTSIDE) {
                if (x->layout) {
                    add_text(x, k, parent, true);
                }
            } else {
                add_inside_marker(x, k, parent);
            }
            break;
        case EMBI_BK_BR:
            add_br(x, k, parent);
            break;
        case EMBI_BK_INLINE:
            add_inline(x, k, parent);
            break;
        default:
            if (!(k->flags & EMBI_BF_OUTSIDE) || x->layout) {
                add_atomic(x, k, parent);
            }
            break;
        }
    }
}

/* The paragraph's analysis: one shaping with the container's style, for the breaks and the levels. */
static void analyze(ifc_st *x)
{
    x->para = nullptr;
    x->para_ent = nullptr;
    x->lv_lo = x->lv_hi = 0;
    if (x->buf.len == 0) {
        return;
    }
    x->c->ld->shaping_para = true;
    uint8_t m0 = line_break_mode(x->rs);
    /* Spans with another line-break mode than the container's (loose, normal and strict differ for the Japanese breaks):
     * the paragraph is analysed once per effective mode, the break list of the loosest one (it has every break the
     * others have) and the flags of each mode from its own analysis. */
    uint8_t seen = (uint8_t)(1u << lb_eff(m0));
    for (uint32_t i = 0; i < x->it.len; i++) {
        if (x->it.data[i].kind == IK_TEXT && x->it.data[i].n > 0) {
            seen |= (uint8_t)(1u << lb_eff(line_break_mode(x->it.data[i].box->style)));
        }
    }
    memset(x->para_alt, 0, sizeof x->para_alt);
    if ((seen & (seen - 1u)) != 0) {
        x->nocache = true;
        for (uint8_t e = 0; e < 4; e++) {
            if ((seen & (1u << e)) != 0) {
                uint8_t lbm = e == 2 ? 3 : e == 1 ? 1 : 0; /* effective mode -> a line_break_mode value */
                embi_lay_tent *ent = nullptr;
                x->para_alt[e] = embi_lay_shape_text_ent(x->c, embi_lay_tkey_of(x->c, x->rs), embi_lay_lang(x->c, x->root), lbm,
                                                         x->buf.data, (uint32_t)x->buf.len, &ent);
            }
        }
        uint8_t tl = (seen & 2u) != 0 ? 1 : 0;
        x->para = embi_lay_shape_text_ent(x->c, embi_lay_tkey_of(x->c, x->rs), embi_lay_lang(x->c, x->root), tl, x->buf.data,
                                          (uint32_t)x->buf.len, &x->para_ent);
    } else {
        x->para = embi_lay_shape_text_ent(x->c, embi_lay_tkey_of(x->c, x->rs), embi_lay_lang(x->c, x->root), m0, x->buf.data,
                                          (uint32_t)x->buf.len, &x->para_ent);
    }
    x->c->ld->shaping_para = false;
}

static uint32_t level_at(const ifc_st *x, uint32_t pos)
{
    if (x->para == nullptr) {
        return x->base;
    }
    ifc_st *m = (ifc_st *)(uintptr_t)x;
    if (pos < m->lv_lo || pos >= m->lv_hi) {
        m->lv_v = embi_tshim_level_span(x->para, pos, &m->lv_lo, &m->lv_hi);
    }
    return m->lv_v;
}

/* The index of the first item after a break at pos: the items before it are on the earlier side.
 * Closing edges at pos stay on that side; the other items at pos go after it. */
static uint32_t split_of(const ifc_st *x, uint32_t pos)
{
    uint32_t n = x->it.len, lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (x->it.data[mid].p < pos) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    while (lo < n && x->it.data[lo].p == pos && x->it.data[lo].kind == IK_CLOSE) {
        lo++;
    }
    return lo;
}

/* The first item of a line that starts at pos: a text item that spans pos starts it too. */
static uint32_t start_of(const ifc_st *x, uint32_t pos)
{
    uint32_t j = split_of(x, pos);
    if (j > 0) {
        const ifc_item *t = &x->it.data[j - 1];
        if (t->kind == IK_TEXT && t->n > 0 && t->p < pos && pos < t->p + t->n) {
            return j - 1;
        }
    }
    return j;
}

/* The style of the character after pos, past the opening edges of the inline boxes that start there: it decides word-break,
 * overflow-wrap and line-break at pos, which are properties of the characters (a break-all span lets a word break before
 * its first letter), while white-space belongs to the box around the break (style_at). */
static const embi_style *style_chars_at(const ifc_st *x, uint32_t pos)
{
    uint32_t j = split_of(x, pos);
    for (uint32_t k = j; k < x->it.len && x->it.data[k].p == pos; k++) {
        const ifc_item *t = &x->it.data[k];
        if (t->kind == IK_TEXT && t->n > 0) {
            return t->box->style;
        }
        if (t->kind != IK_OPEN && t->kind != IK_CLOSE && t->kind != IK_FLOAT) {
            break;
        }
    }
    return nullptr;
}

/* The style that decides the breaks at pos: that of the text after it, else of the text before. */
static const embi_style *style_at(const ifc_st *x, uint32_t pos)
{
    uint32_t j = split_of(x, pos);
    if (j < x->it.len) {
        const ifc_item *t = &x->it.data[j];
        if (t->kind == IK_TEXT && t->n > 0 && t->p == pos) {
            return t->box->style;
        }
    }
    if (j > 0) {
        const ifc_item *t = &x->it.data[j - 1];
        if (t->kind == IK_TEXT && t->n > 0) {
            return t->box->style;
        }
    }
    return x->rs;
}

/* The break opportunities of the paragraph under the CSS modes (CSS Text 3 sections 5.2 to 5.5 and
 * 4.1.3 for the soft hyphen). The end of the paragraph is a soft break: it fits or overflows. */
/* The position after the next line feed at or after index from (its break), or UINT32_MAX. A line
 * feed is a mandatory break even when the shaper reports none there (kb's flags do not carry one
 * for every line feed of a multi-paragraph run). */
static uint32_t next_lf(const ifc_st *x, uint32_t from)
{
    uint32_t len = plen(x);
    for (uint32_t i = from; i < len; i++) {
        if (x->buf.data[i] == '\n') {
            return i + 1;
        }
    }
    return UINT32_MAX;
}

static void push_lf(ifc_st *x, uint32_t pos)
{
    ifc_cand c = mk_cand(pos, IFC_BK_HARD | IFC_BK_LINE, 0.0f, nullptr);
    embi_vec_push(&x->cand, c);
}

/* The width of the hyphen that shows when a line ends at a soft hyphen in style s (cached per style). */
static float hyphen_w(ifc_st *x, const embi_style *s)
{
    ifc_met *m = met_slot(x, s);
    if (m->has_hw) {
        return m->hw;
    }
    embi_lay_text *h = embi_lay_shape_text(x->c, embi_lay_tkey_of(x->c, s), (embi_str){"", 0}, 0, "-", 1);
    float w = h != nullptr ? embi_tshim_advance(h) : 0.0f;
    m->hw = w;
    m->has_hw = true;
    return w;
}

/* The style of the text that holds the byte before pos (the root's when none does). */
static const embi_style *style_before(const ifc_st *x, uint32_t pos)
{
    uint32_t j = split_of(x, pos);
    while (j > 0) {
        j--;
        const ifc_item *t = &x->it.data[j];
        if (t->kind == IK_TEXT && t->n > 0 && t->p < pos && pos <= t->p + t->n) {
            return t->box->style;
        }
    }
    return x->rs;
}

static void add_space_breaks(ifc_st *x);
static bool space_sep3(const uint8_t *p);

/* Whether the break after the space before pos may wrap although the text after it does not: the opportunity of a space
 * belongs to the box that holds the space (CSS Text 3 section 5.1), here also to one whose spaces collapsed into it. */
static bool space_wraps(const ifc_st *x, uint32_t pos)
{
    if (pos == 0 || x->buf.data[pos - 1] != ' ') {
        return false;
    }
    uint32_t j = split_of(x, pos);
    for (uint32_t k = j; k < x->it.len && x->it.data[k].p == pos; k++) {
        const ifc_item *t = &x->it.data[k];
        if (t->kind == IK_TEXT && t->n == 0 && !no_wrap(t->box->style)) {
            return true; /* a space that collapsed into the one before pos */
        }
        if (t->kind != IK_OPEN && t->kind != IK_CLOSE && !(t->kind == IK_TEXT && t->n == 0)) {
            break;
        }
    }
    while (j > 0) {
        j--;
        const ifc_item *t = &x->it.data[j];
        if (t->kind == IK_TEXT && t->n == 0 && t->p == pos) {
            if (!no_wrap(t->box->style)) {
                return true; /* a space that collapsed into the one before pos */
            }
            continue;
        }
        if (t->kind == IK_OPEN || t->kind == IK_CLOSE) {
            continue;
        }
        return t->kind == IK_TEXT && t->n > 0 && t->p < pos && pos <= t->p + t->n && !no_wrap(t->box->style);
    }
    return false;
}

/* An other space separator that is not a no-break space (U+2007 and U+202F glue): white-space: break-spaces
 * puts a soft wrap opportunity after it. */
static bool breaking_sep3(const uint8_t *p)
{
    return space_sep3(p) && !(p[0] == 0xE2u && p[1] == 0x80u && (p[2] == 0x87u || p[2] == 0xAFu));
}

/* The break opportunities of the paragraph under the CSS modes (CSS Text 3 sections 5.2 to 5.5 and
 * 4.1.3 for the soft hyphen). A line feed is a forced break after it. The end of the paragraph is a
 * soft break: it fits or it overflows. */
static void build_cands(ifc_st *x)
{
    uint32_t len = plen(x);
    uint32_t nlf = next_lf(x, 0);
    if (x->para != nullptr) {
        uint32_t nb = embi_tshim_break_count(x->para);
        /* Whether any text of the paragraph has an overflow-wrap other than normal: then a break between such a text
         * and the next one (of another style) is an emergency opportunity too. */
        bool any_ow = false;
        for (uint32_t i = 0; i < x->it.len && !any_ow; i++) {
            const ifc_item *t = &x->it.data[i];
            any_ow = t->kind == IK_TEXT && t->n > 0 && overflow_wrap_mode(t->box->style) != 0;
        }
        /* The modes of the style of the last candidate: most paragraphs have one style at a time. */
        const embi_style *ms = nullptr, *mc = nullptr;
        uint8_t m_ow = 0, m_wb = 0, m_lb = 0;
        bool m_nw = false;
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t off = embi_tshim_break_offset(x->para, i);
            while (nlf < off) {
                push_lf(x, nlf);
                nlf = next_lf(x, nlf);
            }
            bool lf_here = nlf == off;
            if (lf_here) {
                nlf = next_lf(x, off);
            }
            if (off == 0) {
                continue;
            }
            if (off >= len) {
                if (lf_here) {
                    push_lf(x, off);
                }
                continue;
            }
            const embi_style *s = style_at(x, off);
            const embi_style *sc = style_chars_at(x, off);
            if (sc == nullptr) {
                sc = s;
            }
            if (s != ms || sc != mc) {
                ms = s;
                mc = sc;
                m_ow = overflow_wrap_mode(sc);
                m_wb = word_break_mode(sc);
                m_lb = line_break_mode(sc);
                m_nw = no_wrap(s);
            }
            uint8_t ow = m_ow;
            if (ow == 0 && any_ow) {
                ow = overflow_wrap_mode(style_before(x, off));
            }
            const embi_lay_text *pm = x->para_alt[lb_eff(m_lb)] != nullptr ? x->para_alt[lb_eff(m_lb)] : x->para;
            uint8_t fl = embi_tshim_break_flags(pm, off, m_wb, ow, m_lb);
            if (lf_here) {
                fl |= IFC_BK_HARD | IFC_BK_LINE;
            }
            if (ow == 2 && (fl & IFC_BK_WORD) != 0) {
                fl |= IFC_BK_MINC; /* anywhere: its breaks are emergency ones, soft for min-content only */
            }
            /* A soft hyphen breaks only when hyphens is not none; the line then shows a hyphen in
             * the style of the text that holds the soft hyphen. */
            bool shy = off >= 2 && (uint8_t)x->buf.data[off - 2] == 0xC2 &&
                       (uint8_t)x->buf.data[off - 1] == 0xAD && !lf_here;
            const embi_style *hs = nullptr;
            float hw = 0.0f;
            if (shy) {
                hs = style_before(x, off);
                if (kwv(hs, EMBI_PROP_hyphens) == EMBI_KW_none) {
                    fl = 0;
                } else {
                    hw = hyphen_w(x, hs);
                }
            }
            if (m_nw && !lf_here && !space_wraps(x, off)) {
                fl &= IFC_BK_HARD;
            }
            if (fl != 0) {
                ifc_cand c = mk_cand(off, fl, hw, hs);
                embi_vec_push(&x->cand, c);
            }
        }
    }
    while (nlf <= len && nlf != UINT32_MAX) {
        push_lf(x, nlf);
        nlf = next_lf(x, nlf);
    }
    /* The end of the paragraph: a soft end, which fits or overflows like the others (a line feed at
     * the end already gave a forced one). */
    ifc_cand end = mk_cand(len, IFC_BK_LINE, 0.0f, nullptr);
    if (x->cand.len > 0 && x->cand.data[x->cand.len - 1].pos == len) {
        x->cand.data[x->cand.len - 1].flags |= IFC_BK_LINE;
    } else {
        embi_vec_push(&x->cand, end);
    }
    add_space_breaks(x);
}

/* white-space: break-spaces puts a soft wrap opportunity after every preserved space (CSS Text 3 section
 * 4.1.3), also between two spaces, where UAX 14 has none: merges them into the candidates. */
static void add_space_breaks(ifc_st *x)
{
    uint32_t len = plen(x);
    EMBI_VEC(uint32_t) extra;
    memset(&extra, 0, sizeof extra);
    for (uint32_t i = 0; i < x->it.len; i++) {
        const ifc_item *t = &x->it.data[i];
        if (t->kind == IK_ATOM && t->n == 3 && !t->outside && !no_wrap(t->box->style)) {
            /* A soft wrap opportunity before and after an atomic inline, even next to a no-break space or a character
             * that suppresses breaks (CSS Text 3 section 5.1); not next to a word joiner, a ZWJ or another glue character. */
            for (int side = 0; side < 2; side++) {
                uint32_t pos = side == 0 ? t->p : t->p + 3;
                if (pos == 0 || pos >= len || (extra.len > 0 && extra.data[extra.len - 1] >= pos)) {
                    continue;
                }
                uint32_t nb = 0;
                if (side == 0) {
                    uint32_t b = pos - 1;
                    while (b > 0 && (((uint8_t)x->buf.data[b]) & 0xC0) == 0x80) {
                        b--;
                    }
                    const char *q = x->buf.data + b;
                    nb = embi_utf8_decode(&q, x->buf.data + pos);
                } else {
                    const char *q = x->buf.data + pos;
                    nb = embi_utf8_decode(&q, x->buf.data + len);
                }
                bool glue = nb == 0x2060 || nb == 0xFEFF || nb == 0x200D || nb == 0x34F || nb == 0x202F || nb == 0x2007 ||
                            nb == 0x2011 || nb == 0x180E || (nb >= 0x35C && nb <= 0x362) || nb == 0xF08 || nb == 0xF0C ||
                            nb == 0xF12 || nb == 0xFD9 || nb == 0xFDA;
                if (!glue) {
                    embi_vec_push(&extra, pos);
                } else {
                    /* the candidates are sorted by position: binary search instead of a scan per atom */
                    uint32_t lo = 0, hi = x->cand.len;
                    while (lo < hi) {
                        uint32_t mid = lo + (hi - lo) / 2;
                        if (x->cand.data[mid].pos < pos) {
                            lo = mid + 1;
                        } else {
                            hi = mid;
                        }
                    }
                    for (uint32_t k = lo; k < x->cand.len && x->cand.data[k].pos == pos; k++) {
                        x->cand.data[k].flags &= (uint8_t)~IFC_BK_LINE;
                    }
                }
            }
            continue;
        }
        if (t->kind != IK_TEXT || t->n == 0 || no_wrap(t->box->style) ||
            embi_lay_ws_mode(t->box->style) != EMBI_WS_BREAK_SPACES) {
            continue;
        }
        for (uint32_t b = 0; b < t->n; b++) {
            char ch = t->box->text[t->b0 + b];
            uint32_t pos = t->p + b + 1;
            if ((ch == ' ' || ch == '\t') && pos < len) {
                embi_vec_push(&extra, pos);
            } else if (b + 3 <= t->n && pos + 2 < len && breaking_sep3((const uint8_t *)t->box->text + t->b0 + b)) {
                /* and after every other space separator that allows a break (CSS Text 3 section 4.1.3) */
                embi_vec_push(&extra, pos + 2);
                b += 2;
            }
        }
    }
    if (extra.len > 0) {
        EMBI_VEC(ifc_cand) out;
        memset(&out, 0, sizeof out);
        uint32_t a = 0, e = 0;
        while (a < x->cand.len || e < extra.len) {
            if (e >= extra.len || (a < x->cand.len && x->cand.data[a].pos < extra.data[e])) {
                embi_vec_push(&out, x->cand.data[a++]);
            } else if (a < x->cand.len && x->cand.data[a].pos == extra.data[e]) {
                ifc_cand c = x->cand.data[a++];
                c.flags |= IFC_BK_LINE;
                embi_vec_push(&out, c);
                e++;
            } else {
                ifc_cand c = mk_cand(extra.data[e++], IFC_BK_LINE, 0.0f, nullptr);
                embi_vec_push(&out, c);
            }
        }
        embi_vec_free(&x->cand);
        x->cand.data = out.data;
        x->cand.len = out.len;
        x->cand.cap = out.cap;
    }
    embi_vec_free(&extra);
}

/* The in-item advances at the candidates: for each text item, the advance from its start to each
 * break candidate inside it, accumulated segment by segment so the whole pass measures every byte
 * once (prefix_w would otherwise measure the tail of the item for every candidate). */
static void build_ptab(ifc_st *x)
{
    uint32_t cur = UINT32_MAX;
    uint32_t prev = 0;
    float acc = 0.0f;
    for (uint32_t ci = 0; ci < x->cand.len; ci++) {
        uint32_t pos = x->cand.data[ci].pos;
        uint32_t j = split_of(x, pos);
        if (j == 0) {
            continue;
        }
        ifc_item *t = &x->it.data[j - 1];
        if (t->kind != IK_TEXT || t->n == 0 || !(t->p < pos && pos < t->p + t->n)) {
            continue;
        }
        if (cur != j - 1) {
            cur = j - 1;
            t->tab0 = (uint32_t)x->ptab.len;
            prev = t->p;
            acc = 0.0f;
        }
        acc += twi(t, t->b0 + (prev - t->p), t->b0 + (pos - t->p));
        prev = pos;
        ifc_ptab e = {pos, acc};
        embi_vec_push(&x->ptab, e);
        t->tabn++;
    }
}

/* The prefix sums of the item widths: cmn for the minimum and cmx for the maximum sizes. */
static void build_cum(ifc_st *x)
{
    embi_vec_push(&x->cmn, 0.0f);
    embi_vec_push(&x->cmx, 0.0f);
    for (uint32_t i = 0; i < x->it.len; i++) {
        const ifc_item *t = &x->it.data[i];
        float mn = 0.0f, mx = 0.0f;
        switch (t->kind) {
        case IK_TEXT:
        case IK_ATOM:
            mn = t->wmin;
            mx = t->wmax;
            break;
        case IK_FLOAT:
            /* A float is not an item of the line for the minimum (CSS Sizing 3 section 5.1: the
             * minimum is the widest float or word; embi_lay_ifc_intrinsic folds the floats in); its
             * maximum adds to the line it sits in. Both are zero in layout, where floats are placed
             * beside the lines. */
            mx = t->wmax;
            break;
        case IK_OPEN:
        case IK_CLOSE:
            mn = mx = t->w;
            break;
        default:
            break;
        }
        embi_vec_push(&x->cmn, x->cmn.data[i] + mn);
        embi_vec_push(&x->cmx, x->cmx.data[i] + mx);
    }
    build_ptab(x);
}

static float prefix_w(const ifc_st *x, uint32_t pos, bool mx);
static float hang_of(const ifc_st *x, uint32_t s, uint32_t e, uint32_t j0, uint32_t j1, uint32_t *hs, bool sep);

/* The style properties the paragraph analysis reads: the break modes, white-space handling, nowrap and
 * hyphens (build_cands, add_space_breaks, hang_of). Packed into 16 bits. */
uint16_t embi_lay_ifc_amode(const embi_style *s)
{
    uint32_t m = word_break_mode(s);                       /* 2 bits */
    m |= (uint32_t)overflow_wrap_mode(s) << 2;             /* 2 */
    m |= (uint32_t)line_break_mode(s) << 4;                /* 3 */
    m |= (uint32_t)(no_wrap(s) ? 1 : 0) << 7;              /* 1 */
    m |= (uint32_t)embi_lay_ws_mode(s) << 8;               /* 3 */
    m |= (uint32_t)(kwv(s, EMBI_PROP_hyphens) == EMBI_KW_none ? 1 : 0) << 11;
    return (uint16_t)m;
}

static inline uint64_t sig_mix1(uint64_t h, uint64_t w)
{
    h ^= w;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

static inline uint64_t sig_mix2(uint64_t h, uint64_t w)
{
    h += w + 0x632BE59BD9B4E019ull;
    h *= 0xD6E8FEB86659FD93ull;
    return h ^ (h >> 32);
}

/* The 128-bit content key of the paragraph x holds: everything the analysis computes from. The text and its
 * shaping style (family list, size, weight, spacing, features, language) come through the 128-bit keys of the
 * shaped runs; the rest is the item kinds and positions, the widths of the inline box edges, the unicode-bidi
 * kinds (their control characters are bytes without an item), the break modes of the item's style and the
 * container's own shaping style and direction. Atomic inlines, floats, percentages and outside markers make
 * the paragraph uncacheable before this is asked.
 *
 * The key is only these 128 bits: a cache hit compares k1 and k2 (and the text length), not the inputs, so
 * the signature must cover every input of the analysis (a missed input is a stale analysis, not a miss) and
 * its two mixers are independent so that a collision needs both to collide (about 2^-128 per pair). */
static void prep_sig(const ifc_st *x, uint64_t *k1, uint64_t *k2)
{
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull;
#define SIG(w)                                                                                 \
    do {                                                                                       \
        uint64_t w_ = (w);                                                                     \
        a = sig_mix1(a, w_);                                                                   \
        b = sig_mix2(b, w_);                                                                   \
    } while (0)
    const embi_lay_tkey *rk = embi_lay_tkey_of(x->c, x->rs);
    SIG(plen(x));
    SIG(x->it.len);
    SIG(x->base);
    if (rk != nullptr) {
        SIG(rk->h);
        SIG(rk->h2);
        SIG(rk->amode);
    }
    embi_str lang = embi_lay_lang(x->c, x->root);
    SIG(embi_hash_bytes(lang.ptr, lang.len, 1));
    const embi_style *last_s = nullptr;
    uint32_t last_am = 0;
    for (uint32_t i = 0; i < x->it.len; i++) {
        const ifc_item *t = &x->it.data[i];
        uint64_t w = (uint64_t)t->kind | ((uint64_t)t->bm << 8) | ((uint64_t)(t->rtl ? 1 : 0) << 16) |
                     ((uint64_t)t->p << 24);
        if (t->kind == IK_TEXT) {
            const embi_style *s = t->box->style;
            if (s != last_s) {
                const embi_lay_tkey *tk = embi_lay_tkey_of(x->c, s);
                last_am = tk != nullptr ? tk->amode : 0;
                last_s = s;
            }
            uint64_t r1 = t->k1, r2 = t->k2;
            if (r1 == 0 && r2 == 0) {
                embi_tshim_key(t->lt, &r1, &r2); /* the box had its run already: read the key from it */
            }
            /* three words per text item: the item's shape, the run's two hashes (each folded with the item's range) */
            SIG(w ^ ((uint64_t)last_am << 56));
            SIG(r1 ^ ((uint64_t)t->n << 32 | t->b0));
            SIG(r2 + ((uint64_t)t->b0 << 40 | t->n));
        } else if (t->kind == IK_OPEN || t->kind == IK_CLOSE) {
            uint32_t wb;
            memcpy(&wb, &t->w, sizeof wb);
            SIG(w);
            SIG(wb);
        } else {
            SIG(w);
            SIG(t->n);
        }
    }
#undef SIG
    *k1 = a;
    *k2 = b;
}

/* Copies the cached analysis into x. */
static void apply_prep(ifc_st *x, ifc_prep *pc)
{
    x->pc = pc;
    x->para = pc->para;
    x->lv_lo = x->lv_hi = 0;
    if (pc->lv_whole) {
        /* One run: level_at never has to look at the paragraph's analysis (a cache miss). */
        x->lv_hi = pc->buflen;
        x->lv_v = pc->lv;
    }
    /* The cached tables are copied (memcpy) into the scratch vectors of x: later steps of the paragraph
     * (line breaking, intrinsic sizes) read and extend them, and the cache entry must stay unchanged for the
     * next layout. The copy is a few flat arrays of nit + 1 floats and the candidates, far cheaper than the
     * analysis. */
    uint32_t nc = pc->nit + 1u;
    embi_vec_reserve(&x->cand, pc->ncand);
    memcpy(x->cand.data, pc->cand, (size_t)pc->ncand * sizeof(ifc_cand));
    x->cand.len = pc->ncand;
    embi_vec_reserve(&x->cmn, nc);
    embi_vec_reserve(&x->cmx, nc);
    memcpy(x->cmn.data, pc->cum, (size_t)nc * sizeof(float));
    memcpy(x->cmx.data, pc->cum + nc, (size_t)nc * sizeof(float));
    x->cmn.len = nc;
    x->cmx.len = nc;
    if (pc->nptab > 0) {
        embi_vec_reserve(&x->ptab, pc->nptab);
        memcpy(x->ptab.data, pc->ptab, (size_t)pc->nptab * sizeof(ifc_ptab));
        x->ptab.len = pc->nptab;
    }
    for (uint32_t i = 0; i < pc->nit; i++) {
        x->it.data[i].tab0 = pc->tab[2u * i];
        x->it.data[i].tabn = pc->tab[2u * i + 1u];
    }
}

/* ld->pfront (layout_int.h): the analysis a box (an IFC root) used in this tree. */
static inline uint32_t pf_slot(const void *root, uint32_t mask)
{
    return (uint32_t)((((uint64_t)(uintptr_t)root >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & mask;
}

static inline void pf_prefetch(const embi_lay_doc *ld, const void *root)
{
    if (ld->pfront_n != 0) {
        __builtin_prefetch(&ld->pfront[pf_slot(root, ld->pfront_n - 1u)]);
    }
}

static ifc_prep *pf_get(const embi_lay_doc *ld, const void *root)
{
    if (ld->pfront_n == 0) {
        return nullptr;
    }
    uint32_t mask = ld->pfront_n - 1u;
    for (uint32_t i = pf_slot(root, mask);; i = (i + 1u) & mask) {
        if (ld->pfront[i].root == root) {
            return ld->pfront[i].pc;
        }
        if (ld->pfront[i].root == nullptr) {
            return nullptr;
        }
    }
}

static void pf_put(embi_lay_doc *ld, const void *root, ifc_prep *pc)
{
    if ((ld->pfront_cnt + 1u) * 2u > ld->pfront_n) {
        uint32_t n2 = ld->pfront_n == 0 ? 1024u : ld->pfront_n * 2u;
        typeof(*ld->pfront) *t2 = embi_calloc(n2, sizeof *t2);
        if (t2 == nullptr) {
            return; /* the table is only a shortcut: the content key finds the analysis again */
        }
        for (uint32_t j = 0; j < ld->pfront_n; j++) {
            if (ld->pfront[j].root != nullptr) {
                uint32_t k = pf_slot(ld->pfront[j].root, n2 - 1u);
                while (t2[k].root != nullptr) {
                    k = (k + 1u) & (n2 - 1u);
                }
                t2[k] = ld->pfront[j];
            }
        }
        embi_free(ld->pfront, (size_t)ld->pfront_n * sizeof *ld->pfront);
        ld->pfront = t2;
        ld->pfront_n = n2;
    }
    uint32_t mask = ld->pfront_n - 1u;
    uint32_t i = pf_slot(root, mask);
    while (ld->pfront[i].root != nullptr && ld->pfront[i].root != root) {
        i = (i + 1u) & mask;
    }
    if (ld->pfront[i].root == nullptr) {
        ld->pfront[i].root = root;
        ld->pfront_cnt++;
    }
    ld->pfront[i].pc = pc;
}

/* The analysis of the paragraph: from the cache of the box when it has a valid entry, else from the content
 * cache, else computed (and kept when the IFC is simple). lazy: the caller only wants the cached intrinsic
 * widths when the entry has them (x->pc is set and the tables are not copied into x). */
static void analyze_all(ifc_st *x, bool lazy)
{
    embi_lay_doc *ld = x->c->ld;
    uint64_t v = 0;
    uint64_t k1 = 0, k2 = 0;
    if (!x->nocache) {
        ifc_prep *pc0 = pf_get(ld, x->root);
        if (pc0 != nullptr) {
            ifc_prep *pc = pc0;
            if (pc->nit == x->it.len && pc->buflen == plen(x)) {
                if (lazy && pc->have_int) {
                    x->pc = pc;
                    return;
                }
                apply_prep(x, pc);
                return;
            }
        }
        if (x->it.len > 0) {
            prep_sig(x, &k1, &k2);
            if (embi_hashmap_get(&ld->pmap, k1, &v)) {
                for (ifc_prep *pc = (ifc_prep *)(uintptr_t)v; pc != nullptr; pc = pc->chain) {
                    if (pc->k1 == k1 && pc->k2 == k2 && pc->nit == x->it.len && pc->buflen == plen(x)) {
                        pc->gen = ld->tgen;
                        if (pc->pt != nullptr) {
                            pc->pt->gen = ld->tgen;
                        }
                        pf_put(ld, x->root, pc);
                        if (lazy && pc->have_int) {
                            x->pc = pc;
                            return;
                        }
                        apply_prep(x, pc);
                        return;
                    }
                }
            }
        }
    }
    analyze(x);
    uint64_t an0 = SDL_GetPerformanceCounter();
    build_cands(x);
    build_cum(x);
    for (uint32_t i = 0; i < x->cand.len; i++) {
        ifc_cand *cd = &x->cand.data[i];
        cd->pw = prefix_w(x, cd->pos, false);
        cd->hs0 = cd->pos;
        cd->hang = hang_of(x, 0, cd->pos, 0, split_of(x, cd->pos), &cd->hs0, true);
        cd->okc = !x->has_tab;
    }
    if (x->it.len > 0) {
        ld->cold.analyses++; /* an IFC with no items (only out-of-flow children) has nothing to analyse */
    }
    ld->cold.analysis_ticks += SDL_GetPerformanceCounter() - an0;
    if (x->nocache || x->it.len == 0) {
        return;
    }
    for (uint32_t i = 0; i < x->cand.len; i++) {
        if (x->cand.data[i].hs != nullptr || x->cand.data[i].hw != 0.0f) {
            return; /* a soft hyphen: its width depends on the layout's font lookups */
        }
    }
    uint32_t nit = x->it.len, nc = nit + 1u;
    size_t sz = sizeof(ifc_prep) + (size_t)x->cand.len * sizeof(ifc_cand) + (size_t)nc * 2u * sizeof(float) +
                (size_t)x->ptab.len * sizeof(ifc_ptab) + (size_t)nit * 2u * sizeof(uint32_t) + 32u;
    embi_vec_reserve(&ld->preps, ld->preps.len + 1u);
    uint8_t *mem = ld->preps.len < ld->preps.cap ? embi_lay_pool_alloc(&ld->pool, sz) : nullptr;
    if (mem == nullptr) {
        return;
    }
    ifc_prep *pc = (ifc_prep *)mem;
    uint8_t *q = mem + sizeof(ifc_prep);
    pc->sz = sz;
    pc->k1 = k1;
    pc->k2 = k2;
    pc->gen = ld->tgen;
    pc->pt = x->para_ent;
    pc->nit = nit;
    pc->ncand = x->cand.len;
    pc->nptab = x->ptab.len;
    pc->buflen = plen(x);
    pc->para = x->para;
    pc->lv_whole = false;
    if (x->para != nullptr && pc->buflen > 0) {
        uint32_t lo = 0, hi = 0;
        uint8_t lv = embi_tshim_level_span(x->para, 0, &lo, &hi);
        if (lo == 0 && hi >= pc->buflen) {
            pc->lv_whole = true;
            pc->lv = lv;
        }
    }
    pc->cand = (ifc_cand *)q;
    q += (size_t)x->cand.len * sizeof(ifc_cand);
    pc->ptab = (ifc_ptab *)q;
    q += (size_t)x->ptab.len * sizeof(ifc_ptab);
    pc->cum = (float *)q;
    q += (size_t)nc * 2u * sizeof(float);
    pc->tab = (uint32_t *)q;
    q += (size_t)nit * 2u * sizeof(uint32_t);
    memcpy(pc->cand, x->cand.data, (size_t)x->cand.len * sizeof(ifc_cand));
    if (x->ptab.len > 0) {
        memcpy(pc->ptab, x->ptab.data, (size_t)x->ptab.len * sizeof(ifc_ptab));
    }
    memcpy(pc->cum, x->cmn.data, (size_t)nc * sizeof(float));
    memcpy(pc->cum + nc, x->cmx.data, (size_t)nc * sizeof(float));
    for (uint32_t i = 0; i < nit; i++) {
        pc->tab[2u * i] = x->it.data[i].tab0;
        pc->tab[2u * i + 1u] = x->it.data[i].tabn;
    }
    uint64_t head = 0;
    if (embi_hashmap_get(&ld->pmap, k1, &head)) {
        pc->chain = (ifc_prep *)(uintptr_t)head;
    }
    (void)embi_hashmap_put(&ld->pmap, k1, (uint64_t)(uintptr_t)pc);
    ld->preps.data[ld->preps.len++] = pc;
    ld->prep_bytes += sz > EMBI_POOL_MAX ? sz : 0; /* the pooled ones are counted by the slabs */
    pf_put(ld, x->root, pc);
    x->pc = pc;
}

/* Drops the analyses the layout that just ended did not use (all of them when all), and rebuilds the
 * lookup map. Called by embi_lay_tcache_sweep before the shaped runs they point at are swept. */
void embi_lay_prep_sweep(embi_lay_doc *ld, bool all)
{
    uint32_t keep = 0;
    for (uint32_t i = 0; i < ld->preps.len; i++) {
        ifc_prep *pc = ld->preps.data[i];
        if (!all && pc->gen == ld->tgen) {
            ld->preps.data[keep++] = pc;
        } else {
            ld->prep_bytes -= pc->sz > EMBI_POOL_MAX ? pc->sz : 0;
            embi_lay_pool_free(&ld->pool, pc, pc->sz);
        }
    }
    if (keep == ld->preps.len && !all) {
        return;
    }
    ld->preps.len = keep;
    embi_hashmap_clear(&ld->pmap);
    for (uint32_t i = 0; i < keep; i++) {
        ifc_prep *pc = ld->preps.data[i];
        uint64_t head = 0;
        pc->chain = nullptr;
        if (embi_hashmap_get(&ld->pmap, pc->k1, &head)) {
            pc->chain = (ifc_prep *)(uintptr_t)head;
        }
        (void)embi_hashmap_put(&ld->pmap, pc->k1, (uint64_t)(uintptr_t)pc);
    }
}

/* The width of the content before a position: the prefix sum of the items before the one that holds
 * the position, plus the advance inside it (from the candidate table, else measured). */
static float prefix_w(const ifc_st *x, uint32_t pos, bool mx)
{
    uint32_t j = split_of(x, pos);
    float w = mx ? x->cmx.data[j] : x->cmn.data[j];
    if (j > 0) {
        const ifc_item *t = &x->it.data[j - 1];
        if (t->kind == IK_TEXT && t->n > 0 && t->p < pos && pos < t->p + t->n) {
            uint32_t lo = t->tab0, hi = t->tab0 + t->tabn;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (x->ptab.data[mid].pos < pos) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo < t->tab0 + t->tabn && x->ptab.data[lo].pos == pos) {
                return (mx ? x->cmx.data[j - 1] : x->cmn.data[j - 1]) + x->ptab.data[lo].w;
            }
            w -= twi(t, t->b0 + (pos - t->p), t->b0 + t->n);
        }
    }
    return w;
}

/* Whether the three UTF-8 bytes at p encode a space separator that hangs at the end of a line besides U+0020: U+1680,
 * U+2000 to U+200A, U+202F, U+205F and U+3000 (not the no-break space U+00A0). */
static bool space_sep3(const uint8_t *p)
{
    if (p[0] == 0xE1u) {
        return p[1] == 0x9Au && p[2] == 0x80u;
    }
    if (p[0] == 0xE2u) {
        return (p[1] == 0x80u && ((p[2] >= 0x80u && p[2] <= 0x8Au) || p[2] == 0xAFu)) ||
               (p[1] == 0x81u && p[2] == 0x9Fu);
    }
    return p[0] == 0xE3u && p[1] == 0x80u && p[2] == 0x80u;
}

/* The collapsible spaces that end the line [s, e) and hang: their width, and where they start (e
 * when there are none). Only the items [j0, j1) are looked at. */
static float hang_of(const ifc_st *x, uint32_t s, uint32_t e, uint32_t j0, uint32_t j1,
                     uint32_t *hs, bool sep)
{
    float w = 0.0f;
    *hs = e;
    for (uint32_t i = j1; i > j0; i--) {
        const ifc_item *t = &x->it.data[i - 1];
        if (t->kind == IK_OPEN || t->kind == IK_CLOSE || t->kind == IK_BR || t->kind == IK_FLOAT ||
            (t->kind == IK_TEXT && t->n == 0)) {
            continue; /* zero-width (a <br> is the line feed that ends the line, a float is beside it) */
        }
        if (t->kind != IK_TEXT) {
            break;
        }
        uint32_t lo = t->p > s ? t->p : s;
        uint32_t hi = t->p + t->n < e ? t->p + t->n : e;
        if (!hangs(t->box->style)) {
            /* Nothing hangs, but the last character still gets no letter-spacing. */
            uint32_t bl2 = t->b0 + (lo - t->p), kk = t->b0 + (hi - t->p), bh2 = kk;
            while (kk > bl2 && t->box->text[kk - 1] == '\n') {
                kk--;
            }
            if (kk > bl2) {
                w += ls_end(t->box, bh2);
            }
            break;
        }
        if (lo >= hi) {
            continue;
        }
        uint32_t bl = t->b0 + (lo - t->p), bh = t->b0 + (hi - t->p);
        uint32_t k = bh;
        uint32_t nsp = 0; /* the spaces and tabs (not the line feeds) that hang */
        /* The spaces before a preserved line feed hang with the ones at the end of the line; the
         * feed itself takes no room. */
        while (k > bl) {
            if (t->box->text[k - 1] == ' ' || t->box->text[k - 1] == '\n' || t->box->text[k - 1] == 9) {
                nsp += t->box->text[k - 1] != '\n';
                k--;
            } else if (sep && k - bl >= 3 && space_sep3((const uint8_t *)t->box->text + k - 3)) {
                k -= 3; /* an other space separator hangs too (CSS Text 4 section 4.1.3) */
                nsp++;
            } else if (sep && k - bl >= 3 && (uint8_t)t->box->text[k - 3] == 0xE2u && (uint8_t)t->box->text[k - 2] == 0x80u &&
                       (uint8_t)t->box->text[k - 1] == 0x8Bu) {
                k -= 3; /* a zero width space (<wbr>) takes no room: the spaces before it hang */
            } else {
                break;
            }
        }
        if (k < bh) {
            w += twi(t, k, bh);
            *hs = t->p + (k - t->b0);
        }
        if (k > bl) {
            /* The last visible character of the line gets no letter-spacing (CSS Text 4 section 8.2), unless the
             * spaces after it hang or the box already left it out. */
            if (nsp == 0 && w == 0.0f) {
                w += ls_end(t->box, bh);
            }
            break; /* a visible character ends the run of spaces */
        }
    }
    return w;
}

/* ------------------------------------------------------------------ tabs */

/* The advance of the preserved tab at box byte `at` of text box k when the line is `pos` px in (CSS Text 3 section 6.2):
 * to the next multiple of tab-size from the line's start edge. A number is that many advances of a space (the tab's own
 * glyph is a space, so its advance has the letter-spacing in it) plus the word-spacing; a length is itself. A tab stop
 * closer than half a ch (the advance of the 0 glyph) is skipped, which is what the browsers do. */
static float tab_advance(const ifc_st *x, const embi_lay_text *lt, const embi_box *k, uint32_t at, float pos)
{
    const embi_style *st = k->style;
    float space = lt != nullptr ? embi_tshim_measure(lt, at, at + 1) : 0.0f;
    float ws = 0.0f;
    const embi_value *wv = embi_style_value(st, EMBI_PROP_word_spacing);
    ws = embi_lay_spacing_px(wv, embi_lay_font_px_style(st));
    float size = 8.0f * (space + ws);
    const embi_value *tv = embi_style_value(st, EMBI_PROP_tab_size);
    if (tv != nullptr) {
        if (tv->kind == EMBI_V_NUMBER || tv->kind == EMBI_V_INTEGER) {
            size = (float)tv->num * (space + ws);
        } else if (tv->kind == EMBI_V_LENGTH && tv->unit == EMBI_U_PX) {
            size = (float)tv->num;
        } else if (tv->kind == EMBI_V_LENGTH || tv->kind == EMBI_V_CALC) {
            float v = embi_lay_len(x->c, k, tv, NAN);
            if (!isnan(v)) {
                size = v;
            }
        }
    }
    if (!(size > 0.001f)) {
        return 0.0f; /* tab-size: 0 */
    }
    float next = (floorf(pos / size + 0.0001f) + 1.0f) * size;
    /* half a ch: the advance of the 0 glyph of the text's own font */
    embi_lay_text *zt = embi_lay_shape_text(x->c, embi_lay_tkey_of(x->c, st), (embi_str){"", 0}, 0, "0", 1);
    float min_adv = zt != nullptr ? 0.5f * embi_tshim_advance(zt) : 0.0f;
    if (next - pos < min_adv - 0.0001f) {
        next += size;
    }
    return next - pos;
}

/* The advance of the paragraph bytes [s, c) for a line that starts at s, with every tab advanced to its tab stop (only
 * for a paragraph that has tabs: the prefix sums count a tab as one space). The position the stops are measured from is
 * the line's start. */
static float tabbed_w(const ifc_st *x, uint32_t s, uint32_t c, bool mx)
{
    float pos = 0.0f;
    uint32_t j0 = start_of(x, s), j1 = split_of(x, c);
    for (uint32_t i = j0; i < j1; i++) {
        const ifc_item *t = &x->it.data[i];
        switch (t->kind) {
        case IK_TEXT: {
            if (t->n == 0 || t->outside) {
                break;
            }
            uint32_t lo = t->p > s ? t->p : s;
            uint32_t hi = t->p + t->n < c ? t->p + t->n : c;
            if (lo >= hi) {
                break;
            }
            uint32_t a = t->b0 + (lo - t->p), b = t->b0 + (hi - t->p);
            uint32_t seg = a;
            for (uint32_t k = a; k < b; k++) {
                if (t->box->text[k] == 9) {
                    pos += twi(t, seg, k);
                    pos += tab_advance(x, t->lt, t->box, k, pos);
                    seg = k + 1;
                }
            }
            pos += twi(t, seg, b);
            break;
        }
        case IK_OPEN:
        case IK_CLOSE:
            pos += t->w;
            break;
        case IK_ATOM:
            if (!t->outside) {
                pos += mx ? t->wmax : t->wmin;
            }
            break;
        default:
            break;
        }
    }
    return pos;
}

/* Gives the tab units of the line their widths (logical order, from the line's start). */
static void resolve_tabs(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    /* The tab stops are measured from the start content edge of the block (CSS Text 3 section 4.2): the line starts
     * its text-indent (and the floats beside it) in. */
    float pos = (x->base == 1 ? x->cw - ln->rb : ln->lb) + ln->indent;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->tab) {
            u->w = tab_advance(x, u->lt, u->box, u->b0, pos);
        }
        pos += u->w;
    }
}

/* The preserved spaces of white-space: pre-wrap that end the line [s, e) (before its preserved line feeds): when the
 * line ends at a forced break they hang only conditionally (CSS Text 3 section 4.1.3): their width, and where they
 * start (*ts) and end (*te). 0 (and *ts = *te = e) when there are none. Only the items [j0, j1) are looked at. */
static float cond_tail(const ifc_st *x, uint32_t s, uint32_t e, uint32_t j0, uint32_t j1, uint32_t *ts, uint32_t *te)
{
    float w = 0.0f;
    *ts = e;
    *te = e;
    bool any = false;
    for (uint32_t i = j1; i > j0; i--) {
        const ifc_item *t = &x->it.data[i - 1];
        if (t->kind == IK_OPEN || t->kind == IK_CLOSE || t->kind == IK_BR || t->kind == IK_FLOAT ||
            (t->kind == IK_TEXT && t->n == 0)) {
            continue;
        }
        if (t->kind != IK_TEXT) {
            break;
        }
        uint8_t m = embi_lay_ws_mode(t->box->style);
        if ((m != EMBI_WS_PRESERVE && m != EMBI_WS_PRESERVE_SPACES) || no_wrap(t->box->style)) {
            break;
        }
        uint32_t lo = t->p > s ? t->p : s;
        uint32_t hi = t->p + t->n < e ? t->p + t->n : e;
        if (lo >= hi) {
            continue;
        }
        uint32_t bl = t->b0 + (lo - t->p), bh = t->b0 + (hi - t->p);
        uint32_t k = bh;
        if (!any) {
            while (k > bl && t->box->text[k - 1] == '\n') {
                k--;
            }
            bh = k;
        }
        for (;;) {
            if (k > bl && t->box->text[k - 1] == ' ') {
                k--;
            } else if (k - bl >= 3 && space_sep3((const uint8_t *)t->box->text + k - 3)) {
                k -= 3; /* an other space separator (U+3000) hangs conditionally too before a forced break */
            } else {
                break;
            }
        }
        if (k < bh) {
            if (!any) {
                *te = t->p + (bh - t->b0);
            }
            any = true;
            w += tw(t->lt, t->box, k, bh);
            *ts = t->p + (k - t->b0);
        }
        if (k > bl) {
            break;
        }
    }
    return any ? w : 0.0f;
}

/* The advance of the text of the paragraph range [a, b) in the items [j0, j1). */
static float range_w(const ifc_st *x, uint32_t a, uint32_t b, uint32_t j0, uint32_t j1)
{
    float w = 0.0f;
    for (uint32_t i = j0; i < j1 && a < b; i++) {
        const ifc_item *t = &x->it.data[i];
        if (t->kind != IK_TEXT || t->n == 0 || t->outside) {
            continue;
        }
        uint32_t lo = t->p > a ? t->p : a;
        uint32_t hi = t->p + t->n < b ? t->p + t->n : b;
        if (lo < hi) {
            w += tw(t->lt, t->box, t->b0 + (lo - t->p), t->b0 + (hi - t->p));
        }
    }
    return w;
}

/* What hangs at the end of the line [s, e) for its alignment (CSS Text 3 section 4.1.3): the other space separators
 * that hang are drawn but take no room (ln->hw_s), and at a forced break the preserved spaces of pre-wrap hang only
 * conditionally (ln->hw_t: they are drawn, and only the part that does not fit hangs; justify_units decides). *hs is
 * where the units of the line end: after those preserved spaces. */
static void line_hang(ifc_st *x, uint32_t s, uint32_t e, uint32_t j0, uint32_t j1, uint32_t *hs, bool forced)
{
    ifc_line *ln = &x->ln;
    ln->hw_s = 0.0f;
    ln->hw_t = 0.0f;
    uint32_t hsep = e;
    hang_of(x, s, e, j0, j1, &hsep, true);
    if (forced) {
        uint32_t ts = e, te = e;
        float wt = cond_tail(x, s, e, j0, j1, &ts, &te);
        /* (ts < *hs: the tail is other space separators, which hang_of left in the units; a <br> may follow them) */
        if (wt > 0.0f && ((ts >= *hs && te > *hs) || (ts < *hs && te <= *hs))) {
            ln->hw_t = wt;
            *hs = te;
        }
    }
    if (hsep < *hs) {
        uint32_t end = ln->hw_t > 0.0f ? e : *hs;
        float w = range_w(x, hsep, end, j0, j1) - ln->hw_t;
        ln->hw_s = w > 0.0f ? w : 0.0f;
    }
}

/* The content width of the line [s, c), with its hanging spaces left out. */
static float tabbed_w(const ifc_st *x, uint32_t s, uint32_t c, bool mx);

static float content_w(const ifc_st *x, uint32_t s, uint32_t c, bool mx)
{
    uint32_t j0 = start_of(x, s), j1 = split_of(x, c);
    uint32_t hs = c;
    float hang = hang_of(x, s, c, j0, j1, &hs, true);
    if (mx && hang > 0.0f) {
        /* max-content: every line ends at a forced break, where the preserved spaces of pre-wrap hang only
         * conditionally; they fit (the line is as wide as it needs), so nothing before them hangs either. */
        uint32_t ts = c, te = c;
        if (cond_tail(x, s, c, j0, j1, &ts, &te) > 0.0f && te >= hs) {
            hang = 0.0f;
            hs = c;
        }
    }
    if (x->has_tab) {
        return tabbed_w(x, s, hs, mx);
    }
    return prefix_w(x, c, mx) - prefix_w(x, s, mx) - hang;
}

/* The end of a line that starts at s. */
typedef struct ifc_end {
    uint32_t e;
    bool forced;
    bool overflow;
    float hw;                 /* a soft hyphen ends the line: the width of the hyphen shown */
    const embi_style *hs;     /* and its style (nullptr when none) */
} ifc_end;

/* The width of the content [s, c) of candidate c, plus the hyphen a soft hyphen shows there. */
static float cand_w(const ifc_st *x, uint32_t s, const ifc_cand *c, bool mx)
{
    if (!mx && c->okc && s <= c->hs0) {
        /* The line's hanging spaces all lie after its start: they are the ones computed for the paragraph. */
        return c->pw - prefix_w(x, s, false) - c->hang + c->hw;
    }
    return content_w(x, s, c->pos, mx) + c->hw;
}

/* ---- text-spacing-trim (CSS Text 4 section 9.2): fullwidth opening punctuation at the start of a line and closing
 * punctuation at its end are set half-width. The start is trimmed under trim-start, trim-both and trim-all; the end
 * always under trim-both and trim-all, and under the others but space-all only when the line does not fit otherwise
 * (allow-end). Trimming takes half the advance off: an opening mark's glyph moves back by as much (its ink is in its
 * second half), a closing mark's stays. Left-to-right runs only. ---- */

static bool fw_open_punct(uint32_t c)
{
    switch (c) {
    case 0x3008u: case 0x300Au: case 0x300Cu: case 0x300Eu: case 0x3010u: case 0x3014u: case 0x3016u: case 0x3018u:
    case 0x301Au: case 0x301Du: case 0xFF08u: case 0xFF3Bu: case 0xFF5Bu: case 0xFF5Fu:
        return true;
    default:
        return false;
    }
}

static bool fw_close_punct(uint32_t c)
{
    switch (c) {
    case 0x3001u: case 0x3002u: case 0x3009u: case 0x300Bu: case 0x300Du: case 0x300Fu: case 0x3011u: case 0x3015u:
    case 0x3017u: case 0x3019u: case 0x301Bu: case 0x301Eu: case 0x301Fu: case 0xFF09u: case 0xFF0Cu: case 0xFF0Eu:
    case 0xFF3Du: case 0xFF5Du: case 0xFF60u:
        return true;
    default:
        return false;
    }
}

/* The text item that holds paragraph byte q, or null. */
static const ifc_item *text_item_at(const ifc_st *x, uint32_t q)
{
    for (uint32_t j = start_of(x, q); j < x->it.len && x->it.data[j].p <= q; j++) {
        const ifc_item *t = &x->it.data[j];
        if (t->kind == IK_TEXT && t->n > 0 && !t->outside && q < t->p + t->n) {
            return t;
        }
    }
    return nullptr;
}

/* The width trimming takes off the character at paragraph byte q when it is fullwidth opening (open) or closing
 * punctuation that text-spacing-trim may trim there: half its advance, when that is at least 3/4 em (a glyph the font's
 * halt feature has already set half-width is left alone); else 0. *always is set when the end is trimmed even where the
 * line fits. */
static bool has_fwp(const ifc_st *x)
{
    if (x->fwp == 0) {
        uint8_t r = 1;
        for (uint32_t i = 0, n = plen(x); i < n; i++) {
            uint8_t ch = (uint8_t)x->buf.data[i];
            if (ch == 0xE3u || ch == 0xEFu) {
                r = 2;
                break;
            }
        }
        ((ifc_st *)(uintptr_t)x)->fwp = r;
    }
    return x->fwp == 2;
}

static float trim_at(const ifc_st *x, uint32_t q, bool open, bool *always)
{
    if (q >= plen(x) || (uint8_t)x->buf.data[q] < 0xE3u || !has_fwp(x)) {
        return 0.0f;
    }
    const char *p = x->buf.data + q;
    uint32_t c = embi_utf8_decode(&p, x->buf.data + plen(x));
    if (!(open ? fw_open_punct(c) : fw_close_punct(c))) {
        return 0.0f;
    }
    const ifc_item *t = text_item_at(x, q);
    if (t == nullptr || t->lt == nullptr || (level_at(x, q) & 1u) != 0) {
        return 0.0f;
    }
    uint16_t m = kwv(t->box->style, EMBI_PROP_text_spacing_trim);
    bool both = m == EMBI_KW_trim_both || m == EMBI_KW_trim_all;
    if (m == EMBI_KW_space_all || (open && m != EMBI_KW_trim_start && !both)) {
        return 0.0f;
    }
    if (always != nullptr) {
        *always = both;
    }
    uint32_t b = t->b0 + (q - t->p);
    float w = tw_raw(t->lt, t->box, b, b + (uint32_t)(p - (x->buf.data + q)));
    return w >= 0.75f * embi_lay_font_px_style(t->box->style) ? 0.5f * w : 0.0f;
}

/* The trim of the last character before paragraph byte e (a line that ends at e), past nothing. */
static float end_trim(const ifc_st *x, uint32_t s, uint32_t e, bool *always)
{
    if (e <= s || e > plen(x) || !has_fwp(x)) {
        return 0.0f;
    }
    uint32_t q = e - 1;
    while (q > s && ((uint8_t)x->buf.data[q] & 0xC0u) == 0x80u) {
        q--;
    }
    return trim_at(x, q, false, always);
}

/* Trims the units of the line [s, e) built for avail: the first text unit when it starts with opening punctuation and the
 * last one when it ends with closing punctuation. */
static void trim_line_units(ifc_st *x, float avail)
{
    ifc_line *ln = &x->ln;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->kind == UK_OPEN || u->kind == UK_CLOSE) {
            continue;
        }
        if (u->kind == UK_TEXT && u->item >= 0 && u->b1 > u->b0) {
            const ifc_item *t = &x->it.data[u->item];
            float h = trim_at(x, t->p + (u->b0 - t->b0), true, nullptr);
            u->w -= h;
            u->dx = -h;
        }
        break;
    }
    for (uint32_t i = ln->u.len; i > 0; i--) {
        ifc_unit *u = &ln->u.data[i - 1];
        if (u->kind == UK_OPEN || u->kind == UK_CLOSE) {
            continue;
        }
        if (u->kind == UK_TEXT && u->item >= 0 && u->b1 > u->b0) {
            const ifc_item *t = &x->it.data[u->item];
            uint32_t q0 = t->p + (u->b0 - t->b0);
            bool always = false;
            float h = end_trim(x, q0, t->p + (u->b1 - t->b0), &always);
            if (h > 0.0f && !always) {
                float total = 0.0f;
                for (uint32_t k = 0; k < ln->u.len; k++) {
                    total += ln->u.data[k].w;
                }
                if (total <= avail + 0.0005f) {
                    h = 0.0f;
                }
            }
            u->w -= h;
        }
        break;
    }
}

/* The break for a line from s in avail. The candidates after s are taken in order: a candidate
 * whose content does not fit ends the search (the first one overflows when nothing before it fits);
 * a forced break that fits ends the line at once, and is taken even after a normal opportunity;
 * otherwise the last normal opportunity that fits is taken, then the last emergency one
 * (overflow-wrap). */
static ifc_end choose_end(const ifc_st *x, uint32_t s, float avail)
{
    uint32_t n = x->cand.len;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (x->cand.data[mid].pos <= s) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    ifc_end r = {x->cand.data[n - 1].pos, false, false, 0.0f, nullptr};
    const float lim = avail + 0.0005f + trim_at(x, s, true, nullptr);
    int32_t best = -1, emerg = -1, over = -1, hard = -1;
    for (uint32_t k = lo; k < n; k++) {
        const ifc_cand *c = &x->cand.data[k];
        if ((c->flags & (IFC_BK_LINE | IFC_BK_GRAPHEME | IFC_BK_WORD | IFC_BK_HARD)) == 0) {
            continue;
        }
        if (cand_w(x, s, c, false) - end_trim(x, s, c->pos, nullptr) > lim) {
            over = (int32_t)k;
            break;
        }
        if ((c->flags & IFC_BK_HARD) != 0) {
            hard = (int32_t)k;
            break;
        }
        if ((c->flags & (IFC_BK_LINE | IFC_BK_GRAPHEME)) != 0) {
            best = (int32_t)k;
        } else {
            emerg = (int32_t)k;
        }
    }
    int32_t pick = -1;
    if (hard >= 0) {
        pick = hard;
        r.forced = true;
    } else if (best >= 0) {
        pick = best;
    } else if (emerg >= 0) {
        pick = emerg;
    } else if (over >= 0) {
        pick = over;
        r.overflow = true;
        r.forced = (x->cand.data[over].flags & IFC_BK_HARD) != 0;
    }
    if (pick >= 0) {
        const ifc_cand *c = &x->cand.data[pick];
        r.e = c->pos;
        r.hw = c->hw;
        r.hs = c->hs;
        if (!r.overflow) {
            r.overflow = cand_w(x, s, c, false) - end_trim(x, s, c->pos, nullptr) > lim;
        }
    }
    return r;
}

/* The paragraph position after the collapsible spaces that start at s (removed at a line start). */
static uint32_t skip_spaces(const ifc_st *x, uint32_t s)
{
    for (;;) {
        uint32_t i = start_of(x, s);
        while (i < x->it.len && x->it.data[i].p == s &&
               (x->it.data[i].kind == IK_OPEN || x->it.data[i].kind == IK_CLOSE ||
                x->it.data[i].kind == IK_FLOAT || (x->it.data[i].kind == IK_TEXT && x->it.data[i].n == 0))) {
            i++;
        }
        if (i >= x->it.len) {
            return s;
        }
        const ifc_item *t = &x->it.data[i];
        if (t->kind != IK_TEXT || t->n == 0 || t->p > s || s >= t->p + t->n ||
            !strips_start(t->box->style)) {
            return s;
        }
        uint32_t k = t->b0 + (s - t->p);
        uint32_t end = t->b0 + t->n;
        uint32_t m = k;
        while (m < end && t->box->text[m] == ' ') {
            m++;
        }
        if (m == k) {
            return s;
        }
        s = t->p + (m - t->b0);
        if (m < end) {
            return s;
        }
    }
}

/* ------------------------------------------------------------------ the units of a line */

static void push_unit(ifc_st *x, const ifc_unit *u)
{
    embi_vec_push(&x->ln.u, *u);
}

/* One run of a text item [a, b) (box bytes) at one bidi level. */
static void flush_text(ifc_st *x, int32_t item, uint32_t a, uint32_t b, int lv)
{
    if (b <= a) {
        return;
    }
    const ifc_item *t = &x->it.data[item];
    ifc_unit u;
    memset(&u, 0, sizeof u);
    u.kind = UK_TEXT;
    u.level = (uint8_t)lv;
    u.item = item;
    u.box = t->box;
    u.lt = t->lt;
    u.b0 = a;
    u.b1 = b;
    u.a = t->a;
    u.d = t->d;
    u.ca = t->ca;
    u.cd = t->cd;
    u.w = twi(t, a, b);
    u.space = b - a == 1 && t->box->text[a] == ' ';
    push_unit(x, &u);
}

/* The justification opportunities of a line: none, the collapsible spaces, or every character (for
 * ideographic text without spaces, text-justify: auto). */
enum {
    JS_NONE = 0,
    JS_SPACES,
    JS_CHARS,
};

/* Whether the UTF-8 character at p (n bytes remain) is ideographic (U+3000 to U+9FFF and the CJK
 * extensions U+20000 to U+2FFFF), the characters that justify between themselves. */
static bool ideographic(const char *p, uint32_t n)
{
    uint8_t c0 = (uint8_t)p[0];
    if (c0 >= 0xE3 && c0 <= 0xE9) {
        return n >= 3;
    }
    if (c0 == 0xF0 && n >= 4) {
        uint32_t cp = ((uint32_t)(c0 & 7) << 18) | ((uint32_t)((uint8_t)p[1] & 0x3F) << 12) |
                      ((uint32_t)((uint8_t)p[2] & 0x3F) << 6) | ((uint8_t)p[3] & 0x3Fu);
        return cp >= 0x20000 && cp <= 0x2FFFF;
    }
    return false;
}

/* The units of a text item over the paragraph bytes [lo, hi): split at the bidi level changes, at
 * the soft hyphens and line feeds (which take no room), and at the spaces when justifying. */
static void add_text_units(ifc_st *x, int32_t item, uint32_t lo, uint32_t hi, uint8_t split)
{
    const ifc_item *t = &x->it.data[item];
    const char *text = t->box->text;
    uint32_t a = t->b0 + (lo - t->p), bnd = t->b0 + (hi - t->p);
    if (split == JS_NONE && bnd > a) {
        /* W3q: a range at one bidi level with no line feed, tab or possible soft hyphen (lead byte 0xC2) is one unit. */
        uint32_t lv0 = level_at(x, lo);
        if (x->para == nullptr || hi <= x->lv_hi) {
            bool plain = true;
            uint32_t k = a;
            /* eight bytes at a time: a byte of w equal to 9, 10 or 0xC2 makes one of the three zero tests fire */
            for (; k + 8 <= bnd && plain; k += 8) {
                uint64_t w;
                memcpy(&w, text + k, 8);
                const uint64_t lo1 = 0x0101010101010101ull, hi8 = 0x8080808080808080ull;
                uint64_t t9 = w ^ (lo1 * 9u), t10 = w ^ (lo1 * 10u), tc2 = w ^ (lo1 * 0xC2u);
                plain = (((t9 - lo1) & ~t9) | ((t10 - lo1) & ~t10) | ((tc2 - lo1) & ~tc2)) & hi8 ? false : true;
            }
            for (; k < bnd && plain; k++) {
                uint8_t c0 = (uint8_t)text[k];
                plain = !(c0 == 10 || c0 == 9 || c0 == 0xC2);
            }
            if (plain) {
                flush_text(x, item, a, bnd, (int)lv0);
                return;
            }
        }
    }
    uint32_t seg = a;
    int cur = -1;
    uint32_t k = a;
    while (k < bnd) {
        uint8_t c0 = (uint8_t)text[k];
        uint32_t clen = utf8_len(c0);
        if (k + clen > bnd) {
            clen = bnd - k;
        }
        if (c0 == '\n' || (c0 == 0xC2 && k + 1 < bnd && (uint8_t)text[k + 1] == 0xAD)) {
            if (cur >= 0) {
                flush_text(x, item, seg, k, cur);
            }
            k += clen;
            seg = k;
            cur = -1;
            continue;
        }
        int lv = (int)level_at(x, t->p + (k - t->b0));
        if (cur < 0) {
            cur = lv;
        } else if (lv != cur) {
            flush_text(x, item, seg, k, cur);
            seg = k;
            cur = lv;
        }
        if (c0 == 9) {
            /* A preserved tab is a unit of its own, whose width the line gives it. It is a segment separator, at
             * the paragraph's level (UAX 9 rule L1). */
            flush_text(x, item, seg, k, cur);
            flush_text(x, item, k, k + 1, (int)x->base);
            x->ln.u.data[x->ln.u.len - 1].tab = true;
            seg = k + 1;
            cur = -1;
        } else if (split == JS_SPACES && c0 == ' ') {
            flush_text(x, item, seg, k, cur);
            flush_text(x, item, k, k + 1, cur);
            seg = k + 1;
            cur = -1;
        } else if (split == JS_CHARS && ideographic(text + k, bnd - k)) {
            /* Inter-character justification: every ideographic character is an opportunity (a Latin
             * word stays whole even when the line mixes both). */
            flush_text(x, item, seg, k, cur);
            flush_text(x, item, k, k + clen, cur);
            x->ln.u.data[x->ln.u.len - 1].space = true;
            seg = k + clen;
            cur = -1;
        }
        k += clen;
    }
    if (cur >= 0) {
        flush_text(x, item, seg, bnd, cur);
    }
}

/* The units of the items [j0, j1) that fall in the content [s, e_c). */
static void build_units(ifc_st *x, uint32_t s, uint32_t e_c, uint32_t j0, uint32_t j1, uint8_t split)
{
    for (uint32_t i = j0; i < j1; i++) {
        const ifc_item *t = &x->it.data[i];
        switch (t->kind) {
        case IK_TEXT: {
            if (t->n == 0 || t->outside) {
                break;
            }
            uint32_t lo = t->p > s ? t->p : s;
            uint32_t hi = t->p + t->n < e_c ? t->p + t->n : e_c;
            if (lo < hi) {
                add_text_units(x, (int32_t)i, lo, hi, split);
            }
            break;
        }
        case IK_OPEN:
        case IK_CLOSE: {
            ifc_unit u;
            memset(&u, 0, sizeof u);
            u.kind = t->kind == IK_OPEN ? UK_OPEN : UK_CLOSE;
            u.item = (int32_t)i;
            u.box = t->box;
            u.w = t->w;
            push_unit(x, &u);
            break;
        }
        case IK_ATOM: {
            if (t->outside) {
                break;
            }
            ifc_unit u;
            memset(&u, 0, sizeof u);
            u.kind = UK_ATOM;
            u.item = (int32_t)i;
            u.box = t->box;
            u.w = t->w;
            u.level = (uint8_t)level_at(x, t->p);
            push_unit(x, &u);
            break;
        }
        default:
            break;
        }
    }
}

/* The paragraph bytes [*p0, *p1) of a unit; false for the ellipsis. */
static bool unit_range(const ifc_st *x, const ifc_unit *u, uint32_t *p0, uint32_t *p1)
{
    if (u->item < 0) {
        return false;
    }
    const ifc_item *t = &x->it.data[u->item];
    if (u->kind == UK_TEXT) {
        *p0 = t->p + (u->b0 - t->b0);
        *p1 = t->p + (u->b1 - t->b0);
    } else {
        *p0 = t->p;
        *p1 = t->p + t->n;
    }
    return true;
}

/* The level of the isolate controls (LRI, RLI, FSI, PDI) between two units that follow each other in logical order, or
 * -1 when there is none. Such a control keeps the level outside its isolate (UAX 9 rule X5a, X6a), so it separates the
 * isolate's runs from the text around it in the reordering (an inside ::marker, CSS Lists 3 section 3.2). */
static int iso_barrier(const ifc_st *x, const ifc_unit *a, const ifc_unit *b)
{
    uint32_t a0, a1, b0, b1;
    if (!unit_range(x, a, &a0, &a1) || !unit_range(x, b, &b0, &b1) || b0 < a1 + 3 || b0 > plen(x)) {
        return -1;
    }
    const uint8_t *d = (const uint8_t *)x->buf.data;
    int lv = -1;
    for (uint32_t k = a1; k + 3 <= b0; k++) {
        if (d[k] == 0xE2 && d[k + 1] == 0x81 && d[k + 2] >= 0xA6 && d[k + 2] <= 0xA9) {
            int l = (int)level_at(x, k);
            lv = lv < 0 || l < lv ? l : lv;
            k += 2;
        }
    }
    return lv;
}

/* The edges of an inline box that opens no embedding (unicode-bidi: normal) but whose direction has the other parity
 * than all of its text on the line, an rtl box around LTR text or an ltr box around RTL text (CSS 2.1 section 9.10:
 * side margins are unaffected by directionality). The box's start side is the box direction's side of the fragment
 * of its text, not a side of the paragraph: the edges join the level of their text and exchange their places in the
 * logical order, so that the reordering draws the end edge of an rtl box on the left of LTR text, next to it, and
 * leaves the neighbouring text of the same level in one run. Boxes whose text has several levels, boxes that continue
 * from or to another line and boxes that open an embedding keep the levels place_vertical gave their edges. */
static void mirror_edges(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    bool any = false;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ln->u.data[i].mirror = 0;
        any = any || ln->u.data[i].level != 0;
    }
    if (!any) {
        return;
    }
    EMBI_VEC(uint32_t) stk;
    EMBI_VEC(int32_t) lvs;
    memset(&stk, 0, sizeof stk);
    memset(&lvs, 0, sizeof lvs);
    int32_t all = -1; /* the level of all the text of the line so far: that of a box continuing from the line before */
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->kind == UK_OPEN) {
            embi_vec_push(&stk, i);
            embi_vec_push(&lvs, -1);
        } else if (u->kind == UK_CLOSE) {
            const ifc_item *ci = &x->it.data[u->item];
            if (stk.len == 0 || ln->u.data[stk.data[stk.len - 1]].item != ci->parent) {
                /* The box began on an earlier line: its fragment is the text so far. */
                const ifc_item *it = ci->parent >= 0 ? &x->it.data[ci->parent] : nullptr;
                if (it != nullptr && all >= 0 && it->bm == BIDI_NONE && (all & 1) != (it->rtl ? 1 : 0)) {
                    u->mirror = 2;
                    u->level = (uint8_t)all;
                }
                continue;
            }
            uint32_t oi = stk.data[--stk.len];
            int32_t lv = lvs.data[--lvs.len];
            const ifc_item *it = &x->it.data[ln->u.data[oi].item];
            if (lv >= 0 && it->bm == BIDI_NONE && (lv & 1) != (it->rtl ? 1 : 0)) {
                ln->u.data[oi].mirror = u->mirror = 1;
                ln->u.data[oi].level = u->level = (uint8_t)lv;
            }
        } else if (!u->bare) {
            all = all == -1 || all == u->level ? (int32_t)u->level : -2;
            for (uint32_t k = 0; k < lvs.len; k++) {
                lvs.data[k] = lvs.data[k] == -1 || lvs.data[k] == u->level ? (int32_t)u->level : -2;
            }
        }
    }
    /* The boxes that go on to the next line: their start edge is after the text of this fragment. */
    for (uint32_t k = 0; k < stk.len; k++) {
        ifc_unit *u = &ln->u.data[stk.data[k]];
        const ifc_item *it = &x->it.data[u->item];
        if (lvs.data[k] >= 0 && it->bm == BIDI_NONE && (lvs.data[k] & 1) != (it->rtl ? 1 : 0)) {
            u->mirror = 2;
            u->level = (uint8_t)lvs.data[k];
        }
    }
    embi_vec_free(&stk);
    embi_vec_free(&lvs);
}

/* The logical sequence of visual_order with the edges of mirrored boxes moved: a box with both edges on the line
 * exchanges them; the end edge of a box that began on an earlier line goes to the front, the start edge of a box that
 * goes on to the next line to the end, the outer box's outside the inner box's. */
static void mirror_swap(ifc_st *x, int32_t *seq, uint32_t n)
{
    ifc_line *ln = &x->ln;
    EMBI_VEC(uint32_t) stk; /* positions in seq of the open mirrored edges */
    memset(&stk, 0, sizeof stk);
    bool lone = false;
    for (uint32_t k = 0; k < n; k++) {
        int32_t e = seq[k];
        if (e < 0 || ln->u.data[e].mirror == 0) {
            continue;
        }
        if (ln->u.data[e].mirror == 2) {
            lone = true;
        } else if (ln->u.data[e].kind == UK_OPEN) {
            embi_vec_push(&stk, k);
        } else if (stk.len > 0) {
            uint32_t o = stk.data[--stk.len];
            int32_t t = seq[o];
            seq[o] = seq[k];
            seq[k] = t;
        }
    }
    embi_vec_free(&stk);
    if (!lone) {
        return;
    }
    EMBI_VEC(int32_t) out;
    memset(&out, 0, sizeof out);
    for (uint32_t k = n; k-- > 0;) {
        if (seq[k] >= 0 && ln->u.data[seq[k]].mirror == 2 && ln->u.data[seq[k]].kind == UK_CLOSE) {
            embi_vec_push(&out, seq[k]);
        }
    }
    /* out holds the lone end edges outermost first (reverse logical order); the sequence itself follows */
    for (uint32_t k = 0; k < n; k++) {
        if (seq[k] < 0 || ln->u.data[seq[k]].mirror != 2) {
            embi_vec_push(&out, seq[k]);
        }
    }
    for (uint32_t k = n; k-- > 0;) {
        if (seq[k] >= 0 && ln->u.data[seq[k]].mirror == 2 && ln->u.data[seq[k]].kind == UK_OPEN) {
            embi_vec_push(&out, seq[k]);
        }
    }
    for (uint32_t k = 0; k < n && k < out.len; k++) {
        seq[k] = out.data[k];
    }
    embi_vec_free(&out);
}

/* The visual order of the units (UAX 9 rule L2): ord[k] is the unit drawn k-th from the left. The isolate controls
 * between units take part as entries of their own level (iso_barrier), removed after the reordering. */
static void visual_order(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    uint32_t n = ln->u.len;
    ln->ord.len = 0;
    if (n == 0) {
        return;
    }
    bool any = false;
    for (uint32_t i = 0; i < n && !any; i++) {
        any = ln->u.data[i].level != 0;
    }
    if (!any) {
        for (uint32_t i = 0; i < n; i++) {
            embi_vec_push(&ln->ord, i);
        }
        return;
    }
    /* seq: a unit index, or -(level + 1) for an isolate control. */
    EMBI_VEC(int32_t) seq;
    memset(&seq, 0, sizeof seq);
    int maxl = 0, minl = 255;
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&seq, (int32_t)i);
        int l = ln->u.data[i].level;
        maxl = l > maxl ? l : maxl;
        minl = l < minl ? l : minl;
        if (i + 1 < n && maxl > 0) {
            int b = iso_barrier(x, &ln->u.data[i], &ln->u.data[i + 1]);
            if (b >= 0) {
                embi_vec_push(&seq, -(int32_t)b - 1);
                minl = b < minl ? b : minl;
            }
        }
    }
    mirror_swap(x, seq.data, seq.len);
    uint32_t m = seq.len;
    int lo = (minl & 1) ? minl : minl + 1;
    for (int lv = maxl; lv >= lo && lv > 0; lv--) {
        uint32_t i = 0;
        while (i < m) {
            int32_t e = seq.data[i];
            int el = e >= 0 ? (int)ln->u.data[e].level : -e - 1;
            if (el < lv) {
                i++;
                continue;
            }
            uint32_t j = i;
            while (j < m) {
                int32_t f = seq.data[j];
                int fl = f >= 0 ? (int)ln->u.data[f].level : -f - 1;
                if (fl < lv) {
                    break;
                }
                j++;
            }
            for (uint32_t a = i, b = j - 1; a < b; a++, b--) {
                int32_t tmp = seq.data[a];
                seq.data[a] = seq.data[b];
                seq.data[b] = tmp;
            }
            i = j;
        }
    }
    for (uint32_t k = 0; k < m; k++) {
        if (seq.data[k] >= 0) {
            embi_vec_push(&ln->ord, (uint32_t)seq.data[k]);
        }
    }
    embi_vec_free(&seq);
}

/* The shift of an OPEN or ATOM item relative to its parent's baseline (CSS 2.1 10.8.1). The
 * vertical-align classes top and bottom are set here too; their shift comes with the line. The
 * parent's content area (its font's ascent and descent, without the half-leading of line-height)
 * is what text-top and text-bottom align to; the box itself is the content area of an inline box
 * and the margin box of an atomic inline. */
static void item_rel(ifc_st *x, uint32_t i)
{
    ifc_item *it = &x->it.data[i];
    float pca = x->ca0, pcd = x->cd0, pxh = x->xh0;
    const embi_box *pbox = x->root;
    if (it->parent >= 0) {
        const ifc_item *par = &x->it.data[it->parent];
        pca = par->ca;
        pcd = par->cd;
        pxh = par->xh;
        pbox = par->box;
    }
    it->rel = 0.0f;
    const embi_value *v = embi_style_value(it->box->style, EMBI_PROP_vertical_align);
    if (v == nullptr) {
        return;
    }
    bool atom = it->kind == IK_ATOM;
    /* The box's own extent above and below its baseline. */
    float ba = it->a, bd = it->d; /* the inline box: its line-height's half-leadings are part of it (vertical-align-117a) */
    if (v->kind == EMBI_V_KEYWORD) {
        switch (v->kw) {
        case EMBI_KW_sub:
            it->rel = -(embi_lay_font_px(x->c, pbox) / 5.0f + 1.0f);
            break;
        case EMBI_KW_super:
            it->rel = embi_lay_font_px(x->c, pbox) / 3.0f + 1.0f;
            break;
        case EMBI_KW_text_top:
            it->rel = pca - ba;
            break;
        case EMBI_KW_text_bottom:
            it->rel = bd - pcd;
            break;
        case EMBI_KW_middle:
            it->rel = pxh / 2.0f - (ba - bd) / 2.0f;
            break;
        case EMBI_KW_top:
            it->va = VA_TOP;
            break;
        case EMBI_KW_bottom:
            it->va = VA_BOTTOM;
            break;
        default:
            break; /* baseline */
        }
        return;
    }
    if (v->kind == EMBI_V_PERCENT) {
        /* A percentage refers to the element's own line-height (CSS 2.1 10.8.1). An atomic inline's
         * a and d are its margin box, so its line-height comes from its own style. */
        float la = it->a, ld = it->d;
        if (atom) {
            const ifc_met *m = metrics(x, it->box->style);
            la = m->a;
            ld = m->d;
        }
        it->rel = (float)v->num / 100.0f * (la + ld);
    } else if (v->kind == EMBI_V_LENGTH) {
        float len = embi_lay_len(x->c, it->box, v, NAN);
        it->rel = isnan(len) ? 0.0f : len;
    } else if (v->kind == EMBI_V_CALC) {
        /* calc(): its percentages refer to the line-height, as a bare percentage does. */
        float la = it->a, ld = it->d;
        if (atom) {
            const ifc_met *m = metrics(x, it->box->style);
            la = m->a;
            ld = m->d;
        }
        float len = embi_lay_len(x->c, it->box, v, la + ld);
        it->rel = isnan(len) ? 0.0f : len;
    }
}

static void prepare_items(ifc_st *x)
{
    for (uint32_t i = 0; i < x->it.len; i++) {
        uint8_t k = x->it.data[i].kind;
        if (k == IK_OPEN || k == IK_ATOM) {
            item_rel(x, i);
        }
    }
}

/* ------------------------------------------------------------------ the vertical placement */

typedef struct ifc_stk {
    float B;
    bool tb;   /* inside a top or bottom aligned box: its content does not reach the line box */
} ifc_stk;

/* The accumulated extents of the line: the baseline group above and below the baseline, and the
 * top and bottom aligned boxes' heights. */
typedef struct ifc_ext {
    float up, dn, th, bh;
    bool has_top, has_bot;
} ifc_ext;

static void extend_ext(ifc_ext *e, float B, float a, float d, bool tb, uint8_t va)
{
    if (tb) {
        float h = a + d;
        if (va == VA_TOP) {
            e->has_top = true;
            e->th = e->th > h ? e->th : h;
        } else {
            e->has_bot = true;
            e->bh = e->bh > h ? e->bh : h;
        }
        return;
    }
    e->up = e->up > B + a ? e->up : B + a;
    e->dn = e->dn > d - B ? e->dn : d - B;
}

/* Gives each unit and each piece its baseline shift, walking the line in logical order. The pieces
 * open at the line's start come first (the continuations). When ext is set the walk also collects
 * the extents; the aligned boxes keep the shifts they were given (their B is set before the second
 * walk). */
/* The stack of walk_shifts lives on the C stack until a line nests more than 32 inline boxes (it ran on the
 * heap once per line before W3p). */
typedef struct shift_stk {
    ifc_stk *data;
    uint32_t len, cap;
    ifc_stk buf[32];
} shift_stk;

static void shift_push(shift_stk *s, ifc_stk v)
{
    if (s->len == s->cap) {
        ifc_stk *n = embi_realloc(s->data == s->buf ? nullptr : s->data,
                                  s->data == s->buf ? 0 : s->cap * sizeof(ifc_stk), s->cap * 2 * sizeof(ifc_stk));
        if (n == nullptr) {
            return; /* out of memory: the box keeps its parent's shift */
        }
        if (s->data == s->buf) {
            memcpy(n, s->buf, s->len * sizeof(ifc_stk));
        }
        s->data = n;
        s->cap *= 2;
    }
    s->data[s->len++] = v;
}

static void walk_shifts(ifc_st *x, bool ext, ifc_ext *e)
{
    ifc_line *ln = &x->ln;
    shift_stk st;
    st.data = st.buf;
    st.len = 0;
    st.cap = 32;
    ifc_stk base = {0.0f, false};
    shift_push(&st, base);
    for (uint32_t r = 0; r < ln->ncont; r++) {
        ifc_rec *rc = &ln->recs.data[r];
        const ifc_item *o = &x->it.data[rc->item];
        ifc_stk top = st.data[st.len - 1];
        bool tb = o->va != VA_BASE;
        float B = tb ? (ext ? top.B : rc->B) : top.B + o->rel;
        rc->B = B;
        if (ext && !top.tb && !ln->bell_only) {
            extend_ext(e, B, o->a, o->d, tb, o->va);
        }
        ifc_stk nx = {B, top.tb || tb};
        shift_push(&st, nx);
    }
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        ifc_stk top = st.data[st.len - 1];
        switch (u->kind) {
        case UK_OPEN: {
            const ifc_item *o = &x->it.data[u->item];
            bool tb = o->va != VA_BASE;
            float B = tb ? (ext ? top.B : u->B) : top.B + o->rel;
            u->B = B;
            if (u->rec >= 0) {
                ln->recs.data[u->rec].B = B;
            }
            if (ext && !top.tb && !ln->bell_only) {
                extend_ext(e, B, o->a, o->d, tb, o->va);
            }
            ifc_stk nx = {B, top.tb || tb};
            shift_push(&st, nx);
            break;
        }
        case UK_CLOSE:
            if (st.len > 1) {
                st.len--;
            }
            break;
        case UK_TEXT:
            u->B = top.B;
            if (ext && !top.tb) {
                extend_ext(e, top.B, u->a, u->d, false, VA_BASE);
            }
            break;
        case UK_ATOM: {
            const ifc_item *o = &x->it.data[u->item];
            bool tb = o->va != VA_BASE;
            float B = tb ? (ext ? top.B : u->B) : top.B + o->rel;
            u->B = B;
            if (ext && !top.tb) {
                extend_ext(e, B, o->a, o->d, tb, o->va);
            }
            break;
        }
        default:
            break;
        }
    }
    if (st.data != st.buf) {
        embi_free(st.data, st.cap * sizeof(ifc_stk));
    }
}

/* The line box and its baseline: the strut and the units give the baseline group; top and bottom
 * aligned boxes are placed at the top and bottom of the line box; the second walk gives the boxes
 * inside the aligned ones their shifts. */
static void place_vertical(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    /* The levels of the edges: the nearest level on either side, else the base. */
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->kind != UK_OPEN && u->kind != UK_CLOSE) {
            continue;
        }
        int prev = -1, next = -1;
        for (uint32_t j = i; j > 0; j--) {
            const ifc_unit *q = &ln->u.data[j - 1];
            if (q->kind == UK_TEXT || q->kind == UK_ATOM) {
                prev = q->level;
                break;
            }
        }
        for (uint32_t j = i + 1; j < ln->u.len; j++) {
            const ifc_unit *q = &ln->u.data[j];
            if (q->kind == UK_TEXT || q->kind == UK_ATOM) {
                next = q->level;
                break;
            }
        }
        int lv;
        if (prev >= 0 && next >= 0) {
            lv = prev < next ? prev : next;
        } else if (prev >= 0) {
            lv = prev;
        } else if (next >= 0) {
            lv = next;
        } else {
            lv = x->base;
        }
        /* The edge belongs to its box's own direction: an rtl box's edges sit at an odd level and
         * an ltr box's at an even one, the nearest such level not above the neighbours', so a box
         * with Latin text in an rtl paragraph still has its start edge on the right. Between two
         * units of one level the edge stays in their run. */
        if (prev != next && u->item >= 0 && (lv & 1) != (x->it.data[u->item].rtl ? 1 : 0)) {
            lv = lv > (int)x->base ? lv - 1 : lv + 1;
        }
        u->level = (uint8_t)lv;
    }
    mirror_edges(x);
    visual_order(x);

    ifc_ext e;
    memset(&e, 0, sizeof e);
    e.up = x->a0;
    e.dn = x->d0;
    walk_shifts(x, true, &e);
    float H0 = e.up + e.dn;
    float H = H0;
    if (e.th > H) {
        H = e.th;
    }
    if (e.bh > H) {
        H = e.bh;
    }
    float base = e.up;
    if (!e.has_top && e.has_bot && H > H0) {
        base = e.up + (H - H0);
    }
    ln->H = H;
    ln->base = base;
    if (e.has_top || e.has_bot) {
        for (uint32_t i = 0; i < ln->u.len; i++) {
            ifc_unit *u = &ln->u.data[i];
            if (u->kind != UK_OPEN && u->kind != UK_ATOM) {
                continue;
            }
            const ifc_item *o = &x->it.data[u->item];
            if (o->va == VA_TOP) {
                u->B = base - o->a;
            } else if (o->va == VA_BOTTOM) {
                u->B = base + o->d - H;
            }
        }
        for (uint32_t r = 0; r < ln->recs.len; r++) {
            ifc_rec *rc = &ln->recs.data[r];
            const ifc_item *o = &x->it.data[rc->item];
            if (o->va == VA_TOP) {
                rc->B = base - o->a;
            } else if (o->va == VA_BOTTOM) {
                rc->B = base + o->d - H;
            }
        }
        walk_shifts(x, false, &e);
    }
}

/* The pieces of the inline boxes: the records of the boxes open at the start come first, then one
 * per OPEN unit. Their extents are set by measure_pieces. */
static void make_records(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    ln->recs.len = 0;
    for (uint32_t k = 0; k < x->open.len; k++) {
        ifc_rec rc;
        memset(&rc, 0, sizeof rc);
        rc.item = x->open.data[k];
        const ifc_item *o = &x->it.data[rc.item];
        rc.a = o->ca;
        rc.d = o->cd;
        rc.el = INFINITY;
        rc.er = -INFINITY;
        embi_vec_push(&ln->recs, rc);
    }
    ln->ncont = ln->recs.len;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->kind != UK_OPEN) {
            continue;
        }
        const ifc_item *o = &x->it.data[u->item];
        ifc_rec rc;
        memset(&rc, 0, sizeof rc);
        rc.item = u->item;
        rc.a = o->ca;
        rc.d = o->cd;
        rc.el = INFINITY;
        rc.er = -INFINITY;
        u->rec = (int32_t)ln->recs.len;
        embi_vec_push(&ln->recs, rc);
    }
}

static void extend_rec(ifc_rec *rc, float l, float r)
{
    rc->has = true;
    if (l < rc->el) {
        rc->el = l;
    }
    if (r > rc->er) {
        rc->er = r;
    }
}

/* Gives the extent [l, r] of the unit at visual place v to a piece. With the units in logical order
 * (no reordering) the pieces are one run each and just grow; otherwise the extents are collected and
 * split into visual runs by close_runs. */
static void note_piece(ifc_line *ln, bool reord, uint32_t rec, uint32_t v, float l, float r)
{
    if (!reord) {
        extend_rec(&ln->recs.data[rec], l, r);
        return;
    }
    ifc_mem m = {rec, v, l, r};
    embi_vec_push(&ln->mem, m);
}

static int mem_cmp(const void *pa, const void *pb)
{
    const ifc_mem *a = pa, *b = pb;
    if (a->rec != b->rec) {
        return a->rec < b->rec ? -1 : 1;
    }
    return a->v < b->v ? -1 : (a->v > b->v ? 1 : 0);
}

/* The visual runs of the pieces of a reordered line. A piece covers only the units logically inside
 * it, so its extent is the union of those units' places, and when other units lie between them in
 * the visual order it is cut there: one run, one BOX fragment, per contiguous stretch. Units without
 * width between two stretches (the edges of other boxes) do not cut. */
static void close_runs(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    if (ln->mem.len == 0) {
        return;
    }
    SDL_qsort(ln->mem.data, ln->mem.len, sizeof(ifc_mem), mem_cmp);
    uint32_t i = 0;
    while (i < ln->mem.len) {
        uint32_t rec = ln->mem.data[i].rec;
        ifc_rec *rc = &ln->recs.data[rec];
        rc->r0 = ln->runs.len;
        rc->has = true;
        float l = ln->mem.data[i].l, r = ln->mem.data[i].r;
        uint32_t pv = ln->mem.data[i].v;
        bool wide = false;
        uint32_t first = ln->runs.len;
        for (i++;; i++) {
            const ifc_mem *e = i < ln->mem.len && ln->mem.data[i].rec == rec ? &ln->mem.data[i] : nullptr;
            bool joins = false;
            if (e != nullptr) {
                joins = true;
                for (uint32_t q = pv + 1; q < e->v; q++) {
                    if (ln->u.data[ln->ord.data[q]].w != 0.0f) {
                        joins = false;
                        break;
                    }
                }
            }
            if (joins) {
                if (e->l < l) {
                    l = e->l;
                }
                if (e->r > r) {
                    r = e->r;
                }
                pv = e->v;
                continue;
            }
            ifc_run run = {l, r};
            embi_vec_push(&ln->runs, run);
            wide = wide || r > l;
            if (e == nullptr) {
                break;
            }
            l = e->l;
            r = e->r;
            pv = e->v;
        }
        /* Stretches without width are only edges of an empty piece; they are not worth a fragment
         * when the piece has a real one. */
        if (wide) {
            uint32_t w = first;
            for (uint32_t k = first; k < ln->runs.len; k++) {
                if (ln->runs.data[k].r > ln->runs.data[k].l) {
                    ln->runs.data[w++] = ln->runs.data[k];
                }
            }
            ln->runs.len = w;
        }
        rc->rn = ln->runs.len - rc->r0;
    }
}

/* The extents of the pieces, from the placed units: a unit extends every piece that is open at it,
 * and an OPEN or CLOSE unit also sets its own piece's inner edge. */
static void measure_pieces(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    bool reord = false;
    EMBI_VEC(uint32_t) vis;
    memset(&vis, 0, sizeof vis);
    for (uint32_t k = 0; k < ln->ord.len; k++) {
        if (ln->ord.data[k] != k) {
            reord = true;
        }
    }
    if (reord) {
        for (uint32_t k = 0; k < ln->ord.len; k++) {
            embi_vec_push(&vis, 0u);
        }
        for (uint32_t k = 0; k < ln->ord.len; k++) {
            vis.data[ln->ord.data[k]] = k;
        }
    }
    ln->runs.len = 0;
    ln->mem.len = 0;
    EMBI_VEC(int32_t) stk;
    memset(&stk, 0, sizeof stk);
    for (uint32_t k = 0; k < ln->ncont; k++) {
        embi_vec_push(&stk, (int32_t)k);
    }
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        float l = u->x, r = u->x + u->w;
        uint32_t v = reord ? vis.data[i] : i;
        if (u->kind == UK_OPEN) {
            /* The start edges: in ltr the box's border starts after its margin and its content
             * after the whole unit; in rtl the content ends at the unit's left edge and the border
             * ends after the padding and border (the unit's start side is on the right). */
            const ifc_item *o = &x->it.data[u->item];
            for (uint32_t k = 0; k < stk.len; k++) {
                note_piece(ln, reord, (uint32_t)stk.data[k], v, l, r);
            }
            if (o->rtl) {
                note_piece(ln, reord, (uint32_t)u->rec, v, u->x, u->x + o->bl);
            } else {
                note_piece(ln, reord, (uint32_t)u->rec, v, u->x + o->ml, u->x + u->w);
            }
            embi_vec_push(&stk, u->rec);
        } else if (u->kind == UK_CLOSE) {
            /* The closed box (the top) gets only the extent without its end margin. */
            for (uint32_t k = 0; k + 1 < stk.len; k++) {
                note_piece(ln, reord, (uint32_t)stk.data[k], v, l, r);
            }
            if (stk.len > 0) {
                const ifc_item *o = &x->it.data[u->item];
                uint32_t top = (uint32_t)stk.data[stk.len - 1];
                if (o->rtl) {
                    note_piece(ln, reord, top, v, u->x + o->mr, u->x + u->w);
                } else {
                    note_piece(ln, reord, top, v, u->x, u->x + u->w - o->mr);
                }
                ln->recs.data[top].closed = true;
                stk.len--;
            }
        } else if (!u->bare) {
            for (uint32_t k = 0; k < stk.len; k++) {
                note_piece(ln, reord, (uint32_t)stk.data[k], v, l, r);
            }
        }
    }
    embi_vec_free(&stk);
    embi_vec_free(&vis);
    if (reord) {
        close_runs(x);
        return;
    }
    for (uint32_t k = 0; k < ln->recs.len; k++) {
        ifc_rec *rc = &ln->recs.data[k];
        rc->r0 = ln->runs.len;
        rc->rn = 0;
        if (rc->has) {
            ifc_run run = {rc->el, rc->er};
            embi_vec_push(&ln->runs, run);
            rc->rn = 1;
        }
    }
}

/* The text-align of a line (CSS Text 3 sections 7.1 and 7.2): sets ln.align and ln.justify. */
static void resolve_align(ifc_st *x, bool last_or_forced)
{
    ifc_line *ln = &x->ln;
    bool rtl = x->base == 1;
    uint16_t ta = kwv(x->rs, EMBI_PROP_text_align);
    uint16_t tal = kwv(x->rs, EMBI_PROP_text_align_last);
    if (ta == EMBI_KW_match_parent) {
        /* The parent's value, with start and end resolved against the parent's direction. */
        const embi_box *pb = x->root->parent;
        ta = pb != nullptr && pb->style != nullptr ? kwv(pb->style, EMBI_PROP_text_align) : 0;
        if (pb != nullptr && pb->style != nullptr) {
            rtl = kwv(pb->style, EMBI_PROP_direction) == EMBI_KW_rtl;
        }
        if (ta == EMBI_KW_match_parent) {
            ta = 0;
        }
    }
    if (ta == 0) {
        ta = EMBI_KW_start;
    }
    if (ta == EMBI_KW_justify_all) {
        /* justify-all: justify, and the last line (and forced breaks) too unless text-align-last says otherwise */
        ta = EMBI_KW_justify;
        if (tal == 0 || tal == EMBI_KW_auto) {
            tal = EMBI_KW_justify;
        }
    }
    uint16_t eff = ta;
    if (last_or_forced) {
        if (tal == 0 || tal == EMBI_KW_auto) {
            eff = ta == EMBI_KW_justify ? EMBI_KW_start : ta;
        } else {
            eff = tal;
        }
    }
    ln->justify = eff == EMBI_KW_justify;
    bool to_right;
    switch (eff) {
    case EMBI_KW_center:
        ln->align = 2;
        return;
    case EMBI_KW_end:
        to_right = !rtl;
        break;
    case EMBI_KW_right:
        to_right = true;
        break;
    case EMBI_KW_start:
    case EMBI_KW_match_parent:
    case EMBI_KW_justify:
        to_right = rtl;
        break;
    default:
        to_right = false; /* left */
        break;
    }
    ln->align = to_right ? 1 : 0;
}

/* The justification of the line (CSS Text 3 section 7.3): the free space goes to the spaces. Sets
 * the content width. */
static void justify_units(ifc_st *x, float avail)
{
    ifc_line *ln = &x->ln;
    float sum = 0;
    uint32_t nsp = 0;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        sum += ln->u.data[i].w;
        if (ln->u.data[i].space) {
            nsp++;
        }
    }
    /* The hanging glyphs at the end take no room (line_hang); conditionally hanging spaces only for the part of them
     * that does not fit, and the separators before them only when none of them fits. */
    float hang = ln->hw_s;
    if (ln->hw_t > 0.0f) {
        float over = sum - avail;
        float cut = over <= 0.0005f ? 0.0f : over < ln->hw_t ? over : ln->hw_t;
        hang = over < ln->hw_t ? 0.0f : ln->hw_s;
        /* The part that does not fit is visually collapsed (its advance is taken off the last text units, whose
         * last bytes are those spaces), so nothing of it is drawn past the line's end. */
        for (uint32_t i = ln->u.len; i > 0 && cut > 0.0f; i--) {
            ifc_unit *lu = &ln->u.data[i - 1];
            if (lu->kind == UK_OPEN || lu->kind == UK_CLOSE) {
                continue;
            }
            if (lu->kind != UK_TEXT) {
                break;
            }
            float d = lu->w < cut ? lu->w : cut;
            lu->w -= d;
            sum -= d;
            cut -= d;
        }
    }
    sum -= hang;
    float freev = avail - sum;
    /* A preserved tab keeps its tab stop: only the spaces after the line's last tab are justified (the text before it
     * stays where the tab stops put it). */
    uint32_t from = 0;
    for (uint32_t i = 0; i < ln->u.len; i++) {
        if (ln->u.data[i].tab) {
            from = i + 1;
        }
    }
    if (from > 0) {
        nsp = 0;
        for (uint32_t i = from; i < ln->u.len; i++) {
            nsp += ln->u.data[i].space ? 1u : 0u;
        }
    }
    if (ln->justify && nsp > 0 && freev > 0 && !ln->tab_hangs) {
        float add = freev / (float)nsp;
        for (uint32_t i = from; i < ln->u.len; i++) {
            if (ln->u.data[i].space) {
                ln->u.data[i].w += add;
            }
        }
        sum = avail;
    }
    ln->cwc = sum;
}

/* The line's units in the content box: text-align places them, in visual order, in the band. */
static void position_units(ifc_st *x)
{
    ifc_line *ln = &x->ln;
    bool rtl = x->base == 1;
    float region_l = rtl ? ln->lb : ln->lb + ln->indent;
    float region_r = rtl ? ln->rb - ln->indent : ln->rb;
    float regw = region_r - region_l;
    float free = regw - ln->cwc;
    float pos;
    if (free < 0) {
        /* Content that does not fit is start-aligned (CSS Text 3 section 7.1): it overflows the end
         * edge, whatever the alignment. The start edge is the left one in ltr and the right one in rtl. */
        pos = rtl ? region_r - ln->cwc : region_l;
    } else {
        switch (ln->align) {
        case 1:
            pos = region_r - ln->cwc;
            break;
        case 2:
            pos = region_l + free / 2;
            break;
        default:
            pos = region_l;
            break;
        }
    }
    float cur = pos;
    for (uint32_t k = 0; k < ln->ord.len; k++) {
        ifc_unit *u = &ln->u.data[ln->ord.data[k]];
        u->x = cur;
        cur += u->w;
    }
}

/* ------------------------------------------------------------------ fragments */

static void set_box_edges(ifc_st *x, embi_frag *f, const embi_box *box, bool left, bool right)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(x->c, box, x->cw, &bdr, &pad, &mar);
    /* box-decoration-break: slice: the side where the box continues has no border or padding (the run's extent
     * already leaves them out). */
    if (!left) {
        bdr.l = pad.l = 0.0f;
    }
    if (!right) {
        bdr.r = pad.r = 0.0f;
    }
    embi_lay_frag_set_edges(x->c->ld, f, &bdr, &pad);
}

/* Creates the fragments of the line: the pieces of the inline boxes, the text runs and the atomic
 * inlines, and the hanging markers of the first line. */

/* Inserts a hanging marker's fragment f into the line fragment lf after prev (the marker inserted before it, or
 * nullptr for the front); returns f. */
static embi_frag *put_marker_frag(embi_frag *lf, embi_frag *prev, embi_frag *f)
{
    f->parent = lf;
    if (prev == nullptr) {
        f->next = lf->first;
        lf->first = f;
    } else {
        f->next = prev->next;
        prev->next = f;
    }
    if (f->next == nullptr) {
        lf->last = f;
    }
    return f;
}
static void emit_line(ifc_st *x, embi_frag *lf)
{
    ifc_line *ln = &x->ln;
    float base = ln->base;
    for (uint32_t r = 0; r < ln->recs.len; r++) {
        ifc_rec *rc = &ln->recs.data[r];
        const ifc_item *o = &x->it.data[rc->item];
        for (uint32_t q = 0; rc->has && q < rc->rn; q++) {
            const ifc_run *run = &ln->runs.data[rc->r0 + q];
            embi_frag *f = embi_lay_new_frag(x->c, o->box, EMBI_FK_BOX);
            bool start = r >= ln->ncont && (o->box->flags2 & EMBI_BF2_NOSTART) == 0;
            bool end = rc->closed && (o->box->flags2 & EMBI_BF2_NOEND) == 0;
            bool left = q == 0 && (o->rtl ? end : start);
            bool right = q + 1 == rc->rn && (o->rtl ? start : end);
            set_box_edges(x, f, o->box, left, right);
            float content_top = base - rc->B - rc->a;
            float top = content_top - embi_frag_bdr(f, 1) - embi_frag_pad(f, 1);
            f->x = run->l;
            f->w = run->r - run->l;
            f->y = top;
            f->h = rc->a + rc->d + embi_frag_bdr(f, 1) + embi_frag_pad(f, 1) + embi_frag_bdr(f, 3) + embi_frag_pad(f, 3);
            embi_lay_frag_append(lf, f);
        }
    }
    for (uint32_t i = 0; i < ln->u.len; i++) {
        ifc_unit *u = &ln->u.data[i];
        if (u->kind == UK_TEXT) {
            embi_frag *f = embi_lay_new_frag(x->c, u->box, EMBI_FK_TEXT);
            if (u->box->lt == nullptr) {
                u->box->lt = u->lt;
            }
            embi_frag_set_trange(f, u->b0, u->b1);
            f->x = u->x + u->dx;
            f->w = u->w - u->dx;
            f->y = base - u->B - u->ca;
            f->h = u->ca + u->cd;
            embi_lay_frag_append(lf, f);
        } else if (u->kind == UK_ATOM) {
            const ifc_item *o = &x->it.data[u->item];
            embi_frag *f = o->frag;
            if (f == nullptr) {
                continue;
            }
            f->x = u->x + o->ml;
            f->y = base - u->B - o->fa;
            embi_lay_frag_append(lf, f);
        }
    }
    /* The hanging markers of the first line: outside the content box, on the start side. They go first among the
     * line's fragments, so the content paints over them (CSS Lists 3 section 3.1; list-marker-paint-order). */
    embi_frag *mk_prev = nullptr;
    for (uint32_t i = ln->j0; i < ln->j1; i++) {
        const ifc_item *t = &x->it.data[i];
        if (!t->outside) {
            continue;
        }
        /* From the line's start before its own floats, after the text indent (outside-marker-with-float-in-content,
         * outside-marker-with-text-indent). */
        float ox = x->base == 1 ? ln->rb0 - ln->indent : ln->lb0 + ln->indent - t->w;
        if (t->kind == IK_TEXT && t->lt != nullptr) {
            embi_frag *f = embi_lay_new_frag(x->c, t->box, EMBI_FK_TEXT);
            if (t->box->lt == nullptr) {
                t->box->lt = t->lt;
            }
            embi_frag_set_trange(f, 0, t->box->text_len);
            f->x = ox;
            f->w = t->w;
            f->y = base - t->ca;
            f->h = t->ca + t->cd;
            mk_prev = put_marker_frag(lf, mk_prev, f);
        } else if (t->kind == IK_ATOM && t->frag != nullptr) {
            embi_frag *f = t->frag;
            f->x = ox + t->ml;
            f->y = base - t->fa;
            mk_prev = put_marker_frag(lf, mk_prev, f);
        }
    }
    for (embi_frag *k = lf->first; k != nullptr; k = k->next) {
        embi_lay_ov_add_child(lf, k);
    }
}

/* The LINE fragment of a line at top (relative to the content box). */
static embi_frag *new_line_frag(ifc_st *x, embi_frag *bf, const embi_ifc_frame *fr, float top,
                                float h)
{
    embi_frag *lf = embi_lay_new_frag(x->c, x->root, EMBI_FK_LINE);
    lf->x = fr->ox;
    lf->y = fr->oy + top;
    lf->w = x->cw;
    lf->h = h;
    embi_lay_frag_append(bf, lf);
    return lf;
}

/* ------------------------------------------------------------------ the lines */

/* The text-overflow value for the end of the lines (the second of two values when the base direction is
 * ltr, the first when rtl): the keyword ellipsis, or a string; null for clip. */
/* The block container whose overflow and text-overflow apply to the lines of the paragraph: the root, or for the lines of
 * an anonymous block box (which has the initial values of the non-inherited properties) the block container that holds it
 * (CSS Overflow 3 section 3.1; text-overflow-in-anonymous-block). */
static const embi_box *overflow_box(const ifc_st *x)
{
    const embi_box *b = x->root;
    while ((b->flags & EMBI_BF_ANON) != 0 && b->parent != nullptr && b->parent->kind == EMBI_BK_BLOCK &&
           b->parent->style != nullptr) {
        b = b->parent;
    }
    return b;
}

static const embi_value *ellipsis_value(const ifc_st *x)
{
    const embi_value *v = embi_style_value(overflow_box(x)->style, EMBI_PROP_text_overflow);
    if (v == nullptr) {
        return nullptr;
    }
    if (v->kind == EMBI_V_LIST) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(v, &n);
        if (n == 0) {
            return nullptr;
        }
        v = n >= 2 && x->base == 0 ? &items[1] : &items[0];
    }
    if (v->kind == EMBI_V_STRING || (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_ellipsis)) {
        return v;
    }
    return nullptr;
}

/* The ellipsis of text-overflow (U+2026 or the given string): a text run of the container's style, made once. */
static embi_lay_text *ellipsis_text_of(ifc_st *x, const embi_value *ev, embi_box **slot)
{
    if (*slot == nullptr) {
        /* Layout-time boxes live in the fragment arena: the box arena persists between layouts (W3p). */
        embi_box *b = (embi_box *)embi_lay_alloc(&x->c->ld->fa, sizeof(embi_box), alignof(embi_box));
        memset(b, 0, sizeof *b);
        b->kind = EMBI_BK_TEXT;
        b->flags = EMBI_BF_INLINE_LEVEL;
        b->style = x->rs;
        if (ev != nullptr && ev->kind == EMBI_V_STRING) {
            /* A line feed in the string is a collapsible segment break (CSS Overflow 4 section 3.1, CSS Text 3 section 4.1.2):
             * it becomes a space, or goes when it sits between two wide characters. */
            embi_sb pre = {0};
            for (uint32_t i = 0; i < ev->str.len; i++) {
                const uint8_t *q = (const uint8_t *)ev->str.ptr + i;
                if (i + 2 < ev->str.len && q[0] == 0xE2 && q[1] == 0x80 && (q[2] == 0xA8 || q[2] == 0xA9)) {
                    embi_sb_append_char(&pre, '\n'); /* U+2028 and U+2029 collapse like a forced break */
                    i += 2;
                } else if (q[0] == 0x0B || q[0] == 0x0C || (i + 1 < ev->str.len && q[0] == 0xC2 && q[1] == 0x85)) {
                    embi_sb_append_char(&pre, '\n'); /* U+000B, U+000C and U+0085 too (document text keeps U+000C, W4g5) */
                    i += q[0] == 0xC2 ? 1 : 0;
                } else {
                    embi_sb_append_char(&pre, (char)q[0]);
                }
            }
            embi_sb sb = {0};
            bool ps = false, iw = false;
            embi_lay_ws_process(&sb, (embi_str){pre.data, pre.len}, EMBI_WS_COLLAPSE, 0, &ps, &iw);
            embi_sb_free(&pre);
            uint32_t len = (uint32_t)sb.len;
            char *t = embi_lay_alloc(&x->c->ld->fa, len + 1, 1);
            if (len > 0) {
                memcpy(t, sb.data, len);
            }
            embi_sb_free(&sb);
            b->text = t;
            b->text_len = len;
        } else {
            char *t = embi_lay_alloc(&x->c->ld->fa, 3, 1);
            memcpy(t, "\xE2\x80\xA6", 3);
            b->text = t;
            b->text_len = 3;
        }
        *slot = b;
    }
    if ((*slot)->text_len == 0) {
        return nullptr;
    }
    return embi_lay_shape(x->c, *slot);
}

static embi_lay_text *ellipsis_text(ifc_st *x)
{
    return ellipsis_text_of(x, ellipsis_value(x), &x->ell_box);
}

/* The block overflow ellipsis of the paragraph's root (CSS Overflow 4 section 5.3): the value of block-ellipsis when it
 * shows one (the keywords ellipsis and auto, or a non-empty string), else nullptr. */
static const embi_value *block_ellipsis_value(const ifc_st *x)
{
    const embi_value *v = embi_style_value(x->rs, EMBI_PROP_block_ellipsis);
    if (v == nullptr) {
        return nullptr;
    }
    if (v->kind == EMBI_V_STRING) {
        return v->str.len > 0 ? v : nullptr;
    }
    return v->kind == EMBI_V_KEYWORD && (v->kw == EMBI_KW_ellipsis || v->kw == EMBI_KW_auto) ? v : nullptr;
}

/* The end of the grapheme cluster that starts at byte i of the UTF-8 text t (n bytes): a character with the combining
 * marks and joiners that follow it. */
static uint32_t cluster_end(const char *t, uint32_t n, uint32_t i)
{
    bool join = true;
    uint32_t j = i;
    while (j < n && join) {
        uint32_t k = j, cp;
        const uint8_t *q = (const uint8_t *)t + j;
        if (q[0] < 0x80) {
            cp = q[0];
            k = j + 1;
        } else if (q[0] < 0xE0 && j + 1 < n) {
            cp = ((uint32_t)(q[0] & 0x1F) << 6) | (q[1] & 0x3F);
            k = j + 2;
        } else if (q[0] < 0xF0 && j + 2 < n) {
            cp = ((uint32_t)(q[0] & 0x0F) << 12) | ((uint32_t)(q[1] & 0x3F) << 6) | (q[2] & 0x3F);
            k = j + 3;
        } else if (j + 3 < n) {
            cp = ((uint32_t)(q[0] & 0x07) << 18) | ((uint32_t)(q[1] & 0x3F) << 12) | ((uint32_t)(q[2] & 0x3F) << 6) | (q[3] & 0x3F);
            k = j + 4;
        } else {
            return n;
        }
        j = k;
        /* A joiner binds the next character too. */
        join = cp == 0x200D;
        /* The marks that follow stay with their base. */
        while (j < n) {
            const uint8_t *m = (const uint8_t *)t + j;
            uint32_t mc = 0, ml = 0;
            if (m[0] >= 0xF0 && j + 3 < n) {
                mc = ((uint32_t)(m[0] & 0x07) << 18) | ((uint32_t)(m[1] & 0x3F) << 12) | ((uint32_t)(m[2] & 0x3F) << 6) | (m[3] & 0x3F);
                ml = 4;
            } else if (m[0] >= 0xC0 && m[0] < 0xE0 && j + 1 < n) {
                mc = ((uint32_t)(m[0] & 0x1F) << 6) | (m[1] & 0x3F);
                ml = 2;
            } else if (m[0] >= 0xE0 && m[0] < 0xF0 && j + 2 < n) {
                mc = ((uint32_t)(m[0] & 0x0F) << 12) | ((uint32_t)(m[1] & 0x3F) << 6) | (m[2] & 0x3F);
                ml = 3;
            }
            bool mark = (mc >= 0x300 && mc <= 0x36F) || (mc >= 0x483 && mc <= 0x489) || (mc >= 0x591 && mc <= 0x5BD) ||
                        mc == 0x5BF || mc == 0x5C1 || mc == 0x5C2 || mc == 0x5C4 || mc == 0x5C5 || mc == 0x5C7 ||
                        (mc >= 0x64B && mc <= 0x65F) || (mc >= 0x1AB0 && mc <= 0x1AFF) || (mc >= 0x1DC0 && mc <= 0x1DFF) ||
                        (mc >= 0x20D0 && mc <= 0x20FF) || (mc >= 0xFE00 && mc <= 0xFE0F) || (mc >= 0xFE20 && mc <= 0xFE2F) ||
                        mc == 0x200D || (mc >= 0x1F3FB && mc <= 0x1F3FF) /* emoji modifiers */ ||
                        (mc >= 0xE0020 && mc <= 0xE007F) /* tags */ || (mc >= 0xE0100 && mc <= 0xE01EF) /* variation selectors */;
            if (!mark) {
                break;
            }
            if (mc == 0x200D) {
                join = true;
            }
            j += ml;
        }
    }
    return j;
}

/* A copy of the block overflow ellipsis box with only the first nb bytes of its text, shaped. */
static embi_box *bell_prefix(ifc_st *x, const embi_box *src, uint32_t nb)
{
    embi_box *b = (embi_box *)embi_lay_alloc(&x->c->ld->fa, sizeof(embi_box), alignof(embi_box));
    *b = *src;
    b->text_len = nb;
    b->lt = nullptr; /* the run is the prefix's own */
    b->adv = 0.0f;
    return b;
}

/* True when the width of the line clamp container b came from its content: an automatic, fit-content, min-content or
 * max-content width on a float, an absolutely positioned box, an inline-level atomic box, or a flex or grid item. An
 * approximation: a flex item in a column container, or a grid item with the default stretch alignment, takes its width
 * from the container and is counted as content-sized, and so is a flex item whose main size the container fixes; the
 * effect is that such a box keeps the content of its first line and lets the ellipsis overflow instead of displacing
 * the content. */
static bool shrink_wrapped(const embi_box *b)
{
    const embi_value *w = embi_style_value(b->style, EMBI_PROP_width);
    if (w == nullptr || w->kind != EMBI_V_KEYWORD ||
        (w->kw != EMBI_KW_auto && w->kw != EMBI_KW_fit_content && w->kw != EMBI_KW_min_content && w->kw != EMBI_KW_max_content)) {
        return false;
    }
    if ((b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_ATOMIC)) != 0) {
        return true;
    }
    return b->parent != nullptr && (b->parent->kind == EMBI_BK_FLEX || b->parent->kind == EMBI_BK_GRID);
}

/* The end of the content kept before the block ellipsis: the spaces that end it are removed (the ellipsis is added after
 * white space processing, CSS Overflow 4 section 5.3), and so is the U+1680 that ends a line. */
static uint32_t strip_end_spaces(const ifc_st *x, uint32_t s, uint32_t e)
{
    while (e > s) {
        const char *b = x->buf.data;
        if (b[e - 1] == ' ' && strips_start(x->root->style)) {
            e--;
        } else if (e >= s + 3 && (uint8_t)b[e - 3] == 0xE1 && (uint8_t)b[e - 2] == 0x9A && (uint8_t)b[e - 1] == 0x80) {
            e -= 3;
        } else {
            break;
        }
    }
    return e;
}

static uint32_t block_ellipsis_end_raw(ifc_st *x, uint32_t s, uint32_t full_e, float avail, float ell_w);

static uint32_t block_ellipsis_end(ifc_st *x, uint32_t s, uint32_t full_e, float avail, float ell_w)
{
    uint32_t e = block_ellipsis_end_raw(x, s, full_e, avail, ell_w);
    return e < full_e ? strip_end_spaces(x, s, e) : e;
}

/* The end of a clamped line that takes the block overflow ellipsis: the last soft wrap opportunity in (s, full_e] after
 * which the content and the ellipsis fit in avail (CSS Overflow 4 section 5.3: the content is displaced as if the line
 * wrapped); without one that fits, the whole content of the line is displaced. */
static uint32_t block_ellipsis_end_raw(ifc_st *x, uint32_t s, uint32_t full_e, float avail, float ell_w)
{
    uint32_t best = s;
    if (full_e > s && content_w(x, s, full_e, false) + ell_w <= avail + 0.0005f) {
        return full_e; /* the whole line fits with the ellipsis */
    }
    for (uint32_t i = 0; i < x->cand.len; i++) {
        const ifc_cand *cd = &x->cand.data[i];
        if (cd->pos <= s) {
            continue;
        }
        if (cd->pos > full_e) {
            break;
        }
        if ((cd->flags & (IFC_BK_LINE | IFC_BK_HARD)) == 0) {
            continue;
        }
        if (content_w(x, s, cd->pos, false) + ell_w <= avail + 0.0005f) {
            best = cd->pos;
        }
    }
    if (best == s && full_e > s && shrink_wrapped(x->root) && content_w(x, s, full_e, false) <= avail + 0.0005f) {
        /* None fits with the ellipsis. A container whose width came from its content (shrink-to-fit) is as wide as its
         * widest line, and the ellipsis does not count in that width (CSS Overflow 4 section 5.3, WPT
         * line-clamp-intrinsic-size-001): displacing the content would leave the ellipsis alone on a line whose content
         * fits, so the content stays and the ellipsis overflows (scrollable overflow). A container with a definite
         * width displaces the content as the specification says. */
        return full_e;
    }
    return best; /* none fits: the whole content of the line is displaced by the ellipsis */
}

/* text-overflow: ellipsis (or a string) on a block that clips its overflow. */
static bool ellipsis_on(const ifc_st *x)
{
    return (overflow_box(x)->flags & EMBI_BF_CLIP) != 0 && ellipsis_value(x) != nullptr;
}

/* Whether an atomic inline (an item, not a U+FFFC that the text itself holds) starts at paragraph position pos. */
static bool atom_starts_at(const ifc_st *x, uint32_t pos)
{
    uint32_t lo = 0, hi = x->it.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (x->it.data[mid].p < pos) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (; lo < x->it.len && x->it.data[lo].p == pos; lo++) {
        const ifc_item *t = &x->it.data[lo];
        if (t->kind == IK_ATOM && t->n == 3 && !t->outside) {
            return true;
        }
    }
    return false;
}

/* The last grapheme-like position (any break position) from s whose content and the ellipsis fit
 * in avail. */
static uint32_t ellipsis_end(ifc_st *x, uint32_t s, uint32_t full_e, float avail, float ell_w)
{
    uint32_t best = s;
    if (x->para != nullptr) {
        uint32_t nb = embi_tshim_break_count(x->para);
        /* The break table is sorted: search the first position after s, stop after full_e. */
        uint32_t lo = 0, hi = nb;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (embi_tshim_break_offset(x->para, mid) <= s) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        for (uint32_t i = lo; i < nb; i++) {
            uint32_t off = embi_tshim_break_offset(x->para, i);
            if (off > full_e) {
                break;
            }
            if (content_w(x, s, off, false) + ell_w <= avail + 0.0005f) {
                best = off;
            }
        }
    }
    if (full_e > s && content_w(x, s, full_e, false) + ell_w <= avail + 0.0005f) {
        best = full_e;
    }
    if (best == s && s + 3 <= full_e && atom_starts_at(x, s)) {
        /* An atomic inline that starts the line is kept whole, clipped by the box, not ellipsised away: it cannot be
         * cut, and the line must show something of its first item (CSS UI 4 text-overflow; text-overflow-009). */
        best = s + 3;
    } else if (best == s && x->para != nullptr && ell_w > avail) {
        /* An ellipsis wider than the line keeps the first character (CSS Overflow 4 section 3.1) and is clipped. */
        uint32_t nb = embi_tshim_break_count(x->para);
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t off = embi_tshim_break_offset(x->para, i);
            if (off > s) {
                best = off <= full_e ? off : best;
                break;
            }
        }
    }
    return best;
}

static float first_indent(ifc_st *x, const embi_box *b);

/* The number of lines a width gives and whether any overflows (text-wrap: balance). */
static bool count_lines(ifc_st *x, float width, uint32_t limit, uint32_t *count)
{
    uint32_t len = limit < plen(x) ? limit : plen(x);
    uint32_t s0 = 0, n = 0;
    float ind = first_indent(x, x->root);
    for (;;) {
        uint32_t s = skip_spaces(x, s0);
        /* The first line loses the text-indent (lay_line does the same). */
        float avail = n == 0 ? width - ind : width;
        ifc_end ce = choose_end(x, s, avail < 0 ? 0.0f : avail);
        if (ce.overflow && !ce.forced) {
            return false;
        }
        n++;
        if (ce.e >= len || n > 64) {
            break;
        }
        if (ce.e <= s) {
            return false;
        }
        s0 = ce.e;
    }
    *count = n;
    return true;
}

/* The narrowest width that keeps the greedy line count (text-wrap: balance, up to six lines). */
static float balanced_width(ifc_st *x, float full, uint32_t keep)
{
    uint32_t n = 0;
    uint32_t limit = UINT32_MAX;
    if (keep > 0) {
        /* A line clamp that cuts this paragraph is applied first: only the text of the lines it keeps is balanced. */
        uint32_t s0 = 0;
        uint32_t len = plen(x);
        float ind = first_indent(x, x->root);
        for (uint32_t i = 0; i < keep && s0 < len; i++) {
            uint32_t st = skip_spaces(x, s0);
            ifc_end ce = choose_end(x, st, i == 0 ? (full - ind < 0 ? 0.0f : full - ind) : full);
            if ((ce.overflow && !ce.forced) || ce.e <= st) {
                return full;
            }
            s0 = ce.e;
        }
        if (s0 >= len) {
            keep = 0; /* the paragraph fits in the kept lines: the ordinary balance */
        } else {
            limit = s0;
        }
    }
    if (!count_lines(x, full, limit, &n) || n < 2 || n > 6) {
        return full;
    }
    float lo = 0, hi = full;
    for (int it = 0; it < 24; it++) {
        float mid = (lo + hi) / 2;
        uint32_t m = 0;
        if (count_lines(x, mid, limit, &m) && m <= n) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

/* Whether the first line of the inline content of b is the first formatted line of its element (CSS Text 3 section 8.1):
 * an anonymous block after an in-flow sibling (the text between two block children) does not start the element. */
static bool indent_applies(const embi_box *b)
{
    if ((b->flags & EMBI_BF_ANON) == 0 || b->parent == nullptr || b->parent->kind != EMBI_BK_BLOCK) {
        return true;
    }
    for (const embi_box *k = b->parent->first; k != nullptr && k != b; k = k->next) {
        if ((k->flags & (EMBI_BF_ABSPOS | EMBI_BF_FLOAT)) == 0) {
            return false;
        }
    }
    return true;
}

/* The text-indent of box b (CSS Text 3 section 8.1): its length (cw is the percentage basis) and its keywords. */
static float indent_value(const embi_lctx *c, const embi_box *b, float cw, bool *hanging, bool *each)
{
    *hanging = false;
    *each = false;
    const embi_value *ti = embi_lay_prop(c, b, EMBI_PROP_text_indent);
    if (ti != nullptr && ti->kind == EMBI_V_LIST) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(ti, &n);
        for (uint32_t i = 1; items != nullptr && i < n; i++) {
            *hanging = *hanging || (items[i].kind == EMBI_V_KEYWORD && items[i].kw == EMBI_KW_hanging);
            *each = *each || (items[i].kind == EMBI_V_KEYWORD && items[i].kw == EMBI_KW_each_line);
        }
        ti = items != nullptr && n > 0 ? &items[0] : nullptr;
    }
    float v = ti != nullptr ? embi_lay_len(c, b, ti, cw) : 0.0f;
    return isnan(v) ? 0.0f : v;
}

/* The text-indent of the container for a line: the first line (or, with each-line, a line after a forced break) is
 * indented; hanging inverts which lines are. */
static float line_indent(ifc_st *x, const embi_box *b, bool first, bool after_forced)
{
    if (x->ti_box != b) {
        /* read once per paragraph (the container's width does not change while its lines are laid out) */
        x->ti_v = indent_value(x->c, b, x->cw, &x->ti_hang, &x->ti_each);
        x->ti_apply = indent_applies(b);
        x->ti_box = b;
    }
    float v = x->ti_v;
    bool hanging = x->ti_hang, each = x->ti_each;
    if (!x->ti_apply && !each) {
        return hanging && !first ? v : 0.0f;
    }
    bool on = first || (each && after_forced);
    return on != hanging ? v : 0.0f;
}

/* The text-indent of the container, for the first line. */
static float first_indent(ifc_st *x, const embi_box *b)
{
    return line_indent(x, b, true, false);
}

/* Moves the line's open boxes into the open stack for the next line: every OPEN and CLOSE item of
 * [j0, jn), the clipped items of an ellipsised line included (the next line starts after them). */
static void update_open(ifc_st *x)
{
    for (uint32_t i = x->ln.j0; i < x->ln.jn; i++) {
        const ifc_item *t = &x->it.data[i];
        if (t->kind == IK_OPEN) {
            embi_vec_push(&x->open, (int32_t)i);
        } else if (t->kind == IK_CLOSE && x->open.len > 0) {
            x->open.len--;
        }
    }
}

/* The end record of the line from s to the candidate at position e (the ends of a pretty layout). */
static ifc_end end_at(const ifc_st *x, uint32_t s, uint32_t e, float avail)
{
    ifc_end r = {e, false, false, 0.0f, nullptr};
    uint32_t n = x->cand.len, lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (x->cand.data[mid].pos < e) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < n && x->cand.data[lo].pos == e) {
        const ifc_cand *c = &x->cand.data[lo];
        r.forced = (c->flags & IFC_BK_HARD) != 0;
        r.hw = c->hw;
        r.hs = c->hs;
        r.overflow = cand_w(x, s, c, false) > avail + 0.0005f;
    } else {
        r.overflow = content_w(x, s, e, false) > avail + 0.0005f;
    }
    return r;
}

/* The justification opportunities of the line [s, e): the spaces when it has any, else every
 * character of ideographic text (text-justify: auto), else none. */
static uint8_t justify_mode(const ifc_st *x, uint32_t s, uint32_t e)
{
    bool ideo = false;
    for (uint32_t i = s; i < e; i++) {
        uint8_t c = (uint8_t)x->buf.data[i];
        if (c == ' ') {
            return JS_SPACES;
        }
        if (ideographic(x->buf.data + i, e - i)) {
            ideo = true; /* CJK ideographs and kana */
        }
    }
    return ideo ? JS_CHARS : JS_NONE;
}

/* A hyphen unit's box: the synthetic run "-" in style s (made once per line that ends at a soft
 * hyphen). */
static embi_lay_text *hyphen_text(ifc_st *x, const embi_style *s, embi_box **out)
{
    embi_box *b = (embi_box *)embi_lay_alloc(&x->c->ld->fa, sizeof(embi_box), alignof(embi_box));
    memset(b, 0, sizeof *b);
    b->kind = EMBI_BK_TEXT;
    b->flags = EMBI_BF_INLINE_LEVEL;
    b->style = s;
    char *t = embi_lay_alloc(&x->c->ld->fa, 1, 1);
    t[0] = '-';
    b->text = t;
    b->text_len = 1;
    *out = b;
    return embi_lay_shape(x->c, b);
}

/* Places the floats that were left behind by the previous line, at the top (Y, relative to the content box) of the
 * next one. */
static void place_deferred(ifc_st *x, const embi_ifc_frame *fr, float Y)
{
    for (uint32_t i = 0; i < x->defer.len; i++) {
        const ifc_defer *d = &x->defer.data[i];
        float bx = 0, by = 0;
        embi_lay_float_place(x->c, d->box, d->frag, &d->g, fr->ex, fr->cx, x->cw, fr->cy + Y, &bx, &by);
        embi_frag *cf = d->frag;
        cf->x = fr->ox + (bx - fr->cx);
        cf->y = fr->oy + (by - fr->cy);
        embi_lay_frag_append(x->root_frag_bf, cf);
        embi_lay_ov_add_child(x->root_frag_bf, cf);
    }
    x->defer.len = 0;
}

/* Lays out the line from s (its start in the paragraph) with the first item j0, in the band
 * [lb, rb], and sets its units and metrics. Floats met in the line are placed at its top, and the
 * line is laid out again when they change its band or its height. Y is the line's top on entry and
 * on return: it moves down when the content fits nowhere beside the floats. */
/* True for a box with a position other than static. */
static bool positioned_box(const embi_box *b)
{
    const embi_value *v = b != nullptr && b->style != nullptr ? embi_style_value(b->style, EMBI_PROP_position) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw != EMBI_KW_static;
}

/* The items of a line that ends at a soft break at pos, from j1 = split_of(pos): the opening edges at pos of positioned
 * boxes without margin, border or padding stay at the end of the line, as Chromium keeps them (such a box is not culled
 * there: it has an empty fragment, which its containing block for abspos starts from, position-absolute-in-inline-003
 * and 004; a culled box has no fragment there, so the others still open on the next line). */
static uint32_t keep_open_tags(const ifc_st *x, uint32_t pos, uint32_t j1)
{
    while (j1 < x->it.len && x->it.data[j1].p == pos && x->it.data[j1].kind == IK_OPEN && x->it.data[j1].n == 0 &&
           x->it.data[j1].w == 0.0f && positioned_box(x->it.data[j1].box)) {
        j1++;
    }
    return j1;
}

static void lay_line(ifc_st *x, uint32_t s, uint32_t j0, bool first, float *Yp,
                     const embi_ifc_frame *fr, float *lb_out, float *rb_out, float *h_out,
                     ifc_end *ce_out)
{
    ifc_line *ln = &x->ln;
    uint32_t len = plen(x);
    float Y = *Yp; /* the line's top; a line that fits nowhere beside the floats moves down */
    float h_est = x->a0 + x->d0;
    float lb = 0, rb = x->cw;
    float ind = line_indent(x, x->root, first, x->after_forced);
    ifc_end ce = {len, false, false, 0.0f, nullptr};
    uint32_t guard = 0;
    bool band0 = true;
    place_deferred(x, fr, Y);
    for (;;) {
        float bfc_y = fr->cy + Y;
        float bl_bfc = 0, br_bfc = 0;
        if (fr->ex != nullptr) {
            embi_excl_band(fr->ex, fr->cx, fr->cx + x->cw, bfc_y, h_est, &bl_bfc, &br_bfc);
        } else {
            bl_bfc = fr->cx;
            br_bfc = fr->cx + x->cw;
        }
        lb = bl_bfc - fr->cx;
        rb = br_bfc - fr->cx;
        if (lb < 0) {
            lb = 0;
        }
        if (rb > x->cw) {
            rb = x->cw;
        }
        if (band0) {
            ln->lb0 = lb;
            ln->rb0 = rb;
            band0 = false;
        }
        float avail = rb - lb - ind;
        float avail_full = avail;
        if (avail > x->balance_w - ind) {
            avail = x->balance_w - ind;
        }
        if (avail < 0) {
            avail = 0;
        }
        if (x->use_ends && x->li < x->ends.len && x->ends.data[x->li] > s) {
            ce = end_at(x, s, x->ends.data[x->li], avail);
        } else {
            ce = choose_end(x, s, avail);
        }
        /* text-overflow: the line shows its content up to the ellipsis; the rest of the line (up to
         * ce.e, where the next line starts) is clipped. */
        uint32_t e = ce.e;
        bool tov = false; /* text-overflow ellipsises this line */
        if (ce.overflow && ellipsis_on(x)) {
            /* A line that ends at a preserved line feed (white-space: pre) is ellipsised too: the line feed is not content. */
            uint32_t fe = ce.e;
            if (ce.forced && fe > s && x->buf.data[fe - 1] == '\n') {
                fe--;
            }
            embi_lay_text *lt = ellipsis_text(x);
            float ell_w = lt != nullptr ? embi_tshim_advance(lt) : 0.0f;
            e = ellipsis_end(x, s, fe, avail, ell_w);
            tov = true;
            ce.hw = 0.0f;
            ce.hs = nullptr;
        }
        /* The last line a line clamp keeps shows the block overflow ellipsis at its end, after the content that still fits. */
        bool bell = false;
        if (x->clamp_last && ce.e > s) {
            const embi_value *bv = block_ellipsis_value(x);
            embi_lay_text *blt = bv != nullptr ? ellipsis_text_of(x, bv, &x->bell_box) : nullptr;
            if (blt != nullptr) {
                float bw = embi_tshim_advance(blt);
                uint32_t full_e = ce.e;
                if (ce.forced && full_e > s && x->buf.data[full_e - 1] == '\n') {
                    full_e--;
                }
                e = block_ellipsis_end(x, s, full_e, avail_full, bw);
                ce.hw = 0.0f;
                ce.hs = nullptr;
                ce.forced = false;
                bell = true;
            }
        }
        /* text-overflow applies to the line that holds the block ellipsis too, after the clamp (CSS Overflow 4 section
         * 5.3): when the content and the block ellipsis still overflow, the end of that text is hidden for the text-overflow
         * ellipsis. A first character that does not fit stays and is clipped. */
        bool tell = false;
        embi_box *bpre = nullptr; /* the block ellipsis cut to what fits, or nullptr */
        if (bell && ellipsis_on(x)) {
            embi_lay_text *blt = ellipsis_text_of(x, block_ellipsis_value(x), &x->bell_box);
            embi_lay_text *tlt = ellipsis_text(x);
            float bw = embi_tshim_advance(blt);
            float tw = tlt != nullptr ? embi_tshim_advance(tlt) : 0.0f;
            float cw0 = e > s ? content_w(x, s, e, false) : 0.0f;
            if (cw0 + bw > avail_full + 0.0005f) {
                tell = true;
                if (e > s && cw0 + tw > avail_full + 0.0005f) {
                    e = ellipsis_end(x, s, e, avail_full, tw);
                    bell = false;
                } else {
                    /* The longest prefix of whole grapheme clusters that fits with the ellipsis: a binary search over
                     * the cluster ends. A cluster is at least one byte, so a table of bn entries holds any string. */
                    const char *bt = x->bell_box->text;
                    uint32_t bn = x->bell_box->text_len;
                    uint32_t *ends = (uint32_t *)embi_lay_alloc(&x->c->ld->fa, ((size_t)bn + 1) * sizeof(uint32_t), alignof(uint32_t));
                    uint32_t ne = 0;
                    for (uint32_t k = 0; ne < bn && k < bn;) {
                        uint32_t nk = cluster_end(bt, bn, k);
                        if (nk <= k || nk > bn) {
                            break;
                        }
                        ends[ne++] = k = nk;
                    }
                    uint32_t lo = 0, hi = ne; /* lo: clusters known to fit, hi: the most that might */
                    embi_box *best_pb = nullptr;
                    while (lo < hi) {
                        uint32_t mid = lo + (hi - lo + 1) / 2;
                        embi_box *pb = bell_prefix(x, x->bell_box, ends[mid - 1]);
                        embi_lay_text *plt = embi_lay_shape(x->c, pb);
                        float pw = plt != nullptr ? embi_tshim_advance(plt) : 0.0f;
                        if (cw0 + pw + tw <= avail_full + 0.0005f) {
                            lo = mid;
                            best_pb = pb;
                        } else {
                            hi = mid - 1;
                        }
                    }
                    bpre = best_pb;
                    /* Nothing of the line before the ellipsis fits: the ellipsis stands alone, unless it is wider than
                     * the line, which keeps the first character (CSS Overflow 4 section 3.1). */
                    if (bpre == nullptr && ne > 0 && e == s && tw > avail_full + 0.0005f) {
                        bpre = bell_prefix(x, x->bell_box, ends[0]);
                    }
                    if (bpre == nullptr) {
                        bell = false;
                    }
                }
            }
        }
        /* A forced break at the end of the paragraph leaves the items after it (floats) to the next line. */
        uint32_t j1 = e >= len && !ce.forced ? x->it.len : split_of(x, e);
        if (e < len && e == ce.e && !ce.forced) {
            j1 = keep_open_tags(x, e, j1);
        }
        /* An ellipsised line ends before ce.e, where the next line starts: the items between are
         * clipped, but their boxes still open or close and their floats are still placed. */
        uint32_t jn = j1;
        if (ce.e >= len && !ce.forced) {
            jn = x->it.len;
        } else if (e < ce.e) {
            uint32_t js = start_of(x, ce.e);
            jn = js > j1 ? js : j1;
        }

        /* The floats of the line are placed at its top (CSS 2.1 9.5.1, rule 6), when they fit beside what is on the
         * line before them; one that does not fit, or that has to clear a float below the line's top, goes to the
         * next line, and so do the floats after it (their order is kept). */
        uint32_t newf = 0;
        bool deferred = false;
        for (uint32_t i = j0; i < jn; i++) {
            ifc_item *t = &x->it.data[i];
            if (t->kind != IK_FLOAT || t->placed) {
                continue;
            }
            t->placed = true;
            embi_float_geom g;
            embi_frag *cf = embi_lay_float_prepare(x->c, t->box, fr->cx, x->cw, fr->cb_h, &g);
            if (cf == nullptr) {
                continue;
            }
            if (!deferred && fr->ex != nullptr && t->p > s) {
                float before = prefix_w(x, t->p, true) - prefix_w(x, s, true);
                if (before > 0.0005f) {
                    /* A line whose content overflows (an unbreakable run, white-space: nowrap) is full: a float in it
                     * does not fit beside what is on the line, the text after it included. */
                    float whole = prefix_w(x, e, true) - prefix_w(x, s, true);
                    if (whole > avail + 0.0005f) {
                        before = whole;
                    }
                    uint8_t clr = embi_lay_clear_sides(x->c, t->box);
                    deferred = before + g.mw > avail + 0.0005f ||
                               (clr != 0 && embi_excl_clear_y(fr->ex, clr) > bfc_y + 0.0005f);
                }
            }
            if (deferred) {
                ifc_defer d = {t->box, cf, g};
                embi_vec_push(&x->defer, d);
                continue;
            }
            float bx = 0, by = 0;
            embi_lay_float_place(x->c, t->box, cf, &g, fr->ex, fr->cx, x->cw, bfc_y, &bx, &by);
            cf->x = fr->ox + (bx - fr->cx);
            cf->y = fr->oy + (by - fr->cy);
            embi_lay_frag_append(x->root_frag_bf, cf);
            embi_lay_ov_add_child(x->root_frag_bf, cf);
            newf++;
        }
        if (newf > 0 && guard++ < 8) {
            continue;
        }

        ln->s = s;
        ln->e = e;
        ln->j0 = j0;
        ln->j1 = j1;
        ln->jn = jn;
        ln->forced = ce.forced;
        ln->last = e >= len && !bell;
        ln->bell_only = bell && e == s;
        ln->lb = lb;
        ln->rb = rb;
        ln->indent = ind;
        ln->u.len = 0;
        ln->recs.len = 0;
        resolve_align(x, ce.forced || (e >= len && !bell));

        uint32_t hs = e;
        hang_of(x, s, e, j0, j1, &hs, false); /* the separators that hang are still drawn: only the spaces are dropped */
        line_hang(x, s, e, j0, j1, &hs, ce.forced || e >= len);
        ln->tab_hangs = false;
        for (uint32_t i = hs; i < e && x->has_tab; i++) {
            ln->tab_hangs = ln->tab_hangs || x->buf.data[i] == 9;
        }
        uint8_t jm = ln->justify ? justify_mode(x, s, hs) : JS_NONE;
        build_units(x, s, hs, j0, j1, jm);
        bool lf_only = true;
        for (uint32_t i = hs; i < e && lf_only; i++) {
            lf_only = x->buf.data[i] == '\n';
        }
        if (lf_only) {
            /* The last character of the line gets no letter-spacing (hang_of makes the same cut for the fit). */
            for (uint32_t i = ln->u.len; i > 0; i--) {
                ifc_unit *lu = &ln->u.data[i - 1];
                if (lu->kind != UK_TEXT) {
                    if (lu->kind == UK_OPEN || lu->kind == UK_CLOSE) {
                        continue;
                    }
                    break;
                }
                if ((lu->level & 1u) == 0 && lu->box != nullptr && lu->b1 > lu->b0) {
                    lu->w -= ls_end(lu->box, lu->b1);
                }
                break;
            }
        }
        if (jm == JS_CHARS) {
            /* The last character of the line is not followed by a gap. */
            for (uint32_t i = ln->u.len; i > 0; i--) {
                if (ln->u.data[i - 1].space) {
                    ln->u.data[i - 1].space = false;
                    break;
                }
            }
        }
        trim_line_units(x, avail);
        if (ce.hw > 0 && ce.hs != nullptr) {
            /* A line that ends at a soft hyphen shows the hyphen (CSS Text 3 section 5.3). */
            embi_box *hb = nullptr;
            embi_lay_text *lt = hyphen_text(x, ce.hs, &hb);
            if (lt != nullptr) {
                ifc_unit u;
                memset(&u, 0, sizeof u);
                u.kind = UK_TEXT;
                u.level = (uint8_t)level_at(x, e >= 2 ? e - 2 : 0);
                u.item = -1;
                u.box = hb;
                u.lt = lt;
                u.b0 = 0;
                u.b1 = 1;
                u.w = ce.hw;
                const ifc_met *hm = metrics(x, ce.hs);
                u.a = hm->a;
                u.d = hm->d;
                u.ca = hm->ca;
                u.cd = hm->cd;
                push_unit(x, &u);
            }
        }
        if (bell) {
            embi_lay_text *lt = bpre != nullptr ? embi_lay_shape(x->c, bpre) : ellipsis_text_of(x, block_ellipsis_value(x), &x->bell_box);
            if (lt != nullptr) {
                ifc_unit u;
                memset(&u, 0, sizeof u);
                u.kind = UK_TEXT;
                u.level = (uint8_t)x->base;
                u.item = -1;
                u.box = bpre != nullptr ? bpre : x->bell_box;
                u.lt = lt;
                u.b0 = 0;
                u.b1 = bpre != nullptr ? bpre->text_len : x->bell_box->text_len;
                u.w = embi_tshim_advance(lt);
                const ifc_met *em = metrics(x, x->rs);
                u.ca = em->ca;
                u.cd = em->cd;
                /* The ellipsis sits in an anonymous inline with line-height: 0: it does not grow the line box. */
                u.a = 0;
                u.d = 0;
                u.bare = true;
                push_unit(x, &u);
            }
        }
        if (((tov && !bell) || tell) && ellipsis_on(x)) {
            embi_lay_text *lt = ellipsis_text(x);
            if (lt != nullptr) {
                ifc_unit u;
                memset(&u, 0, sizeof u);
                u.kind = UK_TEXT;
                u.level = (uint8_t)x->base;
                u.item = -1;
                u.box = x->ell_box;
                u.lt = lt;
                u.b0 = 0;
                u.b1 = x->ell_box->text_len;
                u.w = embi_tshim_advance(lt);
                const ifc_met *em = metrics(x, x->rs);
                u.a = em->a;
                u.d = em->d;
                u.ca = em->ca;
                u.cd = em->cd;
                push_unit(x, &u);
            }
        }
        if (x->has_tab) {
            resolve_tabs(x);
        }
        justify_units(x, avail);
        make_records(x);
        place_vertical(x);
        position_units(x);
        measure_pieces(x);

        bool any = ce.forced;
        for (uint32_t i = 0; i < ln->u.len && !any; i++) {
            const ifc_unit *u = &ln->u.data[i];
            if (u->kind == UK_TEXT || u->kind == UK_ATOM) {
                any = true;
            } else if ((u->kind == UK_OPEN || u->kind == UK_CLOSE) && u->w > 0) {
                any = true;
            }
        }
        if (first) {
            for (uint32_t i = j0; i < j1 && !any; i++) {
                if (x->it.data[i].outside) {
                    any = true;
                }
            }
        }
        /* Preserved white space is content even when it hangs and takes no room: a line of only such spaces is not
         * empty (CSS Text 3 section 4.1.3, white-space: pre-wrap and break-spaces). */
        for (uint32_t i = j0; i < j1 && !any; i++) {
            const ifc_item *t = &x->it.data[i];
            if (t->kind == IK_TEXT && t->n > 0 && t->p < e && t->p + t->n > s) {
                uint32_t lo = t->p > s ? t->p : s;
                uint32_t hi = t->p + t->n < e ? t->p + t->n : e;
                bool keep = !strips_start(t->box->style);
                for (uint32_t k = t->b0 + (lo - t->p); k < t->b0 + (hi - t->p); k++) {
                    /* the other space separators (U+3000 ...) are never collapsed: content in every mode */
                    if ((keep && t->box->text[k] != '\n') ||
                        (k + 3 <= t->b0 + (hi - t->p) && space_sep3((const uint8_t *)t->box->text + k))) {
                        any = true;
                        break;
                    }
                }
            }
        }
        ln->any = any;

        float H = ln->H;
        if (H > h_est + 0.0005f && guard++ < 8) {
            h_est = H;
            continue;
        }
        if (ce.overflow && !ce.forced && x->any_float && rb - lb < x->cw - 0.5f && guard++ < 8) {
            float nxt = embi_excl_next_y(fr->ex, bfc_y, h_est);
            if (isfinite(nxt)) {
                Y = nxt - fr->cy;
                continue;
            }
        }
        break;
    }
    *Yp = Y;
    *lb_out = lb;
    *rb_out = rb;
    *h_out = ln->H;
    *ce_out = ce;
}

/* True when an out-of-flow box was block-level before it was blockified (its computed display keyword has no
 * EMBI_DISP_FROM_INLINE mark). */
static bool abs_block_level(const embi_box *b)
{
    if (b == nullptr || b->style == nullptr) {
        return false;
    }
    const embi_value *dv = embi_style_value(b->style, EMBI_PROP_display);
    return dv != nullptr && dv->kind == EMBI_V_KEYWORD && dv->aux != EMBI_DISP_FROM_INLINE;
}

/* The visual x (in the content box) of paragraph position pos on the line just laid out, when the text there runs
 * against the paragraph's direction (its bidi level has the other parity): the placeholder of an out-of-flow box then
 * takes that text's direction, and its static position is the box's end edge (Chromium picks the static position's
 * inline edge by the placeholder's direction). The text unit before pos decides, else the one after it. */
static bool abs_flipped(const ifc_st *x, uint32_t pos, float *vx)
{
    const ifc_unit *hit = nullptr;
    uint32_t h0 = 0;
    for (int pass = 0; pass < 2 && hit == nullptr; pass++) {
        for (uint32_t i = 0; i < x->ln.u.len; i++) {
            const ifc_unit *u = &x->ln.u.data[i];
            if (u->kind != UK_TEXT || u->item < 0) {
                continue;
            }
            const ifc_item *t = &x->it.data[u->item];
            uint32_t p0 = t->p + (u->b0 - t->b0), p1 = t->p + (u->b1 - t->b0);
            if (pass == 0 ? (p0 < pos && pos <= p1) : (p0 <= pos && pos < p1)) {
                hit = u;
                h0 = p0;
                break;
            }
        }
    }
    if (hit == nullptr || (uint32_t)(hit->level & 1) == x->base) {
        return false;
    }
    float pre = prefix_w(x, pos, false) - prefix_w(x, h0, false);
    pre = fminf(fmaxf(pre, 0.0f), hit->w);
    *vx = (hit->level & 1) != 0 ? hit->x + hit->w - pre : hit->x + pre;
    return true;
}

/* The static positions of the out-of-flow children that fall in the line just laid out (its paragraph
 * range [s, e], the first line that has them): the advance from the line's content start to their
 * paragraph position, from the start edge (the right edge in rtl), at the line's top (top is relative
 * to the content box). They are recorded for the containing block (W3d, position.c). */
static void abs_static(ifc_st *x, const embi_ifc_frame *fr, float top, float h)
{
    for (uint32_t i = 0; i < x->abs.len; i++) {
        ifc_abs *a = &x->abs.data[i];
        if (a->done || a->p < x->ln.s || a->p > x->ln.e) {
            continue;
        }
        a->done = true;
        float adv = prefix_w(x, a->p, false) - prefix_w(x, x->ln.s, false);
        if (abs_block_level(a->box)) {
            /* A box that was block-level before blockification starts a line of its own: below the line when content
             * comes before it in the line, at the content box's start edge (CSS 2.1 section 10.3.7 and 10.6.4: the
             * hypothetical box is a block; Chromium places it so, ignoring text-indent and floats). */
            float y = top + (x->ln.any && adv > 0.0f ? h : 0.0f);
            float sx = x->base == 1 ? fr->ox + x->cw : fr->ox;
            embi_lay_abspos_at(x->c, a->box, x->root, sx, fr->oy + y, nullptr);
            continue;
        }
        float vx = 0.0f;
        if (abs_flipped(x, a->p, &vx)) {
            embi_lay_abspos_at(x->c, a->box, x->root, fr->ox + vx, fr->oy + top, nullptr);
            embi_lay_abspos_flip(x->c, a->box);
            continue;
        }
        float sx = x->base == 1 ? fr->ox + x->ln.rb - adv : fr->ox + x->ln.lb + x->ln.indent + adv;
        embi_lay_abspos_at(x->c, a->box, x->root, sx, fr->oy + top, nullptr);
    }
}

/* text-wrap: pretty (CSS Text 4 section 4.2, greedy with orphan avoidance on the last line only):
 * the greedy line ends are kept in x->ends (the last one is the paragraph end). When the last line
 * would hold a single word, the line before it breaks at the latest opportunity that still leaves
 * the last line within the width, so that the last line gets two words. Only without floats. */
static void pretty_ends(ifc_st *x, float width)
{
    uint32_t len = plen(x);
    uint32_t s0 = 0;
    x->ends.len = 0;
    for (uint32_t guard = 0; guard < 100000 && len > 0; guard++) {
        uint32_t s = skip_spaces(x, s0);
        float avail = width - (guard == 0 ? first_indent(x, x->root) : 0.0f);
        ifc_end ce = choose_end(x, s, avail < 0 ? 0 : avail);
        embi_vec_push(&x->ends, ce.e);
        if (ce.e >= len) {
            break;
        }
        if (ce.e <= s && !ce.forced) {
            x->ends.len = 0; /* no progress: the rest has no break, so nothing to rebalance */
            return;
        }
        s0 = ce.e;
    }
    uint32_t n = x->ends.len;
    if (n < 2 || x->ends.data[n - 1] != len) {
        return;
    }
    uint32_t sl = skip_spaces(x, x->ends.data[n - 2]); /* the last line's start */
    for (uint32_t i = sl; i < len; i++) {
        if (x->buf.data[i] == ' ') {
            return; /* two or more words: nothing to avoid */
        }
    }
    uint32_t sp = n >= 3 ? skip_spaces(x, x->ends.data[n - 3]) : 0; /* the previous line's start */
    float avail_p = width - (n == 2 ? first_indent(x, x->root) : 0.0f);
    /* The candidates strictly inside (sp, sl), latest first. */
    uint32_t lo = 0, hi = x->cand.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (x->cand.data[mid].pos < sl) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (uint32_t k = lo; k > 0; k--) {
        const ifc_cand *c = &x->cand.data[k - 1];
        if (c->pos <= sp) {
            break;
        }
        if ((c->flags & (IFC_BK_LINE | IFC_BK_GRAPHEME)) == 0 || (c->flags & IFC_BK_HARD) != 0) {
            continue;
        }
        if (cand_w(x, sp, c, false) > avail_p + 0.0005f) {
            continue;
        }
        uint32_t cs = skip_spaces(x, c->pos);
        if (content_w(x, cs, len, false) > width + 0.0005f) {
            continue;
        }
        x->ends.data[n - 2] = c->pos;
        return;
    }
}

/* The inline formatting context's lines. */
/* The clear sides of the <br> that ends a line at pos (CSS 2.1 9.5.2 applies clear to a line break
 * as Chromium does): the next line starts below the floats on those sides. 0 when none. */
static uint8_t br_clear_at(const ifc_st *x, uint32_t pos)
{
    for (uint32_t i = 0; i < x->it.len; i++) {
        const ifc_item *t = &x->it.data[i];
        if (t->kind == IK_BR && t->box != nullptr && t->p < pos && t->p + t->n >= pos) {
            return embi_lay_clear_sides(x->c, t->box);
        }
    }
    return 0;
}


bool embi_lay_ifc(embi_lctx *c, embi_box *b, embi_frag *bf, const embi_ifc_frame *fr, float cw,
                  float *ch, float *bl_first, float *bl_last)
{
    *ch = 0;
    *bl_first = NAN;
    *bl_last = NAN;

    ifc_st x;
    memset(&x, 0, sizeof x);
    x.c = c;
    x.root = b;
    x.rs = b->style;
    x.layout = true;
    x.cb_h = fr->cb_h;
    x.cw = cw;
    x.base = kwv(b->style, EMBI_PROP_direction) == EMBI_KW_rtl ? 1 : 0;
    x.root_frag_bf = bf;
    scr_take(&x);
    pf_prefetch(c->ld, b);

    build_children(&x, b, -1);
    analyze_all(&x, false);
    {
        const ifc_met *sm = metrics(&x, b->style);
        x.a0 = sm->a;
        x.d0 = sm->d;
        x.ca0 = sm->ca;
        x.cd0 = sm->cd;
        x.xh0 = met_xh(&x, b->style);
    }
    prepare_items(&x);

    /* text-wrap: balance, without floats, narrows the lines to the least width that keeps the count. */
    float full = cw;
    uint16_t wstyle = kwv(b->style, EMBI_PROP_text_wrap_style);
    if (wstyle == EMBI_KW_balance && !x.any_float) {
        full = balanced_width(&x, cw, c->cl.on && c->cl.left > 0 ? c->cl.left : 0);
    }
    x.balance_w = full;
    if (wstyle == EMBI_KW_pretty && !x.any_float) {
        pretty_ends(&x, cw);
        x.use_ends = x.ends.len > 0;
    }

    uint32_t len = plen(&x);
    bool any_line = false;
    float Y = 0;
    uint32_t s0 = 0;
    uint32_t j_start = 0;
    uint32_t nlines = 0, lines_laid = 0;
    const uint8_t base0 = x.base;
    bool prev_forced = false;
    const bool plain = kwv(b->style, EMBI_PROP_unicode_bidi) == EMBI_KW_plaintext && x.para != nullptr;
    bool clamped = false; /* the line clamp's last line is laid out: the rest of the paragraph is not */
    for (;;) {
        if (c->cl.on && c->cl.left == 0) {
            clamped = true;
            break;
        }
        x.clamp_last = c->cl.on && c->cl.left == 1 && c->cl.ell;
        uint32_t s = skip_spaces(&x, s0);
        bool first = nlines == 0;
        float lb = 0, rb = 0, h = 0;
        ifc_end ce = {len, false, false, 0.0f, nullptr};
        x.nlines = nlines;
        x.li = lines_laid++;
        x.after_forced = !first && prev_forced;
        if (plain) {
            /* unicode-bidi: plaintext: the line's direction is its bidi paragraph's (start and end alignment too) */
            x.base = embi_tshim_para_level(x.para, s, base0);
        }
        lay_line(&x, s, j_start, first, &Y, fr, &lb, &rb, &h, &ce);
        prev_forced = ce.forced;
        abs_static(&x, fr, Y, h);
        if (x.ln.any) {
            embi_frag *lf = new_line_frag(&x, bf, fr, Y, h);
            lf->bl_first = x.ln.base; /* the baseline of the line, relative to its top */
            lf->bl_last = x.ln.base;
            emit_line(&x, lf);
            if (nlines == 0) {
                *bl_first = Y + x.ln.base;
            }
            *bl_last = Y + x.ln.base;
            Y += h;
            any_line = true;
            nlines++;
            if (c->cl.on) {
                c->cl.left--;
            }
        }
        update_open(&x);
        if (c->cl.on && c->cl.left == 0 && x.ln.any) {
            clamped = true;
            break;
        }
        /* A <br clear> moves the next line below the floats it clears; a <br clear> that ends the content also
         * makes the block as tall as the clearance (the line after it would start there). */
        if (ce.forced && fr->ex != nullptr) {
            uint8_t clr = br_clear_at(&x, ce.e);
            if (clr != 0) {
                float cb = embi_excl_clear_y(fr->ex, clr) - fr->cy;
                if (!isinf(cb) && cb > Y) {
                    Y = cb;
                }
            }
        }
        /* The end of the paragraph ends the loop, unless a forced break leaves items (floats) behind it:
         * those start a line of their own, as a float after a <br> does. */
        if (ce.e >= len && !(ce.forced && split_of(&x, ce.e) < x.it.len)) {
            break;
        }
        if (ce.e <= s && !ce.forced) {
            break; /* no progress: the rest has no break */
        }
        s0 = ce.e;
        j_start = start_of(&x, s0);
        if (!ce.forced && x.ln.jn > split_of(&x, s0) && s0 < len) {
            j_start = x.ln.jn; /* past the opening edges the line kept (keep_open_tags) */
        }
        if (nlines > 100000) {
            break;
        }
    }

    x.base = base0;
    if (!clamped) {
        place_deferred(&x, fr, Y);
    } else {
        /* The floats left out are not laid out; their out-of-flow descendants whose containing block lies outside them
         * still are (linecl.c). */
        for (uint32_t i = 0; i < x.it.len; i++) {
            const ifc_item *t = &x.it.data[i];
            if (t->kind == IK_FLOAT && !t->placed) {
                embi_lay_clamp_skip(c, t->box, b, fr->ox, fr->oy + Y);
            }
        }
        for (uint32_t i = 0; i < x.defer.len; i++) {
            embi_lay_clamp_skip(c, x.defer.data[i].box, b, fr->ox, fr->oy + Y);
        }
    }
    embi_vec_free(&x.defer);
    *ch = Y;
    if (!any_line) {
        *bl_first = NAN;
        *bl_last = NAN;
    }
    scr_give(&x);
    return any_line;
}

/* ------------------------------------------------------------------ intrinsic sizes */

/* The start of the content of a run [s, e) that begins a line: the collapsible spaces at a line start are removed. */
static uint32_t skip_to(const ifc_st *x, uint32_t s, uint32_t e)
{
    uint32_t k = skip_spaces(x, s);
    return k < e ? k : s;
}

void embi_lay_ifc_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    *mn = 0;
    *mx = 0;
    ifc_st x;
    memset(&x, 0, sizeof x);
    x.c = c;
    x.root = b;
    x.rs = b->style;
    x.layout = false;
    x.cw = 0;
    x.base = kwv(b->style, EMBI_PROP_direction) == EMBI_KW_rtl ? 1 : 0;
    scr_take(&x);
    pf_prefetch(c->ld, b);
    build_children(&x, b, -1);
    analyze_all(&x, true);

    uint32_t len = plen(&x);
    float indent = 0, indent_mx = 0, indent_mn = 0; /* the first line's, and the other lines' (max- and min-content) */
    {
        bool hanging = false, each = false;
        float v = indent_value(c, b, 0.0f, &hanging, &each); /* a percentage counts 0 in an intrinsic size */
        bool ap = indent_applies(b) || each;
        indent = ap && !hanging ? v : 0.0f;
        indent_mx = (each && ap) != hanging ? v : 0.0f; /* the later max-content lines follow forced breaks */
        indent_mn = hanging ? v : 0.0f;
    }

    /* The maximum is the widest run between forced breaks; the minimum the widest run between soft
     * breaks (line and grapheme opportunities, and the forced ones). They depend on the analysis alone, so
     * a cached paragraph keeps them (W3q). */
    float best_mx = 0, best_mn = 0, first_mx = 0, first_mn = 0;
    uint32_t seg = 0, prev = 0;
    float fmn = 0; /* the widest float's minimum */
    if (x.pc != nullptr && x.pc->have_int) {
        best_mn = x.pc->i_mn;
        best_mx = x.pc->i_mx;
        first_mn = x.pc->i_mn0;
        first_mx = x.pc->i_mx0;
        goto have;
    }
    for (uint32_t k = 0; k < x.cand.len; k++) {
        const ifc_cand *cd = &x.cand.data[k];
        uint32_t p = cd->pos;
        if (p > seg && (cd->flags & IFC_BK_HARD)) {
            float w = content_w(&x, skip_to(&x, seg, p), p, true);
            if (seg == 0) {
                first_mx = w;
            } else if (w > best_mx) {
                best_mx = w;
            }
            seg = p;
        }
        if (p > prev && (cd->flags & (IFC_BK_LINE | IFC_BK_GRAPHEME | IFC_BK_MINC | IFC_BK_HARD))) {
            float w = content_w(&x, skip_to(&x, prev, p), p, false);
            if (prev == 0) {
                first_mn = w;
            } else if (w > best_mn) {
                best_mn = w;
            }
            prev = p;
        }
    }
    if (len > seg) {
        float w = content_w(&x, skip_to(&x, seg, len), len, true);
        if (seg == 0) {
            first_mx = w;
        } else if (w > best_mx) {
            best_mx = w;
        }
    }
    if (len > prev) {
        float w = content_w(&x, skip_to(&x, prev, len), len, false);
        if (prev == 0) {
            first_mn = w;
        } else if (w > best_mn) {
            best_mn = w;
        }
    }
    bool has_content = false; /* any text or atomic inline with a width */
    for (uint32_t i = 0; i < x.it.len; i++) {
        if (x.it.data[i].kind == IK_FLOAT && x.it.data[i].wmin > fmn) {
            fmn = x.it.data[i].wmin;
        }
        if ((x.it.data[i].kind == IK_TEXT || x.it.data[i].kind == IK_ATOM) && x.it.data[i].wmax > 0) {
            has_content = true;
        }
    }
    if (!has_content && x.it.len > 0) {
        /* Only floats (and collapsed white space): they follow each other on one line when the width
         * is unbounded, so their maximums add up. */
        float total = x.cmx.data[x.it.len];
        if (total > best_mx) {
            best_mx = total;
        }
        if (total > first_mx) {
            first_mx = total;
        }
    }
    if (x.pc != nullptr) {
        x.pc->have_int = true;
        x.pc->i_mn = best_mn;
        x.pc->i_mx = best_mx;
        x.pc->i_mn0 = first_mn;
        x.pc->i_mx0 = first_mx;
    }
have:
    /* Negative inline margins can make a run narrower than nothing: the sizes are clamped at zero,
     * and the maximum is never below the minimum (CSS Sizing 3 section 5.1). */
    /* Only the first line has the text-indent, so only the first run (minimum) and segment (maximum) widen. */
    *mn = clamp0(fmaxf(best_mn + indent_mn, first_mn + indent));
    if (fmn > *mn) {
        *mn = fmn;
    }
    *mx = clamp0(fmaxf(best_mx + indent_mx, first_mx + indent));
    if (*mx < *mn) {
        *mx = *mn;
    }

    scr_give(&x);
}
