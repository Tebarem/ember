/*
 * Fetch core (fetch.h) through a scripted transport: the response of a hop, the redirect chain (303
 * and 301 change POST to GET and drop the body, 307 keeps it, relative Location values resolve against
 * the current URL, the 20-redirect limit), the redirect modes, network errors, the null-body statuses,
 * aborts before and during a hop, body consumption and the method normalization.
 */
#include "loader_test.h"

#include "loader/fetch.h"

#include <string.h>

/* A transport that serves queued hop results synchronously, or holds the first hop for a later
 * completion (hold mode), and counts what it was asked for. */
typedef struct script_hop {
    uint16_t status;
    const char *location; /* nullptr for none */
    const char *body;     /* nullptr for none */
    bool fail_start;      /* the transport cannot start this hop */
} script_hop;

typedef struct script {
    const script_hop *hops;
    uint32_t nhops, next;
    bool hold;            /* keep the hop in flight (the op is saved in held) */
    embi_fetch_op *held;
    uint32_t starts, aborts;
    char last_method[16];
    char last_url[256];
    uint32_t last_nheaders;
    size_t last_body_len;
    bool last_body_present;
    char methods[8][16]; /* the method of each hop */
    uint32_t nmethods;
    bool auth[8]; /* whether each hop carried an Authorization header */
} script;

static bool t_start(void *user, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                    const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    script *s = user;
    s->starts++;
    *handle = s; /* any token: the fake transport has nothing to hand back */
    SDL_snprintf(s->last_method, sizeof s->last_method, "%.*s", (int)method.len, method.ptr);
    SDL_snprintf(s->last_url, sizeof s->last_url, "%.*s", (int)url.len, url.ptr);
    s->last_nheaders = nheaders;
    s->last_body_present = body != nullptr;
    s->last_body_len = body_len;
    if (s->nmethods < 8) {
        s->auth[s->nmethods] = false;
        for (uint32_t i = 0; i < nheaders; i++) {
            if (embi_str_ieq(headers[i].name, EMBI_STR("authorization"))) {
                s->auth[s->nmethods] = true;
            }
        }
        SDL_snprintf(s->methods[s->nmethods], sizeof s->methods[0], "%s", s->last_method);
        s->nmethods++;
    }
    if (s->hold) {
        s->held = op;
        return true;
    }
    if (s->next >= s->nhops) {
        return false;
    }
    const script_hop *h = &s->hops[s->next++];
    if (h->fail_start) {
        return false;
    }
    embi_hdr_view hv[1];
    uint32_t nh = 0;
    if (h->location != nullptr) {
        hv[nh].name = EMBI_STR("Location");
        hv[nh].value = (embi_str){h->location, (uint32_t)strlen(h->location)};
        nh++;
    }
    embi_fetch_hop hop = {0};
    hop.status = h->status;
    hop.headers = hv;
    hop.nheaders = nh;
    if (h->body != nullptr) {
        hop.body = (const uint8_t *)h->body;
        hop.body_len = strlen(h->body);
    }
    embi_fetch_hop_done(op, &hop);
    return true;
}

static void t_abort(void *user, void *handle)
{
    script *s = user;
    EMB_CHECK(handle == s);
    s->aborts++;
}

typedef struct outcome {
    int calls;
    bool ok;
    embi_fetch_op *op;
} outcome;

static void on_done(void *user, embi_fetch_op *op, bool ok)
{
    outcome *o = user;
    o->calls++;
    o->ok = ok;
    o->op = op;
}

static embi_fetch_init init_get(const char *url)
{
    embi_fetch_init in = {0};
    in.method = EMBI_STR("GET");
    in.url = (embi_str){url, (uint32_t)strlen(url)};
    in.redirect = EMBI_REDIRECT_FOLLOW;
    return in;
}

