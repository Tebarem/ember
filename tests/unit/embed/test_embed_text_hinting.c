/*
 * Text hinting modes of a view (emb_text_hinting, text-rendering):
 *   - an emb_view_desc without the text_hinting field (the old size) creates a NATURAL view;
 *   - NONE differs from NATURAL at 13 px, and a NONE view draws the same pixels as a NATURAL view whose text is
 *     text-rendering: geometricPrecision;
 *   - SHARP rounds the pen x of a glyph to a whole device px: a glyph at pen x 10.3 and one at 10.6 render the same
 *     pixels, one px apart.
 * Offscreen views at scale 1, grayscale text, read back from the GPU. Skips without a GPU device.
 */
#include "emb_test.h"

#include "crisp_rig.h"

#include <stddef.h>

#include <SDL3/SDL_init.h>

#define PAGE_W 120
#define PAGE_H 40

static const char HTML_PLAIN[] = "<body style='margin:0'><p style='margin:0;font-size:13px'>lIlHxo 0123 Hamburg</p></body>";
static const char HTML_GEOM[] = "<body style='margin:0'><p style='margin:0;font-size:13px;text-rendering:geometricPrecision'>"
                                "lIlHxo 0123 Hamburg</p></body>";
static const char HTML_PEN_A[] = "<body style='margin:0'><p style='position:absolute;left:10.3px;top:8px;margin:0;"
                                 "font-size:13px'>l</p></body>";
static const char HTML_PEN_B[] = "<body style='margin:0'><p style='position:absolute;left:10.6px;top:8px;margin:0;"
                                 "font-size:13px'>l</p></body>";

/* The view scale of render_html (the page is w x h CSS px; the pixels read back are scaled). */
static float g_scale = 1.0f;

/* Renders html through an offscreen view with the given hinting. old_size gives the descriptor the size it had before
 * text_hinting was appended (the field is then not read, though it is set to the mode here). */
static bool render_html(const char *html, int w, int h, emb_text_hinting mode, bool old_size, crisp_page *out)
{
    out->px = nullptr;
    out->w = (int)((float)w * g_scale);
    out->h = (int)((float)h * g_scale);
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        return false;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = old_size ? (uint32_t)offsetof(emb_view_desc, text_hinting) : (uint32_t)sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = (uint32_t)w;
    d.height = (uint32_t)h;
    d.scale = g_scale;
    d.device = dev;
    d.text_aa = EMB_TEXT_AA_GRAY;
    d.text_hinting = mode;
    emb_view *v = rt != nullptr ? emb_view_create(rt, &d) : nullptr;
    if (v != nullptr && emb_view_load_html(v, html, strlen(html), nullptr)) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd != nullptr) {
            (void)emb_view_render(v, cmd);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            if (fence != nullptr) {
                SDL_WaitForGPUFences(dev, true, &fence, 1);
                SDL_ReleaseGPUFence(dev, fence);
            }
            out->px = crisp_readback(dev, emb_view_texture(v), (uint32_t)out->w, (uint32_t)out->h);
        }
    }
    if (v != nullptr) {
        emb_view_destroy(v);
    }
    if (rt != nullptr) {
        emb_runtime_destroy(rt);
    }
    embi_gpu_destroy(g);
    return out->px != nullptr;
}

/* The pixels of a that differ from those of b moved dx px to the right (b(x + dx, y) against a(x, y)), over the area both
 * cover. */
static int count_diff(const crisp_page *a, const crisp_page *b, int dx)
{
    int n = 0;
    for (int y = 0; y < a->h; y++) {
        for (int x = 0; x < a->w; x++) {
            int bx = x + dx;
            if (bx < 0 || bx >= b->w) {
                continue;
            }
            const uint8_t *p = a->px + ((size_t)y * (size_t)a->w + (size_t)x) * 4u;
            const uint8_t *q = b->px + ((size_t)y * (size_t)b->w + (size_t)bx) * 4u;
            if (memcmp(p, q, 4) != 0) {
                n++;
            }
        }
    }
    return n;
}

/* The pixels that are not white. */
static int ink(const crisp_page *p)
{
    int n = 0;
    for (int i = 0; i < p->w * p->h; i++) {
        if (p->px[(size_t)i * 4u] < 250) {
            n++;
        }
    }
    return n;
}

static void init_video(void)
{
    (void)SDL_Init(SDL_INIT_VIDEO);
}

EMB_TEST(text_hinting_old_desc_size_is_natural)
{
    init_video();
    crisp_page natural, old;
    if (!render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &natural)) {
        EMB_SKIP("no GPU device");
    }
    if (!render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NONE, true, &old)) {
        crisp_free(&natural);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&natural) > 0);
    EMB_CHECK(count_diff(&natural, &old, 0) == 0);
    crisp_free(&natural);
    crisp_free(&old);
}

