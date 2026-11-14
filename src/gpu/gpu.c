#include "gpu/gpu.h"

#include <SDL3/SDL_log.h>

#include "base/base.h"

/* The formats the build actually produced (after the dxil-skip logic in cmake/EmberShaders.cmake):
 * EMBI_SHADER_FMT_* and EMBI_BUILT_SHADER_FORMATS, one global file for the whole build. */
#include "ember_shaders/formats.h"

static_assert((EMBI_SHADER_FMT_SPIRV | EMBI_SHADER_FMT_DXIL | EMBI_SHADER_FMT_MSL) != 0,
              "EMBER_SHADER_FORMATS built no shader format at all");

struct embi_gpu {
    SDL_GPUDevice *device;
    SDL_GPUShaderFormat format;
    bool owned;
};

static SDL_GPUShaderFormat embi_pick_format(SDL_GPUShaderFormat supported)
{
    static const SDL_GPUShaderFormat preference[] = {
        SDL_GPU_SHADERFORMAT_DXIL,
        SDL_GPU_SHADERFORMAT_SPIRV,
        SDL_GPU_SHADERFORMAT_MSL,
    };
    const SDL_GPUShaderFormat usable = supported & EMBI_BUILT_SHADER_FORMATS;

    for (size_t i = 0; i < sizeof preference / sizeof preference[0]; i++) {
        if (usable & preference[i]) {
            return preference[i];
        }
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

static const embi_shader_blob *embi_stage_blob(const embi_shader_stage *s, SDL_GPUShaderFormat f)
{
    if (f == SDL_GPU_SHADERFORMAT_DXIL) {
        return &s->dxil;
    }
    if (f == SDL_GPU_SHADERFORMAT_SPIRV) {
        return &s->spirv;
    }
    if (f == SDL_GPU_SHADERFORMAT_MSL) {
        return &s->msl;
    }
    return nullptr;
}

static const embi_shader_blob *embi_program_blob(const embi_compute_program *p,
                                                 SDL_GPUShaderFormat f)
{
    if (f == SDL_GPU_SHADERFORMAT_DXIL) {
        return &p->dxil;
    }
    if (f == SDL_GPU_SHADERFORMAT_SPIRV) {
        return &p->spirv;
    }
    if (f == SDL_GPU_SHADERFORMAT_MSL) {
        return &p->msl;
    }
    return nullptr;
}

embi_gpu *embi_gpu_create(SDL_GPUDevice *external, bool debug)
{
    embi_gpu *g = SDL_calloc(1, sizeof *g);
    if (!g) {
        return nullptr;
    }

    if (external) {
        g->device = external;
        g->owned = false;
    } else {
        g->device = SDL_CreateGPUDevice(EMBI_BUILT_SHADER_FORMATS, debug, nullptr);
        if (!g->device) {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "embi_gpu_create: SDL_CreateGPUDevice failed: %s",
                         SDL_GetError());
            SDL_free(g);
            return nullptr;
        }
        g->owned = true;
    }

    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(g->device);
    g->format = embi_pick_format(supported);
    if (g->format == SDL_GPU_SHADERFORMAT_INVALID) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU,
                     "embi_gpu_create: no shader format in common: device supports 0x%x, "
                     "embedded blobs are 0x%x",
                     (unsigned)supported, (unsigned)EMBI_BUILT_SHADER_FORMATS);
        if (g->owned) {
            SDL_DestroyGPUDevice(g->device);
        }
        SDL_free(g);
        return nullptr;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "GPU driver %s, shader format 0x%x",
                SDL_GetGPUDeviceDriver(g->device), (unsigned)g->format);
    return g;
}

void embi_gpu_destroy(embi_gpu *g)
{
    if (!g) {
        return;
    }
    if (g->owned) {
        SDL_DestroyGPUDevice(g->device);
    }
    SDL_free(g);
}

SDL_GPUDevice *embi_gpu_device(embi_gpu *g)
{
    return g->device;
}

