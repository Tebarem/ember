// Shared fragment code for the renderer ubershader (box.slang): the fragment
// storage buffers, the rounded-rect SDF and device coverage, the clip chain, and the gradient
// evaluation. Each program includes this file once, through its own vertex stage and outputs.
// Included files are found through -I shaders/lib.

#include "bind.slang"
#include "types.slang"

// Fragment storage buffers. Each one has a SPIR-V/DXIL register (1..6, after the atlas texture)
// and a Metal buffer index (0..5). fsPrims() and the other accessors pick the right one per
// target. See bind.slang.
EMB_FS_SSBO(fsPrims, 1, 101, 0);
EMB_FS_SSBO(fsXforms, 2, 102, 1);
EMB_FS_SSBO(fsClips, 3, 103, 2);
EMB_FS_SSBO(fsBorders, 4, 104, 3);
EMB_FS_SSBO(fsGradients, 5, 105, 4);
EMB_FS_SSBO(fsStops, 6, 106, 5);

// Signed distance to a rounded rectangle, and its local gradient. q is relative to the center,
// hb is the half size, r is (tl, tr, br, bl). Corner order follows CSS, with y pointing down.
// Returns (sdf, gx, gy). The gradient is a unit vector wherever the SDF is differentiable.
//
// Every corner is evaluated independently: sdf = max(box, cut_tl, cut_tr, cut_br, cut_bl), where
// cut_i is the distance outside corner i's circle (length(p - c_i) - r_i) while p lies in that
// corner's outward quadrant (past the arc center on both axes), and -infinity elsewhere. The radii
// are assumed to satisfy the CSS overlap rule (adjacent radii sum to at most the side, which
// embi_scale_radii guarantees), so a single large radius such as `border-radius: 90px 10px 0 0`
// on a 100 px box is honored. The gradient comes from whichever term wins the max.
float3 sdRRectG(float2 q, float2 hb, float4 r)
{
    const float2 sg = float2((q.x < 0.0) ? -1.0 : 1.0, (q.y < 0.0) ? -1.0 : 1.0);
    const float2 d = abs(q) - hb;
    const float2 dp = max(d, float2(0.0, 0.0));
    const float len = length(dp);
    float3 best;
    if (len > 0.0) {
        best = float3(len, (dp / len) * sg);
    } else {
        const float2 gn = (d.x > d.y) ? float2(sg.x, 0.0) : float2(0.0, sg.y);
        best = float3(max(d.x, d.y), gn);
    }
    [unroll]
    for (int i = 0; i < 4; i++) {
        // tl, tr, br, bl
        const float ox = (i == 1 || i == 2) ? 1.0 : -1.0;
        const float oy = (i >= 2) ? 1.0 : -1.0;
        const float ri = (i == 0) ? r.x : (i == 1) ? r.y : (i == 2) ? r.z : r.w;
        const float2 c = float2(ox * (hb.x - ri), oy * (hb.y - ri));
        const float2 v = q - c;
        if (ri > 0.0 && v.x * ox > 0.0 && v.y * oy > 0.0) {
            const float l = length(v);
            const float cut = l - ri;
            if (cut > best.x) {
                best = float3(cut, (l > 0.0) ? v / l : float2(ox, oy) * 0.70710678);
            }
        }
    }
    return best;
}

// Anti-aliased coverage of a local-space SDF sample (see sdRRectG), one device px wide, for a
// transform with 2x2 part (a c; b d). The device gradient is J^-T g.
float devCoverage(float3 s, Xform x)
{
    const float det = x.a * x.d - x.b * x.c;
    const float2 g = s.yz;
    const float2 jg = float2(x.d * g.x - x.b * g.y, -x.c * g.x + x.a * g.y) / det;
    const float dist = s.x / max(length(jg), 1e-12);
    return clamp(0.5 - dist, 0.0, 1.0);
}

float4 premul(float4 c)
{
    return float4(c.rgb * c.a, c.a);
}

// Coverage of clip c at device pixel center dev. 0 for a degenerate transform.
float clipCoverage(Clip c, float2 dev)
{
    Xform x = loadXform(fsXforms(), c.xform);
    if (x.padX <= 0.0) {
        return 0.0;
    }
    const float det = x.a * x.d - x.b * x.c;
    const float2 t = dev - float2(x.e, x.f);
    const float2 lp = float2(x.d * t.x - x.c * t.y, -x.b * t.x + x.a * t.y) / det;
    const float2 hb = c.rect.zw * 0.5;
    return devCoverage(sdRRectG(lp - (c.rect.xy + hb), hb, c.radii), x);
}

// Gradient parameter t of local point x. Degenerate geometry (zero-length line, zero radius)
// returns a huge t, which lands on the last stop. The CPU clears `repeat` for those cases.
float gradientT(uint kind, Gradient g, float2 x)
{
    if (kind == KIND_LINEAR) {
        const float2 d = g.p.zw - g.p.xy;
        const float len2 = dot(d, d);
        return (len2 > 0.0) ? dot(x - g.p.xy, d) / len2 : 1e30;
    }
    if (kind == KIND_RADIAL) {
        const float2 r = g.p.zw;
        if (r.x <= 0.0 || r.y <= 0.0) {
            return 1e30;
        }
        return length((x - g.p.xy) / r);
    }
    // Conic: the angle from straight up, clockwise (CSS), minus the start angle, in turns.
    const float2 d = x - g.p.xy;
    const float turns = (atan2(d.x, -d.y) - g.p.z) * (1.0 / 6.283185307179586);
    return turns - floor(turns);
}

// Premultiplied color of a gradient at parameter t. Stops are [first, first + count) in the stop
// buffer, with non-decreasing offsets (the CPU enforces that).
float4 gradientColor(ByteAddressBuffer stops, uint first, uint count, bool repeat, float t)
{
    if (count == 0u) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const uint last = first + count - 1u;
    if (count == 1u) {
        return premul(embiUnpackRGBA8(loadStop(stops, first).color));
    }
    const float t0 = loadStop(stops, first).offset;
    const float tl = loadStop(stops, last).offset;
    if (repeat && tl > t0) {
        const float period = tl - t0;
        t = t0 + (t - t0) - period * floor((t - t0) / period);
    }
    if (t <= t0) {
        return premul(embiUnpackRGBA8(loadStop(stops, first).color));
    }
    if (t >= tl) {
        return premul(embiUnpackRGBA8(loadStop(stops, last).color));
    }
    // Binary search for the first stop past t, between the invariants offset(lo) <= t and
    // offset(hi) > t (the endpoints above guarantee both). A hard stop (equal offsets) therefore
    // resolves to the stop after it, which is what CSS does at that position. The search costs
    // log2(count) loads of the stop buffer, where a linear scan cost up to count.
    uint lo = first;
    uint hi = last;
    while (hi - lo > 1u) {
        const uint mid = lo + (hi - lo) / 2u;
        if (loadStop(stops, mid).offset > t) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const Stop a = loadStop(stops, lo);
    const Stop b = loadStop(stops, hi);
    const float span = b.offset - a.offset;
    const float f = (span > 0.0) ? clamp((t - a.offset) / span, 0.0, 1.0) : 1.0;
    return lerp(premul(embiUnpackRGBA8(a.color)), premul(embiUnpackRGBA8(b.color)), f);
}
