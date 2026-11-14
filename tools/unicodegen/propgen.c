/*
 * propgen: builds src/js/regexp_unicode.inc, the Unicode data behind RegExp \p{...} escapes and
 * the case folding used by the i flag (ECMA-262 Canonicalize, with and without u or v).
 *
 * Usage: propgen <ucd-dir> <output.inc>
 *   <ucd-dir> must contain the UCD 17.0.0 files listed in tools/unicodegen/data.
 *
 * Output:
 *   - Sets: General_Category values (30 leaves), Script and Script_Extensions values (one set each
 *     per script), and the binary properties of ECMA-262 table 67 plus Any, ASCII and Assigned.
 *     Each set is a sorted list of merged inclusive code point ranges, encoded as LEB128 varints:
 *     a count, then for each range the gap from the previous range end and (hi - lo).
 *   - A name table mapping each accepted spelling to a namespace and one or more set members.
 *     Namespaces: lone names (binary properties and General_Category values), gc=, sc= and scx=.
 *   - Case folding for u and v: simple case folding (CaseFolding.txt statuses C and S).
 *   - Canonicalize for non-u patterns: the full toUppercase mapping, kept only when it maps to a
 *     single code unit, and not from a non-ASCII character to an ASCII one (ECMA-262 22.2.2.7.3).
 *
 * This is a host tool and may use stdio. It runs only through the 'unicodeprops' CMake target.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_MAX 0x110000u
#define GC_COUNT 30
#define GC_CN 29
#define MAX_SCRIPTS 256
#define MAX_NAMES 1024
#define MAX_MEMBERS 8

/* Namespaces, matching EMBI_RE_NS_* in the generated header. */
#define NS_LONE 0
#define NS_GC 1
#define NS_SC 2
#define NS_SCX 3

/* Member kinds, resolved to set ids once the script count is known. */
enum { K_GC, K_SC, K_SCX, K_BIN };

/* Binary property sources. */
enum { SRC_PROPLIST, SRC_DCP, SRC_EMOJI, SRC_NORM, SRC_MIRROR, SRC_ASCII, SRC_ANY, SRC_ASSIGNED };

static const char *const gc_names[GC_COUNT] = {
    "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe",
    "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn",
};

/* General_Category groups: members are leaf indices. */
typedef struct {
    const char *short_name;
    uint8_t n;
    uint8_t members[7];
} gc_group;

static const gc_group gc_groups[] = {
    {"L", 5, {0, 1, 2, 3, 4}},
    {"LC", 3, {0, 1, 2}},
    {"M", 3, {5, 6, 7}},
    {"N", 3, {8, 9, 10}},
    {"P", 7, {11, 12, 13, 14, 15, 16, 17}},
    {"S", 4, {18, 19, 20, 21}},
    {"Z", 3, {22, 23, 24}},
    {"C", 5, {25, 26, 27, 28, 29}},
};

/* ECMA-262 table 67 binary properties: long name, short alias, source. */
typedef struct {
    const char *lname;
    const char *sname;
    int src;
} bin_prop;

