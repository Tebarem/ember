/*
 * Incremental layout (W6b): what a document's layout does with the change set the style
 * engine recorded since the last layout (embi_style_take_delta).
 *
 *   - Nothing changed that layout or paint reads: the fragments stay as they are (EMBI_LU_NONE).
 *   - Only paint properties changed (color, background, opacity, transform ...): the boxes of the changed elements get
 *     their new style pointers, the fragments stay, and the paint module is told which elements to repaint
 *     (EMBI_LU_PAINT, embi_layout_take_paint_dirty).
 *   - Something layout reads changed (a layout property, children, text, an attribute the box build reads): the box
 *     subtree of the nearest enclosing block container that can be rebuilt on its own is built again and spliced in (box
 *     surgery), and then the fragments are patched (EMBI_LU_SURGERY):
 *       * Partial relayout (embi_lay_partial). Every plain block laid out by block flow on a chain of such blocks (fill-width
 *         constraint, size independent of the content: embi_lay_incr_chain_ok) is recorded with the constraint space and
 *         result of its layout (embi_prec). The nearest recorded ancestor R of a rebuilt box is laid out again on its own
 *         with the recorded constraint space, the children it had that did not change are reused as they are (their fragments
 *         move, nothing inside them is touched: embi_lay_incr_reuse), and if the new fragment of R has the size, margins,
 *         baselines and overflow of the old one nothing outside R can have changed: the new fragment replaces the old one.
 *         When R's size changed, the next recorded ancestor is tried (it reuses R's siblings, so what a height change costs
 *         is the flow of the ancestors' children, not their layout). Boxes with a float, an out-of-flow box, a sticky box, a
 *         flex, grid or table ancestor, a shrink-to-fit ancestor, a container query ancestor or counters are not eligible.
 *       * Otherwise the layout runs over the whole patched tree (the layout of W3p's kept tree).
 *   - Anything else (the first layout, a lost change set, a viewport change, ::first-letter in the tree, a change the surgery
 *     cannot place): the tree is built again (EMBI_LU_FULL).
 *
 * Within one run over a tree (a full build, or a surgery that lays out the whole kept tree) the layout cache (lcache.c)
 * gives a box laid out again with the same constraint space its earlier result back. Flex and grid containers lay their
 * items out several times (measures, then the final pass) and nested ones multiply that, so it is what keeps those runs
 * linear; the partial relayout above does not use it (it moves fragments in place). The cache lives for one run only: a
 * kept box's fragment from the last run is never given back by it. EMBER_NO_LAYOUT_CACHE=1 turns it off.
 *
 * The result is exactly what a full build gives: with EMBER_VERIFY_INCREMENTAL=1 every incremental update, and every full
 * build the layout cache gave something to, is followed by a full build without the cache in a throwaway layout state of
 * the same document and the fragment trees are compared (layout.c).
 */
#include "layout_int.h"

#include "css/props.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* The old styles of the grave (layout caches are keyed by their addresses), the surgeries and the growth of the box
 * arena all end in a full build when they pass these bounds. The grave's bound grows with the number of boxes: a frame that
 * restyles every element of a big page (an animation on 10k elements) leaves one old style per element, which the paint-only
 * update releases again; a fixed bound sent such a page to a full build of the box tree on every frame. */
#define EMBI_INCR_GRAVE_MAX 2048u
#define EMBI_INCR_SURGERY_MAX 4096u /* W6e: was 512; the arena checks below bound the memory, and a layout animation does one surgery a frame */
/* The records keep the exclusion rectangles of the box they describe; past this many floats the box is not recorded. */
#define EMBI_PREC_RECTS_MAX 64u

static bool display_none(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_display);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none;
}

static embi_element *parent_element(const embi_element *el)
{
    const embi_node *p = el->base.parent;
    return p != nullptr && p->type == EMBI_NODE_ELEMENT ? (embi_element *)(uintptr_t)p : nullptr;
}

static size_t arena_used_bytes(const embi_arena *a)
{
    size_t total = 0;
    for (const embi_arena_block *blk = a->first; blk != nullptr; blk = blk->next) {
        total += blk->used;
    }
    return total;
}

/* ------------------------------------------------------------------ style pointer refresh */

/* The boxes of element E below b that hold E's old style take the new one: E's own boxes (anonymous wrappers, the
 * text runs that are its direct content). The boxes of other elements are not entered. */
static void refresh_walk(embi_box *b, const embi_element *E, const embi_style *oldst, const embi_style *newst)
{
    if (embi_box_is_text(b)) {
        return;
    }
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (embi_box_is_text(k)) {
            if (k->style == oldst && (k->el == nullptr || k->el == E)) {
                k->style = newst;
            }
            continue;
        }
        if (k->el != nullptr && k->el != E) {
            continue;
        }
        if (k->style == oldst) {
            k->style = newst;
        }
        refresh_walk(k, E, oldst, newst);
    }
}

/* CF1: the boxes of the pseudo-elements of E below b take the pseudo-elements' current styles (their change was a paint
 * or refresh change: the same boxes with the new style). The marker of a list item is a text-kind box, and it may sit in
 * the box of a descendant (the first block of the item), so the walk goes through the boxes of other elements too. */
static void refresh_pseudos(embi_style_engine *se, embi_box *b, embi_element *E)
{
    if (embi_box_is_text(b)) {
        return;
    }
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (embi_box_is_text(k) && !(k->kind == EMBI_BK_MARKER && k->el == E)) {
            continue;
        }
        if (k->pe != 0 && k->el == E) {
            const embi_style *ns = embi_style_generated(se, E, (embi_sel_pseudo)k->pe);
            if (ns != nullptr && k->style != ns) {
                const embi_style *os = k->style;
                k->style = ns;
                refresh_walk(k, E, os, ns);
            }
            continue;
        }
        refresh_pseudos(se, k, E);
    }
}

/* False when the element's boxes cannot be refreshed in place (the caller does more than that). */
static bool refresh_element(embi_style_engine *se, const embi_style_change *ch)
{
    embi_element *E = ch->el;
    embi_box *B = (embi_box *)E->box;
    if (B == nullptr) {
        return true; /* no box: nothing holds the style */
    }
    const embi_style *ns = (const embi_style *)E->style;
    if (embi_box_is_text(B) || B->el != E || ns == nullptr || (B->flags2 & (EMBI_BF2_NOSTART | EMBI_BF2_NOEND)) != 0) {
        return false;
    }
    if (B->style != ch->old_style && B->style != ns) {
        return false; /* the box holds something else than what the delta says: do not guess */
    }
    B->style = ns;
    refresh_walk(B, E, ch->old_style, ns);
    if (ch->pseudo) {
        refresh_pseudos(se, B, E);
    }
    return true;
}

