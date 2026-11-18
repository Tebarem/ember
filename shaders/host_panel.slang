// The textured quad that the world_panel example (examples/world_panel) draws with the atlas of its two Ember views, and the
// dark gradient behind them. It is the host's own drawing, not part of Ember's renderer: one quad of two triangles per draw.
//
// The vertex stage maps the unit square (corner in [0, 1]^2) through the model-view-projection matrix gMvp, and interpolates
// the texture coordinates of the quad's rectangle of the atlas across it. A draw with gMode.x set is the gradient background:
// its uv runs 0 to 1 over the screen, and the fragment stage computes the color from it instead of sampling.
//
// Panels are premultiplied (the views' pictures are), so the pipeline blends ONE / ONE_MINUS_SRC_ALPHA. The sampler has mips
// (trilinear, from the mip chain the example generates each frame).
//
// Uniforms (vertex, one block; mirrored by struct panel_params in examples/world_panel/main.c):
//   gMvp       model-view-projection, column-major
//   gUvRect    the texture coordinates of the quad's corners: (u0, v0) at the top-left, (u1, v1) at the bottom-right
//   gMode.x    1 for the gradient background, 0 for a textured panel
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=1
//! fs: samplers=1 storage_textures=0 storage_buffers=0 uniform_buffers=0

cbuffer PanelParams : register(b0, space1) {
    column_major float4x4 gMvp;
    float4 gUvRect;
    float4 gMode;
};

// The atlas: one level-chained 2D texture. Sampled through the combined form on SPIR-V and the separate texture and sampler
// on DXIL and Metal (the same spelling as shaders/box.slang: SDL reads fragment samplers from space2).
[[vk::binding(0, 2)]] Sampler2D<float4> gAtlasCombined : register(t31, space2);
[[vk::binding(20, 2)]] Texture2D<float4> gAtlasTex : register(t0, space2);
[[vk::binding(21, 2)]] SamplerState gAtlasSamp : register(s0, space2);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float gradient : TEXCOORD1;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID) {
    // Triangles (0, 1, 2) and (2, 1, 3) over the corners of the unit square; bit 0 is x, bit 1 is y.
    uint idx = vid;
    if (vid == 3u) {
        idx = 2u;
    } else if (vid == 4u) {
        idx = 1u;
    } else if (vid == 5u) {
        idx = 3u;
    }
    float2 corner = float2((idx & 1u) != 0u ? 1.0 : 0.0, (idx & 2u) != 0u ? 1.0 : 0.0);
    VSOut o;
    o.pos = mul(gMvp, float4(corner, 0.0, 1.0));
    o.uv = lerp(gUvRect.xy, gUvRect.zw, corner);
    o.gradient = gMode.x;
    return o;
}

float4 sampleAtlas(float2 uv)
{
    __target_switch
    {
    case spirv:
        return gAtlasCombined.Sample(uv);
    default:
        return gAtlasTex.Sample(gAtlasSamp, uv);
    }
}

[shader("fragment")]
float4 fsMain(VSOut i) : SV_Target {
    if (i.gradient > 0.5) {
        // A dark vertical gradient, opaque.
        const float3 top = float3(0.11, 0.12, 0.18);
        const float3 bottom = float3(0.02, 0.02, 0.04);
        return float4(lerp(top, bottom, i.uv.y), 1.0);
    }
    return sampleAtlas(i.uv);
}
