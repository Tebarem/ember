/*
 * Font registry (R2c): every face of every registered file, with the properties that CSS
 * matching reads. WOFF and WOFF2 are decoded through src/codec first.
 */

#include "text/text_int.h"

#include "codec/codec.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>

/* Largest decoded font we accept (WOFF and WOFF2 expansion is capped here). */
#define EMBI_FONT_MAX_BYTES (32u << 20)
/* Variation instances per face. Instance ids stay below this bound so that cache keys fit. */
#define EMBI_FACE_MAX_INSTANCES (1u << 20)

static SDL_AtomicInt g_face_uid_counter;

static bool is_local_marker(const uint8_t *d, size_t n);
static void lp_add(embi_fontdb *db, const uint8_t *marker, size_t n, const embi_face_desc *d);
static void lp_registered(embi_fontdb *db, const embi_face_desc *d, uint32_t first, uint32_t added);
static void lp_free(embi_lpend *r);

static uint32_t next_face_uid(void)
{
    /* Process-wide and never reused, so two registries can share the renderer's caches. The
     * value stays below 2^23, the field width in the glyph cache key. */
    int prev = SDL_AddAtomicInt(&g_face_uid_counter, 1);
    return (uint32_t)(prev % ((1 << 23) - 1)) + 1u;
}

static bool is_woff1(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == 'w' && d[1] == 'O' && d[2] == 'F' && d[3] == 'F';
}

static bool is_woff2(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == 'w' && d[1] == 'O' && d[2] == 'F' && d[3] == '2';
}

static char *dup_str(embi_str s)
{
    char *p = embi_alloc((size_t)s.len + 1);
    if (s.len > 0) {
        SDL_memcpy(p, s.ptr, s.len);
    }
    p[s.len] = '\0';
    return p;
}

static uint16_t clamp_weight(float w)
{
    if (w - w != 0.0f) {
        return 400; /* NaN or infinity from a bad axis: the CSS initial value */
    }
    if (w < 1.0f) {
        return 1;
    }
    if (w > 1000.0f) {
        return 1000;
    }
    return (uint16_t)(w + 0.5f);
}

static float stretch_from_width_class(uint16_t wc)
{
    /* CSS font-stretch keywords (percent) for usWidthClass 1..9. */
    static const float table[10] = {100.0f, 50.0f,  62.5f, 75.0f, 87.5f,
                                    100.0f, 112.5f, 125.0f, 150.0f, 200.0f};
    return (wc >= 1 && wc <= 9) ? table[wc] : 100.0f;
}

