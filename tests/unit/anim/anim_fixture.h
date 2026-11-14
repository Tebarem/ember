#pragma once
/*
 * Shared helpers for the animation unit tests: parsing declaration values into computed values
 * (the same parser the cascade uses), and small numeric checks.
 */
#include "anim/anim.h"
#include "css/css.h"
#include "css/props.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static inline bool anim_near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static inline embi_prop_id anim_prop(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

static inline void anim_arena_open(embi_arena *a)
{
    memset(a, 0, sizeof *a);
    embi_arena_init(a, 16384);
}

/* Parses text as the value of one longhand. *ok is false when the value is invalid. */
static inline embi_value anim_parse(embi_arena *a, const char *prop, const char *text, bool *ok)
{
    embi_css_slice s = embi_css_parse_cvs(a, (embi_str){text, (uint32_t)strlen(text)});
    embi_longhand lh[EMBI_LONGHAND_MAX];
    uint32_t n = embi_css_parse_decl(a, anim_prop(prop), s, lh, EMBI_LONGHAND_MAX);
    embi_value v;
    memset(&v, 0, sizeof v);
    *ok = n > 0;
    if (n > 0) {
        v = lh[0].value;
    }
    return v;
}

/* The first easing of a timing-function text, or nullptr. */
static inline const embi_easing *anim_easing(embi_arena *a, const char *text)
{
    bool ok = false;
    embi_value v = anim_parse(a, "transition-timing-function", text, &ok);
    if (!ok) {
        return nullptr;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(&v, &n);
    if (n == 0 || items[0].kind != EMBI_V_EASING) {
        return nullptr;
    }
    return items[0].easing;
}

/* Mixes two parsed values of prop at p. *ok is false when either text does not parse. */
static inline embi_value anim_mix(embi_arena *a, const char *prop, const char *from, const char *to,
                                  double p, bool *ok)
{
    bool ok1 = false, ok2 = false;
    embi_value f = anim_parse(a, prop, from, &ok1);
    embi_value t = anim_parse(a, prop, to, &ok2);
    embi_value out;
    memset(&out, 0, sizeof out);
    *ok = ok1 && ok2;
    if (*ok) {
        embi_anim_mix(a, anim_prop(prop), &f, &t, p, &out);
    }
    return out;
}

/* A length or calc() value in px, with 16px for em and 200px for 100%. */
static inline double anim_px(const embi_value *v)
{
    embi_len_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.font_px = 16.0;
    ctx.root_px = 16.0;
    embi_calc_env env = {200.0, embi_len_px_cb, &ctx};
    if (v->kind == EMBI_V_CALC) {
        return embi_calc_eval(v->calc, &env);
    }
    if (v->kind == EMBI_V_PERCENT) {
        return v->num / 100.0 * 200.0;
    }
    if (v->kind == EMBI_V_LENGTH) {
        return embi_len_px_cb(&ctx, v->unit, v->num);
    }
    return NAN;
}
