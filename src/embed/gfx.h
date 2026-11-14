#pragma once
/*
 * The GPU side of a view (W4e): the renderer, the layer pool, the image atlas and the image
 * cache, and the replay of a display list into frames. Implemented in gfx.c, which includes the renderer headers, so
 * this header uses plain types only and the view never sees the renderer headers.
 */
#include "base/base.h"
#include "paint/damage.h"
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

typedef struct embi_gpu embi_gpu;
typedef struct embi_text embi_text;
typedef struct embi_gfx embi_gfx;
typedef struct embi_canvas_gpu embi_canvas_gpu;

typedef struct embi_gfx_cfg {
    embi_gpu *gpu;               /* not owned: the device wrapper of the view */
    SDL_GPUTextureFormat format; /* the format of every target this renderer draws into */
    embi_text *text;             /* not owned: the glyph outlines of the text runs */
    size_t image_bytes;          /* decoded image budget; 0 is 64 MiB */
    size_t layer_bytes;          /* idle layer texture budget; 0 is 64 MiB */
    void *user;                  /* for the two callbacks */
    /* Loads an absolute URL into SDL_malloc'd bytes (the caller frees them). */
    bool (*fetch)(void *user, embi_str url, uint8_t **data, size_t *len);
    /* Optional (N1d): counts the loads that answered "not yet" (a network image still on its way). A fetch that failed while
     * the count moved is not a failure: the image stays unrequested and is asked for again at the next layout or paint. */
    uint32_t (*pending_count)(void *user);
    /* The absolute form of a raw URL of the display list (owned by the caller of the view). */
    embi_str (*resolve)(void *user, embi_str raw);
} embi_gfx_cfg;

/* Returns nullptr when the renderer, the atlas or the layer pool cannot be created. */
embi_gfx *embi_gfx_create(const embi_gfx_cfg *cfg);
void embi_gfx_destroy(embi_gfx *gx);

/* Late binding of cfg.text: for a renderer created with cfg.text == nullptr (no other thread may use the text engine
 * during embi_gfx_create), set it afterwards on the thread that owns the text engine. */
void embi_gfx_attach_text(embi_gfx *gx, embi_text *text);

/* A texture usable as a frame target and as a sampled layer, in the renderer's format. */
SDL_GPUTexture *embi_gfx_texture_create(embi_gfx *gx, uint32_t w, uint32_t h);
/* The same on a device and a format, without a renderer (the view makes its target on a thread while the renderer is
 * still being made). Thread safe, like all SDL_gpu resource creation. */
SDL_GPUTexture *embi_gfx_texture_create_on(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h);
void embi_gfx_texture_destroy(embi_gfx *gx, SDL_GPUTexture *t);

/* The natural size of an absolute image URL, fetching and decoding it when it was never requested. Returns false
 * when the image is not ready (failed, unsupported, or it could not be fetched). */
bool embi_gfx_image_size(embi_gfx *gx, embi_str url, float *w, float *h, bool *has_ratio);
/* R4c: SVG images. The style engine SVG documents are styled with (the view's), the image behind an absolute URL (fetching
 * it when it was never requested; nullptr when it is not an SVG image), and the release of every parsed SVG document,
 * which must happen while the style engine is alive. */
struct embi_style_engine;
struct embi_svg_image;
void embi_gfx_set_style_engine(embi_gfx *gx, struct embi_style_engine *se);
const struct embi_svg_image *embi_gfx_svg_image(embi_gfx *gx, embi_str url);
void embi_gfx_svg_clear(embi_gfx *gx);

/* Straight RGBA8 pixels of an absolute image URL (fetching it when it was never requested); valid until the next call
 * that changes the cache. R4b: drawImage and createImageBitmap of an img element. */
bool embi_gfx_image_pixels(embi_gfx *gx, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h);
/* A number that changes whenever an image url may decode to different pixels (a fetch finished or the cache was reset). */
uint32_t embi_gfx_image_generation(const embi_gfx *gx);
/* The canvas texture manager of the view (R4b); nullptr when it could not be made. */
embi_canvas_gpu *embi_gfx_canvas(embi_gfx *gx);
/* Forgets every decoded image (and releases their atlas regions), so a new document fetches its images again. Call it
 * between frames. */
void embi_gfx_reset_images(embi_gfx *gx);
/* IF1: forgets the images the display list d shows (a frame's old document, before the frame loads another: the cache is
 * its root's, shared with the other documents of the tree, so it is not reset whole). The resolve hook must be the
 * list's view's. */
void embi_gfx_forget_images(embi_gfx *gx, const embi_dl *d);

