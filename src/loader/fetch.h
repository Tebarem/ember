#pragma once
/*
 * Fetch core (W7c): the Fetch Standard without a network. Headers
 * (guards, validation, sorting and combining), the body and its used flag, the response record,
 * and the fetch operation: redirects (max 20), the redirect modes, and abort. The operation sends
 * each hop through a transport the embedder provides (runtime.c: the resource handlers v2).
 *
 * This header and fetch.c include no JS header: the JS-facing Headers, Request, Response and fetch
 * are in src/bindings/impl_loader_fetch.c. Internal API: embi_ prefix.
 *
 * Ownership: every struct with an owned buffer has a free function, and a zeroed struct is an
 * empty value. The strings a transport receives are borrowed for the call.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A name and value pair, borrowed. */
typedef struct embi_hdr_view {
    embi_str name;
    embi_str value;
} embi_hdr_view;

/* ------------------------------------------------------------------ Headers */

typedef enum embi_guard {
    EMBI_GUARD_NONE,
    EMBI_GUARD_REQUEST,         /* forbidden request names are ignored */
    EMBI_GUARD_REQUEST_NO_CORS, /* as request (no CORS in Ember, so the same list) */
    EMBI_GUARD_RESPONSE,        /* set-cookie and set-cookie2 are hidden */
    EMBI_GUARD_IMMUTABLE,       /* every change is a TypeError */
} embi_guard;

/* One header of a list. name is lowercase; value has no leading or trailing HTTP whitespace. */
typedef struct embi_kv {
    embi_sb name;
    embi_sb value;
} embi_kv;

typedef struct embi_headers {
    EMBI_VEC(embi_kv) list;
    embi_guard guard;
} embi_headers;

/* Result of a checked change. The JS binding throws a TypeError for the non-OK results. */
typedef enum embi_hdr_status {
    EMBI_HDR_OK,
    EMBI_HDR_INVALID, /* a name that is not a token, or a value with NUL, CR or LF */
    EMBI_HDR_IMMUTABLE,
} embi_hdr_status;

void embi_headers_free(embi_headers *h);
/* Token (RFC 9110 field-name) and value checks (Fetch 2.2). */
bool embi_header_name_ok(embi_str name);
bool embi_header_value_ok(embi_str value);
/* The value with leading and trailing HTTP whitespace (tab, LF, CR, space) removed. */
embi_str embi_header_trim(embi_str value);
/* The forbidden request and response names (Fetch 2.2.2 and 2.2.4), ASCII case-insensitive. */
bool embi_header_forbidden_request(embi_str name);
bool embi_header_forbidden_response(embi_str name);

/* Headers.append, set and delete with the guard rules (forbidden names are ignored, not an error,
 * under the request guards). */
embi_hdr_status embi_headers_append(embi_headers *h, embi_str name, embi_str value);
embi_hdr_status embi_headers_set(embi_headers *h, embi_str name, embi_str value);
embi_hdr_status embi_headers_delete(embi_headers *h, embi_str name);
/* Headers.has: true when a value exists for name. Headers.get: the values joined with ", " (the
 * Fetch rule for get, which joins set-cookie too). Appends to out. */
bool embi_headers_has(const embi_headers *h, embi_str name);
bool embi_headers_get(const embi_headers *h, embi_str name, embi_sb *out);
/* Converts a stored header value (UTF-8 of code points 0-255) to the wire bytes Fetch sends (one byte per code
 * point). Appends to out. */
void embi_header_value_to_wire(embi_str value, embi_sb *out);
/* The inverse, for a value a handler gives as bytes: each byte becomes the UTF-8 of its code point (0 to 255), the stored
 * form. Appends to out. */
void embi_header_value_from_wire(embi_str value, embi_sb *out);
/* The first value of name, borrowed, or {nullptr, 0} when there is none (for Location). */
embi_str embi_headers_first(const embi_headers *h, embi_str name);
/* Adds a header without the name and value checks or the guard (the transport's response and the
 * Response constructor's header copies use the checked calls; this one is for trusted sources). */
void embi_headers_add_raw(embi_headers *h, embi_str name, embi_str value);
/* The iteration order (Fetch 5.2 "sort and combine"): names sorted byte-wise, values of one name
 * combined with ", " except set-cookie, which stays one entry per value. Writes the flattened
 * list to out (zeroed, freed by the caller). */
bool embi_headers_sorted(const embi_headers *h, embi_headers *out);
/* A copy of the list without the names hidden by the response guard (basic response filter). */
bool embi_headers_copy_filtered(const embi_headers *src, embi_headers *dst, bool hide_set_cookie);

