/*
 * Text specimen sheets and crispness numbers.
 *
 * Draws, through the frame API, a sheet of rows of one sentence at 8 to 32 px in NotoSans and SourceSans3 (dark text on
 * white and light text on dark, gray antialiasing, grid fit on) and a second sheet with LCD RGB text on opaque white.
 * When EMBER_CRISP_OUT names a directory, the sheets are written there as specimen_gray.bmp and specimen_lcd.bmp.
 *
 * It also prints crispness numbers per font and size, measured on single glyphs drawn dark on white in gray mode at four
 * horizontal sub-pixel phases:
 *   edge_rows  the fraction of the horizontal edges of H, x, e and z (a change of the mean coverage of the glyph's
 *              columns of more than 0.5 between rows, possibly spread over several rows) that happen within one row,
 *              without a half-covered row. Higher is crisper.
 *   stem       the peak column coverage of a vertical stem of l and H (1.0 is a fully black column), mean over the
 *              glyphs and phases.
 *   bar        the peak row coverage of the crossbar of e and of the bars of z.
 * The numbers are the tuning gate for later work on the text pipeline; the only assertion is that ink was drawn.
 */
#include "text_fixture.h"

#include "emb_gold.h"
#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>

#define SHEET_W 1000
#define SENTENCE "The quick brown fox jumps over the lazy dog 0123456789"

static const float k_sizes[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 28, 32};
#define N_SIZES ((uint32_t)(sizeof k_sizes / sizeof k_sizes[0]))

static const struct {
    const char *file, *family, *label;
} k_fonts[] = {
    {"tests/fonts/NotoSans-Regular.ttf", "Noto Test", "NotoSans"},
    {"tests/fonts/SourceSans3-Regular.otf", "Source Test", "SourceSans3"},
};
#define N_FONTS 2u

static bool load_fonts(embi_text *t)
{
    for (uint32_t i = 0; i < N_FONTS; i++) {
        embi_face_desc d = {0};
        d.family = tx_s(k_fonts[i].family);
        if (tx_add_file(t, k_fonts[i].file, &d) != 1) {
            return false;
        }
    }
    return true;
}

/* Writes RGBA8 pixels as a BMP. */
static bool save_bmp(const char *path, const uint8_t *px, int w, int h)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void *)px, w * 4);
    if (s == nullptr) {
        return false;
    }
    const bool ok = SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    return ok;
}

static void dump(const char *name, const uint8_t *px, int w, int h)
{
    const char *dir = SDL_getenv("EMBER_CRISP_OUT");
    if (dir == nullptr || dir[0] == '\0' || px == nullptr) {
        return;
    }
    char path[512];
    SDL_CreateDirectory(dir);
    SDL_snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    if (!save_bmp(path, px, w, h)) {
        SDL_Log("cannot write %s: %s", path, SDL_GetError());
    }
}

/* ------------------------------------------------------------------ the sheets */

static int row_height(float size)
{
    return (int)SDL_ceilf(size * 1.35f) + 4;
}

static int row_baseline(float size)
{
    return (int)SDL_ceilf(size * 1.05f) + 2;
}

typedef struct sheet {
    embi_text *t;
    bool lcd;
    int height;
} sheet;

static int block_height(void)
{
    int h = 0;
    for (uint32_t k = 0; k < N_SIZES; k++) {
        h += row_height(k_sizes[k]);
    }
    return h * (int)N_FONTS;
}

/* Draws str with the family at size, baseline y (whole pixel), x = 8. */
static void draw_line(embi_text *t, embi_frame *f, const char *family, float size, const char *str, float x, float y,
                      uint32_t color)
{
    embi_str fam = tx_s(family);
    embi_text_style st;
    tx_style(&st, &fam, 1, size);
    embi_text_layout lay;
    embi_text_vruns vr = {0};
    if (!tx_shape(t, str, &st, &lay)) {
        return;
    }
    if (embi_text_line(&lay, 0, lay.text_len, &vr)) {
        embi_text_draw(t, f, &lay, &vr, x, y, color, 0, 0);
        embi_text_vruns_free(&vr);
    }
    embi_text_layout_free(&lay);
}

static void draw_block(embi_text *t, embi_frame *f, int top, uint32_t bg, uint32_t fg)
{
    const float square[4] = {0, 0, 0, 0};
    const float area[4] = {0, (float)top, (float)SHEET_W, (float)block_height()};
    embi_frame_fill(f, area, square, bg, 0, 0);
    int y = top;
    for (uint32_t i = 0; i < N_FONTS; i++) {
        for (uint32_t k = 0; k < N_SIZES; k++) {
            draw_line(t, f, k_fonts[i].family, k_sizes[k], SENTENCE, 8.0f, (float)(y + row_baseline(k_sizes[k])), fg);
            y += row_height(k_sizes[k]);
        }
    }
}

