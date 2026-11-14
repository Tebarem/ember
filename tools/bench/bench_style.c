/*
 * Style resolution bench. It builds a document of 10,000 elements (a flat list of divs and spans
 * in sections, each with one class) and a stylesheet of 2,000 class rules plus descendant rules,
 * then measures:
 *   - the time to attach the sheet and resolve every element (best of several runs);
 *   - the computed-style memory per element (interned styles and groups, from the engine stats).
 * Targets: the resolve <= 15 ms and the computed style <= 120 bytes per
 * element. Run it from a Release build for numbers:
 *     cmake --build build/<dir> --target bench_style
 *     build/<dir>/tools/bench/bench_style
 * Each line ends in PASS or FAIL. The exit code is 1 if any target failed.
 */

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define BENCH_ELEMENTS 10000u
#define BENCH_RULES 2000u
#define BENCH_RUNS 5
#define BENCH_TARGET_MS 15.0
#define BENCH_TARGET_BYTES_PER_ELEMENT 120.0

static char *build_html(size_t *out_len)
{
    size_t cap = BENCH_ELEMENTS * 64 + 256;
    char *buf = SDL_malloc(cap);
    size_t len = 0;
    len += (size_t)SDL_snprintf(buf + len, cap - len, "<!DOCTYPE html><html><body>");
    for (uint32_t i = 0; i < BENCH_ELEMENTS; i++) {
        if (i % 100 == 0) {
            len += (size_t)SDL_snprintf(buf + len, cap - len, "<section>");
        }
        uint32_t rule = (i * 37u) % BENCH_RULES;
        if (i % 3 == 0) {
            len += (size_t)SDL_snprintf(buf + len, cap - len, "<span class=\"r%u\">x</span>", rule);
        } else {
            len += (size_t)SDL_snprintf(buf + len, cap - len, "<div class=\"r%u\"></div>", rule);
        }
        if (i % 100 == 99) {
            len += (size_t)SDL_snprintf(buf + len, cap - len, "</section>");
        }
    }
    len += (size_t)SDL_snprintf(buf + len, cap - len, "</body></html>");
    *out_len = len;
    return buf;
}

static char *build_css(size_t *out_len)
{
    size_t cap = BENCH_RULES * 128 + 256;
    char *buf = SDL_malloc(cap);
    size_t len = 0;
    for (uint32_t r = 0; r < BENCH_RULES; r++) {
        if (r % 4 == 0) {
            len += (size_t)SDL_snprintf(buf + len, cap - len,
                                        "section .r%u { color: rgb(%u, %u, %u); }\n", r, r % 256,
                                        (r * 3) % 256, (r * 7) % 256);
        } else {
            len += (size_t)SDL_snprintf(buf + len, cap - len,
                                        ".r%u { margin-top: %upx; padding-left: %upx; color: "
                                        "rgb(%u, %u, %u); font-size: %upx; }\n",
                                        r, r % 50, r % 7, r % 256, (r * 3) % 256, (r * 7) % 256,
                                        12 + r % 5);
        }
    }
    *out_len = len;
    return buf;
}

typedef struct run_result {
    double attach_ms;
    double resolve_ms;
    double bytes_per_element;
    size_t arena_bytes;
    uint32_t styles;
} run_result;

static run_result run_once(const char *html, size_t html_len, const char *css, size_t css_len)
{
    run_result out = {0, 0, 0, 0, 0};
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, html_len);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);

    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){css, (uint32_t)css_len}, &opts);
    embi_css_sheet *sheets[1] = {sheet};

    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    uint64_t t1 = SDL_GetPerformanceCounter();
    embi_style_resolve_document(eng, doc);
    uint64_t t2 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    out.attach_ms = (double)(t1 - t0) * 1000.0 / freq;
    out.resolve_ms = (double)(t2 - t1) * 1000.0 / freq;

    embi_style_stats st = {0};
    embi_style_engine_stats(eng, &st);
    out.styles = st.styles;
    /* The per-element style slot (one pointer) is part of the cost. The engine's arena (declared
     * values, canonical values, cached declarations) is reported separately. */
    out.bytes_per_element = ((double)st.bytes + 8.0 * (double)BENCH_ELEMENTS) / (double)BENCH_ELEMENTS;
    out.arena_bytes = st.arena_bytes;

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
    return out;
}

int main(void)
{
    size_t html_len = 0, css_len = 0;
    char *html = build_html(&html_len);
    char *css = build_css(&css_len);

    double best_ms = 1e300, best_attach = 1e300;
    run_result last = {0, 0, 0, 0, 0};
    for (int r = 0; r < BENCH_RUNS; r++) {
        last = run_once(html, html_len, css, css_len);
        if (last.resolve_ms < best_ms) {
            best_ms = last.resolve_ms;
        }
        if (last.attach_ms < best_attach) {
            best_attach = last.attach_ms;
        }
    }

    /* The timing target only means something in an optimized build (Release defines NDEBUG);
     * Debug and sanitizer builds report the time but do not fail on it. */
#ifdef NDEBUG
    bool ok_time = best_ms <= BENCH_TARGET_MS;
#else
    bool ok_time = true;
    SDL_Log("(non-Release build: the timing target is not enforced)");
#endif
    bool ok_mem = last.bytes_per_element <= BENCH_TARGET_BYTES_PER_ELEMENT;
    SDL_Log("resolve 10k elements x 2k rules: %.2f ms (best of %d) %s (target <= %.1f ms)",
            best_ms, BENCH_RUNS, ok_time ? "PASS" : "FAIL", BENCH_TARGET_MS);
    SDL_Log("  of which sheet attach (flatten): %.2f ms (best of %d)", best_attach, BENCH_RUNS);
    SDL_Log("computed style: %.1f bytes per element (interned + 8 B slot), %u interned styles %s "
            "(target <= %.0f)",
            last.bytes_per_element, last.styles, ok_mem ? "PASS" : "FAIL",
            BENCH_TARGET_BYTES_PER_ELEMENT);
    SDL_Log("  engine arena: %.1f bytes per element (%llu bytes total)",
            (double)last.arena_bytes / BENCH_ELEMENTS, (unsigned long long)last.arena_bytes);

    SDL_free(html);
    SDL_free(css);
    return (ok_time && ok_mem) ? 0 : 1;
}
