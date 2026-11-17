/*
 * Vertical glyph hinting (text/hint.c): a synthetic H, real glyphs of two faces at 9 to 36 px, and the rows of the crossbar
 * of e and of the bars of z drawn through the renderer.
 */
#include "text_fixture.h"

#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include "text/hint.h"

#include "coverage_ref.h"

#include <SDL3/SDL_init.h>

static float frac_off(float v)
{
    return SDL_fabsf(v - SDL_roundf(v));
}

/* The curves of a closed polygon of straight segments (a line is a quad with its control at the midpoint). */
static uint32_t poly(embi_qcurve *out, const float (*p)[2], uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const float *a = p[i], *b = p[(i + 1) % n];
        out[i] = (embi_qcurve){a[0], a[1], 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), b[0], b[1]};
    }
    return n;
}

/* ------------------------------------------------------------------ synthetic H */

EMB_TEST(hint_synthetic_h)
{
    const float upem = 1000.0f;
    /* H clockwise (y up): stems 0..90 and 530..620 wide, cap 714, crossbar 330..400 (0.07 em). */
    const float h[12][2] = {{80, 0},   {80, 714},  {170, 714}, {170, 400}, {530, 400}, {530, 714},
                            {620, 714}, {620, 0},   {530, 0},   {530, 330}, {170, 330}, {170, 0}};
    embi_qcurve in[12];
    poly(in, h, 12);
    embi_hint_zones z;
    SDL_memset(&z, 0, sizeof z);
    z.z[EMBI_HINT_BASELINE] = (embi_hint_zone){0, 0};
    z.z[EMBI_HINT_XHEIGHT] = (embi_hint_zone){536, 549};
    z.z[EMBI_HINT_CAP] = (embi_hint_zone){714, 726};
    z.n_valid_mask = (1u << EMBI_HINT_BASELINE) | (1u << EMBI_HINT_XHEIGHT) | (1u << EMBI_HINT_CAP);
    for (int size = 9; size <= 36; size++) {
        const float s = (float)size / upem;
        embi_qcurve out[24];
        const uint32_t m = embi_hint_glyph(&z, upem, s, in, 12, out);
        EMB_REQUIRE(m >= 12u);
        float ymin = 1e9f, ymax = -1e9f;
        float bar_lo = -1.0f, bar_hi = -1.0f;
        for (uint32_t i = 0; i < m; i++) {
            const float y0 = out[i].y0 * s;
            ymin = y0 < ymin ? y0 : ymin;
            ymax = y0 > ymax ? y0 : ymax;
            /* The crossbar edges are the horizontal segments between the stems. */
            if (out[i].x0 == 170.0f && out[i].x1 == 530.0f) {
                bar_hi = y0;
            }
            if (out[i].x0 == 530.0f && out[i].x1 == 170.0f) {
                bar_lo = y0;
            }
        }
        EMB_CHECK(frac_off(ymin) < 1.0f / 64.0f);
        EMB_CHECK(frac_off(ymax) < 1.0f / 64.0f);
        /* The stems are fitted from an x-height of 5 px (below that only the zones are). */
        if (536.0f * s >= 5.0f) {
            EMB_CHECK(frac_off(bar_lo) < 1.0f / 64.0f);
            EMB_CHECK(frac_off(bar_hi) < 1.0f / 64.0f);
            EMB_CHECK(bar_hi - bar_lo >= 1.0f - 1.0f / 64.0f);
        }
        /* The x of every point is untouched. */
        for (uint32_t i = 0; i < m; i++) {
            bool found = false;
            for (uint32_t k = 0; k < 12; k++) {
                found = found || out[i].x0 == in[k].x0;
            }
            EMB_CHECK(found);
        }
    }
}

/* ------------------------------------------------------------------ real glyphs */

typedef struct face {
    uint8_t *data;
    embi_font font;
} face;

static bool face_open(face *f, const char *rel)
{
    char path[512];
    size_t n = 0;
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    f->data = (uint8_t *)SDL_LoadFile(path, &n);
    return f->data != nullptr && embi_font_open(f->data, n, 0, &f->font);
}

static void face_close(face *f)
{
    embi_font_close(&f->font);
    SDL_free(f->data);
}

/* The curves of the glyph of cp in out (embi_alloc'd, *n curves). */
static embi_qcurve *face_curves(const face *f, uint32_t cp, uint32_t *n)
{
    embi_outline ol;
    SDL_memset(&ol, 0, sizeof ol);
    *n = 0;
    uint32_t gid = embi_font_glyph_index(&f->font, cp);
    if (gid == 0 || !embi_font_glyph_outline(&f->font, gid, nullptr, &ol) || ol.quads.len == 0) {
        embi_outline_free(&ol);
        return nullptr;
    }
    embi_qcurve *c = SDL_malloc(sizeof *c * ol.quads.len);
    for (uint32_t i = 0; i < ol.quads.len; i++) {
        const embi_quad *q = &ol.quads.data[i];
        c[i] = (embi_qcurve){q->p0.x, q->p0.y, q->p1.x, q->p1.y, q->p2.x, q->p2.y};
    }
    *n = ol.quads.len;
    embi_outline_free(&ol);
    return c;
}

static bool extent_cb(void *user, uint32_t cp, float *ymin, float *ymax)
{
    uint32_t n;
    embi_qcurve *c = face_curves(user, cp, &n);
    if (c == nullptr) {
        return false;
    }
    embi_hint_extent(c, n, ymin, ymax);
    SDL_free(c);
    return true;
}

