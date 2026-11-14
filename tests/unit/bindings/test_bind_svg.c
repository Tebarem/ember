/*
 * The SVG DOM (R4d) seen from JS: the interface of each SVG element (createElementNS, the parser and innerHTML), the
 * element members (className, ownerSVGElement, tabIndex, style, dataset, focus), the animated values (views of the
 * attribute), SVGLength, SVGNumber, DOMPoint and DOMMatrix, the geometry methods over the hook in svg_geometry.h, the
 * global SVGElement that Vue checks, and the memory baseline of SVG wrappers.
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "bindings/svg_geometry.h"
#include "css/css.h"
#include "css/props.h"
#include "js/js.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
} env;

static env env_new(void)
{
    env e = {0};
    e.rt = embi_js_runtime_new();
    e.doc = embi_document_new(true);
    embi_element *html = nullptr;
    embi_element *body = nullptr;
    (void)embi_dom_create_element(e.doc, EMBI_STR("html"), &html);
    (void)embi_append_child(&e.doc->base, &html->base);
    (void)embi_dom_create_element(e.doc, EMBI_STR("body"), &body);
    (void)embi_append_child(&html->base, &body->base);
    e.c = embi_bind_new(e.rt, e.doc);
    return e;
}

static void env_free(env e)
{
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_document_free(e.doc);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e.c), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = v == embi_js_from_bool(true);
    if (!ok) {
        SDL_Log("    js not true: %s", src);
    }
    return ok;
}

static void run_and_free(const char *src)
{
    env e = env_new();
    embi_js_value v = embi_js_eval(embi_bind_realm(e.c), src, strlen(src), "t.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
    }
    env_free(e);
}

static void check_returns_to_baseline(const char *src)
{
    run_and_free(src); /* warm-up: interns the atoms and sizes the runtime's tables */
    size_t base = embi_mem_live();
    run_and_free(src);
    run_and_free(src);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

/* The geometry hook of the tests: it records the last arguments of each entry. */
static struct {
    void *user;
    const embi_node *el;
    bool screen;
    double dist, px, py;
    int calls;
} g_rec;

static bool stub_bbox(void *user, const embi_node *el, double out[4])
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.calls++;
    out[0] = 1;
    out[1] = 2;
    out[2] = 3;
    out[3] = 4;
    return true;
}

static bool stub_ctm(void *user, const embi_node *el, bool screen, double out[6])
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.screen = screen;
    g_rec.calls++;
    double m[6] = {2, 0, 0, 3, screen ? 20 : 10, 30};
    memcpy(out, m, sizeof m);
    return true;
}

static bool stub_len(void *user, const embi_node *el, double *out)
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.calls++;
    *out = 42;
    return true;
}

static bool stub_pal(void *user, const embi_node *el, double d, double out[2])
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.dist = d;
    g_rec.calls++;
    out[0] = d + 1;
    out[1] = d + 2;
    return true;
}

static bool stub_fill(void *user, const embi_node *el, double x, double y)
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.px = x;
    g_rec.py = y;
    g_rec.calls++;
    return x == 5 && y == 6;
}

static bool stub_stroke(void *user, const embi_node *el, double x, double y)
{
    g_rec.user = user;
    g_rec.el = el;
    g_rec.px = x;
    g_rec.py = y;
    g_rec.calls++;
    return x == 7 && y == 8;
}

static const embi_svg_geometry_ops stub_ops = {.bbox = stub_bbox,
                                               .ctm = stub_ctm,
                                               .total_length = stub_len,
                                               .point_at_length = stub_pal,
                                               .point_in_fill = stub_fill,
                                               .point_in_stroke = stub_stroke};

#define PRE "var ns = 'http://www.w3.org/2000/svg'; function mk(t) { return document.createElementNS(ns, t); }"

EMB_TEST(svg_create_element_ns_every_tag)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE " true"));
    static const char *const map[][2] = {
        {"svg", "SVGSVGElement"}, {"g", "SVGGElement"}, {"defs", "SVGDefsElement"}, {"desc", "SVGDescElement"},
        {"title", "SVGTitleElement"}, {"metadata", "SVGMetadataElement"}, {"symbol", "SVGSymbolElement"},
        {"use", "SVGUseElement"}, {"switch", "SVGSwitchElement"}, {"image", "SVGImageElement"},
        {"foreignObject", "SVGForeignObjectElement"}, {"a", "SVGAElement"}, {"view", "SVGViewElement"},
        {"path", "SVGPathElement"}, {"rect", "SVGRectElement"}, {"circle", "SVGCircleElement"},
        {"ellipse", "SVGEllipseElement"}, {"line", "SVGLineElement"}, {"polyline", "SVGPolylineElement"},
        {"polygon", "SVGPolygonElement"}, {"text", "SVGTextElement"}, {"tspan", "SVGTSpanElement"},
        {"textPath", "SVGTextPathElement"}, {"marker", "SVGMarkerElement"}, {"linearGradient", "SVGLinearGradientElement"},
        {"radialGradient", "SVGRadialGradientElement"}, {"stop", "SVGStopElement"}, {"pattern", "SVGPatternElement"},
        {"clipPath", "SVGClipPathElement"}, {"mask", "SVGMaskElement"}, {"style", "SVGStyleElement"},
        {"script", "SVGScriptElement"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        char src[512];
        SDL_snprintf(src, sizeof src,
                     "var o = mk('%s'); o instanceof %s && o instanceof SVGElement && o.localName === '%s' && "
                     "o.namespaceURI === ns && o.constructor === %s",
                     map[i][0], map[i][1], map[i][0], map[i][1]);
        EMB_CHECK(js_true(e, src));
        if (SDL_strcmp(map[i][0], "svg") != 0 && SDL_strcmp(map[i][0], "script") != 0 && SDL_strcmp(map[i][0], "style") != 0) {
            /* innerHTML builds the same interface through the parser */
            SDL_snprintf(src, sizeof src,
                         "document.body.innerHTML = '<svg><%s></%s></svg>'; var q = document.body.firstChild.firstChild; "
                         "q instanceof %s",
                         map[i][0], map[i][0], map[i][1]);
            EMB_CHECK(js_true(e, src));
        }
    }
    /* unknown names are plain SVGElements; the HTML a stays an anchor */
    EMB_CHECK(js_true(e, "mk('nosuch').constructor === SVGElement && mk('nosuch') instanceof SVGElement"));
    EMB_CHECK(js_true(e, "mk('a') instanceof SVGAElement && !(mk('a') instanceof HTMLElement) && "
                         "document.createElement('a') instanceof HTMLAnchorElement && "
                         "!(document.createElement('a') instanceof SVGElement)"));
    /* the hierarchy */
    EMB_CHECK(js_true(e, "mk('rect') instanceof SVGGeometryElement && mk('rect') instanceof SVGGraphicsElement && "
                         "mk('tspan') instanceof SVGTextPositioningElement && mk('text') instanceof SVGTextContentElement && "
                         "mk('linearGradient') instanceof SVGGradientElement && mk('svg') instanceof SVGGraphicsElement"));
    /* the parser and innerHTML */
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<svg viewBox=\"0 0 10 10\"><circle r=\"2\"/><linearGradient id=\"g\"/></svg>';"
                         "var s = document.body.firstChild; s instanceof SVGSVGElement && s.firstChild instanceof SVGCircleElement && "
                         "s.lastChild.localName === 'linearGradient' && s.viewBox.baseVal.width === 10"));
    EMB_CHECK(js_true(e, "var host = document.createElement('div'); document.body.appendChild(host);"
                         "host.innerHTML = '<svg><g><rect width=\"3\"/></g></svg>'; "
                         "host.firstChild.firstChild.firstChild instanceof SVGRectElement && "
                         "host.firstChild.firstChild.firstChild.width.baseVal.value === 3"));
    EMB_CHECK(js_true(e, "var sv0 = mk('svg'); sv0.innerHTML = '<circle r=\"4\"/>'; sv0.firstChild instanceof SVGCircleElement && "
                         "sv0.firstChild.r.baseVal.value === 4 && sv0.innerHTML.indexOf('circle') >= 0"));
    /* Vue checks a global SVGElement */
    EMB_CHECK(js_true(e, "typeof SVGElement === 'function' && mk('g') instanceof SVGElement && "
                         "Object.getPrototypeOf(SVGElement.prototype) === Element.prototype && "
                         "typeof SVGElement.prototype.focus === 'function'"));
    env_free(e);
}

