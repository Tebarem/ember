/*
 * Intrinsic sizes of form controls (W5e, box.c replaced_for): text fields from size and the font, textarea from cols and rows,
 * select from its options, list box from its rows. The page sets the font size so that the numbers do not depend on the UA sheet.
 */
#include "paint_harness.h"

#include <math.h>

#define FONT "font-size:13.3333px;font-family:sans-serif;"

static void size_of(ph *h, const char *id, float *w, float *hh)
{
    embi_rectf r = {0};
    *w = 0.0f; /* EMB_REQUIRE returns early on failure: the outputs are set either way */
    *hh = 0.0f;
    EMB_REQUIRE(embi_layout_border_box(h->eng, ph_id(h, id), &r));
    *w = r.w;
    *hh = r.h;
}

EMB_TEST(text_input_is_177_by_21_at_the_default_font)
{
    ph h = ph_make("<input id=a style='" FONT "'>", "");
    float w, ht;
    size_of(&h, "a", &w, &ht);
    EMB_CHECK_EQ_INT((int)w, 177);
    EMB_CHECK_EQ_INT((int)ht, 21);
    ph_free(&h);
}

EMB_TEST(text_input_width_follows_size_and_the_font)
{
    ph h = ph_make("<input id=a size=10 style='" FONT "'><input id=b size=10 style='font-size:26.6666px'>", "");
    float w, ht, w2, h2;
    size_of(&h, "a", &w, &ht);
    size_of(&h, "b", &w2, &h2);
    EMB_CHECK_EQ_INT((int)w, 93);
    EMB_CHECK(w2 > w * 1.8f);
    EMB_CHECK(h2 > ht);
    ph_free(&h);
}

EMB_TEST(password_and_search_are_sized_like_text)
{
    ph h = ph_make("<input id=a type=password style='" FONT "'><input id=b type=search style='" FONT "'>", "");
    float w, ht, w2, h2;
    size_of(&h, "a", &w, &ht);
    size_of(&h, "b", &w2, &h2);
    EMB_CHECK_EQ_INT((int)w, 177);
    EMB_CHECK_EQ_INT((int)w2, 177);
    ph_free(&h);
}

EMB_TEST(textarea_is_sized_by_cols_and_rows)
{
    ph h = ph_make("<textarea id=a style='" FONT "'></textarea><textarea id=b cols=40 rows=5 style='" FONT "'></textarea>", "");
    float w, ht, w2, h2;
    size_of(&h, "a", &w, &ht);
    size_of(&h, "b", &w2, &h2);
    EMB_CHECK(w2 > w * 1.9f);
    EMB_CHECK(h2 > ht * 2.0f);
    EMB_CHECK_EQ_INT((int)ht, 36); /* two 15px lines + 6 */
    ph_free(&h);
}

EMB_TEST(select_is_as_wide_as_its_widest_option)
{
    ph h = ph_make("<select id=a style='" FONT "'><option>a</select>"
                   "<select id=b style='" FONT "'><option>a<option>a much longer option</select>",
                   "");
    float w, ht, w2, h2;
    size_of(&h, "a", &w, &ht);
    size_of(&h, "b", &w2, &h2);
    EMB_CHECK(w2 > w + 60.0f);
    EMB_CHECK(ht >= 19.0f);
    EMB_CHECK_EQ_INT((int)ht, (int)h2);
    ph_free(&h);
}

EMB_TEST(list_box_shows_size_rows)
{
    ph h = ph_make("<select id=a multiple style='" FONT "'><option>a<option>b</select>"
                   "<select id=b size=6 style='" FONT "'><option>a</select>",
                   "");
    float w, ht, w2, h2;
    size_of(&h, "a", &w, &ht);
    size_of(&h, "b", &w2, &h2);
    EMB_CHECK_EQ_INT((int)ht, 4 * 24);
    EMB_CHECK_EQ_INT((int)h2, 6 * 24);
    ph_free(&h);
}

EMB_TEST_MAIN()
