// fuzz-modules: codec base
/* DEFLATE and zlib: the one-shot decoder and the streaming decoder with odd input and output chunk
 * sizes. Input: a stream as it is. The last byte (not removed; trailing bytes after the end of a stream
 * are never consumed) is the mode: bit 0 = zlib wrapper, bit 1 = streaming, bits 2..7 = chunk sizes. */
#include "fuzz_common.h"

#include "codec/codec.h"

#define MAX_OUT ((size_t)1 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = size ? data[size - 1] : 0;
    uint8_t chunks = (uint8_t)(mode >> 2);
    bool zlib = mode & 1;
    if (mode & 2) {
        size_t in_chunk = (size_t)(chunks & 7) + 1;
        size_t out_chunk = (size_t)(chunks >> 3) * 37 + 1;
        embi_inflater *z = embi_inflater_new(zlib, MAX_OUT);
        uint8_t *out = embi_alloc(out_chunk);
        size_t pos = 0;
        size_t total = 0;
        for (;;) {
            size_t take = in.n - pos < in_chunk ? in.n - pos : in_chunk;
            size_t used = 0;
            size_t got = 0;
            embi_inflate_status st = embi_inflate(z, in.n ? in.p + pos : nullptr, take, &used, out,
                                                  out_chunk, &got);
            pos += used;
            total += got;
            if (total > MAX_OUT) {
                abort();
            }
            if (st == EMBI_INFLATE_DONE || st == EMBI_INFLATE_ERROR) {
                break;
            }
            if (st == EMBI_INFLATE_NEED_INPUT && pos >= in.n) {
                break;
            }
            if (used == 0 && got == 0 && take == 0) {
                break;
            }
        }
        embi_free(out, out_chunk);
        embi_inflater_free(z);
    } else {
        embi_buf out = {0};
        bool ok = embi_inflate_all(in.p, in.n, zlib, MAX_OUT, &out);
        if (out.len > MAX_OUT) {
            abort();
        }
        (void)ok;
        embi_buf_free(&out);
    }
    FUZZ_GUARD_END();
    return 0;
}