EMB_TEST(svg_element_members)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var r = mk('rect'); r.setAttribute('class', 'a b');"
                         "r.className.baseVal === 'a b' && r.className.animVal === 'a b' && r.className instanceof SVGAnimatedString"));
    EMB_CHECK(js_true(e, "r.className.baseVal = 'x'; r.getAttribute('class') === 'x' && r.classList.contains('x')"));
    EMB_CHECK(js_true(e, "r.tabIndex === -1 && (r.tabIndex = 0, r.getAttribute('tabindex') === '0') && r.tabIndex === 0"));
    EMB_CHECK(js_true(e, "r.style.color = 'red'; r.getAttribute('style').indexOf('color') >= 0"));
    EMB_CHECK(js_true(e, "r.dataset.k = 'v'; r.getAttribute('data-k') === 'v'"));
    EMB_CHECK(js_true(e, "var g = mk('g'), s = mk('svg'); s.appendChild(g); g.ownerSVGElement === s && "
                         "s.ownerSVGElement === null && g.viewportElement === s && mk('rect').ownerSVGElement === null"));
    EMB_CHECK(js_true(e, "var fo = mk('foreignObject'), inner = mk('svg'); fo.appendChild(inner); s.appendChild(fo); "
                         "inner.ownerSVGElement === null"));
    EMB_CHECK(js_true(e, "typeof r.focus === 'function' && typeof r.blur === 'function'"));
    env_free(e);
}

EMB_TEST(svg_animated_values_round_trip)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var x = mk('rect'); x.setAttribute('x', '5px'); x.setAttribute('width', '10');"
                         "x.x instanceof SVGAnimatedLength && x.x.baseVal.value === 5 && x.x.baseVal.unitType === 5 && "
                         "x.width.baseVal.unitType === 1 && x.width.baseVal.valueAsString === '10'"));
    /* a live view: an attribute change shows, a held baseVal writes through */
    EMB_CHECK(js_true(e, "x.setAttribute('x', '7'); x.x.baseVal.value === 7 && x.x.animVal.value === 7"));
    EMB_CHECK(js_true(e, "var held = x.x.baseVal; held.value = 3; held.value === 3 && x.getAttribute('x') === '3'"));
    EMB_CHECK(js_true(e, "x.setAttribute('x', '9'); held.value === 9 && x.x.baseVal === held && x.x === x.x"));
    EMB_CHECK(js_true(e, "held.valueAsString = '2.5cm'; x.getAttribute('x') === '2.5cm' && held.unitType === 6"));
    EMB_CHECK(js_true(e, "x.rx.baseVal.value === 0 && x.rx.baseVal.unitType === 1"));
    /* animVal is distinct and read only */
    EMB_CHECK(js_true(e, "x.x.animVal !== x.x.baseVal"));
    EMB_CHECK(js_true(e, "try { x.x.animVal.value = 1; false } catch (er) { er.name === 'NoModificationAllowedError' }"));
    /* number, string, enumeration, rect, preserveAspectRatio, angle */
    EMB_CHECK(js_true(e, "var p = mk('path'); p.pathLength instanceof SVGAnimatedNumber && p.pathLength.baseVal === 0 && "
                         "(p.pathLength.baseVal = 12.5, p.getAttribute('pathLength') === '12.5') && p.pathLength.animVal === 12.5"));
    EMB_CHECK(js_true(e, "var st = mk('stop'); st.setAttribute('offset', '50%'); st.offset.baseVal === 0.5 && "
                         "(st.offset.baseVal = 0.25, st.getAttribute('offset') === '0.25')"));
    EMB_CHECK(js_true(e, "var lk = mk('use'); lk.setAttribute('href', '#a'); lk.href instanceof SVGAnimatedString && "
                         "lk.href.baseVal === '#a' && lk.href.animVal === '#a' && "
                         "(lk.href.baseVal = '#b', lk.getAttribute('href') === '#b')"));
    EMB_CHECK(js_true(e, "var v = mk('svg'); v.setAttribute('viewBox', '0 0 100 50'); v.viewBox instanceof SVGAnimatedRect && "
                         "v.viewBox.baseVal.width === 100 && v.viewBox.baseVal.height === 50 && v.viewBox.animVal.x === 0 && "
                         "(v.viewBox.baseVal.width = 200, v.getAttribute('viewBox') === '0 0 200 50')"));
    EMB_CHECK(js_true(e, "var m = mk('marker'); m.markerUnits instanceof SVGAnimatedEnumeration && m.markerUnits.baseVal === 2 && "
                         "(m.markerUnits.baseVal = 1, m.getAttribute('markerUnits') === 'userSpaceOnUse') && "
                         "(m.setAttribute('markerUnits', 'bogus'), m.markerUnits.baseVal === 2)"));
    EMB_CHECK(js_true(e, "try { m.markerUnits.baseVal = 7; false } catch (er) { er instanceof TypeError }"));
    EMB_CHECK(js_true(e, "var g = mk('linearGradient'); g.spreadMethod.baseVal === 1 && "
                         "(g.setAttribute('spreadMethod', 'reflect'), g.spreadMethod.baseVal === 2) && g.gradientUnits.baseVal === 2"));
    EMB_CHECK(js_true(e, "v.preserveAspectRatio instanceof SVGAnimatedPreserveAspectRatio && "
                         "v.preserveAspectRatio.baseVal.align === 6 && v.preserveAspectRatio.baseVal.meetOrSlice === 1"));
    EMB_CHECK(js_true(e, "v.setAttribute('preserveAspectRatio', 'xMinYMax slice'); v.preserveAspectRatio.baseVal.align === 8 && "
                         "v.preserveAspectRatio.baseVal.meetOrSlice === 2 && (v.preserveAspectRatio.baseVal.align = 1, "
                         "v.getAttribute('preserveAspectRatio') === 'none slice')"));
    EMB_CHECK(js_true(e, "m.setAttribute('orient', 'auto'); m.orientType.baseVal === 1 && m.orientAngle.baseVal.value === 0 && "
                         "(m.setAttribute('orient', '45'), m.orientType.baseVal === 2 && m.orientAngle.baseVal.value === 45) && "
                         "(m.setOrientToAuto(), m.getAttribute('orient') === 'auto') && "
                         "(m.setOrientToAngle(m.orientAngle.baseVal), m.getAttribute('orient') === '0') && "
                         "SVGMarkerElement.SVG_MARKER_ORIENT_AUTO_START_REVERSE === 3"));
    EMB_CHECK(js_true(e, "var tp = mk('textPath'); tp.side.baseVal === 1 && "
                         "(tp.setAttribute('side', 'right'), tp.side.baseVal === 2) && tp.method.baseVal === 1 && tp.spacing.baseVal === 2"));
    EMB_CHECK(js_true(e, "var ta = mk('text'); ta.setAttribute('dx', '1 2 3'); ta.dx instanceof SVGAnimatedLengthList && "
                         "ta.dx.baseVal.numberOfItems === 3 && ta.dx.animVal.numberOfItems === 3 && ta.dx.animVal !== ta.dx.baseVal"));
    env_free(e);
}

