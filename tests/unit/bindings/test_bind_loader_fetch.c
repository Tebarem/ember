/*
 * fetch, Headers, Request and Response through the bindings (W7c): a fake transport answers, the tests read the outcome
 * from globals the scripts set once their promises settle. Covers statuses, headers, body readers, redirects, aborts (before
 * and during), network errors, Request and Response construction and cloning, and bodies of every init type.
 */
#include "loader_env.h"

EMB_TEST(fetch_resolves_response_with_status_headers_and_text)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/hello", 200, "text/plain;charset=utf-8", "x-a: 1\nx-b: two", "hello world", false);
    EMB_CHECK(js_run(&e, "var R = {}; fetch('http://t.test/hello').then(function (r) {"
                         "  R.status = r.status; R.ok = r.ok; R.type = r.type; R.url = r.url; R.ct = r.headers.get('content-type');"
                         "  R.xb = r.headers.get('X-B'); R.used0 = r.bodyUsed;"
                         "  return r.text(); }).then(function (t) { R.text = t; }, function (err) { R.err = String(err); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.status === 200 && R.ok === true && R.type === 'basic' && R.url === 'http://t.test/hello'"));
    EMB_CHECK(js_true(&e, "R.ct === 'text/plain;charset=utf-8' && R.xb === 'two' && R.used0 === false && R.text === 'hello world'"));
    env_done(&e);
}

EMB_TEST(fetch_sends_method_headers_and_body)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/post", 201, "application/json", nullptr, "{\"ok\":true,\"n\":[1,2]}", false);
    EMB_CHECK(js_run(&e, "var R = {}; fetch('http://t.test/post', { method: 'post', headers: { 'X-Test': 'yes' }, body: 'payload' })"
                         ".then(function (r) { R.status = r.status; return r.json(); })"
                         ".then(function (j) { R.json = j; }, function (err) { R.err = String(err); });"));
    settle(&e);
    EMB_CHECK_EQ_STR(e.net.last_method, "POST");
    EMB_CHECK(strstr(e.net.last_headers, "x-test: yes") != nullptr);
    EMB_CHECK(strstr(e.net.last_headers, "content-type: text/plain;charset=UTF-8") != nullptr);
    EMB_CHECK(e.net.last_body_len == 7 && memcmp(e.net.last_body, "payload", 7) == 0);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.status === 201 && R.json.ok === true && R.json.n[1] === 2"));
    env_done(&e);
}

EMB_TEST(fetch_body_readers_arraybuffer_bytes_blob_and_single_use)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/b", 200, "application/octet-stream", nullptr, "abc", false);
    EMB_CHECK(js_run(&e, "var R = {};"
                         "fetch('http://t.test/b').then(function (r) { var r2 = r.clone();"
                         "  return r.arrayBuffer().then(function (ab) { R.ab = ab.byteLength; R.used = r.bodyUsed;"
                         "    return r.text().then(function () { R.second = 'resolved'; }, function (err) { R.second = err instanceof TypeError; });"
                         "  }).then(function () { return r2.blob(); }).then(function (b) { R.blob = b.size; R.btype = b.type; return r2.bytes ? 1 : 1; });"
                         "}).catch(function (err) { R.err = String(err); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.ab === 3 && R.used === true && R.second === true && R.blob === 3 && R.btype === 'application/octet-stream'"));
    env_done(&e);
}

EMB_TEST(fetch_network_error_rejects_with_typeerror)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; fetch('http://nowhere.test/').then(function () { R.res = true; }, function (err) {"
                         "  R.isType = err instanceof TypeError; R.msg = err.message; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.res === undefined && R.isType === true && R.msg.indexOf('Failed to fetch') === 0"));
    EMB_CHECK(js_run(&e, "var R2 = {}; fetch('not a url').catch(function (err) { R2.t = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R2.t === true"));
    env_done(&e);
}

EMB_TEST(fetch_follows_redirects_and_reports_redirected)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/old", 302, nullptr, "location: /new", "", false);
    fake_route_add(&e, "http://t.test/new", 200, "text/plain", nullptr, "arrived", false);
    EMB_CHECK(js_run(&e, "var R = {}; fetch('http://t.test/old').then(function (r) { R.url = r.url; R.redirected = r.redirected; return r.text(); })"
                         ".then(function (t) { R.t = t; });"
                         "var M = {}; fetch('http://t.test/old', { redirect: 'manual' }).then(function (r) { M.type = r.type; M.status = r.status; });"
                         "var X = {}; fetch('http://t.test/old', { redirect: 'error' }).catch(function (err) { X.t = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.url === 'http://t.test/new' && R.redirected === true && R.t === 'arrived'"));
    EMB_CHECK(js_true(&e, "M.type === 'opaqueredirect' && M.status === 0 && X.t === true"));
    env_done(&e);
}

