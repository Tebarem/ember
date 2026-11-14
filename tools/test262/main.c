/*
 * test262 runner: runs the suite against the engine and compares every test variant with
 * tests/test262/expectations.txt.
 *
 *   test262                     full run of test/language and test/built-ins
 *   test262 --smoke             the fixed sample in tests/test262/smoke-list.txt (ctest runs this)
 *   test262 --list FILE         the tests named in FILE (paths relative to the test262 root)
 *   test262 --filter PATH       one file or directory under the test262 root
 *   test262 --update            rewrite expectations.txt for the tests this run covered
 *   --threads N  --root DIR  --expectations FILE  --unsupported FILE  --verbose
 *   --gc-stress N  collect before every N-th allocation (J5h; debug builds also verify the heap)
 *   --generational  sticky marks and minor collections on in every runtime (J5h)
 *   --rope-min N  concatenations of N code units or more build ropes (J5g; 1 = every concat)
 *
 * Watchdog (J6b): the engine's interrupt stops a variant that runs JavaScript past its deadline,
 * but not one stuck in native code. The main thread watches every worker, and a variant that
 * makes no progress for T262_HANG_MS is reported as a timeout ("hung"); the run then reports
 * and exits without waiting for that thread.
 *
 * Exit status: 0 when every variant matches the expectations. 1 for a new failure, an unexpected
 * pass, or an expectation for a variant that no longer runs. 77 when the test262 checkout is
 * missing (ctest reports that as skipped).
 */
#include "test262.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <stdlib.h>

#ifndef EMBER_SOURCE_DIR
#error "EMBER_SOURCE_DIR must name the source root"
#endif

#define PATH_BUF 2048
#define MAX_THREADS 64
#define EXIT_SKIPPED 77

static const char *const default_dirs[] = {"test/language", "test/built-ins"};

typedef struct test_entry {
    char *rel;
    uint8_t state[2];
    bool expected[2]; /* expectations.txt lists this variant as failing */
    bool flaky[2];    /* the expectation is marked flaky: either outcome is accepted */
    char reason[2][T262_REASON_MAX];
} test_entry;

typedef struct str_list {
    char **items;
    int count;
    int cap;
} str_list;

typedef struct exp_entry {
    char *path;
    int variant;
    bool flaky;
} exp_entry;

typedef struct exp_list {
    exp_entry *items;
    int count;
    int cap;
} exp_list;

typedef struct dir_stat {
    char key[128];
    int tests;
    int pass;
    int fail;
    int timeout;
    int skipped;
} dir_stat;

typedef struct dir_list {
    dir_stat *items;
    int count;
    int cap;
} dir_list;

typedef struct options {
    const char *root;
    const char *expectations;
    const char *unsupported;
    const char *list_file;
    const char *filter;
    int threads;
    bool update;
    bool verbose;
    uint32_t gc_stress;   /* --gc-stress N: collect before every N-th allocation */
    bool generational;    /* --generational: sticky marks and minor collections */
    uint32_t rope_min;    /* --rope-min N: concatenations this long or longer make ropes (J5g) */
} options;

typedef struct shared {
    const char *root;
    test_entry *tests;
    int count;
    SDL_AtomicInt next;
    const str_list *unsupported;
} shared;

/* What one worker thread is doing, for the watchdog. */
typedef struct worker_slot {
    shared *s;
    SDL_AtomicInt test;    /* index + 1 of the test it runs, 0 between tests */
    SDL_AtomicInt variant; /* the variant it runs */
    SDL_AtomicInt since;   /* ms after the run start when that variant started */
    SDL_AtomicInt done;    /* 1 once the worker has returned */
    uint64_t t0;
    bool hung;             /* the watchdog gave up on it */
} worker_slot;

/* A variant that runs this long has missed the engine deadline by far: its thread is stuck in
 * native code. */
#define T262_HANG_MS (3u * t262_timeout_ms())

typedef struct collector {
    const char *root;
    const char *rel;
    const char *full;
    str_list *out;
} collector;

/* ------------------------------------------------------------------ growable arrays */

static void out_of_memory(void)
{
    SDL_Log("test262: out of memory");
    exit(2);
}

