/*
 * normgen: builds src/js/unicode_norm.inc, the tables behind String.prototype.normalize (UAX #15:
 * the NFC, NFD, NFKC and NFKD forms that ECMA-262 22.1.3.15 uses).
 *
 * Usage: normgen <ucd-dir> <output.inc>
 *   <ucd-dir> must contain UnicodeData.txt and DerivedNormalizationProps.txt (UCD 17.0.0, see
 *   tools/unicodegen/data).
 *
 * Tables. Every key table is sorted and binary searched by the runtime (src/js/unicode_norm.c). The
 * BMP and supplementary parts are split, so most keys take 16 bits:
 *   ccc    canonical combining classes of the non-zero code points, as runs (lo, hi, class).
 *   dec    decomposition mappings, one record per code point: a key, an offset into the pool and a
 *          length in pool units, with the compatibility flag. Hangul syllables are algorithmic and
 *          are not listed.
 *   pool   the mapping targets as UTF-16 code units (no target is a surrogate, so a supplementary
 *          target is an ordinary surrogate pair).
 *   comp   canonical composition pairs (first, second) -> composite, without the
 *          Full_Composition_Exclusion code points (singletons and non-starter decompositions are
 *          excluded there, so they never compose). Pairs whose two code points are in the BMP go
 *          in one table, the rest in the other.
 *
 * This is a host tool and may use stdio. The output is checked in; a normal build never runs it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_MAX 0x110000u
#define MAX_FIELDS 16
#define POOL_MAX 65536u
#define PAIR_MAX 4096u
#define CCC_MAX 1024u
#define DEC_BMP_MAX 8192u
#define DEC_SMP_MAX 4096u

static uint8_t ccc_of[CP_MAX];
static uint8_t has_dec[CP_MAX]; /* 0 none, 1 canonical, 2 compatibility */
static uint32_t dec_off[CP_MAX];
static uint32_t dec_len[CP_MAX];
static uint8_t excluded[CP_MAX];

static uint16_t pool[POOL_MAX];
static uint32_t pool_len;

typedef struct {
    uint32_t a, b, c;
} pair_t;
static pair_t pairs[PAIR_MAX];
static uint32_t npairs;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "normgen: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static FILE *open_ucd(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        die("cannot open", path);
    }
    return f;
}

static void pool_push_cp(uint32_t cp)
{
    if (pool_len + 2 > POOL_MAX) {
        die("pool overflow", nullptr);
    }
    if (cp < 0x10000u) {
        pool[pool_len++] = (uint16_t)cp;
    } else {
        cp -= 0x10000u;
        pool[pool_len++] = (uint16_t)(0xD800u + (cp >> 10));
        pool[pool_len++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
    }
}

static void read_unicode_data(const char *dir)
{
    FILE *f = open_ucd(dir, "UnicodeData.txt");
    char line[1024];
    uint32_t range_lo = 0;
    bool in_range = false;
    while (fgets(line, sizeof line, f)) {
        char *fields[MAX_FIELDS];
        int n = 0;
        char *p = line;
        while (n < MAX_FIELDS) {
            fields[n++] = p;
            char *semi = strchr(p, ';');
            if (!semi) {
                break;
            }
            *semi = 0;
            p = semi + 1;
        }
        if (n < 6) {
            continue;
        }
        uint32_t cp = (uint32_t)strtoul(fields[0], nullptr, 16);
        if (cp >= CP_MAX) {
            die("code point out of range", fields[0]);
        }
        uint32_t lo = cp;
        uint32_t hi = cp;
        if (strstr(fields[1], ", First>")) {
            range_lo = cp;
            in_range = true;
            continue;
        }
        if (in_range && strstr(fields[1], ", Last>")) {
            lo = range_lo;
            in_range = false;
        }
        int ccc = atoi(fields[3]);
        for (uint32_t c = lo; c <= hi; c++) {
            ccc_of[c] = (uint8_t)ccc;
        }
        const char *d = fields[5];
        if (*d == 0) {
            continue;
        }
        bool compat = *d == '<';
        if (compat) {
            const char *gt = strchr(d, '>');
            if (!gt) {
                die("bad decomposition", fields[0]);
            }
            d = gt + 1;
        }
        uint32_t start = pool_len;
        char *q = (char *)d;
        while (*q) {
            while (*q == ' ') {
                q++;
            }
            if (!*q) {
                break;
            }
            char *e;
            uint32_t t = (uint32_t)strtoul(q, &e, 16);
            if (e == q) {
                die("bad decomposition", fields[0]);
            }
            pool_push_cp(t);
            q = e;
        }
        if (pool_len == start) {
            continue;
        }
        has_dec[cp] = compat ? 2 : 1;
        dec_off[cp] = start;
        dec_len[cp] = pool_len - start;
    }
    fclose(f);
}

static void read_exclusions(const char *dir)
{
    FILE *f = open_ucd(dir, "DerivedNormalizationProps.txt");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "Full_Composition_Exclusion")) {
            continue;
        }
        char *e;
        uint32_t lo = (uint32_t)strtoul(line, &e, 16);
        uint32_t hi = lo;
        if (*e == '.') {
            hi = (uint32_t)strtoul(e + 2, nullptr, 16);
        }
        for (uint32_t c = lo; c <= hi; c++) {
            excluded[c] = 1;
        }
    }
    fclose(f);
}

