/*
 * Resource handlers v2 through the runtime (ember_resource.h, embed/runtime.c): a synchronous v2 handler
 * serving a fetch with a method, a body, a status, a media type and headers; an asynchronous handler that
 * completes later, completes inside its start, or completes with no response (a network error); an abort
 * in flight (the cancel call, and the late completion is dropped); a refused scheme; and the registration
 * rules for schemes.
 */
#include "emb_test.h"

#include <ember/ember.h>
#include <ember/ember_resource.h>

#include "embed/embed_int.h"
#include "embed/resource_int.h"
#include "loader/fetch.h"

#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

static int sync_calls;
static int async_starts;
static int async_cancels;
static emb_request *held_req;
static bool complete_in_start;

static bool sync_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    sync_calls++;
    static const emb_header hs[] = {{"X-Test", 6, "yes", 3}};
    out->status = 201;
    out->mime = "text/plain";
    out->mime_len = 10;
    out->headers = hs;
    out->nheaders = 1;
    out->data = (const uint8_t *)"sync";
    out->len = 4;
    return req->method_len == 4 && memcmp(req->method, "POST", 4) == 0 && req->body_len == 1 &&
           req->url_len > 0;
}

static void async_start(void *user, const emb_request_info *req)
{
    (void)user;
    async_starts++;
    held_req = req->request;
    if (complete_in_start) {
        static const uint8_t body[] = "now";
        emb_response resp = {.size = sizeof resp, .status = 0, .data = body, .len = 3};
        emb_resource_complete(req->request, &resp);
        held_req = nullptr;
    }
}

static void async_cancel(void *user, emb_request *r)
{
    (void)user;
    async_cancels++;
    EMB_CHECK(r == held_req);
}

typedef struct rec {
    int calls;
    bool ok;
    bool has_resp;
    embi_response resp;
} rec;

static void done_cb(void *user, embi_fetch_op *op, bool ok)
{
    rec *r = user;
    r->calls++;
    r->ok = ok;
    if (ok) {
        embi_fetch_op_take(op, &r->resp);
        r->has_resp = true;
    }
}

static void reset_counters(void)
{
    sync_calls = 0;
    async_starts = 0;
    async_cancels = 0;
    held_req = nullptr;
    complete_in_start = false;
}

EMB_TEST(handlers_v2_sync_through_transport)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_v2(rt, "app", sync_handler, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("POST");
    in.url = EMBI_STR("app://bundle/x");
    in.body = (const uint8_t *)"b";
    in.body_len = 1;
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(sync_calls == 1);
    EMB_CHECK(r.calls == 1 && r.ok && r.has_resp);
    EMB_CHECK(r.resp.status == 201);
    EMB_CHECK(r.resp.body.bytes.len == 4);
    EMB_CHECK(embi_headers_has(&r.resp.headers, EMBI_STR("x-test")));
    EMB_CHECK(embi_headers_has(&r.resp.headers, EMBI_STR("content-type")));
    embi_response_free(&r.resp);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_async_pending_then_complete)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, async_cancel, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/slow");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(async_starts == 1);
    EMB_CHECK(r.calls == 0);
    EMB_REQUIRE(held_req != nullptr);
    static const uint8_t body[] = "late";
    emb_response resp = {.size = sizeof resp, .status = 200, .data = body, .len = 4};
    emb_resource_complete(held_req, &resp);
    EMB_CHECK(r.calls == 1 && r.ok && r.has_resp);
    EMB_CHECK(r.resp.body.bytes.len == 4);
    embi_response_free(&r.resp);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_async_abort_drops_late_result)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, async_cancel, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/abort-me");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr && held_req != nullptr);
    embi_fetch_op_abort(op);
    EMB_CHECK(async_cancels == 1);
    emb_response resp = {.size = sizeof resp, .status = 200};
    emb_resource_complete(held_req, &resp);
    EMB_CHECK(r.calls == 0);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_async_complete_inside_start)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    complete_in_start = true;
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, nullptr, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/now");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(r.calls == 1 && r.ok);
    EMB_CHECK(r.resp.status == 200 && r.resp.body.bytes.len == 3);
    embi_response_free(&r.resp);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_async_null_response_is_network_error)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, nullptr, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/none");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr && held_req != nullptr);
    emb_resource_complete(held_req, nullptr);
    EMB_CHECK(r.calls == 1 && !r.ok);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_unregistered_scheme_is_refused)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("zzz://nowhere/x");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(r.calls == 1 && !r.ok);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

