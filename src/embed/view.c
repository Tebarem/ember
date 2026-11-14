/*
 * Views (W4e, ember.h "views"): creation, loading, the update pipeline (style, layout,
 * display list, damage), the offscreen render entry, scrolling input and stats.
 *
 * Pipeline per update. If the document or the viewport changed, the document is restyled and laid out (doc.c). If
 * anything the paint depends on changed (a layout, a scroll, a scrollbar fade, a scheme, a size), the display list is
 * built into the list not on screen, its scrollbars are appended, and the damage is computed against the list on
 * screen (damage.c). A layout invalidates the list on screen (its text runs belong to the old layout), so the next
 * damage is the whole view. The renderer then replays the list into the view's texture (gfx.c), damage rects only.
 */
#include "embed_int.h"

#include "embed/frames.h"
#include "embed/input_int.h"
#include "embed/script_int.h"
#include "base/clock.h"
#include "embed/frame.h"
#include "bindings/observers.h"
#include "canvas/canvas.h"
#include "embed/url.h"
#include "embed/resource_int.h"
#include "embed/netload.h"
#include "loader/loader.h"
#include "layout/text_shim.h"
#include "paint/damage.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define EMBI_VIEW_DEFAULT_W 800u
#define EMBI_VIEW_DEFAULT_H 600u

static double ticks_ms(void)
{
    return (double)SDL_GetTicks();
}

