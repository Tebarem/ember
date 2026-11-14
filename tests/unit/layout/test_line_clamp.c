/*
 * CSS Overflow 4 line-clamp in layout: counting lines, nested blocks, the legacy -webkit-box form, auto,
 * height limits and the block ellipsis. Lines are made with <br> and a 20px line-height so counts are heights.
 */
#include "layout_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static double height_of(lh *h, const char *id)
{
    embi_rectf r;
    embi_element *el = lh_id(h, id);
    if (el == nullptr || !embi_layout_border_box(h->eng, el, &r)) {
        return -1;
    }
    return r.h;
}

#define CSS "div{font:10px/20px sans-serif;width:300px}"
#define L5 "a<br>b<br>c<br>d<br>e"

EMB_TEST(unclamped_container_is_unchanged)
{
    lh h = lh_make("<div id=a>" L5 "</div>", CSS);
    EMB_CHECK(near(height_of(&h, "a"), 100));
    lh_free(&h);
}

EMB_TEST(max_lines_stops_after_n_lines)
{
    lh h = lh_make("<div id=a style='max-lines:3;continue:collapse'>" L5 "</div>", CSS);
    EMB_CHECK(near(height_of(&h, "a"), 60));
    lh_free(&h);
}

EMB_TEST(line_clamp_shorthand_clamps)
{
    lh h = lh_make("<div id=a style='line-clamp:2'>" L5 "</div><div id=b style='line-clamp:none'>" L5 "</div>", CSS);
    EMB_CHECK(near(height_of(&h, "a"), 40));
    EMB_CHECK(near(height_of(&h, "b"), 100));
    lh_free(&h);
}

EMB_TEST(clamp_larger_than_content_keeps_everything)
{
    lh h = lh_make("<div id=a style='line-clamp:9'>" L5 "</div>", CSS);
    EMB_CHECK(near(height_of(&h, "a"), 100));
    lh_free(&h);
}

