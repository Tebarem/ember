#include "codec_test_util.h"

/* Each pair is a Brotli stream (tests/data/brotli/<comp>) and its original (<orig>). The fixture
 * README says every <name>.compressed[.NN] decodes to <name>. */
typedef struct bpair {
    const char *comp;
    const char *orig;
} bpair;

static const bpair kPairs[] = {
    {"10x10y.compressed", "10x10y"},
    {"64x.compressed", "64x"},
    {"alice29.txt.compressed", "alice29.txt"},
    {"asyoulik.txt.compressed", "asyoulik.txt"},
    {"backward65536.compressed", "backward65536"},
    {"compressed_file.compressed", "compressed_file"},
    {"compressed_repeated.compressed", "compressed_repeated"},
    {"cp1251-utf16le.compressed", "cp1251-utf16le"},
    {"cp852-utf8.compressed", "cp852-utf8"},
    {"empty.compressed", "empty"},
    {"empty.compressed.00", "empty"},
    {"empty.compressed.01", "empty"},
    {"empty.compressed.02", "empty"},
    {"empty.compressed.03", "empty"},
    {"empty.compressed.04", "empty"},
    {"empty.compressed.05", "empty"},
    {"empty.compressed.06", "empty"},
    {"empty.compressed.07", "empty"},
    {"empty.compressed.08", "empty"},
    {"empty.compressed.09", "empty"},
    {"empty.compressed.10", "empty"},
    {"empty.compressed.11", "empty"},
    {"empty.compressed.12", "empty"},
    {"empty.compressed.13", "empty"},
    {"empty.compressed.14", "empty"},
    {"empty.compressed.15", "empty"},
    {"empty.compressed.16", "empty"},
    {"empty.compressed.17", "empty"},
    {"empty.compressed.18", "empty"},
    {"monkey.compressed", "monkey"},
    {"quickfox.compressed", "quickfox"},
    {"quickfox_repeated.compressed", "quickfox_repeated"},
    {"random_org_10k.bin.compressed", "random_org_10k.bin"},
    {"ukkonooa.compressed", "ukkonooa"},
    {"x.compressed", "x"},
    {"x.compressed.00", "x"},
    {"x.compressed.01", "x"},
    {"x.compressed.02", "x"},
    {"x.compressed.03", "x"},
    {"xyzzy.compressed", "xyzzy"},
    {"zeros.compressed", "zeros"},
    {"zerosukkanooa.compressed", "zerosukkanooa"},
};

/* Decodes tests/data/brotli/<comp> with the output limit set to the exact original size and
 * compares the result with <orig>. Logs the failing name so a bad vector is easy to find. */
static void check_pair(const bpair *p)
{
    char rc[256];
    char ro[256];
    SDL_snprintf(rc, sizeof rc, "tests/data/brotli/%s", p->comp);
    SDL_snprintf(ro, sizeof ro, "tests/data/brotli/%s", p->orig);
    size_t cl = 0;
    size_t ol = 0;
    uint8_t *c = tu_load(rc, &cl);
    uint8_t *o = tu_load(ro, &ol);
    EMB_REQUIRE(c != nullptr);
    EMB_REQUIRE(o != nullptr || ol == 0);

    embi_buf out = {0};
    bool ok = embi_brotli_decode(c, cl, ol, &out);
    if (!ok) {
        SDL_Log("brotli decode failed: %s", p->comp);
    }
    EMB_CHECK(ok);
    EMB_CHECK(out.len == ol);
    if (ok && out.len == ol && ol > 0) {
        EMB_CHECK(SDL_memcmp(out.data, o, ol) == 0);
    }
    embi_buf_free(&out);
    SDL_free(c);
    SDL_free(o);
}

EMB_TEST(brotli_vectors_decode_to_originals)
{
    for (size_t i = 0; i < sizeof kPairs / sizeof kPairs[0]; i++) {
        check_pair(&kPairs[i]);
    }
}

