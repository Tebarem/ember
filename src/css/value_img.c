/*
 * <image> values (W2c): url(), linear-/radial-/conic-gradient() and the repeating forms with
 * color interpolation methods, image-set() and cross-fade(). Also the background / mask / border
 * image pieces that build on them: <bg-size>, <repeat-style>, the position grammars, and the
 * border-image-slice / -width / -outset / -repeat bodies.
 */
#include "css/value_int.h"

/* ------------------------------------------------------------------ small helpers */

static bool take_ident(embi_cur *c, const char *lit)
{
    if (cv_ident(cur_peek(c), lit)) {
        c->i++;
        return true;
    }
    return false;
}

static bool wrap_image(embi_arena *a, const embi_image *img, embi_value *out)
{
    (void)a;
    *out = (embi_value){0};
    out->kind = EMBI_V_IMAGE;
    out->image = img;
    return true;
}

/* ------------------------------------------------------------------ color interpolation */

static const struct {
    const char *name;
    uint8_t space;
    bool polar;
} interp_names[] = {
    {"srgb", EMBI_IS_SRGB, false},       {"srgb-linear", EMBI_IS_SRGB_LINEAR, false},
    {"display-p3", EMBI_IS_DISPLAY_P3, false}, {"a98-rgb", EMBI_IS_A98_RGB, false},
    {"prophoto-rgb", EMBI_IS_PROPHOTO_RGB, false}, {"rec2020", EMBI_IS_REC2020, false},
    {"lab", EMBI_IS_LAB, false},         {"oklab", EMBI_IS_OKLAB, false},
    {"xyz", EMBI_IS_XYZ_D65, false},     {"xyz-d50", EMBI_IS_XYZ_D50, false},
    {"xyz-d65", EMBI_IS_XYZ_D65, false}, {"hsl", EMBI_IS_HSL, true},
    {"hwb", EMBI_IS_HWB, true},          {"lch", EMBI_IS_LCH, true},
    {"oklch", EMBI_IS_OKLCH, true},     {"display-p3-linear", EMBI_IS_DISPLAY_P3_LINEAR, false},
};

