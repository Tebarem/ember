/*
 * Typed compound values and shorthand grammars (W2c): gradients, image-set(), cross-fade(),
 * transform lists, shadows, filters, basic shapes, easing, grid templates, counters and quotes,
 * and the expansions of font, background, transition, animation, mask, border-image, grid,
 * container, font-variant, font-synthesis and border-radius. Every group has valid cases that
 * check the typed structure and invalid cases that must be rejected. Also the logical property
 * mapping and the font metrics of the relative length hook.
 */
#include "emb_test.h"

#include "css/props.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

static embi_prop_id prop_of(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

static uint32_t expand(embi_arena *a, const char *prop, const char *value, embi_longhand *out)
{
    return embi_css_parse_decl(a, prop_of(prop), text_of(a, value), out, EMBI_LONGHAND_MAX);
}

static const embi_value *find(const embi_longhand *lh, uint32_t n, const char *name)
{
    embi_prop_id id = prop_of(name);
    for (uint32_t i = 0; i < n; i++) {
        if (lh[i].prop == id) {
            return &lh[i].value;
        }
    }
    return nullptr;
}

/* Test fixture: an arena and a result buffer. */
typedef struct fx {
    embi_arena a;
    embi_longhand lh[EMBI_LONGHAND_MAX];
} fx;

static void fx_init(fx *f)
{
    f->a = (embi_arena){0};
    embi_arena_init(&f->a, 8192);
}

/* Parses prop: value and returns the number of longhands written. */
static uint32_t parse(fx *f, const char *prop, const char *value)
{
    return expand(&f->a, prop, value, f->lh);
}

/* True when the value is invalid; prints the offender when it is accepted. */
static bool rejects(fx *f, const char *prop, const char *value)
{
    if (parse(f, prop, value) != 0) {
        fprintf(stderr, "  %s: \"%s\" was accepted\n", prop, value);
        return false;
    }
    return true;
}

/* True when valid; prints the offender when rejected. */
static bool accepts(fx *f, const char *prop, const char *value)
{
    if (parse(f, prop, value) == 0) {
        fprintf(stderr, "  %s: \"%s\" was rejected\n", prop, value);
        return false;
    }
    return true;
}

/* The first item of a list-valued longhand (a single layer is still a one-item list). */
static const embi_value *item0(const fx *f)
{
    uint32_t n = 0;
    return embi_value_items(&f->lh[0].value, &n);
}

static bool is_px(const embi_value *v, double px)
{
    return v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX && v->num == px;
}

static bool is_pct(const embi_value *v, double pct)
{
    return v != nullptr && v->kind == EMBI_V_PERCENT && v->num == pct;
}

static bool is_kw(const embi_value *v, uint16_t kw)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == kw;
}

/* ------------------------------------------------------------------ gradients */

