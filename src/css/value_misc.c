/*
 * Smaller compound grammars (W2c): keyword sets, box alignment, aspect-ratio, border radii and
 * spacing, quotes, content, cursor, counters, color-scheme, scroll snapping, container names,
 * contain-intrinsic-*, overflow-clip-margin, text-overflow and the font property grammars
 * (font-style with an oblique angle, font-weight range, font-size-adjust, feature and variation
 * settings, font-variant-alternates).
 */
#include "css/value_int.h"

/* One value from 1..n items: a lone item is stored as is, more become a space separated list. */
static bool make_seq(embi_arena *a, const embi_value *v, uint32_t n, embi_value *out)
{
    if (n == 1) {
        *out = v[0];
        return true;
    }
    return embi_make_list(a, v, n, EMBI_SEP_SPACE, out);
}

/* ------------------------------------------------------------------ keyword sets */

typedef struct kwset_def {
    uint16_t prop;
    const uint16_t *groups[6]; /* each a 0 terminated list of keywords of which at most one may appear */
    uint8_t required;          /* group index that must appear, or 0xff */
} kwset_def;

#define G(...) (const uint16_t[]){__VA_ARGS__, 0}

static const kwset_def kwset_defs[] = {
    {EMBI_PROP_text_decoration_line,
     {G(EMBI_KW_underline), G(EMBI_KW_overline), G(EMBI_KW_line_through), G(EMBI_KW_blink)}, 0xff},
    {EMBI_PROP_text_decoration_skip_spaces, {G(EMBI_KW_none, EMBI_KW_all), G(EMBI_KW_start), G(EMBI_KW_end)}, 0xff},
    {EMBI_PROP_white_space_trim,
     {G(EMBI_KW_discard_before), G(EMBI_KW_discard_after), G(EMBI_KW_discard_inner)}, 0xff},
    {EMBI_PROP_text_transform,
     {G(EMBI_KW_capitalize, EMBI_KW_uppercase, EMBI_KW_lowercase), G(EMBI_KW_full_width),
      G(EMBI_KW_full_size_kana)}, 0xff},
    {EMBI_PROP_text_underline_position,
     {G(EMBI_KW_from_font, EMBI_KW_under), G(EMBI_KW_left, EMBI_KW_right)}, 0xff},
    {EMBI_PROP_text_emphasis_position,
     {G(EMBI_KW_over, EMBI_KW_under), G(EMBI_KW_left, EMBI_KW_right)}, 0},
    {EMBI_PROP_touch_action,
     {G(EMBI_KW_pan_x, EMBI_KW_pan_left, EMBI_KW_pan_right),
      G(EMBI_KW_pan_y, EMBI_KW_pan_up, EMBI_KW_pan_down), G(EMBI_KW_pinch_zoom)}, 0xff},
    {EMBI_PROP_contain,
     {G(EMBI_KW_size, EMBI_KW_inline_size), G(EMBI_KW_layout), G(EMBI_KW_style), G(EMBI_KW_paint)},
     0xff},
    {EMBI_PROP_scrollbar_gutter, {G(EMBI_KW_stable), G(EMBI_KW_both_edges)}, 0},
    {EMBI_PROP_grid_auto_flow, {G(EMBI_KW_row, EMBI_KW_column), G(EMBI_KW_dense)}, 0xff},
    {EMBI_PROP_font_variant_ligatures,
     {G(EMBI_KW_common_ligatures, EMBI_KW_no_common_ligatures),
      G(EMBI_KW_discretionary_ligatures, EMBI_KW_no_discretionary_ligatures),
      G(EMBI_KW_historical_ligatures, EMBI_KW_no_historical_ligatures),
      G(EMBI_KW_contextual, EMBI_KW_no_contextual)}, 0xff},
    {EMBI_PROP_font_variant_numeric,
     {G(EMBI_KW_lining_nums, EMBI_KW_oldstyle_nums), G(EMBI_KW_proportional_nums, EMBI_KW_tabular_nums),
      G(EMBI_KW_diagonal_fractions, EMBI_KW_stacked_fractions), G(EMBI_KW_ordinal),
      G(EMBI_KW_slashed_zero)}, 0xff},
    {EMBI_PROP_font_variant_east_asian,
     {G(EMBI_KW_jis78, EMBI_KW_jis83, EMBI_KW_jis90, EMBI_KW_jis04, EMBI_KW_simplified,
        EMBI_KW_traditional),
      G(EMBI_KW_full_width, EMBI_KW_proportional_width), G(EMBI_KW_ruby)}, 0xff},
};

static const kwset_def *kwset_for(embi_prop_id id)
{
    for (size_t i = 0; i < sizeof kwset_defs / sizeof kwset_defs[0]; i++) {
        if (kwset_defs[i].prop == id) {
            return &kwset_defs[i];
        }
    }
    return nullptr;
}

/* The group a keyword belongs to, or -1. */
static int kwset_group(const kwset_def *d, uint16_t kw)
{
    for (int gi = 0; gi < 6 && d->groups[gi]; gi++) {
        for (const uint16_t *k = d->groups[gi]; *k; k++) {
            if (*k == kw) {
                return gi;
            }
        }
    }
    return -1;
}

