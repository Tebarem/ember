#pragma once
/*
 * The text module as seen by layout (W3a). style.h and text.h cannot be included in one translation
 * unit: props.h and font.h both define embi_font_metrics, and selector.h and text.h both define
 * EMBI_DIR_LTR. So only text_shim.c includes text.h, and the rest of src/layout works through this
 * header: opaque handles and a plain style struct.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct embi_text embi_text;
typedef struct embi_fontdb embi_fontdb;
/* One shaped text run, with its visual lines (text_shim.c). Its tag is shared with fragment.h and box.h. */
typedef struct embi_lay_text embi_lay_shaped;

/* One font-feature-settings or font-variation-settings entry: the tag as four bytes packed
 * big-endian (as text.h's embi_text_feature), its integer value, and its float value. */
typedef struct embi_lay_feat {
    uint32_t tag;
    int32_t value;
    float fvalue;
} embi_lay_feat;

#define EMBI_LAY_MAX_FEATS 16
#define EMBI_LAY_MAX_VARS 8

/* The shaping inputs of a computed style (filled by embi_lay_text_style in line.c). */
typedef struct embi_lay_tstyle {
    const embi_str *families; /* font-family list */
    uint32_t family_count;
    float size;               /* font-size, px */
    float weight;             /* font-weight, 1..1000 */
    uint8_t font_style;       /* 0 normal, 1 italic, 2 oblique */
    float stretch;            /* font-stretch percent */
    float letter_spacing;     /* px */
    float word_spacing;       /* px */
    uint8_t direction;        /* 0 ltr, 1 rtl */
    uint8_t kerning;          /* 0 auto, 1 normal, 2 none */
    uint8_t line_break;
    uint8_t join_ctx;         /* bit 0: a joining character precedes the run in another box, bit 1: follows it */
    bool bidi_after;          /* a strong character of the other direction follows the run in another box (UAX 9 N1 for its trailing neutrals) */
    bool ligatures;
    uint8_t synth;            /* font-synthesis: bit 0 weight, 1 style, 2 small-caps allowed */
    uint8_t caps;             /* font-variant-caps: 0 normal, 1 small-caps, 2 all-small-caps, 3 unicase, 4 titling-caps */
    bool optical_sizing;
    embi_str lang;            /* the language tag, empty for unknown */
    uint32_t nfeats;          /* font-feature-settings (at most EMBI_LAY_MAX_FEATS, the rest dropped) */
    embi_lay_feat feats[EMBI_LAY_MAX_FEATS];
    uint32_t nvars;           /* font-variation-settings (at most EMBI_LAY_MAX_VARS) */
    embi_lay_feat vars[EMBI_LAY_MAX_VARS];
} embi_lay_tstyle;

embi_text *embi_tshim_create(void);
void embi_tshim_destroy(embi_text *t);
embi_fontdb *embi_tshim_fonts(embi_text *t);

/* The ascent, descent and line gap of the primary face of ts, in px (the face is the first match
 * of the family list). Returns false when no face or no metrics match. */
bool embi_tshim_face_metrics(embi_fontdb *db, const embi_lay_tstyle *ts, float *asc, float *desc,
                             float *gap);

/* The number of runs embi_tshim_shape was asked to shape so far (a counter for the tests that check
 * an unchanged document shapes nothing on its next layout). */
extern _Atomic uint64_t embi_tshim_shaped_runs;
/* The ticks (SDL performance counter) the first embi_tshim_shape call took since the last
 * embi_tshim_reset_first: that call finds the fonts and builds the first face's tables, so it is the font
 * loading and face setup of a cold start (W3q). 0 before any call. */
uint64_t embi_tshim_first_ticks(void);
void embi_tshim_reset_first(void);
/* Shapes text (len bytes, UTF-8) and lays out its whole range. Returns nullptr when the text is
 * empty or shaping fails. */
embi_lay_shaped *embi_tshim_shape(embi_text *t, const char *text, uint32_t len,
                                  const embi_lay_tstyle *ts);
/* As embi_tshim_shape, for the analysis of a whole paragraph: the break opportunities, bidi levels and
 * length only (no font lookup, no glyphs: embi_tshim_advance and embi_tshim_measure of the result are 0). */
embi_lay_shaped *embi_tshim_analyze(embi_text *t, const char *text, uint32_t len, const embi_lay_tstyle *ts);
void embi_tshim_free(embi_lay_shaped *s);
/* A 128-bit content key kept with a shaped run (set by the layout cache from the text, the shaping style, the
 * language and the line-break mode; the inline analysis cache builds its key from them). */
void embi_tshim_set_key(embi_lay_shaped *s, uint64_t k1, uint64_t k2);
void embi_tshim_key(const embi_lay_shaped *s, uint64_t *k1, uint64_t *k2);
float embi_tshim_advance(const embi_lay_shaped *s);
/* Whether a code point is in a cursive script, which letter-spacing leaves alone. */
bool embi_tshim_cursive(uint32_t cp);
/* Whether letter-spacing leaves a code point alone (cursive, or a zero-width format control). */
bool embi_tshim_no_ls(uint32_t cp);
uint32_t embi_tshim_len(const embi_lay_shaped *s);
char embi_tshim_byte(const embi_lay_shaped *s, uint32_t i);
const char *embi_tshim_bytes(const embi_lay_shaped *s);
float embi_tshim_measure(const embi_lay_shaped *s, uint32_t start, uint32_t end);
/* The total number of bytes embi_tshim_measure was asked to measure so far (a counter for the tests
 * that check the layout measures each byte a bounded number of times). */
