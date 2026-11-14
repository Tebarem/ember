/*
 * Effect value grammars (W2c): <transform-list> (and the individual translate / rotate / scale
 * properties), <filter-value-list>, <shadow> lists, <basic-shape> for clip-path (inset, circle,
 * ellipse, polygon, path, xywh, rect) and <easing-function> (cubic-bezier, steps, linear()).
 */
#include "css/value_int.h"

#include <math.h>

/* ------------------------------------------------------------------ transform functions */

typedef struct xf_def {
    const char *name;
    uint8_t fn;
    const char *spec; /* one letter per argument: n number, s number|percentage, p length-percentage,
                       * l length, a angle|zero, P non-negative length|none */
    uint8_t min;
} xf_def;

static const xf_def xf_defs[] = {
    {"matrix", EMBI_XF_MATRIX, "nnnnnn", 6},
    {"matrix3d", EMBI_XF_MATRIX3D, "nnnnnnnnnnnnnnnn", 16},
    {"translate", EMBI_XF_TRANSLATE, "pp", 1},
    {"translatex", EMBI_XF_TRANSLATEX, "p", 1},
    {"translatey", EMBI_XF_TRANSLATEY, "p", 1},
    {"translatez", EMBI_XF_TRANSLATEZ, "l", 1},
    {"translate3d", EMBI_XF_TRANSLATE3D, "ppl", 3},
    {"scale", EMBI_XF_SCALE, "ss", 1},
    {"scalex", EMBI_XF_SCALEX, "s", 1},
    {"scaley", EMBI_XF_SCALEY, "s", 1},
    {"scalez", EMBI_XF_SCALEZ, "s", 1},
    {"scale3d", EMBI_XF_SCALE3D, "sss", 3},
    {"rotate", EMBI_XF_ROTATE, "a", 1},
    {"rotatex", EMBI_XF_ROTATEX, "a", 1},
    {"rotatey", EMBI_XF_ROTATEY, "a", 1},
    {"rotatez", EMBI_XF_ROTATEZ, "a", 1},
    {"rotate3d", EMBI_XF_ROTATE3D, "nnna", 4},
    {"skew", EMBI_XF_SKEW, "aa", 1},
    {"skewx", EMBI_XF_SKEWX, "a", 1},
    {"skewy", EMBI_XF_SKEWY, "a", 1},
    {"perspective", EMBI_XF_PERSPECTIVE, "P", 1},
};

/* A number or percentage as a scale factor (percentages are divided by 100). */
static bool scale_arg(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    embi_value v;
    if (!embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_PCT, cv, &v)) {
        return false;
    }
    if (v.kind == EMBI_V_PERCENT) {
        *out = v_num(v.num / 100.0);
    } else if (v.kind == EMBI_V_NUMBER || v.kind == EMBI_V_INTEGER) {
        *out = v_num(v.num);
    } else {
        *out = v; /* calc() */
    }
    return true;
}

static bool xf_arg(embi_arena *a, char cls, const embi_css_cv *cv, embi_value *out)
{
    switch (cls) {
    case 'n':
        return embi_value_parse_types(a, EMBI_T_NUM, cv, out);
    case 's':
        return scale_arg(a, cv, out);
    case 'p':
        return embi_value_parse_types(a, EMBI_LP, cv, out);
    case 'l':
        return embi_value_parse_types(a, EMBI_T_LEN, cv, out);
    case 'a':
        return embi_angle_or_zero(a, cv, out);
    case 'P':
        if (cv_ident(cv, "none")) {
            *out = v_kw(EMBI_KW_none);
            return true;
        }
        return embi_value_parse_types(a, EMBI_LEN_NN, cv, out);
    default:
        return false;
    }
}

static bool parse_xform(embi_arena *a, const embi_css_cv *fn, embi_xform *out)
{
    const xf_def *def = nullptr;
    for (size_t i = 0; i < sizeof xf_defs / sizeof xf_defs[0]; i++) {
        if (embi_str_is(fn->tok.value, xf_defs[i].name)) {
            def = &xf_defs[i];
            break;
        }
    }
    if (!def) {
        return false;
    }
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    uint32_t max = (uint32_t)strlen(def->spec);
    if (nseg < def->min || nseg > max) {
        return false;
    }
    embi_value *vals = EMBI_NEW_ARRAY(a, embi_value, nseg);
    for (uint32_t i = 0; i < nseg; i++) {
        if (cur_left(&segs[i]) != 1 || !xf_arg(a, def->spec[i], cur_peek(&segs[i]), &vals[i])) {
            return false;
        }
    }
    out->fn = def->fn;
    out->n = (uint8_t)nseg;
    out->args = vals;
    return true;
}

