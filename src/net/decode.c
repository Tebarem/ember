/*
 * Content-Encoding decoding (N1): gzip and deflate stream through src/codec/inflate.c, br is buffered and decoded when
 * the body ends (the Brotli decoder in src/codec is one-shot). One coding per response; identity is no decoder.
 *
 * Memory bounds: gzip and deflate produce at most DEC_STEP bytes per call (embi_net_decoder_feed / _pump); input that
 * is not decoded yet waits inside the decoder, so a small compressed body cannot expand into one huge buffer. The
 * caller applies backpressure between steps. Brotli, which cannot stream, is capped (BR_MAX_IN, BR_MAX_OUT) and its
 * result is also handed out in DEC_STEP pieces. There is no cap on the total size of a streamed body.
 */
#include "net_int.h"

#define DEC_STEP ((size_t)1 << 20)
#define BR_MAX_IN ((size_t)16 << 20)
#define BR_MAX_OUT ((size_t)64 << 20)
#define GZ_HEADER_MAX 65536u

typedef enum dec_kind {
    DEC_GZIP,
    DEC_DEFLATE,
    DEC_BR,
} dec_kind;

typedef enum gz_state {
    GZ_HEADER,
    GZ_BODY,
    GZ_TRAILER,
    GZ_DONE,
} gz_state;

struct embi_net_decoder {
    dec_kind kind;
    embi_inflater *z;
    gz_state gz;
    embi_buf pend;  /* gzip and deflate: input not decoded yet; br: the whole compressed body */
    embi_buf ready; /* br: the decoded body, handed out DEC_STEP bytes at a time */
    size_t ready_off;
    bool fin_called, fin_ok;
    uint32_t crc;
    uint32_t total; /* gzip ISIZE is mod 2^32 */
    uint8_t trailer[8];
    uint32_t trailer_n;
    bool deflate_started;
    bool done;
};

static uint32_t g_crc_table[256];
static SDL_InitState g_crc_init;

