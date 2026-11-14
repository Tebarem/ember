/*
 * Shorthand expansion (W2c), part two: the shorthands with their own grammar. font (with the
 * line-height split and system fonts), font-variant, font-synthesis, background (layers, position
 * and size), mask, border-image, mask-border, transition, animation, container, grid-row,
 * grid-column, grid-area, grid-template and grid.
 *
 * Multi-layer shorthands (background, mask, transition, animation) produce one comma list per
 * longhand with one item per layer, in the same shape as parsing that longhand directly.
 */
#include "css/value_int.h"

#include <string.h>

/* ------------------------------------------------------------------ helpers */

static bool kw_in(const char *const *names, size_t n, const embi_css_cv *t)
{
    if (!cv_any_ident(t)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_str_is(t->tok.value, names[i])) {
            return true;
        }
    }
    return false;
}
#define KW_IN(arr, t) kw_in((arr), sizeof(arr) / sizeof((arr)[0]), (t))

static embi_value axis_val(const embi_pos_axis *ax)
{
    embi_value v = {0};
    v.kind = EMBI_V_POS_AXIS;
    v.axis = ax;
    return v;
}

static embi_value two_kw(embi_arena *a, uint16_t k0, uint16_t k1)
{
    embi_value v[2] = {v_kw(k0), v_kw(k1)};
    embi_value out;
    embi_make_list(a, v, 2, EMBI_SEP_SPACE, &out);
    return out;
}

static embi_position *default_position(embi_arena *a)
{
    embi_position *p = EMBI_NEW(a, embi_position);
    p->x.off = v_pct(0);
    p->y.off = v_pct(0);
    return p;
}

/* ------------------------------------------------------------------ font */

static const char *const sys_fonts[] = {
    "caption", "icon", "menu", "message-box", "small-caption", "status-bar",
};
static const char *const stretch_kws[] = {
    "ultra-condensed", "extra-condensed", "condensed", "semi-condensed", "semi-expanded",
    "expanded", "extra-expanded", "ultra-expanded",
};

static uint32_t expand_font(embi_sh *sh, embi_css_slice value, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    if (cur_left(c) == 1 && KW_IN(sys_fonts, cur_peek(c))) {
        /* System fonts: the embedder resolves the keyword. Everything else is initial, and the
         * family carries the keyword so the embedder can tell. */
        const embi_css_cv *t = cur_peek(c);
        embi_value fam = {0};
        fam.kind = EMBI_V_IDENT;
        fam.ident = embi_atom_intern(t->tok.value.ptr, t->tok.value.len);
        embi_value list;
        embi_make_list(a, &fam, 1, EMBI_SEP_COMMA, &list);
        embi_sh_put(sh, EMBI_PROP_font_family, &list);
        return embi_sh_finish(sh, out);
    }
    embi_value style, caps, weight, stretch;
    bool hs = false, hc = false, hw = false, ht = false;
    uint32_t pre = 0;
    while (pre < 4 && !cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (cv_ident(t, "normal")) {
            c->i++;
        } else if (!hs && (cv_ident(t, "italic") || cv_ident(t, "oblique"))) {
            if (!embi_parse_prefix(a, EMBI_PROP_font_style, c, &style)) {
                return 0;
            }
            hs = true;
        } else if (!hc && cv_ident(t, "small-caps")) {
            caps = v_kw(EMBI_KW_small_caps);
            hc = true;
            c->i++;
        } else if (!hw && (cv_ident(t, "bold") || cv_ident(t, "bolder") || cv_ident(t, "lighter") ||
                           t->tok.type == EMBI_CSS_NUMBER)) {
            if (!embi_parse_prefix(a, EMBI_PROP_font_weight, c, &weight)) {
                return 0;
            }
            hw = true;
        } else if (!ht && KW_IN(stretch_kws, t)) {
            stretch = v_kw(cv_kw(t));
            ht = true;
            c->i++;
        } else {
            break;
        }
        pre++;
    }
    embi_value size, lh, fam;
    if (!embi_parse_prefix(a, EMBI_PROP_font_size, c, &size)) {
        return 0;
    }
    bool have_lh = false;
    if (cv_delim(cur_peek(c), '/')) {
        c->i++;
        if (!embi_parse_prefix(a, EMBI_PROP_line_height, c, &lh)) {
            return 0;
        }
        have_lh = true;
    }
    if (cur_done(c)) {
        return 0;
    }
    uint32_t idx = (uint32_t)(c->c[c->i] - value.p);
    embi_css_slice rest = {value.p + idx, value.n - idx};
    if (!embi_value_parse_longhand(a, EMBI_PROP_font_family, rest, &fam)) {
        return 0;
    }
    if (hs) embi_sh_put(sh, EMBI_PROP_font_style, &style);
    if (hc) embi_sh_put(sh, EMBI_PROP_font_variant_caps, &caps);
    if (hw) embi_sh_put(sh, EMBI_PROP_font_weight, &weight);
    if (ht) embi_sh_put(sh, EMBI_PROP_font_stretch, &stretch);
    embi_sh_put(sh, EMBI_PROP_font_size, &size);
    if (have_lh) embi_sh_put(sh, EMBI_PROP_line_height, &lh);
    embi_sh_put(sh, EMBI_PROP_font_family, &fam);
    return embi_sh_finish(sh, out);
}