static void *grow(void *items, int *cap, size_t elem)
{
    int nc = *cap > 0 ? *cap * 2 : 64;
    void *n = SDL_realloc(items, elem * (size_t)nc);
    if (n == nullptr) {
        out_of_memory();
    }
    *cap = nc;
    return n;
}

static void str_list_push(str_list *l, char *s)
{
    if (l->count == l->cap) {
        l->items = grow(l->items, &l->cap, sizeof *l->items);
    }
    l->items[l->count++] = s;
}

static void exp_list_push(exp_list *l, const char *path, int variant, bool flaky)
{
    if (l->count == l->cap) {
        l->items = grow(l->items, &l->cap, sizeof *l->items);
    }
    l->items[l->count].path = SDL_strdup(path);
    l->items[l->count].variant = variant;
    l->items[l->count].flaky = flaky;
    l->count++;
}

/* ------------------------------------------------------------------ paths and files */

static bool join_path(char *dst, size_t cap, const char *a, const char *b)
{
    int n = SDL_snprintf(dst, cap, "%s/%s", a, b);
    return n >= 0 && (size_t)n < cap;
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t n = SDL_strlen(s);
    size_t m = SDL_strlen(suffix);
    return n >= m && SDL_strcmp(s + (n - m), suffix) == 0;
}

static bool is_test_file(const char *name)
{
    return has_suffix(name, ".js") && !has_suffix(name, "_FIXTURE.js");
}

static SDL_EnumerationResult SDLCALL enum_cb(void *ud, const char *dirname, const char *fname);

static void collect_dir(const char *root, const char *rel, str_list *out)
{
    char full[PATH_BUF];
    if (!join_path(full, sizeof full, root, rel)) {
        return;
    }
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full, &info)) {
        return;
    }
    if (info.type == SDL_PATHTYPE_FILE) {
        if (is_test_file(rel)) {
            str_list_push(out, SDL_strdup(rel));
        }
        return;
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY) {
        return;
    }
    collector c = {root, rel, full, out};
    SDL_EnumerateDirectory(full, enum_cb, &c);
}

static SDL_EnumerationResult SDLCALL enum_cb(void *ud, const char *dirname, const char *fname)
{
    (void)dirname;
    collector *c = ud;
    if (SDL_strcmp(fname, ".") == 0 || SDL_strcmp(fname, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    char child_rel[PATH_BUF];
    if (SDL_snprintf(child_rel, sizeof child_rel, "%s/%s", c->rel, fname) >=
        (int)sizeof child_rel) {
        return SDL_ENUM_CONTINUE;
    }
    if (has_suffix(fname, ".js")) {
        if (is_test_file(fname)) {
            str_list_push(c->out, SDL_strdup(child_rel));
        }
        return SDL_ENUM_CONTINUE;
    }
    char child_full[PATH_BUF];
    if (join_path(child_full, sizeof child_full, c->full, fname)) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(child_full, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            collect_dir(c->root, child_rel, c->out);
        }
    }
    return SDL_ENUM_CONTINUE;
}

/* Calls fn for every non-blank line that does not start with '#'. */
typedef void (*line_fn)(const char *line, size_t len, void *user);

static void for_each_line(const char *text, size_t len, line_fn fn, void *user)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) {
            start++;
        }
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
            end--;
        }
        if (end > start && text[start] != '#') {
            fn(text + start, end - start, user);
        }
        i++;
    }
}

static void push_first_token(const char *line, size_t len, void *user)
{
    size_t n = 0;
    while (n < len && line[n] != ' ' && line[n] != '\t') {
        n++;
    }
    char *tok = SDL_malloc(n + 1);
    if (tok == nullptr) {
        out_of_memory();
    }
    SDL_memcpy(tok, line, n);
    tok[n] = '\0';
    str_list_push((str_list *)user, tok);
}

/* Reads a list file: the first token of each line. A missing file gives an empty list. */
static void read_token_list(const char *path, str_list *out, bool required)
{
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    if (text == nullptr) {
        if (required) {
            SDL_Log("test262: cannot read %s", path);
        }
        return;
    }
    for_each_line(text, len, push_first_token, out);
    SDL_free(text);
}

typedef struct exp_ctx {
    exp_list *list;
    const char *path;
    int bad;
} exp_ctx;

