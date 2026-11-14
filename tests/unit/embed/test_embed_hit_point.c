/*
 * document.elementFromPoint and elementsFromPoint (W8b, src/bindings/impl_hit.c): the topmost element at a viewport point in
 * paint order (a positioned box over an in-flow one, pointer-events: none skipped), the root element on the canvas, null
 * outside the viewport, the root's scroll offset applied, a TypeError for non-finite or missing arguments, null for a
 * document without a layout, and the ancestor list of elementsFromPoint. Views need an offscreen device: the tests skip (77)
 * without one.
 */
#include "input_rig.h"

/* A 300x200 view: #a (0,0 100x50) with #a1 inside (0,0 50x25), #cover positioned over #b (0,50 100x50), #ghost with
 * pointer-events: none over #c (0,100 100x50), and a tall tail so the page scrolls. */
static const char k_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<div id=a style=\"width:100px;height:50px\"><span id=a1 style=\"display:block;width:50px;height:25px\"></span></div>"
    "<div id=b style=\"width:100px;height:50px\"></div>"
    "<div id=cover style=\"position:absolute;left:0;top:50px;width:60px;height:50px\"></div>"
    "<div id=c style=\"width:100px;height:50px\"></div>"
    "<div id=ghost style=\"position:absolute;left:0;top:100px;width:100px;height:50px;pointer-events:none\"></div>"
    "<div id=tail style=\"height:1000px\"></div>";

static const char k_id_at[] = "(function (x, y) { var e = document.elementFromPoint(x, y); "
                              "return e === null ? 'null' : (e.id || e.tagName); })";

static const char *id_at(emb_view *v, float x, float y)
{
    char js[512];
    SDL_snprintf(js, sizeof js, "%s(%g, %g)", k_id_at, (double)x, (double)y);
    return run(v, js);
}

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

static void paint_order_run(void)
{
    {
        SETUP(k_page);
        EMB_CHECK_EQ_STR(id_at(v, 10, 10), "a1");    /* the innermost box */
        EMB_CHECK_EQ_STR(id_at(v, 75, 10), "a");     /* #a outside #a1 */
        EMB_CHECK_EQ_STR(id_at(v, 10, 60), "cover"); /* the positioned box paints over #b */
        EMB_CHECK_EQ_STR(id_at(v, 80, 60), "b");     /* #b beside the cover */
        EMB_CHECK_EQ_STR(id_at(v, 10, 110), "c");    /* pointer-events: none is not a target */
        EMB_CHECK_EQ_STR(id_at(v, 250, 10), "BODY"); /* the body's box spans the viewport width */
        /* The same element as the input's hit test: a click there reaches it. */
        EMB_CHECK_EQ_STR(run(v, "var hits = []; document.addEventListener('click', function (e) { hits.push(e.target.id); });"
                                "document.elementFromPoint(10, 60).id"),
                         "cover");
        emb_view_mouse_button(v, 10, 60, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(v, 10, 60, EMB_BUTTON_PRIMARY, false, 0);
        EMB_CHECK_EQ_STR(run(v, "hits.join(',')"), "cover");
        rig_close(v, g, rt);
    }
}

EMB_TEST(element_from_point_paint_order)
{
    /* The first run makes the process-wide tables (atoms, the GPU stack); the second must leave nothing behind. */
    paint_order_run();
    const size_t base = embi_mem_live();
    paint_order_run();
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST(element_from_point_viewport_and_scroll)
{
    SETUP(k_page);
    /* The viewport of the realm is the view's (300 x 200 CSS px at 1x), not the realm's default. */
    EMB_CHECK_EQ_STR(run(v, "innerWidth + 'x' + innerHeight + '@' + devicePixelRatio"), "300x200@1");
    EMB_CHECK_EQ_STR(id_at(v, -1, 10), "null");
    EMB_CHECK_EQ_STR(id_at(v, 10, -1), "null");
    EMB_CHECK_EQ_STR(id_at(v, 301, 10), "null");
    EMB_CHECK_EQ_STR(id_at(v, 10, 201), "null");
    /* After a scroll of 50 px the point (10, 10) is over the cover (document y 60). */
    EMB_CHECK_EQ_STR(run(v, "window.scrollTo(0, 50); String(window.scrollY)"), "50");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "cover");
    /* Below the end of the content the root element is the answer (a 20 px page in a 200 px view). */
    rig_close(v, g, rt);

    emb_runtime *rt2 = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt2 != nullptr);
    embi_gpu *g2 = nullptr;
    emb_view *v2 = nullptr;
    OPEN_HTML_OR_SKIP(v2, rt2, g2, "<!doctype html><html style=\"height:20px\"><body style=\"margin:0;height:20px\"></body>");
    EMB_CHECK_EQ_STR(id_at(v2, 10, 150), "HTML");
    rig_close(v2, g2, rt2);
}

