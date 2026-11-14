/*
 * Media queries 4/5 (W2d): parsing of a media query list over component values and evaluation
 * against the embedder's environment. Range syntax, min-/max- prefixes, `not`/`and`/`or`, media
 * types, and the features listed in the W2 spec are supported. An unknown feature or a general
 * enclosed condition is false, per spec.
 */
#include "style_int.h"

#include <string.h>

/* ------------------------------------------------------------------ token items */

typedef enum mitem_kind {
    MI_CV,   /* a component value (non-whitespace) */
    MI_LT,   /* < or <= */
    MI_GT,   /* > or >= */
    MI_EQ,   /* = */
    MI_LE,
    MI_GE,
} mitem_kind;

typedef struct mitem {
    mitem_kind kind;
    const embi_css_cv *cv;
} mitem;

#define MITEM_MAX 32u

/* Flattens a component value list into items (whitespace dropped, comparison operators joined).
 * Returns the count, or 0 when it does not fit. */
static uint32_t items_of(embi_css_slice s, mitem *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        const embi_css_cv *c = &s.p[i];
        if (c->tok.type == EMBI_CSS_WS) {
            continue;
        }
        if (n >= MITEM_MAX) {
            return 0;
        }
        if (c->tok.type == EMBI_CSS_DELIM && (c->tok.cp == '<' || c->tok.cp == '>')) {
            bool eq = i + 1 < s.n && s.p[i + 1].tok.type == EMBI_CSS_DELIM &&
                      s.p[i + 1].tok.cp == '=';
            if (eq) {
                i++;
            }
            out[n].kind = c->tok.cp == '<' ? (eq ? MI_LE : MI_LT) : (eq ? MI_GE : MI_GT);
            out[n].cv = nullptr;
            n++;
            continue;
        }
        if (c->tok.type == EMBI_CSS_DELIM && c->tok.cp == '=') {
            out[n].kind = MI_EQ;
            out[n].cv = nullptr;
            n++;
            continue;
        }
        out[n].kind = MI_CV;
        out[n].cv = c;
        n++;
    }
    return n;
}

static bool is_ident(const mitem *it, const char *name)
{
    return it->kind == MI_CV && it->cv->tok.type == EMBI_CSS_IDENT &&
           embi_str_ieq(it->cv->tok.value, (embi_str){name, (uint32_t)strlen(name)});
}

/* ------------------------------------------------------------------ feature values */

typedef struct mval {
    bool num;     /* numeric */
    double n;     /* the number (px for lengths, dppx for resolutions, a/b for ratios) */
    bool bare;    /* an unitless <number> */
    bool len;     /* a length feature (environment side) */
    bool str;     /* keyword */
    embi_str s;
} mval;

