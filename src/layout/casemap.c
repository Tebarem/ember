/*
 * text-transform mapping of one code point (CSS Text 3 section 2.1, W3a section 1). Uppercase and
 * lowercase use the Unicode default case mapping of the JS String methods: the same generated tables
 * (src/js/unicode_case.inc, UCD 17), so text-transform and toUpperCase agree. The lookup code below
 * is a copy of the one in builtins_string.c, because that file is part of the JS module and layout
 * does not depend on it. Full-width and full-size-kana are the fixed mappings of CSS Text 3.
 *
 * Limits. capitalize uses the uppercase mapping, not titlecase (U+01C6 gives U+01C4 rather than
 * U+01C5, and U+00DF gives "SS" rather than "Ss"); the J1f3 tables expose no titlecase mapping, so
 * this waits for them (W3h). The Final_Sigma rule (lowercase of a capital sigma at the end of a word) needs the
 * neighbouring characters, which the per-character mapping does not have, so U+03A3 always lowers
 * to U+03C3.
 *
 * Data dependency. The case tables are the header-only data file js/unicode_case.inc, so layout
 * has no link dependency on the js module (deps.txt); the lookup code is small. A shared home for
 * the tables (base or text) would remove the second copy from the library image; that needs
 * src/js to change, so it is not done.
 */
#include "layout_int.h"

#include "css/props.h"

#include "js/unicode_case.inc"

#define LAY_ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const embi_case_special *special_find(const embi_case_special *tab, size_t n, uint32_t cp)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tab[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < n && tab[lo].cp == cp ? &tab[lo] : nullptr;
}

/* The simple mapping of cp in the runs, when cp has one. */
static bool run_lookup(const embi_case_run *runs, size_t n, uint32_t cp, uint32_t *out)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (runs[mid].start <= cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const embi_case_run *r = &runs[lo - 1];
    uint32_t d = cp - r->start;
    if (d % r->stride != 0 || d / r->stride >= r->count) {
        return false;
    }
    *out = cp + (uint32_t)r->delta;
    return true;
}

/* The default case mapping of cp (SpecialCasing first, then the simple mapping). */
static int map_case(uint32_t cp, bool upper, uint32_t out[3])
{
    if (cp < 0x80u) {
        if (upper && cp >= 'a' && cp <= 'z') {
            out[0] = cp - 32u;
        } else if (!upper && cp >= 'A' && cp <= 'Z') {
            out[0] = cp + 32u;
        } else {
            out[0] = cp;
        }
        return 1;
    }
    const embi_case_special *sp =
        upper ? special_find(embi_case_upper_special, LAY_ARR_LEN(embi_case_upper_special), cp)
              : special_find(embi_case_lower_special, LAY_ARR_LEN(embi_case_lower_special), cp);
    if (sp != nullptr) {
        int n = 0;
        while (n < 3 && sp->out[n] != 0) {
            out[n] = sp->out[n];
            n++;
        }
        return n;
    }
    bool hit = upper ? run_lookup(embi_case_upper_runs, LAY_ARR_LEN(embi_case_upper_runs), cp, out)
                     : run_lookup(embi_case_lower_runs, LAY_ARR_LEN(embi_case_lower_runs), cp, out);
    out[0] = hit ? out[0] : cp;
    return 1;
}

/* full-size-kana: the small hiragana and katakana become their full-size forms (CSS Text 3). */
static uint32_t full_size_kana(uint32_t cp)
{
    switch (cp) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: /* small a i u e o */
    case 0x3063: /* small tsu */
    case 0x3083: case 0x3085: case 0x3087: /* small ya yu yo */
    case 0x308E: /* small wa */
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:
    case 0x30C3: /* small tsu (katakana) */
    case 0x30E3: case 0x30E5: case 0x30E7: /* small ya yu yo */
    case 0x30EE: /* small wa */
        return cp + 1;
    case 0x3095: return 0x304B; /* small ka, ke */
    case 0x3096: return 0x3051;
    case 0x30F5: return 0x30AB;
    case 0x30F6: return 0x30B1;
    case 0x31F0: return 0x30AF; /* katakana phonetic extensions */
    case 0x31F1: return 0x30B7;
    case 0x31F2: return 0x30B9;
    case 0x31F3: return 0x30C8;
    case 0x31F4: return 0x30CC;
    case 0x31F5: return 0x30CF;
    case 0x31F6: return 0x30D2;
    case 0x31F7: return 0x30D5;
    case 0x31F8: return 0x30D8;
    case 0x31F9: return 0x30DB;
    case 0x31FA: return 0x30E0;
    case 0x31FB: return 0x30E9;
    case 0x31FC: return 0x30EA;
    case 0x31FD: return 0x30EB;
    case 0x31FE: return 0x30EC;
    case 0x31FF: return 0x30ED;
    case 0xFF67: case 0xFF68: case 0xFF69: case 0xFF6A: case 0xFF6B: /* halfwidth small a..o */
        return cp + 10;
    case 0xFF6C: case 0xFF6D: case 0xFF6E: /* halfwidth small ya yu yo */
        return cp + 40;
    case 0xFF6F: /* halfwidth small tsu */
        return 0xFF82;
    default:
        return cp;
    }
}

