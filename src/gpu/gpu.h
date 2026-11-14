#pragma once
/*
 * GPU device wrapper over SDL_gpu. Picks the one shader format that the device and the
 * embedded blobs share (DXIL > SPIR-V > MSL) and builds shaders and pipelines from the blobs
 * that A1 generates (see src/emb_shader_blob.h).
 */

#include <SDL3/SDL_gpu.h>

#include "emb_shader_blob.h"

typedef struct embi_gpu embi_gpu;

/* external == nullptr: create our own device (debug validation when debug is true).
 * external != nullptr: wrap the host's device. embi_gpu_destroy never destroys it.
 * Returns nullptr on failure, after logging the reason. */
[[nodiscard]] embi_gpu *embi_gpu_create(SDL_GPUDevice *external, bool debug);
void embi_gpu_destroy(embi_gpu *g);
[[nodiscard]] SDL_GPUDevice *embi_gpu_device(embi_gpu *g);
/* The single shader format used on this device. Never SDL_GPU_SHADERFORMAT_INVALID. */
[[nodiscard]] SDL_GPUShaderFormat embi_gpu_format(embi_gpu *g);

/* The code of a blob as the driver takes it. Not packed: the blob's own bytes. Packed (EMBER_COMPRESS_ASSETS): an
 * expansion, owned until embi_gpu_blob_close. code is nullptr when the blob is empty or damaged. */
typedef struct embi_gpu_blob_view {
    const unsigned char *code;
    size_t size;
    unsigned char *owned;
    size_t owned_size;
} embi_gpu_blob_view;
embi_gpu_blob_view embi_gpu_blob_open(const embi_shader_blob *b);
void embi_gpu_blob_close(embi_gpu_blob_view *v);

[[nodiscard]] SDL_GPUShader *embi_gpu_make_shader(embi_gpu *g, const embi_shader_stage *s,
                                                  SDL_GPUShaderStage stage);
[[nodiscard]] SDL_GPUComputePipeline *embi_gpu_make_compute(embi_gpu *g,
                                                            const embi_compute_program *p);

/* Added beyond the M0 spec: a triangle-list pipeline with no vertex input, one color target
 * of color_format and no blending. Both shader stages are released after creation. */
[[nodiscard]] SDL_GPUGraphicsPipeline *embi_gpu_make_graphics(embi_gpu *g,
                                                              const embi_shader_program *p,
                                                              SDL_GPUTextureFormat color_format);
