/*
 * Incremental update bench (W6b). A page of about 2,000 elements, resolved, laid out and painted
 * once; then one node changes and the frame is redone: restyle, layout and display list, timed per phase. Three changes:
 *   - style (paint): a class that sets color on one element (a paint-only change: the layout keeps its fragments and only
 *     the stacking context holding the element is painted again);
 *   - style (layout): a class that sets a width (the box subtree around the element is built again, the layout runs over
 *     the kept tree);
 *   - text: the text of one element changes.
 * Each is run two ways. "before" is the behavior before W6b, reproduced with the public switches: every style marked stale
 * (embi_style_mark_dirty: a full restyle), the box tree built again (embi_layout_invalidate_all) and the display list built
 * in full. "after" is the incremental path. Output is the median and best of 40 frames per phase, in milliseconds.
 * Run it from a Release build, pinned to a core: taskset -c 9 build/rel/tools/bench/bench_incr
 */

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "paint/paint.h"
#include "style/style.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNS 40

typedef struct world {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
    embi_paint_cache *cache;
    embi_dl dl;
    embi_element *target;
    uint32_t elements;
} world;

static const char *k_css =
    "body{margin:0} .sec{padding:6px;border-bottom:1px solid #ccc} h3{margin:2px 0;font-size:18px} p{margin:3px 0}"
    ".s{color:#336} .hot{color:red} .wide{width:300px} li{margin-left:12px} .card{background-color:#eef;border-radius:4px}"
    "a{color:#06c;text-decoration:underline} b{font-weight:bold}";

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static double now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void build(world *w)
{
    embi_sb sb;
    memset(&sb, 0, sizeof sb);
    w->elements = 0;
    for (uint32_t i = 0; w->elements < 2000u; i++) {
        char buf[512];
        int n = SDL_snprintf(buf, sizeof buf,
                             "<div class=sec%s id=s%u><h3>Section %u</h3><p>Paragraph %u with <span class=s>an inline span</span>, "
                             "<a href=\"#%u\">a link</a> and <b>bold words</b> to make the line wrap around a bit more.</p>"
                             "<ul><li>first item %u</li><li>second item</li><li>third item</li></ul></div>",
                             (i % 3) == 0 ? " card" : "", i, i, i, i, i);
        embi_sb_append(&sb, (embi_str){buf, (uint32_t)n});
        w->elements += 12;
    }
    w->text = embi_tshim_create();
    w->style = embi_style_engine_create(embi_tshim_fonts(w->text), nullptr, nullptr);
    w->doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(w->doc, nullptr);
    embi_html_parser_write(p, sb.data, sb.len);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    embi_sb_free(&sb);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    w->sheet = embi_css_sheet_parse(S(k_css), &opts);
    embi_css_sheet *sheets[1] = {w->sheet};
    embi_document_set_author_sheets(w->style, w->doc, sheets, 1);
    w->eng = embi_layout_engine_create(w->text, w->style, nullptr);
    embi_layout_set_images_stable(w->eng, true);
    embi_dl_init(&w->dl);
    w->cache = embi_paint_cache_create();
    char id[32];
    SDL_snprintf(id, sizeof id, "s%u", (w->elements / 12u) / 2u);
    w->target = embi_get_element_by_id(w->doc, S(id));
}

static void destroy(world *w)
{
    embi_dl_free(&w->dl);
    embi_paint_cache_free(w->cache);
    embi_layout_engine_destroy(w->eng);
    embi_style_forget_document(w->style, w->doc);
    embi_document_free(w->doc);
    embi_css_sheet_unref(w->sheet);
    embi_style_engine_destroy(w->style);
    embi_tshim_destroy(w->text);
}

typedef enum { CH_PAINT, CH_LAYOUT, CH_TEXT } change;

static embi_element *inner(world *w, const char *tag)
{
    for (embi_node *n = w->target->base.first_child; n != nullptr; n = n->next) {
        if (embi_is_element(n)) {
            embi_element *e = embi_as_element(n);
            embi_str nm = embi_atom_str(e->base.name);
            if (nm.len == strlen(tag) && memcmp(nm.ptr, tag, nm.len) == 0) {
                return e;
            }
        }
    }
    return w->target;
}

