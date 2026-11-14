// fuzz-modules: codec base
/* Brotli (RFC 7932) decoder on raw bytes with an output cap. */
#include "fuzz_common.h"

#include "codec/codec.h"

#define MAX_OUT ((size_t)4 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    embi_buf out = {0};
    bool ok = embi_brotli_decode(data, size, MAX_OUT, &out);
    if (out.len > MAX_OUT) {
        abort();
    }
    (void)ok;
    embi_buf_free(&out);
    FUZZ_GUARD_END();
    return 0;
}
