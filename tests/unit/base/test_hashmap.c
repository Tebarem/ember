#include "emb_test.h"

#include "base/base.h"

static uint64_t g_seed_map = 0x9E3779B97F4A7C15ull;

static uint64_t map_rand(void)
{
    g_seed_map ^= g_seed_map << 13;
    g_seed_map ^= g_seed_map >> 7;
    g_seed_map ^= g_seed_map << 17;
    return g_seed_map;
}

EMB_TEST(hashmap_basic_put_get_remove)
{
    embi_hashmap m;
    embi_hashmap_init(&m);
    uint64_t out = 0;
    EMB_CHECK(!embi_hashmap_get(&m, 7, &out));
    EMB_CHECK(embi_hashmap_put(&m, 7, 70));
    EMB_CHECK(!embi_hashmap_put(&m, 7, 71));
    EMB_CHECK(embi_hashmap_get(&m, 7, &out));
    EMB_CHECK(out == 71);
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 1);
    EMB_CHECK(embi_hashmap_remove(&m, 7));
    EMB_CHECK(!embi_hashmap_remove(&m, 7));
    EMB_CHECK(!embi_hashmap_get(&m, 7, nullptr));
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 0);
    embi_hashmap_free(&m);
}

EMB_TEST(hashmap_key_zero_and_max_work)
{
    embi_hashmap m;
    embi_hashmap_init(&m);
    EMB_CHECK(embi_hashmap_put(&m, 0, 1));
    EMB_CHECK(embi_hashmap_put(&m, UINT64_MAX, 2));
    uint64_t v = 0;
    EMB_CHECK(embi_hashmap_get(&m, 0, &v));
    EMB_CHECK(v == 1);
    EMB_CHECK(embi_hashmap_get(&m, UINT64_MAX, &v));
    EMB_CHECK(v == 2);
    embi_hashmap_free(&m);
}

EMB_TEST(hashmap_churn_matches_shadow_array)
{
    enum { KEYS = 1024, OPS = 100000 };
    static uint64_t shadow_val[KEYS];
    static bool shadow_has[KEYS];
    for (int i = 0; i < KEYS; i++) {
        shadow_has[i] = false;
        shadow_val[i] = 0;
    }

    embi_hashmap m;
    embi_hashmap_init(&m);
    int mismatches = 0;
    for (int op = 0; op < OPS; op++) {
        uint32_t i = (uint32_t)(map_rand() % KEYS);
        uint64_t key = (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;
        uint64_t roll = map_rand() % 3;
        if (roll < 2) {
            uint64_t val = map_rand();
            bool inserted = embi_hashmap_put(&m, key, val);
            if (inserted != !shadow_has[i]) {
                mismatches++;
            }
            shadow_has[i] = true;
            shadow_val[i] = val;
        } else {
            bool removed = embi_hashmap_remove(&m, key);
            if (removed != shadow_has[i]) {
                mismatches++;
            }
            shadow_has[i] = false;
        }
        uint64_t got = 0;
        bool found = embi_hashmap_get(&m, key, &got);
        if (found != shadow_has[i] || (found && got != shadow_val[i])) {
            mismatches++;
        }
    }
    EMB_CHECK_EQ_INT(mismatches, 0);

    uint32_t expected_len = 0;
    for (int i = 0; i < KEYS; i++) {
        expected_len += shadow_has[i] ? 1u : 0u;
    }
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), expected_len);

    /* Iteration must visit exactly the live keys. */
    uint32_t seen = 0;
    int iter_bad = 0;
    embi_hashmap_iter it = embi_hashmap_iter_begin(&m);
    uint64_t k = 0;
    uint64_t v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        seen++;
        bool matched = false;
        for (uint32_t i = 0; i < KEYS; i++) {
            if ((uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull == k) {
                matched = shadow_has[i] && shadow_val[i] == v;
                break;
            }
        }
        if (!matched) {
            iter_bad++;
        }
    }
    EMB_CHECK_EQ_INT(seen, expected_len);
    EMB_CHECK_EQ_INT(iter_bad, 0);
    embi_hashmap_free(&m);
}

EMB_TEST(hashmap_many_keys_grow_and_clear)
{
    size_t base = embi_mem_live();
    embi_hashmap m;
    embi_hashmap_init(&m);
    for (uint64_t k = 0; k < 50000; k++) {
        EMB_REQUIRE(embi_hashmap_put(&m, k, k * 2));
    }
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 50000);
    int ok = 1;
    for (uint64_t k = 0; k < 50000; k++) {
        uint64_t v = 0;
        if (!embi_hashmap_get(&m, k, &v) || v != k * 2) {
            ok = 0;
        }
    }
    EMB_CHECK(ok);
    embi_hashmap_clear(&m);
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 0);
    EMB_CHECK(!embi_hashmap_get(&m, 5, nullptr));
    EMB_CHECK(embi_hashmap_put(&m, 5, 9));
    embi_hashmap_free(&m);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(hash_bytes_depends_on_content_length_and_seed)
{
    const char a[] = "hello";
    const char b[] = "hellp";
    EMB_CHECK(embi_hash_bytes(a, 5, 0) != embi_hash_bytes(b, 5, 0));
    EMB_CHECK(embi_hash_bytes(a, 5, 0) != embi_hash_bytes(a, 4, 0));
    EMB_CHECK(embi_hash_bytes(a, 5, 0) != embi_hash_bytes(a, 5, 1));
    EMB_CHECK(embi_hash_bytes(a, 5, 0) == embi_hash_bytes(a, 5, 0));
    EMB_CHECK(embi_hash64(1) != embi_hash64(2));
}

