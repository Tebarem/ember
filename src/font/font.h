#pragma once
/*
 * OpenType font parser (task R2a). Internal to Ember: cross-module symbols use the embi_ prefix.
 *
 * Lifetime: an embi_font borrows the file bytes. The caller keeps them alive until
 * embi_font_close. Every offset, length and count is bounds-checked; malformed data yields false
 * (or a missing optional table), never undefined behaviour.
 *
 * Units: outline and metric values are font units (head.unitsPerEm). Variation coordinates
 * ("coords") are normalized F2DOT14 values, one per fvar axis in fvar order; nullptr means the
 * default instance. User-space axis values ("user") are floats in the axis' own range.
 */
#include "base/base.h"

#include <stddef.h>
#include <stdint.h>

/* Four-character tag packed big-endian, as stored in the font. */
#define EMBI_TAG(a, b, c, d)                                                                   \
    ((((uint32_t)(uint8_t)(a)) << 24) | (((uint32_t)(uint8_t)(b)) << 16) |                     \
     (((uint32_t)(uint8_t)(c)) << 8) | ((uint32_t)(uint8_t)(d)))

/* Borrowed byte range. */
typedef struct embi_bytes {
    const uint8_t *p;
    size_t n;
} embi_bytes;

/* ------------------------------------------------------------------ outlines */

typedef struct embi_pt {
    float x, y;
} embi_pt;

/* Quadratic Bezier from p0 through control p1 to p2. Lines are quads with p1 at the midpoint. */
typedef struct embi_quad {
    embi_pt p0, p1, p2;
} embi_quad;

/* Quads of a glyph in order. Each contour is closed: the last quad of a contour ends at the
 * contour's first point. contours[i] is the index of the first quad of contour i. The bbox is
 * the tight bound of the curves (curve extrema included). */
typedef struct embi_outline {
    EMBI_VEC(embi_quad) quads;
    EMBI_VEC(uint32_t) contours;
    float xmin, ymin, xmax, ymax;
} embi_outline;

/* Frees the storage of an outline. Zeroed or already-freed outlines are fine to free again. */
void embi_outline_free(embi_outline *o);

/* ------------------------------------------------------------------ font metrics and axes */

typedef struct embi_font_axis {
    uint32_t tag;
    float min, def, max;
    uint16_t name_id;
} embi_font_axis;

typedef struct embi_font_metrics {
    uint32_t units_per_em;
    uint32_t num_glyphs;
    int16_t x_min, y_min, x_max, y_max; /* head bbox */
    int16_t hhea_ascender, hhea_descender, hhea_line_gap;
    int16_t typo_ascender, typo_descender, typo_line_gap; /* OS/2 */
    uint16_t win_ascent, win_descent;                     /* OS/2 */
    int16_t x_height, cap_height;                         /* 0 when OS/2 version < 2 */
    int16_t strikeout_size, strikeout_position;
    int16_t underline_position, underline_thickness; /* post */
    float italic_angle;                              /* degrees, post */
    uint16_t weight_class, width_class;              /* OS/2 usWeightClass, usWidthClass */
    bool use_typo_metrics, italic, bold, fixed_pitch;
    bool is_cff, is_cff2, is_variable, has_colr;
    /* UTF-8 names, valid until embi_font_close. Empty when the name table lacks them. */
    embi_str family, subfamily, full_name;
    /* Typographic family and subfamily (name IDs 16 and 17), empty when absent. CSS family
     * matching wants typo_family when set ('Noto Sans' for a face whose ID 1 is 'Noto Sans
     * Light'). family/subfamily fall back to 16/17 only when IDs 1/2 are missing. */
    embi_str typo_family, typo_subfamily;
} embi_font_metrics;

/* An opened face. Zero-initialized is "closed". Copyable by value, but only the open instance
 * owns the parsed data, so pass it by pointer and call embi_font_close once. */
typedef struct embi_cff embi_cff;

