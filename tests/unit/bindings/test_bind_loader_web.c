/*
 * Web platform bindings of the loader (W7c): URL and URLSearchParams (linked and standalone), TextEncoder and
 * TextDecoder (BOM, fatal, streaming, labels), atob and btoa, crypto.getRandomValues and randomUUID (with their
 * DOMException names), and structuredClone. Each case is a JS expression that must be true.
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

static void env_init(env *e)
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
}

static void env_done(env *e)
{
    embi_bind_free(e->c);
    embi_js_runtime_free(e->rt);
    embi_document_free(e->doc);
}

/* Evaluates src; true when it yields the boolean true. A thrown exception is reported and counts as false. */
static bool js_true(env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "loader.js", 0);
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

EMB_TEST(bind_url_parts_and_setters)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var u = new URL('https://user:pw@example.com:8080/a/b?x=1&y=2#frag');"
                          "u.hostname === 'example.com' && u.port === '8080' && u.pathname === '/a/b' &&"
                          "u.search === '?x=1&y=2' && u.hash === '#frag' && u.origin === 'https://example.com:8080' &&"
                          "u.username === 'user' && u.protocol === 'https:' && String(u) === u.href"));
    EMB_CHECK(js_true(&e, "new URL('../c?q', 'https://h.test/a/b/d').href === 'https://h.test/a/c?q'"));
    EMB_CHECK(js_true(&e, "var v = new URL('http://a.test/'); v.pathname = '/x y'; v.pathname === '/x%20y'"));
    EMB_CHECK(js_true(&e, "var w = new URL('http://a.test/'); w.search = 'k=v'; w.search === '?k=v' && w.href === 'http://a.test/?k=v'"));
    EMB_CHECK(js_true(&e, "URL.canParse('http://x') && !URL.canParse('nope') && URL.parse('nope') === null"));
    EMB_CHECK(js_true(&e, "(function(){ try { new URL('nope'); return false; } catch (err) { return err instanceof TypeError; } })()"));
    env_done(&e);
}

EMB_TEST(bind_url_search_params_linked_and_standalone)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var w = new URL('http://a.test/?a=1'); w.searchParams.append('b', '2'); w.search === '?a=1&b=2'"));
    EMB_CHECK(js_true(&e, "var w2 = new URL('http://a.test/?a=1'); w2.searchParams === w2.searchParams"));
    EMB_CHECK(js_true(&e, "var w3 = new URL('http://a.test/?a=1'); var sp = w3.searchParams; w3.search = 'z=9'; sp.get('z') === '9'"));
    EMB_CHECK(js_true(&e, "new URLSearchParams('a=1&a=2&b=%20x').getAll('a').join(',') === '1,2' && new URLSearchParams('b=%20x').get('b') === ' x'"));
    EMB_CHECK(js_true(&e, "var p = new URLSearchParams('c=3&a=1&b=2&a=9'); p.sort(); p.toString() === 'a=1&a=9&b=2&c=3'"));
    EMB_CHECK(js_true(&e, "var q = new URLSearchParams('a=1&b=2&a=3'); q.delete('a'); q.has('a') === false && q.size === 1 && q.has('b', '2')"));
    EMB_CHECK(js_true(&e, "var r = new URLSearchParams('a=1&a=2'); r.set('a', '7'); r.toString() === 'a=7'"));
    EMB_CHECK(js_true(&e, "new URLSearchParams({x: '1', y: '2'}).toString() === 'x=1&y=2'"));
    EMB_CHECK(js_true(&e, "new URLSearchParams([['k', 'v']]).get('k') === 'v' && new URLSearchParams('q=a+b').get('q') === 'a b'"));
    EMB_CHECK(js_true(&e, "String(new URLSearchParams('q=a+b')) === 'q=a+b' && new URLSearchParams().get('nope') === null"));
    EMB_CHECK(js_true(&e, "var lk = new URL('http://a.test/?m=1'); lk.searchParams.delete('m'); lk.href === 'http://a.test/'"));
    env_done(&e);
}