/* ------------------------------------------------------------------ methods */

/* Normalizes a request method (Fetch 2.1.7): the standard methods (DELETE, GET, HEAD, OPTIONS,
 * POST, PUT) are uppercased; a token is kept as given; CONNECT, TRACE and TRACK are forbidden.
 * Writes the method to out and returns false when it is not a token or is forbidden. */
bool embi_method_normalize(embi_str method, embi_sb *out);

/* ------------------------------------------------------------------ body and response */

/* A body: bytes and the used flag. present is false for a null body (no body at all). */
typedef struct embi_body {
    embi_sb bytes;
    bool present;
    bool used;
} embi_body;

typedef enum embi_resp_type {
    EMBI_RESP_DEFAULT,        /* a Response made by the constructor: type "default" */
    EMBI_RESP_BASIC,          /* a fetch response: type "basic" */
    EMBI_RESP_ERROR,          /* a network error or Response.error(): type "error" */
    EMBI_RESP_OPAQUEREDIRECT, /* redirect: "manual": type "opaqueredirect", status 0 */
    EMBI_RESP_CORS,           /* a network response (http and https): type "cors" (there is no CORS check) */
} embi_resp_type;

typedef struct embi_response {
    enum embi_resp_type type;
    uint16_t status;
    embi_sb status_text;
    embi_headers headers;
    embi_body body;
    embi_sb url;     /* the last URL of the list (empty for a constructed Response) */
    bool redirected; /* the response came through at least one redirect */
} embi_response;

void embi_response_free(embi_response *r);
/* Status 200 to 599 is a valid construction status (Response constructor). */
bool embi_status_ok(uint32_t status);
/* 101, 103, 204, 205 and 304: a Response with these must have a null body. */
bool embi_status_null_body(uint32_t status);
/* 301, 302, 303, 307 and 308. */
bool embi_status_redirect(uint32_t status);
/* The reason-phrase text check (Fetch: a reason-phrase is printable ASCII and a tab or space). */
bool embi_status_text_ok(embi_str text);

/* Consumes the body: on success the bytes are moved to out (in the body's own buffer order) and
 * the body is marked used. Returns false when the body is used already, or when there is no body
 * (a null body is never used and yields empty bytes). */
bool embi_body_consume(embi_body *b, embi_sb *out);

/* ------------------------------------------------------------------ fetch operation */

typedef struct embi_fetch_op embi_fetch_op;

/* The redirect modes (RequestRedirect). */
typedef enum embi_redirect {
    EMBI_REDIRECT_FOLLOW,
    EMBI_REDIRECT_ERROR,
    EMBI_REDIRECT_MANUAL,
} embi_redirect;

/* One hop: what the transport serves for a request. body may be nullptr. */
typedef struct embi_fetch_hop {
    uint16_t status;
    embi_str status_text; /* network hops: the reason phrase (empty: none; Response.statusText is then empty) */
    embi_str mime;    /* the media type, empty when the handler gave none (not a header) */
    const embi_hdr_view *headers;
    uint32_t nheaders;
    const uint8_t *body;
    size_t body_len;
    /* Network hops only (N1b). url is the final URL after the transport's own redirects (empty: the operation's URL);
     * followed says the transport already followed the redirects of the redirect mode, so the operation does not;
     * redirected is then whether it did; cors makes the response type "cors". */
    embi_str url;
    bool followed;
    bool redirected;
    bool cors;
} embi_fetch_hop;

/* Credentials mode (RequestCredentials) and the redirect mode, as the transport needs them. */
typedef enum embi_fetch_creds {
    EMBI_FETCH_CREDS_OMIT,
    EMBI_FETCH_CREDS_SAME_ORIGIN,
    EMBI_FETCH_CREDS_INCLUDE,
} embi_fetch_creds;

/* What the transport needs to know about the operation besides the hop. origin is the initiator's origin ("scheme://host[:port]"),
 * empty when there is none (then same-origin never matches). Borrowed from the operation. */
typedef struct embi_fetch_options {
    embi_redirect redirect;
    embi_fetch_creds creds;
    embi_str origin;
} embi_fetch_options;

/* A streamed body (N1b): the operation calls these as the network delivers the body, in order, once the head is done.
 * All run on the thread that completes the hop (the view's thread for a posted transport). After end or error, and after
 * an abort, none run. */
