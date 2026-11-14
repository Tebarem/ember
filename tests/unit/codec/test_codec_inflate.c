#include "codec_test_util.h"

static const char *kCases[] = {"stored", "fixed", "dynamic", "huffman_only", "rle", "empty", "tiny", "maxdist"};

/* Loads tests/data/deflate/<name><ext>. Sets *len; the pointer may be null for an empty file. */
static uint8_t *load_case(const char *name, const char *ext, size_t *len)
{
    char rel[256];
    SDL_snprintf(rel, sizeof rel, "tests/data/deflate/%s%s", name, ext);
    return tu_load(rel, len);
}

/* Decodes with the given chunking. in_chunk and out_chunk bound each call. Returns the last status
 * and appends the produced bytes to out. */
static embi_inflate_status stream_decode(const uint8_t *in, size_t n, bool zlib, size_t max_out, size_t in_chunk,
                                         size_t out_chunk, embi_buf *out)
{
    embi_inflater *z = embi_inflater_new(zlib, max_out);
    uint8_t *obuf = embi_alloc(out_chunk);
    size_t pos = 0;
    embi_inflate_status st = EMBI_INFLATE_NEED_INPUT;
    for (size_t iter = 0; iter < 50000000u; iter++) {
        size_t avail = n - pos;
        size_t feed = TU_MIN(avail, in_chunk);
        size_t used = 0;
        size_t produced = 0;
        st = embi_inflate(z, in + pos, feed, &used, obuf, out_chunk, &produced);
        pos += used;
        if (!embi_buf_append(out, obuf, produced)) {
            st = EMBI_INFLATE_ERROR;
            break;
        }
        if (st == EMBI_INFLATE_DONE || st == EMBI_INFLATE_ERROR) {
            break;
        }
        if (st == EMBI_INFLATE_NEED_INPUT && pos == n) {
            break; /* truncated input */
        }
    }
    embi_free(obuf, out_chunk);
    embi_inflater_free(z);
    return st;
}

EMB_TEST(inflate_vectors_one_shot_zlib_and_raw)
{
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        size_t zl = 0;
        size_t rl = 0;
        size_t ol = 0;
        uint8_t *zs = load_case(kCases[i], ".zlib", &zl);
        uint8_t *rw = load_case(kCases[i], ".raw", &rl);
        uint8_t *ex = load_case(kCases[i], ".out", &ol);
        EMB_REQUIRE(zs != nullptr && rw != nullptr);
        EMB_REQUIRE(ex != nullptr || ol == 0);

        embi_buf out = {0};
        EMB_CHECK(embi_inflate_all(zs, zl, true, ol, &out));
        EMB_CHECK(out.len == ol);
        EMB_CHECK(ol == 0 || SDL_memcmp(out.data, ex, ol) == 0);
        embi_buf_free(&out);

        EMB_CHECK(embi_inflate_all(rw, rl, false, ol, &out));
        EMB_CHECK(out.len == ol);
        EMB_CHECK(ol == 0 || SDL_memcmp(out.data, ex, ol) == 0);
        embi_buf_free(&out);

        /* The output limit is exact: one byte less must fail when there is output. */
        if (ol > 0) {
            EMB_CHECK(!embi_inflate_all(zs, zl, true, ol - 1, &out));
            embi_buf_free(&out);
            EMB_CHECK(!embi_inflate_all(rw, rl, false, ol - 1, &out));
            embi_buf_free(&out);
        }
        SDL_free(zs);
        SDL_free(rw);
        SDL_free(ex);
    }
}

EMB_TEST(inflate_streaming_matches_one_shot)
{
    /* Input one byte at a time, output in 5-byte pieces: every state must resume correctly. */
    static const size_t kInChunks[] = {1, 2, 7, 4096};
    static const size_t kOutChunks[] = {1, 5, 333};
    const char *names[] = {"dynamic", "fixed", "stored", "maxdist"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t zl = 0;
        size_t ol = 0;
        uint8_t *zs = load_case(names[i], ".zlib", &zl);
        uint8_t *ex = load_case(names[i], ".out", &ol);
        EMB_REQUIRE(zs != nullptr && ex != nullptr);
        for (size_t a = 0; a < sizeof kInChunks / sizeof kInChunks[0]; a++) {
            for (size_t b = 0; b < sizeof kOutChunks / sizeof kOutChunks[0]; b++) {
                embi_buf out = {0};
                embi_inflate_status st = stream_decode(zs, zl, true, ol, kInChunks[a], kOutChunks[b], &out);
                EMB_CHECK(st == EMBI_INFLATE_DONE);
                EMB_CHECK(out.len == ol);
                EMB_CHECK(ol == 0 || SDL_memcmp(out.data, ex, ol) == 0);
                embi_buf_free(&out);
            }
        }
        SDL_free(zs);
        SDL_free(ex);
    }
}

