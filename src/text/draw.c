/*
 * Text drawing (R2c): glyph outlines from the font (cached), filled through the renderer's Slug
 * path (embi_frame_fill_quads). COLRv0/v1 glyphs become layered fills.
 *
 * Glyph cache key (bit 63 clear, the font range of gpu/slug.h): face uid (23 bits), variation
 * instance (24 bits), glyph id (16 bits). The same key always carries the same curves.
 * Bit 62 marks a hinted variant (text/hint.c): its key is a hash of face uid, instance, glyph id and the size (1/64 px per
 * em), so one glyph has a blob per size. The plain keys use the bits 40 to 62 for the uid, which stays below 2^22 so that bit
 * 62 is free.
 */

#include "text/text_int.h"

#include "text/embolden.h"
#include "text/hint.h"

#include "font/font.h"
#include "image/png.h"

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_stdinc.h>

#define EMBI_OUTLINE_BUDGET (8u << 20)

/* The bitmap glyph cache (below) is emptied and freed with the outline cache. */
static void bitmap_clear(embi_text *t);
static void bitmap_free(embi_text *t);
#define EMBI_COLR_MAX_DEPTH 64

typedef struct outline_entry {
    embi_qcurve *curves;
    uint32_t n;
    uint32_t *starts; /* the first curve of each contour (nstarts of them, ascending) */
    uint32_t nstarts;
    bool boxy;        /* every segment is axis-aligned (a glyph made of rectangles, such as Ahem's) */
    bool pixel_known; /* pixel_unit has been decided (draw_glyph, from the face unit) */
    float pixel_unit; /* the pixel grid step of the face when this glyph lies on it, else 0 */
    float ymin, ymax; /* the vertical extent in glyph units */
} outline_entry;

typedef struct outline_state {
    embi_hashmap map;    /* key -> outline_entry* */
    embi_hashmap zones;  /* (uid, inst) -> embi_hint_zones* (nullptr: the face is not hinted) */
    embi_hashmap pixel;  /* (uid, inst) -> face pixel unit (float bits) | 1 << 32 (0 unit: not a pixel face) */
    embi_hashmap hinted; /* hint key -> outline_entry* (n == 0: the glyph is drawn unhinted at this size) */
    embi_hashmap bold;   /* variant key -> outline_entry* (the emboldened curves of a glyph at a strength) */
    size_t bytes;
    size_t hint_bytes;
    size_t budget;
} outline_state;

/* The pseudo glyph id the shaper gives a control character that no face maps (drawn as a box). */
#define EMBI_GID_CONTROL 0xFFFFu

static uint64_t glyph_key(uint32_t uid, uint32_t inst, uint32_t gid)
{
    return ((uint64_t)uid << 40) | ((uint64_t)(inst & 0xFFFFFFu) << 16) | (uint64_t)(gid & 0xFFFFu);
}

static outline_state *outline_of(embi_text *t)
{
    if (t->outline == nullptr) {
        outline_state *o = embi_calloc(1, sizeof *o);
        embi_hashmap_init(&o->map);
        embi_hashmap_init(&o->zones);
        embi_hashmap_init(&o->pixel);
        embi_hashmap_init(&o->hinted);
        embi_hashmap_init(&o->bold);
        o->budget = EMBI_OUTLINE_BUDGET;
        t->outline = o;
    }
    return t->outline;
}

static void outline_entry_free(outline_state *o, outline_entry *e)
{
    embi_free(e->curves, sizeof(embi_qcurve) * e->n);
    if (e->starts != nullptr) {
        embi_free(e->starts, sizeof(uint32_t) * e->nstarts);
    }
    embi_free(e, sizeof *e);
    (void)o;
}

/* Frees the hinted curves and the zones. */
static void hint_clear(outline_state *o)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&o->hinted);
    uint64_t k, v;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        outline_entry_free(o, (outline_entry *)(uintptr_t)v);
    }
    embi_hashmap_clear(&o->hinted);
    o->hint_bytes = 0;
    it = embi_hashmap_iter_begin(&o->zones);
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        if (v != 0) {
            embi_free((embi_hint_zones *)(uintptr_t)v, sizeof(embi_hint_zones));
        }
    }
    embi_hashmap_clear(&o->zones);
    embi_hashmap_clear(&o->pixel);
}

void embi_outline_clear(embi_text *t)
{
    bitmap_clear(t);
    outline_state *o = t->outline;
    if (o == nullptr) {
        return;
    }
    hint_clear(o);
    embi_hashmap_iter it = embi_hashmap_iter_begin(&o->map);
    uint64_t k, v;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        outline_entry_free(o, (outline_entry *)(uintptr_t)v);
    }
    embi_hashmap_clear(&o->map);
    it = embi_hashmap_iter_begin(&o->bold);
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        outline_entry_free(o, (outline_entry *)(uintptr_t)v);
    }
    embi_hashmap_clear(&o->bold);
    o->bytes = 0;
}

void embi_outline_fini(embi_text *t)
{
    bitmap_free(t);
    outline_state *o = t->outline;
    if (o == nullptr) {
        return;
    }
    embi_outline_clear(t);
    embi_hashmap_free(&o->map);
    embi_hashmap_free(&o->zones);
    embi_hashmap_free(&o->pixel);
    embi_hashmap_free(&o->hinted);
    embi_hashmap_free(&o->bold);
    embi_free(o, sizeof *o);
    t->outline = nullptr;
}

/* The curves of glyph gid of face in the instance. Built on a miss. */
static outline_entry *outline_get(embi_text *t, embi_face *face, uint32_t inst, uint32_t gid)
{
    outline_state *o = outline_of(t);
    uint64_t key = glyph_key(face->uid, inst, gid);
    uint64_t v;
    if (embi_hashmap_get(&o->map, key, &v)) {
        return (outline_entry *)(uintptr_t)v;
    }
    embi_outline ol;
    SDL_memset(&ol, 0, sizeof ol);
    outline_entry *e = embi_calloc(1, sizeof *e);
    if (embi_font_glyph_outline(&face->font, gid, embi_face_inst_coords(face, inst), &ol) &&
        ol.quads.len > 0) {
        e->n = ol.quads.len;
        e->curves = embi_alloc(sizeof(embi_qcurve) * e->n);
        for (uint32_t i = 0; i < e->n; i++) {
            const embi_quad *q = &ol.quads.data[i];
            e->curves[i] = (embi_qcurve){q->p0.x, q->p0.y, q->p1.x, q->p1.y, q->p2.x, q->p2.y};
        }
        /* The contour starts as the font gives them: a contour may touch its own start point, so they are not found by position. */
        e->nstarts = ol.contours.len > 0 ? ol.contours.len : 1u;
        e->starts = embi_alloc(sizeof(uint32_t) * e->nstarts);
        for (uint32_t c = 0; c < e->nstarts; c++) {
            e->starts[c] = ol.contours.len > 0 ? ol.contours.data[c] : 0u;
        }
        e->boxy = true;
        e->ymin = e->ymax = e->curves[0].y0;
        for (uint32_t i = 0; i < e->n; i++) {
            const embi_qcurve *c = &e->curves[i];
            bool line = SDL_fabsf(c->cx - 0.5f * (c->x0 + c->x1)) < 0.01f &&
                        SDL_fabsf(c->cy - 0.5f * (c->y0 + c->y1)) < 0.01f;
            if (!line || (c->x0 != c->x1 && c->y0 != c->y1)) {
                e->boxy = false;
            }
            float ys[2] = {c->y0, c->y1};
            for (int k = 0; k < 2; k++) {
                e->ymin = ys[k] < e->ymin ? ys[k] : e->ymin;
                e->ymax = ys[k] > e->ymax ? ys[k] : e->ymax;
            }
        }
        /* Only a glyph as wide as the em (Ahem's squares) keeps its edges on whole px by that rule. A narrow glyph
         * of rectangles in a text face (l, I, H) is grid fitted like the rest (W4h). */
        float xmin = e->curves[0].x0, xmax = xmin;
        for (uint32_t i = 0; i < e->n; i++) {
            xmin = e->curves[i].x0 < xmin ? e->curves[i].x0 : xmin;
            xmax = e->curves[i].x0 > xmax ? e->curves[i].x0 : xmax;
        }
        if (xmax - xmin < 0.9f * (float)face->font.metrics.units_per_em) {
            e->boxy = false;
        }
    }
    embi_outline_free(&ol);
    if (gid == EMBI_GID_CONTROL && e->n == 0 && face->font.metrics.units_per_em != 0) {
        /* A control character has no glyph but must be visible (CSS Text 3 section 4.1): a hollow box 0.1 .. 0.5 em wide and 0 .. 0.7 em tall. */
        float u = (float)face->font.metrics.units_per_em;
        float x0 = 0.1f * u, x1 = 0.5f * u, y0 = 0.0f, y1 = 0.7f * u, w = 0.05f * u;
        float ring[2][4] = {{x0, y0, x1, y1}, {x0 + w, y0 + w, x1 - w, y1 - w}};
        e->n = 8;
        e->curves = embi_alloc(sizeof(embi_qcurve) * e->n);
        e->nstarts = 2;
        e->starts = embi_alloc(sizeof(uint32_t) * 2u);
        e->starts[0] = 0;
        e->starts[1] = 4;
        for (uint32_t r = 0; r < 2; r++) {
            const float *b = ring[r];
            /* The outer ring runs counter-clockwise, the inner one clockwise: nonzero winding leaves a frame. */
            float px[5] = {b[0], b[2], b[2], b[0], b[0]}, py[5] = {b[1], b[1], b[3], b[3], b[1]};
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t a = r == 0 ? k : 4 - k, c = r == 0 ? k + 1 : 3 - k;
                e->curves[r * 4 + k] = (embi_qcurve){px[a], py[a], 0.5f * (px[a] + px[c]), 0.5f * (py[a] + py[c]), px[c], py[c]};
            }
        }
    }
    o->bytes += sizeof(embi_qcurve) * e->n + sizeof(uint32_t) * e->nstarts + sizeof *e;
    embi_hashmap_put(&o->map, key, (uint64_t)(uintptr_t)e);
    return e;
}

