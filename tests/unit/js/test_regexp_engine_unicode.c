#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* RegExp engine on Unicode data: \p{...} escapes from the generated tables, simple case folding
 * for u and v, the v-flag set algebra, and code points made of surrogate pairs. Patterns are ASCII
 * source (non-ASCII characters are written as \u escapes); inputs are UTF-16 arrays written out by
 * hand. */

#define MAXCAPS 64

static uint16_t *pat16(const char *s, uint32_t *n)
{
    size_t len = strlen(s);
    uint16_t *u = malloc((len ? len : 1) * sizeof *u);
    for (size_t i = 0; i < len; i++) {
        u[i] = (uint8_t)s[i];
    }
    *n = (uint32_t)len;
    return u;
}

/* The flags string as a bitmask, or 0 when it is not a valid flags string. */
static uint32_t flags_of(const char *s)
{
    uint32_t n;
    uint16_t *u = pat16(s, &n);
    uint32_t f = 0;
    bool ok = embi_re_parse_flags(u, n, &f);
    free(u);
    return ok ? f : 0;
}

/* Compiles and searches the pattern over UTF-16 units. */
static embi_re_status run16(const char *pat, const char *flags, const uint16_t *in, uint32_t n,
                            uint32_t start, uint32_t *caps)
{
    uint32_t pn;
    uint16_t *pu = pat16(pat, &pn);
    embi_re_prog *p = embi_re_compile(pu, pn, flags_of(flags), nullptr);
    free(pu);
    if (!p) {
        return EMBI_RE_NO_MATCH;
    }
    embi_re_input inp = {in, n, true};
    embi_re_status st = embi_re_search(p, inp, start, caps, nullptr);
    embi_re_free(p);
    return st;
}

static bool hit16(const char *pat, const char *flags, const uint16_t *in, uint32_t n)
{
    uint32_t caps[MAXCAPS];
    return run16(pat, flags, in, n, 0, caps) == EMBI_RE_MATCH;
}

static bool span16(const char *pat, const char *flags, const uint16_t *in, uint32_t n, uint32_t lo,
                   uint32_t hi)
{
    uint32_t caps[MAXCAPS];
    return run16(pat, flags, in, n, 0, caps) == EMBI_RE_MATCH && caps[0] == lo && caps[1] == hi;
}

static bool compiles(const char *pat, const char *flags)
{
    uint32_t pn;
    uint16_t *pu = pat16(pat, &pn);
    embi_re_prog *p = embi_re_compile(pu, pn, flags_of(flags), nullptr);
    free(pu);
    if (p) {
        embi_re_free(p);
    }
    return p != nullptr;
}

static bool rejected(const char *pat, const char *flags)
{
    uint32_t pn;
    uint16_t *pu = pat16(pat, &pn);
    embi_re_error err = {0, nullptr};
    embi_re_prog *p = embi_re_compile(pu, pn, flags_of(flags), &err);
    free(pu);
    if (p) {
        embi_re_free(p);
        SDL_Log("    accepted, expected a syntax error: /%s/%s", pat, flags);
        return false;
    }
    return err.msg != nullptr;
}