EMB_TEST(inflate_output_space_reports_ok)
{
    size_t zl = 0;
    size_t ol = 0;
    uint8_t *zs = load_case("dynamic", ".zlib", &zl);
    uint8_t *ex = load_case("dynamic", ".out", &ol);
    EMB_REQUIRE(zs != nullptr && ex != nullptr);
    embi_inflater *z = embi_inflater_new(true, ol);
    uint8_t out[10];
    size_t used = 0;
    size_t produced = 0;
    embi_inflate_status st = embi_inflate(z, zs, zl, &used, out, sizeof out, &produced);
    EMB_CHECK(st == EMBI_INFLATE_OK);
    EMB_CHECK(produced == sizeof out);
    EMB_CHECK(SDL_memcmp(out, ex, sizeof out) == 0);
    embi_inflater_free(z);
    SDL_free(zs);
    SDL_free(ex);
}

EMB_TEST(inflate_truncation_at_every_offset_fails)
{
    size_t zl = 0;
    size_t rl = 0;
    uint8_t *zs = load_case("tiny", ".zlib", &zl);
    uint8_t *rw = load_case("tiny", ".raw", &rl);
    EMB_REQUIRE(zs != nullptr && rw != nullptr);
    embi_buf out = {0};
    for (size_t k = 0; k < zl; k++) {
        EMB_CHECK(!embi_inflate_all(zs, k, true, 4096, &out));
    }
    for (size_t k = 0; k < rl; k++) {
        EMB_CHECK(!embi_inflate_all(rw, k, false, 4096, &out));
    }
    embi_buf_free(&out);
    SDL_free(zs);
    SDL_free(rw);
}

EMB_TEST(inflate_random_byte_flips_never_crash)
{
    size_t zl = 0;
    size_t rl = 0;
    uint8_t *zs = load_case("tiny", ".zlib", &zl);
    uint8_t *rw = load_case("tiny", ".raw", &rl);
    EMB_REQUIRE(zs != nullptr && rw != nullptr);
    tu_rng rng = {0x9E3779B9u};
    embi_buf out = {0};
    for (int iter = 0; iter < 3000; iter++) {
        uint8_t *buf = iter & 1 ? rw : zs;
        size_t n = iter & 1 ? rl : zl;
        uint8_t *copy = embi_alloc(n);
        SDL_memcpy(copy, buf, n);
        int flips = 1 + (int)(tu_rand(&rng) % 4);
        for (int f = 0; f < flips; f++) {
            size_t at = tu_rand(&rng) % n;
            copy[at] ^= (uint8_t)(1u + tu_rand(&rng) % 255u);
        }
        /* Either outcome is fine; the requirement is no crash and output within the limit. */
        bool ok = embi_inflate_all(copy, n, (iter & 1) == 0, 4096, &out);
        EMB_CHECK(!ok || out.len <= 4096);
        embi_free(copy, n);
    }
    embi_buf_free(&out);
    SDL_free(zs);
    SDL_free(rw);
}

EMB_TEST(inflate_zlib_checksum_mismatch_rejected)
{
    size_t zl = 0;
    uint8_t *zs = load_case("tiny", ".zlib", &zl);
    EMB_REQUIRE(zs != nullptr);
    zs[zl - 1] ^= 0x01;
    embi_buf out = {0};
    EMB_CHECK(!embi_inflate_all(zs, zl, true, 4096, &out));
    embi_buf_free(&out);
    SDL_free(zs);
}

