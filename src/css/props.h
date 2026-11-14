#pragma once
/*
 * Property database, value parsing, colors, calc() and shorthand expansion (W2c). Internal to
 * Ember: the cascade (src/style, W2d) and CSSOM (W5) use these types.
 *
 * Entry points:
 *   - embi_prop_lookup maps a property name to its id. Custom properties (--x) are not in the
 *     table; the caller handles them.
 *   - embi_css_parse_decl parses a declaration value into longhands. A longhand gives one entry;
 *     a shorthand gives its longhands. CSS-wide keywords and var() are kept as their own kinds.
 *   - embi_prop_decl_ok is the W2a declaration hook (embi_css_hooks.decl_ok).
 *   - embi_color_*, embi_calc_* are the color and math layers, usable on their own.
 *
 * Values are arena allocated (they point into the arena passed to the parse). Colors and numbers
 * are stored in canonical units: lengths in px for absolute units, angles in deg, times in s, and
 * resolutions in dppx. Relative units stay as written, for layout to resolve.
 */
#include "base/base.h"
#include "css/css.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ value types (EMBI_T_*) */

#define EMBI_T_LEN     (1u << 0)
#define EMBI_T_PCT     (1u << 1)
#define EMBI_T_NUM     (1u << 2)
#define EMBI_T_INT     (1u << 3)
#define EMBI_T_ALPHA   (1u << 4) /* number or percentage, clamped to 0..1 */
#define EMBI_T_ANGLE   (1u << 5)
#define EMBI_T_TIME    (1u << 6)
#define EMBI_T_RES     (1u << 7)
#define EMBI_T_COLOR   (1u << 8)
#define EMBI_T_URL     (1u << 9)
#define EMBI_T_STR     (1u << 10)
#define EMBI_T_IDENT   (1u << 11)
#define EMBI_T_NONNEG  (1u << 12)
#define EMBI_T_LIST    (1u << 13) /* comma-separated list of the other types */
#define EMBI_T_POSINT  (1u << 14) /* integer >= 1 (with EMBI_T_INT) */
#define EMBI_T_LENPCT  (EMBI_T_LEN | EMBI_T_PCT)

/* Property flags. */
#define EMBI_PF_INHERITED  (1u << 0)
#define EMBI_PF_SHORTHAND  (1u << 1)

/* ------------------------------------------------------------------ property table */

typedef struct embi_name_id {
    const char *name;
    uint16_t id;
} embi_name_id;

/* Interpolation kinds (props.txt interp column): the computed value's type family, which decides
 * how two computed values mix (src/anim/interp.c). NONE is discrete. */
typedef enum embi_interp {
    EMBI_IP_NONE = 0, /* discrete: flips at 50% (or as the property's rules say) */
    EMBI_IP_LEN,      /* length, percentage, calc(), and number pairs (line-height) */
    EMBI_IP_NUM,      /* number or percentage */
    EMBI_IP_INT,      /* integer: mixed as a number, then rounded */
    EMBI_IP_COLOR,
    EMBI_IP_SHADOW,   /* box-shadow and text-shadow lists */
    EMBI_IP_FILTER,   /* filter lists */
    EMBI_IP_XFORM,    /* transform lists and the translate, rotate and scale properties */
    EMBI_IP_SHAPE,    /* basic shapes */
    EMBI_IP_POS,      /* two-axis positions */
    EMBI_IP_AXIS,     /* background-position-x and -y lists */
    EMBI_IP_LPLIST,   /* lists (space or comma) of lengths, numbers and keywords */
    EMBI_IP_VIS,      /* visibility: discrete with the visible end */
    EMBI_IP_DISP,     /* display: discrete, but the non-none end applies for 0 < p < 1 */
    EMBI_IP_CV,       /* content-visibility: discrete, but visible applies for 0 < p < 1 */
    EMBI_IP_RATIO,    /* <ratio>: mixed geometrically; auto is discrete */
} embi_interp;

typedef struct embi_prop_info {
    const char *name;
    const char *initial; /* CSS text of the initial value (nullptr for shorthands) */
    const char *group;   /* style struct group for W2d (nullptr when none) */
    uint32_t types;      /* EMBI_T_* */
    uint16_t kw_off, kw_n;   /* accepted keywords (EMBI_KW_*) in the flattened kw list */
    uint16_t sub_off, sub_n; /* shorthand: longhand ids in the flattened sub list */
    uint8_t flags;       /* EMBI_PF_* */
    uint8_t anim;        /* 'N' 'L' 'F' 'C' 'D' 'X' */
    uint8_t interp;      /* embi_interp */
    uint8_t grammar;     /* EMBI_G_*: the grammar function for compound values (0 = plain types) */
    uint8_t logic_family; /* EMBI_LF_*: logical property group (0 = none) */
    uint8_t logic_role;   /* index within the group, see embi_prop_logical_to_physical */
    uint8_t logic_logical; /* 1 for a flow-relative property, 0 for a physical one */
} embi_prop_info;

/* The generated enums EMBI_PROP_* and EMBI_KW_*, and (in one unit only, with EMBI_PROPS_TABLES
 * defined) the tables. */
#include "css/props.inc"

typedef uint16_t embi_prop_id;

/* ASCII case-insensitive lookup. Returns 0 (EMBI_PROP_NONE) for an unknown name. */
embi_prop_id embi_prop_lookup(embi_str name);
/* nullptr for EMBI_PROP_NONE or an out-of-range id. */
const embi_prop_info *embi_prop_info_of(embi_prop_id id);
/* The longhand ids of a shorthand, in expansion order (sub_n entries). nullptr for unknown ids. */
const uint16_t *embi_prop_longhands(embi_prop_id id);
static inline bool embi_prop_is_shorthand(embi_prop_id id)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    return pi != nullptr && (pi->flags & EMBI_PF_SHORTHAND) != 0;
}
static inline bool embi_prop_is_inherited(embi_prop_id id)
{
    const embi_prop_info *pi = embi_prop_info_of(id);
    return pi != nullptr && (pi->flags & EMBI_PF_INHERITED) != 0;
}
/* ASCII case-insensitive keyword lookup for keyword names (any keyword, not per property).
 * Returns EMBI_KW_INVALID (0) when unknown. */
