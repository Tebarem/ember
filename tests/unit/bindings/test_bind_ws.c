/*
 * WebSocket, CloseEvent and EventSource (N1c): the JS API against the 127.0.0.1 test servers, through the real hooks of
 * the embedding layer (src/embed/netapi.c) and the network service. Events arrive on the thread that runs the script,
 * as tasks of a queue the test pumps with a microtask checkpoint after each, as a view's loop does.
 */
#include "emb_test.h"

#ifndef EMBER_NET

EMB_TEST(websocket_needs_the_network_build)
{
    EMB_SKIP("built with EMBER_NET=OFF");
}

EMB_TEST_MAIN()

#else

#include "loader_env.h"

#include "../net/ws_test.h"

#include "embed/netapi.h"

/* ------------------------------------------------------------------ the SSE server */

typedef struct bsse {
    SDL_AtomicInt attempts;
} bsse;

static void bsse_handler(ts_conn *c, const ts_req *r, void *user)
{
    bsse *x = user;
    c->close = true;
    const char *t = r->target;
    if (strncmp(t, "/wrongtype", 10) == 0) {
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "data: x\n\n");
        return;
    }
    if (strncmp(t, "/notfound", 9) == 0) {
        ts_respond_str(c, 404, "", "");
        return;
    }
    int attempt = SDL_AddAtomicInt(&x->attempts, 1) + 1;
    const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
    ts_conn_write(c, head, strlen(head));
    if (strncmp(t, "/reconnect", 10) == 0 && attempt == 1) {
        const char *body = "retry: 30\nid: 7\ndata: first\n\n";
        ts_conn_write(c, body, strlen(body));
        return;
    }
    const char *body = strncmp(t, "/reconnect", 10) == 0 ? "event: custom\ndata: second\ndata: line\n\n"
                                                           : "id: 3\ndata: hello\n\n";
    ts_conn_write(c, body, strlen(body));
    while (ts_conn_pull(c, 20) && !SDL_GetAtomicInt(&c->srv->stop)) {
    }
}

/* ------------------------------------------------------------------ the environment */

typedef struct benv {
    env e;
    wts s;
    ts_server *sse;
    bsse sx;
    embi_netapi_host host;
    bool net_hooks;
} benv;

static bool benv_start(benv *b, bool tls, bool hooks)
{
    memset(b, 0, sizeof *b);
    if (!wts_start(&b->s, tls)) {
        return false;
    }
    b->sse = ts_start(bsse_handler, &b->sx, nullptr, nullptr);
    env_init(&b->e);
    b->host = (embi_netapi_host){b->s.net, tq_post, nullptr, nullptr, b->s.q};
    embi_loader_env le = {0};
    le.user = b->s.q;
    le.post = tq_post;
    if (hooks) {
        embi_netapi_fill(&le.net, &b->host);
    }
    embi_loader_attach(b->e.c, &le);
    b->net_hooks = hooks;
    return b->sse != nullptr;
}

/* The test server's port and the EventSource server's, as globals of the page. */
static void benv_globals(benv *b)
{
    char src[256];
    SDL_snprintf(src, sizeof src, "var WS = 'ws%s://127.0.0.1:%d'; var ES = 'http://127.0.0.1:%d'; var R = {};", b->s.tls ? "s" : "",
                 b->s.srv->port, b->sse->port);
    EMB_CHECK(js_run(&b->e, src));
}

static void benv_stop(benv *b)
{
    env_done(&b->e); /* detaches: connections are dropped, late events find nothing */
    ts_stop(b->sse);
    wts_stop(&b->s);
}

static bool quiet_true(env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "t.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e->rt);
        return false;
    }
    return v == embi_js_from_bool(true);
}

/* Runs the queue (with microtasks after each task) until the expression is true. */
static bool pump(benv *b, const char *expr, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    for (;;) {
        embi_js_run_jobs(b->e.rt);
        if (quiet_true(&b->e, expr)) {
            return true;
        }
        if (SDL_GetTicks() >= end) {
            SDL_Log("    pump timed out waiting for: %s", expr);
            js_dump(&b->e, "R");
            return false;
        }
        (void)tq_step(b->s.q, 10);
    }
}

