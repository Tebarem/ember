/*
 * Property inline caches (J5c). See ic.h for the model and the validation rules.
 *
 * Fill steps run before the generic operation they describe, and they only read shapes, so the
 * entry describes the state the operation starts from. Hits revalidate that state. Anything the
 * fill cannot describe (a hook, an index key, a non-IC class, a chain deeper than the limit, a
 * setter or read-only property found on the chain) leaves the site uncached for that receiver and
 * counts as a fill failure.
 */
#include "js/ic.h"
#include "js/ops.h"

#include <stdint.h>
#include <string.h>

#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_A EMBI_JS_ATTR_ACCESSOR

#define UNDEF EMBI_JS_UNDEFINED

/* ------------------------------------------------------------------ allocation and GC hooks */

static size_t chain_bytes(uint32_t n)
{
    return sizeof(embi_js_ic_chain) + (size_t)n * sizeof(embi_js_shape *);
}

embi_js_ic *embi_js_ic_alloc(uint32_t n)
{
    return n ? embi_calloc(n, sizeof(embi_js_ic)) : nullptr;
}

void embi_js_ic_init(embi_js_runtime *rt, embi_js_ic *ic, embi_js_key key)
{
    ic->key = key;
    ic->key_length = !embi_js_key_is_index(key) && key == embi_js_name_val(rt, EMBI_JS_NAME_LENGTH);
}

/* Entry i of the site: entry 0 is inline, the rest live in ic->more (allocated before use). */
static embi_js_ic_entry *entry_at(embi_js_ic *ic, uint32_t i)
{
    return i == 0 ? &ic->e0 : &ic->more->e[i - 1u];
}

static embi_js_ic_chain **chain_at(embi_js_ic *ic, uint32_t i)
{
    return i == 0 ? &ic->chain0 : &ic->more->chain[i - 1u];
}

/* Releases every entry's side record and the entries beyond the first. The site becomes empty. */
static void ic_clear(embi_js_ic *ic)
{
    for (uint32_t i = 0; i < ic->nentries; i++) {
        embi_js_ic_chain **c = chain_at(ic, i);
        if (*c) {
            embi_free(*c, chain_bytes((*c)->n));
            *c = nullptr;
        }
    }
    ic->nentries = 0;
    if (!ic->isglob && ic->more) {
        embi_free(ic->more, sizeof(embi_js_ic_more));
        ic->more = nullptr;
    }
}

void embi_js_ic_free(embi_js_ic *ics, uint32_t n)
{
    if (!ics) {
        return; /* a code cell whose build did not reach its site table */
    }
    for (uint32_t i = 0; i < n; i++) {
        ic_clear(&ics[i]);
    }
    embi_free(ics, (size_t)n * sizeof(embi_js_ic));
}

