/*
 * Colors (CSS Color 4 and 5): hex, named and system colors, currentcolor, rgb()/hsl()/hwb() (legacy
 * and modern syntax), lab(), lch(), oklab(), oklch(), color(), device-cmyk() and color-mix().
 *
 * Storage: rgb-family colors are stored as sRGB components on the 0..1 scale. hsl() and hwb() are
 * converted to sRGB at parse time. The other spaces keep their own components. Serialization
 * follows the css-parsing-tests expectations: legacy rgb()/rgba() for sRGB, color(...) for the
 * color() forms, and lab()/lch()/oklab()/oklch() in their own spaces.
 *
 * color-mix() interpolates in the spaces listed in mix_spaces[] by way of the XYZ D65 hub. A space
 * name that is not in that table makes the whole color-mix() invalid (parse_color_mix returns
 * false); the XYZ D65 branches in the converters are only the explicit xyz / xyz-d65 spaces.
 * Lab and LCH use the CSS Color 4 D50 white (chromaticity 0.3457, 0.3585) and the Bradford
 * D65 <-> D50 adaptation matrices derived from it.
 */
#include "css/props.h"

#include <string.h>

/* ------------------------------------------------------------------ tables */

typedef struct named_color {
    const char *name;
    uint8_t r, g, b;
} named_color;

static const named_color named_colors[] = {
    {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215}, {"aqua", 0, 255, 255},
    {"aquamarine", 127, 255, 212}, {"azure", 240, 255, 255}, {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196}, {"black", 0, 0, 0}, {"blanchedalmond", 255, 235, 205},
    {"blue", 0, 0, 255}, {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160}, {"chartreuse", 127, 255, 0},
    {"chocolate", 210, 105, 30}, {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60}, {"cyan", 0, 255, 255},
    {"darkblue", 0, 0, 139}, {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0}, {"darkgrey", 169, 169, 169},
    {"darkkhaki", 189, 183, 107}, {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47},
    {"darkorange", 255, 140, 0}, {"darkorchid", 153, 50, 204}, {"darkred", 139, 0, 0},
    {"darksalmon", 233, 150, 122}, {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139},
    {"darkslategray", 47, 79, 79}, {"darkslategrey", 47, 79, 79}, {"darkturquoise", 0, 206, 209},
    {"darkviolet", 148, 0, 211}, {"deeppink", 255, 20, 147}, {"deepskyblue", 0, 191, 255},
    {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105}, {"dodgerblue", 30, 144, 255},
    {"firebrick", 178, 34, 34}, {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220}, {"ghostwhite", 248, 248, 255},
    {"gold", 255, 215, 0}, {"goldenrod", 218, 165, 32}, {"gray", 128, 128, 128},
    {"green", 0, 128, 0}, {"greenyellow", 173, 255, 47}, {"grey", 128, 128, 128},
    {"honeydew", 240, 255, 240}, {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92},
    {"indigo", 75, 0, 130}, {"ivory", 255, 255, 240}, {"khaki", 240, 230, 140},
    {"lavender", 230, 230, 250}, {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0},
    {"lemonchiffon", 255, 250, 205}, {"lightblue", 173, 216, 230}, {"lightcoral", 240, 128, 128},
    {"lightcyan", 224, 255, 255}, {"lightgoldenrodyellow", 250, 250, 210},
    {"lightgray", 211, 211, 211}, {"lightgreen", 144, 238, 144}, {"lightgrey", 211, 211, 211},
    {"lightpink", 255, 182, 193}, {"lightsalmon", 255, 160, 122}, {"lightseagreen", 32, 178, 170},
    {"lightskyblue", 135, 206, 250}, {"lightslategray", 119, 136, 153},
    {"lightslategrey", 119, 136, 153}, {"lightsteelblue", 176, 196, 222},
    {"lightyellow", 255, 255, 224}, {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50},
    {"linen", 250, 240, 230}, {"magenta", 255, 0, 255}, {"maroon", 128, 0, 0},
    {"mediumaquamarine", 102, 205, 170}, {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211},
    {"mediumpurple", 147, 112, 219}, {"mediumseagreen", 60, 179, 113},
    {"mediumslateblue", 123, 104, 238}, {"mediumspringgreen", 0, 250, 154},
    {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133},
    {"midnightblue", 25, 25, 112}, {"mintcream", 245, 255, 250}, {"mistyrose", 255, 228, 225},
    {"moccasin", 255, 228, 181}, {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128},
    {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0}, {"olivedrab", 107, 142, 35},
    {"orange", 255, 165, 0}, {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214},
    {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152},
    {"paleturquoise", 175, 238, 238}, {"palevioletred", 219, 112, 147},
    {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185}, {"peru", 205, 133, 63},
    {"pink", 255, 192, 203}, {"plum", 221, 160, 221}, {"powderblue", 176, 224, 230},
    {"purple", 128, 0, 128}, {"rebeccapurple", 102, 51, 153}, {"red", 255, 0, 0},
    {"rosybrown", 188, 143, 143}, {"royalblue", 65, 105, 225}, {"saddlebrown", 139, 69, 19},
    {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96}, {"seagreen", 46, 139, 87},
    {"seashell", 255, 245, 238}, {"sienna", 160, 82, 45}, {"silver", 192, 192, 192},
    {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205}, {"slategray", 112, 128, 144},
    {"slategrey", 112, 128, 144}, {"snow", 255, 250, 250}, {"springgreen", 0, 255, 127},
    {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140}, {"teal", 0, 128, 128},
    {"thistle", 216, 191, 216}, {"tomato", 255, 99, 71}, {"turquoise", 64, 224, 208},
    {"violet", 238, 130, 238}, {"wheat", 245, 222, 179}, {"white", 255, 255, 255},
    {"whitesmoke", 245, 245, 245}, {"yellow", 255, 255, 0}, {"yellowgreen", 154, 205, 50},
};

const char *const embi_system_color_names[] = {
    "Canvas",     "CanvasText",   "LinkText",     "VisitedText",   "ActiveText",
    "ButtonFace", "ButtonText",   "ButtonBorder", "Field",         "FieldText",
    "Highlight",  "HighlightText", "SelectedItem", "SelectedItemText", "Mark",
    "MarkText",   "GrayText",     "AccentColor",  "AccentColorText",
};
static_assert(sizeof embi_system_color_names / sizeof embi_system_color_names[0] ==
                  EMBI_SYSTEM_COLOR_COUNT,
              "EMBI_SYSTEM_COLOR_COUNT must match embi_system_color_names");
const uint32_t embi_system_color_count = sizeof embi_system_color_names /
                                         sizeof embi_system_color_names[0];

/* Spaces color-mix() interpolates in. Internal to this file. */
enum {
    MS_SRGB,
    MS_LINEAR,
    MS_HSL,
    MS_HWB,
    MS_LAB,
    MS_LCH,
    MS_OKLAB,
    MS_OKLCH,
    MS_XYZ50,
    MS_XYZ65,
    MS_P3,
    MS_P3L,
    MS_A98,
    MS_PROPHOTO,
    MS_REC2020,
};

static const struct {
    const char *name;
    uint8_t ms;
} mix_spaces[] = {
    {"srgb", MS_SRGB},   {"srgb-linear", MS_LINEAR}, {"hsl", MS_HSL},     {"hwb", MS_HWB},
    {"lab", MS_LAB},     {"lch", MS_LCH},            {"oklab", MS_OKLAB}, {"oklch", MS_OKLCH},
    {"xyz", MS_XYZ65},   {"xyz-d65", MS_XYZ65},      {"xyz-d50", MS_XYZ50},
    {"display-p3", MS_P3}, {"display-p3-linear", MS_P3L}, {"a98-rgb", MS_A98}, {"prophoto-rgb", MS_PROPHOTO},
    {"rec2020", MS_REC2020},
};

static bool is_polar(uint8_t ms)
{
    return ms == MS_HSL || ms == MS_HWB || ms == MS_LCH || ms == MS_OKLCH;
}

/* ------------------------------------------------------------------ named and system colors */

bool embi_color_named(embi_str name, embi_color *out)
{
    memset(out, 0, sizeof *out);
    out->space = EMBI_CS_SRGB;
    out->alpha = 1.0;
    if (embi_str_ieq(name, EMBI_STR("transparent"))) {
        out->alpha = 0.0;
        return true;
    }
    for (size_t i = 0; i < sizeof named_colors / sizeof named_colors[0]; i++) {
        const char *n = named_colors[i].name;
        if (embi_str_ieq(name, (embi_str){n, (uint32_t)strlen(n)})) {
            out->c[0] = named_colors[i].r / 255.0;
            out->c[1] = named_colors[i].g / 255.0;
            out->c[2] = named_colors[i].b / 255.0;
            return true;
        }
    }
    return false;
}

bool embi_color_system(embi_str name, uint16_t *index)
{
    for (uint32_t i = 0; i < embi_system_color_count; i++) {
        const char *n = embi_system_color_names[i];
        if (embi_str_ieq(name, (embi_str){n, (uint32_t)strlen(n)})) {
            *index = (uint16_t)i;
            return true;
        }
    }
    /* The deprecated system colors are aliases of the current ones (CSS Color 4 section 6.2). */
    static const struct {
        const char *name, *target;
    } deprecated[] = {
        {"ActiveBorder", "ButtonBorder"}, {"ActiveCaption", "Canvas"}, {"AppWorkspace", "Canvas"},
        {"Background", "Canvas"}, {"ButtonHighlight", "ButtonFace"}, {"ButtonShadow", "ButtonFace"},
        {"CaptionText", "CanvasText"}, {"InactiveBorder", "ButtonBorder"}, {"InactiveCaption", "Canvas"},
        {"InactiveCaptionText", "GrayText"}, {"InfoBackground", "Canvas"}, {"InfoText", "CanvasText"},
        {"Menu", "Canvas"}, {"MenuText", "CanvasText"}, {"Scrollbar", "Canvas"}, {"ThreeDDarkShadow", "ButtonBorder"},
        {"ThreeDFace", "ButtonFace"}, {"ThreeDHighlight", "ButtonBorder"}, {"ThreeDLightShadow", "ButtonBorder"},
        {"ThreeDShadow", "ButtonBorder"}, {"Window", "Canvas"}, {"WindowFrame", "ButtonBorder"},
        {"WindowText", "CanvasText"},
    };
    for (size_t k = 0; k < sizeof deprecated / sizeof deprecated[0]; k++) {
        if (embi_str_ieq(name, (embi_str){deprecated[k].name, (uint32_t)strlen(deprecated[k].name)})) {
            const char *t = deprecated[k].target;
            return embi_color_system((embi_str){t, (uint32_t)strlen(t)}, index);
        }
    }
    return false;
}

