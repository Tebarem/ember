#pragma once
/*
 * Runtime: owns the heap, the atom table, persistent and weak handles, the job queue, extra GC
 * root tracers and the pending exception. One runtime per thread.
 */
#include "js/heap.h"
#include "js/value.h"

struct embi_js_string;

/* Object model (object.c): well-known symbols, interned property names and intrinsic prototypes.
 * names and wk are filled on first use (embi_js_name_val / embi_js_wk_sym) and rooted by the
 * object model's root hook. A realm stores its intrinsic prototypes in protos[] (one set per
 * runtime for now). */
enum embi_js_wk {
    EMBI_JS_WK_ASYNC_ITERATOR,
    EMBI_JS_WK_HAS_INSTANCE,
    EMBI_JS_WK_IS_CONCAT_SPREADABLE,
    EMBI_JS_WK_ITERATOR,
    EMBI_JS_WK_MATCH,
    EMBI_JS_WK_MATCH_ALL,
    EMBI_JS_WK_REPLACE,
    EMBI_JS_WK_SEARCH,
    EMBI_JS_WK_SPECIES,
    EMBI_JS_WK_SPLIT,
    EMBI_JS_WK_TO_PRIMITIVE,
    EMBI_JS_WK_TO_STRING_TAG,
    EMBI_JS_WK_UNSCOPABLES,
    EMBI_JS_WK_COUNT
};

enum embi_js_name {
    EMBI_JS_NAME_LENGTH,
    EMBI_JS_NAME_PROTOTYPE,
    EMBI_JS_NAME_CONSTRUCTOR,
    EMBI_JS_NAME_VALUE,
    EMBI_JS_NAME_WRITABLE,
    EMBI_JS_NAME_GET,
    EMBI_JS_NAME_SET,
    EMBI_JS_NAME_ENUMERABLE,
    EMBI_JS_NAME_CONFIGURABLE,
    EMBI_JS_NAME_MESSAGE,
    EMBI_JS_NAME_NAME,
    EMBI_JS_NAME_TO_STRING,
    EMBI_JS_NAME_VALUE_OF,
    EMBI_JS_NAME_DEFAULT,
    EMBI_JS_NAME_NUMBER,
    EMBI_JS_NAME_STRING,
    EMBI_JS_NAME_BOOLEAN,
    EMBI_JS_NAME_SYMBOL,
    EMBI_JS_NAME_OBJECT,
    EMBI_JS_NAME_FUNCTION,
    EMBI_JS_NAME_UNDEFINED,
    EMBI_JS_NAME_NULL,
    EMBI_JS_NAME_TRUE,
    EMBI_JS_NAME_FALSE,
    EMBI_JS_NAME_THEN,
    EMBI_JS_NAME_RESOLVE,
    EMBI_JS_NAME_INDEX,   /* J5m2: RegExp match result properties, built by define_fresh */
    EMBI_JS_NAME_INPUT,
    EMBI_JS_NAME_GROUPS,
    EMBI_JS_NAME_INDICES,
    EMBI_JS_NAME_EMPTY,   /* J5m2: "", the name of an anonymous function (no per-closure atom lookup) */
    EMBI_JS_NAME_COUNT
};

