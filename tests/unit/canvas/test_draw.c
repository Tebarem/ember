#include "../gpu/layer_rig.h"
#include "../text/text_fixture.h"

#include <math.h>
#include <string.h>

#include "canvas/canvas.h"
#include "image/png.h"

#include <SDL3/SDL_timer.h>

/* The canvas on the GPU: recorded commands replayed into the backing texture and read back. Tolerances are +-2 for flat
 * colors and wider for gradients and blurs. The tests that end in an embi_mem_live comparison check that the canvases and
 * the manager give all their memory back. */

#define CV_PI 3.14159265358979323846
#define TOL 2

typedef struct rig {
    layer_rig k;
    embi_canvas_gpu *cg;
    embi_text *text;
} rig;

static int g_keys[64];
static int g_nkey;

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static bool rig_open(rig *r, bool with_text)
{
    memset(r, 0, sizeof *r);
    if (!layer_rig_open(&r->k)) {
        return false;
    }
    r->cg = embi_canvas_gpu_create(r->k.g, r->k.r, r->k.pool, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if (r->cg == nullptr) {
        layer_rig_close(&r->k);
        return false;
    }
    if (with_text) {
        r->text = embi_text_create();
        embi_face_desc desc = {0};
        desc.family = (embi_str){"Noto Test", 9};
        if (tx_add_file(r->text, "tests/fonts/NotoSans-Regular.ttf", &desc) == 0) {
            SDL_Log("font fixture missing");
        }
    }
    return true;
}

static void rig_close(rig *r)
{
    embi_canvas_gpu_destroy(r->cg);
    if (r->text != nullptr) {
        embi_text_destroy(r->text);
    }
    layer_rig_close(&r->k);
}

static embi_canvas *cv_new(rig *r, uint32_t w, uint32_t h, bool alpha)
{
    embi_canvas_env env;
    memset(&env, 0, sizeof env);
    env.gpu = r->cg;
    env.text = r->text;
    return embi_canvas_create(&env, &g_keys[g_nkey++ % 64], w, h, alpha);
}

static uint8_t *snap(embi_canvas *cv)
{
    uint32_t w = embi_canvas_width(cv), h = embi_canvas_height(cv);
    uint8_t *px = SDL_malloc((size_t)w * h * 4);
    if (px == nullptr || !embi_canvas_read(cv, 0, 0, w, h, px)) {
        emb_test_report_fail(__FILE__, __LINE__, "canvas read failed");
        SDL_memset(px, 0, (size_t)w * h * 4);
    }
    return px;
}

static void fill(embi_canvas *cv, const char *css, double x, double y, double w, double h)
{
    embi_canvas_set_color(cv, 0, S(css));
    embi_canvas_fill_rect(cv, x, y, w, h);
}

#define PX(px, w, x, y, r, g, b, a, tol) CHECK_PX_NEAR(px, w, x, y, r, g, b, a, tol)

/* ------------------------------------------------------------------ fills */

EMB_TEST(draw_fill_rect)
{
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 48, true);
    fill(cv, "#ff0000", 10, 10, 20, 20);
    uint8_t *px = snap(cv);
    PX(px, 64, 15, 15, 255, 0, 0, 255, TOL);
    PX(px, 64, 5, 5, 0, 0, 0, 0, 0);
    PX(px, 64, 35, 15, 0, 0, 0, 0, 0);
    PX(px, 64, 29, 29, 255, 0, 0, 255, TOL);
    PX(px, 64, 30, 30, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(draw_opaque_canvas_starts_black)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 16, 16, false);
    uint8_t *px = snap(cv);
    PX(px, 16, 8, 8, 0, 0, 0, 255, 0);
    SDL_free(px);
    fill(cv, "#00ff00", 0, 0, 8, 16);
    embi_canvas_clear_rect(cv, 0, 0, 4, 16);
    px = snap(cv);
    PX(px, 16, 2, 8, 0, 0, 0, 255, 0); /* clearRect on an opaque canvas gives opaque black */
    PX(px, 16, 6, 8, 0, 255, 0, 255, TOL);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_global_alpha_and_translucent_colors)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 16, 16, true);
    fill(cv, "#ffffff", 0, 0, 16, 16);
    embi_canvas_set_alpha(cv, 0.5);
    fill(cv, "#000000", 0, 0, 8, 16);
    embi_canvas_set_alpha(cv, 1.0);
    fill(cv, "rgba(0, 0, 0, 0.5)", 8, 0, 8, 16);
    uint8_t *px = snap(cv);
    PX(px, 16, 3, 8, 128, 128, 128, 255, 2);
    PX(px, 16, 12, 8, 128, 128, 128, 255, 2);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_clear_rect_erases)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 32, 32, true);
    fill(cv, "#0000ff", 0, 0, 32, 32);
    embi_canvas_clear_rect(cv, 8, 8, 16, 16);
    uint8_t *px = snap(cv);
    PX(px, 32, 4, 4, 0, 0, 255, 255, TOL);
    PX(px, 32, 16, 16, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_path_fill_rules)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 48, 48, true);
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_rect(p, nullptr, 0, 0, 40, 40);
    embi_cpath_rect(p, nullptr, 10, 10, 20, 20);
    embi_canvas_set_color(cv, 0, S("#00ff00"));
    embi_canvas_fill(cv, nullptr, true);
    uint8_t *px = snap(cv);
    PX(px, 48, 5, 5, 0, 255, 0, 255, TOL);
    PX(px, 48, 20, 20, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_clear_rect(cv, 0, 0, 48, 48);
    embi_canvas_fill(cv, nullptr, false);
    px = snap(cv);
    PX(px, 48, 20, 20, 0, 255, 0, 255, TOL);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_transform_applies_to_fills)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    embi_canvas_translate(cv, 20, 10);
    embi_canvas_scale(cv, 2, 2);
    fill(cv, "#ff00ff", 0, 0, 10, 10);
    uint8_t *px = snap(cv);
    PX(px, 64, 30, 20, 255, 0, 255, 255, TOL);
    PX(px, 64, 39, 29, 255, 0, 255, 255, TOL);
    PX(px, 64, 41, 20, 0, 0, 0, 0, 0);
    PX(px, 64, 15, 15, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* ------------------------------------------------------------------ strokes */

EMB_TEST(draw_stroke_line_widths_and_caps)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 32, true);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 8);
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, nullptr, 10, 16);
    embi_cpath_line_to(p, nullptr, 50, 16);
    embi_canvas_stroke(cv, nullptr);
    uint8_t *px = snap(cv);
    PX(px, 64, 30, 16, 255, 0, 0, 255, TOL);
    PX(px, 64, 30, 12, 255, 0, 0, 255, TOL);
    PX(px, 64, 30, 19, 255, 0, 0, 255, TOL);
    PX(px, 64, 30, 10, 0, 0, 0, 0, 0);
    PX(px, 64, 30, 22, 0, 0, 0, 0, 0);
    PX(px, 64, 8, 16, 0, 0, 0, 0, 0); /* butt cap */
    SDL_free(px);
    embi_canvas_clear_rect(cv, 0, 0, 64, 32);
    embi_canvas_set_line_cap(cv, S("square"));
    embi_canvas_stroke(cv, nullptr);
    px = snap(cv);
    PX(px, 64, 8, 16, 255, 0, 0, 255, TOL); /* square cap reaches 4 px beyond the end */
    PX(px, 64, 5, 16, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A zero-length open or closed subpath draws nothing (WPT 2d.path.stroke.prune), a zero-height strokeRect is a closed
 * degenerate contour: caps do not apply to it, round joins do. */
EMB_TEST(draw_stroke_prunes_zero_length_subpaths)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 100, 50, true);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 100);
    embi_canvas_set_line_cap(cv, S("round"));
    embi_canvas_set_line_join(cv, S("round"));
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, nullptr, 50, 25);
    embi_cpath_line_to(p, nullptr, 50, 25);
    embi_canvas_stroke(cv, nullptr);
    embi_cpath_reset(p);
    embi_cpath_move_to(p, nullptr, 50, 25);
    embi_cpath_line_to(p, nullptr, 50, 25);
    embi_cpath_close(p);
    embi_canvas_stroke(cv, nullptr);
    embi_cpath_reset(p);
    embi_cpath_rect(p, nullptr, 50, 25, 0, 0);
    embi_canvas_stroke(cv, nullptr);
    embi_canvas_stroke_rect(cv, 50, 25, 0, 0);
    uint8_t *px = snap(cv);
    PX(px, 100, 50, 25, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_stroke_rect_with_zero_height)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 100, 50, true);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 250);
    embi_canvas_set_line_cap(cv, S("round"));
    embi_canvas_stroke_rect(cv, 100, 25, 100, 0);
    uint8_t *px = snap(cv);
    PX(px, 100, 50, 25, 0, 0, 0, 0, 0); /* a closed contour has no caps and the miter joins of a reversal are cut */
    SDL_free(px);
    embi_canvas_clear_rect(cv, 0, 0, 100, 50);
    embi_canvas_set_line_cap(cv, S("butt"));
    embi_canvas_set_line_join(cv, S("round"));
    embi_canvas_stroke_rect(cv, 100, 25, 100, 0);
    px = snap(cv);
    PX(px, 100, 50, 25, 255, 0, 0, 255, TOL); /* the round join at each end is a disc */
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_stroke_dash)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 16, true);
    embi_canvas_set_color(cv, 1, S("#000000"));
    embi_canvas_set_line_width(cv, 4);
    const double d[2] = {8, 8};
    EMB_CHECK(embi_canvas_set_line_dash(cv, d, 2));
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, nullptr, 0, 8);
    embi_cpath_line_to(p, nullptr, 64, 8);
    embi_canvas_stroke(cv, nullptr);
    uint8_t *px = snap(cv);
    PX(px, 64, 4, 8, 0, 0, 0, 255, TOL);
    PX(px, 64, 12, 8, 0, 0, 0, 0, 0);
    PX(px, 64, 20, 8, 0, 0, 0, 255, TOL);
    PX(px, 64, 28, 8, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_set_dash_offset(cv, 8);
    embi_canvas_clear_rect(cv, 0, 0, 64, 16);
    embi_canvas_stroke(cv, nullptr);
    px = snap(cv);
    PX(px, 64, 4, 8, 0, 0, 0, 0, 0);
    PX(px, 64, 12, 8, 0, 0, 0, 255, TOL);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A horizontal cubic whose control points lie on the chord, some past its ends (a node editor's wire between two
 * sockets at one height): the curve goes forward, back and forward along one line, and its stroke has round-join discs at
 * the turns, with vertices on the rays the coverage shader casts through a row of pixel centers. The stroke is the union
 * of its parts, so the line is solid: the pixel under every column of the central rows has the full color, and nothing is
 * drawn before the start. */
static const char *g_case = "";

/* Every column of [x0, x1) is the color in the rows ya..yb (the line's core) and empty in the rows ye0..ye1; the pixel
 * (xe, yrow) before the start is empty. */
static void check_solid_run(const uint8_t *px, uint32_t w, int x0, int x1, int ya, int yb, int ye0, int ye1, int xe,
                            int yrow)
{
    for (int x = x0; x < x1; x++) {
        for (int y = ya; y <= yb; y++) {
            const uint8_t *q = px + ((size_t)y * w + (uint32_t)x) * 4;
            if (q[0] < 253 || q[1] > 2 || q[2] > 2 || q[3] < 253) {
                SDL_Log("hole at %d,%d: %d %d %d %d (%s)", x, y, q[0], q[1], q[2], q[3], g_case);
                EMB_CHECK(false);
                return;
            }
        }
        for (int y = ye0; y <= ye1; y++) {
            const uint8_t *q = px + ((size_t)y * w + (uint32_t)x) * 4;
            if (q[3] > 2) {
                SDL_Log("stray at %d,%d: alpha %d (%s)", x, y, q[3], g_case);
                EMB_CHECK(false);
                return;
            }
        }
    }
    if (px[((size_t)yrow * w + (uint32_t)xe) * 4 + 3] > 2) {
        SDL_Log("stray before the start at %d,%d (%s)", xe, yrow, g_case);
        EMB_CHECK(false);
    }
}

/* The antialiased rows along the line's two edges have the same coverage in every column of [x0, x1): the stroke is one
 * bar, not pieces stacked on each other whose edge coverages add up (a doubled edge row would be about twice the
 * coverage, a full row). */
static void check_edge_rows(const uint8_t *px, uint32_t w, int x0, int x1, double dy, double zoom, const char *what)
{
    const double top = dy - zoom, bot = dy + zoom;
    const int rt = (int)floor(top), rb = (int)floor(bot);
    const double ct = (rt + 1) - top, cb = bot - rb;
    for (int x = x0; x < x1; x++) {
        const int at = px[((size_t)rt * w + (uint32_t)x) * 4 + 3];
        const int ab = px[((size_t)rb * w + (uint32_t)x) * 4 + 3];
        if (fabs(at - ct * 255.0) > 24.0 || fabs(ab - cb * 255.0) > 24.0) {
            SDL_Log("%s: edge rows at x %d: %d (want %d), %d (want %d) (%s)", what, x, at, (int)(ct * 255.0), ab,
                    (int)(cb * 255.0), g_case);
            EMB_CHECK(false);
            return;
        }
    }
}

/* A wire from (ax, y) to (bx, y) in user space, drawn under translate(120, 20) scale(zoom) translate(-120, -20 - oy). */
static void wire_case(rig *r, const char *join, bool dash_then_clear, double ax, double bx, double y, double dx,
                      double zoom, bool line_to, double oy)
{
    static char name[128];
    SDL_snprintf(name, sizeof name, "%s dash %d from %g to %g at y %g dx %g zoom %g line_to %d oy %g", join,
                 dash_then_clear, ax, bx, y, dx, zoom, line_to, oy);
    g_case = name;
    embi_canvas *cv = cv_new(r, 240, 40, true);
    embi_canvas_translate(cv, 120, 20);
    embi_canvas_scale(cv, zoom, zoom);
    embi_canvas_translate(cv, -120, -20 - oy);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 2);
    embi_canvas_set_line_join(cv, S(join));
    if (dash_then_clear) {
        const double d[2] = {5, 4};
        EMB_CHECK(embi_canvas_set_line_dash(cv, d, 2));
        EMB_CHECK(embi_canvas_set_line_dash(cv, nullptr, 0)); /* setLineDash([]): solid again */
        const double *v = nullptr;
        EMB_CHECK_EQ_INT((int)embi_canvas_get_line_dash(cv, &v), 0);
    }
    double m[6];
    embi_canvas_get_transform(cv, m);
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, m, ax, y);
    if (line_to) {
        embi_cpath_line_to(p, m, bx, y);
    } else {
        embi_cpath_cubic_to(p, m, ax + dx, y, bx - dx, y, bx, y);
    }
    embi_canvas_stroke(cv, nullptr);
    uint8_t *px = snap(cv);
    /* The line is 2 * zoom px thick around the device row of y: the rows wholly inside it are solid. */
    double dy = 20 + (y - oy - 20) * zoom;
    double dax = 120 + (ax - 120) * zoom, dbx = 120 + (bx - 120) * zoom;
    int ya = (int)ceil(dy - zoom + 0.05), yb = (int)floor(dy + zoom - 0.05) - 1;
    check_solid_run(px, 240, (int)ceil(dax) + 1, (int)floor(dbx) - 1, ya, yb, (int)ceil(dy + zoom) + 1,
                    (int)ceil(dy + zoom) + 3, (int)floor(dax) - 1, (int)floor(dy));
    if (bx - ax >= 60) {
        check_edge_rows(px, 240, (int)ceil(dax) + 4, (int)floor(dbx) - 4, dy, zoom, "wire");
    }
    SDL_free(px);
    embi_canvas_destroy(cv);
}

