/*
 * Text system object (R2c): creation, destruction and cache control. The shaping, line and draw
 * code live in their own files.
 */

#include "text/text_int.h"

bool embi_text_has_default_font(void)
{
#ifndef EMBER_NO_DEFAULT_FONT
    return true;
#else
    return false;
#endif
}

embi_text *embi_text_create(void)
{
    embi_bidi_init();
    embi_text *t = embi_calloc(1, sizeof *t);
    t->db = embi_fontdb_create();
    embi_shape_init(t);
    return t;
}

void embi_text_destroy(embi_text *t)
{
    if (t == nullptr) {
        return;
    }
    embi_outline_fini(t);
    embi_shape_fini(t);
    embi_fontdb_destroy(t->db);
    embi_free(t, sizeof *t);
}

embi_fontdb *embi_text_fonts(embi_text *t)
{
    return t != nullptr ? t->db : nullptr;
}

void embi_text_set_shape_cache_budget(embi_text *t, size_t bytes)
{
    if (t != nullptr) {
        embi_shape_set_budget(t, bytes);
    }
}

void embi_text_clear_caches(embi_text *t)
{
    if (t != nullptr) {
        embi_shape_clear(t);
        embi_outline_clear(t);
    }
}
