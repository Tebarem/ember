/*
 * The WPT testharness.js runner (W5f). Every line of tests/wpt/testharness.txt is one test. A
 * test is loaded through the public API (an offscreen view on a host device, a fresh runtime per test) at
 * http://wpt.test/<path>, with a resource handler that serves tests/wpt for that origin and tests/wpt-th/testharnessreport.js
 * for /resources/testharnessreport.js (our own report, which forwards each subtest result and the completion through the
 * console callback). The loop renders the view until the completion arrives or TEST_TIMEOUT_MS passes (scaled under
 * ASan), so timers and promises run as the frames run.
 *
 * Outcomes. A test passes when it completes, the harness status is OK, it has at least one subtest and every subtest
 * passes. Otherwise it fails unless the expectations list it (tests/wpt-th/expectations/<dir>.txt, see the README there):
 *   <path> <reason> [note]                     the whole test is expected not to pass
 *   <path> :: <subtest name> :: <reason> [note] that subtest is expected not to pass
 * A listed test or subtest that passes is an error, so a fixed bug must be deleted from its file.
 *
 * Modes (no argument: the smoke subset, every SMOKE_STRIDE-th test, which is what ctest runs):
 *   --all              every test of the list (also checks that every listed id exists and the bug cap of each directory)
 *   --only <path>      one test, with every subtest printed
 *   --filter <substr>  the tests whose path contains substr
 *   --threads <n>      worker processes (also EMBER_WPT_THREADS); default 2 (4 under ASan)
 *   --verbose          print every subtest of every test
 *   --out <dir>        output directory (also EMBER_WPT_TH_OUT; default <source>/build/tests/wpt-th-out). It receives
 *                      results.txt (one line per test), failures.txt (one line per non-passing subtest, or per test
 *                      that did not complete) and the workers' files.
 * With EMBER_WPT_POISON set, each test runs in the poison mode of the arenas (base.h, embi_arena_debug_poison: memory an
 * arena gives back is poisoned and never reused), and the values in poisoned memory that the restyles read are reported in an
 * L record, "poisoned reads: <n>" (what ASan reports as a use after free once the memory is reused).
 * Exit status: 0 when nothing unexpected happened, 1 on an unexpected failure or an expectation error, 77 without a GPU.
 *
 * Workers: this executable with --worker <ids> <out>, one process per shard (test i goes to shard i % threads). A worker writes
 * one B (begin) record, then S (subtest), L (log) and T (end) records per test, flushed as it goes. A worker that exits or
 * hangs without finishing its test has that test recorded as crash or hang, and its remaining tests run in a new worker.
 *
 * Expectation lines and the reasons are checked when the expectations are loaded.
 */
#define SDL_MAIN_HANDLED
#include <ember/ember.h>
#include <ember/ember_script.h>

#include "base/base.h"
#include "gpu/gpu.h"
#include "emb_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stdlib.h>
#include <string.h>

#define LIST_FILE EMBER_SOURCE_DIR "/tests/wpt/testharness.txt"
#define WPT_ROOT EMBER_SOURCE_DIR "/tests/wpt"
#define REPORT_JS EMBER_SOURCE_DIR "/tests/wpt-th/testharnessreport.js"
#define EXPECT_DIR EMBER_SOURCE_DIR "/tests/wpt-th/expectations"
#define DEFAULT_OUT EMBER_SOURCE_DIR "/build/tests/wpt-th-out"
#define ORIGIN "http://wpt.test"
#define ORIGIN_LEN 15 /* strlen("http://wpt.test") */
#define VIEW_W 800u
#define VIEW_H 600u
#define SMOKE_STRIDE 20u
#define SKIP_CODE 77
#define MAX_RESPAWN 20u
#define ID_MAX 256
#define TEXT_MAX 512
#define BUG_CAP_PERCENT 3u
#define LOG_KEEP 3

/* Under ASan a worker is about 15 times slower and the smoke run is dominated by its slowest shard, so it gets more of them. */
#if EMB_TEST_ASAN
#define DEFAULT_THREADS 4u
#define TEST_TIMEOUT_MS 150000u
#define HANG_TIMEOUT_MS 900000u
#else
#define DEFAULT_THREADS 2u
#define TEST_TIMEOUT_MS 75000u
#define HANG_TIMEOUT_MS 300000u
#endif

static const char *const k_reasons[] = {
    "bug", "not-yet", "out-of-scope", "platform", "layout", "input", "forms", "scripts",
    "needs-http", "harness", "not-applicable",
};

/* ------------------------------------------------------------------ files and strings */

static bool starts_with(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Copies src into dst (cap bytes), replacing control characters (tabs and line breaks would break the record format). */
static void clean_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = c < 32 ? ' ' : (char)c;
    }
    dst[i] = '\0';
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Splits buf (n bytes, which SDL_LoadFile terminates with a NUL) into lines in place; the list holds pointers into buf. */
static char **split_lines(char *buf, size_t n, uint32_t *count)
{
    uint32_t cap = 256, k = 0;
    char **v = SDL_malloc(cap * sizeof *v);
    char *p = buf, *end = buf + n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *next = end;
        if (nl != nullptr) {
            *nl = '\0';
            next = nl + 1;
        } else {
            nl = end;
        }
        size_t len = (size_t)(nl - p);
        if (len > 0 && p[len - 1] == '\r') {
            p[len - 1] = '\0';
        }
        if (k == cap) {
            cap *= 2;
            v = SDL_realloc(v, cap * sizeof *v);
        }
        v[k++] = p;
        p = next;
    }
    *count = k;
    return v;
}

static char *load_text(const char *path, size_t *n)
{
    return SDL_LoadFile(path, n);
}

/* ------------------------------------------------------------------ the test list and expectations */

static char **g_ids; /* the list, sorted */
static uint32_t g_nids;
static char *g_list_buf;

typedef struct expect {
    char id[ID_MAX];
    char sub[TEXT_MAX]; /* empty: the whole test */
    char reason[32];
    char note[TEXT_MAX];
    bool used;
    char where[160]; /* file:line, for messages */
} expect;

