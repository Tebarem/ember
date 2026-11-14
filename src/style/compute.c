/*
 * Computed values (W2d, CSS Cascade 5 section 7 and the per-property rules of CSS Values 4,
 * Fonts 4, Display 3 and Color 4 that the W2 spec lists). A winning declaration is turned into the
 * property's computed value here: CSS-wide keywords, var() substitution, relative units, keyword
 * resolution, system and light-dark colors, and the blockification and border-style fixups.
 */
#include "style_int.h"
#include "style/fontmetric.h"

#include <string.h>

/* Properties whose keyword values are lengths (thin/medium/thick). Physical ids only, because
 * logical properties map to these before computing. */
static bool is_border_width(uint16_t p)
{
    switch (p) {
    case EMBI_PROP_border_top_width:
    case EMBI_PROP_border_right_width:
    case EMBI_PROP_border_bottom_width:
    case EMBI_PROP_border_left_width:
    case EMBI_PROP_outline_width:
    case EMBI_PROP_column_rule_width:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ source values */

const embi_value *embi_style_initial_value(const embi_sctx *sc, uint16_t p)
{
    /* The initial style stores 0px widths (its styles are none), but a width that falls back to its
     * initial value (`border-width: initial`, or a style set without a width) is medium. */
    if (sc->init != nullptr && sc->eng->w_med != nullptr &&
        (p == EMBI_PROP_border_top_width || p == EMBI_PROP_border_right_width ||
         p == EMBI_PROP_border_bottom_width || p == EMBI_PROP_border_left_width ||
         p == EMBI_PROP_outline_width)) {
        return sc->eng->w_med;
    }
    uint16_t g = embi_layout_get()->gid[p];
    if (sc->init != nullptr && g != EMBI_STYLE_NO_GROUP) {
        return sc->init->groups[g]->vals[embi_layout_get()->slot[p]];
    }
    return sc->eng->initial_raw[p];
}

const embi_value *embi_style_parent_value(const embi_sctx *sc, uint16_t p)
{
    uint16_t g = embi_layout_get()->gid[p];
    if (sc->parent != nullptr && g != EMBI_STYLE_NO_GROUP) {
        return sc->parent->groups[g]->vals[embi_layout_get()->slot[p]];
    }
    return embi_style_initial_value(sc, p);
}

/* The value a property has when nothing in the element overrides it: inherited from the parent
 * for inherited properties, the initial value otherwise. */
static const embi_value *src_value(const embi_sctx *sc, uint16_t p)
{
    const embi_prop_info *pi = embi_prop_info_of(p);
    if (pi != nullptr && (pi->flags & EMBI_PF_INHERITED) != 0) {
        return embi_style_parent_value(sc, p);
    }
    return embi_style_initial_value(sc, p);
}

/* The value of p as computed so far for this element. */
static const embi_value *elem_get(const embi_sctx *sc, uint16_t p)
{
    if (sc->ovr_gen[p] == sc->gen) {
        return sc->ovr[p];
    }
    return src_value(sc, p);
}

void embi_style_set_ovr(embi_sctx *sc, uint16_t p, const embi_value *v)
{
    uint16_t g = embi_layout_get()->gid[p];
    EMBI_ASSERT(g != EMBI_STYLE_NO_GROUP);
    sc->ovr[p] = v;
    sc->ovr_gen[p] = sc->gen;
    sc->dirty |= 1ull << g;
}

bool embi_style_touched(const embi_sctx *sc, uint16_t p)
{
    return sc->ovr_gen[p] == sc->gen;
}

const embi_value *embi_style_elem_value(const embi_sctx *sc, uint16_t p)
{
    return elem_get(sc, p);
}

/* ------------------------------------------------------------------ lengths */

static double font_px_of(const embi_value *v)
{
    if (v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return v->num;
    }
    return 16.0;
}

/* The parent's computed font size in px. Read only by the values that need it (font-size and the
 * em-relative ones), so other properties do not pay for the lookup. */
static double parent_font_px(const embi_sctx *sc)
{
    return font_px_of(embi_style_parent_value(sc, EMBI_PROP_font_size));
}

/* ex, cap, ch and ic of the element's font (the parent's for font-size itself), measured from its first face. */
static void unit_metrics(const embi_sctx *sc, bool parent_font, double font_px, embi_font_metrics *fm,
                         double *normal_lh)
{
    if (normal_lh != nullptr) {
        *normal_lh = 0.0; /* the face's normal line height at font_px (0: unknown) */
    }
    const embi_value *ff = parent_font ? embi_style_parent_value(sc, EMBI_PROP_font_family)
                                       : elem_get(sc, EMBI_PROP_font_family);
    if (ff == nullptr) {
        return;
    }
    embi_str fams[16];
    uint32_t n = 0;
    const embi_value *items = embi_value_items(ff, &n);
    if (n > 16) {
        n = 16;
    }
    for (uint32_t i = 0; i < n; i++) {
        fams[i] = items[i].kind == EMBI_V_IDENT    ? embi_atom_str(items[i].ident)
                  : items[i].kind == EMBI_V_STRING ? items[i].str
                                                   : (embi_str){"", 0};
    }
    const embi_value *w = elem_get(sc, EMBI_PROP_font_weight);
    const embi_value *st = elem_get(sc, EMBI_PROP_font_style);
    const embi_value *sr = elem_get(sc, EMBI_PROP_font_stretch);
    float weight = w != nullptr && w->kind == EMBI_V_NUMBER ? (float)w->num : 400.0f;
    uint8_t style = st != nullptr && st->kind == EMBI_V_KEYWORD && st->kw == EMBI_KW_italic ? 1 : 0;
    float stretch = sr != nullptr && (sr->kind == EMBI_V_PERCENT || sr->kind == EMBI_V_NUMBER) ? (float)sr->num : 100.0f;
    double m[5];
    if (embi_style_unit_metrics(sc->eng->fonts, fams, n, weight, style, stretch, font_px, m)) {
        fm->ex = m[0];
        fm->cap = m[1];
        fm->ch = m[2];
        fm->ic = m[3];
        if (normal_lh != nullptr) {
            *normal_lh = m[4];
        }
    }
}

/* The absolute computed line-height in px of the element (parent == false) or of its parent, for the lh unit. For
 * the line-height and font-size properties the unit refers to the parent's line-height (CSS Values 4 section 6.1.2). */
static double lh_px_of(const embi_sctx *sc, bool parent, double font_px)
{
    const embi_value *lv;
    double fpx;
    embi_font_metrics tmp;
    if (parent) {
        lv = embi_style_parent_value(sc, EMBI_PROP_line_height);
        fpx = parent_font_px(sc);
    } else {
        embi_style_compute_target((embi_sctx *)sc, EMBI_PROP_line_height);
        lv = elem_get(sc, EMBI_PROP_line_height);
        fpx = font_px;
    }
    if (lv != nullptr && (lv->kind == EMBI_V_NUMBER || lv->kind == EMBI_V_INTEGER)) {
        return lv->num * fpx;
    }
    if (lv != nullptr && lv->kind == EMBI_V_LENGTH && lv->unit == EMBI_U_PX) {
        return lv->num;
    }
    if (lv != nullptr && lv->kind == EMBI_V_PERCENT) {
        return lv->num * fpx / 100.0;
    }
    memset(&tmp, 0, sizeof tmp);
    double normal = 0.0;
    unit_metrics(sc, parent, fpx, &tmp, &normal);
    return normal > 0.0 ? normal : 0.0;
}

/* The user data of calc_len_px: the length context, filled with the font's ex, ch, cap and ic when a calc() first
 * uses one of those units. */
typedef struct calc_len_user {
    embi_len_ctx *lc;
    const embi_sctx *sc;
    bool parent_font;
    bool allow_cq; /* font-size: cq units use the viewport fallback */
    bool have;
    bool lh_parent; /* lh refers to the parent's line-height (the font-size and line-height properties) */
} calc_len_user;

static double len_to_px(const embi_sctx *sc, uint8_t unit, double num, double font_px,
                        double root_px, bool parent_font, bool lh_parent);

static double calc_len_px(void *user, uint8_t unit, double value)
{
    calc_len_user *u = user;
    if (!u->have && (unit == EMBI_U_EX || unit == EMBI_U_CH || unit == EMBI_U_CAP || unit == EMBI_U_IC)) {
        unit_metrics(u->sc, u->parent_font, u->lc->font_px, &u->lc->font_metrics, nullptr);
        u->have = true;
    }
    if (unit == EMBI_U_LH) {
        u->lc->font_metrics.lh = lh_px_of(u->sc, u->lh_parent, u->lc->font_px);
    }
    if (!u->allow_cq && (unit == EMBI_U_CQW || unit == EMBI_U_CQH || unit == EMBI_U_CQI || unit == EMBI_U_CQB ||
                         unit == EMBI_U_CQMIN || unit == EMBI_U_CQMAX)) {
        return __builtin_nan("");
    }
    return embi_len_px_cb(u->lc, unit, value);
}

static double len_to_px(const embi_sctx *sc, uint8_t unit, double num, double font_px,
                        double root_px, bool parent_font, bool lh_parent)
{
    const embi_style_env *env = &sc->eng->env;
    embi_len_ctx lc = {
        .font_px = font_px,
        .root_px = root_px,
        .vw = env->width,
        .vh = env->height,
        .cq_w = __builtin_nan(""),
        .cq_h = __builtin_nan(""),
    };
    if (unit == EMBI_U_EX || unit == EMBI_U_CH || unit == EMBI_U_CAP || unit == EMBI_U_IC) {
        unit_metrics(sc, parent_font, font_px, &lc.font_metrics, nullptr);
    }
    if (unit == EMBI_U_LH) {
        lc.font_metrics.lh = lh_px_of(sc, lh_parent, font_px);
    }
    double r = embi_len_px_cb(&lc, unit, num);
    return r != r ? num : r; /* NaN: keep the number */
}

/* Container query units depend on the nearest container's size, which only layout knows, so a
 * computed value keeps them and layout resolves them (embi_lay_len). font-size is the exception:
 * a computed font size is always px, so its cq units use the viewport fallback. */
static bool is_cq_unit(uint8_t unit)
{
    return unit == EMBI_U_CQW || unit == EMBI_U_CQH || unit == EMBI_U_CQI || unit == EMBI_U_CQB ||
           unit == EMBI_U_CQMIN || unit == EMBI_U_CQMAX;
}

/* ------------------------------------------------------------------ keyword tables */

static double font_size_keyword(uint16_t kw, double parent_px, bool *ok)
{
    *ok = true;
    switch (kw) {
    case EMBI_KW_xx_small: return 9.0;
    case EMBI_KW_x_small: return 10.0;
    case EMBI_KW_small: return 13.0;
    case EMBI_KW_medium: return 16.0;
    case EMBI_KW_math: return 16.0;
    case EMBI_KW_large: return 18.0;
    case EMBI_KW_x_large: return 24.0;
    case EMBI_KW_xx_large: return 32.0;
    case EMBI_KW_xxx_large: return 48.0;
    case EMBI_KW_larger: return parent_px * 1.2;
    case EMBI_KW_smaller: return parent_px / 1.2;
    default:
        *ok = false;
        return 0;
    }
}

static double bolder(double p)
{
    if (p < 400) {
        return 400;
    }
    if (p < 600) {
        return 700;
    }
    return 900;
}

static double lighter(double p)
{
    if (p < 600) {
        return 100;
    }
    if (p < 800) {
        return 400;
    }
    return 700;
}

static double font_stretch_keyword(uint16_t kw, bool *ok)
{
    *ok = true;
    const char *s = embi_kw_name(kw);
    if (s == nullptr) {
        *ok = false;
        return 100;
    }
    struct {
        const char *name;
        double pct;
    } tab[] = {
        {"ultra-condensed", 50}, {"extra-condensed", 62.5}, {"condensed", 75},
        {"semi-condensed", 87.5}, {"normal", 100}, {"semi-expanded", 112.5},
        {"expanded", 125}, {"extra-expanded", 150}, {"ultra-expanded", 200},
    };
    for (uint32_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strcmp(s, tab[i].name) == 0) {
            return tab[i].pct;
        }
    }
    *ok = false;
    return 100;
}

static uint16_t blockify(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_inline:
    case EMBI_KW_inline_block:
        return EMBI_KW_block;
    case EMBI_KW_inline_flex:
        return EMBI_KW_flex;
    case EMBI_KW_inline_grid:
        return EMBI_KW_grid;
    case EMBI_KW_inline_table:
        return EMBI_KW_table;
    case EMBI_KW__internal_inline_list_item:
        return EMBI_KW_list_item;
    case EMBI_KW__internal_inline_flow_root_list_item:
        return EMBI_KW__internal_flow_root_list_item;
    case EMBI_KW_table_row_group:
    case EMBI_KW_table_header_group:
    case EMBI_KW_table_footer_group:
    case EMBI_KW_table_row:
    case EMBI_KW_table_cell:
    case EMBI_KW_table_column_group:
    case EMBI_KW_table_column:
    case EMBI_KW_table_caption:
        return EMBI_KW_block;
    default:
        return kw;
    }
}