EMB_TEST(draw_stroke_collinear_cubic_is_solid)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    static const char *const joins[3] = {"miter", "round", "bevel"};
    for (int j = 0; j < 3; j++) {
        /* controls past the ends: x = 140 and 126 for the ends 100 and 166; y on a pixel center or on a pixel edge */
        wire_case(&r, joins[j], false, 100, 166, 17.5, 40, 1, false, 0);
        wire_case(&r, joins[j], false, 100, 166, 18, 40, 1, false, 0);
        wire_case(&r, joins[j], false, 100, 166, 17.5, 40, 1.3, false, 0);
        wire_case(&r, joins[j], false, 100, 166, 18, 40, 1.3, false, 0);
        /* far from the origin, as a camera moved over a big graph does it: the same line at y 447.5 and 448 */
        wire_case(&r, joins[j], false, 100, 166, 447.5, 40, 1, false, 430);
        wire_case(&r, joins[j], false, 100, 166, 448, 40, 1, false, 430);
        /* short wires, the turns close together; a vertex of the joins on the ray exactly (user y 449.5) */
        wire_case(&r, joins[j], false, 50.5, 69.5, 449.5, 40, 1, false, 430);
        wire_case(&r, joins[j], false, 50.5, 69.5, 449.786, 40, 1.0001, false, 430);
        /* collinear without overshoot */
        wire_case(&r, joins[j], false, 100, 200, 17.5, 20, 1, false, 0);
        wire_case(&r, joins[j], false, 100, 200, 18, 20, 1.3, false, 0);
        /* with a dash array set and cleared again */
        wire_case(&r, joins[j], true, 100, 166, 17.5, 40, 1, false, 0);
    }
    /* a straight line for comparison */
    wire_case(&r, "miter", false, 100, 166, 17.5, 0, 1, true, 0);
    wire_case(&r, "miter", false, 100, 166, 18, 0, 1.3, true, 0);
    rig_close(&r);
}