/* At `in`: `in <space> [<hue method> hue]?`. */
static bool parse_interp(embi_cur *c, uint8_t *space, uint8_t *hue)
{
    if (!take_ident(c, "in")) {
        return false;
    }
    const embi_css_cv *s = cur_take(c);
    if (!s || s->tok.type != EMBI_CSS_IDENT) {
        return false;
    }
    bool found = false;
    bool polar = false;
    for (size_t i = 0; i < sizeof interp_names / sizeof interp_names[0]; i++) {
        if (embi_str_is(s->tok.value, interp_names[i].name)) {
            *space = interp_names[i].space;
            polar = interp_names[i].polar;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    *hue = EMBI_HUE_SHORTER;
    const embi_css_cv *m = cur_peek(c);
    if (polar && cv_any_ident(m) && cv_ident(cur_at(c, 1), "hue")) {
        if (embi_str_is(m->tok.value, "shorter")) {
            *hue = EMBI_HUE_SHORTER;
        } else if (embi_str_is(m->tok.value, "longer")) {
            *hue = EMBI_HUE_LONGER;
        } else if (embi_str_is(m->tok.value, "increasing")) {
            *hue = EMBI_HUE_INCREASING;
        } else if (embi_str_is(m->tok.value, "decreasing")) {
            *hue = EMBI_HUE_DECREASING;
        } else {
            return false;
        }
        c->i += 2;
    }
    return true;
}

/* ------------------------------------------------------------------ gradients */

typedef struct grad_parse {
    embi_arena *a;
    embi_gradient *gr;
} grad_parse;

/* Linear: [<angle>|<zero>|to <side-or-corner>] || <interp>. Returns 1 when the whole segment is a
 * configuration, 0 when it is not (it is then a color stop). */
static int linear_config(grad_parse *gp, embi_cur *s)
{
    embi_gradient *g = gp->gr;
    bool dir = false;
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        if (cv_ident(t, "to") && !dir) {
            s->i++;
            uint8_t sides = 0;
            for (int k = 0; k < 2; k++) {
                const embi_css_cv *w = cur_peek(s);
                uint8_t bit = 0;
                if (cv_ident(w, "top")) {
                    bit = EMBI_SIDE_TOP;
                } else if (cv_ident(w, "bottom")) {
                    bit = EMBI_SIDE_BOTTOM;
                } else if (cv_ident(w, "left")) {
                    bit = EMBI_SIDE_LEFT;
                } else if (cv_ident(w, "right")) {
                    bit = EMBI_SIDE_RIGHT;
                }
                if (bit == 0) {
                    break;
                }
                if ((sides & bit) != 0 ||
                    ((bit & (EMBI_SIDE_TOP | EMBI_SIDE_BOTTOM)) && (sides & (EMBI_SIDE_TOP | EMBI_SIDE_BOTTOM))) ||
                    ((bit & (EMBI_SIDE_LEFT | EMBI_SIDE_RIGHT)) && (sides & (EMBI_SIDE_LEFT | EMBI_SIDE_RIGHT)))) {
                    return 0;
                }
                sides |= bit;
                s->i++;
            }
            if (sides == 0) {
                return 0;
            }
            g->sides = sides;
            dir = true;
        } else if (cv_ident(t, "in") && !g->has_interp) {
            if (!parse_interp(s, &g->interp, &g->hue)) {
                return 0;
            }
            g->has_interp = true;
        } else if (!dir && embi_angle_or_zero(gp->a, t, &g->angle)) {
            s->i++;
            dir = true;
        } else {
            return 0;
        }
    }
    return dir || g->has_interp;
}

static int radial_config(grad_parse *gp, embi_cur *s)
{
    embi_gradient *g = gp->gr;
    int shape = 0; /* 1 circle, 2 ellipse */
    uint8_t size = EMBI_RS_FARTHEST_CORNER;
    bool have_size_kw = false;
    embi_value lens[2];
    uint32_t nl = 0;
    bool at = false;
    bool any = false;
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        if (at) {
            if (cv_ident(t, "in") && !g->has_interp) {
                if (!parse_interp(s, &g->interp, &g->hue)) {
                    return 0;
                }
                g->has_interp = true;
                continue;
            }
            return 0;
        }
        if (cv_ident(t, "circle") && shape == 0) {
            shape = 1;
            s->i++;
        } else if (cv_ident(t, "ellipse") && shape == 0) {
            shape = 2;
            s->i++;
        } else if (cv_any_ident(t) && !have_size_kw && nl == 0 &&
                   (cv_ident(t, "closest-side") || cv_ident(t, "farthest-side") ||
                    cv_ident(t, "closest-corner") || cv_ident(t, "farthest-corner"))) {
            size = cv_ident(t, "closest-side")     ? EMBI_RS_CLOSEST_SIDE
                   : cv_ident(t, "farthest-side")  ? EMBI_RS_FARTHEST_SIDE
                   : cv_ident(t, "closest-corner") ? EMBI_RS_CLOSEST_CORNER
                                                   : EMBI_RS_FARTHEST_CORNER;
            have_size_kw = true;
            s->i++;
        } else if (cv_ident(t, "at")) {
            s->i++;
            if (!embi_parse_position(gp->a, s, &g->pos)) {
                return 0;
            }
            g->has_pos = true;
            at = true;
        } else if (cv_ident(t, "in") && !g->has_interp) {
            if (!parse_interp(s, &g->interp, &g->hue)) {
                return 0;
            }
            g->has_interp = true;
        } else if (!have_size_kw && nl < 2 && embi_value_parse_types(gp->a, EMBI_LP_NN, t, &lens[nl])) {
            nl++;
            s->i++;
        } else {
            return 0;
        }
        any = true;
    }
    if (!any) {
        return 0;
    }
    if (nl > 0) {
        bool ellipse = shape == 2 || (shape == 0 && nl == 2);
        if (shape == 1 && nl != 1) {
            return 0;
        }
        if (ellipse && nl != 2) {
            return 0;
        }
        if (!ellipse && lens[0].kind == EMBI_V_PERCENT) {
            return 0;
        }
        g->ellipse = ellipse;
        g->size = EMBI_RS_EXPLICIT;
        g->radius[0] = lens[0];
        if (nl == 2) {
            g->radius[1] = lens[1];
        }
    } else {
        /* CSS Images 3: with no <radial-shape> and no length, the shape is an ellipse. */
        g->ellipse = shape != 1;
        g->size = size;
    }
    return 1;
}

