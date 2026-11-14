/*
 * Computed-value helpers for W2c: var() substitution (CSS Variables 1), registered custom property
 * syntax (CSS Properties and Values 1, the value side of @property; the registered-property
 * descriptor store itself (syntax, inherits, initial-value per name) is deferred to W2d, which owns
 * at-rules), relative length resolution
 * for the viewport and container units (the W3 hook), and the system color palette lookup.
 *
 * var() substitution runs at computed-value time. A reference to an unset variable takes its
 * fallback. A reference that cycles, or an expansion that exceeds the token limit, makes the value
 * guaranteed-invalid (the caller then uses the inherited or initial value).
 */
#include "css/props.h"
#include "css/value_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define EMBI_VAR_MAX_DEPTH 64
#define EMBI_VAR_DEFAULT_LIMIT 65536u

typedef EMBI_VEC(embi_css_cv) cv_vec;

typedef struct subst_ctx {
    const embi_var_env *env;
    embi_arena *a;
    embi_str stack[EMBI_VAR_MAX_DEPTH]; /* custom properties being expanded, outermost first */
    uint32_t depth;
    uint32_t count;                     /* component values emitted so far */
    uint64_t work;                      /* elements visited plus var() references expanded */
    uint32_t limit;
} subst_ctx;

static bool is_dashed(embi_str s)
{
    return s.len >= 2 && s.ptr[0] == '-' && s.ptr[1] == '-';
}

static bool is_ws(const embi_css_cv *cv)
{
    return cv->tok.type == EMBI_CSS_WS;
}

static bool is_var_fn(const embi_css_cv *cv)
{
    return cv->tok.type == EMBI_CSS_FUNCTION && embi_str_ieq(cv->tok.value, EMBI_STR("var"));
}

static bool on_stack(const subst_ctx *c, embi_str name)
{
    for (uint32_t i = 0; i < c->depth; i++) {
        if (embi_str_eq(c->stack[i], name)) {
            return true;
        }
    }
    return false;
}

static bool subst(subst_ctx *c, embi_css_slice in, cv_vec *out);

/* Every var() reference and every element visited costs one unit, so an expansion that emits
 * nothing (empty values, nested doubling) is still bounded. The budget is a multiple of the token
 * limit: a legitimate expansion visits about as many elements as it emits. */
static bool spend(subst_ctx *c)
{
    return ++c->work <= 4ull * c->limit;
}

static bool push_cv(subst_ctx *c, cv_vec *out, const embi_css_cv *cv)
{
    if (++c->count > c->limit) {
        return false;
    }
    embi_vec_push(out, *cv);
    return true;
}

/* Expands one var(--name [, fallback]) reference into out. */
static bool subst_var(subst_ctx *c, const embi_css_cv *cv, cv_vec *out)
{
    if (!spend(c)) {
        return false;
    }
    embi_css_slice k = {cv->kids, cv->nkids};
    uint32_t i = 0;
    while (i < k.n && is_ws(&k.p[i])) {
        i++;
    }
    if (i >= k.n || k.p[i].tok.type != EMBI_CSS_IDENT || !is_dashed(k.p[i].tok.value)) {
        return false;
    }
    embi_str name = k.p[i].tok.value;
    i++;
    while (i < k.n && is_ws(&k.p[i])) {
        i++;
    }
    bool has_fallback = false;
    embi_css_slice fb = {nullptr, 0};
    if (i < k.n) {
        if (k.p[i].tok.type != EMBI_CSS_COMMA) {
            return false;
        }
        has_fallback = true;
        fb = (embi_css_slice){k.p + i + 1, k.n - i - 1};
        while (fb.n > 0 && is_ws(&fb.p[0])) {
            fb.p++;
            fb.n--;
        }
        while (fb.n > 0 && is_ws(&fb.p[fb.n - 1])) {
            fb.n--;
        }
    }

    /* A cycle makes the value guaranteed-invalid, even when a fallback is present. */
    if (on_stack(c, name)) {
        return false;
    }
    embi_css_slice val = {nullptr, 0};
    if (c->env && c->env->lookup && c->env->lookup(c->env->user, name, &val)) {
        if (c->depth >= EMBI_VAR_MAX_DEPTH) {
            return false;
        }
        c->stack[c->depth++] = name;
        bool ok = subst(c, val, out);
        c->depth--;
        return ok;
    }
    if (!has_fallback) {
        return false;
    }
    return subst(c, fb, out);
}