static void mutate(world *w, change c, int i)
{
    embi_element *p = inner(w, "p");
    switch (c) {
    case CH_PAINT:
        embi_el_set_attribute(p, EMBI_STR("class"), embi_dstr_from_utf8(w->doc->slab, (i & 1) != 0 ? EMBI_STR("hot") : EMBI_STR("")));
        break;
    case CH_LAYOUT:
        embi_el_set_attribute(p, EMBI_STR("class"), embi_dstr_from_utf8(w->doc->slab, (i & 1) != 0 ? EMBI_STR("wide") : EMBI_STR("")));
        break;
    default: {
        embi_node *t = p->base.first_child;
        embi_chardata_set(embi_as_chardata(t), embi_dstr_from_utf8(w->doc->slab, (i & 1) != 0 ? EMBI_STR("Paragraph with a longer text that wraps onto more lines of the page ")
                                                                                              : EMBI_STR("Paragraph short ")));
        break;
    }
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct phase {
    double v[RUNS];
} phase;

static void report(const char *label, phase *st, phase *la, phase *pa)
{
    double tot[RUNS];
    for (int i = 0; i < RUNS; i++) {
        tot[i] = st->v[i] + la->v[i] + pa->v[i];
    }
    qsort(st->v, RUNS, sizeof(double), cmp_d);
    qsort(la->v, RUNS, sizeof(double), cmp_d);
    qsort(pa->v, RUNS, sizeof(double), cmp_d);
    qsort(tot, RUNS, sizeof(double), cmp_d);
    printf("%-34s style %7.3f  layout %7.3f  paint %7.3f  frame %7.3f ms (median)   best frame %7.3f\n", label,
           st->v[RUNS / 2], la->v[RUNS / 2], pa->v[RUNS / 2], tot[RUNS / 2], tot[0]);
}

static void run(const char *name, change c, bool incremental)
{
    world w;
    memset(&w, 0, sizeof w);
    build(&w);
    embi_paint_cfg pcfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_style_resolve_document(w.style, w.doc);
    (void)embi_layout_document(w.eng, w.doc);
    embi_paint_document_incr(w.cache, &w.dl, w.eng, w.doc, &pcfg, false);
    phase st, la, pa;
    memset(&st, 0, sizeof st);
    memset(&la, 0, sizeof la);
    memset(&pa, 0, sizeof pa);
    for (int i = -4; i < RUNS; i++) {
        mutate(&w, c, i + 5);
        if (!incremental) {
            embi_style_mark_dirty(w.style, w.doc);
            embi_layout_invalidate_all(w.eng, w.doc);
        }
        double t0 = now_ms();
        embi_style_resolve_document(w.style, w.doc);
        double t1 = now_ms();
        (void)embi_layout_document(w.eng, w.doc);
        double t2 = now_ms();
        if (incremental) {
            embi_paint_document_incr(w.cache, &w.dl, w.eng, w.doc, &pcfg, true);
        } else {
            (void)embi_paint_document(&w.dl, w.eng, w.doc, &pcfg);
        }
        double t3 = now_ms();
        if (i >= 0) {
            st.v[i] = t1 - t0;
            la.v[i] = t2 - t1;
            pa.v[i] = t3 - t2;
        }
    }
    char label[64];
    SDL_snprintf(label, sizeof label, "%s %s", incremental ? "after " : "before", name);
    report(label, &st, &la, &pa);
    if (incremental) {
        embi_layout_incr_stats ls;
        embi_layout_incr_stats_get(w.eng, w.doc, &ls);
        printf("        last layout kind %u, boxes laid out %u, boxes rebuilt %u; paint: %u contexts rebuilt, %s\n", ls.last_kind,
               ls.boxes_laid_out, ls.boxes_rebuilt, embi_paint_cache_rebuilt(w.cache),
               embi_paint_cache_last_incremental(w.cache) ? "incremental" : "full");
    }
    destroy(&w);
}

int main(void)
{
    printf("page: about 2000 elements; one-node change, frame = restyle + layout + display list\n");
    run("one-node style change (paint)", CH_PAINT, false);
    run("one-node style change (paint)", CH_PAINT, true);
    run("one-node style change (layout)", CH_LAYOUT, false);
    run("one-node style change (layout)", CH_LAYOUT, true);
    run("one-node text change", CH_TEXT, false);
    run("one-node text change", CH_TEXT, true);
    return 0;
}
