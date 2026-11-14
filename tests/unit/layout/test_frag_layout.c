/*
 * W3h: the size budgets of the box and fragment records, and the side record (embi_fext) that holds
 * the scroll offset, the scrollbar gutters, the z-index and the sticky data of the few fragments that
 * have them.
 */
#include "layout_harness.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdatomic.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static embi_frag *frag_of_id(lh *h, const char *id)
{
    const embi_element *el = lh_id(h, id);
    return el != nullptr && el->box != nullptr ? ((const embi_box *)el->box)->frag : nullptr;
}

/* The budgets: a fragment is at most 96 bytes (a text fragment is allocated with 48 of them, a line with
 * 72, an inline box piece with 80) and a box at most 72 bytes (a text run 64). The compile-time asserts in
 * fragment.h and box.h enforce the same numbers; this checks them in the test build too. */
EMB_TEST(layout_record_size_budgets) {
    EMB_CHECK(sizeof(embi_frag) <= 96);
    EMB_CHECK(EMBI_FSZ_TEXT == 48 && EMBI_FSZ_LINE == 72 && EMBI_FSZ_INLINE == 80);
    EMB_CHECK(sizeof(embi_box) <= 72 && EMBI_BSZ_TEXT == 64);
    EMB_CHECK(sizeof(embi_fmet) == 48 && sizeof(embi_fext) == 88);
}

/* Ordinary fragments carry no side record: the accessors read the defaults. */
EMB_TEST(layout_fext_defaults_are_zero_without_a_side_record) {
    lh h = lh_make("<body><div id=p style='margin:0; height:20px'></div></body>", "body { margin: 0 }");
    embi_frag *p = frag_of_id(&h, "p");
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_frag_ext(p) == nullptr);
    EMB_CHECK(near(embi_frag_sx(p), 0) && near(embi_frag_sy(p), 0));
    EMB_CHECK(near(embi_frag_scr(p, 0), 0) && near(embi_frag_scr(p, 1), 0));
    EMB_CHECK(embi_frag_z_index(p) == 0);
    EMB_CHECK(embi_frag_sticky(p) == nullptr);
    lh_free(&h);
}

/* The scroll offset is set through the engine and survives a read; it does not change the layout. */
EMB_TEST(layout_scroll_offset_round_trips_through_the_side_record) {
    lh h = lh_make("<body><div id=s style='overflow:auto; width:100px; height:50px'>"
                   "<div style='height:300px'></div></div></body>",
                   "body { margin: 0 }");
    embi_frag *s = frag_of_id(&h, "s");
    EMB_REQUIRE(s != nullptr);
    float w0 = s->w, h0 = s->h;
    lh_scroll(&h, s, 3, 7);
    EMB_CHECK(embi_frag_ext(s) != nullptr);
    EMB_CHECK(near(embi_frag_sx(s), 3) && near(embi_frag_sy(s), 7));
    EMB_CHECK(near(s->w, w0) && near(s->h, h0));
    lh_scroll(&h, s, 0, 0);
    EMB_CHECK(near(embi_frag_sx(s), 0) && near(embi_frag_sy(s), 0));
    lh_free(&h);
}

/* The side records are arena memory: a document that gets a side record for every kind of fragment
 * frees all of it with the layout's arenas (embi_mem_live back to its baseline). */
EMB_TEST(layout_side_records_return_all_memory) {
    size_t before = embi_mem_live();
    {
        lh h = lh_make("<body><div id=s style='overflow:scroll; width:100px; height:50px;"
                       " position:relative; z-index:2'><div style='height:300px'></div></div>"
                       "<div id=t style='position:sticky; top:0'></div></body>",
                       "body { margin: 0 }");
        lh_set_scrollbar(&h, 15);
        lh_scroll(&h, frag_of_id(&h, "s"), 1, 2);
        lh_free(&h);
    }
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)before);
}

/* A scroll container with a scrollbar records the gutters it reserved in its side record. */
EMB_TEST(layout_scrollbar_gutters_are_recorded_for_scroll_containers) {
    lh h = lh_make("<body><div id=s style='overflow:scroll; width:100px; height:50px'></div></body>",
                   "body { margin: 0 }");
    lh_set_scrollbar(&h, 15);
    embi_frag *s = frag_of_id(&h, "s");
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(near(embi_frag_scr(s, 0), 15));
    EMB_CHECK(near(embi_frag_scr(s, 1), 15));
    lh_free(&h);
}

