/*
 * LCD antialiasing, its fallbacks and the bounds of the grid fit (W4h).
 *   - LCD text over an opaque backdrop has color fringes (R and B differ), and RGB and BGR mirror them.
 *   - Every fallback draws gray: a frame whose backdrop is not opaque, an item under opacity < 1, a
 *     rotated or skewed transform, and an animated transform (while the device-scale transform stays LCD).
 *     (Text in an opacity group through a view: tests/unit/embed/test_embed_crisp.c.)
 *   - The grid fit moves the ink by at most 1 px against the unfitted outline (the same text under a
 *     transform with a tiny skew, which turns the fit off): top and bottom rows and the vertical
 *     centroid, at 13 to 24 px on a fractional baseline.
 */
#include "text_fixture.h"

#include "emb_gold.h"
#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#define SW 200
#define SH 120

typedef struct scene {
    embi_text *t;
    embi_text_layout lay;
    embi_text_vruns vr;
    embi_text_aa aa;
    bool animating;
    bool opaque;
    bool lcd_allowed;
    float xf[6]; /* the transform of the text, used when use_xf is set */
    bool use_xf;
    float y;
    uint32_t bg, fg;
} scene;

static bool scene_open(scene *s, const char *text, float size)
{
    SDL_memset(s, 0, sizeof *s);
    s->t = embi_text_create();
    if (s->t == nullptr) {
        return false;
    }
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    if (tx_add_file(s->t, "tests/fonts/NotoSans-Regular.ttf", &d) != 1) {
        return false;
    }
    static embi_str fam;
    fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, size);
    if (!tx_shape(s->t, text, &st, &s->lay) || !embi_text_line(&s->lay, 0, s->lay.text_len, &s->vr)) {
        return false;
    }
    s->aa = EMBI_TEXT_AA_LCD_RGB;
    s->opaque = true;
    s->lcd_allowed = true;
    s->y = 40.0f;
    s->bg = RGBA(255, 255, 255, 255);
    s->fg = RGBA(29, 29, 29, 255);
    return true;
}

static void scene_close(scene *s)
{
    embi_text_vruns_free(&s->vr);
    embi_text_layout_free(&s->lay);
    embi_text_destroy(s->t);
}

static void draw_scene(embi_frame *f, void *user)
{
    scene *s = user;
    const float square[4] = {0, 0, 0, 0};
    const float page[4] = {0, 0, (float)SW, (float)SH};
    embi_frame_fill(f, page, square, s->bg, 0, 0);
    embi_frame_set_text_aa(f, s->aa, s->animating);
    embi_frame_set_opaque_backdrop(f, s->opaque);
    embi_frame_set_lcd_allowed(f, s->lcd_allowed);
    uint32_t xf = s->use_xf ? embi_frame_xform(f, s->xf) : 0;
    embi_text_draw(s->t, f, &s->lay, &s->vr, 10.0f, s->y, s->fg, xf, 0);
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

static int render_fringes(scene *s)
{
    uint8_t *px = emb_test_render(SW, SH, draw_scene, s);
    if (px == nullptr) {
        return -1;
    }
    int n = fringe_pixels(px, SW, SH);
    SDL_free(px);
    return n;
}

EMB_TEST(lcd_fringes_and_fallbacks)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    scene s;
    EMB_REQUIRE(scene_open(&s, "Hello lllxxx 123", 15.0f));

    int ok = render_fringes(&s);
    if (ok < 0) {
        scene_close(&s);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(ok > 40);

    /* Gray mode, a backdrop that is not opaque, and an item under opacity < 1. */
    s.aa = EMBI_TEXT_AA_GRAY;
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);
    s.aa = EMBI_TEXT_AA_LCD_RGB;
    s.opaque = false;
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);
    s.opaque = true;
    s.lcd_allowed = false;
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);
    s.lcd_allowed = true;

    /* Rotation and skew draw gray. */
    s.use_xf = true;
    const float rot[6] = {0.9994f, 0.0349f, -0.0349f, 0.9994f, 0.0f, 0.0f};
    SDL_memcpy(s.xf, rot, sizeof rot);
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);
    const float skew[6] = {1.0f, 0.0f, 0.2f, 1.0f, 0.0f, 0.0f};
    SDL_memcpy(s.xf, skew, sizeof skew);
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);

    /* A plain scale (not animated) stays LCD, an animated one does not, and the device-scale
     * transform stays LCD while a transform animation runs elsewhere. */
    const float scl[6] = {1.05f, 0.0f, 0.0f, 1.05f, 0.0f, 0.0f};
    SDL_memcpy(s.xf, scl, sizeof scl);
    EMB_CHECK(render_fringes(&s) > 40);
    s.animating = true;
    EMB_CHECK_EQ_INT(render_fringes(&s), 0);
    s.use_xf = false;
    EMB_CHECK(render_fringes(&s) > 40);
    s.animating = false;
    scene_close(&s);
}