enum embi_js_proto {
    EMBI_JS_PROTO_OBJECT,
    EMBI_JS_PROTO_FUNCTION,
    EMBI_JS_PROTO_ARRAY,
    EMBI_JS_PROTO_STRING,
    EMBI_JS_PROTO_NUMBER,
    EMBI_JS_PROTO_BOOLEAN,
    EMBI_JS_PROTO_SYMBOL,
    EMBI_JS_PROTO_ERROR,
    EMBI_JS_PROTO_TYPE_ERROR,
    EMBI_JS_PROTO_RANGE_ERROR,
    EMBI_JS_PROTO_REFERENCE_ERROR,
    EMBI_JS_PROTO_SYNTAX_ERROR,
    EMBI_JS_PROTO_EVAL_ERROR,
    EMBI_JS_PROTO_URI_ERROR,
    EMBI_JS_PROTO_AGGREGATE_ERROR,    /* %AggregateError.prototype% (J3a) */
    /* J2c: iterator intrinsics, kept here so every builtin and the interpreter reach them. The
     * prototypes are objects; the *_VALUES, *_NEXT and *_ITER entries are the functions that the
     * for-of and destructuring fast paths compare against (see iter_init in interp.c). */
    EMBI_JS_PROTO_ITERATOR,           /* %IteratorPrototype% */
    EMBI_JS_PROTO_ITERATOR_CTOR,      /* %Iterator% */
    EMBI_JS_PROTO_GENERATOR,          /* %GeneratorPrototype% */
    EMBI_JS_PROTO_GENERATOR_FUNCTION, /* %GeneratorFunction.prototype% */
    EMBI_JS_PROTO_ARRAY_ITERATOR,     /* %ArrayIteratorPrototype% */
    EMBI_JS_PROTO_STRING_ITERATOR,    /* %StringIteratorPrototype% */
    EMBI_JS_PROTO_ITER_HELPER,        /* %IteratorHelperPrototype% */
    EMBI_JS_PROTO_WRAP_ITER,          /* %WrapForValidIteratorPrototype% */
    EMBI_JS_PROTO_ARRAY_VALUES,       /* Array.prototype.values, also Array.prototype[@@iterator] */
    EMBI_JS_PROTO_ARRAY_ITER_NEXT,    /* %ArrayIteratorPrototype%.next */
    EMBI_JS_PROTO_STRING_ITER,        /* String.prototype[@@iterator] */
    EMBI_JS_PROTO_STRING_ITER_NEXT,   /* %StringIteratorPrototype%.next */
    EMBI_JS_PROTO_PROMISE,            /* %Promise.prototype% (J3a) */
    EMBI_JS_PROTO_PROMISE_CTOR,       /* %Promise%: the intrinsic identity checks compare with */
    EMBI_JS_PROTO_PROMISE_THEN,       /* %Promise.prototype.then%: the thenable fast path compares */
    /* J3b: async function and async generator intrinsics. */
    EMBI_JS_PROTO_ASYNC_FUNCTION,           /* %AsyncFunction.prototype% */
    EMBI_JS_PROTO_ASYNC_GENERATOR_FUNCTION, /* %AsyncGeneratorFunction.prototype% */
    EMBI_JS_PROTO_ASYNC_GENERATOR,          /* %AsyncGeneratorPrototype% */
    EMBI_JS_PROTO_ASYNC_ITERATOR,           /* %AsyncIteratorPrototype% */
    EMBI_JS_PROTO_ASYNC_FROM_SYNC,          /* %AsyncFromSyncIteratorPrototype% */
    /* J4b: %BigInt.prototype% (builtins_bigint.c). */
    EMBI_JS_PROTO_BIGINT,
    /* J4c: %RegExp.prototype%, the %RegExp% constructor, the intrinsic RegExp.prototype.exec (the
     * fast path compares with it) and %RegExpStringIteratorPrototype% (builtins_regexp.c). */
    EMBI_JS_PROTO_REGEXP,
    EMBI_JS_PROTO_REGEXP_CTOR,
    EMBI_JS_PROTO_REGEXP_EXEC,
    EMBI_JS_PROTO_REGEXP_STRING_ITER,
    /* Not a prototype: the empty shape cached for null-prototype objects (object.c). */
    EMBI_JS_PROTO_NULL_ROOT_SHAPE,
    EMBI_JS_PROTO_COUNT
};

typedef struct embi_js_atoms {
    struct embi_js_string **slots; /* open addressing, linear probing, power-of-two capacity */
    uint32_t cap;
    uint32_t count;
} embi_js_atoms;

/* A handle table slot. Free slots chain through `next` (ids are 1-based slot indices). */
typedef struct embi_js_handle_slot {
    embi_js_value value;
    uint32_t next;
    uint32_t used;
} embi_js_handle_slot;

typedef struct embi_js_handles {
    EMBI_VEC(embi_js_handle_slot) slots;
    uint32_t free_head; /* id of the first free slot, 0 when none */
    uint32_t live;
} embi_js_handles;

typedef void (*embi_js_root_fn)(embi_js_tracer *t, void *user);

typedef struct embi_js_root {
    embi_js_root_fn fn;
    void *user;
} embi_js_root;

/* Host hooks (jobs.c, J3a). HostReportErrors gets the exception that a job threw. The rejection
 * tracker gets the promise and EMBI_JS_REJECTION_REJECT (an unhandled rejection, reported once
 * when a drain ends) or EMBI_JS_REJECTION_HANDLE (a handler arrived after that report). */
enum embi_js_rejection_op {
    EMBI_JS_REJECTION_REJECT,
    EMBI_JS_REJECTION_HANDLE,
};
typedef void (*embi_js_report_error_fn)(embi_js_runtime *rt, embi_js_value error, void *user);
typedef void (*embi_js_rejection_fn)(embi_js_runtime *rt, embi_js_value promise, int op,
                                     void *user);

struct embi_js_stub_cache; /* ic.c: the megamorphic (shape, key) cache, nullptr until first used */