EMB_TEST(element_from_point_arguments_and_other_documents)
{
    SETUP(k_page);
    EMB_CHECK_EQ_STR(run(v, "try { document.elementFromPoint(1); 'no' } catch (e) { e.name }"), "TypeError");
    EMB_CHECK_EQ_STR(run(v, "try { document.elementFromPoint(NaN, 1); 'no' } catch (e) { e.name }"), "TypeError");
    EMB_CHECK_EQ_STR(run(v, "try { document.elementsFromPoint(1, Infinity); 'no' } catch (e) { e.name }"), "TypeError");
    EMB_CHECK_EQ_STR(run(v, "try { Document.prototype.elementFromPoint.call({}, 1, 1); 'no' } catch (e) { e.name }"),
                     "TypeError");
    /* ToNumber of the arguments: strings convert. */
    EMB_CHECK_EQ_STR(run(v, "document.elementFromPoint('10', '60').id"), "cover");
    /* A document with no layout of its own answers null and []. */
    EMB_CHECK_EQ_STR(run(v, "var d = document.implementation.createHTMLDocument('x');"
                            "String(d.elementFromPoint(10, 10)) + ',' + d.elementsFromPoint(10, 10).length"),
                     "null,0");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_lists_the_ancestors)
{
    SETUP(k_page);
    EMB_CHECK_EQ_STR(run(v, "document.elementsFromPoint(10, 10).map(function (e) { return e.id || e.tagName; }).join(',')"),
                     "a1,a,BODY,HTML");
    EMB_CHECK_EQ_STR(run(v, "Array.isArray(document.elementsFromPoint(10, 10))"), "true");
    EMB_CHECK_EQ_STR(run(v, "document.elementsFromPoint(-5, 10).length"), "0");
    EMB_CHECK_EQ_STR(run(v, "document.elementsFromPoint(10, 10)[0] === document.getElementById('a1')"), "true");
    rig_close(v, g, rt);
}

/* elementsFromPoint at (x, y) as "id-or-tag,...", "-" for an empty list. */
static const char *list_at(emb_view *v, float x, float y)
{
    char js[640];
    SDL_snprintf(js, sizeof js,
                 "(function (x, y) { var l = document.elementsFromPoint(x, y); return l.length === 0 ? '-' : "
                 "l.map(function (e) { return e.id || e.tagName; }).join(','); })(%g, %g)",
                 (double)x, (double)y);
    return run(v, js);
}

/* 'ok' when elementFromPoint is the first entry of elementsFromPoint (null with an empty list), no element is listed twice,
 * and the list ends with the document element. */
static const char *consistent_at(emb_view *v, float x, float y)
{
    char js[1024];
    SDL_snprintf(js, sizeof js,
                 "(function (x, y) { var f = document.elementFromPoint(x, y); var l = document.elementsFromPoint(x, y);"
                 " if (l.length === 0 ? f !== null : f !== l[0]) return 'first';"
                 " for (var i = 0; i < l.length; i++) { for (var j = 0; j < i; j++) { if (l[j] === l[i]) return 'twice'; } }"
                 " if (l.length > 0 && l[l.length - 1] !== document.documentElement) return 'root';"
                 " return 'ok'; })(%g, %g)",
                 (double)x, (double)y);
    return run(v, js);
}