EMB_TEST(brotli_output_limit_is_exact)
{
    size_t cl = 0;
    size_t ol = 0;
    uint8_t *c = tu_load("tests/data/brotli/alice29.txt.compressed", &cl);
    uint8_t *o = tu_load("tests/data/brotli/alice29.txt", &ol);
    EMB_REQUIRE(c != nullptr && o != nullptr);
    embi_buf out = {0};
    EMB_CHECK(!embi_brotli_decode(c, cl, ol - 1, &out));
    EMB_CHECK(embi_brotli_decode(c, cl, ol, &out));
    EMB_CHECK(out.len == ol);
    embi_buf_free(&out);
    SDL_free(c);
    SDL_free(o);
}

EMB_TEST(brotli_truncation_at_every_offset_fails)
{
    size_t cl = 0;
    uint8_t *c = tu_load("tests/data/brotli/quickfox.compressed", &cl);
    EMB_REQUIRE(c != nullptr);
    embi_buf out = {0};
    for (size_t k = 0; k < cl; k++) {
        EMB_CHECK(!embi_brotli_decode(c, k, 4096, &out));
    }
    embi_buf_free(&out);
    SDL_free(c);
}

EMB_TEST(brotli_random_byte_flips_never_crash)
{
    size_t cl = 0;
    uint8_t *c = tu_load("tests/data/brotli/monkey.compressed", &cl);
    EMB_REQUIRE(c != nullptr);
    tu_rng rng = {0x51ED270Bu};
    embi_buf out = {0};
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t *copy = embi_alloc(cl);
        SDL_memcpy(copy, c, cl);
        int flips = 1 + (int)(tu_rand(&rng) % 4);
        for (int f = 0; f < flips; f++) {
            size_t at = tu_rand(&rng) % cl;
            copy[at] ^= (uint8_t)(1u + tu_rand(&rng) % 255u);
        }
        /* Either outcome is acceptable; the output must respect the limit. */
        bool ok = embi_brotli_decode(copy, cl, 4096, &out);
        EMB_CHECK(!ok || out.len <= 4096);
        embi_free(copy, cl);
    }
    embi_buf_free(&out);
    SDL_free(c);
}

EMB_TEST(brotli_truncated_empty_streams_fail)
{
    /* An empty stream is a single byte. Anything shorter is not a complete stream. */
    static const uint8_t none[] = {0x06};
    embi_buf out = {0};
    EMB_CHECK(embi_brotli_decode(none, 1, 0, &out));
    EMB_CHECK(out.len == 0);
    EMB_CHECK(!embi_brotli_decode(none, 0, 16, &out));
    embi_buf_free(&out);
}

