#pragma once
/*
 * Property inline caches (J5c).
 *
 * Each GET_FIELD, SET_FIELD, GET_FIELD_THIS, DEFINE_FIELD and SUPER_GET site of a code cell owns one
 * embi_js_ic in code->ics. The op's operand is that site's index. The compiler (embi_jsc_emit_op32)
 * assigns the indices, and build_one fills in the canonical key of each site, so the interpreter
 * never interns a key at run time.
 *
 * A site goes uninitialized, then monomorphic, then polymorphic (up to EMBI_JS_IC_POLY entries,
 * linear scan) and then megamorphic, where it runs the generic operation and stops caching. Every
 * use revalidates its entry against the current shapes, so nothing is invalidated eagerly:
 *
 *   - Shape identity fixes a shape-mode object's keys, attributes, slot layout and prototype. A
 *     dictionary-mode prototype (EMBI_JS_OBJ_IS_PROTO) gets a fresh dictionary shape on each add,
 *     delete or attribute change (object.c dict_renew), so its shape is also a valid key.
 *   - A proto-chain entry records the shape of every object from the receiver down to the holder,
 *     and a use walks the live chain comparing shapes. A shadowing add, a prototype change or a
 *     dictionary renewal anywhere on the path misses.
 *   - Each object on the path must still have an IC-capable class: a Proxy or exotic object can
 *     share a shape with an ordinary object, so the class is checked at each use.
 *   - Entries hold their shapes strongly, through the owning code cell's trace function, so a shape
 *     address cannot be reused while an entry refers to it.
 *
 * Results equal the generic path in every case. A miss runs the generic operation, and the fill
 * step inspects shapes only, never running user code.
 *
 * Primitive receivers (J5d) use the same entries, keyed on the intrinsic prototype's shape: a string,
 * number, boolean, symbol or bigint method load starts at that prototype, with the primitive as the
 * receiver. String length is answered from the string and never reaches the site.
 *
 * Global sites (J5d: GET_GLOBAL, GET_GLOBAL_OPT, SET_GLOBAL) keep one entry in the g* fields rather
 * than the shape entries: a lexical box, or an own property of the global object (its shape and
 * slot). Both are valid for one realm lex count (a later let can shadow a global property). The
 * global object is flagged as a prototype, so its dictionary shape renews on every change of keys
 * or attributes. Proxy trap lookups are cached in builtins_proxy.c, not here.
 *
 * Layout: an entry is 16 bytes (shape, slot, kind, depth). Its side record (the prototype and
 * transition chain) is a separate allocation referenced from the site. The site keeps entry 0 and
 * its chain inline and allocates the entries 1..3 (and their chains) on the first polymorphic fill.
 *
 * Fill failures: a site that keeps failing to describe its receivers (a setter, a read-only
 * property, a Proxy, a Map) stops filling for EMBI_JS_IC_RETRY generic executions after
 * EMBI_JS_IC_FAIL_LIMIT failures in a row. This bounds the cost of a site that cannot cache.
 *
 * Megamorphic sites (J5m1): once a site has seen more than EMBI_JS_IC_POLY shapes it stops using
 * its own entries. Reads and stores of own or prototype-chain data go through the runtime's stub
 * cache (ic.c, a 1 024-entry two-way table of (receiver shape, key) entries, emptied at each collection),
 * and only a miss runs the generic lookup. The first entry of a site is also checked inline
 * (embi_js_ic_get_inline, embi_js_ic_put_inline) before the out-of-line probe. J5n1: reads and stores
 * of one pair use separate entries, a waiting megamorphic site probes the table inline (embi_js_ic_mega_*_inline),
 * and each return to megamorphic doubles the site's wait (mega_level), so a truly megamorphic site almost
 * never refills entries it will drop again.
 */
#include "js/builtins.h"
#include "js/object.h"
#include "js/runtime.h"
#include "js/shape.h"
#include "js/value.h"