/* Two overlapping absolute siblings (the body has no height of its own, so it is not hit). */
static const char k_overlap[] =
    "<!doctype html><body style=\"margin:0\">"
    "<div id=p1 style=\"position:absolute;left:0;top:0;width:100px;height:50px\"></div>"
    "<div id=q1 style=\"position:absolute;left:0;top:0;width:60px;height:50px\"></div>";

EMB_TEST(elements_from_point_overlapping_siblings)
{
    SETUP(k_overlap);
    /* both boxes, the later one (painted on top) first, then the root element (the body has no box at this point) */
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "q1,p1,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 80, 10), "p1,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_z_index_reorders)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=p1 style=\"position:absolute;left:0;top:0;width:100px;height:50px;z-index:2\"></div>"
          "<div id=q1 style=\"position:absolute;left:0;top:0;width:60px;height:50px;z-index:1\"></div>");
    /* p1 is earlier in the tree but has the higher z-index */
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "p1,q1,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_pointer_events_none_skipped)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=p1 style=\"position:absolute;left:0;top:0;width:100px;height:50px\"></div>"
          "<div id=q1 style=\"position:absolute;left:0;top:0;width:60px;height:50px;pointer-events:none\"></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "p1,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_visibility_hidden_skipped)
{
    /* h1 is hidden with a visible child h2: the child is a target, h1 is not. q1 is hidden too. */
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=p1 style=\"position:absolute;left:0;top:0;width:100px;height:50px\"></div>"
          "<div id=q1 style=\"position:absolute;left:0;top:0;width:60px;height:50px;visibility:hidden\"></div>"
          "<div id=h1 style=\"position:absolute;left:0;top:0;width:60px;height:50px;visibility:hidden\">"
          "<div id=h2 style=\"visibility:visible;width:30px;height:20px\"></div></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "h2,p1,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 80, 10), "p1,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_child_outside_parent_box)
{
    /* kid is an absolute child of the relative par, placed below par's 50x25 box: par is not listed at the kid's point. */
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=par style=\"position:relative;width:50px;height:25px\">"
          "<div id=kid style=\"position:absolute;left:0;top:100px;width:40px;height:40px\"></div></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 110), "kid,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "par,BODY,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 110), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_transformed_box)
{
    /* t is moved 200 px right by its transform: its untransformed place is not a hit, its transformed box is. */
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=t style=\"width:50px;height:50px;transform:translate(200px,0)\"></div>"
          "<div style=\"height:1000px\"></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "BODY,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 210, 10), "t,BODY,HTML");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    EMB_CHECK_EQ_STR(consistent_at(v, 210, 10), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_outside_viewport_and_null)
{
    SETUP(k_overlap);
    EMB_CHECK_EQ_STR(list_at(v, -1, 10), "-");
    EMB_CHECK_EQ_STR(list_at(v, 10, -1), "-");
    EMB_CHECK_EQ_STR(list_at(v, 301, 10), "-");
    EMB_CHECK_EQ_STR(list_at(v, 10, 201), "-");
    EMB_CHECK_EQ_STR(id_at(v, -1, 10), "null");
    EMB_CHECK_EQ_STR(id_at(v, 10, 201), "null");
    EMB_CHECK_EQ_STR(consistent_at(v, -1, 10), "ok");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 201), "ok");
    rig_close(v, g, rt);
}

EMB_TEST(elements_from_point_first_entry_is_element_from_point)
{
    /* the k_page points (paint order, pointer-events, the scrolled page) and the overlap page */
    SETUP(k_page);
    static const float pts[][2] = {{10, 10}, {75, 10}, {10, 60}, {80, 60}, {10, 110}, {250, 10}, {10, 150}, {0, 0},
                                   {99.5f, 49.5f}};
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        EMB_CHECK_EQ_STR(consistent_at(v, pts[i][0], pts[i][1]), "ok");
    }
    EMB_CHECK_EQ_STR(list_at(v, 10, 60), "cover,b,BODY,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 10, 110), "c,BODY,HTML");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "a1,a,BODY,HTML");
    rig_close(v, g, rt);
}