/* ------------------------------------------------------------------ the layout context */

/* A layout context for the work outside layout_run (surgery builds boxes, the partial relayout lays them out): what
 * layout_run sets up, without the icb. */
static void ctx_init(embi_lctx *c, embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc)
{
    memset(c, 0, sizeof *c);
    c->eng = e;
    c->ld = ld;
    c->style = e->style;
    c->text = e->text;
    c->fonts = embi_tshim_fonts(e->text);
    c->initial = embi_style_initial(e->style);
    c->vw = e->vw;
    c->vh = e->vh;
    c->scrollbar = e->scrollbar;
    c->image_size = e->image_size;
    c->image_user = e->image_user;
    c->root_px = 16.0f;
    c->max_depth = EMBI_LAY_MAX_DEPTH;
    c->no_pseudo = !embi_style_has_generated(e->style, doc);
    embi_hashmap_init(&c->tmodels);
    embi_element *root = embi_document_element(doc);
    if (root != nullptr) {
        const embi_style *rs = embi_style_get(e->style, root, EMBI_PE_NONE);
        if (rs != nullptr) {
            c->root_style = rs;
            const embi_value *v = embi_style_value(rs, EMBI_PROP_font_size);
            if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
                c->root_px = (float)v->num;
            }
        }
    }
}

static void ctx_free(embi_lctx *c)
{
    embi_ctr_free(&c->ctr);
    embi_table_models_free(c);
}

/* ------------------------------------------------------------------ surgery */

typedef struct sg {
    embi_layout_engine *e;
    embi_lay_doc *ld;
    embi_document *doc;
    embi_lctx c;
    bool c_ready;
    embi_hashmap done; /* the elements whose boxes were rebuilt (covers everything below them) */
    uint32_t removed;  /* boxes of the old subtrees that were replaced */
} sg;

static void sg_init_ctx(sg *s)
{
    if (!s->c_ready) {
        ctx_init(&s->c, s->e, s->ld, s->doc);
        s->c_ready = true;
    }
}

static void sg_free(sg *s)
{
    if (s->c_ready) {
        ctx_free(&s->c);
    }
    embi_hashmap_free(&s->done);
}

static bool covered(const sg *s, const embi_element *a)
{
    if (embi_hashmap_len(&s->done) == 0) {
        return false;
    }
    uint64_t v = 0;
    for (const embi_element *x = a; x != nullptr; x = parent_element(x)) {
        if (embi_hashmap_get(&s->done, (uint64_t)(uintptr_t)x, &v)) {
            return true;
        }
    }
    return false;
}

static uint32_t element_depth(const embi_element *el)
{
    uint32_t d = 0;
    for (const embi_element *x = parent_element(el); x != nullptr; x = parent_element(x)) {
        d++;
    }
    return d;
}

static uint32_t count_box_tree(const embi_box *b)
{
    uint32_t n = 1;
    if (!embi_box_is_text(b)) {
        for (const embi_box *k = b->first; k != nullptr; k = k->next) {
            n += count_box_tree(k);
        }
    }
    return n;
}

/* Rebuilds the boxes of the nearest enclosing element of a (a included) that can be rebuilt on its own. False when
 * there is none. */