#define EMBI_JS_IC_POLY 4u
/* Deepest prototype a cached lookup may reach (the holder's depth). */
#define EMBI_JS_IC_MAXDEPTH 4u
/* Failed fills in a row before a site stops filling, and the generic executions it then waits. */
#define EMBI_JS_IC_FAIL_LIMIT 8u
#define EMBI_JS_IC_RETRY 256u
/* A site that goes megamorphic again after a retry waits EMBI_JS_IC_RETRY << level executions (level
 * saturating at EMBI_JS_IC_MEGA_MAXLEVEL), so a site that is truly megamorphic spends almost no time
 * refilling entries it will drop again (J5n1: the props megamorphic sites refilled once per 50 reads). */
#define EMBI_JS_IC_MEGA_MAXLEVEL 7u

enum : uint8_t {
    EMBI_JS_IC_UNINIT,
    EMBI_JS_IC_MONO,
    EMBI_JS_IC_POLYMORPHIC,
    EMBI_JS_IC_MEGA,
};

/* What an entry does on a hit. */
enum : uint8_t {
    EMBI_JS_IC_DATA,     /* read the data property at depth (0 = the receiver itself) */
    EMBI_JS_IC_ACCESSOR, /* call the getter of the accessor at depth, read from the slot each time */
    EMBI_JS_IC_STORE,    /* write the own writable data property (depth 0) in place */
    EMBI_JS_IC_ADD,      /* SET on an absent key: add by transition after the chain is validated */
    EMBI_JS_IC_DEF_ADD,  /* DEFINE on an absent key: add by transition (no chain involved) */
};

/* The side record of an entry that needs more than its receiver shape. s[i] is the shape of the
 * object at depth i + 1 (for ADD, of every prototype up to the end of the chain). next is the
 * receiver's shape after the add (ADD and DEF_ADD). Owned by the entry, freed with it. */
typedef struct embi_js_ic_chain {
    embi_js_shape *next;
    uint32_t n;
    embi_js_shape *s[];
} embi_js_ic_chain;

/* 16 bytes: the receiver shape, the holder's shape slot (or dictionary position), and the kind and
 * depth. The chain (nullptr for depth-0 DATA and STORE) is kept beside the entry, see the site. */
typedef struct embi_js_ic_entry {
    embi_js_shape *shape; /* receiver shape */
    uint32_t slot;        /* shape slot of the holder, or its dictionary position */
    uint8_t kind;         /* EMBI_JS_IC_* kinds */
    uint8_t depth;        /* prototype hops from the receiver to the holder (DATA and ACCESSOR), or the chain length (ADD) */
} embi_js_ic_entry;

/* Entries 1..POLY-1 and their chains, allocated on the first fill that needs a second entry. */
typedef struct embi_js_ic_more {
    embi_js_ic_entry e[EMBI_JS_IC_POLY - 1u];
    embi_js_ic_chain *chain[EMBI_JS_IC_POLY - 1u];
} embi_js_ic_more;

typedef struct embi_js_ic {
    embi_js_key key;         /* canonical key of the site, resolved at compile time */
    uint8_t state;           /* EMBI_JS_IC_* states */
    uint8_t nentries;
    uint8_t key_length;      /* the key is "length": arrays take the generic define for it */
    uint8_t fails;           /* failed fills in a row (reset by a successful fill) */
    uint16_t skip;           /* generic executions left before fills resume (0: fills allowed) */
    uint16_t fills;          /* successful fills, saturating; the tests read it to tell hits from refills */
    /* Global sites (GET_GLOBAL, GET_GLOBAL_OPT, SET_GLOBAL; J5d). Not part of the entries below: the
     * entry is one lexical box (G_LEX) or one own property of the global object (G_OBJ). */
    uint8_t gkind;           /* EMBI_JS_IC_G_* (G_NONE: no entry) */
    uint8_t gwritable;       /* the entry's binding takes a write (writable, and not const) */
    uint8_t mega_level;      /* J5n1: times this site went megamorphic (saturating); the retry wait doubles with each */
    uint8_t isglob;          /* J5n2: a global fill has used the union below as the global record; sticky */
    uint32_t gepoch;         /* the realm's lex count when the entry was filled */
    /* J5n2: a site is either a field site (entries) or a global site, never both, so the two records
     * share storage (32 bytes instead of 64 per site; an IC array is 56 bytes a site, was 88).
     * isglob says which one is live. Only a field site reads or writes the entry members, and
     * nentries stays 0 on a global site. Code that walks every site (trace, free) checks isglob
     * before it looks at `more`. */
    union {
        struct {
            embi_js_ic_entry e0;     /* entry 0, inline */
            embi_js_ic_chain *chain0;
            embi_js_ic_more *more;   /* entries 1..3, nullptr until a second entry is filled */
        };
        struct {
            /* The realm the entry was filled in. A code cell can run under another realm (vm->realm),
             * and a binding of one realm's lex record must not answer for another. Compared, never
             * dereferenced. */
            embi_js_realm *grealm;
            uint32_t gslot;          /* G_OBJ: shape slot or dictionary position of the property */
            union {
                embi_js_box *gbox;       /* G_LEX: the lexical box (strong) */
                embi_js_shape *gshape;   /* G_OBJ: the global object's shape when filled (strong) */
            };
        };
    };
} embi_js_ic;

