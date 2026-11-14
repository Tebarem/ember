/*
 * Regressions found by the libFuzzer harnesses (tests/fuzz, task M9a). Each input is the minimized
 * reproducer, as a byte array.
 *
 * - A GIF whose graphic control extension declares a block size of 0 (the format fixes it at 4)
 *   made SDL_image's GIF reader write the first pixel through a null surface pointer
 *   (third_party/SDL_image/src/IMG_gif.c, ReadImage). The header probe in src/image/formats.c now
 *   refuses such a file before SDL_image sees it.
 * - A GIF with an application extension whose block size is 0 followed by bytes that do not form
 *   sub-blocks: SDL_image's header parser fails, and its fallback to the single-frame decoder
 *   leaks the GIF decoder context (about 66 KB, found by LeakSanitizer in the asan ctest and by
 *   libFuzzer). embi_gif_probe now mirrors the parser's reading of the block size.
 */
#include "emb_test.h"

#include "base/base.h"
#include "image/image.h"
#include "image/internal.h"

/* fuzz_image input, 85 bytes. The last byte is the harness mode byte. */
static const uint8_t kGifGceSizeZero[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x30, 0x00, 0x20, 0x00, 0xf1, 0x00, 0xff, 0xff, 0x00, 0x41,
    0xe6, 0x14, 0x14, 0x14, 0x14, 0xe6, 0x00, 0xfe, 0x00, 0x21, 0xf9, 0x00, 0x2c, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x20, 0x00, 0xac, 0xca, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

EMB_TEST(gif_graphic_control_extension_with_block_size_zero_is_refused)
{
    size_t live = embi_mem_live();
    embi_pixels px = {0};
    EMB_CHECK(!embi_image_decode(kGifGceSizeZero, sizeof kGifGceSizeZero, EMBI_IMAGE_DEFAULT_MAX_PIXELS,
                                 true, nullptr, nullptr, &px));
    EMB_CHECK(px.px == nullptr);
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(!embi_gif_probe(kGifGceSizeZero, sizeof kGifGceSizeZero, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK(embi_mem_live() == live);
}

/* fuzz_image input, 43 bytes (including the mode byte at the end). */
static const uint8_t kGifAppExtSizeZero[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x30, 0x00, 0x30, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xe6, 0xfd, 0x14, 0x14, 0x14, 0xe6, 0x00, 0x00, 0x00, 0x21, 0xff, 0x00, 0x2c, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x20, 0x00, 0x00, 0x06, 0x00, 0xfc, 0x1e, 0xb4,
};

EMB_TEST(gif_application_extension_with_block_size_zero_is_refused)
{
    size_t live = embi_mem_live();
    embi_pixels px = {0};
    EMB_CHECK(!embi_image_decode(kGifAppExtSizeZero, sizeof kGifAppExtSizeZero, EMBI_IMAGE_DEFAULT_MAX_PIXELS,
                                 true, nullptr, nullptr, &px));
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(!embi_gif_probe(kGifAppExtSizeZero, sizeof kGifAppExtSizeZero, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK(embi_mem_live() == live);
}

/* fuzz_image input, 75 bytes (the last byte is the mode byte). The size-0 application extension
 * makes SDL_image's header walker and frame reader disagree: the frame reader ends up on an image
 * descriptor of size 0 and writes through a null pixel pointer (IMG_gif.c, ReadImage). */
static const uint8_t kGifAppExtMisaligned[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x30, 0x00, 0x03, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x19, 0xeb, 0xf4, 0x14, 0x14, 0xe6, 0x00, 0x00, 0x00, 0x21, 0xff, 0x00, 0x23, 0x05, 0x91, 0x15,
    0x22, 0x23, 0xff, 0xd9, 0x00, 0x2c, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x15, 0x22, 0x23, 0x32, 0x6e, 0x6e, 0x6e, 0x6e, 0xff, 0xd9, 0x02, 0x04, 0x07, 0x02, 0xff, 0xc4,
    0x00, 0x2c, 0x10, 0x00, 0x01, 0x03, 0x02, 0x03, 0x05, 0x15, 0x23,
};

EMB_TEST(gif_application_extension_with_block_size_zero_cannot_misalign_the_frame_reader)
{
    size_t live = embi_mem_live();
    embi_pixels px = {0};
    EMB_CHECK(!embi_image_decode(kGifAppExtMisaligned, sizeof kGifAppExtMisaligned, EMBI_IMAGE_DEFAULT_MAX_PIXELS,
                                 true, nullptr, nullptr, &px));
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(gif_with_a_normal_graphic_control_extension_still_probes)
{
    /* GIF89a, 1x1, no global table, GCE (size 4), one image descriptor. */
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x00, 0, 0,
                                  0x21, 0xF9, 4, 0, 0, 0, 0, 0,
                                  0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0};
    uint32_t w = 0;
    uint32_t h = 0;
    EMB_CHECK(embi_gif_probe(gif, sizeof gif, EMBI_IMAGE_DEFAULT_MAX_PIXELS, &w, &h));
    EMB_CHECK(w == 1 && h == 1);
}

EMB_TEST_MAIN()