static expect *g_exp;
static uint32_t g_nexp, g_capexp;
static uint32_t g_errors;

static void error_msg(const char *fmt, const char *a, const char *b)
{
    SDL_Log("ERROR: %s%s%s", fmt, a ? a : "", b ? b : "");
    g_errors++;
}

static bool reason_known(const char *r)
{
    for (size_t i = 0; i < sizeof k_reasons / sizeof k_reasons[0]; i++) {
        if (strcmp(r, k_reasons[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* One expectation line. Returns false on a malformed line (already reported). */
static void parse_expect_line(char *line, const char *where)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#') {
        return;
    }
    expect e;
    memset(&e, 0, sizeof e);
    SDL_snprintf(e.where, sizeof e.where, "%s", where);
    size_t il = strcspn(p, " \t");
    if (il == 0 || il >= sizeof e.id) {
        error_msg("bad expectation line at ", where, nullptr);
        return;
    }
    memcpy(e.id, p, il);
    e.id[il] = '\0';
    p += il;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (starts_with(p, "::")) {
        p += 2;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char *sep = strstr(p, " :: ");
        if (sep == nullptr) {
            error_msg("subtest expectation without a second ' :: ' at ", where, nullptr);
            return;
        }
        size_t sl = (size_t)(sep - p);
        if (sl == 0 || sl >= sizeof e.sub) {
            error_msg("bad subtest name at ", where, nullptr);
            return;
        }
        memcpy(e.sub, p, sl);
        e.sub[sl] = '\0';
        p = sep + 4;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    size_t rl = strcspn(p, " \t");
    if (rl == 0 || rl >= sizeof e.reason) {
        error_msg("missing reason at ", where, nullptr);
        return;
    }
    memcpy(e.reason, p, rl);
    e.reason[rl] = '\0';
    if (!reason_known(e.reason)) {
        error_msg("unknown reason at ", where, e.reason);
        return;
    }
    p += rl;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    clean_copy(e.note, sizeof e.note, p);
    for (uint32_t i = 0; i < g_nexp; i++) {
        if (strcmp(g_exp[i].id, e.id) == 0 && strcmp(g_exp[i].sub, e.sub) == 0) {
            error_msg("duplicate expectation for ", e.id, nullptr);
            return;
        }
    }
    if (g_nexp == g_capexp) {
        g_capexp = g_capexp ? g_capexp * 2 : 256;
        g_exp = SDL_realloc(g_exp, g_capexp * sizeof *g_exp);
    }
    g_exp[g_nexp++] = e;
}

static SDL_EnumerationResult enum_expect_cb(void *user, const char *dirname, const char *fname)
{
    (void)user;
    if (!ends_with(fname, ".txt")) {
        return SDL_ENUM_CONTINUE;
    }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dirname, fname);
    size_t n = 0;
    char *buf = load_text(path, &n);
    if (buf == nullptr) {
        error_msg("cannot read ", path, nullptr);
        return SDL_ENUM_CONTINUE;
    }
    uint32_t count = 0;
    char **lines = split_lines(buf, n, &count);
    for (uint32_t i = 0; i < count; i++) {
        char where[200];
        SDL_snprintf(where, sizeof where, "%s:%u", fname, i + 1);
        parse_expect_line(lines[i], where);
    }
    SDL_free(lines);
    SDL_free(buf);
    return SDL_ENUM_CONTINUE;
}

static void expect_load(void)
{
    SDL_EnumerateDirectory(EXPECT_DIR, enum_expect_cb, nullptr);
}

static expect *expect_test(const char *id)
{
    for (uint32_t i = 0; i < g_nexp; i++) {
        if (g_exp[i].sub[0] == '\0' && strcmp(g_exp[i].id, id) == 0) {
            return &g_exp[i];
        }
    }
    return nullptr;
}

static expect *expect_sub(const char *id, const char *name)
{
    for (uint32_t i = 0; i < g_nexp; i++) {
        if (g_exp[i].sub[0] != '\0' && strcmp(g_exp[i].id, id) == 0 && strcmp(g_exp[i].sub, name) == 0) {
            return &g_exp[i];
        }
    }
    return nullptr;
}

/* The directory key of a test: "dom-nodes" for dom/nodes/x.html, "html-dom", "uievents", "console", "css-animations". */
static void dir_key(const char *id, char *out, size_t cap)
{
    const char *s1 = strchr(id, '/');
    if (s1 == nullptr) {
        SDL_snprintf(out, cap, "%s", id);
        return;
    }
    size_t l1 = (size_t)(s1 - id);
    if (l1 == 3 && memcmp(id, "css", 3) == 0) { /* W6e: css/css-animations/x.html is "css-animations" */
        const char *s2 = strchr(s1 + 1, '/');
        if (s2 != nullptr) {
            SDL_snprintf(out, cap, "%.*s", (int)(s2 - s1 - 1), s1 + 1);
            return;
        }
    }
    if ((l1 == 3 && memcmp(id, "dom", 3) == 0) || (l1 == 4 && memcmp(id, "html", 4) == 0)) {
        const char *s2 = strchr(s1 + 1, '/');
        if (s2 != nullptr) {
            SDL_snprintf(out, cap, "%.*s-%.*s", (int)l1, id, (int)(s2 - s1 - 1), s1 + 1);
            return;
        }
    }
    SDL_snprintf(out, cap, "%.*s", (int)l1, id);
}

/* ------------------------------------------------------------------ the worker: running tests */

typedef struct env {
    SDL_GPUDevice *dev;
    SDL_IOStream *out;
    /* The current test. */
    bool done;
    int hstatus;
    char hmsg[TEXT_MAX];
    uint32_t nlog;
} env;

static char *g_res_buf; /* the bytes the last handler call returned, kept until the next call */

/* True for a backslash in the path or a ".." path segment (a query, or a file name with dots, is fine). */
static bool bad_path(const char *p, size_t pl)
{
    if (memchr(p, '\\', pl) != nullptr) {
        return true;
    }
    size_t i = 0;
    while (i < pl) {
        size_t j = i;
        while (j < pl && p[j] != '/') {
            j++;
        }
        if (j - i == 2 && p[i] == '.' && p[i + 1] == '.') {
            return true;
        }
        i = j + 1;
    }
    return false;
}

/* The value of a "// META: <key>=<value>" line (WPT's script metadata of a .any.js test), or false. */
static bool meta_line(const char *p, size_t len, const char *key, const char **val, size_t *vlen)
{
    const char *e = p + len;
    if (len < 2 || p[0] != '/' || p[1] != '/') {
        return false;
    }
    p += 2;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if ((size_t)(e - p) < 5 || memcmp(p, "META:", 5) != 0) {
        return false;
    }
    p += 5;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    size_t kl = strlen(key);
    if ((size_t)(e - p) < kl || memcmp(p, key, kl) != 0) {
        return false;
    }
    p += kl;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= e || *p != '=') {
        return false;
    }
    p++;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        e--;
    }
    *val = p;
    *vlen = (size_t)(e - p);
    return true;
}

/* The document WPT serves for foo.any.js as foo.any.html: the harness, our report, the META scripts in order, and the
 * test itself (self_src, root-relative). Returns an SDL_malloc'd buffer of *outn bytes (the caller frees it), or nullptr. */
static char *any_document(const char *js, size_t n, const char *self_src, size_t *outn)
{
    const size_t cap = 4 * n + 512;
    char *out = SDL_malloc(cap);
    if (out == nullptr) {
        return nullptr;
    }
    size_t len = (size_t)SDL_snprintf(out, cap, "%s",
                                      "<!doctype html><script src=\"/resources/testharness.js\"></script>"
                                      "<script src=\"/resources/testharnessreport.js\"></script>");
    const char *p = js, *end = js + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl != nullptr ? nl : end;
        const char *v = nullptr;
        size_t vl = 0;
        if (meta_line(p, (size_t)(le - p), "script", &v, &vl) && len < cap) {
            len += (size_t)SDL_snprintf(out + len, cap - len, "<script src=\"%.*s\"></script>", (int)vl, v);
        }
        p = le + 1;
    }
    len += (size_t)SDL_snprintf(out + len, cap - len, "<script src=\"%s\"></script>", self_src);
    *outn = len;
    return out;
}

