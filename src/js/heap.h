#pragma once
/*
 * Heap: size-class pages carved from chunks (64 KB, growing to 256 KB), large objects on their own
 * allocations, and a non-moving mark-sweep collector with a conservative C-stack scan, lazy
 * sweeping and an optional generational mode (sticky mark bits; off by default). See gc.c.
 */
#include "js/value.h"

#include <stddef.h>
#include <stdint.h>

#define EMBI_JS_PAGE_SHIFT 14u
#define EMBI_JS_PAGE_BYTES ((size_t)1 << EMBI_JS_PAGE_SHIFT) /* 16 KB */
#define EMBI_JS_CHUNK_PAGES 16u /* the largest chunk: 256 KB */
#define EMBI_JS_CHUNK_BYTES (EMBI_JS_PAGE_BYTES * EMBI_JS_CHUNK_PAGES)
/* J5m3: the first chunk of a heap has EMBI_JS_CHUNK_FIRST_PAGES pages (64 KB), and each later
 * chunk doubles the page count up to EMBI_JS_CHUNK_PAGES. A small script then touches only the
 * pages it uses, and its peak RSS is not padded by a 256 KB chunk. */
#define EMBI_JS_CHUNK_FIRST_PAGES 4u
#define EMBI_JS_NUM_CLASSES 20u
#define EMBI_JS_MAX_SMALL 2048u
#define EMBI_JS_NO_CLASS 0xFFu

/* The verifier (gc.c, verify_minor) is compiled into debug builds. It runs at every minor
 * collection, so it only costs time when generational mode is on. */
#if !defined(EMBI_JS_GC_VERIFY) && !defined(NDEBUG)
#define EMBI_JS_GC_VERIFY 1
#endif

/* Makes p live until this point: a use the optimizer cannot remove, so p stays in a callee-saved
 * register or a stack slot across the calls before it. The conservative scan counts only the first
 * byte of a cell, so a cell that is reachable only through an interior pointer (the elems of a
 * varray passed on as argv) must be kept alive like this after the call that reads it; otherwise an
 * optimized build may drop the cell pointer (or turn the call into a tail call) and a collection in
 * the callee frees the arguments (J6b: Reflect.construct under --gc-stress). */
#define EMBI_KEEP_ALIVE(p) __asm__ volatile("" : : "r"(p) : "memory")

/* Cell header flag (the 16-bit flags field): the cell is in the remembered set. */
#define EMBI_JS_FLAG_REMEMBERED 0x8000u

