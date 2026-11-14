/*
 * Event binding tests (W5b): the Event interfaces, EventTarget dispatch from JS against a real
 * document, event handler attributes (including content attributes), exceptions reported to the
 * window, createEvent, click(), AbortController and AbortSignal. Each test evaluates JS expressions
 * that must be true.
 */
#include "emb_test.h"

#include "base/base.h"
#include "bindings/activation.h"
#include "bindings/bind.h"
#include "bindings/impl.h"
#include "js/heap.h"
#include "js/jobs.h"
#include "js/js.h"
#include "js/runtime.h"

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

static embi_js_realm *realm(env e)
{
    return embi_bind_realm(e.c);
}

/* Live heap bytes after a full collection. */
static size_t heap_live(env e)
{
    embi_js_gc(e.rt);
    return e.rt->heap.live_after_gc;
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(realm(e), src, strlen(src), "test.js", 0);
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

EMB_TEST(bind_event_constructors_and_fields)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var e = new Event('x', {bubbles: true, cancelable: true}); "
                         "e.type === 'x' && e.bubbles && e.cancelable && !e.defaultPrevented && "
                         "e.eventPhase === 0 && e.target === null && !e.isTrusted && "
                         "typeof e.timeStamp === 'number' && e.composed === false"));
    EMB_CHECK(js_true(e, "var p = new Event('x', {cancelable: true}); p.preventDefault(); p.defaultPrevented === true && "
                         "p.returnValue === false"));
    EMB_CHECK(js_true(e, "var q = new Event('x'); q.preventDefault(); q.defaultPrevented === false"));
    EMB_CHECK(js_true(e, "Event.NONE === 0 && Event.AT_TARGET === 2 && Event.BUBBLING_PHASE === 3"));
    EMB_CHECK(js_true(e, "new CustomEvent('c', {detail: 42}).detail === 42 && new CustomEvent('c').detail === null"));
    EMB_CHECK(js_true(e, "var m = new MouseEvent('click', {clientX: 5, clientY: 6, button: 2, ctrlKey: true}); "
                         "m.clientX === 5 && m.clientY === 6 && m.button === 2 && m.ctrlKey === true && "
                         "m.x === 5 && m.relatedTarget === null"));
    EMB_CHECK(js_true(e, "var mm = new MouseEvent('x'); mm instanceof UIEvent && mm instanceof Event && "
                         "mm.detail === 0 && mm.view === null"));
    EMB_CHECK(js_true(e, "var k = new KeyboardEvent('keydown', {key: 'a', code: 'KeyA', shiftKey: true}); "
                         "k.key === 'a' && k.code === 'KeyA' && k.getModifierState('Shift') === true && "
                         "k.getModifierState('Control') === false && k.repeat === false"));
    EMB_CHECK(js_true(e, "new WheelEvent('wheel', {deltaY: 3}).deltaY === 3 && WheelEvent.DOM_DELTA_LINE === 1"));
    EMB_CHECK(js_true(e, "var pe = new PointerEvent('pointerdown', {pointerType: 'mouse', pointerId: 2, isPrimary: true}); "
                         "pe.pointerType === 'mouse' && pe.pointerId === 2 && pe.isPrimary === true && "
                         "pe instanceof MouseEvent"));
    EMB_CHECK(js_true(e, "new ErrorEvent('error', {message: 'm', lineno: 4}).message === 'm' && "
                         "new ErrorEvent('error', {lineno: 4}).lineno === 4"));
    EMB_CHECK(js_true(e, "new HashChangeEvent('hashchange', {newURL: '#b'}).newURL === '#b'"));
    EMB_CHECK(js_true(e, "new PopStateEvent('popstate', {state: {a: 1}}).state.a === 1 && "
                         "new PopStateEvent('popstate').state === null"));
    EMB_CHECK(js_true(e, "new MediaQueryListEvent('change', {matches: true, media: '(x)'}).matches === true"));
    EMB_CHECK(js_true(e, "new InputEvent('input', {data: 'q', inputType: 'insertText'}).inputType === 'insertText' && "
                         "new CompositionEvent('compositionend', {data: 'ok'}).data === 'ok'"));
    EMB_CHECK(js_true(e, "new SubmitEvent('submit').submitter === null && "
                         "new FocusEvent('focus').relatedTarget === null"));
    EMB_CHECK(js_true(e, "new PromiseRejectionEvent('unhandledrejection', {promise: Promise.resolve(1), reason: 7}).reason === 7"));
    env_free(e);
}

