/*
 * Fuzz-style mutation test over PngSuite. Each valid file goes through these inputs, with a fixed
 * seed so failures reproduce:
 *  - truncations at evenly spaced lengths, including the empty file;
 *  - raw byte flips with the CRCs left stale, so most stop at the CRC check (this covers that gate);
 *  - repaired mutants: the corruption is applied and then every chunk's CRC is recomputed, so the
 *    corrupt bytes reach the parser. The kinds are random bytes anywhere, random bytes in IHDR,
 *    random bytes in the IDAT zlib payload (with the Adler-32 recomputed when the raw deflate body
 *    still inflates), and PLTE or tRNS data resized with a new length field and CRC.
 * Every decode must return cleanly (no crash, no out-of-bounds read under ASan) and leave the live
 * allocation count where it was.
 *
 * Control checks: repairing an intact file must leave its bytes unchanged (the repair step does
 * not alter good data), and a share of the repaired mutants must still decode, so the test fails
 * if the repair step stops reaching the decoder.
 */
#include "png_suite_util.h"

#include "base/base.h"
#include "codec/codec.h"
#include "image/png.h"

#define FUZZ_TRUNCATIONS 24
#define FUZZ_MUTANTS 64  /* per file, split evenly over the four kinds */
#define FUZZ_RESIZE_DELTAS 3
#define FUZZ_IDAT_MAX_RAW ((size_t)1 << 22)
/* Of the IDAT-payload mutants with repaired CRCs, at least this share must decode. Measured at
 * about 8% with the fixed seed; without repair the same mutants stop at the CRC of IDAT (a
 * critical chunk), so this fails if repair stops reaching the decoder. */
#define FUZZ_MIN_IDAT_REPAIRED_OK_PERCENT 5

static uint32_t fz_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static uint32_t fz_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void fz_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool fz_type_is(const uint8_t *type, const char *name)
{
    return type[0] == (uint8_t)name[0] && type[1] == (uint8_t)name[1] &&
           type[2] == (uint8_t)name[2] && type[3] == (uint8_t)name[3];
}

/* CRC-32 over type and data, bit by bit (the test does not need speed). */
static uint32_t fz_crc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t fz_adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* Walks the chunk list. Returns the offset of the first chunk of the given type, with its data
 * length in *clen, or SIZE_MAX. Stops at IEND or at a chunk whose length does not fit. */