EMB_TEST(gradients_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "background-image", "linear-gradient(to top right, red, blue 50%, #00f)") == 1);
    EMB_REQUIRE(item0(&f)->kind == EMBI_V_IMAGE);
    const embi_image *im = item0(&f)->image;
    EMB_REQUIRE(im->kind == EMBI_IMG_GRADIENT);
    const embi_gradient *g = im->grad;
    EMB_CHECK_EQ_INT(g->kind, EMBI_GRAD_LINEAR);
    EMB_CHECK_EQ_INT(g->sides, EMBI_SIDE_TOP | EMBI_SIDE_RIGHT);
    EMB_CHECK(!g->repeating);
    EMB_REQUIRE(g->nstops == 3);
    EMB_CHECK_EQ_INT(g->stops[0].color.kind, EMBI_V_COLOR);
    EMB_CHECK_EQ_INT(g->stops[0].pos[0].kind, EMBI_V_NONE);
    EMB_CHECK(is_pct(&g->stops[1].pos[0], 50));

    EMB_REQUIRE(parse(&f, "background-image", "repeating-linear-gradient(45deg, red 0 10px, blue 10px 20px)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(g->repeating);
    EMB_CHECK_EQ_INT(g->angle.kind, EMBI_V_ANGLE);
    EMB_CHECK(g->angle.num == 45);
    EMB_REQUIRE(g->nstops == 2);
    EMB_CHECK(is_px(&g->stops[0].pos[1], 10));
    EMB_CHECK(is_px(&g->stops[1].pos[0], 10));

    /* A color hint between two stops. */
    EMB_REQUIRE(parse(&f, "background-image", "linear-gradient(red, 30%, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_REQUIRE(g->nstops == 3);
    EMB_CHECK_EQ_INT(g->stops[1].color.kind, EMBI_V_NONE);
    EMB_CHECK(is_pct(&g->stops[1].pos[0], 30));

    EMB_REQUIRE(parse(&f, "background-image", "radial-gradient(circle closest-side at 10px 20%, red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK_EQ_INT(g->kind, EMBI_GRAD_RADIAL);
    EMB_CHECK(!g->ellipse);
    EMB_CHECK_EQ_INT(g->size, EMBI_RS_CLOSEST_SIDE);
    EMB_CHECK(g->has_pos);
    EMB_CHECK(is_px(&g->pos.x.off, 10));
    EMB_CHECK(is_pct(&g->pos.y.off, 20));

    EMB_REQUIRE(parse(&f, "background-image", "radial-gradient(10px 20px, red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(g->ellipse);
    EMB_CHECK_EQ_INT(g->size, EMBI_RS_EXPLICIT);
    EMB_CHECK(is_px(&g->radius[0], 10));
    EMB_CHECK(is_px(&g->radius[1], 20));

    /* An omitted shape is an ellipse, alone or with a size keyword. */
    EMB_REQUIRE(parse(&f, "background-image", "radial-gradient(red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(g->ellipse);
    EMB_REQUIRE(parse(&f, "background-image", "radial-gradient(closest-corner at 10px 10px, red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(g->ellipse);
    EMB_CHECK_EQ_INT(g->size, EMBI_RS_CLOSEST_CORNER);
    EMB_REQUIRE(parse(&f, "background-image", "radial-gradient(circle, red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(!g->ellipse);

    EMB_REQUIRE(parse(&f, "background-image", "conic-gradient(from 90deg at center, red, blue 180deg, red)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK_EQ_INT(g->kind, EMBI_GRAD_CONIC);
    EMB_CHECK(g->angle.num == 90);
    EMB_CHECK_EQ_INT(g->stops[1].pos[0].kind, EMBI_V_ANGLE);

    /* Interpolation method and hue interpolation. */
    EMB_REQUIRE(parse(&f, "background-image", "linear-gradient(in oklch longer hue to right, red, blue)") == 1);
    g = item0(&f)->image->grad;
    EMB_CHECK(g->has_interp);
    EMB_CHECK_EQ_INT(g->interp, EMBI_IS_OKLCH);
    EMB_CHECK_EQ_INT(g->hue, EMBI_HUE_LONGER);
    EMB_REQUIRE(parse(&f, "background-image", "conic-gradient(in srgb-linear, red, blue)") == 1);
    EMB_CHECK_EQ_INT(item0(&f)->image->grad->interp, EMBI_IS_SRGB_LINEAR);

    /* A layered list keeps the order. */
    EMB_REQUIRE(parse(&f, "background-image", "url(a.png), none, linear-gradient(red, blue)") == 1);
    uint32_t n = 0;
    const embi_value *items = embi_value_items(&f.lh[0].value, &n);
    EMB_REQUIRE(n == 3);
    EMB_CHECK_EQ_INT(items[0].image->kind, EMBI_IMG_URL);
    EMB_CHECK(is_kw(&items[1], EMBI_KW_none));
    EMB_CHECK_EQ_INT(items[2].image->kind, EMBI_IMG_GRADIENT);
    embi_arena_free(&f.a);
}

EMB_TEST(gradients_reject_malformed_forms)
{
    fx f;
    fx_init(&f);
    static const char *const bad[] = {
        "linear-gradient()",
        "linear-gradient(to red, blue)",
        "linear-gradient(to top bottom, red, blue)",
        "linear-gradient(to left right, red, blue)",
        "linear-gradient(45, red, blue)",               /* unitless angle */
        "linear-gradient(red,, blue)",
        "linear-gradient(red, blue,)",
        "linear-gradient(30%, red, blue)",              /* hint without a leading stop */
        "linear-gradient(red, 10%, 20%, blue)",         /* two hints in a row */
        "linear-gradient(red, blue, 90%)",              /* trailing hint */
        "linear-gradient(in nowhere, red, blue)",
        "linear-gradient(in srgb longer hue, red, blue)", /* hue method on a rectangular space */
        "linear-gradient(in hsl, red 10% 20% 30%, blue)",
        "radial-gradient(circle 10px 20px, red, blue)", /* a circle has one radius */
        "radial-gradient(ellipse 10px, red, blue)",     /* an ellipse needs two */
        "radial-gradient(-5px, red, blue)",             /* negative radius */
        "radial-gradient(closest-side farthest-side, red, blue)",
        "radial-gradient(at, red, blue)",
        "conic-gradient(from, red, blue)",
        "conic-gradient(from 10px, red, blue)",
        "conic-gradient(red 10px, blue)",               /* length stop on a conic gradient */
        "linear-gradient(red 10deg, blue)",             /* angle stop on a linear gradient */
        "linear-gradient(red, blue))",
        "gradient(red, blue)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "background-image", bad[i]));
    }
    embi_arena_free(&f.a);
}

EMB_TEST(image_set_and_cross_fade_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "background-image", "image-set(\"a.png\" 1x, url(b.png) 2x type(\"image/png\"), url(c.png) 192dpi)") == 1);
    const embi_image *im = item0(&f)->image;
    EMB_REQUIRE(im->kind == EMBI_IMG_IMAGE_SET);
    EMB_REQUIRE(im->n == 3);
    EMB_CHECK(im->opts[0].dppx == 1.0);
    EMB_CHECK(im->opts[1].dppx == 2.0);
    EMB_CHECK(im->opts[1].type.len == 9);
    EMB_CHECK(fabs(im->opts[2].dppx - 2.0) < 1e-9);

    EMB_REQUIRE(parse(&f, "background-image", "cross-fade(url(a.png) 25%, linear-gradient(red, blue))") == 1);
    im = item0(&f)->image;
    EMB_REQUIRE(im->kind == EMBI_IMG_CROSS_FADE);
    EMB_REQUIRE(im->n == 2);
    EMB_CHECK(im->xf[0].pct == 25.0);
    EMB_CHECK(im->xf[1].pct < 0);
    EMB_CHECK(im->xf[1].img != nullptr && im->xf[1].img->kind == EMBI_IMG_GRADIENT);
    EMB_REQUIRE(parse(&f, "background-image", "cross-fade(url(a.png), red)") == 1);
    EMB_CHECK(item0(&f)->image->xf[1].img == nullptr);

    EMB_CHECK(rejects(&f, "background-image", "image-set()"));
    EMB_CHECK(rejects(&f, "background-image", "image-set(1x)"));
    EMB_CHECK(rejects(&f, "background-image", "image-set(url(a.png) 0x)"));
    EMB_CHECK(rejects(&f, "background-image", "image-set(url(a.png) 2)"));
    EMB_CHECK(rejects(&f, "background-image", "image-set(url(a.png) 1x 2x)"));
    EMB_CHECK(rejects(&f, "background-image", "cross-fade()"));
    EMB_CHECK(rejects(&f, "background-image", "cross-fade(url(a.png) 120%, url(b.png))"));
    EMB_CHECK(rejects(&f, "background-image", "cross-fade(url(a.png) -5%, url(b.png))"));
    EMB_CHECK(rejects(&f, "background-image", "cross-fade(url(a.png) 5px, url(b.png))"));
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ transforms */

EMB_TEST(transform_lists_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "transform", "translate(10px, 5%) rotate(45deg) scale(2) skewX(10deg)") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_TRANSFORM);
    EMB_REQUIRE(f.lh[0].value.xf.n == 4);
    const embi_xform *x = f.lh[0].value.xf.p;
    EMB_CHECK_EQ_INT(x[0].fn, EMBI_XF_TRANSLATE);
    EMB_CHECK_EQ_INT(x[0].n, 2);
    EMB_CHECK(is_px(&x[0].args[0], 10));
    EMB_CHECK(is_pct(&x[0].args[1], 5));
    EMB_CHECK_EQ_INT(x[1].fn, EMBI_XF_ROTATE);
    EMB_CHECK(x[1].args[0].kind == EMBI_V_ANGLE && x[1].args[0].num == 45);
    EMB_CHECK_EQ_INT(x[2].fn, EMBI_XF_SCALE);
    EMB_CHECK_EQ_INT(x[2].n, 1);
    EMB_CHECK(x[2].args[0].num == 2);
    EMB_CHECK_EQ_INT(x[3].fn, EMBI_XF_SKEWX);

    EMB_REQUIRE(parse(&f, "transform", "matrix(1, 0, 0, 1, 10, 20)") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.xf.p[0].n, 6);
    EMB_REQUIRE(parse(&f, "transform", "matrix3d(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.xf.p[0].n, 16);
    EMB_REQUIRE(parse(&f, "transform", "perspective(100px) rotate3d(1, 0, 0, 10deg) translate3d(1px, 2px, 3px)") == 1);
    EMB_REQUIRE(parse(&f, "transform", "rotate(0)") == 1); /* unitless zero is an angle */
    EMB_REQUIRE(parse(&f, "transform", "scale(50%)") == 1);
    EMB_CHECK(f.lh[0].value.xf.p[0].args[0].num == 0.5);
    EMB_REQUIRE(parse(&f, "transform", "none") == 1);
    EMB_CHECK(is_kw(&f.lh[0].value, EMBI_KW_none));

    /* The individual transform properties store one fully populated function. */
    EMB_REQUIRE(parse(&f, "translate", "10px") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.xf.p[0].fn, EMBI_XF_TRANSLATE3D);
    EMB_CHECK(is_px(&f.lh[0].value.xf.p[0].args[1], 0));
    EMB_REQUIRE(parse(&f, "rotate", "x 30deg") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.xf.p[0].fn, EMBI_XF_ROTATE3D);
    EMB_CHECK(f.lh[0].value.xf.p[0].args[0].num == 1);
    EMB_CHECK(f.lh[0].value.xf.p[0].args[3].num == 30);
    EMB_REQUIRE(parse(&f, "scale", "2 3") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.xf.p[0].fn, EMBI_XF_SCALE3D);
    EMB_CHECK(f.lh[0].value.xf.p[0].args[1].num == 3);
    EMB_CHECK(f.lh[0].value.xf.p[0].args[2].num == 1);
    embi_arena_free(&f.a);
}

EMB_TEST(transform_lists_reject_malformed_forms)
{
    fx f;
    fx_init(&f);
    static const char *const bad[] = {
        "rotate(45)",                 /* unitless non-zero angle */
        "rotate(45px)",
        "rotate()",
        "rotate(1deg, 2deg)",
        "translate()",
        "translate(10px, 20px, 30px)",
        "translate(1deg)",
        "translateX(10px, 20px)",
        "translateZ(10%)",            /* percentages are not allowed for z */
        "scale()",
        "scale(1, 2, 3)",
        "scale(10px)",
        "skew(10px)",
        "matrix(1, 2, 3)",
        "matrix(1, 2, 3, 4, 5, 6, 7)",
        "matrix3d(1, 2, 3)",
        "perspective(-10px)",
        "rotate3d(1, 0, 0)",
        "rotate3d(1, 0, 0, 10)",
        "foo(1)",
        "translate(10px) ,",
        "translate(10px), rotate(5deg)",
        "none rotate(5deg)",
        "rotate(5deg) none",
        "translate(10px))",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "transform", bad[i]));
    }
    EMB_CHECK(rejects(&f, "translate", "10px 20px 30px 40px"));
    EMB_CHECK(rejects(&f, "rotate", "x"));
    EMB_CHECK(rejects(&f, "rotate", "1 2 3"));
    EMB_CHECK(rejects(&f, "scale", "1 2 3 4"));
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ shadows, filters */

EMB_TEST(shadows_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "box-shadow", "inset 1px 2px 3px 4px red, 0 0 5px rgba(0,0,0,.5)") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_SHADOWS);
    EMB_REQUIRE(f.lh[0].value.shadows.n == 2);
    const embi_shadow *s = f.lh[0].value.shadows.p;
    EMB_CHECK(s[0].inset);
    EMB_CHECK(is_px(&s[0].x, 1));
    EMB_CHECK(is_px(&s[0].y, 2));
    EMB_CHECK(is_px(&s[0].blur, 3));
    EMB_CHECK(is_px(&s[0].spread, 4));
    EMB_CHECK_EQ_INT(s[0].color.kind, EMBI_V_COLOR);
    EMB_CHECK(!s[1].inset);
    EMB_CHECK(is_px(&s[1].spread, 0));
    EMB_CHECK(is_px(&s[1].blur, 5));

    /* The color may come first or last, inset anywhere around the lengths. */
    EMB_REQUIRE(parse(&f, "box-shadow", "red 1px 1px inset") == 1);
    EMB_CHECK(f.lh[0].value.shadows.p[0].inset);
    EMB_REQUIRE(parse(&f, "box-shadow", "none") == 1);
    EMB_CHECK(is_kw(&f.lh[0].value, EMBI_KW_none));

    EMB_REQUIRE(parse(&f, "text-shadow", "1px 1px 2px red, -1px 0 blue") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.shadows.n, 2);

    static const char *const bad_box[] = {
        "1px",                    /* needs two lengths */
        "1px 2px 3px 4px 5px",
        "inset",
        "red",
        "1px 2px -3px",           /* negative blur */
        "inset inset 1px 1px",
        "red blue 1px 1px",
        "1px 2px red 3px",        /* lengths must be contiguous */
        "1px 1px red,",
        ", 1px 1px red",
        "none, 1px 1px",
        "1% 1px",
    };
    for (size_t i = 0; i < sizeof bad_box / sizeof bad_box[0]; i++) {
        EMB_CHECK(rejects(&f, "box-shadow", bad_box[i]));
    }
    /* text-shadow has neither spread nor inset. */
    EMB_CHECK(rejects(&f, "text-shadow", "1px 2px 3px 4px red"));
    EMB_CHECK(rejects(&f, "text-shadow", "inset 1px 1px"));
    EMB_CHECK(rejects(&f, "text-shadow", "1px"));
    embi_arena_free(&f.a);
}

EMB_TEST(filters_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "filter", "blur(2px) brightness(150%) hue-rotate(90deg) grayscale() drop-shadow(1px 2px 3px red) url(#f)") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_FILTERS);
    EMB_REQUIRE(f.lh[0].value.filters.n == 6);
    const embi_filter *fl = f.lh[0].value.filters.p;
    EMB_CHECK_EQ_INT(fl[0].fn, EMBI_FILTER_BLUR);
    EMB_CHECK(is_px(&fl[0].arg, 2));
    EMB_CHECK_EQ_INT(fl[1].fn, EMBI_FILTER_BRIGHTNESS);
    EMB_CHECK(fl[1].arg.num == 1.5);
    EMB_CHECK_EQ_INT(fl[2].fn, EMBI_FILTER_HUE_ROTATE);
    EMB_CHECK(fl[2].arg.num == 90);
    EMB_CHECK_EQ_INT(fl[3].fn, EMBI_FILTER_GRAYSCALE);
    EMB_CHECK(fl[3].arg.num == 1.0); /* omitted: the default amount */
    EMB_CHECK_EQ_INT(fl[4].fn, EMBI_FILTER_DROP_SHADOW);
    EMB_REQUIRE(fl[4].shadow != nullptr);
    EMB_CHECK(is_px(&fl[4].shadow->blur, 3));
    EMB_CHECK_EQ_INT(fl[5].fn, EMBI_FILTER_URL);
    EMB_REQUIRE(parse(&f, "backdrop-filter", "blur(4px)") == 1);
    EMB_REQUIRE(parse(&f, "filter", "none") == 1);
    EMB_REQUIRE(parse(&f, "filter", "opacity(0.5) saturate(200%) sepia(0) invert(1) contrast(2)") == 1);

    static const char *const bad[] = {
        "blur(-2px)",
        "blur(10%)",
        "blur(2)",
        "brightness(-1)",
        "brightness(10px)",
        "grayscale(1 2)",
        "hue-rotate(10)",
        "hue-rotate(10px)",
        "drop-shadow()",
        "drop-shadow(1px)",
        "drop-shadow(1px 2px 3px 4px red)",
        "drop-shadow(inset 1px 2px)",
        "drop-shadow(1px 2px -3px)",
        "blur(2px),blur(1px)",
        "none blur(1px)",
        "sparkle(1)",
        "blur",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "filter", bad[i]));
    }
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ shapes and easing */

EMB_TEST(basic_shapes_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "clip-path", "inset(1px 2px round 3px / 4px) border-box") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_SHAPE);
    EMB_CHECK_EQ_INT(f.lh[0].value.kw, EMBI_KW_border_box);
    const embi_shape *sh = f.lh[0].value.shape;
    EMB_REQUIRE(sh != nullptr);
    EMB_CHECK_EQ_INT(sh->kind, EMBI_SHAPE_INSET);
    EMB_CHECK(sh->has_round);
    EMB_CHECK(is_px(&sh->box[0], 1));
    EMB_CHECK(is_px(&sh->box[1], 2));
    EMB_CHECK(is_px(&sh->box[2], 1));
    EMB_CHECK(is_px(&sh->box[3], 2));
    EMB_CHECK(is_px(&sh->radii[0], 3));
    EMB_CHECK(is_px(&sh->radii[4], 4));

    EMB_REQUIRE(parse(&f, "clip-path", "circle(50% at 10px bottom)") == 1);
    sh = f.lh[0].value.shape;
    EMB_CHECK_EQ_INT(sh->kind, EMBI_SHAPE_CIRCLE);
    EMB_CHECK(sh->has_at);
    EMB_CHECK(is_pct(&sh->r[0], 50));
    EMB_CHECK(is_pct(&sh->at.y.off, 100));

    EMB_REQUIRE(parse(&f, "clip-path", "ellipse(closest-side 20px)") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.shape->kind, EMBI_SHAPE_ELLIPSE);

    EMB_REQUIRE(parse(&f, "clip-path", "polygon(evenodd, 0 0, 100% 0, 50% 100%)") == 1);
    sh = f.lh[0].value.shape;
    EMB_CHECK_EQ_INT(sh->kind, EMBI_SHAPE_POLYGON);
    EMB_CHECK(sh->evenodd);
    EMB_CHECK_EQ_INT(sh->npts, 3);

    EMB_REQUIRE(parse(&f, "clip-path", "path(\"M0 0 L10 10 H20 V5 Z\")") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.shape->kind, EMBI_SHAPE_PATH);
    EMB_REQUIRE(parse(&f, "clip-path", "xywh(0 0 100px 50%)") == 1);
    EMB_REQUIRE(parse(&f, "clip-path", "rect(auto 10px auto 0)") == 1);
    EMB_REQUIRE(parse(&f, "clip-path", "padding-box") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.kind, EMBI_V_SHAPE);
    EMB_REQUIRE(parse(&f, "clip-path", "none") == 1);

    static const char *const bad[] = {
        "inset()",
        "inset(1px 2px 3px 4px 5px)",
        "inset(1px round)",
        "circle(-5px)",
        "circle(10px 20px)",
        "circle(at)",
        "ellipse(10px)",
        "polygon()",
        "polygon(0 0, 10px)",
        "polygon(0 0 10px 10px)",
        "polygon(nonzero evenodd, 0 0)",
        "path()",
        "path(\"M0 0 Q\")",
        "path(\"hello\")",
        "path(M0 0)",
        "xywh(0 0 -1px 1px)",
        "rect(1px 2px 3px)",
        "border-box padding-box",
        "circle(10px) circle(10px)",
        "blob(10px)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "clip-path", bad[i]));
    }
    EMB_CHECK(accepts(&f, "clip-path", "inset(-1px)"));
    embi_arena_free(&f.a);
}

EMB_TEST(easing_functions_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "transition-timing-function", "cubic-bezier(0.1, 0.7, 1, 0.1), steps(4, jump-both), ease-in, linear") == 1);
    uint32_t n = 0;
    const embi_value *it = embi_value_items(&f.lh[0].value, &n);
    EMB_REQUIRE(n == 4);
    EMB_REQUIRE(it[0].kind == EMBI_V_EASING);
    EMB_CHECK_EQ_INT(it[0].easing->kind, EMBI_EASE_CUBIC);
    EMB_CHECK(it[0].easing->c[1] == 0.7);
    EMB_CHECK_EQ_INT(it[1].easing->kind, EMBI_EASE_STEPS);
    EMB_CHECK_EQ_INT(it[1].easing->steps, 4);
    EMB_CHECK_EQ_INT(it[1].easing->jump, EMBI_JUMP_BOTH);
    EMB_CHECK(is_kw(&it[2], EMBI_KW_ease_in));
    EMB_CHECK(is_kw(&it[3], EMBI_KW_linear));

    /* y values may leave 0..1; steps default to jump-end. */
    EMB_REQUIRE(parse(&f, "animation-timing-function", "cubic-bezier(0.5, -2, 0.5, 3)") == 1);
    EMB_REQUIRE(parse(&f, "animation-timing-function", "steps(1)") == 1);
    EMB_CHECK_EQ_INT(item0(&f)->easing->jump, EMBI_JUMP_END);

    /* linear(): missing first/last inputs are filled in and inputs are made non-decreasing. */
    EMB_REQUIRE(parse(&f, "transition-timing-function", "linear(0, 0.25 25% 75%, 0.5 20%, 1)") == 1);
    const embi_easing *e = item0(&f)->easing;
    EMB_REQUIRE(e != nullptr && e->kind == EMBI_EASE_LINEAR_FN);
    EMB_REQUIRE(e->npts == 5);
    EMB_CHECK(e->pts[0].in == 0.0);
    EMB_CHECK(e->pts[1].in == 0.25 && e->pts[2].in == 0.75);
    EMB_CHECK(e->pts[3].in == 0.75); /* 20% is clamped up to the running maximum */
    EMB_CHECK(e->pts[4].in == 1.0);
    for (uint32_t i = 1; i < e->npts; i++) {
        EMB_CHECK(e->pts[i].in >= e->pts[i - 1].in);
    }

    static const char *const bad[] = {
        "cubic-bezier(2, 0, 0.5, 1)",   /* x outside 0..1 */
        "cubic-bezier(0, 0, -1, 1)",
        "cubic-bezier(0, 0, 0.5)",
        "cubic-bezier(0, 0, 0.5, 1, 1)",
        "cubic-bezier(0, 0, 0.5, 1px)",
        "steps()",
        "steps(0)",
        "steps(1.5)",
        "steps(-1)",
        "steps(0, jump-none)",           /* jump-none needs at least 2 steps */
        "steps(1, jump-none)",
        "steps(2, sideways)",
        "linear()",
        "linear(0 10px)",
        "linear(0 150%)",
        "linear(0 1 2)",
        "bounce",
        "ease-in-out-in",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "transition-timing-function", bad[i]));
    }
    EMB_CHECK(accepts(&f, "transition-timing-function", "steps(2, jump-none)"));
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ grid */

EMB_TEST(grid_templates_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "grid-template-columns", "[full-start] minmax(1em, 1fr) [main-start] 40px repeat(2, [a b] 1fr) fit-content(20%) [full-end]") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_TRACKS);
    const embi_grid_tracks *t = f.lh[0].value.tracks;
    EMB_CHECK(!t->subgrid);
    EMB_REQUIRE(t->n == 7);
    EMB_CHECK_EQ_INT(t->items[0].kind, EMBI_GI_NAMES);
    EMB_CHECK_EQ_INT(t->items[0].n, 1);
    EMB_CHECK_EQ_INT(t->items[1].kind, EMBI_GI_TRACK);
    EMB_CHECK_EQ_INT(t->items[1].size.kind, EMBI_TS_MINMAX);
    EMB_CHECK_EQ_INT(t->items[1].size.max.kind, EMBI_TB_FR);
    EMB_CHECK_EQ_INT(t->items[4].kind, EMBI_GI_REPEAT);
    EMB_CHECK_EQ_INT(t->items[4].rep, EMBI_GR_COUNT);
    EMB_CHECK_EQ_INT(t->items[4].count, 2);
    EMB_CHECK_EQ_INT(t->items[4].n, 2);
    EMB_CHECK_EQ_INT(t->items[5].size.kind, EMBI_TS_FIT_CONTENT);

    EMB_REQUIRE(parse(&f, "grid-template-columns", "repeat(auto-fill, minmax(100px, 1fr))") == 1);
    t = f.lh[0].value.tracks;
    EMB_CHECK(t->has_auto_repeat);
    EMB_CHECK_EQ_INT(t->items[0].rep, EMBI_GR_AUTO_FILL);
    EMB_REQUIRE(parse(&f, "grid-template-rows", "subgrid [a] [b c]") == 1);
    EMB_CHECK(f.lh[0].value.tracks->subgrid);
    EMB_REQUIRE(parse(&f, "grid-template-rows", "none") == 1);
    EMB_CHECK(is_kw(&f.lh[0].value, EMBI_KW_none));
    EMB_REQUIRE(parse(&f, "grid-template-columns", "min-content max-content auto 10%") == 1);

    static const char *const bad[] = {
        "1fr ]",
        "[a",
        "[a b",
        "[1a] 1fr",
        "[span] 1fr",
        "repeat()",
        "repeat(0, 1fr)",
        "repeat(-1, 1fr)",
        "repeat(2)",
        "repeat(2, )",
        "repeat(2, repeat(2, 1fr))",
        "repeat(auto-fill, 1fr) repeat(auto-fill, 1fr)", /* only one auto repeat */
        "repeat(auto-fill, minmax(auto, 1fr))",          /* auto-repeat needs fixed sizes */
        "repeat(auto-fill, 1fr)",                        /* ... which fr is not */
        "repeat(auto-fit, min-content)",
        "minmax(1fr, 2fr)",                              /* fr is only a max */
        "minmax(10px)",
        "minmax(10px, 20px, 30px)",
        "fit-content()",
        "fit-content(auto)",
        "fit-content(-1px)",
        "-1px",
        "-1fr",
        "1fr,",
        "1fr, 2fr",
        "subgrid 1fr",
        "none 1fr",
        "10",
        "red",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "grid-template-columns", bad[i]));
    }
    EMB_CHECK(rejects(&f, "grid-auto-rows", "subgrid"));
    EMB_CHECK(rejects(&f, "grid-auto-rows", "[a] 1fr"));
    embi_arena_free(&f.a);
}

