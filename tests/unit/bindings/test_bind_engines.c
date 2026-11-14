/*
 * Binding tests (W5a), part 3: the members that need the style and layout engines (getComputedStyle,
 * innerText, getBoundingClientRect, offset*, client*, scrollIntoView) and the viewport hook
 * (matchMedia change events), run against a small styled document with real engines.
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "css/css.h"
#include "css/props.h"
#include "html/parser.h"
#include "js/js.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <string.h>

typedef struct env {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *layout;
    embi_css_sheet *sheet;
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
    double vw, vh;
    double scroll_y;
    int scrolls;
} env;

static const char page_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=box>box</div>"
    "<div id=t>a<span id=hid>X</span><span id=inl>b</span></div>"
    "<div id=far>far</div>"
    "</body></html>";

static const char page_css[] =
    "body{margin:0}"
    "#box{display:block;position:relative;width:120px;height:40px;margin-left:10px;padding:3px;"
    "border:1px solid;color:rgb(255,0,0)}"
    "#far{display:block;height:50px;margin-top:2000px}"
    "#hid{display:none}"
    "#inl{display:inline}";

static void host_viewport(void *user, double *w, double *h, double *dpr)
{
    env *e = user;
    *w = e->vw;
    *h = e->vh;
    *dpr = 1;
}

static void host_scroll(void *user, double x, double y)
{
    env *e = user;
    (void)x;
    e->scroll_y = y;
    e->scrolls++;
}

static void env_init(env *e)
{
    memset(e, 0, sizeof *e);
    e->vw = 1024;
    e->vh = 768;
    e->text = embi_tshim_create();
    e->style = embi_style_engine_create(embi_tshim_fonts(e->text), nullptr, nullptr);
    e->doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(e->doc, nullptr);
    embi_html_parser_write(p, page_html, sizeof page_html - 1);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    e->sheet = embi_css_sheet_parse((embi_str){page_css, (uint32_t)(sizeof page_css - 1)}, &opts);
    embi_css_sheet *sheets[1] = {e->sheet};
    embi_document_set_author_sheets(e->style, e->doc, sheets, 1);
    embi_style_resolve_document(e->style, e->doc);
    e->layout = embi_layout_engine_create(e->text, e->style, nullptr);
    embi_layout_set_viewport(e->layout, 1024, 768);
    e->rt = embi_js_runtime_new();
    e->c = embi_bind_new(e->rt, e->doc);
    embi_bind_host h = {.user = e,
                        .viewport = host_viewport,
                        .scroll = host_scroll,
                        .style = e->style,
                        .layout = e->layout};
    embi_bind_set_host(e->c, &h);
}

static void env_done(env *e)
{
    embi_bind_free(e->c);
    embi_js_runtime_free(e->rt);
    embi_layout_engine_destroy(e->layout);
    embi_style_forget_document(e->style, e->doc);
    embi_document_free(e->doc);
    embi_css_sheet_unref(e->sheet);
    embi_style_engine_destroy(e->style);
    embi_tshim_destroy(e->text);
}

static bool js_true(env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e->rt);
        return false;
    }
    if (v != embi_js_from_bool(true)) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(bind_engines_get_computed_style_reads_the_resolved_style)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var b = document.getElementById('box'); var cs = getComputedStyle(b); cs.display === 'block'"));
    EMB_CHECK(js_true(&e, "cs.getPropertyValue('color') === 'rgb(255, 0, 0)'"));
    EMB_CHECK(js_true(&e, "cs.getPropertyValue('display') === 'block' && cs.position === 'relative'"));
    EMB_CHECK(js_true(&e, "getComputedStyle(document.getElementById('hid')).display === 'none'"));
    EMB_CHECK(js_true(&e, "getComputedStyle(document.getElementById('inl')).display === 'inline'"));
    /* the computed style follows a change of the inline style */
    EMB_CHECK(js_true(&e, "b.style.display = 'none'; getComputedStyle(b).display === 'none'"));
    /* a computed declaration is read-only */
    EMB_CHECK(js_true(&e, "cs.setProperty('display', 'block'); getComputedStyle(b).display === 'none'"));
    EMB_CHECK(js_true(&e, "cs.cssText === ''"));
    env_done(&e);
}

EMB_TEST(bind_engines_inner_text_follows_display)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var t = document.getElementById('t'); t.innerText === 'ab'"));
    EMB_CHECK(js_true(&e, "t.textContent === 'aXb'"));
    EMB_CHECK(js_true(&e, "document.getElementById('box').innerText === 'box'"));
    env_done(&e);
}

EMB_TEST(bind_engines_geometry_comes_from_the_layout)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var b = document.getElementById('box'); var r = b.getBoundingClientRect();"
                          "r.x === 10 && r.y === 0 && r.width === 128 && r.height === 48"));
    EMB_CHECK(js_true(&e, "r.left === 10 && r.top === 0 && r.right === 138 && r.bottom === 48"));
    EMB_CHECK(js_true(&e, "b.offsetWidth === 128 && b.offsetHeight === 48 && b.offsetLeft === 10 && b.offsetTop === 0"));
    EMB_CHECK(js_true(&e, "b.clientWidth === 126 && b.clientHeight === 46 && b.clientLeft === 1 && b.clientTop === 1"));
    EMB_CHECK(js_true(&e, "b.getClientRects().length === 1"));
    EMB_CHECK(js_true(&e, "document.getElementById('hid').getBoundingClientRect().width === 0"));
    EMB_CHECK(js_true(&e, "document.getElementById('hid').offsetParent === null"));
    /* a layout-affecting change is seen by the next query */
    EMB_CHECK(js_true(&e, "b.style.width = '200px'; b.getBoundingClientRect().width === 208 && b.offsetWidth === 208"));
    env_done(&e);
}

EMB_TEST(bind_engines_scroll_into_view_uses_the_layout)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var f = document.getElementById('far'); f.getBoundingClientRect().top > 1000"));
    EMB_CHECK(js_true(&e, "f.scrollIntoView(); window.scrollY > 1000"));
    EMB_CHECK(e.scrolls >= 1);
    EMB_CHECK(e.scroll_y > 1000);
    /* the rectangle is relative to the viewport, so it moved up by the scroll offset */
    EMB_CHECK(js_true(&e, "Math.abs(f.getBoundingClientRect().top) < 1"));
    env_done(&e);
}

EMB_TEST(bind_engines_viewport_change_fires_media_query_list_events)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var m = matchMedia('(min-width: 600px)'); var evs = []; m.addEventListener('change', function (ev) { evs.push(ev.matches); });"
                          "var legacy = 0; m.addListener(function () { legacy++; }); m.matches === true && window.innerWidth === 1024"));
    e.vw = 500;
    embi_bind_viewport_changed(e.c);
    EMB_CHECK(js_true(&e, "evs.length === 1 && evs[0] === false && m.matches === false && legacy === 1 && window.innerWidth === 500"));
    /* an unchanged result fires nothing */
    e.vw = 400;
    embi_bind_viewport_changed(e.c);
    EMB_CHECK(js_true(&e, "evs.length === 1"));
    e.vw = 900;
    embi_bind_viewport_changed(e.c);
    EMB_CHECK(js_true(&e, "evs.length === 2 && evs[1] === true && m.matches === true"));
    env_done(&e);
}

EMB_TEST_MAIN()
