/*
 * @import through an embedder loader (resolution, cycle detection, layer and supports on imports),
 * and byte stream parsing (BOM, protocol and @charset precedence, UTF-16, legacy labels).
 */
#include "emb_test.h"

#include "css/css.h"

#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_loader {
    const char *urls[8];
    const char *bodies[8];
    int calls;
} fake_loader;

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

/* Serves exact URLs as written. The resolved URL is the URL itself. */
static bool fake_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                       const unsigned char **bytes, size_t *n)
{
    (void)base;
    fake_loader *fl = user;
    fl->calls++;
    for (int k = 0; k < 8 && fl->urls[k]; k++) {
        if (embi_str_eq(S(fl->urls[k]), url)) {
            *resolved = url;
            *bytes = (const unsigned char *)fl->bodies[k];
            *n = strlen(fl->bodies[k]);
            return true;
        }
    }
    return false;
}

static bool is_str(embi_str s, const char *lit)
{
    return embi_str_eq(s, S(lit));
}

static bool hook_decl_ok(void *user, embi_str name, embi_css_slice value, bool important)
{
    (void)user;
    (void)value;
    (void)important;
    return !is_str(name, "bogus");
}

static const embi_css_hooks hooks = {nullptr, hook_decl_ok, nullptr};

EMB_TEST(css_import_cycle_is_cut)
{
    fake_loader fl = {
        .urls = {"a.css", "b.css"},
        .bodies = {"@import 'b.css'; p { color: red }", "@import 'a.css'; q { color: blue }"},
    };
    embi_css_loader ld = {&fl, fake_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, {nullptr, 0}};
    embi_css_sheet *root = embi_css_sheet_parse(S("@import 'a.css';"), &opts);

    EMB_REQUIRE(root->nrules == 1);
    EMB_REQUIRE(root->rules[0].kind == EMBI_CSS_RULE_IMPORT);
    embi_css_sheet *a = root->rules[0].sheet;
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(a->nrules == 2);
    EMB_CHECK(a->rules[0].kind == EMBI_CSS_RULE_IMPORT);
    embi_css_sheet *b = a->rules[0].sheet;
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(b->nrules == 2);
    /* b imports a again. The URL is only known after the fetch, so it is fetched once more and
     * then cut, which makes three fetches in all: a, b, and a again. */
    EMB_CHECK(b->rules[0].kind == EMBI_CSS_RULE_IMPORT);
    EMB_CHECK(b->rules[0].sheet == nullptr);
    EMB_CHECK(b->rules[1].kind == EMBI_CSS_RULE_STYLE);
    EMB_CHECK(fl.calls == 3);

    embi_css_sheet_unref(root);
}

