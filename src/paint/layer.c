/*
 * Layer decisions and the cached-layer table of W4d. See layer.h. Nothing here
 * touches the GPU: the renderer asks these questions and records what they say.
 */
#include "paint/layer.h"

#include "css/props.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Items of one group that are compared pairwise for overlap; a larger group needs a target without the test. */
#define GROUP_OVERLAP_MAX 256u

bool embi_layer_drawable(uint8_t kind)
{
    switch (kind) {
    case EMBI_DI_FILL:
    case EMBI_DI_BORDER:
    case EMBI_DI_IMAGE:
    case EMBI_DI_GRADIENT:
    case EMBI_DI_TEXT:
    case EMBI_DI_DECORATION:
    case EMBI_DI_TRIANGLE:
    case EMBI_DI_CHECK:
    case EMBI_DI_SHADOW:
    case EMBI_DI_BACKDROP:
    case EMBI_DI_PATH:
        return true;
    default:
        return false;
    }
}

/* The kinds whose alpha is a color (or stop color) alpha, so an opacity folds into them (the fold_alpha of paint.c
 * covers these; a triangle marker is not folded there, so it makes its group a target; a backdrop filter has no
 * color to fold into). */
static bool foldable_kind(uint8_t kind)
{
    return embi_layer_drawable(kind) && kind != EMBI_DI_TRIANGLE && kind != EMBI_DI_CHECK && kind != EMBI_DI_BACKDROP &&
           kind != EMBI_DI_PATH;
}

/* The axis-aligned box of the rect x0, y0, x1, y1 mapped by the transform m (CSS order a b c d e f). */
static void map_bbox(const float m[6], const float r[4], float out[4])
{
    float xs[4], ys[4];
    xs[0] = m[0] * r[0] + m[2] * r[1] + m[4];
    ys[0] = m[1] * r[0] + m[3] * r[1] + m[5];
    xs[1] = m[0] * r[2] + m[2] * r[1] + m[4];
    ys[1] = m[1] * r[2] + m[3] * r[1] + m[5];
    xs[2] = m[0] * r[0] + m[2] * r[3] + m[4];
    ys[2] = m[1] * r[0] + m[3] * r[3] + m[5];
    xs[3] = m[0] * r[2] + m[2] * r[3] + m[4];
    ys[3] = m[1] * r[2] + m[3] * r[3] + m[5];
    out[0] = out[2] = xs[0];
    out[1] = out[3] = ys[0];
    for (int i = 1; i < 4; i++) {
        out[0] = fminf(out[0], xs[i]);
        out[1] = fminf(out[1], ys[i]);
        out[2] = fmaxf(out[2], xs[i]);
        out[3] = fmaxf(out[3], ys[i]);
    }
}

static const float k_identity[6] = {1, 0, 0, 1, 0, 0};

static const float *xform_m(const embi_dl *d, uint32_t xi)
{
    return xi < d->xforms.len ? d->xforms.data[xi].m : k_identity;
}

bool embi_layer_item_bounds_pad(const embi_dl *d, const embi_ditem *it, bool clipped, float pad, float out[4])
{
    if (!embi_layer_drawable(it->kind)) {
        return false;
    }
    float r[4] = {it->rect[0], it->rect[1], it->rect[0] + it->rect[2], it->rect[1] + it->rect[3]};
    if (!(r[2] > r[0]) || !(r[3] > r[1])) {
        return false;
    }
    map_bbox(xform_m(d, it->xform), r, out);
    if (pad > 0.0f) {
        out[0] -= pad;
        out[1] -= pad;
        out[2] += pad;
        out[3] += pad;
    }
    if (!clipped) {
        return true;
    }
    /* Intersect with every clip of the chain, each mapped by its own transform. Index 0 ends the chain. */
    for (uint32_t c = it->clip; c != 0 && c < d->clips.len; c = d->clips.data[c].parent) {
        const embi_dclip *k = &d->clips.data[c];
        float cr[4] = {k->rect[0], k->rect[1], k->rect[0] + k->rect[2], k->rect[1] + k->rect[3]};
        float cb[4];
        map_bbox(xform_m(d, k->xform), cr, cb);
        out[0] = fmaxf(out[0], cb[0]);
        out[1] = fmaxf(out[1], cb[1]);
        out[2] = fminf(out[2], cb[2]);
        out[3] = fminf(out[3], cb[3]);
        if (!(out[2] > out[0]) || !(out[3] > out[1])) {
            return false;
        }
    }
    return out[2] > out[0] && out[3] > out[1];
}

