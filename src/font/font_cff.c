/*
 * CFF (version 1) and CFF2 outlines: INDEX and DICT parsing, the Type 2 charstring interpreter
 * (subroutines, hint skipping, seac, flex, CFF2 blend and vsindex).
 *
 * Charstring coordinates are mapped through FontMatrix and scaled to head.unitsPerEm before they
 * leave this file, so outlines come out in the same font units as glyf outlines.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_CFF_MAX_STACK 513u
#define EMBI_CFF1_MAX_SUBR_DEPTH 10u
#define EMBI_CFF2_MAX_SUBR_DEPTH 16u
#define EMBI_CFF_MAX_OPS 1000000u
#define EMBI_CFF_MAX_REGIONS 256u
#define EMBI_CFF_MAX_FDS 256u

/* Converts a number from DICT or charstring data to uint32. Fails unless 0 <= v <= UINT32_MAX,
 * which also rejects NaN and infinities, so the cast is always defined. */
static bool cs_to_u32(double v, uint32_t *out)
{
    if (!(v >= 0.0 && v <= 4294967295.0)) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* ------------------------------------------------------------------ INDEX */

static bool cff_read_off(embi_bytes t, uint64_t pos, uint32_t size, uint64_t *v)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t b = 0;
        if (!bt_u8(t, pos + i, &b)) {
            return false;
        }
        acc = (acc << 8) | b;
    }
    *v = acc;
    return true;
}

/* Parses an INDEX at pos. CFF2 uses 32-bit counts. Returns the offset just past the INDEX. */
static bool cff_index_parse(embi_bytes t, uint64_t pos, bool cff2, embi_cffidx *x,
                            uint64_t *end)
{
    *x = (embi_cffidx){0};
    x->table = t;
    uint32_t count = 0;
    uint64_t hdr = cff2 ? 4 : 2;
    if (cff2) {
        if (!bt_u32(t, pos, &count)) {
            return false;
        }
    } else if (!bt_u16(t, pos, &count)) {
        return false;
    }
    if (count == 0) {
        *end = pos + hdr;
        return pos + hdr <= t.n;
    }
    uint32_t osz = 0;
    if (!bt_u8(t, pos + hdr, &osz) || osz < 1 || osz > 4) {
        return false;
    }
    uint64_t offs = pos + hdr + 1;
    uint64_t arr_end = offs + ((uint64_t)count + 1) * osz;
    if (arr_end > t.n) {
        return false;
    }
    uint64_t last = 0;
    if (!cff_read_off(t, offs + (uint64_t)count * osz, osz, &last) || last == 0) {
        return false;
    }
    uint64_t data = arr_end - 1;
    if (data + last > t.n) {
        return false;
    }
    x->offs = offs;
    x->data = data;
    x->count = count;
    x->offsize = osz;
    *end = data + last;
    return true;
}

bool embi_cffidx_get(const embi_cffidx *x, uint32_t i, embi_bytes *out)
{
    *out = (embi_bytes){nullptr, 0};
    if (i >= x->count) {
        return false;
    }
    uint64_t a = 0, b = 0;
    if (!cff_read_off(x->table, x->offs + (uint64_t)i * x->offsize, x->offsize, &a) ||
        !cff_read_off(x->table, x->offs + (uint64_t)(i + 1) * x->offsize, x->offsize, &b)) {
        return false;
    }
    if (a == 0 || b < a) {
        return false;
    }
    return bt_sub(x->table, x->data + a, b - a, out);
}

/* ------------------------------------------------------------------ DICT */

/* Parses a real operand (nibble encoded) starting after the 30 byte. */
static bool dict_real(embi_bytes d, uint64_t *pos, double *v)
{
    double mant = 0.0;
    int exp10 = 0;
    bool neg = false, in_frac = false, in_exp = false, exp_neg = false, any = false;
    int exp_val = 0;
    for (;;) {
        uint32_t byte = 0;
        if (!bt_u8(d, (*pos)++, &byte)) {
            return false;
        }
        bool stop = false;
        for (int half = 0; half < 2 && !stop; half++) {
            uint32_t nib = half == 0 ? byte >> 4 : byte & 15u;
            if (nib <= 9) {
                any = true;
                if (in_exp) {
                    exp_val = exp_val * 10 + (int)nib;
                    if (exp_val > 10000) {
                        return false;
                    }
                } else {
                    mant = mant * 10.0 + nib;
                    if (in_frac) {
                        exp10--;
                    }
                }
            } else if (nib == 10) {
                if (in_frac || in_exp) {
                    return false;
                }
                in_frac = true;
            } else if (nib == 11) {
                if (in_exp) {
                    return false;
                }
                in_exp = true;
            } else if (nib == 12) {
                if (!in_exp) {
                    return false;
                }
                exp_neg = true;
            } else if (nib == 14) {
                if (any) {
                    return false;
                }
                neg = true;
            } else if (nib == 15) {
                stop = true;
            } else {
                return false;
            }
        }
        if (stop) {
            break;
        }
    }
    if (!any) {
        return false;
    }
    int e = exp10 + (in_exp ? (exp_neg ? -exp_val : exp_val) : 0);
    double s = mant;
    while (e > 0) {
        s *= 10.0;
        e--;
    }
    while (e < 0) {
        s /= 10.0;
        e++;
    }
    *v = neg ? -s : s;
    return true;
}

/* Operands kept per operator. CFF2 allows 513 (blended hint values in a Private DICT can exceed
 * the CFF1 limit of 48). Only the trailing window matters to callers, so older operands are
 * dropped from a ring instead of failing the dict. */
#define EMBI_DICT_MAX_OPERANDS 513u

/* Finds the first occurrence of operator key in dict d and copies its last cap operands to vals.
 * Returns 1 when found, 0 when absent, -1 when the dict is malformed. */