/* font-variant: every token goes to the longhand whose keywords (or functions) contain it. */
static uint32_t expand_font_variant(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    static const uint16_t targets[] = {
        EMBI_PROP_font_variant_caps, EMBI_PROP_font_variant_ligatures,
        EMBI_PROP_font_variant_numeric, EMBI_PROP_font_variant_east_asian,
        EMBI_PROP_font_variant_alternates, EMBI_PROP_font_variant_position,
        EMBI_PROP_font_variant_emoji,
    };
    enum { NT = sizeof targets / sizeof targets[0] };
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "normal")) {
        return embi_sh_finish(sh, out);
    }
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "none")) {
        embi_value none = v_kw(EMBI_KW_none);
        embi_sh_put(sh, EMBI_PROP_font_variant_ligatures, &none);
        return embi_sh_finish(sh, out);
    }
    const embi_css_cv **bucket[NT];
    uint32_t count[NT] = {0};
    uint32_t total = cur_left(c);
    for (int i = 0; i < NT; i++) {
        bucket[i] = EMBI_NEW_ARRAY(sh->a, const embi_css_cv *, total);
    }
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        int target = -1;
        if (cv_any_ident(t)) {
            uint16_t kw = embi_kw_lookup(t->tok.value);
            if (kw == EMBI_KW_INVALID || kw == EMBI_KW_normal || kw == EMBI_KW_none) {
                return 0;
            }
            for (int i = 0; i < NT && target < 0; i++) {
                if (embi_prop_has_kw(embi_prop_info_of(targets[i]), kw)) {
                    target = i;
                }
            }
        } else if (t->tok.type == EMBI_CSS_FUNCTION) {
            target = 4; /* alternates functions */
        }
        if (target < 0) {
            return 0;
        }
        bucket[target][count[target]++] = t;
    }
    for (int i = 0; i < NT; i++) {
        if (count[i] == 0) {
            continue;
        }
        embi_cur sub = {bucket[i], count[i], 0};
        embi_value v;
        if (!embi_parse_prefix(sh->a, targets[i], &sub, &v) || !cur_done(&sub)) {
            return 0;
        }
        embi_sh_put(sh, targets[i], &v);
    }
    return embi_sh_finish(sh, out);
}

static uint32_t expand_font_synthesis(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    static const struct {
        const char *name;
        uint16_t prop;
    } parts[] = {
        {"weight", EMBI_PROP_font_synthesis_weight},
        {"style", EMBI_PROP_font_synthesis_style},
        {"small-caps", EMBI_PROP_font_synthesis_small_caps},
        {"position", EMBI_PROP_font_synthesis_position},
    };
    embi_value none = v_kw(EMBI_KW_none);
    embi_value autov = v_kw(EMBI_KW_auto);
    for (int i = 0; i < 4; i++) {
        embi_sh_put(sh, parts[i].prop, &none);
    }
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "none")) {
        return embi_sh_finish(sh, out);
    }
    bool seen[4] = {false};
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        bool found = false;
        for (int i = 0; i < 4 && !found; i++) {
            if (cv_ident(t, parts[i].name)) {
                if (seen[i]) {
                    return 0;
                }
                seen[i] = found = true;
                embi_sh_put(sh, parts[i].prop, &autov);
            }
        }
        if (!found) {
            return 0;
        }
    }
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ background and mask */

typedef struct layer {
    embi_value image, pos, size, repeat, attach, origin, clip, comp, mode, color;
    bool h_image, h_pos, h_size, h_repeat, h_attach, h_comp, h_mode, h_color;
    uint16_t box1, box2; /* origin, clip */
    uint8_t nbox;
    bool clip_special; /* the clip is `text` / `no-clip` (it cannot be an origin) */
    embi_position *position;
} layer;

static const char *const visual_boxes[] = {"border-box", "padding-box", "content-box"};
static const char *const geometry_boxes[] = {
    "border-box", "padding-box", "content-box", "margin-box", "fill-box", "stroke-box", "view-box",
};
static const char *const comp_ops[] = {"add", "subtract", "intersect", "exclude"};
static const char *const mask_modes[] = {"match-source", "alpha", "luminance"};

static bool parse_layer(embi_arena *a, embi_cur *s, bool mask, bool allow_color, layer *L)
{
    memset(L, 0, sizeof *L);
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        embi_value v;
        if (!L->h_image) {
            if (cv_ident(t, "none")) {
                L->image = v_kw(EMBI_KW_none);
                L->h_image = true;
                s->i++;
                continue;
            }
            if (embi_parse_image(a, t, &v)) {
                L->image = v;
                L->h_image = true;
                s->i++;
                continue;
            }
        }
        if (!L->h_repeat) {
            embi_cur save = *s;
            if (embi_parse_repeat_style(a, s, &v)) {
                L->repeat = v;
                L->h_repeat = true;
                continue;
            }
            *s = save;
        }
        if (!mask && !L->h_attach &&
            (cv_ident(t, "scroll") || cv_ident(t, "fixed") || cv_ident(t, "local"))) {
            L->attach = v_kw(cv_kw(t));
            L->h_attach = true;
            s->i++;
            continue;
        }
        bool is_box = mask ? KW_IN(geometry_boxes, t) : KW_IN(visual_boxes, t);
        bool is_special = mask ? cv_ident(t, "no-clip") : cv_ident(t, "text");
        if (is_box || is_special) {
            if (L->nbox >= 2 || (L->nbox == 1 && L->clip_special)) {
                return false;
            }
            if (is_special) {
                L->clip_special = true;
            }
            if (L->nbox == 0) {
                L->box1 = cv_kw(t);
            } else {
                L->box2 = cv_kw(t);
            }
            L->nbox++;
            s->i++;
            continue;
        }
        if (mask && !L->h_comp && KW_IN(comp_ops, t)) {
            L->comp = v_kw(cv_kw(t));
            L->h_comp = true;
            s->i++;
            continue;
        }
        if (mask && !L->h_mode && KW_IN(mask_modes, t)) {
            L->mode = v_kw(cv_kw(t));
            L->h_mode = true;
            s->i++;
            continue;
        }
        if (!L->h_pos) {
            embi_position *p = EMBI_NEW(a, embi_position);
            if (embi_parse_position(a, s, p)) {
                L->position = p;
                L->h_pos = true;
                if (cv_delim(cur_peek(s), '/')) {
                    s->i++;
                    if (!embi_parse_bg_size(a, s, &L->size)) {
                        return false;
                    }
                    L->h_size = true;
                }
                continue;
            }
        }
        if (allow_color && !L->h_color && embi_color_cv(a, t, &L->color)) {
            L->h_color = true;
            s->i++;
            continue;
        }
        return false;
    }
    /* Boxes: one box sets origin and clip; two set origin then clip. */
    if (L->nbox == 1) {
        if (L->clip_special) {
            L->clip = v_kw(L->box1);
        } else {
            L->origin = v_kw(L->box1);
            L->clip = v_kw(L->box1);
        }
    } else if (L->nbox == 2) {
        L->origin = v_kw(L->box1);
        L->clip = v_kw(L->box2);
    }
    return true;
}