EMB_TEST(svg_length_units_and_errors)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "SVGLength.SVG_LENGTHTYPE_PX === 5 && SVGLength.SVG_LENGTHTYPE_PC === 10 && "
                         "SVGLength.SVG_LENGTHTYPE_UNKNOWN === 0 && SVGLength.SVG_LENGTHTYPE_NUMBER === 1"));
    EMB_CHECK(js_true(e, PRE "var sv = mk('svg'); var L = sv.createSVGLength(); L.unitType === 1 && L.valueAsString === '0'"));
    EMB_CHECK(js_true(e, "L.newValueSpecifiedUnits(SVGLength.SVG_LENGTHTYPE_PX, 12); L.unitType === 5 && L.valueAsString === '12px'"));
    EMB_CHECK(js_true(e, "L.valueAsString = '1in'; L.value === 96 && L.valueInSpecifiedUnits === 1"));
    EMB_CHECK(js_true(e, "L.valueAsString = '2.54cm'; Math.abs(L.value - 96) < 1e-9"));
    EMB_CHECK(js_true(e, "L.valueAsString = '72pt'; Math.abs(L.value - 96) < 1e-9"));
    EMB_CHECK(js_true(e, "L.valueAsString = '6pc'; Math.abs(L.value - 96) < 1e-9"));
    EMB_CHECK(js_true(e, "L.valueAsString = '25.4mm'; Math.abs(L.value - 96) < 1e-9"));
    EMB_CHECK(js_true(e, "L.valueAsString = '1in'; L.convertToSpecifiedUnits(SVGLength.SVG_LENGTHTYPE_PT); "
                         "L.unitType === 9 && Math.abs(L.valueInSpecifiedUnits - 72) < 1e-9 && L.valueAsString === '72pt'"));
    EMB_CHECK(js_true(e, "L.value = 192; L.unitType === 9 && Math.abs(L.valueInSpecifiedUnits - 144) < 1e-9"));
    /* error paths */
    EMB_CHECK(js_true(e, "try { L.valueAsString = 'abc'; false } catch (er) { er.name === 'SyntaxError' }"));
    EMB_CHECK(js_true(e, "try { L.newValueSpecifiedUnits(99, 1); false } catch (er) { er.name === 'NotSupportedError' }"));
    EMB_CHECK(js_true(e, "try { L.convertToSpecifiedUnits(0); false } catch (er) { er.name === 'NotSupportedError' }"));
    EMB_CHECK(js_true(e, "try { L.value = Infinity; false } catch (er) { er instanceof TypeError }"));
    EMB_CHECK(js_true(e, "try { L.value = NaN; false } catch (er) { er instanceof TypeError }"));
    EMB_CHECK(js_true(e, "var nb = sv.createSVGNumber(); nb.value = 4; nb.value === 4 && "
                         "(function () { try { nb.value = 1e40; return false } catch (er) { return er instanceof TypeError } })()"));
    EMB_CHECK(js_true(e, "var an = sv.createSVGAngle(); an.unitType === 1 && an.value === 0 && "
                         "(an.valueAsString = '90deg', an.unitType === 2 && an.value === 90) && "
                         "(an.convertToSpecifiedUnits(SVGAngle.SVG_ANGLETYPE_RAD), Math.abs(an.valueInSpecifiedUnits - Math.PI / 2) < 1e-9) && "
                         "(an.valueAsString = '1turn', an.value === 360 && an.unitType === 0)"));
    EMB_CHECK(js_true(e, "try { an.valueAsString = '1 deg'; false } catch (er) { er.name === 'SyntaxError' }"));
    /* a percentage resolves against the viewport of the context */
    EMB_CHECK(js_true(e, "var vs = mk('svg'); vs.setAttribute('width', '200'); vs.setAttribute('height', '100'); "
                         "vs.setAttribute('viewBox', '0 0 200 100'); var rr = mk('rect'); vs.appendChild(rr); document.body.appendChild(vs); "
                         "rr.setAttribute('width', '50%'); rr.width.baseVal.unitType === 2 && rr.width.baseVal.value === 100 && "
                         "(rr.height.baseVal.valueAsString = '50%', rr.height.baseVal.value === 50)"));
    EMB_CHECK(js_true(e, "var d = mk('svg').createSVGLength(); d.valueAsString = '50%'; d.value === 50"));
    EMB_CHECK(js_true(e, "try { rr.width.animVal.valueAsString = '3'; false } catch (er) { er.name === 'NoModificationAllowedError' }"));
    env_free(e);
}