/* The media type WPT's server gives a file by its extension. */
static const char *mime_of(const char *p, size_t pl)
{
    static const struct {
        const char *ext;
        const char *mime;
    } k[] = {{".html", "text/html"}, {".htm", "text/html"}, {".js", "text/javascript"}, {".mjs", "text/javascript"},
             {".json", "application/json"}, {".txt", "text/plain"}, {".css", "text/css"}, {".png", "image/png"},
             {".svg", "image/svg+xml"}, {".xml", "text/xml"}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        size_t el = strlen(k[i].ext);
        if (pl >= el && strncmp(p + pl - el, k[i].ext, el) == 0) {
            return k[i].mime;
        }
    }
    return nullptr;
}

#define ASIS_HEADERS_MAX 32
static emb_header g_res_hdrs[ASIS_HEADERS_MAX];

/* A .asis file is a raw HTTP response ("HTTP/1.1 200 OK" line, header lines, a blank line, the body) that WPT's server
 * sends as it is. Splits g_res_buf in place: sets the status, the headers (pointing into the buffer) and the body. */
static void asis_split(char *buf, size_t n, emb_response *out, size_t *body_off)
{
    size_t i = 0;
    out->status = 200;
    /* The status line: "HTTP/x.y NNN reason". */
    size_t eol = i;
    while (eol < n && buf[eol] != '\n') {
        eol++;
    }
    const char *sp = memchr(buf, ' ', eol);
    if (sp != nullptr) {
        out->status = (uint16_t)atoi(sp + 1);
    }
    i = eol < n ? eol + 1 : n;
    size_t nh = 0;
    while (i < n) {
        eol = i;
        while (eol < n && buf[eol] != '\n') {
            eol++;
        }
        size_t le = eol;
        if (le > i && buf[le - 1] == '\r') {
            le--;
        }
        if (le == i) { /* the blank line: the body follows */
            i = eol < n ? eol + 1 : n;
            break;
        }
        const char *colon = memchr(buf + i, ':', le - i);
        if (colon != nullptr && nh < ASIS_HEADERS_MAX) {
            g_res_hdrs[nh++] = (emb_header){buf + i, (size_t)(colon - (buf + i)), colon + 1, (size_t)(buf + le - (colon + 1))};
        }
        i = eol < n ? eol + 1 : n;
    }
    out->headers = g_res_hdrs;
    out->nheaders = nh;
    *body_off = i;
}

/* Maps http://wpt.test/<path> to tests/wpt/<path>; /resources/testharnessreport.js to our report; foo.any.html to the
 * document for foo.any.js. A file that does not exist is a 404 (fetch() resolves, as against a server); a .asis file is
 * served as the raw response it holds. */
