/*
 * The build-time half of src/base/lz.c (SZ1): an encoder for the byte-oriented LZ format that embi_lz_decode reads.
 * Header only, so that tools/lz/lz.c (the build step) and tests/unit/base/test_lz.c share it. Not part of libember.
 *
 * Format: a run of sequences. Sequence = token byte (high nibble: literal count, low nibble: match length - 4), the
 * literal count's extension bytes if the nibble is 15 (each 255 adds 255, the first byte below 255 ends it), the literals,
 * then, unless the output is complete, a 16-bit little-endian match offset (1..65535) and the match length's extension
 * bytes in the same way. The last sequence has literals only. The decoder knows the output size.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LZE_MIN_MATCH 4u
#define LZE_MAX_OFFSET 65535u
#define LZE_HASH_BITS 16u
#define LZE_CHAIN 256u
/* A match this long ends the chain search, and at the next position the encoder takes it one byte shorter at the same offset
 * instead of searching again. Without this the optimal parse searched every position of a long repeat with full-length
 * compares: 70 KB of one byte took over 200 s (zlib's nice_match does the same). */
#define LZE_NICE 256u

static size_t lze_put_len(uint8_t *out, size_t o, size_t v)
{
    while (v >= 255) {
        out[o++] = 255;
        v -= 255;
    }
    out[o++] = (uint8_t)v;
    return o;
}

/* Worst case output size for n input bytes. */
static size_t lze_bound(size_t n)
{
    return n + n / 255 + 16;
}

static uint32_t lze_hash(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - LZE_HASH_BITS);
}

static size_t lze_match_len(const uint8_t *a, const uint8_t *b, size_t max)
{
    size_t n = 0;
    while (n < max && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* Finds the longest match for in[pos..] among earlier positions on the hash chain. */
static size_t lze_find(const uint8_t *in, size_t n, size_t pos, const int32_t *head, const int32_t *prev, size_t *off)
{
    size_t best = 0;
    if (pos + LZE_MIN_MATCH > n) {
        return 0;
    }
    int32_t cand = head[lze_hash(in + pos)];
    for (unsigned depth = 0; cand >= 0 && depth < LZE_CHAIN; depth++) {
        size_t d = pos - (size_t)cand;
        if (d > LZE_MAX_OFFSET) {
            break;
        }
        size_t len = lze_match_len(in + cand, in + pos, n - pos);
        if (len > best) {
            best = len;
            *off = d;
            if (len >= LZE_NICE || len == n - pos) {
                break;
            }
        }
        cand = prev[cand];
    }
    return best >= LZE_MIN_MATCH ? best : 0;
}

/* Cost in bytes of a match of length len: token and 16-bit offset, then the length extension bytes. */
static size_t lze_match_cost(size_t len)
{
    size_t ml = len - LZE_MIN_MATCH;
    return 3 + (ml >= 15 ? 1 + (ml - 15) / 255 : 0);
}

/* Compresses in[0..n) into out (at least lze_bound(n) bytes); returns the compressed size. The parse is optimal for a cost
 * of one byte a literal and lze_match_cost a match (SZ3a; it replaced a lazy greedy parse and is 3 to 6% smaller): the longest
 * match at every position, then a shortest-path pass over "emit a literal" and "emit a match of any length up to the longest". */
static size_t lze_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    int32_t *head = malloc(sizeof(int32_t) << LZE_HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * (n + 1));
    uint32_t *mlen = calloc(n + 1, sizeof(uint32_t));
    uint32_t *moff = calloc(n + 1, sizeof(uint32_t));
    uint32_t *cost = malloc(sizeof(uint32_t) * (n + 1));
    uint32_t *step = calloc(n + 1, sizeof(uint32_t)); /* how position i was reached: 0 literal, else match length */
    for (size_t i = 0; i < ((size_t)1 << LZE_HASH_BITS); i++) {
        head[i] = -1;
    }
    for (size_t i = 0; i <= n; i++) {
        prev[i] = -1;
    }
    for (size_t pos = 0; pos < n; pos++) {
        size_t off = 0, len;
        if (pos > 0 && mlen[pos - 1] > LZE_NICE) {
            len = mlen[pos - 1] - 1u; /* the match at pos - 1 continues */
            off = moff[pos - 1];
        } else {
            len = lze_find(in, n, pos, head, prev, &off);
        }
        mlen[pos] = (uint32_t)len;
        moff[pos] = (uint32_t)off;
        if (pos + 4 <= n) {
            uint32_t h = lze_hash(in + pos);
            prev[pos] = head[h];
            head[h] = (int32_t)pos;
        }
    }
    for (size_t i = 0; i <= n; i++) {
        cost[i] = UINT32_MAX;
    }
    cost[0] = 0;
    for (size_t i = 0; i < n; i++) {
        if (cost[i] + 1 < cost[i + 1]) {
            cost[i + 1] = cost[i] + 1;
            step[i + 1] = 0;
        }
        size_t L = mlen[i];
        if (L >= LZE_MIN_MATCH) {
            for (size_t l = LZE_MIN_MATCH; l <= L && i + l <= n; l++) {
                if (l > 600 && l != L) {
                    l = L - 1; /* long matches: only the longest end matters in practice */
                    continue;
                }
                uint32_t c = cost[i] + (uint32_t)lze_match_cost(l);
                if (c < cost[i + l]) {
                    cost[i + l] = c;
                    step[i + l] = (uint32_t)l;
                }
            }
        }
    }
    /* Walk back from n to find the matches, then emit forward. */
    size_t nm = 0;
    for (size_t j = n; j > 0;) {
        if (step[j] == 0) {
            j--;
        } else {
            nm++;
            j -= step[j];
        }
    }
    size_t *mpos = malloc(sizeof(size_t) * (nm + 1));
    size_t *mlv = malloc(sizeof(size_t) * (nm + 1));
    size_t k = nm;
    for (size_t j = n; j > 0;) {
        if (step[j] == 0) {
            j--;
        } else {
            k--;
            mlv[k] = step[j];
            mpos[k] = j - step[j];
            j -= step[j];
        }
    }
    size_t o = 0, lit = 0;
    for (size_t m = 0; m < nm; m++) {
        size_t pos = mpos[m];
        size_t l = mlv[m];
        size_t off = moff[pos];
        size_t ll = pos - lit;
        size_t ml = l - LZE_MIN_MATCH;
        out[o++] = (uint8_t)(((ll >= 15 ? 15 : ll) << 4) | (ml >= 15 ? 15 : ml));
        if (ll >= 15) {
            o = lze_put_len(out, o, ll - 15);
        }
        memcpy(out + o, in + lit, ll);
        o += ll;
        out[o++] = (uint8_t)(off & 0xff);
        out[o++] = (uint8_t)(off >> 8);
        if (ml >= 15) {
            o = lze_put_len(out, o, ml - 15);
        }
        lit = pos + l;
    }
    size_t ll = n - lit;
    out[o++] = (uint8_t)((ll >= 15 ? 15 : ll) << 4);
    if (ll >= 15) {
        o = lze_put_len(out, o, ll - 15);
    }
    memcpy(out + o, in + lit, ll);
    o += ll;
    free(mpos);
    free(mlv);
    free(step);
    free(cost);
    free(moff);
    free(mlen);
    free(head);
    free(prev);
    return o;
}