/* Global entry kinds. */
enum : uint8_t {
    EMBI_JS_IC_G_NONE,
    EMBI_JS_IC_G_LEX, /* a binding of the realm's global declarative record */
    EMBI_JS_IC_G_OBJ, /* an own property of the global object */
};

/* First-entry hits (J5m1). The interpreter tries these before the out-of-line probe. Each answers only
 * a hit on entry 0 that needs no chain, no getter and no dictionary lookup, with the same checks as
 * the out-of-line path (receiver shape, IC-capable class without the hooks the access would call).
 * Anything else returns false, and the out-of-line path gives the same result. */
static inline bool embi_js_ic_hookless(const embi_js_object *o)
{
    const embi_js_class *k = o->klass;
    return (k->flags & EMBI_JS_CLASS_IC) && !k->get_own && !k->get && !k->has;
}

static inline bool embi_js_ic_get_inline(const embi_js_ic *ic, embi_js_object *o, embi_js_value *out)
{
    const embi_js_ic_entry *e = &ic->e0;
    if (ic->nentries == 0 || e->kind != EMBI_JS_IC_DATA || e->depth != 0 || e->shape != o->shape ||
        (o->shape->hdr.flags & EMBI_JS_SHAPE_DICT) || !embi_js_ic_hookless(o)) {
        return false;
    }
    *out = *embi_js_obj_slot(o, e->slot);
    return true;
}

static inline bool embi_js_ic_put_inline(const embi_js_ic *ic, embi_js_object *o, embi_js_value v)
{
    const embi_js_ic_entry *e = &ic->e0;
    if (ic->nentries == 0 || e->kind != EMBI_JS_IC_STORE || e->shape != o->shape ||
        (o->shape->hdr.flags & EMBI_JS_SHAPE_DICT) || !embi_js_ic_hookless(o) || o->klass->set ||
        o->klass->define_own || (o->hdr.flags & EMBI_JS_OBJ_WATCHED)) {
        return false;
    }
    EMBI_JS_WB(o, v);
    *embi_js_obj_slot(o, e->slot) = v;
    return true;
}

#define EMBI_JS_STUB_BITS 10u
#define EMBI_JS_STUB_SIZE (1u << EMBI_JS_STUB_BITS)
/* Deepest holder the stub cache records (its chain is stored in the entry). */
#define EMBI_JS_STUB_DEPTH 3u
/* Mixed into the key hash of store entries, so a read and a store of one (shape, key) pair index
 * different slots (see stub_index). */
#define EMBI_JS_STUB_STORE_SALT 0xA24BAED4963EE407u

typedef struct embi_js_stub_entry {
    embi_js_shape *shape; /* receiver shape; nullptr when the entry is empty */
    embi_js_key key;
    embi_js_shape *hs[EMBI_JS_STUB_DEPTH]; /* shapes at depths 1..depth (the holder's is hs[depth - 1]) */
    uint32_t slot;
    uint8_t kind; /* EMBI_JS_IC_DATA, EMBI_JS_IC_ACCESSOR or EMBI_JS_IC_STORE */
    uint8_t depth;
    /* J5n1: the inline probes (embi_js_ic_mega_get_inline / _put_inline) answer only an entry with fast
     * set, and then only for a receiver of class `klass`. fast means: depth 0, a shape-mode receiver of a
     * class with no hooks the access would call (reads: DATA entry; stores: STORE entry, no set or
     * define_own hook). One compare of the class then replaces the per-hook checks. */
    uint8_t fast;
    const embi_js_class *klass;
} embi_js_stub_entry;

