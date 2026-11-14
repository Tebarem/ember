/*
 * Value blocks (WPTASAN, style.h): the values the animation layer puts in styles live as long as some style group points
 * into them, whoever holds the style. Run with the poison mode of the arenas: a block that is freed is filled with a pattern
 * and never reused, so a value read after its block went is seen.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"
#include "style/style_int.h"

#include <string.h>

static embi_document *parse_doc(const char *html)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return doc;
}

static embi_element *by_id(embi_document *doc, const char *id)
{
    embi_element *el = embi_get_element_by_id(doc, (embi_str){id, (uint32_t)strlen(id)});
    EMB_CHECK(el != nullptr);
    return el;
}

static embi_prop_id prop_of(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

/* A value of prop parsed from text, in a. */
static const embi_value *value_in(embi_arena *a, const char *prop, const char *text)
{
    embi_longhand lh[EMBI_LONGHAND_MAX];
    embi_css_slice s = embi_css_parse_cvs(a, (embi_str){text, (uint32_t)strlen(text)});
    uint32_t n = embi_css_parse_decl(a, prop_of(prop), s, lh, EMBI_LONGHAND_MAX);
    EMB_CHECK(n == 1);
    if (n != 1) {
        return nullptr;
    }
    embi_value *v = embi_arena_alloc(a, sizeof *v, 8);
    *v = lh[0].value;
    return v;
}

static bool text_is(const embi_style *s, const char *prop, const char *want)
{
    embi_sb sb = {nullptr, 0, 0};
    bool ok = embi_style_value_text(s, prop_of(prop), &sb) && sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return ok;
}

typedef struct fx {
    embi_style_engine *eng;
    embi_document *doc;
} fx;

static void fx_init(fx *f, const char *html)
{
    embi_arena_debug_poison(true);
    f->eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    f->doc = parse_doc(html);
    embi_document_set_author_sheets(f->eng, f->doc, nullptr, 0);
    embi_style_resolve_document(f->eng, f->doc);
}

static void fx_free(fx *f)
{
    embi_style_set_anim_hooks(f->eng, nullptr);
    embi_style_forget_document(f->eng, f->doc);
    embi_document_free(f->doc);
    embi_style_engine_destroy(f->eng);
    embi_arena_debug_poison(false);
}

EMB_TEST(a_block_no_group_points_into_goes_when_dropped)
{
    fx f;
    fx_init(&f, "<div id=a></div>");
    embi_style_vblock *b = embi_style_vblock_open(f.eng, 256);
    const embi_value *v = value_in(embi_style_vblock_arena(b), "width", "33px");
    embi_style_vblock_seal(f.eng, b);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    EMB_CHECK(embi_style_vblock_bytes(f.eng) > 256);
    embi_style_vblock_drop(f.eng, b);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_bytes(f.eng), 0);
    EMB_CHECK(embi_arena_debug_poisoned(v, sizeof *v));
    fx_free(&f);
}

EMB_TEST(an_overlay_keeps_its_block_after_the_drop_until_the_last_style_goes)
{
    fx f;
    fx_init(&f, "<div id=a></div>");
    const embi_style *base = embi_style_get(f.eng, by_id(f.doc, "a"), EMBI_PE_NONE);
    embi_style_vblock *b = embi_style_vblock_open(f.eng, 256);
    const embi_value *v = value_in(embi_style_vblock_arena(b), "width", "33px");
    embi_style_vblock_seal(f.eng, b);
    embi_prop_id p = prop_of("width");
    embi_style *ov = embi_style_overlay(f.eng, base, &p, &v, 1);
    embi_style *ov2 = embi_style_overlay(f.eng, base, &p, &v, 1); /* the same groups, interned */
    embi_style_vblock_drop(f.eng, b);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    EMB_CHECK(text_is(ov, "width", "33px"));
    EMB_CHECK_EQ_INT((int)embi_style_debug_poisoned(ov), 0);
    embi_style_release(f.eng, ov);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    EMB_CHECK(text_is(ov2, "width", "33px"));
    embi_style_release(f.eng, ov2);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    EMB_CHECK(embi_arena_debug_poisoned(v, sizeof *v));
    fx_free(&f);
}

