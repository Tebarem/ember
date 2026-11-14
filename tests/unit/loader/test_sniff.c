/*
 * Document encoding sniffing (WHATWG HTML 13.2.3.2): BOM, transport charset, the meta prescan,
 * the windows-1252 fallback, and the UTF-16 and unsupported-label rules.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

static const char *sniff_name(const char *bytes, size_t n, const char *transport)
{
    const embi_encoding *e = embi_sniff_encoding((const unsigned char *)bytes, n,
                                                 (embi_str){transport, transport ? (uint32_t)strlen(transport) : 0});
    return e->name;
}

EMB_TEST(sniff_bom_then_transport_then_prescan)
{
    static const char utf8_bom[] = "\xEF\xBB\xBF<meta charset=koi8-r>";
    EMB_CHECK(strcmp(sniff_name(utf8_bom, sizeof utf8_bom - 1, "windows-1251"), "utf-8") == 0);

    static const char le_bom[] = "\xFF\xFEx";
    EMB_CHECK(strcmp(sniff_name(le_bom, sizeof le_bom - 1, nullptr), "utf-16le") == 0);

    /* The transport charset beats a meta charset. */
    static const char meta[] = "<html><meta charset=\"iso-8859-2\">";
    EMB_CHECK(strcmp(sniff_name(meta, sizeof meta - 1, nullptr), "iso-8859-2") == 0);
    EMB_CHECK(strcmp(sniff_name(meta, sizeof meta - 1, "windows-1251"), "windows-1251") == 0);

    /* A recognized but unsupported transport label gives windows-1252, not the meta tag. */
    EMB_CHECK(strcmp(sniff_name(meta, sizeof meta - 1, "shift_jis"), "windows-1252") == 0);
    /* An unknown label falls through to the prescan. */
    EMB_CHECK(strcmp(sniff_name(meta, sizeof meta - 1, "bogus-label"), "iso-8859-2") == 0);
    EMB_CHECK(strcmp(sniff_name("plain", 5, "shift_jis"), "windows-1252") == 0);
}

EMB_TEST(sniff_meta_prescan_forms)
{
    static const char http_equiv[] =
        "<!doctype html><meta http-equiv=\"Content-Type\" content=\"text/html; charset=koi8-r\">";
    EMB_CHECK(strcmp(sniff_name(http_equiv, sizeof http_equiv - 1, nullptr), "koi8-r") == 0);

    /* content= without http-equiv=content-type is not believed. */
    static const char no_pragma[] = "<meta content=\"text/html; charset=koi8-r\">";
    EMB_CHECK(strcmp(sniff_name(no_pragma, sizeof no_pragma - 1, nullptr), "windows-1252") == 0);

    /* A comment hides a meta element. */
    static const char commented[] = "<!-- <meta charset=koi8-r> --><p>";
    EMB_CHECK(strcmp(sniff_name(commented, sizeof commented - 1, nullptr), "windows-1252") == 0);

    /* A UTF-16 declaration becomes UTF-8. */
    static const char utf16_meta[] = "<meta charset=utf-16>";
    EMB_CHECK(strcmp(sniff_name(utf16_meta, sizeof utf16_meta - 1, nullptr), "utf-8") == 0);

    /* An unquoted value ends at a space or '>'. */
    static const char unquoted[] = "<meta charset=shift_jis >";
    EMB_CHECK(strcmp(sniff_name(unquoted, sizeof unquoted - 1, nullptr), "windows-1252") == 0);

    /* A charset attribute overrides one from content, in either order. */
    static const char override1[] =
        "<meta http-equiv=content-type content=\"text/html; charset=koi8-r\" charset=iso-8859-2>";
    EMB_CHECK(strcmp(sniff_name(override1, sizeof override1 - 1, nullptr), "iso-8859-2") == 0);
    static const char override2[] = "<meta charset=iso-8859-2 content=\"charset=koi8-r\">";
    EMB_CHECK(strcmp(sniff_name(override2, sizeof override2 - 1, nullptr), "iso-8859-2") == 0);

    /* A charset attribute with an unknown label replaces the one from content, so the element is
     * skipped; an unknown label in content does not count as found, so a later charset= wins. */
    static const char unknown_attr[] =
        "<meta http-equiv=content-type content=\"text/html; charset=koi8-r\" charset=bogus-label>";
    EMB_CHECK(strcmp(sniff_name(unknown_attr, sizeof unknown_attr - 1, nullptr), "windows-1252") == 0);
    /* A charset attribute seen first with an unknown label also blocks a later content attribute. */
    static const char unknown_then_content[] =
        "<meta charset=bogus-label http-equiv=content-type content=\"text/html; charset=koi8-r\">";
    EMB_CHECK(strcmp(sniff_name(unknown_then_content, sizeof unknown_then_content - 1, nullptr), "windows-1252") == 0);
    static const char unknown_content[] =
        "<meta http-equiv=content-type content=\"charset=bogus-label\" charset=koi8-r>";
    EMB_CHECK(strcmp(sniff_name(unknown_content, sizeof unknown_content - 1, nullptr), "koi8-r") == 0);

    /* Attributes of other tags are skipped as attributes, so a quoted "<meta" is not a tag. */
    static const char quoted[] = "<div title=\"<meta charset=koi8-r>\"><p>";
    EMB_CHECK(strcmp(sniff_name(quoted, sizeof quoted - 1, nullptr), "windows-1252") == 0);
    static const char endtag[] = "</p data=\"<meta charset=koi8-r>\">";
    EMB_CHECK(strcmp(sniff_name(endtag, sizeof endtag - 1, nullptr), "windows-1252") == 0);

    /* "<!-->" is a complete comment. */
    static const char empty_comment[] = "<!--><meta charset=koi8-r>";
    EMB_CHECK(strcmp(sniff_name(empty_comment, sizeof empty_comment - 1, nullptr), "koi8-r") == 0);

    /* The prescan looks at 1024 bytes only. */
    char big[1100];
    memset(big, ' ', sizeof big);
    memcpy(big + 1050, "<meta charset=koi8-r>", 21);
    EMB_CHECK(strcmp(sniff_name(big, sizeof big, nullptr), "windows-1252") == 0);
}

EMB_TEST(sniff_decode_document_drops_bom_and_maps_defaults)
{
    static const char doc[] = "\xEF\xBB\xBFhi";
    embi_sb out = {nullptr, 0, 0};
    const embi_encoding *used = nullptr;
    embi_decode_document((const unsigned char *)doc, sizeof doc - 1, EMBI_STR(""), &out, &used);
    EMB_CHECK(used != nullptr && strcmp(used->name, "utf-8") == 0);
    EMB_CHECK(out.len == 2 && memcmp(out.data, "hi", 2) == 0);
    embi_sb_free(&out);

    /* Without a declaration, 0x80 is the euro sign (windows-1252). */
    static const unsigned char legacy[] = {0x80, 'x'};
    out = (embi_sb){nullptr, 0, 0};
    embi_decode_document(legacy, sizeof legacy, EMBI_STR(""), &out, &used);
    EMB_CHECK(strcmp(used->name, "windows-1252") == 0);
    EMB_CHECK(out.len == 4 && memcmp(out.data, "\xE2\x82\xAC" "x", 4) == 0);
    embi_sb_free(&out);
}

LOADER_TEST_MAIN()