EMB_TEST(bind_text_encoder_and_decoder)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "new TextEncoder().encode('\\u00e9\\u20ac').length === 5 && Array.from(new TextEncoder().encode('A')).join() === '65'"));
    EMB_CHECK(js_true(&e, "new TextDecoder().decode(new Uint8Array([0xe2, 0x82, 0xac])) === '\\u20ac'"));
    EMB_CHECK(js_true(&e, "new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])) === 'A'"));
    EMB_CHECK(js_true(&e, "new TextDecoder('utf-8', {ignoreBOM: true}).decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])).length === 2"));
    EMB_CHECK(js_true(&e, "(function(){ try { new TextDecoder('utf-8', {fatal: true}).decode(new Uint8Array([0xff])); return false; } "
                          "catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var d = new TextDecoder(); d.decode(new Uint8Array([0xe2]), {stream: true}) + d.decode(new Uint8Array([0x82, 0xac])) === '\\u20ac'"));
    EMB_CHECK(js_true(&e, "new TextDecoder('latin1').encoding === 'windows-1252' && new TextDecoder().fatal === false"));
    EMB_CHECK(js_true(&e, "(function(){ try { new TextDecoder('shift_jis'); return false; } catch (err) { return err instanceof RangeError; } })()"));
    EMB_CHECK(js_true(&e, "new TextDecoder().decode(new Uint8Array([0x41, 0x42]).buffer) === 'AB'"));
    env_done(&e);
}

EMB_TEST(bind_atob_and_btoa)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "btoa('hello') === 'aGVsbG8=' && atob('aGVsbG8=') === 'hello'"));
    EMB_CHECK(js_true(&e, "atob('/w==') === '\\u00ff' && btoa('\\u00ff') === '/w=='"));
    EMB_CHECK(js_true(&e, "(function(){ try { btoa('\\u20ac'); return false; } catch (err) { return err.name === 'InvalidCharacterError'; } })()"));
    EMB_CHECK(js_true(&e, "(function(){ try { atob('a'); return false; } catch (err) { return err.name === 'InvalidCharacterError'; } })()"));
    env_done(&e);
}

EMB_TEST(bind_crypto_random_values_and_uuid)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var a = new Uint8Array(16); crypto.getRandomValues(a) === a && a.some(function(x) { return x !== 0; })"));
    EMB_CHECK(js_true(&e, "/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID())"));
    EMB_CHECK(js_true(&e, "(function(){ try { crypto.getRandomValues(new Float32Array(2)); return false; } catch (err) { return err.name === 'TypeMismatchError'; } })()"));
    EMB_CHECK(js_true(&e, "(function(){ try { crypto.getRandomValues(new Uint8Array(65537)); return false; } catch (err) { return err.name === 'QuotaExceededError'; } })()"));
    EMB_CHECK(js_true(&e, "crypto.getRandomValues(new Uint8Array(65536)).length === 65536"));
    env_done(&e);
}

EMB_TEST(bind_structured_clone)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var o = {a: [1, 2], d: new Date(0)}; var k = structuredClone(o); k !== o && k.a[1] === 2 && k.d.getTime() === 0"));
    EMB_CHECK(js_true(&e, "(function(){ try { structuredClone(function() {}); return false; } catch (err) { return err.name === 'DataCloneError'; } })()"));
    EMB_CHECK(js_true(&e, "var cyc = {}; cyc.self = cyc; var cc = structuredClone(cyc); cc.self === cc"));
    env_done(&e);
}

