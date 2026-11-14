/*
 * The input bindings (W5d): the focus change algorithm (blur, focusout, focus, focusin in order, document.activeElement),
 * the focus of a removed element, the activation behaviors of a (through the context's navigate hook, and fragments
 * inside the document: hash, hashchange), label (forwards the click to its control), and the UI event payloads (the
 * related target and the coordinates a pointer event carries) as scripts read them.
 */
#include "emb_test.h"

#include "base/base.h"
#include "bindings/activation.h"
#include "bindings/bind.h"
#include "bindings/impl.h"
#include "bindings/impl_input.h"
#include "dom/dom.h"
#include "platform/platform.h"
#include "js/heap.h"
#include "js/jobs.h"
#include "js/js.h"
#include "js/runtime.h"

#include <string.h>

#include <ember/ember_input.h>

#include <SDL3/SDL_stdinc.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
} env;

static const char k_url[] = "http://127.0.0.1/start.html";

static embi_str url_of(void *user)
{
    (void)user;
    return (embi_str){k_url, (uint32_t)strlen(k_url)};
}

/* The navigate hook of the tests: remembers the last URL it was given. */
static char g_url[256];
static int g_nav;

static bool capture_nav(void *user, embi_str url, bool replace)
{
    (void)user;
    (void)replace;
    g_nav++;
    uint32_t n = url.len < sizeof g_url - 1 ? url.len : (uint32_t)(sizeof g_url - 1);
    SDL_memcpy(g_url, url.ptr, n);
    g_url[n] = '\0';
    return true;
}

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
    embi_bind_host h;
    memset(&h, 0, sizeof h);
    h.navigate = capture_nav;
    h.url = url_of;
    embi_bind_set_host(e.c, &h);
    embi_impl_input_ready();
    g_nav = 0;
    g_url[0] = '\0';
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

EMB_TEST(bind_input_focus_order_of_events)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a><input id=b>'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var a = document.getElementById('a'), b = document.getElementById('b'); "
                         "['focus', 'blur', 'focusin', 'focusout'].forEach(function (t) { "
                         "  a.addEventListener(t, function () { log.push('a:' + t); }); "
                         "  b.addEventListener(t, function () { log.push('b:' + t); }); }); "
                         "a.focus(); b.focus(); "
                         "log.join(',') === 'a:focus,a:focusin,a:blur,a:focusout,b:focus,b:focusin' && "
                         "document.activeElement === b"));
    EMB_CHECK(js_true(e, "log = []; b.blur(); log.join(',') === 'b:blur,b:focusout' && "
                         "document.activeElement === document.body"));
    env_free(e);
}

EMB_TEST(bind_input_focus_event_payload)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a><input id=b>'; true"));
    EMB_CHECK(js_true(e, "window.rel = null; var a = document.getElementById('a'), b = document.getElementById('b'); "
                         "b.addEventListener('focus', function (ev) { rel = ev.relatedTarget; }); "
                         "a.focus(); b.focus(); rel === a"));
    env_free(e);
}

EMB_TEST(bind_input_anchor_activation_navigates)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<a id=l href=\"/next\">n</a>'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('l').click(); true"));
    EMB_CHECK_EQ_INT(g_nav, 1);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/next") == 0);
    env_free(e);
}

/* R4c: an SVG a (href, or xlink:href without href) is a link: a click on a shape inside it bubbles to it and navigates. */
EMB_TEST(bind_input_svg_anchor_activation_navigates)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<svg><a href=\"/svg-next\"><rect id=r width=10 height=10 /></a>"
                         "<a xlink:href=\"/xlink-next\"><circle id=c r=5 /></a><a><rect id=n width=5 height=5 /></a></svg>'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('r').dispatchEvent(new MouseEvent('click', {bubbles: true})); true"));
    EMB_CHECK_EQ_INT(g_nav, 1);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/svg-next") == 0);
    EMB_CHECK(js_true(e, "document.getElementById('c').dispatchEvent(new MouseEvent('click', {bubbles: true})); true"));
    EMB_CHECK_EQ_INT(g_nav, 2);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/xlink-next") == 0);
    /* an SVG a with neither attribute is not a link */
    EMB_CHECK(js_true(e, "document.getElementById('n').dispatchEvent(new MouseEvent('click', {bubbles: true})); true"));
    EMB_CHECK_EQ_INT(g_nav, 2);
    env_free(e);
}