bool embi_layer_item_bounds(const embi_dl *d, const embi_ditem *it, bool clipped, float out[4])
{
    return embi_layer_item_bounds_pad(d, it, clipped, 0.0f, out);
}

/* Strict overlap of two boxes (sharing an edge is not an overlap). */
static bool overlaps(const float a[4], const float b[4])
{
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

enum embi_group_mode embi_layer_group_mode(const embi_dl *d, uint32_t begin, uint32_t end)
{
    if (end > d->items.len || begin >= end) {
        return EMBI_GROUP_EMPTY;
    }
    uint32_t n = 0;
    bool foldable = true;
    if (d->items.data[begin].kind == EMBI_DI_LAYER_BEGIN &&
        (d->items.data[begin].sub == EMBI_LAYER_FILTERED || d->items.data[begin].sub == EMBI_LS_COMPOSITE)) {
        /* A filtered or composite group is rendered into a layer whatever it holds (R3a, R3b). */
        for (uint32_t i = begin + 1; i < end; i++) {
            if (embi_layer_drawable(d->items.data[i].kind)) {
                return EMBI_GROUP_TARGET;
            }
        }
        return EMBI_GROUP_EMPTY;
    }
    /* Leading backdrop-filter items are drawn by the renderer outside the group (with the group's alpha). */
    while (begin + 1 < end && d->items.data[begin + 1].kind == EMBI_DI_BACKDROP) {
        begin++;
    }
    for (uint32_t i = begin + 1; i < end; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN) {
            /* A nested group has its own alpha; the outer one cannot fold into it. */
            foldable = false;
            continue;
        }
        if (!embi_layer_drawable(it->kind)) {
            continue;
        }
        n++;
        if (!foldable_kind(it->kind) || (it->kind == EMBI_DI_TEXT && (it->flags & EMBI_DF_NOFOLD) != 0)) {
            foldable = false; /* (text whose glyphs overlap each other is composited as a group) */
        }
    }
    if (n == 0) {
        return EMBI_GROUP_EMPTY;
    }
    if (!foldable || n > GROUP_OVERLAP_MAX) {
        return EMBI_GROUP_TARGET;
    }
    /* Pairwise overlap of the items that are visible at all. The group is small here (at most GROUP_OVERLAP_MAX). */
    float boxes[GROUP_OVERLAP_MAX][4];
    uint32_t m = 0;
    for (uint32_t i = begin + 1; i < end; i++) {
        const embi_ditem *it = &d->items.data[i];
        float b[4];
        if (!embi_layer_item_bounds(d, it, true, b)) {
            continue;
        }
        memcpy(boxes[m++], b, sizeof b);
    }
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = i + 1; j < m; j++) {
            if (overlaps(boxes[i], boxes[j])) {
                return EMBI_GROUP_TARGET;
            }
        }
    }
    return EMBI_GROUP_FOLD;
}

/* True when the will-change value v (an identifier or a list of them) names transform or opacity. */
static bool will_change_names(const embi_value *v)
{
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_IDENT) {
        embi_str n = embi_atom_str(v->ident);
        return (n.len == 9 && memcmp(n.ptr, "transform", 9) == 0) || (n.len == 7 && memcmp(n.ptr, "opacity", 7) == 0);
    }
    if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            if (will_change_names(&v->list.items[i])) {
                return true;
            }
        }
    }
    return false;
}

bool embi_layer_wants_cache(const embi_style *s, bool scroller, float excess)
{
    if (s == nullptr) {
        return false;
    }
    if (will_change_names(embi_style_value(s, EMBI_PROP_will_change))) {
        return true;
    }
    const embi_value *pos = embi_style_value(s, EMBI_PROP_position);
    if (pos != nullptr && pos->kind == EMBI_V_KEYWORD && pos->kw == EMBI_KW_fixed) {
        return true;
    }
    return scroller && excess > EMBI_LAYER_SCROLL_THRESHOLD;
}

