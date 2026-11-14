/*
 * Painting of form controls (W5e, src/paint/control.c) checked through the display list: the chrome of text fields, buttons,
 * checkboxes, radios, selects, ranges, progress and meter, the value text (bullets for a password, the placeholder), the
 * selection highlight, the caret, the composition underline, `appearance: none`, and the wrapping of a textarea. The dynamic
 * state comes from a fake hook on the layout engine, as the bindings give it; without it the attributes are read.
 */
#include "paint_harness.h"

#include <math.h>

/* Colors of control.c as the list stores them (RGBA8, r in the low byte). */
#define C_BLUE 0xFFFF7500u
#define C_BORDER 0xFF767676u
#define C_FACE 0xFFEFEFEFu
#define C_SEL_FOCUS 0xFFFFD7B3u
#define C_SEL_BLUR 0xFFD4D4D4u
#define C_PLACEHOLDER 0xFF757575u

/* The fake state: the control with this id gets the state (the others read their attributes). */
static const char *g_id;
static embi_paint_ctl g_state;

static bool fake_state(void *user, const embi_element *el, void *outp)
{
    (void)user;
    if (g_id == nullptr) {
        return false;
    }
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){"id", 2});
    if (a == nullptr || !embi_dstr_eq_utf8(a->value, (embi_str){g_id, (uint32_t)strlen(g_id)})) {
        return false;
    }
    *(embi_paint_ctl *)outp = g_state;
    return true;
}

/* Paints again with the fake state for the control with the given id. */
static void repaint(ph *h, const char *id, const embi_paint_ctl *st)
{
    g_id = id;
    if (st != nullptr) {
        g_state = *st;
    }
    embi_layout_set_control_hook(h->eng, fake_state, nullptr);
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_paint_document(&h->dl, h->eng, h->doc, &cfg);
}

static int nth(const embi_dl *d, uint8_t kind, uint32_t n)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == kind) {
            if (n == 0) {
                return (int)i;
            }
            n--;
        }
    }
    return -1;
}

/* The first FILL item with color c and the given width (+-0.01), or -1. */
static int fill_w(const embi_dl *d, uint32_t c, float w)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_FILL && it->colors[0] == c && fabsf(it->rect[2] - w) < 0.01f) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t border_count(const embi_dl *d, uint32_t color)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        n += it->kind == EMBI_DI_BORDER && it->colors[0] == color ? 1u : 0u;
    }
    return n;
}

/* ------------------------------------------------------------------ checkbox and radio */

EMB_TEST(checkbox_unchecked_has_a_border_and_no_tick)
{
    ph h = ph_make("<input type=checkbox id=c>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_CHECK), 0);
    EMB_CHECK(border_count(&h.dl, C_BORDER) == 1);
    ph_free(&h);
}

EMB_TEST(checkbox_checked_is_filled_with_a_tick)
{
    ph h = ph_make("<input type=checkbox checked id=c>", "");
    int t = nth(&h.dl, EMBI_DI_CHECK, 0);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(h.dl.items.data[t].colors[0] == PH_WHITE);
    EMB_CHECK(fill_w(&h.dl, C_BLUE, 13.0f) >= 0);
    EMB_CHECK_EQ_INT(border_count(&h.dl, C_BORDER), 0);
    ph_free(&h);
}

EMB_TEST(checkbox_indeterminate_draws_a_dash_instead_of_the_tick)
{
    ph h = ph_make("<input type=checkbox id=c>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_CHECKBOX;
    st.indeterminate = true;
    repaint(&h, "c", &st);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_CHECK), 0);
    EMB_CHECK(fill_w(&h.dl, C_BLUE, 13.0f) >= 0);
    EMB_CHECK(ph_fill_of(&h.dl, PH_WHITE) >= 0);
    ph_free(&h);
}

EMB_TEST(radio_checked_has_a_ring_and_a_round_dot)
{
    ph h = ph_make("<input type=radio checked id=r>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_CHECK), 0);
    int dot = fill_w(&h.dl, C_BLUE, 6.5f);
    EMB_REQUIRE(dot >= 0);
    EMB_CHECK(h.dl.items.data[dot].radii[0] > 3.0f);
    ph_free(&h);
    ph u = ph_make("<input type=radio id=r>", "");
    EMB_CHECK_EQ_INT(fill_w(&u.dl, C_BLUE, 6.5f), -1);
    ph_free(&u);
}

EMB_TEST(checkbox_with_appearance_none_paints_no_chrome)
{
    ph h = ph_make("<input type=checkbox checked style=\"appearance:none\">", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_CHECK), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 0);
    ph_free(&h);
}

/* ------------------------------------------------------------------ text fields */

