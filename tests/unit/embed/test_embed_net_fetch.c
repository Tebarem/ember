/*
 * fetch over the network (N1b) end to end: a runtime with the allowlist and the test CA, an offscreen view, and scripts
 * that call fetch against the 127.0.0.1 HTTP test server. Covers a streamed body (chunks arrive before the response ends),
 * abort during the body, redirects and Response.url and redirected, the redirect modes, the allowlist (a blocked host is a
 * TypeError), credentials (cookies sent for include, not for omit), gzip, and the response type.
 */
#ifdef EMBER_NET

#include "unit/net/net_test.h"
#include "unit/net/net_vectors.h"

#include "base/base.h"
#include "gpu/gpu.h"

#include <ember/ember.h>
#include <ember/ember_net.h>
#include <ember/ember_script.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

#include <string.h>

/* ------------------------------------------------------------------ server */

static SDL_AtomicInt g_released;  /* set by /release: the streamed body continues */
static SDL_AtomicInt g_timed_out; /* /stream gave up waiting for /release (the body was not streamed) */
static SDL_AtomicInt g_flood_aborted; /* /flood: a write failed, so the client went away (the reader cancelled) */
static SDL_AtomicInt g_flood_written; /* bytes /flood has written */
static SDL_AtomicInt g_big_written;   /* bytes /big has written (a slow reader must keep this low) */

/* Writes count chunks of size bytes (chunked coding; the caller sent the head). False when the client went away. */
static bool serve_chunks(ts_conn *c, uint32_t count, uint32_t size, SDL_AtomicInt *written)
{
    static char block[65536];
    for (uint32_t i = 0; i < count; i++) {
        char line[32];
        SDL_snprintf(line, sizeof line, "%x\r\n", (unsigned)size);
        if (!ts_conn_write(c, line, strlen(line)) || !ts_conn_write(c, block, size) || !ts_conn_write(c, "\r\n", 2)) {
            return false;
        }
        SDL_AddAtomicInt(written, (int)size);
    }
    return ts_conn_write(c, "0\r\n\r\n", 5);
}

static void handler(ts_conn *c, const ts_req *r, void *user)
{
    (void)user;
    if (SDL_strcmp(r->target, "/stream") == 0) {
        SDL_SetAtomicInt(&g_released, 0);
        SDL_SetAtomicInt(&g_timed_out, 0);
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        ts_printf(c, "5\r\npart1\r\n");
        for (int i = 0; i < 500 && SDL_GetAtomicInt(&g_released) == 0; i++) {
            SDL_Delay(10);
        }
        if (SDL_GetAtomicInt(&g_released) == 0) {
            SDL_SetAtomicInt(&g_timed_out, 1);
        }
        ts_printf(c, "5\r\npart2\r\n0\r\n\r\n");
        c->close = true;
    } else if (SDL_strcmp(r->target, "/release") == 0) {
        SDL_SetAtomicInt(&g_released, 1);
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "released");
    } else if (SDL_strcmp(r->target, "/r1") == 0) {
        ts_respond_str(c, 302, "Location: /r2\r\n", "");
    } else if (SDL_strcmp(r->target, "/r2") == 0) {
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "landed");
    } else if (SDL_strcmp(r->target, "/set") == 0) {
        ts_respond_str(c, 200, "Set-Cookie: sid=abc; Path=/\r\n", "set");
    } else if (SDL_strcmp(r->target, "/echo") == 0) {
        const char *ck = ts_req_header(r, "cookie");
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", ck != nullptr ? ck : "none");
    } else if (SDL_strcmp(r->target, "/slow") == 0) {
        SDL_Delay(1500);
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "late");
    } else if (SDL_strcmp(r->target, "/flood") == 0) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        if (!serve_chunks(c, 4000, 65536, &g_flood_written)) {
            SDL_SetAtomicInt(&g_flood_aborted, 1);
        }
        c->close = true;
    } else if (SDL_strcmp(r->target, "/big") == 0) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        /* 96 MiB. The connection is left to the client (no close here): closing with the client still reading would reset it. */
        (void)serve_chunks(c, 1536, 65536, &g_big_written);
    } else if (SDL_strcmp(r->target, "/gzip") == 0) {
        ts_respond(c, 200, "Content-Type: text/plain\r\nContent-Encoding: gzip\r\n", net_vec_gzip, sizeof net_vec_gzip);
    } else {
        ts_respond_str(c, 404, "", "");
    }
}