static bool res_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    const char *url = req->url;
    if (!starts_with(url, ORIGIN "/")) {
        return false;
    }
    const char *p = url + ORIGIN_LEN;
    size_t pl = strcspn(p, "?#");
    if (bad_path(p, pl) || pl + sizeof WPT_ROOT + 8 >= 1024) {
        return false;
    }
    char path[1024];
    size_t n = 0;
    size_t off = 0;
    SDL_free(g_res_buf);
    g_res_buf = nullptr;
    out->status = 200;
    if (pl == strlen("/resources/testharnessreport.js") && strncmp(p, "/resources/testharnessreport.js", pl) == 0) {
        SDL_snprintf(path, sizeof path, "%s", REPORT_JS);
        g_res_buf = load_text(path, &n);
    } else if (pl > 9 && strncmp(p + pl - 9, ".any.html", 9) == 0) {
        char src[1024];
        SDL_snprintf(src, sizeof src, "%.*s.js", (int)(pl - 5), p);
        SDL_snprintf(path, sizeof path, "%s%s", WPT_ROOT, src);
        char *js = load_text(path, &n);
        if (js != nullptr) {
            g_res_buf = any_document(js, n, src, &n);
            SDL_free(js);
        }
    } else {
        SDL_snprintf(path, sizeof path, "%s%.*s", WPT_ROOT, (int)pl, p);
        g_res_buf = load_text(path, &n);
        if (g_res_buf != nullptr && pl > 5 && strncmp(p + pl - 5, ".asis", 5) == 0) {
            asis_split(g_res_buf, n, out, &off);
            out->data = (const uint8_t *)g_res_buf + off;
            out->len = n - off;
            return true;
        }
    }
    if (g_res_buf == nullptr) {
        out->status = 404;
        out->data = nullptr;
        out->len = 0;
        return true;
    }
    const char *mime = mime_of(p, pl);
    if (mime != nullptr) {
        out->mime = mime;
        out->mime_len = strlen(mime);
    }
    out->data = (const uint8_t *)g_res_buf;
    out->len = n;
    return true;
}

/* Keeps the runtime's warnings and errors (uncaught exceptions, unhandled rejections) as L records. */
static void on_log(void *user, emb_log_level level, const char *message)
{
    env *e = user;
    if (level < EMB_LOG_WARN || e->nlog >= LOG_KEEP || e->out == nullptr) {
        return;
    }
    e->nlog++;
    char line[TEXT_MAX];
    clean_copy(line, sizeof line, message);
    SDL_IOprintf(e->out, "L\t%s\n", line);
    SDL_FlushIO(e->out);
}

/* The console line of the report: "\x01WPT\x1f<kind>\x1f<field>...", see tests/wpt-th/testharnessreport.js. */
static void on_console(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    env *e = user;
    static const char mark[] = "\x01WPT\x1f";
    const size_t ml = sizeof mark - 1;
    if (len <= ml || len >= 4096 || memcmp(text, mark, ml) != 0) {
        return;
    }
    char line[4096];
    memcpy(line, text, len);
    line[len] = '\0';
    char kind = line[ml];
    char *f[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t nf = 0;
    char *p = line + ml + 1;
    if (*p == '\x1f') {
        p++;
    }
    while (nf < 4 && p != nullptr) {
        f[nf++] = p;
        char *s = strchr(p, '\x1f');
        if (s != nullptr) {
            *s = '\0';
            p = s + 1;
        } else {
            p = nullptr;
        }
    }
    static char empty[] = "";
    for (uint32_t i = nf; i < 4; i++) {
        f[i] = empty;
    }
    if (kind == 'R') {
        SDL_IOprintf(e->out, "S\t%d\t%s\t%s\n", atoi(f[0]), f[1], f[2]);
        SDL_FlushIO(e->out);
    } else if (kind == 'H') {
        e->hstatus = atoi(f[0]);
        clean_copy(e->hmsg, sizeof e->hmsg, f[1]);
    } else if (kind == 'D') {
        e->done = true;
    }
}

static bool render_now(SDL_GPUDevice *dev, emb_view *v)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    return changed;
}

/* Runs one test and writes its B, S, L and T records. */
static void run_one(env *e, const char *id)
{
    e->done = false;
    e->hstatus = 0;
    e->hmsg[0] = '\0';
    e->nlog = 0;
    SDL_IOprintf(e->out, "B\t%s\n", id);
    SDL_FlushIO(e->out);
    const size_t live0 = embi_mem_live();
    const bool poison = SDL_getenv("EMBER_WPT_POISON") != nullptr;
    const uint64_t noted0 = embi_arena_debug_poison_noted();
    if (poison) {
        embi_arena_debug_poison(true);
    }

    const char *state = "loadfail";
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = on_log;
    rd.log_user = e;
    emb_runtime *rt = emb_runtime_create(&rd);
    emb_view *view = nullptr;
    if (rt != nullptr) {
        emb_runtime_set_resource_handler_v2(rt, "http", res_handler, nullptr);
        emb_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.size = sizeof vd;
        vd.mode = EMB_VIEW_OFFSCREEN;
        vd.width = VIEW_W;
        vd.height = VIEW_H;
        vd.scale = 1.0f;
        vd.device = e->dev;
        view = emb_view_create(rt, &vd);
    }
    if (view != nullptr) {
        emb_view_set_console_callback(view, on_console, e);
        char url[ID_MAX + sizeof ORIGIN + 2];
        SDL_snprintf(url, sizeof url, "%s/%s", ORIGIN, id);
        if (emb_view_load_url(view, url)) {
            state = "timeout";
            uint64_t t0 = SDL_GetTicks();
            while (!e->done && SDL_GetTicks() - t0 < TEST_TIMEOUT_MS) {
                render_now(e->dev, view);
                if (!e->done) {
                    SDL_Delay(1);
                }
            }
            if (e->done) {
                state = "done";
            }
        }
    }
    if (view != nullptr) {
        emb_view_destroy(view);
    }
    if (rt != nullptr) {
        emb_runtime_destroy(rt);
    }
    SDL_free(g_res_buf);
    g_res_buf = nullptr;
    if (poison) {
        embi_arena_debug_poison(false); /* frees what it kept */
        uint64_t seen = embi_arena_debug_poison_noted() - noted0;
        if (seen > 0) {
            SDL_IOprintf(e->out, "L\tpoisoned reads: %llu\n", (unsigned long long)seen);
            SDL_FlushIO(e->out);
        }
    }
    /* Live memory after the runtime is gone. Interned atoms and the runtime's tables stay on purpose, so growth is only
     * reported (an L record, shown with --verbose), never a failure: the binding tests check exact returns to baseline. */
    const size_t live1 = embi_mem_live();
    if (live1 > live0) {
        SDL_IOprintf(e->out, "L\tlive memory +%llu bytes after the test\n", (unsigned long long)(live1 - live0));
        SDL_FlushIO(e->out);
    }
    SDL_IOprintf(e->out, "T\t%s\t%d\t%s\n", state, e->done ? e->hstatus : 0, e->done ? e->hmsg : "");
    SDL_FlushIO(e->out);
}

