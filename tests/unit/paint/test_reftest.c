/*
 * The WPT reftest runner (W4f). Every line of tests/wpt/reftests.txt (and of tests/reftest/ours/reftests.txt) is one
 * test. The runner renders the test and each of its <link rel=match> / <link rel=mismatch> references at 800x600
 * through the public embedding API (an offscreen view on a host device) with a resource handler that serves
 * http://wpt.test/ from tests/wpt (so root-relative URLs such as /fonts/ahem.css work) and http://ours.test/ from
 * tests/reftest/ours. Pixels are compared with the test's <meta name=fuzzy> tolerance (maxDifference and
 * totalPixels ranges), or exactly. A reference with its own references is followed (the chain); several references
 * of one file must all hold.
 *
 * XHTML (.xht): the files go through the HTML parser. The handler drops the CDATA markers (<![CDATA[ and ]]>) of an
 * .xht/.xhtml/.xml file so that inline <style> text parses as CSS; the XML prolog is a bogus comment to the HTML parser.
 *
 * Modes (no argument: the smoke subset, which is what ctest runs):
 *   (none)             the smoke subset: every 40th test of each directory plus all of tests/reftest/ours
 *   --all              every test of both lists
 *   --only <id>        one test (id is the path in the list, e.g. css/CSS2/abspos/x.xht), with verbose output
 *   --filter <substr>  the tests whose id contains substr
 *   --threads <n>      worker processes (also EMBER_REFTEST_THREADS); default: the CPU count for --all, 2 otherwise
 *   --regen <dir>      write one expectations file per directory from this run's failures (reason tags are guessed
 *                      from the files' content; review them), instead of checking against the expectations
 *   --verbose          print every failure with its details
 *   --dump <id>        log the fragment tree of one page
 *   --shot <id>        render one page (an id as above) and write <out>/shot.bmp, for debugging
 * A run writes <out>/failures.txt (id, a tab, the detail; one line per failing test) for tools/wpt/classify_reftests.py.
 * Environment: EMBER_REFTEST_ONLY=<id> is --only; EMBER_REFTEST_OUT overrides the output directory (default
 * <source>/build/tests/reftest-out, which receives the .test/.ref/.diff BMPs of every unexpected failure, and with
 * EMBER_REFTEST_SAVE=all of every failure); EMBER_REFTEST_TIMEOUT is the seconds a worker may take for one test.
 *
 * Workers are processes (this executable with --worker), each with its own runtime, device and view, writing one
 * results file; a crash or a hang costs one test (it is recorded as CRASH or TIMEOUT and the worker restarts).
 *
 * Expectations: tests/reftest/expectations/<dir>.txt, lines "<id> <reason> [note]". A failing test must be listed,
 * a listed test must fail, a listed id must be in the list, ids are unique, reasons are from a fixed set, and the
 * "bug" entries of a directory are at most 3% of its tests (checked by --all only).
 */
#define SDL_MAIN_HANDLED
#include "emb_test.h"
#include "embed/embed_int.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WPT_ROOT EMBER_SOURCE_DIR "/tests/wpt"
#define OURS_ROOT EMBER_SOURCE_DIR "/tests/reftest/ours"
#define WPT_LIST EMBER_SOURCE_DIR "/tests/wpt/reftests.txt"
#define OURS_LIST EMBER_SOURCE_DIR "/tests/reftest/ours/reftests.txt"
#define EXPECT_DIR EMBER_SOURCE_DIR "/tests/reftest/expectations"
#define VIEW_W 800u
#define VIEW_H 600u
#define ID_MAX 1024
#define SMOKE_STRIDE 40
#define BUG_CAP_PERCENT 3
#define SKIP_CODE 77
/* A match pair whose pixels differ by at most NOISE_MAXDIFF per channel counts as equal (colour rounding differs between
 * llvmpipe and the Windows drivers). Mismatch pairs stay exact. */
#define NOISE_MAXDIFF 1
#define MAX_REFS 16
#define MAX_FUZZY 8
#define CACHE_MAX 8
#define MAX_DEPTH 8

/* Seconds a worker may go without finishing a test before it is killed; sanitizer builds are much slower. */
#if defined(__SANITIZE_ADDRESS__)
#define EMB_TEST_ASAN_TIMEOUT_DEFAULT 120u
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EMB_TEST_ASAN_TIMEOUT_DEFAULT 120u
#endif
#endif
#ifndef EMB_TEST_ASAN_TIMEOUT_DEFAULT
#define EMB_TEST_ASAN_TIMEOUT_DEFAULT 20u
#endif

static const char *const k_reasons[] = {
    "not-yet",     /* a feature the engine does not implement yet (note names it) */
    "writing-mode",
    "multicol",
    "r3-effects",  /* box-shadow, text-shadow, filter, masks, clip-path, blend modes, border-image */
    "svg",
    "jpeg",
    "xhtml",       /* needs real XML parsing */
    "font-metrics",
    "platform",    /* passes on some GPU drivers and fails on others (antialiasing above the noise floor): either result is fine */
    "needs-js", "needs-http",
    "out-of-scope",
    "not-applicable", /* the test expects behaviour a later spec superseded (note: "superseded by <spec>"; README quotes the section) */
    "layout",      /* a layout feature that is documented as not done (note names it) */
    "paint",       /* a paint feature that is documented as not done (note names it) */
    "blur-pyramid",/* a blur sigma above the direct limit runs on a downsampled pyramid */
    "bug",
};

/* ------------------------------------------------------------------ small utilities */

static bool starts_with(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && SDL_strcasecmp(s + n - m, suffix) == 0;
}

/* The directory a test belongs to: "CSS2" for css/CSS2/x/y.xht, "ours" for ours/x.html. */
static void dir_of_id(const char *id, char *out, size_t cap)
{
    const char *p = id;
    if (starts_with(p, "css/")) {
        p += 4;
    }
    if (starts_with(p, "svg/")) {
        /* R4c: svg/painting/x.html belongs to "svg-painting" */
        const char *sub = p + 4;
        const char *sl = strchr(sub, '/');
        size_t m = sl != nullptr ? (size_t)(sl - sub) : strlen(sub);
        SDL_snprintf(out, cap, "svg-%.*s", (int)m, sub);
        return;
    }
    const char *slash = strchr(p, '/');
    size_t n = slash != nullptr ? (size_t)(slash - p) : strlen(p);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* The file system path of an id. */
static void id_to_path(const char *id, char *out, size_t cap)
{
    if (starts_with(id, "ours/")) {
        SDL_snprintf(out, cap, "%s/%s", OURS_ROOT, id + 5);
    } else {
        SDL_snprintf(out, cap, "%s/%s", WPT_ROOT, id);
    }
}

/* SZ3b: whether the main file of a test mentions an SVG or canvas feature that this build leaves out. */
static bool feature_off_id(const char *id)
{
    char path[512];
    id_to_path(id, path, sizeof path);
    return emb_test_source_needs_disabled_feature(path);
}

static void id_to_url(const char *id, char *out, size_t cap)
{
    if (starts_with(id, "ours/")) {
        SDL_snprintf(out, cap, "http://ours.test/%s", id + 5);
    } else if (ends_with(id, ".svg")) {
        /* R4c: a standalone SVG test or reference is a page: the handler wraps it in an HTML shell (see res_handler) */
        SDL_snprintf(out, cap, "http://wpt.test/%s?ember-page", id);
    } else {
        SDL_snprintf(out, cap, "http://wpt.test/%s", id);
    }
}

/* Resolves href (relative to the file id base, or root-relative) to an id. Returns false for absolute URLs. */
static bool resolve_id(const char *base, const char *href, char *out, size_t cap)
{
    char tmp[ID_MAX * 2];
    /* URL parsing strips leading and trailing ASCII whitespace from an href (some tests have a trailing space). */
    char trimmed[ID_MAX * 2];
    while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r') {
        href++;
    }
    size_t tl = strlen(href);
    while (tl > 0 && (href[tl - 1] == ' ' || href[tl - 1] == '\t' || href[tl - 1] == '\n' || href[tl - 1] == '\r')) {
        tl--;
    }
    if (tl >= sizeof trimmed) {
        return false;
    }
    memcpy(trimmed, href, tl);
    trimmed[tl] = '\0';
    href = trimmed;
    size_t hl = strcspn(href, "?#");
    if (hl == 0 || hl >= ID_MAX || strstr(href, "://") != nullptr) {
        return false;
    }
    const char *prefix = starts_with(base, "ours/") ? "ours/" : "";
    if (href[0] == '/') {
        SDL_snprintf(tmp, sizeof tmp, "%s%.*s", prefix, (int)hl - 1, href + 1);
    } else {
        const char *slash = strrchr(base, '/');
        size_t dl = slash != nullptr ? (size_t)(slash - base) + 1 : 0;
        SDL_snprintf(tmp, sizeof tmp, "%.*s%.*s", (int)dl, base, (int)hl, href);
    }
    /* Normalize "." and ".." segments. */
    char *segs[64];
    int ns = 0;
    char *save = tmp;
    for (char *tok = tmp; tok != nullptr;) {
        char *next = strchr(tok, '/');
        if (next != nullptr) {
            *next = '\0';
        }
        if (strcmp(tok, "..") == 0) {
            if (ns > 0) {
                ns--;
            }
        } else if (strcmp(tok, ".") != 0 && tok[0] != '\0' && ns < 64) {
            segs[ns++] = tok;
        }
        tok = next != nullptr ? next + 1 : nullptr;
    }
    (void)save;
    size_t pos = 0;
    out[0] = '\0';
    for (int i = 0; i < ns; i++) {
        int w = SDL_snprintf(out + pos, cap - pos, "%s%s", i > 0 ? "/" : "", segs[i]);
        if (w < 0 || (size_t)w >= cap - pos) {
            return false;
        }
        pos += (size_t)w;
    }
    return pos > 0;
}

