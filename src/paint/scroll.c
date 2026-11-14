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
    if (ov[2] <= f->w - embi_frag_bdr(f, 2)) {
        /* the overflow rectangle starts as the whole padding box, the gutter of a vertical bar included: content that fits
         * the client box does not scroll sideways */
        *max_x = 0.0f;
    }
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
    float max_x = ov[2] <= f->w - embi_frag_bdr(f, 2) ? 0.0f : fmaxf(0.0f, ov[2] - right);
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

/* ------------------------------------------------------------------ styled (classic) scrollbars */

embi_sb_pe_kind embi_sb_pe_from(embi_style_engine *se, embi_element *el, int hover_axis, embi_sb_pe *out)
{
    memset(out, 0, sizeof *out);
    if (se == nullptr || el == nullptr) {
        return EMBI_SBPE_NONE;
    }
    const embi_style *bar = embi_style_generated(se, el, EMBI_PE_SCROLLBAR);
    if (bar == nullptr) {
        return EMBI_SBPE_NONE;
    }
    float v = 0.0f, h = 0.0f;
    embi_style_scrollbar_thickness(bar, &v, &h);
    if (v < 0.0f || h < 0.0f) {
        return EMBI_SBPE_HIDDEN;
    }
    out->vert = v;
    out->horz = h;
    out->bar = bar;
    out->track = embi_style_generated(se, el, EMBI_PE_SCROLLBAR_TRACK);
    out->corner = embi_style_generated(se, el, EMBI_PE_SCROLLBAR_CORNER);
    out->thumb = embi_style_generated(se, el, EMBI_PE_SCROLLBAR_THUMB);
    if (hover_axis != 0) {
        out->thumb_hover = embi_style_pseudo_hovered(se, el, EMBI_PE_SCROLLBAR_THUMB);
        out->hover_axis = out->thumb_hover != nullptr ? hover_axis : 0;
    }
    out->vbar_left = embi_sb_style_from((const embi_style *)el->style).vbar_left;
    return EMBI_SBPE_STYLED;
}

void embi_sb_pe_free(embi_style_engine *se, embi_sb_pe *pe)
{
    if (pe->thumb_hover != nullptr) {
        embi_style_pseudo_release(se, pe->thumb_hover);
    }
    memset(pe, 0, sizeof *pe);
}

bool embi_sb_geometry(const embi_frag *f, float x, float y, const embi_sb_pe *pe, embi_sb_geo *g)
{
    memset(g, 0, sizeof *g);
    if (f == nullptr || f->kind != EMBI_FK_BOX) {
        return false;
    }
    float ov[4];
    embi_frag_ov(f, ov);
    float right = f->w - embi_frag_bdr(f, 2) - embi_frag_scr(f, 0);
    float bottom = f->h - embi_frag_bdr(f, 3) - embi_frag_scr(f, 1);
    float max_x = fmaxf(0.0f, ov[2] - right), max_y = fmaxf(0.0f, ov[3] - bottom);
    if (ov[2] <= f->w - embi_frag_bdr(f, 2)) {
        max_x = 0.0f; /* as embi_scroll_max: the padding box's own gutter is not overflow */
    }
    float vt = pe->vert, ht = pe->horz;
    g->vert = vt > 0.0f && max_y > 0.0f;
    g->horz = ht > 0.0f && max_x > 0.0f;
    g->vmax = max_y;
    g->hmax = max_x;
    float bl = embi_frag_bdr(f, 0), bt = embi_frag_bdr(f, 1), br = embi_frag_bdr(f, 2), bb = embi_frag_bdr(f, 3);
    float pw = f->w - bl - br, ph = f->h - bt - bb;
    float vx = pe->vbar_left ? x + bl : x + f->w - br - vt;
    float hy = y + f->h - bb - ht;
    if (g->vert) {
        float len = fmaxf(0.0f, ph - (g->horz ? ht : 0.0f));
        g->v[0] = vx;
        g->v[1] = y + bt;
        g->v[2] = vt;
        g->v[3] = len;
        g->vtrack = len;
        float client = bottom - bt, content = ov[3] - bt;
        if (embi_sb_thumb(len, client, content, embi_frag_sy(f), &g->vstart, &g->vlen)) {
            g->vthumb[0] = vx;
            g->vthumb[1] = g->v[1] + g->vstart;
            g->vthumb[2] = vt;
            g->vthumb[3] = g->vlen;
        }
    }
    if (g->horz) {
        float len = fmaxf(0.0f, pw - (g->vert ? vt : 0.0f));
        float hx = x + bl + (pe->vbar_left && g->vert ? vt : 0.0f);
        g->h[0] = hx;
        g->h[1] = hy;
        g->h[2] = len;
        g->h[3] = ht;
        g->htrack = len;
        float client = right - bl, content = ov[2] - bl;
        if (embi_sb_thumb(len, client, content, embi_frag_sx(f), &g->hstart, &g->hlen)) {
            g->hthumb[0] = hx + g->hstart;
            g->hthumb[1] = hy;
            g->hthumb[2] = g->hlen;
            g->hthumb[3] = ht;
        }
    }
    if (g->vert && g->horz) {
        g->has_corner = true;
        g->corner[0] = vx;
        g->corner[1] = hy;
        g->corner[2] = vt;
        g->corner[3] = ht;
    }
    return g->vert || g->horz;
}

