/*
 * ChaCha20 (RFC 8439) and the random source of crypto.getRandomValues (W7c). See random.h.
 */
#include "loader/random.h"

#include "base/base.h"

#include <stdatomic.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_time.h>
#include <SDL3/SDL_timer.h>

static uint32_t rotl32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define QR(a, b, c, d)                                                                             \
    do {                                                                                           \
        a += b;                                                                                    \
        d ^= a;                                                                                    \
        d = rotl32(d, 16);                                                                         \
        c += d;                                                                                    \
        b ^= c;                                                                                    \
        b = rotl32(b, 12);                                                                         \
        a += b;                                                                                    \
        d ^= a;                                                                                    \
        d = rotl32(d, 8);                                                                          \
        c += d;                                                                                    \
        b ^= c;                                                                                    \
        b = rotl32(b, 7);                                                                          \
    } while (0)

void embi_chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64])
{
    uint32_t init[16];
    init[0] = 0x61707865u;
    init[1] = 0x3320646eu;
    init[2] = 0x79622d32u;
    init[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) {
        init[4 + i] = load_le32(key + 4 * i);
    }
    init[12] = counter;
    for (int i = 0; i < 3; i++) {
        init[13 + i] = load_le32(nonce + 4 * i);
    }
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = init[i];
    }
    for (int round = 0; round < 10; round++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        store_le32(out + 4 * i, x[i] + init[i]);
    }
}

void embi_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *buf, size_t n)
{
    uint8_t ks[64];
    size_t off = 0;
    while (off < n) {
        embi_chacha20_block(key, nonce, counter++, ks);
        size_t take = n - off < 64 ? n - off : 64;
        for (size_t i = 0; i < take; i++) {
            buf[off + i] ^= ks[i];
        }
        off += take;
    }
}

static atomic_uint_fast64_t g_seed_counter;
static _Atomic(embi_entropy_fn) g_entropy;

void embi_rng_set_entropy(embi_entropy_fn fn)
{
    atomic_store_explicit(&g_entropy, fn, memory_order_release);
}

/* One splitmix64 step: spreads the bits of a weak sample over the whole word. */
static uint64_t mix64(uint64_t *st)
{
    uint64_t z = (*st += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void embi_rng_seed(embi_rng *r)
{
    /* The material is 32 bytes gathered from the sources. One ChaCha20 block under the material
     * (counter 0, zero nonce) gives the key, so the key is not a raw sample of any one source.
     * SDL's global rand state is not used: it is unsynchronized between threads, and an application
     * that called SDL_srand(n) makes it predictable. The SDL_rand_bits_r generator runs on a local
     * state instead, seeded from the sources below, so every call site contributes its own. */
    SDL_Time now = 0;
    (void)SDL_GetCurrentTime(&now);
    int stack_var = 0;
    uint64_t src[8];
    src[0] = SDL_GetPerformanceCounter();
    src[1] = SDL_GetTicksNS();
    src[2] = (uint64_t)now;
    src[3] = (uint64_t)(uintptr_t)r;
    src[4] = (uint64_t)(uintptr_t)&stack_var;
    src[5] = (uint64_t)SDL_GetCurrentThreadID();
    src[6] = (uint64_t)atomic_fetch_add_explicit(&g_seed_counter, 1, memory_order_relaxed);
    src[7] = (uint64_t)SDL_GetPerformanceFrequency();
    uint64_t st = 0;
    for (int i = 0; i < 8; i++) {
        st ^= src[i];
        (void)mix64(&st);
    }
    uint8_t material[32];
    SDL_memset(material, 0, sizeof material);
    for (int i = 0; i < 8; i++) {
        uint32_t v = (uint32_t)SDL_rand_bits_r(&st);
        store_le32(material + 4 * i, v);
    }
    for (int i = 0; i < 8; i++) {
        for (int bt = 0; bt < 4; bt++) {
            material[4 * i + bt] ^= (uint8_t)(src[i] >> (8 * bt));
        }
    }
    embi_entropy_fn entropy = atomic_load_explicit(&g_entropy, memory_order_acquire);
    uint8_t secure[32];
    if (entropy != nullptr && entropy(secure, sizeof secure)) {
        for (size_t i = 0; i < sizeof secure; i++) {
            material[i] ^= secure[i];
        }
    }
    SDL_memset(secure, 0, sizeof secure);
    static const uint8_t zero_nonce[12];
    uint8_t blk[64];
    embi_chacha20_block(material, zero_nonce, 0, blk);
    SDL_memcpy(r->key, blk, 32);
    SDL_memset(blk, 0, sizeof blk);
    SDL_memset(material, 0, sizeof material);
    r->block = 0;
    r->pos = 32;
    r->seeded = true;
}

/* One block per refill: the first 32 bytes become the next key (fast key erasure), the last 32
 * are handed out. */
static void rng_refill(embi_rng *r)
{
    uint8_t nonce[12];
    SDL_memset(nonce, 0, sizeof nonce);
    store_le32(nonce, r->block);
    uint8_t blk[64];
    embi_chacha20_block(r->key, nonce, 1, blk);
    SDL_memcpy(r->key, blk, 32);
    SDL_memcpy(r->out, blk + 32, 32);
    SDL_memset(blk, 0, sizeof blk);
    r->block++;
    r->pos = 0;
}

void embi_rng_bytes(embi_rng *r, uint8_t *out, size_t n)
{
    if (!r->seeded) {
        embi_rng_seed(r);
    }
    size_t off = 0;
    while (off < n) {
        if (r->pos >= 32) {
            rng_refill(r);
        }
        size_t take = 32 - r->pos;
        if (take > n - off) {
            take = n - off;
        }
        SDL_memcpy(out + off, r->out + r->pos, take);
        r->pos += (uint32_t)take;
        off += take;
    }
}

void embi_rng_uuid(embi_rng *r, char out[37])
{
    uint8_t b[16];
    embi_rng_bytes(r, b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[o++] = '-';
        }
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 15];
    }
    out[o] = '\0';
}