static bool is_attr_fn(const embi_css_cv *cv)
{
    return cv->tok.type == EMBI_CSS_FUNCTION && embi_str_ieq(cv->tok.value, EMBI_STR("attr"));
}

/* Appends text, tokenized as component values, to out. */
static bool push_text(subst_ctx *c, cv_vec *out, embi_str text)
{
    embi_css_slice cvs = embi_css_parse_cvs(c->a, text);
    for (uint32_t i = 0; i < cvs.n; i++) {
        if (!push_cv(c, out, &cvs.p[i])) {
            return false;
        }
    }
    return true;
}

/* Spells the tokens of a type(...) argument back into a syntax string ("<length>", "*"). */
static bool syntax_text(const embi_css_cv *kids, uint32_t n, char *buf, size_t cap, size_t *len)
{
    size_t l = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_token *t = &kids[i].tok;
        if (t->type == EMBI_CSS_WS) {
            if (l > 0 && l < cap) {
                buf[l++] = ' ';
            }
        } else if (t->type == EMBI_CSS_DELIM && t->cp < 0x80) {
            if (l + 1 >= cap) {
                return false;
            }
            buf[l++] = (char)t->cp;
        } else if (t->type == EMBI_CSS_IDENT) {
            if (l + t->value.len >= cap) {
                return false;
            }
            memcpy(buf + l, t->value.ptr, t->value.len);
            l += t->value.len;
        } else {
            return false;
        }
    }
    while (l > 0 && buf[l - 1] == ' ') {
        l--;
    }
    *len = l;
    return l > 0;
}

/* Expands one attr([ns|]name [type(<syntax>) | raw-string | <unit>] [, fallback]) reference. */
static bool subst_attr(subst_ctx *c, const embi_css_cv *cv, cv_vec *out)
{
    if (!spend(c)) {
        return false;
    }
    embi_css_slice k = {cv->kids, cv->nkids};
    uint32_t i = 0;
    while (i < k.n && is_ws(&k.p[i])) {
        i++;
    }
    embi_str ns = {nullptr, 0};
    if (i + 2 < k.n && k.p[i].tok.type == EMBI_CSS_IDENT && k.p[i + 1].tok.type == EMBI_CSS_DELIM &&
        k.p[i + 1].tok.cp == '|' && k.p[i + 2].tok.type == EMBI_CSS_IDENT) {
        ns = k.p[i].tok.value;
        i += 2;
    }
    if (i >= k.n || k.p[i].tok.type != EMBI_CSS_IDENT) {
        return false;
    }
    embi_str name = k.p[i].tok.value;
    i++;
    while (i < k.n && is_ws(&k.p[i])) {
        i++;
    }
    bool has_syntax = false;
    char syn[96];
    size_t synlen = 0;
    embi_str unit = {nullptr, 0};
    if (i < k.n && k.p[i].tok.type != EMBI_CSS_COMMA) {
        const embi_css_cv *t = &k.p[i];
        if (t->tok.type == EMBI_CSS_FUNCTION && embi_str_ieq(t->tok.value, EMBI_STR("type"))) {
            if (!syntax_text(t->kids, t->nkids, syn, sizeof syn, &synlen)) {
                return false;
            }
            has_syntax = true;
        } else if (t->tok.type == EMBI_CSS_IDENT && embi_str_ieq(t->tok.value, EMBI_STR("raw-string"))) {
            /* the default: a string */
        } else if (t->tok.type == EMBI_CSS_IDENT) {
            unit = t->tok.value;
        } else if (t->tok.type == EMBI_CSS_DELIM && t->tok.cp == '%') {
            unit = EMBI_STR("%");
        } else {
            return false;
        }
        i++;
        while (i < k.n && is_ws(&k.p[i])) {
            i++;
        }
    }
    bool has_fallback = false;
    embi_css_slice fb = {nullptr, 0};
    if (i < k.n) {
        if (k.p[i].tok.type != EMBI_CSS_COMMA) {
            return false;
        }
        has_fallback = true;
        fb = (embi_css_slice){k.p + i + 1, k.n - i - 1};
        while (fb.n > 0 && is_ws(&fb.p[0])) {
            fb.p++;
            fb.n--;
        }
        while (fb.n > 0 && is_ws(&fb.p[fb.n - 1])) {
            fb.n--;
        }
    }

    embi_str val = {nullptr, 0};
    bool found = c->env && c->env->attr && c->env->attr(c->env->user, ns, name, &val);
    if (found) {
        if (has_syntax && synlen == 1 && syn[0] == '*') {
            return push_text(c, out, val);
        }
        if (has_syntax) {
            embi_css_slice toks = embi_css_parse_cvs(c->a, val);
            embi_value tmp;
            embi_str sx = {syn, synlen};
            if (embi_css_registered_parse(c->a, sx, toks, &tmp)) {
                return push_text(c, out, val);
            }
        } else if (unit.len > 0) {
            embi_css_slice toks = embi_css_parse_cvs(c->a, val);
            uint32_t s = 0, e = toks.n;
            while (s < e && toks.p[s].tok.type == EMBI_CSS_WS) {
                s++;
            }
            while (e > s && toks.p[e - 1].tok.type == EMBI_CSS_WS) {
                e--;
            }
            if (e == s + 1 && toks.p[s].tok.type == EMBI_CSS_NUMBER) {
                size_t n = toks.p[s].tok.repr.len + unit.len;
                char *copy = embi_arena_alloc(c->a, n, 1);
                memcpy(copy, toks.p[s].tok.repr.ptr, toks.p[s].tok.repr.len);
                memcpy(copy + toks.p[s].tok.repr.len, unit.ptr, unit.len);
                return push_text(c, out, (embi_str){copy, n});
            }
        } else {
            embi_css_cv sv = {0};
            sv.tok.type = EMBI_CSS_STRING;
            sv.tok.value = val;
            return push_cv(c, out, &sv);
        }
    } else if (!has_fallback && !has_syntax && unit.len == 0) {
        embi_css_cv sv = {0};
        sv.tok.type = EMBI_CSS_STRING;
        sv.tok.value = (embi_str){"", 0};
        return push_cv(c, out, &sv);
    }
    if (!has_fallback) {
        return false;
    }
    return subst(c, fb, out);
}

