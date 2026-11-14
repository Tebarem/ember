/*
 * encgen: builds src/loader/encoding_tables.inc from the WHATWG Encoding Standard data.
 *
 * Usage: encgen <encoding-dir> <output.inc>
 *   <encoding-dir> must contain encodings.json and index-*.txt (see tools/unicodegen/data/encoding).
 *
 * Every encoding in encodings.json becomes one entry with its lowercase name and its
 * space-separated labels. A single-byte encoding with an index file gets a 128-entry table (code
 * points for bytes 0x80..0xFF, 0xFFFD where the index has no entry). UTF-8, UTF-16LE/BE,
 * replacement and x-user-defined are marked by kind. The other encodings (the CJK ones, out of
 * scope) are marked unsupported. This is a host tool and may use stdio.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "encgen: %s%s\n", msg, arg ? arg : "");
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        die("out of memory", nullptr);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        die("out of memory", nullptr);
    }
    return q;
}

/* Reads a whole file into a NUL-terminated buffer. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        die("cannot open ", path);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Parses the JSON string starting at *pos (which must be at its opening quote). Escapes are
 * decoded for the subset the data uses. Returns a malloc'd string and moves *pos past the quote. */
static char *next_string(const char *s, size_t *pos)
{
    size_t i = *pos + 1;
    size_t cap = 64, len = 0;
    char *out = xmalloc(cap);
    while (s[i] && s[i] != '"') {
        char c = s[i];
        if (c == '\\') {
            i++;
            c = s[i];
            if (c == 'u') {
                unsigned v = (unsigned)strtoul(s + i + 1, nullptr, 16);
                c = (char)v;
                i += 4;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        out[len++] = c;
        i++;
    }
    out[len] = '\0';
    *pos = i + 1;
    return out;
}

static size_t skip_ws(const char *s, size_t i)
{
    while (s[i] && isspace((unsigned char)s[i])) {
        i++;
    }
    return i;
}

typedef struct enc {
    char *name;   /* as written in encodings.json */
    char *labels; /* space-separated */
} enc;

static enc encs[256];
static size_t nenc;

static void lower_copy(char *dst, const char *src)
{
    size_t k = 0;
    for (; src[k]; k++) {
        dst[k] = (char)tolower((unsigned char)src[k]);
    }
    dst[k] = '\0';
}

/* The C identifier of an encoding's index table: embi_index_<lowercase name, '-' as '_'>. */
static void index_ident(char *ident, const char *lname)
{
    size_t k = strlen("embi_index_");
    memcpy(ident, "embi_index_", k);
    for (const char *p = lname; *p; p++) {
        ident[k++] = (*p == '-' || *p == '.') ? '_' : *p;
    }
    ident[k] = '\0';
}

/* The index an encoding decodes with. iso-8859-8-i (logical order) uses the iso-8859-8 table: the
 * two differ only in how the text is ordered, which the decoder does not do. */
static const char *index_source(const char *lname)
{
    return strcmp(lname, "iso-8859-8-i") == 0 ? "iso-8859-8" : lname;
}

static bool index_exists(const char *dir, const char *lname)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/index-%s.txt", dir, lname);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

static void emit_index(FILE *o, const char *dir, const char *lname)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/index-%s.txt", dir, lname);
    FILE *f = fopen(path, "rb");
    if (!f) {
        die("cannot open ", path);
    }
    uint16_t table[128];
    for (int i = 0; i < 128; i++) {
        table[i] = 0xFFFD;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        int ptr = -1;
        unsigned cp = 0;
        if (sscanf(line, "%d %x", &ptr, &cp) == 2 && ptr >= 0 && ptr < 128) {
            table[ptr] = (uint16_t)cp;
        }
    }
    fclose(f);
    char ident[160];
    index_ident(ident, lname);
    fprintf(o, "static const uint16_t %s[128] = {\n", ident);
    for (int i = 0; i < 128; i++) {
        fprintf(o, "%s0x%04X,", (i % 8 == 0) ? "    " : " ", table[i]);
        if (i % 8 == 7) {
            fputc('\n', o);
        }
    }
    fprintf(o, "};\n\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: encgen <encoding-dir> <output.inc>\n");
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/encodings.json", argv[1]);
    char *json = slurp(path);

    /* A key is a string followed by ':'. Each encoding's labels precede its name. */
    size_t labels_cap = 1024, labels_len = 0;
    char *labels = xmalloc(labels_cap);
    labels[0] = '\0';
    for (size_t i = 0; json[i];) {
        if (json[i] != '"') {
            i++;
            continue;
        }
        size_t after = i;
        char *str = next_string(json, &after);
        size_t k = skip_ws(json, after);
        if (json[k] != ':') {
            /* A value, not a key. */
            free(str);
            i = after;
            continue;
        }
        if (strcmp(str, "labels") == 0) {
            size_t p = skip_ws(json, k + 1);
            if (json[p] != '[') {
                die("labels is not an array", nullptr);
            }
            p++;
            labels_len = 0;
            labels[0] = '\0';
            for (;;) {
                p = skip_ws(json, p);
                if (json[p] == ']') {
                    p++;
                    break;
                }
                if (json[p] == ',') {
                    p++;
                    continue;
                }
                if (json[p] != '"') {
                    die("bad label array", nullptr);
                }
                char *lab = next_string(json, &p);
                size_t ll = strlen(lab);
                while (labels_len + ll + 2 > labels_cap) {
                    labels_cap *= 2;
                    labels = xrealloc(labels, labels_cap);
                }
                if (labels_len > 0) {
                    labels[labels_len++] = ' ';
                }
                memcpy(labels + labels_len, lab, ll);
                labels_len += ll;
                labels[labels_len] = '\0';
                free(lab);
            }
            i = p;
            free(str);
            continue;
        }
        if (strcmp(str, "name") == 0) {
            size_t p = skip_ws(json, k + 1);
            if (nenc >= sizeof encs / sizeof encs[0]) {
                die("too many encodings", nullptr);
            }
            encs[nenc].name = next_string(json, &p);
            encs[nenc].labels = xmalloc(labels_len + 1);
            memcpy(encs[nenc].labels, labels, labels_len + 1);
            nenc++;
            i = p;
            free(str);
            continue;
        }
        free(str);
        i = after;
    }
    if (nenc == 0) {
        die("no encodings found in ", path);
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o) {
        die("cannot write ", argv[2]);
    }
    fprintf(o, "/* Generated by tools/unicodegen/encgen.c from encoding/encodings.json and the\n");
    fprintf(o, " * index-*.txt files (WHATWG Encoding Standard, be17ac5c). Do not edit.\n");
    fprintf(o, " * Regenerate with: cmake --build build/<dir> --target unicodeenc */\n\n");

    /* Index tables first, so the definition table can point at them. */
    for (size_t e = 0; e < nenc; e++) {
        char lname[128];
        lower_copy(lname, encs[e].name);
        const char *src = index_source(lname);
        if (strcmp(src, lname) == 0 && index_exists(argv[1], lname)) {
            emit_index(o, argv[1], lname);
        }
    }

    fprintf(o, "static const embi_encoding embi_enc_defs[%zu] = {\n", nenc);
    for (size_t e = 0; e < nenc; e++) {
        char lname[128];
        lower_copy(lname, encs[e].name);
        const char *kind = "EMBI_ENC_UNSUPPORTED";
        char ident[160] = "nullptr";
        const char *src = index_source(lname);
        if (index_exists(argv[1], src)) {
            kind = "EMBI_ENC_SINGLE";
            index_ident(ident, src);
        } else if (strcmp(lname, "utf-8") == 0) {
            kind = "EMBI_ENC_UTF8";
        } else if (strcmp(lname, "utf-16le") == 0) {
            kind = "EMBI_ENC_UTF16LE";
        } else if (strcmp(lname, "utf-16be") == 0) {
            kind = "EMBI_ENC_UTF16BE";
        } else if (strcmp(lname, "replacement") == 0) {
            kind = "EMBI_ENC_REPLACEMENT";
        } else if (strcmp(lname, "x-user-defined") == 0) {
            kind = "EMBI_ENC_USER_DEFINED";
        }
        fprintf(o, "    {\"%s\", \"%s\", %s, %s},\n", lname, encs[e].labels, kind, ident);
    }
    fprintf(o, "};\n");
    fclose(o);
    fprintf(stderr, "encgen: %zu encodings\n", nenc);
    return 0;
}
