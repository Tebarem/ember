#pragma once
/*
 * Shared by impl_websocket.c and impl_eventsource.c (N1c): the record behind a WebSocket or EventSource wrapper, the
 * link that lets events from the network find it (or find it gone), and the per-context registry that detaches every
 * live object when its document goes away. Private to the bindings module.
 *
 * Lifetime. The wrapper is kept alive by a persistent handle (pin) from construction until the connection is over
 * (close event, fatal error, close()). The network side holds a reference to the link, not to the record: events
 * (which run on the context's thread) look the record up through link->rec and drop themselves when it is nullptr, which
 * happens when the document detaches (embi_loader_detach) or the wrapper is finalized. The link is also released by the
 * network side's release callback, which may run on any thread; the link is freed with the last reference.
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"

#include <SDL3/SDL_atomic.h>

typedef struct nrec nrec;

typedef struct nlink {
    SDL_AtomicInt refs;
    nrec *rec; /* the context's thread only */
} nlink;

struct nrec {
    nrec *next, *prev; /* the registry of the context */
    embi_bind_ctx *c;
    embi_bind_obj *bo;
    nlink *link;
    void *h;           /* the handle of the connection (net_host.h), nullptr once dropped */
    void (*drop)(void *h); /* releases h (ws_free or sse_close) */
    nrec **slot;       /* the registry slot this record is listed in */
    uint32_t pin;      /* persistent handle plus one keeping the wrapper alive, 0 when none */
    uint8_t kind;      /* 1 WebSocket, 2 EventSource */
    uint8_t ready;
    bool dead;         /* detached from its context */
    bool registered;
    bool arraybuffer;  /* WebSocket.binaryType */
    bool creds;        /* EventSource.withCredentials */
    char *url;
    char *protocol;    /* WebSocket */
    char *origin;      /* the origin of url, for message events */
    uint64_t queued;   /* WebSocket: payload bytes passed to send() */
    uint64_t sent_final;
};

/* A link with two references: the record's and the network side's. */
nlink *embi_impl_net_link_new(nrec *r);
void embi_impl_net_link_unref(nlink *l);
/* Registers r with its context, and keeps its wrapper alive. */
void embi_impl_net_start(nrec *r);
/* Lets the wrapper go (the connection is over or was dropped). */
void embi_impl_net_unpin(nrec *r);
/* The finalizer of the wrapper's record. */
void embi_impl_net_rec_release(void *owned);
/* The page origin for the Origin header and same-origin credentials. */
char *embi_impl_net_page_origin(embi_bind_ctx *c);
/* Queues fn(arg) on the context's thread (the loader environment's post). False when there is no way to. */
bool embi_impl_net_post(embi_bind_ctx *c, void (*fn)(void *), void (*drop)(void *), void *arg);
/* Fires a plain trusted Event of type at the record's wrapper. */
void embi_impl_net_fire(nrec *r, const char *type);
/* Fires a MessageEvent with the data value at the wrapper. */
void embi_impl_net_fire_message(nrec *r, const char *type, embi_js_value data, const char *origin, const char *last_event_id);
/* The status text for the console: a warning line. */
void embi_impl_net_warn(nrec *r, const char *what, const char *message);