EMB_TEST(bind_input_fragment_link_stays_in_document)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<a id=l href=\"#sec\">n</a><p id=sec>s</p>'; "
                         "window.hc = []; window.addEventListener('hashchange', function (ev) { "
                         "  hc.push(ev.oldURL + ' ' + ev.newURL); }); true"));
    EMB_CHECK(js_true(e, "document.getElementById('l').click(); true"));
    EMB_CHECK_EQ_INT(g_nav, 0); /* the embedder is not asked to load a fragment of this document */
    EMB_CHECK(js_true(e, "location.hash === '#sec' && hc.length === 1 && "
                         "hc[0] === 'http://127.0.0.1/start.html http://127.0.0.1/start.html#sec'"));
    /* The same fragment again changes nothing: no second hashchange. */
    EMB_CHECK(js_true(e, "document.getElementById('l').click(); hc.length === 1"));
    env_free(e);
}

EMB_TEST(bind_input_canceled_click_does_not_navigate)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<a id=l href=\"/next\">n</a>'; "
                         "document.getElementById('l').addEventListener('click', function (ev) { "
                         "  ev.preventDefault(); }); true"));
    EMB_CHECK(js_true(e, "document.getElementById('l').click(); true"));
    EMB_CHECK_EQ_INT(g_nav, 0);
    env_free(e);
}

EMB_TEST(bind_input_label_forwards_click_to_control)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<label id=l for=c>x</label><input id=c>'; true"));
    EMB_CHECK(js_true(e, "window.hits = 0; document.getElementById('c').addEventListener('click', "
                         "function () { hits++; }); "
                         "document.getElementById('l').click(); hits === 1"));
    env_free(e);
}

EMB_TEST(bind_input_pointer_payload_from_the_context)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<span id=a></span><span id=b></span>'; "
                         "window.seen = null; document.getElementById('b').addEventListener('pointerover', "
                         "function (ev) { seen = ev; }); true"));
    embi_element *a = embi_get_element_by_id(e.doc, EMBI_STR("a"));
    embi_element *b = embi_get_element_by_id(e.doc, EMBI_STR("b"));
    EMB_REQUIRE(a != nullptr && b != nullptr);
    embi_input_ptr p;
    memset(&p, 0, sizeof p);
    p.clientX = 3;
    p.clientY = 4;
    p.pageX = 13;
    p.pageY = 14;
    p.offsetX = 1;
    p.offsetY = 2;
    p.movementX = -2;
    p.movementY = 5;
    p.related = &a->base;
    p.button = 0;
    p.buttons = 2;
    p.detail = 0;
    p.pointerType = "mouse";
    p.pointerId = 1;
    p.isPrimary = true;
    EMB_CHECK(embi_impl_input_pointer(e.c, &b->base, true, "pointerover", &p, true, true));
    EMB_CHECK(js_true(e, "seen !== null && seen.relatedTarget === document.getElementById('a') && "
                         "seen.pageX === 13 && seen.pageY === 14 && seen.offsetX === 1 && seen.offsetY === 2 && "
                         "seen.buttons === 2 && "
                         "seen.pointerType === 'mouse' && seen.isPrimary === true"));
    /* A plain PointerEvent built from JS keeps the constructor's values. */
    EMB_CHECK(js_true(e, "var p = new PointerEvent('pointerdown', {clientX: 3, button: 1, buttons: 4, detail: 2, "
                         "pointerType: 'touch'}); "
                         "p.clientX === 3 && p.button === 1 && p.buttons === 4 && p.detail === 2 && "
                         "p.pointerType === 'touch' && p instanceof MouseEvent"));
    env_free(e);
}

EMB_TEST(bind_input_blur_listener_refocus_stands)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a><input id=b><input id=c>'; true"));
    /* The blur listener of a focuses b: no recursion, the move to c stops, and activeElement agrees with the events. */
    EMB_CHECK(js_true(e, "window.log = []; var a = document.getElementById('a'), b = document.getElementById('b'), "
                         "c = document.getElementById('c'); "
                         "['focus', 'blur'].forEach(function (t) { [a, b, c].forEach(function (x) { "
                         "  x.addEventListener(t, function () { log.push(x.id + ':' + t); }); }); }); "
                         "a.focus(); a.addEventListener('blur', function () { b.focus(); }); c.focus(); "
                         "log.join(',') === 'a:focus,a:blur,b:focus' && document.activeElement === b"));
    /* A focus listener that blurs leaves nothing focused and skips focusin. */
    EMB_CHECK(js_true(e, "log = []; b.blur(); c.addEventListener('focus', function () { c.blur(); }); c.focus(); "
                         "document.activeElement === document.body"));
    env_free(e);
}