/* A z-index on a positioned box lives in the side record, and reads back through the accessor. */
EMB_TEST(layout_z_index_lives_in_the_side_record) {
    lh h = lh_make("<body><div id=z style='position:relative; z-index:5; width:10px; height:10px'></div>"
                   "<div id=n style='position:relative; width:10px; height:10px'></div></body>",
                   "body { margin: 0 }");
    embi_frag *z = frag_of_id(&h, "z");
    embi_frag *n = frag_of_id(&h, "n");
    EMB_REQUIRE(z != nullptr && n != nullptr);
    EMB_CHECK(embi_frag_z_index(z) == 5);
    EMB_CHECK(embi_frag_z_index(n) == 0);
    EMB_CHECK(embi_frag_ext(n) == nullptr);
    lh_free(&h);
}

/* The depth cap is the same in every build (EMBI_LAY_MAX_DEPTH, 512): a page nested deeper than the
 * inline limit is laid out on a worker thread with a large stack, so a sanitizer's bigger frames do not
 * lower it. html is element depth 1 and body 2, so div i (0-based) has depth 3 + i and has a box when
 * that is at most the cap. This runs under ASan in CI. */
EMB_TEST(layout_depth_cap_is_the_same_in_every_build) {
    enum { DEPTH = EMBI_LAY_MAX_DEPTH + 8 };
    static char html[(DEPTH + 8) * 26 + 64];
    size_t n = 0;
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<body>");
    for (int i = 0; i < DEPTH; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<div id=d%d>", i);
    }
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<p id=leaf>x</p>");
    for (int i = 0; i < DEPTH; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "</div>");
    }
    lh h = lh_make(html, "body { margin: 0 }");
    int last = (int)EMBI_LAY_MAX_DEPTH - 3; /* the deepest div that fits */
    char id[16];
    SDL_snprintf(id, sizeof id, "d%d", last);
    EMB_CHECK(frag_of_id(&h, id) != nullptr);
    SDL_snprintf(id, sizeof id, "d%d", last - 1);
    EMB_CHECK(frag_of_id(&h, id) != nullptr);
    SDL_snprintf(id, sizeof id, "d%d", last + 1);
    EMB_CHECK(frag_of_id(&h, id) == nullptr);
    EMB_CHECK(frag_of_id(&h, "leaf") == nullptr);
    EMB_CHECK(frag_of_id(&h, "d0") != nullptr);
    lh_free(&h);
}

/* A page just under the inline limit lays out on the caller's stack, and one just over it on the
 * worker: both give the same geometry for the same content. */
EMB_TEST(layout_deep_pages_lay_out_the_same_on_either_stack) {
    float y[2] = {0, 0};
    int depths[2] = {(int)EMBI_LAY_INLINE_DEPTH - 8, (int)EMBI_LAY_INLINE_DEPTH + 8};
    for (int k = 0; k < 2; k++) {
        static char html[(EMBI_LAY_INLINE_DEPTH + 16) * 40 + 64];
        size_t n = 0;
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<body>");
        for (int i = 0; i < depths[k]; i++) {
            n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<div style='padding-top:1px'>");
        }
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<p id=leaf style='margin:0;height:10px'></p>");
        for (int i = 0; i < depths[k]; i++) {
            n += (size_t)SDL_snprintf(html + n, sizeof html - n, "</div>");
        }
        lh h = lh_make(html, "body { margin: 0 }");
        embi_frag *leaf = frag_of_id(&h, "leaf");
        EMB_REQUIRE(leaf != nullptr);
        y[k] = leaf->h;
        EMB_CHECK(near(leaf->h, 10));
        lh_free(&h);
    }
    EMB_CHECK(near(y[0], y[1]));
}

/* ------------------------------------------------------------------ allocation and memory budgets */

/* A representative document of about `blocks` sections: paragraphs with inline markup, floats, a flex
 * row of cards, a grid, a table and a positioned badge. */
static char *budget_html(uint32_t blocks)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_sb_append(&sb, EMBI_STR("<body id=b>"));
    for (uint32_t i = 0; i < blocks; i++) {
        char t[1280];
        SDL_snprintf(t, sizeof t,
                     "<section><p>Paragraph %u with <span>an inline span</span> and <a href=#x>a link</a>, "
                     "some <b>bold words</b> and a long enough tail to need a few more glyphs.</p>"
                     "<div class=fl>float</div>"
                     "<div class=row><div class=card><h4>Card</h4><p>Card text %u.</p></div>"
                     "<div class=card><h4>Card</h4><p>Card text.</p></div></div>"
                     "<div class=grid><div>a</div><div>b</div><div>c</div><div>d</div></div>"
                     "<table class=t><tr><td>1</td><td>2</td><td>3</td></tr><tr><td>4</td><td>5</td><td>6</td></tr></table>"
                     "<div class=host>host<span class=badge>%u</span></div></section>",
                     i, i, i);
        embi_sb_append(&sb, (embi_str){t, (uint32_t)SDL_strlen(t)});
    }
    embi_sb_append(&sb, EMBI_STR("</body>"));
    char *out = SDL_malloc(sb.len + 1);
    SDL_memcpy(out, sb.data, sb.len);
    out[sb.len] = '\0';
    embi_sb_free(&sb);
    return out;
}

