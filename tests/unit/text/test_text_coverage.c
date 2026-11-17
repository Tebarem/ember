/*
 * Text coverage against an exact reference.
 *
 * A test string is drawn by the text pipeline in gray antialiasing, white on a transparent target, with the grid fit off
 * (a tiny skew in the transform turns the fit and the baseline snap off, as in test_text_crisp_lcd.c). The same outlines
 * go through the CPU box-filter reference (coverage_ref.h) with the same glyph-to-device map. The alpha channel of the
 * target is compared with the reference coverage:
 *   - mean and p99 of |alpha - ref| over the pixels where either is nonzero (raw: the GPU alpha is the text contrast
 *     curve applied to the coverage, nothing is undone), and the maximum;
 *   - holes: pixels with reference coverage 1 where the GPU is not within 2/255 of 1, leaks: pixels with reference
 *     coverage 0 where the GPU is not within 2/255 of 0.
 * One line per font and size is printed. The assertions are loose on purpose: they are the baseline, and the printed
 * numbers are the tuning gate for later work on the text pipeline (tighten the assertions as it improves).
 */
#include "text_fixture.h"

#include "coverage_ref.h"
#include "emb_gold.h"
#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>

#define TW 560
#define TH 64
#define TEXT_X 3.3f
#define TEXT_Y 40.6f
#define SKEW 1e-3f
#define TEXT_STRING "Hamburgefonstiv 0123 &@ewzk"
#define PI_D 3.14159265358979323846

/* ------------------------------------------------------------------ reference self-test */

static void quad_line(float (*q)[6], uint32_t *n, float x0, float y0, float x1, float y1)
{
    float *p = q[(*n)++];
    p[0] = x0;
    p[1] = y0;
    p[2] = 0.5f * (x0 + x1);
    p[3] = 0.5f * (y0 + y1);
    p[4] = x1;
    p[5] = y1;
}

static float overlap(float a0, float a1, float b0, float b1)
{
    const float lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0.0f;
}

EMB_TEST(reference_rect_and_circle)
{
    /* A 3.5 x 2.25 rectangle at a fractional offset: every pixel is the product of two overlaps. */
    const float rx0 = 1.3f, ry0 = 0.6f, rx1 = rx0 + 3.5f, ry1 = ry0 + 2.25f;
    float q[64][6];
    uint32_t n = 0;
    quad_line(q, &n, rx0, ry0, rx1, ry0);
    quad_line(q, &n, rx1, ry0, rx1, ry1);
    quad_line(q, &n, rx1, ry1, rx0, ry1);
    quad_line(q, &n, rx0, ry1, rx0, ry0);
    float out[8 * 6];
    covref_raster((const float (*)[6])q, n, 8, 6, out);
    float worst = 0.0f;
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 8; x++) {
            const float want = overlap((float)x, (float)x + 1.0f, rx0, rx1) * overlap((float)y, (float)y + 1.0f, ry0, ry1);
            const float d = SDL_fabsf(out[y * 8 + x] - want);
            worst = d > worst ? d : worst;
        }
    }
    SDL_Log("reference rect: worst error %.6f", (double)worst);
    EMB_CHECK(worst < 1e-4f);

    /* The same rectangle drawn the other way round gives the same coverage. */
    n = 0;
    quad_line(q, &n, rx0, ry0, rx0, ry1);
    quad_line(q, &n, rx0, ry1, rx1, ry1);
    quad_line(q, &n, rx1, ry1, rx1, ry0);
    quad_line(q, &n, rx1, ry0, rx0, ry0);
    float rev[8 * 6];
    covref_raster((const float (*)[6])q, n, 8, 6, rev);
    float dd = 0.0f;
    for (int i = 0; i < 48; i++) {
        dd = SDL_fabsf(rev[i] - out[i]) > dd ? SDL_fabsf(rev[i] - out[i]) : dd;
    }
    EMB_CHECK(dd < 1e-6f);

    /* A circle of radius 10 made of 32 quads: total area within 0.1% of pi r^2. */
    const float cx = 12.3f, cy = 11.7f, r = 10.0f;
    const int segs = 32;
    const float step = (float)(2.0 * PI_D) / (float)segs;
    n = 0;
    for (int i = 0; i < segs; i++) {
        const float a0 = (float)i * step, a1 = a0 + step, am = 0.5f * (a0 + a1);
        const float rc = r / SDL_cosf(0.5f * step);
        float *p = q[n++];
        p[0] = cx + r * SDL_cosf(a0);
        p[1] = cy + r * SDL_sinf(a0);
        p[2] = cx + rc * SDL_cosf(am);
        p[3] = cy + rc * SDL_sinf(am);
        p[4] = cx + r * SDL_cosf(a1);
        p[5] = cy + r * SDL_sinf(a1);
    }
    float circ[26 * 26];
    covref_raster((const float (*)[6])q, n, 26, 26, circ);
    double area = 0.0;
    for (int i = 0; i < 26 * 26; i++) {
        area += (double)circ[i];
    }
    const double want = PI_D * (double)r * (double)r;
    SDL_Log("reference circle: area %.3f, pi r^2 %.3f (%.4f%%)", area, want, 100.0 * (area - want) / want);
    EMB_CHECK(SDL_fabs(area - want) < 0.001 * want);
}

/* ------------------------------------------------------------------ the comparison */

typedef struct curve_buf {
    float (*q)[6];
    uint32_t n, cap;
} curve_buf;

