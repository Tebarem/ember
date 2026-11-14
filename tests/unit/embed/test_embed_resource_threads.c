/*
 * Thread-safe completion of asynchronous resource handlers (W7c): many requests of a script's fetch calls are completed by
 * worker threads at the same time; every promise must settle on the view's own thread (the thread that updates the view), and
 * a completion that arrives after the view is destroyed must be dropped without touching freed memory. Run under ASan this is
 * the data-race and use-after-free check. Needs a GPU device (software Vulkan); without one the test skips.
 */
#include "emb_test.h"

#include "base/clock.h"
#include "embed/embed_int.h"

#include <ember/ember_resource.h>
#include <ember/ember_script.h>

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#define NREQ 24

typedef struct pending_req {
    emb_request *req;
    char url[64];
} pending_req;

typedef struct handler_env {
    SDL_Mutex *mu;
    pending_req reqs[NREQ];
    int n;
    atomic_int cancelled;
} handler_env;

typedef struct console_env {
    SDL_ThreadID view_thread;
    int lines;
    int wrong_thread;
    char text[1024];
    size_t len;
} console_env;

static void on_start(void *user, const emb_request_info *info)
{
    handler_env *h = user;
    SDL_LockMutex(h->mu);
    if (h->n < NREQ) {
        pending_req *p = &h->reqs[h->n++];
        p->req = info->request;
        size_t l = info->url_len < sizeof p->url - 1 ? info->url_len : sizeof p->url - 1;
        memcpy(p->url, info->url, l);
        p->url[l] = '\0';
    }
    SDL_UnlockMutex(h->mu);
}

static void on_cancel(void *user, emb_request *req)
{
    (void)req;
    atomic_fetch_add(&((handler_env *)user)->cancelled, 1);
}

static void on_console(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    console_env *c = user;
    if (SDL_GetCurrentThreadID() != c->view_thread) {
        c->wrong_thread++;
    }
    c->lines++;
    if (c->len + len + 1 < sizeof c->text) {
        memcpy(c->text + c->len, text, len);
        c->len += len;
        c->text[c->len++] = ';';
        c->text[c->len] = '\0';
    }
}

/* One worker completes a slice of the requests with a body that names the request. */
typedef struct worker {
    handler_env *h;
    int first, count;
    int delay_ms;
} worker;

static int worker_main(void *arg)
{
    worker *w = arg;
    for (int i = w->first; i < w->first + w->count; i++) {
        SDL_Delay((Uint32)w->delay_ms);
        char body[80];
        int n = SDL_snprintf(body, sizeof body, "body:%.60s", w->h->reqs[i].url);
        emb_response resp = {.size = sizeof resp, .status = 200, .mime = "text/plain", .data = (const uint8_t *)body,
                             .len = (size_t)n};
        emb_resource_complete(w->h->reqs[i].req, &resp);
    }
    return 0;
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

static emb_view *make_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc desc = {
        .size = sizeof desc,
        .mode = EMB_VIEW_OFFSCREEN,
        .width = 64,
        .height = 48,
        .scale = 1.0f,
        .device = dev,
        .scheme = EMB_SCHEME_LIGHT,
    };
    return emb_view_create(rt, &desc);
}

static const char *k_page =
    "<!doctype html><body><script>"
    "var got = 0;"
    "for (var i = 0; i < 24; i++) { (function (i) {"
    "  fetch('res://host/item' + i).then(function (r) { return r.text(); })"
    "    .then(function (t) { got++; console.log(t === 'body:res://host/item' + i ? 'ok' : 'bad:' + t); }); })(i); }"
    "</script></body>";

#if EMB_TEST_ASAN
#define SCALE_MS 4
#else
#define SCALE_MS 1
#endif

EMB_TEST(async_completions_from_threads_settle_on_the_view_thread)
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
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    handler_env h = {0};
    h.mu = SDL_CreateMutex();
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "res", on_start, on_cancel, &h));
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);
    console_env ce = {0};
    ce.view_thread = SDL_GetCurrentThreadID();
    emb_view_set_console_callback(v, on_console, &ce);
    EMB_REQUIRE(emb_view_load_html(v, k_page, strlen(k_page), "http://t.test/index.html"));
    for (int i = 0; i < 50 && h.n < NREQ; i++) {
        (void)render_now(dev, v);
        SDL_Delay(2);
    }
    EMB_REQUIRE(h.n == NREQ);

    /* Four workers complete six requests each, concurrently with the view's updates. */
    worker ws[4];
    SDL_Thread *th[4];
    for (int i = 0; i < 4; i++) {
        ws[i] = (worker){&h, i * 6, 6, (i + 1) * SCALE_MS};
        th[i] = SDL_CreateThread(worker_main, "completer", &ws[i]);
        EMB_REQUIRE(th[i] != nullptr);
    }
    for (int i = 0; i < 4000 && ce.lines < NREQ; i++) {
        (void)render_now(dev, v);
        SDL_Delay(1);
    }
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(th[i], nullptr);
    }
    for (int i = 0; i < 50 && ce.lines < NREQ; i++) {
        (void)render_now(dev, v);
    }
    EMB_CHECK_EQ_INT(ce.lines, NREQ);
    EMB_CHECK_EQ_INT(ce.wrong_thread, 0);
    EMB_CHECK(strstr(ce.text, "bad:") == nullptr);
    char result[32] = "";
    (void)result;

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    SDL_DestroyMutex(h.mu);
    embi_gpu_destroy(g);
    SDL_Quit();
}