EMB_TEST(property_escapes_u_mode)
{
    static const uint16_t upper_e_acute[] = {0x00C9};
    static const uint16_t ab_c[] = {'A', 'B', 'c'};
    static const uint16_t greek[] = {0x03B1, 0x03B2, 0x03B3};
    static const uint16_t hira[] = {0x3042};
    static const uint16_t grin[] = {0xD83D, 0xDE00};
    static const uint16_t e_acute[] = {0x00E9};
    static const uint16_t digit5[] = {'5'};
    static const uint16_t arabic3[] = {0x0663};
    static const uint16_t unassigned[] = {0x0378};
    static const uint16_t tilde[] = {'~'};
    static const uint16_t plus[] = {'+'};
    static const uint16_t ideo_space[] = {0x3000};

    EMB_CHECK(span16("\\p{Lu}", "u", upper_e_acute, 1, 0, 1));
    EMB_CHECK(!hit16("\\p{Lu}", "u", ab_c + 2, 1));
    EMB_CHECK(span16("\\P{Lu}+", "u", ab_c, 3, 2, 3));
    EMB_CHECK(span16("\\p{Script=Greek}+", "u", greek, 3, 0, 3));
    EMB_CHECK(span16("\\p{sc=Grek}+", "u", greek, 3, 0, 3));
    EMB_CHECK(span16("\\p{scx=Hira}", "u", hira, 1, 0, 1));
    EMB_CHECK(span16("\\p{Emoji_Presentation}", "u", grin, 2, 0, 2));
    EMB_CHECK(span16("\\p{ID_Start}", "u", e_acute, 1, 0, 1));
    EMB_CHECK(!hit16("\\p{ID_Start}", "u", digit5, 1));
    EMB_CHECK(span16("\\p{General_Category=Decimal_Number}", "u", digit5, 1, 0, 1));
    EMB_CHECK(span16("\\p{gc=Nd}", "u", arabic3, 1, 0, 1));
    EMB_CHECK(!hit16("\\p{Assigned}", "u", unassigned, 1));
    EMB_CHECK(span16("\\p{ASCII}", "u", tilde, 1, 0, 1));
    EMB_CHECK(!hit16("\\p{ASCII}", "u", upper_e_acute, 1));
    EMB_CHECK(span16("\\p{Any}", "u", grin, 2, 0, 2));
    EMB_CHECK(span16("\\p{Lowercase}", "u", e_acute, 1, 0, 1));
    EMB_CHECK(span16("\\p{Math}", "u", plus, 1, 0, 1));
    EMB_CHECK(span16("\\p{White_Space}", "u", ideo_space, 1, 0, 1));
}

EMB_TEST(property_escape_syntax)
{
    EMB_CHECK(compiles("\\p{Lu}", "u"));
    EMB_CHECK(compiles("\\p{Lu}", "v"));
    EMB_CHECK(compiles("\\p{Letter}", "u"));
    EMB_CHECK(rejected("\\p{Lu", "u"));
    EMB_CHECK(rejected("\\p{}", "u"));
    EMB_CHECK(rejected("\\p{script=Latn}", "u"));
    EMB_CHECK(rejected("\\p{General_Category}", "u"));
    EMB_CHECK(rejected("\\p{Alpha=Y}", "u"));
    EMB_CHECK(rejected("\\p{Emoji_Presentation=Yes}", "u"));
    /* Properties of strings are allowed in v mode (RGI_Emoji and friends), but not under the P escape or a negated class. */
    EMB_CHECK(compiles("\\p{RGI_Emoji}", "v"));
    EMB_CHECK(rejected("\\P{RGI_Emoji}", "v"));
    EMB_CHECK(rejected("[^\\p{RGI_Emoji}]", "v"));
    /* Without u or v, \p is an identity escape. */
    EMB_CHECK(compiles("\\p{Lu}", ""));
}

EMB_TEST(case_folding_u_and_non_u)
{
    static const uint16_t kelvin_in[] = {'k'};
    static const uint16_t long_s[] = {0x017F};
    static const uint16_t sharp_s[] = {0x00DF};
    static const uint16_t capital_sharp[] = {0x1E9E};
    static const uint16_t e_upper[] = {0x00C9};
    static const uint16_t e_lower[] = {0x00E9};
    static const uint16_t lower_a[] = {'a'};
    static const uint16_t upper_a[] = {'A'};

    /* Simple folding: KELVIN SIGN folds to k, so it matches k with the i flag in u mode only. */
    EMB_CHECK(hit16("\\u212A", "iu", kelvin_in, 1));
    EMB_CHECK(!hit16("\\u212A", "i", kelvin_in, 1));
    /* LATIN SMALL LETTER LONG S folds to s; without u it does not canonicalize to S. */
    EMB_CHECK(hit16("s", "iu", long_s, 1));
    EMB_CHECK(!hit16("s", "i", long_s, 1));
    /* \w includes U+017F under u with i, and not without u. */
    EMB_CHECK(hit16("\\w", "iu", long_s, 1));
    EMB_CHECK(!hit16("\\w", "i", long_s, 1));
    EMB_CHECK(!hit16("\\W", "iu", long_s, 1));
    /* CAPITAL SHARP S folds to sharp s in u mode; the non-u rule keeps it unchanged. */
    EMB_CHECK(hit16("\\u1E9E", "iu", sharp_s, 1));
    EMB_CHECK(!hit16("\\u1E9E", "i", sharp_s, 1));
    EMB_CHECK(hit16("\\u00DF", "iu", capital_sharp, 1));
    /* Non-ASCII letters in a class, in both modes. */
    EMB_CHECK(hit16("[\\u00e9]", "i", e_upper, 1));
    EMB_CHECK(hit16("[\\u00e9]", "iu", e_upper, 1));
    EMB_CHECK(hit16("\\u00c9", "i", e_lower, 1));
    /* \p{Lu} with i matches the lowercase letters too. */
    EMB_CHECK(hit16("\\p{Lu}", "iu", lower_a, 1));
    /* A negated class is not the complement of its case closure. */
    EMB_CHECK(!hit16("[^a]", "i", upper_a, 1));
    EMB_CHECK(hit16("[^a]", "", upper_a, 1));
}