static bool subst(subst_ctx *c, embi_css_slice in, cv_vec *out)
{
    for (uint32_t i = 0; i < in.n; i++) {
        const embi_css_cv *cv = &in.p[i];
        if (!spend(c)) {
            return false;
        }
        if (is_attr_fn(cv)) {
            if (!subst_attr(c, cv, out)) {
                return false;
            }
            continue;
        }
        if (cv->tok.type == EMBI_CSS_FUNCTION && cv->nkids == 0 &&
            (embi_str_ieq(cv->tok.value, EMBI_STR("sibling-index")) ||
             embi_str_ieq(cv->tok.value, EMBI_STR("sibling-count")))) {
            bool count = embi_str_ieq(cv->tok.value, EMBI_STR("sibling-count"));
            uint32_t n = c->env != nullptr && c->env->sibling != nullptr ? c->env->sibling(c->env->user, count) : 1;
            char nb[16];
            int nl = snprintf(nb, sizeof nb, "%u", (unsigned)n);
            if (!push_text(c, out, (embi_str){nb, (size_t)nl})) {
                return false;
            }
            continue;
        }
        if (is_var_fn(cv)) {
            if (!subst_var(c, cv, out)) {
                return false;
            }
            continue;
        }
        if (cv->nkids == 0) {
            if (!push_cv(c, out, cv)) {
                return false;
            }
            continue;
        }
        /* A function or a simple block: substitute its contents into a fresh child list. */
        cv_vec kids = {0};
        bool ok = subst(c, (embi_css_slice){cv->kids, cv->nkids}, &kids);
        if (!ok) {
            embi_vec_free(&kids);
            return false;
        }
        embi_css_cv nc = *cv;
        nc.kids = kids.len > 0 ? embi_css_arena_copy(c->a, kids.data, kids.len,
                                                     sizeof(embi_css_cv))
                               : nullptr;
        nc.nkids = kids.len;
        embi_vec_free(&kids);
        if (!push_cv(c, out, &nc)) {
            return false;
        }
    }
    return true;
}

bool embi_css_substitute_vars(embi_arena *a, const embi_var_env *env, embi_str self,
                              embi_css_slice value, embi_css_slice *out)
{
    subst_ctx c = {0};
    c.env = env;
    c.a = a;
    c.limit = env && env->max_tokens ? env->max_tokens : EMBI_VAR_DEFAULT_LIMIT;
    if (self.len > 0) {
        c.stack[0] = self;
        c.depth = 1;
    }
    cv_vec v = {0};
    bool ok = subst(&c, value, &v);
    if (ok) {
        out->p = v.len > 0 ? embi_css_arena_copy(a, v.data, v.len, sizeof(embi_css_cv)) : nullptr;
        out->n = v.len;
    }
    embi_vec_free(&v);
    return ok;
}