EMB_TEST(svg_lists)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var ta = mk('text'); ta.setAttribute('x', '1 2 3'); var l = ta.x.baseVal;"
                         "l instanceof SVGLengthList && l.numberOfItems === 3 && l.length === 3 && l[0].value === 1 && l.getItem(2).value === 3"));
    /* identity: the same item object each time, and a write through it changes the attribute */
    EMB_CHECK(js_true(e, "l.getItem(1) === l.getItem(1) && l[1] === l.getItem(1) && "
                         "(l.getItem(1).value = 20, ta.getAttribute('x') === '1 20 3')"));
    /* a list mutation changes the attribute; the returned item is the one inserted */
    EMB_CHECK(js_true(e, "var it = mk('svg').createSVGLength(); it.valueAsString = '7px'; "
                         "var r = l.appendItem(it); r === it && ta.getAttribute('x') === '1 20 3 7px' && l.numberOfItems === 4"));
    /* an item that is already in a list is copied on insert */
    EMB_CHECK(js_true(e, "var other = mk('text'); other.setAttribute('x', '9'); var o = other.x.baseVal; var held = l.getItem(0);"
                         "var c = o.appendItem(held); c !== held && o.numberOfItems === 2 && l.numberOfItems === 4"));
    /* the removed item becomes free */
    EMB_CHECK(js_true(e, "var rm = l.removeItem(0); rm.value === 1 && l.numberOfItems === 3 && ta.getAttribute('x') === '20 3 7px' && "
                         "(rm.value = 55, ta.getAttribute('x') === '20 3 7px')"));
    EMB_CHECK(js_true(e, "l.insertItemBefore(rm, 0) === rm && ta.getAttribute('x') === '55 20 3 7px'"));
    EMB_CHECK(js_true(e, "var rp = mk('svg').createSVGLength(); rp.value = 1; l.replaceItem(rp, 1) === rp && "
                         "ta.getAttribute('x') === '55 1 3 7px'"));
    EMB_CHECK(js_true(e, "var init = mk('svg').createSVGLength(); init.value = 4; l.initialize(init) === init && "
                         "l.numberOfItems === 1 && ta.getAttribute('x') === '4'"));
    EMB_CHECK(js_true(e, "l.clear(); l.numberOfItems === 0 && ta.getAttribute('x') === ''"));
    /* sync: an attribute change shows in the list, an invalid attribute clears it */
    EMB_CHECK(js_true(e, "ta.setAttribute('x', '5 6'); l.numberOfItems === 2 && l.getItem(1).value === 6 && "
                         "(ta.setAttribute('x', '5 bad'), l.numberOfItems === 0)"));
    /* error paths */
    EMB_CHECK(js_true(e, "try { l.getItem(0); false } catch (er) { er.name === 'IndexSizeError' }"));
    EMB_CHECK(js_true(e, "try { l.removeItem(3); false } catch (er) { er.name === 'IndexSizeError' }"));
    EMB_CHECK(js_true(e, "try { l.appendItem({}); false } catch (er) { er instanceof TypeError }"));
    EMB_CHECK(js_true(e, "try { ta.x.animVal.clear(); false } catch (er) { er.name === 'NoModificationAllowedError' }"));
    /* points, numbers, transforms, strings */
    EMB_CHECK(js_true(e, "var pl = mk('polyline'); pl.setAttribute('points', '1,2 3,4 5'); pl.points instanceof SVGPointList && "
                         "pl.points.numberOfItems === 2 && pl.points.getItem(1).y === 4 && pl.animatedPoints.numberOfItems === 2 && "
                         "pl.animatedPoints !== pl.points"));
    EMB_CHECK(js_true(e, "var np = mk('svg').createSVGPoint(); np.x = 9; np.y = 8; pl.points.appendItem(np); "
                         "pl.getAttribute('points') === '1,2 3,4 9,8'"));
    EMB_CHECK(js_true(e, "var tx = mk('text'); tx.setAttribute('rotate', '10 20'); tx.rotate.baseVal instanceof SVGNumberList && "
                         "tx.rotate.baseVal.getItem(1).value === 20"));
    EMB_CHECK(js_true(e, "var rc = mk('rect'); rc.setAttribute('transform', 'translate(10,20) scale(2)'); var tl = rc.transform.baseVal; "
                         "tl instanceof SVGTransformList && tl.numberOfItems === 2 && tl.getItem(0).type === 2 && "
                         "tl.getItem(0).matrix.e === 10 && tl.getItem(0).matrix.f === 20 && tl.getItem(1).type === 3 && "
                         "tl.getItem(1).matrix.a === 2"));
    EMB_CHECK(js_true(e, "tl.getItem(0).setTranslate(1, 2); var ts = rc.getAttribute('transform'); ts.indexOf('translate(1 2)') === 0 && ts.indexOf('scale(2') > 0"));
    EMB_CHECK(js_true(e, "var cons = tl.consolidate(); tl.numberOfItems === 1 && cons.type === 1 && cons.matrix.a === 2 && cons.matrix.e === 1"));
    EMB_CHECK(js_true(e, "var tt = tl.createSVGTransformFromMatrix(mk('svg').createSVGMatrix().translate(5, 6)); tt.type === 1 && tt.matrix.e === 5"));
    EMB_CHECK(js_true(e, "rc.setAttribute('transform', 'rotate(90 10 10)'); var rt = rc.transform.baseVal.getItem(0); "
                         "rt.type === 4 && rt.angle === 90"));
    EMB_CHECK(js_true(e, "rc.setAttribute('transform', 'bogus'); rc.transform.baseVal.numberOfItems === 0"));
    EMB_CHECK(js_true(e, "var sl = mk('rect'); sl.setAttribute('systemLanguage', 'en, fr'); var ssl = sl.systemLanguage; "
                         "ssl instanceof SVGStringList && ssl.numberOfItems === 2 && ssl.getItem(1) === 'fr' && "
                         "ssl.appendItem('de') === 'de' && ssl.numberOfItems === 3"));
    EMB_CHECK(js_true(e, "var ptl = mk('polygon'); ptl.points.numberOfItems === 0 && ptl.animatedPoints.numberOfItems === 0"));
    env_free(e);
}

