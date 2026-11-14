#pragma once
/*
 * Image cache (module image): decoded images keyed by URL, with a state (none, pending, ready,
 * failed), the natural size, pixels under an LRU byte budget, and an optional GPU upload hook.
 *
 * Threading. Single-threaded: every call comes from the view's main thread. Decoding is a pure
 * function of the bytes (embi_png_decode), so a job system can run it on a worker later. Only
 * embi_image_cache_finish would then change: it would queue the decode and return PENDING.
 *
 * Pixels. The cache stores straight (non-premultiplied) RGBA8, because the atlas upload
 * premultiplies while copying. Decoders that output premultiplied pixels (embi_png_decode with
 * premultiply set) are for tests and direct CPU use only.
 *
 * GPU. The atlas (src/gpu/atlas.h) belongs to the paint layer, which links gpu. This module only
 * sees the hook (embi_image_gpu), so it links and tests without a device. The paint layer's hook
 * calls embi_atlas_alloc and embi_atlas_upload.
 *
 * Eviction. Decoded pixels are the only budgeted bytes. The encoded bytes stay in the entry so an
 * evicted image can be decoded again on demand; their total is reported separately by
 * embi_image_cache_encoded_bytes and is not counted against the budget, so memory held by the
 * cache is at most budget + encoded bytes. Eviction runs in embi_image_cache_trim only, which the
 * embedder calls between frames. An image larger than the budget is kept while it is used between
 * two trims, so it is not decoded again on every frame. A GPU handle is released with its pixels,
 * so trimming during an open frame would invalidate a handle that frame still draws.
 *
 * Formats. The format is taken from the magic bytes, never from the URL. PNG uses the built-in
 * decoder (src/image/png.c). JPEG and GIF use SDL_image (stb_image for JPEG; its GIF decoder),
 * built static with no other codecs (src/image/module.cmake). Only the first GIF frame is decoded,
 * through the animation decoder (IMG_CreateAnimationDecoder_IO), which never falls back into the
 * GIF loader. An animated GIF also keeps all its frames (up to 8 MiB per image and 16 MiB per cache; more stays a still) and plays through the
 * frame clock: see embi_image_cache_tick. JPEG pixels get the EXIF
 * orientation applied, so the natural size is the oriented size (see
 * embi_image_cache_set_apply_orientation). ICC profiles are ignored for every format, and the
 * pixels are straight RGBA8 with no color conversion.
 *
 * Failure states. FAILED means the bytes were bad, the fetch failed, a header was over the size
 * limits, or an embedder decoder refused them. UNSUPPORTED means no decoder knows the format
 * (anything but PNG, JPEG and GIF, without an embedder callback). Both paint nothing. A FAILED or
 * UNSUPPORTED url is retried only after embi_image_cache_forget.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#define EMBI_IMAGE_MAX_DIM 16384u
#define EMBI_IMAGE_DEFAULT_BUDGET ((size_t)64 << 20)
#define EMBI_IMAGE_DEFAULT_MAX_PIXELS ((uint64_t)1 << 25)

/* Decoded image. px is RGBA8, row-major from the top row, stride w * 4, allocated with embi_alloc
 * (w * h * 4 bytes). The owner releases it with embi_pixels_free. */
typedef struct embi_pixels {
    uint32_t w;
    uint32_t h;
    uint8_t *px;
} embi_pixels;

void embi_pixels_free(embi_pixels *p);

typedef enum embi_image_state {
    EMBI_IMAGE_NONE,    /* never requested */
    EMBI_IMAGE_PENDING, /* requested, bytes not yet supplied */
    EMBI_IMAGE_READY,   /* decoded; the size is valid */
    EMBI_IMAGE_FAILED,      /* undecodable, fetch failed, or refused by the embedder; paints nothing */
    EMBI_IMAGE_UNSUPPORTED, /* a format no decoder knows (not PNG, JPEG or GIF; no embedder callback) */
} embi_image_state;

