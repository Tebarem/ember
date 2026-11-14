/*
 * Canvas replay on the GPU (R4b): draws the recorded commands of a canvas into its backing texture.
 *
 * Two paths. The fast path draws straight into the backing texture with the renderer's frames: source-over fills, strokes,
 * text and whole images with a solid paint or an image, no shadow and no clip path, one frame for a run of such commands.
 * Everything else takes the layer path: the shape is rendered into a scratch layer (its coverage, or its color for a solid
 * paint), a paint pass turns the coverage into a gradient or pattern, a shadow is the blurred alpha (R3a's blur) tinted and
 * offset, and a composite pass (shaders/canvas.slang) reads the layer, a copy of the backdrop and the clip mask and writes
 * the result for the operator of globalCompositeOperation. A clip path is a mask layer, the product of the coverage of the
 * paths of its chain.
 *
 * The backing texture has the renderer's format and holds premultiplied colors. Gradients take at most 32 stops (the rest
 * are dropped).
 */
#include "canvas/canvas_int.h"

#include "gpu/fx.h"
#include "gpu/gpu.h"
#include "gpu/layer.h"
#include "gpu/path.h"
#include "gpu/stroke.h"
#include "gpu/render.h"

#include "ember_shaders/canvas.h"

#include <string.h>

#include <SDL3/SDL_timer.h>

#define CG_MAX_STOPS 32
#define CG_MAX_SIDE CV_MAX_SIDE

/* Mirrors the cbuffer of shaders/canvas.slang. */
typedef struct cgpu_params {
    float mode[4];
    float sz[4];
    float sz2[4];
    float a[4];
    float color[4];
    float inv0[4];
    float inv1[4];
    float geom0[4];
    float geom1[4];
    float src[4];
    float stop_off[CG_MAX_STOPS];
    float stop_col[CG_MAX_STOPS][4];
} cgpu_params;

/* A texture of a canvas that went away, kept for the next canvas of its size (see embi_canvas_gpu_trim). */
typedef struct cg_spare {
    SDL_GPUTexture *tex;
    uint32_t w, h;
    uint64_t since_ms; /* SDL_GetTicks when it was put aside */
} cg_spare;

#define CG_SPARE_MAX 8u                       /* spare textures kept at most */
#define CG_SPARE_BYTES ((size_t)32 << 20)     /* and their bytes at most */
#define CG_SPARE_KEEP_MS 2000u                /* how long one is kept unused */

struct embi_canvas_gpu {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    embi_renderer *r;
    embi_layer_pool *pool;
    SDL_GPUTextureFormat fmt;
    bool swap_rb; /* the storage order is BGRA */
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUSampler *samp;
    embi_fx *fx;
    bool fx_failed;
    bool pipe_failed;
    embi_canvas *head;
    embi_canvas *all;
    size_t bytes;
    EMBI_VEC(embi_cimage *) images;
    EMBI_VEC(cg_spare) spare; /* oldest first */
    size_t spare_bytes;
};

static void spare_free_at(embi_canvas_gpu *g, uint32_t i)
{
    const cg_spare sp = g->spare.data[i];
    SDL_ReleaseGPUTexture(g->dev, sp.tex); /* SDL keeps it alive until the GPU is done with it */
    g->bytes -= (size_t)sp.w * sp.h * 4;
    g->spare_bytes -= (size_t)sp.w * sp.h * 4;
    for (uint32_t k = i + 1; k < g->spare.len; k++) {
        g->spare.data[k - 1] = g->spare.data[k];
    }
    g->spare.len--;
}

/* Keeps tex (a canvas texture of w x h nothing draws into any more) for a later canvas of that size. */
static void spare_put(embi_canvas_gpu *g, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    const size_t bytes = (size_t)w * h * 4;
    if (bytes > CG_SPARE_BYTES) {
        SDL_ReleaseGPUTexture(g->dev, tex);
        g->bytes -= bytes;
        return;
    }
    while (g->spare.len > 0 && (g->spare.len >= CG_SPARE_MAX || g->spare_bytes + bytes > CG_SPARE_BYTES)) {
        spare_free_at(g, 0);
    }
    const cg_spare sp = {tex, w, h, SDL_GetTicks()};
    embi_vec_push(&g->spare, sp);
    g->spare_bytes += bytes;
}

/* A spare texture of w x h, taken out of the spares; nullptr when there is none. */
static SDL_GPUTexture *spare_take(embi_canvas_gpu *g, uint32_t w, uint32_t h)
{
    for (uint32_t i = g->spare.len; i-- > 0;) {
        const cg_spare sp = g->spare.data[i];
        if (sp.w == w && sp.h == h) {
            for (uint32_t k = i + 1; k < g->spare.len; k++) {
                g->spare.data[k - 1] = g->spare.data[k];
            }
            g->spare.len--;
            g->spare_bytes -= (size_t)w * h * 4;
            return sp.tex;
        }
    }
    return nullptr;
}

void embi_canvas_gpu_trim(embi_canvas_gpu *g, uint64_t now_ms)
{
    while (g != nullptr && g->spare.len > 0 && g->spare.data[0].since_ms + CG_SPARE_KEEP_MS <= now_ms) {
        spare_free_at(g, 0);
    }
}

embi_canvas_gpu *embi_canvas_gpu_create(void *gpu, void *renderer, void *pool, int format)
{
    if (gpu == nullptr || renderer == nullptr || pool == nullptr) {
        return nullptr;
    }
    embi_canvas_gpu *g = embi_calloc(1, sizeof *g);
    g->g = gpu;
    g->dev = embi_gpu_device(g->g);
    g->r = renderer;
    g->pool = pool;
    g->fmt = (SDL_GPUTextureFormat)format;
    g->swap_rb = g->fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || g->fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    return g;
}

static void images_release(embi_canvas_gpu *g)
{
    for (uint32_t i = 0; i < g->images.len; i++) {
        embi_cimage *im = g->images.data[i];
        if (im->tex != nullptr) {
            SDL_ReleaseGPUTexture(g->dev, im->tex);
            g->bytes -= (size_t)im->w * im->h * 4;
        }
        im->tex = nullptr;
        im->tex_owner = nullptr;
    }
    g->images.len = 0;
}

void embi_canvas_gpu_destroy(embi_canvas_gpu *g)
{
    if (g == nullptr) {
        return;
    }
    while (g->all != nullptr) {
        embi_canvas *cv = g->all;
        cgpu_unregister(cv);
        cv->env.gpu = nullptr; /* the canvas outlives the manager as a context with no pixels */
    }
    while (g->head != nullptr) {
        cgpu_detach(g->head);
    }
    while (g->spare.len > 0) {
        spare_free_at(g, g->spare.len - 1);
    }
    embi_vec_free(&g->spare);
    images_release(g);
    embi_vec_free(&g->images);
    SDL_WaitForGPUIdle(g->dev);
    if (g->pipe != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(g->dev, g->pipe);
    }
    if (g->samp != nullptr) {
        SDL_ReleaseGPUSampler(g->dev, g->samp);
    }
    if (g->fx != nullptr) {
        embi_fx_destroy(g->fx);
    }
    embi_free(g, sizeof *g);
}

size_t embi_canvas_gpu_bytes(const embi_canvas_gpu *g)
{
    return g->bytes;
}

