#include "emb_test.h"

#include "dom/slab.h"

#define PROBE_COUNT 100000

static void *g_probe[PROBE_COUNT];

EMB_TEST(slab_reuses_freed_blocks_lifo)
{
    size_t base = embi_mem_live();
    embi_slab *s = embi_slab_new();
    void *p = embi_slab_alloc(s, 40);
    void *q = embi_slab_alloc(s, 40);
    EMB_CHECK(p != q);
    EMB_CHECK(embi_slab_owner(p) == s);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 2);
    embi_slab_free(p);
    void *r = embi_slab_alloc(s, 40);
    EMB_CHECK(r == p);
    embi_slab_free(q);
    embi_slab_free(r);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 0);
    EMB_CHECK_EQ_INT(embi_slab_live_bytes(s), 0);
    embi_slab_release(s);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(slab_payloads_are_aligned_and_writable)
{
    embi_slab *s = embi_slab_new();
    for (size_t n = 0; n <= 600; n += 7) {
        unsigned char *p = embi_slab_alloc(s, n);
        EMB_CHECK((uintptr_t)p % 16u == 0);
        SDL_memset(p, 0xA5, n);
        embi_slab_free(p);
    }
    embi_slab_release(s);
}

EMB_TEST(slab_big_blocks_are_freed_individually)
{
    size_t base = embi_mem_live();
    embi_slab *s = embi_slab_new();
    unsigned char *big = embi_slab_alloc(s, 100000);
    unsigned char *mid = embi_slab_alloc(s, 600);
    SDL_memset(big, 1, 100000);
    SDL_memset(mid, 2, 600);
    EMB_CHECK(embi_slab_owner(big) == s);
    EMB_CHECK(embi_slab_owner(mid) == s);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 2);
    embi_slab_free(big);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 1);
    EMB_CHECK(mid[599] == 2);
    embi_slab_free(mid);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 0);
    embi_slab_release(s);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(slab_many_blocks_span_chunks)
{
    size_t base = embi_mem_live();
    embi_slab *s = embi_slab_new();
    for (int i = 0; i < PROBE_COUNT; i++) {
        g_probe[i] = embi_slab_alloc(s, 64);
        *(int *)g_probe[i] = i;
    }
    bool distinct = true;
    for (int i = 1; i < PROBE_COUNT && distinct; i++) {
        distinct = *(int *)g_probe[i] == i;
    }
    EMB_CHECK(distinct);
    EMB_CHECK(embi_mem_live() > base + 6u * 1024u * 1024u);
    for (int i = 0; i < PROBE_COUNT; i += 2) {
        embi_slab_free(g_probe[i]);
        g_probe[i] = nullptr;
    }
    for (int i = 1; i < PROBE_COUNT; i += 2) {
        embi_slab_free(g_probe[i]);
        g_probe[i] = nullptr;
    }
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(s), 0);
    embi_slab_release(s);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(slab_keep_defers_release_until_last_reference)
{
    size_t base = embi_mem_live();
    embi_slab *a = embi_slab_new();
    embi_slab *b = embi_slab_new();
    unsigned char *p = embi_slab_alloc(a, 64);
    SDL_memset(p, 7, 64);
    embi_slab_keep(b, a);
    embi_slab_keep(b, a);
    EMB_CHECK_EQ_INT(embi_slab_kept_count(b), 1);
    embi_slab_release(a);
    EMB_CHECK(p[63] == 7);
    embi_slab_release(b);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(slab_keep_chain_releases_transitively)
{
    size_t base = embi_mem_live();
    embi_slab *a = embi_slab_new();
    embi_slab *b = embi_slab_new();
    embi_slab *c = embi_slab_new();
    (void)embi_slab_alloc(a, 100);
    (void)embi_slab_alloc(b, 100);
    (void)embi_slab_alloc(c, 100);
    embi_slab_keep(b, a);
    embi_slab_keep(c, b);
    embi_slab_release(a);
    embi_slab_release(b);
    EMB_CHECK(embi_mem_live() > base);
    embi_slab_release(c);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(slab_chunk_tail_is_reused_before_a_new_chunk)
{
    embi_slab *s = embi_slab_new();
    /* 496-byte payloads take 512-byte blocks: 127 fit in a 64 KiB chunk and leave 496 bytes. */
    unsigned char *last = nullptr;
    for (int i = 0; i < 127; i++) {
        last = embi_slab_alloc(s, 496);
    }
    size_t before = embi_slab_live_bytes(s);
    unsigned char *fresh = embi_slab_alloc(s, 496); /* starts a new chunk, splits the old tail */
    EMB_CHECK(fresh != last + 512);
    unsigned char *tail = embi_slab_alloc(s, 480); /* a 496-byte block: exactly the old tail */
    EMB_CHECK(tail == last + 512);
    EMB_CHECK_EQ_INT(embi_slab_live_bytes(s), before + 512 + 496);
    embi_slab_release(s);
}

EMB_TEST_MAIN()