EMB_TEST(bind_event_dispatch_order_and_targets)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var log = []; var p = document.createElement('div'); var c = document.createElement('span'); "
                         "document.body.appendChild(p); p.appendChild(c); "
                         "p.addEventListener('x', function (ev) { log.push('p-cap:' + ev.eventPhase + ':' + "
                         "(ev.currentTarget === p) + ':' + (ev.target === c)); }, true); "
                         "c.addEventListener('x', function (ev) { log.push('c:' + ev.eventPhase); }); "
                         "p.addEventListener('x', function () { log.push('p-bub'); }); "
                         "document.body.addEventListener('x', function () { log.push('body'); }); "
                         "c.dispatchEvent(new Event('x', {bubbles: true})) === true && "
                         "log.join(',') === 'p-cap:1:true:true,c:2,p-bub,body'"));
    EMB_CHECK(js_true(e, "log = []; c.dispatchEvent(new Event('x')); log.join(',') === 'p-cap:1:true:true,c:2'"));
    EMB_CHECK(js_true(e, "var seen = null; c.addEventListener('y', function (ev) { seen = ev.composedPath().length + ':' + "
                         "(ev.composedPath()[0] === c) + ':' + (ev.composedPath()[ev.composedPath().length - 1] === window); }); "
                         "c.dispatchEvent(new Event('y')); seen === '6:true:true'"));
    EMB_CHECK(js_true(e, "var after = null; var ev2 = new Event('z', {bubbles: true}); c.addEventListener('z', function () {}); "
                         "c.dispatchEvent(ev2); ev2.target === c && ev2.currentTarget === null && ev2.eventPhase === 0 && "
                         "ev2.composedPath().length === 0"));
    EMB_CHECK(js_true(e, "var stopped = []; var w1 = document.createElement('a'); var w2 = document.createElement('a'); "
                         "document.body.appendChild(w1); w1.appendChild(w2); "
                         "w1.addEventListener('s', function (ev) { stopped.push('w1'); }); "
                         "w2.addEventListener('s', function (ev) { stopped.push('w2'); ev.stopPropagation(); "
                         "stopped.push(ev.cancelBubble); }); "
                         "w2.addEventListener('s', function () { stopped.push('w2b'); }); "
                         "w2.dispatchEvent(new Event('s', {bubbles: true})); stopped.join() === 'w2,true,w2b'"));
    EMB_CHECK(js_true(e, "var imm = []; var w3 = document.createElement('b'); document.body.appendChild(w3); "
                         "w3.addEventListener('i', function (ev) { imm.push(1); ev.stopImmediatePropagation(); }); "
                         "w3.addEventListener('i', function () { imm.push(2); }); "
                         "w3.addEventListener('i', function () { imm.push(3); }); "
                         "w3.dispatchEvent(new Event('i')); imm.join() === '1'"));
    EMB_CHECK(js_true(e, "var canc = document.createElement('u'); document.body.appendChild(canc); "
                         "canc.addEventListener('k', function (ev) { ev.preventDefault(); }); "
                         "var kev = new Event('k', {cancelable: true}); canc.dispatchEvent(kev) === false && "
                         "kev.defaultPrevented === true"));
    EMB_CHECK(js_true(e, "var pas = document.createElement('i'); document.body.appendChild(pas); "
                         "pas.addEventListener('w', function (ev) { ev.preventDefault(); }, {passive: true}); "
                         "var wev = new Event('w', {cancelable: true}); pas.dispatchEvent(wev) === true && "
                         "wev.defaultPrevented === false"));
    env_free(e);
}

EMB_TEST(bind_event_listener_options_and_mutation)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = document.createElement('i'); document.body.appendChild(t); var n = 0; "
                         "var f = function () { n++; }; t.addEventListener('e', f, {once: true}); "
                         "t.dispatchEvent(new Event('e')); t.dispatchEvent(new Event('e')); n === 1"));
    EMB_CHECK(js_true(e, "var hit = []; var h = function () { hit.push('h'); }; "
                         "var g = function () { hit.push('g'); t.removeEventListener('e2', h); }; "
                         "t.addEventListener('e2', g); t.addEventListener('e2', h); "
                         "t.dispatchEvent(new Event('e2')); hit.join() === 'g'"));
    EMB_CHECK(js_true(e, "var added = 0; var k = function () { t.addEventListener('e3', function () { added++; }); }; "
                         "t.addEventListener('e3', k); t.dispatchEvent(new Event('e3')); added === 0 && "
                         "(t.dispatchEvent(new Event('e3')), added === 1)"));
    EMB_CHECK(js_true(e, "var dup = 0; var d = function () { dup++; }; t.addEventListener('e4', d); "
                         "t.addEventListener('e4', d); t.addEventListener('e4', d, true); "
                         "t.dispatchEvent(new Event('e4')); dup === 2"));
    EMB_CHECK(js_true(e, "var cap = 0; t.addEventListener('e5', function () { cap++; }, true); "
                         "t.removeEventListener('e5', function () {}, true); t.dispatchEvent(new Event('e5')); cap === 1"));
    EMB_CHECK(js_true(e, "var et = new EventTarget(); var seen = 0; var lis = {handleEvent: function (ev) { seen += ev.type === 'a' ? 1 : 0; }}; "
                         "et.addEventListener('a', lis); et.dispatchEvent(new Event('a')); seen === 1"));
    env_free(e);
}