/* Fills the metadata of a freshly opened face from its tables and @font-face overrides. */
static void face_describe(embi_face *f, const embi_face_desc *desc)
{
    embi_font_metrics m;
    SDL_memset(&m, 0, sizeof m);
    embi_font_get_metrics(&f->font, &m);

    f->typo_family = m.typo_family;
    f->family = m.family.len > 0 ? m.family : m.typo_family;

    f->ax_wght = embi_font_axis_index(&f->font, EMBI_TAG('w', 'g', 'h', 't'));
    f->ax_wdth = embi_font_axis_index(&f->font, EMBI_TAG('w', 'd', 't', 'h'));
    f->ax_slnt = embi_font_axis_index(&f->font, EMBI_TAG('s', 'l', 'n', 't'));
    f->ax_ital = embi_font_axis_index(&f->font, EMBI_TAG('i', 't', 'a', 'l'));
    f->ax_opsz = embi_font_axis_index(&f->font, EMBI_TAG('o', 'p', 's', 'z'));

    /* Weight: the wght axis range wins over the OS/2 class for variable faces. */
    uint16_t wc = m.weight_class ? m.weight_class : 400;
    f->weight_min = wc;
    f->weight_max = wc;
    if (f->ax_wght >= 0) {
        const embi_font_axis *ax = &f->font.axes[f->ax_wght];
        f->weight_min = clamp_weight(ax->min);
        f->weight_max = clamp_weight(ax->max);
    }

    /* Stretch: the wdth axis (percent) for variable faces, else usWidthClass. */
    f->stretch_min = f->stretch_max = stretch_from_width_class(m.width_class);
    if (f->ax_wdth >= 0) {
        const embi_font_axis *ax = &f->font.axes[f->ax_wdth];
        f->stretch_min = ax->min;
        f->stretch_max = ax->max;
    }

    /* Style: italic from OS/2 or post. A slnt axis makes the face oblique-capable, which ranks it
     * as an oblique face (it sits behind a real italic when italic is asked for). */
    f->style = EMBI_STYLE_NORMAL;
    if (m.italic || m.italic_angle != 0.0f) {
        f->style = EMBI_STYLE_ITALIC;
    } else if (f->ax_slnt >= 0) {
        f->style = EMBI_STYLE_OBLIQUE;
    }

    f->color = f->font.metrics.has_colr || f->font.cbdt.n > 0 || f->font.sbix.n > 0;

    if (desc != nullptr) {
        if (desc->weight_min != 0 || desc->weight_max != 0) {
            f->weight_min = desc->weight_min ? desc->weight_min : desc->weight_max;
            f->weight_max = desc->weight_max ? desc->weight_max : desc->weight_min;
        }
        if (desc->has_style) {
            f->style = desc->style;
        }
        if (desc->stretch_min != 0.0f || desc->stretch_max != 0.0f) {
            f->stretch_min = desc->stretch_min != 0.0f ? desc->stretch_min : desc->stretch_max;
            f->stretch_max = desc->stretch_max != 0.0f ? desc->stretch_max : desc->stretch_min;
        }
        if (desc->family.len > 0) {
            f->name_buf = dup_str(desc->family);
            f->family = (embi_str){f->name_buf, desc->family.len};
        }
        if (desc->range_count > 0) {
            f->ranges = embi_alloc(sizeof(embi_unicode_range) * desc->range_count);
            SDL_memcpy(f->ranges, desc->ranges, sizeof(embi_unicode_range) * desc->range_count);
            f->range_count = desc->range_count;
        }
    }
}

/* Owned copy of the file bytes (for WOFF/WOFF2 decoding) in SDL memory. */
static uint8_t *sdl_copy(const void *src, size_t n)
{
    uint8_t *p = SDL_malloc(n > 0 ? n : 1);
    if (p != nullptr && n > 0) {
        SDL_memcpy(p, src, n);
    }
    return p;
}

uint32_t embi_fontdb_add(embi_fontdb *db, const uint8_t *data, size_t len, bool owned,
                         const embi_face_desc *desc)
{
    if (db == nullptr || data == nullptr || len == 0) {
        if (owned) {
            SDL_free((void *)data);
        }
        return 0;
    }
    if (is_local_marker(data, len)) {
        /* A local() source looked up before the index was ready (embi_fontdb_local_source): pending, no face yet. */
        lp_add(db, data, len, desc);
        if (owned) {
            SDL_free((void *)data);
        }
        return 0;
    }
    uint32_t faces_before = db->faces.len;
    const uint8_t *bytes = data;
    uint8_t *own_bytes = owned ? (uint8_t *)data : nullptr;

    if (is_woff1(bytes, len) || is_woff2(bytes, len)) {
        embi_buf out = {0};
        bool ok = is_woff1(bytes, len)
                      ? embi_woff1_decode(bytes, len, EMBI_FONT_MAX_BYTES, &out)
                      : embi_woff2_decode(bytes, len, EMBI_FONT_MAX_BYTES, &out);
        SDL_free(own_bytes);
        own_bytes = nullptr;
        if (!ok || out.len == 0) {
            embi_log_warn("text: WOFF/WOFF2 face rejected");
            embi_buf_free(&out);
            return 0;
        }
        own_bytes = sdl_copy(out.data, out.len);
        if (own_bytes == nullptr) {
            embi_buf_free(&out);
            return 0;
        }
        bytes = own_bytes;
        len = out.len;
        embi_buf_free(&out);
    }

    if (own_bytes != nullptr) {
        embi_vec_push(&db->blobs, own_bytes);
    }

    uint32_t faces = embi_font_count(bytes, len);
    uint32_t added = 0;
    for (uint32_t i = 0; i < faces; i++) {
        embi_face *f = embi_calloc(1, sizeof *f);
        if (!embi_font_open(bytes, len, i, &f->font)) {
            embi_free(f, sizeof *f);
            continue;
        }
        f->index = i;
        f->data = (uint8_t *)bytes;
        f->data_len = len;
        f->uid = next_face_uid();
        face_describe(f, desc);
        embi_vec_push(&db->faces, f);
        added++;
    }
    if (added > 0) {
        embi_fontdb_cover_clear(db); /* a new face may cover a code point that was cached as uncovered */
    }
    lp_registered(db, desc, faces_before, added);
    return added;
}