EMB_TEST(fetch_abort_before_and_during)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/slow", 200, "text/plain", nullptr, "late", true);
    EMB_CHECK(js_run(&e, "var R = {}; var c0 = new AbortController(); c0.abort();"
                         "fetch('http://t.test/slow', { signal: c0.signal }).catch(function (err) { R.pre = err.name; });"));
    settle(&e);
    EMB_CHECK(e.net.nrequests == 0);
    EMB_CHECK(js_true(&e, "R.pre === 'AbortError'"));
    EMB_CHECK(js_run(&e, "var c1 = new AbortController(); R.during = 'pending';"
                         "fetch('http://t.test/slow', { signal: c1.signal }).then(function () { R.during = 'resolved'; },"
                         "  function (err) { R.during = err.name; R.same = err === c1.signal.reason; });"
                         "c1.abort();"));
    EMB_CHECK(e.net.aborted == 1);
    settle(&e);
    EMB_CHECK(js_true(&e, "R.during === 'AbortError' && R.same === true"));
    /* A custom reason is what the promise rejects with. */
    EMB_CHECK(js_run(&e, "var c2 = new AbortController(); fetch('http://t.test/slow', { signal: c2.signal })"
                         ".catch(function (err) { R.custom = err; }); c2.abort('why');"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.custom === 'why'"));
    env_done(&e);
}

EMB_TEST(fetch_pending_at_teardown_is_released)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/slow", 200, "text/plain", nullptr, "late", true);
    EMB_CHECK(js_run(&e, "fetch('http://t.test/slow');"));
    EMB_CHECK(e.net.nrequests == 1);
    env_done(&e); /* detach aborts the job; LOADER_TEST_MAIN checks that nothing leaks */
    EMB_CHECK(e.net.aborted == 1);
}

EMB_TEST(fetch_data_and_blob_urls)
{
    env e;
    env_init(&e);
    e.net.nroutes = 0;
    EMB_CHECK(js_run(&e, "var R = {}; var blob = new Blob(['blob body'], { type: 'text/x-test' }); var u = URL.createObjectURL(blob);"
                         "fetch(u).then(function (r) { R.type = r.headers.get('content-type'); return r.text(); }).then(function (t) { R.t = t; });"
                         ""));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.type === 'text/x-test' && R.t === 'blob body'"));
    /* Without a transport the context still answers data: URLs itself, and refuses http. */
    embi_loader_attach(e.c, &(embi_loader_env){0});
    EMB_CHECK(js_run(&e, "fetch('data:text/plain;base64,aGk=').then(function (r) { return r.text(); }).then(function (t) { R.d = t; });"
                         "fetch('http://t.test/x').catch(function (err) { R.noenv = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.d === 'hi' && R.noenv === true"));
    EMB_CHECK(js_run(&e, "URL.revokeObjectURL(u); fetch(u).catch(function (err) { R.revoked = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.revoked === true"));
    env_done(&e);
}

EMB_TEST(headers_api)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var h = new Headers({ 'Content-Type': 'text/html', 'X-A': '1' }); h.append('x-a', '2');"
                          "h.get('x-a') === '1, 2' && h.has('CONTENT-TYPE') && !h.has('nope') && h.get('nope') === null"));
    EMB_CHECK(js_true(&e, "var h2 = new Headers([['b', '2'], ['a', '1']]); Array.from(h2.keys()).join() === 'a,b' && "
                          "Array.from(h2.entries()).map(function (p) { return p.join('='); }).join() === 'a=1,b=2'"));
    EMB_CHECK(js_true(&e, "var h3 = new Headers(); h3.append('Set-Cookie', 'a=1'); h3.append('Set-Cookie', 'b=2');"
                          "h3.getSetCookie().join('|') === 'a=1|b=2'"));
    EMB_CHECK(js_true(&e, "var h4 = new Headers({ a: ' v ' }); h4.get('a') === 'v' && (h4.delete('a'), !h4.has('a'))"));
    EMB_CHECK(js_true(&e, "(function () { try { new Headers().append('bad name', 'x'); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new Headers().append('a', 'x\\ny'); return false; } catch (err) { return err instanceof TypeError; } })()"));
    /* Empty names and values reach the ByteString check with a null pointer (UBSan regression). */
    EMB_CHECK(js_true(&e, "var he = new Headers({ a: '' }); he.has('a') && he.get('a') === ''"));
    EMB_CHECK(js_true(&e, "var hs = new Headers([['a', '']]); hs.has('a') && hs.get('a') === ''"));
    EMB_CHECK(js_true(&e, "(function () { try { new Headers({ '': 'x' }); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new Headers([['', 'x']]); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "new Request('http://t.test/', { headers: { 'X-E': '' } }).headers.get('x-e') === '' &&"
                          "new Response('x', { headers: [['X-E', '']] }).headers.get('x-e') === ''"));
    /* Headers of a constructed Response are mutable; those of a fetch response are immutable. */
    EMB_CHECK(js_true(&e, "var rr = new Response('x'); rr.headers.set('k', 'v'); rr.headers.get('k') === 'v'"));
    EMB_CHECK(js_true(&e, "(function () { try { Response.error().headers.set('k', 'v'); return false; } catch (err) { return err instanceof TypeError; } })()"));
    /* Request headers drop forbidden names silently. */
    EMB_CHECK(js_true(&e, "var rq = new Request('http://t.test/', { headers: { 'Cookie': 'a', 'X-Ok': 'y' } }); !rq.headers.has('cookie') && rq.headers.get('x-ok') === 'y'"));
    env_done(&e);
}

