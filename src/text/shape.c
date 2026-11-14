/*
 * Shaping (R2c): itemization (SheenBidi levels x kb script runs x fonts) and OpenType shaping
 * through kb_text_shape's direct API, with a byte-capped LRU cache of shaping results.
 *
 * Cache entries hold font units and cluster offsets relative to the run start, so one entry
 * serves the same text at any position and at any size.
 */

#include "text/text_int.h"

#include "kb_text_shape.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_SHAPE_DEFAULT_BUDGET (4u << 20)
#define EMBI_FEATURE_MAX 64

typedef struct shaped_glyph {
    uint32_t gid;
    uint32_t cluster; /* byte offset from the run start */
    int32_t adv;      /* font units */
    int32_t offx, offy;
} shaped_glyph;

typedef struct shape_entry {
    uint64_t hash;
    uint32_t face_uid, inst, script, lang;
    uint64_t feat;
    uint8_t rtl;
    uint32_t text_len;
    uint8_t *text;
    uint32_t glyph_count;
    shaped_glyph *glyphs;
    struct shape_entry *prev, *next; /* LRU: head is the most recent */
} shape_entry;

/* A kb shape config kept for the next run of the same face instance, script and language. Creating one reads
 * the font's GSUB and GPOS lookup lists, which cost about 20 us per run, ten times the shaping itself (W3q). */
typedef struct cfg_entry {
    uint32_t uid, inst;
    kbts_script script;
    kbts_language lang;
    kbts_font *font;
    kbts_shape_config *cfg;
} cfg_entry;

#define EMBI_SHAPE_CFG_MAX 128u

typedef struct shape_state {
    kbts_glyph_storage storage; /* zeroed storage initializes itself on first use */
    cfg_entry cfgs[EMBI_SHAPE_CFG_MAX];
    uint32_t ncfg;
    embi_hashmap map;           /* hash -> shape_entry* */
    shape_entry *head, *tail;
    size_t bytes, budget;
    uint64_t hits, misses;
} shape_state;

/* ------------------------------------------------------------------ kb font per face */

static kbts_font *kb_font_of(embi_face *f)
{
    if (f->kb != nullptr) {
        return f->kb;
    }
    if (f->kb_failed) {
        return nullptr;
    }
    kbts_font font = kbts_FontFromMemory(f->data, (int)f->data_len, (int)f->index, nullptr,
                                         nullptr);
    if (!kbts_FontIsValid(&font)) {
        kbts_FreeFont(&font);
        f->kb_failed = true;
        return nullptr;
    }
    kbts_font *p = embi_alloc(sizeof *p);
    *p = font;
    f->kb = p;
    return p;
}

void embi_shape_face_release(embi_face *f)
{
    if (f->kb != nullptr) {
        kbts_FreeFont((kbts_font *)f->kb);
        embi_free(f->kb, sizeof(kbts_font));
        f->kb = nullptr;
    }
}

/* OpenType tags are big-endian in font.h; kb packs them little-endian. */
static uint32_t kb_tag(uint32_t tag)
{
    return KBTS_FOURCC((tag >> 24) & 0xFF, (tag >> 16) & 0xFF, (tag >> 8) & 0xFF, tag & 0xFF);
}

/* Primary language subtag to kb's language system (drives the locl feature). Languages kb does
 * not know, and tags without a two-letter primary subtag, give DONT_KNOW. */
typedef struct lang_entry {
    char tag[3];
    kbts_language lang;
} lang_entry;

static const lang_entry lang_table[] = {
    {"ar", KBTS_LANGUAGE_ARABIC},      {"be", KBTS_LANGUAGE_BELARUSIAN},
    {"bg", KBTS_LANGUAGE_BULGARIAN},   {"bn", KBTS_LANGUAGE_BANGLA},
    {"bs", KBTS_LANGUAGE_BOSNIAN},     {"ca", KBTS_LANGUAGE_CATALAN},
    {"cs", KBTS_LANGUAGE_CZECH},       {"cy", KBTS_LANGUAGE_WELSH},
    {"da", KBTS_LANGUAGE_DANISH},      {"de", KBTS_LANGUAGE_GERMAN},
    {"el", KBTS_LANGUAGE_GREEK},       {"en", KBTS_LANGUAGE_ENGLISH},
    {"es", KBTS_LANGUAGE_SPANISH},     {"et", KBTS_LANGUAGE_ESTONIAN},
    {"fa", KBTS_LANGUAGE_PERSIAN},     {"fi", KBTS_LANGUAGE_FINNISH},
    {"fr", KBTS_LANGUAGE_FRENCH},      {"ga", KBTS_LANGUAGE_IRISH},
    {"gl", KBTS_LANGUAGE_GALICIAN},    {"he", KBTS_LANGUAGE_HEBREW},
    {"hi", KBTS_LANGUAGE_HINDI},       {"hr", KBTS_LANGUAGE_CROATIAN},
    {"hu", KBTS_LANGUAGE_HUNGARIAN},   {"hy", KBTS_LANGUAGE_ARMENIAN},
    {"is", KBTS_LANGUAGE_ICELANDIC},   {"it", KBTS_LANGUAGE_ITALIAN},
    {"ja", KBTS_LANGUAGE_JAPANESE},    {"kk", KBTS_LANGUAGE_KAZAKH},
    {"km", KBTS_LANGUAGE_KHMER},       {"ko", KBTS_LANGUAGE_KOREAN},
    {"lo", KBTS_LANGUAGE_LAO},         {"lt", KBTS_LANGUAGE_LITHUANIAN},
    {"lv", KBTS_LANGUAGE_LATVIAN},     {"mk", KBTS_LANGUAGE_MACEDONIAN},
    {"mn", KBTS_LANGUAGE_MONGOLIAN},   {"mr", KBTS_LANGUAGE_MARATHI},
    {"mt", KBTS_LANGUAGE_MALTESE},     {"nb", KBTS_LANGUAGE_NORWEGIAN},
    {"nl", KBTS_LANGUAGE_DUTCH},       {"no", KBTS_LANGUAGE_NORWEGIAN},
    {"pl", KBTS_LANGUAGE_POLISH},      {"pt", KBTS_LANGUAGE_PORTUGUESE},
    {"rm", KBTS_LANGUAGE_ROMANSH},     {"ro", KBTS_LANGUAGE_ROMANIAN},
    {"ru", KBTS_LANGUAGE_RUSSIAN},     {"sk", KBTS_LANGUAGE_SLOVAK},
    {"sl", KBTS_LANGUAGE_SLOVENIAN},   {"sr", KBTS_LANGUAGE_SERBIAN},
    {"sv", KBTS_LANGUAGE_SWEDISH},     {"ta", KBTS_LANGUAGE_TAMIL},
    {"th", KBTS_LANGUAGE_THAI},        {"tr", KBTS_LANGUAGE_TURKISH},
    {"uk", KBTS_LANGUAGE_UKRAINIAN},   {"ur", KBTS_LANGUAGE_URDU},
    {"vi", KBTS_LANGUAGE_VIETNAMESE},
};