struct embi_js_stub_cache {
    embi_js_stub_entry e[EMBI_JS_STUB_SIZE];
    uint64_t fills; /* entries recorded (stub_fill_get / stub_fill_put), never reset */
};

/* Reads and stores of one (shape, key) pair use different entries (J5n1). A store entry and a read
 * entry for the same pair used to share one slot, so a read-modify-write such as o.c = o.c + 1
 * evicted its own entry on every step and both sides took the fill path each time. */
static inline uint32_t embi_js_stub_index(const embi_js_shape *sh, embi_js_key key, bool store)
{
    uint64_t h = ((uint64_t)(uintptr_t)sh >> 4) * 0x9E3779B97F4A7C15u;
    h ^= (key ^ (store ? EMBI_JS_STUB_STORE_SALT : 0u)) * 0xC2B2AE3D27D4EB4Fu;
    h ^= h >> 29;
    return (uint32_t)(h >> (64u - EMBI_JS_STUB_BITS));
}


/* The table is two-way: the pair (shape, key) lives at its index or at the index with the low bit flipped,
 * so two pairs that hash alike no longer evict each other on every use (J5n1: with one way, props lost a
 * third of its stub hits to such a collision, and by how much depended on where the allocator put the
 * shapes). store selects store entries over read entries. Returns nullptr on a miss. */
static inline const embi_js_stub_entry *embi_js_stub_find(const struct embi_js_stub_cache *st,
                                                          const embi_js_shape *sh, embi_js_key key, bool store)
{
    uint32_t i = embi_js_stub_index(sh, key, store);
    const embi_js_stub_entry *e = &st->e[i];
    if (e->shape == sh && e->key == key && (e->kind == EMBI_JS_IC_STORE) == store) {
        return e;
    }
    e = &st->e[i ^ 1u];
    if (e->shape == sh && e->key == key && (e->kind == EMBI_JS_IC_STORE) == store) {
        return e;
    }
    return nullptr;
}

/* Megamorphic hits answered inline (J5n1): a site that waits megamorphic (state MEGA, skip nonzero)
 * probes the stub cache from the interpreter without the call chain through embi_js_ic_get. Only the
 * simplest hit is answered here (own DATA / STORE entry, shape-mode receiver, no hooks); anything else
 * returns false and the out-of-line path decides, with the same result. A hit counts the site's wait
 * down exactly as the out-of-line path does. */
static inline bool embi_js_ic_mega_get_inline(const embi_js_runtime *rt, embi_js_ic *ic, embi_js_object *o,
                                              embi_js_value *out)
{
    if (ic->state != EMBI_JS_IC_MEGA || !ic->skip || !rt->stub_cache) {
        return false;
    }
    const embi_js_stub_entry *e = embi_js_stub_find(rt->stub_cache, o->shape, ic->key, false);
    if (!e || !e->fast || e->kind != EMBI_JS_IC_DATA || e->klass != o->klass) {
        return false;
    }
    ic->skip--;
    *out = *embi_js_obj_slot(o, e->slot);
    return true;
}

static inline bool embi_js_ic_mega_put_inline(const embi_js_runtime *rt, embi_js_ic *ic, embi_js_object *o,
                                              embi_js_value v)
{
    if (ic->state != EMBI_JS_IC_MEGA || !ic->skip || !rt->stub_cache) {
        return false;
    }
    const embi_js_stub_entry *e = embi_js_stub_find(rt->stub_cache, o->shape, ic->key, true);
    if (!e || !e->fast || e->klass != o->klass || (o->hdr.flags & EMBI_JS_OBJ_WATCHED)) {
        return false;
    }
    ic->skip--;
    EMBI_JS_WB(o, v);
    *embi_js_obj_slot(o, e->slot) = v;
    return true;
}