EMB_TEST(request_construction_and_clone)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var q = new Request('http://t.test/p?x=1#f', { method: 'put', body: 'abc' });"
                          "q.method === 'PUT' && q.url === 'http://t.test/p?x=1#f' && q.redirect === 'follow' && q.bodyUsed === false"));
    EMB_CHECK(js_true(&e, "(function () { try { new Request('http://t.test/', { method: 'GET', body: 'x' }); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new Request('http://t.test/', { method: 'CONNECT' }); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var q2 = q.clone(); q2.method === 'PUT' && q2 !== q"));
    EMB_CHECK(js_run(&e, "var R = {}; q.text().then(function (t) { R.t = t; R.used = q.bodyUsed; return q2.text(); }).then(function (t) { R.t2 = t; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'abc' && R.used === true && R.t2 === 'abc'"));
    EMB_CHECK(js_true(&e, "(function () { try { q.clone(); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var q3 = new Request(new Request('http://t.test/a', { method: 'POST' }), { method: 'PATCH' }); q3.method === 'PATCH' && q3.url === 'http://t.test/a'"));
    env_done(&e);
}

EMB_TEST(response_construction_statics_and_clone)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var r = new Response('body', { status: 201, statusText: 'Made', headers: { 'X-K': 'v' } });"
                          "r.status === 201 && r.statusText === 'Made' && r.ok && r.type === 'default' && r.headers.get('content-type') === 'text/plain;charset=UTF-8'"));
    EMB_CHECK(js_true(&e, "(function () { try { new Response('x', { status: 99 }); return false; } catch (err) { return err instanceof RangeError; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new Response('x', { status: 204 }); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "var er = Response.error(); er.type === 'error' && er.status === 0 && !er.ok"));
    EMB_CHECK(js_true(&e, "var rd = Response.redirect('http://t.test/x', 301); rd.status === 301 && rd.headers.get('location') === 'http://t.test/x'"));
    EMB_CHECK(js_true(&e, "(function () { try { Response.redirect('http://t.test/x', 200); return false; } catch (err) { return err instanceof RangeError; } })()"));
    EMB_CHECK(js_run(&e, "var R = {}; var j = Response.json({ a: [1, 2] }, { status: 202 }); R.ct = j.headers.get('content-type'); R.st = j.status;"
                         "var c = r.clone(); r.text().then(function (t) { R.t = t; return c.text(); }).then(function (t) { R.c = t; });"
                         "j.json().then(function (v) { R.a = v.a[1]; });"
                         "new Response(new Uint8Array([104, 105])).text().then(function (t) { R.u8 = t; });"
                         "new Response(new URLSearchParams('a=1&b=2')).text().then(function (t) { R.usp = t; });"
                         "new Response('nope').json().catch(function (err) { R.bad = err instanceof SyntaxError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.ct === 'application/json' && R.st === 202 && R.t === 'body' && R.c === 'body' && R.a === 2 && R.u8 === 'hi'"));
    EMB_CHECK(js_true(&e, "R.usp === 'a=1&b=2' && R.bad === true"));
    env_done(&e);
}

EMB_TEST(formdata_bodies_roundtrip)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var fd = new FormData(); fd.append('a', '1'); fd.append('f', new Blob(['file body'], { type: 'text/plain' }), 'n.txt');"
                         "var resp = new Response(fd); R.ct = resp.headers.get('content-type');"
                         "resp.formData().then(function (back) { R.a = back.get('a'); R.fname = back.get('f').name; R.fsize = back.get('f').size; });"
                         "new Response('x=1&y=%20z', { headers: { 'content-type': 'application/x-www-form-urlencoded' } }).formData()"
                         ".then(function (f) { R.y = f.get('y'); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.ct.indexOf('multipart/form-data; boundary=') === 0 && R.a === '1' && R.fname === 'n.txt' && R.fsize === 9 && R.y === ' z'"));
    env_done(&e);
}

