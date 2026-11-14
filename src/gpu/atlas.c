#include "gpu/atlas.h"

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

/* Every image occupies a slot one texel larger on each side (the extrusion border), so a slot is
 * (w + 2) x (h + 2). Shared pages are packed in shelves: a shelf is a row of slots that share one
 * height, and a new shelf opens below the last one when an image is taller than the current
 * shelf or does not fit beside the slots already placed in it.
 *
 * Shared pages are layers of one 2D array texture (a->array). A shared page keeps its layer for
 * as long as the atlas lives, so the layer count is the number of shared pages ever created (at
 * most EMBI_ATLAS_MAX_SHARED). The array grows by copying its used layers, lazily (see
 * embi_atlas_grow and embi_atlas_flush). A solo page is a texture of its own, one layer, sized to
 * its image. */

typedef struct embi_atlas_page {
    bool used;        /* the slot holds a page. An unused slot is reused by the next new page */
    bool solo;        /* one image, sized to it, in its own texture */
    SDL_GPUTexture *tex; /* solo pages only */
    uint32_t layer;   /* shared pages only: the layer in a->array */
    uint32_t w;
    uint32_t h;
    uint32_t live;    /* regions still allocated from this page */
    uint32_t shelf_y; /* top of the current shelf */
    uint32_t shelf_h; /* height of the current shelf, 0 for an empty page */
    uint32_t shelf_x; /* next free x in the current shelf */
} embi_atlas_page;

struct embi_atlas {
    SDL_GPUDevice *dev;
    SDL_GPUTexture *array; /* the shared pages, one layer each. nullptr until the first one */
    uint32_t array_layers; /* layers the array holds */
    uint32_t shared;       /* shared pages created, so the layers in use */
    /* A growth whose layer copies are not recorded yet: the array that holds the contents of
     * layers [0, old_layers_used). Nothing is written to array until old is flushed. */
    SDL_GPUTexture *old;
    uint32_t old_layers_used;
    EMBI_VEC(embi_atlas_page) pages;
};

/* A 2D array texture with `layers` layers of w x h. Every atlas texture is an array, so the
 * renderer samples solo pages and shared pages through one binding. */
static SDL_GPUTexture *embi_atlas_make_tex(SDL_GPUDevice *dev, uint32_t w, uint32_t h,
                                           uint32_t layers)
{
    SDL_GPUTexture *t = SDL_CreateGPUTexture(
        dev, &(SDL_GPUTextureCreateInfo){
                 .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
                 .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                 .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                 .width = w,
                 .height = h,
                 .layer_count_or_depth = layers,
                 .num_levels = 1,
                 .sample_count = SDL_GPU_SAMPLECOUNT_1,
             });
    if (t == nullptr) {
        embi_log_error("atlas: SDL_CreateGPUTexture(%u x %u x %u) failed: %s", w, h, layers,
                       SDL_GetError());
    }
    return t;
}

/* The layer count to grow to. The first layer is an exact fit; after that the array grows by about
 * half its size (at least one layer) and never less than `need`, and never past the cap. */
static uint32_t embi_atlas_next_layers(uint32_t cur, uint32_t need)
{
    uint32_t step = cur / 2u;
    if (step < 1u) {
        step = 1u;
    }
    uint32_t want = cur + step;
    if (want < need) {
        want = need;
    }
    if (want > EMBI_ATLAS_MAX_SHARED) {
        want = EMBI_ATLAS_MAX_SHARED;
    }
    return want;
}

/* Makes the array hold at least `need` layers. The new array is created empty; no command is
 * recorded here, because there is no command buffer to record into. The layers of the old array
 * are copied by embi_atlas_flush, on the next command buffer that uploads or draws.
 *
 * If a copy is already pending, the array in a->array has no contents yet (every upload flushes
 * first), so it is dropped, and the pending source stays the one that holds the contents. */
