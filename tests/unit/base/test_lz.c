/*
 * The LZ decoder (src/base/lz.c) against the build-time encoder (tools/lz/lz_encode.h): round trips of text, runs,
 * incompressible bytes and long matches, and refusal of damaged streams.
 */
#include "emb_test.h"

#include "base/base.h"
#include "../../../tools/lz/lz_encode.h"

static bool roundtrip(const uint8_t *in, size_t n)
{
    uint8_t *packed = malloc(lze_bound(n));
    size_t pn = lze_encode(in, n, packed);
    uint8_t *out = malloc(n + 1);
    bool ok = embi_lz_decode(packed, pn, out, n) && (n == 0 || memcmp(in, out, n) == 0);
    free(packed);
    free(out);
    return ok;
}

EMB_TEST(lz_empty_and_tiny)
{
    uint8_t one = 'x';
    EMB_CHECK(roundtrip(&one, 0));
    EMB_CHECK(roundtrip(&one, 1));
    const uint8_t abc[] = "abc";
    EMB_CHECK(roundtrip(abc, 3));
}

EMB_TEST(lz_runs_and_overlap)
{
    uint8_t buf[70000];
    memset(buf, 'a', sizeof buf);
    EMB_CHECK(roundtrip(buf, sizeof buf));
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)("abcabcab"[i % 8]);
    }
    EMB_CHECK(roundtrip(buf, sizeof buf));
}

EMB_TEST(lz_text_and_noise)
{
    const char *text = "the quick brown fox jumps over the lazy dog; the quick brown fox jumps again. ";
    size_t tl = strlen(text);
    uint8_t buf[20000];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)text[i % tl];
    }
    EMB_CHECK(roundtrip(buf, sizeof buf));
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof buf; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
    EMB_CHECK(roundtrip(buf, sizeof buf));
    /* noise with a far repeat: a match offset near the 64 KB limit */
    uint8_t *big = malloc(140000);
    for (size_t i = 0; i < 70000; i++) {
        x = x * 1664525u + 1013904223u;
        big[i] = (uint8_t)(x >> 24);
    }
    memcpy(big + 70000, big + 5000, 60000);
    EMB_CHECK(roundtrip(big, 130000));
    free(big);
}

EMB_TEST(lz_damaged_streams_fail)
{
    uint8_t in[2000];
    for (size_t i = 0; i < sizeof in; i++) {
        in[i] = (uint8_t)(i % 7);
    }
    uint8_t packed[4000], out[2000];
    size_t pn = lze_encode(in, sizeof in, packed);
    EMB_CHECK(embi_lz_decode(packed, pn, out, sizeof out));
    EMB_CHECK(!embi_lz_decode(packed, pn / 2, out, sizeof out));      /* truncated */
    EMB_CHECK(!embi_lz_decode(packed, pn, out, sizeof out - 1));      /* output too small */
    EMB_CHECK(!embi_lz_decode(packed, pn, out, sizeof out + 1));      /* stream ends early */
    const uint8_t bad_offset[] = {0x04, 'a', 0x00, 0x00};             /* offset 0 */
    EMB_CHECK(!embi_lz_decode(bad_offset, sizeof bad_offset, out, 9));
    const uint8_t far_offset[] = {0x04, 'a', 0x09, 0x00};             /* before the start */
    EMB_CHECK(!embi_lz_decode(far_offset, sizeof far_offset, out, 9));
    for (size_t k = 0; k < pn; k++) {                                 /* flipped bytes never overrun (ASan) */
        uint8_t copy[4000];
        memcpy(copy, packed, pn);
        copy[k] ^= 0xA5;
        (void)embi_lz_decode(copy, pn, out, sizeof out);
    }
}

EMB_TEST(lz_unpack_reads_the_size_header_and_nul_terminates)
{
    const uint8_t in[] = "hello hello hello hello";
    size_t raw = sizeof in - 1;
    uint8_t packed[100] = {(uint8_t)raw, 0, 0, 0};
    size_t pn = 4 + lze_encode(in, raw, packed + 4);
    EMB_CHECK_EQ_INT(embi_lz_raw_size(packed, pn), raw);
    size_t base = embi_mem_live();
    size_t got = 0;
    uint8_t *e = embi_lz_unpack(packed, pn, &got);
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK_EQ_INT(got, raw);
    EMB_CHECK_EQ_INT(strcmp((const char *)e, "hello hello hello hello"), 0);
    embi_free(e, raw + 1);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
    EMB_CHECK(embi_lz_unpack(packed, 4 + (pn - 4) / 2, nullptr) == nullptr);
    EMB_CHECK(embi_lz_unpack(packed, 3, nullptr) == nullptr);
    const uint8_t absurd[] = {0xff, 0xff, 0xff, 0x7f, 0x10, 'a', 0, 0}; /* claims 2 GB from 4 bytes of stream */
    EMB_CHECK(embi_lz_unpack(absurd, sizeof absurd, nullptr) == nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST_MAIN()