/* Subtag equality, ASCII case-insensitive. lit is a NUL-terminated lowercase literal. */
static bool subtag_is(const char *s, uint32_t n, const char *lit)
{
    if (n != SDL_strlen(lit)) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

/* zh: Traditional when a subtag names the Hant script or a Traditional-using region (TW, HK, MO). */
static kbts_language chinese_of(embi_str lang)
{
    uint32_t i = 0;
    while (i < lang.len && lang.ptr[i] != '-' && lang.ptr[i] != '_') {
        i++;
    }
    while (i < lang.len) {
        i++;
        uint32_t b = i;
        while (i < lang.len && lang.ptr[i] != '-' && lang.ptr[i] != '_') {
            i++;
        }
        const char *sub = lang.ptr + b;
        uint32_t n = i - b;
        if (subtag_is(sub, n, "hant") || subtag_is(sub, n, "tw") || subtag_is(sub, n, "hk") ||
            subtag_is(sub, n, "mo")) {
            return KBTS_LANGUAGE_CHINESE_TRADITIONAL;
        }
    }
    return KBTS_LANGUAGE_CHINESE_SIMPLIFIED;
}

static kbts_language lang_of(embi_str lang)
{
    uint32_t n = 0;
    while (n < lang.len && lang.ptr[n] != '-' && lang.ptr[n] != '_') {
        n++;
    }
    if (n != 2) {
        return KBTS_LANGUAGE_DONT_KNOW;
    }
    if (subtag_is(lang.ptr, n, "zh")) {
        return chinese_of(lang);
    }
    for (uint32_t i = 0; i < sizeof lang_table / sizeof lang_table[0]; i++) {
        if (subtag_is(lang.ptr, n, lang_table[i].tag)) {
            return lang_table[i].lang;
        }
    }
    return KBTS_LANGUAGE_DONT_KNOW;
}

/* ------------------------------------------------------------------ feature overrides */

typedef struct feature_list {
    kbts_feature_override items[EMBI_FEATURE_MAX];
    uint32_t n;
    bool overflow; /* entries beyond EMBI_FEATURE_MAX were dropped */
} feature_list;

static void feature_set(feature_list *fl, uint32_t kbtag, int value)
{
    for (uint32_t i = 0; i < fl->n; i++) {
        if (fl->items[i].Tag == kbtag) {
            fl->items[i].Value = value;
            return;
        }
    }
    if (fl->n < EMBI_FEATURE_MAX) {
        fl->items[fl->n].Tag = kbtag;
        fl->items[fl->n].Value = value;
        fl->n++;
    } else {
        fl->overflow = true;
    }
}

/* Feature overrides from the style: font-kerning and ligatures first, then the explicit
 * font-feature-settings, which win (CSS Fonts 4 section 6.2). Non-zero letter-spacing turns off
 * the optional ligatures (CSS Text 3 section 8.2). */
static void build_features(const embi_text_style *st, feature_list *fl, bool caps_features)
{
    fl->n = 0;
    fl->overflow = false;
    if (caps_features && (st->caps == EMBI_CAPS_SMALL || st->caps == EMBI_CAPS_ALL_SMALL)) {
        feature_set(fl, KBTS_FOURCC('s', 'm', 'c', 'p'), 1);
        if (st->caps == EMBI_CAPS_ALL_SMALL) {
            feature_set(fl, KBTS_FOURCC('c', '2', 's', 'c'), 1);
        }
    } else if (st->caps == EMBI_CAPS_UNICASE) {
        feature_set(fl, KBTS_FOURCC('u', 'n', 'i', 'c'), 1);
    } else if (st->caps == EMBI_CAPS_TITLING) {
        feature_set(fl, KBTS_FOURCC('t', 'i', 't', 'l'), 1);
    }
    if (st->kerning == EMBI_KERN_NONE) {
        feature_set(fl, KBTS_FOURCC('k', 'e', 'r', 'n'), 0);
    } else if (st->kerning == EMBI_KERN_NORMAL) {
        feature_set(fl, KBTS_FOURCC('k', 'e', 'r', 'n'), 1);
    }
    if (!st->ligatures || st->letter_spacing != 0.0f) {
        feature_set(fl, KBTS_FOURCC('l', 'i', 'g', 'a'), 0);
        feature_set(fl, KBTS_FOURCC('c', 'l', 'i', 'g'), 0);
        feature_set(fl, KBTS_FOURCC('d', 'l', 'i', 'g'), 0);
        feature_set(fl, KBTS_FOURCC('h', 'l', 'i', 'g'), 0);
        feature_set(fl, KBTS_FOURCC('c', 'a', 'l', 't'), 0);
    }
    for (uint32_t i = 0; i < st->feature_count; i++) {
        feature_set(fl, kb_tag(st->features[i].tag), st->features[i].value);
    }
    if (fl->overflow) {
        embi_log_warn("text: more than %d font features, the rest are dropped", EMBI_FEATURE_MAX);
    }
}

static uint64_t features_hash(const feature_list *fl)
{
    uint64_t h = embi_hash64(fl->n + 1u);
    for (uint32_t i = 0; i < fl->n; i++) {
        h = embi_hash64(h ^ ((uint64_t)fl->items[i].Tag << 32) ^
                        (uint64_t)(uint32_t)fl->items[i].Value);
    }
    return h;
}

/* ------------------------------------------------------------------ cache */

static size_t entry_bytes(const shape_entry *e)
{
    return sizeof(shape_entry) + e->text_len + sizeof(shaped_glyph) * e->glyph_count;
}

static void lru_unlink(shape_state *s, shape_entry *e)
{
    if (e->prev != nullptr) {
        e->prev->next = e->next;
    } else {
        s->head = e->next;
    }
    if (e->next != nullptr) {
        e->next->prev = e->prev;
    } else {
        s->tail = e->prev;
    }
    e->prev = e->next = nullptr;
}

static void lru_push_front(shape_state *s, shape_entry *e)
{
    e->prev = nullptr;
    e->next = s->head;
    if (s->head != nullptr) {
        s->head->prev = e;
    }
    s->head = e;
    if (s->tail == nullptr) {
        s->tail = e;
    }
}

static void entry_free(shape_state *s, shape_entry *e)
{
    embi_hashmap_remove(&s->map, e->hash);
    lru_unlink(s, e);
    s->bytes -= entry_bytes(e);
    embi_free(e->text, e->text_len);
    embi_free(e->glyphs, sizeof(shaped_glyph) * e->glyph_count);
    embi_free(e, sizeof *e);
}

static uint64_t entry_hash(uint32_t uid, uint32_t inst, uint32_t script, uint8_t rtl,
                           uint32_t lang, uint64_t feat, const uint8_t *text, uint32_t n)
{
    uint64_t h = embi_hash_bytes(text, n, 0x9e3779b97f4a7c15ull);
    h = embi_hash64(h ^ (((uint64_t)uid << 32) | inst));
    h = embi_hash64(h ^ (((uint64_t)script << 32) | ((uint64_t)rtl << 8) | 0x5au));
    h = embi_hash64(h ^ lang);
    return embi_hash64(h ^ feat);
}

static shape_entry *cache_find(shape_state *s, uint64_t h, uint32_t uid, uint32_t inst,
                               uint32_t script, uint8_t rtl, uint32_t lang, uint64_t feat,
                               const uint8_t *text, uint32_t n)
{
    uint64_t v;
    if (!embi_hashmap_get(&s->map, h, &v)) {
        return nullptr;
    }
    shape_entry *e = (shape_entry *)(uintptr_t)v;
    if (e->face_uid != uid || e->inst != inst || e->script != script || e->rtl != rtl ||
        e->lang != lang || e->feat != feat || e->text_len != n ||
        SDL_memcmp(e->text, text, n) != 0) {
        return nullptr;
    }
    lru_unlink(s, e);
    lru_push_front(s, e);
    return e;
}

static void cache_insert(shape_state *s, shape_entry *e)
{
    uint64_t old;
    if (embi_hashmap_get(&s->map, e->hash, &old)) {
        entry_free(s, (shape_entry *)(uintptr_t)old);
    }
    embi_hashmap_put(&s->map, e->hash, (uint64_t)(uintptr_t)e);
    lru_push_front(s, e);
    s->bytes += entry_bytes(e);
    while (s->bytes > s->budget && s->tail != nullptr && s->tail != e) {
        entry_free(s, s->tail);
    }
}

bool embi_shape_init(embi_text *t)
{
    shape_state *s = embi_calloc(1, sizeof *s);
    s->budget = EMBI_SHAPE_DEFAULT_BUDGET;
    embi_hashmap_init(&s->map);
    t->shape = s;
    return true;
}

static void cfgs_free(shape_state *s)
{
    for (uint32_t i = 0; i < s->ncfg; i++) {
        kbts_DestroyShapeConfig(s->cfgs[i].cfg);
    }
    s->ncfg = 0;
}

/* The shape config of a face instance for a script and language: the kept one, else a new one (kept). */
static kbts_shape_config *cfg_get(shape_state *s, embi_face *face, uint32_t inst, kbts_font *kf,
                                  kbts_script script, kbts_language lang, float *user)
{
    for (uint32_t i = 0; i < s->ncfg; i++) {
        const cfg_entry *e = &s->cfgs[i];
        if (e->uid == face->uid && e->inst == inst && e->script == script && e->lang == lang &&
            e->font == kf) {
            return e->cfg;
        }
    }
    kbts_shape_config *cfg = kbts_CreateShapeConfig2(kf, script, lang, user, nullptr, nullptr, nullptr);
    if (cfg == nullptr) {
        return nullptr;
    }
    if (s->ncfg == EMBI_SHAPE_CFG_MAX) {
        cfgs_free(s);
    }
    s->cfgs[s->ncfg++] = (cfg_entry){face->uid, inst, script, lang, kf, cfg};
    return cfg;
}

void embi_shape_fini(embi_text *t)
{
    shape_state *s = t->shape;
    if (s == nullptr) {
        return;
    }
    cfgs_free(s);
    while (s->tail != nullptr) {
        entry_free(s, s->tail);
    }
    embi_hashmap_free(&s->map);
    kbts_FreeAllGlyphs(&s->storage);
    embi_free(s, sizeof *s);
    t->shape = nullptr;
}

void embi_shape_set_budget(embi_text *t, size_t bytes)
{
    shape_state *s = t->shape;
    s->budget = bytes;
    while (s->bytes > s->budget && s->tail != nullptr) {
        entry_free(s, s->tail);
    }
}

void embi_shape_clear(embi_text *t)
{
    shape_state *s = t->shape;
    cfgs_free(s);
    while (s->tail != nullptr) {
        entry_free(s, s->tail);
    }
}

/* ------------------------------------------------------------------ kb shaping of one run */

/* The longest run handed to kb in one piece: past EMBI_SHAPE_SOFT_CUT bytes a run is cut after the next space,
 * past EMBI_SHAPE_HARD_CUT at the next grapheme boundary. */
#define EMBI_SHAPE_SOFT_CUT 1024u
#define EMBI_SHAPE_HARD_CUT 4096u

/* Shapes text[begin, end) of one face with kb. The result is in font units with cluster offsets
 * relative to begin. Returns false when kb cannot shape the run. */
static bool kb_shape_run(embi_text *t, embi_face *face, uint32_t inst, const float *user,
                         const int16_t *coords, const uint8_t *text, uint32_t n,
                         kbts_script script, bool rtl, kbts_language lang,
                         const feature_list *fl, shaped_glyph **out, uint32_t *out_n)
{
    shape_state *s = t->shape;
    kbts_font *kf = kb_font_of(face);
    if (kf == nullptr) {
        return false;
    }
    bool ok = false;
    kbts_shape_config *cfg = cfg_get(s, face, inst, kf, script, lang, (float *)user);
    if (cfg == nullptr) {
        return false;
    }
    kbts_glyph_config *gc =
        kbts_CreateGlyphConfig(cfg, (kbts_feature_override *)fl->items, fl->n, nullptr, nullptr);
    kbts_shape_scratchpad *sp = kbts_CreateShapeScratchpad(cfg, nullptr, nullptr);
    if (gc == nullptr || sp == nullptr) {
        goto done;
    }
    kbts_ClearActiveGlyphs(&s->storage);
    const char *p = (const char *)text;
    const char *end = p + n;
    while (p < end) {
        uint32_t off = (uint32_t)(p - (const char *)text);
        uint32_t cp = embi_utf8_decode(&p, end);
        if (kbts_PushGlyph(&s->storage, kf, (int)cp, gc, (int)off) == nullptr) {
            goto done;
        }
    }
    kbts_glyph_iterator it;
    kbts_shape_error err = kbts_ShapeDirect2(sp, &s->storage,
                                             rtl ? KBTS_DIRECTION_RTL : KBTS_DIRECTION_LTR,
                                             (kbts_s16 *)coords, &it);
    if (err != KBTS_SHAPE_ERROR_NONE) {
        goto done;
    }
    uint32_t count = 0;
    uint32_t cap = 16;
    shaped_glyph *g = embi_alloc(sizeof(shaped_glyph) * cap);
    if (g == nullptr) {
        goto done;
    }
    kbts_glyph *kg = nullptr;
    while (kbts_GlyphIteratorNext(&it, &kg)) {
        if (count == cap) {
            uint32_t ncap = embi_vec_grow_to(cap, cap + 1);
            g = embi_realloc(g, sizeof(shaped_glyph) * cap, sizeof(shaped_glyph) * ncap);
            cap = ncap;
        }
        g[count].gid = kg->Id;
        g[count].cluster = (uint32_t)kg->UserIdOrCodepointIndex;
        g[count].adv = kg->AdvanceX;
        g[count].offx = kg->OffsetX;
        g[count].offy = kg->OffsetY;
        count++;
    }
    if (count == 0) {
        embi_free(g, sizeof(shaped_glyph) * cap);
        g = nullptr;
    } else if (count < cap) {
        g = embi_realloc(g, sizeof(shaped_glyph) * cap, sizeof(shaped_glyph) * count);
    }
    *out = g;
    *out_n = count;
    ok = true;
done:
    kbts_ClearActiveGlyphs(&s->storage);
    if (sp != nullptr) {
        kbts_DestroyShapeScratchpad(sp);
    }
    if (gc != nullptr) {
        kbts_DestroyGlyphConfig(gc);
    }
    return ok;
}

/* Cache-backed run shaping. On success the entry is in the cache and returned. */
static shape_entry *shape_cached(embi_text *t, embi_face *face, uint32_t inst,
                                 const float *user, const int16_t *coords, const uint8_t *text,
                                 uint32_t n, kbts_script script, bool rtl, kbts_language lang,
                                 const feature_list *fl)
{
    shape_state *s = t->shape;
    uint32_t uid = face->uid;
    uint64_t feat = features_hash(fl);
    uint64_t h = entry_hash(uid, inst, script, rtl ? 1 : 0, lang, feat, text, n);
    shape_entry *e = cache_find(s, h, uid, inst, script, rtl ? 1 : 0, lang, feat, text, n);
    if (e != nullptr) {
        s->hits++;
        return e;
    }
    s->misses++;
    shaped_glyph *g = nullptr;
    uint32_t gn = 0;
    if (!kb_shape_run(t, face, inst, user, coords, text, n, script, rtl, lang, fl, &g, &gn)) {
        return nullptr;
    }
    e = embi_calloc(1, sizeof *e);
    e->hash = h;
    e->face_uid = uid;
    e->inst = inst;
    e->script = script;
    e->lang = lang;
    e->feat = feat;
    e->rtl = rtl ? 1 : 0;
    e->text_len = n;
    e->text = embi_alloc(n);
    if (n > 0) {
        SDL_memcpy(e->text, text, n);
    }
    e->glyph_count = gn;
    e->glyphs = g;
    cache_insert(s, e);
    return e;
}

/* ------------------------------------------------------------------ itemization helpers */

static bool is_cp_start(const char *text, uint32_t i, uint32_t len)
{
    return i == len || i == 0 || (((uint8_t)text[i]) & 0xC0) != 0x80;
}

/* Ignorable code points join the cluster of their base and never decide a face. */
static bool is_ignorable(uint32_t cp)
{
    return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0xFEFF || cp == 0x034F ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* Picks the face for a grapheme cluster [a, b): the candidates that cover it (the bundled default last, after the
 * per-character system fallback, W4i), then the system fallback, then the default, then the first candidate. */
static embi_face *face_for_cluster(embi_text *t, embi_face **cand, uint32_t nc, const char *text,
                                   uint32_t a, uint32_t b, const embi_style_match *m, embi_str lang, bool analysis_only)
{
    embi_face *def = embi_fontdb_default_face(t->db);
    /* The bundled default is the last resort of every list, except when the style names its family: then it is a
     * candidate like any other (a page that asks for "Noto Sans" gets the bundled face before any system fallback). */
    bool def_named = false;
    for (uint32_t i = 0; def != nullptr && i < m->family_count; i++) {
        def_named = def_named || embi_str_ieq(m->families[i], embi_face_family(def));
    }
    uint32_t first = 0;
    bool have_first = false;
    const char *p = text + a;
    const char *end = text + b;
    uint32_t cps[64];
    uint32_t nk = 0;
    while (p < end && nk < 64) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (!is_ignorable(cp)) {
            if (!have_first) {
                first = cp;
                have_first = true;
            }
            cps[nk++] = cp;
        }
    }
    if (!have_first) {
        return nc > 0 ? cand[0] : nullptr;
    }
    if (analysis_only) {
        return nullptr; /* an analysis-only run: no face is looked up */
    }
    /* 1. The family list (and the embedder's fallbacks) covers the whole cluster. */
    for (uint32_t c = 0; c < nc; c++) {
        if (cand[c] == def && !def_named) {
            continue;
        }
        bool all = true;
        for (uint32_t k = 0; k < nk && all; k++) {
            all = embi_face_covers(cand[c], cps[k]);
        }
        if (all) {
            return cand[c];
        }
    }
    /* 2. Per-character system fallback (sysfont.c, match.c): the generic family, the script's preferences, then any
     * system face that covers the code point. */
    embi_face *sys = embi_fontdb_cover_face(t->db, first, m, lang);
    if (sys != nullptr) {
        return sys;
    }
    /* 3. The bundled default, then a family that covers only the first code point, then the first candidate. */
    if (def != nullptr && embi_face_covers(def, first)) {
        return def;
    }
    for (uint32_t c = 0; c < nc; c++) {
        if (cand[c] != def && embi_face_covers(cand[c], first)) {
            return cand[c];
        }
    }
    if (nc == 0) {
        /* No candidate and nothing covers the character: no face at all (a build without the bundled default font, no system
         * font, no registered font). The run is not drawn; the registry says so once (SZ3b). */
        embi_fontdb_warn_no_font(t->db);
        return nullptr;
    }
    return cand[0];
}


/* ------------------------------------------------------------------ small caps */

/* The upper case letter that a small-caps lower case letter is drawn as, when its UTF-8 encoding has the same
 * length (so that byte offsets of the text stay valid); 0 when cp has no such mapping. Covers Latin, Greek and
 * Cyrillic, which is what the bundled face has. */
static uint32_t cp_len(uint32_t cp)
{
    return cp < 0x80 ? 1u : cp < 0x800 ? 2u : cp < 0x10000 ? 3u : 4u;
}

static uint32_t small_cap_upper(uint32_t cp)
{
    uint32_t u = 0;
    if (cp >= 'a' && cp <= 'z') {
        u = cp - 32;
    } else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) {
        u = cp - 32;
    } else if (cp == 0xFF) {
        u = 0x178;
    } else if (cp == 0xB5) {
        u = 0x39C;
    } else if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) {
        if (cp == 0x131) {
            return 0;
        }
        u = (cp & 1) ? cp - 1 : 0;
    } else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
        u = (cp & 1) == 0 ? cp - 1 : 0;
    } else if (cp >= 0x3B1 && cp <= 0x3C9) {
        u = cp == 0x3C2 ? 0x3A3 : cp - 32;
    } else if (cp >= 0x3AC && cp <= 0x3AF) {
        u = cp == 0x3AC ? 0x386 : cp - 0x25;
    } else if (cp >= 0x430 && cp <= 0x44F) {
        u = cp - 32;
    } else if (cp >= 0x450 && cp <= 0x45F) {
        u = cp - 80;
    }
    if (u == 0 || cp_len(u) != cp_len(cp)) {
        return 0;
    }
    return u;
}

