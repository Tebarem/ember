/*
 * Runs the WPT / html5lib tree-construction tests (tests/wpt/html/syntax/parsing/resources, *.dat files)
 * through the parser and compares each tree dump with the expected #document section.
 *
 * Format: docs in tests/wpt/html/syntax/parsing/resources/README.md. A test with #script-on or
 * #script-off runs in that scripting mode only. A test with neither runs in both modes. Fragment
 * tests (#document-fragment) use embi_html_parse_fragment. #errors and #new-errors are ignored.
 *
 * Every run is keyed "<file>#<index>/<on|off>", where index is the 1-based test number in the file.
 * Runs listed in tests/html/expectations.txt may fail. Any other failure fails the test, and so
 * does an overall pass rate below 99%. The run prints the pass count of each file.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <stdlib.h>
#include <string.h>

#define DAT_MAX_FILES 512
#define DAT_MAX_EXPECT 4096

/* ------------------------------------------------------------------ line table */

typedef struct dat_lines {
    const char *base;
    size_t *start; /* start offset of each line */
    size_t *end;   /* end offset (exclusive, without the newline) */
    size_t count;
} dat_lines;

static void lines_split(dat_lines *L, const char *s, size_t n)
{
    size_t cap = 64;
    L->base = s;
    L->start = SDL_malloc(cap * sizeof *L->start);
    L->end = SDL_malloc(cap * sizeof *L->end);
    L->count = 0;
    size_t pos = 0;
    while (pos < n) {
        size_t e = pos;
        while (e < n && s[e] != '\n') {
            e++;
        }
        if (L->count == cap) {
            cap *= 2;
            L->start = SDL_realloc(L->start, cap * sizeof *L->start);
            L->end = SDL_realloc(L->end, cap * sizeof *L->end);
        }
        L->start[L->count] = pos;
        L->end[L->count] = e;
        L->count++;
        pos = e + 1;
    }
}

static void lines_free(dat_lines *L)
{
    SDL_free(L->start);
    SDL_free(L->end);
    L->start = nullptr;
    L->end = nullptr;
    L->count = 0;
}

static bool line_is(const dat_lines *L, size_t i, const char *lit)
{
    size_t n = strlen(lit);
    return L->end[i] - L->start[i] == n && memcmp(L->base + L->start[i], lit, n) == 0;
}

static bool line_is_directive(const dat_lines *L, size_t i)
{
    return line_is(L, i, "#errors") || line_is(L, i, "#new-errors") ||
           line_is(L, i, "#document-fragment") || line_is(L, i, "#script-on") ||
           line_is(L, i, "#script-off") || line_is(L, i, "#document");
}

/* ------------------------------------------------------------------ parsed test records */

typedef struct dat_test {
    unsigned index;      /* 1-based within the file */
    const char *data;    /* input bytes (points into the file buffer) */
    size_t data_len;
    bool has_context;
    char context[128];   /* "name", "svg name" or "math name" */
    int script;          /* -1 both, 0 off, 1 on */
    const char *doc;     /* expected dump bytes */
    size_t doc_len;
} dat_test;

/* Splits a file into tests. Tests begin at a "#description" or "#data" line that follows a blank
 * line (or starts the file). Returns the number of tests written to out (at most max). */