/* ------------------------------------------------------------------ component helpers */

/* Math functions (calc() and the Values 4 functions) are evaluated when a channel is read. */
static bool is_math_cv(const embi_css_cv *cv)
{
    if (cv->tok.type != EMBI_CSS_FUNCTION) {
        return false;
    }
    static const char *const names[] = {
        "calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan", "asin",
        "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (embi_str_ieq(cv->tok.value, (embi_str){names[i], (uint32_t)strlen(names[i])})) {
            return true;
        }
    }
    return false;
}

/* Kinds: 1 number, 2 percentage, 3 none, 4 math function, 0 anything else. */
static int kind_of(const embi_css_cv *cv)
{
    switch (cv->tok.type) {
    case EMBI_CSS_NUMBER:
        return 1;
    case EMBI_CSS_PERCENTAGE:
        return 2;
    case EMBI_CSS_IDENT:
        return embi_str_ieq(cv->tok.value, EMBI_STR("none")) ? 3 : 0;
    case EMBI_CSS_FUNCTION:
        return is_math_cv(cv) ? 4 : 0;
    default:
        return 0;
    }
}

/* Evaluates a math function channel. Percentages in it resolve against pct_ref. Lengths are not
 * allowed in a color channel, so only number, percentage and angle types are accepted. */
static bool math_value(const embi_css_cv *cv, double pct_ref, double *out)
{
    embi_arena ar = {0};
    embi_arena_init(&ar, 512);
    const embi_calc *c = nullptr;
    uint8_t type = 0;
    bool ok = embi_calc_parse_cv(&ar, cv, &c, &type) &&
              (type & ~(EMBI_CD_NUM | EMBI_CD_PCT | EMBI_CD_ANGLE)) == 0;
    if (ok) {
        embi_calc_env env = {pct_ref, nullptr, nullptr};
        *out = embi_calc_eval(c, &env);
        ok = *out == *out;
    }
    embi_arena_free(&ar);
    return ok;
}

/* A number, percentage (scaled so that 100% is pct_ref), or none (0). */
static bool chan(const embi_css_cv *cv, double pct_ref, double *out)
{
    switch (kind_of(cv)) {
    case 1:
        *out = cv->tok.num;
        return true;
    case 2:
        *out = cv->tok.num * pct_ref / 100.0;
        return true;
    case 3:
        *out = 0;
        return true;
    case 4:
        return math_value(cv, pct_ref, out);
    default:
        return false;
    }
}

/* Like chan(), but `none` stays NaN (a missing component) when keep_none is set. */
static bool chan_k(const embi_css_cv *cv, double pct_ref, bool keep_none, double *out)
{
    if (keep_none && kind_of(cv) == 3) {
        *out = __builtin_nan("");
        return true;
    }
    return chan(cv, pct_ref, out);
}

/* A hue: a number (degrees) or an angle, or none. */
static bool hue(const embi_css_cv *cv, double *out)
{
    if (cv->tok.type == EMBI_CSS_NUMBER) {
        *out = cv->tok.num;
        return true;
    }
    if (cv->tok.type == EMBI_CSS_DIMENSION) {
        uint8_t unit = 0;
        uint8_t cls = 0;
        double scale = 1.0;
        if (embi_unit_lookup(cv->tok.value, &unit, &cls, &scale) && cls == EMBI_CD_ANGLE) {
            *out = cv->tok.num * scale;
            return true;
        }
        return false;
    }
    if (kind_of(cv) == 3) {
        *out = 0;
        return true;
    }
    if (is_math_cv(cv)) {
        return math_value(cv, 1.0, out);
    }
    return false;
}

/* An alpha value: number or percentage, clamped to 0..1. */
static bool alpha_of(const embi_css_cv *cv, double *out)
{
    double v = 0;
    switch (kind_of(cv)) {
    case 1:
        v = cv->tok.num;
        break;
    case 2:
        v = cv->tok.num / 100.0;
        break;
    case 3:
        v = 0;
        break;
    case 4:
        if (!math_value(cv, 1.0, &v)) {
            return false;
        }
        break;
    default:
        return false;
    }
    *out = v < 0 ? 0 : (v > 1 ? 1 : v);
    return true;
}

typedef struct cargs {
    const embi_css_cv *c[4];
    const embi_css_cv *alpha;
    const embi_css_cv *space;
    uint32_t nc;
    bool legacy;
} cargs;

/* Splits the function's arguments: channels, then an optional alpha (after a slash, or as a fourth
 * comma-separated value in the legacy form). want is the largest channel count accepted. When
 * space_first is set, the first argument is a colorspace ident. */
static bool split_args(const embi_css_cv *fn, uint32_t want, bool space_first, cargs *out)
{
    memset(out, 0, sizeof *out);
    uint32_t seg = 0;
    uint32_t commas = 0;
    bool after_slash = false;
    for (uint32_t i = 0; i < fn->nkids; i++) {
        const embi_css_cv *cv = &fn->kids[i];
        if (cv->tok.type == EMBI_CSS_WS) {
            continue;
        }
        if (cv->tok.type == EMBI_CSS_COMMA) {
            if (seg != 1 || after_slash) {
                return false;
            }
            seg = 0;
            commas++;
            continue;
        }
        if (cv->tok.type == EMBI_CSS_DELIM && cv->tok.cp == '/') {
            if (after_slash || commas > 0 || seg == 0) {
                return false;
            }
            after_slash = true;
            seg = 0;
            continue;
        }
        seg++;
        if (space_first && out->space == nullptr) {
            if (cv->tok.type != EMBI_CSS_IDENT) {
                return false;
            }
            out->space = cv;
            continue;
        }
        if (after_slash) {
            if (out->alpha) {
                return false;
            }
            out->alpha = cv;
        } else if (out->nc < want) {
            out->c[out->nc++] = cv;
        } else if (commas > 0 && out->nc == want && out->alpha == nullptr) {
            out->alpha = cv;
        } else {
            return false;
        }
    }
    if (commas > 0) {
        out->legacy = true;
        if (seg != 1) {
            return false;
        }
    }
    if (after_slash && out->alpha == nullptr) {
        return false;
    }
    return true;
}

static bool set_alpha(const cargs *a, embi_color *out)
{
    out->alpha = 1.0;
    if (a->alpha != nullptr) {
        return alpha_of(a->alpha, &out->alpha);
    }
    return true;
}

/* ------------------------------------------------------------------ hsl, hwb and conversions */

static double clamp01(double v)
{
    return !(v > 0) ? 0 : (v > 1 ? 1 : v);
}

/* CSS Color 4 hsl-to-rgb on the 0..1 scale (h in degrees, s and l in 0..1). The channel formula
 * is the spec's k/a/m form, which the reference serialization matches digit for digit. */
static void hsl_to_rgb(double h, double s, double l, double rgb[3])
{
    h = h - 360.0 * __builtin_floor(h / 360.0);
    double a = s * (l < 1 - l ? l : 1 - l);
    for (int n = 0; n < 3; n++) {
        double k = (double)((n == 0 ? 0 : (n == 1 ? 8 : 4)) + h / 30.0);
        k = k - 12.0 * __builtin_floor(k / 12.0);
        double m = k - 3.0;
        double t = 9.0 - k;
        if (t < m) {
            m = t;
        }
        if (m > 1.0) {
            m = 1.0;
        }
        if (m < -1.0) {
            m = -1.0;
        }
        rgb[n] = l - a * m;
    }
}

static void rgb_to_hsl(const double rgb[3], double *h, double *s, double *l)
{
    double r = rgb[0], g = rgb[1], b = rgb[2];
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;
    *l = (mx + mn) / 2.0;
    if (d == 0) {
        *h = 0;
        *s = 0;
        return;
    }
    *s = d / (1.0 - (2.0 * *l - 1.0 < 0 ? -(2.0 * *l - 1.0) : (2.0 * *l - 1.0)));
    if (mx == r) {
        *h = (g - b) / d;
        if (*h < 0) {
            *h += 6;
        }
    } else if (mx == g) {
        *h = (b - r) / d + 2;
    } else {
        *h = (r - g) / d + 4;
    }
    *h *= 60.0;
}

static double srgb_to_lin(double c)
{
    double a = c < 0 ? -c : c;
    double v = a <= 0.04045 ? a / 12.92 : SDL_pow((a + 0.055) / 1.055, 2.4);
    return c < 0 ? -v : v;
}

static double lin_to_srgb(double c)
{
    double a = c < 0 ? -c : c;
    double v = a <= 0.0031308 ? 12.92 * a : 1.055 * SDL_pow(a, 1.0 / 2.4) - 0.055;
    return c < 0 ? -v : v;
}

static double cbrt_signed(double x)
{
    return x < 0 ? -SDL_pow(-x, 1.0 / 3.0) : SDL_pow(x, 1.0 / 3.0);
}