static void push_expectation(const char *line, size_t len, void *user)
{
    exp_ctx *c = user;
    size_t n = 0;
    while (n < len && line[n] != ' ' && line[n] != '\t') {
        n++;
    }
    size_t m = n;
    while (m < len && (line[m] == ' ' || line[m] == '\t')) {
        m++;
    }
    size_t e = m;
    while (e < len && line[e] != ' ' && line[e] != '\t') {
        e++;
    }
    size_t mode_len = e - m;
    size_t f = e;
    while (f < len && (line[f] == ' ' || line[f] == '\t')) {
        f++;
    }
    bool flaky = len - f == 5 && SDL_memcmp(line + f, "flaky", 5) == 0;
    int variant = -1;
    if (mode_len == 6 && SDL_memcmp(line + m, "sloppy", 6) == 0) {
        variant = T262_SLOPPY;
    } else if (mode_len == 6 && SDL_memcmp(line + m, "strict", 6) == 0) {
        variant = T262_STRICT;
    }
    if (n == 0 || variant < 0 || (f < len && !flaky)) {
        SDL_Log("test262: %s: bad expectation line (want '<path> sloppy|strict [flaky]')", c->path);
        c->bad++;
        return;
    }
    char *path = SDL_malloc(n + 1);
    if (path == nullptr) {
        out_of_memory();
    }
    SDL_memcpy(path, line, n);
    path[n] = '\0';
    exp_list_push(c->list, path, variant, flaky);
    SDL_free(path);
}

/* Returns the number of malformed lines. A missing file is not an error: it means no expectations. */
static int read_expectations(const char *path, exp_list *out)
{
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    if (text == nullptr) {
        return 0;
    }
    exp_ctx c = {out, path, 0};
    for_each_line(text, len, push_expectation, &c);
    SDL_free(text);
    return c.bad;
}

/* ------------------------------------------------------------------ ordering */

static int SDLCALL cmp_str_ptr(const void *a, const void *b)
{
    return SDL_strcmp(*(char *const *)a, *(char *const *)b);
}

static int SDLCALL cmp_test_key(const void *key, const void *elem)
{
    return SDL_strcmp((const char *)key, ((const test_entry *)elem)->rel);
}

static int SDLCALL cmp_exp_entry(const void *a, const void *b)
{
    const exp_entry *x = a;
    const exp_entry *y = b;
    int c = SDL_strcmp(x->path, y->path);
    if (c != 0) {
        return c;
    }
    return x->variant - y->variant;
}

/* ------------------------------------------------------------------ skipping */

