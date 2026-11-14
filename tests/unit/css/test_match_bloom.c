/*
 * Ancestor bloom filter (W2b): push and pop stay balanced over a walk, bloom-filtered matching
 * agrees with unfiltered matching for every element, and the bloom holds the ancestor buckets.
 */
#include "emb_test.h"

#include "match_fixture.h"

typedef struct walk {
    const embi_sel_complex *items[64];
    uint32_t nitems;
    embi_bloom bloom;
    embi_sel_ctx ctx;
    uint32_t mismatches;
    uint32_t hits;
    uint32_t visited;
} walk;

static void walk_element(walk *w, embi_element *e)
{
    w->visited++;
    for (uint32_t k = 0; k < w->nitems; k++) {
        bool with = embi_sel_match(w->items[k], e, EMBI_PE_NONE, &w->ctx, &w->bloom);
        bool without = embi_sel_match(w->items[k], e, EMBI_PE_NONE, &w->ctx, nullptr);
        if (with != without) {
            w->mismatches++;
        }
        if (without) {
            w->hits++;
        }
    }
    embi_bloom_push(&w->bloom, e);
    for (embi_node *c = e->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            walk_element(w, (embi_element *)c);
        }
    }
    embi_bloom_pop(&w->bloom, e);
}

static bool all_zero(const embi_bloom *b)
{
    for (uint32_t k = 0; k < EMBI_BLOOM_SIZE; k++) {
        if (b->count[k] != 0) {
            return false;
        }
    }
    return true;
}

EMB_TEST(bloom_filter_agrees_with_full_match)
{
    static const char *const sels[] = {
        "div p",          "body li.sel",    "#main .x",  "html > body div p", ".nope p",
        "li + li",        "ul li:nth-child(2)", "div .y", "span",          "#d1 span",
        "[title] p",      "div > p",        "body > li", "li.sel",            "#d1 .y span",
        "body div",       "* div",          "custom-el", "#d1 > p.y",         "body ul > li",
        "p ~ span",       "div li",         "html body",
    };
    mf_tree t = mf_build();
    embi_arena a;
    embi_arena_init(&a, 4096);

    walk w;
    memset(&w, 0, sizeof w);
    for (size_t k = 0; k < sizeof sels / sizeof sels[0]; k++) {
        const embi_sel_list *l = embi_sel_parse_text(&a, ms(sels[k]), nullptr);
        EMB_REQUIRE(l != nullptr);
        EMB_REQUIRE(l->n == 1);
        EMB_REQUIRE(w.nitems < 64);
        w.items[w.nitems++] = &l->items[0];
    }
    embi_sel_ctx_init(&w.ctx, nullptr, nullptr);
    embi_bloom_clear(&w.bloom);

    walk_element(&w, t.html);

    EMB_CHECK_EQ_INT(w.mismatches, 0);
    /* The fixture has 15 elements (head included); the selectors match 21 (element, selector) pairs. */
    EMB_CHECK_EQ_INT(w.visited, 15);
    EMB_CHECK_EQ_INT(w.hits, 21);
    EMB_CHECK(all_zero(&w.bloom));

    embi_sel_ctx_free(&w.ctx);
    embi_arena_free(&a);
    mf_free(&t);
}

EMB_TEST(bloom_holds_ancestor_buckets)
{
    mf_tree t = mf_build();
    embi_bloom b;
    embi_bloom_clear(&b);
    uint32_t main_bucket = embi_bloom_hash_atom(embi_atom_intern("main", 4));
    uint32_t body_bucket = embi_bloom_hash_atom(EMBI_ATOM_body);
    EMB_CHECK(!embi_bloom_test(&b, main_bucket));

    embi_bloom_push(&b, t.body);
    EMB_CHECK(embi_bloom_test(&b, main_bucket));
    EMB_CHECK(embi_bloom_test(&b, body_bucket));
    EMB_CHECK(embi_bloom_test(&b, embi_bloom_hash_atom(embi_atom_intern("a", 1))));
    embi_bloom_pop(&b, t.body);
    EMB_CHECK(all_zero(&b));

    /* Pushes nest: the bucket stays set until every push is popped. */
    embi_bloom_push(&b, t.body);
    embi_bloom_push(&b, t.body);
    embi_bloom_pop(&b, t.body);
    EMB_CHECK(embi_bloom_test(&b, main_bucket));
    embi_bloom_pop(&b, t.body);
    EMB_CHECK(all_zero(&b));
    mf_free(&t);
}

