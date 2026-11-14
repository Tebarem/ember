#pragma once
/*
 * Damage between two display lists (W4d). The new list is compared with the previous
 * one per stacking context. The stacking contexts are aligned first (by the hash of their items, a longest common
 * subsequence), so inserting or removing a context does not shift the pairing of the rest; contexts that differ
 * between two identical ones are paired by position. The items of a context pair are aligned by their hash the same
 * way (layer.h embi_layer_item_hash: kind, rect, transform, clip chain, colors, references), so inserting or removing
 * one item damages only that item. Items without a counterpart (changed, appeared, disappeared) contribute their
 * bounds as device px damage rects; a layer marker among them contributes the bounds of every drawable item between the
 * marker and its partner, since an opacity group paints its content as one. Past a size limit (about one million table
 * cells) the unaligned middle of a very large group is damaged whole instead of aligned.
 *
 * Every item's bounds are grown by 1 device px (the antialiased fringe of a rounded or transformed item), and a text
 * item's by a quarter of its height as well (glyph overhang), before the clip chain is applied.
 *
 * The rects are merged: overlapping ones are joined; when the rects cover more than half the view they are replaced
 * by their bounding box; when the damage then exceeds 60% of the view the frame is a full redraw. A frame with
 * nothing changed has no rects and no full redraw, and records no GPU work.
 *
 * Nothing here touches the GPU. The renderer replays the list clipped to each rect (W4b's partial redraw).
 */
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

/* A device px rectangle, half-open: [x0, x1) x [y0, y1). */
typedef struct embi_drect {
    int32_t x0, y0, x1, y1;
} embi_drect;

typedef struct embi_damage {
    EMBI_VEC(embi_drect) rects; /* the damage, in device px, inside the view; empty for a full redraw */
    bool full;                  /* redraw the whole view */
    bool any;                   /* something changed on screen: full, or rects */
    uint64_t area;              /* the damage area in device px (the view's area for a full redraw) */
    uint32_t changed;           /* items that changed, appeared or disappeared (for the stats) */
} embi_damage;

void embi_damage_init(embi_damage *d);
void embi_damage_free(embi_damage *d);

/* Computes the damage from prev (the list of the previous frame, nullptr when there is none) to cur, for a view of
 * view_w x view_h device px at scale (device px per page px). force_full is set on a resize, a scale change or any
 * other reason to redraw everything. out is reset first. */
void embi_damage_compute(embi_damage *out, const embi_dl *prev, const embi_dl *cur, float scale, uint32_t view_w,
                         uint32_t view_h, bool force_full);

/* Adds to d the bounds of every image item of cur whose URL match accepts (an animated image that changed its frame:
 * the item hash is the same but the pixels are not). d is merged again like a compute's output. Does nothing for a full
 * redraw. */
void embi_damage_add_images(embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w, uint32_t view_h,
                            bool (*match)(void *user, embi_str url), void *user);

/* The damage of a frame that redraws the whole view (a scroll moved all the content). out is reset first. */
void embi_damage_set_full(embi_damage *out, uint32_t view_w, uint32_t view_h, uint32_t changed);

/* The damage of a frame in which the boxes r (x0, y0, x1, y1 in page px) changed: device rects, merged like a compute's. */
void embi_damage_set_page_rects(embi_damage *out, const float (*r)[4], uint32_t n, float scale, uint32_t view_w,
                                uint32_t view_h);
