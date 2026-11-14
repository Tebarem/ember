// Pipeline self-test: a fullscreen-covering triangle with a constant color.
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0
//! fs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0

struct VSOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain() : SV_Target {
    return float4(0.0, 1.0, 0.0, 1.0);
}
