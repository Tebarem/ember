#pragma once
/*
 * Renderer core: batched box primitives (fills, borders, linear, radial and conic gradients, and
 * atlas images) and outlines (glyphs and paths, see gpu/path.h and gpu/slug.h) through one
 * ubershader pipeline, with transforms, rounded-rect clips, and one upload pass per frame. Prims
 * are drawn in the order they were recorded. A frame draws in as few batches as its textures
 * allow.
 *
 * Usage, one frame:
 *     embi_frame *f = embi_frame_begin(r, w, h, scale);
 *     uint32_t x = embi_frame_xform(f, m);                      // optional, 0 = identity
 *     uint32_t c = embi_frame_clip(f, rect, radii, x, 0);       // optional
 *     embi_frame_fill(f, rect, radii, rgba, x, c);
 *     embi_frame_end(f, cmd, target, true, 0);                  // records copy and render passes
 *
 * Coordinates are CSS px in local space. device_scale is applied once, to every transform.
 * Colors are RGBA8 non-premultiplied with r in the low byte. The target is blended in gamma
 * space with premultiplied alpha.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#include "gpu/atlas.h"
#include "gpu/gpu.h"
#include "gpu/render_types.h"

typedef struct embi_renderer embi_renderer;
typedef struct embi_frame embi_frame; /* owned by the renderer, valid between begin and end */

/* The target format must be one the pipeline can render to, and it must match the target that
 * embi_frame_end receives. */
[[nodiscard]] embi_renderer *embi_renderer_create(embi_gpu *g, SDL_GPUTextureFormat target_format);
void embi_renderer_destroy(embi_renderer *r);

/* Starts creating, on a worker thread, the GPU buffers the first frame would otherwise create inside
 * embi_frame_end (the storage buffers, the upload buffer, the slug buffer), each at a size that holds a
 * small page. A larger frame grows them as before. embi_frame_end and embi_renderer_destroy wait for the
 * thread, so the call may be made at any time after create; without a thread it works inline. */
void embi_renderer_warm(embi_renderer *r);

/* True when a region frame (embi_frame_begin_region) can be drawn: its clear needs a pipeline that may fail to be made.
 * A caller that gets false must draw a full frame instead; embi_frame_end drops a region frame in that case. It waits
 * for the warm-up thread. */
[[nodiscard]] bool embi_renderer_region_ok(embi_renderer *r);

/* Test hook: forgets the region clear pipeline and makes every attempt to create it fail. */
void embi_renderer_fault_clear_pipeline(embi_renderer *r);

/* Waits for the thread embi_renderer_warm started (no-op when none runs). */
void embi_renderer_warm_wait(embi_renderer *r);

/* Number of GPU buffers (storage and transfer) the renderer itself has created so far, the warm-up's
 * included. A frame that fits the warmed buffers does not raise it. */
[[nodiscard]] uint32_t embi_renderer_buffer_creates(const embi_renderer *r);

/* Number of outline blobs (glyphs and paths) in the renderer's blob cache (tests and tuning). */
[[nodiscard]] uint32_t embi_renderer_outline_blobs(const embi_renderer *r);

/* Begins a frame of width_px x height_px device px. Only one frame may be open per renderer.
 * Returns nullptr for an empty size. */
[[nodiscard]] embi_frame *embi_frame_begin(embi_renderer *r, uint32_t width_px, uint32_t height_px,
                                           float device_scale);

/* A device-px rectangle: the origin is the top-left of the target, and w and h are sizes. */
typedef struct embi_dev_rect {
    int32_t x, y, w, h;
} embi_dev_rect;

/* Begins a partial redraw of damage (device px) in a target of width_px x height_px. The frame
 * keeps the target's other pixels: its render pass loads the target and sets the scissor to the
 * damage, and prims that cannot touch the damage are dropped on the CPU when they are added.
 * The damage is clipped to the target. With embi_frame_end(clear == true) the damage alone is
 * cleared to clear_rgba first, so a redraw of a region equals the full frame there. Pass
 * clear == false to paint over what the target holds. An empty damage draws nothing. Other
 * recording calls and the rules of embi_frame_begin are unchanged. */
[[nodiscard]] embi_frame *embi_frame_begin_region(embi_renderer *r, uint32_t width_px,
                                                  uint32_t height_px, float device_scale,
                                                  embi_dev_rect damage);

/* Draws the frame into the w x h rectangle at (x, y) of a target of tex_w x tex_h px (the frame's size is the rectangle's):
 * the viewport is the rectangle and the scissor never leaves it. A frame with an offset never clears the whole target:
 * embi_frame_end(clear == true) needs a region frame (embi_frame_begin_region) then, whose damage is in the rectangle's
 * coordinates; a full frame with an offset and clear == true is drawn as if clear were false (logged once). Call it right
 * after the begin. */