/* Wires of every length under camera transforms (zoom, fractional translation), on and off the pixel grid: each one is
 * solid along its whole length. */
EMB_TEST(draw_stroke_collinear_cubic_fuzz)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    enum { W = 400, H = 60 };
    embi_canvas *cv = cv_new(&r, W, H, true);
    static const double zooms[4] = {1, 1.3, 2, 1.0001};
    uint32_t seed = 12345;
    int bad = 0;
    for (int it = 0; it < 300 && bad < 5; it++) {
        seed = seed * 1664525u + 1013904223u;
        const double zoom = zooms[(seed >> 8) & 3];
        seed = seed * 1664525u + 1013904223u;
        const double cx = 100 + ((seed >> 8) & 1023) / 8.0, cy = 400 + ((seed >> 4) & 1023) / 7.0;
        seed = seed * 1664525u + 1013904223u;
        const double len = 3 + ((seed >> 8) % 120);
        const double ax = cx - 100 / zoom, bx = ax + len;
        const double y = cy + (it & 1) * 0.5 + (it % 5 - 2);
        const double dx = fmax(40, len * 0.5);
        embi_canvas_set_transform(cv, (double[6]){1, 0, 0, 1, 0, 0});
        embi_canvas_clear_rect(cv, 0, 0, W, H);
        embi_canvas_translate(cv, W / 2.0, H / 2.0);
        embi_canvas_scale(cv, zoom, zoom);
        embi_canvas_translate(cv, -cx, -cy);
        embi_canvas_set_color(cv, 1, S("#ff0000"));
        embi_canvas_set_line_width(cv, 2);
        double m[6];
        embi_canvas_get_transform(cv, m);
        embi_canvas_begin_path(cv);
        embi_cpath *p = embi_canvas_path(cv);
        embi_cpath_move_to(p, m, ax, y);
        embi_cpath_cubic_to(p, m, ax + dx, y, bx - dx, y, bx, y);
        embi_canvas_stroke(cv, nullptr);
        uint8_t *px = snap(cv);
        const double dy = m[3] * y + m[5], da = m[0] * ax + m[4], db = m[0] * bx + m[4];
        const int row = (int)floor(dy);
        if (len >= 40) {
            check_edge_rows(px, W, (int)ceil(da) + 4, (int)floor(db) - 4, dy, zoom, "fuzz");
        }
        for (int x = (int)ceil(da) + 1; x < (int)floor(db) - 1; x++) {
            if (px[((size_t)row * W + (size_t)x) * 4 + 3] < 250) {
                SDL_Log("gap: len %g zoom %g y %g, device x %g..%g y %g, at x %d alpha %d", len, zoom, y, da, db, dy, x,
                        px[((size_t)row * W + (size_t)x) * 4 + 3]);
                bad++;
                break;
            }
        }
        SDL_free(px);
    }
    EMB_CHECK(bad == 0);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A short wire far from the origin: the stroke is in user space at y about 449 and is drawn under a transform that puts
 * it on row 32. Vertices of the round-join discs at its turns lie exactly on the ray of that row's pixel centers (the
 * ray y is the vertex y in single precision), where a curve that ends on the ray has its root at t = 1 rounded to just
 * above 1, or the one that starts there at just below 0: the crossing was dropped for one and counted for the other,
 * and the winding of everything left of the turn was off by one (a stray pixel before the start, holes after it). */