/* ------------------------------------------------------------------ hashing */

static uint64_t mix(uint64_t h, uint64_t v)
{
    return embi_hash64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)));
}

static uint64_t mixf(uint64_t h, float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return mix(h, b);
}

static uint64_t mix_xform(uint64_t h, const float m[6])
{
    for (int i = 0; i < 6; i++) {
        h = mixf(h, m[i]);
    }
    return h;
}

uint64_t embi_layer_item_hash(const embi_dl *d, uint32_t i, float ox, float oy)
{
    const embi_ditem *it = &d->items.data[i];
    uint64_t h = 0x243F6A8885A308D3ull;
    uint64_t head = (uint64_t)it->kind | ((uint64_t)it->sub << 8) | ((uint64_t)it->flags << 16);
    head |= (uint64_t)it->styles[0] << 24 | (uint64_t)it->styles[1] << 32 | (uint64_t)it->styles[2] << 40 |
            (uint64_t)it->styles[3] << 48;
    h = mix(h, head);
    h = mixf(h, it->rect[0] - ox);
    h = mixf(h, it->rect[1] - oy);
    h = mixf(h, it->rect[2]);
    h = mixf(h, it->rect[3]);
    for (int k = 0; k < 4; k++) {
        h = mixf(h, it->radii[k]);
        h = mixf(h, it->widths[k]);
        h = mix(h, it->colors[k]);
    }
    /* f[]: components that are absolute positions (the text baseline origin, gradient geometry, a tiled image's first
     * tile origin) are hashed relative to (ox, oy) in the kind specific part below, so a layer that only moved keeps
     * its hash. The rest of f is hashed as it is. */
    uint32_t skip = 0; /* bit k: f[k] is a position, handled below */
    if (it->kind == EMBI_DI_TEXT) {
        skip = 0x3;
    } else if (it->kind == EMBI_DI_GRADIENT) {
        skip = it->sub == EMBI_GRAD_LINEAR_DL ? 0xF : 0x3;
    } else if (it->kind == EMBI_DI_IMAGE && (it->flags & EMBI_DF_TILE)) {
        skip = 0x3;
    }
    for (int k = 0; k < 6; k++) {
        if (!(skip >> k & 1u)) {
            h = mixf(h, it->f[k]);
        }
    }
    h = mix_xform(h, xform_m(d, it->xform));
    /* The clip chain: rects are absolute (a scroll does not move a clip), each with its transform. */
    for (uint32_t c = it->clip; c != 0 && c < d->clips.len; c = d->clips.data[c].parent) {
        const embi_dclip *k = &d->clips.data[c];
        for (int q = 0; q < 4; q++) {
            h = mixf(h, k->rect[q]);
            h = mixf(h, k->radii[q]);
        }
        h = mix_xform(h, xform_m(d, k->xform));
    }
    /* Kind specific references: a layer end's begin is relative to the item, an image is its URL, a gradient its
     * stops; text refers to a shaped run by index (the run itself is a layout pointer and is not hashed). */
    switch (it->kind) {
    case EMBI_DI_LAYER_END:
        /* The begin of the group, relative to the end (paint.c sets both markers' aux, see finish_layer). */
        h = mix(h, (uint64_t)((int64_t)it->aux - (int64_t)i));
        break;
    case EMBI_DI_LAYER_BEGIN:
    case EMBI_DI_BACKDROP:
        /* The filter steps (R3a). */
        for (uint32_t s = 0; s < it->aux3 && it->aux2 + s < d->fx.len; s++) {
            const embi_dfx *x = &d->fx.data[it->aux2 + s];
            h = mix(h, x->kind);
            h = mixf(h, x->sigma);
            h = mixf(h, x->dx);
            h = mixf(h, x->dy);
            h = mix(h, x->color);
            for (int q = 0; q < 20; q++) {
                h = mixf(h, x->m[q]);
            }
        }
        if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE && it->colors[1] > 0 &&
            it->colors[1] - 1 < d->cpaths.len) {
            /* The clip-path (R3b): its shape is in a table, so it is hashed here. */
            const embi_dcpath *cp = &d->cpaths.data[it->colors[1] - 1];
            h = mix(h, (uint64_t)cp->kind | ((uint64_t)cp->evenodd << 8));
            for (int q = 0; q < 4; q++) {
                h = mixf(h, cp->rect[q]);
                h = mixf(h, cp->radii[q]);
            }
            for (uint32_t q = 0; q < cp->n && cp->first + q < d->curves.len; q++) {
                const embi_dcurve *c = &d->curves.data[cp->first + q];
                h = mixf(h, c->x0);
                h = mixf(h, c->y0);
                h = mixf(h, c->cx);
                h = mixf(h, c->cy);
                h = mixf(h, c->x1);
                h = mixf(h, c->y1);
            }
        }
        break;
    case EMBI_DI_SHADOW:
        if (it->aux < d->shadows.len) {
            const embi_dshadow *sh = &d->shadows.data[it->aux];
            for (int q = 0; q < 4; q++) {
                h = mixf(h, q < 2 ? sh->shape[q] - (q == 0 ? ox : oy) : sh->shape[q]);
                h = mixf(h, sh->shape_radii[q]);
                h = mixf(h, q < 2 ? sh->clip[q] - (q == 0 ? ox : oy) : sh->clip[q]);
                h = mixf(h, sh->clip_radii[q]);
            }
            h = mixf(h, sh->sigma);
        }
        break;
    case EMBI_DI_IMAGE: {
        embi_str u = embi_dl_url(d, it->aux);
        h = mix(h, embi_hash_bytes(u.ptr, u.len, 0x51ull));
        if (u.len > 13 && memcmp(u.ptr, "ember-canvas:", 13) == 0) {
            h = mix(h, it->aux2); /* R4b: the canvas version, so a redraw of the canvas changes the item */
        }
        if (it->flags & EMBI_DF_TILE) {
            h = mixf(h, it->f[0] - ox);
            h = mixf(h, it->f[1] - oy);
        }
        break;
    }
    case EMBI_DI_PATH:
        if (it->aux < d->paths.len) {
            h = mix(h, d->paths.data[it->aux].hash);
        }
        h = mix(h, it->flags);
        h = mix(h, (uint32_t)it->styles[0] | ((uint32_t)it->styles[1] << 8) | ((uint32_t)it->styles[2] << 16));
        for (int k = 0; k < 4; k++) {
            h = mixf(h, it->f[k]);
        }
        for (uint32_t s = 0; s < it->aux3 && it->aux2 + s < d->stops.len; s++) {
            h = mixf(h, d->stops.data[it->aux2 + s].offset);
            h = mix(h, d->stops.data[it->aux2 + s].color);
        }
        break;
    case EMBI_DI_GRADIENT:
        h = mixf(h, it->f[0] - ox);
        h = mixf(h, it->f[1] - oy);
        if (it->sub == EMBI_GRAD_LINEAR_DL) {
            h = mixf(h, it->f[2] - ox);
            h = mixf(h, it->f[3] - oy);
        }
        for (uint32_t s = 0; s < it->aux2 && it->aux + s < d->stops.len; s++) {
            h = mixf(h, d->stops.data[it->aux + s].offset);
            h = mix(h, d->stops.data[it->aux + s].color);
        }
        break;
    case EMBI_DI_TEXT: {
        /* aux is the index of the run in d->runs, a per-list counter that says nothing about what the run draws. The
         * run's content (its text bytes in the item's range, its glyphs, font and size) is what must be compared. */
        h = mix(h, embi_ditem_chash(it)); /* taken at emit: the run itself may be gone (a list is compared after the next layout) */
        h = mixf(h, it->f[0] - ox);
        h = mixf(h, it->f[1] - oy);
        break;
    }
    default:
        break;
    }
    return h;
}