static uint32_t expand_layers(embi_sh *sh, embi_cur *c, bool mask, embi_longhand *out)
{
    embi_arena *a = sh->a;
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, c, &n);
    layer *ls = EMBI_NEW_ARRAY(a, layer, n);
    for (uint32_t i = 0; i < n; i++) {
        if (cur_left(&segs[i]) == 0 ||
            !parse_layer(a, &segs[i], mask, !mask && i + 1 == n, &ls[i])) {
            return 0;
        }
    }
    embi_value *image = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *posx = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *posy = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *pos = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *size = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *repeat = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *attach = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *origin = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *clip = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *comp = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *mode = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        layer *L = &ls[i];
        image[i] = L->h_image ? L->image : v_kw(EMBI_KW_none);
        embi_position *p = L->h_pos ? L->position : default_position(a);
        posx[i] = axis_val(&p->x);
        posy[i] = axis_val(&p->y);
        pos[i] = (embi_value){0};
        pos[i].kind = EMBI_V_POSITION;
        pos[i].pos = p;
        size[i] = L->h_size ? L->size : two_kw(a, EMBI_KW_auto, EMBI_KW_auto);
        repeat[i] = L->h_repeat ? L->repeat : two_kw(a, cv_kw(&(embi_css_cv){.tok = {.type = EMBI_CSS_IDENT, .value = EMBI_STR("repeat")}}), EMBI_KW_repeat);
        attach[i] = L->h_attach ? L->attach : v_kw(EMBI_KW_scroll);
        origin[i] = L->nbox ? (L->clip_special && L->nbox == 1 ? v_kw(mask ? EMBI_KW_border_box : EMBI_KW_padding_box) : L->origin)
                            : v_kw(mask ? EMBI_KW_border_box : EMBI_KW_padding_box);
        clip[i] = L->nbox ? L->clip : v_kw(EMBI_KW_border_box);
        comp[i] = L->h_comp ? L->comp : v_kw(EMBI_KW_add);
        mode[i] = L->h_mode ? L->mode : v_kw(EMBI_KW_match_source);
    }
    embi_value v;
#define LIST_OF(arr) (embi_make_list_kw(a, (arr), n, EMBI_SEP_COMMA, &v), v)
    if (!mask) {
        embi_value col = n && ls[n - 1].h_color ? ls[n - 1].color : (embi_value){0};
        if (!(n && ls[n - 1].h_color)) {
            col.kind = EMBI_V_COLOR;
            col.color.space = EMBI_CS_SRGB;
            col.color.flags = 0;
            col.color.alpha = 0;
        }
        embi_value t;
        embi_sh_put(sh, EMBI_PROP_background_color, &col);
        t = LIST_OF(image);   embi_sh_put(sh, EMBI_PROP_background_image, &t);
        t = LIST_OF(posx);    embi_sh_put(sh, EMBI_PROP_background_position_x, &t);
        t = LIST_OF(posy);    embi_sh_put(sh, EMBI_PROP_background_position_y, &t);
        t = LIST_OF(size);    embi_sh_put(sh, EMBI_PROP_background_size, &t);
        t = LIST_OF(repeat);  embi_sh_put(sh, EMBI_PROP_background_repeat, &t);
        t = LIST_OF(attach);  embi_sh_put(sh, EMBI_PROP_background_attachment, &t);
        t = LIST_OF(origin);  embi_sh_put(sh, EMBI_PROP_background_origin, &t);
        t = LIST_OF(clip);    embi_sh_put(sh, EMBI_PROP_background_clip, &t);
    } else {
        embi_value t;
        t = LIST_OF(image);   embi_sh_put(sh, EMBI_PROP_mask_image, &t);
        t = LIST_OF(mode);    embi_sh_put(sh, EMBI_PROP_mask_mode, &t);
        t = LIST_OF(repeat);  embi_sh_put(sh, EMBI_PROP_mask_repeat, &t);
        t = LIST_OF(pos);     embi_sh_put(sh, EMBI_PROP_mask_position, &t);
        t = LIST_OF(size);    embi_sh_put(sh, EMBI_PROP_mask_size, &t);
        t = LIST_OF(clip);    embi_sh_put(sh, EMBI_PROP_mask_clip, &t);
        t = LIST_OF(origin);  embi_sh_put(sh, EMBI_PROP_mask_origin, &t);
        t = LIST_OF(comp);    embi_sh_put(sh, EMBI_PROP_mask_composite, &t);
    }