/* ------------------------------------------------------------------ finalize */

static const embi_value *finalize(embi_sctx *sc, uint16_t p, const embi_value *v);

const embi_value *embi_style_finalize_value(embi_sctx *sc, uint16_t p, const embi_value *v)
{
    return finalize(sc, p, v);
}

/* rem inside the root's own font-size resolves against the initial size (16px). */
static double root_px_for(const embi_sctx *sc, uint16_t p)
{
    return (p == EMBI_PROP_font_size && sc->is_root) ? 16.0 : sc->root_px;
}

/* Finalizes the members of an array of values. The array is copied into the arena only when a
 * member changes; otherwise src is returned. */
static const embi_value *fix_items(embi_sctx *sc, uint16_t p, const embi_value *src, uint32_t n,
                                   bool *changed)
{
    embi_value *dst = nullptr;
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *f = finalize(sc, p, &src[i]);
        if (f == &src[i]) {
            continue;
        }
        if (dst == nullptr) {
            dst = embi_arena_alloc(&sc->eng->arena, sizeof(embi_value) * n, 8);
            memcpy(dst, src, sizeof(embi_value) * n);
        }
        dst[i] = *f;
        *changed = true;
    }
    return dst != nullptr ? dst : src;
}

/* Finalizes one member in place (a copy). Returns true when it changed. */
static bool fix_member(embi_sctx *sc, uint16_t p, embi_value *x)
{
    const embi_value *f = finalize(sc, p, x);
    if (f == x) {
        return false;
    }
    *x = *f;
    return true;
}

