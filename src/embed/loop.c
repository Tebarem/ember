/*
 * The event loop of a view (W5c). It runs tasks in the order they were queued, and timers
 * when they are due, with a microtask checkpoint after each. It knows nothing of JavaScript: the script layer (script.c)
 * gives it the hooks that run a timer's payload and the checkpoint.
 *
 * Timers. A timer is due at its time (the clock at the call plus the timeout, clamped as HTML does: a nesting level above
 * five makes a timeout under 4 ms count as 4 ms). Timers due at the same time run in the order they were created. A repeating
 * timer is rescheduled from the time it ran, one nesting level deeper, so setInterval(f, 0) is clamped to 4 ms after five
 * runs. The timers live in a binary heap keyed by (due time, creation order) with a map from id to timer, so a fire and a
 * clear cost O(log n).
 *
 * Progress. Each pump takes the timers that are due at its start and were scheduled before it, and runs only those: a timer
 * that a callback creates or reschedules waits for the next pump, even when it is due already (a zero timeout, or a fake
 * clock that does not move). So a pump always ends, and setInterval(f, 0) runs once per pump.
 *
 * Threads. embi_loop_post may be called from any thread; it queues the task under a mutex, and the loop's thread takes it at
 * its next pump. Everything else is the loop's thread only.
 */
#include "embed/script_int.h"

#include "base/clock.h"
#include "base/vec.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mutex.h>

#include <string.h>

#define EMBI_LOOP_MAX_NEST 5u /* HTML: timers nested deeper than this are clamped */
#define EMBI_LOOP_CLAMP_MS 4.0

typedef struct embi_task {
    void (*fn)(void *user);
    void *user;
    void (*drop)(void *user); /* called instead of fn when the task is discarded unrun (nullptr: nothing to release) */
} embi_task;

typedef struct embi_timer {
    uint32_t id;
    uint32_t nest;      /* nesting level of the run this timer makes (the creator's level plus one, plus one per repeat) */
    uint32_t pos;       /* index in the heap while queued */
    bool repeat;
    bool in_map;        /* the id still names this timer (cleared by clearTimeout, and for a one-shot while it fires) */
    bool in_batch;      /* taken by the running pump */
    bool cancelled;     /* a batch member that was cleared: the pump frees it without running it */
    double due_ms;
    double base_ms;     /* the timeout as requested, before the clamp */
    uint64_t seq;       /* creation order, for timers due at the same time */
    uint64_t born;      /* pump generation of the last (re)schedule: the timer runs in a later pump */
    void *payload;
} embi_timer;

struct embi_loop {
    embi_loop_hooks hooks;
    EMBI_VEC(embi_task) tasks; /* the loop thread's queue, from tasks[head] */
    uint32_t head;
    EMBI_VEC(embi_task) posted; /* tasks posted from other threads, guarded by mu */
    SDL_Mutex *mu;
    EMBI_VEC(embi_timer *) heap;  /* min-heap on (due_ms, seq) */
    EMBI_VEC(embi_timer *) batch; /* the timers the running pump will run */
    EMBI_VEC(embi_timer *) later; /* scratch: due timers that were scheduled in this pump */
    embi_hashmap map;             /* id -> embi_timer * */
    uint32_t next_id;
    uint64_t next_seq;
    uint64_t pump_gen;
    uint32_t epoch;     /* bumped when timers or tasks are dropped, so a pump notices the document was replaced */
    uint32_t cur_nest;  /* nesting level of the timer whose callback runs, 0 outside timers */
    bool pumping;
    bool wake; /* embi_loop_post also pushes an SDL user event, to wake a windowed view blocked in its event wait */
};

/* ------------------------------------------------------------------ the heap */

static bool before(const embi_timer *a, const embi_timer *b)
{
    return a->due_ms < b->due_ms || (a->due_ms == b->due_ms && a->seq < b->seq);
}

static void heap_set(embi_loop *l, uint32_t i, embi_timer *t)
{
    l->heap.data[i] = t;
    t->pos = i;
}

static void sift_up(embi_loop *l, uint32_t i)
{
    embi_timer *t = l->heap.data[i];
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (!before(t, l->heap.data[p])) {
            break;
        }
        heap_set(l, i, l->heap.data[p]);
        i = p;
    }
    heap_set(l, i, t);
}

