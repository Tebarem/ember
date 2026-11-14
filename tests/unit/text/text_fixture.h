#pragma once
/*
 * Shared helpers for the text tests (tests/unit/text/test_text_*.c). Not a test itself: the test
 * glob only picks up test_*.c.
 *
 * Fixtures come from tests/fonts (see its README) and assets/fonts, found through
 * EMBER_SOURCE_DIR. Registered files are handed to the registry as owned SDL memory, as
 * SDL_LoadFile returns it.
 */

#include "emb_test.h"

#include "text/text_int.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

/* Registers tests/<rel>, or the path given relative to the source root. Returns the face count. */
static inline uint32_t tx_add_file(embi_text *t, const char *rel, const embi_face_desc *desc)
{
    char path[512];
    size_t n = 0;
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    uint8_t *p = (uint8_t *)SDL_LoadFile(path, &n);
    if (p == nullptr) {
        SDL_Log("cannot read %s: %s", path, SDL_GetError());
        return 0;
    }
    return embi_fontdb_add(embi_text_fonts(t), p, n, true, desc);
}

static inline embi_str tx_s(const char *s)
{
    return (embi_str){s, (uint32_t)SDL_strlen(s)};
}

/* A style with the CSS initial values: 400, normal, 100%, auto kerning, ligatures on. */
static inline void tx_style(embi_text_style *st, const embi_str *families, uint32_t n, float size)
{
    SDL_memset(st, 0, sizeof *st);
    st->families = families;
    st->family_count = n;
    st->size = size;
    st->weight = 400.0f;
    st->font_style = EMBI_STYLE_NORMAL;
    st->stretch = 100.0f;
    st->direction = EMBI_DIR_AUTO;
    st->kerning = EMBI_KERN_AUTO;
    st->ligatures = true;
    st->optical_sizing = true;
}

/* Shapes a UTF-8 literal. The caller frees out with embi_text_layout_free. */
static inline bool tx_shape(embi_text *t, const char *text, const embi_text_style *st,
                            embi_text_layout *out)
{
    SDL_memset(out, 0, sizeof *out);
    return embi_text_shape(t, text, (uint32_t)SDL_strlen(text), st, out);
}
