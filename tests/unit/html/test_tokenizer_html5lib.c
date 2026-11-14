/*
 * Runs every .test file in third_party/html5lib-tests/tokenizer through the tokenizer.
 *
 * Each case runs once per initialStates entry (default "Data state"), with lastStartTag applied and
 * doubleEscaped unescaped. Output tokens and parse errors are rendered into a canonical form on
 * both sides, so attribute order and character coalescing do not matter:
 *   records:  D (doctype) S (start tag) E (end tag) M (comment) X (unknown expected token)
 *   strings:  length-prefixed bytes; attributes sorted by name
 *   errors:   "code@line:col" lines in order
 * A case passes when tokens and errors match. Cases that fail are allowed only when
 * tests/html/tokenizer-expectations.txt lists them as "<file>#<index> <reason>" (index is the test's
 * position in the file's array, counted from 0). The suite passes when at least 99% of cases pass
 * and every failure is listed.
 */

#include "emb_test.h"

#include "html/tokenizer.h"
#include "json_mini.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <stdlib.h>
#include <string.h>

#define RT_MAX_FILES 64
#define RT_MAX_EXPECT 512
#define RT_SHOW_FAILS 60

typedef struct canon {
    embi_sb out;
    embi_sb pend; /* pending character run, emitted as one C record */
} canon;

typedef struct case_ctx {
    canon act;
    embi_sb errs;
} case_ctx;

typedef struct kv {
    embi_str k;
    embi_str v;
} kv;

typedef struct stats {
    int total;
    int pass;
    int xfail;
    int xpass;
    int fail;
} stats;

typedef struct expect_entry {
    char key[128];
    char reason[256];
} expect_entry;

static char g_names[RT_MAX_FILES][128];
static int g_name_count;
static expect_entry g_expect[RT_MAX_EXPECT];
static int g_expect_count;
static int g_shown_fails;

/* ------------------------------------------------------------------ canonical rendering */

static embi_str sb_view(const embi_sb *sb)
{
    return (embi_str){sb->data, sb->len};
}

static void c_bytes(embi_sb *sb, const char *p, size_t n)
{
    if (n > 0) {
        embi_sb_append(sb, (embi_str){p, (uint32_t)n});
    }
}

static void c_uint(embi_sb *sb, size_t v)
{
    char b[32];
    int n = SDL_snprintf(b, sizeof b, "%llu;", (unsigned long long)v);
    c_bytes(sb, b, (size_t)n);
}

static void c_str(embi_sb *sb, embi_str s)
{
    c_uint(sb, s.len);
    c_bytes(sb, s.ptr, s.len);
}

static void c_opt(embi_sb *sb, bool has, embi_str s)
{
    if (has) {
        embi_sb_append_char(sb, 'S');
        c_str(sb, s);
    } else {
        embi_sb_append_char(sb, 'N');
    }
}

static void c_flush(canon *c)
{
    if (c->pend.len > 0) {
        embi_sb_append_char(&c->out, 'C');
        c_str(&c->out, sb_view(&c->pend));
        embi_sb_append_char(&c->out, '\n');
        embi_sb_clear(&c->pend);
    }
}

static embi_sb *c_begin(canon *c, char kind)
{
    c_flush(c);
    embi_sb_append_char(&c->out, kind);
    return &c->out;
}

static void c_end(embi_sb *sb)
{
    embi_sb_append_char(sb, '\n');
}

static int bytes_cmp(embi_str a, embi_str b)
{
    uint32_t n = a.len < b.len ? a.len : b.len;
    if (n > 0) {
        int r = memcmp(a.ptr, b.ptr, n);
        if (r != 0) {
            return r;
        }
    }
    if (a.len == b.len) {
        return 0;
    }
    return a.len < b.len ? -1 : 1;
}

static void c_attrs(embi_sb *o, kv *arr, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        kv x = arr[i];
        size_t j = i;
        while (j > 0 && bytes_cmp(arr[j - 1].k, x.k) > 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = x;
    }
    c_uint(o, n);
    for (size_t i = 0; i < n; i++) {
        c_str(o, arr[i].k);
        c_str(o, arr[i].v);
    }
}

