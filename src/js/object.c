/*
 * Objects: storage (shape slots, dictionary, dense and sparse elements), ordinary internal methods
 * of ECMA-262 10.1, the exotic classes (Array, String wrapper, bound functions), and creation.
 *
 * Accounting. Element, slot, dictionary and bound-argument buffers are heap storage outside the
 * cell. Growth is reported with embi_js_heap_note_external so it counts toward the next
 * collection, and each trace function adds the buffers it owns to the tracer's external total,
 * which becomes the external part of the live size after that collection.
 */
#include "js/object.h"
#include "js/ops.h"
#include "js/string.h"

#include <string.h>

/* J5j: a plain object is 96 bytes (the 96-byte size class), down from 120. */
static_assert(sizeof(embi_js_object) == 96, "embi_js_object must stay 96 bytes");

/* Dense elements may grow to 2 * len + this gap before the object turns sparse. */
#define EMBI_JS_DENSE_GAP 1024u
/* Objects whose dictionary has this many tombstones (and more tombstones than live entries) are
 * compacted. */
#define EMBI_JS_DICT_COMPACT_MIN 8u
/* Largest number of values CreateListFromArrayLike accepts. */

#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_A EMBI_JS_ATTR_ACCESSOR
#define ATTR_LAZY EMBI_JS_ATTR_LAZY

#define UNDEF EMBI_JS_UNDEFINED

/* ------------------------------------------------------------------ dictionaries */

/* An insertion-ordered table of properties. Removed entries keep their place as tombstones
 * (key EMPTY) until the table is compacted. */
typedef struct embi_js_dict_entry {
    embi_js_key key;
    embi_js_value value;
    uint8_t attrs;
} embi_js_dict_entry;

struct embi_js_dict {
    embi_js_dict_entry *entries;
    uint32_t len;   /* entries used, tombstones included */
    uint32_t cap;
    uint32_t live;  /* entries that are not tombstones */
    embi_hashmap index; /* key -> entry position */
};

static embi_js_dict *dict_new(void)
{
    return embi_calloc(1, sizeof(embi_js_dict));
}

static void dict_free(embi_js_dict *d)
{
    embi_free(d->entries, (size_t)d->cap * sizeof *d->entries);
    embi_hashmap_free(&d->index);
    embi_free(d, sizeof *d);
}

/* Bytes the dictionary owns, for the trace function. */
static size_t dict_bytes(const embi_js_dict *d)
{
    return sizeof *d + (size_t)d->cap * sizeof *d->entries +
           (size_t)d->index.cap * sizeof(embi_hash_slot);
}

static void dict_reserve(embi_js_runtime *rt, embi_js_dict *d, uint32_t n)
{
    if (n <= d->cap) {
        return;
    }
    uint32_t ncap = embi_vec_grow_to(d->cap, n);
    d->entries = embi_realloc(d->entries, (size_t)d->cap * sizeof *d->entries,
                              (size_t)ncap * sizeof *d->entries);
    embi_js_heap_note_external(&rt->heap, (size_t)(ncap - d->cap) * sizeof *d->entries);
    d->cap = ncap;
}

/* Puts into the index and reports the index buffer growth. */
static void dict_index_put(embi_js_runtime *rt, embi_js_dict *d, embi_js_key k, uint32_t pos)
{
    uint32_t before = d->index.cap;
    embi_hashmap_put(&d->index, k, pos);
    if (d->index.cap > before) {
        embi_js_heap_note_external(&rt->heap,
                                   (size_t)(d->index.cap - before) * sizeof(embi_hash_slot));
    }
}

static embi_js_dict_entry *dict_find(const embi_js_dict *d, embi_js_key k)
{
    uint64_t pos = 0;
    if (!embi_hashmap_get(&d->index, k, &pos)) {
        return nullptr;
    }
    return &d->entries[pos];
}

/* Appends an entry to the dictionary d that owner (the object whose dictionary or sparse store d
 * is) holds. Both the key and the value are barriered on owner: a symbol key is a young cell
 * that the owner's trace must reach. */
static void dict_append(embi_js_runtime *rt, embi_js_object *owner, embi_js_dict *d, embi_js_key k,
                        embi_js_value v, uint8_t attrs)
{
    dict_reserve(rt, d, d->len + 1u);
    EMBI_JS_WB(owner, k);
    EMBI_JS_WB(owner, v);
    d->entries[d->len] = (embi_js_dict_entry){k, v, attrs};
    dict_index_put(rt, d, k, d->len);
    d->len++;
    d->live++;
}

/* Compaction only re-inserts keys that were already indexed, so the index does not grow. */
static void dict_compact(embi_js_dict *d)
{
    uint32_t j = 0;
    embi_hashmap_clear(&d->index);
    for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].key != EMBI_JS_EMPTY) {
            d->entries[j] = d->entries[i];
            embi_hashmap_put(&d->index, d->entries[j].key, j);
            j++;
        }
    }
    d->len = j;
    d->live = j;
}

static void dict_remove(embi_js_dict *d, embi_js_key k)
{
    uint64_t pos = 0;
    if (!embi_hashmap_get(&d->index, k, &pos)) {
        return;
    }
    embi_hashmap_remove(&d->index, k);
    d->entries[pos].key = EMBI_JS_EMPTY;
    d->entries[pos].value = EMBI_JS_EMPTY;
    d->live--;
    uint32_t tomb = d->len - d->live;
    if (tomb >= EMBI_JS_DICT_COMPACT_MIN && tomb > d->live) {
        dict_compact(d);
    }
}

static void dict_trace(const embi_js_dict *d, embi_js_tracer *t)
{
    for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].key != EMBI_JS_EMPTY) {
            embi_js_mark_value(t, d->entries[i].key);
            embi_js_mark_value(t, d->entries[i].value);
        }
    }
}

/* ------------------------------------------------------------------ rare state (J5j) */

/* The rare struct of o, allocated on first use. */
static embi_js_rare *rare_get(embi_js_runtime *rt, embi_js_object *o)
{
    if (!o->rare) {
        o->rare = embi_calloc(1, sizeof *o->rare);
        embi_js_heap_note_external(&rt->heap, sizeof *o->rare);
    }
    return o->rare;
}

static embi_js_shape *obj_child_root(const embi_js_object *o)
{
    return o->rare ? o->rare->child_root : nullptr;
}

/* Capacity of the dense block: kept in the value in front of the first element. */
static uint32_t elems_cap(const embi_js_object *o)
{
    return o->elements ? (uint32_t)o->elements[-1] : 0;
}

/* Bytes of the dense block including its capacity word, or 0 when there is no block. */
static size_t elems_block_bytes(uint32_t cap)
{
    return cap ? ((size_t)cap + 1u) * sizeof(embi_js_value) : 0;
}

static void elems_release(embi_js_object *o)
{
    if (o->elements) {
        embi_free(o->elements - 1, elems_block_bytes(elems_cap(o)));
        o->elements = nullptr;
    }
}

/* ------------------------------------------------------------------ GC hooks */

static void mark_nn(embi_js_tracer *t, embi_js_cell *c)
{
    if (c) {
        embi_js_mark_cell(t, c);
    }
}

static void wrapper_trace(embi_js_object *o, embi_js_tracer *t);

static void object_trace(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_object *o = (embi_js_object *)c;
    mark_nn(t, o->shape ? &o->shape->hdr : nullptr);
    embi_js_shape *root = obj_child_root(o);
    mark_nn(t, root ? &root->hdr : nullptr);
    if (o->shape && !embi_js_obj_dict(o)) {
        for (uint32_t i = 0; i < o->shape->count; i++) {
            embi_js_mark_value(t, *(i < EMBI_JS_OBJ_INLINE_SLOTS ? &o->inline_slots[i]
                                                                  : &o->slots[i - EMBI_JS_OBJ_INLINE_SLOTS]));
        }
    }
    t->external += ((size_t)o->slot_cap * sizeof(embi_js_value)) + elems_block_bytes(elems_cap(o));
    if (o->rare) {
        t->external += sizeof *o->rare;
    }
    if (embi_js_obj_dict(o)) {
        dict_trace(embi_js_obj_dict(o), t);
        t->external += dict_bytes(embi_js_obj_dict(o));
    }
    if (embi_js_obj_sparse(o)) {
        dict_trace(embi_js_obj_sparse(o), t);
        t->external += dict_bytes(embi_js_obj_sparse(o));
    }
    for (uint32_t i = 0; i < o->elements_len; i++) {
        embi_js_mark_value(t, o->elements[i]);
    }
    if (o->klass && o->klass->trace) {
        o->klass->trace(o, t);
    }
}

static void object_finalize(embi_js_cell *c)
{
    embi_js_object *o = (embi_js_object *)c;
    if (o->klass && o->klass->finalize) {
        o->klass->finalize(o);
    }
    if (o->slots) {
        embi_free(o->slots, (size_t)o->slot_cap * sizeof(embi_js_value));
    }
    elems_release(o);
    if (o->rare) {
        if (o->rare->dict) {
            dict_free(o->rare->dict);
        }
        if (o->rare->sparse) {
            dict_free(o->rare->sparse);
        }
        embi_free(o->rare, sizeof *o->rare);
        o->rare = nullptr;
    }
}

static void mark_intrinsics(embi_js_tracer *t, void *user)
{
    embi_js_runtime *rt = user;
    for (uint32_t i = 0; i < EMBI_JS_WK_COUNT; i++) {
        embi_js_mark_value(t, rt->wk[i]);
    }
    for (uint32_t i = 0; i < EMBI_JS_NAME_COUNT; i++) {
        embi_js_mark_value(t, rt->names[i]);
    }
    for (uint32_t i = 0; i < EMBI_JS_PROTO_COUNT; i++) {
        embi_js_mark_value(t, rt->protos[i]);
    }
    if (rt->rx_legacy.src) {
        embi_js_mark_value(t, embi_js_from_string(rt->rx_legacy.src));
    }
    if (rt->rx_legacy.input) {
        embi_js_mark_value(t, embi_js_from_string(rt->rx_legacy.input));
    }
}

/* ------------------------------------------------------------------ slot and element storage */

