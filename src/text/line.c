/*
 * Line API (R2c): visual order of a line, measuring and break opportunities. Layout decides where
 * lines break (W3). This file orders the glyphs of a line the way they are displayed and answers
 * "may I break here" questions.
 *
 * Cost. Runs are in logical order with ascending byte ranges, and the breaks are sorted, so every
 * lookup is a binary search. A run whose glyph clusters are monotone (embi_text_run.mono) is cut
 * by binary search too. Only the bidi reordering of a mixed-direction paragraph goes through
 * SheenBidi, and only per line (bidi.c keeps the algorithm objects in the layout).
 */

#include "text/text_int.h"

#include <SDL3/SDL_stdinc.h>

void embi_text_vruns_free(embi_text_vruns *v)
{
    if (v == nullptr) {
        return;
    }
    embi_vec_free(&v->items);
    v->width = 0.0f;
}

/* The paragraph for the range [start, end). The range may run into the paragraph's separator,
 * which lies before the next paragraph's begin. It may not reach that begin. */
static bool para_of(const embi_text_layout *lay, uint32_t start, uint32_t end, uint32_t *idx)
{
    uint32_t n = lay->paras.len;
    for (uint32_t i = 0; i < n; i++) {
        const embi_text_para *p = &lay->paras.data[i];
        uint32_t next = i + 1 < n ? lay->paras.data[i + 1].begin : lay->text_len;
        if (start >= p->begin && start <= p->end && end <= next) {
            *idx = i;
            return true;
        }
    }
    return false;
}

