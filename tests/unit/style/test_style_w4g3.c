/*
 * Style behavior added by the W4g3 burn-down: attr() (restyle after an attribute change, type(), unit idents, the
 * fallback, the case-insensitive name, never style-shared), the lh unit, revert-layer, calc() rounding for integer
 * properties, image(<color>) and calc() in media queries.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <stdio.h>
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
    if (ok) {
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

static bool has_w1(fx *f, const char *id)
{
    char t[128];
    return value(f, id, "width", t, sizeof t) && strcmp(t, "1px") == 0;
}

static void set_attr(fx *f, const char *id, const char *name, const char *v)
{
    EMB_REQUIRE(embi_el_set_attribute(by_id(f, id), S(name), embi_dstr_from_utf8(f->doc->slab, S(v))) == EMBI_DOM_OK);
}

/* ------------------------------------------------------------------ attr() */

EMB_TEST(attr_value_follows_a_neutral_attribute_change)
{
    /* no selector reads data-w, yet the computed width depends on it */
    fx f = make("<div id=a data-w=40></div>", "div { width: attr(data-w px); }");
    EMB_CHECK(is(&f, "a", "width", "40px"));
    set_attr(&f, "a", "data-w", "80");
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(is(&f, "a", "width", "80px"));
    destroy(&f);
}

EMB_TEST(attr_type_unit_fallback_and_name_case)
{
    fx f = make("<div id=a data-h=12px data-w=5></div><div id=b></div>",
                "#a { height: attr(data-h type(<length>)); width: attr(DATA-W px); }"
                "#b { width: attr(data-none px, 7px); }");
    EMB_CHECK(is(&f, "a", "height", "12px"));
    EMB_CHECK(is(&f, "a", "width", "5px")); /* the name matches case-insensitively in an HTML element */
    EMB_CHECK(is(&f, "b", "width", "7px"));  /* absent attribute: the fallback */
    destroy(&f);
}

EMB_TEST(attr_values_are_never_style_shared)
{
    fx f = make("<p><i id=a class=k data-w=10></i><i id=b class=k data-w=20></i><i id=c class=k data-w=10></i></p>",
                ".k { display: block; width: attr(data-w px); }");
    EMB_CHECK(is(&f, "a", "width", "10px"));
    EMB_CHECK(is(&f, "b", "width", "20px"));
    EMB_CHECK(is(&f, "c", "width", "10px"));
    destroy(&f);
}

/* ------------------------------------------------------------------ lh */

EMB_TEST(lh_unit_uses_the_computed_line_height)
{
    fx f = make("<div id=a><p id=b></p></div><div id=c></div>",
                "#a { line-height: 20px; width: 2lh; }"
                "#b { line-height: 30px; width: 1lh; margin-left: calc(1lh + 1px); }"
                "#c { font-size: 10px; line-height: 1.5; width: 1lh; }");
    EMB_CHECK(is(&f, "a", "width", "40px"));
    EMB_CHECK(is(&f, "b", "width", "30px"));
    EMB_CHECK(is(&f, "b", "margin-left", "31px"));
    EMB_CHECK(is(&f, "c", "width", "15px"));
    destroy(&f);
}

/* ------------------------------------------------------------------ revert-layer */

EMB_TEST(revert_layer_falls_back_to_the_lower_layer)
{
    fx f = make("<p id=a></p><p id=b></p>",
                "@layer one { #a { width: 10px; } } @layer two { #a { width: revert-layer; } }"
                "@layer two { #b { width: 5px; } }"
                "#b { width: revert-layer; }");
    EMB_CHECK(is(&f, "a", "width", "10px"));
    /* the unlayered rule reverts to the layers below it */
    EMB_CHECK(is(&f, "b", "width", "5px"));
    destroy(&f);
}

EMB_TEST(revert_layer_without_a_lower_layer_reaches_the_previous_origin)
{
    /* nothing below in the author origin: the UA margin of p (1em) applies */
    fx f = make("<p id=a style=\"font-size:10px\"></p>", "@layer one { #a { margin-top: revert-layer; } }");
    EMB_CHECK(is(&f, "a", "margin-top", "10px"));
    destroy(&f);
}

/* ------------------------------------------------------------------ calc() rounding, image(<color>) */

EMB_TEST(calc_rounds_for_integer_properties)
{
    fx f = make("<div id=a></div><div id=b></div><div id=c></div>",
                "#a { z-index: calc(1.5); order: calc(2.4); }"
                "#b { z-index: calc(-1.5); order: calc(2.5); }"
                "#c { z-index: calc(1 + 2 * 1.2); }");
    EMB_CHECK(is(&f, "a", "z-index", "2"));
    EMB_CHECK(is(&f, "a", "order", "2"));
    EMB_CHECK(is(&f, "b", "z-index", "-1"));
    EMB_CHECK(is(&f, "b", "order", "3"));
    EMB_CHECK(is(&f, "c", "z-index", "3"));
    destroy(&f);
}

EMB_TEST(image_function_with_a_color_is_a_background_image)
{
    fx f = make("<div id=a></div><div id=b></div>",
                "#a { background-image: image(rgb(255, 0, 0)); }"
                "#b { background-image: image(rgb(1 2 3 / 50%)); }");
    char t[128];
    EMB_REQUIRE(value(&f, "a", "background-image", t, sizeof t));
    EMB_CHECK(strcmp(t, "none") != 0 && t[0] != '\0');
    EMB_REQUIRE(value(&f, "b", "background-image", t, sizeof t));
    EMB_CHECK(strcmp(t, "none") != 0 && t[0] != '\0');
    destroy(&f);
}

/* ------------------------------------------------------------------ calc() in media queries */

EMB_TEST(media_queries_evaluate_calc)
{
    /* the default viewport is 800 px wide */
    fx f = make("<p id=a></p><p id=b></p><p id=c></p><p id=d></p>",
                "@media (min-width: calc(100px * 5)) { #a { width: 1px; } }"
                "@media (min-width: calc(100px * 9)) { #b { width: 1px; } }"
                "@media (min-width: calc(100vw + 1px)) { #c { width: 1px; } }"
                "@media (max-width: calc(100vw - 10px)) { #d { width: 1px; } }");
    EMB_CHECK(has_w1(&f, "a"));
    EMB_CHECK(!has_w1(&f, "b"));
    EMB_CHECK(!has_w1(&f, "c")); /* vw is the real viewport: never at least 100vw + 1px */
    EMB_CHECK(!has_w1(&f, "d")); /* 790px: the viewport is wider */
    destroy(&f);
}

EMB_TEST_MAIN()
