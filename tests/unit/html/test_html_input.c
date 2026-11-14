/*
 * The HTML input hook: a document's bytes are decoded to UTF-8 with the sniffing algorithm before
 * the parser sees them. The parser itself takes UTF-8 and is not exercised here.
 */
#include "emb_test.h"

#include "base/base.h"
#include "html/input.h"

EMB_TEST(html_input_decodes_meta_declared_encoding)
{
    static const unsigned char doc[] = "<!doctype html><meta charset=windows-1252><p>\xE9\x80";
    embi_sb out = {nullptr, 0, 0};
    const embi_encoding *used = nullptr;
    embi_html_decode_bytes(doc, sizeof doc - 1, EMBI_STR(""), &out, &used);
    EMB_REQUIRE(used != nullptr);
    EMB_CHECK(strcmp(used->name, "windows-1252") == 0);
    static const char want[] = "<!doctype html><meta charset=windows-1252><p>\xC3\xA9\xE2\x82\xAC";
    EMB_CHECK(out.len == sizeof want - 1 && memcmp(out.data, want, out.len) == 0);
    embi_sb_free(&out);
}

EMB_TEST(html_input_utf8_bom_is_dropped)
{
    static const unsigned char doc[] = "\xEF\xBB\xBF<p>hi";
    embi_sb out = {nullptr, 0, 0};
    const embi_encoding *used = nullptr;
    embi_html_decode_bytes(doc, sizeof doc - 1, EMBI_STR("utf-8"), &out, &used);
    EMB_CHECK(out.len == 5 && memcmp(out.data, "<p>hi", 5) == 0);
    embi_sb_free(&out);
}

EMB_TEST_MAIN()
