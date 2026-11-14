#pragma once
/*
 * Structured serialization (W7b): the HTML standard's StructuredSerialize and StructuredDeserialize
 * over the JS engine's internal API. The byte format is versioned and backs structuredClone,
 * history.state and, later, postMessage.
 *
 * This header includes JS headers, so only JS-facing code includes it. storage.c and the other C
 * core files stay JS-free.
 *
 * Format: "EMCL", u8 version (EMBI_CLONE_VERSION), then one value. Each value starts with a tag
 * byte. Every object is registered in a memory table when it is first written, and a later
 * reference to it is a backreference by table index, which keeps cycles and shared references.
 */
#include "base/base.h"
#include "js/js.h"

#define EMBI_CLONE_VERSION 1u
/* Object nesting limit, counted the same way on both sides: each open object (plain, array, wrapper,
 * Date, RegExp, Error, ArrayBuffer, view, Map, Set) is one level, and primitives and backreferences
 * are not. The serializer refuses the object that would make level EMBI_CLONE_MAX_DEPTH + 1, and the
 * deserializer refuses the same stream, so every stream it writes can be read back. */
#define EMBI_CLONE_MAX_DEPTH 512u

/* Serializes v and appends the bytes to out. transfer lists ArrayBuffers to detach once the value
 * has been serialized (their bytes are copied into the output, and the sources are detached, which
 * is the observable effect of transfer). Returns false with a DataCloneError pending for a value
 * that cannot be cloned (a function, a symbol, a Proxy, a WeakMap and so on), or for a bad transfer
 * list. On failure out is restored to its length at entry. */
bool embi_clone_serialize(embi_js_realm *r, embi_js_value v, const embi_js_value *transfer,
                          uint32_t ntransfer, embi_sb *out);

/* Creates a new value in realm r from the bytes of one serialization. Returns false with an
 * exception pending when the bytes are malformed (an Error) or a constructor throws. */
bool embi_clone_deserialize(embi_js_realm *r, embi_str bytes, embi_js_value *out);

/* Like embi_clone_deserialize; transferred also receives a JS array (or undefined when nothing was transferred) of the
 * platform objects the host rebuilt from the transfer list, in transfer-list order (postMessage's event.ports). */
bool embi_clone_deserialize_ex(embi_js_realm *r, embi_str bytes, embi_js_value *out, embi_js_value *transferred);

/* Host objects (the bindings' Blob and File). serialize returns 1 after writing the object's payload (no nested
 * values), 0 when v is not a host object it knows (the clone then fails with a DataCloneError), and -1 with an exception
 * pending. deserialize rebuilds the object from a payload it wrote. Set once at startup by the bindings; the pointer
 * must stay valid. */
typedef struct embi_clone_host {
    int (*serialize)(embi_js_realm *r, embi_js_value v, embi_sb *payload);
    bool (*deserialize)(embi_js_realm *r, embi_str payload, embi_js_value *out);
    /* Transferable platform objects (W7f: MessagePort, ReadableStream). An entry of the transfer list that is not an
     * ArrayBuffer goes through these. Optional (nullptr: only ArrayBuffers can be transferred).
     *   transfer_prepare: v is in the transfer list. Returns 1 after writing the payload the receiving side rebuilds the
     *     object from (no visible effect yet), 0 when v is not transferable (a DataCloneError follows), -1 with an
     *     exception pending (for example a detached port).
     *   transfer_commit: the whole value serialized: take v away from the sender (detach a port, lock a stream).
     *   transfer_rollback: the serialization failed after transfer_prepare: undo it.
     *   transfer_receive: rebuilds the object from its payload in the receiving realm. */
    int (*transfer_prepare)(embi_js_realm *r, embi_js_value v, embi_sb *payload);
    void (*transfer_commit)(embi_js_realm *r, embi_js_value v);
    void (*transfer_rollback)(embi_js_realm *r, embi_js_value v);
    bool (*transfer_receive)(embi_js_realm *r, embi_str payload, embi_js_value *out);
} embi_clone_host;
void embi_clone_set_host(const embi_clone_host *h);
