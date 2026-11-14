/*
 * CPU and submit-to-idle timing for the renderer core, on two scenes:
 *
 *   1. 10k rounded fills, one frame each (the R1 baseline).
 *   2. A realistic page: 2000 items in paint order, interleaved: 600 text runs (Slug glyphs, shaped
 *      with the default font), 50 paths, 300 atlas images on three pages, 500 fills, 300 borders
 *      and 250 gradients. Its frame must draw in at most SCENE_MAX_DRAWS draw calls, or the bench
 *      fails.
 *
 * Each frame is embi_frame_begin, the recording, and embi_frame_end. The CPU time is measured
 * around those calls. The GPU time is the time from submit to the GPU going idle, which includes
 * the submit and the wait, so it is an upper bound on the GPU work. Build and run by hand, from a
 * Release build for numbers:
 *     cmake --build build/<dir> --target bench_render
 *     build/<dir>/tools/bench_render
 * ctest does not run it. Without a GPU device it prints a note and exits 0.
 */

#include "base/base.h"
#include "gpu/atlas.h"
#include "gpu/path.h"
#include "gpu/render.h"
#include "text/text.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define BENCH_PRIMS 10000u
#define BENCH_WARMUP 20u
#define BENCH_FRAMES 200u
#define BENCH_W 1024u
#define BENCH_H 1024u

#define SCENE_ITEMS 2000u
#define SCENE_TEXT 600u
#define SCENE_PATHS 50u
#define SCENE_IMAGES 300u
#define SCENE_PAGE_IMAGES 9u /* 1000 x 1000 images, four to a shared page: three pages */
#define SCENE_IMG_SIDE 1000u
#define SCENE_FILLS 500u
#define SCENE_BORDERS 300u
#define SCENE_GRADIENTS 250u
#define SCENE_W 1280u
#define SCENE_H 800u
#define SCENE_MAX_DRAWS 4u

