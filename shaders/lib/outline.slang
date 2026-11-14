// Slug coverage for outline prims (glyphs and paths), shared by the box ubershader (box.slang).
// The CPU side (src/gpu/slug.c) builds the blob for each outline: its quadratic curves in em space,
// split into horizontal and vertical bands, with each band listing the curves that reach it.
//
// Coverage: for each pixel center, one horizontal and one vertical ray is cast in em space. Each
// curve in the band that the ray passes through contributes a signed crossing, clamped to the
// pixel footprint (so a crossing half a pixel away counts half). The class code 0x2E74 says which
// of the two roots of the curve's quadratic can lie in [0, 1] for each sign pattern of its three
// control values. The two rays give two coverages. They are blended with weights that favor the
// ray whose crossings are transverse to the outline and lie within the pixel, so a vertical edge
// is measured by the horizontal ray and a horizontal edge by the vertical one. Nonzero takes the
// clamped sum, even-odd the triangle wave of the sum.
//
// Blob layout (src/gpu/slug.c): header (16 bytes), band table (8 bytes per band: start, count;
// horizontal bands first), then the curves (24 bytes each). The band word of the prim is in
// extraBits.w: bits 0..4 horizontal band count, bits 5..9 vertical, bit 10 even-odd.

// Root classes. Bit 0 of the code is the root (b - s) / a, bit 1 the root (b + s) / a, with
// b = v0 - v1, s = sqrt(b^2 - a c), and the sign pattern taken from v > 0 at the three control
// points. The lookup is indexed by shift = 2 (v0 > 0) + 4 (v1 > 0) + 8 (v2 > 0).
static const uint SLUG_ROOT_CLASS = 0x2E74u;

// The class code alone decides whether a curve crosses the ray: a root that rounds to just outside [0, 1] is clamped, not dropped.
// A vertex of the outline can lie exactly on the ray (a stroke's turns along a pixel row), where a curve that ends there has its root at
// t = 1 and the one that starts there at t = 0, and the winding stays right only if each counts the crossing its signs say it has.

// Signed crossing and reliability weight of one curve at parameter t. q holds the along-ray
// coordinates of its three control points (relative to the pixel center), dv and du give the
// derivatives across and along the ray.
float2 slugCrossing(float t, float a, float b, float au, float bu, float3 qa, float k)
{
    const float mt = 1.0 - t;
    const float along = mt * mt * qa.x + 2.0 * t * mt * qa.y + t * t * qa.z;
    const float dv = 2.0 * (a * t - b);
    const float du = 2.0 * (au * t - bu);
    const float len = sqrt(dv * dv + du * du);
    const float transverse = (len > 0.0) ? abs(dv) / len : 0.0;
    const float sgn = (dv > 0.0) ? 1.0 : ((dv < 0.0) ? -1.0 : 0.0);
    const float x = along * k;
    return float2(sgn * clamp(0.5 + x, 0.0, 1.0), transverse * saturate(1.0 - abs(x)));
}

struct SlugRay {
    float sum;      // signed clamped crossings
    float weight;   // reliability of this ray at the pixel
};

// Casts one ray through e. horiz: along +x, with the band list of the horizontal band that holds
// e.y. Otherwise along +y, with the vertical band list. k is the device-pixel footprint along the
// ray in em units. Band lists are sorted by descending maximum along the ray, so the loop stops at
// the first curve that lies entirely behind the pixel.
SlugRay slugCastRay(ByteAddressBuffer sb, uint blob, uint entry, uint curveBase, float2 e, float k,
                    bool horiz)
{
    SlugRay r;
    r.sum = 0.0;
    r.weight = 0.0;
    const uint start = sb.Load(blob + entry);
    const uint count = sb.Load(blob + entry + 4u);
    for (uint n = 0u; n < count; n++) {
        const uint ci = sb.Load(blob + start + 4u * n);
        const uint co = blob + curveBase + 24u * ci;
        float2 q0 = float2(asfloat(sb.Load(co)), asfloat(sb.Load(co + 4u))) - e;
        float2 q1 = float2(asfloat(sb.Load(co + 8u)), asfloat(sb.Load(co + 12u))) - e;
        float2 q2 = float2(asfloat(sb.Load(co + 16u)), asfloat(sb.Load(co + 20u))) - e;
        if (!horiz) {
            q0 = q0.yx;
            q1 = q1.yx;
            q2 = q2.yx;
        }
        // From here .x runs along the ray and .y across it.
        const float maxAlong = max(q0.x, max(q1.x, q2.x));
        if (maxAlong * k < -1.0) {
            break;
        }
        const float v0 = q0.y;
        const float v1 = q1.y;
        const float v2 = q2.y;
        const uint shift = (v0 > 0.0 ? 2u : 0u) | (v1 > 0.0 ? 4u : 0u) | (v2 > 0.0 ? 8u : 0u);
        const uint code = (SLUG_ROOT_CLASS >> shift) & 3u;
        if (code == 0u) {
            continue;
        }
        const float a = v0 - 2.0 * v1 + v2;
        const float b = v0 - v1;
        const float c = v0;
        const float au = q0.x - 2.0 * q1.x + q2.x;
        const float bu = q0.x - q1.x;
        const float3 qa = float3(q0.x, q1.x, q2.x);

        if (a == 0.0) {
            // Linear in t: one root, which exists whenever the class says there is a crossing.
            if (b != 0.0) {
                const float2 cr = slugCrossing(saturate(c / (2.0 * b)), a, b, au, bu, qa, k);
                r.sum += cr.x;
                r.weight += cr.y;
            }
            continue;
        }
        const float disc = b * b - a * c;
        if (disc < 0.0) {
            continue;
        }
        // The stable form: qq = b + sign(b) s, so the two roots are qq / a and c / qq.
        const float s = sqrt(disc);
        const float qq = (b >= 0.0) ? (b + s) : (b - s);
        if (qq == 0.0) {
            continue;
        }
        const float ra = qq / a;
        const float rb = c / qq;
        const float r1 = (b >= 0.0) ? rb : ra; // (b - s) / a, class bit 0
        const float r2 = (b >= 0.0) ? ra : rb; // (b + s) / a, class bit 1
        if ((code & 1u) != 0u) {
            const float2 cr = slugCrossing(saturate(r1), a, b, au, bu, qa, k);
            r.sum += cr.x;
            r.weight += cr.y;
        }
        if ((code & 2u) != 0u) {
            const float2 cr = slugCrossing(saturate(r2), a, b, au, bu, qa, k);
            r.sum += cr.x;
            r.weight += cr.y;
        }
    }
    return r;
}

// Coverage from a signed crossing sum: clamped for nonzero, the triangle wave for even-odd.
float slugCover(float s, bool evenOdd)
{
    float m = abs(s);
    if (evenOdd) {
        m = m - 2.0 * floor(m * 0.5);
        return 1.0 - abs(m - 1.0);
    }
    return min(m, 1.0);
}
