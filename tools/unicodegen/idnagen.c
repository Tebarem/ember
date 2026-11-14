/*
 * idnagen: builds src/loader/idna_table.inc from the UTS #46 mapping table (IdnaMappingTable.txt)
 * and src/loader/idna_props.inc from the UCD.
 *
 * Usage: idnagen <data-dir> <idna_table.inc> <idna_props.inc>
 *   <data-dir> must contain IdnaMappingTable.txt, UnicodeData.txt, DerivedNormalizationProps.txt
 *   and JoiningType.txt (Unicode 17.0.0 except the joining types, see tools/unicodegen/data).
 *
 * Both outputs are compact byte streams with sample indexes (the layouts are documented above
 * emit_table and emit_props) so that the data stays near 40 KB. Code points outside every mapping
 * entry are disallowed. Deviation entries are emitted as valid because the URL Standard uses
 * non-transitional processing. The NV8/XV8 flags are not emitted: the URL Standard runs UTS #46
 * with UseSTD3ASCIIRules=false. The bidi, joining, mark and ccc values are only kept for code
 * points that can reach the lookups (valid ones, mapping targets, parts and results of
 * compositions); the rest take the neighbouring value so the runs stay long. Keep the layouts in
 * step with src/loader/idna.c. The generator is a host tool and may use stdio.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Intermediate form, before the layout above is chosen. */
enum { M_VALID, M_IGNORED, M_DELTA, M_SEQ };

typedef struct item {
    uint32_t lo, hi;
    int mkind;
    uint32_t dst;    /* M_DELTA: the mapping of lo */
    uint32_t seq_at; /* M_SEQ: offset of the sequence in seqs, zero-terminated */
} item;

static item *items;
static size_t nitems, capitems;
static uint32_t *seqs;
static size_t nseqs, capseqs;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "idnagen: %s%s\n", msg, arg ? arg : "");
    exit(1);
}

static void seq_push(uint32_t v)
{
    if (nseqs == capseqs) {
        capseqs = capseqs ? capseqs * 2 : 4096;
        seqs = realloc(seqs, capseqs * sizeof *seqs);
        if (!seqs) {
            die("out of memory", nullptr);
        }
    }
    seqs[nseqs++] = v;
}

static void item_push(item it)
{
    if (nitems == capitems) {
        capitems = capitems ? capitems * 2 : 4096;
        items = realloc(items, capitems * sizeof *items);
        if (!items) {
            die("out of memory", nullptr);
        }
    }
    items[nitems++] = it;
}

/* Trims blanks at both ends in place and returns the start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    *e = '\0';
    return s;
}

static bool seq_equal(size_t a, size_t b);

/* Merges a new item into the list when it extends the previous one. */
static bool try_merge(const item *n)
{
    if (nitems == 0) {
        return false;
    }
    item *p = &items[nitems - 1];
    if (p->hi + 1 != n->lo) {
        return false;
    }
    if ((p->mkind == M_VALID || p->mkind == M_IGNORED) && p->mkind == n->mkind) {
        p->hi = n->hi;
        return true;
    }
    if (p->mkind == M_DELTA && n->mkind == M_DELTA && n->lo == n->hi &&
        p->dst + (p->hi - p->lo + 1) == n->dst) {
        p->hi = n->hi;
        return true;
    }
    if (p->mkind == M_SEQ && n->mkind == M_SEQ && seq_equal(p->seq_at, n->seq_at)) {
        p->hi = n->hi;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ properties for NFC and the
 * IDNA validity checks (src/loader/idna_props.inc) */

#define CP_LIMIT 0x110000u

/* Dense per-code-point values, filled from the UCD files. */
static uint8_t *dense_ccc;
static uint8_t *dense_bidi;
static uint8_t *dense_join;
static uint8_t *dense_mark;
static uint8_t *dense_excl;

/* Bidi class codes; keep in step with src/loader/idna.c (BIDI_*). 0 is every other class. */
enum { BIDI_OTHER = 0, BIDI_L, BIDI_R, BIDI_AL, BIDI_EN, BIDI_ES, BIDI_ET, BIDI_AN, BIDI_CS,
       BIDI_NSM, BIDI_BN, BIDI_ON };
/* Joining type codes; keep in step with src/loader/idna.c (JT_*). 0 is U (none). */
enum { JT_U = 0, JT_T, JT_R, JT_L, JT_D, JT_C };

typedef struct decomp_rec {
    uint32_t cp, a, b; /* b is 0 for a singleton */
} decomp_rec;
static decomp_rec *decomps;
static size_t ndecomps, capdecomps;

static uint32_t parse_hex(const char *s)
{
    return (uint32_t)strtoul(s, nullptr, 16);
}

/* Splits a semicolon-separated UCD line into at most max fields (in place). */
static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        f[n++] = p;
        char *semi = strchr(p, ';');
        if (!semi || n == max) {
            break;
        }
        *semi = '\0';
        p = semi + 1;
    }
    return n;
}