/* Runs the ids (one per line of idsfile) into outfile. Returns the exit status of a worker. */
static int run_worker_file(const char *idsfile, const char *outfile)
{
    size_t n = 0;
    char *buf = load_text(idsfile, &n);
    if (buf == nullptr) {
        return 1;
    }
    uint32_t count = 0;
    char **ids = split_lines(buf, n, &count);
    SDL_IOStream *out = SDL_IOFromFile(outfile, "wb");
    if (out == nullptr) {
        SDL_free(ids);
        SDL_free(buf);
        return 1;
    }
    int status = 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        status = SKIP_CODE;
    } else {
        embi_gpu *gpu = embi_gpu_create(nullptr, false);
        if (gpu == nullptr) {
            status = SKIP_CODE;
        } else {
            env e;
            memset(&e, 0, sizeof e);
            e.dev = embi_gpu_device(gpu);
            e.out = out;
            for (uint32_t i = 0; i < count; i++) {
                if (ids[i][0] != '\0') {
                    run_one(&e, ids[i]);
                }
            }
            embi_gpu_destroy(gpu);
        }
    }
    SDL_CloseIO(out);
    SDL_free(ids);
    SDL_free(buf);
    return status;
}

/* ------------------------------------------------------------------ the parent: records and verdicts */

typedef struct outcome {
    bool recorded;
    bool passed;
    bool expected;
    bool unexpected;
    char state[12];
    uint32_t nsubs, npass;
} outcome;

typedef struct sub_rec {
    int status;
    char name[TEXT_MAX];
    char msg[TEXT_MAX];
} sub_rec;

typedef struct test_rec {
    int idx;
    bool open;
    char state[12];
    int hstatus;
    char hmsg[TEXT_MAX];
    sub_rec *subs;
    uint32_t nsubs, capsubs;
    char logs[LOG_KEEP][TEXT_MAX];
    uint32_t nlogs;
} test_rec;

static outcome *g_out;
static bool g_verbose;
static SDL_IOStream *g_results, *g_failures;
static uint32_t g_unexpected;

static void rec_reset(test_rec *t, int idx)
{
    t->idx = idx;
    t->open = idx >= 0;
    t->state[0] = '\0';
    t->hstatus = 0;
    t->hmsg[0] = '\0';
    t->nsubs = 0;
    t->nlogs = 0;
}

static void rec_add_sub(test_rec *t, int status, const char *name, const char *msg)
{
    if (t->nsubs == t->capsubs) {
        t->capsubs = t->capsubs ? t->capsubs * 2 : 64;
        t->subs = SDL_realloc(t->subs, t->capsubs * sizeof *t->subs);
    }
    sub_rec *s = &t->subs[t->nsubs++];
    s->status = status;
    clean_copy(s->name, sizeof s->name, name);
    clean_copy(s->msg, sizeof s->msg, msg);
}

static const char *status_name(int s)
{
    static const char *const names[] = {"PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"};
    return s >= 0 && s < 5 ? names[s] : "UNKNOWN";
}

/* Decides one test (its records are complete) and reports it. */
/* SZ3b: whether the main file of a test (or the .any.js behind an .any.html id) mentions an SVG or canvas feature that this build
 * leaves out. */
static bool feature_off_id(const char *id)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/%s", WPT_ROOT, id);
    if (emb_test_source_needs_disabled_feature(path)) {
        return true;
    }
    size_t n = strlen(path);
    if (n > 9 && strcmp(path + n - 9, ".any.html") == 0) {
        strcpy(path + n - 4, "js");
        return emb_test_source_needs_disabled_feature(path);
    }
    return false;
}

