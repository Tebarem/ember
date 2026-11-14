/*
 * WPT url/percent-encoding.json: the query of a URL parsed with a document encoding. The
 * single-byte columns (windows-1252, iso-8859-2) and utf-8 are run; the CJK columns (big5,
 * euc-kr, gb18030, iso-2022-jp, shift_jis) have no encoder here and are skipped and counted.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

#include "json_lite.h"

#define PCT_DATA EMBER_SOURCE_DIR "/tests/wpt/url/resources/percent-encoding.json"

EMB_TEST(url_wpt_percent_encoding)
{
    jnode *root = json_load_file(PCT_DATA);
    EMB_REQUIRE(root != nullptr && root->kind == JK_ARR);
    size_t run = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < root->count; i++) {
        const jnode *item = root->vals[i];
        const jnode *input = json_get(item, "input");
        const jnode *output = json_get(item, "output");
        if (!json_is_str(input) || output == nullptr) {
            continue;
        }
        for (size_t k = 0; k < output->count; k++) {
            const char *name = output->keys[k];
            const embi_encoding *enc = embi_encoding_named(name);
            if (enc == nullptr || enc->kind == EMBI_ENC_UNSUPPORTED) {
                skipped++;
                continue;
            }
            embi_sb url = {nullptr, 0, 0};
            embi_sb_append(&url, EMBI_STR("https://example.com/?"));
            embi_sb_append(&url, (embi_str){input->str, (uint32_t)input->slen});
            embi_url u = {0};
            bool ok = embi_url_parse_enc((embi_str){url.data, url.len}, nullptr, enc, &u);
            EMB_CHECK(ok);
            if (ok) {
                embi_sb q = {nullptr, 0, 0};
                embi_url_get(&u, EMBI_URL_SEARCH, &q);
                const jnode *want = output->vals[k];
                /* The search getter has the leading '?'. */
                bool match = json_is_str(want) && q.len == want->slen + 1 && q.data[0] == '?' &&
                             memcmp(q.data + 1, want->str, want->slen) == 0;
                if (!match) {
                    fprintf(stderr, "  percent-encoding %s: got %.*s\n", name, (int)q.len, q.data);
                }
                EMB_CHECK(match);
                embi_sb_free(&q);
            }
            embi_url_free(&u);
            embi_sb_free(&url);
            run++;
        }
    }
    json_free(root);
    EMB_CHECK(run > 0);
    EMB_CHECK(skipped > 0);
}

EMB_TEST(url_query_encoding_is_utf8_for_other_kinds)
{
    /* UTF-16 and a null encoding both mean UTF-8 for the query (URL standard). */
    const char *in = "http://h/?\xC3\xA9";
    const embi_encoding *encs[] = {nullptr, embi_encoding_named("utf-16le"), embi_encoding_named("utf-8"),
                                   embi_encoding_for_label(EMBI_STR("shift_jis"))};
    for (size_t i = 0; i < 4; i++) {
        embi_url u = {0};
        EMB_REQUIRE(embi_url_parse_enc((embi_str){in, (uint32_t)strlen(in)}, nullptr, encs[i], &u));
        EMB_CHECK(strcmp(u.href, "http://h/?%C3%A9") == 0);
        embi_url_free(&u);
    }
}

EMB_TEST(url_query_encoding_utf8_for_ws_and_non_special)
{
    const embi_encoding *w = embi_encoding_named("windows-1252");
    const char *ins[] = {"ws://h/?é", "wss://h/?é", "foo://h/?é"};
    const char *outs[] = {"ws://h/?%C3%A9", "wss://h/?%C3%A9", "foo://h/?%C3%A9"};
    for (size_t i = 0; i < 3; i++) {
        embi_url u = {0};
        EMB_REQUIRE(embi_url_parse_enc((embi_str){ins[i], (uint32_t)strlen(ins[i])}, nullptr, w, &u));
        EMB_CHECK(strcmp(u.href, outs[i]) == 0);
        embi_url_free(&u);
    }
}

LOADER_TEST_MAIN()