EMB_TEST(bind_url_search_params_sort_order_and_iteration)
{
    env e;
    env_init(&e);
    /* Sorted by UTF-16 code units, case-sensitive: uppercase sorts before lowercase, and the sort is stable. */
    EMB_CHECK(js_true(&e, "var m = new URLSearchParams('a=1&B=2'); m.sort(); m.toString() === 'B=2&a=1'"));
    EMB_CHECK(js_true(&e, "var n = new URLSearchParams('b=1&a=2&b=0&A=3'); n.sort(); n.toString() === 'A=3&a=2&b=1&b=0'"));
    EMB_CHECK(js_true(&e, "var lk = new URL('http://a.test/?z=1&Y=2'); lk.searchParams.sort(); lk.search === '?Y=2&z=1'"));
    EMB_CHECK(js_true(&e, "Array.from(new URLSearchParams('a=1&b=2').keys()).join() === 'a,b' && "
                          "Array.from(new URLSearchParams('a=1&b=2').values()).join() === '1,2' && "
                          "Array.from(new URLSearchParams('a=1&b=2').entries())[1].join() === 'b,2'"));
    EMB_CHECK(js_true(&e, "var fe = ''; new URLSearchParams('p=1&q=2').forEach(function(v, k) { fe += k + v; }); fe === 'p1q2'"));
    EMB_CHECK(js_true(&e, "[...new URLSearchParams('x=1&y=2')].length === 2 && new URLSearchParams('x=1').size === 1"));
    EMB_CHECK(js_true(&e, "new URLSearchParams(new URLSearchParams('a=1&b=2')).toString() === 'a=1&b=2'"));
    EMB_CHECK(js_true(&e, "new URLSearchParams(new Map([['m', '1']])).get('m') === '1'"));
    env_done(&e);
}

EMB_TEST(bind_url_href_setter_throws_on_invalid)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "(function(){ try { var h = new URL('http://a.test/'); h.href = 'nope'; return false; } "
                          "catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var ok = new URL('http://a.test/'); ok.href = 'https://b.test/p'; ok.href === 'https://b.test/p'"));
    env_done(&e);
}

EMB_TEST(bind_text_encoder_encode_into)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var b = new Uint8Array(4); var r = new TextEncoder().encodeInto('a\u20ac', b); "
                          "r.read === 2 && r.written === 4 && b[0] === 97 && b[1] === 0xe2"));
    /* The euro sign does not fit in 2 bytes: only the 'a' is written. */
    EMB_CHECK(js_true(&e, "var c = new Uint8Array(2); var r2 = new TextEncoder().encodeInto('a\u20ac', c); "
                          "r2.read === 1 && r2.written === 1"));
    /* A supplementary character (4 bytes, 2 code units) is never split. */
    EMB_CHECK(js_true(&e, "var d = new Uint8Array(3); var r3 = new TextEncoder().encodeInto(String.fromCharCode(0xd83d, 0xde00), d); "
                          "r3.read === 0 && r3.written === 0"));
    EMB_CHECK(js_true(&e, "(function(){ try { new TextEncoder().encodeInto('a', [1]); return false; } "
                          "catch (err) { return err instanceof TypeError; } })()"));
    env_done(&e);
}

/* The serializer's plain error becomes the DOMException of the standard; Blob and File are cloned. */
EMB_TEST(bind_structured_clone_data_clone_error_and_platform_objects)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "(function(){ try { structuredClone(function() {}); return false; } catch (err) { "
                          "return err instanceof DOMException && err.name === 'DataCloneError' && err.code === 25; } })()"));
    EMB_CHECK(js_true(&e, "(function(){ var ab = new ArrayBuffer(4); structuredClone(ab, {transfer: [ab]}); try { structuredClone(ab, {transfer: [ab]}); return false; }"
                          "catch (err) { return err instanceof DOMException && err.code === 25; } })()"));
    EMB_CHECK(js_true(&e, "var b = new Blob(['abc', 'de'], {type: 'text/x'}); var c = structuredClone(b);"
                          "c !== b && c instanceof Blob && c.size === 5 && c.type === 'text/x'"));
    EMB_CHECK(js_true(&e, "var f = new File(['xy'], 'n.txt', {type: 'text/plain', lastModified: 123}); var g = structuredClone({f: f, arr: [f]});"
                          "g.f instanceof File && g.f.name === 'n.txt' && g.f.lastModified === 123 && g.f.size === 2 && g.arr[0] === g.f"));
    env_done(&e);
}

EMB_TEST(bind_text_decoder_fatal_error_resets_the_decoder)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var d = new TextDecoder('utf-8', {fatal: true}); var n = 0;"
                          "try { d.decode(new Uint8Array([0xfd, 0xef]), {stream: true}); } catch (x) { n++; } n === 1 && d.decode() === ''"));
    env_done(&e);
}

EMB_TEST_MAIN()