EMB_TEST(bind_event_handler_attributes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var b = document.createElement('button'); document.body.appendChild(b); var hits = 0; "
                         "b.onclick = function () { hits++; }; b.click(); hits === 1 && typeof b.onclick === 'function'"));
    EMB_CHECK(js_true(e, "b.onclick = null; b.click(); hits === 1 && b.onclick === null"));
    EMB_CHECK(js_true(e, "var order = []; var t = document.createElement('i'); "
                         "t.addEventListener('q', function () { order.push('first'); }); "
                         "t.onq = null; t.onclick = function () { order.push('handler'); }; "
                         "t.addEventListener('click', function () { order.push('listener'); }); "
                         "t.click(); order.join() === 'handler,listener'"));
    EMB_CHECK(js_true(e, "var kd = document.createElement('input'); var kh = function () { return false; }; "
                         "kd.onkeydown = kh; var kev = new KeyboardEvent('keydown', {cancelable: true}); "
                         "kd.dispatchEvent(kev); kev.defaultPrevented === true"));
    EMB_CHECK(js_true(e, "var fresh = document.createElement('p'); fresh.onclick = function () {}; "
                         "typeof fresh.onclick === 'function'"));
    env_free(e);
}

EMB_TEST(bind_event_content_attribute_handlers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var hits = []; var b = document.createElement('button'); document.body.appendChild(b); "
                         "b.setAttribute('onclick', \"hits.push(this.tagName + ':' + event.type)\"); "
                         "b.click(); hits.join() === 'BUTTON:click' && typeof b.onclick === 'function'"));
    EMB_CHECK(js_true(e, "b.setAttribute('onclick', \"hits.push('two')\"); b.click(); hits.join() === 'BUTTON:click,two'"));
    EMB_CHECK(js_true(e, "b.removeAttribute('onclick'); b.click(); hits.length === 2 && b.onclick === null"));
    EMB_CHECK(js_true(e, "var bad = document.createElement('i'); bad.setAttribute('onclick', 'this is not js {'); "
                         "var errs = 0; window.addEventListener('error', function (ev) { errs++; ev.preventDefault(); }); "
                         "bad.click(); errs === 1 && bad.onclick === null"));
    env_free(e);
}

/* CF1: the dispatch finds the content attribute handler and the listeners in one pass and brings the handler up to date
 * with the attribute only when there is one or the handler came from one. A changed value runs the new code, an attribute
 * set or removed by a capture listener above before the target's turn counts at the target, the handler of an ancestor
 * runs in the bubble phase, and a script-set handler gives way to an attribute set later. */
EMB_TEST(bind_event_content_attribute_handler_during_dispatch)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var out = []; var d = document.createElement('div'); document.body.appendChild(d); "
                         "d.setAttribute('onclick', \"out.push('h')\"); d.click(); "
                         "d.setAttribute('onclick', \"out.push('h2')\"); d.click(); out.join() === 'h,h2'"));
    EMB_CHECK(js_true(e, "var o2 = []; var p = document.createElement('div'); var c = document.createElement('span'); "
                         "p.appendChild(c); document.body.appendChild(p); "
                         "p.addEventListener('click', function () { c.setAttribute('onclick', \"o2.push('attr')\"); }, true); "
                         "c.dispatchEvent(new MouseEvent('click', {bubbles: true})); o2.join() === 'attr'"));
    EMB_CHECK(js_true(e, "var o3 = []; var p3 = document.createElement('div'); var q = document.createElement('i'); "
                         "p3.appendChild(q); document.body.appendChild(p3); q.setAttribute('onclick', \"o3.push('gone')\"); "
                         "p3.addEventListener('click', function () { o3.push('cap'); q.removeAttribute('onclick'); }, true); "
                         "q.click(); o3.join() === 'cap' && q.onclick === null"));
    EMB_CHECK(js_true(e, "var o4 = []; var r = document.createElement('div'); var k = document.createElement('b'); "
                         "r.appendChild(k); document.body.appendChild(r); r.setAttribute('onclick', \"o4.push('rh')\"); "
                         "r.addEventListener('click', function () { o4.push('rc'); }, true); "
                         "k.click(); o4.join() === 'rc,rh'"));
    EMB_CHECK(js_true(e, "var o5 = []; var s5 = document.createElement('div'); document.body.appendChild(s5); "
                         "s5.onclick = function () { o5.push('js'); }; s5.click(); "
                         "s5.setAttribute('onclick', \"o5.push('attr')\"); s5.click(); "
                         "s5.addEventListener('click', function () { o5.push('l'); }); s5.click(); "
                         "o5.join() === 'js,attr,attr,l'"));
    env_free(e);
}

EMB_TEST(bind_event_exceptions_go_to_the_window)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var errs = 0; var msg = ''; "
                         "window.addEventListener('error', function (ev) { errs++; msg = ev.message; ev.preventDefault(); }); "
                         "var u = document.createElement('u'); var after = 0; "
                         "u.addEventListener('z', function () { throw new Error('boom'); }); "
                         "u.addEventListener('z', function () { after++; }); "
                         "u.dispatchEvent(new Event('z')); errs === 1 && after === 1 && msg.indexOf('boom') >= 0"));
    env_free(e);
}