static double bench_ms(uint64_t from, uint64_t to)
{
    return (double)(to - from) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* 10k rounded 8 x 8 fills on a 100 x 100 grid of 10 px cells, so every prim lands on the target. */
static void bench_record(embi_frame *f)
{
    for (uint32_t i = 0; i < BENCH_PRIMS; i++) {
        const float rect[4] = {(float)((i % 100u) * 10u), (float)((i / 100u) * 10u), 8.0f, 8.0f};
        const float radii[4] = {2.0f, 2.0f, 2.0f, 2.0f};
        const uint32_t color = 0xff000000u | (i & 255u) | (128u << 8) | (64u << 16);
        embi_frame_fill(f, rect, radii, color, 0, 0);
    }
}

/* ------------------------------------------------------------------ realistic scene */

enum {
    ITEM_TEXT,
    ITEM_PATH,
    ITEM_IMAGE,
    ITEM_FILL,
    ITEM_BORDER,
    ITEM_GRADIENT,
};

/* One item of the page, in paint order. idx selects its text run, path, or image. */
typedef struct scene_item {
    uint8_t kind;
    uint32_t idx;
    float x, y, w, h;
    uint32_t color;
} scene_item;

typedef struct scene {
    embi_text *text;
    embi_text_layout lay[SCENE_TEXT];
    embi_text_vruns vr[SCENE_TEXT];
    uint32_t glyphs;                    /* glyphs over all text runs, the outline prims drawn */
    embi_path *paths[SCENE_PATHS];
    embi_atlas *atlas;
    embi_atlas_region img[SCENE_PAGE_IMAGES];
    scene_item items[SCENE_ITEMS];
} scene;

static const char *const scene_words[] = {
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit.",
    "sed do eiusmod tempor incididunt ut labore et dolore magna",
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco.",
    "duis aute irure dolor in reprehenderit in voluptate velit",
    "Excepteur sint occaecat cupidatat non proident, sunt in culpa.",
    "qui officia deserunt mollit anim id est laborum sed perspiciatis",
};

/* A star of n points with outer radius ro and inner ri, centered on the origin. */
static embi_path *scene_star(uint32_t n, float ro, float ri)
{
    embi_path *p = embi_path_create(0.0f);
    if (p == nullptr) {
        return nullptr;
    }
    for (uint32_t k = 0; k < 2u * n; k++) {
        const float r = (k & 1u) ? ri : ro;
        const float a = (float)k * 3.14159265f / (float)n;
        const float x = r * SDL_cosf(a);
        const float y = r * SDL_sinf(a);
        if (k == 0) {
            embi_path_move_to(p, x, y);
        } else {
            embi_path_line_to(p, x, y);
        }
    }
    embi_path_close(p);
    return p;
}

/* A small deterministic generator, so every run draws the same page. */
static uint32_t bench_rand(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static float bench_frand(uint32_t *state, float lo, float hi)
{
    return lo + (hi - lo) * (float)(bench_rand(state) & 0xffffu) / 65535.0f;
}

static void scene_destroy(scene *s)
{
    for (uint32_t k = 0; k < SCENE_TEXT; k++) {
        embi_text_vruns_free(&s->vr[k]);
        embi_text_layout_free(&s->lay[k]);
    }
    for (uint32_t k = 0; k < SCENE_PATHS; k++) {
        embi_path_destroy(s->paths[k]);
    }
    embi_atlas_destroy(s->atlas);
    embi_text_destroy(s->text);
}

/* Builds the scene's text, paths, atlas and item list. Uploads the images, so it needs the device
 * (a command buffer per image, waited on). Returns false on any failure. */
static bool scene_build(scene *s, SDL_GPUDevice *dev)
{
    SDL_memset(s, 0, sizeof *s);
    s->text = embi_text_create();
    if (s->text == nullptr) {
        return false;
    }

    /* Text: each run is one of the phrases, shaped once. The family is missing on purpose: the
     * shaper then uses the default face (see test_text_defaultfont). */
    embi_str fam = {"Ember Sans", 10};
    embi_text_style st;
    SDL_memset(&st, 0, sizeof st);
    st.families = &fam;
    st.family_count = 1;
    st.size = 16.0f;
    st.weight = 400.0f;
    st.font_style = EMBI_STYLE_NORMAL;
    st.stretch = 100.0f;
    st.direction = EMBI_DIR_AUTO;
    st.kerning = EMBI_KERN_AUTO;
    st.ligatures = true;
    st.optical_sizing = true;
    for (uint32_t k = 0; k < SCENE_TEXT; k++) {
        const char *phrase = scene_words[k % (sizeof scene_words / sizeof scene_words[0])];
        const uint32_t len = (uint32_t)SDL_strlen(phrase);
        if (!embi_text_shape(s->text, phrase, len, &st, &s->lay[k])) {
            return false;
        }
        if (!embi_text_line(&s->lay[k], 0, len, &s->vr[k])) {
            return false;
        }
        s->glyphs += s->lay[k].glyphs.len;
    }

    for (uint32_t k = 0; k < SCENE_PATHS; k++) {
        s->paths[k] = scene_star(5u + k % 5u, 8.0f + (float)(k % 7u), 3.0f + (float)(k % 3u));
        if (s->paths[k] == nullptr) {
            return false;
        }
    }

    /* Atlas: nine 1000 x 1000 images, one color each, uploaded before the frames run. */
    s->atlas = embi_atlas_create(dev);
    if (s->atlas == nullptr) {
        return false;
    }
    uint8_t *px = SDL_malloc((size_t)SCENE_IMG_SIDE * SCENE_IMG_SIDE * 4u);
    if (px == nullptr) {
        return false;
    }
    for (uint32_t k = 0; k < SCENE_PAGE_IMAGES; k++) {
        if (!embi_atlas_alloc(s->atlas, SCENE_IMG_SIDE, SCENE_IMG_SIDE, &s->img[k])) {
            SDL_free(px);
            return false;
        }
        const uint32_t color = 0xff000000u | (k * 27u) | ((255u - k * 27u) << 8) | (128u << 16);
        for (uint32_t i = 0; i < SCENE_IMG_SIDE * SCENE_IMG_SIDE; i++) {
            SDL_memcpy(px + (size_t)i * 4u, &color, 4);
        }
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        const bool ok = embi_atlas_upload(s->atlas, &s->img[k], px, SCENE_IMG_SIDE * 4u, cmd);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ok) {
            SDL_free(px);
            return false;
        }
    }
    SDL_WaitForGPUIdle(dev);
    SDL_free(px);

    /* The item list: the kinds in a fixed shuffle, so the paint order mixes them. */
    uint8_t kinds[SCENE_ITEMS];
    uint32_t n = 0;
    for (uint32_t k = 0; k < SCENE_TEXT; k++) {
        kinds[n++] = ITEM_TEXT;
    }
    for (uint32_t k = 0; k < SCENE_PATHS; k++) {
        kinds[n++] = ITEM_PATH;
    }
    for (uint32_t k = 0; k < SCENE_IMAGES; k++) {
        kinds[n++] = ITEM_IMAGE;
    }
    for (uint32_t k = 0; k < SCENE_FILLS; k++) {
        kinds[n++] = ITEM_FILL;
    }
    for (uint32_t k = 0; k < SCENE_BORDERS; k++) {
        kinds[n++] = ITEM_BORDER;
    }
    for (uint32_t k = 0; k < SCENE_GRADIENTS; k++) {
        kinds[n++] = ITEM_GRADIENT;
    }
    uint32_t seed = 0x2e5u;
    for (uint32_t i = SCENE_ITEMS - 1; i > 0; i--) {
        const uint32_t j = bench_rand(&seed) % (i + 1u);
        const uint8_t t = kinds[i];
        kinds[i] = kinds[j];
        kinds[j] = t;
    }

    uint32_t text_k = 0;
    uint32_t path_k = 0;
    uint32_t img_k = 0;
    for (uint32_t i = 0; i < SCENE_ITEMS; i++) {
        scene_item *it = &s->items[i];
        it->kind = kinds[i];
        it->x = bench_frand(&seed, 0.0f, (float)SCENE_W - 220.0f);
        it->y = bench_frand(&seed, 0.0f, (float)SCENE_H - 40.0f);
        it->w = bench_frand(&seed, 12.0f, 120.0f);
        it->h = bench_frand(&seed, 12.0f, 60.0f);
        it->color = 0xff000000u | (bench_rand(&seed) & 0xffffffu);
        switch (it->kind) {
        case ITEM_TEXT:
            it->idx = text_k++;
            break;
        case ITEM_PATH:
            it->idx = path_k++;
            break;
        case ITEM_IMAGE:
            it->idx = img_k++ % SCENE_PAGE_IMAGES;
            it->w = 24.0f;
            it->h = 24.0f;
            break;
        default:
            break;
        }
    }
    return true;
}

static void scene_record(embi_frame *f, const scene *s)
{
    const float uv[4] = {0.1f, 0.1f, 0.9f, 0.9f};
    const embi_stop stops[2] = {{0.0f, 0xffffffffu}, {1.0f, 0xff000000u}};
    for (uint32_t i = 0; i < SCENE_ITEMS; i++) {
        const scene_item *it = &s->items[i];
        const float rect[4] = {it->x, it->y, it->w, it->h};
        const float radii[4] = {4.0f, 4.0f, 4.0f, 4.0f};
        switch (it->kind) {
        case ITEM_TEXT:
            embi_text_draw(s->text, f, &s->lay[it->idx], &s->vr[it->idx], it->x, it->y + 14.0f,
                           it->color, 0, 0);
            break;
        case ITEM_PATH: {
            const float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, it->x + 20.0f, it->y + 20.0f};
            const uint32_t xf = embi_frame_xform(f, m);
            const embi_paint paint = embi_paint_solid(it->color);
            embi_frame_fill_path(f, s->paths[it->idx], EMBI_FILL_NONZERO, &paint, xf, 0);
            break;
        }
        case ITEM_IMAGE:
            embi_frame_image(f, rect, radii, s->img[it->idx], uv, 0xffffffffu, 0, 0);
            break;
        case ITEM_FILL:
            embi_frame_fill(f, rect, radii, it->color, 0, 0);
            break;
        case ITEM_BORDER: {
            const float widths[4] = {2.0f, 2.0f, 2.0f, 2.0f};
            const uint32_t colors[4] = {it->color, it->color, it->color, it->color};
            embi_frame_border(f, rect, radii, widths, colors, 0, 0);
            break;
        }
        case ITEM_GRADIENT:
            embi_frame_linear(f, rect, radii, it->x, it->y, it->x + it->w, it->y, stops, 2,
                              false, 0, 0);
            break;
        default:
            break;
        }
    }
}

