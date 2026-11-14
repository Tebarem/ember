// Ubershader for every primitive: box prims (solid fills, borders in all CSS styles, linear, radial
// and conic gradients, repeating or not, atlas images) and outline prims (glyphs and paths, filled
// by Slug coverage, see lib/outline.slang). One pipeline draws both, in instance order, so paint
// order is exact and a frame of mixed prims is one batch (see the Batching section of R1.md).
//
// Geometry: one instance per prim, 6 vertices per instance (two triangles over the prim's
// quad). The quad is expanded per axis by the transform's pads (xform.padX / padY, see
// render_types.h), so the one device px anti-aliasing fringe survives any affine transform. An
// outline's quad is its em box grown by half a device px per axis, mapped through its em -> local
// map and the transform.
//
// Coverage: each rounded-rect SDF is evaluated in local space together with its analytic local
// gradient g. The device-space distance is then dist = sdf / |J^-T g|, where J is the 2x2 part
// of the transform. This is exact for uniform scales, rotations and axis-aligned non-uniform
// scales (scale(1, 0.1) keeps vertical edges one px wide), and a first-order estimate for
// skews. It needs no derivatives, so it is safe inside the divergent clip loop.
//
// Borders: coverage = outer - inner, clamped at zero. Subtracting is exact when the inner shape
// lies inside the outer one, and it leaves no residue on a zero-width side (a multiplicative
// form would). Each side then takes its pattern coverage (dashes, dots, the two lines of double),
// evaluated along the side's centerline in the side's own frame (see borderPattern), and its shade
// (groove and ridge split a side in two halves across its width). Corner joins blend the shades
// and the patterns of the two sides across the diagonals.
//
// Gradients: the color is evaluated in local space (the gradient geometry is in the element's
// own CSS px, so transforms apply to it for free). Stops are interpolated premultiplied, as CSS
// specifies, so a transparent stop never darkens its neighbors. Stop offsets are gradient
// parameters: the line position t for linear, the ellipse radius for radial, and turns for
// conic. Repeating gradients wrap t over the span from the first to the last stop.
//
// Images: the sample position is the local point clamped to the rect, mapped into the uv rect.
// The atlas stores every image with a one-texel extrusion, so bilinear sampling at the whole image's
// edge reads only the edge texels. A sub-rect (a border-image slice, a tile) has real texels on its
// interior sides, so its image record clamps the uv to half a texel inside the rect. Nearest sampling
// (image-rendering: pixelated) snaps the uv to a texel center instead, with the same linear sampler.
// The sampler is linear, clamp to edge. A composited layer (see gpu/layer.h) is an image of its own
// texture, so it takes this path with its opacity as the tint.
//
// Output is premultiplied. The pipeline blends ONE, ONE_MINUS_SRC_ALPHA.
//! vs: samplers=0 storage_textures=0 storage_buffers=3 uniform_buffers=0
//! fs: samplers=1 storage_textures=0 storage_buffers=8 uniform_buffers=0

#include "lib/paint.slang"
#include "lib/outline.slang"
#include "lib/shadow.slang"

EMB_VS_SSBO(gVsPrims, 0);
EMB_VS_SSBO(gVsXforms, 1);
EMB_VS_SSBO(gVsFrame, 2);

// The Slug blob buffer (src/gpu/slug.c). Fragment storage slot 6, after the six shared slots of
// paint.slang. Every batch binds it, even one without outlines.
EMB_FS_SSBO(fsSlugData, 7, 107, 6);

// Fragment storage slot 7: one float, the subpixel offset in device px (-1/3, 0, 1/3) of the color
// channel the current LCD text draw writes (render.c draws LCD text prims three times, with write
// masks R, G and B, binding a different buffer each time). Unused by every other prim.
EMB_FS_SSBO(fsChan, 8, 108, 7);