static void fill_range(uint8_t *dense, uint32_t lo, uint32_t hi, uint8_t v)
{
    for (uint32_t c = lo; c <= hi && c < CP_LIMIT; c++) {
        dense[c] = v;
    }
}

static uint8_t bidi_code(const char *s)
{
    static const struct {
        const char *name;
        uint8_t code;
    } table[] = {
        {"L", BIDI_L},   {"R", BIDI_R},   {"AL", BIDI_AL}, {"EN", BIDI_EN},   {"ES", BIDI_ES},
        {"ET", BIDI_ET}, {"AN", BIDI_AN}, {"CS", BIDI_CS}, {"NSM", BIDI_NSM}, {"BN", BIDI_BN},
        {"ON", BIDI_ON},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(s, table[i].name) == 0) {
            return table[i].code;
        }
    }
    return BIDI_OTHER;
}

static void read_unicode_data(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/UnicodeData.txt", dir);
    FILE *in = fopen(path, "rb");
    if (!in) {
        die("cannot open ", path);
    }
    char line[1024];
    uint32_t range_lo = 0;
    bool in_range = false;
    while (fgets(line, sizeof line, in)) {
        char *f[15];
        int n = split_fields(line, f, 15);
        if (n < 6) {
            continue;
        }
        uint32_t cp = parse_hex(f[0]);
        char *name = f[1];
        if (strstr(name, ", First>") != nullptr) {
            range_lo = cp;
            in_range = true;
            continue;
        }
        uint32_t lo = cp;
        if (in_range && strstr(name, ", Last>") != nullptr) {
            lo = range_lo;
            in_range = false;
        }
        uint32_t hi = cp;
        uint8_t ccc = (uint8_t)atoi(f[3]);
        fill_range(dense_ccc, lo, hi, ccc);
        fill_range(dense_bidi, lo, hi, bidi_code(f[4]));
        fill_range(dense_mark, lo, hi, f[2][0] == 'M' ? 1 : 0);
        /* Canonical decompositions only: a tag in angle brackets is a compatibility mapping. */
        if (f[5][0] != '\0' && f[5][0] != '<') {
            uint32_t parts[2] = {0, 0};
            int np = 0;
            for (char *tok = strtok(f[5], " "); tok && np < 2; tok = strtok(nullptr, " ")) {
                parts[np++] = parse_hex(tok);
            }
            if (ndecomps == capdecomps) {
                capdecomps = capdecomps ? capdecomps * 2 : 4096;
                decomps = realloc(decomps, capdecomps * sizeof *decomps);
                if (!decomps) {
                    die("out of memory", nullptr);
                }
            }
            decomps[ndecomps++] = (decomp_rec){cp, parts[0], parts[1]};
        }
    }
    fclose(in);
}

static void read_joining(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/JoiningType.txt", dir);
    FILE *in = fopen(path, "rb");
    if (!in) {
        die("cannot open ", path);
    }
    char line[256];
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        /* start TAB end TAB type; an empty end field means a single code point. */
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        if (t2 == nullptr) {
            die("malformed JoiningType.txt line: ", line);
        }
        *t1 = '\0';
        *t2 = '\0';
        uint32_t lo = parse_hex(line);
        uint32_t hi = t1[1] != '\0' ? parse_hex(t1 + 1) : lo;
        uint8_t code;
        switch (t2[1]) {
        case 'T': code = JT_T; break;
        case 'R': code = JT_R; break;
        case 'L': code = JT_L; break;
        case 'D': code = JT_D; break;
        case 'C': code = JT_C; break;
        default:
            die("bad joining type in JoiningType.txt: ", line);
            code = JT_U;
        }
        fill_range(dense_join, lo, hi, code);
    }
    fclose(in);
}

