/*
 * White space processing (CSS Text 3 section 4, W3a section 1). Each text node is processed when
 * the box tree is built: segment breaks are transformed, runs of collapsible spaces collapse to one,
 * white-space-collapse: discard removes collapsible characters, and text-transform applies. The state
 * prev_space carries across text nodes of one inline formatting context, so a space after a space in
 * a neighbouring inline box is also removed.
 *
 * Segment breaks (CSS Text 3 section 4.1.2). A newline between two East Asian wide characters is
 * removed unless either side is Hangul (then it is a space), and it is removed next to U+200B. Any other collapsible segment break is a space. In the preserving modes a newline stays a
 * break, which W3c turns into a forced line break (a space until then).
 *
 * text-transform (section 2.1) uses the Unicode default case mapping (casemap.c). capitalize starts
 * a word at each character that is not inside one (the word rule in casemap.c), so "foo-bar" gives
 * "Foo-Bar". The word state carries across the text nodes of an inline formatting context (a word
 * split by <b> is capitalised once) and resets at block boundaries, atomic inlines and breaks.
 *
 * Limits. The character before a segment break (for the U+200B and East Asian wide checks) is the
 * last one of the same text node: a break at the very start of a node does not see the previous
 * node's last character (W3c can carry it in embi_ws if it matters). A collapsed tab is a space; a preserved tab stays a tab (inline.c advances it to the next tab stop). The
 * Final_Sigma rule is not applied (see casemap.c).
 */
#include "layout_int.h"

#include "css/props.h"

