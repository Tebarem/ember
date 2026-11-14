/*
 * Partial redraw equals a full redraw (W4d): the damage rects of damage.c, rendered as region
 * frames over the previous frame's pixels (W4b's embi_frame_begin_region with clear), give the same pixels as a full frame
 * of the new list. Cases: one box changes, one text changes between equal-width glyphs, a scroll of a clipped container,
 * several separate changes, and an unchanged frame that records no GPU work.
 *
 * The lists are built here item by item, not from a page: the paint headers (layout, CSS) and the renderer header define
 * clashing names, so this translation unit includes the display list types (paint/dl.h) and the damage functions only.
 * Text runs are shaped through the text shim. Without a GPU device the tests skip.
 */
#include "emb_render_test.h"

#include "base/base.h"
#include "layout/text_shim.h"
#include "paint/damage.h"
#include "paint/dl.h"

#include <math.h>
#include <string.h>

#include <SDL3/SDL_init.h>

#define TW 200
#define TH 200

/* RGBA8 colors, r in the low byte (the display list's format). */
#define PH_RED 0xFF0000FFu
#define PH_GREEN 0xFF00FF00u
#define PH_BLUE 0xFFFF0000u
#define PH_BLACK 0xFF000000u
#define PH_WHITE 0xFFFFFFFFu

/* The renderer context of one test: a device, a renderer and one target that the frames draw into. */
typedef struct rctx {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    embi_renderer *r;
    SDL_GPUTexture *tex;
    embi_text *text;
    uint32_t submits; /* command buffers submitted (a frame with no change must add none) */
} rctx;

static void rctx_close(rctx *c)
{
    if (c->tex != nullptr) {
        SDL_ReleaseGPUTexture(c->dev, c->tex);
    }
    if (c->r != nullptr) {
        embi_renderer_destroy(c->r);
    }
    if (c->g != nullptr) {
        embi_gpu_destroy(c->g);
    }
    if (c->text != nullptr) {
        embi_tshim_destroy(c->text);
    }
    memset(c, 0, sizeof *c);
}

/* Opens the context; false (and the test skips) when there is no GPU device. */
static bool rctx_open(rctx *c)
{
    memset(c, 0, sizeof *c);
    c->g = embi_gpu_create(nullptr, false);
    if (c->g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return false;
    }
    c->dev = embi_gpu_device(c->g);
    c->r = embi_renderer_create(c->g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    SDL_GPUTextureCreateInfo tci;
    memset(&tci, 0, sizeof tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tci.width = TW;
    tci.height = TH;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    c->tex = c->r != nullptr ? SDL_CreateGPUTexture(c->dev, &tci) : nullptr;
    c->text = embi_tshim_create();
    if (c->r == nullptr || c->tex == nullptr || c->text == nullptr) {
        rctx_close(c);
        emb_test_cur_skip = "no GPU device";
        return false;
    }
    return true;
}

static void submit_and_wait(rctx *c, SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    c->submits++;
    if (fence != nullptr) {
        SDL_WaitForGPUFences(c->dev, true, &fence, 1);
        SDL_ReleaseGPUFence(c->dev, fence);
    }
}

/* Replays the list into the frame: the clip chain (parents first, as the list stores it), then the items this test
 * builds (solid fills and text). Other kinds draw nothing here. */
static void replay(embi_frame *f, const embi_dl *d, embi_text *t)
{
    uint32_t *cm = SDL_calloc(d->clips.len + 1, sizeof *cm);
    EMB_REQUIRE(cm != nullptr);
    for (uint32_t c = 1; c < d->clips.len; c++) {
        const embi_dclip *k = &d->clips.data[c];
        cm[c] = embi_frame_clip(f, k->rect, k->radii, 0, k->parent < c ? cm[k->parent] : 0);
    }
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        uint32_t cl = it->clip < d->clips.len ? cm[it->clip] : 0;
        if (it->kind == EMBI_DI_FILL) {
            embi_frame_fill(f, it->rect, it->radii, it->colors[0], 0, cl);
        } else if (it->kind == EMBI_DI_TEXT) {
            const embi_lay_shaped *run = (const embi_lay_shaped *)d->runs.data[it->aux];
            embi_tshim_draw(t, f, run, it->f[0], it->f[1], it->colors[0], 0, cl);
        }
    }
    SDL_free(cm);
}

/* Renders the whole list into the target, clearing it first. */
static void draw_full(rctx *c, const embi_dl *d)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(c->dev);
    EMB_REQUIRE(cmd != nullptr);
    embi_frame *f = embi_frame_begin(c->r, TW, TH, 1.0f);
    replay(f, d, c->text);
    embi_frame_end(f, cmd, c->tex, true, 0);
    submit_and_wait(c, cmd);
}