/* Collector tuning (J5h). Untuned until the J5l memory deltas; change the numbers here only.
 *
 * The heap limit after a collection is live * factor, where factor is EMBI_JS_GC_FACTOR_MAX while
 * the live heap is at most EMBI_JS_GC_CURVE_LO, falls linearly to EMBI_JS_GC_FACTOR_MIN at
 * EMBI_JS_GC_CURVE_HI, and is capped by embi_js_set_gc_params. The allocation budget (bytes
 * allocated since the collection before the next one starts) is the room under that limit:
 *
 *     budget = max(EMBI_JS_GC_FLOOR_BYTES, live * (factor - 1))
 *
 * DECISION (J5h, reviewer item 3): the spec's "live x factor" is read as a heap limit, not as an
 * allocation budget. The old policy allowed about 2.0 x live of total growth (budget = live); this
 * one allows 1.5 x live at small heaps and 1.25 x live above 64 MB.
 *
 * J5n2: the factor is 1.25 at every heap size. The earlier
 * 1.5 start for small heaps was the cause of the framework peaks (preact_render 1.78x of
 * QuickJS-ng's peak delta with 1.5, against the 1.5x bound). The 1.25 schedule exposed a real
 * barrier bug, not a tuning problem: stores made outside an embi_js_enter went to the barrier
 * target of a runtime that had been created and left after the one in use (runtime.c, the
 * innermost-entered scheme), so old shapes lost their remembered entries. It is fixed and covered
 * by tests/unit/js/test_js_gc_wb_target.c. The curve between EMBI_JS_GC_FACTOR_MAX and _MIN is kept
 * for later tuning; both are 1.25 now, so the curve is flat.
 *
 * Measured (interleaved A/B, 9 rounds, CPU 2, factor 1.5 against 1.25 on the same binary through
 * EMBER_GC_FACTOR; default build and -falign-functions=64 build): the ratio of the best medians
 * 1.25 / 1.5 was preact_render 0.991 / 1.004, preact_diff 0.998 / 1.004, vue_reactive 1.009 / 1.001,
 * vue_render 0.993 / 1.001, so every framework workload is within 1% (the bound is 2%).
 *
 * Object, array and closure sizes (J5n2, bytes per item of 50000 kept in an array, the array slot not
 * counted; embi_mem_live difference after a full collection, so malloc headers are not in it; tools/bench/js/micro.sh):
 *   item                          HEAD (J5m3)   J5n2
 *   object, 2 properties              101.8     101.8   (96-byte cell, shape shared)
 *   object, 6 properties              165.8     117.8   (out-of-line slots sized to the need, not to 8)
 *   object, 12 properties             165.9     165.8
 *   array, 5 elements                 173.8     149.8
 *   array, 20 elements                269.8     269.8
 *   arrow function, no upvalue        157.8     125.8   (24-byte closure block, was 56)
 *   function expression, no upvalue   258.3     133.8   (lazy prototype object, 24-byte block)
 *   function expression, 1 upvalue    297.5     156.2
 * Framework live deltas after the change (KiB, QuickJS-ng in brackets): preact_render 129 (69),
 * preact_diff 4921 (5485), vue_reactive 833 (847), vue_render 2987 (3248). preact_render stays above
 * QuickJS-ng: its 76 functions carry about 1.1 KB each of code metadata (bytecode 242 B, inline caches
 * 334 B, positions and exception tables 158 B, the 256-byte code cell, constants, keys), which is
 * fixed per function and not per object. Peak RSS at load time dominates the framework peaks: with
 * zero steps vue_reactive already reaches 6.0 of its 6.2 MB (RssAnon 2384 KiB of 2656), so the trigger
 * schedule moves it by under 2% (floor 64 KiB to 256 KiB, factor 1.1 to 1.25, generational on or off).
 *
 * The minor budget of a generational heap shrinks toward half its value when the allocation rate
 * (bytes per ms of mutator time; 20000 to 200000 = 20 to 200 MB/s) passes EMBI_JS_GC_RATE_LO, so a fast allocator collects before its garbage piles up. */
#define EMBI_JS_GC_FLOOR_BYTES ((size_t)256u << 10)
#define EMBI_JS_GC_FACTOR_MAX 1.25
#define EMBI_JS_GC_FACTOR_MIN 1.25
#define EMBI_JS_GC_CURVE_LO ((size_t)16u << 20)
#define EMBI_JS_GC_CURVE_HI ((size_t)64u << 20)
#define EMBI_JS_GC_RATE_LO 20000.0
#define EMBI_JS_GC_RATE_HI 200000.0

typedef struct embi_js_tracer embi_js_tracer;

/* Ephemeron tables (WeakMap, WeakSet). Their trace function calls embi_js_trace_ephemeron with the
 * table and two callbacks, and does not mark the entries itself. When ordinary marking reaches its
 * fixed point, the collector calls mark(t, data) on every registered table until no call returns
 * true. mark marks each value whose key is marked and returns true when it marked a value that
 * was unmarked. Then the collector calls clear(data) on every registered table, which removes the
 * entries whose key is unmarked, before the sweep. */
typedef bool (*embi_js_ephemeron_mark_fn)(embi_js_tracer *t, void *data);
typedef void (*embi_js_ephemeron_clear_fn)(void *data);

typedef struct embi_js_ephemeron {
    void *data;
    embi_js_ephemeron_mark_fn mark;
    embi_js_ephemeron_clear_fn clear;
} embi_js_ephemeron;

/* trace marks every value the cell holds, through embi_js_mark_value/embi_js_mark_cell. It may
 * not allocate. finalize runs when the cell dies (or at runtime teardown) and may not allocate or
 * touch other cells. Either may be nullptr. */
typedef void (*embi_js_trace_fn)(embi_js_cell *cell, embi_js_tracer *t);
typedef void (*embi_js_finalize_fn)(embi_js_cell *cell);

typedef struct embi_js_type_ops {
    embi_js_trace_fn trace;
    embi_js_finalize_fn finalize;
} embi_js_type_ops;

typedef struct embi_js_chunk embi_js_chunk;

typedef struct embi_js_page_ref {
    embi_js_chunk *chunk;
    uint32_t index;
} embi_js_page_ref;