uint64_t embi_layer_content_hash(const embi_dl *d, uint32_t first, uint32_t end)
{
    if (end > d->items.len || first > end) {
        return 0;
    }
    /* The origin: the smallest corner of the drawable items. A scroll moves all of them alike, so it is unchanged. */
    float ox = INFINITY, oy = INFINITY;
    for (uint32_t i = first; i < end; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (embi_layer_drawable(it->kind)) {
            ox = fminf(ox, it->rect[0]);
            oy = fminf(oy, it->rect[1]);
        }
    }
    if (!isfinite(ox)) {
        ox = 0.0f;
        oy = 0.0f;
    }
    uint64_t h = 0xB7E151628AED2A6Bull ^ (end - first);
    for (uint32_t i = first; i < end; i++) {
        h = mix(h, embi_layer_item_hash(d, i, ox, oy));
    }
    return h;
}

/* ------------------------------------------------------------------ cached layers */

void embi_layer_cache_init(embi_layer_cache *c, size_t budget)
{
    memset(c, 0, sizeof *c);
    c->budget = budget;
}

void embi_layer_cache_free(embi_layer_cache *c)
{
    embi_vec_free(&c->ents);
    c->bytes = 0;
    c->frame = 0;
}

void embi_layer_cache_begin(embi_layer_cache *c)
{
    c->frame++;
    c->evicted_last = 0;
    for (uint32_t i = 0; i < c->ents.len; i++) {
        c->ents.data[i].used = false;
    }
}