EMB_TEST(grid_lines_and_areas_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "grid-row-start", "span 2") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_GRID_LINE);
    EMB_CHECK(f.lh[0].value.gline->span);
    EMB_CHECK_EQ_INT(f.lh[0].value.gline->num, 2);
    EMB_REQUIRE(parse(&f, "grid-row-start", "3 header") == 1);
    EMB_CHECK(!f.lh[0].value.gline->span);
    EMB_CHECK_EQ_INT(f.lh[0].value.gline->num, 3);
    EMB_CHECK(f.lh[0].value.gline->name != 0);
    EMB_REQUIRE(parse(&f, "grid-row-start", "-1") == 1);
    EMB_CHECK_EQ_INT(f.lh[0].value.gline->num, -1);
    EMB_REQUIRE(parse(&f, "grid-column-end", "span a") == 1);
    EMB_CHECK(f.lh[0].value.gline->span);
    EMB_CHECK_EQ_INT(f.lh[0].value.gline->num, 1); /* `span a` spans one track */
    EMB_CHECK(f.lh[0].value.gline->name != 0);

    static const char *const bad[] = {
        "0", "span 0", "span -1", "span", "span span", "1.5", "span 2 3", "auto auto", "1px",
        "span 2 a b", "a b", "1 2", "span auto", "-0",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "grid-row-start", bad[i]));
    }

    EMB_REQUIRE(parse(&f, "grid-template-areas", "\"a a b\" \"c d b\"") == 1);
    EMB_CHECK(rejects(&f, "grid-template-areas", "\"a b\" \"c\""));       /* ragged */
    EMB_CHECK(rejects(&f, "grid-template-areas", "\"a b a\""));           /* a is not a rectangle */
    EMB_CHECK(rejects(&f, "grid-template-areas", "\"a b\" \"b a\""));
    EMB_CHECK(rejects(&f, "grid-template-areas", "\"a b\" \"a a\""));     /* L shaped */
    EMB_CHECK(rejects(&f, "grid-template-areas", "\"\""));
    EMB_CHECK(rejects(&f, "grid-template-areas", "a b"));
    EMB_CHECK(accepts(&f, "grid-template-areas", "\"a . b\" \". . b\""));
    EMB_CHECK(accepts(&f, "grid-template-areas", "\"a ... b\""));
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ counters, quotes, content */