struct embi_js_runtime {
    embi_js_heap heap;
    embi_js_atoms atoms;      /* permanent in J1: every atom is a root */
    embi_js_handles persistent;
    embi_js_handles weak;     /* cleared by the collector when their target dies */
    EMBI_VEC(embi_js_root) roots;
    EMBI_VEC(embi_js_value) jobs; /* FIFO from job_head */
    uint32_t job_head;
    embi_js_value pending_exception;
    uintptr_t stack_base;     /* native stack top recorded at the outermost embi_js_enter */
    uint64_t compile_ticks;   /* FP2a: performance-counter ticks spent in the compiler (embi_js_compile_ms) */
    uint32_t nest;            /* embi_js_enter depth */
    embi_js_runtime *wb_outer; /* J5n2: the runtime that was innermost on the thread at the outermost enter */
    embi_js_value wk[EMBI_JS_WK_COUNT];          /* well-known symbols (rooted) */
    embi_js_value names[EMBI_JS_NAME_COUNT];     /* interned property names (atoms) */
    embi_js_value protos[EMBI_JS_PROTO_COUNT];   /* intrinsic prototypes (rooted) */
    struct embi_js_vm *vm;                       /* interpreter state (interp.c), or nullptr */
    void *modules;                               /* ES module registry (module.c), or nullptr */
    /* The Symbol.for registry (builtins_symbol.c): a null-prototype object from key string to
     * symbol, kept alive by a persistent handle. Per runtime, so it is shared by every realm. It is
     * created by the first Symbol.for call (0 until then). */
    uint32_t sym_registry;
    /* J3a job queue host state (jobs.c). unhandled lists the promises that were rejected with no
     * handler since the last report pass; it is a GC root from the first use. Entries are removed
     * when the pass reports them (or skips them because a handler arrived), so a reported
     * rejection is not kept alive by the runtime. unhandled_head is the next entry to report (the
     * pass can stop early when a host callback interrupts it). */
    embi_js_report_error_fn host_report_error;
    void *host_report_error_user;
    embi_js_rejection_fn host_rejection;
    void *host_rejection_user;
    EMBI_VEC(embi_js_value) unhandled;
    uint32_t unhandled_head;
    bool unhandled_rooted;
    /* J5 seam. protect: protector bits (section 4, J5e), all set at creation
     * and only ever cleared. rope_min: concatenations at least this long make a rope (section 5,
     * J5f/J5g; embi_js_set_rope_min). */
    uint32_t protect;
    uint32_t rope_min;
    /* J5m1: the megamorphic stub cache (ic.c), allocated by the first megamorphic fill and freed
     * with the runtime. Emptied by every collection. */
    struct embi_js_stub_cache *stub_cache;
    /* J6a: the legacy RegExp static properties (builtins_regexp.c). Plain C fields, written on
     * every successful match without a write barrier: the collector marks src and input as roots.
     * src is the string of the last legacy match (the caps refer to it); input is [[RegExpInput]],
     * which RegExp.input assigns (NULL after a regexp without legacy features matched). */
    struct {
        struct embi_js_string *src;
        struct embi_js_string *input;
        int32_t state; /* 1 while src and caps hold a match, 0 when a non-legacy regexp matched */
        uint32_t n;    /* capture groups kept (group 0 included, at most 10) */
        uint32_t caps[20]; /* start/end pairs; EMBI_RE_UNDEF (0xFFFFFFFF) for a group that did not take part */
    } rx_legacy;
};

typedef struct embi_js_mem_stats {
    size_t heap_live_bytes;
    size_t heap_bytes_since_gc;
    size_t heap_chunks;
    size_t large_objects;
    size_t gc_count;
    size_t atoms;
    size_t persistent_handles;
    size_t weak_handles;
    size_t minor_count;      /* J5h: minor collections (generational mode only) */
    size_t major_count;      /* J5h: full collections */
    size_t remembered_peak;  /* J5h: largest remembered set */
    size_t released_chunks;  /* J5h: chunks returned to the allocator */
    size_t live_after_gc;    /* J5n1: live bytes (cells and external) at the end of the last collection */
} embi_js_mem_stats;

/* ------------------------------------------------------------------ lifecycle */

[[nodiscard]] embi_js_runtime *embi_js_runtime_new(void);
void embi_js_runtime_free(embi_js_runtime *rt);

/* Nesting counter for an embedder call into the engine. The outermost enter records the native
 * stack top, which the interpreter uses for its native stack limit.
 *
 * Thread rule: a runtime in generational mode (embi_js_gc_set_generational) must be used only on
 * the thread that enabled it. The write barrier target is thread-local, so another thread would
 * run without a barrier. Nothing checks this at run time: it is the caller's rule. Every engine
 * entry point must run inside embi_js_enter/embi_js_leave (the barrier target is set there). */
void embi_js_enter(embi_js_runtime *rt);
void embi_js_leave(embi_js_runtime *rt);
/* FP2a: a runtime made (and its realm built) on another thread that has finished with it is taken over by the calling
 * thread: the write barrier target of this thread follows it, as it does after embi_js_runtime_new. The other thread must
 * have stopped using the runtime (joined). */