EMB_TEST(text_field_has_a_white_face_a_border_and_its_value)
{
    ph h = ph_make("<input id=t value=hello>", "");
    EMB_CHECK(ph_fill_of(&h.dl, PH_WHITE) >= 0);
    EMB_CHECK(border_count(&h.dl, C_BORDER) == 1);
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    const embi_lay_shaped *run = (const embi_lay_shaped *)h.dl.runs.data[h.dl.items.data[t].aux];
    EMB_CHECK_EQ_INT((int)embi_tshim_len(run), 5);
    EMB_CHECK(strncmp(embi_tshim_bytes(run), "hello", 5) == 0);
    ph_free(&h);
}

EMB_TEST(text_field_appearance_none_drops_the_chrome_but_keeps_the_text)
{
    ph h = ph_make("<input value=hi style=\"appearance:none\">", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    ph_free(&h);
}

EMB_TEST(author_border_and_background_replace_the_chrome)
{
    ph h = ph_make("<input value=hi style=\"border:2px solid #00f;background:#0f0\">", "");
    EMB_CHECK_EQ_INT(border_count(&h.dl, C_BORDER), 0);
    EMB_CHECK(ph_fill_of(&h.dl, PH_GREEN) >= 0);
    EMB_CHECK_EQ_INT(ph_fill_of(&h.dl, PH_WHITE), -1);
    ph_free(&h);
}

EMB_TEST(password_is_drawn_as_bullets)
{
    ph h = ph_make("<input type=password value=abc>", "");
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    const embi_lay_shaped *run = (const embi_lay_shaped *)h.dl.runs.data[h.dl.items.data[t].aux];
    EMB_CHECK_EQ_INT((int)embi_tshim_len(run), 9);
    EMB_CHECK(strncmp(embi_tshim_bytes(run), "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2", 9) == 0);
    ph_free(&h);
}

EMB_TEST(placeholder_shows_in_gray_while_the_value_is_empty)
{
    ph h = ph_make("<input id=t placeholder=name>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXT;
    st.placeholder = "name";
    st.placeholder_len = 4;
    repaint(&h, "t", &st);
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(h.dl.items.data[t].colors[0] == C_PLACEHOLDER);
    ph_free(&h);
}

EMB_TEST(placeholder_comes_from_the_attribute_when_there_is_no_state)
{
    ph h = ph_make("<input placeholder=name>", "");
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(h.dl.items.data[t].colors[0] == C_PLACEHOLDER);
    ph_free(&h);
}

EMB_TEST(selection_is_highlighted_blue_when_focused_and_gray_otherwise)
{
    ph h = ph_make("<input id=t value=hello>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXT;
    st.text = "hello";
    st.text_len = 5;
    st.sel_a = 1;
    st.sel_b = 4;
    st.caret = 4;
    st.focused = true;
    repaint(&h, "t", &st);
    int s = ph_fill_of(&h.dl, C_SEL_FOCUS);
    EMB_REQUIRE(s >= 0);
    EMB_CHECK(h.dl.items.data[s].rect[2] > 5.0f);
    /* the highlight lies under the text */
    EMB_CHECK(s < nth(&h.dl, EMBI_DI_TEXT, 0));
    st.focused = false;
    repaint(&h, "t", &st);
    EMB_CHECK(ph_fill_of(&h.dl, C_SEL_BLUR) >= 0);
    EMB_CHECK_EQ_INT(ph_fill_of(&h.dl, C_SEL_FOCUS), -1);
    ph_free(&h);
}

EMB_TEST(caret_blinks_with_the_phase_and_stands_at_the_caret_offset)
{
    ph h = ph_make("<input id=t value=hello>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXT;
    st.text = "hello";
    st.text_len = 5;
    st.sel_a = st.sel_b = st.caret = 5;
    st.focused = true;
    st.caret_on = true;
    repaint(&h, "t", &st);
    int c = fill_w(&h.dl, PH_BLACK, 1.0f);
    EMB_REQUIRE(c >= 0);
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    /* the caret is right of the text end of "hello" */
    float cx = h.dl.items.data[c].rect[0];
    EMB_CHECK(cx > h.dl.items.data[t].rect[0] + 10.0f);
    st.caret = 0;
    st.sel_a = st.sel_b = 0;
    repaint(&h, "t", &st);
    int c0 = fill_w(&h.dl, PH_BLACK, 1.0f);
    EMB_REQUIRE(c0 >= 0);
    EMB_CHECK(h.dl.items.data[c0].rect[0] < cx);
    st.caret_on = false;
    repaint(&h, "t", &st);
    EMB_CHECK_EQ_INT(fill_w(&h.dl, PH_BLACK, 1.0f), -1);
    ph_free(&h);
}

EMB_TEST(composition_text_is_underlined)
{
    ph h = ph_make("<input id=t>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXT;
    st.text = "abXY";
    st.text_len = 4;
    st.sel_a = st.sel_b = st.caret = 4;
    st.comp_on = true;
    st.comp_a = 2;
    st.comp_b = 4;
    st.focused = true;
    repaint(&h, "t", &st);
    int d = nth(&h.dl, EMBI_DI_DECORATION, 0);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(h.dl.items.data[d].rect[2] > 4.0f);
    EMB_CHECK(fabsf(h.dl.items.data[d].rect[3] - 1.0f) < 0.01f);
    st.comp_on = false;
    repaint(&h, "t", &st);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_DECORATION), 0);
    ph_free(&h);
}

EMB_TEST(a_long_value_scrolls_so_that_the_caret_stays_in_the_box)
{
    ph h = ph_make("<input id=t style=\"width:60px\">", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXT;
    st.text = "the quick brown fox jumps";
    st.text_len = 25;
    st.sel_a = st.sel_b = st.caret = 25;
    st.focused = true;
    st.caret_on = true;
    repaint(&h, "t", &st);
    int c = fill_w(&h.dl, PH_BLACK, 1.0f);
    EMB_REQUIRE(c >= 0);
    EMB_CHECK(h.dl.items.data[c].rect[0] < 60.0f);
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(h.dl.items.data[t].rect[0] < 0.0f); /* the text starts left of the box: scrolled */
    ph_free(&h);
}

EMB_TEST(textarea_wraps_long_text_into_lines_and_honors_newlines)
{
    ph h = ph_make("<textarea id=t cols=10 rows=4></textarea>", "");
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_TEXTAREA;
    st.text = "alpha beta gamma delta\nzeta";
    st.text_len = 27;
    st.caret = st.sel_a = st.sel_b = 0;
    repaint(&h, "t", &st);
    uint32_t lines = ph_count(&h.dl, EMBI_DI_TEXT);
    EMB_CHECK(lines >= 4); /* 10 columns of ~8px cannot hold more than two words a line */
    int a = nth(&h.dl, EMBI_DI_TEXT, 0);
    int b = nth(&h.dl, EMBI_DI_TEXT, 1);
    EMB_REQUIRE(a >= 0 && b >= 0);
    EMB_CHECK(h.dl.items.data[b].f[1] > h.dl.items.data[a].f[1] + 5.0f);
    ph_free(&h);
}

EMB_TEST(textarea_reads_its_child_text_without_state)
{
    ph h = ph_make("<textarea>one\ntwo</textarea>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 2);
    ph_free(&h);
}

/* ------------------------------------------------------------------ buttons, select, range, progress, meter */

EMB_TEST(button_gets_a_gray_face_under_its_label)
{
    ph h = ph_make("<button>go</button>", "");
    int f = ph_fill_of(&h.dl, C_FACE);
    EMB_REQUIRE(f >= 0);
    EMB_CHECK(f < nth(&h.dl, EMBI_DI_TEXT, 0));
    ph_free(&h);
    ph s = ph_make("<input type=submit value=send>", "");
    EMB_CHECK(ph_fill_of(&s.dl, C_FACE) >= 0);
    ph_free(&s);
    ph n = ph_make("<button style=\"appearance:none\">go</button>", "");
    EMB_CHECK(ph_fill_of(&n.dl, C_FACE) >= 0); /* the face is the UA background: it stays with appearance: none */
    ph_free(&n);
    ph a = ph_make("<button style=\"background:#f00\">go</button>", "");
    EMB_CHECK_EQ_INT(ph_fill_of(&a.dl, C_FACE), -1);
    ph_free(&a);
    ph tr = ph_make("<button style=\"background:transparent\">go</button>", "");
    EMB_CHECK_EQ_INT(ph_fill_of(&tr.dl, C_FACE), -1);
    ph_free(&tr);
}

EMB_TEST(select_shows_the_selected_label_and_an_arrow)
{
    ph h = ph_make("<select id=s><option>one<option selected>two</select>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TRIANGLE), 1);
    int t = nth(&h.dl, EMBI_DI_TEXT, 0);
    EMB_REQUIRE(t >= 0);
    const embi_lay_shaped *run = (const embi_lay_shaped *)h.dl.runs.data[h.dl.items.data[t].aux];
    EMB_CHECK(strncmp(embi_tshim_bytes(run), "two", 3) == 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    ph_free(&h);
}

EMB_TEST(an_open_select_draws_its_options_in_a_popup_over_the_page)
{
    ph h = ph_make("<select id=s><option>one<option>two<option>three</select><p>below</p>", "");
    static const embi_paint_opt opts[3] = {{"one", 3, true, false}, {"two", 3, false, false}, {"three", 5, false, false}};
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_SELECT;
    st.text = "one";
    st.text_len = 3;
    st.opts = opts;
    st.nopts = 3;
    st.open = true;
    st.hot = 1;
    repaint(&h, "s", &st);
    /* label + three rows + the paragraph */
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 5);
    int hot = ph_fill_of(&h.dl, C_BLUE);
    EMB_REQUIRE(hot >= 0);
    EMB_CHECK(fabsf(h.dl.items.data[hot].rect[3] - EMBI_CTL_ROW_H) < 0.01f);
    /* the popup is the last thing painted */
    EMB_CHECK(hot > nth(&h.dl, EMBI_DI_TEXT, 1));
    st.open = false;
    repaint(&h, "s", &st);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 2);
    ph_free(&h);
}

EMB_TEST(a_list_box_draws_rows_and_highlights_the_selected_ones)
{
    ph h = ph_make("<select id=s multiple><option>a<option>b<option>c</select>", "");
    static const embi_paint_opt opts[3] = {{"a", 1, false, false}, {"b", 1, true, false}, {"c", 1, false, false}};
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    st.kind = EMBI_PCTL_LISTBOX;
    st.opts = opts;
    st.nopts = 3;
    st.focused = true;
    repaint(&h, "s", &st);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 3);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TRIANGLE), 0);
    int sel = ph_fill_of(&h.dl, C_BLUE);
    EMB_REQUIRE(sel >= 0);
    EMB_CHECK(fabsf(h.dl.items.data[sel].rect[3] - EMBI_CTL_ROW_H) < 0.01f);
    ph_free(&h);
}

EMB_TEST(range_has_a_track_a_filled_part_and_a_round_thumb)
{
    ph h = ph_make("<input type=range id=r min=0 max=10 value=5>", "");
    int th = fill_w(&h.dl, C_BLUE, 16.0f);
    EMB_REQUIRE(th >= 0);
    EMB_CHECK(h.dl.items.data[th].radii[0] >= 7.9f);
    /* the thumb stands in the middle of the 129px track */
    float cx = h.dl.items.data[th].rect[0] + 8.0f;
    EMB_CHECK(fabsf(cx - 64.5f) < 2.0f);
    ph_free(&h);
    ph e = ph_make("<input type=range min=0 max=10 value=10>", "");
    int te = fill_w(&e.dl, C_BLUE, 16.0f);
    EMB_REQUIRE(te >= 0);
    EMB_CHECK(e.dl.items.data[te].rect[0] > 100.0f);
    ph_free(&e);
}

EMB_TEST(progress_fills_in_proportion_to_its_value)
{
    ph h = ph_make("<progress value=25 max=100></progress>", "");
    int bar = fill_w(&h.dl, C_BLUE, 40.0f);
    EMB_CHECK(bar >= 0);
    ph_free(&h);
    ph i = ph_make("<progress></progress>", "");
    int ib = fill_w(&i.dl, C_BLUE, 48.0f); /* the indeterminate segment is 30% of 160 */
    EMB_CHECK(ib >= 0);
    ph_free(&i);
}

EMB_TEST(meter_color_follows_the_region_of_the_value)
{
    ph g = ph_make("<meter value=50 min=0 max=100 low=25 high=75 optimum=50></meter>", "");
    EMB_CHECK(ph_fill_of(&g.dl, 0xFF2DA210u) >= 0); /* green */
    ph_free(&g);
    ph y = ph_make("<meter value=90 min=0 max=100 low=25 high=75 optimum=50></meter>", "");
    EMB_CHECK(ph_fill_of(&y.dl, 0xFF07C1FFu) >= 0); /* yellow: the region next to the optimum's */
    ph_free(&y);
    ph r = ph_make("<meter value=90 min=0 max=100 low=25 high=75 optimum=10></meter>", "");
    EMB_CHECK(ph_fill_of(&r.dl, 0xFF4535DCu) >= 0); /* red: the opposite region */
    ph_free(&r);
}

static size_t leak_of(const char *html)
{
    for (int i = 0; i < 2; i++) {
        ph w = ph_make(html, "");
        ph_free(&w);
    }
    size_t before = embi_mem_live();
    ph h = ph_make(html, "");
    ph_free(&h);
    return embi_mem_live() - before;
}

EMB_TEST(display_lists_free_the_runs_they_shaped)
{
    EMB_CHECK_EQ_INT((int)leak_of("<p>plain</p>"), 0);
    EMB_CHECK_EQ_INT((int)leak_of("<input value=abc>"), 0);
    EMB_CHECK_EQ_INT((int)leak_of("<textarea>x y</textarea>"), 0);
    EMB_CHECK_EQ_INT((int)leak_of("<textarea>x\ny</textarea><input value=abc><select><option>q</select>"), 0);
    EMB_CHECK_EQ_INT((int)leak_of("<select><option>q</select>"), 0);
    EMB_CHECK_EQ_INT((int)leak_of("<input type=password value=abc>"), 0);
}

EMB_TEST_MAIN()
