/*
 * Layout fixes of the BUGBURN2 reftest burn-down. Expected values are worked out by hand from the specs; each test names
 * the reftest that found the bug.
 */
#include "layout_harness.h"

#include <math.h>
#include <string.h>

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}


/* The fragment of the principal box of element el, found from the root of the layout, or nullptr. */
static const embi_frag *frag_of(const embi_frag *f, const embi_element *el)
{
    if (f->kind == EMBI_FK_BOX && f->box != nullptr && f->box->el == el && f->box->pe == 0) {
        return f;
    }
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        const embi_frag *r = frag_of(k, el);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

/* The last TEXT fragment below f in tree order. */
static const embi_frag *last_text(const embi_frag *f)
{
    const embi_frag *best = f->kind == EMBI_FK_TEXT ? f : nullptr;
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        const embi_frag *r = last_text(k);
        if (r != nullptr) {
            best = r;
        }
    }
    return best;
}

/* bidi-box-model-014: the physical margin-left of an rtl inline box (unicode-bidi: normal) around LTR text sits to the left
 * of its text, between "First " and "Second", exactly as in an ltr block; the end edge does not go to the line's end. */
EMB_TEST(rtl_inline_box_with_ltr_text_keeps_its_left_margin_beside_the_text)
{
    lh h = lh_make("<div id=a style=\"font:20px sans-serif;width:400px;text-align:left\"><i id=f>First</i> <span id=s style=\"margin-left:40px\">Second</span></div>"
                   "<div id=b style=\"font:20px sans-serif;width:400px;text-align:left;direction:rtl\"><i id=g>First</i> <span id=t style=\"margin-left:40px\">Second</span></div>",
                   "");
    embi_rectf a, b, s, t, f, g;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "b", &b) && rect_of(&h, "s", &s) && rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "f", &f) && rect_of(&h, "g", &g));
    EMB_CHECK(near(g.x - b.x, f.x - a.x)); /* "First" starts the line; the margin is not before it */
    EMB_CHECK(near(t.x - b.x, s.x - a.x));
    EMB_CHECK(near(t.w, s.w));
    lh_free(&h);
}

/* bidi-box-model-038: the same for padding-right in a right-aligned line. */
EMB_TEST(rtl_inline_box_with_ltr_text_keeps_its_right_padding_beside_the_text)
{
    lh h = lh_make("<div id=a style=\"font:20px sans-serif;width:400px;text-align:right\"><i id=f>First</i> <span id=s style=\"padding-right:40px\"><i id=u>Second</i></span></div>"
                   "<div id=b style=\"font:20px sans-serif;width:400px;text-align:right;direction:rtl\"><i id=g>First</i> <span id=t style=\"padding-right:40px\"><i id=v>Second</i></span></div>",
                   "");
    embi_rectf a, b, s, t, f, g, u, v;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "b", &b) && rect_of(&h, "s", &s) && rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "f", &f) && rect_of(&h, "g", &g) && rect_of(&h, "u", &u) && rect_of(&h, "v", &v));
    EMB_CHECK(near(g.x - b.x, f.x - a.x));
    EMB_CHECK(near(v.x - t.x, u.x - s.x)); /* the padding is after the text, not before it */
    EMB_CHECK(near(t.x - b.x, s.x - a.x));
    EMB_CHECK(near(t.w, s.w));
    lh_free(&h);
}

/* rtl-linebreak: the end margin of an rtl box with LTR text that goes on from an earlier line (a continuation fragment
 * has only its end edge) is on the left of its text, whatever follows it on the line. */
EMB_TEST(end_margin_of_a_continuing_rtl_box_stays_left_of_its_ltr_text)
{
    lh h = lh_make("<div id=d style=\"font:20px sans-serif;width:400px;direction:rtl;text-align:left\">"
                   "<span style=\"margin-left:50px\">One<br><i id=t>Two</i></span> Next</div>",
                   "");
    embi_rectf d, t;
    EMB_REQUIRE(rect_of(&h, "d", &d) && rect_of(&h, "t", &t));
    EMB_CHECK(near(t.x - d.x, 50.0f));
    lh_free(&h);
}

/* baseline-block-with-overflow-001 and vertical-align-baseline-005a: an inline-block that clips its overflow has its bottom
 * margin edge as baseline even when its last line is higher (CSS 2.2 says "higher of", the browsers and these tests do not). */
