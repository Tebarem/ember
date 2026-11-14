#pragma once
/*
 * Shapes (hidden classes) for named properties.
 *
 * A shape node records one property (key, attrs) and points at its parent, so the parent chain
 * lists the object's properties in insertion order. Slot i of an object holds the value of the
 * property whose node has count == i + 1. The prototype is copied into every node of a chain, so
 * an object's shape alone answers "which prototype, which slot".
 *
 * Root shapes (count 0) are cached per prototype (see object.c). A dictionary shape
 * (EMBI_JS_SHAPE_DICT) belongs to exactly one dictionary-mode object and only carries its
 * prototype. Transitions are strong references, so a live root keeps its whole tree alive.
 *
 * Transitions. The first child is stored inline in `child`, which covers the common chain of
 * single-child nodes. The second child allocates `children`, a hash from key to the list of
 * children with that key (linked through next_same_key, since one key can carry several
 * attribute sets). The key lookup `table` is built lazily for chains longer than TABLE_MIN, by
 * shape_find, which has no runtime. Both hash buffers are heap storage outside the cell. Growth
 * of `children` is reported through embi_js_heap_note_external. The collector recounts both
 * buffers in the trace function, so a table built between collections is only counted at the
 * next one. Tables are bounded by the chain length (at most 64 entries per chain).
 */
#include "base/base.h"
#include "js/heap.h"
#include "js/runtime.h"
#include "js/value.h"

/* Property attributes. ACCESSOR marks a slot whose value is an embi_js_accessor cell. */
#define EMBI_JS_ATTR_WRITABLE 0x01u
#define EMBI_JS_ATTR_ENUMERABLE 0x02u
#define EMBI_JS_ATTR_CONFIGURABLE 0x04u
#define EMBI_JS_ATTR_ACCESSOR 0x08u
/* J5n2, shape nodes only (never in a descriptor or a dictionary entry): the property's value does not
 * exist yet. Today only the `prototype` of an ordinary function (object.h, EMBI_JS_OBJ_LAZY_PROTO).
 * It gives such an object a shape of its own, so an inline cache never mistakes it for one that has
 * the value. */
#define EMBI_JS_ATTR_LAZY 0x10u
#define EMBI_JS_ATTR_DEFAULT (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE)
/* Built-in methods and constructors: writable and configurable, not enumerable. */
#define EMBI_JS_ATTR_HIDDEN (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

#define EMBI_JS_SHAPE_DICT 0x0001u
#define EMBI_JS_SHAPE_MAX_CHAIN 64u   /* more properties than this switch the object to dictionary mode */
#define EMBI_JS_SHAPE_TABLE_MIN 8u    /* chains longer than this get a key lookup table */

typedef struct embi_js_shape {
    embi_js_cell hdr;                    /* flags: EMBI_JS_SHAPE_DICT for dictionary shapes */
    embi_js_value proto;                 /* object, or EMBI_JS_NULL */
    struct embi_js_shape *parent;        /* nullptr for roots */
    struct embi_js_shape *next_same_key; /* next child of the same parent with the same key */
    embi_js_key key;                     /* property added by this node (unused for roots) */
    struct embi_js_shape *child;         /* first child, stored inline */
    embi_hashmap *children;              /* key -> first child with that key; nullptr until the second child */
    embi_hashmap *table;                 /* key -> node; built lazily when count > TABLE_MIN */
    uint32_t count;                      /* properties in this chain */
    uint8_t attrs;                       /* attributes of this node's property */
    /* J5m2: the for-in key cache of an object with this shape (interp.c, for_in_init), or EMPTY.
     * It is a varray: [n, shape_1 .. shape_n, key ...] with the chain's shapes and the keys. */
    embi_js_value enum_cache;
} embi_js_shape;

typedef EMBI_VEC(embi_js_key) embi_js_key_vec;

embi_js_shape *embi_js_shape_new_root(embi_js_runtime *rt, embi_js_value proto);
embi_js_shape *embi_js_shape_new_dict(embi_js_runtime *rt, embi_js_value proto);
/* Returns the child of s that adds (k, attrs), creating it when needed. s must not be a
 * dictionary shape and must have fewer than EMBI_JS_SHAPE_MAX_CHAIN properties. */
embi_js_shape *embi_js_shape_add(embi_js_runtime *rt, embi_js_shape *s, embi_js_key k,
                                 uint8_t attrs);
/* Returns the node that added k in s's chain, or nullptr. */
embi_js_shape *embi_js_shape_find(embi_js_shape *s, embi_js_key k);
/* Appends the chain's keys in insertion order. */
void embi_js_shape_keys_append(const embi_js_shape *s, embi_js_key_vec *out);

void embi_js_shape_ops_init(embi_js_runtime *rt);
