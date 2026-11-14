/*
 * Crisp text at 1x through a view (W4h). The page tests/pages/text-crisp.html is
 * rendered by an offscreen view at scale 1 in grayscale, and the stems, the x-height bars and the
 * baseline rows of the default face are measured on the pixels:
 *   - every 1 px vertical stem (the letter l) at 13 to 16 px has a darkest column of at least 85%
 *     of the text color's contrast,
 *   - the top row and the bottom row of the z bars (the x-height and the baseline) are at least 85%
 *     dark, and the row above the top bar and the row below the bottom bar stay light,
 * The LCD mode, its fallbacks and the ink-shift bound of the fit are tested in tests/unit/text/test_text_crisp_lcd.c.
 * Set EMBER_CRISP_OUT=<dir> to write the render (crisp_<mode>.bmp) for inspection.
 */
#include "emb_test.h"
#include "emb_gold.h"

#include "crisp_rig.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#define PAGE_W 260
#define PAGE_H 192
#define TEXT_LUM 29.0

typedef struct stem_stats {
    int stems;
    double worst, mean;
} stem_stats;

/* Darkness of pixel (x, y): 1 is the text color, 0 the white page. */
static double dark(const uint8_t *px, int w, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
    double lum = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    double d = (255.0 - lum) / (255.0 - TEXT_LUM);
    return d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
}

/* The rows of the band (24 px) that hold ink: first and last. */
static bool ink_rows(const uint8_t *px, int w, int y0, int *r0, int *r1)
{
    int a = -1, b = -1;
    for (int y = y0; y < y0 + 24; y++) {
        for (int x = 0; x < w; x++) {
            if (dark(px, w, x, y) > 0.1) {
                if (a < 0) {
                    a = y;
                }
                b = y;
                break;
            }
        }
    }
    *r0 = a;
    *r1 = b;
    return a >= 0;
}

/* Stems of the band: clusters of columns that are dark at the middle row of the ink. */
static stem_stats measure_stems(const uint8_t *px, int w, int y0)
{
    stem_stats s = {0, 1.0, 0.0};
    int r0, r1;
    if (!ink_rows(px, w, y0, &r0, &r1)) {
        return s;
    }
    int mid = (r0 + r1) / 2;
    double best = 0.0;
    bool in = false;
    for (int x = 0; x <= w; x++) {
        double d = x < w ? dark(px, w, x, mid) : 0.0;
        if (d > 0.08) {
            in = true;
            best = d > best ? d : best;
        } else if (in) {
            in = false;
            s.stems++;
            s.mean += best;
            s.worst = best < s.worst ? best : s.worst;
            best = 0.0;
        }
    }
    if (s.stems > 0) {
        s.mean /= s.stems;
    }
    return s;
}

/* The bars of the z band: the darkest of each row over the columns, top and bottom rows of the ink. */
typedef struct bar_stats {
    double top, above, bottom, below;
} bar_stats;

static double row_dark(const uint8_t *px, int w, int y)
{
    /* The mean of the darkest 5 columns, so that one glyph's corner does not decide. */
    double best[5] = {0};
    for (int x = 0; x < w; x++) {
        double d = dark(px, w, x, y);
        for (int k = 0; k < 5; k++) {
            if (d > best[k]) {
                double t = best[k];
                best[k] = d;
                d = t;
            }
        }
    }
    return (best[0] + best[1] + best[2] + best[3] + best[4]) / 5.0;
}

static bar_stats measure_bars(const uint8_t *px, int w, int y0)
{
    bar_stats b = {0, 0, 0, 0};
    int r0, r1;
    if (!ink_rows(px, w, y0, &r0, &r1)) {
        return b;
    }
    b.top = row_dark(px, w, r0);
    b.bottom = row_dark(px, w, r1);
    b.above = r0 > y0 ? row_dark(px, w, r0 - 1) : 0.0;
    b.below = r1 < y0 + 23 ? row_dark(px, w, r1 + 1) : 0.0;
    return b;
}

static void check_page(const uint8_t *px, const char *label)
{
    static const int sizes[4] = {13, 14, 15, 16};
    for (int i = 0; i < 4; i++) {
        stem_stats st = measure_stems(px, PAGE_W, 48 * i);
        bar_stats br = measure_bars(px, PAGE_W, 48 * i + 24);
        SDL_Log("%s %dpx: stems %d worst %.2f mean %.2f | bars top %.2f (above %.2f) bottom %.2f (below %.2f)",
                label, sizes[i], st.stems, st.worst, st.mean, br.top, br.above, br.bottom, br.below);
        EMB_CHECK(st.stems >= 15);
        EMB_CHECK(br.above <= 0.10);
        EMB_CHECK(br.below <= 0.10);
        EMB_CHECK(st.worst >= 0.85);
        EMB_CHECK(st.mean >= 0.90);
        EMB_CHECK(br.top >= 0.85);
        EMB_CHECK(br.bottom >= 0.85);
    }
}