// Text prims (solid outline prims with the text bits in the kind word's index field, mirrored from
// render.c): bit 0 text, bit 1 LCD, bits 2..9 total stem darkening in 1/512 device px.
static const uint TEXT_BIT = 1u;
static const uint TEXT_LCD_BIT = 2u;
// Coverage boost per device px of stem darkening: 0.25 px of darkening raises partial coverage by 21%.
static const float DARKEN_BOOST = 0.84;


// Atlas pages: one 2D array texture, one layer per shared page (src/gpu/atlas.c). The layer is the
// third coordinate of the sample. Sampled through the combined form on SPIR-V and the separate
// texture and sampler on DXIL and Metal. The two spellings are needed because Slang's Sampler2DArray
// puts its sampler in space0 on DXIL, while SDL reads fragment samplers from space2. __target_switch
// keeps one set per target, and an unused global is dropped from the output, so the other set's
// binding numbers never reach the shader. The vk binding numbers of the separate set are distinct
// from the combined one only to keep Slang from reporting overlaps. A solo image or a layer is a
// one-layer array of its own, sampled at layer 0 through the same bindings.
[[vk::binding(0, 2)]] Sampler2DArray<float4> gFsAtlasCombined : register(t31, space2);
[[vk::binding(20, 2)]] Texture2DArray<float4> gFsAtlasTex : register(t0, space2);
[[vk::binding(21, 2)]] SamplerState gFsAtlasSamp : register(s0, space2);

struct VSOut {
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;   // local CSS px (box: the point; outline: the em point in local px)
    float2 em : TEXCOORD1;      // outline only: the em point
    nointerpolation uint prim : TEXCOORD2;
    nointerpolation float2 foot : TEXCOORD3;   // outline only: device px per em unit, along em x, y
};


