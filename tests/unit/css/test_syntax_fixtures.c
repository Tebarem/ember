/*
 * Runs the JSON suites in tests/data/css-parsing-tests (every file except the color and An+B ones) through
 * the syntax parsers and compares canonical dumps. Failing cases must be listed in
 * tests/css/expectations.txt, one per line: "<file> <case index> <reason>". A listed case that
 * passes is an error, so the list stays exact.
 */
#include "emb_test.h"

#include "css/css.h"
#include "css_dump.h"
#include "json_mini.h"

#include <SDL3/SDL_iostream.h>
#include <string.h>

#define FIXTURE_DIR EMBER_SOURCE_DIR "/tests/data/css-parsing-tests/"
#define EXPECT_PATH EMBER_SOURCE_DIR "/tests/css/expectations.txt"
#define MAX_EXPECT 256

typedef struct expect_entry {
    char file[64];
    uint32_t index;
} expect_entry;

static expect_entry expects[MAX_EXPECT];
static uint32_t expect_count;
static bool expects_loaded;

static void load_expectations(void)
{
    if (expects_loaded) {
        return;
    }
    expects_loaded = true;
    size_t n = 0;
    char *text = SDL_LoadFile(EXPECT_PATH, &n);
    if (text == nullptr) {
        SDL_Log("cannot read %s", EXPECT_PATH);
        return;
    }
    size_t p = 0;
    while (p < n && expect_count < MAX_EXPECT) {
        size_t e = p;
        while (e < n && text[e] != '\n') {
            e++;
        }
        if (text[p] != '#' && e > p) {
            char line[256];
            size_t len = e - p;
            if (len >= sizeof line) {
                len = sizeof line - 1;
            }
            memcpy(line, text + p, len);
            line[len] = '\0';
            char *save = nullptr;
            char *file = SDL_strtok_r(line, " \t\r", &save);
            char *idx = SDL_strtok_r(nullptr, " \t\r", &save);
            if (file != nullptr && idx != nullptr) {
                expect_entry *ent = &expects[expect_count++];
                SDL_strlcpy(ent->file, file, sizeof ent->file);
                ent->index = (uint32_t)SDL_strtol(idx, nullptr, 10);
            }
        }
        p = e + 1;
    }
    SDL_free(text);
}

static bool is_expected_failure(const char *file, uint32_t index)
{
    for (uint32_t k = 0; k < expect_count; k++) {
        if (expects[k].index == index && SDL_strcmp(expects[k].file, file) == 0) {
            return true;
        }
    }
    return false;
}

typedef enum fixture_kind {
    FK_CVS,
    FK_ONE_CV,
    FK_DECL_LIST,
    FK_RULE_LIST,
    FK_STYLESHEET,
    FK_BLOCK_CONTENTS,
    FK_ONE_RULE,
    FK_ONE_DECL,
    FK_BYTES,
} fixture_kind;

/* Converts a css_bytes string (code points U+0000..U+00FF are bytes) to bytes, in place. */
static size_t bytes_from_jm(const jm_val *s, unsigned char *out)
{
    const char *p = s->str;
    const char *end = s->str + s->len;
    size_t n = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        out[n++] = (unsigned char)cp;
    }
    return n;
}

static void dump_items_or_error(embi_sb *sb, embi_css_items items)
{
    cd_items(sb, items);
}

/* One stylesheet_bytes input, decoded from the fixture's code-point-per-byte strings. */
typedef struct bytes_case {
    unsigned char data[1 << 14];
    size_t n;
    unsigned char pbuf[256];
    unsigned char ebuf[256];
    embi_str protocol;
    embi_str env;
} bytes_case;

