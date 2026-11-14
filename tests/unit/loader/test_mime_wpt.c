/*
 * MIME Sniffing data (WPT mimesniff/mime-types): parsing and serialization, the essence, the
 * charset-to-encoding result, and the generated corpus that only checks serialization. Also the
 * JavaScript and JSON essence checks and the extension table.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

#include "json_lite.h"

#define MIME_DATA EMBER_SOURCE_DIR "/tests/wpt/mimesniff/mime-types/resources/mime-types.json"
#define MIME_GENERATED EMBER_SOURCE_DIR "/tests/wpt/mimesniff/mime-types/resources/generated-mime-types.json"

static bool sb_matches(const embi_sb *s, const char *want)
{
    return s->len == strlen(want) && (s->len == 0 || memcmp(s->data, want, s->len) == 0);
}

/* Runs one corpus. Returns the failure count and sets *cases. */
static size_t run_mime_corpus(const char *path, bool check_extras, size_t *cases)
{
    jnode *root = json_load_file(path);
    if (root == nullptr || root->kind != JK_ARR) {
        json_free(root);
        *cases = 0;
        return 1;
    }
    size_t n = 0;
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
        n++;
        embi_mime m = {0};
        bool parsed = embi_mime_parse((embi_str){input->str, (uint32_t)input->slen}, &m);
        bool row_ok = true;
        if (output->kind == JK_NULL) {
            row_ok = !parsed;
        } else if (!parsed) {
            row_ok = false;
        } else {
            embi_sb ser = {nullptr, 0, 0};
            embi_mime_serialize(&m, &ser);
            row_ok = ser.len == output->slen && (ser.len == 0 || memcmp(ser.data, output->str, ser.len) == 0);
            embi_sb_free(&ser);
        }

        if (check_extras && row_ok && parsed) {
            /* The essence. */
            /* An empty minimizedMIMEType is no expectation (the WPT harness tests it for truth). */
            const jnode *minimized = json_get(item, "minimizedMIMEType");
            if (json_is_str(minimized) && minimized->slen > 0) {
                embi_sb ess = {nullptr, 0, 0};
                embi_mime_essence(&m, &ess);
                row_ok = sb_matches(&ess, minimized->str);
                embi_sb_free(&ess);
            }
            /* The charset encoding, compared by name ignoring case. */
            const jnode *enc = json_get(item, "encoding");
            if (row_ok && enc != nullptr) {
                embi_str charset = embi_mime_param_value(&m, EMBI_STR("charset"));
                const embi_encoding *e = charset.ptr != nullptr ? embi_encoding_for_label(charset) : nullptr;
                if (enc->kind == JK_NULL) {
                    row_ok = e == nullptr;
                } else {
                    row_ok = e != nullptr && json_is_str(enc) &&
                             embi_str_ieq((embi_str){e->name, (uint32_t)strlen(e->name)},
                                          (embi_str){enc->str, (uint32_t)enc->slen});
                }
            }
        }
        if (!row_ok) {
            failed++;
            if (failed <= 8) {
                fprintf(stderr, "  mime %s: input \"%s\"\n", path, input->str);
            }
        }
        embi_mime_free(&m);
    }
    json_free(root);
    *cases = n;
    return failed;
}

EMB_TEST(mime_wpt_mime_types)
{
    size_t cases = 0;
    size_t failed = run_mime_corpus(MIME_DATA, true, &cases);
    fprintf(stderr, "mime-types: %zu cases, %zu failed\n", cases, failed);
    EMB_CHECK(cases >= 74);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

EMB_TEST(mime_wpt_generated_corpus)
{
    size_t cases = 0;
    size_t failed = run_mime_corpus(MIME_GENERATED, false, &cases);
    fprintf(stderr, "generated-mime-types: %zu cases, %zu failed\n", cases, failed);
    EMB_CHECK(cases > 800);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

EMB_TEST(mime_essence_checks_and_extensions)
{
    EMB_CHECK(embi_mime_essence_is_javascript(EMBI_STR("text/javascript")));
    EMB_CHECK(embi_mime_essence_is_javascript(EMBI_STR("APPLICATION/ECMASCRIPT")));
    EMB_CHECK(!embi_mime_essence_is_javascript(EMBI_STR("text/plain")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("application/json")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("application/ld+json")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("text/json")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("TEXT/JSON")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("x/+json")));
    EMB_CHECK(embi_mime_essence_is_json(EMBI_STR("text/+json")));
    EMB_CHECK(!embi_mime_essence_is_json(EMBI_STR("+json")));
    EMB_CHECK(!embi_mime_essence_is_json(EMBI_STR("/a+json")));
    EMB_CHECK(!embi_mime_essence_is_json(EMBI_STR("text/jsonx")));

    embi_str t = embi_mime_from_path(EMBI_STR("/assets/app.MJS?v=2#x"));
    EMB_CHECK(embi_str_eq(t, EMBI_STR("text/javascript")));
    t = embi_mime_from_path(EMBI_STR("/img/logo.webp"));
    EMB_CHECK(embi_str_eq(t, EMBI_STR("image/webp")));
    t = embi_mime_from_path(EMBI_STR("/dir.v2/file"));
    EMB_CHECK(t.ptr == nullptr);
    t = embi_mime_from_path(EMBI_STR("/x.unknown"));
    EMB_CHECK(t.ptr == nullptr);
}

EMB_TEST(mime_parameter_quoting_round_trip)
{
    embi_mime m = {0};
    EMB_REQUIRE(embi_mime_parse(EMBI_STR("text/plain; a=\"x y\\\"z\"; b=c; a=dup"), &m));
    embi_sb s = {nullptr, 0, 0};
    embi_mime_serialize(&m, &s);
    EMB_CHECK(sb_matches(&s, "text/plain;a=\"x y\\\"z\";b=c"));
    embi_sb_free(&s);
    embi_mime_free(&m);
}

LOADER_TEST_MAIN()
