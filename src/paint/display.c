/*
 * The display list storage (W4c): the arrays of items, clips, transforms, stops, contexts, runs and URLs,
 * and the builders the painters call. Every append goes to a pooled array, so a build on a warm list does
 * not allocate.
 */
#include "paint/paint_int.h"

#include <stdatomic.h>

#include <string.h>

/* dl_paths.c: destroys the paths of the SVG items (it includes the GPU headers, which clash with the CSS ones here). */
void embi_dl_free_paths(embi_dl *d);

/* The runs the painter shaped for form controls (W5e) die with the list's content. */
static void embi_dl_free_own_runs(embi_dl *d)
{
    for (uint32_t i = 0; i < d->own_runs.len; i++) {
        embi_tshim_free((embi_lay_shaped *)d->own_runs.data[i]);
    }
    embi_vec_clear(&d->own_runs);
}

void embi_dl_init(embi_dl *d)
{
    memset(d, 0, sizeof *d);
    /* Index 0 of clips and xforms is the sentinel (no clip) and the identity. */
    embi_dclip none;
    memset(&none, 0, sizeof none);
    embi_vec_push(&d->clips, none);
    embi_dxform id = {{1, 0, 0, 1, 0, 0}, 0};
    embi_vec_push(&d->xforms, id);
}

void embi_dl_reset(embi_dl *d)
{
    if (d->clips.cap == 0) {
        /* never initialised: a zeroed list is empty, so give it its sentinels */
        embi_dl_init(d);
        return;
    }
    embi_vec_clear(&d->items);
    embi_vec_clear(&d->stops);
    embi_vec_clear(&d->ctxs);
    embi_vec_clear(&d->shadows);
    embi_vec_clear(&d->fx);
    d->fx_items = 0;
    embi_vec_clear(&d->planes);
    embi_vec_clear(&d->cpaths);
    embi_vec_clear(&d->curves);
    embi_vec_clear(&d->runs);
    embi_dl_free_own_runs(d);
    embi_dl_free_paths(d);
    embi_vec_clear(&d->urls);
    embi_vec_clear(&d->urltext);
    d->clips.len = 1;
    d->xforms.len = 1;
    embi_vec_clear(&d->scrolls);
    d->cur_scroll = 0;
    d->scroll_rigid = false;
}

void embi_dl_free(embi_dl *d)
{
    embi_vec_free(&d->items);
    embi_vec_free(&d->clips);
    embi_vec_free(&d->xforms);
    embi_vec_free(&d->scrolls);
    embi_vec_free(&d->stops);
    embi_vec_free(&d->ctxs);
    embi_vec_free(&d->shadows);
    embi_vec_free(&d->fx);
    embi_vec_free(&d->planes);
    embi_vec_free(&d->cpaths);
    embi_vec_free(&d->curves);
    embi_vec_free(&d->runs);
    embi_dl_free_own_runs(d);
    embi_vec_free(&d->own_runs);
    embi_dl_free_paths(d);
    embi_vec_free(&d->paths);
    embi_vec_free(&d->urls);
    embi_vec_free(&d->urltext);
}

size_t embi_dl_bytes(const embi_dl *d)
{
    return (size_t)d->items.cap * sizeof(embi_ditem) + (size_t)d->clips.cap * sizeof(embi_dclip) +
           (size_t)d->xforms.cap * sizeof(embi_dxform) + (size_t)d->scrolls.cap * sizeof(embi_dscroll) + (size_t)d->stops.cap * sizeof(embi_dstop) +
           (size_t)d->ctxs.cap * sizeof(embi_dctx) + (size_t)d->shadows.cap * sizeof(embi_dshadow) +
           (size_t)d->fx.cap * sizeof(embi_dfx) + (size_t)d->planes.cap * sizeof(embi_dplane) +
           (size_t)d->cpaths.cap * sizeof(embi_dcpath) + (size_t)d->curves.cap * sizeof(embi_dcurve) +
           (size_t)d->runs.cap * sizeof(void *) + (size_t)d->paths.cap * sizeof(embi_dpath) +
           (size_t)d->urls.cap * sizeof(uint32_t) * 2 + (size_t)d->urltext.cap;
}