/* One overlay of 20 values from 20 blocks: groups that pin more than one block, and more than the 16 kept on the stack. */
EMB_TEST(a_group_pins_every_block_it_points_into)
{
    static const char *const props[] = {"margin-top", "margin-right", "margin-bottom", "margin-left", "padding-top",
                                        "padding-right", "padding-bottom", "padding-left", "width", "height",
                                        "min-width", "min-height", "max-width", "max-height", "top",
                                        "right", "bottom", "left", "column-gap", "row-gap"};
    enum { N = sizeof props / sizeof props[0] };
    fx f;
    fx_init(&f, "<div id=a></div>");
    const embi_style *base = embi_style_get(f.eng, by_id(f.doc, "a"), EMBI_PE_NONE);
    embi_prop_id ids[N];
    const embi_value *vals[N];
    embi_style_vblock *bl[N];
    for (uint32_t i = 0; i < N; i++) {
        bl[i] = embi_style_vblock_open(f.eng, 128);
        char text[16];
        SDL_snprintf(text, sizeof text, "%upx", i + 1u);
        ids[i] = prop_of(props[i]);
        EMB_REQUIRE(ids[i] != EMBI_PROP_NONE);
        vals[i] = value_in(embi_style_vblock_arena(bl[i]), props[i], text);
        embi_style_vblock_seal(f.eng, bl[i]);
    }
    embi_style *ov = embi_style_overlay(f.eng, base, ids, vals, N);
    for (uint32_t i = 0; i < N; i++) {
        embi_style_vblock_drop(f.eng, bl[i]);
    }
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), N);
    EMB_CHECK(text_is(ov, "row-gap", "20px"));
    EMB_CHECK(text_is(ov, "margin-top", "1px"));
    EMB_CHECK_EQ_INT((int)embi_style_debug_poisoned(ov), 0);
    embi_style_release(f.eng, ov);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    EMB_CHECK(embi_arena_debug_poisoned(vals[N - 1], sizeof(embi_value)));
    fx_free(&f);
}

/* The hooks overlay an animated color on #p (in a block, as the animator does). #c inherits it: its style's group points
 * into the block too, and keeps it alive after #p's overlay is gone. */
typedef struct anim_fx {
    bool on;
    embi_element *p;
} anim_fx;

static const embi_style *apply(void *user, embi_style_engine *e, embi_element *el, const embi_style *old_slot,
                               const embi_style *base, const embi_style *parent, bool is_root)
{
    (void)old_slot;
    (void)parent;
    (void)is_root;
    anim_fx *a = user;
    if (!a->on || el != a->p) {
        return base;
    }
    embi_style_vblock *b = embi_style_vblock_open(e, 256);
    const embi_value *v = value_in(embi_style_vblock_arena(b), "color", "rgb(1, 2, 3)");
    embi_style_vblock_seal(e, b);
    embi_prop_id p = prop_of("color");
    const embi_style *ov = embi_style_overlay(e, base, &p, &v, 1);
    embi_style_vblock_drop(e, b);
    embi_style_release(e, base);
    return ov;
}

static void gone(void *user, embi_element *el)
{
    (void)user;
    (void)el;
}

EMB_TEST(a_child_style_that_inherited_an_animated_value_keeps_its_block)
{
    fx f;
    fx_init(&f, "<div id=p><div id=c></div></div>");
    anim_fx a = {true, by_id(f.doc, "p")};
    embi_style_anim_hooks h = {&a, apply, gone};
    embi_style_set_anim_hooks(f.eng, &h);
    embi_style_resolve_document(f.eng, f.doc);
    embi_element *c = by_id(f.doc, "c");
    const embi_style *kept = embi_style_get(f.eng, c, EMBI_PE_NONE);
    EMB_CHECK(text_is(kept, "color", "rgb(1, 2, 3)"));
    embi_style_retain(kept); /* what a layout grave, the change tracking or the animator's kept parent style does */
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);

    a.on = false;
    embi_style_mark_element(f.eng, a.p);
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(embi_style_get(f.eng, c, EMBI_PE_NONE) != kept);
    EMB_CHECK(!text_is(embi_style_get(f.eng, c, EMBI_PE_NONE), "color", "rgb(1, 2, 3)"));
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    EMB_CHECK(text_is(kept, "color", "rgb(1, 2, 3)"));
    EMB_CHECK_EQ_INT((int)embi_style_debug_poisoned(kept), 0);
    embi_style_release(f.eng, kept);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    fx_free(&f);
}

/* ------------------------------------------------------------------ the page map (VBLOCKPERF) */

static uint32_t rnd_state = 12345u;

static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

typedef struct tb {
    embi_style_vblock *b;
    const embi_arena_block *first, *last; /* the arena's blocks as sealed */
    const embi_value *v;
    uintptr_t lo, hi; /* [lo, hi) of the first arena block */
    bool live;
} tb;

/* A block of arena blocks of block_size, with one value at the start of the first and the arena blocks filled to the brim. */
static void tb_make(fx *f, tb *t, size_t block_size, uint32_t extra_arena_blocks)
{
    t->b = embi_style_vblock_open(f->eng, block_size);
    embi_arena *a = embi_style_vblock_arena(t->b);
    t->v = value_in(a, "width", "5px");
    t->first = a->first;
    for (uint32_t i = 0; i < extra_arena_blocks; i++) {
        /* a request that does not fit what is left makes the arena start another block */
        void *p = embi_arena_alloc(a, block_size - 8u, 8);
        EMB_CHECK(p != nullptr);
    }
    embi_style_vblock_seal(f->eng, t->b);
    t->last = a->last;
    t->lo = (uintptr_t)a->first->data;
    t->hi = t->lo + a->first->cap;
    t->live = true;
}

