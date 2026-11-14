#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* code/size are the bytes as embedded. With EMBER_COMPRESS_ASSETS (SZ1) `packed` is true and they are a tools/lz stream
 * (see src/base/lz.c); embi_gpu_blob_open (gpu/gpu.h) returns the code itself either way. */
typedef struct embi_shader_blob {
    const unsigned char *code;
    size_t size;
    const char *entry;
    bool packed;
} embi_shader_blob;

typedef struct embi_shader_stage {
    embi_shader_blob spirv, dxil, msl;
    uint32_t num_samplers, num_storage_textures, num_storage_buffers, num_uniform_buffers;
} embi_shader_stage;

typedef struct embi_shader_program {
    embi_shader_stage vs, fs;
} embi_shader_program;

typedef struct embi_compute_program {
    embi_shader_blob spirv, dxil, msl;
    uint32_t num_samplers, num_ro_storage_textures, num_ro_storage_buffers,
             num_rw_storage_textures, num_rw_storage_buffers, num_uniform_buffers,
             threads_x, threads_y, threads_z;
} embi_compute_program;