EMB_TEST(counters_quotes_and_content_are_typed)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "counter-reset", "a 2 b c -3") == 1);
    uint32_t n = 0;
    const embi_value *it = embi_value_items(&f.lh[0].value, &n);
    EMB_REQUIRE(n == 3); /* one (name, integer) pair per counter; the omitted integer is filled in */
    EMB_REQUIRE(it[0].kind == EMBI_V_LIST && it[0].list.n == 2);
    EMB_CHECK_EQ_INT(it[0].list.items[0].kind, EMBI_V_IDENT);
    EMB_CHECK(it[0].list.items[1].num == 2);
    EMB_CHECK(it[1].list.items[1].num == 0);
    EMB_CHECK(it[2].list.items[1].num == -3);
    EMB_REQUIRE(parse(&f, "counter-increment", "a b 2") == 1);
    EMB_REQUIRE(parse(&f, "counter-set", "none") == 1);
    EMB_CHECK(rejects(&f, "counter-reset", "1 a"));
    EMB_CHECK(rejects(&f, "counter-reset", "a 1.5"));
    EMB_CHECK(rejects(&f, "counter-reset", "a 1px"));
    EMB_CHECK(rejects(&f, "counter-reset", "none a"));
    EMB_CHECK(rejects(&f, "counter-increment", "a,b"));
    EMB_CHECK(rejects(&f, "counter-increment", "inherit a"));
    EMB_CHECK(rejects(&f, "counter-increment", "\"a\""));

    EMB_REQUIRE(parse(&f, "quotes", "\"<\" \">\" '(' ')'") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_LIST);
    EMB_CHECK_EQ_INT(f.lh[0].value.list.n, 4);
    EMB_CHECK_EQ_INT(f.lh[0].value.list.items[0].kind, EMBI_V_STRING);
    EMB_REQUIRE(parse(&f, "quotes", "auto") == 1);
    EMB_REQUIRE(parse(&f, "quotes", "none") == 1);
    EMB_CHECK(rejects(&f, "quotes", "\"<\""));                /* odd count */
    EMB_CHECK(rejects(&f, "quotes", "\"<\" \">\" \"'\""));
    EMB_CHECK(rejects(&f, "quotes", "<  >"));
    EMB_CHECK(rejects(&f, "quotes", "auto \"a\" \"b\""));
    EMB_CHECK(rejects(&f, "quotes", "\"a\", \"b\""));

    EMB_REQUIRE(parse(&f, "content", "\"[\" counter(item, decimal) \". \" counters(item, \".\") attr(title) open-quote url(i.png) \"]\" / \"alt text\"") == 1);
    it = embi_value_items(&f.lh[0].value, &n);
    EMB_REQUIRE(n == 9);
    EMB_CHECK_EQ_INT(f.lh[0].value.aux, 1); /* one alt item */
    EMB_CHECK_EQ_INT(it[1].kind, EMBI_V_FUNC);
    EMB_CHECK_EQ_INT(it[3].kind, EMBI_V_FUNC);
    EMB_CHECK_EQ_INT(it[4].kind, EMBI_V_FUNC);
    EMB_CHECK(is_kw(&it[5], EMBI_KW_open_quote));
    EMB_REQUIRE(parse(&f, "content", "normal") == 1);
    EMB_REQUIRE(parse(&f, "content", "none") == 1);
    EMB_REQUIRE(parse(&f, "content", "counter(c, upper-roman)") == 1);

    static const char *const bad[] = {
        "counter()",
        "counter(1)",
        "counter(a, b, c)",
        "counter(a,)",
        "counters(a)",
        "counters(a, b, c, d)",
        "counters(a, 1)",
        "attr()",
        "attr(1)",
        "normal \"a\"",
        "none \"a\"",
        "/ \"alt\"",
        "\"a\" / ",
        "\"a\" / 1",
        "10px",
        "red",
        "\"a\",\"b\"",
        "open-quote close-quote bogus",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "content", bad[i]));
    }
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ border-radius */

EMB_TEST(border_radius_longhands_are_pairs_and_shorthand_has_slash)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "border-top-left-radius", "10px 20%") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_LIST);
    EMB_REQUIRE(f.lh[0].value.list.n == 2);
    EMB_CHECK(is_px(&f.lh[0].value.list.items[0], 10));
    EMB_CHECK(is_pct(&f.lh[0].value.list.items[1], 20));
    /* One value repeats for both axes. */
    EMB_REQUIRE(parse(&f, "border-top-left-radius", "7px") == 1);
    EMB_REQUIRE(f.lh[0].value.kind == EMBI_V_LIST);
    EMB_CHECK(is_px(&f.lh[0].value.list.items[0], 7));
    EMB_CHECK(is_px(&f.lh[0].value.list.items[1], 7));
    EMB_CHECK(rejects(&f, "border-top-left-radius", "1px 2px 3px"));
    EMB_CHECK(rejects(&f, "border-top-left-radius", "-1px"));
    EMB_CHECK(rejects(&f, "border-top-left-radius", "1px / 2px"));
    EMB_CHECK(rejects(&f, "border-top-left-radius", "auto"));

    /* Four horizontal and four vertical radii: TL TR BR BL. */
    EMB_REQUIRE(parse(&f, "border-radius", "1px 2px 3px 4px / 5px 6px 7px 8px") == 4);
    static const char *const corners[] = {
        "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
        "border-bottom-left-radius",
    };
    for (int i = 0; i < 4; i++) {
        const embi_value *v = find(f.lh, 4, corners[i]);
        EMB_REQUIRE(v != nullptr && v->kind == EMBI_V_LIST && v->list.n == 2);
        EMB_CHECK(is_px(&v->list.items[0], i + 1));
        EMB_CHECK(is_px(&v->list.items[1], i + 5));
    }
    /* The usual short forms of both halves. */
    EMB_REQUIRE(parse(&f, "border-radius", "10px / 20px 30px") == 4);
    const embi_value *tl = find(f.lh, 4, "border-top-left-radius");
    const embi_value *tr = find(f.lh, 4, "border-top-right-radius");
    EMB_CHECK(is_px(&tl->list.items[0], 10) && is_px(&tl->list.items[1], 20));
    EMB_CHECK(is_px(&tr->list.items[0], 10) && is_px(&tr->list.items[1], 30));
    EMB_REQUIRE(parse(&f, "border-radius", "1px 2px 3px / 4px 5px 6px") == 4);
    const embi_value *bl = find(f.lh, 4, "border-bottom-left-radius");
    EMB_CHECK(is_px(&bl->list.items[0], 2) && is_px(&bl->list.items[1], 5));
    EMB_REQUIRE(parse(&f, "border-radius", "50%") == 4);
    EMB_CHECK(is_pct(&find(f.lh, 4, "border-bottom-right-radius")->list.items[1], 50));

    static const char *const bad[] = {
        "1px 2px 3px 4px 5px", "/ 1px", "1px /", "1px / / 2px", "1px / 2px / 3px",
        "1px / 2px 3px 4px 5px 6px", "-1px", "1px / -2px", "auto", "1px, 2px", "1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "border-radius", bad[i]));
    }
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ font */

