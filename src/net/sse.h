#pragma once
/*
 * EventSource (N1c, HTML Standard 9.2): the event stream parser and the connection that reads it, built on
 * embi_net_start with a streaming body. Internal API: embi_ prefix.
 *
 * The connection (embi_sse) is driven from one thread, the one its post function delivers to: every callback runs
 * there, and embi_sse_close must be called there. It fails for good for a status other than 200, a content type
 * other than text/event-stream, a blocked host, a bad URL, a TLS failure and an oversized event; after a lost
 * connection or another network error it reports an error with reconnecting set and connects again after the
 * reconnection time (the retry field, 3 seconds at first), sending Last-Event-ID once an event with an id arrived.
 */
#include "codec/codec.h"
#include "net/net.h"

/* ------------------------------------------------------------------ parser */

typedef struct embi_sse_parser {
    embi_buf line, data, etype, idbuf;
    uint8_t hold[3];
    uint8_t nhold;
    bool bom_done, skip_lf, overflow;
    size_t max;
    /* An event: type is "message" when the stream gave none; data has the trailing LF removed. All are valid UTF-8
     * (invalid sequences are replaced by U+FFFD) and NUL-terminated. */
    void (*on_event)(void *user, const char *type, const char *data, size_t data_len, const char *last_event_id);
    void (*on_retry)(void *user, uint32_t ms);
    void *user;
} embi_sse_parser;

/* max bounds a line and the data of one event; past it, overflow is set and the parser ignores input. */
void embi_sse_parser_init(embi_sse_parser *p, size_t max, const char *last_event_id,
                          void (*on_event)(void *, const char *, const char *, size_t, const char *),
                          void (*on_retry)(void *, uint32_t), void *user);
/* A new connection: forget a half-read line, event and BOM state; the last event id stays. */
void embi_sse_parser_reset(embi_sse_parser *p);
void embi_sse_parser_feed(embi_sse_parser *p, const uint8_t *d, size_t n);
void embi_sse_parser_free(embi_sse_parser *p);

/* ------------------------------------------------------------------ connection */

typedef struct embi_sse embi_sse;

typedef struct embi_sse_callbacks {
    void (*on_open)(void *user);
    /* origin: the origin of the final response URL. */
    void (*on_message)(void *user, const char *type, const char *data, size_t data_len, const char *last_event_id,
                       const char *origin);
    /* reconnecting: the connection will be retried (readyState CONNECTING); otherwise it failed for good (CLOSED). */
    void (*on_error)(void *user, bool reconnecting, const char *message);
    /* Exactly once, last, when nothing will call back any more. May run on any thread; free the user data here. */
    void (*release)(void *user);
} embi_sse_callbacks;

typedef struct embi_sse_params {
    const char *url; /* absolute http or https URL */
    bool with_credentials;
    const char *initiator_origin; /* the page origin, for same-origin credentials */
    uint32_t retry_ms;            /* the first reconnection time (0: 3000) */
    embi_sse_callbacks cb;
    void *user;
    embi_net_post_fn post;        /* required */
    void *post_user;
} embi_sse_params;

/* Starts connecting. nullptr without a post function. The handle is released by embi_sse_close. */
embi_sse *embi_sse_open(embi_net *n, const embi_sse_params *p);
/* Aborts the connection and drops the caller handle; after it only release is called. Call it once. */
void embi_sse_close(embi_sse *s);