static const bin_prop bin_props[] = {
    {"ASCII_Hex_Digit", "AHex", SRC_PROPLIST},
    {"Alphabetic", "Alpha", SRC_DCP},
    {"Bidi_Control", "Bidi_C", SRC_PROPLIST},
    {"Bidi_Mirrored", "Bidi_M", SRC_MIRROR},
    {"Case_Ignorable", "CI", SRC_DCP},
    {"Cased", "Cased", SRC_DCP},
    {"Changes_When_Casefolded", "CWCF", SRC_DCP},
    {"Changes_When_Casemapped", "CWCM", SRC_DCP},
    {"Changes_When_Lowercased", "CWL", SRC_DCP},
    {"Changes_When_NFKC_Casefolded", "CWKCF", SRC_NORM},
    {"Changes_When_Titlecased", "CWT", SRC_DCP},
    {"Changes_When_Uppercased", "CWU", SRC_DCP},
    {"Dash", "Dash", SRC_PROPLIST},
    {"Default_Ignorable_Code_Point", "DI", SRC_DCP},
    {"Deprecated", "Dep", SRC_PROPLIST},
    {"Diacritic", "Dia", SRC_PROPLIST},
    {"Emoji", "Emoji", SRC_EMOJI},
    {"Emoji_Component", "EComp", SRC_EMOJI},
    {"Emoji_Modifier", "EMod", SRC_EMOJI},
    {"Emoji_Modifier_Base", "EBase", SRC_EMOJI},
    {"Emoji_Presentation", "EPres", SRC_EMOJI},
    {"Extended_Pictographic", "ExtPict", SRC_EMOJI},
    {"Extender", "Ext", SRC_PROPLIST},
    {"Grapheme_Base", "Gr_Base", SRC_DCP},
    {"Grapheme_Extend", "Gr_Ext", SRC_DCP},
    {"Hex_Digit", "Hex", SRC_PROPLIST},
    {"IDS_Binary_Operator", "IDSB", SRC_PROPLIST},
    {"IDS_Trinary_Operator", "IDST", SRC_PROPLIST},
    {"ID_Continue", "IDC", SRC_DCP},
    {"ID_Start", "IDS", SRC_DCP},
    {"Ideographic", "Ideo", SRC_PROPLIST},
    {"Join_Control", "Join_C", SRC_PROPLIST},
    {"Logical_Order_Exception", "LOE", SRC_PROPLIST},
    {"Lowercase", "Lower", SRC_DCP},
    {"Math", "Math", SRC_DCP},
    {"Noncharacter_Code_Point", "NChar", SRC_PROPLIST},
    {"Pattern_Syntax", "Pat_Syn", SRC_PROPLIST},
    {"Pattern_White_Space", "Pat_WS", SRC_PROPLIST},
    {"Quotation_Mark", "QMark", SRC_PROPLIST},
    {"Radical", "Radical", SRC_PROPLIST},
    {"Regional_Indicator", "RI", SRC_PROPLIST},
    {"Sentence_Terminal", "STerm", SRC_PROPLIST},
    {"Soft_Dotted", "SD", SRC_PROPLIST},
    {"Terminal_Punctuation", "Term", SRC_PROPLIST},
    {"Unified_Ideograph", "UIdeo", SRC_PROPLIST},
    {"Uppercase", "Upper", SRC_DCP},
    {"Variation_Selector", "VS", SRC_PROPLIST},
    {"White_Space", "space", SRC_PROPLIST},
    {"XID_Continue", "XIDC", SRC_DCP},
    {"XID_Start", "XIDS", SRC_DCP},
    {"ASCII", "ASCII", SRC_ASCII},
    {"Any", "Any", SRC_ANY},
    {"Assigned", "Assigned", SRC_ASSIGNED},
};
#define BIN_COUNT (sizeof bin_props / sizeof bin_props[0])

typedef struct {
    uint32_t lo;
    uint32_t hi;
} range_t;

typedef struct {
    range_t *items;
    size_t count;
    size_t cap;
} rangeset;

/* One name spelling and the sets it stands for. */
typedef struct {
    char name[64];
    uint8_t ns;
    uint8_t n;
    uint8_t kind[MAX_MEMBERS];
    uint16_t idx[MAX_MEMBERS];
} name_entry;

typedef struct {
    uint8_t *items;
    size_t count;
    size_t cap;
} bytevec;

static uint8_t gc_of[CP_MAX];
static uint8_t sc_of[CP_MAX];
static uint32_t upper_of[CP_MAX]; /* 0: no mapping; UPPER_MULTI: full mapping is not one code point */
#define UPPER_MULTI 0xFFFFFFFFu

static rangeset gc_sets[GC_COUNT];
static rangeset sc_sets[MAX_SCRIPTS];
static rangeset scx_sets[MAX_SCRIPTS];
static rangeset bin_sets[BIN_COUNT];
static rangeset listed_any;
static rangeset scx_listed[MAX_SCRIPTS];

static name_entry names[MAX_NAMES];
static size_t name_count;
static size_t nscripts;
static char script_names[MAX_SCRIPTS * 4][64];
static uint16_t script_name_idx[MAX_SCRIPTS * 4];
static size_t script_name_count;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "propgen: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static void rs_add(rangeset *rs, uint32_t lo, uint32_t hi)
{
    if (lo > hi || hi >= CP_MAX) {
        die("bad range", nullptr);
    }
    if (rs->count > 0) {
        range_t *last = &rs->items[rs->count - 1];
        if (lo >= last->lo && lo <= last->hi + 1) {
            if (hi > last->hi) {
                last->hi = hi;
            }
            return;
        }
    }
    if (rs->count == rs->cap) {
        size_t cap = rs->cap ? rs->cap * 2 : 256;
        range_t *items = realloc(rs->items, cap * sizeof *items);
        if (!items) {
            die("out of memory", nullptr);
        }
        rs->items = items;
        rs->cap = cap;
    }
    rs->items[rs->count].lo = lo;
    rs->items[rs->count].hi = hi;
    rs->count++;
}

