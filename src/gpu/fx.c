#include "gpu/fx.h"

#include "base/base.h"

#include <math.h>
#include <string.h>

#include "ember_shaders/fx.h"

/* Mirrors the cbuffer of shaders/fx.slang. */
typedef struct embi_fx_params {
    float mode[4];
    float dir[4];
    float src[4];
    float off[4];
    float m[4][4];
    float bias[4];
    float color[4];
} embi_fx_params;

enum {
    FX_BLUR = 0,
    FX_SCALE = 1,
    FX_MATRIX = 2,
    FX_SHADOW = 3,
    FX_BLEND = 4,
    FX_MASK_APPLY = 5,
    FX_MASK_LUM = 6,
    FX_MASK_COMBINE = 7,
    FX_PLUS = 8,
};

#define FX_MAX_TAPS 32

struct embi_fx {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUSampler *sampler;
    embi_layer_pool *pool;
    uint32_t passes;
};

embi_fx *embi_fx_create(embi_gpu *g, SDL_GPUTextureFormat format, embi_layer_pool *pool)
{
    if (g == nullptr || pool == nullptr) {
        return nullptr;
    }
    embi_fx *fx = embi_calloc(1, sizeof *fx);
    fx->g = g;
    fx->dev = embi_gpu_device(g);
    fx->pool = pool;
    fx->pipe = embi_gpu_make_graphics(g, &embi_shader_fx, format);
    fx->sampler = SDL_CreateGPUSampler(
        fx->dev, &(SDL_GPUSamplerCreateInfo){
                     .min_filter = SDL_GPU_FILTER_LINEAR,
                     .mag_filter = SDL_GPU_FILTER_LINEAR,
                     .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                     .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                     .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                     .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                 });
    if (fx->pipe == nullptr || fx->sampler == nullptr) {
        embi_log_error("fx: pipeline or sampler could not be created: %s", SDL_GetError());
        embi_fx_destroy(fx);
        return nullptr;
    }
    return fx;
}

void embi_fx_destroy(embi_fx *fx)
{
    if (fx == nullptr) {
        return;
    }
    SDL_WaitForGPUIdle(fx->dev);
    if (fx->pipe != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(fx->dev, fx->pipe);
    }
    if (fx->sampler != nullptr) {
        SDL_ReleaseGPUSampler(fx->dev, fx->sampler);
    }
    embi_free(fx, sizeof *fx);
}

uint32_t embi_fx_last_passes(const embi_fx *fx)
{
    return fx == nullptr ? 0 : fx->passes;
}

/* One pass: writes the content rect of dst (the rest of its texture is cleared to transparent) from src0 and src1. */
static bool fx_pass(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src0, const embi_layer *src1,
                    const embi_layer *dst, embi_fx_params *u)
{
    const SDL_GPUColorTargetInfo color = {
        .texture = dst->tex,
        .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    if (pass == nullptr) {
        embi_log_error("fx: SDL_BeginGPURenderPass failed: %s", SDL_GetError());
        return false;
    }
    const SDL_GPUViewport vp = {0.0f, 0.0f, (float)dst->tex_w, (float)dst->tex_h, 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &vp);
    const SDL_Rect sc = {0, 0, (int)dst->w, (int)dst->h};
    SDL_SetGPUScissor(pass, &sc);
    SDL_BindGPUGraphicsPipeline(pass, fx->pipe);
    const SDL_GPUTextureSamplerBinding ts[2] = {
        {.texture = src0->tex, .sampler = fx->sampler},
        {.texture = (src1 != nullptr ? src1 : src0)->tex, .sampler = fx->sampler},
    };
    SDL_BindGPUFragmentSamplers(pass, 0, ts, 2);
    u->src[0] = (float)src0->w;
    u->src[1] = (float)src0->h;
    u->src[2] = (float)src0->tex_w;
    u->src[3] = (float)src0->tex_h;
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof *u);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    fx->passes++;
    return true;
}

static bool fx_valid(const embi_layer *a)
{
    return a != nullptr && a->tex != nullptr && a->w > 0 && a->h > 0;
}

static bool fx_same_size(const embi_layer *a, const embi_layer *b)
{
    return fx_valid(a) && fx_valid(b) && a->w == b->w && a->h == b->h;
}

/* A 1-D Gaussian pass of sigma texels along (dx, dy). */
static bool fx_gauss(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst, float sigma,
                     float dx, float dy, bool clamp)
{
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    int radius = (int)ceilf(3.0f * sigma);
    if (radius < 1) {
        radius = 1;
    }
    if (radius > FX_MAX_TAPS) {
        radius = FX_MAX_TAPS;
    }
    u.mode[0] = (float)FX_BLUR;
    u.mode[1] = (float)radius;
    u.mode[2] = sigma;
    u.mode[3] = clamp ? 1.0f : 0.0f;
    u.dir[0] = dx;
    u.dir[1] = dy;
    return fx_pass(fx, cmd, src, nullptr, dst, &u);
}

/* dst (its own content size) = src read `step` source texels per destination texel, with bilinear filtering. */
static bool fx_scale(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst, float step,
                     bool clamp)
{
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_SCALE;
    u.dir[0] = step;
    u.dir[1] = step;
    u.mode[3] = clamp ? 1.0f : 0.0f;
    return fx_pass(fx, cmd, src, nullptr, dst, &u);
}

bool embi_fx_blur(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst, float sigma)
{
    return embi_fx_blur_edge(fx, cmd, src, dst, sigma, false);
}