static bool embi_atlas_grow(embi_atlas *a, uint32_t need)
{
    if (need <= a->array_layers) {
        return true;
    }
    const uint32_t want = embi_atlas_next_layers(a->array_layers, need);
    if (want < need) {
        return false;
    }
    SDL_GPUTexture *nt = embi_atlas_make_tex(a->dev, EMBI_ATLAS_PAGE, EMBI_ATLAS_PAGE, want);
    if (nt == nullptr) {
        return false;
    }
    if (a->array != nullptr) {
        if (a->old == nullptr && a->shared > 0) {
            /* The array holds the contents; it stays the source until flushed. */
            a->old = a->array;
            a->old_layers_used = a->shared;
        } else {
            /* Either a copy is already pending (this array has no contents), or nothing is used. */
            SDL_ReleaseGPUTexture(a->dev, a->array);
        }
    }
    a->array = nt;
    a->array_layers = want;
    return true;
}

bool embi_atlas_flush(embi_atlas *a, SDL_GPUCommandBuffer *cmd)
{
    if (a == nullptr || a->old == nullptr) {
        return true;
    }
    if (cmd == nullptr || a->array == nullptr) {
        return false;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (cp == nullptr) {
        embi_log_error("atlas: cannot start the growth copy: %s", SDL_GetError());
        return false;
    }
    for (uint32_t l = 0; l < a->old_layers_used; l++) {
        const SDL_GPUTextureLocation src = {.texture = a->old, .layer = l};
        const SDL_GPUTextureLocation dst = {.texture = a->array, .layer = l};
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, EMBI_ATLAS_PAGE, EMBI_ATLAS_PAGE, 1, false);
    }
    SDL_EndGPUCopyPass(cp);
    /* SDL keeps the old array alive until the GPU has run the copy. */
    SDL_ReleaseGPUTexture(a->dev, a->old);
    a->old = nullptr;
    a->old_layers_used = 0;
    return true;
}

/* Index of a free page slot, appending one when none is free. UINT32_MAX on failure. */
static uint32_t embi_atlas_new_slot(embi_atlas *a)
{
    for (uint32_t i = 0; i < a->pages.len; i++) {
        if (!a->pages.data[i].used) {
            return i;
        }
    }
    embi_atlas_page empty = {0};
    embi_vec_push(&a->pages, empty);
    return a->pages.len - 1;
}

/* Places an sw x sh slot in a shared page's shelves. Returns false when it does not fit. */
static bool embi_atlas_place(embi_atlas_page *p, uint32_t sw, uint32_t sh, uint32_t *ox,
                             uint32_t *oy)
{
    if (sw > p->w || sh > p->h) {
        return false;
    }
    if (sh <= p->shelf_h && p->shelf_x + sw <= p->w) {
        *ox = p->shelf_x;
        *oy = p->shelf_y;
        p->shelf_x += sw;
        return true;
    }
    const uint32_t ny = p->shelf_y + p->shelf_h;
    if (ny + sh <= p->h) {
        p->shelf_y = ny;
        p->shelf_h = sh;
        p->shelf_x = sw;
        *ox = 0;
        *oy = ny;
        return true;
    }
    return false;
}

embi_atlas *embi_atlas_create(SDL_GPUDevice *dev)
{
    if (dev == nullptr) {
        return nullptr;
    }
    embi_atlas *a = embi_calloc(1, sizeof *a);
    a->dev = dev;
    return a;
}

void embi_atlas_destroy(embi_atlas *a)
{
    if (a == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < a->pages.len; i++) {
        if (a->pages.data[i].used && a->pages.data[i].solo) {
            SDL_ReleaseGPUTexture(a->dev, a->pages.data[i].tex);
        }
    }
    if (a->array != nullptr) {
        SDL_ReleaseGPUTexture(a->dev, a->array);
    }
    if (a->old != nullptr) {
        SDL_ReleaseGPUTexture(a->dev, a->old);
    }
    embi_vec_free(&a->pages);
    embi_free(a, sizeof *a);
}

SDL_GPUTexture *embi_atlas_texture(const embi_atlas *a)
{
    return a == nullptr ? nullptr : a->array;
}