static void crc_init(void)
{
    if (!SDL_ShouldInit(&g_crc_init)) {
        return;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        g_crc_table[n] = c;
    }
    SDL_SetInitialized(&g_crc_init, true);
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

embi_net_decoder *embi_net_decoder_new(embi_str coding)
{
    coding = embi_header_trim(coding);
    if (coding.len == 0 || embi_str_ieq(coding, EMBI_STR("identity"))) {
        return nullptr;
    }
    dec_kind k;
    if (embi_str_ieq(coding, EMBI_STR("gzip")) || embi_str_ieq(coding, EMBI_STR("x-gzip"))) {
        k = DEC_GZIP;
    } else if (embi_str_ieq(coding, EMBI_STR("deflate"))) {
        k = DEC_DEFLATE;
    } else if (embi_str_ieq(coding, EMBI_STR("br"))) {
        k = DEC_BR;
    } else {
        return nullptr;
    }
    embi_net_decoder *d = embi_net_zalloc(sizeof *d);
    d->kind = k;
    crc_init();
    return d;
}

bool embi_net_decoder_active(const embi_net_decoder *d)
{
    return d != nullptr;
}

void embi_net_decoder_free(embi_net_decoder *d)
{
    if (d == nullptr) {
        return;
    }
    embi_inflater_free(d->z);
    embi_buf_free(&d->pend);
    embi_buf_free(&d->ready);
    embi_net_mfree(d);
}

/* Length of the gzip header in b, 0 when more bytes are needed, -1 when invalid. */
static int64_t gz_header_len(const uint8_t *b, size_t n)
{
    if (n >= 1 && b[0] != 0x1F) {
        return -1;
    }
    if (n >= 2 && b[1] != 0x8B) {
        return -1;
    }
    if (n >= 3 && b[2] != 8) {
        return -1;
    }
    if (n < 10) {
        return 0;
    }
    uint8_t flg = b[3];
    if (flg & 0xE0) {
        return -1;
    }
    size_t p = 10;
    if (flg & 4) { /* FEXTRA */
        if (n < p + 2) {
            return 0;
        }
        size_t xlen = (size_t)b[p] | ((size_t)b[p + 1] << 8);
        p += 2 + xlen;
        if (n < p) {
            return 0;
        }
    }
    for (int bit = 8; bit <= 16; bit <<= 1) { /* FNAME, FCOMMENT: zero-terminated */
        if (flg & bit) {
            while (p < n && b[p] != 0) {
                p++;
            }
            if (p >= n) {
                return 0;
            }
            p++;
        }
    }
    if (flg & 2) { /* FHCRC */
        p += 2;
        if (n < p) {
            return 0;
        }
    }
    return (int64_t)p;
}

/* Inflates in[0..len) into out, producing at most about `room` bytes. Returns false on error; *done is set when the
 * deflate stream ended; *used_total is how much input was consumed (less than len when the room ran out). */
static bool inflate_some(embi_net_decoder *d, const uint8_t *in, size_t len, size_t *used_total, embi_buf *out, bool *done,
                         bool crc, size_t room)
{
    uint8_t tmp[16384];
    size_t used_sum = 0, produced = 0;
    *done = false;
    *used_total = 0;
    for (;;) {
        size_t used = 0, outn = 0;
        embi_inflate_status st = embi_inflate(d->z, in + used_sum, len - used_sum, &used, tmp, sizeof tmp, &outn);
        if (outn > 0) {
            if (!embi_buf_append(out, tmp, outn)) {
                return false;
            }
            produced += outn;
            if (crc) {
                d->crc = crc_update(d->crc, tmp, outn);
                d->total += (uint32_t)outn;
            }
        }
        used_sum += used;
        if (st == EMBI_INFLATE_ERROR) {
            return false;
        }
        if (st == EMBI_INFLATE_DONE) {
            *done = true;
            break;
        }
        if (st == EMBI_INFLATE_NEED_INPUT || (used == 0 && outn == 0)) {
            break;
        }
        if (produced >= room) {
            break; /* the rest of the input stays for the next step */
        }
    }
    *used_total = used_sum;
    return true;
}

/* Decodes from d->pend, appending at most about DEC_STEP bytes to out. */
static bool run(embi_net_decoder *d, embi_buf *out)
{
    size_t start = out->len;
    size_t off = 0; /* consumed from pend */
    bool ok = true;
    while (ok) {
        const uint8_t *in = d->pend.data + off;
        size_t len = d->pend.len - off;
        size_t room = out->len - start >= DEC_STEP ? 0 : DEC_STEP - (out->len - start);
        if (d->kind == DEC_DEFLATE) {
            if (d->done) {
                off = d->pend.len; /* bytes after the end are ignored */
                break;
            }
            if (room == 0) {
                break;
            }
            if (!d->deflate_started) {
                if (len < 2) {
                    break;
                }
                /* "deflate" is zlib-wrapped per the RFC, but some servers send a raw stream: look at the first two bytes. */
                bool zlib = (in[0] & 0x0F) == 8 && (((unsigned)in[0] << 8) | in[1]) % 31 == 0;
                d->z = embi_inflater_new(zlib, SIZE_MAX);
                d->deflate_started = true;
            }
            size_t used;
            bool done;
            ok = inflate_some(d, in, len, &used, out, &done, false, room);
            off += used;
            d->done = done;
            if (!done && used == len) {
                break;
            }
            continue;
        }
        /* gzip */
        if (d->gz == GZ_DONE) {
            off = d->pend.len;
            break;
        }
        if (d->gz == GZ_HEADER) {
            int64_t hl = gz_header_len(in, len);
            if (hl < 0 || (hl == 0 && len > GZ_HEADER_MAX)) {
                ok = false;
                break;
            }
            if (hl == 0) {
                break;
            }
            off += (size_t)hl;
            d->z = embi_inflater_new(false, SIZE_MAX);
            d->gz = GZ_BODY;
            continue;
        }
        if (d->gz == GZ_BODY) {
            if (room == 0) {
                break;
            }
            size_t used;
            bool done;
            ok = inflate_some(d, in, len, &used, out, &done, true, room);
            off += used;
            if (done) {
                d->gz = GZ_TRAILER;
                continue;
            }
            if (used == len) {
                break;
            }
            continue;
        }
        /* GZ_TRAILER */
        while (off < d->pend.len && d->trailer_n < 8) {
            d->trailer[d->trailer_n++] = d->pend.data[off++];
        }
        if (d->trailer_n < 8) {
            break;
        }
        uint32_t crc = (uint32_t)d->trailer[0] | ((uint32_t)d->trailer[1] << 8) | ((uint32_t)d->trailer[2] << 16) |
                       ((uint32_t)d->trailer[3] << 24);
        uint32_t isize = (uint32_t)d->trailer[4] | ((uint32_t)d->trailer[5] << 8) | ((uint32_t)d->trailer[6] << 16) |
                         ((uint32_t)d->trailer[7] << 24);
        if (crc != d->crc || isize != d->total) {
            ok = false;
            break;
        }
        d->gz = GZ_DONE; /* anything after the first member is ignored */
    }
    if (off > 0) {
        memmove(d->pend.data, d->pend.data + off, d->pend.len - off);
        d->pend.len -= off;
    }
    return ok;
}

/* Hands out up to DEC_STEP bytes of the decoded Brotli body. */
static bool br_slice(embi_net_decoder *d, embi_buf *out)
{
    size_t n = d->ready.len - d->ready_off;
    if (n > DEC_STEP) {
        n = DEC_STEP;
    }
    if (n > 0 && !embi_buf_append(out, d->ready.data + d->ready_off, n)) {
        return false;
    }
    d->ready_off += n;
    if (d->ready_off == d->ready.len) {
        embi_buf_free(&d->ready);
        d->ready_off = 0;
    }
    return true;
}

bool embi_net_decoder_feed(embi_net_decoder *d, const uint8_t *in, size_t len, embi_buf *out)
{
    if (d->kind == DEC_BR) {
        if (d->pend.len + len > BR_MAX_IN) {
            return false;
        }
        return embi_buf_append(&d->pend, in, len);
    }
    if (len > 0 && !embi_buf_append(&d->pend, in, len)) {
        return false;
    }
    return run(d, out);
}

bool embi_net_decoder_pending(const embi_net_decoder *d)
{
    if (d->kind == DEC_BR) {
        return d->ready.len > d->ready_off;
    }
    if (d->kind == DEC_DEFLATE) {
        return d->pend.len > 0 && !d->done && (d->deflate_started || d->pend.len >= 2);
    }
    return d->pend.len > 0 && d->gz != GZ_DONE && (d->gz != GZ_HEADER || gz_header_len(d->pend.data, d->pend.len) != 0) &&
           (d->gz != GZ_TRAILER || d->trailer_n >= 8 || d->pend.len + d->trailer_n >= 8);
}

bool embi_net_decoder_pump(embi_net_decoder *d, embi_buf *out)
{
    if (d->kind == DEC_BR) {
        return br_slice(d, out);
    }
    return run(d, out);
}

bool embi_net_decoder_finish(embi_net_decoder *d, embi_buf *out)
{
    if (d->fin_called) {
        return d->fin_ok;
    }
    d->fin_called = true;
    switch (d->kind) {
    case DEC_BR:
        d->fin_ok = true;
        if (d->pend.len > 0) {
            d->fin_ok = embi_brotli_decode(d->pend.data, d->pend.len, BR_MAX_OUT, &d->ready);
            embi_buf_free(&d->pend);
            d->ready_off = 0;
            if (d->fin_ok) {
                d->fin_ok = br_slice(d, out);
            }
        }
        return d->fin_ok;
    case DEC_DEFLATE:
        d->fin_ok = d->done;
        return d->fin_ok;
    case DEC_GZIP:
        d->fin_ok = d->gz == GZ_DONE;
        return d->fin_ok;
    }
    return false;
}