embi_str embi_dl_url(const embi_dl *d, uint32_t i)
{
    if (i >= d->urls.len) {
        return (embi_str){"", 0};
    }
    uint32_t off = d->urls.data[2 * i], len = d->urls.data[2 * i + 1];
    if (len == 0) {
        return (embi_str){"", 0};
    }
    return (embi_str){d->urltext.data + off, len};
}

typedef const embi_svg_image *(*svg_image_fn_t)(void *user, embi_str url);
/* Set at every document creation, possibly from several threads: the same function every time, stored atomically. */
static _Atomic(svg_image_fn_t) g_svg_image_fn;

const embi_svg_image *embi_pb_svg_lookup(void *user, embi_str url)
{
    svg_image_fn_t fn = atomic_load_explicit(&g_svg_image_fn, memory_order_acquire);
    return fn != nullptr ? fn(user, url) : nullptr;
}

void embi_paint_set_svg_image_fn(const embi_svg_image *(*fn)(void *user, embi_str url))
{
    atomic_store_explicit(&g_svg_image_fn, fn, memory_order_release);
}

uint32_t embi_pb_url(embi_pst *p, embi_str url)
{
    embi_dl *d = p->d;
    /* The same URL usually repeats (a sprite, a bullet): reuse the last entry when it matches. */
    uint32_t n = d->urls.len / 2;
    if (n > 0) {
        uint32_t off = d->urls.data[2 * (n - 1)], len = d->urls.data[2 * (n - 1) + 1];
        if (len == url.len && (len == 0 || memcmp(d->urltext.data + off, url.ptr, len) == 0)) {
            return n - 1;
        }
    }
    uint32_t off = d->urltext.len;
    for (uint32_t i = 0; i < url.len; i++) {
        embi_vec_push(&d->urltext, url.ptr[i]);
    }
    embi_vec_push(&d->urls, off);
    embi_vec_push(&d->urls, url.len);
    return n;
}

uint32_t embi_pb_clip(embi_pst *p, const float rect[4], const float radii[4], uint32_t xform, uint32_t parent)
{
    embi_dclip c;
    memcpy(c.rect, rect, sizeof c.rect);
    memcpy(c.radii, radii, sizeof c.radii);
    c.xform = xform;
    c.parent = parent;
    uint32_t i = p->d->clips.len;
    embi_vec_push(&p->d->clips, c);
    return i;
}

uint32_t embi_pb_xform(embi_pst *p, const float m[6])
{
    embi_dxform x;
    memcpy(x.m, m, sizeof x.m);
    x.scroll = p->d->cur_scroll;
    uint32_t i = p->d->xforms.len;
    embi_vec_push(&p->d->xforms, x);
    return i;
}

/* Appends an item with its common fields set; the caller fills the rest through the returned pointer, which
 * is valid until the next append. */
embi_ditem *embi_pb_item(embi_pst *p, uint8_t kind, uint32_t xform, uint32_t clip)
{
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = kind;
    it.xform = xform;
    it.clip = clip;
    embi_vec_push(&p->d->items, it);
    return &p->d->items.data[p->d->items.len - 1];
}

bool embi_pb_fill(embi_pst *p, const float rect[4], const float radii[4], uint32_t rgba, uint32_t xform,
                  uint32_t clip)
{
    if ((rgba >> 24) == 0 || !(rect[2] > 0) || !(rect[3] > 0)) {
        return false;
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_FILL, xform, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    memcpy(it->radii, radii, sizeof it->radii);
    it->colors[0] = rgba;
    return true;
}

void embi_pb_gradient(embi_pst *p, uint8_t kind, const float rect[4], const float radii[4], const float geo[6],
                      const embi_dstop *stops, uint32_t n, bool repeat, uint32_t xform, uint32_t clip)
{
    if (n == 0 || !(rect[2] > 0) || !(rect[3] > 0)) {
        return;
    }
    uint32_t first = p->d->stops.len;
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&p->d->stops, stops[i]);
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_GRADIENT, xform, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    memcpy(it->radii, radii, sizeof it->radii);
    memcpy(it->f, geo, sizeof it->f);
    it->sub = kind;
    it->flags = repeat ? EMBI_DF_REPEAT : 0;
    it->aux = first;
    it->aux2 = n;
}

