#pragma once
/*
 * The bridge between the bindings of WebSocket and EventSource (src/bindings/net_host.h) and the network service
 * (src/net/ws.h, sse.h), N1c. Only built with EMBER_NET.
 */
#include "bindings/net_host.h"

#include <stdbool.h>

struct embi_net;

/* Where the events of a connection go and how the connection keeps that place alive: post queues fn(arg) on the thread of
 * the document (from any thread; false when it cannot, then drop(arg) is not called and the caller frees arg), retain and
 * unretain bracket the life of a connection (the poster outlives it). All three take user. */
typedef struct embi_netapi_host {
    struct embi_net *net;
    bool (*post)(void *user, void (*fn)(void *arg), void (*drop)(void *arg), void *arg);
    void (*retain)(void *user);
    void (*unretain)(void *user);
    void *user;
} embi_netapi_host;

/* Fills the hooks of a loader environment. host must stay valid while the environment is in use. */
void embi_netapi_fill(embi_netapi_env *out, const embi_netapi_host *host);