void cgpu_register(embi_canvas *cv)
{
    embi_canvas_gpu *g = cv->env.gpu;
    if (g == nullptr || cv->registered) {
        return;
    }
    cv->aprev = nullptr;
    cv->anext = g->all;
    if (g->all != nullptr) {
        g->all->aprev = cv;
    }
    g->all = cv;
    cv->registered = true;
    cgpu_attach(cv);
}

void cgpu_unregister(embi_canvas *cv)
{
    cgpu_detach(cv);
    embi_canvas_gpu *g = cv->env.gpu;
    if (!cv->registered || g == nullptr) {
        return;
    }
    if (cv->aprev != nullptr) {
        cv->aprev->anext = cv->anext;
    } else {
        g->all = cv->anext;
    }
    if (cv->anext != nullptr) {
        cv->anext->aprev = cv->aprev;
    }
    cv->anext = cv->aprev = nullptr;
    cv->registered = false;
}

void cgpu_attach(embi_canvas *cv)
{
    embi_canvas_gpu *g = cv->env.gpu;
    if (g == nullptr || cv->listed) {
        return;
    }
    cv->gpu = g;
    cv->gprev = nullptr;
    cv->gnext = g->head;
    if (g->head != nullptr) {
        g->head->gprev = cv;
    }
    g->head = cv;
    cv->listed = true;
}

void cgpu_detach(embi_canvas *cv)
{
    embi_canvas_gpu *g = cv->gpu;
    if (g == nullptr) {
        return;
    }
    if (cv->tex != nullptr) {
        spare_put(g, cv->tex, cv->tex_w, cv->tex_h); /* the bytes stay counted while it is a spare */
        cv->tex = nullptr;
    }
    if (cv->listed) {
        if (cv->gprev != nullptr) {
            cv->gprev->gnext = cv->gnext;
        } else {
            g->head = cv->gnext;
        }
        if (cv->gnext != nullptr) {
            cv->gnext->gprev = cv->gprev;
        }
        cv->listed = false;
    }
    cv->gnext = cv->gprev = nullptr;
    cv->gpu = nullptr;
}

void cgpu_release_image(embi_cimage *im)
{
    embi_canvas_gpu *g = im->tex_owner;
    if (g == nullptr || im->tex == nullptr) {
        return;
    }
    SDL_ReleaseGPUTexture(g->dev, im->tex);
    g->bytes -= (size_t)im->w * im->h * 4;
    im->tex = nullptr;
    im->tex_owner = nullptr;
    for (uint32_t i = 0; i < g->images.len; i++) {
        if (g->images.data[i] == im) {
            g->images.data[i] = g->images.data[--g->images.len];
            break;
        }
    }
}

void *embi_canvas_gpu_texture(embi_canvas_gpu *g, const void *key, uint32_t *w, uint32_t *h)
{
    embi_canvas *cv = embi_canvas_find(key);
    if (cv == nullptr || cv->gpu != g || cv->tex == nullptr) {
        return nullptr;
    }
    *w = cv->w;
    *h = cv->h;
    return cv->tex;
}

/* ------------------------------------------------------------------ textures */

static SDL_GPUTexture *make_tex(embi_canvas_gpu *g, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0 || w > CG_MAX_SIDE || h > CG_MAX_SIDE) {
        return nullptr;
    }
    SDL_GPUTexture *spare = spare_take(g, w, h);
    if (spare != nullptr) {
        return spare; /* its bytes are counted already; the caller clears it */
    }
    SDL_GPUTexture *t = SDL_CreateGPUTexture(
        g->dev, &(SDL_GPUTextureCreateInfo){
                    .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
                    .format = g->fmt,
                    .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                    .width = w,
                    .height = h,
                    .layer_count_or_depth = 1,
                    .num_levels = 1,
                    .sample_count = SDL_GPU_SAMPLECOUNT_1,
                });
    if (t != nullptr) {
        g->bytes += (size_t)w * h * 4;
    }
    return t;
}

static void clear_tex(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, float r, float gr, float b, float a)
{
    const SDL_GPUColorTargetInfo ci = {
        .texture = t,
        .clear_color = {r, gr, b, a},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
    if (pass != nullptr) {
        SDL_EndGPURenderPass(pass);
    }
}

static bool ensure_tex(embi_canvas *cv, SDL_GPUCommandBuffer *cmd)
{
    embi_canvas_gpu *g = cv->gpu;
    if (cv->tex == nullptr) {
        cv->tex = make_tex(g, cv->w, cv->h);
        cv->tex_w = cv->w;
        cv->tex_h = cv->h;
        if (cv->tex == nullptr) {
            return false;
        }
        cv->need_clear = true;
    }
    if (cv->need_clear) {
        clear_tex(cmd, cv->tex, 0, 0, 0, cv->alpha ? 0.0f : 1.0f);
        cv->need_clear = false;
    }
    return true;
}

/* Uploads straight RGBA8 pixels (w x h) premultiplied into region (x, y) of texture t. */
static bool upload_pixels(embi_canvas_gpu *g, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, const uint8_t *px, uint32_t w,
                          uint32_t h, uint32_t x, uint32_t y, bool force_opaque)
{
    size_t bytes = (size_t)w * h * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(
        g->dev, &(SDL_GPUTransferBufferCreateInfo){.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)bytes});
    if (tb == nullptr) {
        return false;
    }
    uint8_t *dst = SDL_MapGPUTransferBuffer(g->dev, tb, false);
    if (dst == nullptr) {
        SDL_ReleaseGPUTransferBuffer(g->dev, tb);
        return false;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t r = px[4 * i], gg = px[4 * i + 1], b = px[4 * i + 2], a = force_opaque ? 255u : px[4 * i + 3];
        if (a != 255u) {
            r = (r * a + 127u) / 255u;
            gg = (gg * a + 127u) / 255u;
            b = (b * a + 127u) / 255u;
        }
        dst[4 * i + 0] = (uint8_t)(g->swap_rb ? b : r);
        dst[4 * i + 1] = (uint8_t)gg;
        dst[4 * i + 2] = (uint8_t)(g->swap_rb ? r : b);
        dst[4 * i + 3] = (uint8_t)a;
    }
    SDL_UnmapGPUTransferBuffer(g->dev, tb);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = tb, .offset = 0, .pixels_per_row = w, .rows_per_layer = h};
    SDL_GPUTextureRegion reg = {.texture = t, .x = x, .y = y, .w = w, .h = h, .d = 1};
    SDL_UploadToGPUTexture(cp, &src, &reg, false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(g->dev, tb);
    return true;
}

static SDL_GPUTexture *image_tex(embi_canvas_gpu *g, SDL_GPUCommandBuffer *cmd, embi_cimage *im)
{
    if (im->tex != nullptr && im->tex_owner == g) {
        return im->tex;
    }
    if (im->tex != nullptr) {
        cgpu_release_image(im);
    }
    SDL_GPUTexture *t = make_tex(g, im->w, im->h);
    if (t == nullptr) {
        return nullptr;
    }
    if (!upload_pixels(g, cmd, t, im->px, im->w, im->h, 0, 0, false)) {
        SDL_ReleaseGPUTexture(g->dev, t);
        g->bytes -= (size_t)im->w * im->h * 4;
        return nullptr;
    }
    im->tex = t;
    im->tex_owner = g;
    embi_vec_push(&g->images, im);
    return t;
}

/* ------------------------------------------------------------------ replay state */

typedef struct fl {
    embi_canvas_gpu *g;
    embi_canvas *cv;
    SDL_GPUCommandBuffer *cmd;
    embi_frame *df; /* the direct frame, or nullptr */
    embi_layer mask;
    uint32_t mask_idx; /* the clip the cached mask is for (0: none cached) */
    bool ok;
} fl;

