/*
 * Property lookup and single-value parsing (W2c). Shorthand expansion is in shorthand.c, colors in
 * color.c, math in calc.c, and units in value_num.c.
 */
#define EMBI_PROPS_TABLES
#include "css/value_int.h"
#include "num/num.h"

#include <string.h>

/* ------------------------------------------------------------------ small helpers */

static bool lower_buf(embi_str s, char *buf, size_t cap)
{
    if (s.len == 0 || s.len >= cap) {
        return false;
    }
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.ptr[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[s.len] = '\0';
    return true;
}

static bool cv_is_ws(const embi_css_cv *cv)
{
    return cv->tok.type == EMBI_CSS_WS;
}

static embi_css_slice trim(embi_css_slice s)
{
    while (s.n > 0 && cv_is_ws(s.p)) {
        s.p++;
        s.n--;
    }
    while (s.n > 0 && cv_is_ws(&s.p[s.n - 1])) {
        s.n--;
    }
    return s;
}

/* True when the slice (trimmed) is exactly one component value. */
static bool single(embi_css_slice s, const embi_css_cv **out)
{
    s = trim(s);
    if (s.n != 1) {
        return false;
    }
    *out = s.p;
    return true;
}

/* ASCII case-insensitive comparison of a function or ident name with a lowercase literal. */
static bool name_is(embi_str name, const char *lit)
{
    size_t n = strlen(lit);
    if (name.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

static bool is_fn(const embi_css_cv *cv, const char *lit)
{
    return cv->tok.type == EMBI_CSS_FUNCTION && name_is(cv->tok.value, lit);
}

/* Writes a number with at most six decimals, trailing zeros removed (the CSS serialization used
 * by the color tests). */
void embi_css_append_num(embi_sb *sb, double v)
{
    if (v != v) {
        embi_sb_append(sb, EMBI_STR("NaN"));
        return;
    }
    /* Round the shortest round-trip decimal half up at six places. Rounding the exact binary value
     * instead would print 42.4989375 (stored just below the midpoint) as 42.498937. */
    char s[EMBI_NUM_STR_MAX];
    embi_num_to_str(v, s);
    if (strchr(s, 'e') || strchr(s, 'I') || strchr(s, 'N')) {
        if (strchr(s, 'I') || strchr(s, 'N')) {
            embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
            return;
        }
        SDL_snprintf(s, sizeof s, "%.6f", v);
    }
    bool neg = s[0] == '-';
    const char *p = neg ? s + 1 : s;
    char d[EMBI_NUM_STR_MAX + 16];
    size_t n = 0;
    while (*p && *p != '.') {
        d[n++] = *p++;
    }
    size_t int_digits = n;
    if (*p == '.') {
        p++;
        while (*p) {
            d[n++] = *p++;
        }
    }
    size_t target = int_digits + 6;
    if (n > target) {
        bool up = d[target] >= '5';
        n = target;
        if (up) {
            size_t i = n;
            while (i > 0 && d[i - 1] == '9') {
                d[--i] = '0';
            }
            if (i > 0) {
                d[i - 1]++;
            } else {
                memmove(d + 1, d, n);
                d[0] = '1';
                n++;
                int_digits++;
            }
        }
    } else {
        while (n < target) {
            d[n++] = '0';
        }
    }
    size_t frac_end = n;
    while (frac_end > int_digits && d[frac_end - 1] == '0') {
        frac_end--;
    }
    size_t int_start = 0;
    while (int_start + 1 < int_digits && d[int_start] == '0') {
        int_start++;
    }
    bool any_nonzero = false;
    for (size_t i = int_start; i < frac_end; i++) {
        if (d[i] != '0') {
            any_nonzero = true;
            break;
        }
    }
    if (neg && any_nonzero) {
        embi_sb_append_char(sb, '-');
    }
    embi_sb_append(sb, (embi_str){d + int_start, (uint32_t)(int_digits - int_start)});
    if (frac_end > int_digits) {
        embi_sb_append_char(sb, '.');
        embi_sb_append(sb, (embi_str){d + int_digits, (uint32_t)(frac_end - int_digits)});
    }
}

/* ------------------------------------------------------------------ lookups */

embi_prop_id embi_prop_lookup(embi_str name)
{
    char buf[128];
    if (!lower_buf(name, buf, sizeof buf)) {
        return EMBI_PROP_NONE;
    }
    size_t lo = 0;
    size_t hi = sizeof embi_prop_name_table / sizeof embi_prop_name_table[0];
    /* The legacy names of the gap properties (CSS Box Alignment 3 section 8.1, CSS Grid 1): grid-gap
     * and its two longhands are aliases of gap, row-gap and column-gap. */
    if (strncmp(buf, "grid-", 5) == 0 &&
        (strcmp(buf + 5, "gap") == 0 || strcmp(buf + 5, "row-gap") == 0 || strcmp(buf + 5, "column-gap") == 0)) {
        memmove(buf, buf + 5, strlen(buf + 5) + 1);
    }
    if (strcmp(buf, "-webkit-box-decoration-break") == 0) { /* the prefixed alias of box-decoration-break */
        memmove(buf, buf + 8, strlen(buf + 8) + 1); /* drops "-webkit-" */
    }
    if (strcmp(buf, "word-wrap") == 0) { /* the legacy name of overflow-wrap (CSS Text 3 section 5.5) */
        memcpy(buf, "overflow-wrap", sizeof "overflow-wrap");
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(embi_prop_name_table[mid].name, buf);
        if (c == 0) {
            return embi_prop_name_table[mid].id;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return EMBI_PROP_NONE;
}

const embi_prop_info *embi_prop_info_of(embi_prop_id id)
{
    if (id == EMBI_PROP_NONE || id >= EMBI_PROP_COUNT) {
        return nullptr;
    }
    return &embi_prop_infos[id];
}

const uint16_t *embi_prop_longhands(embi_prop_id id)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    if (!pi) {
        return nullptr;
    }
    return &embi_prop_sub_list[pi->sub_off];
}

uint16_t embi_kw_lookup(embi_str name)
{
    char buf[64];
    if (!lower_buf(name, buf, sizeof buf)) {
        return EMBI_KW_INVALID;
    }
    size_t lo = 0;
    size_t hi = sizeof embi_kw_name_table / sizeof embi_kw_name_table[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(embi_kw_name_table[mid].name, buf);
        if (c == 0) {
            return embi_kw_name_table[mid].id;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return EMBI_KW_INVALID;
}

const char *embi_kw_name(uint16_t kw)
{
    if (kw == EMBI_KW_INVALID || kw >= EMBI_KW_COUNT) {
        return nullptr;
    }
    return embi_kw_names[kw];
}

/* The keyword is valid for this property when it is in the property's keyword list. */
bool embi_prop_has_kw(const embi_prop_info *pi, uint16_t kw)
{
    for (uint16_t i = 0; i < pi->kw_n; i++) {
        if (embi_prop_kw_list[pi->kw_off + i] == kw) {
            return true;
        }
    }
    return false;
}

static bool prop_has_kw(const embi_prop_info *pi, uint16_t kw)
{
    return embi_prop_has_kw(pi, kw);
}

/* overflow: overlay is a legacy alias of auto, replaced at parse time (CSS Overflow 3 section 3.1, overflow-overlay). */
static bool overlay_alias(const embi_prop_info *pi, const embi_css_cv *cv)
{
    return cv != nullptr && cv->tok.type == EMBI_CSS_IDENT && embi_str_ieq(cv->tok.value, EMBI_STR("overlay")) &&
           pi->name != nullptr && strncmp(pi->name, "overflow-", 9) == 0 && embi_prop_has_kw(pi, EMBI_KW_auto);
}

bool embi_gram_kw(const embi_prop_info *pi, const embi_css_cv *cv, embi_value *out)
{
    uint16_t kw = cv_kw(cv);
    if (overlay_alias(pi, cv)) {
        kw = EMBI_KW_auto;
    }
    if (kw == EMBI_KW_INVALID || !embi_prop_has_kw(pi, kw)) {
        return false;
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_KEYWORD;
    out->kw = kw;
    return true;
}

/* ------------------------------------------------------------------ CSS-wide keywords and var() */

static int wide_kind(embi_str s)
{
    if (embi_str_ieq(s, EMBI_STR("initial"))) {
        return EMBI_V_INITIAL;
    }
    if (embi_str_ieq(s, EMBI_STR("inherit"))) {
        return EMBI_V_INHERIT;
    }
    if (embi_str_ieq(s, EMBI_STR("unset"))) {
        return EMBI_V_UNSET;
    }
    if (embi_str_ieq(s, EMBI_STR("revert"))) {
        return EMBI_V_REVERT;
    }
    if (embi_str_ieq(s, EMBI_STR("revert-layer"))) {
        return EMBI_V_REVERT_LAYER;
    }
    return 0;
}

static bool cvs_has_fn(embi_css_slice s, const char *name)
{
    for (uint32_t i = 0; i < s.n; i++) {
        const embi_css_cv *cv = &s.p[i];
        if (is_fn(cv, name)) {
            return true;
        }
        if (cv->nkids > 0 && cvs_has_fn((embi_css_slice){cv->kids, cv->nkids}, name)) {
            return true;
        }
    }
    return false;
}

bool embi_css_has_var(embi_css_slice in)
{
    return cvs_has_fn(in, "var");
}

bool embi_css_has_attr(embi_css_slice in)
{
    /* sibling-index() and sibling-count() depend on the element like attr() does, so they take the same path */
    return cvs_has_fn(in, "attr") || cvs_has_fn(in, "sibling-index") || cvs_has_fn(in, "sibling-count");
}

/* ------------------------------------------------------------------ single components */

static uint8_t allowed_classes(uint32_t types)
{
    uint8_t m = 0;
    if (types & EMBI_T_LEN) {
        m |= EMBI_CD_LEN;
    }
    if (types & EMBI_T_PCT) {
        m |= EMBI_CD_PCT;
    }
    if (types & (EMBI_T_NUM | EMBI_T_INT | EMBI_T_ALPHA)) {
        m |= EMBI_CD_NUM;
    }
    if (types & EMBI_T_ANGLE) {
        m |= EMBI_CD_ANGLE;
    }
    if (types & EMBI_T_TIME) {
        m |= EMBI_CD_TIME;
    }
    if (types & EMBI_T_RES) {
        m |= EMBI_CD_RES;
    }
    return m;
}

static const char *const math_fns[] = {
    "calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan", "asin", "acos",
    "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign",
};

static bool is_math_fn(const embi_css_cv *cv)
{
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    for (size_t i = 0; i < sizeof math_fns / sizeof math_fns[0]; i++) {
        if (name_is(cv->tok.value, math_fns[i])) {
            return true;
        }
    }
    return false;
}

static const char *const color_fns[] = {
    "rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch", "color", "color-mix",
    "device-cmyk",
};

static bool is_color_fn(const embi_css_cv *cv)
{
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    for (size_t i = 0; i < sizeof color_fns / sizeof color_fns[0]; i++) {
        if (name_is(cv->tok.value, color_fns[i])) {
            return true;
        }
    }
    return false;
}

/* Stores a calc() or math function. Constant results collapse to a plain value. */
static bool store_calc(embi_arena *a, uint32_t types, const embi_css_cv *cv, embi_value *out)
{
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    if (!embi_calc_parse_cv(a, cv, &node, &type)) {
        return false;
    }
    if (type == 0 || (type & ~allowed_classes(types)) != 0) {
        return false;
    }
    if (embi_calc_is_constant(node)) {
        double v = 0;
        if (!embi_calc_reduce(a, node, &v)) {
            return false;
        }
        if (type == EMBI_CD_NUM) {
            if ((types & EMBI_T_ALPHA) != 0) {
                v = v < 0 ? 0 : (v > 1 ? 1 : v);
            }
            if ((types & EMBI_T_INT) != 0 && (types & EMBI_T_NUM) == 0) {
                /* An <integer> from calc() rounds half up (CSS Values 4 section 10.9.1). */
                out->kind = EMBI_V_INTEGER;
                out->num = __builtin_floor(v + 0.5);
            } else {
                out->kind = EMBI_V_NUMBER;
                out->num = v;
            }
        } else if (type == EMBI_CD_LEN) {
            out->kind = EMBI_V_LENGTH;
            out->unit = EMBI_U_PX;
            out->num = v;
        } else if (type == EMBI_CD_ANGLE) {
            out->kind = EMBI_V_ANGLE;
            out->unit = EMBI_U_DEG;
            out->num = v;
        } else if (type == EMBI_CD_TIME) {
            out->kind = EMBI_V_TIME;
            out->unit = EMBI_U_S;
            out->num = v;
        } else if (type == EMBI_CD_RES) {
            out->kind = EMBI_V_RESOLUTION;
            out->unit = EMBI_U_DPPX;
            out->num = v;
        } else {
            out->kind = EMBI_V_CALC;
            out->calc = node;
        }
        return true;
    }
    out->kind = EMBI_V_CALC;
    out->calc = node;
    return true;
}

/* Parses a light-dark(light, dark) pair of colors. */
static bool store_light_dark(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    embi_value *pair = EMBI_NEW_ARRAY(a, embi_value, 2);
    uint32_t idx = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= cv->nkids; i++) {
        if (i == cv->nkids || cv->kids[i].tok.type == EMBI_CSS_COMMA) {
            embi_css_slice part = trim((embi_css_slice){cv->kids + start, i - start});
            if (part.n != 1 || idx >= 2) {
                return false;
            }
            embi_color c;
            if (!embi_color_parse_cv(part.p, &c)) {
                return false;
            }
            /* Computed light-dark() colors are kept in the color(srgb ...) function form. */
            if (c.space == EMBI_CS_SRGB) {
                c.flags |= EMBI_CF_FUNC;
            }
            pair[idx].kind = EMBI_V_COLOR;
            pair[idx].color = c;
            idx++;
            start = i + 1;
        }
    }
    if (idx != 2) {
        return false;
    }
    out->kind = EMBI_V_LIGHT_DARK;
    out->pair = pair;
    return true;
}

static bool parse_one_cv(embi_arena *a, const embi_prop_info *pi, const embi_css_cv *cv,
                         embi_value *out)
{
    if (!cv) {
        return false;
    }
    const uint32_t types = pi->types;
    const embi_css_token *t = &cv->tok;
    *out = (embi_value){0};
    switch (t->type) {
    case EMBI_CSS_IDENT: {
        uint16_t kw = overlay_alias(pi, cv) ? EMBI_KW_auto : embi_kw_lookup(t->value);
        if (kw != EMBI_KW_INVALID && prop_has_kw(pi, kw)) {
            out->kind = EMBI_V_KEYWORD;
            out->kw = kw;
            return true;
        }
        if (types & EMBI_T_COLOR) {
            embi_color c;
            if (embi_color_parse_cv(cv, &c)) {
                out->kind = EMBI_V_COLOR;
                out->color = c;
                return true;
            }
        }
        if (types & EMBI_T_IDENT) {
            out->kind = EMBI_V_IDENT;
            out->ident = embi_atom_intern(t->value.ptr, t->value.len);
            return true;
        }
        return false;
    }
    case EMBI_CSS_NUMBER: {
        double v = t->num;
        if (types & EMBI_T_ALPHA) {
            out->kind = EMBI_V_NUMBER;
            out->num = v < 0 ? 0 : (v > 1 ? 1 : v);
            return true;
        }
        if ((types & EMBI_T_NONNEG) && v < 0) {
            return false;
        }
        if ((types & EMBI_T_POSINT) && v < 1) {
            return false;
        }
        if ((types & EMBI_T_INT) && t->flag) {
            out->kind = EMBI_V_INTEGER;
            out->num = v;
            return true;
        }
        if (types & EMBI_T_NUM) {
            out->kind = EMBI_V_NUMBER;
            out->num = v;
            return true;
        }
        if ((types & EMBI_T_LEN) && v == 0) {
            out->kind = EMBI_V_LENGTH;
            out->unit = EMBI_U_PX;
            out->num = 0;
            return true;
        }
        return false;
    }
    case EMBI_CSS_PERCENTAGE: {
        double v = t->num;
        if (types & EMBI_T_ALPHA) {
            v = v / 100.0;
            out->kind = EMBI_V_NUMBER;
            out->num = v < 0 ? 0 : (v > 1 ? 1 : v);
            return true;
        }
        if (!(types & EMBI_T_PCT)) {
            return false;
        }
        if ((types & EMBI_T_NONNEG) && v < 0) {
            return false;
        }
        out->kind = EMBI_V_PERCENT;
        out->num = v;
        return true;
    }
    case EMBI_CSS_DIMENSION: {
        uint8_t unit = 0;
        uint8_t cls = 0;
        double scale = 1.0;
        if (!embi_unit_lookup(t->value, &unit, &cls, &scale)) {
            return false;
        }
        if ((types & EMBI_T_NONNEG) && t->num < 0) {
            return false;
        }
        double v = t->num * scale;
        if (cls == EMBI_CD_LEN && (types & EMBI_T_LEN)) {
            out->kind = EMBI_V_LENGTH;
            out->unit = unit;
            out->num = v;
            return true;
        }
        if (cls == EMBI_CD_ANGLE && (types & EMBI_T_ANGLE)) {
            out->kind = EMBI_V_ANGLE;
            out->unit = EMBI_U_DEG;
            out->num = v;
            return true;
        }
        if (cls == EMBI_CD_TIME && (types & EMBI_T_TIME)) {
            out->kind = EMBI_V_TIME;
            out->unit = EMBI_U_S;
            out->num = v;
            return true;
        }
        if (cls == EMBI_CD_RES && (types & EMBI_T_RES)) {
            out->kind = EMBI_V_RESOLUTION;
            out->unit = EMBI_U_DPPX;
            out->num = v;
            return true;
        }
        return false;
    }
    case EMBI_CSS_STRING:
        if (types & EMBI_T_STR) {
            out->kind = EMBI_V_STRING;
            out->str = t->value;
            return true;
        }
        return false;
    case EMBI_CSS_URL:
        if (types & EMBI_T_URL) {
            out->kind = EMBI_V_URL;
            out->str = t->value;
            return true;
        }
        return false;
    case EMBI_CSS_HASH:
        if (types & EMBI_T_COLOR) {
            embi_color c;
            if (embi_color_parse_cv(cv, &c)) {
                out->kind = EMBI_V_COLOR;
                out->color = c;
                return true;
            }
        }
        return false;
    case EMBI_CSS_FUNCTION:
        if (is_fn(cv, "url")) {
            if (!(types & EMBI_T_URL)) {
                return false;
            }
            embi_css_slice k = trim((embi_css_slice){cv->kids, cv->nkids});
            if (k.n == 1 && k.p->tok.type == EMBI_CSS_STRING) {
                out->kind = EMBI_V_URL;
                out->str = k.p->tok.value;
                return true;
            }
            return false;
        }
        if (is_math_fn(cv)) {
            if (!(types & (EMBI_T_LEN | EMBI_T_PCT | EMBI_T_NUM | EMBI_T_INT | EMBI_T_ALPHA |
                           EMBI_T_ANGLE | EMBI_T_TIME | EMBI_T_RES))) {
                return false;
            }
            return store_calc(a, types, cv, out);
        }
        if ((types & EMBI_T_COLOR) && is_fn(cv, "light-dark")) {
            return store_light_dark(a, cv, out);
        }
        if ((types & EMBI_T_COLOR) && is_color_fn(cv)) {
            embi_color c;
            if (embi_color_parse_cv(cv, &c)) {
                out->kind = EMBI_V_COLOR;
                out->color = c;
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ lists and whole values */

static bool parse_list(embi_arena *a, embi_prop_id id, const embi_prop_info *pi,
                       embi_css_slice s, embi_value *out)
{
    s = trim(s);
    if (s.n == 0) {
        return false;
    }
    uint32_t count = 1;
    for (uint32_t i = 0; i < s.n; i++) {
        if (s.p[i].tok.type == EMBI_CSS_COMMA) {
            count++;
        }
    }
    embi_value *items = EMBI_NEW_ARRAY(a, embi_value, count);
    uint32_t idx = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.n; i++) {
        if (i == s.n || s.p[i].tok.type == EMBI_CSS_COMMA) {
            embi_css_slice part = trim((embi_css_slice){s.p + start, i - start});
            if (part.n == 0 || idx >= count) {
                return false;
            }
            if (count > 1 || id == EMBI_PROP_animation_name) {
                /* the CSS-wide keywords (and `default` in an animation-name) are not list items */
                const embi_css_cv *one = nullptr;
                if (single(part, &one) && one->tok.type == EMBI_CSS_IDENT) {
                    static const char *const wide[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default"};
                    for (uint32_t k = 0; k < 6; k++) {
                        if ((count > 1 || k == 5) && (k < 5 || id == EMBI_PROP_animation_name) &&
                            embi_str_ieq(one->tok.value, (embi_str){wide[k], (uint32_t)strlen(wide[k])})) {
                            return false;
                        }
                    }
                }
            }
            bool all_idents = true;
            for (uint32_t k = 0; k < part.n; k++) {
                if (part.p[k].tok.type != EMBI_CSS_IDENT && !cv_is_ws(&part.p[k])) {
                    all_idents = false;
                }
            }
            if (id == EMBI_PROP_font_family && part.n > 1 && all_idents) {
                /* Unquoted family names: idents joined by single spaces form one name. */
                embi_sb sb = {0};
                for (uint32_t k = 0; k < part.n; k++) {
                    if (part.p[k].tok.type == EMBI_CSS_IDENT) {
                        if (sb.len > 0) {
                            embi_sb_append_char(&sb, ' ');
                        }
                        embi_sb_append(&sb, part.p[k].tok.value);
                    }
                }
                items[idx].kind = EMBI_V_IDENT;
                items[idx].ident = embi_atom_intern(sb.data, sb.len);
                embi_sb_free(&sb);
            } else {
                const embi_css_cv *cv = nullptr;
                if (!single(part, &cv) || !parse_one_cv(a, pi, cv, &items[idx])) {
                    return false;
                }
                if (id == EMBI_PROP_animation_name && items[idx].kind == EMBI_V_STRING) {
                    /* a quoted animation name is the name itself; the empty string is not a name */
                    if (items[idx].str.len == 0) {
                        return false;
                    }
                    /* (a string that spells a reserved word stays a string, serialized with its quotes) */
                    static const char *const reserved[] = {"none", "initial", "inherit", "unset", "revert", "revert-layer", "default"};
                    bool res = false;
                    for (uint32_t k = 0; k < 7; k++) {
                        res = res || embi_str_ieq(items[idx].str, (embi_str){reserved[k], (uint32_t)strlen(reserved[k])});
                    }
                    if (!res) {
                        items[idx].kind = EMBI_V_IDENT;
                        items[idx].ident = embi_atom_intern(items[idx].str.ptr, items[idx].str.len);
                    }
                }
            }
            idx++;
            start = i + 1;
        }
    }
    out->kind = EMBI_V_LIST;
    out->list.items = items;
    out->list.n = idx;
    return true;
}

/* Opaque grammars: every component value must be well formed. Bad strings and urls are never
 * well formed, and a top-level unmatched closing token or semicolon ends a declaration value. */
static bool opaque_cv_ok(const embi_css_cv *cv, bool top)
{
    switch (cv->tok.type) {
    case EMBI_CSS_BAD_STRING:
    case EMBI_CSS_BAD_URL:
        return false;
    case EMBI_CSS_RPAREN:
    case EMBI_CSS_RBRACKET:
    case EMBI_CSS_RBRACE:
    case EMBI_CSS_SEMICOLON:
        if (top) {
            return false;
        }
        break;
    default:
        break;
    }
    for (uint32_t i = 0; i < cv->nkids; i++) {
        if (!opaque_cv_ok(&cv->kids[i], false)) {
            return false;
        }
    }
    return true;
}

bool embi_css_value_opaque_ok(embi_css_slice s)
{
    s = trim(s);
    if (s.n == 0) {
        return false;
    }
    for (uint32_t i = 0; i < s.n; i++) {
        if (!opaque_cv_ok(&s.p[i], true)) {
            return false;
        }
    }
    return true;
}

#define EMBI_X_ENTRY(n) [EMBI_G_##n] = embi_gram_##n,
static const embi_gram_fn gram_table[EMBI_G_COUNT] = {EMBI_GRAMMAR_LIST(EMBI_X_ENTRY)};
#undef EMBI_X_ENTRY

/* One grammar value (a whole value, or one comma separated item of a list property) over seg.
 * A lone keyword of the property is a keyword value; otherwise the grammar function runs and has to
 * consume the whole segment. */
static bool gram_item(embi_gctx *g, embi_cur *seg, embi_value *out)
{
    if (cur_left(seg) == 1 && embi_gram_kw(g->pi, cur_peek(seg), out)) {
        seg->i++;
        return true;
    }
    embi_gram_fn fn = gram_table[g->pi->grammar];
    if (!fn || cur_left(seg) == 0) {
        return false;
    }
    *out = (embi_value){0};
    return fn(g, seg, out) && cur_done(seg);
}

static bool run_grammar(embi_arena *a, embi_prop_id id, const embi_prop_info *pi, embi_css_slice s,
                        embi_value *out)
{
    embi_gctx g = {a, id, pi};
    embi_cur cur;
    embi_cur_make_slice(a, s, &cur);
    if (!(pi->types & EMBI_T_LIST)) {
        return gram_item(&g, &cur, out);
    }
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, &cur, &n);
    embi_value *items = EMBI_NEW_ARRAY(a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        if (!gram_item(&g, &segs[i], &items[i])) {
            return false;
        }
    }
    return embi_make_list(a, items, n, EMBI_SEP_COMMA, out);
}

/* Parses a non-CSS-wide, non-var value of a longhand: through its grammar function when it has
 * one, otherwise as a typed scalar, keyword or comma list. */
static bool parse_value(embi_arena *a, embi_prop_id id, const embi_prop_info *pi,
                        embi_css_slice s, embi_value *out)
{
    *out = (embi_value){0};
    s = trim(s);
    if (s.n == 0) {
        return false;
    }
    if (pi->grammar) {
        return run_grammar(a, id, pi, s, out);
    }
    const embi_css_cv *cv = nullptr;
    if (id == EMBI_PROP_display) {
        const embi_css_cv *one = nullptr;
        uint16_t dk = 0;
        bool known = single(s, &one) && one->tok.type == EMBI_CSS_IDENT && embi_kw_lookup(one->tok.value) != EMBI_KW_INVALID &&
                     prop_has_kw(pi, embi_kw_lookup(one->tok.value));
        if (!known && embi_css_parse_display_multi(s, &dk)) {
            out->kind = EMBI_V_KEYWORD;
            out->kw = dk;
            return true;
        }
    }
    if (pi->types & EMBI_T_LIST) {
        if (single(s, &cv) && cv->tok.type == EMBI_CSS_IDENT) {
            uint16_t kw = embi_kw_lookup(cv->tok.value);
            if (kw != EMBI_KW_INVALID && prop_has_kw(pi, kw)) {
                out->kind = EMBI_V_KEYWORD;
                out->kw = kw;
                return true;
            }
        }
        return parse_list(a, id, pi, s, out);
    }
    return single(s, &cv) && parse_one_cv(a, pi, cv, out);
}

bool embi_value_parse_longhand(embi_arena *a, embi_prop_id prop, embi_css_slice s, embi_value *out)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || (pi->flags & EMBI_PF_SHORTHAND)) {
        return false;
    }
    *out = (embi_value){0};
    return parse_value(a, prop, pi, s, out);
}

bool embi_parse_prefix(embi_arena *a, embi_prop_id prop, embi_cur *c, embi_value *out)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || (pi->flags & EMBI_PF_SHORTHAND) || cur_done(c)) {
        return false;
    }
    *out = (embi_value){0};
    if (pi->grammar) {
        embi_gctx g = {a, prop, pi};
        embi_cur save = *c;
        embi_gram_fn fn = gram_table[pi->grammar];
        if (fn && fn(&g, c, out)) {
            return true;
        }
        *c = save;
        *out = (embi_value){0};
        if (embi_gram_kw(pi, cur_peek(c), out)) {
            c->i++;
            return true;
        }
        return false;
    }
    if (parse_one_cv(a, pi, cur_peek(c), out)) {
        c->i++;
        return true;
    }
    return false;
}

bool embi_value_parse_one(embi_arena *a, embi_prop_id prop, const embi_css_cv *cv,
                          embi_value *out)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || (pi->flags & EMBI_PF_SHORTHAND) || !cv) {
        return false;
    }
    return parse_one_cv(a, pi, cv, out);
}

bool embi_value_parse_types(embi_arena *a, uint32_t types, const embi_css_cv *cv,
                            embi_value *out)
{
    embi_prop_info pi = {0};
    if (!cv) {
        return false;
    }
    pi.types = types;
    return parse_one_cv(a, &pi, cv, out);
}

bool embi_value_initial(embi_arena *a, embi_prop_id prop, embi_value *out)
{
    const embi_prop_info *pi = embi_prop_info_of(prop);
    if (!pi || !pi->initial) {
        return false;
    }
    embi_css_slice s = embi_css_parse_cvs(a, (embi_str){pi->initial, (uint32_t)strlen(pi->initial)});
    return parse_value(a, prop, pi, s, out);
}

/* The value of the `all` shorthand: only a CSS-wide keyword is valid. Returns its EMBI_V_* kind, or 0. */
int embi_css_all_keyword(embi_css_slice value)
{
    const embi_css_cv *cv = nullptr;
    embi_css_slice s = trim(value);
    if (single(s, &cv) && cv->tok.type == EMBI_CSS_IDENT) {
        return wide_kind(cv->tok.value);
    }
    return 0;
}

uint32_t embi_css_parse_decl(embi_arena *a, embi_prop_id id, embi_css_slice value,
                             embi_longhand *out, uint32_t cap)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    if (!pi) {
        return 0;
    }
    embi_css_slice s = trim(value);
    if (s.n == 0) {
        return 0;
    }
    bool shorthand = (pi->flags & EMBI_PF_SHORTHAND) != 0;
    uint32_t need = shorthand ? pi->sub_n : 1;
    if (cap < need) {
        return 0;
    }

    /* CSS-wide keywords and var() apply to every longhand alike. */
    const embi_css_cv *cv = nullptr;
    int wk = 0;
    if (single(s, &cv) && cv->tok.type == EMBI_CSS_IDENT) {
        wk = wide_kind(cv->tok.value);
    }
    if (wk != 0 || embi_css_has_var(s) || (id != EMBI_PROP_content && embi_css_has_attr(s))) {
        embi_value v = {0};
        v.kind = (uint8_t)(wk != 0 ? wk : EMBI_V_VAR);
        if (wk == 0) {
            v.raw = s;
        }
        if (!shorthand) {
            out[0].prop = id;
            out[0].value = v;
            return 1;
        }
        for (uint32_t i = 0; i < need; i++) {
            out[i].prop = embi_prop_sub_list[pi->sub_off + i];
            out[i].value = v;
        }
        return need;
    }

    if (shorthand) {
        return embi_css_expand_shorthand(a, id, s, out, cap);
    }
    embi_value v = {0};
    if (!parse_value(a, id, pi, s, &v)) {
        return 0;
    }
    out[0].prop = id;
    out[0].value = v;
    return 1;
}

bool embi_prop_decl_ok(void *user, embi_str name, embi_css_slice value, bool important)
{
    (void)user;
    (void)important;
    if (name.len >= 2 && name.ptr[0] == '-' && name.ptr[1] == '-') {
        return true;
    }
    embi_prop_id id = embi_prop_lookup(name);
    if (id == EMBI_PROP_NONE) {
        return embi_str_ieq(name, EMBI_STR("all")) && embi_css_all_keyword(value) != 0;
    }
    embi_arena a = {0};
    embi_arena_init(&a, 512);
    embi_longhand out[EMBI_LONGHAND_MAX];
    bool ok = embi_css_parse_decl(&a, id, value, out, EMBI_LONGHAND_MAX) > 0;
    embi_arena_free(&a);
    return ok;
}

/* ------------------------------------------------------------------ serialization */

static void serialize_list(embi_sb *sb, const embi_value *v)
{
    for (uint32_t i = 0; i < v->list.n; i++) {
        if (i > 0) {
            if (v->sep == EMBI_SEP_SPACE) {
                bool alt = v->aux != 0 && i == v->list.n - v->aux;
                embi_sb_append(sb, alt ? EMBI_STR(" / ") : EMBI_STR(" "));
            } else {
                embi_sb_append(sb, EMBI_STR(", "));
            }
        }
        embi_value_serialize(sb, &v->list.items[i]);
    }
}

static void serialize_cvs(embi_sb *sb, embi_css_slice s);
#define serialize_cv embi_css_serialize_cv

void embi_css_serialize_cv(embi_sb *sb, const embi_css_cv *cv)
{
    const embi_css_token *t = &cv->tok;
    switch (t->type) {
    case EMBI_CSS_WS:
        embi_sb_append_char(sb, ' ');
        break;
    case EMBI_CSS_IDENT:
        embi_sb_append(sb, t->value);
        break;
    case EMBI_CSS_FUNCTION:
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, '(');
        serialize_cvs(sb, (embi_css_slice){cv->kids, cv->nkids});
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_CSS_AT_KEYWORD:
        embi_sb_append_char(sb, '@');
        embi_sb_append(sb, t->value);
        break;
    case EMBI_CSS_HASH:
        embi_sb_append_char(sb, '#');
        embi_sb_append(sb, t->value);
        break;
    case EMBI_CSS_STRING:
        embi_sb_append_char(sb, '"');
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, '"');
        break;
    case EMBI_CSS_URL:
        embi_sb_append(sb, EMBI_STR("url("));
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_CSS_NUMBER:
        embi_css_append_num(sb, t->num);
        break;
    case EMBI_CSS_PERCENTAGE:
        embi_css_append_num(sb, t->num);
        embi_sb_append_char(sb, '%');
        break;
    case EMBI_CSS_DIMENSION:
        embi_css_append_num(sb, t->num);
        embi_sb_append(sb, t->value);
        break;
    case EMBI_CSS_DELIM:
        if (t->cp < 0x80) {
            embi_sb_append_char(sb, (char)t->cp);
        }
        break;
    case EMBI_CSS_COMMA:
        embi_sb_append_char(sb, ',');
        break;
    case EMBI_CSS_COLON:
        embi_sb_append_char(sb, ':');
        break;
    case EMBI_CSS_LPAREN:
        embi_sb_append_char(sb, '(');
        serialize_cvs(sb, (embi_css_slice){cv->kids, cv->nkids});
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_CSS_LBRACKET:
        embi_sb_append_char(sb, '[');
        serialize_cvs(sb, (embi_css_slice){cv->kids, cv->nkids});
        embi_sb_append_char(sb, ']');
        break;
    case EMBI_CSS_LBRACE:
        embi_sb_append_char(sb, '{');
        serialize_cvs(sb, (embi_css_slice){cv->kids, cv->nkids});
        embi_sb_append_char(sb, '}');
        break;
    default:
        break;
    }
}