static void *load_file(const char *path, size_t *n)
{
    return SDL_LoadFile(path, n);
}

/* Lines of a text buffer (modified in place: newlines become NULs). The lines point into buf, which must come from
 * SDL_LoadFile (it is NUL-terminated past n); free both the array and buf when done. */
static char **split_lines(char *buf, size_t n, uint32_t *count)
{
    uint32_t cap = 16, cnt = 0;
    char **v = SDL_malloc(cap * sizeof *v);
    size_t i = 0;
    if (v == nullptr) {
        *count = 0;
        return nullptr;
    }
    while (i < n) {
        size_t j = i;
        while (j < n && buf[j] != '\n') {
            j++;
        }
        char *line = buf + i;
        if (j < n) {
            buf[j] = '\0';
        } /* else: the last line is unterminated; SDL_LoadFile NUL-terminates the buffer at buf[n] */
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\r') {
            line[l - 1] = '\0';
        }
        if (cnt == cap) {
            cap *= 2;
            char **nv = SDL_realloc(v, cap * sizeof *v);
            if (nv == nullptr) {
                break;
            }
            v = nv;
        }
        v[cnt++] = line;
        i = j + 1;
    }
    *count = cnt;
    return v;
}

/* ------------------------------------------------------------------ test lists */

typedef struct tlist {
    char **ids;
    uint32_t n, cap;
} tlist;

static void tlist_add(tlist *l, const char *id)
{
    if (l->n == l->cap) {
        uint32_t ncap = l->cap != 0 ? l->cap * 2 : 1024;
        char **nids = SDL_realloc(l->ids, ncap * sizeof *l->ids);
        if (nids == nullptr) {
            return;
        }
        l->ids = nids;
        l->cap = ncap;
    }
    char *dup = SDL_strdup(id);
    if (dup != nullptr) {
        l->ids[l->n++] = dup;
    }
}

static bool tlist_load(tlist *l, const char *path, const char *prefix)
{
    size_t n = 0;
    char *buf = load_file(path, &n);
    if (buf == nullptr) {
        return false;
    }
    uint32_t cnt = 0;
    char **lines = split_lines(buf, n, &cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        const char *s = lines[i];
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char id[ID_MAX];
        SDL_snprintf(id, sizeof id, "%s%s", prefix, s);
        tlist_add(l, id);
    }
    SDL_free(lines);
    SDL_free(buf);
    return true;
}

/* ------------------------------------------------------------------ expectations */

typedef struct expect {
    char *id;
    char reason[24];
    char *note;
    bool seen;
} expect;

static expect *g_expect;
static uint32_t g_nexpect, g_capexpect;
static uint32_t g_expect_errors;

static int expect_cmp(const void *a, const void *b)
{
    return strcmp(((const expect *)a)->id, ((const expect *)b)->id);
}

static bool reason_known(const char *r)
{
    for (size_t i = 0; i < SDL_arraysize(k_reasons); i++) {
        if (strcmp(r, k_reasons[i]) == 0) {
            return true;
        }
    }
    return false;
}

static SDL_EnumerationResult enum_expect_cb(void *user, const char *dirname, const char *fname)
{
    (void)user;
    if (!ends_with(fname, ".txt")) {
        return SDL_ENUM_CONTINUE;
    }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", dirname, fname);
    size_t n = 0;
    char *buf = load_file(path, &n);
    if (buf == nullptr) {
        return SDL_ENUM_CONTINUE;
    }
    uint32_t cnt = 0;
    char **lines = split_lines(buf, n, &cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        char *s = lines[i];
        if (s[0] == '\0' || s[0] == '#') {
            continue;
        }
        char *sp = strchr(s, ' ');
        if (sp == nullptr) {
            SDL_Log("expectations %s:%u: no reason tag", fname, i + 1);
            g_expect_errors++;
            continue;
        }
        *sp++ = '\0';
        char *note = strchr(sp, ' ');
        if (note != nullptr) {
            *note++ = '\0';
        }
        if (!reason_known(sp)) {
            SDL_Log("expectations %s:%u: unknown reason '%s'", fname, i + 1, sp);
            g_expect_errors++;
            continue;
        }
        if (g_nexpect == g_capexpect) {
            g_capexpect = g_capexpect != 0 ? g_capexpect * 2 : 1024;
            g_expect = SDL_realloc(g_expect, g_capexpect * sizeof *g_expect);
        }
        expect *e = &g_expect[g_nexpect++];
        memset(e, 0, sizeof *e);
        e->id = SDL_strdup(s);
        SDL_strlcpy(e->reason, sp, sizeof e->reason);
        e->note = SDL_strdup(note != nullptr ? note : "");
    }
    SDL_free(lines);
    SDL_free(buf);
    return SDL_ENUM_CONTINUE;
}

static void expect_free(void)
{
    for (uint32_t i = 0; i < g_nexpect; i++) {
        SDL_free(g_expect[i].id);
        SDL_free(g_expect[i].note);
    }
    SDL_free(g_expect);
    g_expect = nullptr;
    g_nexpect = g_capexpect = 0;
}

static void tlist_free(tlist *l)
{
    for (uint32_t i = 0; i < l->n; i++) {
        SDL_free(l->ids[i]);
    }
    SDL_free(l->ids);
    memset(l, 0, sizeof *l);
}

static void expect_load(void)
{
    SDL_EnumerateDirectory(EXPECT_DIR, enum_expect_cb, nullptr);
    qsort(g_expect, g_nexpect, sizeof *g_expect, expect_cmp);
    for (uint32_t i = 1; i < g_nexpect; i++) {
        if (strcmp(g_expect[i - 1].id, g_expect[i].id) == 0) {
            SDL_Log("expectations: %s is listed twice", g_expect[i].id);
            g_expect_errors++;
        }
    }
}

static expect *expect_find(const char *id)
{
    expect key = {.id = (char *)id};
    return bsearch(&key, g_expect, g_nexpect, sizeof *g_expect, expect_cmp);
}

/* ------------------------------------------------------------------ test file metadata */

typedef struct range {
    uint32_t lo, hi;
} range;

typedef struct fuzzy {
    char ref[ID_MAX]; /* "" applies to every reference */
    range maxdiff, total;
} fuzzy;

typedef struct ref_ent {
    char id[ID_MAX];
    bool mismatch;
} ref_ent;

typedef struct meta {
    ref_ent refs[MAX_REFS];
    uint32_t nrefs;
    fuzzy fuzzy[MAX_FUZZY];
    uint32_t nfuzzy;
    char steps[256]; /* <meta name=ember-steps>: how the page is driven before the capture (see run_steps) */
} meta;

/* Reads one attribute value of a tag: name=value with ' or " quotes, or unquoted. */
static bool tag_attr(const char *tag, const char *end, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = tag; p < end; p++) {
        if (p > tag && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '"' || p[-1] == '\'') &&
            (size_t)(end - p) > nl && SDL_strncasecmp(p, name, nl) == 0) {
            const char *q = p + nl;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
                q++;
            }
            if (q >= end || *q != '=') {
                continue;
            }
            q++;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
                q++;
            }
            const char *v0 = q, *v1;
            if (q < end && (*q == '"' || *q == '\'')) {
                char quote = *q++;
                v0 = q;
                while (q < end && *q != quote) {
                    q++;
                }
                v1 = q;
            } else {
                /* an unquoted value runs to white space or the tag end; a '/' ends it only as the tag's "/>" */
                while (q < end && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && !(*q == '/' && q + 1 == end)) {
                    q++;
                }
                v1 = q;
            }
            size_t n = (size_t)(v1 - v0);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, v0, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

