#pragma once
/*
 * Text system (task R2c). Internal to Ember: cross-module symbols use the embi_ prefix.
 *
 * Pipeline, for one paragraph of UTF-8 text with a style:
 *   1. Font registry (embi_fontdb): every face of every registered file, with family names,
 *      weight, stretch, style, unicode coverage and variation axes.
 *   2. Matching (CSS Fonts 4 section 5.2) and fallback: an ordered candidate list of faces for the
 *      family list, then the embedder fallback families, then the bundled default face.
 *   3. Itemization: SheenBidi levels x kb script runs x fonts (grapheme clusters never split).
 *   4. Shaping: kb_text_shape (direct API) with the CSS features. Results are cached.
 *   5. Line API: break opportunities, measuring, visual reordering of a line.
 *   6. Drawing: glyph outlines through the Slug fill of the renderer (gpu/render.h).
 *
 * Offsets are byte offsets into the UTF-8 text. Lengths are bytes, and positions in px are local
 * CSS px (gpu/render.h). Lifetime: a layout owns a copy of its text. A face borrows the file bytes
 * it was registered with, unless the registration gives ownership.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"
#include "font/font.h"
#include "gpu/render.h"
#include "image/image.h"

typedef struct embi_fontdb embi_fontdb;
typedef struct embi_face embi_face;
typedef struct embi_text embi_text;
/* Line-order cache of a layout (bidi.c): the SheenBidi algorithm and one paragraph per paragraph. */
typedef struct embi_bidi_cache embi_bidi_cache;

/* ------------------------------------------------------------------ enums */

/* CSS font-style, as matched. Oblique faces (variable slnt) and italic faces are distinct. */
enum {
    EMBI_STYLE_NORMAL = 0,
    EMBI_STYLE_ITALIC = 1,
    EMBI_STYLE_OBLIQUE = 2,
};

/* Base direction of a paragraph. Auto takes the first strong character (SheenBidi default). */
enum {
    EMBI_DIR_LTR = 0,
    EMBI_DIR_RTL = 1,
    EMBI_DIR_AUTO = 2,
    /* OR-ed into a style's direction: unicode-bidi: bidi-override, every character takes the paragraph's direction. */
    EMBI_DIR_OVERRIDE = 0x10,
};

/* font-kerning. */
enum {
    EMBI_KERN_AUTO = 0,
    EMBI_KERN_NORMAL = 1,
    EMBI_KERN_NONE = 2,
};

/* Break kinds in embi_text_break.flags (bit set). */
enum {
    EMBI_BREAK_LINE = 1u << 0,      /* UAX #14 line break opportunity */
    EMBI_BREAK_HARD = 1u << 1,      /* mandatory break (newline, paragraph separator) */
    EMBI_BREAK_GRAPHEME = 1u << 2,  /* grapheme cluster boundary (caret stop, break-all) */
    EMBI_BREAK_WORD = 1u << 3,      /* UAX #29 word boundary */
};

/* CSS line-break (CSS Text 3 section 5.2). auto, loose, normal and strict pick kb's Japanese
 * line-break style at shaping time (kb's loose/normal/strict kinsoku rules; auto is normal).
 * anywhere makes every grapheme boundary a soft break (see embi_text_break_before). */
enum {
    EMBI_LINE_BREAK_AUTO = 0,
    EMBI_LINE_BREAK_LOOSE = 1,
    EMBI_LINE_BREAK_NORMAL = 2,
    EMBI_LINE_BREAK_STRICT = 3,
    EMBI_LINE_BREAK_ANYWHERE = 4,
};

/* font-synthesis (CSS Fonts 4 section 5.6): which of the synthetic faces the style allows. */
enum {
    EMBI_SYNTH_WEIGHT = 1u << 0,
    EMBI_SYNTH_STYLE = 1u << 1,
    EMBI_SYNTH_SMALL_CAPS = 1u << 2,
    EMBI_SYNTH_ALL = 7u,
};

/* font-variant-caps. Small caps use the face's smcp / c2sc features when it has them, else lower case letters
 * are drawn as upper case letters at EMBI_SMALL_CAPS_SCALE of the size (CSS Fonts 3 section 6.5). */