static bool rebuild_for(sg *s, embi_element *a, uint32_t *boxes)
{
    if ((a->base.flags & EMBI_NF_CONNECTED) == 0) {
        return true; /* left the document: its boxes went with the box they were in */
    }
    if (covered(s, a)) {
        return true;
    }
    if (a->box == nullptr) {
        const embi_style *st = (const embi_style *)a->style;
        if (st == nullptr || display_none(st)) {
            return true; /* generates nothing, and did not before */
        }
    }
    for (embi_element *el = a; el != nullptr; el = parent_element(el)) {
        if (covered(s, el)) {
            return true;
        }
        embi_box *B = (embi_box *)el->box;
        if (B == nullptr) {
            const embi_style *st = (const embi_style *)el->style;
            if (st != nullptr && display_none(st)) {
                return true; /* inside a subtree that generates no boxes */
            }
            continue; /* display: contents, or not yet boxed: the parent's boxes hold it */
        }
        sg_init_ctx(s);
        s->c.bdepth = element_depth(el);
        uint32_t old_n = count_box_tree(B);
        /* CF1: a recorded box whose style stays: the new box can be laid out again where it was (embi_lay_partial) */
        embi_reb_old ro = {nullptr, nullptr, 0};
        if (B->frag != nullptr && B->style == (const embi_style *)el->style && embi_hashmap_len(&s->ld->prmap) != 0 &&
            embi_hashmap_get(&s->ld->prmap, (uint64_t)(uintptr_t)B, &ro.rec)) {
            ro.frag = B->frag;
        }
        if (!embi_lay_rebuild_element(&s->c, el, B)) {
            continue;
        }
        s->removed += old_n;
        embi_box *nb = (embi_box *)el->box;
        /* what the boxes above cache about their content */
        for (embi_box *p = nb->parent; p != nullptr; p = p->parent) {
            p->flags &= (uint16_t)~EMBI_BF_INTR;
            p->flags2 &= (uint16_t)~(EMBI_BF2_HIKNOWN | EMBI_BF2_HINDEP);
            p->tflags &= (uint8_t)~EMBI_TF_FILL;
        }
        embi_hashmap_put(&s->done, (uint64_t)(uintptr_t)el, 1);
        embi_vec_push(&s->ld->reb, el);
        ro.box = nb;
        embi_vec_push(&s->ld->rebo, ro);
        *boxes += count_box_tree(nb);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ records of the partial relayout */

void embi_lay_incr_records_reset(embi_lay_doc *ld)
{
    embi_hashmap_clear(&ld->prmap);
    embi_vec_clear(&ld->precs);
    embi_arena_reset(&ld->ra);
}

void embi_lay_incr_records_free(embi_lay_doc *ld)
{
    embi_hashmap_free(&ld->prmap);
    embi_vec_free(&ld->precs);
    embi_arena_free(&ld->ra);
    embi_hashmap_free(&ld->reusedmap);
    embi_hashmap_free(&ld->rebanc);
    embi_vec_free(&ld->reb);
    embi_vec_free(&ld->rebo);
}

void embi_lay_incr_forget(embi_lay_doc *ld, const embi_box *b)
{
    if (embi_hashmap_len(&ld->prmap) != 0) {
        embi_hashmap_remove(&ld->prmap, (uint64_t)(uintptr_t)b);
    }
}

/* True when b, laid out by block flow with cs, is recorded: an in-flow box whose size is a function of its containing
 * block's size and its own content, on a chain of plain blocks. CF1: a flex or grid container qualifies as well (it can
 * be laid out again on its own), but only a plain block continues the chain to its children (embi_lay_box). */
bool embi_lay_incr_chain_ok(const embi_lctx *c, const embi_box *b, const embi_cs *cs)
{
    if (c->ld->scratch || (b->kind != EMBI_BK_BLOCK && b->kind != EMBI_BK_FLEX && b->kind != EMBI_BK_GRID) ||
        cs->fit != EMBI_FIT_FILL) {
        return false;
    }
    if ((b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC | EMBI_BF_REPLACED |
                     EMBI_BF_BLOCKIFIED | EMBI_BF_CENTER)) != 0 ||
        (b->flags2 & EMBI_BF2_CONTROL) != 0 || b->tr != EMBI_TR_NONE) {
        return false;
    }
    if (c->force_box != nullptr || c->gsub != nullptr || c->edge_box != nullptr || c->ncq != 0) {
        return false;
    }
    const embi_style *s = (b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr ? c->initial : b->style;
    return !embi_lay_ssum_of(c, s)->incr_dep;
}

void embi_lay_incr_record(embi_lctx *c, embi_box *b, const embi_cs *cs, const embi_lres *r, uint32_t nrects0,
                          uint32_t pos0)
{
    embi_lay_doc *ld = c->ld;
    uint32_t n1 = cs->excl != nullptr ? cs->excl->rects.len : 0;
    if (n1 != nrects0 || nrects0 > EMBI_PREC_RECTS_MAX) {
        embi_lay_incr_forget(ld, b); /* the box added floats to its formatting context, or there are too many to keep */
        return;
    }
    embi_prec p;
    memset(&p, 0, sizeof p);
    p.cs = *cs;
    p.cs.excl = nullptr;
    p.r = *r;
    p.r.frag = nullptr;
    p.had_excl = cs->excl != nullptr;
    p.nrects = nrects0;
    p.abs = embi_lay_pos_count(c) != pos0;
    if (nrects0 > 0) {
        embi_excl_rect *q = embi_arena_alloc(&ld->ra, (size_t)nrects0 * sizeof *q, alignof(embi_excl_rect));
        memcpy(q, cs->excl->rects.data, (size_t)nrects0 * sizeof *q);
        p.rects = q;
    }
    uint64_t key = (uint64_t)(uintptr_t)b, v = 0;
    if (embi_hashmap_get(&ld->prmap, key, &v)) {
        ld->precs.data[v - 1] = p;
    } else {
        embi_vec_push(&ld->precs, p);
        embi_hashmap_put(&ld->prmap, key, ld->precs.len);
    }
}

static bool same_f(float a, float b)
{
    return a == b || (isnan(a) && isnan(b));
}

/* The child k of a block that is laid out again: when it was laid out before for the same constraint, its fragment is
 * the result (lay_child, block.c). Only while a partial relayout runs (ld->reuse_on). */
bool embi_lay_incr_reuse(embi_lctx *c, embi_box *k, const embi_cs *cs, embi_lres *r)
{
    embi_lay_doc *ld = c->ld;
    if (!ld->reuse_on || k->frag == nullptr || embi_box_is_text(k)) {
        return false;
    }
    uint64_t v = 0;
    if (!embi_hashmap_get(&ld->prmap, (uint64_t)(uintptr_t)k, &v)) {
        return false;
    }
    if (embi_hashmap_len(&ld->rebanc) != 0 && embi_hashmap_get(&ld->rebanc, (uint64_t)(uintptr_t)k, &v)) {
        return false; /* a box was built again somewhere below it: its old fragment holds the old box */
    }
    embi_prec p = ld->precs.data[v - 1];
    if (p.abs) {
        return false;
    }
    const embi_cs *q = &p.cs;
    if (!same_f(q->avail_w, cs->avail_w) || !same_f(q->cb_w, cs->cb_w) || !same_f(q->cb_h, cs->cb_h) ||
        q->fit != cs->fit || q->mg_ovr != cs->mg_ovr || !same_f(q->ml_ovr, cs->ml_ovr) || !same_f(q->mr_ovr, cs->mr_ovr)) {
        return false;
    }
    uint32_t n = cs->excl != nullptr ? cs->excl->rects.len : 0;
    if (n != p.nrects) {
        return false;
    }
    if (n != 0) {
        /* with floats about, the box's position in the formatting context matters */
        if (!same_f(q->bfc_x, cs->bfc_x) || !same_f(q->bfc_y, cs->bfc_y) ||
            memcmp(p.rects, cs->excl->rects.data, (size_t)n * sizeof *p.rects) != 0) {
            return false;
        }
    }
    *r = p.r;
    r->frag = k->frag;
    embi_hashmap_put(&ld->reusedmap, (uint64_t)(uintptr_t)k->frag, 1);
    ld->reused++;
    return true;
}

/* ------------------------------------------------------------------ planning */

void embi_lay_incr_reset(embi_lay_doc *ld)
{
    ld->force_full = true;
}

void embi_lay_incr_built(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc)
{
    if (ld->scratch) {
        return;
    }
    ld->ba_base = arena_used_bytes(&ld->ba);
    ld->nb_base = ld->nb_cur = ld->root_box != nullptr ? count_box_tree(ld->root_box) : 0u;
    ld->tents_base = ld->tents.len;
    ld->fa_base = arena_used_bytes(&ld->fa);
    ld->surgeries = 0;
    ld->partials = 0;
    /* the layout caches no longer hold the old styles' addresses */
    embi_style_release_grave(e->style, doc);
}

/* The fragments are about to be rebuilt (or already are): the paint module's cache is invalid (fserial moves in the
 * layout run) and its list of dirty elements is moot. */
static void pdirty_full(embi_lay_doc *ld)
{
    ld->pover = false;
    embi_vec_clear(&ld->pdirty);
}

/* True when a transform property starting or ending on B (which makes B the containing block of its absolutely and fixed
 * positioned descendants) changes where such a descendant is placed: a fixed one always, an absolute one unless B is positioned
 * already (it was the containing block of the ones not inside a nearer positioned box). */
static bool xf_moves_abspos(embi_lctx *c, const embi_box *b, bool b_positioned)
{
    if (embi_box_is_text(b)) {
        return false;
    }
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (embi_box_is_text(k)) {
            continue;
        }
        if ((k->flags & EMBI_BF_ABSPOS) != 0 &&
            (!b_positioned || k->style == nullptr || embi_lay_ssum_of(c, k->style)->pos == EMBI_KW_fixed)) {
            return true;
        }
        if (xf_moves_abspos(c, k, b_positioned)) {
            return true;
        }
    }
    return false;
}

typedef struct inplace {
    embi_layout_engine *e;
    embi_lay_doc *ld;
    embi_document *doc;
    embi_lctx c;
    bool ready;
} inplace;

static embi_lctx *inplace_ctx(inplace *ip)
{
    if (!ip->ready) {
        ctx_init(&ip->c, ip->e, ip->ld, ip->doc);
        ip->ready = true;
    }
    return &ip->c;
}

/* W6e: a change the style engine classed LAYOUT only because opacity crossed 1, or a transform property crossed none (the
 * element has a box of its own, and for a transform no out-of-flow box below it changes its containing block): the box is
 * kept and the stacking and containing-block flags of its fragment are recomputed in place. The common case is a fade or a
 * slide of a list of items; rebuilding the boxes of an item is not possible when it takes part in the counters (every list
 * item does). */
static bool in_place_change(inplace *ip, const embi_style_change *ch)
{
    const embi_element *E = ch->el;
    const embi_box *B = (const embi_box *)E->box;
    const embi_style *ns = (const embi_style *)E->style;
    if (B == nullptr || ns == nullptr || embi_box_is_text(B) || B->el != E || B->pe != 0) {
        return false;
    }
    if (ch->pseudo) {
        return false; /* CF1: a layout change of a pseudo-element: its boxes are built again */
    }
    if (embi_style_classify_in_place(ch->old_style, ns, EMBI_INPLACE_OPACITY) <= EMBI_SCH_PAINT) {
        return true;
    }
    if (embi_style_classify_in_place(ch->old_style, ns, EMBI_INPLACE_OPACITY | EMBI_INPLACE_TRANSFORM) > EMBI_SCH_PAINT) {
        return false;
    }
    embi_lctx *c = inplace_ctx(ip);
    uint16_t pos = embi_lay_ssum_of(c, ns)->pos;
    bool positioned = pos == EMBI_KW_relative || pos == EMBI_KW_absolute || pos == EMBI_KW_fixed || pos == EMBI_KW_sticky;
    return !xf_moves_abspos(c, B, positioned);
}

static int incr_plan(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc, inplace *ip)
{
    ld->plan = EMBI_LP_FULL;
    if (ld->scratch) {
        return EMBI_LU_FULL; /* a throwaway state: no change set, always a full build */
    }
    if (!ld->tracking) {
        embi_style_track(e->style, doc, true);
        ld->tracking = true;
    }
    embi_vec_clear(&ld->reb);
    embi_vec_clear(&ld->rebo);
    ld->abs_gone = false;
    embi_style_delta d;
    embi_style_take_delta(e->style, doc, &d);

    static int off = -1;
    if (off < 0) {
        off = SDL_getenv("EMBER_NO_INCREMENTAL_LAYOUT") != nullptr ? 1 : 0; /* a switch for bisecting a layout difference */
    }
    bool full = off == 1 || d.lost || ld->root_box == nullptr || ld->doc != doc || ld->icb == nullptr || ld->force_full ||
                ld->root_box->el != embi_document_element(doc) || /* the root element came or went */
                d.from_epoch != ld->t_epoch || ld->t_vw != e->vw || ld->t_vh != e->vh || ld->t_sb != e->scrollbar ||
                ld->t_depth != EMBI_LAY_MAX_DEPTH ||
                embi_style_grave_count(e->style, doc) > EMBI_INCR_GRAVE_MAX + ld->nb_base || ld->surgeries >= EMBI_INCR_SURGERY_MAX ||
                arena_used_bytes(&ld->ba) > 2u * ld->ba_base + (1u << 20) || ld->tents.len > 4u * ld->tents_base + 1024u;
    /* The embedder's image sizes can change without any DOM change (see embi_layout_set_images_stable). */
    if (!full && ld->has_images && !e->images_stable) {
        full = true;
    }
    if (full) {
        ld->force_full = false;
        pdirty_full(ld);
        return EMBI_LU_FULL;
    }
    if (d.nchanges == 0 && d.norigins == 0) {
        ld->t_epoch = d.to_epoch;
        return EMBI_LU_NONE;
    }
    /* what the caches keyed by DOM objects hold may be stale now */
    embi_hashmap_clear(&ld->langs);
    memset(ld->lfront, 0, sizeof ld->lfront);
    /* the caches by style pointer: a restyle frees styles and a new one can take the address of an old one */
    if (ld->stab != nullptr) {
        memset(ld->stab, 0, (size_t)ld->stab_n * sizeof *ld->stab);
    }
    ld->stab_cnt = 0;
    embi_hashmap_clear(&ld->tkeys);
    embi_arena_reset(&ld->ta);
    embi_hashmap_clear(&ld->ecache);
    memset(ld->efront, 0, sizeof ld->efront);
    memset(ld->tkfront, 0, sizeof ld->tkfront);

    bool layout = false;
    for (uint32_t i = 0; i < d.norigins && !layout; i++) {
        layout = (d.origins[i].bits & (uint8_t)~EMBI_ORIG_PAINT) != 0;
    }
    for (uint32_t i = 0; i < d.nchanges && !layout; i++) {
        layout = d.changes[i].cls == EMBI_SCH_LAYOUT && !in_place_change(ip, &d.changes[i]);
    }
    if (!layout && ld->has_contents) {
        layout = true; /* the text of a display: contents element is not found from its own box */
    }
    /* Counters and ::first-letter: the tree built again keeps their state right; a surgery does not (the rebuilt subtree
     * is checked for counters in embi_lay_rebuild_element, and the ::first-letter boxes of a descendant block reach
     * into the next ones). */
    if (layout && (ld->has_fl || ld->has_contents)) {
        pdirty_full(ld);
        return EMBI_LU_FULL;
    }
    /* the style pointers of the boxes of the elements whose paint or nothing changed */
    bool restack = false;   /* some box needs a layout over the kept tree to get its stacking flag right */
    bool restacked = false; /* some box changed its stacking flag, in place or not */
    for (uint32_t i = 0; i < d.nchanges; i++) {
        const embi_style_change *ch = &d.changes[i];
        bool stack = ch->cls == EMBI_SCH_LAYOUT && in_place_change(ip, ch);
        if (ch->cls == EMBI_SCH_LAYOUT && !stack) {
            continue;
        }
        if (!refresh_element(e->style, ch)) {
            pdirty_full(ld);
            return EMBI_LU_FULL;
        }
        if (stack) {
            /* W6e: the element crossed opacity 1, which flips the stacking-context flag of its fragment. The box is kept
             * (its style pointer is refreshed above) and the flag is recomputed in place; the picture needs a full repaint
             * (the paint cache holds the old context tree). When the fragment cannot be patched, the kept tree is laid out
             * again below. */
            ((embi_box *)(uintptr_t)ch->el->box)->flags2 &= (uint16_t)~EMBI_BF2_PPFLAGS0;
            if (embi_lay_ssum_of(inplace_ctx(ip), (const embi_style *)ch->el->style)->xf) {
                ld->has_xf = true; /* hit testing cannot prune by scrollable overflow then (hit.c) */
            }
            if (!embi_lay_restack(inplace_ctx(ip), (embi_box *)(uintptr_t)ch->el->box)) {
                restack = true;
            }
            restacked = true;
        } else if (ch->cls == EMBI_SCH_PAINT && !ld->pover) {
            embi_vec_push(&ld->pdirty, ch->el);
        }
    }
    /* CF1: an element whose paint-only attribute changed is painted again (its boxes and fragments stay) */
    for (uint32_t i = 0; i < d.norigins && !ld->pover; i++) {
        if (d.origins[i].bits == EMBI_ORIG_PAINT) {
            embi_vec_push(&ld->pdirty, d.origins[i].el);
        }
    }
    if (restacked && !restack) {
        ld->pover = true; /* the stacking contexts changed: the paint cache cannot patch that */
        embi_vec_clear(&ld->pdirty);
    }
    if (!layout && restack) {
        /* No box is built: the kept tree is laid out again (the partial relayout has nothing to start from). */
        pdirty_full(ld);
        ld->surgeries++;
        ld->rebuilt_boxes = 0;
        ld->t_epoch = d.to_epoch;
        ld->abs_gone = true;
        ld->plan = EMBI_LP_KEEP;
        return EMBI_LU_SURGERY;
    }
    if (!layout) {
        if (ld->pdirty.len > 4096u) {
            ld->pover = true;
            embi_vec_clear(&ld->pdirty);
        }
        ld->t_epoch = d.to_epoch;
        return EMBI_LU_PAINT;
    }

    sg s;
    memset(&s, 0, sizeof s);
    s.e = e;
    s.ld = ld;
    s.doc = doc;
    embi_hashmap_init(&s.done);
    uint32_t boxes = 0;
    bool ok = true;
    for (uint32_t i = 0; i < d.nchanges && ok; i++) {
        if (d.changes[i].cls == EMBI_SCH_LAYOUT && !in_place_change(ip, &d.changes[i])) {
            ok = rebuild_for(&s, d.changes[i].el, &boxes);
        }
    }
    for (uint32_t i = 0; i < d.norigins && ok; i++) {
        if (d.origins[i].bits != EMBI_ORIG_PAINT) {
            ok = rebuild_for(&s, d.origins[i].el, &boxes);
        }
    }
    uint32_t removed = s.removed;
    sg_free(&s);
    if (!ok) {
        pdirty_full(ld);
        return EMBI_LU_FULL;
    }
    ld->nb_cur = ld->nb_cur + boxes > removed ? ld->nb_cur + boxes - removed : 0u;
    if (removed > 64u && ld->nb_cur * 2u < ld->nb_base) {
        pdirty_full(ld);
        return EMBI_LU_FULL; /* the tree shrank to under half: built again, which frees the boxes and runs of the rest */
    }
    pdirty_full(ld);
    if (restack) {
        ld->abs_gone = true; /* the flags of the fragments that were not rebuilt are recomputed by a layout of the whole tree */
    }
    ld->surgeries++;
    ld->builds++; /* embi_layout_tree_builds counts the trees built or patched */
    ld->rebuilt_boxes = boxes;
    ld->t_epoch = d.to_epoch;
    ld->plan = EMBI_LP_KEEP;
    return EMBI_LU_SURGERY;
}

int embi_lay_incr_plan(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc)
{
    inplace ip = {e, ld, doc, {0}, false};
    int r = incr_plan(e, ld, doc, &ip);
    if (ip.ready) {
        ctx_free(&ip.c);
    }
    return r;
}

/* ------------------------------------------------------------------ partial relayout */

enum {
    PR_OK = 0,
    PR_SKIP,     /* this box cannot be laid out on its own: try its parent (costs nothing) */
    PR_MISMATCH, /* laid out again, but its size or overflow changed: try its parent */
    PR_HARD,     /* nothing can be patched: lay the whole tree out */
};

typedef struct pst {
    embi_layout_engine *e;
    embi_lay_doc *ld;
    embi_document *doc;
    embi_lctx c;
    embi_hashmap fresh; /* the boxes that were laid out again and spliced in */
    uint32_t failed;
} pst;

static bool lres_same(const embi_lres *a, const embi_lres *b)
{
    return same_f(a->ml, b->ml) && same_f(a->mr, b->mr) && same_f(a->mt, b->mt) && same_f(a->mb, b->mb) &&
           a->selfc == b->selfc && same_f(a->mt_s.pos, b->mt_s.pos) && same_f(a->mt_s.neg, b->mt_s.neg) &&
           same_f(a->mb_s.pos, b->mb_s.pos) && same_f(a->mb_s.neg, b->mb_s.neg) && same_f(a->top_s.pos, b->top_s.pos) &&
           same_f(a->top_s.neg, b->top_s.neg);
}

static bool is_fresh(const pst *s, const embi_box *b)
{
    if (embi_hashmap_len(&s->fresh) == 0) {
        return false;
    }
    uint64_t v = 0;
    for (const embi_box *x = b; x != nullptr; x = x->parent) {
        if (embi_hashmap_get(&s->fresh, (uint64_t)(uintptr_t)x, &v)) {
            return true;
        }
    }
    return false;
}

/* The box b is about to be laid out again in a partial relayout: its fragment, and those of its descendants that are not
 * recorded blocks (those are kept or cleared when they are reached, see embi_lay_box), are from the last layout. The
 * layout sets a box's fragment only when it has none. */
void embi_lay_incr_stale(embi_lay_doc *ld, embi_box *b)
{
    b->frag = nullptr;
    b->flags2 = (uint16_t)(b->flags2 & ~(EMBI_BF2_VGUT | EMBI_BF2_HGUT));
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        uint64_t v = 0;
        if (!embi_box_is_text(k) && embi_hashmap_get(&ld->prmap, (uint64_t)(uintptr_t)k, &v)) {
            continue;
        }
        embi_lay_reset_boxes(k);
    }
}

