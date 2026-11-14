// fuzz-modules: js jslex num base
/* JavaScript execution with limits. Input: byte 0 = mode, rest = source text. The engine has an
 * interrupt hook that runs at every call and loop back-edge, so the harness counts those as steps
 * and stops the script at a step budget, at a wall-clock budget, or when the live heap of the
 * process grows past a cap (embi_mem_live is the allocator's count of every engine byte). The stop
 * unwinds without running script handlers, which is the engine's own mechanism, so a stopped script
 * is not a finding. Hangs inside one native builtin (no back-edge, no call) are findings: the
 * libFuzzer timeout reports them.
 *
 * Modes (byte 0 & 3): 0 classic script, 1 script in strict mode, 2 ES module (the loader hook serves
 * the source for the entry and a small second module for any import), 3 script, then the job queue
 * (promises, async functions) is drained under the same limits (the queue is drained in every mode). Bits 2 and 4 together: collect garbage
 * every 4 KB of allocation (stress mode for stale pointers). Bit 3: also run the result through JSON.stringify
 * and String(). */
#include "fuzz_common.h"

#include "js/js.h"

#include <SDL3/SDL_timer.h>

#define STEP_BUDGET 400000u
#define HEAP_CAP ((size_t)96 << 20)
#define WALL_MS 2000

typedef struct limits {
    unsigned steps;
    size_t base_live;
    uint64_t t0_ms;
    bool stopped;
    const uint8_t *src;
    size_t src_len;
} limits;

static bool interrupt_cb(embi_js_runtime *rt, void *user)
{
    (void)rt;
    limits *l = user;
    if (++l->steps > STEP_BUDGET) {
        l->stopped = true;
        return true;
    }
    if ((l->steps & 1023u) == 0) {
        if (SDL_GetTicks() - l->t0_ms > WALL_MS) {
            l->stopped = true;
            return true;
        }
    }
    if ((l->steps & 63u) == 0 && embi_mem_live() > l->base_live + HEAP_CAP) {
        l->stopped = true;
        return true;
    }
    return false;
}

static void module_load(embi_js_runtime *rt, void *user, uint32_t request, const char *referrer,
                        size_t referrer_len, const char *specifier, size_t specifier_len,
                        const char *type)
{
    limits *l = user;
    (void)referrer;
    (void)referrer_len;
    if (type != nullptr) {
        embi_js_module_loaded(rt, request, specifier, specifier_len, "{\"a\":[1,2,{\"b\":null}]}", 23);
        return;
    }
    if (specifier_len == 5 && memcmp(specifier, "entry", 5) == 0) {
        embi_js_module_loaded(rt, request, "entry", 5, (const char *)l->src, l->src_len);
    } else if (specifier_len > 0 && (specifier[0] & 1)) {
        static const char dep[] = "export const x = 1; export default function f() { return 2; }\n"
                                  "export let y = 3; export * from 'entry';";
        embi_js_module_loaded(rt, request, specifier, specifier_len, dep, sizeof dep - 1);
    } else {
        embi_js_module_load_failed(rt, request, "no such module");
    }
}

static void import_meta(embi_js_runtime *rt, embi_js_value meta, const char *key, size_t key_len,
                        void *user)
{
    (void)rt;
    (void)meta;
    (void)key;
    (void)key_len;
    (void)user;
}

static void drain(embi_js_runtime *rt, limits *l)
{
    for (unsigned i = 0; i < 64 && !l->stopped; i++) {
        embi_js_run_jobs(rt);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    limits l = {0, embi_mem_live(), SDL_GetTicks(), false, in.p, in.n};

    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = embi_js_realm_new(rt);
    if ((mode & 0x14) == 0x14) {
        embi_js_set_gc_params(rt, 4096, 0.0);
    }
    embi_js_set_interrupt(rt, interrupt_cb, &l);
    embi_js_set_limits(rt, 2000, (size_t)512 << 10);
    embi_js_set_module_loader(rt, module_load, &l);
    embi_js_set_import_meta_hook(rt, import_meta, &l);

    embi_js_value v;
    if ((mode & 3) == 2) {
        v = embi_js_module_eval(r, "entry", 5);
    } else {
        v = embi_js_eval(r, (const char *)in.p, in.n, "fuzz.js", (mode & 3) == 1 ? EMBI_JS_EVAL_STRICT : 0);
    }
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
    } else if (mode & 8) {
        embi_js_string *s = embi_js_to_string(rt, v);
        if (s == nullptr) {
            embi_js_exception_clear(rt);
        }
    }
    drain(rt, &l);
    if (mode & 8) {
        static const char tail[] = "try { JSON.stringify(globalThis); Object.keys(globalThis); } catch (e) {}";
        l.steps = 0;
        l.stopped = false;
        embi_js_value w = embi_js_eval(r, tail, sizeof tail - 1, "tail.js", 0);
        if (w == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        }
    }
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    FUZZ_GUARD_END();
    return 0;
}
