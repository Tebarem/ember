#pragma once
/*
 * Grid layout internals (W3f, CSS Grid 2). Shared by grid.c (the container, its items, alignment and
 * out-of-flow children), grid_tmpl.c (templates, repeat(), areas and line names), grid_place.c (the
 * placement algorithm, section 8.5) and grid_track.c (the track sizing algorithm, section 12). Nothing
 * here is used outside the layout module.
 *
 * Index spaces. Each axis (0 rows, 1 columns) has an implicit grid of nt tracks. Explicit tracks occupy
 * the indices [pre, pre + nex), and implicit tracks lie on both sides of them. A grid line with 1-based
 * number L sits at index L - 1 + pre, so a placed item covers the track range [start, end) of its
 * indices.
 */
#include "layout_int.h"

/* Track sizing functions after resolution against the container's sizes (CSS Grid 2 section 7.2). */
enum {
    GTK_FIXED = 1, /* a length in px (minpx for the minimum, maxpx for the maximum) */
    GTK_AUTO,      /* auto */
    GTK_MIN,       /* min-content */
    GTK_MAX,       /* max-content */
    GTK_FR,        /* flexible: fr, as the maximum function only (fr is the flex factor) */
    GTK_FIT,       /* fit-content(limit): the growth limit is clamped to maxpx */
};

/* One track of an axis. base and grow are the base size and growth limit of section 12.4; the used
 * size of a track is its base size once the algorithm has finished. */
typedef struct gtrack {
    uint8_t mink, maxk; /* GTK_* of the minimum and the maximum sizing functions */
    bool inf;           /* the growth limit is infinite (not yet set to the base size) */
    bool infg;          /* infinitely growable (section 12.5 step 3.5) */
    bool frz;           /* sizing scratch: the track is frozen in the current distribution */
    bool collapsed;     /* an auto-fit track with no items: it has no size */
    bool pct;           /* a sizing function was a percentage of an indefinite size (treated as auto) */
    float minpx, maxpx; /* FIXED sizes; for FIT, maxpx is the limit */
    float fr;           /* FR flex factor */
    float base, grow;   /* base size and growth limit */
    float plan;         /* scratch: the planned increase of the current distribution */
    float inc;          /* scratch: the item-incurred increase */
} gtrack;

/* An item's contribution to the sizing of one axis: the span [a, b) of tracks it covers and its outer
 * min-content, max-content and minimum contributions (margin box, px). */
typedef struct gsitem {
    uint32_t a, b;
    float mn, mx, mi;
} gsitem;

/* What an axis is sized under (section 12.1): a definite or indefinite size of the container, or an
 * intrinsic size query. */
enum {
    GSZ_NORMAL = 0, /* the container's size is used (avail), or content-sized under min and max */
    GSZ_MINC = 1,   /* sizing under a min-content constraint */
    GSZ_MAXC = 2,   /* sizing under a max-content constraint */
};

typedef struct gsz {
    float avail;        /* the definite inner size of the container, NaN when indefinite */
    float minsz, maxsz; /* the inner min and max sizes (0 and INFINITY when none) */
    int mode;           /* GSZ_* */
    bool stretch;       /* auto tracks stretch (content distribution normal or stretch) */
} gsz;

/* ------------------------------------------------------------------ track sizing (grid_track.c) */

/* Sets base and growth limit from the sizing functions (section 12.4). */
void embi_grid_tracks_init(gtrack *t, uint32_t n);
/* Resolves the intrinsic track sizes (section 12.5) from the item contributions. Leaves every growth
 * limit finite (infinite ones become their base size, and fit-content limits apply). */
void embi_grid_resolve_intrinsic(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap, int mode);
/* Maximize tracks (section 12.6): grows base sizes toward their growth limits with free_space,
 * equally; an infinite free space takes every track to its growth limit. */
void embi_grid_maximize(gtrack *t, uint32_t n, float free_space);
/* Expand flexible tracks (section 12.7) against the leftover space: the space left for the flexible
 * tracks after the base sizes of the others and the gaps. */