/* The in-flow child k of a block container laid out by block flow, whose baselines the container's come from (block.c,
 * place_child): false for an out-of-flow box and for a block-level table (it gives no baseline). *known is false when k
 * has no fragment in the container's fragment pf (the caller then cannot tell). */
static bool bl_source(const embi_box *k, const embi_frag *pf, bool *known)
{
    *known = true;
    if (embi_box_is_text(k) || k->kind == EMBI_BK_MARKER) {
        *known = false;
        return false;
    }
    if ((k->flags & EMBI_BF_ABSPOS) != 0 || embi_lay_float_out_of_flow(k)) {
        return false;
    }
    if (k->frag == nullptr || k->frag->parent != pf || k->frag->inl != 0) {
        *known = false;
        return false;
    }
    return !(embi_lay_is_table(k) && (k->flags & EMBI_BF_INLINE_LEVEL) == 0);
}

/* CF1: the box R was laid out again with the same size, position, margins and overflow as before; only its first
 * baseline (from bf0 to bf1) or its last one (bl0 to bl1) moved. The baselines of the plain blocks above it that took
 * theirs from R (block flow: the first and the last in-flow child with one) move by as much, up to the root, which
 * gives them to nobody. False, with nothing changed, when that cannot be followed: a baseline that comes or goes, an
 * ancestor that is not a plain block laid out by block flow (an inline formatting context, a flex, grid or table
 * container reads the baselines of its children), a box split in fragments. With apply the ancestors' fragments are
 * patched. */