/* ------------------------------------------------------------------ hinted outlines */

#define EMBI_HINT_BUDGET (4u << 20)

static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* The key of a synthetic bold variant of a glyph (strength64: the emboldening in 1/64 font units). Bit 61 is set, bits 62 and 63
 * clear. Uids are small sequential counters, so the uid << 40 of glyph_key cannot reach bit 61 in practice. */
static uint64_t variant_key(uint32_t uid, uint32_t inst, uint32_t gid, uint32_t strength64)
{
    uint64_t h = mix64(uid);
    h = mix64(h ^ inst);
    h = mix64(h ^ gid);
    h = mix64(h ^ (0x8000000000000000ull | strength64));
    return (1ull << 61) | (h & ((1ull << 61) - 1u));
}

/* The blob and cache key of the hinted variant of a glyph at a size (ppem64: px per em in 1/64). Bit 62 is set, bit 63 clear.
 * variant is the synthetic bold key of the outline (0 for the regular one). */
static uint64_t hint_key(uint32_t uid, uint32_t inst, uint32_t gid, uint32_t ppem64, uint64_t variant)
{
    SDL_assert(uid < (1u << 22));
    uint64_t h = mix64(uid);
    h = mix64(h ^ inst);
    h = mix64(h ^ gid);
    h = mix64(h ^ ppem64);
    if (variant != 0) {
        h = mix64(h ^ variant);
    }
    return (1ull << 62) | (h & ((1ull << 62) - 1u));
}

/* The emboldened curves of glyph gid at a strength (strength64 / 64 font units), centred, for the unhinted drawing. Built on a
 * miss; the flags and the extent come from the regular outline and the new curves. */
static outline_entry *bold_get(embi_text *t, embi_face *face, uint32_t inst, uint32_t gid, uint32_t strength64)
{
    outline_state *o = outline_of(t);
    const uint64_t key = variant_key(face->uid, inst, gid, strength64);
    uint64_t v;
    if (embi_hashmap_get(&o->bold, key, &v)) {
        return (outline_entry *)(uintptr_t)v;
    }
    const outline_entry *base = outline_get(t, face, inst, gid);
    outline_entry *e = embi_calloc(1, sizeof *e);
    if (base != nullptr && base->n > 0) {
        const float str = (float)strength64 / 64.0f;
        e->n = base->n;
        e->curves = embi_alloc(sizeof(embi_qcurve) * e->n);
        embi_embolden(base->curves, base->n, base->starts, base->nstarts, str, str, false, e->curves);
        e->boxy = base->boxy;
        e->ymin = e->ymax = e->curves[0].y0;
        for (uint32_t i = 0; i < e->n; i++) {
            const float ys[2] = {e->curves[i].y0, e->curves[i].y1};
            for (int k = 0; k < 2; k++) {
                e->ymin = ys[k] < e->ymin ? ys[k] : e->ymin;
                e->ymax = ys[k] > e->ymax ? ys[k] : e->ymax;
            }
        }
    }
    o->bytes += sizeof(embi_qcurve) * e->n + sizeof *e;
    embi_hashmap_put(&o->bold, key, (uint64_t)(uintptr_t)e);
    return e;
}

typedef struct extent_ctx {
    embi_text *t;
    embi_face *face;
    uint32_t inst;
} extent_ctx;

static bool extent_cb(void *user, uint32_t cp, float *ymin, float *ymax)
{
    extent_ctx *c = user;
    uint32_t gid = embi_font_glyph_index(&c->face->font, cp);
    if (gid == 0) {
        return false;
    }
    outline_entry *oe = outline_get(c->t, c->face, c->inst, gid);
    if (oe == nullptr || oe->n == 0) {
        return false;
    }
    embi_hint_extent(oe->curves, oe->n, ymin, ymax);
    return true;
}

/* The alignment zones of the face instance, or nullptr when it is not to be hinted. */
static const embi_hint_zones *hint_zones(embi_text *t, outline_state *o, embi_face *face, uint32_t inst)
{
    const uint64_t key = ((uint64_t)face->uid << 24) | (uint64_t)(inst & 0xFFFFFFu);
    uint64_t v;
    if (embi_hashmap_get(&o->zones, key, &v)) {
        return (const embi_hint_zones *)(uintptr_t)v;
    }
    embi_hint_zones *z = nullptr;
    const float upem = (float)face->font.metrics.units_per_em;
    extent_ctx c = {t, face, inst};
    float lo = 0.0f, hi = 0.0f;
    /* Only a face with Latin letters to measure is hinted: the zones of a script without them (Arabic, Devanagari) are not
     * the x-height and cap height, and the OS/2 x-height alone would pull its features onto arbitrary rows. */
    const bool latin = extent_cb(&c, 'x', &lo, &hi) || extent_cb(&c, 'o', &lo, &hi) || extent_cb(&c, 'H', &lo, &hi) ||
                       extent_cb(&c, 'O', &lo, &hi);
    if (upem > 0.0f && latin) {
        z = embi_alloc(sizeof *z);
        embi_hint_zones_measure(z, upem, (float)face->font.metrics.x_height, extent_cb, &c);
        if (!embi_hint_zones_usable(z)) {
            embi_free(z, sizeof *z);
            z = nullptr;
        }
    }
    embi_hashmap_put(&o->zones, key, (uint64_t)(uintptr_t)z);
    return z;
}