static void direct_end(fl *f)
{
    if (f->df != nullptr) {
        embi_frame_end(f->df, f->cmd, f->cv->tex, false, 0);
        f->df = nullptr;
    }
}

static embi_frame *direct(fl *f)
{
    if (f->df == nullptr) {
        f->df = embi_frame_begin(f->g->r, f->cv->w, f->cv->h, 1.0f);
    }
    return f->df;
}

static bool pipeline(embi_canvas_gpu *g)
{
    if (g->pipe == nullptr && !g->pipe_failed) {
        g->pipe = embi_gpu_make_graphics(g->g, &embi_shader_canvas, g->fmt);
        g->samp = SDL_CreateGPUSampler(
            g->dev, &(SDL_GPUSamplerCreateInfo){
                        .min_filter = SDL_GPU_FILTER_LINEAR,
                        .mag_filter = SDL_GPU_FILTER_LINEAR,
                        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                    });
        if (g->pipe == nullptr || g->samp == nullptr) {
            embi_log_error("canvas: pipeline or sampler could not be created: %s", SDL_GetError());
            g->pipe_failed = true;
        }
    }
    return g->pipe != nullptr && g->samp != nullptr;
}

static embi_fx *get_fx(embi_canvas_gpu *g)
{
    if (g->fx == nullptr && !g->fx_failed) {
        g->fx = embi_fx_create(g->g, g->fmt, g->pool);
        g->fx_failed = g->fx == nullptr;
    }
    return g->fx;
}

typedef struct ibox {
    int32_t x0, y0, x1, y1;
} ibox;

static ibox full_box(const embi_canvas *cv)
{
    return (ibox){0, 0, (int32_t)cv->w, (int32_t)cv->h};
}

static ibox box_clip(ibox a, ibox b)
{
    ibox r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
              a.y1 < b.y1 ? a.y1 : b.y1};
    return r;
}

static bool box_empty(ibox b)
{
    return b.x1 <= b.x0 || b.y1 <= b.y0;
}

/* One pass: out <- shader(t0, t1, t2) over bb. out: the texture and its size; load: keep what the target holds. */
static bool run_pass(fl *f, SDL_GPUTexture *out, uint32_t ow, uint32_t oh, bool load, ibox bb, SDL_GPUTexture *t0,
                     SDL_GPUTexture *t1, SDL_GPUTexture *t2, cgpu_params *u)
{
    embi_canvas_gpu *g = f->g;
    if (!pipeline(g) || box_empty(bb)) {
        return false;
    }
    const SDL_GPUColorTargetInfo ci = {
        .texture = out,
        .clear_color = {0, 0, 0, 0},
        .load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(f->cmd, &ci, 1, nullptr);
    if (pass == nullptr) {
        return false;
    }
    const SDL_GPUViewport vp = {0.0f, 0.0f, (float)ow, (float)oh, 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &vp);
    const SDL_Rect sc = {bb.x0, bb.y0, bb.x1 - bb.x0, bb.y1 - bb.y0};
    SDL_SetGPUScissor(pass, &sc);
    SDL_BindGPUGraphicsPipeline(pass, g->pipe);
    const SDL_GPUTextureSamplerBinding ts[3] = {
        {.texture = t0, .sampler = g->samp},
        {.texture = t1 != nullptr ? t1 : t0, .sampler = g->samp},
        {.texture = t2 != nullptr ? t2 : t0, .sampler = g->samp},
    };
    SDL_BindGPUFragmentSamplers(pass, 0, ts, 3);
    SDL_PushGPUFragmentUniformData(f->cmd, 0, u, sizeof *u);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

static void set_sz(cgpu_params *u, const embi_layer *a, const embi_layer *b, const embi_layer *c, const embi_canvas *cv)
{
    u->sz[0] = a != nullptr ? (float)a->tex_w : 1.0f;
    u->sz[1] = a != nullptr ? (float)a->tex_h : 1.0f;
    u->sz[2] = b != nullptr ? (float)b->tex_w : 1.0f;
    u->sz[3] = b != nullptr ? (float)b->tex_h : 1.0f;
    u->sz2[0] = c != nullptr ? (float)c->tex_w : 1.0f;
    u->sz2[1] = c != nullptr ? (float)c->tex_h : 1.0f;
    u->sz2[2] = (float)cv->w;
    u->sz2[3] = (float)cv->h;
}

/* ------------------------------------------------------------------ paths */

static embi_path *build_path(const embi_cpath *p)
{
    embi_path *g = embi_path_create(0.0f);
    const double *q = p->pts.data;
    for (uint32_t i = 0; i < p->ops.len; i++) {
        switch (p->ops.data[i]) {
        case CP_M:
            embi_path_move_to(g, (float)q[0], (float)q[1]);
            q += 2;
            break;
        case CP_L:
            embi_path_line_to(g, (float)q[0], (float)q[1]);
            q += 2;
            break;
        case CP_Q:
            embi_path_quad_to(g, (float)q[0], (float)q[1], (float)q[2], (float)q[3]);
            q += 4;
            break;
        case CP_C:
            embi_path_cubic_to(g, (float)q[0], (float)q[1], (float)q[2], (float)q[3], (float)q[4], (float)q[5]);
            q += 6;
            break;
        default:
            embi_path_close(g);
            break;
        }
    }
    return g;
}

/* The same in user space (under m, the inverse CTM). */
static embi_path *build_path_m(const embi_cpath *p, const double m[6])
{
    embi_cpath *t = embi_cpath_new();
    embi_cpath_add(t, p, m);
    embi_path *g = build_path(t);
    embi_cpath_release(t);
    return g;
}

static void path_bounds_box(const embi_cpath *p, double pad, ibox *out)
{
    double b[4];
    if (!embi_cpath_bounds(p, b)) {
        *out = (ibox){0, 0, 0, 0};
        return;
    }
    *out = (ibox){(int32_t)floor(fmax(b[0] - pad, -1e8)), (int32_t)floor(fmax(b[1] - pad, -1e8)),
                  (int32_t)ceil(fmin(b[2] + pad, 1e8)), (int32_t)ceil(fmin(b[3] + pad, 1e8))};
}

static uint32_t pack_rgba(const float c[4], double alpha_scale)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        double x = c[i];
        if (i == 3) {
            x *= alpha_scale;
        }
        x = x < 0 ? 0 : (x > 1 ? 1 : x);
        v |= (uint32_t)(x * 255.0 + 0.5) << (8 * i);
    }
    return v;
}

static void xf6(const double m[6], float out[6])
{
    for (int i = 0; i < 6; i++) {
        out[i] = (float)m[i];
    }
}

/* ------------------------------------------------------------------ the stroked path of a command */

/* The path without its subpaths of zero length (every point equal), which the canvas strokes as nothing whatever the
 * caps where the stroker draws a dot, and without the lines that go nowhere. */
