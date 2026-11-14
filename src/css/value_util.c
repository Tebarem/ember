/*
 * Shared helpers for the compound value grammars (W2c): cursors over component values, small value
 * constructors, <position>, border-radius sets and angles.
 */
#include "css/value_int.h"

/* ------------------------------------------------------------------ memory and strings */

void *embi_anew(embi_arena *a, size_t size, size_t align)
{
    void *p = embi_arena_alloc(a, size == 0 ? 1 : size, align);
    memset(p, 0, size);
    return p;
}

bool embi_str_is(embi_str s, const char *lit)
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

uint16_t cv_kw(const embi_css_cv *cv)
{
    if (!cv || cv->tok.type != EMBI_CSS_IDENT) {
        return EMBI_KW_INVALID;
    }
    return embi_kw_lookup(cv->tok.value);
}

bool embi_ident_reserved(embi_str s)
{
    return embi_str_is(s, "initial") || embi_str_is(s, "inherit") || embi_str_is(s, "unset") ||
           embi_str_is(s, "revert") || embi_str_is(s, "revert-layer") || embi_str_is(s, "default");
}

bool embi_make_list(embi_arena *a, const embi_value *src, uint32_t n, uint8_t sep, embi_value *out)
{
    embi_value *items = EMBI_NEW_ARRAY(a, embi_value, n);
    if (n > 0) {
        memcpy(items, src, sizeof(embi_value) * n);
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_LIST;
    out->sep = sep;
    out->list.items = items;
    out->list.n = n;
    return true;
}

bool embi_make_list_kw(embi_arena *a, const embi_value *src, uint32_t n, uint8_t sep,
                       embi_value *out)
{
    if (n == 1 && src[0].kind == EMBI_V_KEYWORD) {
        *out = src[0];
        return true;
    }
    return embi_make_list(a, src, n, sep, out);
}

/* ------------------------------------------------------------------ cursors */

bool embi_cur_make(embi_arena *a, const embi_css_cv *cvs, uint32_t n, embi_cur *out)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cvs[i].tok.type != EMBI_CSS_WS) {
            count++;
        }
    }
    const embi_css_cv **arr = EMBI_NEW_ARRAY(a, const embi_css_cv *, count);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cvs[i].tok.type != EMBI_CSS_WS) {
            arr[k++] = &cvs[i];
        }
    }
    out->c = arr;
    out->n = count;
    out->i = 0;
    return true;
}

embi_cur *embi_cur_split(embi_arena *a, const embi_cur *c, uint32_t *n)
{
    uint32_t count = 1;
    for (uint32_t i = c->i; i < c->n; i++) {
        if (cv_comma(c->c[i])) {
            count++;
        }
    }
    embi_cur *segs = EMBI_NEW_ARRAY(a, embi_cur, count);
    uint32_t k = 0;
    uint32_t start = c->i;
    for (uint32_t i = c->i; i <= c->n; i++) {
        if (i == c->n || cv_comma(c->c[i])) {
            segs[k].c = c->c + start;
            segs[k].n = i - start;
            segs[k].i = 0;
            k++;
            start = i + 1;
        }
    }
    *n = count;
    return segs;
}

/* ------------------------------------------------------------------ scalars */

bool embi_angle_or_zero(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    if (!cv) {
        return false;
    }
    if (cv->tok.type == EMBI_CSS_NUMBER && cv->tok.num == 0) {
        out->kind = EMBI_V_ANGLE;
        out->unit = EMBI_U_DEG;
        out->num = 0;
        return true;
    }
    return embi_value_parse_types(a, EMBI_T_ANGLE, cv, out);
}

/* ------------------------------------------------------------------ <position> */

typedef enum { PK_NONE, PK_LEFT, PK_RIGHT, PK_TOP, PK_BOTTOM, PK_CENTER, PK_LP } pk_t;

