/* Op statistics (J5b): only meaningful in a build with the compile
 * definition EMBI_JS_OPSTATS; otherwise the test skips. Runs a small loop and checks the TSV
 * dump has a "single<TAB>ADD<TAB>n" line and a pair line. */
#include "es6_util.h"

#include <string.h>

#ifdef EMBI_JS_OPSTATS
#include "js/interp.h"

EMB_TEST(js_opstats_dump)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_opstats_reset();
    embi_js_value v = es6_run(e, "var s = 0; for (var i = 0; i < 100; i = i + 1) { s = s + i; } s;");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_opstats_dump(&sb);
    const char *t = embi_sb_cstr(&sb);
    EMB_CHECK(strstr(t, "single\tADD\t") != nullptr);
    EMB_CHECK(strstr(t, "pair\t") != nullptr);
    EMB_CHECK(strstr(t, "triple\t") != nullptr);
    embi_sb_free(&sb);
    es6_env_free(e);
}
#else
EMB_TEST(js_opstats_dump)
{
    EMB_SKIP("built without EMBI_JS_OPSTATS");
}
#endif

EMB_TEST_MAIN()