static embi_js_value *slot_at(embi_js_object *o, uint32_t i)
{
    return i < EMBI_JS_OBJ_INLINE_SLOTS ? &o->inline_slots[i]
                                        : &o->slots[i - EMBI_JS_OBJ_INLINE_SLOTS];
}

static void reserve_slots(embi_js_runtime *rt, embi_js_object *o, uint32_t count)
{
    if (count <= EMBI_JS_OBJ_INLINE_SLOTS) {
        return;
    }
    uint32_t need = count - EMBI_JS_OBJ_INLINE_SLOTS;
    if (need <= o->slot_cap) {
        return;
    }
    /* J5n2: the first out-of-line allocation is the exact need rounded up to two slots (objects with
     * 5 to 8 properties are the common case, and the generic vector policy gave each of them 8 slots,
     * 64 bytes); later growth doubles. */
    uint32_t ncap = o->slot_cap == 0 ? (need + 1u) & ~1u : o->slot_cap * 2u;
    if (ncap < need) {
        ncap = need;
    }
    o->slots = embi_realloc(o->slots, (size_t)o->slot_cap * sizeof(embi_js_value),
                            (size_t)ncap * sizeof(embi_js_value));
    embi_js_heap_note_external(&rt->heap, (size_t)(ncap - o->slot_cap) * sizeof(embi_js_value));
    o->slot_cap = ncap;
}

static void reserve_elements(embi_js_runtime *rt, embi_js_object *o, uint32_t n)
{
    uint32_t cap = elems_cap(o);
    if (n <= cap) {
        return;
    }
    /* J5n2: the first allocation is the exact need (at least 4): an array literal of five elements took
     * eight slots (72 bytes with the capacity word) under the generic vector policy. Later growth is
     * the vector policy (1.5x). */
    uint32_t ncap = cap == 0 ? (n < 4u ? 4u : n) : embi_vec_grow_to(cap, n);
    size_t old_bytes = elems_block_bytes(cap);
    size_t new_bytes = elems_block_bytes(ncap);
    embi_js_value *base = embi_realloc(o->elements ? o->elements - 1 : nullptr, old_bytes, new_bytes);
    base[0] = ncap;
    o->elements = base + 1;
    embi_js_heap_note_external(&rt->heap, new_bytes - old_bytes);
}

/* Moves dense elements to the sparse dictionary. Dense storage is released. */
static void to_sparse(embi_js_runtime *rt, embi_js_object *o)
{
    if (embi_js_obj_sparse(o)) {
        return;
    }
    o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_PACKED;
    embi_js_dict *d = dict_new();
    for (uint32_t i = 0; i < o->elements_len; i++) {
        if (o->elements[i] != EMBI_JS_EMPTY) {
            dict_append(rt, o, d, embi_js_key_from_index(i), o->elements[i], EMBI_JS_ATTR_DEFAULT);
        }
    }
    elems_release(o);
    o->elements_len = 0;
    rare_get(rt, o)->sparse = d;
}

/* Switches the object to dictionary mode: named properties move from slots to a dictionary
 * owned by a fresh dictionary shape that keeps the prototype. */
static void to_dict(embi_js_runtime *rt, embi_js_object *o);
void embi_js_obj_make_dict(embi_js_runtime *rt, embi_js_object *o)
{
    to_dict(rt, o);
}

static void to_dict(embi_js_runtime *rt, embi_js_object *o)
{
    if (embi_js_obj_dict(o)) {
        return;
    }
    embi_js_obj_lazy_proto_realize(rt, o); /* the dictionary copies slot values */
    embi_js_shape *old = o->shape;
    uint32_t n = old->count;
    embi_js_dict *d = dict_new();
    dict_reserve(rt, d, n);
    const embi_js_shape *node = old;
    for (uint32_t i = n; i-- > 0; node = node->parent) {
        EMBI_JS_WB(o, node->key);
        d->entries[i] = (embi_js_dict_entry){node->key, *slot_at(o, i), node->attrs};
        dict_index_put(rt, d, node->key, i);
    }
    d->len = n;
    d->live = n;

    /* The values stay in the old slots until the new shape exists, so a collection here is safe. */
    embi_js_shape *ds = embi_js_shape_new_dict(rt, old->proto);
    EMBI_JS_WB(o, embi_js_from_cell(ds));
    rare_get(rt, o)->dict = d;
    o->shape = ds;
    if (o->slots) {
        embi_free(o->slots, (size_t)o->slot_cap * sizeof(embi_js_value));
    }
    o->slots = nullptr;
    o->slot_cap = 0;
    for (uint32_t i = 0; i < EMBI_JS_OBJ_INLINE_SLOTS; i++) {
        EMBI_JS_WB(o, EMBI_JS_EMPTY);
        o->inline_slots[i] = EMBI_JS_EMPTY;
    }
}

/* A prototype's dictionary changed its keys or attributes: give it a fresh dictionary shape so that
 * every inline cache keyed on the old shape misses (ic.c). Value writes do not come here. The
 * prototype is kept in the new shape. */
static void dict_renew(embi_js_runtime *rt, embi_js_object *o)
{
    if (!(o->hdr.flags & EMBI_JS_OBJ_IS_PROTO)) {
        return;
    }
    embi_js_shape *ds = embi_js_shape_new_dict(rt, o->shape->proto);
    EMBI_JS_WB(o, embi_js_from_cell(ds));
    o->shape = ds;
}

/* ------------------------------------------------------------------ protectors (J5e) */

/* A store of an index key into o: an index property on a prototype breaks NO_PROTO_ELEMENTS. */
static void protect_index_store(embi_js_runtime *rt, const embi_js_object *o)
{
    if (o->hdr.flags & EMBI_JS_OBJ_IS_PROTO) {
        rt->protect &= ~EMBI_JS_PROTECT_NO_PROTO_ELEMENTS;
    }
}

/* A store or delete of a named key on a watched intrinsic (Array.prototype, %Array%,
 * %ArrayIteratorPrototype%) clears the protector that depends on that key. */
static void protect_named_store(embi_js_runtime *rt, const embi_js_object *o, embi_js_key k)
{
    if (!(o->hdr.flags & EMBI_JS_OBJ_WATCHED)) {
        return;
    }
    if (k == embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES) ||
        k == embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR)) {
        rt->protect &= ~EMBI_JS_PROTECT_ARRAY_SPECIES;
    }
    if (k == embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR) || k == embi_js_key_c(rt, "next")) {
        rt->protect &= ~EMBI_JS_PROTECT_ARRAY_ITERATOR;
    }
}

static void protect_key_store(embi_js_runtime *rt, const embi_js_object *o, embi_js_key k)
{
    if (embi_js_key_is_index(k)) {
        protect_index_store(rt, o);
    } else {
        protect_named_store(rt, o, k);
    }
}

/* ------------------------------------------------------------------ stored properties */

/* ------------------------------------------------------------------ lazy prototype (J5n2) */

/* The shape o would have without its lazy property: the same keys in the same order with the lazy
 * node's attribute cleared. Later properties (a static added to a function that was never used with
 * new) are replayed on top. The slots keep their values and positions. */
static embi_js_shape *shape_without_lazy(embi_js_runtime *rt, const embi_js_shape *shape)
{
    const embi_js_shape *nodes[EMBI_JS_SHAPE_MAX_CHAIN];
    uint32_t n = 0;
    for (const embi_js_shape *s = shape; s->count > 0; s = s->parent) {
        nodes[n++] = s;
        if (s->attrs & ATTR_LAZY) {
            break;
        }
    }
    EMBI_ASSERT(n > 0 && (nodes[n - 1]->attrs & ATTR_LAZY));
    embi_js_shape *cur = embi_js_shape_add(rt, (embi_js_shape *)nodes[n - 1]->parent, nodes[n - 1]->key,
                                           (uint8_t)(nodes[n - 1]->attrs & ~ATTR_LAZY));
    for (uint32_t i = n - 1u; i-- > 0;) {
        cur = embi_js_shape_add(rt, cur, nodes[i]->key, nodes[i]->attrs);
    }
    return cur;
}

/* Ends the lazy state of o: its `prototype` slot takes v, and o moves to the shape without the flag. */
static void lazy_proto_finish(embi_js_runtime *rt, embi_js_object *o, embi_js_value v)
{
    embi_js_shape *ns = shape_without_lazy(rt, o->shape);
    EMBI_JS_WB(o, v);
    EMBI_JS_WB(o, embi_js_from_cell(ns));
    *slot_at(o, EMBI_JS_LAZY_PROTO_SLOT) = v;
    o->shape = ns;
    o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_LAZY_PROTO;
}

void embi_js_obj_lazy_proto_realize(embi_js_runtime *rt, embi_js_object *o)
{
    if (!(o->hdr.flags & EMBI_JS_OBJ_LAZY_PROTO)) {
        return;
    }
    embi_js_object *p = embi_js_object_new_plain(rt);
    embi_js_define_fresh(rt, p, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR), embi_js_from_object(o),
                         ATTR_W | ATTR_C);
    lazy_proto_finish(rt, o, embi_js_from_object(p));
}


/* Looks up an own stored property. This is the ordinary storage, without exotic hooks. */
static bool store_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_prop *out)
{
    if ((o->hdr.flags & EMBI_JS_OBJ_LAZY_PROTO) && rt != nullptr &&
        k == embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE)) {
        embi_js_obj_lazy_proto_realize(rt, o);
    }
    if (embi_js_key_is_index(k)) {
        if (embi_js_obj_sparse(o)) {
            embi_js_dict_entry *e = dict_find(embi_js_obj_sparse(o), k);
            if (!e) {
                return false;
            }
            out->value = e->value;
            out->attrs = e->attrs;
            return true;
        }
        uint32_t i = embi_js_key_index(k);
        if (i < o->elements_len && o->elements[i] != EMBI_JS_EMPTY) {
            out->value = o->elements[i];
            out->attrs = EMBI_JS_ATTR_DEFAULT;
            return true;
        }
        return false;
    }
    if (embi_js_obj_dict(o)) {
        embi_js_dict_entry *e = dict_find(embi_js_obj_dict(o), k);
        if (!e) {
            return false;
        }
        out->value = e->value;
        out->attrs = e->attrs;
        return true;
    }
    embi_js_shape *n = embi_js_shape_find(o->shape, k);
    if (!n) {
        return false;
    }
    out->value = *slot_at(o, n->count - 1u);
    out->attrs = n->attrs;
    return true;
}

