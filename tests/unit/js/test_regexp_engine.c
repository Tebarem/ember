#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RegExp engine through its C API: syntax errors, matching semantics from ECMA-262 examples, the
 * Annex B grammar, lookarounds, named groups, modifiers, and the resource limits. */

#define MAXCAPS 64

static uint16_t *units16(const char *s, uint32_t *n)
{
    size_t len = strlen(s);
    uint16_t *u = calloc(len ? len : 1, sizeof *u);
    for (size_t i = 0; i < len; i++) {
        u[i] = (uint8_t)s[i];
    }
    *n = (uint32_t)len;
    return u;
}

static uint32_t flags_of(const char *s)
{
    uint32_t n;
    uint16_t *u = units16(s, &n);
    uint32_t f = 0;
    bool ok = embi_re_parse_flags(u, n, &f);
    free(u);
    return ok ? f : 0xFFFFFFFFu;
}

static embi_re_prog *compile_str(const char *pat, const char *flags, embi_re_error *err)
{
    uint32_t n;
    uint16_t *u = units16(pat, &n);
    embi_re_prog *p = embi_re_compile(u, n, flags_of(flags), err);
    free(u);
    return p;
}

/* Searches 'input' (ASCII) with the pattern. wide selects UTF-16 units instead of Latin-1 bytes.
 * Returns the status; caps receives the captures on a match. */
static embi_re_status exec_str(const char *pat, const char *flags, const char *input,
                               uint32_t start, bool sticky, bool wide, uint32_t *caps,
                               const embi_re_limits *lim, bool *compiled)
{
    embi_re_prog *p = compile_str(pat, flags, nullptr);
    if (compiled) {
        *compiled = p != nullptr;
    }
    if (!p) {
        return EMBI_RE_NO_MATCH;
    }
    uint32_t n = (uint32_t)strlen(input);
    embi_re_input in;
    uint16_t *w = nullptr;
    uint8_t *b = malloc(n ? n : 1);
    for (uint32_t i = 0; i < n; i++) {
        b[i] = (uint8_t)input[i];
    }
    if (wide) {
        w = units16(input, &n);
        in.units = w;
        in.wide = true;
    } else {
        in.units = b;
        in.wide = false;
    }
    in.len = n;
    embi_re_status st = sticky ? embi_re_match_at(p, in, start, caps, lim)
                               : embi_re_search(p, in, start, caps, lim);
    free(w);
    free(b);
    embi_re_free(p);
    return st;
}

/* True when the pattern matches the input, in both encodings. */
static bool matches(const char *pat, const char *flags, const char *input, uint32_t *caps)
{
    uint32_t local[MAXCAPS];
    uint32_t *c = caps ? caps : local;
    bool a = exec_str(pat, flags, input, 0, false, false, c, nullptr, nullptr) == EMBI_RE_MATCH;
    bool b = exec_str(pat, flags, input, 0, false, true, c, nullptr, nullptr) == EMBI_RE_MATCH;
    if (a != b) {
        SDL_Log("    latin1/utf16 disagree: /%s/%s on \"%s\"", pat, flags, input);
    }
    return a && b;
}

/* Checks that group g of the captures is the expected ASCII substring (nullptr: undefined). */
static bool group_is(const char *input, const uint32_t *caps, uint32_t g, const char *expected)
{
    if (caps[2 * g] == EMBI_RE_UNDEF || caps[2 * g + 1] == EMBI_RE_UNDEF) {
        return expected == nullptr;
    }
    if (!expected) {
        return false;
    }
    size_t len = caps[2 * g + 1] - caps[2 * g];
    return strlen(expected) == len && strncmp(input + caps[2 * g], expected, len) == 0;
}

static bool span_is(const uint32_t *caps, uint32_t g, uint32_t lo, uint32_t hi)
{
    return caps[2 * g] == lo && caps[2 * g + 1] == hi;
}

/* Compiles and reports whether the compile was rejected with a message. */
static bool rejected(const char *pat, const char *flags)
{
    embi_re_error err = {0, nullptr};
    embi_re_prog *p = compile_str(pat, flags, &err);
    if (p) {
        embi_re_free(p);
        SDL_Log("    accepted, expected a syntax error: /%s/%s", pat, flags);
        return false;
    }
    if (err.msg == nullptr) {
        SDL_Log("    no message for /%s/%s", pat, flags);
        return false;
    }
    return true;
}

static bool accepted(const char *pat, const char *flags)
{
    embi_re_error err = {0, nullptr};
    embi_re_prog *p = compile_str(pat, flags, &err);
    if (!p) {
        SDL_Log("    rejected: /%s/%s: %s at %u", pat, flags, err.msg ? err.msg : "?", err.pos);
        return false;
    }
    embi_re_free(p);
    return true;
}

