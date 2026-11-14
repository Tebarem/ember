#include "gpu/render.h"

#include "base/base.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_stdinc.h>

#include "gpu/atlas.h"

#include "gpu/slug.h"

#include "ember_shaders/box.h"

/* Renderer design (see render.h and shaders/box.slang):
 *
 * - One ubershader pipeline draws every prim. A prim is an instance of a 6-vertex quad (no vertex
 *   buffer). Box prims (fills, borders, gradients, images) and outline prims (glyphs and paths,
 *   Slug coverage) share one 64-byte layout (embi_prim), and the prims of a frame are drawn in
 *   the order they were recorded, so paint order is exact.
 * - Storage buffers hold the data: prims, transforms, clips, borders, gradients, stops, the Slug
 *   blob buffer, and a one-element frame-params buffer. The vertex stage uses no uniform buffers,
 *   so every backend sees the same slot numbers (shaders/lib/bind.slang).
 * - The prim data for each batch is its own GPU buffer, so the shader indexes from 0 and no
 *   base-offset uniform is needed. A batch breaks where its texture changes: the atlas array is one
 *   texture whatever its layer, so images on different atlas pages share a batch, and a solo image
 *   or a layer (a texture of its own) starts one. It also breaks at EMBI_BATCH_MAX_PRIMS prims and
 *   at the clear prim of a region frame. A prim that samples no texture joins the batch of its
 *   neighbors. A batch with no texture binds the renderer's 1x1 blank array, which only image prims
 *   sample. A frame under the cap with one texture is one batch and one draw.
 * - Textures are sampled as 2D arrays (one layer per atlas page; a solo image or a layer has one
 *   layer). Image prims carry their layer in the kind word and their texture slot in the batch
 *   tags. The atlas texture is resolved when the frame ends, after the atlas's pending growth
 *   copies are recorded into the frame's command buffer, so an atlas that grew during the
 *   frame is bound at its current size.
 * - Each frame is uploaded at once. The CPU arrays go through one ring transfer buffer (embi_upload_map), in a
 *   single copy pass before the render pass.
 * - Outlines take their blob from the Slug buffer (slug.c), uploaded before the frame's copy pass.
 *   Every batch binds that buffer, so a batch may mix outline and box prims freely. If the upload
 *   failed, the outline prims of the frame are hidden rather than drawn from stale data.
 * - Steady state: the CPU arrays, batch list and transfer buffer are reused, so a frame with the
 *   same size allocates no CPU memory.
 *
 * Clip depth: the shader walks at most EMBI_MAX_CLIP_DEPTH clips. When a clip is added under a
 * parent that is already that deep, the outermost two levels of the parent's chain are folded
 * into one clip (clip_fold). If two adjacent levels are unrounded and share a transform, they
 * are intersected exactly. Otherwise they become one axis-aligned clip with the intersection of
 * their device-space bounds, which can leak a little at the rounded corners of the outer level.
 * Every other level is copied unchanged, so the rest of the chain stays exact.
 *
 * Limits: a frame that exceeds a cap is still ended, but with nothing drawn, and the error is
 * logged. Caps are chosen so that every upload fits in 32-bit offsets.
 */

#define EMBI_RENDER_MAX_DIM 16384u
#define EMBI_RENDER_SIGMA_MIN 1e-6f
#define EMBI_RENDER_MAX_PRIMS (1u << 22)   /* 256 MiB of prims */
#define EMBI_RENDER_MAX_XFORMS (1u << 20)
#define EMBI_RENDER_MAX_CLIPS (1u << 20)
#define EMBI_RENDER_MAX_BORDERS (1u << 21) /* 128 MiB of 64-byte records; the index fits 24 bits */
#define EMBI_BATCH_MAX_PRIMS (1u << 20)    /* 64 MiB of prims per batch buffer */
#define EMBI_RENDER_MAX_GRADIENTS (1u << 20) /* 32 MiB of gradient records */
#define EMBI_RENDER_MAX_STOPS (1u << 22)     /* 32 MiB of stops */
#define EMBI_RENDER_MAX_TEXS 4096u           /* distinct textures (atlas, solo pages, layers) per frame */
#define EMBI_NO_TEX UINT32_MAX               /* a prim or batch that samples no texture */

/* Largest byte size each storage buffer can reach, from the element caps above. Each is at most
 * 2^27, the smallest maxStorageBufferRange Vulkan guarantees. */
#define EMBI_MAXB_PRIMS ((size_t)EMBI_BATCH_MAX_PRIMS * sizeof(embi_prim))
#define EMBI_MAXB_XFORMS ((size_t)EMBI_RENDER_MAX_XFORMS * sizeof(embi_xform))
#define EMBI_MAXB_CLIPS ((size_t)EMBI_RENDER_MAX_CLIPS * sizeof(embi_clip))
#define EMBI_MAXB_BORDERS ((size_t)EMBI_RENDER_MAX_BORDERS * sizeof(embi_border))
#define EMBI_MAXB_GRADIENTS ((size_t)EMBI_RENDER_MAX_GRADIENTS * sizeof(embi_gradient))
#define EMBI_MAXB_STOPS ((size_t)EMBI_RENDER_MAX_STOPS * sizeof(embi_stop))
static_assert(EMBI_MAXB_PRIMS <= (1u << 27) && EMBI_MAXB_XFORMS <= (1u << 27) &&
                  EMBI_MAXB_CLIPS <= (1u << 27) && EMBI_MAXB_BORDERS <= (1u << 27) &&
                  EMBI_MAXB_GRADIENTS <= (1u << 27) && EMBI_MAXB_STOPS <= (1u << 27),
              "storage buffers must fit maxStorageBufferRange");
static_assert(EMBI_RENDER_MAX_BORDERS < (1u << 24) && EMBI_RENDER_MAX_GRADIENTS < (1u << 24) &&
                  EMBI_ATLAS_MAX_SHARED < (1u << 24),
              "per-kind prim index is 24 bits");

typedef struct embi_gbuf {
    SDL_GPUBuffer *buf;
    uint32_t cap; /* bytes */
} embi_gbuf;

/* Pipelines a batch is drawn with. BOX is the ubershader with the source-over blend (every prim
 * but the region clear). CLEAR is the same shader writing over the target (the region clear of
 * embi_frame_begin_region). */
#define EMBI_PIPE_BOX 0u
#define EMBI_PIPE_CLEAR 1u
/* LCD text prims: the ubershader with a color write mask, drawn once per channel (R, G, B) over the
 * same batch. A batch of this pipe is drawn three times. */
#define EMBI_PIPE_LCD 2u
/* R4c: the ubershader with the destination-in blend (out = dst * src.a), for the composite of a mask layer. */
#define EMBI_PIPE_DSTIN 3u

typedef struct embi_batch {
    uint32_t first;
    uint32_t count;
    uint32_t tex; /* index into the frame's texture list, or EMBI_NO_TEX */
    uint8_t pipe; /* EMBI_PIPE_* */
} embi_batch;

/* A texture a frame samples. A solo page or a layer is its own texture (tex). The atlas array
 * (atlas) is named by its atlas and resolved when the frame ends, so a growth of the array during
 * the frame does not leave a stale texture in the frame. */
typedef struct embi_tex_ref {
    SDL_GPUTexture *tex;
    embi_atlas *atlas;
} embi_tex_ref;

/* Per prim, parallel to the prim array: the pipeline, and the texture slot the prim samples. */
typedef struct embi_ptag {
    uint32_t tex; /* index into the frame's texture list, or EMBI_NO_TEX */
    uint8_t pipe; /* EMBI_PIPE_* */
} embi_ptag;

struct embi_frame {
    embi_renderer *r;
    uint32_t w;
    uint32_t h;
    float scale;
    uint32_t degenerate_xform; /* index of a zero transform, or 0 when not created yet */
    bool overflow;             /* a cap was hit during recording; the frame draws nothing */
    bool region;              /* a partial redraw (embi_frame_begin_region) */
    bool region_clear;        /* set at embi_frame_end: the damage is cleared first */
    embi_text_aa text_aa;     /* text antialiasing of the frame */
    bool animating;           /* a transform animation runs: text under transforms is gray, unfitted */
    EMBI_VEC(uint8_t) xstill; /* per transform: 1 for a translation that no animation moves (a scroll offset): text keeps its fit */
    bool opaque_backdrop;     /* text may use LCD: the target is opaque under everything drawn */
    bool lcd_allowed;         /* per item switch (opacity < 1 turns it off) */
    uint32_t lcd_prims;       /* LCD text prims recorded */
    uint8_t blend_pipe;       /* R4c: the pipeline new prims are tagged with (EMBI_PIPE_BOX or EMBI_PIPE_DSTIN) */
    uint32_t dstin_prims;     /* R4c: prims recorded with the destination-in blend */
    embi_dev_rect damage;     /* region frames: clipped to the target, device px */

    EMBI_VEC(embi_prim) prims;
    EMBI_VEC(embi_ptag) tags; /* per prim: pipeline and texture slot (parallel to prims) */
    EMBI_VEC(embi_xform) xforms;
    EMBI_VEC(embi_clip) clips;
    EMBI_VEC(uint32_t) clip_depth; /* chain length including the clip; parallel to clips */
    uint32_t fold_parent[4];       /* recent parents embi_clip_fold ran for (0 = empty slot) */
    uint32_t fold_result[4];       /* what it returned for each, reused by sibling clips */
    uint32_t fold_next;            /* round-robin replacement slot */
    EMBI_VEC(embi_border) borders;
    EMBI_VEC(embi_gradient) gradients;
    EMBI_VEC(embi_stop) stops;
    EMBI_VEC(embi_tex_ref) texs; /* textures this frame samples; image prims index them */
};

struct embi_renderer {
    SDL_GPUDevice *dev;
    SDL_GPUGraphicsPipeline *pipeline;       /* the ubershader: box and outline prims */
    SDL_GPUGraphicsPipeline *clear_pipeline; /* the same shader, replacing blend (region clear) */
    embi_slug *slug;
    SDL_Thread *clear_thread; /* making the region clear pipeline; never joined by a full frame */
    SDL_Thread *warm_thread;  /* embi_renderer_warm: creating the first frame's buffers */
    SDL_AtomicInt buffer_creates; /* GPU buffers created by this renderer */
    embi_gpu *gpu;                           /* for the lazily made LCD pipelines */
    SDL_GPUGraphicsPipeline *lcd_pipeline[3]; /* write masks R, G, B; made on first use */
    bool clear_failed;                        /* the region clear pipeline could not be made: not retried */
    bool clear_fault;                         /* test hook: making it fails */
    bool warmed;                              /* embi_renderer_warm ran once */
    bool lcd_failed;                          /* making them failed once: not retried */
    SDL_GPUGraphicsPipeline *dstin_pipeline;  /* R4c: destination-in blend; made on first use */
    bool dstin_failed;
    embi_gbuf chan_buf[3];                   /* per channel pass: the subpixel offset (shader slot 7) */
    SDL_GPUTextureFormat target_format;
    SDL_GPUSampler *sampler;  /* linear, clamp to edge; binds with every texture */
    SDL_GPUTexture *blank;    /* 1x1 array, bound to batches that sample no texture (never read) */

    embi_frame frame;
    bool frame_open;

    SDL_GPUTransferBuffer *upload;
    uint32_t upload_cap;
    uint32_t upload_off; /* where the next frame's arrays go in upload (see embi_upload_map) */

    embi_gbuf xform_buf;
    embi_gbuf clip_buf;
    embi_gbuf border_buf;
    embi_gbuf gradient_buf;
    embi_gbuf stop_buf;
    embi_gbuf frame_buf;
    embi_gbuf slug_dummy;          /* bound as the slug buffer until the first blob exists */
    EMBI_VEC(embi_gbuf) prim_bufs; /* one per batch */
    EMBI_VEC(embi_batch) batches;

    uint32_t last_draws;
    uint32_t last_batches; /* batches and prims of the last frame that drew (instrumentation) */
    uint32_t last_prims;
};

/* ------------------------------------------------------------------ helpers */

static bool embi_finite(float x)
{
    return x - x == 0.0f;
}

static bool embi_rect_ok(const float rect[4])
{
    return embi_finite(rect[0]) && embi_finite(rect[1]) && embi_finite(rect[2]) &&
           embi_finite(rect[3]) && rect[2] > 0.0f && rect[3] > 0.0f;
}

static float embi_maxf(float a, float b)
{
    return a > b ? a : b;
}

static float embi_minf(float a, float b)
{
    return a < b ? a : b;
}

/* CSS corner radii, all scaled by one factor when adjacent radii overflow a side (CSS
 * Backgrounds 3, corner overlap). Circular radii only: x and y use the same value. After this
 * rule a single radius may exceed half the shorter side (`20px 0 0 0` on a 200x30 box); the shader
 * (sdRRectG) evaluates each corner independently and honors it. */
