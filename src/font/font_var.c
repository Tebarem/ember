/*
 * Variations: ItemVariationStore (region scalars and deltas), DeltaSetIndexMap, gvar tuple
 * deltas with IUP, avar (v1) segment maps, axis normalization, and MVAR metric deltas.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_IVS_MAX_REGIONS 256u
#define EMBI_GVAR_MAX_AXES 64u

static int16_t coord_at(const int16_t *coords, uint32_t nc, uint32_t a)
{
    return (coords != nullptr && a < nc) ? coords[a] : 0;
}

/* Scalar of one region for the given normalized coords. Returns false for a bad region index. */
static bool region_scalar(embi_bytes ivs, uint32_t rl, uint32_t region, const int16_t *coords,
                          uint32_t nc, double *out)
{
    uint32_t axes = 0, nreg = 0;
    if (!bt_u16(ivs, rl, &axes) || !bt_u16(ivs, rl + 2, &nreg) || region >= nreg) {
        return false;
    }
    double s = 1.0;
    for (uint32_t a = 0; a < axes; a++) {
        int32_t start = 0, peak = 0, end = 0;
        uint64_t rec = rl + 4ull + (uint64_t)region * axes * 6 + (uint64_t)a * 6;
        if (!bt_i16(ivs, rec, &start) || !bt_i16(ivs, rec + 2, &peak) ||
            !bt_i16(ivs, rec + 4, &end)) {
            return false;
        }
        if (peak == 0 || start > peak || peak > end) {
            continue;
        }
        if (start < 0 && end > 0) {
            continue; /* HarfBuzz/FreeType: a region axis spanning zero is ignored */
        }
        int32_t c = coord_at(coords, nc, a);
        if (c < start || c > end) {
            *out = 0.0;
            return true;
        }
        if (c == peak) {
            continue;
        }
        if (c < peak) {
            s *= (double)(c - start) / (double)(peak - start);
        } else {
            s *= (double)(end - c) / (double)(end - peak);
        }
    }
    *out = s;
    return true;
}

/* ItemVariationStore header checks. Sets *rl (region list offset) and returns the VarData bytes
 * for outer, with its counts. */
typedef struct ivs_data {
    embi_bytes vd;   /* VarData table */
    uint32_t rl;     /* VarRegionList offset in ivs */
    uint32_t items;  /* itemCount */
    uint32_t nshort; /* shortDeltaCount */
    uint32_t nregion; /* regionIndexCount */
    bool wide;       /* LONG_WORDS */
} ivs_data;

static bool ivs_open(embi_bytes ivs, uint32_t outer, ivs_data *d)
{
    uint32_t fmt = 0, rl = 0, count = 0;
    if (!bt_u16(ivs, 0, &fmt) || fmt != 1 || !bt_u32(ivs, 2, &rl) || !bt_u16(ivs, 6, &count)) {
        return false;
    }
    if (outer >= count) {
        return false;
    }
    uint32_t voff = 0;
    if (!bt_u32(ivs, 8 + 4ull * outer, &voff)) {
        return false;
    }
    if (!bt_tail(ivs, voff, &d->vd)) {
        return false;
    }
    uint32_t wdc = 0;
    if (!bt_u16(d->vd, 0, &d->items) || !bt_u16(d->vd, 2, &wdc) || !bt_u16(d->vd, 4, &d->nregion)) {
        return false;
    }
    d->wide = (wdc & 0x8000u) != 0;
    d->nshort = wdc & 0x7FFFu;
    if (d->nshort > d->nregion) {
        return false;
    }
    d->rl = rl;
    return true;
}

