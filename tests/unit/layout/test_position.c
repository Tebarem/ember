/*
 * Positioned layout (W3d): the containing block of out-of-flow boxes, the placement records and
 * their re-layout, sticky offsets under scrolling, the stacking flags, and the memory of a layout.
 * The check-layout values of the position tests are in tests/layout/position (test_layout_wpt.c);
 * this file checks what the geometry API does not show.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b) { return fabs(a - b) < 0.01; }

/* The fragment of the element with the given id (its principal box's first fragment), or nullptr.
 */
static const embi_frag *frag_of(lh *h, const char *id) {
    embi_element *el = lh_id(h, id);
    if (el == nullptr || el->box == nullptr) {
        return nullptr;
    }
    return ((const embi_box *)el->box)->frag;
}

static const embi_box *box_of(lh *h, const char *id) {
    embi_element *el = lh_id(h, id);
    return el != nullptr ? (const embi_box *)el->box : nullptr;
}

/* The position of a fragment in document coordinates: the sum of the positions up to the root (the
 * initial containing block, at the origin). */
static void doc_xy(const embi_frag *f, float *x, float *y) {
    *x = 0;
    *y = 0;
    for (; f != nullptr; f = f->parent) {
        *x += f->x;
        *y += f->y;
    }
}

/* The fragment of an element by id, as an embi_frag *, for the tests that change scroll offsets. */
static embi_frag *mut_frag(lh *h, const char *id) { return (embi_frag *)frag_of(h, id); }

/* Percentage insets and sizes resolve against the padding box of the containing block. */
EMB_TEST(position_absolute_percentages_resolve_against_the_containing_block) {
    lh h = lh_make("<body><div id=c style='position:relative; width:200px; height:100px'>"
                   "<div id=a style='position:absolute; left:10%; top:25%; width:50%; height:50%'>"
                   "</div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->x, 20) && near(a->y, 25));
    EMB_CHECK(near(a->w, 100) && near(a->h, 50));
    lh_free(&h);
}

/* A positioned box's out-of-flow child is placed in the fragment of the containing block, and the
 * containing block of a child of an absolute box is that box (its records are placed after it). */
EMB_TEST(position_nested_absolute_boxes_are_children_of_their_containing_blocks) {
    lh h = lh_make("<body><div id=o style='position:absolute; left:10px; top:10px; width:100px;"
                   " height:100px'><div id=i style='position:absolute; left:5px; top:5px;"
                   " width:10px; height:10px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *o = frag_of(&h, "o");
    const embi_frag *i = frag_of(&h, "i");
    EMB_REQUIRE(o != nullptr && i != nullptr);
    EMB_CHECK(i->parent == o);
    EMB_CHECK(near(i->x, 5) && near(i->y, 5));
    EMB_CHECK(o->parent == embi_layout_root(h.eng, h.doc));
    lh_free(&h);
}

/* A fixed box is placed in the initial containing block, whatever positioned ancestors it has. */
EMB_TEST(position_fixed_box_is_a_child_of_the_initial_containing_block) {
    lh h = lh_make("<body><div style='position:relative; margin-left:50px'>"
                   "<div id=f style='position:fixed; right:0; bottom:0; width:10px; height:10px'>"
                   "</div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *f = frag_of(&h, "f");
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(f->parent == embi_layout_root(h.eng, h.doc));
    EMB_CHECK(near(f->x, 790) && near(f->y, 590));
    lh_free(&h);
}

/* Re-running a layout that placed a box leaves one fragment for it: the records of the earlier
 * layouts of its container are replaced by the last ones. */
EMB_TEST(position_absolute_box_is_placed_once_after_relayouts) {
    lh h =
        lh_make("<body><div id=c style='position:relative; overflow:auto; height:50px'>"
                "<div style='height:20px'></div><div style='float:left; width:10px; height:10px'>"
                "</div><div id=a style='position:absolute; top:5px; left:5px; width:5px;"
                " height:5px'></div></div></body>",
                "body { margin: 0 }");
    lh_relayout(&h);
    lh_relayout(&h);
    const embi_frag *c = frag_of(&h, "c");
    const embi_box *ab = box_of(&h, "a");
    EMB_REQUIRE(c != nullptr && ab != nullptr);
    uint32_t n = 0;
    for (const embi_frag *k = c->first; k != nullptr; k = k->next) {
        if (k->box == ab) {
            n++;
        }
    }
    EMB_CHECK_EQ_INT(n, 1);
    lh_free(&h);
}

/* The sticky offset follows the scroll offset of the scroll container: at zero there is none, and a
 * scroll past the box's top moves it down by the difference, within its containing block. */
EMB_TEST(position_sticky_offset_follows_the_scroll_offset) {
    lh h =
        lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                " height:100px'><div id=w style='height:500px'><div style='height:200px'></div>"
                "<div id=st style='position:sticky; top:0; height:20px'></div></div></div></body>",
                "body { margin: 0 }");
    embi_frag *st = (embi_frag *)frag_of(&h, "st");
    embi_frag *s = (embi_frag *)frag_of(&h, "s");
    EMB_REQUIRE(st != nullptr && s != nullptr);
    EMB_CHECK(embi_frag_sticky(st) != nullptr);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dx, 0) && near(dy, 0));
    lh_scroll(&h, s, embi_frag_sx(s), 250);
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, 50));
    EMB_CHECK(near(dx, 0));
    /* Beyond the containing block's bottom the box stops (500 - 220 = 280 is more than 50 here, so
     * scroll far enough that the box would pass the block's end). */
    lh_scroll(&h, s, embi_frag_sx(s), 1000);
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, 280));
    lh_free(&h);
}

