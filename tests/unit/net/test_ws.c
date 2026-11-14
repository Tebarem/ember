/*
 * WebSocket client tests (N1c) against the 127.0.0.1 test server (plain and TLS): the handshake and its checks, text and
 * binary messages of every length class, fragmentation, ping and pong, the closing handshake and its codes, every kind
 * of invalid frame, limits, abort, and shutdown. Events are delivered through a task queue that stands for the receiving
 * thread, like the poster of a view.
 */
#include "ws_test.h"

static void finish_and_check(wts *s, wc *w)
{
    EMB_CHECK(wc_wait(w, WC_RELEASE, 1, 5000));
    EMB_CHECK(!w->order_bad);
    if (w->n > 0) {
        EMB_CHECK_EQ_INT(w->ev[w->n - 1].kind, WC_RELEASE);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s->x.unmasked), 0);
}

static void teardown(wts *s, wc *w)
{
    wts_stop(s);
    wc_free(w);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

static bool has(const char *hay, const char *needle)
{
    return hay != nullptr && strstr(hay, needle) != nullptr;
}

/* The close event: wait for it and check it. */
static const wc_ev *expect_close(wc *w, bool clean, int code)
{
    if (!wc_wait(w, WC_CLOSE, 1, 8000)) {
        emb_test_report_fail(__FILE__, __LINE__, "no close event");
        return nullptr;
    }
    const wc_ev *e = wc_nth(w, WC_CLOSE, 0);
    EMB_CHECK_EQ_INT(e->clean, clean);
    EMB_CHECK_EQ_INT(e->code, code);
    EMB_CHECK_EQ_INT(wc_count(w, WC_CLOSE), 1);
    return e;
}

EMB_TEST(accept_key_matches_the_rfc_example)
{
    char out[32];
    embi_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out);
    EMB_CHECK_EQ_STR(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    /* a key long enough that the padded block spills into a second one */
    embi_ws_accept_key("AAAAAAAAAAAAAAAAAAAAAA==", out);
    EMB_CHECK_EQ_INT(strlen(out), 28);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

/* ------------------------------------------------------------------ messages */

static void scenario_echo(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/echo"));
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_OPEN, 0)->data, "");
    EMB_CHECK_EQ_STR(s.x.req_version, "13");
    EMB_CHECK_EQ_INT(strlen(s.x.req_key), 24);
    EMB_CHECK_EQ_STR(s.x.req_extensions, "(none)");

    EMB_CHECK(embi_ws_send(w.req, false, "hello", 5));
    EMB_CHECK(embi_ws_send(w.req, true, "\x00\x01\x02\xFF", 4));
    EMB_CHECK(embi_ws_send(w.req, false, "", 0));
    const char *uni = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80";
    EMB_CHECK(embi_ws_send(w.req, false, uni, strlen(uni)));
    uint8_t med[300], big[70000];
    for (size_t i = 0; i < sizeof med; i++) {
        med[i] = (uint8_t)(i * 3);
    }
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i * 11 + 1);
    }
    EMB_CHECK(embi_ws_send(w.req, true, med, sizeof med));
    EMB_CHECK(embi_ws_send(w.req, true, big, sizeof big));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 6, 10000));
    const wc_ev *e = wc_nth(&w, WC_MESSAGE, 0);
    EMB_CHECK(!e->binary && e->len == 5 && memcmp(e->data, "hello", 5) == 0);
    e = wc_nth(&w, WC_MESSAGE, 1);
    EMB_CHECK(e->binary && e->len == 4 && memcmp(e->data, "\x00\x01\x02\xFF", 4) == 0);
    e = wc_nth(&w, WC_MESSAGE, 2);
    EMB_CHECK(!e->binary && e->len == 0);
    e = wc_nth(&w, WC_MESSAGE, 3);
    EMB_CHECK(!e->binary && e->len == strlen(uni) && memcmp(e->data, uni, e->len) == 0);
    e = wc_nth(&w, WC_MESSAGE, 4);
    EMB_CHECK(e->binary && e->len == sizeof med && memcmp(e->data, med, e->len) == 0);
    e = wc_nth(&w, WC_MESSAGE, 5);
    EMB_CHECK(e->binary && e->len == sizeof big && memcmp(e->data, big, e->len) == 0);

    embi_ws_close(w.req, 1000, "bye", 3);
    e = expect_close(&w, true, 1000);
    EMB_CHECK(e != nullptr && strcmp(e->data, "bye") == 0);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 0);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.close_code), 1000);
    EMB_CHECK_EQ_STR(s.x.close_reason, "bye");
    finish_and_check(&s, &w);
    EMB_CHECK_EQ_INT(w.ev[0].kind, WC_OPEN);
    teardown(&s, &w);
}