static int conic_config(grad_parse *gp, embi_cur *s)
{
    embi_gradient *g = gp->gr;
    bool from = false, at = false;
    bool any = false;
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        if (cv_ident(t, "from") && !from) {
            s->i++;
            if (!embi_angle_or_zero(gp->a, cur_peek(s), &g->angle)) {
                return 0;
            }
            s->i++;
            from = true;
        } else if (cv_ident(t, "at") && !at) {
            s->i++;
            if (!embi_parse_position(gp->a, s, &g->pos)) {
                return 0;
            }
            g->has_pos = true;
            at = true;
        } else if (cv_ident(t, "in") && !g->has_interp) {
            if (!parse_interp(s, &g->interp, &g->hue)) {
                return 0;
            }
            g->has_interp = true;
        } else {
            return 0;
        }
        any = true;
    }
    return any;
}

static bool stop_pos(embi_arena *a, bool conic, const embi_css_cv *cv, embi_value *out)
{
    if (conic) {
        return embi_value_parse_types(a, EMBI_T_ANGLE | EMBI_T_PCT, cv, out) ||
               (cv->tok.type == EMBI_CSS_NUMBER && cv->tok.num == 0 &&
                embi_angle_or_zero(a, cv, out));
    }
    return embi_value_parse_types(a, EMBI_LP, cv, out);
}

/* One segment: a color stop (color plus up to two positions) or a color hint (one position). */
static bool parse_stop(embi_arena *a, bool conic, embi_cur *s, embi_color_stop *st, bool *hint)
{
    *st = (embi_color_stop){0};
    *hint = false;
    const embi_css_cv *f = cur_peek(s);
    if (!f) {
        return false;
    }
    if (embi_color_cv(a, f, &st->color)) {
        s->i++;
        uint32_t np = 0;
        while (!cur_done(s) && np < 2) {
            if (!stop_pos(a, conic, cur_peek(s), &st->pos[np])) {
                return false;
            }
            np++;
            s->i++;
        }
        return cur_done(s);
    }
    if (cur_left(s) == 1 && stop_pos(a, conic, f, &st->pos[0])) {
        s->i++;
        *hint = true;
        return true;
    }
    return false;
}

