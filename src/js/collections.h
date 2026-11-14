#pragma once
/*
 * Map, Set, WeakMap and WeakSet (J2f): the intrinsics the collection
 * builtins install, and the fast-iteration hook that J2c's for-of and spread fast path uses.
 *
 * Fast path (for J2c). Iterating a Map or Set does not need the JS-visible protocol when the
 * protocol is the original one. The loop may take the fast path when both checks hold:
 *   (a) the receiver's @@iterator is the intrinsic: EMBI_JS_COLL_MAP_ENTRIES for a Map,
 *       EMBI_JS_COLL_SET_VALUES for a Set (read it with embi_js_coll_intrinsic and compare values);
 *   (b) the iterator's next is the intrinsic: EMBI_JS_COLL_MAP_ITER_NEXT for a Map iterator,
 *       EMBI_JS_COLL_SET_ITER_NEXT for a Set iterator (read it from the iterator prototype, which is
 *       embi_js_coll_intrinsic(r, EMBI_JS_COLL_MAP_ITER_PROTO) or ..._SET_ITER_PROTO).
 * Then the loop calls embi_js_coll_iter_new(rt, coll, kind) once and embi_js_coll_iter_step on each
 * pass. Step returns EMBI_JS_YES with the value that next would have produced (an entry array for
 * the entries kind, which is the only allocation), EMBI_JS_NO when the iteration is done, and
 * EMBI_JS_THREW only if building an entry array fails, which does not happen in practice. The
 * iterator object is a real one: it is registered with the collection, so the loop body may add or
 * delete entries, and the cursor stays correct. Step never runs user code, so no script can observe
 * that the protocol was skipped.
 */
#include "js/js.h"

#include <stdint.h>

/* The intrinsic slots that other code identifies. embi_js_coll_intrinsic returns the value. */
enum embi_js_coll_slot {
    EMBI_JS_COLL_MAP_PROTO,
    EMBI_JS_COLL_SET_PROTO,
    EMBI_JS_COLL_WEAKMAP_PROTO,
    EMBI_JS_COLL_WEAKSET_PROTO,
    EMBI_JS_COLL_MAP_ITER_PROTO,
    EMBI_JS_COLL_SET_ITER_PROTO,
    EMBI_JS_COLL_MAP_CTOR,
    EMBI_JS_COLL_SET_CTOR,
    EMBI_JS_COLL_MAP_ENTRIES,  /* Map.prototype.entries, also Map.prototype[@@iterator] */
    EMBI_JS_COLL_SET_VALUES,   /* Set.prototype.values, also keys and Set.prototype[@@iterator] */
    EMBI_JS_COLL_MAP_ITER_NEXT,
    EMBI_JS_COLL_SET_ITER_NEXT,
    EMBI_JS_COLL_MAP_SET,      /* Map.prototype.set (the adder of the Map constructor) */
    EMBI_JS_COLL_SET_ADD,      /* Set.prototype.add */
    EMBI_JS_COLL_WEAKMAP_SET,
    EMBI_JS_COLL_WEAKSET_ADD,
    EMBI_JS_COLL_COUNT
};

enum embi_js_coll_iter_kind {
    EMBI_JS_COLL_ITER_KEYS,
    EMBI_JS_COLL_ITER_VALUES,
    EMBI_JS_COLL_ITER_ENTRIES
};

/* The intrinsic value for one slot of the realm. */
embi_js_value embi_js_coll_intrinsic(embi_js_realm *r, enum embi_js_coll_slot which);

/* True for a Map (resp. Set) object. */
bool embi_js_coll_is_map(embi_js_value v);
bool embi_js_coll_is_set(embi_js_value v);

/* Live entries of a Map, Set, WeakMap or WeakSet object (0 for other objects). */
uint32_t embi_js_coll_size(const embi_js_object *o);

/* Creates an iterator over a Map or Set object (see the fast-path note above). Returns nullptr when
 * coll is neither. */
[[nodiscard]] embi_js_object *embi_js_coll_iter_new(embi_js_runtime *rt, embi_js_object *coll,
                                                    enum embi_js_coll_iter_kind kind);
/* Advances a Map or Set iterator created by embi_js_coll_iter_new. Returns EMBI_JS_YES with the
 * next value in *value, EMBI_JS_NO when done, or EMBI_JS_THREW with an exception pending. */
int embi_js_coll_iter_step(embi_js_runtime *rt, embi_js_object *iter, embi_js_value *value);
