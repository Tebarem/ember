/*
 * The GPU side of a view (W4e, see gfx.h): the replay of a display list into renderer frames.
 *
 * Replay. The list is walked in order. Every item maps to the renderer's calls (fills, borders, gradients, atlas images,
 * text through the text system, decorations and triangles as fills or outlines). A frame is one renderer frame; the
 * renderer allows one open frame, so a group that needs an intermediate target (EMBI_GROUP_TARGET, W4d) ends the
 * frame in progress, renders the group into a pooled layer texture, begins a new frame on the target and composites
 * the layer into it. Frames of a partial redraw are region frames, so the rect is cleared once and the rest is kept.
 *
 * Opacity. A group that folds (EMBI_GROUP_FOLD) multiplies the alpha of each of its items. The frame's transforms and
 * clips are numbered per frame, so the replay maps the list's indices to the frame's as it goes (per frame arrays).
 */
#include "canvas/canvas.h"
#include "gpu/atlas.h"
#include "gpu/fx.h"
#include "gpu/gpu.h"
#include "gpu/layer.h"
#include "gpu/path.h"
#include "gpu/stroke.h"
#include "gpu/render.h"
#include "image/image.h"
#include "embed/gfx.h"
#include "embed/svgcache.h"
#include "layout/text_shim.h"
#include "paint/damage.h"
#include "paint/dl.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define EMBI_GFX_DEFAULT_BUDGET ((size_t)64 << 20)
/* How long a layer texture nothing used stays in the pool. A layer the size of the view costs about a millisecond to create
 * (the driver allocates and clears it), and a page that changes draws the same layers frame after frame. */
#define EMBI_GFX_LAYER_KEEP_MS 2000u
#define EMBI_GFX_MAX_LAYER_DEPTH 8u
#define EMBI_GFX_MAX_TILES 65536u
#define EMBI_GFX_MAX_WAVE 512u
#define EMBI_GFX_MAX_DASHES 4096u   /* dashes or dots of one decoration line; more are drawn as a solid line */
#define EMBI_GFX_CULL_MARGIN 32.0f  /* CSS px an item may reach beyond its rect (glyph overhang, outlines) */
#define EMBI_GFX_UNMAPPED 0xFFFFFFFFu

struct embi_gfx {
    embi_svgcache *svgs;               /* R4c: parsed SVG images by URL */
    struct embi_style_engine *svg_se;  /* R4c: the engine they are styled with */
    embi_gpu *g;
    SDL_GPUDevice *dev;
    SDL_GPUTextureFormat fmt;
    embi_text *text;
    embi_renderer *r;
    embi_atlas *atlas;
    embi_layer_pool *pool;
    uint32_t image_gen; /* changes whenever a url of the image cache may decode to other pixels (R4b: drawImage's cache) */
    embi_canvas_gpu *canvas; /* R4b: the canvases' textures and their replay */
    embi_fx *fx;        /* filter passes (R3a), made on first use */
    bool fx_failed;     /* embi_fx_create failed once: not retried */
    embi_image_cache *images;
    size_t image_bytes;
    int text_aa;        /* embi_text_aa of the draw in progress */
    bool animating;
    void *user;
    bool (*fetch)(void *user, embi_str url, uint8_t **data, size_t *len);
    uint32_t (*pending_count)(void *user);
    embi_str (*resolve)(void *user, embi_str raw);
    embi_stop *stops;   /* scratch for the stops of a gradient (the frame copies them) */
    uint32_t nstops, capstops;
    embi_hashmap bitmaps;        /* W4i: bitmap colour glyph key -> embi_atlas_region * (text sink below) */
    uint32_t nbitmaps;
    SDL_GPUCommandBuffer *cmd;   /* the command buffer of the draw in progress (the glyph uploads go into it) */
    EMBI_VEC(embi_str) changed;  /* W6c: URLs (heap copies) of the animated images whose frame changed, until damaged */
    /* IF1: the textures of the iframes' documents ("ember-frame:" items), looked up by the iframe element. */
    SDL_GPUTexture *(*frame_tex)(void *user, const void *key, uint32_t *w, uint32_t *h);
    void *frame_user;
};

/* One frame stream: a target and the frame being recorded into it, with the index maps of that frame. */
typedef struct seg {
    embi_gfx *gx;
    const embi_dl *d;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *target;
    uint32_t tw, th;          /* the frame's size in device px */
    uint32_t cw, ch;          /* the content size in device px (the composite covers it) */
    float scale;
    bool region;
    embi_dev_rect rect;
    bool clear_pending;       /* the next end clears (the first frame of a target) */
    uint32_t clear_rgba;
    embi_frame *f;
    uint32_t *xmap, *cmap;    /* the frame's indices of the list's transforms and clips */
    uint32_t nx, nc;
    EMBI_VEC(embi_layer) pending; /* layers composited into the current frame: released when it ends */
    embi_gfx_stats *st;
    embi_gfx_group_fn group;
    uint32_t depth;
    bool cull;                /* skip the items whose bounds miss cull_rect (device px), when they have no transform */
    int32_t cull_rect[4];     /* x0, y0, x1, y1 */
    int32_t ox, oy;           /* a filter (R3a) or plane (R3c) layer: the target device px (ox, oy) is the texture's (0, 0) */
    uint32_t clip_base;       /* a filter layer: the list clip that the layer's composite applies, so the items inside
                               * are not clipped by it (nor by its parents) before the filter runs (R3a) */
} seg;

/* ------------------------------------------------------------------ images */

static void gpu_release(void *user, uint64_t handle);

static uint64_t gpu_upload(void *user, const uint8_t *px, uint32_t w, uint32_t h, SDL_GPUCommandBuffer *cmd)
{
    embi_gfx *gx = user;
    embi_atlas_region *reg = embi_alloc(sizeof *reg);
    memset(reg, 0, sizeof *reg);
    if (!embi_atlas_alloc(gx->atlas, w, h, reg)) {
        embi_free(reg, sizeof *reg);
        return 0;
    }
    if (!embi_atlas_upload(gx->atlas, reg, px, w * 4, cmd)) {
        embi_atlas_free(gx->atlas, reg);
        embi_free(reg, sizeof *reg);
        return 0;
    }
    return (uint64_t)(uintptr_t)reg;
}

static void gpu_release(void *user, uint64_t handle)
{
    embi_gfx *gx = user;
    embi_atlas_region *reg = (embi_atlas_region *)(uintptr_t)handle;
    if (reg != nullptr) {
        embi_atlas_free(gx->atlas, reg);
        embi_free(reg, sizeof *reg);
    }
}

/* ------------------------------------------------------------------ bitmap colour glyphs (W4i) */

/* The text system hands decoded CBDT/sbix glyph pixels to this sink (fonts.c registers it). They are uploaded to the
 * atlas once per key (face, glyph, strike) and drawn as an image quad, in the text's order with the other glyphs. */
#define EMBI_GFX_MAX_BITMAPS 1024u

void embi_fonts_set_bitmap_sink(embi_text *t,
                                bool (*fn)(void *, embi_frame *, const embi_pixels *, uint64_t, const float[4], uint32_t,
                                           uint32_t),
                                void *user);

static void bitmaps_release(embi_gfx *gx)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&gx->bitmaps);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        gpu_release(gx, v);
    }
    embi_hashmap_clear(&gx->bitmaps);
    gx->nbitmaps = 0;
}