/* ------------------------------------------------------------------ runtime and view */

static ts_server *g_srv;
static emb_runtime *g_rt;
static emb_view *g_view;
static embi_gpu *g_gpu;
static char g_base[128];
static char g_out[512];

static void on_log(void *user, emb_log_level level, const char *text)
{
    (void)user;
    (void)level;
    (void)text;
}

static void on_result(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    (void)ok;
    size_t n = len < sizeof g_out - 1 ? len : sizeof g_out - 1;
    SDL_memcpy(g_out, text, n);
    g_out[n] = '\0';
}

/* False when no offscreen view can be made (an offscreen view needs a GPU device): the test then skips. */
static bool setup(const char *allow)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return false;
    }
    g_gpu = embi_gpu_create(nullptr, false);
    if (g_gpu == nullptr) {
        SDL_Quit();
        return false;
    }
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = on_log;
    g_rt = emb_runtime_create(&rd);
    EMB_CHECK(g_rt != nullptr);
    EMB_CHECK(emb_runtime_net_allow(g_rt, allow));
    emb_view_desc vd;
    SDL_memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 320;
    vd.height = 240;
    vd.scale = 1.0f;
    vd.device = embi_gpu_device(g_gpu);
    g_view = emb_view_create(g_rt, &vd);
    if (g_view == nullptr) {
        emb_runtime_destroy(g_rt);
        g_rt = nullptr;
        embi_gpu_destroy(g_gpu);
        g_gpu = nullptr;
        return false;
    }
    EMB_CHECK(emb_view_load_html(g_view, "<!doctype html><title>n</title>", 29, nullptr));
    return true;
}

static void teardown(void)
{
    emb_view_destroy(g_view);
    g_view = nullptr;
    emb_runtime_destroy(g_rt);
    g_rt = nullptr;
    embi_gpu_destroy(g_gpu);
    g_gpu = nullptr;
}

/* Runs a script in the view (its promise jobs too). */
static void run(const char *src)
{
    EMB_CHECK(emb_view_eval(g_view, src, SDL_strlen(src), nullptr, nullptr));
}

/* One frame of the offscreen view: rendering runs the view's script loop (its posted tasks and timers) too. */
static void pump_frame(void)
{
    SDL_GPUDevice *dev = embi_gpu_device(g_gpu);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return;
    }
    (void)emb_view_render(g_view, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
}

/* Evaluates a boolean expression in the view: renders frames until it holds or ms passes. */
static bool wait_for(const char *expr, uint32_t ms)
{
    char src[1024];
    SDL_snprintf(src, sizeof src, "(function(){try{return (%s) ? 'yes' : 'no';}catch(e){return 'threw ' + e;}})()", expr);
    uint64_t t0 = SDL_GetTicks();
    for (;;) {
        pump_frame();
        g_out[0] = '\0';
        (void)emb_view_eval(g_view, src, SDL_strlen(src), on_result, nullptr);
        if (SDL_strcmp(g_out, "yes") == 0) {
            return true;
        }
        if (SDL_GetTicks() - t0 > ms) {
            SDL_Log("    wait_for timed out: %s (last: %s)", expr, g_out);
            return false;
        }
        SDL_Delay(2);
    }
}

static void url_of(char *out, size_t cap, const char *path)
{
    SDL_snprintf(out, cap, "%s%s", g_base, path);
}