static int dict_find(embi_bytes d, uint32_t key, double *vals, uint32_t cap, uint32_t *n)
{
    uint64_t pos = 0;
    double ops[EMBI_DICT_MAX_OPERANDS];
    uint32_t nops = 0;  /* operands since the last operator, capped at the ring size */
    uint32_t head = 0;  /* next ring slot */
    *n = 0;
    while (pos < d.n) {
        uint32_t b0 = 0;
        if (!bt_u8(d, pos, &b0)) {
            return -1;
        }
        /* 22 (vsindex) and 24 (VarStore) are CFF2 operators; 25..27 are reserved. */
        if (b0 <= 24) {
            uint32_t op = b0;
            pos++;
            if (b0 == 12) {
                uint32_t b1 = 0;
                if (!bt_u8(d, pos++, &b1)) {
                    return -1;
                }
                op = 1200 + b1;
            }
            if (op == key) {
                uint32_t m = nops < cap ? nops : cap;
                for (uint32_t i = 0; i < m; i++) {
                    vals[i] = ops[(head + EMBI_DICT_MAX_OPERANDS - m + i) % EMBI_DICT_MAX_OPERANDS];
                }
                *n = m;
                return 1;
            }
            nops = 0;
            continue;
        }
        double v = 0;
        if (b0 == 28) {
            int32_t iv = 0;
            if (!bt_i16(d, pos + 1, &iv)) {
                return -1;
            }
            v = iv;
            pos += 3;
        } else if (b0 == 29) {
            int32_t iv = 0;
            if (!bt_i32(d, pos + 1, &iv)) {
                return -1;
            }
            v = iv;
            pos += 5;
        } else if (b0 == 30) {
            uint64_t p = pos + 1;
            if (!dict_real(d, &p, &v)) {
                return -1;
            }
            pos = p;
        } else if (b0 >= 32 && b0 <= 246) {
            v = (double)b0 - 139;
            pos++;
        } else if (b0 >= 247 && b0 <= 250) {
            uint32_t b1 = 0;
            if (!bt_u8(d, pos + 1, &b1)) {
                return -1;
            }
            v = (double)(b0 - 247) * 256 + b1 + 108;
            pos += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            uint32_t b1 = 0;
            if (!bt_u8(d, pos + 1, &b1)) {
                return -1;
            }
            v = -(double)(b0 - 251) * 256 - b1 - 108;
            pos += 2;
        } else {
            return -1; /* 22..27, 31, 255 are reserved in DICT data */
        }
        ops[head] = v;
        head = (head + 1) % EMBI_DICT_MAX_OPERANDS;
        if (nops < EMBI_DICT_MAX_OPERANDS) {
            nops++;
        }
    }
    return 0;
}

/* Single integer operand. Absent is success with *present false. */
static bool dict_int(embi_bytes d, uint32_t key, uint32_t *v, bool *present)
{
    double vals[1];
    uint32_t n = 0;
    int r = dict_find(d, key, vals, 1, &n);
    *present = r == 1 && n >= 1;
    if (r < 0) {
        return false;
    }
    if (!*present) {
        return true;
    }
    return cs_to_u32(vals[0], v);
}

/* ------------------------------------------------------------------ open */

void embi_cff_close(embi_cff *c)
{
    if (c == nullptr) {
        return;
    }
    if (c->fds != nullptr) {
        embi_free(c->fds, (size_t)c->nfds * sizeof(embi_cff_fd));
    }
    embi_free(c, sizeof(embi_cff));
}

/* Reads the private dict at off/size: Subrs, vsindex. */
/* Reads a private dict at off with size bytes: Subrs and (CFF2) vsindex. A zero-size dict has
 * no subroutines. */
static bool cff_private(embi_bytes t, double off, double size, bool cff2, embi_cff_fd *fd)
{
    fd->subrs = (embi_cffidx){0};
    fd->vsindex = 0;
    uint32_t poff = 0, psize = 0;
    if (!cs_to_u32(off, &poff) || !cs_to_u32(size, &psize)) {
        return false;
    }
    if (psize == 0) {
        return true;
    }
    embi_bytes pd;
    if (!bt_sub(t, poff, psize, &pd)) {
        return false;
    }
    double v[2];
    uint32_t n = 0;
    int r = dict_find(pd, 19, v, 1, &n);
    if (r < 0) {
        return false;
    }
    if (r == 1 && n == 1) {
        uint32_t subrs_off = 0;
        if (!cs_to_u32(v[0], &subrs_off)) {
            return false;
        }
        uint64_t end = 0;
        if (!cff_index_parse(t, (uint64_t)poff + subrs_off, cff2, &fd->subrs, &end)) {
            return false;
        }
    }
    if (cff2) {
        r = dict_find(pd, 22, v, 1, &n);
        if (r < 0) {
            return false;
        }
        if (r == 1 && n == 1 && !cs_to_u32(v[0], &fd->vsindex)) {
            return false;
        }
    }
    return true;
}

/* Reads the top FontMatrix. Absent keeps the default. Non-finite entries fail the font. */
static bool cff_fontmatrix(embi_bytes d, double fm[6], bool *present)
{
    double v[6];
    uint32_t n = 0;
    *present = false;
    int r = dict_find(d, 1207, v, 6, &n);
    if (r < 0) {
        return false;
    }
    if (r == 1 && n == 6) {
        for (int i = 0; i < 6; i++) {
            if (v[i] - v[i] != 0.0) { /* NaN or infinity */
                return false;
            }
            fm[i] = v[i];
        }
        *present = true;
    }
    return true;
}