static void bytes_case_load(bytes_case *c, const jm_val *input)
{
    c->n = bytes_from_jm(jm_get(input, "css_bytes"), c->data);
    c->protocol = (embi_str){nullptr, 0};
    c->env = (embi_str){nullptr, 0};
    const jm_val *pv = jm_get(input, "protocol_encoding");
    const jm_val *ev = jm_get(input, "environment_encoding");
    if (pv != nullptr && pv->type == JM_STRING) {
        size_t m = bytes_from_jm(pv, c->pbuf);
        c->protocol = (embi_str){(const char *)c->pbuf, (uint32_t)m};
    }
    if (ev != nullptr && ev->type == JM_STRING) {
        size_t m = bytes_from_jm(ev, c->ebuf);
        c->env = (embi_str){(const char *)c->ebuf, (uint32_t)m};
    }
}

/* A failing case still counts as a partial pass when the detected encoding label is the one the
 * expected output names (its last element). */
static bool bytes_label_matches(const jm_val *input, const jm_val *expected)
{
    if (expected->type != JM_ARRAY || expected->count == 0) {
        return false;
    }
    const jm_val *last = &expected->items[expected->count - 1];
    if (last->type != JM_STRING) {
        return false;
    }
    bytes_case *c = embi_alloc(sizeof *c);
    bytes_case_load(c, input);
    embi_arena a;
    embi_arena_init(&a, 8192);
    embi_str decoded;
    const char *enc = nullptr;
    (void)embi_css_decode_bytes(&a, c->data, c->n, c->protocol, c->env, &decoded, &enc);
    bool same = enc != nullptr && strlen(enc) == last->len && memcmp(enc, last->str, last->len) == 0;
    embi_arena_free(&a);
    embi_free(c, sizeof *c);
    return same;
}

/* Produces our canonical output for one case. Returns false when the input is not a string. */
static void run_case(fixture_kind kind, const jm_val *input, embi_sb *out)
{
    embi_arena a;
    embi_arena_init(&a, 8192);
    embi_str text = {input->str, (uint32_t)input->len};

    if (kind == FK_BYTES) {
        bytes_case *c = embi_alloc(sizeof *c);
        bytes_case_load(c, input);
        embi_str decoded;
        const char *enc = nullptr;
        if (!embi_css_decode_bytes(&a, c->data, c->n, c->protocol, c->env, &decoded, &enc)) {
            embi_sb_append(out, EMBI_STR("[\"unsupported\","));
            cd_cstr(out, enc);
            embi_sb_append(out, EMBI_STR("]"));
        } else {
            embi_css_slice cvs = embi_css_parse_cvs(&a, decoded);
            embi_css_items items = embi_css_consume_rules(&a, cvs, true);
            embi_sb_append(out, EMBI_STR("["));
            cd_items(out, items);
            embi_sb_append(out, EMBI_STR(","));
            cd_cstr(out, enc);
            embi_sb_append(out, EMBI_STR("]"));
        }
        embi_free(c, sizeof *c);
        embi_arena_free(&a);
        return;
    }

    embi_css_slice cvs = embi_css_parse_cvs(&a, text);
    switch (kind) {
    case FK_CVS:
        embi_sb_append(out, EMBI_STR("["));
        cd_cv_list(out, cvs);
        embi_sb_append(out, EMBI_STR("]"));
        break;
    case FK_ONE_CV: {
        const embi_css_cv *cv = nullptr;
        const char *err = embi_css_parse_component_value(cvs, &cv);
        if (err != nullptr) {
            cd_lit(out, "[\"error\",");
            cd_cstr(out, err);
            cd_lit(out, "]");
        } else {
            /* Dump the single value by wrapping it as a one-element list. */
            embi_css_slice one = embi_css_slice_make(cv, 1);
            cd_cv(out, &one.p[0]);
        }
        break;
    }
    case FK_DECL_LIST:
        dump_items_or_error(out, embi_css_consume_declarations(&a, cvs));
        break;
    case FK_RULE_LIST:
        dump_items_or_error(out, embi_css_consume_rules(&a, cvs, false));
        break;
    case FK_STYLESHEET:
        dump_items_or_error(out, embi_css_consume_rules(&a, cvs, true));
        break;
    case FK_BLOCK_CONTENTS:
        dump_items_or_error(out, embi_css_consume_block_contents(&a, cvs));
        break;
    case FK_ONE_RULE: {
        embi_css_item it = embi_css_parse_rule(&a, cvs);
        cd_item(out, &it);
        break;
    }
    case FK_ONE_DECL: {
        embi_css_item it = embi_css_parse_declaration(&a, cvs);
        cd_item(out, &it);
        break;
    }
    default:
        break;
    }
    embi_arena_free(&a);
}