#undef LIST_OF
    return embi_sh_finish(sh, out);
}

/* background-position: a comma list of positions split into the x and y lists. */
static uint32_t expand_background_position(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, c, &n);
    embi_value *xs = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *ys = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        embi_position *p = EMBI_NEW(a, embi_position);
        if (!embi_parse_position(a, &segs[i], p) || !cur_done(&segs[i])) {
            return 0;
        }
        xs[i] = axis_val(&p->x);
        ys[i] = axis_val(&p->y);
    }
    embi_value vx, vy;
    embi_make_list_kw(a, xs, n, EMBI_SEP_COMMA, &vx);
    embi_make_list_kw(a, ys, n, EMBI_SEP_COMMA, &vy);
    embi_sh_put(sh, EMBI_PROP_background_position_x, &vx);
    embi_sh_put(sh, EMBI_PROP_background_position_y, &vy);
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ border-image, mask-border */

static uint32_t expand_border_image(embi_sh *sh, embi_cur *c, bool mask, embi_longhand *out)
{
    embi_arena *a = sh->a;
    embi_prop_id p_src = mask ? EMBI_PROP_mask_border_source : EMBI_PROP_border_image_source;
    embi_prop_id p_slice = mask ? EMBI_PROP_mask_border_slice : EMBI_PROP_border_image_slice;
    embi_prop_id p_width = mask ? EMBI_PROP_mask_border_width : EMBI_PROP_border_image_width;
    embi_prop_id p_out = mask ? EMBI_PROP_mask_border_outset : EMBI_PROP_border_image_outset;
    embi_prop_id p_rep = mask ? EMBI_PROP_mask_border_repeat : EMBI_PROP_border_image_repeat;
    bool h_src = false, h_slice = false, h_rep = false, h_mode = false;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        embi_value v;
        if (!h_src && cv_ident(t, "none")) {
            v = v_kw(EMBI_KW_none);
            embi_sh_put(sh, p_src, &v);
            h_src = true;
            c->i++;
        } else if (!h_src && embi_parse_image(a, t, &v)) {
            embi_sh_put(sh, p_src, &v);
            h_src = true;
            c->i++;
        } else if (!h_slice && (t->tok.type == EMBI_CSS_NUMBER || t->tok.type == EMBI_CSS_PERCENTAGE ||
                                cv_ident(t, "fill"))) {
            if (!embi_parse_bi_slice(a, c, &v)) {
                return 0;
            }
            embi_sh_put(sh, p_slice, &v);
            h_slice = true;
            if (cv_delim(cur_peek(c), '/')) {
                c->i++;
                if (!cv_delim(cur_peek(c), '/')) {
                    if (!embi_parse_bi_width(a, c, &v)) {
                        return 0;
                    }
                    embi_sh_put(sh, p_width, &v);
                }
                if (cv_delim(cur_peek(c), '/')) {
                    c->i++;
                    if (!embi_parse_bi_outset(a, c, &v)) {
                        return 0;
                    }
                    embi_sh_put(sh, p_out, &v);
                }
            }
        } else if (!h_rep && embi_parse_bi_repeat(a, c, &v)) {
            embi_sh_put(sh, p_rep, &v);
            h_rep = true;
        } else if (mask && !h_mode && (cv_ident(t, "alpha") || cv_ident(t, "luminance"))) {
            v = v_kw(cv_kw(t));
            embi_sh_put(sh, EMBI_PROP_mask_border_mode, &v);
            h_mode = true;
            c->i++;
        } else {
            return 0;
        }
    }
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ container */

static uint32_t expand_container(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_value name, type;
    if (!embi_parse_prefix(sh->a, EMBI_PROP_container_name, c, &name)) {
        return 0;
    }
    embi_sh_put(sh, EMBI_PROP_container_name, &name);
    if (cv_delim(cur_peek(c), '/')) {
        c->i++;
        if (!embi_parse_prefix(sh->a, EMBI_PROP_container_type, c, &type)) {
            return 0;
        }
        embi_sh_put(sh, EMBI_PROP_container_type, &type);
    }
    if (!cur_done(c)) {
        return 0;
    }
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ transition, animation */

static bool time_arg(embi_arena *a, const embi_css_cv *t, embi_value *out)
{
    return embi_value_parse_types(a, EMBI_T_TIME, t, out) &&
           (out->kind == EMBI_V_TIME || out->kind == EMBI_V_CALC);
}

static bool custom_ident(const embi_css_cv *t)
{
    return cv_any_ident(t) && !embi_ident_reserved(t->tok.value);
}

static embi_value ident_val(const embi_css_cv *t)
{
    embi_value v = {0};
    v.kind = EMBI_V_IDENT;
    v.ident = embi_atom_intern(t->tok.value.ptr, t->tok.value.len);
    return v;
}

static embi_value time_zero(void)
{
    embi_value v = {0};
    v.kind = EMBI_V_TIME;
    v.unit = EMBI_U_S;
    return v;
}

typedef struct tlayer {
    embi_value prop, dur, ease, delay, behavior;
    bool h_prop, h_dur, h_ease, h_delay, h_beh;
} tlayer;

static bool parse_transition_layer(embi_arena *a, embi_cur *s, tlayer *L)
{
    memset(L, 0, sizeof *L);
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        embi_value v;
        if (time_arg(a, t, &v)) {
            if (!L->h_dur) {
                if (v.kind == EMBI_V_TIME && v.num < 0) {
                    return false;
                }
                L->dur = v;
                L->h_dur = true;
            } else if (!L->h_delay) {
                L->delay = v;
                L->h_delay = true;
            } else {
                return false;
            }
            s->i++;
        } else if (!L->h_ease && embi_parse_easing(a, t, &v)) {
            L->ease = v;
            L->h_ease = true;
            s->i++;
        } else if (!L->h_beh && (cv_ident(t, "normal") || cv_ident(t, "allow-discrete"))) {
            L->behavior = v_kw(cv_kw(t));
            L->h_beh = true;
            s->i++;
        } else if (!L->h_prop && cv_ident(t, "none")) {
            L->prop = v_kw(EMBI_KW_none);
            L->h_prop = true;
            s->i++;
        } else if (!L->h_prop && cv_ident(t, "all")) {
            L->prop = v_kw(EMBI_KW_all);
            L->h_prop = true;
            s->i++;
        } else if (!L->h_prop && custom_ident(t)) {
            L->prop = ident_val(t);
            L->h_prop = true;
            s->i++;
        } else {
            return false;
        }
    }
    return true;
}

