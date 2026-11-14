#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

/*
 * Resumable DEFLATE decoder (RFC 1951) with an optional zlib wrapper (RFC 1950).
 *
 * Huffman codes are decoded one bit at a time from canonical (count, symbol) tables, so the
 * partial state of a symbol survives a NEED_INPUT or OK return. Every state either consumes its
 * bits atomically or saves what it has, which lets the caller resume with more input or output.
 */

#define WSIZE 32768u
#define WMASK (WSIZE - 1u)
#define ADLER_MOD 65521u
#define CHUNK_OUT (64u * 1024u)
#define CONT 100 /* internal: state changed, keep dispatching */

typedef struct huff {
    uint16_t count[16];
    uint16_t sym[320];
} huff;

enum {
    S_ZHDR,
    S_BLOCK,
    S_STORED,
    S_STORED_COPY,
    S_DYN,
    S_CL_LENS,
    S_LENS,
    S_LENS_EXTRA,
    S_SYMS,
    S_LIT,
    S_LEN_EXTRA,
    S_DIST,
    S_DIST_EXTRA,
    S_COPY,
    S_TRAILER,
    S_DONE,
    S_FAIL, /* terminal: a previous call returned EMBI_INFLATE_ERROR */
};

struct embi_inflater {
    uint64_t bitbuf;
    unsigned bitcnt;
    int state;
    bool zlib;
    bool last;
    size_t max_out;
    uint64_t total;
    uint32_t adler_a;
    uint32_t adler_b;
    uint32_t wpos;
    uint32_t stored_left;
    uint32_t copy_left;
    uint32_t copy_dist;
    uint32_t len_val;
    unsigned len_extra;
    unsigned dist_sym;
    unsigned dist_extra;
    uint32_t dist_val;
    unsigned nlen;
    unsigned ndist;
    unsigned ncode;
    unsigned idx;
    unsigned rep_sym;
    unsigned pend_lit;
    unsigned hcode, hfirst, hindex, hlen; /* resumable canonical decode state */
    uint8_t lens[320];
    uint8_t clens[19];
    huff clcode;
    huff lencode;
    huff distcode;
    uint8_t window[WSIZE];
};

/* Input and output cursors for one embi_inflate call. */
struct io {
    const uint8_t *in;
    size_t in_len;
    size_t pos;
    uint8_t *out;
    size_t out_cap;
    size_t produced;
    size_t hashed; /* out[0..hashed) is already in the Adler-32 sums */
};

static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                       33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                       1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* Canonical table from code lengths. Over-subscribed codes fail. Incomplete codes fail unless
 * they hold exactly one code of length 1 (a single distance code, as zlib allows). */
static bool huff_build(huff *h, const uint8_t *lens, unsigned n, bool is_code_len_code)
{
    uint16_t count[16] = {0};
    for (unsigned s = 0; s < n; s++) {
        count[lens[s]]++;
    }
    int left = 1;
    unsigned max_len = 0;
    for (unsigned len = 1; len < 16; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) {
            return false;
        }
        if (count[len]) {
            max_len = len;
        }
    }
    /* An empty code (no lengths at all) is allowed; decoding from it fails. */
    if (left > 0 && max_len != 0 && (is_code_len_code || max_len != 1)) {
        return false;
    }
    SDL_memcpy(h->count, count, sizeof count);
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
    return true;
}