static embi_cpath *prune_degenerate(const embi_cpath *dev, bool *owned)
{
    embi_cpath *out = embi_cpath_new();
    *owned = true;
    const double *q = dev->pts.data;
    uint32_t i = 0;
    while (i < dev->ops.len) {
        /* A subpath: a move to, then everything up to the next move to. */
        const double *qs = q;
        double x0 = 0, y0 = 0;
        bool same = true;
        uint32_t j = i;
        const double *qq = q;
        if (dev->ops.data[j] == CP_M) {
            x0 = qq[0];
            y0 = qq[1];
            qq += 2;
            j++;
        }
        while (j < dev->ops.len && dev->ops.data[j] != CP_M) {
            int n = dev->ops.data[j] == CP_L ? 2 : dev->ops.data[j] == CP_Q ? 4 : dev->ops.data[j] == CP_C ? 6 : 0;
            for (int k = 0; k < n; k += 2) {
                same = same && qq[k] == x0 && qq[k + 1] == y0;
            }
            qq += n;
            j++;
        }
        if (!same) {
            const double *a = qs;
            double cx = 0, cy = 0;
            for (uint32_t k = i; k < j; k++) {
                switch (dev->ops.data[k]) {
                case CP_M:
                    embi_cpath_move_to(out, nullptr, a[0], a[1]);
                    cx = a[0];
                    cy = a[1];
                    a += 2;
                    break;
                case CP_L:
                    /* A line to the point it starts at has no direction: it would only confuse the joins. */
                    if (a[0] != cx || a[1] != cy) {
                        embi_cpath_line_to(out, nullptr, a[0], a[1]);
                        cx = a[0];
                        cy = a[1];
                    }
                    a += 2;
                    break;
                case CP_Q:
                    embi_cpath_quad_to(out, nullptr, a[0], a[1], a[2], a[3]);
                    cx = a[2];
                    cy = a[3];
                    a += 4;
                    break;
                case CP_C:
                    embi_cpath_cubic_to(out, nullptr, a[0], a[1], a[2], a[3], a[4], a[5]);
                    cx = a[4];
                    cy = a[5];
                    a += 6;
                    break;
                default: embi_cpath_close(out); break;
                }
            }
        }
        i = j;
        q = qq;
    }
    return out;
}

/* The expanded stroke of a device-space path, as a path in user space (draw it under the CTM); nullptr when nothing. */
static embi_path *stroke_user(const embi_cpath *dev0, const cstroke *l, const double ctm[6])
{
    double inv[6];
    if (!cm_invert(ctm, inv)) {
        return nullptr;
    }
    bool pruned_owned;
    const embi_cpath *dev = prune_degenerate(dev0, &pruned_owned);
    embi_path *g = build_path_m(dev, inv);
    if (pruned_owned) {
        embi_cpath_release((embi_cpath *)dev);
    }
    float scale = (float)sqrt(fabs(cm_det(ctm)));
    if (!(scale > 0.0f)) {
        embi_path_destroy(g);
        return nullptr;
    }
    embi_stroke_style st = {
        .width = (float)l->width,
        .cap = (embi_stroke_cap)l->cap,
        .join = (embi_stroke_join)l->join,
        .miter_limit = (float)l->miter,
        .scale = scale,
    };
    float *dash = nullptr;
    if (l->ndash > 0) {
        dash = embi_alloc(l->ndash * sizeof *dash);
        for (uint32_t i = 0; i < l->ndash; i++) {
            dash[i] = (float)l->dash[i];
        }
        st.dash = dash;
        st.dash_count = l->ndash;
        st.dash_offset = (float)l->dash_off;
    }
    embi_path *s = embi_path_stroke(g, &st, 0.25f / scale);
    if (dash != nullptr) {
        embi_free(dash, l->ndash * sizeof *dash);
    }
    embi_path_destroy(g);
    return s;
}

static void gpu_path_box(embi_path *p, const double ctm[6], ibox *out)
{
    uint32_t n = 0;
    const embi_qcurve *sg = embi_path_segments(p, &n);
    if (n == 0) {
        *out = (ibox){0, 0, 0, 0};
        return;
    }
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (uint32_t i = 0; i < n; i++) {
        const float pts[3][2] = {{sg[i].x0, sg[i].y0}, {sg[i].cx, sg[i].cy}, {sg[i].x1, sg[i].y1}};
        for (int k = 0; k < 3; k++) {
            double dx, dy;
            cm_apply(ctm, pts[k][0], pts[k][1], &dx, &dy);
            x0 = dx < x0 ? dx : x0;
            y0 = dy < y0 ? dy : y0;
            x1 = dx > x1 ? dx : x1;
            y1 = dy > y1 ? dy : y1;
        }
    }
    *out = (ibox){(int32_t)floor(fmax(x0 - 2, -1e8)), (int32_t)floor(fmax(y0 - 2, -1e8)),
                  (int32_t)ceil(fmin(x1 + 2, 1e8)), (int32_t)ceil(fmin(y1 + 2, 1e8))};
}

/* ------------------------------------------------------------------ the clip mask */

static void release_mask(fl *f)
{
    if (f->mask.tex != nullptr) {
        embi_layer_release(f->g->pool, &f->mask);
    }
    f->mask_idx = 0;
}

/* Renders the coverage of a path (device space, nonzero or even-odd) into a new layer. */
static bool coverage_layer(fl *f, const embi_cpath *p, bool evenodd, embi_layer *out)
{
    direct_end(f);
    if (!embi_layer_acquire(f->g->pool, f->cv->w, f->cv->h, out)) {
        return false;
    }
    embi_frame *fr = embi_frame_begin(f->g->r, out->tex_w, out->tex_h, 1.0f);
    if (!embi_cpath_empty(p)) {
        embi_path *gp = build_path(p);
        embi_paint white = embi_paint_solid(0xFFFFFFFFu);
        embi_frame_fill_path(fr, gp, evenodd ? EMBI_FILL_EVENODD : EMBI_FILL_NONZERO, &white, 0, 0);
        embi_path_destroy(gp);
    }
    embi_frame_end(fr, f->cmd, out->tex, true, 0);
    return true;
}

static bool get_mask_chain(fl *f, uint32_t clip, const uint32_t *chain, uint32_t n);

static bool get_mask(fl *f, uint32_t clip)
{
    if (clip == 0) {
        return false;
    }
    if (f->mask_idx == clip && f->mask.tex != nullptr) {
        return true;
    }
    release_mask(f);
    /* The whole chain of ancestors (a clip is the intersection of every clip() since the matching save). */
    uint32_t depth = 0;
    for (uint32_t c = clip; c != 0; c = f->cv->clips.data[c].parent) {
        depth++;
    }
    uint32_t *chain = embi_alloc((size_t)depth * sizeof *chain);
    uint32_t n = 0;
    for (uint32_t c = clip; c != 0; c = f->cv->clips.data[c].parent) {
        chain[n++] = c;
    }
    bool ok = get_mask_chain(f, clip, chain, n);
    embi_free(chain, (size_t)depth * sizeof *chain);
    return ok;
}

static bool get_mask_chain(fl *f, uint32_t clip, const uint32_t *chain, uint32_t n)
{
    embi_layer m = {0};
    bool have = false;
    for (uint32_t i = n; i-- > 0;) {
        const cclip *cc = &f->cv->clips.data[chain[i]];
        embi_layer cov = {0};
        if (!coverage_layer(f, cc->path, cc->evenodd, &cov)) {
            if (have) {
                embi_layer_release(f->g->pool, &m);
            }
            return false;
        }
        if (!have) {
            m = cov;
            have = true;
            continue;
        }
        embi_layer nm = {0};
        if (!embi_layer_acquire(f->g->pool, f->cv->w, f->cv->h, &nm)) {
            embi_layer_release(f->g->pool, &cov);
            embi_layer_release(f->g->pool, &m);
            return false;
        }
        cgpu_params u;
        memset(&u, 0, sizeof u);
        u.mode[0] = 2.0f;
        set_sz(&u, &m, &cov, nullptr, f->cv);
        (void)run_pass(f, nm.tex, nm.tex_w, nm.tex_h, false, full_box(f->cv), m.tex, cov.tex, nullptr, &u);
        embi_layer_release(f->g->pool, &m);
        embi_layer_release(f->g->pool, &cov);
        m = nm;
    }
    f->mask = m;
    f->mask_idx = clip;
    return true;
}

