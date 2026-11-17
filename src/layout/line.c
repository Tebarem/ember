/*
 * Line box metrics and the text style of a box (CSS 2.1 section 10.8, CSS Text 3, W3a section 5).
 * The inline formatting context (inline.c) places the pieces; this file answers what the pieces
 * need: the ascent and descent of an inline box's content area for its line-height, and the
 * text style of a computed style for shaping.
 *
 * Limits. ascent-override, descent-override and line-gap-override are not applied: the font registry
 * (embi_face_desc, style/fontface.c) keeps no field for them, so normal uses the face's own metrics
 * (a change to the registry is needed, outside W3c). The primary font is the first candidate face of
 * the style's font-family list. The shaping inputs also carry the lang attribute (embi_lay_lang, set
 * by the caller from the element), font-feature-settings, font-variation-settings, font-kerning and
 * font-variant-ligatures.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <stdatomic.h>
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

float embi_lay_font_px_style(const embi_style *s)
{
    const embi_value *fs = embi_style_value(s, EMBI_PROP_font_size);
    return fs != nullptr && fs->kind == EMBI_V_LENGTH && fs->unit == EMBI_U_PX ? (float)fs->num : 16.0f;
}

float embi_lay_spacing_px(const embi_value *v, float font_px)
{
    if (v == nullptr) {
        return 0.0f;
    }
    switch (v->kind) {
    case EMBI_V_LENGTH:
        if (v->unit == EMBI_U_PX) {
            return (float)v->num;
        }
        break;
    case EMBI_V_PERCENT:
        return font_px * (float)v->num / 100.0f;
    case EMBI_V_CALC:
        break;
    default:
        return 0.0f;
    }
    embi_len_ctx lc;
    memset(&lc, 0, sizeof lc);
    lc.font_px = font_px;
    lc.root_px = 16.0;
    lc.cq_w = NAN;
    lc.cq_h = NAN;
    if (v->kind == EMBI_V_LENGTH) {
        double r = embi_len_px_cb(&lc, v->unit, v->num);
        return isnan(r) ? 0.0f : (float)r;
    }
    embi_calc_env env;
    env.pct_base = (double)font_px;
    env.len_px = embi_len_px_cb;
    env.user = &lc;
    double r = embi_calc_eval(v->calc, &env);
    return isnan(r) ? 0.0f : (float)r;
}


/* The stretch keywords as percentages (CSS Fonts 4 section 2.2). */
static float stretch_kw(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_ultra_condensed:
        return 50.0f;
    case EMBI_KW_extra_condensed:
        return 62.5f;
    case EMBI_KW_condensed:
        return 75.0f;
    case EMBI_KW_semi_condensed:
        return 87.5f;
    case EMBI_KW_semi_expanded:
        return 112.5f;
    case EMBI_KW_expanded:
        return 125.0f;
    case EMBI_KW_extra_expanded:
        return 150.0f;
    case EMBI_KW_ultra_expanded:
        return 200.0f;
    default:
        return 100.0f;
    }
}

/* Values shared with the text module (text.h enums, which layout does not include). */
enum {
    LAY_STYLE_NORMAL = 0,
    LAY_STYLE_ITALIC = 1,
    LAY_STYLE_OBLIQUE = 2,
    LAY_DIR_LTR = 0,
    LAY_DIR_RTL = 1,
    LAY_DIR_AUTO = 2,
    LAY_KERN_AUTO = 0,
    LAY_KERN_NORMAL = 1,
    LAY_KERN_NONE = 2,
    LAY_LINE_BREAK_AUTO = 0,
};

/* The tag of a four-byte string value, packed big-endian. */
static uint32_t tag_of(embi_str s)
{
    return ((uint32_t)(uint8_t)s.ptr[0] << 24) | ((uint32_t)(uint8_t)s.ptr[1] << 16) |
           ((uint32_t)(uint8_t)s.ptr[2] << 8) | (uint32_t)(uint8_t)s.ptr[3];
}

/* The value of a feature entry [tag, value?]: the number, 1 when the value is missing (CSS Fonts 4:
 * a tag alone means on), and 0 for an off keyword. W2 already normalises the entry to a number; the
 * fallbacks keep an unnormalised list (a bare tag, on, off) meaning the same. */
static double feat_value(const embi_value *p, uint32_t m)
{
    if (m < 2) {
        return 1.0;
    }
    if (p[1].kind == EMBI_V_NUMBER || p[1].kind == EMBI_V_INTEGER) {
        return p[1].num;
    }
    if (p[1].kind == EMBI_V_IDENT) {
        embi_str k = embi_atom_str(p[1].ident);
        if (k.len == 3 && memcmp(k.ptr, "off", 3) == 0) {
            return 0.0;
        }
    }
    return 1.0;
}

/* Reads a font-feature-settings (variations false) or font-variation-settings (variations true)
 * value into out: a list of entries, each a list of a tag string and a number (a single entry may
 * be the list itself). Keywords such as normal read as no entries. */