/* True for an upper case letter that all-small-caps draws small as well: one that small_cap_upper maps a letter to. */
static bool is_small_cap_upper(uint32_t cp)
{
    const uint32_t lowers[4] = {cp + 32, cp + 1, cp + 80, cp + 0x25};
    for (int i = 0; i < 4; i++) {
        if (small_cap_upper(lowers[i]) == cp) {
            return true;
        }
    }
    return false;
}

/* Whether the face has real small capitals: 'a' maps to another glyph under the smcp feature. Probed once per face. */
static bool face_has_smcp(embi_text *t, embi_face *face, kbts_language lang)
{
    if (face->smcp != 0) {
        return face->smcp > 0;
    }
    face->smcp = -1;
    if (!embi_face_covers(face, 'a')) {
        return false;
    }
    feature_list off, on;
    off.n = on.n = 0;
    off.overflow = on.overflow = false;
    feature_set(&on, KBTS_FOURCC('s', 'm', 'c', 'p'), 1);
    const float *user = embi_face_inst_user(face, 0);
    const int16_t *coords = embi_face_inst_coords(face, 0);
    shape_entry *a = shape_cached(t, face, 0, user, coords, (const uint8_t *)"a", 1, KBTS_SCRIPT_LATIN, false, lang, &off);
    uint32_t ga = a != nullptr && a->glyph_count > 0 ? a->glyphs[0].gid : 0;
    shape_entry *b = shape_cached(t, face, 0, user, coords, (const uint8_t *)"a", 1, KBTS_SCRIPT_LATIN, false, lang, &on);
    uint32_t gb = b != nullptr && b->glyph_count > 0 ? b->glyphs[0].gid : 0;
    if (ga != 0 && gb != 0 && ga != gb) {
        face->smcp = 1;
    }
    return face->smcp > 0;
}

bool embi_text_cursive(uint32_t cp)
{
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0700 && cp <= 0x074F) || (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x07C0 && cp <= 0x07FF) || (cp >= 0x0840 && cp <= 0x085F) || (cp >= 0x08A0 && cp <= 0x08FF) ||
           (cp >= 0x1800 && cp <= 0x18AF) || (cp >= 0xA840 && cp <= 0xA87F) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF) || (cp >= 0x1E900 && cp <= 0x1E95F);
}

/* A combining mark the font lacks (a .notdef with no advance) in a left-to-right run: its box is drawn over its base, as it
 * is in a right-to-left run, not after it (the pen has already moved past the base). The base is the last glyph of the
 * run [first, end) with an advance. */
static void mark_box_over_base(const embi_text_layout *out, uint32_t first, embi_text_glyph *g)
{
    for (uint32_t i = out->glyphs.len; i > first; i--) {
        if (out->glyphs.data[i - 1].advance > 0.0f) {
            g->x_offset -= out->glyphs.data[i - 1].advance;
            return;
        }
    }
}