/* Relative units inside compound values (lists, function arguments, positions, shadows,
 * transforms, filters) resolve like top-level lengths (CSS Values 4 section 6). o is the copy of
 * v being built; it is changed in place when a member changes. */
static void finalize_containers(embi_sctx *sc, uint16_t p, const embi_value *v, embi_value *o,
                                bool *changed)
{
    embi_style_engine *e = sc->eng;
    switch (v->kind) {
    case EMBI_V_LIST: {
        bool c = false;
        const embi_value *items = fix_items(sc, p, v->list.items, v->list.n, &c);
        if (c) {
            o->list.items = items;
            *changed = true;
        }
        break;
    }
    case EMBI_V_FUNC: {
        bool c = false;
        const embi_value *args = fix_items(sc, p, v->func.args, v->func.n, &c);
        if (c) {
            o->func.args = args;
            *changed = true;
        }
        break;
    }
    case EMBI_V_POS_AXIS: {
        embi_value off = v->axis->off;
        if (fix_member(sc, p, &off)) {
            embi_pos_axis *na = embi_arena_alloc(&e->arena, sizeof *na, 8);
            *na = *v->axis;
            na->off = off;
            o->axis = na;
            *changed = true;
        }
        break;
    }
    case EMBI_V_POSITION: {
        embi_value xo = v->pos->x.off;
        embi_value yo = v->pos->y.off;
        embi_value zo = v->pos->z;
        bool cx = fix_member(sc, p, &xo);
        bool cy = fix_member(sc, p, &yo);
        bool cz = zo.kind != EMBI_V_NONE && fix_member(sc, p, &zo);
        if (cx || cy || cz) {
            embi_position *np = embi_arena_alloc(&e->arena, sizeof *np, 8);
            *np = *v->pos;
            np->x.off = xo;
            np->y.off = yo;
            np->z = zo;
            o->pos = np;
            *changed = true;
        }
        break;
    }
    case EMBI_V_SHADOWS: {
        embi_shadow *na = nullptr;
        for (uint32_t i = 0; i < v->shadows.n; i++) {
            embi_shadow sh = v->shadows.p[i];
            bool c = fix_member(sc, p, &sh.x);
            c |= fix_member(sc, p, &sh.y);
            c |= fix_member(sc, p, &sh.blur);
            c |= fix_member(sc, p, &sh.spread);
            if (c && na == nullptr) {
                na = embi_arena_alloc(&e->arena, sizeof(embi_shadow) * v->shadows.n, 8);
                memcpy(na, v->shadows.p, sizeof(embi_shadow) * v->shadows.n);
            }
            if (c) {
                na[i] = sh;
            }
        }
        if (na != nullptr) {
            o->shadows.p = na;
            *changed = true;
        }
        break;
    }
    case EMBI_V_TRANSFORM: {
        embi_xform *nx = nullptr;
        for (uint32_t i = 0; i < v->xf.n; i++) {
            bool c = false;
            const embi_value *args = fix_items(sc, p, v->xf.p[i].args, v->xf.p[i].n, &c);
            if (c) {
                if (nx == nullptr) {
                    nx = embi_arena_alloc(&e->arena, sizeof(embi_xform) * v->xf.n, 8);
                    memcpy(nx, v->xf.p, sizeof(embi_xform) * v->xf.n);
                }
                nx[i].args = args;
            }
        }
        if (nx != nullptr) {
            o->xf.p = nx;
            *changed = true;
        }
        break;
    }
    case EMBI_V_FILTERS: {
        embi_filter *nf = nullptr;
        for (uint32_t i = 0; i < v->filters.n; i++) {
            embi_filter fl = v->filters.p[i];
            bool c = fix_member(sc, p, &fl.arg);
            if (c && nf == nullptr) {
                nf = embi_arena_alloc(&e->arena, sizeof(embi_filter) * v->filters.n, 8);
                memcpy(nf, v->filters.p, sizeof(embi_filter) * v->filters.n);
            }
            if (c) {
                nf[i] = fl;
            }
        }
        if (nf != nullptr) {
            o->filters.p = nf;
            *changed = true;
        }
        break;
    }
    default:
        break;
    }
}