EMB_TEST(hashmap_take_remove_if_insert_new_and_ref)
{
    embi_hashmap m;
    embi_hashmap_init(&m);
    uint64_t out = 0;
    EMB_CHECK(!embi_hashmap_take(&m, 5, &out));
    EMB_CHECK(!embi_hashmap_remove_if(&m, 5, 50));
    embi_hashmap_insert_new(&m, 5, 50);
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 1);
    EMB_CHECK(!embi_hashmap_remove_if(&m, 5, 51)); /* another value: stays */
    EMB_CHECK(embi_hashmap_get(&m, 5, &out) && out == 50);
    EMB_CHECK(embi_hashmap_remove_if(&m, 5, 50));
    EMB_CHECK(!embi_hashmap_get(&m, 5, nullptr));
    embi_hashmap_insert_new(&m, 6, 60);
    EMB_CHECK(embi_hashmap_take(&m, 6, &out) && out == 60);
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 0);
    bool added = false;
    uint64_t *v = embi_hashmap_ref(&m, 9, 90, &added);
    EMB_CHECK(added && *v == 90);
    *v |= 1;
    v = embi_hashmap_ref(&m, 9, 7, &added);
    EMB_CHECK(!added && *v == 91);
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), 1);
    embi_hashmap_free(&m);
}

/* The new operations agree with put, get and remove on a map that grows and shrinks. */
EMB_TEST(hashmap_new_operations_match_a_reference_array)
{
    enum { K = 3000 };
    static uint64_t ref[K];
    static bool present[K];
    embi_hashmap m;
    embi_hashmap_init(&m);
    for (int step = 0; step < 60000; step++) {
        uint64_t r = map_rand();
        uint32_t k = (uint32_t)(r % K);
        uint64_t key = (uint64_t)k * 0x10001u + 3u;
        switch ((r >> 32) % 5) {
        case 0:
            if (!present[k]) {
                embi_hashmap_insert_new(&m, key, r);
                present[k] = true;
                ref[k] = r;
            }
            break;
        case 1: {
            uint64_t out = 0;
            bool had = embi_hashmap_take(&m, key, &out);
            EMB_CHECK_EQ_INT(had, present[k]);
            if (had) {
                EMB_CHECK(out == ref[k]);
            }
            present[k] = false;
            break;
        }
        case 2: {
            bool hit = embi_hashmap_remove_if(&m, key, ref[k]);
            EMB_CHECK_EQ_INT(hit, present[k]);
            present[k] = false;
            break;
        }
        case 3: {
            bool added = false;
            uint64_t *v = embi_hashmap_ref(&m, key, r, &added);
            EMB_CHECK_EQ_INT(added, !present[k]);
            if (added) {
                present[k] = true;
                ref[k] = r;
            }
            EMB_CHECK(*v == ref[k]);
            break;
        }
        default: {
            uint64_t out = 0;
            EMB_CHECK_EQ_INT(embi_hashmap_get(&m, key, &out), present[k]);
            if (present[k]) {
                EMB_CHECK(out == ref[k]);
            }
            break;
        }
        }
    }
    uint32_t n = 0;
    for (int k = 0; k < K; k++) {
        n += present[k] ? 1u : 0u;
    }
    EMB_CHECK_EQ_INT(embi_hashmap_len(&m), n);
    embi_hashmap_free(&m);
}

/* The hash is the wyhash mum step over the full 128-bit product, whichever way the product is made. */
static uint64_t ref_mum(uint64_t a, uint64_t b)
{
    uint64_t ha = a >> 32, la = (uint32_t)a, hb = b >> 32, lb = (uint32_t)b;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32);
    uint64_t c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    return lo ^ hi;
}

EMB_TEST(hash64_equals_the_portable_multiply)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t x = map_rand();
        uint64_t want = ref_mum(ref_mum(x ^ 0xa0761d6478bd642full, 0xe7037ed1a0b428dbull) ^ 0x8ebc6af09c88c6e3ull,
                                x ^ 0x589965cc75374cc3ull);
        EMB_CHECK(embi_hash64(x) == want);
    }
}

EMB_TEST_MAIN()