/* The bytes of a w x h RGBA8 texture. A size whose product does not fit size_t (32-bit targets) saturates at SIZE_MAX / 4,
 * so the byte accounting never wraps. */
static size_t layer_bytes(uint32_t w, uint32_t h)
{
    if (h != 0 && (uint64_t)w > ((uint64_t)SIZE_MAX / 4u) / h) {
        return SIZE_MAX / 4u;
    }
    return (size_t)w * (size_t)h * 4u;
}

const embi_lc_ent *embi_layer_cache_find(const embi_layer_cache *c, uint64_t key)
{
    for (uint32_t i = 0; i < c->ents.len; i++) {
        if (c->ents.data[i].key == key) {
            return &c->ents.data[i];
        }
    }
    return nullptr;
}

bool embi_layer_cache_touch(embi_layer_cache *c, uint64_t key, uint32_t w, uint32_t h, uint64_t hash)
{
    for (uint32_t i = 0; i < c->ents.len; i++) {
        embi_lc_ent *e = &c->ents.data[i];
        if (e->key != key) {
            continue;
        }
        bool again = e->w != w || e->h != h || e->hash != hash;
        if (e->w != w || e->h != h) {
            c->bytes = c->bytes - layer_bytes(e->w, e->h) + layer_bytes(w, h);
        }
        e->w = w;
        e->h = h;
        e->hash = hash;
        e->used = true;
        e->last = c->frame;
        return again;
    }
    embi_lc_ent e;
    memset(&e, 0, sizeof e);
    e.key = key;
    e.w = w;
    e.h = h;
    e.hash = hash;
    e.used = true;
    e.last = c->frame;
    embi_vec_push(&c->ents, e);
    c->bytes += layer_bytes(w, h);
    return true;
}

static void remove_at(embi_layer_cache *c, uint32_t i)
{
    embi_lc_ent *e = &c->ents.data[i];
    c->bytes -= layer_bytes(e->w, e->h);
    c->ents.data[i] = c->ents.data[c->ents.len - 1];
    c->ents.len--;
}

void embi_layer_cache_end(embi_layer_cache *c)
{
    /* Entries that no frame used are dropped: a layer that is not painted needs no texture. */
    for (uint32_t i = 0; i < c->ents.len;) {
        if (!c->ents.data[i].used) {
            remove_at(c, i);
            c->evicted_last++;
        } else {
            i++;
        }
    }
    /* Over the byte budget, the least recently used entries go, first found on ties. */
    while (c->bytes > c->budget && c->ents.len > 0) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < c->ents.len; i++) {
            if (c->ents.data[i].last < c->ents.data[best].last) {
                best = i;
            }
        }
        remove_at(c, best);
        c->evicted_last++;
    }
}
