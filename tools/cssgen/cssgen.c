/*
 * cssgen: reads src/css/props.txt and writes src/css/props.inc, the property and keyword tables
 * used by the CSS value parser (src/css/value.c, shorthand.c).
 *
 * props.txt has one property per line. Blank lines and lines starting with '#' are skipped. A line
 * "@kw word word ..." declares extra keywords that no property lists (grammar internals). Each
 * property line has ten or eleven '|' separated fields:
 *
 *     name | kind | inherited | keywords | types | initial | anim | interp | group | longhands [| logic]
 *
 *   kind        L (longhand) or S (shorthand)
 *   inherited   0 or 1
 *   keywords    comma-separated keyword idents accepted as a whole value, or '-'
 *   types       comma-separated value types (len pct lenpct num int pint alpha angle time res color
 *               url str ident list), or '-'. A '+' anywhere in the field makes the numeric types
 *               non-negative. 'list' makes the value a comma-separated list of the grammar
 *               item. 'g:<name>' selects a compound grammar function (embi_gram_<name> in
 *               src/css/value_*.c), which then parses the value or each list item.
 *   initial     CSS text of the initial value (parsed on demand), or '-' for shorthands
 *   anim        animation type: N (not animatable), D (discrete), L (length-percentage), F (number),
 *               C (color), X (compound: the interp kind decides how the value mixes)
 *   interp      interpolation kind (props.h EMBI_IP_*): - (discrete), len, num, int, color, shadow,
 *               filter, xform, shape, pos, axis, lplist, vis, disp, cv or ratio
 *   group       style struct group name (for W2d), or '-'
 *   longhands   comma-separated longhand names for a shorthand, or '-'
 *   logic       optional logical-property metadata: P:<kind>.<family>:<role> for a physical
 *               longhand, L:<kind>.<family>:<role> for its flow-relative counterpart. kind is box
 *               (roles top right bottom left | block-start block-end inline-start inline-end),
 *               size (width height | inline block) or corner (top-left top-right bottom-right
 *               bottom-left | start-start start-end end-start end-end). Properties of one family
 *               map onto each other per writing mode (props.h, embi_prop_to_physical).
 *
 * The output defines:
 *   - enum EMBI_PROP_<name> (with '-' mapped to '_') and EMBI_KW_<keyword> (ids in sorted keyword
 *     order), plus the *_COUNT values;
 *   - the tables, only when EMBI_PROPS_TABLES is defined (in exactly one translation unit):
 *     embi_prop_infos, embi_prop_kw_list, embi_prop_sub_list, embi_kw_names, and the sorted
 *     (name, id) tables used for binary search.
 *
 * Usage: cssgen <props.txt> <props.inc>
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct prop {
    char *name;
    char kind;
    bool inh;
    char **kws;
    uint32_t nkws;
    uint32_t types;
    bool nonneg;
    bool list;
    char *initial;
    char anim;
    const char *interp; /* EMBI_IP_* suffix, from the interp field */
    char *group;
    char **subs;
    uint32_t nsubs;
    uint32_t id;
    char *grammar;      /* g:<name> */
    char *logic_fam;    /* "kind.family" or nullptr */
    char *logic_role;
    bool logic_logical;
} prop;

/* Must match the EMBI_T_* values in src/css/props.h. */
enum {
    T_LEN = 1u << 0,
    T_PCT = 1u << 1,
    T_NUM = 1u << 2,
    T_INT = 1u << 3,
    T_ALPHA = 1u << 4,
    T_ANGLE = 1u << 5,
    T_TIME = 1u << 6,
    T_RES = 1u << 7,
    T_COLOR = 1u << 8,
    T_URL = 1u << 9,
    T_STR = 1u << 10,
    T_IDENT = 1u << 11,
    T_NONNEG = 1u << 12,
    T_LIST = 1u << 13,
    T_POSINT = 1u << 14,
};

static prop *props;
static uint32_t nprops;
static uint32_t capprops;
static char **gram_all;
static uint32_t ngram_all;
static char **fam_all;
static uint32_t nfam_all;
static char **kw_all;
static uint32_t nkw_all;
static uint32_t capkw_all;