/* Signed areas of the contours of curves (a contour ends where the next curve does not start at its end). */
static uint32_t contour_signs(const embi_qcurve *c, uint32_t n, int *sign, uint32_t max)
{
    uint32_t k = 0;
    double a = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        a += (double)c[i].x0 * c[i].cy - (double)c[i].cx * c[i].y0 + (double)c[i].cx * c[i].y1 - (double)c[i].x1 * c[i].cy;
        if (i + 1 == n || c[i + 1].x0 != c[i].x1 || c[i + 1].y0 != c[i].y1) {
            if (k < max) {
                sign[k++] = a < 0.0 ? -1 : 1;
            }
            a = 0.0;
        }
    }
    return k;
}

static void check_font(const char *rel)
{
    face f;
    EMB_REQUIRE(face_open(&f, rel));
    const float upem = (float)f.font.metrics.units_per_em;
    embi_hint_zones z;
    embi_hint_zones_measure(&z, upem, (float)f.font.metrics.x_height, extent_cb, &f);
    EMB_REQUIRE(embi_hint_zones_usable(&z));
    static const char glyphs[] = "Hezxopdt";
    for (int size = 9; size <= 36; size++) {
        const float s = (float)size / upem;
        for (const char *g = glyphs; *g != 0; g++) {
            uint32_t n;
            embi_qcurve *in = face_curves(&f, (uint32_t)*g, &n);
            EMB_REQUIRE(in != nullptr);
            embi_qcurve *out = SDL_malloc(sizeof *out * 2u * n), *out2 = SDL_malloc(sizeof *out * 2u * n);
            const uint32_t m = embi_hint_glyph(&z, upem, s, in, n, out);
            const uint32_t m2 = embi_hint_glyph(&z, upem, s, in, n, out2);
            EMB_CHECK(m > 0u);
            EMB_CHECK(m == m2 && SDL_memcmp(out, out2, sizeof *out * m) == 0);
            if (m == 0u) {
                SDL_free(in);
                SDL_free(out);
                SDL_free(out2);
                continue;
            }
            /* No point moves by more than a pixel (the points are matched by their unchanged x). */
            uint32_t j = 0;
            bool moved_ok = true;
            for (uint32_t i = 0; i < n && moved_ok; i++) {
                while (j < m && !(out[j].x0 == in[i].x0 && SDL_fabsf((out[j].y0 - in[i].y0) * s) <= 1.0f)) {
                    j++;
                }
                moved_ok = j < m;
                j = moved_ok ? j + 1 : j;
            }
            EMB_CHECK(moved_ok);
            /* The contours keep their orientation. */
            int sa[64], sb[64];
            const uint32_t ka = contour_signs(in, n, sa, 64), kb = contour_signs(out, m, sb, 64);
            EMB_CHECK(ka == kb);
            for (uint32_t i = 0; i < ka && i < kb; i++) {
                EMB_CHECK(sa[i] == sb[i]);
            }
            /* Zone edges land on rows. */
            float lo, hi;
            embi_hint_extent(out, m, &lo, &hi);
            if (*g == 'H' || *g == 'x' || *g == 'z' || *g == 'd') {
                EMB_CHECK(frac_off(hi * s) < 1.0f / 64.0f);
                EMB_CHECK(frac_off(lo * s) < 1.0f / 64.0f);
            }
            if (*g == 'o' || *g == 'e') {
                EMB_CHECK(frac_off(hi * s) < 1.0f / 64.0f);
                EMB_CHECK(frac_off(lo * s) < 1.0f / 64.0f);
                /* An overshoot below a pixel is suppressed: the round glyph is as tall as the flat one. */
                const float over_px = (z.z[EMBI_HINT_XHEIGHT].over - z.z[EMBI_HINT_XHEIGHT].flat) * s;
                /* (An anchor that would move its edge by more than 3/4 px is dropped, and the stem rules place it.) */
                const float flat_row = SDL_roundf(z.z[EMBI_HINT_XHEIGHT].flat * s);
                if (over_px < 0.5f && SDL_fabsf(z.z[EMBI_HINT_XHEIGHT].over * s - flat_row) <= 0.75f) {
                    EMB_CHECK(SDL_fabsf(hi * s - SDL_roundf(z.z[EMBI_HINT_XHEIGHT].flat * s)) < 1.0f / 64.0f);
                    EMB_CHECK(SDL_fabsf(lo * s) < 1.0f / 64.0f);
                }
            }
            if (*g == 'p') {
                EMB_CHECK(frac_off(lo * s) < 1.0f / 64.0f);
            }
            SDL_free(in);
            SDL_free(out);
            SDL_free(out2);
        }
    }
    face_close(&f);
}

EMB_TEST(hint_real_notosans)
{
    check_font("tests/fonts/NotoSans-Regular.ttf");
}

EMB_TEST(hint_real_sourcesans)
{
    check_font("tests/fonts/SourceSans3-Regular.otf");
}

/* ------------------------------------------------------------------ shape fidelity */

#define FID_W 28
#define FID_H 36
#define FID_BASE 24