static bool parse_range(const char *s, range *r)
{
    char *e = nullptr;
    unsigned long a = strtoul(s, &e, 10);
    if (e == s) {
        return false;
    }
    if (*e == '-') {
        char *e2 = nullptr;
        unsigned long b = strtoul(e + 1, &e2, 10);
        if (e2 == e + 1) {
            return false;
        }
        r->lo = (uint32_t)a;
        r->hi = (uint32_t)b;
    } else {
        r->lo = 0;
        r->hi = (uint32_t)a;
    }
    return true;
}

/* content = [ref ":"] ("maxDifference=" range ";" "totalPixels=" range | range ";" range) */
static void parse_fuzzy(const char *base, const char *content, meta *m)
{
    if (m->nfuzzy >= MAX_FUZZY) {
        return;
    }
    fuzzy *f = &m->fuzzy[m->nfuzzy];
    memset(f, 0, sizeof *f);
    f->maxdiff = (range){0, 0};
    f->total = (range){0, 0};
    while (*content == ' ' || *content == '\n' || *content == '\t' || *content == '\r') {
        content++;
    }
    const char *colon = strchr(content, ':');
    if (colon != nullptr) {
        char ref[ID_MAX];
        size_t n = (size_t)(colon - content);
        if (n >= sizeof ref) {
            n = sizeof ref - 1;
        }
        memcpy(ref, content, n);
        ref[n] = '\0';
        if (!resolve_id(base, ref, f->ref, sizeof f->ref)) {
            f->ref[0] = '\0';
        }
        content = colon + 1;
    }
    char tmp[256];
    SDL_strlcpy(tmp, content, sizeof tmp);
    uint32_t part = 0;
    char *sv = nullptr;
    for (char *tok = SDL_strtok_r(tmp, ";", &sv); tok != nullptr; tok = SDL_strtok_r(nullptr, ";", &sv)) {
        while (*tok == ' ' || *tok == '\n' || *tok == '\t' || *tok == '\r') {
            tok++;
        }
        if (starts_with(tok, "maxDifference=")) {
            parse_range(tok + 14, &f->maxdiff);
        } else if (starts_with(tok, "totalPixels=")) {
            parse_range(tok + 12, &f->total);
        } else if (part == 0) {
            parse_range(tok, &f->maxdiff);
            part = 1;
        } else {
            parse_range(tok, &f->total);
        }
    }
    m->nfuzzy++;
}

static void meta_read(const char *id, meta *m)
{
    memset(m, 0, sizeof *m);
    char path[1024];
    id_to_path(id, path, sizeof path);
    size_t n = 0;
    char *buf = load_file(path, &n);
    if (buf == nullptr) {
        return;
    }
    for (size_t i = 0; i + 5 < n; i++) {
        if (buf[i] != '<') {
            continue;
        }
        bool html_pfx = SDL_strncasecmp(buf + i, "<html:", 6) == 0; /* standalone SVG tests: <html:link rel="match" .../> */
        size_t pfx = html_pfx ? 6 : 1;
        bool is_link = SDL_strncasecmp(buf + i + pfx, "link", 4) == 0;
        bool is_meta = SDL_strncasecmp(buf + i + pfx, "meta", 4) == 0;
        if (!is_link && !is_meta) {
            continue;
        }
        const char *tag = buf + i + pfx + 4;
        const char *end = tag;
        const char *bufend = buf + n;
        char quote = 0;
        while (end < bufend && (quote != 0 || *end != '>')) {
            if (quote != 0) {
                if (*end == quote) {
                    quote = 0;
                }
            } else if (*end == '"' || *end == '\'') {
                quote = *end;
            }
            end++;
        }
        char rel[64], href[ID_MAX], name[64], content[256];
        if (is_link && tag_attr(tag, end, "rel", rel, sizeof rel) && tag_attr(tag, end, "href", href, sizeof href)) {
            bool match = SDL_strcasecmp(rel, "match") == 0;
            bool mism = SDL_strcasecmp(rel, "mismatch") == 0;
            if ((match || mism) && m->nrefs < MAX_REFS) {
                ref_ent *r = &m->refs[m->nrefs];
                if (resolve_id(id, href, r->id, sizeof r->id)) {
                    r->mismatch = mism;
                    m->nrefs++;
                }
            }
        } else if (is_meta && tag_attr(tag, end, "name", name, sizeof name) && SDL_strcasecmp(name, "fuzzy") == 0 &&
                   tag_attr(tag, end, "content", content, sizeof content)) {
            parse_fuzzy(id, content, m);
        } else if (is_meta && tag_attr(tag, end, "name", name, sizeof name) && SDL_strcasecmp(name, "ember-steps") == 0 &&
                   tag_attr(tag, end, "content", content, sizeof content)) {
            SDL_strlcpy(m->steps, content, sizeof m->steps);
        }
        i = (size_t)(end - buf);
    }
    SDL_free(buf);
}

/* ------------------------------------------------------------------ resource handler */

static char *g_res_buf; /* the bytes the last handler call returned, kept until the next call */

static bool is_void_name(const char *n, size_t len)
{
    static const char *const v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    for (size_t i = 0; i < SDL_arraysize(v); i++) {
        if (strlen(v[i]) == len && SDL_strncasecmp(n, v[i], len) == 0) {
            return true;
        }
    }
    return false;
}

/* Turns an XHTML file into what the HTML parser needs: the CDATA markers are dropped (the text inside stays: <style>
 * and <script> read it as raw text), and an XML empty-element tag of a non-void element, <div class="a"/>, becomes
 * <div class="a"></div> (the HTML parser would take it for a start tag and nest what follows). Returns a new
 * SDL_malloc'd buffer and its length; buf is freed. */
static char *xhtml_to_html(char *buf, size_t n, size_t *out_n, bool is_xml)
{
    char *out = SDL_malloc(n * 2 + 16);
    size_t w = 0;
    bool in_style = false, in_cdata = false;
    for (size_t r = 0; r < n;) {
        if (n - r >= 9 && memcmp(buf + r, "<![CDATA[", 9) == 0) {
            r += 9;
            in_cdata = true;
        } else if (n - r >= 3 && memcmp(buf + r, "]]>", 3) == 0) {
            r += 3;
            in_cdata = false;
        } else if (in_style && !in_cdata && buf[r] == '&' &&
                   (n - r >= 4 && (memcmp(buf + r, "&gt;", 4) == 0 || memcmp(buf + r, "&lt;", 4) == 0))) {
            /* XML decodes the entities of the text of a style element (outside CDATA); the HTML parser does not. */
            out[w++] = buf[r + 1] == 'g' ? '>' : '<';
            r += 4;
        } else if (in_style && !in_cdata && n - r >= 5 && memcmp(buf + r, "&amp;", 5) == 0) {
            out[w++] = '&';
            r += 5;
        } else if (n - r >= 4 && memcmp(buf + r, "<!--", 4) == 0) {
            size_t e = r + 4;
            while (e + 2 < n && memcmp(buf + e, "-->", 3) != 0) {
                e++;
            }
            e = e + 3 <= n ? e + 3 : n;
            if (!in_style || !is_xml) {
                memcpy(out + w, buf + r, e - r);
                w += e - r;
            } /* else: in a real XML document (.xhtml, .xml; .xht is served as HTML) a comment inside a style element is not part of its text */
            r = e;
        } else if (in_style && !in_cdata && n - r >= 2 && buf[r] == '<' && buf[r + 1] == '/') {
            in_style = false;
            out[w++] = buf[r++];
        } else if (buf[r] == '<' && r + 1 < n && SDL_isalpha((unsigned char)buf[r + 1])) {
            size_t e = r + 1;
            char quote = 0;
            while (e < n && (quote != 0 || buf[e] != '>')) {
                if (quote != 0) {
                    quote = buf[e] == quote ? 0 : quote;
                } else if (buf[e] == '"' || buf[e] == 0x27) {
                    quote = buf[e];
                }
                e++;
            }
            size_t name_len = 0;
            while (r + 1 + name_len < e && !SDL_isspace((unsigned char)buf[r + 1 + name_len]) && buf[r + 1 + name_len] != '/' &&
                   buf[r + 1 + name_len] != '>') {
                name_len++;
            }
            bool empty = e < n && e > r + 1 && buf[e - 1] == '/' && !is_void_name(buf + r + 1, name_len);
            in_style = name_len == 5 && SDL_strncasecmp(buf + r + 1, "style", 5) == 0 && !empty;
            size_t body_end = empty ? e - 1 : e;
            memcpy(out + w, buf + r, body_end - r);
            w += body_end - r;
            if (e < n) {
                out[w++] = '>';
                if (empty) {
                    out[w++] = '<';
                    out[w++] = '/';
                    memcpy(out + w, buf + r + 1, name_len);
                    w += name_len;
                    out[w++] = '>';
                }
                e++;
            }
            r = e;
        } else {
            out[w++] = buf[r++];
        }
    }
    SDL_free(buf);
    *out_n = w;
    return out;
}