EMB_TEST(bind_event_create_and_init)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var ce = document.createEvent('MouseEvent'); ce.initEvent('custom', true, true); "
                         "ce.type === 'custom' && ce.bubbles && ce.cancelable && !ce.isTrusted && ce instanceof MouseEvent"));
    EMB_CHECK(js_true(e, "var threw = ''; try { document.createEvent('Nope'); } catch (x) { threw = x.name; } threw === 'NotSupportedError'"));
    EMB_CHECK(js_true(e, "var t = document.createElement('i'); document.body.appendChild(t); var st = ''; "
                         "try { t.dispatchEvent(document.createEvent('Event')); } catch (x) { st = x.name; } st === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "var re = ''; var rt = document.createElement('s'); "
                         "rt.addEventListener('re', function (ev) { try { rt.dispatchEvent(ev); } catch (x) { re = x.name; } }); "
                         "rt.dispatchEvent(new Event('re')); re === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "var iv = new Event('x'); var t2 = document.createElement('s'); var did = 0; "
                         "t2.addEventListener('x', function () { did++; }); t2.dispatchEvent(iv); did === 1 && "
                         "(function () { try { t2.dispatchEvent(iv); return true; } catch (x) { return x.name === 'InvalidStateError'; } })()"));
    env_free(e);
}

EMB_TEST(bind_event_click_bubbles_to_window)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var got = ''; var winGot = ''; var cl = document.createElement('button'); "
                         "document.body.appendChild(cl); "
                         "document.addEventListener('click', function (ev) { got = ev.target.tagName; }); "
                         "window.addEventListener('click', function (ev) { winGot = ev.type + ':' + ev.isTrusted; }); "
                         "cl.click(); got === 'BUTTON' && winGot === 'click:false'"));
    env_free(e);
}

EMB_TEST(bind_abort_controller_and_signal)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var ac = new AbortController(); var s = 0; var t = document.createElement('i'); "
                         "t.addEventListener('q', function () { s++; }, {signal: ac.signal}); "
                         "t.dispatchEvent(new Event('q')); ac.abort(); t.dispatchEvent(new Event('q')); "
                         "s === 1 && ac.signal.aborted === true"));
    EMB_CHECK(js_true(e, "var a2 = new AbortController(); a2.abort(); a2.signal.reason.name === 'AbortError' && "
                         "a2.signal.reason instanceof DOMException"));
    EMB_CHECK(js_true(e, "var a3 = new AbortController(); var fired = 0; a3.signal.addEventListener('abort', function () { fired++; }); "
                         "a3.abort('r'); a3.abort('again'); fired === 1 && a3.signal.reason === 'r'"));
    EMB_CHECK(js_true(e, "var a4 = new AbortController(); a4.abort('x'); var thrown = ''; "
                         "try { a4.signal.throwIfAborted(); } catch (x) { thrown = x; } thrown === 'x'"));
    EMB_CHECK(js_true(e, "var a5 = new AbortController(); var n5 = 0; a5.abort(); "
                         "var tt = document.createElement('i'); tt.addEventListener('q', function () { n5++; }, {signal: a5.signal}); "
                         "tt.dispatchEvent(new Event('q')); n5 === 0"));
    EMB_CHECK(js_true(e, "AbortSignal.abort('why').reason === 'why' && AbortSignal.abort().aborted === true"));
    EMB_CHECK(js_true(e, "var p1 = new AbortController(); var p2 = new AbortController(); var comb = AbortSignal.any([p1.signal, p2.signal]); "
                         "p2.abort('second'); comb.aborted === true && comb.reason === 'second' && p1.signal.aborted === false"));
    EMB_CHECK(js_true(e, "var pre = new AbortController(); pre.abort('early'); AbortSignal.any([pre.signal]).reason === 'early'"));
    EMB_CHECK(js_true(e, "typeof AbortSignal.timeout === 'function' && AbortSignal.timeout(10).aborted === false"));
    env_free(e);
}

static void churn(void)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var root = document.createElement('div'); document.body.appendChild(root); "
                         "for (var i = 0; i < 100; i++) { var n = document.createElement('span'); root.appendChild(n); "
                         "  var ac = new AbortController(); n.addEventListener('c', function () {}, {signal: ac.signal}); "
                         "  n.onclick = function () {}; n.dispatchEvent(new MouseEvent('c', {bubbles: true})); "
                         "  ac.abort(); } root.remove(); true"));
    env_free(e);
}

/* Event objects, listeners and abort signals are released with their document (no leak). */
EMB_TEST(bind_event_memory_returns_to_baseline)
{
    churn();
    size_t before = embi_mem_live();
    churn();
    churn();
    EMB_CHECK(embi_mem_live() == before);
}

/* Listeners live with their detached nodes: once the nodes are unreachable, a collection frees
 * the wrappers, the nodes and their listener lists. */