bool embi_fontdb_set_fallback(embi_fontdb *db, const embi_str *families, uint32_t count)
{
    if (db == nullptr || (count > 0 && families == nullptr)) {
        return false;
    }
    for (uint32_t i = 0; i < db->fallback_count; i++) {
        embi_free(db->fallback[i], SDL_strlen(db->fallback[i]) + 1);
    }
    embi_free(db->fallback, sizeof(char *) * db->fallback_count);
    db->fallback = nullptr;
    db->fallback_count = 0;
    embi_fontdb_cover_clear(db);
    if (count == 0) {
        return true;
    }
    db->fallback = embi_calloc(count, sizeof(char *));
    for (uint32_t i = 0; i < count; i++) {
        db->fallback[i] = dup_str(families[i]);
        db->fallback_count++;
    }
    return true;
}

/* Frees the storage of a face and the face. Its owned file bytes (system faces) go with it. */
void embi_face_release(embi_face *f)
{
    embi_shape_face_release(f);
    embi_font_close(&f->font);
    if (f->own != nullptr) {
        SDL_free(f->own);
        f->own = nullptr;
    }
    if (f->name_buf != nullptr) {
        embi_free(f->name_buf, f->family.len + 1);
    }
    if (f->ranges != nullptr) {
        embi_free(f->ranges, sizeof(embi_unicode_range) * f->range_count);
    }
    for (uint32_t i = 0; i < f->inst_count; i++) {
        embi_free(f->insts[i].user, sizeof(float) * f->font.axis_count);
        embi_free(f->insts[i].coords, sizeof(int16_t) * f->font.axis_count);
    }
    embi_free(f->insts, sizeof(embi_face_inst) * f->inst_cap);
    embi_free(f, sizeof *f);
}

void embi_fontdb_destroy(embi_fontdb *db)
{
    if (db == nullptr) {
        return;
    }
    /* The index first: its loaded faces are released by it (no layout may use them any more). */
    embi_sys_stop(db);
    for (uint32_t i = 0; i < db->faces.len; i++) {
        embi_face_release(db->faces.data[i]);
    }
    embi_vec_free(&db->faces);
    for (uint32_t i = 0; i < db->blobs.len; i++) {
        SDL_free(db->blobs.data[i]);
    }
    embi_vec_free(&db->blobs);
    for (uint32_t i = 0; i < db->fallback_count; i++) {
        embi_free(db->fallback[i], SDL_strlen(db->fallback[i]) + 1);
    }
    embi_free(db->fallback, sizeof(char *) * db->fallback_count);
    for (uint32_t g = 0; g < EMBI_GEN_COUNT; g++) {
        for (uint32_t i = 0; i < db->gen_over_n[g]; i++) {
            embi_free(db->gen_over[g][i], SDL_strlen(db->gen_over[g][i]) + 1);
        }
        embi_free(db->gen_over[g], sizeof(char *) * db->gen_over_n[g]);
    }
    embi_hashmap_free(&db->cover);
    for (uint32_t i = 0; i < db->lpend.len; i++) {
        lp_free(&db->lpend.data[i]);
    }
    embi_vec_free(&db->lpend);
    if (db->scratch != nullptr) {
        embi_free(db->scratch, sizeof(uint32_t) * db->scratch_cap);
    }
    embi_free(db, sizeof *db);
}