double embi_ivs_delta(embi_bytes ivs, uint32_t outer, uint32_t inner, const int16_t *coords,
                      uint32_t ncoords)
{
    ivs_data d;
    if (!ivs_open(ivs, outer, &d) || inner >= d.items) {
        return 0.0;
    }
    uint64_t row = 6ull + 2ull * d.nregion;
    uint64_t rowsize = d.wide ? 4ull * d.nshort + 2ull * (d.nregion - d.nshort)
                              : 2ull * d.nshort + (d.nregion - d.nshort);
    uint64_t rp = row + inner * rowsize;
    double sum = 0.0;
    for (uint32_t j = 0; j < d.nregion; j++) {
        uint32_t ridx = 0;
        if (!bt_u16(d.vd, 6 + 2ull * j, &ridx)) {
            return 0.0;
        }
        double sc = 0.0;
        if (!region_scalar(ivs, d.rl, ridx, coords, ncoords, &sc)) {
            return 0.0;
        }
        if (sc == 0.0) {
            continue;
        }
        int32_t v = 0;
        uint64_t p = 0;
        if (j < d.nshort) {
            p = rp + (d.wide ? 4ull * j : 2ull * j);
            if (d.wide) {
                if (!bt_i32(d.vd, p, &v)) {
                    return 0.0;
                }
            } else {
                if (!bt_i16(d.vd, p, &v)) {
                    return 0.0;
                }
            }
        } else {
            uint64_t k = j - d.nshort;
            p = rp + (d.wide ? 4ull * d.nshort + 2ull * k : 2ull * d.nshort + k);
            if (d.wide) {
                if (!bt_i16(d.vd, p, &v)) {
                    return 0.0;
                }
            } else {
                uint32_t b = 0;
                if (!bt_u8(d.vd, p, &b)) {
                    return 0.0;
                }
                v = (int8_t)b;
            }
        }
        sum += (double)v * sc;
    }
    return sum;
}

bool embi_ivs_scalars(embi_bytes ivs, uint32_t outer, const int16_t *coords, uint32_t ncoords,
                      double *scal, uint32_t cap, uint32_t *count)
{
    ivs_data d;
    if (!ivs_open(ivs, outer, &d) || d.nregion > cap) {
        return false;
    }
    for (uint32_t j = 0; j < d.nregion; j++) {
        uint32_t ridx = 0;
        if (!bt_u16(d.vd, 6 + 2ull * j, &ridx) || !region_scalar(ivs, d.rl, ridx, coords, ncoords, &scal[j])) {
            return false;
        }
    }
    *count = d.nregion;
    return true;
}

bool embi_var_map(embi_bytes map, uint32_t i, uint32_t *packed)
{
    if (map.n == 0) {
        *packed = i;
        return true;
    }
    uint32_t fmt = 0, ef = 0, count = 0;
    uint64_t data = 0;
    if (!bt_u8(map, 0, &fmt) || !bt_u8(map, 1, &ef)) {
        return false;
    }
    if (fmt == 0) {
        if (!bt_u16(map, 2, &count)) {
            return false;
        }
        data = 4;
    } else if (fmt == 1) {
        if (!bt_u32(map, 2, &count)) {
            return false;
        }
        data = 6;
    } else {
        return false;
    }
    uint32_t esz = ((ef >> 4) & 3u) + 1u;
    uint32_t bits = (ef & 15u) + 1u;
    if (count == 0) {
        *packed = i;
        return true;
    }
    uint32_t idx = i < count ? i : count - 1;
    uint32_t v = 0;
    for (uint32_t k = 0; k < esz; k++) {
        uint32_t b = 0;
        if (!bt_u8(map, data + (uint64_t)idx * esz + k, &b)) {
            return false;
        }
        v = (v << 8) | b;
    }
    uint32_t outer = bits >= 32 ? 0 : v >> bits;
    uint32_t inner = bits >= 32 ? v : v & ((1u << bits) - 1u);
    if (outer > 0xFFFFu || inner > 0xFFFFu) {
        return false;
    }
    *packed = (outer << 16) | inner;
    return true;
}

/* ------------------------------------------------------------------ gvar */

/* Cursor over packed gvar data. */
typedef struct gcur {
    embi_bytes b;
    uint64_t pos;
} gcur;

static bool gc_u8(gcur *c, uint32_t *v)
{
    bool ok = bt_u8(c->b, c->pos, v);
    c->pos++;
    return ok;
}

/* Packed point numbers. *all is set when the set is "every point" (count zero). Writes the
 * point numbers to pl (capacity cap) and their count to *n. */
