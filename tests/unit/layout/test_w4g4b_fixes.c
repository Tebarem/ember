/*
 * Regression tests for the layout fixes of the second and third rounds of the css-flexbox/css-sizing/css-lists/css-overflow/
 * css-content/css-tables burn-down (W4g4b): counter scope per CSS Lists 3, georgian and armenian counter styles, `quotes: auto`
 * per language, kerning across inline box boundaries, text-overflow strings (segment breaks and the per-character fallback of
 * the marker), fit-content() and transferred sizes of aspect-ratio boxes, table fixes (calc() percentages, white space in
 * anonymous cells, max-height, th alignment) and the scrollable overflow of lines. Expected values are worked out by hand
 * from the specs.
 */
#include "layout_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

/* True when the text of the ::before (or ::after) box of the element with the id is want; a mismatch is printed. */
static bool pseudo_is(lh *h, const char *id, uint8_t pe, const char *want)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    lh_pseudo_text(lh_root_box(h), el, pe, &sb);
    bool ok = sb.len == strlen(want) && (sb.len == 0 || memcmp(sb.data, want, sb.len) == 0);
    if (!ok) {
        fprintf(stderr, "    %s: got \"%.*s\", want \"%s\"\n", id, (int)sb.len, sb.data != nullptr ? sb.data : "", want);
    }
    embi_sb_free(&sb);
    return ok;
}

static bool fmt_is(int32_t v, uint16_t kw, const char *want)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_ctr_format(&sb, v, kw);
    bool ok = sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return ok;
}

EMB_TEST(counter_created_by_a_sibling_is_not_inherited_when_the_parent_has_one)
{
    /* counter-001: the span that resets c to 98 shows 99, and the spans after it go on with the parent's counter. */
    lh h = lh_make("<body><div id=t><span id=a></span><span id=b></span><span id=r></span><span id=z></span></div></body>",
                   "#t{counter-reset:c} span{counter-increment:c} span::before{content:counter(c)}"
                   "#r{counter-reset:c 98}");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_BEFORE, "1"));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_BEFORE, "2"));
    EMB_CHECK(pseudo_is(&h, "r", EMBI_PE_BEFORE, "99"));
    EMB_CHECK(pseudo_is(&h, "z", EMBI_PE_BEFORE, "3"));
    lh_free(&h);
}

EMB_TEST(counter_created_by_a_sibling_stays_without_an_ancestor_counter)
{
    lh h = lh_make("<body><div><span id=r></span><span id=z></span></div></body>",
                   "#r{counter-reset:c 5} span{counter-increment:c} span::before{content:counter(c)}");
    EMB_CHECK(pseudo_is(&h, "r", EMBI_PE_BEFORE, "6"));
    EMB_CHECK(pseudo_is(&h, "z", EMBI_PE_BEFORE, "7"));
    lh_free(&h);
}

EMB_TEST(display_contents_element_does_not_set_counters)
{
    lh h = lh_make("<body><div><span id=a></span><span id=k></span><span id=z></span></div></body>",
                   "span::before{content:counter(c)} #a{counter-reset:c 6} #k{display:contents;counter-reset:c 666}"
                   "#z{counter-increment:c}");
    EMB_CHECK(pseudo_is(&h, "z", EMBI_PE_BEFORE, "7"));
    lh_free(&h);
}

EMB_TEST(georgian_and_armenian_counter_styles)
{
    EMB_CHECK(fmt_is(1, EMBI_KW_georgian, "\xE1\x83\x90"));
    EMB_CHECK(fmt_is(10, EMBI_KW_georgian, "\xE1\x83\x98"));
    EMB_CHECK(fmt_is(100, EMBI_KW_georgian, "\xE1\x83\xA0"));
    EMB_CHECK(fmt_is(1, EMBI_KW_armenian, "\xD4\xB1"));
    EMB_CHECK(fmt_is(10, EMBI_KW_armenian, "\xD4\xBA"));
    EMB_CHECK(fmt_is(11, EMBI_KW_armenian, "\xD4\xBA\xD4\xB1"));
    EMB_CHECK(fmt_is(10000, EMBI_KW_armenian, "10000"));
    EMB_CHECK(fmt_is(0, EMBI_KW_georgian, "0"));
}

