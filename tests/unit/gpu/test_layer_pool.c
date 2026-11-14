#include "layer_rig.h"

#include "base/base.h"

/* Texture pool: size classes, reuse, the byte budget, and the in-use rule. A texture is 4 bytes
 * per texel, and a class is a power of two per side, at least 64 px. */

#define FMT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM

EMB_TEST(layer_pool_reuses_a_released_texture_of_the_same_class)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer a = {0};
    EMB_REQUIRE(embi_layer_acquire(k.pool, 100, 100, &a));
    EMB_CHECK(a.tex != nullptr);
    EMB_CHECK(a.w == 100 && a.h == 100);
    EMB_CHECK(a.tex_w == 128 && a.tex_h == 128);
    SDL_GPUTexture *first = a.tex;
    embi_layer_release(k.pool, &a);
    EMB_CHECK(a.tex == nullptr);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(k.pool), 1);

    /* 120 x 90 is in the same 128 x 128 class, so the same texture comes back. */
    embi_layer b = {0};
    EMB_REQUIRE(embi_layer_acquire(k.pool, 120, 90, &b));
    EMB_CHECK(b.tex == first);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(k.pool), 0);
    embi_layer_release(k.pool, &b);

    /* 200 x 200 is a 256 class: a new texture, and the 128 one stays idle. */
    embi_layer c = {0};
    EMB_REQUIRE(embi_layer_acquire(k.pool, 200, 200, &c));
    EMB_CHECK(c.tex != first);
    EMB_CHECK(c.tex_w == 256 && c.tex_h == 256);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(k.pool), 128 * 128 * 4 + 256 * 256 * 4);
    embi_layer_release(k.pool, &c);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(k.pool), 2);

    embi_layer_pool_trim(k.pool);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(k.pool), 0);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(k.pool), 0);
    layer_rig_close(&k);
}

EMB_TEST(layer_pool_budget_frees_the_oldest_idle_textures)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    /* A budget of one 128 x 128 texture. */
    embi_layer_pool *p = embi_layer_pool_create(k.g, FMT, 128 * 128 * 4);
    EMB_REQUIRE(p != nullptr);

    embi_layer a = {0};
    EMB_REQUIRE(embi_layer_acquire(p, 100, 100, &a));
    embi_layer_release(p, &a);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(p), 1); /* fits the budget: kept */

    /* A 256 texture does not fit next to the idle 128: the idle one is freed. */
    embi_layer b = {0};
    EMB_REQUIRE(embi_layer_acquire(p, 200, 200, &b));
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(p), 0);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(p), 256 * 256 * 4);

    /* Released over budget: the texture goes at once. */
    embi_layer_release(p, &b);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(p), 0);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(p), 0);
    embi_layer_pool_destroy(p);
    layer_rig_close(&k);
}

EMB_TEST(layer_pool_never_frees_a_texture_in_use)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer_pool *p = embi_layer_pool_create(k.g, FMT, 128 * 128 * 4);
    EMB_REQUIRE(p != nullptr);

    /* Both acquired: nothing is idle, so the pool goes over budget and keeps both. */
    embi_layer a = {0};
    embi_layer b = {0};
    EMB_REQUIRE(embi_layer_acquire(p, 100, 100, &a));
    EMB_REQUIRE(embi_layer_acquire(p, 200, 200, &b));
    EMB_CHECK(a.tex != nullptr && b.tex != nullptr);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(p), 128 * 128 * 4 + 256 * 256 * 4);

    /* Releasing a frees a (the oldest idle) to get under the budget, but b stays alive. */
    embi_layer_release(p, &a);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_idle(p), 0);
    EMB_CHECK(b.tex != nullptr);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(p), 256 * 256 * 4);
    embi_layer_release(p, &b);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(p), 0);
    embi_layer_pool_destroy(p);
    layer_rig_close(&k);
}

EMB_TEST(layer_pool_rejects_empty_and_oversized_layers)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer l = {0};
    EMB_CHECK(!embi_layer_acquire(k.pool, 0, 10, &l));
    EMB_CHECK(l.tex == nullptr);
    EMB_CHECK(!embi_layer_acquire(k.pool, 10, 0, &l));
    EMB_CHECK(!embi_layer_acquire(k.pool, 16385, 10, &l));
    EMB_CHECK(!embi_layer_acquire(k.pool, 10, 16385, &l));
    EMB_CHECK(l.tex == nullptr);
    EMB_CHECK_EQ_INT((int)embi_layer_pool_bytes(k.pool), 0);
    layer_rig_close(&k);
}

EMB_TEST(layer_pool_frees_everything_it_allocated)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const size_t before = embi_mem_live();
    embi_layer_pool *p = embi_layer_pool_create(k.g, FMT, (size_t)1 << 30);
    EMB_REQUIRE(p != nullptr);
    for (int i = 0; i < 20; i++) {
        embi_layer l = {0};
        EMB_REQUIRE(embi_layer_acquire(p, (uint32_t)(40 + i * 17), 50, &l));
        embi_layer_release(p, &l);
    }
    embi_layer_pool_destroy(p);
    EMB_CHECK(embi_mem_live() == before);
    layer_rig_close(&k);
}

EMB_RENDER_TEST_MAIN()
