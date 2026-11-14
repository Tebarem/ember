/*
 * Constructable stylesheets (W7d, src/bindings/impl_cssom_sheet.c, doc.c): new CSSStyleSheet, replaceSync, cssRules and
 * document.adoptedStyleSheets. The adopted sheets join the document's cascade after its own <style> sheets, and both
 * computed style and layout read them (a width set by an adopted rule shows in getBoundingClientRect). A change of the
 * array or a replaceSync of an adopted sheet is seen at once, and a new load of the document adopts nothing.
 * The document is made without a view (the realm and the engines are the same as in a view).
 */
#include "emb_test.h"

#include "base/base.h"
#include "embed/embed_int.h"
#include "embed/resource_int.h"
#include "embed/script_int.h"

#include <ember/ember_resource.h>

#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct errlog {
    int errors;
    char first[512];
} errlog;

static void log_cb(void *user, emb_log_level level, const char *text)
{
    errlog *l = user;
    if (level == EMB_LOG_ERROR) {
        if (l->errors == 0) {
            SDL_snprintf(l->first, sizeof l->first, "%s", text);
        }
        l->errors++;
    }
}

typedef struct evalres {
    bool ok;
    char text[512];
} evalres;

static void eval_cb(void *user, bool ok, const char *text, size_t len)
{
    evalres *r = user;
    r->ok = ok;
    size_t n = len < sizeof r->text - 1 ? len : sizeof r->text - 1;
    SDL_memcpy(r->text, text, n);
    r->text[n] = '\0';
}

static bool eval_doc(embi_doc *d, const char *js, evalres *r)
{
    SDL_memset(r, 0, sizeof *r);
    return embi_script_eval(embi_doc_script(d), js, strlen(js), eval_cb, r) && r->ok;
}

static bool fetch_rt(void *user, embi_str url, uint8_t **data, size_t *len)
{
    return embi_rt_fetch(user, url, data, len);
}

#define SHEET_HTML "<!doctype html><html><head><style>#x { color: rgb(0, 0, 255); }</style></head>" \
                   "<body><p id=\"x\">hello</p></body></html>"

/* Checks that the document shows what the test expects for a given JS expression. */
#define EXPECT_EVAL(d, js, want)                                        \
    do {                                                                \
        evalres r_;                                                     \
        EMB_CHECK(eval_doc((d), (js), &r_));                            \
        EMB_CHECK(strcmp(r_.text, (want)) == 0);                        \
        if (strcmp(r_.text, (want)) != 0) {                             \
            SDL_Log("eval %s: got \"%s\", want \"%s\"", (js), r_.text, (want)); \
        }                                                               \
    } while (0)

static embi_doc *make_doc(emb_runtime *rt, const char *html)
{
    embi_doc_cb cb = {rt, fetch_rt, nullptr, nullptr};
    embi_doc *d = embi_doc_create(rt->text, rt->fonts, &cb);
    if (d != nullptr) {
        EMB_CHECK(embi_doc_load(d, (embi_str){html, (uint32_t)SDL_strlen(html)}, EMBI_STR("app://local/index.html"),
                                nullptr));
    }
    return d;
}

static emb_runtime *make_runtime(errlog *log)
{
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_cb;
    rd.log_user = log;
    return emb_runtime_create(&rd);
}

static void sheet_scenario(void)
{
    errlog log = {0};
    emb_runtime *rt = make_runtime(&log);
    EMB_REQUIRE(rt != nullptr);
    embi_doc *d = make_doc(rt, SHEET_HTML);
    EMB_REQUIRE(d != nullptr);

    /* Before the adoption: the document's own sheet decides. */
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(0, 0, 255)");

    /* A constructed sheet has its rules, and it is not in the cascade until it is adopted. */
    EXPECT_EVAL(d, "var s = new CSSStyleSheet(); s.replaceSync('#x { color: rgb(255, 0, 0); width: 123px; }'); "
                   "s.cssRules.length", "1");
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(0, 0, 255)");

    /* Adopted: the sheet comes after the document's sheets, so its rule wins; layout reads its width. */
    EXPECT_EVAL(d, "document.adoptedStyleSheets = [s]; document.adoptedStyleSheets.length", "1");
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(255, 0, 0)");
    EXPECT_EVAL(d, "document.getElementById('x').getBoundingClientRect().width", "123");

    /* replaceSync of an adopted sheet is seen by the cascade. */
    EXPECT_EVAL(d, "s.replaceSync('#x { color: rgb(0, 128, 0); }'); "
                   "getComputedStyle(document.getElementById('x')).color", "rgb(0, 128, 0)");
    EXPECT_EVAL(d, "document.getElementById('x').getBoundingClientRect().width === 123", "false");

    /* The array is replaced: the sheet goes away and the document's own rule decides again. */
    EXPECT_EVAL(d, "document.adoptedStyleSheets = []; document.adoptedStyleSheets.length", "0");
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(0, 0, 255)");

    /* Only CSSStyleSheet objects can be adopted. */
    EXPECT_EVAL(d, "try { document.adoptedStyleSheets = [{}]; 'none' } catch (e) { e.name }", "TypeError");

    /* replace() resolves to the sheet itself, with its rules. */
    EXPECT_EVAL(d, "var p = new CSSStyleSheet(); var got = null; p.replace('p { margin: 0 }').then(function (x) "
                   "{ got = x; }); 'queued'", "queued");
    EXPECT_EVAL(d, "got === p ? 'same' : 'other'", "same");
    EXPECT_EVAL(d, "p.cssRules.length", "1");
    EMB_CHECK(log.errors == 0);
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

static void clear_scenario(void)
{
    errlog log = {0};
    emb_runtime *rt = make_runtime(&log);
    EMB_REQUIRE(rt != nullptr);
    embi_doc *d = make_doc(rt, SHEET_HTML);
    EMB_REQUIRE(d != nullptr);
    EXPECT_EVAL(d, "var s = new CSSStyleSheet(); s.replaceSync('#x { color: rgb(255, 0, 0); }'); "
                   "document.adoptedStyleSheets = [s]; 'ok'", "ok");
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(255, 0, 0)");

    /* A new document: its own sheets only, and an empty adopted list. */
    EMB_CHECK(embi_doc_load(d, (embi_str){SHEET_HTML, (uint32_t)SDL_strlen(SHEET_HTML)},
                            EMBI_STR("app://local/index.html"), nullptr));
    EXPECT_EVAL(d, "document.adoptedStyleSheets.length", "0");
    EXPECT_EVAL(d, "getComputedStyle(document.getElementById('x')).color", "rgb(0, 0, 255)");
    EMB_CHECK(log.errors == 0);
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

EMB_TEST(vite_sheet_adopted_rules_reach_style_and_layout)
{
    /* The first run warms the process-wide tables (atoms, the engine caches); the second run must give the same memory
     * back, so nothing it allocates outlives the runtime (ASan on Windows has no leak check). */
    sheet_scenario();
    const size_t mem0 = embi_mem_live();
    sheet_scenario();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(vite_sheet_adoption_is_cleared_by_a_new_load)
{
    /* The first run warms the process-wide tables (atoms, the engine caches); the second run must give the same memory
     * back, so nothing it allocates outlives the runtime (ASan on Windows has no leak check). */
    clear_scenario();
    const size_t mem0 = embi_mem_live();
    clear_scenario();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
