#include "emb_test.h"

#include "base/base.h"

#include <string.h>

static size_t g_seed_arena = 12345;

static size_t arena_rand(void)
{
    g_seed_arena ^= g_seed_arena << 13;
    g_seed_arena ^= g_seed_arena >> 7;
    g_seed_arena ^= g_seed_arena << 17;
    return g_seed_arena;
}

EMB_TEST(arena_alignment_for_all_powers_of_two)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    size_t aligns[] = {1, 2, 4, 8, 16, 32, 64};
    int ok = 1;
    for (int round = 0; round < 50; round++) {
        for (size_t k = 0; k < sizeof aligns / sizeof aligns[0]; k++) {
            size_t size = (arena_rand() % 100) + 1;
            void *p = embi_arena_alloc(&a, size, aligns[k]);
            if (((uintptr_t)p % aligns[k]) != 0) {
                ok = 0;
            }
        }
    }
    EMB_CHECK(ok);
    embi_arena_free(&a);
}

EMB_TEST(arena_zero_size_gives_unique_pointers)
{
    embi_arena a;
    embi_arena_init(&a, 256);
    void *p = embi_arena_alloc(&a, 0, 1);
    void *q = embi_arena_alloc(&a, 0, 1);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(p != q);
    embi_arena_free(&a);
}

EMB_TEST(arena_data_survives_block_switches)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 512);
    unsigned char *ptrs[200];
    for (int i = 0; i < 200; i++) {
        size_t n = (size_t)(i % 37) + 1;
        ptrs[i] = embi_arena_alloc(&a, n, 8);
        for (size_t k = 0; k < n; k++) {
            ptrs[i][k] = (unsigned char)(i + k);
        }
    }
    int ok = 1;
    for (int i = 0; i < 200; i++) {
        size_t n = (size_t)(i % 37) + 1;
        for (size_t k = 0; k < n; k++) {
            if (ptrs[i][k] != (unsigned char)(i + k)) {
                ok = 0;
            }
        }
    }
    EMB_CHECK(ok);
    EMB_CHECK(embi_mem_live() > base);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_oversized_gets_own_block)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 1024);
    void *big = embi_arena_alloc(&a, 1u << 20, 16);
    EMB_REQUIRE(big != nullptr);
    EMB_CHECK(a.first != nullptr);
    EMB_CHECK(a.first->cap >= (1u << 20));
    EMB_CHECK(((uintptr_t)big % 16) == 0);
    void *small = embi_arena_alloc(&a, 8, 8);
    EMB_CHECK(small != nullptr);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_mark_and_reset_to_rolls_back)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 256);
    unsigned char *keep = embi_arena_alloc(&a, 32, 8);
    keep[0] = 0xAB;
    embi_arena_pos m = embi_arena_mark(&a);
    unsigned char *tmp1 = embi_arena_alloc(&a, 100, 8);
    unsigned char *tmp2 = embi_arena_alloc(&a, 1000, 8); /* oversized, after the mark */
    (void)tmp1;
    (void)tmp2;
    embi_arena_reset_to(&a, m);
    EMB_CHECK(keep[0] == 0xAB);
    /* The space after the mark is handed out again from the same position. */
    unsigned char *again = embi_arena_alloc(&a, 100, 8);
    EMB_CHECK(again == tmp1);
    EMB_CHECK(a.first->next == nullptr || a.first->next == a.last);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_mark_before_oversized_keeps_it)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 256);
    (void)embi_arena_alloc(&a, 1000, 8); /* oversized, before the mark */
    embi_arena_pos m = embi_arena_mark(&a);
    (void)embi_arena_alloc(&a, 1000, 8);
    embi_arena_reset_to(&a, m);
    EMB_CHECK(a.last == m.block);
    EMB_CHECK(a.last->next == nullptr);
    EMB_CHECK(embi_mem_live() > base);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_reset_keeps_first_block)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 128);
    for (int i = 0; i < 50; i++) {
        (void)embi_arena_alloc(&a, 60, 8);
    }
    EMB_REQUIRE(a.first != nullptr);
    embi_arena_block *first = a.first;
    embi_arena_reset(&a);
    EMB_CHECK(a.first == first);
    EMB_CHECK(first->next == nullptr);
    EMB_CHECK_EQ_INT(first->used, 0);
    EMB_CHECK_EQ_INT(embi_mem_live(), base + sizeof *first + first->cap);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_mark_on_empty_arena_resets_to_empty)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 128);
    embi_arena_pos m = embi_arena_mark(&a);
    (void)embi_arena_alloc(&a, 300, 8);
    (void)embi_arena_alloc(&a, 10, 8);
    embi_arena_reset_to(&a, m);
    EMB_CHECK(a.first == nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
    embi_arena_free(&a);
}