static size_t parse_dat(const char *buf, size_t n, dat_test *out, size_t max)
{
    dat_lines L;
    lines_split(&L, buf, n);
    size_t ntests = 0;
    size_t i = 0;
    while (i < L.count) {
        bool starts = (line_is(&L, i, "#data") || line_is(&L, i, "#description")) &&
                      (i == 0 || L.end[i - 1] == L.start[i - 1]);
        if (!starts) {
            i++;
            continue;
        }
        size_t block_begin = i;
        size_t j = i + 1;
        while (j < L.count) {
            bool next = (line_is(&L, j, "#data") || line_is(&L, j, "#description")) &&
                        L.end[j - 1] == L.start[j - 1];
            if (next) {
                break;
            }
            j++;
        }
        /* Block is [block_begin, j). Find the directives. */
        size_t data_line = (size_t)-1;
        size_t doc_line = (size_t)-1;
        dat_test t = {0};
        t.script = -1;
        for (size_t k = block_begin; k < j; k++) {
            if (line_is(&L, k, "#data") && data_line == (size_t)-1) {
                data_line = k;
            } else if (line_is(&L, k, "#document-fragment") && k + 1 < j) {
                t.has_context = true;
                size_t cl = L.end[k + 1] - L.start[k + 1];
                if (cl >= sizeof t.context) {
                    cl = sizeof t.context - 1;
                }
                SDL_memcpy(t.context, L.base + L.start[k + 1], cl);
                t.context[cl] = '\0';
            } else if (line_is(&L, k, "#script-on")) {
                t.script = 1;
            } else if (line_is(&L, k, "#script-off")) {
                t.script = 0;
            } else if (line_is(&L, k, "#document")) {
                doc_line = k;
                break;
            }
        }
        if (data_line != (size_t)-1) {
            size_t d = data_line + 1;
            size_t de = d;
            while (de < j && !line_is_directive(&L, de)) {
                de++;
            }
            /* The data is the lines d..de-1 joined with newlines (the last newline dropped). */
            if (de > d) {
                t.data = L.base + L.start[d];
                t.data_len = L.end[de - 1] - L.start[d];
            } else {
                t.data = "";
                t.data_len = 0;
            }
        } else {
            t.data = "";
            t.data_len = 0;
        }
        if (doc_line != (size_t)-1) {
            size_t d = doc_line + 1;
            size_t last = j; /* exclusive: trailing empty lines are dropped */
            while (last > d && L.end[last - 1] == L.start[last - 1]) {
                last--;
            }
            if (last > d) {
                t.doc = L.base + L.start[d];
                t.doc_len = L.end[last - 1] - L.start[d];
            } else {
                t.doc = "";
                t.doc_len = 0;
            }
        } else {
            t.doc = "";
            t.doc_len = 0;
        }
        t.index = (unsigned)(ntests + 1);
        if (ntests < max) {
            out[ntests] = t;
        }
        ntests++;
        i = j;
    }
    lines_free(&L);
    return ntests;
}

/* ------------------------------------------------------------------ tree dump (README format) */

static void dat_line(embi_sb *o, int depth)
{
    if (o->len) {
        embi_sb_append_char(o, '\n');
    }
    embi_sb_append(o, EMBI_STR("| "));
    for (int i = 0; i < depth; i++) {
        embi_sb_append(o, EMBI_STR("  "));
    }
}

typedef struct attr_key {
    embi_sb name;
    uint32_t idx;
} attr_key;

static void attr_name(embi_sb *o, const embi_attr *a)
{
    switch (a->ns) {
    case EMBI_NS_XLINK:
        embi_sb_append(o, EMBI_STR("xlink "));
        break;
    case EMBI_NS_XML:
        embi_sb_append(o, EMBI_STR("xml "));
        break;
    case EMBI_NS_XMLNS:
        embi_sb_append(o, EMBI_STR("xmlns "));
        break;
    default:
        break;
    }
    embi_sb_append(o, embi_atom_str(a->name));
}

static int attr_key_cmp(const void *pa, const void *pb)
{
    const attr_key *x = pa;
    const attr_key *y = pb;
    uint32_t n = x->name.len < y->name.len ? x->name.len : y->name.len;
    if (n > 0) {
        int c = memcmp(x->name.data, y->name.data, n);
        if (c != 0) {
            return c;
        }
    }
    return (int)x->name.len - (int)y->name.len;
}

static void dump_node(embi_sb *o, const embi_node *n, int depth);

static void dump_children(embi_sb *o, const embi_node *parent, int depth)
{
    for (const embi_node *c = parent->first_child; c; c = c->next) {
        dump_node(o, c, depth);
    }
}

