#pragma once
/*
 * Internal declarations of the layout module (W3a). Every file in src/layout includes this header.
 * Nothing here is used outside the module: the public surface is layout.h.
 *
 * Conventions. A constraint space (embi_cs) gives the available inline size and the percentage
 * bases. A layout result (embi_lres) gives the fragment and the margins the parent collapses. Lengths
 * are float px. NaN means auto, indefinite, or not available, and isnan() tests it.
 */
#include "layout/box.h"
#include "layout/fragment.h"
#include "layout/layout.h"

#include "base/base.h"
#include "css/props.h"
#include "dom/dom.h"
#include "style/style.h"
#include "layout/text_shim.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ shaped runs */

/* A shaped text run owned by the document layout: the shaping result and its visual runs (for
 * W4). Freed with the rest of the document layout. */
/* The shaped run (text_shim.c). */
typedef struct embi_lay_text embi_lay_text;

/* ------------------------------------------------------------------ counters (counters.c) */

typedef struct embi_ctr_inst {
    embi_atom name;
    int32_t value;
    int32_t dir; /* +1 normally, -1 for reversed counters */
} embi_ctr_inst;

/* Counter instances in scope, innermost last. A mark is the height of the stack. */
typedef struct embi_ctrs {
    EMBI_VEC(embi_ctr_inst) st;
    uint32_t quote_depth;
    /* The stack height where the preceding siblings of the element being built begin. A
     * counter-reset that finds an instance of its name at or above this height replaces it
     * (CSS Lists 3 section 4.4: sibling scope), so the stack does not grow with the siblings. The
     * box builder sets it; instances below it belong to ancestors. */
    uint32_t sib_base;
    /* The element (and pseudo-element) whose counters embi_ctr_apply is applying, and the layout
     * context, for the initial value of a reversed counter that has none (embi_ctr_reversed_initial,
     * box.c). */
    void *lctx;
    void *cur_el;
    uint8_t cur_pe;
} embi_ctrs;

/* The counter-set / counter-increment entry of a style for a name: true when listed. The last entry wins. */
bool embi_ctr_style_op(const embi_style *s, embi_prop_id prop, embi_atom name, int32_t *num, bool *has_num);
/* The initial value of a reversed counter instantiated on cur_el without a value (CSS Lists 3 section 4.4.2). */
int32_t embi_ctr_reversed_initial(embi_ctrs *c, embi_atom name);

uint32_t embi_ctr_mark(const embi_ctrs *c);
void embi_ctr_truncate(embi_ctrs *c, uint32_t mark);
void embi_ctr_free(embi_ctrs *c);
/* Ends an element's counter scope for its following siblings (CSS Lists 3 section 4.4.1, counters.c). */
void embi_ctr_end_element(embi_ctrs *c, uint32_t start, uint32_t base, embi_atom keep);
/* Applies counter-reset, counter-set and counter-increment of a style, then the implicit
 * list-item increment for list items (implicit_list). When has_value, the list item's value is
 * set first (the li value attribute). */
void embi_ctr_apply(embi_ctrs *c, const embi_style *s, bool implicit_list, bool has_value,
                    int32_t value);
/* Forces the innermost instance of a counter to value (creating one when none is in scope), with
 * the given direction. Used by <ol start reversed>. */
void embi_ctr_force(embi_ctrs *c, embi_atom name, int32_t value, int32_t dir);
/* The value of the innermost instance (0 when none). */
int32_t embi_ctr_value(const embi_ctrs *c, embi_atom name);
/* Counter styles that are not keywords of the property tables. They are symbols (the counter value
 * is not shown): disclosure-open and disclosure-closed (CSS Counter Styles 3). The closed triangle
 * of a right-to-left list is a separate style. */
#define EMBI_CTR_DISCLOSURE_OPEN 0xF001u
#define EMBI_CTR_DISCLOSURE_CLOSED 0xF002u
#define EMBI_CTR_DISCLOSURE_CLOSED_RTL 0xF003u
/* The counter style of a style name (an ident such as decimal or disclosure-open). 0 when the name
 * is unknown. */
uint16_t embi_ctr_style_name(embi_str name);
/* Appends the value in a counter style (decimal, roman, alpha, greek, ...). Unknown styles use
 * decimal. */
void embi_ctr_format(embi_sb *sb, int32_t v, uint16_t style_kw);
/* The quote text for open-quote / close-quote (CSS Content 3), advancing the depth. */
void embi_ctr_quote(embi_ctrs *c, const embi_value *quotes, bool open, embi_str lang, embi_sb *sb);

/* ------------------------------------------------------------------ white space (whitespace.c) */

enum {
    EMBI_WS_COLLAPSE = 0,
    EMBI_WS_DISCARD,
    EMBI_WS_PRESERVE,
    EMBI_WS_PRESERVE_BREAKS,
    EMBI_WS_PRESERVE_SPACES,
    EMBI_WS_BREAK_SPACES,
};

uint8_t embi_lay_ws_mode(const embi_style *s);
/* Processes character data per white-space-collapse and text-transform (CSS Text 3 section 4).
 * *prev_space is the state at the start of the data: true when the output so far ends in a
 * collapsible space or the line is empty. It is updated to the state at the end. *in_word is the
 * same for text-transform: capitalize: true when the text so far ends inside a word, so a word
 * split over inline boxes is capitalised once. The caller resets both at block boundaries, atomic
 * inlines and breaks. */
/* The fast path of embi_lay_ws_process: when the bytes are all ASCII and collapsing the white space and
 * the style's text-transform leave them unchanged (no tab or line feed, no space after a space or after
 * prev_space), updates the state exactly as the full process would and returns true; else changes nothing. */
bool embi_lay_ws_ascii_unchanged(const uint8_t *p, uint32_t n, uint8_t mode, uint16_t transform,
                                 bool *prev_space, bool *in_word);
void embi_lay_ws_process(embi_sb *out, embi_str in, uint8_t mode, uint16_t transform,
                         bool *prev_space, bool *in_word);
/* embi_lay_transform packs a text-transform that combines values (CSS Text 4 section 2.1: a case keyword with full-width
 * and/or full-size-kana): the case keyword (or full-width) in the low bits, and these flags. */
#define EMBI_TT_FW 0x4000u
#define EMBI_TT_FSK 0x2000u
#define EMBI_TT_BASE 0x1FFFu
/* Passed to embi_lay_ws_process only (never in a style's transform): the text's writing system is Chinese, Japanese or Yi
 * (embi_lay_lang_cjy), which removes more segment breaks (CSS Text 3 section 4.1.2). */
#define EMBI_TT_CJY 0x8000u
bool embi_lay_lang_cjy(embi_str lang);
uint16_t embi_lay_transform(const embi_style *s);
/* Language tailoring of the text-transform output of one text node (CSS Text 3 section 2.1): Dutch IJ with capitalize,
 * and Greek upper case (no tonos, dialytika on a diphthong, the disjunctive eta keeps its tonos). word_before is the
 * in_word state before the node. */
void embi_lay_case_tailor(embi_sb *sb, embi_str lang, uint16_t transform, bool word_before);

/* General_Category classes of charclass.c: the punctuation (Ps, Pe, Pi, Pf, Po) and the combining marks that the
 * ::first-letter pseudo-element takes around its letter. */
bool embi_lay_is_fl_punct(uint32_t cp);
bool embi_lay_is_mark(uint32_t cp);

/* ------------------------------------------------------------------ the document layout */

/* One entry of the shaped-run cache of a document (line.c). The key is the text, the serialized shaping
 * style and the language and line-break mode. The cache is bounded by one layout: the entries that a
 * layout did not use are freed when it ends, so an unchanged page shapes nothing on its next layout
 * and the runs held are exactly those the last layout needed. */
typedef struct embi_lay_tent {
    embi_lay_text *t;
    uint64_t h, h2;              /* two independent hashes of the key (h is the map key) */
    uint64_t kh, kh2;            /* the two hashes of the shaping style (embi_lay_tkey h, h2): 128 bits stand for it */
    uint32_t gen;                /* the layout that used it last */
    uint32_t len;                /* bytes of text that follow the language */
    float adv;                   /* the advance of the whole run when clean (embi_tshim_advance), else 0 */
    uint8_t lb;
    uint8_t lang_len;
    bool clean;                  /* the whole text is one measured run with no line feed or soft hyphen: adv is the width of a text box with this text (inline.c tw) */
    /* lang_len bytes of language, then len bytes of text follow: a lookup compares them in the entry's own
     * cache lines instead of going through the shaped run. The style is compared by its 128-bit key. */
} embi_lay_tent;

