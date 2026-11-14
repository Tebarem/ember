// Renderer data layout. Mirrors src/gpu/render_types.h field for field. Keep the two in sync:
// the C side has static_asserts on the sizes (64, 32, 48, 64, 32, 8, 16 bytes).
//
// One prim layout serves box prims and outline prims (glyphs and paths). See embi_prim in
// render_types.h for the field meaning of each kind.
//
// Storage buffers are read as ByteAddressBuffer with explicit byte offsets, never as
// StructuredBuffer. SDL_gpu creates every storage-buffer SRV as a raw R32 view (D3D12,
// SDL_gpu_d3d12.c, D3D12_BUFFER_SRV_FLAG_RAW, StructureByteStride = 0), so a StructuredBuffer
// read in DXIL always returned element 0. Byte addressing works the same on SPIR-V and MSL.
//
// Every loader below reads one element of a layout from a buffer at element index i.

struct Prim {
    float4 rect;     // box: x, y, w, h (local CSS px). outline: em box x0, y0, x1, y1
    float4 radii;    // box: tl, tr, br, bl. outline: a, b, c, d (em -> local, CSS order)
    uint color;      // RGBA8, non-premultiplied, r in the low byte
    uint kind;       // low 7 bits = KIND_*, bit 7 = KIND_OUTLINE, bits 8.. = per-kind index
    uint xform;
    uint clip;
    float4 extra;    // box image: uv rect. outline: e, f in xy (see extraBits for z, w)
    uint4 extraBits; // the raw words of extra: outline z = slug byte offset, w = band word
};

struct Xform {
    float a;
    float b;
    float c;
    float d;
    float e;
    float f;
    float padX;      // |J^-T e1|: local distance per device px along the local x normal
    float padY;      // |J^-T e2|: same for the local y normal. padX == 0 means degenerate.
};

struct Clip {
    float4 rect;
    float4 radii;
    uint xform;
    uint parent;
    uint pad0;
    uint pad1;
};

struct Border {
    float4 widths;   // top, right, bottom, left
    uint4 colors;    // per side: the outer-half color (the whole side for most styles)
    uint4 inner;     // per side: the inner-half color (equal to colors except groove and ridge)
    uint4 pattern;   // per side: 0 solid, 1 dotted, 2 dashed, 3 double (EMBI_BPAT_*)
};

struct Gradient {
    float4 p;        // p0.xy, p1.xy
    uint firstStop;
    uint stopCount;
    uint repeat;
    uint pad;
};

struct Stop {
    float offset;
    uint color;
};

struct FrameParams {
    float2 viewport; // device px
    float2 pad;
};

// Longest clip chain walked by the fragment shader. Same as EMBI_MAX_CLIP_DEPTH in
// render_types.h.
static const uint MAX_CLIP_DEPTH = 8;

// Kind values, same as EMBI_PRIM_* in render_types.h.
static const uint KIND_FILL = 0;
static const uint KIND_BORDER = 1;
static const uint KIND_IMAGE = 2;
static const uint KIND_LINEAR = 3;
static const uint KIND_RADIAL = 4;
static const uint KIND_CONIC = 5;
static const uint KIND_SHADOW = 6;
// A layer or image through a projective map (EMBI_PRIM_PLANE): xform i holds X and Y, xform i + 1 holds W.
static const uint KIND_PLANE = 12;
static const uint KIND_IMAGE_REC = 13;
// Outline flag (EMBI_PRIM_OUTLINE) and the mask of the kind bits below it.
static const uint KIND_OUTLINE = 0x80u;
static const uint KIND_MASK = 0x7Fu;

// Unpacks an RGBA8 value (r in the low byte) into floats in [0, 1].
float4 embiUnpackRGBA8(uint c)
{
    return float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u),
                  float((c >> 24) & 255u)) * (1.0 / 255.0);
}

Prim loadPrim(ByteAddressBuffer b, uint i)
{
    const uint o = i * 64u;
    Prim p;
    p.rect = asfloat(b.Load4(o));
    p.radii = asfloat(b.Load4(o + 16u));
    p.color = b.Load(o + 32u);
    p.kind = b.Load(o + 36u);
    p.xform = b.Load(o + 40u);
    p.clip = b.Load(o + 44u);
    p.extraBits = b.Load4(o + 48u);
    p.extra = asfloat(p.extraBits);
    return p;
}

Xform loadXform(ByteAddressBuffer b, uint i)
{
    const uint o = i * 32u;
    const float4 v0 = asfloat(b.Load4(o));
    const float4 v1 = asfloat(b.Load4(o + 16u));
    Xform x;
    x.a = v0.x;
    x.b = v0.y;
    x.c = v0.z;
    x.d = v0.w;
    x.e = v1.x;
    x.f = v1.y;
    x.padX = v1.z;
    x.padY = v1.w;
    return x;
}

Clip loadClip(ByteAddressBuffer b, uint i)
{
    const uint o = i * 48u;
    Clip c;
    c.rect = asfloat(b.Load4(o));
    c.radii = asfloat(b.Load4(o + 16u));
    c.xform = b.Load(o + 32u);
    c.parent = b.Load(o + 36u);
    c.pad0 = b.Load(o + 40u);
    c.pad1 = b.Load(o + 44u);
    return c;
}

Border loadBorder(ByteAddressBuffer b, uint i)
{
    const uint o = i * 64u;
    Border r;
    r.widths = asfloat(b.Load4(o));
    r.colors = b.Load4(o + 16u);
    r.inner = b.Load4(o + 32u);
    r.pattern = b.Load4(o + 48u);
    return r;
}

// An image record (EMBI_PRIM_IMAGE_REC, embi_image_rec in render_types.h): the uv clamp rect, the size of one
// texel in uv, the atlas layer and the nearest flag.
struct ImageRec {
    float4 clamp;
    float2 texel;
    uint layer;
    uint nearest;
};

ImageRec loadImageRec(ByteAddressBuffer b, uint i)
{
    const uint o = i * 64u;
    ImageRec r;
    r.clamp = asfloat(b.Load4(o));
    r.texel = asfloat(b.Load2(o + 16u));
    r.layer = b.Load(o + 24u);
    r.nearest = b.Load(o + 28u);
    return r;
}

Gradient loadGradient(ByteAddressBuffer b, uint i)
{
    const uint o = i * 32u;
    Gradient g;
    g.p = asfloat(b.Load4(o));
    g.firstStop = b.Load(o + 16u);
    g.stopCount = b.Load(o + 20u);
    g.repeat = b.Load(o + 24u);
    g.pad = b.Load(o + 28u);
    return g;
}

Stop loadStop(ByteAddressBuffer b, uint i)
{
    const uint o = i * 8u;
    Stop s;
    s.offset = asfloat(b.Load(o));
    s.color = b.Load(o + 4u);
    return s;
}

FrameParams loadFrame(ByteAddressBuffer b, uint i)
{
    const uint o = i * 16u;
    FrameParams f;
    const float4 v = asfloat(b.Load4(o));
    f.viewport = v.xy;
    f.pad = v.zw;
    return f;
}