/* Rasterizes curves (font units, y up) at s px per unit, the baseline on row FID_BASE and x at 4 px. */
static void fid_raster(const embi_qcurve *c, uint32_t n, float s, float *cov)
{
    float (*q)[6] = SDL_malloc(sizeof *q * n);
    for (uint32_t i = 0; i < n; i++) {
        q[i][0] = c[i].x0 * s + 4.0f;
        q[i][1] = (float)FID_BASE - c[i].y0 * s;
        q[i][2] = c[i].cx * s + 4.0f;
        q[i][3] = (float)FID_BASE - c[i].cy * s;
        q[i][4] = c[i].x1 * s + 4.0f;
        q[i][5] = (float)FID_BASE - c[i].y1 * s;
    }
    covref_raster((const float (*)[6])q, n, FID_W, FID_H, cov);
    SDL_free(q);
}

/* Normalized cross-correlation of two coverage images (means removed). */
static float ncc(const float *a, const float *b)
{
    const int n = FID_W * FID_H;
    double ma = 0.0, mb = 0.0;
    for (int i = 0; i < n; i++) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < n; i++) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return aa > 0.0 && bb > 0.0 ? (float)(ab / SDL_sqrt(aa * bb)) : 1.0f;
}

/* Whether a row of the columns [x0, x1] with almost no ink lies between two rows with ink: a gap between the dot and the stem. */
static bool has_gap_row(const float *cov, int x0, int x1)
{
    int seen_ink = 0;
    bool gap = false;
    for (int y = 0; y < FID_H; y++) {
        float m = 0.0f;
        for (int x = x0; x <= x1 && x < FID_W; x++) {
            m = cov[y * FID_W + x] > m ? cov[y * FID_W + x] : m;
        }
        if (m >= 0.15f) {
            if (gap) {
                return true;
            }
            seen_ink = 1;
        } else if (seen_ink) {
            gap = true;
        }
    }
    return false;
}

static void check_fidelity(const char *rel)
{
    face f;
    EMB_REQUIRE(face_open(&f, rel));
    const float upem = (float)f.font.metrics.units_per_em;
    embi_hint_zones z;
    embi_hint_zones_measure(&z, upem, (float)f.font.metrics.x_height, extent_cb, &f);
    static const int sizes[] = {8, 9, 10, 12};
    for (uint32_t si = 0; si < 4; si++) {
        const float s = (float)sizes[si] / upem;
        for (const char *g = "abdeghijkmnpqstuyz"; *g != 0; g++) {
            uint32_t n;
            embi_qcurve *in = face_curves(&f, (uint32_t)*g, &n);
            EMB_REQUIRE(in != nullptr);
            embi_qcurve *out = SDL_malloc(sizeof *out * 2u * n);
            uint32_t m = embi_hint_glyph(&z, upem, s, in, n, out);
            if (m == 0u) {
                SDL_memcpy(out, in, sizeof *in * n);
                m = n;
            }
            float *ca = SDL_calloc(FID_W * FID_H, sizeof(float)), *cb = SDL_calloc(FID_W * FID_H, sizeof(float));
            fid_raster(in, n, s, ca);
            fid_raster(out, m, s, cb);
            const float r = ncc(ca, cb);
            SDL_Log("fidelity %s %c %dpx ncc=%.3f", rel, *g, sizes[si], (double)r);
            EMB_CHECK(r >= 0.85f);
            if (*g == 'i' || *g == 'j') {
                /* The dot is the contour that starts highest; the gap to the stem is the one of the unhinted outline. */
                float dot_lo = -1e9f, dot_x0 = 0.0f, dot_x1 = 0.0f, stem_hi;
                uint32_t start = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (i + 1 == n || in[i + 1].x0 != in[i].x1 || in[i + 1].y0 != in[i].y1) {
                        float lo, hi, xa = in[start].x0, xb = xa;
                        embi_hint_extent(in + start, i + 1 - start, &lo, &hi);
                        (void)hi;
                        for (uint32_t k = start; k <= i; k++) {
                            xa = in[k].x0 < xa ? in[k].x0 : xa;
                            xb = in[k].x0 > xb ? in[k].x0 : xb;
                        }
                        if (lo > dot_lo) {
                            dot_lo = lo;
                            dot_x0 = xa;
                            dot_x1 = xb;
                        }
                        start = i + 1;
                    }
                }
                /* The stem is the other contour: the highest top among those that are not the dot. */
                stem_hi = -1e9f;
                start = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (i + 1 == n || in[i + 1].x0 != in[i].x1 || in[i + 1].y0 != in[i].y1) {
                        float lo, hi;
                        embi_hint_extent(in + start, i + 1 - start, &lo, &hi);
                        if (lo != dot_lo) {
                            stem_hi = hi > stem_hi ? hi : stem_hi;
                        }
                        start = i + 1;
                    }
                }
                const bool gap_in = (dot_lo - stem_hi) * s >= 0.5f;
                const int c0 = (int)SDL_floorf(dot_x0 * s + 4.0f), c1 = (int)SDL_ceilf(dot_x1 * s + 4.0f) - 1;
                EMB_CHECK(has_gap_row(cb, c0, c1) == gap_in);
            }
            SDL_free(ca);
            SDL_free(cb);
            SDL_free(in);
            SDL_free(out);
        }
    }
    face_close(&f);
}

EMB_TEST(hint_fidelity_notosans)
{
    check_fidelity("tests/fonts/NotoSans-Regular.ttf");
}

EMB_TEST(hint_fidelity_sourcesans)
{
    check_fidelity("tests/fonts/SourceSans3-Regular.otf");
}

/* ------------------------------------------------------------------ through the renderer */

