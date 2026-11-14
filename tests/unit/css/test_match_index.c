/*
 * Rule buckets (W2b): for every element, each selector that matches it must be among the candidates
 * the index returns. The index may return extra candidates, which the matcher then rejects.
 */
#include "emb_test.h"

#include "match_fixture.h"

typedef struct cand {
    bool *seen;
    uint32_t n;
    uint32_t count;
} cand;

static void mark(void *user, uint64_t payload)
{
    cand *c = user;
    if (payload < c->n) {
        if (!c->seen[payload]) {
            c->count++;
        }
        c->seen[payload] = true;
    }
}

#define N_SELS 22

static const char *const k_sels[N_SELS] = {
    "div",          "p",           "#d1",          ".sel",         "li",         "[title]",
    "*",            "body div",    ":not(p)",      "input[type=checkbox]", "a:link", "span:lang(fr)",
    "custom-el",    "#main",       "[lang]",       "li:nth-child(2)", "div > p.y", ":is(li, span)",
    ":root",        "rect",        "[TYPE]",       ".a",
};

EMB_TEST(index_candidates_cover_matches)
{
    mf_tree t = mf_build();
    embi_arena a;
    embi_arena_init(&a, 8192);
    embi_sel_index ix = {0};
    const embi_sel_list *lists[N_SELS];

    for (uint32_t k = 0; k < N_SELS; k++) {
        lists[k] = embi_sel_parse_text(&a, ms(k_sels[k]), nullptr);
        EMB_REQUIRE(lists[k] != nullptr);
        EMB_REQUIRE(lists[k]->n == 1);
        embi_sel_index_add(&ix, &lists[k]->items[0], k);
    }

    bool seen[N_SELS];
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, nullptr, nullptr);

    /* Walk every element in tree order. */
    embi_element *els[32];
    uint32_t nel = 0;
    for (embi_node *n = embi_next_in_tree(&t.doc->base, &t.doc->base); n != nullptr;
         n = embi_next_in_tree(&t.doc->base, n)) {
        if (n->type == EMBI_NODE_ELEMENT && nel < 32) {
            els[nel++] = (embi_element *)n;
        }
    }
    EMB_CHECK_EQ_INT(nel, 15);

    uint32_t misses = 0;
    uint32_t total_matches = 0;
    uint32_t total_candidates = 0;
    for (uint32_t e = 0; e < nel; e++) {
        memset(seen, 0, sizeof seen);
        cand c = {seen, N_SELS, 0};
        embi_sel_index_candidates(&ix, els[e], mark, &c);
        total_candidates += c.count;
        for (uint32_t k = 0; k < N_SELS; k++) {
            if (embi_sel_match(&lists[k]->items[0], els[e], EMBI_PE_NONE, &ctx, nullptr)) {
                total_matches++;
                if (!seen[k]) {
                    misses++;
                    SDL_Log("    element %u: selector \"%s\" matches but is not a candidate", e,
                            k_sels[k]);
                }
            }
        }
    }
    EMB_CHECK_EQ_INT(misses, 0);
    EMB_CHECK(total_matches > 20);
    /* The buckets must prune: fewer candidates than selectors times elements. */
    EMB_CHECK(total_candidates < N_SELS * nel);

    /* A type bucket only returns selectors keyed by that type or by something the element has. */
    memset(seen, 0, sizeof seen);
    cand lic = {seen, N_SELS, 0};
    embi_sel_index_candidates(&ix, t.li1, mark, &lic);
    EMB_CHECK(!seen[1]); /* "p" is keyed by the p type */
    EMB_CHECK(seen[4]);  /* "li" */

    embi_sel_ctx_free(&ctx);
    embi_sel_index_free(&ix);
    embi_arena_free(&a);
    mf_free(&t);
}

static void count_visit(void *user, uint64_t payload)
{
    uint32_t *counts = user;
    if (payload < 5) {
        counts[payload]++;
    }
}

/* Case variants of one class or attribute name share a lowercase bucket. Each payload must be
 * visited exactly once, not once per variant. */
EMB_TEST(index_visits_case_variant_names_once)
{
    mf_tree t = mf_build();
    embi_element *p = mf_el(t.doc, "p");
    mf_attr(p, "class", "Foo foo FOO");
    mf_attr(p, "data-x", "1");
    mf_attr(p, "Data-X", "2");
    mf_app(&t.body->base, &p->base);

    static const char *const sels[5] = {".foo", ".FOO", "p.FoO", "p", "[data-x]"};
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_sel_index ix = {0};
    for (uint32_t k = 0; k < 5; k++) {
        const embi_sel_list *l = embi_sel_parse_text(&a, ms(sels[k]), nullptr);
        EMB_REQUIRE(l != nullptr);
        embi_sel_index_add(&ix, &l->items[0], k);
    }

    uint32_t counts[5] = {0, 0, 0, 0, 0};
    embi_sel_index_candidates(&ix, p, count_visit, counts);
    for (uint32_t k = 0; k < 5; k++) {
        if (counts[k] != 1) {
            SDL_Log("    selector \"%s\" visited %u times", sels[k], counts[k]);
        }
        EMB_CHECK_EQ_INT(counts[k], 1);
    }

    embi_sel_index_free(&ix);
    embi_arena_free(&a);
    mf_free(&t);
}

EMB_TEST_MAIN()