EMB_TEST(svg_transform_matrix_point_rect)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var sv = mk('svg'); var m = sv.createSVGMatrix(); "
                         "m.a === 1 && m.d === 1 && m.e === 0"));
    EMB_CHECK(js_true(e, "var m2 = m.translate(5, 6); m2.e === 5 && m2.f === 6 && m.e === 0 && m2.scale(2).a === 2"));
    EMB_CHECK(js_true(e, "var inv = m.scaleNonUniform(2, 4).inverse(); inv.a === 0.5 && inv.d === 0.25"));
    EMB_CHECK(js_true(e, "var r90 = m.rotate(90); Math.abs(r90.a) < 1e-12 && Math.abs(r90.b - 1) < 1e-12"));
    EMB_CHECK(js_true(e, "m.a = 3; m.a === 3 && m.multiply(m).a === 9"));
    EMB_CHECK(js_true(e, "var tf = sv.createSVGTransform(); tf.type === 1 && tf.matrix.a === 1 && "
                         "(tf.setScale(2, 3), tf.type === 3 && tf.matrix.d === 3) && "
                         "(tf.setRotate(45, 1, 2), tf.type === 4 && tf.angle === 45) && (tf.setSkewX(10), tf.type === 5) && "
                         "(tf.setSkewY(10), tf.type === 6) && "
                         "(tf.setMatrix(sv.createSVGMatrix().translate(5, 6)), tf.type === 1 && tf.matrix.f === 6)"));
    EMB_CHECK(js_true(e, "try { tf.setScale(NaN, 1); false } catch (er) { er instanceof TypeError }"));
    EMB_CHECK(js_true(e, "var pt = sv.createSVGPoint(); pt.x = 3; pt.y = 4; "
                         "var tp = pt.matrixTransform(sv.createSVGMatrix().translate(1, 1).scale(2)); tp.x === 7 && tp.y === 9"));
    EMB_CHECK(js_true(e, "var dp = new DOMPoint(1, 2, 3, 4); dp.x === 1 && dp.z === 3 && dp.w === 4 && (dp.x = 10, dp.x === 10) && "
                         "DOMPoint.fromPoint({x: 5}).x === 5 && dp.toJSON().w === 4"));
    EMB_CHECK(js_true(e, "var dm = new DOMMatrix([1, 2, 3, 4, 5, 6]); dm.is2D && dm.m11 === 1 && dm.m12 === 2 && dm.m21 === 3 && "
                         "dm.m41 === 5 && dm.f === 6 && (dm.m11 = 9, dm.a === 9) && (dm.a = 8, dm.m11 === 8) && dm.isIdentity === false"));
    EMB_CHECK(js_true(e, "var dm2 = new DOMMatrix(); dm2.translateSelf(3, 4).scaleSelf(2); dm2.e === 3 && dm2.f === 4 && dm2.a === 2 && "
                         "new DOMMatrixReadOnly().isIdentity && new DOMMatrix().isIdentity"));
    EMB_CHECK(js_true(e, "var rc = sv.createSVGRect(); rc.x = 1; rc.width = 5; rc.x === 1 && rc.width === 5 && rc.height === 0"));
    EMB_CHECK(js_true(e, "SVGTransform.SVG_TRANSFORM_ROTATE === 4 && SVGPreserveAspectRatio.SVG_PRESERVEASPECTRATIO_XMINYMIN === 2 && "
                         "SVGPreserveAspectRatio.SVG_MEETORSLICE_SLICE === 2 && SVGAngle.SVG_ANGLETYPE_GRAD === 4 && "
                         "SVGUnitTypes.SVG_UNIT_TYPE_OBJECTBOUNDINGBOX === 2"));
    /* the matrix of a transform item is live */
    EMB_CHECK(js_true(e, "var g = mk('g'); g.setAttribute('transform', 'translate(1,2)'); var item = g.transform.baseVal.getItem(0); "
                         "item.matrix.e = 40; g.getAttribute('transform').indexOf('40') >= 0"));
    env_free(e);
}

EMB_TEST(svg_svg_element_api)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var sv = mk('svg'); sv.setAttribute('id', 'root'); var gg = mk('g'); gg.setAttribute('id', 'target'); "
                         "var inner = mk('rect'); inner.setAttribute('id', 'deep'); gg.appendChild(inner); sv.appendChild(gg);"
                         "sv.getElementById('target') === gg && sv.getElementById('deep') === inner && sv.getElementById('nope') === null"));
    EMB_CHECK(js_true(e, "sv.zoomAndPan === 2 && (sv.zoomAndPan = 1, sv.zoomAndPan === 1) && "
                         "SVGSVGElement.SVG_ZOOMANDPAN_DISABLE === 1 && SVGSVGElement.SVG_ZOOMANDPAN_MAGNIFY === 2"));
    EMB_CHECK(js_true(e, "sv.currentScale === 1 && sv.currentTranslate instanceof DOMPoint && sv.currentTranslate.x === 0"));
    /* currentTranslate is [SameObject] and writable; currentScale keeps its value */
    EMB_CHECK(js_true(e, "sv.currentTranslate === sv.currentTranslate && (sv.currentTranslate.x = 5, sv.currentTranslate.x === 5) && "
                         "(sv.currentScale = 2, sv.currentScale === 2)"));
    EMB_CHECK(js_true(e, "var gc = []; for (var gi = 0; gi < 4000; gi++) gc.push(mk('g')); gc.length = 0; "
                         "sv.currentTranslate.x === 5 && sv.currentScale === 2 && document.body.appendChild(sv) === sv && "
                         "sv.currentTranslate.x === 5"));
    EMB_CHECK(js_true(e, "typeof sv.createSVGLength === 'function' && typeof sv.createSVGNumber === 'function' && "
                         "typeof sv.createSVGAngle === 'function' && typeof sv.createSVGPoint === 'function' && "
                         "typeof sv.createSVGMatrix === 'function' && typeof sv.createSVGRect === 'function' && "
                         "typeof sv.createSVGTransform === 'function' && typeof sv.createSVGTransformFromMatrix === 'function'"));
    EMB_CHECK(js_true(e, "sv.x.baseVal.value === 0 && sv.height.baseVal.unitType === 2"));
    env_free(e);
}

EMB_TEST(svg_geometry_defaults)
{
    env e = env_new();
    /* Without a render tree (the hook is not installed) the methods answer the SVG 2 defaults. */
    EMB_CHECK(js_true(e, PRE "var r = mk('rect');"
                         "r.getBBox().width === 0 && r.getBBox() instanceof DOMRect && r.getCTM().a === 1 && "
                         "r.getScreenCTM().d === 1 && r.getTotalLength() === 0 && r.getPointAtLength(1).x === 0 && "
                         "r.isPointInFill({x: 0, y: 0}) === false && r.isPointInStroke({x: 0, y: 0}) === false"));
    env_free(e);
}

