#pragma once
#ifndef EMBER_CANVAS
#error "EMBER_CANVAS must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * Canvas 2D (R4b). The module has three layers:
 *
 *   - the context: the drawing state, the state stack, the current path and the commands that draw calls record
 *     (canvas.c, cpath.c, cstyle.c, ctext.c). It is plain CPU data and needs no GPU. The bindings (impl_canvas*.c)
 *     are a thin layer over it.
 *   - the replay (cgpu.c): at frame time, or right away when pixels are read, the recorded commands are drawn into
 *     the canvas' backing texture: fills and strokes on the Slug path fill, text through the text system,
 *     compositing, clipping, shadows and gradients in shaders/canvas.slang. Embedders create one embi_canvas_gpu
 *     per renderer and call embi_canvas_gpu_flush before the frame that shows the canvases.
 *   - the registry: the painter asks embi_canvas_paint_info for the element's current picture (a version that
 *     changes with every draw, so the damage of the display list sees the change).
 *
 * Execution model. A draw call appends one command. Nothing touches the GPU until the flush; the cost of a draw
 * is the command (a path is shared by reference, a gradient or pattern is snapshotted). willReadFrequently: the
 * pixels of a canvas live only in the backing texture and a read flushes first and downloads; there is no CPU
 * copy. A readback is a stall of one GPU round trip however the hint is set, and the hint is accepted and
 * ignored (a CPU copy would double the memory of every canvas for a rare pattern). A canvas with no GPU (a
 * document with no renderer) keeps its state but reads back as transparent black.
 *
 * Coordinates. The current path stores its points transformed by the CTM at the time of the call, as the
 * specification says (Path2D stores user-space points and is transformed when drawn). All numbers are doubles.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_canvas embi_canvas;
typedef struct embi_cpath embi_cpath;
typedef struct embi_cimage embi_cimage;
typedef struct embi_cgrad embi_cgrad;
typedef struct embi_cpattern embi_cpattern;
typedef struct embi_canvas_gpu embi_canvas_gpu;
typedef struct embi_text embi_text;

#if !EMBER_CANVAS
/* EMBER_CANVAS=OFF (SZ3b): src/canvas is not built. The few functions that the renderer and the painter
 * call exist as empty stubs, so that those files need no #if: no canvas ever exists, so no element has a picture and the
 * renderer has no canvas texture manager. */
static inline bool embi_canvas_paint_info(const void *key, uint32_t *version, uint32_t *w, uint32_t *h)
{
    (void)key;
    (void)version;
    (void)w;
    (void)h;
    return false;
}
static inline embi_canvas_gpu *embi_canvas_gpu_create(void *gpu, void *renderer, void *pool, int format)
{
    (void)gpu;
    (void)renderer;
    (void)pool;
    (void)format;
    return nullptr;
}
static inline void embi_canvas_gpu_destroy(embi_canvas_gpu *g)
{
    (void)g;
}
static inline void embi_canvas_gpu_flush(embi_canvas_gpu *g, void *cmd)
{
    (void)g;
    (void)cmd;
}
static inline void *embi_canvas_gpu_texture(embi_canvas_gpu *g, const void *key, uint32_t *w, uint32_t *h)
{
    (void)g;
    (void)key;
    (void)w;
    (void)h;
    return nullptr;
}
#else

/* The largest bitmap (canvas, image, ImageData rectangle) the module handles: each side and the area. A canvas beyond it
 * keeps its state but has no pixels (toDataURL gives "data:,", reads return transparent black). */
#define EMBI_CANVAS_MAX_SIDE 16384u
#define EMBI_CANVAS_MAX_AREA (1u << 28)
bool embi_canvas_size_ok(uint32_t w, uint32_t h);

/* Errors of the operations that throw. */
enum {
    EMBI_CV_OK = 0,
    EMBI_CV_INDEX_SIZE,    /* IndexSizeError */
    EMBI_CV_INVALID_STATE, /* InvalidStateError */
    EMBI_CV_SYNTAX,        /* SyntaxError */
    EMBI_CV_TYPE,          /* TypeError */
    EMBI_CV_SECURITY,      /* SecurityError */
};