static uint32_t expand_transition(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, c, &n);
    tlayer *ls = EMBI_NEW_ARRAY(a, tlayer, n);
    embi_value *prop = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *dur = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *ease = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *delay = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *beh = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        if (cur_left(&segs[i]) == 0 || !parse_transition_layer(a, &segs[i], &ls[i])) {
            return 0;
        }
        if (ls[i].h_prop && ls[i].prop.kind == EMBI_V_KEYWORD && ls[i].prop.kw == EMBI_KW_none && n > 1) {
            return 0; /* `none` only as the single transition */
        }
        prop[i] = ls[i].h_prop ? ls[i].prop : v_kw(EMBI_KW_all);
        dur[i] = ls[i].h_dur ? ls[i].dur : time_zero();
        ease[i] = ls[i].h_ease ? ls[i].ease : v_kw(EMBI_KW_ease);
        delay[i] = ls[i].h_delay ? ls[i].delay : time_zero();
        beh[i] = ls[i].h_beh ? ls[i].behavior : v_kw(EMBI_KW_normal);
    }
    embi_value v;
    embi_make_list_kw(a, prop, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_transition_property, &v);
    embi_make_list_kw(a, dur, n, EMBI_SEP_COMMA, &v);   embi_sh_put(sh, EMBI_PROP_transition_duration, &v);
    embi_make_list_kw(a, ease, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_transition_timing_function, &v);
    embi_make_list_kw(a, delay, n, EMBI_SEP_COMMA, &v); embi_sh_put(sh, EMBI_PROP_transition_delay, &v);
    embi_make_list_kw(a, beh, n, EMBI_SEP_COMMA, &v);   embi_sh_put(sh, EMBI_PROP_transition_behavior, &v);
    return embi_sh_finish(sh, out);
}

typedef struct alayer {
    embi_value name, dur, ease, delay, iter, dir, fill, state;
    bool h_name, h_dur, h_ease, h_delay, h_iter, h_dir, h_fill, h_state;
} alayer;

static bool parse_animation_layer(embi_arena *a, embi_cur *s, alayer *L)
{
    memset(L, 0, sizeof *L);
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        embi_value v;
        if (time_arg(a, t, &v)) {
            if (!L->h_dur) {
                if (v.kind == EMBI_V_TIME && v.num < 0) {
                    return false;
                }
                L->dur = v;
                L->h_dur = true;
            } else if (!L->h_delay) {
                L->delay = v;
                L->h_delay = true;
            } else {
                return false;
            }
        } else if (!L->h_ease && embi_parse_easing(a, t, &v)) {
            L->ease = v;
            L->h_ease = true;
        } else if (!L->h_iter && cv_ident(t, "infinite")) {
            L->iter = v_kw(EMBI_KW_infinite);
            L->h_iter = true;
        } else if (!L->h_iter && t->tok.type == EMBI_CSS_NUMBER &&
                   embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_NONNEG, t, &v)) {
            L->iter = v;
            L->h_iter = true;
        } else if (!L->h_dir && (cv_ident(t, "normal") || cv_ident(t, "reverse") ||
                                 cv_ident(t, "alternate") || cv_ident(t, "alternate-reverse"))) {
            L->dir = v_kw(cv_kw(t));
            L->h_dir = true;
        } else if (!L->h_fill && (cv_ident(t, "none") || cv_ident(t, "forwards") ||
                                  cv_ident(t, "backwards") || cv_ident(t, "both"))) {
            L->fill = v_kw(cv_kw(t));
            L->h_fill = true;
        } else if (!L->h_state && (cv_ident(t, "running") || cv_ident(t, "paused"))) {
            L->state = v_kw(cv_kw(t));
            L->h_state = true;
        } else if (!L->h_name && cv_ident(t, "none")) {
            L->name = v_kw(EMBI_KW_none);
            L->h_name = true;
        } else if (!L->h_name && cv_string(t)) {
            L->name = (embi_value){0};
            L->name.kind = EMBI_V_STRING;
            L->name.str = t->tok.value;
            L->h_name = true;
        } else if (!L->h_name && custom_ident(t)) {
            L->name = ident_val(t);
            L->h_name = true;
        } else {
            return false;
        }
        s->i++;
    }
    return true;
}