static const char *const BUDGET_CSS =
    "body { margin: 0; font: 16px/1.4 sans-serif } section { margin: 4px 0; width: 600px }"
    ".fl { float: left; width: 80px; height: 30px } .row { display: flex; gap: 6px }"
    ".card { flex: 1 1 0; padding: 4px; border: 1px solid #ccc } .grid { display: grid; grid-template-columns: 1fr 1fr }"
    ".t { border-collapse: collapse } .t td { border: 1px solid #999; padding: 1px 3px }"
    ".host { position: relative; padding-right: 30px } .badge { position: absolute; top: 0; right: 0 }";

/* The live layout memory per box stays under a ceiling (box tree, fragment tree and their side records;
 * the shaped runs are a bounded cache of their own). This page measures 184 bytes per box and bench_layout
 * 155.7 on its 23k-box page (W3p: tiered fragments, interned metrics, 72-byte boxes; the target of 160 is met);
 * the ceiling here is the measured value plus 1.5 percent, so a regression of the fragment recycling or a
 * bigger record fails this test. Sanitizer builds allocate whole records. */
EMB_TEST(layout_live_memory_per_box_stays_under_the_ceiling) {
    char *html = budget_html(30);
    lh h = lh_make(html, BUDGET_CSS);
    SDL_free(html);
    uint32_t boxes = 0, frags = 0;
    size_t bytes = 0;
    embi_layout_stats(h.eng, h.doc, &boxes, &frags, &bytes);
    EMB_REQUIRE(boxes > 500);
    double per_box = (double)bytes / (double)boxes;
    SDL_Log("live layout memory: %.1f bytes per box (%u boxes, %u fragments)", per_box, boxes, frags);
    EMB_CHECK(per_box <= (EMB_TEST_ASAN ? 330.0 : 187.0));
    /* The arenas hold the reachable boxes and fragments and little else: the discarded passes were
     * recycled, so the bytes are within 25 percent of what the reachable records need. */
    double need = (double)boxes * (double)sizeof(embi_box) + (double)frags * (double)sizeof(embi_frag);
    EMB_CHECK((double)bytes <= need * 1.25);
    /* The tiers make most fragments smaller than the full record. */
    if (!EMB_TEST_ASAN) {
        EMB_CHECK((double)bytes <= need * 0.97);
    }
    lh_free(&h);
}

/* The layout asks for a bounded number of fragments: re-layout passes (a block laid out again after
 * clearance or a scroll gutter, flex and grid item measures, table rows) recycle the fragments they
 * threw away, so most requests are served from the free list and the arena holds about the reachable
 * count. */
EMB_TEST(layout_fragment_allocations_are_bounded_and_recycled) {
    char *html = budget_html(30);
    lh h = lh_make(html, BUDGET_CSS);
    SDL_free(html);
    uint32_t boxes = 0, frags = 0, allocs = 0, reused = 0;
    size_t bytes = 0;
    embi_layout_stats(h.eng, h.doc, &boxes, &frags, &bytes);
    embi_layout_alloc_stats(h.eng, h.doc, &allocs, &reused);
    SDL_Log("fragments: %u reachable, %u asked for, %u recycled", frags, allocs, reused);
    EMB_REQUIRE(frags > 500);
    EMB_CHECK(allocs <= frags * 3);
    /* What was not recycled is what the arena had to grow by: it stays close to the reachable count. */
    EMB_CHECK(allocs - reused <= frags + frags / 4);
    (void)reused; /* W3p: flex and grid items are laid out once, so little is thrown away to recycle */
    lh_free(&h);
}

/* Tables nested twelve deep: each table lays its cells out once when their content does not depend on the
 * final column widths, so the number of fragments asked for grows linearly with the depth. A layout that
 * measured every cell twice per level would ask for 2^12 times as many (W3p). */