static bool gc_points(gcur *c, uint32_t *pl, uint32_t cap, uint32_t *n, bool *all)
{
    uint32_t count = 0, b = 0;
    if (!gc_u8(c, &b)) {
        return false;
    }
    if (b & 0x80u) {
        uint32_t b2 = 0;
        if (!gc_u8(c, &b2)) {
            return false;
        }
        count = ((b & 0x7Fu) << 8) | b2;
    } else {
        count = b;
    }
    *all = count == 0;
    *n = 0;
    if (*all) {
        return true;
    }
    if (count > cap) {
        return false;
    }
    uint32_t last = 0;
    uint32_t got = 0;
    while (got < count) {
        uint32_t ctrl = 0;
        if (!gc_u8(c, &ctrl)) {
            return false;
        }
        uint32_t run = (ctrl & 0x7Fu) + 1u;
        bool words = (ctrl & 0x80u) != 0;
        for (uint32_t r = 0; r < run; r++) {
            if (got >= count) {
                return false;
            }
            uint32_t d = 0;
            if (words) {
                int32_t sv = 0;
                if (!bt_i16(c->b, c->pos, &sv)) {
                    return false;
                }
                c->pos += 2;
                /* Word deltas are unsigned in the packed point-number encoding. */
                d = (uint32_t)(sv & 0xFFFF);
                last += d;
            } else {
                if (!gc_u8(c, &d)) {
                    return false;
                }
                last += d;
            }
            pl[got++] = last;
        }
    }
    *n = got;
    return true;
}

/* Packed deltas for count values into out (stride 1). */
static bool gc_deltas(gcur *c, uint32_t count, float *out, const uint32_t *idx, bool all)
{
    uint32_t got = 0;
    while (got < count) {
        uint32_t ctrl = 0;
        if (!gc_u8(c, &ctrl)) {
            return false;
        }
        uint32_t run = (ctrl & 0x3Fu) + 1u;
        for (uint32_t r = 0; r < run; r++) {
            if (got >= count) {
                return false;
            }
            int32_t v = 0;
            if (ctrl & 0x80u) {
                v = 0;
            } else if (ctrl & 0x40u) {
                if (!bt_i16(c->b, c->pos, &v)) {
                    return false;
                }
                c->pos += 2;
            } else {
                uint32_t b = 0;
                if (!gc_u8(c, &b)) {
                    return false;
                }
                v = (int8_t)b;
            }
            uint32_t dst = all ? got : idx[got];
            out[dst] = (float)v;
            got++;
        }
    }
    return true;
}

/* Scalar of a tuple (peak, optional intermediate region) at coords. */
static double tuple_scalar(const int32_t *peak, const int32_t *lo_in, const int32_t *hi_in,
                           const int16_t *coords, uint32_t nc)
{
    double s = 1.0;
    for (uint32_t a = 0; a < nc; a++) {
        int32_t p = peak[a];
        if (p == 0) {
            continue;
        }
        int32_t lo = lo_in ? lo_in[a] : (p < 0 ? p : 0);
        int32_t hi = hi_in ? hi_in[a] : (p > 0 ? p : 0);
        int32_t c = coords[a];
        if (c == p) {
            continue;
        }
        if (c <= lo || c >= hi) {
            return 0.0;
        }
        if (c < p) {
            s *= (double)(c - lo) / (double)(p - lo);
        } else {
            s *= (double)(hi - c) / (double)(hi - p);
        }
    }
    return s;
}

/* IUP for one coordinate over one contour. orig and delta are the coordinate arrays. */
static void iup_contour(uint32_t s, uint32_t e, const float *orig, float *delta,
                        const uint8_t *touched)
{
    uint32_t n = e - s + 1;
    uint32_t first = n;
    for (uint32_t i = 0; i < n; i++) {
        if (touched[s + i]) {
            first = i;
            break;
        }
    }
    if (first == n) {
        return;
    }
    uint32_t cur = first;
    for (;;) {
        uint32_t nxt = cur;
        uint32_t steps = 0;
        do {
            nxt = (nxt + 1) % n;
            steps++;
        } while (!touched[s + nxt] && steps <= n);
        /* Untouched points strictly between cur and nxt, wrapping. */
        float c1 = orig[s + cur], c2 = orig[s + nxt];
        float d1 = delta[s + cur], d2 = delta[s + nxt];
        for (uint32_t k = (cur + 1) % n; k != nxt; k = (k + 1) % n) {
            float c = orig[s + k];
            float d;
            if (c1 == c2) {
                d = d1 == d2 ? d1 : 0.0f;
            } else {
                float cl = c1 < c2 ? c1 : c2;
                float ch = c1 < c2 ? c2 : c1;
                float dl = c1 < c2 ? d1 : d2;
                float dh = c1 < c2 ? d2 : d1;
                if (c <= cl) {
                    d = dl;
                } else if (c >= ch) {
                    d = dh;
                } else {
                    d = dl + (c - cl) * (dh - dl) / (ch - cl);
                }
            }
            delta[s + k] = d;
        }
        cur = nxt;
        if (cur == first) {
            break;
        }
    }
}

