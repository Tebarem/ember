#pragma once
/*
 * Internals of the SVG module (R4c): small value parsers (parse.c), the walker that paints, hit tests and measures the DOM
 * subtree, with clips, masks, markers and use (walk.c), paint servers (pserver.c), text (text.c), geometry queries (geom.c,
 * api.c) and SVG images (image.c). Nothing here is used outside src/svg.
 */
#include "svg/geom.h"
#include "svg/svg.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ matrices (CSS order a b c d e f: x' = a x + c y + e) */

static inline void m_identity(float m[6])
{
    m[0] = 1;
    m[1] = 0;
    m[2] = 0;
    m[3] = 1;
    m[4] = 0;
    m[5] = 0;
}

/* out = a * b: the point goes through b first. out may alias a or b. */
static inline void m_mul(const float a[6], const float b[6], float out[6])
{
    float r[6];
    r[0] = a[0] * b[0] + a[2] * b[1];
    r[1] = a[1] * b[0] + a[3] * b[1];
    r[2] = a[0] * b[2] + a[2] * b[3];
    r[3] = a[1] * b[2] + a[3] * b[3];
    r[4] = a[0] * b[4] + a[2] * b[5] + a[4];
    r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    memcpy(out, r, sizeof r);
}

static inline void m_translate(float m[6], float x, float y)
{
    m[0] = 1;
    m[1] = 0;
    m[2] = 0;
    m[3] = 1;
    m[4] = x;
    m[5] = y;
}

static inline float m_det(const float m[6])
{
    return m[0] * m[3] - m[1] * m[2];
}

/* The inverse; false when the matrix is singular. */
static inline bool m_invert(const float m[6], float out[6])
{
    double det = (double)m[0] * m[3] - (double)m[1] * m[2];
    if (!(fabs(det) > 1e-30) || !isfinite(det)) {
        return false;
    }
    double id = 1.0 / det;
    float r[6];
    r[0] = (float)(m[3] * id);
    r[1] = (float)(-m[1] * id);
    r[2] = (float)(-m[2] * id);
    r[3] = (float)(m[0] * id);
    r[4] = (float)(((double)m[2] * m[5] - (double)m[3] * m[4]) * id);
    r[5] = (float)(((double)m[1] * m[4] - (double)m[0] * m[5]) * id);
    memcpy(out, r, sizeof r);
    return true;
}

static inline void m_apply(const float m[6], float x, float y, float *ox, float *oy)
{
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

/* The uniform scale of m (the square root of the absolute determinant). */
static inline float m_scale(const float m[6])
{
    return sqrtf(fabsf(m_det(m)));
}

/* The axis-aligned box (x, y, w, h) of the rect mapped by m. */
static inline void m_map_rect(const float m[6], const float r[4], float out[4])
{
    float xs[4], ys[4];
    m_apply(m, r[0], r[1], &xs[0], &ys[0]);
    m_apply(m, r[0] + r[2], r[1], &xs[1], &ys[1]);
    m_apply(m, r[0], r[1] + r[3], &xs[2], &ys[2]);
    m_apply(m, r[0] + r[2], r[1] + r[3], &xs[3], &ys[3]);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
        x0 = fminf(x0, xs[i]);
        x1 = fmaxf(x1, xs[i]);
        y0 = fminf(y0, ys[i]);
        y1 = fmaxf(y1, ys[i]);
    }
    out[0] = x0;
    out[1] = y0;
    out[2] = x1 - x0;
    out[3] = y1 - y0;
}

/* ------------------------------------------------------------------ parse.c */

/* Skips the white space of the SVG grammars (space, tab, CR, LF, FF). */
const char *svg_skip_ws(const char *p, const char *end);
/* Skips white space and at most one comma. */
const char *svg_skip_wsp_comma(const char *p, const char *end);
/* An SVG number at *p (sign, digits, fraction, exponent). Advances *p past it. False (and *p unchanged) when none. */
bool svg_number(const char **p, const char *end, double *out);
/* A list of numbers separated by white space and commas (points, viewBox, dash arrays of plain numbers). Returns how many
 * were parsed (at most cap); *ok is false when something that is not a number is left over. */
uint32_t svg_number_list(const char *s, uint32_t len, double *out, uint32_t cap, bool *ok);

typedef struct svg_len {
    float v;
    uint8_t unit; /* SVGL_* */
} svg_len;
enum { SVGL_NONE = 0, SVGL_PX, SVGL_EM, SVGL_EX, SVGL_PCT, SVGL_IN, SVGL_CM, SVGL_MM, SVGL_PT, SVGL_PC };
/* A <length> (a number with an optional unit) filling all of the text. False when it is not one. */
bool svg_parse_length(const char *s, uint32_t len, svg_len *out);
/* The user units of a length: em and ex use the font size fs, a percentage uses pct_base. */
float svg_len_px(svg_len l, float fs, float pct_base);

