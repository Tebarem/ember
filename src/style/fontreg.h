#pragma once
/*
 * The plain descriptor of one @font-face face, and its registration with the font registry. This
 * header depends on base and the font registry only, so the TU that calls into text/ never sees the
 * CSS declarations (the two headers define colliding names). The CSS side (fontparse.c) fills it.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_fontdb embi_fontdb;

typedef struct embi_style_range {
    uint32_t lo, hi; /* inclusive */
} embi_style_range;

typedef struct embi_style_font_desc {
    embi_str family;
    uint16_t weight_min, weight_max; /* 0: from the font */
    bool has_style;
    uint8_t style;                   /* 0 normal, 1 italic, 2 oblique, when has_style */
    float stretch_min, stretch_max;  /* percent; 0: from the font */
    const embi_style_range *ranges;  /* unicode-range; nranges == 0 means every code point */
    uint32_t nranges;
    const embi_str *urls;            /* the sources, in order (each already format-checked) */
    uint32_t nurls;
} embi_style_font_desc;

/* Fetches each source through load (base is the sheet base URL, possibly empty) and registers the
 * first one that yields a face. Returns the number of faces added. */
uint32_t embi_style_font_register(embi_fontdb *fonts,
                                  bool (*load)(void *, embi_str, embi_str, uint8_t **, size_t *),
                                  void *user, embi_str base, const embi_style_font_desc *d);