static void sift_down(embi_loop *l, uint32_t i)
{
    embi_timer *t = l->heap.data[i];
    uint32_t n = l->heap.len;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= n) {
            break;
        }
        if (c + 1 < n && before(l->heap.data[c + 1], l->heap.data[c])) {
            c++;
        }
        if (!before(l->heap.data[c], t)) {
            break;
        }
        heap_set(l, i, l->heap.data[c]);
        i = c;
    }
    heap_set(l, i, t);
}

static void heap_push(embi_loop *l, embi_timer *t)
{
    t->in_batch = false;
    embi_vec_push(&l->heap, t);
    sift_up(l, l->heap.len - 1);
}

static embi_timer *heap_pop(embi_loop *l)
{
    embi_timer *top = l->heap.data[0];
    embi_timer *last = l->heap.data[--l->heap.len];
    if (l->heap.len > 0) {
        heap_set(l, 0, last);
        sift_down(l, 0);
    }
    return top;
}

static void heap_remove(embi_loop *l, embi_timer *t)
{
    uint32_t i = t->pos;
    embi_timer *last = l->heap.data[--l->heap.len];
    if (i < l->heap.len) {
        heap_set(l, i, last);
        sift_down(l, i);
        sift_up(l, last->pos);
    }
}

static void timer_free(embi_timer *t)
{
    embi_free(t, sizeof *t);
}

/* The timeout a run of nesting level nest waits: HTML clamps a level above five to at least 4 ms. */
static double clamped(double base_ms, uint32_t nest)
{
    return (nest > EMBI_LOOP_MAX_NEST && base_ms < EMBI_LOOP_CLAMP_MS) ? EMBI_LOOP_CLAMP_MS : base_ms;
}

/* ------------------------------------------------------------------ lifetime and tasks */

embi_loop *embi_loop_new(const embi_loop_hooks *hooks)
{
    embi_loop *l = embi_calloc(1, sizeof *l);
    l->hooks = *hooks;
    l->mu = SDL_CreateMutex();
    embi_hashmap_init(&l->map);
    l->next_id = 1;
    return l;
}

/* Frees every timer record without calling the release hook (the realm is gone or going). */
static void free_all_records(embi_loop *l)
{
    for (uint32_t i = 0; i < l->heap.len; i++) {
        timer_free(l->heap.data[i]);
    }
    l->heap.len = 0;
    embi_hashmap_clear(&l->map);
}

void embi_loop_free(embi_loop *l)
{
    if (l == nullptr) {
        return;
    }
    free_all_records(l);
    for (uint32_t i = 0; i < l->batch.len; i++) {
        timer_free(l->batch.data[i]);
    }
    embi_loop_clear_tasks(l); /* releases the tasks that never ran */
    embi_vec_free(&l->tasks);
    embi_vec_free(&l->posted);
    embi_vec_free(&l->heap);
    embi_vec_free(&l->batch);
    embi_vec_free(&l->later);
    embi_hashmap_free(&l->map);
    if (l->mu != nullptr) {
        SDL_DestroyMutex(l->mu);
    }
    embi_free(l, sizeof *l);
}

void embi_loop_task(embi_loop *l, void (*fn)(void *user), void *user)
{
    embi_task t = {fn, user, nullptr};
    embi_vec_push(&l->tasks, t);
}

bool embi_loop_post(embi_loop *l, void (*fn)(void *user), void *user)
{
    return embi_loop_post_ex(l, fn, user, nullptr);
}

bool embi_loop_post_ex(embi_loop *l, void (*fn)(void *user), void *user, void (*drop)(void *user))
{
    if (l == nullptr || fn == nullptr) {
        return false;
    }
    embi_task t = {fn, user, drop};
    SDL_LockMutex(l->mu);
    embi_vec_push(&l->posted, t);
    bool wake = l->wake;
    SDL_UnlockMutex(l->mu);
    if (wake) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_EVENT_USER;
        (void)SDL_PushEvent(&ev); /* safe from any thread; the window loop ignores the event after waking */
    }
    return true;
}

void embi_loop_set_wake(embi_loop *l, bool wake)
{
    SDL_LockMutex(l->mu);
    l->wake = wake;
    SDL_UnlockMutex(l->mu);
}

/* ------------------------------------------------------------------ timers */

