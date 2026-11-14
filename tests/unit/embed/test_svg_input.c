/*
 * Input on inline SVG in a view (R4c): a click on a shape inside an SVG a link navigates, the keyboard reaches SVG links,
 * and the HTML content of a foreignObject is hit tested where the SVG walk drew it.
 */
#include "unit/embed/input_rig.h"

#include <SDL3/SDL_stdinc.h>

static void click_at(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

static const char k_links[] =
    "<!doctype html><body style=\"margin:0\"><svg width=\"200\" height=\"100\">"
    "<a id=\"l1\" href=\"/svg-go\"><rect id=\"r1\" width=\"40\" height=\"40\" fill=\"red\"/></a>"
    "<a id=\"l2\" xlink:href=\"/xlink-go\"><circle id=\"c2\" cx=\"100\" cy=\"20\" r=\"15\"/></a>"
    "<a id=\"l3\"><rect id=\"r3\" x=\"150\" width=\"40\" height=\"40\"/></a>"
    "</svg></body>";

EMB_TEST(click_on_shape_in_svg_link_navigates)
{
    SETUP(k_links);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, rig_nav, nullptr);
    rig_log(v, "'click'");
    click_at(v, 20.0f, 20.0f);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:r1");
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    EMB_CHECK_EQ_STR(g_nav_url, "http://127.0.0.1/svg-go");
    /* xlink:href, when there is no href */
    click_at(v, 100.0f, 20.0f);
    EMB_CHECK_EQ_INT(g_nav_calls, 2);
    EMB_CHECK_EQ_STR(g_nav_url, "http://127.0.0.1/xlink-go");
    /* an a without either is not a link */
    click_at(v, 170.0f, 20.0f);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:r1,click:c2,click:r3");
    EMB_CHECK_EQ_INT(g_nav_calls, 2);
    rig_close(v, g, rt);
}

EMB_TEST(keyboard_reaches_svg_links)
{
    SETUP(k_links);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, rig_nav, nullptr);
    /* SVG links are in the sequential focus order (the a without a link is not) */
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "l1");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "l2");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    EMB_CHECK_EQ_STR(g_nav_url, "http://127.0.0.1/xlink-go");
    rig_close(v, g, rt);
}

static const char k_foreign[] =
    "<!doctype html><body style=\"margin:0\"><svg id=\"s\" width=\"300\" height=\"200\">"
    "<foreignObject x=\"100\" y=\"10\" width=\"80\" height=\"50\">"
    "<div id=\"d\" style=\"height:30px;background:#0f0\"></div><div id=\"e\" style=\"height:100px\"></div></foreignObject>"
    "<g transform=\"translate(0 100)\"><foreignObject x=\"20\" y=\"10\" width=\"60\" height=\"40\">"
    "<a id=\"fl\" href=\"/fo-go\" style=\"display:block;height:20px\">link</a></foreignObject></g>"
    "</svg></body>";

EMB_TEST(foreign_object_content_is_hit)
{
    SETUP(k_foreign);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, rig_nav, nullptr);
    rig_log(v, "'click'");
    click_at(v, 110.0f, 20.0f); /* the first div, 10 px into the foreignObject */
    click_at(v, 110.0f, 50.0f); /* the second div */
    click_at(v, 110.0f, 70.0f); /* below the foreignObject: its viewport clips the second div, so the svg is hit */
    click_at(v, 50.0f, 20.0f);  /* left of it */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:d,click:e,click:s,click:s");
    /* through the transform of an SVG group: an HTML link inside navigates */
    click_at(v, 30.0f, 115.0f);
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    EMB_CHECK_EQ_STR(g_nav_url, "http://127.0.0.1/fo-go");
    /* a change inside the foreignObject lays its content out again: the first div shrinks, the second moves up */
    (void)run(v, "log = []; document.getElementById('d').style.height = '5px'; true");
    (void)embi_view_update(v);
    click_at(v, 110.0f, 20.0f);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:e");
    /* and a change of the foreignObject's own size: the viewport grows */
    (void)run(v, "log = []; document.querySelector('foreignObject').setAttribute('height', '100'); true");
    (void)embi_view_update(v);
    click_at(v, 110.0f, 70.0f);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:e");
    rig_close(v, g, rt);
}

RIG_MAIN()
