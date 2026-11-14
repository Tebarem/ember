/*
 * Compositing groups of the display list (R3b): mix-blend-mode, isolation, clip-path and mask-image.
 *
 * A stacking context with a blend mode, a clip-path or a mask becomes an EMBI_LS_COMPOSITE group (dl.h) that carries its
 * filter steps, its opacity, the blend mode, the clip-path outline (dl->cpaths, dl->curves) and, as its first children, one
 * EMBI_LS_MASK group per mask layer. Replay renders the group into a layer, applies filter, clip-path, mask and opacity, and
 * blends the result onto the target.
 *
 * Isolation. A stacking context is an isolated group (Compositing and Blending 1 section 3.2): a blend mode inside it blends
 * with the context's own content, not with the page. A context pays for a layer only when a blend group is inside it, so the
 * paint walk counts the blend groups it emits (p->nblend) and the context wraps its items in an isolated group afterwards
 * (embi_pb_isolate_from) when the count grew. The same holds for a backdrop root with a backdrop-filter inside (p->nbackdrop):
 * the element of a will-change that names a property that makes a backdrop root, or one with a backdrop-filter itself.
 */
#include "paint/paint_int.h"

#include "layout/clip_path.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ the style */

/* True when the value v (an identifier or a list of them) names the property. */
static bool wc_names(const embi_value *v, const char *name)
{
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_IDENT) {
        embi_str n = embi_atom_str(v->ident);
        return n.len == strlen(name) && memcmp(n.ptr, name, n.len) == 0;
    }
    if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            if (wc_names(&v->list.items[i], name)) {
                return true;
            }
        }
    }
    return false;
}

static bool has_filter_list(const embi_style *s, embi_prop_id prop)
{
    const embi_value *v = embi_style_value(s, prop);
    return v != nullptr && v->kind == EMBI_V_FILTERS && v->filters.n > 0;
}

void embi_pb_comp_scan(const embi_style *s, embi_pcomp *c)
{
    memset(c, 0, sizeof *c);
    if (s == nullptr) {
        return;
    }
    const embi_value *mb = embi_style_value(s, EMBI_PROP_mix_blend_mode);
    if (mb != nullptr && mb->kind == EMBI_V_KEYWORD) {
        c->blend = embi_pb_blend_kw(mb->kw);
    }
    c->isolate = embi_pb_kw(s, EMBI_PROP_isolation) == EMBI_KW_isolate;
    const embi_value *cp = embi_style_value(s, EMBI_PROP_clip_path);
    c->clip_path = cp != nullptr && cp->kind == EMBI_V_SHAPE;
    c->clip_ref = cp != nullptr && cp->kind == EMBI_V_URL && cp->str.len > 1 && cp->str.ptr[0] == '#';
    const embi_value *mi = embi_style_value(s, EMBI_PROP_mask_image);
    if (mi != nullptr && !(mi->kind == EMBI_V_KEYWORD && mi->kw == EMBI_KW_none)) {
        c->mask = true;
    }
    const embi_value *wc = embi_style_value(s, EMBI_PROP_will_change);
    c->backdrop_root = has_filter_list(s, EMBI_PROP_backdrop_filter) || wc_names(wc, "opacity") ||
                       wc_names(wc, "filter") || wc_names(wc, "backdrop-filter") || wc_names(wc, "mix-blend-mode") ||
                       wc_names(wc, "clip-path") || wc_names(wc, "mask") || wc_names(wc, "mask-image");
}

/* ------------------------------------------------------------------ groups */

uint32_t embi_pb_group_begin(embi_pst *p, uint32_t blend, bool isolate, uint32_t xform, uint32_t clip)
{
    embi_dl *d = p->d;
    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = EMBI_LS_COMPOSITE;
    open.flags = isolate ? EMBI_DF_ISOLATE : 0;
    open.f[0] = 1.0f;
    open.colors[0] = blend;
    open.xform = xform;
    open.clip = clip;
    embi_vec_push(&d->items, open);
    return d->items.len - 1;
}

void embi_pb_group_end(embi_pst *p, uint32_t begin)
{
    embi_dl *d = p->d;
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = begin;
    embi_vec_push(&d->items, end);
    d->items.data[begin].aux = d->items.len - 1;
    if (d->items.len - begin - 2 == 0) {
        d->items.len = begin; /* an empty group */
        return;
    }
    d->fx_items++;
    if (d->items.data[begin].colors[0] != EMBI_BM_NORMAL) {
        p->nblend++;
    }
}

uint32_t embi_pb_mask_begin(embi_pst *p, uint32_t mode, uint32_t comp, uint32_t xform, uint32_t clip)
{
    embi_dl *d = p->d;
    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = EMBI_LS_MASK;
    open.f[0] = 1.0f;
    open.colors[0] = mode;
    open.colors[1] = comp;
    open.xform = xform;
    open.clip = clip;
    embi_vec_push(&d->items, open);
    return d->items.len - 1;
}

void embi_pb_mask_end(embi_pst *p, uint32_t begin)
{
    embi_dl *d = p->d;
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = begin;
    embi_vec_push(&d->items, end);
    d->items.data[begin].aux = d->items.len - 1;
}

/* ------------------------------------------------------------------ clip-path */

/* The reference box of a clip-path or mask: the geometry box keyword (0 is the border box) of a box with border box bb. */
static void ref_box(const embi_frag *f, const float bb[4], uint16_t kw, float out[4])
{
    embi_pb_area(f, bb, kw != 0 ? kw : EMBI_KW_border_box, out);
}

/* Appends the clip-path of the style to the list; returns false when it has none that can be resolved (a url() is a mask
 * layer, see embi_pb_comp_open). */