EMB_TEST(ws_echo_text_and_binary) { scenario_echo(false); }
EMB_TEST(wss_echo_text_and_binary) { scenario_echo(true); }

static void scenario_big_server_message(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/send/1048576"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 1, 15000));
    const wc_ev *e = wc_nth(&w, WC_MESSAGE, 0);
    EMB_REQUIRE(e->binary && e->len == 1048576);
    bool ok = true;
    for (size_t i = 0; i < e->len && ok; i++) {
        ok = (uint8_t)e->data[i] == (uint8_t)(i * 7 + 3);
    }
    EMB_CHECK(ok);
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_one_megabyte_message_from_the_server) { scenario_big_server_message(false); }
EMB_TEST(wss_one_megabyte_message_from_the_server) { scenario_big_server_message(true); }

static void scenario_flood(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/flood/300"));
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 15000));
    EMB_REQUIRE(wc_count(&w, WC_MESSAGE) == 300);
    bool ok = true;
    for (int i = 0; i < 300; i++) {
        char m[32];
        SDL_snprintf(m, sizeof m, "m%d", i);
        ok = ok && strcmp(wc_nth(&w, WC_MESSAGE, i)->data, m) == 0;
    }
    EMB_CHECK(ok);
    const wc_ev *c = wc_nth(&w, WC_CLOSE, 0);
    EMB_CHECK(c->clean && c->code == 1000 && strcmp(c->data, "done") == 0);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_messages_arrive_in_order) { scenario_flood(false); }
EMB_TEST(wss_messages_arrive_in_order) { scenario_flood(true); }

static void scenario_fragments(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/fragment"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 2, 8000));
    const wc_ev *e = wc_nth(&w, WC_MESSAGE, 0);
    EMB_CHECK(!e->binary && e->len == 11 && memcmp(e->data, "Hello World", 11) == 0); /* with a ping in the middle */
    e = wc_nth(&w, WC_MESSAGE, 1);
    EMB_CHECK(e->binary && e->len == 3 && memcmp(e->data, "\x01\x02\x03", 3) == 0); /* an empty continuation inside */
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_fragmented_messages) { scenario_fragments(false); }
EMB_TEST(wss_fragmented_messages) { scenario_fragments(true); }

EMB_TEST(utf8_sequences_may_span_frames)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/utf8split"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 2, 8000));
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_MESSAGE, 0)->data, "\xE2\x82\xAC");
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_MESSAGE, 1)->data, "\xF0\x9F\x98\x80");
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

static void scenario_ping(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/ping"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 1, 8000));
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_MESSAGE, 0)->data, "after-ping");
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.pong_ok), 1); /* the pong carried the ping payload, masked */
    EMB_CHECK_EQ_INT(wc_count(&w, WC_MESSAGE), 1);        /* the server pong is not a message */
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_ping_is_answered_with_pong) { scenario_ping(false); }
EMB_TEST(wss_ping_is_answered_with_pong) { scenario_ping(true); }

/* ------------------------------------------------------------------ closing */

