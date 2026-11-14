/*
 * Style behavior added by W4g5: the `all` shorthand (every longhand but direction and unicode-bidi), the font-relative
 * units measured in the computed face, and the text-decoration-inset and box-decoration-break properties.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fx {
    embi_style_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
} fx;

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static fx make(const char *html, const char *css)
{
    fx f;
    memset(&f, 0, sizeof f);
    f.doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(f.doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    f.eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    f.sheet = embi_css_sheet_parse(S(css), &opts);
    embi_css_sheet *sheets[1] = {f.sheet};
    embi_document_set_author_sheets(f.eng, f.doc, sheets, 1);
    embi_style_resolve_document(f.eng, f.doc);
    return f;
}

static void destroy(fx *f)
{
    embi_style_forget_document(f->eng, f->doc);
    embi_document_free(f->doc);
    embi_style_engine_destroy(f->eng);
    embi_css_sheet_unref(f->sheet);
}

static embi_element *by_id(fx *f, const char *id)
{
    embi_element *el = embi_get_element_by_id(f->doc, S(id));
    EMB_CHECK(el != nullptr);
    return el;
}

/* The serialized computed value of a property (of a pseudo element when pe is not NONE), "" when it has none. */
static bool value_pe(fx *f, const char *id, embi_sel_pseudo pe, const char *prop, char *out, size_t cap)
{
    out[0] = '\0';
    embi_element *el = by_id(f, id);
    const embi_style *s = el != nullptr ? embi_style_get(f->eng, el, pe) : nullptr;
    embi_prop_id pid = embi_prop_lookup(S(prop));
    embi_sb sb = {nullptr, 0, 0};
    bool ok = s != nullptr && pid != EMBI_PROP_NONE && embi_style_value_text(s, pid, &sb);
    size_t n = ok ? sb.len : 0;
    if (n >= cap) {
        n = cap - 1;
    }
    if (ok && n > 0) {
        memcpy(out, sb.data, n);
    }
    out[n] = '\0';
    embi_sb_free(&sb);
    return ok;
}

static bool value(fx *f, const char *id, const char *prop, char *out, size_t cap)
{
    return value_pe(f, id, EMBI_PE_NONE, prop, out, cap);
}

static bool is(fx *f, const char *id, const char *prop, const char *want)
{
    char t[128];
    bool ok = value(f, id, prop, t, sizeof t) && strcmp(t, want) == 0;
    if (!ok) {
        fprintf(stderr, "  #%s %s: want %s got %s\n", id, prop, want, t);
    }
    return ok;
}



/* ------------------------------------------------------------------ all */

EMB_TEST(all_initial_resets_everything_but_direction_and_unicode_bidi)
{
    fx f = make("<div id=a></div>",
                "#a { width: 10px; color: red; direction: rtl; unicode-bidi: bidi-override; all: initial; }");
    EMB_CHECK(is(&f, "a", "width", "auto"));
    EMB_CHECK(is(&f, "a", "direction", "rtl"));
    EMB_CHECK(is(&f, "a", "unicode-bidi", "bidi-override"));
    destroy(&f);
}

EMB_TEST(all_inherit_and_a_later_longhand_wins)
{
    fx f = make("<div id=p style=\"padding-left: 7px; color: green\"><div id=a></div><div id=b></div></div>",
                "#a { all: inherit; } #b { all: inherit; padding-left: 3px; }");
    EMB_CHECK(is(&f, "a", "padding-left", "7px"));
    EMB_CHECK(is(&f, "b", "padding-left", "3px"));
    destroy(&f);
}

EMB_TEST(all_rejects_values_that_are_not_css_wide_keywords)
{
    fx f = make("<div id=a style=\"width: 5px\"></div>", "#a { all: 4px; }");
    EMB_CHECK(is(&f, "a", "width", "5px"));
    destroy(&f);
}

/* ------------------------------------------------------------------ decoration properties */