EMB_TEST(handlers_registration_rules)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(!emb_runtime_set_resource_handler_v2(rt, "file", sync_handler, nullptr));
    EMB_CHECK(!emb_runtime_set_resource_handler_async(rt, "data", async_start, nullptr, nullptr));
    EMB_CHECK(!emb_runtime_set_resource_handler_v2(rt, "bad scheme!", sync_handler, nullptr));
    EMB_CHECK(emb_runtime_set_resource_handler_v2(rt, "app", sync_handler, nullptr));
    /* Replacing the handler frees the old record (the leak check of the run sees it). */
    EMB_CHECK(emb_runtime_set_resource_handler_async(rt, "app", async_start, nullptr, nullptr));
    EMB_CHECK(emb_runtime_set_resource_handler_v2(rt, "app", nullptr, nullptr));
    EMB_CHECK(emb_runtime_set_resource_handler_v2(rt, "app", sync_handler, nullptr));
    emb_runtime_destroy(rt);
}

static int destroy_cancels;
static uint16_t v1_status;

/* A cancel that completes the request at once (the handler owes one completion after the cancel). */
static void destroy_cancel(void *user, emb_request *r)
{
    (void)user;
    destroy_cancels++;
    static const uint8_t b[] = "x";
    emb_response resp = {.size = sizeof resp, .status = 200, .data = b, .len = 1};
    emb_resource_complete(r, &resp);
}

static bool status_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    (void)req;
    static const uint8_t b[] = "ok";
    out->status = v1_status;
    out->data = b;
    out->len = 2;
    return true;
}

EMB_TEST(handlers_destroy_aborts_pending_request)
{
    reset_counters();
    destroy_cancels = 0;
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, destroy_cancel, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/pending");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr && held_req != nullptr);
    /* The runtime goes away with the request in flight: the request is aborted, its handler completes it (freeing it),
     * and the operation is no longer pending. */
    emb_runtime_destroy(rt);
    EMB_CHECK(destroy_cancels == 1);
    EMB_CHECK(r.calls == 0);
    EMB_CHECK(!embi_fetch_op_pending(op));
    embi_fetch_op_free(op);
}

EMB_TEST(handlers_completion_after_destroy_is_safe)
{
    reset_counters();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "app", async_start, nullptr, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("app://bundle/late");
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr && held_req != nullptr);
    emb_request *late = held_req;
    emb_runtime_destroy(rt);
    /* The handler completes after the runtime is gone: the record frees itself and nothing is delivered. */
    static const uint8_t body[] = "late";
    emb_response resp = {.size = sizeof resp, .status = 200, .data = body, .len = 4};
    emb_resource_complete(late, &resp);
    EMB_CHECK(r.calls == 0);
    embi_fetch_op_free(op);
}

EMB_TEST(handlers_v1_path_refuses_non_2xx)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_v2(rt, "app", status_handler, nullptr));
    uint8_t *data = nullptr;
    size_t len = 0;
    v1_status = 404;
    EMB_CHECK(!embi_rt_fetch(rt, EMBI_STR("app://bundle/missing"), &data, &len));
    v1_status = 0;
    EMB_CHECK(embi_rt_fetch(rt, EMBI_STR("app://bundle/present"), &data, &len));
    EMB_CHECK(len == 2);
    SDL_free(data);
    emb_runtime_destroy(rt);
}

