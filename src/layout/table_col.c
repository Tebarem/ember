/*
 * Table columns (CSS Tables 3 section 3, W3g): the width and span of column and column group boxes,
 * the integer attributes of the table parts (colspan, rowspan, span), and the column width
 * distribution of auto and fixed layout (CSS Tables 3 sections 3.4 and 3.5). The distribution is pure
 * arithmetic on float arrays, so the unit tests reach it without a document.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

float embi_table_col_width(embi_lctx *c, embi_box *col)
{
    const embi_value *v = embi_lay_prop(c, col, EMBI_PROP_width);
    if (v == nullptr || embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD ||
        v->kind == EMBI_V_PERCENT) {
        return NAN;
    }
    float w = embi_lay_len(c, col, v, NAN);
    if (isnan(w)) {
        return NAN;
    }
    return w < 0 ? 0.0f : w;
}

/* The width of a column or column group after min-width and max-width (both apply to them, CSS 2.1 section 10.4);
 * a min-width alone gives a column that has no width its minimum. */
float embi_table_col_width_clamped(embi_lctx *c, embi_box *col)
{
    float w = embi_table_col_width(c, col);
    const embi_value *mx = embi_lay_prop(c, col, EMBI_PROP_max_width);
    if (mx != nullptr && mx->kind != EMBI_V_KEYWORD && mx->kind != EMBI_V_PERCENT) {
        float m = embi_lay_len(c, col, mx, NAN);
        if (!isnan(m) && !isnan(w) && w > m) {
            w = m < 0 ? 0.0f : m;
        }
    }
    const embi_value *mn = embi_lay_prop(c, col, EMBI_PROP_min_width);
    if (mn != nullptr && mn->kind != EMBI_V_KEYWORD && mn->kind != EMBI_V_PERCENT) {
        float m = embi_lay_len(c, col, mn, NAN);
        if (!isnan(m) && m > 0 && (isnan(w) || w < m)) {
            w = m;
        }
    }
    return w;
}

float embi_table_col_pct(embi_lctx *c, embi_box *col)
{
    const embi_value *v = embi_lay_prop(c, col, EMBI_PROP_width);
    if (v == nullptr || v->kind != EMBI_V_PERCENT) {
        return NAN;
    }
    return v->num < 0 ? 0.0f : (float)(v->num / 100.0);
}

uint32_t embi_table_attr_uint(const embi_box *b, const char *name, uint32_t dflt, uint32_t lo,
                              uint32_t hi)
{
    if (b->el == nullptr || b->el->base.extra != EMBI_NS_HTML) {
        return dflt;
    }
    embi_str q = {name, (uint32_t)strlen(name)};
    const embi_attr *a = embi_el_get_attribute(b->el, q);
    if (a == nullptr) {
        return dflt;
    }
    /* Parse the units in place (no heap buffer): leading ASCII white space, then digits (HTML
     * "rules for parsing non-negative integers", trailing junk ignored). */
    uint32_t i = 0, n = a->value.len;
    while (i < n) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u != ' ' && u != 0x09 && u != 0x0A && u != 0x0C && u != 0x0D) {
            break;
        }
        i++;
    }
    if (i < n && embi_dstr_unit(a->value, i) == '+') {
        i++;
    }
    bool any = false;
    uint64_t v = 0;
    while (i < n) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u < '0' || u > '9') {
            break;
        }
        if (v < 1000000) {
            v = v * 10 + (uint64_t)(u - '0');
        }
        any = true;
        i++;
    }
    if (!any) {
        return dflt;
    }
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return (uint32_t)v;
}

uint32_t embi_table_col_span(const embi_box *col)
{
    return embi_table_attr_uint(col, "span", 1, 1, 1000);
}

/* Auto layout (CSS Tables 3 section 3.5, "distributing width to columns"). The sizing guesses are, in
 * order: every column at its minimum; the percentage columns at max(min, pct * avail); the same with
 * the constrained (fixed width) columns at their maximum; and the same with every column at its
 * maximum. When avail is below the sum of the minima the columns get their minima; between two
 * guesses each column is the linear combination of the two; past the last one the extra goes to the
 * auto columns in proportion to their maximum, else to the constrained ones, else to the percentage
 * columns in proportion to their percentage. */
