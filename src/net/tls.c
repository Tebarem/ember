/*
 * TLS client over Mbed TLS (N1). Certificate verification is always required: there is no switch for it anywhere.
 * TLS 1.2 is the minimum (Mbed TLS 4 has nothing older), SNI and hostname verification are on for every connection,
 * ALPN offers http/1.1. The roots are Mozilla's list (embedded with #embed, parsed on the first connection that needs
 * it) plus the embedder's. Randomness is Mbed TLS's PSA RNG seeded by its own OS-backed entropy source.
 */
#include "net_int.h"

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef EMBER_LZ_ASSETS
/* Mozilla's roots (third_party/cacert, see its README) as DER records, each a 2-byte little-endian length and the
 * certificate (tools/lz/pem2der.c converts the PEM at build time: 127 KB against 200 KB; SZ3a). */
#include "ember_lz/net/roots.der.h"

static int parse_bundled_roots(mbedtls_x509_crt *chain)
{
    /* Like mbedtls_x509_crt_parse on the PEM: the number of roots that failed to parse (an algorithm this build lacks),
     * or a negative code when the chain cannot take more. */
    int failed = 0;
    for (size_t i = 0; i + 2 <= sizeof embi_net_roots_der;) {
        size_t n = (size_t)embi_net_roots_der[i] | ((size_t)embi_net_roots_der[i + 1] << 8);
        i += 2;
        if (n > sizeof embi_net_roots_der - i) {
            return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
        }
        int rc = mbedtls_x509_crt_parse_der(chain, embi_net_roots_der + i, n);
        if (rc == MBEDTLS_ERR_X509_ALLOC_FAILED) {
            return rc;
        }
        failed += rc != 0;
        i += n;
    }
    return failed;
}
#else
/* Mozilla's roots as PEM text, NUL-terminated (third_party/cacert, see its README). */
static const unsigned char g_bundled_roots[] = {
#embed "../../third_party/cacert/cacert.pem" suffix(, 0)
};

static int parse_bundled_roots(mbedtls_x509_crt *chain)
{
    return mbedtls_x509_crt_parse(chain, g_bundled_roots, sizeof g_bundled_roots);
}
#endif

/* ------------------------------------------------------------------ process-wide setup */

static int thr_mutex_init(mbedtls_platform_mutex_t *m)
{
    *m = SDL_CreateMutex();
    return *m != nullptr ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}

static void thr_mutex_free(mbedtls_platform_mutex_t *m)
{
    SDL_DestroyMutex(*m);
    *m = nullptr;
}

static int thr_mutex_lock(mbedtls_platform_mutex_t *m)
{
    SDL_LockMutex(*m);
    return 0;
}

static int thr_mutex_unlock(mbedtls_platform_mutex_t *m)
{
    SDL_UnlockMutex(*m);
    return 0;
}

static int thr_cond_init(mbedtls_platform_condition_variable_t *c)
{
    *c = SDL_CreateCondition();
    return *c != nullptr ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}

static void thr_cond_free(mbedtls_platform_condition_variable_t *c)
{
    SDL_DestroyCondition(*c);
    *c = nullptr;
}

static int thr_cond_signal(mbedtls_platform_condition_variable_t *c)
{
    SDL_SignalCondition(*c);
    return 0;
}

static int thr_cond_broadcast(mbedtls_platform_condition_variable_t *c)
{
    SDL_BroadcastCondition(*c);
    return 0;
}

static int thr_cond_wait(mbedtls_platform_condition_variable_t *c, mbedtls_platform_mutex_t *m)
{
    SDL_WaitCondition(*c, *m);
    return 0;
}

static SDL_InitState g_tls_init;
static bool g_tls_ok;

bool embi_net_tls_global_init(void)
{
    if (SDL_ShouldInit(&g_tls_init)) {
        mbedtls_threading_set_alt(thr_mutex_init, thr_mutex_free, thr_mutex_lock, thr_mutex_unlock, thr_cond_init,
                                  thr_cond_free, thr_cond_signal, thr_cond_broadcast, thr_cond_wait);
        embi_net_platform_init();
        uint8_t probe[16];
        g_tls_ok = embi_net_entropy(probe, sizeof probe) && psa_crypto_init() == PSA_SUCCESS;
        SDL_SetInitialized(&g_tls_init, true);
    }
    return g_tls_ok;
}

/* ------------------------------------------------------------------ roots */

typedef struct embi_net_root_blob {
    uint8_t *data; /* PEM text is NUL-terminated (one extra byte) */
    size_t len;
    bool pem;
} embi_net_root_blob;

void embi_net_roots_init(embi_net_roots *r)
{
    *r = (embi_net_roots){0};
    r->m = SDL_CreateMutex();
    r->bundled = true;
    r->version = 1;
}