static int range_cmp(const void *a, const void *b)
{
    const range_t *x = a;
    const range_t *y = b;
    if (x->lo != y->lo) {
        return x->lo < y->lo ? -1 : 1;
    }
    return 0;
}

/* Sorts and merges adjacent or overlapping ranges. */
static void rs_finalize(rangeset *rs)
{
    if (rs->count == 0) {
        return;
    }
    qsort(rs->items, rs->count, sizeof *rs->items, range_cmp);
    size_t k = 0;
    for (size_t i = 1; i < rs->count; i++) {
        if (rs->items[i].lo <= rs->items[k].hi + 1) {
            if (rs->items[i].hi > rs->items[k].hi) {
                rs->items[k].hi = rs->items[i].hi;
            }
        } else {
            rs->items[++k] = rs->items[i];
        }
    }
    rs->count = k + 1;
}

static void rs_complement(const rangeset *src, rangeset *out)
{
    uint32_t next = 0;
    for (size_t i = 0; i < src->count; i++) {
        if (src->items[i].lo > next) {
            rs_add(out, next, src->items[i].lo - 1);
        }
        next = src->items[i].hi + 1;
    }
    if (next < CP_MAX) {
        rs_add(out, next, CP_MAX - 1);
    }
}

static void bv_byte(bytevec *b, uint8_t v)
{
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        uint8_t *items = realloc(b->items, cap);
        if (!items) {
            die("out of memory", nullptr);
        }
        b->items = items;
        b->cap = cap;
    }
    b->items[b->count++] = v;
}

static void put_varint(bytevec *b, uint32_t v)
{
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) {
            byte |= 0x80;
        }
        bv_byte(b, byte);
    } while (v);
}

static FILE *open_ucd(const char *dir, const char *name)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        die("ucd-dir path too long", name);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        die("cannot open", path);
    }
    return f;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

/* Splits s in place at sep; at most max fields. Returns the field count. */
static size_t split(char *s, char sep, char **out, size_t max)
{
    size_t n = 0;
    char *p = s;
    while (n < max) {
        out[n++] = p;
        char *q = strchr(p, sep);
        if (!q) {
            break;
        }
        *q = '\0';
        p = q + 1;
    }
    return n;
}

static void parse_range(const char *s, uint32_t *lo, uint32_t *hi)
{
    const char *dots = strstr(s, "..");
    char *end = nullptr;
    *lo = (uint32_t)strtoul(s, &end, 16);
    *hi = dots ? (uint32_t)strtoul(dots + 2, nullptr, 16) : *lo;
    if (*lo > *hi || *hi >= CP_MAX) {
        die("bad code point range", s);
    }
}

/* Reads the next data line (comments and blank lines skipped). Returns false at end of file. */
static bool next_data_line(FILE *f, char *buf, size_t cap, char **out)
{
    while (fgets(buf, (int)cap, f)) {
        char *h = strchr(buf, '#');
        if (h) {
            *h = '\0';
        }
        char *s = trim(buf);
        if (*s) {
            *out = s;
            return true;
        }
    }
    return false;
}

static int script_index_of(const char *name)
{
    for (size_t i = 0; i < script_name_count; i++) {
        if (strcmp(script_names[i], name) == 0) {
            return script_name_idx[i];
        }
    }
    return -1;
}