static const char *skip_reason(const shared *s, const t262_meta *m, char *buf, size_t cap)
{
    if (m->overflow) {
        return "frontmatter list too long for the runner";
    }
    /* Atomics.wait must never block the engine thread, and there are no $262.agent threads. */
    if (m->flags & T262_FLAG_CAN_BLOCK) {
        return "needs a thread that may block (CanBlockIsTrue)";
    }
    for (int i = 0; i < m->n_includes; i++) {
        if (SDL_strcmp(m->includes[i], "atomicsHelper.js") == 0) {
            return "agent-dependent ($262.agent is not provided)";
        }
    }
    for (int i = 0; i < m->n_features; i++) {
        for (int k = 0; k < s->unsupported->count; k++) {
            if (SDL_strcmp(m->features[i], s->unsupported->items[k]) == 0) {
                SDL_snprintf(buf, cap, "unsupported feature %s", m->features[i]);
                return buf;
            }
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ workers */

static void set_all(test_entry *t, t262_outcome o, const char *reason)
{
    for (int v = 0; v < 2; v++) {
        t->state[v] = (uint8_t)o;
        SDL_snprintf(t->reason[v], sizeof t->reason[v], "%s", reason);
    }
}

static void process_test(const shared *s, test_entry *t, worker_slot *w)
{
    char path[PATH_BUF];
    if (!join_path(path, sizeof path, s->root, t->rel)) {
        set_all(t, T262_FAIL, "path too long");
        return;
    }
    size_t len = 0;
    char *src = SDL_LoadFile(path, &len);
    if (src == nullptr) {
        set_all(t, T262_FAIL, "cannot read file");
        return;
    }

    t262_meta meta;
    t262_parse_meta(src, len, &meta);
    char buf[T262_REASON_MAX];
    const char *skip = skip_reason(s, &meta, buf, sizeof buf);

    for (int v = 0; v < 2; v++) {
        bool strict = v == T262_STRICT;
        bool applies;
        if ((meta.flags & T262_FLAG_MODULE) != 0) {
            /* Module code is always strict: the module runs once, in the strict variant. */
            applies = strict;
        } else if ((meta.flags & T262_FLAG_ONLY_STRICT) != 0) {
            applies = strict;
        } else if ((meta.flags & (T262_FLAG_NO_STRICT | T262_FLAG_RAW)) != 0) {
            applies = !strict;
        } else {
            applies = true;
        }
        if (!applies) {
            t->state[v] = T262_NONE;
            continue;
        }
        if (skip != nullptr) {
            t->state[v] = T262_SKIP;
            SDL_snprintf(t->reason[v], sizeof t->reason[v], "%s", skip);
            continue;
        }
        if (w != nullptr) {
            SDL_SetAtomicInt(&w->variant, v);
            SDL_SetAtomicInt(&w->since, (int)(SDL_GetTicks() - w->t0));
        }
        t->state[v] = (uint8_t)t262_run(s->root, t->rel, src, len, &meta, strict, t->reason[v],
                                       sizeof t->reason[v]);
    }
    SDL_free(src);
}

static int SDLCALL worker_main(void *arg)
{
    worker_slot *w = arg;
    shared *s = w->s;
    for (;;) {
        int i = SDL_AddAtomicInt(&s->next, 1);
        if (i >= s->count) {
            break;
        }
        SDL_SetAtomicInt(&w->since, (int)(SDL_GetTicks() - w->t0));
        SDL_SetAtomicInt(&w->test, i + 1);
        process_test(s, &s->tests[i], w);
        SDL_SetAtomicInt(&w->test, 0);
    }
    SDL_SetAtomicInt(&w->done, 1);
    return 0;
}

/* ------------------------------------------------------------------ reporting */

static void dir_key(const char *rel, char *key, size_t cap)
{
    const char *p = rel;
    if (SDL_strncmp(p, "test/", 5) == 0) {
        p += 5;
    }
    const char *slash = SDL_strchr(p, '/');
    size_t n1 = slash != nullptr ? (size_t)(slash - p) : SDL_strlen(p);
    if (slash != nullptr && n1 == 9 && SDL_strncmp(p, "built-ins", 9) == 0) {
        const char *second = slash + 1;
        const char *s2 = SDL_strchr(second, '/');
        size_t n2 = s2 != nullptr ? (size_t)(s2 - second) : SDL_strlen(second);
        SDL_snprintf(key, cap, "built-ins/%.*s", (int)n2, second);
    } else {
        SDL_snprintf(key, cap, "%.*s", (int)n1, p);
    }
}

static dir_stat *dir_for(dir_list *dl, const char *key)
{
    for (int i = 0; i < dl->count; i++) {
        if (SDL_strcmp(dl->items[i].key, key) == 0) {
            return &dl->items[i];
        }
    }
    if (dl->count == dl->cap) {
        dl->items = grow(dl->items, &dl->cap, sizeof *dl->items);
    }
    dir_stat *d = &dl->items[dl->count++];
    SDL_memset(d, 0, sizeof *d);
    SDL_snprintf(d->key, sizeof d->key, "%s", key);
    return d;
}

static bool write_expectations(const char *path, const exp_list *list)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == nullptr) {
        SDL_Log("test262: cannot write %s: %s", path, SDL_GetError());
        return false;
    }
    SDL_IOprintf(io, "# test262 expectations: the variants that the JS engine is known to fail.\n");
    SDL_IOprintf(io, "# Format: <path relative to the test262 root> sloppy|strict [flaky]\n");
    SDL_IOprintf(io, "# Regenerate with: test262 --update (after a full run). Keep the flaky marker\n");
    SDL_IOprintf(io, "# on entries whose outcome varies between runs (engine nondeterminism): either\n");
    SDL_IOprintf(io, "# outcome is accepted for those variants.\n");
    for (int i = 0; i < list->count; i++) {
        if (i > 0 && SDL_strcmp(list->items[i].path, list->items[i - 1].path) == 0 &&
            list->items[i].variant == list->items[i - 1].variant) {
            continue;
        }
        SDL_IOprintf(io, "%s %s%s\n", list->items[i].path,
                     list->items[i].variant == T262_STRICT ? "strict" : "sloppy",
                     list->items[i].flaky ? " flaky" : "");
    }
    return SDL_CloseIO(io);
}

static const char *variant_name(int v)
{
    return v == T262_STRICT ? "strict" : "sloppy";
}

/* ------------------------------------------------------------------ driver */

static const char *next_arg(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        return nullptr;
    }
    *i += 1;
    return argv[*i];
}