/* Scratch buffers of the inline formatting contexts (inline.c): the vectors of an IFC keep their memory
 * from one paragraph to the next, so a layout does not grow and free them for every block of text. An IFC
 * takes the slots of the next free depth and gives them back when it ends (an atomic inline lays out an
 * IFC inside another, so the depth nests). The slots live with the document layout state (W3p) and are freed with it. */
typedef struct embi_ifc_slot {
    void *data;
    uint32_t cap;
} embi_ifc_slot;
#define EMBI_IFC_SLOTS 14
#define EMBI_IFC_POOL_DEPTH 6

typedef struct embi_lay_tkey embi_lay_tkey;
struct ifc_prep;
#define EMBI_MET_SLOTS 1024u
#define EMBI_TKFRONT 256u

/* Style-only facts of a box, computed once per style (position.c embi_lay_ssum_of): what make_box and the
 * stacking pass would otherwise read property by property for every box. */
/* A pool for the small cache entries (shaped-run entries and paragraph analyses, line.c and inline.c): they are
 * allocated in document order and read in document order by every layout of a rebuilt tree, so they sit in
 * slabs instead of between the big allocations of the shaped runs (W3q: a lookup missed the cache and the TLB
 * on every entry). Freed entries go to a free list of their size class and are reused; slabs are freed with the
 * document state. Entries over EMBI_POOL_MAX bytes are plain allocations. */
#define EMBI_POOL_GRAIN 32u
#define EMBI_POOL_MAX 2048u
#define EMBI_POOL_SLAB (128u * 1024u)
typedef struct embi_lay_pool {
    EMBI_VEC(uint8_t *) slabs;
    uint8_t *cur;
    size_t left;
    void *freel[EMBI_POOL_MAX / EMBI_POOL_GRAIN + 1u];
    size_t bytes;                 /* slab bytes held */
} embi_lay_pool;
/* Zeroed memory of sz bytes; nullptr when out of memory. */
void *embi_lay_pool_alloc(embi_lay_pool *p, size_t sz);
void embi_lay_pool_free(embi_lay_pool *p, void *e, size_t sz);
void embi_lay_pool_destroy(embi_lay_pool *p);

typedef struct embi_lay_ssum {
    const embi_style *key;
    uint16_t disp, pos, flt, ox, oy; /* keywords: display (block when not a keyword), position, float, overflow-x/y (0: none) */
    int32_t zi;                      /* z-index when it is an integer */
    bool zauto;                      /* z-index is auto */
    bool contain_lay, contain_paint; /* contain / container-type apply layout, paint containment */
    bool st_common;                  /* a stacking context from opacity, filter, isolation, blending, clip-path, mask, will-change */
    bool st_block;                   /* one from transform, perspective, individual transforms, containment (not inline boxes) */
    bool cb_block;                   /* a containing block for all descendants (not inline boxes) */
    bool ctr_none;                   /* counter-reset, counter-set and counter-increment are all none (embi_ctr_apply would do nothing) */
    uint8_t ws;                      /* embi_lay_ws_mode(style) */
    uint16_t tt;                     /* embi_lay_transform(style) */
    uint16_t ub, va;                 /* unicode-bidi and vertical-align keywords (0: none), read for every inline box of a paragraph */
    bool rtl;                        /* direction: rtl */
    bool xf;                         /* embi_lay_has_transform(style) */
    bool edges_vary;                 /* a margin or padding is a percentage, calc() or a container unit (inline.c) */
    bool calc_size;                  /* W3q: one of width, min-width, max-width, height, min-height, max-height is calc-size() */
    bool mc;                         /* column-count or column-width is not auto (a multi-column container when it is a block container) */
    bool clamp;                      /* continue: collapse or discard: a line-clamp container when it is a block container (linecl.c) */
    bool incr_dep;                   /* W6b: the box's size can depend on its content or on more than the containing block's size
                                      * (an intrinsic keyword in a size property, aspect-ratio, multicol, containment, a vertical
                                      * writing mode): not a link of a partial relayout's chain (incr.c) */
} embi_lay_ssum;
#define EMBI_SSUM_N 256u /* initial size of the table; it doubles at half load */

/* CF1: a box the last surgery built again (an entry of embi_lay_doc.reb) and, when it replaced a recorded box of the same
 * style, that box's fragment and record (index + 1 in precs): the partial relayout starts at the new box itself. */
typedef struct embi_reb_old {
    struct embi_box *box;
    struct embi_frag *frag; /* nullptr: no record to start from */
    uint64_t rec;
} embi_reb_old;