bool embi_gram_transform(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_xform *fns = EMBI_NEW_ARRAY(g->a, embi_xform, n);
    uint32_t k = 0;
    while (!cur_done(c)) {
        const embi_css_cv *cv = cur_take(c);
        if (cv->tok.type != EMBI_CSS_FUNCTION || !parse_xform(g->a, cv, &fns[k])) {
            return false;
        }
        k++;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_TRANSFORM;
    out->xf.p = fns;
    out->xf.n = k;
    return true;
}

static bool one_xform(embi_arena *a, uint8_t fn, const embi_value *args, uint8_t n,
                      embi_value *out)
{
    embi_xform *x = EMBI_NEW(a, embi_xform);
    embi_value *vals = EMBI_NEW_ARRAY(a, embi_value, n);
    memcpy(vals, args, sizeof(embi_value) * n);
    x->fn = fn;
    x->n = n;
    x->args = vals;
    *out = (embi_value){0};
    out->kind = EMBI_V_TRANSFORM;
    out->xf.p = x;
    out->xf.n = 1;
    return true;
}

/* translate: <length-percentage> [<length-percentage> <length>?]? */
bool embi_gram_translate(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[3] = {v_px(0), v_px(0), v_px(0)};
    if (!embi_value_parse_types(g->a, EMBI_LP, cur_take(c), &v[0])) {
        return false;
    }
    if (!cur_done(c)) {
        if (!embi_value_parse_types(g->a, EMBI_LP, cur_take(c), &v[1])) {
            return false;
        }
        if (!cur_done(c) && !embi_value_parse_types(g->a, EMBI_T_LEN, cur_take(c), &v[2])) {
            return false;
        }
    }
    return one_xform(g->a, EMBI_XF_TRANSLATE3D, v, 3, out);
}

/* scale: [<number> | <percentage>]{1,3} */
bool embi_gram_scale(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[3];
    uint32_t n = 0;
    while (n < 3 && !cur_done(c)) {
        if (!scale_arg(g->a, cur_peek(c), &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    if (n < 3) {
        v[2] = v_num(1);
    }
    return one_xform(g->a, EMBI_XF_SCALE3D, v, 3, out);
}

/* rotate: [x | y | z | <number>{3}]? && <angle> */
bool embi_gram_rotate(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value axis[3] = {v_num(0), v_num(0), v_num(1)};
    embi_value angle = {0};
    bool have_axis = false, have_angle = false;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (!have_axis && cv_any_ident(t) &&
            (cv_ident(t, "x") || cv_ident(t, "y") || cv_ident(t, "z"))) {
            axis[0] = v_num(cv_ident(t, "x") ? 1 : 0);
            axis[1] = v_num(cv_ident(t, "y") ? 1 : 0);
            axis[2] = v_num(cv_ident(t, "z") ? 1 : 0);
            have_axis = true;
            c->i++;
        } else if (!have_axis && t->tok.type == EMBI_CSS_NUMBER && cur_left(c) >= 3 &&
                   cur_at(c, 1)->tok.type == EMBI_CSS_NUMBER &&
                   cur_at(c, 2)->tok.type == EMBI_CSS_NUMBER) {
            for (int k = 0; k < 3; k++) {
                if (!embi_value_parse_types(g->a, EMBI_T_NUM, cur_peek(c), &axis[k])) {
                    return false;
                }
                c->i++;
            }
            have_axis = true;
        } else if (!have_angle && embi_angle_or_zero(g->a, t, &angle)) {
            have_angle = true;
            c->i++;
        } else {
            return false;
        }
    }
    if (!have_angle) {
        return false;
    }
    embi_value args[4] = {axis[0], axis[1], axis[2], angle};
    return one_xform(g->a, EMBI_XF_ROTATE3D, args, 4, out);
}

/* ------------------------------------------------------------------ shadows */

/* One shadow layer from a segment: [inset]? <color>? <length>{2,4} in any arrangement where the
 * lengths are consecutive. text-shadow takes 2 or 3 lengths and no inset. */
static bool parse_shadow(embi_arena *a, embi_cur *s, bool text, embi_shadow *sh)
{
    *sh = (embi_shadow){0};
    sh->color = (embi_value){0};
    sh->color.kind = EMBI_V_COLOR;
    sh->color.color.space = EMBI_CS_CURRENT;
    sh->color.color.alpha = 1.0;
    sh->blur = v_px(0);
    sh->spread = v_px(0);
    bool have_color = false, have_inset = false, have_lens = false;
    while (!cur_done(s)) {
        const embi_css_cv *t = cur_peek(s);
        embi_value v;
        if (cv_ident(t, "inset") && !text && !have_inset) {
            have_inset = true;
            sh->inset = true;
            s->i++;
        } else if (!have_lens && embi_value_parse_types(a, EMBI_T_LEN, t, &v)) {
            embi_value l[4];
            uint32_t n = 0;
            while (n < 4 && !cur_done(s) &&
                   embi_value_parse_types(a, EMBI_T_LEN, cur_peek(s), &l[n])) {
                n++;
                s->i++;
            }
            if (n < 2 || (text && n > 3)) {
                return false;
            }
            if (n >= 3 && l[2].kind == EMBI_V_LENGTH && l[2].num < 0) {
                return false; /* negative blur radius */
            }
            sh->x = l[0];
            sh->y = l[1];
            if (n >= 3) {
                sh->blur = l[2];
            }
            if (n == 4) {
                sh->spread = l[3];
            }
            have_lens = true;
        } else if (!have_color && embi_color_cv(a, t, &sh->color)) {
            have_color = true;
            s->i++;
        } else {
            return false;
        }
    }
    return have_lens;
}

static bool shadow_list(embi_gctx *g, embi_cur *c, bool text, embi_value *out)
{
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(g->a, c, &n);
    embi_shadow *sh = EMBI_NEW_ARRAY(g->a, embi_shadow, n);
    for (uint32_t i = 0; i < n; i++) {
        if (!parse_shadow(g->a, &segs[i], text, &sh[i])) {
            return false;
        }
    }
    c->i = c->n;
    *out = (embi_value){0};
    out->kind = EMBI_V_SHADOWS;
    out->shadows.p = sh;
    out->shadows.n = n;
    return true;
}

bool embi_gram_box_shadow(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return shadow_list(g, c, false, out);
}

bool embi_gram_text_shadow(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return shadow_list(g, c, true, out);
}

/* ------------------------------------------------------------------ filters */

static bool filter_amount(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    embi_value v;
    if (!embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_PCT | EMBI_T_NONNEG, cv, &v)) {
        return false;
    }
    if (v.kind == EMBI_V_PERCENT) {
        *out = v_num(v.num / 100.0);
    } else if (v.kind == EMBI_V_NUMBER || v.kind == EMBI_V_INTEGER) {
        *out = v_num(v.num);
    } else {
        *out = v;
    }
    return true;
}

static bool parse_filter(embi_arena *a, const embi_css_cv *cv, embi_filter *f)
{
    *f = (embi_filter){0};
    if (cv->tok.type == EMBI_CSS_URL || cv_fn(cv, "url")) {
        if (!embi_value_parse_types(a, EMBI_T_URL, cv, &f->arg)) {
            return false;
        }
        f->fn = EMBI_FILTER_URL;
        return true;
    }
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    static const struct {
        const char *name;
        uint8_t fn;
    } amount[] = {
        {"brightness", EMBI_FILTER_BRIGHTNESS}, {"contrast", EMBI_FILTER_CONTRAST},
        {"grayscale", EMBI_FILTER_GRAYSCALE},   {"invert", EMBI_FILTER_INVERT},
        {"opacity", EMBI_FILTER_OPACITY},       {"saturate", EMBI_FILTER_SATURATE},
        {"sepia", EMBI_FILTER_SEPIA},
    };
    embi_cur args;
    embi_cur_kids(a, cv, &args);
    uint32_t left = cur_left(&args);
    if (embi_str_is(cv->tok.value, "blur")) {
        f->fn = EMBI_FILTER_BLUR;
        f->arg = v_px(0);
        if (left == 0) {
            return true;
        }
        return left == 1 && embi_value_parse_types(a, EMBI_LEN_NN, cur_peek(&args), &f->arg);
    }
    if (embi_str_is(cv->tok.value, "hue-rotate")) {
        f->fn = EMBI_FILTER_HUE_ROTATE;
        f->arg = (embi_value){0};
        f->arg.kind = EMBI_V_ANGLE;
        f->arg.unit = EMBI_U_DEG;
        if (left == 0) {
            return true;
        }
        return left == 1 && embi_angle_or_zero(a, cur_peek(&args), &f->arg);
    }
    if (embi_str_is(cv->tok.value, "drop-shadow")) {
        embi_shadow *sh = EMBI_NEW(a, embi_shadow);
        if (!parse_shadow(a, &args, true, sh) || sh->blur.kind == EMBI_V_NONE ||
            (sh->spread.kind == EMBI_V_LENGTH && sh->spread.num != 0)) {
            return false;
        }
        f->fn = EMBI_FILTER_DROP_SHADOW;
        f->shadow = sh;
        return true;
    }
    for (size_t i = 0; i < sizeof amount / sizeof amount[0]; i++) {
        if (embi_str_is(cv->tok.value, amount[i].name)) {
            f->fn = amount[i].fn;
            f->arg = v_num(1);
            if (left == 0) {
                return true;
            }
            return left == 1 && filter_amount(a, cur_peek(&args), &f->arg);
        }
    }
    return false;
}

bool embi_gram_filter(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_filter *fs = EMBI_NEW_ARRAY(g->a, embi_filter, n);
    uint32_t k = 0;
    while (!cur_done(c)) {
        if (!parse_filter(g->a, cur_take(c), &fs[k])) {
            return false;
        }
        k++;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_FILTERS;
    out->filters.p = fs;
    out->filters.n = k;
    return true;
}

/* ------------------------------------------------------------------ easing functions */

static bool arg_number(embi_arena *a, embi_cur *s, double *out)
{
    embi_value v;
    if (cur_left(s) != 1 || !embi_value_parse_types(a, EMBI_T_NUM, cur_peek(s), &v) ||
        (v.kind != EMBI_V_NUMBER && v.kind != EMBI_V_INTEGER)) {
        return false;
    }
    *out = v.num;
    return true;
}

static bool parse_linear_fn(embi_arena *a, const embi_css_cv *cv, embi_easing *e)
{
    embi_cur args;
    embi_cur_kids(a, cv, &args);
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, &args, &nseg);
    if (nseg < 2) {
        return false;
    }
    embi_easing_pt *pts = EMBI_NEW_ARRAY(a, embi_easing_pt, nseg * 2);
    bool *known = EMBI_NEW_ARRAY(a, bool, nseg * 2);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        embi_cur *s = &segs[i];
        embi_value v;
        double out = 0;
        if (cur_left(s) < 1 || cur_left(s) > 3 ||
            !embi_value_parse_types(a, EMBI_T_NUM, cur_peek(s), &v) ||
            (v.kind != EMBI_V_NUMBER && v.kind != EMBI_V_INTEGER)) {
            return false;
        }
        out = v.num;
        s->i++;
        uint32_t np = 0;
        double in[2] = {0, 0};
        while (!cur_done(s)) {
            const embi_css_cv *t = cur_take(s);
            if (np >= 2 || t->tok.type != EMBI_CSS_PERCENTAGE) {
                return false;
            }
            in[np++] = t->tok.num / 100.0;
        }
        if (np == 0) {
            pts[n].out = out;
            known[n++] = false;
        } else {
            for (uint32_t k = 0; k < np; k++) {
                pts[n].in = in[k];
                pts[n].out = out;
                known[n++] = true;
            }
        }
    }
    /* Normalize: first and last inputs, running maximum, then fill the gaps linearly. */
    if (!known[0]) {
        pts[0].in = 0;
        known[0] = true;
    }
    if (!known[n - 1]) {
        pts[n - 1].in = pts[n - 1].in > 1.0 ? pts[n - 1].in : 1.0;
        known[n - 1] = true;
    }
    double run = -INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        if (known[i]) {
            if (pts[i].in < run) {
                pts[i].in = run;
            }
            run = pts[i].in;
        }
    }
    for (uint32_t i = 1; i + 1 < n; i++) {
        if (known[i]) {
            continue;
        }
        uint32_t lo = i - 1;
        uint32_t hi = i;
        while (!known[hi]) {
            hi++;
        }
        for (uint32_t k = i; k < hi; k++) {
            pts[k].in = pts[lo].in + (pts[hi].in - pts[lo].in) * (double)(k - lo) / (double)(hi - lo);
            known[k] = true;
        }
    }
    e->kind = EMBI_EASE_LINEAR_FN;
    e->npts = n;
    e->pts = pts;
    return true;
}