uint16_t embi_kw_lookup(embi_str name);
/* The keyword's name, or nullptr for 0 or an out-of-range id. */
const char *embi_kw_name(uint16_t kw);

/* ------------------------------------------------------------------ units */

/* The CSS spelling of a unit (empty for EMBI_U_NONE). */
const char *embi_unit_name(uint8_t unit);
/* Writes a number with at most six decimals and no trailing zeros (serialization). */
void embi_css_append_num(embi_sb *sb, double v);
/* Serializes one component value back to CSS text (no escaping). */
void embi_css_serialize_cv(embi_sb *sb, const embi_css_cv *cv);

typedef enum embi_unit {
    EMBI_U_NONE = 0,
    /* Length. Absolute units are stored as px. */
    EMBI_U_PX,
    EMBI_U_EM, EMBI_U_REM, EMBI_U_EX, EMBI_U_REX, EMBI_U_CAP, EMBI_U_RCAP, EMBI_U_CH,
    EMBI_U_RCH, EMBI_U_IC, EMBI_U_RIC, EMBI_U_LH, EMBI_U_RLH,
    EMBI_U_VW, EMBI_U_VH, EMBI_U_VI, EMBI_U_VB, EMBI_U_VMIN, EMBI_U_VMAX,
    EMBI_U_SVW, EMBI_U_SVH, EMBI_U_SVI, EMBI_U_SVB, EMBI_U_SVMIN, EMBI_U_SVMAX,
    EMBI_U_LVW, EMBI_U_LVH, EMBI_U_LVI, EMBI_U_LVB, EMBI_U_LVMIN, EMBI_U_LVMAX,
    EMBI_U_DVW, EMBI_U_DVH, EMBI_U_DVI, EMBI_U_DVB, EMBI_U_DVMIN, EMBI_U_DVMAX,
    EMBI_U_CQW, EMBI_U_CQH, EMBI_U_CQI, EMBI_U_CQB, EMBI_U_CQMIN, EMBI_U_CQMAX,
    /* Other classes: angle in deg, time in s, resolution in dppx. */
    EMBI_U_DEG, EMBI_U_S, EMBI_U_DPPX,
    EMBI_U_COUNT
} embi_unit;

/* Dimension classes (bit masks), used by calc() type checking. NUM is a plain number. */
#define EMBI_CD_NUM    (1u << 0)
#define EMBI_CD_LEN    (1u << 1)
#define EMBI_CD_PCT    (1u << 2)
#define EMBI_CD_ANGLE  (1u << 3)
#define EMBI_CD_TIME   (1u << 4)
#define EMBI_CD_RES    (1u << 5)
#define EMBI_CD_LENPCT (EMBI_CD_LEN | EMBI_CD_PCT)

/* Looks up a dimension unit (ASCII case-insensitive). On success sets *unit, *cls (one of
 * EMBI_CD_LEN, EMBI_CD_ANGLE, EMBI_CD_TIME, EMBI_CD_RES) and *scale, the factor from the written
 * unit to the canonical one. Returns false for unknown units. */
bool embi_unit_lookup(embi_str s, uint8_t *unit, uint8_t *cls, double *scale);

/* ------------------------------------------------------------------ colors */

typedef enum embi_cspace {
    EMBI_CS_SRGB,         /* legacy rgb() when not EMBI_CF_FUNC, color(srgb) when it is */
    EMBI_CS_SRGB_LINEAR,
    EMBI_CS_DISPLAY_P3,
    EMBI_CS_A98_RGB,
    EMBI_CS_PROPHOTO_RGB,
    EMBI_CS_REC2020,
    EMBI_CS_XYZ_D50,
    EMBI_CS_XYZ_D65,
    EMBI_CS_LAB,
    EMBI_CS_LCH,
    EMBI_CS_OKLAB,
    EMBI_CS_OKLCH,
    EMBI_CS_DEVICE_CMYK,
    EMBI_CS_CURRENT,      /* currentcolor: resolved by the cascade */
    EMBI_CS_SYSTEM,       /* a system color keyword: sys is the index into embi_system_color_names */
    EMBI_CS_DEFERRED,     /* a relative color or color-mix() that reads currentcolor: its source text
                           * is an atom (in alpha_k); embi_color_resolve_deferred evaluates it with
                           * the used color (CSS Color 5 section 4: the value depends on color) */
    EMBI_CS_DISPLAY_P3_LINEAR, /* color(display-p3-linear ...): Display P3 primaries, no transfer curve */
} embi_cspace;

#define EMBI_CF_FUNC (1u << 0) /* written as color(...) */
/* Component i of rgb(), lab(), lch(), oklab() or oklch() was written as none (EMBI_CF_NONE0 << i): c[i] holds 0, and
 * interpolation (gradients) treats it as missing (CSS Color 4 section 4.4). */
#define EMBI_CF_NONE0 (1u << 1)
#define EMBI_CF_NONE_ALL (7u << 1)

typedef struct embi_color {
    uint8_t space;   /* embi_cspace */
    uint8_t flags;   /* EMBI_CF_* */
    uint16_t sys;    /* SYSTEM: index into the system color table */
    double c[3];     /* components in the space's own units (rgb: 0..1 scale; lab: L a b; lch:
                      * L C H(deg); oklch: L C H(deg); xyz: 0..1; device-cmyk: c m y k in c[0..2]
                      * and the black channel in alpha_k) */
    double alpha_k; /* DEVICE_CMYK: the k component. */
    double alpha;    /* 0..1 */
} embi_color;

/* Parses a single color component (hex, named, system, currentcolor, or a color function).
 * cv is one component value. */
