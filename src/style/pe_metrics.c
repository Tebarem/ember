/*
 * Sizes read from the styles of the parts that have no box: the thickness of a ::-webkit-scrollbar and the length
 * properties of the parts of a range input (::-webkit-slider-runnable-track, ::-webkit-slider-thumb). Layout (the scrollbar
 * gutters) and paint (the bars and the range) read them through these so both see the same numbers.
 */
#include "style_int.h"

#include <math.h>

float embi_style_px(const embi_style *s, embi_prop_id prop)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, prop) : nullptr;
    if (v == nullptr || v->kind != EMBI_V_LENGTH) {
        return NAN;
    }
    float fs = 16.0f;
    const embi_value *f = embi_style_value(s, EMBI_PROP_font_size);
    if (f != nullptr && f->kind == EMBI_V_LENGTH && f->unit == EMBI_U_PX) {
        fs = (float)f->num;
    }
    switch (v->unit) {
    case EMBI_U_PX:
        return (float)v->num;
    case EMBI_U_EM:
        return (float)v->num * fs;
    case EMBI_U_REM:
        return (float)v->num * 16.0f;
    default:
        return NAN;
    }
}

/* The default thickness of a scrollbar whose style gives none (a classic scrollbar of Windows and Chromium). */
#define SB_DEFAULT_THICKNESS 15.0f

void embi_style_scrollbar_thickness(const embi_style *sb, float *vert, float *horz)
{
    *vert = 0.0f;
    *horz = 0.0f;
    if (sb == nullptr) {
        return;
    }
    const embi_value *d = embi_style_value(sb, EMBI_PROP_display);
    if (d != nullptr && d->kind == EMBI_V_KEYWORD && d->kw == EMBI_KW_none) {
        *vert = *horz = -1.0f; /* hidden: no bar and no gutter */
        return;
    }
    float w = embi_style_px(sb, EMBI_PROP_width), h = embi_style_px(sb, EMBI_PROP_height);
    float dflt = !isnan(w) ? w : (!isnan(h) ? h : SB_DEFAULT_THICKNESS);
    *vert = isnan(w) ? dflt : (w > 0.0f ? w : 0.0f);
    *horz = isnan(h) ? dflt : (h > 0.0f ? h : 0.0f);
    if (*vert == 0.0f && *horz == 0.0f) {
        *vert = *horz = -1.0f;
    }
}
