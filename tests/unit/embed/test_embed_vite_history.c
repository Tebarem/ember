/*
 * history.state through the structured serializer (W7c carry-over, done in W7d, src/bindings/impl_window.c): pushState and
 * replaceState store a copy of the state (not the live value), history.state and the popstate event hand out a value
 * made from those bytes, and a value that cannot be cloned makes pushState throw DataCloneError and leaves the history as
 * it was. Runs on a document without a view, with the URL app://local/index.html as the base.
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

#define HIST_HTML "<!doctype html><html><head><title>h</title></head><body></body></html>"

#define EXPECT_EVAL(d, js, want)                                                 \
    do {                                                                         \
        evalres r_;                                                              \
        EMB_CHECK(eval_doc((d), (js), &r_));                                     \
        EMB_CHECK(strcmp(r_.text, (want)) == 0);                                 \
        if (strcmp(r_.text, (want)) != 0) {                                      \
            SDL_Log("eval %s: got \"%s\", want \"%s\"", (js), r_.text, (want));  \
        }                                                                        \
    } while (0)

static void history_scenario(void)
{
    errlog log = {0};
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_cb;
    rd.log_user = &log;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    embi_doc_cb cb = {rt, fetch_rt, nullptr, nullptr};
    embi_doc *d = embi_doc_create(rt->text, rt->fonts, &cb);
    EMB_REQUIRE(d != nullptr);
    EMB_REQUIRE(embi_doc_load(d, (embi_str){HIST_HTML, (uint32_t)SDL_strlen(HIST_HTML)},
                              EMBI_STR("app://local/index.html"), nullptr));

    /* An object and a Date round-trip, and the stored state is a copy: mutating the original changes nothing. */
    EXPECT_EVAL(d, "var o = {a: 1, d: new Date(5)}; history.pushState(o, '', '/page2'); o.a = 2; 'pushed'", "pushed");
    EXPECT_EVAL(d, "history.state.a", "1");
    EXPECT_EVAL(d, "history.state !== o ? 'distinct' : 'same'", "distinct");
    EXPECT_EVAL(d, "history.state.d instanceof Date ? 'date' : 'other'", "date");
    EXPECT_EVAL(d, "String(history.state.d.getTime())", "5");
    EXPECT_EVAL(d, "history.state.d !== o.d ? 'copy' : 'shared'", "copy");
    /* The same value is handed out on every read (the copy is made once per entry). */
    EXPECT_EVAL(d, "history.state === history.state ? 'stable' : 'fresh'", "stable");

    /* replaceState stores a copy too. */
    EXPECT_EVAL(d, "var r = {k: [1, 2]}; history.replaceState(r, ''); r.k.push(3); history.state.k.length", "2");

    /* A value that cannot be cloned throws DataCloneError and leaves the state as it was. */
    EXPECT_EVAL(d, "try { history.pushState(function () {}, ''); 'no' } catch (e) { e.name }", "DataCloneError");
    EXPECT_EVAL(d, "try { history.replaceState(Symbol('x'), ''); 'no' } catch (e) { e.name }", "DataCloneError");
    EXPECT_EVAL(d, "history.state.k.length", "2");
    EXPECT_EVAL(d, "location.pathname", "/page2");

    /* popstate hands out a copy of the entry's state, the same value history.state reads. */
    EXPECT_EVAL(d, "var s0 = {v: 1, d: new Date(9)}; history.replaceState(s0, ''); history.pushState({w: 2}, '', '/b'); "
                   "window.got = null; window.addEventListener('popstate', function (e) { window.got = e.state; }); "
                   "history.back(); 'back'", "back");
    EXPECT_EVAL(d, "window.got === null ? 'none' : String(window.got.v)", "1");
    EXPECT_EVAL(d, "window.got !== s0 ? 'copy' : 'shared'", "copy");
    EXPECT_EVAL(d, "window.got.d.getTime() === 9 ? 'date' : 'other'", "date");
    EXPECT_EVAL(d, "window.got === history.state ? 'same' : 'different'", "same");
    EXPECT_EVAL(d, "location.pathname", "/page2");
    EMB_CHECK(log.errors == 0);
    if (log.errors > 0) {
        SDL_Log("first error: %s", log.first);
    }
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

EMB_TEST(history_state_is_a_copy_of_the_structured_clone)
{
    /* The first run warms the process-wide tables (atoms, the engine caches); the second run must give the same memory
     * back, so nothing it allocates outlives the runtime (ASan on Windows has no leak check). */
    history_scenario();
    const size_t mem0 = embi_mem_live();
    history_scenario();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