EMB_TEST(quotes_auto_follow_the_language_and_repeat_the_last_pair_below_two_levels)
{
    lh h = lh_make("<html lang=fr><body><q id=a>x<q id=b>y<q id=c>z<q id=d>w</q></q></q></q></body></html>", "");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_AFTER, "\xC2\xBB"));
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_AFTER, "\xC2\xBB"));
    lh_free(&h);
    lh h2 = lh_make("<html lang=ja><body><q id=a>x<q id=b>y</q></q></body></html>", "");
    EMB_CHECK(pseudo_is(&h2, "a", EMBI_PE_BEFORE, "\xE3\x80\x8C"));
    EMB_CHECK(pseudo_is(&h2, "b", EMBI_PE_BEFORE, "\xE3\x80\x8E"));
    lh_free(&h2);
    /* quotes-030: the quotes of a q come from its parent's language, and Japanese repeats its second pair. */
    lh h4 = lh_make("<body><q id=a>x<q id=b lang=ja>y<q id=c lang=fr>z</q></q></q></body>", "q{quotes:auto}");
    EMB_CHECK(pseudo_is(&h4, "a", EMBI_PE_BEFORE, "\xE2\x80\x9C"));
    EMB_CHECK(pseudo_is(&h4, "b", EMBI_PE_BEFORE, "\xE2\x80\x98"));
    EMB_CHECK(pseudo_is(&h4, "c", EMBI_PE_BEFORE, "\xE3\x80\x8E"));
    EMB_CHECK(pseudo_is(&h4, "c", EMBI_PE_AFTER, "\xE3\x80\x8F"));
    lh_free(&h4);
    /* quotes-match-parent-001: match-parent takes the parent's auto as resolved for the parent (French here). */
    lh h5 = lh_make("<body><div lang=fr><q id=a lang=de>x<q id=b lang=sv>y<q id=c lang=en>z</q></q></q></div></body>",
                    "q{quotes:auto} q q{quotes:match-parent}");
    EMB_CHECK(pseudo_is(&h5, "a", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h5, "b", EMBI_PE_BEFORE, "\xC2\xAB"));
    EMB_CHECK(pseudo_is(&h5, "c", EMBI_PE_AFTER, "\xC2\xBB"));
    lh_free(&h5);
    lh h3 = lh_make("<body><q id=a>x<q id=b>y</q></q></body>", "");
    EMB_CHECK(pseudo_is(&h3, "a", EMBI_PE_BEFORE, "\xE2\x80\x9C"));
    EMB_CHECK(pseudo_is(&h3, "b", EMBI_PE_BEFORE, "\xE2\x80\x98"));
    lh_free(&h3);
}

EMB_TEST(explicit_quotes_list_keeps_more_than_two_pairs)
{
    lh h = lh_make("<body><q id=a>1<q id=b>2<q id=c>3</q></q></q></body>",
                   "q{quotes:'<' '>' '[' ']' '{' '}'}");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_BEFORE, "<"));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_BEFORE, "["));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_BEFORE, "{"));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_AFTER, "}"));
    lh_free(&h);
}

