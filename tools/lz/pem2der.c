/* pem2der IN.pem OUT: converts the CERTIFICATE blocks of a PEM bundle (Mozilla's cacert.pem; the text between the blocks
 * is ignored) into one file of records: a 2-byte little-endian length, then the DER certificate. Used at build time
 * under EMBER_COMPRESS_ASSETS with EMBER_NET (SZ3a): DER is 3/4 of the base64 and drops the headers and the
 * per-root comments, 200 KB to about 140 KB in the library. src/net/tls.c reads the records. Prints the number of
 * certificates and the output size. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : (c == '/' ? 63 : -1);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: pem2der IN.pem OUT\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "pem2der: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = sz < 0 ? nullptr : malloc((size_t)sz + 1);
    if (!s || fread(s, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "pem2der: cannot read %s\n", argv[1]);
        free(s);
        fclose(f);
        return 1;
    }
    fclose(f);
    s[sz] = 0;

    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    uint8_t *out = malloc((size_t)sz + 1);
    if (!out) {
        fprintf(stderr, "pem2der: out of memory\n");
        free(s);
        return 1;
    }
    size_t o = 0, count = 0;
    char *p = s;
    while ((p = strstr(p, begin)) != nullptr) {
        p += sizeof begin - 1;
        char *e = strstr(p, end);
        if (!e) {
            fprintf(stderr, "pem2der: unterminated block\n");
            free(s);
            free(out);
            return 1;
        }
        size_t len_at = o;
        o += 2;
        uint32_t acc = 0;
        int bits = 0;
        for (; p < e; p++) {
            int v = b64val((unsigned char)*p);
            if (v < 0) {
                continue; /* line breaks, '=' padding */
            }
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[o++] = (uint8_t)(acc >> bits);
            }
        }
        size_t n = o - len_at - 2;
        if (n == 0 || n > 65535) {
            fprintf(stderr, "pem2der: bad certificate size %zu\n", n);
            free(s);
            free(out);
            return 1;
        }
        out[len_at] = (uint8_t)n;
        out[len_at + 1] = (uint8_t)(n >> 8);
        count++;
        p = e + sizeof end - 1;
    }
    FILE *g = fopen(argv[2], "wb");
    if (!g || fwrite(out, 1, o, g) != o || fclose(g) != 0 || count == 0) {
        fprintf(stderr, "pem2der: cannot write %s\n", argv[2]);
        free(s);
        free(out);
        return 1;
    }
    printf("%zu certificates, %zu bytes\n", count, o);
    free(s);
    free(out);
    return 0;
}