/* ------------------------------------------------------------------ composing */

static bool op_needs_whole(uint8_t op)
{
    return op == CV_OP_SOURCE_IN || op == CV_OP_SOURCE_OUT || op == CV_OP_DEST_IN || op == CV_OP_DEST_ATOP ||
           op == CV_OP_COPY;
}

/* backdrop <- op(src * alpha, backdrop) over bb, under the mask of the clip. */
static void compose(fl *f, const embi_layer *src, const csnap *sn, double alpha, ibox bb)
{
    embi_canvas *cv = f->cv;
    bb = box_clip(bb, full_box(cv));
    if (box_empty(bb)) {
        return;
    }
    bool masked = get_mask(f, sn->clip);
    if (sn->clip != 0 && !masked) {
        return;
    }
    embi_layer d = {0};
    if (!embi_layer_acquire(f->g->pool, cv->w, cv->h, &d)) {
        return;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(f->cmd);
    SDL_GPUTextureLocation sl = {.texture = cv->tex, .x = (Uint32)bb.x0, .y = (Uint32)bb.y0};
    SDL_GPUTextureLocation dl = {.texture = d.tex, .x = (Uint32)bb.x0, .y = (Uint32)bb.y0};
    SDL_CopyGPUTextureToTexture(cp, &sl, &dl, (Uint32)(bb.x1 - bb.x0), (Uint32)(bb.y1 - bb.y0), 1, false);
    SDL_EndGPUCopyPass(cp);
    cgpu_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = 0.0f;
    u.mode[1] = (float)sn->op;
    u.mode[3] = (float)((masked ? 1 : 0) | (cv->alpha ? 0 : 2));
    u.a[0] = (float)alpha;
    set_sz(&u, src, &d, masked ? &f->mask : nullptr, cv);
    (void)run_pass(f, cv->tex, cv->w, cv->h, true, bb, src->tex, d.tex, masked ? f->mask.tex : nullptr, &u);
    embi_layer_release(f->g->pool, &d);
}

typedef struct source {
    embi_layer layer; /* the premultiplied source picture */
    bool have;
} source;

static void paint_uniforms(cgpu_params *u, const cpaint_cmd *p, const double ctm[6], const csnap *sn, uint32_t flags)
{
    double m[6];
    memcpy(m, ctm, sizeof m);
    if (p->kind == 2) {
        cm_mul(m, p->pm, m);
    }
    double inv[6];
    if (!cm_invert(m, inv)) {
        memset(inv, 0, sizeof inv);
    }
    u->inv0[0] = (float)inv[0];
    u->inv0[1] = (float)inv[1];
    u->inv0[2] = (float)inv[2];
    u->inv0[3] = (float)inv[3];
    u->inv1[0] = (float)inv[4];
    u->inv1[1] = (float)inv[5];
    u->mode[3] = (float)flags;
    (void)sn;
}

/* Turns the coverage layer cov into the paint of p (a gradient) as a new layer. */
static bool paint_pass(fl *f, const embi_layer *cov, const cpaint_cmd *p, const double ctm[6], const csnap *sn, ibox bb,
                       SDL_GPUTexture *img, uint32_t img_w, uint32_t img_h, const double srect[4], embi_layer *out)
{
    if (!embi_layer_acquire(f->g->pool, f->cv->w, f->cv->h, out)) {
        return false;
    }
    cgpu_params u;
    memset(&u, 0, sizeof u);
    u.mode[0] = 1.0f;
    uint32_t flags = sn->smooth ? 4u : 0u;
    if (p->kind == 1) {
        const embi_cgrad *g = p->g;
        u.mode[2] = (float)g->kind;
        uint32_t n = g->stops.len > CG_MAX_STOPS ? CG_MAX_STOPS : g->stops.len;
        u.a[1] = (float)n;
        for (uint32_t i = 0; i < n; i++) {
            u.stop_off[i] = g->stops.data[i].off;
            memcpy(u.stop_col[i], g->stops.data[i].rgba, sizeof u.stop_col[i]);
        }
        if (g->kind == EMBI_CG_LINEAR) {
            for (int i = 0; i < 4; i++) {
                u.geom0[i] = (float)g->g[i];
            }
        } else if (g->kind == EMBI_CG_RADIAL) {
            u.geom0[0] = (float)g->g[0];
            u.geom0[1] = (float)g->g[1];
            u.geom0[2] = (float)g->g[2];
            u.geom0[3] = (float)g->g[3];
            u.geom1[0] = (float)g->g[4];
            u.geom1[1] = (float)g->g[5];
        } else {
            u.geom0[0] = (float)g->g[0];
            u.geom0[1] = (float)g->g[1];
            u.geom0[2] = (float)g->g[2];
        }
    } else {
        u.mode[2] = 3.0f;
        if (p->repeat == EMBI_CP_REPEAT || p->repeat == EMBI_CP_REPEAT_X) {
            flags |= 8u;
        }
        if (p->repeat == EMBI_CP_REPEAT || p->repeat == EMBI_CP_REPEAT_Y) {
            flags |= 16u;
        }
        if (srect != nullptr) {
            flags |= 32u;
            for (int i = 0; i < 4; i++) {
                u.src[i] = (float)srect[i];
            }
        }
    }
    paint_uniforms(&u, p, ctm, sn, flags);
    set_sz(&u, cov, nullptr, nullptr, f->cv);
    if (img != nullptr) {
        u.sz[2] = (float)img_w;
        u.sz[3] = (float)img_h;
    }
    bool ok = run_pass(f, out->tex, out->tex_w, out->tex_h, false, bb, cov->tex, img != nullptr ? img : cov->tex,
                       nullptr, &u);
    if (!ok) {
        embi_layer_release(f->g->pool, out);
    }
    return ok;
}

/* Builds the premultiplied source picture of a command displaced by (dx, dy) device pixels: the shadow is the source drawn
 * at the shadow offset, so that a shape wholly outside the canvas still casts a shadow into it. */
typedef bool (*src_build_fn)(fl *f, const void *ud, double dx, double dy, embi_layer *out);

/* Composes the finished source (the layer s) with the shadow first, then the picture. bb is the unclipped device box of the
 * source. */
static void finish_source(fl *f, embi_layer *s, const csnap *sn, ibox bb, bool whole_canvas, src_build_fn build,
                          const void *ud)
{
    embi_canvas *cv = f->cv;
    bool shadow = sn->sh_rgba[3] > 0.0f && (sn->sh_x != 0.0f || sn->sh_y != 0.0f || sn->sh_blur != 0.0f);
    if (shadow && build != nullptr) {
        embi_fx *fx = get_fx(f->g);
        embi_layer shifted = {0};
        if (fx != nullptr && build(f, ud, sn->sh_x, sn->sh_y, &shifted)) {
            embi_layer blurred = {0};
            const embi_layer *b = &shifted;
            if (sn->sh_blur > 0.0f && embi_layer_acquire(f->g->pool, cv->w, cv->h, &blurred)) {
                (void)embi_fx_blur(fx, f->cmd, &shifted, &blurred, sn->sh_blur * 0.5f);
                b = &blurred;
            }
            embi_layer tint = {0};
            if (embi_layer_acquire(f->g->pool, cv->w, cv->h, &tint)) {
                cgpu_params u;
                memset(&u, 0, sizeof u);
                u.mode[0] = 3.0f;
                memcpy(u.color, sn->sh_rgba, sizeof u.color);
                set_sz(&u, b, nullptr, nullptr, cv);
                (void)run_pass(f, tint.tex, tint.tex_w, tint.tex_h, false, full_box(cv), b->tex, nullptr, nullptr, &u);
                ibox sbb = bb;
                double pad = 3.0 * (sn->sh_blur * 0.5) + 2.0;
                sbb.x0 += (int32_t)floor(sn->sh_x - pad);
                sbb.y0 += (int32_t)floor(sn->sh_y - pad);
                sbb.x1 += (int32_t)ceil(sn->sh_x + pad);
                sbb.y1 += (int32_t)ceil(sn->sh_y + pad);
                compose(f, &tint, sn, sn->alpha, whole_canvas ? full_box(cv) : box_clip(sbb, full_box(cv)));
                embi_layer_release(f->g->pool, &tint);
            }
            if (blurred.tex != nullptr) {
                embi_layer_release(f->g->pool, &blurred);
            }
            embi_layer_release(f->g->pool, &shifted);
        }
    }
    compose(f, s, sn, sn->alpha, whole_canvas ? full_box(cv) : box_clip(bb, full_box(cv)));
}

/* ------------------------------------------------------------------ commands */

static bool is_fast(const csnap *sn, const cpaint_cmd *p)
{
    bool shadow = sn->sh_rgba[3] > 0.0f && (sn->sh_x != 0.0f || sn->sh_y != 0.0f || sn->sh_blur != 0.0f);
    return sn->op == CV_OP_SOURCE_OVER && !shadow && sn->clip == 0 && (p == nullptr || p->kind == 0);
}

/* Renders the shape of a fill or a stroke into a new layer: color when the paint is solid (nonzero alpha is baked in), else
 * white coverage. gp: the path in the space of the xform m (nullptr: device space). */
static bool shape_layer(fl *f, embi_path *gp, bool evenodd, const double *m, const cpaint_cmd *p, embi_layer *out)
{
    direct_end(f);
    if (!embi_layer_acquire(f->g->pool, f->cv->w, f->cv->h, out)) {
        return false;
    }
    embi_frame *fr = embi_frame_begin(f->g->r, out->tex_w, out->tex_h, 1.0f);
    embi_paint paint = embi_paint_solid(p != nullptr && p->kind == 0 ? pack_rgba(p->rgba, 1.0) : 0xFFFFFFFFu);
    uint32_t x = 0;
    if (m != nullptr) {
        float fm[6];
        xf6(m, fm);
        x = embi_frame_xform(fr, fm);
    }
    embi_frame_fill_path(fr, gp, evenodd ? EMBI_FILL_EVENODD : EMBI_FILL_NONZERO, &paint, x, 0);
    embi_frame_end(fr, f->cmd, out->tex, true, 0);
    return true;
}

typedef struct shape_ctx {
    embi_path *gp;
    bool evenodd;
    const double *m;         /* the transform of gp (nullptr: device space) */
    const cpaint_cmd *fill;  /* what shape_layer fills with */
    const cpaint_cmd *pp;    /* the paint pass that follows (gradient, pattern, image), or nullptr */
    SDL_GPUTexture *img;
    uint32_t iw, ih;
    const double *srect;
    const csnap *sn;
} shape_ctx;

/* The source of a shape command displaced by (dx, dy). */
static bool build_shape(fl *f, const void *ud, double dx, double dy, embi_layer *out)
{
    shape_ctx clr;
    const shape_ctx *sc = ud;
    static const cpaint_cmd opaque = {.rgba = {1.0f, 1.0f, 1.0f, 1.0f}};
    if (sc->sn->clear) {
        /* clear erases by coverage alone: an opaque source whatever the paint is */
        clr = *sc;
        clr.fill = &opaque;
        clr.pp = nullptr;
        sc = &clr;
    }
    double t[6] = {1, 0, 0, 1, dx, dy}, m2[6], c2[6];
    const double *mp = sc->m;
    cm_mul(t, sc->sn->ctm, c2);
    if (dx != 0.0 || dy != 0.0) {
        if (sc->m != nullptr) {
            cm_mul(t, sc->m, m2);
        } else {
            memcpy(m2, t, sizeof m2);
        }
        mp = m2;
    }
    embi_layer cov = {0};
    if (!shape_layer(f, sc->gp, sc->evenodd, mp, sc->fill, &cov)) {
        return false;
    }
    if (sc->pp == nullptr) {
        *out = cov;
        return true;
    }
    bool ok = paint_pass(f, &cov, sc->pp, c2, sc->sn, full_box(f->cv), sc->img, sc->iw, sc->ih, sc->srect, out);
    embi_layer_release(f->g->pool, &cov);
    return ok;
}

/* Draws an already-built GPU path (device space, or user space under m) with the paint of a command through the whole
 * pipeline. */
static void draw_shape(fl *f, embi_path *gp, bool evenodd, const double *m, const cpaint_cmd *paint, const csnap *sn,
                       ibox bb)
{
    if (is_fast(sn, paint)) {
        embi_frame *fr = direct(f);
        float fm[6];
        uint32_t x = 0;
        if (m != nullptr) {
            xf6(m, fm);
            x = embi_frame_xform(fr, fm);
        }
        embi_paint pt = embi_paint_solid(pack_rgba(paint->rgba, sn->alpha));
        embi_frame_fill_path(fr, gp, evenodd ? EMBI_FILL_EVENODD : EMBI_FILL_NONZERO, &pt, x, 0);
        return;
    }
    shape_ctx sc = {.gp = gp, .evenodd = evenodd, .m = m, .fill = paint, .sn = sn};
    if (paint != nullptr && paint->kind != 0) {
        sc.pp = paint;
        if (paint->kind == 2) {
            embi_cimage *im = paint->im;
            sc.img = im != nullptr ? image_tex(f->g, f->cmd, im) : nullptr;
            if (sc.img == nullptr) {
                return;
            }
            sc.iw = im->w;
            sc.ih = im->h;
        }
    }
    embi_layer s = {0};
    if (!build_shape(f, &sc, 0.0, 0.0, &s)) {
        return;
    }
    finish_source(f, &s, sn, bb, op_needs_whole(sn->op), build_shape, &sc);
    embi_layer_release(f->g->pool, &s);
}

static void do_fill(fl *f, const ccmd *c, const csnap *sn)
{
    embi_path *gp = build_path(c->path);
    ibox bb;
    path_bounds_box(c->path, 2.0, &bb);
    draw_shape(f, gp, c->evenodd, nullptr, &c->paint, sn, bb);
    embi_path_destroy(gp);
}

static void do_stroke(fl *f, const ccmd *c, const csnap *sn)
{
    embi_path *s = stroke_user(c->path, &c->line, sn->ctm);
    if (s == nullptr) {
        return;
    }
    ibox bb;
    gpu_path_box(s, sn->ctm, &bb);
    draw_shape(f, s, false, sn->ctm, &c->paint, sn, bb);
    embi_path_destroy(s);
}

static void do_clear(fl *f, const ccmd *c, const csnap *sn)
{
    embi_canvas *cv = f->cv;
    csnap n = *sn;
    memset(n.sh_rgba, 0, sizeof n.sh_rgba);
    n.alpha = 1.0f;
    n.op = cv->alpha ? CV_OP_DEST_OUT : CV_OP_SOURCE_OVER;
    cpaint_cmd black;
    memset(&black, 0, sizeof black);
    black.rgba[3] = 1.0f;
    embi_path *gp = build_path(c->path);
    ibox bb;
    path_bounds_box(c->path, 2.0, &bb);
    draw_shape(f, gp, false, nullptr, &black, &n, bb);
    embi_path_destroy(gp);
}

static void do_image(fl *f, const ccmd *c, const csnap *sn)
{
    embi_canvas *cv = f->cv;
    SDL_GPUTexture *img = nullptr;
    uint32_t iw, ih;
    if (c->im != nullptr) {
        img = image_tex(f->g, f->cmd, c->im);
        iw = c->im->w;
        ih = c->im->h;
    } else {
        embi_canvas *src = c->src_cv;
        if (src->gpu == f->g && src->tex != nullptr && src != cv) {
            img = src->tex;
        }
        iw = c->src_w;
        ih = c->src_h;
    }
    if (img == nullptr) {
        return;
    }
    const double *s = c->s, *d = c->d;
    ibox bb;
    path_bounds_box(c->path, 2.0, &bb);
    /* The whole image under a smoothing sampler, source-over, no shadow and no clip: the renderer's image prim. */
    bool whole = s[0] == 0.0 && s[1] == 0.0 && s[2] == (double)iw && s[3] == (double)ih;
    if (whole && sn->smooth && is_fast(sn, nullptr)) {
        embi_layer lay = {.tex = img, .w = iw, .h = ih, .tex_w = iw, .tex_h = ih};
        embi_frame *fr = direct(f);
        float fm[6];
        xf6(sn->ctm, fm);
        uint32_t x = embi_frame_xform(fr, fm);
        const float src[4] = {0, 0, (float)iw, (float)ih};
        const float dst[4] = {(float)d[0], (float)d[1], (float)d[2], (float)d[3]};
        const float radii[4] = {0, 0, 0, 0};
        embi_frame_layer(fr, &lay, src, dst, radii, sn->alpha, x, 0);
        return;
    }
    embi_path *gp = build_path(c->path);
    static const cpaint_cmd white = {.rgba = {1.0f, 1.0f, 1.0f, 1.0f}};
    /* Device pixel to image pixel: the inverse of CTM . translate(d) . scale(d / s) . translate(-s). */
    cpaint_cmd pp;
    memset(&pp, 0, sizeof pp);
    pp.kind = 2;
    pp.repeat = EMBI_CP_NO_REPEAT;
    pp.pm[0] = d[2] / s[2];
    pp.pm[3] = d[3] / s[3];
    pp.pm[4] = d[0] - s[0] * d[2] / s[2];
    pp.pm[5] = d[1] - s[1] * d[3] / s[3];
    pp.pm[1] = pp.pm[2] = 0;
    double srect[4] = {s[0], s[1], s[0] + s[2], s[1] + s[3]};
    shape_ctx sc = {.gp = gp, .fill = &white, .pp = &pp, .img = img, .iw = iw, .ih = ih, .srect = srect, .sn = sn};
    embi_layer pl = {0};
    if (build_shape(f, &sc, 0.0, 0.0, &pl)) {
        finish_source(f, &pl, sn, bb, op_needs_whole(sn->op), build_shape, &sc);
        embi_layer_release(f->g->pool, &pl);
    }
    embi_path_destroy(gp);
}

typedef struct text_ctx {
    ctext_layout *l;
    double t[6]; /* origin and horizontal scale of the layout */
    const ccmd *c;
    const csnap *sn;
    SDL_GPUTexture *img;
} text_ctx;

static bool build_text(fl *f, const void *ud, double dx, double dy, embi_layer *out)
{
    const text_ctx *tc = ud;
    const csnap *sn = tc->sn;
    const ccmd *c = tc->c;
    double tr[6] = {1, 0, 0, 1, dx, dy}, c2[6], m[6];
    cm_mul(tr, sn->ctm, c2);
    cm_mul(c2, tc->t, m);
    float fm[6];
    xf6(m, fm);
    embi_layer s = {0};
    if (!embi_layer_acquire(f->g->pool, f->cv->w, f->cv->h, &s)) {
        return false;
    }
    embi_frame *fr = embi_frame_begin(f->g->r, s.tex_w, s.tex_h, 1.0f);
    uint32_t x = embi_frame_xform(fr, fm);
    ctext_draw(tc->l, fr, c->paint.kind == 0 && !sn->clear ? pack_rgba(c->paint.rgba, 1.0) : 0xFFFFFFFFu, x, 0);
    embi_frame_end(fr, f->cmd, s.tex, true, 0);
    if (c->paint.kind == 0 || sn->clear) {
        *out = s;
        return true;
    }
    uint32_t iw = c->paint.kind == 2 ? c->paint.im->w : 0, ih = c->paint.kind == 2 ? c->paint.im->h : 0;
    bool ok = paint_pass(f, &s, &c->paint, c2, sn, full_box(f->cv), tc->img, iw, ih, nullptr, out);
    embi_layer_release(f->g->pool, &s);
    return ok;
}

static void do_text(fl *f, const ccmd *c, const csnap *sn)
{
    embi_canvas *cv = f->cv;
    ctext_layout *l = ctext_shape(cv->env.text, &c->text);
    if (l == nullptr) {
        return;
    }
    double ox, oy, sx;
    ctext_origin(l, &c->text, &ox, &oy, &sx);
    if (c->stroke_text) {
        embi_cpath *outline = embi_cpath_new();
        ctext_outline(l, outline, ox, oy, sx);
        if (!embi_cpath_empty(outline)) {
            embi_path *gp = build_path(outline);
            /* The outline is in user space already: stroke it there. */
            double inv[6];
            if (cm_invert(sn->ctm, inv)) {
                embi_stroke_style st = {
                    .width = (float)c->line.width,
                    .cap = (embi_stroke_cap)c->line.cap,
                    .join = (embi_stroke_join)c->line.join,
                    .miter_limit = (float)c->line.miter,
                    .scale = (float)sqrt(fabs(cm_det(sn->ctm))),
                };
                float *dash = nullptr;
                if (c->line.ndash > 0) {
                    dash = embi_alloc(c->line.ndash * sizeof *dash);
                    for (uint32_t i = 0; i < c->line.ndash; i++) {
                        dash[i] = (float)c->line.dash[i];
                    }
                    st.dash = dash;
                    st.dash_count = c->line.ndash;
                    st.dash_offset = (float)c->line.dash_off;
                }
                embi_path *sp = embi_path_stroke(gp, &st, st.scale > 0 ? 0.25f / st.scale : 0.0f);
                if (dash != nullptr) {
                    embi_free(dash, c->line.ndash * sizeof *dash);
                }
                if (sp != nullptr) {
                    ibox bb;
                    gpu_path_box(sp, sn->ctm, &bb);
                    draw_shape(f, sp, false, sn->ctm, &c->paint, sn, bb);
                    embi_path_destroy(sp);
                }
            }
            embi_path_destroy(gp);
        }
        embi_cpath_release(outline);
        ctext_layout_free(l);
        return;
    }
    /* Fill: the glyphs drawn at the origin under CTM . translate . scale. */
    double t[6] = {sx, 0, 0, 1, ox, oy};
    double m[6];
    cm_mul(sn->ctm, t, m);
    float fm[6];
    xf6(m, fm);
    if (is_fast(sn, &c->paint)) {
        embi_frame *fr = direct(f);
        uint32_t x = embi_frame_xform(fr, fm);
        ctext_draw(l, fr, pack_rgba(c->paint.rgba, sn->alpha), x, 0);
        ctext_layout_free(l);
        return;
    }
    direct_end(f);
    text_ctx tc = {.l = l, .c = c, .sn = sn};
    memcpy(tc.t, t, sizeof tc.t);
    SDL_GPUTexture *img = nullptr;
    if (c->paint.kind == 2) {
        img = c->paint.im != nullptr ? image_tex(f->g, f->cmd, c->paint.im) : nullptr;
        if (img == nullptr) {
            ctext_layout_free(l);
            return;
        }
        tc.img = img;
    }
    embi_layer s = {0};
    if (build_text(f, &tc, 0.0, 0.0, &s)) {
        finish_source(f, &s, sn, full_box(cv), true, build_text, &tc);
        embi_layer_release(f->g->pool, &s);
    }
    ctext_layout_free(l);
}

static void do_put(fl *f, const ccmd *c)
{
    embi_canvas *cv = f->cv;
    direct_end(f);
    const embi_cimage *im = c->im;
    int64_t x0 = c->px, y0 = c->py;
    int64_t cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0;
    int64_t cx1 = x0 + (int64_t)im->w > (int64_t)cv->w ? (int64_t)cv->w : x0 + (int64_t)im->w;
    int64_t cy1 = y0 + (int64_t)im->h > (int64_t)cv->h ? (int64_t)cv->h : y0 + (int64_t)im->h;
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }
    uint32_t w = (uint32_t)(cx1 - cx0), h = (uint32_t)(cy1 - cy0);
    uint8_t *sub = embi_alloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        memcpy(sub + (size_t)y * w * 4, im->px + (((size_t)(cy0 - y0) + y) * im->w + (size_t)(cx0 - x0)) * 4, (size_t)w * 4);
    }
    (void)upload_pixels(f->g, f->cmd, cv->tex, sub, w, h, (uint32_t)cx0, (uint32_t)cy0, !cv->alpha);
    embi_free(sub, (size_t)w * h * 4);
}

