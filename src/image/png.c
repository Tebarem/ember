#include "image/png.h"

#include "codec/codec.h"

#include <stdlib.h>

#define PNG_IDAT_OUT 4096u

static const uint8_t kSig[8] = {137, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

/* Adam7 passes as {x origin, y origin, x step, y step}. */
static const uint8_t kPass[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

typedef struct png_dec {
    embi_pixels out; /* owned until the decode succeeds */
    uint32_t w;
    uint32_t h;
    unsigned depth;
    unsigned ctype;
    unsigned channels;
    size_t bpp; /* bytes per complete pixel, the filter distance (at least 1) */
    bool interlaced;
    bool premul;

    bool has_plte;
    uint32_t npal;
    uint8_t pal[256 * 3];
    uint32_t ntrns; /* palette alpha entries */
    uint8_t trns_pal[256];
    bool has_key; /* gray or RGB color key */
    uint16_t key[3];

    /* Scanline machine. */
    int pass;
    uint32_t px0;
    uint32_t py0;
    uint32_t pdx;
    uint32_t pdy;
    uint32_t pw;
    uint32_t ph;
    uint32_t prow;
    size_t rowbytes; /* bytes per row in the current pass */
    size_t maxrow;   /* rowbytes at full width: the buffer size */
    uint8_t *buf;    /* 3 * maxrow + 1 bytes: cur, row, prev */
    uint8_t *cur;    /* filter byte, then rowbytes raw bytes */
    uint8_t *row;    /* reconstructed row */
    uint8_t *prev;   /* previous reconstructed row of this pass (zeros at pass start) */
    size_t fill;
    bool complete;

    embi_inflater *z;
    bool z_done;
    bool started;
} png_dec;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

/* CRC-32 (ISO 3309, the PNG polynomial) by bytes through a 256-entry table. The table is the
 * reflected polynomial 0xEDB88320 indexed by the low byte of the running CRC. */
static const uint32_t kCrcTable[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
    0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu,
    0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
    0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u,
    0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u,
    0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au,
    0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u,
    0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu,
    0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u,
    0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u,
    0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u,
    0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
    0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu,
    0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u,
    0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u,
    0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u,
    0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
    0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu,
    0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu,
    0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u,
    0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u,
    0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc = kCrcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

/* The CRC covers the type and the data, which are contiguous in the file. */
static bool chunk_crc_ok(const uint8_t *type, size_t data_len)
{
    uint32_t c = crc_update(0xFFFFFFFFu, type, 4 + data_len) ^ 0xFFFFFFFFu;
    return c == be32(type + 4 + data_len);
}

static bool type_is(const uint8_t *type, const char *name)
{
    return type[0] == (uint8_t)name[0] && type[1] == (uint8_t)name[1] &&
           type[2] == (uint8_t)name[2] && type[3] == (uint8_t)name[3];
}

static bool depth_ok(unsigned ctype, unsigned depth)
{
    switch (ctype) {
    case 0:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 2:
    case 4:
    case 6:
        return depth == 8 || depth == 16;
    case 3:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    default:
        return false;
    }
}

static unsigned channels_of(unsigned ctype)
{
    switch (ctype) {
    case 0:
    case 3:
        return 1;
    case 2:
        return 3;
    case 4:
        return 2;
    default:
        return 4;
    }
}

static bool parse_ihdr(png_dec *d, const uint8_t *p, uint64_t max_pixels)
{
    uint32_t w = be32(p);
    uint32_t h = be32(p + 4);
    unsigned depth = p[8];
    unsigned ctype = p[9];
    if (w == 0 || h == 0 || w > EMBI_IMAGE_MAX_DIM || h > EMBI_IMAGE_MAX_DIM) {
        return false;
    }
    if ((uint64_t)w * h > max_pixels) {
        return false;
    }
    if (p[10] != 0 || p[11] != 0 || p[12] > 1 || !depth_ok(ctype, depth)) {
        return false;
    }
    d->w = w;
    d->h = h;
    d->depth = depth;
    d->ctype = ctype;
    d->channels = channels_of(ctype);
    d->bpp = ((size_t)d->channels * depth + 7) / 8;
    d->interlaced = p[12] == 1;
    d->maxrow = ((size_t)w * d->channels * depth + 7) / 8;
    return true;
}

/* Geometry of pass p: origin and step. Non-interlaced images have a single pass at (0, 0). */
static void pass_geom(bool interlaced, int p, uint32_t *x0, uint32_t *y0, uint32_t *dx, uint32_t *dy)
{
    if (!interlaced) {
        *x0 = 0;
        *y0 = 0;
        *dx = 1;
        *dy = 1;
        return;
    }
    *x0 = kPass[p][0];
    *y0 = kPass[p][1];
    *dx = kPass[p][2];
    *dy = kPass[p][3];
}

static uint32_t pass_count(uint32_t size, uint32_t origin, uint32_t step)
{
    return size > origin ? (size - origin + step - 1) / step : 0;
}

static size_t row_bytes_for(const png_dec *d, uint32_t pw)
{
    return ((size_t)pw * d->channels * d->depth + 7) / 8;
}

/* Makes the first non-empty pass current, or marks the image complete. */
static void start_pass(png_dec *d)
{
    int np = d->interlaced ? 7 : 1;
    while (d->pass < np) {
        uint32_t x0;
        uint32_t y0;
        uint32_t dx;
        uint32_t dy;
        pass_geom(d->interlaced, d->pass, &x0, &y0, &dx, &dy);
        uint32_t pw = pass_count(d->w, x0, dx);
        uint32_t ph = pass_count(d->h, y0, dy);
        if (pw && ph) {
            d->px0 = x0;
            d->py0 = y0;
            d->pdx = dx;
            d->pdy = dy;
            d->pw = pw;
            d->ph = ph;
            d->prow = 0;
            d->rowbytes = row_bytes_for(d, pw);
            SDL_memset(d->prev, 0, d->maxrow);
            d->fill = 0;
            return;
        }
        d->pass++;
    }
    d->complete = true;
}

/* Total inflated bytes the image needs: one filter byte plus one row, per row of every pass. */
static uint64_t expected_raw(const png_dec *d)
{
    uint64_t total = 0;
    int np = d->interlaced ? 7 : 1;
    for (int p = 0; p < np; p++) {
        uint32_t x0;
        uint32_t y0;
        uint32_t dx;
        uint32_t dy;
        pass_geom(d->interlaced, p, &x0, &y0, &dx, &dy);
        uint32_t pw = pass_count(d->w, x0, dx);
        uint32_t ph = pass_count(d->h, y0, dy);
        if (pw && ph) {
            total += (uint64_t)ph * (row_bytes_for(d, pw) + 1);
        }
    }
    return total;
}

static uint32_t sample_at(const uint8_t *row, size_t idx, unsigned depth)
{
    switch (depth) {
    case 16:
        return ((uint32_t)row[2 * idx] << 8) | row[2 * idx + 1];
    case 8:
        return row[idx];
    default: {
        size_t bit = idx * depth;
        unsigned shift = 8u - depth - (unsigned)(bit & 7u);
        return (row[bit >> 3] >> shift) & ((1u << depth) - 1u);
    }
    }
}

static uint8_t scale_to_8(uint32_t v, unsigned depth)
{
    uint32_t max = (1u << depth) - 1u;
    return (uint8_t)((v * 255u + max / 2u) / max);
}

/* Converts pixel j of a reconstructed row to straight RGBA8. False for a palette index past the
 * palette. */
static bool pixel_rgba(const png_dec *d, const uint8_t *row, size_t j, uint8_t c[4])
{
    unsigned depth = d->depth;
    switch (d->ctype) {
    case 0: {
        uint32_t v = sample_at(row, j, depth);
        c[0] = c[1] = c[2] = scale_to_8(v, depth);
        c[3] = (d->has_key && v == d->key[0]) ? 0 : 255;
        return true;
    }
    case 2: {
        size_t s = j * 3;
        uint32_t r = sample_at(row, s, depth);
        uint32_t g = sample_at(row, s + 1, depth);
        uint32_t b = sample_at(row, s + 2, depth);
        c[0] = scale_to_8(r, depth);
        c[1] = scale_to_8(g, depth);
        c[2] = scale_to_8(b, depth);
        bool keyed = d->has_key && r == d->key[0] && g == d->key[1] && b == d->key[2];
        c[3] = keyed ? 0 : 255;
        return true;
    }
    case 3: {
        uint32_t idx = sample_at(row, j, depth);
        if (idx >= d->npal) {
            return false;
        }
        c[0] = d->pal[3 * idx];
        c[1] = d->pal[3 * idx + 1];
        c[2] = d->pal[3 * idx + 2];
        c[3] = idx < d->ntrns ? d->trns_pal[idx] : 255;
        return true;
    }
    case 4: {
        size_t s = j * 2;
        uint8_t v = scale_to_8(sample_at(row, s, depth), depth);
        c[0] = c[1] = c[2] = v;
        c[3] = scale_to_8(sample_at(row, s + 1, depth), depth);
        return true;
    }
    default: {
        size_t s = j * 4;
        for (unsigned k = 0; k < 4; k++) {
            c[k] = scale_to_8(sample_at(row, s + k, depth), depth);
        }
        return true;
    }
    }
}

static bool emit_row(png_dec *d, const uint8_t *row)
{
    uint32_t y = d->py0 + d->prow * d->pdy;
    uint8_t *dst_row = d->out.px + (size_t)y * d->w * 4;
    for (uint32_t j = 0; j < d->pw; j++) {
        uint32_t x = d->px0 + j * d->pdx;
        uint8_t *p = dst_row + (size_t)x * 4;
        uint8_t c[4];
        if (!pixel_rgba(d, row, j, c)) {
            return false;
        }
        if (d->premul) {
            uint32_t a = c[3];
            for (unsigned k = 0; k < 3; k++) {
                p[k] = (uint8_t)(((uint32_t)c[k] * a + 127u) / 255u);
            }
        } else {
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
        }
        p[3] = c[3];
    }
    return true;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

static void unfilter(uint8_t type, const uint8_t *in, uint8_t *out, const uint8_t *prev, size_t n,
                     size_t bpp)
{
    switch (type) {
    case 0:
        SDL_memcpy(out, in, n);
        break;
    case 1:
        for (size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)(in[i] + (i >= bpp ? out[i - bpp] : 0));
        }
        break;
    case 2:
        for (size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)(in[i] + prev[i]);
        }
        break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            unsigned a = i >= bpp ? out[i - bpp] : 0;
            out[i] = (uint8_t)(in[i] + ((a + prev[i]) >> 1));
        }
        break;
    default:
        for (size_t i = 0; i < n; i++) {
            int a = i >= bpp ? out[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            out[i] = (uint8_t)(in[i] + paeth(a, b, c));
        }
        break;
    }
}

static bool process_row(png_dec *d)
{
    uint8_t ft = d->cur[0];
    if (ft > 4) {
        return false;
    }
    unfilter(ft, d->cur + 1, d->row, d->prev, d->rowbytes, d->bpp);
    if (!emit_row(d, d->row)) {
        return false;
    }
    uint8_t *t = d->row;
    d->row = d->prev;
    d->prev = t;
    d->prow++;
    if (d->prow == d->ph) {
        d->pass++;
        start_pass(d);
    }
    return true;
}

/* Feeds inflated bytes into the scanline machine. */
static bool feed(png_dec *d, const uint8_t *p, size_t n)
{
    while (n > 0) {
        if (d->complete) {
            return false;
        }
        size_t total = d->rowbytes + 1;
        size_t need = total - d->fill;
        size_t take = n < need ? n : need;
        SDL_memcpy(d->cur + d->fill, p, take);
        d->fill += take;
        p += take;
        n -= take;
        if (d->fill == total) {
            d->fill = 0;
            if (!process_row(d)) {
                return false;
            }
        }
    }
    return true;
}

/* Pushes one IDAT chunk's data through the inflater. Stops at the end of the zlib stream. */
static bool feed_idat(png_dec *d, const uint8_t *p, size_t n)
{
    uint8_t ob[PNG_IDAT_OUT];
    size_t pos = 0;
    while (!d->z_done && pos < n) {
        size_t used = 0;
        size_t produced = 0;
        embi_inflate_status st =
            embi_inflate(d->z, p + pos, n - pos, &used, ob, sizeof ob, &produced);
        pos += used;
        if (!feed(d, ob, produced)) {
            return false;
        }
        if (st == EMBI_INFLATE_ERROR) {
            return false;
        }
        if (st == EMBI_INFLATE_DONE) {
            d->z_done = true;
            break;
        }
        if (st == EMBI_INFLATE_NEED_INPUT) {
            break;
        }
        if (used == 0 && produced == 0) {
            return false;
        }
    }
    return true;
}

/* Sums the data lengths of the IDAT chunks from pos on, reading headers only. CRCs are not checked
 * and an IDAT after a malformed chunk is not counted, but the decoder cannot read past the same
 * point, so this bounds the compressed bytes it can consume. */
static uint64_t idat_bytes_ahead(const uint8_t *in, size_t len, size_t pos)
{
    uint64_t total = 0;
    while (len - pos >= 12) {
        uint32_t clen = be32(in + pos);
        if (clen > 0x7fffffffu || len - pos - 12 < clen) {
            break;
        }
        const uint8_t *type = in + pos + 4;
        if (type_is(type, "IDAT")) {
            total += clen;
        } else if (type_is(type, "IEND")) {
            break;
        }
        pos += 12 + clen;
    }
    return total;
}

/* DEFLATE cannot expand more than 1032 bytes per compressed byte (RFC 1951 at its densest, a
 * 258-byte match coded in 2 bits). An image needing more output than that from its IDAT bytes is
 * refused before its pixel buffer is allocated. */
#define PNG_MAX_INFLATE_RATIO 1032u

/* Sizes the decode and allocates its buffers. Fails before any allocation when the image cannot
 * fit size_t or cannot plausibly come from idat_bytes of compressed data. */
static bool init_decode(png_dec *d, uint64_t idat_bytes)
{
    uint64_t raw = expected_raw(d);
    if (raw > SIZE_MAX || (uint64_t)d->w * d->h * 4 > SIZE_MAX) {
        return false;
    }
    if (raw > (idat_bytes + 16) * PNG_MAX_INFLATE_RATIO) {
        return false;
    }
    d->out.w = d->w;
    d->out.h = d->h;
    d->out.px = embi_alloc((size_t)d->w * d->h * 4);
    d->buf = embi_alloc(3 * d->maxrow + 1);
    d->cur = d->buf;
    d->row = d->buf + d->maxrow + 1;
    d->prev = d->buf + 2 * d->maxrow + 1;
    d->z = embi_inflater_new(true, (size_t)raw);
    d->started = true;
    d->pass = 0;
    start_pass(d);
    return true;
}

static void parse_trns(png_dec *d, const uint8_t *data, size_t n)
{
    switch (d->ctype) {
    case 0:
        if (n == 2) {
            d->has_key = true;
            d->key[0] = (uint16_t)be16(data);
        }
        break;
    case 2:
        if (n == 6) {
            d->has_key = true;
            d->key[0] = (uint16_t)be16(data);
            d->key[1] = (uint16_t)be16(data + 2);
            d->key[2] = (uint16_t)be16(data + 4);
        }
        break;
    case 3:
        if (d->has_plte && n <= d->npal) {
            SDL_memcpy(d->trns_pal, data, n);
            d->ntrns = (uint32_t)n;
        }
        break;
    default:
        break;
    }
}

static bool png_run(const uint8_t *in, size_t len, uint64_t max_pixels, png_dec *d)
{
    if (len < 8 || SDL_memcmp(in, kSig, 8) != 0) {
        return false;
    }
    size_t pos = 8;
    bool have_ihdr = false;
    bool seen_idat = false;
    bool iend = false;
    while (!iend) {
        if (len - pos < 12) {
            return false;
        }
        uint32_t clen32 = be32(in + pos);
        if (clen32 > 0x7fffffffu) {
            return false;
        }
        size_t clen = clen32;
        if (len - pos - 12 < clen) {
            return false;
        }
        const uint8_t *type = in + pos + 4;
        const uint8_t *data = in + pos + 8;
        bool critical = (type[0] & 0x20) == 0;
        if (!chunk_crc_ok(type, clen)) {
            if (critical) {
                return false;
            }
            pos += 12 + clen;
            continue;
        }
        if (!have_ihdr) {
            if (!type_is(type, "IHDR") || clen != 13 || !parse_ihdr(d, data, max_pixels)) {
                return false;
            }
            have_ihdr = true;
        } else if (type_is(type, "IHDR")) {
            return false;
        } else if (type_is(type, "PLTE")) {
            if (seen_idat || d->has_plte) {
                return false;
            }
            if (d->ctype == 3) {
                if (clen == 0 || clen % 3 != 0 || clen / 3 > 256 || clen / 3 > (1u << d->depth)) {
                    return false;
                }
                SDL_memcpy(d->pal, data, clen);
                d->npal = (uint32_t)(clen / 3);
                d->has_plte = true;
            }
        } else if (type_is(type, "tRNS")) {
            if (!seen_idat) {
                parse_trns(d, data, clen);
            }
        } else if (type_is(type, "IDAT")) {
            if (d->ctype == 3 && !d->has_plte) {
                return false;
            }
            if (!d->started && !init_decode(d, idat_bytes_ahead(in, len, pos))) {
                return false;
            }
            if (!feed_idat(d, data, clen)) {
                return false;
            }
            seen_idat = true;
        } else if (type_is(type, "IEND")) {
            if (clen != 0) {
                return false;
            }
            iend = true;
        } else if (critical) {
            return false;
        }
        pos += 12 + clen;
    }
    return have_ihdr && seen_idat && d->started && d->complete && d->z_done;
}

bool embi_png_probe(const uint8_t *in, size_t len, uint32_t *w, uint32_t *h)
{
    if (len < 8 + 25 || SDL_memcmp(in, kSig, 8) != 0) {
        return false;
    }
    const uint8_t *c = in + 8;
    if (be32(c) != 13 || !type_is(c + 4, "IHDR") || !chunk_crc_ok(c + 4, 13)) {
        return false;
    }
    png_dec d = {0};
    if (!parse_ihdr(&d, c + 8, UINT64_MAX)) {
        return false;
    }
    *w = d.w;
    *h = d.h;
    return true;
}

bool embi_png_decode(const uint8_t *in, size_t len, uint64_t max_pixels, bool premultiply,
                     embi_pixels *out)
{
    png_dec d = {0};
    d.premul = premultiply;
    bool ok = png_run(in, len, max_pixels, &d);
    if (ok) {
        *out = d.out;
        d.out.px = nullptr;
    }
    if (d.z) {
        embi_inflater_free(d.z);
    }
    if (d.buf) {
        embi_free(d.buf, 3 * d.maxrow + 1);
    }
    if (d.out.px) {
        embi_free(d.out.px, (size_t)d.out.w * d.out.h * 4);
    }
    return ok;
}