static void verdict(test_rec *t)
{
    const char *id = g_ids[t->idx];
    outcome *o = &g_out[t->idx];
    o->recorded = true;
    SDL_snprintf(o->state, sizeof o->state, "%s", t->state);
    o->nsubs = t->nsubs;
    o->npass = 0;
    for (uint32_t i = 0; i < t->nsubs; i++) {
        if (t->subs[i].status == 0) {
            o->npass++;
        }
    }
    bool completed = strcmp(t->state, "done") == 0 && t->hstatus == 0 && t->nsubs > 0;
    bool passed = completed && o->npass == t->nsubs;
    expect *te = expect_test(id);
    bool unexpected = false;

    /* A listed subtest must fail, and must be reported when the test completed. */
    for (uint32_t i = 0; i < g_nexp; i++) {
        if (g_exp[i].sub[0] == '\0' || strcmp(g_exp[i].id, id) != 0) {
            continue;
        }
        bool found = false;
        for (uint32_t k = 0; k < t->nsubs; k++) {
            if (strcmp(t->subs[k].name, g_exp[i].sub) == 0) {
                found = true;
                g_exp[i].used = true;
                if (t->subs[k].status == 0 && strcmp(g_exp[i].reason, "platform") != 0) { /* platform: either result is accepted */
                    SDL_Log("ERROR: listed subtest passes: %s :: %s (%s)", id, g_exp[i].sub, g_exp[i].where);
                    g_errors++;
                }
                break;
            }
        }
        if (!found && completed) {
            SDL_Log("ERROR: listed subtest not reported: %s :: %s (%s)", id, g_exp[i].sub, g_exp[i].where);
            g_errors++;
        }
    }

    if (passed) {
        o->passed = true;
        if (te != nullptr) {
            te->used = true;
            if (strcmp(te->reason, "platform") != 0) {
                SDL_Log("ERROR: listed test passes: %s (%s, delete the line)", id, te->where);
                g_errors++;
            }
        }
    } else if (te != nullptr) {
        te->used = true;
        o->expected = true;
    } else {
        unexpected = !completed;
        for (uint32_t k = 0; k < t->nsubs && !unexpected; k++) {
            if (t->subs[k].status != 0 && expect_sub(id, t->subs[k].name) == nullptr) {
                unexpected = true;
            }
        }
        if (unexpected && feature_off_id(id)) {
            unexpected = false; /* SZ3b: the page uses SVG or canvas and this build has it off */
        }
        o->expected = !unexpected;
        o->unexpected = unexpected;
    }
    if (unexpected) {
        g_unexpected++;
        SDL_Log("UNEXPECTED %s: %s, harness %d %s, %u of %u subtests pass", id, t->state, t->hstatus, t->hmsg,
               o->npass, o->nsubs);
        for (uint32_t k = 0; k < t->nsubs && k < 3; k++) {
            if (t->subs[k].status != 0) {
                SDL_Log("    %s: %s", status_name(t->subs[k].status), t->subs[k].name);
                if (t->subs[k].msg[0] != '\0') {
                    SDL_Log("        %s", t->subs[k].msg);
                }
            }
        }
    }
    if (g_verbose) {
        SDL_Log("%s %s: %u of %u subtests pass", o->passed ? "PASS" : (o->expected ? "EXPECTED" : "FAIL"), id,
               o->npass, o->nsubs);
        for (uint32_t k = 0; k < t->nsubs; k++) {
            SDL_Log("    %s: %s%s%s", status_name(t->subs[k].status), t->subs[k].name,
                   t->subs[k].msg[0] ? " - " : "", t->subs[k].msg);
        }
    }
    if (g_results != nullptr) {
        SDL_IOprintf(g_results, "%s\t%s\t%u\t%u\t%s\n", id, o->passed ? "pass" : (o->expected ? "expected" : "unexpected"),
                o->npass, o->nsubs, t->state);
    }
    if (g_failures != nullptr) {
        if (strcmp(t->state, "done") != 0 || t->hstatus != 0) {
            SDL_IOprintf(g_failures, "%s\t\t%s\t%d %s\n", id, t->state, t->hstatus, t->hmsg);
        }
        for (uint32_t k = 0; k < t->nsubs; k++) {
            if (t->subs[k].status != 0) {
                SDL_IOprintf(g_failures, "%s\t%s\t%s\t%s\n", id, t->subs[k].name, status_name(t->subs[k].status),
                        t->subs[k].msg);
            }
        }
    }
    for (uint32_t k = 0; g_verbose && k < t->nlogs; k++) {
        SDL_Log("    log: %s", t->logs[k]);
    }
}

static int find_id(const char *id)
{
    char **hit = bsearch(&id, g_ids, g_nids, sizeof *g_ids, cmp_str);
    return hit != nullptr ? (int)(hit - g_ids) : -1;
}

/* Reads one worker's output. killed: the worker was stopped by the parent (a test with no T record is a hang). */
static void absorb(const char *file, bool killed)
{
    size_t n = 0;
    char *buf = load_text(file, &n);
    if (buf == nullptr) {
        return;
    }
    test_rec t;
    memset(&t, 0, sizeof t);
    rec_reset(&t, -1);
    char *p = buf, *end = buf + n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl == nullptr) {
            break; /* a partial line from a killed worker */
        }
        *nl = '\0';
        if (nl > p && nl[-1] == '\r') {
            nl[-1] = '\0';
        }
        char *s = p;
        p = nl + 1;
        if (s[0] == 'B' && s[1] == '\t') {
            if (t.open) {
                SDL_snprintf(t.state, sizeof t.state, "%s", killed ? "hang" : "crash");
                verdict(&t);
            }
            rec_reset(&t, find_id(s + 2));
        } else if (t.open && s[0] == 'S' && s[1] == '\t') {
            char *status = s + 2;
            char *name = strchr(status, '\t');
            char *msg = name ? strchr(name + 1, '\t') : nullptr;
            if (name != nullptr && msg != nullptr) {
                *name++ = '\0';
                *msg++ = '\0';
                rec_add_sub(&t, atoi(status), name, msg);
            }
        } else if (t.open && s[0] == 'L' && s[1] == '\t') {
            if (t.nlogs < LOG_KEEP) {
                clean_copy(t.logs[t.nlogs++], TEXT_MAX, s + 2);
            }
        } else if (t.open && s[0] == 'T' && s[1] == '\t') {
            char *state = s + 2;
            char *hs = strchr(state, '\t');
            if (hs != nullptr) {
                *hs++ = '\0';
                char *hm = strchr(hs, '\t');
                if (hm != nullptr) {
                    *hm++ = '\0';
                    t.hstatus = atoi(hs);
                    clean_copy(t.hmsg, sizeof t.hmsg, hm);
                }
            }
            SDL_snprintf(t.state, sizeof t.state, "%s", state);
            verdict(&t);
            t.open = false;
        }
    }
    if (t.open) {
        SDL_snprintf(t.state, sizeof t.state, "%s", killed ? "hang" : "crash");
        verdict(&t);
    }
    SDL_free(t.subs);
    SDL_free(buf);
}

/* ------------------------------------------------------------------ the parent: workers */

typedef struct shard {
    uint32_t *idx; /* the full-list indices this shard still has to run */
    uint32_t n;
    SDL_Process *proc;
    char outfile[1024];
    uint32_t attempt;
    uint64_t last_size;
    uint64_t last_growth_ms;
} shard;

static bool write_ids(const char *path, const uint32_t *idx, uint32_t n)
{
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (f == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        SDL_IOprintf(f, "%s\n", g_ids[idx[i]]);
    }
    return SDL_CloseIO(f);
}

static bool spawn(const char *exe, const char *outdir, shard *sh, uint32_t k)
{
    char idsfile[1024];
    SDL_snprintf(idsfile, sizeof idsfile, "%s/shard-%u-%u.ids", outdir, k, sh->attempt);
    SDL_snprintf(sh->outfile, sizeof sh->outfile, "%s/shard-%u-%u.out", outdir, k, sh->attempt);
    sh->attempt++;
    if (!write_ids(idsfile, sh->idx, sh->n)) {
        return false;
    }
    const char *args[] = {exe, "--worker", idsfile, sh->outfile, nullptr};
    sh->proc = SDL_CreateProcess(args, false);
    sh->last_size = 0;
    sh->last_growth_ms = SDL_GetTicks();
    return sh->proc != nullptr;
}