static void embi_scale_radii(const float rect[4], const float in[4], float out[4])
{
    float r[4];
    float f = 1.0f;
    for (int i = 0; i < 4; i++) {
        r[i] = embi_maxf(0.0f, embi_finite(in[i]) ? in[i] : 0.0f);
    }
    const float w = rect[2];
    const float h = rect[3];
    const float sums[4] = {r[0] + r[1], r[3] + r[2], r[0] + r[3], r[1] + r[2]};
    const float dims[4] = {w, w, h, h};
    for (int i = 0; i < 4; i++) {
        if (sums[i] > dims[i] && sums[i] > 0.0f) {
            const float s = dims[i] / sums[i];
            if (s < f) {
                f = s;
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        out[i] = r[i] * f;
    }
}

/* Smallest singular value of the 2x2 part (a c; b d), from the eigenvalues of M^T M. */
static float embi_sigma_min(float a, float b, float c, float d)
{
    const double ad = (double)a;
    const double bd = (double)b;
    const double cd = (double)c;
    const double dd = (double)d;
    const double tr = ad * ad + bd * bd + cd * cd + dd * dd;
    const double det = ad * dd - bd * cd;
    double disc = tr * tr - 4.0 * det * det;
    if (disc < 0.0) {
        disc = 0.0;
    }
    const double lmin = 0.5 * (tr - SDL_sqrt(disc));
    if (lmin <= 0.0) {
        return 0.0f;
    }
    return (float)SDL_sqrt(lmin);
}

static bool embi_xform_usable(const embi_frame *f, uint32_t xform)
{
    if (xform >= f->xforms.len) {
        return false;
    }
    return embi_vec_at(&f->xforms, xform).pad[0] > 0.0f;
}

/* Storage buffers and the transfer buffer --------------------------------- */

/* Largest size one SDL buffer can have (SDL sizes and offsets are Uint32). */
static bool embi_size_ok(size_t bytes, const char *what)
{
    if (bytes > UINT32_MAX) {
        embi_log_error("render: %s needs %llu bytes, over the 32-bit limit", what,
                       (unsigned long long)bytes);
        return false;
    }
    return true;
}

/* Grows a storage buffer to at least `bytes` (and at least one element). Growth is geometric but
 * clamped to max_bytes, which each caller derives from its element cap. That keeps every buffer
 * within Vulkan's guaranteed-minimum maxStorageBufferRange of 2^27 bytes. SDL keeps a released buffer alive until the work that uses it has finished. */
static bool embi_gbuf_ensure(SDL_AtomicInt *creates, SDL_GPUDevice *dev, embi_gbuf *b, size_t bytes, size_t max_bytes)
{
    if (bytes < 16) {
        bytes = 16;
    }
    if (!embi_size_ok(bytes, "storage buffer")) {
        return false;
    }
    if (bytes > max_bytes) {
        embi_log_error("render: storage buffer needs %llu bytes, over its cap of %llu",
                       (unsigned long long)bytes, (unsigned long long)max_bytes);
        return false;
    }
    if (b->buf != nullptr && b->cap >= bytes) {
        return true;
    }
    size_t want = bytes;
    if (b->cap != 0 && (size_t)b->cap * 2u > want) {
        want = (size_t)b->cap * 2u;
    }
    if (want > max_bytes) {
        want = max_bytes; /* never grow past what the element caps allow */
    }
    SDL_GPUBuffer *nb = SDL_CreateGPUBuffer(
        dev, &(SDL_GPUBufferCreateInfo){
                 .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                 .size = (Uint32)want,
             });
    if (nb == nullptr) {
        embi_log_error("render: SDL_CreateGPUBuffer(%llu bytes) failed: %s",
                       (unsigned long long)want, SDL_GetError());
        return false;
    }
    SDL_AddAtomicInt(creates, 1);
    if (b->buf != nullptr) {
        SDL_ReleaseGPUBuffer(dev, b->buf);
    }
    b->buf = nb;
    b->cap = (uint32_t)want;
    return true;
}

static void embi_gbuf_release(SDL_GPUDevice *dev, embi_gbuf *b)
{
    if (b->buf != nullptr) {
        SDL_ReleaseGPUBuffer(dev, b->buf);
    }
    b->buf = nullptr;
    b->cap = 0;
}

static size_t embi_align16(size_t n)
{
    return (n + 15u) & ~(size_t)15u;
}

static bool embi_upload_ensure(embi_renderer *r, size_t bytes)
{
    if (!embi_size_ok(bytes, "upload")) {
        return false;
    }
    if (r->upload != nullptr && r->upload_cap >= bytes) {
        return true;
    }
    size_t want = bytes;
    if ((size_t)r->upload_cap * 2u > want) {
        want = (size_t)r->upload_cap * 2u;
    }
    if (want > UINT32_MAX) {
        want = UINT32_MAX;
    }
    SDL_GPUTransferBuffer *nb = SDL_CreateGPUTransferBuffer(
        r->dev, &(SDL_GPUTransferBufferCreateInfo){
                    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                    .size = (Uint32)want,
                });
    if (nb == nullptr) {
        embi_log_error("render: SDL_CreateGPUTransferBuffer(%llu bytes) failed: %s",
                       (unsigned long long)want, SDL_GetError());
        return false;
    }
    SDL_AddAtomicInt(&r->buffer_creates, 1);
    if (r->upload != nullptr) {
        SDL_ReleaseGPUTransferBuffer(r->dev, r->upload);
    }
    r->upload = nb;
    r->upload_cap = (uint32_t)want;
    r->upload_off = 0;
    return true;
}

/* Maps room for bytes in the upload buffer (embi_upload_ensure made it at least that big) and gives its offset. The buffer is
 * a ring: a frame writes after the frames before it, without cycling, since the copies already recorded (in this command
 * buffer or in ones the GPU still runs) read only what lies before. When the rest of the buffer is too small, the map cycles
 * and the frame starts at 0: SDL then hands out an instance no work uses (made once, reused afterwards). Before, every frame
 * cycled: a page drawn with many layers records a frame per layer, and with frames in flight SDL created a new upload buffer
 * per layer and frame until there were enough (a few ms each time a buffer grew). */
static uint8_t *embi_upload_map(embi_renderer *r, size_t bytes, size_t *off)
{
    size_t start = r->upload_off;
    const bool cycle = start + bytes > r->upload_cap;
    if (cycle) {
        start = 0;
    }
    uint8_t *m = SDL_MapGPUTransferBuffer(r->dev, r->upload, cycle);
    if (m == nullptr) {
        return nullptr;
    }
    const size_t next = embi_align16(start + bytes);
    r->upload_off = next < r->upload_cap ? (uint32_t)next : r->upload_cap;
    *off = start;
    return m + start;
}

/* Pipeline ----------------------------------------------------------------- */

/* replace == true makes a pipeline that writes its output over the target (no blending), which
 * the region clear needs. */
static SDL_GPUGraphicsPipeline *embi_make_pipeline_vsfs(SDL_GPUDevice *dev, SDL_GPUTextureFormat format,
                                                        SDL_GPUShader *vs, SDL_GPUShader *fs,
                                                        bool replace, SDL_GPUColorComponentFlags mask, bool dstin)
{

    /* Premultiplied source over: out = src + dst * (1 - src.a). */
    const SDL_GPUColorTargetDescription color = {
        .format = format,
        .blend_state =
            {
                /* dstin (a mask layer): out = dst * src.a, colors and alpha alike. */
                .src_color_blendfactor = dstin ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE,
                .dst_color_blendfactor = dstin ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor = dstin ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE,
                .dst_alpha_blendfactor = dstin ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .color_write_mask = mask,
                .enable_color_write_mask = mask != 0,
                .enable_blend = !replace,
            },
    };
    const SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                             .cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &color, .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &info);
    if (p == nullptr) {
        embi_log_error("render: SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
    }
    return p;
}

static SDL_GPUGraphicsPipeline *embi_make_pipeline_mask(embi_gpu *g, SDL_GPUTextureFormat format,
                                                        const embi_shader_program *prog, bool replace,
                                                        SDL_GPUColorComponentFlags mask)
{
    SDL_GPUDevice *dev = embi_gpu_device(g);
    SDL_GPUShader *vs = embi_gpu_make_shader(g, &prog->vs, SDL_GPU_SHADERSTAGE_VERTEX);
    if (vs == nullptr) {
        return nullptr;
    }
    SDL_GPUShader *fs = embi_gpu_make_shader(g, &prog->fs, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (fs == nullptr) {
        SDL_ReleaseGPUShader(dev, vs);
        return nullptr;
    }
    SDL_GPUGraphicsPipeline *p = embi_make_pipeline_vsfs(dev, format, vs, fs, replace, mask, false);
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    return p;
}

/* The three LCD pipelines (write masks R, G, B) from one compile of the ubershader. False on failure
 * (nothing is kept). */
static bool embi_make_lcd_pipelines(embi_renderer *r)
{
    static const SDL_GPUColorComponentFlags masks[3] = {
        SDL_GPU_COLORCOMPONENT_R, SDL_GPU_COLORCOMPONENT_G, SDL_GPU_COLORCOMPONENT_B};
    SDL_GPUDevice *dev = embi_gpu_device(r->gpu);
    SDL_GPUShader *vs = embi_gpu_make_shader(r->gpu, &embi_shader_box.vs, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fs =
        vs != nullptr ? embi_gpu_make_shader(r->gpu, &embi_shader_box.fs, SDL_GPU_SHADERSTAGE_FRAGMENT)
                      : nullptr;
    bool ok = fs != nullptr;
    for (int i = 0; ok && i < 3; i++) {
        r->lcd_pipeline[i] = embi_make_pipeline_vsfs(dev, r->target_format, vs, fs, false, masks[i], false);
        ok = r->lcd_pipeline[i] != nullptr;
    }
    if (vs != nullptr) {
        SDL_ReleaseGPUShader(dev, vs);
    }
    if (fs != nullptr) {
        SDL_ReleaseGPUShader(dev, fs);
    }
    if (!ok) {
        for (int i = 0; i < 3; i++) {
            if (r->lcd_pipeline[i] != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(dev, r->lcd_pipeline[i]);
                r->lcd_pipeline[i] = nullptr;
            }
        }
    }
    return ok;
}

/* The destination-in pipeline (R4c). nullptr on failure. */
static SDL_GPUGraphicsPipeline *embi_make_dstin_pipeline(embi_renderer *r)
{
    SDL_GPUDevice *dev = embi_gpu_device(r->gpu);
    SDL_GPUShader *vs = embi_gpu_make_shader(r->gpu, &embi_shader_box.vs, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fs =
        vs != nullptr ? embi_gpu_make_shader(r->gpu, &embi_shader_box.fs, SDL_GPU_SHADERSTAGE_FRAGMENT) : nullptr;
    SDL_GPUGraphicsPipeline *p =
        fs != nullptr ? embi_make_pipeline_vsfs(dev, r->target_format, vs, fs, false, 0, true) : nullptr;
    if (vs != nullptr) {
        SDL_ReleaseGPUShader(dev, vs);
    }
    if (fs != nullptr) {
        SDL_ReleaseGPUShader(dev, fs);
    }
    return p;
}

static SDL_GPUGraphicsPipeline *embi_make_pipeline(embi_gpu *g, SDL_GPUTextureFormat format,
                                                   const embi_shader_program *prog, bool replace)
{
    return embi_make_pipeline_mask(g, format, prog, replace, 0);
}

/* Renderer lifetime -------------------------------------------------------- */

embi_renderer *embi_renderer_create(embi_gpu *g, SDL_GPUTextureFormat target_format)
{
    if (g == nullptr || target_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        return nullptr;
    }
    embi_renderer *r = embi_calloc(1, sizeof *r);
    r->dev = embi_gpu_device(g);
    r->gpu = g;
    r->target_format = target_format;
    r->frame.r = r;
    r->pipeline = embi_make_pipeline(g, target_format, &embi_shader_box, false);
    /* The replacing (region clear) pipeline is made by the warm-up or, failing that, by the first region frame: a
     * full frame never draws with it. */
    r->slug = embi_slug_create(r->dev);
    /* The sampler and the 1x1 blank texture are made by the warm-up, or by the first frame when there was none: only
     * frames use them, and a committed resource costs about 0.4 ms. */
    if (r->pipeline == nullptr || r->slug == nullptr) {
        embi_log_error("render: renderer setup failed: %s", SDL_GetError());
        embi_renderer_destroy(r);
        return nullptr;
    }
    return r;
}

/* What the warm-up creates: room for a small page (the numbers are of records, not bytes). A first frame
 * that needs more grows the buffer inside embi_frame_end, as every frame does. */
#define EMBI_WARM_PRIMS 4096u
#define EMBI_WARM_XFORMS 256u
#define EMBI_WARM_CLIPS 256u
#define EMBI_WARM_BORDERS 128u
#define EMBI_WARM_GRADIENTS 64u
#define EMBI_WARM_STOPS 256u

/* The replacing pipeline of the region clear: made once, and a failure is remembered. */
static bool embi_clear_make(embi_renderer *r)
{
    if (r->clear_pipeline != nullptr) {
        return true;
    }
    if (r->clear_failed) {
        return false;
    }
    if (!r->clear_fault) {
        r->clear_pipeline = embi_make_pipeline(r->gpu, r->target_format, &embi_shader_box, true);
    }
    if (r->clear_pipeline == nullptr) {
        r->clear_failed = true;
        embi_log_error("render: the region clear pipeline could not be made; region frames are drawn as full frames");
        return false;
    }
    return true;
}

static void embi_clear_wait(embi_renderer *r)
{
    if (r->clear_thread != nullptr) {
        SDL_WaitThread(r->clear_thread, nullptr);
        r->clear_thread = nullptr;
    }
}

/* Whether the pipeline exists, making it here if it is not made yet. Waits for the thread that is making it. */
static bool embi_clear_pipeline_ensure(embi_renderer *r)
{
    embi_clear_wait(r);
    return embi_clear_make(r);
}

static int SDLCALL embi_clear_main(void *arg)
{
    (void)embi_clear_make(arg);
    return 0;
}

/* The sampler and the blank texture every frame binds. Idempotent; false when one cannot be made. */
static bool embi_aux_make(embi_renderer *r)
{
    if (r->sampler == nullptr) {
        r->sampler = SDL_CreateGPUSampler(
            r->dev, &(SDL_GPUSamplerCreateInfo){
                        .min_filter = SDL_GPU_FILTER_LINEAR,
                        .mag_filter = SDL_GPU_FILTER_LINEAR,
                        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                    });
    }
    if (r->blank == nullptr) {
        r->blank = SDL_CreateGPUTexture(
            r->dev, &(SDL_GPUTextureCreateInfo){
                        .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
                        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                        .width = 1,
                        .height = 1,
                        .layer_count_or_depth = 1,
                        .num_levels = 1,
                        .sample_count = SDL_GPU_SAMPLECOUNT_1,
                    });
    }
    return r->sampler != nullptr && r->blank != nullptr;
}

static int SDLCALL embi_warm_main(void *arg)
{
    embi_renderer *r = arg;
    SDL_GPUDevice *dev = r->dev;
    SDL_AtomicInt *c = &r->buffer_creates;
    (void)embi_aux_make(r); /* a failure is met again by embi_frame_end */
    /* The same buffers embi_frame_end ensures, in its order. A failure is left for it to retry. */
    if (r->prim_bufs.len == 0) {
        const embi_gbuf zero = {0};
        embi_vec_push(&r->prim_bufs, zero);
    }
    bool ok = embi_gbuf_ensure(c, dev, &r->prim_bufs.data[0], (size_t)EMBI_WARM_PRIMS * sizeof(embi_prim), EMBI_MAXB_PRIMS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->xform_buf, (size_t)EMBI_WARM_XFORMS * sizeof(embi_xform), EMBI_MAXB_XFORMS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->clip_buf, (size_t)EMBI_WARM_CLIPS * sizeof(embi_clip), EMBI_MAXB_CLIPS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->border_buf, (size_t)EMBI_WARM_BORDERS * sizeof(embi_border), EMBI_MAXB_BORDERS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->gradient_buf, (size_t)EMBI_WARM_GRADIENTS * sizeof(embi_gradient), EMBI_MAXB_GRADIENTS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->stop_buf, (size_t)EMBI_WARM_STOPS * sizeof(embi_stop), EMBI_MAXB_STOPS);
    ok = ok && embi_gbuf_ensure(c, dev, &r->frame_buf, sizeof(embi_frame_params), 256);
    ok = ok && embi_gbuf_ensure(c, dev, &r->slug_dummy, 16, 16);
    for (int i = 0; i < 3; i++) {
        ok = ok && embi_gbuf_ensure(c, dev, &r->chan_buf[i], 16, 16);
    }
    if (ok) {
        /* Big enough for the arrays above laid end to end, as embi_frame_end lays them in the upload buffer. */
        const size_t total = embi_align16((size_t)EMBI_WARM_PRIMS * sizeof(embi_prim)) +
                             embi_align16((size_t)EMBI_WARM_XFORMS * sizeof(embi_xform)) +
                             embi_align16((size_t)EMBI_WARM_CLIPS * sizeof(embi_clip)) +
                             embi_align16((size_t)EMBI_WARM_BORDERS * sizeof(embi_border)) +
                             embi_align16((size_t)EMBI_WARM_GRADIENTS * sizeof(embi_gradient)) +
                             embi_align16((size_t)EMBI_WARM_STOPS * sizeof(embi_stop)) + 256u;
        ok = embi_upload_ensure(r, total);
    }
    SDL_AddAtomicInt(c, (int)embi_slug_prewarm(r->slug));
    return 0; /* a failure here is left for embi_frame_end to meet and report */
}

void embi_renderer_warm(embi_renderer *r)
{
    if (r == nullptr || r->warm_thread != nullptr || r->warmed) {
        return; /* once per renderer */
    }
    r->warmed = true;
    /* The region clear pipeline is on a thread of its own: a full frame does not need it, so the first frame must not
     * wait for its compile (slow on a software driver). The first region frame does wait for it. */
    r->clear_thread = SDL_CreateThread(embi_clear_main, "ember-gpu-clear", r);
    if (r->clear_thread == nullptr) {
        (void)embi_clear_make(r);
    }
    r->warm_thread = SDL_CreateThread(embi_warm_main, "ember-gpu-warm", r);
    if (r->warm_thread == nullptr) {
        (void)embi_warm_main(r); /* no thread: the same work, here */
    }
}

void embi_renderer_warm_wait(embi_renderer *r)
{
    if (r != nullptr && r->warm_thread != nullptr) {
        SDL_WaitThread(r->warm_thread, nullptr);
        r->warm_thread = nullptr;
    }
}

bool embi_renderer_region_ok(embi_renderer *r)
{
    embi_renderer_warm_wait(r);
    return embi_clear_pipeline_ensure(r);
}

void embi_renderer_fault_clear_pipeline(embi_renderer *r)
{
    embi_renderer_warm_wait(r);
    embi_clear_wait(r);
    if (r->clear_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(r->dev, r->clear_pipeline);
        r->clear_pipeline = nullptr;
    }
    r->clear_failed = false;
    r->clear_fault = true;
}

uint32_t embi_renderer_buffer_creates(const embi_renderer *r)
{
    return (uint32_t)SDL_GetAtomicInt((SDL_AtomicInt *)(uintptr_t)&r->buffer_creates);
}

uint32_t embi_renderer_outline_blobs(const embi_renderer *r)
{
    return r != nullptr ? embi_slug_get_stats(r->slug).blobs : 0;
}

void embi_renderer_destroy(embi_renderer *r)
{
    if (r == nullptr) {
        return;
    }
    embi_renderer_warm_wait(r);
    embi_clear_wait(r);
    SDL_GPUDevice *dev = r->dev;
    SDL_WaitForGPUIdle(dev);
    if (r->sampler != nullptr) {
        SDL_ReleaseGPUSampler(dev, r->sampler);
    }
    if (r->blank != nullptr) {
        SDL_ReleaseGPUTexture(dev, r->blank);
    }
    for (uint32_t i = 0; i < r->prim_bufs.len; i++) {
        embi_gbuf_release(dev, &r->prim_bufs.data[i]);
    }
    embi_vec_free(&r->prim_bufs);
    embi_vec_free(&r->batches);
    embi_gbuf_release(dev, &r->xform_buf);
    embi_gbuf_release(dev, &r->clip_buf);
    embi_gbuf_release(dev, &r->border_buf);
    embi_gbuf_release(dev, &r->gradient_buf);
    embi_gbuf_release(dev, &r->stop_buf);
    embi_gbuf_release(dev, &r->frame_buf);
    embi_gbuf_release(dev, &r->slug_dummy);
    for (int i = 0; i < 3; i++) {
        embi_gbuf_release(dev, &r->chan_buf[i]);
        if (r->lcd_pipeline[i] != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(dev, r->lcd_pipeline[i]);
        }
    }
    if (r->upload != nullptr) {
        SDL_ReleaseGPUTransferBuffer(dev, r->upload);
    }
    if (r->pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev, r->pipeline);
    }
    if (r->clear_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev, r->clear_pipeline);
    }
    if (r->dstin_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev, r->dstin_pipeline);
    }
    embi_slug_destroy(r->slug);
    embi_vec_free(&r->frame.prims);
    embi_vec_free(&r->frame.tags);
    embi_vec_free(&r->frame.xforms);
    embi_vec_free(&r->frame.xstill);
    embi_vec_free(&r->frame.clips);
    embi_vec_free(&r->frame.clip_depth);
    embi_vec_free(&r->frame.borders);
    embi_vec_free(&r->frame.gradients);
    embi_vec_free(&r->frame.stops);
    embi_vec_free(&r->frame.texs);
    embi_free(r, sizeof *r);
}

uint32_t embi_renderer_last_draw_calls(const embi_renderer *r)
{
    return r->last_draws;
}

uint32_t embi_renderer_last_batches(const embi_renderer *r)
{
    return r->last_batches;
}

uint32_t embi_renderer_last_prims(const embi_renderer *r)
{
    return r->last_prims;
}

/* Frame recording ---------------------------------------------------------- */

/* Pushes a transform, or returns 0 and marks the frame overflowed. */
static uint32_t embi_xform_push(embi_frame *f, const embi_xform *x)
{
    if (f->xforms.len >= EMBI_RENDER_MAX_XFORMS) {
        f->overflow = true;
        return 0;
    }
    embi_vec_push(&f->xforms, *x);
    return f->xforms.len - 1;
}

/* Index of a zero transform. Anything clipped by it is invisible. */
static uint32_t embi_frame_degenerate_xform(embi_frame *f)
{
    if (f->degenerate_xform == 0) {
        const embi_xform z = {0};
        f->degenerate_xform = embi_xform_push(f, &z);
    }
    return f->degenerate_xform;
}

/* Empties the frame's tables and pushes the entries every frame starts with: xform 0 (device
 * scale only) and clip 0 (the "no clip" sentinel). A region frame also starts with its clear prim
 * (prim 0, pipeline EMBI_PIPE_CLEAR), which covers the damage plus a 1 px margin; the clear color
 * is set at embi_frame_end, and the prim is dropped there unless the frame clears. */
static void embi_frame_reset(embi_frame *f)
{
    f->degenerate_xform = 0;
    for (int i = 0; i < 4; i++) {
        f->fold_parent[i] = 0;
        f->fold_result[i] = 0;
    }
    f->fold_next = 0;
    f->overflow = false;
    f->region_clear = false;
    f->text_aa = EMBI_TEXT_AA_GRAY;
    f->animating = false;
    embi_vec_clear(&f->xstill);
    f->opaque_backdrop = false;
    f->lcd_allowed = true;
    f->lcd_prims = 0;
    f->blend_pipe = EMBI_PIPE_BOX;
    f->dstin_prims = 0;
    embi_vec_clear(&f->prims);
    embi_vec_clear(&f->tags);
    embi_vec_clear(&f->xforms);
    embi_vec_clear(&f->clips);
    embi_vec_clear(&f->clip_depth);
    embi_vec_clear(&f->borders);
    embi_vec_clear(&f->gradients);
    embi_vec_clear(&f->stops);
    embi_vec_clear(&f->texs);

    /* Xform 0: device scale only. Clip 0: the sentinel for "no clip". */
    const float id[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_frame_xform(f, id);
    embi_vec_push(&f->clips, ((embi_clip){0}));
    embi_vec_push(&f->clip_depth, 0u);

    if (f->region) {
        const float s = f->scale;
        embi_prim clear = {0};
        clear.rect[0] = ((float)f->damage.x - 1.0f) / s;
        clear.rect[1] = ((float)f->damage.y - 1.0f) / s;
        clear.rect[2] = ((float)f->damage.w + 2.0f) / s;
        clear.rect[3] = ((float)f->damage.h + 2.0f) / s;
        clear.kind = EMBI_PRIM_FILL;
        clear.xform = 0;
        clear.clip = 0;
        embi_vec_push(&f->prims, clear);
        const embi_ptag tag = {.tex = EMBI_NO_TEX, .pipe = EMBI_PIPE_CLEAR};
        embi_vec_push(&f->tags, tag);
    }
}

static embi_frame *embi_frame_open(embi_renderer *r, uint32_t width_px, uint32_t height_px,
                                   float device_scale, bool region, embi_dev_rect damage)
{
    if (r == nullptr || r->frame_open) {
        embi_log_error("render: embi_frame_begin: renderer missing or a frame is already open");
        return nullptr;
    }
    if (width_px == 0 || height_px == 0 || width_px > EMBI_RENDER_MAX_DIM ||
        height_px > EMBI_RENDER_MAX_DIM || !embi_finite(device_scale) || device_scale <= 0.0f) {
        return nullptr;
    }

    embi_frame *f = &r->frame;
    f->w = width_px;
    f->h = height_px;
    f->scale = device_scale;
    f->region = region;
    f->damage = damage;
    embi_frame_reset(f);

    embi_slug_begin_frame(r->slug);
    r->frame_open = true;
    return f;
}

embi_frame *embi_frame_begin(embi_renderer *r, uint32_t width_px, uint32_t height_px,
                             float device_scale)
{
    const embi_dev_rect none = {0, 0, 0, 0};
    return embi_frame_open(r, width_px, height_px, device_scale, false, none);
}

embi_frame *embi_frame_begin_region(embi_renderer *r, uint32_t width_px, uint32_t height_px,
                                    float device_scale, embi_dev_rect damage)
{
    /* Clip the damage to the target, in 64-bit so no sum can wrap. */
    const int64_t x0 = damage.x < 0 ? 0 : (int64_t)damage.x;
    const int64_t y0 = damage.y < 0 ? 0 : (int64_t)damage.y;
    const int64_t x1 = (int64_t)damage.x + (int64_t)damage.w;
    const int64_t y1 = (int64_t)damage.y + (int64_t)damage.h;
    const int64_t cx1 = x1 < (int64_t)width_px ? x1 : (int64_t)width_px;
    const int64_t cy1 = y1 < (int64_t)height_px ? y1 : (int64_t)height_px;
    embi_dev_rect d = {0, 0, 0, 0};
    if (cx1 > x0 && cy1 > y0) {
        d.x = (int32_t)x0;
        d.y = (int32_t)y0;
        d.w = (int32_t)(cx1 - x0);
        d.h = (int32_t)(cy1 - y0);
    } else {
        d.x = (int32_t)x0;
        d.y = (int32_t)y0;
    }
    return embi_frame_open(r, width_px, height_px, device_scale, true, d);
}

uint32_t embi_frame_xform(embi_frame *f, const float m[6])
{
    embi_xform x = {0};
    const float s = f->scale;
    for (int i = 0; i < 6; i++) {
        x.m[i] = embi_finite(m[i]) ? m[i] * s : 0.0f;
    }
    /* The pads are only meaningful for an invertible map. A degenerate map keeps zero pads. */
    if (embi_sigma_min(x.m[0], x.m[1], x.m[2], x.m[3]) > EMBI_RENDER_SIGMA_MIN) {
        const double a = x.m[0];
        const double b = x.m[1];
        const double c = x.m[2];
        const double d = x.m[3];
        const double det = a * d - b * c;
        double p0 = SDL_sqrt(c * c + d * d) / SDL_fabs(det);
        double p1 = SDL_sqrt(a * a + b * b) / SDL_fabs(det);
        /* A near-singular map has huge pads (up to 1 / sigma_min), so each prim's quad would
         * cover far more than the viewport. Cap the pad so the expansion moves a vertex by at
         * most one viewport extent in device space. Past that cap the fringe of edges that far
         * off-screen is lost, which is invisible. The cap is far above any real pad. */
        const double tr = a * a + b * b + c * c + d * d; /* >= sigma_max^2 */
        const double extent = (double)(f->w > f->h ? f->w : f->h) + 2.0;
        const double cap = extent / SDL_sqrt(tr);
        p0 = p0 < cap ? p0 : cap;
        p1 = p1 < cap ? p1 : cap;
        x.pad[0] = (float)p0;
        x.pad[1] = (float)p1;
        if (!(x.pad[0] > 0.0f) || !(x.pad[1] > 0.0f)) {
            x.pad[0] = x.pad[1] = 0.0f; /* underflow: treat as degenerate */
        }
    }

    /* The device-scale identity is always index 0. */
    if (f->xforms.len > 0 && SDL_memcmp(&embi_vec_at(&f->xforms, 0), &x, sizeof x) == 0) {
        return 0;
    }
    /* Dedup against the most recent entry. */
    if (f->xforms.len > 1 &&
        SDL_memcmp(&embi_vec_at(&f->xforms, f->xforms.len - 1), &x, sizeof x) == 0) {
        return f->xforms.len - 1;
    }
    return embi_xform_push(f, &x);
}

void embi_frame_xform_set_still(embi_frame *f, uint32_t xform)
{
    if (f == nullptr || xform == 0 || xform >= f->xforms.len) {
        return;
    }
    while (f->xstill.len <= xform) {
        embi_vec_push(&f->xstill, (uint8_t)0);
    }
    embi_vec_at(&f->xstill, xform) = 1;
}

/* Clips ------------------------------------------------------------------- */

static uint32_t embi_clip_push(embi_frame *f, const embi_clip *c, uint32_t depth)
{
    if (f->clips.len >= EMBI_RENDER_MAX_CLIPS) {
        f->overflow = true;
        return 0;
    }
    embi_vec_push(&f->clips, *c);
    embi_vec_push(&f->clip_depth, depth);
    return f->clips.len - 1;
}

static bool embi_clip_plain(const embi_clip *c)
{
    return c->radii[0] == 0.0f && c->radii[1] == 0.0f && c->radii[2] == 0.0f &&
           c->radii[3] == 0.0f;
}

/* Two adjacent levels can be intersected exactly: both are plain rects in the same usable
 * space. */
static bool embi_clip_exact_pair(const embi_frame *f, const embi_clip *a, const embi_clip *b)
{
    return embi_clip_plain(a) && embi_clip_plain(b) && a->xform == b->xform &&
           embi_xform_usable(f, a->xform);
}

/* Device-space bounds of a clip's rect: x0, y0, x1, y1. */
static void embi_clip_device_bounds(const embi_frame *f, const embi_clip *c, float out[4])
{
    const float *m = embi_vec_at(&f->xforms, c->xform).m;
    const float xs[2] = {c->rect[0], c->rect[0] + c->rect[2]};
    const float ys[2] = {c->rect[1], c->rect[1] + c->rect[3]};
    for (int i = 0; i < 4; i++) {
        const float px = xs[i & 1];
        const float py = ys[i >> 1];
        const float dx = m[0] * px + m[2] * py + m[4];
        const float dy = m[1] * px + m[3] * py + m[5];
        if (i == 0) {
            out[0] = out[2] = dx;
            out[1] = out[3] = dy;
        } else {
            out[0] = embi_minf(out[0], dx);
            out[1] = embi_minf(out[1], dy);
            out[2] = embi_maxf(out[2], dx);
            out[3] = embi_maxf(out[3], dy);
        }
    }
}

/* A clip that hides everything: an empty rect under the zero transform. */
static embi_clip embi_clip_hidden(embi_frame *f)
{
    embi_clip c = {0};
    c.xform = embi_frame_degenerate_xform(f);
    return c;
}

/* Intersection of two clip levels as one level. Exact for two plain rects in the same usable
 * space. Otherwise an xform-0 rect that holds the intersection of their device bounds. */
static embi_clip embi_clip_merge(embi_frame *f, const embi_clip *a, const embi_clip *b)
{
    if (embi_clip_exact_pair(f, a, b)) {
        const float x0 = embi_maxf(a->rect[0], b->rect[0]);
        const float y0 = embi_maxf(a->rect[1], b->rect[1]);
        const float x1 = embi_minf(a->rect[0] + a->rect[2], b->rect[0] + b->rect[2]);
        const float y1 = embi_minf(a->rect[1] + a->rect[3], b->rect[1] + b->rect[3]);
        if (x1 <= x0 || y1 <= y0) {
            return embi_clip_hidden(f);
        }
        embi_clip c = {0};
        c.rect[0] = x0;
        c.rect[1] = y0;
        c.rect[2] = x1 - x0;
        c.rect[3] = y1 - y0;
        c.xform = a->xform;
        return c;
    }

    float ba[4];
    float bb[4];
    embi_clip_device_bounds(f, a, ba);
    embi_clip_device_bounds(f, b, bb);
    const float x0 = embi_maxf(ba[0], bb[0]);
    const float y0 = embi_maxf(ba[1], bb[1]);
    const float x1 = embi_minf(ba[2], bb[2]);
    const float y1 = embi_minf(ba[3], bb[3]);
    if (x1 <= x0 || y1 <= y0) {
        return embi_clip_hidden(f);
    }
    /* xform 0 is device scale only, so device px divided by the scale are CSS px. */
    const float s = f->scale;
    embi_clip c = {0};
    c.rect[0] = x0 / s;
    c.rect[1] = y0 / s;
    c.rect[2] = (x1 - x0) / s;
    c.rect[3] = (y1 - y0) / s;
    c.xform = 0;
    return c;
}

/* Level `outer` adds nothing under level `inner` when every pixel that `inner` can touch lies
 * deep inside `outer`, where outer's coverage is exactly 1. That holds when outer's transform
 * is axis-aligned in device space (no rotation or skew), so outer's rounded rect has a plain
 * device rect and a known largest corner radius. inner's device bounds, grown by the 0.5 px
 * anti-aliasing reach, must lie inside outer's device rect inset by the largest corner radius
 * plus 0.5 px (the point where outer's own fringe ends). inner may use any transform. */
static bool embi_clip_redundant(const embi_frame *f, const embi_clip *outer, const embi_clip *inner)
{
    if (!embi_xform_usable(f, outer->xform) || !embi_xform_usable(f, inner->xform)) {
        return false;
    }
    const float *m = embi_vec_at(&f->xforms, outer->xform).m;
    if (m[1] != 0.0f || m[2] != 0.0f) {
        return false;
    }
    float rmax = 0.0f;
    for (int i = 0; i < 4; i++) {
        rmax = embi_maxf(rmax, outer->radii[i]);
    }
    const float scale = embi_maxf(SDL_fabsf(m[0]), SDL_fabsf(m[3]));
    const float inset = embi_maxf(rmax * scale, 0.5f) + 0.5f + 0.01f;
    float bo[4];
    float bi[4];
    embi_clip_device_bounds(f, outer, bo);
    embi_clip_device_bounds(f, inner, bi);
    return bi[0] - 0.5f >= bo[0] + inset && bi[1] - 0.5f >= bo[1] + inset &&
           bi[2] + 0.5f <= bo[2] - inset && bi[3] + 0.5f <= bo[3] - inset;
}

/* Called when `leaf` already has EMBI_MAX_CLIP_DEPTH ancestors-plus-self, so a child would be one
 * level too deep. Returns a parent of depth EMBI_MAX_CLIP_DEPTH - 1 that clips exactly like
 * the old chain, except where the last resort (a non-exact merge) was needed. Returns 0 on
 * overflow. In order of preference, the outermost of:
 *   1. two adjacent plain rects in one space: intersected (exact)
 *   2. a level that is redundant under a deeper one (embi_clip_redundant): dropped (exact)
 *   3. the two outermost levels: one xform-0 rect of their device bounds (may leak at rounded or
 *      rotated corners of the outer level) */
static uint32_t embi_clip_fold(embi_frame *f, uint32_t leaf)
{
    enum { N = EMBI_MAX_CLIP_DEPTH };
    uint32_t chain[N]; /* chain[k] is the ancestor at depth k + 1; chain[N - 1] is the leaf */
    uint32_t c = leaf;
    for (uint32_t k = N; k-- > 0;) {
        chain[k] = c;
        c = embi_vec_at(&f->clips, c).parent;
    }

    /* Choose what to do. j is the first level that changes. */
    enum { MERGE_PAIR, DROP, MERGE_BOUNDS } mode = MERGE_BOUNDS;
    uint32_t j = 0;
    for (uint32_t k = 0; k + 1 < N; k++) {
        if (embi_clip_exact_pair(f, &embi_vec_at(&f->clips, chain[k]),
                                 &embi_vec_at(&f->clips, chain[k + 1]))) {
            mode = MERGE_PAIR;
            j = k;
            break;
        }
    }
    for (uint32_t k = 0; k + 1 < N && (mode == MERGE_BOUNDS || k < j); k++) {
        bool red = false;
        for (uint32_t m = k + 1; m < N && !red; m++) {
            red = embi_clip_redundant(f, &embi_vec_at(&f->clips, chain[k]),
                                      &embi_vec_at(&f->clips, chain[m]));
        }
        if (red) {
            mode = DROP;
            j = k;
            break;
        }
    }

    /* Keep chain[0..j-1] as they are. Replace chain[j] (and chain[j+1] for a merge) and copy the
     * levels below them, re-parented, so the stored levels keep their own shapes. */
    uint32_t prev = (j == 0) ? 0u : chain[j - 1];
    uint32_t depth = (j == 0) ? 0u : embi_vec_at(&f->clip_depth, chain[j - 1]);
    uint32_t next = j + 1;
    if (mode != DROP) {
        const embi_clip ca = embi_vec_at(&f->clips, chain[j]);
        const embi_clip cb = embi_vec_at(&f->clips, chain[j + 1]);
        embi_clip merged = embi_clip_merge(f, &ca, &cb);
        merged.parent = prev;
        prev = embi_clip_push(f, &merged, ++depth);
        if (prev == 0) {
            return 0;
        }
        next = j + 2;
    }
    for (uint32_t k = next; k < N; k++) {
        embi_clip copy = embi_vec_at(&f->clips, chain[k]);
        copy.parent = prev;
        prev = embi_clip_push(f, &copy, ++depth);
        if (prev == 0) {
            return 0;
        }
    }
    return prev;
}

uint32_t embi_frame_clip(embi_frame *f, const float rect[4], const float radii[4], uint32_t xform,
                         uint32_t parent)
{
    if (xform >= f->xforms.len) {
        xform = 0;
    }
    if (parent >= f->clips.len) {
        parent = 0;
    }
    if (parent != 0 && embi_vec_at(&f->clip_depth, parent) >= EMBI_MAX_CLIP_DEPTH) {
        /* Siblings under one deep parent share one folded replacement. Clips never change after
         * they are pushed, so the fold of a given parent is stable within the frame. */
        int slot = -1;
        for (int i = 0; i < 4; i++) {
            if (f->fold_parent[i] == parent) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            const uint32_t folded = embi_clip_fold(f, parent);
            if (folded == 0) {
                return 0; /* overflow: the frame draws nothing */
            }
            slot = (int)(f->fold_next++ & 3u);
            f->fold_parent[slot] = parent;
            f->fold_result[slot] = folded;
        }
        parent = f->fold_result[slot];
    }
    const uint32_t depth = (parent == 0) ? 1u : embi_vec_at(&f->clip_depth, parent) + 1u;

    embi_clip c = {0};
    if (embi_rect_ok(rect)) {
        for (int i = 0; i < 4; i++) {
            c.rect[i] = rect[i];
        }
        embi_scale_radii(rect, radii, c.radii);
        c.xform = xform;
    } else {
        /* An empty clip hides everything it covers: give it a zero transform. */
        c.xform = embi_frame_degenerate_xform(f);
    }
    c.parent = parent;
    return embi_clip_push(f, &c, depth);
}

/* Prims ------------------------------------------------------------------- */

/* Every prim goes through here, so prims and their tags stay in step. tex is the texture slot
 * the prim samples (EMBI_NO_TEX for none). */
static void embi_frame_push_prim(embi_frame *f, const embi_prim *p, uint32_t tex)
{
    embi_vec_push(&f->prims, *p);
    const embi_ptag tag = {.tex = tex, .pipe = f->blend_pipe};
    if (f->blend_pipe == EMBI_PIPE_DSTIN) {
        f->dstin_prims++;
    }
    embi_vec_push(&f->tags, tag);
}

/* Region frames only: true when the local rect (x, y, w, h) under xform cannot touch the damage
 * rect. The device bounds of the rect grow by 2 px, which covers the anti-aliasing fringe and the
 * pads, so a prim is dropped only when it is entirely outside. Always false for other frames. */
static bool embi_region_culled(const embi_frame *f, float x, float y, float w, float h,
                               uint32_t xform)
{
    if (!f->region) {
        return false;
    }
    const float *m = embi_vec_at(&f->xforms, xform).m;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float px = x + ((i & 1) ? w : 0.0f);
        const float py = y + ((i & 2) ? h : 0.0f);
        const float dx = m[0] * px + m[2] * py + m[4];
        const float dy = m[1] * px + m[3] * py + m[5];
        if (i == 0) {
            x0 = x1 = dx;
            y0 = y1 = dy;
        } else {
            x0 = embi_minf(x0, dx);
            x1 = embi_maxf(x1, dx);
            y0 = embi_minf(y0, dy);
            y1 = embi_maxf(y1, dy);
        }
    }
    const float dx0 = (float)f->damage.x - 2.0f;
    const float dy0 = (float)f->damage.y - 2.0f;
    const float dx1 = (float)((int64_t)f->damage.x + f->damage.w) + 2.0f;
    const float dy1 = (float)((int64_t)f->damage.y + f->damage.h) + 2.0f;
    return x1 < dx0 || x0 > dx1 || y1 < dy0 || y0 > dy1;
}

void embi_frame_fill(embi_frame *f, const float rect[4], const float radii[4], uint32_t rgba,
                     uint32_t xform, uint32_t clip)
{
    if (!embi_rect_ok(rect) || (rgba >> 24) == 0u || !embi_xform_usable(f, xform) ||
        clip >= f->clips.len) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS) {
        f->overflow = true;
        return;
    }
    if (embi_region_culled(f, rect[0], rect[1], rect[2], rect[3], xform)) {
        return;
    }
    embi_prim p = {0};
    for (int i = 0; i < 4; i++) {
        p.rect[i] = rect[i];
    }
    embi_scale_radii(rect, radii, p.radii);
    p.color = rgba;
    p.kind = EMBI_PRIM_FILL;
    p.xform = xform;
    p.clip = clip;
    embi_frame_push_prim(f, &p, EMBI_NO_TEX);
}

/* The squared distance of two colors' RGB. */
static uint32_t embi_rgb_dist2(uint32_t a, uint32_t b)
{
    uint32_t d = 0;
    for (int i = 0; i < 24; i += 8) {
        int x = (int)((a >> i) & 255u) - (int)((b >> i) & 255u);
        d += (uint32_t)(x * x);
    }
    return d;
}

/* One shade of a color for the sides of inset, outset, groove and ridge borders, as Chromium computes them
 * (BoxBorderPainter, Color::Dark and Color::Light). The dark side scales each channel by (v - 0.33) / v, v being the
 * largest channel in [0, 1], unless its distance from black is at most that of #202020; the light side by min(1, v + 0.33) / v,
 * black becoming #545454, unless its distance from white is at most that of #EBEBEB (red stays red, gray gets lighter, black
 * grooves are visible). Alpha is kept. */
static uint32_t embi_shade(uint32_t c, bool dark)
{
    if (dark ? embi_rgb_dist2(c, 0u) <= embi_rgb_dist2(0x202020u, 0u) : embi_rgb_dist2(c, 0xFFFFFFu) <= embi_rgb_dist2(0xEBEBEBu, 0xFFFFFFu)) {
        return c;
    }
    const uint32_t ch[3] = {c & 255u, (c >> 8) & 255u, (c >> 16) & 255u};
    uint32_t mx = ch[0];
    if (ch[1] > mx) {
        mx = ch[1];
    }
    if (ch[2] > mx) {
        mx = ch[2];
    }
    const float v = (float)mx / 255.0f;
    uint32_t out = c & 0xff000000u;
    if (!dark && mx == 0) {
        return out | 0x545454u;
    }
    float m;
    if (dark) {
        m = (v > 0.0f) ? embi_maxf(0.0f, (v - 0.33f) / v) : 0.0f;
    } else {
        m = (v + 0.33f < 1.0f ? v + 0.33f : 1.0f) / v;
    }
    for (int i = 0; i < 3; i++) {
        float x = (float)ch[i] * m + 0.5f;
        if (x > 255.0f) {
            x = 255.0f;
        }
        out |= (uint32_t)x << (8 * i);
    }
    return out;
}

void embi_frame_border_styled(embi_frame *f, const float rect[4], const float radii[4],
                              const float widths[4], const uint32_t colors[4],
                              const uint32_t styles[4], uint32_t xform, uint32_t clip)
{
    if (!embi_rect_ok(rect) || !embi_xform_usable(f, xform) || clip >= f->clips.len ||
        widths == nullptr || colors == nullptr) {
        return;
    }
    embi_border b = {0};
    bool visible = false;
    for (int i = 0; i < 4; i++) {
        const uint32_t st = styles != nullptr ? styles[i] : EMBI_BSTYLE_SOLID;
        float w = (embi_finite(widths[i]) && widths[i] > 0.0f) ? widths[i] : 0.0f;
        if (st == EMBI_BSTYLE_NONE) {
            w = 0.0f; /* none paints nothing; an unknown style value is drawn as solid */
        }
        /* Top and left (sides 0 and 3) are the "lit" sides for the 3D styles. */
        const bool lead = (i == 0 || i == 3);
        const uint32_t c = colors[i];
        const uint32_t dk = embi_shade(c, true);
        const uint32_t lt = embi_shade(c, false);
        uint32_t outer = c;
        uint32_t inner = c;
        uint32_t pat = EMBI_BPAT_SOLID;
        switch (st) {
        case EMBI_BSTYLE_DOTTED:
            pat = EMBI_BPAT_DOTTED;
            break;
        case EMBI_BSTYLE_DASHED:
            pat = EMBI_BPAT_DASHED;
            break;
        case EMBI_BSTYLE_DOUBLE:
            /* Below 3 px a double border is solid, as CSS allows. */
            pat = (w >= 3.0f) ? EMBI_BPAT_DOUBLE : EMBI_BPAT_SOLID;
            break;
        case EMBI_BSTYLE_GROOVE:
            outer = lead ? dk : lt;
            inner = lead ? lt : dk;
            break;
        case EMBI_BSTYLE_RIDGE:
            outer = lead ? lt : dk;
            inner = lead ? dk : lt;
            break;
        case EMBI_BSTYLE_INSET:
            outer = inner = lead ? dk : lt;
            break;
        case EMBI_BSTYLE_OUTSET:
            outer = inner = lead ? lt : dk;
            break;
        default:
            break;
        }
        b.widths[i] = w;
        b.colors[i] = outer;
        b.inner[i] = inner;
        b.pattern[i] = pat;
        if (w > 0.0f && ((outer >> 24) != 0u || (inner >> 24) != 0u)) {
            visible = true;
        }
    }
    if (!visible) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS || f->borders.len >= EMBI_RENDER_MAX_BORDERS) {
        f->overflow = true;
        return;
    }
    if (embi_region_culled(f, rect[0], rect[1], rect[2], rect[3], xform)) {
        return;
    }
    embi_vec_push(&f->borders, b);

    embi_prim p = {0};
    for (int i = 0; i < 4; i++) {
        p.rect[i] = rect[i];
    }
    embi_scale_radii(rect, radii, p.radii);
    p.kind = EMBI_PRIM_BORDER | ((f->borders.len - 1u) << 8);
    p.xform = xform;
    p.clip = clip;
    embi_frame_push_prim(f, &p, EMBI_NO_TEX);
}