static bool bl_patch(const embi_lctx *c, const embi_box *R, float bf0, float bf1, float bl0, float bl1, bool apply)
{
    bool cf = !same_f(bf0, bf1), cl = !same_f(bl0, bl1);
    if ((cf && (isnan(bf0) || isnan(bf1))) || (cl && (isnan(bl0) || isnan(bl1)))) {
        return false;
    }
    const float df = cf ? bf1 - bf0 : 0.0f, dl = cl ? bl1 - bl0 : 0.0f;
    const embi_box *X = R;
    while (cf || cl) {
        /* A block that clips its overflow gives its parent the bottom margin edge as baseline, not its own baselines (block.c
         * flow_child): they cannot be followed by shifting. */
        if ((X->flags & EMBI_BF_CLIP) != 0 && (X->flags & EMBI_BF_INLINE_LEVEL) == 0) {
            return false;
        }
        const embi_box *P = X->parent;
        if (P == nullptr) {
            return true; /* X is the root: the initial containing block reads no baseline */
        }
        embi_frag *pf = P->frag;
        if (P->kind != EMBI_BK_BLOCK || (P->flags & EMBI_BF_IFC) != 0 || P->tr != EMBI_TR_NONE || pf == nullptr ||
            pf->kind != EMBI_FK_BOX || pf->inl != 0 || (P->flags & EMBI_BF_ANON) != 0 || P->style == nullptr ||
            embi_lay_ssum_of(c, P->style)->incr_dep) {
            return false;
        }
        bool known = true;
        if (cf) {
            /* X gave P its first baseline when no in-flow child before it has one */
            for (const embi_box *k = P->first; k != nullptr && k != X && cf; k = k->next) {
                bool src = bl_source(k, pf, &known);
                if (!known || (src && (k->flags & EMBI_BF_CLIP) != 0 && (k->flags & EMBI_BF_INLINE_LEVEL) == 0 &&
                           (P->flags & EMBI_BF_INLINE_LEVEL) != 0)) {
                    return false;
                }
                cf = !(src && !isnan(k->frag->bl_first));
            }
            if (cf && isnan(pf->bl_first)) {
                return false;
            }
        }
        if (cl) {
            /* X gave P its last baseline when no in-flow child after it has one */
            for (const embi_box *k = X->next; k != nullptr && cl; k = k->next) {
                bool src = bl_source(k, pf, &known);
                if (!known || (src && (k->flags & EMBI_BF_CLIP) != 0 && (k->flags & EMBI_BF_INLINE_LEVEL) == 0 &&
                           (P->flags & EMBI_BF_INLINE_LEVEL) != 0)) {
                    return false;
                }
                cl = !(src && !isnan(embi_fl(k->frag)->bl_last));
            }
            if (cl && isnan(embi_fl(pf)->bl_last)) {
                return false;
            }
        }
        if (apply) {
            if (cf) {
                pf->bl_first += df;
            }
            if (cl) {
                embi_fl(pf)->bl_last += dl;
            }
        }
        X = P;
    }
    return true;
}