EMB_TEST(font_shorthand_splits_into_typed_longhands)
{
    fx f;
    fx_init(&f);
    EMB_REQUIRE(parse(&f, "font", "italic small-caps bold condensed 12px/1.5 \"Helvetica Neue\", Arial, sans-serif") == 19);
    EMB_CHECK_EQ_INT(find(f.lh, 19, "font-style")->kind, EMBI_V_KEYWORD);
    EMB_CHECK(is_kw(find(f.lh, 19, "font-style"), EMBI_KW_italic));
    EMB_CHECK(is_kw(find(f.lh, 19, "font-variant-caps"), EMBI_KW_small_caps));
    EMB_CHECK(find(f.lh, 19, "font-weight")->num == 700 || is_kw(find(f.lh, 19, "font-weight"), EMBI_KW_bold));
    EMB_CHECK(is_kw(find(f.lh, 19, "font-stretch"), EMBI_KW_condensed) ||
              find(f.lh, 19, "font-stretch")->kind == EMBI_V_PERCENT);
    EMB_CHECK(is_px(find(f.lh, 19, "font-size"), 12));
    const embi_value *lh = find(f.lh, 19, "line-height");
    EMB_REQUIRE(lh != nullptr);
    EMB_CHECK_EQ_INT(lh->kind, EMBI_V_NUMBER);
    EMB_CHECK(lh->num == 1.5);
    const embi_value *fam = find(f.lh, 19, "font-family");
    EMB_REQUIRE(fam != nullptr && fam->kind == EMBI_V_LIST);
    EMB_CHECK_EQ_INT(fam->list.n, 3);
    EMB_CHECK_EQ_INT(fam->list.items[0].kind, EMBI_V_STRING);

    /* Omitted longhands go back to initial: no style, weight, line-height. */
    EMB_REQUIRE(parse(&f, "font", "12px serif") == 19);
    EMB_CHECK(is_kw(find(f.lh, 19, "font-style"), EMBI_KW_normal));
    EMB_CHECK(is_kw(find(f.lh, 19, "line-height"), EMBI_KW_normal));
    EMB_CHECK(is_kw(find(f.lh, 19, "font-variant-caps"), EMBI_KW_normal));
    EMB_REQUIRE(parse(&f, "font", "normal normal normal 14px/20px monospace") == 19);
    EMB_CHECK(is_px(find(f.lh, 19, "line-height"), 20));
    EMB_REQUIRE(parse(&f, "font", "bold 1em/110% serif") == 19);
    EMB_CHECK(is_pct(find(f.lh, 19, "line-height"), 110));
    EMB_REQUIRE(parse(&f, "font", "300 large serif") == 19);
    EMB_CHECK(find(f.lh, 19, "font-weight")->num == 300);
    EMB_REQUIRE(parse(&f, "font", "oblique 10deg 12px serif") == 19);
    EMB_REQUIRE(parse(&f, "font", "12px / 2 serif") == 19);
    /* System fonts. */
    EMB_REQUIRE(parse(&f, "font", "caption") == 19);
    EMB_REQUIRE(parse(&f, "font", "status-bar") == 19);

    static const char *const bad[] = {
        "serif",                       /* size is required */
        "12px",                        /* so is the family */
        "bold serif",
        "12px/ serif",
        "12px/-1 serif",               /* negative line-height */
        "/1.5 12px serif",
        "italic italic 12px serif",    /* repeated style */
        "bold bold 12px serif",
        "small-caps small-caps 12px serif",
        "condensed expanded 12px serif",
        "12px 14px serif",
        "-12px serif",
        "12px/1.5/2 serif",
        "caption 12px serif",
        "caption bold",
        "1001 12px serif",              /* weight out of range */
        "0 12px serif",
        "12px serif,",
        "12px , serif",
        "12px 'a' b",
        "normal normal normal normal normal 12px serif", /* more than 4 prefix keywords */
        "bolder lighter 12px serif",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "font", bad[i]));
    }
    embi_arena_free(&f.a);
}

EMB_TEST(font_variant_and_synthesis_shorthands)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "font-variant", "small-caps oldstyle-nums no-common-ligatures");
    EMB_REQUIRE(n >= 6);
    EMB_CHECK(is_kw(find(f.lh, n, "font-variant-caps"), EMBI_KW_small_caps));
    EMB_CHECK(find(f.lh, n, "font-variant-numeric") != nullptr);
    EMB_CHECK(find(f.lh, n, "font-variant-ligatures") != nullptr);
    EMB_CHECK(is_kw(find(f.lh, n, "font-variant-position"), EMBI_KW_normal));
    EMB_REQUIRE(parse(&f, "font-variant", "normal") >= 6);
    n = parse(&f, "font-variant", "none");
    EMB_REQUIRE(n >= 6);
    EMB_CHECK(is_kw(find(f.lh, n, "font-variant-ligatures"), EMBI_KW_none));

    EMB_CHECK(rejects(&f, "font-variant", "small-caps small-caps"));
    EMB_CHECK(rejects(&f, "font-variant", "small-caps all-small-caps"));
    EMB_CHECK(rejects(&f, "font-variant", "bogus"));
    EMB_CHECK(rejects(&f, "font-variant", "normal small-caps"));
    EMB_CHECK(rejects(&f, "font-variant", "none small-caps"));
    EMB_CHECK(rejects(&f, "font-variant", "super sub"));
    EMB_CHECK(rejects(&f, "font-variant", "small-caps,"));
    EMB_CHECK(rejects(&f, "font-variant", "12px"));

    n = parse(&f, "font-synthesis", "weight small-caps");
    EMB_REQUIRE(n >= 3);
    EMB_CHECK(is_kw(find(f.lh, n, "font-synthesis-weight"), EMBI_KW_auto));
    EMB_CHECK(is_kw(find(f.lh, n, "font-synthesis-small-caps"), EMBI_KW_auto));
    EMB_CHECK(is_kw(find(f.lh, n, "font-synthesis-style"), EMBI_KW_none));
    n = parse(&f, "font-synthesis", "none");
    EMB_REQUIRE(n >= 3);
    EMB_CHECK(is_kw(find(f.lh, n, "font-synthesis-weight"), EMBI_KW_none));
    EMB_CHECK(rejects(&f, "font-synthesis", "weight weight"));
    EMB_CHECK(rejects(&f, "font-synthesis", "none weight"));
    EMB_CHECK(rejects(&f, "font-synthesis", "bold"));
    EMB_CHECK(rejects(&f, "font-synthesis", ""));
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ background and mask */

EMB_TEST(background_shorthand_expands_layers)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "background",
                       "url(a.png) 10px 20px / cover no-repeat fixed padding-box content-box, "
                       "linear-gradient(red, blue) center / 50% auto repeat-x, blue");
    EMB_REQUIRE(n == 9);
    uint32_t k = 0;
    const embi_value *img = embi_value_items(find(f.lh, n, "background-image"), &k);
    EMB_REQUIRE(k == 3);
    EMB_CHECK_EQ_INT(img[0].image->kind, EMBI_IMG_URL);
    EMB_CHECK_EQ_INT(img[1].image->kind, EMBI_IMG_GRADIENT);
    EMB_CHECK(is_kw(&img[2], EMBI_KW_none)); /* the third layer has only a color */
    const embi_value *px = embi_value_items(find(f.lh, n, "background-position-x"), &k);
    EMB_REQUIRE(k == 3);
    EMB_REQUIRE(px[0].kind == EMBI_V_POS_AXIS);
    EMB_CHECK(is_px(&px[0].axis->off, 10));
    EMB_CHECK(is_pct(&px[1].axis->off, 50));
    const embi_value *py = embi_value_items(find(f.lh, n, "background-position-y"), &k);
    EMB_CHECK(is_px(&py[0].axis->off, 20));
    const embi_value *size = embi_value_items(find(f.lh, n, "background-size"), &k);
    EMB_REQUIRE(k == 3);
    EMB_CHECK(is_kw(&size[0], EMBI_KW_cover));
    const embi_value *org = embi_value_items(find(f.lh, n, "background-origin"), &k);
    const embi_value *clip = embi_value_items(find(f.lh, n, "background-clip"), &k);
    EMB_CHECK(is_kw(&org[0], EMBI_KW_padding_box));
    EMB_CHECK(is_kw(&clip[0], EMBI_KW_content_box));
    EMB_CHECK(is_kw(&org[1], EMBI_KW_padding_box)); /* initial */
    EMB_CHECK(is_kw(&clip[1], EMBI_KW_border_box));
    const embi_value *att = embi_value_items(find(f.lh, n, "background-attachment"), &k);
    EMB_CHECK(is_kw(&att[0], EMBI_KW_fixed));
    EMB_CHECK(is_kw(&att[1], EMBI_KW_scroll));
    /* The color belongs to the last layer only. */
    const embi_value *col = find(f.lh, n, "background-color");
    EMB_REQUIRE(col != nullptr);
    EMB_CHECK_EQ_INT(col->kind, EMBI_V_COLOR);

    /* A single box sets both origin and clip. */
    n = parse(&f, "background", "content-box red");
    EMB_REQUIRE(n == 9);
    EMB_CHECK(is_kw(find(f.lh, n, "background-origin"), EMBI_KW_content_box));
    EMB_CHECK(is_kw(find(f.lh, n, "background-clip"), EMBI_KW_content_box));
    /* Omitted parts reset: `background: red` clears the image. */
    n = parse(&f, "background", "red");
    EMB_REQUIRE(n == 9);
    EMB_CHECK(is_kw(find(f.lh, n, "background-image"), EMBI_KW_none));
    EMB_CHECK(is_kw(find(f.lh, n, "background-attachment"), EMBI_KW_scroll));
    n = parse(&f, "background", "none");
    EMB_REQUIRE(n == 9);
    EMB_CHECK_EQ_INT(find(f.lh, n, "background-color")->kind, EMBI_V_COLOR);

    static const char *const bad[] = {
        "",
        "red blue",                          /* two colors */
        "red, blue",                         /* the color is only in the last layer */
        "url(a.png) / cover",                /* size needs a position */
        "10px / ",
        "10px / 20px 30px 40px",
        "10px 20px 30px 40px 50px",
        "url(a.png) url(b.png)",
        "repeat-x repeat-y",
        "no-repeat repeat-x",
        "fixed local",
        "border-box padding-box content-box",
        "url(a b)",
        "url(a.png),",
        ", url(a.png)",
        "bogus",
        "center / -10px",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "background", bad[i]));
    }
    EMB_CHECK(accepts(&f, "background", "url(a.png), red url(b.png)"));
    EMB_CHECK(accepts(&f, "background", "linear-gradient(red)")); /* one stop is a solid fill */
    embi_arena_free(&f.a);
}

