#include "emb_test.h"

#include "base/base.h"

EMB_TEST(alloc_zero_size_is_unique_and_accounted)
{
    size_t base = embi_mem_live();
    void *a = embi_alloc(0);
    void *b = embi_alloc(0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(a != b);
    embi_free(a, 0);
    embi_free(b, 0);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(alloc_counts_live_bytes)
{
    size_t base = embi_mem_live();
    void *p = embi_alloc(1000);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + 1000);
    embi_free(p, 1000);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(calloc_returns_zeroed_memory)
{
    size_t base = embi_mem_live();
    unsigned char *p = embi_calloc(64, 4);
    EMB_REQUIRE(p != nullptr);
    int all_zero = 1;
    for (size_t i = 0; i < 256; i++) {
        if (p[i] != 0) {
            all_zero = 0;
        }
    }
    EMB_CHECK(all_zero);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + 256);
    embi_free(p, 256);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(realloc_preserves_contents_and_accounts_delta)
{
    size_t base = embi_mem_live();
    unsigned char *p = embi_alloc(16);
    for (int i = 0; i < 16; i++) {
        p[i] = (unsigned char)i;
    }
    p = embi_realloc(p, 16, 4096);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + 4096);
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        if (p[i] != (unsigned char)i) {
            ok = 0;
        }
    }
    EMB_CHECK(ok);
    p = embi_realloc(p, 4096, 8);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + 8);
    embi_free(p, 8);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(realloc_from_null_acts_as_alloc)
{
    size_t base = embi_mem_live();
    void *p = embi_realloc(nullptr, 0, 32);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + 32);
    embi_free(p, 32);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(free_null_is_noop)
{
    size_t base = embi_mem_live();
    embi_free(nullptr, 0);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(peak_tracks_maximum)
{
    size_t base = embi_mem_live();
    void *p = embi_alloc(5000);
    size_t peak_after_alloc = embi_mem_peak();
    EMB_CHECK(peak_after_alloc >= base + 5000);
    embi_free(p, 5000);
    EMB_CHECK(embi_mem_peak() >= peak_after_alloc);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