static int gc_leaf_index(const char *name)
{
    for (int i = 0; i < GC_COUNT; i++) {
        if (strcmp(gc_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static const gc_group *gc_group_of(const char *name)
{
    for (size_t i = 0; i < sizeof gc_groups / sizeof gc_groups[0]; i++) {
        if (strcmp(gc_groups[i].short_name, name) == 0) {
            return &gc_groups[i];
        }
    }
    return nullptr;
}

static name_entry *add_name(const char *name, uint8_t ns)
{
    if (name_count == MAX_NAMES) {
        die("too many names", nullptr);
    }
    for (size_t i = 0; i < name_count; i++) {
        if (names[i].ns == ns && strcmp(names[i].name, name) == 0) {
            return &names[i];
        }
    }
    name_entry *e = &names[name_count++];
    memset(e, 0, sizeof *e);
    if (strlen(name) >= sizeof e->name) {
        die("name too long", name);
    }
    strcpy(e->name, name);
    e->ns = ns;
    return e;
}

static void name_add_member(name_entry *e, int kind, uint32_t idx)
{
    if (e->n == MAX_MEMBERS) {
        die("too many members for a name", e->name);
    }
    e->kind[e->n] = (uint8_t)kind;
    e->idx[e->n] = (uint16_t)idx;
    e->n++;
}

/* PropertyValueAliases.txt: gc and sc value names and their aliases. */
static void load_value_aliases(const char *dir)
{
    FILE *f = open_ucd(dir, "PropertyValueAliases.txt");
    char buf[4096];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *tok[8];
        size_t n = split(line, ';', tok, 8);
        for (size_t i = 0; i < n; i++) {
            tok[i] = trim(tok[i]);
        }
        if (n < 3) {
            continue;
        }
        if (strcmp(tok[0], "gc") == 0) {
            const gc_group *grp = gc_group_of(tok[1]);
            int leaf = gc_leaf_index(tok[1]);
            if (!grp && leaf < 0) {
                die("unknown gc value", tok[1]);
            }
            for (size_t i = 1; i < n; i++) {
                if (tok[i][0] == '\0') {
                    continue;
                }
                name_entry *e = add_name(tok[i], NS_LONE);
                name_entry *g = add_name(tok[i], NS_GC);
                if (grp) {
                    for (uint8_t m = 0; m < grp->n; m++) {
                        name_add_member(e, K_GC, grp->members[m]);
                        name_add_member(g, K_GC, grp->members[m]);
                    }
                } else {
                    name_add_member(e, K_GC, (uint32_t)leaf);
                    name_add_member(g, K_GC, (uint32_t)leaf);
                }
            }
        } else if (strcmp(tok[0], "sc") == 0) {
            if (nscripts == MAX_SCRIPTS) {
                die("too many scripts", nullptr);
            }
            uint32_t s = (uint32_t)nscripts++;
            for (size_t i = 1; i < n; i++) {
                if (tok[i][0] == '\0') {
                    continue;
                }
                if (script_name_count >= sizeof script_names / sizeof script_names[0]) {
                    die("too many script names", nullptr);
                }
                if (strlen(tok[i]) >= sizeof script_names[0]) {
                    die("script name too long", tok[i]);
                }
                strcpy(script_names[script_name_count], tok[i]);
                script_name_idx[script_name_count] = (uint16_t)s;
                script_name_count++;
                name_entry *sc = add_name(tok[i], NS_SC);
                name_add_member(sc, K_SC, s);
                name_entry *scx = add_name(tok[i], NS_SCX);
                name_add_member(scx, K_SCX, s);
            }
        }
    }
    fclose(f);
    if (nscripts == 0) {
        die("no script values", nullptr);
    }
}

/* UnicodeData.txt: Bidi_Mirrored, and simple and full uppercase for Canonicalize. */
static void load_unicode_data(const char *dir)
{
    FILE *f = open_ucd(dir, "UnicodeData.txt");
    char buf[1024];
    uint32_t range_start = 0;
    while (fgets(buf, sizeof buf, f)) {
        char *fld[15];
        size_t n = split(buf, ';', fld, 15);
        if (n < 14) {
            die("short UnicodeData line", nullptr);
        }
        uint32_t cp = (uint32_t)strtoul(fld[0], nullptr, 16);
        if (cp >= CP_MAX) {
            die("bad UnicodeData code point", fld[0]);
        }
        bool first = strstr(fld[1], "First>") != nullptr;
        bool last = strstr(fld[1], "Last>") != nullptr;
        uint32_t lo = last ? range_start : cp;
        if (fld[9][0] == 'Y') {
            rs_add(&bin_sets[3], lo, cp); /* Bidi_Mirrored is bin index 3 */
        }
        if (first) {
            range_start = cp;
            continue;
        }
        if (fld[12][0] != '\0') {
            upper_of[cp] = (uint32_t)strtoul(fld[12], nullptr, 16);
        }
    }
    fclose(f);
}

/* SpecialCasing.txt: unconditional full uppercase mappings override the simple ones. */
static void load_special_casing(const char *dir)
{
    FILE *f = open_ucd(dir, "SpecialCasing.txt");
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[6];
        size_t n = split(line, ';', fld, 6);
        if (n < 4) {
            die("short SpecialCasing line", line);
        }
        if (n >= 5 && trim(fld[4])[0] != '\0') {
            continue; /* conditional mapping */
        }
        uint32_t cp = (uint32_t)strtoul(trim(fld[0]), nullptr, 16);
        char *up = trim(fld[3]);
        size_t cps = 0;
        char *save = nullptr;
        char *tok = strtok_r(up, " ", &save);
        uint32_t first_cp = 0;
        while (tok) {
            if (cps == 0) {
                first_cp = (uint32_t)strtoul(tok, nullptr, 16);
            }
            cps++;
            tok = strtok_r(nullptr, " ", &save);
        }
        if (cps == 1) {
            upper_of[cp] = first_cp;
        } else if (cps > 1) {
            upper_of[cp] = UPPER_MULTI;
        }
    }
    fclose(f);
}

static void load_gc(const char *dir)
{
    for (uint32_t i = 0; i < CP_MAX; i++) {
        gc_of[i] = GC_CN;
    }
    FILE *f = open_ucd(dir, "DerivedGeneralCategory.txt");
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[2];
        if (split(line, ';', fld, 2) != 2) {
            die("bad DerivedGeneralCategory line", line);
        }
        uint32_t lo, hi;
        parse_range(trim(fld[0]), &lo, &hi);
        int leaf = gc_leaf_index(trim(fld[1]));
        if (leaf < 0) {
            die("unknown gc in DerivedGeneralCategory", fld[1]);
        }
        for (uint32_t cp = lo; cp <= hi; cp++) {
            gc_of[cp] = (uint8_t)leaf;
        }
    }
    fclose(f);
}

static void load_scripts(const char *dir)
{
    for (uint32_t i = 0; i < CP_MAX; i++) {
        sc_of[i] = 0xFF;
    }
    FILE *f = open_ucd(dir, "Scripts.txt");
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[2];
        if (split(line, ';', fld, 2) != 2) {
            die("bad Scripts line", line);
        }
        uint32_t lo, hi;
        parse_range(trim(fld[0]), &lo, &hi);
        int s = script_index_of(trim(fld[1]));
        if (s < 0) {
            die("unknown script", fld[1]);
        }
        for (uint32_t cp = lo; cp <= hi; cp++) {
            sc_of[cp] = (uint8_t)s;
        }
    }
    fclose(f);
    /* Unlisted code points have the Unknown script (Zzzz). */
    int unknown = script_index_of("Zzzz");
    if (unknown < 0) {
        die("no Zzzz script", nullptr);
    }
    for (uint32_t cp = 0; cp < CP_MAX; cp++) {
        if (sc_of[cp] == 0xFF) {
            sc_of[cp] = (uint8_t)unknown;
        }
    }
}

