/*
 * Ember host for the shared JS workloads. The compare harness
 * (tools/bench/js/compare.sh) builds one concatenated script per workload, the same file that
 * QuickJS-ng runs, and passes it here:
 *     build/<dir>/tools/bench/bench_js_suite build/<dir>/js/<name>.bench.js
 * In a build with EMBI_JS_OPSTATS the op statistics (TSV: single/pair/triple lines, see
 * js/interp.h) are written at exit to the file after an optional "--opstats FILE", else to stderr.
 * The host installs the two globals the driver uses, print and performance.now (SDL_GetTicksNS),
 * evaluates the file as a script, drains the job queue and exits. print writes its arguments,
 * joined by spaces, and a newline to stdout. The engine's memory statistics go to stderr at exit,
 * for diagnosis; the harness does not parse them.
 * Exit status is 0 when the script ran to completion, 1 when it threw or could not be read, and 2
 * on a usage error or a bad environment value.
 *
 * Environment (memory experiments only; unset means the runtime's defaults from heap.h):
 *   EMBER_GC_GENERATIONAL=0  run without generational mode.
 *   EMBER_GC_FLOOR=N         allocation budget floor in bytes, 1..1073741824.
 *   EMBER_GC_FACTOR=F        cap on the heap growth factor, a number in (1, 8].
 */

#include "base/base.h"
#include "js/heap.h"
#include "js/js.h"
#include "js/interp.h"
#include "js/bytecode.h"
#include "js/ic.h"
#include "js/compiler.h"
#include "js/object.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* print(...args): the string conversion of each argument, separated by spaces, then a newline. */
static embi_js_value native_print(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_sb sb = {nullptr, 0, 0};
    for (int i = 0; i < argc; i++) {
        embi_js_string *s = embi_js_to_string(r->rt, argv[i]);
        if (!s) {
            embi_sb_free(&sb);
            return EMBI_JS_EXCEPTION;
        }
        if (i > 0) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_js_str_append_utf8(s, &sb);
    }
    embi_sb_append_char(&sb, '\n');
    fputs(embi_sb_cstr(&sb), stdout);
    fflush(stdout);
    embi_sb_free(&sb);
    return EMBI_JS_UNDEFINED;
}

/* performance.now(): milliseconds since an arbitrary start, with a fraction. */
static embi_js_value native_now(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_number((double)SDL_GetTicksNS() / 1e6);
}

/* gc(): one full collection (J5n1, the live-memory runs; driver.js calls it at the end of a run).
 * J5n2: the collection runs when the script has returned, not inside the call. The collector scans the
 * native stack conservatively, and the interpreter's C frame, which lives for the whole script, keeps
 * stale words in dead slots: one of them pinned a whole replaced DOM tree (1.4 MB, preact_diff) in a
 * build whose frame layout differed by a few bytes. Nothing runs after driver.js calls gc(), so the
 * live figure is the same set of objects, without that noise. */
static bool g_gc_requested;

static embi_js_value native_gc(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)r;
    g_gc_requested = true;
    return EMBI_JS_UNDEFINED;
}

/* J5n2 census: external bytes by owner and part (EMBER_CENSUS=1). */
typedef struct census_ext {
    size_t hist[12], slots, elems, rare, closures, bc, consts, other_code, ics, ic_keys, nfn, nobj, narr, nctor, nplain, ncl_up;
} census_ext;

static void census_visit(const embi_js_cell *cell, void *user)
{
    census_ext *x = user;
    if (cell->type == EMBI_JS_CELL_OBJECT) {
        const embi_js_object *o = (const embi_js_object *)cell;
        x->nobj++;
        if (o->klass == &embi_js_class_closure) {
            x->nfn++;
            x->nctor += (o->hdr.flags & EMBI_JS_OBJ_CONSTRUCTOR) != 0;
            x->ncl_up += ((const embi_js_closure *)o->internal)->nupvals;
        } else if (o->klass == &embi_js_class_array) {
            x->narr++;
        } else if (o->klass == &embi_js_class_object) {
            x->nplain++;
            x->hist[o->shape->count > 11 ? 11 : o->shape->count]++;
        }
        if (o->klass && o->internal) {
            /* function objects carry a closure record; count them by the code pointer test below */
        }
        x->slots += (size_t)o->slot_cap * 8u;
        if (o->elements) {
            x->elems += 8u + (size_t)o->elements_len * 8u;
        }
        if (o->rare) {
            x->rare += sizeof *o->rare;
        }
    } else if (cell->type == EMBI_JS_CELL_CODE) {
        const embi_js_code *c = (const embi_js_code *)cell;
        x->bc += c->bc_len;
        x->consts += (size_t)c->nconsts * 8u;
        x->other_code += (size_t)c->nexc * sizeof(embi_js_exc_entry) + c->pos_bytes + c->nupv * sizeof(embi_js_upval_desc) +
                         (c->file ? strlen(c->file) + 1u : 0u);
        x->ics += (c->ics ? (size_t)c->nics * sizeof(struct embi_js_ic) : 0u);
        x->ic_keys += c->ic_keys ? (size_t)c->nics * 8u : 0u;
    }
}