bool embi_atlas_alloc(embi_atlas *a, uint32_t w, uint32_t h, embi_atlas_region *out)
{
    if (a == nullptr || out == nullptr || w == 0 || h == 0 || w > EMBI_ATLAS_MAX_SIDE ||
        h > EMBI_ATLAS_MAX_SIDE) {
        return false;
    }
    const uint32_t sw = w + 2u;
    const uint32_t sh = h + 2u;
    uint32_t pi = UINT32_MAX;
    uint32_t ox = 0;
    uint32_t oy = 0;

    if (w <= EMBI_ATLAS_SOLO_SIDE && h <= EMBI_ATLAS_SOLO_SIDE) {
        for (uint32_t i = 0; i < a->pages.len && pi == UINT32_MAX; i++) {
            embi_atlas_page *p = &a->pages.data[i];
            if (!p->used || p->solo) {
                continue;
            }
            if (embi_atlas_place(p, sw, sh, &ox, &oy)) {
                pi = i;
            }
        }
        if (pi == UINT32_MAX) {
            if (a->shared >= EMBI_ATLAS_MAX_SHARED) {
                return false;
            }
            if (!embi_atlas_grow(a, a->shared + 1u)) {
                return false;
            }
            const uint32_t slot = embi_atlas_new_slot(a);
            if (slot == UINT32_MAX) {
                return false;
            }
            embi_atlas_page *np = &a->pages.data[slot];
            *np = (embi_atlas_page){
                .used = true, .solo = false, .layer = a->shared, .w = EMBI_ATLAS_PAGE,
                .h = EMBI_ATLAS_PAGE};
            a->shared++;
            if (!embi_atlas_place(np, sw, sh, &ox, &oy)) {
                /* Unreachable: a slot is far smaller than a page. Keep the page, fail the call. */
                return false;
            }
            pi = slot;
        }
    } else {
        const uint32_t slot = embi_atlas_new_slot(a);
        if (slot == UINT32_MAX) {
            return false;
        }
        SDL_GPUTexture *t = embi_atlas_make_tex(a->dev, sw, sh, 1);
        if (t == nullptr) {
            return false;
        }
        a->pages.data[slot] = (embi_atlas_page){
            .used = true, .solo = true, .tex = t, .w = sw, .h = sh};
        pi = slot;
    }

    embi_atlas_page *p = &a->pages.data[pi];
    p->live++;
    out->tex = p->solo ? p->tex : a->array;
    out->atlas = p->solo ? nullptr : a;
    out->page = pi;
    out->layer = p->solo ? 0u : p->layer;
    out->x = (uint16_t)(ox + 1u);
    out->y = (uint16_t)(oy + 1u);
    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    out->page_w = (uint16_t)p->w;
    out->page_h = (uint16_t)p->h;
    return true;
}

void embi_atlas_free(embi_atlas *a, const embi_atlas_region *r)
{
    if (a == nullptr || r == nullptr || r->page >= a->pages.len) {
        return;
    }
    embi_atlas_page *p = &a->pages.data[r->page];
    if (!p->used || p->live == 0) {
        return;
    }
    if (p->solo) {
        if (r->atlas != nullptr || p->tex != r->tex) {
            return;
        }
    } else if (r->atlas != a || p->layer != r->layer) {
        return;
    }
    p->live--;
    if (p->solo) {
        SDL_ReleaseGPUTexture(a->dev, p->tex);
        *p = (embi_atlas_page){0};
    } else if (p->live == 0) {
        /* Every slot in the page is free, so reuse it from the top. */
        p->shelf_x = 0;
        p->shelf_y = 0;
        p->shelf_h = 0;
    }
}