static bool text_bitmap_sink(void *user, embi_frame *f, const embi_pixels *px, uint64_t key, const float rect[4],
                             uint32_t xform, uint32_t clip)
{
    embi_gfx *gx = user;
    if (gx->cmd == nullptr || px == nullptr || px->px == nullptr) {
        return false;
    }
    uint64_t h = 0;
    if (!embi_hashmap_get(&gx->bitmaps, key, &h)) {
        h = gpu_upload(gx, px->px, px->w, px->h, gx->cmd);
        if (h == 0) {
            return false;
        }
        embi_hashmap_put(&gx->bitmaps, key, h);
        gx->nbitmaps++;
    }
    static const float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const float square[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_image(f, rect, square, *(const embi_atlas_region *)(uintptr_t)h, full, 0xFFFFFFFFu, xform, clip);
    return true;
}

/* Requests and decodes url when it was never requested. */
static void fetch_into_cache(embi_gfx *gx, embi_str url)
{
    uint8_t *bytes = nullptr;
    size_t len = 0;
    uint32_t p0 = gx->pending_count != nullptr ? gx->pending_count(gx->user) : 0;
    bool fetched = gx->fetch != nullptr && gx->fetch(gx->user, url, &bytes, &len);
    if (!fetched && gx->pending_count != nullptr && gx->pending_count(gx->user) != p0) {
        return; /* a network image that has not arrived: not requested yet, not failed */
    }
    gx->image_gen++;
    if (fetched) {
        if (gx->svg_se != nullptr && gx->svgs != nullptr && embi_svgcache_add(gx->svgs, gx->svg_se, url, bytes, len)) {
            SDL_free(bytes); /* an SVG image is kept parsed, not decoded */
            return;
        }
        embi_image_cache_finish(gx->images, url, bytes, len);
        SDL_free(bytes);
    } else {
        embi_image_cache_fail(gx->images, url);
    }
}

void embi_gfx_set_style_engine(embi_gfx *gx, struct embi_style_engine *se)
{
    gx->svg_se = se;
}

const struct embi_svg_image *embi_gfx_svg_image(embi_gfx *gx, embi_str url)
{
    const struct embi_svg_image *si = embi_svgcache_get(gx->svgs, url);
    if (si != nullptr) {
        return si;
    }
    uint32_t iw = 0, ih = 0;
    if (embi_image_cache_query(gx->images, url, &iw, &ih) == EMBI_IMAGE_NONE) {
        fetch_into_cache(gx, url);
    }
    return embi_svgcache_get(gx->svgs, url);
}

void embi_gfx_svg_clear(embi_gfx *gx)
{
    if (gx != nullptr && gx->svgs != nullptr) {
        embi_svgcache_destroy(gx->svgs);
        gx->svgs = embi_svgcache_create();
    }
}

bool embi_gfx_image_size(embi_gfx *gx, embi_str url, float *w, float *h, bool *has_ratio)
{
    if (embi_svgcache_size(gx->svgs, url, w, h, has_ratio)) {
        return true;
    }
    uint32_t iw = 0, ih = 0;
    embi_image_state st = embi_image_cache_query(gx->images, url, &iw, &ih);
    if (st == EMBI_IMAGE_NONE) {
        fetch_into_cache(gx, url);
        if (embi_svgcache_size(gx->svgs, url, w, h, has_ratio)) {
            return true;
        }
        st = embi_image_cache_query(gx->images, url, &iw, &ih);
    }
    if (st != EMBI_IMAGE_READY) {
        return false;
    }
    *w = (float)iw;
    *h = (float)ih;
    *has_ratio = true;
    return true;
}

bool embi_gfx_image_pixels(embi_gfx *gx, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h)
{
    uint32_t iw = 0, ih = 0;
    embi_image_state st = embi_image_cache_query(gx->images, url, &iw, &ih);
    if (st == EMBI_IMAGE_NONE) {
        fetch_into_cache(gx, url);
        st = embi_image_cache_query(gx->images, url, &iw, &ih);
    }
    return st == EMBI_IMAGE_READY && embi_image_cache_pixels(gx->images, url, px, w, h);
}

uint32_t embi_gfx_image_generation(const embi_gfx *gx)
{
    return gx->image_gen;
}

embi_canvas_gpu *embi_gfx_canvas(embi_gfx *gx)
{
    return gx->canvas;
}

/* The atlas region of a raw URL of the list, uploaded for this frame's command buffer (its time goes to the upload
 * stat). False when it is not drawable. */
static bool image_region(seg *s, embi_str raw, embi_atlas_region *out)
{
    embi_gfx *gx = s->gx;
    embi_str url = gx->resolve != nullptr ? gx->resolve(gx->user, raw) : raw;
    uint32_t iw = 0, ih = 0;
    embi_image_state st = embi_image_cache_query(gx->images, url, &iw, &ih);
    if (st == EMBI_IMAGE_NONE) {
        fetch_into_cache(gx, url);
        st = embi_image_cache_query(gx->images, url, &iw, &ih);
    }
    if (st != EMBI_IMAGE_READY) {
        return false;
    }
    uint64_t t0 = SDL_GetPerformanceCounter();
    uint64_t handle = 0;
    bool ok = embi_image_cache_upload(gx->images, url, s->cmd, &handle) && handle != 0;
    s->st->upload_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (!ok) {
        return false;
    }
    *out = *(const embi_atlas_region *)(uintptr_t)handle;
    return true;
}

/* ------------------------------------------------------------------ colors and indices */

/* A color with its alpha multiplied by a. */
static uint32_t scale_rgba(uint32_t c, float a)
{
    if (a >= 1.0f) {
        return c;
    }
    uint32_t na = (uint32_t)lroundf((float)(c >> 24) * (a > 0.0f ? a : 0.0f));
    return (c & 0x00FFFFFFu) | (na << 24);
}

/* Whether the list's transform is the content transform of a scroll container that no transform around it moves: a pure
 * translation by the scroll offset, which a transform animation does not touch (text under it stays fitted). */
static bool scroll_still(const embi_dl *d, uint32_t dl_index)
{
    for (uint32_t guard = 0; guard < 16; guard++) {
        const embi_dscroll *n = nullptr;
        for (uint32_t i = 0; i < d->scrolls.len; i++) {
            if (d->scrolls.data[i].xform == dl_index) {
                n = &d->scrolls.data[i];
                break;
            }
        }
        if (n == nullptr) {
            return false;
        }
        if (n->outer == 0) {
            return true;
        }
        dl_index = n->outer;
    }
    return false;
}

static uint32_t frame_xform(seg *s, uint32_t dl_index)
{
    bool shifted = s->ox != 0 || s->oy != 0;
    if (dl_index == 0 && !shifted) {
        return 0;
    }
    if (dl_index >= s->nx) {
        return 0;
    }
    if (s->xmap[dl_index] == EMBI_GFX_UNMAPPED) {
        float m[6];
        memcpy(m, s->d->xforms.data[dl_index].m, sizeof m);
        if (shifted) {
            /* A layer that starts at (ox, oy) of the target: every transform also moves by minus that. */
            m[4] -= (float)s->ox / s->scale;
            m[5] -= (float)s->oy / s->scale;
        }
        s->xmap[dl_index] = embi_frame_xform(s->f, m);
        if (scroll_still(s->d, dl_index)) {
            embi_frame_xform_set_still(s->f, s->xmap[dl_index]);
        }
    }
    return s->xmap[dl_index];
}

/* The frame's clip of a list clip, creating its parent chain first. */
static uint32_t frame_clip(seg *s, uint32_t dl_index)
{
    if (dl_index == 0 || dl_index >= s->nc || dl_index == s->clip_base) {
        return 0;
    }
    if (s->cmap[dl_index] != EMBI_GFX_UNMAPPED) {
        return s->cmap[dl_index];
    }
    const embi_dclip *c = &s->d->clips.data[dl_index];
    uint32_t parent = c->parent < dl_index ? frame_clip(s, c->parent) : 0;
    uint32_t xf = frame_xform(s, c->xform);
    uint32_t idx = embi_frame_clip(s->f, c->rect, c->radii, xf, parent);
    s->cmap[dl_index] = idx;
    return idx;
}

/* ------------------------------------------------------------------ frames */

static void seg_begin(seg *s)
{
    if (s->region) {
        s->f = embi_frame_begin_region(s->gx->r, s->tw, s->th, s->scale, s->rect);
    } else {
        s->f = embi_frame_begin(s->gx->r, s->tw, s->th, s->scale);
    }
    if (s->f != nullptr) {
        /* LCD text needs an opaque backdrop in the same target: the root target when the canvas is
         * opaque. A layer starts transparent, so text in it is drawn gray. */
        embi_frame_set_text_aa(s->f, (embi_text_aa)s->gx->text_aa, s->gx->animating);
        embi_frame_set_opaque_backdrop(s->f, s->depth == 0 && (s->clear_rgba >> 24) == 0xFFu);
        memset(s->xmap, 0xFF, s->nx * sizeof *s->xmap);
        memset(s->cmap, 0xFF, s->nc * sizeof *s->cmap);
        if (s->xmap != nullptr && s->nx > 0 && s->ox == 0 && s->oy == 0) {
            s->xmap[0] = 0;
        }
    }
}

static void release_pending(seg *s)
{
    for (uint32_t i = 0; i < s->pending.len; i++) {
        embi_layer_release(s->gx->pool, &s->pending.data[i]);
    }
    s->pending.len = 0;
}

/* Ends the frame in progress (recording its passes into the command buffer). The layers it composited can be reused
 * from now on, since their composites are recorded before anything a later frame records. */
static void seg_end(seg *s)
{
    if (s->f != nullptr) {
        embi_frame_end(s->f, s->cmd, s->target, s->clear_pending, s->clear_rgba);
        s->st->draw_calls += embi_renderer_last_draw_calls(s->gx->r);
        s->st->batches += embi_renderer_last_batches(s->gx->r);
        s->clear_pending = false;
        s->f = nullptr;
    }
    release_pending(s);
}

/* ------------------------------------------------------------------ item drawing */

/* Copies n stops of the list (scaled by alpha) into the gradient scratch, and returns it. */
static embi_stop *gradient_stops(embi_gfx *gx, const embi_dl *d, uint32_t first, uint32_t n, float alpha)
{
    if (gx->capstops < n) {
        uint32_t cap = n > 16 ? n : 16;
        embi_stop *p = SDL_realloc(gx->stops, cap * sizeof *p);
        EMBI_ASSERT(p != nullptr);
        gx->stops = p;
        gx->capstops = cap;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_dstop *src = &d->stops.data[first + i];
        gx->stops[i].offset = src->offset;
        gx->stops[i].color = scale_rgba(src->color, alpha);
    }
    gx->nstops = n;
    return gx->stops;
}

static void draw_tiles(seg *s, const embi_ditem *it, const embi_atlas_region *reg, uint32_t tint, uint32_t xf,
                       uint32_t cl)
{
    float fx = it->f[0], fy = it->f[1], sw = it->f[2], sh = it->f[3], stx = it->f[4], sty = it->f[5];
    if (!(sw > 0.0f) || !(sh > 0.0f) || !(stx > 0.0f) || !(sty > 0.0f)) {
        return;
    }
    float x0 = it->rect[0], y0 = it->rect[1], x1 = x0 + it->rect[2], y1 = y0 + it->rect[3];
    const bool nn = (it->flags & EMBI_DF_NEAREST) != 0;
    /* The tiles that overlap the span: tile i covers [fx + i*stx, fx + i*stx + sw), so a tile that only touches the
     * span's edge is not drawn. */
    float ia = floorf((x0 - sw - fx) / stx) + 1.0f, ib = ceilf((x1 - fx) / stx) - 1.0f;
    float ja = floorf((y0 - sh - fy) / sty) + 1.0f, jb = ceilf((y1 - fy) / sty) - 1.0f;
    if (!(ib >= ia) || !(jb >= ja)) {
        return;
    }
    float count = (ib - ia + 1.0f) * (jb - ja + 1.0f);
    const float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float *w = it->widths;
    bool sub = w[2] > w[0] && w[3] > w[1]; /* a sub-rect of the image per tile, each tile clipped to the span */
    if (!(count <= (float)EMBI_GFX_MAX_TILES)) {
        /* Too many tiles to draw one by one. Contiguous tiles of a one texel image are all the same color: one quad over
         * the span (a 1x1 image is a common solid background, and tiled over a page it is millions of tiles). */
        if (reg->w == 1 && reg->h == 1 && stx == sw && sty == sh) {
            embi_frame_image_ex(s->f, it->rect, radii, *reg, uv, tint, xf, cl, nn);
        }
        return;
    }
    for (float j = ja; j <= jb; j += 1.0f) {
        for (float i = ia; i <= ib; i += 1.0f) {
            float rect[4] = {fx + i * stx, fy + j * sty, sw, sh};
            if (sub) {
                float ax = fmaxf(rect[0], x0), ay = fmaxf(rect[1], y0);
                float bx = fminf(rect[0] + sw, x1), by = fminf(rect[1] + sh, y1);
                if (!(bx > ax) || !(by > ay)) {
                    continue;
                }
                float t[4] = {w[0] + (ax - rect[0]) / sw * (w[2] - w[0]), w[1] + (ay - rect[1]) / sh * (w[3] - w[1]),
                              w[0] + (bx - rect[0]) / sw * (w[2] - w[0]), w[1] + (by - rect[1]) / sh * (w[3] - w[1])};
                float r[4] = {ax, ay, bx - ax, by - ay};
                embi_frame_image_ex(s->f, r, radii, *reg, t, tint, xf, cl, nn);
                continue;
            }
            embi_frame_image_ex(s->f, rect, radii, *reg, uv, tint, xf, cl, nn);
        }
    }
}

/* The top edge of a wave at vertex i: alternately at the centerline and one amplitude below it. */
static float wave_top(float yc, float amp, uint32_t i)
{
    return yc - amp / 2.0f + ((i & 1u) ? amp : 0.0f);
}

/* A straight edge as a quadratic curve (its control point at the midpoint). */
static embi_qcurve wave_line(float x0, float y0, float x1, float y1)
{
    return (embi_qcurve){x0, y0, (x0 + x1) / 2.0f, (y0 + y1) / 2.0f, x1, y1};
}

static void draw_decoration(seg *s, const embi_ditem *it, uint32_t rgba, uint32_t xf, uint32_t cl)
{
    float x = it->rect[0], y = it->rect[1], w = it->rect[2], h = it->rect[3];
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return;
    }
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (it->sub) {
    case EMBI_DECO_DOUBLE: {
        float t = floorf(h / 3.0f);
        if (t < 1.0f) {
            t = 1.0f;
        }
        float top[4] = {x, y, w, t}, bot[4] = {x, y + h - t, w, t};
        embi_frame_fill(s->f, top, zero, rgba, xf, cl);
        embi_frame_fill(s->f, bot, zero, rgba, xf, cl);
        break;
    }
    case EMBI_DECO_DOTTED:
    case EMBI_DECO_DASHED: {
        /* A dash or a gap is at least one device px (a thinner line would make an unbounded number of pieces), and the
         * count is capped: a line with more pieces than that is drawn solid. The loop is over an integer count, so it
         * always ends. */
        float unit = 1.0f / s->scale;
        bool dotted = it->sub == EMBI_DECO_DOTTED;
        float dash = fmaxf(dotted ? h : 3.0f * h, unit);
        float gap = fmaxf(dotted ? h : 3.0f * h, unit);
        float step = dash + gap;
        float off = fmodf(fmaxf(it->f[0], 0.0f), step); /* the pattern continues from the previous run of the line */
        float count = ceilf((w + off) / step);
        if (!(count <= (float)EMBI_GFX_MAX_DASHES)) {
            float r[4] = {x, y, w, h};
            embi_frame_fill(s->f, r, zero, rgba, xf, cl);
            break;
        }
        uint32_t n = (uint32_t)count;
        float dot = dotted ? h / 2.0f : 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            float p = x - off + (float)i * step;
            if (p < x - 0.001f) {
                continue; /* it started in the previous run of the line, which drew it whole */
            }
            /* A piece that starts here is drawn whole when the line goes on in a later run, so a dot is not cut. */
            float len = it->f[1] > 0.0f ? dash : fminf(dash, x + w - p);
            if (!(len > 0.0f)) {
                break;
            }
            float r[4] = {p, y, len, h};
            const float rr[4] = {dot, dot, dot, dot};
            embi_frame_fill(s->f, r, rr, rgba, xf, cl);
        }
        break;
    }
    case EMBI_DECO_WAVY: {
        /* A zigzag band. Its top edge and bottom edge are polylines, each segment a quadratic curve whose control point
         * is the midpoint (a straight line), joined into one closed outline. The outline depends only on the rect, so
         * its key is the hash of the rect. */
        float half = fmaxf(2.0f, h);
        uint32_t n = (uint32_t)ceilf(w / half);
        if (n < 1) {
            n = 1;
        }
        if (n > EMBI_GFX_MAX_WAVE) {
            n = EMBI_GFX_MAX_WAVE;
        }
        float step = w / (float)n;
        float amp = h;
        float yc = y + h / 2.0f;
        embi_qcurve *cv = SDL_malloc((2 * (size_t)n + 2) * sizeof *cv);
        EMBI_ASSERT(cv != nullptr);
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            cv[k++] = wave_line(x + (float)i * step, wave_top(yc, amp, i), x + (float)(i + 1) * step,
                                wave_top(yc, amp, i + 1));
        }
        cv[k++] = wave_line(x + w, wave_top(yc, amp, n), x + w, wave_top(yc, amp, n) + h);
        for (uint32_t i = n; i > 0; i--) {
            cv[k++] = wave_line(x + (float)i * step, wave_top(yc, amp, i) + h, x + (float)(i - 1) * step,
                                wave_top(yc, amp, i - 1) + h);
        }
        cv[k++] = wave_line(x, wave_top(yc, amp, 0) + h, x, wave_top(yc, amp, 0));
        float rect_key[4] = {x, y, w, h};
        uint64_t key = embi_hash_bytes(rect_key, sizeof rect_key, 0x7a7e) & ~(1ull << 63);
        const float to_local[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        embi_paint paint = embi_paint_solid(rgba);
        embi_frame_fill_quads(s->f, key, cv, k, to_local, &paint, EMBI_FILL_NONZERO, xf, cl);
        SDL_free(cv);
        break;
    }
    default: {
        float r[4] = {x, y, w, h};
        embi_frame_fill(s->f, r, zero, rgba, xf, cl);
        break;
    }
    }
}