static void pump_for(benv *b, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        embi_js_run_jobs(b->e.rt);
        (void)tq_step(b->s.q, 10);
    }
    embi_js_run_jobs(b->e.rt);
}

/* Every test returns embi_mem_live to where it was. */
#define BEGIN(tls, hooks)                                                                                              \
    benv b;                                                                                                            \
    EMB_REQUIRE(benv_start(&b, tls, hooks));                                                                           \
    benv_globals(&b)

/* ------------------------------------------------------------------ WebSocket */

EMB_TEST(websocket_constants_and_constructor_errors)
{
    BEGIN(false, true);
    EMB_CHECK(js_true(&b.e, "WebSocket.CONNECTING === 0 && WebSocket.OPEN === 1 && WebSocket.CLOSING === 2 && WebSocket.CLOSED === 3"));
    EMB_CHECK(js_true(&b.e, "WebSocket.prototype.OPEN === 1 && WebSocket.prototype instanceof EventTarget && CloseEvent.prototype instanceof Event"));
    EMB_CHECK(js_true(&b.e, "(function(){ var n = ''; for (var u of ['ftp://x/', 'ws://h/#frag', 'http://', 'ws://h:99999/', 'data:,x']) {"
                            "try { new WebSocket(u); n += '!'; } catch (e) { n += e.name === 'SyntaxError' ? 'S' : '?'; } } R.errs = n; return n === 'SSSSS'; })()") || (js_dump(&b.e, "R.errs"), false));
    EMB_CHECK(js_true(&b.e, "(function(){ var n = ''; for (var p of [['a', 'a'], [''], ['bad token'], ['a,b'], ['ok', 'ok']]) {"
                            "try { new WebSocket(WS + '/echo', p); n += '!'; } catch (e) { n += e.name === 'SyntaxError' ? 'S' : '?'; } } R.errs2 = n; return n === 'SSSS!' || n === 'SSSSS'; })()")
              || js_true(&b.e, "R.errs2 === 'SSSS!'"));
    EMB_CHECK(js_true(&b.e, "(function(){ try { WebSocket('ws://h/'); return false; } catch (e) { return e instanceof TypeError; } })()"));
    /* http and https stand for ws and wss; a relative URL is resolved against the document */
    EMB_CHECK(js_true(&b.e, "new WebSocket('http://127.0.0.1:1/x').url === 'ws://127.0.0.1:1/x'"));
    EMB_CHECK(js_true(&b.e, "new WebSocket('https://127.0.0.1:1/x').url === 'wss://127.0.0.1:1/x'"));
    EMB_CHECK(js_true(&b.e, "new WebSocket('/chat?q=1').url === 'ws://t.test/chat?q=1'"));
    EMB_CHECK(js_true(&b.e, "(function(){ var w = new WebSocket('ws://127.0.0.1:1/x'); return w.readyState === 0 && w.bufferedAmount === 0 && w.protocol === '' && w.extensions === '' && w.binaryType === 'blob'; })()"));
    benv_stop(&b);
}

static const char *k_events_script =
    "R.log = [];"
    "var ws = new WebSocket(WS + '/echo');"
    "R.ws = ws;"
    "ws.onopen = function (e) { R.log.push('open:' + ws.readyState + ':' + e.type + ':' + (e.target === ws) + ':' + e.isTrusted); ws.send('hi'); };"
    "ws.onmessage = function (e) { R.log.push('message:' + e.data + ':' + (e instanceof MessageEvent) + ':' + (e.origin === WS)); R.msgState = ws.readyState; ws.close(1000, 'bye'); R.after = ws.readyState; };"
    "ws.onerror = function (e) { R.log.push('error'); };"
    "ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + e.reason + ':' + e.wasClean + ':' + ws.readyState + ':' + (e instanceof CloseEvent) + ':' + e.isTrusted); };";