/* Renders the list over the target's current pixels: one region frame per damage rect, each clearing its own rect
 * (so the rect equals the full frame there) and keeping the rest. */
static void draw_damage(rctx *c, const embi_dl *d, const embi_damage *dm)
{
    for (uint32_t i = 0; i < dm->rects.len; i++) {
        const embi_drect *q = &dm->rects.data[i];
        embi_dev_rect dr = {q->x0, q->y0, q->x1 - q->x0, q->y1 - q->y0};
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(c->dev);
        EMB_REQUIRE(cmd != nullptr);
        embi_frame *f = embi_frame_begin_region(c->r, TW, TH, 1.0f, dr);
        replay(f, d, c->text);
        embi_frame_end(f, cmd, c->tex, true, 0);
        submit_and_wait(c, cmd);
    }
}

/* Reads the target back as tightly packed RGBA8 (SDL_free). */
static uint8_t *readback(rctx *c)
{
    const size_t bytes = (size_t)TW * (size_t)TH * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(c->dev, &tbci);
    if (tb == nullptr) {
        emb_test_report_fail(__FILE__, __LINE__, "no transfer buffer");
        return nullptr;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(c->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = c->tex;
    region.w = TW;
    region.h = TH;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = TW;
    info.rows_per_layer = TH;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    submit_and_wait(c, cmd);
    uint8_t *out = SDL_malloc(bytes);
    void *mapped = SDL_MapGPUTransferBuffer(c->dev, tb, false);
    if (out != nullptr && mapped != nullptr) {
        memcpy(out, mapped, bytes);
        SDL_UnmapGPUTransferBuffer(c->dev, tb);
    } else {
        emb_test_report_fail(__FILE__, __LINE__, "readback failed");
    }
    SDL_ReleaseGPUTransferBuffer(c->dev, tb);
    return out;
}

/* Lists are built with these helpers: a clip sentinel at index 0, then the items in order. */
static void list_init(embi_dl *d)
{
    memset(d, 0, sizeof *d);
    embi_dl_init(d);
    embi_dclip sentinel;
    memset(&sentinel, 0, sizeof sentinel);
    embi_vec_push(&d->clips, sentinel);
}

static uint32_t list_clip(embi_dl *d, float x, float y, float w, float h)
{
    embi_dclip k;
    memset(&k, 0, sizeof k);
    k.rect[0] = x;
    k.rect[1] = y;
    k.rect[2] = w;
    k.rect[3] = h;
    embi_vec_push(&d->clips, k);
    return d->clips.len - 1;
}

static void list_fill(embi_dl *d, float x, float y, float w, float h, uint32_t rgba, uint32_t clip)
{
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_FILL;
    it.rect[0] = x;
    it.rect[1] = y;
    it.rect[2] = w;
    it.rect[3] = h;
    it.colors[0] = rgba;
    it.clip = clip;
    embi_vec_push(&d->items, it);
}

/* A text item of a run, its box at (x, y) of w x h and its baseline at (x, baseline). */
static void list_text(embi_dl *d, embi_lay_shaped *run, float x, float y, float w, float h, float baseline, uint32_t rgba)
{
    uint32_t ri = d->runs.len;
    embi_vec_push(&d->runs, (const void *)run);
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_TEXT;
    it.rect[0] = x;
    it.rect[1] = y;
    it.rect[2] = w;
    it.rect[3] = h;
    it.aux = ri;
    it.aux3 = 0;
    it.aux2 = embi_tshim_len(run);
    embi_ditem_set_chash(&it, embi_tshim_content_hash(run, it.aux3, it.aux2));
    it.f[0] = x;
    it.f[1] = baseline;
    it.colors[0] = rgba;
    embi_vec_push(&d->items, it);
}

static embi_lay_shaped *shape(rctx *c, const char *text)
{
    embi_lay_tstyle ts;
    memset(&ts, 0, sizeof ts);
    ts.size = 28.0f;
    ts.weight = 400.0f;
    ts.stretch = 100.0f;
    ts.letter_spacing = 0.0f;
    ts.word_spacing = 0.0f;
    ts.kerning = 0;
    ts.line_break = 0;
    ts.ligatures = true;
    embi_lay_shaped *s = embi_tshim_shape(c->text, text, (uint32_t)strlen(text), &ts);
    if (s == nullptr) {
        emb_test_report_fail(__FILE__, __LINE__, "shaping failed");
    }
    return s;
}

static void list_free(embi_dl *d)
{
    embi_dl_free(d);
}

/* Checks the pixels of the two readbacks are equal and that something was drawn (a vacuous pass is a failure). */
static void check_same(const uint8_t *partial, const uint8_t *full)
{
    if (partial == nullptr || full == nullptr) {
        return;
    }
    size_t drawn = 0;
    for (size_t i = 0; i < (size_t)TW * TH * 4u; i += 4) {
        drawn += full[i + 3] != 0 ? 1u : 0u;
    }
    EMB_CHECK(drawn > 0);
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < (size_t)TW * TH * 4u; i++) {
        if (partial[i] != full[i]) {
            if (diff == 0) {
                first = i / 4;
            }
            diff++;
        }
    }
    if (diff != 0) {
        char msg[160];
        SDL_snprintf(msg, sizeof msg, "%u bytes differ, first at pixel (%u,%u)", (unsigned)diff,
                     (unsigned)(first % TW), (unsigned)(first / TW));
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
}

EMB_TEST(box_change_partial_redraw_equals_full_redraw)
{
    rctx c;
    if (!rctx_open(&c)) {
        return;
    }
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    list_fill(&a, 10, 10, 60, 40, PH_RED, 0);
    list_fill(&a, 40, 30, 60, 60, PH_BLUE, 0);
    list_fill(&b, 10, 10, 60, 40, PH_RED, 0);
    list_fill(&b, 40, 30, 60, 60, PH_GREEN, 0);
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    EMB_CHECK(dm.any && !dm.full);

    draw_full(&c, &a);
    draw_damage(&c, &b, &dm);
    uint8_t *partial = readback(&c);
    draw_full(&c, &b);
    uint8_t *full = readback(&c);
    check_same(partial, full);
    SDL_free(partial);
    SDL_free(full);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    rctx_close(&c);
}

EMB_TEST(text_change_between_equal_width_glyphs_partial_redraw_equals_full)
{
    rctx c;
    if (!rctx_open(&c)) {
        return;
    }
    embi_lay_shaped *s10 = shape(&c, "10");
    embi_lay_shaped *s11 = shape(&c, "11");
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    list_fill(&a, 0, 0, TW, TH, PH_WHITE, 0);
    list_text(&a, s10, 20, 20, embi_tshim_advance(s10), 34, 46, PH_BLACK);
    list_fill(&b, 0, 0, TW, TH, PH_WHITE, 0);
    list_text(&b, s11, 20, 20, embi_tshim_advance(s11), 34, 46, PH_BLACK);
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    /* The digits' glyphs differ, so the damage is the text box (a change of the item's content, not of its rect). */
    EMB_CHECK(dm.any);

    draw_full(&c, &a);
    draw_damage(&c, &b, &dm);
    uint8_t *partial = readback(&c);
    draw_full(&c, &b);
    uint8_t *full = readback(&c);
    check_same(partial, full);
    SDL_free(partial);
    SDL_free(full);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    embi_tshim_free(s10);
    embi_tshim_free(s11);
    rctx_close(&c);
}

EMB_TEST(scroll_of_a_clipped_container_partial_redraw_equals_full_redraw)
{
    rctx c;
    if (!rctx_open(&c)) {
        return;
    }
    /* A scroll container window at (20, 20) 120 x 80 holds ten rows; the second list is the same rows scrolled by 30 px
     * (the offset the display list build subtracts from the rows). The rows outside the window are clipped away. */
    const float wx = 20, wy = 20, ww = 120, wh = 80;
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    uint32_t ca = list_clip(&a, wx, wy, ww, wh);
    uint32_t cb = list_clip(&b, wx, wy, ww, wh);
    list_fill(&a, 0, 0, TW, TH, PH_WHITE, 0);
    list_fill(&b, 0, 0, TW, TH, PH_WHITE, 0);
    static const uint32_t rows[4] = {PH_RED, PH_GREEN, PH_BLUE, PH_BLACK};
    for (uint32_t i = 0; i < 10; i++) {
        float y = wy + 4.0f + (float)i * 22.0f;
        list_fill(&a, wx + 4, y, 90, 18, rows[i % 4], ca);
        list_fill(&b, wx + 4, y - 30.0f, 90, 18, rows[i % 4], cb);
    }
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    EMB_CHECK(dm.any && !dm.full);
    /* Every damage rect lies in the window (the clip bounds the changed rows). */
    for (uint32_t i = 0; i < dm.rects.len; i++) {
        const embi_drect *q = &dm.rects.data[i];
        EMB_CHECK(q->x0 >= (int32_t)wx && q->y0 >= (int32_t)wy && q->x1 <= (int32_t)(wx + ww) &&
                  q->y1 <= (int32_t)(wy + wh));
    }

    draw_full(&c, &a);
    draw_damage(&c, &b, &dm);
    uint8_t *partial = readback(&c);
    draw_full(&c, &b);
    uint8_t *full = readback(&c);
    check_same(partial, full);
    SDL_free(partial);
    SDL_free(full);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    rctx_close(&c);
}

EMB_TEST(separate_changes_partial_redraw_equals_full_redraw)
{
    rctx c;
    if (!rctx_open(&c)) {
        return;
    }
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    list_fill(&a, 0, 0, TW, TH, PH_WHITE, 0);
    list_fill(&b, 0, 0, TW, TH, PH_WHITE, 0);
    list_fill(&a, 5, 5, 30, 30, PH_RED, 0);
    list_fill(&b, 5, 5, 30, 30, PH_BLUE, 0);
    list_fill(&a, 150, 150, 30, 30, PH_RED, 0);
    list_fill(&b, 150, 150, 30, 30, PH_GREEN, 0);
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    EMB_REQUIRE(dm.rects.len == 2);

    draw_full(&c, &a);
    draw_damage(&c, &b, &dm);
    uint8_t *partial = readback(&c);
    draw_full(&c, &b);
    uint8_t *full = readback(&c);
    check_same(partial, full);
    SDL_free(partial);
    SDL_free(full);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    rctx_close(&c);
}

EMB_TEST(unchanged_frame_records_no_gpu_work)
{
    rctx c;
    if (!rctx_open(&c)) {
        return;
    }
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    list_fill(&a, 10, 10, 60, 40, PH_RED, 0);
    list_fill(&b, 10, 10, 60, 40, PH_RED, 0);
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    EMB_CHECK(!dm.any);
    draw_full(&c, &a);
    uint32_t before = c.submits;
    draw_damage(&c, &b, &dm);
    EMB_CHECK_EQ_INT((int)(c.submits - before), 0);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    rctx_close(&c);
}

EMB_TEST(text_items_and_lists_leave_no_live_memory)
{
    /* The text system, the shaped runs, the lists and the damage are all freed; nothing is kept between them. */
    size_t base = embi_mem_live();
    rctx c;
    memset(&c, 0, sizeof c);
    c.text = embi_tshim_create();
    embi_lay_shaped *s10 = shape(&c, "10");
    embi_lay_shaped *s11 = shape(&c, "11");
    embi_dl a, b;
    list_init(&a);
    list_init(&b);
    list_text(&a, s10, 20, 20, embi_tshim_advance(s10), 34, 46, PH_BLACK);
    list_text(&b, s11, 20, 20, embi_tshim_advance(s11), 34, 46, PH_BLACK);
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a, &b, 1.0f, TW, TH, false);
    EMB_CHECK(dm.any);
    embi_damage_free(&dm);
    list_free(&a);
    list_free(&b);
    embi_tshim_free(s10);
    embi_tshim_free(s11);
    embi_tshim_destroy(c.text);
    EMB_CHECK(embi_mem_live() == base);
}

/* A plain main: the device needs the video subsystem, which is initialized once around the run (as render_check.h). */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