static void read_feats(const embi_value *v, embi_lay_feat *out, uint32_t cap, uint32_t *n,
                       bool variations)
{
    *n = 0;
    if (v == nullptr || v->kind != EMBI_V_LIST) {
        return;
    }
    uint32_t cnt = 0;
    const embi_value *items = embi_value_items(v, &cnt);
    if (cnt >= 1 && items[0].kind == EMBI_V_STRING) {
        /* One entry, the list itself. */
        if (cnt < 1 || items[0].str.len != 4 || cap == 0) {
            return;
        }
        double val = feat_value(items, cnt);
        out[0].tag = tag_of(items[0].str);
        out[0].value = variations ? 0 : (int32_t)val;
        out[0].fvalue = variations ? (float)val : 0.0f;
        *n = 1;
        return;
    }
    for (uint32_t i = 0; i < cnt && *n < cap; i++) {
        uint32_t m = 0;
        const embi_value *p = embi_value_items(&items[i], &m);
        if (m < 1 || p[0].kind != EMBI_V_STRING || p[0].str.len != 4) {
            continue;
        }
        double val = feat_value(p, m);
        embi_lay_feat f;
        f.tag = tag_of(p[0].str);
        f.value = variations ? 0 : (int32_t)val;
        f.fvalue = variations ? (float)val : 0.0f;
        out[(*n)++] = f;
    }
}

static bool kw_is_none(const embi_style *s, embi_prop_id prop)
{
    const embi_value *v = embi_style_value(s, prop);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none;
}

void embi_lay_text_style(const embi_style *s, embi_lay_tstyle *ts, embi_str *fams, uint32_t cap)
{
    memset(ts, 0, sizeof *ts);
    const embi_value *ff = embi_style_value(s, EMBI_PROP_font_family);
    uint32_t n = 0;
    if (ff != nullptr) {
        const embi_value *items = embi_value_items(ff, &n);
        if (n > cap) {
            n = cap;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (items[i].kind == EMBI_V_IDENT) {
                fams[i] = embi_atom_str(items[i].ident);
            } else if (items[i].kind == EMBI_V_STRING) {
                fams[i] = items[i].str;
            } else {
                fams[i] = (embi_str){"", 0};
            }
        }
    }
    ts->families = fams;
    ts->family_count = n;

    const embi_value *fs = embi_style_value(s, EMBI_PROP_font_size);
    ts->size = fs != nullptr && fs->kind == EMBI_V_LENGTH && fs->unit == EMBI_U_PX ? (float)fs->num : 16.0f;

    const embi_value *fw = embi_style_value(s, EMBI_PROP_font_weight);
    ts->weight = 400.0f;
    if (fw != nullptr) {
        if (fw->kind == EMBI_V_NUMBER || fw->kind == EMBI_V_INTEGER) {
            ts->weight = (float)fw->num;
        } else if (fw->kind == EMBI_V_KEYWORD && fw->kw == EMBI_KW_bold) {
            ts->weight = 700.0f;
        }
    }
    ts->font_style = LAY_STYLE_NORMAL;
    const embi_value *fst = embi_style_value(s, EMBI_PROP_font_style);
    if (fst != nullptr && fst->kind == EMBI_V_KEYWORD) {
        if (fst->kw == EMBI_KW_italic) {
            ts->font_style = LAY_STYLE_ITALIC;
        } else if (fst->kw == EMBI_KW_oblique) {
            ts->font_style = LAY_STYLE_OBLIQUE;
        }
    } else if (fst != nullptr && fst->kind == EMBI_V_FUNC) {
        ts->font_style = LAY_STYLE_OBLIQUE;
    }
    ts->stretch = 100.0f;
    const embi_value *fsr = embi_style_value(s, EMBI_PROP_font_stretch);
    if (fsr != nullptr) {
        if (fsr->kind == EMBI_V_PERCENT) {
            ts->stretch = (float)fsr->num;
        } else if (fsr->kind == EMBI_V_KEYWORD) {
            ts->stretch = stretch_kw(fsr->kw);
        }
    }
    ts->letter_spacing = embi_lay_spacing_px(embi_style_value(s, EMBI_PROP_letter_spacing), ts->size);
    ts->word_spacing = embi_lay_spacing_px(embi_style_value(s, EMBI_PROP_word_spacing), ts->size);
    const embi_value *dir = embi_style_value(s, EMBI_PROP_direction);
    ts->direction = dir != nullptr && dir->kind == EMBI_V_KEYWORD && dir->kw == EMBI_KW_rtl ? LAY_DIR_RTL : LAY_DIR_LTR;
    {
        const embi_value *ub = embi_style_value(s, EMBI_PROP_unicode_bidi);
        if (ub != nullptr && ub->kind == EMBI_V_KEYWORD && (ub->kw == EMBI_KW_bidi_override || ub->kw == EMBI_KW_isolate_override)) {
            ts->direction |= 0x10; /* EMBI_DIR_OVERRIDE */
        } else if (ub != nullptr && ub->kind == EMBI_V_KEYWORD && ub->kw == EMBI_KW_plaintext) {
            ts->direction = LAY_DIR_AUTO; /* each paragraph takes its direction from its first strong character */
        }
    }
    ts->kerning = LAY_KERN_AUTO;
    const embi_value *kn = embi_style_value(s, EMBI_PROP_font_kerning);
    if (kn != nullptr && kn->kind == EMBI_V_KEYWORD) {
        if (kn->kw == EMBI_KW_normal) {
            ts->kerning = LAY_KERN_NORMAL;
        } else if (kn->kw == EMBI_KW_none) {
            ts->kerning = LAY_KERN_NONE;
        }
    }
    ts->line_break = LAY_LINE_BREAK_AUTO;
    /* font-variant-ligatures: none turns the standard ligatures off; its value may be a list. */
    ts->ligatures = true;
    const embi_value *lg = embi_style_value(s, EMBI_PROP_font_variant_ligatures);
    if (lg != nullptr) {
        uint32_t ln = 0;
        const embi_value *li = embi_value_items(lg, &ln);
        for (uint32_t i = 0; i < ln; i++) {
            if (li[i].kind == EMBI_V_KEYWORD && li[i].kw == EMBI_KW_none) {
                ts->ligatures = false;
            }
        }
    }
    read_feats(embi_style_value(s, EMBI_PROP_font_feature_settings), ts->feats,
               EMBI_LAY_MAX_FEATS, &ts->nfeats, false);
    read_feats(embi_style_value(s, EMBI_PROP_font_variation_settings), ts->vars,
               EMBI_LAY_MAX_VARS, &ts->nvars, true);
    /* font-optical-sizing: none leaves a variable font's opsz axis at its default. */
    ts->optical_sizing = !kw_is_none(s, EMBI_PROP_font_optical_sizing);
    ts->synth = 7;
    if (kw_is_none(s, EMBI_PROP_font_synthesis_weight)) {
        ts->synth &= ~1u;
    }
    if (kw_is_none(s, EMBI_PROP_font_synthesis_style)) {
        ts->synth &= ~2u;
    }
    if (kw_is_none(s, EMBI_PROP_font_synthesis_small_caps)) {
        ts->synth &= ~4u;
    }
    const embi_value *vc = embi_style_value(s, EMBI_PROP_font_variant_caps);
    if (vc != nullptr && vc->kind == EMBI_V_KEYWORD) {
        switch (vc->kw) {
        case EMBI_KW_small_caps:
        case EMBI_KW_petite_caps:
            ts->caps = 1;
            break;
        case EMBI_KW_all_small_caps:
        case EMBI_KW_all_petite_caps:
            ts->caps = 2;
            break;
        case EMBI_KW_unicase:
            ts->caps = 3;
            break;
        case EMBI_KW_titling_caps:
            ts->caps = 4;
            break;
        default:
            break;
        }
    }
}