/* Formats known by their magic bytes. */
typedef enum embi_image_format {
    EMBI_IMAGE_FMT_OTHER,
    EMBI_IMAGE_FMT_PNG,  /* 89 'PNG' CR LF 1A LF */
    EMBI_IMAGE_FMT_JPEG, /* FF D8 FF */
    EMBI_IMAGE_FMT_GIF,  /* 'GIF87a' or 'GIF89a' */
} embi_image_format;

[[nodiscard]] embi_image_format embi_image_sniff(const uint8_t *in, size_t len);

typedef struct embi_image_cache embi_image_cache;

/* Embedder decoder for formats the cache does not know (anything but PNG, JPEG and GIF). Called
 * with the encoded bytes. On success it fills *out with straight RGBA8 pixels allocated with
 * embi_alloc(w * h * 4) and returns true. On failure it leaves *out untouched and returns false. */
typedef bool (*embi_image_decoder_fn)(void *user, const uint8_t *in, size_t len, embi_pixels *out);

/* Decodes encoded bytes to straight RGBA8, dispatching on the magic bytes (embi_image_sniff):
 * PNG to the built-in decoder, JPEG and GIF to SDL_image, anything else to fallback when set.
 * Refuses a header whose sides exceed EMBI_IMAGE_MAX_DIM or whose area exceeds max_pixels before
 * any pixels are allocated. When apply_orientation is set, JPEG pixels (and PNG pixels with an eXIf chunk before the first IDAT) are rotated or flipped by
 * their EXIF orientation, so the size may swap. On success *out owns the pixels. On failure *out
 * is untouched and nothing is leaked. */
[[nodiscard]] bool embi_image_decode(const uint8_t *in, size_t len, uint64_t max_pixels,
                                     bool apply_orientation, embi_image_decoder_fn fallback,
                                     void *user, embi_pixels *out);

/* GPU hook, supplied by the paint layer, which owns the atlas. The hook is copied on set.
 *   upload   copies straight RGBA8 pixels (rows w * 4 bytes apart) to the GPU, recording the copy
 *            into cmd outside any render pass. Returns a nonzero handle, or 0 on failure.
 *   release  frees a handle from upload. Called on eviction, when the hook is replaced, and on
 *            destroy, so the state behind the hook must outlive the cache. */
typedef struct embi_image_gpu {
    void *user;
    uint64_t (*upload)(void *user, const uint8_t *px, uint32_t w, uint32_t h,
                       SDL_GPUCommandBuffer *cmd);
    void (*release)(void *user, uint64_t handle);
} embi_image_gpu;

[[nodiscard]] embi_image_cache *embi_image_cache_create(size_t budget_bytes);
/* Releases the pixels, the encoded bytes and every GPU handle. */
void embi_image_cache_destroy(embi_image_cache *c);
void embi_image_cache_set_budget(embi_image_cache *c, size_t bytes);
void embi_image_cache_set_max_pixels(embi_image_cache *c, uint64_t pixels);
void embi_image_cache_set_decoder(embi_image_cache *c, embi_image_decoder_fn fn, void *user);
/* Whether JPEG pixels get their EXIF orientation applied (default on, the browser's
 * image-orientation: from-image). Off gives the stored orientation (image-orientation: none). Set
 * it before the images it should affect are decoded; it does not re-decode cached entries. */
void embi_image_cache_set_apply_orientation(embi_image_cache *c, bool on);
/* Sets the GPU hook (nullptr clears it). Releases the handles the previous hook made. */
void embi_image_cache_set_gpu(embi_image_cache *c, const embi_image_gpu *gpu);

/* Marks url as requested. Does nothing when the url is already pending, ready or failed. */
void embi_image_cache_begin(embi_image_cache *c, embi_str url);
/* Supplies the encoded bytes (copied) and decodes them now. Returns READY or FAILED. */
embi_image_state embi_image_cache_finish(embi_image_cache *c, embi_str url, const uint8_t *bytes,
                                         size_t len);