EMB_TEST(text_decoration_inset_and_box_decoration_break_parse)
{
    fx f = make("<u id=a></u><u id=b></u><u id=c></u>",
                "#a { text-decoration-inset: 5px; box-decoration-break: clone; }"
                "#b { text-decoration-inset: 2px 10%; }"
                "#c { text-decoration-inset: bogus; }");
    EMB_CHECK(is(&f, "a", "text-decoration-inset", "5px 5px"));
    EMB_CHECK(is(&f, "a", "box-decoration-break", "clone"));
    EMB_CHECK(is(&f, "b", "text-decoration-inset", "2px 10%"));
    EMB_CHECK(is(&f, "c", "text-decoration-inset", "0px 0px")); /* the initial value is 0, `auto` is a keyword of its own */
    EMB_CHECK(is(&f, "c", "box-decoration-break", "slice"));
    destroy(&f);
}

EMB_TEST(text_decoration_inset_auto_is_a_keyword_and_not_the_initial_value)
{
    fx f = make("<u id=a></u><u id=b></u>", "#a { text-decoration-inset: auto; }");
    EMB_CHECK(is(&f, "a", "text-decoration-inset", "auto"));
    EMB_CHECK(is(&f, "b", "text-decoration-inset", "0px 0px"));
    destroy(&f);
}

EMB_TEST(the_prefixed_box_decoration_break_is_an_alias)
{
    fx f = make("<u id=a></u>", "#a { -webkit-box-decoration-break: clone; }");
    EMB_CHECK(is(&f, "a", "box-decoration-break", "clone"));
    destroy(&f);
}

EMB_TEST(text_decoration_skip_spaces_parses_none_all_and_start_end)
{
    fx f = make("<u id=a></u><u id=b></u><u id=c></u><u id=d></u><u id=e></u>",
                "#a { text-decoration-skip-spaces: none; } #b { text-decoration-skip-spaces: all; }"
                "#c { text-decoration-skip-spaces: end start; } #d { text-decoration-skip-spaces: start start; }");
    EMB_CHECK(is(&f, "a", "text-decoration-skip-spaces", "none"));
    EMB_CHECK(is(&f, "b", "text-decoration-skip-spaces", "all"));
    EMB_CHECK(is(&f, "c", "text-decoration-skip-spaces", "end start"));
    EMB_CHECK(is(&f, "d", "text-decoration-skip-spaces", "start end")); /* invalid: the initial value */
    EMB_CHECK(is(&f, "e", "text-decoration-skip-spaces", "start end"));
    destroy(&f);
}

EMB_TEST(text_decoration_skip_spaces_is_inherited)
{
    fx f = make("<div id=a><span id=b></span></div>", "#a { text-decoration-skip-spaces: none; }");
    EMB_CHECK(is(&f, "b", "text-decoration-skip-spaces", "none"));
    destroy(&f);
}

EMB_TEST(white_space_trim_and_text_fill_color_and_border_area_parse)
{
    fx f = make("<span id=a></span><span id=b></span><span id=c></span><span id=d></span>",
                "#a { white-space-trim: discard-before discard-inner; } #b { white-space-trim: discard-after discard-after; }"
                "#c { -webkit-text-fill-color: red; background-clip: border-area, padding-box; }");
    EMB_CHECK(is(&f, "a", "white-space-trim", "discard-before discard-inner"));
    EMB_CHECK(is(&f, "b", "white-space-trim", "none")); /* a keyword twice is invalid */
    EMB_CHECK(is(&f, "d", "white-space-trim", "none"));
    EMB_CHECK(is(&f, "c", "background-clip", "border-area, padding-box"));
    char t[64];
    EMB_CHECK(value(&f, "c", "-webkit-text-fill-color", t, sizeof t) && t[0] != 0);
    destroy(&f);
}

