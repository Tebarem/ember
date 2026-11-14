/*
 * The Canvas 2D API (R4b) seen from JS, without a GPU: getContext and its identity, the width/height reset, the state stack
 * and the layer rules, styles, transforms, line styles, Path2D, hit tests, gradients and patterns, ImageData, measureText
 * errors, toDataURL, the argument rules (EnforceRange, NaN, Infinity), and the memory baseline. What the context draws is
 * tested in tests/unit/canvas (test_draw.c) and by the WPT run (tests/wpt-th/expectations/html-canvas.txt).
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "bindings/canvas_host.h"
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
    embi_canvas_env ce;
    memset(&ce, 0, sizeof ce);
    embi_canvas_bind_install(e.c, &ce);
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

#define SETUP "var cv = document.createElement('canvas'); var ctx = cv.getContext('2d');"

EMB_TEST(bind_canvas_element_and_context_identity)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var cv = document.createElement('canvas'); cv.width === 300 && cv.height === 150"));
    EMB_CHECK(js_true(e, "cv.width = 40; cv.height = 20; cv.getAttribute('width') === '40' && cv.height === 20"));
    EMB_CHECK(js_true(e, "var ctx = cv.getContext('2d'); ctx === cv.getContext('2d') && ctx.canvas === cv"));
    EMB_CHECK(js_true(e, "cv.getContext('webgl') === null && cv.getContext('bitmaprenderer') === null"));
    EMB_CHECK(js_true(e, "ctx instanceof CanvasRenderingContext2D && cv instanceof HTMLCanvasElement"));
    /* "non-negative integer" parsing stops at the first non-digit: " 0x10" is 0. */
    EMB_CHECK(js_true(e, "cv.setAttribute('width', ' 0x10'); cv.width === 0"));
    EMB_CHECK(js_true(e, "cv.setAttribute('width', 'abc'); cv.width === 300"));
    EMB_CHECK(js_true(e, "cv.setAttribute('width', '25'); cv.width === 25"));
    EMB_CHECK(js_true(e, "ctx.getContextAttributes().alpha === true && ctx.isContextLost() === false"));
    env_free(e);
}

EMB_TEST(bind_canvas_width_reset_clears_state)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "ctx.fillStyle = '#123456'; ctx.lineWidth = 7; ctx.translate(5, 6); cv.width = cv.width;"
                              "ctx.fillStyle === '#000000' && ctx.lineWidth === 1 && ctx.getTransform().e === 0"));
    EMB_CHECK(js_true(e, "ctx.save(); ctx.lineWidth = 3; cv.height = 10; ctx.restore(); ctx.lineWidth === 1"));
    env_free(e);
}

EMB_TEST(bind_canvas_state_stack_and_layers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "ctx.lineWidth = 4; ctx.save(); ctx.lineWidth = 9; ctx.restore(); ctx.lineWidth === 4"));
    EMB_CHECK(js_true(e, "ctx.restore(); ctx.restore(); ctx.lineWidth === 4")); /* an empty stack is ignored */
    EMB_CHECK(js_true(e, "ctx.globalAlpha = 0.5; ctx.beginLayer(); var a = ctx.globalAlpha; ctx.endLayer();"
                         "a === 1 && ctx.globalAlpha === 0.5"));
    EMB_CHECK(js_true(e, "var n = ''; try { ctx.endLayer(); } catch (x) { n = x.name; } n === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "n = ''; try { ctx.beginLayer(); ctx.restore(); } catch (x) { n = x.name; } ctx.reset(); n === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "n = ''; try { ctx.save(); ctx.endLayer(); } catch (x) { n = x.name; } n === 'InvalidStateError'"));
    env_free(e);
}