void embi_grid_expand_flex(gtrack *t, uint32_t n, float leftover);
/* The used fr size of the flexible tracks for a leftover space (section 12.7.1, the loop that
 * freezes inflexible tracks). Frozen tracks keep their base sizes. */
float embi_grid_find_fr(gtrack *t, uint32_t n, float leftover);
/* The fr size for an indefinite free space (section 12.7.1): the largest of the base sizes of the
 * flexible tracks over their flex factors, and the item-based sizes for the items that cross them. */
float embi_grid_fr_indef(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap);
/* Stretch auto tracks (section 12.8): free space is shared equally by the tracks with an auto
 * maximum. */
void embi_grid_stretch_auto(gtrack *t, uint32_t n, float free_space);
/* The size of a fr for a leftover space and the sum of the flex factors (section 12.7.1: a sum below
 * one counts as one). */
float embi_grid_fr_size(float free_space, float flex_sum);
/* The size of the tracks [a, b) with the gaps between them. */
float embi_grid_span_size(const gtrack *t, uint32_t a, uint32_t b, float gap);
/* The gutters of the tracks [a, b): one gap between each pair of neighbouring tracks that are not
 * collapsed. */
float embi_grid_gutters(const gtrack *t, uint32_t a, uint32_t b, float gap);
/* The whole algorithm of section 12.3 for one axis: initialize, resolve intrinsic sizes, maximize,
 * expand flexible tracks and stretch auto tracks, with the min and max sizes of the container applied
 * when its size is indefinite. Returns the resulting size of the tracks with their gutters. */
float embi_grid_size_tracks(gtrack *t, uint32_t n, const gsitem *it, uint32_t ni, float gap, const gsz *z);

/* ------------------------------------------------------------------ placement (grid_place.c) */

/* A grid placement value of one end of an item on one axis (grid-row-start and friends). */
typedef struct gpl {
    bool set;      /* not auto */
    bool span;     /* span num */
    int32_t num;   /* the line number (negative counts from the end), or the span; 0 when absent */
    embi_atom name; /* a named line or area, 0 when absent */
} gpl;

/* A line name of an axis: the explicit line index (0 to n) it is attached to. */
typedef struct gname {
    uint32_t line;
    embi_atom name;
} gname;

typedef struct gname_vec {
    gname *data;
    uint32_t len, cap;
} gname_vec;

/* A grid area from grid-template-areas: explicit track indices, inclusive. */
typedef struct garea {
    embi_atom name;
    uint32_t r0, r1, c0, c1;
} garea;

typedef struct gplace_ctx {
    uint32_t nex[2];          /* explicit track counts per axis */
    gname_vec names[2];       /* line names per axis (the implicit X-start and X-end of areas included) */
    const garea *areas;
    uint32_t narea;
    bool row_flow;            /* grid-auto-flow: row (else column) */
    bool dense;               /* grid-auto-flow: dense */
    bool clamp[2];            /* a subgridded axis: no implicit tracks, lines are clamped to the explicit grid */
} gplace_ctx;

/* An item to place. pl[axis][0] is the start and pl[axis][1] the end placement. st and en are the
 * index range it covers, set by embi_grid_place. */
struct gst;
typedef struct gitem {
    embi_box *box;
    int32_t order;
    gpl pl[2][2];
    uint32_t st[2], en[2];
    /* scratch of the placement */
    uint8_t kind[2];
    int32_t raw_s[2], raw_e[2];
    uint32_t span[2];
    /* sizing and layout state (grid.c) */
    struct gst *sub;       /* the grid state of a subgrid item (sizing phase) */
    bool subax[2];         /* the item is a subgrid in this axis */
    embi_frag *fr;         /* the laid out fragment */
    bool mvalid;           /* fr is the layout lay_item made without a row height, at the inline area size maw (W3p) */
    bool mselfc;           /* that layout was self-collapsing */
    float maw;
    float m[2][2];         /* margins as numbers, [axis][start/end] (auto is 0), logical */
    bool am[2][2];         /* the margin is auto */
    float xm[2][2];        /* extra margin from a parent subgrid: gap difference and edge mbp */
    int al[2];             /* alignment kind per axis (AK_*) */
    float asc[2];          /* the baseline distance from the margin edge (start) or to it (last) */
    float ba[2];           /* the baseline shim added to the contribution */
    float cell[2];         /* the grid area size along each axis */
    float size[2];         /* border-box size of the item along each axis, after layout */
    bool ovf_safe[2];      /* safe overflow alignment */
    bool rowk;             /* rowh is the sized row area of the item (set after the first row pass) */
    float rowh;            /* the height of the item's row area, with the gaps in it */
    bool pctdep;           /* its column contribution looked at its percentage height basis (col_contrib, mark_rows) */
} gitem;