EMB_TEST(background_position_shorthand_and_longhands)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "background-position", "right 10px bottom 20px, left top");
    EMB_REQUIRE(n == 2);
    uint32_t k = 0;
    const embi_value *x = embi_value_items(find(f.lh, n, "background-position-x"), &k);
    EMB_REQUIRE(k == 2);
    EMB_CHECK_EQ_INT(x[0].axis->anchor, 1);
    EMB_CHECK(is_px(&x[0].axis->off, 10));
    EMB_CHECK_EQ_INT(x[1].axis->anchor, 0);
    EMB_CHECK(is_pct(&x[1].axis->off, 0));
    EMB_REQUIRE(parse(&f, "background-position-x", "right") == 1);
    EMB_CHECK_EQ_INT(item0(&f)->kind, EMBI_V_POS_AXIS);
    EMB_CHECK(is_pct(&item0(&f)->axis->off, 100));
    EMB_REQUIRE(parse(&f, "background-position-y", "bottom 5px") == 1);
    EMB_CHECK(rejects(&f, "background-position-x", "top"));
    EMB_CHECK(rejects(&f, "background-position-y", "left"));
    EMB_CHECK(rejects(&f, "background-position", "left right"));
    EMB_CHECK(rejects(&f, "background-position", "top bottom"));
    EMB_CHECK(rejects(&f, "background-position", "10px 20px 30px"));
    EMB_CHECK(accepts(&f, "background-position", "left 10px top")); /* 3-value form */
    EMB_CHECK(rejects(&f, "background-position", "center 10px 10px"));
    EMB_CHECK(rejects(&f, "background-position", ","));
    embi_arena_free(&f.a);
}

EMB_TEST(mask_and_border_image_shorthands)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "mask", "url(m.svg) center / contain no-repeat border-box content-box exclude luminance");
    EMB_REQUIRE(n >= 8);
    EMB_CHECK(is_kw(find(f.lh, n, "mask-composite"), EMBI_KW_exclude));
    EMB_CHECK(is_kw(find(f.lh, n, "mask-mode"), EMBI_KW_luminance));
    EMB_CHECK(is_kw(find(f.lh, n, "mask-origin"), EMBI_KW_border_box));
    EMB_CHECK(is_kw(find(f.lh, n, "mask-clip"), EMBI_KW_content_box));
    EMB_CHECK(is_kw(find(f.lh, n, "mask-size"), EMBI_KW_contain));
    n = parse(&f, "mask", "linear-gradient(red, transparent), none");
    EMB_REQUIRE(n >= 8);
    uint32_t k = 0;
    embi_value_items(find(f.lh, n, "mask-image"), &k);
    EMB_CHECK_EQ_INT(k, 2);
    n = parse(&f, "mask", "none");
    EMB_CHECK(n >= 8);
    EMB_CHECK(rejects(&f, "mask", "red"));       /* masks have no color */
    EMB_CHECK(rejects(&f, "mask", "add subtract"));
    EMB_CHECK(rejects(&f, "mask", "alpha luminance"));
    EMB_CHECK(rejects(&f, "mask", "url(a b)"));
    EMB_CHECK(rejects(&f, "mask", "url(a),"));
    EMB_CHECK(rejects(&f, "mask", "no-clip border-box"));

    n = parse(&f, "border-image", "url(b.png) 30 20% / 10px 4 / 1 stretch round");
    EMB_REQUIRE(n == 5);
    EMB_CHECK_EQ_INT(find(f.lh, n, "border-image-source")->image->kind, EMBI_IMG_URL);
    const embi_value *slice = find(f.lh, n, "border-image-slice");
    EMB_REQUIRE(slice != nullptr);
    EMB_CHECK_EQ_INT(slice->kind, EMBI_V_LIST);
    EMB_CHECK_EQ_INT(slice->list.n, 4);
    EMB_CHECK(slice->list.items[0].num == 30);
    EMB_CHECK(is_pct(&slice->list.items[1], 20));
    EMB_CHECK_EQ_INT(find(f.lh, n, "border-image-width")->list.n, 4);
    EMB_CHECK_EQ_INT(find(f.lh, n, "border-image-outset")->list.n, 4);
    EMB_CHECK_EQ_INT(find(f.lh, n, "border-image-repeat")->list.n, 2);
    n = parse(&f, "border-image", "linear-gradient(red, blue) 1");
    EMB_REQUIRE(n == 5);
    EMB_CHECK(is_kw(find(f.lh, n, "border-image-repeat"), EMBI_KW_stretch) ||
              find(f.lh, n, "border-image-repeat")->kind == EMBI_V_LIST);
    EMB_REQUIRE(parse(&f, "border-image", "none") == 5);
    EMB_REQUIRE(parse(&f, "border-image", "url(a.png) fill 10 / 5px / 2px space") == 5);
    EMB_REQUIRE(parse(&f, "border-image", "url(a.png) 10 / / 2px") == 5);
    EMB_REQUIRE(parse(&f, "mask-border", "url(a.png) 10 alpha") >= 5);

    static const char *const bad[] = {
        "url(a.png) 1 2 3 4 5",
        "url(a.png) -1",
        "url(a.png) 10px",                /* slice takes numbers and percentages */
        "url(a.png) 10 / -1px",
        "url(a.png) 10 / 1 / 2 / 3",
        "url(a.png) 10 stretch stretch stretch",
        "url(a.png) url(b.png)",
        "10 20 30 40 50",
        "url(a.png) / 10px",
        "url(a.png) 10 /",
        "url(a.png) 10 round space stretch",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "border-image", bad[i]));
    }
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ transition, animation */

EMB_TEST(transition_shorthand_expands_layers)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "transition", "opacity 1s ease-in 0.5s, transform 200ms cubic-bezier(0,0,1,1) allow-discrete, all 3s");
    EMB_REQUIRE(n == 5);
    uint32_t k = 0;
    const embi_value *prop = embi_value_items(find(f.lh, n, "transition-property"), &k);
    EMB_REQUIRE(k == 3);
    EMB_CHECK_EQ_INT(prop[0].kind, EMBI_V_IDENT);
    EMB_CHECK_EQ_INT(prop[1].kind, EMBI_V_IDENT);
    EMB_CHECK(is_kw(&prop[2], EMBI_KW_all));
    const embi_value *dur = embi_value_items(find(f.lh, n, "transition-duration"), &k);
    EMB_CHECK_EQ_INT(dur[0].kind, EMBI_V_TIME);
    EMB_CHECK(dur[0].num == 1.0);
    EMB_CHECK(fabs(dur[1].num - 0.2) < 1e-12);
    const embi_value *delay = embi_value_items(find(f.lh, n, "transition-delay"), &k);
    EMB_CHECK(delay[0].num == 0.5);
    EMB_CHECK(delay[1].num == 0.0);
    const embi_value *ease = embi_value_items(find(f.lh, n, "transition-timing-function"), &k);
    EMB_CHECK(is_kw(&ease[0], EMBI_KW_ease_in));
    EMB_CHECK_EQ_INT(ease[1].kind, EMBI_V_EASING);
    EMB_CHECK(is_kw(&ease[2], EMBI_KW_ease));
    const embi_value *beh = embi_value_items(find(f.lh, n, "transition-behavior"), &k);
    EMB_CHECK(is_kw(&beh[1], EMBI_KW_allow_discrete));
    EMB_CHECK(is_kw(&beh[0], EMBI_KW_normal));

    /* The first time is the duration, the second the delay; `none` and `all` are properties. */
    n = parse(&f, "transition", "none");
    EMB_REQUIRE(n == 5);
    EMB_CHECK(is_kw(find(f.lh, n, "transition-property"), EMBI_KW_none));
    n = parse(&f, "transition", "1s 2s");
    EMB_REQUIRE(n == 5);
    EMB_CHECK(is_kw(find(f.lh, n, "transition-property"), EMBI_KW_all));

    static const char *const bad[] = {
        "1s 2s 3s",
        "opacity transform",
        "opacity,",
        ", opacity",
        "none, opacity",           /* none only as the single transition */
        "opacity none",
        "-1s",                     /* negative duration */
        "opacity 1",               /* unitless time */
        "opacity 1px",
        "steps(0)",
        "inherit 1s",
        "initial 1s",              /* CSS-wide keywords are not property names */
        "opacity 1s bogus(1)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "transition", bad[i]));
    }
    EMB_CHECK(accepts(&f, "transition", "initial"));
    embi_arena_free(&f.a);
}