static bool parse_args(int argc, char **argv, options *o, const char *smoke_list)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = nullptr;
        if (SDL_strcmp(a, "--update") == 0) {
            o->update = true;
        } else if (SDL_strcmp(a, "--verbose") == 0) {
            o->verbose = true;
        } else if (SDL_strcmp(a, "--smoke") == 0) {
            o->list_file = smoke_list;
        } else if (SDL_strcmp(a, "--generational") == 0) {
            o->generational = true;
        } else if (SDL_strcmp(a, "--root") == 0 || SDL_strcmp(a, "--list") == 0 ||
                   SDL_strcmp(a, "--filter") == 0 || SDL_strcmp(a, "--threads") == 0 ||
                   SDL_strcmp(a, "--expectations") == 0 || SDL_strcmp(a, "--unsupported") == 0 ||
                   SDL_strcmp(a, "--gc-stress") == 0 || SDL_strcmp(a, "--rope-min") == 0) {
            val = next_arg(argc, argv, &i);
            if (val == nullptr) {
                SDL_Log("test262: %s needs a value", a);
                return false;
            }
            if (SDL_strcmp(a, "--root") == 0) {
                o->root = val;
            } else if (SDL_strcmp(a, "--list") == 0) {
                o->list_file = val;
            } else if (SDL_strcmp(a, "--filter") == 0) {
                o->filter = val;
            } else if (SDL_strcmp(a, "--threads") == 0) {
                o->threads = SDL_atoi(val);
            } else if (SDL_strcmp(a, "--gc-stress") == 0) {
                int n = SDL_atoi(val);
                o->gc_stress = n > 0 ? (uint32_t)n : 0u;
            } else if (SDL_strcmp(a, "--rope-min") == 0) {
                int n = SDL_atoi(val);
                o->rope_min = n > 0 ? (uint32_t)n : 0u;
            } else if (SDL_strcmp(a, "--expectations") == 0) {
                o->expectations = val;
            } else {
                o->unsupported = val;
            }
        } else {
            SDL_Log("test262: unknown argument %s", a);
            SDL_Log("usage: test262 [--smoke | --list FILE | --filter PATH] [--update] "
                    "[--threads N] [--root DIR] [--expectations FILE] [--unsupported FILE] "
                    "[--gc-stress N] [--generational] [--rope-min N] [--verbose]");
            return false;
        }
    }
    return true;
}

static test_entry *build_tests(const options *o, int *count)
{
    str_list paths = {nullptr, 0, 0};
    if (o->list_file != nullptr) {
        read_token_list(o->list_file, &paths, true);
    } else if (o->filter != nullptr) {
        /* A trailing slash is dropped: "dir/" must yield the same rel paths as "dir", or they
         * would not match the expectations keys. An empty path selects nothing. */
        char *rel = SDL_strdup(o->filter);
        if (rel == nullptr) {
            out_of_memory();
        }
        size_t n = SDL_strlen(rel);
        while (n > 0 && rel[n - 1] == '/') {
            rel[--n] = '\0';
        }
        if (n > 0) {
            collect_dir(o->root, rel, &paths);
        }
        SDL_free(rel);
    } else {
        for (size_t i = 0; i < sizeof default_dirs / sizeof default_dirs[0]; i++) {
            collect_dir(o->root, default_dirs[i], &paths);
        }
    }
    if (paths.count > 1) {
        SDL_qsort(paths.items, (size_t)paths.count, sizeof *paths.items, cmp_str_ptr);
    }
    /* Drop duplicates (a path can be listed twice, or reached by two filters). */
    int n = 0;
    for (int i = 0; i < paths.count; i++) {
        if (n > 0 && SDL_strcmp(paths.items[n - 1], paths.items[i]) == 0) {
            SDL_free(paths.items[i]);
            continue;
        }
        paths.items[n++] = paths.items[i];
    }
    test_entry *tests = SDL_calloc((size_t)n + 1u, sizeof *tests);
    if (tests == nullptr) {
        out_of_memory();
    }
    for (int i = 0; i < n; i++) {
        tests[i].rel = paths.items[i];
    }
    SDL_free(paths.items);
    *count = n;
    return tests;
}