extern _Atomic uint64_t embi_tshim_measured_bytes;
uint32_t embi_tshim_break_count(const embi_lay_shaped *s);
/* Break i: true when it is a line or mandatory break opportunity, with its byte offset in offset. */
bool embi_tshim_break_at(const embi_lay_shaped *s, uint32_t i, uint32_t *offset);

/* Break and bidi queries for the inline layout (W3c, inline.c). The break list of a shaped run
 * holds every byte with a break kind; embi_tshim_break_flags applies the CSS modes to one
 * position (EMBI_BREAK_* bits, see text.h). The modes are the text.h enum values. */
uint32_t embi_tshim_break_offset(const embi_lay_shaped *s, uint32_t i);
uint8_t embi_tshim_break_flags(const embi_lay_shaped *s, uint32_t at, uint8_t word_break,
                               uint8_t overflow_wrap, uint8_t line_break);
/* The bidi embedding level of the byte at (0 when the run list is empty). */
uint8_t embi_tshim_level(const embi_lay_shaped *s, uint32_t at);
/* The level at `at` and the byte range [*lo, *hi) of the run it lies in (an empty range when no run covers it). */
uint8_t embi_tshim_level_span(const embi_lay_shaped *s, uint32_t at, uint32_t *lo, uint32_t *hi);
/* The base level of the bidi paragraph that holds byte `at` (dflt when there is none). */
uint8_t embi_tshim_para_level(const embi_lay_shaped *s, uint32_t at, uint8_t dflt);
/* The x-height of the primary face of ts, in px (0 when unknown). */
float embi_tshim_x_height(embi_fontdb *db, const embi_lay_tstyle *ts);
/* The decoration metrics of the first face of a shaped run, in px (W4c, text decorations): the underline position
 * (negative below the baseline, the font's own sign), its thickness, and the strikeout position (positive above the
 * baseline) and size. Each value
 * comes from the font's post and OS/2 tables scaled to the run's size. False when the run has no face or the font
 * gives no metrics; the caller then uses its own defaults. */
/* The content area of the first run's face (CSS 2.1 10.6.1): its ascent and descent in px, rounded as the layout rounds
 * them (the height of a text fragment of that face is their sum). False when the run has no face or no metrics. */
bool embi_tshim_content_metrics(const embi_lay_shaped *s, float *ca, float *cd);
bool embi_tshim_deco_metrics(const embi_lay_shaped *s, float *ul_pos, float *ul_thick, float *st_pos, float *st_size);

/* The content hash of a shaped run for the damage of W4d: the bytes [lo, hi) of the run, the glyphs whose cluster lies in
 * that range (glyph id, advance, offsets), the run's size and instance, and the content key the layout gave the run (the
 * font request). The face's address is not hashed (two text systems load separate faces).
 * Two text items with equal hashes paint the same glyphs. The hash changes when a glyph changes even if the item's
 * rect, baseline and color stay the same (a "10" that becomes "11" in equal-width digits). */
uint64_t embi_tshim_content_hash(const embi_lay_shaped *s, uint32_t lo, uint32_t hi);

typedef struct embi_frame embi_frame; /* owned by the renderer (gpu/render.h); the same typedef there */
/* Draws the bytes [lo, hi) of a run (one line's piece of its text) with the pen at (x, y). */
void embi_tshim_draw_range(embi_text *t, embi_frame *f, const embi_lay_shaped *s, uint32_t lo, uint32_t hi, float x, float y,
                           uint32_t color, uint32_t xform, uint32_t clip);
/* R4e: the outline of a shaped run as quadratic curves (x0, y0, cx, cy, x1, y1) with the baseline origin at (x, y), y down. */
void embi_tshim_outline(embi_text *t, const embi_lay_shaped *s, float x, float y, void (*fn)(void *user, const float q[6]),
                        void *user);
/* The outline of the bytes [lo, hi) of a shaped run as a path in local px, with the baseline origin at (x, y) and the glyphs
 * made as embi_text_draw makes them (the run's instance, the synthetic oblique). bounds gets x0, y0, x1, y1. nullptr when the
 * range has no outline. The caller owns the path until a display list takes it (embi_tshim_path_hash gives its content hash). */
struct embi_path *embi_tshim_glyph_path(const embi_lay_shaped *s, uint32_t lo, uint32_t hi, float x, float y, float bounds[4]);
uint64_t embi_tshim_path_hash(const struct embi_path *p);
/* Draws the visual runs of a shaped run with the baseline origin at (x, y) (local px) through the text system's glyph path
 * (the renderer's Slug fill). t is the text system the run was shaped with. xform and clip are as for embi_frame_fill. */
void embi_tshim_draw(embi_text *t, embi_frame *f, const embi_lay_shaped *s, float x, float y, uint32_t color,
                     uint32_t xform, uint32_t clip);