typedef struct embi_lay_doc {
    embi_document *doc;
    bool scrolled;                     /* a scroll offset was set on a fragment (the relayout keeps them: layout.c, W5d2) */
    embi_lay_pool pool;                /* tents and analyses (see embi_lay_pool) */
    embi_lay_ssum *stab;               /* open-addressed table by style pointer (position.c); emptied with the box tree */
    uint32_t stab_n, stab_cnt;
    struct { embi_atom a; uint16_t ns; uint8_t f; } tagc[256]; /* box.c: what the element names mean (tag_flags), by atom; atoms never change */
    embi_ifc_slot ifc_pool[EMBI_IFC_POOL_DEPTH][EMBI_IFC_SLOTS];
    uint32_t ifc_depth;
    embi_arena ba;   /* boxes and the strings of the box tree */
    embi_arena fa;   /* fragments */
    embi_frag *free_frag;  /* recycled fragments (block.c): an intrusive list through `next` */
    const embi_fmet *mtab[EMBI_MET_SLOTS]; /* interned metrics records of the fragments (embi_lay_met), emptied per layout */
    embi_fext *free_ext;   /* recycled side records, linked through `ext` of the record's sticky field */
    uint32_t frag_allocs;  /* fragments taken from the arena or the free list in the last layout */
    uint32_t frag_reused;  /* of those, the ones that came from the free list */
    /* W3p: the box tree outlives a layout. It is rebuilt only when the styles of the document were
     * recomputed (the epoch), the viewport or the scrollbar size changed, or an image size was asked
     * for (the embedder's sizes can change without any DOM change). Otherwise the next layout resets the
     * per-layout fields of the boxes (fragments, scrollbar gutters) and lays the same boxes out again. */
    embi_box *root_box;    /* the built tree, nullptr when none is kept */
    uint64_t t_epoch;
    float t_vw, t_vh, t_sb;
    uint32_t t_depth;
    uint64_t deep_epoch;   /* the style epoch deep was computed at (0: not computed) */
    bool deep;             /* the DOM is nested deeper than EMBI_LAY_INLINE_DEPTH (valid at deep_epoch) */
    uint32_t builds;       /* how many times the box tree was built (embi_layout_tree_builds) */
    bool has_images;       /* the build asked the embedder for an image size */
    bool has_sticky;       /* the built tree has a position: sticky box (position.c walks for them only then) */
    bool has_xf;           /* the built tree has a box with a transform, translate, rotate or scale (hit.c, R3c) */
    embi_hashmap tkeys;    /* embi_style * -> embi_lay_tkey * (keys live in ta) */
    embi_arena ta;         /* W6e: the embi_lay_tkey records and their blobs (line.c). They are only read by pointer within a layout
                            * (the shaped-run cache keeps their hashes), so the arena is reset whenever tkeys is emptied: a
                            * restyle that replaces the styles of 200 items every frame no longer grows the box arena. */
    embi_hashmap langs;    /* embi_element * -> embi_attr * of the nearest lang attribute, or 1 for none (line.c) */
    /* The analysis of the inline content of simple IFC boxes (inline.c): break candidates and advance
     * tables are functions of the text and styles alone, so they live with the box tree, in an arena of
     * their own (derived text data, like the shaped runs: not counted as box or fragment memory). */
    /* IFC embi_box * -> ifc_prep * (this tree's boxes; the entries belong to pmap): an open-addressed table of 16-byte slots by
     * box address, at most half full, emptied with the box tree (W3q: it was a hashmap, three probes of 24-byte robin hood slots
     * per analysis on a rebuild). */
    struct { const void *root; struct ifc_prep *pc; } *pfront;
    uint32_t pfront_n, pfront_cnt;
    embi_hashmap ecache;   /* style (the box's, or the initial one) -> its used edges (size.c), in `pa` */
    struct { uint64_t key; const void *e; } efront[64]; /* direct-mapped front of ecache (size.c) */
    embi_arena pa;
    /* The paragraph analyses are keyed by content (W3q): the shaped runs of the text, the break modes and the
     * item widths, not by box. They survive a rebuild of the box tree and are dropped when a layout that
     * rebuilt the tree did not use them (embi_lay_tcache_sweep). pmap: hash -> chain of ifc_prep, preps: all. */
    embi_hashmap pmap;
    embi_layout_cold_stats cold; /* where the time went (embi_layout_cold_stats_get) */
    bool shaping_para;           /* the next shaped run is a paragraph's (analyze), for cold */
    EMBI_VEC(struct ifc_prep *) preps;
    size_t prep_bytes;
    struct { const embi_style *key; const embi_lay_tkey *k; } tkfront[EMBI_TKFRONT]; /* 2-way front of tkeys (line.c) */
    struct { const void *key; uintptr_t v; } lfront[EMBI_TKFRONT]; /* direct-mapped front of langs (line.c) */
    EMBI_VEC(embi_lay_tent *) tents;
    embi_lay_tent **tmemo;       /* the index of the shaped-run cache: open-addressed table of the entries of tents (line.c) */
    uint32_t tmemo_n;            /* entries of tmemo, a power of two (0: none yet) */
    uint32_t tgen;
    embi_frag *icb;  /* the initial containing block fragment, nullptr when there is no root */
    /* ---- W6b (incr.c): incremental layout state ---- */
    bool tracking;       /* the style engine records this document's change set for this layout state */
    bool ctr_used;       /* the built tree used counters or quotes (the counter state is not kept: no surgery) */
    bool ctr_read;       /* CF1: the built tree read a counter value or the quote depth (counter(), counters(), quotes, a
                          * marker that numbers): without that, the counter state is not observable and a surgery may
                          * rebuild a subtree that takes part in the counters (embi_lay_rebuild_element) */
    bool has_contents;   /* the built tree has a display: contents element */
    bool has_fl;         /* the built tree has a ::first-letter box */
    bool force_full;     /* the next layout rebuilds the tree (an external change the delta does not describe) */
    bool flush_text;     /* ... and shapes every text again (the font registry gained a face; embi_layout_flush_text) */
    uint8_t plan;        /* EMBI_LP_*: what the next layout run does with the tree (layout.c) */
    uint8_t last_kind;   /* EMBI_LU_*: what the last embi_layout_document did */
    uint32_t surgeries;  /* box-tree surgeries since the tree was built */
    size_t ba_base;      /* bytes of the box arena after the build (the growth bounds the surgeries) */
    uint32_t nb_base, nb_cur; /* boxes after the build, and now (a tree that shrank a lot is built again, to free the rest) */
    uint32_t tents_base; /* shaped-run entries after the build (the cache grows with partial updates, not shrinks) */
    uint32_t rebuilt_boxes; /* boxes built by the last surgery */
    uint64_t n_surgery, n_paint, n_none, n_full;
    EMBI_VEC(embi_element *) pdirty; /* elements whose paint changed since the paint module last looked (paint-only updates) */
    bool pover;          /* ... or too many: repaint everything */
    uint64_t fserial;    /* counts the layouts that rebuilt the fragments (the paint module's cache is valid for one) */
    /* ---- W6b: partial relayout (incr.c) ---- */
    embi_hashmap prmap;  /* block embi_box * -> index + 1 in precs: how it was laid out (embi_prec) */
    EMBI_VEC(struct embi_prec) precs;
    embi_arena ra;       /* the exclusion rectangles of the records */
    EMBI_VEC(embi_element *) reb; /* elements whose boxes the last surgery built again */
    EMBI_VEC(embi_reb_old) rebo; /* CF1: per entry of reb, what the box built again replaced */
    bool abs_gone;       /* the last surgery dropped an out-of-flow box: its fragment sits under its containing block, outside the
                          * subtree a partial relayout replaces */
    bool last_partial;   /* the last layout patched the fragment tree instead of laying the whole tree out */
    bool scratch;        /* a throwaway state that verifies another (EMBER_VERIFY_INCREMENTAL): no delta, no records */
    size_t fa_base;      /* bytes of the fragment arena after the last full layout */
    uint32_t partials;   /* partial relayouts since the tree was built */
    uint64_t n_partial;
    bool reuse_on;       /* a partial relayout runs: lay_child reuses the fragments of children that did not change */
    embi_hashmap reusedmap; /* fragment -> 1: the fragments reused in this relayout (their insides are final) */
    uint32_t reused;     /* fragments reused by the last partial relayout */
    embi_hashmap rebanc; /* box -> 1: the box ancestors of the boxes this surgery built again (never reused) */
} embi_lay_doc;

struct embi_layout_engine {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_image_size_fn image_size;
    void *image_user;
    float vw, vh, scrollbar;
    embi_hashmap *docs; /* embi_document * -> embi_lay_doc * */
    bool scratch;       /* W6b: a throwaway engine that builds a document again to verify an incremental update (layout.c) */
    bool images_stable; /* W6b: the embedder's image sizes only change with a DOM or style change (embi_layout_set_images_stable) */
    embi_layout_control_fn control_fn; /* W5e: the painter's form control state hook */
    void *control_user;
};

/* Depth of the container-size stack (see embi_lctx.cq). Deeper containers are not recorded. */
#define EMBI_CQ_DEPTH 32

struct embi_pos;

/* The context of one layout run. */
/* Line clamping (linecl.c). While a line-clamp container is laid out for the second time, with the number of lines its
 * clamp point keeps, cl holds what the in-flow content of its block formatting context needs: the lines that may still
 * be laid out (left; zero: the clamp point is reached and the rest is not laid out) and whether the last of them ends in
 * the block overflow ellipsis (ell, with the container's style for the string). A box that establishes its own
 * formatting context lays out with cl.on false and restores it (embi_lay_box). */
typedef struct embi_clamp {
    bool on;
    bool ell;           /* the last kept line takes the block overflow ellipsis */
    uint32_t left;      /* lines that may still be laid out */
} embi_clamp;

