#include "emb_test.h"

#include "base/base.h"

typedef EMBI_VEC(int) int_vec;

EMB_TEST(vec_push_pop_at)
{
    int_vec v = {0};
    for (int i = 0; i < 100; i++) {
        embi_vec_push(&v, i * 3);
    }
    EMB_CHECK_EQ_INT(v.len, 100);
    EMB_CHECK_EQ_INT(embi_vec_at(&v, 0), 0);
    EMB_CHECK_EQ_INT(embi_vec_at(&v, 99), 297);
    int last = embi_vec_pop(&v);
    EMB_CHECK_EQ_INT(last, 297);
    EMB_CHECK_EQ_INT(v.len, 99);
    embi_vec_free(&v);
    EMB_CHECK(v.data == nullptr);
    EMB_CHECK_EQ_INT(v.cap, 0);
}

EMB_TEST(vec_growth_is_one_and_a_half_min_eight)
{
    int_vec v = {0};
    embi_vec_push(&v, 1);
    EMB_CHECK_EQ_INT(v.cap, 8);
    for (int i = 1; i < 8; i++) {
        embi_vec_push(&v, i);
    }
    EMB_CHECK_EQ_INT(v.cap, 8);
    embi_vec_push(&v, 8);
    EMB_CHECK_EQ_INT(v.cap, 12);
    embi_vec_push(&v, 9);
    for (int i = 10; i < 13; i++) {
        embi_vec_push(&v, i);
    }
    EMB_CHECK_EQ_INT(v.cap, 18);
    embi_vec_free(&v);
}

EMB_TEST(vec_reserve_grows_to_need)
{
    int_vec v = {0};
    embi_vec_reserve(&v, 100);
    EMB_CHECK(v.cap >= 100);
    EMB_CHECK_EQ_INT(v.len, 0);
    embi_vec_free(&v);
}

EMB_TEST(vec_free_returns_memory_to_baseline)
{
    size_t base = embi_mem_live();
    int_vec v = {0};
    for (int i = 0; i < 1000; i++) {
        embi_vec_push(&v, i);
    }
    EMB_CHECK(embi_mem_live() > base);
    embi_vec_free(&v);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(vec_clear_keeps_capacity)
{
    int_vec v = {0};
    for (int i = 0; i < 20; i++) {
        embi_vec_push(&v, i);
    }
    uint32_t cap = v.cap;
    embi_vec_clear(&v);
    EMB_CHECK_EQ_INT(v.len, 0);
    EMB_CHECK_EQ_INT(v.cap, cap);
    embi_vec_free(&v);
}

EMB_TEST_MAIN()
