/*
 * Map, Set, WeakMap and WeakSet (ECMA-262 24.1 to 24.4), the ES2025 Set methods, Map.groupBy and
 * Object.groupBy, and the Map and Set iterators. The fast-iteration hook for J2c is declared in
 * js/collections.h.
 *
 * Storage. A collection keeps its entries in insertion order in one array of (key, value) pairs.
 * Deleting an entry turns it into a tombstone (key EMPTY), so the other entries keep their
 * positions. A hash index maps keys to positions by linear probing. It has one slot per entry
 * position since the last rebuild, so it never holds deleted slots and stays at most half full.
 * An insert that finds the entry array full rebuilds it: compacting when at least half of it is
 * tombstones, doubling otherwise.
 *
 * Cursors. A Map or Set iterator is an object whose internal state is a cursor (a position in the
 * entry array). The collection keeps a registry of weak handles to the cursors of its iterators. A
 * rebuild moves each live cursor to the position of the same entry, and a finished or collected
 * cursor is dropped from the registry at the next rebuild or purge. Native loops that run user code
 * (forEach, the ES2025 Set methods) use a cursor too, so the code they call may mutate the
 * collection.
 *
 * Weak collections. WeakMap and WeakSet keep their entries in the same kind of table, but the
 * collector traces the table as an ephemeron (heap.h): a value is kept alive only while its key is.
 *
 * Intrinsics. The prototypes and the functions that other code identifies (js/collections.h) live
 * in a value array cell, the record. A root hook marks it, and coll_record finds it through the
 * runtime's root list, so neither the runtime nor the realm needs an extra field.
 */
#include "js/bigint.h"
#include "js/builtins.h"
#include "js/collections.h"
#include "js/heap.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/shape.h"
#include "js/string.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define NOT_FOUND UINT32_MAX
#define MIN_CAP 8u
#define MIN_INDEX 16u

/* Keeps a cell reachable up to this point. A native loop that reads a cell only through a pointer
 * derived from it would otherwise let the optimizer drop the last reference the collector sees. */
#define KEEP_ALIVE(p) __asm__ volatile("" : : "r"(p) : "memory")

enum coll_kind {
    KIND_MAP,
    KIND_SET,
    KIND_WEAKMAP,
    KIND_WEAKSET,
};

/* Method selectors: the magic value of each native function. The name tables below follow the
 * same order. */
enum {
    M_GET,
    M_HAS,
    M_SET,
    M_DELETE,
    M_CLEAR,
    M_FOREACH,
    M_KEYS,
    M_VALUES,
    M_ENTRIES,
    M_GET_OR_INSERT,          /* ES2026 upsert (J4f) */
    M_GET_OR_INSERT_COMPUTED,
};

enum {
    S_ADD,
    S_HAS,
    S_DELETE,
    S_CLEAR,
    S_FOREACH,
    S_VALUES,
    S_ENTRIES,
    S_UNION,
    S_INTERSECTION,
    S_DIFFERENCE,
    S_SYMMETRIC_DIFFERENCE,
    S_IS_SUBSET_OF,
    S_IS_SUPERSET_OF,
    S_IS_DISJOINT_FROM,
};

enum {
    WM_GET,
    WM_HAS,
    WM_SET,
    WM_DELETE,
    WM_GET_OR_INSERT,
    WM_GET_OR_INSERT_COMPUTED,
};

enum {
    WS_ADD,
    WS_HAS,
    WS_DELETE,
};

static const char *const k_map_names[] = {"get",     "has",    "set",     "delete", "clear",
                                          "forEach", "keys",   "values",  "entries",
                                          "getOrInsert", "getOrInsertComputed"};
static const char *const k_set_names[] = {
    "add",    "has",     "delete",     "clear",     "forEach",           "values",
    "entries", "union",  "intersection", "difference", "symmetricDifference", "isSubsetOf",
    "isSupersetOf", "isDisjointFrom"};
static const char *const k_weakmap_names[] = {"get", "has", "set", "delete", "getOrInsert",
                                              "getOrInsertComputed"};
static const char *const k_weakset_names[] = {"add", "has", "delete"};

/* ------------------------------------------------------------------ data structures */

/* The entries of one collection. Its storage is plain heap memory owned by the table. The trace
 * and finalize hooks of the owning object account for it and release it. */
typedef struct coll_table {
    embi_js_runtime *rt;
    embi_js_cell *owner;     /* the Map, Set, ... object, for the write barrier */
    embi_js_value *kv;       /* cap (key, value) pairs; key EMPTY marks a deleted entry */
    uint32_t *index;         /* idx_slots slots: 0 is free, otherwise entry position + 1 */
    uint32_t idx_slots;      /* power of two; 0 while kv is null */
    uint32_t cap;            /* entry positions available in kv */
    uint32_t len;            /* entry positions in use, deleted ones included */
    uint32_t live;           /* entries not deleted */
    uint32_t *iters;         /* weak handle ids of this collection's cursors */
    uint32_t iters_len;
    uint32_t iters_cap;
    uint32_t iters_purge_at; /* registry size that triggers a purge */
} coll_table;

/* The internal state of a Map or Set iterator. */
typedef struct coll_cursor {
    embi_js_value coll; /* the Map or Set object */
    uint32_t pos;       /* next entry position to examine */
    uint8_t kind;       /* enum embi_js_coll_iter_kind */
    bool done;
} coll_cursor;

/* An iterator record of the iterator protocol. */
typedef struct iter_rec {
    embi_js_value iter;
    embi_js_value next;
} iter_rec;

/* A Set Record of the ES2025 set-method protocol (GetSetRecord). */
typedef struct set_rec {
    embi_js_value obj;
    double size;
    embi_js_value has;
    embi_js_value keys;
} set_rec;

static void coll_root_mark(embi_js_tracer *t, void *user);
static embi_js_value close_throwing(embi_js_runtime *rt, const iter_rec *ir);
static embi_js_value cursor_next_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic);

/* ------------------------------------------------------------------ table storage */

static size_t kv_bytes(uint32_t cap)
{
    return (size_t)cap * 2u * sizeof(embi_js_value);
}

static uint32_t index_slots_for(uint32_t cap)
{
    uint32_t n = MIN_INDEX;
    while (n < cap * 2u) {
        n <<= 1;
    }
    return n;
}

static size_t table_bytes(const coll_table *t)
{
    return kv_bytes(t->cap) + (size_t)t->idx_slots * sizeof(uint32_t) +
           (size_t)t->iters_cap * sizeof(uint32_t);
}

static coll_table *table_attach(embi_js_runtime *rt, embi_js_object *o)
{
    coll_table *t = embi_calloc(1, sizeof *t);
    t->rt = rt;
    t->owner = &o->hdr;
    o->internal = t;
    EMBI_JS_WB_STRUCT(o);
    return t;
}

/* Normalizes a key: -0 becomes +0, integral numbers become int32, and NaN is canonical. After this,
 * SameValueZero is bit equality, except for strings, which compare by content. */
static embi_js_value norm_key(embi_js_value v)
{
    if (embi_js_is_double(v)) {
        double d = embi_js_as_double(v);
        if (d == 0.0) {
            return embi_js_from_int32(0);
        }
        return embi_js_number(d);
    }
    return v;
}

static uint64_t key_hash(embi_js_value k)
{
    if (embi_js_is_bigint(k)) {
        return embi_hash64(embi_js_bigint_hash(embi_js_as_bigint(k)));
    }
    uint64_t h = embi_js_is_string(k) ? (uint64_t)embi_js_str_hash(embi_js_as_string(k)) : k;
    return embi_hash64(h);
}

/* SameValueZero on normalized keys: BigInts are equal by value, strings by content. */
static bool key_eq(embi_js_value a, embi_js_value b)
{
    if (a == b) {
        return true;
    }
    if (embi_js_is_bigint(a) && embi_js_is_bigint(b)) {
        return embi_js_bigint_compare(embi_js_as_bigint(a), embi_js_as_bigint(b)) == 0;
    }
    return embi_js_is_string(a) && embi_js_is_string(b) &&
           embi_js_str_equal(embi_js_as_string(a), embi_js_as_string(b));
}

/* Position of the live entry with normalized key k, or NOT_FOUND. */
static uint32_t table_find(const coll_table *t, embi_js_value k)
{
    if (t->live == 0) {
        return NOT_FOUND;
    }
    uint32_t mask = t->idx_slots - 1u;
    uint32_t i = (uint32_t)key_hash(k) & mask;
    for (;;) {
        uint32_t slot = t->index[i];
        if (slot == 0) {
            return NOT_FOUND;
        }
        if (key_eq(t->kv[2u * (slot - 1u)], k)) {
            return slot - 1u;
        }
        i = (i + 1u) & mask;
    }
}