EMB_TEST(layout_nested_tables_ask_for_a_linear_number_of_fragments) {
    enum { DEPTH = 12 };
    char *html = SDL_calloc(1, 16384);
    EMB_REQUIRE(html != nullptr);
    SDL_strlcat(html, "<body>", 16384);
    for (int i = 0; i < DEPTH; i++) {
        SDL_strlcat(html, "<table><tr><td>cell<td>", 16384);
    }
    SDL_strlcat(html, "<p>leaf text</p>", 16384);
    for (int i = 0; i < DEPTH; i++) {
        SDL_strlcat(html, "</table>", 16384);
    }
    lh h = lh_make(html, "body { margin: 0 } table { border-spacing: 0 } td { padding: 1px }");
    SDL_free(html);
    uint32_t boxes = 0, frags = 0, allocs = 0, reused = 0;
    size_t bytes = 0;
    embi_layout_stats(h.eng, h.doc, &boxes, &frags, &bytes);
    embi_layout_alloc_stats(h.eng, h.doc, &allocs, &reused);
    SDL_Log("nested tables: %u boxes, %u fragments, %u asked for", boxes, frags, allocs);
    EMB_REQUIRE(frags > DEPTH * 4);
    /* Linear: the layout asks for about 1.4 fragments per surviving one. A table that measured each cell
     * once more per level would ask for 2^12 times as many; even 1.5 passes per level is 130 times. */
    EMB_CHECK(allocs <= frags * 2 + 16);
    lh_free(&h);
}

/* An unchanged document shapes nothing on its next layout, and the memory a layout holds is the same
 * after every layout (no growth across layouts). */
EMB_TEST(layout_relayout_reuses_the_shaped_runs_and_holds_steady_memory) {
    char *html = budget_html(12);
    lh h = lh_make(html, BUDGET_CSS);
    SDL_free(html);
    lh_relayout(&h);
    size_t live1 = embi_mem_live();
    uint64_t shaped0 = atomic_load(&embi_tshim_shaped_runs);
    lh_relayout(&h);
    uint64_t shaped1 = atomic_load(&embi_tshim_shaped_runs);
    size_t live2 = embi_mem_live();
    lh_relayout(&h);
    size_t live3 = embi_mem_live();
    EMB_CHECK_EQ_INT((long long)(shaped1 - shaped0), 0);
    EMB_CHECK(live2 <= live1 + 4096);
    EMB_CHECK(live3 <= live2 + 4096);
    lh_free(&h);
}

/* The shaped runs are a cache bounded by the last layout: after the content shrinks, the runs of the
 * removed content are freed by the next layout, and a layout never keeps more shaped text than it uses. */
EMB_TEST(layout_shaped_runs_are_bounded_by_the_last_layout) {
    char *big = budget_html(40);
    lh h = lh_make(big, BUDGET_CSS);
    SDL_free(big);
    size_t live_big = embi_mem_live();
    embi_element *body = lh_id(&h, "b");
    EMB_REQUIRE(body != nullptr);
    while (body->base.last_child != nullptr && body->base.last_child != body->base.first_child) {
        EMB_REQUIRE(embi_remove_child(&body->base, body->base.last_child) == EMBI_DOM_OK);
    }
    lh_relayout(&h);
    size_t live_small = embi_mem_live();
    EMB_CHECK(live_small + 100000 < live_big);
    lh_free(&h);
}


/* ------------------------------------------------------------------ the kept box tree (W3p) */

/* One fragment of a snapshot of the fragment tree: everything a consumer reads, no pointers. */
typedef struct snap_rec {
    uint8_t kind, flags, bkind;
    uint32_t nchild, tb, te;
    float v[22]; /* x y w h bl_first bl_last ov[4] bdr[4] pad[4] mg[4] */
} snap_rec;

typedef struct snap {
    snap_rec *r;
    uint32_t n, cap;
} snap;

static void snap_walk(snap *s, const embi_frag *f)
{
    if (s->n == s->cap) {
        s->cap = s->cap != 0 ? s->cap * 2 : 256;
        s->r = SDL_realloc(s->r, s->cap * sizeof *s->r);
    }
    uint32_t idx = s->n++;
    snap_rec *r = &s->r[idx];
    SDL_memset(r, 0, sizeof *r);
    r->kind = f->kind;
    r->flags = f->flags;
    r->bkind = f->box != nullptr ? f->box->kind : 0;
    r->v[0] = f->x;
    r->v[1] = f->y;
    r->v[2] = f->w;
    r->v[3] = f->h;
    r->v[4] = embi_frag_bl_first(f);
    if (f->kind != EMBI_FK_TEXT) {
        r->v[5] = f->bl_last;
        float ov[4];
        embi_frag_ov(f, ov);
        for (int i = 0; i < 4; i++) {
            r->v[6 + i] = ov[i];
        }
    } else {
        r->tb = embi_frag_tbegin(f);
        r->te = embi_frag_tend(f);
    }
    if (f->kind == EMBI_FK_BOX) {
        for (int i = 0; i < 4; i++) {
            r->v[10 + i] = embi_frag_bdr(f, i);
            r->v[14 + i] = embi_frag_pad(f, i);
            r->v[18 + i] = embi_frag_mg(f, i);
        }
    }
    uint32_t n = 0;
    for (const embi_frag *k = f->kind != EMBI_FK_TEXT ? f->first : nullptr; k != nullptr; k = k->next) {
        snap_walk(s, k);
        n++;
    }
    s->r[idx].nchild = n; /* s->r may have moved while the children were added */
}