bool embi_color_parse_cv(const embi_css_cv *cv, embi_color *out);
/* Parses a whole color value from a slice (leading and trailing whitespace allowed). */
bool embi_color_parse(embi_css_slice in, embi_color *out);
/* Writes the CSS serialization: rgb()/rgba() for sRGB, color(...) for FUNC forms, lab(),
 * lch(), oklab(), oklch(), color(device-cmyk ...), currentcolor and system color names. */
void embi_color_serialize(embi_sb *sb, const embi_color *c);
/* Converts to the sRGB space (0..1 components) with CSS Color 4 gamut mapping for colors outside
 * the sRGB gamut (lightness and hue kept, chroma reduced in OKLCH). */
bool embi_color_to_srgb(const embi_color *in, embi_color *out);
/* Converts for painting: gamut-mapped sRGB as 8-bit channels (rounded), alpha last. */
bool embi_color_to_srgb8(const embi_color *in, uint8_t rgba[4]);

/* Evaluates a EMBI_CS_DEFERRED color against the color that currentcolor stands for (which must
 * be concrete: not CURRENT, SYSTEM or DEFERRED). Returns false when it does not evaluate. */
bool embi_color_resolve_deferred(const embi_color *c, const embi_color *current, embi_color *out);

/* Color interpolation (CSS Color 4 section 12; gradients). A mixer holds two colors converted into the
 * interpolation space (embi_interp_space, embi_hue_method). Missing components (NaN) take the other color's value
 * and the hue of a gray is ignored. embi_color_mixer_at writes the color at weight w (0: a, 1: b) in the mix space,
 * alpha included; convert it with embi_color_to_srgb8. */
typedef struct embi_color_mixer embi_color_mixer;
bool embi_color_mixer_init(embi_color_mixer **out, const embi_color *a, const embi_color *b, uint8_t interp,
                           uint8_t hue);
void embi_color_mixer_at(const embi_color_mixer *mx, double w, embi_color *out);
void embi_color_mixer_free(embi_color_mixer *mx);

/* Named color lookup (CSS named colors, transparent, rebeccapurple). */
bool embi_color_named(embi_str name, embi_color *out);

/* System colors: names are in embi_system_color_names (sorted by insertion order); the index is
 * stored in embi_color.sys. Returns false for other names. */
bool embi_color_system(embi_str name, uint16_t *index);
extern const char *const embi_system_color_names[];
extern const uint32_t embi_system_color_count;
/* The number of system color names; the default palettes and any embedder palette passed to
 * embi_color_system_value must have at least this many entries (static_assert'd in color.c and
 * value_var.c). */
#define EMBI_SYSTEM_COLOR_COUNT 19u

/* ------------------------------------------------------------------ calc() */

typedef enum embi_calc_op {
    EMBI_CALC_LEAF,
    EMBI_CALC_NEG,
    EMBI_CALC_ADD,
    EMBI_CALC_SUB,
    EMBI_CALC_MUL,
    EMBI_CALC_DIV,
    EMBI_CALC_MIN,
    EMBI_CALC_MAX,
    EMBI_CALC_CLAMP,
    EMBI_CALC_ROUND,
    EMBI_CALC_MOD,
    EMBI_CALC_REM,
    EMBI_CALC_SIN,
    EMBI_CALC_COS,
    EMBI_CALC_TAN,
    EMBI_CALC_ASIN,
    EMBI_CALC_ACOS,
    EMBI_CALC_ATAN,
    EMBI_CALC_ATAN2,
    EMBI_CALC_POW,
    EMBI_CALC_SQRT,
    EMBI_CALC_HYPOT,
    EMBI_CALC_LOG,
    EMBI_CALC_EXP,
    EMBI_CALC_ABS,
    EMBI_CALC_SIGN,
    EMBI_CALC_SIZE,   /* the `size` keyword of calc-size() (CSS Values 5 section 10.1): a leaf
                       * whose value is the basis size, given in env->size */
} embi_calc_op;

typedef enum embi_round_mode {
    EMBI_ROUND_NEAREST,
    EMBI_ROUND_UP,
    EMBI_ROUND_DOWN,
    EMBI_ROUND_TO_ZERO,
} embi_round_mode;

typedef struct embi_calc embi_calc;
struct embi_calc {
    uint8_t op;          /* embi_calc_op */
    uint8_t type;        /* EMBI_CD_* mask of the node's dimension (LENPCT: mixed length and
                          * percentage) */
    uint8_t unit;        /* LEAF: embi_unit (LEN: a length unit, NUM and PCT: EMBI_U_NONE) */
    uint8_t mode;        /* ROUND: embi_round_mode */
    double num;          /* LEAF: value (percentages hold the written number, 50 for 50%) */
    uint32_t n;          /* child count */
    const embi_calc *const *kids;
};

/* Parses a math function (calc, min, max, clamp, round, mod, rem, sin, cos, tan, asin, acos,
 * atan, atan2, pow, sqrt, hypot, log, exp, abs, sign) from a FUNCTION component value. Type
 * checks the expression (sums need equal dimensions, except length and percentage which may mix;
 * products need at most one non-number; and so on). Returns false when invalid. *type_out gets the
 * root dimension mask. */
bool embi_calc_parse_cv(embi_arena *a, const embi_css_cv *cv, const embi_calc **out,
                        uint8_t *type_out);

typedef struct embi_calc_env {
    double pct_base;                                         /* the value of 100% (px) */
    double (*len_px)(void *user, uint8_t unit, double value); /* relative lengths to px */
    void *user;
} embi_calc_env;

/* Evaluates a node. Returns NaN when a relative unit cannot be resolved (len_px nullptr) and for
 * domain errors that the spec turns into NaN. */
double embi_calc_eval(const embi_calc *c, const embi_calc_env *env);
/* Evaluates like embi_calc_eval, with the `size` keyword (SIZE leaves) standing for size (px). A NaN
 * size leaves `size` unresolved (NaN). */
double embi_calc_eval_sized(const embi_calc *c, const embi_calc_env *env, double size);
/* Parses the calc-sum in p[0..n) of a calc-size() expression: like the math functions, but the
 * `size` keyword is allowed. The type must be a length, a percentage or both. */