bool embi_atlas_upload(embi_atlas *a, const embi_atlas_region *r, const uint8_t *pixels,
                       uint32_t stride, SDL_GPUCommandBuffer *cmd)
{
    if (a == nullptr || r == nullptr || pixels == nullptr || cmd == nullptr || r->w == 0 ||
        r->h == 0 || stride < (uint32_t)r->w * 4u || r->page >= a->pages.len) {
        return false;
    }
    const embi_atlas_page *pg = &a->pages.data[r->page];
    if (!pg->used) {
        return false;
    }
    if (pg->solo ? (r->atlas != nullptr || pg->tex != r->tex)
                 : (r->atlas != a || pg->layer != r->layer)) {
        return false;
    }
    /* The slot is the image plus its extrusion border, so it must fit inside the page the region
     * names: x - 1 >= 0, y - 1 >= 0, and the border's far edge inside the page. */
    if (r->x == 0 || r->y == 0 || (uint32_t)r->x + r->w + 1u > pg->w ||
        (uint32_t)r->y + r->h + 1u > pg->h || r->page_w != pg->w || r->page_h != pg->h) {
        embi_log_error("atlas: region %u x %u at (%u, %u) is outside its page", r->w, r->h,
                       (unsigned)r->x, (unsigned)r->y);
        return false;
    }
    const uint32_t w = r->w;
    const uint32_t h = r->h;
    const uint32_t sw = w + 2u;
    const uint32_t sh = h + 2u;
    const size_t bytes = (size_t)sw * (size_t)sh * 4u;
    if (bytes > UINT32_MAX) {
        embi_log_error("atlas: upload of %u x %u needs %llu bytes", w, h,
                       (unsigned long long)bytes);
        return false;
    }
    /* Growth copies pending on this atlas are recorded into cmd first. Uploads recorded into cmd
     * before the growth went to the old array, so the copy runs after them and carries them over,
     * and this upload lands in the current array. */
    if (!embi_atlas_flush(a, cmd)) {
        return false;
    }

    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(
        a->dev, &(SDL_GPUTransferBufferCreateInfo){
                    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                    .size = (Uint32)bytes,
                });
    if (tb == nullptr) {
        embi_log_error("atlas: SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return false;
    }
    uint8_t *m = SDL_MapGPUTransferBuffer(a->dev, tb, false);
    if (m == nullptr) {
        embi_log_error("atlas: SDL_MapGPUTransferBuffer failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(a->dev, tb);
        return false;
    }

    /* Premultiply while copying. The border rows and columns repeat the nearest image texel. */
    for (uint32_t y = 0; y < sh; y++) {
        const uint32_t sy = (y == 0) ? 0u : (y > h ? h - 1u : y - 1u);
        const uint8_t *src_row = pixels + (size_t)sy * stride;
        uint8_t *dst_row = m + (size_t)y * sw * 4u;
        for (uint32_t x = 0; x < sw; x++) {
            const uint32_t sx = (x == 0) ? 0u : (x > w ? w - 1u : x - 1u);
            const uint8_t *s = src_row + (size_t)sx * 4u;
            uint8_t *d = dst_row + (size_t)x * 4u;
            const uint32_t al = s[3];
            d[0] = (uint8_t)((s[0] * al + 127u) / 255u);
            d[1] = (uint8_t)((s[1] * al + 127u) / 255u);
            d[2] = (uint8_t)((s[2] * al + 127u) / 255u);
            d[3] = (uint8_t)al;
        }
    }
    SDL_UnmapGPUTransferBuffer(a->dev, tb);

    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (cp == nullptr) {
        embi_log_error("atlas: SDL_BeginGPUCopyPass failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(a->dev, tb);
        return false;
    }
    const SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = tb,
        .offset = 0,
        .pixels_per_row = sw,
        .rows_per_layer = sh,
    };
    const SDL_GPUTextureRegion dst = {
        .texture = pg->solo ? pg->tex : a->array,
        .mip_level = 0,
        .layer = pg->solo ? 0u : pg->layer,
        .x = r->x - 1u,
        .y = r->y - 1u,
        .z = 0,
        .w = sw,
        .h = sh,
        .d = 1,
    };
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    /* SDL keeps the transfer buffer alive until the copy has run. */
    SDL_ReleaseGPUTransferBuffer(a->dev, tb);
    return true;
}