/* Writes the text of v (its string conversion, or a fallback) to stderr. */
static void write_error_text(embi_js_runtime *rt, embi_js_value v)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_js_string *s = embi_js_to_string(rt, v);
    if (s) {
        embi_js_str_append_utf8(s, &sb);
    } else {
        embi_js_exception_clear(rt);
        embi_sb_append(&sb, EMBI_STR("<unprintable error>"));
    }
    fprintf(stderr, "%s\n", embi_sb_cstr(&sb));
    embi_sb_free(&sb);
}

/* HostReportErrors: an exception thrown by a job that nothing caught. */
static void report_error(embi_js_runtime *rt, embi_js_value error, void *user)
{
    (void)user;
    fputs("bench_js_suite: uncaught error in a job: ", stderr);
    write_error_text(rt, error);
}

static bool install_host(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_define_method(r, r->global, "print", native_print, 1, 0);
    embi_js_define_method(r, r->global, "gc", native_gc, 0, 0);
    embi_js_object *perf = embi_js_object_new_plain(rt);
    if (!perf) {
        return false;
    }
    embi_js_define_method(r, perf, "now", native_now, 0, 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "performance"), embi_js_from_cell(perf),
                       EMBI_JS_ATTR_HIDDEN);
    return true;
}

/* FP1a: --compile-only [-n N] FILE...: times embi_js_compile (parse plus bytecode emission, nothing
 * runs) per file inside the process, best and median of N (default 15) runs, with a collection
 * between runs that is not timed. Prints one line per file: ms and MB/s. */