static void index_put(coll_table *t, uint32_t pos, embi_js_value k)
{
    uint32_t mask = t->idx_slots - 1u;
    uint32_t i = (uint32_t)key_hash(k) & mask;
    while (t->index[i] != 0) {
        i = (i + 1u) & mask;
    }
    t->index[i] = pos + 1u;
}

/* Walks the cursor registry. Cursors whose iterator is gone or done are dropped. With remap, live
 * cursors move to remap[pos]; with zero, they move to 0 (clear). */
static void iters_sweep(coll_table *t, const uint32_t *remap, bool zero)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < t->iters_len; i++) {
        uint32_t id = t->iters[i];
        embi_js_value v = embi_js_weak_get(t->rt, id);
        bool keep = false;
        if (v != EMBI_JS_EMPTY) {
            coll_cursor *c = embi_js_as_object(v)->internal;
            if (!c->done) {
                keep = true;
                if (zero) {
                    c->pos = 0;
                } else if (remap) {
                    c->pos = remap[c->pos];
                }
            }
        }
        if (keep) {
            t->iters[w++] = id;
        } else {
            embi_js_weak_free(t->rt, id);
        }
    }
    t->iters_len = w;
}

static void iters_register(coll_table *t, embi_js_value cursor_obj)
{
    if (t->iters_len >= t->iters_purge_at) {
        iters_sweep(t, nullptr, false);
        uint32_t want = t->iters_len * 2u;
        t->iters_purge_at = want > MIN_CAP * 2u ? want : MIN_CAP * 2u;
    }
    if (t->iters_len == t->iters_cap) {
        uint32_t ncap = t->iters_cap ? t->iters_cap * 2u : MIN_CAP;
        t->iters = embi_realloc(t->iters, (size_t)t->iters_cap * sizeof(uint32_t),
                                (size_t)ncap * sizeof(uint32_t));
        embi_js_heap_note_external(&t->rt->heap,
                                   (size_t)(ncap - t->iters_cap) * sizeof(uint32_t));
        t->iters_cap = ncap;
    }
    t->iters[t->iters_len++] = embi_js_weak_new(t->rt, cursor_obj);
}

/* Moves the entries into fresh storage of newcap positions and drops the tombstones. The cursors
 * move with them: remap sends each old position to its new one. */
static void table_rebuild(coll_table *t, uint32_t newcap)
{
    uint32_t old_len = t->len;
    uint32_t *remap = nullptr;
    if (t->iters_len != 0) {
        remap = embi_alloc((size_t)(old_len + 1u) * sizeof *remap);
    }
    embi_js_value *nkv = embi_alloc(kv_bytes(newcap));
    uint32_t w = 0;
    for (uint32_t i = 0; i < old_len; i++) {
        if (remap) {
            remap[i] = w;
        }
        embi_js_value k = t->kv[2u * i];
        if (k != EMBI_JS_EMPTY) {
            nkv[2u * w] = k;
            nkv[2u * w + 1u] = t->kv[2u * i + 1u];
            w++;
        }
    }
    if (remap) {
        remap[old_len] = w;
    }
    if (t->kv) {
        embi_free(t->kv, kv_bytes(t->cap));
    }
    if (t->index) {
        embi_free(t->index, (size_t)t->idx_slots * sizeof(uint32_t));
    }
    t->kv = nkv;
    t->cap = newcap;
    t->len = w;
    t->idx_slots = index_slots_for(newcap);
    t->index = embi_calloc(t->idx_slots, sizeof(uint32_t));
    embi_js_heap_note_external(&t->rt->heap,
                               kv_bytes(newcap) + (size_t)t->idx_slots * sizeof(uint32_t));
    for (uint32_t e = 0; e < t->len; e++) {
        index_put(t, e, t->kv[2u * e]);
    }
    if (remap) {
        iters_sweep(t, remap, false);
        embi_free(remap, (size_t)(old_len + 1u) * sizeof *remap);
    }
}

/* Adds an entry whose normalized key is not present. */
static void table_insert(coll_table *t, embi_js_value k, embi_js_value v)
{
    if (t->len == t->cap) {
        uint32_t want = t->live + 1u;
        uint32_t ncap = t->cap;
        if (want * 2u > t->cap) {
            ncap = t->cap ? t->cap * 2u : MIN_CAP;
        }
        table_rebuild(t, ncap);
    }
    uint32_t pos = t->len++;
    EMBI_JS_WB(t->owner, k);
    EMBI_JS_WB(t->owner, v);
    t->kv[2u * pos] = k;
    t->kv[2u * pos + 1u] = v;
    t->live++;
    index_put(t, pos, k);
}

/* Deletes the entry with normalized key k. The index keeps its slot, and lookups skip the
 * tombstone. */
static bool table_remove(coll_table *t, embi_js_value k)
{
    uint32_t pos = table_find(t, k);
    if (pos == NOT_FOUND) {
        return false;
    }
    t->kv[2u * pos] = EMBI_JS_EMPTY;
    t->kv[2u * pos + 1u] = EMBI_JS_EMPTY;
    t->live--;
    return true;
}

static void table_clear(coll_table *t)
{
    iters_sweep(t, nullptr, true);
    t->len = 0;
    t->live = 0;
    if (t->index) {
        memset(t->index, 0, (size_t)t->idx_slots * sizeof(uint32_t));
    }
}

static bool can_hold_weakly(embi_js_value v)
{
    if (embi_js_is_object(v)) {
        return true;
    }
    return embi_js_is_symbol(v) && !embi_js_symbol_is_registered(v);
}

/* ------------------------------------------------------------------ GC hooks */

static void map_trace(embi_js_object *o, embi_js_tracer *t)
{
    coll_table *tb = o->internal;
    if (!tb) {
        return;
    }
    for (uint32_t i = 0; i < tb->len; i++) {
        if (tb->kv[2u * i] != EMBI_JS_EMPTY) {
            embi_js_mark_value(t, tb->kv[2u * i]);
            embi_js_mark_value(t, tb->kv[2u * i + 1u]);
        }
    }
    t->external += table_bytes(tb);
}

/* Ephemeron rule: a value is marked only when its key is. Returns true when it marked a value. */
static bool weak_mark(embi_js_tracer *t, void *data)
{
    coll_table *tb = data;
    bool any = false;
    for (uint32_t i = 0; i < tb->len; i++) {
        embi_js_value k = tb->kv[2u * i];
        if (k == EMBI_JS_EMPTY) {
            continue;
        }
        embi_js_value v = tb->kv[2u * i + 1u];
        if (embi_js_as_cell(k)->mark && embi_js_is_cell(v) && !embi_js_as_cell(v)->mark) {
            embi_js_mark_value(t, v);
            any = true;
        }
    }
    return any;
}

/* Removes the entries whose key was not marked. Runs before the sweep. */
static void weak_clear(void *data)
{
    coll_table *tb = data;
    for (uint32_t i = 0; i < tb->len; i++) {
        embi_js_value k = tb->kv[2u * i];
        if (k != EMBI_JS_EMPTY && !embi_js_as_cell(k)->mark) {
            tb->kv[2u * i] = EMBI_JS_EMPTY;
            tb->kv[2u * i + 1u] = EMBI_JS_EMPTY;
            tb->live--;
        }
    }
}

static void weak_trace(embi_js_object *o, embi_js_tracer *t)
{
    coll_table *tb = o->internal;
    if (!tb) {
        return;
    }
    embi_js_trace_ephemeron(t, tb, weak_mark, weak_clear);
    t->external += table_bytes(tb);
}

static void table_finalize(embi_js_object *o)
{
    coll_table *t = o->internal;
    if (!t) {
        return;
    }
    for (uint32_t i = 0; i < t->iters_len; i++) {
        embi_js_weak_free(t->rt, t->iters[i]);
    }
    if (t->iters) {
        embi_free(t->iters, (size_t)t->iters_cap * sizeof(uint32_t));
    }
    if (t->kv) {
        embi_free(t->kv, kv_bytes(t->cap));
    }
    if (t->index) {
        embi_free(t->index, (size_t)t->idx_slots * sizeof(uint32_t));
    }
    embi_free(t, sizeof *t);
    o->internal = nullptr;
}

static void cursor_trace(embi_js_object *o, embi_js_tracer *t)
{
    coll_cursor *c = o->internal;
    if (c) {
        embi_js_mark_value(t, c->coll);
    }
}