static void mat_mul(const double m[9], const double v[3], double out[3])
{
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static const double srgb_to_xyz65[9] = {
    0.412390799266, 0.357584339384, 0.180480788402, 0.212639005872, 0.715168678768,
    0.0721923153607, 0.0193308187156, 0.119194779795, 0.950532152250,
};
static const double xyz65_to_srgb[9] = {
    3.24096994190, -1.53738317757, -0.498610760293, -0.969243636281, 1.87596750151,
    0.0415550574072, 0.0556300796970, -0.203976958889, 1.05697151424,
};
static const double p3_to_xyz65[9] = {
    0.486570948648, 0.265667693169, 0.198217285234, 0.228974564070, 0.691738521837,
    0.0792869140937, 0.0, 0.0451133818589, 1.04394436890,
};
static const double xyz65_to_p3[9] = {
    2.49349691194, -0.931383617919, -0.402710784451, -0.829488969562, 1.76266406032,
    0.0236246858419, 0.0358458302438, -0.0761723892680, 0.956884524008,
};
static const double a98_to_xyz65[9] = {
    0.576669042910, 0.185558237907, 0.188228646235, 0.297344975251, 0.627363566255,
    0.0752914584940, 0.0270313613864, 0.0706888525358, 0.991337536838,
};
static const double xyz65_to_a98[9] = {
    2.04158790381, -0.565006974279, -0.344731350778, -0.969243636281, 1.87596750151,
    0.0415550574072, 0.0134442806320, -0.118362392231, 1.01517499439,
};
static const double rec2020_to_xyz65[9] = {
    0.636958048301, 0.144616903586, 0.168880975164, 0.262700212011, 0.677998071519,
    0.0593017164699, 0.0, 0.0280726930491, 1.06098505771,
};
static const double xyz65_to_rec2020[9] = {
    1.71665118797, -0.355670783776, -0.253366281374, -0.666684351832, 1.61648123663,
    0.0157685458139, 0.0176398574453, -0.0427706132578, 0.942103121235,
};
static const double prophoto_to_xyz50[9] = {
    0.797760489672, 0.135185837176, 0.0313493495815, 0.288071128229, 0.711843217810,
    0.0000856539606053, 0.0, 0.0, 0.825104602510,
};
static const double xyz50_to_prophoto[9] = {
    1.34579897310, -0.255580100080, -0.0511062850675, -0.544622493903, 1.50823274131,
    0.0205360323915, 0.0, 0.0, 1.21196754564,
};
/* Bradford chromatic adaptation between the CSS Color 4 whites D65 (x 0.3127, y 0.3290) and D50
 * (x 0.3457, y 0.3585), computed from the Bradford cone response matrix to full precision. */
static const double d50_to_d65[9] = {
    0.955473421488075, -0.0230984549487647, 0.0632592432005707,
    -0.0283697093338639, 1.0099953980813, 0.0210414411919173,
    0.012314014864482, -0.020507649298899, 1.33036592624212,
};
static const double d65_to_d50[9] = {
    1.047929792545, 0.0229468706016097, -0.0501922662892053,
    0.029627808770056, 0.99043442675388, -0.0170737990634188,
    -0.00924304064620451, 0.0150551914902982, 0.751874281428137,
};

/* Transfer functions of the RGB spaces, applied per channel with the sign kept. */
static double srgb_decode(double c)
{
    return srgb_to_lin(c);
}

static double srgb_encode(double c)
{
    return lin_to_srgb(c);
}

static double a98_decode(double c)
{
    double a = c < 0 ? -c : c;
    double v = SDL_pow(a, 563.0 / 256.0);
    return c < 0 ? -v : v;
}

static double a98_encode(double c)
{
    double a = c < 0 ? -c : c;
    double v = SDL_pow(a, 256.0 / 563.0);
    return c < 0 ? -v : v;
}

static double prophoto_decode(double c)
{
    double a = c < 0 ? -c : c;
    double v = a < 16.0 / 512.0 ? a / 16.0 : SDL_pow(a, 1.8);
    return c < 0 ? -v : v;
}

static double prophoto_encode(double c)
{
    double a = c < 0 ? -c : c;
    double v = a < 1.0 / 512.0 ? 16.0 * a : SDL_pow(a, 1.0 / 1.8);
    return c < 0 ? -v : v;
}

static double rec2020_decode(double c)
{
    const double alpha = 1.09929682680944, beta = 0.018053968510807;
    double a = c < 0 ? -c : c;
    double v = a < beta * 4.5 ? a / 4.5 : SDL_pow((a + alpha - 1.0) / alpha, 1.0 / 0.45);
    return c < 0 ? -v : v;
}

static double rec2020_encode(double c)
{
    const double alpha = 1.09929682680944, beta = 0.018053968510807;
    double a = c < 0 ? -c : c;
    double v = a < beta ? 4.5 * a : alpha * SDL_pow(a, 0.45) - (alpha - 1.0);
    return c < 0 ? -v : v;
}

/* D50 reference white for lab(): CSS Color 4 derives it from the chromaticity (0.3457, 0.3585):
 * X = 0.3457 / 0.3585, Z = (1 - 0.3457 - 0.3585) / 0.3585. */
static const double white50[3] = {0.9642956764295677, 1.0, 0.8251046025104602};

static void xyz50_to_lab(const double xyz[3], double lab[3])
{
    const double eps = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;
    double f[3];
    for (int i = 0; i < 3; i++) {
        double t = xyz[i] / white50[i];
        f[i] = t > eps ? cbrt_signed(t) : (kappa * t + 16.0) / 116.0;
    }
    lab[0] = 116.0 * f[1] - 16.0;
    lab[1] = 500.0 * (f[0] - f[1]);
    lab[2] = 200.0 * (f[1] - f[2]);
}

static void lab_to_xyz50(const double lab[3], double xyz[3])
{
    const double eps = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;
    double fy = (lab[0] + 16.0) / 116.0;
    double fx = fy + lab[1] / 500.0;
    double fz = fy - lab[2] / 200.0;
    double x = fx * fx * fx > eps ? fx * fx * fx : (116.0 * fx - 16.0) / kappa;
    double y = lab[0] > kappa * eps ? fy * fy * fy : lab[0] / kappa;
    double z = fz * fz * fz > eps ? fz * fz * fz : (116.0 * fz - 16.0) / kappa;
    xyz[0] = x * white50[0];
    xyz[1] = y * white50[1];
    xyz[2] = z * white50[2];
}

static void xyz65_to_oklab(const double xyz[3], double lab[3])
{
    double lms[3] = {
        0.8189330101 * xyz[0] + 0.3618667424 * xyz[1] - 0.1288597137 * xyz[2],
        0.0329845436 * xyz[0] + 0.9293118715 * xyz[1] + 0.0361456387 * xyz[2],
        0.0482003018 * xyz[0] + 0.2643662691 * xyz[1] + 0.6338517070 * xyz[2],
    };
    for (int i = 0; i < 3; i++) {
        lms[i] = cbrt_signed(lms[i]);
    }
    lab[0] = 0.2104542553 * lms[0] + 0.7936177850 * lms[1] - 0.0040720468 * lms[2];
    lab[1] = 1.9779984951 * lms[0] - 2.4285922050 * lms[1] + 0.4505937099 * lms[2];
    lab[2] = 0.0259040371 * lms[0] + 0.7827717662 * lms[1] - 0.8086757660 * lms[2];
}

static void oklab_to_xyz65(const double lab[3], double xyz[3])
{
    double l_ = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    double m_ = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    double s_ = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    xyz[0] = 1.2270138511 * l - 0.5577999807 * m + 0.2812561490 * s;
    xyz[1] = -0.0405801784 * l + 1.1122568696 * m - 0.0716766787 * s;
    xyz[2] = -0.0763812845 * l - 0.4214819784 * m + 1.5861632204 * s;
}

/* Converts a color to XYZ D65. Returns false for spaces that have no conversion here. */
static bool to_xyz65(const embi_color *c, double xyz[3])
{
    double v[3];
    switch (c->space) {
    case EMBI_CS_SRGB:
        for (int i = 0; i < 3; i++) {
            v[i] = srgb_decode(c->c[i]);
        }
        mat_mul(srgb_to_xyz65, v, xyz);
        return true;
    case EMBI_CS_SRGB_LINEAR:
        mat_mul(srgb_to_xyz65, c->c, xyz);
        return true;
    case EMBI_CS_DISPLAY_P3:
        for (int i = 0; i < 3; i++) {
            v[i] = srgb_decode(c->c[i]);
        }
        mat_mul(p3_to_xyz65, v, xyz);
        return true;
    case EMBI_CS_DISPLAY_P3_LINEAR:
        mat_mul(p3_to_xyz65, c->c, xyz);
        return true;
    case EMBI_CS_A98_RGB:
        for (int i = 0; i < 3; i++) {
            v[i] = a98_decode(c->c[i]);
        }
        mat_mul(a98_to_xyz65, v, xyz);
        return true;
    case EMBI_CS_PROPHOTO_RGB: {
        double x50[3];
        for (int i = 0; i < 3; i++) {
            v[i] = prophoto_decode(c->c[i]);
        }
        mat_mul(prophoto_to_xyz50, v, x50);
        mat_mul(d50_to_d65, x50, xyz);
        return true;
    }
    case EMBI_CS_REC2020:
        for (int i = 0; i < 3; i++) {
            v[i] = rec2020_decode(c->c[i]);
        }
        mat_mul(rec2020_to_xyz65, v, xyz);
        return true;
    case EMBI_CS_XYZ_D65:
        xyz[0] = c->c[0];
        xyz[1] = c->c[1];
        xyz[2] = c->c[2];
        return true;
    case EMBI_CS_XYZ_D50:
        mat_mul(d50_to_d65, c->c, xyz);
        return true;
    case EMBI_CS_LAB: {
        double x50[3];
        lab_to_xyz50(c->c, x50);
        mat_mul(d50_to_d65, x50, xyz);
        return true;
    }
    case EMBI_CS_LCH: {
        double lab[3] = {c->c[0], c->c[1] * SDL_cos(c->c[2] * SDL_PI_D / 180.0),
                         c->c[1] * SDL_sin(c->c[2] * SDL_PI_D / 180.0)};
        double x50[3];
        lab_to_xyz50(lab, x50);
        mat_mul(d50_to_d65, x50, xyz);
        return true;
    }
    case EMBI_CS_OKLAB:
        oklab_to_xyz65(c->c, xyz);
        return true;
    case EMBI_CS_OKLCH: {
        double lab[3] = {c->c[0], c->c[1] * SDL_cos(c->c[2] * SDL_PI_D / 180.0),
                         c->c[1] * SDL_sin(c->c[2] * SDL_PI_D / 180.0)};
        oklab_to_xyz65(lab, xyz);
        return true;
    }
    default:
        return false;
    }
}

/* Writes the components of a mix space from XYZ D65. */
static void from_xyz65(const double xyz[3], uint8_t ms, double comp[3])
{
    double lin[3];
    switch (ms) {
    case MS_LINEAR:
        mat_mul(xyz65_to_srgb, xyz, comp);
        return;
    case MS_SRGB:
    case MS_HSL:
    case MS_HWB: {
        mat_mul(xyz65_to_srgb, xyz, lin);
        double rgb[3];
        for (int i = 0; i < 3; i++) {
            rgb[i] = lin_to_srgb(lin[i]);
        }
        if (ms == MS_SRGB) {
            comp[0] = rgb[0];
            comp[1] = rgb[1];
            comp[2] = rgb[2];
        } else if (ms == MS_HSL) {
            rgb_to_hsl(rgb, &comp[0], &comp[1], &comp[2]);
            comp[1] *= 100.0;
            comp[2] *= 100.0;
        } else {
            double h, s, l;
            rgb_to_hsl(rgb, &h, &s, &l);
            double w = rgb[0] < rgb[1] ? (rgb[0] < rgb[2] ? rgb[0] : rgb[2])
                                       : (rgb[1] < rgb[2] ? rgb[1] : rgb[2]);
            double mx = rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2])
                                        : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
            comp[0] = h;
            comp[1] = w * 100.0;
            comp[2] = (1.0 - mx) * 100.0;
        }
        return;
    }
    case MS_P3L:
        mat_mul(xyz65_to_p3, xyz, comp);
        return;
    case MS_P3:
    case MS_A98:
    case MS_PROPHOTO:
    case MS_REC2020: {
        double rgbl[3];
        const double *m = ms == MS_P3 ? xyz65_to_p3
                          : ms == MS_A98 ? xyz65_to_a98
                          : ms == MS_REC2020 ? xyz65_to_rec2020
                                             : nullptr;
        if (ms == MS_PROPHOTO) {
            double x50[3];
            mat_mul(d65_to_d50, xyz, x50);
            mat_mul(xyz50_to_prophoto, x50, rgbl);
        } else {
            mat_mul(m, xyz, rgbl);
        }
        for (int i = 0; i < 3; i++) {
            comp[i] = ms == MS_P3 ? srgb_encode(rgbl[i])
                      : ms == MS_A98 ? a98_encode(rgbl[i])
                      : ms == MS_PROPHOTO ? prophoto_encode(rgbl[i])
                                          : rec2020_encode(rgbl[i]);
        }
        return;
    }
    case MS_XYZ65:
        comp[0] = xyz[0];
        comp[1] = xyz[1];
        comp[2] = xyz[2];
        return;
    case MS_XYZ50:
        mat_mul(d65_to_d50, xyz, comp);
        return;
    case MS_LAB:
    case MS_LCH: {
        double x50[3];
        mat_mul(d65_to_d50, xyz, x50);
        double lab[3];
        xyz50_to_lab(x50, lab);
        if (ms == MS_LAB) {
            comp[0] = lab[0];
            comp[1] = lab[1];
            comp[2] = lab[2];
        } else {
            double c = SDL_sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            double h = SDL_atan2(lab[2], lab[1]) * 180.0 / SDL_PI_D;
            comp[0] = lab[0];
            comp[1] = c;
            comp[2] = h < 0 ? h + 360.0 : h;
        }
        return;
    }
    case MS_OKLAB:
    case MS_OKLCH:
    default: { /* unknown spaces never get here: parse_color_mix rejects them first */
        double lab[3];
        xyz65_to_oklab(xyz, lab);
        if (ms == MS_OKLAB) {
            comp[0] = lab[0];
            comp[1] = lab[1];
            comp[2] = lab[2];
        } else {
            double c = SDL_sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            double h = SDL_atan2(lab[2], lab[1]) * 180.0 / SDL_PI_D;
            comp[0] = lab[0];
            comp[1] = c;
            comp[2] = h < 0 ? h + 360.0 : h;
        }
        return;
    }
    }
}