void embi_js_ic_trace(embi_js_ic *ics, uint32_t n, embi_js_tracer *t)
{
    if (!ics) {
        return;
    }
    t->external += (size_t)n * sizeof(embi_js_ic);
    for (uint32_t i = 0; i < n; i++) {
        embi_js_ic *ic = &ics[i];
        embi_js_mark_value(t, ic->key);
        if (!ic->isglob && ic->more) {
            t->external += sizeof(embi_js_ic_more);
        }
        if (ic->gkind == EMBI_JS_IC_G_LEX) {
            embi_js_mark_cell(t, &ic->gbox->hdr);
        } else if (ic->gkind == EMBI_JS_IC_G_OBJ) {
            embi_js_mark_cell(t, &ic->gshape->hdr);
        }
        for (uint32_t j = 0; j < ic->nentries; j++) {
            embi_js_mark_cell(t, &entry_at(ic, j)->shape->hdr);
            const embi_js_ic_chain *ch = *chain_at(ic, j);
            if (ch) {
                t->external += chain_bytes(ch->n);
                for (uint32_t k = 0; k < ch->n; k++) {
                    embi_js_mark_cell(t, &ch->s[k]->hdr);
                }
                if (ch->next) {
                    embi_js_mark_cell(t, &ch->next->hdr);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ class and shape checks */

/* A class whose named keys are answered by ordinary storage for reads. */
static bool class_get_ok(const embi_js_object *o)
{
    const embi_js_class *k = o->klass;
    return (k->flags & EMBI_JS_CLASS_IC) && !k->get_own && !k->get && !k->has;
}

bool embi_js_ic_class_plain(const embi_js_object *o)
{
    return class_get_ok(o) && !o->klass->set && !o->klass->define_own;
}

/* prim: o is on the chain of a primitive receiver's intrinsic prototype (embi_js_ic_get_prim). A String
 * wrapper's only hook answers index and length keys, and a primitive site never looks those up (it
 * takes the generic path), so such a wrapper stays in the chain. Ordinary objects are as before. */
static bool class_get_ok_p(const embi_js_object *o, bool prim)
{
    return class_get_ok(o) || (prim && o->klass == &embi_js_class_string);
}

/* A class whose named define takes the ordinary path: no define_own hook, except the array's, which
 * is ordinary for every named key but length. */
static bool class_define_ok(const embi_js_object *o, bool key_length)
{
    const embi_js_class *k = o->klass;
    /* A watched intrinsic (Array.prototype, %Array%, %ArrayIteratorPrototype%) takes every named
     * store through embi_js_put, which clears the protector for the key (object.c protect_named_store).
     * A cached store would skip that, and a shape-identical ordinary object can share the cache entry. */
    if (!class_get_ok(o) || (o->hdr.flags & EMBI_JS_OBJ_WATCHED)) {
        return false;
    }
    if (!k->define_own) {
        return true;
    }
    return k == &embi_js_class_array && !key_length;
}

/* A class whose named [[Set]] on the receiver takes the ordinary path. */
static bool class_set_ok(const embi_js_object *o, bool key_length)
{
    return !o->klass->set && class_define_ok(o, key_length);
}

static bool is_dict(const embi_js_object *o)
{
    return (o->shape->hdr.flags & EMBI_JS_SHAPE_DICT) != 0;
}

/* A dictionary-mode object may be cached only as a prototype, whose dictionary shape is renewed on
 * every key change (object.c). A plain dictionary object is not stable enough to key on. */
static bool shape_stable(const embi_js_object *o)
{
    return !is_dict(o) || (o->hdr.flags & EMBI_JS_OBJ_IS_PROTO) != 0;
}

/* ------------------------------------------------------------------ entries */

/* The holder of entry e for a lookup from o, or nullptr when the live objects no longer match. ch
 * is the entry's chain (used only when e->depth > 0). */
static embi_js_object *entry_holder(const embi_js_ic_entry *e, const embi_js_ic_chain *ch,
                                    embi_js_object *o, bool prim)
{
    if (o->shape != e->shape || !class_get_ok_p(o, prim)) {
        return nullptr;
    }
    for (uint32_t i = 0; i < e->depth; i++) {
        embi_js_value p = o->shape->proto;
        if (!embi_js_is_object(p)) {
            return nullptr;
        }
        o = embi_js_as_object(p);
        if (o->shape != ch->s[i] || !class_get_ok_p(o, prim)) {
            return nullptr;
        }
    }
    return o;
}

/* An ADD entry's chain: every prototype from o's up to the end is still absent for the key, and
 * none of them has a [[Set]] hook. */
static bool add_chain_ok(const embi_js_ic_chain *ch, embi_js_object *o)
{
    for (uint32_t i = 0; i < ch->n; i++) {
        embi_js_value p = o->shape->proto;
        if (!embi_js_is_object(p)) {
            return false;
        }
        o = embi_js_as_object(p);
        if (o->shape != ch->s[i] || !class_get_ok(o) || o->klass->set) {
            return false;
        }
    }
    return !embi_js_is_object(o->shape->proto);
}

/* Builds a side record (owned by the entry it is stored in) with the given shapes and next. */
static embi_js_ic_chain *chain_new(embi_js_runtime *rt, embi_js_cell *owner, uint32_t n,
                                   embi_js_shape *const *shapes, embi_js_shape *next)
{
    embi_js_ic_chain *ch = embi_alloc(chain_bytes(n));
    ch->n = n;
    ch->next = nullptr;
    for (uint32_t i = 0; i < n; i++) {
        ch->s[i] = shapes[i];
        EMBI_JS_WB(owner, embi_js_from_cell(shapes[i]));
    }
    if (next) {
        ch->next = next;
        EMBI_JS_WB(owner, embi_js_from_cell(next));
    }
    embi_js_heap_note_external(&rt->heap, chain_bytes(n));
    return ch;
}

static void chain_drop(embi_js_ic_chain *ch)
{
    if (ch) {
        embi_free(ch, chain_bytes(ch->n));
    }
}

/* Puts a new entry (with its side record, which the site takes over) into ic. An entry for the same
 * receiver shape replaces the old one. The site goes megamorphic when a fifth shape arrives: its
 * entries are dropped and it waits EMBI_JS_IC_RETRY generic executions before it fills again. */
static void ic_insert(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_ic_entry ne,
                      embi_js_ic_chain *ch)
{
    for (uint32_t i = 0; i < ic->nentries; i++) {
        embi_js_ic_entry *e = entry_at(ic, i);
        if (e->shape == ne.shape) {
            embi_js_ic_chain **c = chain_at(ic, i);
            chain_drop(*c);
            *e = ne;
            *c = ch;
            EMBI_JS_WB(owner, embi_js_from_cell(ne.shape));
            goto recorded;
        }
    }
    if (ic->nentries == EMBI_JS_IC_POLY) {
        chain_drop(ch);
        ic_clear(ic);
        ic->state = EMBI_JS_IC_MEGA;
        ic->skip = (uint16_t)(EMBI_JS_IC_RETRY << ic->mega_level);
        if (ic->mega_level < EMBI_JS_IC_MEGA_MAXLEVEL) {
            ic->mega_level++;
        }
        ic->fails = 0;
        return;
    }
    uint32_t i = ic->nentries;
    if (i > 0 && !ic->more) {
        ic->more = embi_calloc(1, sizeof(embi_js_ic_more));
        embi_js_heap_note_external(&rt->heap, sizeof(embi_js_ic_more));
    }
    *entry_at(ic, i) = ne;
    *chain_at(ic, i) = ch;
    EMBI_JS_WB(owner, embi_js_from_cell(ne.shape));
    ic->nentries++;
    ic->state = ic->nentries == 1u ? EMBI_JS_IC_MONO : EMBI_JS_IC_POLYMORPHIC;
recorded:
    ic->fails = 0;
    if (ic->fills < UINT16_MAX) {
        ic->fills++;
    }
}

static embi_js_value call_getter(embi_js_runtime *rt, embi_js_value getter, embi_js_value receiver,
                                 embi_js_key key);
static bool describe_get(embi_js_object *obj, embi_js_key key, bool prim, uint32_t max_depth,
                         embi_js_shape *sh[], uint32_t *depth_out, uint32_t *slot_out,
                         uint8_t *attrs_out);

/* ------------------------------------------------------------------ megamorphic stub cache (J5m1)
 *
 * A runtime-wide direct-mapped table of (receiver shape, key) entries, consulted by a site that is
 * megamorphic (more than EMBI_JS_IC_POLY shapes) before the generic lookup. An entry records what
 * describe_get found for that shape and key: the holder's depth, the shapes of the objects on the
 * path (so a hit revalidates the chain exactly as an IC entry does, with no epoch), the slot and the
 * kind. STORE entries are own writable data properties (depth 0). Entries hold shapes without
 * marking them, so the table is emptied at the start of every collection (embi_js_stub_cache_clear),
 * before any shape can be freed. A miss runs the generic operation, and the fill runs before it,
 * reading shapes only, as for the site entries. */
/* The table, allocated on first use. */
static struct embi_js_stub_cache *stub_cache_get(embi_js_runtime *rt)
{
    if (!rt->stub_cache) {
        rt->stub_cache = embi_calloc(1, sizeof(struct embi_js_stub_cache));
        /* Counted as external memory, so the pacing sees the table (see embi_js_heap_note_external). */
        embi_js_heap_note_external(&rt->heap, sizeof(struct embi_js_stub_cache));
    }
    return rt->stub_cache;
}

void embi_js_stub_cache_clear(embi_js_runtime *rt)
{
    if (rt->stub_cache) {
        memset(rt->stub_cache->e, 0, sizeof rt->stub_cache->e);
    }
}

uint32_t embi_js_stub_cache_count(embi_js_runtime *rt)
{
    uint32_t n = 0;
    if (rt->stub_cache) {
        for (uint32_t i = 0; i < EMBI_JS_STUB_SIZE; i++) {
            n += rt->stub_cache->e[i].shape != nullptr;
        }
    }
    return n;
}

uint64_t embi_js_stub_cache_fills(embi_js_runtime *rt)
{
    return rt->stub_cache ? rt->stub_cache->fills : 0;
}

void embi_js_stub_cache_free(embi_js_runtime *rt)
{
    if (rt->stub_cache) {
        embi_free(rt->stub_cache, sizeof(struct embi_js_stub_cache));
        rt->stub_cache = nullptr;
    }
}

/* The entry a fill writes: the pair's own entry if present, else an empty one of its two ways, else the
 * way picked by the fill count (so a set that two pairs share does not always lose the same one). */
static embi_js_stub_entry *stub_victim(struct embi_js_stub_cache *st, const embi_js_shape *sh, embi_js_key key,
                                       bool store)
{
    uint32_t i = embi_js_stub_index(sh, key, store);
    embi_js_stub_entry *a = &st->e[i];
    embi_js_stub_entry *b = &st->e[i ^ 1u];
    if (a->shape == sh && a->key == key && (a->kind == EMBI_JS_IC_STORE) == store) {
        return a;
    }
    if (b->shape == sh && b->key == key && (b->kind == EMBI_JS_IC_STORE) == store) {
        return b;
    }
    if (!a->shape) {
        return a;
    }
    if (!b->shape) {
        return b;
    }
    return (st->fills & 1u) ? b : a;
}

/* A hit: the value of a DATA or ACCESSOR entry, or false. Revalidates the receiver and every object
 * on the chain, with the same class rules as entry_holder. */
static bool stub_get(embi_js_runtime *rt, embi_js_object *obj, embi_js_key key, bool prim,
                     embi_js_value receiver, embi_js_value *out)
{
    if (!rt->stub_cache || !class_get_ok_p(obj, prim)) {
        return false;
    }
    const embi_js_stub_entry *e = embi_js_stub_find(rt->stub_cache, obj->shape, key, false);
    if (!e) {
        return false;
    }
    embi_js_object *h = obj;
    for (uint32_t i = 0; i < e->depth; i++) {
        embi_js_value p = h->shape->proto;
        if (!embi_js_is_object(p)) {
            return false;
        }
        h = embi_js_as_object(p);
        if (h->shape != e->hs[i] || !class_get_ok_p(h, prim)) {
            return false;
        }
    }
    embi_js_value v = *embi_js_obj_ic_value(h, e->slot);
    if (e->kind == EMBI_JS_IC_DATA) {
        *out = v;
    } else {
        *out = call_getter(rt, ((const embi_js_accessor *)embi_js_as_cell(v))->getter, receiver, key);
    }
    return true;
}

/* Records what describe_get finds for (obj's shape, key). Nothing is recorded for a description the
 * entry format cannot hold (a chain deeper than EMBI_JS_STUB_DEPTH, an index key, a dictionary
 * receiver that is not a prototype, a hook). */
static void stub_fill_get(embi_js_runtime *rt, embi_js_object *obj, embi_js_key key, bool prim)
{
    embi_js_shape *sh[EMBI_JS_IC_MAXDEPTH + 1u];
    uint32_t depth;
    uint32_t slot;
    uint8_t attrs;
    if (!describe_get(obj, key, prim, EMBI_JS_STUB_DEPTH, sh, &depth, &slot, &attrs)) {
        return;
    }
    struct embi_js_stub_cache *st = stub_cache_get(rt);
    if (!st) {
        return;
    }
    embi_js_stub_entry *e = stub_victim(st, obj->shape, key, false);
    e->shape = obj->shape;
    e->key = key;
    for (uint32_t i = 0; i < depth; i++) {
        e->hs[i] = sh[i + 1u];
    }
    e->slot = slot;
    e->kind = (uint8_t)((attrs & ATTR_A) ? EMBI_JS_IC_ACCESSOR : EMBI_JS_IC_DATA);
    e->depth = (uint8_t)depth;
    e->klass = obj->klass;
    e->fast = (uint8_t)(depth == 0 && e->kind == EMBI_JS_IC_DATA && !prim && !is_dict(obj) && class_get_ok(obj));
    st->fills++;
}

/* [[Set]] of an own writable data property through a STORE entry. */
static bool stub_put(embi_js_runtime *rt, embi_js_object *o, embi_js_key key, bool key_length,
                     embi_js_value v)
{
    if (!rt->stub_cache || !class_set_ok(o, key_length)) {
        return false;
    }
    const embi_js_stub_entry *e = embi_js_stub_find(rt->stub_cache, o->shape, key, true);
    if (!e) {
        return false;
    }
    EMBI_JS_WB(o, v);
    *embi_js_obj_ic_value(o, e->slot) = v;
    return true;
}

static void stub_fill_put(embi_js_runtime *rt, embi_js_object *o, embi_js_key key, bool key_length)
{
    if (embi_js_key_is_index(key) || is_dict(o) || !class_set_ok(o, key_length)) {
        return;
    }
    uint32_t slot;
    uint8_t attrs;
    if (!embi_js_obj_ic_own(o, key, &slot, &attrs) || (attrs & ATTR_A) || !(attrs & ATTR_W) ||
        (attrs & EMBI_JS_ATTR_LAZY)) {
        return;
    }
    struct embi_js_stub_cache *st = stub_cache_get(rt);
    if (!st) {
        return;
    }
    embi_js_stub_entry *e = stub_victim(st, o->shape, key, true);
    e->shape = o->shape;
    e->key = key;
    e->slot = slot;
    e->kind = EMBI_JS_IC_STORE;
    e->depth = 0;
    e->klass = o->klass;
    e->fast = (uint8_t)(!is_dict(o) && class_get_ok(o) && !o->klass->set && !o->klass->define_own);
    st->fills++;
}

/* A read at a megamorphic site: the stub cache, else the generic lookup (after a fill attempt). */
static embi_js_value mega_get(embi_js_runtime *rt, embi_js_ic *ic, embi_js_object *obj, bool prim,
                              embi_js_value receiver)
{
    embi_js_value v;
    if (stub_get(rt, obj, ic->key, prim, receiver, &v)) {
        return v;
    }
    stub_fill_get(rt, obj, ic->key, prim);
    return embi_js_obj_get(rt, obj, ic->key, receiver);
}

/* ------------------------------------------------------------------ fills (true: an entry was recorded) */

/* Describes a read of key from obj (prim: from a primitive's intrinsic prototype): the holder's
 * depth, the shapes of the objects at depths 1..depth in sh[1..depth] (sh[0] is obj's shape), and
 * the holder's slot and attributes. The chain is at most max_depth hops. Reads no user code. */
static bool describe_get(embi_js_object *obj, embi_js_key key, bool prim, uint32_t max_depth,
                         embi_js_shape *sh[], uint32_t *depth_out, uint32_t *slot_out,
                         uint8_t *attrs_out)
{
    if (embi_js_key_is_index(key) || !class_get_ok_p(obj, prim) || !shape_stable(obj)) {
        return false;
    }
    embi_js_object *cur = obj;
    uint32_t depth = 0;
    sh[0] = obj->shape;
    for (;;) {
        if (!class_get_ok_p(cur, prim) || !shape_stable(cur)) {
            return false;
        }
        if (embi_js_obj_ic_own(cur, key, slot_out, attrs_out)) {
            if (*attrs_out & EMBI_JS_ATTR_LAZY) {
                return false; /* J5n2: the value does not exist yet; the generic read creates it */
            }
            *depth_out = depth;
            return true;
        }
        embi_js_value p = cur->shape->proto;
        if (!embi_js_is_object(p) || depth == max_depth) {
            return false;
        }
        cur = embi_js_as_object(p);
        sh[++depth] = cur->shape;
    }
}

static bool fill_get(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *obj,
                     bool prim)
{
    embi_js_shape *sh[EMBI_JS_IC_MAXDEPTH + 1u];
    uint32_t depth;
    uint32_t slot;
    uint8_t attrs;
    if (!describe_get(obj, ic->key, prim, EMBI_JS_IC_MAXDEPTH, sh, &depth, &slot, &attrs)) {
        return false;
    }
    embi_js_ic_entry ne = {obj->shape, slot,
                           (uint8_t)((attrs & ATTR_A) ? EMBI_JS_IC_ACCESSOR : EMBI_JS_IC_DATA),
                           (uint8_t)depth};
    embi_js_ic_chain *ch = depth > 0 ? chain_new(rt, owner, depth, &sh[1], nullptr) : nullptr;
    ic_insert(rt, owner, ic, ne, ch);
    return true;
}

static bool fill_put(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o)
{
    if (embi_js_key_is_index(ic->key) || is_dict(o) || !class_set_ok(o, ic->key_length)) {
        return false;
    }
    uint32_t slot;
    uint8_t attrs;
    if (embi_js_obj_ic_own(o, ic->key, &slot, &attrs)) {
        if ((attrs & ATTR_A) || !(attrs & ATTR_W) || (attrs & EMBI_JS_ATTR_LAZY)) {
            return false;
        }
        embi_js_ic_entry ne = {o->shape, slot, EMBI_JS_IC_STORE, 0};
        ic_insert(rt, owner, ic, ne, nullptr);
        return true;
    }
    if (!embi_js_obj_is_extensible(o) || o->shape->count >= EMBI_JS_SHAPE_MAX_CHAIN) {
        return false;
    }
    /* The key must be absent from every prototype, and none may take the set through a hook. */
    embi_js_shape *sh[EMBI_JS_IC_MAXDEPTH];
    uint32_t n = 0;
    embi_js_object *cur = o;
    for (;;) {
        embi_js_value p = cur->shape->proto;
        if (!embi_js_is_object(p)) {
            break;
        }
        if (n == EMBI_JS_IC_MAXDEPTH) {
            return false;
        }
        cur = embi_js_as_object(p);
        if (!class_get_ok(cur) || cur->klass->set || !shape_stable(cur)) {
            return false;
        }
        uint32_t s2;
        uint8_t a2;
        if (embi_js_obj_ic_own(cur, ic->key, &s2, &a2)) {
            return false;
        }
        sh[n++] = cur->shape;
    }
    embi_js_shape *next = embi_js_shape_add(rt, o->shape, ic->key, EMBI_JS_ATTR_DEFAULT);
    embi_js_ic_entry ne = {o->shape, 0, EMBI_JS_IC_ADD, (uint8_t)n};
    embi_js_ic_chain *ch = chain_new(rt, owner, n, sh, next);
    ic_insert(rt, owner, ic, ne, ch);
    return true;
}

static bool fill_define(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o)
{
    if (embi_js_key_is_index(ic->key) || is_dict(o) || !class_define_ok(o, ic->key_length)) {
        return false;
    }
    uint32_t slot;
    uint8_t attrs;
    if (embi_js_obj_ic_own(o, ic->key, &slot, &attrs)) {
        /* CreateDataProperty on a writable, enumerable, configurable data property is a value write. */
        if ((attrs & ATTR_A) || attrs != EMBI_JS_ATTR_DEFAULT) {
            return false;
        }
        embi_js_ic_entry ne = {o->shape, slot, EMBI_JS_IC_STORE, 0};
        ic_insert(rt, owner, ic, ne, nullptr);
        return true;
    }
    if (!embi_js_obj_is_extensible(o) || o->shape->count >= EMBI_JS_SHAPE_MAX_CHAIN) {
        return false;
    }
    embi_js_shape *next = embi_js_shape_add(rt, o->shape, ic->key, EMBI_JS_ATTR_DEFAULT);
    embi_js_ic_entry ne = {o->shape, 0, EMBI_JS_IC_DEF_ADD, 0};
    embi_js_ic_chain *ch = chain_new(rt, owner, 0, nullptr, next);
    ic_insert(rt, owner, ic, ne, ch);
    return true;
}

/* ------------------------------------------------------------------ hits and misses */

static embi_js_value call_getter(embi_js_runtime *rt, embi_js_value getter, embi_js_value receiver,
                                 embi_js_key key)
{
    if (getter == UNDEF) {
        return UNDEF;
    }
    embi_js_value restricted;
    if (embi_js_restricted_get(rt, getter, receiver, key, &restricted)) {
        return restricted;
    }
    return embi_js_call(rt, getter, receiver, 0, nullptr);
}

/* A hit on a read entry of the site: the value, or the getter's result. Returns false on a miss. */
static bool hit_get(embi_js_runtime *rt, embi_js_ic *ic, embi_js_object *obj, bool prim,
                    embi_js_value receiver, embi_js_value *out)
{
    if (ic->state == EMBI_JS_IC_MEGA) {
        return false;
    }
    for (uint32_t i = 0; i < ic->nentries; i++) {
        const embi_js_ic_entry *e = entry_at(ic, i);
        if (e->kind != EMBI_JS_IC_DATA && e->kind != EMBI_JS_IC_ACCESSOR) {
            continue;
        }
        embi_js_object *h = entry_holder(e, *chain_at(ic, i), obj, prim);
        if (!h) {
            continue;
        }
        embi_js_value v = *embi_js_obj_ic_value(h, e->slot);
        if (e->kind == EMBI_JS_IC_DATA) {
            *out = v;
        } else {
            *out = call_getter(rt, ((const embi_js_accessor *)embi_js_as_cell(v))->getter, receiver, ic->key);
        }
        return true;
    }
    return false;
}

/* The fill turn of a generic execution at ic. A megamorphic site counts its wait down (embi_js_ic_may_fill)
 * and, once the wait ends, returns to uninitialized and fills again, so a site that was polymorphic only
 * during warm-up regains its entries. While it waits, it runs through the stub cache instead. */
static bool fill_turn(embi_js_ic *ic, bool *fill)
{
    *fill = embi_js_ic_may_fill(ic);
    return !*fill && ic->state == EMBI_JS_IC_MEGA;
}

embi_js_value embi_js_ic_get(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                             embi_js_object *obj, embi_js_value receiver)
{
    if (ic->state == EMBI_JS_IC_MEGA && ic->skip) {
        /* Waiting megamorphic site (the common case of a megamorphic one): straight to the stub cache. */
        ic->skip--;
        return mega_get(rt, ic, obj, false, receiver);
    }
    embi_js_value v;
    if (hit_get(rt, ic, obj, false, receiver, &v)) {
        return v;
    }
    bool fill;
    if (fill_turn(ic, &fill)) {
        return mega_get(rt, ic, obj, false, receiver);
    }
    if (fill && !fill_get(rt, owner, ic, obj, false)) {
        embi_js_ic_fill_failed(ic);
    }
    return embi_js_obj_get(rt, obj, ic->key, receiver);
}

embi_js_value embi_js_ic_get_prim(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                                  embi_js_value base)
{
    if (embi_js_key_is_index(ic->key) || ic->key_length) {
        return embi_js_get_v(rt, base, ic->key);
    }
    enum embi_js_proto which = EMBI_JS_PROTO_BIGINT;
    if (embi_js_is_string(base)) {
        which = EMBI_JS_PROTO_STRING;
    } else if (embi_js_is_number(base)) {
        which = EMBI_JS_PROTO_NUMBER;
    } else if (embi_js_is_bool(base)) {
        which = EMBI_JS_PROTO_BOOLEAN;
    } else if (embi_js_is_symbol(base)) {
        which = EMBI_JS_PROTO_SYMBOL;
    }
    if (embi_js_is_empty(rt->protos[which])) {
        return embi_js_get_v(rt, base, ic->key);
    }
    /* The wrapper ToObject would make has no own named property, so the lookup starts at the
     * intrinsic prototype with the primitive as the receiver, as GetV does. */
    embi_js_object *p = embi_js_as_object(rt->protos[which]);
    embi_js_value v;
    if (hit_get(rt, ic, p, true, base, &v)) {
        return v;
    }
    bool fill;
    if (fill_turn(ic, &fill)) {
        return mega_get(rt, ic, p, true, base);
    }
    if (fill && !fill_get(rt, owner, ic, p, true)) {
        embi_js_ic_fill_failed(ic);
    }
    return embi_js_obj_get(rt, p, ic->key, base);
}

int embi_js_ic_put(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o,
                   embi_js_value v, bool strict)
{
    if (ic->state == EMBI_JS_IC_MEGA && ic->skip) {
        ic->skip--;
        if (stub_put(rt, o, ic->key, ic->key_length, v)) {
            return EMBI_JS_YES;
        }
        stub_fill_put(rt, o, ic->key, ic->key_length);
        return embi_js_put(rt, o, ic->key, v, strict);
    }
    if (ic->state != EMBI_JS_IC_MEGA) {
        for (uint32_t i = 0; i < ic->nentries; i++) {
            const embi_js_ic_entry *e = entry_at(ic, i);
            if (e->shape != o->shape) {
                continue;
            }
            if (e->kind == EMBI_JS_IC_STORE) {
                if (!class_set_ok(o, ic->key_length)) {
                    continue;
                }
                EMBI_JS_WB(o, v);
                *embi_js_obj_ic_value(o, e->slot) = v;
                return EMBI_JS_YES;
            }
            if (e->kind == EMBI_JS_IC_ADD) {
                const embi_js_ic_chain *ch = *chain_at(ic, i);
                if (!class_set_ok(o, ic->key_length) || !embi_js_obj_is_extensible(o) || !add_chain_ok(ch, o)) {
                    continue;
                }
                embi_js_obj_ic_add(rt, o, ch->next, v);
                return EMBI_JS_YES;
            }
        }
    }
    bool fill;
    if (fill_turn(ic, &fill)) {
        if (stub_put(rt, o, ic->key, ic->key_length, v)) {
            return EMBI_JS_YES;
        }
        stub_fill_put(rt, o, ic->key, ic->key_length);
        return embi_js_put(rt, o, ic->key, v, strict);
    }
    if (fill && !fill_put(rt, owner, ic, o)) {
        embi_js_ic_fill_failed(ic);
    }
    return embi_js_put(rt, o, ic->key, v, strict);
}

int embi_js_ic_define(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o,
                      embi_js_value v)
{
    if (ic->state != EMBI_JS_IC_MEGA) {
        for (uint32_t i = 0; i < ic->nentries; i++) {
            const embi_js_ic_entry *e = entry_at(ic, i);
            if (e->shape != o->shape) {
                continue;
            }
            if (e->kind == EMBI_JS_IC_STORE) {
                if (!class_define_ok(o, ic->key_length)) {
                    continue;
                }
                EMBI_JS_WB(o, v);
                *embi_js_obj_ic_value(o, e->slot) = v;
                return EMBI_JS_YES;
            }
            if (e->kind == EMBI_JS_IC_DEF_ADD) {
                const embi_js_ic_chain *ch = *chain_at(ic, i);
                if (!class_define_ok(o, ic->key_length) || !embi_js_obj_is_extensible(o)) {
                    continue;
                }
                embi_js_obj_ic_add(rt, o, ch->next, v);
                return EMBI_JS_YES;
            }
        }
    }
    bool fill;
    if (fill_turn(ic, &fill)) {
        return embi_js_create_data_property_or_throw(rt, o, ic->key, v) == EMBI_JS_THREW ? EMBI_JS_THREW
                                                                                           : EMBI_JS_YES;
    }
    if (fill && !fill_define(rt, owner, ic, o)) {
        embi_js_ic_fill_failed(ic);
    }
    return embi_js_create_data_property_or_throw(rt, o, ic->key, v) == EMBI_JS_THREW ? EMBI_JS_THREW
                                                                                       : EMBI_JS_YES;
}