static void cursor_finalize(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(coll_cursor));
        o->internal = nullptr;
    }
}

/* ------------------------------------------------------------------ classes */

static const embi_js_class class_map = {
    .name = "Map", .trace = map_trace, .finalize = table_finalize};
static const embi_js_class class_set = {
    .name = "Set", .trace = map_trace, .finalize = table_finalize};
static const embi_js_class class_weakmap = {
    .name = "WeakMap", .trace = weak_trace, .finalize = table_finalize};
static const embi_js_class class_weakset = {
    .name = "WeakSet", .trace = weak_trace, .finalize = table_finalize};
static const embi_js_class class_map_iter = {
    .name = "Map Iterator", .trace = cursor_trace, .finalize = cursor_finalize};
static const embi_js_class class_set_iter = {
    .name = "Set Iterator", .trace = cursor_trace, .finalize = cursor_finalize};

static embi_js_value coll_construct(embi_js_runtime *rt, enum coll_kind kind, int argc,
                                    const embi_js_value *argv, embi_js_value new_target);

static embi_js_value map_ctor_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                   int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "Constructor Map requires 'new'");
}

static embi_js_value map_ctor_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                        const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    return coll_construct(rt, KIND_MAP, argc, argv, new_target);
}

static embi_js_value set_ctor_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                   int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "Constructor Set requires 'new'");
}

static embi_js_value set_ctor_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                        const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    return coll_construct(rt, KIND_SET, argc, argv, new_target);
}

static embi_js_value weakmap_ctor_call(embi_js_runtime *rt, embi_js_object *f,
                                       embi_js_value this_v, int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "Constructor WeakMap requires 'new'");
}

static embi_js_value weakmap_ctor_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                            const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    return coll_construct(rt, KIND_WEAKMAP, argc, argv, new_target);
}

static embi_js_value weakset_ctor_call(embi_js_runtime *rt, embi_js_object *f,
                                       embi_js_value this_v, int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "Constructor WeakSet requires 'new'");
}

static embi_js_value weakset_ctor_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                            const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    return coll_construct(rt, KIND_WEAKSET, argc, argv, new_target);
}

static const embi_js_class class_map_ctor = {
    .name = "Function", .call = map_ctor_call, .construct = map_ctor_construct};
static const embi_js_class class_set_ctor = {
    .name = "Function", .call = set_ctor_call, .construct = set_ctor_construct};
static const embi_js_class class_weakmap_ctor = {
    .name = "Function", .call = weakmap_ctor_call, .construct = weakmap_ctor_construct};
static const embi_js_class class_weakset_ctor = {
    .name = "Function", .call = weakset_ctor_call, .construct = weakset_ctor_construct};

/* ------------------------------------------------------------------ records and helpers */

static embi_js_varray *coll_record(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == coll_root_mark) {
            return rt->roots.data[i].user;
        }
    }
    EMBI_ASSERT(false);
    return nullptr;
}

static void coll_root_mark(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, user);
}

static void set_slot(embi_js_varray *rec, enum embi_js_coll_slot which, embi_js_value v)
{
    EMBI_JS_WB(rec, v);
    EMBI_JS_STORE(rec, rec->elems[which], v);
}

static enum embi_js_coll_slot proto_slot(enum coll_kind kind)
{
    switch (kind) {
    case KIND_MAP:
        return EMBI_JS_COLL_MAP_PROTO;
    case KIND_SET:
        return EMBI_JS_COLL_SET_PROTO;
    case KIND_WEAKMAP:
        return EMBI_JS_COLL_WEAKMAP_PROTO;
    case KIND_WEAKSET:
        return EMBI_JS_COLL_WEAKSET_PROTO;
    }
    return EMBI_JS_COLL_MAP_PROTO;
}

static enum embi_js_coll_slot adder_slot(enum coll_kind kind)
{
    switch (kind) {
    case KIND_MAP:
        return EMBI_JS_COLL_MAP_SET;
    case KIND_SET:
        return EMBI_JS_COLL_SET_ADD;
    case KIND_WEAKMAP:
        return EMBI_JS_COLL_WEAKMAP_SET;
    case KIND_WEAKSET:
        return EMBI_JS_COLL_WEAKSET_ADD;
    }
    return EMBI_JS_COLL_MAP_SET;
}

static const embi_js_class *kind_class(enum coll_kind kind)
{
    switch (kind) {
    case KIND_MAP:
        return &class_map;
    case KIND_SET:
        return &class_set;
    case KIND_WEAKMAP:
        return &class_weakmap;
    case KIND_WEAKSET:
        return &class_weakset;
    }
    return &class_map;
}

static bool is_obj_of(embi_js_value v, const embi_js_class *k)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == k;
}

/* The table of a receiver of the given class, or nullptr. */
static coll_table *table_of(embi_js_value v, const embi_js_class *k)
{
    if (!is_obj_of(v, k)) {
        return nullptr;
    }
    return embi_js_as_object(v)->internal;
}

static embi_js_value incompatible(embi_js_runtime *rt, const char *type, const char *method)
{
    return embi_js_throw_type_error(rt, "%s.prototype.%s called on incompatible receiver", type,
                                    method);
}

/* A new empty Map, Set, WeakMap or WeakSet with its intrinsic prototype. */
static embi_js_object *coll_new(embi_js_runtime *rt, enum coll_kind kind)
{
    embi_js_varray *rec = coll_record(rt);
    embi_js_object *o =
        embi_js_object_new_class(rt, rec->elems[proto_slot(kind)], kind_class(kind));
    table_attach(rt, o);
    return o;
}

/* GetPrototypeFromConstructor for the collection constructors: the new_target's prototype when it
 * is an object, else the intrinsic fallback. The realm of new_target is not tracked (one realm per
 * runtime). */
static embi_js_value proto_for_new(embi_js_runtime *rt, embi_js_value new_target,
                                   embi_js_value fallback)
{
    if (!embi_js_is_object(new_target)) {
        return fallback;
    }
    embi_js_value p = embi_js_get(rt, embi_js_as_object(new_target),
                                  embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
    if (p == EXC) {
        return EXC;
    }
    return embi_js_is_object(p) ? p : fallback;
}

static embi_js_value pair_new(embi_js_runtime *rt, embi_js_value k, embi_js_value v)
{
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    if (embi_js_create_data_property_or_throw(rt, a, embi_js_key_from_index(0), k) !=
        EMBI_JS_YES) {
        return EXC;
    }
    if (embi_js_create_data_property_or_throw(rt, a, embi_js_key_from_index(1), v) !=
        EMBI_JS_YES) {
        return EXC;
    }
    return embi_js_from_object(a);
}

static embi_js_value iter_result(embi_js_runtime *rt, embi_js_value v, bool done)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_VALUE), v, EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "done"), embi_js_from_bool(done),
                       EMBI_JS_ATTR_DEFAULT);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ cursors */

/* Creates an iterator object over coll (whose table is t) and registers its cursor. */
static embi_js_object *cursor_new(embi_js_runtime *rt, embi_js_object *coll, coll_table *t,
                                  enum embi_js_coll_iter_kind kind, bool map_side)
{
    embi_js_varray *rec = coll_record(rt);
    embi_js_value proto = rec->elems[map_side ? EMBI_JS_COLL_MAP_ITER_PROTO
                                              : EMBI_JS_COLL_SET_ITER_PROTO];
    embi_js_object *it = embi_js_object_new_class(rt, proto,
                                                  map_side ? &class_map_iter : &class_set_iter);
    coll_cursor *c = embi_calloc(1, sizeof *c);
    c->coll = embi_js_from_object(coll);
    EMBI_JS_WB(it, c->coll);
    c->kind = (uint8_t)kind;
    it->internal = c;
    EMBI_JS_WB_STRUCT(it);
    iters_register(t, embi_js_from_object(it));
    return it;
}

/* Advances a cursor without running user code. */
static bool cursor_next(const coll_table *t, coll_cursor *c, embi_js_value *k, embi_js_value *v)
{
    if (c->done) {
        return false;
    }
    while (c->pos < t->len) {
        uint32_t p = c->pos++;
        if (t->kv[2u * p] != EMBI_JS_EMPTY) {
            *k = t->kv[2u * p];
            *v = t->kv[2u * p + 1u];
            return true;
        }
    }
    c->done = true;
    return false;
}

static embi_js_value cursor_value(embi_js_runtime *rt, uint8_t kind, embi_js_value k,
                                  embi_js_value v)
{
    if (kind == EMBI_JS_COLL_ITER_KEYS) {
        return k;
    }
    if (kind == EMBI_JS_COLL_ITER_VALUES) {
        return v;
    }
    return pair_new(rt, k, v);
}