static void run_fixture(const char *file, fixture_kind kind)
{
    load_expectations();

    char path[512];
    SDL_snprintf(path, sizeof path, "%s%s", FIXTURE_DIR, file);
    size_t n = 0;
    char *text = SDL_LoadFile(path, &n);
    EMB_REQUIRE(text != nullptr);

    jm_val root;
    bool parsed = jm_parse(text, n, &root);
    SDL_free(text);
    EMB_REQUIRE(parsed);
    EMB_REQUIRE(root.type == JM_ARRAY);
    EMB_REQUIRE(root.count % 2 == 0);

    uint32_t pass = 0;
    uint32_t xfail = 0;
    uint32_t partial = 0;
    uint32_t fail = 0;
    for (size_t k = 0; k + 1 < root.count; k += 2) {
        uint32_t index = (uint32_t)(k / 2);
        const jm_val *input = &root.items[k];
        const jm_val *expected = &root.items[k + 1];

        embi_sb got = {0};
        embi_sb want = {0};
        run_case(kind, input, &got);
        cd_jm(&want, expected);
        bool match = got.len == want.len && memcmp(got.data, want.data, got.len) == 0;
        bool listed = is_expected_failure(file, index);

        if (match && !listed) {
            pass++;
        } else if (!match && listed && kind == FK_BYTES && bytes_label_matches(input, expected)) {
            partial++;
        } else if (!match && listed) {
            xfail++;
        } else if (!match) {
            fail++;
            SDL_Log("%s case %u: got %.*s", file, index, (int)got.len, got.data ? got.data : "");
            SDL_Log("%s case %u: want %.*s", file, index, (int)want.len,
                    want.data ? want.data : "");
        } else {
            fail++;
            SDL_Log("%s case %u matches but is listed as an expected failure", file, index);
        }
        embi_sb_free(&got);
        embi_sb_free(&want);
    }
    jm_free(&root);
    SDL_Log("css-parsing-tests/%s: %u pass, %u expected failures (%u with the right encoding "
            "label), %u fail",
            file, pass, xfail + partial, partial, fail);
    EMB_CHECK(fail == 0);
}

EMB_TEST(css_fixture_component_value_list)
{
    run_fixture("component_value_list.json", FK_CVS);
}

EMB_TEST(css_fixture_one_component_value)
{
    run_fixture("one_component_value.json", FK_ONE_CV);
}

EMB_TEST(css_fixture_declaration_list)
{
    run_fixture("declaration_list.json", FK_DECL_LIST);
}

EMB_TEST(css_fixture_rule_list)
{
    run_fixture("rule_list.json", FK_RULE_LIST);
}

EMB_TEST(css_fixture_stylesheet)
{
    run_fixture("stylesheet.json", FK_STYLESHEET);
}

EMB_TEST(css_fixture_blocks_contents)
{
    run_fixture("blocks_contents.json", FK_BLOCK_CONTENTS);
}

EMB_TEST(css_fixture_one_rule)
{
    run_fixture("one_rule.json", FK_ONE_RULE);
}

EMB_TEST(css_fixture_one_declaration)
{
    run_fixture("one_declaration.json", FK_ONE_DECL);
}

EMB_TEST(css_fixture_stylesheet_bytes)
{
    run_fixture("stylesheet_bytes.json", FK_BYTES);
}

EMB_TEST_MAIN()