static bool ws_collapsible(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_segment_break(uint32_t c)
{
    return c == '\n';
}

/* East Asian Wide and Fullwidth characters (UAX 11, approximated by the blocks that hold them). */
static bool is_east_asian_wide(uint32_t c)
{
    return (c >= 0x1100u && c <= 0x115Fu) || (c >= 0x2E80u && c <= 0x303Eu) ||
           (c >= 0x3041u && c <= 0x33FFu) || (c >= 0x3400u && c <= 0x4DBFu) ||
           (c >= 0x4E00u && c <= 0x9FFFu) || (c >= 0xA000u && c <= 0xA4CFu) ||
           (c >= 0xAC00u && c <= 0xD7A3u) || (c >= 0xF900u && c <= 0xFAFFu) ||
           (c >= 0xFE30u && c <= 0xFE4Fu) || (c >= 0xFF00u && c <= 0xFF60u) ||
           (c >= 0xFFE0u && c <= 0xFFE6u) || (c >= 0x1B000u && c <= 0x1B2FFu) ||
           (c >= 0x20000u && c <= 0x3FFFDu);
}

static bool is_hangul(uint32_t c)
{
    return (c >= 0x1100u && c <= 0x11FFu) || (c >= 0x3130u && c <= 0x318Fu) ||
           (c >= 0xA960u && c <= 0xA97Fu) || (c >= 0xAC00u && c <= 0xD7FFu);
}

/* Whether a segment break next to this character may be removed (CSS Text 3 section 4.1.2): the
 * character is Fullwidth, Wide or Halfwidth, and not Hangul. */
static bool break_removable_next_to(uint32_t c)
{
    if (is_hangul(c)) {
        return false;
    }
    return is_east_asian_wide(c) || (c >= 0xFF61u && c <= 0xFFDCu);
}

/* Punctuation and symbols (general category P* or S*) of East Asian Width A, approximated by the characters and blocks
 * that hold them (UAX 11). */
static bool ambiguous_punct(uint32_t c)
{
    switch (c) {
    case 0xA1u: case 0xA4u: case 0xA7u: case 0xA8u: case 0xB0u: case 0xB1u: case 0xB4u: case 0xB6u: case 0xB7u:
    case 0xB8u: case 0xBFu: case 0xD7u: case 0xF7u: case 0x2010u: case 0x2013u: case 0x2014u: case 0x2015u:
    case 0x2016u: case 0x2018u: case 0x2019u: case 0x201Cu: case 0x201Du: case 0x2020u: case 0x2021u: case 0x2022u:
    case 0x2024u: case 0x2025u: case 0x2026u: case 0x2027u: case 0x2030u: case 0x2032u: case 0x2033u: case 0x2035u:
    case 0x203Bu: case 0x203Eu: case 0x20ACu: case 0x2103u: case 0x2105u: case 0x2109u: case 0x2116u: case 0x2121u:
    case 0x2122u: case 0x2126u: case 0x212Bu:
        return true;
    default:
        break;
    }
    return (c >= 0x2190u && c <= 0x2199u) || (c >= 0x21B8u && c <= 0x21B9u) || c == 0x21D2u || c == 0x21D4u ||
           c == 0x21E7u || (c >= 0x2200u && c <= 0x22FFu) || c == 0x2312u ||
           (c >= 0x249Cu && c <= 0x24E9u) || (c >= 0x2500u && c <= 0x25FFu) || (c >= 0x2605u && c <= 0x2606u) ||
           c == 0x2609u || (c >= 0x260Eu && c <= 0x260Fu) || c == 0x261Cu || c == 0x261Eu || c == 0x2640u ||
           c == 0x2642u || (c >= 0x2660u && c <= 0x266Fu);
}

/* CSS Text 3 section 4.1.2, the writing-system rule: in Chinese, Japanese or Yi text a segment break is also removed when
 * one side is such punctuation and the other side is Fullwidth, Wide or Halfwidth (and not Hangul). */
static bool cjy_break_removed(uint32_t a, uint32_t b)
{
    return (ambiguous_punct(a) && break_removable_next_to(b)) || (ambiguous_punct(b) && break_removable_next_to(a));
}

bool embi_lay_lang_cjy(embi_str lang)
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
        if (e - b == 4) {
            static const char *const sc[] = {"hani", "hans", "hant", "hira", "kana", "hrkt", "jpan", "bopo", "yiii"};
            for (uint32_t i = 0; i < sizeof sc / sizeof sc[0]; i++) {
                if (SDL_strncasecmp(lang.ptr + b, sc[i], 4) == 0) {
                    return true;
                }
            }
            return false;
        }
    }
    static const char *const pl[] = {"zh", "ja", "ii", "yue", "cmn", "wuu", "hak", "nan", "gan", "hsn", "lzh", "cdo"};
    for (uint32_t i = 0; i < sizeof pl / sizeof pl[0]; i++) {
        if (SDL_strlen(pl[i]) == n && SDL_strncasecmp(lang.ptr, pl[i], n) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t embi_lay_ws_mode(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_white_space_collapse);
    if (v == nullptr || v->kind != EMBI_V_KEYWORD) {
        return EMBI_WS_COLLAPSE;
    }
    switch (v->kw) {
    case EMBI_KW_discard:
        return EMBI_WS_DISCARD;
    case EMBI_KW_preserve:
        return EMBI_WS_PRESERVE;
    case EMBI_KW_preserve_breaks:
        return EMBI_WS_PRESERVE_BREAKS;
    case EMBI_KW_preserve_spaces:
        return EMBI_WS_PRESERVE_SPACES;
    case EMBI_KW_break_spaces:
        return EMBI_WS_BREAK_SPACES;
    default:
        return EMBI_WS_COLLAPSE;
    }
}

uint16_t embi_lay_transform(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_text_transform);
    if (v == nullptr) {
        return EMBI_KW_none;
    }
    if (v->kind != EMBI_V_KEYWORD) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(v, &n);
        uint16_t cs = EMBI_KW_none, flags = 0;
        for (uint32_t i = 0; items != nullptr && i < n; i++) {
            if (items[i].kind != EMBI_V_KEYWORD) {
                continue;
            }
            if (items[i].kw == EMBI_KW_full_width) {
                flags |= EMBI_TT_FW;
            } else if (items[i].kw == EMBI_KW_full_size_kana) {
                flags |= EMBI_TT_FSK;
            } else {
                cs = (uint16_t)items[i].kw;
            }
        }
        if (cs == EMBI_KW_none && (flags & EMBI_TT_FW) != 0) {
            cs = EMBI_KW_full_width;
            flags &= (uint16_t)~EMBI_TT_FW;
        } else if (cs == EMBI_KW_none && (flags & EMBI_TT_FSK) != 0) {
            cs = EMBI_KW_full_size_kana;
            flags &= (uint16_t)~EMBI_TT_FSK;
        }
        return (uint16_t)(cs | flags);
    }
    return (uint16_t)v->kw;
}