EMB_TEST(bind_input_wheel_event_payload_and_cancel)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<span id=a></span>'; window.w = null; "
                         "document.getElementById('a').addEventListener('wheel', function (ev) { w = ev; "
                         "  ev.preventDefault(); }); true"));
    embi_element *a = embi_get_element_by_id(e.doc, EMBI_STR("a"));
    EMB_REQUIRE(a != nullptr);
    embi_input_wheel wv;
    memset(&wv, 0, sizeof wv);
    wv.ptr.clientX = 5;
    wv.ptr.clientY = 6;
    wv.ptr.mods = EMB_MOD_CTRL;
    wv.deltaX = -3;
    wv.deltaY = 120;
    wv.deltaMode = 0;
    EMB_CHECK(!embi_impl_input_wheel(e.c, &a->base, &wv)); /* canceled */
    EMB_CHECK(js_true(e, "w instanceof WheelEvent && w instanceof MouseEvent && w.deltaX === -3 && w.deltaY === 120 && "
                         "w.deltaZ === 0 && w.deltaMode === 0 && w.clientX === 5 && w.ctrlKey && w.cancelable && "
                         "w.bubbles && w.isTrusted && w.defaultPrevented"));
    env_free(e);
}

EMB_TEST(bind_input_listening_probe)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<div id=o><span id=a></span></div><p id=p onclick=\"1\"></p>'; true"));
    embi_element *a = embi_get_element_by_id(e.doc, EMBI_STR("a"));
    embi_element *p = embi_get_element_by_id(e.doc, EMBI_STR("p"));
    EMB_REQUIRE(a != nullptr && p != nullptr);
    EMB_CHECK(!embi_impl_input_listening(e.c, &a->base, "pointermove"));
    EMB_CHECK(js_true(e, "document.getElementById('o').addEventListener('pointermove', function () {}); true"));
    EMB_CHECK(embi_impl_input_listening(e.c, &a->base, "pointermove")); /* on an ancestor */
    EMB_CHECK(!embi_impl_input_listening(e.c, &a->base, "pointerdown")); /* another type */
    EMB_CHECK(embi_impl_input_listening(e.c, &p->base, "click"));        /* a content attribute */
    EMB_CHECK(!embi_impl_input_listening(e.c, &p->base, "keydown"));
    EMB_CHECK(js_true(e, "window.addEventListener('keydown', function () {}); true"));
    EMB_CHECK(embi_impl_input_listening(e.c, &a->base, "keydown")); /* on the window */
    env_free(e);
}

EMB_TEST(bind_input_clipboard_event_round_trip)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a>'; window.seen = []; "
                         "document.getElementById('a').addEventListener('copy', function (ev) { "
                         "  seen.push(ev.clipboardData.getData('text/plain') + '|' + (ev instanceof ClipboardEvent)); "
                         "  ev.clipboardData.setData('text/plain', 'from listener \u00e9'); ev.preventDefault(); }); "
                         "document.getElementById('a').addEventListener('paste', function (ev) { "
                         "  seen.push(ev.clipboardData.getData('text')); }); true"));
    embi_element *a = embi_get_element_by_id(e.doc, EMBI_STR("a"));
    EMB_REQUIRE(a != nullptr);
    embi_sb out = {0};
    bool has = false;
    EMB_CHECK(!embi_impl_input_clipboard_event(e.c, &a->base, "copy", "initial", &out, &has)); /* canceled */
    EMB_CHECK(has);
    EMB_CHECK(out.len == 16 && memcmp(out.data, "from listener \xC3\xA9", 16) == 0);
    EMB_CHECK(embi_impl_input_clipboard_event(e.c, &a->base, "paste", "pasted", nullptr, nullptr));
    EMB_CHECK(js_true(e, "seen.join(',') === 'initial|true,pasted'"));
    embi_sb_free(&out);
    env_free(e);
}

EMB_TEST(bind_input_pointer_capture_needs_the_input_layer)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<span id=a></span>'; true"));
    /* No input layer attached to this context: the calls fail like an unknown pointer. */
    EMB_CHECK(js_true(e, "var a = document.getElementById('a'); var n = ''; "
                         "try { a.setPointerCapture(1); } catch (x) { n = x.name; } n === 'NotFoundError' && "
                         "a.hasPointerCapture(1) === false"));
    env_free(e);
}

EMB_TEST(bind_input_movement_defaults_to_zero_for_script_events)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var m = new MouseEvent('mousemove'); m.movementX === 0 && m.movementY === 0"));
    env_free(e);
}

EMB_TEST(bind_navigator_clipboard_exists)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "navigator.clipboard instanceof Clipboard && typeof navigator.clipboard.readText === 'function' && "
                         "navigator.clipboard.writeText('x') instanceof Promise && "
                         "navigator.clipboard === navigator.clipboard"));
    env_free(e);
}

EMB_TEST_MAIN()
