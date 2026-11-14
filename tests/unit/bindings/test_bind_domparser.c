/*
 * DOMParser (W5f, src/bindings/impl_domparser.c): text/html through the HTML parser, the XML types through the small XML
 * parser (elements, namespaces, entities, character references, CDATA, comments, processing instructions, DOCTYPE), the
 * <parsererror> document for malformed input, argument errors, and the release of the parsed documents.
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

EMB_TEST(domparser_text_html)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = new DOMParser();"
                         "var d = p.parseFromString('<!doctype html><title>T</title><p class=x>a<b>c</p>', 'text/html');"
                         "d.contentType === 'text/html' && d.title === 'T' && d.body.firstChild.className === 'x' && "
                         "d.body.querySelector('b').textContent === 'c' && d.doctype.name === 'html' && d !== document && d.location === null"));
    /* scripts of the parsed document do not run and the parsed document is not the main one */
    EMB_CHECK(js_true(e, "var ran = false; var d2 = p.parseFromString('<script>ran = true<\\/script><noscript>n</noscript>', 'text/html');"
                         "!ran && d2.querySelector('script') !== null"));
    EMB_CHECK(js_true(e, "var d3 = p.parseFromString('', 'text/html'); d3.documentElement.localName === 'html' && !!d3.head && !!d3.body"));
    env_free(e);
}

EMB_TEST(domparser_xml_well_formed)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = new DOMParser();"
                         "var x = p.parseFromString('<?xml version=\"1.0\"?><!-- c --><r xmlns=\"urn:a\" xmlns:q=\"urn:q\" a=\"1&amp;2\"><q:k q:z=\"y\">t&#65;&#x42;&lt;</q:k><![CDATA[<raw>]]><?pi data?><e/></r>', 'application/xml');"
                         "var r = x.documentElement; x.contentType === 'application/xml' && r.localName === 'r' && r.namespaceURI === 'urn:a' && "
                         "r.getAttribute('a') === '1&2' && r.firstChild.prefix === 'q' && r.firstChild.namespaceURI === 'urn:q' && "
                         "r.firstChild.textContent === 'tAB<' && r.firstChild.getAttributeNS('urn:q', 'z') === 'y'"));
    EMB_CHECK(js_true(e, "r.childNodes[1].nodeType === 4 && r.childNodes[1].data === '<raw>' && r.childNodes[2].nodeType === 7 && "
                         "r.childNodes[2].target === 'pi' && r.childNodes[3].localName === 'e' && x.firstChild.nodeType === 8"));
    EMB_CHECK(js_true(e, "var s = p.parseFromString('<svg xmlns=\"http://www.w3.org/2000/svg\"/>', 'image/svg+xml'); s.contentType === 'image/svg+xml'"));
    EMB_CHECK(js_true(e, "var h = p.parseFromString('<html xmlns=\"http://www.w3.org/1999/xhtml\"><body/></html>', 'application/xhtml+xml');"
                         "h.contentType === 'application/xhtml+xml' && h.documentElement.namespaceURI === 'http://www.w3.org/1999/xhtml'"));
    EMB_CHECK(js_true(e, "var dt = p.parseFromString('<!DOCTYPE r PUBLIC \"pub\" \"sys\"><r/>', 'text/xml'); dt.doctype.name === 'r' && "
                         "dt.doctype.publicId === 'pub' && dt.doctype.systemId === 'sys'"));
    env_free(e);
}

EMB_TEST(domparser_xml_errors_make_a_parsererror_document)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = new DOMParser(); var bad = ['<a>', '<a></b>', '<a b=1/>', '<a b=\"1\" b=\"2\"/>', '<a>&nope;</a>', '', '<a/><b/>', '<a><!-- -- --></a>', '<1/>', '<a xmlns:p=\"u\"><q:b/></a>'];"
                         "bad.every(s => { var d = p.parseFromString(s, 'application/xml'); var pe = d.getElementsByTagName('parsererror')[0];"
                         "  return d.documentElement.localName === 'parsererror' || !!pe })"));
    EMB_CHECK(js_true(e, "p.parseFromString('<a>', 'application/xml').documentElement.namespaceURI === 'http://www.mozilla.org/newlayout/xml/parsererror.xml'"));
    env_free(e);
}

