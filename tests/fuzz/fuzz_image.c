// fuzz-modules: image codec base
/* The image decode entry point on raw bytes: the format is sniffed from the magic bytes (PNG built
 * in, JPEG and GIF through SDL_image), the header limits apply, EXIF orientation is optional. The
 * second half of the inputs also goes through the image cache (finish, query, pixels, trim).
 * Input: an image file as it is. The last byte (not removed) is the mode: bit 0 = apply EXIF
 * orientation, bit 1 = go through the cache, bit 2 = tight pixel limit. */
#include "fuzz_common.h"

#include "image/image.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = size ? data[size - 1] : 0;
    uint64_t max_pixels = (mode & 4) ? (uint64_t)1 << 16 : (uint64_t)1 << 22;
    (void)embi_image_sniff(in.p, in.n);
    if (mode & 2) {
        embi_image_cache *c = embi_image_cache_create((size_t)8 << 20);
        embi_image_cache_set_max_pixels(c, max_pixels);
        embi_image_cache_set_apply_orientation(c, mode & 1);
        embi_str url = EMBI_STR("fuzz:image");
        embi_image_cache_begin(c, url);
        embi_image_state st = embi_image_cache_finish(c, url, in.p, in.n);
        uint32_t w = 0;
        uint32_t h = 0;
        (void)embi_image_cache_query(c, url, &w, &h);
        if (st == EMBI_IMAGE_READY) {
            const uint8_t *px = nullptr;
            if (embi_image_cache_pixels(c, url, &px, &w, &h) && w && h) {
                volatile uint8_t sink = px[0] ^ px[(size_t)w * h * 4 - 1];
                (void)sink;
            }
            embi_image_cache_trim(c);
        }
        embi_image_cache_destroy(c);
    } else {
        embi_pixels out = {0};
        if (embi_image_decode(in.p, in.n, max_pixels, mode & 1, nullptr, nullptr, &out)) {
            if (out.w == 0 || out.h == 0 || out.px == nullptr || out.w > EMBI_IMAGE_MAX_DIM ||
                out.h > EMBI_IMAGE_MAX_DIM) {
                abort();
            }
            volatile uint8_t sink = out.px[0] ^ out.px[(size_t)out.w * out.h * 4 - 1];
            (void)sink;
            embi_pixels_free(&out);
        }
    }
    FUZZ_GUARD_END();
    return 0;
}