/* ScriptExtensions.txt: listed code points get their extension set; the rest use Script. */
static void load_script_extensions(const char *dir)
{
    FILE *f = open_ucd(dir, "ScriptExtensions.txt");
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[2];
        if (split(line, ';', fld, 2) != 2) {
            die("bad ScriptExtensions line", line);
        }
        uint32_t lo, hi;
        parse_range(trim(fld[0]), &lo, &hi);
        rs_add(&listed_any, lo, hi);
        char *save = nullptr;
        char *tok = strtok_r(trim(fld[1]), " ", &save);
        while (tok) {
            int s = script_index_of(tok);
            if (s < 0) {
                die("unknown script in ScriptExtensions", tok);
            }
            rs_add(&scx_listed[s], lo, hi);
            tok = strtok_r(nullptr, " ", &save);
        }
    }
    fclose(f);
    rs_finalize(&listed_any);
}

/* Binary properties from the four files that define them, matched by long name. */
static void load_binary_file(const char *dir, const char *file, int src)
{
    FILE *f = open_ucd(dir, file);
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[2];
        if (split(line, ';', fld, 2) != 2) {
            continue;
        }
        char *name = trim(fld[1]);
        for (size_t b = 0; b < BIN_COUNT; b++) {
            if (bin_props[b].src == src && strcmp(bin_props[b].lname, name) == 0) {
                uint32_t lo, hi;
                parse_range(trim(fld[0]), &lo, &hi);
                rs_add(&bin_sets[b], lo, hi);
            }
        }
    }
    fclose(f);
}

