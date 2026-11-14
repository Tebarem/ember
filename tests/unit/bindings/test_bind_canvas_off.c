/*
 * EMBER_CANVAS=OFF (SZ3b): built only in such a build. The canvas interfaces are not bound,
 * getContext() returns null, the element and the things that are not canvas (document.fonts, DOMMatrix, DOMPoint) stay.
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

EMB_TEST(canvas_interfaces_are_not_bound)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "typeof CanvasRenderingContext2D === 'undefined' && typeof Path2D === 'undefined'"));
    EMB_CHECK(js_true(e, "typeof CanvasGradient === 'undefined' && typeof CanvasPattern === 'undefined'"));
    EMB_CHECK(js_true(e, "typeof ImageData === 'undefined' && typeof ImageBitmap === 'undefined' && typeof TextMetrics === 'undefined'"));
    EMB_CHECK(js_true(e, "typeof createImageBitmap === 'undefined' && !('createImageBitmap' in window)"));
    env_free(e);
}

EMB_TEST(get_context_returns_null)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "typeof HTMLCanvasElement === 'function'"));
    EMB_CHECK(js_true(e, "var c = document.createElement('canvas'); c instanceof HTMLCanvasElement"));
    EMB_CHECK(js_true(e, "c.getContext('2d') === null && c.getContext('webgl') === null && c.getContext('bitmaprenderer') === null"));
    EMB_CHECK(js_true(e, "c.toDataURL() === 'data:,'"));
    EMB_CHECK(js_true(e, "var got = 'unset'; c.toBlob(function (b) { got = b; }); got === null"));
    EMB_CHECK(js_true(e, "c.width === 300 && c.height === 150"));
    EMB_CHECK(js_true(e, "c.width = 64; c.height = 32; c.width === 64 && c.getAttribute('height') === '32'"));
    env_free(e);
}

EMB_TEST(unrelated_interfaces_stay)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "typeof document.fonts === 'object' && typeof document.fonts.ready === 'object'"));
    EMB_CHECK(js_true(e, "new DOMMatrix().isIdentity && new DOMPoint(1, 2).y === 2 && typeof DOMRect === 'function'"));
    env_free(e);
}

EMB_TEST_MAIN()