static void start_server(void)
{
    g_srv = ts_start(handler, nullptr, nullptr, nullptr);
    EMB_CHECK(g_srv != nullptr);
    /* The base is the server's own origin, as a document would name it. */
    ts_url(g_srv, g_base, sizeof g_base, "");
}

static void stop_server(void)
{
    ts_stop(g_srv);
    g_srv = nullptr;
}

/* ------------------------------------------------------------------ tests */

EMB_TEST(fetch_streams_chunks_before_the_response_ends)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var R = {}; fetch('%s/stream').then(function (r) {"
                 "  R.type = r.type; R.status = r.status; R.url = r.url; R.rd = r.body.getReader(); return R.rd.read(); })"
                 ".then(function (a) { R.first = new TextDecoder().decode(a.value); return fetch('%s/release'); })"
                 ".then(function () { return R.rd.read(); })"
                 ".then(function (a) { R.second = new TextDecoder().decode(a.value); return R.rd.read(); })"
                 ".then(function (a) { R.end = a.done; R.done = true; }, function (e) { R.err = String(e); R.done = true; });",
                 g_base, g_base);
    run(src);
    EMB_CHECK(wait_for("R.done === true", 8000));
    EMB_CHECK(wait_for("R.err === undefined && R.type === 'cors' && R.status === 200", 1000));
    EMB_CHECK(wait_for("R.first === 'part1' && R.second === 'part2' && R.end === true", 1000));
    EMB_CHECK(SDL_GetAtomicInt(&g_timed_out) == 0);
    teardown();
    stop_server();
}

EMB_TEST(fetch_abort_mid_body_errors_the_stream)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var A = {}; var ac = new AbortController();"
                 "fetch('%s/stream', { signal: ac.signal }).then(function (r) { A.rd = r.body.getReader(); return A.rd.read(); })"
                 ".then(function (a) { A.first = new TextDecoder().decode(a.value); ac.abort(); return A.rd.read(); })"
                 ".then(function () { A.res = 'read'; }, function (e) { A.name = e.name; A.done = true; });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("A.done === true", 8000));
    EMB_CHECK(wait_for("A.first === 'part1' && A.name === 'AbortError' && A.res === undefined", 1000));
    /* Nothing is left running: the server's wait for /release is never answered, so it gives up on its own. */
    teardown();
    stop_server();
}

EMB_TEST(fetch_follows_redirects_and_reports_the_final_url)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var D = {}; fetch('%s/r1').then(function (r) { D.url = r.url; D.redirected = r.redirected; D.type = r.type;"
                 "  return r.text(); }).then(function (t) { D.text = t; });"
                 "fetch('%s/r1', { redirect: 'manual' }).then(function (r) { D.manual = r.type; });"
                 "fetch('%s/r1', { redirect: 'error' }).catch(function (e) { D.err = e instanceof TypeError; });",
                 g_base, g_base, g_base);
    run(src);
    EMB_CHECK(wait_for("D.text === 'landed' && D.manual === 'opaqueredirect' && D.err === true", 8000));
    char want[160];
    url_of(want, sizeof want, "/r2");
    char check[320];
    SDL_snprintf(check, sizeof check, "D.url === '%s' && D.redirected === true && D.type === 'cors'", want);
    EMB_CHECK(wait_for(check, 1000));
    teardown();
    stop_server();
}

EMB_TEST(fetch_blocked_host_is_a_type_error)
{
    start_server();
    /* Only the literal address is allowed: the same server under localhost is refused before any lookup. */
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[512];
    char port_url[160];
    SDL_snprintf(port_url, sizeof port_url, "http://localhost:%d/hello", g_srv->port);
    SDL_snprintf(src, sizeof src,
                 "var B = {}; fetch('%s').then(function () { B.ok = true; }, function (e) { B.type = e instanceof TypeError; B.done = true; });",
                 port_url);
    run(src);
    EMB_CHECK(wait_for("B.done === true", 8000));
    EMB_CHECK(wait_for("B.ok === undefined && B.type === true", 1000));
    teardown();
    stop_server();
}