EMB_TEST(lines_are_counted_across_nested_blocks)
{
    lh h = lh_make("<div id=a style='line-clamp:3'><p style='margin:0'>a<br>b</p><p style='margin:0'>c<br>d<br>e</p></div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "a"), 60));
    lh_free(&h);
}

EMB_TEST(clamped_nested_blocks_leave_no_later_content)
{
    lh h = lh_make("<div id=a style='line-clamp:2'><p id=p style='margin:0'>a<br>b</p><p id=q style='margin:0'>c<br>d</p></div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "a"), 40));
    EMB_CHECK(near(height_of(&h, "p"), 40));
    EMB_CHECK(height_of(&h, "q") <= 0); /* no box, or an empty one */
    lh_free(&h);
}

EMB_TEST(legacy_webkit_box_form_clamps)
{
    lh h = lh_make("<div id=a style='display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:2;overflow:hidden'>"
                   L5 "</div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "a"), 40));
    lh_free(&h);
}

EMB_TEST(webkit_box_without_vertical_orient_is_not_clamped_as_a_block)
{
    lh h = lh_make("<div id=a style='display:-webkit-box;-webkit-line-clamp:2'>" L5 "</div>", CSS);
    /* A horizontal -webkit-box is a flex container: one anonymous item holds all five lines. */
    EMB_CHECK(near(height_of(&h, "a"), 100));
    lh_free(&h);
}

EMB_TEST(auto_clamps_at_the_height_limit)
{
    lh h = lh_make("<div id=a style='line-clamp:auto;max-height:50px'>" L5 "</div>"
                   "<div id=b style='line-clamp:auto;height:70px'>" L5 "</div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "a"), 40)); /* two lines fit in 50px; the box is as high as they are */
    EMB_CHECK(near(height_of(&h, "b"), 70));
    lh_free(&h);
}

EMB_TEST(max_lines_and_auto_take_the_smaller_count)
{
    lh h = lh_make("<div id=a style='max-lines:4 auto;continue:collapse;max-height:50px'>" L5 "</div>", CSS);
    EMB_CHECK(h.eng != nullptr);
    /* Two lines fit in 50px, fewer than the four allowed; the box is as high as the kept lines. */
    EMB_CHECK(near(height_of(&h, "a"), 40));
    lh_free(&h);
}

EMB_TEST(block_ellipsis_ends_the_last_kept_line)
{
    lh h = lh_make("<div id=a style='line-clamp:1'>one two three four five six seven eight nine ten eleven twelve "
                   "thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty</div>"
                   "<div id=b style='max-lines:1;continue:collapse;block-ellipsis:none'>x<br>y</div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "a"), 20));
    EMB_CHECK(near(height_of(&h, "b"), 20));
    lh_free(&h);
}

EMB_TEST(clamp_does_not_touch_neighbours)
{
    lh h = lh_make("<div id=a style='line-clamp:1'>" L5 "</div><div id=b>" L5 "</div>", CSS);
    EMB_CHECK(near(height_of(&h, "b"), 100));
    EMB_CHECK(near(height_of(&h, "a"), 20));
    lh_free(&h);
}

/* The text of the fragment dump of the document (the ellipsis shows as a text piece of its line). */
static char *dump_of(lh *h, embi_sb *sb)
{
    embi_layout_dump(h->eng, h->doc, sb);
    return (char *)embi_sb_cstr(sb);
}

/* The x of the text piece in the dump that shows str (the dump line "text <> at (x, y) WxH "str""), or -1. */
static double text_x(const char *dump, const char *str)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", str);
    const char *hit = strstr(dump, pat);
    if (hit == nullptr) {
        return -1;
    }
    const char *ls = hit;
    while (ls > dump && ls[-1] != '\n') {
        ls--;
    }
    const char *at = strstr(ls, " at (");
    return at != nullptr && at < hit ? atof(at + 5) : -1;
}

static bool dump_has(lh *h, const char *str)
{
    embi_sb sb = {0};
    bool r = strstr(dump_of(h, &sb), str) != nullptr;
    embi_sb_free(&sb);
    return r;
}

static int count_text(const char *d, const char *str)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", str);
    int n = 0;
    for (const char *p = strstr(d, pat); p != nullptr; p = strstr(p + 1, pat)) {
        n++;
    }
    return n;
}

#define ELL "\xE2\x80\xA6"

EMB_TEST(ellipsis_ends_the_last_kept_line)
{
    lh h = lh_make("<div id=a style='line-clamp:2'>aaa<br>bbb<br>ccc</div>", CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    EMB_CHECK(strstr(d, "\"" ELL "\"") != nullptr);
    EMB_CHECK(strstr(d, "\"aaa\"") != nullptr);
    EMB_CHECK(strstr(d, "\"bbb\"") != nullptr);
    EMB_CHECK(strstr(d, "ccc") == nullptr);
    /* the ellipsis follows "bbb", which is on the second line */
    EMB_CHECK(text_x(d, ELL) > 0);
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(no_ellipsis_when_nothing_is_clamped_or_block_ellipsis_is_off)
{
    lh h = lh_make("<div id=a style='line-clamp:9'>aaa<br>bbb</div>"
                   "<div id=b style='max-lines:1;continue:collapse;block-ellipsis:no-ellipsis'>ddd<br>eee</div>",
                   CSS);
    EMB_CHECK(!dump_has(&h, ELL));
    lh_free(&h);
}

EMB_TEST(custom_ellipsis_string)
{
    lh h = lh_make("<div id=a style='line-clamp:1 \"[more]\"'>aaa<br>bbb</div>", CSS);
    EMB_CHECK(dump_has(&h, "\"[more]\""));
    EMB_CHECK(!dump_has(&h, ELL));
    lh_free(&h);
}

EMB_TEST(rtl_ellipsis_is_at_the_line_end_on_the_left)
{
    lh h = lh_make("<div id=a dir=rtl style='line-clamp:1'>aaa<br>bbb</div>", CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    double ex = text_x(d, ELL), ax = text_x(d, "aaa");
    EMB_CHECK(ex >= 0 && ax >= 0);
    EMB_CHECK(ex < ax); /* the end of a right-to-left line is its left side */
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_clamp_container_shows_the_first_line_and_the_ellipsis)
{
    /* The width is the max-content width of the lines plus room for the ellipsis: "aaa" + ellipsis fits and nothing is
     * displaced (a lone ellipsis would be all the line showed). */
    lh h = lh_make("<div id=f style='line-clamp:1;float:left;width:auto'>aaa<br>bbb<br>ccc</div>"
                   "<div id=i style='line-clamp:1;display:inline-block;width:auto'>aaa<br>bbb<br>ccc</div>"
                   "<div style='display:flex'><div id=x style='line-clamp:1;width:auto'>aaa<br>bbb<br>ccc</div></div>",
                   CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    EMB_CHECK(count_text(d, "aaa") == 3);
    EMB_CHECK(count_text(d, ELL) == 3);
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(nested_clamp_containers_clamp_on_their_own)
{
    /* The inner container is an independent formatting context: its lines do not count for the outer one, which counts
     * the lines of the paragraph after it. */
    lh h = lh_make("<div id=o style='line-clamp:2'><div id=n style='line-clamp:1'>aaa<br>bbb</div>"
                   "<p style='margin:0'>c<br>d<br>e</p></div>",
                   CSS);
    EMB_CHECK(near(height_of(&h, "n"), 20));
    EMB_CHECK(near(height_of(&h, "o"), 60)); /* the inner box, then two lines of the paragraph */
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    EMB_CHECK(count_text(d, ELL) == 2); /* one for each container */
    EMB_CHECK(strstr(d, "\"e\"") == nullptr);
    embi_sb_free(&sb);
    lh_free(&h);
}

static char *fresh_dump(const char *html, embi_sb *sb)
{
    lh f = lh_make(html, CSS);
    char *d = dump_of(&f, sb);
    lh_free(&f);
    return d;
}

EMB_TEST(restyling_a_child_matches_a_fresh_layout)
{
    const char *before = "<div id=a style='line-clamp:2'><span id=s>aaa</span><br>bbb<br>ccc<br>ddd</div>";
    const char *after = "<div id=a style='line-clamp:2'><span id=s style='font-size:20px'>aaa</span><br>bbb<br>ccc<br>ddd</div>";
    lh h = lh_make(before, CSS);
    embi_element *s = lh_id(&h, "s");
    EMB_REQUIRE(s != nullptr);
    embi_str k = {"style", 5};
    EMB_REQUIRE(embi_el_set_attribute(s, k, embi_dstr_from_utf8(h.doc->slab, (embi_str){"font-size:20px", 14})) == EMBI_DOM_OK);
    embi_style_resolve_document(h.style, h.doc);
    lh_relayout(&h);
    embi_sb a = {0}, b = {0};
    const char *da = dump_of(&h, &a);
    const char *db = fresh_dump(after, &b);
    EMB_CHECK(strcmp(da, db) == 0);
    if (strcmp(da, db) != 0) {
        printf("incremental:\n%s\nfresh:\n%s\n", da, db);
    }
    embi_sb_free(&a);
    embi_sb_free(&b);
    lh_free(&h);
}

EMB_TEST(text_overflow_ellipsises_lines_that_end_at_a_preserved_line_feed)
{
    lh h = lh_make("<div id=a style='white-space:pre;width:60px;overflow:hidden;text-overflow:ellipsis'>"
                   "aaaaaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbbbb</div>",
                   CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    EMB_CHECK(count_text(d, ELL) == 2); /* the first line ends at a line feed and is ellipsised too */
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(text_overflow_truncates_a_long_block_ellipsis)
{
    /* The block ellipsis does not fit the 60px line: its end is hidden for the text-overflow ellipsis. */
    lh h = lh_make("<div id=a style='width:60px;overflow:hidden;text-overflow:ellipsis;line-clamp:1 &quot;abcdefghijklmnop&quot;'>"
                   "x<br>y</div>",
                   CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    EMB_CHECK(strstr(d, "abcdefghijklmnop") == nullptr);
    EMB_CHECK(count_text(d, ELL) == 1);
    EMB_CHECK(text_x(d, ELL) > 0 && text_x(d, ELL) <= 60);
    embi_sb_free(&sb);
    lh_free(&h);
}

/* ------------------------------------------------------------------ the legacy -webkit-box properties */

static double left_of(lh *h, const char *id)
{
    embi_rectf r;
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, &r) ? r.x : -1;
}

static double top_of(lh *h, const char *id)
{
    embi_rectf r;
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, &r) ? r.y : -1;
}

#define BOX "#p{display:-webkit-box;width:200px;height:100px;font:10px/10px sans-serif}.i{width:20px;height:20px}"

EMB_TEST(box_pack_places_the_items_on_the_main_axis)
{
    lh h = lh_make("<div id=p style='-webkit-box-pack:end'><div class=i id=a></div><div class=i id=b></div></div>"
                   "<div id=q style='display:-webkit-box;width:200px;-webkit-box-pack:center'><div class=i id=c></div></div>"
                   "<div id=r style='display:-webkit-box;width:200px;-webkit-box-pack:justify'><div class=i id=d></div><div class=i id=e></div></div>",
                   BOX);
    EMB_CHECK(near(left_of(&h, "a"), 160) && near(left_of(&h, "b"), 180));
    EMB_CHECK(near(left_of(&h, "c"), 90));
    EMB_CHECK(near(left_of(&h, "d"), 0) && near(left_of(&h, "e"), 180));
    lh_free(&h);
}

EMB_TEST(box_align_places_the_items_on_the_cross_axis)
{
    lh h = lh_make("<div id=p style='-webkit-box-align:center'><div class=i id=a></div></div>"
                   "<div id=q style='display:-webkit-box;height:100px;-webkit-box-align:end'><div class=i id=b></div></div>"
                   "<div id=r style='display:-webkit-box;height:100px'><div id=c style='width:20px'></div></div>",
                   BOX);
    EMB_CHECK(near(top_of(&h, "a"), 40));
    EMB_CHECK(near(top_of(&h, "b"), 100 + 80));
    EMB_CHECK(near(height_of(&h, "c"), 100)); /* the default is stretch */
    lh_free(&h);
}

EMB_TEST(box_flex_shares_the_free_space_by_ratio)
{
    lh h = lh_make("<div id=p><div id=a class=i style='width:auto;-webkit-box-flex:1'></div>"
                   "<div id=b class=i style='width:auto;-webkit-box-flex:3'></div><div id=c class=i></div></div>",
                   BOX);
    embi_rectf ra, rb;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &ra) && embi_layout_border_box(h.eng, lh_id(&h, "b"), &rb));
    EMB_CHECK(near(ra.w, 45) && near(rb.w, 135));
    EMB_CHECK(near(left_of(&h, "c"), 180));
    lh_free(&h);
}

EMB_TEST(box_ordinal_group_and_direction)
{
    lh h = lh_make("<div id=p><div id=a class=i style='-webkit-box-ordinal-group:2'></div><div id=b class=i></div></div>"
                   "<div id=q style='display:-webkit-box;width:200px;-webkit-box-direction:reverse'><div id=c class=i></div>"
                   "<div id=d class=i></div></div>",
                   BOX);
    EMB_CHECK(left_of(&h, "b") < left_of(&h, "a"));
    EMB_CHECK(near(left_of(&h, "c"), 180) && near(left_of(&h, "d"), 160));
    lh_free(&h);
}

EMB_TEST(box_properties_do_not_touch_a_real_flex_container)
{
    lh h = lh_make("<div id=p style='display:flex;-webkit-box-pack:end;-webkit-box-align:center'><div class=i id=a></div></div>",
                   BOX);
    EMB_CHECK(near(left_of(&h, "a"), 0));
    EMB_CHECK(near(top_of(&h, "a"), 0));
    lh_free(&h);
}

EMB_TEST(box_flex_is_ignored_under_a_modern_flex_container)
{
    lh h = lh_make("<div id=p style='display:flex'><div id=a class=i style='width:auto;-webkit-box-flex:1;flex:none'></div>"
                   "<div id=b class=i></div></div>"
                   "<div id=q style='display:flex;width:200px'><div id=c class=i style='-webkit-box-flex:1;flex-shrink:0;width:150px'></div>"
                   "<div id=d class=i style='width:100px;flex-shrink:0'></div></div>",
                   BOX);
    embi_rectf ra;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &ra));
    EMB_CHECK(near(ra.w, 0)); /* flex: none: no growth from -webkit-box-flex */
    embi_rectf rc;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "c"), &rc));
    EMB_CHECK(near(rc.w, 150)); /* flex-shrink: 0 stays */
    lh_free(&h);
}

EMB_TEST(box_ordinal_group_is_ignored_under_a_modern_flex_container)
{
    lh h = lh_make("<div id=p style='display:flex'><div id=a class=i style='-webkit-box-ordinal-group:3;order:0'></div>"
                   "<div id=b class=i></div></div>",
                   BOX);
    EMB_CHECK(left_of(&h, "a") < left_of(&h, "b"));
    lh_free(&h);
}

EMB_TEST(legacy_box_maps_box_flex_and_ordinal_group_over_modern_properties)
{
    lh h = lh_make("<div id=p><div id=a class=i style='width:auto;-webkit-box-flex:1;flex:none;-webkit-box-ordinal-group:2;order:-5'></div>"
                   "<div id=b class=i></div></div>",
                   BOX);
    embi_rectf ra;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &ra));
    EMB_CHECK(near(ra.w, 180)); /* grows by the box flex though flex: none */
    EMB_CHECK(left_of(&h, "b") < left_of(&h, "a")); /* group 2, not order -5 */
    lh_free(&h);
}