/* The code point that starts at pool unit off: one unit, or a surrogate pair. */
static uint32_t pool_cp_at(uint32_t off, uint32_t *units)
{
    uint32_t u = pool[off];
    if (u >= 0xD800u && u < 0xDC00u) {
        *units = 2;
        return 0x10000u + ((u - 0xD800u) << 10) + (pool[off + 1] - 0xDC00u);
    }
    *units = 1;
    return u;
}

/* Canonical mappings of exactly two code points (singletons and longer ones never compose). */
static void build_pairs(void)
{
    for (uint32_t c = 0; c < CP_MAX; c++) {
        if (has_dec[c] != 1 || excluded[c]) {
            continue;
        }
        uint32_t off = dec_off[c];
        uint32_t a_units;
        uint32_t a = pool_cp_at(off, &a_units);
        if (a_units == dec_len[c]) {
            continue; /* a singleton mapping */
        }
        uint32_t b_units;
        uint32_t b = pool_cp_at(off + a_units, &b_units);
        if (a_units + b_units != dec_len[c]) {
            continue; /* three or more code points */
        }
        if (npairs >= PAIR_MAX) {
            die("too many pairs", nullptr);
        }
        pairs[npairs].a = a;
        pairs[npairs].b = b;
        pairs[npairs].c = c;
        npairs++;
    }
}

static int pair_cmp(const void *x, const void *y)
{
    const pair_t *p = x;
    const pair_t *q = y;
    if (p->a != q->a) {
        return p->a < q->a ? -1 : 1;
    }
    if (p->b != q->b) {
        return p->b < q->b ? -1 : 1;
    }
    return 0;
}

static void emit_u16(FILE *o, const char *name, const uint16_t *v, uint32_t n)
{
    fprintf(o, "static const uint16_t %s[%u] = {", name, n);
    for (uint32_t i = 0; i < n; i++) {
        fprintf(o, "%s0x%04X,", i % 12 == 0 ? "\n    " : " ", v[i]);
    }
    fprintf(o, "\n};\n");
}