typedef struct embi_fetch_sink {
    void *user;
    void (*chunk)(void *user, const uint8_t *data, size_t len);
    void (*end)(void *user);
    void (*error)(void *user, embi_str message);
} embi_fetch_sink;

/* The transport the operation sends its hops through. start begins one hop: it must complete the hop with
 * embi_fetch_hop_done, now (inside start) or later, and it sets *handle to its own token for the hop
 * (nullptr when the hop was already completed inside start). Returning false refuses the hop (a network
 * error): it must then not complete it, and leaves *handle alone. abort(user, handle) is called when the
 * operation is aborted or freed while the hop is in flight; the transport must then not complete that
 * hop. */
typedef struct embi_fetch_transport {
    void *user;
    bool (*start)(void *user, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                  const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len);
    void (*abort)(void *user, void *handle);
    /* Optional (nullptr: none). hold(user, handle, on) pauses (on) or resumes (off) the network's reading of an
     * in-flight hop while the operation's consumer is behind (backpressure); it is called only while the hop is in flight. */
    void (*hold)(void *user, void *handle, bool on);
} embi_fetch_transport;

/* What a fetch starts from. url is absolute and already parsed by the caller (the URL bindings).
 * headers are the request's (copied). The body is copied. */
typedef struct embi_fetch_init {
    embi_str method;
    embi_str url;
    const embi_hdr_view *headers;
    uint32_t nheaders;
    const uint8_t *body; /* nullptr for no body */
    size_t body_len;
    embi_redirect redirect;
    embi_fetch_creds creds;       /* N1b: credentials mode */
    embi_str initiator_origin;    /* N1b: empty when none */
    const embi_fetch_sink *sink;  /* N1b: nullptr unless the body streams to the caller */
} embi_fetch_init;

/* Called once with ok true (the response is in embi_fetch_op_take) or ok false (a network error: see
 * embi_fetch_op_error). Not called after an abort. It may run inside embi_fetch_op_new. */
typedef void (*embi_fetch_done_fn)(void *user, embi_fetch_op *op, bool ok);

/* Starts the operation: the first hop goes out at once. Returns nullptr only when out of memory or
 * when the URL is not valid (no done call then). */
embi_fetch_op *embi_fetch_op_new(const embi_fetch_transport *t, const embi_fetch_init *in, embi_fetch_done_fn done,
                                 void *user);
/* Aborts a pending operation: the in-flight hop is aborted through the transport and done is not called.
 * Does nothing once the operation has completed. */
void embi_fetch_op_abort(embi_fetch_op *op);
/* Completes the current hop (called by the transport, see embi_fetch_transport). Redirects are followed here (unless
 * hop->followed). */
void embi_fetch_hop_done(embi_fetch_op *op, const embi_fetch_hop *hop);
/* Streamed hops (N1b). embi_fetch_hop_stream_begin completes the operation with the head (the response body is empty
 * and the operation is then streaming: done is called with ok true, see embi_fetch_op_streaming). The transport then
 * reports the body with _chunk, _end, or _fail, and the operation calls the sink for each. _fail before the head is a
 * network error. Once the operation is streaming, aborting it aborts the transport's hop. */
void embi_fetch_hop_stream_begin(embi_fetch_op *op, const embi_fetch_hop *head);
void embi_fetch_hop_stream_chunk(embi_fetch_op *op, const uint8_t *data, size_t len);
void embi_fetch_hop_stream_end(embi_fetch_op *op);
void embi_fetch_hop_fail(embi_fetch_op *op, embi_str message);
/* Whether the operation delivers its body through the sink (its response has a body that is still arriving). */
bool embi_fetch_op_streaming(const embi_fetch_op *op);
/* The options of the operation (valid while it lives). */
void embi_fetch_op_options(const embi_fetch_op *op, embi_fetch_options *out);

/* Pauses or resumes the in-flight hop's transport (see embi_fetch_transport.hold). No effect when nothing is in flight. */
void embi_fetch_op_hold(embi_fetch_op *op, bool on);
/* Frees the operation. A pending one is aborted first. */
void embi_fetch_op_free(embi_fetch_op *op);
/* Whether the operation has neither completed nor been aborted. */
bool embi_fetch_op_pending(const embi_fetch_op *op);
/* After done with ok true: moves the response out (the operation then holds an empty one). */
void embi_fetch_op_take(embi_fetch_op *op, embi_response *out);
/* After done with ok false: the message of the network error (borrowed). */
embi_str embi_fetch_op_error(const embi_fetch_op *op);
/* Builds a network-error response (Response.error()). */
void embi_response_error(embi_response *out);