/* Lays out the recorded box R again on its own and splices the new fragment in where the old one was. */
static int relayout_box(pst *s, embi_box *R)
{
    embi_lay_doc *ld = s->ld;
    embi_lctx *c = &s->c;
    uint64_t v = 0;
    if (!embi_hashmap_get(&ld->prmap, (uint64_t)(uintptr_t)R, &v)) {
        return PR_SKIP;
    }
    embi_prec rec = ld->precs.data[v - 1];
    embi_frag *old = R->frag;
    bool root = R->parent == nullptr;
    if (old == nullptr || old->kind != EMBI_FK_BOX || old->inl != 0 || old->parent == nullptr ||
        old->parent->kind != EMBI_FK_BOX || (root && old->parent != ld->icb) ||
        (!root && (R->parent->kind != EMBI_BK_BLOCK || (R->parent->flags & EMBI_BF_IFC) != 0))) {
        return root ? PR_HARD : PR_SKIP;
    }
    embi_frag *pf = old->parent;
    float dx = 0.0f, dy = 0.0f;
    embi_lay_frag_shift(c, R, &dx, &dy);

    /* R is laid out afresh; the boxes below it that did not change keep their fragments (embi_lay_incr_reuse) */
    embi_lay_incr_stale(ld, R);
    embi_hashmap_clear(&ld->reusedmap);
    embi_excl ex;
    memset(&ex, 0, sizeof ex);
    if (rec.nrects > 0) {
        embi_vec_reserve(&ex.rects, rec.nrects);
        memcpy(ex.rects.data, rec.rects, (size_t)rec.nrects * sizeof *rec.rects);
        ex.rects.len = rec.nrects;
    }
    embi_cs cs = rec.cs;
    cs.excl = rec.had_excl ? &ex : nullptr;
    c->chain_ok = true;
    c->via_flow = true;
    embi_lres r;
    memset(&r, 0, sizeof r);
    embi_lay_box(c, R, &cs, &r);
    bool excl_same = ex.rects.len == rec.nrects;
    embi_vec_free(&ex.rects);
    c->chain_ok = false;
    c->via_flow = false;
    embi_frag *nf = r.frag;
    if (embi_lay_pos_count(c) != 0) {
        embi_lay_pos_discard(c); /* an out-of-flow box: its placement needs the whole document */
        return PR_HARD;
    }
    if (nf == nullptr || nf->kind != EMBI_FK_BOX || nf->inl != 0) {
        return PR_HARD;
    }
    /* the position it had, without the offset a relative box adds in the final pass; the final pass then runs over the
     * new subtree (the reused fragments only move). It runs even when the result is not used: the record of R now
     * describes this fragment, and the parent's relayout may keep it. */
    nf->x = old->x - dx;
    nf->y = old->y - dy;
    embi_lay_frag_set_mg(ld, nf, embi_frag_mg(old, 0), embi_frag_mg(old, 1), embi_frag_mg(old, 2), embi_frag_mg(old, 3));
    nf->parent = nullptr;
    nf->next = nullptr;
    embi_lay_prep_subtree(c, nf);
    if (!root && (!excl_same || !lres_same(&r, &rec.r))) {
        R->frag = nullptr;
        return PR_MISMATCH;
    }
    /* CF1: baselines that moved are followed up the plain blocks that took them from R (bl_patch) */
    const float bf0 = embi_fl(old)->bl_first, bf1 = embi_fl(nf)->bl_first;
    const float bl0 = embi_fl(old)->bl_last, bl1 = embi_fl(nf)->bl_last;
    bool bl_moved = false;
    if (!root) {
        float oo[4], no[4];
        embi_frag_ov(old, oo);
        embi_frag_ov(nf, no);
        bl_moved = !same_f(bf0, bf1) || !same_f(bl0, bl1);
        if (!same_f(nf->x, old->x) || !same_f(nf->y, old->y) || !same_f(nf->w, old->w) || !same_f(nf->h, old->h) ||
            (bl_moved && !bl_patch(c, R, bf0, bf1, bl0, bl1, false)) ||
            !same_f(oo[0], no[0]) || !same_f(oo[1], no[1]) || !same_f(oo[2], no[2]) || !same_f(oo[3], no[3])) {
            R->frag = nullptr;
            return PR_MISMATCH;
        }
    }

    /* splice */
    embi_frag *pfl = embi_fl(pf);
    nf->parent = pf;
    nf->next = old->next;
    if (pfl->first == old) {
        pfl->first = nf;
    } else {
        embi_frag *k = pfl->first;
        while (k != nullptr && k->next != old) {
            k = k->next;
        }
        if (k == nullptr) {
            R->frag = nullptr;
            return PR_HARD;
        }
        k->next = nf;
    }
    if (pfl->last == old) {
        pfl->last = nf;
    }
    if (root) {
        embi_lay_frag_set_size(pf, c->vw, c->vh); /* the initial containing block's overflow covers the new root */
        embi_lay_ov_add_child(pf, nf);
    }
    R->frag = nf;
    if (bl_moved) {
        (void)bl_patch(c, R, bf0, bf1, bl0, bl1, true);
    }
    embi_hashmap_put(&s->fresh, (uint64_t)(uintptr_t)R, 1);
    return PR_OK;
}