[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Prim p = loadPrim(gVsPrims, iid);
    Xform x = loadXform(gVsXforms, p.xform);
    FrameParams fr = loadFrame(gVsFrame, 0);
    const uint c = vid % 6u;
    const float2 corner = float2((c == 1u || c == 4u || c == 5u) ? 1.0 : 0.0,
                                 (c == 2u || c == 3u || c == 5u) ? 1.0 : 0.0);

    VSOut o;
    o.prim = iid;
    o.em = float2(0.0, 0.0);
    o.foot = float2(0.0, 0.0);
    o.local = float2(0.0, 0.0);

    if ((p.kind & KIND_MASK) == KIND_PLANE && (p.kind & KIND_OUTLINE) == 0u) {
        // A projective plane (3D transforms): X and Y from the first xform record, W = a lx + b ly + c from the
        // second. The clip position carries W, so the rasterizer interpolates the local point perspective-correct.
        const Xform xw = loadXform(gVsXforms, p.xform + 1u);
        const float2 local = lerp(p.rect.xy, p.rect.xy + p.rect.zw, corner);
        const float dx = x.a * local.x + x.c * local.y + x.e;
        const float dy = x.b * local.x + x.d * local.y + x.f;
        const float w = xw.a * local.x + xw.b * local.y + xw.c;
        o.pos = float4(dx / fr.viewport.x * 2.0 - w, w - dy / fr.viewport.y * 2.0, 0.0, w);
        o.local = local;
        return o;
    }

    if ((p.kind & KIND_OUTLINE) == 0u) {
        // Local-space expansion that puts a one device px fringe on every edge, per axis.
        const float2 pad = float2(x.padX, x.padY);
        const float2 lo = p.rect.xy - pad;
        const float2 hi = p.rect.xy + p.rect.zw + pad;
        const float2 local = lerp(lo, hi, corner);

        const float dx = x.a * local.x + x.c * local.y + x.e;
        const float dy = x.b * local.x + x.d * local.y + x.f;

        o.pos = float4(dx / fr.viewport.x * 2.0 - 1.0, 1.0 - dy / fr.viewport.y * 2.0, 0.0, 1.0);
        o.local = local;
        return o;
    }

    // Outline. The device map is D e + T, with D = X L (the 2x2 parts of the transform and of
    // em -> local) and T the translation. X is the transform, which carries the device scale.
    const float la = p.radii.x;
    const float lb = p.radii.y;
    const float lc = p.radii.z;
    const float ld = p.radii.w;
    const float2 linT = p.extra.xy;
    const float D00 = x.a * la + x.c * lb;
    const float D01 = x.a * lc + x.c * ld;
    const float D10 = x.b * la + x.d * lb;
    const float D11 = x.b * lc + x.d * ld;
    const float T0 = x.a * linT.x + x.c * linT.y + x.e;
    const float T1 = x.b * linT.x + x.d * linT.y + x.f;
    const float det = D00 * D11 - D01 * D10;
    if (abs(det) < 1e-20) {
        // Degenerate map: draw nothing, by moving every vertex off screen.
        o.pos = float4(2.0, 2.0, 0.0, 1.0);
        return o;
    }

    // The inverse of D maps device px to em, E = D^-1. Half a device px along device x moves em x
    // by at most h = 0.5 (|E00| + |E01|), so one device px spans 2h em along x, and
    // kx = |det| / (|D11| + |D01|) = 1 / (2h) is the device px per em unit along em x. The pads
    // below are h = 0.5 / kx, which grows the em box by exactly half a device px per device axis.
    const float kx = abs(det) / max(abs(D11) + abs(D01), 1e-20);
    const float ky = abs(det) / max(abs(D10) + abs(D00), 1e-20);
    // LCD text reaches 5/6 px beyond a pixel center along x (the filter), so its quad grows a full px.
    const bool lcdPrim = (p.kind & KIND_MASK) == KIND_FILL && ((p.kind >> 8) & TEXT_LCD_BIT) != 0u;
    const float2 pad = float2((lcdPrim ? 1.0 : 0.5) / kx, 0.5 / ky);
    const float2 lo = p.rect.xy - pad;
    const float2 hi = p.rect.zw + pad;

    const float2 em = lerp(lo, hi, corner);
    const float2 dev = float2(D00 * em.x + D01 * em.y + T0, D10 * em.x + D11 * em.y + T1);

    o.pos = float4(dev.x / fr.viewport.x * 2.0 - 1.0, 1.0 - dev.y / fr.viewport.y * 2.0, 0.0, 1.0);
    o.em = em;
    o.local = float2(la * em.x + lc * em.y + linT.x, lb * em.x + ld * em.y + linT.y);
    o.foot = float2(kx, ky);
    return o;
}


// Samples layer `layer` of the atlas at uv. The result is premultiplied, as the atlas stores it.
float4 sampleAtlas(float2 uv, uint layer)
{
    const float3 uvl = float3(uv, float(layer));
    __target_switch
    {
    case spirv:
        return gFsAtlasCombined.SampleLevel(uvl, 0);
    default:
        return gFsAtlasTex.SampleLevel(gFsAtlasSamp, uvl, 0);
    }
}


// Border styles. A side is one of four: 0 top, 1 right, 2 bottom, 3 left. For side k, t is the
// distance from the outer edge (0 at the outer edge, the side's width at its inner edge) and s is
// the distance along the side from its start, the centerline corner the side begins at, going
// clockwise. tg and sg are the local unit vectors of t and s, and len is the side's length between
// its two centerline corners (its patterns are fitted to len).
static const float BORDER_FAR = 1.0e9;

float2 sideTDir(uint k)
{
    return (k == 0u) ? float2(0.0, 1.0) : (k == 1u) ? float2(-1.0, 0.0)
         : (k == 2u) ? float2(0.0, -1.0) : float2(1.0, 0.0);
}

float2 sideSDir(uint k)
{
    return (k == 0u) ? float2(1.0, 0.0) : (k == 1u) ? float2(0.0, 1.0)
         : (k == 2u) ? float2(-1.0, 0.0) : float2(0.0, -1.0);
}