enum {
    EMBI_CAPS_NORMAL = 0,
    EMBI_CAPS_SMALL = 1,       /* small-caps, petite-caps */
    EMBI_CAPS_ALL_SMALL = 2,   /* all-small-caps, all-petite-caps */
    EMBI_CAPS_UNICASE = 3,
    EMBI_CAPS_TITLING = 4,
};
#define EMBI_SMALL_CAPS_SCALE 0.7f

/* Synthetic faces drawn by embi_text_draw (embi_text_run.synth). */
enum {
    EMBI_RUN_SYNTH_BOLD = 1u << 0,
    EMBI_RUN_SYNTH_OBLIQUE = 1u << 1,
};
#define EMBI_OBLIQUE_SKEW 0.24932800f /* tan(14 deg): the angle CSS Fonts 4 gives a synthesized oblique */

/* ------------------------------------------------------------------ font registry */

/* @font-face descriptors. Zero or empty means "take it from the font". W2 fills these. */
typedef struct embi_unicode_range {
    uint32_t lo, hi; /* inclusive */
} embi_unicode_range;

typedef struct embi_face_desc {
    embi_str family;                      /* empty: the font's own family name */
    uint16_t weight_min, weight_max;      /* 0: from the font */
    bool has_style;
    uint8_t style;                        /* EMBI_STYLE_*, when has_style */
    float stretch_min, stretch_max;       /* percent; 0: from the font */
    const embi_unicode_range *ranges;     /* unicode-range; empty means every code point */
    uint32_t range_count;
} embi_face_desc;

/* True when this build bundles the default face (EMBER_NO_DEFAULT_FONT unset). */
bool embi_text_has_default_font(void);

#ifdef EMBI_TEST_DEFAULT_FONT_HOOK
/* Tests of a build without the bundled font: the face every new registry gets as its default (the bytes must outlive it). */
void embi_fontdb_test_default(const void *data, size_t size);
#endif

/* A new registry. With EMBER_NO_DEFAULT_FONT unset it holds the bundled default face. */
[[nodiscard]] embi_fontdb *embi_fontdb_create(void);
void embi_fontdb_destroy(embi_fontdb *db);

/* Registers every face of a file (one for an sfnt, all of them for a TTC). WOFF and WOFF2 files
 * are decoded first (src/codec). Returns the number of faces added, 0 on failure.
 *   owned == false: the bytes must stay alive as long as the registry.
 *   owned == true:  the registry takes the bytes. They must come from SDL_malloc (SDL_LoadFile
 *                   does), and the registry frees them with SDL_free. */
uint32_t embi_fontdb_add(embi_fontdb *db, const uint8_t *data, size_t len, bool owned,
                         const embi_face_desc *desc);

/* Embedder fallback families, tried after the family list of a style and before the bundled
 * default. The strings are copied. */
bool embi_fontdb_set_fallback(embi_fontdb *db, const embi_str *families, uint32_t count);

uint32_t embi_fontdb_face_count(const embi_fontdb *db);
/* The bundled default face, nullptr when there is none. */
embi_face *embi_fontdb_default_face(embi_fontdb *db);
/* SZ3b: where the one-time "no font available" warning goes. Text that no face can draw (a build without the bundled default
 * font, no system font and none registered) is skipped, and the first time that happens fn gets a message (fn may be called from
 * a shaping thread). nullptr: the warning goes to SDL_Log. */
/* SZ3b: in a build without the bundled default font the first face the embedder registers becomes the default (last resort)
 * face, which the bundled one would have been. index: the registry index of the face (the face count before the call that
 * added it). No effect when the registry has a default face. */
void embi_fontdb_default_if_none(embi_fontdb *db, uint32_t index);
void embi_fontdb_set_warn(embi_fontdb *db, void (*fn)(void *user, const char *msg), void *user);
/* Gives the warning, once per registry. */
void embi_fontdb_warn_no_font(embi_fontdb *db);
const embi_font *embi_face_font(const embi_face *f);
/* Family name (font name ID 1, or the @font-face override). */
embi_str embi_face_family(const embi_face *f);