/* First layout run whose byte range ends after pos. Runs are ascending, so this is a search. */
static uint32_t first_run_after(const embi_text_layout *lay, uint32_t pos)
{
    uint32_t lo = 0, hi = lay->runs.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (lay->runs.data[mid].text_end > pos) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

/* Within [b, e), the first glyph whose cluster is >= v, for non-decreasing clusters. */
static uint32_t first_cluster_ge(const embi_text_glyph *gl, uint32_t b, uint32_t e, uint32_t v)
{
    while (b < e) {
        uint32_t mid = b + (e - b) / 2;
        if (gl[mid].cluster >= v) {
            e = mid;
        } else {
            b = mid + 1;
        }
    }
    return b;
}

/* Within [b, e), the first glyph whose cluster is < v, for non-increasing clusters. */
static uint32_t first_cluster_lt(const embi_text_glyph *gl, uint32_t b, uint32_t e, uint32_t v)
{
    while (b < e) {
        uint32_t mid = b + (e - b) / 2;
        if (gl[mid].cluster < v) {
            e = mid;
        } else {
            b = mid + 1;
        }
    }
    return b;
}

/* The glyph range [g0, g1) of run r whose clusters lie in [lo, hi). Glyphs of a run are in visual
 * order, so the selected clusters form one contiguous block. Returns false when there are none. */
static bool glyph_span(const embi_text_layout *lay, const embi_text_run *r, uint32_t lo,
                       uint32_t hi, uint32_t *g0, uint32_t *g1)
{
    const embi_text_glyph *gl = lay->glyphs.data;
    uint32_t a, z;
    if (r->mono) {
        if (!r->rtl) {
            a = first_cluster_ge(gl, r->glyph_begin, r->glyph_end, lo);
            z = first_cluster_ge(gl, r->glyph_begin, r->glyph_end, hi);
        } else {
            a = first_cluster_lt(gl, r->glyph_begin, r->glyph_end, hi);
            z = first_cluster_lt(gl, r->glyph_begin, r->glyph_end, lo);
        }
    } else {
        /* Clusters out of order (a pre-base matra, say): scan the run. */
        a = r->glyph_end;
        z = r->glyph_begin;
        for (uint32_t g = r->glyph_begin; g < r->glyph_end; g++) {
            uint32_t c = gl[g].cluster;
            if (c >= lo && c < hi) {
                if (a == r->glyph_end) {
                    a = g;
                }
                z = g + 1;
            }
        }
        if (a == r->glyph_end) {
            z = a;
        }
    }
    *g0 = a;
    *g1 = z;
    return a < z;
}

/* Appends the visual run of layout run ri for the bytes [lo, hi), at pen x. */
static void emit_span(const embi_text_layout *lay, uint32_t ri, uint32_t lo, uint32_t hi,
                      uint8_t level, float *x, embi_text_vruns *out)
{
    uint32_t g0 = 0, g1 = 0;
    if (!glyph_span(lay, &lay->runs.data[ri], lo, hi, &g0, &g1)) {
        return;
    }
    embi_text_vrun v;
    v.run = ri;
    v.glyph_begin = g0;
    v.glyph_end = g1;
    v.x = *x;
    v.level = level;
    embi_vec_push(&out->items, v);
    for (uint32_t g = g0; g < g1; g++) {
        *x += lay->glyphs.data[g].advance;
    }
}

static uint32_t umax(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t umin(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/* The byte length of the paragraph separator at `at` when it is a control character that is drawn (U+001C, U+001D, U+001E,
 * U+0085), else 0. */
static uint32_t visible_sep_len(const embi_text_layout *lay, uint32_t at)
{
    if (at >= lay->text_len) {
        return 0;
    }
    uint8_t b = (uint8_t)lay->text[at];
    if (b >= 0x1Cu && b <= 0x1Eu) {
        return 1;
    }
    return b == 0xC2u && at + 1 < lay->text_len && (uint8_t)lay->text[at + 1] == 0x85u ? 2 : 0;
}

bool embi_text_line(const embi_text_layout *lay, uint32_t start, uint32_t end,
                    embi_text_vruns *out)
{
    if (lay == nullptr || out == nullptr || start > end || end > lay->text_len) {
        return false;
    }
    out->items.len = 0;
    out->width = 0.0f;
    if (start == end) {
        return true;
    }
    uint32_t pi = 0;
    if (!para_of(lay, start, end, &pi)) {
        return false;
    }
    const embi_text_para *para = &lay->paras.data[pi];
    /* The separator draws nothing: the line ends at the paragraph's last byte. Except the control characters among the
     * separators (U+001C to U+001E and NEL), which CSS Text 3 section 4.1 draws visibly: they end the line drawn. */
    uint32_t cend = umin(end, para->end);
    uint32_t vend = umin(end, para->end + visible_sep_len(lay, para->end));
    if (vend <= start) {
        return true;
    }

    float x = 0.0f;
    if (lay->bidi == nullptr) {
        /* Every level is 0: the logical order is the visual order. */
        for (uint32_t ri = first_run_after(lay, start); ri < lay->runs.len; ri++) {
            const embi_text_run *r = &lay->runs.data[ri];
            if (r->text_begin >= vend) {
                break;
            }
            uint32_t lo = umax(r->text_begin, start), hi = umin(r->text_end, vend);
            if (hi > lo) {
                emit_span(lay, ri, lo, hi, r->level, &x, out);
            }
        }
        out->width = x;
        return true;
    }

    embi_bidi_run_vec bruns = {0};
    if (cend > start && !embi_bidi_line_runs(lay->bidi, pi, start, cend, &bruns)) {
        embi_vec_free(&bruns);
        return false;
    }
    for (uint32_t bi = 0; bi < bruns.len; bi++) {
        const embi_bidi_run *br = &bruns.data[bi];
        uint32_t blo = br->begin, bhi = br->begin + br->len;
        /* The layout runs that overlap this visual run: a contiguous block in logical order. */
        uint32_t first = first_run_after(lay, blo);
        uint32_t last = first;
        while (last < lay->runs.len && lay->runs.data[last].text_begin < bhi) {
            last++;
        }
        /* Odd levels reverse the order of the runs. The glyphs of each run keep their order (kb
         * already put them in display order). */
        uint32_t count = last - first;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t ri = (br->level & 1) ? last - 1 - k : first + k;
            const embi_text_run *r = &lay->runs.data[ri];
            uint32_t lo = umax(umax(r->text_begin, blo), start);
            uint32_t hi = umin(umin(r->text_end, bhi), cend);
            if (hi > lo) {
                emit_span(lay, ri, lo, hi, br->level, &x, out);
            }
        }
    }
    embi_vec_free(&bruns);
    for (uint32_t ri = first_run_after(lay, umax(cend, start)); vend > cend && ri < lay->runs.len; ri++) {
        const embi_text_run *r = &lay->runs.data[ri];
        if (r->text_begin >= vend) {
            break;
        }
        uint32_t lo = umax(umax(r->text_begin, cend), start), hi = umin(r->text_end, vend);
        if (hi > lo) {
            emit_span(lay, ri, lo, hi, r->level, &x, out);
        }
    }
    out->width = x;
    return true;
}

/* Advance of the glyphs of the bytes [start, end). */
float embi_text_measure(const embi_text_layout *lay, uint32_t start, uint32_t end)
{
    if (lay == nullptr || start >= end) {
        return 0.0f;
    }
    float w = 0.0f;
    for (uint32_t ri = first_run_after(lay, start); ri < lay->runs.len; ri++) {
        const embi_text_run *r = &lay->runs.data[ri];
        if (r->text_begin >= end) {
            break;
        }
        uint32_t lo = umax(r->text_begin, start), hi = umin(r->text_end, end);
        uint32_t g0 = 0, g1 = 0;
        if (hi > lo && glyph_span(lay, r, lo, hi, &g0, &g1)) {
            for (uint32_t g = g0; g < g1; g++) {
                w += lay->glyphs.data[g].advance;
            }
        }
    }
    return w;
}

/* keep-all (CSS Text 3 section 5.3): no soft break between two letter-like or ideographic
 * characters. Letter-like is AL and NU (ASCII letters and digits, and the letters of other
 * scripts, which are the code points outside the punctuation, symbol, CJK and fullwidth blocks).
 * Ideographic is ID and AI: Han, kana, Hangul syllables and jamo. */
static bool keep_all_class(uint32_t cp)
{
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
        (cp >= 0xAC00 && cp <= 0xD7AF)) {
        return true; /* Hangul */
    }
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0x20000 && cp <= 0x2FA1F)) {
        return true; /* kana and ideographs */
    }
    return cp >= 0xC0 && !(cp >= 0x0300 && cp <= 0x036F) && !(cp >= 0x2000 && cp <= 0x2BFF) &&
           !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xFF00 && cp <= 0xFFEF);
}