bool embi_parse_easing(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    if (!cv) {
        return false;
    }
    *out = (embi_value){0};
    if (cv->tok.type == EMBI_CSS_IDENT) {
        embi_str s = cv->tok.value;
        if (embi_str_is(s, "ease") || embi_str_is(s, "linear") || embi_str_is(s, "ease-in") ||
            embi_str_is(s, "ease-out") || embi_str_is(s, "ease-in-out") ||
            embi_str_is(s, "step-start") || embi_str_is(s, "step-end")) {
            *out = v_kw(embi_kw_lookup(s));
            return true;
        }
        return false;
    }
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    embi_easing *e = EMBI_NEW(a, embi_easing);
    embi_cur args;
    embi_cur_kids(a, cv, &args);
    if (embi_str_is(cv->tok.value, "linear")) {
        if (!parse_linear_fn(a, cv, e)) {
            return false;
        }
    } else if (embi_str_is(cv->tok.value, "cubic-bezier")) {
        uint32_t n = 0;
        embi_cur *segs = embi_cur_split(a, &args, &n);
        if (n != 4) {
            return false;
        }
        for (int i = 0; i < 4; i++) {
            if (!arg_number(a, &segs[i], &e->c[i])) {
                return false;
            }
        }
        if (e->c[0] < 0 || e->c[0] > 1 || e->c[2] < 0 || e->c[2] > 1) {
            return false;
        }
        e->kind = EMBI_EASE_CUBIC;
    } else if (embi_str_is(cv->tok.value, "steps")) {
        uint32_t n = 0;
        embi_cur *segs = embi_cur_split(a, &args, &n);
        if (n < 1 || n > 2) {
            return false;
        }
        embi_value sv;
        if (cur_left(&segs[0]) != 1 || !embi_value_parse_types(a, EMBI_T_INT, cur_peek(&segs[0]), &sv) ||
            sv.kind != EMBI_V_INTEGER || sv.num < 1 || sv.num > 1e6) {
            return false;
        }
        e->steps = (uint32_t)sv.num;
        e->jump = EMBI_JUMP_END;
        if (n == 2) {
            const embi_css_cv *j = cur_peek(&segs[1]);
            if (cur_left(&segs[1]) != 1 || !cv_any_ident(j)) {
                return false;
            }
            if (embi_str_is(j->tok.value, "jump-start") || embi_str_is(j->tok.value, "start")) {
                e->jump = EMBI_JUMP_START;
            } else if (embi_str_is(j->tok.value, "jump-end") || embi_str_is(j->tok.value, "end")) {
                e->jump = EMBI_JUMP_END;
            } else if (embi_str_is(j->tok.value, "jump-none")) {
                e->jump = EMBI_JUMP_NONE;
                if (e->steps < 2) {
                    return false;
                }
            } else if (embi_str_is(j->tok.value, "jump-both")) {
                e->jump = EMBI_JUMP_BOTH;
            } else {
                return false;
            }
        }
        e->kind = EMBI_EASE_STEPS;
    } else {
        return false;
    }
    out->kind = EMBI_V_EASING;
    out->easing = e;
    return true;
}