/* What the embedder gives a canvas (copied). All hooks are optional. */
typedef struct embi_canvas_env {
    void *user;
    embi_canvas_gpu *gpu; /* nullptr: no pixels */
    embi_text *text;      /* fonts for fillText and measureText; nullptr: no text */
    /* A canvas changed: a repaint is due. */
    void (*dirty)(void *user);
    /* Straight RGBA8 pixels of an image URL (resolved by the embedder), valid until the next call. */
    bool (*image_pixels)(void *user, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h);
    /* A number that changes whenever an image URL may decode to different pixels (a reload, a new document); nullptr: the
     * pictures of images are not cached. */
    uint32_t (*image_gen)(void *user);
    /* The computed style of the canvas element: font-size (px), color (RGBA8, r in the low byte), whether the text
     * direction is rtl, and the root element's font size. Without the hook: 10px, black, ltr, 16px (the values a canvas
     * that is not in a document has). */
    void (*element_style)(void *user, const void *el, float *font_px, uint32_t *color, bool *rtl, float *root_px);
    /* When gpu is nullptr: the GPU of a new canvas, asked for when the canvas is created (a windowed view makes its GPU
     * device on first need, so a page with no canvas does not wait for it while it loads). nullptr: no pixels. */
    embi_canvas_gpu *(*gpu_get)(void *user);
} embi_canvas_env;

/* ------------------------------------------------------------------ images */

/* A refcounted straight RGBA8 picture (ImageBitmap, a decoded <img>, or putImageData's rectangle). */
struct embi_cimage {
    uint32_t rc;
    uint32_t w, h;
    uint8_t *px; /* w * h * 4 */
    /* The GPU copy (cgpu.c), made when the image is first drawn. */
    void *tex;
    embi_canvas_gpu *tex_owner;
    uint32_t id;
};

[[nodiscard]] embi_cimage *embi_cimage_new(uint32_t w, uint32_t h);
embi_cimage *embi_cimage_retain(embi_cimage *im);
void embi_cimage_release(embi_cimage *im);

/* ------------------------------------------------------------------ paths */

[[nodiscard]] embi_cpath *embi_cpath_new(void);
embi_cpath *embi_cpath_retain(embi_cpath *p);
void embi_cpath_release(embi_cpath *p);
[[nodiscard]] embi_cpath *embi_cpath_clone(const embi_cpath *p);
void embi_cpath_reset(embi_cpath *p);
bool embi_cpath_empty(const embi_cpath *p);
/* m: the CTM applied to the points (a b c d e f), nullptr for the identity. Non-finite arguments change nothing. */
void embi_cpath_move_to(embi_cpath *p, const double *m, double x, double y);
void embi_cpath_line_to(embi_cpath *p, const double *m, double x, double y);
void embi_cpath_quad_to(embi_cpath *p, const double *m, double cx, double cy, double x, double y);
void embi_cpath_cubic_to(embi_cpath *p, const double *m, double c1x, double c1y, double c2x, double c2y, double x,
                         double y);
int embi_cpath_arc_to(embi_cpath *p, const double *m, double x1, double y1, double x2, double y2, double r);
int embi_cpath_arc(embi_cpath *p, const double *m, double x, double y, double r, double a0, double a1, bool ccw);
int embi_cpath_ellipse(embi_cpath *p, const double *m, double x, double y, double rx, double ry, double rot,
                       double a0, double a1, bool ccw);
void embi_cpath_rect(embi_cpath *p, const double *m, double x, double y, double w, double h);
/* radii: four (x, y) pairs, top-left first. Already normalized to nonnegative finite values by the caller; this
 * function scales them down when they overlap. */
void embi_cpath_round_rect(embi_cpath *p, const double *m, double x, double y, double w, double h,
                           const double radii[8]);