/* break-all treats the letters like ideographs (UAX #14 class ID) and leaves the other classes as they are. What follows
 * is the part of the UAX #14 pair table that matters for an ID next to another class: whether a break is forbidden
 * before cp (closing and exclamation marks, separators, hyphens, quotes, glue, postfix signs, the small kana) and
 * after cp (opening marks, quotes, glue, prefix signs). line-break: loose lets a break go before U+2010 and U+2013. */
static bool ba_forbid_before(uint32_t cp, uint8_t line_break)
{
    switch (cp) {
    case ' ': case 0x09: case 0x0A: case 0x0C: case 0x0D:
    case '!': case '?': case ',': case '.': case ':': case ';': case '/': case ')': case ']': case '}':
    case '"': case 0x27: case '-': case '%': case '|': case 0x00A0: case 0x00A2: case 0x00B0: case 0x00BB:
    case 0x2011: case 0x2019: case 0x201D: case 0x2026: case 0x2030: case 0x2032: case 0x2033: case 0x203C:
    case 0x203D: case 0x2047: case 0x2048: case 0x2049: case 0x3001: case 0x3002: case 0x3009: case 0x300B:
    case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x30FB: case 0x30FC: case 0xFF01: case 0xFF09:
    case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF3D: case 0xFF5D: case 0xFF60:
    case 0xFF61: case 0xFF63: case 0xFF64: case 0x200B: case 0x2060: case 0x200D: case 0xFEFF:
        return true;
    case 0x2010: case 0x2013: case 0x301C: case 0x30A0:
        return line_break != EMBI_LINE_BREAK_LOOSE;
    default:
        break;
    }
    if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x1680 || cp == 0x3000 || cp == 0x205F || cp == 0x202F) {
        return true; /* spaces: BA, SP and GL */
    }
    /* small kana (NS) */
    return (cp >= 0x3041 && cp <= 0x3049 && (cp & 1) != 0) || cp == 0x3063 || cp == 0x3083 || cp == 0x3085 ||
           cp == 0x3087 || cp == 0x308E || (cp >= 0x30A1 && cp <= 0x30A9 && (cp & 1) != 0) || cp == 0x30C3 ||
           cp == 0x30E3 || cp == 0x30E5 || cp == 0x30E7 || cp == 0x30EE || (cp >= 0x0300 && cp <= 0x036F);
}

