/*
 * The public networking functions (include/ember/ember_net.h): with EMBER_NET they validate and accept; with
 * EMBER_NET=OFF they still link and return false.
 */
#include "emb_test.h"

#include <ember/ember.h>

EMB_TEST(public_api_allow_patterns)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
#ifdef EMBER_NET
    EMB_CHECK(emb_runtime_net_allow(rt, "example.com"));
    EMB_CHECK(emb_runtime_net_allow(rt, "example.com:8443"));
    EMB_CHECK(emb_runtime_net_allow(rt, "*.example.org"));
    EMB_CHECK(emb_runtime_net_allow(rt, "[::1]:8080"));
    EMB_CHECK(emb_runtime_net_allow(rt, "*"));
    EMB_CHECK(!emb_runtime_net_allow(rt, ""));
    EMB_CHECK(!emb_runtime_net_allow(rt, "exa mple.com"));
    EMB_CHECK(!emb_runtime_net_allow(rt, "example.com:99999"));
    EMB_CHECK(!emb_runtime_net_allow(rt, "http://example.com"));
    EMB_CHECK(!emb_runtime_net_allow(rt, "*.:80"));
    EMB_CHECK(!emb_runtime_net_allow(rt, nullptr));
    EMB_CHECK(!emb_runtime_net_add_root(rt, "not a certificate", 17));
    EMB_CHECK(!emb_runtime_net_add_root(rt, nullptr, 0));
#else
    EMB_CHECK(!emb_runtime_net_allow(rt, "example.com"));
    EMB_CHECK(!emb_runtime_net_allow(rt, "*"));
    EMB_CHECK(!emb_runtime_net_add_root(rt, "x", 1));
#endif
    emb_runtime_net_use_bundled_roots(rt, false);
    emb_runtime_net_set_user_agent(rt, "Test/1");
    EMB_CHECK(!emb_runtime_net_allow(nullptr, "example.com"));
    emb_runtime_destroy(rt);
}

EMB_TEST_MAIN()