EMB_TEST(literals_alternation_anchors)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("abc", "", "xxabcx", caps) && span_is(caps, 0, 2, 5));
    EMB_CHECK(matches("a|bc", "", "xbc", caps) && span_is(caps, 0, 1, 3));
    EMB_CHECK(matches("a|ab", "", "ab", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("^a$", "", "a", caps));
    EMB_CHECK(!matches("^a$", "", "ab", nullptr));
    EMB_CHECK(matches("a$", "m", "a\nb", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("^b", "m", "a\nb", caps) && span_is(caps, 0, 2, 3));
    EMB_CHECK(!matches("^b", "", "a\nb", nullptr));
    EMB_CHECK(!matches("a.b", "", "a\nb", nullptr));
    EMB_CHECK(matches("a.b", "s", "a\nb", caps) && span_is(caps, 0, 0, 3));
    EMB_CHECK(matches("A", "i", "xa", caps) && span_is(caps, 0, 1, 2));
    EMB_CHECK(matches("[a-c]+", "i", "xABCx", caps) && span_is(caps, 0, 1, 4));
}

EMB_TEST(escapes_and_classes)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("\\d+", "", "ab123c", caps) && span_is(caps, 0, 2, 5));
    EMB_CHECK(matches("\\s", "", "a b", caps) && span_is(caps, 0, 1, 2));
    EMB_CHECK(matches("\\W+", "", "ab-- c", caps) && span_is(caps, 0, 2, 5));
    EMB_CHECK(matches("\\bfoo\\b", "", "a foo b", caps) && span_is(caps, 0, 2, 5));
    EMB_CHECK(!matches("\\bfoo\\b", "", "afoo", nullptr));
    EMB_CHECK(matches("\\Boo", "", "foo", caps) && span_is(caps, 0, 1, 3));
    EMB_CHECK(matches("[\\d-]+", "", "a1-2", caps) && span_is(caps, 0, 1, 4));
    EMB_CHECK(matches("\\x41", "", "A", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\u0041", "", "A", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\cJ", "", "\n", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("[\\b]", "", "\b", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("[^\\s\\d]+", "", "12 ab", caps) && span_is(caps, 0, 3, 5));
    EMB_CHECK(matches("\\t\\n", "", "\t\n", caps) && span_is(caps, 0, 0, 2));
}

EMB_TEST(quantifiers)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("a{2,3}", "", "aaaa", caps) && span_is(caps, 0, 0, 3));
    EMB_CHECK(matches("a{2,3}?", "", "aaaa", caps) && span_is(caps, 0, 0, 2));
    EMB_CHECK(!matches("a{2}", "", "a", nullptr));
    EMB_CHECK(matches("a*?b", "", "aab", caps) && span_is(caps, 0, 0, 3));
    EMB_CHECK(matches("(a|b)*c", "", "ababc", caps) && span_is(caps, 0, 0, 5) &&
              group_is("ababc", caps, 1, "b"));
    EMB_CHECK(matches("x{0}", "", "x", caps) && span_is(caps, 0, 0, 0));
    EMB_CHECK(matches("a{,2}", "", "a{,2}", caps) && span_is(caps, 0, 0, 5));
    EMB_CHECK(matches("a{1,}", "", "baaa", caps) && span_is(caps, 0, 1, 4));
    EMB_CHECK(matches("(?:ab){2}", "", "xababab", caps) && span_is(caps, 0, 1, 5));
    EMB_CHECK(matches("(a)?b", "", "b", caps) && group_is("b", caps, 1, nullptr));
}

EMB_TEST(empty_iteration_rule)
{
    uint32_t caps[MAXCAPS];
    /* ECMA-262: the pattern (a*)* on "b" matches empty with group 1 undefined (the empty iteration fails). */
    EMB_CHECK(matches("(a*)*", "", "b", caps) && span_is(caps, 0, 0, 0) &&
              group_is("b", caps, 1, nullptr));
    /* The pattern (a*)+ on "b": the mandatory iteration may be empty, so group 1 is "". */
    EMB_CHECK(matches("(a*)+", "", "b", caps) && span_is(caps, 0, 0, 0) &&
              group_is("b", caps, 1, ""));
    EMB_CHECK(matches("(?:a?)*b", "", "aab", caps) && span_is(caps, 0, 0, 3));
}

EMB_TEST(captures_spec_examples)
{
    uint32_t caps[MAXCAPS];
    const char *s1 = "abcd";
    EMB_CHECK(matches("(a|ab)(c|bcd)(d*)", "", s1, caps) && span_is(caps, 0, 0, 4) &&
              group_is(s1, caps, 1, "a") && group_is(s1, caps, 2, "bcd") &&
              group_is(s1, caps, 3, ""));
    /* ECMA-262: captures inside a quantified group are reset on each iteration. */
    const char *s2 = "zaacbbbcac";
    EMB_CHECK(matches("(z)((a+)?(b+)?(c))*", "", s2, caps) && span_is(caps, 0, 0, 10) &&
              group_is(s2, caps, 1, "z") && group_is(s2, caps, 2, "ac") &&
              group_is(s2, caps, 3, "a") && group_is(s2, caps, 4, nullptr) &&
              group_is(s2, caps, 5, "c"));
}

EMB_TEST(backreferences)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("(a)\\1", "", "xaa", caps) && span_is(caps, 0, 1, 3));
    /* Forward reference: matches empty before the group is set. */
    EMB_CHECK(matches("\\1(a)", "", "aa", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("(?<n>x)\\k<n>", "", "xx", caps) && span_is(caps, 0, 0, 2));
    EMB_CHECK(matches("(a)\\1", "i", "aA", caps) && span_is(caps, 0, 0, 2));
    EMB_CHECK(matches("(\\w)\\1", "", "abccd", caps) && span_is(caps, 0, 2, 4));
    EMB_CHECK(!matches("(a)\\1", "", "ab", nullptr));
}