/* Writes one character with the text-transform applied. word_start is true at the start of a
 * word (capitalize). */
static void emit_cp(embi_sb *out, uint32_t cp, uint16_t transform, bool word_start, bool final_sigma)
{
    uint32_t m[3] = {cp, 0, 0};
    int n = 1;
    uint16_t extra = (uint16_t)(transform & ~EMBI_TT_BASE);
    transform = (uint16_t)(transform & EMBI_TT_BASE);
    if (final_sigma) {
        m[0] = 0x3C2u; /* Final_Sigma: a capital sigma that ends a word lowers to the final form (SpecialCasing) */
    } else if (transform == EMBI_KW_capitalize) {
        if (word_start) {
            n = embi_lay_title_cp(cp, m);
        }
    } else if (transform != EMBI_KW_none) {
        n = embi_lay_map_cp(cp, transform, m);
    }
    for (int i = 0; i < n; i++) {
        uint32_t cpi = m[i], t[3];
        if ((extra & EMBI_TT_FSK) != 0 && transform != EMBI_KW_full_size_kana) {
            (void)embi_lay_map_cp(cpi, EMBI_KW_full_size_kana, t);
            cpi = t[0];
        }
        if ((extra & EMBI_TT_FW) != 0 && transform != EMBI_KW_full_width) {
            (void)embi_lay_map_cp(cpi, EMBI_KW_full_width, t);
            cpi = t[0];
        }
        embi_sb_append_cp(out, cpi);
    }
}

/* Appends a collapsed space: U+3000 under text-transform: full-width, which applies after the collapsing (CSS Text 3
 * section 2.1), so the space then hangs or not as an ideographic space does. */
static void put_space(embi_sb *out, uint16_t transform)
{
    if ((transform & EMBI_TT_BASE) == EMBI_KW_full_width || (transform & EMBI_TT_FW) != 0) {
        embi_sb_append_cp(out, 0x3000u);
    } else {
        embi_sb_append_char(out, ' ');
    }
}

/* Whether a capital sigma at p-1 (the character just decoded) ends a word: the text before is in a word (`word`) and the next
 * character is not a letter of one. */
static bool sigma_final(uint32_t cp, uint16_t transform, bool word, const char *p, const char *end)
{
    if (transform != EMBI_KW_lowercase || cp != 0x3A3u || !word) {
        return false;
    }
    if (p >= end) {
        return true;
    }
    return !embi_lay_word_cp(embi_utf8_decode(&p, end));
}

bool embi_lay_ws_ascii_unchanged(const uint8_t *p, uint32_t n, uint8_t mode, uint16_t transform,
                                 bool *prev_space, bool *in_word)
{
    if (mode != EMBI_WS_COLLAPSE || transform != EMBI_KW_none || n == 0) {
        return false;
    }
    bool prev = *prev_space;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = p[i];
        if (ch >= 0x80u || ch == 9 || ch == 10 || ch == 12 || ch == 13) {
            return false;
        }
        if (ch == ' ') {
            if (prev) {
                return false; /* the space would be removed */
            }
            prev = true;
        } else {
            prev = false;
        }
    }
    *prev_space = prev;
    *in_word = p[n - 1] != ' ' && embi_lay_word_cp(p[n - 1]);
    return true;
}

