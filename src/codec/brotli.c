#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

/*
 * Brotli decoder (RFC 7932). The whole compressed stream is in memory, so the decoder is one-shot:
 * every read past the end of the input is an error. The output buffer doubles as the sliding
 * window, which is bounded by the stream's own output (the caller's max_out limit applies).
 */

/* ------------------------------------------------------------------ bit reader (LSB first) */

typedef struct br {
    const uint8_t *p;
    size_t n;
    size_t pos;
    uint64_t buf;
    unsigned cnt; /* buffered bits, always fewer than 8 between reads */
    bool err;
    const uint8_t *dict; /* the static dictionary, opened on the first reference (embi_brotli_dict_open) */
} br;

static uint32_t br_bits(br *b, unsigned n)
{
    while (b->cnt < n) {
        if (b->pos >= b->n) {
            b->err = true;
            return 0;
        }
        b->buf |= (uint64_t)b->p[b->pos++] << b->cnt;
        b->cnt += 8;
    }
    uint32_t v = (uint32_t)(b->buf & ((1ull << n) - 1));
    b->buf >>= n;
    b->cnt -= n;
    return v;
}

/* Skips to a byte boundary. The skipped bits must be zero. */
static bool br_align(br *b)
{
    unsigned r = b->cnt & 7u;
    return br_bits(b, r) == 0 && !b->err;
}

/* Trailing fill bits of the final byte must be zero. */
static bool br_finish(const br *b)
{
    return !b->err && (b->buf & ((1ull << b->cnt) - 1)) == 0;
}

/* ------------------------------------------------------------------ prefix codes */

/* Canonical prefix code, decoded one bit at a time from (count, sorted symbols). A code with a
 * single symbol takes zero bits. */
typedef struct bhuff {
    uint16_t count[16];
    uint16_t sym[704];
    bool single;
    uint16_t single_sym;
} bhuff;

/* lens[] are code lengths over n symbols. The lengths must form a complete code: the sum of
 * full >> len over all used symbols equals full (full is 32768 for the alphabet, 32 for the
 * code-length code). */
static bool bh_build(bhuff *h, const uint8_t *lens, unsigned n, uint32_t full)
{
    uint16_t count[16] = {0};
    uint32_t space = full;
    unsigned nonzero = 0;
    for (unsigned s = 0; s < n; s++) {
        unsigned len = lens[s];
        if (len == 0) {
            continue;
        }
        if (len > 15 || (full >> len) > space) {
            return false;
        }
        space -= full >> len;
        count[len]++;
        nonzero++;
    }
    if (space != 0 || nonzero == 0) {
        return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (unsigned len = 1; len < 15; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + count[len]);
    }
    for (unsigned s = 0; s < n; s++) {
        if (lens[s]) {
            h->sym[offs[lens[s]]++] = (uint16_t)s;
        }
    }
    SDL_memcpy(h->count, count, sizeof count);
    h->single = false;
    return true;
}

static void bh_single(bhuff *h, unsigned sym)
{
    SDL_memset(h->count, 0, sizeof h->count);
    h->single = true;
    h->single_sym = (uint16_t)sym;
}

/* Returns the decoded symbol, or -1 on error. */
static int bh_decode(br *b, const bhuff *h)
{
    if (h->single) {
        return h->single_sym;
    }
    int code = 0;
    int first = 0;
    int ix = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)br_bits(b, 1);
        if (b->err) {
            return -1;
        }
        int count = h->count[len];
        if (code - count < first) {
            return h->sym[ix + (code - first)];
        }
        ix += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static unsigned ceil_log2(uint32_t n)
{
    unsigned bits = 0;
    while ((1u << bits) < n) {
        bits++;
    }
    return bits;
}

