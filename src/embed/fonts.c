/*
 * The font registry entry point of the embedding layer (W4e, W4i). It is the only unit of the module that includes the text
 * registry header, which clashes with the style header, so it stays on its own.
 */
#include "embed_int.h"

#include "text/text.h"
#include "text/text_int.h"

#include <SDL3/SDL_stdinc.h>

uint32_t embi_fonts_add(embi_fontdb *db, const uint8_t *data, size_t len, bool owned)
{
    return embi_fontdb_add(db, data, len, owned, nullptr);
}

void embi_fonts_system_start(embi_fontdb *db, bool enabled)
{
    if (!enabled) {
        return;
    }
    /* The index is built on a job thread, cached in SDL user storage, from the platform's font directories. */
    embi_sys_opts opts;
    SDL_memset(&opts, 0, sizeof opts);
    opts.cache = true;
    embi_sys_start(db, &opts);
}

/* Registers the sink of bitmap colour glyphs (gfx.c declares the same function: gfx.c cannot include the text header). */
void embi_fonts_set_bitmap_sink(embi_text *t, embi_text_bitmap_fn fn, void *user);
void embi_fonts_set_bitmap_sink(embi_text *t, embi_text_bitmap_fn fn, void *user)
{
    embi_text_set_bitmap_sink(t, fn, user);
}

uint32_t embi_fonts_face_count(const embi_fontdb *db)
{
    return embi_fontdb_face_count(db);
}

void embi_fonts_test_dirs(const char *const *dirs, uint32_t n, uint32_t delay_ms)
{
    embi_sysfont_set_dirs(dirs, n, delay_ms);
}

uint64_t embi_fonts_sys_epoch(const embi_fontdb *fonts)
{
    return embi_fontdb_sys_epoch(fonts);
}

uint64_t embi_fonts_fallback_uses(const embi_fontdb *fonts)
{
    return embi_fontdb_fallback_uses(fonts);
}

bool embi_fonts_local(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len)
{
    /* Before the index is ready this hands out a placeholder that the registry keeps as a pending local() source of the
     * @font-face (no face is added, so the rule's next source is still tried); it is resolved at match time once the
     * index arrives, and the document's relayout on the index epoch picks the system face up. */
    return embi_fontdb_local_source(db, name, data, len);
}

/* Test hook (declared by its tests, the embedding header is not this unit's): the weight of the face that family resolves
 * to for a normal 400 style, registered or system face; 0 when it resolves to none. */
uint32_t embi_fonts_family_weight(embi_fontdb *db, const char *family);
uint32_t embi_fonts_family_weight(embi_fontdb *db, const char *family)
{
    embi_str name = {family, (uint32_t)SDL_strlen(family)};
    embi_style_match m = {&name, 1, 400.0f, EMBI_STYLE_NORMAL, 100.0f};
    embi_face *f = embi_fontdb_resolve_family(db, name, &m);
    return f != nullptr ? f->weight_min : 0u;
}

uint32_t embi_fonts_trim(embi_fontdb *db, embi_text *t)
{
    return embi_fontdb_trim_system(db, t);
}

void embi_fonts_default_if_none(embi_fontdb *db, uint32_t index)
{
    embi_fontdb_default_if_none(db, index);
}

void embi_fonts_set_warn(embi_fontdb *db, void (*fn)(void *user, const char *msg), void *user)
{
    embi_fontdb_set_warn(db, fn, user);
}

bool embi_fonts_generic(embi_fontdb *db, const char *generic, const char *const *families, size_t count)
{
    if (generic == nullptr || (count > 0 && families == nullptr)) {
        return false;
    }
    int g = embi_gen_index((embi_str){generic, (uint32_t)SDL_strlen(generic)});
    if (g < 0) {
        return false;
    }
    embi_str *v = count > 0 ? SDL_malloc(sizeof(embi_str) * count) : nullptr;
    if (count > 0 && v == nullptr) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        v[i] = (embi_str){families[i], (uint32_t)SDL_strlen(families[i])};
    }
    bool ok = embi_fontdb_set_generic(db, (uint32_t)g, v, (uint32_t)count);
    SDL_free(v);
    return ok;
}

void embi_fonts_stats(const embi_fontdb *db, bool enabled, emb_runtime_stats *out)
{
    embi_sys_stats s;
    embi_sys_stats_get(db, &s);
    out->system_fonts = enabled ? 1u : 0u;
    out->system_index_ready = s.ready ? 1u : 0u;
    out->system_index_warm = s.warm ? 1u : 0u;
    out->system_files = s.files;
    out->system_index_entries = s.entries;
    out->system_index_reparsed = s.reparsed;
    out->system_faces_loaded = s.faces_loaded;
    out->system_font_bytes = s.font_bytes;
    out->system_index_bytes = s.index_bytes;
    out->system_index_ms = s.index_ms;
}