static void sheet_scene(embi_frame *f, void *user)
{
    sheet *s = user;
    if (s->lcd) {
        embi_frame_set_text_aa(f, EMBI_TEXT_AA_LCD_RGB, false);
        embi_frame_set_opaque_backdrop(f, true);
        draw_block(s->t, f, 0, RGBA(255, 255, 255, 255), RGBA(29, 29, 29, 255));
    } else {
        embi_frame_set_text_aa(f, EMBI_TEXT_AA_GRAY, false);
        draw_block(s->t, f, 0, RGBA(255, 255, 255, 255), RGBA(29, 29, 29, 255));
        draw_block(s->t, f, block_height(), RGBA(30, 30, 30, 255), RGBA(232, 232, 232, 255));
    }
}

/* Number of pixels that differ from the first pixel of the area (the backdrop). */
static uint32_t ink_pixels(const uint8_t *px, int w, int h)
{
    uint32_t n = 0;
    for (int i = 1; i < w * h; i++) {
        n += SDL_memcmp(px + (size_t)i * 4u, px, 4) != 0 ? 1u : 0u;
    }
    return n;
}

EMB_TEST(specimen_sheets)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    if (!load_fonts(t)) {
        embi_text_destroy(t);
        EMB_REQUIRE(false);
    }
    sheet s = {.t = t, .lcd = false};
    const int hg = block_height() * 2;
    uint8_t *gray = emb_test_render(SHEET_W, (uint32_t)hg, sheet_scene, &s);
    if (gray == nullptr) {
        embi_text_destroy(t);
        EMB_RENDER_OR_SKIP(gray);
        return;
    }
    s.lcd = true;
    const int hl = block_height();
    uint8_t *lcd = emb_test_render(SHEET_W, (uint32_t)hl, sheet_scene, &s);
    EMB_REQUIRE(lcd != nullptr);
    const uint32_t ig = ink_pixels(gray, SHEET_W, hg), il = ink_pixels(lcd, SHEET_W, hl);
    SDL_Log("specimen sheets: gray %dx%d ink %u, lcd %dx%d ink %u", SHEET_W, hg, ig, SHEET_W, hl, il);
    EMB_CHECK(ig > 1000u);
    EMB_CHECK(il > 1000u);
    dump("specimen_gray", gray, SHEET_W, hg);
    dump("specimen_lcd", lcd, SHEET_W, hl);
    SDL_free(gray);
    SDL_free(lcd);
    embi_text_destroy(t);
}

/* ------------------------------------------------------------------ crispness numbers */

#define CELL_W 40
#define CELL_H 48
#define CELL_BASE 40
#define N_GLYPHS 5u
#define N_PHASES 4u
static const char k_glyphs[N_GLYPHS] = {'H', 'x', 'e', 'z', 'l'};
static const float k_phases[N_PHASES] = {0.0f, 0.25f, 0.5f, 0.75f};
#define GRID_W ((int)(N_GLYPHS * N_PHASES) * CELL_W)
#define GRID_H ((int)N_SIZES * CELL_H)

typedef struct grid_scene {
    embi_text *t;
    const char *family;
} grid_scene;

static void grid_cb(embi_frame *f, void *user)
{
    grid_scene *g = user;
    const float square[4] = {0, 0, 0, 0};
    const float page[4] = {0, 0, (float)GRID_W, (float)GRID_H};
    embi_frame_fill(f, page, square, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_GRAY, false);
    for (uint32_t k = 0; k < N_SIZES; k++) {
        for (uint32_t gi = 0; gi < N_GLYPHS; gi++) {
            const char str[2] = {k_glyphs[gi], 0};
            for (uint32_t p = 0; p < N_PHASES; p++) {
                const float x = (float)((gi * N_PHASES + p) * CELL_W) + 6.0f + k_phases[p];
                draw_line(g->t, f, g->family, k_sizes[k], str, x, (float)((int)k * CELL_H + CELL_BASE),
                          RGBA(29, 29, 29, 255));
            }
        }
    }
}

/* Coverage of a dark (29) on white pixel, 0..1. */
static float cov_at(const uint8_t *px, int x, int y)
{
    const float c = (255.0f - (float)px[((size_t)y * GRID_W + (size_t)x) * 4u]) / (255.0f - 29.0f);
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

typedef struct cell {
    const uint8_t *px;
    int x0, y0;
    int cols, rows;      /* the ink box: columns and rows with coverage above 0.05 */
    int cx, ry;          /* its first column and first row (cell relative) */
} cell;

static float cell_cov(const cell *c, int x, int y)
{
    return cov_at(c->px, c->x0 + x, c->y0 + y);
}

static bool cell_find(cell *c, const uint8_t *px, int gx, int row)
{
    c->px = px;
    c->x0 = gx * CELL_W;
    c->y0 = row * CELL_H;
    int minx = CELL_W, maxx = -1, miny = CELL_H, maxy = -1;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            if (cell_cov(c, x, y) > 0.05f) {
                minx = x < minx ? x : minx;
                maxx = x > maxx ? x : maxx;
                miny = y < miny ? y : miny;
                maxy = y > maxy ? y : maxy;
            }
        }
    }
    if (maxx < 0) {
        return false;
    }
    c->cx = minx;
    c->ry = miny;
    c->cols = maxx - minx + 1;
    c->rows = maxy - miny + 1;
    return true;
}

