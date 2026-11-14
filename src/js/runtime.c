#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include "js/ic.h"
#include "js/interp.h"
void embi_jsm_runtime_free(embi_js_runtime *rt); /* module.c */
#include "js/object.h"
#include "js/runtime.h"
#include "js/string.h"

#include <SDL3/SDL_timer.h>

/* ------------------------------------------------------------------ native stack */

/* A thread's stack top never changes, so it is looked up once per thread. On Linux the lookup
 * (pthread_getattr_np) parses /proc/self/maps, and calling it on every parser nesting level made
 * document loads take seconds (W8b0 triage G1). */
static uintptr_t stack_top_lookup(void)
{
#if defined(_WIN32)
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return (uintptr_t)high;
#elif defined(__APPLE__)
    return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#elif defined(__linux__)
    pthread_attr_t attr;
    void *addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return 0;
    }
    int rc = pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    return rc == 0 ? (uintptr_t)addr + size : 0;
#else
    return 0;
#endif
}

uintptr_t embi_js_stack_top(void)
{
    static thread_local uintptr_t top;
    if (top == 0) {
        top = stack_top_lookup();
    }
    return top;
}

/* ------------------------------------------------------------------ cell kind tracing */

static void trace_symbol(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_mark_value(t, ((embi_js_symbol *)c)->description);
}

static void trace_box(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_mark_value(t, ((embi_js_box *)c)->value);
}

static void trace_accessor(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_accessor *a = (embi_js_accessor *)c;
    embi_js_mark_value(t, a->getter);
    embi_js_mark_value(t, a->setter);
}

static void trace_varray(embi_js_cell *c, embi_js_tracer *t)
{
    const embi_js_varray *a = (const embi_js_varray *)c;
    for (uint32_t i = 0; i < c->aux; i++) {
        embi_js_mark_value(t, a->elems[i]);
    }
}

/* ------------------------------------------------------------------ lifecycle */