static void serialize_cvs(embi_sb *sb, embi_css_slice s)
{
    for (uint32_t i = 0; i < s.n; i++) {
        serialize_cv(sb, &s.p[i]);
    }
}

static bool str_identical(embi_str a, embi_str b)
{
    return a.len == b.len && (a.len == 0 || a.ptr == b.ptr || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static bool num_identical(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

bool embi_value_identical(const embi_value *x, const embi_value *y)
{
    if (x == y) {
        return true;
    }
    if (x == nullptr || y == nullptr || x->kind != y->kind || x->unit != y->unit || x->sep != y->sep ||
        x->aux != y->aux || x->kw != y->kw) {
        return false;
    }
    switch (x->kind) {
    case EMBI_V_NONE:
    case EMBI_V_KEYWORD:
    case EMBI_V_INITIAL:
    case EMBI_V_INHERIT:
    case EMBI_V_UNSET:
    case EMBI_V_REVERT:
    case EMBI_V_REVERT_LAYER:
        return true;
    case EMBI_V_NUMBER:
    case EMBI_V_INTEGER:
    case EMBI_V_PERCENT:
    case EMBI_V_LENGTH:
    case EMBI_V_ANGLE:
    case EMBI_V_TIME:
    case EMBI_V_RESOLUTION:
        return num_identical(x->num, y->num);
    case EMBI_V_COLOR: {
        const embi_color *a = &x->color, *b = &y->color;
        return a->space == b->space && a->flags == b->flags && a->sys == b->sys && num_identical(a->c[0], b->c[0]) &&
               num_identical(a->c[1], b->c[1]) && num_identical(a->c[2], b->c[2]) &&
               num_identical(a->alpha_k, b->alpha_k) && num_identical(a->alpha, b->alpha);
    }
    case EMBI_V_STRING:
    case EMBI_V_URL:
        return str_identical(x->str, y->str);
    case EMBI_V_IDENT:
        return x->ident == y->ident;
    case EMBI_V_LIST:
        if (x->list.n != y->list.n) {
            return false;
        }
        for (uint32_t i = 0; i < x->list.n; i++) {
            if (!embi_value_identical(&x->list.items[i], &y->list.items[i])) {
                return false;
            }
        }
        return true;
    case EMBI_V_IMAGE: {
        const embi_image *a = x->image, *b = y->image;
        if (a == b) {
            return true;
        }
        return a != nullptr && b != nullptr && a->kind == EMBI_IMG_URL && b->kind == EMBI_IMG_URL && a->grad == b->grad &&
               a->opts == b->opts && a->n == b->n && a->xf == b->xf && a->fb == b->fb && str_identical(a->url, b->url);
    }
    case EMBI_V_CALC:
        return x->calc == y->calc;
    default:
        return false; /* the other compound values: their serializations tell */
    }
}

/* Numbers serialize with six decimals (embi_css_append_num), so two numbers whose serializations are equal can differ in the
 * double. A difference above this bound shows in the six decimals however the two round, so the strings differ too. */
#define EMBI_SER_CLEAR 1e-5

static bool num_clearly_differs(double a, double b)
{
    double d = a - b;
    return d == d && (d > EMBI_SER_CLEAR || d < -EMBI_SER_CLEAR); /* NaN, and an infinity minus itself: not decided */
}

bool embi_value_clearly_differs(const embi_value *x, const embi_value *y)
{
    if (x == y || x == nullptr || y == nullptr || x->kind != y->kind) {
        return false;
    }
    switch (x->kind) {
    case EMBI_V_NUMBER:
    case EMBI_V_INTEGER:
    case EMBI_V_PERCENT:
    case EMBI_V_LENGTH:
    case EMBI_V_ANGLE:
    case EMBI_V_TIME:
    case EMBI_V_RESOLUTION:
        return x->unit == y->unit && num_clearly_differs(x->num, y->num);
    case EMBI_V_COLOR: {
        const embi_color *a = &x->color, *b = &y->color;
        if (a->space != b->space || a->flags != b->flags || a->sys != b->sys || a->space == EMBI_CS_CURRENT ||
            a->space == EMBI_CS_SYSTEM || a->space == EMBI_CS_DEFERRED || a->space == EMBI_CS_DEVICE_CMYK) {
            return false;
        }
        double k = a->space == EMBI_CS_SRGB && !(a->flags & EMBI_CF_FUNC) ? 255.0 : 1.0; /* the rgb() form prints c * 255 */
        return num_clearly_differs(a->c[0] * k, b->c[0] * k) || num_clearly_differs(a->c[1] * k, b->c[1] * k) ||
               num_clearly_differs(a->c[2] * k, b->c[2] * k) || num_clearly_differs(a->alpha, b->alpha);
    }
    case EMBI_V_TRANSFORM:
        if (x->xf.n != y->xf.n) {
            return true; /* a different number of functions */
        }
        for (uint32_t i = 0; i < x->xf.n; i++) {
            const embi_xform *p = &x->xf.p[i], *q = &y->xf.p[i];
            if (p->fn != q->fn || p->n != q->n) {
                return true;
            }
            for (uint32_t k = 0; k < p->n; k++) {
                if (embi_value_clearly_differs(&p->args[k], &q->args[k])) {
                    return true;
                }
            }
        }
        return false;
    default:
        return false;
    }
}

void embi_value_serialize(embi_sb *sb, const embi_value *v)
{
    switch (v->kind) {
    case EMBI_V_RAW:
        serialize_cvs(sb, v->raw);
        break;
    case EMBI_V_KEYWORD: {
        const char *name = embi_css_display_kw_text(v->kw);
        if (name == nullptr) {
            name = embi_kw_name(v->kw);
        }
        if (name) {
            embi_sb_append(sb, (embi_str){name, (uint32_t)strlen(name)});
        }
        break;
    }
    case EMBI_V_NUMBER:
    case EMBI_V_INTEGER:
        embi_css_append_num(sb, v->num);
        break;
    case EMBI_V_PERCENT:
        embi_css_append_num(sb, v->num);
        embi_sb_append_char(sb, '%');
        break;
    case EMBI_V_LENGTH:
    case EMBI_V_ANGLE:
    case EMBI_V_TIME:
    case EMBI_V_RESOLUTION:
        embi_css_append_num(sb, v->num);
        embi_sb_append(sb, (embi_str){embi_unit_name(v->unit), (uint32_t)strlen(embi_unit_name(v->unit))});
        break;
    case EMBI_V_CALC:
        embi_calc_serialize(sb, v->calc, false);
        break;
    case EMBI_V_COLOR:
        embi_color_serialize(sb, &v->color);
        break;
    case EMBI_V_STRING:
        embi_sb_append_char(sb, '"');
        embi_sb_append(sb, v->str);
        embi_sb_append_char(sb, '"');
        break;
    case EMBI_V_URL:
        embi_sb_append(sb, EMBI_STR("url("));
        embi_sb_append(sb, v->str);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_V_IDENT: {
        embi_str s = embi_atom_str(v->ident);
        embi_sb_append(sb, s);
        break;
    }
    case EMBI_V_LIST:
        serialize_list(sb, v);
        break;
    case EMBI_V_LIGHT_DARK:
        embi_sb_append(sb, EMBI_STR("light-dark("));
        embi_value_serialize(sb, &v->pair[0]);
        embi_sb_append(sb, EMBI_STR(", "));
        embi_value_serialize(sb, &v->pair[1]);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_V_VAR:
        embi_sb_append(sb, EMBI_STR("var()"));
        break;
    case EMBI_V_INITIAL:
        embi_sb_append(sb, EMBI_STR("initial"));
        break;
    case EMBI_V_INHERIT:
        embi_sb_append(sb, EMBI_STR("inherit"));
        break;
    case EMBI_V_UNSET:
        embi_sb_append(sb, EMBI_STR("unset"));
        break;
    case EMBI_V_REVERT:
        embi_sb_append(sb, EMBI_STR("revert"));
        break;
    case EMBI_V_REVERT_LAYER:
        embi_sb_append(sb, EMBI_STR("revert-layer"));
        break;
    default:
        embi_serialize_compound(sb, v);
        break;
    }
}

/* ------------------------------------------------------------------ logical properties */

/* Physical side indices: top 0, right 1, bottom 2, left 3. */
typedef struct flow_sides {
    uint8_t bs, be, is, ie;
    bool vertical; /* the inline axis is vertical */
} flow_sides;

static flow_sides flow_of(uint16_t wm, uint16_t dir)
{
    bool rtl = dir == EMBI_KW_rtl;
    flow_sides f;
    if (wm == EMBI_KW_vertical_rl || wm == EMBI_KW_sideways_rl) {
        f = (flow_sides){1, 3, rtl ? 2 : 0, rtl ? 0 : 2, true};
    } else if (wm == EMBI_KW_vertical_lr) {
        f = (flow_sides){3, 1, rtl ? 2 : 0, rtl ? 0 : 2, true};
    } else if (wm == EMBI_KW_sideways_lr) {
        f = (flow_sides){3, 1, rtl ? 0 : 2, rtl ? 2 : 0, true};
    } else {
        f = (flow_sides){0, 2, rtl ? 1 : 3, rtl ? 3 : 1, false};
    }
    return f;
}

/* The physical role index (within the family) a logical role stands for. */
static uint8_t logical_to_phys_role(uint8_t kind, uint8_t role, flow_sides f)
{
    if (kind == 0) {
        switch (role) {
        case 0: return f.bs;
        case 1: return f.be;
        case 2: return f.is;
        default: return f.ie;
        }
    }
    if (kind == 1) {
        /* size: role 0 inline-size, 1 block-size; physical 0 width, 1 height */
        bool inline_is_height = f.vertical;
        return (uint8_t)((role == 0) == inline_is_height ? 1 : 0);
    }
    uint8_t b = role < 2 ? f.bs : f.be;
    uint8_t i = (role & 1) ? f.ie : f.is;
    uint8_t v = (b % 2 == 0) ? b : i;
    uint8_t h = (b % 2 == 1) ? b : i;
    if (v == 0) {
        return h == 3 ? 0 : 1;
    }
    return h == 1 ? 2 : 3;
}

bool embi_prop_is_logical(embi_prop_id id)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    return pi && pi->logic_family != EMBI_LF_NONE && pi->logic_logical;
}

embi_prop_id embi_prop_to_physical(embi_prop_id id, uint16_t writing_mode, uint16_t direction)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    if (!pi || pi->logic_family == EMBI_LF_NONE || !pi->logic_logical) {
        return id;
    }
    flow_sides f = flow_of(writing_mode, direction);
    uint8_t pr = logical_to_phys_role(embi_logic_kind[pi->logic_family], pi->logic_role, f);
    uint16_t phys = embi_logic_phys[pi->logic_family][pr];
    return phys ? phys : id;
}

embi_prop_id embi_prop_to_logical(embi_prop_id id, uint16_t writing_mode, uint16_t direction)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    if (!pi || pi->logic_family == EMBI_LF_NONE || pi->logic_logical) {
        return EMBI_PROP_NONE;
    }
    flow_sides f = flow_of(writing_mode, direction);
    uint8_t kind = embi_logic_kind[pi->logic_family];
    uint8_t nroles = kind == 1 ? 2 : 4;
    for (uint8_t r = 0; r < nroles; r++) {
        if (logical_to_phys_role(kind, r, f) == pi->logic_role) {
            return embi_logic_log[pi->logic_family][r];
        }
    }
    return EMBI_PROP_NONE;
}