EMB_TEST(completion_after_view_destroy_is_dropped)
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
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    handler_env h = {0};
    h.mu = SDL_CreateMutex();
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "res", on_start, on_cancel, &h));
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);
    console_env ce = {0};
    ce.view_thread = SDL_GetCurrentThreadID();
    emb_view_set_console_callback(v, on_console, &ce);
    EMB_REQUIRE(emb_view_load_html(v, k_page, strlen(k_page), "http://t.test/index.html"));
    for (int i = 0; i < 50 && h.n < NREQ; i++) {
        (void)render_now(dev, v);
        SDL_Delay(2);
    }
    EMB_REQUIRE(h.n == NREQ);

    /* The workers start completing while the main thread destroys the view and the runtime. */
    worker ws[4];
    SDL_Thread *th[4];
    for (int i = 0; i < 4; i++) {
        ws[i] = (worker){&h, i * 6, 6, 1};
        th[i] = SDL_CreateThread(worker_main, "completer", &ws[i]);
        EMB_REQUIRE(th[i] != nullptr);
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(th[i], nullptr);
    }
    EMB_CHECK_EQ_INT(ce.wrong_thread, 0);
    SDL_DestroyMutex(h.mu);
    embi_gpu_destroy(g);
    SDL_Quit();
}

/* Completions queued on the view's loop and never pumped: destroying the view must release every request (the handler's
 * reference), with nothing left allocated. */
static void queued_scenario(SDL_GPUDevice *dev)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    handler_env h = {0};
    h.mu = SDL_CreateMutex();
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "res", on_start, on_cancel, &h));
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page, strlen(k_page), "http://t.test/index.html"));
    for (int i = 0; i < 50 && h.n < NREQ; i++) {
        (void)render_now(dev, v);
        SDL_Delay(2);
    }
    EMB_REQUIRE(h.n == NREQ);
    /* Complete every request from the main thread and do not pump: each finishing step sits in the loop's queue. */
    for (int i = 0; i < NREQ; i++) {
        static const char body[] = "queued";
        emb_response resp = {.size = sizeof resp, .status = 200, .mime = "text/plain", .data = (const uint8_t *)body,
                             .len = sizeof body - 1};
        emb_resource_complete(h.reqs[i].req, &resp);
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    SDL_DestroyMutex(h.mu);
}

EMB_TEST(queued_completions_are_released_when_the_view_is_destroyed)
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
    queued_scenario(dev); /* warms process-wide caches (fonts, thread-local state) */
    size_t base = embi_mem_live();
    queued_scenario(dev);
    queued_scenario(dev);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
    embi_gpu_destroy(g);
    SDL_Quit();
}

/* A v1 document load of an asynchronous handler that completes from a worker thread while start is still running: the
 * request is on no list, so the two threads share nothing but the reference count. Run under ASan or TSan. */
typedef struct sync_env {
    SDL_Thread *th;
    int iter;
    atomic_int done;
} sync_env;

static int sync_worker(void *arg)
{
    emb_request *req = arg;
    emb_response resp = {.size = sizeof resp, .status = 200, .mime = "text/plain", .data = (const uint8_t *)"v1", .len = 2};
    emb_resource_complete(req, &resp);
    return 0;
}

static void sync_start(void *user, const emb_request_info *info)
{
    sync_env *e = user;
    e->th = SDL_CreateThread(sync_worker, "v1-completer", info->request);
    for (int i = 0; i < e->iter % 7; i++) {
        SDL_Delay(0); /* the worker completes during start, after it, or in between */
    }
}

EMB_TEST(v1_load_of_async_handler_completing_on_a_worker_is_race_free)
{
    /* The first runtime of a process fills the atom table (its UA sheet prefetch interns names, FP2b) and keeps it. */
    emb_runtime_destroy(emb_runtime_create(nullptr));
    size_t base = embi_mem_live();
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    sync_env e = {0};
    EMB_REQUIRE(emb_runtime_set_resource_handler_async(rt, "res", sync_start, nullptr, &e));
    for (int i = 0; i < 300; i++) {
        e.iter = i;
        e.th = nullptr;
        uint8_t *data = nullptr;
        size_t len = 0;
        bool ok = embi_rt_fetch(rt, EMBI_STR("res://host/x"), &data, &len);
        /* The v1 caller cannot wait: the load fails, or (when the worker was inline, which it cannot be) succeeds. */
        if (ok) {
            SDL_free(data);
        }
        if (e.th != nullptr) {
            SDL_WaitThread(e.th, nullptr);
        }
    }
    emb_runtime_destroy(rt);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

EMB_TEST_MAIN()
