#include "layer_rig.h"

#include <math.h>
#include <stdlib.h>

#include "base/base.h"
#include "gpu/fx.h"

/* The blend and mask passes of gpu/fx.h (R3b) against a CPU reference of the compositing spec's formulas. The source
 * and the backdrop are 8x8 grids of cells with different colors and alphas; every cell pair is one case. */

#define ZERO_RADII ((float[4]){0.0f, 0.0f, 0.0f, 0.0f})
#define GRID 8u
#define CELL 4u

typedef struct fx_rig {
    layer_rig k;
    embi_fx *fx;
    SDL_GPUCommandBuffer *cmd;
} fx_rig;

static bool fx_rig_open(fx_rig *r)
{
    *r = (fx_rig){0};
    if (!layer_rig_open(&r->k)) {
        return false;
    }
    r->fx = embi_fx_create(r->k.g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, r->k.pool);
    r->cmd = SDL_AcquireGPUCommandBuffer(r->k.dev);
    if (r->fx == nullptr || r->cmd == nullptr) {
        embi_fx_destroy(r->fx);
        layer_rig_close(&r->k);
        return false;
    }
    return true;
}

static void fx_rig_close(fx_rig *r)
{
    embi_fx_destroy(r->fx);
    layer_rig_close(&r->k);
}

/* A grid layer: cell (i, j) holds the color pick(seed, i, j). */
static uint32_t pick(uint32_t seed, uint32_t i, uint32_t j)
{
    uint32_t h = (i * 7919u + j * 104729u + seed * 15485863u) * 2654435761u;
    uint32_t r = (h >> 3) & 255u, g = (h >> 11) & 255u, b = (h >> 19) & 255u;
    uint32_t a = ((h >> 27) & 3u) == 0 ? 128u : 255u;
    if (i == 0) {
        a = 255; /* a column of opaque cells */
    }
    if (j == 0 && seed == 1u) {
        a = 0; /* a row of transparent source cells */
    }
    return RGBA(r, g, b, a);
}

static bool make_grid(fx_rig *r, uint32_t seed, embi_layer *L)
{
    const uint32_t n = GRID * CELL;
    if (!embi_layer_acquire(r->k.pool, n, n, L)) {
        return false;
    }
    embi_frame *f = embi_frame_begin(r->k.r, L->tex_w, L->tex_h, 1.0f);
    if (f == nullptr) {
        return false;
    }
    for (uint32_t j = 0; j < GRID; j++) {
        for (uint32_t i = 0; i < GRID; i++) {
            const float rect[4] = {(float)(i * CELL), (float)(j * CELL), (float)CELL, (float)CELL};
            embi_frame_fill(f, rect, ZERO_RADII, pick(seed, i, j), 0, 0);
        }
    }
    embi_frame_end(f, r->cmd, L->tex, true, 0);
    return true;
}

static layer_dl grab(fx_rig *r, const embi_layer *L, SDL_GPUTexture **target_out)
{
    SDL_GPUTexture *target = layer_rig_target(&r->k, L->w, L->h);
    *target_out = target;
    embi_frame *f = embi_frame_begin(r->k.r, L->w, L->h, 1.0f);
    const float src[4] = {0, 0, (float)L->w, (float)L->h};
    embi_frame_layer(f, L, src, src, ZERO_RADII, 1.0f, 0, 0);
    embi_frame_end(f, r->cmd, target, true, 0);
    return layer_rig_download(&r->k, r->cmd, target, L->w, L->h);
}

/* ---- the reference (unpremultiplied colors in 0..1) ---- */

typedef struct v3 {
    double x, y, z;
} v3;

static double lum(v3 c)
{
    return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z;
}

static v3 clip_color(v3 c)
{
    double l = lum(c), n = fmin(c.x, fmin(c.y, c.z)), x = fmax(c.x, fmax(c.y, c.z));
    if (n < 0.0) {
        c = (v3){l + (c.x - l) * l / (l - n), l + (c.y - l) * l / (l - n), l + (c.z - l) * l / (l - n)};
    }
    if (x > 1.0) {
        c = (v3){l + (c.x - l) * (1.0 - l) / (x - l), l + (c.y - l) * (1.0 - l) / (x - l),
                 l + (c.z - l) * (1.0 - l) / (x - l)};
    }
    return c;
}

static v3 set_lum(v3 c, double l)
{
    double d = l - lum(c);
    return clip_color((v3){c.x + d, c.y + d, c.z + d});
}

static double sat(v3 c)
{
    return fmax(c.x, fmax(c.y, c.z)) - fmin(c.x, fmin(c.y, c.z));
}

static v3 set_sat(v3 c, double s)
{
    double mx = fmax(c.x, fmax(c.y, c.z)), mn = fmin(c.x, fmin(c.y, c.z));
    if (mx <= mn) {
        return (v3){0, 0, 0};
    }
    double k = s / (mx - mn);
    return (v3){(c.x - mn) * k, (c.y - mn) * k, (c.z - mn) * k};
}

