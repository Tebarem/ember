/*
 * Shape cells: creation, transitions, key lookup and GC hooks. See shape.h for the model.
 */
#include "js/shape.h"

static void mark_nn(embi_js_tracer *t, embi_js_cell *c)
{
    if (c) {
        embi_js_mark_cell(t, c);
    }
}

static size_t map_bytes(const embi_hashmap *m)
{
    return sizeof(embi_hashmap) + (size_t)m->cap * sizeof(embi_hash_slot);
}

static embi_js_shape *new_shape(embi_js_runtime *rt, embi_js_value proto, uint8_t flags)
{
    /* The hooks go in on the first shape rather than at runtime creation, and only when no hook
     * is registered yet: the cell type number is also free for embedders and tests. */
    if (!rt->heap.ops[EMBI_JS_CELL_SHAPE].trace && !rt->heap.ops[EMBI_JS_CELL_SHAPE].finalize) {
        embi_js_shape_ops_init(rt);
    }
    embi_js_shape *s = (embi_js_shape *)embi_js_alloc_cell(rt, sizeof(embi_js_shape),
                                                           EMBI_JS_CELL_SHAPE);
    s->hdr.flags = flags;
    s->proto = proto;
    s->enum_cache = EMBI_JS_EMPTY;
    return s;
}

embi_js_shape *embi_js_shape_new_root(embi_js_runtime *rt, embi_js_value proto)
{
    return new_shape(rt, proto, 0);
}

embi_js_shape *embi_js_shape_new_dict(embi_js_runtime *rt, embi_js_value proto)
{
    return new_shape(rt, proto, EMBI_JS_SHAPE_DICT);
}

/* Puts into a transition map and reports the buffer growth to the collector. */
static void child_map_put(embi_js_runtime *rt, embi_hashmap *m, embi_js_key k, uint64_t v)
{
    uint32_t before = m->cap;
    embi_hashmap_put(m, k, v);
    if (m->cap > before) {
        embi_js_heap_note_external(&rt->heap, (size_t)(m->cap - before) * sizeof(embi_hash_slot));
    }
}

embi_js_shape *embi_js_shape_add(embi_js_runtime *rt, embi_js_shape *s, embi_js_key k,
                                 uint8_t attrs)
{
    EMBI_ASSERT((s->hdr.flags & EMBI_JS_SHAPE_DICT) == 0);
    EMBI_ASSERT(s->count < EMBI_JS_SHAPE_MAX_CHAIN);

    /* Common case: the inline child is the one asked for. */
    if (s->child && s->child->key == k && s->child->attrs == attrs) {
        return s->child;
    }
    if (s->children) {
        uint64_t head = 0;
        if (embi_hashmap_get(s->children, k, &head)) {
            for (embi_js_shape *c = (embi_js_shape *)(uintptr_t)head; c; c = c->next_same_key) {
                if (c->attrs == attrs) {
                    return c;
                }
            }
        }
    }

    /* The transition links below are owned by s: s holds the new child n in child or in its map.
     * An old s is not traced by a minor, so each link is barriered on s; s is then remembered and
     * its trace reaches n. n itself is young, so n->parent and n->next_same_key need no barrier. */
    embi_js_shape *n = new_shape(rt, s->proto, 0);
    n->parent = s;
    n->key = k;
    n->count = s->count + 1u;
    n->attrs = attrs;
    if (!s->child) {
        EMBI_JS_STORE(s, s->child, n);
        return n;
    }
    if (!s->children) {
        /* The second child: the transition map starts with the inline child. */
        s->children = embi_calloc(1, sizeof(embi_hashmap));
        embi_js_heap_note_external(&rt->heap, sizeof(embi_hashmap));
        EMBI_JS_WB(s, embi_js_from_cell(s->child));
        child_map_put(rt, s->children, s->child->key, (uint64_t)(uintptr_t)s->child);
    }
    /* A child with the same key but other attributes stays on the sibling list behind n. */
    uint64_t head = 0;
    if (embi_hashmap_get(s->children, k, &head)) {
        EMBI_JS_WB(n, head);
        n->next_same_key = (embi_js_shape *)(uintptr_t)head;
    }
    EMBI_JS_WB(s, embi_js_from_cell(n));
    child_map_put(rt, s->children, k, (uint64_t)(uintptr_t)n);
    return n;
}