/* Copies an eval result into the caller's buffer (255 bytes). */
static void copy_eval(void *user, bool ok, const char *text, size_t len)
{
    (void)ok;
    char *dst = user;
    size_t n = len < 255 ? len : 255;
    memcpy(dst, text, n);
    dst[n] = 0;
}

/* A blob: URL made by script resolves for the view's own fetch path (documents, <img>) and the transport, and stops
 * resolving once it is revoked. */
EMB_TEST(blob_urls_resolve_for_view_loads_until_revoked)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view_desc desc = {.size = sizeof desc,
                          .mode = EMB_VIEW_OFFSCREEN,
                          .width = 64,
                          .height = 64,
                          .scale = 1.0f,
                          .device = embi_gpu_device(g),
                          .scheme = EMB_SCHEME_LIGHT};
    emb_view *v = emb_view_create(rt, &desc);
    EMB_REQUIRE(v != nullptr);
    static const char page[] = "<!doctype html><body>x</body>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), "http://t.test/index.html"));

    char url[256] = "";
    const char *mk = "window.u = URL.createObjectURL(new Blob(['<b>hi</b>'], {type: 'text/html'})); window.u";
    EMB_REQUIRE(emb_view_eval(v, mk, strlen(mk), copy_eval, url));
    EMB_REQUIRE(strncmp(url, "blob:", 5) == 0);

    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_REQUIRE(embi_rt_fetch(rt, (embi_str){url, (uint32_t)strlen(url)}, &data, &len));
    EMB_CHECK_EQ_INT((int)len, 9);
    EMB_CHECK(data != nullptr && memcmp(data, "<b>hi</b>", 9) == 0);
    SDL_free(data);

    char url2[256] = "";
    const char *mk2 = "window.u2 = URL.createObjectURL(new Blob(['<i>yo</i>'], {type: 'text/html'})); window.u2";
    EMB_REQUIRE(emb_view_eval(v, mk2, strlen(mk2), copy_eval, url2));

    /* The transport (hop_start builtin) serves it too. */
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = (embi_str){url, (uint32_t)strlen(url)};
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK_EQ_INT(r.calls, 1);
    EMB_CHECK(r.ok && r.resp.status == 200 && r.resp.body.bytes.len == 9);
    embi_response_free(&r.resp);
    embi_fetch_op_free(op);

    const char *rv = "URL.revokeObjectURL(window.u); 'ok'";
    char res[256] = "";
    EMB_REQUIRE(emb_view_eval(v, rv, strlen(rv), copy_eval, res));
    EMB_CHECK_EQ_STR(res, "ok");
    EMB_CHECK(!embi_rt_fetch(rt, (embi_str){url, (uint32_t)strlen(url)}, &data, &len));
    EMB_CHECK(!emb_view_load_url(v, url));
    /* The view's loader path takes a live one as a document. */
    EMB_CHECK(emb_view_load_url(v, url2));

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

/* ---- request header values reach a handler in wire form: U+00E9 is the single byte 0xE9 ---- */
static unsigned char wire_seen[16];
static size_t wire_seen_len;

static bool wire_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    for (size_t i = 0; i < req->nheaders; i++) {
        if (req->headers[i].name_len == 5 && memcmp(req->headers[i].name, "x-lat", 5) == 0) {
            wire_seen_len = req->headers[i].value_len < sizeof wire_seen ? req->headers[i].value_len : sizeof wire_seen;
            memcpy(wire_seen, req->headers[i].value, wire_seen_len);
        }
    }
    /* A response header value is bytes too: 0xE9 is stored as the UTF-8 of U+00E9. */
    static const emb_header resp_hdr[] = {{"x-resp", 6, "caf\xE9", 4}};
    out->headers = resp_hdr;
    out->nheaders = 1;
    out->status = 200;
    return true;
}