/* Writes an embi_color from mix-space components (alpha is set separately by the caller). */
static void color_from_ms(uint8_t ms, const double comp[3], embi_color *out)
{
    memset(out, 0, sizeof *out);
    out->alpha = 1.0;
    switch (ms) {
    case MS_SRGB:
        out->space = EMBI_CS_SRGB;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_HSL:
        out->space = EMBI_CS_SRGB;
        hsl_to_rgb(comp[0], comp[1] / 100.0, comp[2] / 100.0, out->c);
        break;
    case MS_HWB: {
        out->space = EMBI_CS_SRGB;
        double w = comp[1] / 100.0, b = comp[2] / 100.0;
        if (w + b >= 1.0) {
            double gray = w / (w + b);
            out->c[0] = out->c[1] = out->c[2] = gray;
        } else {
            double rgb[3];
            hsl_to_rgb(comp[0], 1.0, 0.5, rgb);
            for (int i = 0; i < 3; i++) {
                out->c[i] = rgb[i] * (1.0 - w - b) + w;
            }
        }
        break;
    }
    case MS_LINEAR:
        out->space = EMBI_CS_SRGB_LINEAR;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_LAB:
        out->space = EMBI_CS_LAB;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_LCH:
        out->space = EMBI_CS_LCH;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_OKLAB:
        out->space = EMBI_CS_OKLAB;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_OKLCH:
        out->space = EMBI_CS_OKLCH;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_P3:
        out->space = EMBI_CS_DISPLAY_P3;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_P3L:
        out->space = EMBI_CS_DISPLAY_P3_LINEAR;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_A98:
        out->space = EMBI_CS_A98_RGB;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_PROPHOTO:
        out->space = EMBI_CS_PROPHOTO_RGB;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_REC2020:
        out->space = EMBI_CS_REC2020;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_XYZ50:
        out->space = EMBI_CS_XYZ_D50;
        memcpy(out->c, comp, sizeof out->c);
        break;
    case MS_XYZ65:
    default: /* unknown spaces never get here: parse_color_mix rejects them first */
        out->space = EMBI_CS_XYZ_D65;
        memcpy(out->c, comp, sizeof out->c);
        break;
    }
}

/* ------------------------------------------------------------------ gamut mapping (CSS Color 4) */

static bool in_srgb_gamut(const double rgb[3])
{
    for (int i = 0; i < 3; i++) {
        if (rgb[i] < 0 || rgb[i] > 1) {
            return false;
        }
    }
    return true;
}

static void clip_srgb(const double rgb[3], double out[3])
{
    for (int i = 0; i < 3; i++) {
        out[i] = clamp01(rgb[i]);
    }
}

/* Gamma-encoded sRGB of an XYZ D65 color, unclipped. */
static void xyz65_to_srgb_gamma(const double xyz[3], double rgb[3])
{
    double lin[3];
    mat_mul(xyz65_to_srgb, xyz, lin);
    for (int i = 0; i < 3; i++) {
        rgb[i] = lin_to_srgb(lin[i]);
    }
}

static void srgb_gamma_to_oklab(const double rgb[3], double lab[3])
{
    double lin[3], xyz[3];
    for (int i = 0; i < 3; i++) {
        lin[i] = srgb_to_lin(rgb[i]);
    }
    mat_mul(srgb_to_xyz65, lin, xyz);
    xyz65_to_oklab(xyz, lab);
}

static double delta_eok(const double a[3], const double b[3])
{
    double dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];
    return SDL_sqrt(dl * dl + da * da + db * db);
}

/* The CSS Color 4 gamut mapping algorithm: keep the OKLCH lightness and hue, and binary search
 * the chroma for the largest value whose clipped color is within a just noticeable difference
 * (0.02 deltaEOK) of the unclipped one. Colors inside the sRGB gamut are returned unchanged. */
static void gamut_map_srgb(const double xyz[3], double out[3])
{
    const double jnd = 0.02;
    const double eps = 0.0001;
    double lab[3];
    xyz65_to_oklab(xyz, lab);
    const double L = lab[0];
    if (L >= 1.0) {
        out[0] = out[1] = out[2] = 1.0;
        return;
    }
    if (L <= 0.0) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    double rgb[3];
    xyz65_to_srgb_gamma(xyz, rgb);
    if (in_srgb_gamut(rgb)) {
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        return;
    }
    /* A color that is out of gamut only by floating point noise (the D50 white converted through
     * the Bradford matrices lands within 1e-11 of 1) is clipped, not chroma-reduced. */
    {
        const double noise = 1e-7;
        bool near = true;
        for (int i = 0; i < 3; i++) {
            if (rgb[i] < -noise || rgb[i] > 1.0 + noise) {
                near = false;
            }
        }
        if (near) {
            clip_srgb(rgb, out);
            return;
        }
    }
    const double chroma = SDL_sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
    const double hue = SDL_atan2(lab[2], lab[1]);
    double lo = 0.0, hi = chroma;
    bool lo_in_gamut = true;
    double clipped[3];
    clip_srgb(rgb, clipped);
    while (hi - lo > eps) {
        const double c = (lo + hi) / 2.0;
        const double cur[3] = {L, c * SDL_cos(hue), c * SDL_sin(hue)};
        double cur_xyz[3];
        oklab_to_xyz65(cur, cur_xyz);
        xyz65_to_srgb_gamma(cur_xyz, rgb);
        if (lo_in_gamut && in_srgb_gamut(rgb)) {
            lo = c;
            continue;
        }
        clip_srgb(rgb, clipped);
        double clipped_lab[3];
        srgb_gamma_to_oklab(clipped, clipped_lab);
        const double e = delta_eok(clipped_lab, cur);
        if (e < jnd) {
            if (jnd - e < eps) {
                break;
            }
            lo_in_gamut = false;
            lo = c;
        } else {
            hi = c;
        }
    }
    out[0] = clipped[0];
    out[1] = clipped[1];
    out[2] = clipped[2];
}