/* RFC 7932 3.4: simple prefix code. The 2-bit NSYM-1 field follows the HSKIP == 1 marker. */
static bool read_simple(br *b, uint32_t alphabet, bhuff *h)
{
    unsigned nsym = br_bits(b, 2) + 1;
    unsigned abits = ceil_log2(alphabet);
    uint32_t sym[4];
    for (unsigned i = 0; i < nsym; i++) {
        sym[i] = br_bits(b, abits);
        if (sym[i] >= alphabet) {
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            if (sym[j] == sym[i]) {
                return false;
            }
        }
    }
    if (b->err) {
        return false;
    }
    if (nsym == 1) {
        bh_single(h, sym[0]);
        return true;
    }
    uint8_t lens[704];
    SDL_memset(lens, 0, sizeof lens);
    if (nsym == 2) {
        lens[sym[0]] = 1;
        lens[sym[1]] = 1;
    } else if (nsym == 3) {
        lens[sym[0]] = 1;
        lens[sym[1]] = 2;
        lens[sym[2]] = 2;
    } else if (br_bits(b, 1)) {
        lens[sym[0]] = 1;
        lens[sym[1]] = 2;
        lens[sym[2]] = 3;
        lens[sym[3]] = 3;
    } else {
        lens[sym[0]] = 2;
        lens[sym[1]] = 2;
        lens[sym[2]] = 2;
        lens[sym[3]] = 2;
    }
    if (b->err) {
        return false;
    }
    return bh_build(h, lens, alphabet, 32768);
}

/* RFC 7932 3.5: the 2..4 bit variable-length code of the code-length code lengths. Patterns are
 * read one bit at a time: 00->0, 01->4, 10->3, 0111->1, 011->2, 1111->5. */
static unsigned read_cl_len(br *b)
{
    unsigned b0 = br_bits(b, 1);
    unsigned b1 = br_bits(b, 1);
    if (!b0) {
        return b1 ? 3u : 0u;
    }
    if (!b1) {
        return 4u;
    }
    if (!br_bits(b, 1)) {
        return 2u;
    }
    return br_bits(b, 1) ? 5u : 1u;
}