EMB_TEST(fit_content_function_is_the_contribution_of_a_box_in_a_min_content_parent)
{
    /* width: fit-content(100px) of a box with two 60px inline-blocks: min(max-content 120, max(min-content 60, 100)) = 100. */
    lh h = lh_make("<body><div id=o style='width:min-content'><div id=i style='width:fit-content(100px)'>"
                   "<div style='display:inline-block;width:60px'></div> <div style='display:inline-block;width:60px'></div>"
                   "</div></div></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "o", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h);
}

EMB_TEST(fit_content_percentage_in_a_max_width_is_cyclic_and_means_none)
{
    /* max-width: fit-content(50%) acts as none for the contribution, then clamps to the max-content size (100px). The two
     * 50px inline-blocks follow each other with no white space between them. */
    lh h = lh_make("<body><div id=o style='width:min-content'><div style='width:200px;max-width:fit-content(50%)'>"
                   "<div style='display:inline-block;width:50px'></div><div style='display:inline-block;width:50px'></div>"
                   "</div></div></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "o", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h);
}

EMB_TEST(aspect_ratio_block_takes_its_max_height_as_a_transferred_max_width)
{
    lh h = lh_make("<body><div id=a style='aspect-ratio:1/1;max-height:100px;background:green'></div></body>", "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(near(r.w, 100.0f));
    EMB_CHECK(near(r.h, 100.0f));
    lh_free(&h);
}

EMB_TEST(aspect_ratio_block_min_width_auto_is_the_content_size)
{
    lh h = lh_make("<body><div id=a style='height:100px;aspect-ratio:1/2'><div style='width:100px'></div></div></body>", "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h);
    lh h2 = lh_make("<body><div id=a style='height:100px;aspect-ratio:2/1;max-width:100px'><div style='width:200px'></div></div></body>", "");
    EMB_REQUIRE(rect_of(&h2, "a", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h2);
}

EMB_TEST(height_stretch_fills_the_containing_block_and_ignores_margins)
{
    lh h = lh_make("<body><div style='width:200px;height:200px'><div id=a style='height:stretch;margin:10px 0 20px;border:2px solid'></div></div></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(near(r.h, 200.0f));
    lh_free(&h);
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

/* The text and the width of the text-overflow marker that the line of block id ends with (the block's last TEXT fragment).
 * The text is copied into out (NUL-terminated). */
static bool marker_of(lh *h, const char *id, char *out, size_t cap, float *w)
{
    embi_element *el = lh_id(h, id);
    const embi_frag *root = embi_layout_root(h->eng, h->doc);
    const embi_frag *bf = el != nullptr && root != nullptr ? frag_of(root, el) : nullptr;
    const embi_frag *t = bf != nullptr ? last_text(bf) : nullptr;
    if (t == nullptr || t->box == nullptr || t->box->text == nullptr || (size_t)t->box->text_len + 1 > cap) {
        return false;
    }
    memcpy(out, t->box->text, t->box->text_len);
    out[t->box->text_len] = '\0';
    *w = t->w;
    return true;
}

#define OVERFLOW_BOX "width:10px;overflow:hidden;white-space:nowrap;font:16px sans-serif;"

EMB_TEST(text_overflow_string_turns_a_line_feed_between_narrow_characters_into_a_space)
{
    /* "a\0Ab" is a, a line feed, b: the segment break collapses to a space (CSS Text 3 section 4.1.2), so the marker is
     * "a b" and exactly as wide as the text "a b" in the same font. */
    lh h = lh_make("<body><div id=a>xxxxxxxxxxxxxxxx</div><span id=r>a b</span></body>",
                   "#a{" OVERFLOW_BOX "text-overflow:\"a\\00000Ab\"}#r{font:16px sans-serif;display:inline-block}");
    char txt[64];
    float w = 0;
    EMB_REQUIRE(marker_of(&h, "a", txt, sizeof txt, &w));
    EMB_CHECK(strcmp(txt, "a b") == 0);
    embi_rectf rr;
    EMB_REQUIRE(rect_of(&h, "r", &rr));
    EMB_CHECK(w > 0.0f);
    EMB_CHECK(near(w, rr.w));
    lh_free(&h);
}

EMB_TEST(text_overflow_string_drops_a_line_feed_between_wide_characters)
{
    /* U+4F60, a line feed, U+597D: between two wide characters the break is removed, so the marker is the two characters
     * with nothing between and as wide as the same two characters as normal text. */
    lh h = lh_make("<body><div id=a>xxxxxxxxxxxxxxxx</div><span id=r>\xE4\xBD\xA0\xE5\xA5\xBD</span></body>",
                   "#a{" OVERFLOW_BOX "text-overflow:\"\\4F60\\0A\\597D\"}#r{font:16px sans-serif;display:inline-block}");
    char txt[64];
    float w = 0;
    EMB_REQUIRE(marker_of(&h, "a", txt, sizeof txt, &w));
    EMB_CHECK(strcmp(txt, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
    embi_rectf rr;
    EMB_REQUIRE(rect_of(&h, "r", &rr));
    EMB_CHECK(w > 0.0f);
    EMB_CHECK(near(w, rr.w));
    lh_free(&h);
    /* Between a wide and a narrow character the line feed is a space. */
    lh h2 = lh_make("<body><div id=a>xxxxxxxxxxxxxxxx</div></body>", "#a{" OVERFLOW_BOX "text-overflow:\"\\4F60\\00000Ab\"}");
    EMB_REQUIRE(marker_of(&h2, "a", txt, sizeof txt, &w));
    EMB_CHECK(strcmp(txt, "\xE4\xBD\xA0 b") == 0);
    lh_free(&h2);
}

EMB_TEST(text_overflow_string_characters_use_the_per_character_fallback_of_normal_text)
{
    /* The marker is shaped like other text of the block: a character the block's family lacks comes from the fallback (W4i),
     * so the Hebrew letter alef in a block of Ahem (no Hebrew) is as wide as the letter as text of the same font. */
    lh h = lh_make("<body><div id=a>xxxxxxxxxxxxxxxx</div><span id=r>\xD7\x90</span></body>",
                   "#a{" OVERFLOW_BOX "font-family:Ahem;text-overflow:\"\\5D0\"}#r{font:16px Ahem;display:inline-block}");
    char txt[64];
    float w = 0;
    EMB_REQUIRE(marker_of(&h, "a", txt, sizeof txt, &w));
    EMB_CHECK(strcmp(txt, "\xD7\x90") == 0);
    embi_rectf rr;
    EMB_REQUIRE(rect_of(&h, "r", &rr));
    EMB_CHECK(w > 0.0f);
    EMB_CHECK(near(w, rr.w));
    lh_free(&h);
}

EMB_TEST(kerning_crosses_the_boundary_between_text_and_a_generated_quote)
{
    lh h = lh_make("<body><p id=a style='font:32px serif;white-space:nowrap;display:inline-block'>the &#x201c;outer</p><br>"
                   "<p id=b style='font:32px serif;white-space:nowrap;display:inline-block'>the <span>&#x201c;</span>outer</p></body>",
                   "");
    embi_rectf ra, rb;
    EMB_REQUIRE(rect_of(&h, "a", &ra));
    EMB_REQUIRE(rect_of(&h, "b", &rb));
    EMB_CHECK(near(ra.w, rb.w));
    lh_free(&h);
}

EMB_TEST(table_width_calc_of_a_percentage_is_that_percentage)
{
    /* calc(50% + 0px) is calc(50%): a percentage column, so with a 100px column the table is 200px wide. */
    lh h = lh_make("<body><table id=t style='border-spacing:0;table-layout:auto'><tr>"
                   "<td style='width:calc(50% + 0px);padding:0'></td><td style='width:100px;padding:0'></td></tr></table></body>", "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(near(r.w, 200.0f));
    lh_free(&h);
}

EMB_TEST(only_the_first_thead_and_tfoot_are_hoisted)
{
    /* CSS 2.1 section 17.2.1: the other header and footer groups are row groups in document order. */
    lh h = lh_make("<body><table style='border-spacing:0'><tbody><tr><td id=b1 style='height:10px;padding:0'></td></tr></tbody>"
                   "<thead><tr><td id=h1 style='height:10px;padding:0'></td></tr></thead>"
                   "<thead><tr><td id=h2 style='height:10px;padding:0'></td></tr></thead></table></body>", "");
    embi_rectf a, b, c;
    EMB_REQUIRE(rect_of(&h, "h1", &a));
    EMB_REQUIRE(rect_of(&h, "b1", &b));
    EMB_REQUIRE(rect_of(&h, "h2", &c));
    EMB_CHECK(a.y < b.y && b.y < c.y);
    lh_free(&h);
}

EMB_TEST(nested_row_group_joins_the_outer_group_and_cells_share_a_row)
{
    lh h = lh_make("<body><div style='display:table-row-group'><div style='display:table-row-group'>"
                   "<div id=a style='display:table-cell'>a</div></div><div id=b style='display:table-cell'>b</div></div></body>", "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.y, b.y) && b.x > a.x);
    lh_free(&h);
}

EMB_TEST(table_cell_ignores_max_height_and_border_widths_snap)
{
    lh h = lh_make("<body><table style='border-spacing:0'><tr><td id=c style='padding:0;height:20px;max-height:20px'>"
                   "<div style='height:50px'></div></td></tr></table>"
                   "<div id=d style='border:0.4px solid;width:10px;height:10px'></div></body>", "");
    embi_rectf c, d;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.h, 50.0f));
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.w, 12.0f)); /* 0.4px snaps up to 1px */
    lh_free(&h);
}

EMB_TEST(li_value_applies_after_the_increments_of_the_same_item)
{
    /* CSS Lists 3 section 4.4: an element's counter-increment applies before its counter-set, and HTML maps value= to
     * counter-set: list-item. So b shows 10 whatever it increments by, and c goes on from there. */
    size_t base = embi_mem_live();
    lh h = lh_make("<body><ol><li id=a>x<li id=b value=10 style='counter-increment:list-item 5'>y"
                   "<li id=c style='counter-increment:list-item 5'>z<li id=d value=-3>w<li id=e>v</ol></body>", "");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "1. "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "10. "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "15. "));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_MARKER, "-3. "));
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_MARKER, "-2. "));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(counters_saturate_at_plus_and_minus_2_1e9)
{
    /* Counters saturate at +/-2.1e9 (CSS Lists 3 lets the UA pick the range; WPT counter-reset-increment-overflow-underflow
     * expects 2.1e9). A start or value attribute outside the 32-bit range is a parse error (HTML), so it is ignored. */
    size_t base = embi_mem_live();
    lh h = lh_make("<body><ol start=2099999999><li id=a>x<li id=b>y<li id=c>z</ol>"
                   "<ol><li id=d value=2000000000 style='counter-increment:list-item 2000000000'>w"
                   "<li id=e style='counter-increment:list-item 2000000000'>v<li id=f value=-2099999999>u"
                   "<li id=g style='counter-increment:list-item -5'>t</ol>"
                   "<ol start=99999999999><li id=h>s</ol><ol reversed start=2147483647><li id=i>r</ol>"
                   "<div style='counter-reset:c -2000000000'><span id=n></span><span id=m></span></div>"
                   "<div style='counter-reset:k 99999999999'><b id=k></b></div></body>",
                   "span{counter-increment:c -2000000000} span::before{content:counter(c)} b::before{content:counter(k)}");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "2099999999. "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "2100000000. "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "2100000000. "));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_MARKER, "2000000000. "));
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_MARKER, "2100000000. "));
    EMB_CHECK(pseudo_is(&h, "f", EMBI_PE_MARKER, "-2099999999. "));
    EMB_CHECK(pseudo_is(&h, "g", EMBI_PE_MARKER, "-2100000000. "));
    EMB_CHECK(pseudo_is(&h, "h", EMBI_PE_MARKER, "1. "));
    EMB_CHECK(pseudo_is(&h, "i", EMBI_PE_MARKER, "2099999999. ")); /* the reset is 2.1e9, then the item counts down */
    EMB_CHECK(pseudo_is(&h, "n", EMBI_PE_BEFORE, "-2100000000"));
    EMB_CHECK(pseudo_is(&h, "m", EMBI_PE_BEFORE, "-2100000000"));
    EMB_CHECK(pseudo_is(&h, "k", EMBI_PE_BEFORE, "2100000000"));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(reversed_list_starts_from_the_negated_increments_plus_the_last_non_zero_one)
{
    /* CSS Lists 3 section 4.4.2 (li-value-reversed-006c): the initial value is the sum of the negated increments plus the
     * last non-zero negated increment, so the items show 17, 16 and 8; an explicit zero increment at the end does not
     * count as the last one (2, 1, 1 rather than 1, 0, 0). */
    size_t base = embi_mem_live();
    lh h = lh_make("<body><ol reversed><li id=a style='counter-increment:list-item -3'>x"
                   "<div style='counter-increment:list-item 0'></div>"
                   "<li id=b style='counter-increment:list-item -1'>y<li id=c style='counter-increment:list-item -8'>z</ol>"
                   "<ol reversed><li id=d style='counter-increment:list-item -3'>x<li id=e>y"
                   "<li id=f style='counter-increment:list-item 0'>z</ol></body>", "");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "17. "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "16. "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "8. "));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_MARKER, "2. "));
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_MARKER, "1. "));
    EMB_CHECK(pseudo_is(&h, "f", EMBI_PE_MARKER, "1. "));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(reversed_list_stops_at_a_counter_set_and_nested_lists_have_their_own_scope)
{
    /* li-value-reversed-019: the outer list counts Three, the div and One (initial 4); the inner list stops at the div that
     * sets 3, so it starts at 1 + 3 + 1 = 5 and shows Four and Two. A value= item counts as a set too. */
    size_t base = embi_mem_live();
    lh h = lh_make("<body><ol reversed><li id=a>Three<div style='counter-increment:list-item -1'></div>"
                   "<ol reversed><li id=b>Four<div style='counter-set:list-item 3'></div><li id=c>Two</ol>"
                   "<li id=d>One</ol>"
                   "<ol reversed><li id=e>x<li id=f>y<li id=g value=7>z<li id=h>w</ol></body>", "");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "3. "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "4. "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "2. "));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_MARKER, "1. "));
    /* e and f: num = 1 + 1, then g sets 7 and ends the walk: 9, plus the last non-zero 1 = 10. */
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_MARKER, "9. "));
    EMB_CHECK(pseudo_is(&h, "f", EMBI_PE_MARKER, "8. "));
    EMB_CHECK(pseudo_is(&h, "g", EMBI_PE_MARKER, "7. "));
    EMB_CHECK(pseudo_is(&h, "h", EMBI_PE_MARKER, "6. "));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(counter_style_rule_extends_a_system_with_prefix_and_suffix)
{
    size_t base = embi_mem_live();
    lh h = lh_make("<body><ol><li id=a>x<li id=b>y</ol><ul><li id=c>z</ul></body>",
                   "@counter-style paren{system:extends decimal;prefix:'(';suffix:') '}"
                   "@counter-style brack{system:extends paren;prefix:'['} ol{list-style-type:paren} ul{list-style-type:brack}");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "(1) "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "(2) "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "[1) "));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

