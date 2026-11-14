/*
 * ChaCha20 known answers (RFC 8439 2.3.2 block function and 2.4.2 encryption), the xor round trip
 * across block boundaries, and the sanity of the random source behind crypto.getRandomValues and
 * randomUUID: seeded output is not constant, a UUID has the version 4 and variant 1 bits.
 */
#include "loader_test.h"

#include "loader/random.h"

#include <string.h>

static void key_0_to_31(uint8_t key[32])
{
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
}

/* RFC 8439 2.3.2: key 00..1f, nonce 00:00:00:09:00:00:00:4a:00:00:00:00, counter 1. */
EMB_TEST(chacha20_block_rfc8439_vector)
{
    static const uint8_t expect[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    uint8_t key[32];
    key_0_to_31(key);
    const uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[64];
    embi_chacha20_block(key, nonce, 1, out);
    EMB_CHECK(memcmp(out, expect, sizeof expect) == 0);
}

/* RFC 8439 2.4.2: the sunscreen plaintext (114 bytes), nonce 00:00:00:00:00:00:00:4a:00:00:00:00,
 * counter 1. The first 16 ciphertext bytes and the total length are checked, and decryption
 * restores the plaintext. */
EMB_TEST(chacha20_encrypt_rfc8439_vector)
{
    static const char text[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    size_t n = strlen(text);
    EMB_CHECK_EQ_INT((long long)n, 114);
    uint8_t key[32];
    key_0_to_31(key);
    const uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    uint8_t buf[128];
    memcpy(buf, text, n);
    embi_chacha20_xor(key, nonce, 1, buf, n);
    static const uint8_t head[16] = {0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80,
                                     0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81};
    EMB_CHECK(memcmp(buf, head, sizeof head) == 0);
    embi_chacha20_xor(key, nonce, 1, buf, n);
    EMB_CHECK(memcmp(buf, text, n) == 0);
}

/* The xor over a length that crosses two block boundaries equals the blockwise keystream. */
EMB_TEST(chacha20_xor_crosses_blocks)
{
    uint8_t key[32];
    key_0_to_31(key);
    const uint8_t nonce[12] = {0};
    uint8_t a[150];
    memset(a, 0, sizeof a);
    embi_chacha20_xor(key, nonce, 7, a, sizeof a);
    uint8_t ks[64];
    embi_chacha20_block(key, nonce, 7, ks);
    EMB_CHECK(memcmp(a, ks, 64) == 0);
    embi_chacha20_block(key, nonce, 9, ks);
    EMB_CHECK(memcmp(a + 128, ks, 22) == 0);
}

EMB_TEST(rng_bytes_are_not_constant)
{
    embi_rng r;
    memset(&r, 0, sizeof r);
    uint8_t a[64];
    uint8_t b[64];
    embi_rng_bytes(&r, a, sizeof a);
    embi_rng_bytes(&r, b, sizeof b);
    EMB_CHECK(memcmp(a, b, sizeof a) != 0);
    EMB_CHECK(r.seeded);
    /* Filling in pieces gives the same stream as one call (copies of one seeded state). */
    embi_rng whole;
    memset(&whole, 0, sizeof whole);
    embi_rng_seed(&whole);
    embi_rng pieces = whole;
    uint8_t c[100];
    uint8_t d[100];
    embi_rng_bytes(&whole, c, sizeof c);
    embi_rng_bytes(&pieces, d, 3);
    embi_rng_bytes(&pieces, d + 3, 61);
    embi_rng_bytes(&pieces, d + 64, sizeof d - 64);
    EMB_CHECK(memcmp(c, d, sizeof c) == 0);
}

static int g_entropy_calls;

static bool fixed_entropy(void *buf, size_t len)
{
    g_entropy_calls++;
    memset(buf, 0xA5, len);
    return true;
}

EMB_TEST(rng_seed_mixes_in_the_installed_entropy_source)
{
    g_entropy_calls = 0;
    embi_rng_set_entropy(fixed_entropy);
    embi_rng r;
    memset(&r, 0, sizeof r);
    embi_rng_seed(&r);
    EMB_CHECK(g_entropy_calls == 1);
    embi_rng_set_entropy(nullptr);
    embi_rng_seed(&r);
    EMB_CHECK(g_entropy_calls == 1);
}

EMB_TEST(rng_uuid_is_version4)
{
    embi_rng r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 16; i++) {
        char u[37];
        embi_rng_uuid(&r, u);
        EMB_CHECK_EQ_INT(strlen(u), 36);
        EMB_CHECK(u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-');
        EMB_CHECK(u[14] == '4');
        EMB_CHECK(u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
        for (int k = 0; k < 36; k++) {
            if (k == 8 || k == 13 || k == 18 || k == 23) {
                continue;
            }
            EMB_CHECK((u[k] >= '0' && u[k] <= '9') || (u[k] >= 'a' && u[k] <= 'f'));
        }
    }
}

LOADER_TEST_MAIN()