static bool res_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    (void)user;
    (void)url_len;
    const char *root;
    const char *p;
    bool ours = false;
    if (starts_with(url, "http://wpt.test/")) {
        root = WPT_ROOT;
        p = url + 15;
    } else if (starts_with(url, "http://ours.test/")) {
        root = OURS_ROOT;
        ours = true;
        p = url + 16;
    } else {
        return false;
    }
    char path[1024];
    size_t pl = strcspn(p, "?#");
    size_t w = (size_t)SDL_snprintf(path, sizeof path, "%s", root);
    for (size_t i = 0; i < pl && w + 1 < sizeof path; i++) {
        if (p[i] == '%' && i + 2 < pl + 1 && SDL_isxdigit((unsigned char)p[i + 1]) && SDL_isxdigit((unsigned char)p[i + 2])) {
            char hex[3] = {p[i + 1], p[i + 2], 0};
            path[w++] = (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            path[w++] = p[i];
        }
    }
    path[w] = '\0';
    if (strstr(path, "..") != nullptr) {
        return false;
    }
    SDL_free(g_res_buf);
    size_t n = 0;
    g_res_buf = load_file(path, &n);
    if (g_res_buf == nullptr && ours) {
        /* Our own tests find the WPT support files (fonts, stylesheets) at the same root-relative paths. */
        SDL_snprintf(path, sizeof path, "%s%s", WPT_ROOT, path + sizeof OURS_ROOT - 1);
        g_res_buf = load_file(path, &n);
    }
    if (g_res_buf == nullptr) {
        return false;
    }
    if (ends_with(path, ".xht") || ends_with(path, ".xhtml") || ends_with(path, ".xml")) {
        g_res_buf = xhtml_to_html(g_res_buf, n, &n, !ends_with(path, ".xht"));
    } else if (ends_with(path, ".svg") && strstr(p, "ember-page") != nullptr) {
        /* R4c: a standalone SVG document is shown as the HTML shell a browser's own SVG viewer is equivalent to: no margin,
         * the root fills the viewport unless it has a size */
        static const char shell[] =
            "<!doctype html><style>html,body{margin:0;padding:0}body>svg{display:block}"
            "body>svg:not([width]){width:100vw}body>svg:not([height]){height:100vh}</style>";
        char *w2 = SDL_malloc(sizeof shell + n);
        memcpy(w2, shell, sizeof shell - 1);
        size_t wn = sizeof shell - 1;
        /* <metadata> holds the XHTML <link> and <meta> of the test: the HTML parser would break out of the svg at <meta>,
         * so the element is dropped (as an XML parser would keep it apart, it paints nothing) */
        for (size_t r = 0; r < n;) {
            if (n - r >= 9 && memcmp(g_res_buf + r, "<metadata", 9) == 0) {
                const char *end = SDL_strstr(g_res_buf + r, "</metadata>");
                if (end != nullptr) {
                    r = (size_t)(end - g_res_buf) + 11;
                    continue;
                }
            }
            w2[wn++] = g_res_buf[r++];
        }
        SDL_free(g_res_buf);
        g_res_buf = w2;
        n = wn;
    }
    out->data = (const uint8_t *)g_res_buf;
    out->len = n;
    return true;
}

static void quiet_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    (void)level;
    (void)message;
}

/* ------------------------------------------------------------------ rendering */

typedef struct cache_ent {
    char id[ID_MAX];
    uint8_t *px; /* nullptr when the page did not load */
} cache_ent;

typedef struct rctx {
    emb_runtime *rt;
    embi_gpu *gpu;
    SDL_GPUDevice *dev;
    emb_view *view;
    cache_ent cache[CACHE_MAX];
    uint32_t ncache;
    char fail[512];
    uint32_t nfail_imgs;
    /* The images of the first failing comparison, for the output files. */
    uint8_t *img_a, *img_b;
    char img_a_id[ID_MAX], img_b_id[ID_MAX];
} rctx;

static void cache_clear(rctx *c)
{
    for (uint32_t i = 0; i < c->ncache; i++) {
        SDL_free(c->cache[i].px);
    }
    c->ncache = 0;
}

static bool render_wait(SDL_GPUDevice *dev, emb_view *v)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    (void)emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    return true;
}

