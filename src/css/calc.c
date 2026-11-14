/*
 * Math functions (CSS Values 4): calc(), min(), max(), clamp(), round(), mod(), rem(), trig and
 * exponential functions, abs(), sign(), and the constants e, pi, infinity and NaN.
 *
 * The parser works on the component value tree. It type checks each node against the dimension
 * classes in props.h (sums need equal dimensions, except length and percentage which mix to a
 * length-percentage; products need at most one non-number; division needs a number divisor).
 * Leaves hold canonical units (see value_num.c), so constant trees reduce to one value.
 */
#include "css/props.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ nodes */

static double nan_value(void)
{
    return __builtin_nan("");
}

static double inf_value(void)
{
    return __builtin_inf();
}

static embi_calc *new_node(embi_arena *a, uint8_t op, uint8_t type, uint8_t unit, double num,
                           uint32_t n, const embi_calc *const *kids_in)
{
    embi_calc *c = embi_arena_alloc(a, sizeof(embi_calc), alignof(embi_calc));
    const embi_calc **kids = nullptr;
    if (n > 0) {
        kids = embi_arena_alloc(a, sizeof(const embi_calc *) * n, alignof(const embi_calc *));
        for (uint32_t i = 0; i < n; i++) {
            kids[i] = kids_in[i];
        }
    }
    c->op = op;
    c->type = type;
    c->unit = unit;
    c->mode = 0;
    c->num = num;
    c->n = n;
    c->kids = kids;
    return c;
}

static const embi_calc *leaf(embi_arena *a, uint8_t type, uint8_t unit, double num)
{
    return new_node(a, EMBI_CALC_LEAF, type, unit, num, 0, nullptr);
}