EMB_TEST(bind_canvas_attribute_rules)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "ctx.globalAlpha = 2; ctx.globalAlpha === 1 && (ctx.globalAlpha = NaN, ctx.globalAlpha === 1)"));
    EMB_CHECK(js_true(e, "ctx.globalCompositeOperation = 'bogus'; ctx.globalCompositeOperation === 'source-over'"));
    EMB_CHECK(js_true(e, "ctx.globalCompositeOperation = 'clear'; ctx.globalCompositeOperation === 'clear'"));
    EMB_CHECK(js_true(e, "ctx.lineCap = 'round'; ctx.lineCap = 'nope'; ctx.lineCap === 'round'"));
    EMB_CHECK(js_true(e, "ctx.lineWidth = -3; ctx.lineWidth === 1 && (ctx.lineWidth = Infinity, ctx.lineWidth === 1)"));
    EMB_CHECK(js_true(e, "ctx.font = '20px serif'; ctx.font === '20px serif'"));
    EMB_CHECK(js_true(e, "ctx.font = 'bogus'; ctx.font === '20px serif'"));
    EMB_CHECK(js_true(e, "ctx.font = '10PX  SERIF'; ctx.font === '10px serif'"));
    EMB_CHECK(js_true(e, "ctx.font = '10px {bogus}'; ctx.font === '10px serif'"));
    EMB_CHECK(js_true(e, "ctx.letterSpacing = '2EM'; ctx.letterSpacing === '2em'"));
    EMB_CHECK(js_true(e, "ctx.shadowColor = 'rgba(0, 0, 255, 0.5)'; ctx.shadowColor === 'rgba(0, 0, 255, 0.5)'"));
    EMB_CHECK(js_true(e, "ctx.fillStyle = 'red'; ctx.fillStyle === '#ff0000'"));
    EMB_CHECK(js_true(e, "ctx.fillStyle = 'nope'; ctx.fillStyle === '#ff0000'"));
    EMB_CHECK(js_true(e, "var t = ''; try { ctx.fillStyle = { toString() { throw new TypeError('x'); } }; } catch (x) { t = x.name; } t === 'TypeError'"));
    env_free(e);
}

EMB_TEST(bind_canvas_transforms)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "ctx.translate(10, 20); ctx.scale(2, 3); var m = ctx.getTransform();"
                              "m.a === 2 && m.d === 3 && m.e === 10 && m.f === 20"));
    EMB_CHECK(js_true(e, "ctx.setTransform(1, 0, 0, 1, 5, 6); ctx.getTransform().e === 5"));
    EMB_CHECK(js_true(e, "ctx.setTransform({ a: 2, d: 2, e: 1, f: 1 }); ctx.getTransform().a === 2"));
    EMB_CHECK(js_true(e, "ctx.setTransform(NaN, 0, 0, 1, 0, 0); ctx.getTransform().a === 2")); /* non-finite: ignored */
    EMB_CHECK(js_true(e, "ctx.resetTransform(); var r = ctx.getTransform(); r.a === 1 && r.e === 0"));
    env_free(e);
}