bool embi_lay_lh_static(const embi_style *s)
{
    const embi_value *lh = embi_style_value(s, EMBI_PROP_line_height);
    if (lh == nullptr) {
        return true;
    }
    if (lh->kind == EMBI_V_CALC) {
        return false;
    }
    return lh->kind != EMBI_V_LENGTH || lh->unit == EMBI_U_PX;
}

void embi_lay_line_metrics(embi_lctx *c, const embi_style *s, float *a, float *d)
{
    embi_str fams[16];
    embi_lay_tstyle ts;
    embi_lay_text_style(s, &ts, fams, 16);
    float size = ts.size;
    float asc = 0.8f * size, desc = 0.2f * size, gap = 0;
    (void)embi_tshim_face_metrics(c->fonts, &ts, &asc, &desc, &gap);
    const embi_value *lh = embi_style_value(s, EMBI_PROP_line_height);
    bool normal = lh == nullptr || (lh->kind == EMBI_V_KEYWORD && lh->kw == EMBI_KW_normal);
    if (normal) {
        /* Chromium's SimpleFontData rounds the ascent, descent and line gap to whole pixels, and
         * the normal line height is their sum. The half-leading is half the rounded gap. */
        float ra = roundf(asc), rd = roundf(desc), rg = roundf(gap);
        *a = ra + rg / 2;
        *d = rd + rg / 2;
        return;
    }
    float line = size * 1.2f;
    if (lh->kind == EMBI_V_NUMBER || lh->kind == EMBI_V_INTEGER) {
        line = (float)lh->num * size;
    } else if (lh->kind == EMBI_V_LENGTH && lh->unit == EMBI_U_PX) {
        line = (float)lh->num;
    } else if (lh->kind == EMBI_V_PERCENT) {
        line = (float)lh->num * size / 100.0f;
    } else if (lh->kind == EMBI_V_LENGTH || lh->kind == EMBI_V_CALC) {
        /* A relative unit or calc(): resolved against the element's font (a percentage in calc()
         * refers to its font size too). */
        embi_len_ctx lc;
        memset(&lc, 0, sizeof lc);
        lc.font_px = size;
        lc.root_px = c->root_px;
        lc.vw = c->vw;
        lc.vh = c->vh;
        lc.cq_w = NAN;
        lc.cq_h = NAN;
        if (c->ncq > 0) {
            lc.cq_w = c->cq[c->ncq - 1].w;
            lc.cq_h = c->cq[c->ncq - 1].h;
        }
        double px;
        if (lh->kind == EMBI_V_LENGTH) {
            px = embi_len_px_cb(&lc, lh->unit, lh->num);
        } else {
            embi_calc_env env;
            env.pct_base = (double)size;
            env.len_px = embi_len_px_cb;
            env.user = &lc;
            px = embi_calc_eval(lh->calc, &env);
        }
        if (isfinite(px)) {
            line = (float)px;
        }
    }
    asc = roundf(asc);
    desc = roundf(desc);
    float half = (line - (asc + desc)) / 2;
    *a = asc + half;
    *d = desc + half;
}

/* The lang attribute of el itself by its atom (embi_el_get_attribute would look the name up in the atom table, a
 * lock and a hash, for every element of the page). */
static const embi_attr *lang_own(const embi_element *el)
{
    static _Atomic embi_atom lang_atom;
    embi_atom la = atomic_load_explicit(&lang_atom, memory_order_relaxed);
    if (la == 0) {
        la = embi_atom_intern("lang", 4);
        atomic_store_explicit(&lang_atom, la, memory_order_relaxed);
    }
    return embi_el_find_attr(el, EMBI_NS_NULL, 0, la);
}

/* The nearest lang attribute of el and its ancestors, as the attribute (1 when there is none), cached per
 * element for the layout: the lookups that reach here are the ancestors of the paragraphs. */