static uint16_t display_of(lh *h, const char *id)
{
    embi_element *el = lh_id(h, id);
    const embi_style *s = el != nullptr ? embi_style_get(h->style, el, EMBI_PE_NONE) : nullptr;
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_display) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

EMB_TEST(multi_keyword_display_values_map_to_their_short_forms)
{
    size_t base = embi_mem_live();
    lh h = lh_make("<body><div id=a style='display:block flex'></div><span id=b style='display:inline flex'></span>"
                   "<span id=c style='display:flow-root inline'></span><div id=d style='display:list-item block flow'></div>"
                   "<span id=e style='display:inline list-item'></span><span id=f style='display:inline flow-root list-item'></span>"
                   "<div id=g style='display:flow-root list-item'></div><div id=h style='display:block run-in'></div>"
                   "<div id=i style='display:table block'></div><div id=j style='display:block flex grid'></div></body>", "");
    EMB_CHECK_EQ_INT(display_of(&h, "a"), EMBI_KW_flex);
    EMB_CHECK_EQ_INT(display_of(&h, "b"), EMBI_KW_inline_flex);
    EMB_CHECK_EQ_INT(display_of(&h, "c"), EMBI_KW_inline_block);
    EMB_CHECK_EQ_INT(display_of(&h, "d"), EMBI_KW_list_item);
    EMB_CHECK_EQ_INT(display_of(&h, "e"), EMBI_KW__internal_inline_list_item);
    EMB_CHECK_EQ_INT(display_of(&h, "f"), EMBI_KW__internal_inline_flow_root_list_item);
    EMB_CHECK_EQ_INT(display_of(&h, "g"), EMBI_KW__internal_flow_root_list_item);
    EMB_CHECK_EQ_INT(display_of(&h, "h"), EMBI_KW_block); /* run-in is not supported: the declaration is dropped */
    EMB_CHECK_EQ_INT(display_of(&h, "i"), EMBI_KW_table); /* the keywords may come in either order */
    EMB_CHECK_EQ_INT(display_of(&h, "j"), EMBI_KW_block); /* two inner keywords: dropped */
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(inline_list_item_has_an_inside_marker_and_flow_start_aligns_like_flex_start)
{
    size_t base = embi_mem_live();
    lh h = lh_make("<body><div style='width:200px'><span id=a style='display:inline list-item;list-style-type:decimal'>x</span></div>"
                   "<div style='display:flex;width:100px;justify-content:flow-end'><div id=b style='width:10px;height:10px'></div></div>"
                   "<div style='display:flex;width:100px;justify-content:flow-start'><div id=c style='width:10px;height:10px'></div></div>"
                   "</body>", "");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "1.\xC2\xA0"));
    embi_rectf b, c;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(b.x - c.x, 90.0f));
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(table_whitespace_between_boxes_of_an_anonymous_cell_is_kept)
{
    /* Two inline-blocks in a table box share one anonymous cell, and the space between them is a space. */
    lh h = lh_make("<body><div id=t style='display:table;font:16px/1 monospace'><span style='display:inline-block;width:10px'></span> "
                   "<span style='display:inline-block;width:10px'></span></div></body>", "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(r.w > 20.5f);
    lh_free(&h);
}

EMB_TEST(table_max_height_does_not_shrink_the_table_under_its_rows)
{
    lh h = lh_make("<body><table id=t style='border-spacing:0;max-height:0'><tr><td style='padding:0'><div style='width:5px;height:5px'></div></td></tr></table></body>", "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(near(r.h, 5.0f));
    lh_free(&h);
}

EMB_TEST(th_is_centered_unless_the_table_sets_text_align)
{
    lh h = lh_make("<body><table style='border-spacing:0'><tr><th id=a>A</th></tr></table>"
                   "<table style='border-spacing:0;text-align:end'><tr><th id=b>A</th></tr></table></body>", "");
    embi_element *a = lh_id(&h, "a");
    embi_element *b = lh_id(&h, "b");
    EMB_REQUIRE(a != nullptr && b != nullptr);
    const embi_style *sa = embi_style_get(h.style, a, EMBI_PE_NONE);
    const embi_style *sb = embi_style_get(h.style, b, EMBI_PE_NONE);
    EMB_REQUIRE(sa != nullptr && sb != nullptr);
    const embi_value *va = embi_style_value(sa, EMBI_PROP_text_align);
    const embi_value *vb = embi_style_value(sb, EMBI_PROP_text_align);
    EMB_CHECK(va->kind == EMBI_V_KEYWORD && va->kw == EMBI_KW_center);
    EMB_CHECK(vb->kind == EMBI_V_KEYWORD && vb->kw == EMBI_KW_end);
    lh_free(&h);
}

EMB_TEST(a_nowrap_line_wider_than_its_scroll_container_is_scrollable_overflow)
{
    /* The inline-block on the line is 300px wide in a 100px container, so scrollWidth is 300. */
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;overflow-y:hidden;white-space:nowrap'>"
                   "<div style='display:inline-block;width:300px;height:100%'></div></div></body>", "");
    embi_element *a = lh_id(&h, "a");
    EMB_REQUIRE(a != nullptr);
    float w = 0, hh = 0;
    EMB_REQUIRE(embi_layout_scroll_size(h.eng, a, &w, &hh));
    EMB_CHECK(near(w, 300.0f));
    lh_free(&h);
}

EMB_TEST_MAIN()
