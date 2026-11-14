#pragma once
/*
 * The network service of a runtime (N1a): HTTP/1.1 over TCP (SDL_net) and TLS (Mbed TLS), with a
 * keep-alive pool, redirects per the Fetch Standard, content decoding, a cookie jar and the host allowlist.
 * Internal API: embi_ prefix. The embedder's controls are in include/ember/ember_net.h.
 *
 * One SDL thread per service runs every socket. It is started by the first request and stopped by embi_net_destroy.
 * A request reports through callbacks (headers, body chunks, end or error, then release). They run on the network
 * thread, or, when the request has a post function, on the thread that function posts to (the same poster the
 * async v2 handlers use, src/embed/resource.c): every event is then copied and queued in order, so JS only ever sees
 * results on its own thread.
 *
 * Lifetime of a request: embi_net_start returns a handle with one reference owned by the caller (embi_net_req_unref).
 * The release callback runs exactly once, after the last event of the request (end, error or abort) was delivered or
 * dropped, and is where the caller frees its user data. embi_net_abort is thread-safe and never blocks: no callback
 * except release starts after it returns when the caller is the thread events are posted to (a post-mode request),
 * and at most one that is already running on the network thread otherwise.
 */
#include "base/base.h"
#include "loader/fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_net embi_net;
typedef struct embi_net_req embi_net_req;

/* The most connections (active and idle) to one (scheme, host, port). */
#define EMBI_NET_MAX_PER_ORIGIN 6
/* Redirects followed by redirect mode follow. */
#define EMBI_NET_MAX_REDIRECTS 20

typedef void (*embi_net_log_fn)(void *user, emb_log_level level, const char *message);

/* Creates the service (no thread yet). log may be nullptr (SDL_Log is then used). */
embi_net *embi_net_create(embi_net_log_fn log, void *log_user);
/* Stops the thread (started or not), aborts every request in flight (their release callbacks run, nothing else) and
 * frees the service. Handles the callers still hold stay valid until they unref them. */
void embi_net_destroy(embi_net *n);

/* ------------------------------------------------------------------ configuration (thread-safe) */

/* Allowlist entry; see ember_net.h for the patterns. False for a malformed pattern. */
bool embi_net_allow_add(embi_net *n, const char *pattern);
/* Whether host (as the URL parser serializes it) and port (the effective one) match an entry. */
bool embi_net_allowed(embi_net *n, embi_str host, int port);
/* PEM text (NUL not required) or one DER certificate. False when nothing parses. Applied to connections made later. */
bool embi_net_add_root(embi_net *n, const void *pem_or_der, size_t len);
void embi_net_use_bundled_roots(embi_net *n, bool on);
void embi_net_set_user_agent(embi_net *n, const char *ua);
/* Connect timeout (DNS, TCP and TLS handshake; default 30000) and idle timeout (no byte received while waiting for
 * the response head or body; default 60000), in milliseconds. 0 keeps the current value. */
void embi_net_set_timeouts(embi_net *n, uint32_t connect_ms, uint32_t idle_ms);

/* ------------------------------------------------------------------ requests */

typedef enum embi_net_redirect {
    EMBI_NET_REDIRECT_FOLLOW,
    EMBI_NET_REDIRECT_ERROR,  /* a redirect response is a network error */
    EMBI_NET_REDIRECT_MANUAL, /* a redirect response is delivered as it is */
} embi_net_redirect;

/* Which requests carry the jar's cookies and store its Set-Cookie headers. */
typedef enum embi_net_creds {
    EMBI_NET_CREDS_OMIT,
    EMBI_NET_CREDS_SAME_ORIGIN, /* only when the initiator origin equals the request's origin */
    EMBI_NET_CREDS_INCLUDE,
} embi_net_creds;