static bool ba_forbid_after(uint32_t cp)
{
    switch (cp) {
    case '(': case '[': case '{': case '"': case 0x27: case '$': case 0x5C: case '+': case 0x00A0:
    case 0x00A3: case 0x00A5: case 0x00AB: case 0x2011: case 0x2018: case 0x201C: case 0x2060: case 0x200B:
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0xFF08: case 0xFF3B:
    case 0xFF5B: case 0xFF5F: case 0xFFE1: case 0xFFE5: case 0x200D: case 0xFEFF:
        return true;
    default:
        return false;
    }
}

uint8_t embi_text_break_before(const embi_text_layout *lay, uint32_t at, uint8_t word_break,
                               uint8_t overflow_wrap, uint8_t line_break)
{
    if (lay == nullptr || at > lay->text_len) {
        return 0;
    }
    /* The breaks are ascending by offset: find the one at `at`. */
    uint32_t lo = 0, hi = lay->breaks.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (lay->breaks.data[mid].offset < at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= lay->breaks.len || lay->breaks.data[lo].offset != at) {
        return 0;
    }
    uint8_t kinds = lay->breaks.data[lo].flags;
    uint8_t out = 0;
    if ((kinds & EMBI_BREAK_HARD) != 0) {
        out |= EMBI_BREAK_HARD;
    }
    if ((kinds & EMBI_BREAK_LINE) != 0) {
        bool suppress = false;
        if (word_break == EMBI_WORD_BREAK_KEEP_ALL && at > 0 && at < lay->text_len) {
            /* The code point before `at`: back over its continuation bytes. */
            uint32_t b = at - 1;
            while (b > 0 && (((uint8_t)lay->text[b]) & 0xC0) == 0x80) {
                b--;
            }
            const char *end = lay->text + lay->text_len;
            const char *p = lay->text + b;
            uint32_t before = embi_utf8_decode(&p, end);
            p = lay->text + at;
            uint32_t after = embi_utf8_decode(&p, end);
            suppress = keep_all_class(before) && keep_all_class(after);
        }
        if (!suppress) {
            out |= EMBI_BREAK_LINE;
        }
    }
    if ((kinds & EMBI_BREAK_GRAPHEME) != 0) {
        if (line_break == EMBI_LINE_BREAK_ANYWHERE) {
            out |= EMBI_BREAK_GRAPHEME;
        } else if (word_break == EMBI_WORD_BREAK_BREAK_ALL && at > 0 && at < lay->text_len) {
            /* break-all makes the letters (AL, HL, NU and the like) break like ideographs: a break between two of them. Every
             * other pair keeps the UAX #14 rules (no break before a space, a closing mark or a full stop, after an opening
             * mark, around a no-break space), which are the LINE breaks above. */
            uint32_t b = at - 1;
            while (b > 0 && (((uint8_t)lay->text[b]) & 0xC0) == 0x80) {
                b--;
            }
            const char *end = lay->text + lay->text_len;
            const char *p = lay->text + b;
            uint32_t before = embi_utf8_decode(&p, end);
            p = lay->text + at;
            uint32_t after = embi_utf8_decode(&p, end);
            bool lb = keep_all_class(before), la = keep_all_class(after);
            if ((lb && la) || (lb && !ba_forbid_before(after, line_break)) || (la && !ba_forbid_after(before))) {
                out |= EMBI_BREAK_GRAPHEME;
            }
        }
        if (overflow_wrap != EMBI_OVERFLOW_WRAP_NORMAL) {
            out |= EMBI_BREAK_WORD;
        }
    }
    return out;
}