void embi_pb_image(embi_pst *p, embi_str url, const float rect[4], const float uv[4], uint32_t tint,
                   bool nearest, uint32_t xform, uint32_t clip)
{
    if (!(rect[2] > 0) || !(rect[3] > 0)) {
        return;
    }
    if (atomic_load_explicit(&g_svg_image_fn, memory_order_acquire) != nullptr) {
        const embi_svg_image *si = embi_pb_svg_lookup(p->cfg->user, url);
        if (si != nullptr) {
            embi_svg_env env;
            embi_pb_svg_env(p, &env);
            embi_svg_paint_image(&env, p->d, si, rect, uv, tint, xform, clip);
            return;
        }
    }
    uint32_t idx = embi_pb_url(p, url);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_IMAGE, xform, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    memcpy(it->f, uv, 4 * sizeof(float));
    it->colors[0] = tint;
    it->flags = nearest ? EMBI_DF_NEAREST : 0;
    it->aux = idx;
}

void embi_pb_image_tile(embi_pst *p, embi_str url, const float span[4], const float first[2], const float size[2],
                        const float step[2], const float sub[4], uint32_t tint, bool nearest, uint32_t xform,
                        uint32_t clip)
{
    if (!(span[2] > 0) || !(span[3] > 0) || !(size[0] > 0) || !(size[1] > 0) || !(step[0] > 0) || !(step[1] > 0)) {
        return;
    }
    if (atomic_load_explicit(&g_svg_image_fn, memory_order_acquire) != nullptr) {
        const embi_svg_image *si = embi_pb_svg_lookup(p->cfg->user, url);
        if (si != nullptr) {
            /* a vector image is painted once per tile, clipped to the span */
            embi_svg_env env;
            embi_pb_svg_env(p, &env);
            float zero[4] = {0, 0, 0, 0};
            uint32_t tclip = embi_pb_clip(p, span, zero, xform, clip);
            float uvf[4] = {0, 0, 1, 1};
            if (sub != nullptr) {
                memcpy(uvf, sub, sizeof uvf);
            }
            float x0 = first[0] + floorf((span[0] - first[0]) / step[0]) * step[0];
            float y0 = first[1] + floorf((span[1] - first[1]) / step[1]) * step[1];
            uint32_t count = 0;
            for (float y = y0; y < span[1] + span[3] && count < 4096; y += step[1]) {
                for (float x = x0; x < span[0] + span[2] && count < 4096; x += step[0], count++) {
                    float dest[4] = {x, y, size[0], size[1]};
                    embi_svg_paint_image(&env, p->d, si, dest, uvf, tint, xform, tclip);
                }
            }
            return;
        }
    }
    uint32_t idx = embi_pb_url(p, url);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_IMAGE, xform, clip);
    memcpy(it->rect, span, sizeof it->rect);
    it->f[0] = first[0];
    it->f[1] = first[1];
    it->f[2] = size[0];
    it->f[3] = size[1];
    it->f[4] = step[0];
    it->f[5] = step[1];
    if (sub != nullptr) {
        memcpy(it->widths, sub, sizeof it->widths); /* the source sub-rect, see EMBI_DI_IMAGE */
    }
    it->colors[0] = tint;
    it->flags = (uint8_t)(EMBI_DF_TILE | (nearest ? EMBI_DF_NEAREST : 0));
    it->aux = idx;
}

void embi_pb_triangle(embi_pst *p, const float rect[4], uint8_t dir, uint32_t rgba, uint32_t xform, uint32_t clip)
{
    if ((rgba >> 24) == 0 || !(rect[2] > 0) || !(rect[3] > 0)) {
        return;
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_TRIANGLE, xform, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    it->sub = dir;
    it->colors[0] = rgba;
}

void embi_pb_gradient_at(embi_pst *p, uint8_t kind, const float rect[4], const float geo[6], uint32_t first,
                         uint32_t n, bool repeat, uint32_t xform, uint32_t clip)
{
    if (n == 0 || !(rect[2] > 0) || !(rect[3] > 0)) {
        return;
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_GRADIENT, xform, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    memcpy(it->f, geo, sizeof it->f);
    it->sub = kind;
    it->flags = repeat ? EMBI_DF_REPEAT : 0;
    it->aux = first;
    it->aux2 = n;
}