/* The full-width forms of the Latin-1 symbols, the won sign and the halfwidth katakana (CSS Text 3
 * section 2.1, the compatibility <wide> and <narrow> mappings). */
static uint32_t full_width_other(uint32_t cp)
{
    static const uint16_t kata[] = {
        0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
        0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB,
        0x30AD, 0x30AF, 0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1,
        0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5,
        0x30D8, 0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9,
        0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C};
    switch (cp) {
    case 0xA2u:
        return 0xFFE0u;
    case 0xA3u:
        return 0xFFE1u;
    case 0xACu:
        return 0xFFE2u;
    case 0xAFu:
        return 0xFFE3u;
    case 0xA6u:
        return 0xFFE4u;
    case 0xA5u:
        return 0xFFE5u;
    case 0x20A9u:
        return 0xFFE6u;
    default:
        break;
    }
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) {
        return kata[cp - 0xFF61u];
    }
    /* The white parentheses, and the <narrow> forms: halfwidth Hangul to the compatibility jamo, and the halfwidth
     * symbols to their ordinary forms. */
    if (cp == 0x2985u || cp == 0x2986u) {
        return cp - 0x2985u + 0xFF5Fu;
    }
    if (cp == 0xFFA0u) {
        return 0x3164u;
    }
    if (cp >= 0xFFA1u && cp <= 0xFFBEu) {
        return cp - 0xFFA1u + 0x3131u;
    }
    static const uint16_t vowel_lo[4] = {0xFFC2u, 0xFFCAu, 0xFFD2u, 0xFFDAu}, vowel_to[4] = {0x314Fu, 0x3155u, 0x315Bu, 0x3161u};
    for (int i = 0; i < 4; i++) {
        if (cp >= vowel_lo[i] && cp <= vowel_lo[i] + (i == 3 ? 2u : 5u)) {
            return cp - vowel_lo[i] + vowel_to[i];
        }
    }
    static const uint16_t sym[7] = {0x2502u, 0x2190u, 0x2191u, 0x2192u, 0x2193u, 0x25A0u, 0x25CBu};
    if (cp >= 0xFFE8u && cp <= 0xFFEEu) {
        return sym[cp - 0xFFE8u];
    }
    return cp;
}

int embi_lay_map_cp(uint32_t cp, uint16_t transform, uint32_t out[3])
{
    out[0] = cp;
    switch (transform) {
    case EMBI_KW_uppercase:
        if ((cp >= 0x1F80u && cp <= 0x1FAFu) || cp == 0x1FB3u || cp == 0x1FBCu || cp == 0x1FC3u || cp == 0x1FCCu || cp == 0x1FF3u ||
            cp == 0x1FFCu) {
            /* A Greek letter with ypogegrammeni uppercases to the capital with prosgegrammeni, as the WPT references and
             * the simple mapping of UnicodeData.txt have it (not the capital and an iota of SpecialCasing.txt). */
            return embi_lay_title_cp(cp, out);
        }
        return map_case(cp, true, out);
    case EMBI_KW_lowercase:
        return map_case(cp, false, out);
    case EMBI_KW_full_width:
        if (cp == 0x20u) {
            out[0] = 0x3000u;
        } else if (cp >= 0x21u && cp <= 0x7Eu) {
            out[0] = cp + 0xFEE0u;
        } else {
            out[0] = full_width_other(cp);
        }
        return 1;
    case EMBI_KW_full_size_kana:
        out[0] = full_size_kana(cp);
        return 1;
    default:
        return 1;
    }
}

int embi_lay_title_cp(uint32_t cp, uint32_t out[3])
{
    if ((cp >= 0x1C4u && cp <= 0x1CCu) || (cp >= 0x1F1u && cp <= 0x1F3u)) {
        out[0] = cp >= 0x1F1u ? 0x1F2u : cp <= 0x1C6u ? 0x1C5u : cp <= 0x1C9u ? 0x1C8u : 0x1CBu;
        return 1;
    }
    /* The Greek letters with ypogegrammeni titlecase to the letter with prosgegrammeni (SpecialCasing.txt, title column),
     * not to the capital and a capital iota of the uppercase mapping. */
    if ((cp >= 0x1F80u && cp <= 0x1FAFu) || cp == 0x1FB3u || cp == 0x1FBCu || cp == 0x1FC3u || cp == 0x1FCCu ||
        cp == 0x1FF3u || cp == 0x1FFCu) {
        out[0] = cp <= 0x1FAFu ? (cp | 8u) : cp == 0x1FB3u ? 0x1FBCu : cp == 0x1FC3u ? 0x1FCCu : cp == 0x1FF3u ? 0x1FFCu : cp;
        return 1;
    }
    int n = map_case(cp, true, out);
    if (n > 1 && (cp == 0xDFu || (cp >= 0xFB00u && cp <= 0xFB06u) || (cp >= 0xFB13u && cp <= 0xFB17u) || cp == 0x587u)) {
        for (int i = 1; i < n; i++) {
            uint32_t lo[3];
            if (map_case(out[i], false, lo) == 1) {
                out[i] = lo[0];
            }
        }
    }
    return n;
}