static void mark_expected(test_entry *tests, int count, const exp_list *exp)
{
    for (int i = 0; i < exp->count; i++) {
        test_entry *t = SDL_bsearch(exp->items[i].path, tests, (size_t)count, sizeof *tests,
                                    cmp_test_key);
        if (t != nullptr) {
            t->expected[exp->items[i].variant] = true;
            t->flaky[exp->items[i].variant] = exp->items[i].flaky;
        }
    }
}

/* Writes the expectations: entries for tests this run did not cover are kept, and the covered
 * tests contribute their current failing variants. */
static bool update_expectations(const options *o, const test_entry *tests, int count,
                                const exp_list *old)
{
    exp_list next = {nullptr, 0, 0};
    for (int i = 0; i < old->count; i++) {
        if (SDL_bsearch(old->items[i].path, tests, (size_t)count, sizeof *tests, cmp_test_key) ==
            nullptr) {
            exp_list_push(&next, old->items[i].path, old->items[i].variant, old->items[i].flaky);
        }
    }
    for (int i = 0; i < count; i++) {
        for (int v = 0; v < 2; v++) {
            bool failing = tests[i].state[v] == T262_FAIL || tests[i].state[v] == T262_TIMEOUT;
            /* Flaky entries are kept whatever this run saw: their outcome is not stable. */
            if (failing || (tests[i].flaky[v] && tests[i].expected[v])) {
                exp_list_push(&next, tests[i].rel, v, tests[i].flaky[v]);
            }
        }
    }
    if (next.count > 1) {
        SDL_qsort(next.items, (size_t)next.count, sizeof *next.items, cmp_exp_entry);
    }
    bool ok = write_expectations(o->expectations, &next);
    if (ok) {
        SDL_Log("test262: wrote %d expectation entries to %s", next.count, o->expectations);
    }
    for (int i = 0; i < next.count; i++) {
        SDL_free(next.items[i].path);
    }
    SDL_free(next.items);
    return ok;
}

/* Prints the per-variant results and returns the number of problems found. */
static int report(const options *o, test_entry *tests, int count, dir_list *dirs, int *n_pass,
                  int *n_fail, int *n_timeout)
{
    int problems = 0;
    for (int i = 0; i < count; i++) {
        test_entry *t = &tests[i];
        char key[128];
        dir_key(t->rel, key, sizeof key);
        dir_stat *d = dir_for(dirs, key);
        d->tests++;
        if (t->state[0] == T262_SKIP || t->state[1] == T262_SKIP) {
            d->skipped++;
        }
        for (int v = 0; v < 2; v++) {
            t262_outcome st = (t262_outcome)t->state[v];
            bool failing = st == T262_FAIL || st == T262_TIMEOUT;
            switch (st) {
            case T262_PASS:
                d->pass++;
                (*n_pass)++;
                break;
            case T262_FAIL:
                d->fail++;
                (*n_fail)++;
                break;
            case T262_TIMEOUT:
                d->timeout++;
                (*n_timeout)++;
                break;
            default:
                break;
            }
            if (st != T262_PASS && st != T262_FAIL && st != T262_TIMEOUT) {
                if (t->expected[v]) {
                    SDL_Log("STALE %s [%s]: no longer runs (%s)", t->rel, variant_name(v),
                            st == T262_SKIP ? t->reason[v] : "not applicable");
                    problems++;
                }
                continue;
            }
            if (failing && !t->expected[v]) {
                SDL_Log("NEW FAILURE %s [%s]: %s", t->rel, variant_name(v), t->reason[v]);
                problems++;
            } else if (!failing && t->expected[v] && !t->flaky[v]) {
                SDL_Log("UNEXPECTED PASS %s [%s]: remove it from the expectations", t->rel,
                        variant_name(v));
                problems++;
            } else if (failing && o->verbose) {
                SDL_Log("fail %s [%s]: %s", t->rel, variant_name(v), t->reason[v]);
            }
        }
    }
    return problems;
}