bool embi_color_to_srgb(const embi_color *in_orig, embi_color *out)
{
    /* A missing component ("none") is zero once the color is used (CSS Color 4 section 4.4). */
    embi_color flat = *in_orig;
    for (int i = 0; i < 3; i++) {
        if (flat.c[i] != flat.c[i]) {
            flat.c[i] = 0.0;
        }
    }
    if (flat.alpha != flat.alpha) {
        flat.alpha = 0.0;
    }
    const embi_color *in = &flat;
    if (in->space == EMBI_CS_SRGB && in->c[0] >= 0 && in->c[0] <= 1 && in->c[1] >= 0 &&
        in->c[1] <= 1 && in->c[2] >= 0 && in->c[2] <= 1) {
        *out = *in;
        out->flags = 0;
        return true;
    }
    /* Full lightness is white and zero lightness is black whatever the other components say (CSS Color 4 section
     * 13.2), which the round trip through XYZ would only approximate. */
    bool ok_l = in->space == EMBI_CS_OKLAB || in->space == EMBI_CS_OKLCH;
    bool lab_l = in->space == EMBI_CS_LAB || in->space == EMBI_CS_LCH;
    if ((ok_l || lab_l) && in->c[0] == in->c[0]) {
        /* Within 1e-5 of the ends counts (a lightness of 99.9999% is white), as in the gamut mapping step. */
        double top = ok_l ? 0.99999 : 99.999;
        double bottom = ok_l ? 0.00001 : 0.001;
        if (in->c[0] >= top || in->c[0] <= bottom) {
            double v = in->c[0] >= top ? 1.0 : 0.0;
            color_from_ms(MS_SRGB, (double[3]){v, v, v}, out);
            out->alpha = in->alpha;
            return true;
        }
    }
    double xyz[3];
    if (!to_xyz65(in, xyz)) {
        return false;
    }
    double rgb[3];
    gamut_map_srgb(xyz, rgb);
    color_from_ms(MS_SRGB, rgb, out);
    out->alpha = in->alpha;
    return true;
}

bool embi_color_to_srgb8(const embi_color *in, uint8_t rgba[4])
{
    embi_color s;
    if (!embi_color_to_srgb(in, &s)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        double v = i < 3 ? s.c[i] : s.alpha;
        v = clamp01(v) * 255.0;
        rgba[i] = (uint8_t)(v + 0.5);
    }
    return true;
}

/* ------------------------------------------------------------------ parsing functions *//* ------------------------------------------------------------------ parsing functions */

static bool parse_rgb(const embi_css_cv *fn, embi_color *out)
{
    cargs a;
    if (!split_args(fn, 3, false, &a) || a.nc != 3) {
        return false;
    }
    int k0 = kind_of(a.c[0]), k1 = kind_of(a.c[1]), k2 = kind_of(a.c[2]);
    if (a.legacy && (k0 == 3 || k1 == 3 || k2 == 3 || k0 != k1 || k1 != k2)) {
        return false;
    }
    out->space = EMBI_CS_SRGB;
    out->flags = 0;
    for (int i = 0; i < 3; i++) {
        double v = 0;
        if (!chan(a.c[i], 255.0, &v)) {
            return false;
        }
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        out->c[i] = v / 255.0;
        if (kind_of(a.c[i]) == 3) {
            out->flags |= (uint8_t)(EMBI_CF_NONE0 << i);
        }
    }
    return set_alpha(&a, out);
}

static bool parse_hsl(const embi_css_cv *fn, embi_color *out)
{
    cargs a;
    if (!split_args(fn, 3, false, &a) || a.nc != 3) {
        return false;
    }
    double h = 0, s = 0, l = 0;
    if (!hue(a.c[0], &h)) {
        return false;
    }
    if (a.legacy && (kind_of(a.c[1]) != 2 || kind_of(a.c[2]) != 2)) {
        return false;
    }
    if (!chan(a.c[1], 100.0, &s) || !chan(a.c[2], 100.0, &l)) {
        return false;
    }
    /* Out-of-range saturation and lightness extrapolate (hsl(120 140% 50%) is color(srgb -0.2 1.2 -0.2)); only a
     * negative saturation is clamped. The value is gamut mapped when it is painted. */
    double rgb[3];
    hsl_to_rgb(h, s < 0 ? 0.0 : s / 100.0, l / 100.0, rgb);
    out->space = EMBI_CS_SRGB;
    out->flags = 0;
    memcpy(out->c, rgb, sizeof rgb);
    return set_alpha(&a, out);
}

static bool parse_hwb(const embi_css_cv *fn, embi_color *out)
{
    cargs a;
    if (!split_args(fn, 3, false, &a) || a.nc != 3 || a.legacy) {
        return false;
    }
    double h = 0, w = 0, b = 0;
    if (!hue(a.c[0], &h) || !chan(a.c[1], 100.0, &w) || !chan(a.c[2], 100.0, &b)) {
        return false;
    }
    double comp[3] = {h, w, b};
    color_from_ms(MS_HWB, comp, out);
    return set_alpha(&a, out);
}

static bool parse_three(const embi_css_cv *fn, uint8_t space, double pct_l, double pct_a,
                        double pct_b, bool polar, embi_color *out)
{
    cargs a;
    if (!split_args(fn, 3, false, &a) || a.nc != 3 || a.legacy) {
        return false;
    }
    double comp[3] = {0, 0, 0};
    if (!chan(a.c[0], pct_l, &comp[0])) {
        return false;
    }
    if (polar) {
        if (!chan(a.c[1], pct_a, &comp[1]) || !hue(a.c[2], &comp[2])) {
            return false;
        }
    } else if (!chan(a.c[1], pct_a, &comp[1]) || !chan(a.c[2], pct_b, &comp[2])) {
        return false;
    }
    if (polar) {
        comp[2] = comp[2] - 360.0 * __builtin_floor(comp[2] / 360.0);
    }
    color_from_ms(space, comp, out);
    for (int i = 0; i < 3; i++) {
        if (kind_of(a.c[i]) == 3) {
            out->flags |= (uint8_t)(EMBI_CF_NONE0 << i);
        }
    }
    return set_alpha(&a, out);
}

/* color(<space> c1 c2 c3 [/ alpha]) and device-cmyk(c m y k [/ alpha]). */
static bool parse_color_fn(const embi_css_cv *fn, bool device_form, embi_color *out)
{
    cargs a;
    if (!split_args(fn, 4, !device_form, &a) || (a.legacy && !device_form)) {
        return false;
    }
    uint8_t space = EMBI_CS_SRGB;
    uint32_t want = 3;
    uint32_t flags = 0;
    if (device_form) {
        space = EMBI_CS_DEVICE_CMYK;
        want = 4;
    } else {
        if (a.space == nullptr) {
            return false;
        }
        embi_str s = a.space->tok.value;
        if (embi_str_ieq(s, EMBI_STR("srgb"))) {
            space = EMBI_CS_SRGB;
            flags = EMBI_CF_FUNC;
        } else if (embi_str_ieq(s, EMBI_STR("srgb-linear"))) {
            space = EMBI_CS_SRGB_LINEAR;
        } else if (embi_str_ieq(s, EMBI_STR("display-p3"))) {
            space = EMBI_CS_DISPLAY_P3;
        } else if (embi_str_ieq(s, EMBI_STR("display-p3-linear"))) {
            space = EMBI_CS_DISPLAY_P3_LINEAR;
        } else if (embi_str_ieq(s, EMBI_STR("a98-rgb"))) {
            space = EMBI_CS_A98_RGB;
        } else if (embi_str_ieq(s, EMBI_STR("prophoto-rgb"))) {
            space = EMBI_CS_PROPHOTO_RGB;
        } else if (embi_str_ieq(s, EMBI_STR("rec2020"))) {
            space = EMBI_CS_REC2020;
        } else if (embi_str_ieq(s, EMBI_STR("xyz-d50"))) {
            space = EMBI_CS_XYZ_D50;
        } else if (embi_str_ieq(s, EMBI_STR("xyz")) || embi_str_ieq(s, EMBI_STR("xyz-d65"))) {
            space = EMBI_CS_XYZ_D65;
        } else if (embi_str_ieq(s, EMBI_STR("device-cmyk"))) {
            space = EMBI_CS_DEVICE_CMYK;
            want = 4;
        } else {
            return false;
        }
    }
    if (a.nc != want) {
        return false;
    }
    double v[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < want; i++) {
        if (!chan_k(a.c[i], 1.0, !device_form, &v[i])) {
            return false;
        }
        if (device_form && a.legacy && kind_of(a.c[i]) != 1) {
            return false;
        }
        if (space == EMBI_CS_DEVICE_CMYK) {
            v[i] = clamp01(v[i]);
        }
    }
    memset(out, 0, sizeof *out);
    out->space = space;
    out->flags = (uint8_t)flags;
    out->c[0] = v[0];
    out->c[1] = v[1];
    out->c[2] = v[2];
    if (want == 4) {
        out->alpha_k = v[3];
    }
    return set_alpha(&a, out);
}

