/*
 * Scrolling of W4d. See scroll.h. The offsets are stored by the layout (W3h) and read by
 * the display list build; the damage between two lists then covers the moved content (damage.c).
 */
#include "paint/scroll.h"

#include "css/props.h"
#include "paint/paint_int.h"
#include "style/style.h"

#include <math.h>
#include <SDL3/SDL_stdinc.h>
#include <stdint.h>
#include <string.h>

/* Clamps v to [lo, hi]. A NaN comes out as lo, so a bad input can never reach a stored offset. */
static float clampf(float v, float lo, float hi)
{
    if (!(v >= lo)) {
        return lo;
    }
    return v > hi ? hi : v;
}

void embi_scroll_max(const embi_frag *f, float *max_x, float *max_y)
{
    *max_x = 0.0f;
    *max_y = 0.0f;
    if (f == nullptr || f->kind != EMBI_FK_BOX) {
        return;
    }
    float ov[4];
    embi_frag_ov(f, ov);
    /* The visible window of the content ends at the padding edge less the scrollbar gutters (the inline end gutter
     * narrows the width, the block end one the height). The overflow rectangle includes the padding end. */
    float right = f->w - embi_frag_bdr(f, 2) - embi_frag_scr(f, 0);
    float bottom = f->h - embi_frag_bdr(f, 3) - embi_frag_scr(f, 1);
    *max_x = fmaxf(0.0f, ov[2] - right);
    *max_y = fmaxf(0.0f, ov[3] - bottom);
}

void embi_scroll_max_root(const embi_frag *root, float vw, float vh, float *max_x, float *max_y)
{
    *max_x = 0.0f;
    *max_y = 0.0f;
    if (root == nullptr || root->kind != EMBI_FK_BOX) {
        return;
    }
    float ov[4];
    embi_frag_ov(root, ov);
    *max_x = fmaxf(0.0f, ov[2] - vw);
    *max_y = fmaxf(0.0f, ov[3] - vh);
}

bool embi_paint_scroll_by(embi_layout_engine *e, const embi_document *doc, embi_frag *f, float dx, float dy, bool root,
                          float vw, float vh)
{
    if (f == nullptr || f->kind != EMBI_FK_BOX) {
        return false;
    }
    if (!root && (f->flags & EMBI_FF_SCROLL) == 0) {
        return false;
    }
    /* A non-finite delta (NaN, or inf - inf in the caller) scrolls nothing. */
    if (!isfinite(dx)) {
        dx = 0.0f;
    }
    if (!isfinite(dy)) {
        dy = 0.0f;
    }
    float mx, my;
    if (root) {
        embi_scroll_max_root(f, vw, vh, &mx, &my);
    } else {
        embi_scroll_max(f, &mx, &my);
    }
    float sx = embi_frag_sx(f), sy = embi_frag_sy(f);
    float nx = clampf(sx + dx, 0.0f, mx);
    float ny = clampf(sy + dy, 0.0f, my);
    if (nx == sx && ny == sy) {
        return false;
    }
    embi_layout_frag_set_scroll(e, doc, f, nx, ny);
    /* The layout stores the offset in the side record; a fragment without one keeps its offset. */
    return embi_frag_sx(f) != sx || embi_frag_sy(f) != sy;
}

/* ------------------------------------------------------------------ scroll as a translation of the content (scroll60) */

/* An offset rounded to a whole device px: the content then keeps the pixel grid its boxes were snapped to, whatever the offset. */
static float quant_off(float v, float scale)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    if (!(scale > 0.0f)) {
        scale = 1.0f; /* a config without a scale paints at 1 */
    }
    return roundf(v * scale) / scale;
}

uint32_t embi_pb_xform_scroll(const embi_dl *d, uint32_t xf)
{
    return xf < d->xforms.len ? d->xforms.data[xf].scroll : 0u;
}