static uint8_t *readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    uint8_t *out = nullptr;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (tb == nullptr || cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = tex;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = w;
    info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            SDL_memcpy(out, mapped, bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

/* A fresh runtime and view on the worker's device. Every page is rendered in its own runtime, so that nothing a page
 * leaves behind (web fonts registered under a family name, glyph and shaping caches, atlas state) can change what the
 * next page looks like: the results do not depend on the order of the tests or on which worker ran them. */
static bool rctx_open_view(rctx *c)
{
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = quiet_log;
    rd.flags = EMB_RUNTIME_NO_SYSTEM_FONTS; /* the expectations must not depend on the machine's fonts (W4i) */
    c->rt = emb_runtime_create(&rd);
    if (c->rt == nullptr) {
        return false;
    }
    emb_runtime_set_resource_handler(c->rt, "http", res_handler, nullptr);
    /* No system fonts, so the generic monospace family points at the WPT test font Ahem (every glyph is a 1 em square,
     * which is what the tests that set font-family: monospace rely on for equal advances). W4i. */
    {
        size_t flen = 0;
        void *ahem = SDL_LoadFile(WPT_ROOT "/fonts/Ahem.ttf", &flen);
        static const char *const mono[] = {"Ahem"};
        if (ahem != nullptr) {
            (void)emb_runtime_add_font(c->rt, ahem, flen);
            (void)emb_runtime_set_generic_family(c->rt, "monospace", mono, 1);
            SDL_free(ahem);
        }
    }
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = VIEW_W;
    vd.height = VIEW_H;
    vd.scale = 1.0f;
    vd.device = c->dev;
    c->view = emb_view_create(c->rt, &vd);
    return c->view != nullptr;
}

static void rctx_close_view(rctx *c)
{
    if (c->view != nullptr) {
        emb_view_destroy(c->view);
        c->view = nullptr;
    }
    if (c->rt != nullptr) {
        emb_runtime_destroy(c->rt);
        c->rt = nullptr;
    }
}

static bool rctx_open(rctx *c)
{
    memset(c, 0, sizeof *c);
    c->gpu = embi_gpu_create(nullptr, false);
    if (c->gpu == nullptr) {
        return false;
    }
    c->dev = embi_gpu_device(c->gpu);
    return rctx_open_view(c);
}

static void rctx_close(rctx *c)
{
    cache_clear(c);
    SDL_free(c->img_a);
    SDL_free(c->img_b);
    rctx_close_view(c);
    if (c->gpu != nullptr) {
        embi_gpu_destroy(c->gpu);
    }
    SDL_free(g_res_buf);
    g_res_buf = nullptr;
}

/* Drives the loaded page: steps are separated by ';' and are "render" (draw a frame, the damage path after the first
 * one), "scroll X Y" (emb_view_scroll_to) or "scrollby X Y". The last step should be a "render"; a page without
 * steps gets "render". This is how the ours/ reftests compare a partial redraw (render, scroll, render) with a full
 * one (scroll, render) and check scrolled content. */
static bool run_steps(rctx *c, const char *steps)
{
    char tmp[256];
    SDL_strlcpy(tmp, steps[0] != '\0' ? steps : "render", sizeof tmp);
    char *sv = nullptr;
    for (char *tok = SDL_strtok_r(tmp, ";", &sv); tok != nullptr; tok = SDL_strtok_r(nullptr, ";", &sv)) {
        while (*tok == ' ') {
            tok++;
        }
        if (starts_with(tok, "render")) {
            if (!render_wait(c->dev, c->view)) {
                return false;
            }
        } else if (starts_with(tok, "scrollby")) {
            float x = 0, y = 0;
            if (SDL_sscanf(tok + 8, "%f %f", &x, &y) == 2) {
                emb_view_scroll_by(c->view, x, y);
            }
        } else if (starts_with(tok, "scroll")) {
            float x = 0, y = 0;
            if (SDL_sscanf(tok + 6, "%f %f", &x, &y) == 2) {
                emb_view_scroll_to(c->view, x, y);
            }
        }
    }
    return true;
}

/* The rendering of a page (cached for the current test). nullptr when it did not load. */
static uint8_t *render_id(rctx *c, const char *id)
{
    for (uint32_t i = 0; i < c->ncache; i++) {
        if (strcmp(c->cache[i].id, id) == 0) {
            return c->cache[i].px;
        }
    }
    char url[ID_MAX + 32];
    id_to_url(id, url, sizeof url);
    uint8_t *px = nullptr;
    meta pm;
    meta_read(id, &pm);
    rctx_close_view(c);
    if (!rctx_open_view(c)) {
        return nullptr;
    }
    if (emb_view_load_url(c->view, url)) {
        if (run_steps(c, pm.steps)) {
            px = readback(c->dev, emb_view_texture(c->view), VIEW_W, VIEW_H);
        }
    }
    if (c->ncache == CACHE_MAX) {
        SDL_free(c->cache[0].px);
        memmove(&c->cache[0], &c->cache[1], (CACHE_MAX - 1) * sizeof c->cache[0]);
        c->ncache--;
    }
    cache_ent *e = &c->cache[c->ncache++];
    SDL_strlcpy(e->id, id, sizeof e->id);
    e->px = px;
    return px;
}

/* ------------------------------------------------------------------ comparison */

typedef struct cmp {
    uint64_t differing; /* pixels with some channel different */
    uint32_t maxdiff;
    uint32_t x0, y0, x1, y1; /* the bounding box of the differing pixels */
    uint8_t pa[3], pb[3];    /* the first differing pixel in each image */
} cmp;

static cmp compare_px(const uint8_t *a, const uint8_t *b)
{
    cmp r;
    memset(&r, 0, sizeof r);
    r.x0 = VIEW_W;
    r.y0 = VIEW_H;
    size_t n = (size_t)VIEW_W * VIEW_H;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *pa = a + i * 4, *pb = b + i * 4;
        uint32_t d = 0;
        for (int k = 0; k < 3; k++) {
            uint32_t x = pa[k] > pb[k] ? (uint32_t)(pa[k] - pb[k]) : (uint32_t)(pb[k] - pa[k]);
            if (x > d) {
                d = x;
            }
        }
        if (d != 0) {
            uint32_t px = (uint32_t)(i % VIEW_W), py = (uint32_t)(i / VIEW_W);
            if (r.differing == 0) {
                memcpy(r.pa, pa, 3);
                memcpy(r.pb, pb, 3);
            }
            r.differing++;
            if (d > r.maxdiff) {
                r.maxdiff = d;
            }
            r.x0 = px < r.x0 ? px : r.x0;
            r.y0 = py < r.y0 ? py : r.y0;
            r.x1 = px > r.x1 ? px : r.x1;
            r.y1 = py > r.y1 ? py : r.y1;
        }
    }
    return r;
}

static bool within(range r, uint64_t v)
{
    return v >= r.lo && v <= r.hi;
}

/* Whether the pair counts as "equal" for the reference ref of the test with metadata m. */
static bool equal_within_fuzzy(const meta *m, const char *ref, cmp r)
{
    const fuzzy *f = nullptr;
    for (uint32_t i = 0; i < m->nfuzzy; i++) {
        if (m->fuzzy[i].ref[0] == '\0' || strcmp(m->fuzzy[i].ref, ref) == 0) {
            f = &m->fuzzy[i];
            if (f->ref[0] != '\0') {
                break;
            }
        }
    }
    if (f == nullptr) {
        return r.differing == 0;
    }
    return within(f->maxdiff, r.maxdiff) && within(f->total, r.differing);
}

/* ------------------------------------------------------------------ one test */

static void set_fail(rctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_fail(rctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(c->fail, sizeof c->fail, fmt, ap);
    va_end(ap);
}

static void keep_images(rctx *c, const char *ida, const uint8_t *a, const char *idb, const uint8_t *b)
{
    size_t bytes = (size_t)VIEW_W * VIEW_H * 4u;
    SDL_free(c->img_a);
    SDL_free(c->img_b);
    c->img_a = a != nullptr ? SDL_malloc(bytes) : nullptr;
    c->img_b = b != nullptr ? SDL_malloc(bytes) : nullptr;
    if (a != nullptr && c->img_a != nullptr) {
        memcpy(c->img_a, a, bytes);
    }
    if (b != nullptr && c->img_b != nullptr) {
        memcpy(c->img_b, b, bytes);
    }
    SDL_strlcpy(c->img_a_id, ida, sizeof c->img_a_id);
    SDL_strlcpy(c->img_b_id, idb, sizeof c->img_b_id);
}

/* Checks the page id against its references (and theirs). Returns true when every relation holds. */
static bool check_node(rctx *c, const char *id, uint32_t depth)
{
    if (depth > MAX_DEPTH) {
        set_fail(c, "reference chain too deep at %s", id);
        return false;
    }
    meta m;
    meta_read(id, &m);
    if (m.nrefs == 0) {
        set_fail(c, "no reference in %s", id);
        return false;
    }
    uint8_t *a = render_id(c, id);
    if (a == nullptr) {
        set_fail(c, "load failed: %s", id);
        return false;
    }
    /* Like WPT's runner: every mismatch reference must differ, and one passing match reference is enough
     * (a match reference passes only when its own chain does). */
    bool have_match = false, match_ok = false;
    for (uint32_t i = 0; i < m.nrefs; i++) {
        const ref_ent *r = &m.refs[i];
        if (!r->mismatch) {
            have_match = true;
            if (match_ok) {
                continue;
            }
        }
        uint8_t *b = render_id(c, r->id);
        if (b == nullptr) {
            set_fail(c, "load failed: %s", r->id);
            if (r->mismatch) {
                return false;
            }
            continue;
        }
        a = render_id(c, id); /* may have been evicted from the cache */
        if (a == nullptr) {
            set_fail(c, "load failed: %s", id);
            return false;
        }
        cmp d = compare_px(a, b);
        bool eq = equal_within_fuzzy(&m, r->id, d);
        static int exact = -1; /* EMBER_REFTEST_EXACT=1 turns the noise floor off (exact comparison, as the spec says) */
        if (exact < 0) {
            const char *e = SDL_getenv("EMBER_REFTEST_EXACT");
            exact = e != nullptr && e[0] == '1';
        }
        if (!eq && !r->mismatch && !exact && (d.differing == 0 || d.maxdiff <= NOISE_MAXDIFF)) {
            /* The noise floor: one level per channel is rounding (llvmpipe and the Windows drivers convert 0.5 to 127 or 128),
             * and identical images match even when a fuzzy range asks for a minimum difference. */
            eq = true;
        }
        if (r->mismatch ? eq : !eq) {
            set_fail(c, "%s %s: %llu px differ, max %u, in (%u,%u)-(%u,%u), first %02x%02x%02x vs %02x%02x%02x",
                     r->mismatch ? "mismatch" : "match", r->id, (unsigned long long)d.differing, d.maxdiff, d.x0, d.y0,
                     d.x1, d.y1, d.pa[0], d.pa[1], d.pa[2], d.pb[0], d.pb[1], d.pb[2]);
            keep_images(c, id, a, r->id, b);
            if (r->mismatch) {
                return false;
            }
            continue;
        }
        if (!r->mismatch) {
            meta rm;
            meta_read(r->id, &rm);
            if (rm.nrefs > 0 && !check_node(c, r->id, depth + 1)) {
                continue;
            }
            match_ok = true;
            c->fail[0] = '\0'; /* an earlier alternative's failure is moot */
            keep_images(c, "", nullptr, "", nullptr);
        }
    }
    return !have_match || match_ok;
}

static void sanitize(const char *id, char *out, size_t cap)
{
    size_t i = 0;
    for (; id[i] != '\0' && i + 1 < cap; i++) {
        out[i] = id[i] == '/' ? '#' : id[i];
    }
    out[i] = '\0';
}

static void save_failure(rctx *c, const char *id)
{
    if (c->img_a == nullptr || c->img_b == nullptr) {
        return;
    }
    const char *outdir = SDL_getenv("EMBER_REFTEST_OUT");
    char dir[1024], name[ID_MAX], path[2048];
    if (outdir != nullptr && outdir[0] != '\0') {
        SDL_snprintf(dir, sizeof dir, "%s", outdir);
    } else {
        SDL_snprintf(dir, sizeof dir, "%s", EMBER_SOURCE_DIR "/build/tests/reftest-out");
    }
    SDL_CreateDirectory(dir);
    sanitize(id, name, sizeof name);
    size_t bytes = (size_t)VIEW_W * VIEW_H * 4u;
    uint8_t *diff = SDL_malloc(bytes);
    if (diff == nullptr) {
        return;
    }
    for (size_t i = 0; i < (size_t)VIEW_W * VIEW_H; i++) {
        const uint8_t *pa = c->img_a + i * 4, *pb = c->img_b + i * 4;
        bool differs = pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2];
        diff[i * 4 + 0] = differs ? 255 : (uint8_t)(pa[0] / 4 + 190);
        diff[i * 4 + 1] = differs ? 0 : (uint8_t)(pa[1] / 4 + 190);
        diff[i * 4 + 2] = differs ? 0 : (uint8_t)(pa[2] / 4 + 190);
        diff[i * 4 + 3] = 255;
    }
    const char *kinds[3] = {".test.bmp", ".ref.bmp", ".diff.bmp"};
    const uint8_t *imgs[3] = {c->img_a, c->img_b, diff};
    for (int k = 0; k < 3; k++) {
        SDL_snprintf(path, sizeof path, "%s/%s%s", dir, name, kinds[k]);
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)VIEW_W, (int)VIEW_H, SDL_PIXELFORMAT_RGBA32, (void *)imgs[k], (int)VIEW_W * 4);
        if (s != nullptr) {
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
        }
    }
    SDL_free(diff);
}

