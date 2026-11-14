// Filter effect passes (R3a, src/gpu/fx.c): separable Gaussian blur, 2x scaling, a color matrix and
// the drop-shadow composite. Every pass reads one or two layer textures (premultiplied) and writes the
// content rect of a layer texture with the pipeline's replace blending, so a layer is never blended
// into. The vertex stage draws one full-screen triangle; fx.c limits it with the scissor.
//
// Sampled layers are one-layer 2D arrays, bound through the same dual spelling as box.slang (a combined
// sampler on SPIR-V, separate texture and sampler on DXIL and Metal). Slot 0 is the main source, slot 1
// the second source of the drop-shadow composite (the blurred copy), bound to the same texture otherwise.
//
// Uniforms (fragment, one block; see struct embi_fx_params in fx.c, which mirrors it):
//   gMode.x    mode: 0 blur, 1 scale, 2 matrix, 3 drop-shadow composite, 4 blend (R3b), 5 mask apply, 6 mask luminance,
//              7 mask combine
//   mode 8     plus-lighter: min(1, source * gOff.y + backdrop), the premultiplied sum (slot 0 source, slot 1 backdrop)
//   gOff.x     blend: the blend mode (embi_fx_blend_mode in fx.h); mask combine: the composite operator (0 add,
//              1 subtract, 2 intersect, 3 exclude)
//   gMode.y    blur: the kernel radius in taps (floor 3 sigma + 1, at most 32)
//   gMode.z    blur: sigma in texels
//   gMode.w    1: edge mode duplicate (the content edge repeats outward) instead of transparent
//   gDir       blur: the tap step in texels (dx, dy); scale: the source step per destination px (sx, sy)
//   gSrc       the source content size (w, h) and the source texture size (tw, th)
//   gOff       drop-shadow: the shadow offset in px (dx, dy)
//   gM0..gM3   matrix: rows producing r, g, b, a from (r, g, b, a); gBias: the added column
//   gColor     drop-shadow: the shadow color, non-premultiplied
//! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0
//! fs: samplers=2 storage_textures=0 storage_buffers=0 uniform_buffers=1

[[vk::binding(0, 3)]] cbuffer FxParams : register(b0, space3) {
    float4 gMode;
    float4 gDir;
    float4 gSrc;
    float4 gOff;
    float4 gM0;
    float4 gM1;
    float4 gM2;
    float4 gM3;
    float4 gBias;
    float4 gColor;
};

[[vk::binding(0, 2)]] Sampler2DArray<float4> gT0Combined : register(t31, space2);
[[vk::binding(20, 2)]] Texture2DArray<float4> gT0Tex : register(t0, space2);
[[vk::binding(21, 2)]] SamplerState gT0Samp : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2DArray<float4> gT1Combined : register(t30, space2);
[[vk::binding(22, 2)]] Texture2DArray<float4> gT1Tex : register(t1, space2);
[[vk::binding(23, 2)]] SamplerState gT1Samp : register(s1, space2);

struct VSOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID)
{
    // One triangle covering the target: (-1,-1), (3,-1), (-1,3).
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

// The texel at integer position t of the main source: zero outside the content rect.
float4 texel0(float2 t)
{
    if (gMode.w > 0.5) {
        // Edge mode duplicate (backdrop-filter): the content edge repeats outward.
        t = clamp(t, float2(0.0, 0.0), gSrc.xy - 1.0);
        return sample0((t + 0.5) / gSrc.zw);
    }
    if (t.x < 0.0 || t.y < 0.0 || t.x >= gSrc.x || t.y >= gSrc.y) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return sample0((t + 0.5) / gSrc.zw);
}


// ---- blend modes (R3b, the compositing spec's formulas on unpremultiplied colors) ----

float3 hardLight(float3 cb, float3 cs)
{
    const float3 lo = cb * (2.0 * cs);
    const float3 s2 = 2.0 * cs - 1.0;
    const float3 hi = cb + s2 - cb * s2;
    return float3(cs.x <= 0.5 ? lo.x : hi.x, cs.y <= 0.5 ? lo.y : hi.y, cs.z <= 0.5 ? lo.z : hi.z);
}

float softLightD(float x)
{
    return x <= 0.25 ? ((16.0 * x - 12.0) * x + 4.0) * x : sqrt(x);
}

float softLight1(float cb, float cs)
{
    if (cs <= 0.5) {
        return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
    }
    return cb + (2.0 * cs - 1.0) * (softLightD(cb) - cb);
}

float dodge1(float cb, float cs)
{
    if (cb <= 0.0) {
        return 0.0;
    }
    if (cs >= 1.0) {
        return 1.0;
    }
    return min(1.0, cb / (1.0 - cs));
}

float burn1(float cb, float cs)
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
    return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z;
}