/* The pixel grid step of the face instance in font units, or 0 when it is not a pixel-art face: decided once from sample glyphs
 * (embi_hint_pixel_face). */
static float face_pixel_unit(embi_text *t, outline_state *o, embi_face *face, uint32_t inst)
{
    const uint64_t key = ((uint64_t)face->uid << 24) | (uint64_t)(inst & 0xFFFFFFu);
    uint64_t v;
    if (embi_hashmap_get(&o->pixel, key, &v)) {
        uint32_t bits = (uint32_t)v;
        float u;
        SDL_memcpy(&u, &bits, sizeof u);
        return u;
    }
    static const uint32_t samples[] = {'H', 'o', 'a', 'e', 'x', '0', 'g'};
    const embi_qcurve *g[sizeof samples / sizeof samples[0]];
    uint32_t cnt[sizeof samples / sizeof samples[0]];
    uint32_t ng = 0;
    for (uint32_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        const uint32_t gid = embi_font_glyph_index(&face->font, samples[i]);
        const outline_entry *oe = gid != 0 ? outline_get(t, face, inst, gid) : nullptr;
        if (oe != nullptr && oe->n > 0) {
            g[ng] = oe->curves;
            cnt[ng++] = oe->n;
        }
    }
    float unit = 0.0f;
    if (!embi_hint_pixel_face(g, cnt, ng, (float)face->font.metrics.units_per_em, &unit)) {
        unit = 0.0f;
    }
    uint32_t bits;
    SDL_memcpy(&bits, &unit, sizeof bits);
    embi_hashmap_put(&o->pixel, key, (uint64_t)bits | (1ull << 32));
    return unit;
}

/* A smaller x-height than this (px) is not hinted: the rows are too few to fit. */
#define EMBI_HINT_MIN_XHEIGHT_PX 3.0f

/* The scheme of the hinted synthetic bold: the key of the bold variant is mixed with this, so it never meets the centred one. */
#define EMBI_BOLD_HINT_SCHEME 0x9E3779B97F4A7C15ull

#define EMBI_SHARP_HINT_SCHEME 0xC2B2AE3D27D4EB4Full
#define EMBI_PIXEL_HINT_SCHEME 0x165667B19E3779F9ull

/* The cache key of the hinted outline of glyph gid at ppem64 (px per em in 1/64) with the synthetic bold strength64 (0: regular)
 * and the text hinting mode: SHARP curves (x fitted) never meet the NATURAL ones. */
static uint64_t hint_key_for(embi_face *face, uint32_t inst, uint32_t gid, uint32_t ppem64, uint32_t strength64,
                             embi_text_hinting mode)
{
    uint64_t variant = strength64 != 0 ? (variant_key(face->uid, inst, gid, strength64) ^ EMBI_BOLD_HINT_SCHEME) : 0;
    if (mode == EMBI_TEXT_HINTING_SHARP) {
        variant ^= EMBI_SHARP_HINT_SCHEME;
    }
    return hint_key(face->uid, inst, gid, ppem64, variant);
}

/* The hinted curves of glyph gid at s device px per font unit (vertically), or nullptr to draw it unhinted. oe is the regular
 * outline. A bold request (strength64 != 0) hints the regular outline and then emboldens the hinted curves: the stems grow to the
 * right, the baseline and the left edges stay, and the top grows by the strength rounded to whole device px (FreeType's
 * EmboldenXY with the bottom-left anchor), so the top edges stay on their rows. The result is cached by size, strength and mode;
 * the key (*key_out) names the curves. A glyph without edges is remembered as unhinted. */
static const outline_entry *hint_get(embi_text *t, embi_face *face, uint32_t inst, uint32_t gid, const outline_entry *oe,
                                     float s, uint32_t strength64, embi_text_hinting mode, uint64_t *key_out)
{
    outline_state *o = outline_of(t);
    const float upem = (float)face->font.metrics.units_per_em;
    const uint32_t ppem64 = (uint32_t)SDL_roundf(s * upem * 64.0f);
    /* The curves are a function of the key alone: hinted at the size the key names, not at s, whose float sum may land on
     * either side of a rounding (the same key must always carry the same curves, gpu/slug.h). The drawing still uses the
     * true scale, which differs by at most 1/128 px per em. */
    if (ppem64 == 0 || !(upem > 0.0f)) {
        *key_out = 0;
        return nullptr;
    }
    s = (float)ppem64 / (64.0f * upem);
    const uint64_t key = hint_key_for(face, inst, gid, ppem64, strength64, mode);
    *key_out = key;
    uint64_t v;
    if (embi_hashmap_get(&o->hinted, key, &v)) {
        const outline_entry *e = (const outline_entry *)(uintptr_t)v;
        return e->n > 0 ? e : nullptr;
    }
    outline_entry *e = embi_calloc(1, sizeof *e);
    if (strength64 == 0) {
        const embi_hint_zones *z = hint_zones(t, o, face, inst);
        /* The y fit needs usable zones and a tall enough x-height; SHARP also fits the vertical stems in x, at any size. */
        bool fit_y = false;
        if (z != nullptr) {
            const embi_hint_zone *xz = (z->n_valid_mask & (1u << EMBI_HINT_XHEIGHT)) != 0 ? &z->z[EMBI_HINT_XHEIGHT]
                                                                                           : &z->z[EMBI_HINT_CAP];
            fit_y = xz->flat * s >= EMBI_HINT_MIN_XHEIGHT_PX;
        }
        const bool fit_x = mode == EMBI_TEXT_HINTING_SHARP;
        if (fit_y || fit_x) {
            {
                /* x fitting cuts the curves a second time: room for 4 * n. */
                embi_qcurve *buf = embi_alloc(sizeof(embi_qcurve) * 4u * oe->n);
                uint32_t *map = embi_alloc(sizeof(uint32_t) * (oe->n + 1u));
                /* The single place where the mode reaches the hinter (hint_key_for keeps the SHARP curves apart). */
                uint32_t m = embi_hint_glyph_fit(fit_y ? z : nullptr, upem, s, oe->curves, oe->n, buf, map, fit_x);
                if (m > 0) {
                    /* The buffer holds 4 * n curves; keep the exact size so that it frees with the entry. */
                    embi_qcurve *fit = embi_alloc(sizeof(embi_qcurve) * m);
                    SDL_memcpy(fit, buf, sizeof(embi_qcurve) * m);
                    e->curves = fit;
                    e->n = m;
                    e->ymin = oe->ymin;
                    e->ymax = oe->ymax;
                    /* A contour starts where its regular start went. */
                    e->nstarts = oe->nstarts;
                    e->starts = embi_alloc(sizeof(uint32_t) * e->nstarts);
                    for (uint32_t c = 0; c < e->nstarts; c++) {
                        e->starts[c] = map[oe->starts[c]];
                    }
                }
                embi_free(buf, sizeof(embi_qcurve) * 4u * oe->n);
                embi_free(map, sizeof(uint32_t) * (oe->n + 1u));
            }
        }
    } else {
        uint64_t rkey = 0;
        const outline_entry *r = hint_get(t, face, inst, gid, oe, s, 0, mode, &rkey);
        if (r != nullptr && r->n > 0) {
            const float str = (float)strength64 / 64.0f;
            /* The strength in whole device px, as a function of the key (strength64 and ppem64). */
            const float ystr = SDL_roundf(str * s) / s;
            /* SHARP: the stems grow by whole columns too (at least one), so they stay on them. */
            float xstr = str;
            if (mode == EMBI_TEXT_HINTING_SHARP) {
                const float cols = SDL_roundf(str * s);
                xstr = (cols < 1.0f ? 1.0f : cols) / s;
            }
            e->n = r->n;
            e->curves = embi_alloc(sizeof(embi_qcurve) * e->n);
            embi_embolden(r->curves, r->n, r->starts, r->nstarts, xstr, ystr, true, e->curves);
            e->nstarts = r->nstarts;
            e->starts = embi_alloc(sizeof(uint32_t) * e->nstarts);
            SDL_memcpy(e->starts, r->starts, sizeof(uint32_t) * e->nstarts);
            e->ymin = e->ymax = e->curves[0].y0;
            for (uint32_t i = 0; i < e->n; i++) {
                const float ys[2] = {e->curves[i].y0, e->curves[i].y1};
                for (int k = 0; k < 2; k++) {
                    e->ymin = ys[k] < e->ymin ? ys[k] : e->ymin;
                    e->ymax = ys[k] > e->ymax ? ys[k] : e->ymax;
                }
            }
        }
    }
    o->hint_bytes += sizeof *e + sizeof(embi_qcurve) * e->n + sizeof(uint32_t) * e->nstarts;
    embi_hashmap_put(&o->hinted, key, (uint64_t)(uintptr_t)e);
    return e->n > 0 ? e : nullptr;
}