void embi_net_roots_free(embi_net_roots *r)
{
    for (uint32_t i = 0; i < r->nblobs; i++) {
        embi_net_mfree(r->blobs[i].data);
    }
    embi_net_mfree(r->blobs);
    SDL_DestroyMutex(r->m);
    *r = (embi_net_roots){0};
}

static bool looks_pem(const uint8_t *d, size_t n)
{
    static const char tag[] = "-----BEGIN";
    for (size_t i = 0; i + sizeof tag - 1 <= n; i++) {
        if (memcmp(d + i, tag, sizeof tag - 1) == 0) {
            return true;
        }
    }
    return false;
}

bool embi_net_roots_add(embi_net_roots *r, const void *data, size_t len)
{
    if (data == nullptr || len == 0 || len > ((size_t)16 << 20) || !embi_net_tls_global_init()) {
        return false; /* (parsing a certificate needs the crypto library initialized, which is lazy) */
    }
    /* Validate now so the embedder hears about bad data: parse into a scratch chain. */
    bool pem = looks_pem(data, len);
    uint8_t *copy = embi_net_zalloc(len + 1);
    memcpy(copy, data, len);
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = pem ? mbedtls_x509_crt_parse(&crt, copy, len + 1) : mbedtls_x509_crt_parse_der(&crt, copy, len);
    bool any = crt.version != 0;
    mbedtls_x509_crt_free(&crt);
    if (rc < 0 || !any) {
        embi_net_mfree(copy);
        return false;
    }
    SDL_LockMutex(r->m);
    r->blobs = embi_net_realloc(r->blobs, sizeof *r->blobs * (r->nblobs + 1));
    r->blobs[r->nblobs++] = (embi_net_root_blob){copy, len, pem};
    r->version++;
    SDL_UnlockMutex(r->m);
    return true;
}

void embi_net_roots_use_bundled(embi_net_roots *r, bool on)
{
    SDL_LockMutex(r->m);
    if (r->bundled != on) {
        r->bundled = on;
        r->version++;
    }
    SDL_UnlockMutex(r->m);
}

size_t embi_net_bundled_roots_check(int *failed)
{
    *failed = -1;
    if (!embi_net_tls_global_init()) {
        return 0;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    *failed = parse_bundled_roots(&crt);
    size_t n = 0;
    for (const mbedtls_x509_crt *p = &crt; p != nullptr && p->version != 0; p = p->next) {
        n++;
    }
    mbedtls_x509_crt_free(&crt);
    return n;
}

/* ------------------------------------------------------------------ client configuration */

struct embi_net_tlsctx {
    SDL_AtomicInt refs;
    uint32_t version;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt chain;
};

static const char *const g_alpn[] = {"http/1.1", nullptr};

static void tlsctx_destroy(embi_net_tlsctx *c)
{
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->chain);
    embi_net_mfree(c);
}

void embi_net_tlsctx_unref(embi_net_tlsctx *c)
{
    if (c != nullptr && SDL_AddAtomicInt(&c->refs, -1) == 1) {
        tlsctx_destroy(c);
    }
}

static void set_err(char *err, size_t cap, const char *what, int rc)
{
    char detail[160];
    detail[0] = '\0';
    mbedtls_strerror(rc, detail, sizeof detail);
    SDL_snprintf(err, cap, "%s: %s (-0x%04X)", what, detail, (unsigned)-rc);
}

embi_net_tlsctx *embi_net_tlsctx_get(embi_net_roots *r, embi_net_tlsctx *current, char *err, size_t err_cap)
{
    if (!embi_net_tls_global_init()) {
        SDL_snprintf(err, err_cap, "TLS library initialization failed");
        return nullptr;
    }
    SDL_LockMutex(r->m);
    uint32_t version = r->version;
    if (current != nullptr && current->version == version) {
        SDL_UnlockMutex(r->m);
        SDL_AddAtomicInt(&current->refs, 1);
        return current;
    }
    embi_net_tlsctx *c = embi_net_zalloc(sizeof *c);
    SDL_SetAtomicInt(&c->refs, 1);
    c->version = version;
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->chain);
    bool ok = true;
    int rc = 0;
    if (r->bundled) {
        /* A few roots may use algorithms this build lacks: the call returns how many failed to parse. */
        rc = parse_bundled_roots(&c->chain);
        if (rc < 0) {
            set_err(err, err_cap, "parsing the bundled roots failed", rc);
            ok = false;
        }
    }
    for (uint32_t i = 0; ok && i < r->nblobs; i++) {
        const embi_net_root_blob *b = &r->blobs[i];
        rc = b->pem ? mbedtls_x509_crt_parse(&c->chain, b->data, b->len + 1)
                    : mbedtls_x509_crt_parse_der(&c->chain, b->data, b->len);
        if (rc < 0) {
            set_err(err, err_cap, "parsing a root failed", rc);
            ok = false;
        }
    }
    SDL_UnlockMutex(r->m);
    if (ok) {
        rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) {
            set_err(err, err_cap, "TLS configuration failed", rc);
            ok = false;
        }
    }
    if (ok) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->chain, nullptr);
        rc = mbedtls_ssl_conf_alpn_protocols(&c->conf, g_alpn);
        if (rc != 0) {
            set_err(err, err_cap, "TLS configuration failed", rc);
            ok = false;
        }
    }
    if (!ok) {
        tlsctx_destroy(c);
        return nullptr;
    }
    return c;
}