/* Stacking contexts: positioned boxes with a z-index, opacity below one, fixed and sticky boxes,
 * transforms (which also make the containing block of fixed descendants), and the root. Absolute
 * boxes with z-index auto and relative boxes with z-index auto are not stacking contexts. */
EMB_TEST(position_stacking_flags_follow_appendix_e) {
    lh h = lh_make("<body><div id=a style='position:absolute; z-index:2'></div>"
                   "<div id=b style='position:absolute'></div>"
                   "<div id=c style='position:relative'></div>"
                   "<div id=d style='opacity:0.5'></div>"
                   "<div id=e style='position:fixed'></div>"
                   "<div id=f style='transform:translateX(0)'></div>"
                   "<div id=g style='position:relative; z-index:0'></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    const embi_frag *c = frag_of(&h, "c");
    const embi_frag *d = frag_of(&h, "d");
    const embi_frag *e = frag_of(&h, "e");
    const embi_frag *f = frag_of(&h, "f");
    const embi_frag *g = frag_of(&h, "g");
    EMB_REQUIRE(a && b && c && d && e && f && g);
    EMB_CHECK((a->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK_EQ_INT(embi_frag_z_index(a), 2);
    EMB_CHECK((b->flags & EMBI_FF_STACKING) == 0);
    EMB_CHECK((c->flags & EMBI_FF_STACKING) == 0);
    EMB_CHECK((d->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((e->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((f->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((f->flags & EMBI_FF_CB_FIXED) != 0);
    EMB_CHECK((b->flags & EMBI_FF_CB_FIXED) == 0);
    EMB_CHECK((g->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK_EQ_INT(embi_frag_z_index(g), 0);
    lh_free(&h);
}

/* A relative box moves its own fragment and leaves the flow of its siblings alone. */
EMB_TEST(position_relative_offset_does_not_move_siblings) {
    lh h = lh_make("<body><div id=a style='position:relative; top:7px; height:10px'></div>"
                   "<div id=b style='height:10px'></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    EMB_REQUIRE(a != nullptr && b != nullptr);
    EMB_CHECK(near(a->y, 7));
    EMB_CHECK(near(b->y, 10));
    lh_free(&h);
}

/* The layout of positioned boxes does not leak: the live bytes after a layout and its release are
 * the bytes before it. */
EMB_TEST(position_layouts_release_all_memory) {
    static const char html[] = "<body><div style='position:relative; width:100px'>"
                               "<div style='position:absolute; left:4px; width:20%'>x</div>"
                               "<div style='position:fixed; bottom:0'>y</div>"
                               "<div style='position:sticky; top:0'>z</div></div></body>";
    {
        lh w = lh_make(html, "body { margin: 0 }");
        lh_free(&w);
    }
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        lh h = lh_make(html, "body { margin: 0 }");
        lh_relayout(&h);
        lh_free(&h);
    }
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

/* An absolute box whose containing block is a relatively positioned inline follows the inline: its
 * left and top (0, 0) are the padding edge of the inline's first fragment, which the relative
 * offset moved by (40, 0). The inline has a 3px top border and no left border, so the padding edge
 * is 3px below the border edge. Derivation: the line starts at x = 0 (body margin 0), so the
 * inline's border box is at x = 40 with the offset, and the absolute box is at x = 40 and 3px below
 * the inline's top. */
EMB_TEST(position_abspos_in_relative_inline_follows_the_inline_offset) {
    lh h = lh_make("<body><div style='width:200px'><span id=r style='position:relative; left:40px;"
                   " border-top:3px solid'>x<span id=ab style='position:absolute; left:0; top:0;"
                   " width:5px; height:5px'></span></span></div></body>",
                   "body { margin: 0 }");
    const embi_frag *r = frag_of(&h, "r");
    const embi_frag *ab = frag_of(&h, "ab");
    EMB_REQUIRE(r != nullptr && ab != nullptr);
    float rx, ry, ax, ay;
    doc_xy(r, &rx, &ry);
    doc_xy(ab, &ax, &ay);
    EMB_CHECK(near(rx, 40));
    EMB_CHECK(near(ax, rx));
    EMB_CHECK(near(ay - ry, 3));
    EMB_CHECK(near(ab->w, 5) && near(ab->h, 5));
    lh_free(&h);
}

/* The same without the relative offset: the padding edge of the inline is its border edge plus the
 * top border, at the line start. */
EMB_TEST(position_abspos_in_plain_inline_uses_the_inline_padding_edge) {
    lh h = lh_make("<body><div style='width:200px'><span id=r style='border-top:3px solid'>x"
                   "<span id=ab style='position:absolute; left:0; top:0; width:5px; height:5px'>"
                   "</span></span></div></body>",
                   "body { margin: 0 }");
    const embi_frag *r = frag_of(&h, "r");
    const embi_frag *ab = frag_of(&h, "ab");
    EMB_REQUIRE(r != nullptr && ab != nullptr);
    float rx, ry, ax, ay;
    doc_xy(r, &rx, &ry);
    doc_xy(ab, &ax, &ay);
    EMB_CHECK(near(rx, 0));
    EMB_CHECK(near(ax, rx));
    EMB_CHECK(near(ay - ry, 3));
    lh_free(&h);
}

/* The static position of an absolute box inside a relatively positioned inline moves with the
 * inline: its static position is the line's content start (x = 0) plus the inline's offset of 40.
 */
EMB_TEST(position_static_position_inside_relative_inline_moves_with_it) {
    lh h = lh_make("<body><div style='width:200px'><span id=r style='position:relative; left:40px'>"
                   "<span id=ab style='position:absolute; width:5px; height:5px'></span></span>"
                   "</div></body>",
                   "body { margin: 0 }");
    const embi_frag *ab = frag_of(&h, "ab");
    EMB_REQUIRE(ab != nullptr);
    float ax, ay;
    doc_xy(ab, &ax, &ay);
    EMB_CHECK(near(ax, 40));
    lh_free(&h);
}

/* A fixed box whose static container lies below a relative box inside a transformed box: the
 * transformed box is its containing block, and the static position includes the relative box's
 * offset (30, 20) and the 10px spacer above the box. Derivation: the relative box moves to (30,
 * 20), the spacer is 10px, so the box's static position is (30, 30) in the transformed box, which
 * is at the document origin. */
EMB_TEST(position_fixed_static_position_includes_relative_ancestors) {
    lh h = lh_make("<body><div id=t style='transform:translateX(0); width:200px; height:200px'>"
                   "<div style='position:relative; left:30px; top:20px'><div style='height:10px'>"
                   "</div><div id=f style='position:fixed; width:5px; height:5px'></div></div>"
                   "</div></body>",
                   "body { margin: 0 }");
    const embi_frag *t = frag_of(&h, "t");
    const embi_frag *f = frag_of(&h, "f");
    EMB_REQUIRE(t != nullptr && f != nullptr);
    EMB_CHECK(f->parent == t);
    float fx, fy;
    doc_xy(f, &fx, &fy);
    EMB_CHECK(near(fx, 30) && near(fy, 30));
    lh_free(&h);
}

/* Replaced boxes keep their size: an img with width and height attributes, and a canvas with the
 * default 300 x 150, are not stretched by left, right, top and bottom (CSS 2.1 10.3.8 and 10.6.5).
 */
EMB_TEST(position_replaced_abspos_keeps_its_size) {
    lh h = lh_make("<body><div id=c style='position:relative; width:100px; height:100px'>"
                   "<img id=i width=30 height=20 style='position:absolute; left:0; right:0; top:0;"
                   " bottom:0'><canvas id=k style='position:absolute; left:0; right:0; top:0;"
                   " bottom:0'></canvas></div></body>",
                   "body { margin: 0 }");
    const embi_frag *i = frag_of(&h, "i");
    const embi_frag *k = frag_of(&h, "k");
    EMB_REQUIRE(i != nullptr && k != nullptr);
    EMB_CHECK(near(i->w, 30) && near(i->h, 20));
    EMB_CHECK(near(k->w, 300) && near(k->h, 150));
    lh_free(&h);
}

/* Shrink-to-fit widths of absolute boxes with fixed-width content (30px). Left only: the width is
 * 30 and the box is at 10. Right only: the box's right edge is 10 from the right, so x = 100 - 10 -
 * 30 =
 * 60. Both auto with a 20px margin: the box sits at its static position plus the margin, x = 20,
 * and its width is 30 (the available width, from the static position to the far edge, is 80). */
EMB_TEST(position_shrink_to_fit_widths) {
    lh h = lh_make("<body><div id=c style='position:relative; width:100px; height:100px'>"
                   "<div id=l style='position:absolute; left:10px'><div style='width:30px;"
                   " height:5px'></div></div>"
                   "<div id=r style='position:absolute; right:10px'><div style='width:30px;"
                   " height:5px'></div></div>"
                   "<div id=b style='position:absolute; margin-left:20px'><div style='width:30px;"
                   " height:5px'></div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *l = frag_of(&h, "l");
    const embi_frag *r = frag_of(&h, "r");
    const embi_frag *b = frag_of(&h, "b");
    EMB_REQUIRE(l != nullptr && r != nullptr && b != nullptr);
    EMB_CHECK(near(l->w, 30) && near(l->x, 10));
    EMB_CHECK(near(r->w, 30) && near(r->x, 60));
    EMB_CHECK(near(b->w, 30) && near(b->x, 20));
    lh_free(&h);
}

/* justify-self and align-self place a box with both insets set and no auto margin inside the inset
 * containing block. The box is 10 x 10 in a 100 x 100 containing block with all insets 0: center
 * gives x = 45, end gives x = 90 (the slack is 90), and align-self end gives y = 90. */
EMB_TEST(position_justify_and_align_self_place_a_box_between_its_insets) {
    lh h =
        lh_make("<body><div id=c style='position:relative; width:100px; height:100px'>"
                "<div id=j1 style='position:absolute; left:0; right:0; top:0; bottom:0; width:10px;"
                " height:10px; justify-self:center; align-self:end'></div>"
                "<div id=j2 style='position:absolute; left:0; right:0; top:0; bottom:0; width:10px;"
                " height:10px; justify-self:end'></div></div></body>",
                "body { margin: 0 }");
    const embi_frag *j1 = frag_of(&h, "j1");
    const embi_frag *j2 = frag_of(&h, "j2");
    EMB_REQUIRE(j1 != nullptr && j2 != nullptr);
    EMB_CHECK(near(j1->x, 45) && near(j1->y, 90));
    EMB_CHECK(near(j2->x, 90) && near(j2->y, 0));
    lh_free(&h);
}

/* Over-constrained boxes in rtl: left is ignored, so the box sits at the right edge of the
 * containing block (x = 100 - 10 = 90). justify-self end is the left edge (x = 0) and center is in
 * the middle (x = 45). */
EMB_TEST(position_rtl_over_constrained_box_follows_the_direction) {
    lh h = lh_make(
        "<body><div id=c style='position:relative; direction:rtl; width:100px;"
        " height:100px'>"
        "<div id=s style='position:absolute; left:0; right:0; width:10px; height:10px'></div>"
        "<div id=e style='position:absolute; left:0; right:0; width:10px; height:10px;"
        " justify-self:end'></div>"
        "<div id=m style='position:absolute; left:0; right:0; width:10px; height:10px;"
        " justify-self:center'></div></div></body>",
        "body { margin: 0 }");
    const embi_frag *s = frag_of(&h, "s");
    const embi_frag *e = frag_of(&h, "e");
    const embi_frag *m = frag_of(&h, "m");
    EMB_REQUIRE(s != nullptr && e != nullptr && m != nullptr);
    EMB_CHECK(near(s->x, 90));
    EMB_CHECK(near(e->x, 0));
    EMB_CHECK(near(m->x, 45));
    lh_free(&h);
}

/* A relative box with both left and right set: left wins in ltr (dx = 10), right wins in rtl
 * (dx = -20). The flow positions are the block's own: in ltr at 0, in rtl over-constrained at the
 * right edge (100 - 10 = 90), so the document positions are 10 and 70. */
EMB_TEST(position_relative_with_both_horizontal_insets_follows_the_direction) {
    lh h = lh_make("<body><div style='width:100px'><div id=a style='position:relative; left:10px;"
                   " right:20px; width:10px; height:5px'></div></div>"
                   "<div style='width:100px; direction:rtl'><div id=b style='position:relative;"
                   " left:10px; right:20px; width:10px; height:5px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    EMB_REQUIRE(a != nullptr && b != nullptr);
    float ax, ay, bx, by;
    doc_xy(a, &ax, &ay);
    doc_xy(b, &bx, &by);
    EMB_CHECK(near(ax, 10));
    EMB_CHECK(near(bx, 70));
    lh_free(&h);
}

/* Insets that overlap give way: left 60 and right 60 in a 100px containing block make the inset-
 * modified block negative, so ltr ignores right (right = 100 - 60 = 40) and the box has width 0 at
 * x = 60. In rtl, left gives way (left = 100 - 60 = 40), and the box is at x = 40. */
EMB_TEST(position_overlapping_insets_give_way) {
    lh h = lh_make("<body><div id=c style='position:relative; width:100px; height:100px'>"
                   "<div id=o style='position:absolute; left:60px; right:60px; height:10px'></div>"
                   "</div><div id=d style='position:relative; width:100px; height:100px;"
                   " direction:rtl'>"
                   "<div id=p style='position:absolute; left:60px; right:60px; height:10px'></div>"
                   "</div></body>",
                   "body { margin: 0 }");
    const embi_frag *o = frag_of(&h, "o");
    const embi_frag *p = frag_of(&h, "p");
    EMB_REQUIRE(o != nullptr && p != nullptr);
    EMB_CHECK(near(o->w, 0) && near(o->x, 60));
    EMB_CHECK(near(p->w, 0) && near(p->x, 40));
    lh_free(&h);
}

/* A fixed box inside a scrolling ancestor is placed in the viewport, and the scroll offset does not
 * move it. The box is at (10, 10) in the document whatever the scroll offset. */
EMB_TEST(position_fixed_box_ignores_a_scrolled_ancestor) {
    lh h = lh_make(
        "<body><div id=s style='position:relative; overflow:auto; width:100px;"
        " height:50px'><div style='height:300px'><div id=f style='position:fixed; left:10px;"
        " top:10px; width:5px; height:5px'></div></div></div></body>",
        "body { margin: 0 }");
    const embi_frag *f = frag_of(&h, "f");
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(f->parent == embi_layout_root(h.eng, h.doc));
    lh_scroll(&h, mut_frag(&h, "s"), embi_frag_sx(mut_frag(&h, "s")), 200);
    float x, y;
    doc_xy(f, &x, &y);
    EMB_CHECK(near(x, 10) && near(y, 10));
    lh_free(&h);
}

/* Sticky bottom: the box (200 to 220 in its containing block) stays inside the scrollport's bottom
 * edge (100 at zero scroll, 200 when scrolled by 100). At zero scroll the offset is -120, and at
 * 100 it is -20. */
EMB_TEST(position_sticky_bottom_moves_to_the_scrollport_edge) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div id=w><div style='height:200px'></div>"
                   "<div id=st style='position:sticky; bottom:0; height:20px'></div></div></div>"
                   "</body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    embi_frag *s = mut_frag(&h, "s");
    EMB_REQUIRE(st != nullptr && s != nullptr);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, -120));
    lh_scroll(&h, s, embi_frag_sx(s), 100);
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, -20));
    EMB_CHECK(near(dx, 0));
    lh_free(&h);
}

/* Sticky top clamps at the bottom of its containing block. Two scroll containers: the first holds
 * the box in a 220px block, so at scroll 250 the box would move 50 down but the block ends 0 below
 * it (offset 0); the second block is 400px, so the offset is 50. */
EMB_TEST(position_sticky_top_stops_at_the_containing_block_end) {
    lh h = lh_make("<body><div id=s1 style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div style='height:220px'><div style='height:200px'></div>"
                   "<div id=t1 style='position:sticky; top:0; height:20px'></div></div></div>"
                   "<div id=s2 style='position:relative; overflow:auto; width:100px; height:100px'>"
                   "<div style='height:400px'><div style='height:200px'></div>"
                   "<div id=t2 style='position:sticky; top:0; height:20px'></div></div></div>"
                   "</body>",
                   "body { margin: 0 }");
    const embi_frag *t1 = frag_of(&h, "t1");
    const embi_frag *t2 = frag_of(&h, "t2");
    EMB_REQUIRE(t1 != nullptr && t2 != nullptr);
    lh_scroll(&h, mut_frag(&h, "s1"), embi_frag_sx(mut_frag(&h, "s1")), 250);
    lh_scroll(&h, mut_frag(&h, "s2"), embi_frag_sx(mut_frag(&h, "s2")), 250);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(t1, &dx, &dy);
    EMB_CHECK(near(dy, 0));
    embi_lay_sticky_offset(t2, &dx, &dy);
    EMB_CHECK(near(dy, 50));
    lh_free(&h);
}

/* Sticky left follows the horizontal scroll: at scroll 250 the box (at x = 0 in its 400px block)
 * moves right by 250. */
EMB_TEST(position_sticky_left_follows_the_horizontal_scroll) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div id=w style='width:400px'><div id=st style='position:sticky;"
                   " left:0; width:20px; height:10px'></div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    EMB_REQUIRE(st != nullptr);
    lh_scroll(&h, mut_frag(&h, "s"), 250, embi_frag_sy(mut_frag(&h, "s")));
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dx, 250));
    EMB_CHECK(near(dy, 0));
    lh_free(&h);
}

/* Sticky right cannot move its box out of the containing block: the box (200 wide at x = 0) would
 * move left by 100 to meet the scrollport's right edge, but its left edge is already at the
 * containing block's left edge, so the offset is 0. */
EMB_TEST(position_sticky_right_stops_at_the_containing_block_edge) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div id=w style='width:400px'><div id=st style='position:sticky;"
                   " right:0; width:200px; height:10px'></div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    EMB_REQUIRE(st != nullptr);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dx, 0));
    lh_free(&h);
}

/* The static position of an absolute box is the flow position after the layout that the last
 * records came from. A float and a box that clears it (its top at 50) move the static position of
 * the box after it to y = 60, not the y = 10 it would have without the clearance. The container
 * overflows horizontally with a 15px scrollbar, so it is laid out a second time, and the records of
 * the first layout are replaced. */
EMB_TEST(position_static_position_uses_the_final_layout_after_relayouts) {
    lh h = lh_make("<body><div id=c style='position:relative; overflow:auto; width:100px'>"
                   "<div style='float:left; width:10px; height:50px'></div>"
                   "<div id=k style='clear:left; height:10px'></div>"
                   "<div id=a style='position:absolute; width:5px; height:5px'></div>"
                   "<div style='width:150px; height:1px'></div></div></body>",
                   "body { margin: 0 }");
    lh_set_scrollbar(&h, 15.0f);
    lh_relayout(&h);
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    float ax, ay;
    doc_xy(a, &ax, &ay);
    EMB_CHECK(near(ax, 0) && near(ay, 60));
    const embi_frag *c = frag_of(&h, "c");
    uint32_t n = 0;
    for (const embi_frag *k = c->first; k != nullptr; k = k->next) {
        n += k->box == (const embi_box *)box_of(&h, "a") ? 1 : 0;
    }
    EMB_CHECK_EQ_INT(n, 1);
    lh_free(&h);
}

/* Inline boxes take the stacking flags of Appendix E that apply to them: a positioned inline with
 * z-index, and opacity below one, create stacking contexts. A relative inline with z-index auto
 * does not, and transform does not apply to a non-replaced inline, so it does not either. */
EMB_TEST(position_inline_boxes_take_stacking_flags) {
    lh h = lh_make("<body><div><span id=z style='position:relative; z-index:3; padding-left:1px'>"
                   "</span><span id=o style='opacity:0.5; padding-left:1px'></span>"
                   "<span id=p style='position:relative; padding-left:1px'></span>"
                   "<span id=t style='transform:translateX(0); padding-left:1px'></span></div>"
                   "</body>",
                   "body { margin: 0 }");
    const embi_frag *z = frag_of(&h, "z");
    const embi_frag *o = frag_of(&h, "o");
    const embi_frag *p = frag_of(&h, "p");
    const embi_frag *t = frag_of(&h, "t");
    EMB_REQUIRE(z != nullptr && o != nullptr && p != nullptr && t != nullptr);
    EMB_CHECK((z->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK_EQ_INT(embi_frag_z_index(z), 3);
    EMB_CHECK((o->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((p->flags & EMBI_FF_STACKING) == 0);
    EMB_CHECK((t->flags & EMBI_FF_STACKING) == 0);
    lh_free(&h);
}

/* Clipping, containment and size properties create stacking contexts on boxes (clip-path on any
 * box, container-type on a block, content-visibility hidden on a block), and a clip-path on an
 * inline too. */
EMB_TEST(position_clip_and_containment_create_stacking_contexts) {
    lh h = lh_make("<body><div id=a style='clip-path:inset(0px)'></div>"
                   "<div id=b style='container-type:inline-size'></div>"
                   "<div id=c style='content-visibility:hidden'></div>"
                   "<div id=d style='width:4px'></div>"
                   "<div><span id=e style='clip-path:inset(0px); padding-left:1px'></span></div>"
                   "</body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    const embi_frag *c = frag_of(&h, "c");
    const embi_frag *d = frag_of(&h, "d");
    const embi_frag *e = frag_of(&h, "e");
    EMB_REQUIRE(a && b && c && d && e);
    EMB_CHECK((a->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((b->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((c->flags & EMBI_FF_STACKING) != 0);
    EMB_CHECK((d->flags & EMBI_FF_STACKING) == 0);
    EMB_CHECK((e->flags & EMBI_FF_STACKING) != 0);
    lh_free(&h);
}

/* A relative inline moves only its own fragments: the inline after it keeps its flow position. The
 * empty inline r has 5px of padding (so it has a box and is 5px wide), and it moves by 40. The next
 * inline n starts at x = 5, where r would have been. */
EMB_TEST(position_relative_inline_moves_only_itself) {
    lh h = lh_make(
        "<body><div style='width:200px'><span id=r style='position:relative; left:40px;"
        " padding-left:5px'></span><span id=n style='padding-left:5px'></span></div></body>",
        "body { margin: 0 }");
    const embi_frag *r = frag_of(&h, "r");
    const embi_frag *n = frag_of(&h, "n");
    EMB_REQUIRE(r != nullptr && n != nullptr);
    float rx, ry, nx, ny;
    doc_xy(r, &rx, &ry);
    doc_xy(n, &nx, &ny);
    EMB_CHECK(near(rx, 40));
    EMB_CHECK(near(nx, 5));
    lh_free(&h);
}

/* The inline block pieces used by the wrapped-inline tests: a 20px and a 30px wide box. */
#define PIECE20 "<span style='display:inline-block; width:20px; height:10px'></span>"
#define PIECE30 "<span style='display:inline-block; width:30px; height:10px'></span>"

/* A fixed box inside a container-type:inline-size box (layout containment) is placed against that
 * box: c is at (30, 50), so the fixed box with left:5px top:6px is at (35, 56). */
EMB_TEST(position_fixed_in_a_query_container_uses_the_container) {
    lh h = lh_make("<body><div style='height:50px'></div><div id=c style='container-type:inline-size;"
                   " margin-left:30px; width:100px; height:80px'><div id=f style='position:fixed;"
                   " left:5px; top:6px; width:10px; height:10px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *f = frag_of(&h, "f");
    EMB_REQUIRE(f != nullptr);
    float x, y;
    doc_xy(f, &x, &y);
    EMB_CHECK(near(x, 35) && near(y, 56));
    EMB_CHECK(f->parent == frag_of(&h, "c"));
    lh_free(&h);
}

/* The same for content-visibility:hidden (c at (30, 0): the fixed box is at (35, 6)) and for an
 * absolute box in a static container-type:size box (d at (40, 80): the box is at (47, 88)). */
EMB_TEST(position_content_visibility_and_size_containers_make_containing_blocks) {
    lh h = lh_make("<body><div id=c style='content-visibility:hidden; margin-left:30px;"
                   " width:100px; height:80px'><div id=f style='position:fixed; left:5px; top:6px;"
                   " width:10px; height:10px'></div></div>"
                   "<div id=d style='container-type:size; margin-left:40px; width:100px;"
                   " height:80px'><div id=a style='position:absolute; left:7px; top:8px;"
                   " width:10px; height:10px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *f = frag_of(&h, "f");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(f != nullptr && a != nullptr);
    float x, y;
    doc_xy(f, &x, &y);
    EMB_CHECK(near(x, 35) && near(y, 6));
    doc_xy(a, &x, &y);
    EMB_CHECK(near(x, 47) && near(y, 88));
    lh_free(&h);
}

/* A relative inline that wraps: the first piece is at x = 100..120 (text-indent), the last at
 * x = 0..30. The containing block runs from the start edge of the first padding box to the end edge
 * of the last one, swapped as they cross (CSS 2.1 10.1, as Chromium): x = 30..100. left:0 gives
 * x = 30; right:0 with width 10 gives x = 90. */
EMB_TEST(position_abspos_in_a_wrapped_inline_spans_first_start_to_last_end) {
    lh h = lh_make("<body><div style='width:200px; text-indent:100px'><span id=r"
                   " style='position:relative'>" PIECE20 "<br>" PIECE30
                   "<span id=a style='position:absolute; left:0; top:0; width:10px; height:5px'>"
                   "</span><span id=b style='position:absolute; right:0; bottom:0; width:10px;"
                   " height:5px'></span></span></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    EMB_REQUIRE(a != nullptr && b != nullptr);
    float ax, ay, bx, by;
    doc_xy(a, &ax, &ay);
    doc_xy(b, &bx, &by);
    EMB_CHECK(near(ax, 30));
    EMB_CHECK(near(bx, 90));
    EMB_CHECK(ay < by);
    lh_free(&h);
}

/* The same in an rtl block: the first piece is at x = 80..100 (indent from the right), the last at
 * x = 170..200; the start edge is the right one, so the containing block is x = 100..170 (the first
 * piece's right edge to the last piece's left edge, swapped): left:0 gives 100, right:0 with width
 * 10 gives 160. */
EMB_TEST(position_abspos_in_a_wrapped_rtl_inline_spans_first_start_to_last_end) {
    lh h = lh_make("<body><div style='width:200px; direction:rtl; text-indent:100px'><span id=r"
                   " style='position:relative'>" PIECE20 "<br>" PIECE30
                   "<span id=a style='position:absolute; left:0; top:0; width:10px; height:5px'>"
                   "</span><span id=b style='position:absolute; right:0; bottom:0; width:10px;"
                   " height:5px'></span></span></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    EMB_REQUIRE(a != nullptr && b != nullptr);
    float ax, ay, bx, by;
    doc_xy(a, &ax, &ay);
    doc_xy(b, &bx, &by);
    EMB_CHECK(near(ax, 100));
    EMB_CHECK(near(bx, 160));
    lh_free(&h);
}

#define LONG_TEXT                                                                                  \
    "x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x "  \
    "x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x"

/* Shrink-to-fit with both insets auto uses the static container's direction: an rtl block (width
 * 100, margin-left 50) in an ltr containing block of 200px has its static edge at its right, x =
 * 150, so the room is 150 (x = 0..150) and the long text fills it: width 150 at x = 0. */
EMB_TEST(position_shrink_to_fit_follows_the_static_container_direction_rtl_in_ltr) {
    lh h = lh_make("<body><div id=c style='position:relative; width:200px; height:100px'>"
                   "<div style='direction:rtl; width:100px; margin-left:50px'>"
                   "<div id=a style='position:absolute'>" LONG_TEXT "</div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->w, 150));
    EMB_CHECK(near(a->x, 0));
    lh_free(&h);
}

/* The reverse: an ltr block in an rtl containing block. Over-constrained in the rtl parent, the
 * block (width 100, margin-right 20) has margin-left 80, so its static edge is its left, x = 80,
 * and the room is 200 - 80 = 120 (x = 80..200). */
EMB_TEST(position_shrink_to_fit_follows_the_static_container_direction_ltr_in_rtl) {
    lh h = lh_make("<body><div id=c style='position:relative; direction:rtl; width:200px;"
                   " height:100px'><div style='direction:ltr; width:100px; margin-right:20px'>"
                   "<div id=a style='position:absolute'>" LONG_TEXT "</div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->w, 120));
    EMB_CHECK(near(a->x, 80));
    lh_free(&h);
}

/* z-index beyond the int32 range clamps instead of overflowing. */
EMB_TEST(position_z_index_clamps_to_the_int32_range) {
    lh h = lh_make("<body><div id=p style='position:relative; z-index:99999999999'></div>"
                   "<div id=n style='position:relative; z-index:-99999999999'></div></body>",
                   "body { margin: 0 }");
    const embi_frag *p = frag_of(&h, "p");
    const embi_frag *n = frag_of(&h, "n");
    EMB_REQUIRE(p != nullptr && n != nullptr);
    EMB_CHECK(embi_frag_z_index(p) == INT32_MAX);
    EMB_CHECK(embi_frag_z_index(n) == INT32_MIN);
    lh_free(&h);
}

/* Auto margins with both insets set centre the box: (200 - 50) / 2 = 75. The vertical insets with
 * an auto height fill the block: 100 - 10 - 20 = 70. */
EMB_TEST(position_auto_margins_centre_and_auto_height_fills_between_insets) {
    lh h = lh_make("<body><div id=c style='position:relative; width:200px; height:100px'>"
                   "<div id=a style='position:absolute; left:0; right:0; margin:auto; width:50px;"
                   " top:10px; bottom:20px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->x, 75));
    EMB_CHECK(near(a->y, 10) && near(a->h, 70));
    lh_free(&h);
}

/* A containing block with a border: the padding box starts at (5, 5) in c, and percentages resolve
 * against the padding box (200 x 100): left:10% = 20, top:50% = 50, so the box is at (25, 55). */
EMB_TEST(position_bordered_containing_block_origin_is_the_padding_box) {
    lh h = lh_make("<body><div id=c style='position:relative; border:5px solid; width:200px;"
                   " height:100px'><div id=a style='position:absolute; left:10%; top:50%;"
                   " width:10px; height:10px'></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->x, 25) && near(a->y, 55));
    lh_free(&h);
}

/* A sticky box with a margin: it is at y = 210 (200 + a 10px margin). The border box is compared
 * with the scrollport edge (CSS Position 3 section 3.4): at scroll 250 the offset is 250 - 210 = 40,
 * which leaves the border box on the scrollport edge. */
EMB_TEST(position_sticky_with_a_margin_sticks_by_its_border_box) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div style='height:500px'><div style='height:200px'></div>"
                   "<div id=st style='position:sticky; top:0; margin-top:10px; height:20px'></div>"
                   "</div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    embi_frag *s = mut_frag(&h, "s");
    EMB_REQUIRE(st != nullptr && s != nullptr);
    lh_scroll(&h, s, embi_frag_sx(s), 250);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, 40) && near(dx, 0));
    lh_free(&h);
}

/* The margin box must stay in the containing block. Each block is 260px tall; a is at y = 210 with
 * a 10px bottom margin: room 260 - 230 - 10 = 20. b (second block, at y = 470) has a 4px bottom
 * margin: room 26. The wanted offsets (40 and 50) are larger, so the clamp decides. */
EMB_TEST(position_sticky_margin_clamps_at_the_containing_block_end) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div style='height:260px'><div style='height:200px'></div>"
                   "<div id=a style='position:sticky; top:0; margin:10px 0; height:20px'></div>"
                   "</div><div style='height:260px'><div style='height:200px'></div>"
                   "<div id=b style='position:sticky; top:0; margin:10px 0 4px; height:20px'>"
                   "</div></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *a = frag_of(&h, "a");
    const embi_frag *b = frag_of(&h, "b");
    embi_frag *s = mut_frag(&h, "s");
    EMB_REQUIRE(a != nullptr && b != nullptr && s != nullptr);
    lh_scroll(&h, s, embi_frag_sx(s), 250);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(a, &dx, &dy);
    EMB_CHECK(near(dy, 20));
    lh_scroll(&h, s, embi_frag_sx(s), 520);
    embi_lay_sticky_offset(b, &dx, &dy);
    EMB_CHECK(near(dy, 26));
    lh_free(&h);
}

/* Both top and bottom: the box is pushed down by top and, when that leaves it below the bottom
 * limit, up again by bottom. A 20px box at y = 10 in a 100px scrollport with top:0 and bottom:0
 * never leaves the scrollport; at scroll 50 it moves down by 40 (top), still inside. */
EMB_TEST(position_sticky_top_and_bottom_both_apply) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div style='height:500px'><div style='height:10px'></div>"
                   "<div id=st style='position:sticky; top:0; bottom:0; height:20px'></div>"
                   "</div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    embi_frag *s = mut_frag(&h, "s");
    EMB_REQUIRE(st != nullptr && s != nullptr);
    lh_scroll(&h, s, embi_frag_sx(s), 50);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, 40));
    lh_scroll(&h, s, embi_frag_sx(s), 0);
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(near(dy, 0));
    lh_free(&h);
}

/* A sticky inline-block lives in a line fragment: it still gets sticky data, with the line's
 * position summed in. The line is at y = 200; at scroll 250 the box moves down by 50. */
EMB_TEST(position_sticky_inline_block_in_a_line_sticks) {
    lh h = lh_make("<body><div id=s style='position:relative; overflow:auto; width:100px;"
                   " height:100px'><div style='height:500px'><div style='height:200px'></div>"
                   "<span id=st style='display:inline-block; position:sticky; top:0; width:20px;"
                   " height:20px'></span></div></div></body>",
                   "body { margin: 0 }");
    const embi_frag *st = frag_of(&h, "st");
    embi_frag *s = mut_frag(&h, "s");
    EMB_REQUIRE(st != nullptr && s != nullptr);
    EMB_CHECK(embi_frag_sticky(st) != nullptr);
    lh_scroll(&h, s, embi_frag_sx(s), 250);
    float dx = 1, dy = 1;
    embi_lay_sticky_offset(st, &dx, &dy);
    EMB_CHECK(dy > 30 && dy <= 50);
    lh_free(&h);
}

EMB_TEST_MAIN()