EMB_TEST(bloom_rejects_missing_ancestor)
{
    mf_tree t = mf_build();
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = embi_sel_parse_text(&a, ms("section#nope p"), nullptr);
    EMB_REQUIRE(l != nullptr);
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, nullptr, nullptr);
    embi_bloom b;
    embi_bloom_clear(&b);
    /* With the real ancestors of p in the bloom, section#nope is absent, so the bloom rejects. */
    embi_bloom_push(&b, t.html);
    embi_bloom_push(&b, t.body);
    embi_bloom_push(&b, t.div);
    EMB_CHECK(!embi_sel_match(&l->items[0], t.p, EMBI_PE_NONE, &ctx, &b));
    EMB_CHECK(!embi_sel_match(&l->items[0], t.p, EMBI_PE_NONE, &ctx, nullptr));
    embi_bloom_pop(&b, t.div);
    embi_bloom_pop(&b, t.body);
    embi_bloom_pop(&b, t.html);
    embi_sel_ctx_free(&ctx);
    embi_arena_free(&a);
    mf_free(&t);
}

/* Differential: over a push/pop walk, a bloom-filtered match gives the same answer as an unfiltered
 * one for every selector and element, in standards and quirks mode. The selectors cover bloom
 * requirements (ids, classes, types, child and descendant chains), sibling combinators that break
 * the chain, and :is/:not/:has/nth forms that nest matching. */
static const char *const k_diff_sels[] = {
    "div p", "body li.sel", "#main .x", "html > body div p", ".nope p", "li + li",
    "ul li:nth-child(2)", "div .y", "span", "#d1 span", "[title] p", "div > p", "body > li",
    "#d1 .y span", "body div", "* div", "custom-el", "#d1 > p.y", "body ul > li", "p ~ span",
    "div li", "html body", ".a .x", ".a .X", "#MAIN .x", "BODY DIV P", "body.a.b > ul > li.sel",
    ":is(.x, ul) li", ":is(.x, ul) > li:last-child", ":not(.x) > li", "body :not(.nope) p",
    "ul:has(> li.sel) li", ":where(body, html) div", "div:has(span) p", "body:has(i) li",
    "li:nth-child(2 of .sel)", "li:nth-last-child(1 of :not(.sel))", "html :is(ul, div) > *",
    ".a.b ul > li + li", "input:checked", "a:link", "body > * + *", "#d1 ~ ul li",
    "div p, ul li", ".a .b .c .d .e",
};

static void diff_walk(walk *w, embi_element *e, uint32_t depth)
{
    for (uint32_t k = 0; k < w->nitems; k++) {
        bool with = embi_sel_match(w->items[k], e, EMBI_PE_NONE, &w->ctx, &w->bloom);
        bool without = embi_sel_match(w->items[k], e, EMBI_PE_NONE, &w->ctx, nullptr);
        if (with != without) {
            w->mismatches++;
        }
        if (without) {
            w->hits++;
        }
    }
    w->visited++;
    embi_bloom_push(&w->bloom, e);
    for (embi_node *c = e->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            diff_walk(w, (embi_element *)c, depth + 1);
        }
    }
    embi_bloom_pop(&w->bloom, e);
}

static void run_diff(mf_tree *t, uint32_t min_hits)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    walk w;
    memset(&w, 0, sizeof w);
    for (size_t k = 0; k < sizeof k_diff_sels / sizeof k_diff_sels[0]; k++) {
        const embi_sel_list *l = embi_sel_parse_text(&a, ms(k_diff_sels[k]), nullptr);
        EMB_REQUIRE(l != nullptr);
        for (uint32_t j = 0; j < l->n && w.nitems < 64; j++) {
            w.items[w.nitems++] = &l->items[j];
        }
    }
    embi_sel_ctx_init(&w.ctx, nullptr, nullptr);
    embi_bloom_clear(&w.bloom);
    diff_walk(&w, t->html, 0);
    EMB_CHECK_EQ_INT(w.mismatches, 0);
    EMB_CHECK(w.hits >= min_hits);
    EMB_CHECK(all_zero(&w.bloom));
    embi_sel_ctx_free(&w.ctx);
    embi_arena_free(&a);
}

EMB_TEST(bloom_differential_standards_and_quirks)
{
    mf_tree t = mf_build();
    run_diff(&t, 40);
    mf_free(&t);
    /* Quirks mode ignores the bloom, because class and id compare case-insensitively. */
    mf_tree q = mf_build_quirks();
    run_diff(&q, 40);
    mf_free(&q);
}

EMB_TEST_MAIN()
