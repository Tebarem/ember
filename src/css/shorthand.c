/*
 * Shorthand expansion (W2c), part one: the box-model and simple shorthands. Each shorthand
 * expands to every longhand it names, in the order props.txt lists them, with typed values; a
 * longhand the value does not mention takes its initial value. Shapes:
 *   - positional sides (margin, padding, inset, border-width/style/color): one to four values;
 *   - border-radius: one to four horizontal radii, optionally `/` and one to four vertical ones;
 *   - positional pairs (margin-block, gap, overflow, place-*, ...): one or two values, the second
 *     defaulting to the first when it is valid for the second longhand;
 *   - any order (border, outline, text-decoration, columns, flex-flow, ...), where each component
 *     fills the first group it parses for, once per group;
 *   - flex, white-space and list-style have their own rules.
 * font, background, mask, border-image, grid, transition, animation and the font-variant style
 * shorthands are in shorthand_ext.c.
 */
#include "css/value_int.h"

#include <string.h>

/* ------------------------------------------------------------------ collector */

bool embi_sh_begin(embi_sh *sh, embi_arena *a, embi_prop_id prop)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || !(pi->flags & EMBI_PF_SHORTHAND) || pi->sub_n == 0 || pi->sub_n > EMBI_LONGHAND_MAX) {
        return false;
    }
    memset(sh, 0, sizeof *sh);
    sh->a = a;
    sh->prop = prop;
    sh->subs = embi_prop_longhands(prop);
    sh->nsub = pi->sub_n;
    return true;
}

static int sh_index(const embi_sh *sh, embi_prop_id sub)
{
    for (uint32_t i = 0; i < sh->nsub; i++) {
        if (sh->subs[i] == sub) {
            return (int)i;
        }
    }
    return -1;
}

void embi_sh_put(embi_sh *sh, embi_prop_id sub, const embi_value *v)
{
    int i = sh_index(sh, sub);
    if (i >= 0) {
        sh->val[i] = *v;
        sh->set[i] = true;
    }
}

bool embi_sh_isset(const embi_sh *sh, embi_prop_id sub)
{
    int i = sh_index(sh, sub);
    return i >= 0 && sh->set[i];
}

uint32_t embi_sh_finish(embi_sh *sh, embi_longhand *out)
{
    for (uint32_t i = 0; i < sh->nsub; i++) {
        if (!sh->set[i] && !embi_value_initial(sh->a, sh->subs[i], &sh->val[i])) {
            return 0;
        }
        out[i].prop = sh->subs[i];
        out[i].value = sh->val[i];
    }
    return sh->nsub;
}

/* ------------------------------------------------------------------ helpers */

/* Collects the non-whitespace component values. */
static bool collect(embi_arena *a, embi_css_slice value, embi_cur *c)
{
    return embi_cur_make_slice(a, value, c) && c->n > 0;
}

static uint32_t expand_sides(embi_arena *a, const uint16_t *subs, embi_cur *c, embi_longhand *out)
{
    uint32_t nc = c->n;
    if (nc < 1 || nc > 4) {
        return 0;
    }
    embi_value v[4];
    for (uint32_t i = 0; i < nc; i++) {
        if (c->c[i]->tok.type == EMBI_CSS_COMMA || cv_delim(c->c[i], '/') ||
            !embi_value_parse_one(a, subs[0], c->c[i], &v[i])) {
            return 0;
        }
    }
    static const uint8_t pick[5][4] = {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3},
    };
    for (uint32_t s = 0; s < 4; s++) {
        out[s].prop = subs[s];
        out[s].value = v[pick[nc][s]];
    }
    return 4;
}

static uint32_t expand_radius(embi_arena *a, const uint16_t *subs, embi_cur *c, embi_longhand *out)
{
    embi_value h[4], v[4];
    if (!embi_parse_radius_set(a, c, true, h, v) || !cur_done(c)) {
        return 0;
    }
    for (uint32_t i = 0; i < 4; i++) {
        embi_value pair[2] = {h[i], v[i]};
        out[i].prop = subs[i];
        if (!embi_make_list(a, pair, 2, EMBI_SEP_SPACE, &out[i].value)) {
            return 0;
        }
    }
    return 4;
}

/* One or two values; the second falls back to a re-parse of the first with the second longhand's
 * own grammar. */