EMB_TEST(multipart_parsing_checks_the_filename_parameter_and_header_case)
{
    env e;
    env_init(&e);
    /* A field named "filename=x" is a text field; a part with a filename and a lower-case content-type is a File. */
    EMB_CHECK(js_run(&e, "var R = {}; var body = '--B\\r\\nContent-Disposition: form-data; name=\"filename=x\"\\r\\n\\r\\ntext\\r\\n"
                         "--B\\r\\nContent-Disposition: form-data; name=\"g\"; filename=\"g.txt\"\\r\\nCONTENT-TYPE: text/x-t\\r\\n\\r\\nfile\\r\\n--B--\\r\\n';"
                         "new Response(body, { headers: { 'content-type': 'multipart/form-data; boundary=B' } }).formData()"
                         ".then(function (f) { R.t = typeof f.get('filename=x'); R.isFile = f.get('g') instanceof File; R.gt = f.get('g').type; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'string' && R.isFile === true && R.gt === 'text/x-t'"));
    env_done(&e);
}

/* Headers from a record: the name converts before the value is read, so a bad name stops the walk; an enumerable
 * symbol key is a TypeError. */
EMB_TEST(headers_record_conversion_order)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var log = []; var rec = {a: 'b'}; Object.defineProperty(rec, 'bad' + String.fromCharCode(0xffff), {value: 'x', enumerable: true});"
                          "var p = new Proxy(rec, {get: function (t, k) { log.push('get:' + String(k)); return t[k]; }});"
                          "var threw = false; try { new Headers(p); } catch (x) { threw = x instanceof TypeError; }"
                          "threw && log.join() === 'get:Symbol(Symbol.iterator),get:a'"));
    EMB_CHECK(js_true(&e, "var o = {}; o[Symbol('s')] = 'v'; var t = false; try { new Headers(o); } catch (x) { t = x instanceof TypeError; } t"));
    EMB_CHECK(js_true(&e, "var o2 = {}; Object.defineProperty(o2, Symbol('s'), {value: 'v', enumerable: false}); [...new Headers(o2)].length === 0"));
    EMB_CHECK(js_true(&e, "var it = new Headers().keys(); Object.getOwnPropertyDescriptor(Object.getPrototypeOf(it), 'next').enumerable === true"));
    env_done(&e);
}

EMB_TEST(request_init_validation)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "function bad(i) { try { new Request('http://x.test/', i); return false; } catch (x) { return x instanceof TypeError; } }"
                          "bad({window: 'x'}) && bad({referrer: 'http://:not a valid URL'}) && bad({mode: 'no-cors', method: 'PUT'}) && "
                          "bad({cache: 'only-if-cached', mode: 'cors'}) && bad({priority: 'invalid'}) && bad({duplex: 'full'}) && "
                          "bad({method: 'POST', body: new ReadableStream()}) && !bad({window: null, priority: 'low', referrer: 'about:client', duplex: 'half'}) && "
                          "!bad({cache: 'only-if-cached', mode: 'same-origin'}) && !bad({method: 'POST', body: new ReadableStream(), duplex: 'half'})"));
    /* no-cors filters the headers copied from another request; a locked body stream stops the copy. */
    EMB_CHECK(js_true(&e, "var r1 = new Request('http://x.test/', {headers: {'Content-Type': 'potato', 'Accept': 'a/b'}});"
                          "var r2 = new Request(r1, {mode: 'no-cors'}); r2.headers.get('content-type') === null && r2.headers.get('accept') === 'a/b'"));
    EMB_CHECK(js_true(&e, "var r3 = new Request('http://x.test/', {method: 'POST', body: 'b'}); r3.body.getReader(); var t3 = false;"
                          "try { new Request(r3); } catch (x) { t3 = x instanceof TypeError; } t3"));
    env_done(&e);
}

EMB_TEST(response_status_text_and_consumed_body)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var t = false; try { new Response('', {statusText: String.fromCharCode(256)}); } catch (x) { t = x instanceof TypeError; } t"));
    EMB_CHECK(js_true(&e, "var r = new Response('abc'); r.text(); var t2 = false; try { r.body.getReader(); } catch (x) { t2 = x instanceof TypeError; } t2"));
    EMB_CHECK(js_true(&e, "var h = new Request('http://x.test/', {mode: 'no-cors'}).headers; h.append('accept', ''); h.append('accept', 's'.repeat(127));"
                          "h.get('accept') === ''"));
    env_done(&e);
}

BIND_LOADER_MAIN()