void embi_lay_ws_process(embi_sb *out, embi_str in, uint8_t mode, uint16_t transform,
                         bool *prev_space, bool *in_word)
{
    bool keep_spaces = mode == EMBI_WS_PRESERVE || mode == EMBI_WS_PRESERVE_SPACES ||
                       mode == EMBI_WS_BREAK_SPACES;
    bool keep_breaks = mode == EMBI_WS_PRESERVE || mode == EMBI_WS_PRESERVE_BREAKS ||
                       mode == EMBI_WS_BREAK_SPACES;
    bool discard = mode == EMBI_WS_DISCARD;
    const bool cjy = (transform & EMBI_TT_CJY) != 0;
    transform = (uint16_t)(transform & ~EMBI_TT_CJY);
    bool prev = *prev_space;
    bool word = *in_word; /* the previous character is inside a word */
    uint32_t last = 0; /* the previous non-space character of the input, 0 at the start */
    bool after_break = false; /* a segment break was handled and no other character came since */
    const char *p = in.ptr;
    const char *end = in.len ? in.ptr + in.len : in.ptr;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (cp == '\r') {
            cp = ' '; /* a carriage return is treated as a space in every respect (CSS Text 3 section 4.1.1); a form feed is a visible control character */
        }
        if (discard && ws_collapsible(cp)) {
            /* white-space-collapse: discard removes collapsible characters entirely. */
            continue;
        }
        if (keep_spaces) {
            if (is_segment_break(cp) || cp == '\t') {
                /* A preserved break stays a line feed (a forced break, W3c); a preserved tab stays a tab, which
                 * the line layout advances to the next tab stop (inline.c, tab-size) and the shaper draws as a
                 * space. */
                embi_sb_append_char(out, is_segment_break(cp) ? '\n' : '\t');
                prev = false;
                word = false;
            } else {
                emit_cp(out, cp, transform, !word, sigma_final(cp, transform, word, p, end));
                prev = false;
                word = embi_lay_word_cp(cp);
            }
            last = cp;
            continue;
        }
        if (ws_collapsible(cp)) {
            if (!is_segment_break(cp)) {
                /* A run of spaces and tabs. Next to a segment break it is removed (CSS Text 3
                 * section 4.1.1, phase I); elsewhere it collapses to one space. */
                const char *q = p;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) {
                    q++;
                }
                bool before_break = q < end && *q == '\n';
                p = q;
                if (after_break || before_break) {
                    continue;
                }
                if (!prev) {
                    put_space(out, transform);
                    prev = true;
                }
            } else {
                after_break = true;
                /* The first character after the break and the spaces, tabs and other segment breaks that follow it
                 * (they all collapse into this one). */
                const char *q = p;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || (!keep_breaks && *q == '\n'))) {
                    q++;
                }
                uint32_t next = 0;
                if (q < end) {
                    const char *q2 = q;
                    next = embi_utf8_decode(&q2, end);
                }
                if (keep_breaks) {
                    embi_sb_append_char(out, '\n');
                    prev = false;
                } else if (last == 0x200Bu || next == 0x200Bu ||
                           (break_removable_next_to(last) && next != 0 &&
                            break_removable_next_to(next)) ||
                           (cjy && next != 0 && cjy_break_removed(last, next))) {
                    /* removed between two wide characters; the collapsing state is unchanged */
                } else if (!prev) {
                    put_space(out, transform);
                    prev = true;
                }
            }
            word = false;
            continue;
        }
        after_break = false;
        emit_cp(out, cp, transform, !word, sigma_final(cp, transform, word, p, end));
        prev = false;
        word = embi_lay_word_cp(cp);
        last = cp;
    }
    *prev_space = prev;
    *in_word = word;
}