static uint32_t expand_animation(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, c, &n);
    embi_value *name = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *dur = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *ease = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *delay = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *iter = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *dir = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *fill = EMBI_NEW_ARRAY(a, embi_value, n);
    embi_value *state = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        alayer L;
        if (cur_left(&segs[i]) == 0 || !parse_animation_layer(a, &segs[i], &L)) {
            return 0;
        }
        name[i] = L.h_name ? L.name : v_kw(EMBI_KW_none);
        dur[i] = L.h_dur ? L.dur : time_zero();
        ease[i] = L.h_ease ? L.ease : v_kw(EMBI_KW_ease);
        delay[i] = L.h_delay ? L.delay : time_zero();
        iter[i] = L.h_iter ? L.iter : v_num(1);
        dir[i] = L.h_dir ? L.dir : v_kw(EMBI_KW_normal);
        fill[i] = L.h_fill ? L.fill : v_kw(EMBI_KW_none);
        state[i] = L.h_state ? L.state : v_kw(EMBI_KW_running);
    }
    embi_value v;
    embi_make_list_kw(a, name, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_animation_name, &v);
    embi_make_list_kw(a, dur, n, EMBI_SEP_COMMA, &v);   embi_sh_put(sh, EMBI_PROP_animation_duration, &v);
    embi_make_list_kw(a, ease, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_animation_timing_function, &v);
    embi_make_list_kw(a, delay, n, EMBI_SEP_COMMA, &v); embi_sh_put(sh, EMBI_PROP_animation_delay, &v);
    embi_make_list_kw(a, iter, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_animation_iteration_count, &v);
    embi_make_list_kw(a, dir, n, EMBI_SEP_COMMA, &v);   embi_sh_put(sh, EMBI_PROP_animation_direction, &v);
    embi_make_list_kw(a, fill, n, EMBI_SEP_COMMA, &v);  embi_sh_put(sh, EMBI_PROP_animation_fill_mode, &v);
    embi_make_list_kw(a, state, n, EMBI_SEP_COMMA, &v); embi_sh_put(sh, EMBI_PROP_animation_play_state, &v);
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ grid */

/* An explicit grid-line is a bare custom-ident. */
static bool line_is_ident(const embi_value *v)
{
    return v->kind == EMBI_V_GRID_LINE && !v->gline->span && v->gline->num == 0 && v->gline->name != 0;
}

static uint32_t expand_grid_line_pair(embi_sh *sh, embi_cur *c, embi_prop_id start_p,
                                      embi_prop_id end_p, embi_longhand *out)
{
    embi_value s, e;
    if (!embi_parse_grid_line(sh->a, c, &s)) {
        return 0;
    }
    if (cv_delim(cur_peek(c), '/')) {
        c->i++;
        if (!embi_parse_grid_line(sh->a, c, &e)) {
            return 0;
        }
    } else {
        e = line_is_ident(&s) ? s : v_kw(EMBI_KW_auto);
    }
    if (!cur_done(c)) {
        return 0;
    }
    embi_sh_put(sh, start_p, &s);
    embi_sh_put(sh, end_p, &e);
    return embi_sh_finish(sh, out);
}

static uint32_t expand_grid_area(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_value l[4];
    uint32_t n = 0;
    while (n < 4) {
        if (!embi_parse_grid_line(sh->a, c, &l[n])) {
            return 0;
        }
        n++;
        if (n < 4 && cv_delim(cur_peek(c), '/')) {
            c->i++;
        } else {
            break;
        }
    }
    if (!cur_done(c)) {
        return 0;
    }
    /* row-start / column-start / row-end / column-end */
    if (n < 2) {
        l[1] = line_is_ident(&l[0]) ? l[0] : v_kw(EMBI_KW_auto);
    }
    if (n < 3) {
        l[2] = line_is_ident(&l[0]) ? l[0] : v_kw(EMBI_KW_auto);
    }
    if (n < 4) {
        l[3] = line_is_ident(&l[1]) ? l[1] : v_kw(EMBI_KW_auto);
    }
    embi_sh_put(sh, EMBI_PROP_grid_row_start, &l[0]);
    embi_sh_put(sh, EMBI_PROP_grid_column_start, &l[1]);
    embi_sh_put(sh, EMBI_PROP_grid_row_end, &l[2]);
    embi_sh_put(sh, EMBI_PROP_grid_column_end, &l[3]);
    return embi_sh_finish(sh, out);
}

/* `none` or a track list/subgrid, stopping at `/`. */
static bool tracks_or_none(embi_arena *a, embi_cur *c, embi_value *out)
{
    if (cv_ident(cur_peek(c), "none")) {
        c->i++;
        *out = v_kw(EMBI_KW_none);
        return true;
    }
    return embi_parse_track_list(a, c, out);
}

static bool has_slash(const embi_cur *c)
{
    for (uint32_t i = c->i; i < c->n; i++) {
        if (cv_delim(c->c[i], '/')) {
            return true;
        }
    }
    return false;
}

static bool has_string(const embi_cur *c)
{
    for (uint32_t i = c->i; i < c->n; i++) {
        if (cv_string(c->c[i])) {
            return true;
        }
    }
    return false;
}

/* grid-template: none | <rows> / <columns> | [<line-names>? <string> <track-size>? <line-names>?]+
 * [/ <explicit-track-list>]? */