/* ------------------------------------------------------------------ flushing */

void cgpu_flush_canvas(embi_canvas *cv, void *cmd_in)
{
    embi_canvas_gpu *g = cv->gpu;
    if (g == nullptr && cv->env.gpu != nullptr) {
        cgpu_attach(cv);
        g = cv->gpu;
    }
    if (g == nullptr) {
        cv_cmds_clear(cv);
        return;
    }
    if (cv->cmds.len == 0 && !cv->need_clear && cv->tex != nullptr) {
        return;
    }
    SDL_GPUCommandBuffer *cmd = cmd_in;
    bool own = cmd == nullptr;
    if (own) {
        cmd = SDL_AcquireGPUCommandBuffer(g->dev);
        if (cmd == nullptr) {
            cv_cmds_clear(cv);
            return;
        }
    }
    fl f;
    memset(&f, 0, sizeof f);
    f.g = g;
    f.cv = cv;
    f.cmd = cmd;
    f.ok = ensure_tex(cv, cmd);
    if (f.ok) {
        for (uint32_t i = 0; i < cv->cmds.len; i++) {
            const ccmd *c = &cv->cmds.data[i];
            const csnap *sn = &cv->snaps.data[c->snap];
            switch (c->kind) {
            case CK_FILL: do_fill(&f, c, sn); break;
            case CK_STROKE: do_stroke(&f, c, sn); break;
            case CK_CLEAR: do_clear(&f, c, sn); break;
            case CK_IMAGE: do_image(&f, c, sn); break;
            case CK_TEXT: do_text(&f, c, sn); break;
            case CK_PUT: do_put(&f, c); break;
            default: break;
            }
        }
    }
    direct_end(&f);
    release_mask(&f);
    cv_cmds_clear(cv);
    if (own) {
        SDL_SubmitGPUCommandBuffer(cmd);
    }
}

