#pragma once
/*
 * Canvas internals shared by canvas.c, cpath.c, cstyle.c, ctext.c and cgpu.c. This header includes no CSS and no
 * renderer header: cstyle.c is the only file that sees the CSS headers and cgpu.c the only one that sees the renderer.
 */
#include "canvas/canvas.h"

#include <math.h>

/* ------------------------------------------------------------------ matrices (a b c d e f, x' = a x + c y + e) */

static inline void cm_mul(const double a[6], const double b[6], double out[6])
{
    double r[6];
    r[0] = a[0] * b[0] + a[2] * b[1];
    r[1] = a[1] * b[0] + a[3] * b[1];
    r[2] = a[0] * b[2] + a[2] * b[3];
    r[3] = a[1] * b[2] + a[3] * b[3];
    r[4] = a[0] * b[4] + a[2] * b[5] + a[4];
    r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    for (int i = 0; i < 6; i++) {
        out[i] = r[i];
    }
}

static inline void cm_apply(const double m[6], double x, double y, double *ox, double *oy)
{
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

static inline double cm_det(const double m[6])
{
    return m[0] * m[3] - m[1] * m[2];
}

static inline bool cm_invert(const double m[6], double out[6])
{
    double det = cm_det(m);
    if (!(det != 0.0) || !isfinite(det)) {
        return false;
    }
    double id = 1.0 / det;
    double r[6] = {m[3] * id, -m[1] * id, -m[2] * id, m[0] * id, 0, 0};
    r[4] = -(r[0] * m[4] + r[2] * m[5]);
    r[5] = -(r[1] * m[4] + r[3] * m[5]);
    for (int i = 0; i < 6; i++) {
        out[i] = r[i];
    }
    return true;
}

static inline bool cm_finite(const double *v, int n)
{
    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ paths */

enum { CP_M = 0, CP_L, CP_Q, CP_C, CP_Z };

struct embi_cpath {
    uint32_t rc;
    EMBI_VEC(uint8_t) ops;
    EMBI_VEC(double) pts; /* M, L: 2; Q: 4; C: 6; Z: 0 */
    double cur[2], start[2];
    bool has_cur;   /* a current point exists (a subpath was started) */
    bool closed;    /* the last op was Z: the next segment starts a new subpath at start */
};

/* Calls fn for the flattened polylines (curves split to within tol); the polyline of each subpath and whether it was
 * closed. Used by hit testing and the bounds. */
typedef void (*cp_poly_fn)(void *user, const double *xy, uint32_t n, bool closed);
void cpath_flatten(const embi_cpath *p, double tol, cp_poly_fn fn, void *user);

/* ------------------------------------------------------------------ gradients and patterns */

typedef struct cstop {
    float off;
    float rgba[4]; /* straight alpha, 0..1 */
} cstop;

struct embi_cgrad {
    uint32_t rc;
    int kind;
    double g[6];
    EMBI_VEC(cstop) stops;
    struct embi_cgrad *snap; /* an immutable copy made for the commands, invalidated by addColorStop */
    bool dirty;
    void *wrapper;
};

struct embi_cpattern {
    uint32_t rc;
    embi_cimage *im;
    embi_canvas *src; /* unused: patterns of canvases are read back into im */
    int repeat;
    double m[6];
    void *wrapper;
};

/* The paint of the state: the objects the script assigned. */
typedef struct cpaint {
    uint8_t kind; /* 0 color, 1 gradient, 2 pattern */
    uint8_t rgb[3];
    double alpha;
    embi_cgrad *g;
    embi_cpattern *p;
} cpaint;

/* The paint of a command: values that no later change of the script's objects can touch. */
typedef struct cpaint_cmd {
    uint8_t kind;
    float rgba[4]; /* straight alpha */
    embi_cgrad *g; /* a snapshot, retained */
    embi_cimage *im; /* retained */
    int repeat;
    double pm[6];
} cpaint_cmd;

/* ------------------------------------------------------------------ the state */

enum {
    CV_OP_SOURCE_OVER = 0, CV_OP_SOURCE_IN, CV_OP_SOURCE_OUT, CV_OP_SOURCE_ATOP, CV_OP_DEST_OVER, CV_OP_DEST_IN,
    CV_OP_DEST_OUT, CV_OP_DEST_ATOP, CV_OP_LIGHTER, CV_OP_COPY, CV_OP_XOR, CV_OP_MULTIPLY, CV_OP_SCREEN,
    CV_OP_OVERLAY, CV_OP_DARKEN, CV_OP_LIGHTEN, CV_OP_COLOR_DODGE, CV_OP_COLOR_BURN, CV_OP_HARD_LIGHT,
    CV_OP_SOFT_LIGHT, CV_OP_DIFFERENCE, CV_OP_EXCLUSION, CV_OP_HUE, CV_OP_SATURATION, CV_OP_COLOR,
    CV_OP_LUMINOSITY, CV_OP_CLEAR, CV_OP_COUNT
};

enum { CV_ALIGN_START = 0, CV_ALIGN_END, CV_ALIGN_LEFT, CV_ALIGN_RIGHT, CV_ALIGN_CENTER };
enum { CV_BASE_ALPHABETIC = 0, CV_BASE_TOP, CV_BASE_HANGING, CV_BASE_MIDDLE, CV_BASE_IDEOGRAPHIC, CV_BASE_BOTTOM };

typedef struct cfont {
    char *css;     /* the serialized value (the getter) */
    char *family;  /* the family list, comma separated */
    float px;
    float weight;
    float stretch; /* percent */
    uint8_t style; /* EMBI_STYLE_* values: 0 normal, 1 italic, 2 oblique */
    bool small_caps;
} cfont;

typedef struct cstate {
    double ctm[6];
    cpaint fill, stroke;
    double alpha;
    uint8_t op;
    bool smooth;
    uint8_t quality;
    /* shadow */
    uint8_t sh_rgb[3];
    double sh_alpha;
    double sh_blur, sh_x, sh_y;
    /* line */
    double line_width, miter;
    uint8_t cap, join;
    double *dash;
    uint32_t ndash;
    double dash_off;
    /* text */
    cfont font;
    uint8_t align, baseline, dir; /* dir: 0 ltr, 1 rtl, 2 inherit */
    double letter_spacing, word_spacing;
    char *letter_css, *word_css;
    uint8_t kerning;   /* 0 auto, 1 normal, 2 none */
    uint8_t stretch;   /* font-stretch keyword index */
    uint8_t caps;      /* font-variant-caps keyword index */
    uint8_t rendering; /* text-rendering keyword index */
    char *filter;
    uint32_t clip;     /* index into canvas.clips, 0 none */
    bool is_layer;     /* a saved entry made by beginLayer (not by save) */
} cstate;

/* The part of the state a draw command needs at replay. */
typedef struct csnap {
    double ctm[6];
    float alpha;
    uint8_t op;
    uint8_t smooth;
    uint8_t quality;
    uint8_t clear; /* globalCompositeOperation clear: replayed as destination-out of an opaque source */
    float sh_rgba[4]; /* straight; alpha 0 when there is no shadow */
    float sh_blur, sh_x, sh_y;
    uint32_t clip;
} csnap;

typedef struct cclip {
    embi_cpath *path; /* in device space */
    bool evenodd;
    uint32_t parent;
} cclip;

typedef struct cstroke {
    double width, miter, dash_off;
    uint8_t cap, join;
    double *dash;
    uint32_t ndash;
} cstroke;

typedef struct ctextcmd {
    char *text;
    uint32_t len;
    double x, y, max_width; /* max_width < 0: none */
    cfont font;
    uint8_t align, baseline, dir, kerning, stretch, caps;
    double letter_spacing, word_spacing;
    bool rtl;
} ctextcmd;

enum { CK_FILL = 1, CK_STROKE, CK_CLEAR, CK_IMAGE, CK_TEXT, CK_PUT };

typedef struct ccmd {
    uint8_t kind;
    bool evenodd;
    bool stroke_text;
    uint32_t snap;
    embi_cpath *path;
    cpaint_cmd paint;
    cstroke line;
    embi_cimage *im;
    embi_canvas *src_cv; /* a canvas source: drawn from its texture when the command runs */
    uint32_t src_w, src_h;
    double s[4], d[4];
    int32_t px, py;
    ctextcmd text;
} ccmd;

struct embi_canvas {
    embi_canvas_env env;
    const void *key;
    uint32_t w, h;
    bool alpha;
    cstate st;
    EMBI_VEC(cstate) stack;
    embi_cpath *path;
    EMBI_VEC(ccmd) cmds;
    EMBI_VEC(csnap) snaps;
    EMBI_VEC(cclip) clips; /* index 0 is a sentinel */
    uint32_t version;
    bool need_clear;
    bool font_set;
    /* GPU */
    embi_canvas_gpu *gpu;
    void *tex; /* SDL_GPUTexture * */
    uint32_t tex_w, tex_h;
    struct embi_canvas *gnext, *gprev; /* the manager's list of canvases with a texture or work */
    bool listed;
    struct embi_canvas *anext, *aprev; /* the manager's registry of every canvas made for it */
    bool registered;
};

/* canvas.c */
void cv_state_init(embi_canvas *cv, cstate *s);
void cv_state_free(cstate *s);
void cv_paint_cmd(const cpaint *p, cpaint_cmd *out);
void cv_paint_cmd_free(cpaint_cmd *p);
void cv_cmd_free(ccmd *c);
void cv_cmds_clear(embi_canvas *cv);
void cv_touch(embi_canvas *cv);
#define CV_MAX_SIDE EMBI_CANVAS_MAX_SIDE
#define CV_MAX_AREA EMBI_CANVAS_MAX_AREA
#define cv_size_ok embi_canvas_size_ok

/* cstyle.c: colors and fonts (the only CSS user) */
bool cs_parse_color(embi_canvas *cv, embi_str css, uint8_t rgb[3], double *alpha);
void cs_color_string(const uint8_t rgb[3], double alpha, embi_sb *out);
/* Parses a font shorthand into f (the strings are allocated); base_px/root_px resolve relative sizes. */
bool cs_parse_font(embi_str css, float base_px, float root_px, cfont *out);
void cfont_free(cfont *f);
void cfont_copy(cfont *dst, const cfont *src);
bool cs_parse_length(embi_str css, float base_px, float root_px, double *out); /* a CSS length in px (letter/word spacing) */
void cv_element_style(const embi_canvas *cv, float *font_px, uint32_t *color, bool *rtl, float *root_px);
const char *cs_font_stretch_name(uint8_t i);
const char *cs_caps_name(uint8_t i);
const char *cs_rendering_name(uint8_t i);

/* ctext.c: CPU text metrics (shared by measureText and the replay) */
typedef struct ctext_layout ctext_layout;
/* Shapes text with the command's font; returns nullptr without a text system. */
ctext_layout *ctext_shape(embi_text *t, const ctextcmd *tc);
void ctext_layout_free(ctext_layout *l);
double ctext_advance(const ctext_layout *l);
/* Draw origin (x, y) of the baseline start after the alignment and baseline of the command, and the scale for max_width. */
void ctext_origin(const ctext_layout *l, const ctextcmd *tc, double *ox, double *oy, double *sx);
void ctext_metrics(const ctext_layout *l, const ctextcmd *tc, embi_text_metrics *m);
/* The glyph outlines as a user-space path (strokeText): points are transformed by (ox, oy) and scale sx. */
void ctext_outline(const ctext_layout *l, embi_cpath *dst, double ox, double oy, double sx);

/* cgpu.c: the canvas side of the GPU manager */
void cgpu_register(embi_canvas *cv);   /* a canvas made with env.gpu: joins the registry and the flush list */
void cgpu_unregister(embi_canvas *cv); /* detaches and leaves the registry */
void cgpu_attach(embi_canvas *cv);
void cgpu_detach(embi_canvas *cv);
/* Draws the pending commands (cmd: an SDL_GPUCommandBuffer, or nullptr to record into one of its own and wait). */
void cgpu_flush_canvas(embi_canvas *cv, void *cmd);
bool cgpu_read(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t *out);
void cgpu_release_image(embi_cimage *im);

/* ctext.c: draws the layout with its baseline origin at (0, 0) of the frame (frame: an embi_frame *). */
void ctext_draw(const ctext_layout *l, void *frame, uint32_t color, uint32_t xform, uint32_t clip);