static double sep(uint32_t m, double cb, double cs)
{
    switch (m) {
    case 1:
        return cb * cs;
    case 2:
        return cb + cs - cb * cs;
    case 3: /* overlay = hard-light with the roles swapped */
        return cb <= 0.5 ? cs * (2.0 * cb) : cs + (2.0 * cb - 1.0) - cs * (2.0 * cb - 1.0);
    case 4:
        return fmin(cb, cs);
    case 5:
        return fmax(cb, cs);
    case 6:
        return cb <= 0.0 ? 0.0 : (cs >= 1.0 ? 1.0 : fmin(1.0, cb / (1.0 - cs)));
    case 7:
        return cb >= 1.0 ? 1.0 : (cs <= 0.0 ? 0.0 : 1.0 - fmin(1.0, (1.0 - cb) / cs));
    case 8:
        return cs <= 0.5 ? cb * (2.0 * cs) : cb + (2.0 * cs - 1.0) - cb * (2.0 * cs - 1.0);
    case 9: {
        if (cs <= 0.5) {
            return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        }
        double d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (d - cb);
    }
    case 10:
        return fabs(cb - cs);
    case 11:
        return cb + cs - 2.0 * cb * cs;
    case 16:
        return fmin(1.0, cb + cs);
    case 17:
        return fmax(0.0, cb + cs - 1.0);
    default:
        return cs;
    }
}

static v3 blend_ref(uint32_t m, v3 cb, v3 cs)
{
    switch (m) {
    case 12:
        return set_lum(set_sat(cs, sat(cb)), lum(cb));
    case 13:
        return set_lum(set_sat(cb, sat(cs)), lum(cb));
    case 14:
        return set_lum(cs, lum(cb));
    case 15:
        return set_lum(cb, lum(cs));
    default:
        return (v3){sep(m, cb.x, cs.x), sep(m, cb.y, cs.y), sep(m, cb.z, cs.z)};
    }
}

/* The expected premultiplied bytes of the blend of two premultiplied pixels. */
static void blend_px(uint32_t m, const uint8_t *s, const uint8_t *b, double out[4])
{
    double as = s[3] / 255.0, ab = b[3] / 255.0;
    if (as <= 0.0) {
        out[0] = out[1] = out[2] = out[3] = 0.0;
        return;
    }
    v3 cs = {fmin(1.0, s[0] / 255.0 / as), fmin(1.0, s[1] / 255.0 / as), fmin(1.0, s[2] / 255.0 / as)};
    v3 cb = {0, 0, 0};
    if (ab > 0.0) {
        cb = (v3){fmin(1.0, b[0] / 255.0 / ab), fmin(1.0, b[1] / 255.0 / ab), fmin(1.0, b[2] / 255.0 / ab)};
    }
    v3 bl = blend_ref(m, cb, cs);
    double mx = (1.0 - ab) * cs.x + ab * bl.x, my = (1.0 - ab) * cs.y + ab * bl.y, mz = (1.0 - ab) * cs.z + ab * bl.z;
    out[0] = mx * as * 255.0;
    out[1] = my * as * 255.0;
    out[2] = mz * as * 255.0;
    out[3] = as * 255.0;
}

static void blend_case(uint32_t mode)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const uint32_t n = GRID * CELL;
    embi_layer src, bk, dst;
    EMB_REQUIRE(make_grid(&r, 1, &src));
    EMB_REQUIRE(make_grid(&r, 2, &bk));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, n, n, &dst));
    SDL_GPUTexture *t0, *t1, *t2;
    layer_dl d0 = grab(&r, &src, &t0);
    layer_dl d1 = grab(&r, &bk, &t1);
    EMB_REQUIRE(embi_fx_blend(r.fx, r.cmd, &src, &bk, &dst, mode));
    layer_dl d2 = grab(&r, &dst, &t2);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *ps = layer_rig_map(&r.k, d0);
    uint8_t *pb = layer_rig_map(&r.k, d1);
    uint8_t *po = layer_rig_map(&r.k, d2);
    EMB_REQUIRE(ps != nullptr && pb != nullptr && po != nullptr);
    int worst = 0;
    for (size_t i = 0; i < (size_t)n * n; i++) {
        double want[4];
        blend_px(mode, ps + i * 4, pb + i * 4, want);
        for (int c = 0; c < 4; c++) {
            int d = (int)lround(fabs((double)po[i * 4 + (size_t)c] - want[c]));
            worst = d > worst ? d : worst;
        }
    }
    if (worst > 4) {
        char msg[120];
        SDL_snprintf(msg, sizeof msg, "blend mode %u: max difference %d > 4", (unsigned)mode, worst);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(ps);
    SDL_free(pb);
    SDL_free(po);
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &bk);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t0);
    SDL_ReleaseGPUTexture(r.k.dev, t1);
    SDL_ReleaseGPUTexture(r.k.dev, t2);
    fx_rig_close(&r);
}