static size_t fz_find_chunk(const uint8_t *b, size_t n, const char *type, uint32_t *clen)
{
    if (n < 8) {
        return SIZE_MAX;
    }
    size_t pos = 8;
    while (n - pos >= 12) {
        uint32_t len = fz_be32(b + pos);
        if (len > n - pos - 12) {
            break;
        }
        if (fz_type_is(b + pos + 4, type)) {
            *clen = len;
            return pos;
        }
        if (fz_type_is(b + pos + 4, "IEND")) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    return SIZE_MAX;
}

static int fz_count_chunks(const uint8_t *b, size_t n, const char *type)
{
    if (n < 8) {
        return 0;
    }
    int count = 0;
    size_t pos = 8;
    while (n - pos >= 12) {
        uint32_t len = fz_be32(b + pos);
        if (len > n - pos - 12) {
            break;
        }
        if (fz_type_is(b + pos + 4, type)) {
            count++;
        }
        if (fz_type_is(b + pos + 4, "IEND")) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    return count;
}

/* Recomputes the CRC of every chunk the walk reaches. A chunk whose length field does not fit
 * ends the walk, so mangled lengths are left as they are. */
static void fz_repair(uint8_t *b, size_t n)
{
    if (n < 8) {
        return;
    }
    size_t pos = 8;
    while (n - pos >= 12) {
        uint32_t len = fz_be32(b + pos);
        if (len > n - pos - 12) {
            break;
        }
        fz_put_be32(b + pos + 8 + len, fz_crc(b + pos + 4, 4 + (size_t)len));
        if (fz_type_is(b + pos + 4, "IEND")) {
            break;
        }
        pos += 12 + (size_t)len;
    }
}

/* Flips one to four bytes at random offsets from 'from' on (the signature is left alone). */
static void fz_flip_bytes(uint8_t *b, size_t n, size_t from, uint32_t *seed)
{
    if (n <= from) {
        return;
    }
    int flips = 1 + (int)(fz_next(seed) % 4u);
    for (int k = 0; k < flips; k++) {
        size_t at = from + fz_next(seed) % (n - from);
        uint8_t v = (uint8_t)fz_next(seed);
        if (v == 0) {
            v = 1;
        }
        b[at] ^= v;
    }
}

/* Randomizes one to three bytes of the IHDR data. The CRC is repaired by the caller. */
static void fz_mutate_ihdr(uint8_t *b, size_t n, uint32_t *seed)
{
    uint32_t clen = 0;
    size_t at = fz_find_chunk(b, n, "IHDR", &clen);
    if (at == SIZE_MAX || clen != 13) {
        return;
    }
    int k = 1 + (int)(fz_next(seed) % 3u);
    for (int i = 0; i < k; i++) {
        b[at + 8 + fz_next(seed) % 13u] = (uint8_t)fz_next(seed);
    }
}

/* Changes one byte inside the deflate body of the first IDAT. The CRC is repaired by the caller.
 * When there is one IDAT and the body still inflates, the Adler-32 is recomputed too, so the
 * decoder gets past the checksum and into the scanlines. */
static void fz_mutate_idat(uint8_t *b, size_t n, uint32_t *seed)
{
    uint32_t clen = 0;
    size_t at = fz_find_chunk(b, n, "IDAT", &clen);
    if (at == SIZE_MAX || clen < 8) {
        return;
    }
    uint8_t *d = b + at + 8;
    size_t body = (size_t)clen - 6; /* deflate bytes are d[2 .. clen - 4) */
    size_t idx = 2 + fz_next(seed) % body;
    uint8_t v = (uint8_t)fz_next(seed);
    d[idx] ^= v ? v : 1;

    if (fz_count_chunks(b, n, "IDAT") == 1 && (fz_next(seed) & 1u)) {
        embi_buf out = {0};
        if (embi_inflate_all(d + 2, body, false, FUZZ_IDAT_MAX_RAW, &out)) {
            fz_put_be32(d + clen - 4, fz_adler(out.data, out.len));
        }
        embi_buf_free(&out);
    }
}

/* Copies src (n bytes) with the chunk at offset at changed to newlen data bytes. Kept data is
 * copied, new bytes come from the seed, and the CRC is recomputed. dst holds n + newlen bytes at
 * most. Returns the new size. */
static size_t fz_resize_chunk(const uint8_t *src, size_t n, size_t at, uint32_t oldlen,
                              uint32_t newlen, uint32_t *seed, uint8_t *dst)
{
    size_t o = 0;
    SDL_memcpy(dst, src, at);
    o = at;
    fz_put_be32(dst + o, newlen);
    SDL_memcpy(dst + o + 4, src + at + 4, 4); /* type */
    uint8_t *data = dst + o + 8;
    uint32_t keep = oldlen < newlen ? oldlen : newlen;
    SDL_memcpy(data, src + at + 8, keep);
    for (uint32_t i = keep; i < newlen; i++) {
        data[i] = (uint8_t)fz_next(seed);
    }
    fz_put_be32(data + newlen, fz_crc(dst + o + 4, 4 + (size_t)newlen));
    o += 12 + (size_t)newlen;
    size_t tail = at + 12 + (size_t)oldlen;
    SDL_memcpy(dst + o, src + tail, n - tail);
    o += n - tail;
    return o;
}

/* Decodes one mutated buffer and checks that nothing leaked. Returns false on a leak, and sets
 * *decoded when the decode succeeded. */
static bool fuzz_one(const uint8_t *data, size_t len, size_t live_before, bool *decoded)
{
    embi_pixels p = {0};
    *decoded = embi_png_decode(data, len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &p);
    if (*decoded) {
        embi_pixels_free(&p);
    }
    return embi_mem_live() == live_before;
}

EMB_TEST(pngsuite_mutations_never_crash_or_leak)
{
    ps_list pngs;
    ps_dir("tests/data/pngsuite", &pngs);
    uint32_t seed = 0x9E3779B9u;
    int files = 0;
    int leaks = 0;
    int identity_fail = 0;
    int kind_total[4] = {0};
    int kind_ok[4] = {0};
    int resize_total = 0;
    int resize_ok = 0;
    for (int i = 0; i < pngs.n; i++) {
        const char *fname = pngs.names[i];
        if (!ps_has_suffix(fname, ".png") || fname[0] == 'x') {
            continue;
        }
        char rel[256];
        SDL_snprintf(rel, sizeof rel, "tests/data/pngsuite/%s", fname);
        size_t len = 0;
        uint8_t *orig = ps_load(rel, &len);
        if (!orig || len == 0) {
            SDL_free(orig);
            continue;
        }
        files++;

        size_t cap = len + 16; /* room for a PLTE or tRNS that grows by up to 3 bytes */
        uint8_t *work = SDL_malloc(cap);
        uint8_t *resized = SDL_malloc(cap);
        size_t live = embi_mem_live();

        /* Repairing an intact file must change nothing. */
        SDL_memcpy(work, orig, len);
        fz_repair(work, len);
        if (SDL_memcmp(work, orig, len) != 0) {
            SDL_Log("fuzz: CRC repair changed intact %s", fname);
            identity_fail++;
        }

        /* Truncations, including the empty file. */
        bool decoded = false;
        for (int t = 0; t <= FUZZ_TRUNCATIONS; t++) {
            size_t cut = (len * (size_t)t) / FUZZ_TRUNCATIONS;
            if (cut >= len) {
                cut = len - 1;
            }
            if (!fuzz_one(orig, cut, live, &decoded)) {
                SDL_Log("fuzz: leak after truncating %s to %llu bytes", fname, (unsigned long long)cut);
                leaks++;
            }
        }

        /* Mutants. Kind 0 flips raw bytes and keeps the stale CRCs. Kinds 1 to 3 repair every CRC
         * after the corruption, so the corrupt bytes reach the decoder. */
        for (int f = 0; f < FUZZ_MUTANTS; f++) {
            int kind = f & 3;
            SDL_memcpy(work, orig, len);
            switch (kind) {
            case 0:
                fz_flip_bytes(work, len, 0, &seed);
                break;
            case 1:
                fz_flip_bytes(work, len, 8, &seed);
                fz_repair(work, len);
                break;
            case 2:
                fz_mutate_ihdr(work, len, &seed);
                fz_repair(work, len);
                break;
            default:
                fz_mutate_idat(work, len, &seed);
                fz_repair(work, len);
                break;
            }
            if (!fuzz_one(work, len, live, &decoded)) {
                SDL_Log("fuzz: leak after mutant %d (kind %d) of %s", f, kind, fname);
                leaks++;
            }
            kind_total[kind]++;
            kind_ok[kind] += decoded;
        }

        /* Length changes to PLTE and tRNS, with the CRC repaired for the new data. */
        static const char *const kResizable[2] = {"PLTE", "tRNS"};
        static const int kDeltas[FUZZ_RESIZE_DELTAS] = {-1, 1, 3};
        for (int r = 0; r < 2; r++) {
            uint32_t clen = 0;
            size_t at = fz_find_chunk(orig, len, kResizable[r], &clen);
            if (at == SIZE_MAX) {
                continue;
            }
            for (int k = 0; k < FUZZ_RESIZE_DELTAS; k++) {
                int64_t want = (int64_t)clen + kDeltas[k];
                uint32_t newlen = want < 0 ? 0u : (uint32_t)want;
                size_t n2 = fz_resize_chunk(orig, len, at, clen, newlen, &seed, resized);
                if (!fuzz_one(resized, n2, live, &decoded)) {
                    SDL_Log("fuzz: leak after resizing %s in %s", kResizable[r], fname);
                    leaks++;
                }
                resize_total++;
                resize_ok += decoded;
            }
        }
        SDL_free(resized);
        SDL_free(work);
        SDL_free(orig);
    }

    SDL_Log("fuzz: %d files; decoded raw %d/%d, repaired random %d/%d, repaired IHDR %d/%d, "
            "repaired IDAT %d/%d, resized %d/%d",
            files, kind_ok[0], kind_total[0], kind_ok[1], kind_total[1], kind_ok[2], kind_total[2],
            kind_ok[3], kind_total[3], resize_ok, resize_total);
    EMB_CHECK(files >= 150);
    EMB_CHECK_EQ_INT(leaks, 0);
    EMB_CHECK_EQ_INT(identity_fail, 0);
    /* The repair step must reach the decoder: repaired IDAT corruption has to decode sometimes. */
    EMB_CHECK(kind_ok[3] * 100 >= kind_total[3] * FUZZ_MIN_IDAT_REPAIRED_OK_PERCENT);
}

EMB_TEST_MAIN()