/* A free cell: the header carries type FREE, the link lives in the first body word. */
typedef struct embi_js_free_cell {
    embi_js_cell hdr;
    struct embi_js_free_cell *next;
} embi_js_free_cell;

/* Mark bits. Outside a collection, a cell's mark is 0, or 1 when it survived a collection in a
 * generational heap (sticky: 1 means old). A minor collection marks only the young cells it
 * reaches, from the roots and from the remembered set, and sweeps only the pages that can hold
 * young cells. The pages are the ones given cells since the previous collection began, so old
 * unmarked cells wait for the next major collection. */
typedef struct embi_js_heap {
    embi_js_type_ops ops[EMBI_JS_CELL_TYPE_COUNT];
    embi_js_free_cell *free_lists[EMBI_JS_NUM_CLASSES]; /* cells of swept pages only */
    EMBI_VEC(embi_js_chunk *) chunks; /* sorted by base address */
    EMBI_VEC(embi_js_page_ref) free_pages; /* the page pool: pages with no size class */
    EMBI_VEC(embi_js_page_ref) unswept[EMBI_JS_NUM_CLASSES]; /* queued for lazy sweep, per class */
    EMBI_VEC(embi_js_page_ref) young; /* pages given free cells since the last collection began */
    EMBI_VEC(embi_js_cell *) remembered; /* old cells that may point at young cells */
    embi_hashmap large; /* cell address -> size, for cells above EMBI_JS_MAX_SMALL */
    uint8_t class_of[EMBI_JS_MAX_SMALL / 16u + 1u];
    size_t unswept_total;     /* pages queued for lazy sweep and not yet swept */
    size_t live_bytes;        /* bytes of cells allocated and not yet swept free */
    size_t bytes_since_gc;    /* bytes allocated since the last collection, external ones included */
    size_t live_after_gc;     /* live bytes (cells and external) at the end of the last collection */
    size_t major_live;        /* the same at the end of the last major collection */
    size_t old_growth;        /* bytes promoted by minor collections since the last major one */
    size_t major_limit;       /* a generational collection turns major once old_growth reaches it */
    size_t threshold;         /* bytes_since_gc above this starts a collection */
    uint32_t gc_hold;         /* FP1a: embi_js_gc_hold_begin nesting; the budget does not trigger a collection while > 0 */
    size_t min_trigger;       /* floor of the allocation budget (embi_js_set_gc_params) */
    double trigger_factor;    /* cap on the growth factor (embi_js_set_gc_params) */
    double alloc_rate;        /* moving average of bytes allocated per ms of mutator time */
    uint64_t last_end_ns;     /* SDL_GetTicksNS at the end of the last collection */
    uint32_t stress_every;    /* test hook: collect every N allocations (0 = off) */
    uint32_t stress_left;
    size_t gc_count;          /* collections, minor and major */
    size_t minor_count;
    size_t major_count;
    size_t remembered_peak;   /* largest remembered set seen */
    size_t released_chunks;   /* chunks returned to the allocator */
    uint32_t next_chunk_pages; /* pages of the next chunk to carve (J5m3: 4, doubling to 16) */
    bool generational;        /* sticky marks and minor collections (off by default) */
    bool release_pending;     /* a collection's sweep may leave whole chunks free */
    bool in_gc;
} embi_js_heap;

struct embi_js_tracer {
    embi_js_heap *heap;
    EMBI_VEC(embi_js_cell *) gray; /* explicit mark stack, never recursion */
    EMBI_VEC(embi_js_ephemeron) ephemerons; /* weak tables reached in this collection */
    /* Bytes of storage that marked cells own outside the heap (element and slot arrays,
     * dictionaries, shape tables, bound arguments). Trace functions add to it; the collector
     * stores the total as the external part of live_after_gc. */
    size_t external;
    /* Size-class bytes of the cells this collection marked (the promoted bytes of a minor one). */
    size_t marked_bytes;
    /* The chunk of the last cell whose bytes were counted: marks cluster, so most lookups hit it. */
    embi_js_chunk *chunk_hint;
#ifdef EMBI_JS_GC_VERIFY
    embi_hashmap *seen;     /* verifier: cells reached by its full mark (nullptr otherwise) */
    embi_js_cell *owner;    /* verifier: the cell whose trace is running */
    uint32_t field;         /* verifier: index of the current mark within the owner's trace */
#endif
};