static void dump_node(embi_sb *o, const embi_node *n, int depth)
{
    switch (n->type) {
    case EMBI_NODE_ELEMENT: {
        const embi_element *el = (const embi_element *)n;
        dat_line(o, depth);
        embi_sb_append_char(o, '<');
        if (n->extra == EMBI_NS_SVG) {
            embi_sb_append(o, EMBI_STR("svg "));
        } else if (n->extra == EMBI_NS_MATHML) {
            embi_sb_append(o, EMBI_STR("math "));
        }
        embi_sb_append(o, embi_atom_str(n->name));
        embi_sb_append_char(o, '>');

        uint32_t cnt = embi_el_attr_count(el);
        uint32_t cap = cnt ? cnt : 1;
        attr_key *keys = SDL_calloc(cap, sizeof *keys);
        for (uint32_t i = 0; i < cnt; i++) {
            attr_name(&keys[i].name, embi_el_attr_at(el, i));
            keys[i].idx = i;
        }
        if (cnt > 1) {
            SDL_qsort(keys, cnt, sizeof *keys, attr_key_cmp);
        }
        for (uint32_t i = 0; i < cnt; i++) {
            const embi_attr *a = embi_el_attr_at(el, keys[i].idx);
            dat_line(o, depth + 1);
            embi_sb_append(o, (embi_str){keys[i].name.data, keys[i].name.len});
            embi_sb_append(o, EMBI_STR("=\""));
            embi_dstr_append_utf8(o, a->value);
            embi_sb_append_char(o, '"');
        }
        for (uint32_t i = 0; i < cnt; i++) {
            embi_sb_free(&keys[i].name);
        }
        SDL_free(keys);

        if (n->extra == EMBI_NS_HTML && n->name == EMBI_ATOM_template) {
            dat_line(o, depth + 1);
            embi_sb_append(o, EMBI_STR("content"));
            if (el->attrs && el->attrs->content) {
                dump_children(o, el->attrs->content, depth + 2);
            }
        }
        dump_children(o, n, depth + 1);
        return;
    }
    case EMBI_NODE_TEXT: {
        const embi_chardata *cd = (const embi_chardata *)n;
        dat_line(o, depth);
        embi_sb_append_char(o, '"');
        embi_dstr_append_utf8(o, cd->data);
        embi_sb_append_char(o, '"');
        return;
    }
    case EMBI_NODE_COMMENT: {
        const embi_chardata *cd = (const embi_chardata *)n;
        dat_line(o, depth);
        embi_sb_append(o, EMBI_STR("<!-- "));
        embi_dstr_append_utf8(o, cd->data);
        embi_sb_append(o, EMBI_STR(" -->"));
        return;
    }
    case EMBI_NODE_PI: {
        const embi_chardata *cd = (const embi_chardata *)n;
        dat_line(o, depth);
        embi_sb_append(o, EMBI_STR("<?"));
        embi_sb_append(o, embi_atom_str(n->name));
        embi_sb_append_char(o, ' ');
        embi_dstr_append_utf8(o, cd->data);
        embi_sb_append(o, EMBI_STR("?>"));
        return;
    }
    case EMBI_NODE_DOCTYPE: {
        const embi_doctype *dt = (const embi_doctype *)n;
        dat_line(o, depth);
        embi_sb_append(o, EMBI_STR("<!DOCTYPE "));
        embi_dstr_append_utf8(o, dt->name);
        if (dt->public_id.len || dt->system_id.len) {
            embi_sb_append(o, EMBI_STR(" \""));
            embi_dstr_append_utf8(o, dt->public_id);
            embi_sb_append(o, EMBI_STR("\" \""));
            embi_dstr_append_utf8(o, dt->system_id);
            embi_sb_append_char(o, '"');
        }
        embi_sb_append_char(o, '>');
        return;
    }
    default:
        dump_children(o, n, depth);
        return;
    }
}