EMB_TEST(domparser_argument_errors)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = new DOMParser(); function nm(f) { try { f(); return 'ok' } catch (x) { return x.constructor.name } }"
                         "nm(() => p.parseFromString('<a/>', 'text/plain')) === 'TypeError' && nm(() => p.parseFromString('<a/>')) === 'TypeError' && "
                         "nm(() => p.parseFromString('<a/>', 'TEXT/HTML')) === 'TypeError' && nm(() => DOMParser()) === 'TypeError'"));
    EMB_CHECK(js_true(e, "typeof p.parseFromString === 'function' && Object.prototype.toString.call(p) === '[object DOMParser]'"));
    env_free(e);
}

static void parse_cycle(void)
{
    env e = env_new();
    (void)js_true(e, "var p = new DOMParser(); for (let i = 0; i < 6; i++) {"
                     "  var a = p.parseFromString('<p>x<b>y</b></p>', 'text/html');"
                     "  var b = p.parseFromString('<r a=\"1\"><k/><![CDATA[z]]></r>', 'application/xml');"
                     "  var c = p.parseFromString('<r>', 'application/xml'); a.body.appendChild(a.createElement('i')); document.body.appendChild(document.importNode(b.documentElement, true)); } true");
    env_free(e);
}

EMB_TEST(domparser_documents_are_freed_with_the_context)
{
    parse_cycle(); /* warm-up: interns the atoms and sizes the runtime's tables */
    size_t base = embi_mem_live();
    parse_cycle();
    parse_cycle();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(domparser_deep_nesting_and_many_attributes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var p = new DOMParser(); var deep = '<a>'.repeat(600) + '</a>'.repeat(600);"
                         "var dd = p.parseFromString(deep, 'application/xml'); dd.documentElement.localName === 'parsererror'"));
    EMB_CHECK(js_true(e, "var ok200 = p.parseFromString('<a>'.repeat(200) + '</a>'.repeat(200), 'application/xml'); ok200.documentElement.localName === 'a' && "
                         "ok200.getElementsByTagName('a').length === 200"));
    EMB_CHECK(js_true(e, "var at = ''; for (var i = 0; i < 100; i++) at += ' k' + i + '=\"v' + i + '\"';"
                         "var many = p.parseFromString('<r' + at + '/>', 'application/xml').documentElement;"
                         "many.localName === 'r' && many.attributes.length === 100 && many.getAttribute('k99') === 'v99' && many.getAttribute('k0') === 'v0'"));
    env_free(e);
}

static void doc_loop(env e, int n)
{
    char src[768];
    SDL_snprintf(src, sizeof src,
                 "for (let i = 0; i < %d; i++) { var q = new DOMParser().parseFromString('<p>x<b>y</b></p>', 'text/html');"
                 "  q.body.appendChild(q.createElement('i')); var w = q.body.firstChild; var r2 = document.implementation.createHTMLDocument('t');"
                 "  var x = new DOMParser().parseFromString('<r a=\"1\"><k/></r>', 'application/xml'); } q = w = r2 = x = null; true", n);
    (void)js_true(e, src);
}

EMB_TEST(domparser_script_made_documents_are_collected)
{
    env e = env_new();
    doc_loop(e, 20); /* warm-up: interns the atoms and sizes the tables */
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    size_t base = embi_mem_live();
    doc_loop(e, 3000);
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(embi_mem_live() < base + 512u * 1024u);
    EMB_CHECK(e.c->nxdocs < 100);
    env_free(e);
}

EMB_TEST(domparser_a_document_reachable_through_a_node_survives_collection)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var keep = (function () { var d = new DOMParser().parseFromString('<div id=k><p>hi</p></div>', 'text/html');"
                         "  var ni = d.createNodeIterator(d.body, 1); ni.nextNode(); var t = d.createElement('x'); t.id = 'detached'; return [d.getElementById('k'), t, ni, d.implementation]; })();"
                         "var junk = []; for (let i = 0; i < 200; i++) junk.push(new DOMParser().parseFromString('<p>' + i, 'text/html')); junk = null; true"));
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(js_true(e, "var d2 = keep[0].ownerDocument; d2.body.querySelector('p').textContent === 'hi' && keep[0].parentNode === d2.body && "
                         "keep[1].ownerDocument === d2 && keep[1].id === 'detached' && keep[2].nextNode().localName === 'div' && "
                         "keep[3].createDocument(null, 'z').documentElement.localName === 'z'"));
    /* the document dies once its last node is dropped, and nothing breaks */
    EMB_CHECK(js_true(e, "keep = null; d2 = null; true"));
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    EMB_CHECK(e.c->nxdocs == 0);
    env_free(e);
}

EMB_TEST_MAIN()
