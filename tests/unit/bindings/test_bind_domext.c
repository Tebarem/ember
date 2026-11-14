/*
 * W5f DOM additions seen from JS: documents made by script (createDocument, createHTMLDocument, new Document, clones),
 * CDATASection, name validation, moveBefore, named window properties, window.event, default passive listeners, optional
 * arguments with defaults, Attr nodes, supported property names and the unscopables. Every test builds its own document and
 * binding context; the ones that make documents check that embi_mem_live returns to its baseline after embi_bind_free.
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

/* Runs src (a script that makes a few documents and nodes), frees everything and compares the live memory. */
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

EMB_TEST(domext_documents_made_by_script)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var d = document.implementation.createHTMLDocument('t');"
                         "d.title === 't' && d.body && d.doctype.name === 'html' && d.location === null"));
    EMB_CHECK(js_true(e, "var x = document.implementation.createDocument('urn:x', 'a:b', null);"
                         "x.documentElement.localName === 'b' && x.documentElement.prefix === 'a' && "
                         "Object.getPrototypeOf(x) === XMLDocument.prototype && x.contentType === 'application/xml'"));
    EMB_CHECK(js_true(e, "var n = new Document(); n.childNodes.length === 0 && n.URL === 'about:blank'"));
    EMB_CHECK(js_true(e, "document.location === document.location && String(document.location) === String(window.location)"));
    env_free(e);
}

EMB_TEST(domext_clone_of_a_document)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var c = document.cloneNode(true); c !== document && c.documentElement.localName === 'html' && "
                         "c.documentElement !== document.documentElement && c.contentType === document.contentType"));
    EMB_CHECK(js_true(e, "var s = document.cloneNode(false); s.childNodes.length === 0 && s.ownerDocument === null"));
    EMB_CHECK(js_true(e, "var x = document.implementation.createDocument(null, 'r', null).cloneNode(true);"
                         "x.documentElement.localName === 'r' && x instanceof Document"));
    env_free(e);
}

EMB_TEST(domext_extra_documents_are_freed_with_the_context)
{
    check_returns_to_baseline("for (let i = 0; i < 4; i++) {"
                              "  var a = document.implementation.createHTMLDocument('x');"
                              "  var b = document.implementation.createDocument(null, 'r', null);"
                              "  var c = document.cloneNode(true); var d = new Document();"
                              "  a.body.appendChild(a.createElement('p'));"
                              "  b.documentElement.setAttribute('k', 'v'); d.appendChild(d.createElement('z'));"
                              "}");
}

EMB_TEST(domext_cdata_section)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = false; try { document.createCDATASection('x') } catch (x) { t = x.name === 'NotSupportedError' } t"));
    EMB_CHECK(js_true(e, "var xd = new Document(); var cd = xd.createCDATASection('a<b');"
                         "cd.nodeType === 4 && cd.nodeName === '#cdata-section' && cd.data === 'a<b' && "
                         "cd instanceof CDATASection && cd instanceof Text"));
    EMB_CHECK(js_true(e, "var bad = false; try { xd.createCDATASection('a]]>b') } catch (x) { bad = x.name === 'InvalidCharacterError' } bad"));
    /* normalize() leaves a CDATASection alone and does not merge across it */
    EMB_CHECK(js_true(e, "var r = xd.createElement('r'); xd.appendChild(r);"
                         "r.append(xd.createTextNode('a'), xd.createCDATASection(''), xd.createTextNode('b'));"
                         "r.normalize(); r.childNodes.length === 3 && r.childNodes[1].nodeType === 4"));
    EMB_CHECK(js_true(e, "var k = r.cloneNode(true); k.childNodes[1].nodeType === 4"));
    env_free(e);
}