void embi_cpath_close(embi_cpath *p);
/* Appends the subpaths of src transformed by m (nullptr: identity). */
void embi_cpath_add(embi_cpath *dst, const embi_cpath *src, const double *m);
/* Parses SVG path data into p (user space). Errors stop the parse and keep what was read (SVG error handling). */
void embi_cpath_parse_svg(embi_cpath *p, embi_str d);
/* The current point; false without one. */
bool embi_cpath_current(const embi_cpath *p, double *x, double *y);
/* The device bounds of the control polygon, false when empty. */
bool embi_cpath_bounds(const embi_cpath *p, double b[4]);
/* Whether (x, y) is inside under the nonzero or even-odd rule (the path is in the point's space). */
bool embi_cpath_contains(const embi_cpath *p, double x, double y, bool evenodd);

/* ------------------------------------------------------------------ gradients and patterns */

enum { EMBI_CG_LINEAR = 0, EMBI_CG_RADIAL = 1, EMBI_CG_CONIC = 2 };

[[nodiscard]] embi_cgrad *embi_cgrad_new(int kind, const double g[6]); /* linear x0 y0 x1 y1; radial x0 y0 r0 x1 y1 r1; conic angle x y */
embi_cgrad *embi_cgrad_retain(embi_cgrad *g);
void embi_cgrad_release(embi_cgrad *g);
/* The offset must be in 0..1; the color is a CSS color. Returns EMBI_CV_INDEX_SIZE or EMBI_CV_SYNTAX on error. */
int embi_cgrad_add_stop(embi_cgrad *g, embi_canvas *cv, double offset, embi_str css);
void *embi_cgrad_wrapper(const embi_cgrad *g);
void embi_cgrad_set_wrapper(embi_cgrad *g, void *w);

enum { EMBI_CP_REPEAT = 0, EMBI_CP_REPEAT_X = 1, EMBI_CP_REPEAT_Y = 2, EMBI_CP_NO_REPEAT = 3 };

/* A pattern of a picture. The picture is shared. */
[[nodiscard]] embi_cpattern *embi_cpattern_new(embi_cimage *im, int repeat);
embi_cpattern *embi_cpattern_retain(embi_cpattern *p);
void embi_cpattern_release(embi_cpattern *p);
void embi_cpattern_set_transform(embi_cpattern *p, const double m[6]);
void *embi_cpattern_wrapper(const embi_cpattern *p);
void embi_cpattern_set_wrapper(embi_cpattern *p, void *w);

/* ------------------------------------------------------------------ the canvas */

/* key: the element (an opaque pointer used to find the canvas again from the painter). alpha: the context attribute
 * (false: an opaque black canvas). */
[[nodiscard]] embi_canvas *embi_canvas_create(const embi_canvas_env *env, const void *key, uint32_t w, uint32_t h,
                                              bool alpha);
void embi_canvas_destroy(embi_canvas *cv);
/* The width or height attribute changed (or the context was reset): the bitmap is cleared, the state returns to its
 * defaults and the path is emptied. */
void embi_canvas_resize(embi_canvas *cv, uint32_t w, uint32_t h);
uint32_t embi_canvas_width(const embi_canvas *cv);
uint32_t embi_canvas_height(const embi_canvas *cv);
bool embi_canvas_has_alpha(const embi_canvas *cv);
/* The element key the canvas was made for. */
const void *embi_canvas_key(const embi_canvas *cv);
/* ctx.reset() */
void embi_canvas_reset(embi_canvas *cv);

/* The picture of the element for the painter: false when the element has no context. version changes with every draw. */
bool embi_canvas_paint_info(const void *key, uint32_t *version, uint32_t *w, uint32_t *h);
embi_canvas *embi_canvas_find(const void *key);

/* state */
void embi_canvas_save(embi_canvas *cv);
/* restore: EMBI_CV_INVALID_STATE when the top of the stack is a layer (an open beginLayer), else 0. */
int embi_canvas_restore(embi_canvas *cv);
/* beginLayer and endLayer keep the state semantics of the Canvas layers proposal: the rendering state (alpha, composite
 * operation, shadow, filter) is reset inside the layer and restored by endLayer, which also restores the rest of the state
 * like restore. The layer content is NOT composed as a group (labelled not-yet). endLayer returns EMBI_CV_INVALID_STATE
 * when the top of the stack is not a layer. */