/* Parses a value (viewport units in a length or calc() use the environment viewport): a number, a length or resolution dimension, an identifier, or a ratio. */
static bool parse_value(const embi_style_env *env, const mitem *it, uint32_t n, mval *out)
{
    memset(out, 0, sizeof *out);
    if (n == 1 && it[0].kind == MI_CV) {
        const embi_css_token *t = &it[0].cv->tok;
        if (t->type == EMBI_CSS_NUMBER) {
            out->num = true;
            out->bare = true;
            out->n = t->num;
            return true;
        }
        if (t->type == EMBI_CSS_IDENT) {
            out->str = true;
            out->s = t->value;
            return true;
        }
        if (t->type == EMBI_CSS_DIMENSION) {
            uint8_t unit = 0;
            uint8_t cls = 0;
            double scale = 1.0;
            if (!embi_unit_lookup(t->value, &unit, &cls, &scale)) {
                return false;
            }
            out->num = true;
            if (cls == EMBI_CD_RES) {
                out->n = t->num * scale;
            } else if (cls == EMBI_CD_LEN) {
                if (unit == EMBI_U_PX) {
                    out->n = t->num * scale;
                } else {
                    embi_len_ctx lc = {.font_px = 16.0, .root_px = 16.0, .vw = env->width, .vh = env->height,
                                       .cq_w = __builtin_nan(""), .cq_h = __builtin_nan("")};
                    out->n = embi_len_px_cb(&lc, unit, t->num);
                }
            } else {
                return false;
            }
            return true;
        }
        if (t->type == EMBI_CSS_FUNCTION) {
            /* calc(), min(), max() and the other math functions: evaluated with the default font size. */
            embi_arena ar = {0};
            embi_arena_init(&ar, 512);
            embi_value v;
            bool ok = false;
            if (!embi_value_parse_types(&ar, EMBI_T_LEN | EMBI_T_NUM | EMBI_T_RES, it[0].cv, &v)) {
                /* not a math function */
            } else if (v.kind == EMBI_V_LENGTH || v.kind == EMBI_V_NUMBER || v.kind == EMBI_V_RESOLUTION) {
                /* a constant expression, already reduced */
                out->num = true;
                out->n = v.num;
                out->bare = v.kind == EMBI_V_NUMBER;
                ok = true;
            } else if (v.kind == EMBI_V_CALC) {
                embi_len_ctx lc = {.font_px = 16.0, .root_px = 16.0, .vw = env->width, .vh = env->height,
                                   .cq_w = __builtin_nan(""), .cq_h = __builtin_nan("")};
                embi_calc_env ce = {.pct_base = 0, .len_px = embi_len_px_cb, .user = &lc};
                double x = embi_calc_eval(v.calc, &ce);
                if (x == x) {
                    out->num = true;
                    out->n = x;
                    out->bare = (v.calc->type & EMBI_CD_LEN) == 0 && (v.calc->type & EMBI_CD_NUM) != 0;
                    ok = true;
                }
            }
            embi_arena_free(&ar);
            return ok;
        }
        return false;
    }
    if (n == 3 && it[0].kind == MI_CV && it[1].kind == MI_CV && it[2].kind == MI_CV &&
        it[0].cv->tok.type == EMBI_CSS_NUMBER && it[1].cv->tok.type == EMBI_CSS_DELIM &&
        it[1].cv->tok.cp == '/' && it[2].cv->tok.type == EMBI_CSS_NUMBER &&
        it[2].cv->tok.num != 0) {
        out->num = true;
        out->n = it[0].cv->tok.num / it[2].cv->tok.num;
        return true;
    }
    return false;
}

/* The environment's value of a feature. Returns false for an unknown feature. */
static bool feature_value(const embi_style_env *env, embi_str name, mval *out)
{
    memset(out, 0, sizeof *out);
    struct {
        const char *name;
        bool num;
        double n;
        const char *s;
    } tab[] = {
        {"width", true, env->width, nullptr},
        {"height", true, env->height, nullptr},
        {"device-width", true, env->width, nullptr},
        {"device-height", true, env->height, nullptr},
        {"aspect-ratio", true, env->height > 0 ? env->width / env->height : 0, nullptr},
        {"device-aspect-ratio", true, env->height > 0 ? env->width / env->height : 0, nullptr},
        {"resolution", true, env->dppx, nullptr},
        {"color", true, 8, nullptr},
        {"color-index", true, 0, nullptr},
        {"monochrome", true, 0, nullptr},
        {"grid", true, 0, nullptr},
        {"orientation", false, 0,
         env->height >= env->width ? "portrait" : "landscape"},
        {"prefers-color-scheme", false, 0, env->dark ? "dark" : "light"},
        {"prefers-reduced-motion", false, 0, env->reduced_motion ? "reduce" : "no-preference"},
        {"prefers-contrast", false, 0,
         env->contrast == 1 ? "more" : env->contrast == 2 ? "less" : "no-preference"},
        {"hover", false, 0, env->hover ? "hover" : "none"},
        {"any-hover", false, 0, env->hover ? "hover" : "none"},
        {"pointer", false, 0, env->pointer == 1 ? "coarse" : env->pointer == 2 ? "fine" : "none"},
        {"any-pointer", false, 0,
         env->pointer == 1 ? "coarse" : env->pointer == 2 ? "fine" : "none"},
        {"scripting", false, 0, env->scripting ? "enabled" : "none"},
        {"display-mode", false, 0, "browser"},
        {"forced-colors", false, 0, env->forced_colors ? "active" : "none"},
        {"color-gamut", false, 0, env->gamut == 2 ? "rec2020" : env->gamut == 1 ? "p3" : "srgb"},
        {"dynamic-range", false, 0, env->hdr ? "high" : "standard"},
        {"update", false, 0, env->update == 2 ? "fast" : env->update == 1 ? "slow" : "none"},
        {"inverted-colors", false, 0, "none"},
        {"overflow-block", false, 0, "scroll"},
        {"overflow-inline", false, 0, "scroll"},
    };
    for (uint32_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        embi_str want = {tab[i].name, (uint32_t)strlen(tab[i].name)};
        if (embi_str_ieq(name, want)) {
            out->num = tab[i].num;
            out->len = i < 4;
            out->n = tab[i].n;
            if (tab[i].s != nullptr) {
                out->str = true;
                out->s = (embi_str){tab[i].s, (uint32_t)strlen(tab[i].s)};
            }
            return true;
        }
    }
    return false;
}