typedef struct embi_lctx {
    embi_layout_engine *eng;
    embi_lay_doc *ld;
    embi_style_engine *style;
    embi_text *text;
    embi_fontdb *fonts;
    const embi_style *initial;
    const embi_style *root_style;
    float vw, vh, scrollbar;
    float root_px;
    embi_layout_image_size_fn image_size;
    void *image_user;
    embi_ctrs ctr;
    /* Content sizes of the open containers with container-type (innermost last): the cq* units
     * of their descendants resolve against them. h is NaN when the block size is not definite. */
    struct {
        float w, h;
    } cq[EMBI_CQ_DEPTH];
    uint32_t ncq;
    const embi_box *span_k; /* multicol.c: the plain block being laid out in the flow of a multi-column container; its flow is given span_w */
    float span_w;
    bool no_pseudo;  /* the document has no generated (pseudo-element) styles: the box build skips the lookups */
    uint32_t bdepth; /* element nesting depth while the box tree is built */
    uint32_t max_depth; /* the depth past which elements get no box (EMBI_LAY_MAX_DEPTH) */
    embi_hashmap tmodels; /* table embi_box * -> the table model built for it in this layout (table.c) */
    EMBI_VEC(void *) tmods; /* the models of tmodels, freed when the layout ends */
    struct embi_pos *pos; /* the out-of-flow records of this layout (position.c, W3d), or nullptr */
    /* W3e: the flex item whose used sizes are forced (flex.c). While force_box is laid out, its
     * border-box width (force_wset) and height (force_hset) are force_w and force_h: they replace its
     * own sizes, and the flex algorithm has already applied min and max. Nested layouts save and
     * restore them, so only that box sees them. */
    embi_box *force_box;
    float force_w, force_h;
    bool force_wset, force_hset;
    bool force_nominmaxw; /* the forced box's min-width and max-width are ignored (replaced boxes) */
    bool force_hnone; /* the forced box's height is auto for this layout (its content height) */
    const embi_box *hraw_box; /* with force_hnone on this box: its min-height and max-height are ignored too (flex.c) */
    /* W4g4c: the percentage basis for the heights of the boxes whose intrinsic sizes are being computed (size.c): the
     * definite content height of their parent when ih_on, else none. ih_need counts the percentage heights that
     * looked at it, so that an intrinsic size that depends on it is not cached. Callers of embi_lay_intrinsic set it
     * (embi_lay_ih_push) to the box's containing block height; embi_lay_box clears it for the layouts it starts. */
    bool ih_on;
    float ih_cb;
    uint32_t ih_need;
    bool force_hsoft; /* force_h is not definite for the percentages of the forced box's children (a table cell
                       * stretched to its row: Chromium resolves them only against the cell's own height) */
    /* W3f: the request of a grid to the layout of one of its subgrid items (grid.c): the item's grid
     * takes its tracks from the parent's. Set only around that item's layout. */
    void *gsub;
    /* W3g: a cell laid out in a collapsing table takes its half borders from edge_bdr (top, right,
     * bottom, left) instead of its own borders (table.c). Set only around that cell's layout. */
    const embi_box *edge_box;
    float edge_bdr[4];
    /* W6b (incr.c): every box on the path from the root is a plain in-flow block laid out by its parent's block flow with
     * a fill-width constraint and a size that does not depend on its content. Only such a box is recorded for the partial
     * relayout; via_flow says that the embi_lay_box call in progress came from block flow (lay_child). */
    bool chain_ok;
    bool via_flow;
    embi_clamp cl; /* the line clamp of the BFC being laid out (linecl.c) */
    float cl_limit; /* the block size limit (content box) of the block container laid out last: height and max-height, else infinity */
} embi_lctx;

/* Elements nested deeper than this get no box, in every build: the recursion of the box builder and of
 * layout is bounded, so a very deep DOM cannot overflow the stack. The cap is the same in a Release
 * build, a Debug build and under a sanitizer because the layout of a deep page does not run on the
 * caller's stack: when the DOM is nested deeper than EMBI_LAY_INLINE_DEPTH, embi_layout_document runs
 * it on a worker thread with EMBI_LAY_STACK_BYTES of stack (SDL_thread), which holds EMBI_LAY_MAX_DEPTH
 * levels of the largest frames measured (clang ASan, about 16 KB per level with the builder, layout,
 * positioning and fragment walks on the stack: 512 levels use about 8 MB). The deepest built element is
 * an empty block. If the thread cannot be created embi_layout_document logs an error and returns false
 * (the document keeps no layout): the result never depends on the environment. */
#define EMBI_LAY_MAX_DEPTH 512u
#define EMBI_LAY_INLINE_DEPTH 16u
#define EMBI_LAY_STACK_BYTES ((size_t)32 << 20)

/* The fragment arena of a document keeps its blocks from one layout to the next (W3p): embi_lay_rewind
 * empties it, embi_lay_alloc hands out the retained blocks again before it asks the heap for new ones. */
void *embi_lay_alloc(embi_arena *a, size_t size, size_t align);
void embi_lay_rewind(embi_arena *a);

#define EMBI_LAY_NEW(c, T) ((T *)embi_arena_alloc(&(c)->ld->ba, sizeof(T), alignof(T)))
/* Sanitizer builds poison the part of a fragment past its tier (block.c) and unpoison arena memory on reuse. */
#if defined(__SANITIZE_ADDRESS__)
#define EMBI_LAY_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EMBI_LAY_ASAN 1
#endif
#endif
#ifdef EMBI_LAY_ASAN
#include <sanitizer/asan_interface.h>
#endif

#define EMBI_LAY_NEW_FRAG(c) ((embi_frag *)embi_lay_alloc(&(c)->ld->fa, sizeof(embi_frag), alignof(embi_frag)))

/* ------------------------------------------------------------------ constraint and result */

enum {
    EMBI_FIT_FILL = 0,   /* auto widths fill the available size (block-level boxes) */
    EMBI_FIT_SHRINK = 1, /* auto widths shrink to fit (atomic inlines, abspos, floats) */
};

/* The exclusion space of a block formatting context (exclusion.c, W3b): the margin boxes of its
 * floats in placement order, in the BFC's own coordinates (y grows down from the BFC root's
 * content top). */
typedef struct embi_excl_rect {
    float x0, y0, x1, y1;
    float ymax; /* W3q: the greatest y1 of this rect and the ones before it in the list (set by embi_excl_add): a query at or below it
                 * has nothing to find in the rects up to this one */
    bool left; /* a left float (else a right float) */
} embi_excl_rect;

typedef struct embi_excl {
    EMBI_VEC(embi_excl_rect) rects;
} embi_excl;

/* Sides for clearance (embi_excl_clear_y and embi_lay_clear_sides). */
enum {
    EMBI_CLR_LEFT = 1,
    EMBI_CLR_RIGHT = 2,
};

typedef struct embi_cs {
    float avail_w; /* the available inline size of the margin box */
    float cb_w;    /* the containing block's width (percentage base); NaN when indefinite */
    float cb_h;    /* the containing block's height (percentage base); NaN when indefinite */
    uint8_t fit;   /* EMBI_FIT_* */
    /* W3b: the exclusion space of the BFC the box lies in (nullptr: the box starts a frame of its
     * own, as a BFC root does), the BFC x of the containing block's content-box left edge, and the
     * BFC y of the box's border-box top as its parent estimated it (see block.c). */
    embi_excl *excl;
    float bfc_x;
    float bfc_y;
    /* W3b: a BFC box beside floats resolves its width in the free interval, but its margins are
     * measured from the containing block, so the parent hands it the margins that remain inside the
     * interval (CSS 2.1 section 9.5: only the border box keeps out of the float margin boxes). When
     * mg_ovr is set, ml_ovr and mr_ovr replace the specified left and right margins that are not
     * auto (an auto margin is NaN here and still resolves against the interval). */
    bool mg_ovr;
    float ml_ovr, mr_ovr;
} embi_cs;

/* Margin collapsing strut: the largest positive and most negative margins seen. */
typedef struct embi_strut {
    float pos, neg;
} embi_strut;

static inline void embi_strut_add(embi_strut *s, float m)
{
    if (m >= 0) {
        if (m > s->pos) {
            s->pos = m;
        }
    } else if (m < s->neg) {
        s->neg = m;
    }
}

static inline float embi_strut_val(const embi_strut *s)
{
    return s->pos + s->neg;
}

/* Merges a margin set into a strut (collapsing): largest positive, most negative. */
static inline void embi_strut_merge(embi_strut *s, embi_strut o)
{
    if (o.pos > s->pos) {
        s->pos = o.pos;
    }
    if (o.neg < s->neg) {
        s->neg = o.neg;
    }
}

typedef struct embi_lres {
    embi_frag *frag; /* the border-box size is set (w, h); the position is not */
    float ml, mr;    /* used horizontal margins */
    float mt, mb;    /* vertical margins as numbers (the strut values), for replaced and inline */
    bool selfc;      /* self-collapsing: the margins collapse through and it has no height */
    embi_strut mt_s; /* the top margin set the parent collapses (own and a first child's when the top
                      * is open); a self-collapsing box: all its margins */
    embi_strut mb_s; /* the bottom margin set (zero for a self-collapsing box) */
    embi_strut top_s; /* selfc only: the top margin set alone (own and first child's), which places
                       * the box; mt_s is all its margins, which the next sibling collapses with */
} embi_lres;

typedef struct embi_edges {
    float l, t, r, b;
} embi_edges;


/* ------------------------------------------------------------------ box style access */

/* The computed value of a property for a box: inherited properties come from the box's style,
 * and non-inherited ones from the box's style too, except for anonymous boxes, which read the
 * initial value. Returns nullptr for shorthands. */
