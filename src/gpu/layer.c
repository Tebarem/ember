#include "gpu/layer.h"

#include "base/base.h"

#include "gpu/atlas.h"

#include <SDL3/SDL_timer.h>

/* Textures are 4 bytes per texel (RGBA8 or BGRA8, the only formats the renderer draws to). */
#define EMBI_LAYER_BPP 4u
#define EMBI_LAYER_MIN_CLASS 64u

typedef struct embi_layer_idle {
    SDL_GPUTexture *tex;
    uint32_t tw;
    uint32_t th;
    uint64_t released_ms; /* SDL_GetTicks when it was released */
} embi_layer_idle;

struct embi_layer_pool {
    SDL_GPUDevice *dev;
    SDL_GPUTextureFormat format;
    size_t budget;
    size_t owned;                   /* bytes of every texture the pool created and not freed */
    uint32_t live;                  /* textures acquired and not released */
    EMBI_VEC(embi_layer_idle) idle; /* released textures, oldest first */
};

static uint32_t embi_layer_class(uint32_t d)
{
    uint32_t c = EMBI_LAYER_MIN_CLASS;
    while (c < d) {
        c <<= 1; /* d <= EMBI_LAYER_MAX_SIDE, so c stays at or below it */
    }
    return c;
}

static size_t embi_layer_bytes(uint32_t w, uint32_t h)
{
    return (size_t)w * (size_t)h * EMBI_LAYER_BPP;
}

/* Frees the idle texture at index i and keeps the order of the rest. */
static void embi_layer_free_idle_at(embi_layer_pool *p, uint32_t i)
{
    const embi_layer_idle e = embi_vec_at(&p->idle, i);
    SDL_ReleaseGPUTexture(p->dev, e.tex); /* SDL keeps it alive until the GPU is done with it */
    p->owned -= embi_layer_bytes(e.tw, e.th);
    for (uint32_t k = i + 1; k < p->idle.len; k++) {
        p->idle.data[k - 1] = p->idle.data[k];
    }
    p->idle.len--;
}

embi_layer_pool *embi_layer_pool_create(embi_gpu *g, SDL_GPUTextureFormat format,
                                        size_t budget_bytes)
{
    if (g == nullptr || format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        return nullptr;
    }
    embi_layer_pool *p = embi_calloc(1, sizeof *p);
    p->dev = embi_gpu_device(g);
    p->format = format;
    p->budget = budget_bytes;
    return p;
}

void embi_layer_pool_destroy(embi_layer_pool *p)
{
    if (p == nullptr) {
        return;
    }
    if (p->live > 0) {
        embi_log_warn("layer: pool destroyed with %u layers still acquired", (unsigned)p->live);
    }
    while (p->idle.len > 0) {
        embi_layer_free_idle_at(p, p->idle.len - 1);
    }
    embi_vec_free(&p->idle);
    embi_free(p, sizeof *p);
}

bool embi_layer_acquire(embi_layer_pool *p, uint32_t w, uint32_t h, embi_layer *out)
{
    if (out == nullptr) {
        return false;
    }
    *out = (embi_layer){0};
    if (p == nullptr || w == 0 || h == 0 || w > EMBI_LAYER_MAX_SIDE || h > EMBI_LAYER_MAX_SIDE) {
        return false;
    }
    const uint32_t tw = embi_layer_class(w);
    const uint32_t th = embi_layer_class(h);

    SDL_GPUTexture *tex = nullptr;
    /* Reuse the most recently released texture of this class. */
    for (uint32_t i = p->idle.len; i-- > 0;) {
        const embi_layer_idle e = embi_vec_at(&p->idle, i);
        if (e.tw == tw && e.th == th) {
            tex = e.tex;
            for (uint32_t k = i + 1; k < p->idle.len; k++) {
                p->idle.data[k - 1] = p->idle.data[k];
            }
            p->idle.len--;
            break;
        }
    }
    if (tex == nullptr) {
        /* Make room under the budget by freeing the oldest idle textures. A budget smaller than
         * one texture, or only textures in use, leaves the pool over budget. */
        const size_t bytes = embi_layer_bytes(tw, th);
        while (p->owned + bytes > p->budget && p->idle.len > 0) {
            embi_layer_free_idle_at(p, 0);
        }
        /* A one-layer array, like every texture the renderer samples (see gpu/atlas.h). */
        tex = SDL_CreateGPUTexture(
            p->dev, &(SDL_GPUTextureCreateInfo){
                        .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
                        .format = p->format,
                        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                        .width = tw,
                        .height = th,
                        .layer_count_or_depth = 1,
                        .num_levels = 1,
                        .sample_count = SDL_GPU_SAMPLECOUNT_1,
                    });
        if (tex == nullptr) {
            embi_log_error("layer: SDL_CreateGPUTexture(%ux%u) failed: %s", (unsigned)tw,
                           (unsigned)th, SDL_GetError());
            return false;
        }
        p->owned += bytes;
    }
    out->tex = tex;
    out->w = w;
    out->h = h;
    out->tex_w = tw;
    out->tex_h = th;
    p->live++;
    return true;
}

