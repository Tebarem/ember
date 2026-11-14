/*
 * The event loop of a view (W5c): tasks in queue order, timers in due order with a fake clock, the HTML nesting clamp, repeat
 * and clear, payload release, and posting from another thread. The loop knows no JavaScript, so the hooks here are plain C.
 */
#include "emb_test.h"

#include "base/clock.h"
#include "embed/script_int.h"

#include <stdint.h>
#include <string.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_thread.h>

/* A record of what ran: values in the order they ran, and the release and checkpoint counts. */
typedef struct trace {
    int v[64];
    int n;
    int released;
    int checkpoints;
    embi_loop *loop; /* the loop under test (the chain test makes timers from its fire hook) */
} trace;

static void push(trace *t, int x)
{
    if (t->n < 64) {
        t->v[t->n++] = x;
    }
}

static void fire_plain(void *user, void *payload)
{
    push(user, (int)(intptr_t)payload);
}

static void release_count(void *user, void *payload)
{
    (void)payload;
    ((trace *)user)->released++;
}

static void checkpoint_count(void *user)
{
    ((trace *)user)->checkpoints++;
}

static embi_loop *make_loop(trace *t, void (*fire)(void *, void *))
{
    embi_loop_hooks h = {.user = t, .fire = fire, .release = release_count, .checkpoint = checkpoint_count};
    embi_loop *l = embi_loop_new(&h);
    t->loop = l;
    return l;
}

static void mark_100(void *user)
{
    push(user, 100);
}

static void mark_101(void *user)
{
    push(user, 101);
}

EMB_TEST(loop_tasks_run_in_queue_order_with_checkpoints)
{
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    embi_loop_task(l, mark_100, &t);
    embi_loop_task(l, mark_101, &t);
    EMB_CHECK(!embi_loop_idle(l));
    EMB_CHECK(embi_loop_pump(l, 0.0));
    EMB_CHECK_EQ_INT(t.n, 2);
    EMB_CHECK_EQ_INT(t.v[0], 100);
    EMB_CHECK_EQ_INT(t.v[1], 101);
    EMB_CHECK_EQ_INT(t.checkpoints, 2);
    EMB_CHECK(embi_loop_idle(l));
    EMB_CHECK(!embi_loop_pump(l, 0.0));
    embi_loop_free(l);
}

EMB_TEST(loop_timers_fire_in_due_order_then_creation_order)
{
    embi_clock_set_fake(true, 1000000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    /* Created in this order; due at 30, 10, 10 ms (ties keep creation order). */
    (void)embi_loop_timer_add(l, 30.0, false, (void *)(intptr_t)3);
    (void)embi_loop_timer_add(l, 10.0, false, (void *)(intptr_t)1);
    (void)embi_loop_timer_add(l, 10.0, false, (void *)(intptr_t)2);
    double due = 0;
    EMB_CHECK(embi_loop_next_due(l, &due));
    EMB_CHECK(due == 1010.0);

    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 0);

    embi_clock_advance_fake(10000000ULL); /* 10 ms */
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 2);
    EMB_CHECK_EQ_INT(t.v[0], 1);
    EMB_CHECK_EQ_INT(t.v[1], 2);
    /* One-shot timers release their payloads once they have fired. */
    EMB_CHECK_EQ_INT(t.released, 2);

    embi_clock_advance_fake(20000000ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 3);
    EMB_CHECK_EQ_INT(t.v[2], 3);
    EMB_CHECK_EQ_INT(t.released, 3);
    EMB_CHECK(!embi_loop_next_due(l, &due));
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

/* Each timer makes the next one, with timeout 0, from inside its callback, down to depth 8. */
static void fire_chain(void *user, void *payload)
{
    trace *t = user;
    int depth = (int)(intptr_t)payload;
    push(t, depth);
    if (depth < 8) {
        (void)embi_loop_timer_add(t->loop, 0.0, false, (void *)(intptr_t)(depth + 1));
    }
}

