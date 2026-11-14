#include "codec_test_util.h"

/* Big-endian writers for synthetic WOFF files. */
static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static size_t align4(size_t x)
{
    return (x + 3) & ~(size_t)3;
}

typedef struct wtab {
    uint32_t tag;
    const uint8_t *comp; /* stored bytes: raw when clen == olen, zlib otherwise */
    size_t clen;
    size_t olen;
    uint32_t csum;
} wtab;

/* Builds a WOFF 1.0 file in o (capacity cap). Tables must be sorted by tag. Returns the file length,
 * or 0 when it does not fit. */
static size_t woff1_build(uint8_t *o, size_t cap, uint32_t flavor, const wtab *t, size_t n)
{
    if (n == 0 || n > 16) {
        return 0;
    }
    size_t offs[16];
    size_t pos = align4(44 + 20 * n);
    size_t sfnt = 12 + 16 * n;
    for (size_t i = 0; i < n; i++) {
        offs[i] = pos;
        pos += align4(t[i].clen);
        sfnt += align4(t[i].olen);
    }
    if (pos > cap) {
        return 0;
    }
    SDL_memset(o, 0, pos);
    w32(o, 0x774F4646u); /* 'wOFF' */
    w32(o + 4, flavor);
    w32(o + 8, (uint32_t)pos);
    w16(o + 12, (uint16_t)n);
    w32(o + 16, (uint32_t)sfnt);
    w16(o + 20, 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = o + 44 + 20 * i;
        w32(e, t[i].tag);
        w32(e + 4, (uint32_t)offs[i]);
        w32(e + 8, (uint32_t)t[i].clen);
        w32(e + 12, (uint32_t)t[i].olen);
        w32(e + 16, t[i].csum);
        SDL_memcpy(o + offs[i], t[i].comp, t[i].clen);
    }
    return pos;
}

/* Compares every table of the reference sfnt f with the same tag in the decoded font. */
static void compare_tables(const uint8_t *dec, size_t dec_len, const uint8_t *f, size_t fl)
{
    uint32_t nt = tu_be16(f + 4);
    EMB_CHECK(tu_be16(dec + 4) == nt);
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t tag = tu_be32(f + 12 + 16 * i);
        const uint8_t *rd = nullptr;
        const uint8_t *od = nullptr;
        size_t rl = 0;
        size_t ol = 0;
        bool have_ref = tu_sfnt_table(f, fl, tag, &rd, &rl);
        bool have_out = tu_sfnt_table(dec, dec_len, tag, &od, &ol);
        EMB_CHECK(have_ref && have_out);
        if (have_ref && have_out) {
            EMB_CHECK(rl == ol);
            /* head checkSumAdjustment (8..11) is recomputed, and head.modified (28..35) differs between
             * the WOFF and TTF builds of the fixtures, so both are skipped. */
            for (size_t at = 0; at < rl; at++) {
                if (tag == tu_tag("head") && ((at >= 8 && at < 12) || (at >= 28 && at < 36))) {
                    continue;
                }
                if (rd[at] != od[at]) {
                    SDL_Log("table %c%c%c%c differs at byte %llu of %llu", (char)(tag >> 24), (char)(tag >> 16),
                            (char)(tag >> 8), (char)tag, (unsigned long long)at, (unsigned long long)rl);
                    EMB_CHECK(false);
                    break;
                }
            }
        }
    }
}

/* Decodes a real WOFF 1.0 font and compares it table by table with its sfnt original. */
static void check_font(const char *woff_rel, const char *sfnt_rel)
{
    size_t wl = 0;
    size_t fl = 0;
    uint8_t *w = tu_load(woff_rel, &wl);
    uint8_t *f = tu_load(sfnt_rel, &fl);
    EMB_REQUIRE(w != nullptr && f != nullptr);

    embi_buf out = {0};
    bool ok = embi_woff1_decode(w, wl, fl * 2, &out);
    if (!ok) {
        SDL_Log("woff1 decode failed: %s", woff_rel);
    }
    EMB_CHECK(ok);
    if (ok) {
        compare_tables(out.data, out.len, f, fl);
        /* A complete sfnt sums to 0xB1B0AFBA, so the head adjustment must be right. */
        EMB_CHECK(embi_sfnt_checksum(out.data, out.len) == 0xB1B0AFBAu);
    }
    embi_buf_free(&out);
    SDL_free(w);
    SDL_free(f);
}

EMB_TEST(woff1_noto_sans_matches_ttf)
{
    check_font("tests/fonts/NotoSans-Regular.woff", "tests/fonts/NotoSans-Regular.ttf");
}

