/*
 * Storage backend, memory-only: the Storage operations (length, key, getItem, setItem, removeItem,
 * clear), insertion order, UTF-16 keys with lone surrogates, origin and session separation, the
 * quota, and the memory accounting. Nothing here touches the disk.
 */
#include "loader_test.h"

#include "loader/storage.h"

#include <stdio.h>
#include <string.h>

/* Fills buf with the ASCII string and returns a view on it. buf must hold strlen(s) units. */
static embi_u16view units_of(uint16_t *buf, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint16_t)(unsigned char)s[i];
    }
    return (embi_u16view){buf, (uint32_t)n};
}

static bool view_is(embi_u16view v, const char *s)
{
    size_t n = strlen(s);
    if (v.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (v.p[i] != (uint16_t)(unsigned char)s[i]) {
            return false;
        }
    }
    return true;
}

static embi_str origin_of(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

EMB_TEST(storage_memory_basic_operations)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    EMB_REQUIRE(st != nullptr);
    EMB_CHECK(!embi_storage_is_persistent(st));
    embi_storage_area *a = embi_storage_local(st, origin_of("app://bundle"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 0);

    uint16_t kb[16], vb[16], k2[16], v2[16];
    embi_u16view out = {nullptr, 0};
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "alpha"), units_of(vb, "1")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_set(a, units_of(k2, "beta"), units_of(v2, "two")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 2);

    EMB_CHECK(embi_storage_area_get(a, units_of(kb, "alpha"), &out));
    EMB_CHECK(view_is(out, "1"));
    EMB_CHECK(!embi_storage_area_get(a, units_of(kb, "gamma"), &out));

    /* Overwriting keeps the key's position. */
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "alpha"), units_of(vb, "replaced")) ==
              EMBI_STORAGE_OK);
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 2);
    EMB_CHECK(embi_storage_area_key(a, 0, &out));
    EMB_CHECK(view_is(out, "alpha"));
    EMB_CHECK(embi_storage_area_get(a, units_of(kb, "alpha"), &out));
    EMB_CHECK(view_is(out, "replaced"));

    /* Removing the first key shifts the others down, in order. */
    embi_storage_area_remove(a, units_of(kb, "alpha"));
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 1);
    EMB_CHECK(embi_storage_area_key(a, 0, &out));
    EMB_CHECK(view_is(out, "beta"));
    EMB_CHECK(!embi_storage_area_key(a, 1, &out));
    embi_storage_area_remove(a, units_of(kb, "missing")); /* absent: no effect */
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 1);

    embi_storage_area_clear(a);
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 0);
    EMB_CHECK(embi_storage_area_units(a) == 0);
    embi_storage_close(st);
}

EMB_TEST(storage_empty_and_unicode_keys)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    EMB_REQUIRE(st != nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://u"));

    /* The empty key and empty value are valid, and lone surrogates are kept as code units. */
    embi_u16view none = {nullptr, 0};
    EMB_CHECK(embi_storage_area_set(a, none, none) == EMBI_STORAGE_OK);
    embi_u16view out = {nullptr, 0};
    EMB_CHECK(embi_storage_area_get(a, none, &out));
    EMB_CHECK_EQ_INT(out.len, 0);

    uint16_t lone[3] = {0xD800, 'x', 0xDFFF};
    uint16_t val[2] = {0xFFFD, 0x20AC};
    embi_u16view key = {lone, 3};
    EMB_CHECK(embi_storage_area_set(a, key, (embi_u16view){val, 2}) == EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_get(a, key, &out));
    EMB_CHECK_EQ_INT(out.len, 2);
    EMB_CHECK(out.p[0] == 0xFFFD && out.p[1] == 0x20AC);
    /* A key that differs only in a surrogate is a different key. */
    uint16_t other[3] = {0xDC00, 'x', 0xDFFF};
    EMB_CHECK(!embi_storage_area_get(a, (embi_u16view){other, 3}, &out));
    embi_storage_close(st);
}