static snap snap_of(const lh *h)
{
    snap s = {nullptr, 0, 0};
    const embi_frag *root = embi_layout_root(h->eng, h->doc);
    if (root != nullptr) {
        snap_walk(&s, root);
    }
    return s;
}

static bool same_f(float a, float b)
{
    return (isnan(a) && isnan(b)) || a == b;
}

/* True when the snapshots are the same tree with the same numbers. */
static bool snap_eq(const snap *a, const snap *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (uint32_t i = 0; i < a->n; i++) {
        const snap_rec *x = &a->r[i], *y = &b->r[i];
        if (x->kind != y->kind || x->flags != y->flags || x->bkind != y->bkind || x->nchild != y->nchild ||
            x->tb != y->tb || x->te != y->te) {
            return false;
        }
        for (int k = 0; k < 22; k++) {
            if (!same_f(x->v[k], y->v[k])) {
                return false;
            }
        }
    }
    return true;
}

static void snap_free(snap *s)
{
    SDL_free(s->r);
    SDL_memset(s, 0, sizeof *s);
}

static void set_attr(lh *h, const char *id, const char *name, const char *value)
{
    embi_element *el = lh_id(h, id);
    EMB_REQUIRE(el != nullptr);
    embi_str v = {value, (uint32_t)SDL_strlen(value)};
    embi_str n = {name, (uint32_t)SDL_strlen(name)};
    EMB_REQUIRE(embi_el_set_attribute(el, n, embi_dstr_from_utf8(h->doc->slab, v)) == EMBI_DOM_OK);
}

/* Styles that depend on the viewport and the scrollbar, so that a stale kept tree (the cached edges and
 * the remembered gutters live with it) gives a different result from a fresh one. */
static const char *const KEPT_HTML =
    "<body id=b><div id=m class=v><p>Paragraph with <b>bold</b> and <span id=sp>a span</span> of text that wraps "
    "around inside the box when it is narrow enough to need several lines of text.</p></div>"
    "<div id=sc style='overflow:auto; height:40px'><div style='height:200px'>tall</div></div>"
    "<div class=row><div>one</div><div>two</div><div>three</div></div>"
    "<table><tr><td>a<td>b</table></body>";

static const char *const KEPT_CSS =
    "body { margin: 0; font: 16px/1.3 sans-serif } .v { padding: 2vw 3vh; margin: 1vw }"
    ".row { display: flex } .row div { padding: 1vw; border: 1px solid }";

/* A second layout of an unchanged document keeps the box tree (the element still points at the same
 * root box) and gives the same fragments, down to every rectangle, baseline and text range; so does a
 * third. */
EMB_TEST(layout_unchanged_relayout_keeps_the_tree_and_the_fragments) {
    lh h = lh_make(KEPT_HTML, KEPT_CSS);
    const void *root0 = lh_root_box(&h);
    snap s0 = snap_of(&h);
    EMB_REQUIRE(s0.n > 20);
    for (int i = 0; i < 3; i++) {
        lh_relayout(&h);
        snap s = snap_of(&h);
        EMB_CHECK(snap_eq(&s0, &s));
        EMB_CHECK(lh_root_box(&h) == root0);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(h.eng, h.doc), 1);
        snap_free(&s);
    }
    /* Against a document that was never laid out twice. */
    lh fresh = lh_make(KEPT_HTML, KEPT_CSS);
    snap sf = snap_of(&fresh);
    EMB_CHECK(snap_eq(&s0, &sf));
    snap_free(&sf);
    lh_free(&fresh);
    snap_free(&s0);
    lh_free(&h);
}

/* Each part of the key the kept tree is valid for forces a rebuild: after the change the layout equals
 * that of a fresh engine on the same (changed) document, and differs from the layout before it. */