EMB_TEST(domext_name_validation)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "function nm(f) { try { f(); return 'ok' } catch (x) { return x.name } }"
                         "nm(() => document.createElement('1a')) === 'InvalidCharacterError' && "
                         "nm(() => document.createElement('a b')) === 'InvalidCharacterError' && "
                         "nm(() => document.createElement('a:b')) === 'ok' && nm(() => document.createElement('\\u00e9')) === 'ok'"));
    EMB_CHECK(js_true(e, "nm(() => document.createElementNS(null, 'a:b')) === 'NamespaceError' && "
                         "nm(() => document.createElementNS('x', 'xml:a')) === 'NamespaceError' && "
                         "nm(() => document.createElementNS('http://www.w3.org/2000/xmlns/', 'a')) === 'NamespaceError' && "
                         "nm(() => document.createElementNS('x', 'xmlns')) === 'NamespaceError' && "
                         "nm(() => document.createElementNS('x', 'p:')) === 'InvalidCharacterError'"));
    EMB_CHECK(js_true(e, "nm(() => document.createAttribute('')) === 'InvalidCharacterError' && "
                         "document.createAttribute('FOO').name === 'foo' && "
                         "nm(() => document.createProcessingInstruction('a', 'b?>c')) === 'InvalidCharacterError'"));
    env_free(e);
}

EMB_TEST(domext_attr_nodes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = document.createElement('p'); p.setAttribute('title', 'one');"
                         "var a = p.getAttributeNode('title'); a.ownerElement === p && a.value === 'one' && "
                         "a.nodeType === 2 && p.getAttributeNode('title') === a"));
    EMB_CHECK(js_true(e, "a.value = 'two'; p.getAttribute('title') === 'two' && p.attributes[0] === a"));
    EMB_CHECK(js_true(e, "p.removeAttribute('title'); a.ownerElement === null && a.value === 'two' && p.attributes.length === 0"));
    EMB_CHECK(js_true(e, "var q = document.createElement('q'); var old = q.setAttributeNode(a);"
                         "old === null && a.ownerElement === q && q.title === 'two'"));
    EMB_CHECK(js_true(e, "var inuse = false; try { p.setAttributeNode(a) } catch (x) { inuse = x.name === 'InUseAttributeError' } inuse"));
    EMB_CHECK(js_true(e, "var b = document.createAttribute('title'); b.value = 'three'; q.setAttributeNode(b) === a && "
                         "a.ownerElement === null && q.title === 'three'"));
    env_free(e);
}

EMB_TEST(domext_move_before)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var a = document.createElement('div'), b = document.createElement('div');"
                         "var x = document.createElement('i'), y = document.createElement('i'), z = document.createElement('i');"
                         "document.body.append(a, b); a.append(x, y); b.append(z);"
                         "b.moveBefore(x, z); x.parentNode === b && b.firstChild === x && b.lastChild === z && a.firstChild === y"));
    EMB_CHECK(js_true(e, "var rec = []; var mo = new MutationObserver(r => rec.push(...r)); mo.observe(document.body, {childList: true, subtree: true});"
                         "b.moveBefore(y, null); var r2 = mo.takeRecords(); b.lastChild === y && r2.length >= 1"));
    EMB_CHECK(js_true(e, "var bad = false; try { a.moveBefore(document.body, null) } catch (t) { bad = t.name === 'HierarchyRequestError' } bad"));
    EMB_CHECK(js_true(e, "var d2 = document.implementation.createHTMLDocument(''); var other = d2.createElement('p');"
                         "var cross = false; try { a.moveBefore(other, null) } catch (t) { cross = t.name === 'HierarchyRequestError' } cross"));
    env_free(e);
}

EMB_TEST(domext_window_named_properties)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var el = document.createElement('div'); el.id = 'target'; document.body.appendChild(el);"
                         "target === el && window.target === el && !Object.prototype.hasOwnProperty.call(window, 'target')"));
    /* a var or an assignment shadows the element and survives the id changing */
    EMB_CHECK(js_true(e, "var target = el; el.id = 'other'; target === el"));
    EMB_CHECK(js_true(e, "var gone = false; try { missing_name_xyz } catch (x) { gone = x instanceof ReferenceError } gone"));
    /* members of the Window object win over an id of the same name */
    EMB_CHECK(js_true(e, "var el2 = document.createElement('i'); el2.id = 'location'; document.body.appendChild(el2);"
                         "window.location !== el2 && typeof location.href === 'string'"));
    env_free(e);
}