EMB_TEST(text_hinting_none_differs_from_natural_at_13px)
{
    init_video();
    crisp_page natural, none;
    if (!render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &natural)) {
        EMB_SKIP("no GPU device");
    }
    if (!render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NONE, false, &none)) {
        crisp_free(&natural);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&none) > 0);
    EMB_CHECK(count_diff(&natural, &none, 0) > 0);
    crisp_free(&natural);
    crisp_free(&none);
}

EMB_TEST(text_hinting_none_equals_geometric_precision_under_natural)
{
    init_video();
    crisp_page none, geom, natural;
    if (!render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NONE, false, &none)) {
        EMB_SKIP("no GPU device");
    }
    if (!render_html(HTML_GEOM, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &geom) ||
        !render_html(HTML_PLAIN, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &natural)) {
        crisp_free(&none);
        crisp_free(&geom);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&geom) > 0);
    EMB_CHECK(count_diff(&none, &geom, 0) == 0);
    EMB_CHECK(count_diff(&natural, &geom, 0) > 0);
    crisp_free(&none);
    crisp_free(&geom);
    crisp_free(&natural);
}

EMB_TEST(text_hinting_sharp_rounds_pen_x_to_device_px)
{
    init_video();
    crisp_page a, b;
    if (!render_html(HTML_PEN_A, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &a)) {
        EMB_SKIP("no GPU device");
    }
    if (!render_html(HTML_PEN_B, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &b)) {
        crisp_free(&a);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&a) > 0);
    /* 10.3 rounds to pen 10 and 10.6 to pen 11: b is a shifted one px right, pixel for pixel. */
    EMB_CHECK(count_diff(&a, &b, 1) == 0);
    crisp_free(&a);
    crisp_free(&b);
}

static const char HTML_SHARP_L[] = "<body style='margin:0'><p style='position:absolute;left:10px;top:8px;margin:0;"
                                   "font-size:12px'>l</p></body>";
static const char HTML_SHARP_H[] = "<body style='margin:0'><p style='position:absolute;left:10px;top:8px;margin:0;"
                                   "font-size:12px'>H</p></body>";

/* The number of columns in which at least nine tenths of the rows that carry ink have coverage of 0.95 or more. */
static int solid_columns(const crisp_page *p)
{
    int rows = 0;
    for (int y = 0; y < p->h; y++) {
        for (int x = 0; x < p->w; x++) {
            if (p->px[((size_t)y * (size_t)p->w + (size_t)x) * 4u] < 250) {
                rows++;
                break;
            }
        }
    }
    int cols = 0;
    for (int x = 0; x < p->w; x++) {
        int solid = 0;
        for (int y = 0; y < p->h; y++) {
            solid += p->px[((size_t)y * (size_t)p->w + (size_t)x) * 4u] <= 12 ? 1 : 0;
        }
        cols += rows > 0 && solid * 10 >= rows * 9 ? 1 : 0;
    }
    return cols;
}

EMB_TEST(text_hinting_sharp_stems_have_a_solid_column)
{
    init_video();
    crisp_page l, h, l2;
    if (!render_html(HTML_SHARP_L, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &l)) {
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(render_html(HTML_SHARP_H, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &h));
    EMB_REQUIRE(render_html(HTML_SHARP_L, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &l2));
    EMB_CHECK(ink(&l) > 0);
    EMB_CHECK(solid_columns(&l) >= 1);
    EMB_CHECK(solid_columns(&h) >= 2);
    /* Deterministic. */
    EMB_CHECK(count_diff(&l, &l2, 0) == 0);
    crisp_free(&l);
    crisp_free(&h);
    crisp_free(&l2);
}

/* The control: NATURAL does not round the pen, so a glyph at 10.3 and one at 10.6 are not one shifted copy of the other. */
EMB_TEST(text_hinting_natural_keeps_fractional_pen_x)
{
    init_video();
    crisp_page a, b;
    if (!render_html(HTML_PEN_A, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &a)) {
        EMB_SKIP("no GPU device");
    }
    if (!render_html(HTML_PEN_B, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &b)) {
        crisp_free(&a);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&a) > 0);
    EMB_CHECK(ink(&b) > 0);
    EMB_CHECK(count_diff(&a, &b, 1) != 0);
    crisp_free(&a);
    crisp_free(&b);
}

/* SHARP at a fractional view scale: the pen is a whole device px (10.3 and 10.6 CSS px are 15.45 and 15.9 device px, 15 and 16)
 * and the stem is still a solid column. */
EMB_TEST(text_hinting_sharp_at_scale_1_5)
{
    init_video();
    g_scale = 1.5f;
    crisp_page a, b, l;
    bool ok = render_html(HTML_PEN_A, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &a);
    if (!ok) {
        g_scale = 1.0f;
        EMB_SKIP("no GPU device");
    }
    ok = render_html(HTML_PEN_B, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &b);
    const bool ok2 = render_html(HTML_SHARP_L, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &l);
    g_scale = 1.0f;
    EMB_REQUIRE(ok && ok2);
    EMB_CHECK(ink(&a) > 0);
    EMB_CHECK(count_diff(&a, &b, 1) == 0);
    EMB_CHECK(solid_columns(&l) >= 1);
    crisp_free(&a);
    crisp_free(&b);
    crisp_free(&l);
}