static uint32_t expand_pair(embi_arena *a, const uint16_t *subs, embi_cur *c, embi_longhand *out)
{
    embi_cur start = *c;
    embi_value v0, v1;
    if (!embi_parse_prefix(a, subs[0], c, &v0)) {
        return 0;
    }
    if (cur_done(c)) {
        embi_cur again = start;
        if (!embi_parse_prefix(a, subs[1], &again, &v1) || !cur_done(&again)) {
            return 0;
        }
    } else if (!embi_parse_prefix(a, subs[1], c, &v1) || !cur_done(c)) {
        return 0;
    }
    out[0] = (embi_longhand){subs[0], v0};
    out[1] = (embi_longhand){subs[1], v1};
    return 2;
}

/* One value for every longhand (marker: marker-start, marker-mid and marker-end). */
static uint32_t expand_all_same(embi_arena *a, const uint16_t *subs, uint32_t n, embi_cur *c, embi_longhand *out)
{
    embi_value v;
    if (!embi_parse_prefix(a, subs[0], c, &v) || !cur_done(c)) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (embi_longhand){subs[i], v};
    }
    return n;
}

/* Components in any order. subs[0 .. ngroups*group) are ngroups groups of `group` longhands that
 * share one grammar (the first of each group is parsed and copied to the rest). A group is set at
 * most once. Remaining longhands, and unset groups, take their initial values. */
static uint32_t expand_any(embi_sh *sh, uint32_t ngroups, uint32_t group, embi_cur *c,
                           embi_longhand *out)
{
    bool set[EMBI_LONGHAND_MAX] = {false};
    embi_value gval[EMBI_LONGHAND_MAX];
    while (!cur_done(c)) {
        bool done = false;
        for (uint32_t g = 0; g < ngroups && !done; g++) {
            if (set[g]) {
                continue;
            }
            embi_cur save = *c;
            embi_value v;
            if (embi_parse_prefix(sh->a, sh->subs[g * group], c, &v)) {
                set[g] = true;
                gval[g] = v;
                done = true;
            } else {
                *c = save;
            }
        }
        if (!done) {
            return 0;
        }
    }
    for (uint32_t g = 0; g < ngroups; g++) {
        if (set[g]) {
            for (uint32_t k = 0; k < group; k++) {
                embi_sh_put(sh, sh->subs[g * group + k], &gval[g]);
            }
        }
    }
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ flex */

static uint32_t expand_flex(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    const uint16_t *subs = sh->subs;
    embi_value grow = v_num(1), shrink = v_num(1);
    embi_value basis = v_pct(0);
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "none")) {
        grow = v_num(0);
        shrink = v_num(0);
        basis = v_kw(EMBI_KW_auto);
    } else if (cur_left(c) == 1 && cv_ident(cur_peek(c), "auto")) {
        basis = v_kw(EMBI_KW_auto);
    } else {
        bool have_grow = false, have_shrink = false, have_basis = false;
        uint32_t last_num = UINT32_MAX;
        while (!cur_done(c)) {
            const embi_css_cv *t = cur_peek(c);
            embi_value v;
            if (t->tok.type == EMBI_CSS_NUMBER && !have_grow &&
                embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_NONNEG, t, &v)) {
                grow = v_num(v.num);
                have_grow = true;
                last_num = c->i;
                c->i++;
            } else if (t->tok.type == EMBI_CSS_NUMBER && have_grow && !have_shrink &&
                       last_num + 1 == c->i &&
                       embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_NONNEG, t, &v)) {
                shrink = v_num(v.num);
                have_shrink = true;
                c->i++;
            } else if (!have_basis && embi_parse_prefix(a, subs[2], c, &basis)) {
                have_basis = true;
            } else {
                return 0;
            }
        }
        if (!have_grow && !have_basis) {
            return 0;
        }
    }
    embi_sh_put(sh, subs[0], &grow);
    embi_sh_put(sh, subs[1], &shrink);
    embi_sh_put(sh, subs[2], &basis);
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ line-clamp */

/* line-clamp: none | [<integer [1,inf]> || auto || <block-ellipsis>] -webkit-legacy?
 * -webkit-line-clamp: none | <integer [1,inf]> (CSS Overflow 4 sections 5.1 and 5.1.1). The shorthands set
 * max-lines, block-ellipsis and continue; a value that is given means continue: collapse (-webkit-legacy for the
 * prefixed property and the keyword) and, without an explicit block-ellipsis, block-ellipsis: ellipsis. */