static double ticks_to_ms(uint64_t ticks)
{
    return (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* The focus ring color of outline-style: auto: #1A73E8, packed as the paint colors are (0xAABBGGRR). */
#define EMBI_VIEW_FOCUS 0xFFE8731Au

/* The device size of the view's texture for a CSS size and a scale (at least 1 px per side). */
static uint32_t device_px(uint32_t css, float scale)
{
    float px = ceilf((float)css * scale);
    if (!(px >= 1.0f)) {
        return 1;
    }
    return px > 16384.0f ? 16384u : (uint32_t)px;
}

/* ------------------------------------------------------------------ callbacks for the document and the GPU side */

/* The renderer of v, with its loader callbacks pointed at v: the frames of a view share their root's renderer (IF1), so each
 * one sets itself before its images are measured, painted or drawn. */
static embi_gfx *gfx_of(emb_view *v)
{
    if (v->gfx != nullptr) {
        embi_gfx_set_user(v->gfx, v);
    }
    return v->gfx;
}

/* The style engine the renderer's SVG images are styled with: the root view's, which outlives every frame (the images are
 * shared by the frames and released only with the root). */
static struct embi_style_engine *svg_engine(emb_view *v)
{
    return embi_doc_style_engine(embi_frames_root(v)->doc);
}

/* The document's loader (stylesheets, @import): an http(s) URL goes over the network. One that has not arrived is pending (the
 * view's thread does not wait): the document load is suspended on a render-blocking sheet, later sheets are collected again,
 * and both happen when it arrives. */
static bool view_fetch(void *user, embi_str url, uint8_t **data, size_t *len)
{
    emb_view *v = user;
    if (!embi_netload_scheme_ok(v, url)) {
        return false;
    }
    if (embi_netload_claims(v, url)) {
        return embi_netload_get(v, url, EMBI_NL_SHEET, data, len, nullptr) == EMBI_NL_OK;
    }
    return embi_rt_fetch(v->rt, url, data, len);
}

/* Scripts: the source of one that has arrived (the callers check embi_netload_state first and never run a pending one). */
static bool view_fetch_script(void *user, embi_str url, uint8_t **data, size_t *len)
{
    emb_view *v = user;
    if (!embi_netload_scheme_ok(v, url)) {
        return false;
    }
    if (embi_netload_claims(v, url)) {
        return embi_netload_get(v, url, 0, data, len, nullptr) == EMBI_NL_OK;
    }
    return embi_rt_fetch(v->rt, url, data, len);
}

static void doc_load_done(void *user, const embi_doc_times *t, bool ok);

/* Whether url is a network resource that is still downloading (its download starts if nobody asked yet). */
static bool view_net_pending(void *user, embi_str url)
{
    emb_view *v = user;
    return embi_netload_claims(v, url) && embi_netload_scheme_ok(v, url) &&
           embi_netload_state(v, url) == EMBI_NL_PENDING;
}

/* Images (the renderer's loader): an http(s) image that has not arrived is pending, not failed. */
static bool view_fetch_image(void *user, embi_str url, uint8_t **data, size_t *len)
{
    emb_view *v = user;
    if (!embi_netload_scheme_ok(v, url)) {
        embi_netload_refuse_image(v, url);
        return false;
    }
    if (embi_netload_claims(v, url)) {
        return embi_netload_get(v, url, EMBI_NL_IMAGE, data, len, nullptr) == EMBI_NL_OK;
    }
    return embi_rt_fetch(v->rt, url, data, len);
}

/* Prefetch (the link hrefs of the document): a network URL starts downloading before the parser needs it. */
static void view_prefetch(void *user, embi_str url)
{
    emb_view *v = user;
    if (embi_netload_claims(v, url) && embi_netload_scheme_ok(v, url)) {
        embi_netload_prefetch(v, url);
    }
}

static uint32_t view_pending_count(void *user)
{
    return embi_netload_pending_count(user);
}

static bool view_fetch_font(void *user, embi_str url, uint8_t **data, size_t *len)
{
    emb_view *v = user;
    if (!embi_netload_scheme_ok(v, url)) {
        return false;
    }
    if (embi_netload_claims(v, url)) {
        return embi_netload_get(v, url, EMBI_NL_FONT, data, len, nullptr) == EMBI_NL_OK;
    }
    return embi_rt_fetch_font(v->rt, url, data, len);
}

static bool view_image_size(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    emb_view *v = user;
    if (!embi_view_gfx_ready(v)) {
        return false;
    }
    embi_gfx_set_style_engine(gfx_of(v), svg_engine(v));
    return embi_gfx_image_size(gfx_of(v), url, w, h, has_ratio);
}

static const struct embi_svg_image *view_svg_image(void *user, embi_str url)
{
    emb_view *v = user;
    if (!embi_view_gfx_ready(v)) {
        return nullptr;
    }
    if ((url.len > 12 && memcmp(url.ptr, "ember-frame:", 12) == 0) ||
        (url.len > 13 && memcmp(url.ptr, "ember-canvas:", 13) == 0)) {
        return nullptr; /* the pictures of frames and canvases are textures, not images to fetch */
    }
    embi_gfx_set_style_engine(gfx_of(v), svg_engine(v));
    return embi_gfx_svg_image(gfx_of(v), url);
}

static embi_str view_resolve(void *user, embi_str raw)
{
    emb_view *v = user;
    return embi_doc_resolve(v->doc, raw);
}

#if EMBER_CANVAS
/* R4b: what a canvas needs from the view. */
static void canvas_dirty(void *user)
{
    emb_view *v = user;
    v->paint_dirty = true;
}

static bool canvas_pixels(void *user, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h)
{
    emb_view *v = user;
    return embi_view_gfx_ready(v) && embi_gfx_image_pixels(gfx_of(v), url, px, w, h);
}

static uint32_t canvas_image_gen(void *user)
{
    emb_view *v = user;
    return embi_view_gfx_ready(v) ? embi_gfx_image_generation(v->gfx) : 0;
}

static embi_canvas_gpu *canvas_gpu(void *user)
{
    emb_view *v = user;
    return embi_view_gfx_ready(v) ? embi_gfx_canvas(v->gfx) : nullptr;
}

void embi_view_canvas_env(emb_view *v, embi_canvas_env *e)
{
    memset(e, 0, sizeof *e);
    e->user = v;
    /* Not embi_view_gfx_ready here: this runs when a document's bindings are set up, and a windowed view's GPU device
     * should wait until a canvas is really made (canvas_gpu). */
    e->gpu = v->gfx != nullptr ? embi_gfx_canvas(v->gfx) : nullptr;
    e->gpu_get = canvas_gpu;
    e->text = v->rt->text;
    e->dirty = canvas_dirty;
    e->image_pixels = canvas_pixels;
    e->image_gen = canvas_image_gen;
}
#endif

/* ------------------------------------------------------------------ texture and sizes */

/* Makes the view's texture the given device size. A new texture holds nothing, so the next damage is the whole view. */
static void set_texture_size(emb_view *v, uint32_t dw, uint32_t dh)
{
    if (v->tex != nullptr && v->dev_w == dw && v->dev_h == dh) {
        return;
    }
    if (v->gfx == nullptr) {
        /* A windowed view before its GPU device: the texture is made at embi_view_gfx_ready, at this size. */
        v->dev_w = dw;
        v->dev_h = dh;
        v->paint_dirty = true;
        return;
    }
    embi_gfx_texture_destroy(v->gfx, v->tex);
    v->tex = embi_gfx_texture_create(v->gfx, dw, dh);
    if (v->tex != nullptr && v->dev != nullptr && embi_frames_parent(v) != nullptr) {
        /* A frame is sampled by its parent before its own document has painted (a load in flight): the new texture is
         * undefined memory on some GPUs (Metal), so it starts out transparent. */
        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(v->dev);
        if (cb != nullptr) {
            SDL_GPUColorTargetInfo ct;
            memset(&ct, 0, sizeof ct);
            ct.texture = v->tex;
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr);
            if (rp != nullptr) {
                SDL_EndGPURenderPass(rp);
            }
            (void)SDL_SubmitGPUCommandBuffer(cb);
        }
    }
    v->dev_w = dw;
    v->dev_h = dh;
    v->force_full = true;
    v->have_prev = false;
    v->paint_dirty = true;
}

static void sync_doc_viewport(emb_view *v)
{
    embi_doc_set_viewport(v->doc, (float)v->css_w, (float)v->css_h, v->scale, v->scheme == EMB_SCHEME_DARK);
}

/* The renderer of the view (pipelines, atlas, layer pool) on its GPU device. */
/* The renderer is made without the text engine so that it can be made on a thread (create_gfx_start) while the
 * view builds its document; create_gfx_finish joins and hooks the text engine in. */
typedef struct gfx_job {
    embi_gfx_cfg cfg;
    embi_gfx *gfx;
    double ms;
    SDL_Thread *thread;
    /* The view target (offscreen views): made by a thread of its own, since it needs only the device (FP2b). */
    SDL_GPUDevice *dev;
    uint32_t tex_w, tex_h;
    SDL_GPUTexture *tex;
    SDL_Thread *tex_thread;
} gfx_job;

static int SDLCALL tex_job_main(void *arg)
{
    gfx_job *j = arg;
    j->tex = embi_gfx_texture_create_on(j->dev, j->cfg.format, j->tex_w, j->tex_h);
    return 0;
}

static int SDLCALL gfx_job_main(void *arg)
{
    gfx_job *j = arg;
    uint64_t tp = SDL_GetPerformanceCounter();
    j->gfx = embi_gfx_create(&j->cfg);
    j->ms = ticks_to_ms(SDL_GetPerformanceCounter() - tp);
    return 0;
}

static void create_gfx_start(emb_view *v, gfx_job *j, bool threaded, uint32_t tex_w, uint32_t tex_h)
{
    memset(j, 0, sizeof *j);
    j->cfg.gpu = v->gpu;
    j->cfg.format = v->fmt;
    if (v->mode == EMB_VIEW_WINDOWED) {
        j->cfg.format = SDL_GetGPUSwapchainTextureFormat(v->dev, v->window);
        v->fmt = j->cfg.format;
    }
    j->cfg.text = nullptr;
    j->cfg.image_bytes = v->rt->image_bytes;
    j->cfg.user = v;
    j->cfg.fetch = view_fetch_image;
    j->cfg.pending_count = view_pending_count;
    j->cfg.resolve = view_resolve;
    if (threaded) {
        j->dev = v->dev;
        j->tex_w = tex_w;
        j->tex_h = tex_h;
        j->thread = SDL_CreateThread(gfx_job_main, "ember-gfx-create", j);
        if (j->thread != nullptr) {
            j->tex_thread = SDL_CreateThread(tex_job_main, "ember-view-tex", j);
        }
    }
    if (j->thread == nullptr) {
        gfx_job_main(j);
    }
}

static bool create_gfx_finish(emb_view *v, gfx_job *j)
{
    if (j->thread != nullptr) {
        SDL_WaitThread(j->thread, nullptr);
        j->thread = nullptr;
    }
    if (j->tex_thread != nullptr) {
        SDL_WaitThread(j->tex_thread, nullptr);
        j->tex_thread = nullptr;
    }
    v->gfx = j->gfx;
    if (v->gfx == nullptr) {
        if (j->tex != nullptr) {
            SDL_ReleaseGPUTexture(v->dev, j->tex);
            j->tex = nullptr;
        }
        embi_rt_log(v->rt, EMB_LOG_ERROR, "the renderer could not be created");
        return false;
    }
    embi_gfx_attach_text(v->gfx, v->rt->text);
    embi_gfx_set_frame_lookup(v->gfx, embi_frames_texture, v); /* IF1: the iframes' pictures */
    v->st.gpu_pipelines_ms = j->ms;
    return true;
}

static bool create_gfx(emb_view *v)
{
    gfx_job j;
    create_gfx_start(v, &j, false, 0, 0);
    return create_gfx_finish(v, &j);
}

bool embi_view_gfx_ready(emb_view *v)
{
    if (v->gfx != nullptr) {
        return true;
    }
    if (embi_frames_parent(v) != nullptr) {
        /* A frame: the root's renderer, made by the root when it is windowed. */
        if (!embi_view_gfx_ready(embi_frames_root(v))) {
            return false;
        }
        embi_view_child_gfx(v);
        return v->gfx != nullptr;
    }
    if (v->failed || v->mode != EMB_VIEW_WINDOWED) {
        return false;
    }
    /* A windowed view: the device was left until now so that the page could load while the driver started. */
    uint32_t dw = v->dev_w, dh = v->dev_h;
    if (!embi_window_gpu_open(v) || !create_gfx(v)) {
        v->failed = true;
        return false;
    }
    v->dev_w = 0;
    v->dev_h = 0;
    set_texture_size(v, dw, dh);
    if (v->tex == nullptr) {
        embi_rt_log(v->rt, EMB_LOG_ERROR, "the view texture could not be created");
        v->failed = true;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ lifetime */

/* The view's document state, with the view's loaders. */
static void make_doc(emb_view *v, bool no_script)
{
    embi_doc_cb cb = {v, view_fetch, view_image_size, v};
    v->doc = embi_doc_create(v->rt->text, v->rt->fonts, &cb);
    embi_doc_set_net(v->doc, view_fetch_script, view_pending_count, view_prefetch, view_net_pending);
    embi_doc_set_load_done(v->doc, doc_load_done);
    embi_doc_set_font_fetch(v->doc, view_fetch_font);
    embi_doc_set_svg_image(v->doc, view_svg_image);
    if (no_script && v->doc != nullptr) {
        embi_doc_set_scripting(v->doc, false);
    }
    sync_doc_viewport(v);
    embi_doc_prime_env(v->doc);
}

emb_view *embi_view_new_child(emb_view *parent)
{
    emb_view *v = embi_alloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->rt = parent->rt;
    v->mode = EMB_VIEW_OFFSCREEN;
    v->scheme = parent->scheme;
    v->text_aa = EMB_TEXT_AA_GRAY; /* a frame's texture is transparent where its document paints nothing */
    v->css_w = 300; /* the default object size of an iframe; the parent's layout sets the real one */
    v->css_h = 150;
    v->scale = parent->scale;
    v->st.size = sizeof v->st;
    v->fmt = parent->fmt;
    v->force_full = true;
    v->paint_dirty = true;
    embi_damage_init(&v->damage);
    embi_dl_init(&v->dl[0]);
    embi_dl_init(&v->dl[1]);
    if (!embi_frames_adopt(v, parent)) {
        embi_damage_free(&v->damage);
        embi_free(v, sizeof *v);
        return nullptr;
    }
    make_doc(v, parent->doc != nullptr && !embi_doc_scripting(parent->doc));
    if (v->doc == nullptr) {
        emb_view_destroy(v);
        return nullptr;
    }
    embi_view_child_gfx(v);
    return v;
}

void embi_view_child_gfx(emb_view *v)
{
    emb_view *root = embi_frames_root(v);
    if (v->gfx == nullptr && root != v && root->gfx != nullptr) {
        v->gfx = root->gfx;
        v->fmt = root->fmt;
        v->gpu = root->gpu;
        v->dev = root->dev;
        v->dev_w = 0;
        v->dev_h = 0;
        set_texture_size(v, device_px(v->css_w, v->scale), device_px(v->css_h, v->scale));
    }
}

void embi_view_set_child_size(emb_view *v, uint32_t css_w, uint32_t css_h)
{
    emb_view *root = embi_frames_root(v);
    v->css_w = css_w > 0 ? css_w : 1;
    v->css_h = css_h > 0 ? css_h : 1;
    if (v->scale != root->scale || v->scheme != root->scheme) {
        v->scale = root->scale;
        v->scheme = root->scheme;
        v->force_full = true;
    }
    sync_doc_viewport(v);
    set_texture_size(v, device_px(v->css_w, v->scale), device_px(v->css_h, v->scale));
    v->paint_dirty = true;
}

emb_view *emb_view_create(emb_runtime *rt, const emb_view_desc *desc)
{
    if (desc == nullptr || rt == nullptr) {
        return nullptr;
    }
    emb_view *v = embi_alloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->rt = rt;
    v->mode = desc->mode;
    v->scheme = desc->scheme;
    if (desc->size >= offsetof(emb_view_desc, text_aa) + sizeof desc->text_aa) {
        v->text_aa = desc->text_aa;
    }
    if (desc->size >= offsetof(emb_view_desc, transparent) + sizeof desc->transparent) {
        v->transparent = desc->mode == EMB_VIEW_OFFSCREEN && desc->transparent;
    }
    v->css_w = desc->width > 0 ? desc->width : EMBI_VIEW_DEFAULT_W;
    v->css_h = desc->height > 0 ? desc->height : EMBI_VIEW_DEFAULT_H;
    v->scale = desc->scale > 0.0f ? desc->scale : 1.0f;
    v->st.size = sizeof v->st;
    v->st.runtime_init_ms = rt->runtime_init_ms;
    v->fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    v->force_full = true;
    v->paint_dirty = true;
    embi_damage_init(&v->damage);
    embi_dl_init(&v->dl[0]);
    embi_dl_init(&v->dl[1]);

    uint32_t dev_w = 0, dev_h = 0;
    if (v->mode == EMB_VIEW_OFFSCREEN) {
        if (desc->device == nullptr) {
            embi_rt_log(rt, EMB_LOG_ERROR, "emb_view_create: offscreen mode needs a device");
            emb_view_destroy(v);
            return nullptr;
        }
        v->gpu = embi_gpu_create(desc->device, false);
        v->gpu_owned = false;
        v->dev = desc->device;
        dev_w = device_px(v->css_w, v->scale);
        dev_h = device_px(v->css_h, v->scale);
    } else {
        if (!embi_window_open(v, desc)) {
            emb_view_destroy(v);
            return nullptr;
        }
        int lw = 0, lh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(v->window, &lw, &lh);
        SDL_GetWindowSizeInPixels(v->window, &pw, &ph);
        v->css_w = lw > 0 ? (uint32_t)lw : v->css_w;
        v->css_h = lh > 0 ? (uint32_t)lh : v->css_h;
        dev_w = pw > 0 ? (uint32_t)pw : v->css_w;
        dev_h = ph > 0 ? (uint32_t)ph : v->css_h;
        v->scale = (float)dev_w / (float)v->css_w;
    }
    if (v->mode == EMB_VIEW_OFFSCREEN && v->gpu == nullptr) {
        embi_rt_log(rt, EMB_LOG_ERROR, "emb_view_create: no GPU device");
        emb_view_destroy(v);
        return nullptr;
    }

    /* The pipelines and buffers are made on a thread while the document (the UA sheet, the style engine) is built. */
    gfx_job gj;
    memset(&gj, 0, sizeof gj);
    if (v->mode == EMB_VIEW_OFFSCREEN) {
        create_gfx_start(v, &gj, true, dev_w, dev_h);
    }

    make_doc(v, desc->size >= offsetof(emb_view_desc, no_script) + sizeof desc->no_script && desc->no_script);
    if (v->mode == EMB_VIEW_OFFSCREEN && !create_gfx_finish(v, &gj)) {
        emb_view_destroy(v);
        return nullptr;
    }
    v->dev_w = 0;
    v->dev_h = 0;
    if (gj.tex != nullptr) {
        /* The target made by the thread (the same as set_texture_size would make). */
        v->tex = gj.tex;
        v->dev_w = dev_w;
        v->dev_h = dev_h;
        v->force_full = true;
        v->have_prev = false;
        v->paint_dirty = true;
    }
    set_texture_size(v, dev_w, dev_h);
    if (v->mode == EMB_VIEW_OFFSCREEN && v->tex == nullptr) {
        embi_rt_log(rt, EMB_LOG_ERROR, "emb_view_create: the view texture could not be created");
        emb_view_destroy(v);
        return nullptr;
    }
    if (v->mode == EMB_VIEW_WINDOWED) {
        embi_view_apply_title(v);
    }
    return v;
}

void emb_view_destroy(emb_view *v)
{
    if (v == nullptr) {
        return;
    }
    /* IF1: the frames first (they share this view's renderer). A frame keeps its root's renderer and device. */
    const bool child = embi_frames_parent(v) != nullptr;
    embi_frames_destroy(v);
    SDL_free(v->defer_html);
    SDL_free(v->defer_base);
    embi_netload_detach(v); /* the network thread posts to the view's document: stop before it goes */
    embi_input_destroy(v); /* unpins the nodes while the document (and its binding context) still exists */
    if (!child) {
        embi_gfx_svg_clear(v->gfx); /* the parsed SVG images are styled by the document's engine: they go first */
    }
    if (v->doc != nullptr) {
        embi_doc_destroy(v->doc);
        v->doc = nullptr;
    }
    embi_netload_free(v); /* after the document: its tasks that hold a record are dropped with its loop */
    if (v->gfx != nullptr) {
        embi_gfx_texture_destroy(v->gfx, v->tex);
        v->tex = nullptr;
        if (!child) {
            embi_gfx_destroy(v->gfx);
        }
        v->gfx = nullptr;
    }
    SDL_free(v->tracks);
    embi_damage_free(&v->damage);
    embi_dl_free(&v->dl[0]);
    embi_dl_free(&v->dl[1]);
    /* SDL's order: the window leaves the device, the device goes, then the window and the video subsystem. */
    if (v->mode == EMB_VIEW_WINDOWED) {
        embi_window_release(v);
    }
    /* The device of an offscreen view is the host's: embi_gpu_destroy leaves it alone. A frame's is its root's. */
    if (!child) {
        embi_gpu_destroy(v->gpu);
    }
    if (v->mode == EMB_VIEW_WINDOWED) {
        embi_window_close(v);
    }
    SDL_free(v->fallback_title);
    embi_free(v, sizeof *v);
}

SDL_GPUTexture *emb_view_texture(const emb_view *view)
{
    return view != nullptr ? view->tex : nullptr;
}

const char *emb_view_title(const emb_view *view)
{
    return view != nullptr && view->doc != nullptr ? embi_doc_title(view->doc) : "";
}

/* ------------------------------------------------------------------ loading */

bool emb_view_load_html(emb_view *view, const char *html, size_t len, const char *base_url)
{
    if (view == nullptr || (html == nullptr && len > 0) || len > UINT32_MAX) {
        return false;
    }
    if (view->doc != nullptr && embi_doc_script_running(view->doc)) {
        /* Called from inside a script of the current document (a navigation hook): the load waits for the script to return, and the call returns true. */
        char *h = SDL_malloc(len + 1);
        char *b = base_url != nullptr ? SDL_strdup(base_url) : nullptr;
        if (h == nullptr || (base_url != nullptr && b == nullptr)) {
            SDL_free(h);
            SDL_free(b);
            return false;
        }
        if (len > 0) {
            memcpy(h, html, len);
        }
        h[len] = '\0';
        SDL_free(view->defer_html);
        SDL_free(view->defer_base);
        view->defer_html = h;
        view->defer_len = len;
        view->defer_base = b;
        view->defer_has = true;
        return true;
    }
    embi_frames_doc_begin(view); /* IF1: the old document's frames go; the new one has a new window id */
    embi_input_reset(view); /* the hover, press and focus of the old document go before its binding context does */
    embi_netload_reset(view); /* the network loads of the old document (and a navigation still in flight) are over */
    size_t base_len = base_url != nullptr ? strlen(base_url) : 0;
    if (base_len > UINT32_MAX) {
        return false;
    }
    embi_str base = base_url != nullptr ? (embi_str){base_url, (uint32_t)base_len} : (embi_str){"", 0};
    /* The images of the previous document are not the new document's, even for the same URL. A frame shares the cache of its
     * root's renderer with the other documents of the tree: it keeps it. */
    if (view->gfx != nullptr && embi_frames_parent(view) == nullptr) {
        embi_gfx_reset_images(view->gfx);
    } else if (view->gfx != nullptr) {
        embi_gfx *gx = gfx_of(view); /* the frame's old document resolves its URLs */
        embi_gfx_forget_images(gx, &view->dl[0]);
        embi_gfx_forget_images(gx, &view->dl[1]);
    }
    view->net_doc = view->net_doc_next; /* decided before parsing, so the inline scripts see it */
    view->net_doc_next = false;
    embi_doc_times tm;
    memset(&tm, 0, sizeof tm);
    /* A page from the network may be suspended on a script or a stylesheet: the view then keeps showing what it showed (the
     * frame in its texture) until the document finishes and embi_view_load_done starts the new one. */
    embi_doc_load_status st = embi_doc_load_begin(view->doc, (embi_str){html != nullptr ? html : "", (uint32_t)len}, base, &tm);
    if (st == EMBI_DOC_LOAD_SUSPENDED || st == EMBI_DOC_LOAD_GONE) {
        return true; /* gone: a nested load replaced this document and reports for itself */
    }
    embi_view_load_done(view, &tm, st != EMBI_DOC_LOAD_FAILED);
    return st != EMBI_DOC_LOAD_FAILED;
}

void embi_view_run_deferred_load(void *vp)
{
    emb_view *view = vp;
    if (view == nullptr || !view->defer_has) {
        return;
    }
    char *h = view->defer_html;
    char *b = view->defer_base;
    size_t n = view->defer_len;
    view->defer_html = nullptr;
    view->defer_base = nullptr;
    view->defer_has = false;
    (void)emb_view_load_html(view, h, n, b);
    SDL_free(h);
    SDL_free(b);
}

/* A document finished loading (the end of emb_view_load_html, or later when it was suspended): the new picture replaces the old. */
void embi_view_load_done(emb_view *view, const embi_doc_times *tm, bool ok)
{
    (void)ok;
    view->st.parse_ms = tm->parse_ms;
    view->st.script_realm_ms = tm->script_realm_ms;
    view->st.script_compile_ms = tm->script_compile_ms;
    view->st.script_run_ms = tm->script_run_ms;
    view->st.style_ms = tm->style_ms;
    view->st.layout_ms = tm->layout_ms;
    view->st.stylesheets_ms = tm->sheets_ms;
    view->st.fonts_ms = ticks_to_ms(embi_tshim_first_ticks());
    view->have_prev = false;
    view->force_full = true;
    view->paint_dirty = true;
    view->anim.active = false;
    view->ntracks = 0;
    if (view->mode == EMB_VIEW_WINDOWED) {
        embi_view_apply_title(view);
    }
}

static void doc_load_done(void *user, const embi_doc_times *t, bool ok)
{
    emb_view *view = user;
    if (!ok) {
        /* A load that finished later and could not be laid out: the caller of emb_view_load_html has long returned, so the
         * view shows the same error page a failed main document gets. */
        embi_str u = embi_doc_url(view->doc);
        char *url = SDL_strndup(u.ptr, u.len);
        embi_rt_log(view->rt, EMB_LOG_WARN, "cannot show %s: the page could not be laid out", url);
        embi_netload_show_error(view, url, "the page could not be laid out");
        SDL_free(url);
        return;
    }
    embi_view_load_done(view, t, ok);
}

bool emb_view_load_url(emb_view *view, const char *url)
{
    if (view == nullptr || url == nullptr) {
        return false;
    }
    size_t url_len = strlen(url);
    if (url_len > UINT32_MAX) {
        return false;
    }
    embi_str u = {url, (uint32_t)url_len};
    embi_str scheme;
    if (!embi_url_scheme(u, &scheme)) {
        /* A bare path (relative paths resolve against the current directory): its file: URL is the document's URL. */
        size_t cap = embi_url_from_bare_path(u, nullptr, 0) + 1;
        char *furl = SDL_malloc(cap);
        if (furl == nullptr) {
            return false;
        }
        embi_url_from_bare_path(u, furl, cap);
        bool ok = emb_view_load_url(view, furl);
        SDL_free(furl);
        return ok;
    }
    if (embi_netload_claims(view, u)) {
        /* An http(s) page: the request starts now and the old document stays until the response is in (the load then runs
         * on the view's thread, from the event queue). */
        return embi_netload_navigate(view, u);
    }
    uint8_t *bytes = nullptr;
    size_t blen = 0;
    embi_sb mime = {nullptr, 0, 0};
    uint64_t t0 = SDL_GetPerformanceCounter();
    bool fetched;
    if (embi_url_scheme_is(scheme, "file") || embi_url_scheme_is(scheme, "data") || embi_url_scheme_is(scheme, "blob")) {
        fetched = embi_rt_fetch(view->rt, u, &bytes, &blen);
    } else {
        /* A handler's response carries its Content-Type: its charset is the transport label of the sniffing. */
        fetched = embi_rt_load(view->rt, u, &bytes, &blen, &mime);
    }
    if (!fetched) {
        embi_sb_free(&mime);
        return false;
    }
    double fetch_ms = ticks_to_ms(SDL_GetPerformanceCounter() - t0);
    bool ok = embi_view_load_bytes(view, bytes, blen, &mime, url);
    embi_sb_free(&mime);
    view->st.fetch_ms = fetch_ms;
    SDL_free(bytes);
    return ok;
}

bool embi_view_load_bytes(emb_view *view, const uint8_t *bytes, size_t blen, const embi_sb *mime, const char *base_url)
{
    /* The parser takes UTF-8: sniff the encoding (BOM, Content-Type charset, meta charset). Without any of them the
     * document is UTF-8, not windows-1252: files and app:// bundles are overwhelmingly UTF-8. */
    embi_str label = {nullptr, 0};
    embi_mime mt = {0};
    if (mime != nullptr && mime->len > 0 && embi_mime_parse((embi_str){mime->data, (uint32_t)mime->len}, &mt)) {
        label = embi_mime_param_value(&mt, EMBI_STR("charset"));
    }
    embi_sb utf8 = {nullptr, 0, 0};
    embi_decode_document_fb(bytes, blen, label, embi_encoding_named("utf-8"), &utf8, nullptr);
    embi_mime_free(&mt);
    bool ok = emb_view_load_html(view, utf8.len ? utf8.data : "", utf8.len, base_url);
    embi_sb_free(&utf8);
    return ok;
}

/* ------------------------------------------------------------------ viewport and presentation settings */

void emb_view_resize(emb_view *view, uint32_t width, uint32_t height)
{
    if (view == nullptr) {
        return;
    }
    if (view->mode == EMB_VIEW_WINDOWED) {
        if (width > 0 && height > 0) {
            SDL_SetWindowSize(view->window, (int)width, (int)height);
        }
        return;
    }
    if (width > 0) {
        view->css_w = width;
    }
    if (height > 0) {
        view->css_h = height;
    }
    sync_doc_viewport(view);
    set_texture_size(view, device_px(view->css_w, view->scale), device_px(view->css_h, view->scale));
}

void emb_view_set_scale(emb_view *view, float scale)
{
    if (view == nullptr || view->mode == EMB_VIEW_WINDOWED || !(scale > 0.0f)) {
        return;
    }
    view->scale = scale;
    sync_doc_viewport(view);
    set_texture_size(view, device_px(view->css_w, view->scale), device_px(view->css_h, view->scale));
    view->force_full = true;
    view->paint_dirty = true;
}

void emb_view_set_color_scheme(emb_view *view, emb_color_scheme scheme)
{
    if (view == nullptr || view->scheme == scheme) {
        return;
    }
    view->scheme = scheme;
    sync_doc_viewport(view);
    view->force_full = true;
    view->paint_dirty = true;
}

void emb_view_set_text_aa(emb_view *view, emb_text_aa mode)
{
    if (view == nullptr || view->text_aa == mode) {
        return;
    }
    view->text_aa = mode;
    view->force_full = true;
    view->paint_dirty = true;
}

/* The antialiasing a view draws with (an embi_text_aa value): AUTO is LCD RGB on a windowed view on
 * Windows and Linux below 2x, gray otherwise. */
static int resolve_text_aa(const emb_view *v)
{
    switch (v->text_aa) {
    case EMB_TEXT_AA_GRAY:
        return 0;
    case EMB_TEXT_AA_LCD_RGB:
        return 1;
    case EMB_TEXT_AA_LCD_BGR:
        return 2;
    default:
        break;
    }
    if (v->mode != EMB_VIEW_WINDOWED || v->scale >= 2.0f) {
        return 0;
    }
    const char *pf = SDL_GetPlatform();
    return (pf != nullptr && (SDL_strcmp(pf, "Windows") == 0 || SDL_strcmp(pf, "Linux") == 0)) ? 1 : 0;
}

void embi_view_set_window_size(emb_view *v, uint32_t css_w, uint32_t css_h, uint32_t dev_w, uint32_t dev_h)
{
    if (css_w == 0 || css_h == 0 || dev_w == 0 || dev_h == 0) {
        return;
    }
    v->css_w = css_w;
    v->css_h = css_h;
    v->scale = (float)dev_w / (float)css_w;
    sync_doc_viewport(v);
    set_texture_size(v, dev_w, dev_h);
    v->paint_dirty = true;
}

/* ------------------------------------------------------------------ scrolling */

/* Drops the tracks whose bar has faded out (their fade is 0), so a page with many scrollers keeps room for new ones. A
 * track that was on screen makes the view repaint, so its bar is removed from the picture. */
static void prune_tracks(emb_view *v, double now)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < v->ntracks; i++) {
        embi_view_track *tr = &v->tracks[i];
        if (embi_doc_fade_alpha(tr->last_ms, tr->seen, now) > 0.0f) {
            v->tracks[k++] = *tr;
        } else if (tr->shown) {
            v->paint_dirty = true;
        }
    }
    v->ntracks = k;
}

void embi_view_note_scroll(emb_view *v, const void *key)
{
    double now = ticks_ms();
    for (uint32_t i = 0; i < v->ntracks; i++) {
        if (v->tracks[i].key == key) {
            v->tracks[i].last_ms = now;
            v->tracks[i].seen = true;
            return;
        }
    }
    if (v->ntracks == v->captracks) {
        /* Bars that faded out make room first; the table grows only when they all still show. */
        prune_tracks(v, now);
    }
    if (v->ntracks == v->captracks) {
        uint32_t cap = v->captracks ? v->captracks * 2 : 16;
        embi_view_track *t = SDL_realloc(v->tracks, (size_t)cap * sizeof *t);
        EMBI_ASSERT(t != nullptr);
        v->tracks = t;
        v->captracks = cap;
    }
    v->tracks[v->ntracks++] = (embi_view_track){key, now, true, false};
}

void emb_view_scroll_by(emb_view *view, float dx, float dy)
{
    if (view == nullptr || embi_view_load_suspended(view)) {
        return;
    }
    embi_view_scroll_root(view, dx, dy);
}

void embi_view_scroll_root(emb_view *v, float dx, float dy)
{
    if (embi_doc_scroll_root_by(v->doc, dx, dy, (float)v->css_w, (float)v->css_h)) {
        embi_view_note_scroll(v, embi_doc_root_key(v->doc));
        v->scroll_dirty = true;
    }
}

void emb_view_scroll_to(emb_view *view, float x, float y)
{
    if (view == nullptr || embi_view_load_suspended(view)) {
        return;
    }
    if (embi_doc_root_smooth(view->doc)) {
        embi_doc_anim_start(view->doc, &view->anim, x, y, ticks_ms(), (float)view->css_w, (float)view->css_h);
        embi_view_note_scroll(view, embi_doc_root_key(view->doc));
        view->scroll_dirty = true;
        return;
    }
    float cx = 0.0f, cy = 0.0f;
    embi_doc_root_offset(view->doc, &cx, &cy);
    embi_view_scroll_root(view, x - cx, y - cy);
}

bool embi_view_wheel(emb_view *v, float x, float y, float dx, float dy)
{
    const void *key = nullptr;
    v->anim.active = false;
    if (embi_doc_wheel(v->doc, x, y, dx, dy, (float)v->css_w, (float)v->css_h, &key)) {
        embi_view_note_scroll(v, key);
        v->scroll_dirty = true;
        return true;
    }
    return false;
}

void emb_view_wheel(emb_view *view, float x, float y, float dx, float dy)
{
    if (view != nullptr) {
        embi_input_wheel_at(view, x, y, dx, dy); /* the cancelable wheel event first, then embi_view_wheel (input.c) */
    }
}

/* ------------------------------------------------------------------ scheme-dependent queries */

bool embi_view_animating(const emb_view *v)
{
    if (v->anim.active) {
        return true;
    }
    if (v->doc != nullptr && embi_doc_animating(v->doc)) {
        return true; /* W6c: a running animation or transition, or a requestAnimationFrame callback */
    }
    if (embi_frames_animating((emb_view *)(uintptr_t)v)) {
        return true; /* IF1: a frame animates, or one waits to be made, loaded or removed */
    }
    if (v->gfx != nullptr && embi_gfx_anim_active(v->gfx, embi_clock_ms())) {
        return true; /* W6c: an animated GIF on screen */
    }
    double now = ticks_ms();
    for (uint32_t i = 0; i < v->ntracks; i++) {
        if (embi_doc_fade_alpha(v->tracks[i].last_ms, v->tracks[i].seen, now) > 0.0f) {
            return true;
        }
    }
    return false;
}

void embi_view_apply_title(emb_view *v)
{
    if (v->window == nullptr) {
        return;
    }
    const char *t = v->doc != nullptr ? embi_doc_title(v->doc) : "";
    if (t[0] != '\0') {
        SDL_SetWindowTitle(v->window, t);
    } else {
        SDL_SetWindowTitle(v->window, v->fallback_title != nullptr ? v->fallback_title : "Ember");
    }
}

/* ------------------------------------------------------------------ update and frame */

void embi_view_layout(emb_view *v)
{
    if (v->doc == nullptr || embi_doc_load_suspended(v->doc)) {
        return;
    }
    /* Style and layout. A layout replaces the runs of the list on screen, so that list is no longer comparable. */
    embi_doc_times tm;
    memset(&tm, 0, sizeof tm);
    if (embi_doc_update(v->doc, &tm)) {
        v->st.style_ms = tm.style_ms;
        v->st.layout_ms = tm.layout_ms;
        v->have_prev = false;
        v->force_full = true;
        v->paint_dirty = true;
        if (v->mode == EMB_VIEW_WINDOWED) {
            embi_view_apply_title(v);
        }
    } else if (embi_doc_take_paint_pending(v->doc)) {
        /* W6b: only paint properties changed. The fragments and their runs stand; the picture needs a (partial) repaint. */
        v->st.style_ms = tm.style_ms;
        v->st.layout_ms = tm.layout_ms;
        v->style_paint = true;
    }
}

bool embi_view_update(emb_view *v)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (v->doc == nullptr) {
        return false;
    }
    if (embi_doc_load_suspended(v->doc)) {
        /* N1f: the document is half built and waits for the network. Its tasks and timers run (the arrivals that resume the
         * load are among them) and nothing is laid out or painted: the view keeps what it showed before. */
        (void)embi_script_pump(embi_doc_script(v->doc));
        if (v->doc == nullptr || embi_doc_load_suspended(v->doc)) {
            return false;
        }
    }
    double now = ticks_ms();

    /* A smooth scroll advances with the clock. */
    if (v->anim.active) {
        embi_doc_anim_step(v->doc, &v->anim, now, (float)v->css_w, (float)v->css_h);
        embi_view_note_scroll(v, embi_doc_root_key(v->doc));
        v->scroll_dirty = true;
    }

    /* W6c: animated GIFs follow the frame clock; a changed frame repaints (the damage is the image rects, below). A frame's
     * renderer is its root's, which ticks it once for the whole tree before the frames update. */
    const bool child = embi_frames_parent(v) != nullptr;
    if (!child && v->gfx != nullptr && embi_gfx_anim_tick(gfx_of(v), embi_clock_ms()) > 0) {
        v->paint_dirty = true;
    }

    /* W6c: the rendering opportunity. The due timers, the animation timeline and events, the animation frame callbacks. */
    embi_doc_frame(v->doc);
    if (v->doc == nullptr) {
        return false;
    }
    /* A script may have moved the focus since the last input: the state the rules read is brought up to date first. */
    embi_input_refresh_state(v);
    embi_view_layout(v);
    /* ResizeObserver and IntersectionObserver run once per frame after layout and before paint. Their callbacks may
     * change the DOM (or load another document), so the layout is brought up to date again before painting. */
    embi_script *script = v->doc != nullptr ? embi_doc_script(v->doc) : nullptr;
    embi_bind_ctx *bc = script != nullptr ? embi_script_bind(script) : nullptr;
    if (bc != nullptr) {
        embi_observers_after_layout(bc);
        if (v->doc == nullptr) {
            return false;
        }
        embi_view_layout(v);
    }
    if (v->doc != nullptr) {
        embi_doc_frame_end(v->doc);
    }
    /* W6e: the events the frame end dispatches (animation and transition events, their promises) run script; a change they make
     * to the DOM (a test removing its elements when it completes) is laid out before the paint reads the boxes. */
    if (v->doc != nullptr) {
        embi_view_layout(v);
    }
    /* The pointer's cursor follows the style this frame resolved (a :hover rule or a script may have changed it). */
    embi_input_cursor_refresh(v);
    /* IF1: the frames, now that the iframes' boxes are laid out (their viewports). */
    const bool kids_changed = v->doc != nullptr && embi_frames_update(v);
    if (v->doc == nullptr) {
        return false;
    }
    /* N1f: a task of this frame resumed the load and the parse stopped again (at a render-blocking sheet or a parser-blocking
     * script), or a script laid the half-built document out: nothing of it is painted until the load goes on. The layout
     * serial is not taken, so the first update after the load paints the document. */
    if (embi_doc_load_suspended(v->doc)) {
        return false;
    }
    const bool anim_kept = child && embi_gfx_anim_pending(v->gfx);

    /* Scrollbars: those that fade are repainted until they are gone. */
    bool fading = false, sb_dirty = false;
    embi_doc_sb *sbs = v->ntracks > 0 ? SDL_malloc((size_t)v->ntracks * sizeof *sbs) : nullptr;
    EMBI_ASSERT(sbs != nullptr || v->ntracks == 0);
    uint32_t nsb = 0;
    for (uint32_t i = 0; i < v->ntracks; i++) {
        embi_view_track *tr = &v->tracks[i];
        float alpha = embi_doc_fade_alpha(tr->last_ms, tr->seen, now);
        bool shown = alpha > 0.0f;
        if (shown != tr->shown) {
            sb_dirty = true;
        }
        tr->shown = shown;
        if (shown) {
            fading = true;
            sbs[nsb++] = (embi_doc_sb){tr->key, alpha};
        }
    }
    if (fading) {
        sb_dirty = true;
    }
    if (v->sbar_dirty) {
        sb_dirty = true; /* the pointer moved on or off the thumb of a styled scrollbar */
        v->sbar_dirty = false;
    }

    if (embi_input_forms_tick(v)) {
        v->paint_dirty = true; /* W5e: a form control changed or the caret blinked */
    }
    /* Layouts this update did not see through embi_view_layout: a script that read geometry (a task, a host message, a timer,
     * an event or an observer callback) laid the document out at once, and embi_doc_update then had nothing to do. A layout
     * that rebuilt the fragments changed the picture: it is painted again (compared with the list on screen). One that freed
     * shaped runs (a tree built again sweeps the runs it no longer uses) left the list on screen naming freed runs: that list
     * is neither compared (damage.c hashes its runs' text) nor reused for a scroll, and the whole view is drawn. */
    const uint64_t lay_serial = embi_doc_layout_serial(v->doc);
    const uint64_t run_epoch = embi_doc_run_epoch(v->doc);
    if (lay_serial != v->prev_serial) {
        v->paint_dirty = true;
    }
    if (v->have_prev && run_epoch != v->prev_runs) {
        v->have_prev = false;
        v->force_full = true;
        v->paint_dirty = true;
    }
    const bool scroll_pending = v->scroll_dirty;
    v->scroll_dirty = false;
    if (!v->paint_dirty && !v->force_full && !v->style_paint && !scroll_pending && !sb_dirty && v->have_prev &&
        (kids_changed || anim_kept)) {
        /* IF1: only the pictures of frames (or of animated images a frame shows) changed: the list on screen stays, and the
         * damage is the rects of the items that show them. */
        SDL_free(sbs);
        embi_damage_set_page_rects(&v->damage, nullptr, 0, v->scale, v->dev_w, v->dev_h);
        embi_frames_add_damage(v, &v->damage, &v->dl[v->cur]);
        if (anim_kept) {
            embi_gfx_anim_damage_keep(gfx_of(v), &v->damage, &v->dl[v->cur], v->scale, v->dev_w, v->dev_h);
        }
        v->st.paint_ms = ticks_to_ms(SDL_GetPerformanceCounter() - t0);
        return v->damage.any;
    }
    if (!v->paint_dirty && !v->force_full && !v->style_paint && !scroll_pending && !sb_dirty) {
        v->st.damage_rects = 0;
        SDL_free(sbs);
        /* CF2: a frame with nothing to paint while nothing animates is idle time: the realm does one step of the work the
         * next interaction would otherwise pay for (embi_doc_idle). */
        if (v->doc != nullptr && !embi_view_animating(v)) {
            (void)embi_doc_idle(v->doc);
        }
        return false;
    }

    /* The display list into the list not on screen, then its scrollbars after their content. */
    uint32_t next = 1u - v->cur;
    embi_dl *out = &v->dl[next];
    /* scroll60: a change of scroll offsets only, or of the scrollbars only, reuses the list on screen: its content is moved by its
     * scroll transforms in place and nothing is built. The scrollbars (which fade, and follow the offsets) are made again after
     * the content as for any list. */
    int reused = 0;
    double paint_ms = 0.0;
    embi_scroll_damage sdmg;
    memset(&sdmg, 0, sizeof sdmg);
    embi_ditem *old_tail = nullptr; /* the scrollbar items the list on screen has after its content */
    uint32_t old_n = 0;
    const bool in_place = (scroll_pending || sb_dirty) && !v->paint_dirty && !v->force_full && !v->style_paint && v->have_prev &&
                          v->content_len[v->cur] <= v->dl[v->cur].items.len;
    if (in_place) {
        const embi_dl *cl = &v->dl[v->cur];
        old_n = cl->items.len - v->content_len[v->cur];
        if (old_n > 0) {
            old_tail = SDL_malloc((size_t)old_n * sizeof *old_tail);
            EMBI_ASSERT(old_tail != nullptr);
            memcpy(old_tail, cl->items.data + v->content_len[v->cur], (size_t)old_n * sizeof *old_tail);
        }
        reused = embi_doc_paint_scroll(v->doc, &v->dl[v->cur], v->content_len[v->cur], (float)v->css_w, (float)v->css_h,
                                       EMBI_VIEW_FOCUS, &sdmg, &paint_ms);
    }
    if (reused != 0) {
        next = v->cur;
        out = &v->dl[next];
        out->items.len = v->content_len[next]; /* the page; its scrollbars follow */
    } else {
        /* A paint that only the update's paint-only changes asked for may rebuild just the contexts they touch. */
        bool incr = v->style_paint && !v->paint_dirty && !v->force_full && v->have_prev && !scroll_pending && !sb_dirty;
        paint_ms = embi_doc_paint_ex(v->doc, out, (float)v->css_w, (float)v->css_h, EMBI_VIEW_FOCUS, true, incr);
        v->content_len[next] = out->items.len;
    }
    v->st.scroll_reused = reused == 2;
    embi_doc_emit_scrollbars(v->doc, out, sbs, nsb);
    SDL_free(sbs);
    v->st.display_list_ms = paint_ms;
    v->st.items = out->items.len;

    /* The damage against the list on screen, which is compared only when it belongs to the same layout. */
    bool full = v->force_full || !v->have_prev;
    uint64_t td0 = SDL_GetPerformanceCounter();
    if (reused != 0) {
        /* The content moved or stayed: the damage is the boxes of the containers that moved, and the scrollbar items that
         * differ from the ones on screen (before and after). The transforms differ in every item of the moved content, so a
         * compare would only say so. */
        enum { RR_MAX = 64 };
        float rr[RR_MAX][4];
        uint32_t nr = 0;
        bool whole = reused == 2 && sdmg.full;
        if (reused == 2 && !whole) {
            memcpy(rr, sdmg.r, (size_t)sdmg.n * sizeof rr[0]);
            nr = sdmg.n;
        }
        const embi_ditem *nt = out->items.data + v->content_len[next];
        uint32_t new_n = out->items.len - v->content_len[next];
        if (old_n != new_n || (new_n > 0 && memcmp(old_tail, nt, (size_t)new_n * sizeof *nt) != 0)) {
            for (uint32_t i = 0; i < old_n + new_n && !whole; i++) {
                const embi_ditem *it = i < old_n ? &old_tail[i] : &nt[i - old_n];
                if (nr >= RR_MAX) {
                    whole = true;
                    break;
                }
                rr[nr][0] = it->rect[0];
                rr[nr][1] = it->rect[1];
                rr[nr][2] = it->rect[0] + it->rect[2];
                rr[nr][3] = it->rect[1] + it->rect[3];
                nr++;
            }
        }
        if (whole) {
            embi_damage_set_full(&v->damage, v->dev_w, v->dev_h, out->items.len);
        } else {
            embi_damage_set_page_rects(&v->damage, rr, nr, v->scale, v->dev_w, v->dev_h);
        }
    } else {
        embi_damage_compute(&v->damage, v->have_prev ? &v->dl[v->cur] : nullptr, out, v->scale, v->dev_w, v->dev_h, full);
    }
    SDL_free(old_tail);
    v->st.damage_ms = ticks_to_ms(SDL_GetPerformanceCounter() - td0);
    if (v->gfx != nullptr) {
        /* An animated image that changed frame has the same item hash: its rects are damaged by URL. A frame leaves the
         * changes to its root, which forgets them after its frames have seen them. */
        if (child) {
            embi_gfx_anim_damage_keep(gfx_of(v), &v->damage, out, v->scale, v->dev_w, v->dev_h);
        } else {
            embi_gfx_anim_damage(gfx_of(v), &v->damage, out, v->scale, v->dev_w, v->dev_h);
        }
    }
    /* IF1: a frame whose picture changed has the same item: its rect is damaged by name. */
    embi_frames_add_damage(v, &v->damage, out);
    v->cur = next;
    v->have_prev = true;
    v->prev_serial = lay_serial;
    v->prev_runs = run_epoch;
    v->paint_dirty = false;
    v->style_paint = false;
    v->force_full = false;
    v->st.paint_ms = ticks_to_ms(SDL_GetPerformanceCounter() - t0);
    return v->damage.any;
}

