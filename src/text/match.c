/*
 * CSS font matching (R2c, extended by W4i): CSS Fonts 4 section 5.2 for stretch, style and weight, the candidate list
 * for fallback, the system index and the generic families, the per-character fallback, and the variation coordinates
 * a style asks for.
 *
 * Order of a family list (embi_fontdb_candidates): each family resolves to the registered faces (@font-face and the
 * embedder's) first, then to the system index once it is ready; a CSS generic family resolves to the first face of
 * its list (the embedder's override, else the platform's). The embedder fallback families come next, and the bundled
 * default last. Per-character fallback (embi_fontdb_cover_face) is consulted by shaping for a code point that no
 * candidate covers, before the bundled default.
 */

#include "text/text_int.h"

#include <SDL3/SDL_stdinc.h>

/* Candidates that the per-character fallback examines for one code point before it gives up (each costs a cmap read). */
#define EMBI_COVER_ATTEMPTS 64u
/* Whole-file loads that the per-character fallback may spend on one code point (the cmap probe rules out the rest). */
#define EMBI_COVER_LOADS 4u
/* Entries of one family that one lookup considers. */
#define EMBI_FAMILY_MAX 64u

/* One rank of a face against a desired value: a group (smaller first) and a key inside the
 * group (smaller first). Ranks compare lexicographically. */
typedef struct embi_rank {
    int group;
    float key;
} embi_rank;

static bool rank_less(embi_rank a, embi_rank b)
{
    if (a.group != b.group) {
        return a.group < b.group;
    }
    return a.key < b.key;
}

static float fmin_(float a, float b)
{
    return a < b ? a : b;
}

static float fmax_(float a, float b)
{
    return a > b ? a : b;
}

/* Weight: [lo, hi] of the face against desired w (CSS Fonts 4 section 5.2.4). */
static embi_rank rank_weight(float lo, float hi, float w)
{
    embi_rank r = {0, 0.0f};
    if (w >= 400.0f && w <= 500.0f) {
        if (hi >= w && lo <= 500.0f) {
            r.group = 0;
            r.key = fmax_(lo, w) - w;
        } else if (hi < w) {
            r.group = 1;
            r.key = w - hi;
        } else {
            r.group = 2;
            r.key = lo - 500.0f;
        }
    } else if (w < 400.0f) {
        if (lo <= w) {
            r.group = 0;
            r.key = w - fmin_(hi, w);
        } else {
            r.group = 1;
            r.key = lo - w;
        }
    } else {
        if (hi >= w) {
            r.group = 0;
            r.key = fmax_(lo, w) - w;
        } else {
            r.group = 1;
            r.key = w - hi;
        }
    }
    return r;
}

/* Stretch: the same rule with the threshold 100% (CSS Fonts 4 section 5.2.5). */
static embi_rank rank_stretch(float lo, float hi, float d)
{
    embi_rank r = {0, 0.0f};
    if (d <= 100.0f) {
        if (lo <= d) {
            r.group = 0;
            r.key = d - fmin_(hi, d);
        } else {
            r.group = 1;
            r.key = lo - d;
        }
    } else {
        if (hi >= d) {
            r.group = 0;
            r.key = fmax_(lo, d) - d;
        } else {
            r.group = 1;
            r.key = d - hi;
        }
    }
    return r;
}

/* Style preference lists (CSS Fonts 4 section 5.2.3): the position of the face's style in the
 * list for the desired style. */
static int rank_style(uint8_t face_style, uint8_t want)
{
    static const uint8_t order[3][3] = {
        /* normal:  normal, oblique, italic */
        {EMBI_STYLE_NORMAL, EMBI_STYLE_OBLIQUE, EMBI_STYLE_ITALIC},
        /* italic:  italic, oblique, normal */
        {EMBI_STYLE_ITALIC, EMBI_STYLE_OBLIQUE, EMBI_STYLE_NORMAL},
        /* oblique: oblique, italic, normal */
        {EMBI_STYLE_OBLIQUE, EMBI_STYLE_ITALIC, EMBI_STYLE_NORMAL},
    };
    int row = want == EMBI_STYLE_ITALIC ? 1 : want == EMBI_STYLE_OBLIQUE ? 2 : 0;
    for (int i = 0; i < 3; i++) {
        if (order[row][i] == face_style) {
            return i;
        }
    }
    return 3;
}

typedef struct embi_face_rank {
    embi_rank stretch;
    int style;
    embi_rank weight;
} embi_face_rank;