void embi_frame_border(embi_frame *f, const float rect[4], const float radii[4],
                       const float widths[4], const uint32_t colors[4], uint32_t xform,
                       uint32_t clip)
{
    embi_frame_border_styled(f, rect, radii, widths, colors, nullptr, xform, clip);
}

/* Box shadows (R3a): the analytic rounded-rect shadow prim. The shape and clip rounded rects live in a
 * border record (the 64-byte record is reused as 16 floats, see embi_frame_shadow in render.h). */
void embi_frame_shadow(embi_frame *f, const float quad[4], const float shape[4],
                       const float shape_radii[4], const float clip_rect[4],
                       const float clip_radii[4], float sigma, bool inset, uint32_t rgba,
                       uint32_t xform, uint32_t clip)
{
    if (!embi_rect_ok(quad) || (rgba >> 24) == 0u || !embi_xform_usable(f, xform) ||
        clip >= f->clips.len || shape == nullptr || clip_rect == nullptr) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS || f->borders.len >= EMBI_RENDER_MAX_BORDERS) {
        f->overflow = true;
        return;
    }
    if (embi_region_culled(f, quad[0], quad[1], quad[2], quad[3], xform)) {
        return;
    }
    float rec[16] = {0};
    for (int i = 0; i < 4; i++) {
        rec[i] = embi_finite(shape[i]) ? shape[i] : 0.0f;
        rec[8 + i] = embi_finite(clip_rect[i]) ? clip_rect[i] : 0.0f;
    }
    if (!(rec[2] > 0.0f) || !(rec[3] > 0.0f)) {
        rec[2] = rec[3] = 0.0f; /* an empty shape: the shader treats a zero size as nothing */
    } else if (shape_radii != nullptr) {
        embi_scale_radii(rec, shape_radii, rec + 4);
    }
    if (!(rec[10] > 0.0f) || !(rec[11] > 0.0f)) {
        rec[10] = rec[11] = 0.0f;
    } else if (clip_radii != nullptr) {
        embi_scale_radii(rec + 8, clip_radii, rec + 12);
    }
    embi_border b;
    static_assert(sizeof b == sizeof rec, "a shadow record reuses the border record");
    SDL_memcpy(&b, rec, sizeof b);
    embi_vec_push(&f->borders, b);

    embi_prim p = {0};
    for (int i = 0; i < 4; i++) {
        p.rect[i] = quad[i];
    }
    p.color = rgba;
    p.kind = EMBI_PRIM_SHADOW | ((f->borders.len - 1u) << 8);
    p.xform = xform;
    p.clip = clip;
    p.extra[0] = embi_finite(sigma) && sigma > 0.0f ? sigma : 0.0f;
    p.extra[1] = inset ? 1.0f : 0.0f;
    embi_frame_push_prim(f, &p, EMBI_NO_TEX);
}