/* Horizontal edges of a glyph: the mean coverage of its columns per row, d = change from the row above. A run of rows
 * whose change has the same sign, each above 0.04, is one edge when its total is above 0.5; it is crisp when one row
 * carries at least 85% of it. */
static void cell_edges(const cell *c, uint32_t *edges, uint32_t *crisp)
{
    float prev = 0.0f;
    float run = 0.0f, big = 0.0f;
    for (int y = c->ry; y <= c->ry + c->rows; y++) {
        float m = 0.0f;
        if (y < CELL_H && y < c->ry + c->rows) {
            for (int x = c->cx; x < c->cx + c->cols; x++) {
                m += cell_cov(c, x, y);
            }
            m /= (float)c->cols;
        }
        const float d = m - prev;
        prev = m;
        const bool same = run != 0.0f && ((d > 0.0f) == (run > 0.0f)) && SDL_fabsf(d) > 0.04f;
        if (same) {
            run += d;
            big = SDL_fabsf(d) > big ? SDL_fabsf(d) : big;
            continue;
        }
        if (SDL_fabsf(run) > 0.5f) {
            (*edges)++;
            *crisp += big >= 0.85f * SDL_fabsf(run) ? 1u : 0u;
        }
        if (SDL_fabsf(d) > 0.04f) {
            run = d;
            big = SDL_fabsf(d);
        } else {
            run = 0.0f;
            big = 0.0f;
        }
    }
}

/* Peak over the columns of the mean coverage of the rows [r0, r1) of the ink box (given in tenths of its height). */
static float cell_stem(const cell *c, int t0, int t1)
{
    const int r0 = c->ry + c->rows * t0 / 10, r1 = c->ry + c->rows * t1 / 10;
    float peak = 0.0f;
    for (int x = c->cx; x < c->cx + c->cols; x++) {
        float m = 0.0f;
        for (int y = r0; y < r1; y++) {
            m += cell_cov(c, x, y);
        }
        m /= (float)(r1 - r0 > 0 ? r1 - r0 : 1);
        peak = m > peak ? m : peak;
    }
    return peak;
}

/* Peak over the rows [t0, t1) tenths of the ink box of the mean coverage across the middle 60% of its columns. */
static float cell_bar(const cell *c, int t0, int t1)
{
    const int r0 = c->ry + c->rows * t0 / 10, r1 = c->ry + c->rows * t1 / 10;
    const int x0 = c->cx + c->cols / 5, x1 = c->cx + c->cols - c->cols / 5;
    float peak = 0.0f;
    for (int y = r0; y < r1; y++) {
        float m = 0.0f;
        for (int x = x0; x < x1; x++) {
            m += cell_cov(c, x, y);
        }
        m /= (float)(x1 - x0 > 0 ? x1 - x0 : 1);
        peak = m > peak ? m : peak;
    }
    return peak;
}

EMB_TEST(specimen_crispness)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    if (!load_fonts(t)) {
        embi_text_destroy(t);
        EMB_REQUIRE(false);
    }
    for (uint32_t i = 0; i < N_FONTS; i++) {
        grid_scene g = {t, k_fonts[i].family};
        uint8_t *px = emb_test_render(GRID_W, GRID_H, grid_cb, &g);
        if (px == nullptr) {
            embi_text_destroy(t);
            EMB_RENDER_OR_SKIP(px);
            return;
        }
        for (uint32_t k = 0; k < N_SIZES; k++) {
            uint32_t edges = 0, crisp = 0, nstem = 0, nbar = 0, nink = 0;
            float stem = 0.0f, bar = 0.0f;
            for (uint32_t gi = 0; gi < N_GLYPHS; gi++) {
                for (uint32_t p = 0; p < N_PHASES; p++) {
                    cell c;
                    if (!cell_find(&c, px, (int)(gi * N_PHASES + p), (int)k)) {
                        continue;
                    }
                    nink++;
                    const char ch = k_glyphs[gi];
                    if (ch != 'l') {
                        cell_edges(&c, &edges, &crisp);
                    }
                    if (ch == 'l' || ch == 'H') {
                        /* The upper part of the ink box holds only the stems (the crossbar of H is lower). */
                        stem += cell_stem(&c, 1, 4);
                        nstem++;
                    }
                    if (ch == 'e') {
                        bar += cell_bar(&c, 3, 7);
                        nbar++;
                    } else if (ch == 'z') {
                        bar += 0.5f * (cell_bar(&c, 0, 3) + cell_bar(&c, 7, 10));
                        nbar++;
                    }
                }
            }
            EMB_CHECK(nink > 0u);
            SDL_Log("specimen %s %.0fpx edge_rows=%.3f stem=%.3f bar=%.3f (edges %u)", k_fonts[i].label,
                    (double)k_sizes[k], edges > 0 ? (double)crisp / (double)edges : 0.0,
                    nstem > 0 ? (double)(stem / (float)nstem) : 0.0, nbar > 0 ? (double)(bar / (float)nbar) : 0.0,
                    edges);
        }
        SDL_free(px);
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