/* Parses the keywords of kwset_def d in c (any order, at most one per group). */
static bool kwset_parse(embi_arena *a, const kwset_def *d, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(a, embi_value, n);
    uint32_t used = 0;
    uint32_t k = 0;
    while (!cur_done(c)) {
        uint16_t kw = cv_kw(cur_peek(c));
        int gi = kw ? kwset_group(d, kw) : -1;
        if (gi < 0 || (used & (1u << gi)) != 0) {
            return false;
        }
        used |= 1u << gi;
        v[k++] = v_kw(kw);
        c->i++;
    }
    if (d->required != 0xff && (used & (1u << d->required)) == 0) {
        return false;
    }
    return embi_make_list_kw(a, v, k, EMBI_SEP_SPACE, out);
}

bool embi_gram_kwset(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const kwset_def *d = kwset_for(g->id);
    return d && kwset_parse(g->a, d, c, out);
}

/* ------------------------------------------------------------------ box alignment */

static bool align_pos_kw(uint16_t kw)
{
    return kw == EMBI_KW_start || kw == EMBI_KW_end || kw == EMBI_KW_flex_start ||
           kw == EMBI_KW_flex_end || kw == EMBI_KW_center || kw == EMBI_KW_self_start ||
           kw == EMBI_KW_self_end || kw == EMBI_KW_left || kw == EMBI_KW_right;
}

/* A keyword of the property, `[first|last] baseline`, or `[safe|unsafe] <position>`. */
/* flow-start and flow-end (CSS Box Alignment 3): the start and end of the flow, which is what flex-start and flex-end
 * mean in every layout mode that uses them (in flex layout the flex flow; elsewhere start and end), so they are
 * stored as those. */
static uint16_t align_kw(const embi_css_cv *t)
{
    if (cv_ident(t, "flow-start")) {
        return EMBI_KW_flex_start;
    }
    if (cv_ident(t, "flow-end")) {
        return EMBI_KW_flex_end;
    }
    return cv_kw(t);
}

bool embi_gram_align(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_peek(c);
    uint16_t kw = align_kw(t);
    if (!kw) {
        return false;
    }
    if ((kw == EMBI_KW_first || kw == EMBI_KW_last) && cv_ident(cur_at(c, 1), "baseline")) {
        embi_value v[2] = {v_kw(kw), v_kw(EMBI_KW_baseline)};
        c->i += 2;
        return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
    }
    if ((kw == EMBI_KW_safe || kw == EMBI_KW_unsafe)) {
        uint16_t pos = align_kw(cur_at(c, 1));
        if (pos && align_pos_kw(pos) && embi_prop_has_kw(g->pi, pos)) {
            embi_value v[2] = {v_kw(kw), v_kw(pos)};
            c->i += 2;
            return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
        }
        return false;
    }
    if (!embi_prop_has_kw(g->pi, kw)) {
        return false;
    }
    c->i++;
    *out = v_kw(kw);
    return true;
}

/* ------------------------------------------------------------------ small numeric grammars */

static bool parse_ratio_part(embi_arena *a, embi_cur *c, double *out)
{
    embi_value v;
    const embi_css_cv *t = cur_peek(c);
    if (!t || t->tok.type != EMBI_CSS_NUMBER || !embi_value_parse_types(a, EMBI_T_NUM | EMBI_T_NONNEG, t, &v)) {
        return false;
    }
    *out = v.num;
    c->i++;
    return true;
}