static uintptr_t lang_attr(embi_lctx *c, const embi_element *el)
{
    if (el == nullptr) {
        return 1;
    }
    uint64_t v = 0;
    if (embi_hashmap_get(&c->ld->langs, (uint64_t)(uintptr_t)el, &v)) {
        return (uintptr_t)v;
    }
    uintptr_t r;
    const embi_attr *a = lang_own(el);
    if (a != nullptr) {
        r = (uintptr_t)a;
    } else {
        const embi_node *up = el->base.parent;
        r = lang_attr(c, up != nullptr && embi_is_element(up) ? (const embi_element *)up : nullptr);
    }
    (void)embi_hashmap_put(&c->ld->langs, (uint64_t)(uintptr_t)el, (uint64_t)r);
    return r;
}

embi_str embi_lay_lang(embi_lctx *c, const embi_box *b)
{
    embi_str none = {"", 0};
    const embi_element *el = b != nullptr ? b->el : nullptr;
    uintptr_t r;
    if (el == nullptr) {
        return none;
    }
    /* The element's own attribute is one short scan; its ancestors are shared by its siblings, so they are
     * looked up in a front keyed by the parent (and in the map behind it). */
    const embi_attr *own = lang_own(el);
    if (own != nullptr) {
        r = (uintptr_t)own;
    } else {
        const embi_node *up = el->base.parent;
        const embi_element *pe = up != nullptr && embi_is_element(up) ? (const embi_element *)up : nullptr;
        if (pe == nullptr) {
            return none;
        }
        uint32_t slot = (uint32_t)(((uintptr_t)pe >> 4) * 0x9E3779B1u >> 24) & (EMBI_TKFRONT - 1u);
        if (c->ld->lfront[slot].key == pe) {
            r = c->ld->lfront[slot].v;
        } else {
            r = lang_attr(c, pe);
            c->ld->lfront[slot].key = pe;
            c->ld->lfront[slot].v = r;
        }
    }
    if (r == 1) {
        return none;
    }
    const embi_attr *a = (const embi_attr *)r;
    /* A lang value is ASCII in practice; a wide one counts as unknown. */
    return a->value.wide ? none : (embi_str){(const char *)a->value.ptr, a->value.len};
}

/* Appends the bytes of v to the serialized key. */
static void put_bytes(embi_sb *sb, const void *p, size_t n)
{
    embi_sb_append(sb, (embi_str){(const char *)p, (uint32_t)n});
}

/* Serializes every field of ts that the shaper reads (all but the language and the line-break mode,
 * which the cache entry keeps beside the blob). */
static void serialize_ts(embi_sb *sb, const embi_lay_tstyle *ts)
{
    put_bytes(sb, &ts->size, sizeof ts->size);
    put_bytes(sb, &ts->weight, sizeof ts->weight);
    put_bytes(sb, &ts->stretch, sizeof ts->stretch);
    put_bytes(sb, &ts->letter_spacing, sizeof ts->letter_spacing);
    put_bytes(sb, &ts->word_spacing, sizeof ts->word_spacing);
    uint8_t flags[7] = {ts->font_style, ts->direction, ts->kerning, (uint8_t)ts->ligatures,
                        (uint8_t)ts->optical_sizing, ts->synth, ts->caps};
    put_bytes(sb, flags, sizeof flags);
    put_bytes(sb, &ts->nfeats, sizeof ts->nfeats);
    put_bytes(sb, ts->feats, ts->nfeats * sizeof ts->feats[0]);
    put_bytes(sb, &ts->nvars, sizeof ts->nvars);
    put_bytes(sb, ts->vars, ts->nvars * sizeof ts->vars[0]);
    put_bytes(sb, &ts->family_count, sizeof ts->family_count);
    for (uint32_t i = 0; i < ts->family_count; i++) {
        uint32_t n = ts->families[i].len;
        put_bytes(sb, &n, sizeof n);
        put_bytes(sb, ts->families[i].ptr, n);
    }
}

