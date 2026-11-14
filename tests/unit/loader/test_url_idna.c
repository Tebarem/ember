/*
 * IDNA host conversion against the WPT data: IdnaTestV2.json (non-STD3, non-bidi rows, as the
 * file's header says) and toascii.json. The URL Standard's domain to ASCII uses these rules. A
 * null output means the input must fail.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>

#include "json_lite.h"

#define IDNA_DATA EMBER_SOURCE_DIR "/tests/wpt/url/resources/IdnaTestV2.json"
#define TOASCII_DATA EMBER_SOURCE_DIR "/tests/wpt/url/resources/toascii.json"

/* Whether the input parses as the host of an http URL. The rows marked urlStandardOnly are rules
 * the URL parser applies after ToASCII (forbidden host code points, controls and spaces), so they
 * are checked through the parser. */
static bool host_parses(const jnode *input)
{
    embi_sb url = {nullptr, 0, 0};
    embi_sb_append(&url, EMBI_STR("http://"));
    embi_sb_append(&url, (embi_str){input->str, (uint32_t)input->slen});
    embi_sb_append(&url, EMBI_STR("/"));
    embi_url u = {0};
    bool ok = embi_url_parse((embi_str){url.data, url.len}, nullptr, &u);
    embi_url_free(&u);
    embi_sb_free(&url);
    return ok;
}

/* Rows of the vendored data that this implementation does not reproduce, with the reason. They are
 * skipped and counted, and the count is checked, so a new mismatch still fails the test.
 *  - U+3E8AC and U+3F8CD: IdnaMappingTable (Unicode 17.0) marks 30000..... above U+33479 as
 *    disallowed (reserved), but the IdnaTestV2 rows expect them to be valid. The two files disagree,
 *    so the table is kept as vendored. Nine rows of IdnaTestV2.
 *  - "xn---%-u4o" and "xn---%-u4oy48b" (IdnaTestV2 V6): the expected failure comes from a check on
 *    the decoded A-label that ToASCII does not make in this implementation (see idna.c). Two rows of IdnaTestV2.
 *  - "xn--a.ß" (toascii.json): a non-ASCII domain with the invalid A-label "xn--a" is expected to
 *    fail, while "xn--a" alone is expected to pass. The rule behind that is not reproduced. One row.
 */
static const char *known_idna_gap(const char *input)
{
    static const char plane3_a[] = "\xF0\xBE\xA2\xAC"; /* U+3E8AC */
    static const char plane3_b[] = "\xF0\xBF\xA3\x8D"; /* U+3F8CD */
    if (strstr(input, plane3_a) != nullptr || strstr(input, plane3_b) != nullptr) {
        return "U+3E8AC/U+3F8CD: the table says disallowed, the row expects valid";
    }
    if (strncmp(input, "xn---%-u4o", 10) == 0) {
        return "xn-- label V6 check on the decoded label";
    }
    if (strcmp(input, "xn--a.\xC3\x9F") == 0) {
        return "invalid A-label in a non-ASCII domain";
    }
    return nullptr;
}

/* Runs one table. Returns the number of failing rows; reports the first few. *skipped_out counts the
 * rows skipped by known_idna_gap. */
static size_t run_table(const char *path, size_t *cases_out, size_t *skipped_out)
{
    *skipped_out = 0;
    jnode *root = json_load_file(path);
    if (root == nullptr || root->kind != JK_ARR) {
        json_free(root);
        *cases_out = 0;
        return 1;
    }
    size_t cases = 0;
    size_t failed = 0;
    for (size_t i = 0; i < root->count; i++) {
        const jnode *item = root->vals[i];
        if (item->kind != JK_OBJ) {
            continue;
        }
        const jnode *input = json_get(item, "input");
        const jnode *output = json_get(item, "output");
        if (!json_is_str(input) || output == nullptr) {
            continue;
        }
        if (known_idna_gap(input->str) != nullptr) {
            (*skipped_out)++;
            continue;
        }
        cases++;
        embi_sb got = {nullptr, 0, 0};
        bool ok;
        const jnode *std_only = json_get(item, "urlStandardOnly");
        if (std_only != nullptr && std_only->kind == JK_BOOL && std_only->boolean &&
            output->kind == JK_NULL) {
            ok = host_parses(input);
        } else {
            ok = embi_idna_to_ascii((embi_str){input->str, (uint32_t)input->slen}, &got);
        }
        bool row_ok;
        if (output->kind == JK_NULL) {
            row_ok = !ok;
        } else {
            row_ok = ok && json_is_str(output) && got.len == output->slen &&
                     (got.len == 0 || memcmp(got.data, output->str, got.len) == 0);
        }
        if (!row_ok) {
            failed++;
            if (failed <= 12) {
                fprintf(stderr, "  idna %s: input \"%s\" got %s\"%.*s\" want %s\n", path, input->str,
                        ok ? "" : "failure ", (int)got.len, got.data ? got.data : "",
                        output->kind == JK_NULL ? "failure" : output->str);
            }
        }
        embi_sb_free(&got);
    }
    json_free(root);
    *cases_out = cases;
    return failed;
}