EMB_TEST(svg_geometry_hook)
{
    env e = env_new();
    memset(&g_rec, 0, sizeof g_rec);
    int token = 0;
    embi_svg_geometry_install(e.c, &stub_ops, &token);
    EMB_CHECK(embi_svg_geometry_current(e.c) == &stub_ops);
    EMB_CHECK(js_true(e, PRE "var r = mk('rect'); var b = r.getBBox(); b.x === 1 && b.y === 2 && b.width === 3 && b.height === 4"));
    EMB_CHECK(g_rec.calls == 1 && g_rec.el != nullptr && g_rec.el->type == EMBI_NODE_ELEMENT && g_rec.el->extra == EMBI_NS_SVG);
    EMB_CHECK(js_true(e, "var c = r.getCTM(); c.a === 2 && c.d === 3 && c.e === 10 && c.f === 30"));
    EMB_CHECK(!g_rec.screen);
    EMB_CHECK(js_true(e, "var sc = r.getScreenCTM(); sc.e === 20 && sc.f === 30"));
    EMB_CHECK(g_rec.screen);
    EMB_CHECK(js_true(e, "r.getTotalLength() === 42"));
    EMB_CHECK(g_rec.user == &token); /* the callbacks get the pointer given at install time */
    EMB_CHECK(js_true(e, "var pp = r.getPointAtLength(5); pp.x === 6 && pp.y === 7 && pp instanceof DOMPoint"));
    EMB_CHECK(g_rec.dist == 5);
    EMB_CHECK(js_true(e, "r.isPointInFill({x: 5, y: 6}) === true && r.isPointInFill({x: 1, y: 1}) === false"));
    EMB_CHECK(g_rec.px == 1 && g_rec.py == 1);
    EMB_CHECK(js_true(e, "r.isPointInStroke({x: 7, y: 8}) === true && r.isPointInStroke(new DOMPoint(0, 0)) === false"));
    /* the table belongs to the context: another context in the process does not see it */
    env other = env_new();
    EMB_CHECK(embi_svg_geometry_current(other.c) == nullptr);
    int calls = g_rec.calls;
    EMB_CHECK(js_true(other, PRE "mk('rect').getBBox().width === 0 && mk('rect').getTotalLength() === 0 && "
                             "mk('rect').getCTM().a === 1"));
    EMB_CHECK(g_rec.calls == calls);
    env_free(other);
    embi_svg_geometry_install(e.c, nullptr, nullptr);
    EMB_CHECK(embi_svg_geometry_current(e.c) == nullptr);
    EMB_CHECK(js_true(e, "mk('rect').getBBox().width === 0 && mk('rect').getTotalLength() === 0"));
    env_free(e);
}

/* The lists have an indexed property setter: list[i] = item is replaceItem(item, i). */
EMB_TEST(svg_list_indexed_setter)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var t = mk('text'); t.setAttribute('x', '1 2 3'); var l = t.x.baseVal; var nl = mk('svg').createSVGLength(); nl.value = 9; "
                         "l[1] = nl; t.getAttribute('x') === '1 9 3' && l[1] === nl && l.numberOfItems === 3 && Object.getOwnPropertyDescriptor(l, '0').writable"));
    EMB_CHECK(js_true(e, "function ty(f, c) { try { f(); return false } catch (er) { return typeof c === 'string' ? er.name === c : er instanceof c } } "
                         "ty(function () { l[0] = 30 }, TypeError) && ty(function () { l[0] = 'aString' }, TypeError) && "
                         "ty(function () { l[0] = null }, TypeError) && ty(function () { l[0] = t }, TypeError) && "
                         "ty(function () { l[7] = nl }, 'IndexSizeError') && t.getAttribute('x') === '1 9 3'"));
    EMB_CHECK(js_true(e, "ty(function () { Object.defineProperty(l, '0', {get: function () { return 1 }}) }, TypeError) && "
                         "ty(function () { t.x.animVal[0] = nl }, 'NoModificationAllowedError')"));
    EMB_CHECK(js_true(e, "var pl = mk('polyline'); pl.setAttribute('points', '1,2 3,4'); pl.points[0] = new DOMPoint(7, 8); "
                         "pl.getAttribute('points') === '7,8 3,4'"));
    EMB_CHECK(js_true(e, "var sl = mk('rect'); sl.setAttribute('systemLanguage', 'en, fr'); sl.systemLanguage[1] = 'de'; "
                         "sl.getAttribute('systemLanguage') === 'en, de'"));
    /* an indexed getter without a setter: a strict-mode write throws, a sloppy one does nothing */
    EMB_CHECK(js_true(e, "var rl = document.body.getClientRects(); (function () { 'use strict'; return ty(function () { rl[0] = 1 }, TypeError) && "
                         "ty(function () { rl.length = 3 }, TypeError) })() && (rl[0] = 1, true)"));
    env_free(e);
}

/* SVGPoint, SVGMatrix and SVGRect are a DOMPoint, a DOMMatrix and a DOMRect (R4.md): one object model for the values
 * and for the geometry of the elements. */
EMB_TEST(svg_value_types_are_dom_types)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var sv = mk('svg'); var m = sv.createSVGMatrix(), p = sv.createSVGPoint(), r = sv.createSVGRect();"
                         "m instanceof SVGMatrix && m instanceof DOMMatrix && m instanceof DOMMatrixReadOnly && "
                         "p instanceof SVGPoint && p instanceof DOMPoint && p instanceof DOMPointReadOnly && "
                         "r instanceof SVGRect && r instanceof DOMRect && r instanceof DOMRectReadOnly"));
    EMB_CHECK(js_true(e, "var vb = mk('svg'); vb.setAttribute('viewBox', '1 2 30 40'); var bb = mk('rect').getBBox(); "
                         "vb.viewBox.baseVal instanceof DOMRect && vb.viewBox.animVal instanceof DOMRectReadOnly && "
                         "bb instanceof DOMRect && vb.viewBox.baseVal.right === 31 && vb.viewBox.baseVal.bottom === 42 && "
                         "JSON.stringify(vb.viewBox.baseVal) === JSON.stringify(new DOMRect(1, 2, 30, 40))"));
    EMB_CHECK(js_true(e, "var tf0 = sv.createSVGTransform(); tf0.matrix instanceof DOMMatrix && tf0.matrix instanceof SVGMatrix && "
                         "tf0.matrix === tf0.matrix"));
    /* a DOMMatrix goes wherever a matrix goes, and the other way round */
    EMB_CHECK(js_true(e, "var tf1 = sv.createSVGTransform(); tf1.setMatrix(new DOMMatrix([2, 0, 0, 3, 4, 5])); "
                         "tf1.type === 1 && tf1.matrix.a === 2 && tf1.matrix.d === 3 && tf1.matrix.e === 4 && tf1.matrix.f === 5"));
    EMB_CHECK(js_true(e, "var tf2 = sv.createSVGTransformFromMatrix(new DOMMatrix().translate(7, 8)); "
                         "tf2.type === 1 && tf2.matrix.e === 7 && tf2.matrix.f === 8"));
    EMB_CHECK(js_true(e, "var tf3 = sv.createSVGTransformFromMatrix(sv.createSVGMatrix().scale(3)); tf3.matrix.a === 3 && "
                         "sv.createSVGTransformFromMatrix().matrix.a === 1"));
    EMB_CHECK(js_true(e, "p.x = 3; p.y = 4; var dmx = new DOMMatrix().translate(1, 1).scale(2); "
                         "var tp0 = p.matrixTransform(dmx), tp1 = new DOMPoint(3, 4).matrixTransform(m.translate(1, 1).scale(2)); "
                         "tp0 instanceof DOMPoint && tp0.x === 7 && tp0.y === 9 && tp1.x === 7 && tp1.y === 9"));
    EMB_CHECK(js_true(e, "dmx.transformPoint(p).x === 7 && m.multiply(dmx).a === 2 && dmx.multiply(m).a === 2"));
    /* the DOMMatrix methods of a matrix: all the arguments of scale() and the legacy scaleNonUniform */
    EMB_CHECK(js_true(e, "var sc = m.scale(2, 3, 1, 10, 20); sc.a === 2 && sc.d === 3 && sc.e === -10 && sc.f === -40 && "
                         "m.scaleNonUniform(2, 4).d === 4 && m.scale(2).d === 2"));
    /* restricted floats: only SVGPoint, SVGMatrix and SVGRect throw, the DOM types take any double */
    EMB_CHECK(js_true(e, "var dp = new DOMPoint(); dp.x = NaN; dp.y = Infinity; var dm = new DOMMatrix(); dm.a = NaN; dm.e = -Infinity; "
                         "var dr = new DOMRect(); dr.width = NaN; Number.isNaN(dp.x) && dp.y === Infinity && Number.isNaN(dm.a) && "
                         "dm.e === -Infinity && Number.isNaN(dr.width)"));
    EMB_CHECK(js_true(e, "var thr = 0; try { p.x = NaN } catch (er) { if (er instanceof TypeError) thr++ } "
                         "try { m.a = Infinity } catch (er) { if (er instanceof TypeError) thr++ } "
                         "try { r.width = NaN } catch (er) { if (er instanceof TypeError) thr++ } thr === 3 && p.x === 3"));
    /* the same classes in the list of points: appendItem takes an SVGPoint and a DOMPoint */
    EMB_CHECK(js_true(e, "var pl = mk('polyline'); pl.setAttribute('points', '1,2 3,4'); var it0 = pl.points.getItem(0); "
                         "it0 instanceof SVGPoint && it0 instanceof DOMPoint && it0.matrixTransform(new DOMMatrix().translate(1, 1)).x === 2 && "
                         "pl.points.appendItem(new DOMPoint(5, 6)) instanceof DOMPoint && pl.getAttribute('points') === '1,2 3,4 5,6'"));
    env_free(e);
}