void embi_pb_unscroll_delta(embi_pst *p, uint32_t xf, float *dx, float *dy)
{
    *dx = 0.0f;
    *dy = 0.0f;
    const embi_dl *d = p->d;
    uint32_t t = embi_pb_xform_scroll(d, xf);
    if (t == 0 || t > d->scrolls.len || xf >= d->xforms.len) {
        return;
    }
    uint32_t xo = xf;
    for (uint32_t guard = 0; t != 0 && t <= d->scrolls.len && guard < 64; guard++) {
        xo = d->scrolls.data[t - 1].outer;
        t = embi_pb_xform_scroll(d, xo);
    }
    const float *a = d->xforms.data[xf].m;
    *dx = a[4] - (xo != 0 && xo < d->xforms.len ? d->xforms.data[xo].m[4] : 0.0f);
    *dy = a[5] - (xo != 0 && xo < d->xforms.len ? d->xforms.data[xo].m[5] : 0.0f);
}

void embi_pb_scroll_rigid(embi_pst *p)
{
    p->d->scroll_rigid = true;
}

uint32_t embi_pb_scroll_enter(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf)
{
    embi_dl *d = p->d;
    bool root = k == p->root;
    if (!root && (k->flags & EMBI_FF_SCROLL) == 0) {
        return xf;
    }
    if (p->c3 != nullptr) {
        d->scroll_rigid = true; /* inside a 3D rendering context: the plane's transforms are made another way */
        return xf;
    }
    float sx = embi_frag_sx(k), sy = embi_frag_sy(k);
    float mx = 0.0f, my = 0.0f;
    if (root) {
        embi_scroll_max_root(k, p->vw, p->vh, &mx, &my);
    } else {
        embi_scroll_max(k, &mx, &my);
    }
    if (!(mx > 0.0f || my > 0.0f || sx != 0.0f || sy != 0.0f)) {
        return xf;
    }
    uint64_t v = 0;
    if (embi_hashmap_get(&p->scr, (uint64_t)(uintptr_t)k, &v) && (uint32_t)(v >> 32) == xf) {
        return (uint32_t)v;
    }
    float sc = p->cfg != nullptr && p->cfg->scale > 0.0f ? p->cfg->scale : 1.0f;
    float qx = quant_off(sx, sc), qy = quant_off(sy, sc);
    static const float id[6] = {1, 0, 0, 1, 0, 0};
    float m[6];
    memcpy(m, xf < d->xforms.len ? d->xforms.data[xf].m : id, sizeof m);
    embi_dscroll n;
    memset(&n, 0, sizeof n);
    n.key = (uint64_t)(uintptr_t)k;
    n.outer = xf;
    n.parent = embi_pb_xform_scroll(d, xf);
    n.sx = qx;
    n.sy = qy;
    memcpy(n.lin, m, sizeof n.lin);
    n.root = root;
    if (root) {
        n.rect[2] = p->vw;
        n.rect[3] = p->vh;
    } else {
        n.rect[0] = kx;
        n.rect[1] = ky;
        n.rect[2] = k->w;
        n.rect[3] = k->h;
    }
    embi_vec_push(&d->scrolls, n);
    uint32_t idx = d->scrolls.len - 1;
    d->cur_scroll = idx + 1;
    float c[6] = {m[0], m[1], m[2], m[3], m[4] - m[0] * qx - m[2] * qy, m[5] - m[1] * qx - m[3] * qy};
    uint32_t nx = embi_pb_xform(p, c);
    d->scrolls.data[idx].xform = nx;
    (void)embi_hashmap_put(&p->scr, (uint64_t)(uintptr_t)k, ((uint64_t)xf << 32) | nx);
    return nx;
}