EMB_TEST(bind_canvas_path2d_and_hit_tests)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "var p = new Path2D(); p.rect(10, 10, 20, 20); ctx.isPointInPath(p, 20, 20) === true"));
    EMB_CHECK(js_true(e, "ctx.isPointInPath(p, 10, 10) === true && ctx.isPointInPath(p, 30, 30) === true")); /* edges count */
    EMB_CHECK(js_true(e, "ctx.isPointInPath(p, 31, 20) === false && ctx.isPointInPath(p, NaN, 20) === false"));
    EMB_CHECK(js_true(e, "var q = new Path2D('M0 0 L10 0 L10 10 Z'); ctx.isPointInPath(q, 8, 2) === true"));
    EMB_CHECK(js_true(e, "var c2 = new Path2D(p); c2.addPath(q, { e: 100 }); ctx.isPointInPath(c2, 108, 2) === true"));
    EMB_CHECK(js_true(e, "var tt = ''; try { ctx.isPointInPath(p, 1, 1, 'gazonk'); } catch (x) { tt = x.name; } tt === 'TypeError'"));
    EMB_CHECK(js_true(e, "tt = ''; try { ctx.isPointInPath(null, 1, 1); } catch (x) { tt = x.name; } tt === 'TypeError'"));
    EMB_CHECK(js_true(e, "ctx.beginPath(); ctx.rect(0, 0, 100, 100); ctx.rect(25, 25, 50, 50);"
                         "ctx.isPointInPath(50, 50) === true && ctx.isPointInPath(50, 50, 'evenodd') === false"));
    EMB_CHECK(js_true(e, "ctx.lineWidth = 10; ctx.beginPath(); ctx.moveTo(0, 50); ctx.lineTo(100, 50);"
                         "ctx.isPointInStroke(50, 53) === true && ctx.isPointInStroke(50, 60) === false"));
    EMB_CHECK(js_true(e, "var rr = ''; try { ctx.roundRect(0, 0, 10, 10, -1); } catch (x) { rr = x.name; } rr === 'RangeError'"));
    EMB_CHECK(js_true(e, "rr = ''; try { ctx.arc(0, 0, -1, 0, 1); } catch (x) { rr = x.name; } rr === 'IndexSizeError'"));
    EMB_CHECK(js_true(e, "ctx.beginPath(); ctx.roundRect(10, 10, 80, 30); ctx.isPointInPath(10, 10) === true"));
    env_free(e);
}

EMB_TEST(bind_canvas_gradients_patterns_and_dashes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "var g = ctx.createLinearGradient(0, 0, 10, 0); g.addColorStop(0, 'red'); g.addColorStop(1, '#00f');"
                              "ctx.fillStyle = g; ctx.fillStyle === g"));
    EMB_CHECK(js_true(e, "var er = ''; try { g.addColorStop(2, 'red'); } catch (x) { er = x.name; } er === 'IndexSizeError'"));
    EMB_CHECK(js_true(e, "er = ''; try { g.addColorStop(0.5, 'nope'); } catch (x) { er = x.name; } er === 'SyntaxError'"));
    EMB_CHECK(js_true(e, "er = ''; try { ctx.createLinearGradient(NaN, 0, 1, 1); } catch (x) { er = x.name; } er === 'TypeError'"));
    EMB_CHECK(js_true(e, "var pat = ctx.createPattern(cv, 'repeat'); pat !== null && pat.constructor === CanvasPattern"));
    EMB_CHECK(js_true(e, "er = ''; try { ctx.createPattern(cv, 'bogus'); } catch (x) { er = x.name; } er === 'SyntaxError'"));
    EMB_CHECK(js_true(e, "er = ''; try { ctx.createPattern(cv, undefined); } catch (x) { er = x.name; } er === 'SyntaxError'"));
    EMB_CHECK(js_true(e, "ctx.createPattern(cv, null) !== null && ctx.createPattern(cv, '') !== null"));
    EMB_CHECK(js_true(e, "ctx.setLineDash([2, 3]); ctx.getLineDash().join() === '2,3'"));
    EMB_CHECK(js_true(e, "ctx.setLineDash([1, 2, 3]); ctx.getLineDash().join() === '1,2,3,1,2,3'"));
    EMB_CHECK(js_true(e, "ctx.setLineDash([-1, 2]); ctx.getLineDash().join() === '1,2,3,1,2,3'")); /* invalid: ignored */
    env_free(e);
}