static void scenario_events(bool tls)
{
    BEGIN(tls, true);
    EMB_CHECK(js_run(&b.e, k_events_script));
    EMB_CHECK(js_true(&b.e, "R.ws.readyState === 0 && R.log.length === 0")); /* nothing happens synchronously */
    EMB_CHECK(pump(&b, "R.log.length === 3", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'open:1:open:true:true|message:hi:true:true|close:1000:bye:true:3:true:true'"));
    EMB_CHECK(js_true(&b.e, "R.msgState === 1 && R.after === 2"));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.s.x.close_code), 1000);
    pump_for(&b, 100);
    EMB_CHECK(js_true(&b.e, "R.log.length === 3")); /* no more events */
    benv_stop(&b);
}

EMB_TEST(websocket_events_and_their_order) { scenario_events(false); }
EMB_TEST(secure_websocket_events_and_their_order) { scenario_events(true); }

EMB_TEST(send_while_connecting_throws_invalid_state_error)
{
    BEGIN(false, true);
    EMB_CHECK(js_true(&b.e, "(function(){ var w = new WebSocket(WS + '/echo'); try { w.send('x'); return false; } catch (e) { R.name = e.name; return e.name === 'InvalidStateError' && e instanceof DOMException && w.bufferedAmount === 0; } })()"));
    benv_stop(&b);
}

EMB_TEST(close_argument_validation)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "var ws = new WebSocket(WS + '/echo'); ws.onopen = function () { R.open = true; }; ws.onclose = function (e) { R.closed = e.code + ':' + e.wasClean; };"));
    EMB_CHECK(pump(&b, "R.open === true", 10000));
    EMB_CHECK(js_true(&b.e, "(function(){ var n = ''; for (var c of [1001, 999, 2999, 5000, 1005, 1006, 0]) {"
                            "try { ws.close(c); n += '!'; } catch (e) { n += e.name === 'InvalidAccessError' ? 'A' : '?'; } } return n === 'AAAAAAA'; })()"));
    EMB_CHECK(js_true(&b.e, "(function(){ try { ws.close(1000, 'x'.repeat(124)); return false; } catch (e) { return e.name === 'SyntaxError'; } })()"));
    EMB_CHECK(js_true(&b.e, "(function(){ try { ws.close(1000, '\\u20ac'.repeat(42)); return false; } catch (e) { return e.name === 'SyntaxError'; } })()")); /* 126 bytes */
    EMB_CHECK(js_true(&b.e, "ws.readyState === 1")); /* the failed calls closed nothing */
    EMB_CHECK(js_run(&b.e, "ws.close(3000, 'x'.repeat(123)); R.state = ws.readyState; ws.close(1000); ws.close(3001, 'later');"));
    EMB_CHECK(js_true(&b.e, "R.state === 2"));
    EMB_CHECK(pump(&b, "typeof R.closed === 'string'", 10000));
    EMB_CHECK(js_true(&b.e, "R.closed === '3000:true'"));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.s.x.close_code), 3000);
    benv_stop(&b);
}

EMB_TEST(close_without_arguments_and_with_a_clamped_code)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "var ws = new WebSocket(WS + '/echo'); ws.onopen = function () { ws.close(); }; ws.onclose = function (e) { R.c = e.code + ':' + e.wasClean + ':' + e.reason; };"));
    EMB_CHECK(pump(&b, "typeof R.c === 'string'", 10000));
    EMB_CHECK(js_true(&b.e, "R.c === '1005:true:'"));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.s.x.close_code), 1005);
    EMB_CHECK(js_run(&b.e, "var w2 = new WebSocket(WS + '/echo'); w2.onopen = function () { try { w2.close(1e9); } catch (e) { R.clamp = e.name; } w2.close(1000); }; w2.onclose = function (e) { R.c2 = e.code; };"));
    EMB_CHECK(pump(&b, "typeof R.c2 === 'number'", 10000));
    EMB_CHECK(js_true(&b.e, "R.clamp === 'InvalidAccessError' && R.c2 === 1000"));
    benv_stop(&b);
}

