#pragma once
/*
 * Internal types shared by the files of module net. Not for other modules: they use net.h.
 */
#include "net/net.h"

#include "codec/codec.h"
#include "loader/loader.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <SDL3_net/SDL_net.h>

#include <string.h>

/* ------------------------------------------------------------------ memory (counted: embi_net_live) */

/* Zeroed allocation with a size header, so frees need no size. Failure is fatal (the base allocator's rule). */
[[nodiscard]] void *embi_net_zalloc(size_t n);
void embi_net_mfree(void *p);
[[nodiscard]] void *embi_net_realloc(void *p, size_t n);
[[nodiscard]] char *embi_net_strdup(const char *s);
[[nodiscard]] char *embi_net_strndup(const char *s, size_t n);

/* ------------------------------------------------------------------ allowlist (allow.c) */

typedef struct embi_net_pattern {
    char *host;    /* normalized; nullptr for "*" */
    bool sub;      /* "*.host": subdomains only */
    int32_t port;  /* 0 for any */
} embi_net_pattern;

bool embi_net_pattern_parse(const char *s, embi_net_pattern *out);
bool embi_net_pattern_match(const embi_net_pattern *p, embi_str host, int port);
void embi_net_pattern_free(embi_net_pattern *p);

/* ------------------------------------------------------------------ cookies (cookie.c) */

typedef struct embi_cookie {
    char *name;
    char *value;
    char *domain;
    char *path;
    bool host_only;
    bool secure;
    bool http_only;
    uint8_t samesite; /* 0 unset, 1 None, 2 Lax, 3 Strict */
    int64_t expiry_ms; /* INT64_MAX for a session cookie */
    int64_t created_ms;
    uint64_t seq;
} embi_cookie;

typedef struct embi_cookiejar {
    embi_cookie *v;
    uint32_t n, cap;
    uint64_t seq;
} embi_cookiejar;

void embi_cookiejar_free(embi_cookiejar *j);
/* Stores the cookie of one Set-Cookie value received for url. from_script: document.cookie (HttpOnly refused).
 * Returns true when the jar changed. */
bool embi_cookiejar_set(embi_cookiejar *j, const embi_url *url, embi_str set_cookie, bool from_script, int64_t now_ms);
/* Appends the Cookie header value for a request to url: "a=1; b=2". include_http_only is false for document.cookie. */
void embi_cookiejar_header(embi_cookiejar *j, const embi_url *url, bool include_http_only, int64_t now_ms, embi_sb *out);
uint32_t embi_cookiejar_count(const embi_cookiejar *j);
/* RFC 6265 date parsing (5.1.1): milliseconds since the epoch, or false. */
bool embi_cookie_parse_date(embi_str s, int64_t *ms);

/* ------------------------------------------------------------------ TLS (tls.c) */

typedef struct embi_net_tlsctx embi_net_tlsctx; /* the client configuration: roots and options */
typedef struct embi_net_tls embi_net_tls;       /* one connection */

/* The one entropy entry point (platform.c), and the time hooks installed into Mbed TLS. */
bool embi_net_entropy(void *buf, size_t len);
void embi_net_platform_init(void);

/* Process-wide, idempotent: the threading layer, time and PSA. */
bool embi_net_tls_global_init(void);

/* The roots a service trusts: pending additions are applied by tls_ctx_get on the network thread. */
typedef struct embi_net_roots {
    SDL_Mutex *m;
    bool bundled;
    uint32_t version;      /* bumped on every change */
    struct embi_net_root_blob *blobs;
    uint32_t nblobs;
} embi_net_roots;

void embi_net_roots_init(embi_net_roots *r);
void embi_net_roots_free(embi_net_roots *r);
bool embi_net_roots_add(embi_net_roots *r, const void *data, size_t len);
void embi_net_roots_use_bundled(embi_net_roots *r, bool on);
/* Parses the bundled roots into a scratch chain and returns how many certificates it holds; *failed gets the number that did not
 * parse (or a negative Mbed TLS code). For tests. */
size_t embi_net_bundled_roots_check(int *failed);

/* Returns the (refcounted) client configuration for the current roots, building it when they changed. Network thread
 * only. nullptr on failure (message in err). */
embi_net_tlsctx *embi_net_tlsctx_get(embi_net_roots *r, embi_net_tlsctx *current, char *err, size_t err_cap);
void embi_net_tlsctx_unref(embi_net_tlsctx *c);

/* Raw transport of a TLS connection. recv: bytes, 0 for none yet, -1 for end or error. send: false on error. */
typedef struct embi_net_io {
    int (*recv)(void *user, void *buf, int len);
    bool (*send)(void *user, const void *buf, int len);
    void *user;
} embi_net_io;