void embi_frame_set_target_offset(embi_frame *f, int32_t x, int32_t y, uint32_t tex_w, uint32_t tex_h);

/* Adds a 2x3 transform, CSS order (a, b, c, d, e, f) in local CSS px. Returns its index, which
 * is 0 for the identity. Consecutive equal transforms share one index. */
uint32_t embi_frame_xform(embi_frame *f, const float m[6]);

/* Adds a rounded-rect clip in its own space (xform, from embi_frame_xform, 0 = identity).
 * parent is an earlier clip index or 0. Returns the clip index (>= 1), or 0 on overflow. A
 * chain deeper than EMBI_MAX_CLIP_DEPTH is folded when the deeper clip is added (see the notes
 * in render.c): the content stays clipped exactly, except that when two outer levels cannot be
 * merged exactly, they become one axis-aligned clip that may leak at rounded corners. */
uint32_t embi_frame_clip(embi_frame *f, const float rect[4], const float radii[4], uint32_t xform,
                         uint32_t parent);

/* Adds a solid fill. rgba is RGBA8 non-premultiplied. clip is 0 for none. */
void embi_frame_fill(embi_frame *f, const float rect[4], const float radii[4], uint32_t rgba,
                     uint32_t xform, uint32_t clip);

/* Adds a solid-style border. widths and colors are in top, right, bottom, left order. The
 * outer edge is the rounded rect. */
void embi_frame_border(embi_frame *f, const float rect[4], const float radii[4],
                       const float widths[4], const uint32_t colors[4], uint32_t xform,
                       uint32_t clip);

/* A border with a CSS border-style per side (EMBI_BSTYLE_*; top, right, bottom, left order).
 * styles may be nullptr for all solid. The styles are resolved here, so the shader sees only
 * colors and patterns:
 *   dotted, dashed: round dots, or dashes, on the side's centerline (CSS Backgrounds 3 spacing,
 *     fitted to a whole number of dashes or dots per side);
 *   double: two solid lines a third of the width each, and solid below 3 px width;
 *   groove: each side split along its width into an outer and an inner half. The top and left
 *     sides are dark outside and the color inside; the bottom and right sides are the color
 *     outside and dark inside. ridge is the reverse of groove on every side;
 *   inset: top and left dark, bottom and right the color; outset: the reverse;
 *   none: the side paints nothing (its width is taken as zero).
 * "dark" is the shade Chromium uses: each channel is scaled by (v - 0.33) / v, where v is the
 * largest channel as a fraction of 1, so that red (255, 0, 0) gives (171, 0, 0). */
void embi_frame_border_styled(embi_frame *f, const float rect[4], const float radii[4],
                              const float widths[4], const uint32_t colors[4],
                              const uint32_t styles[4], uint32_t xform, uint32_t clip);

/* A box shadow (CSS Backgrounds 3 section 7.1), drawn analytically by the ubershader: a Gaussian blur of a
 * rounded rect in closed form (erf), or its AA edge when sigma is 0. quad is the area the prim covers (the
 * shadow shape grown by 3 sigma for an outer shadow, the padding box for an inset one). shape and shape_radii
 * are the rounded rect that is blurred, clip_rect and clip_radii the sharp rounded rect:
 *   outer: alpha = blur(shape) * (1 - coverage(clip))   (the shadow is not drawn under the box)
 *   inset: alpha = (1 - blur(shape)) * coverage(clip)   (shape is the hole, clip the padding box)
 * sigma is in local CSS px (the CSS blur radius divided by 2). A zero-size shape is empty. rgba is
 * RGBA8 non-premultiplied. */
void embi_frame_shadow(embi_frame *f, const float quad[4], const float shape[4],
                       const float shape_radii[4], const float clip_rect[4],
                       const float clip_radii[4], float sigma, bool inset, uint32_t rgba,
                       uint32_t xform, uint32_t clip);

/* Gradients. rect and radii are the box, as for a fill. The gradient geometry is in the same
 * local CSS px as the rect. Stop offsets are gradient parameters: for linear, the position along
 * the line from (x0, y0) to (x1, y1), where 0 is the start and 1 the end; for radial, the
 * distance from the center in units of (rx, ry), where 1 is the ellipse edge; for conic, the
 * fraction of a full turn from from_radians, clockwise from straight up. Colors are RGBA8
 * non-premultiplied, and they interpolate premultiplied.
 *
 * The emitters clean up the stops: each offset is clamped to be at least the one before it
 * (non-finite offsets take the previous one), and a stop with equal offsets makes a hard edge.
 * Outside the first and last offsets the gradient keeps the nearest stop's color. With repeat,
 * t wraps over the span from the first offset to the last one (no wrap when that span is zero).
 * A zero-length linear line, or a radial gradient with a zero radius, is painted with the last
 * stop's color, as CSS does. n == 0 draws nothing. The stops are copied, so the caller's array
 * may be reused at once. */