/* %MapIteratorPrototype%.next (magic 0) and %SetIteratorPrototype%.next (magic 1). */
static embi_js_value cursor_next_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    bool map_side = magic == 0;
    if (!is_obj_of(this_v, map_side ? &class_map_iter : &class_set_iter)) {
        if (map_side) {
            return embi_js_throw_type_error(rt, "Map Iterator next called on incompatible receiver");
        }
        return embi_js_throw_type_error(rt, "Set Iterator next called on incompatible receiver");
    }
    coll_cursor *c = embi_js_as_object(this_v)->internal;
    coll_table *t = embi_js_as_object(c->coll)->internal;
    embi_js_value k = UNDEF;
    embi_js_value v = UNDEF;
    if (!cursor_next(t, c, &k, &v)) {
        return iter_result(rt, UNDEF, true);
    }
    embi_js_value val = cursor_value(rt, c->kind, k, v);
    if (val == EXC) {
        return EXC;
    }
    return iter_result(rt, val, false);
}

/* ------------------------------------------------------------------ the iterator protocol */

/* GetIterator(v, sync). Returns 0, or -1 with an exception pending. */
static int get_iterator(embi_js_runtime *rt, embi_js_value v, iter_rec *out)
{
    embi_js_value method = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return -1;
    }
    if (method == UNDEF) {
        embi_js_throw_type_error(rt, "Value is not iterable");
        return -1;
    }
    embi_js_value it = embi_js_call(rt, method, v, 0, nullptr);
    if (it == EXC) {
        return -1;
    }
    if (!embi_js_is_object(it)) {
        embi_js_throw_type_error(rt, "Result of the Symbol.iterator method is not an object");
        return -1;
    }
    embi_js_value next = embi_js_get(rt, embi_js_as_object(it), embi_js_key_c(rt, "next"));
    if (next == EXC) {
        return -1;
    }
    out->iter = it;
    out->next = next;
    return 0;
}

/* IteratorStepValue: 1 with the value in *value, 0 when done, -1 when it threw. */
static int iter_step(embi_js_runtime *rt, const iter_rec *ir, embi_js_value *value)
{
    embi_js_value res = embi_js_call(rt, ir->next, ir->iter, 0, nullptr);
    if (res == EXC) {
        return -1;
    }
    if (!embi_js_is_object(res)) {
        embi_js_throw_type_error(rt, "Iterator result is not an object");
        return -1;
    }
    embi_js_value done = embi_js_get(rt, embi_js_as_object(res), embi_js_key_c(rt, "done"));
    if (done == EXC) {
        return -1;
    }
    if (embi_js_to_boolean(done)) {
        return 0;
    }
    embi_js_value v = embi_js_get(rt, embi_js_as_object(res), embi_js_key_c(rt, "value"));
    if (v == EXC) {
        return -1;
    }
    *value = v;
    return 1;
}

/* IteratorClose. A throw completion ignores the errors of return (the caller restores the original
 * exception). A normal completion lets them propagate: returns 0, or -1 when they threw. */
static int iter_close(embi_js_runtime *rt, const iter_rec *ir, bool throwing)
{
    embi_js_value ret = embi_js_get_method(rt, ir->iter, embi_js_key_c(rt, "return"));
    if (ret == EXC) {
        return throwing ? 0 : -1;
    }
    if (ret == UNDEF) {
        return 0;
    }
    embi_js_value res = embi_js_call(rt, ret, ir->iter, 0, nullptr);
    if (throwing) {
        return 0;
    }
    if (res == EXC) {
        return -1;
    }
    if (!embi_js_is_object(res)) {
        embi_js_throw_type_error(rt, "Iterator result is not an object");
        return -1;
    }
    return 0;
}

/* Closes an iterator on a throw completion and returns EXC with the original exception pending. */
static embi_js_value close_throwing(embi_js_runtime *rt, const iter_rec *ir)
{
    embi_js_value saved = embi_js_exception_get(rt);
    iter_close(rt, ir, true);
    embi_js_exception_set(rt, saved);
    return EXC;
}

/* ------------------------------------------------------------------ entries */

/* Adds or updates an entry. Returns UNDEF, or EXC when the key cannot be held weakly. */
static embi_js_value coll_put(embi_js_runtime *rt, enum coll_kind kind, coll_table *t,
                              embi_js_value k, embi_js_value v)
{
    if (kind == KIND_WEAKMAP && !can_hold_weakly(k)) {
        return embi_js_throw_type_error(rt, "Invalid value used as weak map key");
    }
    if (kind == KIND_WEAKSET && !can_hold_weakly(k)) {
        return embi_js_throw_type_error(rt, "Invalid value used in weak set");
    }
    if (kind == KIND_MAP) {
        k = norm_key(k);
    } else if (kind == KIND_SET) {
        k = norm_key(k);
        v = k;
    } else if (kind == KIND_WEAKSET) {
        v = EMBI_JS_TRUE;
    }
    uint32_t pos = table_find(t, k);
    if (pos != NOT_FOUND) {
        EMBI_JS_WB(t->owner, v);
        t->kv[2u * pos + 1u] = v;
    } else {
        table_insert(t, k, v);
    }
    return UNDEF;
}

/* The constructors of Map, Set, WeakMap and WeakSet. The adder of the prototype is called for each
 * entry of the iterable, unless it is the intrinsic one, which is inlined. */
static embi_js_value coll_construct(embi_js_runtime *rt, enum coll_kind kind, int argc,
                                    const embi_js_value *argv, embi_js_value new_target)
{
    embi_js_varray *rec = coll_record(rt);
    embi_js_value proto = proto_for_new(rt, new_target, rec->elems[proto_slot(kind)]);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, kind_class(kind));
    coll_table *t = table_attach(rt, o);
    embi_js_value iterable = argc > 0 ? argv[0] : UNDEF;
    if (embi_js_is_nullish(iterable)) {
        return embi_js_from_object(o);
    }
    bool entries = kind == KIND_MAP || kind == KIND_WEAKMAP;
    embi_js_value adder = embi_js_get(rt, o, embi_js_key_c(rt, entries ? "set" : "add"));
    if (adder == EXC) {
        return EXC;
    }
    if (!embi_js_is_callable(adder)) {
        return embi_js_throw_type_error(rt, "The adder of the collection is not callable");
    }
    bool direct = adder == rec->elems[adder_slot(kind)];
    iter_rec ir;
    if (get_iterator(rt, iterable, &ir) != 0) {
        return EXC;
    }
    for (;;) {
        embi_js_value item = UNDEF;
        int st = iter_step(rt, &ir, &item);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        if (entries) {
            if (!embi_js_is_object(item)) {
                embi_js_throw_type_error(rt, "Iterator value is not an entry object");
                return close_throwing(rt, &ir);
            }
            embi_js_object *entry = embi_js_as_object(item);
            embi_js_value k = embi_js_get(rt, entry, embi_js_key_from_index(0));
            if (k == EXC) {
                return close_throwing(rt, &ir);
            }
            embi_js_value v = embi_js_get(rt, entry, embi_js_key_from_index(1));
            if (v == EXC) {
                return close_throwing(rt, &ir);
            }
            if (direct) {
                if (coll_put(rt, kind, t, k, v) == EXC) {
                    return close_throwing(rt, &ir);
                }
            } else {
                embi_js_value args[2] = {k, v};
                if (embi_js_call(rt, adder, embi_js_from_object(o), 2, args) == EXC) {
                    return close_throwing(rt, &ir);
                }
            }
        } else if (direct) {
            if (coll_put(rt, kind, t, item, item) == EXC) {
                return close_throwing(rt, &ir);
            }
        } else if (embi_js_call(rt, adder, embi_js_from_object(o), 1, &item) == EXC) {
            return close_throwing(rt, &ir);
        }
    }
    return embi_js_from_object(o);
}

/* forEach over a Map (map_side) or a Set. The loop uses a cursor, so the callback may mutate. */
static embi_js_value coll_for_each(embi_js_runtime *rt, embi_js_value this_v, coll_table *t,
                                   bool map_side, embi_js_value cb, embi_js_value this_arg)
{
    if (!embi_js_is_callable(cb)) {
        if (map_side) {
            return embi_js_throw_type_error(rt, "Map.prototype.forEach callback is not a function");
        }
        return embi_js_throw_type_error(rt, "Set.prototype.forEach callback is not a function");
    }
    embi_js_object *it = cursor_new(rt, embi_js_as_object(this_v), t,
                                    EMBI_JS_COLL_ITER_ENTRIES, map_side);
    coll_cursor *c = it->internal;
    for (;;) {
        embi_js_value k = UNDEF;
        embi_js_value v = UNDEF;
        if (!cursor_next(t, c, &k, &v)) {
            break;
        }
        embi_js_value args[3] = {v, k, this_v};
        if (embi_js_call(rt, cb, this_arg, 3, args) == EXC) {
            KEEP_ALIVE(it);
            return EXC;
        }
    }
    KEEP_ALIVE(it);
    return UNDEF;
}