static bool face_rank_less(embi_face_rank a, embi_face_rank b)
{
    if (rank_less(a.stretch, b.stretch)) {
        return true;
    }
    if (rank_less(b.stretch, a.stretch)) {
        return false;
    }
    if (a.style != b.style) {
        return a.style < b.style;
    }
    return rank_less(a.weight, b.weight);
}

/* The metadata of a registered face, as the rank reads it. */
static embi_fmeta meta_of_face(const embi_face *f)
{
    embi_fmeta m;
    m.wlo = (float)f->weight_min;
    m.whi = (float)f->weight_max;
    m.slo = f->stretch_min;
    m.shi = f->stretch_max;
    m.style = f->style;
    return m;
}

static embi_face_rank rank_of(const embi_fmeta *fm, const embi_style_match *m)
{
    embi_face_rank r;
    r.stretch = rank_stretch(fm->slo, fm->shi, m->stretch);
    r.style = rank_style(fm->style, m->style);
    r.weight = rank_weight(fm->wlo, fm->whi, m->weight);
    return r;
}

static bool family_is(const embi_face *f, embi_str name)
{
    return (f->family.len > 0 && embi_str_ieq(f->family, name)) ||
           (f->typo_family.len > 0 && embi_str_ieq(f->typo_family, name));
}

/* The best registered face of one family for the style, or nullptr when the family has no registered face. */
static embi_face *best_of_family(embi_fontdb *db, embi_str name, const embi_style_match *m)
{
    embi_face *best = nullptr;
    embi_face_rank best_rank = {0};
    for (uint32_t i = 0; i < db->faces.len; i++) {
        embi_face *f = db->faces.data[i];
        if (f->superseded || !family_is(f, name)) {
            continue;
        }
        embi_fmeta fm = meta_of_face(f);
        embi_face_rank r = rank_of(&fm, m);
        if (best == nullptr || face_rank_less(r, best_rank)) {
            best = f;
            best_rank = r;
        }
    }
    return best;
}

static bool list_has(embi_face **list, uint32_t n, const embi_face *f)
{
    for (uint32_t i = 0; i < n; i++) {
        if (list[i] == f) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ the system index */

static bool sys_ready(const embi_fontdb *db)
{
    return db != nullptr && db->sys != nullptr && embi_sys_ready(db);
}

/* True when entry a ranks before entry b for the style. With color_first, colour faces come before the others. */
static bool id_better(embi_fontdb *db, uint32_t a, uint32_t b, const embi_style_match *m, bool color_first)
{
    embi_fmeta fa, fb;
    bool ca = false, cb = false;
    (void)embi_sys_meta(db->sys, a, &fa, &ca);
    (void)embi_sys_meta(db->sys, b, &fb, &cb);
    if (color_first && ca != cb) {
        return ca;
    }
    return face_rank_less(rank_of(&fa, m), rank_of(&fb, m));
}

static void sort_ids(embi_fontdb *db, uint32_t *ids, uint32_t n, const embi_style_match *m, bool color_first)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = ids[i];
        uint32_t j = i;
        while (j > 0 && id_better(db, v, ids[j - 1], m, color_first)) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = v;
    }
}

/* The first entry of the sorted list that loads (and, for cp != 0, covers cp). */
static embi_face *first_loadable(embi_fontdb *db, const uint32_t *ids, uint32_t n, uint32_t cp)
{
    for (uint32_t k = 0; k < n; k++) {
        /* The block bit is a cheap conservative filter: a face whose bit is clear cannot cover cp, and its file is not
         * read at all. */
        if (cp != 0 && !embi_sys_block(db->sys, ids[k], cp)) {
            continue;
        }
        /* A face that is not loaded yet is first checked against its cmap alone. */
        embi_face **slot = embi_sys_face_slot(db->sys, ids[k]);
        if (cp != 0 && slot != nullptr && *slot == nullptr && embi_sys_probe(db->sys, ids[k], cp) == 0) {
            continue;
        }
        embi_face *f = embi_fontdb_system_face(db, ids[k]);
        if (f != nullptr && (cp == 0 || embi_face_covers(f, cp))) {
            return f;
        }
    }
    return nullptr;
}

/* A system face of one family for the style, loaded. */
static embi_face *resolve_system_family(embi_fontdb *db, embi_str name, const embi_style_match *m)
{
    if (!sys_ready(db)) {
        return nullptr;
    }
    uint32_t ids[EMBI_FAMILY_MAX];
    uint32_t n = embi_sys_family(db->sys, name, ids, EMBI_FAMILY_MAX);
    if (n == 0) {
        return nullptr;
    }
    sort_ids(db, ids, n, m, false);
    return first_loadable(db, ids, n, 0);
}

