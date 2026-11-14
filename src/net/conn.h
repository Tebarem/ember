#pragma once
/*
 * One connection of the pool (N1): the SDL_net socket, the optional TLS session over it, and a receive buffer.
 * Everything here runs on the network thread. Private to module net.
 */
#include "net_int.h"

typedef enum conn_state {
    CONN_CONNECTING,
    CONN_HANDSHAKE,
    CONN_READY,
} conn_state;

typedef struct embi_net_conn embi_net_conn;
typedef struct embi_net_origin embi_net_origin;

struct embi_net_conn {
    embi_net_origin *org;
    NET_StreamSocket *sock;
    embi_net_tls *tls;
    conn_state state;
    embi_buf rbuf;        /* plaintext received and not yet consumed */
    embi_net_req *owner;  /* nullptr when idle in the pool */
    uint64_t idle_since;  /* SDL_GetTicks when it went idle */
    bool used;            /* has served a request: a failure before any response byte is retried */
    bool eof;             /* the peer closed */
    bool tls_unclean;     /* ... over TLS without close_notify */
    int pending_rc;       /* a fill_rc found after bytes were read, reported by the next fill */
    char pending_err[160];
};

typedef enum fill_rc {
    FILL_NONE,   /* nothing more right now */
    FILL_DATA,   /* bytes were appended to rbuf */
    FILL_EOF,    /* the peer closed */
    FILL_ERROR,  /* a socket or TLS error (err has the text) */
} fill_rc;

/* Starts the TCP connection (non-blocking). nullptr on failure, with the text in err. */
embi_net_conn *embi_net_conn_open(NET_Address *addr, int port, char *err, size_t err_cap);
void embi_net_conn_close(embi_net_conn *c);
/* Advances CONNECTING and HANDSHAKE. Returns false on failure (code: CONNECT or TLS, and err), true otherwise (check
 * c->state). tlsctx and host are used when tls is true. */
bool embi_net_conn_advance(embi_net_conn *c, bool tls, embi_net_tlsctx *tlsctx, const char *host, embi_net_err *code, char *err, size_t err_cap);
/* Reads what is available (at most max bytes) into rbuf. */
fill_rc embi_net_conn_fill(embi_net_conn *c, size_t max, char *err, size_t err_cap);
bool embi_net_conn_write(embi_net_conn *c, const void *buf, size_t len, char *err, size_t err_cap);
/* Whether an idle connection is still usable (no EOF, no unexpected bytes). */
bool embi_net_conn_alive(embi_net_conn *c);