static void load_casefolding(const char *dir, uint32_t (*pairs)[2], size_t *count, size_t cap)
{
    FILE *f = open_ucd(dir, "CaseFolding.txt");
    char buf[1024];
    char *line = nullptr;
    while (next_data_line(f, buf, sizeof buf, &line)) {
        char *fld[4];
        if (split(line, ';', fld, 4) < 3) {
            die("bad CaseFolding line", line);
        }
        char *status = trim(fld[1]);
        if (strcmp(status, "C") != 0 && strcmp(status, "S") != 0) {
            continue;
        }
        if (*count >= cap) {
            die("too many case folding entries", nullptr);
        }
        pairs[*count][0] = (uint32_t)strtoul(trim(fld[0]), nullptr, 16);
        pairs[*count][1] = (uint32_t)strtoul(trim(fld[2]), nullptr, 16);
        (*count)++;
    }
    fclose(f);
}

static int pair_cmp(const void *a, const void *b)
{
    const uint32_t *x = a;
    const uint32_t *y = b;
    if (x[0] != y[0]) {
        return x[0] < y[0] ? -1 : 1;
    }
    return 0;
}

static uint32_t ref_id(int kind, uint32_t idx)
{
    switch (kind) {
    case K_GC:
        return idx;
    case K_SC:
        return GC_COUNT + idx;
    case K_SCX:
        return (uint32_t)(GC_COUNT + nscripts + idx);
    default:
        return (uint32_t)(GC_COUNT + 2 * nscripts + idx);
    }
}