/* A preserve-3d parent with a child: the child's hit is in the parent's plane, walk order (child first). */
EMB_TEST(elements_from_point_preserve3d_parent_child)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=card style=\"transform-style:preserve-3d;transform:rotateY(10deg)\">"
          "<div id=kid style=\"height:50px\"></div></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "kid,card,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "kid");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

/* Overlapping children of one preserve-3d plane: their walk order (the later one first) survives the plane's sort. */
EMB_TEST(elements_from_point_siblings_in_one_3d_plane)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=card style=\"position:relative;height:50px;transform-style:preserve-3d;transform:rotateY(10deg)\">"
          "<div id=p1 style=\"position:absolute;left:0;top:0;width:100px;height:50px\"></div>"
          "<div id=q1 style=\"position:absolute;left:0;top:0;width:60px;height:50px\"></div></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "q1,p1,card,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "q1");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

/* Two overlapping planes at different depths (translateZ +10 and -10) in a rotated preserve-3d parent: the nearer plane
 * first, the parent's own box between them, the farther plane last. */
EMB_TEST(elements_from_point_overlapping_3d_planes_by_depth)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=par style=\"position:relative;width:100px;height:50px;transform-style:preserve-3d;"
          "transform:rotateY(10deg)\">"
          "<div id=a style=\"position:absolute;left:0;top:0;width:100px;height:50px;transform:translateZ(10px)\"></div>"
          "<div id=b style=\"position:absolute;left:0;top:0;width:100px;height:50px;transform:translateZ(-10px)\"></div>"
          "</div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "a,par,b,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "a");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

/* Nested preserve-3d: a plane (inner, nearer than its parent) holding a child; the inner's child, the inner, then the
 * outer parent. */
EMB_TEST(elements_from_point_nested_preserve3d)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=outer style=\"transform-style:preserve-3d;transform:rotateY(10deg);height:50px\">"
          "<div id=inner style=\"transform-style:preserve-3d;transform:translateZ(20px)\">"
          "<div id=kid style=\"height:50px\"></div></div></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "kid,inner,outer,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "kid");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

/* A float over inline content: the inline text paints above the float, which paints above the block that holds both. */
EMB_TEST(elements_from_point_float_under_inline)
{
    SETUP("<!doctype html><body style=\"margin:0;font:16px/20px sans-serif\">"
          "<div id=wrap style=\"width:200px\">"
          "<div id=fl style=\"float:left;width:100px;height:20px\"></div>"
          "<span id=sp style=\"margin-left:-50px\">text</span></div>");
    EMB_CHECK_EQ_STR(list_at(v, 55, 5), "sp,fl,wrap,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 55, 5), "sp");
    EMB_CHECK_EQ_STR(consistent_at(v, 55, 5), "ok");
    rig_close(v, g, rt);
}

/* An atomic inline-block over a block: the inline-block (inline phase of its line) is above the block; the line div is
 * painted after the block, so it is above it too. */
EMB_TEST(elements_from_point_inline_block_over_block)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=blk style=\"height:40px\"></div>"
          "<div id=line style=\"margin-top:-40px\">"
          "<span id=ib style=\"display:inline-block;vertical-align:top;width:50px;height:30px\"></span></div>");
    EMB_CHECK_EQ_STR(list_at(v, 10, 10), "ib,line,blk,BODY,HTML");
    EMB_CHECK_EQ_STR(id_at(v, 10, 10), "ib");
    EMB_CHECK_EQ_STR(consistent_at(v, 10, 10), "ok");
    rig_close(v, g, rt);
}

RIG_MAIN()