/* ------------------------------------------------------------------ Map and Set methods */

static embi_js_value map_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    coll_table *t = table_of(this_v, &class_map);
    if (!t) {
        return incompatible(rt, "Map", k_map_names[magic]);
    }
    switch (magic) {
    case M_GET: {
        uint32_t pos = table_find(t, norm_key(a0));
        return pos == NOT_FOUND ? UNDEF : t->kv[2u * pos + 1u];
    }
    case M_HAS:
        return embi_js_from_bool(table_find(t, norm_key(a0)) != NOT_FOUND);
    case M_SET:
        if (coll_put(rt, KIND_MAP, t, a0, a1) == EXC) {
            return EXC;
        }
        return this_v;
    case M_DELETE:
        return embi_js_from_bool(table_remove(t, norm_key(a0)));
    case M_CLEAR:
        table_clear(t);
        return UNDEF;
    case M_FOREACH:
        return coll_for_each(rt, this_v, t, true, a0, a1);
    case M_KEYS:
        return embi_js_from_object(
            cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_KEYS, true));
    case M_VALUES:
        return embi_js_from_object(
            cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_VALUES, true));
    case M_ENTRIES:
        return embi_js_from_object(
            cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_ENTRIES, true));
    case M_GET_OR_INSERT:
    case M_GET_OR_INSERT_COMPUTED: {
        /* getOrInsert(key, value) and getOrInsertComputed(key, callback): the key is normalized
         * (-0 becomes +0) before the lookup and before the callback sees it. The callback runs
         * only when the key is absent, and whatever it did to the map is overwritten by its
         * result (the entry is then set, not appended twice). */
        bool computed = magic == M_GET_OR_INSERT_COMPUTED;
        if (computed && !embi_js_is_callable(a1)) {
            return embi_js_throw_type_error(rt, "Map.prototype.getOrInsertComputed: callback is not a function");
        }
        embi_js_value k = norm_key(a0);
        uint32_t pos = table_find(t, k);
        if (pos != NOT_FOUND) {
            return t->kv[2u * pos + 1u];
        }
        embi_js_value v = a1;
        if (computed) {
            embi_js_value args[1] = {k};
            v = embi_js_call(rt, a1, UNDEF, 1, args);
            if (v == EXC) {
                return EXC;
            }
        }
        if (coll_put(rt, KIND_MAP, t, k, v) == EXC) {
            return EXC;
        }
        return v;
    }
    default:
        return UNDEF;
    }
}

/* The size getters of Map (magic 0) and Set (magic 1). */
static embi_js_value coll_size(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    bool map_side = magic == 0;
    coll_table *t = table_of(this_v, map_side ? &class_map : &class_set);
    if (!t) {
        if (map_side) {
            return embi_js_throw_type_error(r->rt,
                                            "get Map.prototype.size called on incompatible receiver");
        }
        return embi_js_throw_type_error(r->rt,
                                        "get Set.prototype.size called on incompatible receiver");
    }
    return embi_js_number((double)t->live);
}

static embi_js_value set_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic);

/* ------------------------------------------------------------------ ES2025 Set methods */

/* GetSetRecord. Returns 0, or -1 with an exception pending. */
static int get_set_record(embi_js_runtime *rt, embi_js_value v, set_rec *out)
{
    if (!embi_js_is_object(v)) {
        embi_js_throw_type_error(rt, "Set operation argument is not an object");
        return -1;
    }
    embi_js_object *o = embi_js_as_object(v);
    embi_js_value raw = embi_js_get(rt, o, embi_js_key_c(rt, "size"));
    if (raw == EXC) {
        return -1;
    }
    double n;
    if (!embi_js_to_number(rt, raw, &n)) {
        return -1;
    }
    if (n != n) {
        embi_js_throw_type_error(rt, "Set operation size is not a number");
        return -1;
    }
    double size;
    if (!embi_js_to_integer_or_infinity(rt, embi_js_number(n), &size)) {
        return -1;
    }
    if (size < 0) {
        embi_js_throw_range_error(rt, "Set operation size must not be negative");
        return -1;
    }
    embi_js_value has = embi_js_get(rt, o, embi_js_key_c(rt, "has"));
    if (has == EXC) {
        return -1;
    }
    if (!embi_js_is_callable(has)) {
        embi_js_throw_type_error(rt, "Set operation has is not a function");
        return -1;
    }
    embi_js_value keys = embi_js_get(rt, o, embi_js_key_c(rt, "keys"));
    if (keys == EXC) {
        return -1;
    }
    if (!embi_js_is_callable(keys)) {
        embi_js_throw_type_error(rt, "Set operation keys is not a function");
        return -1;
    }
    out->obj = v;
    out->size = size;
    out->has = has;
    out->keys = keys;
    return 0;
}

/* GetKeysIterator: calls keys on the other set and checks the result. Returns 0 or -1. */
static int get_keys_iter(embi_js_runtime *rt, const set_rec *rec, iter_rec *out)
{
    embi_js_value it = embi_js_call(rt, rec->keys, rec->obj, 0, nullptr);
    if (it == EXC) {
        return -1;
    }
    if (!embi_js_is_object(it)) {
        embi_js_throw_type_error(rt, "keys() result is not an object");
        return -1;
    }
    embi_js_value next = embi_js_get(rt, embi_js_as_object(it), embi_js_key_c(rt, "next"));
    if (next == EXC) {
        return -1;
    }
    if (!embi_js_is_callable(next)) {
        embi_js_throw_type_error(rt, "keys() iterator next is not a function");
        return -1;
    }
    out->iter = it;
    out->next = next;
    return 0;
}

/* ToBoolean(Call(has, other, k)): 1 or 0, or -1 when it threw. */
static int set_has(embi_js_runtime *rt, const set_rec *rec, embi_js_value k)
{
    embi_js_value r = embi_js_call(rt, rec->has, rec->obj, 1, &k);
    if (r == EXC) {
        return -1;
    }
    return embi_js_to_boolean(r) ? 1 : 0;
}

/* Copies the live entries of src into the empty table dst. No user code runs. */
static void table_copy(coll_table *dst, const coll_table *src)
{
    for (uint32_t i = 0; i < src->len; i++) {
        embi_js_value k = src->kv[2u * i];
        if (k != EMBI_JS_EMPTY) {
            table_insert(dst, k, k);
        }
    }
}

static embi_js_value set_union(embi_js_runtime *rt, embi_js_value this_v, coll_table *t,
                               embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    iter_rec kit;
    if (get_keys_iter(rt, &rec, &kit) != 0) {
        return EXC;
    }
    embi_js_object *res = coll_new(rt, KIND_SET);
    coll_table *res_t = res->internal;
    table_copy(res_t, t);
    for (;;) {
        embi_js_value v = UNDEF;
        int st = iter_step(rt, &kit, &v);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        embi_js_value k = norm_key(v);
        if (table_find(res_t, k) == NOT_FOUND) {
            table_insert(res_t, k, k);
        }
    }
    KEEP_ALIVE(this_v);
    return embi_js_from_object(res);
}

static embi_js_value set_intersection(embi_js_runtime *rt, embi_js_value this_v, coll_table *t,
                                      embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    embi_js_object *res = coll_new(rt, KIND_SET);
    coll_table *res_t = res->internal;
    if ((double)t->live <= rec.size) {
        embi_js_object *it = cursor_new(rt, embi_js_as_object(this_v), t,
                                        EMBI_JS_COLL_ITER_KEYS, false);
        coll_cursor *c = it->internal;
        for (;;) {
            embi_js_value k = UNDEF;
            embi_js_value v = UNDEF;
            if (!cursor_next(t, c, &k, &v)) {
                break;
            }
            int in_other = set_has(rt, &rec, k);
            if (in_other < 0) {
                KEEP_ALIVE(it);
                return EXC;
            }
            if (in_other && table_find(res_t, k) == NOT_FOUND) {
                table_insert(res_t, k, k);
            }
        }
        KEEP_ALIVE(it);
    } else {
        iter_rec kit;
        if (get_keys_iter(rt, &rec, &kit) != 0) {
            return EXC;
        }
        for (;;) {
            embi_js_value v = UNDEF;
            int st = iter_step(rt, &kit, &v);
            if (st < 0) {
                return EXC;
            }
            if (st == 0) {
                break;
            }
            embi_js_value k = norm_key(v);
            if (table_find(t, k) != NOT_FOUND && table_find(res_t, k) == NOT_FOUND) {
                table_insert(res_t, k, k);
            }
        }
    }
    KEEP_ALIVE(this_v);
    return embi_js_from_object(res);
}