const embi_value *embi_lay_prop(const embi_lctx *c, const embi_box *b, embi_prop_id p);
/* The calc-size() expression of a sizing property (CSS Values 5 section 10.1): the calc-sum of a
 * computed calc-size(<basis>, <calc>), or nullptr. embi_lay_prop reads such a value as its basis, so
 * a caller that does not know calc-size() sees auto or the basis length. The caller applies the
 * expression to the used size with embi_lay_size_eval, once, where it knows that size. */
const embi_value *embi_lay_size_expr_slow(const embi_lctx *c, const embi_box *b, embi_prop_id p);
/* W3q: boxes whose style has no calc-size() in the six size properties (EMBI_BF2_CALCSIZE) skip the style read. */
static inline const embi_value *embi_lay_size_expr(const embi_lctx *c, const embi_box *b, embi_prop_id p)
{
    return (b->flags2 & EMBI_BF2_CALCSIZE) != 0 ? embi_lay_size_expr_slow(c, b, p) : nullptr;
}
/* Whether v is a calc-size(<basis>, <calc>) value. */
bool embi_lay_is_calc_size(const embi_value *v);
/* embi_lay_prop and embi_lay_size_expr in one lookup: returns the basis (as embi_lay_prop) and sets *expr to the
 * calc-sum, or nullptr when the value is not a calc-size(). */
const embi_value *embi_lay_prop_calc(const embi_lctx *c, const embi_box *b, embi_prop_id p, const embi_value **expr);
/* Evaluates a calc-size() expression with `size` (px, in the property's box-sizing space) and pct
 * as the percentage base. NaN when it does not resolve. */
float embi_lay_size_eval(const embi_lctx *c, const embi_box *b, const embi_value *expr, float size, float pct);
/* The style-only summary of style s. The table is open-addressed and doubles at half load, so the pointer is valid only until the next call: use it at once or copy the struct. */
const embi_lay_ssum *embi_lay_ssum_of(const embi_lctx *c, const embi_style *s);
/* The box's font size in px (the em base). */
float embi_lay_font_px(const embi_lctx *c, const embi_box *b);
/* The px value of a word-spacing / letter-spacing value (a length, a percentage of the element's own font size, or a calc of
 * them) for an element with font size font_px. 0 for normal. */
float embi_lay_spacing_px(const embi_value *v, float font_px);
/* The computed font size of a style in px (16 when it has none). */
float embi_lay_font_px_style(const embi_style *s);
/* True when the box's auto height ignores its content (size containment in the block axis). */
bool embi_lay_block_size_contained(const embi_lctx *c, const embi_box *b);

/* A length or percentage as px. NaN for auto, keywords, and percentages when pct is NaN. */
float embi_lay_len(const embi_lctx *c, const embi_box *b, const embi_value *v, float pct);
/* True for an absent value or the keyword auto. */
bool embi_lay_is_auto(const embi_value *v);
/* Border, padding and margin widths of a box against a containing block width. Margins that are
 * auto are NaN in mar. bdr and pad are never NaN. */
/* The used border width of one side (0 = top, 1 = right, 2 = bottom, 3 = left), zero for none and
 * hidden (size.c). */
float embi_lay_border_side(const embi_lctx *c, const embi_box *b, int side);
/* The collapsed outer half borders of a table box with border-collapse: collapse (top, right, bottom,
 * left in h), and false for other tables (table.c, W3g). */
/* Frees the table models a layout cached (table.c). */
void embi_table_models_free(embi_lctx *c);
bool embi_table_collapse_edges(const embi_lctx *c, const embi_box *b, float h[4]);
void embi_lay_edges(const embi_lctx *c, const embi_box *b, float cbw, embi_edges *bdr,
                    embi_edges *pad, embi_edges *mar);

/* ------------------------------------------------------------------ box building (box.c) */

/* Builds the box tree of the root element. Returns nullptr when the root has no box. */
embi_box *embi_lay_build_root(embi_lctx *c, embi_element *root);
/* Clears the box slot of every element in the document (before a rebuild). */
void embi_lay_clear_boxes(embi_document *doc);

/* ------------------------------------------------------------------ sizing (size.c) */

/* The intrinsic content sizes of a box (CSS Sizing 3): min-content and max-content, for the box's
 * own content, without its own width and margins. Cached on the box. */
void embi_lay_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* The percentage basis of intrinsic sizing (embi_lctx.ih_on): push sets it to h (NaN: none) and returns the old one. */
typedef struct embi_ih {
    bool on;
    float h;
} embi_ih;
static inline embi_ih embi_lay_ih_push(embi_lctx *c, float h)
{
    embi_ih s = {c->ih_on, c->ih_cb};
    c->ih_on = !isnan(h);
    c->ih_cb = h;
    return s;
}
static inline void embi_lay_ih_pop(embi_lctx *c, embi_ih s)
{
    c->ih_on = s.on;
    c->ih_cb = s.h;
}
static inline float embi_lay_ih_get(const embi_lctx *c)
{
    return c->ih_on ? c->ih_cb : NAN;
}
/* The contribution of a box to its parent's intrinsic size: the margin box, with its own width
 * when that is a definite length. */