bool embi_calc_parse_sized(embi_arena *a, const embi_css_cv *p, uint32_t n, const embi_calc **out,
                           uint8_t *type_out);
/* Writes the CSS serialization of a calc node. With bare false, a sum or product is wrapped in
 * calc(); with bare true (the second argument of calc-size()) it is written as it is. */
void embi_calc_serialize(embi_sb *sb, const embi_calc *c, bool bare);
/* True when the expression has no percentage and no relative length leaves, so it reduces to a
 * constant (numbers, absolute lengths in px, angles in deg, times in s, resolutions in dppx). */
bool embi_calc_is_constant(const embi_calc *c);
/* Reduces a constant expression to a single leaf value in *out (see EMBI_CALC_LEAF). Returns
 * false if the expression is not constant. */
bool embi_calc_reduce(embi_arena *a, const embi_calc *c, double *value);

/* ------------------------------------------------------------------ values */

typedef enum embi_vkind {
    EMBI_V_NONE = 0,
    EMBI_V_KEYWORD,      /* kw */
    EMBI_V_NUMBER,       /* num (alpha values already clamped) */
    EMBI_V_INTEGER,      /* num holds an integer */
    EMBI_V_PERCENT,      /* num: the written percentage (50 for 50%) */
    EMBI_V_LENGTH,       /* num with unit (EMBI_U_PX or a relative unit) */
    EMBI_V_ANGLE,        /* num in deg */
    EMBI_V_TIME,         /* num in s */
    EMBI_V_RESOLUTION,   /* num in dppx */
    EMBI_V_CALC,         /* calc */
    EMBI_V_COLOR,        /* color */
    EMBI_V_STRING,       /* str */
    EMBI_V_URL,          /* str */
    EMBI_V_IDENT,        /* ident */
    EMBI_V_LIST,         /* list (comma or space separated, see sep) */
    EMBI_V_LIGHT_DARK,   /* pair: two COLOR values (light, dark) */
    EMBI_V_RAW,          /* raw: a <declaration-value> kept as component values. Only the
                          * universal registered syntax "*" and the fallback of attr() produce it. */
    EMBI_V_VAR,          /* raw: the declaration contains var(), substituted at computed time */
    EMBI_V_INITIAL,      /* CSS-wide keywords */
    EMBI_V_INHERIT,
    EMBI_V_UNSET,
    EMBI_V_REVERT,
    EMBI_V_REVERT_LAYER,
    EMBI_V_RATIO,        /* ratio: <ratio> a / b */
    EMBI_V_FUNC,         /* func: counter(), counters(), attr(), reversed(), styleset(), ... */
    EMBI_V_IMAGE,        /* image: url(), gradients, image-set(), cross-fade() */
    EMBI_V_POSITION,     /* pos: a two-axis <position> (transform-origin also fills z) */
    EMBI_V_POS_AXIS,     /* axis: one axis of a position (background-position-x/-y) */
    EMBI_V_TRANSFORM,    /* xf: a <transform-list> (also translate, rotate and scale) */
    EMBI_V_SHADOWS,      /* shadows: a comma list of box-shadow / text-shadow layers */
    EMBI_V_FILTERS,      /* filters: a <filter-value-list> */
    EMBI_V_SHAPE,        /* shape: a <basic-shape> and/or a <geometry-box> (kw) */
    EMBI_V_EASING,       /* easing: cubic-bezier(), steps() or linear() */
    EMBI_V_TRACKS,       /* tracks: a grid track list (or subgrid) */
    EMBI_V_GRID_LINE,    /* gline: a <grid-line> */
} embi_vkind;

/* LIST separators. A list that has exactly one keyword item is stored as that keyword. Use
 * embi_value_items to read a value as a list either way. */
#define EMBI_SEP_COMMA 0
#define EMBI_SEP_SPACE 1

typedef struct embi_value embi_value;

typedef struct embi_pos_axis embi_pos_axis;
typedef struct embi_position embi_position;
typedef struct embi_image embi_image;
typedef struct embi_xform embi_xform;
typedef struct embi_shadow embi_shadow;
typedef struct embi_filter embi_filter;
typedef struct embi_shape embi_shape;
typedef struct embi_easing embi_easing;
typedef struct embi_grid_tracks embi_grid_tracks;
typedef struct embi_grid_line embi_grid_line;

struct embi_value {
    uint8_t kind;   /* embi_vkind */
    uint8_t unit;   /* embi_unit for LENGTH, ANGLE, TIME, RESOLUTION */
    uint8_t sep;    /* LIST: EMBI_SEP_* */
    uint8_t aux;    /* kind specific */
    uint16_t kw;    /* KEYWORD: EMBI_KW_*. SHAPE: the geometry box keyword, or 0. */
    union {
        double num;
        const embi_calc *calc;
        embi_color color;
        embi_str str;
        embi_atom ident;
        embi_css_slice raw;
        struct {
            const embi_value *items;
            uint32_t n;
        } list;
        const embi_value *pair;
        struct {
            double a, b; /* a / b; b is 1 when the denominator was omitted */
        } ratio;
        struct {
            embi_atom name;
            const embi_value *args;
            uint32_t n;
        } func;
        const embi_image *image;
        const embi_position *pos;
        const embi_pos_axis *axis;
        struct {
            const embi_xform *p;
            uint32_t n;
        } xf;
        struct {
            const embi_shadow *p;
            uint32_t n;
        } shadows;
        struct {
            const embi_filter *p;
            uint32_t n;
        } filters;
        const embi_shape *shape;
        const embi_easing *easing;
        const embi_grid_tracks *tracks;
        const embi_grid_line *gline;
    };
};

/* ---- compound value structs. All are arena allocated; embedded embi_value members are LENGTH,
 * PERCENT, NUMBER, ANGLE or CALC values unless noted. */

/* One axis of a <position>. anchor 0: off is measured from the left/top edge. anchor 1: off is
 * measured from the right/bottom edge (`right 10px`). Plain keywords are normalized:
 * left/top = 0%, center = 50%, right/bottom = 100%, all anchored at the start. */