[[noreturn]] static void fail(const char *what, const char *arg, unsigned line)
{
    fprintf(stderr, "cssgen: line %u: %s%s%s\n", line, what, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

[[noreturn]] static void oom(void)
{
    fprintf(stderr, "cssgen: out of memory\n");
    exit(1);
}

static char *dup_n(const char *p, size_t n)
{
    char *s = malloc(n + 1);
    if (!s) {
        oom();
    }
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

static char *trim_dup(const char *p, size_t n)
{
    while (n > 0 && isspace((unsigned char)*p)) {
        p++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)p[n - 1])) {
        n--;
    }
    return dup_n(p, n);
}

/* Splits s on sep. Returns the count and the strdup'd pieces (empty pieces are kept). */
static char **split(const char *s, char sep, uint32_t *count)
{
    uint32_t n = 1;
    for (const char *p = s; *p; p++) {
        if (*p == sep) {
            n++;
        }
    }
    char **out = malloc(sizeof(char *) * n);
    if (!out) {
        oom();
    }
    uint32_t i = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p == sep || *p == '\0') {
            out[i++] = trim_dup(start, (size_t)(p - start));
            if (*p == '\0') {
                break;
            }
            start = p + 1;
        }
    }
    *count = n;
    return out;
}

static bool valid_ident(const char *s)
{
    if (!*s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-')) {
            return false;
        }
    }
    return true;
}

static void add_kw(const char *k)
{
    for (uint32_t i = 0; i < nkw_all; i++) {
        if (strcmp(kw_all[i], k) == 0) {
            return;
        }
    }
    if (nkw_all == capkw_all) {
        capkw_all = capkw_all ? capkw_all * 2 : 256;
        kw_all = realloc(kw_all, sizeof(char *) * capkw_all);
        if (!kw_all) {
            oom();
        }
    }
    kw_all[nkw_all++] = dup_n(k, strlen(k));
}

static void add_unique(char ***arr, uint32_t *n, const char *k)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (strcmp((*arr)[i], k) == 0) {
            return;
        }
    }
    *arr = realloc(*arr, sizeof(char *) * (*n + 1));
    if (!*arr) {
        oom();
    }
    (*arr)[(*n)++] = dup_n(k, strlen(k));
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static uint32_t type_bits(const char *field, unsigned line, bool *nonneg, bool *list, char **grammar)
{
    uint32_t bits = 0;
    if (strchr(field, '+')) {
        *nonneg = true;
    }
    if (strcmp(field, "-") == 0) {
        return 0;
    }
    uint32_t n = 0;
    char **toks = split(field, ',', &n);
    for (uint32_t i = 0; i < n; i++) {
        const char *t = toks[i];
        if (*t == '+') {
            t++;
        }
        if (strcmp(t, "len") == 0) {
            bits |= T_LEN;
        } else if (strcmp(t, "pct") == 0) {
            bits |= T_PCT;
        } else if (strcmp(t, "lenpct") == 0) {
            bits |= T_LEN | T_PCT;
        } else if (strcmp(t, "num") == 0) {
            bits |= T_NUM;
        } else if (strcmp(t, "int") == 0) {
            bits |= T_INT;
        } else if (strcmp(t, "pint") == 0) {
            bits |= T_INT | T_POSINT;
        } else if (strcmp(t, "alpha") == 0) {
            bits |= T_ALPHA;
        } else if (strcmp(t, "angle") == 0) {
            bits |= T_ANGLE;
        } else if (strcmp(t, "time") == 0) {
            bits |= T_TIME;
        } else if (strcmp(t, "res") == 0) {
            bits |= T_RES;
        } else if (strcmp(t, "color") == 0) {
            bits |= T_COLOR;
        } else if (strcmp(t, "url") == 0) {
            bits |= T_URL;
        } else if (strcmp(t, "str") == 0) {
            bits |= T_STR;
        } else if (strcmp(t, "ident") == 0) {
            bits |= T_IDENT;
        } else if (strcmp(t, "list") == 0) {
            *list = true;
        } else if (strncmp(t, "g:", 2) == 0) {
            *grammar = dup_n(t + 2, strlen(t + 2));
        } else {
            fail("unknown value type", t, line);
        }
    }
    return bits;
}

