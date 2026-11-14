/*
 * Blob, File and object URLs through the bindings (W7c): construction from every part type, slicing with negative and
 * clamped bounds, content type normalization, the async readers, File properties, and createObjectURL / revokeObjectURL.
 * Also crypto.getRandomValues on detached and out-of-bounds views.
 */
#include "loader_env.h"

EMB_TEST(blob_construction_size_type_and_parts)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var b = new Blob(['ab', new Uint8Array([99, 100]), new Blob(['ef']), new Uint16Array([0x6867]).buffer], { type: 'Text/PLAIN' });"
                          "b.size === 8 && b.type === 'text/plain'"));
    EMB_CHECK(js_true(&e, "new Blob().size === 0 && new Blob([], { type: 'bad\\u0001type' }).type === ''"));
    EMB_CHECK(js_true(&e, "(function () { try { new Blob('abc'); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "String(new Blob([])) === '[object Blob]'"));
    EMB_CHECK(js_run(&e, "var R = {}; b.text().then(function (t) { R.t = t; }); b.arrayBuffer().then(function (ab) { R.n = ab.byteLength; });"
                         "b.bytes().then(function (u) { R.u = u[0] + ',' + u[7]; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'abcdefgh' && R.n === 8 && R.u === '97,104'"));
    env_done(&e);
}

EMB_TEST(blob_slice_bounds)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var b = new Blob(['0123456789']); var R = {};"
                         "b.slice(2, 5).text().then(function (t) { R.a = t; });"
                         "b.slice(-3).text().then(function (t) { R.b = t; });"
                         "b.slice(-4, -1).text().then(function (t) { R.c = t; });"
                         "b.slice(8, 100).text().then(function (t) { R.d = t; });"
                         "b.slice(5, 2).text().then(function (t) { R.e = t; });"
                         "b.slice(1, 3, 'Image/PNG').type === 'image/png' ? R.f = 1 : R.f = 0;"
                         "b.slice(2, 8).slice(1, 3).text().then(function (t) { R.g = t; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.a === '234' && R.b === '789' && R.c === '678' && R.d === '89' && R.e === '' && R.f === 1 && R.g === '34'"));
    env_done(&e);
}

EMB_TEST(file_last_modified_conversion)
{
    env e;
    env_init(&e);
    /* WebIDL long long: NaN and the infinities are 0, others truncate toward zero and wrap modulo 2^64. */
    EMB_CHECK(js_true(&e, "function lm(v) { return new File([], 'a', { lastModified: v }).lastModified; }"
                          "lm(NaN) === 0 && lm(Infinity) === 0 && lm(-Infinity) === 0 && lm(12.9) === 12 && lm(-12.9) === -12 && lm(0) === 0"));
    EMB_CHECK(js_true(&e, "lm(1e30) === 5076964154930102272 && lm(-1e30) === -5076964154930102272"));
    EMB_CHECK(js_true(&e, "lm(18446744073709551616) === 0 && lm(9223372036854775808) === -9223372036854775808"));
    env_done(&e);
}

EMB_TEST(file_properties)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var f = new File(['hello'], 'a.txt', { type: 'text/plain', lastModified: 1234 });"
                          "f.name === 'a.txt' && f.size === 5 && f.type === 'text/plain' && f.lastModified === 1234 && f instanceof Blob"));
    EMB_CHECK(js_true(&e, "typeof new File([], 'x').lastModified === 'number' && String(new File([], 'x')) === '[object File]'"));
    EMB_CHECK(js_true(&e, "(function () { try { new File([]); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_run(&e, "var R = {}; f.slice(1, 3).text().then(function (t) { R.t = t; R.isBlob = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'el'"));
    env_done(&e);
}

EMB_TEST(object_urls_create_resolve_revoke)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var b = new Blob(['x'], { type: 'text/plain' }); var u = URL.createObjectURL(b);"
                          "u.indexOf('blob:http://t.test/') === 0 && URL.createObjectURL(b) !== u"));
    EMB_CHECK(js_true(&e, "new URL(u).protocol === 'blob:' && new URL(u).origin === 'http://t.test'"));
    EMB_CHECK(js_run(&e, "var R = {}; fetch(u).then(function (r) { return r.text(); }).then(function (t) { R.t = t; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'x'"));
    EMB_CHECK(js_true(&e, "URL.revokeObjectURL(u) === undefined && URL.revokeObjectURL('blob:nothing') === undefined"));
    EMB_CHECK(js_run(&e, "fetch(u).catch(function (err) { R.gone = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.gone === true"));
    EMB_CHECK(js_true(&e, "(function () { try { URL.createObjectURL({}); return false; } catch (err) { return err instanceof TypeError; } })()"));
    env_done(&e);
}

EMB_TEST(object_urls_of_opaque_origin)
{
    env e;
    env_init(&e);
    e.url = "about:blank";
    EMB_CHECK(js_true(&e, "URL.createObjectURL(new Blob(['x'])).indexOf('blob:null/') === 0"));
    env_done(&e);
}

EMB_TEST(crypto_get_random_values_detached_and_bounds)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var a = new Uint8Array(16); crypto.getRandomValues(a) === a && a.some(function (x) { return x !== 0; })"));
    EMB_CHECK(js_true(&e, "var u32 = new Uint32Array(4); crypto.getRandomValues(u32); u32.length === 4"));
    /* A detached buffer is a TypeError, and nothing is written. */
    EMB_CHECK(js_true(&e, "var buf = new ArrayBuffer(16); var v = new Uint8Array(buf); structuredClone(buf, { transfer: [buf] });"
                          "buf.byteLength === 0 && v.length === 0 && (function () { try { crypto.getRandomValues(v); return false; }"
                          " catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var buf2 = new ArrayBuffer(8); var dv = new Uint16Array(buf2, 2, 2); structuredClone(buf2, { transfer: [buf2] });"
                          "(function () { try { crypto.getRandomValues(dv); return false; } catch (err) { return err instanceof TypeError; } })()"));
    /* A float view and an oversized view keep their DOMException names. */
    EMB_CHECK(js_true(&e, "(function () { try { crypto.getRandomValues(new Float32Array(2)); return false; } catch (err) { return err.name === 'TypeMismatchError'; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { crypto.getRandomValues(new Uint8Array(65537)); return false; } catch (err) { return err.name === 'QuotaExceededError'; } })()"));
    EMB_CHECK(js_true(&e, "crypto.getRandomValues(new Uint8Array(65536)).length === 65536"));
    EMB_CHECK(js_true(&e, "/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID())"));
    env_done(&e);
}

BIND_LOADER_MAIN()