/* ------------------------------------------------------------------ one connection */

struct embi_net_tls {
    embi_net_tlsctx *ctx;
    mbedtls_ssl_context ssl;
    embi_net_io io;
};

static int bio_send(void *p, const unsigned char *buf, size_t len)
{
    embi_net_tls *t = p;
    if (len > 1 << 20) {
        len = 1 << 20;
    }
    return t->io.send(t->io.user, buf, (int)len) ? (int)len : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *p, unsigned char *buf, size_t len)
{
    embi_net_tls *t = p;
    if (len > 1 << 20) {
        len = 1 << 20;
    }
    int n = t->io.recv(t->io.user, buf, (int)len);
    if (n < 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : n;
}

embi_net_tls *embi_net_tls_new(embi_net_tlsctx *ctx, const char *host, embi_net_io io, char *err, size_t err_cap)
{
    embi_net_tls *t = embi_net_zalloc(sizeof *t);
    t->ctx = ctx;
    SDL_AddAtomicInt(&ctx->refs, 1);
    t->io = io;
    mbedtls_ssl_init(&t->ssl);
    /* The name is verified against the certificate and sent as SNI. An IPv6 URL host comes in brackets. */
    char name[256];
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']') {
        host++;
        hl -= 2;
    }
    if (hl == 0 || hl >= sizeof name) {
        SDL_snprintf(err, err_cap, "invalid host name for TLS");
        embi_net_tls_free(t);
        return nullptr;
    }
    memcpy(name, host, hl);
    name[hl] = '\0';
    int rc = mbedtls_ssl_setup(&t->ssl, &ctx->conf);
    if (rc == 0) {
        rc = mbedtls_ssl_set_hostname(&t->ssl, name);
    }
    if (rc != 0) {
        set_err(err, err_cap, "TLS setup failed", rc);
        embi_net_tls_free(t);
        return nullptr;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, nullptr);
    return t;
}

void embi_net_tls_free(embi_net_tls *t)
{
    if (t == nullptr) {
        return;
    }
    mbedtls_ssl_free(&t->ssl);
    embi_net_tlsctx_unref(t->ctx);
    embi_net_mfree(t);
}

embi_net_tls_rc embi_net_tls_handshake(embi_net_tls *t, char *err, size_t err_cap)
{
    int rc = mbedtls_ssl_handshake(&t->ssl);
    if (rc == 0) {
        return EMBI_TLS_OK;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return EMBI_TLS_WANT_IO;
    }
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        char info[256];
        info[0] = '\0';
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
        size_t n = strlen(info);
        while (n > 0 && (info[n - 1] == '\n' || info[n - 1] == ' ')) {
            info[--n] = '\0';
        }
        for (size_t i = 0; i < n; i++) {
            if (info[i] == '\n') {
                info[i] = ';';
            }
        }
        SDL_snprintf(err, err_cap, "certificate verification failed: %s", info);
    } else {
        set_err(err, err_cap, "TLS handshake failed", rc);
    }
    return EMBI_TLS_ERROR;
}

embi_net_tls_rc embi_net_tls_read(embi_net_tls *t, void *buf, size_t cap, size_t *got, char *err, size_t err_cap)
{
    *got = 0;
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buf, cap);
        if (rc > 0) {
            *got = (size_t)rc;
            return EMBI_TLS_OK;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return EMBI_TLS_WANT_IO;
        }
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        /* mbedtls_ssl_read returns 0 when the transport ended without a close_notify alert. */
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_CONN_EOF) {
            return EMBI_TLS_CLOSED_UNCLEAN;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return EMBI_TLS_CLOSED;
        }
        set_err(err, err_cap, "TLS read failed", rc);
        return EMBI_TLS_ERROR;
    }
}

embi_net_tls_rc embi_net_tls_write(embi_net_tls *t, const void *buf, size_t len, size_t *written, char *err, size_t err_cap)
{
    *written = 0;
    int rc = mbedtls_ssl_write(&t->ssl, buf, len);
    if (rc >= 0) {
        *written = (size_t)rc;
        return EMBI_TLS_OK;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return EMBI_TLS_WANT_IO;
    }
    set_err(err, err_cap, "TLS write failed", rc);
    return EMBI_TLS_ERROR;
}

void embi_net_tls_close_notify(embi_net_tls *t)
{
    (void)mbedtls_ssl_close_notify(&t->ssl);
}