void embi_canvas_begin_layer(embi_canvas *cv);
int embi_canvas_end_layer(embi_canvas *cv);
void embi_canvas_get_transform(const embi_canvas *cv, double m[6]);
void embi_canvas_set_transform(embi_canvas *cv, const double m[6]);
void embi_canvas_transform(embi_canvas *cv, const double m[6]); /* multiplies the CTM by m (CTM * m) */
void embi_canvas_translate(embi_canvas *cv, double x, double y);
void embi_canvas_scale(embi_canvas *cv, double x, double y);
void embi_canvas_rotate(embi_canvas *cv, double a);
double embi_canvas_get_alpha(const embi_canvas *cv);
void embi_canvas_set_alpha(embi_canvas *cv, double a);
const char *embi_canvas_get_composite(const embi_canvas *cv);
void embi_canvas_set_composite(embi_canvas *cv, embi_str op);
bool embi_canvas_get_smoothing(const embi_canvas *cv);
void embi_canvas_set_smoothing(embi_canvas *cv, bool on);
const char *embi_canvas_get_quality(const embi_canvas *cv);
void embi_canvas_set_quality(embi_canvas *cv, embi_str q);

/* styles: set_*_css returns false for an invalid color (the value is kept). which: 0 fill, 1 stroke */
bool embi_canvas_set_color(embi_canvas *cv, int which, embi_str css);
void embi_canvas_set_gradient(embi_canvas *cv, int which, embi_cgrad *g);
void embi_canvas_set_pattern(embi_canvas *cv, int which, embi_cpattern *p);
/* The style: kind 0 string (css filled in), 1 gradient, 2 pattern. */
int embi_canvas_get_style(const embi_canvas *cv, int which, embi_sb *css, embi_cgrad **g, embi_cpattern **p);

double embi_canvas_get_line_width(const embi_canvas *cv);
void embi_canvas_set_line_width(embi_canvas *cv, double w);
const char *embi_canvas_get_line_cap(const embi_canvas *cv);
void embi_canvas_set_line_cap(embi_canvas *cv, embi_str s);
const char *embi_canvas_get_line_join(const embi_canvas *cv);
void embi_canvas_set_line_join(embi_canvas *cv, embi_str s);
double embi_canvas_get_miter_limit(const embi_canvas *cv);
void embi_canvas_set_miter_limit(embi_canvas *cv, double v);
/* Returns false (and does nothing) for a negative or non-finite entry. */
bool embi_canvas_set_line_dash(embi_canvas *cv, const double *v, uint32_t n);
uint32_t embi_canvas_get_line_dash(const embi_canvas *cv, const double **v);
double embi_canvas_get_dash_offset(const embi_canvas *cv);
void embi_canvas_set_dash_offset(embi_canvas *cv, double v);

void embi_canvas_get_shadow_color(const embi_canvas *cv, embi_sb *css);
void embi_canvas_set_shadow_color(embi_canvas *cv, embi_str css);
double embi_canvas_get_shadow_blur(const embi_canvas *cv);
void embi_canvas_set_shadow_blur(embi_canvas *cv, double v);
double embi_canvas_get_shadow_x(const embi_canvas *cv);
double embi_canvas_get_shadow_y(const embi_canvas *cv);
void embi_canvas_set_shadow_x(embi_canvas *cv, double v);
void embi_canvas_set_shadow_y(embi_canvas *cv, double v);

/* text attributes: which 0 font, 1 textAlign, 2 textBaseline, 3 direction, 4 letterSpacing, 5 wordSpacing,
 * 6 fontKerning, 7 fontStretch, 8 fontVariantCaps, 9 textRendering, 10 filter (stored, not applied). The getter
 * fills out; the setter ignores an invalid value. */
void embi_canvas_get_text_attr(const embi_canvas *cv, int which, embi_sb *out);
void embi_canvas_set_text_attr(embi_canvas *cv, int which, embi_str value);