EMB_TEST(a_ping_flood_costs_few_pongs)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/pingflood/30000"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 1, 15000));
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_MESSAGE, 0)->data, "end");
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    int pongs = SDL_GetAtomicInt(&s.x.pongs);
    EMB_CHECK(pongs >= 1);   /* the latest ping is answered */
    EMB_CHECK(pongs < 3000); /* ... not every one of 30000 */
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(a_flood_of_empty_frames_is_bounded_while_the_consumer_is_busy)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/tiny/100000"));
    /* The receiving thread does not run: every event waits in its queue. */
    uint64_t end = SDL_GetTicks() + 1500;
    int peak = 0;
    while (SDL_GetTicks() < end) {
        SDL_Delay(20);
        int n = tq_len(s.q);
        peak = n > peak ? n : peak;
    }
    EMB_CHECK(peak > 1000);  /* it did fill up to the limit */
    EMB_CHECK(peak < 40000); /* about 4 MiB / 256 bytes of events, not 100000 */
    /* Now it runs: everything arrives, in order, with the last message last. */
    uint64_t t_end = SDL_GetTicks() + 30000;
    while (w.msgs < 100001 && SDL_GetTicks() < t_end) {
        (void)tq_step(s.q, 10);
    }
    EMB_CHECK_EQ_INT(w.msgs, 100001);
    EMB_CHECK_EQ_INT(w.nonempty_msgs, 1);
    EMB_CHECK(!w.order_bad);
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(client_close_without_code_sends_an_empty_close_frame)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/echo"));
    embi_ws_close(w.req, 0, nullptr, 0);
    expect_close(&w, true, 1005);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.close_code), 1005);
    EMB_CHECK(!embi_ws_send(w.req, false, "late", 4)); /* nothing can be queued after close */
    embi_ws_close(w.req, 1000, "again", 5);             /* a second close is ignored */
    finish_and_check(&s, &w);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_CLOSE), 1);
    teardown(&s, &w);
}

EMB_TEST(messages_sent_before_close_are_delivered_first)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/sink"));
    embi_ws_send(w.req, false, "one", 3);
    embi_ws_send(w.req, true, "two", 3);
    embi_ws_close(w.req, 3000, "custom", 6);
    expect_close(&w, true, 3000);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.sink_msgs), 2);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.close_code), 3000);
    EMB_CHECK_EQ_STR(s.x.close_reason, "custom");
    EMB_CHECK_EQ_INT(embi_ws_sent(w.req), 6);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

static void scenario_server_close(bool tls, const char *path, int code, const char *reason, int echoed)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, path));
    const wc_ev *e = expect_close(&w, true, code);
    EMB_CHECK(e != nullptr && strcmp(e->data, reason) == 0);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 0);
    for (int i = 0; i < 300 && SDL_GetAtomicInt(&s.x.close_code) == 0; i++) {
        SDL_Delay(10); /* the server notes the echo when it reads it */
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.close_code), echoed); /* the client echoed the status code */
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_server_initiated_close_with_code_and_reason) { scenario_server_close(false, "/close/3001", 3001, "bye", 3001); }
EMB_TEST(wss_server_initiated_close_with_code_and_reason) { scenario_server_close(true, "/close/1001", 1001, "bye", 1001); }
EMB_TEST(server_close_without_a_status_is_1005) { scenario_server_close(false, "/closenocode", 1005, "", 1005); }

static void scenario_protocol_error(const char *path, int expect_sent, uint32_t max_message, const char *needle)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    embi_ws_request r = wc_request(&w, wts_url(&s, path));
    r.max_message = max_message;
    w.req = embi_ws_connect(s.net, &r);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 8000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_OPEN), 1);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_MESSAGE), 0);
    EMB_REQUIRE(wc_count(&w, WC_ERROR) == 1);
    EMB_CHECK(has(wc_nth(&w, WC_ERROR, 0)->data, needle));
    const wc_ev *c = wc_nth(&w, WC_CLOSE, 0);
    EMB_CHECK(!c->clean && c->code == 1006);
    /* the close frame the client sent before dropping the connection */
    for (int i = 0; i < 100 && SDL_GetAtomicInt(&s.x.close_code) == 0; i++) {
        SDL_Delay(20);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.close_code), expect_sent);
    finish_and_check(&s, &w);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_CLOSE), 1);
    teardown(&s, &w);
}