/* The fetch failed. Drops any bytes and pixels and marks the url FAILED. */
void embi_image_cache_fail(embi_image_cache *c, embi_str url);
/* Forgets url entirely (state NONE, bytes, pixels and GPU handle released) so a FAILED or
 * UNSUPPORTED url can be requested again, for example after a fetch is retried. */
void embi_image_cache_forget(embi_image_cache *c, embi_str url);

/* Reports the state for url. w and h (either may be nullptr) get the natural size when READY and
 * zero otherwise. */
[[nodiscard]] embi_image_state embi_image_cache_query(const embi_image_cache *c, embi_str url,
                                                      uint32_t *w, uint32_t *h);
/* Straight RGBA8 pixels for a READY url, decoding again when they were evicted. Marks the image
 * as recently used. The pointer stays valid until the next call that changes the cache. */
[[nodiscard]] bool embi_image_cache_pixels(embi_image_cache *c, embi_str url, const uint8_t **px,
                                           uint32_t *w, uint32_t *h);
/* Uploads a READY url through the GPU hook on first call, recording the copy in cmd. Later calls
 * return the same handle. Call outside any render pass. Returns false when no hook is set, the url
 * is not ready, or the upload fails. */
[[nodiscard]] bool embi_image_cache_upload(embi_image_cache *c, embi_str url,
                                           SDL_GPUCommandBuffer *cmd, uint64_t *handle);
/* Evicts least-recently-used decoded images (and their GPU handles) until the decoded bytes fit
 * the budget. An image larger than the budget is exempt while it was used since the previous
 * trim. Call between frames. */
void embi_image_cache_trim(embi_image_cache *c);
/* Decoded pixel bytes currently held (counted against the budget). */
[[nodiscard]] size_t embi_image_cache_bytes(const embi_image_cache *c);
/* Encoded bytes kept for re-decoding. Not counted against the budget. */
[[nodiscard]] size_t embi_image_cache_encoded_bytes(const embi_image_cache *c);

/* ------------------------------------------------------------------ animated GIF (W6c)
 *
 * An animated GIF is READY as a still first, with the frames kept next to it. embi_image_cache_tick moves every
 * animated image to the frame clock (the view's rendering opportunity, the same clock as requestAnimationFrame): the
 * frame shown follows the delays (10 ms or less count as 100 ms), the loop count of the file (0: forever; a finite count
 * leaves the last frame), and a long pause skips whole plays. A change drops the GPU copy (the next draw uploads the new
 * frame) and calls cb with the URL, so the view can damage just the rects that draw it. Frames are not counted against the pixel budget but are capped (8 MiB per image, 16 MiB per cache, beyond which an image
 * stays a still); trim drops the frames of an image not drawn lately (the still stays). embi_image_cache_anim_bytes reports them. */
typedef void (*embi_image_frame_fn)(void *user, embi_str url);

/* Frames of a READY image: 1 for a still, 0 when it is not ready. */
[[nodiscard]] uint32_t embi_image_cache_frame_count(const embi_image_cache *c, embi_str url);
/* The index of the frame shown (0 for a still). */
[[nodiscard]] uint32_t embi_image_cache_frame(const embi_image_cache *c, embi_str url);
/* Moves the animated images to now_ms (the frame clock, ms). Returns how many changed their frame. The first tick after
 * an image is ready starts its clock. */
uint32_t embi_image_cache_tick(embi_image_cache *c, double now_ms, embi_image_frame_fn cb, void *user);
/* True while an animated image that was drawn lately (an upload or a pixel read within its current delay and a second of
 * the last tick) still has frames to show: the view needs another frame. */
[[nodiscard]] bool embi_image_cache_animating(const embi_image_cache *c, double now_ms);
[[nodiscard]] size_t embi_image_cache_anim_bytes(const embi_image_cache *c);
/* Overrides the frame-memory caps (per image, all images together). For tests. */
void embi_image_cache_set_anim_caps(embi_image_cache *c, size_t per_image, size_t total);