static int bench_scene(SDL_GPUDevice *dev, embi_renderer *r)
{
    scene *s = SDL_calloc(1, sizeof *s);
    if (s == nullptr || !scene_build(s, dev)) {
        SDL_Log("bench_render: scene setup failed: %s", SDL_GetError());
        if (s != nullptr) {
            scene_destroy(s);
        }
        SDL_free(s);
        return 1;
    }
    /* A scene target of its own size. The scene and the 10k target share the renderer. */
    SDL_GPUTexture *stex = SDL_CreateGPUTexture(
        dev, &(SDL_GPUTextureCreateInfo){
                 .type = SDL_GPU_TEXTURETYPE_2D,
                 .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                 .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                 .width = SCENE_W,
                 .height = SCENE_H,
                 .layer_count_or_depth = 1,
                 .num_levels = 1,
                 .sample_count = SDL_GPU_SAMPLECOUNT_1,
             });
    if (stex == nullptr) {
        SDL_Log("bench_render: scene target failed: %s", SDL_GetError());
        scene_destroy(s);
        SDL_free(s);
        return 1;
    }

    double cpu_total = 0.0;
    double cpu_worst = 0.0;
    double gpu_total = 0.0;
    double gpu_worst = 0.0;
    uint32_t draws = 0;
    int status = 0;
    for (uint32_t frame = 0; frame < BENCH_WARMUP + BENCH_FRAMES; frame++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        const uint64_t t0 = SDL_GetPerformanceCounter();
        embi_frame *f = embi_frame_begin(r, SCENE_W, SCENE_H, 1.0f);
        if (f != nullptr) {
            scene_record(f, s);
            embi_frame_end(f, cmd, stex, true, 0);
        }
        const uint64_t t1 = SDL_GetPerformanceCounter();
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_WaitForGPUIdle(dev);
        const uint64_t t2 = SDL_GetPerformanceCounter();
        draws = embi_renderer_last_draw_calls(r);
        if (draws > SCENE_MAX_DRAWS) {
            SDL_Log("bench_render: scene drew %u times, over the limit of %u", (unsigned)draws,
                    (unsigned)SCENE_MAX_DRAWS);
            status = 1;
            break;
        }
        if (frame >= BENCH_WARMUP) {
            const double cpu = bench_ms(t0, t1);
            const double gpu = bench_ms(t1, t2);
            cpu_total += cpu;
            gpu_total += gpu;
            if (cpu > cpu_worst) {
                cpu_worst = cpu;
            }
            if (gpu > gpu_worst) {
                gpu_worst = gpu;
            }
        }
    }
    if (status == 0) {
        SDL_Log("bench_render: scene: %u items (%u text runs, %u paths, %u images on 3 pages, "
                "%u fills, %u borders, %u gradients), %u glyph prims per frame",
                (unsigned)SCENE_ITEMS, (unsigned)SCENE_TEXT, (unsigned)SCENE_PATHS,
                (unsigned)SCENE_IMAGES, (unsigned)SCENE_FILLS, (unsigned)SCENE_BORDERS,
                (unsigned)SCENE_GRADIENTS, (unsigned)s->glyphs);
        SDL_Log("bench_render: scene: %u draw calls per frame; %u measured frames: CPU mean %.3f "
                "ms, worst %.3f ms; submit to idle mean %.3f ms, worst %.3f ms",
                (unsigned)draws, (unsigned)BENCH_FRAMES, cpu_total / (double)BENCH_FRAMES,
                cpu_worst, gpu_total / (double)BENCH_FRAMES, gpu_worst);
    }
    SDL_WaitForGPUIdle(dev);
    SDL_ReleaseGPUTexture(dev, stex);
    scene_destroy(s);
    SDL_free(s);
    return status;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("bench_render: SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        SDL_Log("bench_render: no GPU device, nothing measured");
        SDL_Quit();
        return 0;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    embi_renderer *r = embi_renderer_create(g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(
        dev, &(SDL_GPUTextureCreateInfo){
                 .type = SDL_GPU_TEXTURETYPE_2D,
                 .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                 .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                 .width = BENCH_W,
                 .height = BENCH_H,
                 .layer_count_or_depth = 1,
                 .num_levels = 1,
                 .sample_count = SDL_GPU_SAMPLECOUNT_1,
             });
    if (r == nullptr || tex == nullptr) {
        SDL_Log("bench_render: setup failed: %s", SDL_GetError());
        if (tex != nullptr) {
            SDL_ReleaseGPUTexture(dev, tex);
        }
        embi_renderer_destroy(r);
        embi_gpu_destroy(g);
        SDL_Quit();
        return 1;
    }

    double total_ms = 0.0;
    double worst_ms = 0.0;
    uint32_t draws = 0;
    for (uint32_t frame = 0; frame < BENCH_WARMUP + BENCH_FRAMES; frame++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        const uint64_t t0 = SDL_GetPerformanceCounter();
        embi_frame *f = embi_frame_begin(r, BENCH_W, BENCH_H, 1.0f);
        if (f != nullptr) {
            bench_record(f);
            embi_frame_end(f, cmd, tex, true, 0);
        }
        const uint64_t t1 = SDL_GetPerformanceCounter();
        SDL_SubmitGPUCommandBuffer(cmd);
        if (frame >= BENCH_WARMUP) {
            const double ms = bench_ms(t0, t1);
            total_ms += ms;
            if (ms > worst_ms) {
                worst_ms = ms;
            }
        }
    }
    SDL_WaitForGPUIdle(dev);
    draws = embi_renderer_last_draw_calls(r);

    SDL_Log("bench_render: %u prims per frame: %u draw calls, %u measured frames: mean %.3f ms, "
            "worst %.3f ms CPU per frame (begin + fills + end)",
            (unsigned)BENCH_PRIMS, (unsigned)draws, (unsigned)BENCH_FRAMES,
            total_ms / (double)BENCH_FRAMES, worst_ms);

    const int status = bench_scene(dev, r);

    SDL_ReleaseGPUTexture(dev, tex);
    embi_renderer_destroy(r);
    embi_gpu_destroy(g);
    SDL_Quit();
    return status;
}