float3 clipColor(float3 c)
{
    const float l = lum(c);
    const float n = min(c.x, min(c.y, c.z));
    const float x = max(c.x, max(c.y, c.z));
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
    return max(c.x, max(c.y, c.z)) - min(c.x, min(c.y, c.z));
}

float3 setSat(float3 c, float s)
{
    const float mx = max(c.x, max(c.y, c.z));
    const float mn = min(c.x, min(c.y, c.z));
    if (mx <= mn) {
        return float3(0.0, 0.0, 0.0);
    }
    return (c - mn) * s / (mx - mn);
}

// Blend modes by number: 0 normal, 1 multiply, 2 screen, 3 overlay, 4 darken, 5 lighten, 6 color-dodge, 7 color-burn,
// 8 hard-light, 9 soft-light, 10 difference, 11 exclusion, 12 hue, 13 saturation, 14 color, 15 luminosity,
// 16 plus-lighter, 17 plus-darker.
float3 blendFn(uint m, float3 cb, float3 cs)
{
    switch (m) {
    case 1u:
        return cb * cs;
    case 2u:
        return cb + cs - cb * cs;
    case 3u:
        return hardLight(cs, cb);
    case 4u:
        return min(cb, cs);
    case 5u:
        return max(cb, cs);
    case 6u:
        return float3(dodge1(cb.x, cs.x), dodge1(cb.y, cs.y), dodge1(cb.z, cs.z));
    case 7u:
        return float3(burn1(cb.x, cs.x), burn1(cb.y, cs.y), burn1(cb.z, cs.z));
    case 8u:
        return hardLight(cb, cs);
    case 9u:
        return float3(softLight1(cb.x, cs.x), softLight1(cb.y, cs.y), softLight1(cb.z, cs.z));
    case 10u:
        return abs(cb - cs);
    case 11u:
        return cb + cs - 2.0 * cb * cs;
    case 12u:
        return setLum(setSat(cs, sat(cb)), lum(cb));
    case 13u:
        return setLum(setSat(cb, sat(cs)), lum(cb));
    case 14u:
        return setLum(cs, lum(cb));
    case 15u:
        return setLum(cb, lum(cs));
    case 16u:
        return min(cb + cs, float3(1.0, 1.0, 1.0));
    case 17u:
        return max(cb + cs - 1.0, float3(0.0, 0.0, 0.0));
    default:
        return cs;
    }
}