struct embi_pos_axis {
    uint8_t anchor;
    embi_value off;
};
struct embi_position {
    embi_pos_axis x, y;
    embi_value z; /* transform-origin only: a LENGTH, or kind NONE when absent */
};

/* Color interpolation methods (gradients; the same spaces as color-mix()). */
typedef enum embi_interp_space {
    EMBI_IS_DEFAULT = 0, /* no `in <space>` given */
    EMBI_IS_SRGB, EMBI_IS_SRGB_LINEAR, EMBI_IS_DISPLAY_P3, EMBI_IS_A98_RGB, EMBI_IS_PROPHOTO_RGB,
    EMBI_IS_REC2020, EMBI_IS_LAB, EMBI_IS_OKLAB, EMBI_IS_XYZ_D50, EMBI_IS_XYZ_D65,
    EMBI_IS_HSL, EMBI_IS_HWB, EMBI_IS_LCH, EMBI_IS_OKLCH, EMBI_IS_DISPLAY_P3_LINEAR,
} embi_interp_space;
typedef enum embi_hue_method {
    EMBI_HUE_SHORTER = 0, EMBI_HUE_LONGER, EMBI_HUE_INCREASING, EMBI_HUE_DECREASING,
} embi_hue_method;

typedef struct embi_color_stop {
    embi_value color;  /* COLOR or LIGHT_DARK; kind NONE for a color hint */
    embi_value pos[2]; /* LENGTH/PERCENT/CALC (ANGLE/PERCENT for conic); kind NONE when absent.
                        * A hint has its position in pos[0]. */
} embi_color_stop;

typedef enum embi_grad_kind { EMBI_GRAD_LINEAR = 1, EMBI_GRAD_RADIAL, EMBI_GRAD_CONIC } embi_grad_kind;
#define EMBI_SIDE_TOP    1u /* linear `to <side-or-corner>` */
#define EMBI_SIDE_RIGHT  2u
#define EMBI_SIDE_BOTTOM 4u
#define EMBI_SIDE_LEFT   8u
typedef enum embi_radial_size {
    EMBI_RS_FARTHEST_CORNER = 0, /* the default */
    EMBI_RS_CLOSEST_CORNER, EMBI_RS_CLOSEST_SIDE, EMBI_RS_FARTHEST_SIDE,
    EMBI_RS_EXPLICIT, /* radius[0] (circle) or radius[0], radius[1] (ellipse) */
} embi_radial_size;

typedef struct embi_gradient {
    uint8_t kind;      /* embi_grad_kind */
    bool repeating;
    bool ellipse;      /* radial: false is a circle */
    uint8_t size;      /* radial: embi_radial_size */
    uint8_t sides;     /* linear: EMBI_SIDE_* bits of `to ...`; 0 when an angle is used */
    bool has_pos;      /* radial, conic: `at <position>` written */
    bool has_interp;   /* `in <space>` written */
    uint8_t interp;    /* embi_interp_space */
    uint8_t hue;       /* embi_hue_method */
    embi_value angle;  /* linear: ANGLE (kind NONE: 180deg or `sides`). conic: `from` angle */
    embi_value radius[2];
    embi_position pos; /* radial, conic: default 50% 50% */
    uint32_t nstops;
    const embi_color_stop *stops;
} embi_gradient;

typedef enum embi_image_kind {
    EMBI_IMG_URL = 1, EMBI_IMG_GRADIENT, EMBI_IMG_IMAGE_SET, EMBI_IMG_CROSS_FADE, EMBI_IMG_COLOR, EMBI_IMG_FALLBACKS,
} embi_image_kind;
typedef struct embi_image_opt {
    const embi_image *img; /* a url() or string option is an EMBI_IMG_URL image */
    double dppx;           /* 1 when omitted */
    embi_str type;         /* type("...") or empty */
} embi_image_opt;
typedef struct embi_xfade_item {
    const embi_image *img; /* nullptr when the item is a color */
    embi_value color;
    double pct;            /* -1 when omitted */
} embi_xfade_item;
struct embi_image {
    uint8_t kind;               /* embi_image_kind */
    embi_str url;               /* URL */
    const embi_gradient *grad;  /* GRADIENT */
    const embi_image_opt *opts; /* IMAGE_SET */
    uint32_t n;                 /* IMAGE_SET, CROSS_FADE: item count */
    const embi_xfade_item *xf;  /* CROSS_FADE */
    const struct embi_image *fb; /* FALLBACKS: the color image painted when no option loads (or nullptr) */
};

typedef enum embi_xform_fn {
    EMBI_XF_MATRIX = 1, EMBI_XF_MATRIX3D,
    EMBI_XF_TRANSLATE, EMBI_XF_TRANSLATEX, EMBI_XF_TRANSLATEY, EMBI_XF_TRANSLATEZ,
    EMBI_XF_TRANSLATE3D,
    EMBI_XF_SCALE, EMBI_XF_SCALEX, EMBI_XF_SCALEY, EMBI_XF_SCALEZ, EMBI_XF_SCALE3D,
    EMBI_XF_ROTATE, EMBI_XF_ROTATEX, EMBI_XF_ROTATEY, EMBI_XF_ROTATEZ, EMBI_XF_ROTATE3D,
    EMBI_XF_SKEW, EMBI_XF_SKEWX, EMBI_XF_SKEWY, EMBI_XF_PERSPECTIVE,
} embi_xform_fn;
/* args hold the written arguments: translate takes LENGTH/PERCENT/CALC, scale NUMBER (percentages
 * are divided by 100), rotate/skew ANGLE, matrix NUMBER, perspective LENGTH (or a KEYWORD none).
 * n is the number written. The individual properties `translate`, `rotate` and `scale` store one
 * function with every argument filled in: TRANSLATE3D (x y z), SCALE3D (x y z), and ROTATE3D
 * (x y z angle). */
struct embi_xform {
    uint8_t fn; /* embi_xform_fn */
    uint8_t n;
    const embi_value *args;
};