#define CELL_W 40
#define CELL_H 40
#define N_SZ 3u
static const float k_sz[N_SZ] = {11.0f, 13.0f, 16.0f};
#define GRID_W (2 * CELL_W)
#define GRID_H ((int)N_SZ * CELL_H)

typedef struct grid {
    embi_text *t;
    const char *family;
} grid;

static void grid_cb(embi_frame *f, void *user)
{
    grid *g = user;
    const float square[4] = {0, 0, 0, 0};
    const float page[4] = {0, 0, (float)GRID_W, (float)GRID_H};
    embi_frame_fill(f, page, square, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_GRAY, false);
    embi_str fam = tx_s(g->family);
    for (uint32_t k = 0; k < N_SZ; k++) {
        for (uint32_t gi = 0; gi < 2; gi++) {
            const char str[2] = {gi == 0 ? 'e' : 'z', 0};
            embi_text_style st;
            tx_style(&st, &fam, 1, k_sz[k]);
            embi_text_layout lay;
            embi_text_vruns vr = {0};
            if (!tx_shape(g->t, str, &st, &lay)) {
                continue;
            }
            if (embi_text_line(&lay, 0, lay.text_len, &vr)) {
                embi_text_draw(g->t, f, &lay, &vr, (float)(gi * CELL_W) + 8.25f, (float)((int)k * CELL_H + 30), RGBA(29, 29, 29, 255), 0, 0);
                embi_text_vruns_free(&vr);
            }
            embi_text_layout_free(&lay);
        }
    }
}

static float cov(const uint8_t *px, int x, int y)
{
    const float c = (255.0f - (float)px[((size_t)y * GRID_W + (size_t)x) * 4u]) / (255.0f - 29.0f);
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

/* The ink box of a cell and the peak over the rows [r0, r1) (tenths of the box) of the mean coverage of its middle columns. */
static float row_peak(const uint8_t *px, int gx, int gy, int t0, int t1)
{
    int minx = CELL_W, maxx = -1, miny = CELL_H, maxy = -1;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            if (cov(px, gx * CELL_W + x, gy * CELL_H + y) > 0.05f) {
                minx = x < minx ? x : minx;
                maxx = x > maxx ? x : maxx;
                miny = y < miny ? y : miny;
                maxy = y > maxy ? y : maxy;
            }
        }
    }
    if (maxx < 0) {
        return 0.0f;
    }
    const int cols = maxx - minx + 1, rows = maxy - miny + 1;
    const int r0 = miny + rows * t0 / 10, r1 = miny + rows * t1 / 10;
    const int x0 = minx + cols / 5, x1 = minx + cols - cols / 5;
    float peak = 0.0f;
    for (int y = r0; y < r1; y++) {
        float m = 0.0f;
        for (int x = x0; x < x1; x++) {
            m += cov(px, gx * CELL_W + x, gy * CELL_H + y);
        }
        m /= (float)(x1 - x0 > 0 ? x1 - x0 : 1);
        peak = m > peak ? m : peak;
    }
    return peak;
}

static void check_rendered(const char *file, const char *family)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s(family);
    EMB_REQUIRE(tx_add_file(t, file, &d) == 1u);
    grid g = {t, family};
    uint8_t *px = emb_test_render(GRID_W, GRID_H, grid_cb, &g);
    if (px == nullptr) {
        embi_text_destroy(t);
        EMB_RENDER_OR_SKIP(px);
        return;
    }
    for (int k = 0; k < (int)N_SZ; k++) {
        const float bar_e = row_peak(px, 0, k, 3, 7);
        const float bar_z_top = row_peak(px, 1, k, 0, 3), bar_z_bot = row_peak(px, 1, k, 7, 10);
        SDL_Log("hint %s %.0fpx e-bar=%.3f z-top=%.3f z-bottom=%.3f", family, (double)k_sz[k], (double)bar_e,
                (double)bar_z_top, (double)bar_z_bot);
        EMB_CHECK(bar_e >= 0.9f);
        EMB_CHECK(bar_z_top >= 0.9f);
        EMB_CHECK(bar_z_bot >= 0.9f);
    }
    SDL_free(px);
    embi_text_destroy(t);
}

EMB_TEST(hint_rendered_bars_notosans)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    check_rendered("tests/fonts/NotoSans-Regular.ttf", "Noto Test");
}

EMB_TEST(hint_rendered_bars_sourcesans)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    check_rendered("tests/fonts/SourceSans3-Regular.otf", "Source Test");
}

/* ------------------------------------------------------------------ natural output is unchanged */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ull;
    }
    return h;
}

static uint64_t natural_hash(const char *rel)
{
    face f;
    if (!face_open(&f, rel)) {
        return 0;
    }
    const float upem = (float)f.font.metrics.units_per_em;
    embi_hint_zones z;
    embi_hint_zones_measure(&z, upem, (float)f.font.metrics.x_height, extent_cb, &f);
    uint64_t h = 14695981039346656037ull;
    for (int size = 9; size <= 36; size++) {
        const float s = (float)size / upem;
        for (const char *g = "Hezxopdtlmnig"; *g != 0; g++) {
            uint32_t n;
            embi_qcurve *in = face_curves(&f, (uint32_t)*g, &n);
            if (in == nullptr) {
                continue;
            }
            embi_qcurve *out = SDL_malloc(sizeof *out * 4u * n);
            uint32_t *map = SDL_malloc(sizeof *map * (n + 1u));
            const uint32_t m = embi_hint_glyph_fit(&z, upem, s, in, n, out, map, false);
            h = fnv(h, &m, sizeof m);
            h = fnv(h, out, sizeof *out * m);
            h = fnv(h, map, sizeof *map * (m > 0 ? n + 1u : 0u));
            SDL_free(in);
            SDL_free(out);
            SDL_free(map);
        }
    }
    face_close(&f);
    return h;
}

