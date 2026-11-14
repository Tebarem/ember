/*
 * Display-list behavior added by W4g5: the paint order of decoration lines against the text, their snapping to whole
 * pixels, thick line-through and overline placement, every decorating box drawing its own line, text-decoration-skip-spaces,
 * text-decoration-inset (negative and percentage values, padding in the percentage base), box-decoration-break, the dotted
 * pattern phase carried in f[0] / f[1] across the text fragments of one decorating box, -webkit-text-fill-color,
 * background-clip: border-area, and the degenerate repeating gradient.
 */
#include "paint_harness.h"

#include <math.h>

#define DECO EMBI_DI_DECORATION
#define TEXT EMBI_DI_TEXT

static const embi_ditem *item_at(const ph *h, int i)
{
    return &h->dl.items.data[i];
}

/* The n-th (0-based) item of kind k, or -1. */
static int nth(const ph *h, uint8_t k, uint32_t n)
{
    for (uint32_t i = 0; i < h->dl.items.len; i++) {
        if (h->dl.items.data[i].kind == k && n-- == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool near(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

/* The number of FILL items of one color. */
static int fills_of(const ph *h, uint32_t rgba)
{
    int c = 0;
    for (uint32_t i = 0; i < h->dl.items.len; i++) {
        c += h->dl.items.data[i].kind == EMBI_DI_FILL && h->dl.items.data[i].colors[0] == rgba ? 1 : 0;
    }
    return c;
}

EMB_TEST(underline_is_below_the_text_and_line_through_above_it)
{
    ph h = ph_make("<span style=\"text-decoration: underline line-through\">abc</span>", "");
    EMB_REQUIRE(ph_count(&h.dl, DECO) == 2);
    int ul = nth(&h, DECO, 0), tx = nth(&h, TEXT, 0), lt = nth(&h, DECO, 1);
    EMB_REQUIRE(ul >= 0 && tx >= 0 && lt >= 0);
    EMB_CHECK(ul < tx);
    EMB_CHECK(tx < lt);
    /* the underline is lower on the page than the line-through */
    EMB_CHECK(item_at(&h, ul)->rect[1] > item_at(&h, lt)->rect[1]);
    ph_free(&h);
}

EMB_TEST(decoration_lines_are_snapped_to_whole_pixels)
{
    ph h = ph_make("<div style=\"font-size:17.3px;margin-top:3.4px;text-decoration:underline overline line-through\">abc</div>", "");
    EMB_REQUIRE(ph_count(&h.dl, DECO) == 3);
    for (uint32_t i = 0; i < 3; i++) {
        const embi_ditem *d = item_at(&h, nth(&h, DECO, i));
        EMB_CHECK(near(d->rect[1], roundf(d->rect[1]), 1e-4f));
        EMB_CHECK(near(d->rect[3], roundf(d->rect[3]), 1e-4f));
        EMB_CHECK(d->rect[3] >= 1.0f);
    }
    ph_free(&h);
}

EMB_TEST(a_thick_line_through_grows_both_ways_from_the_same_center)
{
    ph a = ph_make("<div style=\"font-size:40px;text-decoration:line-through\">abc</div>", "");
    ph b = ph_make("<div style=\"font-size:40px;text-decoration:line-through;text-decoration-thickness:20px\">abc</div>", "");
    const embi_ditem *da = item_at(&a, nth(&a, DECO, 0)), *db = item_at(&b, nth(&b, DECO, 0));
    EMB_CHECK(near(db->rect[3], 20.0f, 0.01f));
    EMB_CHECK(db->rect[3] > da->rect[3]);
    float ca = da->rect[1] + da->rect[3] * 0.5f, cb = db->rect[1] + db->rect[3] * 0.5f;
    EMB_CHECK(near(ca, cb, 1.0f));
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(a_thick_overline_grows_upward_and_an_underline_grows_downward)
{
    ph a = ph_make("<div style=\"font-size:40px;text-decoration:overline underline\">abc</div>", "");
    ph b = ph_make("<div style=\"font-size:40px;text-decoration:overline underline;text-decoration-thickness:12px\">abc</div>", "");
    const embi_ditem *oa = item_at(&a, nth(&a, DECO, 0)), *ob = item_at(&b, nth(&b, DECO, 0));
    const embi_ditem *ua = item_at(&a, nth(&a, DECO, 1)), *ub = item_at(&b, nth(&b, DECO, 1));
    EMB_CHECK(near(oa->rect[1] + oa->rect[3], ob->rect[1] + ob->rect[3], 1.0f)); /* the bottom edge stays */
    EMB_CHECK(ob->rect[1] < oa->rect[1]);
    EMB_CHECK(near(ua->rect[1], ub->rect[1], 1.0f)); /* the top edge stays */
    EMB_CHECK(ub->rect[1] + ub->rect[3] > ua->rect[1] + ua->rect[3]);
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(an_ancestor_and_a_descendant_each_draw_their_line)
{
    ph h = ph_make("<div style=\"text-decoration:underline;color:#00f\"><span style=\"text-decoration:underline;"
                   "text-decoration-color:#0f0\">t</span></div>", "");
    EMB_REQUIRE(ph_count(&h.dl, DECO) == 2);
    uint32_t c0 = item_at(&h, nth(&h, DECO, 0))->colors[0], c1 = item_at(&h, nth(&h, DECO, 1))->colors[0];
    /* the ancestor's line is drawn first, the descendant's on top */
    EMB_CHECK(c0 == PH_BLUE);
    EMB_CHECK(c1 == PH_GREEN);
    ph_free(&h);
}

EMB_TEST(an_inline_descendant_keeps_the_decorating_boxs_line_position)
{
    /* the superscript has a smaller font, but the underline of the div is one uniform line */
    ph h = ph_make("<div style=\"font-size:30px;text-decoration:underline\">a<sup>2</sup>b</div>", "");
    uint32_t n = ph_count(&h.dl, DECO);
    EMB_REQUIRE(n >= 2);
    const embi_ditem *d0 = item_at(&h, nth(&h, DECO, 0));
    for (uint32_t i = 1; i < n; i++) {
        const embi_ditem *d = item_at(&h, nth(&h, DECO, i));
        EMB_CHECK(near(d->rect[1], d0->rect[1], 0.51f));
        EMB_CHECK(near(d->rect[3], d0->rect[3], 0.51f));
    }
    ph_free(&h);
}

EMB_TEST(spaces_at_the_line_ends_are_not_underlined_by_default)
{
    ph a = ph_make("<div style=\"white-space:pre;text-decoration:underline\">    ab    </div>", "");
    ph b = ph_make("<div style=\"white-space:pre;text-decoration:underline;text-decoration-skip-spaces:none\">    ab    </div>", "");
    EMB_REQUIRE(ph_count(&a.dl, DECO) == 1 && ph_count(&b.dl, DECO) == 1);
    float tw = item_at(&a, nth(&a, TEXT, 0))->rect[2];
    const embi_ditem *da = item_at(&a, nth(&a, DECO, 0)), *db = item_at(&b, nth(&b, DECO, 0));
    EMB_CHECK(near(db->rect[2], tw, 0.01f)); /* none: the whole fragment */
    EMB_CHECK(da->rect[2] > 0.0f);
    EMB_CHECK(da->rect[2] < tw * 0.5f); /* default: only "ab" */
    EMB_CHECK(da->rect[0] > db->rect[0]);
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(skip_spaces_all_splits_the_line_at_interior_spaces)
{
    ph h = ph_make("<div style=\"white-space:pre;text-decoration:underline;text-decoration-skip-spaces:all\">AB CD EF</div>", "");
    EMB_REQUIRE(ph_count(&h.dl, DECO) == 3);
    const embi_ditem *d0 = item_at(&h, nth(&h, DECO, 0)), *d1 = item_at(&h, nth(&h, DECO, 1));
    EMB_CHECK(d1->rect[0] > d0->rect[0] + d0->rect[2]); /* a gap for the space */
    ph_free(&h);
}

EMB_TEST(a_negative_inset_extends_the_line_at_both_ends)
{
    ph a = ph_make("<span style=\"text-decoration:underline\">abc</span>", "");
    ph b = ph_make("<span style=\"text-decoration:underline;text-decoration-inset:-10px\">abc</span>", "");
    const embi_ditem *da = item_at(&a, nth(&a, DECO, 0)), *db = item_at(&b, nth(&b, DECO, 0));
    EMB_CHECK(near(db->rect[0], da->rect[0] - 10.0f, 0.01f));
    EMB_CHECK(near(db->rect[2], da->rect[2] + 20.0f, 0.01f));
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(a_positive_inset_with_a_percentage_trims_a_fraction_of_the_decoration_length)
{
    ph a = ph_make("<span style=\"text-decoration:underline\">abcd</span>", "");
    ph b = ph_make("<span style=\"text-decoration:underline;text-decoration-inset:25% 0\">abcd</span>", "");
    const embi_ditem *da = item_at(&a, nth(&a, DECO, 0)), *db = item_at(&b, nth(&b, DECO, 0));
    EMB_CHECK(near(db->rect[0], da->rect[0] + da->rect[2] * 0.25f, 0.01f));
    EMB_CHECK(near(db->rect[2], da->rect[2] * 0.75f, 0.01f));
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(the_percentage_base_of_the_inset_includes_the_padding_and_border_of_the_box)
{
    ph a = ph_make("<span style=\"text-decoration:underline;padding:0 10px;border:5px solid #000\">abcd</span>", "");
    ph b = ph_make("<span style=\"text-decoration:underline;padding:0 10px;border:5px solid #000;"
                   "text-decoration-inset:10% 0\">abcd</span>", "");
    const embi_ditem *da = item_at(&a, nth(&a, DECO, 0)), *db = item_at(&b, nth(&b, DECO, 0));
    float len = da->rect[2] + 30.0f; /* the text plus 2 x (10px padding + 5px border) */
    EMB_CHECK(near(db->rect[0] - da->rect[0], 0.1f * len, 0.05f));
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(box_decoration_break_slice_trims_only_the_outer_ends_and_clone_every_fragment)
{
    const char *html_slice = "<span style=\"text-decoration:underline;text-decoration-inset:5px\">aaa<br>bbb<br>ccc</span>";
    const char *html_clone = "<span style=\"text-decoration:underline;text-decoration-inset:5px;box-decoration-break:clone\">"
                             "aaa<br>bbb<br>ccc</span>";
    ph a = ph_make(html_slice, "");
    ph b = ph_make(html_clone, "");
    EMB_REQUIRE(ph_count(&a.dl, DECO) == 3 && ph_count(&b.dl, DECO) == 3);
    for (uint32_t i = 0; i < 3; i++) {
        float tw_a = item_at(&a, nth(&a, TEXT, i))->rect[2], tw_b = item_at(&b, nth(&b, TEXT, i))->rect[2];
        float wa = item_at(&a, nth(&a, DECO, i))->rect[2], wb = item_at(&b, nth(&b, DECO, i))->rect[2];
        /* slice: 5px off the start of line 1 and off the end of line 3; the middle line is whole */
        float want_a = i == 1 ? tw_a : tw_a - 5.0f;
        EMB_CHECK(near(wa, want_a, 0.05f));
        EMB_CHECK(near(wb, tw_b - 10.0f, 0.05f));
    }
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(the_dotted_phase_carries_across_the_text_fragments_of_one_decorating_box)
{
    ph h = ph_make("<span style=\"text-decoration:underline dotted\">ab<b>cd</b></span>", "");
    EMB_REQUIRE(ph_count(&h.dl, DECO) == 2);
    const embi_ditem *d0 = item_at(&h, nth(&h, DECO, 0)), *d1 = item_at(&h, nth(&h, DECO, 1));
    EMB_CHECK(d0->sub == EMBI_DECO_DOTTED);
    EMB_CHECK(near(d0->f[0], 0.0f, 0.001f));       /* the first fragment starts the pattern */
    EMB_CHECK(d0->f[1] == 1.0f);                   /* and more of the line follows */
    EMB_CHECK(near(d1->f[0], d0->rect[2], 0.05f)); /* the second continues where the first ended */
    EMB_CHECK(d1->f[1] == 0.0f);
    ph_free(&h);
}

EMB_TEST(text_fill_color_paints_the_glyphs_but_not_the_decoration)
{
    ph h = ph_make("<div style=\"color:#00f;-webkit-text-fill-color:#f00;text-decoration:underline\">t</div>", "");
    int t = nth(&h, TEXT, 0), d = nth(&h, DECO, 0);
    EMB_REQUIRE(t >= 0 && d >= 0);
    EMB_CHECK(item_at(&h, t)->colors[0] == PH_RED);
    EMB_CHECK(item_at(&h, d)->colors[0] == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(background_clip_border_area_paints_one_clip_per_border_side)
{
    ph all = ph_make("<div style=\"width:50px;height:50px;border:10px solid transparent;background:#f00;"
                     "background-clip:border-area\"></div>", "");
    ph left = ph_make("<div style=\"width:50px;height:50px;border-left:10px solid transparent;background:#f00;"
                      "background-clip:border-area\"></div>", "");
    ph none = ph_make("<div style=\"width:50px;height:50px;background:#f00;background-clip:border-area\"></div>", "");
    EMB_CHECK_EQ_INT(fills_of(&all, PH_RED), 4);  /* top, bottom, left and right bands */
    EMB_CHECK_EQ_INT(fills_of(&left, PH_RED), 1); /* only the left border exists */
    EMB_CHECK_EQ_INT(fills_of(&none, PH_RED), 0); /* no border: no border area */
    ph_free(&all);
    ph_free(&left);
    ph_free(&none);
}

EMB_TEST(a_repeating_gradient_with_no_length_paints_its_last_color)
{
    ph h = ph_make("<div style=\"width:100px;height:100px;background-image:repeating-linear-gradient(#ffa500 50%, #00f 50%)\"></div>", "");
    int g = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(g >= 0);
    const embi_ditem *it = item_at(&h, g);
    EMB_REQUIRE(it->aux2 >= 1);
    for (uint32_t i = 0; i < it->aux2; i++) {
        EMB_CHECK(h.dl.stops.data[it->aux + i].color == PH_BLUE); /* opaque blue */
    }
    ph_free(&h);
}

EMB_TEST(a_huge_radial_gradient_center_stays_finite)
{
    ph h = ph_make("<div style=\"width:100px;height:100px;background:radial-gradient(circle at 0% calc(Infinity * 1px),"
                   " #0f0 calc(Infinity * 1px), #f00 0)\"></div>", "");
    int g = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(g >= 0);
    const embi_ditem *it = item_at(&h, g);
    for (int k = 0; k < 4; k++) {
        EMB_CHECK(isfinite(it->f[k]));
    }
    ph_free(&h);
}

EMB_TEST(paint_containment_on_the_body_keeps_its_background_off_the_canvas)
{
    ph a = ph_make("<body style=\"background:#f00\">hi</body>", "");
    ph b = ph_make("<body style=\"background:#f00;contain:paint\">hi</body>", "");
    int fa = -1, fb = -1;
    for (uint32_t i = 0; i < a.dl.items.len; i++) {
        if (a.dl.items.data[i].kind == EMBI_DI_FILL && a.dl.items.data[i].colors[0] == PH_RED) {
            fa = (int)i;
        }
    }
    for (uint32_t i = 0; i < b.dl.items.len; i++) {
        if (b.dl.items.data[i].kind == EMBI_DI_FILL && b.dl.items.data[i].colors[0] == PH_RED) {
            fb = (int)i;
        }
    }
    EMB_REQUIRE(fa >= 0 && fb >= 0);
    EMB_CHECK(near(item_at(&a, fa)->rect[3], 600.0f, 0.01f)); /* propagated: the canvas */
    EMB_CHECK(item_at(&b, fb)->rect[3] < 100.0f);       /* contained: the body's own box */
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(the_background_color_is_clipped_by_the_last_image_layer)
{
    /* two image layers (none, none): the color takes the clip of the second one (content-box); the third clip value is cut */
    ph h = ph_make("<div style=\"width:100px;height:50px;border:10px solid transparent;padding:5px;background-color:#f00;"
                   "background-clip:border-box, content-box, border-box;background-image:none, none\"></div>", "");
    int f = -1;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_FILL && h.dl.items.data[i].colors[0] == PH_RED) {
            f = (int)i;
        }
    }
    EMB_REQUIRE(f >= 0);
    EMB_CHECK(near(item_at(&h, f)->rect[2], 100.0f, 0.01f));
    EMB_CHECK(near(item_at(&h, f)->rect[3], 50.0f, 0.01f));
    ph_free(&h);
}

EMB_TEST_MAIN()
