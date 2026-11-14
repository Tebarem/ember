#pragma once
/*
 * Internal declarations shared by the value parsers (value*.c, shorthand*.c). Not for use outside
 * src/css.
 *
 * A grammar function parses one value (or one comma separated item of a list property) from a
 * cursor over the component values, with whitespace already skipped. Commas, slashes and every
 * other token stay in the stream. A function or block is one component with its contents in kids.
 */
#include "css/props.h"

#include <string.h>

typedef struct embi_gctx {
    embi_arena *a;
    embi_prop_id id;
    const embi_prop_info *pi;
} embi_gctx;

typedef struct embi_cur {
    const embi_css_cv *const *c;
    uint32_t n, i;
} embi_cur;

typedef bool (*embi_gram_fn)(embi_gctx *g, embi_cur *c, embi_value *out);

#define EMBI_X_DECL(n) bool embi_gram_##n(embi_gctx *g, embi_cur *c, embi_value *out);
EMBI_GRAMMAR_LIST(EMBI_X_DECL)
#undef EMBI_X_DECL

/* ------------------------------------------------------------------ cursors */

bool embi_cur_make(embi_arena *a, const embi_css_cv *cvs, uint32_t n, embi_cur *out);
static inline bool embi_cur_make_slice(embi_arena *a, embi_css_slice s, embi_cur *out)
{
    return embi_cur_make(a, s.p, s.n, out);
}
/* Cursor over the contents of a function or block component. */
static inline bool embi_cur_kids(embi_arena *a, const embi_css_cv *cv, embi_cur *out)
{
    return embi_cur_make(a, cv->kids, cv->nkids, out);
}
/* Splits the rest of c at top-level commas. Returns the segments (a segment may be empty) and
 * the count in *n, or nullptr on allocation failure. */
embi_cur *embi_cur_split(embi_arena *a, const embi_cur *c, uint32_t *n);

static inline bool cur_done(const embi_cur *c) { return c->i >= c->n; }
static inline uint32_t cur_left(const embi_cur *c) { return c->n - c->i; }
static inline const embi_css_cv *cur_peek(const embi_cur *c)
{
    return c->i < c->n ? c->c[c->i] : nullptr;
}
static inline const embi_css_cv *cur_at(const embi_cur *c, uint32_t k)
{
    return c->i + k < c->n ? c->c[c->i + k] : nullptr;
}
static inline const embi_css_cv *cur_take(embi_cur *c)
{
    return c->i < c->n ? c->c[c->i++] : nullptr;
}

/* ------------------------------------------------------------------ token predicates */

/* ASCII case-insensitive comparison with a lowercase literal. */
bool embi_str_is(embi_str s, const char *lit);
static inline bool cv_ident(const embi_css_cv *cv, const char *lit)
{
    return cv && cv->tok.type == EMBI_CSS_IDENT && embi_str_is(cv->tok.value, lit);
}
static inline bool cv_fn(const embi_css_cv *cv, const char *lit)
{
    return cv && cv->tok.type == EMBI_CSS_FUNCTION && embi_str_is(cv->tok.value, lit);
}
static inline bool cv_delim(const embi_css_cv *cv, uint32_t cp)
{
    return cv && cv->tok.type == EMBI_CSS_DELIM && cv->tok.cp == cp;
}
static inline bool cv_comma(const embi_css_cv *cv) { return cv && cv->tok.type == EMBI_CSS_COMMA; }
static inline bool cv_any_ident(const embi_css_cv *cv)
{
    return cv && cv->tok.type == EMBI_CSS_IDENT;
}
/* The keyword id of an ident component (EMBI_KW_INVALID when not an ident or unknown). */
uint16_t cv_kw(const embi_css_cv *cv);
/* True when the ident is a CSS-wide keyword or `default` (not allowed as a <custom-ident>). */
bool embi_ident_reserved(embi_str s);

/* ------------------------------------------------------------------ value helpers */