/* Adds a property that does not exist yet. Extensibility is the caller's check. */
static void store_add(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                      uint8_t attrs)
{
    EMBI_JS_WB(o, v);
    protect_key_store(rt, o, k);
    if (embi_js_key_is_index(k)) {
        uint32_t i = embi_js_key_index(k);
        if (!embi_js_obj_sparse(o) && attrs == EMBI_JS_ATTR_DEFAULT) {
            if (i < o->elements_len) {
                o->elements[i] = v; /* fills a hole */
                return;
            }
            if ((uint64_t)i <= 2ull * o->elements_len + EMBI_JS_DENSE_GAP) {
                if (i > o->elements_len) {
                    o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_PACKED; /* the gap is holes */
                }
                reserve_elements(rt, o, i + 1u);
                for (uint32_t j = o->elements_len; j < i; j++) {
                    EMBI_JS_STORE(o, o->elements[j], EMBI_JS_EMPTY);
                }
                EMBI_JS_STORE(o, o->elements[i], v);
                o->elements_len = i + 1u;
                return;
            }
        }
        to_sparse(rt, o);
        dict_append(rt, o, embi_js_obj_sparse(o), k, v, attrs);
        return;
    }
    if (embi_js_obj_dict(o)) {
        dict_append(rt, o, embi_js_obj_dict(o), k, v, attrs);
        dict_renew(rt, o);
        return;
    }
    if (o->shape->count >= EMBI_JS_SHAPE_MAX_CHAIN) {
        to_dict(rt, o);
        dict_append(rt, o, embi_js_obj_dict(o), k, v, attrs);
        return;
    }
    embi_js_shape *n = embi_js_shape_add(rt, o->shape, k, attrs);
    reserve_slots(rt, o, n->count);
    *slot_at(o, n->count - 1u) = v;
    EMBI_JS_WB(o, embi_js_from_cell(n));
    o->shape = n;
}

/* Replaces the value and attributes of an existing property. */
static void store_update(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                         uint8_t attrs)
{
    EMBI_JS_WB(o, v);
    protect_key_store(rt, o, k);
    if (embi_js_key_is_index(k)) {
        if (!embi_js_obj_sparse(o) && attrs == EMBI_JS_ATTR_DEFAULT) {
            EMBI_JS_STORE(o, o->elements[embi_js_key_index(k)], v);
            return;
        }
        to_sparse(rt, o);
        embi_js_dict_entry *e = dict_find(embi_js_obj_sparse(o), k);
        e->value = v;
        e->attrs = attrs;
        return;
    }
    if (embi_js_obj_dict(o)) {
        embi_js_dict_entry *e = dict_find(embi_js_obj_dict(o), k);
        bool attr_change = e->attrs != attrs;
        e->value = v;
        e->attrs = attrs;
        if (attr_change) {
            dict_renew(rt, o);
        }
        return;
    }
    embi_js_shape *n = embi_js_shape_find(o->shape, k);
    if (n->attrs == attrs) {
        *slot_at(o, n->count - 1u) = v;
        return;
    }
    to_dict(rt, o);
    embi_js_dict_entry *e = dict_find(embi_js_obj_dict(o), k);
    e->value = v;
    e->attrs = attrs;
}

/* Removes an own stored property. Missing properties are ignored. */
static void store_remove(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    if (embi_js_key_is_index(k)) {
        if (embi_js_obj_sparse(o)) {
            dict_remove(embi_js_obj_sparse(o), k);
            return;
        }
        uint32_t i = embi_js_key_index(k);
        if (i < o->elements_len) {
            EMBI_JS_STORE(o, o->elements[i], EMBI_JS_EMPTY);
            o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_PACKED; /* a hole below elements_len */
        }
        return;
    }
    protect_named_store(rt, o, k);
    if (embi_js_obj_dict(o)) {
        dict_remove(embi_js_obj_dict(o), k);
        dict_renew(rt, o);
        return;
    }
    embi_js_shape *n = embi_js_shape_find(o->shape, k);
    if (!n) {
        return;
    }
    embi_js_obj_lazy_proto_realize(rt, o); /* the removal replays or drops shape nodes */
    n = embi_js_shape_find(o->shape, k);
    if (n == o->shape) {
        /* The last property: step back to the parent shape and free its slot. */
        EMBI_JS_WB(o, EMBI_JS_EMPTY);
        *slot_at(o, n->count - 1u) = EMBI_JS_EMPTY;
        EMBI_JS_WB(o, embi_js_from_cell(n->parent));
        o->shape = n->parent;
        return;
    }
    to_dict(rt, o);
    dict_remove(embi_js_obj_dict(o), k);
}

/* ------------------------------------------------------------------ descriptors */

static void pd_from_prop(const embi_js_prop *p, embi_js_pdesc *out)
{
    if (p->attrs & ATTR_A) {
        const embi_js_accessor *a = (const embi_js_accessor *)embi_js_as_cell(p->value);
        out->value = UNDEF;
        out->get = a->getter;
        out->set = a->setter;
        out->has = EMBI_JS_PD_GET | EMBI_JS_PD_SET | EMBI_JS_PD_ENUMERABLE |
                   EMBI_JS_PD_CONFIGURABLE;
        out->attrs = p->attrs & (ATTR_E | ATTR_C);
    } else {
        out->value = p->value;
        out->get = UNDEF;
        out->set = UNDEF;
        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                   EMBI_JS_PD_CONFIGURABLE;
        out->attrs = p->attrs & (ATTR_W | ATTR_E | ATTR_C);
    }
}

/* Converts a complete descriptor into a stored property. Accessors get a new accessor cell. */
static void pd_to_prop(embi_js_runtime *rt, const embi_js_pdesc *d, embi_js_prop *out)
{
    if (embi_js_pd_is_accessor(d)) {
        embi_js_accessor *a = embi_js_accessor_new(rt, d->get, d->set);
        out->value = embi_js_from_cell(a);
        out->attrs = (d->attrs & (ATTR_E | ATTR_C)) | ATTR_A;
    } else {
        out->value = d->value;
        out->attrs = d->attrs & (ATTR_W | ATTR_E | ATTR_C);
    }
}

/* ValidateAndApplyPropertyDescriptor, the validation part. cur is complete. */
static bool pd_validate(const embi_js_pdesc *d, const embi_js_pdesc *cur)
{
    if (cur->attrs & ATTR_C) {
        return true;
    }
    if ((d->has & EMBI_JS_PD_CONFIGURABLE) && (d->attrs & ATTR_C)) {
        return false;
    }
    if ((d->has & EMBI_JS_PD_ENUMERABLE) && ((d->attrs & ATTR_E) != (cur->attrs & ATTR_E))) {
        return false;
    }
    if (!embi_js_pd_is_generic(d) && embi_js_pd_is_accessor(d) != embi_js_pd_is_accessor(cur)) {
        return false;
    }
    if (embi_js_pd_is_accessor(cur)) {
        if ((d->has & EMBI_JS_PD_GET) && !embi_js_same_value(d->get, cur->get)) {
            return false;
        }
        if ((d->has & EMBI_JS_PD_SET) && !embi_js_same_value(d->set, cur->set)) {
            return false;
        }
    } else if (!(cur->attrs & ATTR_W)) {
        if ((d->has & EMBI_JS_PD_WRITABLE) && (d->attrs & ATTR_W)) {
            return false;
        }
        if ((d->has & EMBI_JS_PD_VALUE) && !embi_js_same_value(d->value, cur->value)) {
            return false;
        }
    }
    return true;
}

/* ValidateAndApplyPropertyDescriptor, the apply part: merges d into cur and returns the complete
 * result. Changing between data and accessor keeps E and C and resets the other fields. */
static void pd_merge(const embi_js_pdesc *cur, const embi_js_pdesc *d, embi_js_pdesc *out)
{
    uint8_t attrs = cur->attrs & (ATTR_W | ATTR_E | ATTR_C);
    if (d->has & EMBI_JS_PD_ENUMERABLE) {
        attrs = (uint8_t)((attrs & ~ATTR_E) | (d->attrs & ATTR_E));
    }
    if (d->has & EMBI_JS_PD_CONFIGURABLE) {
        attrs = (uint8_t)((attrs & ~ATTR_C) | (d->attrs & ATTR_C));
    }
    bool cur_acc = embi_js_pd_is_accessor(cur);
    bool d_acc = embi_js_pd_is_accessor(d);
    bool d_data = embi_js_pd_is_data(d);

    if (d_acc || (!d_data && cur_acc)) {
        out->value = UNDEF;
        out->get = cur_acc ? cur->get : UNDEF;
        out->set = cur_acc ? cur->set : UNDEF;
        if (d->has & EMBI_JS_PD_GET) {
            out->get = d->get;
        }
        if (d->has & EMBI_JS_PD_SET) {
            out->set = d->set;
        }
        out->attrs = (uint8_t)(attrs & ~ATTR_W);
        out->has = EMBI_JS_PD_GET | EMBI_JS_PD_SET | EMBI_JS_PD_ENUMERABLE |
                   EMBI_JS_PD_CONFIGURABLE;
    } else {
        bool w = cur_acc ? false : (cur->attrs & ATTR_W) != 0;
        out->value = cur_acc ? UNDEF : cur->value;
        if (d->has & EMBI_JS_PD_VALUE) {
            out->value = d->value;
        }
        if (d->has & EMBI_JS_PD_WRITABLE) {
            w = (d->attrs & ATTR_W) != 0;
        }
        out->get = UNDEF;
        out->set = UNDEF;
        out->attrs = (uint8_t)((attrs & ~ATTR_W) | (w ? ATTR_W : 0));
        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                   EMBI_JS_PD_CONFIGURABLE;
    }
}