/* Reads the four placement values of an item (grid-row-start, grid-row-end, grid-column-start,
 * grid-column-end) into pl. */
void embi_grid_read_placement(embi_lctx *c, embi_box *k, gpl pl[2][2]);
/* Places the items (section 8.5). Sets each item's st and en, and pre (the implicit tracks before the
 * explicit grid) and total (the track count) per axis. False only on an allocation failure. */
bool embi_grid_place(const gplace_ctx *p, gitem *items, uint32_t ni, uint32_t pre[2],
                     uint32_t total[2]);
/* Resolves the placement of an out-of-flow child on axis ax (pl[0] start, pl[1] end) against the placed
 * grid (section 9.2). s and e are track-line indices in [0, total], or UINT32_MAX for a side that is
 * auto, or that refers to a line outside the implicit grid (such a line is treated as auto). Returns
 * false when both sides are auto. */
bool embi_grid_place_abs(const gplace_ctx *p, uint32_t ax, const gpl pl[2], uint32_t pre,
                         uint32_t total, uint32_t *s, uint32_t *e);
/* Resolves a line number to a 1-based line, with a flag for whether it is definite (a line number of
 * zero or below is a legitimate implicit line before the grid; only auto and span are not definite). */
bool embi_grid_line_of(const gplace_ctx *p, uint32_t ax, bool start, const gpl *pl, int32_t *line);

/* ------------------------------------------------------------------ templates (grid_tmpl.c) */

#define GRID_MAX_TRACKS 10000u

typedef struct gdef {
    const embi_track_size *ts;
    bool autofit;
} gdef;

typedef struct gdef_vec {
    gdef *data;
    uint32_t len, cap;
} gdef_vec;

/* The explicit template of an axis: its track definitions and its line names. */
typedef struct gtmpl {
    gdef_vec defs;
    gname_vec names;
    bool err; /* an allocation failed while building: the layout fails */
} gtmpl;

/* Builds the explicit template of an axis from its track list. basis is the content size the
 * repetitions of auto-fill and auto-fit fill (NaN when indefinite), minb and maxb the min and max
 * content sizes used when it is (0 and INFINITY for none). */
void embi_grid_build_tmpl(embi_lctx *c, embi_box *b, const embi_value *v, float basis, float minb,
                          float maxb, float gap, gtmpl *t);
/* Builds the line names of a subgrid axis of n tracks from its `subgrid [names]` list, then appends the
 * names of the parent's lines [off, off + n] (rebased to 0), in names. */
void embi_grid_build_subgrid_names(const embi_value *v, uint32_t n, gtmpl *t);
void embi_grid_free_tmpl(gtmpl *t);
void embi_grid_push_name(gname_vec *v, uint32_t line, embi_atom name, bool *err);
/* The sizing function of a track definition resolved against basis (NaN: indefinite), into t. */
void embi_grid_track_from_def(embi_lctx *c, embi_box *b, const embi_track_size *ts, float basis, gtrack *t);
/* The areas of grid-template-areas, and the number of rows and columns they cover. */
bool embi_grid_parse_areas(const embi_value *v, garea **out, uint32_t *nout, uint32_t *ncap,
                           uint32_t *nrows, uint32_t *ncols);
/* The size of the implicit track at index e (relative to the first explicit track) from the
 * grid-auto-rows or -columns list. */
const embi_track_size *embi_grid_auto_size(const embi_grid_tracks *al, int64_t e, uint32_t ntmpl);
float embi_grid_track_len(embi_lctx *c, embi_box *b, const embi_value *v, float basis);
