#include "emb_test.h"

#include "base/base.h"

EMB_TEST(log_levels_do_not_crash)
{
    embi_log_debug("debug %d", 1);
    embi_log_info("info %s", "x");
    embi_log_warn("warn %llu", (unsigned long long)2);
    embi_log_error("error %d", 3);
    EMB_CHECK(1);
}

EMB_TEST(assert_passes_when_true)
{
    EMBI_ASSERT(1 + 1 == 2);
    EMB_CHECK(1);
}

EMB_TEST_MAIN()