/* OrdinaryDefineOwnProperty: no hooks. */
static int ordinary_define(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                           const embi_js_pdesc *d)
{
    embi_js_prop p;
    embi_js_pdesc cur = {};
    embi_js_pdesc merged;
    if (!store_get(rt, o, k, &p)) {
        if (!embi_js_obj_is_extensible(o)) {
            return EMBI_JS_NO;
        }
        embi_js_pdesc def = {UNDEF, UNDEF, UNDEF, EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE |
                                                 EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE,
                             0};
        pd_merge(&def, d, &merged);
        embi_js_prop np;
        pd_to_prop(rt, &merged, &np);
        store_add(rt, o, k, np.value, np.attrs);
        return EMBI_JS_YES;
    }
    pd_from_prop(&p, &cur);
    if (!pd_validate(d, &cur)) {
        return EMBI_JS_NO;
    }
    pd_merge(&cur, d, &merged);
    embi_js_prop np;
    pd_to_prop(rt, &merged, &np);
    store_update(rt, o, k, np.value, np.attrs);
    return EMBI_JS_YES;
}

/* OrdinaryOwnPropertyKeys: indices ascending, then strings, then symbols, each in creation
 * order. Symbols are moved behind strings with a stable partition. */
static void partition_symbols_last(embi_js_key_vec *v, uint32_t start)
{
    uint32_t n = v->len - start;
    bool any_symbol = false;
    for (uint32_t i = 0; i < n; i++) {
        if (embi_js_is_symbol(v->data[start + i])) {
            any_symbol = true;
            break;
        }
    }
    if (!any_symbol) {
        return;
    }
    embi_js_key *tmp = embi_alloc((size_t)n * sizeof *tmp);
    memcpy(tmp, v->data + start, (size_t)n * sizeof *tmp);
    uint32_t j = start;
    for (uint32_t i = 0; i < n; i++) {
        if (!embi_js_is_symbol(tmp[i])) {
            v->data[j++] = tmp[i];
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (embi_js_is_symbol(tmp[i])) {
            v->data[j++] = tmp[i];
        }
    }
    embi_free(tmp, (size_t)n * sizeof *tmp);
}

static int cmp_u64_asc(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_u64_desc(const void *a, const void *b)
{
    return -cmp_u64_asc(a, b);
}

static int ordinary_own_keys(embi_js_object *o, embi_js_key_vec *out)
{
    if (embi_js_obj_sparse(o)) {
        uint32_t base = out->len;
        for (uint32_t i = 0; i < embi_js_obj_sparse(o)->len; i++) {
            if (embi_js_obj_sparse(o)->entries[i].key != EMBI_JS_EMPTY) {
                embi_vec_push(out, embi_js_obj_sparse(o)->entries[i].key);
            }
        }
        SDL_qsort(out->data + base, out->len - base, sizeof(embi_js_key), cmp_u64_asc);
    } else {
        embi_vec_reserve(out, out->len + o->elements_len);
        for (uint32_t i = 0; i < o->elements_len; i++) {
            if (o->elements[i] != EMBI_JS_EMPTY) {
                out->data[out->len++] = embi_js_key_from_index(i);
            }
        }
    }

    uint32_t start = out->len;
    if (embi_js_obj_dict(o)) {
        embi_js_dict *d = embi_js_obj_dict(o);
        embi_vec_reserve(out, out->len + d->live);
        for (uint32_t i = 0; i < d->len; i++) {
            if (d->entries[i].key != EMBI_JS_EMPTY) {
                out->data[out->len++] = d->entries[i].key;
            }
        }
    } else {
        embi_js_shape_keys_append(o->shape, out);
    }
    partition_symbols_last(out, start);
    /* Private names are not property keys (J2d): leave them out. */
    uint32_t w = start;
    for (uint32_t i = start; i < out->len; i++) {
        embi_js_key key = out->data[i];
        if (!(embi_js_is_symbol(key) && (embi_js_as_cell(key)->flags & EMBI_JS_FLAG_SYM_PRIVATE))) {
            out->data[w++] = key;
        }
    }
    out->len = w;
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ internal methods */

static int ordinary_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    embi_js_prop p;
    if (!store_get(rt, o, k, &p)) {
        return EMBI_JS_NO;
    }
    pd_from_prop(&p, out);
    return EMBI_JS_YES;
}

int embi_js_obj_get_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_pdesc *out)
{
    if (o->klass->get_own) {
        return o->klass->get_own(rt, o, k, out);
    }
    return ordinary_get_own(rt, o, k, out);
}

int embi_js_obj_define_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                    const embi_js_pdesc *d)
{
    if (o->klass->define_own) {
        return o->klass->define_own(rt, o, k, d);
    }
    return ordinary_define(rt, o, k, d);
}

int embi_js_obj_has_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_object *cur = o;
    for (;;) {
        if (cur->klass->has) {
            return cur->klass->has(rt, cur, k);
        }
        int r;
        if (cur->klass->get_own) {
            embi_js_pdesc d;
            r = cur->klass->get_own(rt, cur, k, &d);
        } else {
            embi_js_prop sp;
            r = store_get(rt, cur, k, &sp) ? EMBI_JS_YES : EMBI_JS_NO;
        }
        if (r != EMBI_JS_NO) {
            return r;
        }
        embi_js_value p = embi_js_obj_proto(cur);
        if (!embi_js_is_object(p)) {
            return EMBI_JS_NO;
        }
        cur = embi_js_as_object(p);
    }
}

int embi_js_obj_has_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_pdesc d;
    return embi_js_obj_get_own_property(rt, o, k, &d);
}

/* Calls the getter of an accessor pair. An undefined getter reads as undefined. */
static embi_js_value call_getter(embi_js_runtime *rt, embi_js_value getter, embi_js_value receiver,
                                 embi_js_key k)
{
    if (getter == UNDEF) {
        return UNDEF;
    }
    embi_js_value restricted;
    if (embi_js_restricted_get(rt, getter, receiver, k, &restricted)) {
        return restricted;
    }
    return embi_js_call(rt, getter, receiver, 0, nullptr);
}

embi_js_value embi_js_obj_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                              embi_js_value receiver)
{
    embi_js_object *cur = o;
    for (;;) {
        if (cur->klass->get) {
            return cur->klass->get(rt, cur, k, receiver);
        }
        if (cur->klass->get_own) {
            embi_js_pdesc d;
            int r = cur->klass->get_own(rt, cur, k, &d);
            if (r == EMBI_JS_THREW) {
                return EMBI_JS_EXCEPTION;
            }
            if (r == EMBI_JS_YES) {
                return embi_js_pd_is_accessor(&d) ? call_getter(rt, d.get, receiver, k) : d.value;
            }
        } else {
            /* Ordinary fast path: read the stored property, without building a descriptor. */
            embi_js_prop sp;
            if (store_get(rt, cur, k, &sp)) {
                if (!(sp.attrs & ATTR_A)) {
                    return sp.value;
                }
                const embi_js_accessor *a = (const embi_js_accessor *)embi_js_as_cell(sp.value);
                return call_getter(rt, a->getter, receiver, k);
            }
        }
        embi_js_value p = embi_js_obj_proto(cur);
        if (!embi_js_is_object(p)) {
            return UNDEF;
        }
        cur = embi_js_as_object(p);
    }
}

int embi_js_obj_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                    embi_js_value receiver)
{
    embi_js_object *cur = o;
    embi_js_pdesc own;
    for (;;) {
        if (cur->klass->set) {
            return cur->klass->set(rt, cur, k, v, receiver);
        }
        if (cur->klass->get_own) {
            int r = cur->klass->get_own(rt, cur, k, &own);
            if (r == EMBI_JS_THREW) {
                return EMBI_JS_THREW;
            }
            if (r == EMBI_JS_YES) {
                break;
            }
        } else {
            embi_js_prop sp;
            if (store_get(rt, cur, k, &sp)) {
                pd_from_prop(&sp, &own);
                break;
            }
        }
        embi_js_value p = embi_js_obj_proto(cur);
        if (!embi_js_is_object(p)) {
            own = (embi_js_pdesc){UNDEF, UNDEF, UNDEF,
                                  EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                                      EMBI_JS_PD_CONFIGURABLE,
                                  ATTR_W | ATTR_E | ATTR_C};
            break;
        }
        cur = embi_js_as_object(p);
    }

    if (!embi_js_pd_is_accessor(&own)) {
        if (!(own.attrs & ATTR_W)) {
            return EMBI_JS_NO;
        }
        if (!embi_js_is_object(receiver)) {
            return EMBI_JS_NO;
        }
        embi_js_object *rcv = embi_js_as_object(receiver);
        if (!rcv->klass->get_own && !rcv->klass->define_own) {
            /* Ordinary receiver: update or add the stored property directly. This is the
             * define-own-property result for these receivers, without the descriptor. */
            embi_js_prop sp;
            if (store_get(rt, rcv, k, &sp)) {
                if ((sp.attrs & ATTR_A) || !(sp.attrs & ATTR_W)) {
                    return EMBI_JS_NO;
                }
                store_update(rt, rcv, k, v, sp.attrs);
                return EMBI_JS_YES;
            }
            if (!embi_js_obj_is_extensible(rcv)) {
                return EMBI_JS_NO;
            }
            store_add(rt, rcv, k, v, EMBI_JS_ATTR_DEFAULT);
            return EMBI_JS_YES;
        }
        if (rcv->klass == &embi_js_class_array && embi_js_key_is_index(k)) {
            /* Array element fast path: same result as the generic define below. */
            embi_js_prop sp;
            if (store_get(rt, rcv, k, &sp)) {
                if ((sp.attrs & ATTR_A) || !(sp.attrs & ATTR_W)) {
                    return EMBI_JS_NO;
                }
                store_update(rt, rcv, k, v, sp.attrs);
                return EMBI_JS_YES;
            }
            embi_js_value lk = embi_js_name_val(rt, EMBI_JS_NAME_LENGTH);
            embi_js_prop lp;
            if (store_get(rt, rcv, lk, &lp) && (lp.attrs & ATTR_W) && !(lp.attrs & ATTR_A) &&
                embi_js_obj_is_extensible(rcv)) {
                uint32_t idx = embi_js_key_index(k);
                uint32_t old_len = (uint32_t)embi_js_as_double(lp.value);
                store_add(rt, rcv, k, v, EMBI_JS_ATTR_DEFAULT);
                if (idx >= old_len) {
                    store_update(rt, rcv, lk, embi_js_number((double)idx + 1.0), lp.attrs);
                }
                return EMBI_JS_YES;
            }
        }
        embi_js_pdesc ex;
        int r = embi_js_obj_get_own_property(rt, rcv, k, &ex);
        if (r == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (r == EMBI_JS_YES) {
            if (embi_js_pd_is_accessor(&ex) || !(ex.attrs & ATTR_W)) {
                return EMBI_JS_NO;
            }
            embi_js_pdesc vd = {v, UNDEF, UNDEF, EMBI_JS_PD_VALUE, 0};
            return embi_js_obj_define_own_property(rt, rcv, k, &vd);
        }
        embi_js_pdesc nd = {v, UNDEF, UNDEF,
                            EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                                EMBI_JS_PD_CONFIGURABLE,
                            ATTR_W | ATTR_E | ATTR_C};
        return embi_js_obj_define_own_property(rt, rcv, k, &nd);
    }

    if (own.set == UNDEF) {
        return EMBI_JS_NO;
    }
    embi_js_value res = embi_js_call(rt, own.set, receiver, 1, &v);
    if (embi_js_is_exception(res)) {
        return EMBI_JS_THREW;
    }
    return EMBI_JS_YES;
}

int embi_js_obj_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    if (o->klass->delete) {
        return o->klass->delete(rt, o, k);
    }
    embi_js_pdesc d;
    int r = embi_js_obj_get_own_property(rt, o, k, &d);
    if (r == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (r == EMBI_JS_NO) {
        return EMBI_JS_YES;
    }
    if (!(d.attrs & ATTR_C)) {
        return EMBI_JS_NO;
    }
    store_remove(rt, o, k);
    return EMBI_JS_YES;
}

int embi_js_obj_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    if (o->klass->own_keys) {
        return o->klass->own_keys(rt, o, out);
    }
    return ordinary_own_keys(o, out);
}