#if defined(EMBER_NO_DEFAULT_FONT) && defined(EMBI_TEST_DEFAULT_FONT_HOOK)
/* SZ3b, test builds only (EMBER_DEFAULT_FONT=OFF with EMBER_TESTS=ON): a test executable may hand the registry the face the
 * bundled font would have been (tests/support/test_default_font.c), so that the whole suite runs in a build without it. */
static const void *g_test_font;
static size_t g_test_font_size;

void embi_fontdb_test_default(const void *data, size_t size)
{
    g_test_font = data;
    g_test_font_size = size;
}
#endif

embi_fontdb *embi_fontdb_create(void)
{
    embi_fontdb *db = embi_calloc(1, sizeof *db);
    embi_hashmap_init(&db->cover);
#ifndef EMBER_NO_DEFAULT_FONT
    extern const uint8_t embi_default_font_data[];
    extern const size_t embi_default_font_size;
    if (embi_default_font_size > 0) {
        uint32_t before = db->faces.len;
        if (embi_fontdb_add(db, embi_default_font_data, embi_default_font_size, false, nullptr) >
            0) {
            db->default_face = db->faces.data[before];
        }
    }
#elif defined(EMBI_TEST_DEFAULT_FONT_HOOK)
    if (g_test_font != nullptr) {
        uint32_t before = db->faces.len;
        if (embi_fontdb_add(db, g_test_font, g_test_font_size, false, nullptr) > 0) {
            db->default_face = db->faces.data[before];
        }
    }
#endif
    return db;
}

void embi_fontdb_default_if_none(embi_fontdb *db, uint32_t index)
{
    if (db != nullptr && db->default_face == nullptr && index < db->faces.len) {
        db->default_face = db->faces.data[index];
    }
}

void embi_fontdb_set_warn(embi_fontdb *db, void (*fn)(void *user, const char *msg), void *user)
{
    if (db != nullptr) {
        db->warn_fn = fn;
        db->warn_user = user;
    }
}

void embi_fontdb_warn_no_font(embi_fontdb *db)
{
    if (db == nullptr || !SDL_CompareAndSwapAtomicInt(&db->warned, 0, 1)) {
        return;
    }
    static const char msg[] =
        "no font can draw this text: this build has no bundled default font (EMBER_DEFAULT_FONT=OFF) and no system or "
        "registered font covers it; text is not drawn. Register a font with emb_runtime_add_font, or allow system fonts";
    if (db->warn_fn != nullptr) {
        db->warn_fn(db->warn_user, msg);
    } else {
        SDL_Log("ember: %s", msg);
    }
}

uint32_t embi_fontdb_face_count(const embi_fontdb *db)
{
    return db != nullptr ? db->faces.len : 0;
}

embi_face *embi_fontdb_default_face(embi_fontdb *db)
{
    return db != nullptr ? db->default_face : nullptr;
}

const embi_font *embi_face_font(const embi_face *f)
{
    return f != nullptr ? &f->font : nullptr;
}

embi_str embi_face_family(const embi_face *f)
{
    return f != nullptr ? f->family : (embi_str){nullptr, 0};
}

bool embi_face_covers(const embi_face *f, uint32_t cp)
{
    if (f == nullptr) {
        return false;
    }
    if (f->range_count > 0) {
        bool in = false;
        for (uint32_t i = 0; i < f->range_count && !in; i++) {
            in = cp >= f->ranges[i].lo && cp <= f->ranges[i].hi;
        }
        if (!in) {
            return false;
        }
    }
    return embi_font_has_codepoint(&f->font, cp);
}

/* ------------------------------------------------------------------ variation instances */

const float *embi_face_inst_user(const embi_face *f, uint32_t inst)
{
    if (f == nullptr || f->font.axis_count == 0 || inst >= f->inst_count) {
        return nullptr;
    }
    return f->insts[inst].user;
}

float embi_text_run_mvar(const embi_text_run *r, uint32_t tag)
{
    const int16_t *co = r != nullptr ? embi_face_inst_coords(r->face, r->inst) : nullptr;
    float d = 0.0f;
    if (co == nullptr || !embi_font_mvar_delta(&r->face->font, tag, co, &d)) {
        return 0.0f;
    }
    return d;
}