static const embi_value *finalize(embi_sctx *sc, uint16_t p, const embi_value *v)
{
    embi_style_engine *e = sc->eng;
    embi_value o = *v;
    bool changed = false;
    double root_px = root_px_for(sc, p);

    finalize_containers(sc, p, v, &o, &changed);

    switch (v->kind) {

    case EMBI_V_COLOR:
        if (v->color.space == EMBI_CS_SYSTEM) {
            embi_color c;
            const embi_color_rgb8 *pal = sc->dark ? e->env.palette_dark : e->env.palette_light;
            if (embi_color_system_value(v->color.sys, sc->dark, pal, &c)) {
                o.color = c;
                changed = true;
            }
        } else if (v->color.space == EMBI_CS_CURRENT && p == EMBI_PROP_color) {
            return embi_style_parent_value(sc, p);
        } else if (v->color.space == EMBI_CS_DEFERRED && p == EMBI_PROP_color) {
            /* color: the relative color or mix reads the inherited color as currentcolor. */
            const embi_value *pv = embi_style_parent_value(sc, p);
            embi_color c;
            if (pv != nullptr && pv->kind == EMBI_V_COLOR &&
                embi_color_resolve_deferred(&v->color, &pv->color, &c)) {
                o.color = c;
                changed = true;
            }
        }
        break;
    case EMBI_V_LIGHT_DARK:
        return finalize(sc, p, sc->dark ? &v->pair[1] : &v->pair[0]);
    case EMBI_V_LENGTH:
        if (v->unit != EMBI_U_PX && !(is_cq_unit(v->unit) && p != EMBI_PROP_font_size)) {
            double fpx = p == EMBI_PROP_font_size
                             ? parent_font_px(sc)
                             : font_px_of(elem_get(sc, EMBI_PROP_font_size));
            o.num = len_to_px(sc, v->unit, v->num, fpx, root_px, p == EMBI_PROP_font_size,
                              p == EMBI_PROP_font_size || p == EMBI_PROP_line_height);
            o.unit = EMBI_U_PX;
            changed = true;
        }
        break;
    case EMBI_V_PERCENT:
        if (p == EMBI_PROP_font_size) {
            o.kind = EMBI_V_LENGTH;
            o.unit = EMBI_U_PX;
            o.num = v->num / 100.0 * parent_font_px(sc);
            changed = true;
        } else if (p == EMBI_PROP_line_height) {
            o.kind = EMBI_V_LENGTH;
            o.unit = EMBI_U_PX;
            o.num = v->num / 100.0 * font_px_of(elem_get(sc, EMBI_PROP_font_size));
            changed = true;
        }
        break;
    case EMBI_V_KEYWORD:
        if (p == EMBI_PROP_font_size) {
            bool ok;
            double px = font_size_keyword(v->kw, parent_font_px(sc), &ok);
            if (ok) {
                o.kind = EMBI_V_LENGTH;
                o.unit = EMBI_U_PX;
                o.num = px;
                changed = true;
            }
        } else if (p == EMBI_PROP_font_weight) {
            double w = -1;
            const embi_value *pw = embi_style_parent_value(sc, p);
            double parent_w = pw->kind == EMBI_V_NUMBER ? pw->num : 400;
            if (v->kw == EMBI_KW_normal) {
                w = 400;
            } else if (v->kw == EMBI_KW_bold) {
                w = 700;
            } else if (v->kw == EMBI_KW_bolder) {
                w = bolder(parent_w);
            } else if (v->kw == EMBI_KW_lighter) {
                w = lighter(parent_w);
            }
            if (w >= 0) {
                o.kind = EMBI_V_NUMBER;
                o.num = w;
                changed = true;
            }
        } else if (p == EMBI_PROP_font_stretch) {
            bool ok;
            double pct = font_stretch_keyword(v->kw, &ok);
            if (ok) {
                o.kind = EMBI_V_PERCENT;
                o.num = pct;
                changed = true;
            }
        } else if (is_border_width(p)) {
            double px = -1;
            if (v->kw == EMBI_KW_thin) {
                px = 1;
            } else if (v->kw == EMBI_KW_medium) {
                px = 3;
            } else if (v->kw == EMBI_KW_thick) {
                px = 5;
            }
            if (px >= 0) {
                o.kind = EMBI_V_LENGTH;
                o.unit = EMBI_U_PX;
                o.num = px;
                changed = true;
            }
        }
        break;
    case EMBI_V_CALC:
        if ((v->calc->type & EMBI_CD_PCT) == 0) {
            const embi_style_env *env = &e->env;
            embi_len_ctx lc = {
                .font_px = p == EMBI_PROP_font_size
                               ? parent_font_px(sc)
                               : font_px_of(elem_get(sc, EMBI_PROP_font_size)),
                .root_px = root_px,
                .vw = env->width,
                .vh = env->height,
                .cq_w = __builtin_nan(""),
                .cq_h = __builtin_nan(""),
            };
            calc_len_user cu = {&lc, sc, p == EMBI_PROP_font_size, p == EMBI_PROP_font_size, false,
                                p == EMBI_PROP_font_size || p == EMBI_PROP_line_height};
            embi_calc_env ce = {.pct_base = 0, .len_px = calc_len_px, .user = &cu};
            double x = embi_calc_eval(v->calc, &ce);
            if (x == x) {
                uint8_t t = v->calc->type;
                if (t & EMBI_CD_LEN) {
                    o.kind = EMBI_V_LENGTH;
                    o.unit = EMBI_U_PX;
                    o.num = x;
                    changed = true;
                } else if (t & EMBI_CD_NUM) {
                    if (p == EMBI_PROP_z_index || p == EMBI_PROP_order) {
                        /* An <integer> from calc(): halfway values round towards positive infinity (CSS Values 4
                         * section 10.9.1). */
                        o.kind = EMBI_V_INTEGER;
                        o.num = __builtin_floor(x + 0.5);
                    } else {
                        o.kind = EMBI_V_NUMBER;
                        o.num = x;
                    }
                    changed = true;
                } else if (t & EMBI_CD_ANGLE) {
                    o.kind = EMBI_V_ANGLE;
                    o.unit = EMBI_U_DEG;
                    o.num = x;
                    changed = true;
                } else if (t & EMBI_CD_TIME) {
                    o.kind = EMBI_V_TIME;
                    o.unit = EMBI_U_S;
                    o.num = x;
                    changed = true;
                } else if (t & EMBI_CD_RES) {
                    o.kind = EMBI_V_RESOLUTION;
                    o.unit = EMBI_U_DPPX;
                    o.num = x;
                    changed = true;
                }
            }
        }
        break;
    default:
        break;
    }
    if (!changed) {
        return v;
    }
    return embi_style_canon(e, &o);
}

/* ------------------------------------------------------------------ cascade winners */

