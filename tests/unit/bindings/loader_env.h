#pragma once
/*
 * Shared harness of the loader binding tests (W7c): a document with a binding context whose loader environment has a fake
 * fetch transport. The transport answers from a route table; a route is served at once, or held until settle() (so a test
 * can abort a fetch while it is in flight). Requests are recorded. settle() alternates microtask runs and held completions
 * until nothing moves.
 */
#include "emb_test.h"

#include "../loader/loader_test.h"

#include "bindings/bind.h"
#include "bindings/loader_host.h"
#include "js/js.h"
#include "js/jobs.h"

#include <string.h>

#define FAKE_MAX_ROUTES 16
#define FAKE_MAX_PENDING 16

typedef struct fake_route {
    const char *url;
    uint16_t status;
    const char *mime;
    const char *headers; /* "name: value\nname: value" or nullptr */
    const char *body;
    size_t body_len; /* 0: strlen(body) */
    bool hold;       /* completes at settle() instead of inside start */
} fake_route;

typedef struct fake_pending {
    embi_fetch_op *op;
    const fake_route *route;
    bool live;
} fake_pending;

typedef struct fake_net {
    fake_route routes[FAKE_MAX_ROUTES];
    int nroutes;
    fake_pending pending[FAKE_MAX_PENDING];
    int nrequests;
    char last_method[16];
    char last_url[256];
    char last_headers[512]; /* "name: value\n" per header */
    char last_body[512];
    size_t last_body_len;
    int aborted;
} fake_net;

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
    fake_net net;
    const char *url; /* the document URL the context reports */
} env;

static embi_str env_url(void *user)
{
    const env *e = user;
    return (embi_str){e->url, (uint32_t)strlen(e->url)};
}

[[maybe_unused]] static void fake_route_add(env *e, const char *url, uint16_t status, const char *mime, const char *headers,
                           const char *body, bool hold)
{
    fake_route *r = &e->net.routes[e->net.nroutes++];
    *r = (fake_route){url, status, mime, headers, body, 0, hold};
}

static void fake_serve(fake_net *n, embi_fetch_op *op, const fake_route *r)
{
    embi_hdr_view hv[8];
    uint32_t nh = 0;
    char buf[512];
    buf[0] = 0;
    if (r->headers != nullptr) {
        SDL_strlcpy(buf, r->headers, sizeof buf);
        char *line = buf;
        while (line != nullptr && *line != 0 && nh < 8) {
            char *nl = strchr(line, '\n');
            if (nl != nullptr) {
                *nl = 0;
            }
            char *colon = strchr(line, ':');
            if (colon != nullptr) {
                *colon = 0;
                char *val = colon + 1;
                while (*val == ' ') {
                    val++;
                }
                hv[nh++] = (embi_hdr_view){{line, (uint32_t)strlen(line)}, {val, (uint32_t)strlen(val)}};
            }
            line = nl != nullptr ? nl + 1 : nullptr;
        }
    }
    size_t bl = r->body != nullptr ? (r->body_len != 0 ? r->body_len : strlen(r->body)) : 0;
    embi_fetch_hop hop = {0};
    hop.status = r->status;
    hop.mime = (embi_str){r->mime, r->mime != nullptr ? (uint32_t)strlen(r->mime) : 0};
    hop.headers = hv;
    hop.nheaders = nh;
    hop.body = (const uint8_t *)r->body;
    hop.body_len = bl;
    (void)n;
    embi_fetch_hop_done(op, &hop);
}