typedef struct embi_font {
    embi_font_metrics metrics;
    embi_bytes file;     /* the face's whole container (a TTC shares it across faces) */
    uint32_t num_hmetrics;
    bool loca_long;
    embi_bytes glyf, loca, hmtx;
    embi_bytes cmap4, cmap12, cmap14; /* chosen subtables; empty when absent */
    bool cmap_symbol;                 /* cmap4 is a (3,0) symbol subtable */
    embi_bytes colr, cpal;
    embi_bytes cblc, cbdt, sbix;       /* bitmap colour glyphs (W4i); empty when absent */
    embi_bytes avar, gvar;
    embi_bytes hvar_ivs, hvar_adv_map; /* HVAR; advance map empty means identity */
    embi_bytes mvar_ivs;               /* MVAR value records are read from the table */
    embi_bytes mvar;
    embi_bytes cff;                    /* CFF or CFF2 table */
    embi_cff *cff_data;                /* parsed CFF/CFF2 structure, owned */
    embi_font_axis *axes;              /* owned */
    uint32_t axis_count;
    float *instances;                  /* instance_count * axis_count user coords, owned */
    uint32_t instance_count;
    uint8_t *name_buf;                 /* owned storage behind the metrics names */
    size_t name_len;
} embi_font;

/* Number of faces in the data: 1 for an sfnt, numFonts for a TTC, 0 when unrecognized. */
uint32_t embi_font_count(const uint8_t *data, size_t len);

/* Opens face `index` of data. On success out is filled and owns its parsed state. On failure
 * out is left zeroed and false is returned. Fails for WOFF/WOFF2 containers (decoded in R2d). */
bool embi_font_open(const uint8_t *data, size_t len, uint32_t index, embi_font *out);
void embi_font_close(embi_font *f);

bool embi_font_get_metrics(const embi_font *f, embi_font_metrics *out);

/* Glyph for a code point (cmap format 12 preferred, then format 4). 0 when unmapped. A font whose
 * only cmap is (3,0) symbol format 4 is read through the 0xF000 + cp convention for cp <= 0xFF
 * (raw cp is tried too). */
uint32_t embi_font_glyph_index(const embi_font *f, uint32_t cp);
/* Glyph for a code point with a variation selector (cmap format 14). 0 when the font has no
 * specific mapping; callers then fall back to embi_font_glyph_index. */
uint32_t embi_font_glyph_variant(const embi_font *f, uint32_t cp, uint32_t selector);
bool embi_font_has_codepoint(const embi_font *f, uint32_t cp);

/* Advance width in font units for the given instance (HVAR applied when present, gvar phantom
 * points otherwise for glyf fonts). */
bool embi_font_glyph_advance(const embi_font *f, uint32_t gid, const int16_t *coords,
                             float *advance);

/* Appends nothing: resets out (its storage is reused, so zero-init it once) and fills it with the
 * outline of glyph gid for the given instance. Composite glyphs are flattened. */
bool embi_font_glyph_outline(const embi_font *f, uint32_t gid, const int16_t *coords,
                             embi_outline *out);

/* ------------------------------------------------------------------ variations */

/* Index of the axis with this tag, or -1. */
int32_t embi_font_axis_index(const embi_font *f, uint32_t tag);
/* Converts user values (one per axis, fvar order) to normalized F2DOT14 coords, applying avar. */
bool embi_font_normalize(const embi_font *f, const float *user, int16_t *coords);
uint32_t embi_font_instance_count(const embi_font *f);
/* User-space coordinates of named instance i (fvar order). */
bool embi_font_instance_coords(const embi_font *f, uint32_t i, float *user_out);
/* Delta for an MVAR metric tag (for example EMBI_TAG('x','h','g','t')) at coords. */
bool embi_font_mvar_delta(const embi_font *f, uint32_t tag, const int16_t *coords, float *delta);

/* ------------------------------------------------------------------ color (CPAL, COLR) */

uint32_t embi_font_palette_count(const embi_font *f);
/* Color entry of a palette as straight-alpha RGBA bytes. */
bool embi_font_cpal_color(const embi_font *f, uint32_t palette, uint32_t entry, uint8_t rgba[4]);

typedef enum embi_colr_op_kind {
    EMBI_COLR_TRANSFORM_PUSH, /* xf applies to later ops until the matching POP */
    EMBI_COLR_TRANSFORM_POP,
    EMBI_COLR_CLIP_PUSH,      /* restrict later paints to the coverage of glyph gid */
    EMBI_COLR_CLIP_POP,
    EMBI_COLR_GROUP_PUSH,     /* ops until GROUP_POP form a layer composited with `composite` */
    EMBI_COLR_GROUP_POP,
    EMBI_COLR_PAINT           /* fill the current clip with `paint` */
} embi_colr_op_kind;