SDL_GPUShaderFormat embi_gpu_format(embi_gpu *g)
{
    return g->format;
}

embi_gpu_blob_view embi_gpu_blob_open(const embi_shader_blob *b)
{
    embi_gpu_blob_view v = {nullptr, 0, nullptr, 0};
    if (!b || !b->code || b->size == 0) {
        return v;
    }
    if (!b->packed) {
        v.code = b->code;
        v.size = b->size;
        return v;
    }
    size_t raw = 0;
    v.owned = embi_lz_unpack(b->code, b->size, &raw);
    if (v.owned != nullptr) {
        v.owned_size = raw + 1;
        v.code = v.owned;
        v.size = raw;
    }
    return v;
}

void embi_gpu_blob_close(embi_gpu_blob_view *v)
{
    if (v->owned != nullptr) {
        embi_free(v->owned, v->owned_size);
    }
    *v = (embi_gpu_blob_view){nullptr, 0, nullptr, 0};
}

SDL_GPUShader *embi_gpu_make_shader(embi_gpu *g, const embi_shader_stage *s,
                                    SDL_GPUShaderStage stage)
{
    const embi_shader_blob *b = embi_stage_blob(s, g->format);
    embi_gpu_blob_view bv = embi_gpu_blob_open(b);
    if (!bv.code) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU,
                     "embi_gpu_make_shader: no blob for format 0x%x in this stage",
                     (unsigned)g->format);
        return nullptr;
    }

    const SDL_GPUShaderCreateInfo info = {
        .code_size = bv.size,
        .code = bv.code,
        .entrypoint = b->entry,
        .format = g->format,
        .stage = stage,
        .num_samplers = s->num_samplers,
        .num_storage_textures = s->num_storage_textures,
        .num_storage_buffers = s->num_storage_buffers,
        .num_uniform_buffers = s->num_uniform_buffers,
    };
    SDL_GPUShader *sh = SDL_CreateGPUShader(g->device, &info);
    embi_gpu_blob_close(&bv);
    return sh;
}

SDL_GPUComputePipeline *embi_gpu_make_compute(embi_gpu *g, const embi_compute_program *p)
{
    const embi_shader_blob *b = embi_program_blob(p, g->format);
    embi_gpu_blob_view bv = embi_gpu_blob_open(b);
    if (!bv.code) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU,
                     "embi_gpu_make_compute: no blob for format 0x%x", (unsigned)g->format);
        return nullptr;
    }

    const SDL_GPUComputePipelineCreateInfo info = {
        .code_size = bv.size,
        .code = bv.code,
        .entrypoint = b->entry,
        .format = g->format,
        .num_samplers = p->num_samplers,
        .num_readonly_storage_textures = p->num_ro_storage_textures,
        .num_readonly_storage_buffers = p->num_ro_storage_buffers,
        .num_readwrite_storage_textures = p->num_rw_storage_textures,
        .num_readwrite_storage_buffers = p->num_rw_storage_buffers,
        .num_uniform_buffers = p->num_uniform_buffers,
        .threadcount_x = p->threads_x,
        .threadcount_y = p->threads_y,
        .threadcount_z = p->threads_z,
    };
    SDL_GPUComputePipeline *cp = SDL_CreateGPUComputePipeline(g->device, &info);
    embi_gpu_blob_close(&bv);
    return cp;
}

SDL_GPUGraphicsPipeline *embi_gpu_make_graphics(embi_gpu *g, const embi_shader_program *p,
                                                SDL_GPUTextureFormat color_format)
{
    SDL_GPUShader *vs = embi_gpu_make_shader(g, &p->vs, SDL_GPU_SHADERSTAGE_VERTEX);
    if (!vs) {
        return nullptr;
    }
    SDL_GPUShader *fs = embi_gpu_make_shader(g, &p->fs, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (!fs) {
        SDL_ReleaseGPUShader(g->device, vs);
        return nullptr;
    }

    const SDL_GPUColorTargetDescription color = {.format = color_format};
    const SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                             .cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &color, .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);

    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    return pipeline;
}