/* The timer hooks of the binding context (bind.h embi_bind_timers): user is the loop. */
uint32_t embi_loop_timer_add(void *user, double timeout_ms, bool repeat, void *payload)
{
    embi_loop *l = user;
    embi_timer *t = embi_calloc(1, sizeof *t);
    uint64_t existing;
    while (l->next_id == 0 || embi_hashmap_get(&l->map, l->next_id, &existing)) {
        l->next_id++; /* ids are 32 bit and wrap; 0 means "no timer" to the caller */
    }
    t->id = l->next_id++;
    t->nest = l->cur_nest + 1u;
    t->repeat = repeat;
    t->in_map = true;
    t->base_ms = timeout_ms;
    t->due_ms = embi_clock_ms() + clamped(timeout_ms, t->nest);
    t->seq = l->next_seq++;
    t->born = l->pump_gen;
    t->payload = payload;
    embi_hashmap_put(&l->map, t->id, (uint64_t)(uintptr_t)t);
    heap_push(l, t);
    return t->id;
}

/* Cancels the timer id. Returns its payload (for the caller to release), or nullptr when it has no live timer. */
void *embi_loop_timer_clear(void *user, uint32_t id)
{
    embi_loop *l = user;
    uint64_t v;
    if (!embi_hashmap_get(&l->map, id, &v)) {
        return nullptr;
    }
    embi_timer *t = (embi_timer *)(uintptr_t)v;
    void *payload = t->payload;
    embi_hashmap_remove(&l->map, id);
    t->in_map = false;
    if (t->in_batch) {
        t->cancelled = true; /* the running pump frees it */
    } else {
        heap_remove(l, t);
        timer_free(t);
    }
    return payload;
}

void embi_loop_clear_timers(embi_loop *l)
{
    l->epoch++;
    for (uint32_t i = 0; i < l->heap.len; i++) {
        embi_timer *t = l->heap.data[i];
        if (l->hooks.release != nullptr) {
            l->hooks.release(l->hooks.user, t->payload);
        }
        timer_free(t);
    }
    l->heap.len = 0;
    for (uint32_t i = 0; i < l->batch.len; i++) {
        embi_timer *t = l->batch.data[i];
        if (t->in_map && !t->cancelled && l->hooks.release != nullptr) {
            l->hooks.release(l->hooks.user, t->payload);
        }
        t->in_map = false;
        t->cancelled = true; /* the running pump frees the record */
    }
    embi_hashmap_clear(&l->map);
}

void embi_loop_clear_tasks(embi_loop *l)
{
    l->epoch++;
    /* Take the unrun tasks out first: a drop function may queue or post again. */
    embi_task *old = nullptr;
    uint32_t n = 0;
    if (l->head < l->tasks.len) {
        n = l->tasks.len - l->head;
        old = embi_alloc((size_t)n * sizeof *old);
        memcpy(old, l->tasks.data + l->head, (size_t)n * sizeof *old);
    }
    embi_vec_clear(&l->tasks);
    l->head = 0;
    embi_task *oldp = nullptr;
    uint32_t np = 0;
    SDL_LockMutex(l->mu);
    if (l->posted.len > 0) {
        np = l->posted.len;
        oldp = embi_alloc((size_t)np * sizeof *oldp);
        memcpy(oldp, l->posted.data, (size_t)np * sizeof *oldp);
        l->posted.len = 0;
    }
    SDL_UnlockMutex(l->mu);
    for (uint32_t i = 0; i < n; i++) {
        if (old[i].drop != nullptr) {
            old[i].drop(old[i].user);
        }
    }
    for (uint32_t i = 0; i < np; i++) {
        if (oldp[i].drop != nullptr) {
            oldp[i].drop(oldp[i].user);
        }
    }
    if (old != nullptr) {
        embi_free(old, (size_t)n * sizeof *old);
    }
    if (oldp != nullptr) {
        embi_free(oldp, (size_t)np * sizeof *oldp);
    }
}

/* Takes the posted tasks into the loop's queue. */
static void take_posted(embi_loop *l)
{
    SDL_LockMutex(l->mu);
    for (uint32_t i = 0; i < l->posted.len; i++) {
        embi_vec_push(&l->tasks, l->posted.data[i]);
    }
    l->posted.len = 0;
    SDL_UnlockMutex(l->mu);
}