EMB_TEST(clipping_inline_block_baseline_is_the_margin_edge)
{
    lh h = lh_make("<div style=\"font:20px/20px sans-serif\"><span id=a style=\"display:inline-block;overflow:hidden;height:60px\">X</span>"
                   "<span id=b>X</span></div>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "b", &b));
    EMB_CHECK(b.y > a.y + 30.0f); /* the text sits at the bottom of the 60px box */
    lh_free(&h);
}

/* text-overflow-in-anonymous-block: the inline content of a block container that also holds a block child sits in an anonymous
 * block box, whose lines are ellipsised by the container's text-overflow and overflow. */
EMB_TEST(text_overflow_ellipsises_the_lines_of_an_anonymous_block_box)
{
    lh h = lh_make("<div id=a style=\"overflow:hidden;text-overflow:ellipsis;width:100px;white-space:pre;font:16px sans-serif\">"
                   "this text should have an ellipsis<div style=\"height:0\"></div></div>",
                   "");
    embi_element *el = lh_id(&h, "a");
    const embi_frag *root = embi_layout_root(h.eng, h.doc);
    const embi_frag *bf = el != nullptr && root != nullptr ? frag_of(root, el) : nullptr;
    const embi_frag *t = bf != nullptr ? last_text(bf) : nullptr;
    EMB_REQUIRE(t != nullptr && t->box != nullptr && t->box->text != nullptr);
    EMB_CHECK(t->box->text_len == 3 && memcmp(t->box->text, "\342\200\246", 3) == 0); /* the ellipsis closes the line */
    lh_free(&h);
}

/* Whether a fragment of a ::marker box is at or below f. */
static bool has_marker(const embi_frag *f)
{
    if (f->box != nullptr && f->box->kind == EMBI_BK_MARKER) {
        return true;
    }
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        if (has_marker(k)) {
            return true;
        }
    }
    return false;
}

/* after-content-display-003: a generated box with display: list-item gets a marker like an element does. */
EMB_TEST(generated_list_item_has_a_marker)
{
    lh h = lh_make("<div id=a>text</div><div id=b></div>", "#a::after{content:\"gen\";display:list-item;margin-left:1em}"
                                                          "#b::after{content:\"gen\";display:block}");
    const embi_frag *root = embi_layout_root(h.eng, h.doc);
    const embi_frag *fa = frag_of(root, lh_id(&h, "a")), *fb = frag_of(root, lh_id(&h, "b"));
    EMB_REQUIRE(fa != nullptr && fb != nullptr);
    EMB_CHECK(has_marker(fa));
    EMB_CHECK(!has_marker(fb));
    lh_free(&h);
}

/* table-anonymous-objects-199: white space that follows the inline content of an anonymous table cell, up to the next table
 * part, belongs to the cell: with white-space: pre the cell holds " bc " and the next cell starts after the trailing space. */
EMB_TEST(preserved_white_space_after_an_anonymous_cell_stays_in_it)
{
    lh h = lh_make("<div style=\"display:table;white-space:pre;font:20px sans-serif\"><span style=\"display:table-row\">"
                   "<span id=a style=\"display:table-cell\">a</span> <span>bc</span> <span id=d style=\"display:table-cell\">d</span></span></div>"
                   "<span id=r style=\"white-space:pre;display:inline-block;font:20px sans-serif\"> bc </span>",
                   "");
    embi_rectf a, d, r;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "d", &d) && rect_of(&h, "r", &r));
    EMB_CHECK(near(d.x - (a.x + a.w), r.w));
    lh_free(&h);
}

/* table-anonymous-objects-089: a table cell whose first content is a nested (anonymous) table takes that table's first row
 * baseline, so a sibling cell with baseline alignment lines its text up with the nested table's text. */
EMB_TEST(cell_with_a_nested_table_has_its_first_row_baseline)
{
    lh h = lh_make("<div style=\"display:table;font:20px sans-serif\"><div style=\"display:table-row\">"
                   "<div id=a style=\"display:table-cell\"><div style=\"display:table\"><div style=\"display:table-row\">"
                   "<div style=\"display:table-cell\"><i id=n>n</i></div></div><div style=\"display:table-row\"><div style=\"display:table-cell\">m</div></div></div></div>"
                   "<div style=\"display:table-cell\"><i id=b>b</i></div></div></div>",
                   "");
    embi_rectf n, b;
    EMB_REQUIRE(rect_of(&h, "n", &n) && rect_of(&h, "b", &b));
    EMB_CHECK(near(n.y, b.y));
    lh_free(&h);
}

EMB_TEST_MAIN()