bool embi_dl_scroll_retarget(embi_dl *d, const float *off, embi_scroll_damage *dmg)
{
    if (dmg != nullptr) {
        memset(dmg, 0, sizeof *dmg);
    }
    if (d->scroll_rigid || d->planes.len != 0 || d->fx_items != 0) {
        return false;
    }
    uint32_t n = d->scrolls.len;
    if (n == 0) {
        return true;
    }
    float *tot = SDL_calloc((size_t)(n + 1) * 2, sizeof *tot); /* the translation each container adds, [0] is none */
    if (tot == nullptr) {
        return false;
    }
    bool moved = false;
    for (uint32_t i = 0; i < n; i++) {
        embi_dscroll *s = &d->scrolls.data[i];
        float dx = off[2 * i] - s->sx, dy = off[2 * i + 1] - s->sy;
        float tx = -(s->lin[0] * dx + s->lin[2] * dy), ty = -(s->lin[1] * dx + s->lin[3] * dy);
        tot[2 * (i + 1)] = tx + (s->parent != 0 ? tot[2 * s->parent] : 0.0f);
        tot[2 * (i + 1) + 1] = ty + (s->parent != 0 ? tot[2 * s->parent + 1] : 0.0f);
        moved = moved || dx != 0.0f || dy != 0.0f;
    }
    if (moved) {
        for (uint32_t i = 1; i < d->xforms.len; i++) {
            embi_dxform *x = &d->xforms.data[i];
            if (x->scroll != 0 && x->scroll <= n) {
                x->m[4] += tot[2 * x->scroll];
                x->m[5] += tot[2 * x->scroll + 1];
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_dscroll *s = &d->scrolls.data[i];
            bool did = off[2 * i] != s->sx || off[2 * i + 1] != s->sy;
            s->sx = off[2 * i];
            s->sy = off[2 * i + 1];
            if (did && dmg != nullptr) {
                if (s->root || dmg->n >= EMBI_SCROLL_DAMAGE_MAX) {
                    dmg->full = true;
                } else {
                    /* the box in page px (the outer transform is already moved by the containers around it) */
                    float b[4] = {0, 0, 0, 0};
                    static const float id[6] = {1, 0, 0, 1, 0, 0};
                    const float *m = s->outer < d->xforms.len ? d->xforms.data[s->outer].m : id;
                    float xs[4] = {s->rect[0], s->rect[0] + s->rect[2], s->rect[0], s->rect[0] + s->rect[2]};
                    float ys[4] = {s->rect[1], s->rect[1], s->rect[1] + s->rect[3], s->rect[1] + s->rect[3]};
                    for (int c = 0; c < 4; c++) {
                        float x = m[0] * xs[c] + m[2] * ys[c] + m[4], y = m[1] * xs[c] + m[3] * ys[c] + m[5];
                        b[0] = c == 0 ? x : fminf(b[0], x);
                        b[1] = c == 0 ? y : fminf(b[1], y);
                        b[2] = c == 0 ? x : fmaxf(b[2], x);
                        b[3] = c == 0 ? y : fmaxf(b[3], y);
                    }
                    memcpy(dmg->r[dmg->n++], b, sizeof b);
                }
            }
        }
    }
    SDL_free(tot);
    return true;
}

float embi_scroll_quantize(float off, float scale)
{
    return quant_off(off, scale);
}

void embi_scroll_sticky_offset(const embi_frag *f, float *dx, float *dy)
{
    embi_lay_sticky_offset(f, dx, dy);
}

bool embi_scroll_is_smooth(const embi_style *s)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_scroll_behavior) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_smooth;
}

void embi_scroll_anim_start(embi_scroll_anim *a, float x0, float y0, float x1, float y1, double now_ms)
{
    a->x0 = x0;
    a->y0 = y0;
    a->x1 = x1;
    a->y1 = y1;
    a->t0 = now_ms;
    a->active = true;
}