struct embi_shadow {
    embi_value color; /* COLOR; currentcolor when omitted */
    embi_value x, y, blur, spread; /* LENGTH or CALC; blur and spread are 0px when omitted */
    bool inset;
};

typedef enum embi_filter_fn {
    EMBI_FILTER_BLUR = 1, EMBI_FILTER_BRIGHTNESS, EMBI_FILTER_CONTRAST, EMBI_FILTER_GRAYSCALE,
    EMBI_FILTER_HUE_ROTATE, EMBI_FILTER_INVERT, EMBI_FILTER_OPACITY, EMBI_FILTER_SATURATE,
    EMBI_FILTER_SEPIA, EMBI_FILTER_DROP_SHADOW, EMBI_FILTER_URL,
} embi_filter_fn;
struct embi_filter {
    uint8_t fn;                /* embi_filter_fn */
    embi_value arg;            /* BLUR: LENGTH. HUE_ROTATE: ANGLE. URL: URL. Others: NUMBER (a
                                * percentage is divided by 100); the omitted default is filled in */
    const embi_shadow *shadow; /* DROP_SHADOW (inset is false, spread is 0) */
};

typedef enum embi_easing_kind { EMBI_EASE_CUBIC = 1, EMBI_EASE_STEPS, EMBI_EASE_LINEAR_FN } embi_easing_kind;
typedef enum embi_step_jump {
    EMBI_JUMP_END = 0, EMBI_JUMP_START, EMBI_JUMP_NONE, EMBI_JUMP_BOTH,
} embi_step_jump;
typedef struct embi_easing_pt {
    double in;  /* input progress, normalized (non-decreasing, first/last filled in) */
    double out;
} embi_easing_pt;
struct embi_easing {
    uint8_t kind;  /* embi_easing_kind */
    uint8_t jump;  /* STEPS: embi_step_jump */
    uint32_t steps; /* STEPS */
    double c[4];   /* CUBIC: x1 y1 x2 y2 */
    uint32_t npts; /* LINEAR_FN */
    const embi_easing_pt *pts;
};

typedef enum embi_shape_kind {
    EMBI_SHAPE_INSET = 1, EMBI_SHAPE_CIRCLE, EMBI_SHAPE_ELLIPSE, EMBI_SHAPE_POLYGON,
    EMBI_SHAPE_PATH, EMBI_SHAPE_XYWH, EMBI_SHAPE_RECT, EMBI_SHAPE_SHAPE,
} embi_shape_kind;
/* A command of shape() (CSS Shapes 2). Points are pos_axis pairs: an absolute `to` point is a position (keywords and edge
 * offsets allowed), a relative `by` point holds only the offset in .off. */
typedef enum embi_shape_op {
    EMBI_SOP_MOVE = 1, EMBI_SOP_LINE, EMBI_SOP_HLINE, EMBI_SOP_VLINE, EMBI_SOP_CURVE, EMBI_SOP_SMOOTH, EMBI_SOP_ARC,
    EMBI_SOP_CLOSE,
} embi_shape_op;
typedef struct embi_shape_cmd {
    uint8_t op;     /* embi_shape_op */
    bool rel;       /* `by` (relative to the current point) instead of `to` */
    uint8_t ncp;    /* CURVE: 1 (quadratic) or 2 (cubic). SMOOTH: 0 or 1 control points written */
    bool large, cw; /* ARC */
    uint8_t nr;     /* ARC: radii written (1 or 2) */
    embi_pos_axis p[2];    /* the end point (HLINE: p[0] is x, VLINE: p[0] is y) */
    embi_pos_axis c[2][2]; /* control points (x, y each); relative ones are relative to the start of the command */
    embi_value r[2];       /* ARC radii: LENGTH/PERCENT */
    double rot;            /* ARC rotation in deg */
} embi_shape_cmd;
struct embi_shape {
    uint8_t kind;      /* embi_shape_kind */
    bool evenodd;      /* POLYGON, PATH fill rule */
    bool has_round;    /* INSET, XYWH, RECT: border-radius present */
    bool has_at;       /* CIRCLE, ELLIPSE: `at <position>` written */
    embi_value box[4]; /* INSET, RECT: top right bottom left (RECT: LENGTH/PERCENT or KEYWORD
                        * auto). XYWH: x y width height */
    embi_value radii[8]; /* has_round: horizontal radii TL TR BR BL, then vertical radii */
    embi_value r[2];   /* CIRCLE: r[0]. ELLIPSE: rx, ry. LENGTH/PERCENT or KEYWORD closest-side
                        * / farthest-side (the default is closest-side) */
    embi_position at;
    const embi_value *pts; /* POLYGON: x0 y0 x1 y1 ... */
    uint32_t npts;         /* number of vertices */
    embi_str path;         /* PATH: the SVG path data (validated) */
    embi_position from;    /* SHAPE: the start point (absolute) */
    const embi_shape_cmd *cmds; /* SHAPE: the commands after `from` */
    uint32_t ncmds;
};

typedef enum embi_track_breadth_kind {
    EMBI_TB_LP = 1, EMBI_TB_FR, EMBI_TB_AUTO, EMBI_TB_MIN_CONTENT, EMBI_TB_MAX_CONTENT,
} embi_track_breadth_kind;
typedef struct embi_track_breadth {
    uint8_t kind; /* embi_track_breadth_kind */
    embi_value v; /* LP: LENGTH/PERCENT/CALC. FR: NUMBER */
} embi_track_breadth;
typedef enum embi_track_size_kind {
    EMBI_TS_BREADTH = 1, EMBI_TS_MINMAX, EMBI_TS_FIT_CONTENT,
} embi_track_size_kind;
typedef struct embi_track_size {
    uint8_t kind;           /* embi_track_size_kind */
    embi_track_breadth min; /* BREADTH: the breadth (max is the same). MINMAX: the minimum. */
    embi_track_breadth max; /* MINMAX: the maximum. FIT_CONTENT: the limit (an LP). */
} embi_track_size;