/* W6c: animated GIF through the frame clock. embi_gfx_anim_tick moves the animated images to now_ms (the same clock as the
 * animation frame) and returns how many changed their frame; embi_gfx_anim_damage then adds, to the damage of the list
 * just painted, the rects of the image items that draw those images (and forgets the changes). embi_gfx_anim_active says
 * whether a drawn animated image still has frames to show (the view wants another frame). */
uint32_t embi_gfx_anim_tick(embi_gfx *gx, double now_ms);
void embi_gfx_anim_damage(embi_gfx *gx, embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w,
                          uint32_t view_h);
bool embi_gfx_anim_active(const embi_gfx *gx, double now_ms);
/* IF1: the frames of a view share their root view's renderer. embi_gfx_anim_pending says whether frame changes of animated
 * images wait for their damage, and embi_gfx_anim_damage_keep adds those rects to a frame's damage without forgetting them
 * (the root's embi_gfx_anim_damage, which runs after its frames, forgets them). */
bool embi_gfx_anim_pending(const embi_gfx *gx);
void embi_gfx_anim_damage_keep(embi_gfx *gx, embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w,
                               uint32_t view_h);
/* IF1: the user of the loader callbacks (cfg.user): a frame that shares the renderer sets itself before it paints or draws,
 * so the images of its list resolve against its own document. */
void embi_gfx_set_user(embi_gfx *gx, void *user);
/* IF1: the lookup of the iframes' textures. An image item whose URL is "ember-frame:<element address>" draws the texture fn
 * gives for the element (w x h device px), premultiplied, stretched over the item's rect; nothing when fn gives none. */
void embi_gfx_set_frame_lookup(embi_gfx *gx, SDL_GPUTexture *(*fn)(void *user, const void *key, uint32_t *w, uint32_t *h),
                               void *user);

/* Evicts decoded images over the budget, and the layer textures unused for EMBI_GFX_LAYER_KEEP_MS. Call between frames. */
void embi_gfx_trim(embi_gfx *gx);
/* A frame that drew nothing: frees the layer textures unused for EMBI_GFX_LAYER_KEEP_MS (a page that stopped changing gives
 * its layers back while the host keeps rendering). */
void embi_gfx_trim_idle(embi_gfx *gx);

typedef struct embi_gfx_target {
    SDL_GPUTexture *tex;
    uint32_t w, h;         /* device px of the target (the whole content) */
    float scale;           /* device px per CSS px */
    uint32_t clear_rgba;   /* the canvas color of a full redraw, non-premultiplied, r in the low byte */
    int text_aa;           /* 0 gray, 1 LCD RGB, 2 LCD BGR (embi_text_aa of gpu/render.h), already resolved */
    bool animating;        /* a transform animation runs: text under transforms is drawn gray and unfitted */
} embi_gfx_target;

typedef struct embi_gfx_stats {
    uint32_t layers;      /* intermediate layers drawn */
    uint32_t draw_calls;  /* draw calls of the frames */
    uint32_t items_recorded; /* drawable items replayed into a frame (the culled ones are not counted) */
    uint32_t items_culled;   /* drawable items skipped because their bounds miss the region the frame draws */
    uint32_t batches;     /* instanced batches of the frames */
    double record_ms;     /* time spent recording and ending frames, uploads included */
    double upload_ms;     /* the part of record_ms spent uploading decoded images to the atlas */
} embi_gfx_stats;

/* The layer decision of W4d for the group between LAYER_BEGIN at begin and LAYER_END at end: 0 empty, 1 fold, 2 target
 * (the numbers of enum embi_group_mode). */
typedef int (*embi_gfx_group_fn)(const embi_dl *d, uint32_t begin, uint32_t end);

/* Draws the display list d into the target. A full damage (dm->full) redraws the whole target, cleared to clear_rgba;
 * otherwise each damage rect is redrawn over the target's pixels, with the rect cleared first. Nothing is recorded when
 * dm->any is false. Records into cmd; the caller submits it. Returns false when a frame could not be started. */
bool embi_gfx_draw(embi_gfx *gx, SDL_GPUCommandBuffer *cmd, const embi_gfx_target *t, const embi_dl *d,
                   const embi_damage *dm, embi_gfx_group_fn group, embi_gfx_stats *st);

/* Copies the sw x sh texture src over the whole dst (dw x dh device px), in the renderer's format. */
bool embi_gfx_blit(embi_gfx *gx, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t sw, uint32_t sh,
                   SDL_GPUTexture *dst, uint32_t dw, uint32_t dh);
