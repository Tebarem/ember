/* incbin INC NAME WIDTH OUT: writes the elements of the array NAME of a generated .inc file (a one- or two-dimensional list of
 * integer literals, as tools/unicodegen and tools/entitiesgen write them) as WIDTH-byte (1, 2 or 4) little-endian values to
 * OUT. Used at build time (SZ3a, cmake/EmberCompress.cmake) to give the lookup tables that are stored LZ-compressed a binary
 * input, so that the .inc files stay the single source of the data. The array is found by "NAME[" followed by "= {"
 * on the declaration. Prints the number of elements. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: incbin INC NAME WIDTH OUT\n");
        return 2;
    }
    int width = atoi(argv[3]);
    if (width != 1 && width != 2 && width != 4) {
        fprintf(stderr, "incbin: width must be 1, 2 or 4\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "incbin: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = sz < 0 ? nullptr : malloc((size_t)sz + 1);
    if (!s || fread(s, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "incbin: cannot read %s\n", argv[1]);
        free(s);
        fclose(f);
        return 1;
    }
    fclose(f);
    s[sz] = 0;

    /* The declaration: the name, then "[...] = {" with nothing but brackets and digits between. */
    size_t nl = strlen(argv[2]);
    char *p = s;
    char *body = nullptr;
    while ((p = strstr(p, argv[2])) != nullptr) {
        char *q = p + nl;
        bool word_start = p == s || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        if (word_start && *q == '[') {
            while (*q == '[' || *q == ']' || isalnum((unsigned char)*q) || *q == '_' || *q == '+' || *q == ' ') {
                q++;
            }
            if (*q == '=') {
                q++;
                while (isspace((unsigned char)*q)) {
                    q++;
                }
                if (*q == '{') {
                    body = q + 1;
                    break;
                }
            }
        }
        p += nl;
    }
    if (!body) {
        fprintf(stderr, "incbin: array %s not found in %s\n", argv[2], argv[1]);
        free(s);
        return 1;
    }

    FILE *o = fopen(argv[4], "wb");
    if (!o) {
        fprintf(stderr, "incbin: cannot open %s\n", argv[4]);
        free(s);
        return 1;
    }
    size_t count = 0;
    int depth = 1;
    for (p = body; depth > 0 && *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
        } else if (isdigit((unsigned char)*p)) {
            char *end;
            unsigned long long v = strtoull(p, &end, 0);
            if (v >> (8 * width) != 0) { /* (a shift by 64 never happens: width is at most 4) */
                fprintf(stderr, "incbin: value %llu does not fit %d bytes\n", v, width);
                fclose(o);
                free(s);
                return 1;
            }
            for (int i = 0; i < width; i++) {
                fputc((int)((v >> (8 * i)) & 0xff), o);
            }
            count++;
            p = end - 1;
        } else if (*p == '/' && p[1] == '*') {
            p = strstr(p + 2, "*/");
            if (!p) {
                break;
            }
            p++;
        }
    }
    if (depth != 0 || fclose(o) != 0) {
        fprintf(stderr, "incbin: malformed array %s\n", argv[2]);
        free(s);
        return 1;
    }
    printf("%zu\n", count);
    free(s);
    return 0;
}