static void collect(void *user, const float q[6])
{
    curve_buf *b = user;
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2u : 1024u;
        b->q = SDL_realloc(b->q, (size_t)b->cap * sizeof *b->q);
        if (b->q == nullptr) {
            b->n = b->cap = 0;
            return;
        }
    }
    /* The map to the device: the tiny skew of the transform (x += SKEW * y). */
    float *o = b->q[b->n++];
    for (int i = 0; i < 6; i += 2) {
        o[i] = q[i] + SKEW * q[i + 1];
        o[i + 1] = q[i + 1];
    }
}

typedef struct scene {
    embi_text *t;
    embi_text_layout lay;
    embi_text_vruns vr;
} scene;

static void draw_scene(embi_frame *f, void *user)
{
    scene *s = user;
    const float xf[6] = {1.0f, 0.0f, SKEW, 1.0f, 0.0f, 0.0f};
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_GRAY, false);
    embi_text_draw(s->t, f, &s->lay, &s->vr, TEXT_X, TEXT_Y, RGBA(255, 255, 255, 255), embi_frame_xform(f, xf), 0);
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct stats {
    double mean, p99, max;
    uint32_t holes, leaks, glyph_px;
} stats;

enum { M_OK = 0, M_FAIL = 1, M_NO_GPU = 2 };

static int measure(embi_text *t, const char *family, float size, stats *st)
{
    SDL_memset(st, 0, sizeof *st);
    scene s = {.t = t};
    embi_str fam = tx_s(family);
    embi_text_style style;
    tx_style(&style, &fam, 1, size);
    if (!tx_shape(t, TEXT_STRING, &style, &s.lay)) {
        return M_FAIL;
    }
    if (!embi_text_line(&s.lay, 0, s.lay.text_len, &s.vr)) {
        embi_text_layout_free(&s.lay);
        return M_FAIL;
    }
    uint8_t *px = emb_test_render(TW, TH, draw_scene, &s);
    if (px == nullptr) {
        embi_text_vruns_free(&s.vr);
        embi_text_layout_free(&s.lay);
        return emb_test_cur_skip != nullptr ? M_NO_GPU : M_FAIL;
    }
    curve_buf cb = {0};
    embi_text_outline(t, &s.lay, &s.vr, TEXT_X, TEXT_Y, collect, &cb);
    float *ref = SDL_calloc((size_t)TW * TH, sizeof(float));
    float *diff = SDL_calloc((size_t)TW * TH, sizeof(float));
    bool ok = ref != nullptr && diff != nullptr && cb.n > 0;
    if (ok) {
        covref_raster((const float (*)[6])cb.q, cb.n, TW, TH, ref);
        uint32_t nd = 0;
        double sum = 0.0;
        for (uint32_t i = 0; i < (uint32_t)TW * TH; i++) {
            const float g = (float)px[i * 4 + 3] / 255.0f;
            const float r = ref[i];
            if (r > 0.002f) {
                st->glyph_px++;
            }
            if (r > 0.998f && g < 1.0f - 2.0f / 255.0f - 1e-4f) {
                st->holes++;
            }
            if (r < 0.002f && g > 2.0f / 255.0f + 1e-4f) {
                st->leaks++;
            }
            if (r > 0.002f || px[i * 4 + 3] != 0) {
                const float d = SDL_fabsf(g - r);
                diff[nd++] = d;
                sum += (double)d;
                st->max = (double)d > st->max ? (double)d : st->max;
            }
        }
        if (nd > 0) {
            SDL_qsort(diff, nd, sizeof(float), cmp_float);
            st->mean = sum / (double)nd;
            st->p99 = (double)diff[(size_t)((double)(nd - 1) * 0.99)];
        }
        ok = st->glyph_px > 0;
    }
    SDL_free(ref);
    SDL_free(diff);
    SDL_free(cb.q);
    SDL_free(px);
    embi_text_vruns_free(&s.vr);
    embi_text_layout_free(&s.lay);
    return ok ? M_OK : M_FAIL;
}

EMB_TEST(coverage_against_reference)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    static const struct {
        const char *file, *family, *label;
    } fonts[] = {
        {"tests/fonts/NotoSans-Regular.ttf", "Noto Test", "NotoSans"},
        {"tests/fonts/SourceSans3-Regular.otf", "Source Test", "SourceSans3"},
    };
    static const float sizes[] = {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 32};
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    for (uint32_t i = 0; i < 2; i++) {
        embi_face_desc d = {0};
        d.family = tx_s(fonts[i].family);
        EMB_REQUIRE(tx_add_file(t, fonts[i].file, &d) == 1);
    }
    for (uint32_t i = 0; i < 2; i++) {
        for (uint32_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            stats st;
            const int rc = measure(t, fonts[i].family, sizes[k], &st);
            if (rc == M_NO_GPU) {
                embi_text_destroy(t);
                EMB_SKIP("no GPU device");
            }
            EMB_CHECK(rc == M_OK);
            SDL_Log("coverage %s %.0fpx mean=%.4f p99=%.4f max=%.4f holes=%u leaks=%u (glyph px %u)", fonts[i].label,
                    (double)sizes[k], st.mean, st.p99, st.max, st.holes, st.leaks, st.glyph_px);
            EMB_CHECK(st.holes + st.leaks <= st.glyph_px / 100u);
            EMB_CHECK(st.mean <= 0.08);
        }
    }
    embi_text_destroy(t);
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