EMB_TEST(invalid_utf8_in_a_text_message_fails_with_1007) { scenario_protocol_error("/badutf8", 1007, 0, "UTF-8"); }
EMB_TEST(invalid_utf8_is_found_in_the_first_fragment) { scenario_protocol_error("/badutf8frag", 1007, 0, "UTF-8"); }
EMB_TEST(a_text_message_cut_inside_a_sequence_fails_with_1007) { scenario_protocol_error("/truncutf8", 1007, 0, "UTF-8"); }
EMB_TEST(overlong_utf8_fails_with_1007) { scenario_protocol_error("/overlong", 1007, 0, "UTF-8"); }
EMB_TEST(utf8_surrogate_fails_with_1007) { scenario_protocol_error("/surrogate", 1007, 0, "UTF-8"); }
EMB_TEST(utf8_above_10ffff_fails_with_1007) { scenario_protocol_error("/toolarge", 1007, 0, "UTF-8"); }
EMB_TEST(a_masked_server_frame_fails_with_1002) { scenario_protocol_error("/masked", 1002, 0, "masked"); }
EMB_TEST(reserved_bits_fail_with_1002) { scenario_protocol_error("/rsv", 1002, 0, "reserved"); }
EMB_TEST(an_unknown_opcode_fails_with_1002) { scenario_protocol_error("/badopcode", 1002, 0, "opcode"); }
EMB_TEST(a_long_control_frame_fails_with_1002) { scenario_protocol_error("/ctlbig", 1002, 0, "control"); }
EMB_TEST(a_fragmented_control_frame_fails_with_1002) { scenario_protocol_error("/ctlfrag", 1002, 0, "control"); }
EMB_TEST(a_continuation_with_nothing_to_continue_fails_with_1002) { scenario_protocol_error("/contfirst", 1002, 0, "continuation"); }
EMB_TEST(a_new_message_inside_a_fragmented_one_fails_with_1002) { scenario_protocol_error("/newwhile", 1002, 0, "new message"); }
EMB_TEST(close_code_1005_on_the_wire_fails_with_1002) { scenario_protocol_error("/closeraw/1005", 1002, 0, "close code"); }
EMB_TEST(close_code_below_1000_fails_with_1002) { scenario_protocol_error("/closeraw/999", 1002, 0, "close code"); }
EMB_TEST(close_code_1016_fails_with_1002) { scenario_protocol_error("/closeraw/1016", 1002, 0, "close code"); }
EMB_TEST(a_one_byte_close_payload_fails_with_1002) { scenario_protocol_error("/close1byte", 1002, 0, "one byte"); }
EMB_TEST(an_invalid_close_reason_fails_with_1007) { scenario_protocol_error("/closebadreason", 1007, 0, "UTF-8"); }
EMB_TEST(a_length_with_the_top_bit_set_fails_with_1002) { scenario_protocol_error("/lenmsb", 1002, 0, "length"); }
EMB_TEST(an_oversized_message_fails_with_1009) { scenario_protocol_error("/big", 1009, 1000, "too big"); }
EMB_TEST(fragments_that_add_up_past_the_limit_fail_with_1009) { scenario_protocol_error("/bigfrags", 1009, 500, "too big"); }
EMB_TEST(a_huge_announced_frame_fails_with_1009_before_it_arrives) { scenario_protocol_error("/hugeframe", 1009, 1000, "too big"); }

EMB_TEST(a_dropped_connection_closes_with_1006_and_no_error_event)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/drop"));
    expect_close(&w, false, 1006);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 0);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(a_tls_connection_cut_without_close_notify_closes_with_1006)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, true));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/drop"));
    expect_close(&w, false, 1006);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 0);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

/* ------------------------------------------------------------------ the handshake */