EMB_TEST(hint_natural_unchanged)
{
    /* The y fitting without x fitting is what it was before the sharp mode: the hash of its output over a set of glyphs. */
    EMB_CHECK(natural_hash("tests/fonts/NotoSans-Regular.ttf") == 0x5fe65012925f767cull);
    EMB_CHECK(natural_hash("tests/fonts/SourceSans3-Regular.otf") == 0x6566eb1f0bb7883dull);
}

/* ------------------------------------------------------------------ x fitting */

/* The distinct x (device px) of the vertical segments at least 1 px tall, ascending. */
static uint32_t vedges(const embi_qcurve *c, uint32_t n, float s, float *xs, uint32_t max)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (SDL_fabsf(c[i].x0 - c[i].x1) > 0.01f || SDL_fabsf(c[i].cx - c[i].x0) > 0.01f ||
            SDL_fabsf(c[i].y0 - c[i].y1) * s < 1.0f) {
            continue;
        }
        const float x = c[i].x0 * s;
        uint32_t j = 0;
        while (j < k && SDL_fabsf(xs[j] - x) > 0.05f) {
            j++;
        }
        if (j == k && k < max) {
            xs[k++] = x;
        }
    }
    for (uint32_t a = 1; a < k; a++) {
        for (uint32_t b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
            const float t = xs[b];
            xs[b] = xs[b - 1];
            xs[b - 1] = t;
        }
    }
    return k;
}

/* The stems of the vertical edges of g fitted at size px: the edges are on columns, the stems at least 1 px wide and their
 * centres within half a px of where they were. stems: the number expected. */
static void check_stems(const face *f, const embi_hint_zones *z, char g, int px, uint32_t stems, bool with_zones)
{
    const float upem = (float)f->font.metrics.units_per_em;
    const float s = (float)px / upem;
    uint32_t n;
    embi_qcurve *in = face_curves(f, (uint32_t)g, &n);
    EMB_REQUIRE(in != nullptr);
    embi_qcurve *out = SDL_malloc(sizeof *out * 4u * n);
    const uint32_t m = embi_hint_glyph_fit(with_zones ? z : nullptr, upem, s, in, n, out, nullptr, true);
    EMB_CHECK(m > 0u);
    float a[16], b[16];
    const uint32_t ka = vedges(in, n, s, a, 16), kb = vedges(out, m, s, b, 16);
    EMB_CHECK(ka == 2u * stems);
    EMB_CHECK(kb == ka);
    for (uint32_t i = 0; i + 1 < kb && i + 1 < ka; i += 2) {
        EMB_CHECK(frac_off(b[i]) < 1.0f / 64.0f);
        EMB_CHECK(frac_off(b[i + 1]) < 1.0f / 64.0f);
        EMB_CHECK(b[i + 1] - b[i] >= 1.0f - 1.0f / 64.0f);
        EMB_CHECK(SDL_fabsf(0.5f * (b[i] + b[i + 1]) - 0.5f * (a[i] + a[i + 1])) <= 0.5f + 1.0f / 64.0f);
        EMB_CHECK(SDL_fabsf((b[i + 1] - b[i]) - (a[i + 1] - a[i])) <= 0.5f + 1.0f / 64.0f);
    }
    SDL_free(in);
    SDL_free(out);
}

EMB_TEST(hint_x_stems_whole_columns)
{
    face f;
    EMB_REQUIRE(face_open(&f, "tests/fonts/NotoSans-Regular.ttf"));
    embi_hint_zones z;
    embi_hint_zones_measure(&z, (float)f.font.metrics.units_per_em, (float)f.font.metrics.x_height, extent_cb, &f);
    for (int px = 9; px <= 36; px++) {
        check_stems(&f, &z, 'l', px, 1, true);
        check_stems(&f, &z, 'H', px, 2, true);
        check_stems(&f, &z, 'l', px, 1, false);
        check_stems(&f, &z, 'H', px, 2, false);
    }
    face_close(&f);
    EMB_REQUIRE(face_open(&f, "tests/fonts/SourceSans3-Regular.otf"));
    embi_hint_zones_measure(&z, (float)f.font.metrics.units_per_em, (float)f.font.metrics.x_height, extent_cb, &f);
    for (int px = 9; px <= 36; px++) {
        check_stems(&f, &z, 'H', px, 2, true);
    }
    face_close(&f);
}

/* Over many glyphs and sizes: deterministic, every point moves at most a px (plus the rounding), the map is monotone and the
 * contours keep their orientation. */
