#pragma once
/*
 * The environment of the loader's web APIs (W7c): what fetch, URL.createObjectURL and
 * localStorage need from the embedder. The embedding layer installs it in the binding context of a document's realm
 * (embi_loader_attach) and removes it before the context is freed (embi_loader_detach). A context with no environment
 * still has working Blob, Headers, Response, ReadableStream, a memory-only localStorage and its own blob: registry;
 * its fetch refuses everything except blob: and data: URLs with a network error.
 *
 * Threads: every call here comes from the context's own thread, except the posting hook (see post).
 */
#include "bindings/bind.h"
#include "bindings/net_host.h"
#include "loader/blob.h"
#include "loader/fetch.h"
#include "loader/storage.h"

typedef struct embi_loader_env {
    void *user; /* passed to the hooks below */
    /* The transport of fetch hops (start == nullptr: none). Its completions run on the context's thread. */
    embi_fetch_transport transport;
    /* The blob: registry shared by the contexts of a runtime (nullptr: the context's own). */
    embi_blob_urls *blobs;
    /* The storage backend shared by the contexts of a runtime (nullptr: a memory-only one of the context's own). Views
     * that share a backend hear about each other's localStorage changes through storage events. */
    embi_storage *storage;
    /* Queues fn(arg) on the context's thread; may be called from any thread. Returns false when it cannot (the caller then
     * frees arg itself). When the queued task is discarded without running (the view navigates or is destroyed), drop(arg)
     * is called instead of fn, on the context's thread, and must free arg. nullptr: storage events are not delivered. */
    bool (*post)(void *user, void (*fn)(void *arg), void (*drop)(void *arg), void *arg);
    /* Optional pair that keeps what user names alive while another thread calls post on it: ldr_for_peers calls retain
     * under its lock (so a concurrent embi_loader_detach cannot have released the environment yet), calls post after the
     * lock is dropped, then calls unretain. release must then free user only when the last reference is gone. Both
     * nullptr: the embedder guarantees user outlives any concurrent post. */
    void (*retain)(void *user);
    void (*unretain)(void *user);
    /* Called by embi_loader_detach after the context's operations were aborted: the embedder frees what user names. */
    void (*release)(void *user);
    /* N1c: WebSocket and EventSource (net_host.h). All hooks nullptr: no network, the objects fail as a network error does. */
    embi_netapi_env net;
} embi_loader_env;

/* Installs the environment (copied) in the context. Replaces any earlier one (its release hook runs). */
void embi_loader_attach(embi_bind_ctx *c, const embi_loader_env *env);
/* Aborts the context's pending fetches (their promises are left pending: the realm is going away), forgets the
 * context's sessionStorage area and runs the environment's release hook. Call it before embi_bind_free. */
void embi_loader_detach(embi_bind_ctx *c);
/* IF1: the context's MessagePorts live in shared (the registry of the view's frames, all on one thread), so a port can be
 * transferred to another frame and keep its peer. Takes a reference; no effect once the context has made ports. */
struct embi_mports;
void embi_loader_share_ports(embi_bind_ctx *c, struct embi_mports *shared);
