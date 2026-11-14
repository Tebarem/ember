/*
 * Binding tests (W5a): the generated DOM, Window, console and CSSOM glue, exercised from JS against
 * a real document. Every test builds a fresh document and binding context and checks results
 * with JS expressions that must evaluate to true.
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

/* The binding context goes first, then the runtime, then the document (see bind.h). */
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

/* Evaluates src and returns true when it yields the boolean true. Logs the source otherwise. */
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

/* Evaluates src and reports whether it threw (the exception is cleared). */
static bool js_throws(env e, const char *src)
{
    embi_js_value v = embi_js_eval(realm(e), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
        return true;
    }
    return false;
}

EMB_TEST(bind_window_globals_are_the_realm_global)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "window === globalThis && self === window"));
    EMB_CHECK(js_true(e, "window.document === document && document.defaultView === window"));
    EMB_CHECK(js_true(e, "typeof console.log === 'function' && typeof Window === 'function'"));
    EMB_CHECK(js_true(e, "typeof window.innerWidth === 'number' && window.devicePixelRatio === 1"));
    env_free(e);
}

EMB_TEST(bind_document_structure_and_wrapper_identity)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.documentElement.tagName === 'HTML'"));
    EMB_CHECK(js_true(e, "document.body.tagName === 'BODY'"));
    EMB_CHECK(js_true(e, "document.body === document.body"));
    EMB_CHECK(js_true(e, "document.body.parentNode === document.documentElement"));
    EMB_CHECK(js_true(e, "document.body.ownerDocument === document && document.ownerDocument === null"));
    EMB_CHECK(js_true(e, "document.body.nodeName === 'BODY' && document.nodeName === '#document'"));
    EMB_CHECK(js_true(e, "document.isConnected === true"));
    env_free(e);
}

EMB_TEST(bind_create_append_and_find_by_id)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var d = document.createElement('div'); d.id = 'x';"
                      "document.body.appendChild(d);"
                      "document.getElementById('x') === d && d.parentNode === document.body"));
    EMB_CHECK(js_true(e, "document.body.lastChild === document.getElementById('x')"));
    EMB_CHECK(js_true(e, "document.body.hasChildNodes() === true"));
    EMB_CHECK(js_true(e, "document.body.contains(document.getElementById('x'))"));
    EMB_CHECK(js_true(e, "document.body.contains(null) === false"));
    env_free(e);
}

EMB_TEST(bind_text_content_round_trip)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var p = document.createElement('p'); p.textContent = 'a<b\\u00e9\\u4e2d';"
                      "p.textContent === 'a<b\\u00e9\\u4e2d' && p.firstChild.nodeType === 3"));
    EMB_CHECK(js_true(e,
                      "var t = document.createTextNode('hey'); t.data === 'hey' && t.length === 3"));
    EMB_CHECK(js_true(e, "t.substringData(1, 2) === 'ey' && t.substringData(2, 99) === 'y'"));
    EMB_CHECK(js_throws(e, "t.substringData(9, 1)"));
    EMB_CHECK(js_true(e, "t.data = 'bye'; t.data === 'bye' && t.length === 3"));
    EMB_CHECK(js_throws(e, "t.substringData(1)"));
    env_free(e);
}

EMB_TEST(bind_clone_and_remove)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var d = document.createElement('div');"
                      "d.appendChild(document.createTextNode('q'));"
                      "var c = d.cloneNode(true);"
                      "c !== d && c.textContent === 'q' && d.cloneNode().firstChild === null"));
    EMB_CHECK(js_true(e,
                      "document.body.appendChild(d); document.body.removeChild(d);"
                      "d.parentNode === null"));
    EMB_CHECK(js_true(e,
                      "var lost = false; try { document.body.removeChild(document.createElement('p')); }"
                      "catch (x) { lost = x.name === 'NotFoundError' && x.code === 8; } lost"));
    env_free(e);
}

EMB_TEST(bind_attributes_and_reflection)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var d = document.createElement('div');"
                      "d.setAttribute('Data-X', 'v');"
                      "d.getAttribute('data-x') === 'v' && d.hasAttribute('DATA-X') === true"));
    EMB_CHECK(js_true(e, "d.getAttribute('missing') === null"));
    EMB_CHECK(js_true(e, "d.removeAttribute('data-x'); d.getAttribute('data-x') === null"));
    EMB_CHECK(js_true(e, "d.title = 't'; d.getAttribute('title') === 't' && d.title === 't'"));
    EMB_CHECK(js_true(e, "d.hidden = true; d.hasAttribute('hidden') && d.hidden === true"));
    EMB_CHECK(js_true(e, "d.hidden = false; !d.hasAttribute('hidden') && d.hidden === false"));
    EMB_CHECK(js_true(e, "d.className = 'a b'; d.className === 'a b' && d.getAttribute('class') === 'a b'"));
    EMB_CHECK(js_true(e,
                      "var a = document.createElement('a'); a.href = 'http://x/';"
                      "a instanceof HTMLAnchorElement && a instanceof HTMLElement && a instanceof Element"
                      " && a.href === 'http://x/' && a.tagName === 'A'"));
    env_free(e);
}

