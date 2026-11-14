/*
 * Interpolation of computed values (W6a). embi_anim_mix dispatches on the property's interp kind
 * (props.txt): lengths, percentages and calc() mixes build a calc() node when the units differ;
 * numbers mix linearly; colors mix in premultiplied Oklab (CSS Color 4, the default for animation);
 * transform lists mix pairwise when their function lists match and otherwise by the 2D matrix
 * decomposition of CSS Transforms 2; shadow, filter, shape, position and list values mix
 * item by item. A value that cannot mix is discrete.
 */
#include "anim/anim.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ arena helpers */

#define EMBI_PI 3.14159265358979323846

static void *anew(embi_arena *a, size_t n)
{
    void *p = embi_arena_alloc(a, n, 8);
    memset(p, 0, n);
    return p;
}

static double lerp(double x, double y, double p)
{
    return x + (y - x) * p;
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static embi_value val_kw(uint16_t kw)
{
    embi_value v;
    memset(&v, 0, sizeof v);
    v.kind = EMBI_V_KEYWORD;
    v.kw = kw;
    return v;
}

/* ------------------------------------------------------------------ calc() nodes for lengths */

static embi_calc *calc_leaf(embi_arena *a, uint8_t type, uint8_t unit, double num)
{
    embi_calc *c = anew(a, sizeof *c);
    c->op = EMBI_CALC_LEAF;
    c->type = type;
    c->unit = unit;
    c->num = num;
    return c;
}

static embi_calc *calc_op(embi_arena *a, uint8_t op, uint8_t type, const embi_calc *x,
                          const embi_calc *y)
{
    embi_calc *c = anew(a, sizeof *c);
    const embi_calc **k = embi_arena_alloc(a, sizeof(*k) * 2, 8);
    k[0] = x;
    k[1] = y;
    c->op = op;
    c->type = type;
    c->n = 2;
    c->kids = k;
    return c;
}

/* A length, percentage or calc() of length-percentage type as a calc node, or nullptr. */
static const embi_calc *lp_node(embi_arena *a, const embi_value *v)
{
    switch (v->kind) {
    case EMBI_V_LENGTH:
        return calc_leaf(a, EMBI_CD_LEN, v->unit, v->num);
    case EMBI_V_PERCENT:
        return calc_leaf(a, EMBI_CD_PCT, EMBI_U_NONE, v->num);
    case EMBI_V_CALC:
        if (v->calc->type != 0 && (v->calc->type & ~EMBI_CD_LENPCT) == 0) {
            return v->calc;
        }
        return nullptr;
    default:
        return nullptr;
    }
}

static bool is_lp(const embi_value *v)
{
    return v->kind == EMBI_V_LENGTH || v->kind == EMBI_V_PERCENT ||
           (v->kind == EMBI_V_CALC && v->calc->type != 0 &&
            (v->calc->type & ~EMBI_CD_LENPCT) == 0);
}

/* c scaled by k (k == 1 keeps the node). */
static const embi_calc *scale_node(embi_arena *a, const embi_calc *c, double k)
{
    if (k == 1.0) {
        return c;
    }
    return calc_op(a, EMBI_CALC_MUL, c->type, c, calc_leaf(a, EMBI_CD_NUM, EMBI_U_NONE, k));
}

static void set_calc(embi_value *r, const embi_calc *c)
{
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_CALC;
    r->calc = c;
}

/* A length-percentage mix: plain lengths of one unit and plain percentages stay plain. */
static bool mix_lp(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                   embi_value *r)
{
    if (f->kind == EMBI_V_LENGTH && t->kind == EMBI_V_LENGTH && f->unit == t->unit) {
        *r = *f;
        r->num = lerp(f->num, t->num, p);
        return true;
    }
    if (f->kind == EMBI_V_PERCENT && t->kind == EMBI_V_PERCENT) {
        *r = *f;
        r->num = lerp(f->num, t->num, p);
        return true;
    }
    const embi_calc *fc = lp_node(a, f);
    const embi_calc *tc = lp_node(a, t);
    if (fc == nullptr || tc == nullptr) {
        return false;
    }
    const embi_calc *ma = scale_node(a, fc, 1.0 - p);
    const embi_calc *mb = scale_node(a, tc, p);
    set_calc(r, calc_op(a, EMBI_CALC_ADD, (uint8_t)(fc->type | tc->type), ma, mb));
    return true;
}

/* The start-edge form of a position axis: `right 10px` is calc(100% - 10px). */
static bool axis_start(embi_arena *a, const embi_pos_axis *ax, embi_value *r)
{
    if (ax->anchor == 0) {
        *r = ax->off;
        return true;
    }
    const embi_calc *n = lp_node(a, &ax->off);
    if (n == nullptr) {
        return false;
    }
    const embi_calc *full = calc_leaf(a, EMBI_CD_PCT, EMBI_U_NONE, 100.0);
    set_calc(r, calc_op(a, EMBI_CALC_SUB, (uint8_t)(n->type | EMBI_CD_PCT), full, n));
    return true;
}

/* ------------------------------------------------------------------ color (premultiplied Oklab) */

static double srgb_to_linear(double c)
{
    double m = fabs(c);
    double r = m <= 0.04045 ? m / 12.92 : pow((m + 0.055) / 1.055, 2.4);
    return c < 0 ? -r : r;
}

static double linear_to_srgb(double c)
{
    double m = fabs(c);
    double r = m <= 0.0031308 ? 12.92 * m : 1.055 * pow(m, 1.0 / 2.4) - 0.055;
    return c < 0 ? -r : r;
}

/* An sRGB color as premultiplied-ready Oklab with its alpha. False when the color has no sRGB
 * form (currentcolor, system colors without a palette). */
static bool color_to_oklab(const embi_color *c, double lab[3], double *alpha)
{
    embi_color s;
    if (!embi_color_to_srgb(c, &s)) {
        return false;
    }
    double r = srgb_to_linear(s.c[0]);
    double g = srgb_to_linear(s.c[1]);
    double b = srgb_to_linear(s.c[2]);
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double q = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * q;
    lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * q;
    lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * q;
    *alpha = s.alpha;
    return true;
}

static void oklab_to_color(const double lab[3], double alpha, embi_color *out)
{
    double l = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    double m = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    double q = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    l = l * l * l;
    m = m * m * m;
    q = q * q * q;
    memset(out, 0, sizeof *out);
    out->space = EMBI_CS_SRGB;
    /* Extrapolated colors can leave the sRGB cube by rounding error or by overshoot: the result is
     * clamped into it (paint maps the gamut anyway). */
    out->c[0] = clamp01(linear_to_srgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * q));
    out->c[1] = clamp01(linear_to_srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * q));
    out->c[2] = clamp01(linear_to_srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * q));
    out->alpha = clamp01(alpha);
}

/* A legacy sRGB color (hex, named, rgb(), hsl(), hwb()): such colors interpolate in gamma-encoded sRGB (CSS Color 4
 * section 12.1, a requirement for web compatibility); any other pair interpolates in Oklab. */
static bool legacy_srgb(const embi_color *c)
{
    return c->space == EMBI_CS_SRGB && (c->flags & (EMBI_CF_FUNC | EMBI_CF_NONE_ALL)) == 0;
}