/* The longhands a declaration sets, as written (for a var() shorthand: its longhands). */
static uint32_t origs_of(const embi_pdecl *d, uint16_t *out, uint32_t cap)
{
    uint32_t n = 0;
    if ((d->flags & EMBI_PD_CUSTOM) != 0) {
        return 0;
    }
    if ((d->flags & EMBI_PD_ALL) != 0) {
        /* `all` resets every longhand except direction and unicode-bidi (CSS Cascade 4 section 3.2). */
        for (uint32_t p = 1; p < EMBI_PROP_COUNT && n < cap; p++) {
            if (embi_prop_is_shorthand((embi_prop_id)p) || p == EMBI_PROP_direction ||
                p == EMBI_PROP_unicode_bidi) {
                continue;
            }
            out[n++] = (uint16_t)p;
        }
        return n;
    }
    if ((d->flags & EMBI_PD_HAS_VAR) != 0) {
        if (embi_prop_is_shorthand(d->prop)) {
            const uint16_t *subs = embi_prop_longhands(d->prop);
            const embi_prop_info *pi = embi_prop_info_of(d->prop);
            for (uint32_t i = 0; i < pi->sub_n && n < cap; i++) {
                out[n++] = subs[i];
            }
        } else if (cap > 0) {
            out[n++] = d->prop;
        }
        return n;
    }
    for (uint32_t i = 0; i < d->nlh && n < cap; i++) {
        out[n++] = d->lh[i].prop;
    }
    return n;
}

uint32_t embi_style_origs(const embi_pdecl *d, uint16_t *out, uint32_t cap)
{
    return origs_of(d, out, cap);
}

uint16_t embi_style_target(const embi_sctx *sc, uint16_t o)
{
    if (embi_prop_is_logical((embi_prop_id)o)) {
        return embi_prop_to_physical((embi_prop_id)o, sc->wm, sc->dir);
    }
    return o;
}

/* Finds, scanning backwards from candidate ci, the nearest candidate that sets target t below ci
 * in the cascade (same importance and a lower origin, or the same origin and a lower layer). */
static int32_t find_lower(const embi_sctx *sc, uint16_t t, int32_t ci, bool layer_only,
                          uint16_t *orig_out)
{
    const embi_cand *c = &sc->cands[ci];
    for (int32_t j = ci - 1; j >= 0; j--) {
        const embi_cand *o = &sc->cands[j];
        /* revert-layer: a lower layer of the same importance and origin; the style attribute counts as a layer above
         * all the others, so it rolls back to the style sheets. */
        bool lower = layer_only ? (o->oset == c->oset && (o->layer < c->layer || (c->attr && !o->attr)))
                                : (o->origin < c->origin);
        if (!lower) {
            continue;
        }
        uint16_t ids[EMBI_PROP_COUNT];
        uint32_t n = origs_of(o->d, ids, EMBI_PROP_COUNT);
        for (uint32_t i = 0; i < n; i++) {
            if (embi_style_target(sc, ids[i]) == t) {
                *orig_out = ids[i];
                return j;
            }
        }
    }
    return -1;
}

static const embi_value *unset_value(const embi_sctx *sc, uint16_t t)
{
    const embi_prop_info *pi = embi_prop_info_of(t);
    if (pi != nullptr && (pi->flags & EMBI_PF_INHERITED) != 0) {
        return embi_style_parent_value(sc, t);
    }
    return embi_style_initial_value(sc, t);
}

static bool var_lookup(void *user, embi_str name, embi_css_slice *value);

/* attr(): the element's attribute, case-insensitive for an HTML element. A namespace prefix is not
 * resolved (the style engine keeps no @namespace table at computed-value time): such a reference
 * finds nothing and takes its fallback. */
static bool embi_style_attr_lookup(void *user, embi_str ns, embi_str name, embi_str *value)
{
    embi_sctx *sc = user;
    if (sc->el == nullptr || ns.len > 0) {
        return false;
    }
    const embi_attr *a = embi_el_get_attribute(sc->el, name);
    if (a == nullptr) {
        return false;
    }
    size_t cap = (size_t)a->value.len * 3 + 4;
    char *buf = embi_arena_alloc(&sc->eng->arena, cap, 1);
    size_t n = embi_dstr_to_utf8(a->value, buf, cap);
    if (n > cap) {
        n = cap;
    }
    *value = (embi_str){buf, n};
    return true;
}

/* sibling-index() / sibling-count(): the element's position among its parent's element children, or their number. */
static uint32_t embi_style_sibling_lookup(void *user, bool count)
{
    embi_sctx *sc = user;
    if (sc->el == nullptr) {
        return 1;
    }
    uint32_t idx = 1, total = 1;
    const embi_node *self = (const embi_node *)sc->el;
    if (self->parent != nullptr) {
        idx = 0;
        total = 0;
        for (const embi_node *k = self->parent->first_child; k != nullptr; k = k->next) {
            if (embi_is_element(k)) {
                total++;
                if (k == self) {
                    idx = total;
                }
            }
        }
        if (idx == 0) {
            idx = total = 1;
        }
    }
    return count ? total : idx;
}

static const embi_value *compute_var(embi_sctx *sc, const embi_pdecl *d, uint16_t orig)
{
    if (embi_css_has_attr(d->raw)) {
        sc->unsafe = true; /* the value depends on the element's attributes: never shared */
    }
    embi_var_env env = {.user = sc, .lookup = var_lookup, .attr = embi_style_attr_lookup, .max_tokens = 0,
                       .sibling = embi_style_sibling_lookup};
    embi_style_engine *e = sc->eng;
    /* Substitute into the scratch arena and look the text up: the same declaration, longhand and substituted text give the
     * same value, so a restyle that changes nothing here (an animation frame) allocates nothing in the engine arena. */
    embi_arena_pos mark = embi_arena_mark(&e->vscratch);
    embi_css_slice sub = d->raw;
    if (!embi_css_substitute_vars(&e->vscratch, &env, (embi_str){nullptr, 0}, d->raw, &sub)) {
        embi_arena_reset_to(&e->vscratch, mark);
        return nullptr;
    }
    embi_sb_clear(&e->vsb);
    for (uint32_t i = 0; i < sub.n; i++) {
        embi_css_serialize_cv(&e->vsb, &sub.p[i]);
    }
    embi_arena_reset_to(&e->vscratch, mark);
    embi_str text = {e->vsb.data, e->vsb.len};
    uint64_t h = embi_hash_bytes(text.ptr, text.len, 0x7661u) ^ embi_hash64((uint64_t)(uintptr_t)d ^ ((uint64_t)orig << 48));
    uint64_t found = 0;
    if (embi_hashmap_get(&e->vcache, h, &found)) {
        const embi_varrec *rec = (const embi_varrec *)(uintptr_t)found;
        if (rec->d == d && rec->orig == orig && rec->prop == (uint16_t)d->prop && rec->len == text.len && memcmp(rec->text, text.ptr, text.len) == 0) {
            return rec->v;
        }
    }
    embi_longhand buf[EMBI_LONGHAND_MAX];
    uint32_t n = embi_css_parse_decl_vars(&e->arena, d->prop, d->raw, &env, buf, EMBI_LONGHAND_MAX);
    const embi_value *v = nullptr;
    for (uint32_t i = 0; i < n && v == nullptr; i++) {
        if (buf[i].prop == orig) {
            v = embi_style_canon(e, &buf[i].value);
        }
    }
    /* A value that substitutes a new text on every restyle (a custom property animated by script) would grow the cache
     * without bound: at the cap the index starts over, so texts seen again are parsed once more and then remembered. */
    if (e->nvcache >= EMBI_STYLE_VCACHE_MAX) {
        embi_hashmap_clear(&e->vcache);
        e->nvcache = 0;
    }
    if (!embi_hashmap_get(&e->vcache, h, &found)) {
        embi_varrec *rec = embi_arena_alloc(&e->arena, sizeof *rec, 8);
        rec->d = d;
        rec->orig = orig;
        rec->prop = (uint16_t)d->prop;
        rec->len = text.len;
        rec->text = embi_style_copy_str(&e->arena, text).ptr;
        rec->v = v;
        embi_hashmap_put(&e->vcache, h, (uint64_t)(uintptr_t)rec);
        e->nvcache++;
    }
    return v;
}