typedef enum embi_net_tls_rc {
    EMBI_TLS_OK,
    EMBI_TLS_WANT_IO, /* retry when the socket moves */
    EMBI_TLS_CLOSED,  /* close_notify received: a clean end */
    EMBI_TLS_CLOSED_UNCLEAN, /* the transport ended without close_notify: a truncation cannot be ruled out */
    EMBI_TLS_ERROR,
} embi_net_tls_rc;

/* host: the name to verify and to send as SNI. */
embi_net_tls *embi_net_tls_new(embi_net_tlsctx *ctx, const char *host, embi_net_io io, char *err, size_t err_cap);
void embi_net_tls_free(embi_net_tls *t);
embi_net_tls_rc embi_net_tls_handshake(embi_net_tls *t, char *err, size_t err_cap);
embi_net_tls_rc embi_net_tls_read(embi_net_tls *t, void *buf, size_t cap, size_t *got, char *err, size_t err_cap);
/* Writes up to len bytes; *written is how many were taken. On WANT_IO call again with the same arguments. */
embi_net_tls_rc embi_net_tls_write(embi_net_tls *t, const void *buf, size_t len, size_t *written, char *err, size_t err_cap);
void embi_net_tls_close_notify(embi_net_tls *t);

/* ------------------------------------------------------------------ HTTP/1.1 (http.c) */

typedef struct embi_http_head {
    int minor;                /* HTTP/1.minor */
    uint16_t status;
    char *reason;
    embi_headers headers;     /* lowercase names, trimmed values */
} embi_http_head;

void embi_http_head_free(embi_http_head *h);

typedef enum embi_http_parse {
    EMBI_HTTP_NEED_MORE,
    EMBI_HTTP_DONE,
    EMBI_HTTP_BAD,
} embi_http_parse;

/* Parses a response head from buf (all of it so far). On DONE, *used is the head's length (through the blank line).
 * Obsolete line folding, bare LF line ends are rejected, the head is limited to 64 KiB. */
embi_http_parse embi_http_parse_head(const uint8_t *buf, size_t len, size_t *used, embi_http_head *out);

typedef enum embi_body_kind {
    EMBI_BODY_NONE,
    EMBI_BODY_LENGTH,
    EMBI_BODY_CHUNKED,
    EMBI_BODY_CLOSE,
} embi_body_kind;

/* The framing of a response (RFC 9112 6.3). false: invalid (conflicting Content-Length). */
bool embi_http_body_kind(const embi_http_head *h, bool head_request, embi_body_kind *kind, uint64_t *length);

/* Incremental body framing: strips chunk framing or counts a length. */
typedef struct embi_http_body {
    embi_body_kind kind;
    uint64_t remaining;       /* LENGTH: bytes left; CHUNKED: bytes left in the chunk */
    int state;                /* chunked state machine */
    uint64_t size_acc;
    uint32_t trailer_bytes;
    bool done;
    bool bad;
} embi_http_body;

void embi_http_body_init(embi_http_body *b, embi_body_kind kind, uint64_t length);
/* Consumes bytes from in; the payload is appended to out. Returns how many bytes of in were consumed; stops at the end
 * of the body (done) or an error (bad). */
size_t embi_http_body_feed(embi_http_body *b, const uint8_t *in, size_t len, embi_buf *out);

/* Whether a header value (comma list) has the token, ASCII case-insensitively. */
bool embi_http_has_token(embi_str value, embi_str token);

/* ------------------------------------------------------------------ content decoding (decode.c) */

typedef struct embi_net_decoder embi_net_decoder;

/* coding: the Content-Encoding value (trimmed, lowercase); NULL for identity. Fails for an unsupported coding. */
embi_net_decoder *embi_net_decoder_new(embi_str coding);
/* Feeds bytes; decoded bytes (at most about 1 MB per call; the rest waits, see _pending) are appended to out. */
bool embi_net_decoder_feed(embi_net_decoder *d, const uint8_t *in, size_t len, embi_buf *out);
/* The end of the body: false when the stream is incomplete or br fails. Appends the buffered output (br). */
/* Whether input is still waiting inside the decoder (or decoded Brotli output to hand out): call _pump. */
bool embi_net_decoder_pending(const embi_net_decoder *d);
/* Decodes the next step (at most about 1 MB) of the waiting input, appending to out. False on a decoding error. */
bool embi_net_decoder_pump(embi_net_decoder *d, embi_buf *out);
bool embi_net_decoder_finish(embi_net_decoder *d, embi_buf *out);
void embi_net_decoder_free(embi_net_decoder *d);
bool embi_net_decoder_active(const embi_net_decoder *d);
