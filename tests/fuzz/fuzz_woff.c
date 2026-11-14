// fuzz-modules: codec font base
/* WOFF 1.0 and WOFF 2.0 decoders. When a container decodes, the resulting sfnt is opened with the
 * font module too, so the decoder's output is checked as an input of the next stage. Input: a file as
 * it is. The signature picks the decoder ('wOF2' = WOFF2, anything else WOFF1, except that the last
 * byte decides for an input with neither signature), so the fixtures in tests/fonts work as seeds. */
#include "fuzz_common.h"

#include "codec/codec.h"
#include "font/font.h"

#define MAX_OUT ((size_t)8 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    bool w2 = size >= 4 && memcmp(data, "wOF2", 4) == 0;
    if (size >= 4 && memcmp(data, "wOFF", 4) != 0 && !w2) {
        w2 = (data[size - 1] & 1) != 0;
    }
    embi_buf out = {0};
    bool ok = w2 ? embi_woff2_decode(in.p, in.n, MAX_OUT, &out)
                 : embi_woff1_decode(in.p, in.n, MAX_OUT, &out);
    if (out.len > MAX_OUT) {
        abort();
    }
    if (ok && out.len > 0) {
        embi_font f;
        memset(&f, 0, sizeof f);
        if (embi_font_open(out.data, out.len, 0, &f)) {
            embi_outline ol = {0};
            for (uint32_t g = 0; g < 4; g++) {
                (void)embi_font_glyph_outline(&f, g, nullptr, &ol);
            }
            embi_outline_free(&ol);
        }
        embi_font_close(&f);
    }
    embi_buf_free(&out);
    FUZZ_GUARD_END();
    return 0;
}