static const char *interp_token(const char *tok, unsigned line, const char *name);

static void read_props(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cssgen: cannot open %s\n", path);
        exit(1);
    }
    char buf[8192];
    unsigned line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *p = buf;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (strncmp(p, "@kw", 3) == 0 && isspace((unsigned char)p[3])) {
            p += 3;
            while (*p) {
                while (isspace((unsigned char)*p)) {
                    p++;
                }
                const char *w = p;
                while (*p && !isspace((unsigned char)*p)) {
                    p++;
                }
                if (p > w) {
                    char *kw = dup_n(w, (size_t)(p - w));
                    if (!valid_ident(kw)) {
                        fail("bad keyword", kw, line);
                    }
                    add_kw(kw);
                }
            }
            continue;
        }
        uint32_t nf = 0;
        char **f9 = split(p, '|', &nf);
        if (nf != 10 && nf != 11) {
            fprintf(stderr, "cssgen: line %u: expected 10 or 11 fields, got %u\n", line, nf);
            exit(1);
        }
        if (nprops == capprops) {
            capprops = capprops ? capprops * 2 : 128;
            props = realloc(props, sizeof(prop) * capprops);
            if (!props) {
                oom();
            }
        }
        prop *pr = &props[nprops++];
        memset(pr, 0, sizeof *pr);
        pr->name = f9[0];
        if (!valid_ident(pr->name)) {
            fail("bad property name", pr->name, line);
        }
        if (strcmp(f9[1], "L") == 0) {
            pr->kind = 'L';
        } else if (strcmp(f9[1], "S") == 0) {
            pr->kind = 'S';
        } else {
            fail("kind must be L or S", f9[1], line);
        }
        if (strcmp(f9[2], "0") == 0) {
            pr->inh = false;
        } else if (strcmp(f9[2], "1") == 0) {
            pr->inh = true;
        } else {
            fail("inherited must be 0 or 1", f9[2], line);
        }
        if (strcmp(f9[3], "-") != 0) {
            pr->kws = split(f9[3], ',', &pr->nkws);
            for (uint32_t i = 0; i < pr->nkws; i++) {
                if (!valid_ident(pr->kws[i])) {
                    fail("bad keyword", pr->kws[i], line);
                }
                add_kw(pr->kws[i]);
            }
        }
        pr->types = type_bits(f9[4], line, &pr->nonneg, &pr->list, &pr->grammar);
        if (pr->grammar) {
            add_unique(&gram_all, &ngram_all, pr->grammar);
        }
        if (nf == 11 && strcmp(f9[10], "-") != 0) {
            /* P:kind.family:role or L:kind.family:role */
            if ((f9[10][0] != 'P' && f9[10][0] != 'L') || f9[10][1] != ':') {
                fail("bad logic field", f9[10], line);
            }
            pr->logic_logical = f9[10][0] == 'L';
            char *colon = strchr(f9[10] + 2, ':');
            if (!colon) {
                fail("bad logic field", f9[10], line);
            }
            pr->logic_fam = dup_n(f9[10] + 2, (size_t)(colon - (f9[10] + 2)));
            pr->logic_role = dup_n(colon + 1, strlen(colon + 1));
            add_unique(&fam_all, &nfam_all, pr->logic_fam);
        }
        if (pr->nonneg) {
            pr->types |= T_NONNEG;
        }
        pr->initial = strcmp(f9[5], "-") == 0 ? nullptr : dup_n(f9[5], strlen(f9[5]));
        if (pr->kind == 'L' && !pr->initial) {
            fail("longhand needs an initial value", pr->name, line);
        }
        if (pr->kind == 'S' && strcmp(f9[6], "-") == 0) {
            pr->anim = 'N';
        } else if (strlen(f9[6]) != 1 || !strchr("NLFCDX", f9[6][0])) {
            fail("anim must be one of N L F C D X", f9[6], line);
        } else {
            pr->anim = f9[6][0];
        }
        pr->interp = interp_token(f9[7], line, pr->name);
        pr->group = strcmp(f9[8], "-") == 0 ? nullptr : dup_n(f9[8], strlen(f9[8]));
        if (pr->kind == 'S') {
            if (strcmp(f9[9], "-") == 0) {
                fail("shorthand needs longhands", pr->name, line);
            }
            pr->subs = split(f9[9], ',', &pr->nsubs);
        } else if (strcmp(f9[9], "-") != 0) {
            fail("longhand must not list longhands", pr->name, line);
        }
    }
    fclose(f);
}