EMB_TEST(brotli_rejects_invalid_wbits)
{
    /* First bit 1, three bits 000 (n = 0), then three bits 001 (m = 1): invalid WBITS. */
    static const uint8_t bad_wbits[] = {0x11, 0x00, 0x00, 0x00};
    embi_buf out = {0};
    EMB_CHECK(!embi_brotli_decode(bad_wbits, sizeof bad_wbits, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST(brotli_handbuilt_uncompressed_metablock)
{
    tu_bits w = {0};
    tu_put_bits(&w, 0, 1);  /* WBITS 16 */
    tu_put_bits(&w, 0, 1);  /* ISLAST = 0 */
    tu_put_bits(&w, 0, 2);  /* MNIBBLES = 4 */
    tu_put_bits(&w, 4, 16); /* MLEN - 1 = 4: five bytes */
    tu_put_bits(&w, 1, 1);  /* ISUNCOMPRESSED */
    tu_bits_finish(&w);     /* pad to a byte boundary */
    const char *text = "hello";
    for (int i = 0; i < 5; i++) {
        tu_put_bits(&w, (uint8_t)text[i], 8);
    }
    tu_put_bits(&w, 1, 1); /* ISLAST */
    tu_put_bits(&w, 1, 1); /* ISLASTEMPTY */
    size_t n = tu_bits_finish(&w);

    embi_buf out = {0};
    EMB_CHECK(embi_brotli_decode(w.buf, n, 64, &out));
    EMB_CHECK(out.len == 5);
    EMB_CHECK(out.len == 5 && SDL_memcmp(out.data, "hello", 5) == 0);
    embi_buf_free(&out);
}

EMB_TEST(brotli_handbuilt_metadata_metablock_is_skipped)
{
    tu_bits w = {0};
    tu_put_bits(&w, 0, 1);  /* WBITS 16 */
    tu_put_bits(&w, 0, 1);  /* ISLAST = 0 */
    tu_put_bits(&w, 3, 2);  /* MNIBBLES code 3: metadata */
    tu_put_bits(&w, 0, 1);  /* reserved, must be zero */
    tu_put_bits(&w, 1, 2);  /* MSKIPBYTES = 1 */
    tu_put_bits(&w, 2, 8);  /* MSKIPLEN - 1 = 2: three skipped bytes */
    tu_bits_finish(&w);
    for (int i = 0; i < 3; i++) {
        tu_put_bits(&w, 0xAA, 8);
    }
    tu_put_bits(&w, 1, 1); /* ISLAST */
    tu_put_bits(&w, 1, 1); /* ISLASTEMPTY */
    size_t n = tu_bits_finish(&w);

    embi_buf out = {0};
    EMB_CHECK(embi_brotli_decode(w.buf, n, 64, &out));
    EMB_CHECK(out.len == 0);
    embi_buf_free(&out);
}

EMB_TEST(brotli_dictionary_matches_rfc_crc)
{
    /* RFC 7932 Appendix A: CRC-32 of the 122784-byte static dictionary. */
    const uint8_t *dict = embi_brotli_dict_open();
    EMB_REQUIRE(dict != nullptr);
    EMB_CHECK(tu_crc32(dict, EMBI_BROTLI_DICT_SIZE) == 0x5136cb04u);
    embi_brotli_dict_close(dict);
}

EMB_TEST(brotli_transform_table_basics)
{
    /* Transform 0 is Identity with no affixes; transform 1 appends a space (RFC 7932 Appendix B). */
    EMB_CHECK(embi_brotli_transforms[0].type == 0);
    EMB_CHECK_EQ_STR(embi_brotli_transforms[0].prefix, "");
    EMB_CHECK_EQ_STR(embi_brotli_transforms[0].suffix, "");
    EMB_CHECK_EQ_STR(embi_brotli_transforms[1].suffix, " ");
}

EMB_TEST(brotli_transform_table_crc_matches_rfc)
{
    /* Appendix B serialization: per transform, the prefix, 0x00, the type byte, the suffix, 0x00. */
    uint8_t buf[1024];
    size_t n = 0;
    for (unsigned t = 0; t < EMBI_BROTLI_NUM_TRANSFORMS; t++) {
        const embi_brotli_transform *x = &embi_brotli_transforms[t];
        size_t pl = SDL_strlen(x->prefix);
        size_t sl = SDL_strlen(x->suffix);
        EMB_REQUIRE(n + pl + sl + 3 <= sizeof buf);
        SDL_memcpy(buf + n, x->prefix, pl);
        n += pl;
        buf[n++] = 0;
        buf[n++] = x->type;
        SDL_memcpy(buf + n, x->suffix, sl);
        n += sl;
        buf[n++] = 0;
    }
    EMB_CHECK(n == 648);
    EMB_CHECK(tu_crc32(buf, n) == 0x3d965f81u);
}

/* Builds a Brotli stream with one metablock and one command: insert 0 literals, copy copy_len
 * bytes (4..9) from the static dictionary. With no output before the command the maximum backward
 * distance is 0, so distance = word_id + 1 names dictionary word word_id. Every prefix code is a
 * single-symbol simple code (zero bits per symbol), so only the extra bits take space. Returns the
 * stream length, or 0 when the distance cannot be encoded. */
static size_t brotli_dict_ref_stream(uint32_t copy_len, uint32_t word_id, uint32_t mlen, uint8_t *dst)
{
    uint32_t dcode = 0;
    uint32_t dbits = 0;
    uint32_t dextra = 0;
    bool found = false;
    /* Distance codes 16.. with NPOSTFIX = 0 and NDIRECT = 0 (RFC 7932 4). */
    for (uint32_t c = 16; c < 64 && !found; c++) {
        uint32_t nb = 1 + ((c - 16) >> 1);
        uint32_t off = ((2 + ((c - 16) & 1)) << nb) - 4;
        if (word_id >= off && word_id - off < (1u << nb)) {
            dcode = c;
            dbits = nb;
            dextra = word_id - off;
            found = true;
        }
    }
    if (!found || copy_len < 4 || copy_len > 9) {
        return 0;
    }

    tu_bits w = {0};
    tu_put_bits(&w, 0, 1);            /* WBITS 16 */
    tu_put_bits(&w, 1, 1);            /* ISLAST */
    tu_put_bits(&w, 0, 1);            /* ISLASTEMPTY = 0 */
    tu_put_bits(&w, 0, 2);            /* MNIBBLES = 4 */
    tu_put_bits(&w, mlen - 1, 16);    /* MLEN - 1 */
    tu_put_bits(&w, 0, 1);            /* NBLTYPESL = 1 */
    tu_put_bits(&w, 0, 1);            /* NBLTYPESI = 1 */
    tu_put_bits(&w, 0, 1);            /* NBLTYPESD = 1 */
    tu_put_bits(&w, 0, 2);            /* NPOSTFIX = 0 */
    tu_put_bits(&w, 0, 4);            /* NDIRECT = 0 */
    tu_put_bits(&w, 0, 2);            /* context mode of literal block type 0 */
    tu_put_bits(&w, 0, 1);            /* NTREESL = 1 */
    tu_put_bits(&w, 0, 1);            /* NTREESD = 1 */
    /* Literal code: simple, one symbol of 8 bits. */
    tu_put_bits(&w, 1, 2);
    tu_put_bits(&w, 0, 2);
    tu_put_bits(&w, 0, 8);
    /* Insert-and-copy code: simple, one symbol of 10 bits. Cell 2 (insert 0, explicit distance)
     * with copy code copy_len - 2 gives symbol 128 + copy code. */
    tu_put_bits(&w, 1, 2);
    tu_put_bits(&w, 0, 2);
    tu_put_bits(&w, 128 + (copy_len - 2), 10);
    /* Distance code: simple, one symbol of 6 bits. */
    tu_put_bits(&w, 1, 2);
    tu_put_bits(&w, 0, 2);
    tu_put_bits(&w, dcode, 6);
    /* The command: zero-bit codes, insert code 0 and copy code without extra bits, then the
     * distance extra bits. */
    tu_put_bits(&w, dextra, dbits);
    size_t n = tu_bits_finish(&w);
    SDL_memcpy(dst, w.buf, n);
    return n;
}

/* Finds the first dictionary word of the given length whose bytes are all lowercase ASCII. */
static bool brotli_find_lower_word(const uint8_t *dict, uint32_t len, uint32_t *idx)
{
    uint32_t count = 1u << embi_brotli_ndbits[len];
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *w = dict + embi_brotli_doffset[len] + (size_t)i * len;
        bool lower = true;
        for (uint32_t k = 0; k < len; k++) {
            lower = lower && w[k] >= 'a' && w[k] <= 'z';
        }
        if (lower) {
            *idx = i;
            return true;
        }
    }
    return false;
}

/* RFC 7932 Appendix B, written independently of the decoder: affixes around the word after the
 * omit, with ASCII case changes (FermentFirst uppercases the first byte, FermentAll every byte). */
static size_t brotli_expect_transform(const embi_brotli_transform *t, const uint8_t *word, uint32_t len,
                                      uint8_t *out)
{
    uint32_t start = 0;
    uint32_t wl = len;
    if (t->type >= 3 && t->type <= 11) {
        start = t->type - 2u; /* OmitFirst1..9 */
        wl -= start;
    } else if (t->type >= 12) {
        wl -= t->type - 11u; /* OmitLast1..9 */
    }
    size_t n = 0;
    SDL_memcpy(out + n, t->prefix, SDL_strlen(t->prefix));
    n += SDL_strlen(t->prefix);
    SDL_memcpy(out + n, word + start, wl);
    if (t->type == 1 && wl > 0 && out[n] >= 'a' && out[n] <= 'z') {
        out[n] ^= 32;
    }
    if (t->type == 2) {
        for (uint32_t k = 0; k < wl; k++) {
            if (out[n + k] >= 'a' && out[n + k] <= 'z') {
                out[n + k] ^= 32;
            }
        }
    }
    n += wl;
    SDL_memcpy(out + n, t->suffix, SDL_strlen(t->suffix));
    n += SDL_strlen(t->suffix);
    return n;
}

/* Decodes a dictionary reference through transform tid and compares with the expected bytes. */
static void brotli_check_dict_transform(uint32_t tid, uint32_t len)
{
    uint32_t idx = 0;
    const uint8_t *dict = embi_brotli_dict_open();
    EMB_REQUIRE(dict != nullptr);
    if (!brotli_find_lower_word(dict, len, &idx)) {
        embi_brotli_dict_close(dict);
        EMB_REQUIRE(false);
    }
    uint32_t word_id = (tid << embi_brotli_ndbits[len]) | idx;
    const uint8_t *word = dict + embi_brotli_doffset[len] + (size_t)idx * len;

    uint8_t expect[64];
    size_t mlen = brotli_expect_transform(&embi_brotli_transforms[tid], word, len, expect);
    embi_brotli_dict_close(dict);
    EMB_REQUIRE(mlen > 0);

    uint8_t stream[64];
    size_t sn = brotli_dict_ref_stream(len, word_id, (uint32_t)mlen, stream);
    EMB_REQUIRE(sn > 0);

    embi_buf out = {0};
    bool ok = embi_brotli_decode(stream, sn, 64, &out);
    if (!ok) {
        SDL_Log("dictionary transform %u (type %u) failed", (unsigned)tid, (unsigned)embi_brotli_transforms[tid].type);
    }
    EMB_CHECK(ok);
    EMB_CHECK(out.len == mlen);
    EMB_CHECK(out.len == mlen && SDL_memcmp(out.data, expect, mlen) == 0);
    embi_buf_free(&out);
}

EMB_TEST(brotli_dict_ref_ferment_first_transform)
{
    /* Transform 9: FermentFirst, no affixes. Transform 15: FermentFirst with affixes. */
    brotli_check_dict_transform(9, 6);
    brotli_check_dict_transform(15, 7);
}

EMB_TEST(brotli_dict_ref_ferment_all_transform)
{
    /* Transform 83: FermentAll with a space on each side. */
    brotli_check_dict_transform(83, 6);
}

EMB_TEST(brotli_dict_ref_omit_first_transform)
{
    /* Transform 3: OmitFirst1. */
    brotli_check_dict_transform(3, 6);
}

EMB_TEST(brotli_dict_ref_omit_last_transform)
{
    /* Transform 12: OmitLast1. */
    brotli_check_dict_transform(12, 6);
}

EMB_TEST(brotli_dict_ref_unknown_transform_rejected)
{
    /* Transform ids run 0..120; id 121 names nothing and must fail. */
    uint32_t idx = 0;
    const uint8_t *dict = embi_brotli_dict_open();
    EMB_REQUIRE(dict != nullptr);
    bool found = brotli_find_lower_word(dict, 6, &idx);
    embi_brotli_dict_close(dict);
    EMB_REQUIRE(found);
    uint8_t stream[64];
    size_t sn = brotli_dict_ref_stream(6, (121u << embi_brotli_ndbits[6]) | idx, 6, stream);
    EMB_REQUIRE(sn > 0);
    embi_buf out = {0};
    EMB_CHECK(!embi_brotli_decode(stream, sn, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST_MAIN()