EMB_TEST(backreference_case_folding_unicode)
{
    static const uint16_t pair[] = {0x00E9, 0x00C9};
    static const uint16_t pair_no[] = {0x00E9, 0x00E8};
    EMB_CHECK(span16("(\\u00e9)\\1", "iu", pair, 2, 0, 2));
    EMB_CHECK(span16("(\\u00e9)\\1", "i", pair, 2, 0, 2));
    EMB_CHECK(!hit16("(\\u00e9)\\1", "i", pair_no, 2));
}

EMB_TEST(v_flag_set_operations)
{
    static const uint16_t a_[] = {'a'};
    static const uint16_t b_[] = {'b'};
    static const uint16_t e_[] = {'e'};
    static const uint16_t B_[] = {'B'};
    static const uint16_t one[] = {'1'};
    static const uint16_t abc[] = {'a', 'b', 'c'};
    static const uint16_t ab[] = {'a', 'b'};
    static const uint16_t d_[] = {'d'};
    static const uint16_t ae[] = {'a', 'e'};
    static const uint16_t c_[] = {'c'};

    /* Subtraction: letters except a-z. */
    EMB_CHECK(!hit16("[\\p{L}--[a-z]]", "v", a_, 1));
    EMB_CHECK(hit16("[\\p{L}--[a-z]]", "v", B_, 1));
    /* Intersection: vowels among a-z. */
    EMB_CHECK(hit16("[[a-z]&&[aeiou]]", "v", e_, 1));
    EMB_CHECK(!hit16("[[a-z]&&[aeiou]]", "v", b_, 1));
    /* Class strings: the longest string wins. */
    EMB_CHECK(span16("[\\q{abc|d}]", "v", abc, 3, 0, 3));
    EMB_CHECK(!hit16("[\\q{abc|d}]", "v", ab, 2));
    EMB_CHECK(span16("[\\q{abc|d}]", "v", d_, 1, 0, 1));
    EMB_CHECK(span16("[\\q{ab}a]", "v", ae, 2, 0, 1));
    EMB_CHECK(span16("[\\q{ab}a]", "v", ab, 2, 0, 2));
    /* The empty string is an alternative tried after the single characters. */
    EMB_CHECK(span16("[\\q{}a]", "v", a_, 1, 0, 1));
    /* Word characters without digits. */
    EMB_CHECK(span16("^[\\w--\\d]+$", "v", ab, 2, 0, 2));
    EMB_CHECK(!hit16("^[\\w--\\d]+$", "v", one, 1));
    /* Subtraction of a range. */
    /* A range cannot be a subtraction operand, so the bare form is a SyntaxError. */
    EMB_CHECK(rejected("[a-z--aeiou]", "v"));
    EMB_CHECK(rejected("[a-z--[aeiou]]", "v"));
    EMB_CHECK(hit16("[[a-z]--[aeiou]]", "v", b_, 1));
    EMB_CHECK(!hit16("[[a-z]--[aeiou]]", "v", e_, 1));
    /* Under v+i the members are folded; the complement ranges over characters that fold to
     * themselves, so it cannot match an uppercase letter whose fold is excluded. */
    EMB_CHECK(hit16("[a]", "vi", B_, 1) == false);
    EMB_CHECK(hit16("[a]", "vi", a_, 1));
    EMB_CHECK(hit16("[^a]", "vi", B_, 1));
    EMB_CHECK(!hit16("[^a]", "vi", a_, 1));
    EMB_CHECK(hit16("[^a]", "v", B_, 1));
    /* Nested classes. */
    EMB_CHECK(hit16("[[ab][cd]]", "v", c_, 1));
    EMB_CHECK(!hit16("[^[ab]]", "v", a_, 1));
}