typedef enum embi_grid_item_kind { EMBI_GI_NAMES = 1, EMBI_GI_TRACK, EMBI_GI_REPEAT } embi_grid_item_kind;
typedef enum embi_grid_repeat { EMBI_GR_COUNT = 0, EMBI_GR_AUTO_FILL, EMBI_GR_AUTO_FIT } embi_grid_repeat;
typedef struct embi_grid_item embi_grid_item;
struct embi_grid_item {
    uint8_t kind;                /* embi_grid_item_kind */
    uint8_t rep;                 /* REPEAT: embi_grid_repeat */
    uint32_t count;              /* REPEAT with EMBI_GR_COUNT: the repetition count */
    uint32_t n;                  /* NAMES: number of names. REPEAT: number of items */
    const embi_atom *names;      /* NAMES: line names of one [ ... ] block */
    embi_track_size size;        /* TRACK */
    const embi_grid_item *items; /* REPEAT: names and tracks (no nested repeats) */
};
/* A track list is a flat sequence of NAMES, TRACK and REPEAT items. Adjacent NAMES blocks are
 * merged. `subgrid`: only NAMES items and REPEATs of names are present. */
struct embi_grid_tracks {
    bool subgrid;
    bool has_auto_repeat; /* contains an auto-fill / auto-fit repeat */
    uint32_t n;
    const embi_grid_item *items;
};

/* grid-row-start and friends: auto is the keyword auto. Otherwise `span` (span true), an integer
 * (num, nonzero) and/or a line name (name, 0 when absent). */
struct embi_grid_line {
    bool span;
    int32_t num; /* 0 when absent */
    embi_atom name;
};

/* Reads a value as a list: a LIST gives its items, anything else is a list of one (itself). */
static inline const embi_value *embi_value_items(const embi_value *v, uint32_t *n)
{
    if (v->kind == EMBI_V_LIST) {
        *n = v->list.n;
        return v->list.items;
    }
    *n = 1;
    return v;
}

/* A longhand produced by parsing a declaration. */
typedef struct embi_longhand {
    embi_prop_id prop;
    embi_value value;
} embi_longhand;

/* The most longhands any shorthand expands to (font: 18, border: 17). */
#define EMBI_LONGHAND_MAX 32

/* Parses a declaration value. A longhand writes one entry, a shorthand writes its longhands.
 * Returns the number of entries written, or 0 when the value is invalid (also when cap is too
 * small). Values are allocated in a. */
uint32_t embi_css_parse_decl(embi_arena *a, embi_prop_id prop, embi_css_slice value,
                             embi_longhand *out, uint32_t cap);

/* Parses one component value for a longhand (no CSS-wide keywords, no var(), no lists). Used by
 * the shorthand expansion. */
bool embi_value_parse_one(embi_arena *a, embi_prop_id prop, const embi_css_cv *cv,
                          embi_value *out);
/* Parses the property's initial value (its CSS text in the property table). */
bool embi_value_initial(embi_arena *a, embi_prop_id prop, embi_value *out);
/* True when the component values contain a var() function at any depth. */
bool embi_css_has_var(embi_css_slice in);
/* True when the component values contain an attr() function at any depth. */
bool embi_css_has_attr(embi_css_slice in);
/* True when the value is non-empty and every component value is well formed (no bad strings or
 * urls, no unmatched closing tokens). This is the <declaration-value> check of the universal
 * registered syntax "*"; no property uses it. */
bool embi_css_value_opaque_ok(embi_css_slice s);

/* Parses a whole longhand value (not a shorthand, no CSS-wide keywords, no var()) from component
 * values: typed grammars, keywords and lists, exactly as embi_css_parse_decl does for a longhand.
 * Used by the shorthand expansion for pieces such as the font-family list. */
bool embi_value_parse_longhand(embi_arena *a, embi_prop_id prop, embi_css_slice s, embi_value *out);

/* The W2a declaration hook: true when the declaration is valid for its property. Custom
 * properties (--x) are always valid. Unknown names are invalid. Shorthand and longhand values are
 * validated by parsing them, so `user` is ignored. */
/* The `all` shorthand takes only a CSS-wide keyword: its EMBI_V_* kind, or 0 for an invalid value. */
int embi_css_all_keyword(embi_css_slice value);
bool embi_prop_decl_ok(void *user, embi_str name, embi_css_slice value, bool important);

/* Parses one component value against a set of EMBI_T_* types (no keywords other than the typed
 * ones, no lists). Used by the registered-syntax matcher. */
bool embi_value_parse_types(embi_arena *a, uint32_t types, const embi_css_cv *cv,
                            embi_value *out);

/* ------------------------------------------------------------------ var() and computed values */

/* Looks up a custom property's declared value (the token stream after the colon, trimmed or
 * not). Returns false when the property is not set. */
typedef struct embi_var_env {
    void *user;
    bool (*lookup)(void *user, embi_str name, embi_css_slice *value);
    uint32_t max_tokens; /* substitution limit in emitted component values; 0 = default 65536 */
    /* attr() (CSS Values 5 section 7.1): the value of the element's attribute `name`; ns is the
     * namespace prefix of an `ns|name` reference (empty ptr and len when there is none). nullptr:
     * no attribute is ever found. */
    bool (*attr)(void *user, embi_str ns, embi_str name, embi_str *value);
    /* sibling-index() and sibling-count() (CSS Values 5 section 8.3): the element's 1-based index among its siblings, or the
     * number of its siblings. nullptr: both are 1. */
    uint32_t (*sibling)(void *user, bool count);
} embi_var_env;

/* Substitutes every var() in value (CSS Variables 1). self names the custom property whose value
 * this is (nullptr-length for a declaration of an ordinary property), so a direct self reference
 * is a cycle. On success *out holds the substituted component values in a. Returns false when the
 * value is guaranteed-invalid at computed-value time: a missing variable with no fallback, a
 * cycle, or an expansion over the token limit. The caller then uses the inherited or initial
 * value. */