/* ------------------------------------------------------------------ CSS matching */

/* The CSS properties that select a face. */
typedef struct embi_style_match {
    const embi_str *families; /* font-family list, in order */
    uint32_t family_count;
    float weight;             /* font-weight, 1..1000 (400 normal) */
    uint8_t style;            /* EMBI_STYLE_* */
    float stretch;            /* font-stretch percent (100 normal) */
} embi_style_match;

/* The ordered candidate faces for a style: for each family the best matching face of that
 * family (CSS Fonts 4 section 5.2: stretch, then style, then weight), then the embedder fallback
 * families, then the bundled default. Each face appears once. Returns the count (at most cap). */
uint32_t embi_fontdb_candidates(embi_fontdb *db, const embi_style_match *m, embi_face **out,
                                uint32_t cap);

/* True when the face's cmap, and its unicode-range descriptor, covers cp. */
bool embi_face_covers(const embi_face *f, uint32_t cp);

/* ------------------------------------------------------------------ text system */

/* Font-feature-settings and font-variation-settings entries. Tags are four characters packed
 * big-endian (EMBI_TAG in font/font.h). */
typedef struct embi_text_feature {
    uint32_t tag;
    int32_t value;  /* features: 0 or 1 usually, larger for alternates; variations: float value */
    float fvalue;   /* variations only */
} embi_text_feature;

typedef struct embi_text_style {
    const embi_str *families; /* font-family list */
    uint32_t family_count;
    float size;               /* font-size, px */
    float weight;             /* font-weight, 1..1000 */
    uint8_t font_style;       /* EMBI_STYLE_* */
    float stretch;            /* font-stretch percent */
    float letter_spacing;     /* px, added after each character (CSS letter-spacing) */
    float word_spacing;       /* px, added at each U+0020 and U+00A0 */
    uint8_t direction;        /* EMBI_DIR_* */
    uint8_t kerning;          /* EMBI_KERN_* */
    uint8_t line_break;       /* EMBI_LINE_BREAK_*: auto..strict set kb's style; anywhere is in break_before */
    uint8_t join_ctx;         /* bit 0: a joining character of another text run precedes the text, bit 1: follows it (shaped with a ZWJ there) */
    bool bidi_after;          /* a strong character of the other direction than `direction` follows the text in its paragraph (another text box): trailing neutrals take it into account (UAX 9 N1) */
    bool ligatures;           /* false: font-variant-ligatures none (or letter-spacing != 0) */
    uint8_t synth;            /* EMBI_SYNTH_* bits the style allows (font-synthesis); 0: none */
    uint8_t caps;             /* EMBI_CAPS_*: font-variant-caps */
    bool optical_sizing;      /* font-optical-sizing auto: opsz follows the font size */
    embi_str lang;            /* BCP 47 language tag, empty for unknown */
    const embi_text_feature *features;       /* font-feature-settings */
    uint32_t feature_count;
    const embi_text_feature *variations;     /* font-variation-settings */
    uint32_t variation_count;
    bool analysis_only;       /* paragraphs, breaks and bidi runs only: no font lookup and no glyphs (the runs have
                               * no face and an empty glyph range); layout shapes a paragraph this way for its
                               * break opportunities and bidi levels (W3q) */
} embi_text_style;

[[nodiscard]] embi_text *embi_text_create(void);
void embi_text_destroy(embi_text *t);
embi_fontdb *embi_text_fonts(embi_text *t);
/* Bytes the shaping cache may hold before the least recently used entries go (default 4 MiB). */
void embi_text_set_shape_cache_budget(embi_text *t, size_t bytes);
/* Drops every cached shaping result and glyph outline (for tests and memory pressure). */
void embi_text_clear_caches(embi_text *t);