embi_js_runtime *embi_js_runtime_new(void)
{
    embi_js_runtime *rt = embi_calloc(1, sizeof *rt);
    embi_js_heap_init(&rt->heap);
    embi_js_atoms_init(&rt->atoms);
    rt->pending_exception = EMBI_JS_UNDEFINED;
    rt->protect = UINT32_MAX;
    rt->rope_min = 256;

    embi_js_heap_set_ops(&rt->heap, EMBI_JS_CELL_SYMBOL, (embi_js_type_ops){trace_symbol, nullptr});
    embi_js_heap_set_ops(&rt->heap, EMBI_JS_CELL_BOX, (embi_js_type_ops){trace_box, nullptr});
    embi_js_heap_set_ops(&rt->heap, EMBI_JS_CELL_ACCESSOR,
                         (embi_js_type_ops){trace_accessor, nullptr});
    embi_js_heap_set_ops(&rt->heap, EMBI_JS_CELL_VALUE_ARRAY,
                         (embi_js_type_ops){trace_varray, nullptr});
    embi_js_objects_init(rt); /* object.c: object and shape cell ops, GC root hook */
    /* Generational from the start (J5k). Entering first makes the barrier target this runtime for
     * the switch, and leaving restores the previous target. */
    embi_js_enter(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_leave(rt);
    return rt;
}

void embi_js_runtime_free(embi_js_runtime *rt)
{
    if (!rt) {
        return;
    }
    embi_js_vm_free(rt);
    embi_jsm_runtime_free(rt);
    /* Freed after the heap is destroyed: its finalizers run inside embi_js_heap_destroy and could
     * still fill the stub cache. Entries hold shape addresses only, which are never dereferenced
     * here. */
    embi_js_heap_destroy(&rt->heap);
    embi_js_stub_cache_free(rt);
    embi_js_atoms_free(&rt->atoms);
    embi_vec_free(&rt->persistent.slots);
    embi_vec_free(&rt->weak.slots);
    embi_vec_free(&rt->roots);
    embi_vec_free(&rt->jobs);
    embi_vec_free(&rt->unhandled);
    embi_free(rt, sizeof *rt);
}

/* The write barrier target (embi_js_wb_heap) is per thread and follows the innermost runtime that is
 * entered on the thread (embi_js_entered). The outermost embi_js_enter of a runtime makes it the
 * innermost one and records the runtime that was innermost before (rt->wb_outer); its outermost
 * embi_js_leave makes that runtime innermost again. With nothing entered the target stays on the
 * runtime that left last, so stores made outside an entered call still reach its remembered set
 * (J5n2: the earlier scheme restored the target saved at the first enter, which could be a runtime
 * that was created later or long gone, and the barrier then went to the wrong heap; see
 * tests/unit/js/test_js_gc_wb_target.c, which fails on that scheme).
 *
 * Entries nest in stack order (LIFO) in every caller in the tree: host.c, jobs.c, interp.c and
 * src/loader/clone.c pair an enter with a leave in the same function, around a call that cannot
 * return early, and src/bindings and src/embed reach the engine only through those entry points
 * (audited J5n2). A leave that is not LIFO is still safe: the runtime is unlinked from the chain
 * wherever it sits, so the chain never keeps a runtime that has left, and the barrier target stays
 * on the innermost runtime that is still entered.
 *
 * Requirement (J5k): every engine entry point runs inside embi_js_enter/embi_js_leave on the
 * thread that uses the runtime. Two generational runtimes on one thread are safe only when each
 * one's stores happen inside its own enter/leave; a store outside any enter goes to the last
 * generational runtime that left on the thread, which may be the other one. */
static _Thread_local embi_js_runtime *embi_js_entered = nullptr;

static embi_js_heap *wb_target_of(const embi_js_runtime *rt)
{
    return rt->heap.generational ? (embi_js_heap *)&rt->heap : nullptr;
}

void embi_js_enter(embi_js_runtime *rt)
{
    if (rt->nest == 0) {
        rt->stack_base = embi_js_stack_top();
        rt->wb_outer = embi_js_entered;
        embi_js_entered = rt;
        embi_js_wb_heap = wb_target_of(rt);
    }
    rt->nest++;
}

void embi_js_leave(embi_js_runtime *rt)
{
    EMBI_ASSERT(rt->nest > 0);
    rt->nest--;
    if (rt->nest == 0) {
        if (embi_js_entered == rt) {
            embi_js_entered = rt->wb_outer;
            embi_js_wb_heap = wb_target_of(embi_js_entered != nullptr ? embi_js_entered : rt);
        } else {
            /* Not LIFO: another runtime was entered after rt and is still entered. Unlink rt from
             * the chain below it; the innermost runtime and the barrier target do not change. */
            for (embi_js_runtime *p = embi_js_entered; p != nullptr; p = p->wb_outer) {
                if (p->wb_outer == rt) {
                    p->wb_outer = rt->wb_outer;
                    break;
                }
            }
        }
        rt->wb_outer = nullptr;
    }
}

double embi_js_compile_ms(const embi_js_runtime *rt)
{
    return (double)rt->compile_ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void embi_js_compile_ms_add(embi_js_runtime *rt, uint64_t ticks)
{
    rt->compile_ticks += ticks;
}

void embi_js_adopt_thread(embi_js_runtime *rt)
{
    embi_js_enter(rt);
    embi_js_leave(rt);
}

void embi_js_set_gc_params(embi_js_runtime *rt, size_t min_trigger, double factor)
{
    rt->heap.min_trigger = min_trigger;
    rt->heap.trigger_factor = factor;
    embi_js_gc_retune(&rt->heap);
}

void embi_js_get_mem_stats(const embi_js_runtime *rt, embi_js_mem_stats *out)
{
    out->heap_live_bytes = rt->heap.live_bytes;
    out->heap_bytes_since_gc = rt->heap.bytes_since_gc;
    out->heap_chunks = rt->heap.chunks.len;
    out->large_objects = rt->heap.large.len;
    out->gc_count = rt->heap.gc_count;
    out->atoms = rt->atoms.count;
    out->persistent_handles = rt->persistent.live;
    out->weak_handles = rt->weak.live;
    out->minor_count = rt->heap.minor_count;
    out->major_count = rt->heap.major_count;
    out->remembered_peak = rt->heap.remembered_peak;
    out->released_chunks = rt->heap.released_chunks;
    out->live_after_gc = rt->heap.live_after_gc;
}

void embi_js_set_type_ops(embi_js_runtime *rt, uint8_t type, embi_js_type_ops ops)
{
    embi_js_heap_set_ops(&rt->heap, type, ops);
}

/* ------------------------------------------------------------------ handles */

static uint32_t handles_add(embi_js_handles *hs, embi_js_value v)
{
    uint32_t id;
    if (hs->free_head != 0) {
        id = hs->free_head;
        embi_js_handle_slot *s = &hs->slots.data[id - 1u];
        hs->free_head = s->next;
        s->value = v;
        s->next = 0;
        s->used = 1;
    } else {
        embi_js_handle_slot s = {v, 0, 1};
        embi_vec_push(&hs->slots, s);
        id = hs->slots.len;
    }
    hs->live++;
    return id;
}

static embi_js_handle_slot *handle_slot(const embi_js_handles *hs, uint32_t id)
{
    EMBI_ASSERT(id >= 1u && id <= hs->slots.len);
    embi_js_handle_slot *s = &hs->slots.data[id - 1u];
    EMBI_ASSERT(s->used);
    return s;
}

static void handles_free(embi_js_handles *hs, uint32_t id)
{
    embi_js_handle_slot *s = handle_slot(hs, id);
    s->used = 0;
    s->value = EMBI_JS_EMPTY;
    s->next = hs->free_head;
    hs->free_head = id;
    hs->live--;
}

uint32_t embi_js_persistent_new(embi_js_runtime *rt, embi_js_value v)
{
    return handles_add(&rt->persistent, v);
}

embi_js_value embi_js_persistent_get(const embi_js_runtime *rt, uint32_t id)
{
    return handle_slot(&rt->persistent, id)->value;
}

void embi_js_persistent_set(embi_js_runtime *rt, uint32_t id, embi_js_value v)
{
    handle_slot(&rt->persistent, id)->value = v;
}

void embi_js_persistent_free(embi_js_runtime *rt, uint32_t id)
{
    handles_free(&rt->persistent, id);
}

uint32_t embi_js_weak_new(embi_js_runtime *rt, embi_js_value v)
{
    return handles_add(&rt->weak, v);
}

embi_js_value embi_js_weak_get(const embi_js_runtime *rt, uint32_t id)
{
    return handle_slot(&rt->weak, id)->value;
}

void embi_js_weak_free(embi_js_runtime *rt, uint32_t id)
{
    handles_free(&rt->weak, id);
}

void embi_js_root_add(embi_js_runtime *rt, embi_js_root_fn fn, void *user)
{
    embi_js_root r = {fn, user};
    embi_vec_push(&rt->roots, r);
}

void embi_js_root_remove(embi_js_runtime *rt, embi_js_root_fn fn, void *user)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == fn && rt->roots.data[i].user == user) {
            rt->roots.data[i] = rt->roots.data[rt->roots.len - 1u];
            rt->roots.len--;
            return;
        }
    }
}

