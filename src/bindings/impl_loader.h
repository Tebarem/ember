#pragma once
/*
 * Shared declarations of the loader's web API bindings (W7c): Blob and File, Headers, Request, Response, fetch,
 * ReadableStream, FormData, Storage. Private to the bindings module. The embedder-facing part is loader_host.h.
 *
 * Conventions. A body or a chunk is a Uint8Array of the realm. Promises are made with embi_js_promise_new and settled
 * from C. Values a function keeps across calls are held in persistent handles (embi_js_persistent_new) or in a traced
 * slot of a wrapper (the `ref` array of a record); locals on the C stack are scanned by the collector.
 */
#include "bindings/impl.h"
#include "bindings/loader_host.h"
#include "js/promise.h"
#include "js/typedarray.h"
#include "loader/blob.h"
#include "loader/fetch.h"
#include "loader/loader.h"
#include "loader/msgport.h"
#include "loader/storage.h"
#include "loader/stream.h"

#include <string.h>

typedef struct ldr_state ldr_state;
typedef struct ldr_job ldr_job;

/* ------------------------------------------------------------------ state (impl_loader_state.c) */

/* The loader state of a context, made on first use (never nullptr). */
ldr_state *ldr_state_of(embi_bind_ctx *c);
const embi_loader_env *ldr_env(embi_bind_ctx *c);
embi_blob_urls *ldr_blobs(embi_bind_ctx *c);
/* The storage backend of the context (the environment's, else a memory-only one of the context's own). */
embi_storage *ldr_storage(embi_bind_ctx *c);
/* The sessionStorage area of the context (memory only, made on first use). */
embi_storage_area *ldr_session(embi_bind_ctx *c);
/* The random source of this thread. */
embi_rng *ldr_rng(void);
/* The jobs list of the fetches in flight (impl_loader_fetch.c owns the elements). */
ldr_job **ldr_jobs_head(ldr_state *st);
uint32_t ldr_next_job_id(ldr_state *st);
/* Aborts every fetch in flight of the state (impl_loader_fetch.c). */
void ldr_jobs_abort_all(ldr_state *st);
/* Runs once per context before the first loader object is made: installs what idlgen cannot (the async iterator of
 * ReadableStream). */
void ldr_prepare(embi_bind_ctx *c);
/* Registers the Blob and File hooks of the structured serializer (impl_loader_clone.c). Idempotent. */
void ldr_clone_install(void);
/* Calls fn for every other live context that shares the storage backend and can be posted to (under the registry lock). */
void ldr_for_peers(embi_bind_ctx *c,
                   void (*fn)(embi_bind_ctx *peer, embi_js_realm *realm, const embi_loader_env *env, void *arg),
                   void *arg);
/* True while the context (with this realm) is still registered. */
bool ldr_ctx_live(embi_bind_ctx *c, embi_js_realm *realm);
/* Delivers a storage event (for a change of the local area) to the other contexts that share the backend; a receiver drops it unless its own origin maps to that area (impl_loader_storage.c). A null key is a clear. */
void ldr_storage_changed(embi_bind_ctx *c, embi_storage_area *area, const embi_u16view *key, const embi_u16view *old_v,
                         const embi_u16view *new_v);
/* Frees what impl_loader_storage.c keeps per state. */
void ldr_storage_release(ldr_state *st);

/* W7f. The persistent handle (plus one; 0: not loaded yet) of the exports of the JS-written streams (impl_loader_prelude.c). */
uint32_t *ldr_prelude_slot(embi_bind_ctx *c);
/* The message port registry of the context, made on first use (impl_msgport.c). */
embi_mports *ldr_ports(embi_bind_ctx *c);
/* N1c. The registry of the context's live WebSocket and EventSource objects (impl_websocket.c), a slot in the state. */
void **ldr_net_registry(embi_bind_ctx *c);
/* Drops their connections and the handles that keep them alive; late events find nothing. Called by embi_loader_detach. */
void embi_impl_net_release(embi_bind_ctx *c, void *registry);
/* Releases the ports of a state: their wrappers' persistent handles and the registry (impl_msgport.c). */
void ldr_msg_release(embi_bind_ctx *c, embi_mports *ports);
/* Frees what the state keeps for the streams prelude. */
void ldr_prelude_release(embi_bind_ctx *c, uint32_t slot);