bool embi_gram_easing(embi_gctx *g, embi_cur *c, embi_value *out)
{
    return embi_parse_easing(g->a, cur_take(c), out);
}

/* ------------------------------------------------------------------ basic shapes */

/* SVG path data: a loose but complete syntax check (commands, argument counts, numbers, arc
 * flags). The string is kept as written. */
static bool svg_number(const char *s, size_t n, size_t *i)
{
    size_t p = *i;
    if (p < n && (s[p] == '+' || s[p] == '-')) {
        p++;
    }
    size_t digits = 0;
    while (p < n && s[p] >= '0' && s[p] <= '9') {
        p++;
        digits++;
    }
    if (p < n && s[p] == '.') {
        p++;
        while (p < n && s[p] >= '0' && s[p] <= '9') {
            p++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        size_t q = p + 1;
        if (q < n && (s[q] == '+' || s[q] == '-')) {
            q++;
        }
        size_t ed = 0;
        while (q < n && s[q] >= '0' && s[q] <= '9') {
            q++;
            ed++;
        }
        if (ed > 0) {
            p = q;
        }
    }
    *i = p;
    return true;
}

static void svg_skip(const char *s, size_t n, size_t *i, bool comma)
{
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r' || s[*i] == '\f')) {
        (*i)++;
    }
    if (comma && *i < n && s[*i] == ',') {
        (*i)++;
        while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r' || s[*i] == '\f')) {
            (*i)++;
        }
    }
}