bool embi_cff_open(embi_font *f, embi_bytes t)
{
    uint32_t major = 0, hdr = 0;
    if (!bt_u8(t, 0, &major) || !bt_u8(t, 2, &hdr) || hdr < 4) {
        return false;
    }
    if (major != 1 && major != 2) {
        return false;
    }
    embi_cff *c = embi_calloc(1, sizeof(embi_cff));
    if (c == nullptr) {
        return false;
    }
    f->cff_data = c;
    c->cff2 = major == 2;
    c->table = t;
    uint64_t end = 0;
    embi_bytes top;
    embi_cffidx topidx = {0};
    if (!c->cff2) {
        uint64_t p = hdr;
        embi_cffidx names, strs;
        if (!cff_index_parse(t, p, false, &names, &end)) {
            return false;
        }
        p = end;
        if (!cff_index_parse(t, p, false, &topidx, &end)) {
            return false;
        }
        p = end;
        if (!cff_index_parse(t, p, false, &strs, &end)) {
            return false;
        }
        p = end;
        if (!cff_index_parse(t, p, false, &c->gsubrs, &end)) {
            return false;
        }
        if (topidx.count != 1 || !embi_cffidx_get(&topidx, 0, &top)) {
            return false;
        }
    } else {
        uint32_t tlen = 0;
        if (!bt_u16(t, 3, &tlen) || hdr + (uint64_t)tlen > t.n) {
            return false;
        }
        if (!bt_sub(t, hdr, tlen, &top)) {
            return false;
        }
        if (!cff_index_parse(t, hdr + tlen, true, &c->gsubrs, &end)) {
            return false;
        }
    }

    /* Top DICT. */
    uint32_t cs_off = 0;
    bool present = false;
    if (!dict_int(top, 17, &cs_off, &present) || !present) {
        return false;
    }
    if (!cff_index_parse(t, cs_off, c->cff2, &c->charstrings, &end)) {
        return false;
    }
    if (!c->cff2) {
        uint32_t ctype = 2;
        if (!dict_int(top, 1206, &ctype, &present) || ctype != 2) {
            return false; /* Type 1 charstrings are not supported */
        }
        uint32_t cset = 0;
        if (!dict_int(top, 15, &cset, &present)) {
            return false;
        }
        c->charset_off = present ? cset : 0;
    }
    /* Without a FontMatrix the charstring units are already head.unitsPerEm units (FreeType does
     * the same), not the 1/1000 default of the CFF spec. */
    bool fm_present = false;
    if (!cff_fontmatrix(top, c->fm, &fm_present)) {
        return false;
    }
    if (!fm_present) {
        c->fm[0] = 1.0 / (double)f->metrics.units_per_em;
        c->fm[3] = c->fm[0];
    }
    if (c->cff2) {
        uint32_t vs_off = 0;
        if (!dict_int(top, 24, &vs_off, &present)) {
            return false;
        }
        if (present) {
            /* The VarStore starts with a uint16 length; the ItemVariationStore follows it. */
            uint32_t vs_len = 0;
            if (!bt_u16(t, vs_off, &vs_len) || !bt_tail(t, (uint64_t)vs_off + 2, &c->vstore)) {
                return false;
            }
        }
    }

    uint32_t fd_off = 0, fds_off = 0;
    bool has_fdarray = false, has_fdselect = false;
    if (!dict_int(top, 1236, &fd_off, &has_fdarray) ||
        !dict_int(top, 1237, &fds_off, &has_fdselect)) {
        return false;
    }

    /* Font dicts and private dicts. */
    if (has_fdarray) {
        embi_cffidx fdarr;
        if (!cff_index_parse(t, fd_off, c->cff2, &fdarr, &end)) {
            return false;
        }
        if (fdarr.count == 0 || fdarr.count > EMBI_CFF_MAX_FDS) {
            return false;
        }
        c->nfds = fdarr.count;
        c->fds = embi_calloc(c->nfds, sizeof(embi_cff_fd));
        if (c->fds == nullptr) {
            return false;
        }
        for (uint32_t i = 0; i < c->nfds; i++) {
            embi_bytes fdd;
            if (!embi_cffidx_get(&fdarr, i, &fdd)) {
                return false;
            }
            double pv[2];
            uint32_t pn = 0;
            if (dict_find(fdd, 18, pv, 2, &pn) != 1 || pn != 2) {
                return false;
            }
            if (!cff_private(t, pv[1], pv[0], c->cff2, &c->fds[i])) {
                return false;
            }
        }
    } else {
        c->nfds = 1;
        c->fds = embi_calloc(1, sizeof(embi_cff_fd));
        if (c->fds == nullptr) {
            return false;
        }
        double pv[2];
        uint32_t pn = 0;
        int r = dict_find(top, 18, pv, 2, &pn);
        if (r < 0) {
            return false;
        }
        if (r == 1 && pn == 2) {
            if (!cff_private(t, pv[1], pv[0], c->cff2, &c->fds[0])) {
                return false;
            }
        }
    }
    if (has_fdselect) {
        uint32_t fmt = 0;
        if (!bt_u8(t, fds_off, &fmt) || (fmt != 0 && fmt != 3 && !(fmt == 4 && c->cff2))) {
            return false;
        }
        c->fdselect_fmt = fmt;
        if (!bt_tail(t, fds_off, &c->fdselect)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ lookups */

static bool cff_fd_for_gid(const embi_cff *c, uint32_t gid, uint32_t *fd)
{
    *fd = 0;
    if (c->nfds <= 1 || c->fdselect.n == 0) {
        return true;
    }
    if (c->fdselect_fmt == 0) {
        uint32_t v = 0;
        if (!bt_u8(c->fdselect, 1 + (uint64_t)gid, &v)) {
            return false;
        }
        *fd = v;
    } else {
        /* Format 3: u16 nRanges, {u16 first, u8 fd}, u16 sentinel. Format 4 (CFF2): u32 nRanges,
         * {u32 first, u16 fd}, u32 sentinel. Both are binary searched. */
        bool f4 = c->fdselect_fmt == 4;
        uint64_t hdr = f4 ? 5u : 3u, rsz = f4 ? 6u : 3u;
        uint32_t nr = 0;
        if (!(f4 ? bt_u32(c->fdselect, 1, &nr) : bt_u16(c->fdselect, 1, &nr)) || nr == 0) {
            return false;
        }
        uint32_t lo = 0, hi = nr; /* last record with first <= gid */
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t first = 0;
            uint64_t rec = hdr + rsz * mid;
            if (!(f4 ? bt_u32(c->fdselect, rec, &first) : bt_u16(c->fdselect, rec, &first))) {
                return false;
            }
            if (first <= gid) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            return false;
        }
        uint32_t idx = lo - 1, nfd = 0, sentinel = 0;
        uint64_t rec = hdr + rsz * idx;
        if (!(f4 ? bt_u16(c->fdselect, rec + 4, &nfd) : bt_u8(c->fdselect, rec + 2, &nfd)) ||
            !(f4 ? bt_u32(c->fdselect, hdr + rsz * nr, &sentinel)
                 : bt_u16(c->fdselect, hdr + rsz * nr, &sentinel))) {
            return false;
        }
        if (gid >= sentinel) {
            return false;
        }
        *fd = nfd;
    }
    return *fd < c->nfds;
}

/* Standard Encoding code -> standard string id. Codes 32..126 map to SID code-31; the 54 upper
 * codes below are the ones with names in CFF's standard strings 96..149, in code order. */
static bool std_sid(uint32_t code, uint32_t *sid)
{
    static const uint8_t high[54] = {
        161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 177, 178, 179,
        180, 182, 183, 184, 185, 186, 187, 188, 189, 191, 193, 194, 195, 196, 197, 198, 199, 200,
        202, 203, 205, 206, 207, 208, 225, 227, 232, 233, 234, 235, 241, 245, 248, 249, 250, 251};
    if (code >= 32 && code <= 126) {
        *sid = code - 31;
        return true;
    }
    for (uint32_t i = 0; i < 54; i++) {
        if (high[i] == code) {
            *sid = 96 + i;
            return true;
        }
    }
    return false;
}

/* glyph id whose charset entry is sid (CFF1 only). */
static bool cff_gid_for_sid(const embi_cff *c, uint32_t n_glyphs, uint32_t sid, uint32_t *gid)
{
    if (c->charset_off == 0) { /* ISOAdobe: gid == sid */
        if (sid < 229 && sid < n_glyphs) {
            *gid = sid;
            return true;
        }
        return false;
    }
    if (c->charset_off <= 2) {
        return false;
    }
    embi_bytes cs;
    if (!bt_tail(c->table, c->charset_off, &cs)) {
        return false;
    }
    uint32_t fmt = 0;
    if (!bt_u8(cs, 0, &fmt)) {
        return false;
    }
    uint32_t g = 1;
    uint64_t pos = 1;
    if (fmt == 0) {
        for (; g < n_glyphs; g++) {
            uint32_t s = 0;
            if (!bt_u16(cs, pos, &s)) {
                return false;
            }
            pos += 2;
            if (s == sid) {
                *gid = g;
                return true;
            }
        }
        return false;
    }
    if (fmt != 1 && fmt != 2) {
        return false;
    }
    while (g < n_glyphs) {
        uint32_t first = 0, left = 0;
        if (!bt_u16(cs, pos, &first)) {
            return false;
        }
        if (fmt == 1) {
            if (!bt_u8(cs, pos + 2, &left)) {
                return false;
            }
            pos += 3;
        } else {
            if (!bt_u16(cs, pos + 2, &left)) {
                return false;
            }
            pos += 4;
        }
        if (sid >= first && sid <= first + left) {
            uint32_t found = g + (sid - first);
            if (found >= n_glyphs) {
                return false;
            }
            *gid = found;
            return true;
        }
        g += left + 1;
    }
    return false;
}

/* ------------------------------------------------------------------ interpreter */

typedef struct cs_state {
    const embi_font *f;
    const embi_cff *c;
    const int16_t *coords;
    uint32_t ncoords;
    const embi_cff_fd *fd;
    embi_olb b;
    double stack[EMBI_CFF_MAX_STACK];
    uint32_t sp;
    double trans[32];
    uint32_t nstems;
    bool width_seen;
    bool done;
    double x, y;
    uint32_t vsindex;
    uint32_t ops;
    float tol;
    bool in_seac; /* inside a seac base or accent: a nested seac is invalid */
} cs_state;

static uint32_t cs_max_depth(const cs_state *st)
{
    return st->c->cff2 ? EMBI_CFF2_MAX_SUBR_DEPTH : EMBI_CFF1_MAX_SUBR_DEPTH;
}

static bool cs_push(cs_state *st, double v)
{
    if (st->sp >= EMBI_CFF_MAX_STACK) {
        return false;
    }
    st->stack[st->sp++] = v;
    return true;
}

static bool cs_need(const cs_state *st, uint32_t n)
{
    return st->sp >= n;
}

static bool cs_move(cs_state *st, double dx, double dy)
{
    st->x += dx;
    st->y += dy;
    return embi_olb_move(&st->b, (embi_pt){(float)st->x, (float)st->y});
}

static bool cs_line(cs_state *st, double dx, double dy)
{
    st->x += dx;
    st->y += dy;
    return embi_olb_line(&st->b, (embi_pt){(float)st->x, (float)st->y});
}

static bool cs_curve(cs_state *st, double a, double b, double c, double d, double e, double f)
{
    double x1 = st->x + a, y1 = st->y + b;
    double x2 = x1 + c, y2 = y1 + d;
    double x3 = x2 + e, y3 = y2 + f;
    st->x = x3;
    st->y = y3;
    return embi_olb_cubic(&st->b, (embi_pt){(float)x1, (float)y1}, (embi_pt){(float)x2, (float)y2},
                          (embi_pt){(float)x3, (float)y3}, st->tol);
}

static uint32_t cs_bias(uint32_t n)
{
    return n < 1240 ? 107 : (n < 33900 ? 1131 : 32768);
}

/* Blend for CFF2: stack holds N base values, then N*K deltas, then N is on top. */
static bool cs_blend(cs_state *st)
{
    if (!cs_need(st, 1)) {
        return false;
    }
    double nd = st->stack[--st->sp];
    if (!(nd >= 1 && nd <= 512)) {
        return false;
    }
    uint32_t n = (uint32_t)nd;
    double scal[EMBI_CFF_MAX_REGIONS];
    uint32_t k = 0;
    if (!embi_ivs_scalars(st->c->vstore, st->vsindex, st->coords, st->ncoords, scal,
                          EMBI_CFF_MAX_REGIONS, &k)) {
        return false;
    }
    uint64_t need = (uint64_t)n * (k + 1);
    if (need > st->sp) {
        return false;
    }
    uint32_t start = st->sp - (uint32_t)need;
    for (uint32_t i = 0; i < n; i++) {
        double v = st->stack[start + i];
        for (uint32_t j = 0; j < k; j++) {
            v += st->stack[start + n + i * k + j] * scal[j];
        }
        st->stack[start + i] = v;
    }
    st->sp = start + n;
    return true;
}

static bool cs_exec(cs_state *st, embi_bytes cs, uint32_t depth);

/* Executes glyph gid's charstring into the builder, translated by (dx, dy). */
static bool cs_sub_glyph(cs_state *st, uint32_t gid, double dx, double dy, uint32_t depth)
{
    if (depth > cs_max_depth(st) || gid >= st->c->charstrings.count) {
        return false;
    }
    embi_bytes cs;
    if (!embi_cffidx_get(&st->c->charstrings, gid, &cs)) {
        return false;
    }
    uint32_t start = st->b.o->quads.len;
    double sx = st->x, sy = st->y;
    uint32_t ssp = st->sp;
    uint32_t snst = st->nstems;
    bool swidth = st->width_seen;
    bool sdone = st->done;
    st->x = st->y = 0;
    st->sp = 0;
    st->nstems = 0;
    st->width_seen = false;
    st->done = false;
    bool ok = cs_exec(st, cs, depth + 1) && embi_olb_close(&st->b);
    if (ok) {
        for (uint32_t i = start; i < st->b.o->quads.len; i++) {
            embi_quad *q = &st->b.o->quads.data[i];
            embi_pt *pts[3] = {&q->p0, &q->p1, &q->p2};
            for (int k = 0; k < 3; k++) {
                pts[k]->x += (float)dx;
                pts[k]->y += (float)dy;
            }
        }
    }
    st->x = sx;
    st->y = sy;
    st->sp = ssp;
    st->nstems = snst;
    st->width_seen = swidth;
    st->done = sdone;
    return ok;
}

/* Handles the optional width operand. It is the first stack value when the operator takes an
 * odd number of arguments (or one extra), and only the first stack-clearing operator may carry it.
 */
static void cs_width(cs_state *st, bool extra)
{
    if (!st->width_seen && !st->c->cff2) {
        if (extra && st->sp > 0) {
            for (uint32_t i = 1; i < st->sp; i++) {
                st->stack[i - 1] = st->stack[i];
            }
            st->sp--;
        }
    }
    st->width_seen = true;
}

#define CS_NEED(n)                                                                             \
    do {                                                                                       \
        if (st->sp < (n)) {                                                                    \
            return false;                                                                      \
        }                                                                                      \
    } while (0)

static bool cs_escape(cs_state *st, uint32_t op, embi_bytes cs, uint64_t *pos, uint32_t depth)
{
    (void)cs;
    (void)pos;
    (void)depth;
    double *s = st->stack;
    switch (op) {
    case 0: /* dotsection: obsolete hint operator, ignored */
        return true;
    case 3: /* and */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] = (s[st->sp - 1] != 0 && s[st->sp] != 0) ? 1 : 0;
        return true;
    case 4: /* or */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] = (s[st->sp - 1] != 0 || s[st->sp] != 0) ? 1 : 0;
        return true;
    case 5: /* not */
        CS_NEED(1);
        s[st->sp - 1] = s[st->sp - 1] == 0 ? 1 : 0;
        return true;
    case 9: /* abs */
        CS_NEED(1);
        s[st->sp - 1] = s[st->sp - 1] < 0 ? -s[st->sp - 1] : s[st->sp - 1];
        return true;
    case 10: /* add */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] += s[st->sp];
        return true;
    case 11: /* sub */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] -= s[st->sp];
        return true;
    case 12: /* div */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] = s[st->sp] == 0 ? 0 : s[st->sp - 1] / s[st->sp];
        return true;
    case 14: /* neg */
        CS_NEED(1);
        s[st->sp - 1] = -s[st->sp - 1];
        return true;
    case 15: /* eq */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] = s[st->sp - 1] == s[st->sp] ? 1 : 0;
        return true;
    case 18: /* drop */
        CS_NEED(1);
        st->sp--;
        return true;
    case 20: { /* put */
        CS_NEED(2);
        double idx = s[st->sp - 1];
        double val = s[st->sp - 2];
        st->sp -= 2;
        uint32_t slot = 0;
        if (!cs_to_u32(idx, &slot) || slot > 31) {
            return false;
        }
        st->trans[slot] = val;
        return true;
    }
    case 21: { /* get */
        CS_NEED(1);
        double idx = s[st->sp - 1];
        uint32_t slot = 0;
        if (!cs_to_u32(idx, &slot) || slot > 31) {
            return false;
        }
        s[st->sp - 1] = st->trans[slot];
        return true;
    }
    case 22: /* ifelse */
        CS_NEED(4);
        {
            double v2 = s[st->sp - 1], v1 = s[st->sp - 2];
            double s2 = s[st->sp - 3], s1 = s[st->sp - 4];
            st->sp -= 3;
            s[st->sp - 1] = v1 <= v2 ? s1 : s2;
        }
        return true;
    case 23: /* random: a fixed value keeps output deterministic */
        return cs_push(st, 0.5);
    case 24: /* mul */
        CS_NEED(2);
        st->sp--;
        s[st->sp - 1] *= s[st->sp];
        return true;
    case 26: /* sqrt */
        CS_NEED(1);
        s[st->sp - 1] = s[st->sp - 1] > 0 ? SDL_sqrt(s[st->sp - 1]) : 0;
        return true;
    case 27: /* dup */
        CS_NEED(1);
        return cs_push(st, s[st->sp - 1]);
    case 28: /* exch */
        CS_NEED(2);
        {
            double t = s[st->sp - 1];
            s[st->sp - 1] = s[st->sp - 2];
            s[st->sp - 2] = t;
        }
        return true;
    case 29: { /* index */
        CS_NEED(1);
        double i = s[st->sp - 1];
        if (!(i >= 0)) { /* negative (or NaN) indexes the top element */
            i = 0;
        }
        if (i + 2.0 > (double)st->sp) {
            return false;
        }
        uint32_t k = 0;
        if (!cs_to_u32(i, &k)) {
            return false;
        }
        s[st->sp - 1] = s[st->sp - 2 - k];
        return true;
    }
    case 30: { /* roll: rotate the top N values right by J (negative J rotates left) */
        CS_NEED(2);
        double jd = s[st->sp - 1];
        double nd = s[st->sp - 2];
        st->sp -= 2;
        if (!(nd >= 0.0 && nd <= (double)st->sp) || !(jd > -1e15 && jd < 1e15)) {
            return false;
        }
        uint32_t n = (uint32_t)nd;
        if (n == 0) {
            return true;
        }
        int64_t jm = (int64_t)jd % (int64_t)n;
        if (jm < 0) {
            jm += n;
        }
        /* Right rotation by jm: reverse all, then reverse each part. */
        uint32_t base = st->sp - n;
        double *w = s + base;
        for (uint32_t lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
            double t = w[lo];
            w[lo] = w[hi];
            w[hi] = t;
        }
        uint32_t r = (uint32_t)jm;
        for (uint32_t lo = 0, hi = r == 0 ? 0 : r - 1; r != 0 && lo < hi; lo++, hi--) {
            double t = w[lo];
            w[lo] = w[hi];
            w[hi] = t;
        }
        for (uint32_t lo = r, hi = n - 1; lo < hi; lo++, hi--) {
            double t = w[lo];
            w[lo] = w[hi];
            w[hi] = t;
        }
        return true;
    }
    case 34: /* hflex */
        CS_NEED(7);
        {
            double *a = s;
            if (!cs_curve(st, a[0], 0, a[1], a[2], a[3], 0)) {
                return false;
            }
            if (!cs_curve(st, a[4], 0, a[5], -a[2], a[6], 0)) {
                return false;
            }
        }
        st->sp = 0;
        return true;
    case 35: /* flex */
        CS_NEED(12);
        if (!cs_curve(st, s[0], s[1], s[2], s[3], s[4], s[5]) ||
            !cs_curve(st, s[6], s[7], s[8], s[9], s[10], s[11])) {
            return false;
        }
        st->sp = 0;
        return true;
    case 36: /* hflex1 */
        CS_NEED(9);
        {
            double *a = s;
            double dy6 = -(a[1] + a[3] + a[7]);
            if (!cs_curve(st, a[0], a[1], a[2], a[3], a[4], 0) ||
                !cs_curve(st, a[5], 0, a[6], a[7], a[8], dy6)) {
                return false;
            }
        }
        st->sp = 0;
        return true;
    case 37: /* flex1 */
        CS_NEED(11);
        {
            double *a = s;
            double dx = a[0] + a[2] + a[4] + a[6] + a[8];
            double dy = a[1] + a[3] + a[5] + a[7] + a[9];
            double dx6, dy6;
            if (dx < 0) {
                dx = -dx;
            }
            if (dy < 0) {
                dy = -dy;
            }
            double sdx = a[0] + a[2] + a[4] + a[6] + a[8];
            double sdy = a[1] + a[3] + a[5] + a[7] + a[9];
            if (dx > dy) {
                dx6 = a[10];
                dy6 = -sdy;
            } else {
                dx6 = -sdx;
                dy6 = a[10];
            }
            if (!cs_curve(st, a[0], a[1], a[2], a[3], a[4], a[5]) ||
                !cs_curve(st, a[6], a[7], a[8], a[9], dx6, dy6)) {
                return false;
            }
        }
        st->sp = 0;
        return true;
    default:
        return false;
    }
}