EMB_TEST(woff1_source_sans_matches_otf)
{
    check_font("tests/fonts/SourceSans3-Regular.woff", "tests/fonts/SourceSans3-Regular.otf");
}

EMB_TEST(woff1_synthetic_stored_and_zlib_tables)
{
    uint8_t head[54];
    SDL_memset(head, 0, sizeof head);
    w32(head, 0x00010000u);
    for (size_t i = 12; i < sizeof head; i++) {
        head[i] = (uint8_t)(i * 7);
    }
    static const uint8_t test_data[10] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

    size_t zl = 0;
    size_t ol = 0;
    uint8_t *zs = tu_load("tests/data/deflate/tiny.zlib", &zl);
    uint8_t *ex = tu_load("tests/data/deflate/tiny.out", &ol);
    EMB_REQUIRE(zs != nullptr && ex != nullptr);
    EMB_REQUIRE(zl < ol); /* the zlib table must be stored compressed */

    wtab t[3] = {
        {tu_tag("head"), head, sizeof head, sizeof head, embi_sfnt_checksum(head, sizeof head)},
        {tu_tag("test"), test_data, sizeof test_data, sizeof test_data, 0},
        {tu_tag("zlib"), zs, zl, ol, 0},
    };
    uint8_t file[2048];
    size_t n = woff1_build(file, sizeof file, 0x00010000u, t, 3);
    EMB_REQUIRE(n > 0);

    embi_buf out = {0};
    EMB_CHECK(embi_woff1_decode(file, n, 4096, &out));
    if (out.len > 0) {
        const uint8_t *d = nullptr;
        size_t dl = 0;
        EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("test"), &d, &dl));
        EMB_CHECK(dl == sizeof test_data && SDL_memcmp(d, test_data, dl) == 0);
        EMB_CHECK(tu_sfnt_table(out.data, out.len, tu_tag("zlib"), &d, &dl));
        EMB_CHECK(dl == ol && SDL_memcmp(d, ex, ol) == 0);
        EMB_CHECK(embi_sfnt_checksum(out.data, out.len) == 0xB1B0AFBAu);
    }
    embi_buf_free(&out);
    SDL_free(zs);
    SDL_free(ex);
}

EMB_TEST(woff1_synthetic_rejections)
{
    static const uint8_t data[10] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};
    size_t zl = 0;
    size_t ol = 0;
    uint8_t *zs = tu_load("tests/data/deflate/tiny.zlib", &zl);
    uint8_t *ex = tu_load("tests/data/deflate/tiny.out", &ol);
    EMB_REQUIRE(zs != nullptr && ex != nullptr);
    uint8_t file[2048];
    embi_buf out = {0};

    /* Stored table longer than its original length: compLength > origLength. */
    wtab over[1] = {{tu_tag("test"), data, 10, 9, 0}};
    size_t n = woff1_build(file, sizeof file, 0x00010000u, over, 1);
    EMB_REQUIRE(n > 0);
    EMB_CHECK(!embi_woff1_decode(file, n, 4096, &out));

    /* Corrupted zlib checksum. */
    uint8_t *bad = embi_alloc(zl);
    SDL_memcpy(bad, zs, zl);
    bad[zl - 1] ^= 0x01;
    wtab badz[1] = {{tu_tag("zlib"), bad, zl, ol, 0}};
    n = woff1_build(file, sizeof file, 0x00010000u, badz, 1);
    EMB_REQUIRE(n > 0);
    EMB_CHECK(!embi_woff1_decode(file, n, 4096, &out));
    embi_free(bad, zl);

    /* Declared origLength does not match what the zlib stream produces. */
    wtab lenbad[1] = {{tu_tag("zlib"), zs, zl, ol - 1, 0}};
    n = woff1_build(file, sizeof file, 0x00010000u, lenbad, 1);
    EMB_REQUIRE(n > 0);
    EMB_CHECK(!embi_woff1_decode(file, n, 4096, &out));

    /* A good file that is cut short, and one whose limit is too small. */
    wtab good[1] = {{tu_tag("test"), data, 10, 10, 0}};
    n = woff1_build(file, sizeof file, 0x00010000u, good, 1);
    EMB_REQUIRE(n > 0);
    EMB_CHECK(embi_woff1_decode(file, n, 4096, &out));
    EMB_CHECK(!embi_woff1_decode(file, n - 1, 4096, &out));
    EMB_CHECK(!embi_woff1_decode(file, n, 16, &out));

    /* Wrong signature. */
    uint8_t sig[sizeof file];
    SDL_memcpy(sig, file, n);
    sig[0] = 'X';
    EMB_CHECK(!embi_woff1_decode(sig, n, 4096, &out));

    embi_buf_free(&out);
    SDL_free(zs);
    SDL_free(ex);
}