/* Object.prototype is an immutable prototype exotic object. */
static bool is_immutable_proto(embi_js_runtime *rt, const embi_js_object *o)
{
    return rt->protos[EMBI_JS_PROTO_OBJECT] == embi_js_from_object(o);
}

/* The root shape of objects whose prototype is proto (an object or EMBI_JS_NULL). Cached per
 * prototype object, and once for null. */
static embi_js_shape *root_shape(embi_js_runtime *rt, embi_js_value proto)
{
    if (embi_js_is_null(proto)) {
        embi_js_value cached = rt->protos[EMBI_JS_PROTO_NULL_ROOT_SHAPE];
        if (embi_js_is_empty(cached)) {
            embi_js_shape *s = embi_js_shape_new_root(rt, EMBI_JS_NULL);
            rt->protos[EMBI_JS_PROTO_NULL_ROOT_SHAPE] = embi_js_from_cell(s);
            return s;
        }
        return (embi_js_shape *)embi_js_as_cell(cached);
    }
    embi_js_object *p = embi_js_as_object(proto);
    p->hdr.flags |= EMBI_JS_OBJ_IS_PROTO;
    if (p->elements_len || embi_js_obj_sparse(p)) {
        rt->protect &= ~EMBI_JS_PROTECT_NO_PROTO_ELEMENTS;
    }
    if (!obj_child_root(p)) {
        embi_js_shape *s = embi_js_shape_new_root(rt, proto);
        EMBI_JS_WB(p, embi_js_from_cell(s));
        rare_get(rt, p)->child_root = s;
    }
    return obj_child_root(p);
}

/* Gives a shape-mode object a new prototype by replaying its keys from the new root. The slots
 * keep their values: the chain has the same keys in the same order, so slot i still belongs to
 * the node with count i + 1. The old chain stays alive until o->shape is replaced. */
static void rebuild_shape_chain(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto)
{
    embi_js_key keys[EMBI_JS_SHAPE_MAX_CHAIN];
    uint8_t attrs[EMBI_JS_SHAPE_MAX_CHAIN];
    uint32_t n = o->shape->count;
    const embi_js_shape *node = o->shape;
    for (uint32_t i = n; i-- > 0; node = node->parent) {
        keys[i] = node->key;
        attrs[i] = node->attrs;
    }
    embi_js_shape *s = root_shape(rt, proto);
    for (uint32_t i = 0; i < n; i++) {
        s = embi_js_shape_add(rt, s, keys[i], attrs[i]);
    }
    EMBI_JS_WB(o, embi_js_from_cell(s));
    o->shape = s;
}

int embi_js_obj_set_prototype_of(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto)
{
    if (o->klass->set_proto) {
        return o->klass->set_proto(rt, o, proto);
    }
    embi_js_value cur = embi_js_obj_proto(o);
    if (proto == cur) {
        return EMBI_JS_YES;
    }
    if (is_immutable_proto(rt, o)) {
        return EMBI_JS_NO;
    }
    if (!embi_js_obj_is_extensible(o)) {
        return EMBI_JS_NO;
    }
    for (embi_js_value p = proto; embi_js_is_object(p);) {
        embi_js_object *po = embi_js_as_object(p);
        if (po == o) {
            return EMBI_JS_NO;
        }
        if (embi_js_obj_is_proxy(po)) {
            break; /* a proxy ends the walk: its [[GetPrototypeOf]] is a trap */
        }
        p = embi_js_obj_proto(po);
    }
    if (embi_js_is_object(proto)) {
        embi_js_object *po = embi_js_as_object(proto);
        po->hdr.flags |= EMBI_JS_OBJ_IS_PROTO;
        if (po->elements_len || embi_js_obj_sparse(po)) {
            rt->protect &= ~EMBI_JS_PROTECT_NO_PROTO_ELEMENTS;
        }
    }
    if (o->hdr.flags & EMBI_JS_OBJ_WATCHED) {
        /* The intrinsic Array.prototype chain changes. */
        rt->protect &= ~EMBI_JS_PROTECT_NO_PROTO_ELEMENTS;
    }
    if (embi_js_obj_dict(o)) {
        /* A dictionary shape belongs to this object alone, so its prototype changes in place. */
        EMBI_JS_WB(o->shape, proto);
        o->shape->proto = proto;
    } else {
        rebuild_shape_chain(rt, o, proto);
    }
    return EMBI_JS_YES;
}

int embi_js_obj_prevent_extensions(embi_js_runtime *rt, embi_js_object *o)
{
    if (o->klass->prevent_extensions) {
        return o->klass->prevent_extensions(rt, o);
    }
    o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_EXTENSIBLE;
    return EMBI_JS_YES;
}

int embi_js_obj_get_prototype_of(embi_js_runtime *rt, embi_js_object *o, embi_js_value *out)
{
    if (o->klass->get_proto) {
        return o->klass->get_proto(rt, o, out);
    }
    *out = embi_js_obj_proto(o);
    return EMBI_JS_YES;
}

int embi_js_obj_extensible(embi_js_runtime *rt, embi_js_object *o)
{
    if (o->klass->is_extensible) {
        return o->klass->is_extensible(rt, o);
    }
    return embi_js_obj_is_extensible(o) ? EMBI_JS_YES : EMBI_JS_NO;
}

bool embi_js_pd_compatible(const embi_js_pdesc *d, const embi_js_pdesc *cur)
{
    return pd_validate(d, cur);
}

/* ------------------------------------------------------------------ raw helpers */

void embi_js_define_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                        uint8_t attrs)
{
    embi_js_prop p;
    uint8_t a = attrs & (ATTR_W | ATTR_E | ATTR_C);
    if ((o->hdr.flags & EMBI_JS_OBJ_LAZY_PROTO) && k == embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE)) {
        lazy_proto_finish(rt, o, EMBI_JS_UNDEFINED); /* the value is replaced below (a class setup): no object */
    }
    if (store_get(rt, o, k, &p)) {
        store_update(rt, o, k, v, a);
    } else {
        store_add(rt, o, k, v, a);
    }
}

void embi_js_define_accessor_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_value getter, embi_js_value setter, uint8_t attrs)
{
    embi_js_accessor *acc = embi_js_accessor_new(rt, getter, setter);
    embi_js_value v = embi_js_from_cell(acc);
    uint8_t a = (uint8_t)((attrs & (ATTR_E | ATTR_C)) | ATTR_A);
    embi_js_prop p;
    if (store_get(rt, o, k, &p)) {
        store_update(rt, o, k, v, a);
    } else {
        store_add(rt, o, k, v, a);
    }
}

void embi_js_remove_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    store_remove(rt, o, k);
}

void embi_js_define_fresh(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                          uint8_t attrs)
{
    EMBI_ASSERT(!embi_js_key_is_index(k));
    EMBI_ASSERT(!embi_js_obj_dict(o) && !embi_js_obj_sparse(o));
    EMBI_ASSERT(embi_js_shape_find(o->shape, k) == nullptr);
    store_add(rt, o, k, v, (uint8_t)(attrs & (ATTR_W | ATTR_E | ATTR_C | ATTR_LAZY)));
}

void embi_js_obj_append_shape(embi_js_runtime *rt, embi_js_object *o, embi_js_shape *to,
                              const embi_js_value *vals, uint32_t n)
{
    EMBI_ASSERT(!embi_js_obj_dict(o) && !embi_js_obj_sparse(o) && !(o->hdr.flags & EMBI_JS_OBJ_WATCHED));
    EMBI_ASSERT(to->count == o->shape->count + n);
    uint32_t base = o->shape->count;
    reserve_slots(rt, o, to->count);
    for (uint32_t i = 0; i < n; i++) {
        EMBI_JS_WB(o, vals[i]);
        *slot_at(o, base + i) = vals[i];
    }
    EMBI_JS_WB(o, embi_js_from_cell(to));
    o->shape = to;
}

bool embi_js_obj_shape_only(const embi_js_object *o)
{
    return (o->klass == &embi_js_class_object || o->klass == &embi_js_class_array) &&
           o->elements_len == 0 && !embi_js_obj_dict(o) && !embi_js_obj_sparse(o);
}