/* The curves of a pixel-art glyph with every edge on a whole device px at kpx px per grid step (the unit of the glyph), cached by
 * glyph and kpx alone. The key (*key_out) names them. */
static const outline_entry *pixel_get(embi_text *t, embi_face *face, uint32_t inst, uint32_t gid, const outline_entry *oe,
                                      uint32_t kpx, uint64_t *key_out)
{
    outline_state *o = outline_of(t);
    const uint64_t key = hint_key(face->uid, inst, gid, kpx, EMBI_PIXEL_HINT_SCHEME);
    *key_out = key;
    uint64_t v;
    if (embi_hashmap_get(&o->hinted, key, &v)) {
        return (const outline_entry *)(uintptr_t)v;
    }
    outline_entry *e = embi_calloc(1, sizeof *e);
    e->n = oe->n;
    e->curves = embi_alloc(sizeof(embi_qcurve) * e->n);
    embi_hint_pixel_snap(oe->curves, oe->n, (float)kpx / oe->pixel_unit, e->curves);
    e->nstarts = oe->nstarts;
    e->starts = embi_alloc(sizeof(uint32_t) * e->nstarts);
    SDL_memcpy(e->starts, oe->starts, sizeof(uint32_t) * e->nstarts);
    e->ymin = e->ymax = e->curves[0].y0;
    for (uint32_t i = 0; i < e->n; i++) {
        const float ys[2] = {e->curves[i].y0, e->curves[i].y1};
        for (int k = 0; k < 2; k++) {
            e->ymin = ys[k] < e->ymin ? ys[k] : e->ymin;
            e->ymax = ys[k] > e->ymax ? ys[k] : e->ymax;
        }
    }
    o->hint_bytes += sizeof *e + sizeof(embi_qcurve) * e->n + sizeof(uint32_t) * e->nstarts;
    embi_hashmap_put(&o->hinted, key, (uint64_t)(uintptr_t)e);
    return e;
}

/* ------------------------------------------------------------------ affine helpers */

/* out = a o b: applying out is applying b, then a. CSS order (a, b, c, d, e, f). */
static void mat_mul(const float a[6], const float b[6], float out[6])
{
    float r[6];
    r[0] = a[0] * b[0] + a[2] * b[1];
    r[1] = a[1] * b[0] + a[3] * b[1];
    r[2] = a[0] * b[2] + a[2] * b[3];
    r[3] = a[1] * b[2] + a[3] * b[3];
    r[4] = a[0] * b[4] + a[2] * b[5] + a[4];
    r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    for (int i = 0; i < 6; i++) {
        out[i] = r[i];
    }
}

static void mat_point(const float m[6], float x, float y, float *ox, float *oy)
{
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

static float mat_scale(const float m[6])
{
    float det = m[0] * m[3] - m[1] * m[2];
    return det < 0.0f ? -det : det;
}

static uint32_t rgba_pack(const float c[4], float alpha_scale)
{
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i];
        if (i == 3) {
            v *= alpha_scale;
        }
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        out |= ((uint32_t)(v * 255.0f + 0.5f)) << (8 * i);
    }
    return out;
}

/* ------------------------------------------------------------------ COLR */

typedef struct colr_ctx {
    embi_frame *f;
    embi_text *t;
    embi_face *face;
    uint32_t inst;
    float G[6];           /* glyph units -> local px */
    float total[6];       /* COLR transform stack, applied to paint geometry */
    float stack[EMBI_COLR_MAX_DEPTH][6];
    uint32_t depth;
    uint32_t clip_gid[EMBI_COLR_MAX_DEPTH];
    uint32_t clip_depth;
    bool clip_active;
    uint32_t text_rgba;   /* for foreground colours */
    uint32_t xform, clip;
    uint32_t emitted;
    bool overflow;
} colr_ctx;

/* Clockwise angle (radians, from straight up, y down: the conic convention of gpu/render.h) of
 * the local direction of a font-space direction at deg (counter-clockwise from +x, y up), mapped
 * through M. */
static float colr_local_angle(const float M[6], float deg)
{
    float r = deg * 0.017453292519943295f;
    float dx = SDL_cosf(r), dy = SDL_sinf(r);
    float vx = M[0] * dx + M[2] * dy;
    float vy = M[1] * dx + M[3] * dy;
    return SDL_atan2f(vx, -vy);
}

