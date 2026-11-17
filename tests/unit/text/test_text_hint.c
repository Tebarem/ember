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

int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