/* Atlas images and gradients ----------------------------------------------- */

static float embi_clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/* Index of the texture an image samples in the frame's texture list, adding it when it is new. A
 * shared region names the atlas (resolved at frame end); any other region names its texture. */
static bool embi_frame_tex_slot(embi_frame *f, const embi_atlas_region *img, uint32_t *slot)
{
    const embi_tex_ref want = img->atlas != nullptr ? (embi_tex_ref){.tex = nullptr, .atlas = img->atlas}
                                                    : (embi_tex_ref){.tex = img->tex, .atlas = nullptr};
    for (uint32_t i = 0; i < f->texs.len; i++) {
        const embi_tex_ref *t = &embi_vec_at(&f->texs, i);
        if (t->tex == want.tex && t->atlas == want.atlas) {
            *slot = i;
            return true;
        }
    }
    if (f->texs.len >= EMBI_RENDER_MAX_TEXS) {
        f->overflow = true;
        return false;
    }
    embi_vec_push(&f->texs, want);
    *slot = f->texs.len - 1u;
    return true;
}

/* Pushes a gradient record and copies its stops. Returns false when a cap is hit, and the frame
 * then draws nothing. */
static bool embi_frame_gradient_record(embi_frame *f, const float p0[2], const float p1[2],
                                       bool degenerate, const embi_stop *stops, uint32_t n,
                                       bool repeat, uint32_t *index)
{
    if (n > EMBI_RENDER_MAX_STOPS || f->stops.len > EMBI_RENDER_MAX_STOPS - n ||
        f->gradients.len >= EMBI_RENDER_MAX_GRADIENTS) {
        f->overflow = true;
        return false;
    }

    /* Stops: offsets never decrease. A non-finite offset takes the one before it. */
    const uint32_t first = f->stops.len;
    float prev = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float o = stops[i].offset;
        if (!embi_finite(o) || (i > 0 && o < prev)) {
            o = prev;
        }
        const embi_stop st = {.offset = o, .color = stops[i].color};
        embi_vec_push(&f->stops, st);
        prev = o;
    }

    embi_gradient g = {0};
    g.p0[0] = p0[0];
    g.p0[1] = p0[1];
    g.p1[0] = p1[0];
    g.p1[1] = p1[1];
    g.first_stop = first;
    g.stop_count = n;
    g.repeat = (repeat && !degenerate) ? 1u : 0u;
    embi_vec_push(&f->gradients, g);
    *index = f->gradients.len - 1u;
    return true;
}