EMB_TEST(bind_dom_exceptions_carry_name_and_code)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var bad = false; try { document.createElement('1bad'); }"
                      "catch (x) { bad = x instanceof DOMException && x.name === 'InvalidCharacterError'"
                      " && x.code === 5 && typeof x.message === 'string'; } bad"));
    EMB_CHECK(js_true(e, "DOMException.NOT_FOUND_ERR === 8 && DOMException.INDEX_SIZE_ERR === 1"));
    EMB_CHECK(js_true(e,
                      "var dx = new DOMException('boom', 'AbortError');"
                      "dx.name === 'AbortError' && dx.message === 'boom' && dx.code === 20"));
    env_free(e);
}

EMB_TEST(bind_argument_checks_throw_type_errors)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var r = false; try { document.body.appendChild(); }"
                         "catch (x) { r = x instanceof TypeError; } r"));
    EMB_CHECK(js_true(e, "var r2 = false; try { document.body.appendChild({}); }"
                         "catch (x) { r2 = x instanceof TypeError && x.message.indexOf('Node') >= 0; } r2"));
    EMB_CHECK(js_true(e, "var r3 = false; try { Node.prototype.appendChild.call({}, null); }"
                         "catch (x) { r3 = x instanceof TypeError; } r3"));
    EMB_CHECK(js_true(e, "Node.TEXT_NODE === 3 && Node.DOCUMENT_FRAGMENT_NODE === 11"));
    env_free(e);
}

EMB_TEST(bind_inline_style_through_css_declaration)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
                      "var d = document.createElement('div');"
                      "d.style.setProperty('color', 'red'); d.style.setProperty('margin', '0');"
                      "d.getAttribute('style') === 'color: red; margin: 0;'"));
    EMB_CHECK(js_true(e, "d.style === d.style && d.style.getPropertyValue('COLOR') === 'red'"));
    EMB_CHECK(js_true(e, "d.style.removeProperty('color') === 'red' && d.getAttribute('style') === 'margin: 0;'"));
    EMB_CHECK(js_true(e, "d.style.cssText = 'top: 1px'; d.style.cssText === 'top: 1px;' && d.style.getPropertyValue('top') === '1px'"));
    EMB_CHECK(js_true(e, "d.style.setProperty('top', ''); d.getAttribute('style') === ''"));
    env_free(e);
}

EMB_TEST(bind_internal_longhands_are_not_author_visible)
{
    /* -internal-legacy-box marks a display:-webkit-box flex container for its items (props.txt: -internal-*
     * longhands have an id and a value but no name). No script or declaration can see or set it. */
    env e = env_new();
    EMB_CHECK(js_true(e, "var d = document.createElement('div'); d.style.setProperty('-internal-legacy-box', 'yes');"
                         "d.style.getPropertyValue('-internal-legacy-box') === '' && d.style.length === 0"
                         " && d.getAttribute('style') === null"));
    EMB_CHECK(js_true(e, "d.style.cssText = '-internal-legacy-box: yes; top: 1px'; d.style.cssText === 'top: 1px;'"));
    EMB_CHECK(js_true(e, "CSS.supports('-internal-legacy-box', 'yes') === false && CSS.supports('-internal-legacy-box: yes') === false"
                         " && CSS.supports('top', '1px') === true"));
    EMB_CHECK(js_true(e, "!('internalLegacyBox' in d.style) && !('-internal-legacy-box' in d.style)"
                         " && !Object.getOwnPropertyNames(CSSStyleDeclaration.prototype).some(function (n) { return /internal/i.test(n); })"));
    EMB_CHECK(js_true(e, "var cs = getComputedStyle(document.body), found = false;"
                         "for (var i = 0; i < cs.length; i++) { if (/internal/i.test(cs.item(i))) found = true; }"
                         "!found && cs.length > 100 && cs.getPropertyValue('-internal-legacy-box') === ''"));
    env_free(e);
}

EMB_TEST(bind_navigator_and_console)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "navigator.userAgent.indexOf('Ember/') === 0 && navigator.onLine === true"));
    EMB_CHECK(js_true(e, "navigator === navigator && navigator.language === 'en-US'"));
    EMB_CHECK(js_true(e, "console.log('x', 1, {}, null) === undefined"));
    EMB_CHECK(js_true(e, "console.assert(true, 'no') === undefined && console.count('c') === undefined"));
    EMB_CHECK(js_true(e, "console.warn(); console.error(undefined); console.table([1]) === undefined"));
    env_free(e);
}

/* Builds a fixture of many wrapped nodes and runs it, then releases everything. */
static void churn_cycle(void)
{
    env e = env_new();
    (void)embi_js_eval(realm(e),
                       "for (var i = 0; i < 200; i++) {"
                       "  var d = document.createElement('div');"
                       "  d.setAttribute('id', 'n');"
                       "  d.style.setProperty('top', '1px');"
                       "  d.textContent = 'x';"
                       "  document.body.appendChild(d);"
                       "}"
                       "document.body.removeChild(document.body.firstChild);",
                       0, "churn.js", 0);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

EMB_TEST(bind_memory_returns_to_baseline_after_release)
{
    churn_cycle(); /* warm-up: interns the atoms and sizes the runtime's tables */
    size_t base = embi_mem_live();
    churn_cycle();
    churn_cycle();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(bind_free_before_document_detaches_node_wrappers)
{
    /* A wrapper that outlives the context reads as a stale object (no native behaviour), and the
     * document can be freed afterwards without touching it. */
    env e = env_new();
    embi_js_value v = embi_js_eval(realm(e), "document.body", 13, "t.js", 0);
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_document_free(e.doc);
}

EMB_TEST_MAIN()