EMB_TEST(fetch_credentials_send_cookies_only_for_include)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var K = {}; fetch('%s/set', { credentials: 'include' }).then(function () {"
                 "  return fetch('%s/echo', { credentials: 'include' }); })"
                 ".then(function (r) { return r.text(); }).then(function (t) { K.inc = t; return fetch('%s/echo', { credentials: 'omit' }); })"
                 ".then(function (r) { return r.text(); }).then(function (t) { K.omit = t; K.done = true; });",
                 g_base, g_base, g_base);
    run(src);
    EMB_CHECK(wait_for("K.done === true", 8000));
    EMB_CHECK(wait_for("K.inc === 'sid=abc' && K.omit === 'none'", 1000));
    teardown();
    stop_server();
}

EMB_TEST(fetch_decodes_gzip_bodies)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[512];
    SDL_snprintf(src, sizeof src,
                 "var Z = {}; fetch('%s/gzip').then(function (r) { return r.text(); }).then(function (t) { Z.text = t; Z.done = true; });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("Z.done === true", 8000));
    EMB_CHECK(wait_for("Z.text.length === 900 && Z.text.slice(0, 45) === 'The quick brown fox jumps over the lazy dog. '"
                       " && Z.text.slice(855) === 'The quick brown fox jumps over the lazy dog. '",
                       1000));
    teardown();
    stop_server();
}

EMB_TEST(xhr_over_the_network_reports_states_and_times_out)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var X = new XMLHttpRequest(); var L = []; var T = {};"
                 "X.onreadystatechange = function () { L.push('rs' + X.readyState); };"
                 "X.addEventListener('load', function () { L.push('load'); });"
                 "X.addEventListener('loadend', function () { T.done = true; });"
                 "X.open('GET', '%s/r2'); X.send();"
                 "var Y = new XMLHttpRequest(); Y.timeout = 200; Y.open('GET', '%s/slow');"
                 "Y.addEventListener('timeout', function () { T.timeout = Y.readyState; T.status = Y.status; });"
                 "Y.addEventListener('loadend', function () { T.tend = true; }); Y.send();",
                 g_base, g_base);
    run(src);
    EMB_CHECK(wait_for("T.done === true && T.tend === true", 8000));
    EMB_CHECK(wait_for("X.status === 200 && X.responseText === 'landed' && L.join(',') === 'rs1,rs2,rs3,rs4,load'", 1000));
    EMB_CHECK(wait_for("T.timeout === 4 && T.status === 0 && Y.readyState === 4", 1000));
    teardown();
    stop_server();
}

EMB_TEST(fetch_view_destroyed_while_the_body_streams)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var V = {}; fetch('%s/stream').then(function (r) { V.rd = r.body.getReader(); return V.rd.read(); })"
                 ".then(function (a) { V.first = new TextDecoder().decode(a.value); }, function (e) { V.err = String(e); });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("V.first === 'part1'", 8000));
    /* The body is still arriving: the view goes first (the loader aborts its requests), then the server is let go. */
    teardown();
    SDL_SetAtomicInt(&g_released, 1);
    stop_server();
}

EMB_TEST(fetch_reader_cancel_aborts_the_request)
{
    SDL_SetAtomicInt(&g_flood_aborted, 0);
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "var Q = {}; fetch('%s/flood').then(function (r) { Q.rd = r.body.getReader(); return Q.rd.read(); })"
                 ".then(function (a) { Q.got = a.value.byteLength; return Q.rd.cancel(); })"
                 ".then(function () { Q.cancelled = true; }, function (e) { Q.err = String(e); });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("Q.cancelled === true && Q.got > 0 && Q.err === undefined", 8000));
    /* The cancel closes the socket, so the server's writes fail and it stops before sending the whole body. */
    bool aborted = false;
    uint64_t t0 = SDL_GetTicks();
    while (!aborted && SDL_GetTicks() - t0 < 8000) {
        pump_frame();
        SDL_Delay(5);
        aborted = SDL_GetAtomicInt(&g_flood_aborted) != 0;
    }
    EMB_CHECK(aborted);
    teardown();
    stop_server();
}