/* Writes the canonical bytes of a rendered actual token. */
static bool actual_sink(void *ctx, const embi_html_token *tok)
{
    case_ctx *cc = ctx;
    canon *c = &cc->act;
    switch (tok->kind) {
    case EMBI_HTML_TOK_CHARACTERS:
        c_bytes(&c->pend, tok->data.ptr, tok->data.len);
        break;
    case EMBI_HTML_TOK_DOCTYPE: {
        embi_sb *o = c_begin(c, 'D');
        c_opt(o, tok->has_name, tok->name);
        c_opt(o, tok->has_public_id, tok->public_id);
        c_opt(o, tok->has_system_id, tok->system_id);
        embi_sb_append_char(o, tok->force_quirks ? '1' : '0');
        c_end(o);
        break;
    }
    case EMBI_HTML_TOK_START_TAG: {
        embi_sb *o = c_begin(c, 'S');
        c_str(o, tok->name);
        kv *arr = SDL_malloc((tok->attr_count ? tok->attr_count : 1) * sizeof *arr);
        for (uint32_t i = 0; i < tok->attr_count; i++) {
            arr[i].k = tok->attrs[i].name;
            arr[i].v = tok->attrs[i].value;
        }
        c_attrs(o, arr, tok->attr_count);
        SDL_free(arr);
        embi_sb_append_char(o, tok->self_closing ? '1' : '0');
        c_end(o);
        break;
    }
    case EMBI_HTML_TOK_END_TAG: {
        embi_sb *o = c_begin(c, 'E');
        c_str(o, tok->name);
        c_end(o);
        break;
    }
    case EMBI_HTML_TOK_COMMENT: {
        embi_sb *o = c_begin(c, 'M');
        c_str(o, tok->data);
        c_end(o);
        break;
    }
    case EMBI_HTML_TOK_PI: {
        embi_sb *o = c_begin(c, 'P');
        c_str(o, tok->name);
        c_str(o, tok->data);
        c_end(o);
        break;
    }
    case EMBI_HTML_TOK_EOF:
        c_flush(c);
        break;
    }
    return true;
}

static void actual_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    case_ctx *cc = ctx;
    (void)offset;
    char buf[160];
    int n = SDL_snprintf(buf, sizeof buf, "%s@%u:%u\n", code, line, col);
    c_bytes(&cc->errs, buf, (size_t)n);
}

/* ------------------------------------------------------------------ expected side */

static void hex_unescape_bytes(jm_buf *out, const char *s, size_t n, bool dbl);

/* Writes a JSON string as canonical bytes, after the double-escape pass when dbl is set. */
static void c_jstr(embi_sb *sb, const jm_val *v, bool dbl)
{
    jm_buf tmp = {nullptr, 0, 0};
    hex_unescape_bytes(&tmp, v->str, v->len, dbl);
    c_str(sb, (embi_str){tmp.p, (uint32_t)tmp.n});
    free(tmp.p);
}

static bool is_str(const jm_val *v)
{
    return v != nullptr && v->type == JM_STRING;
}

static void c_jopt(embi_sb *sb, const jm_val *v, bool dbl)
{
    if (is_str(v)) {
        embi_sb_append_char(sb, 'S');
        c_jstr(sb, v, dbl);
    } else {
        embi_sb_append_char(sb, 'N');
    }
}