static void emit_u32(FILE *o, const char *name, const uint32_t *v, uint32_t n)
{
    fprintf(o, "static const uint32_t %s[%u] = {", name, n);
    for (uint32_t i = 0; i < n; i++) {
        fprintf(o, "%s0x%05X,", i % 10 == 0 ? "\n    " : " ", v[i]);
    }
    fprintf(o, "\n};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        die("usage: normgen <ucd-dir> <output.inc>", nullptr);
    }
    read_unicode_data(argv[1]);
    read_exclusions(argv[1]);
    build_pairs();
    qsort(pairs, npairs, sizeof pairs[0], pair_cmp);

    /* ccc runs of equal class. */
    static uint32_t ccc_lo[CCC_MAX];
    static uint32_t ccc_hi[CCC_MAX];
    static uint8_t ccc_cls[CCC_MAX];
    uint32_t nccc = 0;
    for (uint32_t c = 0; c < CP_MAX; c++) {
        if (!ccc_of[c]) {
            continue;
        }
        if (nccc >= CCC_MAX) {
            die("too many ccc runs", nullptr);
        }
        uint32_t s = c;
        while (c + 1 < CP_MAX && ccc_of[c + 1] == ccc_of[s]) {
            c++;
        }
        ccc_lo[nccc] = s;
        ccc_hi[nccc] = c;
        ccc_cls[nccc] = ccc_of[s];
        nccc++;
    }

    /* Decomposition records, BMP and supplementary. The pool is filled in code point order, so
     * the mapping of record i ends where record i + 1 starts. The offset array therefore has one
     * entry more than the key array: its last entry is the end of the table's pool range (for the
     * BMP table that is the start of the first supplementary record). Bit 15 of an offset is the
     * compatibility flag. */
    static uint16_t dec_bmp_key[DEC_BMP_MAX];
    static uint16_t dec_bmp_off[DEC_BMP_MAX + 1];
    static uint32_t dec_smp_key[DEC_SMP_MAX];
    static uint16_t dec_smp_off[DEC_SMP_MAX + 1];
    uint32_t nbmp = 0;
    uint32_t nsmp = 0;
    uint32_t prev_end = 0;
    for (uint32_t c = 0; c < CP_MAX; c++) {
        if (!has_dec[c]) {
            continue;
        }
        if (dec_off[c] != prev_end || dec_off[c] + dec_len[c] > 0x7FFFu) {
            die("decomposition pool is not contiguous in code point order", nullptr);
        }
        prev_end = dec_off[c] + dec_len[c];
        uint16_t off = (uint16_t)(dec_off[c] | (has_dec[c] == 2 ? 0x8000u : 0u));
        if (c <= 0xFFFFu) {
            if (nbmp >= DEC_BMP_MAX) {
                die("too many BMP decompositions", nullptr);
            }
            dec_bmp_key[nbmp] = (uint16_t)c;
            dec_bmp_off[nbmp] = off;
            nbmp++;
        } else {
            if (nsmp >= DEC_SMP_MAX) {
                die("too many supplementary decompositions", nullptr);
            }
            dec_smp_key[nsmp] = c;
            dec_smp_off[nsmp] = off;
            nsmp++;
        }
    }
    /* Sentinels: the BMP table ends where the supplementary records start, the other at the end. */
    dec_bmp_off[nbmp] = (uint16_t)(nsmp ? dec_smp_off[0] & 0x7FFFu : pool_len);
    dec_smp_off[nsmp] = (uint16_t)pool_len;

    /* Composition pairs, split by whether both first and second are in the BMP. */
    static uint16_t cmp_bmp[PAIR_MAX][3];
    static uint32_t cmp_smp[PAIR_MAX][3];
    uint32_t nc16 = 0;
    uint32_t nc32 = 0;
    for (uint32_t i = 0; i < npairs; i++) {
        const pair_t *p = &pairs[i];
        if (p->a <= 0xFFFFu && p->b <= 0xFFFFu) {
            if (p->c > 0xFFFFu) {
                die("BMP pair with a supplementary composite", nullptr);
            }
            cmp_bmp[nc16][0] = (uint16_t)p->a;
            cmp_bmp[nc16][1] = (uint16_t)p->b;
            cmp_bmp[nc16][2] = (uint16_t)p->c;
            nc16++;
        } else {
            cmp_smp[nc32][0] = p->a;
            cmp_smp[nc32][1] = p->b;
            cmp_smp[nc32][2] = p->c;
            nc32++;
        }
    }

    FILE *o = fopen(argv[2], "w");
    if (!o) {
        die("cannot write", argv[2]);
    }
    fprintf(o, "/* Generated by tools/unicodegen/normgen.c from UCD 17.0.0. Do not edit. */\n");
    fprintf(o, "#define EMBI_NORM_CCC_COUNT %u\n", nccc);
    fprintf(o, "#define EMBI_NORM_DEC_BMP_COUNT %u\n", nbmp);
    fprintf(o, "#define EMBI_NORM_DEC_SMP_COUNT %u\n", nsmp);
    fprintf(o, "#define EMBI_NORM_POOL_COUNT %u\n", pool_len);
    fprintf(o, "#define EMBI_NORM_COMP_BMP_COUNT %u\n", nc16);
    fprintf(o, "#define EMBI_NORM_COMP_SMP_COUNT %u\n", nc32);
    fprintf(o, "static const struct { uint32_t lo, hi; uint8_t ccc; } embi_norm_ccc[%u] = {\n",
            nccc);
    for (uint32_t i = 0; i < nccc; i++) {
        fprintf(o, "    {0x%05X, 0x%05X, %u},\n", ccc_lo[i], ccc_hi[i], ccc_cls[i]);
    }
    fprintf(o, "};\n");
    emit_u16(o, "embi_norm_dec_bmp_key", dec_bmp_key, nbmp);
    emit_u16(o, "embi_norm_dec_bmp_off", dec_bmp_off, nbmp + 1);
    emit_u32(o, "embi_norm_dec_smp_key", dec_smp_key, nsmp);
    emit_u16(o, "embi_norm_dec_smp_off", dec_smp_off, nsmp + 1);
    emit_u16(o, "embi_norm_pool", pool, pool_len);
    fprintf(o, "static const uint16_t embi_norm_comp_bmp[%u][3] = {", nc16);
    for (uint32_t i = 0; i < nc16; i++) {
        fprintf(o, "\n    {0x%04X, 0x%04X, 0x%04X},", cmp_bmp[i][0], cmp_bmp[i][1], cmp_bmp[i][2]);
    }
    fprintf(o, "\n};\n");
    fprintf(o, "static const uint32_t embi_norm_comp_smp[%u][3] = {", nc32);
    for (uint32_t i = 0; i < nc32; i++) {
        fprintf(o, "\n    {0x%05X, 0x%05X, 0x%05X},", cmp_smp[i][0], cmp_smp[i][1],
                cmp_smp[i][2]);
    }
    fprintf(o, "\n};\n");
    fclose(o);
    fprintf(stderr,
            "normgen: ccc runs %u, dec bmp %u smp %u, pool %u units, comp bmp %u smp %u\n", nccc,
            nbmp, nsmp, pool_len, nc16, nc32);
    return 0;
}