EMB_TEST(close_while_connecting_fails_the_connection)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "var ws = new WebSocket(WS + '/echo'); R.log = [];"
                           "ws.onopen = function () { R.log.push('open'); }; ws.onerror = function () { R.log.push('error'); };"
                           "ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + e.wasClean); };"
                           "ws.close(); R.state = ws.readyState;"));
    EMB_CHECK(js_true(&b.e, "R.state === 2"));
    EMB_CHECK(pump(&b, "R.log.length >= 2 && R.log[R.log.length - 1].indexOf('close') === 0", 10000));
    EMB_CHECK(js_true(&b.e, "ws.readyState === 3"));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'error|close:1006:false' || R.log.join('|') === 'open|close:1005:true'"));
    benv_stop(&b);
}

EMB_TEST(failed_handshake_gives_error_then_close_1006)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/badaccept');"
                           "ws.onopen = function () { R.log.push('open'); }; ws.onmessage = function () { R.log.push('message'); };"
                           "ws.onerror = function (e) { R.log.push('error:' + e.type + ':' + ws.readyState); };"
                           "ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + e.wasClean + '[' + e.reason + ']:' + ws.readyState); };"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    EMB_CHECK(js_true(&b.e, "R.log[0] === 'error:error:0' || R.log[0] === 'error:error:3'"));
    EMB_CHECK(js_true(&b.e, "R.log[1] === 'close:1006:false[]:3'"));
    benv_stop(&b);
}

EMB_TEST(a_host_off_the_allowlist_fails_the_connection)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket('ws://example.invalid/x');"
                           "ws.onerror = function () { R.log.push('error'); }; ws.onclose = function (e) { R.log.push('close:' + e.code); };"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'error|close:1006'"));
    benv_stop(&b);
}

EMB_TEST(without_a_network_the_connection_fails_asynchronously)
{
    BEGIN(false, false);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/echo'); R.sync = ws.readyState;"
                           "ws.onerror = function () { R.log.push('error'); }; ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + e.wasClean); };"
                           "var es = new EventSource(ES + '/basic'); R.esState = es.readyState;"
                           "es.onerror = function () { R.log.push('es-error:' + es.readyState); }; es.onopen = function () { R.log.push('es-open'); };"));
    EMB_CHECK(js_true(&b.e, "R.sync === 0 && R.esState === 0 && R.log.length === 0"));
    EMB_CHECK(pump(&b, "R.log.length === 3", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|').indexOf('error|close:1006:false') >= 0 && R.log.join('|').indexOf('es-error:2') >= 0 && R.log.join('|').indexOf('open') < 0"));
    benv_stop(&b);
}

EMB_TEST(without_a_network_close_right_after_construction_still_ends_the_object)
{
    BEGIN(false, false);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/echo'); ws.onerror = function () { R.log.push('error'); };"
                           "ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + ws.readyState); }; ws.close(); R.state = ws.readyState;"));
    EMB_CHECK(js_true(&b.e, "R.state === 2"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'error|close:1006:3'"));
    benv_stop(&b);
}

EMB_TEST(subprotocols_and_the_protocol_attribute)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "var ws = new WebSocket(WS + '/proto', ['chat', 'other']); ws.onopen = function () { R.p = ws.protocol; };"
                           "var w2 = new WebSocket(WS + '/proto', 'chat'); w2.onopen = function () { R.p2 = w2.protocol; };"
                           "var w3 = new WebSocket(WS + '/proto'); w3.onopen = function () { R.p3 = w3.protocol; };"));
    EMB_CHECK(pump(&b, "R.p !== undefined && R.p2 !== undefined && R.p3 !== undefined", 10000));
    EMB_CHECK(js_true(&b.e, "R.p === 'chat' && R.p2 === 'chat' && R.p3 === ''"));
    benv_stop(&b);
}

