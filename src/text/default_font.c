/*
 * The bundled default face (R2c): assets/fonts/ember-default.ttf, a subset of Noto Sans with
 * Latin, Greek and Cyrillic, punctuation and symbols, and every layout feature. It is the last
 * resort of the fallback chain. EMBER_NO_DEFAULT_FONT (module.cmake) leaves it out.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef EMBER_NO_DEFAULT_FONT
const uint8_t embi_default_font_data[] = {
#embed "../../assets/fonts/ember-default.ttf"
};
const size_t embi_default_font_size = sizeof embi_default_font_data;
#else
const uint8_t embi_default_font_data[1] = {0};
const size_t embi_default_font_size = 0;
#endif