// Side k's t, s and length at box-local point u. size is the box, bw the widths (top, right,
// bottom, left).
void sideCoords(uint k, float2 u, float2 size, float4 bw, out float t, out float s, out float len)
{
    if (k == 0u) {
        t = u.y;
        s = u.x - bw.w * 0.5;
        len = size.x - (bw.w + bw.y) * 0.5;
    } else if (k == 1u) {
        t = size.x - u.x;
        s = u.y - bw.x * 0.5;
        len = size.y - (bw.x + bw.z) * 0.5;
    } else if (k == 2u) {
        t = size.y - u.y;
        s = size.x - bw.y * 0.5 - u.x;
        len = size.x - (bw.w + bw.y) * 0.5;
    } else {
        t = u.x;
        s = size.y - bw.z * 0.5 - u.y;
        len = size.y - (bw.x + bw.z) * 0.5;
    }
}

uint pick4u(uint4 v, uint k)
{
    return (k == 0u) ? v.x : (k == 1u) ? v.y : (k == 2u) ? v.z : v.w;
}

float pick4f(float4 v, uint k)
{
    return (k == 0u) ? v.x : (k == 1u) ? v.y : (k == 2u) ? v.z : v.w;
}

// Coverage of the interval [a, b] along a coordinate x whose local gradient is dir. Its edges are
// anti-aliased one device px wide, and a point inside the interval has coverage 1.
float intervalCov(float x, float a, float b, float2 dir, Xform px)
{
    const float ea = a - x;
    const float eb = x - b;
    return devCoverage(float3(max(ea, eb), (ea > eb) ? -dir : dir), px);
}

// Dash k of a side with n dashes, each d long, starting at k * p. The first dash extends back to
// the start corner and the last one forward to the far corner, so the corners are never gapped.
float dashCov(float k, float n, float p, float d, float s, float2 sg, Xform px)
{
    const float a = (k < 0.5) ? -BORDER_FAR : k * p;
    const float b = (k > n - 1.5) ? BORDER_FAR : k * p + d;
    return intervalCov(s, a, b, sg, px);
}

// Coverage of side k's pattern at u, one device px wide. 1 for a solid side, and for a side of
// zero width (it has no pattern).
//
// Dashed: dashes 3w long with gaps of the same length, but the period is refitted so that a whole
// number n of dashes exactly spans the side (n = round((len + d) / (2d)), at least 1).
// Dotted: round dots of diameter w on the centerline, n = round(len / 2w) + 1 of them with the
// period refitted the same way, so the first and last dots sit on the corners and each corner dot
// is split between the two sides that meet there. The nearest dot along s is the nearest dot
// overall, because the distance across the side does not depend on which dot it is.
// Double: two solid lines a third of the width each, at the outer and inner edges. The CPU sets
// the double pattern only for widths of 3 px and more; below that the side is solid.
float borderPattern(uint k, Border b, float2 u, float2 size, Xform px)
{
    const uint pat = pick4u(b.pattern, k);
    const float w = pick4f(b.widths, k);
    if (pat == 0u || w <= 0.0) {
        return 1.0;
    }
    float t;
    float s;
    float len;
    sideCoords(k, u, size, b.widths, t, s, len);
    len = max(len, 0.0);
    const float2 tg = sideTDir(k);
    const float2 sg = sideSDir(k);

    if (pat == 3u) {
        const float a = w / 3.0;
        return max(intervalCov(t, -BORDER_FAR, a, tg, px),
                   intervalCov(t, w - a, BORDER_FAR, tg, px));
    }
    if (pat == 2u) {
        const float d = 3.0 * w;
        const float n = max(1.0, floor((len + d) / (2.0 * d) + 0.5));
        if (n <= 1.0) {
            return intervalCov(s, -BORDER_FAR, BORDER_FAR, sg, px);
        }
        const float p = (len - d) / (n - 1.0);
        if (!(p > 0.0)) {
            return 1.0;
        }
        const float k1 = clamp(floor(s / p), 0.0, n - 1.0);
        const float k2 = min(k1 + 1.0, n - 1.0);
        return max(dashCov(k1, n, p, d, s, sg, px), dashCov(k2, n, p, d, s, sg, px));
    }
    // Dotted.
    const float r = 0.5 * w;
    const float n = max(1.0, floor(len / (2.0 * w) + 0.5) + 1.0);
    float sc = 0.5 * len;
    if (n > 1.0) {
        const float p = len / (n - 1.0);
        sc = clamp(floor(s / p + 0.5), 0.0, n - 1.0) * p;
    }
    const float2 v = (s - sc) * sg + (t - r) * tg;
    const float l = length(v);
    return devCoverage(float3(l - r, (l > 0.0) ? v / l : tg), px);
}