/* Runs the selected tests in threads worker processes, respawning a worker that dies or hangs for its remaining tests. */
static bool run_sharded(const char *exe, const char *outdir, const uint32_t *sel, uint32_t nsel, uint32_t threads)
{
    shard *sh = SDL_calloc(threads, sizeof *sh);
    for (uint32_t k = 0; k < threads; k++) {
        sh[k].idx = SDL_malloc((nsel / threads + 1) * sizeof *sh[k].idx);
        for (uint32_t i = k; i < nsel; i += threads) {
            sh[k].idx[sh[k].n++] = sel[i];
        }
        if (!spawn(exe, outdir, &sh[k], k)) {
            SDL_Log("cannot start worker %u: %s", k, SDL_GetError());
            for (uint32_t j = 0; j < k; j++) {
                if (sh[j].proc != nullptr) {
                    SDL_KillProcess(sh[j].proc, true);
                    SDL_DestroyProcess(sh[j].proc);
                }
            }
            return false;
        }
    }
    uint32_t running = threads;
    while (running > 0) {
        SDL_Delay(50);
        for (uint32_t k = 0; k < threads; k++) {
            shard *s = &sh[k];
            if (s->proc == nullptr) {
                continue;
            }
            int code = 0;
            bool exited = SDL_WaitProcess(s->proc, false, &code);
            SDL_PathInfo pi;
            uint64_t size = SDL_GetPathInfo(s->outfile, &pi) ? (uint64_t)pi.size : 0;
            uint64_t now = SDL_GetTicks();
            if (size != s->last_size) {
                s->last_size = size;
                s->last_growth_ms = now;
            }
            bool hung = !exited && now - s->last_growth_ms > HANG_TIMEOUT_MS;
            if (!exited && !hung) {
                continue;
            }
            if (hung) {
                SDL_KillProcess(s->proc, true);
                SDL_WaitProcess(s->proc, true, &code);
            }
            SDL_DestroyProcess(s->proc);
            s->proc = nullptr;
            absorb(s->outfile, hung);
            /* Tests the worker did not record (after the one it died in) run again in a new worker. */
            uint32_t left = 0;
            for (uint32_t i = 0; i < s->n; i++) {
                if (!g_out[s->idx[i]].recorded) {
                    s->idx[left++] = s->idx[i];
                }
            }
            s->n = left;
            if (s->n > 0 && s->attempt < MAX_RESPAWN && spawn(exe, outdir, s, k)) {
                continue;
            }
            for (uint32_t i = 0; i < s->n; i++) {
                outcome *o = &g_out[s->idx[i]];
                o->recorded = true;
                o->unexpected = true;
                SDL_snprintf(o->state, sizeof o->state, "%s", "lost");
                g_unexpected++;
                SDL_Log("UNEXPECTED %s: lost after %u worker attempts", g_ids[s->idx[i]], s->attempt);
            }
            running--;
        }
    }
    for (uint32_t k = 0; k < threads; k++) {
        SDL_free(sh[k].idx);
    }
    SDL_free(sh);
    return true;
}

/* ------------------------------------------------------------------ reporting */

typedef struct dirstat {
    char key[64];
    uint32_t tests, pass, expected, unexpected, subs, subs_pass;
    uint32_t listed, bugs; /* over the whole list (for the bug cap) */
} dirstat;

static dirstat *dir_get(dirstat **v, uint32_t *n, uint32_t *cap, const char *key)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (strcmp((*v)[i].key, key) == 0) {
            return &(*v)[i];
        }
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *v = SDL_realloc(*v, *cap * sizeof **v);
    }
    dirstat *d = &(*v)[(*n)++];
    memset(d, 0, sizeof *d);
    SDL_snprintf(d->key, sizeof d->key, "%s", key);
    return d;
}

static void report(const uint32_t *sel, uint32_t nsel, bool all)
{
    dirstat *v = nullptr;
    uint32_t nv = 0, capv = 0;
    for (uint32_t i = 0; i < g_nids; i++) {
        char key[64];
        dir_key(g_ids[i], key, sizeof key);
        dirstat *d = dir_get(&v, &nv, &capv, key);
        d->listed++;
    }
    for (uint32_t i = 0; i < g_nexp; i++) {
        if (strcmp(g_exp[i].reason, "bug") == 0) {
            char key[64];
            dir_key(g_exp[i].id, key, sizeof key);
            dir_get(&v, &nv, &capv, key)->bugs++;
        }
    }
    uint32_t nrun = 0;
    for (uint32_t j = 0; j < nsel; j++) {
        uint32_t i = sel[j];
        outcome *o = &g_out[i];
        if (!o->recorded) {
            continue;
        }
        nrun++;
        char key[64];
        dir_key(g_ids[i], key, sizeof key);
        dirstat *d = dir_get(&v, &nv, &capv, key);
        d->tests++;
        d->pass += o->passed;
        d->expected += o->expected;
        d->unexpected += o->unexpected;
        d->subs += o->nsubs;
        d->subs_pass += o->npass;
    }
    SDL_Log("\n%-28s %6s %6s %8s %10s %12s", "directory", "tests", "pass", "expected", "unexpected", "subtests");
    dirstat tot;
    memset(&tot, 0, sizeof tot);
    for (uint32_t i = 0; i < nv; i++) {
        dirstat *d = &v[i];
        if (d->tests == 0) {
            continue;
        }
        SDL_Log("%-28s %6u %6u %8u %10u %6u/%-5u", d->key, d->tests, d->pass, d->expected, d->unexpected,
               d->subs_pass, d->subs);
        tot.tests += d->tests;
        tot.pass += d->pass;
        tot.expected += d->expected;
        tot.unexpected += d->unexpected;
        tot.subs += d->subs;
        tot.subs_pass += d->subs_pass;
    }
    SDL_Log("%-28s %6u %6u %8u %10u %6u/%-5u", "total", tot.tests, tot.pass, tot.expected, tot.unexpected,
           tot.subs_pass, tot.subs);
    SDL_Log("%u tests run of %u selected, %u errors", nrun, nsel, g_errors);
    if (all) {
        /* The bug cap (as W4's reftests): at most BUG_CAP_PERCENT of a directory's tests may be bug entries, whole
         * tests and subtests alike. */
        for (uint32_t i = 0; i < nv; i++) {
            dirstat *d = &v[i];
            if (d->bugs * 100u > BUG_CAP_PERCENT * d->listed) {
                SDL_Log("ERROR: %s has %u bug entries of %u tests (cap %u%%)", d->key, d->bugs, d->listed,
                        BUG_CAP_PERCENT);
                g_errors++;
            }
        }
        for (uint32_t i = 0; i < g_nexp; i++) {
            if (!g_exp[i].used) {
                SDL_Log("ERROR: expectation not used (not in the list or not run): %s %s (%s)", g_exp[i].id,
                        g_exp[i].sub, g_exp[i].where);
                g_errors++;
            }
        }
    }
    SDL_free(v);
}

