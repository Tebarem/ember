/*
 * MessagePort core (msgport.h): entangled pairs, the message queue, start/stop, close, ids and the registry.
 */
#include "loader_test.h"

#include "loader/msgport.h"

#include <string.h>

static embi_sb msg(const char *text)
{
    embi_sb sb = {0};
    embi_sb_append(&sb, (embi_str){text, strlen(text)});
    return sb;
}

static bool take_is(embi_mport *p, const char *text)
{
    embi_sb m = {0};
    if (!embi_mp_take(p, &m)) {
        return false;
    }
    bool ok = m.len == strlen(text) && memcmp(m.data, text, m.len) == 0;
    embi_sb_free(&m);
    return ok;
}

EMB_TEST(msgport_channel_makes_an_entangled_pair)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    EMB_CHECK(a != nullptr && b != nullptr && a != b);
    EMB_CHECK(embi_mp_peer(a) == b && embi_mp_peer(b) == a);
    EMB_CHECK(embi_mp_id(a) == 1 && embi_mp_id(b) == 2);
    EMB_CHECK(embi_mp_find(r, 1) == a && embi_mp_find(r, 2) == b);
    EMB_CHECK(embi_mp_find(r, 0) == nullptr && embi_mp_find(r, 3) == nullptr);
    EMB_CHECK(!embi_mp_is_started(a) && !embi_mp_is_closed(a) && embi_mp_queued(a) == 0);
    embi_mports_free(r);
}

EMB_TEST(msgport_release_unlinks_a_port_and_keeps_the_rest_findable)
{
    embi_mports *r = embi_mports_new();
    size_t base = embi_mem_live();
    for (int i = 0; i < 1000; i++) {
        embi_mport *b = nullptr;
        embi_mport *a = embi_mp_channel(r, &b);
        embi_mp_release(r, a);
        embi_mp_release(r, b);
    }
    EMB_CHECK(embi_mem_live() <= base + 4096);
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    embi_mport *to = nullptr;
    embi_sb m = msg("x");
    EMB_CHECK(embi_mp_post(a, &m, &to) == EMBI_MP_QUEUED && to == b);
    uint32_t ia = embi_mp_id(a);
    embi_mp_release(r, a);
    EMB_CHECK(embi_mp_find(r, ia) == nullptr && embi_mp_find(r, embi_mp_id(b)) == b);
    EMB_CHECK(embi_mp_peer(b) == nullptr && embi_mp_queued(b) == 1);
    embi_mports_free(r);
}

EMB_TEST(msgport_messages_wait_for_start_and_keep_order)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    embi_mport *to = nullptr;
    embi_sb m1 = msg("one"), m2 = msg("two");
    EMB_CHECK(embi_mp_post(a, &m1, &to) == EMBI_MP_QUEUED && to == b && m1.data == nullptr);
    EMB_CHECK(embi_mp_post(a, &m2, &to) == EMBI_MP_QUEUED);
    EMB_CHECK(embi_mp_queued(b) == 2);
    embi_sb out = {0};
    EMB_CHECK(!embi_mp_take(b, &out));
    EMB_CHECK(embi_mp_start(b) == 2 && embi_mp_is_started(b));
    EMB_CHECK(embi_mp_start(b) == 0);
    embi_sb m3 = msg("three");
    EMB_CHECK(embi_mp_post(a, &m3, &to) == EMBI_MP_SCHEDULE);
    EMB_CHECK(take_is(b, "one") && take_is(b, "two") && take_is(b, "three"));
    EMB_CHECK(!embi_mp_take(b, &out) && embi_mp_queued(b) == 0);
    embi_mports_free(r);
}

EMB_TEST(msgport_close_drops_the_queue_and_disentangles)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    embi_mport *to = nullptr;
    embi_sb m1 = msg("lost");
    EMB_CHECK(embi_mp_post(a, &m1, &to) == EMBI_MP_QUEUED);
    embi_mp_close(b);
    EMB_CHECK(embi_mp_is_closed(b) && embi_mp_queued(b) == 0);
    EMB_CHECK(embi_mp_peer(a) == nullptr && embi_mp_peer(b) == nullptr);
    embi_sb m2 = msg("dropped");
    EMB_CHECK(embi_mp_post(a, &m2, &to) == EMBI_MP_DROPPED);
    embi_sb_free(&m2);
    EMB_CHECK(embi_mp_start(b) == 0);
    embi_mp_close(b);
    embi_mp_close(a);
    embi_mports_free(r);
}

EMB_TEST(msgport_stop_disables_the_queue_until_the_next_start)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    embi_mport *to = nullptr;
    EMB_CHECK(embi_mp_start(b) == 0);
    embi_sb m1 = msg("x");
    EMB_CHECK(embi_mp_post(a, &m1, &to) == EMBI_MP_SCHEDULE);
    embi_mp_stop(b);
    embi_sb out = {0};
    EMB_CHECK(!embi_mp_take(b, &out));
    EMB_CHECK(embi_mp_start(b) == 1 && take_is(b, "x"));
    embi_mports_free(r);
}

static void count_each(embi_mport *p, void *arg)
{
    (void)p;
    ++*(int *)arg;
}

EMB_TEST(msgport_registry_visits_ports_and_frees_queued_messages)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b1 = nullptr, *b2 = nullptr;
    embi_mport *a1 = embi_mp_channel(r, &b1);
    (void)embi_mp_channel(r, &b2);
    embi_mport *to = nullptr;
    embi_sb m = msg("left in the queue");
    (void)embi_mp_post(a1, &m, &to);
    int n = 0;
    embi_mports_each(r, count_each, &n);
    EMB_CHECK(n == 4);
    embi_mp_set_slot(a1, 7);
    EMB_CHECK(embi_mp_slot(a1) == 7 && embi_mp_slot(b1) == 0);
    embi_mports_free(r);
}

/* IF1: a registry shared by the realms of several frames: each port names the context it belongs to, and the registry
 * lives until the last reference goes (the leak checker sees the free). */
EMB_TEST(msgport_shared_registry_owner_and_refs)
{
    embi_mports *r = embi_mports_new();
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(r, &b);
    int x = 0, y = 0;
    EMB_CHECK(embi_mp_owner(a) == nullptr && embi_mp_owner(b) == nullptr);
    embi_mp_set_owner(a, &x);
    embi_mp_set_owner(b, &y);
    EMB_CHECK(embi_mp_owner(a) == &x && embi_mp_owner(b) == &y);
    embi_mports_retain(r);
    embi_mports_retain(r);
    embi_mports_unref(r);
    embi_mports_unref(r);
    EMB_CHECK(embi_mp_find(r, embi_mp_id(a)) == a); /* one reference left: still there */
    embi_mports_unref(r);
    embi_mports_unref(nullptr);
}

LOADER_TEST_MAIN()