void embi_lay_intrinsic_outer(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* Lays out a replaced box (image, canvas, video, form control, ...). */
void embi_lay_replaced(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);

/* Resolves the horizontal margins of a box of border-box width bw in an available width avail
 * (CSS 2.1 section 10.3.3): auto margins take the rest, or split it when both are auto. ml and mr
 * are in and out; NaN marks an auto margin. */
void embi_lay_hmargins(float avail, float bw, float *ml, float *mr, bool rtl);
/* The direction of the containing block of a box, for the over-constrained margin rule (CSS 2.1
 * sections 10.3.3 and 10.3.4 look at the containing block's direction, not the box's own). The root
 * box uses its own direction (the initial containing block takes the root's). */
bool embi_lay_cb_rtl(const embi_lctx *c, const embi_box *b);

/* ------------------------------------------------------------------ block flow (block.c) */

/* Lays out a block-level or atomic box (CSS 2.1 section 9.4.1 and 10.3.3, 10.6.3). */
void embi_lay_block(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);

/* ------------------------------------------------------------------ multi-column layout (multicol.c) */

/* The used column count, column width and gap of a multi-column container whose content box is U wide (CSS Multi-column 1
 * section 3.4). */
typedef struct embi_mc_geo {
    uint32_t n;
    float colw, gap;
} embi_mc_geo;

void embi_lay_mc_geom(const embi_lctx *c, const embi_box *b, float U, embi_mc_geo *g);
/* The min-content and max-content widths of a multi-column container from those of its content (section 3.2 of the spec). */
void embi_lay_mc_intrinsic(const embi_lctx *c, const embi_box *b, float *mn, float *mx);
/* Whether k, a child of a multi-column container, spans all its columns (column-span: all). */
bool embi_lay_is_spanner(const embi_lctx *c, const embi_box *k);
/* Whether k is an ordinary block container (no new formatting context, no fragmentation context, in flow): a column-span: all
 * box below it spans the columns of the multi-column container above it (section 6 of the spec). */
bool embi_lay_mc_plain(const embi_box *k);

typedef struct embi_mc_in {
    embi_mc_geo g;
    bool rtl;
    bool fill_auto;  /* column-fill: auto */
    bool definite;   /* the content box height is definite (cap is it) */
    float ox, oy;    /* the content start in the container fragment */
    float ch;        /* the content height of the flow laid out in one column of the column width */
    float cap;       /* the height limit of the content box (height, else max-height), INFINITY for none */
    embi_edges bdr;
} embi_mc_in;
/* Cuts the flow in f (laid out at the column width, one tall column) into columns and sets: the children of f are replaced
 * by the pieces in their columns. Returns the content height. */
float embi_lay_multicol(embi_lctx *c, embi_box *b, embi_frag *f, const embi_mc_in *in);

/* linecl.c: the clamp plan of a line-clamp container laid out once (limit: its content box size limit; content_top: the
 * content box's top from the fragment's border top). True when its content has to be laid out again with *plan. */
bool embi_lay_clamp_plan(embi_lctx *c, embi_box *b, const embi_frag *f, float limit, float content_top, embi_clamp *plan);
/* True for an in-flow box that block flow lays out as one unit of a line clamp (an independent formatting context). */
bool embi_lay_clamp_indep(const embi_box *b);
/* Registers the out-of-flow descendants of k (left out by the clamp) whose containing blocks lie outside k. */
void embi_lay_clamp_skip(embi_lctx *c, embi_box *k, embi_box *sbox, float sx, float sy);
/* Dispatches a box to its formatting context. Out-of-flow boxes are not passed here. */
void embi_lay_box(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);
/* Fragment helpers (block.c). A new fragment becomes b->frag when it is the box's first one. */
embi_frag *embi_lay_new_frag(embi_lctx *c, embi_box *b, uint8_t kind);
void embi_lay_frag_append(embi_frag *parent, embi_frag *child);
/* The interned metrics record equal to *key (nullptr for all zeros); fragments with the same border, padding
 * and margins share one. */
const embi_fmet *embi_lay_met(embi_lay_doc *ld, const embi_fmet *key);
/* Whether the box tree of the document has a box with a transform (set while the boxes are made; hit.c). */
bool embi_lay_doc_has_transform(const embi_layout_engine *e, const embi_document *doc);
/* Sets the used border widths and paddings of a box fragment (keeps its margins), or its margins (keeps the
 * rest). Each call interns the new combination, so set the margins once the fragment is placed. */
void embi_lay_frag_set_edges(embi_lay_doc *ld, embi_frag *f, const embi_edges *bdr, const embi_edges *pad);
void embi_lay_frag_set_mg(embi_lay_doc *ld, embi_frag *f, float l, float t, float r, float b);
void embi_lay_ov_add_child(embi_frag *f, const embi_frag *child);
void embi_lay_frag_set_size(embi_frag *f, float w, float h);
/* True for a float (float.c). Floats are placed by block flow, not laid out as flow children. */
bool embi_lay_float_out_of_flow(const embi_box *b);
/* True for a left float: float left, or inline-start in ltr and inline-end in rtl. */
bool embi_lay_float_left(const embi_lctx *c, const embi_box *b);
/* The sides a box clears (EMBI_SIDE_* bits, 0 for none), with inline-start and inline-end resolved
 * against the box's containing block (float.c). */
uint8_t embi_lay_clear_sides(const embi_lctx *c, const embi_box *b);
/* Places a float (float.c, CSS 2.1 section 9.5.1) in the exclusion space ex of its BFC. cx and cw
 * are the BFC x and the width of the containing block's content box, cb_h the height base, and top
 * the earliest BFC y of the float's margin edge (the flow position). The float is laid out and
 * recorded in ex; the returned fragment has its border-box size set and its position unset. *bx
 * and *by get the BFC position of its border-box top-left corner. nullptr when there is no box.
 * Inline formatting contexts call this for each float they meet (W3c). */
embi_frag *embi_lay_float(embi_lctx *c, embi_box *b, embi_excl *ex, float cx, float cw, float cb_h,
                          float top, float *bx, float *by);
/* The two halves of embi_lay_float: the layout of the float (its margin box size and side), and its placement in
 * the exclusion space. An inline formatting context lays a float out first to see whether it fits on the line. */
typedef struct embi_float_geom {
    float ml, mr, mt, mb; /* the margins used */
    float mw, mh;         /* the margin box size */
    bool left;
} embi_float_geom;
embi_frag *embi_lay_float_prepare(embi_lctx *c, embi_box *b, float cx, float cw, float cb_h, embi_float_geom *g);
void embi_lay_float_place(embi_lctx *c, embi_box *b, const embi_frag *fr, const embi_float_geom *g, embi_excl *ex,
                          float cx, float cw, float top, float *bx, float *by);

/* ------------------------------------------------------------------ inline content (inline.c) */

/* Lays out the inline content of IFC box b (one unbroken line; see W3c) into bf: appends a line
 * box fragment and sets the content height. Returns false when there is no line (only collapsible
 * white space). */
/* The frame an inline formatting context lays its lines in: the BFC's exclusion space ex (nullptr:
 * no floats), the BFC x and y of the containing block's content box (cx, cy), the position of the
 * box's content box in its parent's fragment (ox, oy), and the containing block's height base. */
typedef struct embi_ifc_frame {
    embi_excl *ex;
    float cx, cy, ox, oy, cb_h;
} embi_ifc_frame;
bool embi_lay_ifc(embi_lctx *c, embi_box *b, embi_frag *bf, const embi_ifc_frame *fr, float cw,
                  float *ch, float *bl_first, float *bl_last);
/* The intrinsic content sizes of the inline content of an IFC box. */
void embi_lay_ifc_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* The shaped run of a TEXT or MARKER box (shaped on first use). nullptr when it is empty. */
embi_lay_text *embi_lay_shape(embi_lctx *c, embi_box *b);
/* As embi_lay_shape, and the 128-bit content key of the run in *k1 and *k2 when this call looked the run up
 * (left unchanged for a box that already has its run: embi_tshim_key reads it from the run then). */
embi_lay_text *embi_lay_shape_k(embi_lctx *c, embi_box *b, uint64_t *k1, uint64_t *k2);
/* As embi_lay_shape_k; ovr shapes the text as inside a unicode-bidi: bidi-override ancestor (every character strong
 * in the box's direction). */
embi_lay_text *embi_lay_shape_k_ovr(embi_lctx *c, embi_box *b, uint64_t *k1, uint64_t *k2, bool ovr);
/* The text style of a computed style for shaping (family names in fams, up to cap). */
void embi_lay_text_style(const embi_style *s, embi_lay_tstyle *ts, embi_str *fams, uint32_t cap);

/* The shaping style of a computed style with its serialized key and its font metrics, built once per
 * style in a layout (line.c). The line metrics are filled on first use. */
typedef struct embi_lay_tkey {
    embi_lay_tstyle ts;       /* families points into fams */
    embi_str fams[16];
    const uint8_t *blob;      /* the serialized style, the key of the shaped-run cache */
    uint32_t blob_len;
    uint64_t h, h2;           /* two independent hashes of blob */
    uint16_t amode;           /* the style properties the inline analysis reads, packed (embi_lay_ifc_amode) */
    bool has_lm;              /* the line metrics below are valid (cached only when the line-height does not
                               * depend on the layout context: a relative unit or calc() is not cached) */
    bool has_xh;
    float lm_a, lm_d;         /* the content area above and below the baseline with half-leading */
    float lm_ca, lm_cd;       /* the font's rounded ascent and descent (the CSS 2.1 10.6.1 content area) */
    float xh;                 /* the x-height */
} embi_lay_tkey;
const embi_lay_tkey *embi_lay_tkey_of(embi_lctx *c, const embi_style *s);
/* True when the line metrics of s depend on s alone (line.c). */
bool embi_lay_lh_static(const embi_style *s);
/* The shaped run of text under key k with a language and a line-break mode, from the document cache
 * (shaped on a miss). The run is owned by the cache and is valid until the next layout of the document
 * ends. nullptr when the text is empty or cannot be shaped. */
embi_lay_text *embi_lay_shape_text(embi_lctx *c, const embi_lay_tkey *k, embi_str lang, uint8_t lb,
                                   const char *text, uint32_t len);
/* As embi_lay_shape_text; *ent (when not nullptr) receives the cache entry of the run, whose generation the
 * caller may refresh to keep the run alive through the sweep of the layout. */
embi_lay_text *embi_lay_shape_text_ent(embi_lctx *c, const embi_lay_tkey *k, embi_str lang, uint8_t lb,
                                       const char *text, uint32_t len, embi_lay_tent **ent);
/* The style properties the paragraph analysis reads (break modes, white space, nowrap, hyphens), packed:
 * two styles with the same value, shaping key and text analyse the same way. */
uint16_t embi_lay_ifc_amode(const embi_style *s);
/* Frees the IFC scratch buffers of a layout (inline.c). */
void embi_lay_ifc_pool_free(embi_lay_doc *ld);
/* Lowers the longest text a box holds (box.c), for tests of the split of long text; 0 restores EMBI_TEXT_MAX.
 * Process wide: set it before the layout and restore it after. */
void embi_lay_set_text_max(uint32_t max);
/* Clears what a layout leaves in the boxes of a kept tree: the fragment pointers and the scrollbar gutter
 * bits (layout.c; the tests call it). */
void embi_lay_reset_boxes(embi_box *top);
size_t embi_lay_ifc_pool_bytes(const embi_lay_doc *ld);
/* Drops the paragraph analyses the last layout did not use, or all of them (inline.c; called by the shaped-run sweep). */
void embi_lay_prep_sweep(embi_lay_doc *ld, bool all);
/* Drops the cache entries the layout that just ended did not use (layout.c), or all when all. */
void embi_lay_tcache_sweep(embi_lay_doc *ld, bool all);

/* ------------------------------------------------------------------ line metrics (line.c) */

/* The ascent and descent of an inline box's content area for its font and line-height (CSS 2.1
 * section 10.8), including half-leading. A and D are px above and below the baseline. */
void embi_lay_line_metrics(embi_lctx *c, const embi_style *s, float *a, float *d);
/* The language of a box (its nearest lang attribute, inherited; empty when none). */
embi_str embi_lay_lang(embi_lctx *c, const embi_box *b);

/* The exclusion space (exclusion.c, W3b). embi_excl_band gives the inline interval [*l, *r] inside
 * [cl, cr] left free by the floats that overlap the block band [y, y + h) (h may be 0: then the
 * floats that contain y). embi_excl_next_y gives the lowest bottom of the floats that overlap the
 * band, the next position to try (INFINITY when none). embi_excl_clear_y gives the lowest bottom of
 * the floats on the given sides (-INFINITY when none). embi_excl_max_top and embi_excl_bottom give the
 * highest top and the lowest bottom of any float (-INFINITY when none). */
void embi_excl_init(embi_excl *e);
void embi_excl_free(embi_excl *e);
void embi_excl_add(embi_excl *e, float x0, float y0, float x1, float y1, bool left);
void embi_excl_band(const embi_excl *e, float cl, float cr, float y, float h, float *l, float *r);
float embi_excl_next_y(const embi_excl *e, float y, float h);
float embi_excl_clear_y(const embi_excl *e, uint8_t sides);
float embi_excl_max_top(const embi_excl *e);
float embi_excl_bottom(const embi_excl *e);

/* ------------------------------------------------------------------ case mapping (casemap.c) */

/* The text-transform mapping of one code point (CSS Text 3 section 2.1). transform is an
 * EMBI_KW_* of text-transform: uppercase, lowercase, full-width or full-size-kana. Uses the same
 * Unicode default case tables as the JS String methods (src/js/unicode_case.inc). Writes up to
 * three code points and returns how many. Other keywords return the code point unchanged. */
int embi_lay_map_cp(uint32_t cp, uint16_t transform, uint32_t out[3]);
/* The titlecase mapping of cp for text-transform: capitalize (the ligatures and digraphs whose title form differs from the
 * upper case one: fl gives Fl, dz with caron gives its title form); otherwise the upper case mapping. */
int embi_lay_title_cp(uint32_t cp, uint32_t out[3]);
/* True for a code point that belongs to a word for capitalize (letters, digits, marks, and the
 * apostrophes inside words). Approximates the UAX 29 word boundaries. */
bool embi_lay_word_cp(uint32_t cp);

/* The aspect ratio (width / height) that applies to a box (CSS Sizing 4 section 5): the
 * aspect-ratio property's ratio, or the natural ratio of a replaced box with auto. Returns false
 * when there is none. */
bool embi_lay_ratio(const embi_lctx *c, const embi_box *b, float *ratio);
/* True when the box's ratio relates content boxes (aspect-ratio: auto <ratio>) rather than the boxes box-sizing selects. */
bool embi_lay_ratio_content(const embi_lctx *c, const embi_box *b);

/* ------------------------------------------------------------------ other formatting contexts */

/* Clears the fragments of a box and its descendants (before the box is laid out again). The fragments of
 * the box's own previous layout that nothing attaches (the root fragment has no parent) go to the
 * document's free list, so a pass that is thrown away costs no new fragments. */
void embi_lay_clear_frags(embi_lctx *c, embi_box *b);
/* True when the fragment tree of box b (a plain block-flow box) is the same whatever height it is given,
 * except for its own border-box height: nothing in its subtree reads a definite height of its
 * containing block (no percentage, calc or keyword height, min-height or max-height, no out-of-flow or
 * size-container box), and b itself is a plain block (not a scroll container, a table, a flex or grid
 * container, a replaced box or a centred button, and without an aspect ratio). A flex item or grid item
 * that is stretched in the block axis, or laid out first without a height and then in a row of a given
 * height, can then reuse its first layout and only change its height (W3p). Cached on the box. */
bool embi_lay_height_independent(const embi_lctx *c, embi_box *b);

/* Flex containers (flex.c, W3e). embi_lay_flex lays out the container as a block box whose content
 * is the flex layout: block_pass (block.c) calls embi_flex_content for the content of a flex
 * container, with the content box geometry it has computed. Out: ch is the content height and bl1,
 * bl2 the first and last baselines relative to the border box top. embi_flex_intrinsic gives the
 * min-content and max-content content widths (CSS Flexbox 1 section 9.9). */
typedef struct embi_flex_in {
    float cw;         /* the content box width */
    float ch_def;     /* the content box height when definite, else NaN */
    float minh, maxh; /* the content box min and max heights (maxh infinite when none) */
    float ox, oy;     /* the content box origin in the container's border box */
    /* calc-size() on the height with an auto basis (CSS Values 5 section 10.1): a column container
     * applies the calc-sum to its intrinsic main size before the flexible lengths are resolved (CSS
     * Flexbox 1 section 9.7), so items shrink to the final size. nullptr when there is none. */
    const embi_value *main_expr;
    float main_pad;   /* added to the content size before the calc-sum (border-box: the vertical edges) */
    float main_pct;   /* the percentage base of the calc-sum (NaN when indefinite) */
} embi_flex_in;
typedef struct embi_flex_out {
    float ch, bl1, bl2;
    bool main_calc;   /* the container's main size already has the calc-sum applied (ch is final) */
} embi_flex_out;
void embi_flex_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in,
                       embi_flex_out *out);