static const embi_value *compute_winner(embi_sctx *sc, uint16_t t, int32_t ci, uint16_t orig,
                                        int depth)
{
    if (ci < 0 || depth > 16) {
        return unset_value(sc, t);
    }
    const embi_pdecl *d = sc->cands[ci].d;
    const embi_value *v = nullptr;
    if ((d->flags & EMBI_PD_ALL) != 0) {
        v = d->lh[0].val;
    } else if ((d->flags & EMBI_PD_HAS_VAR) != 0) {
        v = compute_var(sc, d, orig);
    } else {
        for (uint32_t j = 0; j < d->nlh && v == nullptr; j++) {
            if (d->lh[j].prop == orig) {
                v = d->lh[j].val;
            }
        }
    }
    if (v == nullptr) {
        return unset_value(sc, t);
    }
    switch (v->kind) {
    case EMBI_V_INITIAL:
        return embi_style_initial_value(sc, t);
    case EMBI_V_INHERIT:
        return embi_style_parent_value(sc, t);
    case EMBI_V_UNSET:
        return unset_value(sc, t);
    case EMBI_V_REVERT:
    case EMBI_V_REVERT_LAYER: {
        uint16_t oj = 0;
        int32_t j = find_lower(sc, t, ci, v->kind == EMBI_V_REVERT_LAYER, &oj);
        if (j < 0 && v->kind == EMBI_V_REVERT_LAYER) {
            /* No lower layer sets it: the cascade rolls back to the previous origin, as revert does. */
            j = find_lower(sc, t, ci, false, &oj);
        }
        if (j < 0) {
            return unset_value(sc, t);
        }
        return compute_winner(sc, t, j, oj, depth + 1);
    }
    default:
        return finalize(sc, t, v);
    }
}

/* Computes the value of target t from its winning candidate, and sets it. */
void embi_style_compute_target(embi_sctx *sc, uint16_t t)
{
    if (sc->ovr_gen[t] == sc->gen || sc->win_gen[t] != sc->gen) {
        return;
    }
    const embi_win *w = &sc->win[t];
    const embi_value *v = compute_winner(sc, t, w->cand, w->orig, 0);
    embi_style_set_ovr(sc, t, v);
}

/* ------------------------------------------------------------------ var() lookups */

/* Custom property lookup for var(): the element's own declared value, else the inherited computed
 * value. The custom-winner list is built by the cascade before any normal property is computed. */
static bool var_lookup(void *user, embi_str name, embi_css_slice *value)
{
    embi_sctx *sc = user;
    embi_atom a = embi_atom_find(name.ptr, name.len);
    if (a == 0) {
        return false;
    }
    if (sc->custom_ready) {
        /* The element's computed custom properties: registered resets and validated values. */
        if (sc->ccustom != nullptr) {
            for (uint32_t i = 0; i < sc->ccustom->n; i++) {
                if (sc->ccustom->e[i].name == a) {
                    *value = sc->ccustom->e[i].val;
                    return true;
                }
            }
        }
        return false;
    }
    for (uint32_t i = 0; i < sc->ncw; i++) {
        if (sc->cw[i].name == a) {
            *value = sc->cw[i].d->raw;
            return true;
        }
    }
    if (sc->pcustom != nullptr) {
        for (uint32_t i = 0; i < sc->pcustom->n; i++) {
            if (sc->pcustom->e[i].name == a) {
                *value = sc->pcustom->e[i].val;
                return true;
            }
        }
    }
    return false;
}

bool embi_style_var_lookup(void *user, embi_str name, embi_css_slice *value)
{
    return var_lookup(user, name, value);
}

/* ------------------------------------------------------------------ fixups */

static bool keyword_is(const embi_value *v, uint16_t kw)
{
    return v->kind == EMBI_V_KEYWORD && v->kw == kw;
}

/* The blockified display of an absolutely positioned box whose own display was inline-level: the keyword with aux
 * EMBI_DISP_FROM_INLINE, so that layout knows the original display (the static position of an inline-level box is in
 * its line, a block-level one starts the next line: CSS 2.1 section 10.3.7, CSS Position 3 section 4.1). One shared
 * value per keyword (the canonical table keys values by their text, which has no aux). nullptr for other keywords. */
static const embi_value k_from_inline[] = {
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW_block},
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW_flex},
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW_grid},
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW_table},
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW_list_item},
    {.kind = EMBI_V_KEYWORD, .aux = EMBI_DISP_FROM_INLINE, .kw = EMBI_KW__internal_flow_root_list_item},
};

static const embi_value *abspos_from_inline(uint16_t nk)
{
    for (size_t i = 0; i < sizeof k_from_inline / sizeof k_from_inline[0]; i++) {
        if (k_from_inline[i].kw == nk) {
            return &k_from_inline[i];
        }
    }
    return nullptr;
}

/* The legacy flexbox (display: -webkit-box) as a flex container: -webkit-box-orient, -webkit-box-direction,
 * -webkit-box-pack, -webkit-box-align and -webkit-box-lines set the flex properties. Flex properties do not apply to a
 * legacy box, so they are replaced whatever the author wrote. */
static void set_kw_ovr(embi_sctx *sc, uint16_t prop, uint16_t kw)
{
    embi_value k;
    memset(&k, 0, sizeof k);
    k.kind = EMBI_V_KEYWORD;
    k.kw = kw;
    embi_style_set_ovr(sc, prop, embi_style_canon(sc->eng, &k));
}