static void print_summary(const dir_list *dirs, int tests, int n_pass, int n_fail, int n_timeout,
                          uint64_t elapsed_ms, int threads)
{
    SDL_Log("%-34s %8s %8s %8s %8s %8s %8s", "directory", "pass", "run", "pass%", "fail",
            "timeout", "skipped");
    for (int i = 0; i < dirs->count; i++) {
        const dir_stat *d = &dirs->items[i];
        int run = d->pass + d->fail + d->timeout;
        double pct = run > 0 ? 100.0 * d->pass / run : 0.0;
        SDL_Log("%-34s %8d %8d %7.2f%% %8d %8d %8d", d->key, d->pass, run, pct, d->fail,
                d->timeout, d->skipped);
    }
    int run = n_pass + n_fail + n_timeout;
    double pct = run > 0 ? 100.0 * n_pass / run : 0.0;
    SDL_Log("total: %d tests, %d variants run: %d pass (%.2f%%), %d fail, %d timeout; "
            "%.1f s on %d threads",
            tests, run, n_pass, pct, n_fail, n_timeout, elapsed_ms / 1000.0, threads);
}

/* Waits for the workers. A worker whose variant makes no progress for T262_HANG_MS is marked
 * hung: its variant becomes a timeout and the wait goes on without it. Returns the number of hung
 * workers (their threads are still running). */
static int watch_workers(worker_slot *slots, SDL_Thread **th, int threads, test_entry *tests)
{
    int hung = 0;
    for (;;) {
        bool waiting = false;
        for (int i = 0; i < threads; i++) {
            worker_slot *w = &slots[i];
            if (th[i] == nullptr || w->hung || SDL_GetAtomicInt(&w->done) != 0) {
                continue;
            }
            waiting = true;
            int ti = SDL_GetAtomicInt(&w->test);
            int since = SDL_GetAtomicInt(&w->since);
            int v = SDL_GetAtomicInt(&w->variant);
            if (ti == 0 || SDL_GetAtomicInt(&w->test) != ti || v < 0 || v > 1) {
                continue;
            }
            /* The clock is read after since, so a variant that started meanwhile cannot look old. */
            int64_t ran = (int64_t)(SDL_GetTicks() - w->t0) - since;
            if (ran < (int64_t)T262_HANG_MS || SDL_GetAtomicInt(&w->since) != since) {
                continue;
            }
            test_entry *t = &tests[ti - 1];
            w->hung = true;
            hung++;
            t->state[v] = T262_TIMEOUT;
            SDL_snprintf(t->reason[v], sizeof t->reason[v],
                         "hung: no progress for %u s outside JavaScript (watchdog)",
                         T262_HANG_MS / 1000u);
            SDL_Log("WATCHDOG %s [%s]: no progress for %u s; reported as a timeout", t->rel,
                    variant_name(v), T262_HANG_MS / 1000u);
        }
        if (!waiting) {
            break;
        }
        SDL_Delay(100);
    }
    for (int i = 0; i < threads; i++) {
        if (th[i] != nullptr && !slots[i].hung) {
            SDL_WaitThread(th[i], nullptr);
        }
    }
    return hung;
}