void embi_canvas_gpu_flush(embi_canvas_gpu *g, void *cmd)
{
    for (embi_canvas *cv = g->head; cv != nullptr; cv = cv->gnext) {
        if (cv->cmds.len > 0 || cv->need_clear) {
            cgpu_flush_canvas(cv, cmd);
        }
    }
}

/* ------------------------------------------------------------------ readback */

bool cgpu_read(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t *out)
{
    embi_canvas_gpu *g = cv->gpu;
    if (g == nullptr && cv->env.gpu != nullptr) {
        cgpu_attach(cv);
        g = cv->gpu;
    }
    if (g == nullptr || cv->w == 0 || cv->h == 0) {
        return false;
    }
    cgpu_flush_canvas(cv, nullptr);
    if (cv->tex == nullptr) {
        return false;
    }
    int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w > cv->w ? cv->w : (int64_t)x + w;
    int64_t y1 = (int64_t)y + h > cv->h ? cv->h : (int64_t)y + h;
    if (x1 <= x0 || y1 <= y0) {
        return true;
    }
    uint32_t rw = (uint32_t)(x1 - x0), rh = (uint32_t)(y1 - y0);
    size_t bytes = (size_t)rw * rh * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(
        g->dev, &(SDL_GPUTransferBufferCreateInfo){.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)bytes});
    SDL_GPUCommandBuffer *cmd = tb != nullptr ? SDL_AcquireGPUCommandBuffer(g->dev) : nullptr;
    if (cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(g->dev, tb);
        }
        return false;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg = {.texture = cv->tex, .x = (Uint32)x0, .y = (Uint32)y0, .w = rw, .h = rh, .d = 1};
    SDL_GPUTextureTransferInfo info = {.transfer_buffer = tb, .pixels_per_row = rw, .rows_per_layer = rh};
    SDL_DownloadFromGPUTexture(cp, &reg, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    bool ok = false;
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g->dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g->dev, fence);
        const uint8_t *m = SDL_MapGPUTransferBuffer(g->dev, tb, false);
        if (m != nullptr) {
            for (uint32_t j = 0; j < rh; j++) {
                for (uint32_t i = 0; i < rw; i++) {
                    const uint8_t *s = m + ((size_t)j * rw + i) * 4;
                    uint32_t r = s[g->swap_rb ? 2 : 0], gg = s[1], b = s[g->swap_rb ? 0 : 2], a = s[3];
                    if (a != 255u && a != 0u) {
                        r = r >= a ? 255u : (r * 255u + a / 2u) / a;
                        gg = gg >= a ? 255u : (gg * 255u + a / 2u) / a;
                        b = b >= a ? 255u : (b * 255u + a / 2u) / a;
                    } else if (a == 0u) {
                        r = gg = b = 0;
                    }
                    uint8_t *d = out + (((size_t)(y0 - y) + j) * w + (size_t)(x0 - x) + i) * 4;
                    d[0] = (uint8_t)r;
                    d[1] = (uint8_t)gg;
                    d[2] = (uint8_t)b;
                    d[3] = (uint8_t)a;
                }
            }
            SDL_UnmapGPUTransferBuffer(g->dev, tb);
            ok = true;
        }
    }
    SDL_ReleaseGPUTransferBuffer(g->dev, tb);
    return ok;
}
