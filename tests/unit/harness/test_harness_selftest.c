#include "emb_test.h"

#include "ember_shaders/selftest.h"
#include "gpu/gpu.h"

/* SPIR-V magic 0x07230203 and the DXBC container tag, as little-endian words. */
#define EMB_TEST_SPIRV_MAGIC 0x07230203u
#define EMB_TEST_DXBC_MAGIC 0x43425844u

/* A built format must carry code, an entry name and the container's magic number. An unbuilt
 * format must be the empty placeholder. */
static void check_blob(const embi_shader_blob *b, int built, uint32_t magic)
{
    if (!built) {
        EMB_CHECK(b->code == nullptr);
        EMB_CHECK(b->size == 0);
        EMB_CHECK(b->entry == nullptr);
        return;
    }
    EMB_REQUIRE(b->code != nullptr);
    EMB_REQUIRE(b->size >= 4);
    EMB_REQUIRE(b->entry != nullptr);
    embi_gpu_blob_view v = embi_gpu_blob_open(b); /* the code itself, or the expansion of a packed blob */
    EMB_REQUIRE(v.code != nullptr);
    EMB_REQUIRE(v.size >= 4);
    if (magic != 0) {
        uint32_t first = (uint32_t)v.code[0] | ((uint32_t)v.code[1] << 8) |
                         ((uint32_t)v.code[2] << 16) | ((uint32_t)v.code[3] << 24);
        EMB_CHECK(first == magic);
    }
    embi_gpu_blob_close(&v);
}

EMB_TEST(harness_checks_pass)
{
    EMB_CHECK(1 == 1);
    EMB_CHECK_EQ_INT(3, 3);
    EMB_CHECK_EQ_INT(-1, (int)-1);
    EMB_CHECK_EQ_STR("abc", "abc");
    EMB_REQUIRE(sizeof(int) >= 2);
    EMB_CHECK(true);
}

EMB_TEST(harness_skip_is_reported)
{
    EMB_SKIP("selftest: deliberate skip");
    EMB_CHECK(0);
}

EMB_TEST(shader_selftest_blobs_are_embedded)
{
    const embi_shader_program *p = &embi_shader_selftest;
    EMB_CHECK(EMBI_SHADER_FMT_SPIRV || EMBI_SHADER_FMT_DXIL || EMBI_SHADER_FMT_MSL);

    check_blob(&p->vs.spirv, EMBI_SHADER_FMT_SPIRV, EMB_TEST_SPIRV_MAGIC);
    check_blob(&p->vs.dxil, EMBI_SHADER_FMT_DXIL, EMB_TEST_DXBC_MAGIC);
    check_blob(&p->vs.msl, EMBI_SHADER_FMT_MSL, 0);
    check_blob(&p->fs.spirv, EMBI_SHADER_FMT_SPIRV, EMB_TEST_SPIRV_MAGIC);
    check_blob(&p->fs.dxil, EMBI_SHADER_FMT_DXIL, EMB_TEST_DXBC_MAGIC);
    check_blob(&p->fs.msl, EMBI_SHADER_FMT_MSL, 0);

    if (EMBI_SHADER_FMT_SPIRV) {
        EMB_CHECK_EQ_STR(p->vs.spirv.entry, "vsMain");
        EMB_CHECK_EQ_STR(p->fs.spirv.entry, "fsMain");
    }
    EMB_CHECK_EQ_INT(p->vs.num_uniform_buffers, 0);
    EMB_CHECK_EQ_INT(p->fs.num_samplers, 0);
}

EMB_TEST_MAIN()