/* A glyph in a laid-out run, in visual order within its run (left to right). */
typedef struct embi_text_glyph {
    uint32_t gid;        /* glyph index in the run's face */
    uint32_t cluster;    /* byte offset of the cluster the glyph belongs to */
    float advance;       /* px, including letter-spacing and word-spacing */
    float x_offset;      /* px, positive to the right */
    float y_offset;      /* px, positive up (OpenType convention) */
    bool safe_to_break;  /* the cluster starts a grapheme: a caret stop, safe to break before */
} embi_text_glyph;

/* A shaping run: one face, one instance, one script and one direction. */
typedef struct embi_text_run {
    embi_face *face;
    uint32_t inst;             /* variation instance id within the face (0 when static) */
    float size;                /* px per em */
    uint32_t text_begin, text_end;     /* bytes */
    uint32_t glyph_begin, glyph_end;   /* into layout.glyphs */
    uint8_t level;             /* bidi embedding level */
    uint8_t rtl;               /* direction of the run (level odd) */
    uint8_t mono;              /* clusters never move against the run direction (binary search ok) */
    uint8_t synth;             /* EMBI_RUN_SYNTH_* */
    float advance;             /* px, sum of its glyph advances */
} embi_text_run;
/* The MVAR delta (font units) of metric tag (for example EMBI_TAG('u','n','d','o')) at the variation instance of run r;
 * 0 for a static face or a metric the font does not vary. */
float embi_text_run_mvar(const embi_text_run *r, uint32_t tag);

typedef struct embi_text_break {
    uint32_t offset;  /* byte offset; the break is before this byte */
    uint8_t flags;    /* EMBI_BREAK_* */
} embi_text_break;

typedef struct embi_text_para {
    uint32_t begin, end; /* bytes, end excludes the separator */
    uint8_t level;       /* paragraph embedding level (0 LTR, 1 RTL) */
    bool neutral;        /* EMBI_DIR_AUTO only: the first paragraph, with no strong character (its direction is the caller's) */
} embi_text_para;
typedef EMBI_VEC(embi_text_para) embi_para_vec;

/* The result of shaping. Zeroed is empty. Free with embi_text_layout_free. */
typedef struct embi_text_layout {
    char *text;                                /* owned UTF-8 copy */
    uint32_t text_len;
    EMBI_VEC(embi_text_run) runs;              /* logical order */
    EMBI_VEC(embi_text_glyph) glyphs;
    EMBI_VEC(embi_text_break) breaks;          /* all break positions, ascending */
    embi_para_vec paras;
    embi_bidi_cache *bidi;                     /* line order cache; nullptr when all levels are 0 */
    float size;                                /* the style's font size, px */
    float advance;                             /* px, the whole text in logical order */
} embi_text_layout;

/* Shapes text (len bytes, UTF-8) with style into out. out must be zeroed. Returns false on
 * invalid arguments or allocation failure, after which out is empty. Invalid UTF-8 is shaped as
 * U+FFFD. */
/* Whether a code point is in a cursive script (Arabic, Syriac, N'Ko, Mongolian and so on), which letter-spacing must not
 * pull apart (CSS Text 4 section 8.2). */
[[nodiscard]] bool embi_text_cursive(uint32_t cp);
/* Whether letter-spacing leaves a code point alone: a cursive letter, or a default ignorable code point (a zero-width
 * format control such as U+200B or U+FEFF, CSS Text 4 section 8.2). */
[[nodiscard]] bool embi_text_no_ls(uint32_t cp);
[[nodiscard]] bool embi_text_shape(embi_text *t, const char *text, uint32_t len,
                                   const embi_text_style *style, embi_text_layout *out);
void embi_text_layout_free(embi_text_layout *lay);

/* ------------------------------------------------------------------ line API */

/* One visual run of a line: glyphs [glyph_begin, glyph_end) of layout run `run`, drawn with its
 * first glyph at x (local px from the line's start, left edge). */
typedef struct embi_text_vrun {
    uint32_t run;
    uint32_t glyph_begin, glyph_end;
    float x;
    uint8_t level;
} embi_text_vrun;

typedef struct embi_text_vruns {
    EMBI_VEC(embi_text_vrun) items; /* visual order, left to right */
    float width;                    /* px, the line's advance */
} embi_text_vruns;