bool embi_gvar_glyph_deltas(const embi_font *f, uint32_t gid, const int16_t *coords,
                            const embi_pt *orig, uint32_t npts, const uint16_t *ends,
                            uint32_t ncont, float *dx, float *dy)
{
    uint32_t total = npts + 4;
    for (uint32_t i = 0; i < total; i++) {
        dx[i] = 0.0f;
        dy[i] = 0.0f;
    }
    embi_bytes gv = f->gvar;
    if (gv.n == 0 || coords == nullptr || f->axis_count == 0) {
        return true;
    }
    uint32_t major = 0, axes = 0, shared_n = 0, gcount = 0, flags = 0, data_arr = 0;
    uint32_t shared_off = 0;
    if (!bt_u16(gv, 0, &major) || major != 1 || !bt_u16(gv, 4, &axes) || !bt_u16(gv, 6, &shared_n) ||
        !bt_u32(gv, 8, &shared_off) || !bt_u16(gv, 12, &gcount) || !bt_u16(gv, 14, &flags) ||
        !bt_u32(gv, 16, &data_arr)) {
        return false;
    }
    if (axes != f->axis_count || axes > EMBI_GVAR_MAX_AXES) {
        return false;
    }
    if (gid >= gcount) {
        return true;
    }
    uint32_t a = 0, b = 0;
    bool longoff = (flags & 1u) != 0;
    if (longoff) {
        if (!bt_u32(gv, 20 + 4ull * gid, &a) || !bt_u32(gv, 20 + 4ull * gid + 4, &b)) {
            return false;
        }
    } else {
        if (!bt_u16(gv, 20 + 2ull * gid, &a) || !bt_u16(gv, 20 + 2ull * gid + 2, &b)) {
            return false;
        }
        a *= 2;
        b *= 2;
    }
    if (b < a) {
        return false;
    }
    if (b == a) {
        return true;
    }
    embi_bytes gdata;
    if (!bt_sub(gv, (uint64_t)data_arr + a, b - a, &gdata)) {
        return false;
    }

    /* One scratch block per call: shared points, tuple points, tuple deltas, the original
     * coordinates for IUP (only when it applies), and touched flags. */
    const bool iup = orig != nullptr && ends != nullptr && ncont > 0;
    const size_t blk_size = 16u * (size_t)total + (iup ? 8u * (size_t)npts : 0u) + total;
    uint8_t *blk = embi_alloc(blk_size);
    if (blk == nullptr) {
        return false;
    }
    uint32_t *shp = (uint32_t *)blk;
    uint32_t *pl = shp + total;
    float *tdx = (float *)(pl + total);
    float *tdy = tdx + total;
    float *ox = tdy + total;
    float *oy = ox + (iup ? npts : 0);
    uint8_t *touched = (uint8_t *)(oy + (iup ? npts : 0));
    if (iup) {
        for (uint32_t i = 0; i < npts; i++) {
            ox[i] = orig[i].x;
            oy[i] = orig[i].y;
        }
    }
    int32_t peak[EMBI_GVAR_MAX_AXES], lo[EMBI_GVAR_MAX_AXES], hi[EMBI_GVAR_MAX_AXES];
    bool ok = true;
    uint32_t nshp = 0;
    bool shp_all = false;

    uint32_t tcount = 0, dataoff = 0;
    gcur sc = {gdata, 0};
    if (ok) {
        ok = bt_u16(gdata, 0, &tcount) && bt_u16(gdata, 2, &dataoff);
    }
    bool shared_pts = ok && (tcount & 0x8000u) != 0;
    tcount &= 0x0FFFu;
    if (ok) {
        sc.pos = dataoff;
        if (shared_pts) {
            ok = gc_points(&sc, shp, total, &nshp, &shp_all);
        }
    }
    uint64_t hpos = 4;
    for (uint32_t t = 0; ok && t < tcount; t++) {
        uint32_t size = 0, idx = 0;
        ok = bt_u16(gdata, hpos, &size) && bt_u16(gdata, hpos + 2, &idx);
        hpos += 4;
        if (!ok) {
            break;
        }
        if (idx & 0x8000u) {
            for (uint32_t k = 0; ok && k < axes; k++) {
                uint32_t v = 0;
                ok = bt_u16(gdata, hpos + 2ull * k, &v);
                peak[k] = (int16_t)v;
            }
            hpos += 2ull * axes;
        } else {
            uint32_t ti = idx & 0x0FFFu;
            if (ti >= shared_n) {
                ok = false;
                break;
            }
            for (uint32_t k = 0; ok && k < axes; k++) {
                uint32_t v = 0;
                ok = bt_u16(gv, shared_off + (uint64_t)ti * axes * 2 + 2ull * k, &v);
                peak[k] = (int16_t)v;
            }
        }
        bool has_range = (idx & 0x4000u) != 0;
        if (has_range) {
            for (uint32_t k = 0; ok && k < axes; k++) {
                uint32_t v0 = 0, v1 = 0;
                ok = bt_u16(gdata, hpos + 2ull * k, &v0) && bt_u16(gdata, hpos + 2ull * (axes + k), &v1);
                lo[k] = (int16_t)v0;
                hi[k] = (int16_t)v1;
            }
            hpos += 4ull * axes;
        }
        if (!ok) {
            break;
        }
        /* The tuple's serialized data follows the shared points, in tuple order. */
        uint64_t tstart = sc.pos;
        if ((uint64_t)tstart + size > gdata.n) {
            ok = false;
            break;
        }
        embi_bytes tb;
        ok = bt_sub(gdata, tstart, size, &tb);
        gcur tc = {tb, 0};
        sc.pos = tstart + size;
        int16_t cbuf[EMBI_GVAR_MAX_AXES];
        for (uint32_t k = 0; k < axes; k++) {
            cbuf[k] = coords[k];
        }
        double scal = ok ? tuple_scalar(peak, has_range ? lo : nullptr, has_range ? hi : nullptr,
                                        cbuf, axes)
                         : 0.0;
        if (!ok || scal == 0.0) {
            continue;
        }
        uint32_t npl = 0;
        bool all = false;
        if (idx & 0x2000u) {
            ok = gc_points(&tc, pl, total, &npl, &all);
        } else if (shared_pts) {
            all = shp_all;
            npl = nshp;
            for (uint32_t k = 0; k < nshp; k++) {
                pl[k] = shp[k];
            }
        } else {
            all = true;
        }
        if (!ok) {
            break;
        }
        uint32_t nitems = all ? total : npl;
        for (uint32_t k = 0; k < total; k++) {
            tdx[k] = 0.0f;
            tdy[k] = 0.0f;
            touched[k] = 0;
        }
        for (uint32_t k = 0; k < nitems; k++) {
            if (!all) {
                if (pl[k] >= total) {
                    ok = false;
                    break;
                }
                touched[pl[k]] = 1;
            } else {
                touched[k] = 1;
            }
        }
        if (!ok) {
            break;
        }
        ok = gc_deltas(&tc, nitems, tdx, all ? nullptr : pl, all);
        if (ok) {
            ok = gc_deltas(&tc, nitems, tdy, all ? nullptr : pl, all);
        }
        if (!ok) {
            break;
        }
        /* IUP fills untouched contour points from their touched neighbours. */
        if (!all && iup) {
            uint32_t s0 = 0;
            for (uint32_t c = 0; c < ncont; c++) {
                uint32_t e = ends[c];
                if (e >= npts || e < s0) {
                    break;
                }
                iup_contour(s0, e, ox, tdx, touched);
                iup_contour(s0, e, oy, tdy, touched);
                s0 = e + 1;
            }
        }
        for (uint32_t k = 0; k < total; k++) {
            dx[k] += (float)scal * tdx[k];
            dy[k] += (float)scal * tdy[k];
        }
    }
    embi_free(blk, blk_size);
    return ok;
}