static bool parse_gradient(embi_arena *a, const embi_css_cv *fn, uint8_t kind, bool repeating,
                           embi_value *out)
{
    embi_gradient *g = EMBI_NEW(a, embi_gradient);
    g->kind = kind;
    g->repeating = repeating;
    g->size = EMBI_RS_FARTHEST_CORNER;
    grad_parse gp = {a, g};

    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    if (nseg < 1) {
        return false;
    }
    uint32_t first_stop = 0;
    embi_cur probe = segs[0];
    int is_cfg = 0;
    if (kind == EMBI_GRAD_LINEAR) {
        is_cfg = linear_config(&gp, &probe);
    } else if (kind == EMBI_GRAD_RADIAL) {
        is_cfg = radial_config(&gp, &probe);
    } else {
        is_cfg = conic_config(&gp, &probe);
    }
    if (is_cfg) {
        first_stop = 1;
    } else {
        /* Not a configuration: reset anything the failed attempt may have stored. */
        *g = (embi_gradient){0};
        g->kind = kind;
        g->repeating = repeating;
        g->size = EMBI_RS_FARTHEST_CORNER;
        g->ellipse = kind == EMBI_GRAD_RADIAL; /* the default radial shape is an ellipse */
        if (segs[0].n == 0) {
            return false;
        }
    }
    uint32_t nstops = nseg - first_stop;
    embi_color_stop *stops = EMBI_NEW_ARRAY(a, embi_color_stop, nstops);
    uint32_t ncolor = 0;
    bool prev_hint = true; /* a hint cannot come first */
    for (uint32_t i = 0; i < nstops; i++) {
        embi_cur s = segs[first_stop + i];
        bool hint = false;
        if (!parse_stop(a, kind == EMBI_GRAD_CONIC, &s, &stops[i], &hint)) {
            return false;
        }
        if (hint) {
            if (prev_hint || i + 1 == nstops) {
                return false;
            }
        } else {
            ncolor++;
        }
        prev_hint = hint;
    }
    /* One color stop is a solid fill (CSS Images 4 section 3.5). */
    if (ncolor < 1) {
        return false;
    }
    g->nstops = nstops;
    g->stops = stops;

    embi_image *img = EMBI_NEW(a, embi_image);
    img->kind = EMBI_IMG_GRADIENT;
    img->grad = g;
    return wrap_image(a, img, out);
}

/* ------------------------------------------------------------------ image-set() and cross-fade() */

static bool parse_image_set(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    embi_image_opt *opts = EMBI_NEW_ARRAY(a, embi_image_opt, nseg);
    for (uint32_t i = 0; i < nseg; i++) {
        embi_cur *s = &segs[i];
        const embi_css_cv *f = cur_take(s);
        if (!f) {
            return false;
        }
        embi_value iv;
        if (cv_string(f)) {
            embi_image *u = EMBI_NEW(a, embi_image);
            u->kind = EMBI_IMG_URL;
            u->url = f->tok.value;
            opts[i].img = u;
        } else if (embi_parse_image(a, f, &iv)) {
            opts[i].img = iv.image;
        } else {
            return false;
        }
        opts[i].dppx = 1.0;
        bool have_res = false, have_type = false;
        while (!cur_done(s)) {
            const embi_css_cv *t = cur_take(s);
            embi_value rv;
            if (!have_res && embi_value_parse_types(a, EMBI_T_RES, t, &rv) &&
                rv.kind == EMBI_V_RESOLUTION && rv.num > 0) {
                opts[i].dppx = rv.num;
                have_res = true;
            } else if (!have_type && cv_fn(t, "type")) {
                embi_cur k;
                embi_cur_kids(a, t, &k);
                if (cur_left(&k) != 1 || !cv_string(cur_peek(&k))) {
                    return false;
                }
                opts[i].type = cur_peek(&k)->tok.value;
                have_type = true;
            } else {
                return false;
            }
        }
    }
    embi_image *img = EMBI_NEW(a, embi_image);
    img->kind = EMBI_IMG_IMAGE_SET;
    img->opts = opts;
    img->n = nseg;
    return wrap_image(a, img, out);
}

static bool parse_cross_fade(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    embi_xfade_item *items = EMBI_NEW_ARRAY(a, embi_xfade_item, nseg);
    for (uint32_t i = 0; i < nseg; i++) {
        embi_cur *s = &segs[i];
        items[i].pct = -1;
        bool have_pct = false, have_what = false;
        if (cur_left(s) == 0 || cur_left(s) > 2) {
            return false;
        }
        while (!cur_done(s)) {
            const embi_css_cv *t = cur_take(s);
            embi_value v;
            if (t->tok.type == EMBI_CSS_PERCENTAGE && !have_pct) {
                if (t->tok.num < 0 || t->tok.num > 100) {
                    return false;
                }
                items[i].pct = t->tok.num;
                have_pct = true;
            } else if (!have_what && embi_parse_image(a, t, &v)) {
                items[i].img = v.image;
                have_what = true;
            } else if (!have_what && embi_color_cv(a, t, &items[i].color)) {
                have_what = true;
            } else {
                return false;
            }
        }
        if (!have_what) {
            return false;
        }
    }
    embi_image *img = EMBI_NEW(a, embi_image);
    img->kind = EMBI_IMG_CROSS_FADE;
    img->xf = items;
    img->n = nseg;
    return wrap_image(a, img, out);
}