const int16_t *embi_face_inst_coords(const embi_face *f, uint32_t inst)
{
    if (f == nullptr || f->font.axis_count == 0 || inst >= f->inst_count) {
        return nullptr;
    }
    return f->insts[inst].coords;
}

uint32_t embi_face_instance(embi_face *f, const float *user)
{
    uint32_t axes = f->font.axis_count;
    if (axes == 0 || user == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < f->inst_count; i++) {
        if (SDL_memcmp(f->insts[i].user, user, sizeof(float) * axes) == 0) {
            return i;
        }
    }
    if (f->inst_count >= EMBI_FACE_MAX_INSTANCES) {
        embi_log_warn("text: too many variation instances for one face");
        return 0;
    }
    if (f->inst_count == f->inst_cap) {
        uint32_t cap = embi_vec_grow_to(f->inst_cap, f->inst_count + 1);
        f->insts = embi_realloc(f->insts, sizeof(embi_face_inst) * f->inst_cap,
                                sizeof(embi_face_inst) * cap);
        f->inst_cap = cap;
    }
    embi_face_inst *fi = &f->insts[f->inst_count];
    fi->user = embi_alloc(sizeof(float) * axes);
    fi->coords = embi_alloc(sizeof(int16_t) * axes);
    SDL_memcpy(fi->user, user, sizeof(float) * axes);
    if (!embi_font_normalize(&f->font, user, fi->coords)) {
        SDL_memset(fi->coords, 0, sizeof(int16_t) * axes);
    }
    return f->inst_count++;
}

/* ------------------------------------------------------------------ system faces (W4i) */

embi_face *embi_face_open_owned(uint8_t *bytes, size_t len, uint32_t index, const embi_face_desc *desc)
{
    if (bytes == nullptr || len == 0) {
        SDL_free(bytes);
        return nullptr;
    }
    uint8_t *own = bytes;
    size_t n = len;
    if (is_woff1(bytes, len) || is_woff2(bytes, len)) {
        embi_buf out = {0};
        bool ok = is_woff1(bytes, len) ? embi_woff1_decode(bytes, len, EMBI_FONT_MAX_BYTES, &out)
                                       : embi_woff2_decode(bytes, len, EMBI_FONT_MAX_BYTES, &out);
        SDL_free(own);
        own = nullptr;
        if (!ok || out.len == 0) {
            embi_buf_free(&out);
            return nullptr;
        }
        own = sdl_copy(out.data, out.len);
        n = out.len;
        embi_buf_free(&out);
        if (own == nullptr) {
            return nullptr;
        }
    }
    embi_face *f = embi_calloc(1, sizeof *f);
    if (!embi_font_open(own, n, index, &f->font)) {
        embi_free(f, sizeof *f);
        SDL_free(own);
        return nullptr;
    }
    f->index = index;
    f->data = own;
    f->data_len = n;
    f->own = own;
    f->uid = next_face_uid();
    face_describe(f, desc);
    return f;
}

embi_face *embi_fontdb_system_face(embi_fontdb *db, uint32_t i)
{
    if (db == nullptr || db->sys == nullptr) {
        return nullptr;
    }
    embi_face **slot = embi_sys_face_slot(db->sys, i);
    if (slot == nullptr) {
        return nullptr;
    }
    if (*slot != nullptr) {
        return *slot;
    }
    if (embi_sys_bad(db->sys, i)) {
        return nullptr;
    }
    uint8_t *bytes = nullptr;
    size_t len = 0;
    if (!embi_sys_read(db->sys, i, &bytes, &len)) {
        embi_sys_mark_bad(db->sys, i);
        return nullptr;
    }
    uint32_t index = 0;
    (void)embi_sys_path(db->sys, i, &index);
    embi_face *f = embi_face_open_owned(bytes, len, index, nullptr);
    if (f == nullptr) {
        embi_sys_mark_bad(db->sys, i);
        return nullptr;
    }
    f->sys_ent1 = i + 1;
    *slot = f;
    embi_sys_loaded(db->sys, 1, (int64_t)f->data_len);
    return f;
}