EMB_TEST(css_import_layer_supports_media)
{
    fake_loader fl = {.urls = {"c.css"}, .bodies = {"p { color: red }"}};
    embi_css_loader ld = {&fl, fake_fetch};
    embi_css_parse_opts opts = {&ld, &hooks, {nullptr, 0}};
    const char *css =
        "@import url('c.css') layer(base) supports(display: grid) screen;"
        "@import url(\"c.css\") supports(bogus: 1);"
        "@import 'c.css' layer;"
        "@import 'c.css' layer(a b);";
    embi_css_sheet *s = embi_css_sheet_parse(S(css), &opts);
    EMB_REQUIRE(s->nrules == 3);

    const embi_css_rule *r0 = &s->rules[0];
    EMB_CHECK(r0->kind == EMBI_CSS_RULE_IMPORT);
    EMB_CHECK(r0->has_layer);
    EMB_CHECK(r0->name != 0 && is_str(embi_atom_str(r0->name), "base"));
    EMB_CHECK(r0->cond);
    EMB_CHECK(r0->prelude.n == 1);
    EMB_CHECK(r0->sheet != nullptr);
    EMB_CHECK(is_str(r0->url, "c.css"));

    /* A false supports() condition: the import is neither fetched nor applied. */
    const embi_css_rule *r1 = &s->rules[1];
    EMB_CHECK(!r1->has_layer);
    EMB_CHECK(!r1->cond);
    EMB_CHECK(r1->sheet == nullptr);

    const embi_css_rule *r2 = &s->rules[2];
    EMB_CHECK(r2->has_layer);
    EMB_CHECK(r2->name == 0);
    EMB_CHECK(r2->cond);
    EMB_CHECK(r2->sheet != nullptr);
    /* Fetches: the first import and the third. The second is skipped by supports(). */
    EMB_CHECK(fl.calls == 2);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_import_without_loader_has_no_sheet)
{
    embi_css_sheet *s = embi_css_sheet_parse(S("@import 'x.css'; a{}"), nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_IMPORT);
    EMB_CHECK(s->rules[0].sheet == nullptr);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_import_missing_target_has_no_sheet)
{
    fake_loader fl = {.urls = {"other.css"}, .bodies = {"a{}"}};
    embi_css_loader ld = {&fl, fake_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, {nullptr, 0}};
    embi_css_sheet *s = embi_css_sheet_parse(S("@import 'x.css';"), &opts);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].sheet == nullptr);
    EMB_CHECK(fl.calls == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_import_base_url_cycle)
{
    /* The root's own URL is on the import stack. The import is fetched, resolved to that URL,
     * and cut. */
    fake_loader fl = {.urls = {"root.css"}, .bodies = {"@import 'root.css'; a {}"}};
    embi_css_loader ld = {&fl, fake_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, S("root.css")};
    embi_css_sheet *s = embi_css_sheet_parse(S("@import 'root.css';"), &opts);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].sheet == nullptr);
    EMB_CHECK(fl.calls == 1);
    embi_css_sheet_unref(s);
}

/* ------------------------------------------------------------------ relative resolution */

/* A site of absolute http URLs. The loader resolves relative URLs against the base it is given,
 * the way an embedder would, and records each call's base for the assertions. */
typedef struct site {
    const char *urls[8];
    const char *bodies[8];
    int calls;
    char bases[8][128];
    char resolved[256];
} site;

/* Resolves url against base for http URLs with a path (no query or fragment). Writes the result
 * with "." and ".." segments removed. Returns false when the result would not fit. */
static bool resolve_url(embi_str base, embi_str url, char *out, size_t cap)
{
    char tmp[256];
    if (url.len >= 7 && memcmp(url.ptr, "http://", 7) == 0) {
        if (url.len >= sizeof tmp) {
            return false;
        }
        memcpy(tmp, url.ptr, url.len);
        tmp[url.len] = '\0';
    } else {
        size_t dir = base.len;
        while (dir > 0 && base.ptr[dir - 1] != '/') {
            dir--;
        }
        if (dir + url.len >= sizeof tmp) {
            return false;
        }
        memcpy(tmp, base.ptr, dir);
        memcpy(tmp + dir, url.ptr, url.len);
        tmp[dir + url.len] = '\0';
    }
    char *host_end = strstr(tmp, "://");
    if (host_end == nullptr) {
        return false;
    }
    char *path = strchr(host_end + 3, '/');
    if (path == nullptr) {
        return false;
    }
    char *segs[32];
    size_t lens[32];
    size_t ns = 0;
    char *s = path + 1;
    while (*s != '\0') {
        char *e = strchr(s, '/');
        size_t sl = e != nullptr ? (size_t)(e - s) : strlen(s);
        if (sl == 1 && s[0] == '.') {
            /* stays */
        } else if (sl == 2 && s[0] == '.' && s[1] == '.') {
            if (ns > 0) {
                ns--;
            }
        } else {
            if (ns == 32) {
                return false;
            }
            segs[ns] = s;
            lens[ns] = sl;
            ns++;
        }
        if (e == nullptr) {
            break;
        }
        s = e + 1;
    }
    size_t hl = (size_t)(path - tmp);
    if (hl >= cap) {
        return false;
    }
    memcpy(out, tmp, hl);
    size_t o = hl;
    for (size_t k = 0; k < ns; k++) {
        if (o + 1 + lens[k] >= cap) {
            return false;
        }
        out[o++] = '/';
        memcpy(out + o, segs[k], lens[k]);
        o += lens[k];
    }
    out[o] = '\0';
    return true;
}