void embi_frame_linear(embi_frame *f, const float rect[4], const float radii[4], float x0, float y0,
                       float x1, float y1, const embi_stop *stops, uint32_t n, bool repeat,
                       uint32_t xform, uint32_t clip);
void embi_frame_radial(embi_frame *f, const float rect[4], const float radii[4], float cx, float cy,
                       float rx, float ry, const embi_stop *stops, uint32_t n, bool repeat,
                       uint32_t xform, uint32_t clip);
void embi_frame_conic(embi_frame *f, const float rect[4], const float radii[4], float cx, float cy,
                      float from_radians, const embi_stop *stops, uint32_t n, bool repeat,
                      uint32_t xform, uint32_t clip);

/* Like embi_frame_image (no radii), through a projective map instead of an xform: the point (x, y) of rect in local CSS
 * px goes to the device point (X / W, Y / W) of h, a row-major 3x3 acting on (x, y, 1) with X and Y in CSS px (the frame
 * multiplies them by the device scale). The map must keep W positive over the part of rect that is visible; the GPU
 * clips what lies behind W = 0. The edge of rect is not anti-aliased (draw layers whose edge is transparent). Used for
 * the planes of 3D transforms (R3c). */
void embi_frame_image_proj(embi_frame *f, const float rect[4], embi_atlas_region img, const float uv[4],
                           uint32_t tint_rgba, const float h[9], uint32_t clip);

/* Draws the part `uv` of an atlas image into rect. uv is {u0, v0, u1, v1} in image space, where
 * (0, 0) is the top-left of the image and (1, 1) the bottom-right. Pass {0, 0, 1, 1} for the whole
 * image. Sampling is linear and clamped to the edge. The sampled texel is multiplied by tint
 * (RGBA8 non-premultiplied, so white is the identity). img must come from embi_atlas_alloc and
 * have been uploaded. Images of different atlas pages share a batch (the pages are layers of one
 * array), while a solo image or a layer (a texture of its own) starts a new batch. */
void embi_frame_image(embi_frame *f, const float rect[4], const float radii[4],
                      embi_atlas_region img, const float uv[4], uint32_t tint_rgba, uint32_t xform,
                      uint32_t clip);

/* Like embi_frame_image. nearest selects nearest-texel sampling (image-rendering: pixelated and crisp-edges, CSS Images
 * 3 section 5.3); otherwise it samples linearly. A sub-rect uv never samples the texels outside it, either way. */
void embi_frame_image_ex(embi_frame *f, const float rect[4], const float radii[4], embi_atlas_region img,
                         const float uv[4], uint32_t tint_rgba, uint32_t xform, uint32_t clip, bool nearest);

/* Fill rule for outlines (paths and glyphs). */
typedef enum embi_fill_rule {
    EMBI_FILL_NONZERO = 0,
    EMBI_FILL_EVENODD = 1,
} embi_fill_rule;

/* A paint for an outline. kind is EMBI_PRIM_FILL for a solid color, or a gradient kind with
 * the geometry of the matching gradient call, in the outline's local CSS px: g = (x0, y0, x1, y1)
 * for EMBI_PRIM_LINEAR, (cx, cy, rx, ry) for EMBI_PRIM_RADIAL, (cx, cy, from_radians, unused) for
 * EMBI_PRIM_CONIC. The stops are copied, and repeat and the stop rules are those of the gradient
 * calls. */
typedef struct embi_paint {
    uint32_t kind;
    uint32_t color; /* solid: RGBA8 non-premultiplied */
    float g[4];
    const embi_stop *stops;
    uint32_t n_stops;
    bool repeat;
} embi_paint;

static inline embi_paint embi_paint_solid(uint32_t rgba)
{
    embi_paint p = {0};
    p.kind = EMBI_PRIM_FILL;
    p.color = rgba;
    return p;
}

/* Adds a filled outline given by closed quadratic curves in its own space (em for glyphs, path
 * units for paths; see gpu/slug.h). key names the outline for the blob cache: the same key must
 * always come with the same curves. Keys with bit 63 set belong to paths. to_local maps the
 * outline's space to local CSS px (a b c d e f, CSS order). Fills follow the rule of the outline,
 * and draw nothing when the outline cannot be built or its transform is degenerate. */
void embi_frame_fill_quads(embi_frame *f, uint64_t key, const embi_qcurve *curves, uint32_t n,
                           const float to_local[6], const embi_paint *paint, embi_fill_rule rule,
                           uint32_t xform, uint32_t clip);
