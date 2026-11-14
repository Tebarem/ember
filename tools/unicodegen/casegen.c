/*
 * casegen: builds src/js/unicode_case.inc, the tables behind String.prototype.toLowerCase and
 * toUpperCase (full Unicode default case conversion, without the locale-sensitive rules), and the
 * Cased and Case_Ignorable properties that the Final_Sigma rule needs.
 *
 * Usage: casegen <ucd-dir> <output.inc>
 *   <ucd-dir> must contain UnicodeData.txt, SpecialCasing.txt and DerivedCoreProperties.txt
 *   (UCD 17.0.0, see tools/unicodegen/data).
 *
 * Simple mappings from UnicodeData are encoded as runs (start, delta, count, stride): code points
 * spaced by stride that all shift by the same delta. Unconditional mappings from SpecialCasing
 * that produce more than one code point (U+00DF upper case is "SS") are listed separately. The
 * conditional entries of SpecialCasing are skipped: Final_Sigma is handled by the runtime, and the
 * language-specific ones do not apply to ECMAScript. This is a host tool and may use stdio.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_MAX 0x110000u
#define MAX_FIELDS 16

typedef struct {
    uint32_t start;
    int32_t delta;
    uint32_t count;
    uint32_t stride;
} run_t;

typedef struct {
    uint32_t cp;
    uint32_t out[3];
} special_t;

typedef struct {
    uint32_t lo;
    uint32_t hi;
} range_t;

typedef struct {
    special_t *items;
    size_t count;
    size_t cap;
} special_list;

typedef struct {
    range_t *items;
    size_t count;
    size_t cap;
} range_list;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "casegen: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static FILE *open_ucd(const char *dir, const char *name)
{
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        die("ucd-dir path too long", nullptr);
    }
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        die("cannot open", path);
    }
    return f;
}

static uint32_t parse_hex(const char *s, const char *what)
{
    char *end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > 0x10FFFFul) {
        die("bad code point in", what);
    }
    return (uint32_t)v;
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

/* Splits line in place on ';'. Returns the number of fields (at most MAX_FIELDS). */
static size_t split_fields(char *line, char **fields)
{
    size_t n = 0;
    char *p = line;
    for (;;) {
        if (n == MAX_FIELDS) {
            return n;
        }
        fields[n++] = p;
        char *semi = strchr(p, ';');
        if (semi == nullptr) {
            return n;
        }
        *semi = '\0';
        p = semi + 1;
    }
}

static void special_push(special_list *l, uint32_t cp, const uint32_t *out)
{
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 128;
        special_t *items = realloc(l->items, cap * sizeof *items);
        if (items == nullptr) {
            die("out of memory", nullptr);
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->count].cp = cp;
    memcpy(l->items[l->count].out, out, sizeof l->items[l->count].out);
    l->count++;
}

static void range_push(range_list *l, uint32_t lo, uint32_t hi)
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
    return (x->lo > y->lo) - (x->lo < y->lo);
}

static int cmp_special(const void *a, const void *b)
{
    const special_t *x = a;
    const special_t *y = b;
    return (x->cp > y->cp) - (x->cp < y->cp);
}

