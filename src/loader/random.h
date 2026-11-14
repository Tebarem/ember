#pragma once
/*
 * ChaCha20 (RFC 8439) and the random source behind crypto.getRandomValues and crypto.randomUUID
 * (W7c). C only: no JS header.
 *
 * The random source is a ChaCha20 generator with fast key erasure: each block gives 32 output
 * bytes and the next key, so a later state compromise does not reveal earlier output. It is seeded
 * from a local SDL_rand_bits_r state mixed from the performance counter, the tick counter, the wall clock,
 * the thread id, a global counter and addresses (ASLR), then run through one ChaCha20 block. When an entropy source
 * is installed (embi_rng_set_entropy: module net installs the OS's secure random source, see src/net/platform.c),
 * 32 bytes from it are mixed in too, and then the output is as unpredictable as that source. Without one (EMBER_NET
 * off) it is not a certified DRBG: it is for page-visible randomness (Math-like use by scripts), not key generation.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ChaCha20 block function (RFC 8439 2.3): 64 bytes of keystream for key, nonce and counter. */
void embi_chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]);
/* Encrypts or decrypts n bytes of buf in place (RFC 8439 2.4). */
void embi_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *buf, size_t n);

typedef struct embi_rng {
    uint8_t key[32];
    uint32_t block;  /* blocks generated under the current key, used as the nonce */
    uint8_t out[32]; /* output bytes of the last block */
    uint32_t pos;    /* next unread byte of out; 32 means empty */
    bool seeded;
} embi_rng;

/* A secure entropy source: fills buf with len random bytes, or returns false. */
typedef bool (*embi_entropy_fn)(void *buf, size_t len);
/* Installs the source every later embi_rng_seed mixes in (process-wide; nullptr removes it). Thread-safe. */
void embi_rng_set_entropy(embi_entropy_fn fn);

/* Seeds the generator from the system sources. A zeroed value is not seeded until this runs. */
void embi_rng_seed(embi_rng *r);
/* Fills out with n random bytes; seeds the generator first when needed. */
void embi_rng_bytes(embi_rng *r, uint8_t *out, size_t n);
/* Writes an RFC 4122 version 4 UUID in lowercase hex, NUL-terminated (37 bytes). */
void embi_rng_uuid(embi_rng *r, char out[37]);