static bool site_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                       const unsigned char **bytes, size_t *n)
{
    site *st = user;
    int call = st->calls++;
    if (call >= 8) {
        return false;
    }
    /* Record what the parser passed as the base for this call. */
    size_t bl = base.len < sizeof st->bases[call] - 1 ? base.len : sizeof st->bases[call] - 1;
    memcpy(st->bases[call], base.ptr, bl);
    st->bases[call][bl] = '\0';

    if (!resolve_url(base, url, st->resolved, sizeof st->resolved)) {
        return false;
    }
    *resolved = (embi_str){st->resolved, (uint32_t)strlen(st->resolved)};
    for (int k = 0; k < 8 && st->urls[k] != nullptr; k++) {
        if (strcmp(st->urls[k], st->resolved) == 0) {
            *bytes = (const unsigned char *)st->bodies[k];
            *n = strlen(st->bodies[k]);
            return true;
        }
    }
    return false;
}

EMB_TEST(css_import_relative_resolution_and_spellings)
{
    /* main -> sub/b.css -> c.css. c.css imports b.css and itself through other spellings. Both
     * are cut because the resolved URLs are on the stack. */
    site st = {
        .urls = {"http://h/a/sub/b.css", "http://h/a/sub/c.css"},
        .bodies = {"@import 'c.css'; b { x: y }",
                   "@import '../sub/b.css'; @import './c.css'; c { x: y }"},
    };
    embi_css_loader ld = {&st, site_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, S("http://h/a/main.css")};
    embi_css_sheet *root = embi_css_sheet_parse(S("@import 'sub/b.css';"), &opts);

    EMB_REQUIRE(root->nrules == 1);
    embi_css_sheet *b = root->rules[0].sheet;
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(b->nrules == 2);
    EMB_CHECK(b->rules[1].kind == EMBI_CSS_RULE_STYLE);

    embi_css_sheet *c = b->rules[0].sheet;
    EMB_REQUIRE(c != nullptr);
    EMB_REQUIRE(c->nrules == 3);
    EMB_CHECK(c->rules[0].kind == EMBI_CSS_RULE_IMPORT && c->rules[0].sheet == nullptr);
    EMB_CHECK(c->rules[1].kind == EMBI_CSS_RULE_IMPORT && c->rules[1].sheet == nullptr);
    EMB_CHECK(c->rules[2].kind == EMBI_CSS_RULE_STYLE);

    /* Calls: main's sub/b.css, b's c.css, and the two cycle attempts in c.css. */
    EMB_CHECK(st.calls == 4);
    EMB_CHECK(strcmp(st.bases[0], "http://h/a/main.css") == 0);
    EMB_CHECK(strcmp(st.bases[1], "http://h/a/sub/b.css") == 0);
    EMB_CHECK(strcmp(st.bases[2], "http://h/a/sub/c.css") == 0);
    EMB_CHECK(strcmp(st.bases[3], "http://h/a/sub/c.css") == 0);

    embi_css_sheet_unref(root);
}

/* ------------------------------------------------------------------ byte streams */

