#pragma once
/*
 * Layers: intermediate render targets, the pool that recycles them, and the composite that draws
 * one into a frame.
 *
 *     embi_layer_pool *pool = embi_layer_pool_create(g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, budget);
 *     embi_layer L;
 *     embi_layer_acquire(pool, w, h, &L);                         // a texture at least w x h
 *     embi_frame *lf = embi_frame_begin(r, L.tex_w, L.tex_h, s);  // the whole texture
 *     ...record the layer's content...
 *     embi_frame_end(lf, cmd, L.tex, true, 0);
 *     embi_frame_layer(f, &L, src, dst, radii, opacity, xform, clip);   // composite
 *     embi_layer_release(pool, &L);                               // once nothing draws it
 *
 * Size classes. Each side of a texture is a power of two, at least 64 px (so a layer's texture can
 * be up to 2x too large per side). A released texture is reused by any layer of the same class.
 * The layer content is the top-left w x h of its texture. Frames that render a layer use the
 * texture size, so the viewport maps the whole texture. Layers hold premultiplied colors, like
 * the target.
 *
 * Budget. The pool owns every texture it creates. A released texture waits in an idle list, and
 * whenever the pool owns more bytes than its budget the oldest idle textures are freed. A texture
 * in use is never freed, so an acquire may exceed the budget when everything is in use.
 *
 * Keeping. Creating a texture costs about a millisecond for a view-sized layer (the driver
 * allocates and clears it), so the idle textures are kept from frame to frame, and
 * embi_layer_pool_trim_unused frees the ones that have not been used for a while (a page that
 * stops changing gives its layers back).
 *
 * Composite. embi_frame_layer draws the layer as an image of its own texture, so it batches with
 * the other image prims (a layer on a different texture starts a new batch). Its opacity is the
 * tint, quantized to 1/255.
 */

#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#include "gpu/gpu.h"
#include "gpu/render.h"

/* The largest content side a layer can have (a size class of this value fits uint16 fields). */
#define EMBI_LAYER_MAX_SIDE 16384u

typedef struct embi_layer_pool embi_layer_pool;

typedef struct embi_layer {
    SDL_GPUTexture *tex; /* nullptr when not acquired */
    uint32_t w;          /* content size in px: the top-left w x h of the texture */
    uint32_t h;
    uint32_t tex_w;      /* texture size (a size class, a power of two per side) */
    uint32_t tex_h;
    bool nearest;        /* sample with nearest texels (a canvas drawn with image-rendering: pixelated) */
} embi_layer;

/* format is the render target format the layers are drawn with (the one the renderer uses).
 * budget_bytes caps the bytes of idle textures the pool keeps (see the header comment). */
[[nodiscard]] embi_layer_pool *embi_layer_pool_create(embi_gpu *g, SDL_GPUTextureFormat format,
                                                      size_t budget_bytes);
/* Frees the idle textures. Textures still acquired are left to the device (logged). */
void embi_layer_pool_destroy(embi_layer_pool *p);

/* Acquires a layer of w x h content. Returns false for an empty or oversized size, or when the
 * GPU cannot create the texture. *out is cleared in every case. */
[[nodiscard]] bool embi_layer_acquire(embi_layer_pool *p, uint32_t w, uint32_t h,
                                      embi_layer *out);
/* Gives a layer back. *l is cleared. The texture may be reused at once, so no frame may still
 * sample it in a way that matters after a later frame writes it (a frame recorded earlier in
 * the same command buffer is fine: the GPU runs the work in order). */
void embi_layer_release(embi_layer_pool *p, embi_layer *l);
/* Frees every idle texture. */
void embi_layer_pool_trim(embi_layer_pool *p);
/* Frees the idle textures released keep_ms or more before now_ms (SDL_GetTicks time). */
void embi_layer_pool_trim_unused(embi_layer_pool *p, uint64_t now_ms, uint64_t keep_ms);
/* Bytes of every texture the pool owns (idle and in use). */
[[nodiscard]] size_t embi_layer_pool_bytes(const embi_layer_pool *p);
/* Number of idle textures. */
[[nodiscard]] uint32_t embi_layer_pool_idle(const embi_layer_pool *p);

/* Draws layer into the frame. src is the part of the layer's content, in its own px (x, y, w,
 * h), and dst the rect it fills, in local CSS px, under xform and clip, as for a fill. radii
 * rounds dst. opacity multiplies the premultiplied layer (1 = opaque, 0 draws nothing). With the
 * identity transform, a device scale of 1 and dst at an integer position the texels map one to
 * one. */
void embi_frame_layer(embi_frame *f, const embi_layer *layer, const float src[4],
                      const float dst[4], const float radii[4], float opacity, uint32_t xform,
                      uint32_t clip);
/* Like embi_frame_layer, through the projective map h of embi_frame_image_proj instead of a transform (the planes
 * of 3D transforms, R3c). */
void embi_frame_layer_proj(embi_frame *f, const embi_layer *layer, const float src[4],
                           const float dst[4], float opacity, const float h[9], uint32_t clip);
