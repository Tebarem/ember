#pragma once
/*
 * Image atlas: packs decoded images into RGBA8 texture pages for the renderer.
 *
 *     embi_atlas *a = embi_atlas_create(dev);
 *     embi_atlas_region reg;
 *     embi_atlas_alloc(a, w, h, &reg);            // reserves a slot, creates the page if needed
 *     embi_atlas_upload(a, &reg, px, stride, cmd); // copy pass, call outside any render pass
 *     embi_frame_image(f, rect, radii, reg, uv, tint, xform, clip);
 *     embi_atlas_free(a, &reg);                    // when the image is no longer needed
 *
 * Layout. Shared pages are 2048 x 2048 RGBA8 and hold premultiplied pixels. All shared pages are
 * the layers of one 2D array texture (embi_atlas_texture), so images on different pages draw in
 * one batch: the renderer samples the layer the region names. The array holds as many layers as
 * it needs, grown as it needs them, up to EMBI_ATLAS_MAX_SHARED pages. Images with a side above
 * EMBI_ATLAS_SOLO_SIDE get a solo page of their own, sized to the image, as a one-layer array of
 * their own. Each image is stored with a one-texel border on every side, filled with copies of the
 * edge texels (extrusion). Bilinear sampling at the image edge therefore reads only the image's
 * own edge colors, and neighboring images never leak in.
 *
 * Growth. Adding a layer past the array's size makes a larger array (exact fit for the first
 * layer, then geometric: about 1.5x, see embi_atlas_alloc). The copy of the used layers into it is
 * lazy. Growth itself records nothing, because embi_atlas_alloc has no command buffer. The old
 * array stays the source until the next embi_atlas_upload or embi_atlas_flush on a command buffer,
 * which records the layer copies into that buffer first, then does its own work. Commands recorded
 * earlier into the same buffer run before the copy, so they land in the old array and the copy
 * carries them over. Submit a buffer's uploads in the order they were recorded: an upload recorded
 * into a buffer submitted after the copy writes the old array and is lost. The renderer flushes
 * every atlas a frame samples at the frame's end, so a growth without a later upload is safe. The
 * region's tex is the array as of its allocation and may be stale after a growth: use
 * embi_atlas_texture, or let embi_frame_image resolve it, which the renderer does.
 *
 * Memory. Each shared page is 16 MiB (2048 x 2048 RGBA8), and the first page is the array's only
 * layer, so one small image costs one page, not a larger array. A growth holds the old and the new
 * array until the copy is recorded: the last step to the cap (63 to 64 layers) peaks at about
 * 2 GiB of VRAM. Lower EMBI_ATLAS_MAX_SHARED if that is too much for a target.
 *
 * Lifetime. A region is valid until embi_atlas_free. A freed region of a shared page leaves its
 * space unused until every region of that page is freed, then the page is reused from the top.
 * Do not free a region that an open frame still draws. Freeing between frames is always safe.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#define EMBI_ATLAS_PAGE 2048u      /* side of a shared page, texels */
#define EMBI_ATLAS_SOLO_SIDE 1024u /* an image with a side above this gets a solo page */
#define EMBI_ATLAS_MAX_SIDE 8192u  /* largest image side the atlas accepts */
#define EMBI_ATLAS_MAX_SHARED 64u  /* most shared pages (array layers): 1 GiB of RGBA8 at most */

typedef struct embi_atlas embi_atlas;

typedef struct embi_atlas_region {
    SDL_GPUTexture *tex;     /* solo page: its texture. Shared page: the array when allocated */
    embi_atlas *atlas;       /* shared page: the atlas that holds it. nullptr for a solo page */
    uint32_t page;           /* page slot inside the atlas */
    uint32_t layer;          /* shared page: its layer in the array. 0 for a solo page */
    uint16_t x, y;           /* top-left texel of the image inside the page */
    uint16_t w, h;           /* image size in texels */
    uint16_t page_w;         /* page size in texels */
    uint16_t page_h;
} embi_atlas_region;

[[nodiscard]] embi_atlas *embi_atlas_create(SDL_GPUDevice *dev);
/* Releases every page. SDL keeps the textures alive until the GPU no longer uses them. */
void embi_atlas_destroy(embi_atlas *a);

/* The array texture holding the shared pages, or nullptr before the first shared page. Its layer
 * count is at least the number of shared pages, and it changes when the array grows. */
[[nodiscard]] SDL_GPUTexture *embi_atlas_texture(const embi_atlas *a);

/* Reserves a w x h slot. Returns false for an empty or oversized image, when the GPU cannot create
 * the page, or when a new shared page would pass EMBI_ATLAS_MAX_SHARED. */
[[nodiscard]] bool embi_atlas_alloc(embi_atlas *a, uint32_t w, uint32_t h,
                                    embi_atlas_region *out);

/* Releases a region from embi_atlas_alloc. Each region must be freed exactly once: a second
 * free is not detected while another region still shares its page. */
void embi_atlas_free(embi_atlas *a, const embi_atlas_region *r);

/* Uploads w x h non-premultiplied RGBA8 pixels (rows `stride` bytes apart) into the region,
 * premultiplying them and filling the extrusion border. The copy pass is recorded into cmd, so
 * call this outside any render pass. Returns false on a bad argument or a GPU failure. */
[[nodiscard]] bool embi_atlas_upload(embi_atlas *a, const embi_atlas_region *r,
                                     const uint8_t *pixels, uint32_t stride,
                                     SDL_GPUCommandBuffer *cmd);

/* Records the pending layer copies of a growth into cmd (see Growth above), then releases the old
 * array. A no-op when nothing is pending. Call it outside any render pass. Returns false when a
 * copy could not be recorded: the copies stay pending, and a later flush retries them. Internal
 * (the renderer flushes the atlases a frame samples before it binds them). */
[[nodiscard]] bool embi_atlas_flush(embi_atlas *a, SDL_GPUCommandBuffer *cmd);