EMB_TEST(switching_a_parent_between_legacy_box_and_flex_updates_its_items)
{
    const char *pre = "<div id=p style='";
    const char *post = "'><div id=a class=i style='-webkit-box-ordinal-group:2'></div><div id=b class=i></div></div>";
    char before[512], after[512];
    snprintf(before, sizeof before, "%sdisplay:-webkit-box%s", pre, post);
    snprintf(after, sizeof after, "%sdisplay:flex%s", pre, post);
    lh h = lh_make(before, BOX);
    EMB_CHECK(left_of(&h, "b") < left_of(&h, "a")); /* legacy: group 2 goes after group 1 */
    embi_element *p = lh_id(&h, "p");
    EMB_REQUIRE(embi_el_set_attribute(p, (embi_str){"style", 5},
                                      embi_dstr_from_utf8(h.doc->slab, (embi_str){"display:flex", 12})) == EMBI_DOM_OK);
    embi_style_resolve_document(h.style, h.doc);
    lh_relayout(&h);
    EMB_CHECK(left_of(&h, "a") < left_of(&h, "b")); /* flex: -webkit-box-ordinal-group does nothing */
    embi_sb a = {0}, b = {0};
    const char *da = dump_of(&h, &a);
    lh f = lh_make(after, BOX);
    const char *db = dump_of(&f, &b);
    EMB_CHECK(strcmp(da, db) == 0);
    lh_free(&f);
    /* and back */
    EMB_REQUIRE(embi_el_set_attribute(p, (embi_str){"style", 5},
                                      embi_dstr_from_utf8(h.doc->slab, (embi_str){"display:-webkit-box", 19})) == EMBI_DOM_OK);
    embi_style_resolve_document(h.style, h.doc);
    lh_relayout(&h);
    EMB_CHECK(left_of(&h, "b") < left_of(&h, "a"));
    embi_sb_free(&a);
    embi_sb_free(&b);
    lh_free(&h);
}