static const embi_lay_tkey *tkey_of_impl(embi_lctx *c, const embi_style *s, bool ovr);
const embi_lay_tkey *embi_lay_tkey_of(embi_lctx *c, const embi_style *s)
{
    /* A direct-mapped front: a page has a few dozen styles and asks for one per text box. */
    embi_lay_doc *ld = c->ld;
    uint32_t slot = (uint32_t)((((uint64_t)(uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & (EMBI_TKFRONT - 1u) & ~1u;
    if (s != nullptr) {
        if (ld->tkfront[slot].key == s) {
            return ld->tkfront[slot].k;
        }
        if (ld->tkfront[slot + 1].key == s) {
            /* the second way: promote it, the way that held the first becomes the second */
            const embi_lay_tkey *r = ld->tkfront[slot + 1].k;
            ld->tkfront[slot + 1] = ld->tkfront[slot];
            ld->tkfront[slot].key = s;
            ld->tkfront[slot].k = r;
            return r;
        }
    }
    const embi_lay_tkey *r = tkey_of_impl(c, s, false);
    if (r != nullptr) {
        ld->tkfront[slot + 1] = ld->tkfront[slot];
        ld->tkfront[slot].key = s;
        ld->tkfront[slot].k = r;
    }
    return r;
}
static const embi_lay_tkey *tkey_of_impl(embi_lctx *c, const embi_style *s, bool ovr)
{
    uint64_t v = 0;
    uint64_t hk = (uint64_t)(uintptr_t)s + (ovr ? 1u : 0u); /* styles are 8-aligned: +1 is the override variant */
    if (embi_hashmap_get(&c->ld->tkeys, hk, &v)) {
        return (const embi_lay_tkey *)(uintptr_t)v;
    }
    embi_lay_tkey *k = embi_arena_alloc(&c->ld->ta, sizeof(embi_lay_tkey), alignof(embi_lay_tkey));
    if (k == nullptr) {
        return nullptr;
    }
    memset(k, 0, sizeof *k);
    embi_lay_text_style(s, &k->ts, k->fams, 16);
    k->ts.families = k->fams;
    if (ovr) {
        k->ts.direction |= 0x10; /* EMBI_DIR_OVERRIDE */
    }
    embi_sb sb = {nullptr, 0, 0};
    serialize_ts(&sb, &k->ts);
    uint8_t *blob = embi_arena_alloc(&c->ld->ta, sb.len > 0 ? sb.len : 1u, 1);
    if (blob == nullptr) {
        embi_sb_free(&sb);
        return nullptr;
    }
    if (sb.len > 0) {
        memcpy(blob, sb.data, sb.len);
    }
    k->blob = blob;
    k->blob_len = (uint32_t)sb.len;
    k->h = embi_hash_bytes(blob, sb.len, 0x9e3779b97f4a7c15ull);
    k->h2 = embi_hash_bytes(blob, sb.len, 0xc2b2ae3d27d4eb4full);
    k->amode = embi_lay_ifc_amode(s);
    embi_sb_free(&sb);
    (void)embi_hashmap_put(&c->ld->tkeys, hk, (uint64_t)(uintptr_t)k);
    return k;
}

static void tent_free(embi_lay_doc *ld, embi_lay_tent *e);

/* Equal bytes: eight at a time (the runs of a page are short, and a libc call costs more than the compare). */
static inline bool bytes_eq(const char *a, const char *b, uint32_t n)
{
    while (n >= 8) {
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y) {
            return false;
        }
        a += 8;
        b += 8;
        n -= 8;
    }
    while (n > 0) {
        if (*a != *b) {
            return false;
        }
        a++;
        b++;
        n--;
    }
    return true;
}

static bool tent_match(const embi_lay_tent *e, const embi_lay_tkey *k, embi_str lang, uint8_t lb,
                       const char *text, uint32_t len)
{
    const char *data = (const char *)(e + 1);
    return e->lb == lb && e->len == len && e->kh == k->h && e->kh2 == k->h2 && e->lang_len == lang.len &&
           bytes_eq(data + e->lang_len, text, len) && (lang.len == 0 || memcmp(data, lang.ptr, lang.len) == 0);
}

/* The index of the shaped-run cache: an open-addressed table of entries (ld->tmemo, linear probing, at most half
 * full) keyed by a cheap content mix, no hashing of the whole text. The slot is a function of the length, the
 * first, middle and last 8 bytes, the shaping style key and the mode; a candidate is verified with tent_match,
 * so the mix only has to spread. The table is rebuilt from the entries when the sweep frees some (no
 * tombstones). */
static uint32_t memo_slot(uint32_t n, uint64_t kh, uint8_t lb, const char *text, uint32_t len)
{
    uint64_t a = 0, b = 0, m = 0;
    if (len >= 8) {
        memcpy(&a, text, 8);
        memcpy(&b, text + len - 8, 8);
        memcpy(&m, text + (len >> 1) - (len >= 16 ? 4u : 0u), len >= 16 ? 8u : 0u);
    } else {
        memcpy(&a, text, len);
    }
    uint64_t h = (a ^ 0x9E3779B97F4A7C15ull) * 0xD6E8FEB86659FD93ull;
    h = (h ^ (h >> 32) ^ b) * 0xD6E8FEB86659FD93ull;
    h = (h ^ (h >> 32) ^ m ^ ((uint64_t)len << 40) ^ ((uint64_t)lb << 56)) * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 29) ^ kh) * 0xD6E8FEB86659FD93ull;
    return (uint32_t)(h >> 40) & (n - 1u);
}

/* Puts e in the table (which has room: grow_index made it). */
static void memo_put(embi_lay_doc *ld, embi_lay_tent *e)
{
    uint32_t mask = ld->tmemo_n - 1u;
    uint32_t i = memo_slot(ld->tmemo_n, e->kh, e->lb, (const char *)(e + 1) + e->lang_len, e->len);
    while (ld->tmemo[i] != nullptr) {
        i = (i + 1u) & mask;
    }
    ld->tmemo[i] = e;
}

/* Makes room for `entries` entries (the table stays at most half full): a bigger table is filled from ld->tents.
 * Returns false when out of memory. */
static bool memo_reserve(embi_lay_doc *ld, uint32_t entries)
{
    uint32_t want = ld->tmemo_n == 0 ? 1024u : ld->tmemo_n;
    while (want < entries * 2u) {
        want <<= 1;
    }
    if (want == ld->tmemo_n) {
        return true;
    }
    embi_lay_tent **m = embi_calloc(want, sizeof *m);
    if (m == nullptr) {
        return false;
    }
    embi_free(ld->tmemo, (size_t)ld->tmemo_n * sizeof *m);
    ld->tmemo = m;
    ld->tmemo_n = want;
    for (uint32_t i = 0; i < ld->tents.len; i++) {
        memo_put(ld, ld->tents.data[i]);
    }
    return true;
}

/* The entry for this key, nullptr when there is none. */
static embi_lay_tent *memo_find(embi_lay_doc *ld, const embi_lay_tkey *k, embi_str lang, uint8_t lb, const char *text,
                                uint32_t len)
{
    if (ld->tmemo_n == 0) {
        return nullptr;
    }
    uint32_t mask = ld->tmemo_n - 1u;
    uint32_t i = memo_slot(ld->tmemo_n, k->h, lb, text, len);
    for (;;) {
        embi_lay_tent *e = ld->tmemo[i];
        if (e == nullptr || tent_match(e, k, lang, lb, text, len)) {
            return e;
        }
        i = (i + 1u) & mask;
    }
}

embi_lay_text *embi_lay_shape_text_ent(embi_lctx *c, const embi_lay_tkey *k, embi_str lang, uint8_t lb,
                                       const char *text, uint32_t len, embi_lay_tent **ent)
{
    if (k == nullptr || text == nullptr || len == 0) {
        return nullptr;
    }
    if (lang.len > 255) {
        lang.len = 255;
    }
    embi_lay_doc *ld = c->ld;
    /* A paragraph's run (ld->shaping_para, set by the analysis) is only asked for its breaks and bidi levels:
     * a full run of the same text, style and mode serves (a paragraph of one text box finds its box's run),
     * else an analysis-only run, which is kept under its own key (bit 7 of the mode byte) so a text box never
     * gets a run without glyphs. */
    const bool para = ld->shaping_para;
    embi_lay_tent *hit = memo_find(ld, k, lang, lb, text, len);
    if (hit == nullptr && para) {
        lb |= 0x80u;
        hit = memo_find(ld, k, lang, lb, text, len);
    }
    if (hit != nullptr) {
        hit->gen = ld->tgen;
        if (ent != nullptr) {
            *ent = hit;
        }
        return hit->t;
    }
    embi_lay_tstyle ts = k->ts;
    ts.families = k->fams;
    ts.lang = lang;
    ts.line_break = (uint8_t)(lb & 0x0Fu);
    ts.join_ctx = (uint8_t)((lb >> 4) & 3u);
    ts.bidi_after = (lb & 0x40u) != 0;
    uint64_t st0 = SDL_GetPerformanceCounter();
    embi_lay_text *lt = para ? embi_tshim_analyze(c->text, text, len, &ts) : embi_tshim_shape(c->text, text, len, &ts);
    st0 = SDL_GetPerformanceCounter() - st0;
    if (ld->shaping_para) {
        ld->cold.para_runs++;
        ld->cold.para_ticks += st0;
    } else {
        ld->cold.box_runs++;
        ld->cold.box_ticks += st0;
    }
    if (lt == nullptr) {
        return nullptr;
    }
    size_t sz = sizeof(embi_lay_tent) + lang.len + len;
    embi_lay_tent *e = embi_lay_pool_alloc(&ld->pool, sz);
    embi_vec_reserve(&ld->tents, ld->tents.len + 1u);
    if (e == nullptr || ld->tents.len == ld->tents.cap) {
        embi_lay_pool_free(&ld->pool, e, sz);
        embi_tshim_free(lt);
        return nullptr;
    }
    /* The second half of the 128-bit content key (the analysis cache keys its paragraphs by it). */
    uint64_t h = embi_hash_bytes(text, len, k->h ^ ((uint64_t)lb << 56));
    h = embi_hash_bytes(lang.ptr, lang.len, h);
    uint64_t h2 = embi_hash_bytes(text, len, k->h2 ^ ((uint64_t)lb << 56));
    h2 = embi_hash_bytes(lang.ptr, lang.len, h2);
    embi_tshim_set_key(lt, h, h2);
    e->t = lt;
    e->h = h;
    e->h2 = h2;
    e->kh = k->h;
    e->kh2 = k->h2;
    e->gen = ld->tgen;
    e->len = len;
    e->lb = lb;
    e->lang_len = (uint8_t)lang.len;
    char *data = (char *)(e + 1);
    memcpy(data, lang.ptr, lang.len);
    memcpy(data + lang.len, text, len);
    if (!para && len < 4096 && embi_tshim_len(lt) == len) {
        /* inline.c tw: such a run's width is its shaped advance (no line feed, no soft hyphen, which start with 0xC2) */
        uint32_t i = 0;
        while (i < len && text[i] != 0x0A && (uint8_t)text[i] != 0xC2) {
            i++;
        }
        if (i == len) {
            e->clean = true;
            e->adv = embi_tshim_advance(lt);
        }
    }
    if (!memo_reserve(ld, ld->tents.len + 1u)) {
        tent_free(ld, e);
        return nullptr;
    }
    embi_vec_push(&ld->tents, e);
    memo_put(ld, e);
    if (ent != nullptr) {
        *ent = e;
    }
    return lt;
}

embi_lay_text *embi_lay_shape_text(embi_lctx *c, const embi_lay_tkey *k, embi_str lang, uint8_t lb,
                                   const char *text, uint32_t len)
{
    return embi_lay_shape_text_ent(c, k, lang, lb, text, len, nullptr);
}

static void tent_free(embi_lay_doc *ld, embi_lay_tent *e)
{
    embi_tshim_free(e->t);
    embi_lay_pool_free(&ld->pool, e, sizeof *e + e->lang_len + e->len);
}

/* Process-wide, so that a layout state made again for the same document never repeats the epoch of one that was destroyed. */
static _Atomic uint64_t g_run_epoch;

uint64_t embi_lay_next_run_epoch(void)
{
    return atomic_fetch_add_explicit(&g_run_epoch, 1, memory_order_relaxed) + 1;
}

void embi_lay_tcache_sweep(embi_lay_doc *ld, bool all)
{
    /* The analyses point at shaped runs: they go first, so a survivor's runs (kept alive by its generation) are never freed. */
    embi_lay_prep_sweep(ld, all);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < ld->tents.len; i++) {
        embi_lay_tent *e = ld->tents.data[i];
        if (!all && e->gen == ld->tgen) {
            ld->tents.data[keep++] = e;
        } else {
            tent_free(ld, e);
        }
    }
    if (keep != ld->tents.len) {
        /* Display lists name these runs (paint/text_paint.c): one built before this sweep must not be read again. */
        ld->run_epoch = embi_lay_next_run_epoch();
    }
    if (keep == ld->tents.len && !all) {
        return;
    }
    ld->tents.len = keep;
    /* The table points at freed entries: refill it from the survivors. */
    if (ld->tmemo_n != 0) {
        memset(ld->tmemo, 0, (size_t)ld->tmemo_n * sizeof *ld->tmemo);
        for (uint32_t i = 0; i < keep; i++) {
            memo_put(ld, ld->tents.data[i]);
        }
    }
}