static void scenario_handshake_failure(bool tls, const char *path, const char *protocol, const char *needle)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    wc w;
    wc_init(&w, s.q);
    const char *protos[] = {protocol};
    embi_ws_request r = wc_request(&w, wts_url(&s, path));
    r.protocols = protocol != nullptr ? protos : nullptr;
    r.nprotocols = protocol != nullptr ? 1 : 0;
    w.req = embi_ws_connect(s.net, &r);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 8000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_OPEN), 0);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_MESSAGE), 0);
    EMB_REQUIRE(wc_count(&w, WC_ERROR) == 1);
    EMB_CHECK(has(wc_nth(&w, WC_ERROR, 0)->data, needle));
    const wc_ev *c = wc_nth(&w, WC_CLOSE, 0);
    EMB_CHECK(!c->clean && c->code == 1006);
    /* error before close, release last */
    EMB_CHECK(w.ev[0].kind == WC_ERROR && w.ev[1].kind == WC_CLOSE);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(handshake_with_a_wrong_accept_key_fails) { scenario_handshake_failure(false, "/badaccept", nullptr, "Sec-WebSocket-Accept"); }
EMB_TEST(handshake_over_tls_with_a_wrong_accept_key_fails) { scenario_handshake_failure(true, "/badaccept", nullptr, "Sec-WebSocket-Accept"); }
EMB_TEST(handshake_answered_with_200_fails) { scenario_handshake_failure(false, "/plain", nullptr, "status 200"); }
EMB_TEST(handshake_answered_with_403_fails) { scenario_handshake_failure(false, "/status403", nullptr, "status 403"); }
EMB_TEST(handshake_answered_with_a_redirect_fails) { scenario_handshake_failure(false, "/redirect", nullptr, "status 302"); }
EMB_TEST(handshake_without_the_upgrade_header_fails) { scenario_handshake_failure(false, "/noupgradehdr", nullptr, "Upgrade"); }
EMB_TEST(handshake_without_the_connection_header_fails) { scenario_handshake_failure(false, "/noconnhdr", nullptr, "Connection"); }
EMB_TEST(handshake_with_an_unoffered_subprotocol_fails) { scenario_handshake_failure(false, "/badproto", "chat", "subprotocol"); }
EMB_TEST(handshake_with_a_subprotocol_nobody_asked_for_fails) { scenario_handshake_failure(false, "/badproto", nullptr, "subprotocol"); }
EMB_TEST(handshake_with_two_subprotocols_fails) { scenario_handshake_failure(false, "/twoprotos", "chat", "subprotocol"); }
EMB_TEST(handshake_with_an_extension_fails) { scenario_handshake_failure(false, "/ext", nullptr, "extension"); }

EMB_TEST(subprotocol_negotiation_and_request_headers)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    const char *protos[] = {"chat", "other"};
    embi_ws_request r = wc_request(&w, wts_url(&s, "/proto"));
    r.protocols = protos;
    r.nprotocols = 2;
    r.origin = "app://example";
    w.req = embi_ws_connect(s.net, &r);
    EMB_REQUIRE(wc_wait(&w, WC_OPEN, 1, 8000));
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_OPEN, 0)->data, "chat");
    EMB_CHECK_EQ_STR(s.x.req_proto, "chat, other");
    EMB_CHECK_EQ_STR(s.x.req_origin, "app://example");
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(no_protocol_header_when_none_is_offered)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/proto"));
    EMB_CHECK_EQ_STR(s.x.req_proto, "(none)");
    EMB_CHECK_EQ_STR(s.x.req_origin, "(none)");
    EMB_CHECK_EQ_STR(wc_nth(&w, WC_OPEN, 0)->data, "");
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

static void scenario_cookies(bool tls)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, tls));
    char u[200];
    SDL_snprintf(u, sizeof u, "%s://127.0.0.1:%d/", tls ? "https" : "http", s.srv->port);
    embi_net_cookie_set(s.net, u, "sid=abc; Path=/");
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/info"));
    EMB_REQUIRE(wc_wait(&w, WC_MESSAGE, 1, 8000));
    EMB_CHECK(has(wc_nth(&w, WC_MESSAGE, 0)->data, "cookie=sid=abc;"));
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(ws_sends_the_cookies_of_the_jar) { scenario_cookies(false); }
EMB_TEST(wss_sends_the_cookies_of_the_jar) { scenario_cookies(true); }