/* ------------------------------------------------------------------ running one case */

static void run_case(const dat_test *t, bool scripting, embi_sb *got)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser_config cfg = {0};
    cfg.scripting = scripting;
    embi_str input = {t->data, (uint32_t)t->data_len};
    if (t->has_context) {
        uint8_t ns = EMBI_NS_HTML;
        const char *name = t->context;
        if (SDL_strncmp(name, "svg ", 4) == 0) {
            ns = EMBI_NS_SVG;
            name += 4;
        } else if (SDL_strncmp(name, "math ", 5) == 0) {
            ns = EMBI_NS_MATHML;
            name += 5;
        }
        embi_str local = {name, (uint32_t)SDL_strlen(name)};
        embi_element *ctx = embi_new_element(doc, ns, 0, 0, embi_atom_intern(local.ptr, local.len));
        embi_node *frag = embi_html_parse_fragment(ctx, input, &cfg);
        dump_children(got, frag, 0);
        embi_destroy_detached(frag);
        embi_destroy_detached(&ctx->base);
    } else {
        embi_html_parser *p = embi_html_parser_new(doc, &cfg);
        embi_html_parser_write(p, input.ptr, input.len);
        embi_html_parser_end(p);
        embi_html_parser_destroy(p);
        dump_children(got, &doc->base, 0);
    }
    embi_document_free(doc);
}

/* ------------------------------------------------------------------ directory and expectations */

static char *g_names[DAT_MAX_FILES];
static size_t g_name_count;
static const char *g_dir;

static SDL_EnumerationResult collect_name(void *ud, const char *dir, const char *fname)
{
    (void)ud;
    (void)dir;
    size_t n = SDL_strlen(fname);
    if (g_name_count < DAT_MAX_FILES && n > 4 && SDL_strcmp(fname + n - 4, ".dat") == 0) {
        g_names[g_name_count++] = SDL_strdup(fname);
    }
    return SDL_ENUM_CONTINUE;
}

static int name_cmp(const void *a, const void *b)
{
    return SDL_strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every expectation is one run key "<file>#<index>/<on|off>". A key that is not a well-formed run
 * key, or that never matches a run in this suite, fails the test (stale or mistyped entries). */
static char *g_expect[DAT_MAX_EXPECT];
static bool g_expect_seen[DAT_MAX_EXPECT];
static size_t g_expect_count;
static size_t g_expect_bad;

/* k is NUL-terminated. */
static bool run_key_wellformed(const char *k)
{
    size_t len = SDL_strlen(k);
    bool on = len > 3 && SDL_strcmp(k + len - 3, "/on") == 0;
    bool off = len > 4 && SDL_strcmp(k + len - 4, "/off") == 0;
    if (!on && !off) {
        return false;
    }
    const char *hash = nullptr;
    for (size_t i = 0; i < len; i++) {
        if (k[i] == '#') {
            hash = k + i;
        }
    }
    return hash != nullptr && hash != k && hash[1] >= '0' && hash[1] <= '9';
}

static void load_expectations(void)
{
    size_t n = 0;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/tests/html/expectations.txt", EMBER_SOURCE_DIR);
    char *buf = SDL_LoadFile(path, &n);
    if (!buf) {
        SDL_Log("cannot read %s", path);
        g_expect_bad++;
        return;
    }
    size_t pos = 0;
    while (pos < n) {
        size_t e = pos;
        while (e < n && buf[e] != '\n') {
            e++;
        }
        size_t s = pos;
        while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r')) {
            s++;
        }
        size_t k = s;
        while (k < e && buf[k] != ' ' && buf[k] != '\t' && buf[k] != '\r') {
            k++;
        }
        if (k > s && buf[s] != '#') {
            char *key = SDL_malloc(k - s + 1);
            SDL_memcpy(key, buf + s, k - s);
            key[k - s] = '\0';
            if (g_expect_count == DAT_MAX_EXPECT || !run_key_wellformed(key)) {
                SDL_Log("bad expectation line: %s", key);
                g_expect_bad++;
                SDL_free(key);
            } else {
                g_expect[g_expect_count++] = key;
            }
        }
        pos = e + 1;
    }
    SDL_free(buf);
}