float4 fsMain(VSOut i) : SV_Target
{
    const float2 pix = floor(i.pos.xy);
    const uint mode = (uint)gMode.x;
    if (mode == 0u) {
        // Separable Gaussian along gDir. Taps outside the content are transparent, and the weights
        // are normalized over the whole kernel, so the edge fades out (CSS blur edge mode: none).
        const int radius = (int)gMode.y;
        const float sigma = max(gMode.z, 1e-4);
        const float k = -0.5 / (sigma * sigma);
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        float wsum = 0.0;
        for (int j = -radius; j <= radius; j++) {
            const float w = exp(k * float(j * j));
            sum += w * texel0(pix + gDir.xy * float(j));
            wsum += w;
        }
        return sum / wsum;
    }
    if (mode == 1u) {
        // Bilinear read at the destination center scaled to the source: a 2x2 box average when the
        // step is 2, an upsample when it is 0.5.
        float2 sp = (pix + 0.5) * gDir.xy;
        if (gMode.w > 0.5) {
            sp = clamp(sp, float2(0.5, 0.5), gSrc.xy - 0.5);
        }
        return sample0(sp / gSrc.zw);
    }
    if (mode == 2u) {
        const float4 c = texel0(pix);
        float3 rgb = float3(0.0, 0.0, 0.0);
        if (c.a > 0.0) {
            rgb = c.rgb / c.a;
        }
        const float4 u = float4(rgb, c.a);
        float4 o = float4(dot(gM0, u), dot(gM1, u), dot(gM2, u), dot(gM3, u)) + gBias;
        o = clamp(o, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, 1.0));
        return float4(o.rgb * o.a, o.a);
    }
    if (mode == 4u) {
        // Blend (R3b): the source layer (slot 0) blended onto the backdrop copy (slot 1) at the same pixel. The
        // result is the source after blending, premultiplied by its own alpha, so that the source-over composite
        // that draws it into the target gives the compositing spec's result.
        const float2 uv = (pix + 0.5) / gSrc.zw;
        const float4 s = sample0(uv);
        if (s.a <= 0.0) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        const float4 b = sample1(uv);
        const float3 cs = min(s.rgb / s.a, float3(1.0, 1.0, 1.0));
        const float3 cb = b.a > 0.0 ? min(b.rgb / b.a, float3(1.0, 1.0, 1.0)) : float3(0.0, 0.0, 0.0);
        const float3 bl = blendFn(uint(gOff.x + 0.5), cb, cs);
        const float3 mixed = (1.0 - b.a) * cs + b.a * bl;
        return float4(mixed * s.a, s.a);
    }
    if (mode == 8u) {
        // Plus-lighter (R3b): the premultiplied sum of the source (scaled by the opacity gOff.y) and the backdrop copy. The
        // result replaces the backdrop, so it cannot go through the source-over composite.
        const float2 uv = (pix + 0.5) / gSrc.zw;
        return min(sample0(uv) * gOff.y + sample1(uv), float4(1.0, 1.0, 1.0, 1.0));
    }
    if (mode == 5u) {
        // Mask apply: the source scaled by the alpha of the mask (slot 1).
        const float2 uv = (pix + 0.5) / gSrc.zw;
        return sample0(uv) * sample1(uv).a;
    }
    if (mode == 6u) {
        // Mask luminance: the alpha of the result is the luminance of the premultiplied source (luminance times alpha).
        const float4 s = sample0((pix + 0.5) / gSrc.zw);
        return float4(0.0, 0.0, 0.0, dot(s.rgb, float3(0.2125, 0.7154, 0.0721)));
    }
    if (mode == 7u) {
        // Mask combine: the top layer (slot 0) composited with the layers below it (slot 1), alpha only.
        const float2 uv = (pix + 0.5) / gSrc.zw;
        const float a = sample0(uv).a;
        const float c = sample1(uv).a;
        const uint op = uint(gOff.x + 0.5);
        float r = a + c * (1.0 - a);
        if (op == 1u) {
            r = a * (1.0 - c);
        } else if (op == 2u) {
            r = a * c;
        } else if (op == 3u) {
            r = a * (1.0 - c) + c * (1.0 - a);
        }
        return float4(0.0, 0.0, 0.0, r);
    }
    // Drop-shadow composite: the blurred alpha of the source, tinted, offset, under the source.
    const float4 src = texel0(pix);
    const float2 sp = pix - gOff.xy;
    float sh = 0.0;
    if (sp.x >= 0.0 && sp.y >= 0.0 && sp.x < gSrc.x && sp.y < gSrc.y) {
        sh = sample1((sp + 0.5) / gSrc.zw).a * gColor.a;
    }
    const float4 shadow = float4(gColor.rgb * sh, sh);
    return src + shadow * (1.0 - src.a);
}