static void legacy_box(embi_sctx *sc, bool vert)
{
    const embi_value *dv = elem_get(sc, EMBI_PROP__webkit_box_direction);
    bool rev = keyword_is(dv, EMBI_KW_reverse);
    set_kw_ovr(sc, EMBI_PROP_flex_direction,
               vert ? (rev ? EMBI_KW_column_reverse : EMBI_KW_column) : (rev ? EMBI_KW_row_reverse : EMBI_KW_row));
    const embi_value *pk = elem_get(sc, EMBI_PROP__webkit_box_pack);
    uint16_t jc = EMBI_KW_flex_start;
    if (keyword_is(pk, EMBI_KW_end)) {
        jc = EMBI_KW_flex_end;
    } else if (keyword_is(pk, EMBI_KW_center)) {
        jc = EMBI_KW_center;
    } else if (keyword_is(pk, EMBI_KW_justify)) {
        jc = EMBI_KW_space_between;
    }
    set_kw_ovr(sc, EMBI_PROP_justify_content, jc);
    const embi_value *al = elem_get(sc, EMBI_PROP__webkit_box_align);
    uint16_t ai = EMBI_KW_stretch;
    if (keyword_is(al, EMBI_KW_start)) {
        ai = EMBI_KW_flex_start;
    } else if (keyword_is(al, EMBI_KW_end)) {
        ai = EMBI_KW_flex_end;
    } else if (keyword_is(al, EMBI_KW_center)) {
        ai = EMBI_KW_center;
    } else if (keyword_is(al, EMBI_KW_baseline)) {
        ai = EMBI_KW_baseline;
    }
    set_kw_ovr(sc, EMBI_PROP_align_items, ai);
    set_kw_ovr(sc, EMBI_PROP__internal_legacy_box, EMBI_KW_yes);
    set_kw_ovr(sc, EMBI_PROP_flex_wrap, keyword_is(elem_get(sc, EMBI_PROP__webkit_box_lines), EMBI_KW_multiple) ? EMBI_KW_wrap : EMBI_KW_nowrap);
}

/* An item of a legacy box (a parent that legacy_box() marked): -webkit-box-flex gives the flex factors (a legacy box
 * grows and shrinks its flexible items by the one ratio) and -webkit-box-ordinal-group the order. The legacy box ignores
 * the item's own flex-* and order, so the mapping is unconditional there; under a modern flex container the
 * -webkit-box-* properties of the items do nothing. */
static void legacy_item(embi_sctx *sc)
{
    if (sc->parent == nullptr || !keyword_is(embi_style_parent_value(sc, EMBI_PROP__internal_legacy_box), EMBI_KW_yes)) {
        return;
    }
    const embi_value *bf = elem_get(sc, EMBI_PROP__webkit_box_flex);
    embi_value o;
    memset(&o, 0, sizeof o);
    o.kind = EMBI_V_NUMBER;
    o.num = bf->kind == EMBI_V_NUMBER ? bf->num : 0.0;
    const embi_value *c = embi_style_canon(sc->eng, &o);
    embi_style_set_ovr(sc, EMBI_PROP_flex_grow, c);
    embi_style_set_ovr(sc, EMBI_PROP_flex_shrink, c);
    const embi_value *og = elem_get(sc, EMBI_PROP__webkit_box_ordinal_group);
    memset(&o, 0, sizeof o);
    o.kind = EMBI_V_INTEGER;
    o.num = (og->kind == EMBI_V_INTEGER || og->kind == EMBI_V_NUMBER ? og->num : 1.0) - 1.0; /* group 1 is the default */
    embi_style_set_ovr(sc, EMBI_PROP_order, embi_style_canon(sc->eng, &o));
}

/* display: blockification (CSS Display 3 section 2.7). */
void embi_style_fix_display(embi_sctx *sc)
{
    const embi_value *dv = elem_get(sc, EMBI_PROP_display);
    bool marked = false; /* legacy_box() set -internal-legacy-box on this element */
    if (dv->kind == EMBI_V_KEYWORD && (dv->kw == EMBI_KW__webkit_box || dv->kw == EMBI_KW__webkit_inline_box)) {
        /* The legacy box (CSS Overflow 4 section 5.1.1): with -webkit-box-orient vertical and a line clamp
         * (continue: collapse, discard or -webkit-legacy) it is a flow-root (inline-block), the form real sites use
         * for -webkit-line-clamp; otherwise a flex container (a column for the vertical orientation). */
        bool inl = dv->kw == EMBI_KW__webkit_inline_box;
        const embi_value *ov = elem_get(sc, EMBI_PROP__webkit_box_orient);
        bool vert = ov->kind == EMBI_V_KEYWORD && (ov->kw == EMBI_KW_vertical || ov->kw == EMBI_KW_block_axis);
        const embi_value *cv = elem_get(sc, EMBI_PROP_continue);
        bool legacy = cv->kind == EMBI_V_KEYWORD && cv->kw == EMBI_KW__webkit_legacy;
        bool clamp = cv->kind == EMBI_V_KEYWORD &&
                     (cv->kw == EMBI_KW_collapse || cv->kw == EMBI_KW_discard || legacy);
        embi_value o;
        memset(&o, 0, sizeof o);
        o.kind = EMBI_V_KEYWORD;
        if (vert && clamp) {
            o.kw = inl ? EMBI_KW_inline_block : EMBI_KW_flow_root;
            if (legacy) {
                embi_value k;
                memset(&k, 0, sizeof k);
                k.kind = EMBI_V_KEYWORD;
                k.kw = EMBI_KW_collapse;
                embi_style_set_ovr(sc, EMBI_PROP_continue, embi_style_canon(sc->eng, &k));
            }
        } else {
            o.kw = inl ? EMBI_KW_inline_flex : EMBI_KW_flex;
            legacy_box(sc, vert);
            marked = true;
        }
        embi_style_set_ovr(sc, EMBI_PROP_display, embi_style_canon(sc->eng, &o));
        dv = elem_get(sc, EMBI_PROP_display);
    }
    /* Only legacy_box() may mark an element: `all: inherit` copies every longhand, the internal marker included, so an
     * inherited "yes" on an element that is not a legacy box is reset (otherwise its children would map as legacy items). */
    if (!marked && keyword_is(elem_get(sc, EMBI_PROP__internal_legacy_box), EMBI_KW_yes)) {
        set_kw_ovr(sc, EMBI_PROP__internal_legacy_box, EMBI_KW_no);
    }
    legacy_item(sc);
    if (dv->kind != EMBI_V_KEYWORD) {
        return;
    }
    bool need = sc->is_root;
    const embi_value *fl = elem_get(sc, EMBI_PROP_float);
    if (fl->kind == EMBI_V_KEYWORD && fl->kw != EMBI_KW_none) {
        need = true;
    }
    const embi_value *pos = elem_get(sc, EMBI_PROP_position);
    if (keyword_is(pos, EMBI_KW_absolute) || keyword_is(pos, EMBI_KW_fixed)) {
        need = true;
    }
    if (sc->parent != nullptr) {
        const embi_value *pd = embi_style_parent_value(sc, EMBI_PROP_display);
        if (pd->kind == EMBI_V_KEYWORD && (pd->kw == EMBI_KW_flex || pd->kw == EMBI_KW_inline_flex ||
                                           pd->kw == EMBI_KW_grid || pd->kw == EMBI_KW_inline_grid)) {
            need = true;
        }
    }
    if (!need) {
        return;
    }
    uint16_t nk = blockify(dv->kw);
    if (sc->is_root && dv->kw == EMBI_KW_contents) {
        nk = EMBI_KW_block; /* CSS Display 3 section 2.5: contents on the root element computes to block */
    }
    if (nk != dv->kw) {
        bool inline_level = dv->kw == EMBI_KW_inline || dv->kw == EMBI_KW_inline_block || dv->kw == EMBI_KW_inline_flex ||
                            dv->kw == EMBI_KW_inline_grid || dv->kw == EMBI_KW_inline_table ||
                            dv->kw == EMBI_KW__internal_inline_list_item ||
                            dv->kw == EMBI_KW__internal_inline_flow_root_list_item;
        const embi_value *from_inline = inline_level ? abspos_from_inline(nk) : nullptr;
        if (from_inline != nullptr && (keyword_is(pos, EMBI_KW_absolute) || keyword_is(pos, EMBI_KW_fixed))) {
            embi_style_set_ovr(sc, EMBI_PROP_display, from_inline);
            return;
        }
        embi_value o;
        memset(&o, 0, sizeof o);
        o.kind = EMBI_V_KEYWORD;
        o.kw = nk;
        embi_style_set_ovr(sc, EMBI_PROP_display, embi_style_canon(sc->eng, &o));
    }
}