EMB_TEST(animation_shorthand_expands_layers)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "animation", "spin 2s linear 1s infinite alternate both paused, fade 500ms 3");
    EMB_REQUIRE(n == 9); /* animation-composition is reset too */
    uint32_t k = 0;
    const embi_value *name = embi_value_items(find(f.lh, n, "animation-name"), &k);
    EMB_REQUIRE(k == 2);
    EMB_CHECK_EQ_INT(name[0].kind, EMBI_V_IDENT);
    const embi_value *iter = embi_value_items(find(f.lh, n, "animation-iteration-count"), &k);
    EMB_CHECK(is_kw(&iter[0], EMBI_KW_infinite));
    EMB_CHECK(iter[1].num == 3);
    const embi_value *dir = embi_value_items(find(f.lh, n, "animation-direction"), &k);
    EMB_CHECK(is_kw(&dir[0], EMBI_KW_alternate));
    const embi_value *fill = embi_value_items(find(f.lh, n, "animation-fill-mode"), &k);
    EMB_CHECK(is_kw(&fill[0], EMBI_KW_both));
    EMB_CHECK(is_kw(&fill[1], EMBI_KW_none));
    const embi_value *state = embi_value_items(find(f.lh, n, "animation-play-state"), &k);
    EMB_CHECK(is_kw(&state[0], EMBI_KW_paused));
    EMB_CHECK(is_kw(&state[1], EMBI_KW_running));
    const embi_value *delay = embi_value_items(find(f.lh, n, "animation-delay"), &k);
    EMB_CHECK(delay[0].num == 1.0);
    EMB_CHECK(delay[1].num == 0.0);

    /* A keyword that fits another longhand goes there first; the name is what remains. */
    n = parse(&f, "animation", "none");
    EMB_REQUIRE(n == 9);
    EMB_CHECK(is_kw(find(f.lh, n, "animation-name"), EMBI_KW_none) ||
              is_kw(find(f.lh, n, "animation-fill-mode"), EMBI_KW_none));
    n = parse(&f, "animation", "1s");
    EMB_REQUIRE(n == 9);
    EMB_CHECK(is_kw(find(f.lh, n, "animation-name"), EMBI_KW_none));

    static const char *const bad[] = {
        "spin 1s 2s 3s",
        "spin other",
        "spin infinite 3",
        "spin -1",                 /* negative iteration count */
        "spin 1s,",
        "spin alternate reverse",
        "spin 1px",
        "spin paused running",
        "spin steps(0)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(rejects(&f, "animation", bad[i]));
    }
    EMB_CHECK(accepts(&f, "animation", "spin 1")); /* a bare number is the iteration count */
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ container, grid shorthands */

EMB_TEST(container_shorthand)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "container", "sidebar card / inline-size");
    EMB_REQUIRE(n == 2);
    uint32_t k = 0;
    embi_value_items(find(f.lh, n, "container-name"), &k);
    EMB_CHECK_EQ_INT(k, 2);
    EMB_CHECK(is_kw(find(f.lh, n, "container-type"), EMBI_KW_inline_size));
    n = parse(&f, "container", "sidebar");
    EMB_REQUIRE(n == 2);
    EMB_CHECK(is_kw(find(f.lh, n, "container-type"), EMBI_KW_normal));
    n = parse(&f, "container", "none / size");
    EMB_CHECK(n == 2);
    EMB_CHECK(rejects(&f, "container", "/ inline-size"));
    EMB_CHECK(rejects(&f, "container", "a / "));
    EMB_CHECK(rejects(&f, "container", "a / bogus"));
    EMB_CHECK(rejects(&f, "container", "a / size size"));
    EMB_CHECK(rejects(&f, "container", "a / size / size"));
    EMB_CHECK(rejects(&f, "container", "a, b"));
    EMB_CHECK(rejects(&f, "container", "a 1"));
    EMB_CHECK(rejects(&f, "container", "a none"));
    EMB_CHECK(rejects(&f, "container-name", "initial a"));
    embi_arena_free(&f.a);
}

EMB_TEST(grid_shorthands)
{
    fx f;
    fx_init(&f);
    uint32_t n = parse(&f, "grid-area", "1 / 2 / span 3 / 4");
    EMB_REQUIRE(n == 4);
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-row-start")->gline->num, 1);
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-column-start")->gline->num, 2);
    EMB_CHECK(find(f.lh, n, "grid-row-end")->gline->span);
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-column-end")->gline->num, 4);
    /* A single name repeats on all four; fewer values copy the opposite side when it is a name. */
    n = parse(&f, "grid-area", "header");
    EMB_REQUIRE(n == 4);
    for (int i = 0; i < 4; i++) {
        EMB_CHECK_EQ_INT(f.lh[i].value.kind, EMBI_V_GRID_LINE);
        EMB_CHECK(f.lh[i].value.gline->name != 0);
    }
    n = parse(&f, "grid-area", "2");
    EMB_REQUIRE(n == 4);
    EMB_CHECK(is_kw(find(f.lh, n, "grid-column-start"), EMBI_KW_auto));
    n = parse(&f, "grid-row", "2 / span 2");
    EMB_REQUIRE(n == 2);
    EMB_CHECK(find(f.lh, n, "grid-row-end")->gline->span);
    n = parse(&f, "grid-column", "main");
    EMB_REQUIRE(n == 2);
    EMB_CHECK(find(f.lh, n, "grid-column-end")->kind == EMBI_V_GRID_LINE);
    n = parse(&f, "grid-column", "3");
    EMB_CHECK(is_kw(find(f.lh, n, "grid-column-end"), EMBI_KW_auto));

    EMB_CHECK(rejects(&f, "grid-area", "1 / 2 / 3 / 4 / 5"));
    EMB_CHECK(rejects(&f, "grid-area", "1 / / 3"));
    EMB_CHECK(rejects(&f, "grid-area", "/ 1"));
    EMB_CHECK(rejects(&f, "grid-area", "1 /"));
    EMB_CHECK(rejects(&f, "grid-area", "0 / 1"));
    EMB_CHECK(rejects(&f, "grid-row", "1 / 2 / 3"));
    EMB_CHECK(rejects(&f, "grid-row", "1 2"));
    EMB_CHECK(rejects(&f, "grid-column", "span"));
    EMB_CHECK(rejects(&f, "grid-row", ""));

    /* grid-template: the rows / columns form. */
    n = parse(&f, "grid-template", "auto 1fr / repeat(2, 100px)");
    EMB_REQUIRE(n == 3);
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-template-rows")->kind, EMBI_V_TRACKS);
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-template-columns")->kind, EMBI_V_TRACKS);
    EMB_CHECK(is_kw(find(f.lh, n, "grid-template-areas"), EMBI_KW_none));
    /* The areas form: rows may carry sizes and line names, columns follow the slash. */
    n = parse(&f, "grid-template", "[top] \"a a\" 40px [mid] \"b c\" auto / 1fr 2fr");
    EMB_REQUIRE(n == 3);
    const embi_value *areas = find(f.lh, n, "grid-template-areas");
    EMB_REQUIRE(areas->kind == EMBI_V_LIST);
    EMB_CHECK_EQ_INT(areas->list.n, 2);
    const embi_value *rows = find(f.lh, n, "grid-template-rows");
    EMB_REQUIRE(rows->kind == EMBI_V_TRACKS);
    EMB_CHECK_EQ_INT(rows->tracks->n, 4); /* [top] 40px [mid] auto */
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-template-columns")->tracks->n, 2);
    n = parse(&f, "grid-template", "\"a b\" \"c d\"");
    EMB_REQUIRE(n == 3);
    EMB_CHECK(is_kw(find(f.lh, n, "grid-template-columns"), EMBI_KW_none));
    n = parse(&f, "grid-template", "none");
    EMB_REQUIRE(n == 3);

    static const char *const bad_tpl[] = {
        "auto 1fr",                     /* the slash is required without strings */
        "/ 1fr",
        "1fr /",
        "\"a\" \"b c\"",                /* ragged areas */
        "\"a b\" \"c a\"",              /* non-rectangular */
        "\"a\" / auto-fill",
        "\"a\" / repeat(auto-fill, 100px)", /* the explicit list has no auto repeat */
        "\"a\" subgrid",
        "\"a\" / subgrid",
        "none / none / none",
        "1fr / 1fr / 1fr",
        "\"a\" 1fr 2fr",                /* one size per row */
        "[x]",                          /* names only */
    };
    for (size_t i = 0; i < sizeof bad_tpl / sizeof bad_tpl[0]; i++) {
        EMB_CHECK(rejects(&f, "grid-template", bad_tpl[i]));
    }

    /* grid: the template form, then auto-flow forms in both directions. */
    n = parse(&f, "grid", "auto-flow dense 40px / 1fr 1fr");
    EMB_REQUIRE(n >= 6);
    const embi_value *flow = find(f.lh, n, "grid-auto-flow");
    EMB_REQUIRE(flow != nullptr);
    EMB_CHECK_EQ_INT(flow->kind, EMBI_V_LIST);
    EMB_CHECK_EQ_INT(flow->list.n, 2);
    EMB_CHECK(is_kw(&flow->list.items[0], EMBI_KW_row));
    EMB_CHECK(is_kw(&flow->list.items[1], EMBI_KW_dense));
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-template-columns")->kind, EMBI_V_TRACKS);
    EMB_CHECK(find(f.lh, n, "grid-auto-rows") != nullptr);
    n = parse(&f, "grid", "100px 200px / auto-flow 1fr");
    EMB_REQUIRE(n >= 6);
    flow = find(f.lh, n, "grid-auto-flow");
    EMB_CHECK(is_kw(flow, EMBI_KW_column));
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-template-rows")->kind, EMBI_V_TRACKS);
    EMB_CHECK(find(f.lh, n, "grid-auto-columns") != nullptr);
    n = parse(&f, "grid", "auto-flow / 1fr 1fr");
    EMB_REQUIRE(n >= 6);
    n = parse(&f, "grid", "\"a a\" \"b c\" / auto 1fr");
    EMB_REQUIRE(n >= 6);
    EMB_CHECK(find(f.lh, n, "grid-template-areas")->kind == EMBI_V_LIST);
    /* The omitted auto sizes reset to their initial values. */
    n = parse(&f, "grid", "auto 1fr / auto 1fr");
    EMB_REQUIRE(n >= 6);
    EMB_CHECK(is_kw(find(f.lh, n, "grid-auto-flow"), EMBI_KW_row));
    EMB_CHECK_EQ_INT(find(f.lh, n, "grid-auto-rows")->kind, EMBI_V_TRACKS); /* initial: auto */

    static const char *const bad_grid[] = {
        "auto-flow auto-flow / 1fr",
        "auto-flow / auto-flow",
        "auto-flow dense dense / 1fr",
        "auto-flow 1fr",                /* no slash */
        "1fr / 1fr / 1fr",
        "auto-flow subgrid / 1fr",
        "dense / 1fr",
        "auto-flow 1fr / 1fr 1fr 1fr ]",
        "auto-flow 1fr / auto-flow 1fr",
        "bogus",
    };
    for (size_t i = 0; i < sizeof bad_grid / sizeof bad_grid[0]; i++) {
        EMB_CHECK(rejects(&f, "grid", bad_grid[i]));
    }
    embi_arena_free(&f.a);
}