/* Total ink against the backdrop: the sum over all pixels and channels of |pixel - backdrop|. */
static double ink_sum(const uint8_t *px, int w, int h, const int bg[3])
{
    double sum = 0.0;
    for (int i = 0; i < w * h; i++) {
        for (int c = 0; c < 3; c++) {
            sum += SDL_abs((int)px[i * 4 + c] - bg[c]);
        }
    }
    return sum;
}

/* Colored text over a mid-gray backdrop in LCD mode blends correctly: every channel stays between the backdrop and the
 * text color (no overshoot), the edges have fringes, and the total ink stays within 10% of the gray render of the same
 * text (the filter taps sum to one, so there is no brightness shift). */
EMB_TEST(lcd_colored_text_on_gray_backdrop)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    scene s;
    EMB_REQUIRE(scene_open(&s, "llll Hxg 123 llll", 15.0f));
    s.bg = RGBA(128, 128, 128, 255);
    s.fg = RGBA(200, 40, 30, 255);
    const int bg[3] = {128, 128, 128}, fg[3] = {200, 40, 30};
    s.aa = EMBI_TEXT_AA_GRAY;
    uint8_t *gray = emb_test_render(SW, SH, draw_scene, &s);
    EMB_RENDER_OR_SKIP(gray);
    s.aa = EMBI_TEXT_AA_LCD_RGB;
    uint8_t *lcd = emb_test_render(SW, SH, draw_scene, &s);
    EMB_REQUIRE(lcd != nullptr);
    int over = 0;
    for (int i = 0; i < SW * SH; i++) {
        for (int c = 0; c < 3; c++) {
            int lo = bg[c] < fg[c] ? bg[c] : fg[c], hi = bg[c] < fg[c] ? fg[c] : bg[c];
            int v = lcd[i * 4 + c];
            over += (v < lo - 3 || v > hi + 3) ? 1 : 0;
        }
    }
    const double ig = ink_sum(gray, SW, SH, bg), il = ink_sum(lcd, SW, SH, bg);
    SDL_Log("colored text on gray: ink gray %.0f, lcd %.0f (ratio %.3f), overshoots %d", ig, il, il / ig, over);
    EMB_CHECK_EQ_INT(over, 0);
    EMB_CHECK(ig > 1000.0);
    EMB_CHECK(il > 0.9 * ig && il < 1.1 * ig);
    /* Fringes: the green-blue split of the edges (the text is red, so R barely moves and G and B carry the filter). */
    int split = 0;
    for (int i = 0; i < SW * SH; i++) {
        int d = (int)lcd[i * 4 + 1] - (int)lcd[i * 4 + 2];
        split += (d > 12 || d < -12) ? 1 : 0;
    }
    EMB_CHECK(split > 20);
    SDL_free(gray);
    SDL_free(lcd);
    scene_close(&s);
}

