// Analytic box shadows (R3a): the Gaussian blur of a rounded rectangle in closed form, as browsers
// draw box-shadow. Included by box.slang for prims of KIND_SHADOW (embi_frame_shadow).
//
// The blur of a shape S at point p is the integral of the Gaussian over S. For a rectangle the
// integral is separable and exact: a product of two erf differences. For a rounded rectangle the
// rows are integrated numerically: each row of the shape has an x extent (narrower where a corner
// arc cuts it), the x integral of the row is an erf difference, and the rows are summed with the
// exact Gaussian mass of each row cell. Corners may have different radii.

// erf, Abramowitz and Stegun 7.1.26 (absolute error below 1.5e-7).
float embiErf(float x)
{
    const float a = abs(x);
    const float t = 1.0 / (1.0 + 0.3275911 * a);
    const float poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t +
                        0.254829592) * t;
    const float y = 1.0 - poly * exp(-a * a);
    return (x < 0.0) ? -y : y;
}

// Mass of the normalized Gaussian centered at p (inv = 1 / (sigma sqrt 2)) over [a, b].
float embiGaussMass(float a, float b, float p, float inv)
{
    return 0.5 * (embiErf((b - p) * inv) - embiErf((a - p) * inv));
}

// Blur of the rounded rect r (x, y, w, h) with radii (tl, tr, br, bl) at p. sigma > 0.
static const int SHADOW_ROWS = 28;
static const float SHADOW_REACH = 3.5;

float embiBlurredRRect(float2 p, float4 r, float4 rad, float sigma)
{
    const float x0 = r.x;
    const float x1 = r.x + r.z;
    const float y0 = r.y;
    const float y1 = r.y + r.w;
    const float inv = 0.70710678118 / sigma;
    const float reach = SHADOW_REACH * sigma;
    if (p.x < x0 - reach || p.x > x1 + reach || p.y < y0 - reach || p.y > y1 + reach) {
        return 0.0;
    }
    if (max(max(rad.x, rad.y), max(rad.z, rad.w)) <= 0.0) {
        return embiGaussMass(x0, x1, p.x, inv) * embiGaussMass(y0, y1, p.y, inv);
    }
    const float ya = max(y0, p.y - reach);
    const float yb = min(y1, p.y + reach);
    if (yb <= ya) {
        return 0.0;
    }
    const float dy = (yb - ya) / float(SHADOW_ROWS);
    float sum = 0.0;
    for (int j = 0; j < SHADOW_ROWS; j++) {
        const float a = ya + float(j) * dy;
        const float b = a + dy;
        const float yy = a + 0.5 * dy;
        float lx = x0;
        float rx = x1;
        if (yy < y0 + rad.x) {
            const float d = y0 + rad.x - yy;
            lx = max(lx, x0 + rad.x - sqrt(max(rad.x * rad.x - d * d, 0.0)));
        }
        if (yy < y0 + rad.y) {
            const float d = y0 + rad.y - yy;
            rx = min(rx, x1 - rad.y + sqrt(max(rad.y * rad.y - d * d, 0.0)));
        }
        if (yy > y1 - rad.z) {
            const float d = yy - (y1 - rad.z);
            rx = min(rx, x1 - rad.z + sqrt(max(rad.z * rad.z - d * d, 0.0)));
        }
        if (yy > y1 - rad.w) {
            const float d = yy - (y1 - rad.w);
            lx = max(lx, x0 + rad.w - sqrt(max(rad.w * rad.w - d * d, 0.0)));
        }
        if (rx > lx) {
            sum += embiGaussMass(a, b, p.y, inv) * embiGaussMass(lx, rx, p.x, inv);
        }
    }
    return sum;
}

// The alpha of a shadow prim at local point q: shape and clip are the two rounded rects of the
// prim's record (x, y, w, h and radii each), sigma the blur, inset the polarity.
float embiShadowAlpha(float2 q, float4 sr, float4 srad, float4 cr, float4 crad, float sigma,
                      bool inset, Xform px)
{
    float shape = 0.0;
    if (sr.z > 0.0 && sr.w > 0.0) {
        if (sigma > 0.0) {
            shape = embiBlurredRRect(q, sr, srad, sigma);
        } else {
            const float2 hb = sr.zw * 0.5;
            shape = devCoverage(sdRRectG(q - (sr.xy + hb), hb, srad), px);
        }
    }
    float clipc = 0.0;
    if (cr.z > 0.0 && cr.w > 0.0) {
        const float2 hc = cr.zw * 0.5;
        clipc = devCoverage(sdRRectG(q - (cr.xy + hc), hc, crad), px);
    }
    // The outer cut-out starts only where the box covers more than half a pixel: the box is drawn over the
    // shadow with its own coverage, so a full cut-out at its anti-aliased edge would leave a light fringe.
    return inset ? (1.0 - shape) * clipc : shape * (1.0 - saturate(2.0 * clipc - 1.0));
}