EMB_TEST(layout_each_invalidating_change_rebuilds_the_tree) {
    /* A style change (the style epoch) and a change of the box structure. */
    for (int k = 0; k < 2; k++) {
        lh a = lh_make(KEPT_HTML, KEPT_CSS), b = lh_make(KEPT_HTML, KEPT_CSS);
        snap before = snap_of(&a);
        const char *id = k == 0 ? "m" : "sp";
        const char *style = k == 0 ? "padding: 20px; border: 5px solid" : "display: block; margin: 9px";
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 1);
        set_attr(&a, id, "style", style);
        set_attr(&b, id, "style", style);
        lh_relayout(&a);
        lh_relayout(&b);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        lh_relayout(&a); /* and then it is kept */
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        snap sa = snap_of(&a), sb = snap_of(&b);
        EMB_CHECK(snap_eq(&sa, &sb));
        EMB_CHECK(!snap_eq(&sa, &before));
        snap_free(&before);
        snap_free(&sa);
        snap_free(&sb);
        lh_free(&a);
        lh_free(&b);
    }
    /* The viewport width, then the height. */
    for (int axis = 0; axis < 2; axis++) {
        lh a = lh_make(KEPT_HTML, KEPT_CSS), b = lh_make(KEPT_HTML, KEPT_CSS);
        snap before = snap_of(&a);
        float w = axis == 0 ? 500.0f : 800.0f, hh = axis == 0 ? 600.0f : 350.0f;
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 1);
        embi_layout_set_viewport(a.eng, w, hh);
        embi_layout_set_viewport(b.eng, w, hh);
        lh_relayout(&a);
        lh_relayout(&b);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        lh_relayout(&a);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        snap sa = snap_of(&a), sb = snap_of(&b);
        EMB_CHECK(snap_eq(&sa, &sb));
        EMB_CHECK(!snap_eq(&sa, &before));
        snap_free(&before);
        snap_free(&sa);
        snap_free(&sb);
        lh_free(&a);
        lh_free(&b);
    }
    /* The scrollbar thickness (the engine has no setter: the key is checked against the field). */
    {
        lh a = lh_make(KEPT_HTML, KEPT_CSS);
        lh b = lh_make_cfg(KEPT_HTML, SDL_strlen(KEPT_HTML), KEPT_CSS, &(embi_layout_config){nullptr, nullptr, 15});
        snap before = snap_of(&a);
        a.eng->scrollbar = 15;
        lh_relayout(&a);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        lh_relayout(&a);
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
        snap sa = snap_of(&a), sb = snap_of(&b);
        EMB_CHECK(snap_eq(&sa, &sb));
        EMB_CHECK(!snap_eq(&sa, &before));
        snap_free(&before);
        snap_free(&sa);
        snap_free(&sb);
        lh_free(&a);
        lh_free(&b);
    }
}

/* Replaces the data of the first text child of element id with `text` (through the DOM API). */
static void set_text_of(lh *h, const char *id, const char *text)
{
    embi_element *el = lh_id(h, id);
    EMB_REQUIRE(el != nullptr);
    embi_node *t = el->base.first_child;
    while (t != nullptr && t->type != EMBI_NODE_TEXT) {
        t = t->next;
    }
    EMB_REQUIRE(t != nullptr);
    embi_str v = {text, (uint32_t)SDL_strlen(text)};
    embi_chardata_set(embi_as_chardata(t), embi_dstr_from_utf8(h->doc->slab, v));
}

#define TXT_HTML "<body id=b><div id=m><p id=p>short text</p><p>other <i>x</i></p></div></body>"
#define TXT_CSS "body { margin: 0; font: 16px/1.3 sans-serif } #m { width: 300px }"

/* The text boxes point straight at the bytes of the DOM text node: a mutation of that node (a longer string
 * that reallocates the data, a double space or a tab that takes the processing path, non-ASCII text)
 * must drop the kept tree, rebuild it, and give the layout of a fresh engine. ASan reports a
 * box that still read the freed bytes. A text over EMBI_TEXT_MAX (16 MB) is not covered: laying out one
 * unbreakable run that long is quadratic (200 KB already takes about 30 s), see the W3 notes. */
EMB_TEST(layout_dom_text_mutation_rebuilds_the_tree_and_never_reads_freed_text) {
    char *longer = SDL_malloc(5001);
    EMB_REQUIRE(longer != nullptr);
    for (int i = 0; i < 5000; i++) {
        longer[i] = (i % 7 == 6) ? ' ' : (char)('a' + i % 7);
    }
    longer[5000] = 0;
    const char *const texts[] = {
        longer,                                  /* much longer: reallocates the data */
        "x",                                     /* shorter again */
        "two  spaces\tand a tab\nand a newline", /* the white space processing path */
        "caf\xC3\xA9 non-ascii text",            /* not narrow ASCII */
        "plain again, narrow ASCII once more",
        "back to short",
    };
    lh a = lh_make(TXT_HTML, TXT_CSS);
    EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 1);
    uint32_t builds = 1;
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        snap before = snap_of(&a);
        set_text_of(&a, "p", texts[i]);
        lh_relayout(&a);
        builds++;
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), builds);
        lh_relayout(&a); /* and then it is kept */
        EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), builds);
        snap sa = snap_of(&a);
        lh b = lh_make(TXT_HTML, TXT_CSS);
        set_text_of(&b, "p", texts[i]);
        lh_relayout(&b);
        snap sb = snap_of(&b);
        EMB_CHECK(sa.n > 3);
        EMB_CHECK(snap_eq(&sa, &sb));
        EMB_CHECK(!snap_eq(&sa, &before));
        snap_free(&before);
        snap_free(&sa);
        snap_free(&sb);
        lh_free(&b);
    }
    lh_free(&a);
    SDL_free(longer);
}

