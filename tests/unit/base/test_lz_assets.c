/*
 * The compressed assets of an EMBER_COMPRESS_ASSETS build (SZ1): the UA style sheet and a prelude script, as the build
 * stored them (the .lz.h files under build/gen/ember_lz), expand to exactly the bytes of their source files. Built only when the assets
 * are compressed (tests/CMakeLists.txt); the shader blobs are checked by tests/unit/gpu/test_blob_lz.c.
 */
#include "emb_test.h"

#include "base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ember_lz/ua/html.css.lz.h"
#include "ember_lz/js/streams.js.lz.h"

static uint8_t *read_file(const char *rel, size_t *size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (buf == nullptr || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static bool expands_to_file(const unsigned char *packed, size_t packed_size, const char *rel)
{
    size_t want_n = 0;
    uint8_t *want = read_file(rel, &want_n);
    if (want == nullptr) {
        return false;
    }
    size_t got_n = 0;
    uint8_t *got = embi_lz_unpack(packed, packed_size, &got_n);
    bool ok = got != nullptr && got_n == want_n && memcmp(got, want, want_n) == 0 && got[got_n] == 0;
    if (got != nullptr) {
        embi_free(got, got_n + 1);
    }
    free(want);
    return ok;
}

EMB_TEST(ua_sheet_expands_to_the_source_file_byte_for_byte)
{
    EMB_CHECK(expands_to_file(embi_lz_ua_html_css, sizeof embi_lz_ua_html_css, "src/style/ua/html.css"));
}

EMB_TEST(prelude_script_expands_to_the_source_file_byte_for_byte)
{
    EMB_CHECK(expands_to_file(embi_lz_js_streams_js, sizeof embi_lz_js_streams_js, "src/bindings/js/streams.js"));
}

EMB_TEST_MAIN()