/* ------------------------------------------------------------------ jobs and exceptions */

void embi_js_job_push(embi_js_runtime *rt, embi_js_value job)
{
    embi_vec_push(&rt->jobs, job);
}

/* Popped slots are reclaimed once the queue drains, and also whenever the consumed prefix is at
 * least half the array, so a producer and consumer that never empty the queue keep it bounded.
 * Each compaction moves at most as many entries as were popped since the last one, so pops stay
 * amortized O(1). */
#define EMBI_JOB_COMPACT_MIN 32u

bool embi_js_job_pop(embi_js_runtime *rt, embi_js_value *out)
{
    if (rt->job_head >= rt->jobs.len) {
        embi_vec_clear(&rt->jobs);
        rt->job_head = 0;
        return false;
    }
    *out = rt->jobs.data[rt->job_head++];
    if (rt->job_head == rt->jobs.len) {
        embi_vec_clear(&rt->jobs);
        rt->job_head = 0;
    } else if (rt->job_head >= EMBI_JOB_COMPACT_MIN && rt->job_head * 2u >= rt->jobs.len) {
        uint32_t remaining = rt->jobs.len - rt->job_head;
        memmove(rt->jobs.data, rt->jobs.data + rt->job_head,
                (size_t)remaining * sizeof rt->jobs.data[0]);
        rt->jobs.len = remaining;
        rt->job_head = 0;
    }
    return true;
}

void embi_js_exception_set(embi_js_runtime *rt, embi_js_value v)
{
    rt->pending_exception = v;
}

embi_js_value embi_js_exception_get(const embi_js_runtime *rt)
{
    return rt->pending_exception;
}

void embi_js_exception_clear(embi_js_runtime *rt)
{
    rt->pending_exception = EMBI_JS_UNDEFINED;
}

/* ------------------------------------------------------------------ built-in cell kinds */

embi_js_symbol *embi_js_symbol_new(embi_js_runtime *rt, embi_js_value description)
{
    embi_js_symbol *s =
        (embi_js_symbol *)embi_js_alloc_cell(rt, sizeof *s, EMBI_JS_CELL_SYMBOL);
    EMBI_JS_WB(s, description);
    s->description = description;
    return s;
}

embi_js_box *embi_js_box_new(embi_js_runtime *rt, embi_js_value v)
{
    embi_js_box *b = (embi_js_box *)embi_js_alloc_cell(rt, sizeof *b, EMBI_JS_CELL_BOX);
    EMBI_JS_WB(b, v);
    b->value = v;
    return b;
}

embi_js_accessor *embi_js_accessor_new(embi_js_runtime *rt, embi_js_value getter,
                                       embi_js_value setter)
{
    embi_js_accessor *a =
        (embi_js_accessor *)embi_js_alloc_cell(rt, sizeof *a, EMBI_JS_CELL_ACCESSOR);
    EMBI_JS_WB(a, getter);
    EMBI_JS_WB(a, setter);
    a->getter = getter;
    a->setter = setter;
    return a;
}

embi_js_varray *embi_js_varray_new(embi_js_runtime *rt, uint32_t len)
{
    size_t bytes = sizeof(embi_js_cell) + (size_t)len * sizeof(embi_js_value);
    embi_js_varray *a =
        (embi_js_varray *)embi_js_alloc_cell(rt, bytes, EMBI_JS_CELL_VALUE_ARRAY);
    a->hdr.aux = len;
    for (uint32_t i = 0; i < len; i++) {
        EMBI_JS_STORE(a, a->elems[i], EMBI_JS_EMPTY);
    }
    return a;
}

embi_js_varray *embi_js_varray_grow(embi_js_runtime *rt, const embi_js_varray *a,
                                    uint32_t new_len)
{
    embi_js_varray *n = embi_js_varray_new(rt, new_len);
    uint32_t old_len = a->hdr.aux;
    uint32_t keep = old_len < new_len ? old_len : new_len;
    for (uint32_t i = 0; i < keep; i++) {
        EMBI_JS_WB(n, a->elems[i]);
        EMBI_JS_STORE(n, n->elems[i], a->elems[i]);
    }
    return n;
}