embi_lay_text *embi_lay_shape_k(embi_lctx *c, embi_box *b, uint64_t *k1, uint64_t *k2)
{
    return embi_lay_shape_k_ovr(c, b, k1, k2, false);
}

/* Shaping across inline boxes (CSS Text 3 section 6.1): the text of a box whose neighbour in the line is a letter of a
 * cursive script, with no margin, border or padding between them and the same font, is shaped as if the two were one
 * run. join_in looks into a box (or past it) for the text next to the one being shaped. */
enum { JR_EMPTY, JR_STOP, JR_FOUND };

static bool edges_zero(const embi_lctx *c, const embi_box *b, bool loose)
{
    if (loose) {
        /* bidi context: margins, borders and padding do not matter, only boxes that open an embedding or isolate */
        const embi_value *lub = embi_lay_prop(c, b, EMBI_PROP_unicode_bidi);
        return lub == nullptr || lub->kind != EMBI_V_KEYWORD || lub->kw == EMBI_KW_normal;
    }
    static const embi_prop_id props[4] = {EMBI_PROP_margin_left, EMBI_PROP_margin_right, EMBI_PROP_padding_left,
                                          EMBI_PROP_padding_right};
    for (int i = 0; i < 4; i++) {
        const embi_value *v = embi_lay_prop(c, b, props[i]);
        if (v != nullptr && (v->kind == EMBI_V_PERCENT ? v->num != 0 : embi_lay_len(c, b, v, 0.0f) != 0.0f)) {
            return false;
        }
    }
    const embi_value *ub = embi_lay_prop(c, b, EMBI_PROP_unicode_bidi);
    if (ub != nullptr && ub->kind == EMBI_V_KEYWORD && ub->kw != EMBI_KW_normal) {
        return false; /* an isolate, embedding or override box breaks the shaping */
    }
    return embi_lay_border_side(c, b, 1) == 0.0f && embi_lay_border_side(c, b, 3) == 0.0f;
}