void embi_layer_release(embi_layer_pool *p, embi_layer *l)
{
    if (l == nullptr) {
        return;
    }
    if (p == nullptr || l->tex == nullptr) {
        *l = (embi_layer){0};
        return;
    }
    const embi_layer_idle e = {.tex = l->tex, .tw = l->tex_w, .th = l->tex_h, .released_ms = SDL_GetTicks()};
    embi_vec_push(&p->idle, e);
    if (p->live > 0) {
        p->live--;
    }
    *l = (embi_layer){0};
    while (p->owned > p->budget && p->idle.len > 0) {
        embi_layer_free_idle_at(p, 0);
    }
}

void embi_layer_pool_trim(embi_layer_pool *p)
{
    if (p == nullptr) {
        return;
    }
    while (p->idle.len > 0) {
        embi_layer_free_idle_at(p, p->idle.len - 1);
    }
}

void embi_layer_pool_trim_unused(embi_layer_pool *p, uint64_t now_ms, uint64_t keep_ms)
{
    if (p == nullptr) {
        return;
    }
    /* The list is in release order, so the textures unused the longest come first. */
    while (p->idle.len > 0 && embi_vec_at(&p->idle, 0).released_ms + keep_ms <= now_ms) {
        embi_layer_free_idle_at(p, 0);
    }
}

size_t embi_layer_pool_bytes(const embi_layer_pool *p)
{
    return p == nullptr ? 0 : p->owned;
}

uint32_t embi_layer_pool_idle(const embi_layer_pool *p)
{
    return p == nullptr ? 0 : p->idle.len;
}

void embi_frame_layer_proj(embi_frame *f, const embi_layer *layer, const float src[4],
                           const float dst[4], float opacity, const float h[9], uint32_t clip)
{
    if (f == nullptr || layer == nullptr || layer->tex == nullptr || src == nullptr ||
        dst == nullptr || h == nullptr || layer->w == 0 || layer->h == 0 || !(opacity > 0.0f)) {
        return;
    }
    if (opacity > 1.0f) {
        opacity = 1.0f;
    }
    const float lw = (float)layer->w;
    const float lh = (float)layer->h;
    const float uv[4] = {src[0] / lw, src[1] / lh, (src[0] + src[2]) / lw, (src[1] + src[3]) / lh};
    const uint32_t alpha = (uint32_t)(opacity * 255.0f + 0.5f);
    const uint32_t tint = 0x00ffffffu | (alpha << 24);
    const embi_atlas_region img = {
        .tex = layer->tex,
        .page = 0,
        .x = 0,
        .y = 0,
        .w = (uint16_t)layer->w,
        .h = (uint16_t)layer->h,
        .page_w = (uint16_t)layer->tex_w,
        .page_h = (uint16_t)layer->tex_h,
    };
    embi_frame_image_proj(f, dst, img, uv, tint, h, clip);
}

void embi_frame_layer(embi_frame *f, const embi_layer *layer, const float src[4],
                      const float dst[4], const float radii[4], float opacity, uint32_t xform,
                      uint32_t clip)
{
    if (f == nullptr || layer == nullptr || layer->tex == nullptr || src == nullptr ||
        dst == nullptr || layer->w == 0 || layer->h == 0 || !(opacity > 0.0f)) {
        return;
    }
    if (opacity > 1.0f) {
        opacity = 1.0f;
    }
    /* The layer is an image of its own texture: uv runs over the content, 0..1 per side. */
    const float lw = (float)layer->w;
    const float lh = (float)layer->h;
    const float uv[4] = {src[0] / lw, src[1] / lh, (src[0] + src[2]) / lw, (src[1] + src[3]) / lh};
    const uint32_t alpha = (uint32_t)(opacity * 255.0f + 0.5f);
    const uint32_t tint = 0x00ffffffu | (alpha << 24); /* white: the texel is kept */
    const embi_atlas_region img = {
        .tex = layer->tex,
        .page = 0,
        .x = 0,
        .y = 0,
        .w = (uint16_t)layer->w,
        .h = (uint16_t)layer->h,
        .page_w = (uint16_t)layer->tex_w,
        .page_h = (uint16_t)layer->tex_h,
    };
    embi_frame_image_ex(f, dst, radii, img, uv, tint, xform, clip, layer->nearest);
}