/* Pixels whose red and blue differ by more than 24. */
static int fringe_pixels(const uint8_t *px, int w, int h)
{
    int n = 0;
    for (int i = 0; i < w * h; i++) {
        int d = (int)px[i * 4] - (int)px[i * 4 + 2];
        n += (d > 24 || d < -24) ? 1 : 0;
    }
    return n;
}

EMB_TEST(crisp_gray_page_measurements)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    crisp_page pg;
    if (!crisp_render_page("tests/pages/text-crisp.html", PAGE_W, PAGE_H, EMB_TEXT_AA_GRAY, &pg)) {
        EMB_SKIP("no GPU device");
    }
    crisp_dump(&pg, "gray");
    check_page(pg.px, "gray");
    crisp_free(&pg);
}

EMB_TEST(crisp_lcd_page_dump)
{
    crisp_page pg;
    if (!crisp_render_page("tests/pages/text-crisp.html", PAGE_W, PAGE_H, EMB_TEXT_AA_LCD_RGB, &pg)) {
        EMB_SKIP("no GPU device");
    }
    crisp_dump(&pg, "lcd");
    crisp_free(&pg);
}

/* Text in an opacity group through a view in LCD mode is drawn gray (the layer has a transparent backdrop), and
 * plain text has fringes. */
EMB_TEST(view_opacity_group_text_is_gray)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    static const char plain[] =
        "<!doctype html><style>body{margin:0;background:#fff;color:#1d1d1d;font:15px sans-serif}"
        "p{margin:8px;height:20px}</style><p>Hello lllxxx 123</p>";
    static const char grouped[] =
        "<!doctype html><style>body{margin:0;background:#fff;color:#1d1d1d;font:15px sans-serif}"
        "p{margin:8px;height:20px;opacity:.9;position:relative}"
        "i{position:absolute;left:0;top:0;width:3px;height:3px;background:#fff}</style>"
        "<p>Hello lllxxx 123<i></i><i></i></p>";
    crisp_page a, b;
    if (!crisp_render_html(plain, sizeof plain - 1, 200, 40, EMB_TEXT_AA_LCD_RGB, &a)) {
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(crisp_render_html(grouped, sizeof grouped - 1, 200, 40, EMB_TEXT_AA_LCD_RGB, &b));
    crisp_dump(&a, "view_plain_lcd");
    crisp_dump(&b, "view_group_lcd");
    EMB_CHECK(fringe_pixels(a.px, 200, 40) > 30);
    EMB_CHECK_EQ_INT(fringe_pixels(b.px, 200, 40), 0);
    crisp_free(&a);
    crisp_free(&b);
}

/* Report (no assertion): the time of a full redraw of the todomvc page with gray and with LCD text, on this device, for the
 * cost note of W4h. Each frame is forced by switching the mode, so the two are measured alternately. */
EMB_TEST(report_text_draw_cost)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/tests/pages/static-todomvc.html", EMBER_SOURCE_DIR);
    size_t n = 0;
    char *html = SDL_LoadFile(path, &n);
    if (html == nullptr) {
        EMB_SKIP("no page");
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        SDL_free(html);
        EMB_SKIP("no GPU device");
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view_desc d;
    SDL_memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = 800;
    d.height = 600;
    d.scale = 1.0f;
    d.device = dev;
    emb_view *v = emb_view_create(rt, &d);
    EMB_REQUIRE(v != nullptr && emb_view_load_html(v, html, n, nullptr));
    double total[2] = {0.0, 0.0};
    const int rounds = 30;
    for (int r = -3; r < rounds; r++) {
        for (int m = 0; m < 2; m++) {
            emb_view_set_text_aa(v, m == 0 ? EMB_TEXT_AA_GRAY : EMB_TEXT_AA_LCD_RGB);
            uint64_t t0 = SDL_GetPerformanceCounter();
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
            (void)emb_view_render(v, cmd);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            if (fence != nullptr) {
                SDL_WaitForGPUFences(dev, true, &fence, 1);
                SDL_ReleaseGPUFence(dev, fence);
            }
            if (r == -3 && m == 1) {
                SDL_Log("first LCD frame (builds the three LCD pipelines): %.2f ms", (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
            }
            if (r >= 0) {
                total[m] += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            }
        }
    }
    SDL_Log("full redraw of static-todomvc at 800x600 1x, record + GPU wait: gray %.2f ms, LCD %.2f ms (mean of %d)",
            total[0] / rounds, total[1] / rounds, rounds);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
    SDL_free(html);
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
