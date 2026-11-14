#pragma once
/*
 * Shared declarations of the canvas bindings (R4b): impl_canvas.c (element, context, state), impl_canvas_path.c (Path2D and
 * rounded rects) and impl_canvas_data.c (gradients, patterns, ImageData, ImageBitmap, TextMetrics, images). Private to the
 * bindings module.
 */
#include "bindings/canvas_host.h"
#include "bindings/gen/canvas.h"
#include "bindings/gen/htmlcanvas.h"
#include "bindings/impl.h"
#include "bindings/impl_loader.h"
#include "bindings/svg_int.h"
#include "canvas/canvas.h"

/* What the embedder gave a context (canvas_host.h), shared by its canvases. */
typedef struct cvimg {
    char *url;
    uint32_t len;
    embi_cimage *im; /* a straight RGBA8 copy of the decoded image, shared by every draw of it */
} cvimg;

typedef struct cvhost {
    uint32_t rc;
    embi_bind_ctx *c; /* nullptr after the context was freed */
    embi_canvas_env base;
    /* The decoded <img> pictures by resolved URL (oldest first), valid for the embedder's image generation img_gen. A
     * sprite drawn many times shares one picture, so its GPU texture is made and uploaded once. */
    EMBI_VEC(cvimg) imgs;
    size_t img_bytes;
    uint32_t img_gen;
    /* The last computed style read for a canvas element, valid while the document's style epoch is unchanged. */
    const void *sty_el;
    uint64_t sty_epoch;
    float sty_font_px, sty_root_px;
    uint32_t sty_color;
    bool sty_rtl;
} cvhost;

void cvh_images_clear(cvhost *h);

void cvh_unref(cvhost *h);

/* Throws the DOMException (or TypeError) of an EMBI_CV_* code and returns EMBI_BIND_THREW; EMBI_DOM_OK for EMBI_CV_OK. */
embi_dom_err cv_throw(embi_bind_ctx *c, int code, const char *what);
/* The canvas of a context wrapper. */
embi_canvas *cv_of(embi_bind_obj *self);
/* Resets the canvas of the element to its attribute size (force: even when it is the same). */
void cv_sync_size(embi_element *el, bool force);

/* Wrappers of the objects the context hands out. g, p are retained. */
embi_js_value cv_gradient_value(embi_bind_ctx *c, embi_cgrad *g);
embi_js_value cv_pattern_value(embi_bind_ctx *c, embi_cpattern *p);
embi_js_value cv_metrics_value(embi_bind_ctx *c, const embi_text_metrics *m);

/* roundRect on a path under m: radii is any (a number, a DOMPointInit or a sequence of up to four). */
embi_dom_err cv_round_rect(embi_bind_ctx *c, embi_cpath *p, const double *m, double x, double y, double w, double h,
                           embi_js_value radii);