static int hex4_at(const char *s, size_t n, size_t at, uint32_t *out)
{
    if (at + 4 > n) {
        return 0;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) {
        int h = jm_hexval(s[at + i]);
        if (h < 0) {
            return 0;
        }
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

/* Second unescape pass for doubleEscaped tests: \uHHHH (with surrogate pairs) becomes UTF-8. */
static void hex_unescape_bytes(jm_buf *out, const char *s, size_t n, bool dbl)
{
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        if (dbl && s[i] == '\\' && i + 1 < n && s[i + 1] == 'u' && hex4_at(s, n, i + 2, &cp)) {
            i += 6;
            uint32_t lo;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u' &&
                hex4_at(s, n, i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            jm_buf_put_cp(out, cp);
            continue;
        }
        jm_buf_put(out, (unsigned char)s[i]);
        i++;
    }
}

/* Expected tokens use the same records as the actual side. Unknown kinds become X records. */
static void expected_tokens(canon *c, const jm_val *output, bool dbl)
{
    for (size_t i = 0; i < output->count; i++) {
        const jm_val *tok = &output->items[i];
        if (tok->type != JM_ARRAY || tok->count < 1 || !is_str(&tok->items[0])) {
            c_begin(c, 'X');
            c_end(&c->out);
            continue;
        }
        const char *kind = tok->items[0].str;
        size_t n = tok->count;
        if (strcmp(kind, "Character") == 0 && n >= 2 && is_str(&tok->items[1])) {
            jm_buf tmp = {nullptr, 0, 0};
            hex_unescape_bytes(&tmp, tok->items[1].str, tok->items[1].len, dbl);
            c_bytes(&c->pend, tmp.p, tmp.n);
            free(tmp.p);
        } else if (strcmp(kind, "DOCTYPE") == 0 && n >= 5) {
            embi_sb *o = c_begin(c, 'D');
            c_jopt(o, &tok->items[1], dbl);
            c_jopt(o, &tok->items[2], dbl);
            c_jopt(o, &tok->items[3], dbl);
            bool correct = tok->items[4].type == JM_BOOL && tok->items[4].b;
            embi_sb_append_char(o, correct ? '0' : '1');
            c_end(o);
        } else if (strcmp(kind, "StartTag") == 0 && n >= 2 && is_str(&tok->items[1])) {
            embi_sb *o = c_begin(c, 'S');
            c_jstr(o, &tok->items[1], dbl);
            size_t na = 0;
            kv *arr = nullptr;
            if (n >= 3 && tok->items[2].type == JM_OBJECT) {
                na = tok->items[2].count;
                arr = SDL_malloc((na ? na : 1) * sizeof *arr);
                for (size_t k = 0; k < na; k++) {
                    jm_buf kb = {nullptr, 0, 0};
                    jm_buf vb = {nullptr, 0, 0};
                    hex_unescape_bytes(&kb, tok->items[2].keys[k], strlen(tok->items[2].keys[k]), dbl);
                    const jm_val *val = &tok->items[2].items[k];
                    hex_unescape_bytes(&vb, val->str ? val->str : "", val->len, dbl);
                    arr[k].k = (embi_str){kb.p, (uint32_t)kb.n};
                    arr[k].v = (embi_str){vb.p, (uint32_t)vb.n};
                }
                c_attrs(o, arr, na);
                /* Free the temporary byte copies after rendering. */
                for (size_t k = 0; k < na; k++) {
                    free((void *)arr[k].k.ptr);
                    free((void *)arr[k].v.ptr);
                }
                SDL_free(arr);
            } else {
                c_uint(o, 0);
            }
            embi_sb_append_char(o, (n >= 4 && tok->items[3].type == JM_BOOL && tok->items[3].b) ? '1' : '0');
            c_end(o);
        } else if (strcmp(kind, "EndTag") == 0 && n >= 2 && is_str(&tok->items[1])) {
            embi_sb *o = c_begin(c, 'E');
            c_jstr(o, &tok->items[1], dbl);
            c_end(o);
        } else if (strcmp(kind, "Comment") == 0 && n >= 2 && is_str(&tok->items[1])) {
            embi_sb *o = c_begin(c, 'M');
            c_jstr(o, &tok->items[1], dbl);
            c_end(o);
        } else if (strcmp(kind, "ProcessingInstruction") == 0 && n >= 3 && is_str(&tok->items[1]) &&
                   is_str(&tok->items[2])) {
            embi_sb *o = c_begin(c, 'P');
            c_jstr(o, &tok->items[1], dbl);
            c_jstr(o, &tok->items[2], dbl);
            c_end(o);
        } else {
            /* Malformed entries never match a produced token. */
            c_begin(c, 'X');
            c_end(&c->out);
        }
    }
    c_flush(c);
}

static void expected_errors(embi_sb *out, const jm_val *errors)
{
    if (errors == nullptr || errors->type != JM_ARRAY) {
        return;
    }
    for (size_t i = 0; i < errors->count; i++) {
        const jm_val *e = &errors->items[i];
        const jm_val *code = jm_get(e, "code");
        const jm_val *line = jm_get(e, "line");
        const jm_val *col = jm_get(e, "col");
        if (!is_str(code) || line == nullptr || col == nullptr) {
            c_bytes(out, "malformed\n", 10);
            continue;
        }
        char buf[200];
        int n = SDL_snprintf(buf, sizeof buf, "%s@%.0f:%.0f\n", code->str, line->num, col->num);
        c_bytes(out, buf, (size_t)n);
    }
}

/* ------------------------------------------------------------------ expectations and files */

static void load_expectations(void)
{
    size_t size = 0;
    char *text = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/html/tokenizer-expectations.txt", &size);
    if (text == nullptr) {
        SDL_Log("tokenizer expectations file not found; no failures are allowed");
        return;
    }
    const char *p = text;
    const char *end = text + size;
    while (p < end && g_expect_count < RT_MAX_EXPECT) {
        const char *line = p;
        while (p < end && *p != '\n') {
            p++;
        }
        size_t len = (size_t)(p - line);
        if (p < end) {
            p++;
        }
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            len--;
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        size_t k = 0;
        while (k < len && line[k] != ' ' && line[k] != '\t') {
            k++;
        }
        size_t r = k;
        while (r < len && (line[r] == ' ' || line[r] == '\t')) {
            r++;
        }
        expect_entry *e = &g_expect[g_expect_count++];
        size_t klen = k < sizeof e->key - 1 ? k : sizeof e->key - 1;
        memcpy(e->key, line, klen);
        e->key[klen] = '\0';
        size_t rlen = len - r < sizeof e->reason - 1 ? len - r : sizeof e->reason - 1;
        memcpy(e->reason, line + r, rlen);
        e->reason[rlen] = '\0';
        if (rlen == 0) {
            SDL_Log("expectation without a reason: %s", e->key);
            EMB_CHECK(false);
        }
    }
    SDL_free(text);
}

static const expect_entry *find_expectation(const char *key)
{
    for (int i = 0; i < g_expect_count; i++) {
        if (SDL_strcmp(g_expect[i].key, key) == 0) {
            return &g_expect[i];
        }
    }
    return nullptr;
}

static SDL_EnumerationResult SDLCALL collect_name(void *ud, const char *dir, const char *fname)
{
    (void)ud;
    (void)dir;
    size_t n = SDL_strlen(fname);
    if (n > 5 && SDL_strcmp(fname + n - 5, ".test") == 0 && g_name_count < RT_MAX_FILES && n < 127) {
        SDL_strlcpy(g_names[g_name_count++], fname, sizeof g_names[0]);
    }
    return SDL_ENUM_CONTINUE;
}

static int cmp_names(const void *a, const void *b)
{
    return SDL_strcmp((const char *)a, (const char *)b);
}

static void print_block(const char *label, const embi_sb *sb)
{
    size_t shown = sb->len < 240 ? sb->len : 240;
    SDL_Log("    %s (%llu bytes): %.*s", label, (unsigned long long)sb->len, (int)shown,
            sb->len ? sb->data : "");
}

/* Runs one initial state of one test. Returns true when the case passed. */
static bool run_case(const char *file, size_t index, const jm_val *test, const char *state_name,
                     stats *fs, stats *all)
{
    const jm_val *input = jm_get(test, "input");
    const jm_val *output = jm_get(test, "output");
    const jm_val *errors = jm_get(test, "errors");
    const jm_val *dbl_v = jm_get(test, "doubleEscaped");
    const jm_val *last = jm_get(test, "lastStartTag");
    const jm_val *desc = jm_get(test, "description");
    bool dbl = dbl_v != nullptr && dbl_v->type == JM_BOOL && dbl_v->b;

    embi_html_state st = EMBI_HTML_STATE_DATA;
    if (SDL_strcmp(state_name, "RCDATA state") == 0) {
        st = EMBI_HTML_STATE_RCDATA;
    } else if (SDL_strcmp(state_name, "RAWTEXT state") == 0) {
        st = EMBI_HTML_STATE_RAWTEXT;
    } else if (SDL_strcmp(state_name, "Script data state") == 0) {
        st = EMBI_HTML_STATE_SCRIPT_DATA;
    } else if (SDL_strcmp(state_name, "PLAINTEXT state") == 0) {
        st = EMBI_HTML_STATE_PLAINTEXT;
    } else if (SDL_strcmp(state_name, "CDATA section state") == 0) {
        st = EMBI_HTML_STATE_CDATA_SECTION;
    } else if (SDL_strcmp(state_name, "Data state") != 0) {
        SDL_Log("unknown initial state '%s' in %s", state_name, file);
        return false;
    }

    jm_buf in = {nullptr, 0, 0};
    if (!is_str(input)) {
        return false;
    }
    hex_unescape_bytes(&in, input->str, input->len, dbl);

    case_ctx cc = {0};
    embi_html_tokenizer *t = embi_html_tokenizer_create(actual_sink, actual_error, &cc);
    embi_html_tokenizer_set_state(t, st);
    if (is_str(last)) {
        jm_buf lb = {nullptr, 0, 0};
        hex_unescape_bytes(&lb, last->str, last->len, false);
        embi_html_tokenizer_set_last_start_tag(t, (embi_str){lb.p, (uint32_t)lb.n});
        free(lb.p);
    }
    size_t accepted = embi_html_tokenizer_write(t, in.p, in.n);
    EMB_CHECK(accepted == in.n);
    embi_html_tokenizer_end(t);
    c_flush(&cc.act);
    embi_html_tokenizer_destroy(t);

    canon exp = {0};
    expected_tokens(&exp, output, dbl);
    c_flush(&exp);
    embi_sb exp_errs = {0};
    expected_errors(&exp_errs, errors);

    bool tok_ok = embi_str_eq(sb_view(&exp.out), sb_view(&cc.act.out));
    bool err_ok = embi_str_eq(sb_view(&exp_errs), sb_view(&cc.errs));
    bool pass = tok_ok && err_ok;

    char key[160];
    SDL_snprintf(key, sizeof key, "%s#%llu", file, (unsigned long long)index);
    const expect_entry *ex = find_expectation(key);

    fs->total++;
    all->total++;
    if (pass) {
        fs->pass++;
        all->pass++;
        if (ex != nullptr) {
            fs->xpass++;
            all->xpass++;
            SDL_Log("NOTE %s is listed as an expected failure but passes; remove it", key);
        }
    } else if (ex != nullptr) {
        fs->xfail++;
        all->xfail++;
    } else {
        fs->fail++;
        all->fail++;
        if (g_shown_fails < RT_SHOW_FAILS) {
            g_shown_fails++;
            SDL_Log("FAIL %s [%s] %s%s%s", key, state_name,
                    is_str(desc) ? desc->str : "", tok_ok ? "" : " (tokens)",
                    err_ok ? "" : " (errors)");
            if (!tok_ok) {
                print_block("expected tokens", &exp.out);
                print_block("actual tokens  ", &cc.act.out);
            }
            if (!err_ok) {
                print_block("expected errors", &exp_errs);
                print_block("actual errors  ", &cc.errs);
            }
        }
    }

    embi_sb_free(&exp.out);
    embi_sb_free(&exp.pend);
    embi_sb_free(&exp_errs);
    embi_sb_free(&cc.act.out);
    embi_sb_free(&cc.act.pend);
    embi_sb_free(&cc.errs);
    free(in.p);
    return pass;
}

static void run_file(const char *fname, stats *all)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/third_party/html5lib-tests/tokenizer/%s", EMBER_SOURCE_DIR,
                 fname);
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    if (data == nullptr) {
        SDL_Log("cannot read %s", path);
        EMB_CHECK(false);
        return;
    }
    jm_val root;
    bool ok = jm_parse((const char *)data, size, &root);
    SDL_free(data);
    if (!ok) {
        SDL_Log("cannot parse %s as JSON", path);
        EMB_CHECK(false);
        return;
    }
    const jm_val *tests = jm_get(&root, "tests");
    if (tests == nullptr) {
        tests = jm_get(&root, "xmlViolationTests");
    }
    if (tests == nullptr || tests->type != JM_ARRAY) {
        SDL_Log("%s has no test array", fname);
        EMB_CHECK(false);
        jm_free(&root);
        return;
    }

    stats fs = {0};
    for (size_t ti = 0; ti < tests->count; ti++) {
        const jm_val *test = &tests->items[ti];
        const jm_val *states = jm_get(test, "initialStates");
        if (states != nullptr && states->type == JM_ARRAY) {
            for (size_t si = 0; si < states->count; si++) {
                if (!is_str(&states->items[si])) {
                    continue;
                }
                run_case(fname, ti, test, states->items[si].str, &fs, all);
            }
        } else {
            run_case(fname, ti, test, "Data state", &fs, all);
        }
    }
    jm_free(&root);

    SDL_Log("%-32s %5d / %-5d passed  (expected failures %d, unexpected %d)", fname, fs.pass,
            fs.total, fs.xfail, fs.fail);
}

EMB_TEST(html5lib_tokenizer_suite)
{
    load_expectations();

    char dir[512];
    SDL_snprintf(dir, sizeof dir, "%s/third_party/html5lib-tests/tokenizer", EMBER_SOURCE_DIR);
    if (!SDL_EnumerateDirectory(dir, collect_name, nullptr)) {
        SDL_Log("cannot enumerate %s", dir);
        EMB_CHECK(false);
        return;
    }
    EMB_CHECK(g_name_count > 0);
    qsort(g_names, (size_t)g_name_count, sizeof g_names[0], cmp_names);

    stats all = {0};
    for (int i = 0; i < g_name_count; i++) {
        run_file(g_names[i], &all);
    }

    double pct = all.total ? 100.0 * all.pass / all.total : 0.0;
    SDL_Log("TOTAL %d / %d cases passed (%.2f%%); expected failures %d, unexpected %d, "
            "listed but passing %d",
            all.pass, all.total, pct, all.xfail, all.fail, all.xpass);
    EMB_CHECK(all.total > 0);
    EMB_CHECK(all.fail == 0);
    EMB_CHECK(all.pass * 100 >= all.total * 99);
}

EMB_TEST_MAIN()