/* Shared emitter for the three gradient kinds. p0 and p1 are the record's two points (for conic,
 * p1[0] is the start angle). degenerate marks geometry that paints the last stop. */
static void embi_frame_gradient(embi_frame *f, const float rect[4], const float radii[4],
                                uint32_t kind, const float p0[2], const float p1[2],
                                bool degenerate, const embi_stop *stops, uint32_t n, bool repeat,
                                uint32_t xform, uint32_t clip)
{
    if (!embi_rect_ok(rect) || !embi_xform_usable(f, xform) || clip >= f->clips.len || n == 0 ||
        stops == nullptr || !embi_finite(p0[0]) || !embi_finite(p0[1]) ||
        !embi_finite(p1[0]) || !embi_finite(p1[1])) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS) {
        f->overflow = true;
        return;
    }
    if (embi_region_culled(f, rect[0], rect[1], rect[2], rect[3], xform)) {
        return;
    }
    uint32_t gi = 0;
    if (!embi_frame_gradient_record(f, p0, p1, degenerate, stops, n, repeat, &gi)) {
        return;
    }

    embi_prim pr = {0};
    for (int i = 0; i < 4; i++) {
        pr.rect[i] = rect[i];
    }
    embi_scale_radii(rect, radii, pr.radii);
    pr.kind = kind | (gi << 8);
    pr.xform = xform;
    pr.clip = clip;
    embi_frame_push_prim(f, &pr, EMBI_NO_TEX);
}