bool embi_text_no_ls(uint32_t cp)
{
    if (embi_text_cursive(cp)) {
        return true;
    }
    /* Default_Ignorable_Code_Point (DerivedCoreProperties), without U+00AD (a soft hyphen shows when a line breaks there). */
    return cp == 0x034Fu || cp == 0x061Cu || (cp >= 0x115Fu && cp <= 0x1160u) || (cp >= 0x17B4u && cp <= 0x17B5u) ||
           (cp >= 0x180Bu && cp <= 0x180Fu) || (cp >= 0x200Bu && cp <= 0x200Fu) || (cp >= 0x202Au && cp <= 0x202Eu) ||
           (cp >= 0x2060u && cp <= 0x206Fu) || cp == 0x3164u || (cp >= 0xFE00u && cp <= 0xFE0Fu) || cp == 0xFEFFu ||
           cp == 0xFFA0u || (cp >= 0xFFF0u && cp <= 0xFFF8u) || (cp >= 0x1BCA0u && cp <= 0x1BCA3u) ||
           (cp >= 0x1D173u && cp <= 0x1D17Au) || (cp >= 0xE0000u && cp <= 0xE0FFFu);
}

/* Whether a join-causing character (U+200D ZERO WIDTH JOINER or U+0640 ARABIC TATWEEL) ends just before byte at. */
static bool joiner_before(const char *text, uint32_t at)
{
    if (at >= 3 && (uint8_t)text[at - 3] == 0xE2 && (uint8_t)text[at - 2] == 0x80 && (uint8_t)text[at - 1] == 0x8D) {
        return true;
    }
    return at >= 2 && (uint8_t)text[at - 2] == 0xD9 && (uint8_t)text[at - 1] == 0x80;
}

/* Whether a join-causing character starts at byte at (text is len bytes). */
static bool joiner_after(const char *text, uint32_t len, uint32_t at)
{
    if (at + 3 <= len && (uint8_t)text[at] == 0xE2 && (uint8_t)text[at + 1] == 0x80 && (uint8_t)text[at + 2] == 0x8D) {
        return true;
    }
    return at + 2 <= len && (uint8_t)text[at] == 0xD9 && (uint8_t)text[at + 1] == 0x80;
}

/* Word spacing applies to U+0020 and U+00A0. */
static bool is_word_space(const char *text, uint32_t len, uint32_t at)
{
    if (at >= len) {
        return false;
    }
    if (text[at] == ' ') {
        return true;
    }
    return at + 1 < len && (uint8_t)text[at] == 0xC2 && (uint8_t)text[at + 1] == 0xA0;
}

/* Whether the code point at p is East Asian Wide or Fullwidth (UAX 11: the CJK ideographs and symbols, kana, Hangul
 * syllables and the fullwidth forms): its glyphs are 1 em wide in the fonts that have them. */
static bool wide_cp_at(const char *p, const char *end)
{
    uint32_t c = embi_utf8_decode(&p, end);
    return (c >= 0x1100u && c <= 0x115Fu) || (c >= 0x2E80u && c <= 0x303Eu) || (c >= 0x3041u && c <= 0x33FFu) ||
           (c >= 0x3400u && c <= 0x4DBFu) || (c >= 0x4E00u && c <= 0x9FFFu) || (c >= 0xA000u && c <= 0xA4CFu) ||
           (c >= 0xAC00u && c <= 0xD7A3u) || (c >= 0xF900u && c <= 0xFAFFu) || (c >= 0xFE30u && c <= 0xFE4Fu) ||
           (c >= 0xFF01u && c <= 0xFF60u) || (c >= 0xFFE0u && c <= 0xFFE6u) || (c >= 0x20000u && c <= 0x3FFFDu);
}

/* Adds word-spacing for the space glyph i (glyphs [first, ...) of a segment in visual order): after the whole typographic
 * character unit, so a combining mark on the space stays on it (CSS Text 3 section 8.1). Left to right, the last glyph of the
 * grapheme takes it in its advance; right to left, the gap goes on the left of the unit: its leftmost glyph takes it in its
 * advance and its offset, so its ink stays where it was. */
static void word_space_at(embi_text_layout *out, uint32_t first, uint32_t i, bool rtl, uint32_t text_len,
                          const kbts_break_flags *flags, float word)
{
    uint32_t sb = out->glyphs.data[i].cluster, ge = sb + 1;
    while (flags != nullptr && ge < text_len && (flags[ge] & KBTS_BREAK_FLAG_GRAPHEME) == 0) {
        ge++;
    }
    uint32_t j = i;
    if (!rtl) {
        if (out->glyphs.data[i].cluster != sb) {
            return;
        }
        /* only the first glyph of the space's cluster starts the unit; the spacing goes on the unit's last glyph */
        if (i > first && out->glyphs.data[i - 1].cluster == sb) {
            return;
        }
        while (j + 1 < out->glyphs.len && out->glyphs.data[j + 1].cluster >= sb && out->glyphs.data[j + 1].cluster < ge) {
            j++;
        }
        out->glyphs.data[j].advance += word;
        return;
    }
    if (i + 1 < out->glyphs.len && out->glyphs.data[i + 1].cluster == sb) {
        return; /* the space's cluster has more glyphs: its rightmost one handles it */
    }
    while (j > first && out->glyphs.data[j - 1].cluster >= sb && out->glyphs.data[j - 1].cluster < ge) {
        j--;
    }
    out->glyphs.data[j].advance += word;
    out->glyphs.data[j].x_offset += word;
}

/* Whether the code point at byte at is a default ignorable one (a zero-width format control). */
static bool ignorable_at(const char *text, uint32_t len, uint32_t at)
{
    const char *p = text + at;
    uint32_t cp = embi_utf8_decode(&p, text + len);
    return !embi_text_cursive(cp) && embi_text_no_ls(cp);
}

/* The first byte of the grapheme cluster that holds byte at. */
static uint32_t grapheme_start(const kbts_break_flags *flags, uint32_t at)
{
    while (at > 0 && (flags[at] & KBTS_BREAK_FLAG_GRAPHEME) == 0) {
        at--;
    }
    return at;
}

/* letter-spacing of a right to left segment (glyphs [first, ...) in visual order): each typographic character unit (a
 * grapheme cluster) gets the space on its left, its logical end. The unit's leftmost glyph takes it in its advance and its
 * offset, so the unit's own glyphs, marks included, keep their places relative to each other. Cursive letters get none. */
static void rtl_letter_spacing(embi_text_layout *out, uint32_t first, const char *text, uint32_t text_len,
                               const kbts_break_flags *flags, float letter)
{
    uint32_t prev = UINT32_MAX;
    for (uint32_t i = first; i < out->glyphs.len; i++) {
        embi_text_glyph *g = &out->glyphs.data[i];
        if (g->cluster >= text_len) {
            continue;
        }
        uint32_t gs = ignorable_at(text, text_len, g->cluster) ? g->cluster : grapheme_start(flags, g->cluster);
        if (gs == prev) {
            continue;
        }
        prev = gs;
        const char *cp = text + gs;
        if (embi_text_no_ls(embi_utf8_decode(&cp, text + text_len))) {
            continue;
        }
        g->advance += letter;
        g->x_offset += letter;
    }
}