EMB_TEST(text_overflow_cuts_a_very_long_block_ellipsis_beyond_256_clusters)
{
    /* The prefix search has no fixed cluster limit: a 1000 character block ellipsis in a 2000px line keeps more than
     * 256 characters and still ends in the text-overflow ellipsis. */
    char html[1600];
    int n = snprintf(html, sizeof html, "<div id=a style='width:2000px;overflow:hidden;text-overflow:ellipsis;line-clamp:1 &quot;");
    for (int i = 0; i < 1000; i++) {
        html[n++] = 'a';
    }
    n += snprintf(html + n, sizeof html - (size_t)n, "&quot;'>x<br>y</div>");
    lh h = lh_make(html, CSS);
    embi_sb sb = {0};
    const char *d = dump_of(&h, &sb);
    /* 256 characters of this font are about 1400px; the kept prefix is the whole 2000px line less the ellipsis. */
    EMB_CHECK(text_x(d, ELL) > 1500 && text_x(d, ELL) <= 2000);
    EMB_CHECK(count_text(d, ELL) == 1);
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(an_author_internal_legacy_box_declaration_does_nothing)
{
    /* The legacy-box marker is internal: a stylesheet or inline declaration of its name is dropped, so a modern
     * flex container is never taken for a legacy box. */
    lh h = lh_make("<div id=p style='display:flex;-internal-legacy-box:yes'><div id=a class=i style='width:auto;-webkit-box-flex:1;flex:none'></div>"
                   "<div id=c class=i style='-webkit-box-ordinal-group:3;order:0'></div><div id=b class=i></div></div>",
                   BOX "#p{-internal-legacy-box:yes}.i{-internal-legacy-box:yes}");
    embi_rectf ra;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &ra));
    EMB_CHECK(near(ra.w, 0));
    EMB_CHECK(left_of(&h, "c") < left_of(&h, "b"));
    lh_free(&h);
}