static embi_js_value set_difference(embi_js_runtime *rt, embi_js_value this_v, coll_table *t,
                                    embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    embi_js_object *res = coll_new(rt, KIND_SET);
    coll_table *res_t = res->internal;
    table_copy(res_t, t);
    if ((double)t->live <= rec.size) {
        embi_js_object *it = cursor_new(rt, embi_js_as_object(this_v), t,
                                        EMBI_JS_COLL_ITER_KEYS, false);
        coll_cursor *c = it->internal;
        for (;;) {
            embi_js_value k = UNDEF;
            embi_js_value v = UNDEF;
            if (!cursor_next(t, c, &k, &v)) {
                break;
            }
            int in_other = set_has(rt, &rec, k);
            if (in_other < 0) {
                KEEP_ALIVE(it);
                return EXC;
            }
            if (in_other) {
                table_remove(res_t, k);
            }
        }
        KEEP_ALIVE(it);
    } else {
        iter_rec kit;
        if (get_keys_iter(rt, &rec, &kit) != 0) {
            return EXC;
        }
        for (;;) {
            embi_js_value v = UNDEF;
            int st = iter_step(rt, &kit, &v);
            if (st < 0) {
                return EXC;
            }
            if (st == 0) {
                break;
            }
            table_remove(res_t, norm_key(v));
        }
    }
    KEEP_ALIVE(this_v);
    return embi_js_from_object(res);
}

static embi_js_value set_symmetric_difference(embi_js_runtime *rt, embi_js_value this_v,
                                              coll_table *t, embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    iter_rec kit;
    if (get_keys_iter(rt, &rec, &kit) != 0) {
        return EXC;
    }
    embi_js_object *res = coll_new(rt, KIND_SET);
    coll_table *res_t = res->internal;
    table_copy(res_t, t);
    for (;;) {
        embi_js_value v = UNDEF;
        int st = iter_step(rt, &kit, &v);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        embi_js_value k = norm_key(v);
        if (table_find(t, k) != NOT_FOUND) {
            table_remove(res_t, k);
        } else if (table_find(res_t, k) == NOT_FOUND) {
            table_insert(res_t, k, k);
        }
    }
    KEEP_ALIVE(this_v);
    return embi_js_from_object(res);
}

static embi_js_value set_is_subset_of(embi_js_runtime *rt, embi_js_value this_v, coll_table *t,
                                      embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    if ((double)t->live > rec.size) {
        return EMBI_JS_FALSE;
    }
    embi_js_object *it = cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_KEYS,
                                    false);
    coll_cursor *c = it->internal;
    for (;;) {
        embi_js_value k = UNDEF;
        embi_js_value v = UNDEF;
        if (!cursor_next(t, c, &k, &v)) {
            break;
        }
        int in_other = set_has(rt, &rec, k);
        if (in_other < 0) {
            KEEP_ALIVE(it);
            return EXC;
        }
        if (!in_other) {
            KEEP_ALIVE(it);
            return EMBI_JS_FALSE;
        }
    }
    KEEP_ALIVE(it);
    return EMBI_JS_TRUE;
}

static embi_js_value set_is_superset_of(embi_js_runtime *rt, embi_js_value this_v,
                                        coll_table *t, embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    if ((double)t->live < rec.size) {
        return EMBI_JS_FALSE;
    }
    iter_rec kit;
    if (get_keys_iter(rt, &rec, &kit) != 0) {
        return EXC;
    }
    for (;;) {
        embi_js_value v = UNDEF;
        int st = iter_step(rt, &kit, &v);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        if (table_find(t, norm_key(v)) == NOT_FOUND) {
            if (iter_close(rt, &kit, false) != 0) {
                return EXC;
            }
            return EMBI_JS_FALSE;
        }
    }
    KEEP_ALIVE(this_v);
    return EMBI_JS_TRUE;
}

static embi_js_value set_is_disjoint_from(embi_js_runtime *rt, embi_js_value this_v,
                                          coll_table *t, embi_js_value other)
{
    set_rec rec;
    if (get_set_record(rt, other, &rec) != 0) {
        return EXC;
    }
    if ((double)t->live <= rec.size) {
        embi_js_object *it = cursor_new(rt, embi_js_as_object(this_v), t,
                                        EMBI_JS_COLL_ITER_KEYS, false);
        coll_cursor *c = it->internal;
        for (;;) {
            embi_js_value k = UNDEF;
            embi_js_value v = UNDEF;
            if (!cursor_next(t, c, &k, &v)) {
                break;
            }
            int in_other = set_has(rt, &rec, k);
            if (in_other < 0) {
                KEEP_ALIVE(it);
                return EXC;
            }
            if (in_other) {
                KEEP_ALIVE(it);
                return EMBI_JS_FALSE;
            }
        }
        KEEP_ALIVE(it);
    } else {
        iter_rec kit;
        if (get_keys_iter(rt, &rec, &kit) != 0) {
            return EXC;
        }
        for (;;) {
            embi_js_value v = UNDEF;
            int st = iter_step(rt, &kit, &v);
            if (st < 0) {
                return EXC;
            }
            if (st == 0) {
                break;
            }
            if (table_find(t, norm_key(v)) != NOT_FOUND) {
                if (iter_close(rt, &kit, false) != 0) {
                    return EXC;
                }
                return EMBI_JS_FALSE;
            }
        }
    }
    KEEP_ALIVE(this_v);
    return EMBI_JS_TRUE;
}

static embi_js_value set_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    coll_table *t = table_of(this_v, &class_set);
    if (!t) {
        return incompatible(rt, "Set", k_set_names[magic]);
    }
    switch (magic) {
    case S_ADD:
        if (coll_put(rt, KIND_SET, t, a0, a0) == EXC) {
            return EXC;
        }
        return this_v;
    case S_HAS:
        return embi_js_from_bool(table_find(t, norm_key(a0)) != NOT_FOUND);
    case S_DELETE:
        return embi_js_from_bool(table_remove(t, norm_key(a0)));
    case S_CLEAR:
        table_clear(t);
        return UNDEF;
    case S_FOREACH:
        return coll_for_each(rt, this_v, t, false, a0, a1);
    case S_VALUES:
        return embi_js_from_object(
            cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_VALUES, false));
    case S_ENTRIES:
        return embi_js_from_object(
            cursor_new(rt, embi_js_as_object(this_v), t, EMBI_JS_COLL_ITER_ENTRIES, false));
    case S_UNION:
        return set_union(rt, this_v, t, a0);
    case S_INTERSECTION:
        return set_intersection(rt, this_v, t, a0);
    case S_DIFFERENCE:
        return set_difference(rt, this_v, t, a0);
    case S_SYMMETRIC_DIFFERENCE:
        return set_symmetric_difference(rt, this_v, t, a0);
    case S_IS_SUBSET_OF:
        return set_is_subset_of(rt, this_v, t, a0);
    case S_IS_SUPERSET_OF:
        return set_is_superset_of(rt, this_v, t, a0);
    case S_IS_DISJOINT_FROM:
        return set_is_disjoint_from(rt, this_v, t, a0);
    default:
        return UNDEF;
    }
}

/* ------------------------------------------------------------------ WeakMap and WeakSet */