/* ------------------------------------------------------------------ JS helpers (impl_loader_state.c) */

/* A Uint8Array of the realm holding a copy of the bytes (EMBI_JS_EXCEPTION on failure). */
embi_js_value ldr_new_uint8(embi_bind_ctx *c, const uint8_t *p, size_t n);
/* An ArrayBuffer holding a copy of the bytes. */
embi_js_value ldr_new_arraybuffer(embi_bind_ctx *c, const uint8_t *p, size_t n);
/* The bytes of a BufferSource value (ArrayBuffer, typed array, DataView). Returns false when v is not one. detached is set
 * when it is an ArrayBuffer or a view over one that is detached (the bytes are then empty). */
bool ldr_buffer_source(embi_js_value v, const uint8_t **p, size_t *n, bool *detached);
/* { value, done } iterator result. */
embi_js_value ldr_iter_result(embi_bind_ctx *c, embi_js_value value, bool done);
/* A new function object calling fn with the given magic. */
embi_js_value ldr_native(embi_bind_ctx *c, const char *name, int length, embi_js_native fn, int magic);
/* A rejected / resolved promise, not pinned (the caller holds it). */
embi_js_value ldr_promise_reject(embi_bind_ctx *c, embi_js_value reason);
embi_js_value ldr_promise_resolve(embi_bind_ctx *c, embi_js_value v);
/* A promise rejected with the pending exception (which is cleared). */
embi_js_value ldr_promise_reject_pending(embi_bind_ctx *c);
/* A promise rejected with a TypeError made from the message. */
embi_js_value ldr_promise_type_error(embi_bind_ctx *c, const char *msg);
/* A TypeError object (not thrown). */
embi_js_value ldr_make_type_error(embi_bind_ctx *c, const char *msg);
/* The text of a JS value as UTF-8 (ToString), appended to out. False when ToString threw. */
bool ldr_to_utf8(embi_bind_ctx *c, embi_js_value v, embi_sb *out);
/* Property read by name; undefined for a missing one and for a non-object. A throwing getter leaves the exception
 * pending and returns EMBI_JS_EXCEPTION. */
embi_js_value ldr_get(embi_bind_ctx *c, embi_js_value obj, const char *name);
/* The key of a well-known symbol. */
embi_js_key ldr_wk_key(embi_bind_ctx *c, enum embi_js_wk which);

/* UTF-8 decode of bytes (BOM stripped, invalid sequences replaced), appended to out. */
void ldr_utf8_decode(const uint8_t *p, size_t n, embi_sb *out);
/* Appends the bytes of every part of a BlobPart sequence (strings, buffers, Blobs). False with an exception pending. */
bool ldr_blob_parts(embi_bind_ctx *c, embi_js_value seq, const char *what, embi_sb *out);

/* ------------------------------------------------------------------ Blob and File (impl_loader_blob.c) */

/* The bytes and media type of a Blob or File wrapper. Returns false when v is neither. */
bool ldr_blob_view(embi_js_value v, const uint8_t **p, size_t *n, embi_str *type);
/* A new Blob wrapper holding a copy of the bytes. */
embi_js_value ldr_blob_new(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str type);
/* A new File wrapper. */
embi_js_value ldr_file_new(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str type, embi_str name,
                           double last_modified);
/* True when v is a File wrapper; its name (UTF-8) and last-modified time. */
bool ldr_file_info(embi_js_value v, embi_str *name, double *last_modified);

/* ------------------------------------------------------------------ Headers (impl_loader_headers.c) */