static void colr_fill_clip(colr_ctx *c, const embi_colr_paint *p)
{
    uint32_t gid = c->clip_gid[c->clip_depth - 1];
    outline_entry *oe = outline_get(c->t, c->face, c->inst, gid);
    if (oe == nullptr || oe->n == 0) {
        return;
    }
    float M[6];
    mat_mul(c->G, c->total, M); /* glyph units through the COLR stack, then to local px */

    embi_paint paint = embi_paint_solid(0);
    /* Stops are shared by the gradient kinds; the renderer copies them. */
    embi_stop stops[64];
    embi_stop conic[64]; /* sweep stops, as fractions of a turn */
    uint32_t ns = p->num_stops < 64 ? p->num_stops : 64;
    for (uint32_t i = 0; i < ns; i++) {
        const embi_colr_stop *cs = &p->stops[i];
        stops[i].offset = cs->offset;
        if (cs->foreground) {
            uint32_t a = (c->text_rgba >> 24) & 0xFF;
            float alpha = cs->rgba[3];
            uint32_t col = (c->text_rgba & 0x00FFFFFFu) |
                           ((uint32_t)(alpha * (float)a + 0.5f) << 24);
            stops[i].color = col;
        } else {
            stops[i].color = rgba_pack(cs->rgba, 1.0f);
        }
    }

    if (p->kind == EMBI_PAINT_SOLID) {
        paint.kind = EMBI_PRIM_FILL;
        if (p->foreground) {
            paint.color = c->text_rgba;
            uint32_t a = (c->text_rgba >> 24) & 0xFF;
            paint.color = (c->text_rgba & 0x00FFFFFFu) |
                          ((uint32_t)(p->rgba[3] * (float)a + 0.5f) << 24);
        } else {
            paint.color = rgba_pack(p->rgba, 1.0f);
        }
    } else if (p->kind == EMBI_PAINT_LINEAR && ns > 0) {
        /* Paint geometry is in glyph units at this op: M maps it to local px. COLR's third point
         * p2 rotates the colour line: the line p0 to p1 is projected onto the perpendicular of
         * p0 to p2, so the gradient runs across p0p2 (p1 is kept when p2 is on p0). */
        float vx = p->p[2] - p->p[0], vy = p->p[3] - p->p[1];
        float ux = -(p->p[5] - p->p[1]), uy = p->p[4] - p->p[0];
        float uu = ux * ux + uy * uy;
        float q1x = p->p[2], q1y = p->p[3];
        if (uu > 0.0f) {
            float t = (vx * ux + vy * uy) / uu;
            q1x = p->p[0] + t * ux;
            q1y = p->p[1] + t * uy;
        }
        float x0, y0, x1, y1;
        mat_point(M, p->p[0], p->p[1], &x0, &y0);
        mat_point(M, q1x, q1y, &x1, &y1);
        paint.kind = EMBI_PRIM_LINEAR;
        paint.g[0] = x0;
        paint.g[1] = y0;
        paint.g[2] = x1;
        paint.g[3] = y1;
        paint.stops = stops;
        paint.n_stops = ns;
        paint.repeat = p->extend != 0;
    } else if (p->kind == EMBI_PAINT_RADIAL && ns > 0) {
        /* Approximation until R3: only the end circle (x1, y1, r1) is used, as a radial with
         * equal radii. The COLR focal circle (x0, y0, r0) is ignored. */
        float cx, cy;
        mat_point(M, p->p[3], p->p[4], &cx, &cy);
        float r = p->p[5] * SDL_sqrtf(mat_scale(M));
        paint.kind = EMBI_PRIM_RADIAL;
        paint.g[0] = cx;
        paint.g[1] = cy;
        paint.g[2] = r;
        paint.g[3] = r;
        paint.stops = stops;
        paint.n_stops = ns;
        paint.repeat = p->extend != 0;
    } else if (p->kind == EMBI_PAINT_SWEEP && ns > 0 && p->p[3] > p->p[2]) {
        /* COLR sweep: the colour line runs from start to end angle (degrees, counter-clockwise
         * from +x in font space, y up). The renderer's conic sweeps clockwise from straight up
         * in local px, so the angles are converted through M, and the stops are re-ordered and
         * rescaled to fractions of a turn. Known gap: outside [start, end] the colours are the
         * end colours (COLR pads the start colour before start), since a conic has no pad. */
        float cx, cy;
        mat_point(M, p->p[0], p->p[1], &cx, &cy);
        float a0 = p->p[2], a1 = p->p[3];
        float span = a1 - a0;
        float d = colr_local_angle(M, a0 + 0.01f) - colr_local_angle(M, a0);
        while (d > 3.14159265f) {
            d -= 6.28318531f;
        }
        while (d <= -3.14159265f) {
            d += 6.28318531f;
        }
        bool fwd = d > 0.0f; /* the local angle grows with the COLR angle */
        float from = fwd ? colr_local_angle(M, a0) : colr_local_angle(M, a1);
        float frac = span / 360.0f;
        if (frac > 1.0f) {
            frac = 1.0f;
        }
        for (uint32_t i = 0; i < ns; i++) {
            uint32_t k = fwd ? i : ns - 1 - i;
            float o = p->stops[k].offset;
            o = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
            conic[i].offset = fwd ? o * frac : (1.0f - o) * frac;
            conic[i].color = stops[k].color;
        }
        paint.kind = EMBI_PRIM_CONIC;
        paint.g[0] = cx;
        paint.g[1] = cy;
        paint.g[2] = from;
        paint.stops = conic;
        paint.n_stops = ns;
        paint.repeat = p->extend != 0;
    } else {
        /* A degenerate sweep (end not after start) or a paint without stops: the last stop. */
        paint.kind = EMBI_PRIM_FILL;
        paint.color = ns > 0 ? stops[ns - 1].color : 0;
    }
    embi_frame_fill_quads(c->f, glyph_key(c->face->uid, c->inst, gid), oe->curves, oe->n, M,
                          &paint, EMBI_FILL_NONZERO, c->xform, c->clip);
    c->emitted++;
}

static bool colr_visit(void *ctx, const embi_colr_op *op)
{
    colr_ctx *c = ctx;
    switch (op->kind) {
    case EMBI_COLR_TRANSFORM_PUSH:
        if (c->depth >= EMBI_COLR_MAX_DEPTH) {
            c->overflow = true;
            return false;
        }
        SDL_memcpy(c->stack[c->depth], c->total, sizeof c->total);
        c->depth++;
        mat_mul(c->total, op->xf, c->total);
        return true;
    case EMBI_COLR_TRANSFORM_POP:
        if (c->depth > 0) {
            c->depth--;
            SDL_memcpy(c->total, c->stack[c->depth], sizeof c->total);
        }
        return true;
    case EMBI_COLR_CLIP_PUSH:
        if (c->clip_depth >= EMBI_COLR_MAX_DEPTH) {
            c->overflow = true;
            return false;
        }
        c->clip_gid[c->clip_depth++] = op->gid;
        c->clip_active = true;
        return true;
    case EMBI_COLR_CLIP_POP:
        if (c->clip_depth > 0) {
            c->clip_depth--;
        }
        c->clip_active = c->clip_depth > 0;
        return true;
    case EMBI_COLR_GROUP_PUSH:
    case EMBI_COLR_GROUP_POP:
        /* Composite modes are recorded but drawn as src-over until R3. */
        return true;
    case EMBI_COLR_PAINT:
        if (c->clip_active) {
            colr_fill_clip(c, &op->paint);
        }
        return true;
    }
    return true;
}

/* ------------------------------------------------------------------ crisp text (W4h) */

/* Text above this many device px per em is drawn unfitted, and so is text with no darkening. */
#define EMBI_FIT_MAX_PX 36.0f
#define EMBI_FIT_MIN_XHEIGHT_PX 3.0f
#define EMBI_LCD_MAX_PX 40.0f /* larger text gains nothing from LCD and costs 15x the coverage work: gray */
#define EMBI_DARKEN_SMALL_PX 0.40f /* total stem darkening at 13 px and below: the worst subpixel phase of a 1.17 px stem */
#define EMBI_DARKEN_MAX_PX 0.25f   /* ... 0.25 px from 16 px up, fading to none at the next size ... */
#define EMBI_DARKEN_SMALL_UPTO_PX 13.0f
#define EMBI_DARKEN_FULL_PX 16.0f
#define EMBI_DARKEN_NONE_PX 26.0f /* ... fading to none at this one */

/* How a run is drawn on its transform: the vertical fit scale k (1 = unfitted), the device y map
 * (m3, m5) used to put baselines on pixel rows, the stem darkening in device px, and whether LCD
 * antialiasing is wanted. on == false draws through the plain path (colour glyphs, rotation). */
typedef struct text_fit {
    bool on;
    float k;
    float m3, m5;
    float darken;
    bool lcd;
    bool hint;  /* glyphs are hinted (text/hint.c) instead of scaled by k */
    embi_text_hinting mode; /* the view's text hinting; NONE takes no fit, snap, hint or boxy rounding */
    float m0, m4;           /* the device x map of the target (x * m0 + m4) */
    bool iso;               /* m0 == m3: x and y fitting, pen rounding and pixel snapping assume it */
    float pixel_unit;       /* the pixel grid step of the face (font units), 0 when it is not pixel art */
} text_fit;

/* The top of the outline of glyph gid (font units), or 0 when it has none. */
static float glyph_top(embi_text *t, embi_face *face, uint32_t inst, uint32_t cp)
{
    uint32_t gid = embi_font_glyph_index(&face->font, cp);
    if (gid == 0) {
        return 0.0f;
    }
    outline_entry *oe = outline_get(t, face, inst, gid);
    if (oe == nullptr || oe->n == 0) {
        return 0.0f;
    }
    return oe->ymax;
}

/* The vertical scale that puts the x-height of the face at size_px (device px per em) on a whole
 * pixel row, light autohint style. The change is limited so that the cap height moves by at most
 * half a pixel, which keeps the ink within a pixel of the unfitted outline (the baseline moves by
 * up to half a pixel as well). 1 when the face has no usable x-height or the size is out of range. */