EMB_TEST(the_handshake_response_may_set_cookies)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/setcookie"));
    embi_sb out = {0};
    char u[200];
    SDL_snprintf(u, sizeof u, "http://127.0.0.1:%d/", s.srv->port);
    embi_net_cookie_get(s.net, u, &out);
    EMB_CHECK(out.len == 4 && memcmp(out.data, "ws=1", 4) == 0);
    embi_sb_free(&out);
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(a_tls_server_with_an_untrusted_certificate_fails_the_connection)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, true));
    embi_net *n2 = embi_net_create(nullptr, nullptr); /* trusts nothing */
    embi_net_allow_add(n2, "127.0.0.1");
    embi_net_use_bundled_roots(n2, false);
    wc w;
    wc_init(&w, s.q);
    embi_ws_request r = wc_request(&w, wts_url(&s, "/echo"));
    w.req = embi_ws_connect(n2, &r);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 10000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_OPEN), 0);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 1);
    EMB_CHECK(!wc_nth(&w, WC_CLOSE, 0)->clean && wc_nth(&w, WC_CLOSE, 0)->code == 1006);
    EMB_CHECK(wc_wait(&w, WC_RELEASE, 1, 5000));
    embi_net_destroy(n2);
    teardown(&s, &w);
}

EMB_TEST(a_host_off_the_allowlist_is_not_contacted)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    embi_net *n2 = embi_net_create(nullptr, nullptr);
    embi_net_allow_add(n2, "example.org");
    wc w;
    wc_init(&w, s.q);
    embi_ws_request r = wc_request(&w, wts_url(&s, "/echo"));
    w.req = embi_ws_connect(n2, &r);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 8000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_OPEN), 0);
    EMB_REQUIRE(wc_count(&w, WC_ERROR) == 1);
    EMB_CHECK(has(wc_nth(&w, WC_ERROR, 0)->data, "allowlist"));
    EMB_CHECK(wc_wait(&w, WC_RELEASE, 1, 5000));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.srv->accepted), 0);
    embi_net_destroy(n2);
    teardown(&s, &w);
}

EMB_TEST(a_refused_connection_and_a_bad_url_are_errors_not_crashes)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    /* a port nobody listens on: the server's port plus one is a fair guess */
    char u[100];
    SDL_snprintf(u, sizeof u, "ws://127.0.0.1:%d/", s.srv->port == 65535 ? 1 : 65535);
    embi_ws_request r = wc_request(&w, u);
    w.req = embi_ws_connect(s.net, &r);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 15000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 1);
    EMB_CHECK(!wc_nth(&w, WC_CLOSE, 0)->clean);
    EMB_CHECK(wc_wait(&w, WC_RELEASE, 1, 5000));
    wc w2;
    wc_init(&w2, s.q);
    embi_ws_request r2 = wc_request(&w2, "gopher://127.0.0.1/x");
    w2.req = embi_ws_connect(s.net, &r2);
    EMB_REQUIRE(wc_wait(&w2, WC_CLOSE, 1, 8000));
    EMB_CHECK_EQ_INT(wc_count(&w2, WC_ERROR), 1);
    EMB_CHECK(wc_wait(&w2, WC_RELEASE, 1, 5000));
    wts_stop(&s);
    wc_free(&w2);
    wc_free(&w);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

/* ------------------------------------------------------------------ sending, buffering, aborting */