/* ------------------------------------------------------------------ deep copy (WPTASAN) */

/* The deep check of the poison mode of the tests (embi_value_debug_poisoned) runs the copy with a probe: every part is
 * checked before it is read, and a poisoned one is counted and not followed. */
static thread_local uint32_t *g_probe;

static bool bad(const void *p, size_t n)
{
    if (g_probe == nullptr || p == nullptr) {
        return false;
    }
    if (embi_arena_debug_poisoned(p, n < 8 ? n : 8)) {
        (*g_probe)++;
        return true;
    }
    return false;
}

static void *copy_bytes(embi_arena *a, const void *src, size_t n, size_t align)
{
    if (src == nullptr || n == 0 || bad(src, n)) {
        return nullptr;
    }
    void *p = embi_arena_alloc(a, n, align);
    memcpy(p, src, n);
    return p;
}

static embi_str copy_str(embi_arena *a, embi_str s)
{
    if (s.ptr == nullptr || s.len == 0) {
        return s;
    }
    return (embi_str){copy_bytes(a, s.ptr, s.len, 1), s.len};
}

static const embi_css_cv *copy_cvs(embi_arena *a, const embi_css_cv *src, uint32_t n)
{
    if (src == nullptr || n == 0 || bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_css_cv *d = embi_arena_alloc(a, sizeof *d * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        embi_css_cv c = src[i];
        c.tok.value = copy_str(a, c.tok.value);
        c.tok.repr = copy_str(a, c.tok.repr);
        c.kids = copy_cvs(a, src[i].kids, src[i].nkids);
        d[i] = c;
    }
    return d;
}

static const embi_calc *copy_calc(embi_arena *a, const embi_calc *c)
{
    if (c == nullptr || bad(c, sizeof *c)) {
        return nullptr;
    }
    embi_calc *d = embi_arena_alloc(a, sizeof *d, 8);
    *d = *c;
    d->kids = nullptr;
    if (c->n > 0 && c->kids != nullptr && !bad(c->kids, sizeof *c->kids)) {
        const embi_calc **kids = embi_arena_alloc(a, sizeof *kids * c->n, 8);
        for (uint32_t i = 0; i < c->n; i++) {
            kids[i] = copy_calc(a, c->kids[i]);
        }
        d->kids = kids;
    }
    return d;
}

/* The n values at src copied into a new array (nullptr for none). */
static const embi_value *copy_values(embi_arena *a, const embi_value *src, uint32_t n)
{
    if (src == nullptr || n == 0 || bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_value *d = embi_arena_alloc(a, sizeof *d * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        embi_value_copy(a, &src[i], &d[i]);
    }
    return d;
}

/* In place: the parts of a struct member that was copied by value. */
static void own_value(embi_arena *a, embi_value *v)
{
    embi_value_copy(a, v, v);
}

static void own_axis(embi_arena *a, embi_pos_axis *x)
{
    own_value(a, &x->off);
}

static void own_position(embi_arena *a, embi_position *p)
{
    own_axis(a, &p->x);
    own_axis(a, &p->y);
    own_value(a, &p->z);
}

static const embi_shadow *copy_shadows(embi_arena *a, const embi_shadow *src, uint32_t n)
{
    if (src == nullptr || n == 0 || bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_shadow *d = embi_arena_alloc(a, sizeof *d * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        d[i] = src[i];
        own_value(a, &d[i].color);
        own_value(a, &d[i].x);
        own_value(a, &d[i].y);
        own_value(a, &d[i].blur);
        own_value(a, &d[i].spread);
    }
    return d;
}

static const embi_image *copy_image(embi_arena *a, const embi_image *src)
{
    if (src == nullptr || bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_image *d = embi_arena_alloc(a, sizeof *d, 8);
    *d = *src;
    d->url = copy_str(a, src->url);
    if (src->grad != nullptr && !bad(src->grad, sizeof *src->grad)) {
        embi_gradient *g = embi_arena_alloc(a, sizeof *g, 8);
        *g = *src->grad;
        own_value(a, &g->angle);
        own_value(a, &g->radius[0]);
        own_value(a, &g->radius[1]);
        own_position(a, &g->pos);
        g->stops = nullptr;
        if (src->grad->stops != nullptr && src->grad->nstops > 0 && !bad(src->grad->stops, sizeof *src->grad->stops)) {
            embi_color_stop *s = embi_arena_alloc(a, sizeof *s * src->grad->nstops, 8);
            for (uint32_t i = 0; i < src->grad->nstops; i++) {
                s[i] = src->grad->stops[i];
                own_value(a, &s[i].color);
                own_value(a, &s[i].pos[0]);
                own_value(a, &s[i].pos[1]);
            }
            g->stops = s;
        }
        d->grad = g;
    }
    if (src->opts != nullptr && src->n > 0 && !bad(src->opts, sizeof *src->opts)) {
        embi_image_opt *o = embi_arena_alloc(a, sizeof *o * src->n, 8);
        for (uint32_t i = 0; i < src->n; i++) {
            o[i] = src->opts[i];
            o[i].img = copy_image(a, src->opts[i].img);
            o[i].type = copy_str(a, src->opts[i].type);
        }
        d->opts = o;
    }
    if (src->xf != nullptr && src->n > 0 && !bad(src->xf, sizeof *src->xf)) {
        embi_xfade_item *x = embi_arena_alloc(a, sizeof *x * src->n, 8);
        for (uint32_t i = 0; i < src->n; i++) {
            x[i] = src->xf[i];
            x[i].img = copy_image(a, src->xf[i].img);
            own_value(a, &x[i].color);
        }
        d->xf = x;
    }
    d->fb = copy_image(a, src->fb);
    return d;
}

static const embi_grid_item *copy_grid_items(embi_arena *a, const embi_grid_item *src, uint32_t n)
{
    if (src == nullptr || n == 0 || bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_grid_item *d = embi_arena_alloc(a, sizeof *d * n, 8);
    for (uint32_t i = 0; i < n; i++) {
        d[i] = src[i];
        if (src[i].kind == EMBI_GI_NAMES) {
            d[i].names = copy_bytes(a, src[i].names, sizeof(embi_atom) * src[i].n, alignof(embi_atom));
        } else if (src[i].kind == EMBI_GI_TRACK) {
            own_value(a, &d[i].size.min.v);
            own_value(a, &d[i].size.max.v);
        } else if (src[i].kind == EMBI_GI_REPEAT) {
            d[i].items = copy_grid_items(a, src[i].items, src[i].n);
        }
    }
    return d;
}

static const embi_shape *copy_shape(embi_arena *a, const embi_shape *src)
{
    if (bad(src, sizeof *src)) {
        return nullptr;
    }
    embi_shape *s = embi_arena_alloc(a, sizeof *s, 8);
    *s = *src;
    for (uint32_t i = 0; i < 4; i++) {
        own_value(a, &s->box[i]);
    }
    for (uint32_t i = 0; i < 8; i++) {
        own_value(a, &s->radii[i]);
    }
    own_value(a, &s->r[0]);
    own_value(a, &s->r[1]);
    own_position(a, &s->at);
    s->pts = copy_values(a, src->pts, src->npts * 2u);
    s->path = copy_str(a, src->path);
    own_position(a, &s->from);
    s->cmds = nullptr;
    if (src->cmds != nullptr && src->ncmds > 0 && !bad(src->cmds, sizeof *src->cmds)) {
        embi_shape_cmd *c = embi_arena_alloc(a, sizeof *c * src->ncmds, 8);
        for (uint32_t i = 0; i < src->ncmds; i++) {
            c[i] = src->cmds[i];
            for (uint32_t j = 0; j < 2; j++) {
                own_axis(a, &c[i].p[j]);
                own_axis(a, &c[i].c[j][0]);
                own_axis(a, &c[i].c[j][1]);
                own_value(a, &c[i].r[j]);
            }
        }
        s->cmds = c;
    }
    return s;
}

void embi_value_copy(embi_arena *a, const embi_value *src, embi_value *dst)
{
    if (bad(src, sizeof *src)) {
        *dst = (embi_value){0};
        return;
    }
    embi_value v = *src;
    switch (v.kind) {
    case EMBI_V_CALC:
        v.calc = copy_calc(a, src->calc);
        break;
    case EMBI_V_STRING:
    case EMBI_V_URL:
        v.str = copy_str(a, src->str);
        break;
    case EMBI_V_LIST:
        v.list.items = copy_values(a, src->list.items, src->list.n);
        break;
    case EMBI_V_LIGHT_DARK:
        v.pair = copy_values(a, src->pair, src->pair != nullptr ? 2u : 0u);
        break;
    case EMBI_V_RAW:
    case EMBI_V_VAR:
        v.raw.p = copy_cvs(a, src->raw.p, src->raw.n);
        break;
    case EMBI_V_FUNC:
        v.func.args = copy_values(a, src->func.args, src->func.n);
        break;
    case EMBI_V_IMAGE:
        v.image = copy_image(a, src->image);
        break;
    case EMBI_V_POSITION:
        if (src->pos != nullptr && !bad(src->pos, sizeof *src->pos)) {
            embi_position *p = embi_arena_alloc(a, sizeof *p, 8);
            *p = *src->pos;
            own_position(a, p);
            v.pos = p;
        }
        break;
    case EMBI_V_POS_AXIS:
        if (src->axis != nullptr && !bad(src->axis, sizeof *src->axis)) {
            embi_pos_axis *x = embi_arena_alloc(a, sizeof *x, 8);
            *x = *src->axis;
            own_axis(a, x);
            v.axis = x;
        }
        break;
    case EMBI_V_TRANSFORM:
        if (src->xf.p != nullptr && src->xf.n > 0 && !bad(src->xf.p, sizeof *src->xf.p)) {
            embi_xform *x = embi_arena_alloc(a, sizeof *x * src->xf.n, 8);
            for (uint32_t i = 0; i < src->xf.n; i++) {
                x[i] = src->xf.p[i];
                x[i].args = copy_values(a, src->xf.p[i].args, src->xf.p[i].n);
            }
            v.xf.p = x;
        }
        break;
    case EMBI_V_SHADOWS:
        v.shadows.p = copy_shadows(a, src->shadows.p, src->shadows.n);
        break;
    case EMBI_V_FILTERS:
        if (src->filters.p != nullptr && src->filters.n > 0 && !bad(src->filters.p, sizeof *src->filters.p)) {
            embi_filter *f = embi_arena_alloc(a, sizeof *f * src->filters.n, 8);
            for (uint32_t i = 0; i < src->filters.n; i++) {
                f[i] = src->filters.p[i];
                own_value(a, &f[i].arg);
                f[i].shadow = copy_shadows(a, src->filters.p[i].shadow, src->filters.p[i].shadow != nullptr ? 1u : 0u);
            }
            v.filters.p = f;
        }
        break;
    case EMBI_V_SHAPE:
        if (src->shape != nullptr) {
            v.shape = copy_shape(a, src->shape);
        }
        break;
    case EMBI_V_EASING:
        if (src->easing != nullptr && !bad(src->easing, sizeof *src->easing)) {
            embi_easing *e = embi_arena_alloc(a, sizeof *e, 8);
            *e = *src->easing;
            e->pts = copy_bytes(a, src->easing->pts, sizeof(embi_easing_pt) * src->easing->npts, 8);
            v.easing = e;
        }
        break;
    case EMBI_V_TRACKS:
        if (src->tracks != nullptr && !bad(src->tracks, sizeof *src->tracks)) {
            embi_grid_tracks *t = embi_arena_alloc(a, sizeof *t, 8);
            *t = *src->tracks;
            t->items = copy_grid_items(a, src->tracks->items, src->tracks->n);
            v.tracks = t;
        }
        break;
    case EMBI_V_GRID_LINE:
        if (src->gline != nullptr) {
            v.gline = copy_bytes(a, src->gline, sizeof *src->gline, 8);
        }
        break;
    default:
        break; /* numbers, keywords, colors, idents, ratios: no parts elsewhere */
    }
    *dst = v;
}

uint32_t embi_value_debug_poisoned(const embi_value *v)
{
    if (v == nullptr) {
        return 0;
    }
    uint32_t n = 0;
    embi_arena scratch;
    embi_arena_init(&scratch, 1024);
    uint32_t *outer = g_probe;
    g_probe = &n;
    embi_value tmp;
    embi_value_copy(&scratch, v, &tmp);
    g_probe = outer;
    embi_arena_debug_free_now(&scratch);
    return n;
}