/* FP2a: blocks made ahead by a thread. */

EMB_TEST(arena_prefault_data_survives_and_accounting_returns)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_arena_prefault_start(&a, 4096u * 64u);
    unsigned char *ptrs[300];
    for (int i = 0; i < 300; i++) {
        ptrs[i] = embi_arena_alloc(&a, 1000, 16); /* 4 per block: about 75 blocks, more than were planned */
        EMB_REQUIRE(ptrs[i] != nullptr);
        EMB_CHECK(((uintptr_t)ptrs[i] % 16) == 0);
        memset(ptrs[i], i & 0xFF, 1000);
    }
    int ok = 1;
    for (int i = 0; i < 300; i++) {
        ok = ok && ptrs[i][0] == (unsigned char)(i & 0xFF) && ptrs[i][999] == (unsigned char)(i & 0xFF);
    }
    EMB_CHECK(ok);
    embi_arena_free(&a);
    EMB_CHECK(a.pf == nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_prefault_stop_at_once_frees_every_block)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 32768);
    embi_arena_prefault_start(&a, 32768u * 200u);
    embi_arena_prefault_stop(&a); /* before anything was taken, the thread may be at any block */
    EMB_CHECK(a.pf == nullptr);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
    embi_arena_free(&a);
}

EMB_TEST(arena_prefault_ignored_after_the_first_allocation_and_for_nothing)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_arena_prefault_start(&a, 100); /* less than a block */
    EMB_CHECK(a.pf == nullptr);
    (void)embi_arena_alloc(&a, 16, 8);
    embi_arena_prefault_start(&a, 4096u * 8u); /* the arena is in use */
    EMB_CHECK(a.pf == nullptr);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arena_prefault_oversized_and_reset_work)
{
    size_t base = embi_mem_live();
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_arena_prefault_start(&a, 4096u * 16u);
    unsigned char *big = embi_arena_alloc(&a, 20000, 8); /* an oversized request gets its own block */
    EMB_REQUIRE(big != nullptr);
    memset(big, 7, 20000);
    for (int i = 0; i < 20; i++) {
        (void)embi_arena_alloc(&a, 3000, 8);
    }
    embi_arena_reset(&a);
    EMB_CHECK(a.first != nullptr);
    (void)embi_arena_alloc(&a, 100, 8);
    embi_arena_free(&a);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST_MAIN()

/* WPTASAN: the poison mode (a test hook) fills what an arena gives back with EMBI_ARENA_POISON and never hands it out again,
 * so a pointer that outlived a reset reads the poison deterministically. */
EMB_TEST(arena_poison_mode_poisons_and_keeps_what_a_reset_or_free_gives_back)
{
    embi_arena_debug_poison(true);
    EMB_CHECK(embi_arena_debug_poison_on());
    embi_arena a;
    embi_arena_init(&a, 256);
    uint64_t *x = embi_arena_alloc(&a, sizeof *x, 8);
    *x = 42;
    uint64_t *big = embi_arena_alloc(&a, 1000, 8); /* a block of its own */
    big[0] = 7;
    embi_arena_reset(&a);
    EMB_CHECK(embi_arena_debug_poisoned(x, sizeof *x));
    EMB_CHECK(embi_arena_debug_poisoned(big, 1000));
    uint64_t *y = embi_arena_alloc(&a, sizeof *y, 8);
    *y = 1;
    EMB_CHECK(y != x);                          /* a reset starts a new block */
    EMB_CHECK(embi_arena_debug_poisoned(x, 8)); /* the old one stays poisoned */
    embi_arena_pos m = embi_arena_mark(&a);
    uint64_t *z = embi_arena_alloc(&a, sizeof *z, 8);
    *z = 3;
    embi_arena_reset_to(&a, m);
    EMB_CHECK(embi_arena_debug_poisoned(z, sizeof *z));
    EMB_CHECK(!embi_arena_debug_poisoned(y, sizeof *y));
    embi_arena_free(&a);
    EMB_CHECK(embi_arena_debug_poisoned(y, sizeof *y));
    embi_arena_debug_poison(false);
    EMB_CHECK(!embi_arena_debug_poison_on());
    /* off: the arena reuses its first block as before */
    embi_arena_init(&a, 256);
    uint64_t *p = embi_arena_alloc(&a, sizeof *p, 8);
    embi_arena_reset(&a);
    uint64_t *q = embi_arena_alloc(&a, sizeof *q, 8);
    EMB_CHECK(p == q);
    embi_arena_free(&a);
}