void embi_flex_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx);

/* Stubs laid out as block containers until their task lands (see grid.c, table.c). */
void embi_lay_flex(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);
void embi_lay_grid(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);
void embi_lay_table(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out);
/* True for a table box (its own box and wrapper, roles EMBI_TR_TABLE): block_pass lays out its content
 * through embi_table_content, and its auto width is shrink-to-fit (table.c, W3g). */
static inline bool embi_lay_is_table(const embi_box *b)
{
    return b->kind == EMBI_BK_TABLE && b->tr == EMBI_TR_TABLE;
}
/* The content of a table box (table.c, W3g): the captions and the row groups, rows and cells, laid out
 * in the content width in->cw. The content height is out->ch and the first baseline out->bl1. The
 * table's own geometry comes from block_pass. embi_table_intrinsic gives the min-content and
 * max-content content widths (CSS Tables 3 section 3.5). */
void embi_table_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in,
                        embi_flex_out *out);
void embi_table_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* The width: max-content of a table: every column at its maximum, percentages do not widen it. */
float embi_table_max_content_plain(embi_lctx *c, embi_box *b);
/* The sizes the table uses for its own width: like embi_table_intrinsic, but the percentage widths of a
 * table inside a cell count (they do not in its contribution to the cell). */
void embi_table_intrinsic_own(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* True when a table box lies inside a table cell. */
bool embi_table_in_cell(const embi_box *b);
/* Grid containers (grid.c, W3f): the content of a grid container (block_pass calls it with the content
 * box geometry, as for flex) and its intrinsic inline sizes (size.c). The internals are in grid_int.h. */
void embi_grid_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in, embi_flex_out *out);
void embi_grid_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx);
/* Table columns (table_col.c, W3g). embi_table_col_width gives the width property of a column or a
 * column group as px (NaN when auto or a percentage), embi_table_col_pct the percentage as a fraction
 * (NaN when not a percentage), and embi_table_col_span the span attribute (1 when absent). */
float embi_table_col_width(embi_lctx *c, embi_box *col);
float embi_table_col_width_clamped(embi_lctx *c, embi_box *col);
float embi_table_col_pct(embi_lctx *c, embi_box *col);
uint32_t embi_table_col_span(const embi_box *col);
/* The value of an unsigned integer attribute of an HTML element (colspan, rowspan, span), clamped to
 * [1, max] for the span attributes; dflt when absent or not a number. */