/* Sorts and merges overlapping or adjacent ranges in place. */
static void merge_ranges(range_list *l)
{
    if (l->count == 0) {
        return;
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
}

/* Reads the simple case mappings (fields 12 and 13 of UnicodeData) into the maps. */
static void read_unicode_data(const char *dir, uint32_t *upper, uint32_t *lower)
{
    FILE *f = open_ucd(dir, "UnicodeData.txt");
    char line[1024];
    while (fgets(line, sizeof line, f) != nullptr) {
        char *fields[MAX_FIELDS] = {nullptr};
        size_t n = split_fields(line, fields);
        if (n < 14) {
            continue;
        }
        uint32_t cp = parse_hex(trim(fields[0]), "UnicodeData.txt");
        char *u = trim(fields[12]);
        char *l = trim(fields[13]);
        if (*u != '\0') {
            upper[cp] = parse_hex(u, "UnicodeData.txt");
        }
        if (*l != '\0') {
            lower[cp] = parse_hex(l, "UnicodeData.txt");
        }
    }
    fclose(f);
}

/* Reads the unconditional entries of SpecialCasing. A result of one code point replaces the
 * simple mapping; a longer result goes to the special list and clears the simple mapping. */
static void read_special_casing(const char *dir, uint32_t *upper, uint32_t *lower,
                                special_list *sup, special_list *slow)
{
    FILE *f = open_ucd(dir, "SpecialCasing.txt");
    char line[1024];
    while (fgets(line, sizeof line, f) != nullptr) {
        char *hash = strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char *fields[MAX_FIELDS] = {nullptr};
        size_t n = split_fields(line, fields);
        /* Unconditional: code; lower; title; upper; then an empty tail. Conditional entries have
         * a further condition field and are skipped. */
        if (n != 5 || *trim(fields[4]) != '\0') {
            continue;
        }
        uint32_t cp = parse_hex(trim(fields[0]), "SpecialCasing.txt");
        for (int k = 0; k < 2; k++) {
            char *seq = trim(fields[k == 0 ? 1 : 3]);
            uint32_t out[3] = {0, 0, 0};
            size_t m = 0;
            char *tok = strtok(seq, " ");
            while (tok != nullptr) {
                if (m == 3) {
                    die("mapping longer than three code points in", "SpecialCasing.txt");
                }
                out[m++] = parse_hex(tok, "SpecialCasing.txt");
                tok = strtok(nullptr, " ");
            }
            if (m == 0) {
                die("empty mapping in", "SpecialCasing.txt");
            }
            uint32_t *map = k == 0 ? lower : upper;
            special_list *sl = k == 0 ? slow : sup;
            if (m == 1) {
                map[cp] = out[0];
            } else {
                map[cp] = 0;
                special_push(sl, cp, out);
            }
        }
    }
    fclose(f);
}

/* Derives the runs of a map: code points whose single mapping differs from themselves. */
static run_t *build_runs(const uint32_t *map, size_t *nruns)
{
    size_t cap = 1024;
    size_t n = 0;
    run_t *runs = malloc(cap * sizeof *runs);
    if (runs == nullptr) {
        die("out of memory", nullptr);
    }
    for (uint32_t cp = 0; cp < CP_MAX; cp++) {
        if (map[cp] == 0 || map[cp] == cp) {
            continue;
        }
        int32_t delta = (int32_t)((int64_t)map[cp] - (int64_t)cp);
        if (n > 0) {
            run_t *last = &runs[n - 1];
            if (last->delta == delta && cp > last->start) {
                uint32_t next = last->start + last->count * last->stride;
                if (last->count == 1) {
                    /* The second member fixes the stride, which must fit the stride field. */
                    uint32_t st = cp - last->start;
                    if (st <= 255) {
                        last->stride = st;
                        last->count = 2;
                        continue;
                    }
                } else if (cp == next && last->count < 65535) {
                    last->count++;
                    continue;
                }
            }
        }
        if (n == cap) {
            cap *= 2;
            runs = realloc(runs, cap * sizeof *runs);
            if (runs == nullptr) {
                die("out of memory", nullptr);
            }
        }
        runs[n].start = cp;
        runs[n].delta = delta;
        runs[n].count = 1;
        runs[n].stride = 1;
        n++;
    }
    *nruns = n;
    return runs;
}

static range_list read_property(const char *dir, const char *file, const char *prop)
{
    FILE *f = open_ucd(dir, file);
    range_list l = {nullptr, 0, 0};
    char line[1024];
    while (fgets(line, sizeof line, f) != nullptr) {
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
        if (strcmp(trim(semi + 1), prop) != 0) {
            continue;
        }
        char *dots = strstr(range, "..");
        uint32_t lo;
        uint32_t hi;
        if (dots != nullptr) {
            *dots = '\0';
            lo = parse_hex(range, file);
            hi = parse_hex(dots + 2, file);
        } else {
            lo = parse_hex(range, file);
            hi = lo;
        }
        range_push(&l, lo, hi);
    }
    fclose(f);
    merge_ranges(&l);
    return l;
}

static void emit_runs(FILE *out, const char *name, const run_t *runs, size_t n)
{
    fprintf(out, "static const embi_case_run %s[] = {\n", name);
    for (size_t i = 0; i < n; i++) {
        if (i % 4 == 0) {
            fprintf(out, "   ");
        }
        fprintf(out, " {0x%05X, %d, %u, %u},", (unsigned)runs[i].start, runs[i].delta,
                (unsigned)runs[i].count, (unsigned)runs[i].stride);
        if (i % 4 == 3 || i + 1 == n) {
            fprintf(out, "\n");
        }
    }
    fprintf(out, "};\n\n");
}

static void emit_special(FILE *out, const char *name, const special_list *l)
{
    fprintf(out, "static const embi_case_special %s[] = {\n", name);
    for (size_t i = 0; i < l->count; i++) {
        const special_t *s = &l->items[i];
        fprintf(out, "   {0x%05X, {0x%04X, 0x%04X, 0x%04X}},\n", (unsigned)s->cp,
                (unsigned)s->out[0], (unsigned)s->out[1], (unsigned)s->out[2]);
    }
    fprintf(out, "};\n\n");
}

static void emit_ranges(FILE *out, const char *name, const range_list *l)
{
    fprintf(out, "static const embi_case_range %s[] = {\n", name);
    for (size_t i = 0; i < l->count; i++) {
        if (i % 4 == 0) {
            fprintf(out, "   ");
        }
        fprintf(out, " {0x%05X, 0x%05X},", (unsigned)l->items[i].lo, (unsigned)l->items[i].hi);
        if (i % 4 == 3 || i + 1 == l->count) {
            fprintf(out, "\n");
        }
    }
    fprintf(out, "};\n\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        die("usage: casegen <ucd-dir> <output.inc>", nullptr);
    }
    const char *dir = argv[1];
    uint32_t *upper = calloc(CP_MAX, sizeof *upper);
    uint32_t *lower = calloc(CP_MAX, sizeof *lower);
    if (upper == nullptr || lower == nullptr) {
        die("out of memory", nullptr);
    }
    read_unicode_data(dir, upper, lower);
    special_list sup = {nullptr, 0, 0};
    special_list slow = {nullptr, 0, 0};
    read_special_casing(dir, upper, lower, &sup, &slow);
    /* The runtime binary-searches these lists, and SpecialCasing is not sorted in every release. */
    qsort(sup.items, sup.count, sizeof *sup.items, cmp_special);
    qsort(slow.items, slow.count, sizeof *slow.items, cmp_special);

    size_t nup = 0;
    size_t nlow = 0;
    run_t *up_runs = build_runs(upper, &nup);
    run_t *low_runs = build_runs(lower, &nlow);
    range_list cased = read_property(dir, "DerivedCoreProperties.txt", "Cased");
    range_list ci = read_property(dir, "DerivedCoreProperties.txt", "Case_Ignorable");
    if (cased.count == 0 || ci.count == 0) {
        die("no Cased or Case_Ignorable ranges in", dir);
    }

    FILE *out = fopen(argv[2], "w");
    if (out == nullptr) {
        die("cannot write", argv[2]);
    }
    fprintf(out,
            "/* Generated by tools/unicodegen/casegen.c from UCD 17.0.0 (UnicodeData, SpecialCasing,\n"
            " * DerivedCoreProperties). Do not edit. Regenerate with:\n"
            " *     cmake --build <dir> --target unicodecase */\n"
            "typedef struct embi_case_run {\n"
            "    uint32_t start;\n"
            "    int32_t delta;\n"
            "    uint16_t count;\n"
            "    uint8_t stride;\n"
            "} embi_case_run;\n"
            "typedef struct embi_case_special {\n"
            "    uint32_t cp;\n"
            "    uint32_t out[3]; /* zero-terminated */\n"
            "} embi_case_special;\n"
            "typedef struct embi_case_range {\n"
            "    uint32_t lo;\n"
            "    uint32_t hi;\n"
            "} embi_case_range;\n\n");
    emit_runs(out, "embi_case_upper_runs", up_runs, nup);
    emit_runs(out, "embi_case_lower_runs", low_runs, nlow);
    emit_special(out, "embi_case_upper_special", &sup);
    emit_special(out, "embi_case_lower_special", &slow);
    emit_ranges(out, "embi_case_cased", &cased);
    emit_ranges(out, "embi_case_case_ignorable", &ci);
    if (fclose(out) != 0) {
        die("write failed", argv[2]);
    }
    free(upper);
    free(lower);
    free(up_runs);
    free(low_runs);
    free(sup.items);
    free(slow.items);
    free(cased.items);
    free(ci.items);
    return 0;
}