/* Components of a color already expressed in the mix space, without an XYZ round trip. */
static bool direct_components(const embi_color *c, uint8_t ms, double comp[3])
{
    if (ms == MS_SRGB && c->space == EMBI_CS_SRGB) {
        memcpy(comp, c->c, 3 * sizeof comp[0]);
        return true;
    }
    if (ms == MS_LINEAR && c->space == EMBI_CS_SRGB) {
        for (int i = 0; i < 3; i++) {
            comp[i] = srgb_to_lin(c->c[i]);
        }
        return true;
    }
    if (ms == MS_LINEAR && c->space == EMBI_CS_SRGB_LINEAR) {
        memcpy(comp, c->c, 3 * sizeof comp[0]);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ interpolation (color-mix, gradients) */

/* A color converted into an interpolation space, ready to be blended at any weight. NaN components are missing
 * ("none"): the other color's value is used, and a result that is missing on both sides is 0. */
struct embi_color_mixer {
    uint8_t ms;
    uint8_t hue_m; /* 0 shorter, 1 longer, 2 increasing, 3 decreasing */
    double comp[2][3];
    double alpha[2];
};

/* The channel that is a hue in the space, or -1. */
static int hue_channel(uint8_t ms)
{
    return ms == MS_HSL || ms == MS_HWB ? 0 : (ms == MS_LCH || ms == MS_OKLCH ? 2 : -1);
}

/* Whether the hue of components in a polar space carries no information (a gray). */
static bool hue_powerless(uint8_t ms, const double comp[3])
{
    switch (ms) {
    case MS_LCH:
        return comp[1] <= 0.0015;
    case MS_OKLCH:
        return comp[1] <= 0.000004;
    case MS_HSL:
        return comp[1] <= 1e-5;
    case MS_HWB:
        return comp[1] + comp[2] >= 100.0 - 1e-5;
    default:
        return false;
    }
}

/* The mix space that is the color's own space, or 0xff. */
static uint8_t own_ms(const embi_color *c)
{
    switch (c->space) {
    case EMBI_CS_SRGB:
        return MS_SRGB;
    case EMBI_CS_SRGB_LINEAR:
        return MS_LINEAR;
    case EMBI_CS_LAB:
        return MS_LAB;
    case EMBI_CS_LCH:
        return MS_LCH;
    case EMBI_CS_OKLAB:
        return MS_OKLAB;
    case EMBI_CS_OKLCH:
        return MS_OKLCH;
    case EMBI_CS_XYZ_D50:
        return MS_XYZ50;
    case EMBI_CS_XYZ_D65:
        return MS_XYZ65;
    case EMBI_CS_DISPLAY_P3:
        return MS_P3;
    case EMBI_CS_DISPLAY_P3_LINEAR:
        return MS_P3L;
    case EMBI_CS_A98_RGB:
        return MS_A98;
    case EMBI_CS_PROPHOTO_RGB:
        return MS_PROPHOTO;
    case EMBI_CS_REC2020:
        return MS_REC2020;
    default:
        return 0xff;
    }
}

static bool mixer_setup(embi_color_mixer *mx, const embi_color *a, const embi_color *b, uint8_t ms, uint8_t hue_m)
{
    mx->ms = ms;
    mx->hue_m = hue_m;
    const embi_color *in[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        const embi_color *c = in[k];
        mx->alpha[k] = c->alpha;
        /* Inputs already in the mix space (or sRGB for the linear space) are used directly, keeping missing
         * components; the XYZ hub matrices only carry about seven digits, which would show as rounding noise. */
        if (own_ms(c) == ms) {
            memcpy(mx->comp[k], c->c, sizeof mx->comp[k]);
        } else if (direct_components(c, ms, mx->comp[k])) {
            /* done */
        } else {
            embi_color flat = *c;
            for (int i = 0; i < 3; i++) {
                if (flat.c[i] != flat.c[i]) {
                    flat.c[i] = 0.0;
                }
            }
            double xyz[3];
            if (!to_xyz65(&flat, xyz)) {
                return false;
            }
            from_xyz65(xyz, ms, mx->comp[k]);
        }
        int hc = hue_channel(ms);
        if (hc >= 0 && mx->comp[k][hc] == mx->comp[k][hc] && hue_powerless(ms, mx->comp[k])) {
            mx->comp[k][hc] = __builtin_nan("");
        }
    }
    return true;
}

/* The blend at weights w0 and w1 (they need not sum to 1; they are normalised by their sum). Components are
 * premultiplied by alpha, except the hue (CSS Color 4 section 12.3). */
static void mixer_at(const embi_color_mixer *mx, double w0, double w1, embi_color *out)
{
    double sum = w0 + w1;
    if (sum > 0) {
        w0 /= sum;
        w1 /= sum;
    }
    double a0 = mx->alpha[0] != mx->alpha[0] ? mx->alpha[1] : mx->alpha[0];
    double a1 = mx->alpha[1] != mx->alpha[1] ? mx->alpha[0] : mx->alpha[1];
    if (a0 != a0) {
        a0 = a1 = 1.0;
    }
    double alpha = a0 * w0 + a1 * w1;
    int hc = hue_channel(mx->ms);
    double mixed[3];
    for (int i = 0; i < 3; i++) {
        double x0 = mx->comp[0][i], x1 = mx->comp[1][i];
        if (x0 != x0 && x1 != x1) {
            mixed[i] = 0.0;
            continue;
        }
        if (x0 != x0) {
            x0 = x1;
        } else if (x1 != x1) {
            x1 = x0;
        }
        if (i == hc) {
            /* Hue: adjust one end by a full turn for the chosen arc (CSS Color 4 section 12.4). */
            double d = x1 - x0;
            if (mx->hue_m == 0) {
                if (d > 180) {
                    x0 += 360;
                } else if (d < -180) {
                    x1 += 360;
                }
            } else if (mx->hue_m == 1) {
                if (d > 0 && d < 180) {
                    x0 += 360;
                } else if (d > -180 && d <= 0) {
                    x1 += 360;
                }
            } else if (mx->hue_m == 2) {
                if (d < 0) {
                    x1 += 360;
                }
            } else if (d > 0) {
                x0 += 360;
            }
            double h = w0 * x0 + w1 * x1;
            h = h - 360.0 * __builtin_floor(h / 360.0);
            mixed[i] = h;
        } else if (alpha > 0) {
            mixed[i] = (x0 * a0 * w0 + x1 * a1 * w1) / alpha;
        } else {
            mixed[i] = 0;
        }
    }
    color_from_ms(mx->ms, mixed, out);
    out->alpha = alpha;
}

bool embi_color_mixer_init(embi_color_mixer **out, const embi_color *a, const embi_color *b, uint8_t interp,
                           uint8_t hue)
{
    static const uint8_t map[] = {
        [EMBI_IS_DEFAULT] = MS_SRGB, [EMBI_IS_SRGB] = MS_SRGB, [EMBI_IS_SRGB_LINEAR] = MS_LINEAR,
        [EMBI_IS_DISPLAY_P3] = MS_P3, [EMBI_IS_A98_RGB] = MS_A98, [EMBI_IS_PROPHOTO_RGB] = MS_PROPHOTO,
        [EMBI_IS_REC2020] = MS_REC2020, [EMBI_IS_LAB] = MS_LAB, [EMBI_IS_OKLAB] = MS_OKLAB,
        [EMBI_IS_XYZ_D50] = MS_XYZ50, [EMBI_IS_XYZ_D65] = MS_XYZ65, [EMBI_IS_HSL] = MS_HSL,
        [EMBI_IS_HWB] = MS_HWB, [EMBI_IS_LCH] = MS_LCH, [EMBI_IS_OKLCH] = MS_OKLCH,
        [EMBI_IS_DISPLAY_P3_LINEAR] = MS_P3L,
    };
    if (interp >= sizeof map / sizeof map[0]) {
        return false;
    }
    embi_color_mixer *mx = embi_alloc(sizeof *mx);
    if (!mixer_setup(mx, a, b, map[interp], hue)) {
        embi_free(mx, sizeof *mx);
        return false;
    }
    *out = mx;
    return true;
}

void embi_color_mixer_at(const embi_color_mixer *mx, double w, embi_color *out)
{
    mixer_at(mx, 1.0 - w, w, out);
}

void embi_color_mixer_free(embi_color_mixer *mx)
{
    if (mx != nullptr) {
        embi_free(mx, sizeof *mx);
    }
}

/* currentcolor inside a relative color origin or a color-mix() operand depends on the element's
 * color, so such a color is deferred: parsed with a placeholder first (tl_deferred is set), and
 * evaluated later with tl_current set to the color that currentcolor stands for. */
static _Thread_local const embi_color *tl_current;
static _Thread_local bool tl_deferred;
static _Thread_local uint32_t tl_depth;

static void substitute_current(embi_color *c)
{
    if (c->space != EMBI_CS_CURRENT) {
        return;
    }
    if (tl_current != nullptr) {
        *c = *tl_current;
        return;
    }
    tl_deferred = true;
    memset(c, 0, sizeof *c);
    c->space = EMBI_CS_SRGB;
    c->alpha = 1.0;
}

/* color-mix(in <space> [<hue-method> hue]?, <color> <percent>?, <color> <percent>?). */
static bool parse_color_mix(const embi_css_cv *fn, embi_color *out)
{
    /* Split the top-level arguments at commas. */
    const embi_css_cv *seg[3][8];
    uint32_t seg_n[3] = {0, 0, 0};
    uint32_t nseg = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= fn->nkids; i++) {
        if (i == fn->nkids || fn->kids[i].tok.type == EMBI_CSS_COMMA) {
            if (nseg >= 3) {
                return false;
            }
            for (uint32_t k = start; k < i; k++) {
                if (fn->kids[k].tok.type == EMBI_CSS_WS) {
                    continue;
                }
                if (seg_n[nseg] >= 8) {
                    return false;
                }
                seg[nseg][seg_n[nseg]++] = &fn->kids[k];
            }
            nseg++;
            start = i + 1;
        }
    }
    if (nseg != 3 || seg_n[0] < 2) {
        return false;
    }
    /* "in" <space> [<method> hue]. */
    if (seg[0][0]->tok.type != EMBI_CSS_IDENT || !embi_str_ieq(seg[0][0]->tok.value, EMBI_STR("in"))) {
        return false;
    }
    if (seg[0][1]->tok.type != EMBI_CSS_IDENT) {
        return false;
    }
    uint8_t ms = 0xff;
    for (size_t i = 0; i < sizeof mix_spaces / sizeof mix_spaces[0]; i++) {
        if (embi_str_ieq(seg[0][1]->tok.value,
                         (embi_str){mix_spaces[i].name, (uint32_t)strlen(mix_spaces[i].name)})) {
            ms = mix_spaces[i].ms;
        }
    }
    if (ms == 0xff) {
        return false;
    }
    uint8_t hue_m = 0; /* 0 shorter, 1 longer, 2 increasing, 3 decreasing */
    if (seg_n[0] > 2) {
        if (!is_polar(ms) || seg_n[0] != 4 || seg[0][2]->tok.type != EMBI_CSS_IDENT ||
            seg[0][3]->tok.type != EMBI_CSS_IDENT ||
            !embi_str_ieq(seg[0][3]->tok.value, EMBI_STR("hue"))) {
            return false;
        }
        const embi_str m = seg[0][2]->tok.value;
        if (embi_str_ieq(m, EMBI_STR("longer"))) {
            hue_m = 1;
        } else if (embi_str_ieq(m, EMBI_STR("increasing"))) {
            hue_m = 2;
        } else if (embi_str_ieq(m, EMBI_STR("decreasing"))) {
            hue_m = 3;
        } else if (!embi_str_ieq(m, EMBI_STR("shorter"))) {
            return false;
        }
    }

    embi_color col[2];
    double pct[2] = {-1, -1};
    for (int k = 0; k < 2; k++) {
        const embi_css_cv **s = seg[k + 1];
        uint32_t n = seg_n[k + 1];
        if (n < 1 || n > 2 || !embi_color_parse_cv(s[0], &col[k])) {
            return false;
        }
        substitute_current(&col[k]);
        if (n == 2) {
            if (s[1]->tok.type != EMBI_CSS_PERCENTAGE || s[1]->tok.num < 0 ||
                s[1]->tok.num > 100) {
                return false;
            }
            pct[k] = s[1]->tok.num / 100.0;
        }
    }
    /* Missing percentages: the other one takes the rest, or both are half. */
    if (pct[0] < 0 && pct[1] < 0) {
        pct[0] = pct[1] = 0.5;
    } else if (pct[0] < 0) {
        pct[0] = 1.0 - pct[1];
    } else if (pct[1] < 0) {
        pct[1] = 1.0 - pct[0];
    }
    double sum = pct[0] + pct[1];
    if (sum <= 0) {
        return false;
    }
    double scale = sum > 1.0 ? 1.0 : sum;
    double w0 = pct[0] / sum, w1 = pct[1] / sum;

    embi_color_mixer mx;
    if (!mixer_setup(&mx, &col[0], &col[1], ms, hue_m)) {
        return false;
    }
    mixer_at(&mx, w0, w1, out);
    out->alpha *= scale;
    return true;
}

/* ------------------------------------------------------------------ relative color syntax */

typedef struct relspec {
    uint8_t ms;
    char names[3]; /* channel keywords, in order: lowercase letters */
    double scale;  /* a channel's value is the mix-space component times scale */
} relspec;

static bool relspec_of_space(embi_str s, relspec *rs)
{
    if (embi_str_ieq(s, EMBI_STR("srgb"))) {
        *rs = (relspec){MS_SRGB, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("srgb-linear"))) {
        *rs = (relspec){MS_LINEAR, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("display-p3"))) {
        *rs = (relspec){MS_P3, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("display-p3-linear"))) {
        *rs = (relspec){MS_P3L, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("a98-rgb"))) {
        *rs = (relspec){MS_A98, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("prophoto-rgb"))) {
        *rs = (relspec){MS_PROPHOTO, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("rec2020"))) {
        *rs = (relspec){MS_REC2020, {'r', 'g', 'b'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("xyz")) || embi_str_ieq(s, EMBI_STR("xyz-d65"))) {
        *rs = (relspec){MS_XYZ65, {'x', 'y', 'z'}, 1.0};
    } else if (embi_str_ieq(s, EMBI_STR("xyz-d50"))) {
        *rs = (relspec){MS_XYZ50, {'x', 'y', 'z'}, 1.0};
    } else {
        return false;
    }
    return true;
}

static bool relspec_of_fn(embi_str n, relspec *rs)
{
    if (embi_str_ieq(n, EMBI_STR("rgb")) || embi_str_ieq(n, EMBI_STR("rgba"))) {
        /* Legacy rgb() channels are 0..255 numbers in relative syntax. */
        *rs = (relspec){MS_SRGB, {'r', 'g', 'b'}, 255.0};
    } else if (embi_str_ieq(n, EMBI_STR("hsl")) || embi_str_ieq(n, EMBI_STR("hsla"))) {
        *rs = (relspec){MS_HSL, {'h', 's', 'l'}, 1.0};
    } else if (embi_str_ieq(n, EMBI_STR("hwb"))) {
        *rs = (relspec){MS_HWB, {'h', 'w', 'b'}, 1.0};
    } else if (embi_str_ieq(n, EMBI_STR("lab"))) {
        *rs = (relspec){MS_LAB, {'l', 'a', 'b'}, 1.0};
    } else if (embi_str_ieq(n, EMBI_STR("lch"))) {
        *rs = (relspec){MS_LCH, {'l', 'c', 'h'}, 1.0};
    } else if (embi_str_ieq(n, EMBI_STR("oklab"))) {
        *rs = (relspec){MS_OKLAB, {'l', 'a', 'b'}, 1.0};
    } else if (embi_str_ieq(n, EMBI_STR("oklch"))) {
        *rs = (relspec){MS_OKLCH, {'l', 'c', 'h'}, 1.0};
    } else {
        return false;
    }
    return true;
}

static bool first_is_from(const embi_css_cv *fn)
{
    for (uint32_t i = 0; i < fn->nkids; i++) {
        const embi_css_cv *cv = &fn->kids[i];
        if (cv->tok.type == EMBI_CSS_WS) {
            continue;
        }
        return cv->tok.type == EMBI_CSS_IDENT && embi_str_ieq(cv->tok.value, EMBI_STR("from"));
    }
    return false;
}

/* Copies n component values, replacing channel keywords and alpha with numbers. */
static embi_css_cv *replace_channels(embi_arena *ar, const embi_css_cv *in, uint32_t n,
                                     const relspec *rs, const double vals[3], double alpha)
{
    embi_css_cv *out = embi_arena_alloc(ar, sizeof(embi_css_cv) * (n ? n : 1), alignof(embi_css_cv));
    for (uint32_t i = 0; i < n; i++) {
        out[i] = in[i];
        const embi_css_cv *src = &in[i];
        if (src->tok.type == EMBI_CSS_IDENT) {
            const embi_str id = src->tok.value;
            bool hit = false;
            double v = 0;
            if (id.len == 1) {
                const char c = (char)(id.ptr[0] | 0x20);
                for (int k = 0; k < 3 && !hit; k++) {
                    if (c == rs->names[k]) {
                        v = vals[k];
                        hit = true;
                    }
                }
            } else if (embi_str_ieq(id, EMBI_STR("alpha"))) {
                v = alpha;
                hit = true;
            }
            if (hit) {
                out[i].tok = (embi_css_token){0};
                out[i].tok.type = EMBI_CSS_NUMBER;
                out[i].tok.num = v != v ? 0.0 : v; /* none (NaN) reads as 0 */
                out[i].kids = nullptr;
                out[i].nkids = 0;
                out[i].end = in[i].end;
                continue;
            }
        }
        if (src->nkids > 0) {
            out[i].kids = replace_channels(ar, src->kids, src->nkids, rs, vals, alpha);
        }
    }
    return out;
}

static bool parse_function(const embi_css_cv *cv, embi_color *out);

/* rgb(from <origin> r g b / alpha) and the other functions in relative syntax: the origin color
 * is converted to the function's space, its channels are substituted for the channel keywords,
 * and the rewritten arguments are parsed as the absolute form. */
static bool parse_relative(const embi_css_cv *fn, embi_color *out)
{
    const embi_css_cv *kids = fn->kids;
    const uint32_t n = fn->nkids;
    uint32_t i = 0;
    while (i < n && kids[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    i++; /* the from keyword */
    while (i < n && kids[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    if (i >= n) {
        return false;
    }
    embi_color origin;
    if (!embi_color_parse_cv(&kids[i], &origin)) {
        return false;
    }
    substitute_current(&origin);
    i++;
    while (i < n && kids[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    relspec rs;
    if (embi_str_ieq(fn->tok.value, EMBI_STR("color"))) {
        /* color(from <origin> <space> ...): the space stays in the arguments. */
        if (i >= n || kids[i].tok.type != EMBI_CSS_IDENT || !relspec_of_space(kids[i].tok.value, &rs)) {
            return false;
        }
    } else if (!relspec_of_fn(fn->tok.value, &rs)) {
        return false;
    }
    double xyz[3];
    if (!to_xyz65(&origin, xyz)) {
        return false;
    }
    double comp[3];
    from_xyz65(xyz, rs.ms, comp);
    double vals[3];
    for (int k = 0; k < 3; k++) {
        vals[k] = comp[k] * rs.scale;
    }
    embi_arena ar = {0};
    embi_arena_init(&ar, 1024);
    const uint32_t m = n - i;
    embi_css_cv synth = *fn;
    synth.kids = replace_channels(&ar, kids + i, m, &rs, vals, origin.alpha);
    synth.nkids = m;
    bool ok = parse_function(&synth, out);
    embi_arena_free(&ar);
    return ok;
}

static bool parse_function(const embi_css_cv *cv, embi_color *out)
{
    if (first_is_from(cv)) {
        return parse_relative(cv, out);
    }
    const embi_str n = cv->tok.value;
    if (embi_str_ieq(n, EMBI_STR("rgb")) || embi_str_ieq(n, EMBI_STR("rgba"))) {
        return parse_rgb(cv, out);
    }
    if (embi_str_ieq(n, EMBI_STR("hsl")) || embi_str_ieq(n, EMBI_STR("hsla"))) {
        return parse_hsl(cv, out);
    }
    if (embi_str_ieq(n, EMBI_STR("hwb"))) {
        return parse_hwb(cv, out);
    }
    if (embi_str_ieq(n, EMBI_STR("lab"))) {
        return parse_three(cv, MS_LAB, 100.0, 125.0, 125.0, false, out);
    }
    if (embi_str_ieq(n, EMBI_STR("lch"))) {
        return parse_three(cv, MS_LCH, 100.0, 150.0, 0, true, out);
    }
    if (embi_str_ieq(n, EMBI_STR("oklab"))) {
        return parse_three(cv, MS_OKLAB, 1.0, 0.4, 0.4, false, out);
    }
    if (embi_str_ieq(n, EMBI_STR("oklch"))) {
        return parse_three(cv, MS_OKLCH, 1.0, 0.4, 0, true, out);
    }
    if (embi_str_ieq(n, EMBI_STR("color"))) {
        return parse_color_fn(cv, false, out);
    }
    if (embi_str_ieq(n, EMBI_STR("device-cmyk"))) {
        return parse_color_fn(cv, true, out);
    }
    if (embi_str_ieq(n, EMBI_STR("color-mix"))) {
        return parse_color_mix(cv, out);
    }
    return false;
}

/* Hex digits: #rgb, #rgba, #rrggbb and #rrggbbaa. */
static bool parse_hex(embi_str s, embi_color *out)
{
    if (s.len != 3 && s.len != 4 && s.len != 6 && s.len != 8) {
        return false;
    }
    uint8_t v[4] = {0, 0, 0, 255};
    uint32_t comps = s.len == 3 || s.len == 4 ? 1 : 2;
    uint32_t count = s.len / comps;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t byte = 0;
        for (uint32_t d = 0; d < comps; d++) {
            char c = s.ptr[i * comps + d];
            uint8_t nib = 0;
            if (c >= '0' && c <= '9') {
                nib = (uint8_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nib = (uint8_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                nib = (uint8_t)(c - 'A' + 10);
            } else {
                return false;
            }
            byte = (uint8_t)(byte * 16 + nib);
        }
        if (comps == 1) {
            byte = (uint8_t)(byte * 17);
        }
        v[i] = byte;
    }
    memset(out, 0, sizeof *out);
    out->space = EMBI_CS_SRGB;
    out->c[0] = v[0] / 255.0;
    out->c[1] = v[1] / 255.0;
    out->c[2] = v[2] / 255.0;
    out->alpha = v[3] / 255.0;
    return true;
}

static bool color_parse_cv(const embi_css_cv *cv, embi_color *out);

bool embi_color_parse_cv(const embi_css_cv *cv, embi_color *out)
{
    if (!cv || !out) {
        return false;
    }
    bool top = tl_depth == 0;
    if (top) {
        tl_deferred = false;
    }
    tl_depth++;
    bool ok = color_parse_cv(cv, out);
    tl_depth--;
    if (ok && top && tl_deferred && tl_current == nullptr && cv->tok.type == EMBI_CSS_FUNCTION) {
        embi_sb sb = {0};
        embi_css_serialize_cv(&sb, cv);
        embi_atom at = embi_atom_intern(sb.data, (uint32_t)sb.len);
        embi_sb_free(&sb);
        memset(out, 0, sizeof *out);
        out->space = EMBI_CS_DEFERRED;
        out->alpha = 1.0;
        out->alpha_k = (double)at;
    }
    if (top) {
        tl_deferred = false;
    }
    return ok;
}

bool embi_color_resolve_deferred(const embi_color *c, const embi_color *current, embi_color *out)
{
    if (c->space != EMBI_CS_DEFERRED || current == nullptr || current->space == EMBI_CS_CURRENT ||
        current->space == EMBI_CS_SYSTEM || current->space == EMBI_CS_DEFERRED) {
        return false;
    }
    embi_str text = embi_atom_str((embi_atom)c->alpha_k);
    embi_arena ar = {0};
    embi_arena_init(&ar, 1024);
    embi_css_slice cvs = embi_css_parse_cvs(&ar, text);
    bool ok = false;
    if (cvs.n >= 1) {
        const embi_color *saved = tl_current;
        tl_current = current;
        ok = embi_color_parse_cv(&cvs.p[0], out);
        tl_current = saved;
    }
    embi_arena_free(&ar);
    return ok;
}

static bool color_parse_cv(const embi_css_cv *cv, embi_color *out)
{
    switch (cv->tok.type) {
    case EMBI_CSS_HASH:
        return parse_hex(cv->tok.value, out);
    case EMBI_CSS_IDENT: {
        if (embi_str_ieq(cv->tok.value, EMBI_STR("currentcolor"))) {
            memset(out, 0, sizeof *out);
            out->space = EMBI_CS_CURRENT;
            out->alpha = 1.0;
            return true;
        }
        if (embi_color_named(cv->tok.value, out)) {
            return true;
        }
        uint16_t idx = 0;
        if (embi_color_system(cv->tok.value, &idx)) {
            memset(out, 0, sizeof *out);
            out->space = EMBI_CS_SYSTEM;
            out->sys = idx;
            out->alpha = 1.0;
            return true;
        }
        return false;
    }
    case EMBI_CSS_FUNCTION:
        return parse_function(cv, out);
    default:
        return false;
    }
}

bool embi_color_parse(embi_css_slice in, embi_color *out)
{
    uint32_t start = 0;
    uint32_t end = in.n;
    while (start < end && in.p[start].tok.type == EMBI_CSS_WS) {
        start++;
    }
    while (end > start && in.p[end - 1].tok.type == EMBI_CSS_WS) {
        end--;
    }
    if (end - start != 1) {
        return false;
    }
    return embi_color_parse_cv(&in.p[start], out);
}

/* ------------------------------------------------------------------ serialization */

static const char *const space_names[] = {
    "srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb", "rec2020",
    "xyz-d50", "xyz-d65", "lab", "lch", "oklab", "oklch", "device-cmyk",
};

static void append_alpha(embi_sb *sb, double alpha)
{
    if (alpha != 1.0) {
        embi_sb_append(sb, EMBI_STR(" / "));
        embi_css_append_num(sb, alpha);
    }
}

static void append_str(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

void embi_color_serialize(embi_sb *sb, const embi_color *c)
{
    if (c->space == EMBI_CS_CURRENT) {
        append_str(sb, "currentcolor");
        return;
    }
    if (c->space == EMBI_CS_SYSTEM) {
        append_str(sb, embi_system_color_names[c->sys]);
        return;
    }
    if (c->space == EMBI_CS_DEFERRED) {
        embi_sb_append(sb, embi_atom_str((embi_atom)c->alpha_k));
        return;
    }
    if (c->space == EMBI_CS_SRGB && !(c->flags & EMBI_CF_FUNC)) {
        bool has_alpha = c->alpha != 1.0;
        append_str(sb, has_alpha ? "rgba(" : "rgb(");
        for (int i = 0; i < 3; i++) {
            if (i > 0) {
                append_str(sb, ", ");
            }
            embi_css_append_num(sb, c->c[i] * 255.0);
        }
        if (has_alpha) {
            append_str(sb, ", ");
            embi_css_append_num(sb, c->alpha);
        }
        append_str(sb, ")");
        return;
    }
    if (c->space == EMBI_CS_LAB || c->space == EMBI_CS_LCH || c->space == EMBI_CS_OKLAB ||
        c->space == EMBI_CS_OKLCH) {
        const char *fn = c->space == EMBI_CS_LAB     ? "lab("
                         : c->space == EMBI_CS_LCH   ? "lch("
                         : c->space == EMBI_CS_OKLAB ? "oklab("
                                                     : "oklch(";
        append_str(sb, fn);
        for (int i = 0; i < 3; i++) {
            if (i > 0) {
                append_str(sb, " ");
            }
            embi_css_append_num(sb, c->c[i]);
        }
        append_alpha(sb, c->alpha);
        append_str(sb, ")");
        return;
    }
    /* color(<space> ...) forms: srgb, srgb-linear, the wide gamuts, xyz and device-cmyk. */
    append_str(sb, "color(");
    append_str(sb, c->space == EMBI_CS_DISPLAY_P3_LINEAR ? "display-p3-linear" : space_names[c->space]);
    uint32_t n = c->space == EMBI_CS_DEVICE_CMYK ? 4 : 3;
    for (uint32_t i = 0; i < n; i++) {
        double v = i < 3 ? c->c[i] : c->alpha_k;
        append_str(sb, " ");
        if (v != v) {
            append_str(sb, "none");
        } else {
            embi_css_append_num(sb, v);
        }
    }
    append_alpha(sb, c->alpha);
    append_str(sb, ")");
}