static float fit_scale(embi_text *t, embi_face *face, uint32_t inst, float size_px)
{
    if (!(size_px > 0.0f) || size_px > EMBI_FIT_MAX_PX) {
        return 1.0f;
    }
    float upem = (float)face->font.metrics.units_per_em;
    float xh = glyph_top(t, face, inst, 'x');
    if (!(xh > 0.0f)) {
        xh = (float)face->font.metrics.x_height;
    }
    if (!(xh > 0.0f) || !(upem > 0.0f)) {
        return 1.0f;
    }
    float xpx = xh / upem * size_px;
    if (xpx < EMBI_FIT_MIN_XHEIGHT_PX) {
        return 1.0f;
    }
    float k = SDL_roundf(xpx) / xpx;
    float cap = glyph_top(t, face, inst, 'H');
    float cpx = (cap > xh ? cap : xh * 1.4f) / upem * size_px;
    float lim = 0.5f / cpx;
    k = k < 1.0f - lim ? 1.0f - lim : (k > 1.0f + lim ? 1.0f + lim : k);
    /* The cap height is fitted too when it is within 0.25 px of a whole pixel and the x-height fit left it
     * off the grid: the scale that puts the cap on its row is taken when it moves the x-height by at most
     * 0.35 px from its row. Otherwise the x-height decides. */
    float capk = cpx * k;
    if (SDL_fabsf(capk - SDL_roundf(capk)) > 0.25f && SDL_fabsf(cpx - SDL_roundf(cpx)) <= 0.25f) {
        float kc = SDL_roundf(cpx) / cpx;
        if (SDL_fabsf(xpx * kc - SDL_roundf(xpx)) <= 0.35f && SDL_fabsf(kc - 1.0f) <= lim) {
            k = kc;
        }
    }
    return k;
}

static float stem_darkening(float size_px)
{
    if (size_px <= EMBI_DARKEN_SMALL_UPTO_PX) {
        return EMBI_DARKEN_SMALL_PX;
    }
    if (size_px <= EMBI_DARKEN_FULL_PX) {
        return EMBI_DARKEN_SMALL_PX + (EMBI_DARKEN_MAX_PX - EMBI_DARKEN_SMALL_PX) *
                                          (size_px - EMBI_DARKEN_SMALL_UPTO_PX) /
                                          (EMBI_DARKEN_FULL_PX - EMBI_DARKEN_SMALL_UPTO_PX);
    }
    if (size_px >= EMBI_DARKEN_NONE_PX) {
        return 0.0f;
    }
    return EMBI_DARKEN_MAX_PX * (EMBI_DARKEN_NONE_PX - size_px) /
           (EMBI_DARKEN_NONE_PX - EMBI_DARKEN_FULL_PX);
}

/* ------------------------------------------------------------------ bitmap colour glyphs (W4i) */

/* The decoded bitmap glyphs: key -> embi_pixels * (or BITMAP_FAILED, so a glyph that does not decode is tried once). */
#define EMBI_BITMAP_BUDGET (16u << 20)
#define EMBI_BITMAP_FAILED ((uintptr_t)1)

typedef struct bitmap_state {
    embi_hashmap map;
    size_t bytes;
} bitmap_state;

static bitmap_state *bitmap_of(embi_text *t)
{
    if (t->bitmap == nullptr) {
        bitmap_state *b = embi_calloc(1, sizeof *b);
        embi_hashmap_init(&b->map);
        t->bitmap = b;
    }
    return t->bitmap;
}

/* Frees the decoded pixels and the cache. Entries are only read during a call, so this is safe between calls. */
static void bitmap_free(embi_text *t)
{
    bitmap_clear(t);
    if (t->bitmap != nullptr) {
        bitmap_state *b = t->bitmap;
        embi_hashmap_free(&b->map);
        embi_free(b, sizeof *b);
        t->bitmap = nullptr;
    }
}

/* Frees the decoded pixels and empties the cache. */
static void bitmap_clear(embi_text *t)
{
    bitmap_state *b = t->bitmap;
    if (b == nullptr) {
        return;
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&b->map);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        if (v != EMBI_BITMAP_FAILED) {
            embi_pixels *p = (embi_pixels *)(uintptr_t)v;
            embi_pixels_free(p);
            embi_free(p, sizeof *p);
        }
    }
    embi_hashmap_clear(&b->map);
    b->bytes = 0;
}

void embi_text_set_bitmap_sink(embi_text *t, embi_text_bitmap_fn fn, void *user)
{
    if (t != nullptr) {
        t->bitmap_fn = fn;
        t->bitmap_user = user;
    }
}

/* The decoded pixels of a bitmap glyph, from the cache or decoded now (nullptr when it does not decode). */
static const embi_pixels *bitmap_pixels(embi_text *t, uint64_t key, const embi_bitmap_glyph *bg)
{
    bitmap_state *b = bitmap_of(t);
    uint64_t v = 0;
    if (embi_hashmap_get(&b->map, key, &v)) {
        return v == EMBI_BITMAP_FAILED ? nullptr : (const embi_pixels *)(uintptr_t)v;
    }
    embi_pixels px = {0, 0, nullptr};
    embi_pixels *p = nullptr;
    if (embi_png_decode(bg->png, bg->len, EMBI_IMAGE_DEFAULT_MAX_PIXELS, false, &px)) {
        p = embi_alloc(sizeof *p);
        *p = px;
        size_t bytes = (size_t)px.w * px.h * 4u;
        if (b->bytes + bytes > EMBI_BITMAP_BUDGET) {
            bitmap_clear(t); /* the sink has copied what this frame draws; older glyphs decode again */
        }
        b->bytes += bytes;
        embi_hashmap_put(&b->map, key, (uint64_t)(uintptr_t)p);
        return p;
    }
    embi_hashmap_put(&b->map, key, (uint64_t)EMBI_BITMAP_FAILED);
    return nullptr;
}

/* Draws glyph gid of face as its bitmap strike, when the face has one for the size (size and scale in local px and
 * device px per local px). The image is handed to the sink; false when the glyph has no bitmap or nothing was drawn,
 * and the caller then draws the outline. */
static bool draw_bitmap_glyph(embi_text *t, embi_frame *f, embi_face *face, uint32_t gid, float size, float scale,
                              float x, float y, uint32_t xform, uint32_t clip)
{
    if (t->bitmap_fn == nullptr || size <= 0.0f || scale <= 0.0f) {
        return false;
    }
    embi_bitmap_glyph bg;
    if (!embi_font_bitmap_glyph(&face->font, gid, size * scale, &bg)) {
        return false;
    }
    /* Strikes are keyed by face, glyph and ppem: the sink's image cache uses the same key. */
    uint64_t key = ((uint64_t)face->uid << 40) ^ ((uint64_t)(bg.ppem & 0xFFFFu) << 24) ^ (uint64_t)(gid & 0xFFFFFFu);
    const embi_pixels *px = bitmap_pixels(t, key, &bg);
    if (px == nullptr || px->w == 0 || px->h == 0 || bg.ppem == 0) {
        return false;
    }
    /* Local px per strike px. The top edge is bg.top (CBDT) or originY + height (sbix). */
    float k = size / (float)bg.ppem;
    float top = bg.top_from_bottom ? bg.origin_y + (float)px->h : bg.top;
    float rect[4] = {x + bg.left * k, y - top * k, (float)px->w * k, (float)px->h * k};
    return t->bitmap_fn(t->bitmap_user, f, px, key, rect, xform, clip);
}

/* Draws one glyph at the glyph-units-to-local transform G. fit says how crisp text is done. strength64 is the synthetic bold
 * emboldening in 1/64 font units (0: regular outline); colour layers and bitmaps are never emboldened. */