EMB_TEST(bind_canvas_image_data)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "var id = ctx.createImageData(4, 3); id.width === 4 && id.height === 3 && id.data.length === 48"));
    EMB_CHECK(js_true(e, "id.colorSpace === 'srgb' && id.data instanceof Uint8ClampedArray"));
    EMB_CHECK(js_true(e, "var i2 = new ImageData(new Uint8ClampedArray(16), 2); i2.width === 2 && i2.height === 2"));
    EMB_CHECK(js_true(e, "var x = ''; try { new ImageData(0, 1); } catch (q) { x = q.name; } x === 'IndexSizeError'"));
    EMB_CHECK(js_true(e, "x = ''; try { new ImageData(1 << 31, 1 << 31); } catch (q) { x = q.name; } x === 'IndexSizeError'"));
    EMB_CHECK(js_true(e, "x = ''; try { new ImageData(new Uint8ClampedArray(0), 1); } catch (q) { x = q.name; } x === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "x = ''; try { new ImageData(new Uint8ClampedArray(3), 1); } catch (q) { x = q.name; } x === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "x = ''; try { new ImageData(new Uint8Array(8), 1, 2); } catch (q) { x = q.name; } x === 'TypeError'"));
    EMB_CHECK(js_true(e, "x = ''; try { ctx.getImageData(Infinity, 0, 1, 1); } catch (q) { x = q.name; } x === 'TypeError'"));
    EMB_CHECK(js_true(e, "x = ''; try { ctx.createImageData(NaN, 1); } catch (q) { x = q.name; } x === 'TypeError'"));
    EMB_CHECK(js_true(e, "x = ''; try { ctx.getImageData(0, 0, 0, 1); } catch (q) { x = q.name; } x === 'IndexSizeError'"));
    EMB_CHECK(js_true(e, "var g1 = ctx.getImageData(0, 0, 2, 2); g1.width === 2 && g1.data.length === 16"));
    env_free(e);
}

EMB_TEST(bind_canvas_data_url_and_blob)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "cv.width = 2; cv.height = 2; cv.toDataURL().indexOf('data:image/png;base64,') === 0"));
    EMB_CHECK(js_true(e, "cv.toDataURL('image/jpeg').indexOf('data:image/png') === 0")); /* only PNG is encoded */
    EMB_CHECK(js_true(e, "var got = null; cv.toBlob(function (b) { got = b; }); got !== null && got.type === 'image/png'"));
    EMB_CHECK(js_true(e, "cv.width = 0; cv.toDataURL() === 'data:,'"));
    env_free(e);
}

EMB_TEST(bind_canvas_fonts_set)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.fonts.status === 'loaded' && document.fonts.check('12px serif') === true"));
    EMB_CHECK(js_true(e, "document.fonts.ready instanceof Promise && document.fonts === document.fonts"));
    env_free(e);
}

/* This environment has no style engine, so nothing is computed: the empty string is the exact answer. The used size of a
 * laid out canvas (its bitmap size as the replaced element's natural size) is checked by the view test, test_embed_canvas.c. */
EMB_TEST(bind_canvas_computed_size_of_a_canvas)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var c3 = document.createElement('canvas'); c3.width = 100; document.body.appendChild(c3);"
                         "getComputedStyle(c3).width === ''"));
    env_free(e);
}

static void canvas_run(const char *src)
{
    env e = env_new();
    embi_js_value v = embi_js_eval(embi_bind_realm(e.c), src, strlen(src), "t.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
    }
    env_free(e);
}

EMB_TEST(bind_canvas_memory_returns_to_baseline)
{
    const char *src = SETUP
        "ctx.fillStyle = ctx.createLinearGradient(0, 0, 5, 5); ctx.fillRect(0, 0, 5, 5); var p = new Path2D('M0 0 L5 5');"
        "ctx.stroke(p); var d = ctx.getImageData(0, 0, 3, 3); ctx.putImageData(d, 1, 1); ctx.createPattern(cv, 'repeat');"
        "createImageBitmap(cv).then(function (b) { b.close(); }); cv.toDataURL(); ctx.beginLayer(); ctx.endLayer();";
    canvas_run(src); /* warm-up: interns the atoms and sizes the tables */
    size_t base = embi_mem_live();
    canvas_run(src);
    canvas_run(src);
    EMB_CHECK_EQ_INT((long)embi_mem_live(), (long)base);
}