/* rectangles and paths */
void embi_canvas_clear_rect(embi_canvas *cv, double x, double y, double w, double h);
void embi_canvas_fill_rect(embi_canvas *cv, double x, double y, double w, double h);
void embi_canvas_stroke_rect(embi_canvas *cv, double x, double y, double w, double h);
embi_cpath *embi_canvas_path(embi_canvas *cv); /* the current default path (for the path calls) */
void embi_canvas_begin_path(embi_canvas *cv);
/* path: nullptr draws the current default path. rule: true even-odd. */
void embi_canvas_fill(embi_canvas *cv, embi_cpath *path, bool evenodd);
void embi_canvas_stroke(embi_canvas *cv, embi_cpath *path);
void embi_canvas_clip(embi_canvas *cv, embi_cpath *path, bool evenodd);
bool embi_canvas_in_path(embi_canvas *cv, embi_cpath *path, double x, double y, bool evenodd);
bool embi_canvas_in_stroke(embi_canvas *cv, embi_cpath *path, double x, double y);

/* text */
typedef struct embi_text_metrics {
    double width;
    double left, right, ascent, descent; /* actualBoundingBoxLeft, Right, Ascent, Descent */
    double font_ascent, font_descent;
    double em_ascent, em_descent;
    double hanging, alphabetic, ideographic;
} embi_text_metrics;
void embi_canvas_draw_text(embi_canvas *cv, embi_str utf8, double x, double y, double max_width, bool stroke);
bool embi_canvas_measure_text(embi_canvas *cv, embi_str utf8, embi_text_metrics *out);

/* images. src: the picture to draw, or a canvas (cv_src). sx, sy, sw, sh: the source rect in the source's px; dx..dh
 * the destination. Returns EMBI_CV_INVALID_STATE for an empty source. */
int embi_canvas_draw_image(embi_canvas *cv, embi_cimage *im, embi_canvas *src, const double s[4], const double d[4]);
embi_cpattern *embi_canvas_pattern_from(embi_canvas *cv, embi_cimage *im, embi_canvas *src, int repeat);
embi_cimage *embi_canvas_snapshot(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h);
/* Straight RGBA8 pixels of the rectangle (x, y, w, h); out is w * h * 4 bytes. Parts outside the canvas are zero. */
bool embi_canvas_read(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t *out);
/* putImageData: pixels is the dirty rectangle (dw x dh), placed at (x, y) of the bitmap, ignoring the state. */
void embi_canvas_put(embi_canvas *cv, const uint8_t *px, uint32_t dw, uint32_t dh, int32_t x, int32_t y);

/* Encodes the whole canvas as PNG (a minimal stored-block encoder, cpng.c); false without pixels. */
bool embi_canvas_to_png(embi_canvas *cv, embi_sb *out);
/* Parses a CSS color (no currentcolor) into RGBA8, r in the low byte. */
bool embi_canvas_parse_color(embi_str css, uint32_t *rgba);
/* Encodes straight RGBA8 pixels as a PNG into out. */
void embi_png_encode_rgba(const uint8_t *px, uint32_t w, uint32_t h, embi_sb *out);

/* Draws everything recorded so far (no-op without a GPU). */
void embi_canvas_flush(embi_canvas *cv);

/* ------------------------------------------------------------------ GPU side (cgpu.c) */

/* Opaque renderer pieces are passed as void * so that this header does not include the renderer headers (they clash
 * with the CSS ones). renderer: embi_renderer *, pool: embi_layer_pool *. */
[[nodiscard]] embi_canvas_gpu *embi_canvas_gpu_create(void *gpu, void *renderer, void *pool, int format);
void embi_canvas_gpu_destroy(embi_canvas_gpu *g);
/* Draws the pending commands of every canvas into their textures, recording into cmd (an SDL_GPUCommandBuffer). */
void embi_canvas_gpu_flush(embi_canvas_gpu *g, void *cmd);
/* The backing texture of the canvas (SDL_GPUTexture *) and its size, for drawing it as an image; nullptr when none. */
void *embi_canvas_gpu_texture(embi_canvas_gpu *g, const void *key, uint32_t *w, uint32_t *h);
/* Bytes of the canvases' textures. */
size_t embi_canvas_gpu_bytes(const embi_canvas_gpu *g);
#endif /* EMBER_CANVAS */