/* A solid color image, built as a one-stop gradient. */
static embi_image *color_image(embi_arena *a, const embi_color_stop *st)
{
    embi_gradient *g = EMBI_NEW(a, embi_gradient);
    g->kind = EMBI_GRAD_LINEAR;
    g->size = EMBI_RS_FARTHEST_CORNER;
    g->nstops = 1;
    g->stops = st;
    embi_image *img = EMBI_NEW(a, embi_image);
    img->kind = EMBI_IMG_GRADIENT;
    img->grad = g;
    return img;
}

/* image() (CSS Images 4 section 2.2): image(<color>) is a solid color image (a one-stop gradient); image(<url> or
 * <string>, ..., <color>?) is the first source that loads, else the color (none when it is omitted). The image tags and
 * the image-orientation argument are not supported. */
static bool parse_image_color(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    if (nseg == 0) {
        return false;
    }
    embi_image_opt *opts = EMBI_NEW_ARRAY(a, embi_image_opt, nseg);
    uint32_t nopt = 0;
    embi_image *fb = nullptr;
    for (uint32_t i = 0; i < nseg; i++) {
        embi_cur *s = &segs[i];
        const embi_css_cv *f = cur_take(s);
        if (f == nullptr) {
            return false;
        }
        embi_value iv;
        if (cv_string(f) && cur_done(s)) {
            embi_image *u = EMBI_NEW(a, embi_image);
            u->kind = EMBI_IMG_URL;
            u->url = f->tok.value;
            opts[nopt++].img = u;
            continue;
        }
        if ((f->tok.type == EMBI_CSS_URL || (f->tok.type == EMBI_CSS_FUNCTION && embi_str_is(f->tok.value, "url"))) &&
            cur_done(s) && embi_parse_image(a, f, &iv)) {
            opts[nopt++].img = iv.image;
            continue;
        }
        embi_color_stop *st = EMBI_NEW_ARRAY(a, embi_color_stop, 1);
        memset(st, 0, sizeof *st);
        if (i != nseg - 1 || !embi_color_cv(a, f, &st->color) || !cur_done(s)) {
            return false;
        }
        fb = color_image(a, st);
    }
    if (nopt == 0) {
        if (fb == nullptr) {
            return false;
        }
        return wrap_image(a, fb, out);
    }
    for (uint32_t i = 0; i < nopt; i++) {
        opts[i].dppx = 1.0;
    }
    embi_image *img = EMBI_NEW(a, embi_image);
    img->kind = EMBI_IMG_FALLBACKS;
    img->opts = opts;
    img->n = nopt;
    img->fb = fb;
    return wrap_image(a, img, out);
}