static bool need(embi_inflater *z, struct io *io, unsigned n)
{
    while (z->bitcnt < n) {
        if (io->pos >= io->in_len) {
            return false;
        }
        z->bitbuf |= (uint64_t)io->in[io->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    return true;
}

/* Only call after need(n) succeeded. */
static uint32_t take(embi_inflater *z, unsigned n)
{
    uint32_t v = (uint32_t)(z->bitbuf & ((1ull << n) - 1));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

/* Returns 1 with *sym set, 0 when input ran out (state saved), -1 on an invalid code. */
static int decode_sym(embi_inflater *z, struct io *io, const huff *h, unsigned *sym)
{
    while (z->hlen <= 15) {
        if (z->bitcnt == 0) {
            if (io->pos >= io->in_len) {
                return 0;
            }
            z->bitbuf = io->in[io->pos++];
            z->bitcnt = 8;
        }
        z->hcode |= (unsigned)(z->bitbuf & 1);
        z->bitbuf >>= 1;
        z->bitcnt--;
        int count = h->count[z->hlen];
        if ((int)z->hcode - count < (int)z->hfirst) {
            *sym = h->sym[z->hindex + (z->hcode - z->hfirst)];
            z->hlen = 1;
            z->hcode = z->hfirst = z->hindex = 0;
            return 1;
        }
        z->hindex += (unsigned)count;
        z->hfirst += (unsigned)count;
        z->hfirst <<= 1;
        z->hcode <<= 1;
        z->hlen++;
    }
    z->hlen = 1;
    z->hcode = z->hfirst = z->hindex = 0;
    return -1;
}

/* Adler-32 over n bytes. The sums are reduced every 5552 bytes, the largest run for which the
 * 32-bit accumulators cannot overflow (the zlib NMAX bound). */
static void adler_span(embi_inflater *z, const uint8_t *p, size_t n)
{
    uint32_t a = z->adler_a;
    uint32_t b = z->adler_b;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        for (size_t i = 0; i < k; i++) {
            a += p[i];
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
        p += k;
    }
    z->adler_a = a;
    z->adler_b = b;
}

/* Hashes the output bytes of this call that are not hashed yet. Runs before the zlib trailer is
 * compared and at the end of every call. */
static void adler_flush(embi_inflater *z, struct io *io)
{
    if (z->zlib && io->produced > io->hashed) {
        adler_span(z, io->out + io->hashed, io->produced - io->hashed);
    }
    io->hashed = io->produced;
}

/* Writes one byte. The caller checked output space and the output limit. */
static void put_byte(embi_inflater *z, struct io *io, uint8_t b)
{
    io->out[io->produced++] = b;
    z->window[z->wpos] = b;
    z->wpos = (z->wpos + 1) & WMASK;
    z->total++;
}

static int st_zhdr(embi_inflater *z, struct io *io)
{
    if (!need(z, io, 16)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    unsigned cmf = take(z, 8);
    unsigned flg = take(z, 8);
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
        return EMBI_INFLATE_ERROR;
    }
    z->state = S_BLOCK;
    return CONT;
}

static void build_fixed(embi_inflater *z)
{
    uint8_t lens[288];
    unsigned s = 0;
    for (; s < 144; s++) {
        lens[s] = 8;
    }
    for (; s < 256; s++) {
        lens[s] = 9;
    }
    for (; s < 280; s++) {
        lens[s] = 7;
    }
    for (; s < 288; s++) {
        lens[s] = 8;
    }
    huff_build(&z->lencode, lens, 288, false);
    uint8_t dlens[32];
    for (s = 0; s < 32; s++) {
        dlens[s] = 5;
    }
    huff_build(&z->distcode, dlens, 32, false);
}

static int st_block(embi_inflater *z, struct io *io)
{
    if (!need(z, io, 3)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    z->last = take(z, 1) != 0;
    switch (take(z, 2)) {
    case 0:
        z->state = S_STORED;
        break;
    case 1:
        build_fixed(z);
        z->state = S_SYMS;
        break;
    case 2:
        z->state = S_DYN;
        break;
    default:
        return EMBI_INFLATE_ERROR;
    }
    return CONT;
}

static int st_stored(embi_inflater *z, struct io *io)
{
    /* Bits held in the accumulator are always fewer than 8 here, so they are the byte's fill. */
    take(z, z->bitcnt & 7u);
    if (!need(z, io, 32)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    uint32_t len = take(z, 16);
    uint32_t nlen = take(z, 16);
    if (len != (~nlen & 0xffffu)) {
        return EMBI_INFLATE_ERROR;
    }
    z->stored_left = len;
    z->state = S_STORED_COPY;
    return CONT;
}

static int st_stored_copy(embi_inflater *z, struct io *io)
{
    while (z->stored_left > 0) {
        if (io->pos >= io->in_len) {
            return EMBI_INFLATE_NEED_INPUT;
        }
        if (io->produced >= io->out_cap) {
            return EMBI_INFLATE_OK;
        }
        if (z->total >= z->max_out) {
            return EMBI_INFLATE_ERROR;
        }
        put_byte(z, io, io->in[io->pos++]);
        z->stored_left--;
    }
    z->state = z->last ? S_TRAILER : S_BLOCK;
    return CONT;
}

static int st_dyn(embi_inflater *z, struct io *io)
{
    if (!need(z, io, 14)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    z->nlen = take(z, 5) + 257;
    z->ndist = take(z, 5) + 1;
    z->ncode = take(z, 4) + 4;
    if (z->nlen > 286 || z->ndist > 30) {
        return EMBI_INFLATE_ERROR;
    }
    SDL_memset(z->clens, 0, sizeof z->clens);
    z->idx = 0;
    z->state = S_CL_LENS;
    return CONT;
}

static int st_cl_lens(embi_inflater *z, struct io *io)
{
    while (z->idx < z->ncode) {
        if (!need(z, io, 3)) {
            return EMBI_INFLATE_NEED_INPUT;
        }
        z->clens[kClOrder[z->idx]] = (uint8_t)take(z, 3);
        z->idx++;
    }
    if (!huff_build(&z->clcode, z->clens, 19, true)) {
        return EMBI_INFLATE_ERROR;
    }
    z->idx = 0;
    z->hlen = 1;
    z->hcode = z->hfirst = z->hindex = 0;
    z->state = S_LENS;
    return CONT;
}

static int st_lens(embi_inflater *z, struct io *io)
{
    unsigned total = z->nlen + z->ndist;
    while (z->idx < total) {
        unsigned sym;
        int r = decode_sym(z, io, &z->clcode, &sym);
        if (r == 0) {
            return EMBI_INFLATE_NEED_INPUT;
        }
        if (r < 0) {
            return EMBI_INFLATE_ERROR;
        }
        if (sym < 16) {
            z->lens[z->idx++] = (uint8_t)sym;
        } else {
            z->rep_sym = sym;
            z->state = S_LENS_EXTRA;
            return CONT;
        }
    }
    /* End-of-block must have a code, and both tables must be valid. */
    if (z->lens[256] == 0) {
        return EMBI_INFLATE_ERROR;
    }
    if (!huff_build(&z->lencode, z->lens, z->nlen, false)) {
        return EMBI_INFLATE_ERROR;
    }
    if (!huff_build(&z->distcode, z->lens + z->nlen, z->ndist, false)) {
        return EMBI_INFLATE_ERROR;
    }
    z->state = S_SYMS;
    return CONT;
}

static int st_lens_extra(embi_inflater *z, struct io *io)
{
    unsigned sym = z->rep_sym;
    unsigned extra = sym == 16 ? 2 : (sym == 17 ? 3 : 7);
    if (!need(z, io, extra)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    unsigned reps;
    uint8_t value = 0;
    if (sym == 16) {
        if (z->idx == 0) {
            return EMBI_INFLATE_ERROR;
        }
        value = z->lens[z->idx - 1];
        reps = 3 + take(z, 2);
    } else if (sym == 17) {
        reps = 3 + take(z, 3);
    } else {
        reps = 11 + take(z, 7);
    }
    if (z->idx + reps > z->nlen + z->ndist) {
        return EMBI_INFLATE_ERROR;
    }
    for (unsigned i = 0; i < reps; i++) {
        z->lens[z->idx++] = value;
    }
    z->state = S_LENS;
    return CONT;
}

static int st_syms(embi_inflater *z, struct io *io)
{
    unsigned sym;
    int r = decode_sym(z, io, &z->lencode, &sym);
    if (r == 0) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    if (r < 0) {
        return EMBI_INFLATE_ERROR;
    }
    if (sym < 256) {
        z->pend_lit = sym;
        z->state = S_LIT;
        return CONT;
    }
    if (sym == 256) {
        z->state = z->last ? S_TRAILER : S_BLOCK;
        return CONT;
    }
    if (sym > 285) {
        return EMBI_INFLATE_ERROR;
    }
    z->len_val = kLenBase[sym - 257];
    z->len_extra = kLenExtra[sym - 257];
    z->state = S_LEN_EXTRA;
    return CONT;
}

static int st_lit(embi_inflater *z, struct io *io)
{
    if (io->produced >= io->out_cap) {
        return EMBI_INFLATE_OK;
    }
    if (z->total >= z->max_out) {
        return EMBI_INFLATE_ERROR;
    }
    put_byte(z, io, (uint8_t)z->pend_lit);
    z->state = S_SYMS;
    return CONT;
}

static int st_len_extra(embi_inflater *z, struct io *io)
{
    if (!need(z, io, z->len_extra)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    z->copy_left = z->len_val + take(z, z->len_extra);
    z->state = S_DIST;
    return CONT;
}

static int st_dist(embi_inflater *z, struct io *io)
{
    unsigned sym;
    int r = decode_sym(z, io, &z->distcode, &sym);
    if (r == 0) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    if (r < 0 || sym >= 30) {
        return EMBI_INFLATE_ERROR;
    }
    z->dist_sym = sym;
    z->dist_val = kDistBase[sym];
    z->dist_extra = kDistExtra[sym];
    z->state = S_DIST_EXTRA;
    return CONT;
}

static int st_dist_extra(embi_inflater *z, struct io *io)
{
    if (!need(z, io, z->dist_extra)) {
        return EMBI_INFLATE_NEED_INPUT;
    }
    uint32_t dist = z->dist_val + take(z, z->dist_extra);
    if (dist > z->total) {
        return EMBI_INFLATE_ERROR;
    }
    z->copy_dist = dist;
    z->state = S_COPY;
    return CONT;
}

static int st_copy(embi_inflater *z, struct io *io)
{
    while (z->copy_left > 0) {
        if (io->produced >= io->out_cap) {
            return EMBI_INFLATE_OK;
        }
        if (z->total >= z->max_out) {
            return EMBI_INFLATE_ERROR;
        }
        uint8_t b = z->window[(z->wpos - z->copy_dist) & WMASK];
        put_byte(z, io, b);
        z->copy_left--;
    }
    z->state = S_SYMS;
    return CONT;
}

static int st_trailer(embi_inflater *z, struct io *io)
{
    if (z->zlib) {
        adler_flush(z, io);
        take(z, z->bitcnt & 7u);
        if (!need(z, io, 32)) {
            return EMBI_INFLATE_NEED_INPUT;
        }
        uint32_t hi = take(z, 8);
        uint32_t lo = take(z, 8);
        uint32_t want = (hi << 24) | (lo << 16);
        want |= take(z, 8) << 8;
        want |= take(z, 8);
        if (want != ((z->adler_b << 16) | z->adler_a)) {
            return EMBI_INFLATE_ERROR;
        }
    }
    z->state = S_DONE;
    return CONT;
}

static embi_inflate_status run(embi_inflater *z, struct io *io)
{
    for (;;) {
        int r;
        switch (z->state) {
        case S_ZHDR:
            r = st_zhdr(z, io);
            break;
        case S_BLOCK:
            r = st_block(z, io);
            break;
        case S_STORED:
            r = st_stored(z, io);
            break;
        case S_STORED_COPY:
            r = st_stored_copy(z, io);
            break;
        case S_DYN:
            r = st_dyn(z, io);
            break;
        case S_CL_LENS:
            r = st_cl_lens(z, io);
            break;
        case S_LENS:
            r = st_lens(z, io);
            break;
        case S_LENS_EXTRA:
            r = st_lens_extra(z, io);
            break;
        case S_SYMS:
            r = st_syms(z, io);
            break;
        case S_LIT:
            r = st_lit(z, io);
            break;
        case S_LEN_EXTRA:
            r = st_len_extra(z, io);
            break;
        case S_DIST:
            r = st_dist(z, io);
            break;
        case S_DIST_EXTRA:
            r = st_dist_extra(z, io);
            break;
        case S_COPY:
            r = st_copy(z, io);
            break;
        case S_TRAILER:
            r = st_trailer(z, io);
            break;
        case S_FAIL:
            return EMBI_INFLATE_ERROR;
        default:
            return EMBI_INFLATE_DONE;
        }
        if (r != CONT) {
            return (embi_inflate_status)r;
        }
    }
}

embi_inflater *embi_inflater_new(bool zlib_wrapped, size_t max_out)
{
    embi_inflater *z = embi_calloc(1, sizeof *z);
    z->zlib = zlib_wrapped;
    z->max_out = max_out;
    z->adler_a = 1;
    z->adler_b = 0;
    z->hlen = 1;
    z->state = zlib_wrapped ? S_ZHDR : S_BLOCK;
    return z;
}

void embi_inflater_free(embi_inflater *z)
{
    if (z) {
        embi_free(z, sizeof *z);
    }
}

embi_inflate_status embi_inflate(embi_inflater *z, const uint8_t *in, size_t in_len, size_t *in_used,
                                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    struct io io = {in, in_len, 0, out, out_cap, 0, 0};
    embi_inflate_status st = run(z, &io);
    adler_flush(z, &io);
    if (st == EMBI_INFLATE_ERROR) {
        z->state = S_FAIL; /* sticky: later calls keep failing */
    }
    *in_used = io.pos;
    *out_len = io.produced;
    return st;
}

bool embi_inflate_all(const uint8_t *in, size_t in_len, bool zlib_wrapped, size_t max_out,
                      embi_buf *out)
{
    out->len = 0;
    embi_inflater *z = embi_inflater_new(zlib_wrapped, max_out);
    size_t pos = 0;
    bool ok = false;
    for (;;) {
        /* One byte past the limit is allowed so an overflow is detected inside the decoder. */
        size_t room = max_out - out->len + 1;
        size_t cap = room < CHUNK_OUT ? room : CHUNK_OUT;
        if (!embi_buf_reserve(out, cap)) {
            break;
        }
        size_t used = 0;
        size_t produced = 0;
        embi_inflate_status st = embi_inflate(z, in + pos, in_len - pos, &used, out->data + out->len,
                                              cap, &produced);
        pos += used;
        out->len += produced;
        if (st == EMBI_INFLATE_DONE) {
            ok = out->len <= max_out;
            break;
        }
        if (st != EMBI_INFLATE_OK) {
            break;
        }
    }
    embi_inflater_free(z);
    return ok;
}