EMB_TEST(draw_stroke_wire_with_vertices_on_the_pixel_row)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    static const double cfg[][5] = {
        /* zoom, user x of the start, user y, device x of the start, device y of the line */
        {1.0001, 50.51, 449.7857, 100, 32.5002},
        {1, 50.51, 449.7857, 100, 32.5002},
        {1, 50.51, 449.7857, 100, 32.5},
        {1, 50.5, 449.5, 100, 32.5},
        {1, 50.0, 449.5, 100, 32.5},
        {1, 50.51, 49.7857, 100, 32.5002},
    };
    embi_canvas *cv = cv_new(&r, 200, 60, true);
    for (size_t c = 0; c < sizeof cfg / sizeof *cfg; c++) {
        const double z = cfg[c][0], x0 = cfg[c][1], y0 = cfg[c][2];
        embi_canvas_set_transform(cv, (double[6]){1, 0, 0, 1, 0, 0});
        embi_canvas_clear_rect(cv, 0, 0, 200, 60);
        embi_canvas_set_transform(cv, (double[6]){z, 0, 0, z, cfg[c][3] - z * x0, cfg[c][4] - z * y0});
        double m[6];
        embi_canvas_get_transform(cv, m);
        embi_canvas_set_color(cv, 1, S("#ff0000"));
        embi_canvas_set_line_width(cv, 2);
        embi_canvas_begin_path(cv);
        embi_cpath *p = embi_canvas_path(cv);
        embi_cpath_move_to(p, m, x0, y0);
        embi_cpath_cubic_to(p, m, x0 + 40, y0, x0 + 19 - 40, y0, x0 + 19, y0);
        embi_canvas_stroke(cv, nullptr);
        uint8_t *px = snap(cv);
        const int row = (int)floor(cfg[c][4]);
        EMB_CHECK(px[((size_t)row * 200 + 99) * 4 + 3] <= 2);
        for (int x = 101; x < 117; x++) {
            const uint8_t *q = px + ((size_t)row * 200 + (size_t)x) * 4;
            if (q[3] < 253 || q[0] < 253) {
                SDL_Log("wire %d: hole at %d,%d alpha %d", (int)c, x, row, q[3]);
                EMB_CHECK(false);
                break;
            }
        }
        SDL_free(px);
    }
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A reversing collinear wire inside a longer path and with each line cap: its stroke joins the neighbouring segments
 * and ends as a plain line's does. Path: a line to the right, the wire (overshooting past both ends), a line down. */
EMB_TEST(draw_stroke_collinear_cubic_with_neighbours_and_caps)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 240, 80, true);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 4);
    /* in a longer path: line (20,20)-(60,20), wire (60,20)-(126,20), line down to (126,60) */
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, nullptr, 20, 20);
    embi_cpath_line_to(p, nullptr, 60, 20);
    embi_cpath_cubic_to(p, nullptr, 100, 20, 86, 20, 126, 20);
    embi_cpath_line_to(p, nullptr, 126, 60);
    embi_canvas_stroke(cv, nullptr);
    uint8_t *px = snap(cv);
    for (int x = 20; x < 122; x++) {
        PX(px, 240, x, 19, 255, 0, 0, 255, TOL); /* rows 18..21 are the line, whole */
        PX(px, 240, x, 20, 255, 0, 0, 255, TOL);
        PX(px, 240, x, 17, 0, 0, 0, 0, 0);
        PX(px, 240, x, 22, 0, 0, 0, 0, 0);
    }
    PX(px, 240, 127, 19, 255, 0, 0, 255, TOL); /* the miter corner: filled out to the square */
    PX(px, 240, 127, 18, 255, 0, 0, 255, TOL);
    PX(px, 240, 126, 40, 255, 0, 0, 255, TOL); /* the line down */
    PX(px, 240, 126, 62, 0, 0, 0, 0, 0);
    PX(px, 240, 19, 20, 0, 0, 0, 0, 0); /* butt cap at the start of the path */
    SDL_free(px);
    /* the wire alone with each cap: it starts at 50.5, the lowest point it reaches, and ends at 69.5 */
    static const char *const caps[3] = {"butt", "round", "square"};
    for (int c = 0; c < 3; c++) {
        embi_canvas_clear_rect(cv, 0, 0, 240, 80);
        embi_canvas_begin_path(cv);
        embi_canvas_set_line_cap(cv, S(caps[c]));
        p = embi_canvas_path(cv);
        embi_cpath_move_to(p, nullptr, 50.5, 40.5);
        embi_cpath_cubic_to(p, nullptr, 90.5, 40.5, 29.5, 40.5, 69.5, 40.5);
        embi_canvas_stroke(cv, nullptr);
        px = snap(cv);
        const int want = c == 0 ? 0 : 255; /* just before the start: only round and square caps reach it */
        EMB_CHECK_EQ_INT(px[((size_t)40 * 240 + 49) * 4 + 3] > 128, want != 0);
        EMB_CHECK_EQ_INT(px[((size_t)40 * 240 + 70) * 4 + 3] > 128, want != 0);
        PX(px, 240, 60, 40, 255, 0, 0, 255, TOL);
        PX(px, 240, 60, 43, 0, 0, 0, 0, 0);
        SDL_free(px);
    }
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A curve that doubles back has a round cusp whatever the line join: a wide line with the default miter join and a
 * collinear cubic that overshoots its end points ends in a half disc at its highest and lowest points. */