/* Appends the glyphs of one shaped segment, converted to px. */
static bool append_glyphs(embi_text_layout *out, const shape_entry *e, uint32_t text_begin,
                          const char *text, uint32_t text_len, float scale, float letter,
                          float word, bool no_face, float size, const kbts_break_flags *flags)
{
    uint32_t first = out->glyphs.len;
    if (no_face) {
        /* No face covers the text: one empty glyph per code point, half an em wide. */
        const char *p = text + text_begin;
        const char *end = p + e->text_len;
        while (p < end) {
            uint32_t at = (uint32_t)(p - text);
            (void)embi_utf8_decode(&p, end);
            embi_text_glyph g = {0};
            g.gid = 0;
            g.cluster = at;
            g.advance = 0.5f * size + letter;
            if (is_word_space(text, text_len, at)) {
                g.advance += word;
            }
            g.safe_to_break = true;
            embi_vec_push(&out->glyphs, g);
        }
        return true;
    }
    for (uint32_t i = 0; i < e->glyph_count; i++) {
        const shaped_glyph *sg = &e->glyphs[i];
        embi_text_glyph g = {0};
        g.gid = sg->gid;
        if (sg->cluster < e->text_len) {
            /* An unmapped control character (Cc, except TAB, LF, FF and CR, which CSS Text handles itself) gets the box glyph of text/draw.c. Other unmapped code points stay
             * as the font's .notdef. */
            uint8_t b0 = (uint8_t)text[text_begin + sg->cluster];
            bool c1 = b0 == 0xC2 && text_begin + sg->cluster + 1 < text_len &&
                      ((uint8_t)text[text_begin + sg->cluster + 1] & 0xE0) == 0x80;
            if ((b0 < 0x20 && b0 != 9 && b0 != 10 && b0 != 13) || b0 == 0x7F || c1) {
                g.gid = 0xFFFFu;
            }
        }
        g.cluster = text_begin + sg->cluster;
        g.advance = (float)sg->adv * scale;
        if (sg->gid == 0 && sg->cluster < e->text_len && wide_cp_at(text + text_begin + sg->cluster, text + text_len)) {
            g.advance = size; /* a missing East Asian wide character keeps its full-width (1 em) advance */
        }
        if (sg->gid == 0 && sg->cluster < e->text_len) {
            const char *ip = text + text_begin + sg->cluster;
            uint32_t icp = embi_utf8_decode(&ip, text + text_len);
            if (!embi_text_cursive(icp) && embi_text_no_ls(icp)) {
                /* A default ignorable code point the font lacks (U+2060, U+FEFF) is not rendered: no box, no advance. */
                g.gid = 0xFFFEu;
                g.advance = 0.0f;
            }
        }
        if (g.gid == 0xFFFFu && g.advance < 0.55f * size) {
            g.advance = 0.55f * size; /* the box of a control character needs room even where the font maps it to a null glyph */
        }
        g.x_offset = (float)sg->offx * scale;
        g.y_offset = (float)sg->offy * scale;
        if (!e->rtl && sg->gid == 0 && sg->adv == 0) {
            mark_box_over_base(out, first, &g);
        }
        embi_vec_push(&out->glyphs, g);
    }
    /* A control character that the shaper dropped (it has no glyph) still gets its box (left to right runs). */
    for (uint32_t k = 0; !e->rtl && k < e->text_len; k++) {
        uint8_t b0 = (uint8_t)text[text_begin + k];
        bool ctl = (b0 < 0x20 && b0 != 9 && b0 != 10 && b0 != 13) || b0 == 0x7F ||
                   (b0 == 0xC2 && k + 1 < e->text_len && ((uint8_t)text[text_begin + k + 1] & 0xE0) == 0x80);
        if (!ctl) {
            continue;
        }
        uint32_t cl = text_begin + k, at = out->glyphs.len;
        bool have = false;
        for (uint32_t gi = first; gi < out->glyphs.len && !have; gi++) {
            have = out->glyphs.data[gi].cluster == cl;
            if (!have && out->glyphs.data[gi].cluster > cl && at == out->glyphs.len) {
                at = gi;
            }
        }
        if (have) {
            continue;
        }
        embi_text_glyph ng = {0};
        ng.gid = 0xFFFFu;
        ng.cluster = cl;
        ng.advance = 0.55f * size;
        ng.safe_to_break = true;
        embi_vec_push(&out->glyphs, ng);
        for (uint32_t gi = out->glyphs.len - 1; gi > at; gi--) {
            out->glyphs.data[gi] = out->glyphs.data[gi - 1];
        }
        out->glyphs.data[at] = ng;
    }
    /* letter-spacing after the last glyph of each cluster, word-spacing on spaces. */
    for (uint32_t i = first; i < out->glyphs.len; i++) {
        embi_text_glyph *g = &out->glyphs.data[i];
        bool last_of_cluster = i + 1 == out->glyphs.len ||
                               out->glyphs.data[i + 1].cluster != g->cluster;
        /* A typographic character unit is a whole grapheme cluster: no letter-spacing between a base and the marks that follow
         * it, even where the shaper gives the mark a cluster of its own (left to right runs; flags is indexed by byte). */
        if (last_of_cluster && letter != 0.0f && flags != nullptr && !e->rtl && i + 1 < out->glyphs.len) {
            uint32_t nc = out->glyphs.data[i + 1].cluster;
            if (nc > g->cluster && nc < text_len && (flags[nc] & KBTS_BREAK_FLAG_GRAPHEME) == 0 && !ignorable_at(text, text_len, nc)) {
                last_of_cluster = false;
            }
        }
        bool rtl_units = e->rtl && flags != nullptr && letter != 0.0f;
        if (rtl_units) {
            /* right to left: rtl_letter_spacing below */
        } else if (last_of_cluster && letter != 0.0f && g->cluster < text_len) {
            /* a zero-width format control gets none; a mark goes with its base, whose script decides */
            const char *cp = text + (flags != nullptr && !ignorable_at(text, text_len, g->cluster) ? grapheme_start(flags, g->cluster)
                                                                                                  : g->cluster);
            if (!embi_text_no_ls(embi_utf8_decode(&cp, text + text_len))) {
                g->advance += letter;
            }
        } else if (last_of_cluster) {
            g->advance += letter;
        }
        if (is_word_space(text, text_len, g->cluster) && word != 0.0f) {
            word_space_at(out, first, i, e->rtl, text_len, flags, word);
        }
    }
    if (e->rtl && flags != nullptr && letter != 0.0f) {
        rtl_letter_spacing(out, first, text, text_len, flags, letter);
    }
    return true;
}

/* ------------------------------------------------------------------ layout */

static void layout_free(embi_text_layout *lay)
{
    /* Runs of a failed shaping hold no reference, so only the success path counts (see embi_text_layout_free). */
    embi_bidi_cache_free(lay->bidi);
    embi_free(lay->text, lay->text_len + 1);
    embi_vec_free(&lay->runs);
    embi_vec_free(&lay->glyphs);
    embi_vec_free(&lay->breaks);
    embi_vec_free(&lay->paras);
    SDL_memset(lay, 0, sizeof *lay);
}

void embi_text_layout_free(embi_text_layout *lay)
{
    if (lay != nullptr) {
        /* Each run that a successful shaping counted (refs, below) is released here, so a system face that no layout
         * uses any more can be trimmed. */
        for (uint32_t r = 0; r < lay->runs.len; r++) {
            embi_face *f = lay->runs.data[r].face;
            if (f != nullptr && f->refs > 0) {
                f->refs--;
            }
        }
        layout_free(lay);
    }
}

void embi_shape_forget_face(embi_text *t, uint32_t uid)
{
    shape_state *s = t->shape;
    if (s == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < s->ncfg;) {
        if (s->cfgs[i].uid == uid) {
            kbts_DestroyShapeConfig(s->cfgs[i].cfg);
            s->cfgs[i] = s->cfgs[--s->ncfg];
        } else {
            i++;
        }
    }
    shape_entry *e = s->head;
    while (e != nullptr) {
        shape_entry *next = e->next;
        if (e->face_uid == uid) {
            entry_free(s, e);
        }
        e = next;
    }
}

static kbts_direction kb_dir(uint8_t dir)
{
    if (dir == EMBI_DIR_LTR) {
        return KBTS_DIRECTION_LTR;
    }
    if (dir == EMBI_DIR_RTL) {
        return KBTS_DIRECTION_RTL;
    }
    return KBTS_DIRECTION_DONT_KNOW;
}

/* CSS line-break (auto..strict) to kb's Japanese line-break style. auto is normal. */
static kbts_japanese_line_break_style kb_line_style(uint8_t line_break)
{
    if (line_break == EMBI_LINE_BREAK_LOOSE) {
        return KBTS_JAPANESE_LINE_BREAK_STYLE_LOOSE;
    }
    if (line_break == EMBI_LINE_BREAK_STRICT) {
        return KBTS_JAPANESE_LINE_BREAK_STYLE_STRICT;
    }
    return KBTS_JAPANESE_LINE_BREAK_STYLE_NORMAL;
}

/* Segment breaks (kb) for the whole text. flags[i] has the break kinds at byte i. scripts[i] is
 * the script of the codepoint at byte i. flags and scripts hold len + 1 zeroed entries.
 *
 * This is kbts_BreakEntireString's loop without its break array: that function merges each new break
 * into the earlier ones by a linear search ("@Speed: Binary search!"), which makes a run of n
 * characters cost n * n (a 200 KB word took 4 s). The flags are indexed by position, so a break
 * merges for free here, and the script of a position is the last script break reported at it, as
 * the merge would leave it (W3q). */
static bool analyze_breaks(const char *text, uint32_t len, uint8_t dir, uint8_t line_break,
                           kbts_break_flags *flags, kbts_script *scripts)
{
    kbts_break_state state;
    memset(&state, 0, sizeof state);
    kbts_BreakBegin(&state, kb_dir(dir), kb_line_style(line_break), 0);
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        int codepoint = 0;
        int size = 1;
        kbts_decode d = kbts_DecodeUtf8(p, (kbts_un)(end - p));
        if (d.Valid) {
            codepoint = d.Codepoint;
            size = d.SourceCharactersConsumed;
        }
        p += size;
        kbts_BreakAddCodepoint(&state, codepoint, size, p >= end);
        kbts_break br;
        while (kbts_Break(&state, &br)) {
            if (br.Position < 0 || (uint32_t)br.Position > len) {
                continue;
            }
            flags[br.Position] |= br.Flags;
            if ((br.Flags & KBTS_BREAK_FLAG_SCRIPT) != 0 && (uint32_t)br.Script < (uint32_t)KBTS_SCRIPT_COUNT) {
                scripts[br.Position] = br.Script;
            }
        }
    }
    /* Scripts: the last script break at or before each byte. Bytes with no break before them
     * (or a DONT_KNOW script) read as Latin. */
    kbts_script cur = KBTS_SCRIPT_DONT_KNOW;
    for (uint32_t i = 0; i <= len; i++) {
        /* A combining mark takes the script of its base: no script change inside a grapheme cluster, so the mark is
         * shaped with its base (a mark on a space in a right to left run stays anchored to it). */
        if ((flags[i] & KBTS_BREAK_FLAG_SCRIPT) != 0 &&
            (i == 0 || i >= len || (flags[i] & KBTS_BREAK_FLAG_GRAPHEME) != 0 || cur == KBTS_SCRIPT_DONT_KNOW)) {
            cur = scripts[i];
        }
        scripts[i] = cur;
    }
    return true;
}

/* A code point that breaks like an ideograph here: CJK ideographs, kana, the CJK symbols and the full-width forms. */
static bool lb_ideo(uint32_t cp)
{
    return (cp >= 0x3040 && cp <= 0x30FF && cp != 0x30FB) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF) || (cp >= 0xFF66 && cp <= 0xFF9F) || (cp >= 0x3100 && cp <= 0x312F) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

/* The scripts that need a dictionary to find word boundaries (Thai, Lao, Burmese, Khmer) get a fallback break between
 * orthographic units (CSS Text 3 section 5.1: some form of fallback breaking must occur): before each consonant or
 * independent vowel, never between a leading vowel or a stacking mark and what follows. */
static bool sa_starter(uint32_t cp)
{
    return (cp >= 0x0E01 && cp <= 0x0E2E) || (cp >= 0x0E40 && cp <= 0x0E44) || (cp >= 0x0E81 && cp <= 0x0EAE) ||
           (cp >= 0x0EC0 && cp <= 0x0EC4) || (cp >= 0x0EDC && cp <= 0x0EDF) || (cp >= 0x1780 && cp <= 0x17B3) ||
           (cp >= 0x1000 && cp <= 0x102A) || cp == 0x103F;
}

static bool sa_joins_next(uint32_t cp)
{
    return (cp >= 0x0E40 && cp <= 0x0E44) || (cp >= 0x0EC0 && cp <= 0x0EC4) || cp == 0x17D2 || cp == 0x1039;
}

