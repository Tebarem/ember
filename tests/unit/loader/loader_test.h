#pragma once
/*
 * Test main for the loader unit tests. It is EMB_TEST_MAIN with a leak check: every allocation
 * the tests make goes through the base allocator, so embi_mem_live() must be back at its value
 * before the first test when the run ends (the baseline is taken in main, before any test runs).
 * Windows ASan has no leak detector, so this is the check that runs there.
 */
#include "emb_test.h"

#include "base/base.h"

#include <stdio.h>

#define LOADER_TEST_MAIN()                                                                     \
    int main(int argc, char **argv)                                                            \
    {                                                                                          \
        (void)argc;                                                                            \
        (void)argv;                                                                            \
        size_t loader_live_base = embi_mem_live();                                             \
        int loader_rc = emb_test_run_all();                                                    \
        size_t loader_live_end = embi_mem_live();                                              \
        if (loader_live_end != loader_live_base) {                                             \
            fprintf(stderr, "LEAK: %zu bytes still live after the run (baseline %zu)\n",       \
                    loader_live_end, loader_live_base);                                        \
            loader_rc = 1;                                                                     \
        }                                                                                      \
        return loader_rc;                                                                      \
    }