void embi_js_obj_enum_string_keys(const embi_js_object *o, embi_js_key_vec *out)
{
    EMBI_ASSERT(embi_js_obj_shape_only(o));
    uint32_t cnt = 0;
    for (const embi_js_shape *n = o->shape; n->count > 0; n = n->parent) {
        if ((n->attrs & EMBI_JS_ATTR_ENUMERABLE) && !embi_js_is_symbol(n->key)) {
            cnt++;
        }
    }
    uint32_t base = out->len;
    embi_vec_reserve(out, base + cnt);
    /* The chain runs newest first, so the keys are written from the back. */
    uint32_t pos = base + cnt;
    for (const embi_js_shape *n = o->shape; n->count > 0; n = n->parent) {
        if ((n->attrs & EMBI_JS_ATTR_ENUMERABLE) && !embi_js_is_symbol(n->key)) {
            out->data[--pos] = n->key;
        }
    }
    out->len = base + cnt;
}

embi_js_key embi_js_key_c(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_from_str(rt, embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* ------------------------------------------------------------------ array class */

static int array_truncate(embi_js_runtime *rt, embi_js_object *o, uint32_t new_len,
                          uint32_t *final_len)
{
    (void)rt;
    *final_len = new_len;
    if (embi_js_obj_sparse(o)) {
        embi_js_key_vec doomed = {nullptr, 0, 0};
        for (uint32_t i = 0; i < embi_js_obj_sparse(o)->len; i++) {
            embi_js_key key = embi_js_obj_sparse(o)->entries[i].key;
            if (key != EMBI_JS_EMPTY && embi_js_key_index(key) >= new_len) {
                embi_vec_push(&doomed, key);
            }
        }
        SDL_qsort(doomed.data, doomed.len, sizeof(embi_js_key), cmp_u64_desc);
        int result = EMBI_JS_YES;
        for (uint32_t i = 0; i < doomed.len; i++) {
            embi_js_dict_entry *e = dict_find(embi_js_obj_sparse(o), doomed.data[i]);
            if (!(e->attrs & ATTR_C)) {
                *final_len = embi_js_key_index(doomed.data[i]) + 1u;
                result = EMBI_JS_NO;
                break;
            }
            dict_remove(embi_js_obj_sparse(o), doomed.data[i]);
        }
        embi_vec_free(&doomed);
        return result;
    }
    if (o->elements_len > new_len) {
        o->elements_len = new_len;
    }
    return EMBI_JS_YES;
}

static int array_set_length_core(embi_js_runtime *rt, embi_js_object *o, const embi_js_pdesc *d);

/* A non-writable length clears PACKED, so an append (which the fast paths allow on the flag alone)
 * stays on the generic path (J5e). */
static int array_set_length(embi_js_runtime *rt, embi_js_object *o, const embi_js_pdesc *d)
{
    embi_js_value lk = embi_js_name_val(rt, EMBI_JS_NAME_LENGTH);
    embi_js_prop lp;
    double before = 0;
    if (store_get(rt, o, lk, &lp)) {
        before = embi_js_as_double(lp.value);
    }
    int r = array_set_length_core(rt, o, d);
    if (!store_get(rt, o, lk, &lp)) {
        return r;
    }
    double after = embi_js_as_double(lp.value);
    /* A length increase past elements_len makes holes (ARRAY_HOLE and length stores). */
    if ((after > before && after > (double)o->elements_len) || !(lp.attrs & ATTR_W)) {
        o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_PACKED;
    }
    return r;
}

static int array_set_length_core(embi_js_runtime *rt, embi_js_object *o, const embi_js_pdesc *d)
{
    embi_js_value lk = embi_js_name_val(rt, EMBI_JS_NAME_LENGTH);
    if (!(d->has & EMBI_JS_PD_VALUE)) {
        return ordinary_define(rt, o, lk, d);
    }
    uint32_t new_len = 0;
    double num_len = 0;
    if (!embi_js_to_uint32(rt, d->value, &new_len)) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_to_number(rt, d->value, &num_len)) {
        return EMBI_JS_THREW;
    }
    if ((double)new_len != num_len) {
        (void)embi_js_throw_range_error(rt, "Invalid array length");
        return EMBI_JS_THREW;
    }

    embi_js_pdesc nd = *d;
    nd.value = embi_js_number((double)new_len);
    embi_js_prop lp;
    store_get(rt, o, lk, &lp);
    uint32_t old_len = (uint32_t)embi_js_as_double(lp.value);
    if (new_len >= old_len) {
        return ordinary_define(rt, o, lk, &nd);
    }
    if (!(lp.attrs & ATTR_W)) {
        return EMBI_JS_NO;
    }
    bool new_writable = !(d->has & EMBI_JS_PD_WRITABLE) || (d->attrs & ATTR_W);
    if (!new_writable) {
        nd.has |= EMBI_JS_PD_WRITABLE;
        nd.attrs |= ATTR_W; /* keep writable until the elements are gone */
    }
    if (ordinary_define(rt, o, lk, &nd) != EMBI_JS_YES) {
        return EMBI_JS_NO;
    }

    uint32_t final_len = new_len;
    if (array_truncate(rt, o, new_len, &final_len) == EMBI_JS_NO) {
        embi_js_pdesc fail = {embi_js_number((double)final_len), UNDEF, UNDEF,
                              EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE, new_writable ? ATTR_W : 0};
        ordinary_define(rt, o, lk, &fail);
        return EMBI_JS_NO;
    }
    if (!new_writable) {
        embi_js_pdesc ro = {UNDEF, UNDEF, UNDEF, EMBI_JS_PD_WRITABLE, 0};
        ordinary_define(rt, o, lk, &ro);
    }
    return EMBI_JS_YES;
}

static int array_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                            const embi_js_pdesc *d)
{
    embi_js_value lk = embi_js_name_val(rt, EMBI_JS_NAME_LENGTH);
    if (k == lk) {
        return array_set_length(rt, o, d);
    }
    if (!embi_js_key_is_index(k)) {
        return ordinary_define(rt, o, k, d);
    }
    embi_js_prop lp;
    store_get(rt, o, lk, &lp);
    uint32_t old_len = (uint32_t)embi_js_as_double(lp.value);
    uint32_t idx = embi_js_key_index(k);
    if (idx >= old_len && !(lp.attrs & ATTR_W)) {
        return EMBI_JS_NO;
    }
    int r = ordinary_define(rt, o, k, d);
    if (r != EMBI_JS_YES) {
        return r;
    }
    if (idx >= old_len) {
        store_update(rt, o, lk, embi_js_number((double)idx + 1.0), lp.attrs);
    }
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ dense fast paths (J5e) */

/* The length slot of an array: slot 0 in shape mode (its first named property), the dictionary
 * entry in dictionary mode. */
static embi_js_value *array_length_ref(embi_js_runtime *rt, embi_js_object *o)
{
    if (embi_js_obj_dict(o)) {
        return &dict_find(embi_js_obj_dict(o), embi_js_name_val(rt, EMBI_JS_NAME_LENGTH))->value;
    }
    return slot_at(o, 0);
}

static uint32_t array_length_u32(embi_js_runtime *rt, embi_js_object *o)
{
    embi_js_value v = *array_length_ref(rt, o);
    return embi_js_is_int32(v) ? (uint32_t)embi_js_as_int32(v) : (uint32_t)embi_js_as_double(v);
}

bool embi_js_array_length_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t *out)
{
    if (o->klass != &embi_js_class_array) {
        return false;
    }
    *out = array_length_u32(rt, o);
    return true;
}

bool embi_js_obj_index_get_fast(embi_js_runtime *rt, const embi_js_object *o, uint32_t i,
                                embi_js_value *out)
{
    if (!embi_js_obj_dense_plain(o)) {
        return false;
    }
    if (i < o->elements_len && !embi_js_is_empty(o->elements[i])) {
        *out = o->elements[i];
        return true;
    }
    /* A hole: absent on the array and, under NO_PROTO_ELEMENTS, on the intrinsic chain. */
    if (o->klass == &embi_js_class_array && (rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS) &&
        o->shape->proto == rt->protos[EMBI_JS_PROTO_ARRAY]) {
        *out = EMBI_JS_UNDEFINED;
        return true;
    }
    return false;
}

/* Appends v at index elements_len, which is also the length, of a packed extensible array with a
 * writable length (PACKED clears on a non-writable length). CreateDataProperty then length = n + 1. */
static bool dense_append(embi_js_runtime *rt, embi_js_object *o, embi_js_value v)
{
    uint32_t n = o->elements_len;
    if (!embi_js_obj_is_packed(o) || (o->hdr.flags & EMBI_JS_OBJ_IS_PROTO) ||
        !embi_js_obj_is_extensible(o) || n >= 4294967295u) {
        return false;
    }
    if (array_length_u32(rt, o) != n) {
        return false;
    }
    reserve_elements(rt, o, n + 1u);
    EMBI_JS_STORE(o, o->elements[n], v);
    EMBI_JS_WB(o, v);
    o->elements_len = n + 1u;
    *array_length_ref(rt, o) = embi_js_number((double)n + 1.0);
    return true;
}

/* Re-establishes PACKED when a dense Array becomes complete: elements_len == length, a writable
 * length and no hole below elements_len. Runs a one-time O(n) scan, only on the append that fills
 * the array up to its length (array literals, new Array(n) filled in order). An elision leaves a
 * hole, so the scan finds it and the flag stays clear. */
static void mark_packed_if_complete(embi_js_runtime *rt, embi_js_object *o)
{
    uint32_t n = o->elements_len;
    if (embi_js_obj_is_packed(o) || embi_js_obj_sparse(o) || o->klass != &embi_js_class_array ||
        array_length_u32(rt, o) != n) {
        return;
    }
    embi_js_prop lp;
    if (!store_get(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), &lp) || !(lp.attrs & ATTR_W)) {
        return;
    }
    for (uint32_t j = 0; j < n; j++) {
        if (embi_js_is_empty(o->elements[j])) {
            return;
        }
    }
    o->hdr.flags |= EMBI_JS_OBJ_PACKED;
}

/* Appends v at elements_len of an extensible Array whose length is above it (an index below the
 * length: the length does not change). PACKED is already clear there, since packed means
 * elements_len == length. A hole below elements_len stays a hole. */