static bool sa_any(uint32_t cp)
{
    return (cp >= 0x0E01 && cp <= 0x0EDF) || (cp >= 0x1000 && cp <= 0x109F) || (cp >= 0x1780 && cp <= 0x17FF);
}

/* UAX 14 rules LB8a and LB30b: no line break after a zero width joiner (an emoji ZWJ sequence) or before an emoji modifier
 * (U+1F3FB to U+1F3FF, the skin tones): the soft breaks the analysis left there are removed. And LB5: a forced break
 * after NEL (U+0085), which the analysis does not give. */
static void keep_emoji_sequences(const char *text, uint32_t len, kbts_break_flags *flags)
{
    const char *p = text, *end = text + len;
    uint32_t prev = 0;
    while (p < end) {
        uint32_t at = (uint32_t)(p - text);
        uint32_t cp = embi_utf8_decode(&p, end);
        if (at > 0 && (prev == 0x200Du || (cp >= 0x1F3FBu && cp <= 0x1F3FFu))) {
            flags[at] &= (kbts_break_flags)~KBTS_BREAK_FLAG_LINE_SOFT;
        }
        if (prev == 0x85u) {
            flags[at] |= KBTS_BREAK_FLAG_LINE_HARD; /* LB5: NEL (class NL) is a mandatory break */
        }
        prev = cp;
    }
}

/* Whether the language tag names a writing system other than Chinese or Japanese (CSS Text 3 section 1.5: the script
 * subtag when there is one, else Korean): the breaks below are for those writing systems only. An empty or other tag keeps
 * them (the writing system is then not known). */
static bool lang_not_cj(embi_str lang)
{
    uint32_t n = 0;
    while (n < lang.len && lang.ptr[n] != '-' && lang.ptr[n] != '_') {
        n++;
    }
    if (n < lang.len) {
        uint32_t b = n + 1, e = b;
        while (e < lang.len && lang.ptr[e] != '-' && lang.ptr[e] != '_') {
            e++;
        }
        const char *sub = lang.ptr + b;
        if (e - b == 4) {
            static const char *const cj[] = {"hani", "hans", "hant", "hira", "kana", "hrkt", "jpan", "bopo"};
            for (uint32_t i = 0; i < sizeof cj / sizeof cj[0]; i++) {
                if (subtag_is(sub, 4, cj[i])) {
                    return false;
                }
            }
            return true;
        }
    }
    return subtag_is(lang.ptr, n, "ko");
}

/* CSS Text 3 section 5.3: line-break normal and loose break where UAX 14 does not, between Japanese characters. normal allows
 * a break before the wave dash and the double hyphen; loose also before iteration marks, centred punctuation, postfixes and hyphens,
 * between the inseparable leaders, and after prefixes. Opportunities are added to flags (the bytes before a code point). */
static void tailor_line_breaks(const char *text, uint32_t len, uint8_t line_break, embi_str lang, kbts_break_flags *flags)
{
    const bool cj = !lang_not_cj(lang);
    if (line_break != EMBI_LINE_BREAK_NORMAL && line_break != EMBI_LINE_BREAK_LOOSE && line_break != EMBI_LINE_BREAK_AUTO) {
        if (line_break != EMBI_LINE_BREAK_STRICT) {
            return;
        }
    }
    bool loose = line_break == EMBI_LINE_BREAK_LOOSE;
    const char *p = text, *end = text + len;
    uint32_t prev = 0;
    while (p < end) {
        uint32_t at = (uint32_t)(p - text);
        uint32_t cp = embi_utf8_decode(&p, end);
        if (prev != 0 && (flags[at] & (KBTS_BREAK_FLAG_LINE_SOFT | KBTS_BREAK_FLAG_LINE_HARD)) == 0) {
            bool brk = false;
            if (sa_any(cp) && sa_any(prev)) {
                brk = sa_starter(cp) && !sa_joins_next(prev);
            } else if (line_break == EMBI_LINE_BREAK_STRICT) {
                brk = false;
            } else if (!cj) {
                brk = false;
            } else if (cp == 0x301C || cp == 0x30A0) {
                brk = lb_ideo(prev);
            } else if (loose) {
                switch (cp) {
                case 0x3005: case 0x303B: case 0x309D: case 0x309E: case 0x30FD: case 0x30FE: /* iteration marks */
                case 0x30FB: case 0xFF65: case 0xFF1A: case 0xFF1B: case 0x203C: case 0x2047: case 0x2048: case 0x2049:
                case 0xFF01: case 0xFF1F: /* centred punctuation */
                case 0xB0: case 0x2030: case 0x2032: case 0x2033: case 0x2103: case 0xFF05: case 0xFFE0: /* postfixes */
                case 0x2010: case 0x2013: /* hyphens */
                    brk = lb_ideo(prev);
                    break;
                case 0x2025: case 0x2026: /* inseparable: also between two of them */
                    brk = lb_ideo(prev) || prev == 0x2025 || prev == 0x2026;
                    break;
                default:
                    break;
                }
                if (!brk && lb_ideo(cp) && (prev == 0x20AC || prev == 0x2116 || prev == 0xFF04 || prev == 0xFFE1 || prev == 0xFFE5)) {
                    brk = true; /* after a prefix */
                }
            }
            if (brk) {
                flags[at] |= KBTS_BREAK_FLAG_LINE_SOFT;
            }
        }
        prev = cp;
    }
}

/* Test hooks (tests/unit/text/test_text_shape_long.c): kb's headers are private to this module, so the
 * tests reach the break analysis through these. out_flags and out_scripts hold len + 1 entries. The
 * reference is the loop this module used before W3q: kbts_BreakEntireStringUtf8 with its merged break
 * array (quadratic, so tests keep the text short). */
void embi_shape_breaks(const char *text, uint32_t len, uint8_t dir, uint8_t line_break,
                       uint32_t *out_flags, uint32_t *out_scripts)
{
    kbts_break_flags *f = embi_calloc((size_t)len + 1, sizeof(kbts_break_flags));
    kbts_script *sc = embi_calloc((size_t)len + 1, sizeof(kbts_script));
    if (len > 0) {
        analyze_breaks(text, len, dir, line_break, f, sc);
    }
    for (uint32_t i = 0; i <= len; i++) {
        out_flags[i] = f[i];
        out_scripts[i] = sc[i];
    }
    embi_free(f, ((size_t)len + 1) * sizeof(kbts_break_flags));
    embi_free(sc, ((size_t)len + 1) * sizeof(kbts_script));
}

void embi_shape_breaks_reference(const char *text, uint32_t len, uint8_t dir, uint8_t line_break,
                                 uint32_t *out_flags, uint32_t *out_scripts)
{
    size_t cap = (size_t)len * 3 + 8;
    kbts_break *br = embi_calloc(cap, sizeof(kbts_break));
    kbts_break_flags *f = embi_calloc((size_t)len + 1, sizeof(kbts_break_flags));
    int nbr = 0, nf = 0;
    if (len > 0) {
        kbts_BreakEntireStringUtf8(kb_dir(dir), kb_line_style(line_break), 0, text, (int)len, br, (int)cap,
                                   &nbr, f, (int)len + 1, &nf);
    }
    for (uint32_t i = 0; i <= len; i++) {
        out_flags[i] = f[i];
        out_scripts[i] = 0;
    }
    for (size_t i = 0; i < cap; i++) {
        if ((br[i].Flags & KBTS_BREAK_FLAG_SCRIPT) != 0 && br[i].Position >= 0 && (uint32_t)br[i].Position <= len) {
            out_scripts[br[i].Position] = br[i].Script;
        }
    }
    kbts_script cur = KBTS_SCRIPT_DONT_KNOW;
    for (uint32_t i = 0; i <= len; i++) {
        if ((out_flags[i] & KBTS_BREAK_FLAG_SCRIPT) != 0) {
            cur = out_scripts[i];
        }
        out_scripts[i] = cur;
    }
    embi_free(br, cap * sizeof(kbts_break));
    embi_free(f, ((size_t)len + 1) * sizeof(kbts_break_flags));
}

static float finite_or(float v, float fallback)
{
    return (v - v == 0.0f) ? v : fallback; /* false for NaN and infinities */
}