// Premultiplied color of side k at u. Groove and ridge split the side along its width: the outer
// half (t below half the width) takes the outer color, the inner half the inner one, and the
// boundary is one device px wide. For every other style both colors are equal, so this is the
// side's color.
float4 sideShade(uint k, float2 u, float2 size, Border b, Xform px)
{
    float t;
    float s;
    float len;
    sideCoords(k, u, size, b.widths, t, s, len);
    const float wk = pick4f(b.widths, k);
    const float aO = (wk > 0.0) ? devCoverage(float3(t - 0.5 * wk, sideTDir(k)), px) : 0.0;
    const float4 outer = premul(embiUnpackRGBA8(pick4u(b.colors, k)));
    const float4 inner = premul(embiUnpackRGBA8(pick4u(b.inner, k)));
    return lerp(inner, outer, aO);
}


// Slug coverage of an outline prim at em point em, with foot the device px per em unit. Its blob
// is at extraBits.z (byte offset) and its band word at extraBits.w. Returns the coverage in [0, 1].
// widthScale narrows the horizontal footprint of the pixel to 1 / widthScale px (the three LCD
// subpixels use 3).
float outlineCoverageEx(float2 em, float2 foot, Prim p, float widthScale)
{
    ByteAddressBuffer sb = fsSlugData();
    const uint nh = p.extraBits.w & 31u;
    const uint nv = (p.extraBits.w >> 5) & 31u;
    const bool evenOdd = ((p.extraBits.w >> 10) & 1u) != 0u;
    const uint blob = p.extraBits.z;
    const uint curveBase = 16u + 8u * (nh + nv);

    const float2 bmin = p.rect.xy;
    const float2 ext = max(p.rect.zw - p.rect.xy, float2(1e-9, 1e-9));
    const uint jh = (uint)clamp(floor((em.y - bmin.y) / ext.y * float(nh)), 0.0, float(nh - 1u));
    const uint jv = (uint)clamp(floor((em.x - bmin.x) / ext.x * float(nv)), 0.0, float(nv - 1u));

    const SlugRay h = slugCastRay(sb, blob, 16u + 8u * jh, curveBase, em, foot.x * widthScale, true);
    const SlugRay v = slugCastRay(sb, blob, 16u + 8u * (nh + jv), curveBase, em, foot.y, false);
    const float ch = slugCover(h.sum, evenOdd);
    const float cv = slugCover(v.sum, evenOdd);
    const float wsum = h.weight + v.weight;
    return clamp((wsum > 1e-4) ? (h.weight * ch + v.weight * cv) / wsum : 0.5 * (ch + cv),
                 0.0, 1.0);
}

float outlineCoverage(float2 em, float2 foot, Prim p)
{
    return outlineCoverageEx(em, foot, p, 1.0);
}