static float g_img_w = 40, g_img_h = 20;

static bool test_image_size(void *user, embi_str url, float *w, float *hh, bool *ratio)
{
    (void)user;
    (void)url;
    *w = g_img_w;
    *hh = g_img_h;
    *ratio = true;
    return true;
}

/* A document with an image asks the embedder for its size on every build, and the embedder's answer can
 * change without any DOM change: such a tree is never kept, so the next layout follows the new size. */
EMB_TEST(layout_a_document_with_images_is_rebuilt_every_layout) {
    static const char *html = "<body><div id=d><img id=i src=a.png><p>after the image</p></div></body>";
    embi_layout_config cfg = {nullptr, test_image_size, 0};
    g_img_w = 40;
    g_img_h = 20;
    lh a = lh_make_cfg(html, SDL_strlen(html), "body { margin: 0 }", &cfg);
    embi_frag *img = frag_of_id(&a, "i");
    EMB_REQUIRE(img != nullptr);
    EMB_CHECK(near(img->w, 40) && near(img->h, 20));
    g_img_w = 100;
    g_img_h = 70;
    lh_relayout(&a);
    img = frag_of_id(&a, "i");
    EMB_REQUIRE(img != nullptr);
    EMB_CHECK(near(img->w, 100) && near(img->h, 70));
    EMB_CHECK_EQ_INT(embi_layout_tree_builds(a.eng, a.doc), 2);
    lh b = lh_make_cfg(html, SDL_strlen(html), "body { margin: 0 }", &cfg);
    snap sa = snap_of(&a), sb = snap_of(&b);
    EMB_CHECK(snap_eq(&sa, &sb));
    snap_free(&sa);
    snap_free(&sb);
    lh_free(&a);
    lh_free(&b);
}

/* What a layout leaves in the boxes of a kept tree is reset before the next one: the fragment pointers
 * (they point into the fragment arena the new layout rewinds) and the remembered scrollbar gutters. The
 * rest of a box (style, text, intrinsic sizes) stays. */
EMB_TEST(layout_reset_boxes_clears_the_per_layout_fields) {
    lh h = lh_make(KEPT_HTML, KEPT_CSS);
    embi_box *root = (embi_box *)(uintptr_t)lh_root_box(&h);
    EMB_REQUIRE(root != nullptr);
    uint32_t n = 0, with_frag = 0;
    for (embi_box *b = root;;) {
        n++;
        if (!embi_box_is_text(b) && b->frag != nullptr) {
            with_frag++;
        }
        b->flags2 |= (uint16_t)(EMBI_BF2_VGUT | EMBI_BF2_HGUT | EMBI_BF2_HINDEP);
        if (!embi_box_is_text(b) && b->first != nullptr) {
            b = b->first;
            continue;
        }
        while (b != root && b->next == nullptr) {
            b = b->parent;
        }
        if (b == root) {
            break;
        }
        b = b->next;
    }
    EMB_CHECK(with_frag > 10 && with_frag <= n);
    const embi_style *st = root->style;
    embi_lay_reset_boxes(root);
    for (embi_box *b = root;;) {
        EMB_CHECK(embi_box_is_text(b) || b->frag == nullptr);
        EMB_CHECK(embi_box_is_text(b) || (b->flags2 & (EMBI_BF2_VGUT | EMBI_BF2_HGUT)) == 0);
        EMB_CHECK((b->flags2 & EMBI_BF2_HINDEP) != 0);
        if (!embi_box_is_text(b) && b->first != nullptr) {
            b = b->first;
            continue;
        }
        while (b != root && b->next == nullptr) {
            b = b->parent;
        }
        if (b == root) {
            break;
        }
        b = b->next;
    }
    EMB_CHECK(root->style == st);
    lh_free(&h);
}

/* A scroll container that needed a bar remembers it in the box (VGUT) for the next layout; a kept tree
 * lays out the same as the first layout, so the remembered gutter never changes the result. */
EMB_TEST(layout_remembered_gutters_do_not_change_a_kept_layout) {
    static const char *html = "<body><div id=s style='overflow:auto; width:120px; height:60px'>"
                              "<p>text that is long enough to wrap in a narrow box</p><div style='height:200px'></div></div></body>";
    embi_layout_config cfg = {nullptr, nullptr, 15};
    lh a = lh_make_cfg(html, SDL_strlen(html), "body { margin: 0 }", &cfg);
    snap s0 = snap_of(&a);
    EMB_CHECK(near(embi_frag_scr(frag_of_id(&a, "s"), 0), 15));
    for (int i = 0; i < 3; i++) {
        lh_relayout(&a);
        snap s = snap_of(&a);
        EMB_CHECK(snap_eq(&s0, &s));
        snap_free(&s);
    }
    snap_free(&s0);
    lh_free(&a);
}