static bool append_below_length(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v)
{
    if (!embi_js_obj_is_extensible(o) || (o->hdr.flags & EMBI_JS_OBJ_IS_PROTO) || i >= 4294967295u) {
        return false;
    }
    if (array_length_u32(rt, o) <= i) {
        return false;
    }
    reserve_elements(rt, o, i + 1u);
    EMBI_JS_STORE(o, o->elements[i], v);
    EMBI_JS_WB(o, v);
    o->elements_len = i + 1u;
    mark_packed_if_complete(rt, o);
    return true;
}

bool embi_js_obj_index_set_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v)
{
    if (!embi_js_obj_dense_plain(o) || (o->hdr.flags & EMBI_JS_OBJ_IS_PROTO)) {
        return false;
    }
    if (i < o->elements_len && !embi_js_is_empty(o->elements[i])) {
        EMBI_JS_WB(o, v);
        EMBI_JS_STORE(o, o->elements[i], v);
        return true;
    }
    /* A hole or an append: [[Set]] walks the intrinsic chain, which holds no index property under
     * NO_PROTO_ELEMENTS, so the store is an ordinary CreateDataProperty. */
    if (o->klass != &embi_js_class_array || !(rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS) ||
        o->shape->proto != rt->protos[EMBI_JS_PROTO_ARRAY] || !embi_js_obj_is_extensible(o)) {
        return false;
    }
    if (i < o->elements_len) {
        EMBI_JS_WB(o, v);
        EMBI_JS_STORE(o, o->elements[i], v);
        return true;
    }
    if (i != o->elements_len) {
        return false;
    }
    return append_below_length(rt, o, i, v) || dense_append(rt, o, v);
}

bool embi_js_array_push_fast(embi_js_runtime *rt, embi_js_object *a, embi_js_value v)
{
    /* push is a Set: the intrinsic chain must hold no index property (NO_PROTO_ELEMENTS). */
    if (a->klass != &embi_js_class_array || embi_js_obj_sparse(a) ||
        !(rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS) ||
        a->shape->proto != rt->protos[EMBI_JS_PROTO_ARRAY]) {
        return false;
    }
    return dense_append(rt, a, v);
}

int embi_js_obj_index_probe(embi_js_runtime *rt, const embi_js_object *o, uint32_t i,
                            embi_js_value *out)
{
    if (!embi_js_obj_dense_plain(o)) {
        return -1;
    }
    if (i < o->elements_len && !embi_js_is_empty(o->elements[i])) {
        *out = o->elements[i];
        return 1;
    }
    /* A hole of an Array under NO_PROTO_ELEMENTS with the intrinsic prototype is absent for certain. */
    if (o->klass == &embi_js_class_array && (rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS) &&
        o->shape->proto == rt->protos[EMBI_JS_PROTO_ARRAY]) {
        return 0;
    }
    return -1;
}

bool embi_js_obj_create_index_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v)
{
    if (!embi_js_obj_dense_plain(o)) {
        return false;
    }
    if (i < o->elements_len) {
        if (embi_js_is_empty(o->elements[i])) {
            /* A new property below elements_len: it needs an extensible object, and an index on a
             * prototype breaks NO_PROTO_ELEMENTS. */
            if (!embi_js_obj_is_extensible(o)) {
                return false;
            }
            protect_index_store(rt, o);
        }
        EMBI_JS_WB(o, v);
        o->elements[i] = v; /* default attributes, as every dense element has */
        return true;
    }
    if (i != o->elements_len) {
        return false;
    }
    if (o->klass == &embi_js_class_array) {
        return append_below_length(rt, o, i, v) || dense_append(rt, o, v);
    }
    if (o->klass != &embi_js_class_object || !embi_js_obj_is_extensible(o) ||
        i >= 4294967295u) {
        return false;
    }
    protect_index_store(rt, o);
    reserve_elements(rt, o, i + 1u);
    EMBI_JS_STORE(o, o->elements[i], v);
    EMBI_JS_WB(o, v);
    o->elements_len = i + 1u;
    return true;
}

void embi_js_array_reserve(embi_js_runtime *rt, embi_js_object *a, uint32_t n)
{
    if (a->klass == &embi_js_class_array && !embi_js_obj_sparse(a)) {
        reserve_elements(rt, a, n);
    }
}

bool embi_js_array_pop_fast(embi_js_runtime *rt, embi_js_object *a, embi_js_value *out)
{
    if (a->klass != &embi_js_class_array || embi_js_obj_sparse(a) || !embi_js_obj_is_packed(a) ||
        (a->hdr.flags & EMBI_JS_OBJ_IS_PROTO)) {
        return false;
    }
    uint32_t n = a->elements_len;
    if (n == 0 || array_length_u32(rt, a) != n) {
        return false;
    }
    *out = a->elements[n - 1];
    EMBI_JS_STORE(a, a->elements[n - 1], EMBI_JS_EMPTY);
    a->elements_len = n - 1u;
    *array_length_ref(rt, a) = embi_js_number((double)n - 1.0);
    return true;
}

/* ------------------------------------------------------------------ string wrapper class */

embi_js_value embi_js_wrapper_value(const embi_js_object *o)
{
    const embi_js_class *k = o->klass;
    if (k != &embi_js_class_string && k != &embi_js_class_number &&
        k != &embi_js_class_boolean && k != &embi_js_class_symbol) {
        return EMBI_JS_UNDEFINED;
    }
    return (embi_js_value)(uintptr_t)o->internal;
}

static void wrapper_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_js_mark_value(t, embi_js_wrapper_value(o));
}

static int string_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                          embi_js_pdesc *out)
{
    if (embi_js_key_is_index(k)) {
        embi_js_string *s = embi_js_str_flat(rt, embi_js_wrapper_value(o));
        uint32_t i = embi_js_key_index(k);
        if (i < embi_js_str_len(s)) {
            embi_js_string *c = embi_js_str_substring(rt, s, i, i + 1u);
            out->value = embi_js_from_string(c);
            out->get = UNDEF;
            out->set = UNDEF;
            out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                       EMBI_JS_PD_CONFIGURABLE;
            out->attrs = ATTR_E;
            return EMBI_JS_YES;
        }
    }
    return ordinary_get_own(rt, o, k, out);
}

static int string_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                             const embi_js_pdesc *d)
{
    if (embi_js_key_is_index(k)) {
        embi_js_string *s = embi_js_str_flat(rt, embi_js_wrapper_value(o));
        if (embi_js_key_index(k) < embi_js_str_len(s)) {
            embi_js_pdesc cur = {};
            string_get_own(rt, o, k, &cur);
            return pd_validate(d, &cur) ? EMBI_JS_YES : EMBI_JS_NO;
        }
    }
    return ordinary_define(rt, o, k, d);
}

static int string_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    (void)rt;
    embi_js_string *s = embi_js_str_flat(rt, embi_js_wrapper_value(o));
    uint32_t n = embi_js_str_len(s);
    embi_vec_reserve(out, out->len + n);
    for (uint32_t i = 0; i < n; i++) {
        out->data[out->len++] = embi_js_key_from_index(i);
    }
    return ordinary_own_keys(o, out);
}

/* ------------------------------------------------------------------ bound functions */

typedef struct bound_data {
    embi_js_value target;
    embi_js_value this_v;
    uint32_t nargs;
    embi_js_value *args; /* nargs values, nullptr when nargs is 0 */
} bound_data;

static bound_data *bound_of(const embi_js_object *f)
{
    return (bound_data *)f->internal;
}

static embi_js_value bound_invoke(embi_js_runtime *rt, const bound_data *b, int argc,
                                  const embi_js_value *argv, bool construct,
                                  embi_js_value new_target)
{
    uint32_t total = b->nargs + (uint32_t)argc;
    embi_js_value small[8];
    embi_js_value *buf = small;
    if (total > 8u) {
        buf = embi_alloc((size_t)total * sizeof *buf);
    }
    for (uint32_t i = 0; i < b->nargs; i++) {
        buf[i] = b->args[i];
    }
    for (int i = 0; i < argc; i++) {
        buf[b->nargs + (uint32_t)i] = argv[i];
    }
    embi_js_value r = construct
                          ? embi_js_construct(rt, b->target, (int)total, buf, new_target)
                          : embi_js_call(rt, b->target, b->this_v, (int)total, buf);
    if (buf != small) {
        embi_free(buf, (size_t)total * sizeof *buf);
    }
    return r;
}

static embi_js_value bound_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                int argc, const embi_js_value *argv)
{
    (void)this_v;
    return bound_invoke(rt, bound_of(f), argc, argv, false, EMBI_JS_UNDEFINED);
}

static embi_js_value bound_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                     const embi_js_value *argv, embi_js_value new_target)
{
    bound_data *b = bound_of(f);
    if (new_target == embi_js_from_object(f)) {
        new_target = b->target;
    }
    return bound_invoke(rt, b, argc, argv, true, new_target);
}

static void bound_trace(embi_js_object *f, embi_js_tracer *t)
{
    const bound_data *b = bound_of(f);
    if (!b) {
        return;
    }
    embi_js_mark_value(t, b->target);
    embi_js_mark_value(t, b->this_v);
    for (uint32_t i = 0; i < b->nargs; i++) {
        embi_js_mark_value(t, b->args[i]);
    }
    t->external += (size_t)b->nargs * sizeof *b->args;
}

static void bound_finalize(embi_js_object *f)
{
    bound_data *b = bound_of(f);
    if (!b) {
        return;
    }
    if (b->args) {
        embi_free(b->args, (size_t)b->nargs * sizeof *b->args);
    }
    embi_free(b, sizeof *b);
    f->internal = nullptr;
}

bool embi_js_bound_target(const embi_js_object *o, embi_js_value *target)
{
    if (o->klass != &embi_js_class_bound) {
        return false;
    }
    *target = bound_of(o)->target;
    return true;
}

/* ------------------------------------------------------------------ class table */

const embi_js_class embi_js_class_object = {
    .name = "Object",
    .flags = EMBI_JS_CLASS_IC,
};

const embi_js_class embi_js_class_array = {
    .name = "Array",
    .flags = EMBI_JS_CLASS_IC,
    .define_own = array_define_own,
};

