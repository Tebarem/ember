/*
 * Box tree construction (W3a): display mapping, anonymous block wrapping, whitespace-only text, and
 * memory across relayouts.
 */
#include "layout_harness.h"

#include "layout/layout_int.h"

static const embi_box *box_of(embi_element *el)
{
    return el != nullptr ? (const embi_box *)el->box : nullptr;
}

EMB_TEST(box_display_maps_to_box_kinds)
{
    lh h = lh_make("<body><div id=b></div><span id=s>x</span><div id=f style='display:flex'></div>"
                   "<div id=n style='display:none'></div></body>",
                   "body { margin: 0 }");
    const embi_box *b = box_of(lh_id(&h, "b"));
    const embi_box *s = box_of(lh_id(&h, "s"));
    const embi_box *f = box_of(lh_id(&h, "f"));
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(s != nullptr);
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK_EQ_INT(b->kind, EMBI_BK_BLOCK);
    EMB_CHECK_EQ_INT(s->kind, EMBI_BK_INLINE);
    EMB_CHECK((s->flags & EMBI_BF_INLINE_LEVEL) != 0);
    EMB_CHECK_EQ_INT(f->kind, EMBI_BK_FLEX);
    EMB_CHECK(box_of(lh_id(&h, "n")) == nullptr);
    lh_free(&h);
}

EMB_TEST(box_inline_run_between_blocks_is_wrapped_in_anonymous_block)
{
    lh h = lh_make("<body><div id=d>text<p id=p>para</p>more</div></body>", "body { margin: 0 }");
    const embi_box *d = box_of(lh_id(&h, "d"));
    EMB_REQUIRE(d != nullptr);
    const embi_box *a = d->first;
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK_EQ_INT(a->kind, EMBI_BK_BLOCK);
    EMB_CHECK((a->flags & EMBI_BF_ANON) != 0);
    EMB_CHECK((a->flags & EMBI_BF_IFC) != 0);
    const embi_box *p = box_of(lh_id(&h, "p"));
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(a->next == p);
    EMB_CHECK((p->flags & EMBI_BF_ANON) == 0);
    const embi_box *c = p->next;
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK((c->flags & EMBI_BF_ANON) != 0);
    EMB_CHECK(c->next == nullptr);
    lh_free(&h);
}

EMB_TEST(box_whitespace_between_blocks_creates_no_boxes)
{
    lh h = lh_make("<body><div id=d>\n  <p>a</p>\n  </div></body>", "body { margin: 0 }");
    const embi_box *d = box_of(lh_id(&h, "d"));
    EMB_REQUIRE(d != nullptr);
    uint32_t n = 0;
    for (const embi_box *k = d->first; k != nullptr; k = k->next) {
        n++;
    }
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK((d->first->flags & EMBI_BF_ANON) == 0);
    lh_free(&h);
}

EMB_TEST(box_relayout_does_not_grow_live_memory)
{
    lh h = lh_make("<body><div id=d><p>one</p><p>two <span>three</span></p></div></body>",
                   "body { margin: 0 } p { margin: 4px 0 }");
    lh_relayout(&h);
    lh_relayout(&h);
    /* A relayout frees the previous box and fragment arenas as a whole, so the live bytes after
     * further layouts are exactly what they were after the first ones. */
    size_t before = embi_mem_live();
    for (int i = 0; i < 20; i++) {
        lh_relayout(&h);
    }
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)before);
    lh_free(&h);
}

EMB_TEST(box_harness_lifecycle_returns_to_the_baseline)
{
    /* The first harness warms the global caches (atoms, font database); the later ones must not
     * leave anything behind. */
    {
        lh w = lh_make("<body><p>warm</p></body>", "");
        lh_free(&w);
    }
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        lh h = lh_make("<body><ol><li>a<li>b</ol><div style='float:left;width:50%'>x</div>"
                       "<table><tr><td>c</table><input><select></select></body>",
                       "li::before { content: counter(list-item) '. ' }");
        lh_relayout(&h);
        lh_free(&h);
    }
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST_MAIN()