static bool clip_record(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t *idx)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_clip_path);
    if (v == nullptr || v->kind != EMBI_V_SHAPE) {
        return false;
    }
    embi_dl *d = p->d;
    float ref[4];
    ref_box(f, bb, v->kw, ref);
    embi_dcpath rec;
    memset(&rec, 0, sizeof rec);
    if (v->shape == nullptr) {
        /* A box alone: the edge of the box, with its rounded corners. */
        if (!(ref[2] > 0.0f) || !(ref[3] > 0.0f)) {
            rec.kind = EMBI_CP_EMPTY;
        } else {
            rec.kind = EMBI_CP_RRECT;
            memcpy(rec.rect, ref, sizeof rec.rect);
            embi_pb_area_radii(p, s, f, bb, v->kw != 0 ? v->kw : EMBI_KW_border_box, rec.radii);
            rec.bounds[0] = ref[0];
            rec.bounds[1] = ref[1];
            rec.bounds[2] = ref[0] + ref[2];
            rec.bounds[3] = ref[1] + ref[3];
        }
    } else {
        embi_lay_curves cv;
        memset(&cv, 0, sizeof cv);
        embi_lay_cp cp;
        if (!embi_lay_clip_shape(v->shape, ref, embi_pb_fontsize(s), p->vw, p->vh, p->rem, &cp, &cv)) {
            embi_vec_free(&cv);
            return false;
        }
        rec.kind = cp.kind;
        rec.evenodd = cp.evenodd;
        memcpy(rec.rect, cp.rect, sizeof rec.rect);
        memcpy(rec.radii, cp.radii, sizeof rec.radii);
        memcpy(rec.bounds, cp.bounds, sizeof rec.bounds);
        if (cp.kind == EMBI_LCP_PATH) {
            rec.first = d->curves.len;
            rec.n = cp.n;
            static_assert(sizeof(embi_lay_curve) == sizeof(embi_dcurve), "the curve layouts match");
            for (uint32_t i = 0; i < cp.n; i++) {
                embi_dcurve c;
                memcpy(&c, cv.data[cp.first + i].c, sizeof c);
                embi_vec_push(&d->curves, c);
            }
        }
        embi_vec_free(&cv);
    }
    *idx = d->cpaths.len;
    embi_vec_push(&d->cpaths, rec);
    return true;
}

/* ------------------------------------------------------------------ the group of a stacking context */

uint32_t embi_pb_comp_open(embi_pst *p, const embi_frag *f, const embi_style *s, const embi_pcomp *c, const float bb[4],
                           uint32_t fx_first, uint32_t fx_n, float opacity, uint32_t xform, uint32_t clip)
{
    embi_dl *d = p->d;
    uint32_t cidx = 0;
    bool has_clip = c->clip_path && clip_record(p, f, s, bb, &cidx);
    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = EMBI_LS_COMPOSITE;
    open.f[0] = opacity;
    open.aux2 = fx_first;
    open.aux3 = fx_n;
    open.xform = xform;
    open.clip = clip;
    open.colors[0] = c->blend;
    open.colors[1] = has_clip ? cidx + 1u : 0u;
    memcpy(open.rect, bb, sizeof open.rect);
    embi_vec_push(&d->items, open);
    uint32_t begin = d->items.len - 1;
    uint32_t nlayers = 0;
    const embi_box *fb = f != nullptr && f->kind == EMBI_FK_BOX ? f->box : nullptr;
    if (c->clip_ref && fb != nullptr && fb->el != nullptr && (fb->flags2 & EMBI_BF2_SVGFO) == 0) {
        /* R4e: clip-path: url(#id) names a clipPath of the document: its shapes are the top mask layer, which intersects
         * with the layers of mask-image (the SVG walk applies the clip-path of a foreignObject itself). */
        const embi_value *cp = embi_style_value(s, EMBI_PROP_clip_path);
        uint32_t mb = embi_pb_mask_begin(p, EMBI_MM_ALPHA, EMBI_MC_INTERSECT, xform, clip);
        embi_svg_env env;
        embi_pb_svg_env(p, &env);
        int r = embi_svg_ref_content(&env, d, fb->el->base.doc, cp->str, true, bb, xform, clip);
        if (r == EMBI_SVGFX_REF_OK) {
            embi_pb_mask_end(p, mb);
            nlayers++;
        } else {
            d->items.len = mb; /* a reference that names no clipPath clips nothing */
        }
    }
    if (c->mask) {
        nlayers += embi_pb_mask_layers(p, f, s, bb, xform, clip);
    }
    d->items.data[begin].colors[2] = nlayers;
    return begin;
}

void embi_pb_isolate_from(embi_pst *p, uint32_t at, uint32_t ctx_index)
{
    embi_dl *d = p->d;
    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = EMBI_LS_COMPOSITE;
    open.flags = EMBI_DF_ISOLATE;
    open.f[0] = 1.0f;
    uint32_t n = d->items.len;
    embi_vec_push(&d->items, open); /* room for one more */
    if (n > at) {
        memmove(&d->items.data[at + 1], &d->items.data[at], (size_t)(n - at) * sizeof(embi_ditem));
    }
    d->items.data[at] = open;
    for (uint32_t k = at + 1; k <= n; k++) {
        embi_ditem *it = &d->items.data[k];
        if (it->kind == EMBI_DI_LAYER_BEGIN || it->kind == EMBI_DI_LAYER_END) {
            it->aux++; /* the partner moved with it */
        }
    }
    for (uint32_t k = ctx_index + 1; k < d->ctxs.len; k++) {
        d->ctxs.data[k].first++;
        d->ctxs.data[k].end++;
    }
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = at;
    embi_vec_push(&d->items, end);
    d->items.data[at].aux = d->items.len - 1;
    d->fx_items++;
}