/* ------------------------------------------------------------------ avar, normalize */

/* Applies the segment map of axis a to a normalized value. Version 2 keeps the version 1 segment
 * maps in the same place; its extra tables (axis id map, delta VarStore) are not applied. */
static float avar_map(embi_bytes avar, uint32_t axes, uint32_t a, float v)
{
    uint32_t major = 0, nax = 0;
    if (avar.n == 0 || !bt_u16(avar, 0, &major) || (major != 1 && major != 2) ||
        !bt_u16(avar, 6, &nax) || a >= nax || a >= axes) {
        return v;
    }
    uint64_t pos = 8;
    for (uint32_t k = 0; k < a; k++) {
        uint32_t np = 0;
        if (!bt_u16(avar, pos, &np)) {
            return v;
        }
        pos += 2 + 4ull * np;
    }
    uint32_t np = 0;
    if (!bt_u16(avar, pos, &np) || np < 2) {
        return v;
    }
    for (uint32_t k = 0; k + 1 < np; k++) {
        uint32_t f0 = 0, t0 = 0, f1 = 0, t1 = 0;
        uint64_t rec = pos + 2 + 4ull * k;
        if (!bt_u16(avar, rec, &f0) || !bt_u16(avar, rec + 2, &t0) ||
            !bt_u16(avar, rec + 4, &f1) || !bt_u16(avar, rec + 6, &t1)) {
            return v;
        }
        float fa = f2dot14((int16_t)f0), ta = f2dot14((int16_t)t0);
        float fb = f2dot14((int16_t)f1), tb = f2dot14((int16_t)t1);
        if (v >= fa && v <= fb) {
            if (fb == fa) {
                return ta;
            }
            return ta + (v - fa) * (tb - ta) / (fb - fa);
        }
    }
    return v;
}