float embi_scroll_ease_out(float t)
{
    float u = 1.0f - clampf(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

bool embi_scroll_anim_sample(embi_scroll_anim *a, double now_ms, float *x, float *y)
{
    if (!a->active) {
        *x = a->x1;
        *y = a->y1;
        return false;
    }
    double t = (now_ms - a->t0) / EMBI_SCROLL_SMOOTH_MS;
    if (t >= 1.0) {
        a->active = false;
        *x = a->x1;
        *y = a->y1;
        return false;
    }
    float k = embi_scroll_ease_out(t < 0.0 ? 0.0f : (float)t);
    *x = a->x0 + (a->x1 - a->x0) * k;
    *y = a->y0 + (a->y1 - a->y0) * k;
    return true;
}

void embi_sb_note_scroll(embi_sb_fade *f, double now_ms)
{
    f->last_ms = now_ms;
    f->seen = true;
}

float embi_sb_alpha(const embi_sb_fade *f, double now_ms)
{
    if (!f->seen) {
        return 0.0f;
    }
    double dt = now_ms - f->last_ms;
    if (dt <= EMBI_SB_VISIBLE_MS) {
        return 1.0f;
    }
    double u = (dt - EMBI_SB_VISIBLE_MS) / EMBI_SB_FADE_MS;
    if (u >= 1.0) {
        return 0.0f;
    }
    return (float)(1.0 - u);
}

embi_sb_style embi_sb_style_default(void)
{
    embi_sb_style s;
    s.width = 8.0f;
    s.thumb = 0x66808080u; /* gray at 40% alpha */
    s.track = 0x00000000u;
    s.vbar_left = false;
    return s;
}

bool embi_sb_thumb(float track, float client, float content, float offset, float *start, float *len)
{
    *start = 0.0f;
    *len = 0.0f;
    if (!(track > 0.0f) || !(client >= 0.0f) || !(content > client)) {
        return false;
    }
    float l = track * client / content;
    float minl = track < EMBI_SB_MIN_THUMB ? track : EMBI_SB_MIN_THUMB;
    if (l < minl) {
        l = minl;
    }
    if (l > track) {
        l = track;
    }
    float range = content - client;
    float o = clampf(offset, 0.0f, range);
    *start = (track - l) * (o / range);
    *len = l;
    return true;
}

embi_sb_style embi_sb_style_resolve(enum embi_sb_width width, bool has_colors, uint32_t thumb, uint32_t track)
{
    embi_sb_style s = embi_sb_style_default();
    switch (width) {
    case EMBI_SBW_THIN:
        s.width = 4.0f;
        break;
    case EMBI_SBW_NONE:
        s.width = 0.0f;
        break;
    default:
        s.width = 8.0f;
        break;
    }
    if (has_colors) {
        s.thumb = thumb;
        s.track = track;
    }
    return s;
}

embi_sb_style embi_sb_style_from(const embi_style *st)
{
    if (st == nullptr) {
        return embi_sb_style_default();
    }
    enum embi_sb_width w = EMBI_SBW_AUTO;
    uint16_t wk = embi_pb_kw(st, EMBI_PROP_scrollbar_width);
    if (wk == EMBI_KW_thin) {
        w = EMBI_SBW_THIN;
    } else if (wk == EMBI_KW_none) {
        w = EMBI_SBW_NONE;
    }
    bool has = false;
    uint32_t thumb = 0, track = 0;
    const embi_value *cv = embi_style_value(st, EMBI_PROP_scrollbar_color);
    if (cv != nullptr && cv->kind == EMBI_V_LIST && cv->list.n == 2) {
        has = embi_pb_color(st, &cv->list.items[0], &thumb) && embi_pb_color(st, &cv->list.items[1], &track);
    }
    embi_sb_style out = embi_sb_style_resolve(w, has, thumb, track);
    /* The vertical bar moves to the left edge for direction: rtl in a horizontal writing mode. In a vertical writing mode
     * the bars stay on the right and bottom edges. */
    uint16_t wm = embi_pb_kw(st, EMBI_PROP_writing_mode);
    bool horizontal = wm == 0 || wm == EMBI_KW_horizontal_tb;
    out.vbar_left = horizontal && embi_pb_kw(st, EMBI_PROP_direction) == EMBI_KW_rtl;
    return out;
}

/* The color with its alpha multiplied by a (0..1). */
static uint32_t scale_alpha(uint32_t rgba, float a)
{
    float v = (float)((rgba >> 24) & 0xFFu) * a + 0.5f;
    uint32_t al = v <= 0.0f ? 0u : (v >= 255.0f ? 255u : (uint32_t)v);
    return (rgba & 0x00FFFFFFu) | (al << 24);
}

static void push_fill(embi_dl *d, float x, float y, float w, float h, uint32_t rgba)
{
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_FILL;
    it.rect[0] = x;
    it.rect[1] = y;
    it.rect[2] = w;
    it.rect[3] = h;
    it.colors[0] = rgba;
    embi_vec_push(&d->items, it);
}

/* One bar along an axis: the track of length track starting at (x, y) and width width across the bar, and the thumb of a
 * client of length client over content of length content at offset off. Returns the items appended. */
static uint32_t emit_bar(embi_dl *d, bool vertical, float x, float y, float width, float track, float client,
                         float content, float off, const embi_sb_style *st, float alpha)
{
    float start = 0.0f, len = 0.0f;
    if (!embi_sb_thumb(track, client, content, off, &start, &len)) {
        return 0;
    }
    uint32_t n = 0;
    uint32_t tc = scale_alpha(st->track, alpha);
    uint32_t hc = scale_alpha(st->thumb, alpha);
    if ((tc >> 24) != 0) {
        if (vertical) {
            push_fill(d, x, y, width, track, tc);
        } else {
            push_fill(d, x, y, track, width, tc);
        }
        n++;
    }
    if ((hc >> 24) != 0 && len > 0.0f) {
        if (vertical) {
            push_fill(d, x, y + start, width, len, hc);
        } else {
            push_fill(d, x + start, y, len, width, hc);
        }
        n++;
    }
    return n;
}

uint32_t embi_sb_emit(embi_dl *d, const embi_frag *f, float x, float y, const embi_sb_style *st, float alpha)
{
    if (d == nullptr || f == nullptr || st == nullptr || f->kind != EMBI_FK_BOX || !(alpha > 0.0f) ||
        !(st->width > 0.0f)) {
        return 0;
    }
    float ov[4];
    embi_frag_ov(f, ov);
    /* The padding box, less the block and inline end gutters (the same visible window as embi_scroll_max). The
     * scroll range of an axis is content - client with both measured from the padding edge, so the thumb math of
     * embi_sb_thumb gives the same end position as the offset clamp. */
    float right = f->w - embi_frag_bdr(f, 2) - embi_frag_scr(f, 0);
    float bottom = f->h - embi_frag_bdr(f, 3) - embi_frag_scr(f, 1);
    uint32_t n = 0;
    float max_y = fmaxf(0.0f, ov[3] - bottom);
    float max_x = fmaxf(0.0f, ov[2] - right);
    bool vert = max_y > 0.0f, horz = max_x > 0.0f;
    /* With both bars showing, each track stops short of the other bar (the corner square stays empty). The thumb ratio
     * still uses the full client size, so its end position matches the offset clamp. */
    float corner = vert && horz ? st->width : 0.0f;
    float left = x + embi_frag_bdr(f, 0), top = y + embi_frag_bdr(f, 1);
    if (vert) {
        float vx = st->vbar_left ? left : x + f->w - embi_frag_bdr(f, 2) - st->width;
        float client = bottom - embi_frag_bdr(f, 1);
        float content = ov[3] - embi_frag_bdr(f, 1);
        float track = fmaxf(0.0f, client - corner);
        n += emit_bar(d, true, vx, top, st->width, track, client, content, embi_frag_sy(f), st, alpha);
    }
    if (horz) {
        float hx = left + (st->vbar_left ? corner : 0.0f);
        float hy = y + f->h - embi_frag_bdr(f, 3) - st->width;
        float client = right - embi_frag_bdr(f, 0);
        float content = ov[2] - embi_frag_bdr(f, 0);
        float track = fmaxf(0.0f, client - corner);
        n += emit_bar(d, false, hx, hy, st->width, track, client, content, embi_frag_sx(f), st, alpha);
    }
    return n;
}