void embi_frame_linear(embi_frame *f, const float rect[4], const float radii[4], float x0, float y0,
                       float x1, float y1, const embi_stop *stops, uint32_t n, bool repeat,
                       uint32_t xform, uint32_t clip)
{
    const float p0[2] = {x0, y0};
    const float p1[2] = {x1, y1};
    const bool degenerate = (x0 == x1 && y0 == y1);
    embi_frame_gradient(f, rect, radii, EMBI_PRIM_LINEAR, p0, p1, degenerate, stops, n, repeat,
                        xform, clip);
}

void embi_frame_radial(embi_frame *f, const float rect[4], const float radii[4], float cx, float cy,
                       float rx, float ry, const embi_stop *stops, uint32_t n, bool repeat,
                       uint32_t xform, uint32_t clip)
{
    const float p0[2] = {cx, cy};
    const float p1[2] = {rx, ry};
    const bool degenerate = !(rx > 0.0f && ry > 0.0f);
    embi_frame_gradient(f, rect, radii, EMBI_PRIM_RADIAL, p0, p1, degenerate, stops, n, repeat,
                        xform, clip);
}

void embi_frame_conic(embi_frame *f, const float rect[4], const float radii[4], float cx, float cy,
                      float from_radians, const embi_stop *stops, uint32_t n, bool repeat,
                      uint32_t xform, uint32_t clip)
{
    const float p0[2] = {cx, cy};
    const float p1[2] = {from_radians, 0.0f};
    embi_frame_gradient(f, rect, radii, EMBI_PRIM_CONIC, p0, p1, false, stops, n, repeat, xform,
                        clip);
}

void embi_frame_image(embi_frame *f, const float rect[4], const float radii[4], embi_atlas_region img,
                      const float uv[4], uint32_t tint_rgba, uint32_t xform, uint32_t clip)
{
    embi_frame_image_ex(f, rect, radii, img, uv, tint_rgba, xform, clip, false);
}

void embi_frame_image_ex(embi_frame *f, const float rect[4], const float radii[4], embi_atlas_region img,
                         const float uv[4], uint32_t tint_rgba, uint32_t xform, uint32_t clip, bool nearest)
{
    const bool shared = img.atlas != nullptr;
    if (!embi_rect_ok(rect) || uv == nullptr || (!shared && img.tex == nullptr) || img.w == 0 ||
        img.h == 0 || img.page_w == 0 || img.page_h == 0 || !embi_xform_usable(f, xform) ||
        clip >= f->clips.len || (tint_rgba >> 24) == 0u) {
        return;
    }
    if (shared && img.layer >= EMBI_ATLAS_MAX_SHARED) {
        return;
    }
    if (!embi_finite(uv[0]) || !embi_finite(uv[1]) || !embi_finite(uv[2]) ||
        !embi_finite(uv[3])) {
        return;
    }
    const float u0 = embi_clamp01(uv[0]);
    const float v0 = embi_clamp01(uv[1]);
    const float u1 = embi_clamp01(uv[2]);
    const float v1 = embi_clamp01(uv[3]);
    if (u0 == u1 || v0 == v1) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS) {
        f->overflow = true;
        return;
    }
    if (embi_region_culled(f, rect[0], rect[1], rect[2], rect[3], xform)) {
        return;
    }
    uint32_t slot = 0;
    if (!embi_frame_tex_slot(f, &img, &slot)) {
        return;
    }

    /* The image's texels in page space, then normalized to the page. A shared image is its layer
     * of the atlas array; a solo image or a layer is layer 0 of its own texture. */
    const float pw = (float)img.page_w;
    const float ph = (float)img.page_h;
    const float iw = (float)img.w;
    const float ih = (float)img.h;
    const uint32_t layer = shared ? img.layer : 0u;
    embi_prim pr = {0};
    for (int i = 0; i < 4; i++) {
        pr.rect[i] = rect[i];
    }
    embi_scale_radii(rect, radii, pr.radii);
    pr.color = tint_rgba;
    pr.xform = xform;
    pr.clip = clip;
    pr.extra[0] = ((float)img.x + u0 * iw) / pw;
    pr.extra[1] = ((float)img.y + v0 * ih) / ph;
    pr.extra[2] = ((float)img.x + u1 * iw) / pw;
    pr.extra[3] = ((float)img.y + v1 * ih) / ph;

    /* A whole image sampled linearly needs nothing more: its edge texels are extruded in the atlas. Anything else gets
     * a record (embi_image_rec): a sub-rect's uv is clamped half a texel inside itself, because its interior edges
     * have real neighbors, and nearest sampling snaps to texel centers. The clamp midpoint keeps a one-texel
     * sub-rect from inverting. */
    const bool sub = u0 > 0.0f || v0 > 0.0f || u1 < 1.0f || v1 < 1.0f;
    if (!nearest && !sub) {
        pr.kind = EMBI_PRIM_IMAGE | (layer << 8);
    } else {
        if (f->borders.len >= EMBI_RENDER_MAX_BORDERS) {
            f->overflow = true;
            return;
        }
        const float hx = 0.5f / pw;
        const float hy = 0.5f / ph;
        const float mx = 0.5f * (pr.extra[0] + pr.extra[2]);
        const float my = 0.5f * (pr.extra[1] + pr.extra[3]);
        embi_image_rec rec = {0};
        rec.clamp[0] = SDL_min(pr.extra[0] + hx, mx);
        rec.clamp[1] = SDL_min(pr.extra[1] + hy, my);
        rec.clamp[2] = SDL_max(pr.extra[2] - hx, mx);
        rec.clamp[3] = SDL_max(pr.extra[3] - hy, my);
        rec.texel[0] = 1.0f / pw;
        rec.texel[1] = 1.0f / ph;
        rec.layer = layer;
        rec.nearest = nearest ? 1u : 0u;
        embi_border b;
        static_assert(sizeof b == sizeof rec, "an image record reuses the border record");
        SDL_memcpy(&b, &rec, sizeof b);
        embi_vec_push(&f->borders, b);
        pr.kind = EMBI_PRIM_IMAGE_REC | ((f->borders.len - 1u) << 8);
    }
    embi_frame_push_prim(f, &pr, slot);
}

void embi_frame_image_proj(embi_frame *f, const float rect[4], embi_atlas_region img, const float uv[4],
                           uint32_t tint_rgba, const float h[9], uint32_t clip)
{
    const bool shared = img.atlas != nullptr;
    if (!embi_rect_ok(rect) || uv == nullptr || h == nullptr || (!shared && img.tex == nullptr) || img.w == 0 ||
        img.h == 0 || img.page_w == 0 || img.page_h == 0 || clip >= f->clips.len || (tint_rgba >> 24) == 0u) {
        return;
    }
    if (shared && img.layer >= EMBI_ATLAS_MAX_SHARED) {
        return;
    }
    for (int i = 0; i < 9; i++) {
        if (!embi_finite(h[i])) {
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!embi_finite(uv[i])) {
            return;
        }
    }
    const float u0 = embi_clamp01(uv[0]);
    const float v0 = embi_clamp01(uv[1]);
    const float u1 = embi_clamp01(uv[2]);
    const float v1 = embi_clamp01(uv[3]);
    if (u0 == u1 || v0 == v1) {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS || f->xforms.len + 2 > EMBI_RENDER_MAX_XFORMS) {
        f->overflow = true;
        return;
    }
    uint32_t slot = 0;
    if (!embi_frame_tex_slot(f, &img, &slot)) {
        return;
    }
    /* Two records: X and Y (device scale in) with a nonzero pad so the prim counts as usable, then W. */
    const float s = f->scale;
    embi_xform xa = {0};
    xa.m[0] = h[0] * s;
    xa.m[1] = h[3] * s;
    xa.m[2] = h[1] * s;
    xa.m[3] = h[4] * s;
    xa.m[4] = h[2] * s;
    xa.m[5] = h[5] * s;
    xa.pad[0] = xa.pad[1] = 1.0f;
    embi_xform xw = {0};
    xw.m[0] = h[6];
    xw.m[1] = h[7];
    xw.m[2] = h[8];
    const uint32_t xi = embi_xform_push(f, &xa);
    embi_xform_push(f, &xw);

    const float pw = (float)img.page_w;
    const float ph = (float)img.page_h;
    const float iw = (float)img.w;
    const float ih = (float)img.h;
    const uint32_t layer = shared ? img.layer : 0u;
    embi_prim pr = {0};
    for (int i = 0; i < 4; i++) {
        pr.rect[i] = rect[i];
    }
    pr.color = tint_rgba;
    pr.kind = EMBI_PRIM_PLANE | (layer << 8);
    pr.xform = xi;
    pr.clip = clip;
    pr.extra[0] = ((float)img.x + u0 * iw) / pw;
    pr.extra[1] = ((float)img.y + v0 * ih) / ph;
    pr.extra[2] = ((float)img.x + u1 * iw) / pw;
    pr.extra[3] = ((float)img.y + v1 * ih) / ph;
    embi_frame_push_prim(f, &pr, slot);
}

/* Outlines (glyphs and paths) ---------------------------------------------- */