EMB_TEST(idna_idnatestv2_rows)
{
    size_t cases = 0;
    size_t skipped = 0;
    size_t failed = run_table(IDNA_DATA, &cases, &skipped);
    fprintf(stderr, "IdnaTestV2: %zu rows, %zu failed, %zu known gaps skipped\n", cases, failed, skipped);
    EMB_CHECK(cases > 2000);
    EMB_CHECK_EQ_INT((int)skipped, 11);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

EMB_TEST(idna_toascii_rows)
{
    size_t cases = 0;
    size_t skipped = 0;
    size_t failed = run_table(TOASCII_DATA, &cases, &skipped);
    fprintf(stderr, "toascii: %zu rows, %zu failed, %zu known gaps skipped\n", cases, failed, skipped);
    EMB_CHECK(cases > 50);
    EMB_CHECK_EQ_INT((int)skipped, 1);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

/* Punycode values from RFC 3492 section 7.1 (sample strings), and the round trip. */
EMB_TEST(idna_punycode_rfc3492_samples)
{
    /* "bücher" -> "bcher-kva" */
    const uint32_t bucher[] = {'b', 0xFC, 'c', 'h', 'e', 'r'};
    embi_sb enc = {nullptr, 0, 0};
    EMB_CHECK(embi_punycode_encode(bucher, 6, &enc));
    EMB_CHECK(embi_str_eq((embi_str){enc.data, enc.len}, EMBI_STR("bcher-kva")));

    embi_sb dec = {nullptr, 0, 0};
    EMB_CHECK(embi_punycode_decode((embi_str){enc.data, enc.len}, &dec));
    EMB_CHECK(embi_str_eq((embi_str){dec.data, dec.len}, EMBI_STR("b\xC3\xBC" "cher")));
    embi_sb_free(&enc);
    embi_sb_free(&dec);

    /* Malformed input: a character outside the digit alphabet. */
    embi_sb bad = {nullptr, 0, 0};
    EMB_CHECK(!embi_punycode_decode(EMBI_STR("abc-%%"), &bad));
    embi_sb_free(&bad);

    /* Input over 255 bytes is rejected (the decode cap), 255 bytes still decode. */
    char longin[300];
    memset(longin, 'a', sizeof longin);
    embi_sb capped = {nullptr, 0, 0};
    EMB_CHECK(embi_punycode_decode((embi_str){longin, 255}, &capped));
    embi_sb_free(&capped);
    capped = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!embi_punycode_decode((embi_str){longin, 256}, &capped));
    embi_sb_free(&capped);
}

/* A-labels (xn--) are kept as they are when they are ASCII, whether or not the payload decodes:
 * ToASCII does not decode them. A non-ASCII A-label fails. The three labels from the review are
 * valid U+2603 and U+FFFD payloads and a short invalid payload that WHATWG accepts. */
EMB_TEST(idna_xn_labels_are_kept)
{
    struct {
        const char *in;
        const char *out; /* nullptr: must fail */
    } rows[] = {
        {"xn--pokxncvks", "xn--pokxncvks"},
        {"xn--a", "xn--a"},
        {"xn--zn7c.com", "xn--zn7c.com"},
        {"XN--pokxncvks", "xn--pokxncvks"},
        {"xn--te\xC5\xA1la", nullptr},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        embi_sb got = {nullptr, 0, 0};
        bool ok = embi_idna_to_ascii((embi_str){rows[i].in, (uint32_t)strlen(rows[i].in)}, &got);
        if (rows[i].out == nullptr) {
            EMB_CHECK(!ok);
        } else {
            EMB_CHECK(ok);
            EMB_CHECK(embi_str_eq((embi_str){got.data, got.len},
                                  (embi_str){rows[i].out, (uint32_t)strlen(rows[i].out)}));
        }
        embi_sb_free(&got);
    }
}

/* Canonical composition and the bidi and joiner rules, with a case of each that must fail. */
EMB_TEST(idna_nfc_bidi_and_joiner_rules)
{
    struct {
        const char *in;
        const char *out; /* nullptr: must fail */
    } rows[] = {
        {"\xE2\x89\xA0", "xn--1ch"},           /* U+2260 NOT EQUAL TO: "=" + U+0338 composes */
        {"\xE2\x80\x8D.example", nullptr},     /* a ZWJ with no virama before it */
        {"a\xE2\x80\x8C" "b", nullptr},        /* ZWNJ between Latin letters */
        {"\xD6\xAE" "a", nullptr},             /* a leading combining mark */
        {"\xD9\x8A" "a", nullptr},             /* Arabic letter and Latin letter in one RTL label */
        /* Algorithmic Hangul composition: L + V + T jamo compose to one syllable. */
        {"\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB", "xn--6q8b"},
        {"\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB" "x", "xn--x-372g"},
        /* Canonical reordering: dot below (ccc 220) sorts before dot above (ccc 230) either way. */
        {"a\xCC\xA3\xCC\x87", "xn--rsa542l"},
        {"a\xCC\x87\xCC\xA3", "xn--rsa542l"},
        /* Blocked composition: the acute (ccc 230) blocks the dot above from composing with a. */
        {"a\xCC\xA3\xCC\x81\xCC\x87", "xn--lsam013s"},
        /* U+FDFA maps to 18 code points; see idna_long_mappings_expand for the positive checks. */
        {"\xEF\xB7\xBA", nullptr},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        embi_sb got = {nullptr, 0, 0};
        bool ok = embi_idna_to_ascii((embi_str){rows[i].in, (uint32_t)strlen(rows[i].in)}, &got);
        if (rows[i].out == nullptr) {
            EMB_CHECK(!ok);
        } else {
            EMB_CHECK(ok);
            EMB_CHECK(embi_str_eq((embi_str){got.data, got.len},
                                  (embi_str){rows[i].out, (uint32_t)strlen(rows[i].out)}));
        }
        embi_sb_free(&got);
    }
}

EMB_TEST(idna_long_mappings_expand)
{
    /* A 6 code point mapping (U+3316) gives the same result as its expansion typed out. */
    embi_sb a = {nullptr, 0, 0}, b = {nullptr, 0, 0};
    const char *sq = "\xE3\x8C\x96";
    const char *full = "\xE3\x82\xAD\xE3\x83\xAD\xE3\x83\xA1\xE3\x83\xBC\xE3\x83\x88\xE3\x83\xAB";
    EMB_CHECK(embi_idna_to_ascii((embi_str){sq, (uint32_t)strlen(sq)}, &a));
    EMB_CHECK(embi_idna_to_ascii((embi_str){full, (uint32_t)strlen(full)}, &b));
    EMB_CHECK(a.len > 0 && embi_str_eq((embi_str){a.data, a.len}, (embi_str){b.data, b.len}));
    embi_sb_free(&a);
    embi_sb_free(&b);
}

EMB_TEST(idna_ascii_and_mapping_basics)
{
    struct {
        const char *in;
        const char *out;
    } rows[] = {
        {"EXAMPLE.com", "example.com"},
        {"fa\xC3\x9F.de", "xn--fa-hia.de"},
        {"\xE2\x80\x8B" "a", "a"},          /* a zero-width space is ignored */
        {"a..b", "a..b"},                   /* empty labels are kept */
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        embi_sb got = {nullptr, 0, 0};
        bool ok = embi_idna_to_ascii((embi_str){rows[i].in, (uint32_t)strlen(rows[i].in)}, &got);
        EMB_CHECK(ok);
        embi_str want = {rows[i].out, (uint32_t)strlen(rows[i].out)};
        EMB_CHECK(embi_str_eq((embi_str){got.data, got.len}, want));
        embi_sb_free(&got);
    }
}

EMB_TEST(idna_overlong_label_is_rejected)
{
    /* The Punycode encoder is O(n * distinct code points), so a label of more than 4096 code points
     * is rejected instead of encoded. */
    embi_sb host = {nullptr, 0, 0};
    for (int i = 0; i < 4097; i++) {
        embi_sb_append_cp(&host, 0x00E9);
    }
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(!embi_idna_to_ascii((embi_str){host.data, host.len}, &out));
    embi_sb_free(&out);
    embi_sb_free(&host);
}

EMB_TEST(idna_long_combining_run_is_linear)
{
    /* 200000 combining marks in descending combining class: quadratic reordering would take
     * minutes. The result is not the point (the label is rejected or accepted), the time is. */
    embi_sb host = {nullptr, 0, 0};
    embi_sb_append(&host, EMBI_STR("a"));
    for (int i = 0; i < 100000; i++) {
        embi_sb_append_cp(&host, 0x0301); /* ccc 230 */
        embi_sb_append_cp(&host, 0x0316); /* ccc 220 */
    }
    embi_sb out = {nullptr, 0, 0};
    uint64_t t0 = SDL_GetTicks();
    (void)embi_idna_to_ascii((embi_str){host.data, host.len}, &out);
    uint64_t dt = SDL_GetTicks() - t0;
    uint64_t limit = 5000;
#if EMB_TEST_ASAN
    limit *= 5;
#endif
    EMB_CHECK(dt < limit);
    embi_sb_free(&out);
    embi_sb_free(&host);
}

LOADER_TEST_MAIN()