// LCD coverage of one color channel whose center is chanOff device px from the pixel center: the
// FreeType default LCD filter (taps 0x08 0x4D 0x56 0x4D 0x08 over 256) applied to five third-pixel
// box coverages centered at chanOff + (j - 2) / 3. The map is axis aligned (D01 = 0, D00 = foot.x),
// so a device offset of d px along x is d / foot.x in em x.
float lcdCoverage(float2 em, float2 foot, Prim p, float chanOff)
{
    float sum = 0.0;
    const float taps[5] = {0.03125, 0.30078125, 0.3359375, 0.30078125, 0.03125};
    for (uint j = 0u; j < 5u; j++) {
        const float d = chanOff + (float(j) - 2.0) * (1.0 / 3.0);
        sum += taps[j] * outlineCoverageEx(em + float2(d / foot.x, 0.0), foot, p, 3.0);
    }
    return sum;
}

// Text contrast: coverage is blended in the gamma space the target holds, so it is reshaped by
// cov^g. Dark text on light gets a strong curve (g = 0.58, like DirectWrite's enhanced contrast),
// light text on dark a gentle one (g = 0.9), by the luminance of the text color.
float textContrast(float cov, float4 col, float boost)
{
    const float lum = dot(col.rgb, float3(0.2126, 0.7152, 0.0722));
    // Stem darkening: partial coverage grows by the boost (a fully covered or an empty pixel does not
    // change, so nothing is added beyond the outline's own antialiasing fringe).
    const float c = saturate(cov * (1.0 + boost));
    // The curve fades in over the first half of the coverage range, so faint fringes (the second row
    // of a thin bar, the far column of a stem) are not lifted into grey smears.
    return lerp(c, pow(c, lerp(0.58, 0.9, saturate(lum))), saturate(2.0 * c));
}