static pk_t classify(embi_arena *a, const embi_css_cv *cv, embi_value *lp)
{
    if (cv->tok.type == EMBI_CSS_IDENT) {
        embi_str s = cv->tok.value;
        if (embi_str_is(s, "left")) {
            return PK_LEFT;
        }
        if (embi_str_is(s, "right")) {
            return PK_RIGHT;
        }
        if (embi_str_is(s, "top")) {
            return PK_TOP;
        }
        if (embi_str_is(s, "bottom")) {
            return PK_BOTTOM;
        }
        if (embi_str_is(s, "center")) {
            return PK_CENTER;
        }
        return PK_NONE;
    }
    if (embi_value_parse_types(a, EMBI_LP, cv, lp)) {
        return PK_LP;
    }
    return PK_NONE;
}

static embi_pos_axis axis_kw(pk_t k, bool has_off, const embi_value *off)
{
    embi_pos_axis ax = {0};
    switch (k) {
    case PK_LEFT:
    case PK_TOP:
        ax.off = has_off ? *off : v_pct(0);
        break;
    case PK_RIGHT:
    case PK_BOTTOM:
        if (has_off) {
            ax.anchor = 1;
            ax.off = *off;
        } else {
            ax.off = v_pct(100);
        }
        break;
    case PK_CENTER:
        ax.off = v_pct(50);
        break;
    default:
        ax.off = *off;
        break;
    }
    return ax;
}

static bool horiz(pk_t k) { return k == PK_LEFT || k == PK_RIGHT; }
static bool vert(pk_t k) { return k == PK_TOP || k == PK_BOTTOM; }

typedef struct spec {
    pk_t kw;
    bool has_off;
    embi_value off;
} spec;

static bool take_spec(const pk_t *kind, const embi_value *v, uint32_t k, uint32_t *i, spec *s)
{
    if (*i >= k) {
        return false;
    }
    pk_t kw = kind[*i];
    if (kw == PK_NONE || kw == PK_LP) {
        return false;
    }
    s->kw = kw;
    s->has_off = false;
    (*i)++;
    if (kw != PK_CENTER && *i < k && kind[*i] == PK_LP) {
        s->has_off = true;
        s->off = v[*i];
        (*i)++;
    }
    return true;
}

static bool position_k(const pk_t *kind, const embi_value *v, uint32_t k, embi_position *out)
{
    embi_value none = {0};
    out->z = none;
    if (k == 1) {
        switch (kind[0]) {
        case PK_LEFT:
        case PK_RIGHT:
            out->x = axis_kw(kind[0], false, &none);
            out->y = axis_kw(PK_CENTER, false, &none);
            return true;
        case PK_TOP:
        case PK_BOTTOM:
            out->x = axis_kw(PK_CENTER, false, &none);
            out->y = axis_kw(kind[0], false, &none);
            return true;
        case PK_CENTER:
            out->x = axis_kw(PK_CENTER, false, &none);
            out->y = axis_kw(PK_CENTER, false, &none);
            return true;
        case PK_LP:
            out->x = axis_kw(PK_LP, false, &v[0]);
            out->y = axis_kw(PK_CENTER, false, &none);
            return true;
        default:
            return false;
        }
    }
    if (k == 2) {
        pk_t a = kind[0], b = kind[1];
        bool a_x = a == PK_LEFT || a == PK_RIGHT || a == PK_CENTER || a == PK_LP;
        bool b_y = b == PK_TOP || b == PK_BOTTOM || b == PK_CENTER || b == PK_LP;
        if (a_x && b_y) {
            out->x = axis_kw(a, false, &v[0]);
            out->y = axis_kw(b, false, &v[1]);
            return true;
        }
        bool a_y = a == PK_TOP || a == PK_BOTTOM || a == PK_CENTER;
        bool b_x = b == PK_LEFT || b == PK_RIGHT || b == PK_CENTER;
        if (a_y && b_x) {
            out->x = axis_kw(b, false, &v[1]);
            out->y = axis_kw(a, false, &v[0]);
            return true;
        }
        return false;
    }
    /* Three or four components: two keyword specs, each with an optional offset. */
    spec s1, s2;
    uint32_t i = 0;
    if (!take_spec(kind, v, k, &i, &s1) || !take_spec(kind, v, k, &i, &s2) || i != k) {
        return false;
    }
    spec *sx, *sy;
    if (horiz(s1.kw) && (vert(s2.kw) || s2.kw == PK_CENTER)) {
        sx = &s1;
        sy = &s2;
    } else if (vert(s1.kw) && (horiz(s2.kw) || s2.kw == PK_CENTER)) {
        sx = &s2;
        sy = &s1;
    } else if (s1.kw == PK_CENTER && (horiz(s2.kw) || vert(s2.kw))) {
        if (horiz(s2.kw)) {
            sx = &s2;
            sy = &s1;
        } else {
            sx = &s1;
            sy = &s2;
        }
    } else {
        return false;
    }
    out->x = axis_kw(sx->kw, sx->has_off, &sx->off);
    out->y = axis_kw(sy->kw, sy->has_off, &sy->off);
    return true;
}