EMB_TEST(css_bytes_utf16_with_bom)
{
    static const unsigned char bytes[] = {0xFF, 0xFE, 'a', 0, '{', 0, '}', 0};
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, (embi_str){nullptr, 0},
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-16le") == 0);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_STYLE);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_utf16_big_endian_by_protocol)
{
    /* The protocol label selects UTF-16BE: "@x;" is an unknown at-rule, so no rules remain. */
    static const unsigned char bytes[] = {0, '@', 0, 'x', 0, ';'};
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, S("utf-16be"),
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-16be") == 0);
    EMB_CHECK(s->nrules == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_bom_beats_protocol)
{
    static const unsigned char bytes[] = {0xEF, 0xBB, 0xBF, 'a', '{', '}'};
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, S("ISO-8859-2"),
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-8") == 0);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_legacy_and_charset_rules)
{
    /* A legacy protocol label decodes. Leading space and case are normalized. */
    static const unsigned char bytes[] = {'@', 0xE9, ';'};
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, S(" Iso-8859-2"),
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "iso-8859-2") == 0);
    embi_css_sheet_unref(s);

    /* @charset naming UTF-16 is ignored, so the bytes are UTF-8. */
    static const unsigned char charset_utf16[] = "@charset \"UTF-16LE\"; a{}";
    s = embi_css_sheet_parse_bytes(charset_utf16, sizeof charset_utf16 - 1, (embi_str){nullptr, 0},
                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-8") == 0);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);

    /* A legacy @charset is honoured, and an environment label loses to it. */
    static const unsigned char charset_legacy[] = "@charset \"ISO-8859-5\"; a{}";
    s = embi_css_sheet_parse_bytes(charset_legacy, sizeof charset_legacy - 1,
                                   (embi_str){nullptr, 0}, S("ISO-8859-2"), nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "iso-8859-5") == 0);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_charset_utf16_stops_detection)
{
    /* CSS Syntax 3.2: a UTF-16 @charset label means UTF-8, and detection stops there, so the
     * environment label is not consulted. */
    static const unsigned char bytes[] = "@charset \"utf-16\"; a{}";
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes - 1, (embi_str){nullptr, 0},
                                                   S("ISO-8859-2"), nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-8") == 0);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);

    static const unsigned char be_bytes[] = "@charset \"UTF-16BE\"; a{}";
    s = embi_css_sheet_parse_bytes(be_bytes, sizeof be_bytes - 1, (embi_str){nullptr, 0},
                                   S("ISO-8859-2"), nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-8") == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_windows_1252_decodes)
{
    /* Aliases resolve to windows-1252, whose 0x80..0x9F row differs from Latin-1. */
    static const unsigned char bytes[] = {'a', '{', 0xE9, '}'};
    const char *enc = nullptr;
    embi_str text;
    embi_arena ar;
    embi_arena_init(&ar, 4096);
    EMB_REQUIRE(embi_css_decode_bytes(&ar, bytes, sizeof bytes, S("windows-1252"),
                                      (embi_str){nullptr, 0}, &text, &enc));
    EMB_CHECK(strcmp(enc, "windows-1252") == 0);
    EMB_CHECK(is_str(text, "a{\xC3\xA9}"));

    EMB_REQUIRE(embi_css_decode_bytes(&ar, bytes, sizeof bytes, S(" LATIN1 "),
                                      (embi_str){nullptr, 0}, &text, &enc));
    EMB_CHECK(strcmp(enc, "windows-1252") == 0);

    EMB_REQUIRE(embi_css_decode_bytes(&ar, bytes, sizeof bytes, (embi_str){nullptr, 0},
                                      S("us-ascii"), &text, &enc));
    EMB_CHECK(strcmp(enc, "windows-1252") == 0);

    static const unsigned char euro[] = {0x80, 0x81, 0x9F};
    EMB_REQUIRE(embi_css_decode_bytes(&ar, euro, sizeof euro, S("cp1252"), (embi_str){nullptr, 0},
                                      &text, &enc));
    EMB_CHECK(is_str(text, "\xE2\x82\xAC" "\xC2\x81" "\xC5\xB8"));

    /* ISO-8859-2 and -5 upper halves. */
    static const unsigned char l2[] = {0xA1, 0xFF};
    EMB_REQUIRE(embi_css_decode_bytes(&ar, l2, sizeof l2, S("latin2"), (embi_str){nullptr, 0},
                                      &text, &enc));
    EMB_CHECK(is_str(text, "\xC4\x84" "\xCB\x99"));
    static const unsigned char l5[] = {'a', 0xD0, 0xF0};
    EMB_REQUIRE(embi_css_decode_bytes(&ar, l5, sizeof l5, S("iso-8859-5"), (embi_str){nullptr, 0},
                                      &text, &enc));
    EMB_CHECK(is_str(text, "a\xD0\xB0" "\xE2\x84\x96"));
    embi_arena_free(&ar);

    /* A label that is not a WHATWG label at all falls through to UTF-8. */
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, S("kamoulox"),
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(strcmp(enc, "utf-8") == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_bytes_other_legacy_label_is_unsupported)
{
    static const unsigned char bytes[] = {'a', '{', 0xE9, '}'};
    const char *enc = nullptr;
    embi_css_sheet *s = embi_css_sheet_parse_bytes(bytes, sizeof bytes, S("koi8-r"),
                                                   (embi_str){nullptr, 0}, nullptr, &enc);
    EMB_CHECK(s == nullptr);
    EMB_CHECK(strcmp(enc, "koi8-r") == 0);
}

/* Serves "n<k>.css": each file imports n<k+1>.css three times, and the last is a plain rule. */
typedef struct chain_loader {
    int calls;
    int depth;
} chain_loader;

static bool chain_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                        const unsigned char **bytes, size_t *n)
{
    (void)base;
    chain_loader *cl = user;
    cl->calls++;
    int k = url.len > 1 ? atoi(url.ptr + 1) : 0;
    static char body[256];
    if (k >= cl->depth) {
        snprintf(body, sizeof body, "p { color: red }");
    } else {
        snprintf(body, sizeof body, "@import 'n%d.css'; @import 'n%d.css'; @import 'n%d.css';",
                 k + 1, k + 1, k + 1);
    }
    *resolved = url;
    *bytes = (const unsigned char *)body;
    *n = strlen(body);
    return true;
}

EMB_TEST(css_import_diamond_is_shared_and_fast)
{
    /* 3 imports per level, 30 levels deep: 3^30 sheets if every import were parsed. */
    chain_loader cl = {0, 30};
    embi_css_loader ld = {&cl, chain_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, {nullptr, 0}};
    uint64_t t0 = SDL_GetTicks();
    embi_css_sheet *s = embi_css_sheet_parse(S("@import 'n1.css'; @import 'n1.css';"), &opts);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(dt < 2000);
    EMB_CHECK(cl.calls < 1000);
    /* Identical imports share one parsed sheet. */
    EMB_REQUIRE(s->rules[0].sheet != nullptr);
    EMB_CHECK(s->rules[0].sheet == s->rules[1].sheet);
    embi_css_sheet *c1 = s->rules[0].sheet;
    EMB_REQUIRE(c1->nrules == 3);
    EMB_CHECK(c1->rules[0].sheet == c1->rules[1].sheet);
    embi_css_sheet_unref(s);
}

/* Every file imports two distinct children (no sharing possible): the total is capped. */
static bool tree_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                       const unsigned char **bytes, size_t *n)
{
    (void)base;
    int *calls = user;
    (*calls)++;
    static char body[128];
    snprintf(body, sizeof body, "@import '%.*s0'; @import '%.*s1';", (int)url.len, url.ptr,
             (int)url.len, url.ptr);
    *resolved = url;
    *bytes = (const unsigned char *)body;
    *n = strlen(body);
    return true;
}

static int count_sheets(const embi_css_sheet *s)
{
    int c = 1;
    for (uint32_t k = 0; k < s->nrules; k++) {
        if (s->rules[k].kind == EMBI_CSS_RULE_IMPORT && s->rules[k].sheet != nullptr) {
            c += count_sheets(s->rules[k].sheet);
        }
    }
    return c;
}

EMB_TEST(css_import_total_is_capped)
{
    int calls = 0;
    embi_css_loader ld = {&calls, tree_fetch};
    embi_css_parse_opts opts = {&ld, nullptr, {nullptr, 0}};
    uint64_t t0 = SDL_GetTicks();
    embi_css_sheet *s = embi_css_sheet_parse(S("@import 'x'; @import 'y';"), &opts);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_CHECK(dt < 5000);
    int total = count_sheets(s);
    EMB_CHECK(total > 100);
    EMB_CHECK(total <= 257);
    embi_css_sheet_unref(s);
}

EMB_TEST_MAIN()
