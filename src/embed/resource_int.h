#pragma once
#ifndef EMBER_CANVAS
#error "EMBER_CANVAS must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * Resource handlers v2 (W7c): the loader's transport over the runtime's handlers, the runtime-wide state the web APIs
 * share (storage backend, blob: URL registry), and what the embedding layer's script module installs in a binding
 * context. Internal API: embi_ prefix. The public API is include/ember/ember_resource.h. Implemented in resource.c.
 */
#include <ember/ember.h>

#include "loader/fetch.h"

typedef struct embi_bind_ctx embi_bind_ctx;
typedef struct embi_rt_binding embi_rt_binding;

/* ------------------------------------------------------------------ shared with runtime.c */

/* The v1 function a v2 registration puts in the handler table (its user is the registration record), and the free
 * of that record. runtime.c uses them to recognize and release a v2 slot. */
bool embi_v2_tramp(void *user, const char *url, size_t url_len, emb_resource *out);
void embi_v2_free(void *record);
/* Sets or removes (fn == nullptr) the handler of a scheme in the table (runtime.c). */
bool embi_rt_handler_set(emb_runtime *rt, const char *scheme, emb_resource_fn fn, void *user);
/* Reads the file of a file: URL or bare path: 0 refused, 1 read, 2 allowed but unreadable. SDL_malloc'd bytes. */
int embi_rt_file_load(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len);
/* Loads an absolute URL synchronously through the runtime's transport (the handlers, file:, data:, blob:, the mounts):
 * the bytes are SDL_malloc'd into data and len, and the response's Content-Type is appended to mime (empty when the
 * load gives none). False for a refused load, a status outside 200 to 299, or one a handler completes later (a document
 * load does not wait). Used by the module host (src/loader/modules.c) through the script layer. */
bool embi_rt_load(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len, embi_sb *mime);
/* Resolves a blob: URL against the runtime's registry (the fragment is ignored). On success true, with the bytes copied
 * into an SDL_malloc'd buffer (never nullptr, even for an empty blob) and, when type is not nullptr, the blob's media
 * type appended to it. A URL never created, or revoked, is false. Thread: the registry is mutated by the page's script
 * (URL.createObjectURL/revokeObjectURL) on the view's thread without the runtime's lock, so this must be called on that
 * thread (the single-threaded view model: view loads and fetch hops run there); the lock only guards the table of
 * registries against runtime teardown and creation. */
bool embi_rt_blob_lookup(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len, embi_sb *type);
/* Whether url is an http or https URL that the network service serves: EMBER_NET is built in and no embedder handler is
 * registered for the scheme (a handler for http or https wins). */
bool embi_rt_net_claims(emb_runtime *rt, embi_str url);
/* Frees the runtime-wide web API state (flushes the storage backend). Called by emb_runtime_destroy. */
void embi_rt_extras_free(emb_runtime *rt);

/* ------------------------------------------------------------------ the transport */

/* The first member of every hop handle: which transport made it, so the transport's abort can tell its own handles
 * apart (resource.c hop_abort). */
typedef struct embi_hop_tag {
    int kind;
} embi_hop_tag;
enum {
    EMBI_HOP_REQUEST = 1, /* an emb_request (a resource handler) */
    EMBI_HOP_NET = 2,     /* a network request (net_fetch.c) */
};

/* The poster of a bound transport, as the network glue (net_fetch.c) holds and uses it (resource.c). post has the
 * embi_net_post_fn contract: it queues fn(arg) on the view's thread, or drop(arg) when the view is gone. */
void *embi_rt_poster_ref(void *poster);
void embi_rt_poster_unref(void *poster);
bool embi_rt_poster_post(void *poster, void (*fn)(void *arg), void (*drop)(void *arg), void *arg);
/* A network hop of an http or https URL, started on the service of rt and completed through poster (a bound transport
 * only). False when the build has no network or the service is missing, and the hop is then refused. The handle is what
 * embi_net_fetch_abort takes (net_fetch.c). */
bool embi_net_fetch_start(emb_runtime *rt, void *poster, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                          const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len);
void embi_net_fetch_abort(void *handle);
/* Pauses (on) or resumes the network request's reading of a hop (backpressure, net_fetch.c). */
void embi_net_fetch_hold(void *handle, bool on);

/* Fills t with a transport that serves the fetch hops of rt: the synchronous and asynchronous v2 handlers, the v1
 * handlers and the built-in file: and data: handlers. t->user is rt; rt must outlive the operations. A hop of a scheme
 * with no handler is refused (a network error). This transport has no view: an asynchronous completion runs on the
 * thread that calls emb_resource_complete, so it is for single-threaded users (tests). */
void embi_rt_fetch_transport(emb_runtime *rt, embi_fetch_transport *t);

/* A transport bound to a view: an asynchronous completion from any thread is posted to the view's event queue and
 * runs on the view's thread (emb_view_post_task). */
embi_rt_binding *embi_rt_binding_new(emb_runtime *rt, emb_view *view);
void embi_rt_binding_transport(embi_rt_binding *b, embi_fetch_transport *t);
/* Detaches the binding: completions that come later are dropped (never run) and the binding is freed. The caller has
 * aborted its operations first. */
void embi_rt_binding_free(embi_rt_binding *b);

/* ------------------------------------------------------------------ the web APIs of a document */

/* Installs the loader environment (fetch transport bound to view, blob: registry and storage of the runtime) in the
 * binding context of a document's realm. view may be nullptr (a document made without a view): the web APIs then work
 * without a transport. embi_rt_unbind_ctx must run before the context is freed: it aborts the context's fetches. */
void embi_rt_bind_ctx(embi_bind_ctx *c, emb_view *view);
void embi_rt_unbind_ctx(embi_bind_ctx *c);
/* R4b: the canvas environment of the view (the GPU manager, the text system and the repaint and image hooks). */
struct embi_canvas_env;
#if EMBER_CANVAS
void embi_view_canvas_env(emb_view *v, struct embi_canvas_env *e);
#endif