/* An <angle> (a number, or a number with deg, grad, rad or turn) filling the text (white space around allowed), in radians.
 * False when it is not one (the marker orient attribute then keeps 0). */
bool svg_parse_angle(const char *s, uint32_t len, double *rad);
/* The transform attribute (a transform list). False when the syntax is wrong (m is then the identity). An empty list is
 * valid and gives the identity. */
bool svg_parse_transform(const char *s, uint32_t len, float m[6]);

typedef struct svg_par {
    uint8_t align;  /* 0 none, 1..9: xMin YMin ... xMax YMax in the order of the SVG spec's table (1 = xMinYMin, 5 = xMidYMid) */
    bool slice;
} svg_par;
/* The preserveAspectRatio attribute (the default is xMidYMid meet). */
svg_par svg_parse_par(const char *s, uint32_t len);
/* The matrix that maps the viewBox vb (x, y, w, h) into the viewport (0, 0, vw, vh) with par. */
void svg_viewbox_matrix(const float vb[4], float vw, float vh, svg_par par, float m[6]);

/* Path data (the d attribute and the points of polygons are parsed by the same code). The path is built up to the first
 * error, as the specification says; *err is true when there was one. verts (may be nullptr) receives the marker vertices. */
typedef struct svg_mvert {
    float x, y;
    float ain, aout; /* the direction (radians) of the path coming into and leaving the vertex */
    bool has_in, has_out;
    uint8_t seg; /* 0 start, 1 mid, 2 end (marker-start, -mid, -end) */
} svg_mvert;
typedef struct svg_verts {
    svg_mvert *v;
    uint32_t n, cap;
} svg_verts;
embi_path *svg_parse_path(const char *d, uint32_t len, float tol, svg_verts *verts, bool *err);
void svg_verts_free(svg_verts *vs);
void svg_verts_from_points(svg_verts *vs, const double *xy, uint32_t n, bool closed);

/* ------------------------------------------------------------------ the walker (walk.c) */

enum { SM_PAINT = 1, SM_HIT };

/* The SVG element kinds. */
enum {
    TG_NONE = 0, TG_SVG, TG_G, TG_DEFS, TG_SYMBOL, TG_USE, TG_SWITCH, TG_A, TG_PATH, TG_RECT, TG_CIRCLE, TG_ELLIPSE, TG_LINE,
    TG_POLYLINE, TG_POLYGON, TG_IMAGE, TG_TEXT, TG_FOREIGN, TG_LGRAD, TG_RGRAD, TG_PATTERN, TG_CLIPPATH, TG_MASK, TG_MARKER, TG_VIEW,
    TG_OTHER,
};

typedef struct svg_ctx {
    const embi_svg_env *env;
    int mode;
    embi_dl *dl;
    float base[6];        /* the space of the root content box -> the page (paint) */
    float dev;            /* device px per CSS px */
    float vpw, vph;       /* the viewport percentages are relative to (user units) */
    float rvw, rvh;       /* the root viewport, CSS px */
    EMBI_VEC(const embi_style *) owned;
    EMBI_VEC(const embi_element *) stack; /* use targets being instantiated, clip paths and masks being applied */
    uint32_t nodes;       /* budget of painted nodes */
    uint32_t bbox_nodes;  /* budget of the bounding box visits (a use bomb makes them exponential) */
    int depth;
    int luma;             /* 0: normal, 1: emitting a luminance mask, 2: emitting a clip path (solid black) */
    embi_document *doc;
    /* hit testing */
    float hx, hy;
    const embi_element *hit;
    const embi_element *cur_el; /* the element being walked and its computed style (the instance style inside a use) */
    const embi_style *cur_st;
    uint32_t clip0;       /* the clip the root clip hangs under */
    const embi_element *view_root; /* an SVG image opened at #view-id: the root, and the view element whose viewBox and */
    const embi_element *view;      /* preserveAspectRatio replace the root's */
    bool root_clip;       /* clip the root viewport whatever its overflow says (SVG images) */
    float tol;            /* the flattening tolerance of the shape being built, in its user units */
} svg_ctx;