static void emit_bytes(FILE *out, const char *name, const bytevec *b)
{
    fprintf(out, "static const uint8_t %s[%zu] = {\n", name, b->count ? b->count : 1);
    for (size_t i = 0; i < b->count; i++) {
        fprintf(out, "%s0x%02X,", (i % 16 == 0) ? "    " : " ", b->items[i]);
        if (i % 16 == 15 || i + 1 == b->count) {
            fprintf(out, "\n");
        }
    }
    if (b->count == 0) {
        fprintf(out, "    0,\n");
    }
    fprintf(out, "};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: propgen <ucd-dir> <output.inc>\n");
        return 2;
    }
    const char *dir = argv[1];

    load_value_aliases(dir);
    load_unicode_data(dir);
    load_special_casing(dir);
    load_gc(dir);
    load_scripts(dir);
    load_script_extensions(dir);

    /* Binary property sources. */
    load_binary_file(dir, "PropList.txt", SRC_PROPLIST);
    load_binary_file(dir, "DerivedCoreProperties.txt", SRC_DCP);
    load_binary_file(dir, "emoji-data.txt", SRC_EMOJI);
    load_binary_file(dir, "DerivedNormalizationProps.txt", SRC_NORM);

    /* Per-code-point sets. Ranges are added in ascending order, so adjacent runs merge. */
    for (uint32_t cp = 0; cp < CP_MAX; cp++) {
        rs_add(&gc_sets[gc_of[cp]], cp, cp);
        rs_add(&sc_sets[sc_of[cp]], cp, cp);
    }
    for (int g = 0; g < GC_COUNT; g++) {
        rs_finalize(&gc_sets[g]);
    }

    /* Computed binary sets (Assigned needs the finished Cn set). */
    for (size_t b = 0; b < BIN_COUNT; b++) {
        if (bin_props[b].src == SRC_ASCII) {
            rs_add(&bin_sets[b], 0, 0x7F);
        } else if (bin_props[b].src == SRC_ANY) {
            rs_add(&bin_sets[b], 0, CP_MAX - 1);
        } else if (bin_props[b].src == SRC_ASSIGNED) {
            rs_complement(&gc_sets[GC_CN], &bin_sets[b]);
        }
    }
    /* Script_Extensions: a listed code point has its listed scripts; an unlisted one has Script. */
    {
        size_t j = 0;
        for (uint32_t cp = 0; cp < CP_MAX; cp++) {
            while (j < listed_any.count && listed_any.items[j].hi < cp) {
                j++;
            }
            bool listed = j < listed_any.count && listed_any.items[j].lo <= cp;
            if (!listed) {
                rs_add(&scx_sets[sc_of[cp]], cp, cp);
            }
        }
    }
    for (size_t s = 0; s < nscripts; s++) {
        for (size_t i = 0; i < scx_listed[s].count; i++) {
            rs_add(&scx_sets[s], scx_listed[s].items[i].lo, scx_listed[s].items[i].hi);
        }
    }
    for (size_t s = 0; s < nscripts; s++) {
        rs_finalize(&sc_sets[s]);
        rs_finalize(&scx_sets[s]);
    }
    for (size_t b = 0; b < BIN_COUNT; b++) {
        rs_finalize(&bin_sets[b]);
        if (bin_sets[b].count == 0) {
            fprintf(stderr, "propgen: warning: empty binary property %s\n", bin_props[b].lname);
        }
    }

    /* Name table: binary properties (lone names only). */
    for (size_t b = 0; b < BIN_COUNT; b++) {
        name_entry *e = add_name(bin_props[b].lname, NS_LONE);
        name_add_member(e, K_BIN, (uint32_t)b);
        if (strcmp(bin_props[b].sname, bin_props[b].lname) != 0) {
            e = add_name(bin_props[b].sname, NS_LONE);
            name_add_member(e, K_BIN, (uint32_t)b);
        }
    }

    /* Set table: gc, sc, scx, then binary. Offsets index the encoded data. */
    size_t nsets = GC_COUNT + 2 * nscripts + BIN_COUNT;
    bytevec data = {nullptr, 0, 0};
    uint32_t *off = calloc(nsets + 1, sizeof *off);
    if (!off) {
        die("out of memory", nullptr);
    }
    size_t id = 0;
    for (int g = 0; g < GC_COUNT; g++, id++) {
        off[id] = (uint32_t)data.count;
        const rangeset *rs = &gc_sets[g];
        put_varint(&data, (uint32_t)rs->count);
        uint32_t prev = 0;
        for (size_t i = 0; i < rs->count; i++) {
            put_varint(&data, rs->items[i].lo - prev);
            put_varint(&data, rs->items[i].hi - rs->items[i].lo);
            prev = rs->items[i].hi + 1;
        }
    }
    for (size_t pass = 0; pass < 2; pass++) {
        rangeset *base = pass == 0 ? sc_sets : scx_sets;
        for (size_t s = 0; s < nscripts; s++, id++) {
            off[id] = (uint32_t)data.count;
            const rangeset *rs = &base[s];
            put_varint(&data, (uint32_t)rs->count);
            uint32_t prev = 0;
            for (size_t i = 0; i < rs->count; i++) {
                put_varint(&data, rs->items[i].lo - prev);
                put_varint(&data, rs->items[i].hi - rs->items[i].lo);
                prev = rs->items[i].hi + 1;
            }
        }
    }
    for (size_t b = 0; b < BIN_COUNT; b++, id++) {
        off[id] = (uint32_t)data.count;
        const rangeset *rs = &bin_sets[b];
        put_varint(&data, (uint32_t)rs->count);
        uint32_t prev = 0;
        for (size_t i = 0; i < rs->count; i++) {
            put_varint(&data, rs->items[i].lo - prev);
            put_varint(&data, rs->items[i].hi - rs->items[i].lo);
            prev = rs->items[i].hi + 1;
        }
    }
    off[nsets] = (uint32_t)data.count;

    /* Case folding for u and v (simple, C and S statuses). */
    size_t fold_cap = 4096;
    uint32_t (*fold)[2] = malloc(fold_cap * sizeof *fold);
    if (!fold) {
        die("out of memory", nullptr);
    }
    size_t fold_n = 0;
    load_casefolding(dir, fold, &fold_n, fold_cap);
    qsort(fold, fold_n, sizeof *fold, pair_cmp);

    /* Canonicalize for non-u patterns. */
    size_t canon_cap = 4096;
    uint32_t (*canon)[2] = malloc(canon_cap * sizeof *canon);
    if (!canon) {
        die("out of memory", nullptr);
    }
    size_t canon_n = 0;
    for (uint32_t cp = 0; cp < CP_MAX; cp++) {
        uint32_t up = upper_of[cp];
        if (up == 0 || up == cp || up == UPPER_MULTI) {
            continue;
        }
        if (up > 0xFFFF) {
            continue; /* not a single code unit */
        }
        if (cp >= 0x80 && up < 0x80) {
            continue;
        }
        if (canon_n == canon_cap) {
            canon_cap *= 2;
            canon = realloc(canon, canon_cap * sizeof *canon);
            if (!canon) {
                die("out of memory", nullptr);
            }
        }
        canon[canon_n][0] = cp;
        canon[canon_n][1] = up;
        canon_n++;
    }

    FILE *out = fopen(argv[2], "w");
    if (!out) {
        die("cannot write", argv[2]);
    }
    fprintf(out,
            "/* Generated by tools/unicodegen/propgen.c from UCD 17.0.0 (UnicodeData,\n"
            " * DerivedGeneralCategory, Scripts, ScriptExtensions, PropList, DerivedCoreProperties,\n"
            " * emoji-data, DerivedNormalizationProps, CaseFolding, SpecialCasing). Do not edit.\n"
            " * Regenerate with:\n"
            " *     cmake --build <dir> --target unicodeprops */\n\n");
    fprintf(out, "#define EMBI_RE_NS_LONE %d\n#define EMBI_RE_NS_GC %d\n", NS_LONE, NS_GC);
    fprintf(out, "#define EMBI_RE_NS_SC %d\n#define EMBI_RE_NS_SCX %d\n", NS_SC, NS_SCX);
    fprintf(out, "#define EMBI_RE_NSETS %zu\n", nsets);
    fprintf(out, "#define EMBI_RE_SET_ZS %u\n", ref_id(K_GC, 22));
    fprintf(out, "#define EMBI_RE_SET_CN %u\n\n", ref_id(K_GC, GC_CN));

    fprintf(out, "/* Set i is embi_re_set_data[embi_re_set_off[i] .. embi_re_set_off[i + 1]): a varint\n"
                 " * range count, then per range a varint gap (lo minus the previous hi + 1) and a varint\n"
                 " * (hi - lo). Set ids: General_Category 0..29, then Script (per script), Script_Extensions\n"
                 " * (per script), then the binary properties. */\n");
    fprintf(out, "static const uint32_t embi_re_set_off[EMBI_RE_NSETS + 1] = {\n");
    for (size_t i = 0; i <= nsets; i++) {
        fprintf(out, "%s%u,", (i % 12 == 0) ? "    " : " ", off[i]);
        if (i % 12 == 11 || i == nsets) {
            fprintf(out, "\n");
        }
    }
    fprintf(out, "};\n");
    emit_bytes(out, "embi_re_set_data", &data);
    fprintf(out, "\n");

    /* Name table with member lists. */
    size_t member_total = 0;
    for (size_t i = 0; i < name_count; i++) {
        member_total += names[i].n;
    }
    fprintf(out, "#define EMBI_RE_NPNAMES %zu\n", name_count);
    fprintf(out, "static const embi_re_pname embi_re_pnames[EMBI_RE_NPNAMES] = {\n");
    size_t first = 0;
    for (size_t i = 0; i < name_count; i++) {
        fprintf(out, "    {\"%s\", %u, %u, %zu},\n", names[i].name, names[i].ns, names[i].n, first);
        first += names[i].n;
    }
    fprintf(out, "};\n");
    fprintf(out, "#define EMBI_RE_NPMEMBERS %zu\n", member_total);
    fprintf(out, "static const uint16_t embi_re_pmembers[EMBI_RE_NPMEMBERS] = {\n");
    size_t written = 0;
    for (size_t i = 0; i < name_count; i++) {
        for (uint8_t m = 0; m < names[i].n; m++) {
            fprintf(out, "%s%u,", (written % 16 == 0) ? "    " : " ",
                    ref_id(names[i].kind[m], names[i].idx[m]));
            written++;
            if (written % 16 == 0) {
                fprintf(out, "\n");
            }
        }
    }
    if (written % 16 != 0) {
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");

    fprintf(out, "#define EMBI_RE_NFOLD_U %zu\n", fold_n);
    fprintf(out, "static const embi_re_casemap embi_re_fold_u[EMBI_RE_NFOLD_U] = {\n");
    for (size_t i = 0; i < fold_n; i++) {
        fprintf(out, "    {0x%04X, 0x%04X},\n", fold[i][0], fold[i][1]);
    }
    fprintf(out, "};\n\n");
    fprintf(out, "#define EMBI_RE_NCANON_NU %zu\n", canon_n);
    fprintf(out, "static const embi_re_casemap embi_re_canon_nu[EMBI_RE_NCANON_NU] = {\n");
    for (size_t i = 0; i < canon_n; i++) {
        fprintf(out, "    {0x%04X, 0x%04X},\n", canon[i][0], canon[i][1]);
    }
    fprintf(out, "};\n");
    fclose(out);

    size_t total = data.count + (size_t)member_total * 2 + fold_n * 8 + canon_n * 8 + name_count * 16;
    fprintf(stderr,
            "propgen: %zu sets (%zu scripts), set data %zu bytes, names %zu (%zu members), "
            "case folds %zu, canonicalize %zu, about %zu bytes of tables\n",
            nsets, nscripts, data.count, name_count, member_total, fold_n, canon_n, total);
    return 0;
}