EMB_TEST(bind_event_listeners_of_detached_nodes_are_collected)
{
    env e = env_new();
    size_t baseline = heap_live(e);
    EMB_CHECK(js_true(e, "var last = null; for (var i = 0; i < 300; i++) { last = document.createElement('div'); "
                         "last.addEventListener('x', function () {}); last.onclick = function () {}; } true"));
    /* nothing is collected while the loop runs only by chance; the nodes are all garbage but the last one */
    EMB_CHECK(js_true(e, "last = null; true"));
    size_t after = heap_live(e);
    /* everything the 300 nodes and their listeners allocated is gone again */
    EMB_CHECK(after < baseline + baseline / 50 + 4096);
    env_free(e);
}

/* ------------------------------------------------------------------ review round 3 additions */

EMB_TEST(bind_event_create_event_legacy_initializers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var me = document.createEvent('MouseEvents'); "
                         "me.initMouseEvent('click', true, true, window, 1, 10, 20, 30, 40, true, false, true, false, 2, null); "
                         "me.type === 'click' && me.bubbles && me.cancelable && me.screenX === 10 && me.screenY === 20 && "
                         "me.clientX === 30 && me.clientY === 40 && me.ctrlKey === true && me.altKey === false && "
                         "me.shiftKey === true && me.metaKey === false && me.button === 2 && me.detail === 1 && "
                         "me.view === window && me.relatedTarget === null && !me.isTrusted && me instanceof MouseEvent"));
    EMB_CHECK(js_true(e, "var seenx = -1; var mt = document.createElement('i'); "
                         "mt.addEventListener('click', function (ev) { seenx = ev.clientX; }); mt.dispatchEvent(me); seenx === 30"));
    EMB_CHECK(js_true(e, "var ue = document.createEvent('UIEvents'); ue.initUIEvent('resize', false, false, null, 5); "
                         "ue.type === 'resize' && ue.detail === 5 && ue.view === null && ue instanceof UIEvent"));
    EMB_CHECK(js_true(e, "var ke = document.createEvent('KeyboardEvent'); "
                         "ke.initKeyboardEvent('keydown', true, true, window, 'a', 1, true, false, false, true); "
                         "ke.type === 'keydown' && ke.key === 'a' && ke.location === 1 && ke.ctrlKey === true && "
                         "ke.altKey === false && ke.shiftKey === false && ke.metaKey === true && ke instanceof KeyboardEvent"));
    EMB_CHECK(js_true(e, "var me2 = new MouseEvent('x'); me2.initEvent('y', false, false); me2.type === 'y' && !me2.isTrusted"));
    env_free(e);
}

EMB_TEST(bind_abort_signal_any_fires_the_source_before_its_dependents)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var log = []; var ac = new AbortController(); var d1 = AbortSignal.any([ac.signal]); "
                         "var d2 = AbortSignal.any([d1]); "
                         "ac.signal.addEventListener('abort', function () { log.push('src:' + d1.aborted + d2.aborted + d1.reason); }); "
                         "d1.addEventListener('abort', function () { log.push('d1'); }); "
                         "d2.addEventListener('abort', function () { log.push('d2'); }); "
                         "ac.abort('why'); log.join() === 'src:truetruewhy,d1,d2' && d2.reason === 'why'"));
    env_free(e);
}

/* A dispatch started by the host (no script running) runs a microtask checkpoint after each listener;
 * one started by a script does not. */
EMB_TEST(bind_event_microtask_checkpoint_between_listeners)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var log = []; window.addEventListener('mt', function () { log.push('l1'); "
                         "Promise.resolve().then(function () { log.push('micro'); }); }); "
                         "window.addEventListener('mt', function () { log.push('l2'); }); true"));
    embi_impl_fire(e.c, e.c->gbo, "mt", EMBI_JS_UNDEFINED);
    EMB_CHECK(js_true(e, "log.join() === 'l1,micro,l2'"));
    EMB_CHECK(js_true(e, "log.length = 0; window.dispatchEvent(new Event('mt')); log.join() === 'l1,l2'"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(js_true(e, "log.join() === 'l1,l2,micro'"));
    env_free(e);
}

EMB_TEST(bind_event_handler_results_and_callbacks)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var b2 = document.createElement('button'); document.body.appendChild(b2); "
                         "b2.setAttribute('onclick', 'return false'); var ce2 = new MouseEvent('click', {cancelable: true, bubbles: true}); "
                         "b2.dispatchEvent(ce2); ce2.defaultPrevented === true"));
    EMB_CHECK(js_true(e, "var ce3 = new MouseEvent('click', {bubbles: true}); b2.dispatchEvent(ce3); ce3.defaultPrevented === false"));
    EMB_CHECK(js_true(e, "var b3 = document.createElement('b'); b3.onclick = function () { return false; }; "
                         "var ce4 = new MouseEvent('click', {cancelable: true}); b3.dispatchEvent(ce4); ce4.defaultPrevented === true"));
    /* handleEvent objects, and exceptions thrown by handleEvent go to the window */
    EMB_CHECK(js_true(e, "var hev = ''; var obj = {handleEvent: function (ev) { hev = (this === obj) + ':' + ev.type; }}; "
                         "var h1 = document.createElement('i'); h1.addEventListener('h', obj); h1.dispatchEvent(new Event('h')); hev === 'true:h'"));
    EMB_CHECK(js_true(e, "var herr = 0; var hmsg = ''; window.addEventListener('error', function (ev) { herr++; hmsg = ev.message; ev.preventDefault(); }); "
                         "var h2 = document.createElement('i'); var after2 = 0; "
                         "h2.addEventListener('h', {handleEvent: function () { throw new Error('hboom'); }}); "
                         "h2.addEventListener('h', function () { after2++; }); h2.dispatchEvent(new Event('h')); "
                         "herr === 1 && after2 === 1 && hmsg.indexOf('hboom') >= 0"));
    /* event handler attributes accept callables only */
    EMB_CHECK(js_true(e, "var h3 = document.createElement('i'); h3.onclick = {handleEvent: function () {}}; h3.onclick === null"));
    EMB_CHECK(js_true(e, "h3.onclick = function () {}; typeof h3.onclick === 'function'"));
    EMB_CHECK(js_true(e, "h3.onclick = 5; h3.onclick === null"));
    EMB_CHECK(js_true(e, "var thr = ''; try { h3.addEventListener('x', 5); } catch (x) { thr = x.name; } thr === 'TypeError'"));
    env_free(e);
}

