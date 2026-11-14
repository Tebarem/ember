/*
 * The shader blobs of an EMBER_COMPRESS_ASSETS build (SZ1) are stored packed and embi_gpu_blob_open expands them to the
 * driver's code: SPIR-V starts with its magic number, DXIL is a DXBC container, and the size is the header's. No device is
 * needed. Built only when the assets are compressed (tests/CMakeLists.txt).
 */
#include "emb_test.h"

#include "ember_shaders/formats.h"
#include "ember_shaders/box.h"
#include "gpu/gpu.h"

static void check_blob(const embi_shader_blob *b, const char *kind)
{
    if (b->code == nullptr) {
        return; /* format not built */
    }
    EMB_CHECK(b->packed);
    embi_gpu_blob_view v = embi_gpu_blob_open(b);
    EMB_REQUIRE(v.code != nullptr);
    EMB_CHECK(v.size > 4);
    EMB_CHECK(v.size > b->size / 2); /* an expansion, not the stream */
    if (strcmp(kind, "spirv") == 0) {
        EMB_CHECK(v.size % 4 == 0 && v.code[0] == 0x03 && v.code[1] == 0x02 && v.code[2] == 0x23 && v.code[3] == 0x07);
    } else if (strcmp(kind, "dxil") == 0) {
        EMB_CHECK(memcmp(v.code, "DXBC", 4) == 0);
    }
    embi_gpu_blob_close(&v);
}

EMB_TEST(packed_shader_blobs_expand_to_the_drivers_code)
{
    const embi_shader_stage *stages[] = {&embi_shader_box.vs, &embi_shader_box.fs};
    for (size_t i = 0; i < 2; i++) {
        check_blob(&stages[i]->spirv, "spirv");
        check_blob(&stages[i]->dxil, "dxil");
        check_blob(&stages[i]->msl, "msl");
    }
}

EMB_TEST_MAIN()