static void draw_triangle(seg *s, const embi_ditem *it, uint32_t rgba, uint32_t xf, uint32_t cl)
{
    float x = it->rect[0], y = it->rect[1], w = it->rect[2], h = it->rect[3];
    float p[3][2];
    switch (it->sub) {
    case EMBI_TRI_DOWN:
        p[0][0] = x; p[0][1] = y; p[1][0] = x + w; p[1][1] = y; p[2][0] = x + w / 2.0f; p[2][1] = y + h;
        break;
    case EMBI_TRI_LEFT:
        p[0][0] = x + w; p[0][1] = y; p[1][0] = x; p[1][1] = y + h / 2.0f; p[2][0] = x + w; p[2][1] = y + h;
        break;
    case EMBI_TRI_UP:
        p[0][0] = x; p[0][1] = y + h; p[1][0] = x + w / 2.0f; p[1][1] = y; p[2][0] = x + w; p[2][1] = y + h;
        break;
    default:
        p[0][0] = x; p[0][1] = y; p[1][0] = x + w; p[1][1] = y + h / 2.0f; p[2][0] = x; p[2][1] = y + h;
        break;
    }
    embi_qcurve cv[3];
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        cv[i] = (embi_qcurve){p[i][0], p[i][1], (p[i][0] + p[j][0]) / 2.0f, (p[i][1] + p[j][1]) / 2.0f, p[j][0], p[j][1]};
    }
    uint64_t key = embi_hash_bytes(p, sizeof p, 0x7121) & ~(1ull << 63);
    const float to_local[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_paint paint = embi_paint_solid(rgba);
    embi_frame_fill_quads(s->f, key, cv, 3, to_local, &paint, EMBI_FILL_NONZERO, xf, cl);
}

/* A check mark (W5e): a six-point polygon in the item's rect. */
static void draw_check(seg *s, const embi_ditem *it, uint32_t rgba, uint32_t xf, uint32_t cl)
{
    static const float k[6][2] = {{0.12f, 0.55f}, {0.27f, 0.42f}, {0.42f, 0.58f},
                                  {0.76f, 0.18f}, {0.90f, 0.31f}, {0.42f, 0.86f}};
    float x = it->rect[0], y = it->rect[1], w = it->rect[2], h = it->rect[3];
    float p[6][2];
    for (int i = 0; i < 6; i++) {
        p[i][0] = x + k[i][0] * w;
        p[i][1] = y + k[i][1] * h;
    }
    embi_qcurve cv[6];
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        cv[i] = (embi_qcurve){p[i][0], p[i][1], (p[i][0] + p[j][0]) / 2.0f, (p[i][1] + p[j][1]) / 2.0f, p[j][0], p[j][1]};
    }
    uint64_t key = embi_hash_bytes(p, sizeof p, 0x7122) & ~(1ull << 63);
    const float to_local[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_paint paint = embi_paint_solid(rgba);
    embi_frame_fill_quads(s->f, key, cv, 6, to_local, &paint, EMBI_FILL_NONZERO, xf, cl);
}

/* Whether an item lies wholly outside the region the frame draws: its rect, mapped by its transform when it has one, grown
 * by the margin. Only the items the clip chain cannot hide are tested by their own bounds (the clip chain is not intersected:
 * a bound test per item is all the scan costs). */
static bool culled(const seg *s, const embi_ditem *it)
{
    if (!s->cull) {
        return false;
    }
    float m = EMBI_GFX_CULL_MARGIN * s->scale;
    float x0, y0, x1, y1;
    if (it->xform == 0) {
        x0 = it->rect[0] * s->scale - m;
        y0 = it->rect[1] * s->scale - m;
        x1 = (it->rect[0] + it->rect[2]) * s->scale + m;
        y1 = (it->rect[1] + it->rect[3]) * s->scale + m;
    } else {
        /* Layer segments (filters, planes) keep their transformed items: those live in the layer's own space. */
        if (s->depth != 0 || it->xform >= s->d->xforms.len) {
            return false;
        }
        const float *xm = s->d->xforms.data[it->xform].m;
        const float *r = it->rect;
        float lx0 = r[0], ly0 = r[1], lx1 = r[0] + r[2], ly1 = r[1] + r[3];
        if (!isfinite(lx0 + ly0 + lx1 + ly1) || !isfinite(xm[0] + xm[1] + xm[2] + xm[3] + xm[4] + xm[5])) {
            return false;
        }
        float px[4] = {lx0, lx1, lx0, lx1}, py[4] = {ly0, ly0, ly1, ly1};
        x0 = y0 = INFINITY;
        x1 = y1 = -INFINITY;
        for (int i = 0; i < 4; i++) {
            float x = xm[0] * px[i] + xm[2] * py[i] + xm[4];
            float y = xm[1] * px[i] + xm[3] * py[i] + xm[5];
            x0 = fminf(x0, x);
            y0 = fminf(y0, y);
            x1 = fmaxf(x1, x);
            y1 = fmaxf(y1, y);
        }
        x0 = x0 * s->scale - m;
        y0 = y0 * s->scale - m;
        x1 = x1 * s->scale + m;
        y1 = y1 * s->scale + m;
    }
    return x1 <= (float)s->cull_rect[0] || y1 <= (float)s->cull_rect[1] || x0 >= (float)s->cull_rect[2] ||
           y0 >= (float)s->cull_rect[3];
}

/* Draws one drawable item with its alpha multiplied by alpha. */
static float fx_scale_of(const seg *s, uint32_t xform);