EMB_TEST(bind_canvas_script_controlled_sizes_are_bounded)
{
    env e = env_new();
    EMB_CHECK(js_true(e, SETUP "cv.width = 100000; cv.height = 100000; cv.toDataURL() === 'data:,'"));
    EMB_CHECK(js_true(e, "var blob = 1; cv.toBlob(function (b) { blob = b; }); blob === null"));
    EMB_CHECK(js_true(e, "ctx.createPattern(cv, 'repeat') === null"));
    EMB_CHECK(js_true(e, "ctx.drawImage(cv, 0, 0); ctx.fillRect(0, 0, 5, 5); true"));
    EMB_CHECK(js_true(e, "cv.width = 2147483647; cv.height = 2147483647; cv.toDataURL() === 'data:,'"));
    EMB_CHECK(js_true(e, "var small = document.createElement('canvas'); small.width = 4; small.height = 4; "
                         "var rej = []; var big = [2147483648, 4294967296, 1e300, Infinity, 100000];"
                         "big.forEach(function (n) { createImageBitmap(small, 0, 0, n, n).catch(function (x) { rej.push(x.name); }); });"
                         "true"));
    env_free(e);
}

static int g_pixel_calls;
static uint32_t g_gen = 1;
static const uint8_t g_sprite[4 * 4 * 4] = {255};

static bool test_pixels(void *user, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h)
{
    (void)user;
    (void)url;
    g_pixel_calls++;
    *px = g_sprite;
    *w = 4;
    *h = 4;
    return true;
}

static uint32_t test_gen(void *user)
{
    (void)user;
    return g_gen;
}

EMB_TEST(bind_canvas_draw_image_shares_one_picture_per_url)
{
    env e = env_new();
    embi_canvas_env ce;
    memset(&ce, 0, sizeof ce);
    ce.image_pixels = test_pixels;
    ce.image_gen = test_gen;
    embi_canvas_bind_install(e.c, &ce);
    g_pixel_calls = 0;
    EMB_CHECK(js_true(e, SETUP "var im = document.createElement('img'); im.setAttribute('src', 'http://t.test/s.png');"
                              "for (var i = 0; i < 200; i++) { ctx.drawImage(im, i, 0); } true"));
    EMB_CHECK_EQ_INT(g_pixel_calls, 1); /* decoded pixels were copied once, not per draw */
    /* A new image generation (a reload) fetches the pixels again. */
    g_gen++;
    EMB_CHECK(js_true(e, "ctx.drawImage(im, 0, 0); ctx.drawImage(im, 1, 1); true"));
    EMB_CHECK_EQ_INT(g_pixel_calls, 2);
    EMB_CHECK(js_true(e, "ctx.createPattern(im, 'repeat') !== null"));
    EMB_CHECK_EQ_INT(g_pixel_calls, 2);
    env_free(e);
}

/* A windowed view makes its GPU device on first need: the host gives gpu_get instead of gpu, and it must be asked with
 * the host's own user pointer (the binding puts its own in env.user). */
static int g_gpu_calls;
static void *g_gpu_user;

static embi_canvas_gpu *test_gpu_get(void *user)
{
    g_gpu_calls++;
    g_gpu_user = user;
    return nullptr;
}

EMB_TEST(bind_canvas_asks_the_host_for_its_gpu_with_the_host_user)
{
    env e = env_new();
    static int host_user;
    embi_canvas_env ce;
    memset(&ce, 0, sizeof ce);
    ce.user = &host_user;
    ce.gpu_get = test_gpu_get;
    embi_canvas_bind_install(e.c, &ce);
    g_gpu_calls = 0;
    g_gpu_user = nullptr;
    EMB_CHECK(js_true(e, SETUP "ctx.fillRect(0, 0, 4, 4); true"));
    EMB_CHECK_EQ_INT(g_gpu_calls, 1);
    EMB_CHECK(g_gpu_user == &host_user);
    env_free(e);
}

EMB_TEST_MAIN()