typedef enum embi_colr_paint_kind {
    EMBI_PAINT_SOLID,
    EMBI_PAINT_LINEAR,
    EMBI_PAINT_RADIAL,
    EMBI_PAINT_SWEEP
} embi_colr_paint_kind;

typedef struct embi_colr_stop {
    float offset;   /* normally 0..1; not clamped (variation deltas may leave the range; the
                       extend mode covers it). Stops are stable-sorted by offset when a line
                       has at most 256 of them; longer lines keep font order. */
    float rgba[4];  /* straight alpha, 0..1, palette colour times the stop alpha */
    bool foreground; /* palette index 0xFFFF: rgba holds only the alpha; use the text colour */
} embi_colr_stop;

typedef struct embi_colr_paint {
    embi_colr_paint_kind kind;
    float rgba[4];          /* SOLID: colour (alpha already multiplied in) */
    bool foreground;        /* SOLID from COLRv0 palette index 0xFFFF: use the text colour */
    float p[6];             /* LINEAR: x0 y0 x1 y1 x2 y2. RADIAL: x0 y0 r0 x1 y1 r1.
                               SWEEP: cx cy start_deg end_deg. Font-unit space at this op. */
    uint8_t extend;         /* 0 pad, 1 repeat, 2 reflect */
    const embi_colr_stop *stops; /* valid only during the callback */
    uint32_t num_stops;
} embi_colr_paint;

typedef struct embi_colr_op {
    embi_colr_op_kind kind;
    float xf[6];             /* TRANSFORM_PUSH: a b c d e f, x' = a x + c y + e, y' = b x + d y + f */
    uint32_t gid;            /* CLIP_PUSH */
    uint8_t composite;       /* GROUP_PUSH: COLR CompositeMode (0 clear .. 28 luminosity) */
    embi_colr_paint paint;   /* PAINT */
} embi_colr_op;

typedef struct embi_colr_visitor {
    void *ctx;
    /* Returns false to stop the traversal early; embi_font_colr_paint then returns false. */
    bool (*op)(void *ctx, const embi_colr_op *op);
} embi_colr_visitor;

/* Walks the paint graph of glyph gid (COLRv1, or COLRv0 layers when the glyph has only a v0
 * record). Returns false when the glyph has no colour record or the data is malformed. Ops are
 * emitted in paint order; transforms, clips and groups are balanced. Composite modes are
 * recorded but drawn as src-over until R3. */
bool embi_font_colr_paint(const embi_font *f, uint32_t gid, const int16_t *coords,
                          uint32_t palette, const embi_colr_visitor *v);

/* ------------------------------------------------------------------ bitmap colour glyphs (font_bitmap.c, W4i) */

/* One bitmap glyph of a strike: a PNG held in the font file. Sizes are in the strike's own pixels
 * (ppem); the caller scales them to the size it draws at. */
typedef struct embi_bitmap_glyph {
    const uint8_t *png;   /* PNG bytes, inside the font's table data */
    uint32_t len;
    uint32_t ppem;        /* the strike's pixels per em */
    uint32_t width, height; /* image size in strike pixels; 0 when the table does not say (decode it) */
    float left;           /* from the glyph origin to the image's left edge, px (right is positive) */
    float top;            /* from the baseline to the image's top edge, px (up is positive) */
    bool top_from_bottom; /* sbix: top is originY + height, known once the PNG is decoded */
    float origin_y;       /* sbix: the bottom edge's height above the baseline (top = origin_y + height) */
} embi_bitmap_glyph;

/* True when the face has CBDT/CBLC or sbix bitmap strikes. A face with COLR is drawn from COLR first. */
bool embi_font_has_bitmaps(const embi_font *f);
/* The strike of glyph gid nearest target_ppem (the smallest strike at least that large, else the largest
 * one). Formats: CBDT with index formats 1..5 and image formats 17, 18 and 19; sbix with 'png ' data and
 * 'dupe' references. Returns false when no strike has the glyph. */
bool embi_font_bitmap_glyph(const embi_font *f, uint32_t gid, float target_ppem,
                            embi_bitmap_glyph *out);