/* The live values: reads see the attribute, writes go to it, and an animVal refuses every path to a write. */
EMB_TEST(svg_live_values_and_animval)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var g = mk('g'); g.setAttribute('transform', 'translate(1,2) scale(3)'); var tl = g.transform.baseVal; "
                         "var base0 = tl.getItem(0), mx = base0.matrix; mx.e = 40; g.getAttribute('transform').indexOf('40') > 0"));
    EMB_CHECK(js_true(e, "mx.translateSelf(1, 0) === mx && mx.e === 41 && g.getAttribute('transform').indexOf('41') >= 0 && "
                         "base0.matrix === mx && tl.getItem(0) === base0"));
    EMB_CHECK(js_true(e, "g.setAttribute('transform', 'translate(7,8)'); mx.e === 7 && mx.f === 8 && mx.isIdentity === false"));
    /* the animVal: the items, their matrices and every operation on them throw and leave the attribute alone */
    EMB_CHECK(js_true(e, "var av = g.transform.animVal, at = av.getItem(0), before = g.getAttribute('transform'); "
                         "at.matrix instanceof DOMMatrix && at.matrix.e === 7"));
    EMB_CHECK(js_true(e, "function nm(f) { try { f(); return false } catch (er) { return er.name === 'NoModificationAllowedError' } } "
                         "nm(function () { at.matrix.a = 5 }) && nm(function () { at.matrix.e = 5 }) && "
                         "nm(function () { at.matrix.m41 = 5 }) && nm(function () { at.matrix.translateSelf(1, 1) }) && "
                         "nm(function () { at.matrix.multiplySelf({a: 2}) }) && nm(function () { at.matrix.invertSelf() }) && "
                         "nm(function () { at.setTranslate(1, 1) }) && nm(function () { at.setMatrix(new DOMMatrix()) }) && "
                         "nm(function () { av.clear() }) && g.getAttribute('transform') === before && at.matrix.e === 7"));
    EMB_CHECK(js_true(e, "var pl = mk('polygon'); pl.setAttribute('points', '1,2 3,4'); var ap = pl.animatedPoints.getItem(0); "
                         "nm(function () { ap.x = 9 }) && nm(function () { ap.y = 9 }) && nm(function () { pl.animatedPoints.appendItem(new DOMPoint()) }) && "
                         "pl.getAttribute('points') === '1,2 3,4' && ap.x === 1"));
    EMB_CHECK(js_true(e, "var vb = mk('svg'); vb.setAttribute('viewBox', '0 0 5 6'); var av2 = vb.viewBox.animVal; "
                         "nm(function () { av2.width = 1 }) && nm(function () { av2.x = 1 }) && vb.getAttribute('viewBox') === '0 0 5 6' && "
                         "(vb.viewBox.baseVal.width = 9, vb.getAttribute('viewBox') === '0 0 9 6' && av2.width === 9)"));
    /* a rectangle or a point you made yourself is free */
    EMB_CHECK(js_true(e, "var fr = new DOMRect(1, 2, 3, 4); fr.width = 9; fr.right === 10 && vb.createSVGRect().width === 0"));
    env_free(e);
}

/* A long list: parsing grows by doubling and reading the items does not re-compare the attribute for each. */
EMB_TEST(svg_long_list_is_linear)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "var pl = mk('polyline'); var pts = []; for (var i = 0; i < 20000; i++) pts.push(i + ',' + (i + 1)); "
                         "pl.setAttribute('points', pts.join(' ')); var L = pl.points, sum = 0; "
                         "for (var j = 0; j < L.numberOfItems; j++) sum += L.getItem(j).x + L[j].y; "
                         "L.numberOfItems === 20000 && sum === 20000 * 20000"));
    /* after another mutation the list sees the new text */
    EMB_CHECK(js_true(e, "pl.setAttribute('points', '9,9'); L.numberOfItems === 1 && L.getItem(0).x === 9"));
    EMB_CHECK(js_true(e, "L.getItem(0).x = 4; pl.getAttribute('points') === '4,9' && L.getItem(0).x === 4"));
    env_free(e);
}

EMB_TEST(svg_font_relative_units_without_engine)
{
    env e = env_new();
    /* no style engine: 1em is 16px and the getter does not need (or create) one */
    EMB_CHECK(js_true(e, PRE "var r = mk('rect'); document.body.appendChild(r); r.setAttribute('width', '2em'); "
                         "r.width.baseVal.value === 32"));
    env_free(e);
}

/* em and ex read the computed font-size of the element from the style engine of the host. The engine learns a document when
 * the embed layer hands it the sheets, after the parse, but scripts run before; a length read in between attaches the
 * document with no author sheets, which is what the embed layer's own call replaces: its sheets still take effect. */