static int join_in(const embi_lctx *c, const embi_box *s, bool after, bool loose, const embi_box **out)
{
    if ((s->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) != 0) {
        return JR_EMPTY;
    }
    if (s->kind == EMBI_BK_TEXT) {
        if (s->text_len == 0) {
            return JR_EMPTY;
        }
        *out = s;
        return JR_FOUND;
    }
    if (s->kind != EMBI_BK_INLINE) {
        return JR_STOP;
    }
    if (!edges_zero(c, s, loose)) {
        return JR_STOP;
    }
    int res = JR_EMPTY;
    for (const embi_box *ch = s->first; ch != nullptr; ch = ch->next) {
        int r = join_in(c, ch, after, loose, out);
        if (r != JR_EMPTY) {
            res = r;
            if (after) {
                break;
            }
        }
    }
    return res;
}

static const embi_box *join_neighbour(const embi_lctx *c, const embi_box *b, bool after, bool loose)
{
    const embi_box *n = b;
    for (int guard = 0; guard < 64; guard++) {
        const embi_box *s = nullptr;
        if (after) {
            s = n->next;
        } else if (n->parent != nullptr) {
            for (const embi_box *ch = n->parent->first; ch != nullptr && ch != n; ch = ch->next) {
                s = ch;
            }
        }
        if (s == nullptr) {
            const embi_box *p = n->parent;
            if (p == nullptr || p->kind != EMBI_BK_INLINE || !edges_zero(c, p, loose)) {
                return nullptr;
            }
            n = p;
            continue;
        }
        const embi_box *found = nullptr;
        int r = join_in(c, s, after, loose, &found);
        if (r == JR_FOUND) {
            return found;
        }
        if (r == JR_STOP) {
            return nullptr;
        }
        n = s;
    }
    return nullptr;
}

/* The direction of the first strong character of text [p, e), approximated by the script: 1 for the right to left
 * scripts (Hebrew, Arabic, Syriac, Thaana, NKo and the presentation forms), 0 for other letters, -1 when only neutrals
 * and digits come first. */
static int first_strong_dir(const char *p, const char *e)
{
    while (p < e) {
        uint32_t cp = embi_utf8_decode(&p, e);
        if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||
            cp == 0x200F || (cp >= 0x10800 && cp <= 0x10FFF)) {
            return 1;
        }
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == 0x200E || (cp >= 0xC0 && cp < 0x0590 && cp != 0xD7 && cp != 0xF7)) {
            return 0;
        }
        if (cp >= 0x0900 && cp < 0xFB1D && !(cp >= 0x2000 && cp < 0x3000) && !(cp >= 0x3000 && cp <= 0x303F)) {
            return 0;
        }
        if (cp >= '0' && cp <= '9') {
            return -1;
        }
    }
    return -2; /* only neutrals: look in the next box */
}