static void check_x_moves(const char *rel)
{
    face f;
    EMB_REQUIRE(face_open(&f, rel));
    const float upem = (float)f.font.metrics.units_per_em;
    embi_hint_zones z;
    embi_hint_zones_measure(&z, upem, (float)f.font.metrics.x_height, extent_cb, &f);
    for (int px = 9; px <= 36; px++) {
        const float s = (float)px / upem;
        for (const char *g = "HeznxopdtlmiguwIT"; *g != 0; g++) {
            uint32_t n;
            embi_qcurve *in = face_curves(&f, (uint32_t)*g, &n);
            EMB_REQUIRE(in != nullptr);
            embi_qcurve *out = SDL_malloc(sizeof *out * 4u * n), *out2 = SDL_malloc(sizeof *out * 4u * n);
            uint32_t *map = SDL_malloc(sizeof *map * (n + 1u)), *map2 = SDL_malloc(sizeof *map * (n + 1u));
            const uint32_t m = embi_hint_glyph_fit(&z, upem, s, in, n, out, map, true);
            const uint32_t m2 = embi_hint_glyph_fit(&z, upem, s, in, n, out2, map2, true);
            EMB_CHECK(m > 0u && m == m2);
            EMB_CHECK(SDL_memcmp(out, out2, sizeof *out * m) == 0 && SDL_memcmp(map, map2, sizeof *map * (n + 1u)) == 0);
            if (m > 0u) {
                EMB_CHECK(map[n] == m);
                for (uint32_t i = 0; i < n; i++) {
                    EMB_CHECK(map[i] < map[i + 1]);
                    EMB_CHECK(SDL_fabsf(out[map[i]].x0 - in[i].x0) * s <= 1.0f + 1.0f / 64.0f);
                }
                int sa[64], sb[64];
                const uint32_t ka = contour_signs(in, n, sa, 64), kb = contour_signs(out, m, sb, 64);
                EMB_CHECK(ka == kb);
                for (uint32_t i = 0; i < ka && i < kb; i++) {
                    EMB_CHECK(sa[i] == sb[i]);
                }
                /* The points move up to a px: an edge goes to the column that keeps its stem centre within half a px, and the
                 * width then rounds by up to half a px more, so an edge is off by up to 0.5 + 0.5. (The stem centres are held to
                 * 0.5 px in check_stems.) */
                /* A y-only fit of the same glyph keeps its y: x fitting does not touch the rows. */
                embi_qcurve *yo = SDL_malloc(sizeof *yo * 2u * n);
                const uint32_t my = embi_hint_glyph_map(&z, upem, s, in, n, yo, nullptr);
                if (my > 0u) {
                    float l0, h0, l1, h1;
                    embi_hint_extent(yo, my, &l0, &h0);
                    embi_hint_extent(out, m, &l1, &h1);
                    EMB_CHECK(SDL_fabsf(l0 - l1) * s < 1.0f / 64.0f && SDL_fabsf(h0 - h1) * s < 1.0f / 64.0f);
                }
                SDL_free(yo);
            }
            SDL_free(in);
            SDL_free(out);
            SDL_free(out2);
            SDL_free(map);
            SDL_free(map2);
        }
    }
    face_close(&f);
}

EMB_TEST(hint_x_moves_notosans)
{
    check_x_moves("tests/fonts/NotoSans-Regular.ttf");
}

EMB_TEST(hint_x_moves_sourcesans)
{
    check_x_moves("tests/fonts/SourceSans3-Regular.otf");
}

/* The fitted l and H rasterized by the reference coverage: a full-coverage column in every stem on a row above the baseline. */
EMB_TEST(hint_x_stem_coverage)
{
    face f;
    EMB_REQUIRE(face_open(&f, "tests/fonts/NotoSans-Regular.ttf"));
    const float upem = (float)f.font.metrics.units_per_em;
    embi_hint_zones z;
    embi_hint_zones_measure(&z, upem, (float)f.font.metrics.x_height, extent_cb, &f);
    const float s = 12.0f / upem;
    for (const char *g = "lH"; *g != 0; g++) {
        uint32_t n;
        embi_qcurve *in = face_curves(&f, (uint32_t)*g, &n);
        EMB_REQUIRE(in != nullptr);
        embi_qcurve *out = SDL_malloc(sizeof *out * 4u * n);
        const uint32_t m = embi_hint_glyph_fit(&z, upem, s, in, n, out, nullptr, true);
        EMB_REQUIRE(m > 0u);
        float xs[16];
        const uint32_t k = vedges(out, m, s, xs, 16);
        float *cov = SDL_calloc(FID_W * FID_H, sizeof(float));
        fid_raster(out, m, s, cov);
        const int row = FID_BASE - 2;
        for (uint32_t e = 0; e + 1 < k; e += 2) {
            float best = 0.0f;
            for (int x = (int)SDL_floorf(xs[e] + 4.0f + 0.01f); x < (int)SDL_ceilf(xs[e + 1] + 4.0f - 0.01f); x++) {
                best = cov[row * FID_W + x] > best ? cov[row * FID_W + x] : best;
                const float c = cov[row * FID_W + x];
                EMB_CHECK(c >= 0.95f);
            }
            EMB_CHECK(best >= 0.95f);
            SDL_Log("stem %c %.2f..%.2f best column %.3f", *g, (double)xs[e], (double)xs[e + 1], (double)best);
        }
        SDL_free(cov);
        SDL_free(in);
        SDL_free(out);
    }
    face_close(&f);
}

/* ------------------------------------------------------------------ pixel-art glyphs */

/* A square (x, y of its lower left corner, side d) as four axis-aligned segments, clockwise. */
static uint32_t square(embi_qcurve *out, float x, float y, float d)
{
    const float p[4][2] = {{x, y}, {x, y + d}, {x + d, y + d}, {x + d, y}};
    return poly(out, p, 4);
}