static size_t find_expected(const char *key)
{
    for (size_t i = 0; i < g_expect_count; i++) {
        if (SDL_strcmp(g_expect[i], key) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

/* ------------------------------------------------------------------ the test */

typedef struct run_stats {
    size_t total;         /* every run of every file */
    size_t passed;        /* runs whose tree matched, known or not */
    size_t known;         /* runs listed in expectations.txt (failing ones and stale passing ones) */
    size_t expected_fail; /* listed runs that fail, as recorded */
    size_t unexpected_fail;
    size_t unexpected_pass; /* listed runs that pass: remove them from expectations.txt */
} run_stats;

static bool debug_key(const char *key)
{
    const char *want = SDL_getenv("EMBER_DAT_ONLY");
    return want && SDL_strcmp(want, key) == 0;
}

static void check_one(run_stats *st, const char *file, const dat_test *t, bool scripting,
                      size_t *file_pass, size_t *file_total)
{
    char key[256];
    SDL_snprintf(key, sizeof key, "%s#%u/%s", file, t->index, scripting ? "on" : "off");
    embi_sb got = {nullptr, 0, 0};
    run_case(t, scripting, &got);
    bool same = got.len == t->doc_len && (got.len == 0 || memcmp(got.data, t->doc, got.len) == 0);
    st->total++;
    (*file_total)++;
    size_t ei = find_expected(key);
    if (ei != (size_t)-1) {
        g_expect_seen[ei] = true;
        st->known++;
    }
    bool listed = ei != (size_t)-1;
    if (same) {
        st->passed++;
        (*file_pass)++;
        if (listed) {
            st->unexpected_pass++;
            SDL_Log("FAIL unexpected pass %s (remove it from tests/html/expectations.txt)", key);
        }
    } else if (listed) {
        st->expected_fail++;
    } else {
        st->unexpected_fail++;
        SDL_Log("FAIL %s", key);
    }
    if (listed && SDL_getenv("EMBER_DAT_VERBOSE")) {
        SDL_Log("  known %s: %s", key, same ? "passes" : "fails");
    }
    if (SDL_getenv("EMBER_DAT_LIST")) {
        SDL_Log("RUN %s %s", key, same ? "pass" : "fail");
    }
    if (debug_key(key)) {
        SDL_Log("---- %s expected:\n%.*s", key, (int)t->doc_len, t->doc);
        SDL_Log("---- %s got:\n%.*s", key, (int)got.len, got.data ? got.data : "");
    }
    embi_sb_free(&got);
}

/* Runs that may be listed as known failures. Each one is a vendored case that contradicts the
 * current WHATWG text (the XML processing-instruction cases) or needs a script to run (the
 * scripted files). The cap keeps the list from growing into an excuse. */
#define DAT_MAX_KNOWN 200u
/* The gate: passing runs over all runs that are not listed, in per cent times 100. */
#define DAT_GATE_PCT_X100 9900u

EMB_TEST(tree_dat_conformance)
{
    load_expectations();
    g_dir = EMBER_SOURCE_DIR "/tests/wpt/html/syntax/parsing/resources";
    if (!SDL_EnumerateDirectory(g_dir, collect_name, nullptr)) {
        EMB_SKIP("cannot read tests/wpt/html/syntax/parsing/resources");
        return;
    }
    SDL_qsort(g_names, g_name_count, sizeof g_names[0], name_cmp);

    run_stats st = {0};
    size_t files_run = 0;
    for (size_t f = 0; f < g_name_count; f++) {
        char path[1024];
        SDL_snprintf(path, sizeof path, "%s/%s", g_dir, g_names[f]);
        size_t n = 0;
        char *buf = SDL_LoadFile(path, &n);
        if (!buf) {
            SDL_Log("cannot load %s", path);
            st.unexpected_fail++;
            continue;
        }
        size_t cap = 2048;
        dat_test *tests = SDL_malloc(cap * sizeof *tests);
        size_t count = parse_dat(buf, n, tests, cap);
        if (count > cap) {
            SDL_free(tests);
            cap = count;
            tests = SDL_malloc(cap * sizeof *tests);
            count = parse_dat(buf, n, tests, cap);
        }
        size_t file_pass = 0;
        size_t file_total = 0;
        for (size_t i = 0; i < count; i++) {
            if (tests[i].script == -1) {
                check_one(&st, g_names[f], &tests[i], false, &file_pass, &file_total);
                check_one(&st, g_names[f], &tests[i], true, &file_pass, &file_total);
            } else {
                check_one(&st, g_names[f], &tests[i], tests[i].script == 1, &file_pass,
                          &file_total);
            }
        }
        SDL_Log("%-48s %4llu / %-4llu", g_names[f], (unsigned long long)file_pass, (unsigned long long)file_total);
        files_run++;
        SDL_free(tests);
        SDL_free(buf);
    }

    /* Listed keys that matched no run are stale: report them and fail. */
    size_t stale = 0;
    for (size_t i = 0; i < g_expect_count; i++) {
        if (!g_expect_seen[i]) {
            SDL_Log("FAIL stale expectation %s (no such run)", g_expect[i]);
            stale++;
        }
    }

    size_t gate_runs = st.total - st.known;
    unsigned raw_x100 = st.total ? (unsigned)((st.passed * 10000u) / st.total) : 0u;
    /* Passing runs that are not listed: every passing run minus the listed passing runs, which
     * are unexpected passes and already counted as failures above. */
    size_t gate_pass = st.passed - st.unexpected_pass;
    unsigned gate_x100 = gate_runs ? (unsigned)((gate_pass * 10000u) / gate_runs) : 0u;
    SDL_Log("html tree-construction: %llu files, %llu runs", (unsigned long long)files_run, (unsigned long long)st.total);
    SDL_Log("  raw pass rate (all runs):           %llu / %llu = %u.%02u%%", (unsigned long long)st.passed, (unsigned long long)st.total, raw_x100 / 100u, raw_x100 % 100u);
    SDL_Log("  gate pass rate (runs not listed):   %llu / %llu = %u.%02u%% (target >= 99%%)", (unsigned long long)gate_pass, (unsigned long long)gate_runs, gate_x100 / 100u, gate_x100 % 100u);
    SDL_Log("  listed runs: %llu (cap %u), of which failing as recorded: %llu", (unsigned long long)st.known, DAT_MAX_KNOWN, (unsigned long long)st.expected_fail);
    SDL_Log("  unexpected failures: %llu, unexpected passes: %llu, stale or malformed entries: %llu",
            (unsigned long long)st.unexpected_fail, (unsigned long long)st.unexpected_pass,
            (unsigned long long)(stale + g_expect_bad));

    EMB_CHECK(files_run > 0);
    EMB_CHECK(st.unexpected_fail == 0);
    EMB_CHECK(st.unexpected_pass == 0);
    EMB_CHECK(stale == 0 && g_expect_bad == 0);
    EMB_CHECK(st.known <= DAT_MAX_KNOWN);
    EMB_CHECK(gate_runs > 0 && gate_pass * 10000u >= gate_runs * DAT_GATE_PCT_X100);

    for (size_t i = 0; i < g_name_count; i++) {
        SDL_free(g_names[i]);
    }
    g_name_count = 0;
    for (size_t i = 0; i < g_expect_count; i++) {
        SDL_free(g_expect[i]);
    }
    g_expect_count = 0;
}

EMB_TEST_MAIN()