EMB_TEST(binary_type_blob_and_arraybuffer)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/send/300');"
                           "ws.onmessage = function (e) { R.log.push(e.data); R.isBlob = e.data instanceof Blob; R.size = e.data.size; ws.close(); };"
                           "ws.onclose = function () { R.log.push('closed'); };"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    EMB_CHECK(js_true(&b.e, "R.isBlob === true && R.size === 300"));
    EMB_CHECK(js_true(&b.e, "(function(){ ws.binaryType = 'bogus'; var a = ws.binaryType === 'blob'; ws.binaryType = 'arraybuffer'; return a && ws.binaryType === 'arraybuffer'; })()"));
    EMB_CHECK(js_run(&b.e, "R.log = []; var w2 = new WebSocket(WS + '/send/300'); w2.binaryType = 'arraybuffer';"
                           "w2.onmessage = function (e) { R.buf = e.data; var u = new Uint8Array(e.data); var ok = true; for (var i = 0; i < 300; i++) { if (u[i] !== ((i * 7 + 3) & 255)) ok = false; } R.ok = ok; w2.close(); };"
                           "w2.onclose = function () { R.log.push('closed'); };"));
    EMB_CHECK(pump(&b, "R.log.length === 1", 10000));
    EMB_CHECK(js_true(&b.e, "R.buf instanceof ArrayBuffer && R.buf.byteLength === 300 && R.ok === true"));
    benv_stop(&b);
}

EMB_TEST(send_accepts_strings_blobs_and_buffers)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.got = []; var ws = new WebSocket(WS + '/echo'); ws.binaryType = 'arraybuffer';"
                           "ws.onopen = function () {"
                           "  ws.send('text \\u20ac');"
                           "  ws.send(new Uint8Array([1, 2, 3]));"
                           "  ws.send(new Uint8Array([9, 8, 7, 6]).subarray(1, 3));"
                           "  ws.send(new Uint16Array([258]).buffer);"
                           "  ws.send(new DataView(new Uint8Array([5, 6]).buffer));"
                           "  ws.send(new Blob(['ab', new Uint8Array([99])]));"
                           "  ws.send(12);"
                           "  ws.send({ toString: function () { return 'obj'; } });"
                           "  ws.send('\\ud800');" /* a lone surrogate becomes U+FFFD */
                           "};"
                           "ws.onmessage = function (e) { R.got.push(typeof e.data === 'string' ? 's:' + e.data : 'b:' + Array.prototype.join.call(new Uint8Array(e.data), ',')); };"));
    EMB_CHECK(pump(&b, "R.got.length === 9", 10000));
    EMB_CHECK(js_true(&b.e, "R.got.join('|') === 's:text \\u20ac|b:1,2,3|b:8,7|b:2,1|b:5,6|b:97,98,99|s:12|s:obj|s:\\ufffd'"));
    benv_stop(&b);
}

EMB_TEST(buffered_amount_counts_queued_bytes)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "var ws = new WebSocket(WS + '/sink'); ws.onopen = function () { R.open = true; }; ws.onclose = function () { R.closed = true; };"));
    EMB_CHECK(pump(&b, "R.open === true", 10000));
    EMB_CHECK(js_run(&b.e, "var big = new Uint8Array(3000000); ws.send(big); R.b1 = ws.bufferedAmount; ws.send('\\u20ac'); R.b2 = ws.bufferedAmount;"));
    EMB_CHECK(js_true(&b.e, "R.b1 <= 3000000 && R.b2 - R.b1 <= 3 && R.b2 <= 3000003"));
    EMB_CHECK(pump(&b, "ws.bufferedAmount === 0", 15000)); /* everything reached the socket */
    for (int i = 0; i < 300 && SDL_GetAtomicInt(&b.s.x.sink_msgs) < 2; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.s.x.sink_msgs), 2);
    EMB_CHECK(js_run(&b.e, "ws.close(1000);"));
    EMB_CHECK(pump(&b, "R.closed === true", 10000));
    /* after the close, send() discards but still counts (the standard) */
    EMB_CHECK(js_true(&b.e, "(function(){ var a = ws.bufferedAmount; ws.send('abc'); ws.send('\\u00e9'); return a === 0 && ws.bufferedAmount === 5; })()"));
    EMB_CHECK(js_true(&b.e, "(function(){ ws.send(new Uint8Array(10)); return ws.bufferedAmount === 15; })()"));
    benv_stop(&b);
}