static int compile_only(int argc, char **argv)
{
    int runs = 15;
    int i = 2;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        runs = atoi(argv[i + 1]);
        i += 2;
    }
    if (runs < 1 || i >= argc) {
        fprintf(stderr, "usage: bench_js_suite --compile-only [-n N] FILE...\n");
        return 2;
    }
    int status = 0;
    double freq = (double)SDL_GetPerformanceFrequency();
    for (; i < argc; i++) {
        size_t len = 0;
        char *src = SDL_LoadFile(argv[i], &len);
        if (!src) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            status = 1;
            continue;
        }
        embi_js_runtime *rt = embi_js_runtime_new();
        embi_js_realm *r = rt ? embi_js_realm_new(rt) : nullptr;
        if (!r) {
            fprintf(stderr, "runtime setup failed\n");
            SDL_free(src);
            return 1;
        }
        double *ms = SDL_calloc((size_t)runs, sizeof(double));
        size_t gcs = 0;
        bool ok = true;
        bool as_module = false;
        for (int k = 0; k < runs && ok; k++) {
            size_t live0 = embi_mem_live();
            embi_js_mem_stats st0;
            embi_js_get_mem_stats(rt, &st0);
            embi_js_value v;
            uint64_t t0;
            uint64_t t1;
            if (!as_module) {
                t0 = SDL_GetPerformanceCounter();
                v = embi_js_compile(r, (const uint8_t *)src, (uint32_t)len, argv[i], false);
                t1 = SDL_GetPerformanceCounter();
                if (v == EMBI_JS_EXCEPTION && k == 0) {
                    /* Not a script: an ES module bundle (import/export). Measure it as a module. */
                    embi_js_exception_clear(rt);
                    as_module = true;
                    k = -1;
                    continue;
                }
            } else {
                cmod mod = {};
                embi_js_value holder = embi_js_from_object(embi_js_object_new_plain(rt));
                t0 = SDL_GetPerformanceCounter();
                v = embi_jsc_compile_module(r, (const uint8_t *)src, (uint32_t)len, argv[i], holder, &mod);
                t1 = SDL_GetPerformanceCounter();
                embi_vec_free(&mod.reqs);
                embi_vec_free(&mod.imps);
                embi_vec_free(&mod.exps);
            }
            if (v == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
                ok = false;
                break;
            }
            ms[k] = (double)(t1 - t0) * 1000.0 / freq;
            embi_js_mem_stats st1;
            embi_js_get_mem_stats(rt, &st1);
            gcs += st1.gc_count - st0.gc_count;
            if (k == 0) {
                fprintf(stderr, "first compile: live +%zu KB, peak %zu KB\n", (embi_mem_live() - live0) / 1024u, embi_mem_peak() / 1024u);
            }
            embi_js_gc(rt);
        }
        if (!ok) {
            printf("%-60s compile error\n", argv[i]);
        } else {
            const double first_ms = ms[0];
            for (int a = 1; a < runs; a++) { /* insertion sort */
                double x = ms[a];
                int b = a - 1;
                while (b >= 0 && ms[b] > x) {
                    ms[b + 1] = ms[b];
                    b--;
                }
                ms[b + 1] = x;
            }
            printf("%-60s %7zu B  first %7.3f ms  best %7.3f ms  median %7.3f ms  (%.1f MB/s, %.2f GCs/run)\n", argv[i], len, first_ms, ms[0],
                   ms[runs / 2], (double)len / 1e6 / (ms[0] / 1000.0), (double)gcs / runs);
        }
        SDL_free(ms);
        embi_js_runtime_free(rt);
        SDL_free(src);
    }
    return status;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--compile-only") == 0) {
        return compile_only(argc, argv);
    }
    const char *opstats_path = nullptr;
    if (argc == 4 && strcmp(argv[1], "--opstats") == 0) {
        opstats_path = argv[2];
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: bench_js_suite [--opstats FILE] FILE\n");
        return 2;
    }
    size_t len = 0;
    char *src = SDL_LoadFile(argv[1], &len);
    if (!src) {
        fprintf(stderr, "bench_js_suite: cannot read %s: %s\n", argv[1], SDL_GetError());
        return 1;
    }

    int status = 0;
    embi_js_runtime *rt = embi_js_runtime_new();
    /* J5k: EMBER_GC_GENERATIONAL=0 runs the collector without generational mode (the pre-J5k
     * default), for the before/after memory and time comparison. */
    const char *gen_env = getenv("EMBER_GC_GENERATIONAL");
    if (rt && gen_env && strcmp(gen_env, "0") == 0) {
        embi_js_gc_set_generational(rt, false);
    }
    /* J5n2: EMBER_GC_FLOOR (bytes) and EMBER_GC_FACTOR (cap on the growth factor) override the
     * trigger's floor and factor cap for the memory experiments. Unset, the runtime's own values
     * stay (the same numbers as heap.h). Both are validated: a bad value is a usage error (status
     * 2), never a silent zero. The floor is 1 byte to 1 GiB; the factor is finite and in (1, 8]. */
    const char *floor_env = getenv("EMBER_GC_FLOOR");
    const char *factor_env = getenv("EMBER_GC_FACTOR");
    if (rt && (floor_env || factor_env)) {
        size_t floor_bytes = (size_t)256u << 10;
        double factor = 1.25; /* heap.h EMBI_JS_GC_FACTOR_MAX */
        bool gc_env_ok = true;
        if (floor_env) {
            char *end = nullptr;
            unsigned long long v = strtoull(floor_env, &end, 10);
            if (end == floor_env || *end != 0 || v == 0 || v > (1ull << 30)) {
                fprintf(stderr, "bench_js_suite: EMBER_GC_FLOOR must be 1..1073741824 bytes, got '%s'\n",
                        floor_env);
                gc_env_ok = false;
            } else {
                floor_bytes = (size_t)v;
            }
        }
        if (factor_env) {
            char *end = nullptr;
            double f = strtod(factor_env, &end);
            if (end == factor_env || *end != 0 || !(f > 1.0) || !(f <= 8.0)) {
                fprintf(stderr, "bench_js_suite: EMBER_GC_FACTOR must be a number in (1, 8], got '%s'\n",
                        factor_env);
                gc_env_ok = false;
            } else {
                factor = f;
            }
        }
        if (!gc_env_ok) {
            embi_js_runtime_free(rt);
            SDL_free(src);
            return 2;
        }
        embi_js_set_gc_params(rt, floor_bytes, factor);
    }
    embi_js_realm *r = rt ? embi_js_realm_new(rt) : nullptr;
    if (!r || !install_host(r)) {
        fprintf(stderr, "bench_js_suite: runtime setup failed\n");
        status = 1;
    } else {
        embi_js_set_host_report_error(rt, report_error, nullptr);
#ifdef EMBI_JS_OPSTATS
        embi_js_opstats_reset();
#endif
        embi_js_value v = embi_js_eval(r, src, len, argv[1], 0);
        if (v == EMBI_JS_EXCEPTION) {
            const char *file = "";
            uint32_t line = 0;
            uint32_t col = 0;
            if (embi_js_error_location(rt, &file, &line, &col)) {
                fprintf(stderr, "bench_js_suite: %s:%u:%u: ", file, line, col);
            } else {
                fputs("bench_js_suite: ", stderr);
            }
            write_error_text(rt, embi_js_exception_get(rt));
            embi_js_exception_clear(rt);
            status = 1;
        } else {
            embi_js_run_jobs(rt);
            if (g_gc_requested) {
                embi_js_gc(rt);
            }
        }
#ifdef EMBI_JS_OPSTATS
        {
            embi_sb ob = {nullptr, 0, 0};
            embi_js_opstats_dump(&ob);
            FILE *of = opstats_path ? fopen(opstats_path, "wb") : stderr;
            if (of) {
                fputs(embi_sb_cstr(&ob), of);
                if (of != stderr) {
                    fclose(of);
                }
            }
            embi_sb_free(&ob);
        }
#else
        (void)opstats_path;
#endif
        embi_js_mem_stats st;
        embi_js_get_mem_stats(rt, &st);
        fprintf(stderr,
                "mem: heap_live=%zu heap_since_gc=%zu chunks=%zu large=%zu gc=%zu atoms=%zu "
                "handles=%zu weak=%zu\n",
                st.heap_live_bytes, st.heap_bytes_since_gc, st.heap_chunks, st.large_objects,
                st.gc_count, st.atoms, st.persistent_handles, st.weak_handles);
        /* J5n1: the live figure the harness parses (compare.sh, live runs). It is the last full
         * collection's live total (cells plus external bytes); driver.js calls gc() at the end of a
         * live run, which the host carries out when the script returns (see native_gc), so nothing is
         * allocated after it. */
        fprintf(stderr, "live: %zu\n", st.live_after_gc);
        fprintf(stderr, "alloc-peak: %zu live-now: %zu\n", embi_mem_peak(), embi_mem_live());
        if (getenv("EMBER_CENSUS") != nullptr) { /* J5n2: where the resident set sits (Linux) */
            FILE *sf = fopen("/proc/self/status", "r");
            if (sf != nullptr) {
                char ln[256];
                while (fgets(ln, sizeof ln, sf) != nullptr) {
                    if (strncmp(ln, "VmHWM", 5) == 0 || strncmp(ln, "RssAnon", 7) == 0 ||
                        strncmp(ln, "RssFile", 7) == 0) {
                        fprintf(stderr, "status: %s", ln);
                    }
                }
                fclose(sf);
            }
        }
        if (getenv("EMBER_CENSUS") != nullptr) { /* J5n2: what is alive, by cell type */
            size_t cnt[EMBI_JS_CELL_TYPE_COUNT] = {0};
            size_t byt[EMBI_JS_CELL_TYPE_COUNT] = {0};
            static const char *const names[EMBI_JS_CELL_TYPE_COUNT] = {
                "free", "string", "symbol", "object", "shape", "code", "box", "accessor", "bigint", "varray"};
            census_ext cx = {0};
            embi_js_gc_census(&rt->heap, cnt, byt, census_visit, &cx);
            size_t cells = 0;
            for (unsigned t = 1; t < EMBI_JS_CELL_TYPE_COUNT; t++) {
                fprintf(stderr, "census: %-8s n=%zu bytes=%zu\n", names[t], cnt[t], byt[t]);
                cells += byt[t];
            }
            fprintf(stderr, "census: cells=%zu external=%zu\n", cells, st.live_after_gc - cells);
            fprintf(stderr, "census-ext: obj_slots=%zu obj_elems=%zu rare=%zu code_bc=%zu code_consts=%zu code_other=%zu ics=%zu ic_keys=%zu nobj=%zu closures=%zu ctor=%zu upvals=%zu arrays=%zu plain=%zu\n",
                    cx.slots, cx.elems, cx.rare, cx.bc, cx.consts, cx.other_code, cx.ics, cx.ic_keys, cx.nobj, cx.nfn, cx.nctor, cx.ncl_up, cx.narr, cx.nplain);
            fprintf(stderr, "census-props:");
            for (unsigned i = 0; i < 12; i++) {
                fprintf(stderr, " %u%s=%zu", i, i == 11 ? "+" : "", cx.hist[i]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (r) {
        embi_js_realm_free(r);
    }
    if (rt) {
        embi_js_runtime_free(rt);
    }
    SDL_free(src);
    return status;
}