EMB_TEST(fetch_manual_redirect_is_an_opaqueredirect_over_the_network)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[512];
    SDL_snprintf(src, sizeof src,
                 "var M = {}; fetch('%s/r1', { redirect: 'manual' }).then(function (r) {"
                 "  M.type = r.type; M.status = r.status; M.url = r.url; M.done = true; }, function (e) { M.err = String(e); M.done = true; });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("M.done === true", 8000));
    EMB_CHECK(wait_for("M.type === 'opaqueredirect' && M.status === 0 && M.err === undefined", 1000));
    teardown();
    stop_server();
}

EMB_TEST(xhr_with_credentials_sends_cookies_over_the_network)
{
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1536];
    SDL_snprintf(src, sizeof src,
                 "var W = {};"
                 "fetch('%s/set', { credentials: 'include' }).then(function () {"
                 "  return new Promise(function (res) {"
                 "    var a = new XMLHttpRequest(); a.open('GET', '%s/echo');"
                 "    a.addEventListener('loadend', function () { W.plain = a.responseText; W.statusText = a.statusText; res(); });"
                 "    a.send(); }); })"
                 ".then(function () {"
                 "  return new Promise(function (res) {"
                 "    var b = new XMLHttpRequest(); b.withCredentials = true; b.open('GET', '%s/echo');"
                 "    b.addEventListener('loadend', function () { W.cred = b.responseText; res(); });"
                 "    b.send(); }); })"
                 ".then(function () { W.done = true; });",
                 g_base, g_base, g_base);
    run(src);
    EMB_CHECK(wait_for("W.done === true", 8000));
    EMB_CHECK(wait_for("W.plain === 'none' && W.cred === 'sid=abc' && W.statusText === 'Status'", 1000));
    teardown();
    stop_server();
}

EMB_TEST(fetch_slow_reader_keeps_the_network_body_bounded)
{
    SDL_SetAtomicInt(&g_big_written, 0);
    start_server();
    if (!setup("127.0.0.1")) {
        stop_server();
        EMB_SKIP("no GPU device for an offscreen view");
    }
    char src[1536];
    SDL_snprintf(src, sizeof src,
                 "var B = { bytes: 0 };"
                 "function drain() { return B.rd.read().then(function (a) {"
                 "  if (a.done) { B.done = true; return; } B.bytes += a.value.byteLength; return drain(); }); }"
                 "fetch('%s/big').then(function (r) { B.rd = r.body.getReader(); return B.rd.read(); })"
                 ".then(function (a) { B.bytes += a.value.byteLength; B.started = true;"
                 "  return new Promise(function (res) { setTimeout(res, 2500); }); })"
                 ".then(function () { B.resumed = true; return drain(); })"
                 ".catch(function (e) { B.err = String(e); B.done = true; });",
                 g_base);
    run(src);
    EMB_CHECK(wait_for("B.started === true", 8000));
    /* While the reader stalls the request is held (the queue is past its limit); once the reader drains, the held request
     * resumes, and the whole 96 MiB arrives intact. The bound itself is not asserted here: loopback socket buffers take
     * most of the body, so the server's write count does not show it. */
    EMB_CHECK(wait_for("B.done === true", 60000));
    EMB_CHECK(wait_for("B.bytes === 100663296 && B.err === undefined", 1000));
    teardown();
    stop_server();
}

EMB_TEST_MAIN()

#else

/* EMBER_NET=OFF: there is no network to test; the test is skipped (exit 77). */
int main(void)
{
    return 77;
}

#endif