EMB_TEST(sent_bytes_count_what_reached_the_socket)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/sink"));
    uint8_t *big = SDL_calloc(1, 100000);
    EMB_CHECK_EQ_INT(embi_ws_sent(w.req), 0);
    embi_ws_send(w.req, false, "0123456789", 10);
    embi_ws_send(w.req, true, big, 100000);
    embi_ws_send(w.req, false, "abcde", 5);
    SDL_free(big);
    for (int i = 0; i < 300 && embi_ws_sent(w.req) < 100015; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(embi_ws_sent(w.req), 100015);
    for (int i = 0; i < 300 && SDL_GetAtomicInt(&s.x.sink_msgs) < 3; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.sink_msgs), 3);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.sink_bytes), 100015);
    embi_ws_close(w.req, 1000, "", 0);
    expect_close(&w, true, 1000);
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(close_before_the_connection_is_open_fails_it)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    embi_ws_request r = wc_request(&w, wts_url(&s, "/echo"));
    w.req = embi_ws_connect(s.net, &r);
    embi_ws_close(w.req, 1000, "early", 5);
    EMB_REQUIRE(wc_wait(&w, WC_CLOSE, 1, 8000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_CLOSE), 1);
    if (wc_count(&w, WC_OPEN) == 0) { /* the usual case: the connection never opened */
        EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 1);
        EMB_CHECK(!wc_nth(&w, WC_CLOSE, 0)->clean && wc_nth(&w, WC_CLOSE, 0)->code == 1006);
    } else { /* the handshake won the race: an ordinary closing handshake */
        EMB_CHECK(wc_nth(&w, WC_CLOSE, 0)->clean && wc_nth(&w, WC_CLOSE, 0)->code == 1000);
    }
    finish_and_check(&s, &w);
    teardown(&s, &w);
}

EMB_TEST(abort_drops_the_connection_and_stops_the_events)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/hold"));
    embi_net_abort(w.req);
    EMB_CHECK(wc_wait(&w, WC_RELEASE, 1, 5000));
    for (int i = 0; i < 200 && SDL_GetAtomicInt(&s.x.disconnects) == 0; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.disconnects), 1); /* the server saw the connection end without a close frame */
    EMB_CHECK_EQ_INT(wc_count(&w, WC_CLOSE), 0);
    EMB_CHECK_EQ_INT(wc_count(&w, WC_ERROR), 0);
    EMB_CHECK(!embi_ws_send(w.req, false, "x", 1) || true); /* sending to an aborted socket is harmless */
    teardown(&s, &w);
}

EMB_TEST(destroying_the_service_aborts_open_sockets)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    wc w;
    wc_init(&w, s.q);
    EMB_CHECK(wts_open(&s, &w, "/hold"));
    embi_net_destroy(s.net); /* the runtime goes away */
    SDL_DestroyMutex(s.log.m);
    s.net = nullptr;
    EMB_CHECK(wc_wait(&w, WC_RELEASE, 1, 5000));
    EMB_CHECK_EQ_INT(wc_count(&w, WC_CLOSE), 0);
    for (int i = 0; i < 200 && SDL_GetAtomicInt(&s.x.disconnects) == 0; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.disconnects), 1);
    ts_stop(s.srv);
    tq_free(s.q);
    wsx_free(&s.x);
    wc_free(&w);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(sockets_do_not_use_up_the_connection_pool)
{
    wts s;
    EMB_REQUIRE(wts_start(&s, false));
    enum { N = 9 };
    wc w[N];
    for (int i = 0; i < N; i++) {
        wc_init(&w[i], s.q);
        embi_ws_request r = wc_request(&w[i], wts_url(&s, "/hold"));
        w[i].req = embi_ws_connect(s.net, &r);
    }
    for (int i = 0; i < N; i++) {
        EMB_CHECK(wc_wait(&w[i], WC_OPEN, 1, 10000));
    }
    /* and a plain request still goes through */
    tc t;
    tc_init(&t);
    char u[100];
    SDL_snprintf(u, sizeof u, "http://127.0.0.1:%d/plain", s.srv->port);
    embi_net_request r = tc_request(&t, "GET", u);
    EMB_CHECK(tc_run(s.net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.status, 200);
    tc_free(&t);
    for (int i = 0; i < N; i++) {
        embi_net_abort(w[i].req);
    }
    for (int i = 0; i < N; i++) {
        EMB_CHECK(wc_wait(&w[i], WC_RELEASE, 1, 5000));
    }
    wts_stop(&s);
    for (int i = 0; i < N; i++) {
        wc_free(&w[i]);
    }
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST_MAIN()
