#pragma once
/*
 * Filter effect passes on layers (R3a): Gaussian blur, color matrix and the drop-shadow
 * composite. Every pass is one fragment pass (shaders/fx.slang) that reads layer textures and writes the
 * content rect of another layer. Layers hold premultiplied colors (gpu/layer.h). The passes are recorded
 * into a command buffer, after the frames that wrote the source layers and before the frames that sample the
 * result.
 *
 *     embi_fx *fx = embi_fx_create(g, fmt, pool);
 *     embi_fx_blur(fx, cmd, &src, &dst, sigma_px);      // dst: acquired with the same w x h as src
 *
 * Blur. The kernel is a truncated Gaussian (radius 3 sigma), separable in two passes, normalized over the whole
 * kernel with transparent texels outside the source content (the edge fades out). Sigma above
 * EMBI_FX_SIGMA_DIRECT is first reduced: the source is box-downsampled by 2^k (k passes of a 2x2 average) so that
 * sigma / 2^k is at most EMBI_FX_SIGMA_DIRECT, blurred there, and upsampled with bilinear filtering. The cost per
 * pixel is therefore bounded by the kernel of a sigma of EMBI_FX_SIGMA_DIRECT, plus 4/3 of a pass for the
 * pyramid, however large the radius is.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#include "gpu/gpu.h"
#include "gpu/layer.h"

/* The largest sigma (in layer px) that one separable pass pair blurs directly. */
#define EMBI_FX_SIGMA_DIRECT 8.0f
/* Sigmas above this are clamped (the result is a nearly flat tint anyway). */
#define EMBI_FX_SIGMA_MAX 2048.0f

typedef struct embi_fx embi_fx;

/* format is the format of the layers (the renderer's). pool supplies the temporary layers of the blur. */
[[nodiscard]] embi_fx *embi_fx_create(embi_gpu *g, SDL_GPUTextureFormat format, embi_layer_pool *pool);
void embi_fx_destroy(embi_fx *fx);

/* dst = blur(src) with a Gaussian of standard deviation sigma layer px. dst must be a different layer of the
 * same content size as src. Returns false when a pass or a temporary layer could not be made (dst is then
 * unspecified). sigma <= 0 copies. */
[[nodiscard]] bool embi_fx_blur(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst,
                                float sigma);

/* Same, with the edge mode of backdrop-filter when clamp is true: texels outside the content repeat the edge texel
 * instead of being transparent. */
[[nodiscard]] bool embi_fx_blur_edge(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src,
                                     const embi_layer *dst, float sigma, bool clamp);

/* dst = clamp(m * (r, g, b, a) + bias) on unpremultiplied colors, premultiplied again. m is 20 floats in the
 * order of the feColorMatrix values attribute: m[5 * row + col] for the 4x4 part (rows give r, g, b, a) and
 * m[5 * row + 4] the added bias, all in 0..1 units. dst differs from src, same size. */
[[nodiscard]] bool embi_fx_matrix(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src,
                                  const embi_layer *dst, const float m[20]);

/* dst = src over (the alpha of `blurred`, shifted by (dx, dy) layer px, tinted with rgba (RGBA8 non-premultiplied,
 * r in the low byte)). `blurred` is the source blurred by the caller (embi_fx_blur), all three layers the same
 * content size, dst different from both. */
[[nodiscard]] bool embi_fx_drop_shadow(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src,
                                       const embi_layer *blurred, const embi_layer *dst, float dx, float dy,
                                       uint32_t rgba);

/* Blend modes of embi_fx_blend, numbered as the mix-blend-mode keywords (R3b): 0 normal, 1 multiply, 2 screen, 3 overlay,
 * 4 darken, 5 lighten, 6 color-dodge, 7 color-burn, 8 hard-light, 9 soft-light, 10 difference, 11 exclusion, 12 hue,
 * 13 saturation, 14 color, 15 luminosity, 16 plus-lighter, 17 plus-darker. plus-lighter and plus-darker are run through the
 * general formula with the separable functions min(1, cb + cs) and max(0, cb + cs - 1). Plus-lighter is the exact
 * additive composite for an opaque backdrop; plus-darker is only this approximation, not the exact plus-darker of
 * Chromium and WebKit (an additive composite that darkens). */
#define EMBI_FX_BLEND_MAX 17u

/* dst = `src` blended onto `backdrop` (the compositing spec's formula, Cs' = (1 - ab) Cs + ab B(Cb, Cs)), premultiplied
 * by the alpha of src and with that alpha, so that drawing dst with source-over onto the backdrop gives the blended
 * composite. src, backdrop and dst have the same content size; backdrop is a copy of the target under src. */
[[nodiscard]] bool embi_fx_blend(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *backdrop,
                                 const embi_layer *dst, uint32_t mode);

/* dst = min(1, src * opacity + backdrop) on the premultiplied channels, alpha included (mix-blend-mode: plus-lighter): the
 * sum replaces the backdrop, so the caller copies dst over the target instead of compositing it. Same sizes as embi_fx_blend. */
[[nodiscard]] bool embi_fx_plus(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *backdrop,
                                const embi_layer *dst, float opacity);

/* Masks (R3b). A mask layer holds its value in the alpha channel. dst = src scaled (all four premultiplied channels) by
 * the alpha of mask. */
[[nodiscard]] bool embi_fx_mask_apply(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *mask,
                                      const embi_layer *dst);
/* dst: alpha = the luminance (0.2125 r + 0.7154 g + 0.0721 b) of the premultiplied src, rgb zero. */
[[nodiscard]] bool embi_fx_mask_luminance(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src,
                                          const embi_layer *dst);
/* mask-composite operators of embi_fx_mask_combine. */
enum {
    EMBI_FX_MASK_ADD = 0,
    EMBI_FX_MASK_SUBTRACT = 1,
    EMBI_FX_MASK_INTERSECT = 2,
    EMBI_FX_MASK_EXCLUDE = 3,
};
/* dst (alpha) = the alpha of `top` composited with the alpha of `below` by op (source = top, destination = below). */
[[nodiscard]] bool embi_fx_mask_combine(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *top,
                                        const embi_layer *below, const embi_layer *dst, uint32_t op);

/* The number of render passes the last call recorded (for tests and stats). */
[[nodiscard]] uint32_t embi_fx_last_passes(const embi_fx *fx);