/* embi_frame_fill_quads for an outline whose blob is cached under key, without its curves: returns false, recording
 * nothing, when the cache has no blob for key (the caller then makes the curves and calls embi_frame_fill_quads). */
bool embi_frame_fill_cached(embi_frame *f, uint64_t key, const float to_local[6], const embi_paint *paint,
                            embi_fill_rule rule, uint32_t xform, uint32_t clip);

/* Text antialiasing (W4h). GRAY is plain coverage. LCD_RGB and LCD_BGR sample three horizontal
 * positions per pixel and write each color channel from its own coverage (three draws of the text
 * prims with color write masks), so they need an opaque backdrop in the same target. */
typedef enum embi_text_aa {
    EMBI_TEXT_AA_GRAY = 0,
    EMBI_TEXT_AA_LCD_RGB = 1,
    EMBI_TEXT_AA_LCD_BGR = 2,
} embi_text_aa;

/* Sets the frame's text antialiasing (default GRAY, set it after embi_frame_begin). transforms_
 * animating: a transform animation runs, so text under any transform other than the device scale is
 * drawn gray and unfitted. */
void embi_frame_set_text_aa(embi_frame *f, embi_text_aa mode, bool transforms_animating);
/* Marks a transform of the frame as a translation that no animation moves (a scroll offset): the text under it keeps its
 * grid fit and LCD while a transform animation runs elsewhere (embi_frame_set_text_aa). */
void embi_frame_xform_set_still(embi_frame *f, uint32_t xform);

/* Says whether everything text of this frame is drawn over is opaque in the same target (the
 * root target cleared to an opaque color; not a layer, whose backdrop is transparent). Default
 * false after embi_frame_begin: without it text is gray. */
void embi_frame_set_opaque_backdrop(embi_frame *f, bool opaque);
/* Per item switch: false draws the text recorded from now on gray (an item under opacity < 1).
 * Default true after embi_frame_begin. */
void embi_frame_set_lcd_allowed(embi_frame *f, bool allowed);

/* What text drawing needs to know about a transform of the frame. m is the map from local CSS px
 * to device px (device scale included). fit: the map is an axis-aligned positive scale plus a
 * translation, and no transform animation runs under it, so glyphs may be grid fitted. lcd: text
 * through it may be drawn with LCD antialiasing. Returns false for a bad xform. */
typedef struct embi_text_target {
    float m[6];
    bool fit;
    bool lcd;
} embi_text_target;
[[nodiscard]] bool embi_frame_text_target(const embi_frame *f, uint32_t xform,
                                          embi_text_target *out);

/* embi_frame_fill_quads for a glyph with a solid color: the text path. darken_px is the total
 * stem darkening in device px (0 for none, up to 0.49), applied as a boost of partial coverage (see
 * box.slang). lcd selects LCD antialiasing and is
 * honored only where embi_frame_text_target says lcd for the xform. The text contrast curve
 * applies. */
void embi_frame_fill_text(embi_frame *f, uint64_t key, const embi_qcurve *curves, uint32_t n,
                          const float to_local[6], uint32_t rgba, float darken_px, bool lcd,
                          uint32_t xform, uint32_t clip);

/* Uploads the frame and records its copy and render passes into cmd, targeting `target`.
 * clear == true clears to clear_rgba first (non-premultiplied, premultiplied here). Otherwise
 * the target contents are kept. For a region frame (embi_frame_begin_region) only the damage is
 * cleared, and the rest of the target is always kept. Ends the frame. A frame that exceeds a size
 * cap is logged and drawn with nothing in it (it is still cleared when clear is true). */
void embi_frame_end(embi_frame *f, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, bool clear,
                    uint32_t clear_rgba);

/* Blending of the prims recorded from now on (R4c, the composite of a mask layer). EMBI_BLEND_OVER is the premultiplied
 * source-over of every other prim. EMBI_BLEND_DSTIN multiplies what is in the target by the alpha of the prim (out = dst *
 * src.a for colors and alpha), so drawing a layer with it keeps the target where the layer is opaque and clears it where the
 * layer is transparent, over the layer's quad only. Reset to OVER at embi_frame_begin. */
typedef enum embi_blend {
    EMBI_BLEND_OVER = 0,
    EMBI_BLEND_DSTIN = 1,
} embi_blend;
void embi_frame_set_blend(embi_frame *f, embi_blend blend);

/* Number of draw calls recorded by the last embi_frame_end. */
uint32_t embi_renderer_last_draw_calls(const embi_renderer *r);
/* The batches and the prims the last drawn frame was made of. */
uint32_t embi_renderer_last_batches(const embi_renderer *r);
uint32_t embi_renderer_last_prims(const embi_renderer *r);