static bool compare_num(double a, mitem_kind op, double b)
{
    switch (op) {
    case MI_LT: return a < b;
    case MI_LE: return a <= b;
    case MI_GT: return a > b;
    case MI_GE: return a >= b;
    case MI_EQ: return a == b;
    default: return false;
    }
}

static mitem_kind flip(mitem_kind op)
{
    switch (op) {
    case MI_LT: return MI_GT;
    case MI_LE: return MI_GE;
    case MI_GT: return MI_LT;
    case MI_GE: return MI_LE;
    default: return op;
    }
}

/* Compares an environment value with a parsed one under op. */
static bool compare_values(const mval *env, mitem_kind op, const mval *want)
{
    if (env->num && want->num) {
        if (env->len && want->bare && want->n != 0) {
            return false;
        }
        return compare_num(env->n, op, want->n);
    }
    if (env->str && want->str && op == MI_EQ) {
        return embi_str_ieq(env->s, want->s);
    }
    return false;
}

/* ------------------------------------------------------------------ conditions */

/* Media Queries 4 section 3: a condition is true, false or unknown. An unknown feature, a general
 * enclosed condition or an unknown function is unknown; `not` of unknown stays unknown, and a
 * query whose condition is unknown does not match (even with `not`). */
typedef enum tri {
    T_FALSE = 0,
    T_TRUE = 1,
    T_UNKNOWN = 2,
    T_INVALID = 3, /* a structurally invalid query: becomes `not all`, immune to `not` */
} tri;

static tri tri_not(tri a)
{
    if (a == T_INVALID) {
        return T_INVALID;
    }
    if (a == T_UNKNOWN) {
        return T_UNKNOWN;
    }
    return a == T_TRUE ? T_FALSE : T_TRUE;
}

static tri tri_and(tri a, tri b)
{
    if (a == T_INVALID || b == T_INVALID) {
        return T_INVALID;
    }
    if (a == T_FALSE || b == T_FALSE) {
        return T_FALSE;
    }
    if (a == T_UNKNOWN || b == T_UNKNOWN) {
        return T_UNKNOWN;
    }
    return T_TRUE;
}

static tri tri_or(tri a, tri b)
{
    if (a == T_INVALID || b == T_INVALID) {
        return T_INVALID;
    }
    if (a == T_TRUE || b == T_TRUE) {
        return T_TRUE;
    }
    if (a == T_UNKNOWN || b == T_UNKNOWN) {
        return T_UNKNOWN;
    }
    return T_FALSE;
}

static bool is_cmp_item(const mitem *it)
{
    return it->kind == MI_LT || it->kind == MI_GT || it->kind == MI_EQ || it->kind == MI_LE ||
           it->kind == MI_GE;
}

static tri eval_expr(const mitem *it, uint32_t n, const embi_style_env *env, uint32_t depth);