/* Runs the tasks that were queued before the pump; the ones they queue wait for the next pump. */
static bool run_tasks(embi_loop *l)
{
    bool ran = false;
    uint32_t n = l->tasks.len - l->head;
    uint32_t epoch = l->epoch;
    while (n-- > 0 && l->head < l->tasks.len) {
        embi_task t = l->tasks.data[l->head++];
        ran = true;
        t.fn(t.user);
        if (l->hooks.checkpoint != nullptr) {
            l->hooks.checkpoint(l->hooks.user);
        }
        if (l->epoch != epoch) {
            return ran; /* the document was replaced: the queue is the new document's */
        }
    }
    /* Drop what ran. The remainder moves down, so a chain of tasks that queue tasks does not grow the vector. */
    if (l->head >= l->tasks.len) {
        embi_vec_clear(&l->tasks);
    } else if (l->head > 0) {
        memmove(l->tasks.data, l->tasks.data + l->head, (size_t)(l->tasks.len - l->head) * sizeof l->tasks.data[0]);
        l->tasks.len -= l->head;
    }
    l->head = 0;
    return ran;
}

/* Runs one timer of the batch. */
static void run_timer(embi_loop *l, embi_timer *t)
{
    uint32_t epoch = l->epoch;
    void *payload = t->payload;
    double started = embi_clock_ms();
    uint32_t saved = l->cur_nest;
    l->cur_nest = t->nest;
    if (!t->repeat) {
        /* A one-shot timer is gone before its callback runs: clearTimeout of its own id does nothing. */
        embi_hashmap_remove(&l->map, t->id);
        t->in_map = false;
    }
    if (l->hooks.fire != nullptr) {
        l->hooks.fire(l->hooks.user, payload);
    }
    /* The microtask checkpoint is part of the timer task: a timer set from a promise reaction nests one deeper. */
    if (l->hooks.checkpoint != nullptr) {
        l->hooks.checkpoint(l->hooks.user);
    }
    l->cur_nest = saved;
    if (!t->repeat) {
        /* If the callback replaced the document, clear_timers skipped this running one-shot (it is out of the map), so
         * its payload is not released here either: the caller must free the realm after clear_timers, as
         * embi_script_reset does, which reclaims the payload's handle with it. */
        if (l->epoch == epoch && l->hooks.release != nullptr) {
            l->hooks.release(l->hooks.user, payload);
        }
        timer_free(t);
        return;
    }
    if (t->cancelled) {
        /* Cleared by its own callback (the clear's caller released the payload), or dropped with the document. */
        timer_free(t);
        return;
    }
    /* Rescheduled from the time it ran, one nesting level deeper, for a later pump. */
    t->nest++;
    t->due_ms = started + clamped(t->base_ms, t->nest);
    t->seq = l->next_seq++;
    t->born = l->pump_gen;
    heap_push(l, t);
}

bool embi_loop_pump(embi_loop *l, double now_ms)
{
    if (l->pumping) {
        return false; /* a callback pumped the loop again: the outer pump continues */
    }
    l->pumping = true;
    l->pump_gen++;
    take_posted(l);
    bool ran = run_tasks(l);

    /* The timers due now that were scheduled before this pump, in due order. */
    l->later.len = 0;
    while (l->heap.len > 0 && l->heap.data[0]->due_ms <= now_ms) {
        embi_timer *t = heap_pop(l);
        if (t->born >= l->pump_gen) {
            embi_vec_push(&l->later, t);
            continue;
        }
        t->in_batch = true;
        embi_vec_push(&l->batch, t);
    }
    for (uint32_t i = 0; i < l->later.len; i++) {
        heap_push(l, l->later.data[i]);
    }
    l->later.len = 0;

    for (uint32_t i = 0; i < l->batch.len; i++) {
        embi_timer *t = l->batch.data[i];
        if (t->cancelled) {
            timer_free(t);
            continue;
        }
        ran = true;
        run_timer(l, t);
    }
    l->batch.len = 0;
    l->pumping = false;
    return ran;
}

bool embi_loop_has_tasks(const embi_loop *l)
{
    bool any = l->head < l->tasks.len;
    SDL_LockMutex(l->mu);
    any = any || l->posted.len > 0;
    SDL_UnlockMutex(l->mu);
    return any;
}

bool embi_loop_idle(const embi_loop *l)
{
    bool none = l->head >= l->tasks.len && l->heap.len == 0 && l->batch.len == 0;
    SDL_LockMutex(l->mu);
    none = none && l->posted.len == 0;
    SDL_UnlockMutex(l->mu);
    return none;
}

bool embi_loop_next_due(const embi_loop *l, double *due_ms)
{
    if (l->heap.len == 0) {
        return false;
    }
    if (due_ms != nullptr) {
        *due_ms = l->heap.data[0]->due_ms;
    }
    return true;
}