EMB_TEST(storage_origins_and_session_are_separate)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    EMB_REQUIRE(st != nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://a"));
    embi_storage_area *b = embi_storage_local(st, origin_of("app://b"));
    EMB_CHECK(a != b);
    EMB_CHECK(embi_storage_local(st, origin_of("app://a")) == a); /* same origin, same area */

    uint16_t kb[8], vb[8];
    EMB_CHECK(embi_storage_area_set(a, units_of(kb, "k"), units_of(vb, "a")) == EMBI_STORAGE_OK);
    EMB_CHECK_EQ_INT(embi_storage_area_len(b), 0);

    embi_storage_area *s = embi_storage_session_new();
    EMB_CHECK(embi_storage_area_set(s, units_of(kb, "k"), units_of(vb, "s")) == EMBI_STORAGE_OK);
    embi_u16view out = {nullptr, 0};
    EMB_CHECK(embi_storage_area_get(a, units_of(kb, "k"), &out));
    EMB_CHECK(view_is(out, "a"));
    EMB_CHECK(embi_storage_area_get(s, units_of(kb, "k"), &out));
    EMB_CHECK(view_is(out, "s"));
    embi_storage_session_free(s);
    embi_storage_close(st);
}

EMB_TEST(storage_quota_counts_utf16_units)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    EMB_REQUIRE(st != nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://q"));

    /* One key unit plus a value of quota-1 units fills the quota exactly. */
    size_t big = (size_t)EMBI_STORAGE_QUOTA_UNITS - 1;
    uint16_t *value = embi_alloc(big * sizeof(uint16_t));
    for (size_t i = 0; i < big; i++) {
        value[i] = 'v';
    }
    uint16_t kb[4];
    embi_u16view key = units_of(kb, "k");
    EMB_CHECK(embi_storage_area_set(a, key, (embi_u16view){value, (uint32_t)big}) ==
              EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_units(a) == EMBI_STORAGE_QUOTA_UNITS);

    /* Any new key, even an empty one, is over the quota. */
    uint16_t kz[8];
    EMB_CHECK(embi_storage_area_set(a, units_of(kz, "z"), (embi_u16view){nullptr, 0}) ==
              EMBI_STORAGE_QUOTA);
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 1);

    /* Replacing the value with a shorter one frees room, and the failed write changed nothing. */
    EMB_CHECK(embi_storage_area_set(a, key, units_of(kz, "short")) == EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_units(a) == 1 + 5);
    uint16_t vy[4];
    EMB_CHECK(embi_storage_area_set(a, units_of(kz, "z"), units_of(vy, "y")) == EMBI_STORAGE_OK);
    EMB_CHECK(embi_storage_area_units(a) == 1 + 5 + 2);

    embi_free(value, big * sizeof(uint16_t));
    embi_storage_close(st);
}

EMB_TEST(storage_session_and_local_memory_is_released)
{
    size_t before = embi_mem_live();
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    embi_storage_area *a = embi_storage_local(st, origin_of("app://m"));
    uint16_t kb[8], vb[8];
    for (int i = 0; i < 300; i++) {
        kb[0] = (uint16_t)('a' + (i % 26));
        vb[0] = (uint16_t)('0' + (i % 10));
        (void)embi_storage_area_set(a, (embi_u16view){kb, 1}, (embi_u16view){vb, 1});
    }
    /* 2000 distinct keys "k" + four digits push the index through several rebuilds. */
    uint16_t k[5];
    for (int i = 0; i < 2000; i++) {
        k[0] = 'k';
        k[1] = (uint16_t)('0' + (i / 1000) % 10);
        k[2] = (uint16_t)('0' + (i / 100) % 10);
        k[3] = (uint16_t)('0' + (i / 10) % 10);
        k[4] = (uint16_t)('0' + i % 10);
        EMB_CHECK(embi_storage_area_set(a, (embi_u16view){k, 5}, (embi_u16view){vb, 1}) ==
                  EMBI_STORAGE_OK);
    }
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 2000 + 26);
    embi_u16view out = {nullptr, 0};
    uint16_t probe[5] = {'k', '0', '1', '2', '3'};
    EMB_CHECK(embi_storage_area_get(a, (embi_u16view){probe, 5}, &out));
    /* Removing from the middle keeps the rest findable. */
    uint16_t mid[5] = {'k', '0', '5', '0', '0'};
    embi_storage_area_remove(a, (embi_u16view){mid, 5});
    EMB_CHECK(!embi_storage_area_get(a, (embi_u16view){mid, 5}, &out));
    EMB_CHECK(embi_storage_area_get(a, (embi_u16view){probe, 5}, &out));
    EMB_CHECK_EQ_INT(embi_storage_area_len(a), 2000 + 26 - 1);

    embi_storage_close(st);
    EMB_CHECK(embi_mem_live() == before);
}