static void draw_glyph(embi_text *t, embi_frame *f, embi_face *face, uint32_t inst,
                       uint32_t gid, const float G[6], uint32_t color, uint32_t xform,
                       uint32_t clip, const text_fit *fit, float bsize, float bscale, uint32_t strength64)
{
    const int16_t *coords = embi_face_inst_coords(face, inst);
    if (face->font.metrics.has_colr) {
        colr_ctx c;
        SDL_memset(&c, 0, sizeof c);
        c.f = f;
        c.t = t;
        c.face = face;
        c.inst = inst;
        SDL_memcpy(c.G, G, sizeof c.G);
        c.total[0] = 1.0f;
        c.total[3] = 1.0f;
        c.text_rgba = color;
        c.xform = xform;
        c.clip = clip;
        embi_colr_visitor vis = {&c, colr_visit};
        /* A colour glyph replaces its outline: its layers are the glyph. */
        if (embi_font_colr_paint(&face->font, gid, coords, 0, &vis)) {
            return;
        }
        /* No COLR record for this glyph: a bitmap strike of the face (bsize > 0 asks for it) comes before the outline. */
        if (bsize > 0.0f && draw_bitmap_glyph(t, f, face, gid, bsize, bscale, G[4], G[5], xform, clip)) {
            return;
        }
    }
    /* The regular outline is what hinting works on; the bold one (centred) is what is drawn unhinted. */
    const outline_entry *ro = outline_get(t, face, inst, gid);
    if (ro == nullptr || ro->n == 0) {
        return;
    }
    const outline_entry *oe = strength64 != 0 ? bold_get(t, face, inst, gid, strength64) : ro;
    if (oe == nullptr || oe->n == 0) {
        return;
    }
    /* The cache key of the unhinted curves drawn: the bold variant, or the glyph. */
    const uint64_t ckey = strength64 != 0 ? variant_key(face->uid, inst, gid, strength64) : glyph_key(face->uid, inst, gid);
    float M[6];
    SDL_memcpy(M, G, sizeof M);
    /* A glyph of a pixel-art face (the face passes embi_hint_pixel_face, and the glyph lies on its grid): its edges go onto whole
     * device px, drawn without darkening, with the origin on a device px too. SHARP snaps when the step is within 0.15 px of a
     * whole number of px (else the glyph is hinted as usual: a bigger change would outgrow the advances), NATURAL within 0.02
     * and then only for glyphs not made wholly of rectangles (those keep the top and bottom rounding below). A bold glyph is not
     * snapped: it takes the hinted path, whose stems grow by whole px. */
    if (fit->on && fit->iso && fit->mode != EMBI_TEXT_HINTING_NONE && fit->pixel_unit > 0.0f && strength64 == 0 &&
        xform == 0 && G[1] == 0.0f && G[2] == 0.0f && G[3] < 0.0f &&
        (fit->mode == EMBI_TEXT_HINTING_SHARP || !ro->boxy)) {
        outline_entry *mro = (outline_entry *)(uintptr_t)ro;
        if (!mro->pixel_known) {
            mro->pixel_known = true;
            mro->pixel_unit = embi_hint_pixel_on_grid(ro->curves, ro->n, fit->pixel_unit) ? fit->pixel_unit : 0.0f;
        }
        const bool sharp = fit->mode == EMBI_TEXT_HINTING_SHARP;
        const float dev = -G[3] * fit->m3;
        float k = mro->pixel_unit > 0.0f ? embi_hint_pixel_scale(mro->pixel_unit, dev, sharp) : 0.0f;
        if (k > 0.0f && sharp && SDL_fabsf(mro->pixel_unit * dev - SDL_roundf(mro->pixel_unit * dev)) > 0.15f) {
            k = 0.0f;
        }
        if (k > 0.0f) {
            const uint32_t kpx = (uint32_t)SDL_roundf(k * mro->pixel_unit);
            uint64_t pkey = 0;
            const outline_entry *pe = pixel_get(t, face, inst, gid, ro, kpx, &pkey);
            /* The curves are at k px per unit: the glyph is drawn at that scale (a uniform factor on the layout scale), with
             * its baseline on a row and its origin on a column. */
            const float r = k / dev;
            const float devy = fit->m3 * G[5] + fit->m5;
            const float snap = SDL_floorf(devy + 0.5f + 0.002f) - devy;
            const float devx = fit->m0 * G[4] + fit->m4;
            const float snapx = SDL_floorf(devx + 0.5f + 0.002f) - devx;
            M[0] = G[0] * r;
            M[3] = G[3] * r;
            M[4] = G[4] + snapx / fit->m0;
            M[5] = G[5] + snap / fit->m3;
            embi_frame_fill_text(f, pkey, pe->curves, pe->n, M, color, 0.0f, fit->lcd, xform, clip);
            return;
        }
    }
    if (oe->boxy && fit->mode != EMBI_TEXT_HINTING_NONE && xform == 0 && G[1] == 0.0f && G[2] == 0.0f && G[3] < 0.0f &&
        oe->ymax > oe->ymin) {
        /* A glyph made of rectangles has its top and bottom edges on whole px, as the hinting that browsers apply
         * does for the test font Ahem (an em square 0.8 above and 0.2 below the baseline would otherwise leave a
         * partly covered row at each edge). */
        float top = SDL_roundf(fit->m3 * (G[5] + G[3] * oe->ymax) + fit->m5);
        float bot = SDL_roundf(fit->m3 * (G[5] + G[3] * oe->ymin) + fit->m5);
        if (bot > top) {
            /* Edges are rounded in device px (m3, m5 map local y to the target) and mapped back. */
            float lt = (top - fit->m5) / fit->m3, lb = (bot - fit->m5) / fit->m3;
            float sy = (lb - lt) / (oe->ymax - oe->ymin);
            M[3] = -sy;
            M[5] = lt + oe->ymax * sy;
        }
    }
    if (fit->on && (!oe->boxy || fit->mode == EMBI_TEXT_HINTING_NONE)) {
        /* NONE: the glyph is drawn at its true transform through the same contrast path, with no darkening. */
        if (fit->mode != EMBI_TEXT_HINTING_NONE && G[1] == 0.0f && G[3] < 0.0f) {
            /* The baseline of this glyph goes to a pixel row, and the outline is scaled about it. */
            float devy = fit->m3 * G[5] + fit->m5;
            /* Round half up with a small tolerance, so that two layouts that reach the same baseline through
             * different float sums (a reftest and its reference) land on the same row at an exact .5. */
            float snap = SDL_floorf(devy + 0.5f + 0.002f) - devy;
            M[3] = G[3] * fit->k;
            M[5] = G[5] + snap / fit->m3;
            if (fit->hint) {
                uint64_t hkey = 0;
                /* The x fitting needs equal scales and no skew (a synthetic oblique): else the glyph is hinted as in NATURAL. */
                const embi_text_hinting hmode =
                    fit->mode == EMBI_TEXT_HINTING_SHARP && fit->iso && G[2] == 0.0f ? EMBI_TEXT_HINTING_SHARP
                                                                                     : EMBI_TEXT_HINTING_NATURAL;
                const outline_entry *he = hint_get(t, face, inst, gid, ro, -G[3] * fit->m3, strength64, hmode, &hkey);
                if (he != nullptr) {
                    M[3] = G[3];
                    embi_frame_fill_text(f, hkey, he->curves, he->n, M, color, fit->darken, fit->lcd, xform, clip);
                    return;
                }
            }
        }
        embi_frame_fill_text(f, ckey, oe->curves, oe->n, M, color,
                             fit->darken, fit->lcd, xform, clip);
        return;
    }
    embi_paint paint = embi_paint_solid(color);
    embi_frame_fill_quads(f, ckey, oe->curves, oe->n, M, &paint,
                          EMBI_FILL_NONZERO, xform, clip);
}