/* embi_frame_fill_quads; curves may be nullptr when the blob of key is cached. */
static void embi_frame_outline(embi_frame *f, uint64_t key, const embi_qcurve *curves, uint32_t n,
                               const float to_local[6], const embi_paint *paint, embi_fill_rule rule,
                               uint32_t xform, uint32_t clip)
{
    if (to_local == nullptr || paint == nullptr || !embi_xform_usable(f, xform) || clip >= f->clips.len) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        if (!embi_finite(to_local[i])) {
            return;
        }
    }
    if (to_local[0] * to_local[3] - to_local[1] * to_local[2] == 0.0f) {
        return; /* the outline cannot be placed: nothing is visible */
    }
    const float *g = paint->g;
    if (paint->kind == EMBI_PRIM_FILL) {
        if ((paint->color >> 24) == 0u) {
            return;
        }
    } else if (paint->kind == EMBI_PRIM_LINEAR || paint->kind == EMBI_PRIM_RADIAL ||
               paint->kind == EMBI_PRIM_CONIC) {
        if (paint->stops == nullptr || paint->n_stops == 0 || !embi_finite(g[0]) ||
            !embi_finite(g[1]) || !embi_finite(g[2]) || !embi_finite(g[3])) {
            return;
        }
    } else {
        return;
    }
    if (f->prims.len >= EMBI_RENDER_MAX_PRIMS) {
        f->overflow = true;
        return;
    }

    embi_slug_entry ent;
    if (!embi_slug_get(f->r->slug, key, curves, curves != nullptr ? n : 0, &ent)) {
        return;
    }
    if (f->region) {
        /* Bounds of the outline in local px: its em box mapped through to_local. */
        const float x0 = ent.box[0];
        const float y0 = ent.box[1];
        const float x1 = ent.box[2];
        const float y1 = ent.box[3];
        const float cx[4] = {x0, x1, x0, x1};
        const float cy[4] = {y0, y0, y1, y1};
        float lx0 = 0.0f;
        float ly0 = 0.0f;
        float lx1 = 0.0f;
        float ly1 = 0.0f;
        for (int i = 0; i < 4; i++) {
            const float lx = to_local[0] * cx[i] + to_local[2] * cy[i] + to_local[4];
            const float ly = to_local[1] * cx[i] + to_local[3] * cy[i] + to_local[5];
            if (i == 0) {
                lx0 = lx1 = lx;
                ly0 = ly1 = ly;
            } else {
                lx0 = embi_minf(lx0, lx);
                lx1 = embi_maxf(lx1, lx);
                ly0 = embi_minf(ly0, ly);
                ly1 = embi_maxf(ly1, ly);
            }
        }
        if (embi_region_culled(f, lx0, ly0, lx1 - lx0, ly1 - ly0, xform)) {
            return;
        }
    }
    /* An outline prim: the blob's em box as the rect, the 2x2 map in the radii, the translation
     * and the blob words in the extra block (see embi_prim). */
    embi_prim pr = {0};
    for (int i = 0; i < 4; i++) {
        pr.rect[i] = ent.box[i];
        pr.radii[i] = to_local[i];
    }
    pr.outline.trans[0] = to_local[4];
    pr.outline.trans[1] = to_local[5];
    pr.outline.data = ent.offset;
    pr.outline.bands = ent.nh | (ent.nv << 5) | ((rule == EMBI_FILL_EVENODD) ? (1u << 10) : 0u);
    pr.xform = xform;
    pr.clip = clip;
    if (paint->kind == EMBI_PRIM_FILL) {
        pr.color = paint->color;
        pr.kind = EMBI_PRIM_FILL | EMBI_PRIM_OUTLINE;
    } else {
        float p0[2] = {g[0], g[1]};
        float p1[2] = {g[2], g[3]};
        bool degenerate = false;
        if (paint->kind == EMBI_PRIM_LINEAR) {
            degenerate = (g[0] == g[2] && g[1] == g[3]);
        } else if (paint->kind == EMBI_PRIM_RADIAL) {
            degenerate = !(g[2] > 0.0f && g[3] > 0.0f);
        } else {
            p1[0] = g[2];
            p1[1] = 0.0f;
        }
        uint32_t gi = 0;
        if (!embi_frame_gradient_record(f, p0, p1, degenerate, paint->stops, paint->n_stops,
                                        paint->repeat, &gi)) {
            return;
        }
        pr.kind = paint->kind | EMBI_PRIM_OUTLINE | (gi << 8);
    }
    embi_frame_push_prim(f, &pr, EMBI_NO_TEX);
}

void embi_frame_fill_quads(embi_frame *f, uint64_t key, const embi_qcurve *curves, uint32_t n,
                           const float to_local[6], const embi_paint *paint, embi_fill_rule rule,
                           uint32_t xform, uint32_t clip)
{
    if (f == nullptr || curves == nullptr || n == 0) {
        return;
    }
    embi_frame_outline(f, key, curves, n, to_local, paint, rule, xform, clip);
}

bool embi_frame_fill_cached(embi_frame *f, uint64_t key, const float to_local[6], const embi_paint *paint,
                            embi_fill_rule rule, uint32_t xform, uint32_t clip)
{
    if (f == nullptr || !embi_slug_has(f->r->slug, key)) {
        return false;
    }
    embi_frame_outline(f, key, nullptr, 0, to_local, paint, rule, xform, clip);
    return true;
}

/* Text (W4h) --------------------------------------------------------------- */

void embi_frame_set_text_aa(embi_frame *f, embi_text_aa mode, bool transforms_animating)
{
    if (f == nullptr) {
        return;
    }
    f->text_aa = (mode == EMBI_TEXT_AA_LCD_RGB || mode == EMBI_TEXT_AA_LCD_BGR) ? mode
                                                                                : EMBI_TEXT_AA_GRAY;
    f->animating = transforms_animating;
}

void embi_frame_set_opaque_backdrop(embi_frame *f, bool opaque)
{
    if (f != nullptr) {
        f->opaque_backdrop = opaque;
    }
}

void embi_frame_set_lcd_allowed(embi_frame *f, bool allowed)
{
    if (f != nullptr) {
        f->lcd_allowed = allowed;
    }
}

bool embi_frame_text_target(const embi_frame *f, uint32_t xform, embi_text_target *out)
{
    if (f == nullptr || out == nullptr || xform >= f->xforms.len) {
        return false;
    }
    const embi_xform *x = &embi_vec_at(&f->xforms, xform);
    for (int i = 0; i < 6; i++) {
        out->m[i] = x->m[i];
    }
    /* A skew of 1e-4 or less moves a glyph by under a thousandth of a pixel, so it counts as none:
     * rotate(360deg) leaves sin(2 pi) = -2.4e-7 in the matrix and must still look like no rotation. */
    const float tol = 1e-4f * embi_maxf(x->m[0], x->m[3]);
    const bool plain = SDL_fabsf(x->m[1]) <= tol && SDL_fabsf(x->m[2]) <= tol && x->m[0] > 0.0f &&
                       x->m[3] > 0.0f && x->pad[0] > 0.0f;
    const bool still = !f->animating || xform == 0 || (xform < f->xstill.len && embi_vec_at(&f->xstill, xform) != 0);
    out->fit = plain && still;
    out->lcd = out->fit && f->opaque_backdrop && f->lcd_allowed &&
               f->text_aa != EMBI_TEXT_AA_GRAY;
    return true;
}

/* Bits of the kind word's index field on a solid outline prim (mirrored in box.slang): bit 0 text
 * (contrast curve), bit 1 LCD, bits 2..9 the total stem darkening in 1/512 device px. */
#define EMBI_TEXTBIT_TEXT 1u
#define EMBI_TEXTBIT_LCD 2u

void embi_frame_fill_text(embi_frame *f, uint64_t key, const embi_qcurve *curves, uint32_t n,
                          const float to_local[6], uint32_t rgba, float darken_px, bool lcd,
                          uint32_t xform, uint32_t clip)
{
    if (f == nullptr) {
        return;
    }
    const uint32_t before = f->prims.len;
    const embi_paint paint = embi_paint_solid(rgba);
    embi_frame_fill_quads(f, key, curves, n, to_local, &paint, EMBI_FILL_NONZERO, xform, clip);
    if (f->prims.len != before + 1u) {
        return;
    }
    embi_text_target tt;
    const bool use_lcd = lcd && embi_frame_text_target(f, xform, &tt) && tt.lcd;
    float dk = darken_px;
    dk = dk > 0.0f ? (dk < 0.49f ? dk : 0.49f) : 0.0f;
    uint32_t bits = EMBI_TEXTBIT_TEXT | (use_lcd ? EMBI_TEXTBIT_LCD : 0u) |
                    ((uint32_t)(dk * 512.0f + 0.5f) << 2);
    embi_prim *p = &embi_vec_at(&f->prims, before);
    p->kind |= bits << 8;
    if (use_lcd) {
        embi_vec_at(&f->tags, before).pipe = EMBI_PIPE_LCD;
        f->lcd_prims++;
    }
}

/* Upload, batch and draw --------------------------------------------------- */

/* Splits the prims into batches. A batch ends at the cap, when the pipeline changes, or when a prim
 * samples a different texture than the batch already holds. A prim that samples no texture (a
 * fill, border, gradient or outline) takes the batch's texture. Batches are contiguous runs of the
 * prim array, so each one uploads as one range. The region clear is prim 0 and is skipped unless
 * the frame clears, so the runs that follow it still start at prim 1. */
static void embi_build_batches(embi_renderer *r)
{
    const embi_frame *f = &r->frame;
    embi_vec_clear(&r->batches);
    for (uint32_t i = 0; i < f->prims.len; i++) {
        const embi_ptag tg = embi_vec_at(&f->tags, i);
        if (tg.pipe == EMBI_PIPE_CLEAR && !f->region_clear) {
            continue; /* the region clear of a frame that does not clear */
        }
        bool cut = r->batches.len == 0;
        if (!cut) {
            const embi_batch *last = &embi_vec_at(&r->batches, r->batches.len - 1);
            cut = last->count >= EMBI_BATCH_MAX_PRIMS || last->pipe != tg.pipe ||
                  (tg.tex != EMBI_NO_TEX && last->tex != EMBI_NO_TEX && last->tex != tg.tex);
        }
        if (cut) {
            const embi_batch b = {.first = i, .count = 0, .tex = EMBI_NO_TEX, .pipe = tg.pipe};
            embi_vec_push(&r->batches, b);
        }
        embi_batch *b = &embi_vec_at(&r->batches, r->batches.len - 1);
        if (tg.tex != EMBI_NO_TEX) {
            b->tex = tg.tex;
        }
        b->count++;
    }
}

/* Turns every outline prim of the frame into a prim that paints nothing: a solid fill of color 0
 * with no radii. Used when the Slug blobs did not reach the GPU, so no outline reads stale data.
 * The prim keeps its place, so the batches stay contiguous. */
static void embi_frame_hide_outlines(embi_frame *f)
{
    for (uint32_t i = 0; i < f->prims.len; i++) {
        embi_prim *p = &embi_vec_at(&f->prims, i);
        if ((p->kind & EMBI_PRIM_OUTLINE) != 0u) {
            p->kind = EMBI_PRIM_FILL;
            p->color = 0;
            for (int k = 0; k < 4; k++) {
                p->radii[k] = 0.0f;
            }
        }
    }
}

/* The texture a batch binds: the batch's texture slot, resolved now. A batch with none binds the
 * blank array. An atlas slot resolves to the atlas array as it is at this point. */
static SDL_GPUTexture *embi_batch_texture(const embi_renderer *r, uint32_t slot)
{
    if (slot == EMBI_NO_TEX) {
        return r->blank;
    }
    const embi_tex_ref *t = &embi_vec_at(&r->frame.texs, slot);
    SDL_GPUTexture *tex = t->atlas != nullptr ? embi_atlas_texture(t->atlas) : t->tex;
    return tex != nullptr ? tex : r->blank;
}