/* A range comparison: `name op value`, `value op name`, or `value op name op value`. Ratios are
 * parsed as values (three items), so the item count is not what classifies the form. */
static tri eval_range(const mitem *it, uint32_t n, const embi_style_env *env)
{
    uint32_t ops[2];
    uint32_t nops = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (is_cmp_item(&it[i])) {
            if (nops == 2) {
                return T_FALSE;
            }
            ops[nops++] = i;
        }
    }
    if (nops == 1) {
        uint32_t p = ops[0];
        if (p == 1 && it[0].kind == MI_CV && it[0].cv->tok.type == EMBI_CSS_IDENT) {
            /* name op value */
            mval env_v;
            if (!feature_value(env, it[0].cv->tok.value, &env_v)) {
                return T_UNKNOWN;
            }
            mval want;
            if (!env_v.num || !parse_value(env, it + 2, n - 2, &want) || !want.num ||
                (env_v.len && want.bare && want.n != 0)) {
                return T_FALSE;
            }
            return compare_num(env_v.n, it[p].kind, want.n) ? T_TRUE : T_FALSE;
        }
        if (p >= 1 && n == p + 2 && it[p + 1].kind == MI_CV &&
            it[p + 1].cv->tok.type == EMBI_CSS_IDENT) {
            /* value op name */
            mval env_v;
            if (!feature_value(env, it[p + 1].cv->tok.value, &env_v)) {
                return T_UNKNOWN;
            }
            mval want;
            if (!env_v.num || !parse_value(env, it, p, &want) || !want.num ||
                (env_v.len && want.bare && want.n != 0)) {
                return T_FALSE;
            }
            return compare_num(env_v.n, flip(it[p].kind), want.n) ? T_TRUE : T_FALSE;
        }
        return T_FALSE;
    }
    if (nops == 2) {
        uint32_t p1 = ops[0];
        uint32_t p2 = ops[1];
        if (p1 < 1 || p2 != p1 + 2 || p1 + 1 >= n || it[p1 + 1].kind != MI_CV ||
            it[p1 + 1].cv->tok.type != EMBI_CSS_IDENT) {
            return T_FALSE;
        }
        mval env_v;
        if (!feature_value(env, it[p1 + 1].cv->tok.value, &env_v)) {
            return T_UNKNOWN;
        }
        mval lo;
        mval hi;
        if (!env_v.num || !parse_value(env, it, p1, &lo) || !lo.num ||
            !parse_value(env, it + p2 + 1, n - p2 - 1, &hi) || !hi.num ||
            (env_v.len && ((lo.bare && lo.n != 0) || (hi.bare && hi.n != 0)))) {
            return T_FALSE;
        }
        bool a = compare_num(lo.n, it[p1].kind, env_v.n); /* lo op name */
        bool b = compare_num(env_v.n, it[p2].kind, hi.n);
        return a && b ? T_TRUE : T_FALSE;
    }
    return T_FALSE;
}

