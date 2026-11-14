#include "emb_test.h"

#include <ember/ember.h>

EMB_TEST(version_string_matches_macros)
{
    EMB_CHECK_EQ_STR(emb_version_string(), EMB_VERSION_STRING);
    EMB_CHECK_EQ_STR(EMB_VERSION_STRING, "0.0.1");
}

EMB_TEST(version_number_packs_fields)
{
    uint32_t v = emb_version();
    EMB_CHECK_EQ_INT((v >> 16) & 0xFFu, EMB_VERSION_MAJOR);
    EMB_CHECK_EQ_INT((v >> 8) & 0xFFu, EMB_VERSION_MINOR);
    EMB_CHECK_EQ_INT(v & 0xFFu, EMB_VERSION_PATCH);
}

EMB_TEST_MAIN()