bool embi_parse_image(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    if (!cv) {
        return false;
    }
    if (cv->tok.type == EMBI_CSS_URL ||
        (cv->tok.type == EMBI_CSS_FUNCTION && embi_str_is(cv->tok.value, "url"))) {
        embi_value u;
        if (!embi_value_parse_types(a, EMBI_T_URL, cv, &u)) {
            return false;
        }
        embi_image *img = EMBI_NEW(a, embi_image);
        img->kind = EMBI_IMG_URL;
        img->url = u.str;
        return wrap_image(a, img, out);
    }
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    embi_str n = cv->tok.value;
    if (embi_str_is(n, "linear-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_LINEAR, false, out);
    }
    if (embi_str_is(n, "repeating-linear-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_LINEAR, true, out);
    }
    if (embi_str_is(n, "radial-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_RADIAL, false, out);
    }
    if (embi_str_is(n, "repeating-radial-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_RADIAL, true, out);
    }
    if (embi_str_is(n, "conic-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_CONIC, false, out);
    }
    if (embi_str_is(n, "repeating-conic-gradient")) {
        return parse_gradient(a, cv, EMBI_GRAD_CONIC, true, out);
    }
    if (embi_str_is(n, "image-set")) {
        return parse_image_set(a, cv, out);
    }
    if (embi_str_is(n, "cross-fade")) {
        return parse_cross_fade(a, cv, out);
    }
    if (embi_str_is(n, "image")) {
        return parse_image_color(a, cv, out);
    }
    return false;
}

/* light-dark(<image>, <image>) (CSS Color 5 section 4.2, extended to images): a LIGHT_DARK pair of two IMAGE values,
 * which the computed value resolves by the used color scheme (compute.c, as for colors). */
static bool parse_light_dark_image(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    if (nseg != 2) {
        return false;
    }
    embi_value *pair = EMBI_NEW_ARRAY(a, embi_value, 2);
    for (uint32_t i = 0; i < 2; i++) {
        const embi_css_cv *f = cur_take(&segs[i]);
        if (f == nullptr || !cur_done(&segs[i]) || !embi_parse_image(a, f, &pair[i])) {
            return false;
        }
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_LIGHT_DARK;
    out->pair = pair;
    return true;
}

bool embi_gram_image(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *cv = cur_take(c);
    if (cv != nullptr && cv->tok.type == EMBI_CSS_FUNCTION && embi_str_is(cv->tok.value, "light-dark")) {
        return parse_light_dark_image(g->a, cv, out);
    }
    return embi_parse_image(g->a, cv, out);
}

/* ------------------------------------------------------------------ background / mask pieces */

bool embi_parse_bg_size(embi_arena *a, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_peek(c);
    if (cv_ident(t, "cover") || cv_ident(t, "contain")) {
        c->i++;
        *out = v_kw(cv_kw(t));
        return true;
    }
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        const embi_css_cv *x = cur_peek(c);
        if (cv_ident(x, "auto")) {
            v[n] = v_kw(EMBI_KW_auto);
        } else if (!embi_value_parse_types(a, EMBI_LP_NN, x, &v[n])) {
            break;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v_kw(EMBI_KW_auto);
    }
    return embi_make_list(a, v, 2, EMBI_SEP_SPACE, out);
}

bool embi_gram_bg_size(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_bg_size(g->a, c, out);
}

bool embi_parse_repeat_style(embi_arena *a, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_peek(c);
    if (cv_ident(t, "repeat-x") || cv_ident(t, "repeat-y")) {
        c->i++;
        *out = v_kw(cv_kw(t));
        return true;
    }
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        const embi_css_cv *x = cur_peek(c);
        if (!(cv_ident(x, "repeat") || cv_ident(x, "space") || cv_ident(x, "round") ||
              cv_ident(x, "no-repeat"))) {
            break;
        }
        v[n++] = v_kw(cv_kw(x));
        c->i++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    return embi_make_list(a, v, 2, EMBI_SEP_SPACE, out);
}

bool embi_gram_repeat_style(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_repeat_style(g->a, c, out);
}

bool embi_gram_position(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_position *p = EMBI_NEW(g->a, embi_position);
    if (!embi_parse_position(g->a, c, p)) {
        return false;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_POSITION;
    out->pos = p;
    return true;
}

static bool pos_axis(embi_gctx *g, embi_cur *c, embi_value *out, bool horizontal)
{
    embi_pos_axis *ax = EMBI_NEW(g->a, embi_pos_axis);
    if (!embi_parse_pos_axis(g->a, c, horizontal, ax)) {
        return false;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_POS_AXIS;
    out->axis = ax;
    return true;
}

bool embi_gram_pos_axis_x(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return pos_axis(g, c, out, true);
}

bool embi_gram_pos_axis_y(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return pos_axis(g, c, out, false);
}

/* transform-origin: one or two position components (keywords or <length-percentage>, no offsets
 * from an edge), then for the two component forms an optional <length> for z. */
bool embi_gram_transform_origin(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_position *p = EMBI_NEW(g->a, embi_position);
    uint32_t avail = cur_left(c);
    for (uint32_t k = avail > 2 ? 2 : avail; k >= 1; k--) {
        embi_cur probe = {c->c + c->i, k, 0};
        embi_position tmp;
        if (!embi_parse_position(g->a, &probe, &tmp) || probe.i != k) {
            continue;
        }
        *p = tmp;
        uint32_t used = k;
        if (k == 2 && used < avail) {
            const embi_css_cv *zc = c->c[c->i + used];
            embi_value z;
            if (embi_value_parse_types(g->a, EMBI_T_LEN, zc, &z)) {
                p->z = z;
                used++;
            }
        }
        c->i += used;
        *out = (embi_value){0};
        out->kind = EMBI_V_POSITION;
        out->pos = p;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ border-image bodies */

/* Reads up to four values; each is a number or percentage (slice) or the per-property types. */
static uint32_t take_sides(embi_arena *a, embi_cur *c, uint32_t types, bool allow_auto,
                           embi_value out[4])
{
    uint32_t n = 0;
    while (n < 4 && !cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (allow_auto && cv_ident(t, "auto")) {
            out[n] = v_kw(EMBI_KW_auto);
        } else if (!embi_value_parse_types(a, types, t, &out[n])) {
            break;
        }
        c->i++;
        n++;
    }
    return n;
}

static void sides4(const embi_value in[4], uint32_t n, embi_value out[4])
{
    static const uint8_t pick[5][4] = {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3},
    };
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = in[pick[n][i]];
    }
}

bool embi_parse_bi_slice(embi_arena *a, embi_cur *c, embi_value *out)
{
    embi_value v[5];
    embi_value raw[4];
    bool fill = false;
    if (cv_ident(cur_peek(c), "fill")) {
        fill = true;
        c->i++;
    }
    uint32_t n = take_sides(a, c, EMBI_T_NUM | EMBI_T_PCT | EMBI_T_NONNEG, false, raw);
    if (n == 0) {
        return false;
    }
    if (!fill && cv_ident(cur_peek(c), "fill")) {
        fill = true;
        c->i++;
    }
    sides4(raw, n, v);
    uint32_t total = 4;
    if (fill) {
        v[total++] = v_kw(EMBI_KW_fill);
    }
    return embi_make_list(a, v, total, EMBI_SEP_SPACE, out);
}

bool embi_parse_bi_width(embi_arena *a, embi_cur *c, embi_value *out)
{
    embi_value v[4], raw[4];
    uint32_t n = take_sides(a, c, EMBI_T_NUM | EMBI_T_LEN | EMBI_T_PCT | EMBI_T_NONNEG, true, raw);
    if (n == 0) {
        return false;
    }
    sides4(raw, n, v);
    return embi_make_list(a, v, 4, EMBI_SEP_SPACE, out);
}

bool embi_parse_bi_outset(embi_arena *a, embi_cur *c, embi_value *out)
{
    embi_value v[4], raw[4];
    uint32_t n = take_sides(a, c, EMBI_T_NUM | EMBI_T_LEN | EMBI_T_NONNEG, false, raw);
    if (n == 0) {
        return false;
    }
    sides4(raw, n, v);
    return embi_make_list(a, v, 4, EMBI_SEP_SPACE, out);
}

bool embi_parse_bi_repeat(embi_arena *a, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        const embi_css_cv *x = cur_peek(c);
        if (!(cv_ident(x, "stretch") || cv_ident(x, "repeat") || cv_ident(x, "round") ||
              cv_ident(x, "space"))) {
            break;
        }
        v[n++] = v_kw(cv_kw(x));
        c->i++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    return embi_make_list(a, v, 2, EMBI_SEP_SPACE, out);
}

bool embi_gram_bi_slice(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_bi_slice(g->a, c, out);
}
bool embi_gram_bi_width(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_bi_width(g->a, c, out);
}
bool embi_gram_bi_outset(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_bi_outset(g->a, c, out);
}
bool embi_gram_bi_repeat(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_bi_repeat(g->a, c, out);
}