EMB_TEST(woff1_directory_and_size_validation)
{
    static const uint8_t d1[10] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};
    static const uint8_t d2[10] = {'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't'};
    wtab t[2] = {
        {tu_tag("test"), d1, 10, 10, 0},
        {tu_tag("zzzz"), d2, 10, 10, 0},
    };
    uint8_t file[2048];
    size_t n = woff1_build(file, sizeof file, 0x00010000u, t, 2);
    EMB_REQUIRE(n > 0);
    embi_buf out = {0};
    /* The untouched file decodes. Entry 0 holds offset 84, entry 1 holds 96 (44 + 2 * 20 aligned). */
    EMB_CHECK(embi_woff1_decode(file, n, 4096, &out));

    uint8_t copy[2048];
    /* Entry 1 points at entry 0's table: overlapping stored tables. */
    SDL_memcpy(copy, file, n);
    w32(copy + 44 + 20 + 4, 84);
    EMB_CHECK(!embi_woff1_decode(copy, n, 4096, &out));

    /* Entry 0 points into the header and directory. */
    SDL_memcpy(copy, file, n);
    w32(copy + 44 + 4, 44);
    EMB_CHECK(!embi_woff1_decode(copy, n, 4096, &out));

    /* totalSfntSize is only a hint: a wrong value still decodes to the same font. */
    SDL_memcpy(copy, file, n);
    w32(copy + 16, 68 + 4); /* the real size is 12 + 2 * 16 + 2 * 12 = 68 */
    EMB_CHECK(embi_woff1_decode(copy, n, 4096, &out));
    EMB_CHECK(out.len == 68);
    EMB_CHECK(!embi_woff1_decode(copy, n, 67, &out)); /* but max_out still bounds the output */

    /* Tags out of order (the same tables, swapped in the directory) are sorted on output. */
    wtab rev[2] = {
        {tu_tag("zzzz"), d2, 10, 10, 0},
        {tu_tag("test"), d1, 10, 10, 0},
    };
    size_t rn = woff1_build(file, sizeof file, 0x00010000u, rev, 2);
    EMB_REQUIRE(rn > 0);
    EMB_CHECK(embi_woff1_decode(file, rn, 4096, &out));
    EMB_CHECK(out.len == 68 && tu_be32(out.data + 12) == tu_tag("test") &&
              tu_be32(out.data + 28) == tu_tag("zzzz"));
    /* A repeated tag is still an error. */
    wtab dup[2] = {
        {tu_tag("test"), d1, 10, 10, 0},
        {tu_tag("test"), d2, 10, 10, 0},
    };
    size_t dn = woff1_build(file, sizeof file, 0x00010000u, dup, 2);
    EMB_REQUIRE(dn > 0);
    EMB_CHECK(!embi_woff1_decode(file, dn, 4096, &out));
    embi_buf_free(&out);
}

EMB_TEST(woff1_truncation_is_sampled_and_fails)
{
    size_t wl = 0;
    uint8_t *w = tu_load("tests/fonts/NotoSans-Regular.woff", &wl);
    EMB_REQUIRE(w != nullptr);
    embi_buf out = {0};
    size_t step = wl / 64 + 1;
    for (size_t k = 0; k < wl; k += step) {
        EMB_CHECK(!embi_woff1_decode(w, k, 1u << 22, &out));
    }
    embi_buf_free(&out);
    SDL_free(w);
}

EMB_TEST(woff1_random_byte_flips_never_crash)
{
    size_t wl = 0;
    uint8_t *w = tu_load("tests/fonts/SourceSans3-Regular.woff", &wl);
    EMB_REQUIRE(w != nullptr);
    tu_rng rng = {0xC0FFEE11u};
    embi_buf out = {0};
    for (int iter = 0; iter < 400; iter++) {
        uint8_t *copy = embi_alloc(wl);
        SDL_memcpy(copy, w, wl);
        int flips = 1 + (int)(tu_rand(&rng) % 4);
        for (int f = 0; f < flips; f++) {
            size_t at = tu_rand(&rng) % wl;
            copy[at] ^= (uint8_t)(1u + tu_rand(&rng) % 255u);
        }
        bool ok = embi_woff1_decode(copy, wl, 1u << 22, &out);
        EMB_CHECK(!ok || out.len <= (1u << 22));
        embi_free(copy, wl);
    }
    embi_buf_free(&out);
    SDL_free(w);
}

EMB_TEST_MAIN()