embi_face *embi_fontdb_resolve_family(embi_fontdb *db, embi_str name, const embi_style_match *m)
{
    if (db == nullptr || m == nullptr || name.len == 0) {
        return nullptr;
    }
    /* A local() source of an @font-face of this family that waits for the index: the answer can change when it
     * arrives, so the lookup counts as a fallback (the document lays out again then). Once the index is ready, the
     * pending sources of the family are resolved here, before the registered faces are ranked. */
    if (embi_fontdb_local_settle(db, name)) {
        db->fallback_uses++;
    }
    embi_face *f = best_of_family(db, name, m);
    if (f != nullptr) {
        return f;
    }
    return resolve_system_family(db, name, m);
}

/* The face of a generic family: the first name of its list (the embedder's override, else the platform's) that
 * resolves. */
static embi_face *resolve_generic(embi_fontdb *db, uint32_t gen, const embi_style_match *m)
{
    if (db->gen_over_n[gen] > 0) {
        for (uint32_t i = 0; i < db->gen_over_n[gen]; i++) {
            const char *s = db->gen_over[gen][i];
            embi_face *f = embi_fontdb_resolve_family(db, (embi_str){s, (uint32_t)SDL_strlen(s)}, m);
            if (f != nullptr) {
                return f;
            }
        }
        return nullptr;
    }
    const char *const *list = embi_sys_generic_default(gen);
    for (uint32_t i = 0; list != nullptr && list[i] != nullptr; i++) {
        embi_str s = {list[i], (uint32_t)SDL_strlen(list[i])};
        embi_face *f = resolve_system_family(db, s, m);
        if (f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

uint32_t embi_fontdb_candidates(embi_fontdb *db, const embi_style_match *m, embi_face **out,
                                uint32_t cap)
{
    uint32_t n = 0;
    if (db == nullptr || m == nullptr || out == nullptr || cap == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < m->family_count && n < cap; i++) {
        int g = embi_gen_index(m->families[i]);
        embi_face *f = g >= 0 ? resolve_generic(db, (uint32_t)g, m)
                              : embi_fontdb_resolve_family(db, m->families[i], m);
        if (f != nullptr) {
            if (!list_has(out, n, f)) {
                out[n++] = f;
            }
        } else {
            db->fallback_uses++;
        }
    }
    for (uint32_t i = 0; i < db->fallback_count && n < cap; i++) {
        embi_str name = {db->fallback[i], (uint32_t)SDL_strlen(db->fallback[i])};
        embi_face *f = embi_fontdb_resolve_family(db, name, m);
        if (f != nullptr && !list_has(out, n, f)) {
            out[n++] = f;
        }
    }
    if (db->default_face != nullptr && n < cap && !list_has(out, n, db->default_face)) {
        out[n++] = db->default_face;
    }
    return n;
}

/* ------------------------------------------------------------------ per-character fallback */

/* Code points with default emoji presentation (emoji-data Emoji_Presentation; the main ranges). Such a code point
 * prefers a colour face, as the emoji presentation selector does. */
static bool is_emoji_presentation(uint32_t cp)
{
    static const uint32_t r[][2] = {
        {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
        {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
        {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
        {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A},
        {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F320},
        {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
        {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
        {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
        {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
        {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
        {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    };
    for (uint32_t i = 0; i < sizeof r / sizeof r[0]; i++) {
        if (cp >= r[i][0] && cp <= r[i][1]) {
            return true;
        }
    }
    return false;
}

/* A face of one family that covers cp: registered faces first, then the index. */
static embi_face *cover_in_family(embi_fontdb *db, embi_str name, uint32_t cp, const embi_style_match *m)
{
    (void)embi_fontdb_local_settle(db, name); /* the index is ready here: pending local() faces register now */
    embi_face *best = nullptr;
    embi_face_rank best_rank = {0};
    for (uint32_t i = 0; i < db->faces.len; i++) {
        embi_face *f = db->faces.data[i];
        if (f->superseded || !family_is(f, name) || !embi_face_covers(f, cp)) {
            continue;
        }
        embi_fmeta fm = meta_of_face(f);
        embi_face_rank r = rank_of(&fm, m);
        if (best == nullptr || face_rank_less(r, best_rank)) {
            best = f;
            best_rank = r;
        }
    }
    if (best != nullptr || !sys_ready(db)) {
        return best;
    }
    uint32_t ids[EMBI_FAMILY_MAX];
    uint32_t n = embi_sys_family(db->sys, name, ids, EMBI_FAMILY_MAX);
    sort_ids(db, ids, n, m, is_emoji_presentation(cp));
    return first_loadable(db, ids, n, cp);
}

/* Any system face that covers cp: the entries whose block bit is set, closest first (colour first for emoji).
 *
 * Cost. The candidates go into a scratch array the registry keeps (no allocation per lookup). Up to EMBI_COVER_ATTEMPTS
 * of them are examined, best first. A face that is not loaded yet is probed through its cmap alone (embi_sys_probe:
 * one file open and one table read, no whole-file load), and only a face whose cmap maps cp, or whose cmap cannot be
 * read that way (WOFF), is loaded, at most EMBI_COVER_LOADS times. Worst case for one uncovered code point:
 * EMBI_COVER_ATTEMPTS cmap reads plus EMBI_COVER_LOADS whole-file loads (the latter only for WOFF files or cmaps that
 * claim cp without a usable glyph). The answer is cached per (cp, style, lang) by the caller. */
static embi_face *cover_any(embi_fontdb *db, uint32_t cp, const embi_style_match *m, bool color_only)
{
    uint32_t total = embi_sys_count(db->sys);
    if (total == 0) {
        return nullptr;
    }
    if (db->scratch_cap < total) {
        if (db->scratch != nullptr) {
            embi_free(db->scratch, sizeof(uint32_t) * db->scratch_cap);
        }
        db->scratch = embi_alloc(sizeof(uint32_t) * total);
        db->scratch_cap = total;
    }
    uint32_t *ids = db->scratch;
    uint32_t n = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (embi_sys_block(db->sys, i, cp)) {
            if (color_only) {
                embi_fmeta fm;
                bool col = false;
                (void)embi_sys_meta(db->sys, i, &fm, &col);
                if (!col) {
                    continue;
                }
            }
            ids[n++] = i;
        }
    }
    bool color_first = is_emoji_presentation(cp);
    embi_face *found = nullptr;
    uint32_t loads = 0;
    for (uint32_t attempt = 0; attempt < EMBI_COVER_ATTEMPTS && n > 0 && found == nullptr; attempt++) {
        uint32_t best = 0;
        for (uint32_t k = 1; k < n; k++) {
            if (id_better(db, ids[k], ids[best], m, color_first)) {
                best = k;
            }
        }
        uint32_t id = ids[best];
        ids[best] = ids[--n];
        embi_face **slot = embi_sys_face_slot(db->sys, id);
        if (slot == nullptr || *slot == nullptr) {
            if (embi_sys_bad(db->sys, id) || embi_sys_probe(db->sys, id, cp) == 0) {
                continue; /* its cmap does not map cp: the file is not loaded */
            }
            if (loads == EMBI_COVER_LOADS) {
                continue;
            }
            loads++;
        }
        embi_face *f = embi_fontdb_system_face(db, id);
        if (f != nullptr && embi_face_covers(f, cp)) {
            found = f;
        }
    }
    return found;
}

/* The generic families of a family list, as the list they name (the embedder's override, else the platform's). */
static embi_face *cover_in_generic(embi_fontdb *db, uint32_t gen, uint32_t cp, const embi_style_match *m)
{
    if (db->gen_over_n[gen] > 0) {
        for (uint32_t k = 0; k < db->gen_over_n[gen]; k++) {
            const char *s = db->gen_over[gen][k];
            embi_face *f = cover_in_family(db, (embi_str){s, (uint32_t)SDL_strlen(s)}, cp, m);
            if (f != nullptr) {
                return f;
            }
        }
        return nullptr;
    }
    const char *const *list = embi_sys_generic_default(gen);
    for (uint32_t k = 0; list != nullptr && list[k] != nullptr; k++) {
        embi_face *f = cover_in_family(db, (embi_str){list[k], (uint32_t)SDL_strlen(list[k])}, cp, m);
        if (f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

static embi_face *cover_lookup(embi_fontdb *db, uint32_t cp, const embi_style_match *m, embi_str lang)
{
    /* 0. A default-emoji-presentation code point takes a colour face before any generic family. */
    if (is_emoji_presentation(cp)) {
        embi_face *cf = cover_any(db, cp, m, true);
        if (cf != nullptr) {
            return cf;
        }
    }
    /* 1. The generic family list of the style: the first generic family named, else sans-serif. */
    bool named = false;
    for (uint32_t i = 0; i < m->family_count && !named; i++) {
        int g = embi_gen_index(m->families[i]);
        if (g >= 0) {
            named = true;
            embi_face *f = cover_in_generic(db, (uint32_t)g, cp, m);
            if (f != nullptr) {
                return f;
            }
        }
    }
    if (!named) {
        embi_face *f = cover_in_generic(db, EMBI_GEN_SANS_SERIF, cp, m);
        if (f != nullptr) {
            return f;
        }
    }
    /* 2. The per-script preferences (Han by language, Arabic, Hebrew, Devanagari, Thai and so on). */
    const char *const *pref = embi_sys_script_prefs(cp, lang);
    for (uint32_t k = 0; pref != nullptr && pref[k] != nullptr; k++) {
        embi_face *f = cover_in_family(db, (embi_str){pref[k], (uint32_t)SDL_strlen(pref[k])}, cp, m);
        if (f != nullptr) {
            return f;
        }
    }
    /* 3. Any face that covers the code point. */
    return cover_any(db, cp, m, false);
}

/* Mixes a string into a cache key. */
static uint64_t key_mix(uint64_t h, embi_str s)
{
    return embi_hash64(h ^ embi_hash_bytes(s.ptr, s.len, 0x9E37u));
}

embi_face *embi_fontdb_cover_face(embi_fontdb *db, uint32_t cp, const embi_style_match *m, embi_str lang)
{
    if (db == nullptr || m == nullptr) {
        return nullptr;
    }
    if (!sys_ready(db)) {
        if (db->sys != nullptr) {
            db->fallback_uses++; /* the index is still being built: this lookup is redone when it arrives */
        }
        return nullptr;
    }
    uint64_t h = embi_hash64((uint64_t)cp + 0x51u);
    h = embi_hash64(h ^ (uint64_t)(int64_t)(m->weight * 16.0f));
    h = embi_hash64(h ^ ((uint64_t)m->style << 8) ^ (uint64_t)(int64_t)(m->stretch * 16.0f));
    h = key_mix(h, lang);
    for (uint32_t i = 0; i < m->family_count; i++) {
        h = key_mix(h, m->families[i]);
    }
    uint64_t v = 0;
    if (embi_hashmap_get(&db->cover, h, &v)) {
        return (embi_face *)(uintptr_t)v;
    }
    embi_face *f = cover_lookup(db, cp, m, lang);
    embi_hashmap_put(&db->cover, h, (uint64_t)(uintptr_t)f);
    if (f == nullptr) {
        db->fallback_uses++;
    }
    return f;
}

void embi_fontdb_cover_clear(embi_fontdb *db)
{
    if (db != nullptr) {
        embi_hashmap_clear(&db->cover);
    }
}

/* ------------------------------------------------------------------ variation coordinates */

/* Clamps v to [lo, hi]. A NaN takes lo, so no NaN reaches the variation coordinates. */
static float clampf(float v, float lo, float hi)
{
    if (v != v) {
        return lo;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

void embi_face_user_coords(const embi_face *f, const embi_text_style *st, float *user)
{
    uint32_t axes = f->font.axis_count;
    for (uint32_t i = 0; i < axes; i++) {
        user[i] = f->font.axes[i].def;
    }
    if (axes == 0) {
        return;
    }
    if (f->ax_wght >= 0) {
        const embi_font_axis *ax = &f->font.axes[f->ax_wght];
        user[f->ax_wght] = clampf(st->weight, ax->min, ax->max);
    }
    if (f->ax_wdth >= 0) {
        const embi_font_axis *ax = &f->font.axes[f->ax_wdth];
        user[f->ax_wdth] = clampf(st->stretch, ax->min, ax->max);
    }
    if (f->ax_ital >= 0) {
        const embi_font_axis *ax = &f->font.axes[f->ax_ital];
        user[f->ax_ital] = clampf(st->font_style == EMBI_STYLE_ITALIC ? 1.0f : 0.0f, ax->min,
                                  ax->max);
    }
    if (f->ax_slnt >= 0) {
        /* CSS oblique default: 14 degrees, slnt is negative for a right lean. */
        const embi_font_axis *ax = &f->font.axes[f->ax_slnt];
        if (st->font_style == EMBI_STYLE_OBLIQUE) {
            user[f->ax_slnt] = clampf(-14.0f, ax->min, ax->max);
        }
    }
    if (f->ax_opsz >= 0 && st->optical_sizing) {
        const embi_font_axis *ax = &f->font.axes[f->ax_opsz];
        user[f->ax_opsz] = clampf(st->size, ax->min, ax->max);
    }
    for (uint32_t i = 0; i < st->variation_count; i++) {
        int32_t idx = embi_font_axis_index(&f->font, st->variations[i].tag);
        if (idx >= 0 && st->variations[i].fvalue == st->variations[i].fvalue) {
            const embi_font_axis *ax = &f->font.axes[idx];
            user[idx] = clampf(st->variations[i].fvalue, ax->min, ax->max);
        }
    }
}