/* The line [start, end) of the layout, reordered for display (SheenBidi line, reorder rule of
 * UAX #9 section L2). The range must lie within one paragraph, but it may end at that
 * paragraph's separator (the separator itself draws nothing). The glyphs of a run are cut by
 * cluster. out must be zeroed on the first call; it is reset on each call. */
[[nodiscard]] bool embi_text_line(const embi_text_layout *lay, uint32_t start, uint32_t end,
                                  embi_text_vruns *out);
void embi_text_vruns_free(embi_text_vruns *v);

/* CSS break modes for embi_text_break_before (CSS Text 3 sections 5.2, 5.3 and 5.5). */
enum {
    EMBI_WORD_BREAK_NORMAL = 0,
    EMBI_WORD_BREAK_BREAK_ALL = 1, /* grapheme boundaries are soft break opportunities */
    EMBI_WORD_BREAK_KEEP_ALL = 2,  /* no soft break between two ideographs */
};
enum {
    EMBI_OVERFLOW_WRAP_NORMAL = 0,
    EMBI_OVERFLOW_WRAP_BREAK_WORD = 1, /* emergency breaks, as anywhere */
    EMBI_OVERFLOW_WRAP_ANYWHERE = 2,
};

/* The breaks allowed before byte `at` under the modes, as EMBI_BREAK_* bits:
 *   EMBI_BREAK_HARD      mandatory break (newline, paragraph separator)
 *   EMBI_BREAK_LINE      UAX #14 opportunity, unless keep-all suppresses it (between two
 *                        letter-like or ideographic characters)
 *   EMBI_BREAK_GRAPHEME  a grapheme boundary: a soft opportunity under break-all and under
 *                        line-break: anywhere
 *   EMBI_BREAK_WORD      an emergency break, allowed under overflow-wrap (break-word/anywhere)
 * Zero when `at` is not a break position of the layout. Binary search over lay->breaks. */
uint8_t embi_text_break_before(const embi_text_layout *lay, uint32_t at, uint8_t word_break,
                               uint8_t overflow_wrap, uint8_t line_break);

/* Advance of the bytes [start, end) in px (glyphs whose cluster lies in the range). */
float embi_text_measure(const embi_text_layout *lay, uint32_t start, uint32_t end);

/* Draws the visual runs with the baseline origin at (x, y) (local px). Glyph outlines come from
 * the text system's cache and are filled through the renderer's Slug path. COLR glyphs are drawn
 * as layered fills. xform and clip are as for embi_frame_fill. */
void embi_text_draw(embi_text *t, embi_frame *f, const embi_text_layout *lay,
                    const embi_text_vruns *vr, float x, float y, uint32_t color, uint32_t xform,
                    uint32_t clip);

/* R4e: the outlines of the visual runs, with the baseline origin at (x, y) (local px, y down), as quadratic curves
 * q = x0, y0, cx, cy, x1, y1 handed to fn one by one (the stroke of SVG text). Colour and bitmap glyphs give their outline
 * (or nothing). */
void embi_text_outline(embi_text *t, const embi_text_layout *lay, const embi_text_vruns *vr, float x, float y,
                       void (*fn)(void *user, const float q[6]), void *user);

/* Bitmap colour glyphs (CBDT/CBLC, sbix; W4i) are drawn through a sink, because an image needs a GPU upload on the
 * frame's command buffer, which only the paint layer has. The sink gets the decoded straight RGBA8 pixels (cached by
 * the text system per face, glyph and strike), a key that identifies them for the sink's own image cache, and the
 * device rectangle {x, y, w, h} in local px (origin top-left, as embi_frame_image takes it; the image is drawn
 * stretched to it, linear, tinted white). It returns false when it did not
 * draw. Without a sink, bitmap glyphs draw nothing. COLR glyphs take precedence over bitmaps. */
typedef bool (*embi_text_bitmap_fn)(void *user, embi_frame *f, const embi_pixels *px, uint64_t key,
                                    const float rect[4], uint32_t xform, uint32_t clip);
void embi_text_set_bitmap_sink(embi_text *t, embi_text_bitmap_fn fn, void *user);