/* Copies the headers of a Headers wrapper into out (the caller frees with embi_headers_free). False when v is not one. */
bool ldr_headers_load(embi_js_value v, embi_headers *out);
/* Replaces the headers of a Headers wrapper with h (its guard is kept; a copy is stored). */
void ldr_headers_store(embi_js_value v, const embi_headers *h);
/* A Headers wrapper that owns a copy of src. */
embi_js_value ldr_headers_new(embi_bind_ctx *c, const embi_headers *src, embi_guard guard);
/* Fills h from a HeadersInit value (undefined: nothing). False with an exception pending. */
bool ldr_headers_fill(embi_bind_ctx *c, embi_headers *h, embi_js_value init, const char *what);
/* Copies src into dst (dst zeroed or empty; the guard is dst's own). */
void ldr_headers_copy(const embi_headers *src, embi_headers *dst);

/* ------------------------------------------------------------------ streams (impl_loader_stream.c) */

/* A ReadableStream wrapper holding one chunk of bytes (a Uint8Array copy) and closed after it. */
embi_js_value ldr_stream_from_bytes(embi_bind_ctx *c, const uint8_t *p, size_t n);
/* The stream core of a ReadableStream wrapper (nullptr when v is not one). */
embi_stream *ldr_stream_core(embi_js_value v);
/* The state name ("readable", "closed", "errored") of a ReadableStream wrapper, nullptr for other values, and its stored error. */
const char *ldr_stream_state_name(embi_js_value v);
embi_js_value ldr_stream_stored_error(embi_bind_ctx *c, embi_js_value v);
/* Marks a stream consumed in one go (body consumption): disturbed and closed. */
void ldr_stream_consume(embi_js_value v);
/* A readable stream fed from C (N1b: the body of a network response): made empty, then chunks are pushed, and it is
 * closed or errored once. The chunks are copies. */
embi_js_value ldr_stream_push_new(embi_bind_ctx *c);
/* Returns the bytes queued (0 when delivered at once). */
size_t ldr_stream_push_chunk(embi_bind_ctx *c, embi_js_value stream, const uint8_t *p, size_t n);
/* Hooks of a push stream: on_read(bytes) as a queued chunk is read, on_cancel when the reader cancels (nullptr: none). */
void ldr_stream_set_hooks(embi_js_value stream, void (*on_read)(void *user, size_t bytes), void (*on_cancel)(void *user),
                          void *user);
void ldr_stream_push_close(embi_bind_ctx *c, embi_js_value stream);
void ldr_stream_push_error(embi_bind_ctx *c, embi_js_value stream, embi_js_value reason);
/* Attaches a streamed body to a Response wrapper made with a null body (ldr_response_wrap, body.present false). */
void ldr_response_attach_stream(embi_bind_ctx *c, embi_js_value response, embi_js_value stream);
/* Reads the whole stream: returns a promise of a Uint8Array with the concatenated chunks, rejected with a TypeError when
 * a chunk is not a Uint8Array. The stream is locked for the read. */
embi_js_value ldr_stream_read_all(embi_bind_ctx *c, embi_js_value stream);

/* ------------------------------------------------------------------ the JS-written streams (impl_loader_prelude.c) */

/* A property of the prelude's exports (WritableStream, pipeTo, transferReadable ...), evaluating the source first. */
embi_js_value ldr_prelude_get(embi_bind_ctx *c, const char *name);
/* Calls an exported function with no receiver. */
embi_js_value ldr_prelude_call(embi_bind_ctx *c, const char *name, int argc, const embi_js_value *argv);
/* XMLHttpRequest and ProgressEvent (impl_xhr.c): the slot of the source's exports (plus one), the lazy global accessors, and the
 * release of the exports at the state's end. */