/* ------------------------------------------------------------------ logical properties */

EMB_TEST(logical_properties_resolve_per_writing_mode)
{
    const uint16_t HTB = EMBI_KW_horizontal_tb, VRL = EMBI_KW_vertical_rl, VLR = EMBI_KW_vertical_lr;
    const uint16_t SRL = EMBI_KW_sideways_rl, SLR = EMBI_KW_sideways_lr;
    const uint16_t LTR = EMBI_KW_ltr, RTL = EMBI_KW_rtl;
    EMB_CHECK(embi_prop_is_logical(prop_of("margin-inline-start")));
    EMB_CHECK(!embi_prop_is_logical(prop_of("margin-left")));
    EMB_CHECK(!embi_prop_is_logical(prop_of("color")));

    /* horizontal-tb */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-start"), HTB, LTR), prop_of("margin-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-end"), HTB, LTR), prop_of("margin-right"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-start"), HTB, RTL), prop_of("margin-right"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-end"), HTB, RTL), prop_of("margin-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-block-start"), HTB, LTR), prop_of("margin-top"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-block-end"), HTB, RTL), prop_of("margin-bottom"));
    /* vertical-rl: block flows right to left, inline top to bottom (ltr). */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-block-start"), VRL, LTR), prop_of("margin-right"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-block-end"), VRL, LTR), prop_of("margin-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-start"), VRL, LTR), prop_of("margin-top"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-inline-start"), VRL, RTL), prop_of("margin-bottom"));
    /* vertical-lr: block flows left to right. */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-block-start"), VLR, LTR), prop_of("padding-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-block-end"), VLR, LTR), prop_of("padding-right"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-inline-end"), VLR, LTR), prop_of("padding-bottom"));
    /* sideways modes: sideways-rl is vertical-rl; sideways-lr runs the inline axis bottom to top. */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-block-start"), SRL, LTR), prop_of("padding-right"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-inline-start"), SRL, LTR), prop_of("padding-top"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-block-start"), SLR, LTR), prop_of("padding-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("padding-inline-start"), SLR, LTR), prop_of("padding-bottom"));

    /* Other families: inset, border parts, sizes and corner radii. */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("inset-inline-start"), HTB, LTR), prop_of("left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("inset-block-end"), HTB, LTR), prop_of("bottom"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-inline-end-width"), HTB, RTL), prop_of("border-left-width"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-block-start-style"), VRL, LTR), prop_of("border-right-style"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-block-end-color"), VLR, LTR), prop_of("border-right-color"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("inline-size"), HTB, LTR), prop_of("width"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("inline-size"), VRL, LTR), prop_of("height"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("block-size"), VRL, LTR), prop_of("width"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("min-inline-size"), HTB, LTR), prop_of("min-width"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("max-block-size"), VLR, LTR), prop_of("max-width"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-start-start-radius"), HTB, LTR), prop_of("border-top-left-radius"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-start-end-radius"), HTB, LTR), prop_of("border-top-right-radius"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-end-start-radius"), HTB, RTL), prop_of("border-bottom-right-radius"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-start-start-radius"), VRL, LTR), prop_of("border-top-right-radius"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("border-end-end-radius"), VRL, LTR), prop_of("border-bottom-left-radius"));

    /* A physical property maps to itself, and an unrelated one too. */
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("margin-left"), VRL, RTL), prop_of("margin-left"));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("color"), VRL, RTL), prop_of("color"));

    /* The reverse direction round-trips. */
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("margin-left"), HTB, LTR), prop_of("margin-inline-start"));
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("margin-left"), HTB, RTL), prop_of("margin-inline-end"));
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("margin-right"), VRL, LTR), prop_of("margin-block-start"));
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("width"), VRL, LTR), prop_of("block-size"));
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("color"), HTB, LTR), EMBI_PROP_NONE);
    EMB_CHECK_EQ_INT(embi_prop_to_logical(prop_of("margin-inline-start"), HTB, LTR), EMBI_PROP_NONE);

    /* Every logical property in the table maps to a physical one of the same family and back, in
     * every mode. */
    static const uint16_t modes[] = {EMBI_KW_horizontal_tb, EMBI_KW_vertical_rl, EMBI_KW_vertical_lr,
                                     EMBI_KW_sideways_rl, EMBI_KW_sideways_lr};
    uint32_t logical_count = 0;
    for (embi_prop_id id = 1; id < EMBI_PROP_COUNT; id++) {
        if (!embi_prop_is_logical(id)) {
            continue;
        }
        logical_count++;
        for (size_t m = 0; m < 5; m++) {
            for (int d = 0; d < 2; d++) {
                uint16_t dir = d ? RTL : LTR;
                embi_prop_id phys = embi_prop_to_physical(id, modes[m], dir);
                EMB_CHECK(phys != id && phys != EMBI_PROP_NONE);
                EMB_CHECK(!embi_prop_is_logical(phys));
                EMB_CHECK_EQ_INT(embi_prop_to_logical(phys, modes[m], dir), id);
            }
        }
    }
    EMB_CHECK_EQ_INT(logical_count, 42); /* margin, padding, inset, border parts, sizes, radii */
}

/* ------------------------------------------------------------------ font metrics */

EMB_TEST(font_relative_units_use_supplied_metrics)
{
    embi_len_ctx c = {.font_px = 20.0, .root_px = 10.0, .vw = 1000, .vh = 500, .cq_w = NAN, .cq_h = NAN};
    /* Without metrics the Values 4 fallbacks apply. */
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_EX, 2) - 20.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CH, 2) - 20.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CAP, 1) - 14.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_IC, 1) - 20.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_LH, 1) - 24.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_REX, 2) - 10.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_RLH, 1) - 12.0) < 1e-9);
    /* With metrics from layout the real font values are used. */
    c.font_metrics = (embi_font_metrics){.ex = 9.5, .cap = 13.25, .ch = 11.0, .ic = 21.0, .lh = 27.0};
    c.root_metrics = (embi_font_metrics){.ex = 4.0, .cap = 7.0, .ch = 5.0, .ic = 10.5, .lh = 13.0};
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_EX, 2) - 19.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CAP, 2) - 26.5) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CH, 1) - 11.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_IC, 1) - 21.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_LH, 2) - 54.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_REX, 1) - 4.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_RCAP, 1) - 7.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_RCH, 1) - 5.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_RIC, 1) - 10.5) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_RLH, 1) - 13.0) < 1e-9);
    /* em and rem never use the metrics. */
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_EM, 1) - 20.0) < 1e-9);
    /* A NaN or negative metric falls back. */
    c.font_metrics.ex = NAN;
    c.font_metrics.cap = -1;
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_EX, 1) - 10.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CAP, 1) - 14.0) < 1e-9);
}

/* ------------------------------------------------------------------ whole table */

/* Every longhand's initial value parses with its own grammar, and every shorthand has no initial
 * text but expands from `initial`-free input by its longhands' initials. */
EMB_TEST(every_initial_value_parses_with_its_grammar)
{
    fx f;
    fx_init(&f);
    for (embi_prop_id id = 1; id < EMBI_PROP_COUNT; id++) {
        const embi_prop_info *pi = embi_prop_info_of(id);
        if (embi_prop_is_shorthand(id)) {
            continue;
        }
        embi_arena_reset(&f.a);
        embi_value v;
        bool ok = embi_value_initial(&f.a, id, &v);
        if (!ok) {
            fprintf(stderr, "  %s: initial value \"%s\" does not parse\n", pi->name,
                    pi->initial ? pi->initial : "(null)");
        }
        EMB_CHECK(ok);
    }
    embi_arena_free(&f.a);
}

EMB_TEST_MAIN()