/* One parenthesized feature or group, given its inside items. */
static tri eval_inside(const mitem *it, uint32_t n, const embi_style_env *env, uint32_t depth)
{
    if (n == 0 || depth > 16) {
        return T_FALSE;
    }
    bool has_colon = false;
    bool has_cmp = false;
    bool has_bool = false; /* and / or at this level */
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].kind == MI_CV && it[i].cv->tok.type == EMBI_CSS_COLON) {
            has_colon = true;
        }
        if (is_cmp_item(&it[i])) {
            has_cmp = true;
        }
        if (is_ident(&it[i], "and") || is_ident(&it[i], "or")) {
            has_bool = true;
        }
    }
    /* A group: another condition, `not`, or a nested parenthesis. */
    if (!has_cmp && !has_colon &&
        (is_ident(&it[0], "not") || has_bool ||
         (it[0].kind == MI_CV && it[0].cv->tok.type == EMBI_CSS_LPAREN))) {
        tri g = eval_expr(it, n, env, depth + 1);
        return g == T_INVALID ? T_UNKNOWN : g; /* inside parens: general-enclosed */
    }
    if (!has_cmp && !has_colon) {
        /* Boolean context: (feature). */
        if (n == 1 && it[0].kind == MI_CV && it[0].cv->tok.type == EMBI_CSS_IDENT) {
            mval v;
            if (!feature_value(env, it[0].cv->tok.value, &v)) {
                return T_UNKNOWN;
            }
            if (v.num) {
                return v.n != 0 ? T_TRUE : T_FALSE;
            }
            return embi_str_ieq(v.s, EMBI_STR("none")) ||
                           embi_str_ieq(v.s, EMBI_STR("no-preference"))
                       ? T_FALSE
                       : T_TRUE;
        }
        return T_UNKNOWN; /* a general enclosed condition */
    }
    if (has_colon) {
        /* name : value (min-/max- prefixes compare). */
        if (n < 3 || it[0].kind != MI_CV || it[0].cv->tok.type != EMBI_CSS_IDENT ||
            it[1].kind != MI_CV || it[1].cv->tok.type != EMBI_CSS_COLON) {
            return T_FALSE;
        }
        embi_str name = it[0].cv->tok.value;
        mitem_kind op = MI_EQ;
        if (name.len > 4 && embi_str_ieq((embi_str){name.ptr, 4}, EMBI_STR("min-"))) {
            op = MI_GE;
            name = (embi_str){name.ptr + 4, name.len - 4};
        } else if (name.len > 4 && embi_str_ieq((embi_str){name.ptr, 4}, EMBI_STR("max-"))) {
            op = MI_LE;
            name = (embi_str){name.ptr + 4, name.len - 4};
        }
        mval env_v;
        if (!feature_value(env, name, &env_v)) {
            return T_UNKNOWN;
        }
        mval want;
        if (!parse_value(env, it + 2, n - 2, &want)) {
            return T_FALSE;
        }
        if (op != MI_EQ && (!env_v.num || !want.num)) {
            return T_FALSE;
        }
        if (op == MI_EQ && env_v.num != want.num) {
            return T_FALSE;
        }
        return compare_values(&env_v, op, &want) ? T_TRUE : T_FALSE;
    }
    return eval_range(it, n, env);
}

/* One term: `not` applied to a term, a parenthesized condition, or an unknown general enclosed
 * condition. used receives the number of items consumed. */
static tri eval_term(const mitem *it, uint32_t n, uint32_t *used, const embi_style_env *env,
                     uint32_t depth)
{
    if (n == 0) {
        *used = 0;
        return T_INVALID;
    }
    if (is_ident(&it[0], "not")) {
        if (n == 1) {
            *used = 1;
            return T_INVALID; /* `not` without an operand is invalid */
        }
        tri v = eval_term(it + 1, n - 1, used, env, depth);
        *used += 1;
        return tri_not(v);
    }
    if (it[0].kind == MI_CV && it[0].cv->tok.type == EMBI_CSS_LPAREN) {
        const embi_css_cv *cv = it[0].cv;
        mitem inner[MITEM_MAX];
        uint32_t k = items_of(embi_css_slice_make(cv->kids, cv->nkids), inner);
        *used = 1;
        return eval_inside(inner, k, env, depth);
    }
    /* A function or any other token: a general enclosed condition, unknown. */
    *used = 1;
    return T_UNKNOWN;
}

static tri eval_expr(const mitem *it, uint32_t n, const embi_style_env *env, uint32_t depth)
{
    bool has_and = false;
    bool has_or = false;
    for (uint32_t i = 0; i < n; i++) {
        has_and = has_and || is_ident(&it[i], "and");
        has_or = has_or || is_ident(&it[i], "or");
    }
    if (has_and && has_or) {
        return T_INVALID; /* `and` and `or` cannot mix without parentheses: an invalid condition */
    }
    uint32_t used = 0;
    tri acc = eval_term(it, n, &used, env, depth);
    uint32_t i = used;
    if (i >= n) {
        return acc;
    }
    if (is_ident(&it[0], "not")) {
        return T_INVALID; /* `not` cannot combine with and/or without parentheses */
    }
    if (has_and) {
        while (i < n && is_ident(&it[i], "and")) {
            uint32_t u2 = 0;
            tri v = eval_term(it + i + 1, n - i - 1, &u2, env, depth);
            acc = tri_and(acc, v);
            i += 1 + u2;
        }
        return i < n ? T_INVALID : acc;
    }
    if (has_or) {
        while (i < n && is_ident(&it[i], "or")) {
            uint32_t u2 = 0;
            tri v = eval_term(it + i + 1, n - i - 1, &u2, env, depth);
            acc = tri_or(acc, v);
            i += 1 + u2;
        }
        return i < n ? T_INVALID : acc;
    }
    return T_INVALID; /* trailing items after a complete expression */
}

