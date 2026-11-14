#pragma once
/*
 * Network loads of a view (N1d, N1f): http and https subresources and documents over the runtime's network
 * service (src/net). Internal API: embi_ prefix. Implemented in netload.c (EMBER_NET builds; the OFF build has stubs that
 * claim nothing).
 *
 * The document side is synchronous (fetch callbacks return bytes), so a network load is a record in a per-view table, keyed
 * by URL. A request runs on the network thread; its result is stored in the record. The view's thread never sleeps on a
 * record: a consumer asks for it, and a record that is not complete answers EMBI_NL_PENDING. The consumer treats the resource
 * as not there yet (a parser-blocking script suspends the parse, a render-blocking sheet suspends the first layout, an image
 * or a font is laid out without it), and when the record completes a task posted to the view's event queue tells the
 * document (embi_doc_net_arrived), which resumes whatever was waiting.
 * A script, module or stylesheet that has not arrived after a time (embi_netload_set_wait_ms) fails with a timeout.
 * The records belong to one document: embi_netload_reset (a new document) aborts the requests and drops them.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct emb_view emb_view;
typedef struct embi_netload embi_netload;

/* What a not-yet-complete record tells the document when it completes (bit mask). EMBI_NL_SCRIPT is on every record of a
 * document: the load (scripts, modules, render-blocking sheets) may be waiting for it. */
enum { EMBI_NL_IMAGE = 1, EMBI_NL_FONT = 2, EMBI_NL_SHEET = 4, EMBI_NL_SCRIPT = 8 };

typedef enum embi_nl_result {
    EMBI_NL_FAIL = 0,
    EMBI_NL_OK = 1,
    EMBI_NL_PENDING = 2,
} embi_nl_result;

/* Whether url is an http(s) URL no embedder handler claims (always false with EMBER_NET off). */
bool embi_netload_claims(emb_view *v, embi_str url);

/* Whether url is one the network serves and the embedder's allowlist lets through (the view may navigate to it itself). */
bool embi_netload_allowed(emb_view *v, embi_str url);

/* Loads url. kind: EMBI_NL_* the document is told about on completion (when it is not complete now: EMBI_NL_PENDING).
 * On EMBI_NL_OK the bytes are SDL_malloc'd into data and len, and the Content-Type is appended to mime (may be nullptr). */
embi_nl_result embi_netload_get(emb_view *v, embi_str url, unsigned kind, uint8_t **data, size_t *len, embi_sb *mime);
/* The state of url's record without taking its bytes (starts the request if there is none): PENDING, OK or FAIL. */
embi_nl_result embi_netload_state(emb_view *v, embi_str url);
/* Whether any record of the document is still downloading (the load event waits for them). */
bool embi_netload_busy(emb_view *v);
/* Whether every record (fonts and the main document too) has completed and its arrival task has run: nothing more will come
 * from the network for the current document. For tests that must pump until an arrival is handled. */
bool embi_netload_quiet(emb_view *v);
/* Whether a network image's request is still in flight (img.complete is false meanwhile). */
bool embi_netload_image_pending(emb_view *v, embi_str url);
/* False (with one warning) when the view's document came from the network and url is a local resource (file:, app:, ...). */
bool embi_netload_scheme_ok(emb_view *v, embi_str url);
/* Sets the time a script, module or stylesheet may take to arrive, in milliseconds (default 30000; 0 restores it). It applies
 * to requests started afterwards. For tests. */
void embi_netload_set_wait_ms(unsigned ms);
/* Records a refused image load as failed, so the <img> gets its error event. */
void embi_netload_refuse_image(emb_view *v, embi_str url);
/* Starts the request for url if there is none yet (a script that will run later). */
void embi_netload_prefetch(emb_view *v, embi_str url);
/* How many EMBI_NL_PENDING answers the view has given: a consumer compares before and after to see that something was
 * pending. */
uint32_t embi_netload_pending_count(emb_view *v);

/* Navigates the view to an http(s) URL (the main document): the request starts at once, the old document stays until the
 * response is in, then the new one is loaded with the response's final URL as its base. A failure shows an error page and
 * logs one warning. False when the URL is not one the network serves. */
bool embi_netload_navigate(emb_view *v, embi_str url);

/* Replaces the view's document with a small error page that names url and the reason (the same page a failed main document
 * gets). Used by a suspended load that fails to lay out when it finishes. */
void embi_netload_show_error(emb_view *v, const char *url, const char *msg);

/* The document is replaced (emb_view_load_html): the records of the old one go, and so does a main-document request still
 * in flight (the newest navigation wins). */
void embi_netload_reset(emb_view *v);
/* The view is being destroyed: the network thread stops posting to it. Call this first, before the document goes. */
void embi_netload_detach(emb_view *v);
/* The view is destroyed (after the document): frees the table. */
void embi_netload_free(emb_view *v);