void embi_js_adopt_thread(embi_js_runtime *rt);
/* FP2a: the milliseconds this runtime has spent in the compiler (scripts, modules, eval and Function source), for the
 * first-paint breakdown. */
double embi_js_compile_ms(const embi_js_runtime *rt);
void embi_js_compile_ms_add(embi_js_runtime *rt, uint64_t ticks);

/* Address one past the highest byte of the current thread's stack, or 0 when unknown. */
uintptr_t embi_js_stack_top(void);

/* Collection tuning. The allocation budget after a collection is max(min_trigger, live * (g - 1)),
 * where g is the heap-limit growth factor of heap.h (1.5 down to 1.25 as the heap grows) capped at
 * factor. Both numbers take effect at the next collection. factor 0 with min_trigger 0 collects on
 * every allocation, which the tests use to flush out stale pointers. */
void embi_js_set_gc_params(embi_js_runtime *rt, size_t min_trigger, double factor);
void embi_js_get_mem_stats(const embi_js_runtime *rt, embi_js_mem_stats *out);
void embi_js_set_type_ops(embi_js_runtime *rt, uint8_t type, embi_js_type_ops ops);

/* ------------------------------------------------------------------ handles */

/* Persistent handles keep their value alive. Ids are never 0. */
uint32_t embi_js_persistent_new(embi_js_runtime *rt, embi_js_value v);
embi_js_value embi_js_persistent_get(const embi_js_runtime *rt, uint32_t id);
void embi_js_persistent_set(embi_js_runtime *rt, uint32_t id, embi_js_value v);
void embi_js_persistent_free(embi_js_runtime *rt, uint32_t id);

/* Weak handles do not keep their value alive. After the target dies the handle reads as
 * EMBI_JS_EMPTY. */
uint32_t embi_js_weak_new(embi_js_runtime *rt, embi_js_value v);
embi_js_value embi_js_weak_get(const embi_js_runtime *rt, uint32_t id);
void embi_js_weak_free(embi_js_runtime *rt, uint32_t id);

/* Extra roots (realm intrinsics, the VM value stack, ...). Each callback runs during every
 * collection and marks what it holds. */
void embi_js_root_add(embi_js_runtime *rt, embi_js_root_fn fn, void *user);
void embi_js_root_remove(embi_js_runtime *rt, embi_js_root_fn fn, void *user);

/* ------------------------------------------------------------------ jobs and exceptions */

void embi_js_job_push(embi_js_runtime *rt, embi_js_value job);
bool embi_js_job_pop(embi_js_runtime *rt, embi_js_value *out);

void embi_js_exception_set(embi_js_runtime *rt, embi_js_value v);
embi_js_value embi_js_exception_get(const embi_js_runtime *rt);
void embi_js_exception_clear(embi_js_runtime *rt);

/* ------------------------------------------------------------------ built-in cell kinds */

typedef struct embi_js_symbol {
    embi_js_cell hdr;
    embi_js_value description; /* string or UNDEFINED */
} embi_js_symbol;

typedef struct embi_js_box {
    embi_js_cell hdr;
    embi_js_value value;
} embi_js_box;

typedef struct embi_js_accessor {
    embi_js_cell hdr;
    embi_js_value getter;
    embi_js_value setter;
} embi_js_accessor;

/* A fixed-length array of values. Its length is hdr.aux. Unused slots hold EMBI_JS_EMPTY. */
typedef struct embi_js_varray {
    embi_js_cell hdr;
    embi_js_value elems[];
} embi_js_varray;

embi_js_symbol *embi_js_symbol_new(embi_js_runtime *rt, embi_js_value description);
embi_js_box *embi_js_box_new(embi_js_runtime *rt, embi_js_value v);
embi_js_accessor *embi_js_accessor_new(embi_js_runtime *rt, embi_js_value getter,
                                       embi_js_value setter);
embi_js_varray *embi_js_varray_new(embi_js_runtime *rt, uint32_t len);
/* Returns a new array of new_len elements holding the old ones, then EMPTY. The old array is
 * left alone and becomes garbage when unreferenced. */
embi_js_varray *embi_js_varray_grow(embi_js_runtime *rt, const embi_js_varray *a,
                                    uint32_t new_len);

static inline bool embi_js_is_symbol(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_SYMBOL;
}

static inline embi_js_symbol *embi_js_as_symbol(embi_js_value v)
{
    return (embi_js_symbol *)embi_js_as_cell(v);
}

static inline embi_js_value embi_js_from_symbol(const embi_js_symbol *s)
{
    return embi_js_from_cell(s);
}
