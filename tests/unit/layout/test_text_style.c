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

static uint16_t text_rendering_of(lh *h, const char *id)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr) {
        return EMBI_KW_INVALID;
    }
    const embi_value *v = embi_style_value(embi_style_get(h->style, el, EMBI_PE_NONE), EMBI_PROP_text_rendering);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : EMBI_KW_INVALID;
}

EMB_TEST(text_rendering_parses_inherits_and_rejects_junk)
{
    lh h = lh_make("<body><p id=a>a</p><p id=g>g</p><div id=d><p id=i>i</p></div><p id=j>j</p><p id=k>k</p>"
                   "<svg><text id=s text-rendering='geometricPrecision'>s</text></svg></body>",
                   "#g{text-rendering:geometricPrecision} #d{text-rendering:optimizeSpeed} #j{text-rendering:junk} "
                   "#k{text-rendering:OptimizeLegibility}");
    EMB_CHECK(text_rendering_of(&h, "a") == EMBI_KW_auto);
    EMB_CHECK(text_rendering_of(&h, "g") == EMBI_KW_geometricprecision);
    EMB_CHECK(text_rendering_of(&h, "i") == EMBI_KW_optimizespeed); /* inherited */
    EMB_CHECK(text_rendering_of(&h, "j") == EMBI_KW_auto);          /* junk is dropped */
    EMB_CHECK(text_rendering_of(&h, "k") == EMBI_KW_optimizelegibility);
    EMB_CHECK(text_rendering_of(&h, "s") == EMBI_KW_geometricprecision); /* the SVG presentation attribute */
    /* Serialized in camel case, as browsers do. */
    const embi_value *v = embi_style_value(embi_style_get(h.style, lh_id(&h, "g"), EMBI_PE_NONE), EMBI_PROP_text_rendering);
    embi_sb sb = {nullptr, 0, 0};
    embi_value_serialize(&sb, v);
    EMB_CHECK(sb.len == 18 && SDL_memcmp(sb.data, "geometricPrecision", 18) == 0);
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST_MAIN()