static void draw_item(seg *s, const embi_ditem *it, float alpha)
{
    embi_gfx *gx = s->gx;
    uint32_t xf = frame_xform(s, it->xform);
    uint32_t cl = frame_clip(s, it->clip);
    switch (it->kind) {
    case EMBI_DI_FILL:
        embi_frame_fill(s->f, it->rect, it->radii, scale_rgba(it->colors[0], alpha), xf, cl);
        break;
    case EMBI_DI_BORDER: {
        uint32_t colors[4], styles[4];
        for (int i = 0; i < 4; i++) {
            colors[i] = scale_rgba(it->colors[i], alpha);
            styles[i] = it->styles[i];
        }
        embi_frame_border_styled(s->f, it->rect, it->radii, it->widths, colors, styles, xf, cl);
        break;
    }
    case EMBI_DI_IMAGE: {
        embi_atlas_region reg;
        embi_str curl = embi_dl_url(s->d, it->aux);
        if (curl.len > 13 && memcmp(curl.ptr, "ember-canvas:", 13) == 0) {
            /* R4b: a canvas element, drawn from its own texture (the version rides in aux2). */
            uint32_t cw = 0, ch = 0;
            char name[64];
            void *key = nullptr;
            size_t n = curl.len - 13 < sizeof name - 1 ? curl.len - 13 : sizeof name - 1;
            memcpy(name, (const char *)curl.ptr + 13, n);
            name[n] = '\0';
            (void)SDL_sscanf(name, "%p", &key);
            SDL_GPUTexture *ct = embi_canvas_gpu_texture(gx->canvas, key, &cw, &ch);
            if (ct != nullptr) {
                embi_layer lay = {.tex = ct, .w = cw, .h = ch, .tex_w = cw, .tex_h = ch,
                                  .nearest = (it->flags & EMBI_DF_NEAREST) != 0};
                const float csrc[4] = {0.0f, 0.0f, (float)cw, (float)ch};
                embi_frame_layer(s->f, &lay, csrc, it->rect, it->radii, alpha * (float)(it->colors[0] >> 24) / 255.0f, xf,
                                 cl);
            }
            break;
        }
        if (curl.len > 12 && memcmp(curl.ptr, "ember-frame:", 12) == 0) {
            /* IF1: an iframe, drawn from the texture its document was drawn into (premultiplied, transparent where the
             * document painted nothing). */
            uint32_t fw = 0, fh = 0;
            char name[64];
            void *key = nullptr;
            size_t n = curl.len - 12 < sizeof name - 1 ? curl.len - 12 : sizeof name - 1;
            memcpy(name, (const char *)curl.ptr + 12, n);
            name[n] = '\0';
            (void)SDL_sscanf(name, "%p", &key);
            SDL_GPUTexture *ft = gx->frame_tex != nullptr ? gx->frame_tex(gx->frame_user, key, &fw, &fh) : nullptr;
            if (ft != nullptr && fw > 0 && fh > 0) {
                embi_layer lay = {.tex = ft, .w = fw, .h = fh, .tex_w = fw, .tex_h = fh};
                const float fsrc[4] = {0.0f, 0.0f, (float)fw, (float)fh};
                /* 1:1 in device pixels: the texture is the frame's viewport at the view's scale, drawn at its own size
                 * from the content box's corner snapped to a device pixel (the item's clip is the content box), so a
                 * fractional box does not resample the frame's picture. */
                float dst[4] = {it->rect[0], it->rect[1], it->rect[2], it->rect[3]};
                if (s->scale > 0.0f) {
                    dst[0] = roundf(dst[0] * s->scale) / s->scale;
                    dst[1] = roundf(dst[1] * s->scale) / s->scale;
                    dst[2] = (float)fw / s->scale;
                    dst[3] = (float)fh / s->scale;
                }
                embi_frame_layer(s->f, &lay, fsrc, dst, it->radii, alpha * (float)(it->colors[0] >> 24) / 255.0f, xf, cl);
            }
            break;
        }
        if (!image_region(s, embi_dl_url(s->d, it->aux), &reg)) {
            break;
        }
        uint32_t tint = scale_rgba(it->colors[0], alpha);
        if (it->flags & EMBI_DF_TILE) {
            draw_tiles(s, it, &reg, tint, xf, cl);
        } else {
            embi_frame_image_ex(s->f, it->rect, it->radii, reg, it->f, tint, xf, cl, (it->flags & EMBI_DF_NEAREST) != 0);
        }
        break;
    }
    case EMBI_DI_GRADIENT: {
        embi_stop *st = gradient_stops(gx, s->d, it->aux, it->aux2, alpha);
        bool rep = (it->flags & EMBI_DF_REPEAT) != 0;
        switch (it->sub) {
        case EMBI_GRAD_RADIAL_DL:
            embi_frame_radial(s->f, it->rect, it->radii, it->f[0], it->f[1], it->f[2], it->f[3], st, gx->nstops, rep, xf, cl);
            break;
        case EMBI_GRAD_CONIC_DL:
            embi_frame_conic(s->f, it->rect, it->radii, it->f[0], it->f[1], it->f[2], st, gx->nstops, rep, xf, cl);
            break;
        default:
            embi_frame_linear(s->f, it->rect, it->radii, it->f[0], it->f[1], it->f[2], it->f[3], st, gx->nstops, rep, xf,
                              cl);
            break;
        }
        break;
    }
    case EMBI_DI_TEXT: {
        const embi_lay_shaped *run = (const embi_lay_shaped *)s->d->runs.data[it->aux];
        embi_frame_set_lcd_allowed(s->f, alpha >= 1.0f);
        embi_tshim_draw_range(gx->text, s->f, run, it->aux3, it->aux2, it->f[0], it->f[1], scale_rgba(it->colors[0], alpha), xf, cl);
        break;
    }
    case EMBI_DI_DECORATION:
        draw_decoration(s, it, scale_rgba(it->colors[0], alpha), xf, cl);
        break;
    case EMBI_DI_TRIANGLE:
        draw_triangle(s, it, scale_rgba(it->colors[0], alpha), xf, cl);
        break;
    case EMBI_DI_CHECK:
        draw_check(s, it, scale_rgba(it->colors[0], alpha), xf, cl);
        break;
    case EMBI_DI_SHADOW:
        if (it->aux < s->d->shadows.len) {
            const embi_dshadow *sh = &s->d->shadows.data[it->aux];
            embi_frame_shadow(s->f, it->rect, sh->shape, sh->shape_radii, sh->clip, sh->clip_radii, sh->sigma,
                              it->sub == EMBI_SHADOW_INSET, scale_rgba(it->colors[0], alpha), xf, cl);
        }
        break;
    case EMBI_DI_PATH: {
        /* R4c: an SVG shape or stroke: the path of the list filled with a solid color or a gradient. */
        if (it->aux >= s->d->paths.len || it->aux2 + it->aux3 > s->d->stops.len) {
            break;
        }
        embi_path *pp = s->d->paths.data[it->aux].path;
        embi_paint paint;
        memset(&paint, 0, sizeof paint);
        if (it->styles[0] == 0 || it->aux3 == 0) {
            paint = embi_paint_solid(scale_rgba(it->colors[0], alpha));
        } else {
            embi_stop *st = gradient_stops(gx, s->d, it->aux2, it->aux3, alpha);
            paint.kind = it->styles[0] == EMBI_GRAD_RADIAL_DL ? EMBI_PRIM_RADIAL : EMBI_PRIM_LINEAR;
            memcpy(paint.g, it->f, sizeof paint.g);
            paint.stops = st;
            paint.n_stops = gx->nstops;
            paint.repeat = (it->flags & EMBI_DF_REPEAT) != 0;
        }
        if ((it->flags & EMBI_DF_STROKE) != 0) {
            embi_stroke_style ss;
            memset(&ss, 0, sizeof ss);
            ss.width = it->f[0];
            ss.miter_limit = it->f[1];
            ss.cap = (embi_stroke_cap)it->styles[1];
            ss.join = (embi_stroke_join)it->styles[2];
            float sc = fx_scale_of(s, it->xform);
            ss.scale = sc > 1e-6f ? sc : 1.0f;
            embi_frame_stroke_path(s->f, pp, &ss, 0.2f / ss.scale, &paint, xf, cl);
        } else {
            embi_frame_fill_path(s->f, pp, it->sub == 1 ? EMBI_FILL_EVENODD : EMBI_FILL_NONZERO, &paint, xf, cl);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ replay */

static void replay_range(seg *s, uint32_t begin, uint32_t end, float alpha);
static void render_backdrop(seg *s, const embi_ditem *it, float alpha, SDL_GPUTexture *from_tex, int32_t from_ox,
                            int32_t from_oy, uint32_t from_w, uint32_t from_h);

/* Renders the group [begin, end) into a layer and composites it into the stream with opacity. With mask the layer is a
 * mask (R4c, EMBI_LS_SVGMASK): it multiplies what the stream holds by its alpha (destination-in) instead of drawing over it. */
static void render_group(seg *s, uint32_t begin, uint32_t end, float opacity, bool mask)
{
    embi_gfx *gx = s->gx;
    seg_end(s);
    embi_layer L;
    if (!embi_layer_acquire(gx->pool, s->cw, s->ch, &L)) {
        /* No layer: the items are drawn in place, which is the same picture without the group's isolation. A mask that
         * cannot be rendered leaves its content unmasked. */
        seg_begin(s);
        if (!mask) {
            replay_range(s, begin, end, opacity);
        }
        return;
    }
    seg ls = *s;
    ls.target = L.tex;
    ls.tw = L.tex_w;
    ls.th = L.tex_h;
    ls.region = false;
    ls.clear_pending = true;
    ls.clear_rgba = 0;
    ls.f = nullptr;
    ls.pending.data = nullptr;
    ls.pending.len = 0;
    ls.pending.cap = 0;
    ls.depth = s->depth + 1;
    /* The layer's own index maps: the same sizes, fresh arrays. */
    ls.xmap = SDL_malloc(s->nx * sizeof *ls.xmap);
    ls.cmap = SDL_malloc(s->nc * sizeof *ls.cmap);
    EMBI_ASSERT(ls.xmap != nullptr && ls.cmap != nullptr);
    seg_begin(&ls);
    replay_range(&ls, begin, end, 1.0f);
    seg_end(&ls);
    SDL_free(ls.xmap);
    SDL_free(ls.cmap);
    embi_vec_free(&ls.pending);

    seg_begin(s);
    if (s->f != nullptr) {
        const float src[4] = {0.0f, 0.0f, (float)s->cw, (float)s->ch};
        const float dst[4] = {0.0f, 0.0f, (float)s->cw / s->scale, (float)s->ch / s->scale};
        const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (mask) {
            embi_frame_set_blend(s->f, EMBI_BLEND_DSTIN);
        }
        embi_frame_layer(s->f, &L, src, dst, radii, opacity, 0, 0);
        if (mask) {
            embi_frame_set_blend(s->f, EMBI_BLEND_OVER);
        }
        embi_vec_push(&s->pending, L);
        s->st->layers++;
    } else {
        embi_layer_release(gx->pool, &L);
    }
}

/* ------------------------------------------------------------------ 3D planes (R3c) */

typedef struct pt2 {
    double x, y;
} pt2;

/* Keeps the part of the polygon where a x + b y + c >= 0. out has room for n + 1 vertices; returns the count. */
static int poly_keep(const pt2 *in, int n, double a, double b, double c, pt2 *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int k = (i + 1) % n;
        double fi = a * in[i].x + b * in[i].y + c, fk = a * in[k].x + b * in[k].y + c;
        if (fi >= 0.0) {
            out[m++] = in[i];
        }
        if ((fi > 0.0 && fk < 0.0) || (fi < 0.0 && fk > 0.0)) {
            double t = fi / (fi - fk);
            out[m++] = (pt2){in[i].x + (in[k].x - in[i].x) * t, in[i].y + (in[k].y - in[i].y) * t};
        }
    }
    return m;
}

/* The part of the plane that can be seen, and the scale to rasterize it at. The plane's bounds (local px) are cut to the
 * target window (vx0..vy1, in the space the plane is composed into) and to W > 0 through its homography h; every one of
 * those cuts is linear in the local point. The box of what is left is the layer's region (local px, out[4]), and *k the
 * device px per local px the plane takes on screen at its nearest visible point (largest singular value of the map's
 * Jacobian), within [1/16, 2] and exactly 1 between 0.75 and 1.25. False when nothing is visible. */
static bool plane_visible(const embi_dplane *pl, const float vwin[4], float out[4], float *k)
{
    const float *h = pl->h;
    pt2 a[24], b[24];
    int n = 4;
    a[0] = (pt2){pl->bounds[0], pl->bounds[1]};
    a[1] = (pt2){pl->bounds[2], pl->bounds[1]};
    a[2] = (pt2){pl->bounds[2], pl->bounds[3]};
    a[3] = (pt2){pl->bounds[0], pl->bounds[3]};
    n = poly_keep(a, n, h[6], h[7], h[8] - 1e-3, b);
    if (n < 3) {
        return false;
    }
    /* X - x0 W >= 0, x1 W - X >= 0, and the same for Y. */
    const double wx0 = vwin[0], wy0 = vwin[1], wx1 = vwin[2], wy1 = vwin[3];
    n = poly_keep(b, n, h[0] - wx0 * h[6], h[1] - wx0 * h[7], h[2] - wx0 * h[8], a);
    if (n >= 3) {
        n = poly_keep(a, n, wx1 * h[6] - h[0], wx1 * h[7] - h[1], wx1 * h[8] - h[2], b);
    }
    if (n >= 3) {
        n = poly_keep(b, n, h[3] - wy0 * h[6], h[4] - wy0 * h[7], h[5] - wy0 * h[8], a);
    }
    if (n >= 3) {
        n = poly_keep(a, n, wy1 * h[6] - h[3], wy1 * h[7] - h[4], wy1 * h[8] - h[5], b);
    }
    if (n < 3) {
        return false;
    }
    double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30, sig = 0.0;
    for (int i = 0; i < n; i++) {
        double x = b[i].x, y = b[i].y;
        mnx = fmin(mnx, x);
        mny = fmin(mny, y);
        mxx = fmax(mxx, x);
        mxy = fmax(mxy, y);
        double X = h[0] * x + h[1] * y + h[2], Y = h[3] * x + h[4] * y + h[5], W = h[6] * x + h[7] * y + h[8];
        if (!(W > 1e-6)) {
            sig = 1e9;
            continue;
        }
        double j00 = (h[0] * W - X * h[6]) / (W * W), j01 = (h[1] * W - X * h[7]) / (W * W);
        double j10 = (h[3] * W - Y * h[6]) / (W * W), j11 = (h[4] * W - Y * h[7]) / (W * W);
        double s1 = 0.5 * (j00 * j00 + j01 * j01 + j10 * j10 + j11 * j11);
        double det = j00 * j11 - j01 * j10;
        double s2 = sqrt(fmax(s1 * s1 - det * det, 0.0));
        sig = fmax(sig, sqrt(s1 + s2));
    }
    mnx = fmax(mnx, pl->bounds[0]);
    mny = fmax(mny, pl->bounds[1]);
    mxx = fmin(mxx, pl->bounds[2]);
    mxy = fmin(mxy, pl->bounds[3]);
    if (!(mxx > mnx) || !(mxy > mny)) {
        return false;
    }
    out[0] = (float)mnx;
    out[1] = (float)mny;
    out[2] = (float)mxx;
    out[3] = (float)mxy;
    /* Near native size the layer stays at native size: resampling a plane that is 0.75 to 1.25 times its size would blur edges
     * that look fine at 1, for a saving of nothing. */
    *k = (sig >= 0.75 && sig <= 1.25) ? 1.0f : (float)fmin(2.0, fmax(1.0 / 16.0, sig));
    return true;
}

/* Renders the plane (the LAYER_BEGIN at index bi, its items up to end) into a layer of its own and composites it with the
 * plane's homography (R3c). The layer covers the visible part of the plane only (plane_visible) at the scale the plane has
 * on screen, snapped to whole layer px; its items keep the space they were built in, with the layer's origin subtracted
 * from every transform (seg.ox, oy: x0, y0 at the layer's scale). */
static void render_plane(seg *s, uint32_t bi, uint32_t end)
{
    embi_gfx *gx = s->gx;
    const embi_ditem *bit = &s->d->items.data[bi];
    if (bit->aux2 >= s->d->planes.len) {
        return;
    }
    const embi_dplane pl = s->d->planes.data[bit->aux2];
    const float sc = s->scale;
    /* The target window in the space the plane is composed into, with a px of margin for the filtering at the edge. */
    const float ssx = (float)s->ox / sc, ssy = (float)s->oy / sc; /* this seg's origin in its items' space */
    const float vwin[4] = {ssx - 1.0f, ssy - 1.0f, ssx + (float)s->cw / sc + 1.0f, ssy + (float)s->ch / sc + 1.0f};
    float reg[4], k = 1.0f;
    if (!plane_visible(&pl, vwin, reg, &k)) {
        return;
    }
    /* The layer holds at most a few viewports of px, and no side is longer than 8192. */
    const double budget = 2.0 * (double)s->cw * (double)s->ch + 65536.0;
    float scl = sc * k;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    for (int iter = 0; iter < 24; iter++) {
        x0 = floorf(reg[0] * scl);
        y0 = floorf(reg[1] * scl);
        x1 = ceilf(reg[2] * scl);
        y1 = ceilf(reg[3] * scl);
        if ((double)(x1 - x0) <= 8192.0 && (double)(y1 - y0) <= 8192.0 &&
            (double)(x1 - x0) * (double)(y1 - y0) <= budget) {
            break;
        }
        scl *= 0.8f;
    }
    if (!(x1 > x0) || !(y1 > y0) || (x1 - x0) > 8192.0f || (y1 - y0) > 8192.0f || !(fabsf(x0) < 1e9f) || !(fabsf(y0) < 1e9f)) {
        return; /* nothing to draw, or an origin seg.ox, oy cannot hold */
    }
    uint32_t w = (uint32_t)(x1 - x0), h = (uint32_t)(y1 - y0);
    seg_end(s);
    embi_layer L;
    if (!embi_layer_acquire(gx->pool, w, h, &L)) {
        seg_begin(s);
        return;
    }
    seg ls = *s;
    ls.target = L.tex;
    ls.tw = L.tex_w;
    ls.th = L.tex_h;
    ls.cw = w;
    ls.ch = h;
    ls.region = false;
    ls.cull = false;
    ls.clear_pending = true;
    ls.clear_rgba = 0;
    ls.f = nullptr;
    ls.pending.data = nullptr;
    ls.pending.len = 0;
    ls.pending.cap = 0;
    ls.depth = s->depth + 1;
    ls.scale = scl;
    ls.ox = (int32_t)x0; /* x0, y0 are whole layer px within +-8192 * 2^k: exact */
    ls.oy = (int32_t)y0;
    ls.xmap = SDL_malloc((s->nx > 0 ? s->nx : 1) * sizeof *ls.xmap);
    ls.cmap = SDL_malloc((s->nc > 0 ? s->nc : 1) * sizeof *ls.cmap);
    EMBI_ASSERT(ls.xmap != nullptr && ls.cmap != nullptr);
    seg_begin(&ls);
    replay_range(&ls, bi + 1, end, 1.0f);
    seg_end(&ls);
    SDL_free(ls.xmap);
    SDL_free(ls.cmap);
    embi_vec_free(&ls.pending);

    seg_begin(s);
    if (s->f != nullptr) {
        /* The homography into this seg's space: subtract the seg's own origin from X and Y. */
        float hm[9];
        for (int i = 0; i < 9; i++) {
            hm[i] = pl.h[i];
        }
        for (int c = 0; c < 3; c++) {
            hm[c] -= ssx * pl.h[6 + c];
            hm[3 + c] -= ssy * pl.h[6 + c];
        }
        const float src[4] = {0.0f, 0.0f, (float)w, (float)h};
        const float dst[4] = {x0 / scl, y0 / scl, (float)w / scl, (float)h / scl};
        embi_frame_layer_proj(s->f, &L, src, dst, bit->f[0], hm, frame_clip(s, bit->clip));
        embi_vec_push(&s->pending, L);
        s->st->layers++;
    } else {
        embi_layer_release(gx->pool, &L);
    }
}


/* ------------------------------------------------------------------ filters (R3a) */

static embi_fx *gfx_fx(embi_gfx *gx)
{
    if (gx->fx == nullptr && !gx->fx_failed) {
        gx->fx = embi_fx_create(gx->g, gx->fmt, gx->pool);
        gx->fx_failed = gx->fx == nullptr;
    }
    return gx->fx;
}

/* An integer device rect, half open. */
typedef struct irect {
    int32_t x0, y0, x1, y1;
} irect;

static bool irect_empty(const irect *r)
{
    return r->x1 <= r->x0 || r->y1 <= r->y0;
}

static irect irect_clip(irect a, irect b)
{
    irect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
               a.y1 < b.y1 ? a.y1 : b.y1};
    return r;
}

static void map_bbox(const float m[6], const float r[4], float out[4])
{
    float xs[4] = {r[0], r[0] + r[2], r[0], r[0] + r[2]};
    float ys[4] = {r[1], r[1], r[1] + r[3], r[1] + r[3]};
    for (int i = 0; i < 4; i++) {
        float x = m[0] * xs[i] + m[2] * ys[i] + m[4];
        float y = m[1] * xs[i] + m[3] * ys[i] + m[5];
        if (i == 0) {
            out[0] = out[2] = x;
            out[1] = out[3] = y;
        } else {
            out[0] = fminf(out[0], x);
            out[1] = fminf(out[1], y);
            out[2] = fmaxf(out[2], x);
            out[3] = fmaxf(out[3], y);
        }
    }
}

/* The page px box of an item (its rect mapped by its transform) intersected with its clip chain up to the clip `stop`
 * (exclusive; 0 for the whole chain); x0, y0, x1, y1. */
static bool page_bounds(const embi_dl *d, const embi_ditem *it, bool clipped, uint32_t stop, float out[4])
{
    if (!(it->rect[2] > 0.0f) || !(it->rect[3] > 0.0f)) {
        return false;
    }
    static const float id[6] = {1, 0, 0, 1, 0, 0};
    map_bbox(it->xform < d->xforms.len ? d->xforms.data[it->xform].m : id, it->rect, out);
    if (!clipped) {
        return true;
    }
    for (uint32_t c = it->clip; c != 0 && c != stop && c < d->clips.len; c = d->clips.data[c].parent) {
        const embi_dclip *k = &d->clips.data[c];
        float b[4];
        map_bbox(k->xform < d->xforms.len ? d->xforms.data[k->xform].m : id, k->rect, b);
        out[0] = fmaxf(out[0], b[0]);
        out[1] = fmaxf(out[1], b[1]);
        out[2] = fminf(out[2], b[2]);
        out[3] = fminf(out[3], b[3]);
        if (!(out[2] > out[0]) || !(out[3] > out[1])) {
            return false;
        }
    }
    return true;
}

static float fx_scale_of(const seg *s, uint32_t xform);

static bool is_paint_item(uint8_t kind)
{
    return kind != EMBI_DI_LAYER_BEGIN && kind != EMBI_DI_LAYER_END;
}

/* The CSS px lengths of a filter turn into layer px by the scale of the filtered box's transform and the device scale. */
static float fx_scale_of(const seg *s, uint32_t xform)
{
    static const float id[6] = {1, 0, 0, 1, 0, 0};
    const float *m = xform < s->d->xforms.len ? s->d->xforms.data[xform].m : id;
    float det = fabsf(m[0] * m[3] - m[1] * m[2]);
    return sqrtf(det) * s->scale;
}

/* How far the filter steps spread ink, in layer px. */
static float fx_reach(const embi_dl *d, uint32_t first, uint32_t n, float k)
{
    float r = 0.0f;
    for (uint32_t i = 0; i < n && first + i < d->fx.len; i++) {
        const embi_dfx *x = &d->fx.data[first + i];
        if (x->kind == EMBI_FX_BLUR) {
            r += 3.0f * fminf(x->sigma, EMBI_FX_SIGMA_MAX) * k;
        } else if (x->kind == EMBI_FX_SHADOW) {
            r += (fminf(fmaxf(fabsf(x->dx), fabsf(x->dy)), 1e7f) + 3.0f * fminf(x->sigma, EMBI_FX_SIGMA_MAX)) * k;
        }
    }
    return ceilf(fminf(r, 4.0e6f)) + 1.0f;
}

/* The grid the blur pyramid of the steps [first, first + n) runs on, in layer px: a layer whose origin is a multiple of 2^k
 * (k levels for the largest sigma, at most 6) downsamples the same page texels whatever its offset, so two layers that hold
 * the same picture blur the same. 1 when no blur needs a pyramid. */
static int32_t fx_pyramid_grid(const embi_dl *d, uint32_t first, uint32_t n, float k)
{
    float smax = 0.0f;
    for (uint32_t i = 0; i < n && first + i < d->fx.len; i++) {
        const embi_dfx *x = &d->fx.data[first + i];
        if (x->kind == EMBI_FX_BLUR || x->kind == EMBI_FX_SHADOW) {
            smax = fmaxf(smax, fminf(x->sigma, EMBI_FX_SIGMA_MAX) * k);
        }
    }
    int32_t g = 1;
    while (smax > EMBI_FX_SIGMA_DIRECT && g < 64) {
        smax *= 0.5f;
        g *= 2;
    }
    return g;
}

/* Floor of v to a multiple of g (g > 0). */
static int32_t floor_to(int32_t v, int32_t g)
{
    int32_t r = v % g;
    return r == 0 ? v : (r > 0 ? v - r : v - r - g);
}

/* Applies the steps [first, first + n) to the layer *cur: the result replaces *cur (the layers in between come
 * from the pool and go back to it). False when a pass failed (the layer is then unspecified). */
static bool fx_apply(embi_gfx *gx, SDL_GPUCommandBuffer *cmd, const embi_dl *d, uint32_t first, uint32_t n, float k,
                     embi_layer *cur, bool clamp)
{
    embi_fx *fx = gfx_fx(gx);
    if (fx == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < n && first + i < d->fx.len; i++) {
        const embi_dfx *x = &d->fx.data[first + i];
        embi_layer out;
        if (!embi_layer_acquire(gx->pool, cur->w, cur->h, &out)) {
            return false;
        }
        bool ok = false;
        if (x->kind == EMBI_FX_BLUR) {
            ok = embi_fx_blur_edge(fx, cmd, cur, &out, x->sigma * k, clamp);
        } else if (x->kind == EMBI_FX_MATRIX) {
            ok = embi_fx_matrix(fx, cmd, cur, &out, x->m);
        } else if (x->kind == EMBI_FX_SHADOW) {
            embi_layer blurred;
            if (embi_layer_acquire(gx->pool, cur->w, cur->h, &blurred)) {
                ok = embi_fx_blur(fx, cmd, cur, &blurred, x->sigma * k) &&
                     embi_fx_drop_shadow(fx, cmd, cur, &blurred, &out, x->dx * k, x->dy * k, x->color);
                embi_layer_release(gx->pool, &blurred);
            }
        }
        embi_layer_release(gx->pool, cur);
        *cur = out;
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* The first index of the content of a group: a composite group holds its mask layers (EMBI_LS_MASK groups) first. */
static uint32_t content_first(const embi_dl *d, uint32_t begin, uint32_t end)
{
    const embi_ditem *b = &d->items.data[begin];
    uint32_t first = begin + 1;
    if (b->sub != EMBI_LS_COMPOSITE) {
        return first;
    }
    for (uint32_t m = 0; m < b->colors[2] && first < end; m++) {
        const embi_ditem *it = &d->items.data[first];
        if (it->kind != EMBI_DI_LAYER_BEGIN || it->sub != EMBI_LS_MASK || it->aux <= first || it->aux >= end) {
            break;
        }
        first = it->aux + 1;
    }
    return first;
}

/* A layer-sized stream: a seg that renders into the pooled layer L, whose pixel (0, 0) is the device px lay.(x0, y0) of the
 * parent stream s. The items inside keep the space they were built in. clip_base is the list clip the group's composite
 * applies (the items are not clipped by it nor by its parents). */
static void layer_seg_open(seg *ls, const seg *s, const embi_layer *L, const irect *lay, uint32_t clip_base)
{
    *ls = *s;
    ls->target = L->tex;
    ls->tw = L->tex_w;
    ls->th = L->tex_h;
    ls->cw = L->w;
    ls->ch = L->h;
    ls->ox = lay->x0;
    ls->oy = lay->y0;
    ls->clip_base = clip_base;
    ls->region = false;
    ls->clear_pending = true;
    ls->clear_rgba = 0;
    ls->f = nullptr;
    ls->pending.data = nullptr;
    ls->pending.len = 0;
    ls->pending.cap = 0;
    ls->depth = s->depth + 1;
    ls->cull = true;
    ls->cull_rect[0] = lay->x0;
    ls->cull_rect[1] = lay->y0;
    ls->cull_rect[2] = lay->x1;
    ls->cull_rect[3] = lay->y1;
    ls->xmap = SDL_malloc((s->nx > 0 ? s->nx : 1) * sizeof *ls->xmap);
    ls->cmap = SDL_malloc((s->nc > 0 ? s->nc : 1) * sizeof *ls->cmap);
    EMBI_ASSERT(ls->xmap != nullptr && ls->cmap != nullptr);
    seg_begin(ls);
}

static void layer_seg_close(seg *ls)
{
    seg_end(ls);
    SDL_free(ls->xmap);
    SDL_free(ls->cmap);
    embi_vec_free(&ls->pending);
}

/* Replaces *L by `out` (released *L). */
static void layer_swap(embi_gfx *gx, embi_layer *L, embi_layer *out)
{
    embi_layer_release(gx->pool, L);
    *L = *out;
}

/* Multiplies the layer *L by the coverage of the clip-path cp (R3b): the shape is drawn opaque white into a layer of its own
 * (the outline in the space of the group's transform) and the pass scales L by its alpha. */
static bool apply_clip_path(seg *s, const embi_ditem *b, const embi_dcpath *cp, const irect *lay, embi_layer *L)
{
    embi_gfx *gx = s->gx;
    embi_fx *fx = gfx_fx(gx);
    embi_layer C, out;
    if (fx == nullptr || !embi_layer_acquire(gx->pool, L->w, L->h, &C)) {
        return false;
    }
    seg cs;
    layer_seg_open(&cs, s, &C, lay, b->clip);
    if (cs.f != nullptr) {
        uint32_t xf = frame_xform(&cs, b->xform);
        if (cp->kind == EMBI_CP_RRECT) {
            embi_frame_fill(cs.f, cp->rect, cp->radii, 0xFFFFFFFFu, xf, 0);
        } else if (cp->kind == EMBI_CP_PATH && cp->n > 0 && cp->first + cp->n <= s->d->curves.len) {
            static_assert(sizeof(embi_dcurve) == sizeof(embi_qcurve), "the curve layouts match");
            const embi_qcurve *qc = (const embi_qcurve *)(const void *)&s->d->curves.data[cp->first];
            uint64_t key = embi_hash_bytes(qc, (size_t)cp->n * sizeof *qc, 0xC11Bu) | (1ull << 63);
            static const float id[6] = {1, 0, 0, 1, 0, 0};
            embi_paint white = embi_paint_solid(0xFFFFFFFFu);
            embi_frame_fill_quads(cs.f, key, qc, cp->n, id, &white, cp->evenodd ? EMBI_FILL_EVENODD : EMBI_FILL_NONZERO, xf, 0);
        }
    }
    layer_seg_close(&cs);
    bool ok = embi_layer_acquire(gx->pool, L->w, L->h, &out);
    if (ok) {
        ok = embi_fx_mask_apply(fx, s->cmd, L, &C, &out);
        if (ok) {
            layer_swap(gx, L, &out);
        } else {
            embi_layer_release(gx->pool, &out);
        }
    }
    embi_layer_release(gx->pool, &C);
    return ok;
}

/* Multiplies *L by the mask layers (R3b) of the group b: each layer is rendered into a layer, converted to its mask values
 * (the alpha, or the luminance), and the layers are combined from the bottom one up by their mask-composite operators. */
static bool apply_masks(seg *s, const embi_ditem *b, uint32_t begin, const irect *lay, embi_layer *L)
{
    embi_gfx *gx = s->gx;
    const embi_dl *d = s->d;
    embi_fx *fx = gfx_fx(gx);
    if (fx == nullptr) {
        return false;
    }
    uint32_t n = b->colors[2];
    uint32_t *starts = SDL_malloc((n > 0 ? n : 1) * sizeof *starts);
    EMBI_ASSERT(starts != nullptr);
    uint32_t cnt = 0, i = begin + 1;
    for (uint32_t m = 0; m < n; m++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind != EMBI_DI_LAYER_BEGIN || it->sub != EMBI_LS_MASK) {
            break;
        }
        starts[cnt++] = i;
        i = it->aux + 1;
    }
    embi_layer acc;
    bool have = false, ok = true;
    for (uint32_t m = cnt; m-- > 0 && ok;) {
        const embi_ditem *mit = &d->items.data[starts[m]];
        embi_layer M;
        if (!embi_layer_acquire(gx->pool, L->w, L->h, &M)) {
            ok = false;
            break;
        }
        seg ms;
        layer_seg_open(&ms, s, &M, lay, b->clip);
        replay_range(&ms, starts[m] + 1, mit->aux, 1.0f);
        layer_seg_close(&ms);
        if (mit->colors[0] == EMBI_MM_LUMINANCE) {
            embi_layer lum;
            if (embi_layer_acquire(gx->pool, L->w, L->h, &lum)) {
                if (embi_fx_mask_luminance(fx, s->cmd, &M, &lum)) {
                    layer_swap(gx, &M, &lum);
                } else {
                    embi_layer_release(gx->pool, &lum);
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        if (!ok) {
            embi_layer_release(gx->pool, &M);
            break;
        }
        if (!have) {
            acc = M; /* the bottom layer has nothing below it: its operator does not apply */
            have = true;
            continue;
        }
        embi_layer out;
        if (embi_layer_acquire(gx->pool, L->w, L->h, &out)) {
            ok = embi_fx_mask_combine(fx, s->cmd, &M, &acc, &out, mit->colors[1]);
            if (ok) {
                layer_swap(gx, &acc, &out);
            } else {
                embi_layer_release(gx->pool, &out);
            }
        } else {
            ok = false;
        }
        embi_layer_release(gx->pool, &M);
    }
    SDL_free(starts);
    if (!have) {
        return ok;
    }
    if (ok) {
        embi_layer out;
        if (embi_layer_acquire(gx->pool, L->w, L->h, &out)) {
            ok = embi_fx_mask_apply(fx, s->cmd, L, &acc, &out);
            if (ok) {
                layer_swap(gx, L, &out);
            } else {
                embi_layer_release(gx->pool, &out);
            }
        } else {
            ok = false;
        }
    }
    embi_layer_release(gx->pool, &acc);
    return ok;
}

/* Blends *L onto what the target of s holds under it (R3b): copies that region of the target (every frame of s so far has
 * been recorded), runs the blend pass and replaces L by the blended layer. vis is the part of the layer that is inside the
 * target. */
static bool apply_blend(seg *s, uint32_t mode, const irect *lay, const irect *vis, embi_layer *L)
{
    embi_gfx *gx = s->gx;
    embi_fx *fx = gfx_fx(gx);
    embi_layer bk, out;
    if (fx == nullptr || !embi_layer_acquire(gx->pool, L->w, L->h, &bk)) {
        return false;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s->cmd);
    if (cp == nullptr) {
        embi_layer_release(gx->pool, &bk);
        return false;
    }
    const SDL_GPUTextureLocation from = {
        .texture = s->target, .x = (Uint32)(vis->x0 - s->ox), .y = (Uint32)(vis->y0 - s->oy)};
    const SDL_GPUTextureLocation to = {
        .texture = bk.tex, .x = (Uint32)(vis->x0 - lay->x0), .y = (Uint32)(vis->y0 - lay->y0)};
    SDL_CopyGPUTextureToTexture(cp, &from, &to, (Uint32)(vis->x1 - vis->x0), (Uint32)(vis->y1 - vis->y0), 1, false);
    SDL_EndGPUCopyPass(cp);
    bool ok = embi_layer_acquire(gx->pool, L->w, L->h, &out);
    if (ok) {
        ok = embi_fx_blend(fx, s->cmd, L, &bk, &out, mode);
        if (ok) {
            layer_swap(gx, L, &out);
        } else {
            embi_layer_release(gx->pool, &out);
        }
    }
    embi_layer_release(gx->pool, &bk);
    return ok;
}

/* mix-blend-mode: plus-lighter (R3b): the sum of the layer (with the group's clip and opacity applied) and the target replaces
 * the target under the layer. Everything the composite would do (the clip, the opacity) is done on the layer first, because
 * the result is copied into the target and not drawn. */
static bool apply_plus(seg *s, const embi_ditem *b, const irect *lay, const irect *vis, float opacity, embi_layer *L)
{
    embi_gfx *gx = s->gx;
    embi_fx *fx = gfx_fx(gx);
    if (fx == nullptr) {
        return false;
    }
    if (b->clip != 0) {
        embi_layer C, out;
        if (!embi_layer_acquire(gx->pool, L->w, L->h, &C)) {
            return false;
        }
        seg cs;
        layer_seg_open(&cs, s, &C, lay, 0);
        if (cs.f != nullptr) {
            const float r[4] = {(float)lay->x0 / s->scale, (float)lay->y0 / s->scale, (float)(lay->x1 - lay->x0) / s->scale,
                                (float)(lay->y1 - lay->y0) / s->scale};
            const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t xf = frame_xform(&cs, 0);
            embi_frame_fill(cs.f, r, zero, 0xFFFFFFFFu, xf, frame_clip(&cs, b->clip));
        }
        layer_seg_close(&cs);
        bool ok = embi_layer_acquire(gx->pool, L->w, L->h, &out);
        if (ok) {
            ok = embi_fx_mask_apply(fx, s->cmd, L, &C, &out);
            if (ok) {
                layer_swap(gx, L, &out);
            } else {
                embi_layer_release(gx->pool, &out);
            }
        }
        embi_layer_release(gx->pool, &C);
        if (!ok) {
            return false;
        }
    }
    embi_layer bk, sum;
    if (!embi_layer_acquire(gx->pool, L->w, L->h, &bk)) {
        return false;
    }
    if (!embi_layer_acquire(gx->pool, L->w, L->h, &sum)) {
        embi_layer_release(gx->pool, &bk);
        return false;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s->cmd);
    bool ok = cp != nullptr;
    if (ok) {
        const SDL_GPUTextureLocation from = {
            .texture = s->target, .x = (Uint32)(vis->x0 - s->ox), .y = (Uint32)(vis->y0 - s->oy)};
        const SDL_GPUTextureLocation to = {
            .texture = bk.tex, .x = (Uint32)(vis->x0 - lay->x0), .y = (Uint32)(vis->y0 - lay->y0)};
        SDL_CopyGPUTextureToTexture(cp, &from, &to, (Uint32)(vis->x1 - vis->x0), (Uint32)(vis->y1 - vis->y0), 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    ok = ok && embi_fx_plus(fx, s->cmd, L, &bk, &sum, opacity);
    if (ok) {
        cp = SDL_BeginGPUCopyPass(s->cmd);
        ok = cp != nullptr;
        if (ok) {
            const SDL_GPUTextureLocation from = {
                .texture = sum.tex, .x = (Uint32)(vis->x0 - lay->x0), .y = (Uint32)(vis->y0 - lay->y0)};
            const SDL_GPUTextureLocation to = {
                .texture = s->target, .x = (Uint32)(vis->x0 - s->ox), .y = (Uint32)(vis->y0 - s->oy)};
            SDL_CopyGPUTextureToTexture(cp, &from, &to, (Uint32)(vis->x1 - vis->x0), (Uint32)(vis->y1 - vis->y0), 1, false);
            SDL_EndGPUCopyPass(cp);
        }
    }
    embi_layer_release(gx->pool, &bk);
    embi_layer_release(gx->pool, &sum);
    return ok;
}

/* Renders the group [begin + 1, end) (the group opened at index begin: filtered, or composite with a blend mode, a clip-path, a
 * mask or an isolation) into a layer sized to its ink, applies the filter steps, the clip-path, the mask and the opacity, and
 * composites the result with the group's clip, blended onto the target when the group has a blend mode. */
static void render_filter_group(seg *s, uint32_t begin, uint32_t end, float opacity)
{
    embi_gfx *gx = s->gx;
    const embi_dl *d = s->d;
    const embi_ditem *b = &d->items.data[begin];
    const bool comp = b->sub == EMBI_LS_COMPOSITE;
    const uint32_t blend = comp ? b->colors[0] : 0;
    const embi_dcpath *cpath = comp && b->colors[1] > 0 && b->colors[1] - 1 < d->cpaths.len ? &d->cpaths.data[b->colors[1] - 1]
                                                                                              : nullptr;
    if (cpath != nullptr && cpath->kind == EMBI_CP_EMPTY) {
        return; /* a clip-path with no area clips everything away */
    }
    const uint32_t cfirst = content_first(d, begin, end);
    float k = fx_scale_of(s, b->xform);
    float reach = fminf(fx_reach(d, b->aux2, b->aux3, k), 4.0f * (float)EMBI_LAYER_MAX_SIDE);

    /* The ink of the group in page px. */
    float u[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = cfirst; i < end; i++) {
        const embi_ditem *it = &d->items.data[i];
        float pb[4];
        if (!is_paint_item(it->kind) || !page_bounds(d, it, true, b->clip, pb)) {
            continue;
        }
        float m = it->kind == EMBI_DI_TEXT ? 0.25f * it->rect[3] + 1.0f : 1.0f;
        u[0] = fminf(u[0], pb[0] - m);
        u[1] = fminf(u[1], pb[1] - m);
        u[2] = fmaxf(u[2], pb[2] + m);
        u[3] = fmaxf(u[3], pb[3] + m);
    }
    if (!(u[2] > u[0]) || !(u[3] > u[1])) {
        return;
    }
    const irect view = {s->ox, s->oy, s->ox + (int32_t)s->cw, s->oy + (int32_t)s->ch};
    irect ink = {(int32_t)floorf(fmaxf(u[0] * s->scale, -1e8f)), (int32_t)floorf(fmaxf(u[1] * s->scale, -1e8f)),
                 (int32_t)ceilf(fminf(u[2] * s->scale, 1e8f)), (int32_t)ceilf(fminf(u[3] * s->scale, 1e8f))};
    /* reach <= 4 * MAX_SIDE and every operand is within +-1e8 plus a view size, so int32 cannot overflow. */
    const int32_t rc = (int32_t)reach;
    irect grown = {ink.x0 - rc, ink.y0 - rc, ink.x1 + rc, ink.y1 + rc};
    irect vis = irect_clip(grown, view);
    irect big = {view.x0 - rc, view.y0 - rc, view.x1 + rc, view.y1 + rc};
    irect lay = irect_clip(grown, big);
    if (irect_empty(&vis) || irect_empty(&lay)) {
        return;
    }
    {
        /* The pyramid grid follows the page: the origin of the layer is a multiple of its factor. */
        const int32_t g = fx_pyramid_grid(d, b->aux2, b->aux3, k);
        if (g > 1) {
            irect snapped = lay;
            snapped.x0 = floor_to(lay.x0, g);
            snapped.y0 = floor_to(lay.y0, g);
            if (snapped.x1 - snapped.x0 <= (int32_t)EMBI_LAYER_MAX_SIDE &&
                snapped.y1 - snapped.y0 <= (int32_t)EMBI_LAYER_MAX_SIDE) {
                lay = snapped;
            }
        }
    }
    if (lay.x1 - lay.x0 > (int32_t)EMBI_LAYER_MAX_SIDE || lay.y1 - lay.y0 > (int32_t)EMBI_LAYER_MAX_SIDE) {
        lay = vis;
    }
    seg_end(s);
    embi_layer L;
    if (!embi_layer_acquire(gx->pool, (uint32_t)(lay.x1 - lay.x0), (uint32_t)(lay.y1 - lay.y0), &L)) {
        /* No layer: the items are drawn in place, the same picture without the filter. */
        seg_begin(s);
        replay_range(s, cfirst, end, opacity);
        return;
    }
    seg ls;
    layer_seg_open(&ls, s, &L, &lay, b->clip);
    uint32_t first = cfirst;
    /* A backdrop-filter of the filtered element itself is part of what the filter applies to: it reads the target
     * the group is drawn into (the parent's), then the layer holds its result. */
    for (; first < end && d->items.data[first].kind == EMBI_DI_BACKDROP; first++) {
        if (ls.f != nullptr && !culled(&ls, &d->items.data[first])) {
            render_backdrop(&ls, &d->items.data[first], 1.0f, s->target, s->ox, s->oy, s->cw, s->ch);
        }
    }
    replay_range(&ls, first, end, 1.0f);
    layer_seg_close(&ls);
    s->st->layers++;

    bool ok = fx_apply(gx, s->cmd, d, b->aux2, b->aux3, k, &L, false);
    if (ok && cpath != nullptr) {
        ok = apply_clip_path(s, b, cpath, &lay, &L);
        s->st->layers++;
    }
    if (ok && comp && b->colors[2] > 0) {
        ok = apply_masks(s, b, begin, &lay, &L);
        s->st->layers += b->colors[2];
    }
    if (ok && blend == EMBI_BM_PLUS_LIGHTER) {
        ok = apply_plus(s, b, &lay, &vis, opacity, &L);
        s->st->layers += 2;
        seg_begin(s);
        embi_layer_release(gx->pool, &L);
        return;
    }
    if (ok && blend != EMBI_BM_NORMAL) {
        ok = apply_blend(s, blend, &lay, &vis, &L);
        s->st->layers++;
    }
    seg_begin(s);
    if (s->f == nullptr || !ok || L.tex == nullptr) {
        embi_layer_release(gx->pool, &L);
        return;
    }
    const float src[4] = {(float)(vis.x0 - lay.x0), (float)(vis.y0 - lay.y0), (float)(vis.x1 - vis.x0),
                          (float)(vis.y1 - vis.y0)};
    const float dst[4] = {(float)vis.x0 / s->scale, (float)vis.y0 / s->scale, (float)(vis.x1 - vis.x0) / s->scale,
                          (float)(vis.y1 - vis.y0) / s->scale};
    const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_layer(s->f, &L, src, dst, radii, opacity, frame_xform(s, 0), frame_clip(s, b->clip));
    embi_vec_push(&s->pending, L);
}

/* backdrop-filter: copies what the target holds under the border box (clipped to the view), filters the copy and
 * draws it clipped to the rounded border box. The frame in progress ends first (the copy needs the pixels drawn so
 * far), so each backdrop-filter box splits the frame in two. The backdrop image is the box itself and its edges repeat
 * outward (edge mode duplicate), so the blur needs no margin.
 *
 * Cost per box: one texture copy of the box, the filter passes over a box-sized layer (a blur is two passes, plus a
 * 2x pyramid of 4/3 of a pass above sigma 8 px), one rounded composite and a frame break. Nothing is paid by boxes
 * without a backdrop-filter (the display list holds no item for them). from_tex is the texture the backdrop is read
 * from: the target of s, or the parent's target for the filter of the element a group is drawn for. */
static void render_backdrop(seg *s, const embi_ditem *it, float alpha, SDL_GPUTexture *from_tex, int32_t from_ox,
                            int32_t from_oy, uint32_t from_w, uint32_t from_h)
{
    embi_gfx *gx = s->gx;
    const embi_dl *d = s->d;
    float k = fx_scale_of(s, it->xform);
    float reach = fx_reach(d, it->aux2, it->aux3, k);
    float pb[4];
    if (!page_bounds(d, it, false, 0, pb)) {
        return;
    }
    const irect view = {s->ox, s->oy, s->ox + (int32_t)s->cw, s->oy + (int32_t)s->ch};
    irect box = {(int32_t)floorf(fmaxf(pb[0] * s->scale, -1e8f)), (int32_t)floorf(fmaxf(pb[1] * s->scale, -1e8f)),
                 (int32_t)ceilf(fminf(pb[2] * s->scale, 1e8f)), (int32_t)ceilf(fminf(pb[3] * s->scale, 1e8f))};
    /* The source texture may be smaller than the view (the parent of a filter layer): never read outside it. */
    const irect srcv = {from_ox, from_oy, from_ox + (int32_t)from_w, from_oy + (int32_t)from_h};
    irect vis = irect_clip(irect_clip(box, view), srcv);
    /* The backdrop image is the border box only and its edges repeat outward (edge mode duplicate), so nothing
     * outside the box is read. */
    (void)reach;
    irect lay = vis;
    if (irect_empty(&vis) || irect_empty(&lay)) {
        return;
    }
    seg_end(s);
    embi_layer L;
    if (!embi_layer_acquire(gx->pool, (uint32_t)(lay.x1 - lay.x0), (uint32_t)(lay.y1 - lay.y0), &L)) {
        seg_begin(s);
        return;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s->cmd);
    if (cp != nullptr) {
        const SDL_GPUTextureLocation from = {
            .texture = from_tex, .x = (Uint32)(lay.x0 - from_ox), .y = (Uint32)(lay.y0 - from_oy)};
        const SDL_GPUTextureLocation to = {.texture = L.tex};
        SDL_CopyGPUTextureToTexture(cp, &from, &to, L.w, L.h, 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    s->st->layers++;
    bool ok = cp != nullptr && fx_apply(gx, s->cmd, d, it->aux2, it->aux3, k, &L, true);
    seg_begin(s);
    if (s->f == nullptr || !ok || L.tex == nullptr) {
        embi_layer_release(gx->pool, &L);
        return;
    }
    uint32_t xf = frame_xform(s, it->xform);
    uint32_t ck = embi_frame_clip(s->f, it->rect, it->radii, xf, frame_clip(s, it->clip));
    const float src[4] = {(float)(vis.x0 - lay.x0), (float)(vis.y0 - lay.y0), (float)(vis.x1 - vis.x0),
                          (float)(vis.y1 - vis.y0)};
    const float dst[4] = {(float)vis.x0 / s->scale, (float)vis.y0 / s->scale, (float)(vis.x1 - vis.x0) / s->scale,
                          (float)(vis.y1 - vis.y0) / s->scale};
    const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_layer(s->f, &L, src, dst, radii, alpha, frame_xform(s, 0), ck);
    embi_vec_push(&s->pending, L);
}

static void replay_range(seg *s, uint32_t begin, uint32_t end, float alpha)
{
    const embi_dl *d = s->d;
    if (end > d->items.len) {
        end = d->items.len;
    }
    for (uint32_t i = begin; i < end;) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN) {
            uint32_t j = it->aux;
            if (j <= i || j >= d->items.len || d->items.data[j].kind != EMBI_DI_LAYER_END) {
                i++;
                continue;
            }
            if (it->sub == EMBI_LS_PLANE) {
                if (s->f != nullptr && s->depth < EMBI_GFX_MAX_LAYER_DEPTH) {
                    render_plane(s, i, j);
                }
                i = j + 1;
                continue;
            }
            float opacity = alpha * it->f[0];
            if (it->sub == EMBI_LS_MASK) {
                i = j + 1; /* the mask layers are read by their composite group, never drawn */
                continue;
            }
            if (!(opacity > 0.0f) && it->sub != EMBI_LS_SVGMASK) {
                /* A group drawn at opacity 0 (its filters, blend mode and backdrop-filter included) shows nothing, so it
                 * is skipped rather than given a layer. TodoMVC's checkboxes are opacity: 0 and a checked one holds a
                 * check mark, which cannot fold into the group's alpha: each got a layer, and the first frame with 100
                 * of them took about 800 ms on Direct3D 12 (SDL_gpu creates new buffers for every extra pass of a
                 * command buffer). An SVG mask group always draws: it multiplies what is under it. */
                i = j + 1;
                continue;
            }
            if (it->sub == EMBI_LS_ISO || it->sub == EMBI_LS_SVGMASK) {
                /* R4c: an isolated group of an SVG element, and the mask group inside it. Neither goes through the group
                 * callback: they always render into a layer (when none is left, the content is drawn in place). */
                if (s->depth >= EMBI_GFX_MAX_LAYER_DEPTH || s->f == nullptr) {
                    if (it->sub == EMBI_LS_ISO) {
                        replay_range(s, i + 1, j, opacity);
                    }
                } else if (it->sub == EMBI_LS_ISO) {
                    if (j > i + 1) {
                        render_group(s, i + 1, j, opacity, false);
                    }
                } else {
                    render_group(s, i + 1, j, 1.0f, true);
                }
                i = j + 1;
                continue;
            }
            if (it->sub == EMBI_LAYER_FILTERED || it->sub == EMBI_LS_COMPOSITE) {
                if (s->depth >= EMBI_GFX_MAX_LAYER_DEPTH || s->f == nullptr) {
                    replay_range(s, content_first(d, i, j), j, opacity);
                } else {
                    render_filter_group(s, i, j, opacity);
                }
                i = j + 1;
                continue;
            }
            /* The backdrop-filter of the grouped element itself reads what is under the group, then the group's
             * opacity applies to its result: it is drawn here with the group's alpha, outside the group. */
            uint32_t first = i + 1;
            for (; first < j && d->items.data[first].kind == EMBI_DI_BACKDROP; first++) {
                if (s->f != nullptr && s->depth < EMBI_GFX_MAX_LAYER_DEPTH && !culled(s, &d->items.data[first])) {
                    render_backdrop(s, &d->items.data[first], opacity, s->target, s->ox, s->oy, s->cw, s->ch);
                }
            }
            int mode = s->group != nullptr ? s->group(d, i, j) : 1;
            if (mode == 0) {
                /* Empty: nothing to draw. */
            } else if (mode == 1 || s->depth >= EMBI_GFX_MAX_LAYER_DEPTH || s->f == nullptr) {
                replay_range(s, first, j, opacity);
            } else {
                render_group(s, first, j, opacity, false);
            }
            i = j + 1;
            continue;
        }
        if (it->kind == EMBI_DI_BACKDROP) {
            if (s->f != nullptr && s->depth < EMBI_GFX_MAX_LAYER_DEPTH && !culled(s, it)) {
                render_backdrop(s, it, alpha, s->target, s->ox, s->oy, s->cw, s->ch);
            }
        } else if (it->kind != EMBI_DI_LAYER_END && s->f != nullptr) {
            if (culled(s, it)) {
                s->st->items_culled++;
            } else {
                s->st->items_recorded++;
                draw_item(s, it, alpha);
            }
        }
        i++;
    }
}

/* Whether the list has a blend group (R3b): the page then draws onto a transparent target and the canvas color goes under it
 * afterwards, because the backdrop of the root element group is transparent (a blend mode does not see the canvas color). */
static bool has_blend_group(const embi_dl *d)
{
    if (d->fx_items == 0) {
        return false;
    }
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE && it->colors[0] != EMBI_BM_NORMAL) {
            return true;
        }
    }
    return false;
}

/* Puts the canvas color under what the target holds: the target is copied into a layer, cleared to the color, and the layer is
 * composited over it. */
static void canvas_under(seg *s, uint32_t canvas_rgba)
{
    embi_gfx *gx = s->gx;
    embi_layer L;
    if (!embi_layer_acquire(gx->pool, s->cw, s->ch, &L)) {
        return;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s->cmd);
    if (cp != nullptr) {
        const SDL_GPUTextureLocation from = {.texture = s->target};
        const SDL_GPUTextureLocation to = {.texture = L.tex};
        SDL_CopyGPUTextureToTexture(cp, &from, &to, s->cw, s->ch, 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    s->clear_pending = true;
    s->clear_rgba = canvas_rgba;
    s->region = false;
    seg_begin(s);
    if (s->f != nullptr) {
        const float src[4] = {0.0f, 0.0f, (float)s->cw, (float)s->ch};
        const float dst[4] = {0.0f, 0.0f, (float)s->cw / s->scale, (float)s->ch / s->scale};
        const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        embi_frame_layer(s->f, &L, src, dst, radii, 1.0f, 0, 0);
        embi_vec_push(&s->pending, L);
    } else {
        embi_layer_release(gx->pool, &L);
    }
    seg_end(s);
}

/* Draws the list into the target (see gfx.h). */
bool embi_gfx_draw(embi_gfx *gx, SDL_GPUCommandBuffer *cmd, const embi_gfx_target *t, const embi_dl *d,
                   const embi_damage *dm, embi_gfx_group_fn group, embi_gfx_stats *st)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (!dm->any || t->w == 0 || t->h == 0) {
        return true;
    }
    gx->text_aa = t->text_aa;
    gx->animating = t->animating;
    gx->cmd = cmd;
    if (gx->canvas != nullptr) {
        embi_canvas_gpu_flush(gx->canvas, cmd); /* the canvases' recorded drawing, before any frame of the view */
    }
    if (gx->nbitmaps > EMBI_GFX_MAX_BITMAPS) {
        bitmaps_release(gx); /* between frames: the regions of the last frame are not in use any more */
    }
    seg s;
    memset(&s, 0, sizeof s);
    s.gx = gx;
    s.d = d;
    s.cmd = cmd;
    s.target = t->tex;
    s.tw = t->w;
    s.th = t->h;
    s.cw = t->w;
    s.ch = t->h;
    s.scale = t->scale;
    s.clear_rgba = t->clear_rgba;
    s.st = st;
    s.group = group;
    s.nx = d->xforms.len;
    s.nc = d->clips.len;
    s.xmap = SDL_malloc((s.nx > 0 ? s.nx : 1) * sizeof *s.xmap);
    s.cmap = SDL_malloc((s.nc > 0 ? s.nc : 1) * sizeof *s.cmap);
    EMBI_ASSERT(s.xmap != nullptr && s.cmap != nullptr);
    bool ok = true;
    /* A region frame needs the replacing pipeline; without it the damage is repainted as a full frame. */
    const bool as_full = dm->full || !embi_renderer_region_ok(gx->r);
    if (as_full) {
        s.region = false;
        s.clear_pending = true;
        /* The canvas color goes under the page by copying the target into a layer, which exists only up to
         * EMBI_LAYER_MAX_SIDE: a larger target keeps the opaque clear (blend modes then see the canvas color). */
        const bool under = (t->clear_rgba >> 24) != 0 && has_blend_group(d) && t->w <= EMBI_LAYER_MAX_SIDE &&
                           t->h <= EMBI_LAYER_MAX_SIDE;
        if (under) {
            s.clear_rgba = 0;
        }
        /* Only what is on screen is recorded: the items whose bounds miss the target are skipped, so a long page costs
         * what its visible part costs. */
        s.cull = true;
        s.cull_rect[0] = 0;
        s.cull_rect[1] = 0;
        s.cull_rect[2] = (int32_t)t->w;
        s.cull_rect[3] = (int32_t)t->h;
        seg_begin(&s);
        if (s.f != nullptr) {
            replay_range(&s, 0, d->items.len, 1.0f);
        } else {
            ok = false;
        }
        seg_end(&s);
        s.cull = false;
        if (under && ok) {
            canvas_under(&s, t->clear_rgba);
        }
    } else {
        /* One region frame over the union of the damage rects: the list is replayed once (so its layers are rendered
         * once per frame) and the items outside the union are culled. The pixels of the union that lie outside the
         * rects are repainted from the same list, so they come out as they were. */
        bool any = false;
        int32_t u[4] = {0, 0, 0, 0};
        for (uint32_t k = 0; k < dm->rects.len; k++) {
            const embi_drect *q = &dm->rects.data[k];
            if (q->x1 <= q->x0 || q->y1 <= q->y0) {
                continue;
            }
            if (!any) {
                u[0] = q->x0;
                u[1] = q->y0;
                u[2] = q->x1;
                u[3] = q->y1;
                any = true;
            } else {
                u[0] = q->x0 < u[0] ? q->x0 : u[0];
                u[1] = q->y0 < u[1] ? q->y0 : u[1];
                u[2] = q->x1 > u[2] ? q->x1 : u[2];
                u[3] = q->y1 > u[3] ? q->y1 : u[3];
            }
        }
        if (any) {
            s.region = true;
            s.rect = (embi_dev_rect){u[0], u[1], u[2] - u[0], u[3] - u[1]};
            s.cull = true;
            s.cull_rect[0] = u[0];
            s.cull_rect[1] = u[1];
            s.cull_rect[2] = u[2];
            s.cull_rect[3] = u[3];
            s.clear_pending = true;
            seg_begin(&s);
            if (s.f == nullptr) {
                ok = false;
            } else {
                replay_range(&s, 0, d->items.len, 1.0f);
            }
            seg_end(&s);
            s.cull = false;
        }
    }
    SDL_free(s.xmap);
    SDL_free(s.cmap);
    embi_vec_free(&s.pending);
    st->record_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    return ok;
}

bool embi_gfx_blit(embi_gfx *gx, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t sw, uint32_t sh,
                   SDL_GPUTexture *dst, uint32_t dw, uint32_t dh)
{
    embi_layer L;
    memset(&L, 0, sizeof L);
    L.tex = src;
    L.w = sw;
    L.h = sh;
    L.tex_w = sw;
    L.tex_h = sh;
    embi_frame *f = embi_frame_begin(gx->r, dw, dh, 1.0f);
    if (f == nullptr) {
        return false;
    }
    const float srcr[4] = {0.0f, 0.0f, (float)sw, (float)sh};
    const float dstr[4] = {0.0f, 0.0f, (float)dw, (float)dh};
    const float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_layer(f, &L, srcr, dstr, radii, 1.0f, 0, 0);
    embi_frame_end(f, cmd, dst, true, 0);
    return true;
}

/* ------------------------------------------------------------------ lifetime */

embi_gfx *embi_gfx_create(const embi_gfx_cfg *cfg)
{
    embi_gfx *gx = embi_alloc(sizeof *gx);
    memset(gx, 0, sizeof *gx);
    gx->g = cfg->gpu;
    gx->dev = embi_gpu_device(cfg->gpu);
    gx->fmt = cfg->format;
    gx->text = cfg->text;
    embi_hashmap_init(&gx->bitmaps);
    gx->user = cfg->user;
    gx->fetch = cfg->fetch;
    gx->pending_count = cfg->pending_count;
    gx->resolve = cfg->resolve;
    gx->r = embi_renderer_create(cfg->gpu, cfg->format);
    gx->atlas = embi_atlas_create(gx->dev);
    size_t layer_bytes = cfg->layer_bytes != 0 ? cfg->layer_bytes : EMBI_GFX_DEFAULT_BUDGET;
    embi_renderer_warm(gx->r); /* the first frame's buffers are made while the page loads */
    gx->pool = embi_layer_pool_create(cfg->gpu, cfg->format, layer_bytes);
    size_t image_bytes = cfg->image_bytes != 0 ? cfg->image_bytes : EMBI_GFX_DEFAULT_BUDGET;
    gx->image_bytes = image_bytes;
    gx->svgs = embi_svgcache_create();
    gx->images = embi_image_cache_create(image_bytes);
    gx->canvas = embi_canvas_gpu_create(gx->g, gx->r, gx->pool, (int)cfg->format);
    if (gx->r == nullptr || gx->atlas == nullptr || gx->pool == nullptr || gx->images == nullptr) {
        embi_gfx_destroy(gx);
        return nullptr;
    }
    embi_image_gpu hook = {gx, gpu_upload, gpu_release};
    embi_image_cache_set_gpu(gx->images, &hook);
    if (gx->text != nullptr) {
        embi_fonts_set_bitmap_sink(gx->text, text_bitmap_sink, gx);
    }
    return gx;
}

/* Sets the text engine of a renderer made with cfg.text == nullptr and hooks its bitmap sink. It is split from
 * embi_gfx_create so that the creation can run on a thread while the caller builds the text-side state. */
void embi_gfx_attach_text(embi_gfx *gx, embi_text *text)
{
    if (gx == nullptr || gx->text != nullptr || text == nullptr) {
        return;
    }
    gx->text = text;
    embi_fonts_set_bitmap_sink(gx->text, text_bitmap_sink, gx);
}

static void anim_clear(embi_gfx *gx);

void embi_gfx_destroy(embi_gfx *gx)
{
    if (gx == nullptr) {
        return;
    }
    anim_clear(gx);
    embi_vec_free(&gx->changed);
    if (gx->text != nullptr) {
        embi_fonts_set_bitmap_sink(gx->text, nullptr, nullptr);
    }
    if (gx->atlas != nullptr) {
        bitmaps_release(gx);
        embi_hashmap_free(&gx->bitmaps);
    }
    if (gx->canvas != nullptr) {
        embi_canvas_gpu_destroy(gx->canvas);
    }
    embi_svgcache_destroy(gx->svgs);
    /* The cache releases its atlas regions through the hook, so it goes first. */
    if (gx->images != nullptr) {
        embi_image_cache_set_gpu(gx->images, nullptr);
        embi_image_cache_destroy(gx->images);
    }
    if (gx->atlas != nullptr) {
        embi_atlas_destroy(gx->atlas);
    }
    if (gx->fx != nullptr) {
        embi_fx_destroy(gx->fx);
    }
    if (gx->pool != nullptr) {
        embi_layer_pool_destroy(gx->pool);
    }
    if (gx->r != nullptr) {
        embi_renderer_destroy(gx->r);
    }
    SDL_free(gx->stops);
    embi_free(gx, sizeof *gx);
}

SDL_GPUTexture *embi_gfx_texture_create(embi_gfx *gx, uint32_t w, uint32_t h)
{
    return embi_gfx_texture_create_on(gx->dev, gx->fmt, w, h);
}

SDL_GPUTexture *embi_gfx_texture_create_on(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h)
{
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = fmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w > 0 ? w : 1;
    ci.height = h > 0 ? h : 1;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ci);
}

void embi_gfx_texture_destroy(embi_gfx *gx, SDL_GPUTexture *t)
{
    if (t != nullptr) {
        SDL_ReleaseGPUTexture(gx->dev, t);
    }
}

void embi_gfx_forget_images(embi_gfx *gx, const embi_dl *d)
{
    for (uint32_t i = 0; d != nullptr && i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind != EMBI_DI_IMAGE) {
            continue;
        }
        embi_str raw = embi_dl_url(d, it->aux);
        if (raw.len > 6 && memcmp(raw.ptr, "ember-", 6) == 0) {
            continue; /* a frame's or a canvas's picture */
        }
        embi_str url = gx->resolve != nullptr ? gx->resolve(gx->user, raw) : raw;
        embi_image_cache_forget(gx->images, url);
    }
    gx->image_gen++;
}

void embi_gfx_reset_images(embi_gfx *gx)
{
    anim_clear(gx);
    /* The destroy releases the atlas regions through the hook, which stays set until then. */
    gx->image_gen++;
    embi_gfx_svg_clear(gx);
    embi_image_cache_destroy(gx->images);
    gx->images = embi_image_cache_create(gx->image_bytes);
    EMBI_ASSERT(gx->images != nullptr);
    embi_image_gpu hook = {gx, gpu_upload, gpu_release};
    embi_image_cache_set_gpu(gx->images, &hook);
}

/* ------------------------------------------------------------------ W6c: animated images */

static void anim_note(void *user, embi_str url)
{
    embi_gfx *gx = user;
    char *copy = embi_alloc((size_t)url.len + 1);
    memcpy(copy, url.ptr, url.len);
    copy[url.len] = 0;
    embi_vec_push(&gx->changed, ((embi_str){copy, url.len}));
}

static void anim_clear(embi_gfx *gx)
{
    for (uint32_t i = 0; i < gx->changed.len; i++) {
        embi_free((char *)gx->changed.data[i].ptr, (size_t)gx->changed.data[i].len + 1);
    }
    gx->changed.len = 0;
}

uint32_t embi_gfx_anim_tick(embi_gfx *gx, double now_ms)
{
    return embi_image_cache_tick(gx->images, now_ms, anim_note, gx);
}

bool embi_gfx_anim_active(const embi_gfx *gx, double now_ms)
{
    return embi_image_cache_animating(gx->images, now_ms);
}

static bool anim_match(void *user, embi_str raw)
{
    embi_gfx *gx = user;
    embi_str url = gx->resolve != nullptr ? gx->resolve(gx->user, raw) : raw;
    for (uint32_t i = 0; i < gx->changed.len; i++) {
        embi_str c = gx->changed.data[i];
        if (c.len == url.len && memcmp(c.ptr, url.ptr, url.len) == 0) {
            return true;
        }
    }
    return false;
}

void embi_gfx_anim_damage(embi_gfx *gx, embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w,
                          uint32_t view_h)
{
    if (gx->changed.len > 0) {
        embi_damage_add_images(d, cur, scale, view_w, view_h, anim_match, gx);
        anim_clear(gx);
    }
}

bool embi_gfx_anim_pending(const embi_gfx *gx)
{
    return gx != nullptr && gx->changed.len > 0;
}

void embi_gfx_anim_damage_keep(embi_gfx *gx, embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w,
                               uint32_t view_h)
{
    if (gx != nullptr && gx->changed.len > 0) {
        embi_damage_add_images(d, cur, scale, view_w, view_h, anim_match, gx);
    }
}

void embi_gfx_set_user(embi_gfx *gx, void *user)
{
    if (gx != nullptr) {
        gx->user = user;
    }
}

void embi_gfx_set_frame_lookup(embi_gfx *gx, SDL_GPUTexture *(*fn)(void *user, const void *key, uint32_t *w, uint32_t *h),
                               void *user)
{
    if (gx != nullptr) {
        gx->frame_tex = fn;
        gx->frame_user = user;
    }
}

void embi_gfx_trim(embi_gfx *gx)
{
    embi_svgcache_trim(gx->svgs); /* R4c: after a draw no paint holds an SVG image */
    embi_image_cache_trim(gx->images);
    embi_gfx_trim_idle(gx);
}

void embi_gfx_trim_idle(embi_gfx *gx)
{
    if (gx != nullptr) {
        const uint64_t now = SDL_GetTicks();
        embi_layer_pool_trim_unused(gx->pool, now, EMBI_GFX_LAYER_KEEP_MS);
        if (gx->canvas != nullptr) {
            embi_canvas_gpu_trim(gx->canvas, now);
        }
    }
}