static uint32_t expand_line_clamp(embi_sh *sh, embi_cur *c, bool webkit, embi_longhand *out)
{
    embi_arena *a = sh->a;
    if (cur_left(c) == 1 && cv_ident(cur_peek(c), "none")) {
        return embi_sh_finish(sh, out);
    }
    embi_value iv = {0}, ell = {0};
    bool have_int = false, have_auto = false, have_ell = false, legacy = webkit;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        embi_value v;
        if (!webkit && cv_ident(t, "-webkit-legacy")) {
            if (cur_left(c) != 1) {
                return 0;
            }
            legacy = true;
        } else if (!have_int && embi_scalar(a, EMBI_T_INT | EMBI_T_POSINT, t, &v)) {
            iv = v;
            have_int = true;
        } else if (!webkit && !have_auto && cv_ident(t, "auto")) {
            have_auto = true;
        } else if (!webkit && !have_ell && (cv_string(t) || cv_ident(t, "no-ellipsis") || cv_ident(t, "ellipsis"))) {
            if (cv_string(t)) {
                ell = (embi_value){0};
                ell.kind = EMBI_V_STRING;
                ell.str = t->tok.value;
            } else {
                ell = v_kw(cv_ident(t, "ellipsis") ? EMBI_KW_ellipsis : EMBI_KW_no_ellipsis);
            }
            have_ell = true;
        } else {
            return 0;
        }
        c->i++;
    }
    if (webkit && !have_int) {
        return 0;
    }
    if (!have_int && !have_auto && !have_ell) {
        return 0;
    }
    if (have_int && have_auto) {
        embi_value l[2] = {iv, v_kw(EMBI_KW_auto)};
        embi_value m;
        if (!embi_make_list(a, l, 2, EMBI_SEP_SPACE, &m)) {
            return 0;
        }
        embi_sh_put(sh, EMBI_PROP_max_lines, &m);
    } else if (have_int) {
        embi_sh_put(sh, EMBI_PROP_max_lines, &iv);
    } else {
        embi_value m = v_kw(EMBI_KW_auto);
        embi_sh_put(sh, EMBI_PROP_max_lines, &m);
    }
    if (!have_ell) {
        ell = v_kw(EMBI_KW_ellipsis);
    }
    embi_sh_put(sh, EMBI_PROP_block_ellipsis, &ell);
    embi_value cont = v_kw(legacy ? EMBI_KW__webkit_legacy : EMBI_KW_collapse);
    embi_sh_put(sh, EMBI_PROP_continue, &cont);
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ white-space, list-style */

static uint32_t expand_white_space(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    static const struct {
        const char *name;
        uint16_t collapse;
        uint16_t wrap;
    } map[] = {
        {"normal", EMBI_KW_collapse, EMBI_KW_wrap},
        {"pre", EMBI_KW_preserve, EMBI_KW_nowrap},
        {"pre-wrap", EMBI_KW_preserve, EMBI_KW_wrap},
        {"pre-line", EMBI_KW_preserve_breaks, EMBI_KW_wrap},
        {"nowrap", EMBI_KW_collapse, EMBI_KW_nowrap},
        {"break-spaces", EMBI_KW_break_spaces, EMBI_KW_wrap},
    };
    if (cur_left(c) == 1 && cv_any_ident(cur_peek(c))) {
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (cv_ident(cur_peek(c), map[i].name)) {
                embi_value v = v_kw(map[i].collapse);
                embi_value w = v_kw(map[i].wrap);
                embi_sh_put(sh, EMBI_PROP_white_space_collapse, &v);
                embi_sh_put(sh, EMBI_PROP_text_wrap_mode, &w);
                return embi_sh_finish(sh, out);
            }
        }
    }
    return expand_any(sh, 2, 1, c, out);
}

static uint32_t expand_list_style(embi_sh *sh, embi_cur *c, embi_longhand *out)
{
    embi_arena *a = sh->a;
    embi_value pos = {0}, img = {0}, type = {0};
    bool have_pos = false, have_img = false, have_type = false;
    uint32_t nones = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        embi_cur save = *c;
        if (cv_ident(t, "none")) {
            nones++;
            c->i++;
        } else if (!have_pos && embi_parse_prefix(a, EMBI_PROP_list_style_position, c, &pos)) {
            have_pos = true;
        } else if (!have_img && embi_parse_prefix(a, EMBI_PROP_list_style_image, c, &img)) {
            have_img = true;
        } else if (!have_type && embi_parse_prefix(a, EMBI_PROP_list_style_type, c, &type)) {
            have_type = true;
        } else {
            *c = save;
            return 0;
        }
    }
    if (nones > 2) {
        return 0;
    }
    uint32_t free_slots = (have_img ? 0 : 1) + (have_type ? 0 : 1);
    if (nones > free_slots) {
        return 0;
    }
    if (nones == 1 && free_slots == 2) {
        img = v_kw(EMBI_KW_none);
        type = v_kw(EMBI_KW_none);
        have_img = have_type = true;
    } else if (nones == 1) {
        if (!have_img) {
            img = v_kw(EMBI_KW_none);
            have_img = true;
        } else {
            type = v_kw(EMBI_KW_none);
            have_type = true;
        }
    } else if (nones == 2) {
        img = v_kw(EMBI_KW_none);
        type = v_kw(EMBI_KW_none);
        have_img = have_type = true;
    }
    if (have_pos) {
        embi_sh_put(sh, EMBI_PROP_list_style_position, &pos);
    }
    if (have_img) {
        embi_sh_put(sh, EMBI_PROP_list_style_image, &img);
    }
    if (have_type) {
        embi_sh_put(sh, EMBI_PROP_list_style_type, &type);
    }
    return embi_sh_finish(sh, out);
}