static void tb_check_found(const fx *f, const tb *t)
{
    EMB_CHECK(embi_style_vblock_debug_find(f->eng, t->v) == t->b);
    EMB_CHECK(embi_style_vblock_debug_find(f->eng, (const void *)t->lo) == t->b);        /* the first byte */
    EMB_CHECK(embi_style_vblock_debug_find(f->eng, (const void *)(t->hi - 1u)) == t->b); /* the last byte */
    for (const embi_arena_block *k = t->first; k != nullptr; k = k->next) {
        EMB_CHECK(embi_style_vblock_debug_find(f->eng, k->data) == t->b);
        EMB_CHECK(embi_style_vblock_debug_find(f->eng, k->data + k->cap - 1u) == t->b);
        EMB_CHECK(embi_style_vblock_debug_find(f->eng, k->data + k->cap / 2u) == t->b);
        if (k == t->last) {
            break;
        }
    }
    /* one byte past the range, and before it, is not this block's */
    EMB_CHECK(embi_style_vblock_debug_find(f->eng, (const void *)t->hi) != t->b);
    EMB_CHECK(embi_style_vblock_debug_find(f->eng, (const void *)(t->lo - 1u)) != t->b);
}

EMB_TEST(page_map_finds_every_byte_of_many_blocks_and_forgets_dropped_ones)
{
    enum { N = 300 };
    fx f;
    fx_init(&f, "<div id=a></div>");
    static tb t[N];
    uint32_t live = 0;
    for (uint32_t i = 0; i < N; i++) {
        /* sizes from a few bytes to several pages; some blocks get a second arena block */
        size_t sz = 48u + (size_t)(rnd() % 6000u);
        tb_make(&f, &t[i], sz, (rnd() % 4u) == 0 ? 1u : 0u);
        live++;
        if (i % 3 == 2) { /* drop a random earlier block while the map grows */
            uint32_t j = rnd() % (i + 1u);
            if (t[j].live) {
                uintptr_t lo = t[j].lo;
                embi_style_vblock_drop(f.eng, t[j].b);
                t[j].live = false;
                live--;
                EMB_CHECK(embi_style_vblock_debug_find(f.eng, (const void *)lo) == nullptr);
            }
        }
    }
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), (int)live);
    for (uint32_t i = 0; i < N; i++) {
        if (t[i].live) {
            tb_check_found(&f, &t[i]);
        }
    }
    /* the rest go in random order, the others staying findable all the time */
    while (live > 0) {
        uint32_t j = rnd() % N;
        if (!t[j].live) {
            continue;
        }
        uintptr_t lo = t[j].lo, hi = t[j].hi;
        embi_style_vblock_drop(f.eng, t[j].b);
        t[j].live = false;
        live--;
        EMB_CHECK(embi_style_vblock_debug_find(f.eng, (const void *)lo) == nullptr);
        EMB_CHECK(embi_style_vblock_debug_find(f.eng, (const void *)(hi - 1u)) == nullptr);
        uint32_t k = rnd() % N;
        if (t[k].live) {
            tb_check_found(&f, &t[k]);
        }
    }
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    EMB_CHECK_EQ_INT((int)f.eng->nvpieces, 0);
    fx_free(&f);
}

EMB_TEST(a_pin_outlives_the_churn_of_other_blocks_and_is_undone_after_their_removal)
{
    fx f;
    fx_init(&f, "<div id=a></div>");
    const embi_style *base = embi_style_get(f.eng, by_id(f.doc, "a"), EMBI_PE_NONE);
    embi_prop_id p = prop_of("width");
    tb keep, churn[64];
    tb_make(&f, &keep, 512, 0);
    embi_style *ov = embi_style_overlay(f.eng, base, &p, &keep.v, 1);
    uintptr_t lo = keep.lo;
    embi_style_vblock_drop(f.eng, keep.b); /* only the overlay holds it now */
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 64; i++) {
            tb_make(&f, &churn[i], 512, 0);
            /* each makes a style that points into it (the same shape as an animated element each frame) */
            embi_style *o = embi_style_overlay(f.eng, base, &p, &churn[i].v, 1);
            embi_style_vblock_drop(f.eng, churn[i].b);
            EMB_CHECK(embi_style_vblock_debug_find(f.eng, (const void *)lo) != nullptr);
            embi_style_release(f.eng, o);
        }
        EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 1);
    }
    EMB_CHECK(text_is(ov, "width", "5px"));
    EMB_CHECK_EQ_INT((int)embi_style_debug_poisoned(ov), 0);
    embi_style_release(f.eng, ov);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 0);
    EMB_CHECK(embi_style_vblock_debug_find(f.eng, (const void *)lo) == nullptr);
    EMB_CHECK_EQ_INT((int)f.eng->nvpieces, 0);
    fx_free(&f);
}

EMB_TEST(a_block_sealed_at_engine_teardown_is_freed)
{
    fx f;
    fx_init(&f, "<div id=a></div>");
    tb a, b;
    tb_make(&f, &a, 300, 1);
    tb_make(&f, &b, 5000, 0);
    EMB_CHECK_EQ_INT((int)embi_style_vblock_count(f.eng), 2);
    fx_free(&f); /* the held blocks go with the engine (LSan would see a leak) */
}

EMB_TEST_MAIN()
