#include "emb_test.h"

#include "js/js.h"

#include <SDL3/SDL_thread.h>
#include <string.h>

/* Math.random on several threads at once. Each thread owns its runtime and realm and draws in a
 * loop; the generator state is thread-local, so the draws must stay in range, vary, and average
 * near 0.5 on every thread. Run under TSan or ASan this also shows that no state is shared. */

#define THREADS 4
#define DRAWS 20000

struct worker {
    bool ok;
};

static bool worker_eval_true(embi_js_runtime *rt, embi_js_realm *r, const char *src)
{
    embi_js_value v = embi_js_eval(r, src, strlen(src), "math_threads.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        return false;
    }
    return v == EMBI_JS_TRUE;
}

static int worker_main(void *data)
{
    struct worker *w = data;
    w->ok = false;
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = rt ? embi_js_realm_new(rt) : nullptr;
    if (!r) {
        embi_js_runtime_free(rt);
        return 1;
    }
    char src[512];
    SDL_snprintf(src, sizeof src,
                 "var ok = true, first = Math.random(), diff = false, sum = 0;"
                 "for (var i = 0; i < %d; i++) {"
                 "  var x = Math.random(); ok = ok && x >= 0 && x < 1; diff = diff || x !== first;"
                 "  sum += x;"
                 "}"
                 "ok && diff && sum / %d > 0.4 && sum / %d < 0.6",
                 DRAWS, DRAWS, DRAWS);
    w->ok = worker_eval_true(rt, r, src);
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    return w->ok ? 0 : 1;
}

EMB_TEST(math_random_concurrent_runtimes)
{
    SDL_Thread *threads[THREADS];
    struct worker results[THREADS];
    for (int i = 0; i < THREADS; i++) {
        results[i].ok = false;
        threads[i] = SDL_CreateThread(worker_main, "math_random", &results[i]);
        EMB_CHECK(threads[i] != nullptr);
    }
    for (int i = 0; i < THREADS; i++) {
        int status = -1;
        if (threads[i]) {
            SDL_WaitThread(threads[i], &status);
        }
        EMB_CHECK(status == 0);
        EMB_CHECK(results[i].ok);
    }
}

EMB_TEST_MAIN()