EMB_TEST(draw_stroke_collinear_cubic_cusps_are_round_with_a_miter_join)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 240, 60, true);
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_set_line_width(cv, 10);
    const double ax = 100, bx = 119, dx = 100, y = 30.5;
    double lo = 1e9, hi = -1e9;
    for (int k = 0; k <= 2000; k++) {
        const double t = k / 2000.0, mt = 1 - t;
        const double x = ax * mt * mt * mt + 3 * mt * mt * t * (ax + dx) + 3 * mt * t * t * (bx - dx) + bx * t * t * t;
        lo = fmin(lo, x);
        hi = fmax(hi, x);
    }
    EMB_CHECK(hi > bx + 1 || lo < ax - 1); /* it does overshoot */
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_move_to(p, nullptr, ax, y);
    embi_cpath_cubic_to(p, nullptr, ax + dx, y, bx - dx, y, bx, y);
    embi_canvas_stroke(cv, nullptr);
    uint8_t *px = snap(cv);
    const int row = 30;
    /* the bar is flat at [lo, hi] except where a cusp is: the disc reaches 5 px past an extreme that is a turn */
    for (int e = 0; e < 2; e++) {
        const double ext = e == 0 ? hi : lo;
        const bool turn = e == 0 ? hi > bx + 0.5 : lo < ax - 0.5;
        if (!turn) {
            continue;
        }
        const int sx = e == 0 ? (int)floor(ext) + 3 : (int)ceil(ext) - 4;
        EMB_CHECK(px[((size_t)row * 240 + (size_t)sx) * 4 + 3] > 200);                 /* inside the disc */
        EMB_CHECK(px[((size_t)(row - 5) * 240 + (size_t)sx) * 4 + 3] < 60);            /* its corner is round */
    }
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_stroke_under_a_transform_scales_the_line_width)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    embi_canvas_scale(cv, 4, 1);
    embi_canvas_set_color(cv, 1, S("#0000ff"));
    embi_canvas_set_line_width(cv, 2);
    embi_canvas_stroke_rect(cv, 4, 20, 8, 20);
    uint8_t *px = snap(cv);
    /* The vertical edges are 2 * 4 = 8 px wide, the horizontal ones 2 px. */
    PX(px, 64, 17, 30, 0, 0, 255, 255, TOL);
    PX(px, 64, 14, 30, 0, 0, 255, 255, TOL);
    PX(px, 64, 19, 30, 0, 0, 255, 255, TOL);
    PX(px, 64, 22, 30, 0, 0, 0, 0, 0);
    PX(px, 64, 10, 30, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* ------------------------------------------------------------------ gradients and patterns */

static embi_cgrad *grad2(embi_canvas *cv, int kind, const double g[6], const char *c0, const char *c1)
{
    embi_cgrad *gr = embi_cgrad_new(kind, g);
    int e0 = embi_cgrad_add_stop(gr, cv, 0.0, S(c0)), e1 = embi_cgrad_add_stop(gr, cv, 1.0, S(c1));
    EMB_CHECK(e0 == EMBI_CV_OK && e1 == EMBI_CV_OK);
    return gr;
}

EMB_TEST(draw_linear_gradient)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 100, 8, true);
    const double g4[6] = {0, 0, 100, 0, 0, 0};
    embi_cgrad *g = grad2(cv, EMBI_CG_LINEAR, g4, "#ff0000", "#0000ff");
    embi_canvas_set_gradient(cv, 0, g);
    embi_canvas_fill_rect(cv, 0, 0, 100, 8);
    uint8_t *px = snap(cv);
    PX(px, 100, 1, 4, 252, 0, 3, 255, 6);
    PX(px, 100, 50, 4, 128, 0, 127, 255, 6);
    PX(px, 100, 98, 4, 5, 0, 250, 255, 6);
    SDL_free(px);
    embi_cgrad_release(g);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_gradient_stops_clamp_outside_the_line)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 100, 8, true);
    const double g4[6] = {30, 0, 70, 0, 0, 0};
    embi_cgrad *g = grad2(cv, EMBI_CG_LINEAR, g4, "#ff0000", "#0000ff");
    embi_canvas_set_gradient(cv, 0, g);
    embi_canvas_fill_rect(cv, 0, 0, 100, 8);
    uint8_t *px = snap(cv);
    PX(px, 100, 5, 4, 255, 0, 0, 255, TOL);
    PX(px, 100, 95, 4, 0, 0, 255, 255, TOL);
    SDL_free(px);
    embi_cgrad_release(g);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_radial_gradient)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    const double g6[6] = {32, 32, 0, 32, 32, 30};
    embi_cgrad *g = grad2(cv, EMBI_CG_RADIAL, g6, "#ffffff", "#000000");
    embi_canvas_set_gradient(cv, 0, g);
    embi_canvas_fill_rect(cv, 0, 0, 64, 64);
    uint8_t *px = snap(cv);
    PX(px, 64, 32, 32, 255, 255, 255, 255, 8);
    PX(px, 64, 47, 32, 128, 128, 128, 255, 12);
    PX(px, 64, 2, 2, 0, 0, 0, 255, 2);
    SDL_free(px);
    embi_cgrad_release(g);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_conic_gradient)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    const double g3[6] = {0, 32, 32, 0, 0, 0};
    embi_cgrad *g = grad2(cv, EMBI_CG_CONIC, g3, "#ff0000", "#0000ff");
    embi_canvas_set_gradient(cv, 0, g);
    embi_canvas_fill_rect(cv, 0, 0, 64, 64);
    uint8_t *px = snap(cv);
    /* Angle 0 points right; the sweep goes clockwise on screen: just below the right axis is red, just above it is blue,
     * the bottom is a quarter of the way. */
    PX(px, 64, 60, 34, 252, 0, 3, 255, 24);
    PX(px, 64, 60, 30, 3, 0, 252, 255, 24);
    PX(px, 64, 32, 60, 191, 0, 64, 255, 24);
    SDL_free(px);
    embi_cgrad_release(g);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_pattern_repeat_and_no_repeat)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 32, 32, true);
    embi_cimage *im = embi_cimage_new(2, 2);
    const uint8_t t[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    memcpy(im->px, t, sizeof t);
    embi_cpattern *pt = embi_canvas_pattern_from(cv, im, nullptr, EMBI_CP_REPEAT);
    EMB_REQUIRE(pt != nullptr);
    embi_canvas_set_pattern(cv, 0, pt);
    embi_canvas_fill_rect(cv, 0, 0, 32, 32);
    uint8_t *px = snap(cv);
    PX(px, 32, 0, 0, 255, 0, 0, 255, TOL);
    PX(px, 32, 1, 0, 0, 255, 0, 255, TOL);
    PX(px, 32, 0, 1, 0, 0, 255, 255, TOL);
    PX(px, 32, 1, 1, 255, 255, 255, 255, TOL);
    PX(px, 32, 20, 20, 255, 0, 0, 255, TOL);
    PX(px, 32, 21, 20, 0, 255, 0, 255, TOL);
    SDL_free(px);
    embi_cpattern_release(pt);
    embi_cimage_release(im);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* ------------------------------------------------------------------ compositing */

typedef struct pd_case {
    const char *op;
    /* expected RGBA at (dest only, overlap, source only, empty) */
    uint8_t e[4][4];
} pd_case;

EMB_TEST(draw_porter_duff_operators)
{
    static const pd_case cases[] = {
        {"source-over", {{255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"source-in", {{0, 0, 0, 0}, {0, 0, 255, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
        {"source-out", {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"source-atop", {{255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
        {"destination-over", {{255, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"destination-in", {{0, 0, 0, 0}, {255, 0, 0, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
        {"destination-out", {{255, 0, 0, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
        {"destination-atop", {{0, 0, 0, 0}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"lighter", {{255, 0, 0, 255}, {255, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"copy", {{0, 0, 0, 0}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
        {"xor", {{255, 0, 0, 255}, {0, 0, 0, 0}, {0, 0, 255, 255}, {0, 0, 0, 0}}},
    };
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_canvas *cv = cv_new(&r, 80, 8, true);
        fill(cv, "#ff0000", 0, 0, 40, 8);
        embi_canvas_set_composite(cv, S(cases[i].op));
        fill(cv, "#0000ff", 20, 0, 40, 8);
        uint8_t *px = snap(cv);
        const int xs[4] = {10, 30, 50, 70};
        for (int k = 0; k < 4; k++) {
            const uint8_t *e = cases[i].e[k];
            const uint8_t *p = px + (4 * 80 + (size_t)xs[k]) * 4;
            bool ok = true;
            for (int c = 0; c < 4; c++) {
                int d = (int)p[c] - (int)e[c];
                if (d < -TOL || d > TOL) {
                    ok = false;
                }
            }
            if (!ok) {
                SDL_Log("%s region %d: got %u,%u,%u,%u want %u,%u,%u,%u", cases[i].op, k, p[0], p[1], p[2], p[3], e[0],
                        e[1], e[2], e[3]);
            }
            EMB_CHECK(ok);
        }
        SDL_free(px);
        embi_canvas_destroy(cv);
    }
    rig_close(&r);
}

EMB_TEST(draw_blend_modes)
{
    typedef struct bm {
        const char *op;
        uint8_t dst[3], src[3], want[3];
    } bm;
    static const bm cases[] = {
        {"multiply", {255, 128, 0}, {128, 128, 255}, {128, 64, 0}},
        {"screen", {255, 128, 0}, {128, 128, 255}, {255, 192, 255}},
        {"darken", {255, 128, 0}, {128, 128, 255}, {128, 128, 0}},
        {"lighten", {255, 128, 0}, {128, 128, 255}, {255, 128, 255}},
        {"difference", {255, 128, 0}, {128, 128, 255}, {127, 0, 255}},
        {"exclusion", {255, 128, 0}, {128, 128, 255}, {127, 128, 255}},
        {"hard-light", {255, 128, 0}, {128, 128, 255}, {255, 128, 255}},
        {"overlay", {255, 128, 0}, {128, 128, 255}, {255, 128, 0}},
    };
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_canvas *cv = cv_new(&r, 8, 8, true);
        char css[40];
        SDL_snprintf(css, sizeof css, "rgb(%u, %u, %u)", cases[i].dst[0], cases[i].dst[1], cases[i].dst[2]);
        fill(cv, css, 0, 0, 8, 8);
        embi_canvas_set_composite(cv, S(cases[i].op));
        SDL_snprintf(css, sizeof css, "rgb(%u, %u, %u)", cases[i].src[0], cases[i].src[1], cases[i].src[2]);
        fill(cv, css, 0, 0, 8, 8);
        uint8_t *px = snap(cv);
        const uint8_t *p = px + (4 * 8 + 4) * 4;
        bool ok = true;
        for (int c = 0; c < 3; c++) {
            int d = (int)p[c] - (int)cases[i].want[c];
            if (d < -3 || d > 3) {
                ok = false;
            }
        }
        if (!ok || p[3] != 255) {
            SDL_Log("%s: got %u,%u,%u,%u want %u,%u,%u", cases[i].op, p[0], p[1], p[2], p[3], cases[i].want[0],
                    cases[i].want[1], cases[i].want[2]);
        }
        EMB_CHECK(ok && p[3] == 255);
        SDL_free(px);
        embi_canvas_destroy(cv);
    }
    rig_close(&r);
}

/* ------------------------------------------------------------------ clip and shadow */

EMB_TEST(draw_clip_rect_and_restore)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 48, 48, true);
    embi_canvas_save(cv);
    embi_cpath *p = embi_canvas_path(cv);
    embi_cpath_rect(p, nullptr, 10, 10, 20, 20);
    embi_canvas_clip(cv, nullptr, false);
    fill(cv, "#ff0000", 0, 0, 48, 48);
    embi_canvas_restore(cv);
    fill(cv, "#0000ff", 40, 40, 8, 8);
    uint8_t *px = snap(cv);
    PX(px, 48, 20, 20, 255, 0, 0, 255, TOL);
    PX(px, 48, 5, 5, 0, 0, 0, 0, 0);
    PX(px, 48, 35, 35, 0, 0, 0, 0, 0);
    PX(px, 48, 44, 44, 0, 0, 255, 255, TOL); /* after restore the clip is gone */
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_clip_circle_intersects_with_a_second_clip)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    embi_cpath *p = embi_canvas_path(cv);
    (void)embi_cpath_arc(p, nullptr, 32, 32, 24, 0, 2 * CV_PI, false);
    embi_canvas_clip(cv, nullptr, false);
    embi_canvas_begin_path(cv);
    p = embi_canvas_path(cv);
    embi_cpath_rect(p, nullptr, 0, 0, 32, 64);
    embi_canvas_clip(cv, nullptr, false);
    fill(cv, "#00ff00", 0, 0, 64, 64);
    uint8_t *px = snap(cv);
    PX(px, 64, 20, 32, 0, 255, 0, 255, TOL);
    PX(px, 64, 45, 32, 0, 0, 0, 0, 0); /* inside the circle, outside the second clip */
    PX(px, 64, 3, 3, 0, 0, 0, 0, 0);   /* inside the second clip, outside the circle */
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_shadow_offset_without_blur)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    embi_canvas_set_shadow_color(cv, S("#0000ff"));
    embi_canvas_set_shadow_x(cv, 20);
    embi_canvas_set_shadow_y(cv, 20);
    fill(cv, "#ff0000", 4, 4, 16, 16);
    uint8_t *px = snap(cv);
    PX(px, 64, 10, 10, 255, 0, 0, 255, TOL);
    PX(px, 64, 30, 30, 0, 0, 255, 255, TOL);
    PX(px, 64, 22, 22, 0, 0, 0, 0, 0);
    PX(px, 64, 45, 45, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_shadow_blur_is_soft)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 96, 96, true);
    embi_canvas_set_shadow_color(cv, S("#000000"));
    embi_canvas_set_shadow_blur(cv, 12);
    embi_canvas_set_shadow_x(cv, 0);
    embi_canvas_set_shadow_y(cv, 40);
    fill(cv, "#ffffff", 30, 10, 36, 20);
    uint8_t *px = snap(cv);
    /* The shadow is centred at y = 60, x = 48: dense in the middle, fading at the edge and gone further out. */
    int centre = px[(60 * 96 + 48) * 4 + 3];
    int edge = px[(50 * 96 + 48) * 4 + 3];
    int outside = px[(88 * 96 + 48) * 4 + 3];
    EMB_CHECK(centre > 200);
    EMB_CHECK(edge > 20 && edge < 235);
    EMB_CHECK(outside < 10);
    EMB_CHECK(px[(60 * 96 + 5) * 4 + 3] < 10);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* ------------------------------------------------------------------ images and pixels */

EMB_TEST(draw_put_and_get_image_data)
{
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 8, 8, true);
    uint8_t src[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 128, 10, 20, 30, 0};
    embi_canvas_put(cv, src, 2, 2, 3, 3);
    uint8_t got[8 * 8 * 4];
    EMB_REQUIRE(embi_canvas_read(cv, 0, 0, 8, 8, got));
    PX(got, 8, 3, 3, 255, 0, 0, 255, 0);
    PX(got, 8, 4, 3, 0, 255, 0, 255, 0);
    PX(got, 8, 3, 4, 0, 0, 255, 128, 2);
    PX(got, 8, 4, 4, 0, 0, 0, 0, 0); /* a fully transparent pixel loses its color */
    PX(got, 8, 2, 2, 0, 0, 0, 0, 0);
    embi_canvas_destroy(cv);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(draw_read_outside_the_canvas_is_transparent)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 4, 4, true);
    fill(cv, "#ffffff", 0, 0, 4, 4);
    uint8_t got[8 * 8 * 4];
    EMB_REQUIRE(embi_canvas_read(cv, -2, -2, 8, 8, got));
    PX(got, 8, 0, 0, 0, 0, 0, 0, 0);
    PX(got, 8, 2, 2, 255, 255, 255, 255, 0);
    PX(got, 8, 6, 6, 0, 0, 0, 0, 0);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_image_from_a_picture_with_smoothing)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 32, 32, true);
    embi_cimage *im = embi_cimage_new(2, 2);
    const uint8_t t[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    memcpy(im->px, t, sizeof t);
    embi_canvas_set_smoothing(cv, false);
    const double s[4] = {0, 0, 2, 2}, d[4] = {0, 0, 32, 32};
    EMB_CHECK_EQ_INT(embi_canvas_draw_image(cv, im, nullptr, s, d), EMBI_CV_OK);
    uint8_t *px = snap(cv);
    PX(px, 32, 4, 4, 255, 0, 0, 255, TOL);
    PX(px, 32, 28, 4, 0, 255, 0, 255, TOL);
    PX(px, 32, 4, 28, 0, 0, 255, 255, TOL);
    PX(px, 32, 28, 28, 255, 255, 0, 255, TOL);
    SDL_free(px);
    /* A sub-rectangle of the source. */
    embi_canvas_clear_rect(cv, 0, 0, 32, 32);
    const double s2[4] = {1, 0, 1, 1}, d2[4] = {8, 8, 16, 16};
    EMB_CHECK_EQ_INT(embi_canvas_draw_image(cv, im, nullptr, s2, d2), EMBI_CV_OK);
    px = snap(cv);
    PX(px, 32, 12, 12, 0, 255, 0, 255, TOL);
    PX(px, 32, 4, 4, 0, 0, 0, 0, 0);
    PX(px, 32, 28, 28, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_cimage_release(im);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_image_from_another_canvas)
{
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *src = cv_new(&r, 8, 8, true);
    embi_canvas *dst = cv_new(&r, 32, 32, true);
    fill(src, "#ff8000", 0, 0, 8, 8);
    const double s[4] = {0, 0, 8, 8}, d[4] = {4, 4, 8, 8};
    EMB_CHECK_EQ_INT(embi_canvas_draw_image(dst, nullptr, src, s, d), EMBI_CV_OK);
    /* Drawing on the source afterwards does not change what was drawn. */
    fill(src, "#0000ff", 0, 0, 8, 8);
    uint8_t *px = snap(dst);
    PX(px, 32, 8, 8, 255, 128, 0, 255, TOL);
    PX(px, 32, 14, 14, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(src);
    embi_canvas_destroy(dst);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(draw_a_canvas_onto_itself)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 32, 16, true);
    fill(cv, "#ff0000", 0, 0, 8, 8);
    const double s[4] = {0, 0, 8, 8}, d[4] = {16, 0, 8, 8};
    EMB_CHECK_EQ_INT(embi_canvas_draw_image(cv, nullptr, cv, s, d), EMBI_CV_OK);
    uint8_t *px = snap(cv);
    PX(px, 32, 4, 4, 255, 0, 0, 255, TOL);
    PX(px, 32, 20, 4, 255, 0, 0, 255, TOL);
    PX(px, 32, 12, 4, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_resize_clears_and_resets)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 16, 16, true);
    fill(cv, "#ff0000", 0, 0, 16, 16);
    embi_canvas_resize(cv, 24, 8);
    uint8_t *px = snap(cv);
    PX(px, 24, 4, 4, 0, 0, 0, 0, 0);
    PX(px, 24, 20, 4, 0, 0, 0, 0, 0);
    SDL_free(px);
    fill(cv, "#00ff00", 0, 0, 24, 8);
    px = snap(cv);
    PX(px, 24, 20, 4, 0, 255, 0, 255, TOL);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* A page that makes its canvas again on each update: the new canvas of the same size gets the texture of the one that
 * went away (creating one costs about 0.7 ms on Direct3D 12), starting clear; embi_canvas_gpu_trim frees the spares that
 * stayed unused. */
EMB_TEST(draw_canvas_made_again_reuses_the_texture)
{
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 48, true);
    fill(cv, "#ff0000", 0, 0, 32, 48);
    uint8_t *px = snap(cv);
    PX(px, 64, 10, 10, 255, 0, 0, 255, TOL);
    SDL_free(px);
    const void *tex = nullptr;
    uint32_t tw = 0, th = 0;
    tex = embi_canvas_gpu_texture(r.cg, embi_canvas_key(cv), &tw, &th);
    EMB_CHECK(tex != nullptr);
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 64 * 48 * 4);
    embi_canvas_destroy(cv);
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 64 * 48 * 4); /* kept as a spare */

    cv = cv_new(&r, 64, 48, true);
    fill(cv, "#0000ff", 32, 0, 32, 48);
    px = snap(cv);
    PX(px, 64, 10, 10, 0, 0, 0, 0, 0); /* the old picture is not there */
    PX(px, 64, 40, 10, 0, 0, 255, 255, TOL);
    SDL_free(px);
    EMB_CHECK(embi_canvas_gpu_texture(r.cg, embi_canvas_key(cv), &tw, &th) == tex);
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 64 * 48 * 4);

    /* Another size gets a texture of its own; the spare left unused goes after the keep time. */
    embi_canvas_destroy(cv);
    cv = cv_new(&r, 32, 32, true);
    fill(cv, "#00ff00", 0, 0, 8, 8);
    px = snap(cv);
    PX(px, 32, 4, 4, 0, 255, 0, 255, TOL);
    SDL_free(px);
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 64 * 48 * 4 + 32 * 32 * 4);
    embi_canvas_gpu_trim(r.cg, SDL_GetTicks());
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 64 * 48 * 4 + 32 * 32 * 4);
    embi_canvas_gpu_trim(r.cg, SDL_GetTicks() + 60000);
    EMB_CHECK_EQ_INT((int)embi_canvas_gpu_bytes(r.cg), 32 * 32 * 4);
    embi_canvas_destroy(cv);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(draw_to_png_decodes_to_the_pixels)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 10, 6, true);
    fill(cv, "#336699", 0, 0, 10, 6);
    embi_sb png = {0};
    EMB_REQUIRE(embi_canvas_to_png(cv, &png));
    embi_pixels out;
    EMB_REQUIRE(embi_png_decode((const uint8_t *)png.data, png.len, 1u << 20, false, &out));
    EMB_CHECK_EQ_INT(out.w, 10);
    EMB_CHECK_EQ_INT(out.h, 6);
    PX(out.px, 10, 5, 3, 0x33, 0x66, 0x99, 255, 1);
    embi_pixels_free(&out);
    embi_sb_free(&png);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_many_commands_between_flushes)
{
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 64, 64, true);
    for (int i = 0; i < 400; i++) {
        char css[32];
        SDL_snprintf(css, sizeof css, "rgb(%d, %d, 0)", (i * 7) & 255, (i * 13) & 255);
        fill(cv, css, (i % 8) * 8, ((i / 8) % 8) * 8, 8, 8);
    }
    uint8_t *px = snap(cv);
    /* The last write to the cell (0, 0) was command 384. */
    PX(px, 64, 4, 4, (384 * 7) & 255, (384 * 13) & 255, 0, 255, TOL);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

/* ------------------------------------------------------------------ text */

EMB_TEST(text_measure_and_draw)
{
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, true)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 160, 60, true);
    embi_canvas_set_text_attr(cv, 0, S("32px 'Noto Test'"));
    embi_text_metrics m;
    EMB_REQUIRE(embi_canvas_measure_text(cv, S("Hello"), &m));
    EMB_CHECK(m.width > 40 && m.width < 120);
    EMB_CHECK(m.ascent > 15 && m.ascent < 40);
    EMB_CHECK(m.font_ascent >= m.ascent - 1);
    EMB_CHECK(m.descent >= -1);
    EMB_CHECK(fabs(m.left) < 4);
    EMB_CHECK(m.right > m.width - 6 && m.right < m.width + 6);
    EMB_CHECK(m.alphabetic == 0.0);
    embi_text_metrics e;
    EMB_REQUIRE(embi_canvas_measure_text(cv, S(""), &e));
    EMB_CHECK(e.width == 0.0);
    /* Alignment moves the box. */
    embi_canvas_set_text_attr(cv, 1, S("center"));
    embi_text_metrics c;
    EMB_REQUIRE(embi_canvas_measure_text(cv, S("Hello"), &c));
    EMB_CHECK(fabs(c.left - m.width / 2) < 4);
    embi_canvas_set_text_attr(cv, 1, S("start"));
    embi_canvas_set_color(cv, 0, S("#000000"));
    embi_canvas_draw_text(cv, S("Hello"), 10, 40, INFINITY, false);
    uint8_t *px = snap(cv);
    int inked = 0, outside = 0;
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 160; x++) {
            int a = px[((size_t)y * 160 + (size_t)x) * 4 + 3];
            if (a > 128) {
                inked++;
                if (x < 8 || x > 10 + (int)m.width + 4 || y > 50 || y < 40 - (int)m.ascent - 3) {
                    outside++;
                }
            }
        }
    }
    EMB_CHECK(inked > 150);
    EMB_CHECK_EQ_INT(outside, 0);
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(text_stroke_and_max_width)
{
    rig r;
    if (!rig_open(&r, true)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 160, 60, true);
    embi_canvas_set_text_attr(cv, 0, S("32px 'Noto Test'"));
    embi_canvas_set_color(cv, 1, S("#ff0000"));
    embi_canvas_draw_text(cv, S("Hi"), 10, 40, 20, true);
    uint8_t *px = snap(cv);
    int inked = 0, wide = 0;
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 160; x++) {
            if (px[((size_t)y * 160 + (size_t)x) * 4 + 3] > 64) {
                inked++;
                if (x > 10 + 22) {
                    wide++;
                }
            }
        }
    }
    EMB_CHECK(inked > 30);
    EMB_CHECK_EQ_INT(wide, 0); /* squeezed into 20 px */
    SDL_free(px);
    embi_canvas_destroy(cv);
    rig_close(&r);
}

EMB_TEST(draw_frame_flush_attaches_without_a_read)
{
    /* The frame path: a canvas that is drawn on and never read back is flushed by embi_canvas_gpu_flush and has a texture. */
    size_t live = embi_mem_live();
    rig r;
    if (!rig_open(&r, false)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_canvas *cv = cv_new(&r, 32, 24, true);
    embi_canvas *idle = cv_new(&r, 8, 8, true); /* never drawn on: it still gets a cleared texture */
    fill(cv, "#00ff00", 4, 4, 10, 10);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(r.k.dev);
    EMB_REQUIRE(cmd != nullptr);
    embi_canvas_gpu_flush(r.cg, cmd);
    uint32_t w = 0, h = 0;
    SDL_GPUTexture *t = embi_canvas_gpu_texture(r.cg, embi_canvas_key(cv), &w, &h);
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(w == 32 && h == 24);
    uint32_t iw = 0, ih = 0;
    SDL_GPUTexture *it = embi_canvas_gpu_texture(r.cg, embi_canvas_key(idle), &iw, &ih);
    EMB_CHECK(it != nullptr && iw == 8 && ih == 8);
    layer_dl d = layer_rig_download(&r.k, cmd, t, w, h);
    EMB_REQUIRE(layer_rig_submit(&r.k, cmd));
    uint8_t *px = layer_rig_map(&r.k, d);
    EMB_REQUIRE(px != nullptr);
    PX(px, 32, 8, 8, 0, 255, 0, 255, TOL);
    PX(px, 32, 20, 8, 0, 0, 0, 0, 0);
    SDL_free(px);

    /* A resize drops the texture; the canvas is back on the flush list and gets a texture of the new size. */
    embi_canvas_resize(cv, 16, 16);
    fill(cv, "#0000ff", 0, 0, 16, 16);
    cmd = SDL_AcquireGPUCommandBuffer(r.k.dev);
    EMB_REQUIRE(cmd != nullptr);
    embi_canvas_gpu_flush(r.cg, cmd);
    t = embi_canvas_gpu_texture(r.cg, embi_canvas_key(cv), &w, &h);
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(w == 16 && h == 16);
    d = layer_rig_download(&r.k, cmd, t, w, h);
    EMB_REQUIRE(layer_rig_submit(&r.k, cmd));
    px = layer_rig_map(&r.k, d);
    EMB_REQUIRE(px != nullptr);
    PX(px, 16, 8, 8, 0, 0, 255, 255, TOL);
    SDL_free(px);

    /* A canvas that outlives the manager is a context with no pixels, not a dangling pointer. */
    embi_canvas_gpu_destroy(r.cg);
    r.cg = nullptr;
    embi_canvas_fill_rect(cv, 0, 0, 4, 4);
    uint8_t one[4];
    EMB_CHECK(!embi_canvas_read(cv, 0, 0, 1, 1, one));
    embi_canvas_destroy(cv);
    embi_canvas_destroy(idle);
    rig_close(&r);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_RENDER_TEST_MAIN()
