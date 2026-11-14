// The spinning quad that the offscreen example (examples/offscreen) draws itself, beside the view it blits. It is the
// host's own drawing, not part of Ember's renderer: a quad of two triangles, rotated by an angle pushed per frame.
//
// gA: cos(angle), sin(angle), 2 / width, 2 / height (pixels to clip space).
// gB: the centre x and y in pixels (top-left origin), the half size in pixels, unused.
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=1
//! fs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0

cbuffer SpinParams : register(b0, space1) {
    float4 gA;
    float4 gB;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 col : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID) {
    // Triangles (0, 1, 2) and (2, 1, 3) over the corners of the square; bit 0 is x, bit 1 is y.
    uint idx = vid;
    if (vid == 3u) {
        idx = 2u;
    } else if (vid == 4u) {
        idx = 1u;
    } else if (vid == 5u) {
        idx = 3u;
    }
    float2 corner = float2((idx & 1u) != 0u ? 1.0 : -1.0, (idx & 2u) != 0u ? 1.0 : -1.0);
    float2 q = corner * gB.z;
    float2 r = float2(gA.x * q.x - gA.y * q.y, gA.y * q.x + gA.x * q.y);
    float2 p = float2(gB.x, gB.y) + r;
    VSOut o;
    o.pos = float4(p.x * gA.z - 1.0, 1.0 - p.y * gA.w, 0.0, 1.0);
    o.col = float4(0.25 + 0.75 * (corner.x * 0.5 + 0.5), 0.35, 0.95 - 0.5 * (corner.y * 0.5 + 0.5), 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut i) : SV_Target {
    return i.col;
}