bool embi_lay_word_cp(uint32_t cp)
{
    if (cp < 0x80u) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
               cp == '_' || cp == '\'';
    }
    if (cp == 0x2019u) { /* right single quotation mark: an apostrophe inside a word */
        return true;
    }
    if (cp <= 0xBFu) {
        /* Latin-1 punctuation and symbols, except the letters ª, µ and º. */
        return cp == 0xAAu || cp == 0xB5u || cp == 0xBAu || cp == 0xB7u; /* U+00B7 is MidLetter (UAX 29) */
    }
    if (cp == 0xD7u || cp == 0xF7u) {
        return false;
    }
    if ((cp >= 0x2000u && cp <= 0x206Fu) || (cp >= 0x3000u && cp <= 0x303Fu)) {
        return false; /* general punctuation and CJK symbols */
    }
    if ((cp >= 0xFF01u && cp <= 0xFF0Fu) || (cp >= 0xFF1Au && cp <= 0xFF20u) ||
        (cp >= 0xFF3Bu && cp <= 0xFF40u) || (cp >= 0xFF5Bu && cp <= 0xFF65u)) {
        return false; /* fullwidth punctuation */
    }
    return true;
}

static bool lang_is(embi_str lang, char a, char b)
{
    return lang.len >= 2 && (char)(lang.ptr[0] | 0x20) == a && (char)(lang.ptr[1] | 0x20) == b &&
           (lang.len == 2 || lang.ptr[2] == '-' || lang.ptr[2] == '_');
}

void embi_lay_case_tailor(embi_sb *sb, embi_str lang, uint16_t transform, bool word_before)
{
    bool nl = transform == EMBI_KW_capitalize && lang_is(lang, 'n', 'l');
    bool el = transform == EMBI_KW_uppercase && lang_is(lang, 'e', 'l');
    if ((!nl && !el) || sb->len == 0) {
        return;
    }
    size_t cap = sb->len;
    uint32_t *cp = embi_alloc(cap * sizeof *cp);
    size_t n = 0;
    {
        const char *p = sb->data, *end = sb->data + sb->len;
        while (p < end) {
            cp[n++] = embi_utf8_decode(&p, end);
        }
    }
    embi_sb out = {0};
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cp[i];
        bool prev_word = i == 0 ? word_before : embi_lay_word_cp(cp[i - 1]);
        if (nl) {
            if (c == 'j' && i > 0 && cp[i - 1] == 'I' && (i == 1 ? !word_before : !embi_lay_word_cp(cp[i - 2]))) {
                c = 'J'; /* IJ is one letter in Dutch: both are capitalised at the start of a word */
            }
            embi_sb_append_cp(&out, c);
            continue;
        }
        /* Greek: an accented capital loses its tonos. */
        uint32_t base = c;
        switch (c) {
        case 0x386u: base = 0x391u; break;
        case 0x388u: base = 0x395u; break;
        case 0x389u: base = 0x397u; break;
        case 0x38Au: base = 0x399u; break;
        case 0x38Cu: base = 0x39Fu; break;
        case 0x38Eu: base = 0x3A5u; break;
        case 0x38Fu: base = 0x3A9u; break;
        default: break;
        }
        if (c == 0x389u && !prev_word && (i + 1 == n || !embi_lay_word_cp(cp[i + 1]))) {
            base = c; /* the disjunctive eta keeps its tonos */
        }
        if ((c == 0x399u || c == 0x3A5u) && i + 2 < n + 0 && cp[i + 1] == 0x308u && cp[i + 2] == 0x301u) {
            base = c == 0x399u ? 0x3AAu : 0x3ABu; /* iota or upsilon with dialytika and tonos: just dialytika */
            embi_sb_append_cp(&out, base);
            i += 2;
            continue;
        }
        embi_sb_append_cp(&out, base);
        if (i + 1 < n && (cp[i + 1] == 0x399u || cp[i + 1] == 0x3A5u) &&
            (((c == 0x386u || c == 0x388u || c == 0x38Cu || c == 0x38Eu) ) || (c == 0x389u && cp[i + 1] == 0x3A5u))) {
            /* A diphthong with the tonos on its first vowel: the second one takes a dialytika instead. */
            if (!(i + 2 < n && cp[i + 2] == 0x308u)) {
                embi_sb_append_cp(&out, cp[i + 1] == 0x399u ? 0x3AAu : 0x3ABu);
                i++;
            }
        }
    }
    embi_free(cp, cap * sizeof *cp);
    embi_sb_free(sb);
    *sb = out;
}