void embi_frame_end(embi_frame *f, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, bool clear,
                    uint32_t clear_rgba)
{
    embi_renderer *r = f->r;
    embi_renderer_warm_wait(r); /* the buffers below are the thread's until it is done */
    if (!embi_aux_make(r)) {
        embi_log_error("render: the sampler or the blank texture could not be made: %s", SDL_GetError());
        r->frame_open = false;
        r->last_draws = 0;
        return;
    }
    if (f->region && !embi_clear_pipeline_ensure(r)) {
        /* embi_renderer_region_ok tells the caller before it records a region frame; this is the backstop. */
        r->frame_open = false;
        r->last_draws = 0;
        return;
    }
    if (!r->frame_open || cmd == nullptr || target == nullptr) {
        embi_log_error("render: embi_frame_end: no open frame, or no command buffer or target");
        r->frame_open = false;
        return;
    }
    /* From here the frame is over, whatever happens below. */
    r->frame_open = false;
    SDL_GPUDevice *dev = r->dev;

    if (f->overflow) {
        /* The frame recorded more than a cap allows. Draw nothing (but still clear, if asked),
         * so the output is an empty frame rather than a partial one. */
        embi_log_error("render: frame exceeds a limit (%u prims, %u transforms, %u clips, "
                       "%u borders); drawing nothing",
                       EMBI_RENDER_MAX_PRIMS, EMBI_RENDER_MAX_XFORMS, EMBI_RENDER_MAX_CLIPS,
                       EMBI_RENDER_MAX_BORDERS);
        embi_frame_reset(f); /* keeps the tables every frame needs, and the region clear */
    }

    /* Slug blobs built this frame go up even when nothing is drawn, so the cache never names a
     * blob that did not reach the GPU. */
    if (f->region && (f->damage.w <= 0 || f->damage.h <= 0)) {
        (void)embi_slug_upload(r->slug, cmd);
        r->last_draws = 0;
        return;
    }
    f->region_clear = f->region && clear;
    if (f->region) {
        embi_vec_at(&f->prims, 0).color = clear_rgba; /* prim 0 is the region clear */
    }

    /* The blobs go up first, in their own copy pass, so the outline prims know whether they can
     * draw. Growth of the slug buffer happens here too. */
    const bool slug_ok = embi_slug_upload(r->slug, cmd);
    if (!slug_ok) {
        embi_frame_hide_outlines(f);
    }

    embi_build_batches(r);
    const uint32_t nb = r->batches.len;
    const uint32_t np = f->prims.len;
    const uint32_t nx = f->xforms.len;
    const uint32_t nc = f->clips.len;
    const uint32_t nbd = f->borders.len;
    const uint32_t ng = f->gradients.len;
    const uint32_t ns = f->stops.len;

    bool ok = true;
    for (uint32_t i = 0; i < nb && ok; i++) {
        if (i >= r->prim_bufs.len) {
            const embi_gbuf zero = {0};
            embi_vec_push(&r->prim_bufs, zero);
        }
        ok = embi_gbuf_ensure(&r->buffer_creates, dev, &r->prim_bufs.data[i],
                              (size_t)embi_vec_at(&r->batches, i).count * sizeof(embi_prim),
                              EMBI_MAXB_PRIMS);
    }
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->xform_buf, (size_t)nx * sizeof(embi_xform), EMBI_MAXB_XFORMS);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->clip_buf, (size_t)nc * sizeof(embi_clip), EMBI_MAXB_CLIPS);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->border_buf, (size_t)nbd * sizeof(embi_border),
                                  EMBI_MAXB_BORDERS);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->gradient_buf, (size_t)ng * sizeof(embi_gradient),
                                EMBI_MAXB_GRADIENTS);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->stop_buf, (size_t)ns * sizeof(embi_stop),
                                EMBI_MAXB_STOPS);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->frame_buf, sizeof(embi_frame_params), 256);
    ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->slug_dummy, 16, 16);
    for (int i = 0; i < 3; i++) {
        ok = ok && embi_gbuf_ensure(&r->buffer_creates, dev, &r->chan_buf[i], 16, 16);
    }

    /* One transfer buffer holds every array, one after another, 16-byte aligned. */
    const size_t off_prims = 0;
    const size_t off_xforms = embi_align16(off_prims + (size_t)np * sizeof(embi_prim));
    const size_t off_clips = embi_align16(off_xforms + (size_t)nx * sizeof(embi_xform));
    const size_t off_borders = embi_align16(off_clips + (size_t)nc * sizeof(embi_clip));
    const size_t off_grads = embi_align16(off_borders + (size_t)nbd * sizeof(embi_border));
    const size_t off_stops = embi_align16(off_grads + (size_t)ng * sizeof(embi_gradient));
    const size_t off_frame = embi_align16(off_stops + (size_t)ns * sizeof(embi_stop));
    const size_t off_chan = embi_align16(off_frame + sizeof(embi_frame_params));
    const size_t total = off_chan + 3u * 16u;
    ok = ok && embi_upload_ensure(r, total);
    if (!ok) {
        embi_log_error("render: embi_frame_end: buffer setup failed, frame dropped");
        r->last_draws = 0;
        return;
    }

    size_t base = 0;
    uint8_t *mapped = embi_upload_map(r, total, &base);
    if (mapped == nullptr) {
        embi_log_error("render: SDL_MapGPUTransferBuffer failed: %s", SDL_GetError());
        r->last_draws = 0;
        return;
    }
    if (np > 0) {
        SDL_memcpy(mapped + off_prims, f->prims.data, (size_t)np * sizeof(embi_prim));
    }
    if (nx > 0) {
        SDL_memcpy(mapped + off_xforms, f->xforms.data, (size_t)nx * sizeof(embi_xform));
    }
    if (nc > 0) {
        SDL_memcpy(mapped + off_clips, f->clips.data, (size_t)nc * sizeof(embi_clip));
    }
    if (nbd > 0) {
        SDL_memcpy(mapped + off_borders, f->borders.data, (size_t)nbd * sizeof(embi_border));
    }
    if (ng > 0) {
        SDL_memcpy(mapped + off_grads, f->gradients.data, (size_t)ng * sizeof(embi_gradient));
    }
    if (ns > 0) {
        SDL_memcpy(mapped + off_stops, f->stops.data, (size_t)ns * sizeof(embi_stop));
    }
    const embi_frame_params fp = {
        .viewport = {(float)f->w, (float)f->h},
        .pad = {0.0f, 0.0f},
    };
    SDL_memcpy(mapped + off_frame, &fp, sizeof fp);
    {
        /* The subpixel offset of each channel pass, in device px from the pixel center. */
        const float side = f->text_aa == EMBI_TEXT_AA_LCD_BGR ? -1.0f : 1.0f;
        for (uint32_t c = 0; c < 3u; c++) {
            const float chan[4] = {side * ((float)c - 1.0f) / 3.0f, 0.0f, 0.0f, 0.0f};
            SDL_memcpy(mapped + off_chan + 16u * c, chan, sizeof chan);
        }
    }
    SDL_UnmapGPUTransferBuffer(dev, r->upload);

    /* Growth copies of every atlas the frame samples go into cmd before the frame's own copies and
     * the render pass. A failed copy leaves the layers pending; the frame still draws. */
    for (uint32_t i = 0; i < f->texs.len; i++) {
        const embi_tex_ref *t = &embi_vec_at(&f->texs, i);
        if (t->atlas != nullptr && !embi_atlas_flush(t->atlas, cmd)) {
            embi_log_error("render: embi_frame_end: an atlas growth copy failed");
        }
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (cp == nullptr) {
        embi_log_error("render: SDL_BeginGPUCopyPass failed: %s", SDL_GetError());
        r->last_draws = 0;
        return;
    }
    for (uint32_t i = 0; i < nb; i++) {
        const embi_batch *b = &embi_vec_at(&r->batches, i);
        const SDL_GPUTransferBufferLocation src = {
            .transfer_buffer = r->upload,
            .offset = (Uint32)(base + off_prims + (size_t)b->first * sizeof(embi_prim)),
        };
        const SDL_GPUBufferRegion dst = {
            .buffer = r->prim_bufs.data[i].buf,
            .offset = 0,
            .size = (Uint32)((size_t)b->count * sizeof(embi_prim)),
        };
        /* Not cycled: a page drawn with layers records a frame per layer into one command buffer, and cycling made SDL
         * create a buffer per layer (and per frame in flight). The GPU runs the copy after the work recorded before it
         * that reads the buffer (SDL puts the barrier in). */
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    const struct {
        SDL_GPUBuffer *buf;
        size_t off;
        size_t bytes;
    } segs[] = {
        {r->xform_buf.buf, off_xforms, (size_t)nx * sizeof(embi_xform)},
        {r->clip_buf.buf, off_clips, (size_t)nc * sizeof(embi_clip)},
        {r->border_buf.buf, off_borders, (size_t)nbd * sizeof(embi_border)},
        {r->gradient_buf.buf, off_grads, (size_t)ng * sizeof(embi_gradient)},
        {r->stop_buf.buf, off_stops, (size_t)ns * sizeof(embi_stop)},
        {r->frame_buf.buf, off_frame, sizeof(embi_frame_params)},
        {r->chan_buf[0].buf, off_chan, 16u},
        {r->chan_buf[1].buf, off_chan + 16u, 16u},
        {r->chan_buf[2].buf, off_chan + 32u, 16u},
    };
    for (size_t i = 0; i < sizeof segs / sizeof segs[0]; i++) {
        if (segs[i].bytes == 0) {
            continue;
        }
        const SDL_GPUTransferBufferLocation src = {
            .transfer_buffer = r->upload,
            .offset = (Uint32)(base + segs[i].off),
        };
        const SDL_GPUBufferRegion dst = {
            .buffer = segs[i].buf,
            .offset = 0,
            .size = (Uint32)segs[i].bytes,
        };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);

    if (f->lcd_prims > 0 && r->lcd_pipeline[0] == nullptr && !r->lcd_failed) {
        if (!embi_make_lcd_pipelines(r)) {
            r->lcd_failed = true; /* logged once; LCD text then draws with one pass */
            embi_log_error("render: LCD text pipelines failed; LCD text draws with one pass");
        }
    }

    if (f->dstin_prims > 0 && r->dstin_pipeline == nullptr && !r->dstin_failed) {
        r->dstin_pipeline = embi_make_dstin_pipeline(r);
        if (r->dstin_pipeline == nullptr) {
            r->dstin_failed = true; /* logged by the maker; masks then draw as a plain composite */
        }
    }

    /* Render pass: one draw per batch, in order. The storage slots match shaders/lib/bind.slang. */
    const float a = (float)((clear_rgba >> 24) & 255u) / 255.0f;
    const SDL_GPUColorTargetInfo color = {
        .texture = target,
        .clear_color =
            {
                .r = (float)(clear_rgba & 255u) / 255.0f * a,
                .g = (float)((clear_rgba >> 8) & 255u) / 255.0f * a,
                .b = (float)((clear_rgba >> 16) & 255u) / 255.0f * a,
                .a = a,
            },
        /* A region frame never clears the target: its clear is the CLEAR prim, inside the damage. */
        .load_op = (clear && !f->region) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    if (pass == nullptr) {
        embi_log_error("render: SDL_BeginGPURenderPass failed: %s", SDL_GetError());
        r->last_draws = 0;
        return;
    }
    if (f->region) {
        const SDL_Rect sr = {f->damage.x, f->damage.y, f->damage.w, f->damage.h};
        SDL_SetGPUScissor(pass, &sr);
    }
    /* The slug buffer is bound in every batch. Until a blob exists the dummy stands in for it. */
    SDL_GPUBuffer *slug_buf = embi_slug_buffer(r->slug);
    if (slug_buf == nullptr) {
        slug_buf = r->slug_dummy.buf;
    }
    uint32_t draws = 0;
    SDL_GPUGraphicsPipeline *bound = nullptr;
    for (uint32_t i = 0; i < nb; i++) {
        const embi_batch *b = &embi_vec_at(&r->batches, i);
        const SDL_GPUTextureSamplerBinding ts = {.texture = embi_batch_texture(r, b->tex),
                                                 .sampler = r->sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);
        SDL_GPUBuffer *vs_bufs[3] = {r->prim_bufs.data[i].buf, r->xform_buf.buf, r->frame_buf.buf};
        SDL_BindGPUVertexStorageBuffers(pass, 0, vs_bufs, 3);
        /* An LCD batch is drawn once per color channel; the others once. */
        const bool lcd = b->pipe == EMBI_PIPE_LCD;
        const bool three = lcd && r->lcd_pipeline[0] != nullptr;
        const uint32_t passes = three ? 3u : 1u;
        for (uint32_t c = 0; c < passes; c++) {
            SDL_GPUGraphicsPipeline *pl = b->pipe == EMBI_PIPE_CLEAR ? r->clear_pipeline
                                          : three                   ? r->lcd_pipeline[c]
                                          : b->pipe == EMBI_PIPE_DSTIN && r->dstin_pipeline != nullptr ? r->dstin_pipeline
                                                                    : r->pipeline;
            if (pl != bound) {
                SDL_BindGPUGraphicsPipeline(pass, pl);
                bound = pl;
            }
            /* Slot 0 on every backend. The shader numbers the buffers per target (bind.slang). */
            SDL_GPUBuffer *fs_bufs[8] = {r->prim_bufs.data[i].buf, r->xform_buf.buf,
                                         r->clip_buf.buf,          r->border_buf.buf,
                                         r->gradient_buf.buf,      r->stop_buf.buf,
                                         slug_buf,                 r->chan_buf[three ? c : 1].buf};
            SDL_BindGPUFragmentStorageBuffers(pass, 0, fs_bufs, 8);
            SDL_DrawGPUPrimitives(pass, 6, b->count, 0, 0);
            draws++;
        }
    }
    SDL_EndGPURenderPass(pass);
    r->last_draws = draws;
    r->last_batches = r->batches.len;
    r->last_prims = f->prims.len;
}

void embi_frame_set_blend(embi_frame *f, embi_blend blend)
{
    if (f != nullptr) {
        f->blend_pipe = blend == EMBI_BLEND_DSTIN ? EMBI_PIPE_DSTIN : EMBI_PIPE_BOX;
    }
}