#define P(left, top, extra) "<body style='margin:0'><p style='position:absolute;left:" left ";top:" top ";margin:0;font-size:12px;" extra "'>l</p></body>"

/* The pixels that are neither paper nor solid ink. */
static int partial_pixels(const crisp_page *p)
{
    int n = 0;
    for (int i = 0; i < p->w * p->h; i++) {
        const uint8_t v = p->px[(size_t)i * 4u];
        n += v > 12 && v < 243 ? 1 : 0;
    }
    return n;
}

static bool two_renders(const char *ha, const char *hb, emb_text_hinting mode, crisp_page *a, crisp_page *b)
{
    if (!render_html(ha, PAGE_W, PAGE_H, mode, false, a)) {
        return false;
    }
    if (!render_html(hb, PAGE_W, PAGE_H, mode, false, b)) {
        crisp_free(a);
        return false;
    }
    return true;
}

/* An anisotropic target (scale(1.5, 1)): SHARP rounds nothing, so the pen does not land on whole device px. */
EMB_TEST(text_hinting_sharp_anisotropic_does_not_round_pen)
{
    init_video();
    crisp_page a, b;
    char ha[256], hb[256];
    SDL_snprintf(ha, sizeof ha, "%s", P("10.3px", "8px", "transform:scale(1.5,1);transform-origin:0 0"));
    SDL_snprintf(hb, sizeof hb, "%s", P("10.6px", "8px", "transform:scale(1.5,1);transform-origin:0 0"));
    if (!two_renders(ha, hb, EMB_TEXT_HINTING_SHARP, &a, &b)) {
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ink(&a) > 0 && ink(&b) > 0);
    /* A rounded pen would make b a copy of a moved by whole px (0.45 px * 1.5 rounds to one of them). */
    EMB_CHECK(count_diff(&a, &b, 0) != 0 && count_diff(&a, &b, 1) != 0);
    crisp_free(&a);
    crisp_free(&b);
}

/* A synthetic oblique gets no x fitting: SHARP and NATURAL draw it alike at a whole pen x, while the upright glyph differs. */
EMB_TEST(text_hinting_sharp_oblique_has_no_x_fit)
{
    init_video();
    crisp_page sn, nn, su, nu;
    const char *ob = P("10px", "8px", "font-style:oblique");
    const char *up = P("10px", "8px", "");
    if (!two_renders(ob, ob, EMB_TEXT_HINTING_SHARP, &sn, &nn)) {
        EMB_SKIP("no GPU device");
    }
    crisp_free(&nn);
    EMB_REQUIRE(render_html(ob, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &nn));
    EMB_REQUIRE(two_renders(up, up, EMB_TEXT_HINTING_SHARP, &su, &nu));
    crisp_free(&nu);
    EMB_REQUIRE(render_html(up, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &nu));
    EMB_CHECK(ink(&sn) > 0);
    EMB_CHECK(count_diff(&su, &nu, 0) > 0);
    EMB_CHECK(count_diff(&sn, &nn, 0) == 0);
    crisp_free(&sn);
    crisp_free(&nn);
    crisp_free(&su);
    crisp_free(&nu);
}

/* A synthetic bold stem keeps its edges on whole columns in SHARP. */
EMB_TEST(text_hinting_sharp_bold_stem_on_columns)
{
    init_video();
    crisp_page b, n;
    const char *bold = P("10px", "8px", "font-weight:700");
    if (!render_html(bold, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &b)) {
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(render_html(bold, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NATURAL, false, &n));
    EMB_CHECK(ink(&b) > 0);
    EMB_CHECK(solid_columns(&b) >= 1);
    EMB_CHECK(partial_pixels(&b) == 0);
    SDL_Log("bold l: sharp partial %d natural partial %d", partial_pixels(&b), partial_pixels(&n));
    crisp_free(&b);
    crisp_free(&n);
}

/* NONE draws at the true baseline: a glyph at a fractional baseline keeps partial rows, which SHARP and NATURAL snap away. */
EMB_TEST(text_hinting_none_leaves_fractional_baseline)
{
    init_video();
    crisp_page none, sharp;
    const char *h = P("10px", "8.5px", "");
    if (!render_html(h, PAGE_W, PAGE_H, EMB_TEXT_HINTING_NONE, false, &none)) {
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(render_html(h, PAGE_W, PAGE_H, EMB_TEXT_HINTING_SHARP, false, &sharp));
    EMB_CHECK(ink(&none) > 0);
    EMB_CHECK(partial_pixels(&none) > partial_pixels(&sharp));
    EMB_CHECK(partial_pixels(&sharp) == 0);
    crisp_free(&none);
    crisp_free(&sharp);
}

EMB_TEST_MAIN()
