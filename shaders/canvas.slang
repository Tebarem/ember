// Canvas 2D replay passes (R4b, src/canvas/cgpu.c): compositing with the Porter-Duff operators and the blend modes of
// globalCompositeOperation, gradients and patterns (also drawImage), the clip mask intersection, and the tint of a shadow.
// Every pass is one full-screen triangle limited by the scissor and writes the final premultiplied color of its pixels
// (the pipeline does not blend). Textures are premultiplied one-layer 2D arrays, bound through the same dual spelling as
// fx.slang (a combined sampler on SPIR-V, separate texture and sampler on DXIL and Metal). Slot 0 and 1 are the main
// sources, slot 2 the clip mask. Texels are always read at their centers (SampleLevel on a linear sampler), so the
// filtering of images is done here.
//
// Uniforms (fragment, one block; mirrored by struct cgpu_params in cgpu.c):
//   gMode.x    pass: 0 composite, 1 paint, 2 clip mask product, 3 shadow tint
//   gMode.y    composite operator (CV_OP_* order of canvas_int.h)
//   gMode.z    paint kind: 0 linear, 1 radial, 2 conic, 3 image
//   gMode.w    flags: 1 clip mask on, 2 opaque canvas, 4 smooth images, 8 repeat x, 16 repeat y, 32 clamp to gSrc
//   gSz.xy     texture 0 size, gSz.zw texture 1 size; gSz2.xy texture 2 size, gSz2.zw the canvas size
//   gA         x global alpha, y stop count, zw shadow offset
//   gColor     shadow color, straight
//   gInv0/gInv1  device pixel to paint space: a b c d / e f
//   gGeom0/gGeom1 gradient geometry (see cgpu.c)
//   gSrc       image: source rect to clamp to (x0, y0, x1, y1) in image px
//   gStopOff   stop offsets (32 floats), gStopCol stop colors (straight, 32)
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0
//! fs: samplers=3 storage_textures=0 storage_buffers=0 uniform_buffers=1

[[vk::binding(0, 3)]] cbuffer CanvasParams : register(b0, space3) {
    float4 gMode;
    float4 gSz;
    float4 gSz2;
    float4 gA;
    float4 gColor;
    float4 gInv0;
    float4 gInv1;
    float4 gGeom0;
    float4 gGeom1;
    float4 gSrc;
    float4 gStopOff[8];
    float4 gStopCol[32];
};

[[vk::binding(0, 2)]] Sampler2DArray<float4> gT0Combined : register(t31, space2);
[[vk::binding(20, 2)]] Texture2DArray<float4> gT0Tex : register(t0, space2);
[[vk::binding(21, 2)]] SamplerState gT0Samp : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2DArray<float4> gT1Combined : register(t30, space2);
[[vk::binding(22, 2)]] Texture2DArray<float4> gT1Tex : register(t1, space2);
[[vk::binding(23, 2)]] SamplerState gT1Samp : register(s1, space2);
[[vk::binding(2, 2)]] Sampler2DArray<float4> gT2Combined : register(t29, space2);
[[vk::binding(24, 2)]] Texture2DArray<float4> gT2Tex : register(t2, space2);
[[vk::binding(25, 2)]] SamplerState gT2Samp : register(s2, space2);

struct VSOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID)
{
    const float2 c = float2(float((vid << 1u) & 2u), float(vid & 2u));
    VSOut o;
    o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float4 sample0(float2 uv)
{
    const float3 uvl = float3(uv, 0.0);
    __target_switch
    {
    case spirv:
        return gT0Combined.SampleLevel(uvl, 0);
    default:
        return gT0Tex.SampleLevel(gT0Samp, uvl, 0);
    }
}

float4 sample1(float2 uv)
{
    const float3 uvl = float3(uv, 0.0);
    __target_switch
    {
    case spirv:
        return gT1Combined.SampleLevel(uvl, 0);
    default:
        return gT1Tex.SampleLevel(gT1Samp, uvl, 0);
    }
}

float4 sample2(float2 uv)
{
    const float3 uvl = float3(uv, 0.0);
    __target_switch
    {
    case spirv:
        return gT2Combined.SampleLevel(uvl, 0);
    default:
        return gT2Tex.SampleLevel(gT2Samp, uvl, 0);
    }
}

// ------------------------------------------------------------------ blend functions

float3 blendSoftLight(float3 cb, float3 cs)
{
    const float3 d = select(cb <= 0.25, ((16.0 * cb - 12.0) * cb + 4.0) * cb, sqrt(cb));
    return select(cs <= 0.5, cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb), cb + (2.0 * cs - 1.0) * (d - cb));
}