bool embi_font_normalize(const embi_font *f, const float *user, int16_t *coords)
{
    for (uint32_t a = 0; a < f->axis_count; a++) {
        const embi_font_axis *ax = &f->axes[a];
        float v = user[a];
        if (v != v) { /* NaN: the default position */
            v = ax->def;
        }
        if (v < ax->min) {
            v = ax->min;
        }
        if (v > ax->max) {
            v = ax->max;
        }
        float n = 0.0f;
        if (v == ax->def) {
            n = 0.0f;
        } else if (v < ax->def) {
            n = (v - ax->def) / (ax->def - ax->min);
        } else {
            n = (v - ax->def) / (ax->max - ax->def);
        }
        n = avar_map(f->avar, f->axis_count, a, n);
        if (n < -1.0f) {
            n = -1.0f;
        }
        if (n > 1.0f) {
            n = 1.0f;
        }
        long r = (long)(n * 16384.0f + (n >= 0.0f ? 0.5f : -0.5f));
        if (r < -16384) {
            r = -16384;
        }
        if (r > 16384) {
            r = 16384;
        }
        coords[a] = (int16_t)r;
    }
    return true;
}

/* ------------------------------------------------------------------ MVAR */

bool embi_font_mvar_delta(const embi_font *f, uint32_t tag, const int16_t *coords, float *delta)
{
    *delta = 0.0f;
    embi_bytes mv = f->mvar;
    uint32_t rsize = 0, rcount = 0;
    if (mv.n == 0 || !bt_u16(mv, 6, &rsize) || !bt_u16(mv, 8, &rcount) || rsize < 8) {
        return false;
    }
    for (uint32_t i = 0; i < rcount; i++) {
        uint32_t t = 0, outer = 0, inner = 0;
        uint64_t rec = 12ull + (uint64_t)i * rsize;
        if (!bt_u32(mv, rec, &t) || !bt_u16(mv, rec + 4, &outer) || !bt_u16(mv, rec + 6, &inner)) {
            return false;
        }
        if (t == tag) {
            *delta = (float)embi_ivs_delta(f->mvar_ivs, outer, inner, coords, f->axis_count);
            return true;
        }
    }
    return false;
}