static void fix_width(embi_sctx *sc, uint16_t width, uint16_t style)
{
    if (!embi_style_touched(sc, width) && !embi_style_touched(sc, style)) {
        return;
    }
    const embi_value *s = elem_get(sc, style);
    const embi_value *w = elem_get(sc, width);
    if (!keyword_is(s, EMBI_KW_none) && !keyword_is(s, EMBI_KW_hidden)) {
        /* A visible style with a width that was not written reads the used initial value (the
         * initial style's stored 0px belongs to its none style). */
        if (!embi_style_touched(sc, width) && sc->eng->w_med != nullptr && sc->init != nullptr) {
            embi_style_set_ovr(sc, width, sc->eng->w_med);
        } else if (w != nullptr && w->kind == EMBI_V_LENGTH && w->unit == EMBI_U_PX && w->num > 1.0 &&
                   w->num < 1.0e9 && w->num != (double)(int64_t)w->num) {
            /* A width over one px snaps down to whole px (CSS Values 4 section 6.1.1, snap as a border width). */
            embi_value o = *w;
            o.num = (double)(int64_t)w->num;
            embi_style_set_ovr(sc, width, embi_style_canon(sc->eng, &o));
        }
        return;
    }
    if (keyword_is(s, EMBI_KW_none) || keyword_is(s, EMBI_KW_hidden)) {
        if (!(w->kind == EMBI_V_LENGTH && w->num == 0)) {
            embi_value o;
            memset(&o, 0, sizeof o);
            o.kind = EMBI_V_LENGTH;
            o.unit = EMBI_U_PX;
            o.num = 0;
            embi_style_set_ovr(sc, width, embi_style_canon(sc->eng, &o));
        }
    }
}

/* text-align: -internal-center (the `th` of the UA sheet, HTML section 15.3.8): center when the parent's text-align is
 * start, otherwise the parent's value. */
void embi_style_fix_text_align(embi_sctx *sc)
{
    const embi_value *v = elem_get(sc, EMBI_PROP_text_align);
    if (!keyword_is(v, EMBI_KW__internal_center)) {
        return;
    }
    embi_value o;
    memset(&o, 0, sizeof o);
    o.kind = EMBI_V_KEYWORD;
    o.kw = EMBI_KW_center;
    if (sc->parent != nullptr) {
        const embi_value *pv = embi_style_parent_value(sc, EMBI_PROP_text_align);
        if (pv->kind == EMBI_V_KEYWORD && pv->kw != EMBI_KW_start && pv->kw != EMBI_KW__internal_center) {
            o.kw = pv->kw;
        }
    }
    embi_style_set_ovr(sc, EMBI_PROP_text_align, embi_style_canon(sc->eng, &o));
}

/* border-*-width computes to 0 when its style is none or hidden (CSS Backgrounds 3 section 4). */
void embi_style_fix_widths(embi_sctx *sc)
{
    fix_width(sc, EMBI_PROP_border_top_width, EMBI_PROP_border_top_style);
    fix_width(sc, EMBI_PROP_border_right_width, EMBI_PROP_border_right_style);
    fix_width(sc, EMBI_PROP_border_bottom_width, EMBI_PROP_border_bottom_style);
    fix_width(sc, EMBI_PROP_border_left_width, EMBI_PROP_border_left_style);
    /* outline-width keeps its value when outline-style is none (CSS UI 4 section 5.1; WPT css-ui/outline-009.html). */
}

/* The used color scheme of an element: dark when the environment prefers dark and the element's
 * color-scheme lists dark. */
bool embi_style_scheme_dark(const embi_sctx *sc, const embi_value *scheme)
{
    /* The schemes the element lists. */
    bool has_dark = false;
    bool has_light = false;
    bool only = false;
    if (scheme->kind == EMBI_V_KEYWORD) {
        has_dark = scheme->kw == EMBI_KW_dark;
        has_light = scheme->kw == EMBI_KW_light;
        only = scheme->kw == EMBI_KW_only;
    } else if (scheme->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < scheme->list.n; i++) {
            const embi_value *it = &scheme->list.items[i];
            if (keyword_is(it, EMBI_KW_dark)) {
                has_dark = true;
            } else if (keyword_is(it, EMBI_KW_light)) {
                has_light = true;
            } else if (keyword_is(it, EMBI_KW_only)) {
                only = true;
            }
        }
    }
    /* The preferred scheme is used when the element lists it. Otherwise the other scheme is used
     * unless the list says `only`. A list naming neither (normal) is light. */
    if (sc->eng->env.dark) {
        return has_dark;
    }
    return has_dark && !has_light && !only;
}