uint32_t embi_fontdb_trim_system(embi_fontdb *db, embi_text *t)
{
    if (db == nullptr || db->sys == nullptr || !embi_sys_ready(db)) {
        return 0; /* before the index is published no system face was loaded, and its fields belong to the job */
    }
    uint32_t freed = 0;
    uint32_t n = embi_sys_count(db->sys);
    for (uint32_t i = 0; i < n; i++) {
        embi_face **slot = embi_sys_face_slot(db->sys, i);
        if (slot == nullptr || *slot == nullptr || (*slot)->refs != 0) {
            continue;
        }
        embi_face *f = *slot;
        if (t != nullptr) {
            embi_shape_forget_face(t, f->uid);
        }
        embi_sys_loaded(db->sys, -1, -(int64_t)f->data_len);
        *slot = nullptr;
        embi_face_release(f);
        freed++;
    }
    if (freed > 0) {
        /* The per-character cache may point at the faces just freed. */
        embi_fontdb_cover_clear(db);
    }
    return freed;
}

uint64_t embi_fontdb_fallback_uses(const embi_fontdb *db)
{
    return db != nullptr ? db->fallback_uses : 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The face `index` of a collection (ttcf) as a stand-alone sfnt: a new directory whose tables are copied from the file.
 * Frees bytes and returns the new blob; on any inconsistency the file is returned unchanged (the whole collection). */
static uint8_t *ttc_extract(uint8_t *bytes, size_t *len, uint32_t index)
{
    size_t n = *len;
    if (n < 16 || SDL_memcmp(bytes, "ttcf", 4) != 0 || index >= rd32(bytes + 8) || 12u + 4u * (size_t)index + 4u > n) {
        return bytes;
    }
    size_t off = rd32(bytes + 12 + 4u * (size_t)index);
    if (off > n || n - off < 12) {
        return bytes;
    }
    uint32_t nt = ((uint32_t)bytes[off + 4] << 8) | bytes[off + 5];
    if (nt == 0 || nt > 256 || n - off - 12 < (size_t)nt * 16u) {
        return bytes;
    }
    size_t dir = 12u + (size_t)nt * 16u, total = dir;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = bytes + off + 12 + (size_t)i * 16u;
        size_t to = rd32(r + 8), tl = rd32(r + 12);
        if (to > n || tl > n - to) {
            return bytes;
        }
        total += (tl + 3u) & ~(size_t)3u;
        if (total > EMBI_FONT_MAX_BYTES) {
            return bytes;
        }
    }
    uint8_t *out = SDL_calloc(1, total);
    if (out == nullptr) {
        return bytes;
    }
    SDL_memcpy(out, bytes + off, 12);
    size_t at = dir;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = bytes + off + 12 + (size_t)i * 16u;
        uint8_t *w = out + 12 + (size_t)i * 16u;
        size_t to = rd32(r + 8), tl = rd32(r + 12);
        SDL_memcpy(w, r, 8);
        w[8] = (uint8_t)(at >> 24);
        w[9] = (uint8_t)(at >> 16);
        w[10] = (uint8_t)(at >> 8);
        w[11] = (uint8_t)at;
        SDL_memcpy(w + 12, r + 12, 4);
        SDL_memcpy(out + at, bytes + to, tl);
        at += (tl + 3u) & ~(size_t)3u;
    }
    SDL_free(bytes);
    *len = total;
    return out;
}

bool embi_fontdb_local(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len)
{
    if (db == nullptr || db->sys == nullptr || !embi_sys_ready(db) || name.len == 0) {
        return false;
    }
    uint32_t ids[8];
    uint32_t n = embi_sys_local(db->sys, name, ids, 8);
    for (uint32_t k = 0; k < n; k++) {
        if (embi_sys_read(db->sys, ids[k], data, len)) {
            /* A collection yields the one face that matched, so the @font-face descriptors apply to it alone. */
            uint32_t face = 0;
            (void)embi_sys_path(db->sys, ids[k], &face);
            *data = ttc_extract(*data, len, face);
            return true;
        }
    }
    return false;
}

