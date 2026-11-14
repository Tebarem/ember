/*
 * unicodegen: builds src/jslex/unicode_id.inc from the Unicode Character Database.
 *
 * Usage: unicodegen <ucd-dir> <output.inc>
 *   <ucd-dir> must contain DerivedCoreProperties.txt (UCD 17.0.0, see tools/unicodegen/data).
 *
 * The output lists ID_Start and ID_Continue as sorted, merged, inclusive code point ranges.
 * The jslex module searches them with a binary search. This is a host tool: it runs only
 * through the 'unicodegen' CMake target, never during a normal build, and may use stdio.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t lo;
    uint32_t hi;
} range_t;

typedef struct {
    range_t *items;
    size_t count;
    size_t cap;
} range_list;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "unicodegen: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static void rl_push(range_list *l, uint32_t lo, uint32_t hi)
{
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        range_t *items = realloc(l->items, cap * sizeof *items);
        if (items == nullptr) {
            die("out of memory", nullptr);
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->count].lo = lo;
    l->items[l->count].hi = hi;
    l->count++;
}

static int cmp_range(const void *a, const void *b)
{
    const range_t *x = a;
    const range_t *y = b;
    if (x->lo != y->lo) {
        return x->lo < y->lo ? -1 : 1;
    }
    return 0;
}

/* Sorts and merges overlapping or adjacent ranges in place. Returns the merged count. */
static size_t merge_ranges(range_list *l)
{
    if (l->count == 0) {
        return 0;
    }
    qsort(l->items, l->count, sizeof *l->items, cmp_range);
    size_t out = 0;
    for (size_t i = 1; i < l->count; i++) {
        if (l->items[i].lo <= l->items[out].hi + 1) {
            if (l->items[i].hi > l->items[out].hi) {
                l->items[out].hi = l->items[i].hi;
            }
        } else {
            l->items[++out] = l->items[i];
        }
    }
    l->count = out + 1;
    return l->count;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static uint32_t parse_hex(const char *s, const char *path)
{
    char *end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > 0x10FFFFul) {
        die("bad code point in", path);
    }
    return (uint32_t)v;
}

static void parse_ucd(const char *path, range_list *start, range_list *cont, char *version,
                      size_t vcap)
{
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        die("cannot open", path);
    }
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof line, f) != nullptr) {
        if (first) {
            first = false;
            const char *prefix = "# DerivedCoreProperties-";
            if (strncmp(line, prefix, strlen(prefix)) == 0) {
                const char *v = line + strlen(prefix);
                const char *dot = strstr(v, ".txt");
                if (dot != nullptr && (size_t)(dot - v) < vcap) {
                    memcpy(version, v, (size_t)(dot - v));
                    version[dot - v] = '\0';
                }
            }
        }
        char *hash = strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char *semi = strchr(line, ';');
        if (semi == nullptr) {
            continue;
        }
        *semi = '\0';
        char *range = trim(line);
        char *prop = trim(semi + 1);
        range_list *dst = nullptr;
        if (strcmp(prop, "ID_Start") == 0) {
            dst = start;
        } else if (strcmp(prop, "ID_Continue") == 0) {
            dst = cont;
        } else {
            continue;
        }
        uint32_t lo;
        uint32_t hi;
        char *dots = strstr(range, "..");
        if (dots != nullptr) {
            *dots = '\0';
            lo = parse_hex(range, path);
            hi = parse_hex(dots + 2, path);
        } else {
            lo = parse_hex(range, path);
            hi = lo;
        }
        if (lo > hi) {
            die("inverted range in", path);
        }
        rl_push(dst, lo, hi);
    }
    fclose(f);
}

static void emit_table(FILE *out, const char *name, const range_list *l)
{
    fprintf(out, "static const uint32_t embi_jslex_%s_tab[][2] = {\n", name);
    for (size_t i = 0; i < l->count; i++) {
        if (i % 4 == 0) {
            fprintf(out, "   ");
        }
        fprintf(out, " {0x%04X, 0x%04X},", (unsigned)l->items[i].lo, (unsigned)l->items[i].hi);
        if (i % 4 == 3 || i + 1 == l->count) {
            fprintf(out, "\n");
        }
    }
    fprintf(out, "};\n\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        die("usage: unicodegen <ucd-dir> <output.inc>", nullptr);
    }
    char path[4096];
    if (snprintf(path, sizeof path, "%s/DerivedCoreProperties.txt", argv[1]) >=
        (int)sizeof path) {
        die("ucd-dir path too long", nullptr);
    }
    range_list start = {nullptr, 0, 0};
    range_list cont = {nullptr, 0, 0};
    char version[32] = "unknown";
    parse_ucd(path, &start, &cont, version, sizeof version);
    if (merge_ranges(&start) == 0 || merge_ranges(&cont) == 0) {
        die("no ID_Start or ID_Continue ranges in", path);
    }

    FILE *out = fopen(argv[2], "w");
    if (out == nullptr) {
        die("cannot write", argv[2]);
    }
    fprintf(out,
            "/* Generated by tools/unicodegen from DerivedCoreProperties-%s.txt (UCD %s).\n"
            " * Do not edit. Regenerate with: cmake --build <dir> --target unicodegen */\n",
            version, version);
    fprintf(out, "#define EMBI_JSLEX_UNICODE_VERSION \"%s\"\n\n", version);
    emit_table(out, "id_start", &start);
    emit_table(out, "id_continue", &cont);
    if (fclose(out) != 0) {
        die("write failed", argv[2]);
    }
    free(start.items);
    free(cont.items);
    return 0;
}