/* ------------------------------------------------------------------ query list */

static bool media_type_matches(embi_str t)
{
    return embi_str_ieq(t, EMBI_STR("all")) || embi_str_ieq(t, EMBI_STR("screen"));
}

static bool eval_query(const mitem *it, uint32_t n, const embi_style_env *env)
{
    if (n == 0) {
        return false; /* an empty member of a query list is `not all` */
    }
    bool negate = false;
    uint32_t i = 0;
    if (is_ident(&it[0], "not")) {
        negate = true;
        i = 1;
    } else if (is_ident(&it[0], "only")) {
        i = 1;
    }
    tri res;
    if (i < n && it[i].kind == MI_CV && it[i].cv->tok.type == EMBI_CSS_IDENT &&
        !(is_ident(&it[i], "not") || is_ident(&it[i], "and") || is_ident(&it[i], "or"))) {
        /* A media type, optionally followed by `and` conditions. */
        bool typ = media_type_matches(it[i].cv->tok.value);
        if (i + 1 >= n) {
            res = typ ? T_TRUE : T_FALSE;
        } else if (is_ident(&it[i + 1], "and")) {
            /* After a media type the condition is <media-condition-without-or>. */
            bool has_or = false;
            for (uint32_t k = i + 2; k < n; k++) {
                has_or = has_or || is_ident(&it[k], "or");
            }
            tri e = has_or ? T_INVALID : eval_expr(it + i + 2, n - i - 2, env, 0);
            res = e == T_INVALID ? T_INVALID : (typ ? e : T_FALSE);
        } else {
            res = T_INVALID; /* a media type followed by something other than `and` */
        }
    } else if (i == 1 && !negate) {
        res = T_INVALID; /* `only` requires a media type */
    } else if (negate) {
        /* `not <condition>`: eval_expr owns the `not` so that it can reject `not (a) and (b)`. */
        res = eval_expr(it, n, env, 0);
        negate = false;
    } else {
        res = eval_expr(it + i, n - i, env, 0);
    }
    if (res == T_INVALID) {
        return false; /* an invalid query is `not all` */
    }
    if (res == T_UNKNOWN) {
        return false; /* an unknown condition never matches, even when negated */
    }
    return negate ? res == T_FALSE : res == T_TRUE;
}

bool embi_style_media_match(embi_css_slice prelude, const embi_style_env *env)
{
    bool any_nonempty = false;
    for (uint32_t i = 0; i < prelude.n; i++) {
        if (prelude.p[i].tok.type != EMBI_CSS_WS) {
            any_nonempty = true;
            break;
        }
    }
    if (!any_nonempty) {
        return true;
    }
    /* Split the list on top-level commas. */
    uint32_t start = 0;
    for (uint32_t i = 0; i <= prelude.n; i++) {
        bool end = i == prelude.n || prelude.p[i].tok.type == EMBI_CSS_COMMA;
        if (!end) {
            continue;
        }
        mitem items[MITEM_MAX];
        uint32_t cnt = items_of(embi_css_slice_make(prelude.p + start, i - start), items);
        if (cnt == 0 && i - start > 0) {
            /* Too many items for a query: it cannot match. */
            start = i + 1;
            continue;
        }
        if (eval_query(items, cnt, env)) {
            return true;
        }
        start = i + 1;
    }
    return false;
}