static bool parse_grid_template(embi_sh *sh, embi_cur *c, embi_value *rows, embi_value *cols,
                                embi_value *areas)
{
    embi_arena *a = sh->a;
    *rows = *cols = *areas = v_kw(EMBI_KW_none);
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "none")) {
        c->i++;
        return true;
    }
    if (!has_string(c)) {
        if (!tracks_or_none(a, c, rows) || !cv_delim(cur_take(c), '/') || !tracks_or_none(a, c, cols)) {
            return false;
        }
        return cur_done(c);
    }
    embi_grid_tracks *tr = EMBI_NEW(a, embi_grid_tracks);
    uint32_t cap = cur_left(c) * 2 + 2;
    embi_grid_item *items = EMBI_NEW_ARRAY(a, embi_grid_item, cap);
    embi_value *strs = EMBI_NEW_ARRAY(a, embi_value, cur_left(c));
    embi_str *raw = EMBI_NEW_ARRAY(a, embi_str, cur_left(c));
    uint32_t ni = 0, ns = 0;
    embi_grid_item pending = {0};
    bool have_pending = false;

    while (!cur_done(c) && !cv_delim(cur_peek(c), '/')) {
        const embi_css_cv *t = cur_peek(c);
        if (t->tok.type == EMBI_CSS_LBRACKET) {
            embi_grid_item nm;
            if (!embi_parse_line_names(a, t, &nm)) {
                return false;
            }
            if (have_pending) {
                embi_atom *m = EMBI_NEW_ARRAY(a, embi_atom, pending.n + nm.n);
                memcpy(m, pending.names, sizeof(embi_atom) * pending.n);
                memcpy(m + pending.n, nm.names, sizeof(embi_atom) * nm.n);
                pending.names = m;
                pending.n += nm.n;
            } else {
                pending = nm;
                have_pending = true;
            }
            c->i++;
        } else if (cv_string(t)) {
            if (have_pending) {
                items[ni++] = pending;
                have_pending = false;
            }
            strs[ns] = (embi_value){0};
            strs[ns].kind = EMBI_V_STRING;
            strs[ns].str = t->tok.value;
            raw[ns] = t->tok.value;
            ns++;
            c->i++;
            embi_grid_item tk = {0};
            tk.kind = EMBI_GI_TRACK;
            const embi_css_cv *nx = cur_peek(c);
            if (nx && !cv_string(nx) && nx->tok.type != EMBI_CSS_LBRACKET && !cv_delim(nx, '/')) {
                if (!embi_parse_track_size(a, nx, &tk.size)) {
                    return false;
                }
                c->i++;
            } else {
                tk.size.kind = EMBI_TS_BREADTH;
                tk.size.min.kind = EMBI_TB_AUTO;
                tk.size.max = tk.size.min;
            }
            items[ni++] = tk;
        } else {
            return false;
        }
    }
    if (have_pending) {
        items[ni++] = pending;
    }
    if (ns == 0 || !embi_grid_areas_valid(a, raw, ns)) {
        return false;
    }
    tr->n = ni;
    tr->items = items;
    *rows = (embi_value){0};
    rows->kind = EMBI_V_TRACKS;
    rows->tracks = tr;
    embi_make_list(a, strs, ns, EMBI_SEP_SPACE, areas);
    if (cv_delim(cur_peek(c), '/')) {
        c->i++;
        if (!embi_parse_track_list(a, c, cols) || cols->tracks->subgrid ||
            cols->tracks->has_auto_repeat) {
            return false;
        }
        for (uint32_t i = 0; i < cols->tracks->n; i++) {
            if (cols->tracks->items[i].kind == EMBI_GI_REPEAT) {
                return false;
            }
        }
    }
    return cur_done(c);
}

static uint32_t put_template(embi_sh *sh, const embi_value *rows, const embi_value *cols,
                             const embi_value *areas, embi_longhand *out)
{
    embi_sh_put(sh, EMBI_PROP_grid_template_rows, rows);
    embi_sh_put(sh, EMBI_PROP_grid_template_columns, cols);
    embi_sh_put(sh, EMBI_PROP_grid_template_areas, areas);
    return embi_sh_finish(sh, out);
}

/* [auto-flow && dense?] then (when auto_after) a grid-auto track list. Returns the flow value. */
static bool parse_auto_flow(embi_arena *a, embi_cur *c, bool column, embi_value *flow)
{
    bool af = false, dense = false;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (!af && cv_ident(t, "auto-flow")) {
            af = true;
        } else if (!dense && cv_ident(t, "dense")) {
            dense = true;
        } else {
            break;
        }
        c->i++;
    }
    if (!af) {
        return false;
    }
    embi_value v[2] = {v_kw(column ? EMBI_KW_column : EMBI_KW_row), v_kw(EMBI_KW_dense)};
    return embi_make_list_kw(a, v, dense ? 2 : 1, EMBI_SEP_SPACE, flow);
}

