#pragma once
/*
 * What WebSocket and EventSource need from the embedder (N1c): the network service behind a handful of hooks, so the
 * bindings module does not depend on module net (which EMBER_NET=OFF leaves out). The embedding layer fills
 * embi_loader_env.net (src/embed/netapi.c); a context whose hooks are nullptr gets objects that fail with the usual
 * error events (WebSocket: error then close with 1006; EventSource: error and CLOSED), as a network error would.
 *
 * Threads: the hooks are called on the context's thread. The callbacks run there too (the embedder posts them), except
 * release, which may run on any thread, exactly once and last: it may only free memory. A handle is released by
 * ws_free or sse_close (once), which also drops the connection.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_netapi_ws_cb {
    void (*on_open)(void *user, const char *protocol); /* "" when none was negotiated */
    void (*on_message)(void *user, bool binary, const uint8_t *data, size_t len);
    void (*on_error)(void *user, const char *message);
    void (*on_close)(void *user, bool was_clean, uint16_t code, const char *reason);
    void (*release)(void *user);
} embi_netapi_ws_cb;

typedef struct embi_netapi_sse_cb {
    void (*on_open)(void *user);
    void (*on_message)(void *user, const char *type, const char *data, size_t len, const char *last_event_id, const char *origin);
    void (*on_error)(void *user, bool reconnecting, const char *message);
    void (*release)(void *user);
} embi_netapi_sse_cb;

typedef struct embi_netapi_env {
    void *user;
    /* url: an absolute ws: or wss: URL. origin: the Origin header value (the page origin), or nullptr. */
    void *(*ws_open)(void *user, const char *url, const char *const *protocols, uint32_t nprotocols, const char *origin,
                     const embi_netapi_ws_cb *cb, void *cb_user);
    bool (*ws_send)(void *handle, bool binary, const void *data, size_t len);
    /* code 0: no status code. */
    void (*ws_close)(void *handle, uint16_t code, const char *reason, size_t reason_len);
    /* Payload bytes of messages that reached the socket (bufferedAmount = queued - sent). */
    uint64_t (*ws_sent)(void *handle);
    /* Drops the connection without a closing handshake and releases the handle. */
    void (*ws_free)(void *handle);
    /* url: an absolute http or https URL. initiator: the page origin (for same-origin credentials). */
    void *(*sse_open)(void *user, const char *url, bool with_credentials, const char *initiator, const embi_netapi_sse_cb *cb,
                      void *cb_user);
    void (*sse_close)(void *handle);
} embi_netapi_env;