/* The generational heap of this thread, or nullptr. The write barrier reads it. One generational
 * runtime per thread: a second one would take the barrier over. */
extern _Thread_local embi_js_heap *embi_js_wb_heap;

/* Write barrier. Every store of a value into a heap cell goes through this macro, as
 * EMBI_JS_WB(owner_cell, value_stored). It costs one thread-local load while generational mode is
 * off. With it on, embi_js_wb_slow remembers an old owner that now holds a young cell.
 *
 * `value` must be an embi_js_value (a 64-bit NaN-boxed word) that is a cell or an immediate. A
 * pointer to a C-heap struct is not a cell: the slow path would read its first bytes as a cell
 * header. Attach such a struct with EMBI_JS_WB_STRUCT(owner) instead. It goes to the slow path as
 * a uint64_t directly, never through uintptr_t, which would truncate a boxed value on a 32-bit
 * target. The barrier target is per thread (embi_js_wb_heap): a generational runtime must run on
 * the thread that enabled generational mode, and every entry point must run inside
 * embi_js_enter/embi_js_leave (runtime.h). A store made outside an entered call goes to the last
 * generational runtime used on the thread. */
#define EMBI_JS_WB(owner, value)                                                               \
    do {                                                                                       \
        if (embi_js_wb_heap != nullptr) {                                                      \
            /* J5n1: a young owner (mark 0) needs no barrier: the call is made only for an old one. */\
            embi_js_cell *wbo_ = (embi_js_cell *)(owner);                                      \
            if (wbo_ != nullptr && wbo_->mark != 0) {                                          \
                embi_js_wb_slow(embi_js_wb_heap, wbo_, (uint64_t)(value));                     \
            }                                                                                  \
        }                                                                                      \
    } while (0)

/* Attaches a C-heap struct (or a cell that holds cells) to owner: owner->internal = s. The struct
 * is not a cell, so no value is inspected; an old owner is remembered outright. Use it right
 * after the attach, and barrier every cell the struct holds on owner when it is written. */
#define EMBI_JS_WB_STRUCT(owner)                                                                   do {                                                                                               if (embi_js_wb_heap != nullptr) {                                                                  embi_js_wb_struct_slow(embi_js_wb_heap, (embi_js_cell *)(owner));                          }                                                                                          } while (0)

/* Store of value into the field lv (an lvalue inside the cell owner): the value is evaluated once,
 * the barrier runs right before the store, so a value created after a collection that promoted
 * owner is still barriered. Use it for every store of a value into a cell or a cell-owned buffer,
 * even when the value is immediate (the slow path returns at once). */
#define EMBI_JS_STORE(owner, lv, value)                                                        \
    do {                                                                                       \
        typeof(value) embi_wv_ = (value);                                                      \
        EMBI_JS_WB((owner), embi_wv_);                                                         \
        (lv) = embi_wv_;                                                                       \
    } while (0)

/* Records growth of storage that a cell owns outside the heap. The bytes count toward the next
 * collection's trigger right away. Frees need no call: each collection recomputes the live
 * external total from the marked cells (see embi_js_tracer.external). */
static inline void embi_js_heap_note_external(embi_js_heap *h, size_t bytes)
{
    h->bytes_since_gc += bytes;
}

void embi_js_heap_init(embi_js_heap *h);
/* Runs every live cell's finalizer and releases all heap memory. */
void embi_js_heap_destroy(embi_js_heap *h);
void embi_js_heap_set_ops(embi_js_heap *h, uint8_t type, embi_js_type_ops ops);

/* Allocates a zeroed cell of `bytes` bytes (at least the header) with the given type. May run a
 * collection first when the allocation budget is spent. */
[[nodiscard]] embi_js_cell *embi_js_alloc_cell(embi_js_runtime *rt, size_t bytes, uint8_t type);

/* Returns the cell whose first byte is addr, or nullptr. Interior addresses do not match. After a
 * collection that left pages to sweep lazily, dead cells on those pages still look allocated;
 * embi_js_gc finishes the sweep, so it gives exact answers. */
embi_js_cell *embi_js_heap_find_cell(const embi_js_heap *h, uintptr_t addr);

/* Chunk i in address order (0 <= i < h->chunks.len): its first byte, its page count and how
 * many of its pages are in the pool. For tests and stats; nothing else reads the chunk table. */