void embi_table_distribute_auto(uint32_t n, const float *mn, const float *mx, const float *pct,
                                const uint8_t *fixedc, float avail, float *out)
{
    EMBI_VEC(float) g = {nullptr, 0, 0}; /* four guesses of n columns */
    for (uint32_t i = 0; i < 4 * n; i++) {
        embi_vec_push(&g, 0.0f);
    }
    float sum[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        bool ispct = pct != nullptr && !isnan(pct[i]) && pct[i] > 0;
        bool cons = fixedc != nullptr && fixedc[i];
        float lo = mn[i];
        float hi = fmaxf(mx[i], mn[i]);
        float pv = ispct ? fmaxf(lo, pct[i] * avail) : lo;
        g.data[0 * n + i] = lo;
        g.data[1 * n + i] = pv;
        g.data[2 * n + i] = ispct ? pv : (cons ? hi : lo);
        g.data[3 * n + i] = ispct ? pv : hi;
        for (uint32_t k = 0; k < 4; k++) {
            sum[k] += g.data[k * n + i];
        }
    }
    if (avail <= sum[0]) {
        for (uint32_t i = 0; i < n; i++) {
            out[i] = g.data[i];
        }
        embi_vec_free(&g);
        return;
    }
    for (uint32_t k = 0; k < 3; k++) {
        if (avail <= sum[k + 1]) {
            float span = sum[k + 1] - sum[k];
            float t = span > 0 ? (avail - sum[k]) / span : 1.0f;
            for (uint32_t i = 0; i < n; i++) {
                float a = g.data[k * n + i], z = g.data[(k + 1) * n + i];
                out[i] = a + (z - a) * t;
            }
            embi_vec_free(&g);
            return;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = g.data[3 * n + i];
    }
    float extra = avail - sum[3];
    /* The columns that take the extra: auto, else constrained, else percentage. */
    int cls = 0;
    for (; cls < 3; cls++) {
        bool any = false;
        for (uint32_t i = 0; i < n && !any; i++) {
            bool ispct = pct != nullptr && !isnan(pct[i]) && pct[i] > 0;
            bool cons = fixedc != nullptr && fixedc[i];
            any = cls == 0 ? (!ispct && !cons) : (cls == 1 ? (!ispct && cons) : ispct);
        }
        if (any) {
            break;
        }
    }
    if (cls == 3) {
        cls = 0; /* no columns at all */
    }
    float wsum = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool ispct = pct != nullptr && !isnan(pct[i]) && pct[i] > 0;
        bool cons = fixedc != nullptr && fixedc[i];
        bool take = cls == 0 ? (!ispct && !cons) : (cls == 1 ? (!ispct && cons) : ispct);
        if (take) {
            wsum += cls == 2 ? pct[i] : g.data[3 * n + i];
            cnt++;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        bool ispct = pct != nullptr && !isnan(pct[i]) && pct[i] > 0;
        bool cons = fixedc != nullptr && fixedc[i];
        bool take = cls == 0 ? (!ispct && !cons) : (cls == 1 ? (!ispct && cons) : ispct);
        if (!take) {
            continue;
        }
        float w = cls == 2 ? pct[i] : g.data[3 * n + i];
        out[i] += wsum > 0 ? extra * w / wsum : extra / (float)cnt;
    }
    embi_vec_free(&g);
}

/* Fixed layout (CSS Tables 3 section 3.4, as Chromium resolves it; WPT
 * tentative/table-width-redistribution-fixed.html). Columns with a px width keep it, percentage
 * columns take pct * avail, and the auto columns share what is left (none when that is negative). When
 * the fixed and percentage columns want more than avail, the percentage columns share what the fixed
 * ones leave in proportion to their percentages. With no auto column, extra width goes to the columns
 * with a non-zero px width in proportion to their widths, else to the percentage columns in proportion
 * to their share, else evenly to the zero-width columns. pe (or nullptr) is the border and padding that a
 * content-box cell adds to the width its percentage column asks for: it wants pct * avail + pe. */
void embi_table_distribute_fixed(uint32_t n, const float *spec, const float *pct, const float *pe,
                                 float avail, float *out)
{
    float fixed_sum = 0, p_total = 0;
    uint32_t na = 0, np = 0, nz = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (spec != nullptr && !isnan(spec[i])) {
            fixed_sum += spec[i];
            if (spec[i] <= 0) {
                nz++;
            }
        } else if (pct != nullptr && !isnan(pct[i])) {
            np++;
        } else {
            na++;
        }
    }
    float room = fmaxf(0.0f, avail - fixed_sum);
    float want_sum = 0;
    EMBI_VEC(float) wv = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        float w = 0;
        if (!(spec != nullptr && !isnan(spec[i])) && pct != nullptr && !isnan(pct[i])) {
            w = pct[i] * avail + (pe != nullptr ? pe[i] : 0.0f);
            want_sum += w;
        }
        embi_vec_push(&wv, w);
    }
    const float *want = wv.data;
    bool scale = want_sum > room;
    for (uint32_t i = 0; i < n; i++) {
        if (spec != nullptr && !isnan(spec[i])) {
            out[i] = spec[i];
        } else if (pct != nullptr && !isnan(pct[i])) {
            out[i] = scale ? (want_sum > 0 ? room * want[i] / want_sum : 0.0f) : want[i];
            p_total += out[i];
        } else {
            out[i] = 0;
        }
    }
    embi_vec_free(&wv);
    float left = avail - fixed_sum - p_total;
    if (na > 0) {
        float each = left > 0 ? left / (float)na : 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            bool isfix = spec != nullptr && !isnan(spec[i]);
            bool ispct = !isfix && pct != nullptr && !isnan(pct[i]);
            if (!isfix && !ispct) {
                out[i] = each;
            }
        }
        return;
    }
    if (left <= 0 || n == 0) {
        return;
    }
    float fsum = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (spec != nullptr && !isnan(spec[i]) && spec[i] > 0) {
            fsum += spec[i];
        }
    }
    if (fsum > 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (spec != nullptr && !isnan(spec[i]) && spec[i] > 0) {
                out[i] += left * spec[i] / fsum;
            }
        }
    } else if (np > 0 && p_total > 0) {
        for (uint32_t i = 0; i < n; i++) {
            bool isfix = spec != nullptr && !isnan(spec[i]);
            if (!isfix && pct != nullptr && !isnan(pct[i])) {
                out[i] += left * out[i] / p_total;
            }
        }
    } else if (np > 0) {
        for (uint32_t i = 0; i < n; i++) {
            bool isfix = spec != nullptr && !isnan(spec[i]);
            if (!isfix && pct != nullptr && !isnan(pct[i])) {
                out[i] += left / (float)np;
            }
        }
    } else if (nz > 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (spec != nullptr && !isnan(spec[i])) {
                out[i] += left / (float)nz;
            }
        }
    }
}
