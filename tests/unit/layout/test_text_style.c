/*
 * The text style that layout reads from the computed style (embi_lay_text_style): font-optical-sizing.
 */
#include "layout_harness.h"

#include "layout/layout_int.h"

static bool optical_sizing_of(lh *h, const char *id)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr) {
        return false;
    }
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(embi_style_get(h->style, el, EMBI_PE_NONE), &ts, fams, 16);
    return ts.optical_sizing;
}

EMB_TEST(font_optical_sizing_none_turns_optical_sizing_off)
{
    lh h = lh_make("<body><p id=a>a</p><p id=n>n</p><div style='font-optical-sizing:none'><p id=i>i</p></div></body>",
                   "#n{font-optical-sizing:none}");
    EMB_CHECK(optical_sizing_of(&h, "a"));
    EMB_CHECK(!optical_sizing_of(&h, "n"));
    EMB_CHECK(!optical_sizing_of(&h, "i")); /* inherited */
    lh_free(&h);
}

EMB_TEST_MAIN()