EMB_TEST(inflate_rejects_bad_zlib_header)
{
    static const uint8_t bad_method[] = {0x79, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t bad_check[] = {0x78, 0x9d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t preset_dict[] = {0x78, 0xbb, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    embi_buf out = {0};
    EMB_CHECK(!embi_inflate_all(bad_method, sizeof bad_method, true, 64, &out));
    EMB_CHECK(!embi_inflate_all(bad_check, sizeof bad_check, true, 64, &out));
    EMB_CHECK(!embi_inflate_all(preset_dict, sizeof preset_dict, true, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST(inflate_stored_block_length_check)
{
    static const uint8_t good[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    static const uint8_t bad_nlen[] = {0x01, 0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'};
    embi_buf out = {0};
    EMB_CHECK(embi_inflate_all(good, sizeof good, false, 64, &out));
    EMB_CHECK(out.len == 3 && out.data[0] == 'a' && out.data[2] == 'c');
    EMB_CHECK(!embi_inflate_all(bad_nlen, sizeof bad_nlen, false, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST(inflate_rejects_reserved_block_type)
{
    static const uint8_t reserved[] = {0x07, 0x00, 0x00};
    embi_buf out = {0};
    EMB_CHECK(!embi_inflate_all(reserved, sizeof reserved, false, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST(inflate_rejects_distance_before_start)
{
    /* Fixed block: literal 'a', then length 3 at distance 2. Only one byte exists, so it must fail. */
    tu_bits w = {0};
    tu_put_bits(&w, 1, 1); /* BFINAL */
    tu_put_bits(&w, 1, 2); /* fixed Huffman */
    tu_put_fixed_litlen(&w, 'a');
    tu_put_fixed_litlen(&w, 257); /* length 3 */
    tu_put_code(&w, 1, 5);        /* distance symbol 1: distance 2 */
    tu_put_fixed_litlen(&w, 256);
    size_t n = tu_bits_finish(&w);
    embi_buf out = {0};
    EMB_CHECK(!embi_inflate_all(w.buf, n, false, 64, &out));
    embi_buf_free(&out);
}

EMB_TEST(inflate_fixed_block_with_overlap_copy)
{
    /* Literal 'a', then length 4 at distance 1, then end: "aaaaa". */
    tu_bits w = {0};
    tu_put_bits(&w, 1, 1);
    tu_put_bits(&w, 1, 2);
    tu_put_fixed_litlen(&w, 'a');
    tu_put_fixed_litlen(&w, 258); /* length 4 */
    tu_put_code(&w, 0, 5);        /* distance 1 */
    tu_put_fixed_litlen(&w, 256);
    size_t n = tu_bits_finish(&w);
    embi_buf out = {0};
    EMB_CHECK(embi_inflate_all(w.buf, n, false, 64, &out));
    EMB_CHECK(out.len == 5);
    EMB_CHECK(out.len == 5 && out.data[4] == 'a');
    embi_buf_free(&out);
}

EMB_TEST(inflate_done_stops_before_trailing_input)
{
    size_t rl = 0;
    uint8_t *rw = load_case("tiny", ".raw", &rl);
    EMB_REQUIRE(rw != nullptr);
    uint8_t *padded = embi_alloc(rl + 16);
    SDL_memcpy(padded, rw, rl);
    SDL_memset(padded + rl, 0xA5, 16);
    embi_inflater *z = embi_inflater_new(false, 4096);
    uint8_t out[512];
    size_t used = 0;
    size_t produced = 0;
    embi_inflate_status st = embi_inflate(z, padded, rl + 16, &used, out, sizeof out, &produced);
    EMB_CHECK(st == EMBI_INFLATE_DONE);
    EMB_CHECK(produced == 300);
    EMB_CHECK(used <= rl + 1);
    embi_inflater_free(z);
    embi_free(padded, rl + 16);
    SDL_free(rw);
}

/* After an error the inflater stays failed, even when valid input follows. */
EMB_TEST(inflate_error_is_sticky)
{
    embi_inflater *z = embi_inflater_new(false, 1024);
    static const uint8_t bad[] = {0x07}; /* BFINAL = 1, BTYPE = 3 (reserved) */
    static const uint8_t empty[] = {0x03, 0x00}; /* a valid empty fixed-Huffman stream */
    uint8_t out[16];
    size_t used = 0;
    size_t produced = 0;
    EMB_CHECK(embi_inflate(z, bad, sizeof bad, &used, out, sizeof out, &produced) == EMBI_INFLATE_ERROR);
    EMB_CHECK(embi_inflate(z, empty, sizeof empty, &used, out, sizeof out, &produced) == EMBI_INFLATE_ERROR);
    EMB_CHECK(produced == 0);
    embi_inflater_free(z);
}

EMB_TEST_MAIN()