static bool read_complex(br *b, uint32_t alphabet, unsigned hskip, bhuff *h)
{
    static const uint8_t kOrder[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t cl[18];
    SDL_memset(cl, 0, sizeof cl);
    uint32_t space = 32;
    unsigned ncodes = 0;
    for (unsigned i = hskip; i < 18 && space > 0; i++) {
        unsigned v = read_cl_len(b);
        cl[kOrder[i]] = (uint8_t)v;
        if (v) {
            space -= 32u >> v;
            ncodes++;
        }
    }
    if (b->err || !(ncodes == 1 || space == 0)) {
        return false;
    }
    bhuff clh;
    if (ncodes == 1) {
        unsigned only = 0;
        for (unsigned k = 0; k < 18; k++) {
            if (cl[k]) {
                only = k;
            }
        }
        bh_single(&clh, only);
    } else if (!bh_build(&clh, cl, 18, 32)) {
        return false;
    }

    uint8_t lens[704];
    SDL_memset(lens, 0, sizeof lens);
    uint32_t symbol = 0;
    uint32_t prev_len = 8;
    uint32_t repeat = 0;
    uint32_t repeat_len = 0;
    uint32_t left = 32768;
    while (symbol < alphabet && left > 0) {
        int sym = bh_decode(b, &clh);
        if (sym < 0) {
            return false;
        }
        if (sym < 16) {
            repeat = 0;
            lens[symbol++] = (uint8_t)sym;
            if (sym != 0) {
                if ((32768u >> sym) > left) {
                    return false;
                }
                prev_len = (uint32_t)sym;
                left -= 32768u >> sym;
            }
            continue;
        }
        unsigned extra = sym == 16 ? 2u : 3u;
        uint32_t new_len = sym == 16 ? prev_len : 0;
        if (repeat_len != new_len) {
            repeat = 0;
            repeat_len = new_len;
        }
        uint32_t old_repeat = repeat;
        if (repeat > 0) {
            repeat = (repeat - 2) << extra;
        }
        repeat += br_bits(b, extra) + 3;
        uint32_t delta = repeat - old_repeat;
        if (b->err || symbol + delta > alphabet) {
            return false;
        }
        for (uint32_t i = 0; i < delta; i++) {
            lens[symbol++] = (uint8_t)repeat_len;
        }
        if (repeat_len) {
            if (delta * (32768u >> repeat_len) > left) {
                return false;
            }
            left -= delta * (32768u >> repeat_len);
        }
    }
    if (left != 0) {
        return false;
    }
    return bh_build(h, lens, alphabet, 32768);
}

/* Reads one prefix code over an alphabet of the given size. */
static bool read_prefix(br *b, uint32_t alphabet, bhuff *h)
{
    unsigned hskip = br_bits(b, 2);
    if (b->err) {
        return false;
    }
    if (hskip == 1) {
        return read_simple(b, alphabet, h);
    }
    return read_complex(b, alphabet, hskip, h);
}

/* ------------------------------------------------------------------ tables (RFC 7932 5, 6) */

static const uint16_t kInsBase[24] = {0,  1,  2,  3,  4,   5,   6,   8,   10,   14,   18,   26,
                                      34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t kInsExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint16_t kCopyBase[24] = {2,  3,  4,  5,  6,  7,  8,  9,  10,  12,   14,   18,
                                       22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
/* Insert and copy length code cells (the symbol's top bits), 0..10. */
static const uint8_t kInsRange[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
static const uint8_t kCopyRange[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};

static const uint16_t kBlockBase[26] = {1,   5,   9,   13,  17,  25,  33,   41,   49,   65,   81,   97,  113,
                                        145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625};
static const uint8_t kBlockExtra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};

/* Block-switch state for one category (literal, insert-and-copy, or distance). */
typedef struct bswitch {
    uint32_t n;
    uint32_t cur;
    uint32_t prev;
    uint32_t remain;
    bhuff type;
    bhuff count;
} bswitch;

/* RFC 7932 9.2: the NBLTYPES and NTREES variable-length code (values 1..256). */
static uint32_t read_nbltypes(br *b)
{
    if (!br_bits(b, 1)) {
        return 1;
    }
    unsigned n = br_bits(b, 3);
    if (n == 0) {
        return 2;
    }
    return (1u << n) + 1 + br_bits(b, n);
}

static bool read_block_len(br *b, bswitch *s)
{
    int sym = bh_decode(b, &s->count);
    if (sym < 0) {
        return false;
    }
    s->remain = kBlockBase[sym] + br_bits(b, kBlockExtra[sym]);
    return !b->err;
}

static bool bs_init(br *b, bswitch *s)
{
    s->n = read_nbltypes(b);
    s->cur = 0;
    s->prev = 1;
    s->remain = 1u << 24;
    if (s->n >= 2) {
        if (!read_prefix(b, s->n + 2, &s->type) || !read_prefix(b, 26, &s->count)) {
            return false;
        }
        if (!read_block_len(b, s)) {
            return false;
        }
    }
    return !b->err;
}

/* Called when the current block of a category is used up and there is more than one type. */
static bool bs_next(br *b, bswitch *s)
{
    int sym = bh_decode(b, &s->type);
    if (sym < 0) {
        return false;
    }
    uint32_t nt;
    if (sym == 0) {
        nt = s->prev;
    } else if (sym == 1) {
        nt = (s->cur + 1) % s->n;
    } else {
        nt = (uint32_t)sym - 2;
    }
    if (nt >= s->n) {
        return false;
    }
    s->prev = s->cur;
    s->cur = nt;
    return read_block_len(b, s);
}

static void inverse_mtf(uint8_t *v, uint32_t n)
{
    uint8_t mtf[256];
    for (unsigned i = 0; i < 256; i++) {
        mtf[i] = (uint8_t)i;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ix = v[i];
        uint8_t value = mtf[ix];
        v[i] = value;
        for (; ix; ix--) {
            mtf[ix] = mtf[ix - 1];
        }
        mtf[0] = value;
    }
}

/* RFC 7932 7.3: context map of size entries over ntrees prefix codes. */
static bool read_cmap(br *b, uint32_t size, uint32_t ntrees, uint8_t *map)
{
    unsigned rlemax = 0;
    if (br_bits(b, 1)) {
        rlemax = br_bits(b, 4) + 1;
    }
    bhuff h;
    if (!read_prefix(b, ntrees + rlemax, &h)) {
        return false;
    }
    uint32_t i = 0;
    while (i < size) {
        int sym = bh_decode(b, &h);
        if (sym < 0) {
            return false;
        }
        if (sym == 0) {
            map[i++] = 0;
        } else if ((unsigned)sym <= rlemax) {
            uint32_t reps = (1u << sym) + br_bits(b, (unsigned)sym);
            if (b->err || reps > size - i) {
                return false;
            }
            SDL_memset(map + i, 0, reps);
            i += reps;
        } else {
            map[i++] = (uint8_t)(sym - (int)rlemax);
        }
    }
    if (br_bits(b, 1)) {
        inverse_mtf(map, size);
    }
    return !b->err;
}

/* ------------------------------------------------------------------ output helpers */

static bool out_bytes(embi_buf *o, size_t max_out, const uint8_t *src, size_t n)
{
    if (n > max_out - o->len || !embi_buf_append(o, src, n)) {
        return false;
    }
    return true;
}

static bool out_byte(embi_buf *o, size_t max_out, uint8_t v)
{
    return out_bytes(o, max_out, &v, 1);
}

/* ------------------------------------------------------------------ distances (RFC 7932 4) */

/* Turns a distance symbol into a distance. ring holds the last four pushed distances, ring_n
 * counts pushes. Short codes 0..15 refer to the ring; 16..15+NDIRECT are direct codes. */
static bool decode_distance(br *b, uint32_t code, uint32_t npostfix, uint32_t ndirect,
                            const uint32_t ring[4], uint32_t ring_n, uint32_t *out_dist)
{
    if (code < 16) {
        int64_t last = ring[(ring_n - 1) & 3u];
        int64_t second = ring[(ring_n - 2) & 3u];
        int64_t third = ring[(ring_n - 3) & 3u];
        int64_t fourth = ring[ring_n & 3u];
        int64_t d;
        switch (code) {
        case 0: d = last; break;
        case 1: d = second; break;
        case 2: d = third; break;
        case 3: d = fourth; break;
        case 4: d = last - 1; break;
        case 5: d = last + 1; break;
        case 6: d = last - 2; break;
        case 7: d = last + 2; break;
        case 8: d = last - 3; break;
        case 9: d = last + 3; break;
        case 10: d = second - 1; break;
        case 11: d = second + 1; break;
        case 12: d = second - 2; break;
        case 13: d = second + 2; break;
        case 14: d = second - 3; break;
        default: d = second + 3; break;
        }
        if (d <= 0 || d > (int64_t)UINT32_MAX) {
            return false;
        }
        *out_dist = (uint32_t)d;
        return true;
    }
    if (code < 16 + ndirect) {
        *out_dist = code - 15;
        return true;
    }
    uint32_t dcode = code - 16 - ndirect;
    uint32_t ndistbits = 1 + (dcode >> (npostfix + 1));
    uint32_t hcode = dcode >> npostfix;
    uint32_t lcode = dcode & ((1u << npostfix) - 1);
    uint32_t offset = ((2u + (hcode & 1u)) << ndistbits) - 4u;
    uint32_t dextra = br_bits(b, ndistbits);
    if (b->err) {
        return false;
    }
    *out_dist = ((offset + dextra) << npostfix) + lcode + ndirect + 1;
    return true;
}

static uint32_t lit_context(unsigned mode, uint8_t p1, uint8_t p2)
{
    switch (mode) {
    case 0:
        return p1 & 0x3fu;
    case 1:
        return p1 >> 2;
    case 2:
        return (uint32_t)embi_brotli_lut0[p1] | embi_brotli_lut1[p2];
    default:
        return ((uint32_t)embi_brotli_lut2[p1] << 3) | embi_brotli_lut2[p2];
    }
}

static void ferment_at(uint8_t *w, uint32_t wl, uint32_t pos, uint32_t *advance)
{
    if (w[pos] < 192) {
        if (w[pos] >= 97 && w[pos] <= 122) {
            w[pos] ^= 32;
        }
        *advance = 1;
    } else if (w[pos] < 224) {
        if (pos + 1 < wl) {
            w[pos + 1] ^= 32;
        }
        *advance = 2;
    } else {
        if (pos + 2 < wl) {
            w[pos + 2] ^= 5;
        }
        *advance = 3;
    }
}

/* Writes the transformed static dictionary word into dst (at least 64 bytes). Returns its length
 * or -1 when the reference is invalid. */
static int dict_word(br *b, uint32_t len, uint64_t word_id, uint8_t *dst)
{
    if (len < 4 || len > 24) {
        return -1;
    }
    unsigned nd = embi_brotli_ndbits[len];
    uint64_t idx = word_id & ((1ull << nd) - 1);
    uint64_t tid = word_id >> nd;
    if (tid >= EMBI_BROTLI_NUM_TRANSFORMS) {
        return -1;
    }
    const embi_brotli_transform *t = &embi_brotli_transforms[tid];
    uint8_t word[24];
    if (b->dict == nullptr) {
        b->dict = embi_brotli_dict_open();
        if (b->dict == nullptr) {
            return -1;
        }
    }
    SDL_memcpy(word, b->dict + embi_brotli_doffset[len] + idx * len, len);

    uint32_t start = 0;
    uint32_t wl = len;
    if (t->type >= 3 && t->type <= 11) {
        uint32_t k = t->type - 2u; /* OmitFirstK: keep the last len-k bytes */
        start = k < wl ? k : wl;
        wl -= start;
    } else if (t->type >= 12) {
        uint32_t k = t->type - 11u; /* OmitLastK: keep the first len-k bytes */
        wl = wl > k ? wl - k : 0;
    }
    uint8_t *w = word + start;
    if (t->type == 1) {
        uint32_t adv;
        if (wl > 0) {
            ferment_at(w, wl, 0, &adv);
        }
    } else if (t->type == 2) {
        for (uint32_t i = 0; i < wl;) {
            uint32_t adv;
            ferment_at(w, wl, i, &adv);
            i += adv;
        }
    }

    size_t pl = SDL_strlen(t->prefix);
    size_t sl = SDL_strlen(t->suffix);
    if (pl + wl + sl > 64) {
        return -1;
    }
    size_t n = 0;
    SDL_memcpy(dst + n, t->prefix, pl);
    n += pl;
    SDL_memcpy(dst + n, w, wl);
    n += wl;
    SDL_memcpy(dst + n, t->suffix, sl);
    n += sl;
    return (int)n;
}

/* ------------------------------------------------------------------ metablock data */

typedef struct mblock_codes {
    bhuff *lit;
    bhuff *ins;
    bhuff *dist;
    uint8_t *cmap_lit;
    uint8_t *cmap_dist;
    size_t n_lit;
    size_t n_ins;
    size_t n_dist;
    size_t n_cmap_lit;
    size_t n_cmap_dist;
} mblock_codes;

static void mblock_free(mblock_codes *c)
{
    if (c->lit) {
        embi_free(c->lit, c->n_lit * sizeof *c->lit);
    }
    if (c->ins) {
        embi_free(c->ins, c->n_ins * sizeof *c->ins);
    }
    if (c->dist) {
        embi_free(c->dist, c->n_dist * sizeof *c->dist);
    }
    if (c->cmap_lit) {
        embi_free(c->cmap_lit, c->n_cmap_lit);
    }
    if (c->cmap_dist) {
        embi_free(c->cmap_dist, c->n_cmap_dist);
    }
    *c = (mblock_codes){0};
}

/* Decodes one compressed meta-block of mlen bytes. */
static bool decode_compressed(br *b, uint32_t mlen, uint64_t max_back, uint32_t ring[4],
                              uint32_t *ring_n, embi_buf *out, size_t max_out)
{
    bswitch bl;
    bswitch bi;
    bswitch bd;
    mblock_codes c = {0};
    bool ok = false;

    SDL_memset(&bl, 0, sizeof bl);
    SDL_memset(&bi, 0, sizeof bi);
    SDL_memset(&bd, 0, sizeof bd);
    if (!bs_init(b, &bl) || !bs_init(b, &bi) || !bs_init(b, &bd)) {
        return false;
    }
    uint32_t npostfix = br_bits(b, 2);
    uint32_t ndirect = br_bits(b, 4) << npostfix;
    if (b->err) {
        return false;
    }
    uint8_t cmode[256];
    for (uint32_t i = 0; i < bl.n; i++) {
        cmode[i] = (uint8_t)br_bits(b, 2);
    }
    uint32_t ntrees_lit = read_nbltypes(b);
    c.n_cmap_lit = 64u * bl.n;
    c.cmap_lit = embi_calloc(c.n_cmap_lit, 1);
    if (ntrees_lit >= 2 && !read_cmap(b, (uint32_t)c.n_cmap_lit, ntrees_lit, c.cmap_lit)) {
        goto done;
    }
    uint32_t ntrees_dist = read_nbltypes(b);
    c.n_cmap_dist = 4u * bd.n;
    c.cmap_dist = embi_calloc(c.n_cmap_dist, 1);
    if (ntrees_dist >= 2 && !read_cmap(b, (uint32_t)c.n_cmap_dist, ntrees_dist, c.cmap_dist)) {
        goto done;
    }
    c.n_lit = ntrees_lit;
    c.lit = embi_calloc(c.n_lit, sizeof *c.lit);
    for (uint32_t i = 0; i < ntrees_lit; i++) {
        if (!read_prefix(b, 256, &c.lit[i])) {
            goto done;
        }
    }
    c.n_ins = bi.n;
    c.ins = embi_calloc(c.n_ins, sizeof *c.ins);
    for (uint32_t i = 0; i < bi.n; i++) {
        if (!read_prefix(b, 704, &c.ins[i])) {
            goto done;
        }
    }
    uint32_t dist_alpha = 16 + ndirect + (48u << npostfix);
    c.n_dist = ntrees_dist;
    c.dist = embi_calloc(c.n_dist, sizeof *c.dist);
    for (uint32_t i = 0; i < ntrees_dist; i++) {
        if (!read_prefix(b, dist_alpha, &c.dist[i])) {
            goto done;
        }
    }

    uint32_t produced = 0;
    while (produced < mlen) {
        if (bi.n >= 2 && bi.remain == 0 && !bs_next(b, &bi)) {
            goto done;
        }
        bi.remain--;
        int icode = bh_decode(b, &c.ins[bi.cur]);
        if (icode < 0) {
            goto done;
        }
        unsigned cell = (unsigned)icode >> 6;
        unsigned insc = kInsRange[cell] + (((unsigned)icode >> 3) & 7u);
        unsigned copyc = kCopyRange[cell] + ((unsigned)icode & 7u);
        uint32_t ins_len = kInsBase[insc] + br_bits(b, kInsExtra[insc]);
        uint32_t copy_len = kCopyBase[copyc] + br_bits(b, kCopyExtra[copyc]);
        if (b->err) {
            goto done;
        }

        for (uint32_t k = 0; k < ins_len; k++) {
            if (produced >= mlen) {
                goto done;
            }
            if (bl.n >= 2 && bl.remain == 0 && !bs_next(b, &bl)) {
                goto done;
            }
            bl.remain--;
            uint8_t p1 = out->len >= 1 ? out->data[out->len - 1] : 0;
            uint8_t p2 = out->len >= 2 ? out->data[out->len - 2] : 0;
            uint32_t cid = lit_context(cmode[bl.cur], p1, p2);
            int lit = bh_decode(b, &c.lit[c.cmap_lit[64u * bl.cur + cid]]);
            if (lit < 0 || !out_byte(out, max_out, (uint8_t)lit)) {
                goto done;
            }
            produced++;
        }
        if (produced >= mlen) {
            break; /* the copy length of the last command is ignored */
        }

        bool implicit = cell < 2;
        uint32_t dist = 0;
        bool push = false;
        if (implicit) {
            dist = ring[(*ring_n - 1) & 3u];
        } else {
            if (bd.n >= 2 && bd.remain == 0 && !bs_next(b, &bd)) {
                goto done;
            }
            bd.remain--;
            uint32_t ctx = copy_len > 4 ? 3u : copy_len - 2u;
            int dsym = bh_decode(b, &c.dist[c.cmap_dist[4u * bd.cur + ctx]]);
            if (dsym < 0 || !decode_distance(b, (uint32_t)dsym, npostfix, ndirect, ring, *ring_n, &dist)) {
                goto done;
            }
            push = dsym != 0;
        }

        uint64_t maxd = max_back < out->len ? max_back : out->len;
        if (dist <= maxd) {
            if (copy_len > mlen - produced || copy_len > max_out - out->len || !embi_buf_reserve(out, copy_len)) {
                goto done;
            }
            for (uint32_t k = 0; k < copy_len; k++) {
                out->data[out->len] = out->data[out->len - dist];
                out->len++;
            }
            produced += copy_len;
            if (push) {
                ring[*ring_n & 3u] = dist;
                (*ring_n)++;
            }
        } else {
            /* Static dictionary reference: distances beyond the window do not enter the ring. */
            uint8_t tmp[64];
            int tl = dict_word(b, copy_len, (uint64_t)dist - maxd - 1, tmp);
            if (tl < 0 || (uint32_t)tl > mlen - produced || !out_bytes(out, max_out, tmp, (size_t)tl)) {
                goto done;
            }
            produced += (uint32_t)tl;
        }
    }
    ok = true;

done:
    mblock_free(&c);
    return ok;
}

/* ------------------------------------------------------------------ stream */

static unsigned read_wbits(br *b)
{
    if (!br_bits(b, 1)) {
        return 16;
    }
    unsigned n = br_bits(b, 3);
    if (n) {
        return 17 + n;
    }
    unsigned m = br_bits(b, 3);
    if (m == 1) {
        return 0; /* reserved (large-window) encoding */
    }
    return m ? 8 + m : 17;
}

bool embi_brotli_decode(const uint8_t *in, size_t in_len, size_t max_out, embi_buf *out)
{
    out->len = 0;
    br b = {.p = in, .n = in_len};
    unsigned wbits = read_wbits(&b);
    if (b.err || wbits == 0) {
        return false;
    }
    uint64_t max_back = (1ull << wbits) - 16;
    uint32_t ring[4] = {16, 15, 11, 4};
    uint32_t ring_n = 0;
    bool ok = false;

    for (;;) {
        uint32_t islast = br_bits(&b, 1);
        if (islast && br_bits(&b, 1)) {
            ok = !b.err;
            break; /* ISLASTEMPTY: the stream ends here */
        }
        uint32_t mn = br_bits(&b, 2);
        if (b.err) {
            break;
        }
        if (mn == 3) {
            /* Metadata meta-block: skipped, not part of the output. */
            if (br_bits(&b, 1) != 0) {
                break;
            }
            uint32_t msk = br_bits(&b, 2);
            uint32_t skip = 0;
            bool bad = false;
            for (uint32_t i = 0; i < msk; i++) {
                uint32_t byte = br_bits(&b, 8);
                if (i + 1 == msk && msk > 1 && byte == 0) {
                    bad = true;
                }
                skip |= byte << (8 * i);
            }
            if (b.err || bad) {
                break;
            }
            if (msk) {
                skip += 1;
            }
            if (!br_align(&b) || b.pos > b.n || skip > b.n - b.pos) {
                break;
            }
            b.pos += skip;
            if (islast) {
                ok = true;
                break;
            }
            continue;
        }

        unsigned mnib = mn == 0 ? 4u : (mn == 1 ? 5u : 6u);
        uint32_t mlen_m1 = br_bits(&b, 4 * mnib);
        if (b.err || (mnib > 4 && (mlen_m1 >> (4 * (mnib - 1))) == 0)) {
            break;
        }
        uint32_t mlen = mlen_m1 + 1;
        uint32_t isuncompressed = islast ? 0 : br_bits(&b, 1);
        if (b.err) {
            break;
        }
        if (isuncompressed) {
            if (!br_align(&b) || b.pos > b.n || mlen > b.n - b.pos) {
                break;
            }
            if (!out_bytes(out, max_out, b.p + b.pos, mlen)) {
                break;
            }
            b.pos += mlen;
            continue;
        }
        if (!decode_compressed(&b, mlen, max_back, ring, &ring_n, out, max_out)) {
            break;
        }
        if (islast) {
            ok = true;
            break;
        }
    }

    if (ok && !br_finish(&b)) {
        ok = false;
    }
    embi_brotli_dict_close(b.dict);
    return ok;
}
