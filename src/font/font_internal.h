#pragma once
/*
 * Internal declarations shared by the font parser's translation units. Not part of the module's
 * public surface (font.h is).
 */
#include "font/font.h"

/* ------------------------------------------------------------------ bounds-checked reads */

/* Each reader fails (returns false, leaves *v alone) unless the whole field lies inside b. */
static inline bool bt_u8(embi_bytes b, uint64_t o, uint32_t *v)
{
    if (o >= b.n) {
        return false;
    }
    *v = b.p[o];
    return true;
}

static inline bool bt_u16(embi_bytes b, uint64_t o, uint32_t *v)
{
    if (o > b.n || b.n - o < 2) {
        return false;
    }
    *v = ((uint32_t)b.p[o] << 8) | b.p[o + 1];
    return true;
}

static inline bool bt_i16(embi_bytes b, uint64_t o, int32_t *v)
{
    uint32_t u = 0;
    if (!bt_u16(b, o, &u)) {
        return false;
    }
    *v = (int16_t)(uint16_t)u;
    return true;
}

static inline bool bt_u24(embi_bytes b, uint64_t o, uint32_t *v)
{
    if (o > b.n || b.n - o < 3) {
        return false;
    }
    *v = ((uint32_t)b.p[o] << 16) | ((uint32_t)b.p[o + 1] << 8) | b.p[o + 2];
    return true;
}

static inline bool bt_u32(embi_bytes b, uint64_t o, uint32_t *v)
{
    if (o > b.n || b.n - o < 4) {
        return false;
    }
    *v = ((uint32_t)b.p[o] << 24) | ((uint32_t)b.p[o + 1] << 16) | ((uint32_t)b.p[o + 2] << 8) |
         b.p[o + 3];
    return true;
}

static inline bool bt_i32(embi_bytes b, uint64_t o, int32_t *v)
{
    uint32_t u = 0;
    if (!bt_u32(b, o, &u)) {
        return false;
    }
    *v = (int32_t)u;
    return true;
}

/* Sub-range [o, o+len). Fails when it does not fit. */
static inline bool bt_sub(embi_bytes b, uint64_t o, uint64_t len, embi_bytes *out)
{
    if (o > b.n || len > b.n - o) {
        return false;
    }
    out->p = b.p + o;
    out->n = (size_t)len;
    return true;
}

/* Remainder of b from offset o. */
static inline bool bt_tail(embi_bytes b, uint64_t o, embi_bytes *out)
{
    if (o > b.n) {
        return false;
    }
    return bt_sub(b, o, b.n - o, out);
}

/* F2DOT14 to float. */
static inline float f2dot14(int32_t v)
{
    return (float)v / 16384.0f;
}

/* ------------------------------------------------------------------ outline builder */

/* Appends to an embi_outline. Lines become quads with the midpoint as control. */
typedef struct embi_olb {
    embi_outline *o;
    embi_pt cur;   /* current point */
    embi_pt start; /* first point of the open contour */
    bool open;
} embi_olb;

void embi_olb_init(embi_olb *b, embi_outline *o);
bool embi_olb_move(embi_olb *b, embi_pt p);
bool embi_olb_quad(embi_olb *b, embi_pt c, embi_pt p);
bool embi_olb_line(embi_olb *b, embi_pt p);
/* Adds the cubic cur-p1-p2-p3 as quads whose error is within tol (font units). */
bool embi_olb_cubic(embi_olb *b, embi_pt p1, embi_pt p2, embi_pt p3, float tol);
/* Closes the open contour with a line back to its start. */
bool embi_olb_close(embi_olb *b);
/* Closes and recomputes the outline bbox from the quads. */
bool embi_olb_finish(embi_olb *b);

/* Appends src's quads and contours to dst, mapping every point by (a b c d e f). */
bool embi_outline_append_xf(embi_outline *dst, const embi_outline *src, const float xf[6]);

/* ------------------------------------------------------------------ CFF */

/* An INDEX, positioned inside the CFF/CFF2 table. */
typedef struct embi_cffidx {
    embi_bytes table;
    uint64_t offs; /* offset of the offsets array, relative to table */
    uint64_t data; /* offset of the first object byte minus 1 (offsets are 1-based) */
    uint32_t count;
    uint32_t offsize;
} embi_cffidx;

typedef struct embi_cff_fd {
    embi_cffidx subrs; /* local subroutines, may be empty */
    uint32_t vsindex;  /* CFF2 default ItemVariationStore index */
} embi_cff_fd;

struct embi_cff {
    bool cff2;
    embi_bytes table;
    embi_cffidx charstrings;
    embi_cffidx gsubrs;
    embi_cff_fd *fds;
    uint32_t nfds;
    embi_bytes fdselect; /* raw FDSelect; empty means every glyph uses fd 0 */
    uint32_t fdselect_fmt;
    embi_bytes charset; /* CFF1 charset; empty when absent */
    uint32_t charset_off;
    double fm[6];       /* FontMatrix */
    embi_bytes vstore; /* CFF2 ItemVariationStore */
};

bool embi_cff_open(embi_font *f, embi_bytes table);
void embi_cff_close(embi_cff *c);
/* Outline of gid for the instance coords (nullptr for default), appended to out (font units). */
bool embi_cff_outline(const embi_font *f, uint32_t gid, const int16_t *coords,
                      embi_outline *out);
bool embi_cffidx_get(const embi_cffidx *x, uint32_t i, embi_bytes *out);

/* ------------------------------------------------------------------ glyf */

/* Appends the outline of glyph gid (composites flattened, depth-limited) to out. */
bool embi_glyf_outline(const embi_font *f, uint32_t gid, const int16_t *coords, uint32_t depth,
                       embi_outline *out);

/* ------------------------------------------------------------------ variations */

/* Sum of the item's deltas times the region scalars, for ItemVariationStore row (outer, inner).
 * Returns 0 for an out-of-range index or malformed store. */
double embi_ivs_delta(embi_bytes ivs, uint32_t outer, uint32_t inner, const int16_t *coords,
                      uint32_t ncoords);
/* Region scalars of VarData `outer`, one per regionIndex in order. Returns false when the store
 * is malformed or the VarData has more than cap regions. */
bool embi_ivs_scalars(embi_bytes ivs, uint32_t outer, const int16_t *coords, uint32_t ncoords,
                      double *scal, uint32_t cap, uint32_t *count);
/* DeltaSetIndexMap lookup: packed index (outer << 16 | inner) for map index i. An empty map is
 * the identity: the packed index is i itself, with outer 0. */
bool embi_var_map(embi_bytes map, uint32_t i, uint32_t *packed);

/* Glyph point deltas for gvar. orig holds the default coordinates of npts points, and ends holds
 * the ncont contour end points (simple glyphs; ncont 0 for composites, which skip IUP).
 * dx and dy receive npts + 4 values (phantom points last). */
bool embi_gvar_glyph_deltas(const embi_font *f, uint32_t gid, const int16_t *coords,
                            const embi_pt *orig, uint32_t npts, const uint16_t *ends,
                            uint32_t ncont, float *dx, float *dy);