EMB_TEST(astral_and_surrogates)
{
    static const uint16_t grin[] = {0xD83D, 0xDE00};
    static const uint16_t lone_hi[] = {0xD83D};
    static const uint16_t after_grin[] = {0xD83D, 0xDE00, 'a'};
    static const uint16_t wide_ok[] = {0x4E2D, 0x6587};
    uint32_t caps[MAXCAPS];

    EMB_CHECK(span16("^\\u{1F600}$", "u", grin, 2, 0, 2));
    EMB_CHECK(span16("^[\\u{1F600}-\\u{1F64F}]$", "u", grin, 2, 0, 2));
    /* A lone surrogate matches itself in both modes. */
    EMB_CHECK(span16("^\\uD83D$", "u", lone_hi, 1, 0, 1));
    EMB_CHECK(span16("^\\uD83D$", "", lone_hi, 1, 0, 1));
    /* A lookbehind in u mode reads the pair as one code point. */
    EMB_CHECK(span16("(?<=\\u{1F600})a", "u", after_grin, 3, 2, 3));
    EMB_CHECK(!hit16("(?<=\\uDE00)a", "u", after_grin, 3));
    /* CJK ideographs are in the BMP range tables. */
    EMB_CHECK(span16("[\\u4e00-\\u9fff]+", "u", wide_ok, 2, 0, 2));
    /* Negated ranges in non-u mode skip the surrogates and find the letter. */
    EMB_CHECK(span16("[^\\u0100-\\uFFFF]", "", after_grin, 3, 2, 3));
    /* A u-mode search does not start inside a pair. */
    EMB_CHECK(run16("\\uDE00", "u", grin, 2, 0, caps) == EMBI_RE_NO_MATCH);
    /* Named groups may use \u escapes in their names. */
    uint32_t pn;
    uint16_t *pu = pat16("(?<\\u{e9}t\\u0065>x)", &pn);
    embi_re_prog *p = embi_re_compile(pu, pn, EMBI_RE_FLAG_U, nullptr);
    free(pu);
    EMB_CHECK(p != nullptr);
    if (p) {
        uint32_t len = 0;
        const uint16_t *name = embi_re_group_name(p, 1, &len);
        EMB_CHECK(name != nullptr && len == 3 && name[0] == 0xE9 && name[1] == 't' && name[2] == 'e');
        embi_re_free(p);
    }
}

EMB_TEST(unicode_flag_errors)
{
    EMB_CHECK(rejected("\\u{110000}", "u"));
    EMB_CHECK(rejected("\\u{}", "u"));
    EMB_CHECK(rejected("\\uD83", "u"));
    EMB_CHECK(rejected("(?<\\u0030a>x)", "u"));
    EMB_CHECK(rejected("[a-\\d]", "u"));
    EMB_CHECK(rejected("[z-a]", "v"));
    EMB_CHECK(rejected("\\P{RGI_Emoji}", "v"));
    EMB_CHECK(rejected("[^\\p{RGI_Emoji}]", "v"));
    EMB_CHECK(rejected("[\\q{ab]", "v"));
    EMB_CHECK(rejected("[\\q{a|b}", "v"));
    EMB_CHECK(rejected("\\q{ab}", "v"));
    EMB_CHECK(rejected("[a\\-b]\\-", "u"));
}

EMB_TEST_MAIN()