static void build_table(embi_js_shape *s)
{
    s->table = embi_calloc(1, sizeof(embi_hashmap));
    for (embi_js_shape *n = s; n->count > 0; n = n->parent) {
        embi_hashmap_put(s->table, n->key, (uint64_t)(uintptr_t)n);
    }
}

embi_js_shape *embi_js_shape_find(embi_js_shape *s, embi_js_key k)
{
    if (s->count > EMBI_JS_SHAPE_TABLE_MIN) {
        if (!s->table) {
            if (s->parent && s->parent->table) {
                /* J5n2: a chain built one property at a time (every builtin object, every object
                 * literal) looked up each intermediate node, and each lookup built a table of its own:
                 * 360 KB of tables for the 674 shapes of an empty realm. A node's table is its parent's
                 * plus its own key (the entries name the same ancestor nodes), so the child takes the
                 * parent's table and the parent builds a new one if it is ever looked up again. Only
                 * the child steals, never the parent, so alternating lookups cannot make them fight. */
                s->table = s->parent->table;
                s->parent->table = nullptr;
                embi_hashmap_put(s->table, s->key, (uint64_t)(uintptr_t)s);
            } else {
                build_table(s);
            }
        }
        uint64_t v = 0;
        if (embi_hashmap_get(s->table, k, &v)) {
            return (embi_js_shape *)(uintptr_t)v;
        }
        return nullptr;
    }
    for (embi_js_shape *n = s; n->count > 0; n = n->parent) {
        if (n->key == k) {
            return n;
        }
    }
    return nullptr;
}

void embi_js_shape_keys_append(const embi_js_shape *s, embi_js_key_vec *out)
{
    uint32_t base = out->len;
    embi_vec_reserve(out, base + s->count);
    const embi_js_shape *n = s;
    for (uint32_t i = s->count; i-- > 0; n = n->parent) {
        out->data[base + i] = n->key;
    }
    out->len = base + s->count;
}

/* ------------------------------------------------------------------ GC hooks */

static void mark_shape_map(embi_js_tracer *t, const embi_hashmap *m)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(m);
    uint64_t key = 0;
    uint64_t val = 0;
    while (embi_hashmap_iter_next(&it, &key, &val)) {
        mark_nn(t, &((embi_js_shape *)(uintptr_t)val)->hdr);
    }
}

static void shape_trace(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_shape *s = (embi_js_shape *)c;
    embi_js_mark_value(t, s->proto);
    mark_nn(t, s->parent ? &s->parent->hdr : nullptr);
    mark_nn(t, s->next_same_key ? &s->next_same_key->hdr : nullptr);
    embi_js_mark_value(t, s->key);
    embi_js_mark_value(t, s->enum_cache);
    mark_nn(t, s->child ? &s->child->hdr : nullptr);
    if (s->children) {
        t->external += map_bytes(s->children);
        mark_shape_map(t, s->children);
    }
    if (s->table) {
        t->external += map_bytes(s->table);
        mark_shape_map(t, s->table);
    }
}

static void free_map(embi_hashmap *m)
{
    if (m) {
        embi_hashmap_free(m);
        embi_free(m, sizeof *m);
    }
}

static void shape_finalize(embi_js_cell *c)
{
    embi_js_shape *s = (embi_js_shape *)c;
    free_map(s->children);
    free_map(s->table);
    s->children = nullptr;
    s->table = nullptr;
}

void embi_js_shape_ops_init(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){shape_trace, shape_finalize});
}
