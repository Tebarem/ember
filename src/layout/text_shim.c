/*
 * The text module as seen by layout (W3a). This is the only layout file that includes text.h. See
 * text_shim.h for why. Every function here maps plain layout data onto the text module.
 */
#include "layout/text_shim.h"

#include "font/font.h"
#include "text/glyph_path.h"
#include "text/text.h"

#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdatomic.h>
#include <string.h>

struct embi_lay_text {
    embi_text_layout lay;
    embi_text_vruns vr;
    uint64_t key[2]; /* the content key the layout cache gave this run (embi_tshim_set_key) */
};

embi_text *embi_tshim_create(void)
{
    return embi_text_create();
}

void embi_tshim_destroy(embi_text *t)
{
    embi_text_destroy(t);
}

embi_fontdb *embi_tshim_fonts(embi_text *t)
{
    return embi_text_fonts(t);
}

bool embi_tshim_face_metrics(embi_fontdb *db, const embi_lay_tstyle *ts, float *asc, float *desc,
                             float *gap)
{
    embi_style_match m = {ts->families, ts->family_count, ts->weight, ts->font_style, ts->stretch};
    embi_face *faces[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t n = embi_fontdb_candidates(db, &m, faces, 4);
    if (n == 0 || faces[0] == nullptr) {
        return false;
    }
    const embi_font *font = embi_face_font(faces[0]);
    embi_font_metrics fm;
    memset(&fm, 0, sizeof fm);
    if (font == nullptr || !embi_font_get_metrics(font, &fm) || fm.units_per_em == 0) {
        return false;
    }
    float upem = (float)fm.units_per_em;
    if (fm.use_typo_metrics) {
        *asc = (float)fm.typo_ascender / upem * ts->size;
        *desc = -(float)fm.typo_descender / upem * ts->size;
        *gap = (float)fm.typo_line_gap / upem * ts->size;
    } else {
        *asc = (float)fm.hhea_ascender / upem * ts->size;
        *desc = -(float)fm.hhea_descender / upem * ts->size;
        *gap = (float)fm.hhea_line_gap / upem * ts->size;
    }
    /* Browsers round the ascent and the descent to whole px (Blink's SimpleFontData), so baselines and line boxes
     * land on pixel boundaries and glyph edges stay crisp; the line gap stays as it is. */
    *asc = roundf(*asc);
    *desc = roundf(*desc);
    return true;
}

/* The text module's style of ts. The feature and variation arrays are filled in the caller's
 * storage, which must outlive the call. */
static embi_text_style to_text_style(const embi_lay_tstyle *ts, embi_text_feature *feats,
                                     embi_text_feature *vars)
{
    embi_text_style out;
    memset(&out, 0, sizeof out);
    out.families = ts->families;
    out.family_count = ts->family_count;
    out.size = ts->size;
    out.weight = ts->weight;
    out.font_style = ts->font_style;
    out.stretch = ts->stretch;
    out.letter_spacing = ts->letter_spacing;
    out.word_spacing = ts->word_spacing;
    out.direction = ts->direction;
    out.kerning = ts->kerning;
    out.line_break = ts->line_break;
    out.join_ctx = ts->join_ctx;
    out.bidi_after = ts->bidi_after;
    out.ligatures = ts->ligatures;
    out.optical_sizing = ts->optical_sizing;
    out.synth = ts->synth;
    out.caps = ts->caps;
    out.lang = ts->lang;
    for (uint32_t i = 0; i < ts->nfeats && i < EMBI_LAY_MAX_FEATS; i++) {
        feats[i].tag = ts->feats[i].tag;
        feats[i].value = ts->feats[i].value;
        feats[i].fvalue = ts->feats[i].fvalue;
    }
    out.features = feats;
    out.feature_count = ts->nfeats < EMBI_LAY_MAX_FEATS ? ts->nfeats : EMBI_LAY_MAX_FEATS;
    for (uint32_t i = 0; i < ts->nvars && i < EMBI_LAY_MAX_VARS; i++) {
        vars[i].tag = ts->vars[i].tag;
        vars[i].value = ts->vars[i].value;
        vars[i].fvalue = ts->vars[i].fvalue;
    }
    out.variations = vars;
    out.variation_count = ts->nvars < EMBI_LAY_MAX_VARS ? ts->nvars : EMBI_LAY_MAX_VARS;
    return out;
}

/* Bench and test instrumentation, process-wide and not tied to an engine: embi_tshim_shaped_runs counts every
 * shape call, g_first_* times the first call after embi_tshim_reset_first (the cold start of fonts). They do
 * not take part in layout results, and concurrent engines in one process mix their numbers. */
_Atomic uint64_t embi_tshim_shaped_runs;
static _Atomic uint64_t g_first_ticks;
static _Atomic bool g_first_seen;

uint64_t embi_tshim_first_ticks(void)
{
    return atomic_load_explicit(&g_first_ticks, memory_order_relaxed);
}

void embi_tshim_reset_first(void)
{
    atomic_store_explicit(&g_first_seen, false, memory_order_relaxed);
    atomic_store_explicit(&g_first_ticks, 0, memory_order_relaxed);
}

static embi_lay_shaped *shape_impl(embi_text *t, const char *text, uint32_t len, const embi_lay_tstyle *ts,
                                   bool analysis)
{
    atomic_fetch_add_explicit(&embi_tshim_shaped_runs, 1, memory_order_relaxed);
    embi_lay_shaped *s = embi_calloc(1, sizeof *s);
    if (s == nullptr) {
        return nullptr;
    }
    embi_text_feature feats[EMBI_LAY_MAX_FEATS], vars[EMBI_LAY_MAX_VARS];
    embi_text_style st = to_text_style(ts, feats, vars);
    st.analysis_only = analysis;
    bool first = !atomic_exchange_explicit(&g_first_seen, true, memory_order_relaxed);
    uint64_t t0 = first ? SDL_GetPerformanceCounter() : 0;
    bool shaped = embi_text_shape(t, text, len, &st, &s->lay);
    if (first) {
        atomic_store_explicit(&g_first_ticks, SDL_GetPerformanceCounter() - t0, memory_order_relaxed);
    }
    if (!shaped) {
        embi_text_layout_free(&s->lay);
        embi_free(s, sizeof *s);
        return nullptr;
    }
    /* embi_text_line takes a range inside one paragraph. A run with a preserved newline (a line feed
     * from <br> or from white-space: pre, pre-wrap, pre-line) has several paragraphs: layout takes its
     * breaks, levels and advances from it, and draws each line range on its own, so it has no
     * visual runs of its own (vr stays empty). */
    if (!analysis && s->lay.paras.len <= 1 && !embi_text_line(&s->lay, 0, s->lay.text_len, &s->vr)) {
        embi_text_layout_free(&s->lay);
        embi_text_vruns_free(&s->vr);
        embi_free(s, sizeof *s);
        return nullptr;
    }
    return s;
}

void embi_tshim_set_key(embi_lay_shaped *s, uint64_t k1, uint64_t k2)
{
    s->key[0] = k1;
    s->key[1] = k2;
}

void embi_tshim_key(const embi_lay_shaped *s, uint64_t *k1, uint64_t *k2)
{
    *k1 = s->key[0];
    *k2 = s->key[1];
}

embi_lay_shaped *embi_tshim_shape(embi_text *t, const char *text, uint32_t len, const embi_lay_tstyle *ts)
{
    return shape_impl(t, text, len, ts, false);
}

embi_lay_shaped *embi_tshim_analyze(embi_text *t, const char *text, uint32_t len, const embi_lay_tstyle *ts)
{
    return shape_impl(t, text, len, ts, true);
}

void embi_tshim_free(embi_lay_shaped *s)
{
    if (s == nullptr) {
        return;
    }
    embi_text_layout_free(&s->lay);
    embi_text_vruns_free(&s->vr);
    embi_free(s, sizeof *s);
}

float embi_tshim_advance(const embi_lay_shaped *s)
{
    return s->lay.advance;
}

bool embi_tshim_cursive(uint32_t cp)
{
    return embi_text_cursive(cp);
}

bool embi_tshim_no_ls(uint32_t cp)
{
    return embi_text_no_ls(cp);
}

uint32_t embi_tshim_len(const embi_lay_shaped *s)
{
    return s->lay.text_len;
}

const char *embi_tshim_bytes(const embi_lay_shaped *s)
{
    return s->lay.text;
}

char embi_tshim_byte(const embi_lay_shaped *s, uint32_t i)
{
    return s->lay.text[i];
}

_Atomic uint64_t embi_tshim_measured_bytes;

float embi_tshim_measure(const embi_lay_shaped *s, uint32_t start, uint32_t end)
{
    atomic_fetch_add_explicit(&embi_tshim_measured_bytes, end > start ? end - start : 0u, memory_order_relaxed);
    return embi_text_measure(&s->lay, start, end);
}

uint32_t embi_tshim_break_count(const embi_lay_shaped *s)
{
    return s->lay.breaks.len;
}

bool embi_tshim_break_at(const embi_lay_shaped *s, uint32_t i, uint32_t *offset)
{
    const embi_text_break *br = &s->lay.breaks.data[i];
    if ((br->flags & (EMBI_BREAK_LINE | EMBI_BREAK_HARD)) == 0) {
        return false;
    }
    *offset = br->offset;
    return true;
}

uint32_t embi_tshim_break_offset(const embi_lay_shaped *s, uint32_t i)
{
    return s->lay.breaks.data[i].offset;
}

uint8_t embi_tshim_break_flags(const embi_lay_shaped *s, uint32_t at, uint8_t word_break,
                               uint8_t overflow_wrap, uint8_t line_break)
{
    return embi_text_break_before(&s->lay, at, word_break, overflow_wrap, line_break);
}

uint8_t embi_tshim_level_span(const embi_lay_shaped *s, uint32_t at, uint32_t *lo_out, uint32_t *hi_out)
{
    uint32_t lo = 0, hi = s->lay.runs.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const embi_text_run *r = &s->lay.runs.data[mid];
        if (at < r->text_begin) {
            hi = mid;
        } else if (at >= r->text_end) {
            lo = mid + 1;
        } else {
            *lo_out = r->text_begin;
            *hi_out = r->text_end;
            return r->level;
        }
    }
    *lo_out = 0;
    *hi_out = 0;
    return 0;
}