/* ------------------------------------------------------------------ worker */

static int run_worker(const char *listfile, uint32_t shard, uint32_t nshards, uint32_t from, const char *outfile)
{
    size_t n = 0;
    char *buf = load_file(listfile, &n);
    if (buf == nullptr) {
        SDL_Log("worker: cannot read %s", listfile);
        return 2;
    }
    uint32_t cnt = 0;
    char **ids = split_lines(buf, n, &cnt);
    expect_load();
    rctx c;
    memset(&c, 0, sizeof c);
    if (!SDL_Init(SDL_INIT_VIDEO) || !rctx_open(&c)) {
        rctx_close(&c);
        SDL_free(ids);
        SDL_free(buf);
        expect_free();
        return SKIP_CODE;
    }
    SDL_IOStream *out = outfile != nullptr ? SDL_IOFromFile(outfile, "wb") : nullptr;
    if (outfile != nullptr && out == nullptr) {
        SDL_Log("worker: cannot write %s", outfile);
        rctx_close(&c);
        SDL_free(ids);
        SDL_free(buf);
        expect_free();
        return 2;
    }
    const char *save_all = SDL_getenv("EMBER_REFTEST_SAVE");
    bool all = save_all != nullptr && strcmp(save_all, "all") == 0;
    for (uint32_t i = from; i < cnt; i++) {
        if (i % nshards != shard || ids[i][0] == '\0') {
            continue;
        }
        if (out != nullptr) {
            SDL_IOprintf(out, "B\t%u\n", i);
            SDL_FlushIO(out);
        }
        uint64_t t0 = SDL_GetTicks();
        c.fail[0] = '\0';
        bool ok = check_node(&c, ids[i], 0);
        uint64_t ms = SDL_GetTicks() - t0;
        if (!ok && (all || expect_find(ids[i]) == nullptr)) {
            save_failure(&c, ids[i]);
        }
        const char *line = ok ? "PASS" : "FAIL";
        if (out != nullptr) {
            SDL_IOprintf(out, "R\t%u\t%s\t%llu\t%s\n", i, line, (unsigned long long)ms, ok ? "" : c.fail);
            SDL_FlushIO(out);
        } else {
            SDL_Log("%s %s (%llu ms) %s", line, ids[i], (unsigned long long)ms, ok ? "" : c.fail);
        }
        cache_clear(&c);
    }
    if (out != nullptr) {
        SDL_IOprintf(out, "E\n");
        SDL_CloseIO(out);
    }
    rctx_close(&c);
    return 0;
}

/* ------------------------------------------------------------------ parent */

typedef enum status { ST_NONE = 0, ST_PASS, ST_FAIL, ST_CRASH, ST_TIMEOUT } status;

typedef struct result {
    uint8_t st;
    uint32_t ms;
    char *detail;
} result;

typedef struct shard {
    SDL_Process *proc;
    char file[1024];
    uint32_t next;       /* the first index this shard has not recorded yet */
    uint32_t spawns;
    uint64_t last_size;
    uint64_t last_progress_ms;
    bool done;
    uint32_t current;    /* the index of the last B line */
    bool have_current;
} shard;

static uint32_t uparse(const char *s, char **end)
{
    return (uint32_t)strtoul(s, end, 10);
}

/* Reads a worker's results file. Returns the highest index with a result + 1 and records them. */
static void absorb(const char *file, result *res, uint32_t total, shard *sh)
{
    size_t n = 0;
    char *buf = load_file(file, &n);
    if (buf == nullptr) {
        return;
    }
    uint32_t cnt = 0;
    char **lines = split_lines(buf, n, &cnt);
    bool complete_tail = n > 0 && buf[n - 1] == '\0'; /* the last line ended with a newline (it became a NUL) */
    for (uint32_t i = 0; i < cnt; i++) {
        char *l = lines[i];
        if (i + 1 == cnt && !complete_tail) {
            break;
        }
        if (l[0] == 'B' && l[1] == '\t') {
            sh->current = uparse(l + 2, nullptr);
            sh->have_current = true;
        } else if (l[0] == 'R' && l[1] == '\t') {
            char *e = nullptr;
            uint32_t idx = uparse(l + 2, &e);
            if (*e != '\t' || idx >= total) {
                continue;
            }
            char *st = e + 1;
            char *t = strchr(st, '\t');
            if (t == nullptr) {
                continue;
            }
            *t++ = '\0';
            char *d = strchr(t, '\t');
            if (d == nullptr) {
                continue;
            }
            *d++ = '\0';
            result *r = &res[idx];
            r->st = strcmp(st, "PASS") == 0 ? ST_PASS : ST_FAIL;
            r->ms = (uint32_t)strtoul(t, nullptr, 10);
            SDL_free(r->detail);
            r->detail = SDL_strdup(d);
            sh->have_current = false;
            if (idx + 1 > sh->next) {
                sh->next = idx + 1;
            }
        }
    }
    SDL_free(lines);
    SDL_free(buf);
}

static bool spawn_worker(const char *exe, const char *listfile, shard *sh, uint32_t index, uint32_t nshards, const char *outdir)
{
    char a_shard[32], a_n[32], a_from[32];
    SDL_snprintf(a_shard, sizeof a_shard, "%u", index);
    SDL_snprintf(a_n, sizeof a_n, "%u", nshards);
    SDL_snprintf(a_from, sizeof a_from, "%u", sh->next);
    SDL_snprintf(sh->file, sizeof sh->file, "%s/worker-%u-%u.txt", outdir, index, sh->spawns++);
    const char *args[] = {exe, "--worker", listfile, a_shard, a_n, a_from, sh->file, nullptr};
    sh->proc = SDL_CreateProcess(args, false);
    sh->last_size = 0;
    sh->last_progress_ms = SDL_GetTicks();
    return sh->proc != nullptr;
}

static int cmp_slow(const void *a, const void *b)
{
    const uint32_t *x = a, *y = b;
    return (*x < *y) - (*x > *y);
}

typedef struct dirstat {
    char name[48];
    uint32_t total, pass, expected_fail, bug, unexpected_fail, unexpected_pass;
} dirstat;

static dirstat *dirstat_get(dirstat *v, uint32_t *n, const char *name)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (strcmp(v[i].name, name) == 0) {
            return &v[i];
        }
    }
    dirstat *d = &v[(*n)++];
    memset(d, 0, sizeof *d);
    SDL_strlcpy(d->name, name, sizeof d->name);
    return d;
}

/* ---- reason guessing for --regen */

static bool file_has(const char *id, const char *needle)
{
    char path[1024];
    id_to_path(id, path, sizeof path);
    size_t n = 0;
    char *buf = load_file(path, &n);
    if (buf == nullptr) {
        return false;
    }
    char *z = SDL_realloc(buf, n + 1);
    z[n] = '\0';
    bool r = SDL_strcasestr(z, needle) != nullptr;
    SDL_free(z);
    return r;
}

static bool chain_has(const char *id, const char *needle)
{
    if (file_has(id, needle)) {
        return true;
    }
    meta m;
    meta_read(id, &m);
    for (uint32_t i = 0; i < m.nrefs; i++) {
        if (file_has(m.refs[i].id, needle)) {
            return true;
        }
    }
    return false;
}

static const char *guess_reason(const char *id)
{
    static const char *const fx[] = {"box-shadow", "text-shadow", "filter:", "clip-path", "mask-", "mask:", "mix-blend-mode",
                                     "backdrop-filter", "border-image", "drop-shadow", "background-blend-mode"};
    for (size_t i = 0; i < SDL_arraysize(fx); i++) {
        if (chain_has(id, fx[i])) {
            return "r3-effects";
        }
    }
    if (chain_has(id, "column-count") || chain_has(id, "column-width") || chain_has(id, "columns:") ||
        chain_has(id, "column-rule") || chain_has(id, "column-span") || chain_has(id, "column-fill")) {
        return "multicol";
    }
    if (chain_has(id, "writing-mode") || chain_has(id, "vertical-")) {
        return "writing-mode";
    }
    return "bug";
}