bool embi_gram_aspect_ratio(embi_gctx *g, embi_cur *c, embi_value *out)
{
    bool have_auto = false;
    if (cv_ident(cur_peek(c), "auto")) {
        have_auto = true;
        c->i++;
    }
    embi_value r = {0};
    r.kind = EMBI_V_RATIO;
    if (!parse_ratio_part(g->a, c, &r.ratio.a)) {
        return false;
    }
    r.ratio.b = 1;
    if (cv_delim(cur_peek(c), '/')) {
        c->i++;
        if (!parse_ratio_part(g->a, c, &r.ratio.b)) {
            return false;
        }
    }
    if (!have_auto && cv_ident(cur_peek(c), "auto")) {
        have_auto = true;
        c->i++;
    }
    if (!have_auto) {
        *out = r;
        return true;
    }
    embi_value v[2] = {v_kw(EMBI_KW_auto), r};
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* border-spacing: <length>{1,2}, non-negative. Always two values. */
bool embi_gram_spacing(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        if (!embi_value_parse_types(g->a, EMBI_LEN_NN, cur_peek(c), &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* text-decoration-inset: [ <length-percentage> | auto ]{1,2}: the start and end insets of a decoration line. One value
 * gives both. Always two values. */
bool embi_gram_deco_inset(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (cv_ident(t, "auto")) {
            v[n] = v_kw(EMBI_KW_auto);
        } else if (!embi_value_parse_types(g->a, EMBI_LP, t, &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* text-indent: <length-percentage> && hanging? && each-line? (CSS Text 3 section 8.1). Without keywords the value is the
 * length itself; with them a list of the length and the keywords, in that order (hanging before each-line). */
bool embi_gram_text_indent(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value len = {0};
    bool have_len = false, hang = false, each = false;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (!hang && cv_ident(t, "hanging")) {
            hang = true;
        } else if (!each && cv_ident(t, "each-line")) {
            each = true;
        } else if (!have_len && embi_value_parse_types(g->a, EMBI_LP, t, &len)) {
            have_len = true;
        } else {
            return false;
        }
        c->i++;
    }
    if (!have_len) {
        return false;
    }
    if (!hang && !each) {
        *out = len;
        return true;
    }
    embi_value v[3];
    uint32_t n = 0;
    v[n++] = len;
    if (hang) {
        v[n++] = v_kw(EMBI_KW_hanging);
    }
    if (each) {
        v[n++] = v_kw(EMBI_KW_each_line);
    }
    return embi_make_list(g->a, v, n, EMBI_SEP_SPACE, out);
}

/* border-*-radius: <length-percentage [0,∞)>{1,2}, always two values (horizontal, vertical). */
bool embi_gram_radius(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        if (!embi_value_parse_types(g->a, EMBI_LP_NN, cur_peek(c), &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        v[1] = v[0];
    }
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* ------------------------------------------------------------------ strings and generated content */

bool embi_gram_quotes(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n < 2 || (n & 1) != 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_cv *t = cur_take(c);
        if (!cv_string(t)) {
            return false;
        }
        v[i] = (embi_value){0};
        v[i].kind = EMBI_V_STRING;
        v[i].str = t->tok.value;
    }
    return embi_make_list(g->a, v, n, EMBI_SEP_SPACE, out);
}

static bool name_ident(const embi_css_cv *t)
{
    return cv_any_ident(t) && !embi_ident_reserved(t->tok.value) && !cv_ident(t, "none");
}

static bool make_func(embi_arena *a, const char *name, const embi_value *args, uint32_t n,
                      embi_value *out)
{
    embi_value *copy = EMBI_NEW_ARRAY(a, embi_value, n);
    if (n > 0) {
        memcpy(copy, args, sizeof(embi_value) * n);
    }
    *out = (embi_value){0};
    out->kind = EMBI_V_FUNC;
    out->func.name = embi_atom_intern(name, (uint32_t)strlen(name));
    out->func.args = copy;
    out->func.n = n;
    return true;
}

static embi_value v_ident(const embi_css_cv *t)
{
    embi_value v = {0};
    v.kind = EMBI_V_IDENT;
    v.ident = embi_atom_intern(t->tok.value.ptr, t->tok.value.len);
    return v;
}

/* counter(<name>, <style>?) and counters(<name>, <string>, <style>?). */
static bool parse_counter_fn(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    bool plural = cv_fn(fn, "counters");
    embi_cur args;
    embi_cur_kids(a, fn, &args);
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(a, &args, &n);
    uint32_t min = plural ? 2 : 1;
    if (n < min || n > min + 1) {
        return false;
    }
    embi_value v[3];
    uint32_t k = 0;
    if (cur_left(&segs[0]) != 1 || !name_ident(cur_peek(&segs[0]))) {
        return false;
    }
    v[k++] = v_ident(cur_peek(&segs[0]));
    if (plural) {
        const embi_css_cv *s = cur_peek(&segs[1]);
        if (cur_left(&segs[1]) != 1 || !cv_string(s)) {
            return false;
        }
        v[k] = (embi_value){0};
        v[k].kind = EMBI_V_STRING;
        v[k++].str = s->tok.value;
    }
    if (n > min) {
        const embi_css_cv *st = cur_peek(&segs[min]);
        if (cur_left(&segs[min]) != 1 || !cv_any_ident(st) || embi_ident_reserved(st->tok.value)) {
            return false;
        }
        v[k++] = v_ident(st);
    }
    return make_func(a, plural ? "counters" : "counter", v, k, out);
}

/* attr(<name> <type>?, <fallback>?) */
static bool parse_attr_fn(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    embi_value v[3];
    uint32_t k = 0;
    uint32_t i = 0;
    while (i < fn->nkids && fn->kids[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    if (i >= fn->nkids || fn->kids[i].tok.type != EMBI_CSS_IDENT) {
        return false;
    }
    v[k++] = v_ident(&fn->kids[i]);
    i++;
    while (i < fn->nkids && fn->kids[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    if (i < fn->nkids && fn->kids[i].tok.type == EMBI_CSS_IDENT) {
        v[k++] = v_ident(&fn->kids[i]);
        i++;
        while (i < fn->nkids && fn->kids[i].tok.type == EMBI_CSS_WS) {
            i++;
        }
    }
    if (i < fn->nkids) {
        if (fn->kids[i].tok.type != EMBI_CSS_COMMA) {
            return false;
        }
        i++;
        embi_css_slice fb = {fn->kids + i, fn->nkids - i};
        while (fb.n > 0 && fb.p[0].tok.type == EMBI_CSS_WS) {
            fb.p++;
            fb.n--;
        }
        while (fb.n > 0 && fb.p[fb.n - 1].tok.type == EMBI_CSS_WS) {
            fb.n--;
        }
        if (fb.n > 0 && !embi_css_value_opaque_ok(fb)) {
            return false;
        }
        v[k] = (embi_value){0};
        v[k].kind = EMBI_V_RAW;
        v[k++].raw = fb;
    }
    return make_func(a, "attr", v, k, out);
}

static bool content_item(embi_gctx *g, const embi_css_cv *t, bool alt, embi_value *out)
{
    if (cv_string(t)) {
        *out = (embi_value){0};
        out->kind = EMBI_V_STRING;
        out->str = t->tok.value;
        return true;
    }
    if (cv_fn(t, "counter") || cv_fn(t, "counters")) {
        return parse_counter_fn(g->a, t, out);
    }
    if (cv_fn(t, "attr")) {
        return parse_attr_fn(g->a, t, out);
    }
    if (alt) {
        return false;
    }
    uint16_t kw = cv_kw(t);
    if (kw && embi_prop_has_kw(g->pi, kw) &&
        (kw == EMBI_KW_open_quote || kw == EMBI_KW_close_quote || kw == EMBI_KW_no_open_quote ||
         kw == EMBI_KW_no_close_quote)) {
        *out = v_kw(kw);
        return true;
    }
    return embi_parse_image(g->a, t, out);
}

bool embi_gram_content(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    uint32_t nalt = 0;
    bool alt = false;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        if (cv_delim(t, '/')) {
            if (alt || k == 0) {
                return false;
            }
            alt = true;
            continue;
        }
        if (!content_item(g, t, alt, &v[k])) {
            return false;
        }
        k++;
        if (alt) {
            nalt++;
        }
    }
    if (alt && nalt == 0) {
        return false;
    }
    if (k == 1 && !alt && v[0].kind == EMBI_V_KEYWORD) {
        *out = v[0];
        return true;
    }
    if (!embi_make_list(g->a, v, k, EMBI_SEP_SPACE, out)) {
        return false;
    }
    out->aux = (uint8_t)(nalt > 255 ? 255 : nalt);
    return true;
}

/* cursor: [<url> [<x> <y>]?,]* <keyword> */
bool embi_gram_cursor(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = 0;
    embi_cur *segs = embi_cur_split(g->a, c, &n);
    embi_value *items = EMBI_NEW_ARRAY(g->a, embi_value, n);
    for (uint32_t i = 0; i < n; i++) {
        embi_cur *s = &segs[i];
        if (i + 1 == n) {
            if (cur_left(s) != 1 || !embi_gram_kw(g->pi, cur_peek(s), &items[i])) {
                return false;
            }
            continue;
        }
        embi_value img;
        if (!embi_parse_image(g->a, cur_peek(s), &img) ||
            (img.image->kind != EMBI_IMG_URL && img.image->kind != EMBI_IMG_IMAGE_SET)) {
            return false;
        }
        s->i++;
        if (cur_done(s)) {
            items[i] = img;
            continue;
        }
        embi_value hot[3];
        hot[0] = img;
        if (cur_left(s) != 2 || !embi_value_parse_types(g->a, EMBI_T_NUM, cur_at(s, 0), &hot[1]) ||
            !embi_value_parse_types(g->a, EMBI_T_NUM, cur_at(s, 1), &hot[2])) {
            return false;
        }
        embi_make_list(g->a, hot, 3, EMBI_SEP_SPACE, &items[i]);
    }
    c->i = c->n;
    if (n == 1) {
        *out = items[0];
        return true;
    }
    return embi_make_list(g->a, items, n, EMBI_SEP_COMMA, out);
}

/* counter-reset / counter-set / counter-increment: [<counter-name> <integer>?]+ */
bool embi_gram_counters(embi_gctx *g, embi_cur *c, embi_value *out)
{
    double def = g->id == EMBI_PROP_counter_increment ? 1 : 0;
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *pairs = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        embi_value pr[2];
        if (name_ident(t)) {
            pr[0] = v_ident(t);
        } else if (cv_fn(t, "reversed") && g->id == EMBI_PROP_counter_reset) {
            embi_cur kids;
            embi_cur_kids(g->a, t, &kids);
            if (cur_left(&kids) != 1 || !name_ident(cur_peek(&kids))) {
                return false;
            }
            embi_value nm = v_ident(cur_peek(&kids));
            if (!make_func(g->a, "reversed", &nm, 1, &pr[0])) {
                return false;
            }
        } else {
            return false;
        }
        /* An omitted integer is a NUMBER 0 (or 1), a written one an INTEGER: layout tells them apart (CSS Lists 3
         * section 4.4.2: a reversed counter without a value takes its start from its scope; an ol start attribute
         * replaces a counter-reset without one). */
        pr[1] = v_num(def);
        const embi_css_cv *nx = cur_peek(c);
        if (nx && nx->tok.type == EMBI_CSS_NUMBER && nx->tok.flag) {
            pr[1] = v_int(nx->tok.num);
            c->i++;
        } else if (nx && nx->tok.type == EMBI_CSS_FUNCTION) {
            embi_value iv;
            if (embi_value_parse_types(g->a, EMBI_T_INT, nx, &iv) && iv.kind == EMBI_V_INTEGER) {
                pr[1] = iv; /* a constant calc() of numbers, rounded to an integer */
                c->i++;
            }
        }
        embi_make_list(g->a, pr, 2, EMBI_SEP_SPACE, &pairs[k++]);
    }
    return embi_make_list(g->a, pairs, k, EMBI_SEP_SPACE, out);
}

/* ------------------------------------------------------------------ names and schemes */

bool embi_gram_color_scheme(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    bool only = false;
    bool light = false, dark = false;
    uint32_t schemes = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        if (!cv_any_ident(t) || embi_ident_reserved(t->tok.value) || cv_ident(t, "normal")) {
            return false;
        }
        if (cv_ident(t, "only")) {
            if (only) {
                return false;
            }
            only = true;
            v[k++] = v_kw(EMBI_KW_only);
        } else if (cv_ident(t, "light")) {
            if (light) {
                return false;
            }
            light = true;
            schemes++;
            v[k++] = v_kw(EMBI_KW_light);
        } else if (cv_ident(t, "dark")) {
            if (dark) {
                return false;
            }
            dark = true;
            schemes++;
            v[k++] = v_kw(EMBI_KW_dark);
        } else {
            schemes++;
            v[k++] = v_ident(t);
        }
    }
    if (schemes == 0) {
        return false;
    }
    return embi_make_list(g->a, v, k, EMBI_SEP_SPACE, out);
}

bool embi_gram_container_name(embi_gctx *g, embi_cur *c, embi_value *out)
{
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (!name_ident(t) || cv_ident(t, "and") || cv_ident(t, "or") || cv_ident(t, "not")) {
            break; /* prefix semantics: the container shorthand continues after the names */
        }
        v[k++] = v_ident(t);
        c->i++;
    }
    if (k == 0) {
        return false;
    }
    return embi_make_list(g->a, v, k, EMBI_SEP_SPACE, out);
}

/* scroll-snap-type: none | [x | y | block | inline | both] [mandatory | proximity]? */
bool embi_gram_snap_type(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *ax = cur_take(c);
    if (!ax || !(cv_ident(ax, "x") || cv_ident(ax, "y") || cv_ident(ax, "block") ||
                 cv_ident(ax, "inline") || cv_ident(ax, "both"))) {
        return false;
    }
    embi_value v[2];
    v[0] = v_kw(cv_kw(ax));
    uint32_t n = 1;
    const embi_css_cv *st = cur_peek(c);
    if (st) {
        if (!(cv_ident(st, "mandatory") || cv_ident(st, "proximity"))) {
            return false;
        }
        v[n++] = v_kw(cv_kw(st));
        c->i++;
    }
    return embi_make_list_kw(g->a, v, n, EMBI_SEP_SPACE, out);
}

/* scroll-snap-align: [none | start | end | center]{1,2} */
bool embi_gram_snap_align(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        if (!embi_gram_kw(g->pi, cur_peek(c), &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    return n > 0 && make_seq(g->a, v, n, out);
}

bool embi_gram_text_overflow(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    while (n < 2 && !cur_done(c)) {
        const embi_css_cv *t = cur_peek(c);
        if (cv_string(t)) {
            v[n] = (embi_value){0};
            v[n].kind = EMBI_V_STRING;
            v[n].str = t->tok.value;
        } else if (!embi_gram_kw(g->pi, t, &v[n])) {
            return false;
        }
        c->i++;
        n++;
    }
    return n > 0 && make_seq(g->a, v, n, out);
}

/* text-emphasis-style: none | [filled | open] || [dot | circle | double-circle | triangle | sesame]
 * | <string>. Consumes at most the tokens that belong to it, so the shorthand can continue. */
bool embi_gram_emphasis_style(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_peek(c);
    if (!t) {
        return false;
    }
    if (cv_string(t)) {
        *out = (embi_value){0};
        out->kind = EMBI_V_STRING;
        out->str = t->tok.value;
        c->i++;
        return true;
    }
    embi_value v[2];
    uint32_t n = 0;
    bool have_fill = false, have_shape = false;
    while (n < 2 && !cur_done(c)) {
        uint16_t kw = cv_kw(cur_peek(c));
        bool fill = kw == EMBI_KW_filled || kw == EMBI_KW_open;
        bool shape = kw == EMBI_KW_dot || kw == EMBI_KW_circle || kw == EMBI_KW_double_circle ||
                     kw == EMBI_KW_triangle || kw == EMBI_KW_sesame;
        if ((!fill && !shape) || (fill ? have_fill : have_shape)) {
            break;
        }
        if (fill) {
            have_fill = true;
        } else {
            have_shape = true;
        }
        v[n++] = v_kw(kw);
        c->i++;
    }
    return n > 0 && make_seq(g->a, v, n, out);
}

/* clip: rect(<top>, <right>, <bottom>, <left>) with each side <length> | auto. The commas are
 * optional (CSS 2.1 allows both spellings). The keyword auto is handled by the keyword list. */
bool embi_gram_clip_rect(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *f = cur_take(c);
    if (!f || f->tok.type != EMBI_CSS_FUNCTION || !embi_str_is(f->tok.value, "rect")) {
        return false;
    }
    embi_cur args;
    embi_cur_kids(g->a, f, &args);
    embi_value v[4];
    uint32_t n = 0, commas = 0;
    while (!cur_done(&args)) {
        const embi_css_cv *t = cur_take(&args);
        if (t->tok.type == EMBI_CSS_COMMA) {
            if (n == 0 || commas >= n) {
                return false; /* leading or doubled comma */
            }
            commas++;
            continue;
        }
        if (n == 4 || (n > 0 && commas != 0 && commas != n)) {
            return false;
        }
        if (cv_ident(t, "auto")) {
            v[n] = v_kw(EMBI_KW_auto);
        } else if (!embi_value_parse_types(g->a, EMBI_T_LEN, t, &v[n])) {
            return false;
        }
        n++;
    }
    if (n != 4 || (commas != 0 && commas != 3)) {
        return false;
    }
    return embi_make_list(g->a, v, 4, EMBI_SEP_SPACE, out);
}

/* overflow-clip-margin: <visual-box> || <length [0,∞)>. Always [box, length]. */
bool embi_gram_clip_margin(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value box = v_kw(EMBI_KW_padding_box);
    embi_value len = v_px(0);
    bool have_box = false, have_len = false;
    uint32_t n = cur_left(c);
    if (n == 0 || n > 2) {
        return false;
    }
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        if (!have_box && (cv_ident(t, "content-box") || cv_ident(t, "padding-box") ||
                          cv_ident(t, "border-box"))) {
            box = v_kw(cv_kw(t));
            have_box = true;
        } else if (!have_len && embi_value_parse_types(g->a, EMBI_LEN_NN, t, &len)) {
            have_len = true;
        } else {
            return false;
        }
    }
    embi_value v[2] = {box, len};
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* scrollbar-color: auto | <color>{2} (the thumb color, then the track color). "auto" is a keyword value before the grammar
 * runs; this always builds the list [thumb, track]. */
bool embi_gram_scrollbar_color(embi_gctx *g, embi_cur *c, embi_value *out)
{
    if (cur_left(c) != 2) {
        return false;
    }
    embi_value v[2];
    for (int i = 0; i < 2; i++) {
        if (!embi_value_parse_types(g->a, EMBI_T_COLOR, cur_take(c), &v[i])) {
            return false;
        }
    }
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* contain-intrinsic-*: auto? [none | <length [0,∞)>] */
bool embi_gram_contain_intrinsic(embi_gctx *g, embi_cur *c, embi_value *out)
{
    bool have_auto = false;
    if (cv_ident(cur_peek(c), "auto")) {
        have_auto = true;
        c->i++;
    }
    embi_value x;
    const embi_css_cv *t = cur_take(c);
    if (cv_ident(t, "none")) {
        x = v_kw(EMBI_KW_none);
    } else if (!embi_value_parse_types(g->a, EMBI_LEN_NN, t, &x)) {
        return false;
    }
    if (!have_auto) {
        *out = x;
        return true;
    }
    embi_value v[2] = {v_kw(EMBI_KW_auto), x};
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* ------------------------------------------------------------------ fonts */

/* font-style: normal | italic | oblique <angle [-90deg,90deg]>? */
bool embi_gram_font_style(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_take(c);
    uint16_t kw = cv_kw(t);
    if (kw == EMBI_KW_normal || kw == EMBI_KW_italic) {
        *out = v_kw(kw);
        return true;
    }
    if (kw != EMBI_KW_oblique) {
        return false;
    }
    if (cur_done(c)) {
        *out = v_kw(kw);
        return true;
    }
    embi_value ang;
    if (!embi_angle_or_zero(g->a, cur_peek(c), &ang)) {
        return false;
    }
    if (ang.kind == EMBI_V_ANGLE && (ang.num < -90.0 || ang.num > 90.0)) {
        return false;
    }
    c->i++;
    embi_value v[2] = {v_kw(kw), ang};
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* font-weight: <number [1,1000]> (the keywords are handled as whole-value keywords). */
bool embi_gram_font_weight(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_take(c);
    if (!t || !embi_value_parse_types(g->a, EMBI_T_NUM, t, out)) {
        return false;
    }
    if (out->kind == EMBI_V_NUMBER || out->kind == EMBI_V_INTEGER) {
        return out->num >= 1.0 && out->num <= 1000.0;
    }
    return out->kind == EMBI_V_CALC;
}

/* font-size-adjust: none | [ex-height | cap-height | ch-width | ic-width | ic-height]?
 * [from-font | <number [0,∞)>] */
bool embi_gram_font_size_adjust(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value v[2];
    uint32_t n = 0;
    const embi_css_cv *t = cur_peek(c);
    if (cv_ident(t, "ex-height") || cv_ident(t, "cap-height") || cv_ident(t, "ch-width") ||
        cv_ident(t, "ic-width") || cv_ident(t, "ic-height")) {
        v[n++] = v_kw(cv_kw(t));
        c->i++;
    }
    t = cur_take(c);
    if (cv_ident(t, "from-font")) {
        v[n++] = v_kw(EMBI_KW_from_font);
    } else if (t && embi_value_parse_types(g->a, EMBI_T_NUM | EMBI_T_NONNEG, t, &v[n])) {
        n++;
    } else {
        return false;
    }
    return make_seq(g->a, v, n, out);
}

static bool tag_ok(const embi_css_cv *t)
{
    if (!cv_string(t) || t->tok.value.len != 4) {
        return false;
    }
    for (uint32_t i = 0; i < 4; i++) {
        unsigned char ch = (unsigned char)t->tok.value.ptr[i];
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

/* One <feature-tag-value>: <string> [<integer [0,∞)> | on | off]? */
bool embi_gram_font_features(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_take(c);
    if (!tag_ok(t)) {
        return false;
    }
    embi_value v[2] = {{0}, v_int(1)};
    v[0].kind = EMBI_V_STRING;
    v[0].str = t->tok.value;
    const embi_css_cv *nx = cur_peek(c);
    if (nx) {
        if (cv_ident(nx, "on")) {
            v[1] = v_int(1);
        } else if (cv_ident(nx, "off")) {
            v[1] = v_int(0);
        } else if (nx->tok.type == EMBI_CSS_NUMBER && nx->tok.flag && nx->tok.num >= 0) {
            v[1] = v_int(nx->tok.num);
        } else {
            return false;
        }
        c->i++;
    }
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* One <variation-tag-value>: <string> <number> */
bool embi_gram_font_variations(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_take(c);
    if (!tag_ok(t)) {
        return false;
    }
    embi_value v[2] = {{0}, {0}};
    v[0].kind = EMBI_V_STRING;
    v[0].str = t->tok.value;
    if (!embi_value_parse_types(g->a, EMBI_T_NUM, cur_peek(c), &v[1])) {
        return false;
    }
    c->i++;
    return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
}

/* font-variant-alternates: normal | [stylistic(n) || historical-forms || styleset(n#) ||
 * character-variant(n#) || swash(n) || ornaments(n) || annotation(n)] */
bool embi_gram_font_alternates(embi_gctx *g, embi_cur *c, embi_value *out)
{
    static const struct {
        const char *name;
        bool multi;
    } fns[] = {
        {"stylistic", false}, {"styleset", true}, {"character-variant", true},
        {"swash", false},     {"ornaments", false}, {"annotation", false},
    };
    uint32_t n = cur_left(c);
    if (n == 0) {
        return false;
    }
    embi_value *v = EMBI_NEW_ARRAY(g->a, embi_value, n);
    uint32_t k = 0;
    uint32_t seen = 0;
    while (!cur_done(c)) {
        const embi_css_cv *t = cur_take(c);
        if (cv_ident(t, "historical-forms")) {
            if (seen & 0x80) {
                return false;
            }
            seen |= 0x80;
            v[k++] = v_kw(EMBI_KW_historical_forms);
            continue;
        }
        int fi = -1;
        for (int i = 0; i < 6; i++) {
            if (cv_fn(t, fns[i].name)) {
                fi = i;
            }
        }
        if (fi < 0 || (seen & (1u << fi))) {
            return false;
        }
        seen |= 1u << fi;
        embi_cur kids;
        embi_cur_kids(g->a, t, &kids);
        uint32_t ns = 0;
        embi_cur *segs = embi_cur_split(g->a, &kids, &ns);
        if (ns == 0 || (!fns[fi].multi && ns != 1)) {
            return false;
        }
        embi_value *args = EMBI_NEW_ARRAY(g->a, embi_value, ns);
        for (uint32_t i = 0; i < ns; i++) {
            if (cur_left(&segs[i]) != 1 || !cv_any_ident(cur_peek(&segs[i]))) {
                return false;
            }
            args[i] = v_ident(cur_peek(&segs[i]));
        }
        if (!make_func(g->a, fns[fi].name, args, ns, &v[k++])) {
            return false;
        }
    }
    return embi_make_list(g->a, v, k, EMBI_SEP_SPACE, out);
}

/* calc-size(<basis>, <calc-sum>) (CSS Values 5 section 10.1). The basis is auto (any reads as auto),
 * min-content, max-content, fit-content or a <length-percentage>. The calc-sum is the rest of the
 * function after one top-level comma and may use `size`, the basis size. Stored as a FUNC named
 * calc-size with two arguments: the basis value, and a CALC value holding the sum. */
static bool parse_calc_size(embi_arena *a, const embi_css_cv *fn, embi_value *out)
{
    const embi_css_cv *k = fn->kids;
    uint32_t n = fn->nkids;
    uint32_t comma = n;
    for (uint32_t i = 0; i < n; i++) {
        if (k[i].tok.type == EMBI_CSS_COMMA) {
            if (comma != n) {
                return false; /* one comma only */
            }
            comma = i;
        }
    }
    if (comma == n) {
        return false;
    }
    uint32_t bi = 0, be = comma;
    while (bi < be && k[bi].tok.type == EMBI_CSS_WS) {
        bi++;
    }
    while (be > bi && k[be - 1].tok.type == EMBI_CSS_WS) {
        be--;
    }
    if (be - bi != 1) {
        return false;
    }
    const embi_css_cv *bcv = &k[bi];
    embi_value args[2] = {0};
    if (cv_any_ident(bcv)) {
        if (cv_ident(bcv, "auto") || cv_ident(bcv, "any")) {
            args[0] = v_kw(EMBI_KW_auto);
        } else if (cv_ident(bcv, "min-content")) {
            args[0] = v_kw(EMBI_KW_min_content);
        } else if (cv_ident(bcv, "max-content")) {
            args[0] = v_kw(EMBI_KW_max_content);
        } else if (cv_ident(bcv, "fit-content")) {
            args[0] = v_kw(EMBI_KW_fit_content);
        } else {
            return false;
        }
    } else if (!embi_value_parse_types(a, EMBI_LP_NN, bcv, &args[0])) {
        return false;
    }
    uint32_t si = comma + 1, se = n;
    while (si < se && k[si].tok.type == EMBI_CSS_WS) {
        si++;
    }
    while (se > si && k[se - 1].tok.type == EMBI_CSS_WS) {
        se--;
    }
    if (si >= se) {
        return false;
    }
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    if (!embi_calc_parse_sized(a, k + si, se - si, &node, &type)) {
        return false;
    }
    if (type == 0 || (type & ~EMBI_CD_LENPCT) != 0) {
        return false; /* a length-percentage, not a number or an angle */
    }
    args[1] = (embi_value){0};
    args[1].kind = EMBI_V_CALC;
    args[1].calc = node;
    return make_func(a, "calc-size", args, 2, out);
}

/* A size (width, height, their min-/max- forms and the logical ones): a non-negative
 * <length-percentage> or calc(), or fit-content(<length-percentage>) (CSS Sizing 3 section 5.2.2),
 * or calc-size() (CSS Values 5 section 10.1), stored as a FUNC value with the argument. The sizing
 * keywords are the property's own keywords. */
bool embi_gram_size(embi_gctx *g, embi_cur *c, embi_value *out)
{
    const embi_css_cv *t = cur_peek(c);
    if (t == nullptr) {
        return false;
    }
    if (cv_fn(t, "calc-size")) {
        if (!parse_calc_size(g->a, t, out)) {
            return false;
        }
        c->i++;
        return true;
    }
    if (cv_ident(t, "-webkit-fill-available") || cv_ident(t, "-moz-available")) {
        /* The prefixed names of stretch (CSS Sizing 4), which the WPT table tests use. */
        *out = v_kw(EMBI_KW_stretch);
        c->i++;
        return true;
    }
    if (cv_fn(t, "fit-content")) {
        embi_cur args;
        embi_cur_kids(g->a, t, &args);
        embi_value arg = {0};
        if (cur_left(&args) != 1 || !embi_value_parse_types(g->a, EMBI_LP_NN, cur_peek(&args), &arg)) {
            return false;
        }
        c->i++;
        return make_func(g->a, "fit-content", &arg, 1, out);
    }
    if (!embi_value_parse_types(g->a, EMBI_LP_NN, t, out)) {
        return false;
    }
    c->i++;
    return true;
}

/* ------------------------------------------------------------------ line clamping */

/* max-lines: <integer [1,inf]> || auto (CSS Overflow 4 section 5.2). One component is stored as is; both are a
 * space separated list in the order the integer, then auto. */
bool embi_gram_max_lines(embi_gctx *g, embi_cur *c, embi_value *out)
{
    embi_value iv = {0};
    bool have_int = false, have_auto = false;
    while (!cur_done(c) && !(have_int && have_auto)) {
        const embi_css_cv *t = cur_peek(c);
        if (!have_auto && cv_ident(t, "auto")) {
            have_auto = true;
        } else if (!have_int && embi_scalar(g->a, EMBI_T_INT | EMBI_T_POSINT, t, &iv)) {
            have_int = true;
        } else {
            return false;
        }
        c->i++;
    }
    if (!have_int && !have_auto) {
        return false;
    }
    if (have_int && have_auto) {
        embi_value v[2] = {iv, v_kw(EMBI_KW_auto)};
        return embi_make_list(g->a, v, 2, EMBI_SEP_SPACE, out);
    }
    *out = have_int ? iv : v_kw(EMBI_KW_auto);
    return true;
}
