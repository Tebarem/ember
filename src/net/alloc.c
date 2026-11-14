/*
 * Counted allocation of module net: every object is an embi_alloc with a size header, so tests can check that nothing
 * is left (embi_net_live) and frees need no size.
 */
#include "net_int.h"

typedef union net_hdr {
    size_t size;
    max_align_t align;
} net_hdr;

static SDL_AtomicInt g_live;

void *embi_net_zalloc(size_t n)
{
    net_hdr *h = embi_alloc(sizeof(net_hdr) + n);
    h->size = n;
    memset(h + 1, 0, n);
    SDL_AddAtomicInt(&g_live, 1);
    return h + 1;
}

void embi_net_mfree(void *p)
{
    if (p == nullptr) {
        return;
    }
    net_hdr *h = (net_hdr *)p - 1;
    SDL_AddAtomicInt(&g_live, -1);
    embi_free(h, sizeof(net_hdr) + h->size);
}

void *embi_net_realloc(void *p, size_t n)
{
    if (p == nullptr) {
        return embi_net_zalloc(n);
    }
    net_hdr *h = (net_hdr *)p - 1;
    size_t old = h->size;
    if (n <= old) {
        return p;
    }
    void *np = embi_net_zalloc(n);
    memcpy(np, p, old);
    embi_net_mfree(p);
    return np;
}

char *embi_net_strndup(const char *s, size_t n)
{
    char *d = embi_net_zalloc(n + 1);
    if (n > 0) {
        memcpy(d, s, n);
    }
    return d;
}

char *embi_net_strdup(const char *s)
{
    return embi_net_strndup(s, strlen(s));
}

size_t embi_net_live(void)
{
    return (size_t)SDL_GetAtomicInt(&g_live);
}
