// Covers the top-left half of the target: NDC corners (-1,1), (1,1), (-1,-1), which are the
// pixel corners (0,0), (W,0), (0,H) for a full-target viewport. No resources.
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0
//! fs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0

struct VSOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID) {
    VSOut o;
    float x = (vid == 1u) ? 1.0 : -1.0;
    float y = (vid == 2u) ? -1.0 : 1.0;
    o.pos = float4(x, y, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain() : SV_Target {
    return float4(1.0, 0.0, 0.0, 1.0);
}
