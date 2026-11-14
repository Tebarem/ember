/* lz [--spirv] IN OUT: compresses a file with the format of tools/lz/lz_encode.h (the decoder is embi_lz_decode in src/base/lz.c).
 * OUT is the 4-byte little-endian raw size followed by the stream. Prints "<raw size> <size of OUT>". Used at build time
 * for the embedded shader blobs, prelude scripts and the UA style sheet (cmake/EmberCompress.cmake). With --spirv the input is
 * a SPIR-V module and its debug instructions (names, source text, line info) are dropped first (SZ3a): drivers do not
 * read them, and they are a tenth of an unoptimised module. */
#include "lz_encode.h"

#include <stdbool.h>
#include <stdio.h>

/* Removes OpSourceContinued, OpSource, OpSourceExtension, OpName, OpMemberName, OpString, OpLine, OpNoLine and
 * OpModuleProcessed from a SPIR-V module in place; returns the new size, or 0 if it is not a well-formed module. */
static size_t spirv_strip(uint8_t *b, size_t n)
{
    if (n < 20 || (n & 3) != 0 || b[0] != 0x03 || b[1] != 0x02 || b[2] != 0x23 || b[3] != 0x07) {
        return 0;
    }
    size_t o = 20;
    for (size_t i = 20; i < n;) {
        uint32_t w = (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) | ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24);
        uint32_t op = w & 0xffff, len = (w >> 16) * 4u;
        if (len == 0 || len > n - i) {
            return 0;
        }
        bool drop = (op >= 2 && op <= 8) || op == 317 || op == 330;
        if (!drop) {
            memmove(b + o, b + i, len);
            o += len;
        }
        i += len;
    }
    return o;
}

int main(int argc, char **argv)
{
    bool spirv = argc == 4 && strcmp(argv[1], "--spirv") == 0;
    if (spirv) {
        argv++;
        argc--;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: lz [--spirv] IN OUT\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "lz: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return 1;
    }
    uint8_t *in = malloc((size_t)sz + 1);
    uint8_t *out = malloc(lze_bound((size_t)sz));
    if (!in || !out || fread(in, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "lz: cannot read %s\n", argv[1]);
        free(in);
        free(out);
        fclose(f);
        return 1;
    }
    fclose(f);
    if (spirv) {
        sz = (long)spirv_strip(in, (size_t)sz);
        if (sz == 0) {
            fprintf(stderr, "lz: %s is not a SPIR-V module\n", argv[1]);
            free(in);
            free(out);
            return 1;
        }
    }
    size_t n = lze_encode(in, (size_t)sz, out);
    FILE *g = fopen(argv[2], "wb");
    uint8_t hdr[4] = {(uint8_t)sz, (uint8_t)(sz >> 8), (uint8_t)(sz >> 16), (uint8_t)(sz >> 24)};
    if (!g || fwrite(hdr, 1, 4, g) != 4 || fwrite(out, 1, n, g) != n || fclose(g) != 0) {
        fprintf(stderr, "lz: cannot write %s\n", argv[2]);
        free(in);
        free(out);
        return 1;
    }
    printf("%ld %zu\n", sz, n + 4);
    free(in);
    free(out);
    return 0;
}