uint8_t svg_tag(const embi_element *el);
bool svg_attr_sb(const embi_element *el, const char *name, embi_sb *sb);
bool svg_href_sb(const embi_element *el, embi_sb *sb);
bool svg_attr_len_raw(const embi_element *el, const char *name, svg_len *out);
/* A length attribute in user units (percent against pct_base); fallback when absent or invalid. */
float svg_attr_px(const svg_ctx *c, const embi_element *el, const char *name, float pct_base, float fallback);
float svg_attr_num(const embi_element *el, const char *name, float fallback);
bool svg_ref_id(embi_str text, embi_str *id);
embi_element *svg_ref_element(const embi_document *doc, embi_str ref);
float svg_font_px(const embi_style *st);
uint16_t svg_kw(const embi_style *st, embi_prop_id p);
double svg_num(const embi_style *st, embi_prop_id p, double fallback);
/* Resolves a color value of a style (currentcolor included). */
bool svg_color(const embi_style *st, const embi_value *v, uint32_t *rgba);

/* The geometry of a shape element in its own user space; nullptr when it has none. */
embi_path *svg_shape_path(const svg_ctx *c, const embi_element *el, const embi_style *st, uint8_t tag, svg_verts *verts);
/* The matrix of the transform attribute / property of el (identity when none). */
void svg_el_matrix(const svg_ctx *c, const embi_element *el, const embi_style *st, const float bbox[4], float m[6]);
/* The matrix that a viewport element (nested svg, symbol) puts on its content: x/y/width/height, viewBox,
 * preserveAspectRatio. w, h: the viewport size; clip: the viewport rect in the parent's space. */
void svg_viewport_matrix(const svg_ctx *c, const embi_element *el, float w, float h, float m[6], float vb[4], bool *has_vb);

void svg_ctx_init(svg_ctx *c, const embi_svg_env *env, int mode);
void svg_ctx_free(svg_ctx *c);
/* Walks the svg element: the outer svg is a viewport of size vw x vh whose content matrix is the viewBox mapping. */
void svg_walk_root(svg_ctx *c, embi_element *svg, float vw, float vh);
/* The bbox of an element in its own user space (fill geometry; groups union their children). False when empty. */
bool svg_el_bbox(svg_ctx *c, embi_element *el, const embi_style *st, float out[4]);
const embi_style *svg_style_of(svg_ctx *c, embi_element *el, const embi_style *parent_for_use);

/* pserver.c */
typedef struct svg_paint {
    uint8_t kind;      /* SP_* */
    uint32_t rgba;
    embi_element *server;
    uint32_t fb_rgba;
    bool has_fb;
} svg_paint;
enum { SP_NONE = 0, SP_COLOR, SP_SERVER };
svg_paint svg_style_paint(svg_ctx *c, embi_element *el, const embi_style *st, bool stroke);
/* Emits path as a fill with the paint (solid or gradient). path is taken. m: the shape's matrix (root space), bbox its
 * object bounding box. */
void svg_emit_path(svg_ctx *c, embi_path *path, int rule, const svg_paint *p, const float m[6], const float bbox[4], float opacity,
                   uint32_t clip);

uint32_t svg_em_xform(svg_ctx *c, const float m[6]);
float svg_stroke_width_of(const svg_ctx *c, const embi_style *st);
/* The color of a paint after the mask mode of the context (luminance masks turn colors into alphas). */
uint32_t svg_luma_color(const svg_ctx *c, uint32_t rgba);
/* Emits a stroke as a center line the renderer strokes (a solid color, no dashes). path is taken. */
void svg_emit_stroke_lazy(svg_ctx *c, embi_path *path, const svg_stroke *st, uint32_t rgba, const float m[6], uint32_t clip);
/* A solid fill and a solid center-line stroke of one path in one call; the two list items share the path. */
void svg_emit_fill_stroke_shared(svg_ctx *c, embi_path *path, int rule, uint32_t fill_rgba, float fop, const svg_stroke *st,
                                 uint32_t stroke_rgba, bool stroke_first, const float m[6], uint32_t clip);
uint32_t svg_em_clip(svg_ctx *c, const float rect[4], const float m[6], uint32_t parent);
uint32_t svg_em_group_begin(svg_ctx *c, uint8_t sub, float opacity, uint32_t clip);
void svg_em_group_end(svg_ctx *c, uint32_t begin);
void svg_walk_children(svg_ctx *c, embi_element *parent, const float M[6], uint32_t clip);
/* R4e: a solid stroke of the glyph outline path (taken), in the matrix m, with the stroke properties of st. */
void svg_emit_stroke_of(svg_ctx *c, const embi_style *st, embi_path *path, const float m[6], uint32_t clip, uint32_t rgba);
/* text.c */
bool svg_text_bbox(svg_ctx *c, embi_element *el, float out[4]);
void svg_emit_text(svg_ctx *c, embi_element *el, const embi_style *st, const float M[6], uint32_t clip, bool *hit, double hp[2]);