uint32_t embi_css_parse_decl_vars(embi_arena *a, embi_prop_id id, embi_css_slice value,
                                  const embi_var_env *env, embi_longhand *out, uint32_t cap)
{
    embi_css_slice sub = value;
    if (embi_css_has_var(value) || embi_css_has_attr(value)) {
        if (!embi_css_substitute_vars(a, env, (embi_str){nullptr, 0}, value, &sub)) {
            return 0;
        }
    }
    return embi_css_parse_decl(a, id, sub, out, cap);
}

/* ------------------------------------------------------------------ registered syntax */

/* Maps a syntax component name to the value types it accepts. Returns 0 for unknown names. */
static uint32_t syntax_types(embi_str name)
{
    if (embi_str_ieq(name, EMBI_STR("length"))) {
        return EMBI_T_LEN;
    }
    if (embi_str_ieq(name, EMBI_STR("number"))) {
        return EMBI_T_NUM;
    }
    if (embi_str_ieq(name, EMBI_STR("integer"))) {
        return EMBI_T_INT;
    }
    if (embi_str_ieq(name, EMBI_STR("percentage"))) {
        return EMBI_T_PCT;
    }
    if (embi_str_ieq(name, EMBI_STR("length-percentage"))) {
        return EMBI_T_LENPCT;
    }
    if (embi_str_ieq(name, EMBI_STR("color"))) {
        return EMBI_T_COLOR;
    }
    if (embi_str_ieq(name, EMBI_STR("angle"))) {
        return EMBI_T_ANGLE;
    }
    if (embi_str_ieq(name, EMBI_STR("time"))) {
        return EMBI_T_TIME;
    }
    if (embi_str_ieq(name, EMBI_STR("resolution"))) {
        return EMBI_T_RES;
    }
    if (embi_str_ieq(name, EMBI_STR("url"))) {
        return EMBI_T_URL;
    }
    if (embi_str_ieq(name, EMBI_STR("string"))) {
        return EMBI_T_STR;
    }
    if (embi_str_ieq(name, EMBI_STR("custom-ident"))) {
        return EMBI_T_IDENT;
    }
    return 0;
}

static embi_str trim_str(embi_str s)
{
    while (s.len > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t' || s.ptr[0] == '\n')) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t' ||
                         s.ptr[s.len - 1] == '\n')) {
        s.len--;
    }
    return s;
}

static bool css_wide_kw(embi_str s)
{
    static const char *const kws[] = {"initial", "inherit", "unset", "revert", "revert-layer",
                                      "default"};
    for (size_t i = 0; i < sizeof kws / sizeof kws[0]; i++) {
        if (embi_str_ieq(s, (embi_str){kws[i], (uint32_t)strlen(kws[i])})) {
            return true;
        }
    }
    return false;
}

/* Matches one component value against the data type <name>. */
static bool match_type(embi_arena *a, embi_str name, const embi_css_cv *cv, embi_value *out)
{
    if (embi_str_ieq(name, EMBI_STR("image"))) {
        return embi_parse_image(a, cv, out);
    }
    if (embi_str_ieq(name, EMBI_STR("transform-function"))) {
        if (cv->tok.type != EMBI_CSS_FUNCTION) {
            return false;
        }
        embi_gctx g = {a, 0, nullptr};
        const embi_css_cv *one[1] = {cv};
        embi_cur c = {one, 1, 0};
        return embi_gram_transform(&g, &c, out);
    }
    if (embi_str_ieq(name, EMBI_STR("custom-ident"))) {
        if (cv->tok.type != EMBI_CSS_IDENT || css_wide_kw(cv->tok.value)) {
            return false;
        }
        *out = (embi_value){0};
        out->kind = EMBI_V_IDENT;
        out->ident = embi_atom_intern(cv->tok.value.ptr, cv->tok.value.len);
        return true;
    }
    uint32_t types = syntax_types(name);
    return types != 0 && embi_value_parse_types(a, types, cv, out);
}