static bool fake_start(void *user, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                       const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    fake_net *n = user;
    n->nrequests++;
    SDL_snprintf(n->last_method, sizeof n->last_method, "%.*s", (int)method.len, method.ptr);
    SDL_snprintf(n->last_url, sizeof n->last_url, "%.*s", (int)url.len, url.ptr);
    n->last_headers[0] = 0;
    for (uint32_t i = 0; i < nheaders; i++) {
        size_t l = strlen(n->last_headers);
        SDL_snprintf(n->last_headers + l, sizeof n->last_headers - l, "%.*s: %.*s\n", (int)headers[i].name.len,
                     headers[i].name.ptr, (int)headers[i].value.len, headers[i].value.ptr);
    }
    n->last_body_len = body_len < sizeof n->last_body ? body_len : sizeof n->last_body;
    if (body_len > 0) {
        memcpy(n->last_body, body, n->last_body_len);
    }
    const fake_route *r = nullptr;
    for (int i = 0; i < n->nroutes; i++) {
        if (strlen(n->routes[i].url) == url.len && memcmp(n->routes[i].url, url.ptr, url.len) == 0) {
            r = &n->routes[i];
        }
    }
    if (r == nullptr) {
        return false; /* refused: a network error */
    }
    if (!r->hold) {
        fake_serve(n, op, r);
        return true;
    }
    for (int i = 0; i < FAKE_MAX_PENDING; i++) {
        if (!n->pending[i].live) {
            n->pending[i] = (fake_pending){op, r, true};
            *handle = &n->pending[i];
            return true;
        }
    }
    return false;
}

static void fake_abort(void *user, void *handle)
{
    fake_net *n = user;
    fake_pending *p = handle;
    p->live = false;
    n->aborted++;
}

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
    e->url = "http://t.test/index.html";
    e->c = embi_bind_new(e->rt, e->doc);
    embi_bind_host host;
    memset(&host, 0, sizeof host);
    host.user = e;
    host.url = env_url;
    embi_bind_set_host(e->c, &host);
    embi_loader_env le = {0};
    le.user = &e->net;
    le.transport.user = &e->net;
    le.transport.start = fake_start;
    le.transport.abort = fake_abort;
    embi_loader_attach(e->c, &le);
}

static void env_done(env *e)
{
    embi_loader_detach(e->c);
    embi_bind_free(e->c);
    embi_js_runtime_free(e->rt);
    embi_document_free(e->doc);
}

/* Runs the script; false when it threw. */
static bool js_run(env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "loader.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e->rt);
        return false;
    }
    return true;
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

/* Microtasks, then held completions, until neither produces more. */
static void settle(env *e)
{
    for (int round = 0; round < 64; round++) {
        embi_js_run_jobs(e->rt);
        bool any = false;
        for (int i = 0; i < FAKE_MAX_PENDING; i++) {
            fake_pending *p = &e->net.pending[i];
            if (p->live) {
                p->live = false;
                fake_serve(&e->net, p->op, p->route);
                any = true;
            }
        }
        if (!any) {
            embi_js_run_jobs(e->rt);
            return;
        }
    }
}

/* Logs the string form of an expression (for diagnosing a failed check). */
[[maybe_unused]] static void js_dump(env *e, const char *expr)
{
    char src[512];
    SDL_snprintf(src, sizeof src, "(function(){try{return JSON.stringify(%s);}catch(x){return 'threw '+x;}})()", expr);
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "dump.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e->rt);
        SDL_Log("    dump %s: exception", expr);
        return;
    }
    embi_dstr d;
    if (!embi_bind_to_dstr(e->c, v, &d)) {
        embi_js_exception_clear(e->rt);
        return;
    }
    embi_str s = embi_bind_utf8(e->c, d);
    SDL_Log("    dump %s = %.*s", expr, (int)s.len, s.ptr);
}

/* Every test must return embi_mem_live to where it started. One context is made and freed first, so what the engine and the
 * bindings allocate once per process (tables, caches) is part of the baseline. */
static int bind_loader_main(void)
{
    {
        env warm;
        env_init(&warm);
        (void)js_run(&warm, "fetch('data:,x').then(function (r) { return r.text(); }); new Blob(['x']).text(); localStorage.length;");
        settle(&warm);
        env_done(&warm);
    }
    size_t base = embi_mem_live();
    int rc = emb_test_run_all();
    size_t end = embi_mem_live();
    if (end != base) {
        fprintf(stderr, "LEAK: %llu bytes still live after the run (baseline %llu)\n", (unsigned long long)end,
                (unsigned long long)base);
        rc = 1;
    }
    return rc;
}

#define BIND_LOADER_MAIN()                                                                                             \
    int main(int argc, char **argv)                                                                                    \
    {                                                                                                                  \
        (void)argc;                                                                                                    \
        (void)argv;                                                                                                    \
        return bind_loader_main();                                                                                     \
    }
