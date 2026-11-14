/*
 * XMLHttpRequest through the bindings (N1b): a fake transport answers app-style routes, and the scripts record what they see
 * in globals. Covers the readyState and event order, the response types, the synchronous and document refusals, errors and
 * abort, the header methods, upload progress and withCredentials. The network cases are in tests/unit/embed/test_embed_net_fetch.c.
 */
#include "loader_env.h"

EMB_TEST(xhr_get_reports_states_and_events_in_order)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/hello", 200, "text/plain;charset=utf-8", "x-a: 1", "hello", false);
    EMB_CHECK(js_run(&e, "var X = new XMLHttpRequest(); var L = []; var R = {};"
                         "X.onreadystatechange = function () { L.push('rs' + X.readyState); };"
                         "['loadstart', 'progress', 'load', 'loadend'].forEach(function (t) { X.addEventListener(t, function () { L.push(t); }); });"
                         "X.open('GET', 'http://t.test/hello'); R.open = X.readyState; X.send();"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.open === 1 && X.readyState === 4 && X.status === 200 && X.statusText === ''"));
    EMB_CHECK(js_true(&e, "X.responseText === 'hello' && X.responseURL === 'http://t.test/hello' && X.response === 'hello'"));
    EMB_CHECK(js_true(&e, "L.join(',') === 'rs1,loadstart,rs2,rs3,progress,rs4,progress,load,loadend'"));
    env_done(&e);
}

EMB_TEST(xhr_response_types_and_headers)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/j", 200, "application/json", "x-b: two\nx-a: 1", "{\"n\":[1,2]}", false);
    EMB_CHECK(js_run(&e, "var J = new XMLHttpRequest(); J.responseType = 'json'; J.open('GET', 'http://t.test/j');"
                         "J.send(); var B = {};"
                         "J.addEventListener('loadend', function () { B.json = J.response.n[1]; B.hdr = J.getResponseHeader('X-B');"
                         "  B.all = J.getAllResponseHeaders(); B.none = J.getResponseHeader('x-missing'); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "B.json === 2 && B.hdr === 'two' && B.none === null"));
    EMB_CHECK(js_true(&e, "B.all === 'content-type: application/json\\r\\nx-a: 1\\r\\nx-b: two\\r\\n'"));
    EMB_CHECK(js_run(&e, "var A = new XMLHttpRequest(); A.responseType = 'arraybuffer'; A.open('GET', 'http://t.test/j'); A.send();"
                         "var Bl = new XMLHttpRequest(); Bl.responseType = 'blob'; Bl.open('GET', 'http://t.test/j'); Bl.send();"
                         "var T = new XMLHttpRequest(); T.open('GET', 'http://t.test/j'); T.send();"
                         "var Q = {}; A.onload = function () { Q.ab = A.response.byteLength; };"
                         "Bl.onload = function () { Q.bl = Bl.response.size + ' ' + Bl.response.type; };"
                         "T.onload = function () { Q.tx = T.responseText; try { A.responseText; } catch (x) { Q.inv = x.name; } };"));
    settle(&e);
    EMB_CHECK(js_true(&e, "Q.ab === 11 && Q.bl === '11 application/json' && Q.tx === '{\"n\":[1,2]}' && Q.inv === 'InvalidStateError'"));
    env_done(&e);
}

EMB_TEST(xhr_refuses_sync_and_document)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var S = {}; var X = new XMLHttpRequest();"
                         "try { X.open('GET', 'http://t.test/x', false); } catch (x) { S.sync = x.name; }"
                         "try { X.responseType = 'document'; } catch (x) { S.doc = x.name; }"
                         "try { X.setRequestHeader('a', 'b'); } catch (x) { S.state = x.name; }"
                         "try { X.open('CONNECT', 'http://t.test/x'); } catch (x) { S.connect = x.name; }"));
    EMB_CHECK(js_true(&e, "S.sync === 'InvalidAccessError' && S.doc === 'NotSupportedError' && S.state === 'InvalidStateError' && S.connect === 'SecurityError'"));
    env_done(&e);
}

EMB_TEST(xhr_network_error_and_abort)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/held", 200, "text/plain", nullptr, "late", true);
    EMB_CHECK(js_run(&e, "var E = {}; var X = new XMLHttpRequest(); X.open('GET', 'http://nowhere.test/');"
                         "X.addEventListener('error', function () { E.error = X.status; }); X.addEventListener('loadend', function () { E.end = X.readyState; });"
                         "X.send();"));
    settle(&e);
    EMB_CHECK(js_true(&e, "E.error === 0 && E.end === 4"));
    EMB_CHECK(js_run(&e, "var Z = {}; var Y = new XMLHttpRequest(); Y.open('GET', 'http://t.test/held');"
                         "Y.addEventListener('abort', function () { Z.abort = Y.readyState; });"
                         "Y.addEventListener('loadend', function () { Z.end = 'yes'; }); Y.send(); Y.abort();"
                         "Z.after = Y.readyState;"));
    settle(&e);
    EMB_CHECK(js_true(&e, "Z.abort === 4 && Z.end === 'yes' && Z.after === 0 && Y.readyState === 0 && Y.status === 0"));
    env_done(&e);
}

EMB_TEST(xhr_request_headers_and_upload_progress)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/up", 201, "text/plain", nullptr, "ok", false);
    EMB_CHECK(js_run(&e, "var U = {}; var X = new XMLHttpRequest(); X.open('POST', 'http://t.test/up');"
                         "X.setRequestHeader('X-Test', 'a'); X.setRequestHeader('X-Test', 'b'); X.setRequestHeader('Cookie', 'no');"
                         "X.upload.addEventListener('progress', function (ev) { U.up = ev.loaded + '/' + ev.total; });"
                         "X.upload.addEventListener('load', function () { U.upload_load = true; });"
                         "X.onload = function () { U.status = X.status; }; X.send('payload');"));
    settle(&e);
    EMB_CHECK(e.net.last_body_len == 7 && memcmp(e.net.last_body, "payload", 7) == 0);
    EMB_CHECK(strstr(e.net.last_headers, "x-test: a, b") != nullptr);
    EMB_CHECK(strstr(e.net.last_headers, "cookie: no") == nullptr);
    EMB_CHECK(js_true(&e, "U.up === '7/7' && U.upload_load === true && U.status === 201"));
    EMB_CHECK(js_run(&e, "var W = new XMLHttpRequest(); W.open('GET', 'http://t.test/up'); W.withCredentials = true; W.send();"));
    EMB_CHECK(js_true(&e, "W.readyState === 1"));
    env_done(&e);
}

EMB_TEST(xhr_is_an_event_target_with_progress_events)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "(function () { var p = new ProgressEvent('progress', { loaded: 3, total: 9, lengthComputable: true });"
                          "  return p instanceof Event && p.loaded === 3 && p.total === 9 && p.lengthComputable === true &&"
                          "    new XMLHttpRequest() instanceof EventTarget && XMLHttpRequest.DONE === 4; })()"));
    env_done(&e);
}

BIND_LOADER_MAIN()
