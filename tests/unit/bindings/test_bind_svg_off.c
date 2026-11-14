/*
 * EMBER_SVG=OFF (SZ3b): built only in such a build. The SVG interfaces are not bound, elements in the SVG
 * namespace are plain Elements, and the geometry interfaces that are not SVG's (DOMMatrix, DOMPoint, DOMRect) stay.
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "js/js.h"

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

EMB_TEST(svg_interfaces_are_not_bound)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "typeof SVGSVGElement === 'undefined' && typeof SVGGraphicsElement === 'undefined' && typeof SVGPathElement === 'undefined'"));
    EMB_CHECK(js_true(e, "typeof SVGLength === 'undefined' && typeof SVGMatrix === 'undefined' && typeof SVGPoint === 'undefined'"));
    EMB_CHECK(js_true(e, "typeof SVGAnimatedLength === 'undefined' && typeof SVGTransformList === 'undefined'"));
    env_free(e);
}

EMB_TEST(svg_elements_are_plain_elements)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var NS = 'http://www.w3.org/2000/svg'; var s = document.createElementNS(NS, 'svg'); "
                         "s instanceof Element && !(s instanceof HTMLElement) && s.namespaceURI === NS"));
    /* SVGElement itself stays (frameworks test instanceof SVGElement), with style, dataset, tabIndex, focus and blur. */
    EMB_CHECK(js_true(e, "typeof SVGElement === 'function' && s instanceof SVGElement && Object.getPrototypeOf(s) === SVGElement.prototype"));
    EMB_CHECK(js_true(e, "typeof s.style.setProperty === 'function' && typeof s.dataset === 'object' && s.tabIndex === -1 && typeof s.focus === 'function'"));
    EMB_CHECK(js_true(e, "typeof s.getBBox === 'undefined' && typeof s.createSVGPoint === 'undefined' && typeof s.ownerSVGElement === 'undefined'"));
    /* The parser still builds the tree (foreign content), the elements are inert. */
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<svg viewBox=\"0 0 10 10\"><circle r=\"4\"></circle></svg>'; "
                         "var sv = document.body.firstChild; sv.localName === 'svg' && sv.namespaceURI === NS && "
                         "sv.firstChild.localName === 'circle' && sv.getAttribute('viewBox') === '0 0 10 10'"));
    EMB_CHECK(js_true(e, "sv.outerHTML.indexOf('<circle') > 0"));
    env_free(e);
}

EMB_TEST(unrelated_interfaces_stay)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "new DOMMatrix([1, 0, 0, 1, 5, 6]).e === 5 && new DOMPoint(1, 2).y === 2"));
    EMB_CHECK(js_true(e, "typeof DOMRect === 'function' && typeof document.body.getBoundingClientRect().width === 'number'"));
    env_free(e);
}

EMB_TEST_MAIN()