EMB_TEST(lookarounds)
{
    uint32_t caps[MAXCAPS];
    const char *s1 = "$42";
    EMB_CHECK(matches("(?<=\\$)\\d+", "", s1, caps) && span_is(caps, 0, 1, 3));
    EMB_CHECK(matches("(?<!a)b", "", "abcb", caps) && span_is(caps, 0, 3, 4));
    EMB_CHECK(matches("a(?=b)", "", "ab", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(!matches("a(?=b)", "", "ac", nullptr));
    EMB_CHECK(matches("a(?!b)", "", "ac", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(!matches("a(?!b)", "", "ab", nullptr));
    /* ECMA-262: lookbehind matches right to left, so the second group takes the longest tail. */
    const char *s2 = "1053";
    EMB_CHECK(matches("(?<=(\\d+)(\\d+))$", "", s2, caps) && span_is(caps, 0, 4, 4) &&
              group_is(s2, caps, 1, "1") && group_is(s2, caps, 2, "053"));
    /* Lookahead captures persist; negative ones are undefined. */
    EMB_CHECK(matches("(?=(a+))a", "", "aaa", caps) && span_is(caps, 0, 0, 1) &&
              group_is("aaa", caps, 1, "aaa"));
    EMB_CHECK(matches("(?!(a))b", "", "b", caps) && group_is("b", caps, 1, nullptr));
    /* Lookbehind with a backreference. */
    EMB_CHECK(matches("(a)(?<=\\1)", "", "aa", caps) && span_is(caps, 0, 0, 1));
}

EMB_TEST(named_groups)
{
    uint32_t caps[MAXCAPS];
    const char *s = "2024-11";
    EMB_CHECK(matches("(?<y>\\d{4})-(?<m>\\d\\d)", "", s, caps) && group_is(s, caps, 1, "2024") &&
              group_is(s, caps, 2, "11"));
    embi_re_prog *p = compile_str("(?<y>\\d{4})-(?<m>\\d\\d)", "", nullptr);
    EMB_CHECK(p != nullptr);
    if (p) {
        uint32_t len = 0;
        const uint16_t *name = embi_re_group_name(p, 1, &len);
        EMB_CHECK(name != nullptr && len == 1 && name[0] == 'y');
        EMB_CHECK(embi_re_group_name(p, 0, &len) == nullptr);
        EMB_CHECK(embi_re_group_count(p) == 2);
        EMB_CHECK(embi_re_capture_slots(p) == 6);
        embi_re_free(p);
    }
    /* Duplicate names are allowed in different alternatives (ES2025). */
    EMB_CHECK(matches("(?<a>x)|(?<a>y)", "", "y", caps) && group_is("y", caps, 1, nullptr) &&
              group_is("y", caps, 2, "y"));
    EMB_CHECK(matches("(?<a>x)|(?<a>y)", "", "x", caps) && group_is("x", caps, 1, "x"));
    EMB_CHECK(matches("(?:(?<a>x)|(?<a>y))\\k<a>", "", "yy", caps) && span_is(caps, 0, 0, 2));
    EMB_CHECK(rejected("(?<a>x)(?<a>y)", ""));
    EMB_CHECK(rejected("(?<a>x)\\k<b>", ""));
    EMB_CHECK(rejected("(?<1a>x)", ""));
    EMB_CHECK(rejected("\\k<a>(?<b>x)", ""));
    /* Without named groups in a non-u pattern, \k is an identity escape. */
    EMB_CHECK(matches("\\k", "", "k", caps) && span_is(caps, 0, 0, 1));
}

EMB_TEST(modifiers_and_flags)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("a(?i:b)c", "", "aBc", caps) && span_is(caps, 0, 0, 3));
    EMB_CHECK(!matches("a(?i:b)c", "", "aBC", nullptr));
    EMB_CHECK(!matches("(?-i:a)", "i", "A", nullptr));
    EMB_CHECK(matches("(?s:.)", "", "\n", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("(?m:^)b", "", "a\nb", caps) && span_is(caps, 0, 2, 3));
    EMB_CHECK(rejected("(?i-i:a)", ""));
    EMB_CHECK(rejected("(?-:a)", ""));
    EMB_CHECK(rejected("(?ii:a)", ""));
    EMB_CHECK(rejected("(?x:a)", ""));
    EMB_CHECK(accepted("(?i-:a)", ""));
    EMB_CHECK(flags_of("gimsuy") != 0xFFFFFFFFu);
    EMB_CHECK(flags_of("uv") == 0xFFFFFFFFu);
    EMB_CHECK(flags_of("gg") == 0xFFFFFFFFu);
    EMB_CHECK(flags_of("x") == 0xFFFFFFFFu);
}

EMB_TEST(sticky_and_search_positions)
{
    uint32_t caps[MAXCAPS];
    embi_re_input in;
    const char *s = "ab";
    in.units = s;
    in.len = 2;
    in.wide = false;
    embi_re_prog *p = compile_str("b", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_re_match_at(p, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(p, in, 1, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(caps[0] == 1 && caps[1] == 2);
    EMB_CHECK(embi_re_search(p, in, 0, caps, nullptr) == EMBI_RE_MATCH && caps[0] == 1);
    embi_re_free(p);
}

EMB_TEST(unicode_code_points_in_u_mode)
{
    /* U+1F600 as a surrogate pair: one code point in u mode, two code units otherwise. */
    const uint16_t s[] = {0xD83D, 0xDE00};
    embi_re_input in = {s, 2, true};
    uint32_t caps[MAXCAPS];
    embi_re_prog *pu = compile_str("^.$", "u", nullptr);
    embi_re_prog *pn = compile_str("^.$", "", nullptr);
    EMB_REQUIRE(pu != nullptr && pn != nullptr);
    EMB_CHECK(embi_re_search(pu, in, 0, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(embi_re_search(pn, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    /* Unanchored u-mode search steps over whole code points. */
    embi_re_prog *pd = compile_str("\\uDE00", "u", nullptr);
    EMB_REQUIRE(pd != nullptr);
    EMB_CHECK(embi_re_search(pd, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    embi_re_free(pd);
    embi_re_free(pu);
    embi_re_free(pn);
}

EMB_TEST(syntax_errors)
{
    EMB_CHECK(rejected("(", ""));
    EMB_CHECK(rejected(")", ""));
    EMB_CHECK(rejected("a**", ""));
    EMB_CHECK(rejected("[b-a]", ""));
    EMB_CHECK(rejected("a{2,1}", ""));
    EMB_CHECK(rejected("*", ""));
    EMB_CHECK(rejected("a\\", ""));
    EMB_CHECK(rejected("[a", ""));
    EMB_CHECK(rejected("(?", ""));
    EMB_CHECK(rejected("(?<=a)*", ""));
    EMB_CHECK(rejected("{1}", ""));
    EMB_CHECK(rejected("a{1}{2}", ""));
    EMB_CHECK(rejected("(?=a)*", "u"));
    EMB_CHECK(rejected("\\1", "u"));
    EMB_CHECK(rejected("\\a", "u"));
    EMB_CHECK(rejected("a{", "u"));
    EMB_CHECK(rejected("}", "u"));
    EMB_CHECK(rejected("]", "u"));
    EMB_CHECK(rejected("\\u{110000}", "u"));
    EMB_CHECK(accepted("[\\-a]", "u"));
    EMB_CHECK(rejected("[\\d-a]", "u"));
    EMB_CHECK(accepted("\\cA", "u"));
    EMB_CHECK(rejected("(?<a>x)(?<a>y)", "u"));
    EMB_CHECK(rejected("x", "uv"));
    EMB_CHECK(rejected("\\p{Foo}", "u"));
    EMB_CHECK(rejected("\\p{Script}", "u"));
    EMB_CHECK(accepted("\\p{RGI_Emoji}", "v"));
    EMB_CHECK(rejected("[a&&&b]", "v"));
    EMB_CHECK(rejected("[(]", "v"));
    EMB_CHECK(rejected("[a--]", "v"));
    EMB_CHECK(rejected("[^\\q{ab}]", "v"));
    EMB_CHECK(rejected("[a&&b--c]", "v"));
    EMB_CHECK(rejected("\\k<a>", "u"));
    EMB_CHECK(rejected("[\\B]", "u"));
    EMB_CHECK(accepted("(?<\\u0061>x)", "u"));
    EMB_CHECK(rejected("(?<\\u0030>x)", "u"));
}

EMB_TEST(annex_b_grammar)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(accepted("a{", ""));
    EMB_CHECK(accepted("]", ""));
    EMB_CHECK(accepted("}", ""));
    EMB_CHECK(accepted("{", ""));
    EMB_CHECK(accepted("x{,2}", ""));
    EMB_CHECK(accepted("(?=a)*", ""));
    EMB_CHECK(accepted("\\8", ""));
    EMB_CHECK(accepted("[\\d-a]", ""));
    EMB_CHECK(accepted("\\u{1}", ""));
    EMB_CHECK(matches("\\8", "", "8", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\1", "", "\x01", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("a{", "", "a{", caps) && span_is(caps, 0, 0, 2));
    EMB_CHECK(matches("]", "", "]", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\c1", "", "\\c1", caps) && span_is(caps, 0, 0, 3));
    EMB_CHECK(matches("[\\c1]", "", "\x11", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(accepted("\\0", ""));
    EMB_CHECK(matches("\\101", "", "A", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\u{1}", "", "u", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("[\\d-a]", "", "-", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\p{L}", "", "p{L}", caps) && span_is(caps, 0, 0, 4));
    EMB_CHECK(matches("(a)|\\2(b)", "", "b", caps) && span_is(caps, 0, 0, 1) && group_is("b", caps, 2, "b"));
    EMB_CHECK(matches("\\q", "", "q", caps) && span_is(caps, 0, 0, 1));
}

static bool stop_after_poll(void *ctx)
{
    uint32_t *count = ctx;
    *count += 1;
    return true;
}

EMB_TEST(interrupt_hook_stops_match)
{
    static uint32_t polls;
    polls = 0;
    embi_re_limits lim = {0, nullptr, nullptr};
    /* Catastrophic (a+)+b on a line of a's. The hook is polled and stops the match. */
    uint32_t n = 40;
    char *s = malloc(n + 1);
    memset(s, 'a', n);
    s[n] = '\0';
    uint32_t caps[MAXCAPS];
    embi_re_prog *p = compile_str("(a+)+b", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    uint16_t *u = units16(s, &n);
    embi_re_input in = {u, n, true};
    lim.interrupt = stop_after_poll;
    lim.ctx = &polls;
    embi_re_status st = embi_re_search(p, in, 0, caps, &lim);
    EMB_CHECK(st == EMBI_RE_INTERRUPTED);
    EMB_CHECK(polls >= 1);
    free(u);
    free(s);
    embi_re_free(p);
}

EMB_TEST(backtrack_memory_cap)
{
    /* Each iteration of an alternation loop keeps backtrack records; a small cap is reached. */
    uint32_t n = 200000;
    uint8_t *b = malloc(n);
    memset(b, 'a', n);
    embi_re_input in = {b, n, false};
    embi_re_prog *p = compile_str("(?:a|b)*c", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    uint32_t caps[MAXCAPS];
    embi_re_limits lim = {1u << 20, nullptr, nullptr};
    EMB_CHECK(embi_re_search(p, in, 0, caps, &lim) == EMBI_RE_TOO_COMPLEX);
    embi_re_free(p);
    free(b);
}

EMB_TEST(simple_repeat_uses_no_stack_per_char)
{
    /* A simple greedy repeat keeps one record regardless of length. */
    uint32_t n = 300000;
    uint8_t *b = malloc(n);
    memset(b, 'a', n);
    embi_re_input in = {b, n, false};
    embi_re_prog *p = compile_str("a*b", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    uint32_t caps[MAXCAPS];
    embi_re_limits lim = {1u << 16, nullptr, nullptr};
    EMB_CHECK(embi_re_match_at(p, in, 0, caps, &lim) == EMBI_RE_NO_MATCH);
    embi_re_free(p);
    free(b);
}

EMB_TEST(compile_and_free_balance_memory)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 50; i++) {
        uint32_t caps[MAXCAPS];
        embi_re_prog *p = compile_str("(?<x>a|b)*\\k<x>[\\d\\p{L}]+(?=c)", "iu", nullptr);
        EMB_REQUIRE(p != nullptr);
        (void)caps;
        embi_re_free(p);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(long_input_scanning)
{
    /* A literal first character is found by scanning; the match is at the end. */
    uint32_t n = 100000;
    uint8_t *b = malloc(n);
    memset(b, 'x', n);
    b[n - 2] = 'y';
    b[n - 1] = 'z';
    embi_re_input in = {b, n, false};
    embi_re_prog *p = compile_str("yz", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    uint32_t caps[MAXCAPS];
    EMB_CHECK(embi_re_search(p, in, 0, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(caps[0] == n - 2 && caps[1] == n);
    embi_re_free(p);
    free(b);
}

/* A pattern made of n copies of open, the body, then n copies of close (heap string). */
static char *nested(const char *open, uint32_t n, const char *body, const char *close)
{
    size_t lo = strlen(open);
    size_t lb = strlen(body);
    size_t lc = strlen(close);
    char *s = malloc(n * (lo + lc) + lb + 1);
    char *w = s;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(w, open, lo);
        w += lo;
    }
    memcpy(w, body, lb);
    w += lb;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(w, close, lc);
        w += lc;
    }
    *w = '\0';
    return s;
}

EMB_TEST(nesting_depth_is_capped)
{
    /* 150 levels are accepted and match at full depth; 1000 levels are a SyntaxError with a message. */
    uint32_t big[2 * (150 + 1)];
    char *ok = nested("(", 150, "a", ")");
    EMB_CHECK(accepted(ok, ""));
    EMB_CHECK(matches(ok, "", "a", big) && span_is(big, 0, 0, 1) && span_is(big, 150, 0, 1));
    free(ok);
    char *deep = nested("(", 1000, "a", ")");
    EMB_CHECK(rejected(deep, ""));
    free(deep);

    char *la = nested("(?=", 150, "a", ")");
    EMB_CHECK(accepted(la, ""));
    EMB_CHECK(matches(la, "", "a", nullptr));
    free(la);
    char *lad = nested("(?=", 1000, "a", ")");
    EMB_CHECK(rejected(lad, ""));
    free(lad);

    char *cls = nested("[", 150, "a", "]");
    EMB_CHECK(accepted(cls, "v"));
    EMB_CHECK(matches(cls, "v", "a", nullptr));
    free(cls);
    char *clsd = nested("[", 1000, "a", "]");
    EMB_CHECK(rejected(clsd, "v"));
    free(clsd);
}

static bool count_poll_continue(void *ctx)
{
    uint32_t *count = ctx;
    *count += 1;
    return false;
}

EMB_TEST(interrupt_hook_returning_false_lets_match_finish)
{
    /* The hook is polled during a long match, returns false each time, and the match still succeeds. */
    uint32_t n = 20000;
    uint8_t *b = malloc(n);
    memset(b, 'a', n);
    b[0] = 'x';
    embi_re_input in = {b, n, false};
    embi_re_prog *p = compile_str("x(?:a|b)*", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    uint32_t caps[MAXCAPS];
    uint32_t polls = 0;
    embi_re_limits lim = {0, count_poll_continue, &polls};
    EMB_CHECK(embi_re_search(p, in, 0, caps, &lim) == EMBI_RE_MATCH);
    EMB_CHECK(span_is(caps, 0, 0, n));
    EMB_CHECK(polls >= 1);
    embi_re_free(p);
    free(b);
}

EMB_TEST(first_unit_filter_skips_start_positions)
{
    /* With foo|bar the first unit is f or b, so an input of x's never starts the matcher: the
     * interrupt hook, polled per 4096 matcher steps, is never called. A pattern whose first
     * consumed unit is behind a backreference has no filter and must run at every position. */
    uint32_t n = 1000000;
    uint8_t *b = malloc(n);
    memset(b, 'x', n);
    embi_re_input in = {b, n, false};
    uint32_t caps[MAXCAPS];

    embi_re_prog *filtered = compile_str("foo|bar", "", nullptr);
    EMB_REQUIRE(filtered != nullptr);
    uint32_t polls_filtered = 0;
    embi_re_limits lim_f = {0, count_poll_continue, &polls_filtered};
    EMB_CHECK(embi_re_search(filtered, in, 0, caps, &lim_f) == EMBI_RE_NO_MATCH);
    EMB_CHECK(polls_filtered == 0);
    embi_re_free(filtered);

    /* (a)? makes \1 a real backreference (there is a group), so the first unit is unknown. */
    embi_re_prog *unfiltered = compile_str("(a)?\\1(?:foo|bar)", "", nullptr);
    EMB_REQUIRE(unfiltered != nullptr);
    uint32_t polls_plain = 0;
    embi_re_limits lim_p = {0, count_poll_continue, &polls_plain};
    EMB_CHECK(embi_re_search(unfiltered, in, 0, caps, &lim_p) == EMBI_RE_NO_MATCH);
    EMB_CHECK(polls_plain > 100);
    embi_re_free(unfiltered);
    free(b);
}

EMB_TEST(first_unit_filter_correctness)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("foo|bar", "", "xxxbarxx", caps) && span_is(caps, 0, 3, 6));
    EMB_CHECK(matches("(a|b)c", "", "xxbc", caps) && span_is(caps, 0, 2, 4) &&
              group_is("xxbc", caps, 1, "b"));
    EMB_CHECK(matches("foo", "i", "xxFoO", caps) && span_is(caps, 0, 2, 5));
    EMB_CHECK(matches("[^x]y", "", "xxay", caps) && span_is(caps, 0, 2, 4));
    EMB_CHECK(matches("a*b", "", "xxb", caps) && span_is(caps, 0, 2, 3));
    EMB_CHECK(matches("b", "", "bxb", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("(?:a|)b", "", "b", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("a|", "", "xyz", caps) && span_is(caps, 0, 0, 0));
    EMB_CHECK(matches(".z", "", "\nxz", caps) && span_is(caps, 0, 1, 3));
    EMB_CHECK(matches("[a-c]+", "", "zzbca", caps) && span_is(caps, 0, 2, 5));

    /* Units >= 256 (high flag): a BMP start, a class range, and case folding to Latin-1 under u. */
    const uint16_t wide1[] = {'z', 'z', 0x0100};
    embi_re_input in1 = {wide1, 3, true};
    embi_re_prog *p1 = compile_str("\\u0100|a", "", nullptr);
    EMB_REQUIRE(p1 != nullptr);
    EMB_CHECK(embi_re_search(p1, in1, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 2, 3));
    embi_re_free(p1);
    const uint16_t wide2[] = {'a', 0x0150};
    embi_re_input in2 = {wide2, 2, true};
    embi_re_prog *p2 = compile_str("[\\u0100-\\u0200]", "", nullptr);
    EMB_REQUIRE(p2 != nullptr);
    EMB_CHECK(embi_re_search(p2, in2, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1, 2));
    embi_re_free(p2);

    const uint16_t kelvin[] = {0x212A};
    embi_re_input in3 = {kelvin, 1, true};
    embi_re_prog *pk = compile_str("k", "iu", nullptr);
    EMB_REQUIRE(pk != nullptr);
    EMB_CHECK(embi_re_search(pk, in3, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 1));
    embi_re_free(pk);
    embi_re_prog *pk2 = compile_str("k", "i", nullptr);
    EMB_REQUIRE(pk2 != nullptr);
    EMB_CHECK(embi_re_search(pk2, in3, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    embi_re_free(pk2);

    const uint16_t longs[] = {'x', 0x017F};
    embi_re_input in4 = {longs, 2, true};
    embi_re_prog *ps = compile_str("s", "iu", nullptr);
    EMB_REQUIRE(ps != nullptr);
    EMB_CHECK(embi_re_search(ps, in4, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1, 2));
    embi_re_free(ps);

    /* Supplementary characters: the scan starts on the lead surrogate, never inside the pair. */
    const uint16_t emo[] = {'x', 0xD83D, 0xDE00, 'y'};
    embi_re_input in5 = {emo, 4, true};
    embi_re_prog *pe = compile_str("(?:\\u{1F600}|y)", "u", nullptr);
    EMB_REQUIRE(pe != nullptr);
    EMB_CHECK(embi_re_search(pe, in5, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1, 3));
    embi_re_free(pe);
}

EMB_TEST(start_position_edges)
{
    uint32_t caps[MAXCAPS];
    const char *s = "ab";
    embi_re_input in = {s, 2, false};
    embi_re_prog *dollar = compile_str("$", "", nullptr);
    embi_re_prog *a = compile_str("a", "", nullptr);
    embi_re_prog *m = compile_str("(?m:^)", "", nullptr);
    embi_re_prog *star = compile_str("a*", "", nullptr);
    EMB_REQUIRE(dollar && a && m && star);
    /* start == len: an empty match at the end is found; a consuming pattern is not. */
    EMB_CHECK(embi_re_search(dollar, in, 2, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 2, 2));
    EMB_CHECK(embi_re_search(a, in, 2, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(a, in, 2, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(star, in, 2, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 2, 2));
    /* start > len: no match and no read past the input, for every pattern shape. */
    EMB_CHECK(embi_re_search(dollar, in, 3, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(m, in, 3, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(star, in, 100, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(m, in, 3, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(dollar, in, 3, caps, nullptr) == EMBI_RE_NO_MATCH);
    embi_re_free(dollar);
    embi_re_free(a);
    embi_re_free(m);
    embi_re_free(star);
}

EMB_TEST(empty_input_with_null_units)
{
    uint32_t caps[MAXCAPS];
    embi_re_input nul8 = {nullptr, 0, false};
    embi_re_input nul16 = {nullptr, 0, true};
    embi_re_prog *star = compile_str("a*", "", nullptr);
    embi_re_prog *dollar = compile_str("$", "", nullptr);
    embi_re_prog *lit = compile_str("a", "", nullptr);
    embi_re_prog *wb = compile_str("\\b", "", nullptr);
    EMB_REQUIRE(star && dollar && lit && wb);
    EMB_CHECK(embi_re_search(star, nul8, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 0));
    EMB_CHECK(embi_re_search(star, nul16, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 0));
    EMB_CHECK(embi_re_search(dollar, nul8, 0, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(embi_re_search(lit, nul8, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(lit, nul16, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(wb, nul16, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_match_at(lit, nul8, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(star, nul8, 1, caps, nullptr) == EMBI_RE_NO_MATCH);
    embi_re_free(star);
    embi_re_free(dollar);
    embi_re_free(lit);
    embi_re_free(wb);
}

EMB_TEST(may_contain_strings_is_static)
{
    /* MayContainStrings is decided from the operands, not from the strings left after an operation. */
    EMB_CHECK(rejected("[^[\\q{ab}--\\q{ab}]]", "v"));
    EMB_CHECK(rejected("[^[\\q{ab}&&\\q{ab}]]", "v"));
    EMB_CHECK(accepted("[^[\\q{a}--\\q{ab}]]", "v"));
    EMB_CHECK(accepted("[^[\\q{ab}&&\\q{a}]]", "v"));
    EMB_CHECK(accepted("[^\\q{a}]", "v"));
    EMB_CHECK(rejected("[^\\q{}]", "v"));
    EMB_CHECK(rejected("[^a[bc\\q{de}]]", "v"));
}

EMB_TEST(property_aliases_follow_ecma262)
{
    uint32_t caps[MAXCAPS];
    EMB_CHECK(rejected("\\p{WSpace}", "u"));
    EMB_CHECK(matches("\\p{space}", "u", " ", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(matches("\\p{White_Space}", "u", " ", caps) && span_is(caps, 0, 0, 1));
    EMB_CHECK(rejected("\\p{WSpac}", "u"));
}

EMB_TEST(first_set_literal_prefix_skips_wide_high_units)
{
    /* /foo/ can only start at 'f'. A million CJK units must never reach the matcher. */
    uint32_t n = 1000000;
    uint16_t *w = malloc((size_t)n * sizeof *w);
    for (uint32_t i = 0; i < n; i++) {
        w[i] = 0x4E00;
    }
    embi_re_input in = {w, n, true};
    uint32_t caps[MAXCAPS];
    const char *flags[] = {"", "u", "s", "m"};
    for (size_t f = 0; f < 4; f++) {
        embi_re_prog *p = compile_str("foo", flags[f], nullptr);
        EMB_REQUIRE(p != nullptr);
        uint32_t polls = 0;
        embi_re_limits lim = {0, count_poll_continue, &polls};
        EMB_CHECK(embi_re_search(p, in, 0, caps, &lim) == EMBI_RE_NO_MATCH);
        EMB_CHECK(polls == 0);
        embi_re_free(p);
    }
    /* A non-Latin-1 literal first unit still finds a late match. */
    w[n - 2] = 0x4E01;
    w[n - 1] = 'z';
    embi_re_prog *q = compile_str("\\u4E01z", "", nullptr);
    EMB_REQUIRE(q != nullptr);
    EMB_CHECK(embi_re_search(q, in, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, n - 2, n));
    embi_re_free(q);
    /* Case folding: U+212A (Kelvin) folds onto k in u mode, so it must pass the filter. */
    uint16_t kel[] = {0x4E00, 0x212A, 'z'};
    embi_re_input kin = {kel, 3, true};
    embi_re_prog *k = compile_str("kz", "iu", nullptr);
    EMB_REQUIRE(k != nullptr);
    EMB_CHECK(embi_re_search(k, kin, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1, 3));
    embi_re_free(k);
    /* A class entirely below 256 lets no unit >= 256 through. */
    embi_re_prog *c = compile_str("[a-f]x", "", nullptr);
    EMB_REQUIRE(c != nullptr);
    uint32_t polls = 0;
    embi_re_limits lim = {0, count_poll_continue, &polls};
    EMB_CHECK(embi_re_search(c, in, 0, caps, &lim) == EMBI_RE_NO_MATCH && polls == 0);
    embi_re_free(c);
    free(w);
}

EMB_TEST(first_set_single_unit_memchr_path)
{
    /* One possible first unit over Latin-1 input (memchr path): late and early matches, no match,
     * a start offset, and bytes >= 0x80. */
    uint32_t n = 1000000;
    uint8_t *b = malloc(n);
    memset(b, 0xE9, n);
    memcpy(b + n - 3, "kzz", 3);
    embi_re_input in = {b, n, false};
    uint32_t caps[MAXCAPS];
    embi_re_prog *p = compile_str("kzz", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_re_search(p, in, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, n - 3, n));
    EMB_CHECK(embi_re_search(p, in, n - 3, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, n - 3, n));
    EMB_CHECK(embi_re_search(p, in, n - 2, caps, nullptr) == EMBI_RE_NO_MATCH);
    b[n - 3] = 'x';
    EMB_CHECK(embi_re_search(p, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    memcpy(b, "kzz", 3);
    EMB_CHECK(embi_re_search(p, in, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 3));
    embi_re_free(p);
    /* A run of k's then "kz": each failed candidate restarts correctly. */
    memset(b, 'k', 1000);
    memcpy(b + 1000, "kz", 2);
    embi_re_prog *q = compile_str("kz", "", nullptr);
    EMB_REQUIRE(q != nullptr);
    EMB_CHECK(embi_re_search(q, in, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1000, 1002));
    embi_re_free(q);
    free(b);
}

EMB_TEST(start_inside_surrogate_pair_in_unicode_mode)
{
    uint16_t u[] = {0xD83D, 0xDE00, 'a'};
    embi_re_input in = {u, 3, true};
    uint32_t caps[MAXCAPS];
    embi_re_prog *trail = compile_str("\\uDE00", "u", nullptr);
    embi_re_prog *all = compile_str(".*", "u", nullptr);
    embi_re_prog *dot = compile_str(".", "u", nullptr);
    EMB_REQUIRE(trail && all && dot);
    /* The pair is one code point: starting at 1 means starting at 0, and a lone trail never matches. */
    EMB_CHECK(embi_re_search(trail, in, 1, caps, nullptr) == EMBI_RE_NO_MATCH);
    EMB_CHECK(embi_re_search(all, in, 1, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 3));
    EMB_CHECK(embi_re_search(dot, in, 1, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 2));
    EMB_CHECK(embi_re_match_at(dot, in, 1, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, 2));
    EMB_CHECK(embi_re_search(dot, in, 2, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 2, 3));
    /* Without the u flag the trail is its own unit. */
    embi_re_prog *nu = compile_str("\\uDE00", "", nullptr);
    EMB_REQUIRE(nu != nullptr);
    EMB_CHECK(embi_re_search(nu, in, 1, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 1, 2));
    embi_re_free(nu);
    embi_re_free(trail);
    embi_re_free(all);
    embi_re_free(dot);
}

EMB_TEST(lookbehind_repeats_alternation_and_captures)
{
    uint32_t caps[MAXCAPS];
    const char *in = "aaab";
    EMB_CHECK(matches("(?<=a+)b", "", in, caps) && span_is(caps, 0, 3, 4));
    EMB_CHECK(matches("(?<=a+?)b", "", in, caps) && span_is(caps, 0, 3, 4));
    EMB_CHECK(matches("(?<=a{2})b", "", in, caps) && span_is(caps, 0, 3, 4));
    EMB_CHECK(!matches("(?<=a{4})b", "", in, caps));
    EMB_CHECK(matches("(?<=^a+)b", "", in, caps) && span_is(caps, 0, 3, 4));
    /* Backward greedy takes as much as it can; lazy takes the least. */
    EMB_CHECK(matches("(?<=(a+))b", "", in, caps) && group_is(in, caps, 1, "aaa"));
    EMB_CHECK(matches("(?<=(a+?))b", "", in, caps) && group_is(in, caps, 1, "a"));
    /* Quantified lookbehind captures: the last iteration is the leftmost one. */
    in = "abcd";
    EMB_CHECK(matches("(?<=(\\w)+)d", "", in, caps) && group_is(in, caps, 1, "a"));
    EMB_CHECK(matches("(?<=(?:(\\w)(\\w))+)d", "", in, caps));
    /* Backward alternation and reversed sequence order. */
    EMB_CHECK(matches("(?<=bc|c)d", "", in, caps) && span_is(caps, 0, 3, 4));
    EMB_CHECK(matches("(?<=ab|b)c", "", in, caps) && span_is(caps, 0, 2, 3));
    EMB_CHECK(matches("(?<=(b)(c))d", "", in, caps) && group_is(in, caps, 1, "b") &&
              group_is(in, caps, 2, "c"));
    EMB_CHECK(!matches("(?<=cb)d", "", in, caps));
    EMB_CHECK(matches("(?<!x|y)d", "", in, caps));
    EMB_CHECK(!matches("(?<!c|y)d", "", in, caps));
}

static bool stop_on_second_poll(void *ctx)
{
    uint32_t *count = ctx;
    *count += 1;
    return *count >= 2;
}

EMB_TEST(search_paths_balance_memory)
{
    uint32_t n = 20000;
    uint8_t *b = malloc(n);
    memset(b, 'a', n);
    embi_re_input in = {b, n, false};
    uint32_t caps[2 * 200];
    size_t before = embi_mem_live();

    /* Heap-grown backtrack stack, then the cap: TOO_COMPLEX. */
    embi_re_prog *p = compile_str("(?:a|b)*c", "", nullptr);
    EMB_REQUIRE(p != nullptr);
    embi_re_limits cap = {1u << 16, nullptr, nullptr};
    EMB_CHECK(embi_re_search(p, in, 0, caps, &cap) == EMBI_RE_TOO_COMPLEX);
    EMB_CHECK(embi_re_match_at(p, in, 0, caps, &cap) == EMBI_RE_TOO_COMPLEX);
    /* Heap-grown stack, then INTERRUPTED. */
    uint32_t polls = 0;
    embi_re_limits irq = {0, stop_on_second_poll, &polls};
    EMB_CHECK(embi_re_search(p, in, 0, caps, &irq) == EMBI_RE_INTERRUPTED);
    EMB_CHECK(polls >= 2);
    /* Heap-grown stack, then NO_MATCH. */
    EMB_CHECK(embi_re_match_at(p, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    embi_re_free(p);

    /* More than 64 groups: slots live on the heap. */
    char pat[2048];
    size_t len = 0;
    for (int i = 0; i < 70; i++) {
        len += (size_t)snprintf(pat + len, sizeof pat - len, "(a)");
    }
    snprintf(pat + len, sizeof pat - len, "(?:a|b)*c");
    embi_re_prog *g = compile_str(pat, "", nullptr);
    EMB_REQUIRE(g != nullptr);
    EMB_CHECK(embi_re_capture_slots(g) == 142);
    EMB_CHECK(embi_re_match_at(g, in, 0, caps, &cap) == EMBI_RE_TOO_COMPLEX);
    polls = 0;
    EMB_CHECK(embi_re_match_at(g, in, 0, caps, &irq) == EMBI_RE_INTERRUPTED);
    EMB_CHECK(embi_re_match_at(g, in, 0, caps, nullptr) == EMBI_RE_NO_MATCH);
    b[n - 1] = 'c';
    EMB_CHECK(embi_re_match_at(g, in, 0, caps, nullptr) == EMBI_RE_MATCH && span_is(caps, 0, 0, n));
    EMB_CHECK(span_is(caps, 70, 69, 70));
    embi_re_free(g);

    EMB_CHECK(embi_mem_live() == before);
    free(b);
}

EMB_TEST(compile_many_distinct_classes_is_linear)
{
    /* 100000 distinct three-member classes. A per-class scan of all earlier classes would take
     * tens of seconds; the hashed dedupe keeps this well under the bound. */
    const uint32_t count = 100000;
    size_t cap = (size_t)count * 5 + 16;
    uint16_t *pat = malloc(cap * sizeof *pat);
    size_t len = 0;
    for (uint32_t i = 0; i < count; i++) {
        pat[len++] = '[';
        pat[len++] = (uint16_t)(0x400 + i % 250);
        pat[len++] = (uint16_t)(0x800 + i / 250);
        pat[len++] = 'x';
        pat[len++] = ']';
    }
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, (uint32_t)len, 0, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_REQUIRE(p != nullptr);
    uint64_t limit = EMB_TEST_ASAN ? 20000 : 3000;
    if (dt > limit) {
        SDL_Log("    compile took %llu ms", (unsigned long long)dt);
    }
    EMB_CHECK(dt <= limit);
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
    /* Identical classes share one entry and behave the same. */
    EMB_CHECK(matches("[abc][abc][abc]", "", "cab", nullptr));
    free(pat);
}

static void compile_timed(const uint16_t *pat, size_t len, uint32_t flags, bool expect_ok)
{
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, (uint32_t)len, flags, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_CHECK((p != nullptr) == expect_ok);
    uint64_t limit = EMB_TEST_ASAN ? 20000 : 2000;
    if (dt > limit) {
        SDL_Log("    compile took %llu ms", (unsigned long long)dt);
    }
    EMB_CHECK(dt <= limit);
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(compile_big_v_class_is_linear)
{
    /* A v-flag class of 20000 distinct code points: union must not re-normalize per member. */
    const uint32_t count = 20000;
    uint16_t *pat = malloc(((size_t)count + 4) * sizeof *pat);
    size_t len = 0;
    pat[len++] = '[';
    for (uint32_t i = 0; i < count; i++) {
        pat[len++] = (uint16_t)(0x4E00 + count - 1 - i);
    }
    pat[len++] = ']';
    compile_timed(pat, len, EMBI_RE_FLAG_V, true);
    free(pat);
    /* Same members, spaced so no ranges merge. */
    pat = malloc(((size_t)count + 4) * sizeof *pat);
    len = 0;
    pat[len++] = '[';
    for (uint32_t i = 0; i < count; i++) {
        pat[len++] = (uint16_t)(0x100 + ((i * 2) % 40000));
    }
    pat[len++] = ']';
    compile_timed(pat, len, EMBI_RE_FLAG_V, true);
    free(pat);
}

EMB_TEST(compile_many_class_strings_is_linear)
{
    /* [\q{aaaa|aaab|...}] with 6000 distinct four-character strings plus duplicates. */
    const uint32_t count = 6000;
    size_t cap = (size_t)count * 2 * 5 + 16;
    uint16_t *pat = malloc(cap * sizeof *pat);
    size_t len = 0;
    pat[len++] = '[';
    pat[len++] = 0x5C;
    pat[len++] = 'q';
    pat[len++] = '{';
    for (uint32_t i = 0; i < count * 2; i++) {
        uint32_t k = i % count; /* second half repeats the first */
        if (i) {
            pat[len++] = '|';
        }
        pat[len++] = (uint16_t)(0x400 + k % 70);
        pat[len++] = (uint16_t)(0x400 + k / 70);
        pat[len++] = 'x';
        pat[len++] = 'y';
    }
    pat[len++] = '}';
    pat[len++] = ']';
    compile_timed(pat, len, EMBI_RE_FLAG_V, true);
    free(pat);
}

EMB_TEST(compile_many_named_groups_is_linear)
{
    /* 20000 distinct named groups and a \k reference to each. */
    const uint32_t count = 20000;
    size_t cap = (size_t)count * 24 + 16;
    uint16_t *pat = malloc(cap * sizeof *pat);
    size_t len = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < count; i++) {
            char tmp[32];
            int n = snprintf(tmp, sizeof tmp, pass ? "\\k<g%u>" : "(?<g%u>)", i);
            for (int k = 0; k < n; k++) {
                pat[len++] = (uint16_t)tmp[k];
            }
        }
    }
    compile_timed(pat, len, 0, true);
    free(pat);
}

EMB_TEST(unicode_property_with_ignore_case_stays_cheap)
{
    /* Many \p{L} classes under iu: the case closure must not walk the whole fold table each time. */
    uint32_t caps[MAXCAPS];
    EMB_CHECK(matches("\\p{Lu}", "iu", "a", caps) && span_is(caps, 0, 0, 1));
    uint32_t reps = 3000;
    uint16_t unit[] = {'[', '\\', 'p', '{', 'L', '}', ']'};
    uint16_t *pat = malloc((size_t)reps * 7 * sizeof *pat);
    for (uint32_t i = 0; i < reps; i++) {
        memcpy(pat + (size_t)i * 7, unit, sizeof unit);
    }
    uint16_t fl[] = {'i', 'u'};
    uint32_t f = 0;
    EMB_REQUIRE(embi_re_parse_flags(fl, 2, &f));
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, reps * 7, f, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 15000u : 2000u));
    embi_re_free(p);
    free(pat);
}

EMB_TEST_MAIN()