static uint32_t expand_grid(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    embi_value none = v_kw(EMBI_KW_none);
    const embi_css_cv *first = cur_peek(c);
    /* Is there an `auto-flow` after the slash? (rows / auto-flow ...) */
    bool cols_auto = false;
    for (uint32_t i = c->i; i + 1 < c->n; i++) {
        if (cv_delim(c->c[i], '/')) {
            cols_auto = cv_ident(c->c[i + 1], "auto-flow") || cv_ident(c->c[i + 1], "dense");
            break;
        }
    }
    if (cv_ident(first, "auto-flow") || cv_ident(first, "dense")) {
        /* [auto-flow && dense?] <'grid-auto-rows'>? / <'grid-template-columns'> */
        embi_value flow, autorows, cols;
        if (!parse_auto_flow(a, c, false, &flow)) {
            return 0;
        }
        bool have_auto = false;
        if (!cv_delim(cur_peek(c), '/')) {
            if (!embi_parse_prefix(a, EMBI_PROP_grid_auto_rows, c, &autorows)) {
                return 0;
            }
            have_auto = true;
        }
        if (!cv_delim(cur_take(c), '/') || !tracks_or_none(a, c, &cols) || !cur_done(c)) {
            return 0;
        }
        if (have_auto) embi_sh_put(sh, EMBI_PROP_grid_auto_rows, &autorows);
        embi_sh_put(sh, EMBI_PROP_grid_auto_flow, &flow);
        embi_sh_put(sh, EMBI_PROP_grid_template_rows, &none);
        embi_sh_put(sh, EMBI_PROP_grid_template_columns, &cols);
        embi_sh_put(sh, EMBI_PROP_grid_template_areas, &none);
        return embi_sh_finish(sh, out);
    }
    if (cols_auto && has_slash(c)) {
        /* <'grid-template-rows'> / [auto-flow && dense?] <'grid-auto-columns'>? */
        embi_value rows, flow, autocols;
        if (!tracks_or_none(a, c, &rows) || !cv_delim(cur_take(c), '/') ||
            !parse_auto_flow(a, c, true, &flow)) {
            return 0;
        }
        bool have_auto = false;
        if (!cur_done(c)) {
            if (!embi_parse_prefix(a, EMBI_PROP_grid_auto_columns, c, &autocols) || !cur_done(c)) {
                return 0;
            }
            have_auto = true;
        }
        if (have_auto) embi_sh_put(sh, EMBI_PROP_grid_auto_columns, &autocols);
        embi_sh_put(sh, EMBI_PROP_grid_auto_flow, &flow);
        embi_sh_put(sh, EMBI_PROP_grid_template_rows, &rows);
        embi_sh_put(sh, EMBI_PROP_grid_template_columns, &none);
        embi_sh_put(sh, EMBI_PROP_grid_template_areas, &none);
        return embi_sh_finish(sh, out);
    }
    embi_value rows, cols, areas;
    if (!parse_grid_template(sh, c, &rows, &cols, &areas)) {
        return 0;
    }
    return put_template(sh, &rows, &cols, &areas, out);
}

/* ------------------------------------------------------------------ dispatch */

uint32_t embi_sh_expand_ext(embi_arena *a, embi_prop_id prop, embi_css_slice value,
                            embi_longhand *out, bool *handled)
{
    *handled = true;
    switch (prop) {
    case EMBI_PROP_font:
    case EMBI_PROP_font_variant:
    case EMBI_PROP_font_synthesis:
    case EMBI_PROP_background:
    case EMBI_PROP_background_position:
    case EMBI_PROP_mask:
    case EMBI_PROP_border_image:
    case EMBI_PROP_mask_border:
    case EMBI_PROP_container:
    case EMBI_PROP_transition:
    case EMBI_PROP_animation:
    case EMBI_PROP_grid_row:
    case EMBI_PROP_grid_column:
    case EMBI_PROP_grid_area:
    case EMBI_PROP_grid_template:
    case EMBI_PROP_grid:
        break;
    default:
        *handled = false;
        return 0;
    }
    embi_cur c;
    embi_sh sh;
    if (!embi_cur_make_slice(a, value, &c) || c.n == 0 || !embi_sh_begin(&sh, a, prop)) {
        return 0;
    }
    switch (prop) {
    case EMBI_PROP_font:
        return expand_font(&sh, value, &c, out);
    case EMBI_PROP_font_variant:
        return expand_font_variant(&sh, &c, out);
    case EMBI_PROP_font_synthesis:
        return expand_font_synthesis(&sh, &c, out);
    case EMBI_PROP_background:
        return expand_layers(&sh, &c, false, out);
    case EMBI_PROP_background_position:
        return expand_background_position(&sh, &c, out);
    case EMBI_PROP_mask:
        return expand_layers(&sh, &c, true, out);
    case EMBI_PROP_border_image:
        return expand_border_image(&sh, &c, false, out);
    case EMBI_PROP_mask_border:
        return expand_border_image(&sh, &c, true, out);
    case EMBI_PROP_container:
        return expand_container(&sh, &c, out);
    case EMBI_PROP_transition:
        return expand_transition(&sh, &c, out);
    case EMBI_PROP_animation:
        return expand_animation(&sh, &c, out);
    case EMBI_PROP_grid_row:
        return expand_grid_line_pair(&sh, &c, EMBI_PROP_grid_row_start, EMBI_PROP_grid_row_end, out);
    case EMBI_PROP_grid_column:
        return expand_grid_line_pair(&sh, &c, EMBI_PROP_grid_column_start, EMBI_PROP_grid_column_end, out);
    case EMBI_PROP_grid_area:
        return expand_grid_area(&sh, &c, out);
    case EMBI_PROP_grid_template: {
        embi_value rows, cols, areas;
        if (!parse_grid_template(&sh, &c, &rows, &cols, &areas)) {
            return 0;
        }
        return put_template(&sh, &rows, &cols, &areas, out);
    }
    case EMBI_PROP_grid:
        return expand_grid(&sh, &c, out);
    default:
        *handled = false;
        return 0;
    }
}