/* Churn against a model: random sets and removes over a few hundred keys, with the Storage
 * operations checked against a plain model every thousand steps. Removals leave tombstones and
 * trigger compactions, so the probe runs and the insertion order are exercised well past the
 * simple cases above. */
#define CHURN_IDS 600u
#define CHURN_OPS 20000u

static bool units_equal(embi_u16view v, const uint16_t *p, uint32_t n)
{
    if (v.len != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (v.p[i] != p[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t churn_key(uint16_t *buf, uint32_t id)
{
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "k%u", (unsigned)id);
    for (int i = 0; i < n; i++) {
        buf[i] = (uint16_t)(unsigned char)tmp[i];
    }
    return (uint32_t)n;
}

typedef struct churn_model {
    uint32_t order[CHURN_IDS]; /* live ids in insertion order */
    uint32_t n;
    bool present[CHURN_IDS];
    uint16_t val[CHURN_IDS];
} churn_model;

static bool churn_check(const embi_storage_area *a, const churn_model *m)
{
    if (embi_storage_area_len(a) != m->n) {
        return false;
    }
    for (uint32_t i = 0; i < m->n; i++) {
        uint16_t kb[16];
        uint32_t kl = churn_key(kb, m->order[i]);
        embi_u16view k = {nullptr, 0};
        if (!embi_storage_area_key(a, i, &k) || !units_equal(k, kb, kl)) {
            return false;
        }
    }
    for (uint32_t id = 0; id < CHURN_IDS; id++) {
        uint16_t kb[16];
        uint32_t kl = churn_key(kb, id);
        embi_u16view v = {nullptr, 0};
        bool got = embi_storage_area_get(a, (embi_u16view){kb, kl}, &v);
        if (got != m->present[id]) {
            return false;
        }
        if (got && (v.len != 1 || v.p[0] != m->val[id])) {
            return false;
        }
    }
    return true;
}

EMB_TEST(storage_memory_churn_matches_a_model)
{
    static churn_model m;
    memset(&m, 0, sizeof m);
    embi_storage_area *a = embi_storage_session_new();
    uint32_t seed = 12345u;
    for (uint32_t op = 0; op < CHURN_OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t id = (seed >> 8) % CHURN_IDS;
        uint16_t kb[16];
        uint32_t kl = churn_key(kb, id);
        if (m.present[id] && ((seed >> 4) & 1u) != 0) {
            embi_storage_area_remove(a, (embi_u16view){kb, kl});
            uint32_t pos = 0;
            while (m.order[pos] != id) {
                pos++;
            }
            memmove(&m.order[pos], &m.order[pos + 1], (m.n - pos - 1) * sizeof m.order[0]);
            m.n--;
            m.present[id] = false;
        } else {
            uint16_t vb[1] = {(uint16_t)(0x100u + (op & 0xFFu))};
            EMB_CHECK(embi_storage_area_set(a, (embi_u16view){kb, kl}, (embi_u16view){vb, 1}) ==
                      EMBI_STORAGE_OK);
            if (!m.present[id]) {
                m.order[m.n++] = id;
                m.present[id] = true;
            }
            m.val[id] = vb[0];
        }
        if (op % 1000u == 999u) {
            EMB_CHECK(churn_check(a, &m));
        }
    }
    EMB_CHECK(churn_check(a, &m));
    embi_storage_session_free(a);
}

LOADER_TEST_MAIN()
