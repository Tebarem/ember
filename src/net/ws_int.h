#pragma once
/*
 * The seam between net.c (the request state machine) and websocket.c (framing). Private to module net.
 */
#include "conn.h"
#include "ws.h"

typedef struct embi_ws embi_ws;

void embi_ws_free(embi_ws *w);
/* The Sec-WebSocket-Accept value for a Sec-WebSocket-Key (28 characters and a NUL; used by the tests' server too). */
void embi_ws_accept_key(const char *key, char out[32]);
/* Appends the Upgrade, Connection, Sec-WebSocket-* and Origin header lines of the opening handshake. */
void embi_ws_request_headers(embi_ws *w, embi_sb *out);
/* Checks the handshake response. True: the connection is open (the open event is queued). False: err says why. */
bool embi_ws_accept(embi_net_req *r, embi_ws *w, const embi_http_head *h, char *err, size_t err_cap);
/* Whether close() was called before the connection opened. */
bool embi_ws_close_early(embi_ws *w);
/* One pass over an open connection: read, parse, deliver, write. True when the connection is over. */
bool embi_ws_step(embi_net_req *r, embi_ws *w, embi_net_conn *c, uint64_t now);

/* net.c */
embi_ws *embi_net_req_ws(embi_net_req *r);
/* Starts a request that carries a WebSocket (nullptr ws: a plain request). */
embi_net_req *embi_net_start_ex(embi_net *n, const embi_net_request *q, embi_ws *ws);
/* Queues fn(arg) like an event of the request: on its poster, or inline. Not run (drop instead) once the request was
 * aborted unless always. bytes count against the request's backpressure. */
void embi_net_req_post_call(embi_net_req *r, void (*fn)(void *), void (*drop)(void *), void *arg, size_t bytes, bool always);
/* Whether the receiving thread is more than a few MB behind. */
bool embi_net_req_congested(embi_net_req *r);
bool embi_net_req_aborted(embi_net_req *r);