EMB_TEST(blend_normal_keeps_the_source)
{
    blend_case(0);
}

EMB_TEST(blend_separable_modes_match_the_reference)
{
    for (uint32_t m = 1; m <= 11; m++) {
        blend_case(m);
    }
}

EMB_TEST(blend_non_separable_modes_match_the_reference)
{
    for (uint32_t m = 12; m <= 15; m++) {
        blend_case(m);
    }
}

EMB_TEST(blend_plus_modes_match_the_reference)
{
    blend_case(16);
    blend_case(17);
}

EMB_TEST(blend_rejects_a_bad_mode_and_mismatched_sizes)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer a, b, c, d;
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 16, 16, &a));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 16, 16, &b));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 16, 16, &c));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 20, 16, &d));
    EMB_CHECK(!embi_fx_blend(r.fx, r.cmd, &a, &b, &c, 99));
    EMB_CHECK(!embi_fx_blend(r.fx, r.cmd, &a, &b, &d, 1));
    embi_layer_release(r.k.pool, &a);
    embi_layer_release(r.k.pool, &b);
    embi_layer_release(r.k.pool, &c);
    embi_layer_release(r.k.pool, &d);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    fx_rig_close(&r);
}

/* ------------------------------------------------------------------ masks */

static void mask_case(int which, uint32_t op)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const uint32_t n = GRID * CELL;
    embi_layer a, b, dst;
    EMB_REQUIRE(make_grid(&r, 1, &a));
    EMB_REQUIRE(make_grid(&r, 2, &b));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, n, n, &dst));
    SDL_GPUTexture *t0, *t1, *t2;
    layer_dl d0 = grab(&r, &a, &t0);
    layer_dl d1 = grab(&r, &b, &t1);
    if (which == 0) {
        EMB_REQUIRE(embi_fx_mask_apply(r.fx, r.cmd, &a, &b, &dst));
    } else if (which == 1) {
        EMB_REQUIRE(embi_fx_mask_luminance(r.fx, r.cmd, &a, &dst));
    } else {
        EMB_REQUIRE(embi_fx_mask_combine(r.fx, r.cmd, &a, &b, &dst, op));
    }
    layer_dl d2 = grab(&r, &dst, &t2);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *pa = layer_rig_map(&r.k, d0);
    uint8_t *pb = layer_rig_map(&r.k, d1);
    uint8_t *po = layer_rig_map(&r.k, d2);
    EMB_REQUIRE(pa != nullptr && pb != nullptr && po != nullptr);
    int worst = 0;
    for (size_t i = 0; i < (size_t)n * n; i++) {
        double want[4] = {0, 0, 0, 0};
        double ma = pb[i * 4 + 3] / 255.0, aa = pa[i * 4 + 3] / 255.0, ba = pb[i * 4 + 3] / 255.0;
        if (which == 0) {
            for (int c = 0; c < 4; c++) {
                want[c] = pa[i * 4 + (size_t)c] * ma;
            }
        } else if (which == 1) {
            want[3] = 0.2125 * pa[i * 4] + 0.7154 * pa[i * 4 + 1] + 0.0721 * pa[i * 4 + 2];
        } else {
            double v = aa + ba * (1.0 - aa);
            if (op == 1) {
                v = aa * (1.0 - ba);
            } else if (op == 2) {
                v = aa * ba;
            } else if (op == 3) {
                v = aa * (1.0 - ba) + ba * (1.0 - aa);
            }
            want[3] = v * 255.0;
        }
        for (int c = 0; c < 4; c++) {
            int d = (int)lround(fabs((double)po[i * 4 + (size_t)c] - want[c]));
            worst = d > worst ? d : worst;
        }
    }
    if (worst > 3) {
        char msg[120];
        SDL_snprintf(msg, sizeof msg, "mask pass %d op %u: max difference %d > 3", which, (unsigned)op, worst);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(pa);
    SDL_free(pb);
    SDL_free(po);
    embi_layer_release(r.k.pool, &a);
    embi_layer_release(r.k.pool, &b);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t0);
    SDL_ReleaseGPUTexture(r.k.dev, t1);
    SDL_ReleaseGPUTexture(r.k.dev, t2);
    fx_rig_close(&r);
}

EMB_TEST(mask_apply_scales_by_the_mask_alpha)
{
    mask_case(0, 0);
}

EMB_TEST(mask_luminance_uses_the_premultiplied_luminance)
{
    mask_case(1, 0);
}

EMB_TEST(mask_combine_operators_match_the_reference)
{
    for (uint32_t op = 0; op < 4; op++) {
        mask_case(2, op);
    }
}

EMB_RENDER_TEST_MAIN()
