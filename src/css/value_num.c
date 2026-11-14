/*
 * Dimension units: lengths, angles, times and resolutions. Absolute units are converted to the
 * canonical unit at parse time (px, deg, s, dppx); relative units are kept for layout.
 */
#include "css/props.h"

#include <string.h>

typedef struct unit_def {
    const char *name;
    uint8_t unit;
    uint8_t cls;
    double scale;
} unit_def;

#define PX_PER_IN 96.0

static const unit_def unit_defs[] = {
    {"px", EMBI_U_PX, EMBI_CD_LEN, 1.0},
    {"cm", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN / 2.54},
    {"mm", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN / 25.4},
    {"q", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN / 101.6},
    {"in", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN},
    {"pc", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN / 6.0},
    {"pt", EMBI_U_PX, EMBI_CD_LEN, PX_PER_IN / 72.0},
    {"em", EMBI_U_EM, EMBI_CD_LEN, 1.0},
    {"rem", EMBI_U_REM, EMBI_CD_LEN, 1.0},
    {"ex", EMBI_U_EX, EMBI_CD_LEN, 1.0},
    {"rex", EMBI_U_REX, EMBI_CD_LEN, 1.0},
    {"cap", EMBI_U_CAP, EMBI_CD_LEN, 1.0},
    {"rcap", EMBI_U_RCAP, EMBI_CD_LEN, 1.0},
    {"ch", EMBI_U_CH, EMBI_CD_LEN, 1.0},
    {"rch", EMBI_U_RCH, EMBI_CD_LEN, 1.0},
    {"ic", EMBI_U_IC, EMBI_CD_LEN, 1.0},
    {"ric", EMBI_U_RIC, EMBI_CD_LEN, 1.0},
    {"lh", EMBI_U_LH, EMBI_CD_LEN, 1.0},
    {"rlh", EMBI_U_RLH, EMBI_CD_LEN, 1.0},
    {"vw", EMBI_U_VW, EMBI_CD_LEN, 1.0},
    {"vh", EMBI_U_VH, EMBI_CD_LEN, 1.0},
    {"vi", EMBI_U_VI, EMBI_CD_LEN, 1.0},
    {"vb", EMBI_U_VB, EMBI_CD_LEN, 1.0},
    {"vmin", EMBI_U_VMIN, EMBI_CD_LEN, 1.0},
    {"vmax", EMBI_U_VMAX, EMBI_CD_LEN, 1.0},
    {"svw", EMBI_U_SVW, EMBI_CD_LEN, 1.0},
    {"svh", EMBI_U_SVH, EMBI_CD_LEN, 1.0},
    {"svi", EMBI_U_SVI, EMBI_CD_LEN, 1.0},
    {"svb", EMBI_U_SVB, EMBI_CD_LEN, 1.0},
    {"svmin", EMBI_U_SVMIN, EMBI_CD_LEN, 1.0},
    {"svmax", EMBI_U_SVMAX, EMBI_CD_LEN, 1.0},
    {"lvw", EMBI_U_LVW, EMBI_CD_LEN, 1.0},
    {"lvh", EMBI_U_LVH, EMBI_CD_LEN, 1.0},
    {"lvi", EMBI_U_LVI, EMBI_CD_LEN, 1.0},
    {"lvb", EMBI_U_LVB, EMBI_CD_LEN, 1.0},
    {"lvmin", EMBI_U_LVMIN, EMBI_CD_LEN, 1.0},
    {"lvmax", EMBI_U_LVMAX, EMBI_CD_LEN, 1.0},
    {"dvw", EMBI_U_DVW, EMBI_CD_LEN, 1.0},
    {"dvh", EMBI_U_DVH, EMBI_CD_LEN, 1.0},
    {"dvi", EMBI_U_DVI, EMBI_CD_LEN, 1.0},
    {"dvb", EMBI_U_DVB, EMBI_CD_LEN, 1.0},
    {"dvmin", EMBI_U_DVMIN, EMBI_CD_LEN, 1.0},
    {"dvmax", EMBI_U_DVMAX, EMBI_CD_LEN, 1.0},
    {"cqw", EMBI_U_CQW, EMBI_CD_LEN, 1.0},
    {"cqh", EMBI_U_CQH, EMBI_CD_LEN, 1.0},
    {"cqi", EMBI_U_CQI, EMBI_CD_LEN, 1.0},
    {"cqb", EMBI_U_CQB, EMBI_CD_LEN, 1.0},
    {"cqmin", EMBI_U_CQMIN, EMBI_CD_LEN, 1.0},
    {"cqmax", EMBI_U_CQMAX, EMBI_CD_LEN, 1.0},
    {"deg", EMBI_U_DEG, EMBI_CD_ANGLE, 1.0},
    {"grad", EMBI_U_DEG, EMBI_CD_ANGLE, 0.9},
    {"rad", EMBI_U_DEG, EMBI_CD_ANGLE, 180.0 / SDL_PI_D},
    {"turn", EMBI_U_DEG, EMBI_CD_ANGLE, 360.0},
    {"s", EMBI_U_S, EMBI_CD_TIME, 1.0},
    {"ms", EMBI_U_S, EMBI_CD_TIME, 0.001},
    {"dppx", EMBI_U_DPPX, EMBI_CD_RES, 1.0},
    {"x", EMBI_U_DPPX, EMBI_CD_RES, 1.0},
    {"dpi", EMBI_U_DPPX, EMBI_CD_RES, 1.0 / PX_PER_IN},
    {"dpcm", EMBI_U_DPPX, EMBI_CD_RES, 2.54 / PX_PER_IN},
};

static const char *const unit_names[EMBI_U_COUNT] = {
    "", "px", "em", "rem", "ex", "rex", "cap", "rcap", "ch", "rch", "ic", "ric", "lh", "rlh",
    "vw", "vh", "vi", "vb", "vmin", "vmax",
    "svw", "svh", "svi", "svb", "svmin", "svmax",
    "lvw", "lvh", "lvi", "lvb", "lvmin", "lvmax",
    "dvw", "dvh", "dvi", "dvb", "dvmin", "dvmax",
    "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax",
    "deg", "s", "dppx",
};

bool embi_unit_lookup(embi_str s, uint8_t *unit, uint8_t *cls, double *scale)
{
    if (s.len == 0 || s.len > 8) {
        return false;
    }
    char buf[9];
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.ptr[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[s.len] = '\0';
    for (size_t i = 0; i < sizeof unit_defs / sizeof unit_defs[0]; i++) {
        if (strcmp(unit_defs[i].name, buf) == 0) {
            *unit = unit_defs[i].unit;
            *cls = unit_defs[i].cls;
            *scale = unit_defs[i].scale;
            return true;
        }
    }
    return false;
}

const char *embi_unit_name(uint8_t unit)
{
    if (unit >= EMBI_U_COUNT) {
        return "";
    }
    return unit_names[unit];
}