static embi_js_value weakmap_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    coll_table *t = table_of(this_v, &class_weakmap);
    if (!t) {
        return incompatible(rt, "WeakMap", k_weakmap_names[magic]);
    }
    switch (magic) {
    case WM_GET: {
        if (!can_hold_weakly(a0)) {
            return UNDEF;
        }
        uint32_t pos = table_find(t, a0);
        return pos == NOT_FOUND ? UNDEF : t->kv[2u * pos + 1u];
    }
    case WM_HAS:
        return embi_js_from_bool(can_hold_weakly(a0) && table_find(t, a0) != NOT_FOUND);
    case WM_SET:
        if (coll_put(rt, KIND_WEAKMAP, t, a0, a1) == EXC) {
            return EXC;
        }
        return this_v;
    case WM_DELETE:
        return embi_js_from_bool(can_hold_weakly(a0) && table_remove(t, a0));
    case WM_GET_OR_INSERT:
    case WM_GET_OR_INSERT_COMPUTED: {
        /* The callback (when there is one) is checked first, then the key. The key must be
         * holdable weakly before the lookup, and the callback runs only when it is absent. */
        bool computed = magic == WM_GET_OR_INSERT_COMPUTED;
        if (computed && !embi_js_is_callable(a1)) {
            return embi_js_throw_type_error(rt, "WeakMap.prototype.getOrInsertComputed: callback is not a function");
        }
        if (!can_hold_weakly(a0)) {
            return embi_js_throw_type_error(rt, "Invalid value used as weak map key");
        }
        uint32_t pos = table_find(t, a0);
        if (pos != NOT_FOUND) {
            return t->kv[2u * pos + 1u];
        }
        embi_js_value v = a1;
        if (computed) {
            embi_js_value args[1] = {a0};
            v = embi_js_call(rt, a1, UNDEF, 1, args);
            if (v == EXC) {
                return EXC;
            }
        }
        if (coll_put(rt, KIND_WEAKMAP, t, a0, v) == EXC) {
            return EXC;
        }
        return v;
    }
    default:
        return UNDEF;
    }
}

static embi_js_value weakset_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    coll_table *t = table_of(this_v, &class_weakset);
    if (!t) {
        return incompatible(rt, "WeakSet", k_weakset_names[magic]);
    }
    switch (magic) {
    case WS_ADD:
        if (coll_put(rt, KIND_WEAKSET, t, a0, UNDEF) == EXC) {
            return EXC;
        }
        return this_v;
    case WS_HAS:
        return embi_js_from_bool(can_hold_weakly(a0) && table_find(t, a0) != NOT_FOUND);
    case WS_DELETE:
        return embi_js_from_bool(can_hold_weakly(a0) && table_remove(t, a0));
    default:
        return UNDEF;
    }
}

/* ------------------------------------------------------------------ groupBy */

static int array_push(embi_js_runtime *rt, embi_js_object *a, embi_js_value v)
{
    double len;
    if (!embi_js_length_of_array_like(rt, a, &len)) {
        return -1;
    }
    return embi_js_create_data_property_or_throw(rt, a, embi_js_key_from_index((uint32_t)len),
                                                 v) == EMBI_JS_YES
               ? 0
               : -1;
}

/* GroupBy. Returns a Map whose keys are the group keys and whose values are the arrays of items
 * (keys are normalized as Map keys, or as property keys when prop_keys is set). Returns nullptr
 * with an exception pending. */
static embi_js_object *group_items(embi_js_runtime *rt, embi_js_value items, embi_js_value cb,
                                   bool prop_keys)
{
    if (embi_js_is_nullish(items)) {
        embi_js_throw_type_error(rt, "groupBy called on null or undefined");
        return nullptr;
    }
    if (!embi_js_is_callable(cb)) {
        embi_js_throw_type_error(rt, "groupBy callback is not a function");
        return nullptr;
    }
    embi_js_object *groups = coll_new(rt, KIND_MAP);
    coll_table *gt = groups->internal;
    iter_rec ir;
    if (get_iterator(rt, items, &ir) != 0) {
        return nullptr;
    }
    double index = 0;
    for (;;) {
        embi_js_value item = UNDEF;
        int st = iter_step(rt, &ir, &item);
        if (st < 0) {
            return nullptr;
        }
        if (st == 0) {
            break;
        }
        embi_js_value args[2] = {item, embi_js_number(index)};
        embi_js_value key = embi_js_call(rt, cb, UNDEF, 2, args);
        if (key == EXC) {
            close_throwing(rt, &ir);
            return nullptr;
        }
        if (prop_keys) {
            embi_js_key pk = embi_js_to_property_key(rt, key);
            if (pk == EXC) {
                close_throwing(rt, &ir);
                return nullptr;
            }
            key = pk;
        } else {
            key = norm_key(key);
        }
        uint32_t pos = table_find(gt, key);
        embi_js_value arr;
        if (pos == NOT_FOUND) {
            arr = embi_js_from_object(embi_js_array_create_default(rt, 0));
            table_insert(gt, key, arr);
        } else {
            arr = gt->kv[2u * pos + 1u];
        }
        if (array_push(rt, embi_js_as_object(arr), item) != 0) {
            close_throwing(rt, &ir);
            return nullptr;
        }
        index += 1.0;
    }
    return groups;
}

static embi_js_value map_group_by(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    embi_js_object *g = group_items(r->rt, a0, a1, false);
    if (!g) {
        return EXC;
    }
    return embi_js_from_object(g);
}

static embi_js_value object_group_by(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    embi_js_object *g = group_items(rt, a0, a1, true);
    if (!g) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new(rt, EMBI_JS_NULL);
    coll_table *gt = g->internal;
    for (uint32_t i = 0; i < gt->len; i++) {
        embi_js_value k = gt->kv[2u * i];
        if (k == EMBI_JS_EMPTY) {
            continue;
        }
        if (embi_js_create_data_property_or_throw(rt, o, k, gt->kv[2u * i + 1u]) !=
            EMBI_JS_YES) {
            KEEP_ALIVE(g);
            return EXC;
        }
    }
    KEEP_ALIVE(g);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ setup */

static embi_js_value species_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

static embi_js_object *define_fn(embi_js_realm *r, embi_js_object *o, const char *name,
                                 embi_js_native fn, int length, int magic)
{
    embi_js_object *f = embi_js_native_function_new(r, name, length, fn, magic);
    embi_js_define_raw(r->rt, o, embi_js_key_c(r->rt, name), embi_js_from_object(f), ATTR_WC);
    return f;
}

static void define_size(embi_js_realm *r, embi_js_object *proto, int magic)
{
    embi_js_object *g = embi_js_native_function_new(r, "get size", 0, coll_size, magic);
    embi_js_define_accessor_raw(r->rt, proto, embi_js_key_c(r->rt, "size"),
                                embi_js_from_object(g), UNDEF, ATTR_C);
}

static void define_species(embi_js_realm *r, embi_js_object *ctor)
{
    embi_js_object *g = embi_js_native_function_new(r, "get [Symbol.species]", 0, species_get, 0);
    embi_js_define_accessor_raw(r->rt, ctor, embi_js_wk_sym(r->rt, EMBI_JS_WK_SPECIES),
                                embi_js_from_object(g), UNDEF, ATTR_C);
}

static void define_tag(embi_js_realm *r, embi_js_object *o, const char *tag)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value s = embi_js_from_string(embi_js_atom_intern_utf8(rt, tag, strlen(tag)));
    embi_js_define_raw(rt, o, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), s, ATTR_C);
}

/* The constructor object: a function with [[Construct]], length 0 and the given prototype. */
static embi_js_object *make_ctor(embi_js_realm *r, const embi_js_class *cls, const char *name,
                                 embi_js_object *proto)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *ctor =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], cls);
    ctor->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(0.0),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, name, strlen(name))),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor), ATTR_WC);
    return ctor;
}