/* Runs one charstring (or subroutine). Returns false on malformed data. Sets st->done on
 * endchar; a return from a subroutine just ends this call. */
static bool cs_exec(cs_state *st, embi_bytes cs, uint32_t depth)
{
    if (depth > cs_max_depth(st)) {
        return false;
    }
    uint64_t pos = 0;
    while (pos < cs.n) {
        if (++st->ops > EMBI_CFF_MAX_OPS) {
            return false;
        }
        uint32_t v = cs.p[pos];
        if (v >= 32 || v == 28) {
            if (v == 28) {
                int32_t iv = 0;
                if (!bt_i16(cs, pos + 1, &iv) || !cs_push(st, iv)) {
                    return false;
                }
                pos += 3;
            } else if (v <= 246) {
                if (!cs_push(st, (double)v - 139)) {
                    return false;
                }
                pos++;
            } else if (v <= 250) {
                uint32_t b1 = 0;
                if (!bt_u8(cs, pos + 1, &b1) || !cs_push(st, (double)(v - 247) * 256 + b1 + 108)) {
                    return false;
                }
                pos += 2;
            } else if (v <= 254) {
                uint32_t b1 = 0;
                if (!bt_u8(cs, pos + 1, &b1) ||
                    !cs_push(st, -(double)(v - 251) * 256 - b1 - 108)) {
                    return false;
                }
                pos += 2;
            } else {
                int32_t iv = 0;
                if (!bt_i32(cs, pos + 1, &iv) || !cs_push(st, iv / 65536.0)) {
                    return false;
                }
                pos += 5;
            }
            continue;
        }
        pos++;
        switch (v) {
        case 1:  /* hstem */
        case 3:  /* vstem */
        case 18: /* hstemhm */
        case 23: /* vstemhm */
            cs_width(st, (st->sp & 1) != 0);
            st->nstems += st->sp / 2;
            st->sp = 0;
            break;
        case 19: /* hintmask */
        case 20: /* cntrmask */
            cs_width(st, (st->sp & 1) != 0);
            st->nstems += st->sp / 2;
            st->sp = 0;
            pos += (st->nstems + 7u) / 8u;
            if (pos > cs.n) {
                return false;
            }
            break;
        case 21: /* rmoveto */
            cs_width(st, st->sp > 2);
            CS_NEED(2);
            if (!cs_move(st, st->stack[st->sp - 2], st->stack[st->sp - 1])) {
                return false;
            }
            st->sp = 0;
            break;
        case 22: /* hmoveto */
            cs_width(st, st->sp > 1);
            CS_NEED(1);
            if (!cs_move(st, st->stack[st->sp - 1], 0)) {
                return false;
            }
            st->sp = 0;
            break;
        case 4: /* vmoveto */
            cs_width(st, st->sp > 1);
            CS_NEED(1);
            if (!cs_move(st, 0, st->stack[st->sp - 1])) {
                return false;
            }
            st->sp = 0;
            break;
        case 5: /* rlineto */
            if (st->sp < 2 || (st->sp & 1)) {
                return false;
            }
            for (uint32_t i = 0; i < st->sp; i += 2) {
                if (!cs_line(st, st->stack[i], st->stack[i + 1])) {
                    return false;
                }
            }
            st->sp = 0;
            break;
        case 6:  /* hlineto */
        case 7:  /* vlineto */
            CS_NEED(1);
            for (uint32_t i = 0; i < st->sp; i++) {
                bool horiz = ((i & 1) == 0) == (v == 6);
                double d = st->stack[i];
                if (!(horiz ? cs_line(st, d, 0) : cs_line(st, 0, d))) {
                    return false;
                }
            }
            st->sp = 0;
            break;
        case 8: /* rrcurveto */
            if (st->sp == 0 || st->sp % 6 != 0) {
                return false;
            }
            for (uint32_t i = 0; i < st->sp; i += 6) {
                double *a = st->stack + i;
                if (!cs_curve(st, a[0], a[1], a[2], a[3], a[4], a[5])) {
                    return false;
                }
            }
            st->sp = 0;
            break;
        case 24: /* rcurveline: curves, then one line taking the last two operands */
            if (st->sp < 8 || (st->sp - 2) % 6 != 0) {
                return false;
            }
            for (uint32_t i = 0; i + 6 <= st->sp - 2; i += 6) {
                double *a = st->stack + i;
                if (!cs_curve(st, a[0], a[1], a[2], a[3], a[4], a[5])) {
                    return false;
                }
            }
            if (!cs_line(st, st->stack[st->sp - 2], st->stack[st->sp - 1])) {
                return false;
            }
            st->sp = 0;
            break;
        case 25: /* rlinecurve */
            if (st->sp < 8 || (st->sp - 6) % 2 != 0) {
                return false;
            }
            for (uint32_t i = 0; i + 6 < st->sp; i += 2) {
                if (!cs_line(st, st->stack[i], st->stack[i + 1])) {
                    return false;
                }
            }
            {
                double *a = st->stack + st->sp - 6;
                if (!cs_curve(st, a[0], a[1], a[2], a[3], a[4], a[5])) {
                    return false;
                }
            }
            st->sp = 0;
            break;
        case 26: /* vvcurveto */
        {
            uint32_t i = 0;
            double dx1 = 0;
            if (st->sp & 1) {
                dx1 = st->stack[0];
                i = 1;
            }
            if (st->sp < i + 4 || (st->sp - i) % 4 != 0) {
                return false;
            }
            for (; i < st->sp; i += 4) {
                double *a = st->stack + i;
                if (!cs_curve(st, dx1, a[0], a[1], a[2], 0, a[3])) {
                    return false;
                }
                dx1 = 0;
            }
            st->sp = 0;
            break;
        }
        case 27: /* hhcurveto */
        {
            uint32_t i = 0;
            double dy1 = 0;
            if (st->sp & 1) {
                dy1 = st->stack[0];
                i = 1;
            }
            if (st->sp < i + 4 || (st->sp - i) % 4 != 0) {
                return false;
            }
            for (; i < st->sp; i += 4) {
                double *a = st->stack + i;
                if (!cs_curve(st, a[0], dy1, a[1], a[2], a[3], 0)) {
                    return false;
                }
                dy1 = 0;
            }
            st->sp = 0;
            break;
        }
        case 30: /* vhcurveto */
        case 31: /* hvcurveto */
        {
            bool horiz = v == 31;
            uint32_t i = 0;
            if (st->sp < 4) {
                return false;
            }
            while (st->sp - i >= 4) {
                bool last = st->sp - i == 5;
                double *a = st->stack + i;
                double extra = last ? a[4] : 0;
                if (horiz) {
                    if (!cs_curve(st, a[0], 0, a[1], a[2], extra, a[3])) {
                        return false;
                    }
                } else {
                    if (!cs_curve(st, 0, a[0], a[1], a[2], a[3], extra)) {
                        return false;
                    }
                }
                i += last ? 5 : 4;
                horiz = !horiz;
            }
            if (i != st->sp) {
                return false;
            }
            st->sp = 0;
            break;
        }
        case 10: /* callsubr */
        case 29: /* callgsubr */
        {
            CS_NEED(1);
            double idx = st->stack[--st->sp];
            const embi_cffidx *x = v == 10 ? &st->fd->subrs : &st->c->gsubrs;
            double bias = cs_bias(x->count);
            double ii = idx + bias;
            uint32_t sidx = 0;
            if (!cs_to_u32(ii, &sidx) || sidx >= x->count) {
                return false;
            }
            embi_bytes sub;
            if (!embi_cffidx_get(x, sidx, &sub) || !cs_exec(st, sub, depth + 1)) {
                return false;
            }
            if (st->done) {
                return true;
            }
            break;
        }
        case 11: /* return */
            return true;
        case 14: /* endchar */
        {
            cs_width(st, st->sp == 1 || st->sp == 5);
            if (st->sp == 4) {
                /* seac: the base and accent are plain glyphs. A seac inside a seac component
                 * would allow cycles (A -> B -> A), so it is rejected. */
                uint32_t bchar = 0, achar = 0;
                double adx = st->stack[0], ady = st->stack[1];
                if (st->in_seac || st->c->cff2 || !cs_to_u32(st->stack[2], &bchar) ||
                    !cs_to_u32(st->stack[3], &achar)) {
                    return false;
                }
                uint32_t bsid = 0, asid = 0, bgid = 0, agid = 0;
                if (!std_sid(bchar, &bsid) || !std_sid(achar, &asid) ||
                    !cff_gid_for_sid(st->c, st->f->metrics.num_glyphs, bsid, &bgid) ||
                    !cff_gid_for_sid(st->c, st->f->metrics.num_glyphs, asid, &agid)) {
                    return false;
                }
                st->in_seac = true;
                bool ok = cs_sub_glyph(st, bgid, 0, 0, depth + 1) &&
                          cs_sub_glyph(st, agid, adx, ady, depth + 1);
                st->in_seac = false;
                if (!ok) {
                    return false;
                }
            } else if (st->sp != 0) {
                return false;
            }
            st->sp = 0;
            if (!embi_olb_close(&st->b)) {
                return false;
            }
            st->done = true;
            return true;
        }
        case 15: /* vsindex */
            CS_NEED(1);
            if (!cs_to_u32(st->stack[st->sp - 1], &st->vsindex)) {
                return false;
            }
            st->sp--;
            break;
        case 16: /* blend */
            if (!cs_blend(st)) {
                return false;
            }
            break;
        case 12: { /* escape */
            uint32_t e = 0;
            if (!bt_u8(cs, pos++, &e) || !cs_escape(st, e, cs, &pos, depth)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

/* Rewrites quads from index `from` onward by the affine m (a b c d e f). */
static void xf_quads(embi_outline *o, uint32_t from, const double m[6])
{
    for (uint32_t i = from; i < o->quads.len; i++) {
        embi_quad *q = &o->quads.data[i];
        embi_pt *pts[3] = {&q->p0, &q->p1, &q->p2};
        for (int k = 0; k < 3; k++) {
            double x = pts[k]->x, y = pts[k]->y;
            pts[k]->x = (float)(m[0] * x + m[2] * y + m[4]);
            pts[k]->y = (float)(m[1] * x + m[3] * y + m[5]);
        }
    }
}

bool embi_cff_outline(const embi_font *f, uint32_t gid, const int16_t *coords, embi_outline *out)
{
    const embi_cff *c = f->cff_data;
    if (c == nullptr || gid >= c->charstrings.count) {
        return false;
    }
    embi_bytes cs;
    if (!embi_cffidx_get(&c->charstrings, gid, &cs)) {
        return false;
    }
    uint32_t fdi = 0;
    if (!cff_fd_for_gid(c, gid, &fdi)) {
        return false;
    }
    /* The interpreter state is about 4.6 KB; it lives on the stack so a glyph costs no heap work.
     * Its depth is bounded by the subroutine and seac limits. */
    cs_state st;
    SDL_memset(&st, 0, sizeof st);
    st.f = f;
    st.c = c;
    st.coords = coords;
    st.ncoords = f->axis_count;
    st.fd = &c->fds[fdi];
    st.vsindex = c->fds[fdi].vsindex;
    embi_olb_init(&st.b, out);
    /* One part in 1024 of an em, expressed in charstring units (FontMatrix scales them). */
    double fa = c->fm[0] > 0 ? c->fm[0] : 0.001;
    st.tol = (float)(1.0 / (1024.0 * fa));
    uint32_t base = out->quads.len;
    bool ok = cs_exec(&st, cs, 0) && embi_olb_close(&st.b);
    if (ok) {
        double upm = f->metrics.units_per_em;
        double m[6] = {c->fm[0] * upm, c->fm[1] * upm, c->fm[2] * upm,
                       c->fm[3] * upm, c->fm[4] * upm, c->fm[5] * upm};
        xf_quads(out, base, m);
    }
    return ok;
}