/* Maps an interp token to its EMBI_IP_* suffix (must match the enum in src/css/props.h). */
static const char *interp_token(const char *tok, unsigned line, const char *name)
{
    static const struct {
        const char *tok, *suffix;
    } map[] = {
        {"-", "NONE"}, {"len", "LEN"}, {"num", "NUM"}, {"int", "INT"}, {"color", "COLOR"},
        {"shadow", "SHADOW"}, {"filter", "FILTER"}, {"xform", "XFORM"}, {"shape", "SHAPE"},
        {"pos", "POS"}, {"axis", "AXIS"}, {"lplist", "LPLIST"}, {"vis", "VIS"},
        {"disp", "DISP"}, {"cv", "CV"}, {"ratio", "RATIO"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(map[i].tok, tok) == 0) {
            return map[i].suffix;
        }
    }
    fail("bad interp kind", tok, line);
    (void)name;
    return nullptr;
}

static const prop *find_prop(const char *name)
{
    for (uint32_t i = 0; i < nprops; i++) {
        if (strcmp(props[i].name, name) == 0) {
            return &props[i];
        }
    }
    return nullptr;
}

static void check_props(void)
{
    for (uint32_t i = 0; i < nprops; i++) {
        for (uint32_t j = i + 1; j < nprops; j++) {
            if (strcmp(props[i].name, props[j].name) == 0) {
                fprintf(stderr, "cssgen: duplicate property %s\n", props[i].name);
                exit(1);
            }
        }
        for (uint32_t s = 0; s < props[i].nsubs; s++) {
            const prop *sub = find_prop(props[i].subs[s]);
            if (!sub || sub->kind != 'L') {
                fprintf(stderr, "cssgen: %s lists unknown or non-longhand %s\n", props[i].name,
                        props[i].subs[s]);
                exit(1);
            }
        }
    }
}

static void cident(FILE *o, const char *s)
{
    for (const char *p = s; *p; p++) {
        fputc(*p == '-' ? '_' : *p, o);
    }
}

static void cstr(FILE *o, const char *s)
{
    fputc('"', o);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', o);
        }
        fputc(*p, o);
    }
    fputc('"', o);
}

static void emit_types(FILE *o, uint32_t bits)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {
        {T_LEN, "EMBI_T_LEN"},       {T_PCT, "EMBI_T_PCT"},     {T_NUM, "EMBI_T_NUM"},
        {T_INT, "EMBI_T_INT"},       {T_ALPHA, "EMBI_T_ALPHA"}, {T_ANGLE, "EMBI_T_ANGLE"},
        {T_TIME, "EMBI_T_TIME"},     {T_RES, "EMBI_T_RES"},     {T_COLOR, "EMBI_T_COLOR"},
        {T_URL, "EMBI_T_URL"},       {T_STR, "EMBI_T_STR"},     {T_IDENT, "EMBI_T_IDENT"},
        {T_NONNEG, "EMBI_T_NONNEG"}, {T_LIST, "EMBI_T_LIST"},
        {T_POSINT, "EMBI_T_POSINT"},
    };
    if (bits == 0) {
        fputs("0", o);
        return;
    }
    bool first = true;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (bits & names[i].bit) {
            fprintf(o, "%s%s", first ? "" : " | ", names[i].name);
            first = false;
        }
    }
}