bool embi_view_frame(emb_view *v, SDL_GPUCommandBuffer *cmd)
{
    bool changed = embi_view_update(v);
    /* IF1: the frames draw into their textures first: this view's list samples them. */
    embi_frames_draw(v, cmd);
    return embi_view_draw(v, cmd, changed);
}

bool embi_view_draw(emb_view *v, SDL_GPUCommandBuffer *cmd, bool changed)
{
    v->st.damage_rects = changed ? v->damage.rects.len : 0;
    v->st.damage_area = changed ? v->damage.area : 0;
    v->st.full_redraw = changed && v->damage.full;
    if (changed && v->tex == nullptr) {
        v->force_full = true;
        v->paint_dirty = true;
    }
    if (!changed || v->tex == nullptr) {
        if (embi_frames_parent(v) == nullptr) {
            embi_gfx_trim_idle(v->gfx);
        }
        /* Nothing was recorded: the GPU stats of this frame are zero, not the last frame's. */
        v->st.upload_ms = 0.0;
        v->st.gpu_ms = 0.0;
        v->st.layers = 0;
        v->st.draw_calls = 0;
        v->st.items_recorded = 0;
        v->st.items_culled = 0;
        v->st.batches = 0;
        return false;
    }
    embi_gfx_target t;
    t.tex = v->tex;
    t.w = v->dev_w;
    t.h = v->dev_h;
    t.scale = v->scale;
    /* A frame's canvas is transparent (IF1): the parent shows through where its document paints nothing; so is the canvas of
     * an offscreen view made transparent (emb_view_desc.transparent). */
    const bool child = embi_frames_parent(v) != nullptr;
    t.clear_rgba = child || v->transparent ? 0u : embi_doc_canvas_rgba(v->doc);
    t.text_aa = resolve_text_aa(v);
    t.animating = embi_view_animating(v);
    embi_gfx_stats gs;
    memset(&gs, 0, sizeof gs);
    bool ok = embi_gfx_draw(gfx_of(v), cmd, &t, &v->dl[v->cur], &v->damage, embi_doc_group_mode, &gs);
    v->st.upload_ms = gs.upload_ms;
    v->st.gpu_ms = gs.record_ms;
    v->st.layers = gs.layers;
    v->st.draw_calls = gs.draw_calls;
    v->st.items_recorded = gs.items_recorded;
    v->st.items_culled = gs.items_culled;
    v->st.batches = gs.batches;
    if (!child) {
        embi_gfx_trim(v->gfx); /* between frames: the root trims once its frames have drawn */
    }
    if (!ok) {
        /* The damage is consumed but the texture was not drawn: the next frame redraws everything. */
        v->force_full = true;
        v->have_prev = false;
        v->paint_dirty = true;
        embi_rt_log(v->rt, EMB_LOG_WARN, "a frame could not be started");
    }
    return true;
}

bool emb_view_render(emb_view *view, SDL_GPUCommandBuffer *cmd)
{
    if (view == nullptr || cmd == nullptr || view->mode != EMB_VIEW_OFFSCREEN) {
        return false;
    }
    return embi_view_frame(view, cmd);
}

void emb_view_stats_get(const emb_view *view, emb_view_stats *out)
{
    if (view == nullptr || out == nullptr) {
        return;
    }
    /* The caller's size field says how much of the struct it has: only that much is written. */
    uint32_t caller_size = out->size;
    size_t n = caller_size < sizeof view->st ? caller_size : sizeof view->st;
    if (n < sizeof(uint32_t)) {
        return;
    }
    memcpy(out, &view->st, n);
    out->size = caller_size;
}
