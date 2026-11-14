/*
 * Connections of module net (N1): TCP through SDL_net, TLS through Mbed TLS with BIO callbacks over the SDL_net socket.
 */
#include "conn.h"

#define FILL_CHUNK 16384

embi_net_conn *embi_net_conn_open(NET_Address *addr, int port, char *err, size_t err_cap)
{
    NET_StreamSocket *s = NET_CreateClient(addr, (Uint16)port, 0);
    if (s == nullptr) {
        SDL_snprintf(err, err_cap, "connect failed: %s", SDL_GetError());
        return nullptr;
    }
    embi_net_conn *c = embi_net_zalloc(sizeof *c);
    c->sock = s;
    c->state = CONN_CONNECTING;
    return c;
}

void embi_net_conn_close(embi_net_conn *c)
{
    if (c == nullptr) {
        return;
    }
    if (c->tls != nullptr) {
        if (c->state == CONN_READY && !c->eof) {
            embi_net_tls_close_notify(c->tls);
        }
        embi_net_tls_free(c->tls);
    }
    if (c->sock != nullptr) {
        NET_DestroyStreamSocket(c->sock);
    }
    embi_buf_free(&c->rbuf);
    embi_net_mfree(c);
}

/* The raw transport under TLS. recv: bytes, 0 for none yet, -1 for end or error. */
static int io_recv(void *user, void *buf, int len)
{
    embi_net_conn *c = user;
    int n = NET_ReadFromStreamSocket(c->sock, buf, len);
    if (n < 0) {
        return -1;
    }
    return n;
}

static bool io_send(void *user, const void *buf, int len)
{
    embi_net_conn *c = user;
    return NET_WriteToStreamSocket(c->sock, buf, len);
}

bool embi_net_conn_advance(embi_net_conn *c, bool tls, embi_net_tlsctx *tlsctx, const char *host, embi_net_err *code, char *err, size_t err_cap)
{
    if (c->state == CONN_CONNECTING) {
        NET_Status st = NET_GetConnectionStatus(c->sock);
        if (st == NET_FAILURE) {
            *code = EMBI_NET_ERR_CONNECT;
            SDL_snprintf(err, err_cap, "connect failed: %s", SDL_GetError());
            return false;
        }
        if (st == NET_WAITING) {
            return true;
        }
        if (!tls) {
            c->state = CONN_READY;
            return true;
        }
        c->tls = embi_net_tls_new(tlsctx, host, (embi_net_io){io_recv, io_send, c}, err, err_cap);
        if (c->tls == nullptr) {
            *code = EMBI_NET_ERR_TLS;
            return false;
        }
        c->state = CONN_HANDSHAKE;
    }
    if (c->state == CONN_HANDSHAKE) {
        embi_net_tls_rc rc = embi_net_tls_handshake(c->tls, err, err_cap);
        if (rc == EMBI_TLS_ERROR) {
            *code = EMBI_NET_ERR_TLS;
            return false;
        }
        if (rc == EMBI_TLS_OK) {
            c->state = CONN_READY;
        }
    }
    return true;
}

static bool is_eof_error(void)
{
    return SDL_strstr(SDL_GetError(), "End of stream") != nullptr;
}

fill_rc embi_net_conn_fill(embi_net_conn *c, size_t max, char *err, size_t err_cap)
{
    if (c->pending_rc != FILL_NONE) {
        fill_rc rc = (fill_rc)c->pending_rc;
        SDL_snprintf(err, err_cap, "%s", c->pending_err);
        return rc;
    }
    fill_rc result = FILL_NONE;
    uint8_t tmp[FILL_CHUNK];
    size_t total = 0;
    while (total < max) {
        size_t got = 0;
        fill_rc stop = FILL_NONE;
        if (c->tls != nullptr) {
            embi_net_tls_rc rc = embi_net_tls_read(c->tls, tmp, sizeof tmp, &got, err, err_cap);
            if (rc == EMBI_TLS_WANT_IO) {
                break;
            }
            if (rc == EMBI_TLS_CLOSED || rc == EMBI_TLS_CLOSED_UNCLEAN) {
                c->tls_unclean = rc == EMBI_TLS_CLOSED_UNCLEAN;
                stop = FILL_EOF;
            } else if (rc == EMBI_TLS_ERROR) {
                stop = FILL_ERROR;
            }
        } else {
            int n = NET_ReadFromStreamSocket(c->sock, tmp, sizeof tmp);
            if (n < 0) {
                if (is_eof_error()) {
                    stop = FILL_EOF;
                } else {
                    SDL_snprintf(err, err_cap, "read failed: %s", SDL_GetError());
                    stop = FILL_ERROR;
                }
            } else if (n == 0) {
                break;
            } else {
                got = (size_t)n;
            }
        }
        if (stop != FILL_NONE) {
            c->eof = stop == FILL_EOF;
            if (result == FILL_DATA) { /* report the bytes now and the end on the next call */
                c->pending_rc = stop;
                SDL_snprintf(c->pending_err, sizeof c->pending_err, "%s", stop == FILL_ERROR ? err : "");
                return FILL_DATA;
            }
            return stop;
        }
        (void)embi_buf_append(&c->rbuf, tmp, got);
        total += got;
        result = FILL_DATA;
    }
    return result;
}

bool embi_net_conn_write(embi_net_conn *c, const void *buf, size_t len, char *err, size_t err_cap)
{
    const uint8_t *p = buf;
    if (c->tls == nullptr) {
        while (len > 0) {
            int chunk = len > (1u << 20) ? (1 << 20) : (int)len;
            if (!NET_WriteToStreamSocket(c->sock, p, chunk)) {
                SDL_snprintf(err, err_cap, "write failed: %s", SDL_GetError());
                return false;
            }
            p += chunk;
            len -= (size_t)chunk;
        }
        return true;
    }
    int stalls = 0;
    while (len > 0) {
        size_t w = 0;
        embi_net_tls_rc rc = embi_net_tls_write(c->tls, p, len, &w, err, err_cap);
        if (rc == EMBI_TLS_ERROR) {
            return false;
        }
        if (rc == EMBI_TLS_WANT_IO && ++stalls > 8) {
            SDL_snprintf(err, err_cap, "TLS write stalled");
            return false;
        }
        p += w;
        len -= w;
    }
    return true;
}

bool embi_net_conn_alive(embi_net_conn *c)
{
    if (c->eof || c->state != CONN_READY || c->rbuf.len > 0) {
        return false;
    }
    char err[64];
    if (c->tls != nullptr) {
        uint8_t b;
        size_t got;
        return embi_net_tls_read(c->tls, &b, 1, &got, err, sizeof err) == EMBI_TLS_WANT_IO;
    }
    uint8_t b;
    int n = NET_ReadFromStreamSocket(c->sock, &b, 1);
    return n == 0;
}