static int g_partial_off = -1; /* -1: not read yet, 0: partial relayout on, 1: off */

void embi_lay_set_partial_enabled(bool on)
{
    g_partial_off = on ? 0 : 1;
}

bool embi_lay_partial(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc)
{
    int off = g_partial_off;
    if (off < 0) {
        off = SDL_getenv("EMBER_NO_PARTIAL_RELAYOUT") != nullptr ? 1 : 0; /* a switch for bisecting a layout difference */
        g_partial_off = off;
    }
    if (off == 1) {
        return false;
    }
    if (ld->has_sticky || ld->abs_gone || ld->icb == nullptr || ld->root_box == nullptr || ld->reb.len == 0 || ld->partials >= 4096u ||
        arena_used_bytes(&ld->fa) > 2u * ld->fa_base + (1u << 20)) {
        return false;
    }
    ld->lc_on = false; /* the layout cache (lcache.c) is for whole runs */
    pst s;
    memset(&s, 0, sizeof s);
    s.e = e;
    s.ld = ld;
    s.doc = doc;
    embi_hashmap_init(&s.fresh);
    ctx_init(&s.c, e, ld, doc);
    ld->reuse_on = true;
    ld->reused = 0;
    embi_hashmap_clear(&ld->rebanc);
    for (uint32_t i = 0; i < ld->reb.len; i++) {
        const embi_box *rb = (const embi_box *)ld->reb.data[i]->box;
        for (const embi_box *p = rb != nullptr ? rb->parent : nullptr; p != nullptr; p = p->parent) {
            embi_hashmap_put(&ld->rebanc, (uint64_t)(uintptr_t)p, 1);
        }
    }
    bool ok = true;
    for (uint32_t i = 0; i < ld->reb.len && ok; i++) {
        embi_element *el = ld->reb.data[i];
        embi_box *nb = (embi_box *)el->box;
        if (nb == nullptr || is_fresh(&s, nb)) {
            continue;
        }
        embi_box *R = nb->parent;
        if (i < ld->rebo.len && ld->rebo.data[i].box == nb && ld->rebo.data[i].frag != nullptr) {
            /* CF1: built again in place of a recorded box with the same style: laid out on its own with that box's
             * constraint space, where that box's fragment was (relayout_box); when the result differs, its parent next */
            nb->frag = ld->rebo.data[i].frag;
            embi_hashmap_put(&ld->prmap, (uint64_t)(uintptr_t)nb, ld->rebo.data[i].rec);
            R = nb;
        }
        for (;;) {
            if (R == nullptr) {
                ok = false;
                break;
            }
            int rc = relayout_box(&s, R);
            if (rc != PR_OK && R == nb) {
                nb->frag = nullptr; /* not the fragment of this box: the parent's relayout must not reuse it */
            }
            if (rc == PR_OK) {
                break;
            }
            if (rc == PR_HARD || (rc == PR_MISMATCH && ++s.failed > 6u)) {
                ok = false;
                break;
            }
            R = R->parent;
        }
    }
    ld->reuse_on = false;
    embi_hashmap_clear(&ld->reusedmap);
    embi_hashmap_clear(&ld->rebanc);
    ctx_free(&s.c);
    embi_hashmap_free(&s.fresh);
    if (ok) {
        ld->partials++;
        ld->n_partial++;
    }
    return ok;
}

/* ------------------------------------------------------------------ public queries */

uint64_t embi_layout_run_epoch(embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return 0;
    }
    return ((const embi_lay_doc *)(uintptr_t)v)->run_epoch;
}

bool embi_layout_paint_dirty(embi_layout_engine *e, const embi_document *doc, uint64_t *serial, embi_element *const **els,
                             uint32_t *n)
{
    uint64_t v = 0;
    *els = nullptr;
    *n = 0;
    *serial = 0;
    if (!embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return false;
    }
    embi_lay_doc *ld = (embi_lay_doc *)(uintptr_t)v;
    *serial = ld->fserial;
    if (ld->pover) {
        return false;
    }
    *els = ld->pdirty.data;
    *n = ld->pdirty.len;
    return true;
}

void embi_layout_paint_clean(embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        embi_lay_doc *ld = (embi_lay_doc *)(uintptr_t)v;
        ld->pover = false;
        ld->pdirty.len = 0;
    }
}

void embi_layout_invalidate_all(embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        ((embi_lay_doc *)(uintptr_t)v)->force_full = true;
    }
}