static bool mix_color(const embi_value *f, const embi_value *t, double p, embi_value *r)
{
    if (f->kind != EMBI_V_COLOR || t->kind != EMBI_V_COLOR) {
        return false;
    }
    if (legacy_srgb(&f->color) && legacy_srgb(&t->color)) {
        embi_color sf, st;
        if (!embi_color_to_srgb(&f->color, &sf) || !embi_color_to_srgb(&t->color, &st)) {
            return false;
        }
        /* two colors of the legacy syntaxes (hex, names, rgb(), hsl() ...) mix premultiplied in sRGB, as the browsers do */
        double alpha = lerp(sf.alpha, st.alpha, p);
        memset(r, 0, sizeof *r);
        r->kind = EMBI_V_COLOR;
        r->color.space = EMBI_CS_SRGB;
        for (int i = 0; i < 3; i++) {
            double pm = lerp(sf.c[i] * sf.alpha, st.c[i] * st.alpha, p);
            /* the channels are 8-bit values and the alpha has three digits, as the serialization of the result shows them */
            r->color.c[i] = alpha == 0.0 ? 0.0 : round(clamp01(pm / alpha) * 255.0) / 255.0;
        }
        r->color.alpha = round(clamp01(alpha) * 1000.0) / 1000.0;
        return true;
    }
    double lf[3], lt[3], af, at;
    if (!color_to_oklab(&f->color, lf, &af) || !color_to_oklab(&t->color, lt, &at)) {
        return false;
    }
    double alpha = lerp(af, at, p);
    double lab[3];
    for (int i = 0; i < 3; i++) {
        double pm = lerp(lf[i] * af, lt[i] * at, p);
        lab[i] = alpha == 0.0 ? 0.0 : pm / alpha;
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_COLOR;
    oklab_to_color(lab, alpha, &r->color);
    return true;
}

/* ------------------------------------------------------------------ numbers and integers */

static bool mix_number(const embi_value *f, const embi_value *t, double p, embi_value *r)
{
    if (f->kind == EMBI_V_NUMBER && t->kind == EMBI_V_NUMBER) {
        *r = *f;
        r->num = lerp(f->num, t->num, p);
        return true;
    }
    if (f->kind == EMBI_V_PERCENT && t->kind == EMBI_V_PERCENT) {
        *r = *f;
        r->num = lerp(f->num, t->num, p);
        return true;
    }
    return false;
}

/* Integers mix as numbers and round to the nearest integer. */
static bool mix_integer(const embi_value *f, const embi_value *t, double p, embi_value *r)
{
    bool ints = (f->kind == EMBI_V_INTEGER || f->kind == EMBI_V_NUMBER) &&
                (t->kind == EMBI_V_INTEGER || t->kind == EMBI_V_NUMBER);
    if (!ints) {
        return false;
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_INTEGER;
    r->num = floor(lerp(f->num, t->num, p) + 0.5);
    return true;
}

/* ------------------------------------------------------------------ length-like values (len) */

static bool mix_len(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                    embi_value *r)
{
    if (f->kind == EMBI_V_NUMBER && t->kind == EMBI_V_NUMBER) {
        return mix_number(f, t, p, r);
    }
    if (is_lp(f) && is_lp(t)) {
        return mix_lp(a, f, t, p, r);
    }
    return false;
}

/* ------------------------------------------------------------------ visibility */

static bool mix_visibility(const embi_value *f, const embi_value *t, double p, embi_value *r)
{
    if (f->kind != EMBI_V_KEYWORD || t->kind != EMBI_V_KEYWORD || p <= 0.0 || p >= 1.0) {
        return false;
    }
    if (f->kw != EMBI_KW_visible && t->kw != EMBI_KW_visible) {
        return false;
    }
    *r = val_kw(EMBI_KW_visible);
    return true;
}

/* display (CSS Display 4) and content-visibility (CSS Contain 3): discrete, but for 0 < p < 1 the
 * non-none (display) or visible (content-visibility) end applies when either end is none/visible.
 * Everything else flips at 50% through the caller's discrete fallback. */
static bool mix_none_rule(const embi_value *f, const embi_value *t, double p, bool display,
                          embi_value *r)
{
    if (f->kind != EMBI_V_KEYWORD || t->kind != EMBI_V_KEYWORD || p <= 0.0 || p >= 1.0) {
        return false;
    }
    if (display) {
        bool fn = f->kw == EMBI_KW_none, tn = t->kw == EMBI_KW_none;
        if (fn == tn) {
            return false;
        }
        *r = fn ? *t : *f;
        return true;
    }
    if (f->kw == EMBI_KW_visible) {
        *r = *f;
        return true;
    }
    if (t->kw == EMBI_KW_visible) {
        *r = *t;
        return true;
    }
    return false;
}

/* <ratio> mixes geometrically (the ratio's logarithm is interpolated, CSS Values 4). A degenerate
 * (zero or infinite) ratio and auto stay discrete. */
static bool mix_ratio(const embi_value *f, const embi_value *t, double p, embi_value *r)
{
    if (f->kind != EMBI_V_RATIO || t->kind != EMBI_V_RATIO) {
        return false;
    }
    double fa = f->ratio.a, fb = f->ratio.b, ta = t->ratio.a, tb = t->ratio.b;
    if (!(fa > 0) || !(fb > 0) || !(ta > 0) || !(tb > 0) || isinf(fa) || isinf(fb) || isinf(ta) ||
        isinf(tb)) {
        return false;
    }
    double lf = log(fa / fb), lt = log(ta / tb);
    double q = exp(lerp(lf, lt, p));
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_RATIO;
    r->ratio.a = q;
    r->ratio.b = 1.0;
    return true;
}

/* ------------------------------------------------------------------ shadows */

static embi_value zero_length(void)
{
    embi_value v;
    memset(&v, 0, sizeof v);
    v.kind = EMBI_V_LENGTH;
    v.unit = EMBI_U_PX;
    v.num = 0.0;
    return v;
}

/* A transparent, zero-offset shadow with the given inset (the padding item of a shorter list). */
static embi_shadow zero_shadow(bool inset)
{
    embi_shadow s;
    memset(&s, 0, sizeof s);
    s.color.kind = EMBI_V_COLOR;
    s.color.color.space = EMBI_CS_SRGB;
    s.color.color.alpha = 0.0;
    s.x = zero_length();
    s.y = zero_length();
    s.blur = zero_length();
    s.spread = zero_length();
    s.inset = inset;
    return s;
}

static bool mix_one_shadow(embi_arena *a, const embi_shadow *f, const embi_shadow *t, double p,
                           embi_shadow *r)
{
    if (f->inset != t->inset) {
        return false;
    }
    embi_value c;
    if (!mix_color(&f->color, &t->color, p, &c)) {
        return false;
    }
    r->color = c;
    r->inset = f->inset;
    embi_value *dst[4] = {&r->x, &r->y, &r->blur, &r->spread};
    const embi_value *fs[4] = {&f->x, &f->y, &f->blur, &f->spread};
    const embi_value *ts[4] = {&t->x, &t->y, &t->blur, &t->spread};
    for (int i = 0; i < 4; i++) {
        if (!mix_lp(a, fs[i], ts[i], p, dst[i])) {
            return false;
        }
    }
    return true;
}

/* The shadow list items of a value: a SHADOWS list, or none (an empty list). False for others. */
static bool shadow_list(const embi_value *v, const embi_shadow **p, uint32_t *n)
{
    if (v->kind == EMBI_V_SHADOWS) {
        *p = v->shadows.p;
        *n = v->shadows.n;
        return true;
    }
    if (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none) {
        *p = nullptr;
        *n = 0;
        return true;
    }
    return false;
}

static bool mix_shadows(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                        embi_value *r)
{
    const embi_shadow *fp, *tp;
    uint32_t fn, tn;
    if (!shadow_list(f, &fp, &fn) || !shadow_list(t, &tp, &tn)) {
        return false;
    }
    if (f->kind == EMBI_V_KEYWORD && t->kind == EMBI_V_KEYWORD) {
        *r = *f;
        return true;
    }
    uint32_t n = fn > tn ? fn : tn;
    if (n == 0) {
        return false;
    }
    embi_shadow *out = embi_arena_alloc(a, sizeof(embi_shadow) * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        embi_shadow sf = i < fn ? fp[i] : zero_shadow(i < tn ? tp[i].inset : false);
        embi_shadow st = i < tn ? tp[i] : zero_shadow(i < fn ? fp[i].inset : false);
        if (!mix_one_shadow(a, &sf, &st, p, &out[i])) {
            return false;
        }
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_SHADOWS;
    r->shadows.p = out;
    r->shadows.n = n;
    return true;
}

/* ------------------------------------------------------------------ filters */

static embi_filter identity_filter(embi_arena *a, uint8_t fn)
{
    embi_filter f;
    memset(&f, 0, sizeof f);
    f.fn = fn;
    switch (fn) {
    case EMBI_FILTER_BLUR:
        f.arg = zero_length();
        break;
    case EMBI_FILTER_HUE_ROTATE:
        f.arg.kind = EMBI_V_ANGLE;
        f.arg.unit = EMBI_U_DEG;
        break;
    case EMBI_FILTER_BRIGHTNESS:
    case EMBI_FILTER_CONTRAST:
    case EMBI_FILTER_OPACITY:
    case EMBI_FILTER_SATURATE:
        f.arg.kind = EMBI_V_NUMBER;
        f.arg.num = 1.0;
        break;
    case EMBI_FILTER_DROP_SHADOW: {
        embi_shadow *zero = embi_arena_alloc(a, sizeof(embi_shadow), 8);
        *zero = zero_shadow(false);
        f.shadow = zero;
        break;
    }
    default:
        f.arg.kind = EMBI_V_NUMBER;
        f.arg.num = 0.0;
        break;
    }
    return f;
}

static bool mix_one_filter(embi_arena *a, const embi_filter *f, const embi_filter *t, double p,
                           embi_filter *r)
{
    if (f->fn != t->fn) {
        return false;
    }
    *r = *f;
    switch (f->fn) {
    case EMBI_FILTER_BLUR:
        return mix_lp(a, &f->arg, &t->arg, p, &r->arg);
    case EMBI_FILTER_HUE_ROTATE:
        if (f->arg.kind != EMBI_V_ANGLE || t->arg.kind != EMBI_V_ANGLE) {
            return false;
        }
        r->arg.num = lerp(f->arg.num, t->arg.num, p);
        return true;
    case EMBI_FILTER_DROP_SHADOW: {
        embi_shadow *s = embi_arena_alloc(a, sizeof(embi_shadow), 8);
        if (!mix_one_shadow(a, f->shadow, t->shadow, p, s)) {
            return false;
        }
        r->shadow = s;
        return true;
    }
    case EMBI_FILTER_URL:
        return false;
    default:
        if (f->arg.kind != EMBI_V_NUMBER || t->arg.kind != EMBI_V_NUMBER) {
            return false;
        }
        r->arg.num = lerp(f->arg.num, t->arg.num, p);
        return true;
    }
}

static bool filter_list(const embi_value *v, const embi_filter **p, uint32_t *n)
{
    if (v->kind == EMBI_V_FILTERS) {
        *p = v->filters.p;
        *n = v->filters.n;
        return true;
    }
    if (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none) {
        *p = nullptr;
        *n = 0;
        return true;
    }
    return false;
}

static bool mix_filters(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                        embi_value *r)
{
    const embi_filter *fp, *tp;
    uint32_t fn, tn;
    if (!filter_list(f, &fp, &fn) || !filter_list(t, &tp, &tn)) {
        return false;
    }
    if (f->kind == EMBI_V_KEYWORD && t->kind == EMBI_V_KEYWORD) {
        *r = *f;
        return true;
    }
    uint32_t n = fn > tn ? fn : tn;
    if (n == 0) {
        return false;
    }
    embi_filter *out = embi_arena_alloc(a, sizeof(embi_filter) * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        /* Missing items pad with the identity of the other list's function at that index. */
        embi_filter sf = i < fn ? fp[i] : identity_filter(a, tp[i].fn);
        embi_filter st = i < tn ? tp[i] : identity_filter(a, fp[i].fn);
        if (!mix_one_filter(a, &sf, &st, p, &out[i])) {
            return false;
        }
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_FILTERS;
    r->filters.p = out;
    r->filters.n = n;
    return true;
}

/* ------------------------------------------------------------------ transforms */

typedef struct xf_mat {
    double m[6]; /* a b c d e f: x' = a x + c y + e, y' = b x + d y + f */
} xf_mat;

static const xf_mat xf_identity = {{1, 0, 0, 1, 0, 0}};

/* A numeric argument: a number or angle in its unit (deg is converted to radians by callers). */
static bool arg_num(const embi_value *v, double *out)
{
    if (v->kind == EMBI_V_NUMBER || v->kind == EMBI_V_ANGLE) {
        *out = v->num;
        return true;
    }
    if (v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        *out = v->num;
        return true;
    }
    return false;
}

static xf_mat xf_mul(xf_mat x, xf_mat y)
{
    const double *A = x.m, *B = y.m;
    xf_mat r;
    r.m[0] = A[0] * B[0] + A[2] * B[1];
    r.m[1] = A[1] * B[0] + A[3] * B[1];
    r.m[2] = A[0] * B[2] + A[2] * B[3];
    r.m[3] = A[1] * B[2] + A[3] * B[3];
    r.m[4] = A[0] * B[4] + A[2] * B[5] + A[4];
    r.m[5] = A[1] * B[4] + A[3] * B[5] + A[5];
    return r;
}

/* The 2D matrix of one transform function. False for 3D and perspective functions that are not
 * flat, and for relative lengths and percentages (they need a box). */
static bool xf_item_mat(const embi_xform *x, xf_mat *out)
{
    double v[6] = {0, 0, 0, 0, 0, 0};
    *out = xf_identity;
    if (x->fn == EMBI_XF_MATRIX) {
        if (x->n != 6) {
            return false;
        }
        for (int i = 0; i < 6; i++) {
            if (!arg_num(&x->args[i], &v[i])) {
                return false;
            }
            out->m[i] = v[i];
        }
        return true;
    }
    for (uint32_t i = 0; i < x->n && i < 6; i++) {
        if (!arg_num(&x->args[i], &v[i])) {
            return false;
        }
    }
    switch (x->fn) {
    case EMBI_XF_TRANSLATE:
    case EMBI_XF_TRANSLATE3D:
        out->m[4] = v[0];
        out->m[5] = x->n >= 2 ? v[1] : 0.0;
        return x->fn == EMBI_XF_TRANSLATE || x->n < 3 || v[2] == 0.0;
    case EMBI_XF_TRANSLATEX:
        out->m[4] = v[0];
        return true;
    case EMBI_XF_TRANSLATEY:
        out->m[5] = v[0];
        return true;
    case EMBI_XF_SCALE:
    case EMBI_XF_SCALE3D:
        out->m[0] = v[0];
        out->m[3] = x->n >= 2 ? v[1] : v[0];
        return x->fn == EMBI_XF_SCALE || x->n < 3 || v[2] == 1.0;
    case EMBI_XF_SCALEX:
        out->m[0] = v[0];
        return true;
    case EMBI_XF_SCALEY:
        out->m[3] = v[0];
        return true;
    case EMBI_XF_ROTATE:
    case EMBI_XF_ROTATEZ: {
        double th = v[0] * EMBI_PI / 180.0;
        out->m[0] = cos(th);
        out->m[1] = sin(th);
        out->m[2] = -sin(th);
        out->m[3] = cos(th);
        return true;
    }
    case EMBI_XF_ROTATE3D: {
        if (x->n != 4 || v[0] != 0.0 || v[1] != 0.0 || v[2] == 0.0) {
            return false;
        }
        double th = v[3] * EMBI_PI / 180.0 * (v[2] < 0 ? -1.0 : 1.0);
        out->m[0] = cos(th);
        out->m[1] = sin(th);
        out->m[2] = -sin(th);
        out->m[3] = cos(th);
        return true;
    }
    case EMBI_XF_SKEW:
    case EMBI_XF_SKEWX:
    case EMBI_XF_SKEWY: {
        /* skew(ax, ay): x' = x + tan(ax) y, y' = tan(ay) x + y */
        double ax = x->fn == EMBI_XF_SKEWY ? 0.0 : v[0];
        double ay = x->fn == EMBI_XF_SKEWX ? 0.0 : (x->fn == EMBI_XF_SKEWY ? v[0] : v[1]);
        out->m[2] = tan(ax * EMBI_PI / 180.0);
        out->m[1] = tan(ay * EMBI_PI / 180.0);
        return true;
    }
    default:
        return false;
    }
}

/* The total matrix of a transform value (a list, or none). */
static bool xf_total(const embi_value *v, xf_mat *out)
{
    *out = xf_identity;
    if (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none) {
        return true;
    }
    if (v->kind != EMBI_V_TRANSFORM) {
        return false;
    }
    for (uint32_t i = 0; i < v->xf.n; i++) {
        xf_mat m;
        if (!xf_item_mat(&v->xf.p[i], &m)) {
            return false;
        }
        *out = xf_mul(*out, m);
    }
    return true;
}

/* Decomposition of a 2D matrix as translate, rotate, skew (the factor k along the rotated x axis)
 * and scale (sx, sy signed; the sign of sy is the sign of the determinant). */
typedef struct xf_dec {
    double tx, ty, theta, k, sx, sy;
} xf_dec;

static bool xf_decompose(const xf_mat *mt, xf_dec *d)
{
    const double *m = mt->m;
    double sx = hypot(m[0], m[1]);
    if (sx == 0.0) {
        return false;
    }
    double u0x = m[0] / sx, u0y = m[1] / sx;
    double dot = u0x * m[2] + u0y * m[3];
    double sy = -u0y * m[2] + u0x * m[3];
    if (sy == 0.0) {
        return false;
    }
    d->tx = m[4];
    d->ty = m[5];
    d->theta = atan2(m[1], m[0]);
    d->k = dot / sy;
    d->sx = sx;
    d->sy = sy;
    return true;
}

static xf_mat xf_recompose(const xf_dec *d)
{
    double c = cos(d->theta), s = sin(d->theta);
    xf_mat r;
    r.m[0] = d->sx * c;
    r.m[1] = d->sx * s;
    r.m[2] = d->k * d->sy * c - d->sy * s;
    r.m[3] = d->k * d->sy * s + d->sy * c;
    r.m[4] = d->tx;
    r.m[5] = d->ty;
    return r;
}

static bool mix_xf_decomposed(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                              embi_value *r)
{
    xf_mat mf, mt;
    xf_dec df, dt;
    if (!xf_total(f, &mf) || !xf_total(t, &mt) || !xf_decompose(&mf, &df) ||
        !xf_decompose(&mt, &dt)) {
        return false;
    }
    /* The rotation takes the shorter way round (CSS Transforms 2: the angle difference is wrapped
     * into -pi..pi), so 170deg to -170deg passes through 180deg. */
    double dth = dt.theta - df.theta;
    while (dth > EMBI_PI) {
        dth -= 2.0 * EMBI_PI;
    }
    while (dth < -EMBI_PI) {
        dth += 2.0 * EMBI_PI;
    }
    xf_dec d;
    d.tx = lerp(df.tx, dt.tx, p);
    d.ty = lerp(df.ty, dt.ty, p);
    d.theta = df.theta + dth * p;
    d.k = lerp(df.k, dt.k, p);
    d.sx = lerp(df.sx, dt.sx, p);
    d.sy = lerp(df.sy, dt.sy, p);
    xf_mat m = xf_recompose(&d);
    embi_xform *item = anew(a, sizeof *item);
    embi_value *args = anew(a, sizeof(embi_value) * 6);
    for (int i = 0; i < 6; i++) {
        args[i].kind = EMBI_V_NUMBER;
        args[i].num = m.m[i];
    }
    item->fn = EMBI_XF_MATRIX;
    item->n = 6;
    item->args = args;
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_TRANSFORM;
    r->xf.p = item;
    r->xf.n = 1;
    return true;
}

/* The identity function of the same kind as other, for padding a shorter list (CSS Transforms 2).
 * ROTATE3D keeps the axis of other so that the pair can mix. False for functions with no identity
 * (perspective). */
static bool xf_identity_like(embi_arena *a, const embi_xform *other, embi_xform *out)
{
    out->fn = other->fn;
    out->n = other->n;
    embi_value *args = anew(a, sizeof(embi_value) * (other->n ? other->n : 1));
    out->args = args;
    for (uint32_t k = 0; k < other->n; k++) {
        switch (other->fn) {
        case EMBI_XF_MATRIX:
            if (other->n != 6) {
                return false;
            }
            args[k].kind = EMBI_V_NUMBER;
            args[k].num = (k == 0 || k == 3) ? 1.0 : 0.0;
            break;
        case EMBI_XF_TRANSLATE:
        case EMBI_XF_TRANSLATEX:
        case EMBI_XF_TRANSLATEY:
        case EMBI_XF_TRANSLATEZ:
        case EMBI_XF_TRANSLATE3D:
            args[k] = zero_length();
            break;
        case EMBI_XF_SCALE:
        case EMBI_XF_SCALEX:
        case EMBI_XF_SCALEY:
        case EMBI_XF_SCALEZ:
        case EMBI_XF_SCALE3D:
            args[k].kind = EMBI_V_NUMBER;
            args[k].num = 1.0;
            break;
        case EMBI_XF_ROTATE3D:
            if (k < 3) {
                args[k] = other->args[k];
                break;
            }
            [[fallthrough]];
        case EMBI_XF_ROTATE:
        case EMBI_XF_ROTATEX:
        case EMBI_XF_ROTATEY:
        case EMBI_XF_ROTATEZ:
        case EMBI_XF_SKEW:
        case EMBI_XF_SKEWX:
        case EMBI_XF_SKEWY:
            args[k].kind = EMBI_V_ANGLE;
            args[k].unit = EMBI_U_DEG;
            args[k].num = 0.0;
            break;
        default:
            return false;
        }
    }
    return true;
}

/* Function families that share a primitive (CSS Transforms 2): translate, translateX, translateY
 * (and with a z part translateZ and translate3d), the scale functions and the rotate functions.
 * 0 when fn is in none of them. */
static int xf_family(uint8_t fn)
{
    switch (fn) {
    case EMBI_XF_TRANSLATE: case EMBI_XF_TRANSLATEX: case EMBI_XF_TRANSLATEY:
    case EMBI_XF_TRANSLATEZ: case EMBI_XF_TRANSLATE3D:
        return 1;
    case EMBI_XF_SCALE: case EMBI_XF_SCALEX: case EMBI_XF_SCALEY: case EMBI_XF_SCALEZ:
    case EMBI_XF_SCALE3D:
        return 2;
    case EMBI_XF_ROTATE: case EMBI_XF_ROTATEZ: case EMBI_XF_ROTATEX: case EMBI_XF_ROTATEY:
    case EMBI_XF_ROTATE3D:
        return 3;
    default:
        return 0;
    }
}

/* The components of a translate or scale function as (x, y, z) with the omitted ones at def. */
static void xf_components(const embi_xform *x, embi_value def, embi_value out[3])
{
    out[0] = out[1] = out[2] = def;
    const embi_value *v = x->args;
    switch (x->fn) {
    case EMBI_XF_TRANSLATE: case EMBI_XF_TRANSLATE3D:
    case EMBI_XF_SCALE: case EMBI_XF_SCALE3D:
        for (uint32_t k = 0; k < x->n && k < 3; k++) {
            out[k] = v[k];
        }
        if ((x->fn == EMBI_XF_SCALE) && x->n == 1) {
            out[1] = v[0];
        }
        break;
    case EMBI_XF_TRANSLATEX: case EMBI_XF_SCALEX:
        out[0] = v[0];
        break;
    case EMBI_XF_TRANSLATEY: case EMBI_XF_SCALEY:
        out[1] = v[0];
        break;
    default: /* translateZ, scaleZ */
        out[2] = v[0];
        break;
    }
}

/* Rewrites the pair x, y to the same primitive when they belong to one family and differ in
 * function or in the number of arguments. */
static void xf_normalize_pair(embi_arena *a, embi_xform *x, embi_xform *y)
{
    int fam = xf_family(x->fn);
    if (fam == 0 || fam != xf_family(y->fn) || (x->fn == y->fn && x->n == y->n)) {
        return;
    }
    if (fam == 3) {
        bool d3 = false;
        for (int s = 0; s < 2; s++) {
            uint8_t fn = (s ? y : x)->fn;
            d3 = d3 || fn == EMBI_XF_ROTATEX || fn == EMBI_XF_ROTATEY || fn == EMBI_XF_ROTATE3D;
        }
        if (!d3) {
            x->fn = y->fn = EMBI_XF_ROTATE;
            return;
        }
        for (int s = 0; s < 2; s++) {
            embi_xform *f = s ? y : x;
            if (f->fn == EMBI_XF_ROTATE3D) {
                continue;
            }
            embi_value *args = anew(a, sizeof(embi_value) * 4);
            for (int k = 0; k < 3; k++) {
                args[k].kind = EMBI_V_NUMBER;
                args[k].num = (f->fn == EMBI_XF_ROTATEX && k == 0) ||
                              (f->fn == EMBI_XF_ROTATEY && k == 1) ||
                              ((f->fn == EMBI_XF_ROTATE || f->fn == EMBI_XF_ROTATEZ) && k == 2);
            }
            args[3] = f->args[0];
            f->fn = EMBI_XF_ROTATE3D;
            f->args = args;
            f->n = 4;
        }
        return;
    }
    bool d3 = false;
    for (int s = 0; s < 2; s++) {
        uint8_t fn = (s ? y : x)->fn;
        d3 = d3 || fn == EMBI_XF_TRANSLATEZ || fn == EMBI_XF_TRANSLATE3D ||
             fn == EMBI_XF_SCALEZ || fn == EMBI_XF_SCALE3D;
    }
    embi_value def;
    memset(&def, 0, sizeof def);
    if (fam == 1) {
        def = zero_length();
    } else {
        def.kind = EMBI_V_NUMBER;
        def.num = 1.0;
    }
    uint32_t cnt = d3 ? 3u : 2u;
    for (int s = 0; s < 2; s++) {
        embi_xform *f = s ? y : x;
        embi_value c[3];
        xf_components(f, def, c);
        embi_value *args = anew(a, sizeof(embi_value) * cnt);
        for (uint32_t k = 0; k < cnt; k++) {
            args[k] = c[k];
        }
        f->fn = fam == 1 ? (d3 ? EMBI_XF_TRANSLATE3D : EMBI_XF_TRANSLATE)
                         : (d3 ? EMBI_XF_SCALE3D : EMBI_XF_SCALE);
        f->args = args;
        f->n = cnt;
    }
}

/* Pairwise mixing of two lists of equal length whose functions match. Matrix pairs are not mixed
 * pairwise: they decompose (the caller falls back). */
static bool mix_xf_pairwise(embi_arena *a, const embi_xform *fx, const embi_xform *tx, uint32_t n,
                            double p, embi_value *r)
{
    embi_xform *items = embi_arena_alloc(a, sizeof(embi_xform) * (n ? n : 1), 8);
    for (uint32_t i = 0; i < n; i++) {
        const embi_xform *x = &fx[i], *y = &tx[i];
        if (x->fn != y->fn || x->n != y->n || x->fn == EMBI_XF_MATRIX) {
            return false;
        }
        embi_value *args = anew(a, sizeof(embi_value) * (x->n ? x->n : 1));
        for (uint32_t k = 0; k < x->n; k++) {
            const embi_value *fa = &x->args[k], *ta = &y->args[k];
            bool ok;
            if (x->fn == EMBI_XF_ROTATE3D && k < 3) {
                ok = fa->num == ta->num; /* the axis must match */
                args[k] = *fa;
            } else if (fa->kind == EMBI_V_NUMBER && ta->kind == EMBI_V_NUMBER) {
                ok = mix_number(fa, ta, p, &args[k]);
            } else if (fa->kind == EMBI_V_ANGLE && ta->kind == EMBI_V_ANGLE) {
                args[k] = *fa;
                args[k].num = lerp(fa->num, ta->num, p);
                ok = true;
            } else {
                ok = is_lp(fa) && is_lp(ta) && mix_lp(a, fa, ta, p, &args[k]);
            }
            if (!ok) {
                return false;
            }
        }
        items[i] = *x;
        items[i].args = args;
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_TRANSFORM;
    r->xf.p = items;
    r->xf.n = n;
    return true;
}

/* Lists of transform functions (CSS Transforms 2): a none value is the empty list; the shorter of
 * two lists is padded at its end with identity functions of the other list's kinds. When the
 * padded lists match function by function they mix pairwise, otherwise the total matrices are
 * decomposed and mixed. */
static bool mix_xform(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                      embi_value *r)
{
    if (f->kind == EMBI_V_KEYWORD && t->kind == EMBI_V_KEYWORD && f->kw == EMBI_KW_none &&
        t->kw == EMBI_KW_none) {
        *r = *f;
        return true;
    }
    bool fl = f->kind == EMBI_V_TRANSFORM || (f->kind == EMBI_V_KEYWORD && f->kw == EMBI_KW_none);
    bool tl = t->kind == EMBI_V_TRANSFORM || (t->kind == EMBI_V_KEYWORD && t->kw == EMBI_KW_none);
    if (!fl || !tl) {
        return false;
    }
    uint32_t nf = f->kind == EMBI_V_TRANSFORM ? f->xf.n : 0;
    uint32_t nt = t->kind == EMBI_V_TRANSFORM ? t->xf.n : 0;
    uint32_t n = nf > nt ? nf : nt;
    if (n > 0) {
        embi_xform *pf = embi_arena_alloc(a, sizeof(embi_xform) * n, 8);
        embi_xform *pt = embi_arena_alloc(a, sizeof(embi_xform) * n, 8);
        bool padded = true;
        for (uint32_t i = 0; i < n && padded; i++) {
            if (i < nf) {
                pf[i] = f->xf.p[i];
            } else {
                padded = xf_identity_like(a, &t->xf.p[i], &pf[i]);
            }
            if (padded && i < nt) {
                pt[i] = t->xf.p[i];
            } else if (padded) {
                padded = xf_identity_like(a, &f->xf.p[i], &pt[i]);
            }
        }
        for (uint32_t i = 0; i < n && padded; i++) {
            xf_normalize_pair(a, &pf[i], &pt[i]);
        }
        if (padded && mix_xf_pairwise(a, pf, pt, n, p, r)) {
            return true;
        }
    }
    return mix_xf_decomposed(a, f, t, p, r);
}

/* ------------------------------------------------------------------ basic shapes */

static bool mix_lp_array(embi_arena *a, const embi_value *fv, const embi_value *tv, uint32_t n,
                         double p, embi_value *out)
{
    for (uint32_t i = 0; i < n; i++) {
        if (!mix_lp(a, &fv[i], &tv[i], p, &out[i])) {
            return false;
        }
    }
    return true;
}

static bool mix_shape(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                      embi_value *r)
{
    if (f->kind != EMBI_V_SHAPE || t->kind != EMBI_V_SHAPE) {
        return false;
    }
    const embi_shape *fs = f->shape, *ts = t->shape;
    /* Shapes interpolate only when their reference boxes match (CSS Masking). A geometry-box-only
     * value has no shape: equal boxes stay as they are, anything else is discrete. */
    if (f->kw != t->kw || fs == nullptr || ts == nullptr) {
        if (f->kw == t->kw && fs == nullptr && ts == nullptr) {
            *r = *f;
            return true;
        }
        return false;
    }
    if (fs->kind != ts->kind || fs->evenodd != ts->evenodd || fs->has_round != ts->has_round ||
        fs->npts != ts->npts) {
        return false;
    }
    embi_shape *o = anew(a, sizeof *o);
    *o = *fs;
    switch (fs->kind) {
    case EMBI_SHAPE_INSET:
    case EMBI_SHAPE_XYWH:
    case EMBI_SHAPE_RECT:
        if (!mix_lp_array(a, fs->box, ts->box, 4, p, o->box)) {
            return false;
        }
        if (fs->has_round && !mix_lp_array(a, fs->radii, ts->radii, 8, p, o->radii)) {
            return false;
        }
        break;
    case EMBI_SHAPE_CIRCLE:
        if (!mix_lp(a, &fs->r[0], &ts->r[0], p, &o->r[0])) {
            return false;
        }
        break;
    case EMBI_SHAPE_ELLIPSE:
        if (!mix_lp_array(a, fs->r, ts->r, 2, p, o->r)) {
            return false;
        }
        break;
    case EMBI_SHAPE_POLYGON: {
        uint32_t cnt = 2 * fs->npts;
        embi_value *pts = anew(a, sizeof(embi_value) * (cnt ? cnt : 1));
        if (!mix_lp_array(a, fs->pts, ts->pts, cnt, p, pts)) {
            return false;
        }
        o->pts = pts;
        break;
    }
    case EMBI_SHAPE_PATH:
        if (!embi_str_eq(fs->path, ts->path)) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (fs->kind == EMBI_SHAPE_CIRCLE || fs->kind == EMBI_SHAPE_ELLIPSE) {
        /* at: the center (50% 50% when absent) is mixed at the start edge */
        embi_position pf = {0}, pt = {0};
        pf.x.off.kind = EMBI_V_PERCENT;
        pf.x.off.num = 50;
        pf.y.off = pf.x.off;
        pt = pf;
        if (fs->has_at) {
            pf = fs->at;
        }
        if (ts->has_at) {
            pt = ts->at;
        }
        embi_position po;
        memset(&po, 0, sizeof po);
        embi_value fx, tx, fy, ty;
        if (!axis_start(a, &pf.x, &fx) || !axis_start(a, &pt.x, &tx) ||
            !axis_start(a, &pf.y, &fy) || !axis_start(a, &pt.y, &ty)) {
            return false;
        }
        if (!mix_lp(a, &fx, &tx, p, &po.x.off) || !mix_lp(a, &fy, &ty, p, &po.y.off)) {
            return false;
        }
        o->at = po;
        /* The position is written when either side writes one; otherwise the default center stands. */
        o->has_at = fs->has_at || ts->has_at;
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_SHAPE;
    r->kw = f->kw;
    r->shape = o;
    return true;
}

/* ------------------------------------------------------------------ positions and axes */

/* Comma lists of repeatable values (backgrounds 3, masking): both lists are repeated to the
 * least common multiple of their lengths. A bare value counts as a one-item list. */
static bool rep_len(uint32_t x, uint32_t y, uint32_t *out)
{
    if (x == 0 || y == 0) {
        return false;
    }
    uint32_t g = x, h = y;
    while (h != 0) {
        uint32_t t = g % h;
        g = h;
        h = t;
    }
    uint64_t l = (uint64_t)x / g * y;
    if (l > 1024) {
        return false;
    }
    *out = (uint32_t)l;
    return true;
}

/* True when a and b are both bare or comma lists; *list tells whether the result is a list. */
static bool rep_shape(const embi_value *f, const embi_value *t, bool *list)
{
    bool fl = f->kind == EMBI_V_LIST, tl = t->kind == EMBI_V_LIST;
    if ((fl && f->sep != EMBI_SEP_COMMA) || (tl && t->sep != EMBI_SEP_COMMA)) {
        return false;
    }
    *list = fl || tl;
    return true;
}

static embi_value mk_list(embi_value *items, uint32_t n)
{
    embi_value r;
    memset(&r, 0, sizeof r);
    r.kind = EMBI_V_LIST;
    r.sep = EMBI_SEP_COMMA;
    r.list.items = items;
    r.list.n = n;
    return r;
}

static bool mix_position1(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                          embi_value *r);

static bool mix_position(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                         embi_value *r)
{
    if (f->kind != EMBI_V_LIST && t->kind != EMBI_V_LIST) {
        return mix_position1(a, f, t, p, r);
    }
    bool list;
    uint32_t fn, tn, n;
    if (!rep_shape(f, t, &list)) {
        return false;
    }
    const embi_value *fi = embi_value_items(f, &fn);
    const embi_value *ti = embi_value_items(t, &tn);
    if (!rep_len(fn, tn, &n)) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * n);
    for (uint32_t i = 0; i < n; i++) {
        if (!mix_position1(a, &fi[i % fn], &ti[i % tn], p, &items[i])) {
            return false;
        }
    }
    *r = mk_list(items, n);
    return true;
}

static bool mix_position1(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                          embi_value *r)
{
    if (f->kind != EMBI_V_POSITION || t->kind != EMBI_V_POSITION) {
        return false;
    }
    embi_position *o = anew(a, sizeof *o);
    embi_value fx, tx, fy, ty;
    if (!axis_start(a, &f->pos->x, &fx) || !axis_start(a, &t->pos->x, &tx) ||
        !axis_start(a, &f->pos->y, &fy) || !axis_start(a, &t->pos->y, &ty)) {
        return false;
    }
    if (!mix_lp(a, &fx, &tx, p, &o->x.off) || !mix_lp(a, &fy, &ty, p, &o->y.off)) {
        return false;
    }
    o->x.anchor = 0;
    o->y.anchor = 0;
    /* z: a missing z is 0px */
    embi_value fz = f->pos->z.kind == EMBI_V_NONE ? zero_length() : f->pos->z;
    embi_value tz = t->pos->z.kind == EMBI_V_NONE ? zero_length() : t->pos->z;
    if (!mix_lp(a, &fz, &tz, p, &o->z)) {
        return false;
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_POSITION;
    r->pos = o;
    return true;
}

/* background-position-x and -y: an axis or a list of axes. */
static bool mix_axis(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                     embi_value *r)
{
    uint32_t fn, tn, n;
    bool list;
    const embi_value *fi = embi_value_items(f, &fn);
    const embi_value *ti = embi_value_items(t, &tn);
    if (!rep_shape(f, t, &list) || !rep_len(fn, tn, &n)) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *fa = &fi[i % fn], *ta = &ti[i % tn];
        if (fa->kind != EMBI_V_POS_AXIS || ta->kind != EMBI_V_POS_AXIS) {
            return false;
        }
        embi_value fx, tx;
        embi_pos_axis *ax = anew(a, sizeof *ax);
        if (!axis_start(a, fa->axis, &fx) || !axis_start(a, ta->axis, &tx) ||
            !mix_lp(a, &fx, &tx, p, &ax->off)) {
            return false;
        }
        items[i].kind = EMBI_V_POS_AXIS;
        items[i].axis = ax;
    }
    if (list) {
        *r = mk_list(items, n);
    } else {
        *r = items[0];
    }
    return true;
}

/* ------------------------------------------------------------------ lists of lengths (lplist) */

static bool mix_lplist_item(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                            embi_value *r);

static bool mix_lplist(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                       embi_value *r)
{
    bool fl = f->kind == EMBI_V_LIST, tl = t->kind == EMBI_V_LIST;
    if (!fl && !tl) {
        return mix_lplist_item(a, f, t, p, r);
    }
    if ((fl && f->sep == EMBI_SEP_COMMA) || (tl && t->sep == EMBI_SEP_COMMA)) {
        bool list;
        uint32_t fn, tn, n;
        if (!rep_shape(f, t, &list) || !(fl || tl)) {
            return false;
        }
        const embi_value *fi = embi_value_items(f, &fn);
        const embi_value *ti = embi_value_items(t, &tn);
        if (!rep_len(fn, tn, &n)) {
            return false;
        }
        embi_value *items = anew(a, sizeof(embi_value) * n);
        for (uint32_t i = 0; i < n; i++) {
            if (!mix_lplist_item(a, &fi[i % fn], &ti[i % tn], p, &items[i])) {
                return false;
            }
        }
        *r = mk_list(items, n);
        return true;
    }
    if (!fl || !tl || f->list.n != t->list.n || f->sep != t->sep) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * (f->list.n ? f->list.n : 1));
    for (uint32_t i = 0; i < f->list.n; i++) {
        if (!mix_lplist_item(a, &f->list.items[i], &t->list.items[i], p, &items[i])) {
            return false;
        }
    }
    memset(r, 0, sizeof *r);
    r->kind = EMBI_V_LIST;
    r->sep = f->sep;
    r->list.items = items;
    r->list.n = f->list.n;
    return true;
}

static bool mix_lplist_item(embi_arena *a, const embi_value *f, const embi_value *t, double p,
                            embi_value *r)
{
    if (f->kind == EMBI_V_LIST || t->kind == EMBI_V_LIST) {
        return mix_lplist(a, f, t, p, r);
    }
    if (f->kind == EMBI_V_KEYWORD && t->kind == EMBI_V_KEYWORD && f->kw == t->kw) {
        *r = *f;
        return true;
    }
    if (f->kind == EMBI_V_NUMBER && t->kind == EMBI_V_NUMBER) {
        return mix_number(f, t, p, r);
    }
    return is_lp(f) && is_lp(t) && mix_lp(a, f, t, p, r);
}

/* ------------------------------------------------------------------ text-decoration-inset */

/* text-decoration-inset is `auto` or two <length-percentage | auto> (the lone keyword auto, or a list of two). `auto`
 * interpolates as a length against a length (the draft leaves its used value to the UA: 0px here) and stays `auto` against
 * `auto`. */
static bool deco_all_auto(const embi_value *v)
{
    if (v->kind == EMBI_V_KEYWORD) {
        return v->kw == EMBI_KW_auto;
    }
    if (v->kind == EMBI_V_LIST && v->list.n == 2) {
        return v->list.items[0].kind == EMBI_V_KEYWORD && v->list.items[0].kw == EMBI_KW_auto &&
               v->list.items[1].kind == EMBI_V_KEYWORD && v->list.items[1].kw == EMBI_KW_auto;
    }
    return false;
}

/* v as a list of two with every auto replaced by 0px. */
static void deco_expand(embi_arena *a, const embi_value *v, embi_value *out)
{
    embi_value *items = anew(a, sizeof(embi_value) * 2);
    for (int i = 0; i < 2; i++) {
        const embi_value *x = v->kind == EMBI_V_LIST && v->list.n == 2 ? &v->list.items[i] : v;
        if (x->kind == EMBI_V_KEYWORD && x->kw == EMBI_KW_auto) {
            memset(&items[i], 0, sizeof items[i]);
            items[i].kind = EMBI_V_LENGTH;
            items[i].unit = EMBI_U_PX;
            items[i].num = 0.0;
        } else {
            items[i] = *x;
        }
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_LIST;
    out->sep = EMBI_SEP_SPACE;
    out->list.items = items;
    out->list.n = 2;
}

/* ------------------------------------------------------------------ dispatch */

static bool mix_value(embi_arena *a, uint8_t ip, const embi_value *f, const embi_value *t,
                      double p, embi_value *r)
{
    switch (ip) {
    case EMBI_IP_LEN:
        return mix_len(a, f, t, p, r);
    case EMBI_IP_NUM:
        return mix_number(f, t, p, r);
    case EMBI_IP_INT:
        return mix_integer(f, t, p, r);
    case EMBI_IP_COLOR:
        return mix_color(f, t, p, r);
    case EMBI_IP_SHADOW:
        return mix_shadows(a, f, t, p, r);
    case EMBI_IP_FILTER:
        return mix_filters(a, f, t, p, r);
    case EMBI_IP_XFORM:
        return mix_xform(a, f, t, p, r);
    case EMBI_IP_SHAPE:
        return mix_shape(a, f, t, p, r);
    case EMBI_IP_POS:
        return mix_position(a, f, t, p, r);
    case EMBI_IP_AXIS:
        return mix_axis(a, f, t, p, r);
    case EMBI_IP_LPLIST:
        return mix_lplist(a, f, t, p, r);
    case EMBI_IP_VIS:
        return mix_visibility(f, t, p, r);
    case EMBI_IP_DISP:
        return mix_none_rule(f, t, p, true, r);
    case EMBI_IP_CV:
        return mix_none_rule(f, t, p, false, r);
    case EMBI_IP_RATIO:
        return mix_ratio(f, t, p, r);
    default:
        return false;
    }
}

embi_interp embi_anim_interp_of(embi_prop_id prop)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    return pi ? (embi_interp)pi->interp : EMBI_IP_NONE;
}

bool embi_anim_animatable(embi_prop_id prop)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    return pi != nullptr && !embi_prop_is_shorthand(prop) && pi->anim != 'N';
}

void embi_anim_mix(embi_arena *a, embi_prop_id prop, const embi_value *from, const embi_value *to,
                   double p, embi_value *out)
{
    embi_value dfrom, dto;
    if (prop == EMBI_PROP_text_decoration_inset) {
        if (deco_all_auto(from) && deco_all_auto(to)) {
            *out = *from;
            return;
        }
        deco_expand(a, from, &dfrom);
        deco_expand(a, to, &dto);
        from = &dfrom;
        to = &dto;
    }
    if (p == 0.0) {
        *out = *from;
        return;
    }
    if (p == 1.0) {
        *out = *to;
        return;
    }
    embi_value r;
    memset(&r, 0, sizeof r);
    if (mix_value(a, (uint8_t)embi_anim_interp_of(prop), from, to, p, &r)) {
        *out = r;
        return;
    }
    /* Discrete: from below one half, to from one half. */
    *out = p >= 0.5 ? *to : *from;
}

/* base + k * v for numbers, percentages, lengths and lp calc()s. */
static bool add_scalar(embi_arena *a, const embi_value *base, const embi_value *v, double k,
                       embi_value *out)
{
    if (base->kind == EMBI_V_NUMBER && v->kind == EMBI_V_NUMBER) {
        memset(out, 0, sizeof *out);
        out->kind = EMBI_V_NUMBER;
        out->num = base->num + k * v->num;
        return true;
    }
    if (base->kind == EMBI_V_PERCENT && v->kind == EMBI_V_PERCENT) {
        memset(out, 0, sizeof *out);
        out->kind = EMBI_V_PERCENT;
        out->num = base->num + k * v->num;
        return true;
    }
    if (base->kind == EMBI_V_LENGTH && v->kind == EMBI_V_LENGTH && base->unit == v->unit) {
        *out = *base;
        out->num = base->num + k * v->num;
        return true;
    }
    if (is_lp(base) && is_lp(v)) {
        const embi_calc *bn = lp_node(a, base), *vn = lp_node(a, v);
        set_calc(out, calc_op(a, EMBI_CALC_ADD, (uint8_t)(bn->type | vn->type), bn,
                              scale_node(a, vn, k)));
        return true;
    }
    return false;
}

static bool add_lplist(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                       embi_value *out);

/* One lplist item: numbers, lengths and percentages add, equal keywords stay, lists recurse. */
static bool add_lpitem(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                       embi_value *out)
{
    if (b->kind == EMBI_V_LIST || v->kind == EMBI_V_LIST) {
        return add_lplist(a, b, v, k, out);
    }
    if (b->kind == EMBI_V_KEYWORD && v->kind == EMBI_V_KEYWORD && b->kw == v->kw) {
        *out = *b;
        return true;
    }
    if (b->kind == EMBI_V_INTEGER && v->kind == EMBI_V_INTEGER) {
        memset(out, 0, sizeof *out);
        out->kind = EMBI_V_INTEGER;
        out->num = floor(b->num + k * v->num + 0.5);
        return true;
    }
    return add_scalar(a, b, v, k, out);
}

static bool add_lplist(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                       embi_value *out)
{
    if (b->kind != EMBI_V_LIST && v->kind != EMBI_V_LIST) {
        return add_lpitem(a, b, v, k, out);
    }
    if ((b->kind == EMBI_V_LIST && b->sep == EMBI_SEP_COMMA) ||
        (v->kind == EMBI_V_LIST && v->sep == EMBI_SEP_COMMA)) {
        bool list;
        uint32_t bn, vn, n;
        if (!rep_shape(b, v, &list)) {
            return false;
        }
        const embi_value *bi = embi_value_items(b, &bn);
        const embi_value *vi = embi_value_items(v, &vn);
        if (!rep_len(bn, vn, &n)) {
            return false;
        }
        embi_value *items = anew(a, sizeof(embi_value) * n);
        for (uint32_t i = 0; i < n; i++) {
            if (!add_lpitem(a, &bi[i % bn], &vi[i % vn], k, &items[i])) {
                return false;
            }
        }
        *out = mk_list(items, n);
        return true;
    }
    if (b->kind != EMBI_V_LIST || v->kind != EMBI_V_LIST || b->list.n != v->list.n ||
        b->sep != v->sep) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * (b->list.n ? b->list.n : 1));
    for (uint32_t i = 0; i < b->list.n; i++) {
        if (!add_lpitem(a, &b->list.items[i], &v->list.items[i], k, &items[i])) {
            return false;
        }
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_LIST;
    out->sep = b->sep;
    out->list.items = items;
    out->list.n = b->list.n;
    return true;
}

static bool add_position1(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                          embi_value *out);

static bool add_position(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                         embi_value *out)
{
    if (b->kind != EMBI_V_LIST && v->kind != EMBI_V_LIST) {
        return add_position1(a, b, v, k, out);
    }
    bool list;
    uint32_t bn, vn, n;
    if (!rep_shape(b, v, &list)) {
        return false;
    }
    const embi_value *bi = embi_value_items(b, &bn);
    const embi_value *vi = embi_value_items(v, &vn);
    if (!rep_len(bn, vn, &n)) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * n);
    for (uint32_t i = 0; i < n; i++) {
        if (!add_position1(a, &bi[i % bn], &vi[i % vn], k, &items[i])) {
            return false;
        }
    }
    *out = mk_list(items, n);
    return true;
}

static bool add_position1(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                          embi_value *out)
{
    if (b->kind != EMBI_V_POSITION || v->kind != EMBI_V_POSITION) {
        return false;
    }
    embi_position *o = anew(a, sizeof *o);
    embi_value bx, vx, by, vy;
    if (!axis_start(a, &b->pos->x, &bx) || !axis_start(a, &v->pos->x, &vx) ||
        !axis_start(a, &b->pos->y, &by) || !axis_start(a, &v->pos->y, &vy) ||
        !add_scalar(a, &bx, &vx, k, &o->x.off) || !add_scalar(a, &by, &vy, k, &o->y.off)) {
        return false;
    }
    embi_value bz = b->pos->z.kind == EMBI_V_NONE ? zero_length() : b->pos->z;
    embi_value vz = v->pos->z.kind == EMBI_V_NONE ? zero_length() : v->pos->z;
    if (!add_scalar(a, &bz, &vz, k, &o->z)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_POSITION;
    out->pos = o;
    return true;
}

static bool add_axis(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                     embi_value *out)
{
    uint32_t bn, vn, n;
    bool list;
    const embi_value *bi = embi_value_items(b, &bn);
    const embi_value *vi = embi_value_items(v, &vn);
    if (!rep_shape(b, v, &list) || !rep_len(bn, vn, &n)) {
        return false;
    }
    embi_value *items = anew(a, sizeof(embi_value) * n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *ba = &bi[i % bn], *va = &vi[i % vn];
        if (ba->kind != EMBI_V_POS_AXIS || va->kind != EMBI_V_POS_AXIS) {
            return false;
        }
        embi_value bx, vx;
        embi_pos_axis *ax = anew(a, sizeof *ax);
        if (!axis_start(a, ba->axis, &bx) || !axis_start(a, va->axis, &vx) ||
            !add_scalar(a, &bx, &vx, k, &ax->off)) {
            return false;
        }
        items[i].kind = EMBI_V_POS_AXIS;
        items[i].axis = ax;
    }
    if (list) {
        *out = mk_list(items, n);
    } else {
        *out = items[0];
    }
    return true;
}

/* Colors add component-wise in sRGB, with alpha clamped to 0..1. */
static bool add_color(const embi_color *b, const embi_color *v, double k, embi_color *out)
{
    embi_color sb, sv;
    if (!embi_color_to_srgb(b, &sb) || !embi_color_to_srgb(v, &sv)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->space = EMBI_CS_SRGB;
    for (int i = 0; i < 3; i++) {
        out->c[i] = sb.c[i] + k * sv.c[i];
    }
    out->alpha = clamp01(sb.alpha + k * sv.alpha);
    return true;
}

static bool add_shadow(embi_arena *a, const embi_shadow *b, const embi_shadow *v, double k,
                       embi_shadow *out)
{
    if (b->inset != v->inset || b->color.kind != EMBI_V_COLOR || v->color.kind != EMBI_V_COLOR) {
        return false;
    }
    out->inset = b->inset;
    out->color.kind = EMBI_V_COLOR;
    if (!add_color(&b->color.color, &v->color.color, k, &out->color.color)) {
        return false;
    }
    embi_value *dst[4] = {&out->x, &out->y, &out->blur, &out->spread};
    const embi_value *bs[4] = {&b->x, &b->y, &b->blur, &b->spread};
    const embi_value *vs[4] = {&v->x, &v->y, &v->blur, &v->spread};
    for (int i = 0; i < 4; i++) {
        if (!add_scalar(a, bs[i], vs[i], k, dst[i])) {
            return false;
        }
    }
    return true;
}

/* Shadow lists add pairwise, the shorter list padded with transparent zero shadows. */
static bool add_shadows(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                        embi_value *out)
{
    const embi_shadow *bp, *vp;
    uint32_t bn, vn;
    if (!shadow_list(b, &bp, &bn) || !shadow_list(v, &vp, &vn)) {
        return false;
    }
    uint32_t n = bn > vn ? bn : vn;
    if (n == 0) {
        return false;
    }
    embi_shadow *o = embi_arena_alloc(a, sizeof(embi_shadow) * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        embi_shadow sb = i < bn ? bp[i] : zero_shadow(i < vn ? vp[i].inset : false);
        embi_shadow sv = i < vn ? vp[i] : zero_shadow(i < bn ? bp[i].inset : false);
        if (!add_shadow(a, &sb, &sv, k, &o[i])) {
            return false;
        }
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_SHADOWS;
    out->shadows.p = o;
    out->shadows.n = n;
    return true;
}

/* Basic shapes of one kind add their lengths (inset, rect and xywh boxes, circle and ellipse
 * radii). */
static bool add_shape(embi_arena *a, const embi_value *b, const embi_value *v, double k,
                      embi_value *out)
{
    if (b->kind != EMBI_V_SHAPE || v->kind != EMBI_V_SHAPE) {
        return false;
    }
    const embi_shape *bs = b->shape, *vs = v->shape;
    if (bs == nullptr || vs == nullptr || b->kw != v->kw) {
        return false;
    }
    if (bs->kind != vs->kind || bs->has_round != vs->has_round || bs->evenodd != vs->evenodd) {
        return false;
    }
    embi_shape *o = anew(a, sizeof *o);
    *o = *bs;
    switch (bs->kind) {
    case EMBI_SHAPE_INSET:
    case EMBI_SHAPE_RECT:
    case EMBI_SHAPE_XYWH:
        for (int i = 0; i < 4; i++) {
            if (!add_scalar(a, &bs->box[i], &vs->box[i], k, &o->box[i])) {
                return false;
            }
        }
        if (bs->has_round) {
            for (int i = 0; i < 8; i++) {
                if (!add_scalar(a, &bs->radii[i], &vs->radii[i], k, &o->radii[i])) {
                    return false;
                }
            }
        }
        break;
    case EMBI_SHAPE_CIRCLE:
        if (!add_scalar(a, &bs->r[0], &vs->r[0], k, &o->r[0])) {
            return false;
        }
        break;
    case EMBI_SHAPE_ELLIPSE:
        if (!add_scalar(a, &bs->r[0], &vs->r[0], k, &o->r[0]) ||
            !add_scalar(a, &bs->r[1], &vs->r[1], k, &o->r[1])) {
            return false;
        }
        break;
    default:
        return false;
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_SHAPE;
    out->kw = b->kw;
    out->shape = o;
    return true;
}

/* Transform and filter lists add by concatenation (k is 1 for lists, see anim.h). */
static bool concat_lists(embi_arena *a, const embi_value *b, const embi_value *v, embi_value *out)
{
    uint32_t nb = b->kind == EMBI_V_TRANSFORM ? b->xf.n : 0;
    uint32_t nv = v->kind == EMBI_V_TRANSFORM ? v->xf.n : 0;
    if (nb == 0 && nv == 0) {
        *out = val_kw(EMBI_KW_none);
        return true;
    }
    embi_xform *items = embi_arena_alloc(a, sizeof(embi_xform) * (nb + nv), 8);
    for (uint32_t i = 0; i < nb; i++) {
        items[i] = b->xf.p[i];
    }
    for (uint32_t i = 0; i < nv; i++) {
        items[nb + i] = v->xf.p[i];
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_TRANSFORM;
    out->xf.p = items;
    out->xf.n = nb + nv;
    return true;
}

/* Filter lists add by concatenation, like transform lists. */
static bool concat_filters(embi_arena *a, const embi_value *b, const embi_value *v, embi_value *out)
{
    uint32_t nb = b->kind == EMBI_V_FILTERS ? b->filters.n : 0;
    uint32_t nv = v->kind == EMBI_V_FILTERS ? v->filters.n : 0;
    if (nb == 0 && nv == 0) {
        *out = val_kw(EMBI_KW_none);
        return true;
    }
    embi_filter *items = embi_arena_alloc(a, sizeof(embi_filter) * (nb + nv), 8);
    for (uint32_t i = 0; i < nb; i++) {
        items[i] = b->filters.p[i];
    }
    for (uint32_t i = 0; i < nv; i++) {
        items[nb + i] = v->filters.p[i];
    }
    memset(out, 0, sizeof *out);
    out->kind = EMBI_V_FILTERS;
    out->filters.p = items;
    out->filters.n = nb + nv;
    return true;
}

bool embi_anim_add_scaled(embi_arena *a, embi_prop_id prop, const embi_value *base,
                          const embi_value *v, double k, embi_value *out)
{
    uint8_t ip = (uint8_t)embi_anim_interp_of(prop);
    embi_value dbase, dv;
    if (prop == EMBI_PROP_text_decoration_inset) {
        if (deco_all_auto(base) && deco_all_auto(v)) {
            *out = *base;
            return true;
        }
        deco_expand(a, base, &dbase);
        deco_expand(a, v, &dv);
        base = &dbase;
        v = &dv;
    }
    if (add_scalar(a, base, v, k, out)) {
        return true;
    }
    switch (ip) {
    case EMBI_IP_INT:
        if ((base->kind == EMBI_V_INTEGER || base->kind == EMBI_V_NUMBER) &&
            (v->kind == EMBI_V_INTEGER || v->kind == EMBI_V_NUMBER)) {
            memset(out, 0, sizeof *out);
            out->kind = EMBI_V_INTEGER;
            out->num = floor(base->num + k * v->num + 0.5);
            return true;
        }
        return false;
    case EMBI_IP_POS:
        return add_position(a, base, v, k, out);
    case EMBI_IP_AXIS:
        return add_axis(a, base, v, k, out);
    case EMBI_IP_LPLIST:
        return add_lplist(a, base, v, k, out);
    case EMBI_IP_COLOR:
        if (base->kind == EMBI_V_COLOR && v->kind == EMBI_V_COLOR) {
            memset(out, 0, sizeof *out);
            out->kind = EMBI_V_COLOR;
            return add_color(&base->color, &v->color, k, &out->color);
        }
        return false;
    case EMBI_IP_SHADOW:
        return add_shadows(a, base, v, k, out);
    case EMBI_IP_SHAPE:
        return add_shape(a, base, v, k, out);
    case EMBI_IP_XFORM:
        if (k == 1.0 && (base->kind == EMBI_V_TRANSFORM || base->kind == EMBI_V_KEYWORD) &&
            (v->kind == EMBI_V_TRANSFORM || v->kind == EMBI_V_KEYWORD)) {
            return concat_lists(a, base, v, out);
        }
        return false;
    case EMBI_IP_FILTER:
        if (k == 1.0 && (base->kind == EMBI_V_FILTERS || base->kind == EMBI_V_KEYWORD) &&
            (v->kind == EMBI_V_FILTERS || v->kind == EMBI_V_KEYWORD)) {
            return concat_filters(a, base, v, out);
        }
        return false;
    default:
        return false;
    }
}

bool embi_anim_can_mix(embi_arena *a, embi_prop_id prop, const embi_value *from, const embi_value *to)
{
    embi_value r;
    memset(&r, 0, sizeof r);
    return mix_value(a, (uint8_t)embi_anim_interp_of(prop), from, to, 0.5, &r);
}

/* A copy of v with its currentcolor replaced by cur. */
static void resolve_color_value(const embi_value *v, const embi_color *cur, embi_value *out)
{
    *out = *v;
    if (v->kind == EMBI_V_COLOR && v->color.space == EMBI_CS_CURRENT) {
        out->color = *cur;
    }
}

void embi_anim_resolve_color(embi_arena *a, const embi_value *v, const embi_color *cur,
                             embi_value *out)
{
    *out = *v;
    if (cur == nullptr) {
        return;
    }
    if (v->kind == EMBI_V_COLOR) {
        resolve_color_value(v, cur, out);
    } else if (v->kind == EMBI_V_SHADOWS) {
        embi_shadow *o = embi_arena_alloc(a, sizeof(embi_shadow) * (v->shadows.n ? v->shadows.n : 1), 8);
        for (uint32_t i = 0; i < v->shadows.n; i++) {
            o[i] = v->shadows.p[i];
            resolve_color_value(&v->shadows.p[i].color, cur, &o[i].color);
        }
        out->shadows.p = o;
    } else if (v->kind == EMBI_V_FILTERS) {
        embi_filter *o = embi_arena_alloc(a, sizeof(embi_filter) * (v->filters.n ? v->filters.n : 1), 8);
        for (uint32_t i = 0; i < v->filters.n; i++) {
            o[i] = v->filters.p[i];
            if (o[i].fn == EMBI_FILTER_DROP_SHADOW && o[i].shadow != nullptr) {
                embi_shadow *s = embi_arena_alloc(a, sizeof(embi_shadow), 8);
                *s = *o[i].shadow;
                resolve_color_value(&s->color, cur, &s->color);
                o[i].shadow = s;
            }
        }
        out->filters.p = o;
    }
}