static int role_index(const char *kind, bool logical, const char *role, unsigned *line_dummy)
{
    (void)line_dummy;
    static const char *const box_p[] = {"top", "right", "bottom", "left"};
    static const char *const box_l[] = {"block-start", "block-end", "inline-start", "inline-end"};
    static const char *const size_p[] = {"width", "height"};
    static const char *const size_l[] = {"inline", "block"};
    static const char *const cor_p[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
    static const char *const cor_l[] = {"start-start", "start-end", "end-start", "end-end"};
    const char *const *tab;
    int n;
    if (strcmp(kind, "box") == 0) {
        tab = logical ? box_l : box_p;
        n = 4;
    } else if (strcmp(kind, "size") == 0) {
        tab = logical ? size_l : size_p;
        n = 2;
    } else if (strcmp(kind, "corner") == 0) {
        tab = logical ? cor_l : cor_p;
        n = 4;
    } else {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(tab[i], role) == 0) {
            return i;
        }
    }
    return -1;
}

static int family_index(const char *fam)
{
    for (uint32_t i = 0; i < nfam_all; i++) {
        if (strcmp(fam_all[i], fam) == 0) {
            return (int)i + 1;
        }
    }
    return 0;
}

static int grammar_index(const char *g)
{
    for (uint32_t i = 0; i < ngram_all; i++) {
        if (strcmp(gram_all[i], g) == 0) {
            return (int)i + 1;
        }
    }
    return 0;
}

static void check_logic(void)
{
    for (uint32_t i = 0; i < nprops; i++) {
        const prop *pr = &props[i];
        if (!pr->logic_fam) {
            continue;
        }
        char kind[16];
        const char *dot = strchr(pr->logic_fam, '.');
        if (!dot || (size_t)(dot - pr->logic_fam) >= sizeof kind) {
            fprintf(stderr, "cssgen: %s: logic family must be kind.name\n", pr->name);
            exit(1);
        }
        memcpy(kind, pr->logic_fam, (size_t)(dot - pr->logic_fam));
        kind[dot - pr->logic_fam] = '\0';
        if (role_index(kind, pr->logic_logical, pr->logic_role, nullptr) < 0) {
            fprintf(stderr, "cssgen: %s: bad logic role %s\n", pr->name, pr->logic_role);
            exit(1);
        }
    }
}