int main(int argc, char **argv)
{
    const char *base = EMBER_SOURCE_DIR;
    char default_root[PATH_BUF];
    char default_exp[PATH_BUF];
    char default_unsup[PATH_BUF];
    char default_smoke[PATH_BUF];
    SDL_snprintf(default_root, sizeof default_root, "%s/third_party/test262", base);
    SDL_snprintf(default_exp, sizeof default_exp, "%s/tests/test262/expectations.txt", base);
    SDL_snprintf(default_unsup, sizeof default_unsup, "%s/tests/test262/unsupported-features.txt",
                 base);
    SDL_snprintf(default_smoke, sizeof default_smoke, "%s/tests/test262/smoke-list.txt", base);

    options o = {default_root, default_exp, default_unsup, nullptr, nullptr, 0, false, false, 0, false, 0};
    if (!parse_args(argc, argv, &o, default_smoke)) {
        return 2;
    }
    t262_set_gc(o.gc_stress, o.generational);
    t262_set_rope_min(o.rope_min);

    char probe[PATH_BUF];
    SDL_PathInfo info;
    if (SDL_snprintf(probe, sizeof probe, "%s/harness", o.root) >= (int)sizeof probe ||
        !SDL_GetPathInfo(probe, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        SDL_Log("test262: no test262 checkout at %s (run: git submodule update --init "
                "third_party/test262)",
                o.root);
        return EXIT_SKIPPED;
    }

    str_list unsupported = {nullptr, 0, 0};
    read_token_list(o.unsupported, &unsupported, false);

    int count = 0;
    test_entry *tests = build_tests(&o, &count);
    if (count == 0) {
        SDL_Log("test262: no tests selected");
        return 2;
    }

    exp_list exp = {nullptr, 0, 0};
    if (read_expectations(o.expectations, &exp) > 0) {
        SDL_Log("test262: %s has malformed lines", o.expectations);
        return 2;
    }
    mark_expected(tests, count, &exp);

    int threads = o.threads > 0 ? o.threads : SDL_GetNumLogicalCPUCores();
    if (threads < 1) {
        threads = 1;
    }
    if (threads > MAX_THREADS) {
        threads = MAX_THREADS;
    }
    if (threads > count) {
        threads = count;
    }

    shared s;
    s.root = o.root;
    s.tests = tests;
    s.count = count;
    s.unsupported = &unsupported;
    SDL_SetAtomicInt(&s.next, 0);

    uint64_t t0 = SDL_GetTicks();
    SDL_Thread *th[MAX_THREADS];
    static worker_slot slots[MAX_THREADS];
    int started = 0;
    for (int i = 0; i < threads; i++) {
        slots[i].s = &s;
        slots[i].t0 = t0;
        th[i] = SDL_CreateThread(worker_main, "test262", &slots[i]);
        if (th[i] != nullptr) {
            started++;
        }
    }
    int hung = 0;
    if (started == 0) {
        worker_main(&slots[0]);
    } else {
        hung = watch_workers(slots, th, threads, tests);
    }
    uint64_t elapsed = SDL_GetTicks() - t0;

    if (o.update) {
        if (o.list_file != nullptr || o.filter != nullptr) {
            SDL_Log("test262: --update merges this subset into the expectations");
        }
        bool ok = update_expectations(&o, tests, count, &exp);
        if (!ok) {
            return 2;
        }
    }

    dir_list dirs = {nullptr, 0, 0};
    int n_pass = 0;
    int n_fail = 0;
    int n_timeout = 0;
    int problems = report(&o, tests, count, &dirs, &n_pass, &n_fail, &n_timeout);
    print_summary(&dirs, count, n_pass, n_fail, n_timeout, elapsed, threads);

    int rc = 0;
    if (o.update) {
        SDL_Log("test262: --update wrote %d change(s) relative to the previous expectations",
                problems);
    } else if (problems > 0) {
        SDL_Log("test262: %d problem(s): fix them, or run test262 --update to accept the new "
                "state",
                problems);
        rc = 1;
    } else {
        SDL_Log("test262: OK, every variant matches the expectations");
    }
    if (hung > 0) {
        /* The hung threads still run the engine: exit without freeing what they may touch. */
        SDL_Log("test262: %d worker(s) hung (see WATCHDOG above)", hung);
        exit(rc != 0 ? rc : 1);
    }

    for (int i = 0; i < count; i++) {
        SDL_free(tests[i].rel);
    }
    SDL_free(tests);
    for (int i = 0; i < exp.count; i++) {
        SDL_free(exp.items[i].path);
    }
    SDL_free(exp.items);
    for (int i = 0; i < unsupported.count; i++) {
        SDL_free(unsupported.items[i]);
    }
    SDL_free(unsupported.items);
    SDL_free(dirs.items);
    return rc;
}