void embi_text_draw(embi_text *t, embi_frame *f, const embi_text_layout *lay,
                    const embi_text_vruns *vr, float x, float y, uint32_t color, uint32_t xform,
                    uint32_t clip)
{
    if (t == nullptr || f == nullptr || lay == nullptr || vr == nullptr) {
        return;
    }
    outline_state *o = outline_of(t);
    if (o->bytes > o->budget) {
        /* Entries are only read during a call, so clearing between calls is safe. */
        embi_outline_clear(t);
    } else if (o->hint_bytes > EMBI_HINT_BUDGET) {
        hint_clear(o);
    }
    embi_text_target tt;
    const bool have_target = embi_frame_text_target(f, xform, &tt);
    const embi_text_hinting mode = embi_frame_text_hinting(f);
    for (uint32_t i = 0; i < vr->items.len; i++) {
        const embi_text_vrun *it = &vr->items.data[i];
        const embi_text_run *run = &lay->runs.data[it->run];
        if (run->face == nullptr || run->size <= 0.0f) {
            continue;
        }
        uint32_t upem = run->face->font.metrics.units_per_em;
        if (upem == 0) {
            continue;
        }
        float s = run->size / (float)upem;
        float pen = x + it->x;
        text_fit fit = {.on = false, .k = 1.0f, .m3 = 1.0f, .m5 = 0.0f, .mode = mode, .m0 = 1.0f};
        if (have_target) {
            fit.m3 = tt.m[3];
            fit.m5 = tt.m[5];
            fit.m0 = tt.m[0];
            fit.m4 = tt.m[4];
            fit.iso = SDL_fabsf(tt.m[0] - tt.m[3]) <= 1e-4f * SDL_fabsf(tt.m[3]);
        }
        if (have_target && tt.fit && !run->face->font.metrics.has_colr) {
            float size_px = run->size * tt.m[3];
            fit.on = true;
            fit.m3 = tt.m[3];
            fit.m5 = tt.m[5];
            fit.lcd = tt.lcd && size_px <= EMBI_LCD_MAX_PX;
            fit.m0 = tt.m[0];
            fit.m4 = tt.m[4];
            fit.iso = SDL_fabsf(tt.m[0] - tt.m[3]) <= 1e-4f * SDL_fabsf(tt.m[3]);
            if (mode == EMBI_TEXT_HINTING_NONE) {
                fit.k = 1.0f;
                fit.darken = 0.0f;
                fit.hint = false;
            } else {
                fit.k = fit_scale(t, run->face, run->inst, size_px);
                fit.darken = stem_darkening(size_px);
                fit.hint = size_px <= EMBI_FIT_MAX_PX;
                fit.pixel_unit = face_pixel_unit(t, o, run->face, run->inst);
            }
        }
        /* Synthetic bold: the outline grows by about size / 24 (Skia's fake bold: a 24th of the em up to 9 px, 32nd from
         * 36 px), emboldened geometrically (embolden.c). Synthetic oblique: a skew of 14 degrees. */
        float embolden = 0.0f;
        uint32_t strength64 = 0;
        if ((run->synth & EMBI_RUN_SYNTH_BOLD) != 0) {
            float k = run->size <= 9.0f ? 1.0f / 24.0f : (run->size >= 36.0f ? 1.0f / 32.0f : 1.0f / 24.0f - (run->size - 9.0f) / 27.0f * (1.0f / 24.0f - 1.0f / 32.0f));
            embolden = run->size * k;
            /* The strength is quantised coarsely in device px (1/16), so that a size that moves a little keeps its variants, and
             * then taken in font units at 1/64: the cache key and the curves both come from that value. */
            const float dscale = have_target ? tt.m[3] : 1.0f;
            const float dev16 = SDL_roundf(embolden * dscale * 16.0f);
            const float fu = (dev16 > 0.0f ? dev16 / 16.0f : 1.0f / 16.0f) / (dscale * s);
            strength64 = (uint32_t)SDL_roundf(fu * 64.0f);
            strength64 = strength64 > 0 ? strength64 : 1u;
        }
        float skew = (run->synth & EMBI_RUN_SYNTH_OBLIQUE) != 0 ? s * EMBI_OBLIQUE_SKEW : 0.0f;
        /* Bitmap strikes draw where the face has them, when a sink is set. On a face with a COLR table the glyph is
         * decided one by one: its COLR record wins, then its bitmap (see draw_glyph). */
        const bool has_bm = t->bitmap_fn != nullptr && embi_font_has_bitmaps(&run->face->font);
        const bool bitmap_run = has_bm && !run->face->font.metrics.has_colr;
        const float colr_bm = has_bm && run->face->font.metrics.has_colr && embolden <= 0.0f ? run->size : 0.0f;
        float bscale = have_target ? tt.m[3] : 1.0f;
        for (uint32_t g = it->glyph_begin; g < it->glyph_end; g++) {
            const embi_text_glyph *gl = &lay->glyphs.data[g];
            /* SHARP: the pen x of the glyph is a whole device px (the layout advances are unchanged). Only for axis-aligned
             * scale targets, where the device x is m0 * x + m4. */
            float gx = pen + gl->x_offset;
            if (mode == EMBI_TEXT_HINTING_SHARP && have_target && tt.fit && fit.iso) {
                gx = (SDL_floorf(tt.m[0] * gx + tt.m[4] + 0.5f + 0.002f) - tt.m[4]) / tt.m[0];
            }
            if (bitmap_run && draw_bitmap_glyph(t, f, run->face, gl->gid, run->size, bscale, gx,
                                                y - gl->y_offset, xform, clip)) {
                pen += gl->advance;
                continue;
            }
            const float G[6] = {s, 0.0f, skew, -s, gx, y - gl->y_offset};
            draw_glyph(t, f, run->face, run->inst, gl->gid, G, color, xform, clip, &fit, colr_bm, bscale, strength64);
            pen += gl->advance;
        }
    }
}

void embi_text_outline(embi_text *t, const embi_text_layout *lay, const embi_text_vruns *vr, float x, float y,
                       void (*fn)(void *user, const float q[6]), void *user)
{
    if (t == nullptr || lay == nullptr || vr == nullptr || fn == nullptr) {
        return;
    }
    outline_state *o = outline_of(t);
    if (o->bytes > o->budget) {
        embi_outline_clear(t);
    }
    for (uint32_t i = 0; i < vr->items.len; i++) {
        const embi_text_vrun *it = &vr->items.data[i];
        const embi_text_run *run = &lay->runs.data[it->run];
        if (run->face == nullptr || run->size <= 0.0f) {
            continue;
        }
        uint32_t upem = run->face->font.metrics.units_per_em;
        if (upem == 0) {
            continue;
        }
        float s = run->size / (float)upem;
        float skew = (run->synth & EMBI_RUN_SYNTH_OBLIQUE) != 0 ? s * EMBI_OBLIQUE_SKEW : 0.0f;
        float pen = x + it->x;
        for (uint32_t g = it->glyph_begin; g < it->glyph_end; g++) {
            const embi_text_glyph *gl = &lay->glyphs.data[g];
            outline_entry *oe = outline_get(t, run->face, run->inst, gl->gid);
            if (oe != nullptr && oe->n > 0) {
                const float G[6] = {s, 0.0f, skew, -s, pen + gl->x_offset, y - gl->y_offset};
                for (uint32_t k = 0; k < oe->n; k++) {
                    const embi_qcurve *c = &oe->curves[k];
                    float q[6] = {G[0] * c->x0 + G[2] * c->y0 + G[4], G[1] * c->x0 + G[3] * c->y0 + G[5],
                                  G[0] * c->cx + G[2] * c->cy + G[4], G[1] * c->cx + G[3] * c->cy + G[5],
                                  G[0] * c->x1 + G[2] * c->y1 + G[4], G[1] * c->x1 + G[3] * c->y1 + G[5]};
                    fn(user, q);
                }
            }
            pen += gl->advance;
        }
    }
}
