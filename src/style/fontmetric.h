#pragma once
/* Font metrics for the font-relative units (fontmetric.c). */
#include "base/base.h"

typedef struct embi_fontdb embi_fontdb;

/* Measures the first available face for the given font properties at font_px: out is ex, cap, ch, ic and the normal line height in px (0 for a
 * metric the font does not give; the CSS Values 4 fallbacks then apply). Returns false when no face is found. */
bool embi_style_unit_metrics(embi_fontdb *db, const embi_str *families, uint32_t nfam, float weight, uint8_t style,
                             float stretch, double font_px, double out[5]);
