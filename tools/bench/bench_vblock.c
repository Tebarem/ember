/*
 * bench_vblock (VBLOCKPERF): the per-frame cost of animating N elements at once, the case the value blocks (src/style/vblock.c)
 * have to keep cheap: every animation sample puts its values in a new block, and the old block goes when the old style does,
 * so the block bookkeeping runs once per animated element and frame.
 *
 *   bench_vblock [--frames N] [--n N]... [--case NAME]
 *
 * Cases (N elements each, a document without a view, so no GPU, on the fake clock stepped 16 ms per frame):
 *   anim-opacity   N divs with a CSS animation of opacity, transform and background-color (paint-only properties)
 *   anim-inherit   N/10 parents whose font-size is animated, each with 10 children that inherit it (em-sized width), so
 *                  every frame also makes new groups in the children that point at the parents' animated values
 *   transition     N divs whose opacity and background-color transition, flipped by a script every 20 frames
 * Prints the median, mean and p95 of the frame (animator + style + layout + frame end, measured with the performance counter)
 * over the measured frames, then the median of each phase, the kinds of layout updates the run took, and the number of
 * value blocks alive after the last frame. Run it from a Release build, idle machine.
 */
#include "unit/anim/frame_rig.h"

#include "layout/layout.h"

#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL_timer.h>

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static char *make_page(const char *name, uint32_t n)
{
    size_t cap = (size_t)n * 96 + 2048;
    char *s = SDL_malloc(cap);
    size_t len = 0;
#define ADD(...) (len += (size_t)SDL_snprintf(s + len, cap - len, __VA_ARGS__))
    if (strcmp(name, "anim-opacity") == 0) {
        ADD("<style>.a{width:20px;height:10px;animation:k 1s linear infinite alternate}"
            "@keyframes k{from{opacity:1;transform:translateX(0);background-color:#f00}"
            "to{opacity:.2;transform:translateX(30px);background-color:#00f}}</style><body>");
        for (uint32_t i = 0; i < n; i++) {
            ADD("<div class=a></div>");
        }
    } else if (strcmp(name, "anim-inherit") == 0) {
        ADD("<style>.p{font-size:10px;animation:k 1s linear infinite alternate}.c{width:2em;height:1em}"
            "@keyframes k{from{font-size:10px}to{font-size:20px}}</style><body>");
        for (uint32_t i = 0; i < n / 10u; i++) {
            ADD("<div class=p>");
            for (int j = 0; j < 10; j++) {
                ADD("<span class=c></span>");
            }
            ADD("</div>");
        }
    } else {
        ADD("<style>.t{width:20px;height:10px;opacity:1;background-color:#f00;transition:opacity 1s linear,background-color 1s linear}"
            ".t.on{opacity:.2;background-color:#00f}</style><body>");
        for (uint32_t i = 0; i < n; i++) {
            ADD("<div class=t></div>");
        }
    }
#undef ADD
    return s;
}

static void run_case(const char *name, uint32_t n, uint32_t frames)
{
    char *html = make_page(name, n);
    rig r;
    rig_setup(&r);
    rig_load(&r, html);
    SDL_free(html);
    bool trans = strcmp(name, "transition") == 0;
    uint32_t warm = 10;
    double *ft = SDL_malloc(sizeof(double) * frames);
    double *ph = SDL_malloc(sizeof(double) * frames * 4); /* per phase: animator frame, style, layout, frame end */
    for (uint32_t i = 0; i < warm + frames; i++) {
        if (trans && i % 20 == 0) {
            rig_js(&r, "var l=document.querySelectorAll('.t');for(var i=0;i<l.length;i++)l[i].classList.toggle('on')");
        }
        rig_ms(16);
        double tick = 1000.0 / (double)SDL_GetPerformanceFrequency();
        uint64_t t0 = SDL_GetPerformanceCounter();
        embi_doc_frame(r.d);
        uint64_t t1 = SDL_GetPerformanceCounter();
        embi_doc_times tm;
        memset(&tm, 0, sizeof tm);
        (void)embi_doc_update(r.d, &tm);
        uint64_t t2 = SDL_GetPerformanceCounter();
        embi_doc_frame_end(r.d);
        uint64_t t3 = SDL_GetPerformanceCounter();
        if (i >= warm) {
            uint32_t k = i - warm;
            ft[k] = (double)(t3 - t0) * tick;
            ph[k] = (double)(t1 - t0) * tick;
            ph[frames + k] = tm.style_ms;
            ph[2 * frames + k] = tm.layout_ms;
            ph[3 * frames + k] = (double)(t3 - t2) * tick;
        }
    }
    double sum = 0;
    for (uint32_t i = 0; i < frames; i++) {
        sum += ft[i];
    }
    qsort(ft, frames, sizeof ft[0], cmp_double);
    for (uint32_t q = 0; q < 4; q++) {
        qsort(ph + q * frames, frames, sizeof ph[0], cmp_double);
    }
    printf("%-13s N=%-6u frames=%u  median %8.3f ms  mean %8.3f ms  p95 %8.3f ms  blocks alive %u\n", name, n, frames,
           ft[frames / 2], sum / frames, ft[(size_t)(frames * 0.95)],
           embi_style_vblock_count(embi_doc_style_engine(r.d)));
    printf("%-13s N=%-6u phases (median ms): animator %.3f  style %.3f  layout %.3f  frame-end %.3f\n", "", n, ph[frames / 2],
           ph[frames + frames / 2], ph[2 * frames + frames / 2], ph[3 * frames + frames / 2]);
    embi_layout_incr_stats st;
    embi_layout_incr_stats_get(embi_doc_layout_engine(r.d), embi_doc_dom(r.d), &st);
    printf("%-13s N=%-6u layout updates over the run: none %llu  paint %llu  surgery %llu  full %llu  (tree builds %u)\n", "", n,
           (unsigned long long)st.n_none, (unsigned long long)st.n_paint, (unsigned long long)st.n_surgery,
           (unsigned long long)st.n_full, st.tree_builds);
    SDL_free(ph);
    SDL_free(ft);
    rig_teardown(&r);
}

int main(int argc, char **argv)
{
    uint32_t frames = 60;
    uint32_t ns[8], nn = 0;
    const char *only = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc && nn < 8) {
            ns[nn++] = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--n N]... [--case NAME]\n", argv[0]);
            return 0;
        }
    }
    if (frames < 10) {
        frames = 10;
    }
    if (nn == 0) {
        ns[0] = 100;
        ns[1] = 2000;
        ns[2] = 10000;
        nn = 3;
    }
    static const char *const cases[] = {"anim-opacity", "anim-inherit", "transition"};
    for (size_t c = 0; c < 3; c++) {
        if (only != nullptr && strcmp(only, cases[c]) != 0) {
            continue;
        }
        for (uint32_t i = 0; i < nn; i++) {
            run_case(cases[c], ns[i], frames);
        }
    }
    return 0;
}