const embi_js_class embi_js_class_string = {
    .name = "String",
    .get_own = string_get_own,
    .define_own = string_define_own,
    .own_keys = string_own_keys,
    .trace = wrapper_trace,
};

const embi_js_class embi_js_class_number = {
    .name = "Number",
    .flags = EMBI_JS_CLASS_IC,
    .trace = wrapper_trace,
};

const embi_js_class embi_js_class_boolean = {
    .name = "Boolean",
    .flags = EMBI_JS_CLASS_IC,
    .trace = wrapper_trace,
};

const embi_js_class embi_js_class_symbol = {
    .name = "Symbol",
    .flags = EMBI_JS_CLASS_IC,
    .trace = wrapper_trace,
};

const embi_js_class embi_js_class_error = {
    .name = "Error",
    .flags = EMBI_JS_CLASS_IC,
};

const embi_js_class embi_js_class_bound = {
    .name = "BoundFunction",
    .flags = EMBI_JS_CLASS_IC,
    .call = bound_call,
    .construct = bound_construct,
    .trace = bound_trace,
    .finalize = bound_finalize,
};

/* ------------------------------------------------------------------ creation */

static embi_js_object *alloc_object(embi_js_runtime *rt, embi_js_value proto,
                                    const embi_js_class *klass)
{
    embi_js_object *o = (embi_js_object *)embi_js_alloc_cell(rt, sizeof *o, EMBI_JS_CELL_OBJECT);
    o->hdr.flags = EMBI_JS_OBJ_EXTENSIBLE;
    o->klass = klass;
    embi_js_shape *s = root_shape(rt, proto);
    EMBI_JS_WB(o, embi_js_from_cell(s));
    o->shape = s;
    return o;
}

/* An intrinsic prototype, or null when the realm has not set it yet. */
static embi_js_value intrinsic_proto(embi_js_runtime *rt, enum embi_js_proto which)
{
    embi_js_value v = rt->protos[which];
    return embi_js_is_empty(v) ? EMBI_JS_NULL : v;
}

embi_js_object *embi_js_object_new(embi_js_runtime *rt, embi_js_value proto)
{
    return alloc_object(rt, proto, &embi_js_class_object);
}

embi_js_object *embi_js_object_new_class(embi_js_runtime *rt, embi_js_value proto,
                                         const embi_js_class *klass)
{
    return alloc_object(rt, proto, klass);
}

embi_js_object *embi_js_object_new_plain(embi_js_runtime *rt)
{
    return alloc_object(rt, intrinsic_proto(rt, EMBI_JS_PROTO_OBJECT), &embi_js_class_object);
}

embi_js_object *embi_js_array_create(embi_js_runtime *rt, double len, embi_js_value proto)
{
    if (!(len >= 0.0 && len <= 4294967295.0)) {
        (void)embi_js_throw_range_error(rt, "Invalid array length");
        return nullptr;
    }
    embi_js_object *a = alloc_object(rt, proto, &embi_js_class_array);
    /* PACKED means elements_len == length with no hole, so only the empty array starts packed. */
    if (len == 0.0) {
        a->hdr.flags |= EMBI_JS_OBJ_PACKED;
    }
    store_add(rt, a, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(len), ATTR_W);
    return a;
}

embi_js_object *embi_js_array_create_default(embi_js_runtime *rt, double len)
{
    return embi_js_array_create(rt, len, intrinsic_proto(rt, EMBI_JS_PROTO_ARRAY));
}

embi_js_object *embi_js_primitive_wrapper_new(embi_js_runtime *rt, embi_js_value prim)
{
    const embi_js_class *klass;
    enum embi_js_proto which;
    if (embi_js_is_string(prim)) {
        klass = &embi_js_class_string;
        which = EMBI_JS_PROTO_STRING;
    } else if (embi_js_is_number(prim)) {
        klass = &embi_js_class_number;
        which = EMBI_JS_PROTO_NUMBER;
    } else if (embi_js_is_bool(prim)) {
        klass = &embi_js_class_boolean;
        which = EMBI_JS_PROTO_BOOLEAN;
    } else {
        EMBI_ASSERT(embi_js_is_symbol(prim));
        klass = &embi_js_class_symbol;
        which = EMBI_JS_PROTO_SYMBOL;
    }
    embi_js_object *o = alloc_object(rt, intrinsic_proto(rt, which), klass);
    EMBI_JS_WB(o, prim);
    o->internal = (void *)(uintptr_t)prim;
    EMBI_JS_WB_STRUCT(o);
    if (embi_js_is_string(prim)) {
        uint32_t n = embi_js_str_len(embi_js_as_string(prim));
        store_add(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number((double)n), 0);
    }
    return o;
}

embi_js_object *embi_js_bound_function_new(embi_js_runtime *rt, embi_js_object *target,
                                           embi_js_value bound_this, int argc,
                                           const embi_js_value *args)
{
    /* The prototype comes from the target's [[GetPrototypeOf]], which is a trap for a proxy. */
    embi_js_value proto;
    if (embi_js_obj_get_prototype_of(rt, target, &proto) == EMBI_JS_THREW) {
        return nullptr;
    }
    embi_js_object *f = alloc_object(rt, proto, &embi_js_class_bound);
    if (embi_js_obj_is_constructor(target)) {
        f->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    }
    bound_data *b = embi_calloc(1, sizeof *b);
    b->target = embi_js_from_object(target);
    b->this_v = bound_this;
    EMBI_JS_WB(f, b->target);
    EMBI_JS_WB(f, b->this_v);
    b->nargs = (uint32_t)argc;
    if (argc > 0) {
        b->args = embi_alloc((size_t)argc * sizeof *b->args);
        embi_js_heap_note_external(&rt->heap, (size_t)argc * sizeof *b->args);
        for (int i = 0; i < argc; i++) {
            b->args[i] = args[i];
            EMBI_JS_WB(f, args[i]);
        }
    }
    f->internal = b;
    EMBI_JS_WB_STRUCT(f);
    return f;
}

/* ------------------------------------------------------------------ public internal-method view */

/* Keep the exported header's names in sync with the definitions above. */
static_assert(EMBI_JS_NAME_COUNT == 31, "name table size");

static const char *const name_text[] = {
    "length",   "prototype", "constructor", "value",    "writable",  "get",     "set",
    "enumerable", "configurable", "message", "name",    "toString", "valueOf", "default",
    "number",   "string",    "boolean",     "symbol",  "object",    "function", "undefined",
    "null",     "true",      "false",     "then",     "resolve",
    "index",    "input",     "groups",    "indices",  "",
};

static const char *const wk_text[] = {
    "Symbol.asyncIterator", "Symbol.hasInstance", "Symbol.isConcatSpreadable",
    "Symbol.iterator",      "Symbol.match",       "Symbol.matchAll",
    "Symbol.replace",       "Symbol.search",      "Symbol.species",
    "Symbol.split",         "Symbol.toPrimitive", "Symbol.toStringTag",
    "Symbol.unscopables",
};

static_assert(sizeof name_text / sizeof name_text[0] == EMBI_JS_NAME_COUNT,
              "name_text must list every enum embi_js_name entry");
static_assert(sizeof wk_text / sizeof wk_text[0] == EMBI_JS_WK_COUNT,
              "wk_text must list every enum embi_js_wk entry");

void embi_js_objects_init(embi_js_runtime *rt)
{
    if (!rt->heap.ops[EMBI_JS_CELL_OBJECT].trace && !rt->heap.ops[EMBI_JS_CELL_OBJECT].finalize) {
        embi_js_set_type_ops(rt, EMBI_JS_CELL_OBJECT,
                             (embi_js_type_ops){object_trace, object_finalize});
    }
    embi_js_root_add(rt, mark_intrinsics, rt);
}

/* Interns the property names and creates the well-known symbols. Done on first use, so a fresh
 * runtime's atom table stays empty. Nothing here calls back into this function. */
void embi_js_names_init(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < EMBI_JS_NAME_COUNT; i++) {
        rt->names[i] = embi_js_from_string(
            embi_js_atom_intern_utf8(rt, name_text[i], strlen(name_text[i])));
    }
    for (uint32_t i = 0; i < EMBI_JS_WK_COUNT; i++) {
        embi_js_string *desc = embi_js_atom_intern_utf8(rt, wk_text[i], strlen(wk_text[i]));
        rt->wk[i] = embi_js_from_symbol(embi_js_symbol_new(rt, embi_js_from_string(desc)));
    }
}

/* ------------------------------------------------------------------ private elements (J2d) */

/* The own private element of o keyed by the private name k, or false when o has none. Reads the
 * object's own storage: the class hooks (proxies included) never see a private element. */
bool embi_js_priv_get_own(embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    return ordinary_get_own(nullptr, o, k, out) == EMBI_JS_YES;
}

/* ------------------------------------------------------------------ inline cache support (ic.c) */

bool embi_js_obj_ic_own(const embi_js_object *o, embi_js_key k, uint32_t *slot, uint8_t *attrs)
{
    if (embi_js_key_is_index(k)) {
        return false;
    }
    if (embi_js_obj_dict(o)) {
        embi_js_dict_entry *e = dict_find(embi_js_obj_dict(o), k);
        if (!e) {
            return false;
        }
        *slot = (uint32_t)(e - embi_js_obj_dict(o)->entries);
        *attrs = e->attrs;
        return true;
    }
    embi_js_shape *n = embi_js_shape_find(o->shape, k);
    if (!n) {
        return false;
    }
    *slot = n->count - 1u;
    *attrs = n->attrs;
    return true;
}

embi_js_value *embi_js_obj_ic_value(embi_js_object *o, uint32_t slot)
{
    if (embi_js_obj_dict(o)) {
        return &embi_js_obj_dict(o)->entries[slot].value;
    }
    return embi_js_obj_slot(o, slot);
}

void embi_js_obj_ic_add(embi_js_runtime *rt, embi_js_object *o, embi_js_shape *next, embi_js_value v)
{
    reserve_slots(rt, o, next->count);
    EMBI_JS_WB(o, v);
    *slot_at(o, next->count - 1u) = v;
    EMBI_JS_WB(o, embi_js_from_cell(next));
    o->shape = next;
}