bool embi_parse_position(embi_arena *a, embi_cur *c, embi_position *out)
{
    pk_t kind[4];
    embi_value v[4];
    uint32_t n = 0;
    while (n < 4 && c->i + n < c->n) {
        pk_t k = classify(a, c->c[c->i + n], &v[n]);
        if (k == PK_NONE) {
            break;
        }
        kind[n++] = k;
    }
    for (uint32_t k = n; k >= 1; k--) {
        if (position_k(kind, v, k, out)) {
            c->i += k;
            return true;
        }
    }
    return false;
}

bool embi_parse_pos_axis(embi_arena *a, embi_cur *c, bool horizontal, embi_pos_axis *out)
{
    const embi_css_cv *t0 = cur_peek(c);
    if (!t0) {
        return false;
    }
    embi_value lp0 = {0};
    pk_t k0 = classify(a, t0, &lp0);
    pk_t lo = horizontal ? PK_LEFT : PK_TOP;
    pk_t hi = horizontal ? PK_RIGHT : PK_BOTTOM;
    embi_value none = {0};
    if (k0 == PK_CENTER) {
        c->i++;
        *out = axis_kw(PK_CENTER, false, &none);
        return true;
    }
    if (k0 == PK_LP) {
        c->i++;
        *out = axis_kw(PK_LP, false, &lp0);
        return true;
    }
    if (k0 != lo && k0 != hi) {
        return false;
    }
    c->i++;
    const embi_css_cv *t1 = cur_peek(c);
    if (t1) {
        embi_value lp1 = {0};
        if (classify(a, t1, &lp1) == PK_LP) {
            c->i++;
            *out = axis_kw(k0, true, &lp1);
            return true;
        }
    }
    *out = axis_kw(k0, false, &none);
    return true;
}

/* ------------------------------------------------------------------ border-radius sets */

static uint32_t take_radii(embi_arena *a, embi_cur *c, embi_value out[4])
{
    uint32_t n = 0;
    while (n < 4 && !cur_done(c)) {
        const embi_css_cv *cv = cur_peek(c);
        if (cv_delim(cv, '/') || cv_comma(cv)) {
            break;
        }
        if (!embi_value_parse_types(a, EMBI_LP_NN, cv, &out[n])) {
            return 0;
        }
        c->i++;
        n++;
    }
    return n;
}

static void expand4(const embi_value in[4], uint32_t n, embi_value out[4])
{
    /* Same pattern as margin: top-left, top-right, bottom-right, bottom-left take values
     * 0, 1, 2, 3 with 1->0, 2->{0,1}, 3->{0,1,2,(1)}. */
    static const uint8_t pick[5][4] = {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3},
    };
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = in[pick[n][i]];
    }
}

bool embi_parse_radius_set(embi_arena *a, embi_cur *c, bool allow_slash, embi_value h[4],
                           embi_value v[4])
{
    embi_value tmp[4];
    uint32_t nh = take_radii(a, c, tmp);
    if (nh == 0) {
        return false;
    }
    expand4(tmp, nh, h);
    if (allow_slash && cv_delim(cur_peek(c), '/')) {
        c->i++;
        embi_value tv[4];
        uint32_t nv = take_radii(a, c, tv);
        if (nv == 0) {
            return false;
        }
        expand4(tv, nv, v);
    } else {
        memcpy(v, h, sizeof(embi_value) * 4);
    }
    return true;
}