EMB_TEST(bind_event_window_onerror_gets_the_legacy_arguments)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var got = ''; var dp = null; window.onerror = function (msg, src, line, col, err) { "
                         "got = [typeof msg, typeof src, typeof line, typeof col, err && err.message].join(); return true; }; "
                         "window.addEventListener('error', function (ev) { dp = ev.defaultPrevented; }); "
                         "var u2 = document.createElement('u'); u2.addEventListener('z', function () { throw new Error('bang'); }); "
                         "u2.dispatchEvent(new Event('z')); got === 'string,string,number,number,bang' && dp === true"));
    EMB_CHECK(js_true(e, "var dp2 = null; window.onerror = function () { return false; }; "
                         "window.addEventListener('error', function (ev) { dp2 = ev.defaultPrevented; }); "
                         "u2.dispatchEvent(new Event('z')); dp2 === false"));
    env_free(e);
}

EMB_TEST(bind_event_removed_and_readded_listener_does_not_run_in_the_same_dispatch)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var out = []; var t3 = document.createElement('i'); "
                         "function f2() { out.push('f2'); } "
                         "function f1() { out.push('f1'); t3.removeEventListener('r', f2); t3.addEventListener('r', f2); } "
                         "t3.addEventListener('r', f1); t3.addEventListener('r', f2); t3.dispatchEvent(new Event('r')); "
                         "out.join() === 'f1'"));
    EMB_CHECK(js_true(e, "var o2 = []; var t4 = document.createElement('i'); "
                         "t4.addEventListener('s', function () { o2.push('a'); t4.addEventListener('s', function () { o2.push('late'); }); }); "
                         "t4.dispatchEvent(new Event('s')); o2.join() === 'a'"));
    env_free(e);
}

typedef struct fake_timers {
    void *payload;
    double ms;
} fake_timers;

static uint32_t fake_add(void *user, double ms, bool repeat, void *payload)
{
    fake_timers *f = user;
    (void)repeat;
    f->payload = payload;
    f->ms = ms;
    return 1;
}

static void *fake_clear(void *user, uint32_t id)
{
    (void)user;
    (void)id;
    return nullptr;
}

EMB_TEST(bind_abort_signal_timeout_aborts_with_a_timeout_error)
{
    env e = env_new();
    fake_timers f = {nullptr, 0};
    embi_bind_timers hooks = {&f, fake_add, fake_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(js_true(e, "var ts = AbortSignal.timeout(25); var tfired = 0; ts.addEventListener('abort', function () { tfired++; }); "
                         "ts.aborted === false"));
    EMB_CHECK(f.payload != nullptr && f.ms == 25);
    embi_js_value r = embi_bind_fire_timer(e.c, f.payload);
    EMB_CHECK(r != EMBI_JS_EXCEPTION);
    embi_bind_release_timer(e.c, f.payload);
    EMB_CHECK(js_true(e, "ts.aborted === true && tfired === 1 && ts.reason.name === 'TimeoutError' && ts.reason.code === 23 && "
                         "ts.reason instanceof DOMException"));
    env_free(e);
}

/* Removed listeners are dropped from the deps of their signal. */
static size_t signal_growth(env e, bool remove)
{
    size_t base = heap_live(e);
    const char *src = remove ? "for (var i = 0; i < 300; i++) { var n = document.createElement('div'); "
                               "var f = function () {}; n.addEventListener('x', f, {signal: sig.signal}); "
                               "n.removeEventListener('x', f); } n = f = null; true"
                             : "for (var i = 0; i < 300; i++) { var n = document.createElement('div'); "
                               "var f = function () {}; n.addEventListener('x', f, {signal: sig.signal}); } n = f = null; true";
    EMB_CHECK(js_true(e, src));
    size_t grown = heap_live(e);
    return grown > base ? grown - base : 0;
}

EMB_TEST(bind_abort_signal_deps_do_not_retain_removed_listeners)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var sig = new AbortController(); true"));
    size_t removed = signal_growth(e, true);
    size_t kept = signal_growth(e, false);
    EMB_CHECK(removed * 4 < kept);
    EMB_CHECK(js_true(e, "sig.abort(); true"));
    env_free(e);
}