EMB_TEST(hint_pixel_grid_detect)
{
    embi_qcurve c[16];
    float u = 0.0f;
    /* Three squares of a 64 unit grid: the step is 64. */
    uint32_t n = square(c, 0, 0, 64);
    n += square(c + n, 128, 64, 64);
    n += square(c + n, 192, 192, 64);
    EMB_CHECK(embi_hint_pixel_grid(c, n, &u) && u == 64.0f);
    /* Ahem's square: the em, from 0.2 em below the baseline to 0.8 above. */
    n = square(c, 0, -200, 1000);
    EMB_CHECK(embi_hint_pixel_grid(c, n, &u) && u == 1000.0f);
    /* A 5 x 7 bitmap glyph on a grid of 125 units (an L of 1 x 3 and 3 x 1 cells). */
    {
        const float l[6][2] = {{0, 0}, {0, 375}, {125, 375}, {125, 125}, {375, 125}, {375, 0}};
        n = poly(c, l, 6);
        EMB_CHECK(embi_hint_pixel_grid(c, n, &u) && u == 125.0f);
    }
    /* A diagonal edge. */
    {
        const float tri[3][2] = {{0, 0}, {0, 64}, {64, 0}};
        n = poly(c, tri, 3);
        EMB_CHECK(!embi_hint_pixel_grid(c, n, &u));
    }
    /* A control point off its segment: a curve. */
    n = square(c, 0, 0, 64);
    c[0].cx += 16.0f;
    EMB_CHECK(!embi_hint_pixel_grid(c, n, &u));
    /* A point off the grid: the step shrinks under a unit, or the glyph spans too many steps; never 64. */
    n = square(c, 0, 0, 64);
    n += square(c + n, 128, 64, 64.5f);
    EMB_CHECK(!embi_hint_pixel_grid(c, n, &u));
    n = square(c, 0, 0, 64);
    n += square(c + n, 128, 64, 70);
    /* (steps of 2 units: 99 of them across the glyph, more than the 64 allowed) */
    EMB_CHECK(!embi_hint_pixel_grid(c, n, &u));
    /* A round glyph of a real face. */
    {
        face f;
        EMB_REQUIRE(face_open(&f, "tests/fonts/NotoSans-Regular.ttf"));
        uint32_t k;
        embi_qcurve *o = face_curves(&f, 'o', &k);
        EMB_REQUIRE(o != nullptr);
        EMB_CHECK(!embi_hint_pixel_grid(o, k, &u));
        SDL_free(o);
        face_close(&f);
    }
    EMB_CHECK(!embi_hint_pixel_grid(c, 0, &u));
}

/* The face decision: a text face is not pixel art, whatever its '.', '-' and 'I' are made of. */
static void check_face_not_pixel(const char *rel)
{
    face f;
    EMB_REQUIRE(face_open(&f, rel));
    const float upem = (float)f.font.metrics.units_per_em;
    static const char samples[] = "Hoaex0g";
    embi_qcurve *g[8];
    uint32_t cnt[8], k = 0;
    for (const char *c = samples; *c != 0; c++) {
        g[k] = face_curves(&f, (uint32_t)*c, &cnt[k]);
        k++;
    }
    float u = 0.0f;
    EMB_CHECK(!embi_hint_pixel_face((const embi_qcurve *const *)g, cnt, k, upem, &u));
    for (uint32_t i = 0; i < k; i++) {
        SDL_free(g[i]);
    }
    /* The rectangles of the face, alone, are no face either: the round letters decide. */
    static const char rects[] = ".-I";
    for (const char *c = rects; *c != 0; c++) {
        uint32_t n;
        embi_qcurve *r = face_curves(&f, (uint32_t)*c, &n);
        EMB_CHECK(r != nullptr);
        SDL_free(r);
    }
    face_close(&f);
}

EMB_TEST(hint_pixel_face_text_faces_are_not)
{
    check_face_not_pixel("tests/fonts/NotoSans-Regular.ttf");
    check_face_not_pixel("tests/fonts/SourceSans3-Regular.otf");
}

EMB_TEST(hint_pixel_face_decision)
{
    /* A synthetic pixel face on a 125 unit grid in a 1000 unit em: blocks and an L. */
    embi_qcurve a[16], b[16], c[16];
    uint32_t na = square(a, 125, 0, 250), nb, nc;
    const float l[6][2] = {{0, 0}, {0, 375}, {125, 375}, {125, 125}, {375, 125}, {375, 0}};
    nb = poly(b, l, 6);
    nc = square(c, 0, 0, 500);
    const embi_qcurve *g[3] = {a, b, c};
    uint32_t cnt[3] = {na, nb, nc};
    float u = 0.0f;
    EMB_CHECK(embi_hint_pixel_face(g, cnt, 3, 1000.0f, &u) && u == 125.0f);
    /* A glyph without an outline is skipped; the other two share a step of 250, four of which make the em: too coarse. */
    cnt[1] = 0;
    EMB_CHECK(!embi_hint_pixel_face(g, cnt, 3, 1000.0f, &u));
    EMB_CHECK(embi_hint_pixel_face(g, cnt, 3, 1250.0f, &u) && u == 250.0f);
    /* No glyph at all, or a diagonal one: not a face. */
    EMB_CHECK(!embi_hint_pixel_face(g, cnt, 0, 1000.0f, &u));
    const float tri[3][2] = {{0, 0}, {0, 125}, {125, 0}};
    embi_qcurve d[3];
    const uint32_t nd = poly(d, tri, 3);
    const embi_qcurve *g2[2] = {a, d};
    const uint32_t cnt2[2] = {na, nd};
    EMB_CHECK(!embi_hint_pixel_face(g2, cnt2, 2, 1000.0f, &u));
    /* The em must span 5 to 64 steps: a 1000 unit step (Ahem's squares) is one, a 125 unit step in a 100000 unit em is 800. */
    embi_qcurve e[4];
    const uint32_t ne = square(e, 0, -200, 1000);
    const embi_qcurve *g3[1] = {e};
    const uint32_t cnt3[1] = {ne};
    EMB_CHECK(!embi_hint_pixel_face(g3, cnt3, 1, 1000.0f, &u));
    const uint32_t cnt4[3] = {na, nb, nc};
    EMB_CHECK(!embi_hint_pixel_face(g, cnt4, 3, 100000.0f, &u));
    EMB_CHECK(embi_hint_pixel_face(g, cnt4, 3, 1000.0f, &u) && u == 125.0f);
    /* The glyph test against a face step. */
    EMB_CHECK(embi_hint_pixel_on_grid(a, na, 125.0f) && embi_hint_pixel_on_grid(a, na, 250.0f) &&
              !embi_hint_pixel_on_grid(a, na, 100.0f) && !embi_hint_pixel_on_grid(d, nd, 125.0f));
}