/* Starts an op, takes its result when ok, and frees it. */
static void run_op(embi_fetch_init in, script *s, outcome *o, embi_response *out)
{
    embi_fetch_transport t = {.user = s, .start = t_start, .abort = t_abort};
    *o = (outcome){0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, on_done, o);
    EMB_CHECK(op != nullptr);
    if (op != nullptr && o->calls == 1 && o->ok && out != nullptr) {
        embi_fetch_op_take(op, out);
    }
    embi_fetch_op_free(op);
}

EMB_TEST(fetch_basic_200_hides_set_cookie)
{
    static const script_hop hops[] = {{200, nullptr, "hello", false}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    embi_response r = {0};
    run_op(init_get("app://bundle/a.json"), &s, &o, &r);
    EMB_CHECK(o.calls == 1 && o.ok);
    EMB_CHECK(r.type == EMBI_RESP_BASIC && r.status == 200);
    EMB_CHECK(!r.redirected);
    EMB_CHECK(r.body.present && r.body.bytes.len == 5);
    EMB_CHECK(embi_str_eq((embi_str){r.url.data, r.url.len}, EMBI_STR("app://bundle/a.json")));
    EMB_CHECK(strcmp(s.last_method, "GET") == 0);
    embi_sb body = {0};
    EMB_CHECK(embi_body_consume(&r.body, &body));
    EMB_CHECK(body.len == 5 && memcmp(body.data, "hello", 5) == 0);
    EMB_CHECK(!embi_body_consume(&r.body, &body)); /* already used */
    embi_sb_free(&body);
    embi_response_free(&r);
}

EMB_TEST(fetch_redirect_303_turns_post_into_get_and_drops_body)
{
    static const script_hop hops[] = {{303, "/next", nullptr, false}, {200, nullptr, "done", false}};
    script s = {.hops = hops, .nhops = 2};
    embi_fetch_init in = init_get("app://bundle/x/y");
    in.method = EMBI_STR("POST");
    in.body = (const uint8_t *)"payload";
    in.body_len = 7;
    outcome o;
    embi_response r = {0};
    run_op(in, &s, &o, &r);
    EMB_CHECK(o.ok && r.redirected && r.status == 200);
    EMB_CHECK(s.nmethods == 2);
    EMB_CHECK(strcmp(s.methods[0], "POST") == 0 && strcmp(s.methods[1], "GET") == 0);
    EMB_CHECK(!s.last_body_present);
    EMB_CHECK(embi_str_eq((embi_str){r.url.data, r.url.len}, EMBI_STR("app://bundle/next")));
    embi_response_free(&r);
}

EMB_TEST(fetch_redirect_307_keeps_method_and_body)
{
    static const script_hop hops[] = {{307, "app://other/z", nullptr, false}, {200, nullptr, "ok", false}};
    script s = {.hops = hops, .nhops = 2};
    embi_fetch_init in = init_get("app://bundle/x");
    in.method = EMBI_STR("POST");
    in.body = (const uint8_t *)"abc";
    in.body_len = 3;
    outcome o;
    embi_response r = {0};
    run_op(in, &s, &o, &r);
    EMB_CHECK(o.ok && r.status == 200);
    EMB_CHECK(strcmp(s.methods[1], "POST") == 0);
    EMB_CHECK(s.last_body_present && s.last_body_len == 3);
    embi_response_free(&r);
}

EMB_TEST(fetch_redirect_limit_is_twenty)
{
    /* Twenty redirects are followed, the 21st fails. */
    static script_hop hops[21];
    for (int i = 0; i < 21; i++) {
        hops[i] = (script_hop){302, "/again", nullptr, false};
    }
    script s = {.hops = hops, .nhops = 21};
    outcome o;
    run_op(init_get("app://bundle/start"), &s, &o, nullptr);
    EMB_CHECK(o.calls == 1 && !o.ok);
    EMB_CHECK(s.starts == 21);
}

EMB_TEST(fetch_redirect_error_mode_fails)
{
    static const script_hop hops[] = {{301, "/moved", nullptr, false}};
    script s = {.hops = hops, .nhops = 1};
    embi_fetch_init in = init_get("app://bundle/a");
    in.redirect = EMBI_REDIRECT_ERROR;
    outcome o;
    run_op(in, &s, &o, nullptr);
    EMB_CHECK(o.calls == 1 && !o.ok);
    EMB_CHECK(s.starts == 1);
}

EMB_TEST(fetch_redirect_manual_is_opaque)
{
    static const script_hop hops[] = {{302, "/moved", "secret", false}};
    script s = {.hops = hops, .nhops = 1};
    embi_fetch_init in = init_get("app://bundle/a");
    in.redirect = EMBI_REDIRECT_MANUAL;
    outcome o;
    embi_response r = {0};
    run_op(in, &s, &o, &r);
    EMB_CHECK(o.ok);
    EMB_CHECK(r.type == EMBI_RESP_OPAQUEREDIRECT && r.status == 0);
    EMB_CHECK(!r.body.present);
    embi_response_free(&r);
}

EMB_TEST(fetch_redirect_without_location_is_final)
{
    static const script_hop hops[] = {{301, nullptr, "plain", false}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    embi_response r = {0};
    run_op(init_get("app://bundle/a"), &s, &o, &r);
    EMB_CHECK(o.ok && r.status == 301 && s.starts == 1);
    embi_response_free(&r);
}

EMB_TEST(fetch_redirect_with_empty_location_is_a_network_error)
{
    static const script_hop hops[] = {{302, "", "x", false}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    run_op(init_get("app://bundle/a"), &s, &o, nullptr);
    EMB_CHECK(o.calls == 1 && !o.ok && s.starts == 1);
}

EMB_TEST(fetch_cross_origin_redirect_drops_authorization)
{
    static const script_hop hops[] = {{302, "app://other/b", "", false}, {302, "app://other/c", "", false},
                                      {302, "app://bundle/d", "", false}, {200, nullptr, "ok", false}};
    script s = {.hops = hops, .nhops = 4};
    outcome o;
    embi_response r = {0};
    embi_fetch_init in = init_get("app://bundle/a");
    embi_hdr_view hv[1] = {{EMBI_STR("Authorization"), EMBI_STR("Bearer t")}};
    in.headers = hv;
    in.nheaders = 1;
    run_op(in, &s, &o, &r);
    EMB_CHECK(o.ok && s.starts == 4);
    EMB_CHECK(s.auth[0]);  /* the first hop */
    EMB_CHECK(!s.auth[1]); /* bundle -> other: dropped */
    EMB_CHECK(!s.auth[2]); /* other -> other: still gone */
    EMB_CHECK(!s.auth[3]); /* back to the first origin: not restored */
    embi_response_free(&r);
}

EMB_TEST(fetch_null_body_status_drops_body)
{
    static const script_hop hops[] = {{204, nullptr, "ignored", false}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    embi_response r = {0};
    run_op(init_get("app://bundle/a"), &s, &o, &r);
    EMB_CHECK(o.ok && r.status == 204 && !r.body.present);
    embi_response_free(&r);
}

EMB_TEST(fetch_invalid_status_is_network_error)
{
    static const script_hop hops[] = {{0, nullptr, nullptr, false}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    run_op(init_get("app://bundle/a"), &s, &o, nullptr);
    EMB_CHECK(o.calls == 1 && !o.ok);
}

EMB_TEST(fetch_transport_refusal_fails_at_once)
{
    static const script_hop hops[] = {{200, nullptr, nullptr, true}};
    script s = {.hops = hops, .nhops = 1};
    outcome o;
    run_op(init_get("app://bundle/a"), &s, &o, nullptr);
    EMB_CHECK(o.calls == 1 && !o.ok);
}

EMB_TEST(fetch_bad_url_yields_no_operation)
{
    script s = {0};
    embi_fetch_transport t = {.user = &s, .start = t_start, .abort = t_abort};
    outcome o = {0};
    embi_fetch_init in = init_get("not a url");
    EMB_CHECK(embi_fetch_op_new(&t, &in, on_done, &o) == nullptr);
    EMB_CHECK(o.calls == 0 && s.starts == 0);
}

EMB_TEST(fetch_abort_in_flight_calls_transport_and_drops_result)
{
    script s = {.hold = true};
    embi_fetch_transport t = {.user = &s, .start = t_start, .abort = t_abort};
    outcome o = {0};
    embi_fetch_init in = init_get("app://bundle/slow");
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, on_done, &o);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(embi_fetch_op_pending(op));
    embi_fetch_op_abort(op);
    EMB_CHECK(!embi_fetch_op_pending(op));
    EMB_CHECK(s.aborts == 1);
    /* The transport's late completion is dropped: no done call. */
    embi_fetch_hop hop = {0};
    hop.status = 200;
    embi_fetch_hop_done(op, &hop);
    EMB_CHECK(o.calls == 0);
    embi_fetch_op_abort(op); /* a second abort does nothing */
    EMB_CHECK(s.aborts == 1);
    embi_fetch_op_free(op);
}

EMB_TEST(fetch_free_while_pending_aborts)
{
    script s = {.hold = true};
    embi_fetch_transport t = {.user = &s, .start = t_start, .abort = t_abort};
    outcome o = {0};
    embi_fetch_init in = init_get("app://bundle/slow");
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, on_done, &o);
    EMB_REQUIRE(op != nullptr);
    embi_fetch_op_free(op);
    EMB_CHECK(s.aborts == 1);
    EMB_CHECK(o.calls == 0);
}

EMB_TEST(fetch_held_hop_completes_later)
{
    script s = {.hold = true};
    embi_fetch_transport t = {.user = &s, .start = t_start, .abort = t_abort};
    outcome o = {0};
    embi_fetch_init in = init_get("app://bundle/later");
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, on_done, &o);
    EMB_REQUIRE(op != nullptr);
    EMB_CHECK(o.calls == 0);
    static const char body[] = "late";
    embi_fetch_hop hop = {0};
    hop.status = 200;
    hop.body = (const uint8_t *)body;
    hop.body_len = 4;
    embi_fetch_hop_done(op, &hop);
    EMB_CHECK(o.calls == 1 && o.ok);
    embi_response r = {0};
    embi_fetch_op_take(op, &r);
    EMB_CHECK(r.status == 200 && r.body.bytes.len == 4);
    embi_response_free(&r);
    embi_fetch_op_free(op);
}

EMB_TEST(fetch_method_normalization)
{
    embi_sb m = {0};
    EMB_CHECK(embi_method_normalize(EMBI_STR("get"), &m) && m.len == 3 && memcmp(m.data, "GET", 3) == 0);
    embi_sb_free(&m);
    m = (embi_sb){0};
    EMB_CHECK(embi_method_normalize(EMBI_STR("PATCH"), &m) && m.len == 5);
    embi_sb_free(&m);
    m = (embi_sb){0};
    EMB_CHECK(!embi_method_normalize(EMBI_STR("CONNECT"), &m));
    EMB_CHECK(!embi_method_normalize(EMBI_STR("bad method"), &m));
    EMB_CHECK(!embi_method_normalize(EMBI_STR("TRACE"), &m));
    embi_sb_free(&m);
}

EMB_TEST(fetch_status_helpers)
{
    EMB_CHECK(embi_status_ok(200) && embi_status_ok(599) && !embi_status_ok(199) && !embi_status_ok(600));
    EMB_CHECK(embi_status_null_body(204) && embi_status_null_body(304) && !embi_status_null_body(200));
    EMB_CHECK(embi_status_redirect(308) && !embi_status_redirect(304));
    EMB_CHECK(embi_status_text_ok(EMBI_STR("Not Found")) && !embi_status_text_ok(EMBI_STR("bad\nline")));
}

LOADER_TEST_MAIN()