typedef enum embi_net_err {
    EMBI_NET_ERR_URL = 1,   /* not an absolute http or https URL, or a bad method or header */
    EMBI_NET_ERR_BLOCKED,   /* the host is not on the allowlist */
    EMBI_NET_ERR_DNS,
    EMBI_NET_ERR_CONNECT,   /* the TCP connection failed or closed early */
    EMBI_NET_ERR_TLS,       /* handshake or certificate failure */
    EMBI_NET_ERR_PROTOCOL,  /* malformed HTTP, or a body shorter than announced */
    EMBI_NET_ERR_TIMEOUT,
    EMBI_NET_ERR_REDIRECT,  /* too many, a bad Location, or redirect mode error */
    EMBI_NET_ERR_DECODE,    /* a bad or unsupported Content-Encoding */
    EMBI_NET_ERR_IO,        /* a socket error in the middle of a response */
} embi_net_err;

/* The response head. All pointers are valid during the on_head call only. */
typedef struct embi_net_head {
    uint16_t status;
    const char *status_text;
    const embi_headers *headers; /* lowercase names; Set-Cookie is included (the jar already has the cookies) */
    const char *url;             /* after redirects */
    bool redirected;             /* at least one redirect was followed */
    uint32_t redirect_count;
    bool tls;
    int64_t content_length;      /* the Content-Length header when the body is not content-encoded, else -1 */
} embi_net_head;

typedef struct embi_net_callbacks {
    void (*on_head)(void *user, const embi_net_head *head);
    void (*on_data)(void *user, const uint8_t *data, size_t len); /* decoded bytes; data is valid during the call */
    void (*on_end)(void *user);
    void (*on_error)(void *user, embi_net_err err, const char *message);
    void (*release)(void *user);
} embi_net_callbacks;

/* Queues fn(arg) on the receiving thread. drop(arg) runs instead of fn when the queue is gone. True when queued. */
typedef bool (*embi_net_post_fn)(void *post_user, void (*fn)(void *arg), void (*drop)(void *arg), void *arg);

typedef struct embi_net_request {
    const char *method;                /* uppercase, normalized; nullptr is "GET" */
    const char *url;                   /* absolute */
    const embi_headers *headers;       /* optional; copied. Host, Content-Length, Cookie and Connection are ours */
    const void *body;                  /* optional; copied. Ignored for GET and HEAD */
    size_t body_len;
    embi_net_redirect redirect;
    embi_net_creds creds;
    const char *initiator_origin;      /* for EMBI_NET_CREDS_SAME_ORIGIN: "scheme://host[:port]"; nullptr never matches */
    uint32_t timeout_ms;               /* total time for the whole request including redirects; 0 for none */
    uint32_t delay_ms;                 /* wait this long before connecting (EventSource reconnection); 0 for none */
    bool stream;                       /* an endless body (EventSource): no idle timeout while the body arrives */
    embi_net_callbacks cb;
    void *user;
    embi_net_post_fn post;             /* optional */
    void *post_user;
} embi_net_request;

/* Starts a request (the thread is started when needed). Errors, bad URLs included, are delivered as an on_error
 * event, so there is one path. nullptr only for a null service or request. */
embi_net_req *embi_net_start(embi_net *n, const embi_net_request *r);
/* Cancels the request. Thread-safe, non-blocking, idempotent, and a no-op once the request is over. */
void embi_net_abort(embi_net_req *r);
/* Holds (on) or releases a post-mode request's socket reads: while held, the request reads no more body bytes (the
 * consumer is behind), as with the 4 MB backpressure. Thread-safe; the network thread notices within its tick. */
void embi_net_req_hold(embi_net_req *r, bool on);
void embi_net_req_unref(embi_net_req *r);

/* ------------------------------------------------------------------ cookies (documents) */

/* document.cookie getter for url: "a=1; b=2" without HttpOnly cookies, appended to out. */
void embi_net_cookie_get(embi_net *n, const char *url, embi_sb *out);
/* document.cookie setter: one Set-Cookie style string for url; HttpOnly cookies cannot be set from script. */
void embi_net_cookie_set(embi_net *n, const char *url, const char *cookie);

/* Number of live allocations of the net module (tests: zero after everything is released). */
size_t embi_net_live(void);