void embi_js_heap_chunk_info(const embi_js_heap *h, uint32_t i, uintptr_t *base, uint32_t *npages,
                             uint32_t *pooled);

/* Marking API for trace functions and root tracers. */
void embi_js_mark_value(embi_js_tracer *t, embi_js_value v);
void embi_js_mark_cell(embi_js_tracer *t, embi_js_cell *c);
/* Registers a weak table during marking (see embi_js_ephemeron). data stays valid until the
 * collection ends. Each table is traced once per collection, when its owner is marked. */
void embi_js_trace_ephemeron(embi_js_tracer *t, void *data, embi_js_ephemeron_mark_fn mark,
                             embi_js_ephemeron_clear_fn clear);

/* FP1a: a compile holds the allocation-budget trigger off (a collection in the middle of it only marks
 * a half-built unit). The budget is only deferred, never forgiven: what the compile allocated stays
 * counted, and a collection that became due runs at the first allocation after the hold. A budget
 * below the constant is a stress setting (the tests collect at every allocation): a hold does not
 * defer it, and the stress hook always collects. Calls nest; an unbalanced end clamps at 0. */
#define EMBI_JS_GC_HOLD_MIN_BUDGET (64u * 1024u)
void embi_js_gc_hold_begin(embi_js_runtime *rt);
void embi_js_gc_hold_end(embi_js_runtime *rt);

/* FP2a: forgiving what a compile allocated, for code that is kept for the runtime's life (a module: the registry never
 * frees one, so its cells are not garbage in the making and need not count toward the next collection). The mark is taken
 * before the compile; embi_js_gc_forgive_since lowers bytes_since_gc back to it when it has grown past it and no
 * collection has reset it meanwhile. Nothing else is forgiven: a loop of eval or new Function still counts every byte. */
size_t embi_js_gc_alloc_mark(const embi_js_runtime *rt);
void embi_js_gc_forgive_since(embi_js_runtime *rt, size_t mark);

/* Runs a full collection (stop-the-world) and finishes its sweep. */
void embi_js_gc(embi_js_runtime *rt);
/* Runs a minor collection when generational mode is on, otherwise a full one. Finishes the sweep. */
void embi_js_gc_minor(embi_js_runtime *rt);
/* CF2: a collection ahead of need, for the embedder's idle time (a frame with nothing to do). When half the allocation
 * budget is spent it runs the collection that the budget would start later, in the middle of the next script: major when
 * the old generation has grown by half the major limit (or, not generational, half the budget is allocated), else minor.
 * Returns 2 after a major collection, 1 after a minor one, 0 when nothing was due (or a collection cannot run now: inside
 * one, during a compile hold, or with the stress hook on). An idle heap stays at 0: nothing is allocated. */
int embi_js_gc_idle(embi_js_runtime *rt);

/* Generational mode. Turning it off drops every sticky mark, so the next collection is major. */
void embi_js_gc_set_generational(embi_js_runtime *rt, bool on);
/* Test hook: collect before every `every`-th allocation (0 turns it off). */
void embi_js_gc_set_stress(embi_js_runtime *rt, uint32_t every);
/* Recomputes the allocation budget from the current parameters (embi_js_set_gc_params). */
void embi_js_gc_retune(embi_js_heap *h);
/* Test hook: the non-free small cells with a clear mark. In a generational heap after a minor that
 * is the cells the collection did not sweep (zombies) plus none else, because survivors keep marks. */
size_t embi_js_gc_count_unmarked(const embi_js_heap *h);
/* J5n2 diagnostic: non-free cells by type (count and size-class bytes) after finishing the sweep. */
void embi_js_gc_census(embi_js_heap *h, size_t count[EMBI_JS_CELL_TYPE_COUNT],
                       size_t bytes[EMBI_JS_CELL_TYPE_COUNT],
                       void (*visit)(const embi_js_cell *, void *), void *user);
/* Slow path of EMBI_JS_WB: owner (old) now holds bits (a young cell). Use the macro. */
void embi_js_wb_slow(embi_js_heap *h, embi_js_cell *owner, uint64_t bits);
/* Slow path of EMBI_JS_WB_STRUCT: remembers owner when it is old. */
void embi_js_wb_struct_slow(embi_js_heap *h, embi_js_cell *owner);