EMB_TEST(image_function_with_url_fallbacks_and_a_color_parses)
{
    fx f = make("<div id=a></div><div id=b></div><div id=c></div>",
                "#a { background-image: image(\"x.png\", \"y.png\", red); } #b { background-image: image(red, \"x.png\"); }"
                "#c { background-image: image(blue); }");
    char t[128];
    EMB_CHECK(value(&f, "a", "background-image", t, sizeof t) && strcmp(t, "none") != 0);
    EMB_CHECK(value(&f, "b", "background-image", t, sizeof t) && strcmp(t, "none") == 0); /* a color must come last */
    EMB_CHECK(value(&f, "c", "background-image", t, sizeof t) && strcmp(t, "none") != 0);
    destroy(&f);
}

/* ------------------------------------------------------------------ font-relative units */

static double px_of(fx *f, const char *id, const char *prop)
{
    char t[64];
    double v = -1.0;
    if (value(f, id, prop, t, sizeof t)) {
        v = atof(t);
    }
    return v;
}

EMB_TEST(ex_ch_and_ic_resolve_against_the_computed_font_and_scale_with_its_size)
{
    fx f = make("<div id=a style=\"font-size:20px\"><i id=ex style=\"width:1ex\"></i><i id=ch style=\"width:1ch\"></i>"
                "<i id=ic style=\"width:1ic\"></i><i id=em style=\"width:1em\"></i></div>"
                "<div id=b style=\"font-size:40px\"><i id=ex2 style=\"width:1ex\"></i><i id=ch2 style=\"width:1ch\"></i></div>",
                "i { display: block; }");
    double ex = px_of(&f, "ex", "width"), ch = px_of(&f, "ch", "width"), ic = px_of(&f, "ic", "width");
    double em = px_of(&f, "em", "width");
    EMB_CHECK(em == 20.0);
    EMB_CHECK(ex > 5.0 && ex < 15.0);   /* an x-height is about half an em */
    EMB_CHECK(ch > 6.0 && ch < 16.0);   /* the advance of "0" is about 0.55 em */
    EMB_CHECK(ic > 0.0 && ic <= 40.0);  /* the advance of a CJK ideograph: 1em when the face has none */
    EMB_CHECK(fabs(px_of(&f, "ex2", "width") - 2.0 * ex) < 1.0);
    EMB_CHECK(fabs(px_of(&f, "ch2", "width") - 2.0 * ch) < 1.0);
    destroy(&f);
}

EMB_TEST(font_relative_units_in_calc_follow_the_same_face)
{
    fx f = make("<div id=a style=\"font-size:20px\"><i id=x style=\"width:calc(2ch + 4px)\"></i>"
                "<i id=y style=\"width:1ch\"></i></div>", "i { display: block; }");
    EMB_CHECK(fabs(px_of(&f, "x", "width") - (2.0 * px_of(&f, "y", "width") + 4.0)) < 0.5);
    destroy(&f);
}

EMB_TEST(sibling_index_and_count_are_substituted_per_element)
{
    fx f = make("<div id=p><i id=a></i><b id=m></b><i id=c></i></div>",
                "i, b { display: block; width: calc(10px * sibling-index()); height: calc(1px * sibling-count()); }");
    EMB_CHECK(fabs(px_of(&f, "a", "width") - 10.0) < 0.01);
    EMB_CHECK(fabs(px_of(&f, "m", "width") - 20.0) < 0.01);
    EMB_CHECK(fabs(px_of(&f, "c", "width") - 30.0) < 0.01);
    EMB_CHECK(fabs(px_of(&f, "c", "height") - 3.0) < 0.01);
    destroy(&f);
}

EMB_TEST(letter_spacing_accepts_a_percentage)
{
    fx f = make("<div id=a style=\"letter-spacing: 50%\"></div>", "");
    EMB_CHECK(is(&f, "a", "letter-spacing", "50%"));
    destroy(&f);
}

EMB_TEST_MAIN()