/* The sign of R - B at the left edge of the first dark pixel of a row: RGB and BGR mirror. */
static int left_edge_sign(const uint8_t *px, int w, int y)
{
    for (int x = 0; x < w; x++) {
        const uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
        if (p[0] < 250 || p[1] < 250 || p[2] < 250) {
            return (int)p[0] - (int)p[2];
        }
    }
    return 0;
}

EMB_TEST(lcd_bgr_mirrors_rgb)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    scene s;
    EMB_REQUIRE(scene_open(&s, "llllll", 15.0f));
    uint8_t *rgb = emb_test_render(SW, SH, draw_scene, &s);
    EMB_RENDER_OR_SKIP(rgb);
    s.aa = EMBI_TEXT_AA_LCD_BGR;
    uint8_t *bgr = emb_test_render(SW, SH, draw_scene, &s);
    EMB_REQUIRE(bgr != nullptr);
    int mid = 33; /* a row through the stems */
    int a = left_edge_sign(rgb, SW, mid), b = left_edge_sign(bgr, SW, mid);
    EMB_CHECK(a != 0 && b != 0);
    EMB_CHECK((a > 0) != (b > 0));
    SDL_free(rgb);
    SDL_free(bgr);
    scene_close(&s);
}

/* Row profile of a region: the first and last row with ink and the ink-weighted vertical centroid. */
typedef struct prof {
    int top, bottom;
    double centroid;
} prof;

static prof profile(const uint8_t *px, int w)
{
    prof p = {-1, -1, 0.0};
    double sum = 0.0, wsum = 0.0;
    double rows[SH] = {0};
    double peak = 0.0;
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *q = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            rows[y] += (255.0 - (0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2])) / 226.0;
        }
        peak = rows[y] > peak ? rows[y] : peak;
    }
    for (int y = 0; y < SH; y++) {
        if (rows[y] > 0.04 * peak) {
            p.top = p.top < 0 ? y : p.top;
            p.bottom = y;
        }
        sum += rows[y] * ((double)y + 0.5);
        wsum += rows[y];
    }
    p.centroid = wsum > 0.0 ? sum / wsum : 0.0;
    return p;
}

EMB_TEST(fit_moves_ink_by_at_most_one_pixel)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    static const float sizes[] = {13.0f, 14.0f, 15.0f, 16.0f, 18.0f, 24.0f};
    for (uint32_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        scene s;
        EMB_REQUIRE(scene_open(&s, "Hxgdpl", sizes[i]));
        s.aa = EMBI_TEXT_AA_GRAY;
        s.y = 60.37f;
        uint8_t *fit = emb_test_render(SW, SH, draw_scene, &s);
        EMB_RENDER_OR_SKIP(fit);
        /* A skew of 0.001 (a hundredth of a pixel over a glyph) turns the fit off (and the baseline snap), leaving the plain outline. */
        s.use_xf = true;
        const float sk[6] = {1.0f, 0.0f, 1e-3f, 1.0f, 0.0f, 0.0f};
        SDL_memcpy(s.xf, sk, sizeof sk);
        uint8_t *raw = emb_test_render(SW, SH, draw_scene, &s);
        EMB_REQUIRE(raw != nullptr);
        prof a = profile(fit, SW), b = profile(raw, SW);
        SDL_Log("size %.0f: fitted rows %d..%d centroid %.2f, unfitted %d..%d centroid %.2f", sizes[i], a.top,
                a.bottom, a.centroid, b.top, b.bottom, b.centroid);
        EMB_CHECK(a.top >= 0 && b.top >= 0);
        EMB_CHECK(SDL_abs(a.top - b.top) <= 1);
        EMB_CHECK(SDL_abs(a.bottom - b.bottom) <= 1);
        EMB_CHECK(SDL_fabs(a.centroid - b.centroid) <= 0.75);
        SDL_free(fit);
        SDL_free(raw);
        scene_close(&s);
    }
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