bool embi_fx_blur_edge(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst, float sigma,
                       bool clamp)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || src->tex == dst->tex) {
        return false;
    }
    if (!(sigma > 0.0f)) {
        return fx_scale(fx, cmd, src, dst, 1.0f, clamp);
    }
    if (sigma > EMBI_FX_SIGMA_MAX) {
        sigma = EMBI_FX_SIGMA_MAX;
    }
    if (sigma <= EMBI_FX_SIGMA_DIRECT) {
        embi_layer t;
        if (!embi_layer_acquire(fx->pool, src->w, src->h, &t)) {
            return false;
        }
        bool ok = fx_gauss(fx, cmd, src, &t, sigma, 1.0f, 0.0f, clamp) && fx_gauss(fx, cmd, &t, dst, sigma, 0.0f, 1.0f, clamp);
        embi_layer_release(fx->pool, &t);
        return ok;
    }
    /* Downsample by 2^k so that sigma / 2^k is at most the direct sigma. */
    uint32_t k = 0;
    float s = sigma;
    while (s > EMBI_FX_SIGMA_DIRECT) {
        s *= 0.5f;
        k++;
    }
    embi_layer cur = *src;
    bool own = false; /* cur is a pool layer made here */
    bool ok = true;
    for (uint32_t i = 0; i < k && ok; i++) {
        embi_layer n;
        uint32_t nw = (cur.w + 1u) / 2u;
        uint32_t nh = (cur.h + 1u) / 2u;
        if (!embi_layer_acquire(fx->pool, nw, nh, &n)) {
            ok = false;
            break;
        }
        ok = fx_scale(fx, cmd, &cur, &n, 2.0f, clamp);
        if (own) {
            embi_layer_release(fx->pool, &cur);
        }
        cur = n;
        own = true;
    }
    if (ok) {
        embi_layer t;
        if (embi_layer_acquire(fx->pool, cur.w, cur.h, &t)) {
            ok = fx_gauss(fx, cmd, &cur, &t, s, 1.0f, 0.0f, clamp) && fx_gauss(fx, cmd, &t, &cur, s, 0.0f, 1.0f, clamp);
            embi_layer_release(fx->pool, &t);
        } else {
            ok = false;
        }
    }
    if (ok) {
        /* Up to the full size in one bilinear pass: dst texel -> source (texel + 0.5) / 2^k. */
        ok = fx_scale(fx, cmd, &cur, dst, 1.0f / (float)(1u << k), clamp);
    }
    if (own) {
        embi_layer_release(fx->pool, &cur);
    }
    return ok;
}

bool embi_fx_matrix(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst, const float m[20])
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || src->tex == dst->tex || m == nullptr) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_MATRIX;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            u.m[r][c] = m[5 * r + c];
        }
        u.bias[r] = m[5 * r + 4];
    }
    return fx_pass(fx, cmd, src, nullptr, dst, &u);
}

bool embi_fx_drop_shadow(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *blurred,
                         const embi_layer *dst, float dx, float dy, uint32_t rgba)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || !fx_same_size(src, blurred) || dst->tex == src->tex || dst->tex == blurred->tex) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_SHADOW;
    u.off[0] = dx;
    u.off[1] = dy;
    u.color[0] = (float)(rgba & 255u) / 255.0f;
    u.color[1] = (float)((rgba >> 8) & 255u) / 255.0f;
    u.color[2] = (float)((rgba >> 16) & 255u) / 255.0f;
    u.color[3] = (float)((rgba >> 24) & 255u) / 255.0f;
    return fx_pass(fx, cmd, src, blurred, dst, &u);
}

bool embi_fx_blend(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *backdrop,
                   const embi_layer *dst, uint32_t mode)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || !fx_same_size(src, backdrop) || dst->tex == src->tex || dst->tex == backdrop->tex ||
        mode > EMBI_FX_BLEND_MAX) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_BLEND;
    u.off[0] = (float)mode;
    return fx_pass(fx, cmd, src, backdrop, dst, &u);
}

bool embi_fx_mask_apply(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *mask,
                        const embi_layer *dst)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || !fx_same_size(src, mask) || dst->tex == src->tex || dst->tex == mask->tex) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_MASK_APPLY;
    return fx_pass(fx, cmd, src, mask, dst, &u);
}

bool embi_fx_mask_luminance(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *dst)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || src->tex == dst->tex) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_MASK_LUM;
    return fx_pass(fx, cmd, src, nullptr, dst, &u);
}

bool embi_fx_mask_combine(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *top, const embi_layer *below,
                          const embi_layer *dst, uint32_t op)
{
    fx->passes = 0;
    if (!fx_same_size(top, dst) || !fx_same_size(top, below) || dst->tex == top->tex || dst->tex == below->tex ||
        op > EMBI_FX_MASK_EXCLUDE) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_MASK_COMBINE;
    u.off[0] = (float)op;
    return fx_pass(fx, cmd, top, below, dst, &u);
}

bool embi_fx_plus(embi_fx *fx, SDL_GPUCommandBuffer *cmd, const embi_layer *src, const embi_layer *backdrop,
                  const embi_layer *dst, float opacity)
{
    fx->passes = 0;
    if (!fx_same_size(src, dst) || !fx_same_size(src, backdrop) || dst->tex == src->tex || dst->tex == backdrop->tex) {
        return false;
    }
    embi_fx_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = (float)FX_PLUS;
    u.off[1] = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    return fx_pass(fx, cmd, src, backdrop, dst, &u);
}