EMB_TEST(listeners_handlers_and_event_objects)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/hello');"
                           "ws.addEventListener('open', function (e) { R.log.push('L1'); });"
                           "ws.addEventListener('open', function (e) { R.log.push('L2'); });"
                           "ws.onopen = function () { R.log.push('H'); };"
                           "ws.addEventListener('message', function (e) { R.log.push('m:' + e.data); ws.close(3333, 'r'); });"
                           "ws.addEventListener('close', function (e) { R.log.push('c:' + e.code + e.reason); R.ev = e; });"
                           "R.getter = typeof ws.onopen;"));
    EMB_CHECK(pump(&b, "R.log.length === 5", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'L1|L2|H|m:hello|c:3333r' && R.getter === 'function'"));
    EMB_CHECK(js_true(&b.e, "R.ev.target === ws && R.ev.bubbles === false && R.ev.cancelable === false"));
    EMB_CHECK(js_true(&b.e, "(function(){ var e = new CloseEvent('close', { code: 1000, reason: 'r', wasClean: true, bubbles: true }); var d = new CloseEvent('x');"
                            "return e.code === 1000 && e.reason === 'r' && e.wasClean === true && e.bubbles && d.code === 0 && d.reason === '' && d.wasClean === false && e instanceof Event && e.isTrusted === false; })()"));
    benv_stop(&b);
}

EMB_TEST(invalid_frames_fail_the_connection_with_error_and_close_1006)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var ws = new WebSocket(WS + '/badutf8'); ws.onopen = function () { R.log.push('open'); }; ws.onmessage = function () { R.log.push('message'); };"
                           "ws.onerror = function () { R.log.push('error'); }; ws.onclose = function (e) { R.log.push('close:' + e.code + ':' + e.wasClean); };"));
    EMB_CHECK(pump(&b, "R.log.length === 3", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'open|error|close:1006:false'"));
    for (int i = 0; i < 100 && SDL_GetAtomicInt(&b.s.x.close_code) == 0; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.s.x.close_code), 1007);
    benv_stop(&b);
}

EMB_TEST(a_connection_stays_alive_without_references)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; (function () { var ws = new WebSocket(WS + '/echo');"
                           "ws.onopen = function () { R.log.push('open'); ws.send('x'); };"
                           "ws.onmessage = function (e) { R.log.push('message:' + e.data); ws.close(); };"
                           "ws.onclose = function (e) { R.log.push('close'); }; })();"));
    for (int i = 0; i < 3; i++) {
        embi_js_gc(b.e.rt);
        pump_for(&b, 30);
    }
    EMB_CHECK(pump(&b, "R.log.length === 3", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'open|message:x|close'"));
    /* now nothing refers to it and the connection is over: it can be collected */
    embi_js_gc(b.e.rt);
    benv_stop(&b);
}

EMB_TEST(the_document_going_away_drops_open_and_connecting_sockets)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.open = 0; var a = new WebSocket(WS + '/hold'); a.onopen = function () { R.open++; };"
                           "var c = new WebSocket(WS + '/hold'); c.onopen = function () { R.open++; };"));
    EMB_CHECK(pump(&b, "R.open === 2", 10000));
    EMB_CHECK(js_run(&b.e, "var d = new WebSocket(WS + '/hold'); a.send('queued'); a.close(1000);")); /* one still connecting, one closing */
    benv_stop(&b); /* detach and free with all three alive: the events still in the queue are dropped */
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(many_sockets_at_once)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.n = 0; R.sockets = []; for (var i = 0; i < 10; i++) { (function (i) { var w = new WebSocket(WS + '/echo'); R.sockets.push(w);"
                           "w.onopen = function () { w.send('s' + i); }; w.onmessage = function (e) { if (e.data === 's' + i) { R.n++; w.close(); } }; })(i); }"));
    EMB_CHECK(pump(&b, "R.n === 10", 15000));
    EMB_CHECK(pump(&b, "R.sockets.every(function (w) { return w.readyState === 3; })", 10000));
    benv_stop(&b);
}

/* ------------------------------------------------------------------ EventSource */