uint32_t embi_table_attr_uint(const embi_box *b, const char *name, uint32_t dflt, uint32_t lo,
                              uint32_t hi);
/* Column widths (CSS Tables 3 section 3.5 and 3.4). n columns with minimum and maximum content widths
 * mn and mx, the percentage widths pct (NaN for none) and the fixed flags fixedc (a column with a width
 * from a cell or column element). avail is the width left for the columns after the spacing. Writes the
 * widths to out. Auto layout: percentage columns take their share first, the rest is distributed
 * between min and max in proportion to max - min, and extra width goes to the non-fixed columns in
 * proportion to their max. Fixed layout: px widths are kept (spec), percentages take their share, and the
 * auto columns share what is left. */
void embi_table_distribute_auto(uint32_t n, const float *mn, const float *mx, const float *pct,
                                const uint8_t *fixedc, float avail, float *out);
void embi_table_distribute_fixed(uint32_t n, const float *spec, const float *pct, const float *pe,
                                 float avail, float *out);
/* ------------------------------------------------------------------ positioned layout (position.c, W3d) */

/* Records the static position of an out-of-flow child k of sbox, which is laid out with the
 * containing block found from the box tree. sx is the static position of the margin edge at the
 * inline start (the right edge in rtl) and sy the block start, both relative to sbox's border box.
 * cb, when not nullptr, is the containing block rectangle (x0, y0, x1, y1) relative to sbox's border
 * box instead (grid areas, W3f). Nothing is laid out here: embi_lay_position_finish places the
 * records after the whole tree is laid out. A later record of the same box replaces the earlier
 * ones (a container laid out again). Flex and grid containers call this for their abspos children
 * (W3e, W3f). */
void embi_lay_abspos_at(embi_lctx *c, embi_box *k, embi_box *sbox, float sx, float sy,
                        const float *cb);
/* As embi_lay_abspos_at for an out-of-flow child of a flex container (W3e): its static position is the
 * sole-item position in the container's content box cont (x0, y0, x1, y1 relative to sbox's border
 * box), at the fraction frac[axis] of the free space (0 start, 0.5 center, 1 end) on each axis, from
 * the far edge when flip[axis]. Axis 0 is x and axis 1 is y. */
void embi_lay_abspos_flex(embi_lctx *c, embi_box *k, embi_box *sbox, const float cont[4],
                          const float frac[2], const bool flip[2]);
/* Marks the record embi_lay_abspos_at just made for k: its static position sx is the box's end edge in the static
 * container's direction (the left edge in rtl, the right one in ltr), for a placeholder in text of the opposite
 * direction (CSS Position 3 section 3.4 leaves it to the UA; Chromium picks the edge by the placeholder's
 * direction). */
void embi_lay_abspos_flip(embi_lctx *c, const embi_box *k);
/* Records an out-of-flow child of a grid container that is its containing block (W3f): cb is the grid
 * area (x0, y0, x1, y1 relative to sbox's border box), with NaN for an edge that is the padding box edge
 * (filled in from the final fragment by position.c). With both insets auto the box sits at the start of
 * that containing block. */
void embi_lay_abspos_grid(embi_lctx *c, embi_box *k, embi_box *sbox, const float cb[4]);
/* Records the static positions of the out-of-flow children of the inline content of block b, at
 * the inline start sx and the block start sy (relative to b's border box). */
void embi_lay_abspos_ifc(embi_lctx *c, embi_box *b, float sx, float sy);
/* Places the recorded out-of-flow boxes, applies the relative offsets, stores the sticky data and
 * sets the stacking flags of the fragment tree rooted at icb (the final pass of a layout). */
void embi_lay_position_finish(embi_lctx *c, embi_frag *icb);
/* The containing block box of an out-of-flow box (nullptr: the initial containing block). */
const embi_box *embi_lay_abspos_cb(const embi_lctx *c, const embi_box *k);

/* ---- W6b: box-tree surgery and incremental update (box.c, incr.c) ---- */
enum { EMBI_LP_AUTO = 0, EMBI_LP_FULL, EMBI_LP_KEEP };
bool embi_lay_rebuild_element(embi_lctx *c, embi_element *el, embi_box *old);
/* Plans the next layout of ld from the style engine's delta: sets ld->plan and does the surgery. Returns the kind of
 * update: EMBI_LU_NONE or EMBI_LU_PAINT (the fragments stay as they are: no layout run), EMBI_LU_SURGERY (the tree was
 * patched: run the layout on it), EMBI_LU_FULL (rebuild). */
int embi_lay_incr_plan(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc);
/* Marks the state so that the next update rebuilds the tree. */
void embi_lay_incr_reset(embi_lay_doc *ld);
/* Called by layout_run after it built the tree again (and laid it out): bookkeeping for the next incremental plan. */
void embi_lay_incr_built(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc);

/* How a block box was laid out, to lay it out again on its own (incr.c). */
typedef struct embi_prec {
    embi_cs cs;            /* excl is nullptr here: see rects */
    embi_lres r;           /* r.frag is nullptr */
    const embi_excl_rect *rects; /* the exclusion rectangles the box was laid out against (in the arena ra) */
    uint32_t nrects;
    bool had_excl;
    bool abs;              /* an out-of-flow box was registered while it was laid out (its subtree is not reusable) */
} embi_prec;

/* Called by embi_lay_box (block.c) around every dispatch. */
bool embi_lay_incr_chain_ok(const embi_lctx *c, const embi_box *b, const embi_cs *cs);
void embi_lay_incr_record(embi_lctx *c, embi_box *b, const embi_cs *cs, const embi_lres *r, uint32_t nrects0,
                          uint32_t pos0);
/* lay_child (block.c): the child's fragment from the last layout stands for its layout (partial relayout only). */
bool embi_lay_incr_reuse(embi_lctx *c, embi_box *k, const embi_cs *cs, embi_lres *r);
/* position.c: the number of out-of-flow records of this layout, and dropping them. */
uint32_t embi_lay_pos_count(const embi_lctx *c);
/* W6e (position.c): recompute the stacking flags of a block-level box's fragment in place (opacity crossing 1). */
bool embi_lay_restack(const embi_lctx *c, embi_box *b);
void embi_lay_pos_discard(embi_lctx *c);
void embi_lay_incr_stale(embi_lay_doc *ld, embi_box *b);
void embi_lay_incr_forget(embi_lay_doc *ld, const embi_box *b);
/* Resets the records (the tree is built or laid out from scratch). */
void embi_lay_incr_records_reset(embi_lay_doc *ld);
void embi_lay_incr_records_free(embi_lay_doc *ld);
/* Lays out again the boxes the last surgery built, from the nearest enclosing box that can be laid out on its own, and
 * splices the new fragments in. False when it cannot (the caller lays the whole tree out). */
bool embi_lay_partial(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc);
/* A display value that makes a list item (list-item, and the inline and flow-root list-item forms). */
static inline bool embi_lay_disp_is_li(uint16_t kw)
{
    return kw == EMBI_KW_list_item || kw == EMBI_KW__internal_inline_list_item ||
           kw == EMBI_KW__internal_flow_root_list_item || kw == EMBI_KW__internal_inline_flow_root_list_item;
}

/* Test switch: enables or disables the partial relayout (overrides EMBER_NO_PARTIAL_RELAYOUT). */
void embi_lay_set_partial_enabled(bool on);
/* position.c: the final pass over a detached fragment subtree, and the offset a relative box gives its fragment. */
void embi_lay_prep_subtree(embi_lctx *c, embi_frag *f);
void embi_lay_frag_shift(const embi_lctx *c, const embi_box *b, float *dx, float *dy);
/* block.c: puts the fragment tree rooted at f on the free lists of the document. */
void embi_lay_release_frags(embi_lay_doc *ld, embi_frag *f);

typedef struct embi_lay_snap embi_lay_snap;
embi_lay_snap *embi_lay_snapshot(const embi_frag *root);
bool embi_lay_snapshot_equal(const embi_lay_snap *a, const embi_lay_snap *b);
void embi_lay_snapshot_free(embi_lay_snap *s);