float4 fsMain(VSOut i) : SV_Target
{
    Prim p = loadPrim(fsPrims(), i.prim);
    Xform px = loadXform(fsXforms(), p.xform);
    if (px.padX <= 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const bool outline = (p.kind & KIND_OUTLINE) != 0u;
    const uint kind = p.kind & KIND_MASK;
    const uint index = p.kind >> 8;
    float cov;
    if (outline) {
        if (kind == KIND_FILL && (index & TEXT_BIT) != 0u) {
            const float boost = float((index >> 2) & 255u) * (DARKEN_BOOST / 512.0);
            const float4 tc = embiUnpackRGBA8(p.color);
            if ((index & TEXT_LCD_BIT) != 0u) {
                const float chanOff = asfloat(fsChan().Load(0u));
                cov = textContrast(lcdCoverage(i.em, i.foot, p, chanOff), tc, boost);
            } else {
                cov = textContrast(outlineCoverageEx(i.em, i.foot, p, 1.0), tc, boost);
            }
        } else {
            cov = outlineCoverage(i.em, i.foot, p);
        }
    } else if (kind == KIND_PLANE) {
        // The plane's quad is its whole layer region, transparent at the edge: no edge coverage.
        cov = 1.0;
    } else {
        const float2 hb = p.rect.zw * 0.5;
        const float2 center = p.rect.xy + hb;
        cov = devCoverage(sdRRectG(i.local - center, hb, p.radii), px);
    }
    float4 pm = float4(0.0, 0.0, 0.0, 0.0); // premultiplied color

    if (kind == KIND_FILL) {
        pm = premul(embiUnpackRGBA8(p.color));
    } else if (!outline && kind == KIND_BORDER) {
        Border b = loadBorder(fsBorders(), index);
        const float wt = b.widths.x;
        const float wr = b.widths.y;
        const float wb = b.widths.z;
        const float wl = b.widths.w;

        // Inner rect, with each corner radius reduced by the mean of its two adjacent widths.
        // This is exact for uniform widths. Per-side widths would need elliptical corners (R3).
        const float2 innerMin = p.rect.xy + float2(wl, wt);
        const float2 innerSize = max(p.rect.zw - float2(wl + wr, wt + wb), float2(0.0, 0.0));
        const float2 innerHalf = innerSize * 0.5;
        const float4 innerRadii =
            max(p.radii - float4((wl + wt) * 0.5, (wr + wt) * 0.5, (wr + wb) * 0.5,
                                 (wl + wb) * 0.5),
                float4(0.0, 0.0, 0.0, 0.0));
        // A zero-area inner rect is an empty hole, not a half-covered line.
        float innerCov = 0.0;
        if (innerSize.x > 0.0 && innerSize.y > 0.0) {
            innerCov = devCoverage(sdRRectG(i.local - (innerMin + innerHalf), innerHalf, innerRadii),
                                   px);
        }
        cov = max(cov - innerCov, 0.0);

        // CSS splits the border into four trapezoids along the lines from each outer corner to
        // the matching inner corner. s1..s4 are the signed cross products for those four lines.
        // The point is clamped to the box first, so a fringe pixel is classified by the nearest
        // edge. A side with zero width has no area, so it is never a candidate. When a neighbor's
        // width is zero, a side's wedge grows to cover the opposite side too (top and bottom
        // with no left or right border, say), so among the candidates the winner is the side
        // whose outer edge is closest relative to its own width. With all four widths set only
        // one wedge matches (diagonals are ties of measure zero), and this changes nothing.
        const float2 u = clamp(i.local - p.rect.xy, float2(0.0, 0.0), p.rect.zw);
        const float w = p.rect.z;
        const float h = p.rect.w;
        const float s1 = wl * u.y - wt * u.x;
        const float s2 = (-wr) * u.y - wt * (u.x - w);
        const float s3 = (-wr) * (u.y - h) + wb * (u.x - w);
        const float s4 = wl * (u.y - h) + wb * u.x;
        uint side = 3u; // left, also the fallback
        float best = 1e30;
        if (wt > 0.0 && s1 <= 0.0 && s2 >= 0.0) {
            best = u.y / wt;
            side = 0u;
        }
        if (wr > 0.0 && s2 <= 0.0 && s3 >= 0.0 && (w - u.x) / wr < best) {
            best = (w - u.x) / wr;
            side = 1u;
        }
        if (wb > 0.0 && s3 <= 0.0 && s4 >= 0.0 && (h - u.y) / wb < best) {
            best = (h - u.y) / wb;
            side = 2u;
        }
        if (wl > 0.0 && s4 <= 0.0 && s1 >= 0.0 && u.x / wl < best) {
            side = 3u;
        }
        // Anti-aliased corner joins. A pixel center on a diagonal is a tie that GPUs break
        // differently, so the side picked above is only the nearest region: its color is blended
        // with each neighbor across that side's two diagonals by the one device px coverage of
        // the signed distance to the line. On the line both picks give 50/50, on every GPU.
        // A diagonal only separates colors when both of its sides have width; next to a
        // zero-width side the wedge rules above already decide and nothing is blended.
        // Line k runs from outer corner k (TL, TR, BR, BL) to its inner corner; float3 is
        // (value, d/du.x, d/du.y). Side k lies on the negative side of line k and the positive
        // side of line k+1; its neighbors across them are sides k-1 and k+1.
        const float wd[4] = { wt, wr, wb, wl };
        const float3 ln[4] = { float3(s1, -wt, wl), float3(s2, -wt, -wr), float3(s3, wb, -wr),
                               float3(s4, wb, wl) };
        const uint na = (side + 3u) & 3u;
        const uint nb = (side + 1u) & 3u;
        float aA = 1.0;
        float aB = 1.0;
        if (wd[side] > 0.0 && wd[na] > 0.0) {
            aA = devCoverage(ln[side], px);
        }
        if (wd[side] > 0.0 && wd[nb] > 0.0) {
            aB = devCoverage(-ln[nb], px);
        }
        // The weights never sum to zero: the minimum is 1, when the pixel is fully inside.
        const float tot = aA * aB + (1.0 - aA) + (1.0 - aB);
        const float2 size = float2(w, h);
        const float4 sideColor[4] = { sideShade(0u, u, size, b, px), sideShade(1u, u, size, b, px),
                                      sideShade(2u, u, size, b, px), sideShade(3u, u, size, b, px) };
        pm = (sideColor[side] * (aA * aB) + sideColor[na] * (1.0 - aA) +
              sideColor[nb] * (1.0 - aB)) / tot;
        // The style pattern (dash, dot, double) blends across the joins with the same weights, so
        // a dash that ends at a corner fades into the neighbor's pattern over the join.
        const float patCov = (borderPattern(side, b, u, size, px) * (aA * aB) +
                              borderPattern(na, b, u, size, px) * (1.0 - aA) +
                              borderPattern(nb, b, u, size, px) * (1.0 - aB)) / tot;
        cov *= patCov;
    } else if (kind == KIND_LINEAR || kind == KIND_RADIAL || kind == KIND_CONIC) {
        Gradient g = loadGradient(fsGradients(), index);
        const float t = gradientT(kind, g, i.local);
        pm = gradientColor(fsStops(), g.firstStop, g.stopCount, g.repeat != 0u, t);
    } else if (!outline && kind == KIND_SHADOW) {
        // Analytic box shadow (lib/shadow.slang): the record holds the blurred shape and the sharp clip.
        const uint ro = index * 64u;
        const float4 sr = asfloat(fsBorders().Load4(ro));
        const float4 srad = asfloat(fsBorders().Load4(ro + 16u));
        const float4 cr = asfloat(fsBorders().Load4(ro + 32u));
        const float4 crad = asfloat(fsBorders().Load4(ro + 48u));
        cov = embiShadowAlpha(i.local, sr, srad, cr, crad, p.extra.x, p.extra.y > 0.5, px);
        pm = premul(embiUnpackRGBA8(p.color));
    } else if (!outline && kind == KIND_IMAGE_REC) {
        // A record image (see embi_frame_image_ex). Nearest sampling snaps the uv to the center of its texel;
        // the bilinear sample at a texel center is that texel. The clamp keeps a sub-rect from sampling the
        // texels outside it, so a slice never picks up its neighbor.
        const ImageRec rec = loadImageRec(fsBorders(), index);
        const float2 lp = clamp(i.local, p.rect.xy, p.rect.xy + p.rect.zw);
        const float2 st = (lp - p.rect.xy) / p.rect.zw;
        float2 uv = lerp(p.extra.xy, p.extra.zw, st);
        if (rec.nearest != 0u) {
            uv = (floor(uv / rec.texel) + 0.5) * rec.texel;
        }
        uv = clamp(uv, rec.clamp.xy, rec.clamp.zw);
        pm = sampleAtlas(uv, rec.layer) * premul(embiUnpackRGBA8(p.color));
    } else if (!outline && (kind == KIND_IMAGE || kind == KIND_PLANE)) {
        // The sample point is the local point clamped to the rect, so the anti-aliasing fringe
        // takes the edge texels instead of reaching into the neighboring atlas slots.
        const float2 lp = clamp(i.local, p.rect.xy, p.rect.xy + p.rect.zw);
        const float2 st = (lp - p.rect.xy) / p.rect.zw;
        const float2 uv = lerp(p.extra.xy, p.extra.zw, st);
        // Modulate by the tint (premultiplied too). White, opaque is the identity.
        pm = sampleAtlas(uv, index) * premul(embiUnpackRGBA8(p.color));
    }

    // Clip chain. Each clip is anti-aliased in its own space, and the walk is bounded by the
    // depth the CPU guarantees (render.c folds deeper chains).
    uint ci = p.clip;
    for (uint k = 0u; k < MAX_CLIP_DEPTH && ci != 0u; k++) {
        const Clip cl = loadClip(fsClips(), ci);
        cov *= clipCoverage(cl, i.pos.xy);
        ci = cl.parent;
    }

    return pm * cov;
}
