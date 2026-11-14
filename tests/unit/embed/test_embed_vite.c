/*
 * The Vite output of W8a (tests/apps/vite-vanilla/dist, built by tools/apps/build.sh) through the directory mount of
 * W7d: emb_runtime_mount_dir serves the folder as app://local/, and the app's module script, its CSS and its image asset
 * load through the handlers. Three checks:
 *   - the mount itself: files come back with their media types, a missing file is a 404, a path that climbs out of the
 *     folder or a host that is not mounted is refused (no network, no escape);
 *   - the document without a view: the module script runs (the heading and the counter are rendered by it), the image
 *     URL is resolved with import.meta.url, and nothing is logged as an error;
 *   - the view (offscreen, needs a GPU; skipped otherwise): the app runs with no script error, fetch('./data.json')
 *     renders the data, the counter increments and survives a reload of the view (localStorage on the storage of the
 *     runtime). This is the exit criterion of W7.
 */
#include "emb_test.h"

#include "base/base.h"
#include "embed/embed_int.h"
#include "embed/resource_int.h"
#include "embed/script_int.h"
#include "gpu/gpu.h"

#include <ember/ember_resource.h>

#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define VITE_DIST EMBER_SOURCE_DIR "/tests/apps/vite-vanilla/dist"

/* Counts the error lines and keeps the first one of them. */
typedef struct errlog {
    int errors;
    int warnings;
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
    } else if (level == EMB_LOG_WARN) {
        l->warnings++;
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

/* The loader of a document without a view: the runtime's transport (the mounted folder, file:, data:). */
static bool fetch_rt(void *user, embi_str url, uint8_t **data, size_t *len)
{
    return embi_rt_fetch(user, url, data, len);
}

/* ------------------------------------------------------------------ the mount */

EMB_TEST(vite_mount_serves_files_and_refuses_escapes)
{
    errlog log = {0};
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_cb;
    rd.log_user = &log;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(emb_runtime_mount_dir(rt, "app", "local", VITE_DIST));
    EMB_CHECK(!emb_runtime_mount_dir(rt, "file", "local", VITE_DIST));
    EMB_CHECK(!emb_runtime_mount_dir(rt, "app", "local", ""));

    uint8_t *data = nullptr;
    size_t len = 0;
    embi_sb mime = {nullptr, 0, 0};
    EMB_CHECK(embi_rt_load(rt, EMBI_STR("app://local/index.html"), &data, &len, &mime));
    EMB_CHECK(len > 0 && data != nullptr);
    EMB_CHECK(mime.len >= 9 && memcmp(mime.data, "text/html", 9) == 0);
    SDL_free(data);
    embi_sb_free(&mime);

    /* An asset is served with its extension's media type; "/" is the index page. */
    mime = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(embi_rt_load(rt, EMBI_STR("app://local/data.json"), &data, &len, &mime));
    EMB_CHECK(len > 0);
    SDL_free(data);
    embi_sb_free(&mime);
    EMB_CHECK(embi_rt_load(rt, EMBI_STR("app://local/"), &data, &len, nullptr));
    SDL_free(data);

    /* A missing file is a 404 (not a load), a climb out of the folder is refused, and a host that is not mounted is refused. */
    EMB_CHECK(!embi_rt_load(rt, EMBI_STR("app://local/nothere.js"), &data, &len, nullptr));
    EMB_CHECK(!embi_rt_load(rt, EMBI_STR("app://local/../secret.txt"), &data, &len, nullptr));
    EMB_CHECK(!embi_rt_load(rt, EMBI_STR("app://local/%2e%2e/secret.txt"), &data, &len, nullptr));
    EMB_CHECK(!embi_rt_load(rt, EMBI_STR("app://other/index.html"), &data, &len, nullptr));
    EMB_CHECK(log.errors == 0);
    emb_runtime_destroy(rt);
}

/* ------------------------------------------------------------------ the document without a view */

EMB_TEST(vite_document_runs_its_module_without_a_view)
{
    errlog log = {0};
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_cb;
    rd.log_user = &log;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(emb_runtime_mount_dir(rt, "app", "local", VITE_DIST));

    uint8_t *html = nullptr;
    size_t hlen = 0;
    EMB_REQUIRE(embi_rt_fetch(rt, EMBI_STR("app://local/index.html"), &html, &hlen));
    embi_doc_cb cb = {rt, fetch_rt, nullptr, nullptr};
    embi_doc *d = embi_doc_create(rt->text, rt->fonts, &cb);
    EMB_REQUIRE(d != nullptr);
    /* The base URL is the page's, so the module and its assets resolve under app://local/. */
    EMB_CHECK(embi_doc_load(d, (embi_str){(const char *)html, (uint32_t)hlen}, EMBI_STR("app://local/index.html"),
                            nullptr));
    evalres r = {0};
    EMB_CHECK(eval_doc(d, "document.querySelector('h1').textContent", &r));
    EMB_CHECK(strcmp(r.text, "Vite + Ember") == 0);
    EMB_CHECK(eval_doc(d, "document.getElementById('counter').textContent", &r));
    EMB_CHECK(strcmp(r.text, "count is 0") == 0);
    /* import.meta.url of the module resolves the logo under the mount. */
    EMB_CHECK(eval_doc(d, "document.querySelector('img.logo').src", &r));
    EMB_CHECK(strstr(r.text, "app://local/assets/logo-") != nullptr);
    EMB_CHECK(eval_doc(d, "document.getElementById('counter').className", &r));
    EMB_CHECK(strstr(r.text, "_counter_") != nullptr);
    EMB_CHECK(log.errors == 0);
    if (log.errors > 0) {
        SDL_Log("first error: %s", log.first);
    }
    embi_doc_destroy(d);
    SDL_free(html);
    emb_runtime_destroy(rt);
}

/* ------------------------------------------------------------------ the view (exit criterion) */

/* Runs the view's loop for ms of wall time. */
static void pump_for(emb_view *v, uint64_t ms)
{
    const uint64_t t0 = SDL_GetTicksNS();
    while (SDL_GetTicksNS() - t0 < ms * 1000000u) {
        (void)emb_view_pump(v);
        SDL_Delay(1);
    }
}

static bool eval_view(emb_view *v, const char *js, evalres *r)
{
    SDL_memset(r, 0, sizeof *r);
    return emb_view_eval(v, js, SDL_strlen(js), eval_cb, r) && r->ok;
}

EMB_TEST(vite_vanilla_runs_in_a_view_and_keeps_its_counter)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        SDL_Quit();
        EMB_SKIP("no GPU device");
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    errlog log = {0};
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_cb;
    rd.log_user = &log;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(emb_runtime_mount_dir(rt, "app", "local", VITE_DIST));
    /* A storage folder of this run, so the counter starts at zero and the reload is the test's own. */
    char *pref = SDL_GetPrefPath("Ember", "tests");
    char storage[1024];
    SDL_snprintf(storage, sizeof storage, "%svite-vanilla-%llu", pref != nullptr ? pref : "",
                 (unsigned long long)SDL_GetTicksNS());
    EMB_CHECK(emb_runtime_set_storage_dir(rt, storage));
    SDL_free(pref);

    emb_view_desc vd;
    SDL_memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 800;
    vd.height = 600;
    vd.scale = 1.0f;
    vd.device = dev;
    emb_view *v = emb_view_create(rt, &vd);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "app://local/index.html"));
    pump_for(v, 300);
    evalres r;
    EMB_CHECK(eval_view(v, "document.querySelector('h1').textContent", &r));
    EMB_CHECK(strcmp(r.text, "Vite + Ember") == 0);
    /* fetch('./data.json') through the view's transport renders the data. */
    EMB_CHECK(eval_view(v, "document.getElementById('data').textContent", &r));
    EMB_CHECK(strcmp(r.text, "Loaded from data.json (3 items)") == 0);
    /* A click counts, and the count is stored in localStorage. */
    EMB_CHECK(eval_view(v, "document.getElementById('counter').click(); localStorage.getItem('vite-vanilla-count')", &r));
    EMB_CHECK(strcmp(r.text, "1") == 0);
    emb_view_destroy(v);

    /* The reload: a new view of the same runtime and storage keeps the count. */
    v = emb_view_create(rt, &vd);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "app://local/index.html"));
    pump_for(v, 300);
    EMB_CHECK(eval_view(v, "document.getElementById('counter').textContent", &r));
    EMB_CHECK(strcmp(r.text, "count is 1") == 0);
    EMB_CHECK(log.errors == 0);
    if (log.errors > 0) {
        SDL_Log("first error: %s", log.first);
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    SDL_RemovePath(storage);
    embi_gpu_destroy(g);
    SDL_Quit();
}

EMB_TEST_MAIN()