EMB_TEST(all_inherit_does_not_copy_the_legacy_box_marker)
{
    /* `all: inherit` copies every longhand from the legacy box, the internal marker included; the child is a modern flex
     * container, so its items' -webkit-box-* properties must do nothing. */
    lh h = lh_make("<div style='display:-webkit-box'><div id=m style='all:inherit;display:flex;width:200px'>"
                   "<div id=a class=i style='width:auto;-webkit-box-flex:1;flex:none'></div>"
                   "<div id=c class=i style='-webkit-box-ordinal-group:3;order:0'></div><div id=b class=i></div></div></div>",
                   BOX);
    embi_rectf ra;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &ra));
    EMB_CHECK(near(ra.w, 0));
    EMB_CHECK(left_of(&h, "c") < left_of(&h, "b"));
    lh_free(&h);
}

EMB_TEST(switching_a_parent_between_legacy_box_and_flex_updates_the_used_flex_grow)
{
    const char *post = "'><div id=a class=i style='width:auto;-webkit-box-flex:1;flex:none;order:5'></div><div id=b class=i></div></div>";
    char before[512], after[512];
    snprintf(before, sizeof before, "<div id=p style='display:-webkit-box%s", post);
    snprintf(after, sizeof after, "<div id=p style='display:flex%s", post);
    lh h = lh_make(before, BOX);
    embi_rectf r;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &r));
    EMB_CHECK(near(r.w, 180)); /* legacy: grows by -webkit-box-flex */
    EMB_CHECK(left_of(&h, "a") < left_of(&h, "b")); /* ordinal group 1 for both: document order, not order: 5 */
    embi_element *p = lh_id(&h, "p");
    EMB_REQUIRE(embi_el_set_attribute(p, (embi_str){"style", 5},
                                      embi_dstr_from_utf8(h.doc->slab, (embi_str){"display:flex", 12})) == EMBI_DOM_OK);
    embi_style_resolve_document(h.style, h.doc);
    lh_relayout(&h);
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &r));
    EMB_CHECK(near(r.w, 0)); /* flex: none */
    EMB_CHECK(left_of(&h, "b") < left_of(&h, "a")); /* order: 5 now applies */
    embi_sb a = {0}, b = {0};
    const char *da = dump_of(&h, &a);
    lh f = lh_make(after, BOX);
    const char *db = dump_of(&f, &b);
    EMB_CHECK(strcmp(da, db) == 0);
    lh_free(&f);
    EMB_REQUIRE(embi_el_set_attribute(p, (embi_str){"style", 5},
                                      embi_dstr_from_utf8(h.doc->slab, (embi_str){"display:-webkit-box", 19})) == EMBI_DOM_OK);
    embi_style_resolve_document(h.style, h.doc);
    lh_relayout(&h);
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &r));
    EMB_CHECK(near(r.w, 180));
    EMB_CHECK(left_of(&h, "a") < left_of(&h, "b"));
    embi_sb_free(&a);
    embi_sb_free(&b);
    lh_free(&h);
}

EMB_TEST_MAIN()