/* Matches the whole (trimmed) value against one syntax alternative. */
static bool match_alt(embi_arena *a, embi_str alt, embi_css_slice value, embi_value *out)
{
    char mult = 0;
    if (alt.len >= 4 && alt.ptr[0] == '<' &&
        (alt.ptr[alt.len - 1] == '+' || alt.ptr[alt.len - 1] == '#') &&
        alt.ptr[alt.len - 2] == '>') {
        mult = alt.ptr[alt.len - 1];
        alt.len--;
    }
    if (alt.len < 3 || alt.ptr[0] != '<' || alt.ptr[alt.len - 1] != '>') {
        /* A keyword: case-sensitive, exactly one ident. */
        if (alt.len == 0 || value.n != 1 || value.p[0].tok.type != EMBI_CSS_IDENT ||
            !embi_str_eq(value.p[0].tok.value, alt)) {
            return false;
        }
        *out = (embi_value){0};
        out->kind = EMBI_V_IDENT;
        out->ident = embi_atom_intern(alt.ptr, alt.len);
        return true;
    }
    embi_str name = {alt.ptr + 1, alt.len - 2};
    embi_cur cur;
    embi_cur_make_slice(a, value, &cur);
    if (cur_done(&cur)) {
        return false;
    }
    if (embi_str_ieq(name, EMBI_STR("transform-list"))) {
        if (mult) {
            return false;
        }
        embi_gctx g = {a, 0, nullptr};
        return embi_gram_transform(&g, &cur, out) && cur_done(&cur);
    }
    if (!mult) {
        return cur_left(&cur) == 1 && match_type(a, name, cur_peek(&cur), out);
    }
    uint32_t nseg = 1;
    embi_cur *segs = &cur;
    if (mult == '#') {
        segs = embi_cur_split(a, &cur, &nseg);
        if (!segs) {
            return false;
        }
    }
    /* '+': every component value is one item; '#': every comma segment is exactly one item. */
    uint32_t n = mult == '+' ? cur_left(&cur) : nseg;
    embi_value *items = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_cv *cv;
        if (mult == '+') {
            cv = cur_take(&cur);
        } else {
            if (cur_left(&segs[i]) != 1) {
                return false;
            }
            cv = cur_peek(&segs[i]);
        }
        if (!match_type(a, name, cv, &items[i])) {
            return false;
        }
    }
    return embi_make_list(a, items, n, mult == '+' ? EMBI_SEP_SPACE : EMBI_SEP_COMMA, out);
}