uint8_t embi_tshim_para_level(const embi_lay_shaped *s, uint32_t at, uint8_t dflt)
{
    if (s == nullptr) {
        return dflt;
    }
    for (uint32_t i = 0; i < s->lay.paras.len; i++) {
        const embi_text_para *pp = &s->lay.paras.data[i];
        if (at < pp->end || (i + 1 < s->lay.paras.len && at < s->lay.paras.data[i + 1].begin)) {
            return at >= pp->begin && !pp->neutral ? (uint8_t)(pp->level & 1u) : dflt;
        }
    }
    return s->lay.paras.len > 0 ? (uint8_t)(s->lay.paras.data[s->lay.paras.len - 1].level & 1u) : dflt;
}

uint8_t embi_tshim_level(const embi_lay_shaped *s, uint32_t at)
{
    uint32_t lo = 0, hi = s->lay.runs.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const embi_text_run *r = &s->lay.runs.data[mid];
        if (at < r->text_begin) {
            hi = mid;
        } else if (at >= r->text_end) {
            lo = mid + 1;
        } else {
            return r->level;
        }
    }
    return 0;
}

float embi_tshim_x_height(embi_fontdb *db, const embi_lay_tstyle *ts)
{
    embi_style_match m = {ts->families, ts->family_count, ts->weight, ts->font_style, ts->stretch};
    embi_face *faces[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t n = embi_fontdb_candidates(db, &m, faces, 4);
    if (n == 0 || faces[0] == nullptr) {
        return 0;
    }
    const embi_font *font = embi_face_font(faces[0]);
    embi_font_metrics fm;
    memset(&fm, 0, sizeof fm);
    if (font == nullptr || !embi_font_get_metrics(font, &fm) || fm.units_per_em == 0 ||
        fm.x_height <= 0) {
        return 0;
    }
    return (float)fm.x_height / (float)fm.units_per_em * ts->size;
}

bool embi_tshim_content_metrics(const embi_lay_shaped *s, float *ca, float *cd)
{
    if (s == nullptr || s->lay.runs.len == 0 || s->lay.runs.data[0].face == nullptr) {
        return false;
    }
    const embi_text_run *r = &s->lay.runs.data[0];
    const embi_font *font = embi_face_font(r->face);
    embi_font_metrics fm;
    memset(&fm, 0, sizeof fm);
    if (font == nullptr || !embi_font_get_metrics(font, &fm) || fm.units_per_em == 0) {
        return false;
    }
    float upem = (float)fm.units_per_em;
    float asc = fm.use_typo_metrics ? (float)fm.typo_ascender : (float)fm.hhea_ascender;
    float desc = fm.use_typo_metrics ? -(float)fm.typo_descender : -(float)fm.hhea_descender;
    *ca = roundf(asc / upem * r->size);
    *cd = roundf(desc / upem * r->size);
    return true;
}

bool embi_tshim_deco_metrics(const embi_lay_shaped *s, float *ul_pos, float *ul_thick, float *st_pos, float *st_size)
{
    if (s == nullptr || s->lay.runs.len == 0 || s->lay.runs.data[0].face == nullptr) {
        return false;
    }
    const embi_text_run *r = &s->lay.runs.data[0];
    const embi_font *font = embi_face_font(r->face);
    embi_font_metrics fm;
    memset(&fm, 0, sizeof fm);
    if (font == nullptr || !embi_font_get_metrics(font, &fm) || fm.units_per_em == 0) {
        return false;
    }
    float k = r->size / (float)fm.units_per_em;
    /* A variable font varies these metrics through MVAR (underline offset and size, strikeout offset and size). */
    *ul_pos = ((float)fm.underline_position + embi_text_run_mvar(r, EMBI_TAG('u', 'n', 'd', 'o'))) * k;
    *ul_thick = ((float)fm.underline_thickness + embi_text_run_mvar(r, EMBI_TAG('u', 'n', 'd', 's'))) * k;
    *st_pos = ((float)fm.strikeout_position + embi_text_run_mvar(r, EMBI_TAG('s', 't', 'r', 'o'))) * k;
    *st_size = ((float)fm.strikeout_size + embi_text_run_mvar(r, EMBI_TAG('s', 't', 'r', 's'))) * k;
    return true;
}

static uint64_t hash_float(uint64_t h, float v)
{
    uint32_t b;
    memcpy(&b, &v, sizeof b);
    return embi_hash64(h ^ b);
}

uint64_t embi_tshim_content_hash(const embi_lay_shaped *s, uint32_t lo, uint32_t hi)
{
    uint64_t h = 0x6A09E667F3BCC908ull;
    if (s == nullptr) {
        return h;
    }
    h = embi_hash64(h ^ s->key[0]);
    h = embi_hash64(h ^ s->key[1]);
    if (hi > s->lay.text_len) {
        hi = s->lay.text_len;
    }
    if (lo > hi) {
        lo = hi;
    }
    if (hi > lo) {
        h = embi_hash_bytes(s->lay.text + lo, hi - lo, h);
    }
    for (uint32_t r = 0; r < s->lay.runs.len; r++) {
        const embi_text_run *run = &s->lay.runs.data[r];
        if (run->text_end <= lo || run->text_begin >= hi) {
            continue;
        }
        h = hash_float(h, run->size);
        h = embi_hash64(h ^ run->inst);
        /* The face itself is not hashed: its address differs between two text systems that load the same font, and the
         * glyph ids, advances and the content key (the font request) identify the glyphs. */
        for (uint32_t g = run->glyph_begin; g < run->glyph_end && g < s->lay.glyphs.len; g++) {
            const embi_text_glyph *gl = &s->lay.glyphs.data[g];
            if (gl->cluster < lo || gl->cluster >= hi) {
                continue;
            }
            h = embi_hash64(h ^ gl->gid);
            h = embi_hash64(h ^ gl->cluster);
            h = hash_float(h, gl->advance);
            h = hash_float(h, gl->x_offset);
            h = hash_float(h, gl->y_offset);
        }
    }
    return h;
}

void embi_tshim_draw(embi_text *t, embi_frame *f, const embi_lay_shaped *s, float x, float y, uint32_t color,
                     uint32_t xform, uint32_t clip)
{
    if (s == nullptr) {
        return;
    }
    embi_text_draw(t, f, &s->lay, &s->vr, x, y, color, xform, clip);
}

void embi_tshim_outline(embi_text *t, const embi_lay_shaped *s, float x, float y, void (*fn)(void *user, const float q[6]),
                        void *user)
{
    if (s == nullptr) {
        return;
    }
    embi_text_outline(t, &s->lay, &s->vr, x, y, fn, user);
}

struct embi_path *embi_tshim_glyph_path(const embi_lay_shaped *s, uint32_t lo, uint32_t hi, float x, float y, float bounds[4])
{
    if (s == nullptr) {
        return nullptr;
    }
    return embi_glyph_path(&s->lay, &s->vr, lo, hi, x, y, bounds);
}

uint64_t embi_tshim_path_hash(const struct embi_path *p)
{
    return embi_glyph_path_hash(p);
}

void embi_tshim_draw_range(embi_text *t, embi_frame *f, const embi_lay_shaped *s, uint32_t lo, uint32_t hi, float x, float y,
                           uint32_t color, uint32_t xform, uint32_t clip)
{
    if (s == nullptr) {
        return;
    }
    if (lo == 0 && hi >= s->lay.text_len && s->vr.items.len > 0) {
        embi_text_draw(t, f, &s->lay, &s->vr, x, y, color, xform, clip);
        return;
    }
    if (hi > s->lay.text_len) {
        hi = s->lay.text_len;
    }
    embi_text_vruns vr;
    SDL_memset(&vr, 0, sizeof vr);
    if (lo < hi && embi_text_line(&s->lay, lo, hi, &vr)) {
        embi_text_draw(t, f, &s->lay, &vr, x, y, color, xform, clip);
    }
    embi_text_vruns_free(&vr);
}