static void read_composition_exclusions(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/DerivedNormalizationProps.txt", dir);
    FILE *in = fopen(path, "rb");
    if (!in) {
        die("cannot open ", path);
    }
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        if (strstr(line, "; Full_Composition_Exclusion") == nullptr) {
            continue;
        }
        char *dots = strstr(line, "..");
        uint32_t lo, hi;
        if (dots) {
            *dots = '\0';
            lo = parse_hex(line);
            hi = parse_hex(dots + 2);
        } else {
            lo = hi = parse_hex(line);
        }
        fill_range(dense_excl, lo, hi, 1);
    }
    fclose(in);
}

/* ------------------------------------------------------------------ byte streams */

typedef struct bytes {
    uint8_t *data;
    size_t len, cap;
} bytes;

static void b_push(bytes *b, uint8_t v)
{
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->data = realloc(b->data, b->cap);
        if (!b->data) {
            die("out of memory", nullptr);
        }
    }
    b->data[b->len++] = v;
}

/* LEB128: seven bits per byte, low group first, the high bit says another byte follows. */
static void b_varint(bytes *b, uint32_t v)
{
    while (v >= 0x80) {
        b_push(b, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    b_push(b, (uint8_t)v);
}

static uint32_t zigzag(int64_t d)
{
    return (uint32_t)(((uint64_t)d << 1) ^ (uint64_t)(d >> 63));
}

static void emit_bytes(FILE *o, const char *name, const bytes *b)
{
    fprintf(o, "static const uint8_t %s[%zu] = {\n", name, b->len);
    for (size_t i = 0; i < b->len; i++) {
        fprintf(o, "%s%u,", (i % 16 == 0) ? "    " : "", b->data[i]);
        fputc(i % 16 == 15 || i + 1 == b->len ? '\n' : ' ', o);
    }
    fprintf(o, "};\n\n");
}

static void emit_u32(FILE *o, const char *name, const uint32_t *v, size_t n)
{
    fprintf(o, "static const uint32_t %s[%zu] = {\n", name, n);
    for (size_t i = 0; i < n; i++) {
        fprintf(o, "%s0x%X,", (i % 8 == 0) ? "    " : "", v[i]);
        fputc(i % 8 == 7 || i + 1 == n ? '\n' : ' ', o);
    }
    fprintf(o, "};\n\n");
}

static void emit_u16(FILE *o, const char *name, const uint32_t *v, size_t n)
{
    fprintf(o, "static const uint16_t %s[%zu] = {\n", name, n);
    for (size_t i = 0; i < n; i++) {
        if (v[i] > 0xFFFF) {
            die("u16 overflow in ", name);
        }
        fprintf(o, "%s0x%X,", (i % 12 == 0) ? "    " : "", v[i]);
        fputc(i % 12 == 11 || i + 1 == n ? '\n' : ' ', o);
    }
    fprintf(o, "};\n\n");
}

/* A sample is taken every so many entries, so a lookup binary-searches the samples and then
 * decodes at most that many entries. */
#define MAP_SAMPLE 64
#define PROP_SAMPLE 32

/* A growing list of (code point, byte offset) samples. */
typedef struct samples {
    uint32_t *cp, *off;
    size_t n;
} samples;

static void samples_add(samples *s, uint32_t cp, size_t off)
{
    s->cp = realloc(s->cp, (s->n + 1) * sizeof *s->cp);
    s->off = realloc(s->off, (s->n + 1) * sizeof *s->off);
    if (!s->cp || !s->off) {
        die("out of memory", nullptr);
    }
    s->cp[s->n] = cp;
    s->off[s->n] = (uint32_t)off;
    s->n++;
}

/* ------------------------------------------------------------------ the mapping stream */

/* Stream layout (decoded by idna_lookup in src/loader/idna.c). Entries are in code point order;
 * everything not covered by an entry is disallowed. An entry is
 *   varint gap      code points skipped (disallowed) since the end of the previous entry
 *   varint t        (len - 1) << 3 | kind
 *   payload         by kind
 * kind 0 VALID and 1 IGNORED have no payload. Kind 2 DELTA: varint zigzag(d); every cp in the entry
 * maps to cp + d. Kind 3 SEQ: a sequence inline, byte n then n varint code points; every cp in the
 * entry maps to that sequence. Kind 4 SEQREF: varint offset of an earlier inline sequence's n
 * byte. Samples (the code point where an entry's gap starts, the byte offset) are taken every 64
 * entries. */
enum { K_VALID = 0, K_IGNORED = 1, K_DELTA = 2, K_SEQ = 3, K_SEQREF = 4 };

typedef struct seq_ref {
    size_t seq_at;
    uint32_t pos;
} seq_ref;

static size_t seq_len(size_t at)
{
    size_t n = 0;
    while (seqs[at + n] != 0) {
        n++;
    }
    return n;
}

static bool seq_equal(size_t a, size_t b)
{
    size_t na = seq_len(a);
    return na == seq_len(b) && memcmp(&seqs[a], &seqs[b], na * sizeof seqs[0]) == 0;
}

static void emit_table(const char *out_path)
{
    bytes st = {0};
    samples sm = {0};
    seq_ref *refs = nullptr;
    size_t nrefs = 0;
    uint32_t cur = 0;
    for (size_t i = 0; i < nitems; i++) {
        const item *it = &items[i];
        if (i % MAP_SAMPLE == 0) {
            samples_add(&sm, cur, st.len);
        }
        uint32_t kind = K_VALID;
        switch (it->mkind) {
        case M_VALID: kind = K_VALID; break;
        case M_IGNORED: kind = K_IGNORED; break;
        case M_DELTA: kind = K_DELTA; break;
        default: kind = K_SEQ; break;
        }
        const seq_ref *found = nullptr;
        if (it->mkind == M_SEQ) {
            for (size_t r = 0; r < nrefs; r++) {
                if (seq_equal(refs[r].seq_at, it->seq_at)) {
                    found = &refs[r];
                    kind = K_SEQREF;
                    break;
                }
            }
        }
        b_varint(&st, it->lo - cur);
        b_varint(&st, ((it->hi - it->lo) << 3) | kind);
        if (kind == K_DELTA) {
            b_varint(&st, zigzag((int64_t)it->dst - (int64_t)it->lo));
        } else if (kind == K_SEQREF) {
            b_varint(&st, found->pos);
        } else if (kind == K_SEQ) {
            refs = realloc(refs, (nrefs + 1) * sizeof *refs);
            if (!refs) {
                die("out of memory", nullptr);
            }
            refs[nrefs++] = (seq_ref){it->seq_at, (uint32_t)st.len};
            size_t n = seq_len(it->seq_at);
            b_push(&st, (uint8_t)n);
            for (size_t k = 0; k < n; k++) {
                b_varint(&st, seqs[it->seq_at + k]);
            }
        }
        cur = it->hi + 1;
    }
    if (st.len > 0xFFFF) {
        die("mapping stream exceeds 64 KB", nullptr);
    }
    FILE *o = fopen(out_path, "wb");
    if (!o) {
        die("cannot write ", out_path);
    }
    fprintf(o, "/* Generated by tools/unicodegen/idnagen.c from IdnaMappingTable.txt (Unicode 17.0.0).\n");
    fprintf(o, " * Do not edit. Regenerate with: cmake --build build/<dir> --target unicodeidna */\n");
    emit_bytes(o, "embi_idna_map_stream", &st);
    emit_u32(o, "embi_idna_map_samp_cp", sm.cp, sm.n);
    emit_u16(o, "embi_idna_map_samp_off", sm.off, sm.n);
    fclose(o);
    fprintf(stderr, "idnagen: %zu entries, %zu stream bytes, %zu samples, %zu distinct sequences\n", nitems,
            st.len, sm.n, nrefs);
}

/* ------------------------------------------------------------------ properties */

/* Code points that can reach the property lookups: the valid ones (deviations included), the
 * targets of every mapping, and (added by the caller) both parts and the result of every
 * composition. Everything else is a "don't care", which lets the runs below stay long. */
static uint8_t *dense_rel;

static void mark_relevant(void)
{
    for (size_t i = 0; i < nitems; i++) {
        const item *it = &items[i];
        switch (it->mkind) {
        case M_VALID:
            fill_range(dense_rel, it->lo, it->hi, 1);
            break;
        case M_DELTA:
            for (uint32_t c = it->lo; c <= it->hi; c++) {
                dense_rel[it->dst + (c - it->lo)] = 1;
            }
            break;
        case M_SEQ:
            for (size_t k = it->seq_at; seqs[k] != 0; k++) {
                dense_rel[seqs[k]] = 1;
            }
            break;
        default:
            break;
        }
    }
}

typedef struct tuple {
    uint8_t ccc, bj; /* bj = bidi | joining << 4 | mark << 7 */
} tuple;

static tuple tuples[256];
static size_t ntuples;

static uint32_t tuple_id(tuple t)
{
    for (size_t i = 0; i < ntuples; i++) {
        if (tuples[i].ccc == t.ccc && tuples[i].bj == t.bj) {
            return (uint32_t)i;
        }
    }
    if (ntuples == 256) {
        die("more than 256 property tuples", nullptr);
    }
    tuples[ntuples] = t;
    return (uint32_t)ntuples++;
}

static int decomp_cmp_cp(const void *x, const void *y)
{
    const decomp_rec *p = x, *q = y;
    return p->cp < q->cp ? -1 : (p->cp > q->cp ? 1 : 0);
}

static int pair_index_cmp(const void *x, const void *y)
{
    const decomp_rec *p = &decomps[*(const uint32_t *)x], *q = &decomps[*(const uint32_t *)y];
    if (p->a != q->a) {
        return p->a < q->a ? -1 : 1;
    }
    return p->b < q->b ? -1 : (p->b > q->b ? 1 : 0);
}

static void emit_tuple_array(FILE *o, const char *name, bool bj)
{
    fprintf(o, "static const uint8_t %s[%zu] = {\n", name, ntuples);
    for (size_t i = 0; i < ntuples; i++) {
        fprintf(o, "%s%u,", (i % 16 == 0) ? "    " : "", bj ? tuples[i].bj : tuples[i].ccc);
        fputc(i % 16 == 15 || i + 1 == ntuples ? '\n' : ' ', o);
    }
    fprintf(o, "};\n\n");
}

static void emit_props(const char *dir, const char *out_path)
{
    dense_ccc = calloc(CP_LIMIT, 1);
    dense_bidi = calloc(CP_LIMIT, 1);
    dense_join = calloc(CP_LIMIT, 1);
    dense_mark = calloc(CP_LIMIT, 1);
    dense_excl = calloc(CP_LIMIT, 1);
    dense_rel = calloc(CP_LIMIT, 1);
    uint8_t *cls = calloc(CP_LIMIT + 1, 1);
    if (!dense_ccc || !dense_bidi || !dense_join || !dense_mark || !dense_excl || !dense_rel || !cls) {
        die("out of memory", nullptr);
    }
    read_unicode_data(dir);
    read_joining(dir);
    read_composition_exclusions(dir);
    mark_relevant();
    qsort(decomps, ndecomps, sizeof *decomps, decomp_cmp_cp);

    /* Only the decompositions that NFC can still meet are kept: pairs that are not excluded from
     * composition. A singleton or an excluded composite is never NFC-stable, and every valid or
     * mapped code point is, so one of those in the relevant set is a generator error. */
    size_t nall = ndecomps;
    decomp_rec *all_decomps = malloc((nall ? nall : 1) * sizeof *all_decomps);
    if (!all_decomps) {
        die("out of memory", nullptr);
    }
    memcpy(all_decomps, decomps, nall * sizeof *all_decomps);
    size_t kept = 0;
    for (size_t i = 0; i < ndecomps; i++) {
        const decomp_rec *d = &decomps[i];
        bool keep = d->b != 0 && !dense_excl[d->cp];
        if (!keep && dense_rel[d->cp]) {
            fprintf(stderr, "idnagen: U+%04X has an NFC-unstable decomposition but is valid or a mapping target\n",
                    d->cp);
            exit(1);
        }
        if (keep) {
            decomps[kept++] = *d;
        }
    }
    /* idna.c's recursive decomposition treats a part that is not a kept pair as final. That is only
     * right when no part of a kept pair is a singleton or an excluded composite (which would still
     * decompose); check it against the unfiltered list, which was sorted by cp. */
    for (size_t i = 0; i < kept; i++) {
        uint32_t parts[2] = {decomps[i].a, decomps[i].b};
        for (int k = 0; k < 2; k++) {
            decomp_rec key = {parts[k], 0, 0};
            const decomp_rec *hit = bsearch(&key, all_decomps, nall, sizeof key, decomp_cmp_cp);
            if (hit != nullptr && !(hit->b != 0 && !dense_excl[hit->cp])) {
                fprintf(stderr, "idnagen: U+%04X, a part of U+%04X, is a dropped (singleton or excluded) decomposition\n",
                        parts[k], decomps[i].cp);
                exit(1);
            }
        }
    }
    free(all_decomps);
    ndecomps = kept;
    for (size_t i = 0; i < ndecomps; i++) {
        const decomp_rec *d = &decomps[i];
        if (d->cp > 0x1FFFF || d->a > 0x1FFFF || d->b > 0x1FFFF) {
            die("decomposition beyond plane 1", nullptr);
        }
        dense_rel[d->cp] = dense_rel[d->a] = dense_rel[d->b] = 1;
    }

    /* Tuple per relevant code point, don't-cares take the previous value, nothing after the last
     * relevant code point (the lookup returns tuple 0 past the end of the stream). */
    uint32_t last_rel = 0;
    for (uint32_t c = 0; c < CP_LIMIT; c++) {
        if (dense_rel[c]) {
            last_rel = c;
        }
    }
    (void)tuple_id((tuple){0, 0});
    uint8_t prev = 0;
    for (uint32_t c = 0; c <= last_rel; c++) {
        if (dense_rel[c]) {
            tuple t = {dense_ccc[c], (uint8_t)(dense_bidi[c] | (dense_join[c] << 4) | (dense_mark[c] << 7))};
            prev = (uint8_t)tuple_id(t);
        }
        cls[c] = prev;
    }
    /* Runs: varint length, byte tuple id. */
    bytes runs = {0};
    samples sm = {0};
    size_t nruns = 0;
    uint32_t start = 0;
    for (uint32_t c = 1; c <= last_rel + 1; c++) {
        if (c == last_rel + 1 || cls[c] != cls[start]) {
            if (nruns % PROP_SAMPLE == 0) {
                samples_add(&sm, start, runs.len);
            }
            b_varint(&runs, c - start);
            b_push(&runs, cls[start]);
            nruns++;
            start = c;
        }
    }
    if (runs.len > 0xFFFF) {
        die("property stream exceeds 64 KB", nullptr);
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) {
        die("cannot write ", out_path);
    }
    fprintf(o, "/* Generated by tools/unicodegen/idnagen.c from UnicodeData.txt, DerivedNormalizationProps.txt\n");
    fprintf(o, " * and JoiningType.txt (see that file for the joining type source).\n");
    fprintf(o, " * Do not edit. Regenerate with: cmake --build build/<dir> --target unicodeidna */\n");
    /* Tuples: the combining class, and bidi | joining << 4 | mark << 7. */
    emit_tuple_array(o, "embi_idna_prop_ccc", false);
    emit_tuple_array(o, "embi_idna_prop_bj", true);
    emit_bytes(o, "embi_idna_prop_stream", &runs);
    emit_u32(o, "embi_idna_prop_samp_cp", sm.cp, sm.n);
    emit_u16(o, "embi_idna_prop_samp_off", sm.off, sm.n);

    /* Compositions, sorted by cp, as parallel u16 arrays plus the plane bits (1 cp, 2 a, 4 b). */
    uint32_t *v = malloc((ndecomps ? ndecomps : 1) * sizeof *v);
    if (!v) {
        die("out of memory", nullptr);
    }
    for (size_t i = 0; i < ndecomps; i++) {
        v[i] = decomps[i].cp & 0xFFFF;
    }
    emit_u16(o, "embi_idna_dc_cp", v, ndecomps);
    for (size_t i = 0; i < ndecomps; i++) {
        v[i] = decomps[i].a & 0xFFFF;
    }
    emit_u16(o, "embi_idna_dc_a", v, ndecomps);
    for (size_t i = 0; i < ndecomps; i++) {
        v[i] = decomps[i].b & 0xFFFF;
    }
    emit_u16(o, "embi_idna_dc_b", v, ndecomps);
    bytes hi = {0};
    for (size_t i = 0; i < ndecomps; i++) {
        b_push(&hi, (uint8_t)((decomps[i].cp >> 16) | ((decomps[i].a >> 16) << 1) | ((decomps[i].b >> 16) << 2)));
    }
    emit_bytes(o, "embi_idna_dc_hi", &hi);
    /* Indexes into the above, sorted by (a, b), for composition. */
    for (size_t i = 0; i < ndecomps; i++) {
        v[i] = (uint32_t)i;
    }
    qsort(v, ndecomps, sizeof *v, pair_index_cmp);
    emit_u16(o, "embi_idna_dc_by_pair", v, ndecomps);
    fclose(o);
    fprintf(stderr, "idnagen: props: %zu tuples, %zu runs (%zu bytes), %zu compositions\n", ntuples, nruns,
            runs.len, ndecomps);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: idnagen <data-dir> <idna_table.inc> <idna_props.inc>\n");
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/IdnaMappingTable.txt", argv[1]);
    FILE *in = fopen(path, "rb");
    if (!in) {
        die("cannot open ", path);
    }
    /* seqs entry 0 is a reserved zero, so a sequence offset is never 0. */
    seq_push(0);

    char line[4096];
    while (fgets(line, sizeof line, in)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *body = trim(line);
        if (*body == '\0') {
            continue;
        }
        char *f[3] = {nullptr, nullptr, nullptr};
        int nf = 0;
        for (char *p = body; nf < 3;) {
            char *semi = strchr(p, ';');
            if (semi) {
                *semi = '\0';
            }
            f[nf++] = trim(p);
            if (!semi) {
                break;
            }
            p = semi + 1;
        }
        if (nf < 2) {
            die("malformed line in IdnaMappingTable.txt: ", body);
        }
        item it = {0};
        char *dots = strstr(f[0], "..");
        if (dots) {
            *dots = '\0';
            it.lo = (uint32_t)strtoul(f[0], nullptr, 16);
            it.hi = (uint32_t)strtoul(dots + 2, nullptr, 16);
        } else {
            it.lo = it.hi = (uint32_t)strtoul(f[0], nullptr, 16);
        }
        if (strcmp(f[1], "valid") == 0 || strcmp(f[1], "deviation") == 0) {
            it.mkind = M_VALID;
        } else if (strcmp(f[1], "ignored") == 0) {
            it.mkind = M_IGNORED;
        } else if (strcmp(f[1], "mapped") == 0) {
            if (!f[2]) {
                die("mapped entry without a mapping: ", body);
            }
            uint32_t cps[32];
            size_t ncp = 0;
            for (char *tok = strtok(f[2], " "); tok; tok = strtok(nullptr, " ")) {
                if (ncp == sizeof cps / sizeof cps[0]) {
                    die("mapping too long: ", body);
                }
                cps[ncp++] = (uint32_t)strtoul(tok, nullptr, 16);
            }
            /* A range shares one mapping (every code point maps to the same sequence), so only
             * a single code point with a single target can be a delta. */
            if (ncp == 1 && it.lo == it.hi) {
                it.mkind = M_DELTA;
                it.dst = cps[0];
            } else {
                it.mkind = M_SEQ;
                it.seq_at = (uint32_t)nseqs;
                for (size_t i = 0; i < ncp; i++) {
                    seq_push(cps[i]);
                }
                seq_push(0);
            }
        } else {
            /* disallowed: absent from the items, so it is the default. */
            continue;
        }
        if (!try_merge(&it)) {
            item_push(it);
        }
    }
    fclose(in);

    emit_table(argv[2]);
    emit_props(argv[1], argv[3]);
    return 0;
}