static bool svg_path_ok(embi_str str)
{
    const char *s = str.ptr;
    size_t n = str.len;
    size_t i = 0;
    svg_skip(s, n, &i, false);
    if (i >= n || (s[i] != 'M' && s[i] != 'm')) {
        return false;
    }
    bool first = true;
    while (i < n) {
        char cmd = s[i];
        int argc;
        switch (cmd) {
        case 'M': case 'm': case 'L': case 'l': case 'T': case 't': argc = 2; break;
        case 'H': case 'h': case 'V': case 'v': argc = 1; break;
        case 'C': case 'c': argc = 6; break;
        case 'S': case 's': case 'Q': case 'q': argc = 4; break;
        case 'A': case 'a': argc = 7; break;
        case 'Z': case 'z': argc = 0; break;
        default: return false;
        }
        (void)first;
        first = false;
        i++;
        svg_skip(s, n, &i, false);
        if (argc == 0) {
            continue;
        }
        bool any = false;
        for (;;) {
            size_t save = i;
            bool ok = true;
            for (int k = 0; k < argc; k++) {
                bool flag = cmd != 'A' && cmd != 'a' ? false : (k == 3 || k == 4);
                if (flag) {
                    if (i < n && (s[i] == '0' || s[i] == '1')) {
                        i++;
                    } else {
                        ok = false;
                        break;
                    }
                } else if (!svg_number(s, n, &i)) {
                    ok = false;
                    break;
                }
                svg_skip(s, n, &i, true);
            }
            if (!ok) {
                if (any && i == save) {
                    break;
                }
                return false;
            }
            any = true;
            if (i >= n || !((s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.')) {
                break;
            }
        }
    }
    return true;
}

static bool is_geom_box(const embi_css_cv *cv, uint16_t *kw)
{
    if (!cv_any_ident(cv)) {
        return false;
    }
    static const char *const names[] = {
        "margin-box", "border-box", "padding-box", "content-box", "fill-box", "stroke-box", "view-box",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (embi_str_is(cv->tok.value, names[i])) {
            *kw = embi_kw_lookup(cv->tok.value);
            return true;
        }
    }
    return false;
}

static bool opt_round(embi_arena *a, embi_cur *s, embi_shape *sh)
{
    if (cur_done(s)) {
        return true;
    }
    if (!cv_ident(cur_peek(s), "round")) {
        return false;
    }
    s->i++;
    embi_value h[4], v[4];
    if (!embi_parse_radius_set(a, s, true, h, v)) {
        return false;
    }
    memcpy(&sh->radii[0], h, sizeof h);
    memcpy(&sh->radii[4], v, sizeof v);
    sh->has_round = true;
    return cur_done(s);
}

static bool shape_radius(embi_arena *a, embi_cur *s, embi_value *out)
{
    const embi_css_cv *t = cur_peek(s);
    if (cv_ident(t, "closest-side") || cv_ident(t, "farthest-side")) {
        *out = v_kw(cv_kw(t));
        s->i++;
        return true;
    }
    if (t && embi_value_parse_types(a, EMBI_LP_NN, t, out)) {
        s->i++;
        return true;
    }
    return false;
}


/* ------------------------------------------------------------------ shape() */

/* A point: `to` takes a <position> (absolute), `by` two length-percentages (relative). */
static bool shape_pt(embi_arena *a, embi_cur *s, bool rel, embi_pos_axis out[2])
{
    if (rel) {
        for (int k = 0; k < 2; k++) {
            memset(&out[k], 0, sizeof out[k]);
            if (!embi_value_parse_types(a, EMBI_LP, cur_peek(s), &out[k].off)) {
                return false;
            }
            s->i++;
        }
        return true;
    }
    embi_position p;
    memset(&p, 0, sizeof p);
    if (!embi_parse_position(a, s, &p)) {
        return false;
    }
    out[0] = p.x;
    out[1] = p.y;
    return true;
}

static bool shape_cmd(embi_arena *a, embi_cur *s, embi_shape_cmd *c)
{
    const embi_css_cv *t = cur_take(s);
    if (!t || t->tok.type != EMBI_CSS_IDENT) {
        return false;
    }
    memset(c, 0, sizeof *c);
    embi_str n = t->tok.value;
    if (embi_str_is(n, "close")) {
        c->op = EMBI_SOP_CLOSE;
        return cur_done(s);
    }
    if (embi_str_is(n, "move")) {
        c->op = EMBI_SOP_MOVE;
    } else if (embi_str_is(n, "line")) {
        c->op = EMBI_SOP_LINE;
    } else if (embi_str_is(n, "hline")) {
        c->op = EMBI_SOP_HLINE;
    } else if (embi_str_is(n, "vline")) {
        c->op = EMBI_SOP_VLINE;
    } else if (embi_str_is(n, "curve")) {
        c->op = EMBI_SOP_CURVE;
    } else if (embi_str_is(n, "smooth")) {
        c->op = EMBI_SOP_SMOOTH;
    } else if (embi_str_is(n, "arc")) {
        c->op = EMBI_SOP_ARC;
    } else {
        return false;
    }
    const embi_css_cv *tb = cur_take(s);
    if (cv_ident(tb, "to")) {
        c->rel = false;
    } else if (cv_ident(tb, "by")) {
        c->rel = true;
    } else {
        return false;
    }
    if (c->op == EMBI_SOP_HLINE || c->op == EMBI_SOP_VLINE) {
        if (c->rel) {
            if (!embi_value_parse_types(a, EMBI_LP, cur_peek(s), &c->p[0].off)) {
                return false;
            }
            s->i++;
        } else if (!embi_parse_pos_axis(a, s, c->op == EMBI_SOP_HLINE, &c->p[0])) {
            return false;
        }
        return cur_done(s);
    }
    if (!shape_pt(a, s, c->rel, c->p)) {
        return false;
    }
    if (c->op == EMBI_SOP_MOVE || c->op == EMBI_SOP_LINE) {
        return cur_done(s);
    }
    if (c->op == EMBI_SOP_CURVE || c->op == EMBI_SOP_SMOOTH) {
        if (cur_done(s) && c->op == EMBI_SOP_SMOOTH) {
            return true;
        }
        if (!cv_ident(cur_take(s), "with")) {
            return false;
        }
        uint32_t max = c->op == EMBI_SOP_CURVE ? 2 : 1;
        for (;;) {
            if (c->ncp >= max || !shape_pt(a, s, c->rel, c->c[c->ncp])) {
                return false;
            }
            c->ncp++;
            if (cv_delim(cur_peek(s), '/')) {
                s->i++;
                continue;
            }
            break;
        }
        if (c->op == EMBI_SOP_CURVE && c->ncp == 0) {
            return false;
        }
        return cur_done(s);
    }
    /* arc: of <radius>{1,2} then cw|ccw, large|small and rotate <angle> in any order */
    if (!cv_ident(cur_take(s), "of")) {
        return false;
    }
    for (int k = 0; k < 2; k++) {
        if (!embi_value_parse_types(a, EMBI_LP_NN, cur_peek(s), &c->r[k])) {
            break;
        }
        s->i++;
        c->nr++;
    }
    if (c->nr == 0) {
        return false;
    }
    bool got_dir = false, got_size = false, got_rot = false;
    while (!cur_done(s)) {
        const embi_css_cv *o = cur_take(s);
        if (cv_ident(o, "cw") || cv_ident(o, "ccw")) {
            if (got_dir) {
                return false;
            }
            got_dir = true;
            c->cw = cv_ident(o, "cw");
        } else if (cv_ident(o, "large") || cv_ident(o, "small")) {
            if (got_size) {
                return false;
            }
            got_size = true;
            c->large = cv_ident(o, "large");
        } else if (cv_ident(o, "rotate")) {
            embi_value ang;
            if (got_rot || !embi_value_parse_types(a, EMBI_T_ANGLE, cur_peek(s), &ang)) {
                return false;
            }
            s->i++;
            got_rot = true;
            c->rot = ang.num;
        } else {
            return false;
        }
    }
    return true;
}

static bool parse_shape_fn(embi_arena *a, embi_cur *s, embi_shape *sh)
{
    sh->kind = EMBI_SHAPE_SHAPE;
    if (cv_ident(cur_peek(s), "nonzero") || cv_ident(cur_peek(s), "evenodd")) {
        sh->evenodd = cv_ident(cur_peek(s), "evenodd");
        s->i++;
    }
    if (!cv_ident(cur_take(s), "from") || !embi_parse_position(a, s, &sh->from) || !cv_comma(cur_take(s))) {
        return false;
    }
    uint32_t nseg = 0;
    embi_cur *segs = embi_cur_split(a, s, &nseg);
    if (!segs || nseg == 0) {
        return false;
    }
    embi_shape_cmd *cmds = EMBI_NEW_ARRAY(a, embi_shape_cmd, nseg);
    for (uint32_t i = 0; i < nseg; i++) {
        if (!shape_cmd(a, &segs[i], &cmds[i])) {
            return false;
        }
    }
    sh->cmds = cmds;
    sh->ncmds = nseg;
    return true;
}

static bool parse_shape(embi_arena *a, const embi_css_cv *fn, embi_shape **out)
{
    embi_shape *sh = EMBI_NEW(a, embi_shape);
    embi_cur s;
    embi_cur_kids(a, fn, &s);
    embi_str name = fn->tok.value;
    if (embi_str_is(name, "inset") || embi_str_is(name, "rect")) {
        bool rect = embi_str_is(name, "rect");
        sh->kind = rect ? EMBI_SHAPE_RECT : EMBI_SHAPE_INSET;
        embi_value raw[4];
        uint32_t n = 0;
        while (n < 4 && !cur_done(&s)) {
            const embi_css_cv *t = cur_peek(&s);
            if (rect && cv_ident(t, "auto")) {
                raw[n] = v_kw(EMBI_KW_auto);
            } else if (!embi_value_parse_types(a, EMBI_LP, t, &raw[n])) {
                break;
            }
            s.i++;
            n++;
        }
        if (n == 0 || (rect && n != 4)) {
            return false;
        }
        static const uint8_t pick[5][4] = {
            {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3},
        };
        for (int i = 0; i < 4; i++) {
            sh->box[i] = raw[pick[n][i]];
        }
        if (!opt_round(a, &s, sh)) {
            return false;
        }
    } else if (embi_str_is(name, "xywh")) {
        sh->kind = EMBI_SHAPE_XYWH;
        for (int i = 0; i < 4; i++) {
            if (!embi_value_parse_types(a, i < 2 ? EMBI_LP : EMBI_LP_NN, cur_peek(&s), &sh->box[i])) {
                return false;
            }
            s.i++;
        }
        if (!opt_round(a, &s, sh)) {
            return false;
        }
    } else if (embi_str_is(name, "circle") || embi_str_is(name, "ellipse")) {
        bool ell = embi_str_is(name, "ellipse");
        sh->kind = ell ? EMBI_SHAPE_ELLIPSE : EMBI_SHAPE_CIRCLE;
        sh->r[0] = v_kw(EMBI_KW_closest_side);
        sh->r[1] = v_kw(EMBI_KW_closest_side);
        if (!cur_done(&s) && !cv_ident(cur_peek(&s), "at")) {
            if (!shape_radius(a, &s, &sh->r[0])) {
                return false;
            }
            if (ell) {
                if (!shape_radius(a, &s, &sh->r[1])) {
                    return false;
                }
            }
        }
        if (!cur_done(&s)) {
            if (!cv_ident(cur_peek(&s), "at")) {
                return false;
            }
            s.i++;
            if (!embi_parse_position(a, &s, &sh->at)) {
                return false;
            }
            sh->has_at = true;
        }
        if (!cur_done(&s)) {
            return false;
        }
    } else if (embi_str_is(name, "shape")) {
        if (!parse_shape_fn(a, &s, sh)) {
            return false;
        }
    } else if (embi_str_is(name, "polygon") || embi_str_is(name, "path")) {
        bool path = embi_str_is(name, "path");
        sh->kind = path ? EMBI_SHAPE_PATH : EMBI_SHAPE_POLYGON;
        if (cv_ident(cur_peek(&s), "nonzero") || cv_ident(cur_peek(&s), "evenodd")) {
            sh->evenodd = cv_ident(cur_peek(&s), "evenodd");
            s.i++;
            if (!cv_comma(cur_take(&s))) {
                return false;
            }
        }
        if (path) {
            const embi_css_cv *p = cur_take(&s);
            if (!cv_string(p) || !cur_done(&s) || !svg_path_ok(p->tok.value)) {
                return false;
            }
            sh->path = p->tok.value;
        } else {
            uint32_t nseg = 0;
            embi_cur *segs = embi_cur_split(a, &s, &nseg);
            embi_value *pts = EMBI_NEW_ARRAY(a, embi_value, nseg * 2);
            for (uint32_t i = 0; i < nseg; i++) {
                if (cur_left(&segs[i]) != 2 ||
                    !embi_value_parse_types(a, EMBI_LP, segs[i].c[0], &pts[i * 2]) ||
                    !embi_value_parse_types(a, EMBI_LP, segs[i].c[1], &pts[i * 2 + 1])) {
                    return false;
                }
            }
            sh->pts = pts;
            sh->npts = nseg;
        }
    } else {
        return false;
    }
    *out = sh;
    return true;
}

bool embi_gram_clip_path(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *first = cur_peek(c);
    if (!first) {
        return false;
    }
    if (first->tok.type == EMBI_CSS_URL || cv_fn(first, "url")) {
        c->i++;
        return embi_value_parse_types(g->a, EMBI_T_URL, first, out);
    }
    const embi_shape *shape = nullptr;
    uint16_t box = 0;
    uint32_t taken = 0;
    while (!cur_done(c) && taken < 2) {
        const embi_css_cv *t = cur_peek(c);
        uint16_t kw;
        if (t->tok.type == EMBI_CSS_FUNCTION && !shape) {
            embi_shape *sh = nullptr;
            if (!parse_shape(g->a, t, &sh)) {
                return false;
            }
            shape = sh;
        } else if (!box && is_geom_box(t, &kw)) {
            box = kw;
        } else {
            return false;
        }
        c->i++;
        taken++;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_SHAPE;
    out->shape = shape;
    out->kw = box;
    return true;
}