/* Whether the text of box b ends in a space and a strong character of the other direction than the box's own comes next
 * in its paragraph (in another text box, across inline boxes): the space then has the level of the text before it
 * (UAX 9 N1), which the box's own analysis cannot see (bidi-box-model-014). */
static bool bidi_after_of(embi_lctx *c, const embi_box *b)
{
    if (b->kind != EMBI_BK_TEXT || b->parent == nullptr || b->text_len == 0 || b->text[b->text_len - 1] != ' ') {
        return false;
    }
    const embi_lay_tkey *k = embi_lay_tkey_of(c, b->style);
    if (k == nullptr || (k->ts.direction & 0x10) != 0 || (k->ts.direction & 0x0F) > LAY_DIR_RTL) {
        return false;
    }
    int base = (k->ts.direction & 0x0F) == LAY_DIR_RTL ? 1 : 0;
    const embi_box *n = b;
    for (int hops = 0; hops < 4; hops++) {
        n = join_neighbour(c, n, true, true);
        if (n == nullptr) {
            return false;
        }
        int d = first_strong_dir(n->text, n->text + n->text_len);
        if (d != -2) {
            return d >= 0 && d != base;
        }
    }
    return false;
}

/* The join_ctx bits of a text box (text_shim.h): bit 0 a cursive letter precedes it, bit 1 one follows it. */
static uint8_t join_ctx_of(embi_lctx *c, const embi_box *b)
{
    if (b->kind != EMBI_BK_TEXT || b->parent == nullptr || b->parent->kind != EMBI_BK_INLINE) {
        /* A text box directly in a block has no inline boundary to shape across, except next to an inline sibling. */
        if (b->parent == nullptr) {
            return 0;
        }
    }
    uint8_t first = (uint8_t)b->text[0], last = (uint8_t)b->text[b->text_len - 1];
    if (first < 0xA0 && last < 0xA0) {
        return 0; /* no cursive script is below U+0600 in UTF-8 (lead bytes 0xD8 and up) */
    }
    uint8_t bits = 0;
    const embi_lay_tkey *mk = embi_lay_tkey_of(c, b->style);
    for (int dir = 0; dir < 2; dir++) {
        const embi_box *nb = join_neighbour(c, b, dir == 1, false);
        if (nb == nullptr || nb->text_len == 0) {
            continue;
        }
        const embi_lay_tkey *nk = embi_lay_tkey_of(c, nb->style);
        bool same_font = nk != nullptr && mk != nullptr && nk->h == mk->h && nk->h2 == mk->h2;
        const char *mine = dir == 1 ? b->text + b->text_len - 1 : b->text;
        const char *theirs = dir == 1 ? nb->text : nb->text + nb->text_len - 1;
        if (dir == 1) {
            while (mine > b->text && (((uint8_t)*mine) & 0xC0) == 0x80) {
                mine--;
            }
        } else {
            theirs = nb->text + nb->text_len - 1;
            while (theirs > nb->text && (((uint8_t)*theirs) & 0xC0) == 0x80) {
                theirs--;
            }
        }
        const char *p1 = mine, *p2 = theirs;
        uint32_t cp1 = embi_utf8_decode(&p1, b->text + b->text_len);
        uint32_t cp2 = embi_utf8_decode(&p2, nb->text + nb->text_len);
        /* A join-causing ZWJ or tatweel joins whatever font it is in; other letters only within the same font. */
        bool causing = cp2 == 0x200Du || cp2 == 0x0640u;
        if (embi_tshim_cursive(cp1) && ((same_font && embi_tshim_cursive(cp2)) || causing)) {
            bits |= (uint8_t)(dir == 1 ? 2u : 1u);
        }
    }
    return bits;
}

embi_lay_text *embi_lay_shape_k_ovr(embi_lctx *c, embi_box *b, uint64_t *k1, uint64_t *k2, bool ovr)
{
    if (b->lt != nullptr) {
        return b->lt; /* the key stays unset: reading it would touch the run (the caller asks embi_tshim_key when it needs it) */
    }
    if (b->text == nullptr || b->text_len == 0) {
        return nullptr;
    }
    embi_lay_tent *e = nullptr;
    uint8_t jc = join_ctx_of(c, b);
    b->lt = embi_lay_shape_text_ent(c, ovr ? tkey_of_impl(c, b->style, true) : embi_lay_tkey_of(c, b->style), embi_lay_lang(c, b),
                                    (uint8_t)((jc << 4) | (bidi_after_of(c, b) ? 0x40u : 0u)), b->text, b->text_len, &e);
    if (e != nullptr) {
        *k1 = e->h;
        *k2 = e->h2;
        if (e->clean && e->adv > 0.0f && e->len == b->text_len) {
            b->adv = e->adv; /* what tw would measure: skips the second look at the text and the run */
        }
    }
    return b->lt;
}

embi_lay_text *embi_lay_shape(embi_lctx *c, embi_box *b)
{
    if (b->lt != nullptr) {
        return b->lt;
    }
    if (b->text == nullptr || b->text_len == 0) {
        return nullptr;
    }
    b->lt = embi_lay_shape_text(c, embi_lay_tkey_of(c, b->style), embi_lay_lang(c, b), 0, b->text, b->text_len);
    return b->lt;
}