EMB_TEST(domext_window_event_and_default_passive)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var seen, during, after; var t = new EventTarget(); var ev = new Event('x');"
                         "window.event === undefined && (t.addEventListener('x', () => { during = window.event }), t.dispatchEvent(ev), "
                         "during === ev && window.event === undefined)"));
    EMB_CHECK(js_true(e, "var inner = new Event('in'), outer = new Event('out'), log = [];"
                         "t.addEventListener('in', () => log.push(window.event === inner));"
                         "t.addEventListener('out', () => { log.push(window.event === outer); t.dispatchEvent(inner); log.push(window.event === outer) });"
                         "t.dispatchEvent(outer); log.join() === 'true,true,true'"));
    /* touchstart, touchmove, wheel on window, document, the document element and body are passive by default */
    EMB_CHECK(js_true(e, "function prevented(target, opt) { var pd = null; var h = ev => { ev.preventDefault(); pd = ev.defaultPrevented; target.removeEventListener('wheel', h) };"
                         "opt === undefined ? target.addEventListener('wheel', h) : target.addEventListener('wheel', h, opt);"
                         "target.dispatchEvent(new Event('wheel', {cancelable: true})); return pd }"
                         "prevented(window) === false && prevented(document) === false && prevented(document.body) === false && "
                         "prevented(window, {passive: false}) === true && prevented(document.createElement('div')) === true"));
    env_free(e);
}

EMB_TEST(domext_optional_arguments_with_defaults_take_undefined)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "new Comment(undefined).data === '' && new Text(undefined).data === '' && new Text(null).data === 'null'"));
    EMB_CHECK(js_true(e, "document.createTreeWalker(document, undefined).whatToShow === 0xFFFFFFFF && NodeFilter.SHOW_ALL === 4294967295"));
    env_free(e);
}

EMB_TEST(domext_supported_property_names)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<p id=a></p><p name=b></p><p id=a name=c></p><p></p>';"
                         "var c = document.getElementsByTagName('p');"
                         "Object.getOwnPropertyNames(c).join() === '0,1,2,3,a,b,c'"));
    EMB_CHECK(js_true(e, "var el = document.createElement('div'); el.setAttribute('x', ''); el.setAttribute('Y', '');"
                         "Object.getOwnPropertyNames(el.attributes).join() === '0,1,x,y'"));
    EMB_CHECK(js_true(e, "var s = document.body.children; typeof s[Symbol.iterator] === 'function' && s.values === undefined && "
                         "s.forEach === undefined && s.entries === undefined && s.keys === undefined"));
    env_free(e);
}

EMB_TEST(domext_unscopables_and_console_prototype)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var u = Element.prototype[Symbol.unscopables]; u.remove === true && u.prepend === true && "
                         "Document.prototype[Symbol.unscopables].append === true && DocumentFragment.prototype[Symbol.unscopables].replaceChildren === true"));
    EMB_CHECK(js_true(e, "var p1 = Object.getPrototypeOf(console); Object.getOwnPropertyNames(p1).length === 0 && "
                         "Object.getPrototypeOf(p1) === Object.prototype"));
    env_free(e);
}

EMB_TEST(domext_abort_any_fires_in_creation_order)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var ctl = new AbortController(); var sigs = [ctl.signal];"
                         "sigs.push(AbortSignal.any([ctl.signal])); sigs.push(AbortSignal.any([ctl.signal]));"
                         "sigs.push(AbortSignal.any([sigs[0]])); sigs.push(AbortSignal.any([sigs[1]]));"
                         "var out = ''; sigs.forEach((s, i) => s.addEventListener('abort', () => { out += i })); ctl.abort(); out === '01234'"));
    env_free(e);
}

EMB_TEST(domext_create_thead_skips_non_elements_and_caption_colgroup)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = document.createElement('table'); t.innerHTML = ' <caption>c</caption> <colgroup></colgroup> <tbody></tbody>';"
                         "var h = t.createTHead(); h.localName === 'thead' && h.nextElementSibling.localName === 'tbody' && "
                         "h.previousElementSibling.localName === 'colgroup' && t.createTHead() === h && t.tHead === h"));
    EMB_CHECK(js_true(e, "var t2 = document.createElement('table'); t2.appendChild(document.createTextNode(' ')); t2.appendChild(document.createElement('tbody'));"
                         "var h2 = t2.createTHead(); t2.children[0] === h2 && t2.children[1].localName === 'tbody' && t2.firstChild.nodeType === 3"));
    EMB_CHECK(js_true(e, "var t3 = document.createElement('table'); t3.appendChild(document.createElement('caption')); t3.appendChild(document.createTextNode('x'));"
                         "var h3 = t3.createTHead(); t3.lastChild === h3 && t3.children[0].localName === 'caption'"));
    env_free(e);
}

EMB_TEST_MAIN()
