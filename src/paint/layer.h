#pragma once
/*
 * Layer decisions and the cached-layer bookkeeping of W4d. Everything here is a
 * function of the display list (paint.h) or of styles and sizes, so the decisions are testable on the CPU:
 *
 *  - an opacity group (LAYER_BEGIN .. LAYER_END) folds its alpha into its items when it has one item, or several
 *    items that do not overlap; it needs an intermediate target when they overlap (embi_layer_group_mode);
 *  - a box gets a cached layer when it has will-change: transform or opacity, when it is position: fixed, or when
 *    it is a scroll container whose content overflows it by more than EMBI_LAYER_SCROLL_THRESHOLD px
 *    (embi_layer_wants_cache). A cached layer's texture survives frames: embi_layer_cache says whether a frame must
 *    record it again or can composite the texture it has.
 *
 * Item bounds. An item's bounds are its rect mapped by its transform (the axis-aligned box of the four corners),
 * optionally intersected with the clip chain (each clip's rect mapped by the clip's own transform). Both live in
 * the canvas space of the display list, in page px.
 */
#include "paint/paint.h"

#include <stdbool.h>
#include <stdint.h>

struct embi_style;

/* A scroll container whose content overflows its padding box by more than this many px in one axis gets a cached
 * layer (W4d). */
#define EMBI_LAYER_SCROLL_THRESHOLD 128.0f

/* How an opacity group is painted. */
enum embi_group_mode {
    EMBI_GROUP_EMPTY = 0, /* no drawable item: nothing to paint, the group is dropped */
    EMBI_GROUP_FOLD,      /* the items do not overlap (or there is one): alpha multiplies each item's own alpha */
    EMBI_GROUP_TARGET,    /* the items overlap, or one of them cannot take an alpha: an intermediate texture */
};

/* Whether an item kind is drawable (it has bounds and paints pixels). The layer markers are not. */
bool embi_layer_drawable(uint8_t kind);

/* The bounds of item it in dl, in canvas px as x0, y0, x1, y1. With clipped, the clip chain of the item is
 * intersected. Returns false when the item has no area left (empty rect, or clipped away). */
bool embi_layer_item_bounds(const embi_dl *d, const embi_ditem *it, bool clipped, float out[4]);

/* Like embi_layer_item_bounds, with the mapped box of the item grown by pad canvas px on every side before the clip chain
 * is applied (the clips themselves are not grown: nothing paints outside them). */
bool embi_layer_item_bounds_pad(const embi_dl *d, const embi_ditem *it, bool clipped, float pad, float out[4]);

/* The group mode of the items strictly between the LAYER_BEGIN at index begin and the LAYER_END at index end. Nested
 * layers are not foldable (they make it TARGET when they hold drawable items). */
enum embi_group_mode embi_layer_group_mode(const embi_dl *d, uint32_t begin, uint32_t end);

/* Whether a box with style s (and, for a scroll container, the px by which its content overflows its client box in
 * the worst axis, excess) is painted through a cached layer. scroller: the box is a scroll container. */
bool embi_layer_wants_cache(const struct embi_style *s, bool scroller, float excess);

/* A hash of item i of dl with its rect moved by (-ox, -oy) (the origin of the frame it is hashed in). Two items with the
 * same hash paint the same pixels at the same place relative to that origin. */
uint64_t embi_layer_item_hash(const embi_dl *d, uint32_t i, float ox, float oy);
/* A hash of the items in [first, end) of dl that is the same for a layer whose content only moved (the items'
 * rects are taken relative to the smallest drawable origin of the range, so a scroll offset does not change it).
 * Transforms and clip rects are hashed as they are. */
uint64_t embi_layer_content_hash(const embi_dl *d, uint32_t first, uint32_t end);

/* The cached layers of a document: one entry per layer key (the fragment, as an integer), with the texture size in
 * device px and the content hash it was recorded with. Entries not used in a frame are dropped at its end, and
 * the least recently used ones beyond the byte budget too. */
typedef struct embi_lc_ent {
    uint64_t key;
    uint32_t w, h;
    uint64_t hash;
    uint64_t last; /* the frame it was last used in */
    bool used;     /* used in the current frame */
} embi_lc_ent;

typedef struct embi_layer_cache {
    EMBI_VEC(embi_lc_ent) ents;
    uint64_t frame;
    size_t bytes;  /* texture bytes of the entries (w * h * 4 each) */
    size_t budget; /* the byte budget (default 64 MB) */
    uint32_t evicted_last; /* entries evicted by the last end_frame (for the tests and the stats) */
} embi_layer_cache;

void embi_layer_cache_init(embi_layer_cache *c, size_t budget);
void embi_layer_cache_free(embi_layer_cache *c);
/* Starts a frame: the entries become unused until touched. */
void embi_layer_cache_begin(embi_layer_cache *c);
/* Uses the layer key in this frame at size w x h with content hash. Returns true when the layer must be recorded
 * (it is new, its size changed, or its content hash changed) and false when the texture can be composited as it is. */
bool embi_layer_cache_touch(embi_layer_cache *c, uint64_t key, uint32_t w, uint32_t h, uint64_t hash);
/* Ends a frame: drops the entries not used in it and evicts least recently used ones while over budget. */
void embi_layer_cache_end(embi_layer_cache *c);
/* The entry of key, or nullptr. */
const embi_lc_ent *embi_layer_cache_find(const embi_layer_cache *c, uint64_t key);