uint32_t *ldr_xhr_slot(embi_bind_ctx *c);
uint32_t *ldr_xhr_host_slot(embi_bind_ctx *c);
void ldr_xhr_install_lazy(embi_bind_ctx *c);
void ldr_xhr_release(embi_bind_ctx *c, uint32_t slot);
/* Defines the lazy global accessors (WritableStream ...). Called when the context is made (impl_window.c). */
void ldr_install_lazy_globals(embi_bind_ctx *c);

/* ------------------------------------------------------------------ messaging (impl_msgport.c) */

/* The transfer hooks of the structured serializer for MessagePort and ReadableStream (impl_loader_clone.c installs them). */
int ldr_xfer_prepare(embi_js_realm *r, embi_js_value v, embi_sb *payload);
void ldr_xfer_commit(embi_js_realm *r, embi_js_value v);
void ldr_xfer_rollback(embi_js_realm *r, embi_js_value v);
bool ldr_xfer_receive(embi_js_realm *r, embi_str payload, embi_js_value *out);
/* Replaces the pending plain DataCloneError of the serializer by the DOMException of that name (impl_loader_clone.c). */
void ldr_clone_rethrow_dom(embi_bind_ctx *c);

/* ------------------------------------------------------------------ bodies (impl_loader_body.c) */

/* Body consumption results. */
enum { LDR_BODY_TEXT, LDR_BODY_JSON, LDR_BODY_ARRAYBUFFER, LDR_BODY_BYTES, LDR_BODY_BLOB, LDR_BODY_FORMDATA };
/* The Response wrapper of a finished fetch (takes the response). */
embi_js_value ldr_response_wrap(embi_bind_ctx *c, embi_response *r);
/* Builds the embi_fetch_init of a Request wrapper for fetch (borrowed strings), and copies of the headers. The pieces stay
 * valid while the wrapper's record does. Returns false when v is not a Request. */
typedef struct ldr_request_view {
    embi_str method;
    embi_str url;
    embi_headers headers; /* a copy: free with embi_headers_free */
    bool has_body;
    const uint8_t *body;
    size_t body_len;
    embi_redirect redirect;
    embi_fetch_creds creds;
    embi_js_value signal; /* undefined when none */
    bool body_used;
    bool body_from_stream;
} ldr_request_view;
bool ldr_request_view_of(embi_js_value v, ldr_request_view *out);
/* Builds a Request wrapper from fetch's arguments (input: a string, URL or Request; init: undefined or a RequestInit). */
embi_js_value ldr_request_make(embi_bind_ctx *c, embi_js_value input, embi_js_value init);
/* Marks the body of a Request wrapper used (fetch consumed it). */
void ldr_request_mark_used(embi_js_value v);
/* The bytes of a body init value (string, URLSearchParams, Blob, BufferSource, FormData) and its default content type.
 * Returns 0 for no body, 1 for bytes (bytes in out, type appended to ctype), 2 for a ReadableStream (the value is the
 * stream), -1 with an exception pending. */
int ldr_body_init(embi_bind_ctx *c, embi_js_value v, embi_sb *bytes, embi_sb *ctype);

/* FormData parsing and serialization (impl_loader_form.c). */
/* Serializes a FormData wrapper as multipart/form-data; the boundary is generated. Appends the body to out and the
 * content type to ctype. False when v is not a FormData. */
bool ldr_formdata_multipart(embi_bind_ctx *c, embi_js_value v, embi_sb *out, embi_sb *ctype);
/* A FormData wrapper parsed from a urlencoded or multipart body (content_type is the Content-Type value). EMBI_JS_NULL
 * with no exception when the body does not parse (the caller rejects with a TypeError). */
embi_js_value ldr_formdata_parse(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str content_type);
bool ldr_is_formdata(embi_js_value v);

/* ------------------------------------------------------------------ fetch (impl_loader_fetch.c) */

/* The fetch of blob: URLs: a response from the registry. Returns false when the URL names no blob. */
bool ldr_blob_response(embi_bind_ctx *c, embi_str url, embi_response *out);