static bool in_set(const tlist *sel, const char *id)
{
    for (uint32_t i = 0; i < sel->n; i++) {
        if (strcmp(sel->ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static int run_shot(const char *id)
{
    rctx c;
    memset(&c, 0, sizeof c);
    if (!SDL_Init(SDL_INIT_VIDEO) || !rctx_open(&c)) {
        rctx_close(&c);
        SDL_Log("no GPU device");
        return SKIP_CODE;
    }
    uint8_t *px = render_id(&c, id);
    int rc = 1;
    if (px != nullptr) {
        const char *outdir = SDL_getenv("EMBER_REFTEST_OUT");
        char dir[1024], path[1100];
        SDL_snprintf(dir, sizeof dir, "%s", outdir != nullptr && outdir[0] != '\0' ? outdir : EMBER_SOURCE_DIR "/build/tests/reftest-out");
        SDL_CreateDirectory(dir);
        SDL_snprintf(path, sizeof path, "%s/shot.bmp", dir);
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)VIEW_W, (int)VIEW_H, SDL_PIXELFORMAT_RGBA32, px, (int)VIEW_W * 4);
        if (s != nullptr && SDL_SaveBMP(s, path)) {
            SDL_Log("wrote %s", path);
            rc = 0;
        }
        if (s != nullptr) {
            SDL_DestroySurface(s);
        }
    } else {
        SDL_Log("load failed: %s", id);
    }
    rctx_close(&c);
    return rc;
}

/* Loads one page and logs its fragment tree (a debugging aid). */
static int run_dump(const char *id)
{
    rctx c;
    memset(&c, 0, sizeof c);
    if (!SDL_Init(SDL_INIT_VIDEO) || !rctx_open(&c)) {
        rctx_close(&c);
        return SKIP_CODE;
    }
    char url[ID_MAX + 32];
    id_to_url(id, url, sizeof url);
    if (emb_view_load_url(c.view, url)) {
        render_wait(c.dev, c.view);
        embi_doc_dump_layout(c.view->doc);
    }
    rctx_close(&c);
    return 0;
}

static int usage(void)
{
    SDL_Log("usage: test_paint_reftest [--all] [--only id] [--filter substr] [--threads n] [--regen dir] [--verbose]");
    return 2;
}

static void free_results(result *res, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        SDL_free(res[i].detail);
    }
    SDL_free(res);
}

int main(int argc, char **argv)
{
    if (argc >= 7 && strcmp(argv[1], "--worker") == 0) {
        return run_worker(argv[2], (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), strcmp(argv[6], "-") == 0 ? nullptr : argv[6]);
    }
    bool all = false, verbose = false;
    const char *only = SDL_getenv("EMBER_REFTEST_ONLY");
    const char *filter = nullptr, *regen = nullptr;
    int threads = 0;
    const char *tenv = SDL_getenv("EMBER_REFTEST_THREADS");
    if (tenv != nullptr) {
        threads = atoi(tenv);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            all = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            return run_shot(argv[i + 1]);
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            return run_dump(argv[i + 1]);
        } else if (strcmp(argv[i], "--regen") == 0 && i + 1 < argc) {
            regen = argv[++i];
        } else {
            return usage();
        }
    }
    if (only != nullptr && only[0] == '\0') {
        only = nullptr;
    }

    tlist full = {0};
    if (!tlist_load(&full, WPT_LIST, "")) {
        SDL_Log("cannot read %s", WPT_LIST);
        return 1;
    }
    uint32_t nwpt = full.n;
    tlist_load(&full, OURS_LIST, "ours/");

    /* The selection. */
    tlist sel = {0};
    if (only != nullptr) {
        tlist_add(&sel, only);
        verbose = true;
    } else {
        dirstat counters[64];
        uint32_t ncounters = 0;
        for (uint32_t i = 0; i < full.n; i++) {
            const char *id = full.ids[i];
            if (filter != nullptr && strstr(id, filter) == nullptr) {
                continue;
            }
            if (all || filter != nullptr || i >= nwpt) {
                tlist_add(&sel, id);
                continue;
            }
            char dn[48];
            dir_of_id(id, dn, sizeof dn);
            dirstat *d = dirstat_get(counters, &ncounters, dn);
            if (d->total++ % SMOKE_STRIDE == 0) {
                tlist_add(&sel, id);
            }
        }
    }
    if (sel.n == 0) {
        SDL_Log("no tests selected");
        tlist_free(&sel);
        tlist_free(&full);
        return 1;
    }
    if (threads <= 0) {
        threads = (all || filter != nullptr) ? SDL_GetNumLogicalCPUCores() : 2;
    }
    if ((uint32_t)threads > sel.n) {
        threads = (int)sel.n;
    }
    if (threads < 1) {
        threads = 1;
    }

    /* The selected ids go to a file the workers read. */
    const char *outenv = SDL_getenv("EMBER_REFTEST_OUT");
    char outdir[1024];
    SDL_snprintf(outdir, sizeof outdir, "%s", outenv != nullptr && outenv[0] != '\0' ? outenv : EMBER_SOURCE_DIR "/build/tests/reftest-out");
    SDL_CreateDirectory(EMBER_SOURCE_DIR "/build");
    SDL_CreateDirectory(EMBER_SOURCE_DIR "/build/tests");
    SDL_CreateDirectory(outdir);
    char listfile[1100];
    SDL_snprintf(listfile, sizeof listfile, "%s/selected.txt", outdir);
    {
        SDL_IOStream *io = SDL_IOFromFile(listfile, "wb");
        if (io == nullptr) {
            SDL_Log("cannot write %s", listfile);
            tlist_free(&sel);
            tlist_free(&full);
            return 1;
        }
        for (uint32_t i = 0; i < sel.n; i++) {
            SDL_IOprintf(io, "%s\n", sel.ids[i]);
        }
        SDL_CloseIO(io);
    }

    result *res = SDL_calloc(sel.n, sizeof *res);
    const char *exe = argv[0];
    uint32_t timeout_s = EMB_TEST_ASAN_TIMEOUT_DEFAULT;
    const char *to = SDL_getenv("EMBER_REFTEST_TIMEOUT");
    if (to != nullptr && atoi(to) > 0) {
        timeout_s = (uint32_t)atoi(to);
    }
    shard *shards = SDL_calloc((size_t)threads, sizeof *shards);
    uint32_t running = 0;
    bool skipped = false;
    for (int s = 0; s < threads; s++) {
        shards[s].next = 0;
        if (!spawn_worker(exe, listfile, &shards[s], (uint32_t)s, (uint32_t)threads, outdir)) {
            bool unsupported = s == 0;
            SDL_Log("cannot start worker %d: %s", s, SDL_GetError());
            for (int k = 0; k < s; k++) {
                if (shards[k].proc != nullptr) {
                    SDL_KillProcess(shards[k].proc, true);
                    SDL_DestroyProcess(shards[k].proc);
                }
            }
            SDL_free(shards);
            free_results(res, sel.n);
            tlist_free(&sel);
            tlist_free(&full);
            return unsupported ? SKIP_CODE : 1; /* no SDL_Process on this platform: skip */
        }
        running++;
    }
    uint64_t t_start = SDL_GetTicks(), t_report = t_start;
    while (running > 0) {
        SDL_Delay(100);
        for (int s = 0; s < threads; s++) {
            shard *sh = &shards[s];
            if (sh->proc == nullptr) {
                continue;
            }
            int code = 0;
            bool exited = SDL_WaitProcess(sh->proc, false, &code);
            SDL_PathInfo pi;
            uint64_t sz = SDL_GetPathInfo(sh->file, &pi) ? (uint64_t)pi.size : 0;
            uint64_t now = SDL_GetTicks();
            if (sz != sh->last_size) {
                sh->last_size = sz;
                sh->last_progress_ms = now;
            }
            bool hung = !exited && now - sh->last_progress_ms > (uint64_t)timeout_s * 1000u;
            if (!exited && !hung) {
                continue;
            }
            if (hung) {
                SDL_KillProcess(sh->proc, true);
                SDL_WaitProcess(sh->proc, true, &code);
                code = -1000;
            }
            SDL_DestroyProcess(sh->proc);
            sh->proc = nullptr;
            absorb(sh->file, res, sel.n, sh);
            /* Did the worker run to its end? Its file ends with an E line and the exit code is 0. */
            size_t fn = 0;
            char *fb = load_file(sh->file, &fn);
            bool ended = fb != nullptr && fn >= 2 && fb[fn - 1] == '\n' && fb[fn - 2] == 'E' && (fn == 2 || fb[fn - 3] == '\n');
            SDL_free(fb);
            if (code == SKIP_CODE && sh->spawns == 1 && !ended) {
                skipped = true;
                sh->done = true;
                running--;
                continue;
            }
            if (ended) {
                if (code != 0) {
                    SDL_Log("worker %d finished its tests but exited with code %d (not restarting)", s, code);
                }
                sh->done = true;
                running--;
                continue;
            }
            /* A crash or a hang: the test that was running fails, the worker restarts after it. */
            if (sh->have_current && sh->current < sel.n) {
                result *r = &res[sh->current];
                r->st = hung ? ST_TIMEOUT : ST_CRASH;
                SDL_free(r->detail);
                r->detail = SDL_strdup(hung ? "worker timed out" : "worker crashed");
                if (sh->current + 1 > sh->next) {
                    sh->next = sh->current + 1;
                }
                sh->have_current = false;
            } else if (sh->spawns > 40) {
                SDL_Log("worker %d keeps dying before it starts a test; giving up", s);
                sh->done = true;
                running--;
                continue;
            }
            if (!spawn_worker(exe, listfile, sh, (uint32_t)s, (uint32_t)threads, outdir)) {
                SDL_Log("cannot restart worker %d", s);
                sh->done = true;
                running--;
            }
        }
        uint64_t now = SDL_GetTicks();
        if (now - t_report > 15000 && sel.n > 200) {
            uint32_t d = 0;
            for (uint32_t i = 0; i < sel.n; i++) {
                d += res[i].st != ST_NONE;
            }
            SDL_Log("... %u / %u tests, %llu s", d, sel.n, (unsigned long long)((now - t_start) / 1000u));
            t_report = now;
        }
    }
    SDL_free(shards);
    if (skipped) {
        SDL_Log("SKIP: no GPU device");
        free_results(res, sel.n);
        tlist_free(&sel);
        tlist_free(&full);
        return SKIP_CODE;
    }

    /* ---- report */
    expect_load();
    dirstat dirs[64];
    uint32_t ndirs = 0;
    uint32_t unexpected_fail = 0, unexpected_pass = 0, missing = 0, passed = 0, failed = 0;
    for (uint32_t i = 0; i < sel.n; i++) {
        char dn[48];
        dir_of_id(sel.ids[i], dn, sizeof dn);
        dirstat *d = dirstat_get(dirs, &ndirs, dn);
        d->total++;
        result *r = &res[i];
        expect *e = expect_find(sel.ids[i]);
        if (e != nullptr) {
            e->seen = true;
        }
        if (r->st == ST_NONE) {
            missing++;
            SDL_Log("NO RESULT %s", sel.ids[i]);
            continue;
        }
        bool ok = r->st == ST_PASS;
        if (ok) {
            passed++;
            d->pass++;
        } else {
            failed++;
        }
        if (regen != nullptr) {
            continue;
        }
        if (!ok && e != nullptr && (r->st == ST_CRASH || r->st == ST_TIMEOUT)) {
            /* An engine crash or hang can never be an expected failure. */
            d->unexpected_fail++;
            unexpected_fail++;
            SDL_Log("%s %s (listed as %s, but a crash or hang is never expected): %s", r->st == ST_CRASH ? "CRASH" : "TIMEOUT",
                    sel.ids[i], e->reason, r->detail != nullptr ? r->detail : "");
        } else if (!ok && e != nullptr) {
            d->expected_fail++;
            if (strcmp(e->reason, "bug") == 0) {
                d->bug++;
            }
            if (verbose) {
                SDL_Log("expected fail (%s) %s: %s", e->reason, sel.ids[i], r->detail != nullptr ? r->detail : "");
            }
        } else if (!ok && r->st == ST_FAIL && feature_off_id(sel.ids[i])) {
            d->expected_fail++; /* SZ3b: the page uses SVG or canvas and this build has it off */
        } else if (!ok) {
            d->unexpected_fail++;
            unexpected_fail++;
            SDL_Log("FAIL %s: %s", sel.ids[i], r->detail != nullptr ? r->detail : "");
        } else if (e != nullptr && strcmp(e->reason, "platform") != 0 && feature_off_id(sel.ids[i])) {
            /* SZ3b: with the feature off such a page can pass by accident (a blank page against a blank reference). It is
             * reported, not counted. */
            SDL_Log("NOTE: %s passes with SVG or canvas off (listed as %s; not counted)", sel.ids[i], e->reason);
        } else if (e != nullptr && strcmp(e->reason, "platform") != 0) {
            d->unexpected_pass++;
            unexpected_pass++;
            SDL_Log("UNEXPECTED PASS %s (listed as %s)", sel.ids[i], e->reason);
        }
    }

    /* Every failure with its detail, for tools/wpt/classify_reftests.py and for diffing two runs. */
    {
        char path[1100];
        SDL_snprintf(path, sizeof path, "%s/failures.txt", outdir);
        SDL_IOStream *io = SDL_IOFromFile(path, "wb");
        if (io != nullptr) {
            for (uint32_t i = 0; i < sel.n; i++) {
                if (res[i].st != ST_PASS) {
                    SDL_IOprintf(io, "%s\t%s\n", sel.ids[i], res[i].detail != nullptr ? res[i].detail : "");
                }
            }
            SDL_CloseIO(io);
        }
    }

    /* The summary per directory. */
    SDL_Log("%-16s %6s %6s %7s %5s", "directory", "tests", "pass", "pass%", "bug");
    for (uint32_t i = 0; i < ndirs; i++) {
        SDL_Log("%-16s %6u %6u %6.1f%% %5u", dirs[i].name, dirs[i].total, dirs[i].pass,
                dirs[i].total != 0 ? 100.0 * dirs[i].pass / dirs[i].total : 0.0, dirs[i].bug);
    }
    SDL_Log("TOTAL %u tests, %u pass (%.2f%%), %u fail, %.1f s", sel.n, passed, 100.0 * passed / sel.n, failed,
            (double)(SDL_GetTicks() - t_start) / 1000.0);
    uint32_t slow[5] = {0};
    for (uint32_t i = 0; i < sel.n; i++) {
        if (res[i].ms > slow[4]) {
            slow[4] = res[i].ms;
            qsort(slow, 5, sizeof slow[0], cmp_slow);
        }
    }
    SDL_Log("slowest tests (ms): %u %u %u %u %u", slow[0], slow[1], slow[2], slow[3], slow[4]);

    if (regen != nullptr) {
        SDL_CreateDirectory(regen);
        for (uint32_t i = 0; i < ndirs; i++) {
            char path[1024];
            SDL_snprintf(path, sizeof path, "%s/%s.txt", regen, dirs[i].name);
            SDL_IOStream *io = SDL_IOFromFile(path, "wb");
            if (io == nullptr) {
                continue;
            }
            for (uint32_t k = 0; k < sel.n; k++) {
                char dn[48];
                dir_of_id(sel.ids[k], dn, sizeof dn);
                if (strcmp(dn, dirs[i].name) != 0 || res[k].st == ST_PASS || res[k].st == ST_NONE) {
                    continue;
                }
                expect *old = expect_find(sel.ids[k]);
                const char *reason = old != nullptr ? old->reason : guess_reason(sel.ids[k]);
                SDL_IOprintf(io, "%s %s%s%s\n", sel.ids[k], reason, old != nullptr && old->note[0] != '\0' ? " " : "",
                             old != nullptr ? old->note : "");
            }
            SDL_CloseIO(io);
        }
        free_results(res, sel.n);
        tlist_free(&sel);
        tlist_free(&full);
        expect_free();
        return 0;
    }

    /* Bookkeeping checks of the expectation files (a full run checks all of them). */
    bool bad = g_expect_errors > 0 || unexpected_fail > 0 || unexpected_pass > 0 || missing > 0;
    if (all && only == nullptr && filter == nullptr) {
        for (uint32_t i = 0; i < g_nexpect; i++) {
            if (!g_expect[i].seen && !in_set(&sel, g_expect[i].id)) {
                SDL_Log("expectations: %s is not in the test list", g_expect[i].id);
                bad = true;
            }
        }
        for (uint32_t i = 0; i < ndirs; i++) {
            if (dirs[i].bug * 100u > dirs[i].total * BUG_CAP_PERCENT) {
                SDL_Log("expectations: %s has %u bug entries of %u tests (cap %d%%)", dirs[i].name, dirs[i].bug, dirs[i].total,
                        BUG_CAP_PERCENT);
                bad = true;
            }
        }
    }
    SDL_Log("%s: %u unexpected failures, %u unexpected passes, %u without a result", bad ? "FAILED" : "OK", unexpected_fail,
            unexpected_pass, missing);
    free_results(res, sel.n);
    tlist_free(&sel);
    tlist_free(&full);
    expect_free();
    return bad ? 1 : 0;
}