/* Reduces radii so that no side is shorter than the two radii on it (CSS Backgrounds 3 section 5.5). */
static void fit_radii(float w, float h, float r[4])
{
    float f = 1.0f;
    const float sides[4][2] = {{r[0], r[1]}, {r[1], r[2]}, {r[2], r[3]}, {r[3], r[0]}};
    const float lens[4] = {w, h, w, h};
    for (int i = 0; i < 4; i++) {
        float sum = sides[i][0] + sides[i][1];
        if (sum > lens[i] && sum > 0.0f && lens[i] / sum < f) {
            f = lens[i] / sum;
        }
    }
    for (int i = 0; i < 4; i++) {
        r[i] *= f;
    }
}

/* One part (a bar, its track, its thumb or the corner) with the style s in the rectangle r, cut by the visible rectangle clip
 * (x0, y0, x1, y1): the background color in the area of background-clip with the radii of that area, then the border. */
static uint32_t emit_part(embi_dl *d, const embi_style *s, const float r[4], const float clip[4])
{
    if (s == nullptr || !(r[2] > 0.0f) || !(r[3] > 0.0f) || embi_pb_hidden(s)) {
        return 0;
    }
    /* A part that the clip cuts is cut square: the clip is the visible window of a scroll container around it. */
    float x0 = fmaxf(r[0], clip[0]), y0 = fmaxf(r[1], clip[1]);
    float x1 = fminf(r[0] + r[2], clip[2]), y1 = fminf(r[1] + r[3], clip[3]);
    if (!(x1 > x0) || !(y1 > y0)) {
        return 0;
    }
    bool cut = x0 != r[0] || y0 != r[1] || x1 != r[0] + r[2] || y1 != r[1] + r[3];
    float bw[4];
    for (int i = 0; i < 4; i++) {
        bw[i] = embi_pb_part_border(s, i);
    }
    float rad[4];
    embi_pb_radii(nullptr, s, r[2], r[3], embi_pb_fontsize(s), rad);
    uint32_t n = 0;
    uint32_t bg = 0;
    if (embi_pb_color(s, embi_style_value(s, EMBI_PROP_background_color), &bg) && (bg >> 24) != 0) {
        uint16_t clp = embi_pb_kw(s, EMBI_PROP_background_clip);
        float area[4] = {r[0], r[1], r[2], r[3]};
        float arad[4] = {rad[0], rad[1], rad[2], rad[3]};
        if (clp == EMBI_KW_padding_box || clp == EMBI_KW_content_box) {
            area[0] += bw[0];
            area[1] += bw[1];
            area[2] -= bw[0] + bw[2];
            area[3] -= bw[1] + bw[3];
            embi_pb_inner_radii(rad, bw, arad);
            fit_radii(area[2], area[3], arad);
        }
        if (cut) {
            float cx0 = fmaxf(area[0], x0), cy0 = fmaxf(area[1], y0);
            float cx1 = fminf(area[0] + area[2], x1), cy1 = fminf(area[1] + area[3], y1);
            area[0] = cx0;
            area[1] = cy0;
            area[2] = cx1 - cx0;
            area[3] = cy1 - cy0;
            memset(arad, 0, sizeof arad);
        }
        if (area[2] > 0.0f && area[3] > 0.0f) {
            embi_ditem it;
            memset(&it, 0, sizeof it);
            it.kind = EMBI_DI_FILL;
            memcpy(it.rect, area, sizeof it.rect);
            memcpy(it.radii, arad, sizeof it.radii);
            it.colors[0] = bg;
            embi_vec_push(&d->items, it);
            n++;
        }
    }
    if (bw[0] > 0.0f || bw[1] > 0.0f || bw[2] > 0.0f || bw[3] > 0.0f) {
        static const embi_prop_id cp[4] = {EMBI_PROP_border_top_color, EMBI_PROP_border_right_color,
                                           EMBI_PROP_border_bottom_color, EMBI_PROP_border_left_color};
        static const embi_prop_id sp[4] = {EMBI_PROP_border_top_style, EMBI_PROP_border_right_style,
                                           EMBI_PROP_border_bottom_style, EMBI_PROP_border_left_style};
        static const int idx[4] = {1, 2, 3, 0}; /* the side order of an item: top, right, bottom, left */
        embi_ditem it;
        memset(&it, 0, sizeof it);
        it.kind = EMBI_DI_BORDER;
        bool any = false;
        for (int i = 0; i < 4; i++) {
            uint32_t c = 0xFF000000u;
            if (!embi_pb_color(s, embi_style_value(s, cp[i]), &c)) {
                c = 0xFF000000u;
            }
            it.colors[i] = c;
            uint16_t st = embi_pb_kw(s, sp[i]);
            it.styles[i] = st == EMBI_KW_dashed ? EMBI_DS_DASHED : (st == EMBI_KW_dotted ? EMBI_DS_DOTTED : EMBI_DS_SOLID);
            it.widths[i] = bw[idx[i]];
            any = any || (it.widths[i] > 0.0f && (c >> 24) != 0);
        }
        if (any && !cut) {
            memcpy(it.rect, r, sizeof it.rect);
            memcpy(it.radii, rad, sizeof it.radii);
            embi_vec_push(&d->items, it);
            n++;
        }
    }
    return n;
}

uint32_t embi_sb_emit_styled(embi_dl *d, const embi_frag *f, float x, float y, const embi_sb_pe *pe, const float clip[4])
{
    embi_sb_geo g;
    if (!embi_sb_geometry(f, x, y, pe, &g)) {
        return 0;
    }
    uint32_t n = 0;
    if (g.vert) {
        n += emit_part(d, pe->bar, g.v, clip);
        n += emit_part(d, pe->track, g.v, clip);
    }
    if (g.horz) {
        n += emit_part(d, pe->bar, g.h, clip);
        n += emit_part(d, pe->track, g.h, clip);
    }
    if (g.has_corner) {
        n += emit_part(d, pe->corner, g.corner, clip);
    }
    if (g.vert && g.vlen > 0.0f) {
        n += emit_part(d, pe->hover_axis == 1 ? pe->thumb_hover : pe->thumb, g.vthumb, clip);
    }
    if (g.horz && g.hlen > 0.0f) {
        n += emit_part(d, pe->hover_axis == 2 ? pe->thumb_hover : pe->thumb, g.hthumb, clip);
    }
    return n;
}