EMB_TEST(loop_nesting_clamp_makes_deep_timers_four_ms)
{
    /* A timer a callback makes never runs in the pump that made it, so each pump runs one level. Levels 1 to 5 have the
     * timeout 0 (due at once); level 6 and deeper are clamped to 4 ms, so they wait for the clock. */
    embi_clock_set_fake(true, 2000000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_chain);
    (void)embi_loop_timer_add(l, 0.0, false, (void *)(intptr_t)1);
    for (int level = 1; level <= 5; level++) {
        EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
        EMB_CHECK_EQ_INT(t.n, level);
    }
    /* The level 6 timer is due 4 ms later, so nothing runs now. */
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    embi_clock_advance_fake(3000000ULL);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    embi_clock_advance_fake(1000000ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 6);
    embi_clock_advance_fake(4000000ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 7);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(loop_set_interval_zero_runs_once_per_pump_on_a_fake_clock)
{
    /* The fake clock does not move: a rescheduled timer is due at once, but it must wait for the next pump. */
    embi_clock_set_fake(true, 2500000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    uint32_t id = embi_loop_timer_add(l, 0.0, true, (void *)(intptr_t)9);
    for (int i = 1; i <= 3; i++) {
        EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
        EMB_CHECK_EQ_INT(t.n, i);
    }
    EMB_CHECK((intptr_t)embi_loop_timer_clear(l, id) == 9);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 3);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(loop_repeat_is_clamped_to_four_ms_after_five_runs)
{
    embi_clock_set_fake(true, 2600000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    uint32_t id = embi_loop_timer_add(l, 0.0, true, (void *)(intptr_t)4);
    /* Runs 1 to 5 are not clamped: one per pump with the clock standing still. */
    for (int i = 1; i <= 5; i++) {
        EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
        EMB_CHECK_EQ_INT(t.n, i);
    }
    /* The sixth is 4 ms after the fifth. */
    double due = 0;
    EMB_CHECK(embi_loop_next_due(l, &due));
    EMB_CHECK(due == embi_clock_ms() + 4.0);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    embi_clock_advance_fake(3999999ULL);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    embi_clock_advance_fake(1000001ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 6);
    /* And the interval stays 4 ms from then on. */
    EMB_CHECK(embi_loop_next_due(l, &due));
    EMB_CHECK(due == embi_clock_ms() + 4.0);
    (void)embi_loop_timer_clear(l, id);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

/* The callback of a repeating timer clears the timer itself on its third run. */
static uint32_t g_self_id;
static int g_cleared_payloads;

static void fire_self_clear(void *user, void *payload)
{
    trace *t = user;
    push(t, (int)(intptr_t)payload);
    if (t->n == 3) {
        void *p = embi_loop_timer_clear(t->loop, g_self_id);
        if (p != nullptr) {
            g_cleared_payloads++; /* the caller of clear owns the payload now (the binding releases it) */
        }
        /* A second clear of the same id finds nothing. */
        if (embi_loop_timer_clear(t->loop, g_self_id) != nullptr) {
            g_cleared_payloads += 100;
        }
    }
}

EMB_TEST(loop_repeat_cleared_from_its_own_callback_stops_and_does_not_release_twice)
{
    embi_clock_set_fake(true, 2700000000ULL);
    trace t = {0};
    g_cleared_payloads = 0;
    embi_loop *l = make_loop(&t, fire_self_clear);
    g_self_id = embi_loop_timer_add(l, 0.0, true, (void *)(intptr_t)5);
    for (int i = 0; i < 6; i++) {
        (void)embi_loop_pump(l, embi_clock_ms());
        embi_clock_advance_fake(10000000ULL);
    }
    EMB_CHECK_EQ_INT(t.n, 3);
    EMB_CHECK_EQ_INT(g_cleared_payloads, 1);
    /* The loop released nothing itself: the clear returned the payload to its caller. */
    EMB_CHECK_EQ_INT(t.released, 0);
    EMB_CHECK(embi_loop_idle(l));
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

/* A one-shot timer clearing its own id from its callback finds nothing (it is already gone) and is released once. */
static void fire_oneshot_self_clear(void *user, void *payload)
{
    trace *t = user;
    push(t, (int)(intptr_t)payload);
    if (embi_loop_timer_clear(t->loop, g_self_id) != nullptr) {
        g_cleared_payloads++;
    }
}

EMB_TEST(loop_one_shot_clearing_itself_is_released_once)
{
    embi_clock_set_fake(true, 2800000000ULL);
    trace t = {0};
    g_cleared_payloads = 0;
    embi_loop *l = make_loop(&t, fire_oneshot_self_clear);
    g_self_id = embi_loop_timer_add(l, 0.0, false, (void *)(intptr_t)6);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 1);
    EMB_CHECK_EQ_INT(g_cleared_payloads, 0);
    EMB_CHECK_EQ_INT(t.released, 1);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

/* A callback clears the timers of the loop (the document was replaced): the rest of the batch does not run. */
static void fire_clear_all(void *user, void *payload)
{
    trace *t = user;
    push(t, (int)(intptr_t)payload);
    embi_loop_clear_timers(t->loop);
}

EMB_TEST(loop_clear_timers_inside_a_callback_drops_the_rest_of_the_batch)
{
    embi_clock_set_fake(true, 2900000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_clear_all);
    (void)embi_loop_timer_add(l, 0.0, false, (void *)(intptr_t)1);
    (void)embi_loop_timer_add(l, 0.0, false, (void *)(intptr_t)2);
    (void)embi_loop_timer_add(l, 0.0, true, (void *)(intptr_t)3);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 1);
    /* The two timers that did not run were released by the clear; the one that ran was not released again. */
    EMB_CHECK_EQ_INT(t.released, 2);
    EMB_CHECK(embi_loop_idle(l));
    /* The loop works again afterwards. */
    t.n = 0;
    (void)embi_loop_timer_add(l, 0.0, false, (void *)(intptr_t)4);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.v[0], 4);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(loop_many_timers_all_fire_and_clears_find_them)
{
    embi_clock_set_fake(true, 3100000000ULL);
    enum { N = 2000 };
    static uint32_t ids[N];
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    for (int i = 0; i < N; i++) {
        int due = (i * 7919) % N + 1;
        ids[i] = embi_loop_timer_add(l, (double)due, false, (void *)(intptr_t)due);
    }
    int cleared = 0;
    for (int i = 0; i < N; i += 2) {
        EMB_CHECK(embi_loop_timer_clear(l, ids[i]) != nullptr);
        cleared++;
    }
    EMB_CHECK(embi_loop_timer_clear(l, ids[0]) == nullptr);
    embi_clock_advance_fake((uint64_t)(N + 1) * 1000000ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.released, N - cleared);
    EMB_CHECK(embi_loop_idle(l));
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

/* The order in which timers ran, for the ordering test below. */
static int g_order[256];
static int g_order_n;

static void fire_order(void *user, void *payload)
{
    (void)user;
    if (g_order_n < 256) {
        g_order[g_order_n++] = (int)(intptr_t)payload;
    }
}

EMB_TEST(loop_heap_pops_in_due_then_creation_order)
{
    embi_clock_set_fake(true, 3200000000ULL);
    trace t = {0};
    g_order_n = 0;
    embi_loop *l = make_loop(&t, fire_order);
    for (int i = 0; i < 200; i++) {
        int due = (i * 37) % 50; /* many ties */
        (void)embi_loop_timer_add(l, (double)due, false, (void *)(intptr_t)(due * 1000 + i));
    }
    embi_clock_advance_fake(100000000ULL);
    EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(g_order_n, 200);
    bool sorted = true;
    for (int i = 1; i < 200; i++) {
        sorted = sorted && g_order[i - 1] < g_order[i]; /* by due time, then by creation index (the low digits) */
    }
    EMB_CHECK(sorted);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

static void requeue(void *user)
{
    trace *t = user;
    push(t, 1);
    if (t->n < 40) {
        embi_loop_task(t->loop, requeue, t);
    }
}

EMB_TEST(loop_task_chain_runs_one_task_per_pump)
{
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    embi_loop_task(l, requeue, &t);
    for (int i = 1; i <= 40; i++) {
        EMB_CHECK(embi_loop_pump(l, 0.0));
        EMB_CHECK_EQ_INT(t.n, i);
    }
    EMB_CHECK(embi_loop_idle(l));
    embi_loop_free(l);
}

EMB_TEST(loop_repeat_runs_until_cleared)
{
    embi_clock_set_fake(true, 3000000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    uint32_t id = embi_loop_timer_add(l, 5.0, true, (void *)(intptr_t)7);
    EMB_CHECK(id != 0);
    for (int i = 0; i < 3; i++) {
        embi_clock_advance_fake(5000000ULL);
        EMB_CHECK(embi_loop_pump(l, embi_clock_ms()));
    }
    EMB_CHECK_EQ_INT(t.n, 3);
    /* A repeating timer keeps its payload while it runs. */
    EMB_CHECK_EQ_INT(t.released, 0);
    void *p = embi_loop_timer_clear(l, id);
    EMB_CHECK((intptr_t)p == 7);
    EMB_CHECK(embi_loop_timer_clear(l, id) == nullptr);
    embi_clock_advance_fake(50000000ULL);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 3);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(loop_clear_timers_releases_every_live_payload)
{
    embi_clock_set_fake(true, 4000000000ULL);
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    (void)embi_loop_timer_add(l, 100.0, false, (void *)(intptr_t)1);
    (void)embi_loop_timer_add(l, 100.0, true, (void *)(intptr_t)2);
    embi_loop_clear_timers(l);
    EMB_CHECK_EQ_INT(t.released, 2);
    EMB_CHECK(!embi_loop_next_due(l, nullptr));
    embi_clock_advance_fake(200000000ULL);
    EMB_CHECK(!embi_loop_pump(l, embi_clock_ms()));
    EMB_CHECK_EQ_INT(t.n, 0);
    embi_loop_free(l);
    embi_clock_set_fake(false, 0);
}

typedef struct post_arg {
    trace *t;
    int value;
} post_arg;

static void posted_mark(void *user)
{
    post_arg *a = user;
    push(a->t, a->value);
}

/* The thread posts the tasks; the loop thread (this test) takes them at its next pump. */
static int poster_thread(void *data)
{
    post_arg *args = data;
    embi_loop *l = args[0].t->loop;
    for (int i = 0; i < 4; i++) {
        (void)embi_loop_post(l, posted_mark, &args[i]);
    }
    return 0;
}

EMB_TEST(loop_post_from_another_thread_runs_on_the_loop_thread)
{
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    static post_arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].t = &t;
        args[i].value = 50 + i;
    }
    SDL_Thread *th = SDL_CreateThread(poster_thread, "loop-poster", args);
    EMB_REQUIRE(th != nullptr);
    SDL_WaitThread(th, nullptr);
    /* Nothing ran on the posting thread: the tasks wait for the loop's pump. */
    EMB_CHECK_EQ_INT(t.n, 0);
    EMB_CHECK(!embi_loop_idle(l));
    EMB_CHECK(embi_loop_pump(l, 0.0));
    EMB_CHECK_EQ_INT(t.n, 4);
    EMB_CHECK_EQ_INT(t.v[0], 50);
    EMB_CHECK_EQ_INT(t.v[3], 53);
    EMB_CHECK(embi_loop_idle(l));
    embi_loop_free(l);
}

EMB_TEST(loop_post_wakes_a_blocked_event_wait_when_asked)
{
    EMB_REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS));
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    EMB_CHECK(!embi_loop_has_tasks(l));
    /* Without the wake-up, a post pushes no event. */
    post_arg a = {&t, 1};
    EMB_CHECK(embi_loop_post(l, posted_mark, &a));
    EMB_CHECK(embi_loop_has_tasks(l));
    SDL_PumpEvents();
    SDL_Event ev;
    EMB_CHECK(!SDL_PeepEvents(&ev, 1, SDL_PEEKEVENT, SDL_EVENT_USER, SDL_EVENT_USER));
    /* With it, the post pushes a user event, which ends SDL_WaitEventTimeout at once. */
    embi_loop_set_wake(l, true);
    EMB_CHECK(embi_loop_post(l, posted_mark, &a));
    EMB_CHECK(SDL_WaitEventTimeout(&ev, 1000));
    EMB_CHECK(ev.type == SDL_EVENT_USER);
    EMB_CHECK(embi_loop_pump(l, 0.0));
    EMB_CHECK_EQ_INT(t.n, 2);
    EMB_CHECK(!embi_loop_has_tasks(l));
    embi_loop_free(l);
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

EMB_TEST(loop_post_rejects_null)
{
    trace t = {0};
    embi_loop *l = make_loop(&t, fire_plain);
    EMB_CHECK(!embi_loop_post(l, nullptr, nullptr));
    embi_loop_free(l);
}

EMB_TEST_MAIN()