float blendDodge(float cb, float cs)
{
    if (cb == 0.0) {
        return 0.0;
    }
    if (cs >= 1.0) {
        return 1.0;
    }
    return min(1.0, cb / (1.0 - cs));
}

float blendBurn(float cb, float cs)
{
    if (cb >= 1.0) {
        return 1.0;
    }
    if (cs <= 0.0) {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - cb) / cs);
}

float lum(float3 c)
{
    return dot(c, float3(0.3, 0.59, 0.11));
}

float3 clipColor(float3 c)
{
    const float l = lum(c);
    const float n = min(c.r, min(c.g, c.b));
    const float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) {
        c = l + (c - l) * l / (l - n);
    }
    if (x > 1.0) {
        c = l + (c - l) * (1.0 - l) / (x - l);
    }
    return c;
}

float3 setLum(float3 c, float l)
{
    return clipColor(c + (l - lum(c)));
}

float sat(float3 c)
{
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 setSat(float3 c, float s)
{
    const float mx = max(c.r, max(c.g, c.b));
    const float mn = min(c.r, min(c.g, c.b));
    if (mx > mn) {
        return (c - mn) * s / (mx - mn);
    }
    return float3(0.0, 0.0, 0.0);
}

// B(cb, cs) of the blend mode op (the colors are unpremultiplied).
float3 blendFn(uint op, float3 cb, float3 cs)
{
    switch (op) {
    case 11u: return cb * cs;                       // multiply
    case 12u: return cb + cs - cb * cs;             // screen
    case 13u: return cs;                           // overlay: see blendOverlay
    case 14u: return min(cb, cs);                   // darken
    case 15u: return max(cb, cs);                   // lighten
    case 16u: return float3(blendDodge(cb.r, cs.r), blendDodge(cb.g, cs.g), blendDodge(cb.b, cs.b));
    case 17u: return float3(blendBurn(cb.r, cs.r), blendBurn(cb.g, cs.g), blendBurn(cb.b, cs.b));
    case 18u: return select(cs <= 0.5, cb * 2.0 * cs, cb + (2.0 * cs - 1.0) - cb * (2.0 * cs - 1.0)); // hard-light
    case 19u: return blendSoftLight(cb, cs);
    case 20u: return abs(cb - cs);                  // difference
    case 21u: return cb + cs - 2.0 * cb * cs;       // exclusion
    case 22u: return setLum(setSat(cs, sat(cb)), lum(cb));  // hue
    case 23u: return setLum(setSat(cb, sat(cs)), lum(cb));  // saturation
    case 24u: return setLum(cs, lum(cb));           // color
    case 25u: return setLum(cb, lum(cs));           // luminosity
    default: return cs;
    }
}

// Overlay is hard-light with cb and cs exchanged.
float3 blendOverlay(float3 cb, float3 cs)
{
    return select(cb <= 0.5, cs * 2.0 * cb, cs + (2.0 * cb - 1.0) - cs * (2.0 * cb - 1.0));
}

// The composite of premultiplied source cs and backdrop cb (both with alpha in .a).
float4 composite(uint op, float4 s, float4 b)
{
    const float as = s.a;
    const float ab = b.a;
    if (op < 11u) {
        float fa = 1.0;
        float fb = 1.0 - as;
        switch (op) {
        case 1u: fa = ab; fb = 0.0; break;
        case 2u: fa = 1.0 - ab; fb = 0.0; break;
        case 3u: fa = ab; fb = 1.0 - as; break;
        case 4u: fa = 1.0 - ab; fb = 1.0; break;
        case 5u: fa = 0.0; fb = as; break;
        case 6u: fa = 0.0; fb = 1.0 - as; break;
        case 7u: fa = 1.0 - ab; fb = as; break;
        case 8u: fa = 1.0; fb = 1.0; break;
        case 9u: fa = 1.0; fb = 0.0; break;
        case 10u: fa = 1.0 - ab; fb = 1.0 - as; break;
        default: break;
        }
        return clamp(fa * s + fb * b, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, 1.0));
    }
    const float3 cs = as > 0.0 ? s.rgb / as : float3(0.0, 0.0, 0.0);
    const float3 cb = ab > 0.0 ? b.rgb / ab : float3(0.0, 0.0, 0.0);
    float3 bl = blendFn(op, cb, cs);
    if (op == 13u) {
        bl = blendOverlay(cb, cs);
    }
    const float3 rgb = (1.0 - ab) * s.rgb + (1.0 - as) * b.rgb + as * ab * bl;
    const float a = as + ab * (1.0 - as);
    return float4(rgb, a);
}