bool embi_css_substitute_vars(embi_arena *a, const embi_var_env *env, embi_str self,
                              embi_css_slice value, embi_css_slice *out);

/* Substitutes var() (when present) and parses the result as a declaration value. Returns 0 when
 * the value is invalid at computed-value time or does not parse (see embi_css_parse_decl). */
uint32_t embi_css_parse_decl_vars(embi_arena *a, embi_prop_id id, embi_css_slice value,
                                  const embi_var_env *env, embi_longhand *out, uint32_t cap);

/* Parses a component value against a registered custom property syntax (the value side of
 * @property, CSS Properties and Values 1). Supported: "*" (any well-formed value, including an
 * empty one; kept raw), and '|' alternatives of: <length>, <number>, <integer>, <percentage>,
 * <length-percentage>, <color>, <angle>, <time>, <resolution>, <url>, <string>, <image>,
 * <custom-ident>, <transform-function>, <transform-list>, and case-sensitive keywords. A data
 * type may carry a '+' (space-separated list) or '#' (comma-separated list) multiplier
 * (<transform-list> takes none). Returns false when the value does not match. */
bool embi_css_registered_parse(embi_arena *a, embi_str syntax, embi_css_slice value,
                               embi_value *out);

/* Font metrics of one font, in px, for the font-relative units that need the font itself. Layout
 * fills them in from the real font (x-height, cap height, advance of "0", advance of U+6C34,
 * the used line height). A metric that is zero, negative or NaN is "not available", and the
 * CSS Values 4 fallbacks apply: ex = 0.5em, ch = 0.5em, cap = 0.7em (the typical Latin cap
 * height; Values 4 allows any reasonable value when the font has none), ic = 1em, lh = 1.2em. */
typedef struct embi_font_metrics {
    double ex, cap, ch, ic, lh;
} embi_font_metrics;

/* Relative length context for the font-relative, viewport and container units (the W3 hook).
 * Units are converted to px. cq_w and cq_h are the content box of the nearest query container;
 * NaN when there is no container, in which case cq units use the viewport. em uses font_px and
 * rem uses root_px. ex, cap, ch, ic and lh use font_metrics; rex, rcap, rch, ric and rlh use
 * root_metrics (see embi_font_metrics for the fallbacks). A zeroed metrics struct means every
 * metric falls back. */
typedef struct embi_len_ctx {
    double font_px;
    double root_px;
    double vw, vh;
    double cq_w, cq_h;
    embi_font_metrics font_metrics;
    embi_font_metrics root_metrics;
} embi_len_ctx;

/* embi_calc_env.len_px callback (user is an embi_len_ctx). Returns NaN for an unknown unit. */
double embi_len_px_cb(void *user, uint8_t unit, double value);

/* A palette entry for a system color keyword. */
typedef struct embi_color_rgb8 {
    uint8_t r, g, b;
} embi_color_rgb8;

/* Resolves a system color (index into embi_system_color_names) to sRGB. palette is the
 * embedder's table, with embi_system_color_count entries; nullptr uses the built-in light or dark
 * default. Returns false for an out-of-range index. */
bool embi_color_system_value(uint16_t index, bool dark, const embi_color_rgb8 *palette,
                             embi_color *out);

/* Expands a shorthand. Used by embi_css_parse_decl; exposed for tests. */
uint32_t embi_css_expand_shorthand(embi_arena *a, embi_prop_id prop, embi_css_slice value,
                                   embi_longhand *out, uint32_t cap);

/* Serializes a value to CSS text (keywords, numbers, lengths, colors, idents, strings, urls,
 * lists, CSS-wide keywords). Calc and var() values serialize as their kind's placeholder text. */
void embi_value_serialize(embi_sb *sb, const embi_value *v);
/* CF1: true when x and y are the same value field by field (strings by content; for compound values other than lists and
 * url() images, the same parts), so that they serialize alike. False means only that this cheap test cannot tell: the
 * values may still be equal (compare their serializations). */
bool embi_value_identical(const embi_value *x, const embi_value *y);
/* True only when the serializations of x and y certainly differ (a number or color component that differs beyond the six decimals
 * of the serialization, a transform whose functions differ): the compare that avoids serializing both. False means undecided. */
bool embi_value_clearly_differs(const embi_value *x, const embi_value *y);
/* WPTASAN: *dst becomes a deep copy of src whose every part (lists, calc trees, strings, component values, the parts of
 * images, transforms, shadows, filters, shapes, easings and grid tracks) lives in a, so that it does not depend on the memory
 * src was built in. Atoms are shared (they live as long as the process). src and dst may be the same value. */
void embi_value_copy(embi_arena *a, const embi_value *src, embi_value *dst);
/* WPTASAN, with embi_arena_debug_poison on (tests): the number of v's parts (v itself included, at any depth) that lie in
 * poisoned memory; a poisoned part is not followed. */
uint32_t embi_value_debug_poisoned(const embi_value *v);

/* ------------------------------------------------------------------ logical properties */

/* Flow-relative (logical) properties are separate table entries that name the same physical
 * slots as their physical counterparts, depending on writing-mode and direction. The table in
 * props.txt groups them by family (margin, padding, inset, border-width, border-style,
 * border-color, size, min-size, max-size, border radius); this maps one onto the other.
 *
 * writing_mode is an EMBI_KW_ of the writing-mode property (horizontal_tb, vertical_rl,
 * vertical_lr, sideways_rl, sideways_lr); direction is EMBI_KW_ltr or EMBI_KW_rtl. */
bool embi_prop_is_logical(embi_prop_id id);
/* The physical property a logical one stands for under the given mode. A physical property (or an
 * id outside the logical families) maps to itself. */
embi_prop_id embi_prop_to_physical(embi_prop_id id, uint16_t writing_mode, uint16_t direction);
/* The logical property of the same family that stands for the physical one under the mode, or
 * EMBI_PROP_NONE when id is not physical or has no flow-relative equivalent. */
embi_prop_id embi_prop_to_logical(embi_prop_id id, uint16_t writing_mode, uint16_t direction);