/* Union of two dimensions for sums and min/max/clamp/mod: equal ones, or length with percentage. */
static bool union_type(uint8_t x, uint8_t y, uint8_t *out)
{
    if (x == 0 || y == 0) {
        return false;
    }
    if ((x & ~EMBI_CD_LENPCT) == 0 && (y & ~EMBI_CD_LENPCT) == 0) {
        *out = (uint8_t)(x | y);
        return true;
    }
    /* An angle-percentage mixes the same way (CSS Values 4 section 10.2: the angle stops of a conic gradient). */
    if (((x | y) & ~(EMBI_CD_ANGLE | EMBI_CD_PCT)) == 0) {
        *out = (uint8_t)(x | y);
        return true;
    }
    if (x == y) {
        *out = x;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ parser */

typedef struct cparse {
    embi_arena *a;
    const embi_css_cv *p;
    uint32_t n;
    uint32_t i;
    bool size_ok; /* the `size` keyword is a leaf (calc-size() only) */
} cparse;

static uint32_t skip_ws(cparse *cp)
{
    uint32_t skipped = 0;
    while (cp->i < cp->n && cp->p[cp->i].tok.type == EMBI_CSS_WS) {
        cp->i++;
        skipped++;
    }
    return skipped;
}

static bool at_end(cparse *cp)
{
    skip_ws(cp);
    return cp->i >= cp->n;
}

static bool is_delim(const cparse *cp, uint32_t c)
{
    return cp->i < cp->n && cp->p[cp->i].tok.type == EMBI_CSS_DELIM && cp->p[cp->i].tok.cp == c;
}

static bool parse_fn(cparse *cp, const embi_calc **out, uint8_t *type);

static bool parse_sum(cparse *cp, const embi_calc **out, uint8_t *type);

static bool name_eq(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

static bool parse_primary(cparse *cp, const embi_calc **out, uint8_t *type)
{
    skip_ws(cp);
    if (cp->i >= cp->n) {
        return false;
    }
    const embi_css_cv *cv = &cp->p[cp->i];
    const embi_css_token *t = &cv->tok;
    switch (t->type) {
    case EMBI_CSS_NUMBER:
        cp->i++;
        *out = leaf(cp->a, EMBI_CD_NUM, EMBI_U_NONE, t->num);
        *type = EMBI_CD_NUM;
        return true;
    case EMBI_CSS_PERCENTAGE:
        cp->i++;
        *out = leaf(cp->a, EMBI_CD_PCT, EMBI_U_NONE, t->num);
        *type = EMBI_CD_PCT;
        return true;
    case EMBI_CSS_DIMENSION: {
        uint8_t unit = 0;
        uint8_t cls = 0;
        double scale = 1.0;
        if (!embi_unit_lookup(t->value, &unit, &cls, &scale)) {
            return false;
        }
        cp->i++;
        *out = leaf(cp->a, cls, unit, t->num * scale);
        *type = cls;
        return true;
    }
    case EMBI_CSS_IDENT: {
        double v = 0;
        if (cp->size_ok && name_eq(t->value, "size")) {
            cp->i++;
            *out = new_node(cp->a, EMBI_CALC_SIZE, EMBI_CD_LENPCT, EMBI_U_NONE, 0, 0, nullptr);
            *type = EMBI_CD_LENPCT;
            return true;
        }
        if (name_eq(t->value, "e")) {
            v = 2.718281828459045235360287;
        } else if (name_eq(t->value, "pi")) {
            v = SDL_PI_D;
        } else if (name_eq(t->value, "infinity")) {
            v = inf_value();
        } else if (name_eq(t->value, "-infinity")) {
            v = -inf_value();
        } else if (name_eq(t->value, "nan")) {
            v = nan_value();
        } else {
            return false;
        }
        cp->i++;
        *out = leaf(cp->a, EMBI_CD_NUM, EMBI_U_NONE, v);
        *type = EMBI_CD_NUM;
        return true;
    }
    case EMBI_CSS_LPAREN: {
        cparse sub = {cp->a, cv->kids, cv->nkids, 0, cp->size_ok};
        cp->i++;
        if (!parse_sum(&sub, out, type) || !at_end(&sub)) {
            return false;
        }
        return true;
    }
    case EMBI_CSS_FUNCTION:
        cp->i++;
        return parse_fn(cp, out, type);
    default:
        return false;
    }
}

/* Parses a product: primaries joined by * and /. */
static bool parse_product(cparse *cp, const embi_calc **out, uint8_t *type)
{
    const embi_calc *lhs = nullptr;
    uint8_t lt = 0;
    if (!parse_primary(cp, &lhs, &lt)) {
        return false;
    }
    for (;;) {
        uint32_t save = cp->i;
        skip_ws(cp);
        bool mul = is_delim(cp, '*');
        bool div = is_delim(cp, '/');
        if (!mul && !div) {
            cp->i = save;
            break;
        }
        cp->i++;
        skip_ws(cp);
        const embi_calc *rhs = nullptr;
        uint8_t rt = 0;
        if (!parse_primary(cp, &rhs, &rt)) {
            return false;
        }
        const embi_calc *kids[2] = {lhs, rhs};
        if (mul) {
            if (lt == EMBI_CD_NUM) {
                lt = rt;
            } else if (rt != EMBI_CD_NUM) {
                return false;
            }
            lhs = new_node(cp->a, EMBI_CALC_MUL, lt, 0, 0, 2, kids);
        } else {
            if (rt != EMBI_CD_NUM || lt == 0) {
                return false;
            }
            lhs = new_node(cp->a, EMBI_CALC_DIV, lt, 0, 0, 2, kids);
        }
    }
    *out = lhs;
    *type = lt;
    return true;
}

/* Parses a sum: products joined by + and -, which need whitespace on both sides. */
static bool parse_sum(cparse *cp, const embi_calc **out, uint8_t *type)
{
    const embi_calc *lhs = nullptr;
    uint8_t lt = 0;
    if (!parse_product(cp, &lhs, &lt)) {
        return false;
    }
    for (;;) {
        uint32_t save = cp->i;
        uint32_t ws = skip_ws(cp);
        bool add = is_delim(cp, '+');
        bool sub = is_delim(cp, '-');
        if (ws == 0 || (!add && !sub)) {
            cp->i = save;
            break;
        }
        cp->i++;
        if (skip_ws(cp) == 0) {
            return false;
        }
        const embi_calc *rhs = nullptr;
        uint8_t rt = 0;
        if (!parse_product(cp, &rhs, &rt)) {
            return false;
        }
        uint8_t merged = 0;
        if (!union_type(lt, rt, &merged)) {
            return false;
        }
        const embi_calc *kids[2] = {lhs, rhs};
        lhs = new_node(cp->a, add ? EMBI_CALC_ADD : EMBI_CALC_SUB, merged, 0, 0, 2, kids);
        lt = merged;
    }
    *out = lhs;
    *type = lt;
    return true;
}

/* Parses the whole slice as one sum. */
static bool parse_whole(embi_arena *a, const embi_css_cv *p, uint32_t n, const embi_calc **out,
                        uint8_t *type, bool size_ok)
{
    cparse cp = {a, p, n, 0, size_ok};
    if (!parse_sum(&cp, out, type)) {
        return false;
    }
    return at_end(&cp);
}

/* Splits the function arguments at top-level commas and parses each as a sum. The first `skip`
 * segments are left for the caller (round()'s rounding strategy). */
static bool parse_args(embi_arena *a, const embi_css_cv *fn, uint32_t skip,
                       const embi_calc **args, uint8_t *types, uint32_t cap, uint32_t *count,
                       bool size_ok)
{
    uint32_t start = 0;
    uint32_t n = 0;
    uint32_t seg = 0;
    for (uint32_t i = 0; i <= fn->nkids; i++) {
        if (i == fn->nkids || fn->kids[i].tok.type == EMBI_CSS_COMMA) {
            if (seg >= skip) {
                if (n >= cap) {
                    return false;
                }
                /* fn->kids is null for a function with no arguments (no pointer arithmetic on it, M9a fuzz finding) */
                const embi_css_cv *from = fn->kids != nullptr ? fn->kids + start : nullptr;
                if (!parse_whole(a, from, i - start, &args[n], &types[n], size_ok)) {
                    return false;
                }
                n++;
            }
            seg++;
            start = i + 1;
        }
    }
    *count = n;
    return true;
}

/* For round(): returns 1 and sets *mode when the first argument is a rounding strategy keyword
 * (and there is at least one more argument), otherwise 0. */
static uint32_t round_strategy(const embi_css_cv *fn, embi_round_mode *mode)
{
    uint32_t segs = 1;
    for (uint32_t i = 0; i < fn->nkids; i++) {
        if (fn->kids[i].tok.type == EMBI_CSS_COMMA) {
            segs++;
        }
    }
    if (segs < 2) {
        return 0;
    }
    uint32_t j = 0;
    while (j < fn->nkids && fn->kids[j].tok.type == EMBI_CSS_WS) {
        j++;
    }
    if (j >= fn->nkids || fn->kids[j].tok.type != EMBI_CSS_IDENT) {
        return 0;
    }
    const embi_str s = fn->kids[j].tok.value;
    if (name_eq(s, "nearest")) {
        *mode = EMBI_ROUND_NEAREST;
    } else if (name_eq(s, "up")) {
        *mode = EMBI_ROUND_UP;
    } else if (name_eq(s, "down")) {
        *mode = EMBI_ROUND_DOWN;
    } else if (name_eq(s, "to-zero")) {
        *mode = EMBI_ROUND_TO_ZERO;
    } else {
        return 0;
    }
    return 1;
}

/* Combines a list of same-dimension arguments (min, max, clamp, hypot, mod, rem). */
static bool common_type(const uint8_t *types, uint32_t n, uint8_t *out)
{
    uint8_t t = types[0];
    for (uint32_t i = 1; i < n; i++) {
        if (!union_type(t, types[i], &t)) {
            return false;
        }
    }
    *out = t;
    return true;
}

static bool parse_fn(cparse *cp, const embi_calc **out, uint8_t *type)
{
    /* cp->i already points past the FUNCTION token; the arguments are its kids. */
    const embi_css_cv *fn = &cp->p[cp->i - 1];
    embi_arena *a = cp->a;
    const embi_str name = fn->tok.value;

    if (name_eq(name, "calc")) {
        cparse sub = {a, fn->kids, fn->nkids, 0, cp->size_ok};
        if (!parse_sum(&sub, out, type) || !at_end(&sub)) {
            return false;
        }
        return true;
    }

    const embi_calc *args[64] = {nullptr};
    uint8_t types[64] = {0};
    uint32_t n = 0;
    embi_round_mode mode = EMBI_ROUND_NEAREST;
    uint32_t skip = name_eq(name, "round") ? round_strategy(fn, &mode) : 0;
    if (!parse_args(a, fn, skip, args, types, 64, &n, cp->size_ok) || n == 0) {
        return false;
    }

    if (name_eq(name, "min") || name_eq(name, "max")) {
        uint8_t t = 0;
        if (!common_type(types, n, &t)) {
            return false;
        }
        *out = new_node(a, name_eq(name, "min") ? EMBI_CALC_MIN : EMBI_CALC_MAX, t, 0, 0, n, args);
        *type = t;
        return true;
    }
    if (name_eq(name, "clamp")) {
        uint8_t t = 0;
        if (n != 3 || !common_type(types, 3, &t)) {
            return false;
        }
        *out = new_node(a, EMBI_CALC_CLAMP, t, 0, 0, 3, args);
        *type = t;
        return true;
    }
    if (name_eq(name, "round")) {
        /* round( [<strategy>,]? A [, B]? ): the strategy was consumed by round_strategy. */
        if (n < 1 || n > 2) {
            return false;
        }
        const embi_calc *ra = args[0];
        uint8_t ta = types[0];
        const embi_calc *rb = nullptr;
        if (n == 2) {
            rb = args[1];
            uint8_t merged = 0;
            if (!union_type(ta, types[1], &merged)) {
                return false;
            }
            ta = merged;
        } else {
            /* B defaults to 1 in the same dimension as A. */
            rb = leaf(a, ta, ta == EMBI_CD_LEN ? EMBI_U_PX : EMBI_U_NONE, 1.0);
        }
        const embi_calc *kids[2] = {ra, rb};
        const embi_calc *node = new_node(a, EMBI_CALC_ROUND, ta, 0, 0, 2, kids);
        ((embi_calc *)node)->mode = (uint8_t)mode;
        *out = node;
        *type = ta;
        return true;
    }
    if (name_eq(name, "mod") || name_eq(name, "rem")) {
        uint8_t t = 0;
        if (n != 2 || !common_type(types, 2, &t)) {
            return false;
        }
        *out = new_node(a, name_eq(name, "mod") ? EMBI_CALC_MOD : EMBI_CALC_REM, t, 0, 0, 2, args);
        *type = t;
        return true;
    }
    if (name_eq(name, "hypot")) {
        uint8_t t = 0;
        if (!common_type(types, n, &t)) {
            return false;
        }
        *out = new_node(a, EMBI_CALC_HYPOT, t, 0, 0, n, args);
        *type = t;
        return true;
    }
    if (name_eq(name, "atan2")) {
        uint8_t t = 0;
        if (n != 2 || !common_type(types, 2, &t)) {
            return false;
        }
        *out = new_node(a, EMBI_CALC_ATAN2, EMBI_CD_ANGLE, 0, 0, 2, args);
        *type = EMBI_CD_ANGLE;
        return true;
    }
    if (name_eq(name, "pow") || name_eq(name, "log")) {
        bool is_pow = name_eq(name, "pow");
        if (is_pow ? n != 2 : (n < 1 || n > 2)) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (types[i] != EMBI_CD_NUM) {
                return false;
            }
        }
        if (is_pow) {
            *out = new_node(a, EMBI_CALC_POW, EMBI_CD_NUM, 0, 0, 2, args);
        } else {
            *out = new_node(a, EMBI_CALC_LOG, EMBI_CD_NUM, 0, 0, n, args);
        }
        *type = EMBI_CD_NUM;
        return true;
    }
    if (name_eq(name, "sqrt") || name_eq(name, "exp")) {
        if (n != 1 || types[0] != EMBI_CD_NUM) {
            return false;
        }
        *out = new_node(a, name_eq(name, "sqrt") ? EMBI_CALC_SQRT : EMBI_CALC_EXP, EMBI_CD_NUM, 0,
                        0, 1, args);
        *type = EMBI_CD_NUM;
        return true;
    }
    if (name_eq(name, "abs")) {
        if (n != 1) {
            return false;
        }
        *out = new_node(a, EMBI_CALC_ABS, types[0], 0, 0, 1, args);
        *type = types[0];
        return true;
    }
    if (name_eq(name, "sign")) {
        if (n != 1) {
            return false;
        }
        *out = new_node(a, EMBI_CALC_SIGN, EMBI_CD_NUM, 0, 0, 1, args);
        *type = EMBI_CD_NUM;
        return true;
    }
    if (name_eq(name, "sin") || name_eq(name, "cos") || name_eq(name, "tan")) {
        if (n != 1 || (types[0] != EMBI_CD_NUM && types[0] != EMBI_CD_ANGLE)) {
            return false;
        }
        uint8_t op = name_eq(name, "sin") ? EMBI_CALC_SIN
                     : name_eq(name, "cos") ? EMBI_CALC_COS
                                            : EMBI_CALC_TAN;
        *out = new_node(a, op, EMBI_CD_NUM, 0, 0, 1, args);
        *type = EMBI_CD_NUM;
        return true;
    }
    if (name_eq(name, "asin") || name_eq(name, "acos") || name_eq(name, "atan")) {
        if (n != 1 || types[0] != EMBI_CD_NUM) {
            return false;
        }
        uint8_t op = name_eq(name, "asin") ? EMBI_CALC_ASIN
                     : name_eq(name, "acos") ? EMBI_CALC_ACOS
                                             : EMBI_CALC_ATAN;
        *out = new_node(a, op, EMBI_CD_ANGLE, 0, 0, 1, args);
        *type = EMBI_CD_ANGLE;
        return true;
    }
    return false;
}

bool embi_calc_parse_sized(embi_arena *a, const embi_css_cv *p, uint32_t n, const embi_calc **out,
                           uint8_t *type_out)
{
    if (!a || !out || !type_out || (n > 0 && !p)) {
        return false;
    }
    uint8_t t = 0;
    if (!parse_whole(a, p, n, out, &t, true)) {
        return false;
    }
    *type_out = t;
    return true;
}

bool embi_calc_parse_cv(embi_arena *a, const embi_css_cv *cv, const embi_calc **out,
                        uint8_t *type_out)
{
    if (!cv || cv->tok.type != EMBI_CSS_FUNCTION || !a || !out || !type_out) {
        return false;
    }
    /* Reuse the function dispatcher on a one-element slice that holds the function. */
    cparse cp = {a, cv, 1, 1, false};
    const embi_calc *node = nullptr;
    uint8_t t = 0;
    if (!parse_fn(&cp, &node, &t)) {
        return false;
    }
    *out = node;
    *type_out = t;
    return true;
}

/* ------------------------------------------------------------------ evaluation */

/* The evaluation context: the public environment, plus the `size` keyword of calc-size(). */
typedef struct ectx {
    double pct_base;
    double (*len_px)(void *user, uint8_t unit, double value);
    void *user;
    double size;
    bool size_set;
} ectx;

static double eval(const embi_calc *c, const ectx *env);

static double eval_kid(const embi_calc *c, uint32_t i, const ectx *env)
{
    return eval(c->kids[i], env);
}

static double round_to(double v, double step, uint8_t mode)
{
    double q = v / step;
    double r = 0;
    switch (mode) {
    case EMBI_ROUND_UP:
        r = __builtin_ceil(q);
        break;
    case EMBI_ROUND_DOWN:
        r = __builtin_floor(q);
        break;
    case EMBI_ROUND_TO_ZERO:
        r = __builtin_trunc(q);
        break;
    default:
        r = __builtin_floor(q + 0.5);
        break;
    }
    return r * step;
}

static double eval(const embi_calc *c, const ectx *env)
{
    switch (c->op) {
    case EMBI_CALC_LEAF:
        if (c->type == EMBI_CD_PCT) {
            return c->num / 100.0 * (env ? env->pct_base : 0.0);
        }
        if (c->type == EMBI_CD_LEN && c->unit != EMBI_U_PX) {
            if (env && env->len_px) {
                return env->len_px(env->user, c->unit, c->num);
            }
            return nan_value();
        }
        return c->num;
    case EMBI_CALC_SIZE:
        return env && env->size_set ? env->size : nan_value();
    case EMBI_CALC_NEG:
        return -eval_kid(c, 0, env);
    case EMBI_CALC_ADD:
        return eval_kid(c, 0, env) + eval_kid(c, 1, env);
    case EMBI_CALC_SUB:
        return eval_kid(c, 0, env) - eval_kid(c, 1, env);
    case EMBI_CALC_MUL:
        return eval_kid(c, 0, env) * eval_kid(c, 1, env);
    case EMBI_CALC_DIV:
        return eval_kid(c, 0, env) / eval_kid(c, 1, env);
    case EMBI_CALC_MIN:
    case EMBI_CALC_MAX: {
        double best = eval_kid(c, 0, env);
        for (uint32_t i = 1; i < c->n; i++) {
            double v = eval_kid(c, i, env);
            if (best != best || v != v) {
                best = nan_value();
            } else if (c->op == EMBI_CALC_MIN ? v < best : v > best) {
                best = v;
            }
        }
        return best;
    }
    case EMBI_CALC_CLAMP: {
        double lo = eval_kid(c, 0, env);
        double v = eval_kid(c, 1, env);
        double hi = eval_kid(c, 2, env);
        if (lo != lo || v != v || hi != hi) {
            return nan_value();
        }
        /* max(lo, min(v, hi)) */
        double m = v < hi ? v : hi;
        return lo > m ? lo : m;
    }
    case EMBI_CALC_ROUND: {
        double v = eval_kid(c, 0, env);
        double step = eval_kid(c, 1, env);
        if (step == 0 || v != v || step != step) {
            return nan_value();
        }
        return round_to(v, step, c->mode);
    }
    case EMBI_CALC_MOD: {
        double a = eval_kid(c, 0, env);
        double b = eval_kid(c, 1, env);
        if (b == 0 || a != a || b != b) {
            return nan_value();
        }
        return a - b * __builtin_floor(a / b);
    }
    case EMBI_CALC_REM: {
        double a = eval_kid(c, 0, env);
        double b = eval_kid(c, 1, env);
        if (b == 0 || a != a || b != b) {
            return nan_value();
        }
        return a - b * __builtin_trunc(a / b);
    }
    case EMBI_CALC_SIN:
    case EMBI_CALC_COS:
    case EMBI_CALC_TAN: {
        double x = eval_kid(c, 0, env);
        if (c->kids[0]->type == EMBI_CD_ANGLE) {
            x = x * SDL_PI_D / 180.0;
        }
        if (c->op == EMBI_CALC_SIN) {
            return SDL_sin(x);
        }
        if (c->op == EMBI_CALC_COS) {
            return SDL_cos(x);
        }
        return SDL_tan(x);
    }
    case EMBI_CALC_ASIN:
        return SDL_asin(eval_kid(c, 0, env)) * 180.0 / SDL_PI_D;
    case EMBI_CALC_ACOS:
        return SDL_acos(eval_kid(c, 0, env)) * 180.0 / SDL_PI_D;
    case EMBI_CALC_ATAN:
        return SDL_atan(eval_kid(c, 0, env)) * 180.0 / SDL_PI_D;
    case EMBI_CALC_ATAN2:
        return SDL_atan2(eval_kid(c, 0, env), eval_kid(c, 1, env)) * 180.0 / SDL_PI_D;
    case EMBI_CALC_POW:
        return SDL_pow(eval_kid(c, 0, env), eval_kid(c, 1, env));
    case EMBI_CALC_SQRT:
        return SDL_sqrt(eval_kid(c, 0, env));
    case EMBI_CALC_HYPOT: {
        double sum = 0;
        for (uint32_t i = 0; i < c->n; i++) {
            double v = eval_kid(c, i, env);
            sum += v * v;
        }
        return SDL_sqrt(sum);
    }
    case EMBI_CALC_LOG: {
        double v = SDL_log(eval_kid(c, 0, env));
        if (c->n == 2) {
            v = v / SDL_log(eval_kid(c, 1, env));
        }
        return v;
    }
    case EMBI_CALC_EXP:
        return SDL_exp(eval_kid(c, 0, env));
    case EMBI_CALC_ABS:
        return SDL_fabs(eval_kid(c, 0, env));
    case EMBI_CALC_SIGN: {
        double v = eval_kid(c, 0, env);
        if (v != v) {
            return v;
        }
        if (v > 0) {
            return 1.0;
        }
        if (v < 0) {
            return -1.0;
        }
        return v; /* keeps the sign of zero */
    }
    default:
        return nan_value();
    }
}

static double eval_in(const embi_calc *c, const embi_calc_env *env, double size)
{
    ectx e = {0};
    if (env != nullptr) {
        e.pct_base = env->pct_base;
        e.len_px = env->len_px;
        e.user = env->user;
    }
    e.size = size;
    e.size_set = !isnan(size);
    return eval(c, &e);
}

double embi_calc_eval(const embi_calc *c, const embi_calc_env *env)
{
    if (!c) {
        return nan_value();
    }
    return eval_in(c, env, nan_value());
}

double embi_calc_eval_sized(const embi_calc *c, const embi_calc_env *env, double size)
{
    if (!c) {
        return nan_value();
    }
    return eval_in(c, env, size);
}

static bool constant_tree(const embi_calc *c)
{
    if (c->op == EMBI_CALC_SIZE) {
        return false;
    }
    if (c->op == EMBI_CALC_LEAF) {
        if (c->type == EMBI_CD_PCT) {
            return false;
        }
        return c->type != EMBI_CD_LEN || c->unit == EMBI_U_PX;
    }
    for (uint32_t i = 0; i < c->n; i++) {
        if (!constant_tree(c->kids[i])) {
            return false;
        }
    }
    return true;
}

bool embi_calc_is_constant(const embi_calc *c)
{
    return c != nullptr && constant_tree(c);
}

bool embi_calc_reduce(embi_arena *a, const embi_calc *c, double *value)
{
    (void)a;
    if (!embi_calc_is_constant(c)) {
        return false;
    }
    ectx e = {0};
    *value = eval(c, &e);
    return true;
}

/* ------------------------------------------------------------------ serialization */

static void put_lit(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

/* Precedence of a node in the written form: sums 1, products 2, everything else 3. */
static uint32_t prec_of(const embi_calc *c)
{
    switch (c->op) {
    case EMBI_CALC_ADD:
    case EMBI_CALC_SUB:
        return 1;
    case EMBI_CALC_MUL:
    case EMBI_CALC_DIV:
    case EMBI_CALC_NEG:
        return 2;
    default:
        return 3;
    }
}

static const char *fn_name_of(uint8_t op)
{
    switch (op) {
    case EMBI_CALC_MIN: return "min";
    case EMBI_CALC_MAX: return "max";
    case EMBI_CALC_CLAMP: return "clamp";
    case EMBI_CALC_ROUND: return "round";
    case EMBI_CALC_MOD: return "mod";
    case EMBI_CALC_REM: return "rem";
    case EMBI_CALC_SIN: return "sin";
    case EMBI_CALC_COS: return "cos";
    case EMBI_CALC_TAN: return "tan";
    case EMBI_CALC_ASIN: return "asin";
    case EMBI_CALC_ACOS: return "acos";
    case EMBI_CALC_ATAN: return "atan";
    case EMBI_CALC_ATAN2: return "atan2";
    case EMBI_CALC_POW: return "pow";
    case EMBI_CALC_SQRT: return "sqrt";
    case EMBI_CALC_HYPOT: return "hypot";
    case EMBI_CALC_LOG: return "log";
    case EMBI_CALC_EXP: return "exp";
    case EMBI_CALC_ABS: return "abs";
    case EMBI_CALC_SIGN: return "sign";
    default: return "";
    }
}

static void ser_node(embi_sb *sb, const embi_calc *c);

static void ser_child(embi_sb *sb, const embi_calc *k, bool paren)
{
    if (paren) {
        embi_sb_append_char(sb, '(');
    }
    ser_node(sb, k);
    if (paren) {
        embi_sb_append_char(sb, ')');
    }
}

static void ser_leaf(embi_sb *sb, const embi_calc *c)
{
    if (isnan(c->num)) {
        put_lit(sb, "NaN");
        return;
    }
    if (isinf(c->num)) {
        put_lit(sb, c->num < 0 ? "-infinity" : "infinity");
        return;
    }
    embi_css_append_num(sb, c->num);
    if (c->type == EMBI_CD_PCT) {
        embi_sb_append_char(sb, '%');
    } else if (c->type != EMBI_CD_NUM && c->unit != EMBI_U_NONE) {
        put_lit(sb, embi_unit_name(c->unit));
    }
}

static void ser_node(embi_sb *sb, const embi_calc *c)
{
    switch (c->op) {
    case EMBI_CALC_LEAF:
        ser_leaf(sb, c);
        return;
    case EMBI_CALC_SIZE:
        put_lit(sb, "size");
        return;
    case EMBI_CALC_NEG:
        put_lit(sb, "-1 * ");
        ser_child(sb, c->kids[0], prec_of(c->kids[0]) <= 2);
        return;
    case EMBI_CALC_ADD:
    case EMBI_CALC_SUB:
    case EMBI_CALC_MUL:
    case EMBI_CALC_DIV: {
        uint32_t p = prec_of(c);
        ser_child(sb, c->kids[0], prec_of(c->kids[0]) < p);
        put_lit(sb, c->op == EMBI_CALC_ADD ? " + " : c->op == EMBI_CALC_SUB ? " - "
                    : c->op == EMBI_CALC_MUL ? " * " : " / ");
        ser_child(sb, c->kids[1], prec_of(c->kids[1]) <= p);
        return;
    }
    case EMBI_CALC_ROUND:
        put_lit(sb, "round(");
        if (c->mode != EMBI_ROUND_NEAREST) {
            put_lit(sb, c->mode == EMBI_ROUND_UP ? "up, " : c->mode == EMBI_ROUND_DOWN ? "down, "
                        : "to-zero, ");
        }
        ser_node(sb, c->kids[0]);
        put_lit(sb, ", ");
        ser_node(sb, c->kids[1]);
        embi_sb_append_char(sb, ')');
        return;
    default:
        put_lit(sb, fn_name_of(c->op));
        embi_sb_append_char(sb, '(');
        for (uint32_t i = 0; i < c->n; i++) {
            if (i) {
                put_lit(sb, ", ");
            }
            ser_node(sb, c->kids[i]);
        }
        embi_sb_append_char(sb, ')');
        return;
    }
}

void embi_calc_serialize(embi_sb *sb, const embi_calc *c, bool bare)
{
    if (c == nullptr) {
        return;
    }
    bool wrap = !bare && (c->op == EMBI_CALC_LEAF || c->op == EMBI_CALC_SIZE || c->op == EMBI_CALC_NEG ||
                          c->op == EMBI_CALC_ADD || c->op == EMBI_CALC_SUB || c->op == EMBI_CALC_MUL ||
                          c->op == EMBI_CALC_DIV);
    if (wrap) {
        put_lit(sb, "calc(");
    }
    ser_node(sb, c);
    if (wrap) {
        embi_sb_append_char(sb, ')');
    }
}