static void write_output(const char *path)
{
    FILE *o = fopen(path, "wb");
    if (!o) {
        fprintf(stderr, "cssgen: cannot write %s\n", path);
        exit(1);
    }
    qsort(kw_all, nkw_all, sizeof(char *), cmp_str);

    fputs("/* Generated by tools/cssgen from src/css/props.txt. Do not edit by hand.\n"
          " * Regenerate with: cmake --build <build dir> --target cssgen */\n\n",
          o);
    fputs("enum {\n    EMBI_PROP_NONE = 0,\n", o);
    for (uint32_t i = 0; i < nprops; i++) {
        fputs("    EMBI_PROP_", o);
        cident(o, props[i].name);
        fprintf(o, " = %u,\n", i + 1);
    }
    fprintf(o, "    EMBI_PROP_COUNT = %u\n};\n\n", nprops + 1);

    fputs("enum {\n    EMBI_KW_INVALID = 0,\n", o);
    for (uint32_t i = 0; i < nkw_all; i++) {
        fputs("    EMBI_KW_", o);
        cident(o, kw_all[i]);
        fprintf(o, " = %u,\n", i + 1);
    }
    fprintf(o, "    EMBI_KW_COUNT = %u\n};\n\n", nkw_all + 1);

    fputs("enum {\n    EMBI_G_NONE = 0,\n", o);
    for (uint32_t i = 0; i < ngram_all; i++) {
        fputs("    EMBI_G_", o);
        cident(o, gram_all[i]);
        fprintf(o, " = %u,\n", i + 1);
    }
    fprintf(o, "    EMBI_G_COUNT = %u\n};\n\n", ngram_all + 1);
    fputs("#define EMBI_GRAMMAR_LIST(X)", o);
    for (uint32_t i = 0; i < ngram_all; i++) {
        fputs(" \\\n    X(", o);
        cident(o, gram_all[i]);
        fputs(")", o);
    }
    fputs("\n\n", o);

    fputs("enum {\n    EMBI_LF_NONE = 0,\n", o);
    for (uint32_t i = 0; i < nfam_all; i++) {
        fputs("    EMBI_LF_", o);
        for (const char *q = fam_all[i]; *q; q++) {
            fputc(*q == '-' || *q == '.' ? '_' : *q, o);
        }
        fprintf(o, " = %u,\n", i + 1);
    }
    fprintf(o, "    EMBI_LF_COUNT = %u\n};\n\n", nfam_all + 1);

    fputs("#ifdef EMBI_PROPS_TABLES\n\n", o);

    fputs("static const char *const embi_kw_names[EMBI_KW_COUNT] = {\n    nullptr,\n", o);
    for (uint32_t i = 0; i < nkw_all; i++) {
        fputs("    ", o);
        cstr(o, kw_all[i]);
        fputs(",\n", o);
    }
    fputs("};\n\n", o);

    /* Flattened keyword and sub-longhand lists. A trailing 0 keeps the arrays non-empty. */
    uint32_t *kw_off = malloc(sizeof(uint32_t) * (nprops + 1));
    uint32_t *sub_off = malloc(sizeof(uint32_t) * (nprops + 1));
    if (!kw_off || !sub_off) {
        oom();
    }
    fputs("static const uint16_t embi_prop_kw_list[] = {\n", o);
    uint32_t off = 0;
    for (uint32_t i = 0; i < nprops; i++) {
        kw_off[i] = off;
        for (uint32_t k = 0; k < props[i].nkws; k++) {
            fputs("    EMBI_KW_", o);
            cident(o, props[i].kws[k]);
            fputs(",\n", o);
            off++;
        }
    }
    fputs("    0,\n};\n\n", o);

    fputs("static const uint16_t embi_prop_sub_list[] = {\n", o);
    off = 0;
    for (uint32_t i = 0; i < nprops; i++) {
        sub_off[i] = off;
        for (uint32_t s = 0; s < props[i].nsubs; s++) {
            fputs("    EMBI_PROP_", o);
            cident(o, props[i].subs[s]);
            fputs(",\n", o);
            off++;
        }
    }
    fputs("    0,\n};\n\n", o);

    fputs("static const embi_prop_info embi_prop_infos[EMBI_PROP_COUNT] = {\n", o);
    fputs("    [EMBI_PROP_NONE] = {0},\n", o);
    for (uint32_t i = 0; i < nprops; i++) {
        const prop *pr = &props[i];
        fputs("    [EMBI_PROP_", o);
        cident(o, pr->name);
        fputs("] = {\n        .name = ", o);
        cstr(o, pr->name);
        fputs(",\n        .initial = ", o);
        if (pr->initial) {
            cstr(o, pr->initial);
        } else {
            fputs("nullptr", o);
        }
        fputs(",\n        .group = ", o);
        if (pr->group) {
            cstr(o, pr->group);
        } else {
            fputs("nullptr", o);
        }
        fputs(",\n        .types = ", o);
        emit_types(o, pr->types | (pr->list ? T_LIST : 0));
        fprintf(o, ",\n        .kw_off = %u, .kw_n = %u,\n", kw_off[i], pr->nkws);
        fprintf(o, "        .sub_off = %u, .sub_n = %u,\n", sub_off[i], pr->nsubs);
        const char *flags = "0";
        if (pr->inh && pr->kind == 'S') {
            flags = "EMBI_PF_INHERITED | EMBI_PF_SHORTHAND";
        } else if (pr->inh) {
            flags = "EMBI_PF_INHERITED";
        } else if (pr->kind == 'S') {
            flags = "EMBI_PF_SHORTHAND";
        }
        fprintf(o, "        .flags = %s,\n", flags);
        fprintf(o, "        .anim = '%c',\n", pr->anim);
        fprintf(o, "        .interp = EMBI_IP_%s,\n", pr->interp);
        if (pr->grammar) {
            fprintf(o, "        .grammar = %d,\n", grammar_index(pr->grammar));
        }
        if (pr->logic_fam) {
            char kind[16];
            const char *dot = strchr(pr->logic_fam, '.');
            memcpy(kind, pr->logic_fam, (size_t)(dot - pr->logic_fam));
            kind[dot - pr->logic_fam] = '\0';
            fprintf(o, "        .logic_family = %d, .logic_role = %d, .logic_logical = %d,\n",
                    family_index(pr->logic_fam),
                    role_index(kind, pr->logic_logical, pr->logic_role, nullptr),
                    pr->logic_logical ? 1 : 0);
        }
        fputs("    },\n", o);
    }
    fputs("};\n\n", o);

    /* Logical property families: kind (0 box, 1 size, 2 corner), and the physical and logical
     * property ids per role. */
    fputs("static const uint8_t embi_logic_kind[EMBI_LF_COUNT] = {\n    0,\n", o);
    for (uint32_t f = 0; f < nfam_all; f++) {
        const char *k = fam_all[f];
        fprintf(o, "    %d,\n", strncmp(k, "box.", 4) == 0 ? 0 : strncmp(k, "size.", 5) == 0 ? 1 : 2);
    }
    fputs("};\n\n", o);
    for (int lg = 0; lg < 2; lg++) {
        fprintf(o, "static const uint16_t embi_logic_%s[EMBI_LF_COUNT][4] = {\n    {0},\n",
                lg ? "log" : "phys");
        for (uint32_t f = 0; f < nfam_all; f++) {
            uint16_t ids[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < nprops; i++) {
                const prop *pr = &props[i];
                if (pr->logic_fam && strcmp(pr->logic_fam, fam_all[f]) == 0 &&
                    pr->logic_logical == (lg != 0)) {
                    char kind[16];
                    const char *dot = strchr(pr->logic_fam, '.');
                    memcpy(kind, pr->logic_fam, (size_t)(dot - pr->logic_fam));
                    kind[dot - pr->logic_fam] = '\0';
                    ids[role_index(kind, pr->logic_logical, pr->logic_role, nullptr)] =
                        (uint16_t)(i + 1);
                }
            }
            fprintf(o, "    {%u, %u, %u, %u},\n", ids[0], ids[1], ids[2], ids[3]);
        }
        fputs("};\n\n", o);
    }

    /* Sorted (name, id) tables for binary search. Insertion sort: the lists are small. */
    const prop **sorted = malloc(sizeof(prop *) * (nprops ? nprops : 1));
    if (!sorted) {
        oom();
    }
    for (uint32_t i = 0; i < nprops; i++) {
        sorted[i] = &props[i];
    }
    for (uint32_t i = 1; i < nprops; i++) {
        for (uint32_t j = i; j > 0 && strcmp(sorted[j - 1]->name, sorted[j]->name) > 0; j--) {
            const prop *t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    }
    fputs("static const embi_name_id embi_prop_name_table[] = {\n", o);
    for (uint32_t i = 0; i < nprops; i++) {
        if (strncmp(sorted[i]->name, "-internal-", 10) == 0) {
            continue; /* internal longhand: has an id, a group slot and an initial value, but no author-visible name */
        }
        fputs("    {", o);
        cstr(o, sorted[i]->name);
        fputs(", EMBI_PROP_", o);
        cident(o, sorted[i]->name);
        fputs("},\n", o);
    }
    fputs("};\n\n", o);

    fputs("static const embi_name_id embi_kw_name_table[] = {\n", o);
    for (uint32_t i = 0; i < nkw_all; i++) {
        fputs("    {", o);
        cstr(o, kw_all[i]);
        fputs(", EMBI_KW_", o);
        cident(o, kw_all[i]);
        fputs("},\n", o);
    }
    fputs("};\n\n", o);
    fputs("#endif /* EMBI_PROPS_TABLES */\n", o);
    free(sorted);
    free(kw_off);
    free(sub_off);
    fclose(o);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: cssgen <props.txt> <props.inc>\n");
        return 2;
    }
    read_props(argv[1]);
    check_props();
    check_logic();
    write_output(argv[2]);
    return 0;
}