// ------------------------------------------------------------------ paints

float4 gradientColor(float t, uint n)
{
    if (n == 0u) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float4 c = gStopCol[0];
    if (t >= gStopOff[(n - 1u) >> 2][(n - 1u) & 3u]) {
        c = gStopCol[n - 1u];
    } else if (t >= gStopOff[0][0]) {
        for (uint i = 0u; i + 1u < n; i++) {
            const float o0 = gStopOff[i >> 2][i & 3u];
            const float o1 = gStopOff[(i + 1u) >> 2][(i + 1u) & 3u];
            if (t >= o0 && t < o1) {
                c = lerp(gStopCol[i], gStopCol[i + 1u], (t - o0) / (o1 - o0));
                break;
            }
        }
    }
    return float4(c.rgb * c.a, c.a);
}

// One texel of the image in slot 1 at integer position t, premultiplied.
float4 texelImage(int2 t)
{
    return sample1((float2(t) + 0.5) / gSz.zw);
}

float4 paintImage(float2 q)
{
    const uint flags = (uint)gMode.w;
    const float2 size = gSz.zw;
    const bool rx = (flags & 8u) != 0u;
    const bool ry = (flags & 16u) != 0u;
    if (!rx && (q.x < 0.0 || q.x >= size.x)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    if (!ry && (q.y < 0.0 || q.y >= size.y)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float2 lo = float2(0.0, 0.0);
    float2 hi = size - 1.0;
    if ((flags & 32u) != 0u) {
        lo = gSrc.xy;
        hi = gSrc.zw - 1.0;
    }
    if ((flags & 4u) == 0u) {
        int2 t = int2(floor(q));
        if (rx) {
            t.x = int(floor(q.x - size.x * floor(q.x / size.x)));
        }
        if (ry) {
            t.y = int(floor(q.y - size.y * floor(q.y / size.y)));
        }
        return texelImage(clamp(t, int2(lo), int2(hi)));
    }
    const float2 p = q - 0.5;
    const float2 f0 = floor(p);
    const float2 fr = p - f0;
    int2 t0 = int2(f0);
    int2 t1 = t0 + 1;
    if (rx) {
        const int w = int(size.x);
        t0.x = ((t0.x % w) + w) % w;
        t1.x = ((t1.x % w) + w) % w;
    }
    if (ry) {
        const int h = int(size.y);
        t0.y = ((t0.y % h) + h) % h;
        t1.y = ((t1.y % h) + h) % h;
    }
    t0 = clamp(t0, int2(lo), int2(hi));
    t1 = clamp(t1, int2(lo), int2(hi));
    const float4 a = texelImage(int2(t0.x, t0.y));
    const float4 b = texelImage(int2(t1.x, t0.y));
    const float4 c = texelImage(int2(t0.x, t1.y));
    const float4 d = texelImage(int2(t1.x, t1.y));
    return lerp(lerp(a, b, fr.x), lerp(c, d, fr.x), fr.y);
}

float4 paintAt(float2 pix)
{
    const float2 dp = pix + 0.5;
    const float2 p = float2(gInv0.x * dp.x + gInv0.z * dp.y + gInv1.x, gInv0.y * dp.x + gInv0.w * dp.y + gInv1.y);
    const uint kind = (uint)gMode.z;
    const uint n = (uint)gA.y;
    if (kind == 3u) {
        return paintImage(p);
    }
    if (kind == 0u) {
        // gGeom0: x0 y0 x1 y1
        const float2 d = gGeom0.zw - gGeom0.xy;
        const float dd = dot(d, d);
        if (dd == 0.0) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        return gradientColor(dot(p - gGeom0.xy, d) / dd, n);
    }
    if (kind == 1u) {
        // gGeom0: x0 y0 r0 x1; gGeom1: y1 r1
        const float2 c0 = gGeom0.xy;
        const float r0 = gGeom0.z;
        const float2 c1 = float2(gGeom0.w, gGeom1.x);
        const float r1 = gGeom1.y;
        const float2 cd = c1 - c0;
        const float dr = r1 - r0;
        const float2 pd = p - c0;
        const float a = dot(cd, cd) - dr * dr;
        const float b = dot(pd, cd) + r0 * dr;
        const float c = dot(pd, pd) - r0 * r0;
        float s = 0.0;
        bool ok = false;
        if (abs(a) < 1e-9) {
            if (abs(b) > 1e-12) {
                s = c / (2.0 * b);
                ok = r0 + s * dr >= 0.0;
            }
        } else {
            const float disc = b * b - a * c;
            if (disc >= 0.0) {
                const float sq = sqrt(disc);
                const float s1 = (b + sq) / a;
                const float s2 = (b - sq) / a;
                const float hi = max(s1, s2);
                const float lo = min(s1, s2);
                if (r0 + hi * dr >= 0.0) {
                    s = hi;
                    ok = true;
                } else if (r0 + lo * dr >= 0.0) {
                    s = lo;
                    ok = true;
                }
            }
        }
        if (!ok) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        return gradientColor(s, n);
    }
    // conic: gGeom0: start angle, cx, cy
    const float2 v = p - gGeom0.yz;
    float t = (atan2(v.y, v.x) - gGeom0.x) / 6.28318530718;
    t = t - floor(t);
    return gradientColor(t, n);
}

float4 fsMain(VSOut i) : SV_Target
{
    const float2 pix = floor(i.pos.xy);
    const uint mode = (uint)gMode.x;
    if (mode == 0u) {
        const uint flags = (uint)gMode.w;
        const float4 s = sample0((pix + 0.5) / gSz.xy) * gA.x;
        const float4 b = sample1((pix + 0.5) / gSz.zw);
        float4 r = composite((uint)gMode.y, s, b);
        if ((flags & 1u) != 0u) {
            const float m = sample2((pix + 0.5) / gSz2.xy).a;
            r = lerp(b, r, m);
        }
        if ((flags & 2u) != 0u) {
            r.a = 1.0;
        }
        return r;
    }
    if (mode == 1u) {
        const float cov = sample0((pix + 0.5) / gSz.xy).a;
        if (cov <= 0.0) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        return paintAt(pix) * cov;
    }
    if (mode == 2u) {
        const float a = sample0((pix + 0.5) / gSz.xy).a * sample1((pix + 0.5) / gSz.zw).a;
        return float4(a, a, a, a);
    }
    // shadow tint: the alpha of the blurred source at pix - offset
    const float2 sp = pix - gA.zw;
    float a = 0.0;
    if (sp.x >= 0.0 && sp.y >= 0.0 && sp.x < gSz2.z && sp.y < gSz2.w) {
        a = sample0((sp + 0.5) / gSz.xy).a * gColor.a;
    }
    return float4(gColor.rgb * a, a);
}