EMB_TEST(svg_em_reads_the_engine_and_leaves_later_sheets_alone)
{
    embi_text *text = embi_tshim_create();
    embi_style_engine *style = embi_style_engine_create(embi_tshim_fonts(text), nullptr, nullptr);
    env e = env_new();
    embi_bind_host h = {.style = style};
    embi_bind_set_host(e.c, &h);
    EMB_CHECK(js_true(e, PRE "document.body.innerHTML = '<svg><rect id=\"r\" width=\"2em\"/><rect id=\"q\" style=\"font-size:10px\" "
                         "width=\"3em\"/></svg>'; var r = document.getElementById('r'), q = document.getElementById('q'); "
                         "r.width.baseVal.value === 32 && q.width.baseVal.value === 30"));
    EMB_CHECK(embi_style_document_epoch(style, e.doc) != 0); /* the read attached the document */
    /* the embed layer's sheets arrive later and replace what the read set */
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    static const char css[] = "rect { font-size: 30px }";
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){css, (uint32_t)(sizeof css - 1)}, &opts);
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(style, e.doc, sheets, 1);
    EMB_CHECK(js_true(e, "r.width.baseVal.value === 60 && q.width.baseVal.value === 30"));
    embi_document_set_author_sheets(style, e.doc, nullptr, 0);
    EMB_CHECK(js_true(e, "r.width.baseVal.value === 32"));
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_style_forget_document(style, e.doc);
    embi_document_free(e.doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(style);
    embi_tshim_destroy(text);
}

/* A script-made document must never be attached to the style engine: the GC frees it without a forget call. */
EMB_TEST(svg_em_in_script_made_document_does_not_attach)
{
    embi_text *text = embi_tshim_create();
    embi_style_engine *style = embi_style_engine_create(embi_tshim_fonts(text), nullptr, nullptr);
    env e = env_new();
    embi_bind_host h = {.style = style};
    embi_bind_set_host(e.c, &h);
    EMB_CHECK(js_true(e, PRE "var d = document.implementation.createHTMLDocument('x'); "
                         "var r = d.createElementNS('http://www.w3.org/2000/svg', 'rect'); r.setAttribute('width', '2em'); "
                         "d.body.appendChild(r); r.width.baseVal.value === 32"));
    EMB_CHECK(js_true(e, "var p = new DOMParser().parseFromString('<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"2em\"/></svg>', "
                         "'image/svg+xml'); p.documentElement.firstChild.width.baseVal.value === 32"));
    /* free the script world (and so the documents) before the engine goes */
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_style_forget_document(style, e.doc);
    embi_document_free(e.doc);
    embi_style_engine_destroy(style);
    embi_tshim_destroy(text);
}

EMB_TEST(svg_element_focus_with_tabindex)
{
    env e = env_new();
    EMB_CHECK(js_true(e, PRE "document.body.innerHTML = '<svg><rect id=\"r\" tabindex=\"0\"/><rect id=\"n\"/></svg>'; "
                         "var r = document.getElementById('r'), n = document.getElementById('n'); var log = []; "
                         "r.addEventListener('focus', () => log.push('focus')); r.addEventListener('blur', () => log.push('blur')); "
                         "var before = document.activeElement; r.focus(); document.activeElement === r && log.join() === 'focus'"));
    EMB_CHECK(js_true(e, "n.focus(); document.activeElement === r"));
    EMB_CHECK(js_true(e, "r.blur(); document.activeElement === before && log.join() === 'focus,blur'"));
    env_free(e);
}

EMB_TEST(svg_wrappers_return_to_baseline)
{
    check_returns_to_baseline("var ns = 'http://www.w3.org/2000/svg';"
                              "for (let i = 0; i < 8; i++) {"
                              "  var s = document.createElementNS(ns, 'svg'); var r = s.appendChild(document.createElementNS(ns, 'rect'));"
                              "  r.setAttribute('x', '1'); var a = r.x.baseVal.value; var m = new DOMMatrix(); var p = r.getBBox();"
                              "  r.className.baseVal = 'q'; var L = s.createSVGLength(); L.valueAsString = '3px';"
                              "  var pt = new DOMPoint(i, i); r.getPointAtLength(i);"
                              "}");
    check_returns_to_baseline("var ns = 'http://www.w3.org/2000/svg';"
                              "for (let i = 0; i < 6; i++) {"
                              "  var t = document.createElementNS(ns, 'text'); t.setAttribute('x', '1 2 3'); var l = t.x.baseVal;"
                              "  var it = l.getItem(1); l.removeItem(0); l.appendItem(it); var an = t.x.animVal; an.getItem(0);"
                              "  var g = document.createElementNS(ns, 'g'); g.setAttribute('transform', 'rotate(30) scale(2)');"
                              "  var tl = g.transform.baseVal; tl.getItem(0).setTranslate(1, 2); tl.consolidate(); g.transform.animVal.numberOfItems;"
                              "  var pl = document.createElementNS(ns, 'polyline'); pl.setAttribute('points', '1,2 3,4'); pl.points.getItem(0).x = 5;"
                              "  var sv = document.createElementNS(ns, 'svg'); sv.viewBox.baseVal.width; sv.preserveAspectRatio.baseVal.align = 2;"
                              "  var mk = document.createElementNS(ns, 'marker'); mk.markerUnits.baseVal; mk.setOrientToAuto();"
                              "}");
    check_returns_to_baseline("var ns = 'http://www.w3.org/2000/svg';"
                              "for (let i = 0; i < 6; i++) {"
                              "  var t = document.createElementNS(ns, 'text'); t.setAttribute('x', '1 2'); var held = t.x.baseVal.getItem(0);"
                              "  var other = document.createElementNS(ns, 'text'); other.x.baseVal.appendItem(held);"
                              "  document.body.innerHTML = '<svg><rect id=\"r\" width=\"3\"/></svg>'; document.getElementById('r').width.baseVal.value;"
                              "}");
    /* the DOM geometry types in their bound and free forms (rects, matrices, points and the animVal refusals) */
    check_returns_to_baseline("var ns = 'http://www.w3.org/2000/svg';"
                              "for (let i = 0; i < 6; i++) {"
                              "  var s = document.createElementNS(ns, 'svg'); s.setAttribute('viewBox', '0 0 5 6');"
                              "  s.viewBox.baseVal.width = 9; var r = s.createSVGRect(); r.x = 1; var m = s.createSVGMatrix().scale(2);"
                              "  var g = document.createElementNS(ns, 'g'); g.setAttribute('transform', 'translate(1 2)');"
                              "  var mx = g.transform.baseVal.getItem(0).matrix; mx.translateSelf(1, 1); mx.e = 3;"
                              "  try { g.transform.animVal.getItem(0).matrix.a = 1 } catch (e) {}"
                              "  var pl = document.createElementNS(ns, 'polyline'); pl.setAttribute('points', '1,2 3,4');"
                              "  pl.points.appendItem(new DOMPoint(5, 6)); try { pl.animatedPoints.getItem(0).x = 1 } catch (e) {}"
                              "  var t = s.createSVGTransformFromMatrix(new DOMMatrix([1, 2, 3, 4, 5, 6])); t.setMatrix(m);"
                              "  s.currentTranslate.x = i; s.currentScale = 2; new DOMRect(1, 2, 3, 4).toJSON();"
                              "}");
}

EMB_TEST_MAIN()