static float clamp_or(float v, float lo, float hi, float fallback)
{
    v = finite_or(v, fallback);
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Makes a style safe to use: non-finite numbers take their CSS initial values, out-of-range
 * enums fall back to auto or normal. */
static void sanitize_style(embi_text_style *s)
{
    s->size = clamp_or(s->size, 0.0f, 1.0e6f, 0.0f);
    s->weight = clamp_or(s->weight, 1.0f, 1000.0f, 400.0f);
    s->stretch = clamp_or(s->stretch, 1.0f, 1000.0f, 100.0f);
    s->letter_spacing = finite_or(s->letter_spacing, 0.0f);
    s->word_spacing = finite_or(s->word_spacing, 0.0f);
    if (s->font_style > EMBI_STYLE_OBLIQUE) {
        s->font_style = EMBI_STYLE_NORMAL;
    }
    if ((s->direction & ~EMBI_DIR_OVERRIDE) > EMBI_DIR_AUTO) {
        s->direction = (uint8_t)((s->direction & EMBI_DIR_OVERRIDE) | EMBI_DIR_AUTO);
    }
    if (s->kerning > EMBI_KERN_NONE) {
        s->kerning = EMBI_KERN_AUTO;
    }
    if (s->line_break > EMBI_LINE_BREAK_ANYWHERE) {
        s->line_break = EMBI_LINE_BREAK_AUTO;
    }
}

bool embi_text_shape(embi_text *t, const char *text, uint32_t len, const embi_text_style *style,
                     embi_text_layout *out)
{
    if (t == nullptr || out == nullptr || style == nullptr || (text == nullptr && len > 0)) {
        return false;
    }
    embi_text_style sty = *style;
    sanitize_style(&sty);
    const embi_text_style *st = &sty;
    SDL_memset(out, 0, sizeof *out);
    out->size = st->size;
    out->text_len = len;
    out->text = embi_alloc((size_t)len + 1);
    if (len > 0) {
        SDL_memcpy(out->text, text, len);
    }
    out->text[len] = '\0';
    text = out->text;

    uint8_t *levels = embi_calloc((size_t)len + 1, 1);
    kbts_break_flags *flags = embi_calloc((size_t)len + 1, sizeof(kbts_break_flags));
    kbts_script *scripts = embi_calloc((size_t)len + 1, sizeof(kbts_script));
    embi_face **cand = embi_calloc(64, sizeof(embi_face *));
    uint32_t *cstart = nullptr; /* grapheme cluster starts, ascending, plus len */
    uint32_t ncl = 0, cl_cap = 0;
    embi_face **cl_face = nullptr;
    uint8_t *cl_small = nullptr; /* per cluster: 1 when it is drawn as a synthesized small capital */
    bool ok = false;

    const uint8_t base_dir = st->direction & (uint8_t)~EMBI_DIR_OVERRIDE;
    if (!embi_bidi_analyze(text, len, base_dir, st->bidi_after && !st->analysis_only, &out->paras, levels, &out->bidi)) {
        goto fail;
    }
    if ((st->direction & EMBI_DIR_OVERRIDE) != 0 && base_dir != EMBI_DIR_AUTO && len > 0) {
        /* unicode-bidi: bidi-override: every character is strong in the base direction (UAX 9 X4 to X6, LRO/RLO). */
        embi_bidi_cache_free(out->bidi);
        out->bidi = nullptr;
        memset(levels, base_dir == EMBI_DIR_RTL ? 1 : 0, len);
        for (uint32_t i = 0; i < out->paras.len; i++) {
            out->paras.data[i].level = base_dir == EMBI_DIR_RTL ? 1 : 0;
        }
    }
    if (len > 0) {
        analyze_breaks(text, len, base_dir, st->line_break, flags, scripts);
        tailor_line_breaks(text, len, st->line_break, st->lang, flags);
        keep_emoji_sequences(text, len, flags);
    }

    embi_style_match m;
    m.families = st->families;
    m.family_count = st->family_count;
    m.weight = st->weight;
    m.style = st->font_style;
    m.stretch = st->stretch;
    uint32_t nc = st->analysis_only ? 0 : embi_fontdb_candidates(t->db, &m, cand, 64);

    /* Clusters: a boundary at 0, at every grapheme break and at len. */
    for (uint32_t i = 0; i <= len; i++) {
        bool b = i == 0 || i == len || (flags[i] & KBTS_BREAK_FLAG_GRAPHEME) != 0;
        if (i < len && !is_cp_start(text, i, len)) {
            b = false;
        }
        if (!b) {
            continue;
        }
        if (ncl + 1 >= cl_cap) {
            uint32_t ncap = embi_vec_grow_to(cl_cap, ncl + 2);
            cstart = embi_realloc(cstart, sizeof(uint32_t) * cl_cap, sizeof(uint32_t) * ncap);
            cl_face = embi_realloc(cl_face, sizeof(embi_face *) * cl_cap, sizeof(embi_face *) * ncap);
            cl_cap = ncap;
        }
        cstart[ncl++] = i;
    }
    /* ncl boundaries, the last one is len. cl_face[k] belongs to cluster [cstart[k], cstart[k+1]). */
    for (uint32_t k = 0; k + 1 < ncl; k++) {
        cl_face[k] = face_for_cluster(t, cand, nc, text, cstart[k], cstart[k + 1], &m, st->lang, st->analysis_only);
    }
    if ((st->caps == EMBI_CAPS_SMALL || st->caps == EMBI_CAPS_ALL_SMALL) && !st->analysis_only && ncl > 1) {
        /* Small capitals that the face does not have (no smcp) are synthesized: the lower case letters (and with
         * all-small-caps the upper case ones) are drawn as upper case letters at EMBI_SMALL_CAPS_SCALE. */
        bool synth = (st->synth & EMBI_SYNTH_SMALL_CAPS) != 0;
        if (synth) {
            cl_small = embi_calloc(ncl, 1);
        }
        for (uint32_t k = 0; k + 1 < ncl; k++) {
            if (cl_face[k] == nullptr || face_has_smcp(t, cl_face[k], lang_of(st->lang)) || !synth) {
                continue;
            }
            const char *q = text + cstart[k];
            uint32_t cp = embi_utf8_decode(&q, text + cstart[k + 1]);
            if (small_cap_upper(cp) != 0 || (st->caps == EMBI_CAPS_ALL_SMALL && is_small_cap_upper(cp))) {
                cl_small[k] = 1;
            }
        }
    }

    /* Segments: constant bidi level, script and face, cut at paragraph edges. */
    uint32_t cur_cl = 0;
    for (uint32_t pi = 0; pi < out->paras.len; pi++) {
        const embi_text_para *para = &out->paras.data[pi];
        uint32_t seg_start = para->begin;
        bool seg_open = false;
        uint8_t seg_level = 0;
        kbts_script seg_script = KBTS_SCRIPT_DONT_KNOW;
        embi_face *seg_face = nullptr;
        bool seg_small = false;
        uint32_t p = para->begin;
        /* The separator that ends a paragraph (U+001C to U+001E, U+0085, U+2029, a line feed) is shaped with it, so a
         * control character among them gets its box. */
        uint32_t pend = pi + 1 < out->paras.len ? out->paras.data[pi + 1].begin : len;
        if (pend < para->end) {
            pend = para->end;
        }
        if (pend > para->end) {
            uint8_t sb = (uint8_t)text[para->end];
            if (!(sb >= 0x1C && sb <= 0x1E) && !(sb == 0xC2 && para->end + 1 < len && (uint8_t)text[para->end + 1] == 0x85)) {
                pend = para->end; /* a line feed ends its paragraph without a glyph */
            }
        }
        while (p <= pend) {
            bool at_end = p == pend;
            if (!at_end && !is_cp_start(text, p, len)) {
                p++;
                continue;
            }
            while (cur_cl + 1 < ncl && cstart[cur_cl + 1] <= p) {
                cur_cl++;
            }
            uint8_t lv = at_end ? seg_level : levels[p];
            kbts_script sc = at_end ? seg_script : scripts[p];
            embi_face *fc = (!at_end && cur_cl + 1 < ncl) ? cl_face[cur_cl] : seg_face;
            bool sm = at_end ? seg_small : (cl_small != nullptr && cur_cl + 1 < ncl && cl_small[cur_cl] != 0);
            bool changed = !seg_open || at_end || lv != seg_level || sc != seg_script ||
                           fc != seg_face || sm != seg_small;
            /* kb sorts the glyphs of a run by insertion, which costs n * n for a long run: cut a run that
             * has grown long, after a space (no script joins across one) when it can, else, for a run of
             * letters with no space, past EMBI_SHAPE_HARD_CUT bytes; always at a grapheme boundary, so a cluster
             * (a combining mark, a ZWJ sequence) is never split (W3q; the cut loses only kerning there). Nothing
             * is shaped in an analysis-only run, so it is never cut. */
            if (!changed && !st->analysis_only && p - seg_start >= EMBI_SHAPE_SOFT_CUT &&
                (flags[p] & KBTS_BREAK_FLAG_GRAPHEME) != 0 &&
                (text[p - 1] == ' ' || p - seg_start >= EMBI_SHAPE_HARD_CUT)) {
                changed = true;
            }
            if (changed && seg_open) {
                /* Close the segment [seg_start, p) and shape it. */
                uint32_t a = seg_start, b = p;
                if (b > a) {
                    kbts_script ks = seg_script == KBTS_SCRIPT_DONT_KNOW ? KBTS_SCRIPT_LATIN
                                                                        : seg_script;
                    bool rtl = (seg_level & 1) != 0;
                    uint32_t inst = 0;
                    const float *user = nullptr;
                    const int16_t *coords = nullptr;
                    float user_buf[64];
                    bool no_face = seg_face == nullptr;
                    if (!no_face && seg_face->font.axis_count > 0 &&
                        seg_face->font.axis_count <= 64) {
                        embi_face_user_coords(seg_face, st, user_buf);
                        inst = embi_face_instance(seg_face, user_buf);
                        user = embi_face_inst_user(seg_face, inst);
                        coords = embi_face_inst_coords(seg_face, inst);
                    }
                    feature_list fl;
                    build_features(st, &fl, !no_face && seg_face->smcp > 0);
                    uint32_t run_first_glyph = out->glyphs.len;
                    shape_entry *e = nullptr;
                    char *caps_buf = nullptr;
                    const uint8_t *shape_text = (const uint8_t *)text + a;
                    float run_size = st->size;
                    bool has_tab = false;
                    for (uint32_t ci = a; ci < b && !no_face && !has_tab; ci++) {
                        uint8_t cb = (uint8_t)text[ci];
                        has_tab = cb < 0x20 || cb == 0x7F || (cb == 0xC2 && ci + 1 < b && ((uint8_t)text[ci + 1] & 0xE0) == 0x80);
                    }
                    if ((seg_small || has_tab) && !no_face) {
                        /* The segment is shaped from a copy of the same byte lengths: with small capitals the lower case
                         * letters are upper case ones at the small size, and a tab is a space (the layout gives a tab its
                         * own advance). */
                        caps_buf = embi_alloc(b - a);
                        const char *q = text + a, *qe = text + b;
                        while (q < qe) {
                            const char *q0 = q;
                            uint32_t off = (uint32_t)(q - (text + a));
                            uint32_t cp = embi_utf8_decode(&q, qe);
                            uint32_t used = (uint32_t)(q - q0);
                            uint32_t u = seg_small ? small_cap_upper(cp) : 0;
                            if (u != 0 && cp_len(u) == used) {
                                embi_utf8_encode(u, caps_buf + off);
                            } else {
                                SDL_memcpy(caps_buf + off, q0, used);
                            }
                        }
                        for (uint32_t ci = 0; ci < b - a; ci++) {
                            /* A tab and the other control characters are shaped as a space (the control ones get their box
                             * glyph in append_glyphs, a font may map them to nothing or drop them). */
                            if ((uint8_t)caps_buf[ci] < 0x20 || caps_buf[ci] == 0x7F) {
                                caps_buf[ci] = 32;
                            } else if ((uint8_t)caps_buf[ci] == 0xC2 && ci + 1 < b - a && ((uint8_t)caps_buf[ci + 1] & 0xE0) == 0x80) {
                                caps_buf[ci + 1] = (char)0xA0;
                                ci++;
                            }
                        }
                        shape_text = (const uint8_t *)caps_buf;
                        if (seg_small) {
                            run_size = st->size * EMBI_SMALL_CAPS_SCALE;
                        }
                    }
                    shape_entry ctx_entry = {0};
                    bool ctx_used = false;
                    uint32_t ctx_cap = 0;
                    if (no_face) {
                        /* Synthesize one entry-less group below. */
                    } else {
                        /* A joining character of the neighbouring text box is stood in for by a ZWJ, so the first or
                         * last letter takes its joined form (CSS Text 3 section 6.1 shaping across boxes). */
                        bool zpre = false, zpost = false;
                        if (st->join_ctx != 0 && !st->analysis_only) {
                            const char *q = text + a;
                            zpre = (st->join_ctx & 1u) != 0 && a == 0 && embi_text_cursive(embi_utf8_decode(&q, text + b));
                            q = text + b - 1;
                            while (q > text + a && (((uint8_t)*q) & 0xC0) == 0x80) {
                                q--;
                            }
                            zpost = (st->join_ctx & 2u) != 0 && b == len && embi_text_cursive(embi_utf8_decode(&q, text + b));
                        }
                        if (!st->analysis_only) {
                            /* A ZWJ or tatweel shaped in another segment (another font covers it) still joins the letters
                             * beside it. */
                            const char *q = text + a;
                            zpre = zpre || (joiner_before(text, a) && embi_text_cursive(embi_utf8_decode(&q, text + b)));
                            q = text + b - 1;
                            while (q > text + a && (((uint8_t)*q) & 0xC0) == 0x80) {
                                q--;
                            }
                            zpost = zpost || (joiner_after(text, len, b) && embi_text_cursive(embi_utf8_decode(&q, text + b)));
                        }
                        if (zpre || zpost) {
                            uint32_t pre = zpre ? 3u : 0u, post = zpost ? 3u : 0u, n = b - a;
                            uint8_t *zb = embi_alloc(pre + n + post);
                            if (zpre) {
                                zb[0] = 0xE2;
                                zb[1] = 0x80;
                                zb[2] = 0x8D;
                            }
                            SDL_memcpy(zb + pre, shape_text, n);
                            if (zpost) {
                                zb[pre + n] = 0xE2;
                                zb[pre + n + 1] = 0x80;
                                zb[pre + n + 2] = 0x8D;
                            }
                            shape_entry *ze = shape_cached(t, seg_face, inst, user, coords, zb, pre + n + post, ks, rtl,
                                                           lang_of(st->lang), &fl);
                            embi_free(zb, pre + n + post);
                            if (ze != nullptr) {
                                ctx_entry = *ze;
                                ctx_cap = ze->glyph_count + 1u;
                                ctx_entry.glyphs = embi_alloc(sizeof(shaped_glyph) * ctx_cap);
                                ctx_entry.glyph_count = 0;
                                for (uint32_t gi = 0; gi < ze->glyph_count; gi++) {
                                    uint32_t cl = ze->glyphs[gi].cluster;
                                    if (cl >= pre && cl < pre + n) {
                                        ctx_entry.glyphs[ctx_entry.glyph_count] = ze->glyphs[gi];
                                        ctx_entry.glyphs[ctx_entry.glyph_count].cluster = cl - pre;
                                        ctx_entry.glyph_count++;
                                    }
                                }
                                ctx_entry.text_len = n;
                                ctx_used = true;
                                e = &ctx_entry;
                            }
                        }
                        if (!ctx_used) {
                            e = shape_cached(t, seg_face, inst, user, coords, shape_text,
                                             b - a, ks, rtl, lang_of(st->lang), &fl);
                        }
                    }
                    if (caps_buf != nullptr) {
                        embi_free(caps_buf, b - a);
                    }
                    if (!no_face && e == nullptr) {
                        /* kb refused the run: keep the text visible as empty glyphs. */
                        no_face = true;
                    }
                    uint32_t upem = seg_face != nullptr ? seg_face->font.metrics.units_per_em : 0;
                    float scale = upem > 0 ? run_size / (float)upem : 0.0f;
                    if (no_face && st->analysis_only) {
                        /* No glyphs: the run only marks the bidi level of its bytes. */
                    } else if (no_face) {
                        shape_entry fake = {0};
                        fake.text = (uint8_t *)text + a;
                        fake.text_len = b - a;
                        append_glyphs(out, &fake, a, text, len, scale, st->letter_spacing,
                                      st->word_spacing, true, st->size, flags);
                    } else {
                        append_glyphs(out, e, a, text, len, scale, st->letter_spacing,
                                      st->word_spacing, false, st->size, flags);
                    }
                    if (ctx_used) {
                        embi_free(ctx_entry.glyphs, sizeof(shaped_glyph) * ctx_cap);
                    }
                    /* Safe-to-break: each cluster's first glyph starts at a grapheme boundary. */
                    for (uint32_t gi = run_first_glyph; gi < out->glyphs.len; gi++) {
                        embi_text_glyph *g = &out->glyphs.data[gi];
                        bool first = gi == run_first_glyph ||
                                     out->glyphs.data[gi - 1].cluster != g->cluster;
                        g->safe_to_break = first && ((flags[g->cluster] & KBTS_BREAK_FLAG_GRAPHEME) != 0 ||
                                                     g->cluster == a);
                    }
                    embi_text_run r;
                    SDL_memset(&r, 0, sizeof r);
                    r.face = no_face ? nullptr : seg_face;
                    r.inst = inst;
                    r.size = run_size;
                    if (!no_face && !seg_face->font.metrics.has_colr) {
                        if ((st->synth & EMBI_SYNTH_WEIGHT) != 0 && st->weight >= 600.0f && seg_face->weight_max < 600 &&
                            seg_face->ax_wght < 0) {
                            r.synth |= EMBI_RUN_SYNTH_BOLD;
                        }
                        if ((st->synth & EMBI_SYNTH_STYLE) != 0 && st->font_style != EMBI_STYLE_NORMAL &&
                            seg_face->style == EMBI_STYLE_NORMAL && seg_face->ax_slnt < 0 && seg_face->ax_ital < 0) {
                            r.synth |= EMBI_RUN_SYNTH_OBLIQUE;
                        }
                    }
                    r.text_begin = a;
                    r.text_end = b;
                    r.glyph_begin = run_first_glyph;
                    r.glyph_end = out->glyphs.len;
                    r.level = seg_level;
                    r.rtl = rtl ? 1 : 0;
                    /* Monotone clusters (non-decreasing left to right, or non-increasing for an
                     * RTL run) let line.c search the glyphs by cluster. */
                    r.mono = 1;
                    for (uint32_t gi = r.glyph_begin; gi < r.glyph_end; gi++) {
                        r.advance += out->glyphs.data[gi].advance;
                        if (gi + 1 < r.glyph_end) {
                            uint32_t c0 = out->glyphs.data[gi].cluster;
                            uint32_t c1 = out->glyphs.data[gi + 1].cluster;
                            if ((rtl && c1 > c0) || (!rtl && c1 < c0)) {
                                r.mono = 0;
                            }
                        }
                    }
                    embi_vec_push(&out->runs, r);
                    out->advance += r.advance;
                }
                seg_open = false;
            }
            if (!at_end && !seg_open) {
                seg_open = true;
                seg_start = p;
                seg_level = lv;
                seg_script = sc;
                seg_face = fc;
                seg_small = sm;
            }
            if (at_end) {
                break;
            }
            p++;
        }
    }

    /* Breaks: every byte with a kind (line, hard, grapheme, word), in ascending order. */
    for (uint32_t i = 0; i <= len; i++) {
        uint8_t kinds = 0;
        if ((flags[i] & KBTS_BREAK_FLAG_LINE_SOFT) != 0) {
            kinds |= EMBI_BREAK_LINE;
        }
        if ((flags[i] & KBTS_BREAK_FLAG_LINE_HARD) != 0) {
            kinds |= EMBI_BREAK_HARD;
        }
        if ((flags[i] & KBTS_BREAK_FLAG_GRAPHEME) != 0) {
            kinds |= EMBI_BREAK_GRAPHEME;
        }
        if ((flags[i] & KBTS_BREAK_FLAG_WORD) != 0) {
            kinds |= EMBI_BREAK_WORD;
        }
        if (kinds != 0 && (i == len || is_cp_start(text, i, len))) {
            embi_text_break br = {i, kinds};
            embi_vec_push(&out->breaks, br);
        }
    }
    /* The layout now uses its faces: a system face stays loaded while any layout does (embi_text_layout_free). */
    for (uint32_t r = 0; r < out->runs.len; r++) {
        if (out->runs.data[r].face != nullptr) {
            out->runs.data[r].face->refs++;
        }
    }
    ok = true;

fail:
    if (!ok) {
        layout_free(out);
    }
    embi_free(levels, (size_t)len + 1);
    embi_free(flags, sizeof(kbts_break_flags) * ((size_t)len + 1));
    embi_free(scripts, sizeof(kbts_script) * ((size_t)len + 1));
    embi_free(cand, sizeof(embi_face *) * 64);
    if (cstart != nullptr) {
        embi_free(cstart, sizeof(uint32_t) * cl_cap);
    }
    if (cl_face != nullptr) {
        embi_free(cl_face, sizeof(embi_face *) * cl_cap);
    }
    if (cl_small != nullptr) {
        embi_free(cl_small, ncl);
    }
    return ok;
}

void embi_shape_stats(const embi_text *t, uint64_t *hits, uint64_t *misses, size_t *bytes)
{
    const shape_state *s = t->shape;
    if (hits != nullptr) {
        *hits = s != nullptr ? s->hits : 0;
    }
    if (misses != nullptr) {
        *misses = s != nullptr ? s->misses : 0;
    }
    if (bytes != nullptr) {
        *bytes = s != nullptr ? s->bytes : 0;
    }
}
