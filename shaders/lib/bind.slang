// Resource bindings for the renderer ubershader (shaders/box.slang).
//
// SDL_gpu binding rules (SDL_CreateGPUShader docs), used here:
//   SPIR-V  vertex:   set 0, binding n = storage buffer n.   fragment: set 2, binding n.
//   DXIL    vertex:   space0, t<n>.                          fragment: space2, t<n>.
//   MSL     [[buffer(n)]]: uniform buffers first, then storage buffers, in slot order.
//           [[texture(n)]] and [[sampler(n)]] are separate index spaces.
//
// Vertex stage (no textures, no uniform buffers): storage buffers, SDL slot k = register k.
//   0 prims, 1 transforms, 2 frame params (one element).
//   SPIR-V set 0 binding k, DXIL t<k> space0, MSL buffer(k). The three targets agree.
//
// Fragment stage: SDL puts the sampled textures first, then the storage buffers. The layout is:
//   sampled texture 0 (the atlas array, one layer per page), with its sampler:
//       SPIR-V set 2 binding 0 (combined image sampler), DXIL t0 and s0 in space2, MSL texture(0)
//       and sampler(0).
//   storage buffers, SDL slot k = storage index k, bound at SDL slot 0 on every backend:
//       0 prims      SPIR-V binding 1, DXIL t1, MSL buffer(0)
//       1 transforms SPIR-V binding 2, DXIL t2, MSL buffer(1)
//       2 clips      SPIR-V binding 3, DXIL t3, MSL buffer(2)
//       3 borders    SPIR-V binding 4, DXIL t4, MSL buffer(3)
//       4 gradients  SPIR-V binding 5, DXIL t5, MSL buffer(4)
//       5 stops      SPIR-V binding 6, DXIL t6, MSL buffer(5)
//       6 slug blobs SPIR-V binding 7, DXIL t7, MSL buffer(6) (outline prims; bound in every batch)
//       7 channel    SPIR-V binding 8, DXIL t8, MSL buffer(7) (the subpixel offset of the LCD text
//                    pass; bound in every batch, read only by LCD text prims)
//   SPIR-V and DXIL count the sampled texture (and its sampler) before the storage buffers, so the
//   buffers start at 1. Metal numbers its buffers from 0 and SDL's Metal backend copies the
//   storage buffers from array[0..num_storage-1] into buffer(0..num_storage-1) (SDL_gpu_metal.m,
//   setFragmentBuffers with range {num_uniform, num_storage}). So on Metal the buffers must be
//   numbered 0..5, and SDL slot 0 maps to buffer(0).
//
//   Slang's Metal buffer index is the D3D register number, and it ignores [[vk::binding]]
//   (checked with Slang 2026.10.2: register(t5) -> buffer(5), register(t0) -> buffer(0), with a
//   vk::binding of 1 in both cases). Slang has no target macro, so the fragment buffers are
//   declared twice, once per numbering. The unused set is dropped from the output, so its binding
//   numbers never reach the shader (the same trick box.slang uses for the atlas). The EMB_FS_SSBO
//   macro declares both sets, and fsPrims() and the other accessors pick one with __target_switch.
//   The macro arguments are: the name, the SPIR-V/DXIL register and binding n (1..6), a distinct
//   SPIR-V binding number for the Metal set (only needs to differ from the others in its set),
//   and the Metal buffer index m (n - 1).
//
// Every storage buffer is a ByteAddressBuffer (read with the loaders in types.slang). Raw
// views are what SDL_gpu creates for storage buffers on D3D12, and Slang maps ByteAddressBuffer
// to a plain storage buffer on SPIR-V and to a device byte pointer on Metal.
//
// Verified with Slang 2026.10.2:
//   Sampler2D (combined) emits OpTypeSampledImage at set 2 binding 0 on SPIR-V. On DXIL it puts
//   the sampler in space0, which SDL does not read, so the combined form is used on SPIR-V only
//   (box.slang, __target_switch). Separate Texture2D and SamplerState with explicit registers
//   give t0 space2 and s0 space2 on DXIL, and texture(0) and sampler(0) on Metal.
//   Metal: [[buffer(m)]] follows the register number (see above). The fragment accessors compile
//   to buffer(0..5) on -target metal (checked by reading the generated MSL).
//   -target dxil: checked with dxc -dumpbin (Resource Bindings).
//   Metal is checked by compilation only (on this Windows box). A Metal device run is still to
//   come. The macOS CI job builds the msl blobs.

#define EMB_VS_SSBO(name, n) \
    [[vk::binding(n, 0)]] ByteAddressBuffer name : register(t##n, space0)

// n: SPIR-V binding and DXIL register number (1..6). vkm: the SPIR-V binding of the Metal set
// (unused on SPIR-V, any distinct value). m: the Metal buffer index, which is n - 1.
#define EMB_FS_SSBO(name, n, vkm, m) \
    [[vk::binding(n, 2)]] ByteAddressBuffer name##_vk : register(t##n, space2); \
    [[vk::binding(vkm, 2)]] ByteAddressBuffer name##_msl : register(t##m, space7); \
    ByteAddressBuffer name() \
    { \
        __target_switch \
        { \
        case metal: \
            return name##_msl; \
        default: \
            return name##_vk; \
        } \
    }