void embi_js_init_builtins_collections(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *rec = embi_js_varray_new(rt, EMBI_JS_COLL_COUNT);
    embi_js_root_add(rt, coll_root_mark, rec);

    /* The Map and Set iterator prototypes inherit from %IteratorPrototype% (J2c). */
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];
    embi_js_object *map_proto = embi_js_object_new(rt, obj_proto);
    embi_js_object *set_proto = embi_js_object_new(rt, obj_proto);
    embi_js_object *weakmap_proto = embi_js_object_new(rt, obj_proto);
    embi_js_object *weakset_proto = embi_js_object_new(rt, obj_proto);
    embi_js_value iter_proto = rt->protos[EMBI_JS_PROTO_ITERATOR];
    embi_js_object *map_iter_proto = embi_js_object_new(rt, iter_proto);
    embi_js_object *set_iter_proto = embi_js_object_new(rt, iter_proto);
    set_slot(rec, EMBI_JS_COLL_MAP_PROTO, embi_js_from_object(map_proto));
    set_slot(rec, EMBI_JS_COLL_SET_PROTO, embi_js_from_object(set_proto));
    set_slot(rec, EMBI_JS_COLL_WEAKMAP_PROTO, embi_js_from_object(weakmap_proto));
    set_slot(rec, EMBI_JS_COLL_WEAKSET_PROTO, embi_js_from_object(weakset_proto));
    set_slot(rec, EMBI_JS_COLL_MAP_ITER_PROTO, embi_js_from_object(map_iter_proto));
    set_slot(rec, EMBI_JS_COLL_SET_ITER_PROTO, embi_js_from_object(set_iter_proto));

    /* Map.prototype */
    define_fn(r, map_proto, "get", map_method, 1, M_GET);
    embi_js_object *map_set = define_fn(r, map_proto, "set", map_method, 2, M_SET);
    set_slot(rec, EMBI_JS_COLL_MAP_SET, embi_js_from_object(map_set));
    define_fn(r, map_proto, "has", map_method, 1, M_HAS);
    define_fn(r, map_proto, "delete", map_method, 1, M_DELETE);
    define_fn(r, map_proto, "clear", map_method, 0, M_CLEAR);
    define_fn(r, map_proto, "forEach", map_method, 1, M_FOREACH);
    define_fn(r, map_proto, "keys", map_method, 0, M_KEYS);
    define_fn(r, map_proto, "values", map_method, 0, M_VALUES);
    embi_js_object *map_entries = define_fn(r, map_proto, "entries", map_method, 0, M_ENTRIES);
    set_slot(rec, EMBI_JS_COLL_MAP_ENTRIES, embi_js_from_object(map_entries));
    define_fn(r, map_proto, "getOrInsert", map_method, 2, M_GET_OR_INSERT);
    define_fn(r, map_proto, "getOrInsertComputed", map_method, 2, M_GET_OR_INSERT_COMPUTED);
    embi_js_define_raw(rt, map_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(map_entries), ATTR_WC);
    define_size(r, map_proto, 0);
    define_tag(r, map_proto, "Map");

    /* Set.prototype */
    embi_js_object *set_add = define_fn(r, set_proto, "add", set_method, 1, S_ADD);
    set_slot(rec, EMBI_JS_COLL_SET_ADD, embi_js_from_object(set_add));
    define_fn(r, set_proto, "has", set_method, 1, S_HAS);
    define_fn(r, set_proto, "delete", set_method, 1, S_DELETE);
    define_fn(r, set_proto, "clear", set_method, 0, S_CLEAR);
    define_fn(r, set_proto, "entries", set_method, 0, S_ENTRIES);
    define_fn(r, set_proto, "forEach", set_method, 1, S_FOREACH);
    embi_js_object *set_values = define_fn(r, set_proto, "values", set_method, 0, S_VALUES);
    set_slot(rec, EMBI_JS_COLL_SET_VALUES, embi_js_from_object(set_values));
    embi_js_define_raw(rt, set_proto, embi_js_key_c(rt, "keys"), embi_js_from_object(set_values),
                       ATTR_WC);
    embi_js_define_raw(rt, set_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(set_values), ATTR_WC);
    define_fn(r, set_proto, "union", set_method, 1, S_UNION);
    define_fn(r, set_proto, "intersection", set_method, 1, S_INTERSECTION);
    define_fn(r, set_proto, "difference", set_method, 1, S_DIFFERENCE);
    define_fn(r, set_proto, "symmetricDifference", set_method, 1, S_SYMMETRIC_DIFFERENCE);
    define_fn(r, set_proto, "isSubsetOf", set_method, 1, S_IS_SUBSET_OF);
    define_fn(r, set_proto, "isSupersetOf", set_method, 1, S_IS_SUPERSET_OF);
    define_fn(r, set_proto, "isDisjointFrom", set_method, 1, S_IS_DISJOINT_FROM);
    define_size(r, set_proto, 1);
    define_tag(r, set_proto, "Set");

    /* WeakMap.prototype */
    define_fn(r, weakmap_proto, "delete", weakmap_method, 1, WM_DELETE);
    define_fn(r, weakmap_proto, "get", weakmap_method, 1, WM_GET);
    define_fn(r, weakmap_proto, "has", weakmap_method, 1, WM_HAS);
    define_fn(r, weakmap_proto, "getOrInsert", weakmap_method, 2, WM_GET_OR_INSERT);
    define_fn(r, weakmap_proto, "getOrInsertComputed", weakmap_method, 2,
              WM_GET_OR_INSERT_COMPUTED);
    embi_js_object *weakmap_set = define_fn(r, weakmap_proto, "set", weakmap_method, 2, WM_SET);
    set_slot(rec, EMBI_JS_COLL_WEAKMAP_SET, embi_js_from_object(weakmap_set));
    define_tag(r, weakmap_proto, "WeakMap");

    /* WeakSet.prototype */
    embi_js_object *weakset_add = define_fn(r, weakset_proto, "add", weakset_method, 1, WS_ADD);
    set_slot(rec, EMBI_JS_COLL_WEAKSET_ADD, embi_js_from_object(weakset_add));
    define_fn(r, weakset_proto, "delete", weakset_method, 1, WS_DELETE);
    define_fn(r, weakset_proto, "has", weakset_method, 1, WS_HAS);
    define_tag(r, weakset_proto, "WeakSet");

    /* %MapIteratorPrototype% and %SetIteratorPrototype% */
    embi_js_object *map_next = define_fn(r, map_iter_proto, "next", cursor_next_method, 0, 0);
    set_slot(rec, EMBI_JS_COLL_MAP_ITER_NEXT, embi_js_from_object(map_next));
    define_tag(r, map_iter_proto, "Map Iterator");
    embi_js_object *set_next = define_fn(r, set_iter_proto, "next", cursor_next_method, 0, 1);
    set_slot(rec, EMBI_JS_COLL_SET_ITER_NEXT, embi_js_from_object(set_next));
    define_tag(r, set_iter_proto, "Set Iterator");

    /* The constructors */
    embi_js_object *map_ctor = make_ctor(r, &class_map_ctor, "Map", map_proto);
    set_slot(rec, EMBI_JS_COLL_MAP_CTOR, embi_js_from_object(map_ctor));
    define_fn(r, map_ctor, "groupBy", map_group_by, 2, 0);
    define_species(r, map_ctor);

    embi_js_object *set_ctor = make_ctor(r, &class_set_ctor, "Set", set_proto);
    set_slot(rec, EMBI_JS_COLL_SET_CTOR, embi_js_from_object(set_ctor));
    define_species(r, set_ctor);

    make_ctor(r, &class_weakmap_ctor, "WeakMap", weakmap_proto);
    make_ctor(r, &class_weakset_ctor, "WeakSet", weakset_proto);

    /* Object.groupBy lives with the other collection grouping. The Object constructor is read
     * raw from the global object: it is a data property that the builtins created at startup. */
    embi_js_pdesc pd;
    if (embi_js_obj_get_own_property(rt, r->global, embi_js_key_c(rt, "Object"), &pd) ==
            EMBI_JS_YES &&
        embi_js_is_object(pd.value)) {
        define_fn(r, embi_js_as_object(pd.value), "groupBy", object_group_by, 2, 0);
    }
}

/* ------------------------------------------------------------------ hooks (js/collections.h) */

embi_js_value embi_js_coll_intrinsic(embi_js_realm *r, enum embi_js_coll_slot which)
{
    return coll_record(r->rt)->elems[which];
}

bool embi_js_coll_is_map(embi_js_value v)
{
    return is_obj_of(v, &class_map);
}

bool embi_js_coll_is_set(embi_js_value v)
{
    return is_obj_of(v, &class_set);
}

uint32_t embi_js_coll_size(const embi_js_object *o)
{
    if (o->klass != &class_map && o->klass != &class_set && o->klass != &class_weakmap &&
        o->klass != &class_weakset) {
        return 0;
    }
    const coll_table *t = o->internal;
    return t ? t->live : 0;
}

embi_js_object *embi_js_coll_iter_new(embi_js_runtime *rt, embi_js_object *coll,
                                      enum embi_js_coll_iter_kind kind)
{
    bool map_side = coll->klass == &class_map;
    if (!map_side && coll->klass != &class_set) {
        return nullptr;
    }
    return cursor_new(rt, coll, coll->internal, kind, map_side);
}

int embi_js_coll_iter_step(embi_js_runtime *rt, embi_js_object *iter, embi_js_value *value)
{
    if (iter->klass != &class_map_iter && iter->klass != &class_set_iter) {
        return EMBI_JS_NO;
    }
    coll_cursor *c = iter->internal;
    coll_table *t = embi_js_as_object(c->coll)->internal;
    embi_js_value k = UNDEF;
    embi_js_value v = UNDEF;
    if (!cursor_next(t, c, &k, &v)) {
        return EMBI_JS_NO;
    }
    embi_js_value val = cursor_value(rt, c->kind, k, v);
    if (val == EXC) {
        return EMBI_JS_THREW;
    }
    *value = val;
    return EMBI_JS_YES;
}