/* ------------------------------------------------------------------ entry */

uint32_t embi_css_expand_shorthand(embi_arena *a, embi_prop_id prop, embi_css_slice value,
                                   embi_longhand *out, uint32_t cap)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || !(pi->flags & EMBI_PF_SHORTHAND) || pi->sub_n == 0 || cap < pi->sub_n) {
        return 0;
    }
    const uint16_t *subs = embi_prop_longhands(prop);
    bool handled = false;
    uint32_t n = embi_sh_expand_ext(a, prop, value, out, &handled);
    if (handled) {
        return n;
    }
    embi_cur c;
    if (!collect(a, value, &c)) {
        return 0;
    }
    embi_sh sh;
    if (!embi_sh_begin(&sh, a, prop)) {
        return 0;
    }
    switch (prop) {
    case EMBI_PROP_margin:
    case EMBI_PROP_padding:
    case EMBI_PROP_inset:
    case EMBI_PROP_scroll_margin:
    case EMBI_PROP_scroll_padding:
    case EMBI_PROP_border_width:
    case EMBI_PROP_border_style:
    case EMBI_PROP_border_color:
        return expand_sides(a, subs, &c, out);
    case EMBI_PROP_border_radius:
        return expand_radius(a, subs, &c, out);
    case EMBI_PROP_margin_block:
    case EMBI_PROP_margin_inline:
    case EMBI_PROP_padding_block:
    case EMBI_PROP_padding_inline:
    case EMBI_PROP_inset_block:
    case EMBI_PROP_inset_inline:
    case EMBI_PROP_scroll_margin_block:
    case EMBI_PROP_scroll_margin_inline:
    case EMBI_PROP_scroll_padding_block:
    case EMBI_PROP_scroll_padding_inline:
    case EMBI_PROP_gap:
    case EMBI_PROP_overflow:
    case EMBI_PROP_place_content:
    case EMBI_PROP_place_items:
    case EMBI_PROP_place_self:
    case EMBI_PROP_border_block_width:
    case EMBI_PROP_border_block_style:
    case EMBI_PROP_border_block_color:
    case EMBI_PROP_border_inline_width:
    case EMBI_PROP_border_inline_style:
    case EMBI_PROP_border_inline_color:
    case EMBI_PROP_contain_intrinsic_size:
    case EMBI_PROP_overscroll_behavior:
        return expand_pair(a, subs, &c, out);
    case EMBI_PROP_marker:
        return expand_all_same(a, subs, pi->sub_n, &c, out);
    case EMBI_PROP_flex:
        return expand_flex(&sh, &c, out);
    case EMBI_PROP_white_space:
        return expand_white_space(&sh, &c, out);
    case EMBI_PROP_list_style:
        return expand_list_style(&sh, &c, out);
    case EMBI_PROP_line_clamp:
        return expand_line_clamp(&sh, &c, false, out);
    case EMBI_PROP__webkit_line_clamp:
        return expand_line_clamp(&sh, &c, true, out);
    case EMBI_PROP_border:
        return expand_any(&sh, 3, 4, &c, out); /* width, style, color; border-image resets */
    case EMBI_PROP_border_block:
    case EMBI_PROP_border_inline:
        return expand_any(&sh, 3, 2, &c, out);
    case EMBI_PROP_border_top:
    case EMBI_PROP_border_right:
    case EMBI_PROP_border_bottom:
    case EMBI_PROP_border_left:
    case EMBI_PROP_border_block_start:
    case EMBI_PROP_border_block_end:
    case EMBI_PROP_border_inline_start:
    case EMBI_PROP_border_inline_end:
    case EMBI_PROP_outline:
    case EMBI_PROP_text_decoration:
    case EMBI_PROP_text_emphasis:
    case EMBI_PROP_flex_flow:
    case EMBI_PROP_column_rule:
    case EMBI_PROP_columns:
    case EMBI_PROP_text_wrap:
        return expand_any(&sh, pi->sub_n, 1, &c, out);
    default:
        return 0;
    }
}