/* Fill suspension (see the header comment above). may_fill counts a suspended site down and returns
 * whether this generic execution may fill; fill_failed records a fill that could not describe the
 * receiver. */
static inline bool embi_js_ic_may_fill(embi_js_ic *ic)
{
    if (ic->skip) {
        ic->skip--;
        return false;
    }
    if (ic->state == EMBI_JS_IC_MEGA) {
        ic->state = EMBI_JS_IC_UNINIT;
    }
    return true;
}

static inline void embi_js_ic_fill_failed(embi_js_ic *ic)
{
    if (++ic->fails >= EMBI_JS_IC_FAIL_LIMIT) {
        ic->fails = 0;
        ic->skip = EMBI_JS_IC_RETRY;
    }
}

/* Whether o may be read and written through a cached global entry without its hooks: an IC-capable
 * class with no named get_own, get, has, set or define_own hook. Global sites (interp.c) use it for
 * the global object, so an embedder's exotic global is never cached. */
bool embi_js_ic_class_plain(const embi_js_object *o);

/* Test hook (builtins_proxy.c): the handler shape the trap caches of proxy p were read under (nullptr
 * when none) and the slot cached for trap id (-1 when none). Returns false when p is not a proxy. */
bool embi_js_proxy_trap_entry(const embi_js_object *p, uint32_t id, const embi_js_shape **shape,
                              int32_t *slot);

/* A site table of n sites (nullptr when n is zero). Keys are set by embi_js_ic_init. */
embi_js_ic *embi_js_ic_alloc(uint32_t n);
void embi_js_ic_init(embi_js_runtime *rt, embi_js_ic *ic, embi_js_key key);
/* Releases the entries' side records, then the table. */
void embi_js_ic_free(embi_js_ic *ics, uint32_t n);
/* Marks keys, receiver shapes and side-record shapes; counts the tables as external. owner is the
 * code cell that owns the table. */
void embi_js_ic_trace(embi_js_ic *ics, uint32_t n, embi_js_tracer *t);

/* The runtime's stub cache (J5m1, rt->stub_cache). Clear empties it (the collector calls it before
 * marking, because the entries hold unmarked shapes). Free releases it at runtime teardown. */
void embi_js_stub_cache_clear(embi_js_runtime *rt);
void embi_js_stub_cache_free(embi_js_runtime *rt);
/* Test hook: the number of populated entries (0 when the table was never allocated). */
uint32_t embi_js_stub_cache_count(embi_js_runtime *rt);
/* J5n1: entries recorded since the runtime started (a miss that could be described records one). The
 * tests use it to tell stub hits from refills. */
uint64_t embi_js_stub_cache_fills(embi_js_runtime *rt);

/* Property read at a GET_FIELD, GET_FIELD_THIS or SUPER_GET site. obj is the object the lookup
 * starts at (the receiver, or the super base), receiver is the this value for getters. Returns the
 * value or EMBI_JS_EXCEPTION. owner is the code cell (the write barrier target). */
embi_js_value embi_js_ic_get(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                             embi_js_object *obj, embi_js_value receiver);
/* [[Set]] at a SET_FIELD site with an object base, as embi_js_put does it: returns EMBI_JS_YES,
 * EMBI_JS_NO, or EMBI_JS_THREW (a pending exception, or strict-mode failure). */
int embi_js_ic_put(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o,
                   embi_js_value v, bool strict);
/* CreateDataPropertyOrThrow at a DEFINE_FIELD site: returns EMBI_JS_YES or EMBI_JS_THREW. */
int embi_js_ic_define(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, embi_js_object *o,
                      embi_js_value v);

/* Primitive receiver (string, number, boolean, symbol, bigint) at a GET_FIELD or GET_FIELD_THIS site
 * other than string length: the method is looked up on the intrinsic prototype through the site's
 * entries, with the primitive as the receiver of any getter. No wrapper is allocated on a hit. base
 * must not be an object or nullish. owner is the code cell. */
embi_js_value embi_js_ic_get_prim(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                                  embi_js_value base);
