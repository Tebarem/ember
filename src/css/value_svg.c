/*
 * Grammars of the SVG presentation properties (R4c): fill and stroke paints, stroke-dasharray and paint-order.
 */
#include "css/value_int.h"

/* <paint>: <color> | <url> [none | <color>]?. The keywords none, context-fill and context-stroke are keyword values before
 * the grammar runs. A url without a fallback is the URL value; with one it is a space list [URL, fallback], the fallback
 * being a color or the keyword none. */
bool embi_gram_svg_paint(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *first = cur_peek(c);
    if (!first) {
        return false;
    }
    if (first->tok.type == EMBI_CSS_URL || cv_fn(first, "url")) {
        embi_value u;
        c->i++;
        if (!embi_value_parse_types(g->a, EMBI_T_URL, first, &u)) {
            return false;
        }
        if (cur_done(c)) {
            *out = u;
            return true;
        }
        const embi_css_cv *fb = cur_take(c);
        if (!cur_done(c)) {
            return false;
        }
        embi_value v[2];
        v[0] = u;
        if (cv_ident(fb, "none")) {
            v[1] = v_kw(EMBI_KW_none);
        } else if (!embi_scalar(g->a, EMBI_T_COLOR, fb, &v[1])) {
            return false;
        }
        return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
    }
    if (cur_left(c) != 1) {
        return false;
    }
    c->i++;
    return embi_scalar(g->a, EMBI_T_COLOR, first, out);
}

/* stroke-dasharray: none | [<length-percentage> | <number>]+ with commas and/or spaces between the items. Negative
 * items are invalid. The result is a space list. */
bool embi_gram_svg_dasharray(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    bool want_item = true;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        if (cv_comma(t)) {
            if (want_item) {
                return false;
            }
            want_item = true;
            continue;
        }
        if (!embi_value_parse_types(g->a, EMBI_T_LEN | EMBI_T_PCT | EMBI_T_NUM, t, &v[k])) {
            return false;
        }
        if ((v[k].kind == EMBI_V_NUMBER || v[k].kind == EMBI_V_LENGTH || v[k].kind == EMBI_V_PERCENT) && v[k].num < 0) {
            return false;
        }
        k++;
        want_item = false;
    }
    if (want_item || k == 0) {
        return false;
    }
    return embi_make_list(g->a, v, k, EMBI_SEP_SPACE, out);
}

/* paint-order: normal | [fill || stroke || markers]. The result is the list of the keywords as written (a single one is
 * a keyword value). */
bool embi_gram_svg_paint_order(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0 || n > 3) {
        return false;
    }
    embi_value v[3];
    uint32_t k = 0;
    uint32_t used = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        uint32_t bit = 0;
        uint16_t kw = 0;
        if (cv_ident(t, "fill")) {
            bit = 1;
            kw = EMBI_KW_fill;
        } else if (cv_ident(t, "stroke")) {
            bit = 2;
            kw = EMBI_KW_stroke;
        } else if (cv_ident(t, "markers")) {
            bit = 4;
            kw = EMBI_KW_markers;
        } else {
            return false;
        }
        if (used & bit) {
            return false;
        }
        used |= bit;
        v[k++] = v_kw(kw);
    }
    return embi_make_list_kw(g->a, v, k, EMBI_SEP_SPACE, out);
}