/* The accessors give the defaults for the fragments that are allocated without the field: a text
 * fragment or a line has no margins, no side record and no scroll offset. */
EMB_TEST(layout_accessors_are_safe_on_every_fragment_kind) {
    lh h = lh_make(KEPT_HTML, KEPT_CSS);
    uint32_t texts = 0, lines = 0, boxes = 0;
    const embi_frag *stack[256];
    int sp = 0;
    stack[sp++] = embi_layout_root(h.eng, h.doc);
    while (sp > 0) {
        const embi_frag *f = stack[--sp];
        if (f->kind == EMBI_FK_TEXT) {
            texts++;
            EMB_CHECK(near(embi_frag_mg(f, 2), 0) && near(embi_frag_bdr(f, 0), 0) && near(embi_frag_pad(f, 3), 0));
            EMB_CHECK(isnan(embi_frag_bl_first(f)) && isnan(embi_frag_bl_last(f)) && embi_frag_first(f) == nullptr);
            EMB_CHECK(near(embi_frag_sx(f), 0) && near(embi_frag_scr(f, 1), 0));
            EMB_CHECK(embi_frag_z_index(f) == 0 && embi_frag_sticky(f) == nullptr);
            float ov[4];
            embi_frag_ov(f, ov);
            EMB_CHECK(near(ov[2], f->w) && near(ov[3], f->h));
        } else if (f->kind == EMBI_FK_LINE) {
            lines++;
            EMB_CHECK(near(embi_frag_mg(f, 0), 0) && near(embi_frag_bdr(f, 2), 0));
            EMB_CHECK(embi_frag_z_index(f) == 0 && near(embi_frag_sy(f), 0));
        } else {
            boxes++;
        }
        if (f->kind != EMBI_FK_TEXT) {
            for (const embi_frag *k = f->first; k != nullptr && sp < 255; k = k->next) {
                stack[sp++] = k;
            }
        }
    }
    EMB_CHECK(texts > 3 && lines > 1 && boxes > 5);
    lh_free(&h);
}

/* The derived data a layout keeps (the inline analysis, the edge cache, the scratch pool) is counted,
 * reaches a steady size, and is freed with the document. */
EMB_TEST(layout_cache_bytes_are_counted_and_steady) {
    char *html = budget_html(12);
    size_t before = embi_mem_live();
    {
        lh h = lh_make(html, BUDGET_CSS);
        size_t c0 = embi_layout_cache_bytes(h.eng, h.doc);
        EMB_CHECK(c0 > 4096);
        lh_relayout(&h);
        size_t c1 = embi_layout_cache_bytes(h.eng, h.doc);
        lh_relayout(&h);
        size_t c2 = embi_layout_cache_bytes(h.eng, h.doc);
        EMB_CHECK(c1 == c0 && c2 == c0);
        uint32_t boxes = 0, frags = 0;
        size_t bytes = 0;
        embi_layout_stats(h.eng, h.doc, &boxes, &frags, &bytes);
        SDL_Log("derived caches: %u bytes for %u boxes (%.1f per box)", (unsigned)c0, boxes, (double)c0 / (double)boxes);
        /* W3q: the content-keyed caches (shaped-run entries with their text, paragraph analyses) stay between
         * layouts, so this is 268 on this small page (197 on the bench page; 121 before W3q, 160 was the ceiling).
         * The 160 B/box target counts the box and fragment trees only (layout_live_memory_per_box...). */
        EMB_CHECK((double)c0 <= 300.0 * (double)boxes);
        lh_free(&h);
    }
    SDL_free(html);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)before);
}

/* A coarse guard on speed: a 60-section document lays out well inside a generous ceiling (the bench is
 * the precise measurement; this catches an accidental quadratic or a lost cache). */
EMB_TEST(layout_relayout_of_a_medium_document_is_fast) {
    char *html = budget_html(60);
    lh h = lh_make(html, BUDGET_CSS);
    SDL_free(html);
    lh_relayout(&h); /* warm */
    uint64_t t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < 3; i++) {
        lh_relayout(&h);
    }
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency() / 3.0;
    SDL_Log("medium document relayout: %.1f ms", ms);
    double ceiling = EMB_TEST_ASAN ? 1500.0 : 150.0;
    EMB_CHECK(ms < ceiling);
    lh_free(&h);
}

EMB_TEST_MAIN()