static inline embi_value v_kw(uint16_t kw)
{
    embi_value v = {0};
    v.kind = EMBI_V_KEYWORD;
    v.kw = kw;
    return v;
}
static inline embi_value v_num(double n)
{
    embi_value v = {0};
    v.kind = EMBI_V_NUMBER;
    v.num = n;
    return v;
}
static inline embi_value v_px(double n)
{
    embi_value v = {0};
    v.kind = EMBI_V_LENGTH;
    v.unit = EMBI_U_PX;
    v.num = n;
    return v;
}
static inline embi_value v_pct(double n)
{
    embi_value v = {0};
    v.kind = EMBI_V_PERCENT;
    v.num = n;
    return v;
}
static inline embi_value v_int(double n)
{
    embi_value v = {0};
    v.kind = EMBI_V_INTEGER;
    v.num = n;
    return v;
}
/* A LIST value over items allocated in a (copied from src). */
bool embi_make_list(embi_arena *a, const embi_value *src, uint32_t n, uint8_t sep, embi_value *out);
/* Like embi_make_list, but a one-item list holding a keyword collapses to that keyword. */
bool embi_make_list_kw(embi_arena *a, const embi_value *src, uint32_t n, uint8_t sep,
                       embi_value *out);
void *embi_anew(embi_arena *a, size_t size, size_t align);
#define EMBI_NEW(a, T) ((T *)embi_anew((a), sizeof(T), alignof(T)))
#define EMBI_NEW_ARRAY(a, T, n) ((T *)embi_anew((a), sizeof(T) * (size_t)(n), alignof(T)))

/* ------------------------------------------------------------------ scalars */

#define EMBI_LP     (EMBI_T_LEN | EMBI_T_PCT)
#define EMBI_LP_NN  (EMBI_T_LEN | EMBI_T_PCT | EMBI_T_NONNEG)
#define EMBI_LEN_NN (EMBI_T_LEN | EMBI_T_NONNEG)

/* Parses one component as a typed scalar (numbers, lengths, percentages, angles, times,
 * resolutions, colors, calc()). No keywords. */
static inline bool embi_scalar(embi_arena *a, uint32_t types, const embi_css_cv *cv,
                               embi_value *out)
{
    return cv && embi_value_parse_types(a, types, cv, out);
}
/* An <angle> or the unitless zero (as 0deg). */
bool embi_angle_or_zero(embi_arena *a, const embi_css_cv *cv, embi_value *out);
/* A <color>, including light-dark(). */
static inline bool embi_color_cv(embi_arena *a, const embi_css_cv *cv, embi_value *out)
{
    return embi_scalar(a, EMBI_T_COLOR, cv, out);
}
/* True if the component is an unquoted-or-quoted string. */
static inline bool cv_string(const embi_css_cv *cv)
{
    return cv && cv->tok.type == EMBI_CSS_STRING;
}

/* ------------------------------------------------------------------ shared grammars */

/* A <position> of one to four components. Consumes the longest valid prefix. */
bool embi_parse_position(embi_arena *a, embi_cur *c, embi_position *out);
/* One axis of a background-position-x / -y style value: `[center | [left|right]? <lp>?]` for x,
 * the vertical keywords for y. Consumes one to two components. */
bool embi_parse_pos_axis(embi_arena *a, embi_cur *c, bool horizontal, embi_pos_axis *out);
/* 1 to 4 non-negative <length-percentage> values, optionally followed by `/` and 1 to 4 more.
 * Expands to h[4] and v[4] in the order top-left, top-right, bottom-right, bottom-left. With
 * allow_slash false, only the first set is read (v is a copy of h). */
bool embi_parse_radius_set(embi_arena *a, embi_cur *c, bool allow_slash, embi_value h[4],
                           embi_value v[4]);