/* Ancestors without wrappers or content handlers are not wrapped by a dispatch. */
EMB_TEST(bind_event_dispatch_through_unwrapped_ancestors)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<div onb=\"seen.push(\\'attr\\')\"><section><i id=tt></i></section></div>'; "
                         "var seen = []; var tt = document.getElementById('tt'); "
                         "tt.addEventListener('b', function (ev) { seen.push('t:' + ev.composedPath().length); }); "
                         "tt.dispatchEvent(new Event('b', {bubbles: true})); seen.join() === 't:7,attr'"));
    EMB_CHECK(js_true(e, "var seen2 = []; document.body.innerHTML = '<div><section><i id=t2></i></section></div>'; "
                         "var t2 = document.getElementById('t2'); "
                         "t2.addEventListener('c', function () { seen2.push('t'); t2.parentNode.parentNode.addEventListener('c', function () { seen2.push('div'); }); }); "
                         "t2.dispatchEvent(new Event('c', {bubbles: true})); seen2.join() === 't,div'"));
    /* the target leaves the tree: the unwrapped ancestors are freed, the dispatch goes on */
    EMB_CHECK(js_true(e, "var seen3 = []; document.body.innerHTML = '<div><section><i id=t3></i></section></div>'; "
                         "var t3b = document.getElementById('t3'); document.body.addEventListener('d', function () { seen3.push('body'); }); "
                         "t3b.addEventListener('d', function () { seen3.push('t'); t3b.remove(); }); "
                         "t3b.dispatchEvent(new Event('d', {bubbles: true})); seen3.join() === 't,body'"));
    env_free(e);
}

static int g_pre, g_run, g_cancel;
static void act_pre(embi_bind_ctx *c, embi_node *el, embi_bind_obj *ev)
{
    (void)c;
    (void)el;
    (void)ev;
    g_pre++;
}
static void act_run(embi_bind_ctx *c, embi_node *el, embi_bind_obj *ev)
{
    (void)c;
    (void)el;
    (void)ev;
    g_run++;
}
static void act_cancel(embi_bind_ctx *c, embi_node *el, embi_bind_obj *ev)
{
    (void)c;
    (void)el;
    (void)ev;
    g_cancel++;
}

EMB_TEST(bind_event_activation_behaviors)
{
    static const embi_activation act = {nullptr, act_pre, act_run, act_cancel};
    g_pre = g_run = g_cancel = 0;
    EMB_CHECK(embi_bind_register_activation("button", &act));
    env e = env_new();
    EMB_CHECK(js_true(e, "var b = document.createElement('button'); document.body.appendChild(b); b.click(); true"));
    EMB_CHECK_EQ_INT(g_pre, 1);
    EMB_CHECK_EQ_INT(g_run, 1);
    EMB_CHECK_EQ_INT(g_cancel, 0);
    EMB_CHECK(js_true(e, "b.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true})); true"));
    EMB_CHECK_EQ_INT(g_run, 2);
    /* canceled: the cancel hook, not the behavior */
    EMB_CHECK(js_true(e, "b.addEventListener('click', function (ev) { ev.preventDefault(); }); b.click(); true"));
    EMB_CHECK_EQ_INT(g_pre, 3);
    EMB_CHECK_EQ_INT(g_run, 2);
    EMB_CHECK_EQ_INT(g_cancel, 1);
    /* a plain Event named click is not an activation event; neither is another type */
    EMB_CHECK(js_true(e, "var b2 = document.createElement('button'); document.body.appendChild(b2); "
                         "b2.dispatchEvent(new Event('click', {bubbles: true})); "
                         "b2.dispatchEvent(new MouseEvent('mousedown', {bubbles: true})); true"));
    EMB_CHECK_EQ_INT(g_pre, 3);
    /* the ancestor with a behavior gets a bubbling click on its descendant */
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<button id=ab><span id=as></span></button>'; "
                         "document.getElementById('as').click(); true"));
    EMB_CHECK_EQ_INT(g_run, 3);
    EMB_CHECK(js_true(e, "document.getElementById('as').dispatchEvent(new MouseEvent('click')); true"));
    EMB_CHECK_EQ_INT(g_run, 3);
    /* a script-set handler yields to a later attribute change */
    EMB_CHECK(js_true(e, "var h = document.createElement('i'); h.setAttribute('onclick', 'k = 1'); "
                         "h.onclick = function () { k = 2; }; var k = 0; h.click(); k === 2 && true"));
    EMB_CHECK(js_true(e, "h.setAttribute('onclick', 'k = 3'); h.click(); k === 3"));
    EMB_CHECK(js_true(e, "var re = new Event('x'); re.initEvent('y', false, false); re.target === null"));
    env_free(e);
    embi_bind_clear_activations();
}