static int usage(void)
{
    SDL_Log("usage: test_bindings_wpt_th [--all | --only <path> | --filter <substr>] [--threads <n>] "
            "[--verbose] [--out <dir>]");
    return 2;
}

static uint32_t *g_sel;

static int main_body(int argc, char **argv)
{
    bool all = false;
    const char *only = nullptr, *filter = nullptr, *out_arg = nullptr;
    int threads = 0;
    const char *tenv = SDL_getenv("EMBER_WPT_THREADS");
    if (tenv != nullptr) {
        threads = atoi(tenv);
    }
    const char *oenv = SDL_getenv("EMBER_WPT_TH_OUT");
    if (oenv != nullptr && oenv[0] != '\0') {
        out_arg = oenv;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            all = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_arg = argv[++i];
        } else {
            return usage();
        }
    }
    if (threads <= 0) {
        threads = (int)DEFAULT_THREADS;
    }
    const char *outdir = out_arg != nullptr ? out_arg : DEFAULT_OUT;

    size_t n = 0;
    g_list_buf = load_text(LIST_FILE, &n);
    if (g_list_buf == nullptr) {
        SDL_Log("cannot read %s", LIST_FILE);
        return 1;
    }
    g_ids = split_lines(g_list_buf, n, &g_nids);
    uint32_t w = 0;
    for (uint32_t i = 0; i < g_nids; i++) {
        if (g_ids[i][0] != '\0') {
            g_ids[w++] = g_ids[i];
        }
    }
    g_nids = w;
    qsort(g_ids, g_nids, sizeof *g_ids, cmp_str);
    expect_load();
    if (g_errors > 0) {
        return 1;
    }

    uint32_t *sel = SDL_malloc((g_nids + 1) * sizeof *sel);
    g_sel = sel;
    uint32_t nsel = 0;
    for (uint32_t i = 0; i < g_nids; i++) {
        bool take;
        if (only != nullptr) {
            take = strcmp(g_ids[i], only) == 0;
        } else if (filter != nullptr) {
            take = strstr(g_ids[i], filter) != nullptr;
        } else if (all) {
            take = true;
        } else {
            take = i % SMOKE_STRIDE == 0;
        }
        if (take) {
            sel[nsel++] = i;
        }
    }
    if (only != nullptr && nsel == 0) {
        SDL_Log("no such test in the list: %s", only);
        return 1;
    }
    if (nsel == 0) {
        SDL_Log("no tests selected");
        return 0;
    }
    g_verbose = g_verbose || only != nullptr;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SKIP_CODE;
    }
    embi_gpu *probe = embi_gpu_create(nullptr, false);
    if (probe == nullptr) {
        SDL_Log("no GPU device: skipped");
        return SKIP_CODE;
    }
    embi_gpu_destroy(probe);

    SDL_CreateDirectory(outdir);
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/results.txt", outdir);
    g_results = SDL_IOFromFile(path, "wb");
    SDL_snprintf(path, sizeof path, "%s/failures.txt", outdir);
    g_failures = SDL_IOFromFile(path, "wb");
    g_out = SDL_calloc(g_nids, sizeof *g_out);

    if ((uint32_t)threads > nsel) {
        threads = (int)nsel;
    }
    bool ok = true;
    if (threads <= 1) {
        char idsfile[1024], outfile[1024];
        SDL_snprintf(idsfile, sizeof idsfile, "%s/inline.ids", outdir);
        SDL_snprintf(outfile, sizeof outfile, "%s/inline.out", outdir);
        ok = write_ids(idsfile, sel, nsel);
        if (ok) {
            int rc = run_worker_file(idsfile, outfile);
            if (rc == SKIP_CODE) {
                return SKIP_CODE;
            }
            absorb(outfile, false);
        }
        uint32_t lost = 0;
        for (uint32_t j = 0; j < nsel; j++) {
            if (!g_out[sel[j]].recorded) {
                lost++;
            }
        }
        if (lost > 0) {
            g_errors += lost;
            SDL_Log("%u tests did not run", lost);
        }
    } else {
        ok = run_sharded(argv[0], outdir, sel, nsel, (uint32_t)threads);
    }
    if (g_results != nullptr) {
        SDL_CloseIO(g_results);
    }
    if (g_failures != nullptr) {
        SDL_CloseIO(g_failures);
    }
    if (!ok) {
        return 1;
    }
    report(sel, nsel, all);
    if (g_unexpected > 0 || g_errors > 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "--worker") == 0) {
        return run_worker_file(argv[2], argv[3]);
    }
    int rc = main_body(argc, argv);
    /* every exit path of main_body ends here */
    SDL_free(g_sel);
    SDL_free(g_out);
    SDL_free(g_exp);
    SDL_free(g_ids);
    SDL_free(g_list_buf);
    return rc;
}
