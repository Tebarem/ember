/*
 * Runs every tests/style html fixture. A fixture holds its author CSS in <style> blocks and its
 * expectations as comments of the form
 *
 *     <!-- expect: ID[::before|::after] PROP VALUE -->
 *
 * where ID is the element's id attribute, PROP is a property name and VALUE is the serialized
 * computed value (embi_style_value_text), compared exactly after trimming.
 */
#include "emb_test.h"

#include "css/props.h"
#include "css/selector.h"
#include "css/css.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <string.h>

#define STYLE_DIR EMBER_SOURCE_DIR "/tests/style"

typedef struct fixture_run {
    uint32_t files;
    uint32_t checks;
    uint32_t failures;
} fixture_run;

static char *trim(char *s, size_t *len)
{
    while (*len > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
        (*len)--;
    }
    while (*len > 0 && (s[*len - 1] == ' ' || s[*len - 1] == '\t' || s[*len - 1] == '\r' ||
                        s[*len - 1] == '\n')) {
        (*len)--;
    }
    return s;
}

/* Concatenates the bodies of every <style> block of the fixture. */
static embi_str collect_css(const char *html, size_t n, char **out_buf)
{
    size_t cap = n + 1, len = 0;
    char *buf = SDL_malloc(cap);
    size_t pos = 0;
    while (pos < n) {
        const char *open = SDL_strcasestr(html + pos, "<style");
        if (open == nullptr) {
            break;
        }
        const char *body = SDL_strchr(open, '>');
        if (body == nullptr) {
            break;
        }
        body++;
        const char *close = SDL_strcasestr(body, "</style>");
        size_t blen = close ? (size_t)(close - body) : strlen(body);
        if (len + blen + 1 > cap) {
            cap = (len + blen + 1) * 2;
            buf = SDL_realloc(buf, cap);
        }
        memcpy(buf + len, body, blen);
        len += blen;
        buf[len++] = '\n';
        pos = close ? (size_t)(close - html) + 8 : n;
    }
    buf[len] = '\0';
    *out_buf = buf;
    return (embi_str){buf, (uint32_t)len};
}

/* A malformed expectation is a failure: it must never be skipped silently. */
static void bad_expect(const char *file, const char *line, fixture_run *run)
{
    run->checks++;
    run->failures++;
    SDL_Log("%s: malformed expectation '%s'", file, line);
}

static void run_expect(embi_style_engine *eng, embi_document *doc, const char *file, char *line,
                       size_t len, fixture_run *run)
{
    line = trim(line, &len);
    char *sp = memchr(line, ' ', len);
    if (sp == nullptr) {
        bad_expect(file, line, run);
        return;
    }
    size_t idlen = (size_t)(sp - line);
    char idbuf[128];
    if (idlen == 0 || idlen >= sizeof idbuf) {
        bad_expect(file, line, run);
        return;
    }
    memcpy(idbuf, line, idlen);
    idbuf[idlen] = '\0';

    embi_sel_pseudo pe = EMBI_PE_NONE;
    char *colons = SDL_strstr(idbuf, "::");
    if (colons != nullptr) {
        if (SDL_strcmp(colons, "::before") == 0) {
            pe = EMBI_PE_BEFORE;
        } else if (SDL_strcmp(colons, "::after") == 0) {
            pe = EMBI_PE_AFTER;
        } else if (SDL_strcmp(colons, "::marker") == 0) {
            pe = EMBI_PE_MARKER;
        } else {
            bad_expect(file, line, run);
            return;
        }
        *colons = '\0';
    }

    size_t rest_len = len - idlen - 1;
    char *rest = sp + 1;
    rest = trim(rest, &rest_len);
    char *psp = memchr(rest, ' ', rest_len);
    if (psp == nullptr) {
        bad_expect(file, line, run);
        return;
    }
    size_t plen = (size_t)(psp - rest);
    char prop[64];
    if (plen == 0 || plen >= sizeof prop) {
        bad_expect(file, line, run);
        return;
    }
    memcpy(prop, rest, plen);
    prop[plen] = '\0';
    size_t vlen = rest_len - plen - 1;
    char *val = trim(psp + 1, &vlen);

    run->checks++;
    embi_element *el = embi_get_element_by_id(doc, (embi_str){idbuf, (uint32_t)strlen(idbuf)});
    if (el == nullptr) {
        run->failures++;
        SDL_Log("%s: no element with id %s", file, idbuf);
        return;
    }
    const embi_style *s = embi_style_get(eng, el, pe);
    embi_sb sb = {nullptr, 0, 0};
    embi_prop_id pid = embi_prop_lookup((embi_str){prop, (uint32_t)plen});
    bool ok = s != nullptr && pid != EMBI_PROP_NONE && embi_style_value_text(s, pid, &sb);
    bool same = ok && sb.len == vlen && memcmp(sb.data, val, vlen) == 0;
    if (!same) {
        run->failures++;
        SDL_Log("%s: #%s%s %s expected '%.*s' got '%.*s'", file, idbuf,
                pe == EMBI_PE_BEFORE ? "::before" : pe == EMBI_PE_AFTER ? "::after" : "", prop,
                (int)vlen, val, ok ? (int)sb.len : 0, ok ? sb.data : "");
    }
    embi_sb_free(&sb);
}