EMB_TEST(eventsource_constants_url_and_errors)
{
    BEGIN(false, true);
    EMB_CHECK(js_true(&b.e, "EventSource.CONNECTING === 0 && EventSource.OPEN === 1 && EventSource.CLOSED === 2 && EventSource.prototype instanceof EventTarget"));
    EMB_CHECK(js_true(&b.e, "(function(){ try { new EventSource('http://'); return false; } catch (e) { return e.name === 'SyntaxError'; } })()"));
    EMB_CHECK(js_true(&b.e, "(function(){ try { EventSource('/x'); return false; } catch (e) { return e instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&b.e, "(function(){ var e = new EventSource('/stream'); var f = new EventSource('/stream', { withCredentials: true }); R.es = [e, f];"
                            "return e.url === 'http://t.test/stream' && e.withCredentials === false && f.withCredentials === true && e.readyState === 0; })()"));
    EMB_CHECK(js_true(&b.e, "R.es[0].close() === undefined && R.es[0].readyState === 2 && (R.es[0].close(), R.es[0].readyState === 2)"));
    benv_stop(&b);
}

EMB_TEST(eventsource_delivers_events)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var es = new EventSource(ES + '/basic');"
                           "es.onopen = function (e) { R.log.push('open:' + es.readyState + ':' + e.type + ':' + (e.target === es)); };"
                           "es.onmessage = function (e) { R.log.push('message:' + e.data + ':' + e.lastEventId + ':' + (e.origin === ES) + ':' + (e instanceof MessageEvent) + ':' + e.isTrusted); es.close(); R.state = es.readyState; };"));
    EMB_CHECK(js_true(&b.e, "R.log.length === 0 && es.readyState === 0"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'open:1:open:true|message:hello:3:true:true:true' && R.state === 2"));
    pump_for(&b, 100);
    EMB_CHECK(js_true(&b.e, "R.log.length === 2 && es.readyState === 2"));
    benv_stop(&b);
}

EMB_TEST(eventsource_reconnects_and_dispatches_named_events)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var es = new EventSource(ES + '/reconnect');"
                           "es.onopen = function () { R.log.push('open:' + es.readyState); };"
                           "es.onerror = function () { R.log.push('error:' + es.readyState); };"
                           "es.onmessage = function (e) { R.log.push('message:' + e.data + ':' + e.lastEventId); };"
                           "es.addEventListener('custom', function (e) { R.log.push('custom:' + e.data + ':' + e.type + ':' + e.lastEventId); es.close(); });"));
    EMB_CHECK(pump(&b, "R.log.length >= 5 && es.readyState === 2", 15000));
    EMB_CHECK(js_true(&b.e, "R.log.join('|') === 'open:1|message:first:7|error:0|open:1|custom:second\\nline:custom:7'"));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&b.sx.attempts), 2);
    benv_stop(&b);
}

EMB_TEST(eventsource_fails_on_a_wrong_content_type_or_status)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; var e1 = new EventSource(ES + '/wrongtype'); var e2 = new EventSource(ES + '/notfound');"
                           "e1.onerror = function () { R.log.push('e1:' + e1.readyState); }; e1.onopen = function () { R.log.push('open1'); };"
                           "e2.onerror = function () { R.log.push('e2:' + e2.readyState); }; e2.onopen = function () { R.log.push('open2'); };"));
    EMB_CHECK(pump(&b, "R.log.length === 2", 10000));
    pump_for(&b, 200);
    EMB_CHECK(js_true(&b.e, "R.log.length === 2 && R.log.indexOf('e1:2') >= 0 && R.log.indexOf('e2:2') >= 0 && e1.readyState === 2 && e2.readyState === 2"));
    benv_stop(&b);
}

EMB_TEST(eventsource_survives_garbage_collection_and_document_teardown)
{
    BEGIN(false, true);
    EMB_CHECK(js_run(&b.e, "R.log = []; (function () { var es = new EventSource(ES + '/basic'); es.onmessage = function (e) { R.log.push(e.data); }; })();"));
    embi_js_gc(b.e.rt);
    EMB_CHECK(pump(&b, "R.log.length === 1 && R.log[0] === 'hello'", 10000));
    EMB_CHECK(js_run(&b.e, "var live = new EventSource(ES + '/basic'); var live2 = new EventSource(ES + '/reconnect');"));
    benv_stop(&b); /* two connections alive at teardown */
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

BIND_LOADER_MAIN()

#endif