EMB_TEST(hint_pixel_scale_rule)
{
    /* Natural: only when unit * scale is within 0.02 of a whole number of at least 1. */
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 0.25f, false) == 0.25f);
    EMB_CHECK(SDL_fabsf(embi_hint_pixel_scale(64.0f, 3.0f / 64.0f + 0.0002f, false) - 3.0f / 64.0f) < 1e-9f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 1.5f / 64.0f, false) == 0.0f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 2.1f / 64.0f, false) == 0.0f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 0.5f / 64.0f, false) == 0.0f);
    EMB_CHECK(embi_hint_pixel_scale(1000.0f, 0.013f, false) == 0.013f);
    EMB_CHECK(embi_hint_pixel_scale(1000.0f, 0.0135f, false) == 0.0f);
    /* Sharp: always, at max(1, round(unit * scale)) px per unit. */
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 1.5f / 64.0f, true) == 2.0f / 64.0f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 2.4f / 64.0f, true) == 2.0f / 64.0f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 0.6f / 64.0f, true) == 1.0f / 64.0f);
    EMB_CHECK(embi_hint_pixel_scale(1000.0f, 0.0135f, true) == 0.014f);
    EMB_CHECK(embi_hint_pixel_scale(64.0f, 0.4f / 64.0f, true) == 0.0f);
    EMB_CHECK(embi_hint_pixel_scale(0.0f, 1.0f, true) == 0.0f);
}

EMB_TEST(hint_pixel_snap_edges)
{
    /* Ahem's square at 13.5 px: sharp snaps to 14 px per em, and every edge falls on a whole px with the control points on
     * their segments; the box is 14 px tall, 3 below and 11 above the baseline. */
    embi_qcurve c[4], o[4];
    const uint32_t n = square(c, 0, -200, 1000);
    float u;
    EMB_REQUIRE(embi_hint_pixel_grid(c, n, &u));
    EMB_CHECK(embi_hint_pixel_scale(u, 13.5f / 1000.0f, false) == 0.0f);
    const float s = embi_hint_pixel_scale(u, 13.5f / 1000.0f, true);
    EMB_REQUIRE(s > 0.0f);
    embi_hint_pixel_snap(c, n, s, o);
    float lo = 1e9f, hi = -1e9f, left = 1e9f, right = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(frac_off(o[i].x0 * s) < 1.0f / 64.0f && frac_off(o[i].y0 * s) < 1.0f / 64.0f);
        EMB_CHECK(frac_off(o[i].x1 * s) < 1.0f / 64.0f && frac_off(o[i].y1 * s) < 1.0f / 64.0f);
        EMB_CHECK(o[i].x1 == o[(i + 1) % n].x0 && o[i].y1 == o[(i + 1) % n].y0);
        const bool vert = o[i].x0 == o[i].x1;
        EMB_CHECK(vert ? o[i].cx == o[i].x0 : o[i].cy == o[i].y0);
        lo = o[i].y0 < lo ? o[i].y0 : lo;
        hi = o[i].y0 > hi ? o[i].y0 : hi;
        left = o[i].x0 < left ? o[i].x0 : left;
        right = o[i].x0 > right ? o[i].x0 : right;
    }
    EMB_CHECK(SDL_fabsf((hi - lo) * s - 14.0f) < 1.0f / 64.0f && SDL_fabsf(lo * s + 3.0f) < 1.0f / 64.0f);
    EMB_CHECK(SDL_fabsf((right - left) * s - 14.0f) < 1.0f / 64.0f);
    /* At 13 px, natural keeps the scale and gets the same edges as the plain outline (3 below, 10 above: 2.6 and 10.4 round). */
    const float sn = embi_hint_pixel_scale(u, 13.0f / 1000.0f, false);
    EMB_CHECK(sn == 13.0f / 1000.0f);
    embi_hint_pixel_snap(c, n, sn, o);
    EMB_CHECK(SDL_fabsf(o[0].y0 * sn + 3.0f) < 1.0f / 64.0f && SDL_fabsf(o[1].y0 * sn - 10.0f) < 1.0f / 64.0f);
}

int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