/* @import in a fixture loads the file of that name from tests/style/imports. */
static bool fixture_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                          const unsigned char **bytes, size_t *n)
{
    (void)user;
    (void)base;
    (void)resolved;
    for (uint32_t i = 0; i + 1 < url.len; i++) {
        if (url.ptr[i] == '.' && url.ptr[i + 1] == '.') {
            return false; /* fixtures must not escape tests/style/imports */
        }
    }
    char path[1024];
    SDL_snprintf(path, sizeof path, STYLE_DIR "/imports/%.*s", (int)url.len, url.ptr);
    static char *held = nullptr;
    SDL_free(held);
    size_t len = 0;
    held = SDL_LoadFile(path, &len);
    if (held == nullptr) {
        return false;
    }
    *bytes = (const unsigned char *)held;
    *n = len;
    return true;
}

static void run_fixture(embi_style_engine *eng, const char *file, const char *path,
                        fixture_run *run)
{
    size_t n = 0;
    char *html = SDL_LoadFile(path, &n);
    if (html == nullptr) {
        run->failures++;
        SDL_Log("cannot read %s", path);
        return;
    }
    run->files++;

    char *css = nullptr;
    embi_str text = collect_css(html, n, &css);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_loader loader = {nullptr, fixture_fetch};
    embi_css_parse_opts opts = {&loader, &hooks, EMBI_STR("")};
    embi_css_sheet *sheet = embi_css_sheet_parse(text, &opts);

    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);

    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);

    uint32_t checks_before = run->checks;
    size_t pos = 0;
    while (pos < n) {
        const char *hit = SDL_strstr(html + pos, "<!-- expect:");
        if (hit == nullptr) {
            break;
        }
        const char *start = hit + SDL_strlen("<!-- expect:");
        const char *end = SDL_strstr(start, "-->");
        if (end == nullptr) {
            run->failures++;
            SDL_Log("%s: unterminated expect comment", file);
            break;
        }
        char tmp[512];
        size_t tl = (size_t)(end - start);
        if (tl >= sizeof tmp) {
            tl = sizeof tmp - 1;
        }
        memcpy(tmp, start, tl);
        tmp[tl] = '\0';
        run_expect(eng, doc, file, tmp, tl, run);
        pos = (size_t)(end - html) + 3;
    }

    if (run->checks == checks_before) {
        run->failures++;
        SDL_Log("%s: fixture has no expect lines", file);
    }

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    SDL_free(css);
    SDL_free(html);
}

typedef struct enum_ctx {
    embi_style_engine *eng;
    fixture_run *run;
} enum_ctx;

static SDL_EnumerationResult on_entry(void *userdata, const char *dirname, const char *fname)
{
    enum_ctx *ctx = userdata;
    size_t flen = SDL_strlen(fname);
    if (flen < 5 || SDL_strcmp(fname + flen - 5, ".html") != 0) {
        return SDL_ENUM_CONTINUE;
    }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dirname, fname);
    run_fixture(ctx->eng, fname, path, ctx->run);
    return SDL_ENUM_CONTINUE;
}

EMB_TEST(style_fixtures_match_expectations)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    fixture_run run = {0, 0, 0};
    enum_ctx ctx = {eng, &run};
    EMB_CHECK(SDL_EnumerateDirectory(STYLE_DIR, on_entry, &ctx));
    EMB_CHECK(run.files > 0);
    EMB_CHECK(run.checks > 0);
    SDL_Log("style fixtures: %u files, %u checks, %u failures", run.files, run.checks,
            run.failures);
    EMB_CHECK_EQ_INT(run.failures, 0);
    embi_style_engine_destroy(eng);
}

EMB_TEST_MAIN()
