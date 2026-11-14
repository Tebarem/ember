#pragma once
/*
 * WebSocket client (N1c, RFC 6455), on the connection code of the network service. Internal API: embi_ prefix.
 *
 * A WebSocket is a request of the service (an embi_net_req): the handle embi_ws_connect returns is the request's, so
 * embi_net_abort and embi_net_req_unref apply. The handshake, the frames and the close handshake run on the network
 * thread; the events are copied and queued through the request's post function (or run on the network thread when
 * there is none), in order: open, any number of messages, then (error and) close. The release callback runs exactly
 * once, last, and is where the caller frees its user data (it may run on the network thread).
 *
 * The URL is ws: or wss:, checked against the allowlist as http or https (same host and port rules). Cookies are sent
 * and stored as for a fetch with credentials include. A failure before open (blocked host, DNS, TCP, TLS, a refused
 * handshake) and a protocol failure after open raise on_error and then on_close(false, 1006, ""). A connection that
 * simply ends without the close handshake raises on_close(false, 1006, "") only. A clean close raises
 * on_close(true, code, reason) with 1005 for a close frame without a status code.
 *
 * embi_net_abort drops the connection with no close handshake and no further events but release.
 */
#include "net/net.h"

typedef struct embi_ws_callbacks {
    void (*on_open)(void *user, const char *protocol);                          /* "" when none was negotiated */
    void (*on_message)(void *user, bool binary, const uint8_t *data, size_t len); /* text is valid UTF-8 */
    void (*on_error)(void *user, const char *message);
    void (*on_close)(void *user, bool was_clean, uint16_t code, const char *reason);
    void (*release)(void *user);
} embi_ws_callbacks;

typedef struct embi_ws_request {
    const char *url;                    /* absolute ws: or wss: URL */
    const char *const *protocols;       /* subprotocols to offer (valid tokens), may be nullptr */
    uint32_t nprotocols;
    const char *origin;                 /* the Origin header value, or nullptr for none */
    embi_ws_callbacks cb;
    void *user;
    embi_net_post_fn post;              /* optional, like embi_net_request.post */
    void *post_user;
    uint32_t max_message;               /* largest incoming message in bytes (0: 32 MiB); larger fails with 1009 */
} embi_ws_request;

/* Starts connecting. The handle has one reference owned by the caller. Errors, a bad URL included, are events. */
embi_net_req *embi_ws_connect(embi_net *n, const embi_ws_request *q);
/* Queues a message (thread-safe, copies). False once close was requested. Messages go out in order. */
bool embi_ws_send(embi_net_req *r, bool binary, const void *data, size_t len);
/* Starts the closing handshake after the queued messages (thread-safe, idempotent). code 0: a close frame without a
 * status. The caller validates code (1000 or 3000-4999) and the reason (at most 123 bytes). Before the connection is
 * open this fails the connection. */
void embi_ws_close(embi_net_req *r, uint16_t code, const char *reason, size_t reason_len);
/* Payload bytes of messages handed to the socket so far (thread-safe): bufferedAmount = queued - sent. */
uint64_t embi_ws_sent(embi_net_req *r);