EMB_TEST(bind_set_timeout_propagates_the_handler_string_conversion_exception)
{
    env e = env_new();
    fake_timers f = {nullptr, 0};
    embi_bind_timers hooks = {&f, fake_add, fake_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(js_true(e, "var caught = null; var boom = { toString() { throw new RangeError('conv') } };"
                         "try { setTimeout(boom, 0) } catch (x) { caught = x } caught instanceof RangeError && caught.message === 'conv'"));
    EMB_CHECK(js_true(e, "var c2 = null; try { setInterval(boom, 0) } catch (x) { c2 = x } c2 instanceof RangeError"));
    EMB_CHECK(js_true(e, "typeof setTimeout('1', 5) === 'number' && typeof setTimeout(function () {}, 5) === 'number'"));
    embi_bind_release_timer(e.c, f.payload);
    env_free(e);
}

EMB_TEST(bind_remove_event_listener_reads_only_the_capture_option)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var n = 0; var f = function () { n++ }; var el = document.createElement('div');"
                         "el.addEventListener('x', f, { capture: true }); el.removeEventListener('x', f, { once: true, passive: true });"
                         "el.dispatchEvent(new Event('x')); var after_wrong = n; el.removeEventListener('x', f, { capture: true });"
                         "el.dispatchEvent(new Event('x')); after_wrong === 1 && n === 1"));
    env_free(e);
}

/* CF1: a dispatch snapshots each matching registration with the slot it had and looks at that slot first, falling back to
 * a search by serial when the slot holds another registration. Removed and re-added during the dispatch: covered by
 * bind_event_removed_and_readded_listener_does_not_run_in_the_same_dispatch. Here: once listeners leave the list while
 * it runs (the later ones move down a slot), earlier slots are spliced out by a listener, an earlier listener is removed
 * and added again (a new registration at the end, after slots moved), and capture, passive and bubble listeners mixed on
 * the target and an ancestor. */
EMB_TEST(bind_event_dispatch_finds_registrations_that_moved)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var o = []; var t = document.createElement('i'); document.body.appendChild(t);"
                         "t.addEventListener('x', function () { o.push('a'); }, {once: true});"
                         "t.addEventListener('x', function () { o.push('b'); }, {once: true});"
                         "t.addEventListener('x', function () { o.push('c'); });"
                         "t.dispatchEvent(new Event('x')); t.dispatchEvent(new Event('x')); o.join() === 'a,b,c,c'"));
    EMB_CHECK(js_true(e, "var o2 = []; var t2 = document.createElement('i'); function a2() { o2.push('a'); }"
                         "function z2() { o2.push('z'); }"
                         "t2.addEventListener('y', z2); t2.addEventListener('other', z2); t2.addEventListener('y', a2);"
                         "t2.addEventListener('y', function () { o2.push('b'); t2.removeEventListener('other', z2);"
                         "  t2.removeEventListener('y', a2); t2.removeEventListener('y', z2); });"
                         "t2.addEventListener('y', function () { o2.push('c'); });"
                         "t2.addEventListener('y', function () { o2.push('d'); });"
                         "t2.dispatchEvent(new Event('y')); t2.dispatchEvent(new Event('y')); o2.join() === 'z,a,b,c,d,b,c,d'"));
    EMB_CHECK(js_true(e, "var o3 = []; var t3 = document.createElement('i'); function a3() { o3.push('a'); }"
                         "t3.addEventListener('w', a3);"
                         "t3.addEventListener('w', function () { o3.push('b'); t3.removeEventListener('w', a3);"
                         "  t3.addEventListener('w', a3); });"
                         "t3.addEventListener('w', function () { o3.push('c'); });"
                         "t3.dispatchEvent(new Event('w')); t3.dispatchEvent(new Event('w')); o3.join() === 'a,b,c,b,c'"));
    EMB_CHECK(js_true(e, "var o4 = []; var p4 = document.createElement('div'); var t4 = document.createElement('i');"
                         "p4.appendChild(t4); document.body.appendChild(p4);"
                         "function rec(tag) { return function (ev) { ev.preventDefault(); o4.push(tag + ev.eventPhase +"
                         "  (ev.defaultPrevented ? '!' : '')); }; }"
                         "p4.addEventListener('v', rec('pb'), {passive: true});"
                         "p4.addEventListener('v', rec('pc'), {capture: true, passive: true});"
                         "t4.addEventListener('v', rec('tb'), {passive: true, once: true});"
                         "t4.addEventListener('v', rec('tc'), {capture: true, once: true});"
                         "t4.addEventListener('v', rec('tb2'), {passive: true});"
                         "var r4 = t4.dispatchEvent(new Event('v', {bubbles: true, cancelable: true}));"
                         "var first = o4.join(); o4 = [];"
                         "var r5 = t4.dispatchEvent(new Event('v', {bubbles: true, cancelable: true}));"
                         "first === 'pc1,tc2!,tb2!,tb22!,pb3!' && r4 === false && o4.join() === 'pc1,tb22,pb3' && r5 === true"));
    env_free(e);
}

EMB_TEST_MAIN()