bool embi_fontdb_set_generic(embi_fontdb *db, uint32_t gen, const embi_str *families, uint32_t count)
{
    if (db == nullptr || gen >= EMBI_GEN_COUNT || (count > 0 && families == nullptr)) {
        return false;
    }
    for (uint32_t i = 0; i < db->gen_over_n[gen]; i++) {
        embi_free(db->gen_over[gen][i], SDL_strlen(db->gen_over[gen][i]) + 1);
    }
    embi_free(db->gen_over[gen], sizeof(char *) * db->gen_over_n[gen]);
    db->gen_over[gen] = nullptr;
    db->gen_over_n[gen] = 0;
    /* The per-character cache holds answers of the old list, removed or replaced: drop them either way. */
    embi_fontdb_cover_clear(db);
    if (count == 0) {
        return true;
    }
    db->gen_over[gen] = embi_calloc(count, sizeof(char *));
    for (uint32_t i = 0; i < count; i++) {
        db->gen_over[gen][i] = dup_str(families[i]);
        db->gen_over_n[gen]++;
    }
    return true;
}

/* ------------------------------------------------------------------ local() before the index is ready */

/* The placeholder blob of a pending local() source: the magic, then the name. No font container starts with it. */
#define LP_MAGIC "EMBLOCL"
#define LP_MAGIC_LEN 8u /* with its NUL */

static bool is_local_marker(const uint8_t *d, size_t n)
{
    return n >= LP_MAGIC_LEN && SDL_memcmp(d, LP_MAGIC, LP_MAGIC_LEN) == 0;
}