EMB_TEST(handler_request_headers_are_wire_bytes)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_v2(rt, "wire", wire_handler, nullptr));
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    /* Stored form: UTF-8 of the code points of the byte string "caf" + 0xE9. */
    static const embi_hdr_view hv[] = {{{"x-lat", 5}, {"caf\xC3\xA9", 5}}};
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = EMBI_STR("wire://h/x");
    in.headers = hv;
    in.nheaders = 1;
    in.redirect = EMBI_REDIRECT_FOLLOW;
    rec r = {0};
    wire_seen_len = 0;
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, done_cb, &r);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(r.ok);
    EMB_REQUIRE(wire_seen_len == 4);
    EMB_CHECK(memcmp(wire_seen, "caf\xE9", 4) == 0);
    embi_str rh = embi_headers_first(&r.resp.headers, EMBI_STR("x-resp"));
    EMB_CHECK(rh.len == 5 && memcmp(rh.ptr, "caf\xC3\xA9", 5) == 0);
    embi_response_free(&r.resp);
    embi_fetch_op_free(op);
    emb_runtime_destroy(rt);
}

/* ---- document charset of emb_view_load_url: the transport charset, else a meta tag, else UTF-8 (never mojibake) */
static bool charset_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    /* /utf8: UTF-8 bytes, no charset anywhere; /latin: a Latin-1 byte with a transport charset; /meta: a meta charset. */
    static const uint8_t utf8_page[] = "<!doctype html><body>caf\xC3\xA9</body>";
    static const uint8_t latin_page[] = "<!doctype html><body>caf\xE9</body>";
    static const uint8_t meta_page[] = "<!doctype html><meta charset=windows-1252><body>caf\xE9</body>";
    out->status = 200;
    const char *path = req->url + req->url_len - 5;
    if (memcmp(path, "/utf8", 5) == 0) {
        out->mime = "text/html";
        out->mime_len = 9;
        out->data = utf8_page;
        out->len = sizeof utf8_page - 1;
    } else if (memcmp(path, "latin", 5) == 0) {
        out->mime = "text/html; charset=iso-8859-1";
        out->mime_len = 29;
        out->data = latin_page;
        out->len = sizeof latin_page - 1;
    } else {
        out->mime = "text/html";
        out->mime_len = 9;
        out->data = meta_page;
        out->len = sizeof meta_page - 1;
    }
    return true;
}

EMB_TEST(view_load_url_decodes_utf8_by_default)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler_v2(rt, "cs", charset_handler, nullptr));
    emb_view_desc desc = {.size = sizeof desc,
                          .mode = EMB_VIEW_OFFSCREEN,
                          .width = 64,
                          .height = 64,
                          .scale = 1.0f,
                          .device = embi_gpu_device(g),
                          .scheme = EMB_SCHEME_LIGHT};
    emb_view *v = emb_view_create(rt, &desc);
    EMB_REQUIRE(v != nullptr);
    const char *js = "document.body.textContent.length + ':' + document.body.textContent.charCodeAt(3)";
    char res[256] = "";
    /* No charset anywhere: UTF-8 (4 code units, U+00E9), not windows-1252 (5 units). */
    EMB_REQUIRE(emb_view_load_url(v, "cs://h/utf8"));
    EMB_REQUIRE(emb_view_eval(v, js, strlen(js), copy_eval, res));
    EMB_CHECK(strstr(res, "4:233") != nullptr);
    /* A transport charset wins: one Latin-1 byte. */
    EMB_REQUIRE(emb_view_load_url(v, "cs://h/latin"));
    EMB_REQUIRE(emb_view_eval(v, js, strlen(js), copy_eval, res));
    EMB_CHECK(strstr(res, "4:233") != nullptr);
    /* A meta charset is honored. */
    EMB_REQUIRE(emb_view_load_url(v, "cs://h/meta0"));
    EMB_REQUIRE(emb_view_eval(v, js, strlen(js), copy_eval, res));
    EMB_CHECK(strstr(res, "4:233") != nullptr);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

/* The GPU device of the blob test needs the video subsystem, initialized once around the run. */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