/* <image>: url(), gradients, image-set(), cross-fade(). One component. */
bool embi_parse_image(embi_arena *a, const embi_css_cv *cv, embi_value *out);
/* <bg-size>: `cover | contain | [<lp>|auto]{1,2}`. */
bool embi_parse_bg_size(embi_arena *a, embi_cur *c, embi_value *out);
/* <repeat-style>: `repeat-x | repeat-y | [repeat|space|round|no-repeat]{1,2}`. */
bool embi_parse_repeat_style(embi_arena *a, embi_cur *c, embi_value *out);
/* <easing-function> that is a function (cubic-bezier, steps, linear), or one easing keyword. */
bool embi_parse_easing(embi_arena *a, const embi_css_cv *cv, embi_value *out);
/* border-image-slice / -width / -outset / -repeat bodies. */
bool embi_parse_bi_slice(embi_arena *a, embi_cur *c, embi_value *out);
bool embi_parse_bi_width(embi_arena *a, embi_cur *c, embi_value *out);
bool embi_parse_bi_outset(embi_arena *a, embi_cur *c, embi_value *out);
bool embi_parse_bi_repeat(embi_arena *a, embi_cur *c, embi_value *out);
/* One grid-line: `auto | <custom-ident> | [<integer>&&<custom-ident>?] | [span&&[<integer>||<custom-ident>]]`. */
bool embi_parse_grid_line(embi_arena *a, embi_cur *c, embi_value *out);
/* A <track-size> (one component or minmax()/fit-content()). */
bool embi_parse_track_size(embi_arena *a, const embi_css_cv *cv, embi_track_size *out);
/* A <line-names> block `[a b]`. Allocates the atoms. */
bool embi_parse_line_names(embi_arena *a, const embi_css_cv *cv, embi_grid_item *out);
/* A <track-list>/<auto-track-list>/subgrid from a cursor (the whole rest of it). */
bool embi_parse_track_list(embi_arena *a, embi_cur *c, embi_value *out);
/* font-family list, font-size etc. via the longhand's own grammar: see embi_value_parse_longhand. */

/* Parses the longhand `prop` from the front of c: a keyword of the property, or the property's
 * grammar / single scalar. Consumes only what one value of that property needs, so shorthands
 * can parse their parts in sequence. Does not handle list properties. */
bool embi_parse_prefix(embi_arena *a, embi_prop_id prop, embi_cur *c, embi_value *out);

/* Keyword by id, or a keyword value. true if the component is an ident naming a keyword in the
 * property's keyword list. */
bool embi_prop_has_kw(const embi_prop_info *pi, uint16_t kw);
/* Parses one ident component as one of the property's keywords. */
bool embi_gram_kw(const embi_prop_info *pi, const embi_css_cv *cv, embi_value *out);

/* Appends/serializes helpers used by value_ser.c. */
void embi_serialize_compound(embi_sb *sb, const embi_value *v);

/* ------------------------------------------------------------------ shorthand expansion */

bool embi_grid_areas_valid(embi_arena *a, const embi_str *rows, uint32_t nrows);

/* Collects the longhand values of one shorthand while it is parsed. Unset longhands take their
 * initial values when the expansion is finished. */
typedef struct embi_sh {
    embi_arena *a;
    embi_prop_id prop;
    const uint16_t *subs;
    uint32_t nsub;
    embi_value val[EMBI_LONGHAND_MAX];
    bool set[EMBI_LONGHAND_MAX];
} embi_sh;

bool embi_sh_begin(embi_sh *sh, embi_arena *a, embi_prop_id prop);
void embi_sh_put(embi_sh *sh, embi_prop_id sub, const embi_value *v);
bool embi_sh_isset(const embi_sh *sh, embi_prop_id sub);
/* Fills unset longhands with initial values and writes the entries. Returns the count (0 on
 * failure). */
uint32_t embi_sh_finish(embi_sh *sh, embi_longhand *out);

/* Expansion of the shorthands with their own grammar (shorthand_ext.c). Sets *handled false when
 * prop is not one of them. */
uint32_t embi_sh_expand_ext(embi_arena *a, embi_prop_id prop, embi_css_slice value,
                            embi_longhand *out, bool *handled);

/* display's multi-keyword syntax (value_disp.c): the single keyword it maps to, false when invalid. */
bool embi_css_parse_display_multi(embi_css_slice s, uint16_t *kw);
/* The CSS text of an internal display keyword (the list-item forms without a single keyword), or nullptr. */
const char *embi_css_display_kw_text(uint16_t kw);