static char *dup_cstr_n(const char *s, size_t n)
{
    char *p = embi_alloc(n + 1);
    if (n > 0) {
        SDL_memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

static void lp_free(embi_lpend *r)
{
    embi_free(r->name, SDL_strlen(r->name) + 1);
    embi_free(r->family, SDL_strlen(r->family) + 1);
    if (r->ranges != nullptr) {
        embi_free(r->ranges, sizeof(embi_unicode_range) * r->desc.range_count);
    }
}

/* Whether a record was made from the same descriptors (the same @font-face rule, or one indistinguishable from it). */
static bool lp_desc_eq(const embi_lpend *r, const embi_face_desc *d)
{
    embi_face_desc z;
    if (d == nullptr) {
        SDL_memset(&z, 0, sizeof z);
        d = &z;
    }
    const embi_face_desc *a = &r->desc;
    if (!embi_str_eq(a->family, d->family) || a->weight_min != d->weight_min || a->weight_max != d->weight_max ||
        a->has_style != d->has_style || (a->has_style && a->style != d->style) || a->stretch_min != d->stretch_min ||
        a->stretch_max != d->stretch_max || a->range_count != d->range_count) {
        return false;
    }
    for (uint32_t i = 0; i < a->range_count; i++) {
        if (a->ranges[i].lo != d->ranges[i].lo || a->ranges[i].hi != d->ranges[i].hi) {
            return false;
        }
    }
    return true;
}

/* Closes the open records whose descriptors differ from d (their rule ended without a later source registering). */
static void lp_close_other(embi_fontdb *db, const embi_face_desc *d)
{
    if (db->lp_open < db->lpend.len && !lp_desc_eq(&db->lpend.data[db->lp_open], d)) {
        db->lp_open = db->lpend.len;
    }
}

/* Records a pending local() source of a rule with descriptors d. */
static void lp_add(embi_fontdb *db, const uint8_t *marker, size_t n, const embi_face_desc *d)
{
    lp_close_other(db, d);
    if (db->lp_open == db->lpend.len) {
        db->lp_group++; /* the first pending source of a rule */
    }
    embi_lpend r;
    SDL_memset(&r, 0, sizeof r);
    r.name = dup_cstr_n((const char *)marker + LP_MAGIC_LEN, n - LP_MAGIC_LEN);
    if (d != nullptr) {
        r.desc = *d;
    }
    r.family = dup_cstr_n(r.desc.family.len > 0 ? r.desc.family.ptr : "", r.desc.family.len);
    r.desc.family = (embi_str){r.family, r.desc.family.len};
    if (r.desc.range_count > 0) {
        r.ranges = embi_alloc(sizeof(embi_unicode_range) * r.desc.range_count);
        SDL_memcpy(r.ranges, d->ranges, sizeof(embi_unicode_range) * r.desc.range_count);
    }
    r.desc.ranges = r.ranges;
    r.group = db->lp_group;
    r.state = EMBI_LP_WAITING;
    embi_vec_push(&db->lpend, r);
    db->lp_waiting++;
}

/* A real source registered: the open records of the same rule remember its faces (a hit of theirs supersedes them),
 * and every open record is closed. A failed source closes only the records of another rule. */
static void lp_registered(embi_fontdb *db, const embi_face_desc *d, uint32_t first, uint32_t added)
{
    if (db->lp_open >= db->lpend.len) {
        return;
    }
    lp_close_other(db, d);
    if (added == 0) {
        return;
    }
    for (uint32_t i = db->lp_open; i < db->lpend.len; i++) {
        db->lpend.data[i].alt_first = first;
        db->lpend.data[i].alt_n = added;
    }
    db->lp_open = db->lpend.len;
}

bool embi_fontdb_local_source(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len)
{
    if (db == nullptr || db->sys == nullptr || name.len == 0 || data == nullptr || len == nullptr) {
        return false;
    }
    if (embi_sys_ready(db)) {
        return embi_fontdb_local(db, name, data, len);
    }
    uint8_t *p = SDL_malloc(LP_MAGIC_LEN + name.len);
    if (p == nullptr) {
        return false;
    }
    SDL_memcpy(p, LP_MAGIC, LP_MAGIC_LEN);
    SDL_memcpy(p + LP_MAGIC_LEN, name.ptr, name.len);
    *data = p;
    *len = LP_MAGIC_LEN + name.len;
    return true;
}

/* Resolves the group whose first waiting record is at index i: its names in source order, the first hit registers. */
static void lp_resolve_group(embi_fontdb *db, uint32_t i)
{
    uint32_t group = db->lpend.data[i].group;
    bool hit = false;
    for (uint32_t j = i; j < db->lpend.len && db->lpend.data[j].group == group; j++) {
        embi_lpend *r = &db->lpend.data[j];
        if (r->state != EMBI_LP_WAITING) {
            continue;
        }
        db->lp_waiting--;
        r->state = EMBI_LP_DEAD;
        if (hit) {
            continue; /* an earlier name of the rule won */
        }
        uint8_t *bytes = nullptr;
        size_t n = 0;
        if (!embi_fontdb_local(db, (embi_str){r->name, (uint32_t)SDL_strlen(r->name)}, &bytes, &n)) {
            continue; /* not installed: the next name, else the later source stays */
        }
        /* embi_fontdb_add does not touch lpend (no record is open, and the bytes are a real font). */
        uint32_t added = embi_fontdb_add(db, bytes, n, true, &r->desc);
        r = &db->lpend.data[j];
        if (added == 0) {
            continue;
        }
        r->state = EMBI_LP_RESOLVED;
        hit = true;
        for (uint32_t k = 0; k < r->alt_n && r->alt_first + k < db->faces.len; k++) {
            db->faces.data[r->alt_first + k]->superseded = true;
        }
    }
}

bool embi_fontdb_local_settle(embi_fontdb *db, embi_str family)
{
    if (db == nullptr || db->lp_waiting == 0 || family.len == 0) {
        return false;
    }
    bool ready = embi_sys_ready(db);
    if (ready) {
        db->lp_open = db->lpend.len; /* a lookup runs between rules: no rule is still registering */
    }
    for (uint32_t i = 0; i < db->lpend.len && db->lp_waiting > 0; i++) {
        embi_lpend *r = &db->lpend.data[i];
        if (r->state != EMBI_LP_WAITING || !embi_str_ieq(r->desc.family, family)) {
            continue;
        }
        if (!ready) {
            return true;
        }
        lp_resolve_group(db, i);
    }
    return false;
}