bool embi_css_registered_parse(embi_arena *a, embi_str syntax, embi_css_slice value,
                               embi_value *out)
{
    syntax = trim_str(syntax);
    /* Trim whitespace components. */
    while (value.n > 0 && value.p[0].tok.type == EMBI_CSS_WS) {
        value.p++;
        value.n--;
    }
    while (value.n > 0 && value.p[value.n - 1].tok.type == EMBI_CSS_WS) {
        value.n--;
    }
    if (embi_str_eq(syntax, EMBI_STR("*"))) {
        /* The universal syntax accepts an empty value (--x:;) too. */
        if (value.n > 0 && !embi_css_value_opaque_ok(value)) {
            return false;
        }
        *out = (embi_value){0};
        out->kind = EMBI_V_RAW;
        out->raw = value;
        return true;
    }
    if (value.n == 0) {
        return false;
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i <= syntax.len; i++) {
        if (i < syntax.len && syntax.ptr[i] != '|') {
            continue;
        }
        embi_str alt = trim_str((embi_str){syntax.ptr + start, i - start});
        start = i + 1;
        if (match_alt(a, alt, value, out)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ relative lengths (W3 hook) */

/* Relative length units. The container units use the nearest query container's content box and
 * fall back to the viewport when the element has no container. */
/* A font metric when the embedder supplied a usable one, otherwise the Values 4 fallback. */
static double metric(double supplied, double fallback)
{
    return supplied > 0 && supplied == supplied ? supplied : fallback;
}

double embi_len_px_cb(void *user, uint8_t unit, double value)
{
    const embi_len_ctx *c = user;
    double font = c->font_px;
    double root = c->root_px;
    double cw = c->cq_w == c->cq_w ? c->cq_w : c->vw;
    double ch = c->cq_h == c->cq_h ? c->cq_h : c->vh;
    double vmin = c->vw < c->vh ? c->vw : c->vh;
    double vmax = c->vw > c->vh ? c->vw : c->vh;
    double cmin = cw < ch ? cw : ch;
    double cmax = cw > ch ? cw : ch;
    switch (unit) {
    case EMBI_U_PX:
        return value;
    case EMBI_U_EM:
        return value * font;
    case EMBI_U_REM:
        return value * root;
    case EMBI_U_EX:
        return value * metric(c->font_metrics.ex, 0.5 * font);
    case EMBI_U_REX:
        return value * metric(c->root_metrics.ex, 0.5 * root);
    case EMBI_U_CH:
        return value * metric(c->font_metrics.ch, 0.5 * font);
    case EMBI_U_RCH:
        return value * metric(c->root_metrics.ch, 0.5 * root);
    case EMBI_U_CAP:
        return value * metric(c->font_metrics.cap, 0.7 * font);
    case EMBI_U_RCAP:
        return value * metric(c->root_metrics.cap, 0.7 * root);
    case EMBI_U_IC:
        return value * metric(c->font_metrics.ic, font);
    case EMBI_U_RIC:
        return value * metric(c->root_metrics.ic, root);
    case EMBI_U_LH:
        return value * metric(c->font_metrics.lh, 1.2 * font);
    case EMBI_U_RLH:
        return value * metric(c->root_metrics.lh, 1.2 * root);
    case EMBI_U_VW: case EMBI_U_SVW: case EMBI_U_LVW: case EMBI_U_DVW: case EMBI_U_VI:
    case EMBI_U_SVI: case EMBI_U_LVI: case EMBI_U_DVI:
        return value * c->vw / 100.0;
    case EMBI_U_VH: case EMBI_U_SVH: case EMBI_U_LVH: case EMBI_U_DVH: case EMBI_U_VB:
    case EMBI_U_SVB: case EMBI_U_LVB: case EMBI_U_DVB:
        return value * c->vh / 100.0;
    case EMBI_U_VMIN: case EMBI_U_SVMIN: case EMBI_U_LVMIN: case EMBI_U_DVMIN:
        return value * vmin / 100.0;
    case EMBI_U_VMAX: case EMBI_U_SVMAX: case EMBI_U_LVMAX: case EMBI_U_DVMAX:
        return value * vmax / 100.0;
    case EMBI_U_CQW: case EMBI_U_CQI:
        return value * cw / 100.0;
    case EMBI_U_CQH: case EMBI_U_CQB:
        return value * ch / 100.0;
    case EMBI_U_CQMIN:
        return value * cmin / 100.0;
    case EMBI_U_CQMAX:
        return value * cmax / 100.0;
    default:
        return NAN;
    }
}

/* ------------------------------------------------------------------ system colors */

/* Default palettes for the system color keywords, in embi_system_color_names order. The embedder
 * replaces them with the platform values through embi_color_system_palette. */
static const embi_color_rgb8 default_light[] = {
    {255, 255, 255}, {0, 0, 0},       {0, 0, 238},     {85, 26, 139},  {255, 0, 0},
    {239, 239, 239}, {0, 0, 0},       {118, 118, 118}, {255, 255, 255}, {0, 0, 0},
    {0, 120, 215},   {255, 255, 255}, {0, 120, 215},   {255, 255, 255}, {255, 255, 0},
    {0, 0, 0},       {109, 109, 109}, {0, 117, 255},   {255, 255, 255},
};
static const embi_color_rgb8 default_dark[] = {
    {18, 18, 18},    {255, 255, 255}, {140, 180, 255}, {200, 160, 255}, {255, 90, 90},
    {50, 50, 50},    {255, 255, 255}, {120, 120, 120}, {30, 30, 30},    {255, 255, 255},
    {0, 90, 180},    {255, 255, 255}, {0, 90, 180},    {255, 255, 255}, {120, 100, 0},
    {255, 255, 255}, {150, 150, 150}, {90, 150, 255},  {0, 0, 0},
};

static_assert(sizeof default_light / sizeof default_light[0] == EMBI_SYSTEM_COLOR_COUNT,
              "default_light palette must have one entry per system color");
static_assert(sizeof default_dark / sizeof default_dark[0] == EMBI_SYSTEM_COLOR_COUNT,
              "default_dark palette must have one entry per system color");

bool embi_color_system_value(uint16_t index, bool dark, const embi_color_rgb8 *palette,
                             embi_color *out)
{
    if (index >= EMBI_SYSTEM_COLOR_COUNT) {
        return false;
    }
    /* A caller-supplied palette must hold at least EMBI_SYSTEM_COLOR_COUNT entries. */
    const embi_color_rgb8 *p = palette ? palette : (dark ? default_dark : default_light);
    *out = (embi_color){0};
    out->space = EMBI_CS_SRGB;
    out->flags = EMBI_CF_FUNC;
    out->alpha = 1.0;
    out->c[0] = p[index].r / 255.0;
    out->c[1] = p[index].g / 255.0;
    out->c[2] = p[index].b / 255.0;
    return true;
}