void embi_layout_flush_text(embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        embi_lay_doc *ld = (embi_lay_doc *)(uintptr_t)v;
        ld->force_full = true;
        ld->flush_text = true;
    }
}

void embi_layout_set_images_stable(embi_layout_engine *e, bool stable)
{
    e->images_stable = stable;
}

void embi_layout_incr_stats_get(const embi_layout_engine *e, const embi_document *doc, embi_layout_incr_stats *out)
{
    memset(out, 0, sizeof *out);
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return;
    }
    const embi_lay_doc *ld = (const embi_lay_doc *)(uintptr_t)v;
    out->last_kind = ld->last_kind;
    out->boxes_laid_out = ld->last_kind == EMBI_LU_SURGERY || ld->last_kind == EMBI_LU_FULL ? ld->frag_allocs : 0;
    out->boxes_rebuilt = ld->last_kind == EMBI_LU_SURGERY ? ld->rebuilt_boxes : 0;
    out->boxes_reused = ld->last_partial ? ld->reused : 0;
    out->partial = ld->last_partial;
    out->n_none = ld->n_none;
    out->n_paint = ld->n_paint;
    out->n_surgery = ld->n_surgery;
    out->n_full = ld->n_full;
    out->n_partial = ld->n_partial;
    out->tree_builds = ld->cold.builds;
}

uint32_t embi_layout_boxes_laid_out(const embi_layout_engine *e, const embi_document *doc)
{
    embi_layout_incr_stats st;
    embi_layout_incr_stats_get(e, doc, &st);
    return st.boxes_laid_out;
}

/* ------------------------------------------------------------------ verification */

typedef struct snap_rec {
    const void *el;
    const void *style;
    uint8_t kind, flags, pe, depth;
    float x, y, w, h;
    float ov[4];
    float bl[2]; /* CF1: the first and last baselines of a box fragment (NaN when none), which bl_patch moves */
    uint32_t tb, te, tlen;
    uint64_t thash;
} snap_rec;

struct embi_lay_snap {
    snap_rec *v;
    uint32_t n, cap;
};

static void snap_push(embi_lay_snap *s, const snap_rec *r)
{
    if (s->n == s->cap) {
        uint32_t ncap = s->cap != 0 ? s->cap * 2u : 256u;
        s->v = embi_realloc(s->v, sizeof(snap_rec) * s->cap, sizeof(snap_rec) * ncap);
        s->cap = ncap;
    }
    s->v[s->n++] = *r;
}

static void snap_walk(embi_lay_snap *s, const embi_frag *f, uint8_t depth)
{
    snap_rec r;
    memset(&r, 0, sizeof r);
    r.kind = f->kind;
    /* whether a fragment has a side record is not layout: the record of the initial containing block holds the viewport's
     * scroll offset, and that of a scroll container the offset a relayout kept (layout.c, keeps_apply), which the full
     * build in a throwaway state does not have */
    r.flags = (uint8_t)(f->flags & ~EMBI_FF_EXT);
    r.depth = depth;
    r.x = f->x;
    r.y = f->y;
    r.w = f->w;
    r.h = f->h;
    if (f->box != nullptr) {
        r.el = f->box->el;
        r.style = f->box->style;
        r.pe = f->box->pe;
        if (embi_box_is_text(f->box) && f->box->text != nullptr) {
            r.tlen = f->box->text_len;
            r.thash = embi_hash_bytes(f->box->text, f->box->text_len, 0x77u);
        }
    }
    if (f->kind == EMBI_FK_TEXT) {
        r.tb = embi_frag_tbegin(f);
        r.te = embi_frag_tend(f);
    } else if (f->kind == EMBI_FK_BOX && f->inl == 0) {
        embi_frag_ov(f, r.ov);
        r.bl[0] = f->bl_first;
        r.bl[1] = embi_fl(f)->bl_last;
    }
    snap_push(s, &r);
    if (f->kind != EMBI_FK_TEXT) {
        for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
            snap_walk(s, k, (uint8_t)(depth + 1u));
        }
    }
}

embi_lay_snap *embi_lay_snapshot(const embi_frag *root)
{
    embi_lay_snap *s = embi_calloc(1, sizeof *s);
    if (root != nullptr) {
        snap_walk(s, root, 0);
    }
    return s;
}

static bool rec_equal(const snap_rec *x, const snap_rec *y)
{
    return same_f(x->bl[0], y->bl[0]) && same_f(x->bl[1], y->bl[1]) && x->el == y->el && x->style == y->style && x->kind == y->kind && x->flags == y->flags && x->pe == y->pe &&
           x->depth == y->depth && x->x == y->x && x->y == y->y && x->w == y->w && x->h == y->h && x->ov[0] == y->ov[0] &&
           x->ov[1] == y->ov[1] && x->ov[2] == y->ov[2] && x->ov[3] == y->ov[3] && x->tb == y->tb && x->te == y->te &&
           x->tlen == y->tlen && x->thash == y->thash;
}

/* Compares two snapshots; logs the first difference. */
bool embi_lay_snapshot_equal(const embi_lay_snap *a, const embi_lay_snap *b)
{
    if (a->n != b->n) {
        embi_log_error("layout verify: %u fragments after the update, %u after a full build", a->n, b->n);
        return false;
    }
    for (uint32_t i = 0; i < a->n; i++) {
        const snap_rec *x = &a->v[i], *y = &b->v[i];
        if (!rec_equal(x, y)) {
            embi_str n = x->el != nullptr ? embi_atom_str(((const embi_element *)x->el)->base.name) : (embi_str){"?", 1};
            embi_log_error("layout verify: fragment %u (<%.*s> kind %u depth %u) differs: el %p/%p style %p/%p flags %x/%x "
                           "pos %g,%g/%g,%g size %g,%g/%g,%g ov %g,%g,%g,%g/%g,%g,%g,%g baselines %g,%g/%g,%g text %u/%u",
                           i, (int)n.len, n.ptr, x->kind, x->depth, x->el, y->el, x->style, y->style, x->flags, y->flags,
                           (double)x->x, (double)x->y, (double)y->x, (double)y->y, (double)x->w, (double)x->h,
                           (double)y->w, (double)y->h, (double)x->ov[0], (double)x->ov[1], (double)x->ov[2],
                           (double)x->ov[3], (double)y->ov[0], (double)y->ov[1], (double)y->ov[2], (double)y->ov[3],
                           (double)x->bl[0], (double)x->bl[1], (double)y->bl[0], (double)y->bl[1], x->tlen, y->tlen);
            return false;
        }
    }
    return true;
}

void embi_lay_snapshot_free(embi_lay_snap *s)
{
    if (s != nullptr) {
        embi_free(s->v, sizeof(snap_rec) * s->cap);
        embi_free(s, sizeof *s);
    }
}
