/*
 * Layout bench (W3a). It generates a document of about 10,000
 * elements and resolves its style once. The document is a mix of:
 *   - paragraphs of mixed inline text (spans, links, <b>),
 *   - floats,
 *   - flex rows of cards,
 *   - grid sections,
 *   - one 50 x 10 table,
 *   - positioned badges (a relatively positioned box with an absolutely positioned child).
 * It then times embi_layout_document four ways, each against a target:
 *   - the first layout (cold: empty shaping caches, arenas not sized, fonts not loaded), with a breakdown of where
 *     it went (W3q: font loading and face setup, the other shaped runs, the layout's own code);
 *   - best of 10 layouts that rebuild the box tree because the viewport width changed (no restyle);
 *   - best of 10 layouts that rebuild it after a DOM mutation (the styles are resolved before the clock starts);
 *   - best of 10 layouts of the unchanged document (the box tree is kept between layouts of an unchanged document,
 *     W3p).
 * It reports the boxes and fragments created and the live bytes per box (box tree plus fragment tree, from
 * embi_layout_stats; the shared text shaping caches are not included) and, without a target, the same with the
 * derived caches the layout keeps (embi_layout_cache_bytes).
 * It prints PASS or FAIL for each target (<= 75 ms cold, <= 8 ms for each rebuild, <= 8 ms kept, <= 160 bytes per
 * box, <= 5 ms for the display list) and exits 1 on FAIL. It is
 * not a ctest. Run it from a Release build:
 *     cmake --build build/<dir> --target bench_layout
 *     build/<dir>/tools/bench/bench_layout
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

#include <string.h>

#define BENCH_ELEMENTS 10000u
#define BENCH_RUNS 10
#define BENCH_TARGET_MS 8.0
#define BENCH_TARGET_REBUILD_MS 8.0
#define BENCH_TARGET_COLD_MS 75.0
#define BENCH_TARGET_BYTES_PER_BOX 160.0
#define BENCH_TARGET_CLASS_MS 1.0 /* W6: one class change, restyle plus relayout, on the bench page */
#define BENCH_TARGET_PAINT_MS 5.0 /* W4c: the display list of the bench page (proposed budget) */

typedef struct gen {
    embi_sb sb;
    uint32_t elements;
} gen;

/* Appends markup and counts the elements it opens (every '<' followed by a letter). */
static void put(gen *g, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (p[0] == '<' && ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))) {
            g->elements++;
        }
    }
    embi_sb_append(&g->sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void putf(gen *g, const char *fmt, uint32_t a)
{
    char tmp[512];
    SDL_snprintf(tmp, sizeof tmp, fmt, a, a, a);
    put(g, tmp);
}

static void gen_paragraph(gen *g, uint32_t i)
{
    putf(g, "<p>Paragraph %u with <span class=s>an inline span</span> and <a href=\"#%u\">a link %u</a>, "
            "some <b>bold words</b> and a long enough tail to need a few more glyphs.</p>",
         i);
}

static void gen_float(gen *g, uint32_t i)
{
    putf(g, "<div class=fl>float %u</div>", i);
}

static void gen_flex(gen *g, uint32_t i)
{
    put(g, "<div class=row>");
    for (uint32_t k = 0; k < 3; k++) {
        putf(g, "<div class=card><h4>Card %u</h4><p>Card text for item %u.</p></div>", i * 3 + k);
    }
    put(g, "</div>");
}

static void gen_grid(gen *g, uint32_t i)
{
    put(g, "<div class=grid>");
    for (uint32_t k = 0; k < 6; k++) {
        putf(g, "<div class=cell>cell %u</div>", i * 6 + k);
    }
    put(g, "</div>");
}

static void gen_table(gen *g)
{
    put(g, "<table class=t><tbody>");
    for (uint32_t r = 0; r < 10; r++) {
        put(g, "<tr>");
        for (uint32_t c = 0; c < 50; c++) {
            putf(g, "<td>%u</td>", r * 50 + c);
        }
        put(g, "</tr>");
    }
    put(g, "</tbody></table>");
}

static void gen_badge(gen *g, uint32_t i)
{
    putf(g, "<div class=host>host %u<span class=badge>%u</span></div>", i);
}

/* Ends the markup in g and returns it as a NUL-terminated buffer (free with SDL_free). */
static char *finish_html(gen *g, size_t *out_len, uint32_t *elements)
{
    *out_len = g->sb.len;
    *elements = g->elements;
    char *buf = SDL_malloc(g->sb.len + 1);
    memcpy(buf, g->sb.data, g->sb.len);
    buf[g->sb.len] = '\0';
    embi_sb_free(&g->sb);
    return buf;
}

/* Builds the document: sections of blocks until the element count reaches the target. */
static char *build_html(size_t *out_len, uint32_t *elements)
{
    gen g = {{nullptr, 0, 0}, 0};
    put(&g, "<!DOCTYPE html><html><body>");
    bool table_done = false;
    for (uint32_t i = 0; g.elements < BENCH_ELEMENTS; i++) {
        if (i % 16 == 0) {
            put(&g, "<section><div class=wrap>");
        }
        gen_paragraph(&g, i);
        if (i % 10 == 3) {
            gen_float(&g, i);
        }
        if (i % 8 == 5) {
            gen_flex(&g, i);
        }
        if (i % 20 == 7) {
            gen_grid(&g, i);
        }
        if (i % 5 == 1) {
            gen_badge(&g, i);
        }
        if (!table_done && i == 40) {
            gen_table(&g);
            table_done = true;
        }
        if (i % 16 == 15) {
            put(&g, "</div></section>");
        }
    }
    put(&g, "</body></html>");
    return finish_html(&g, out_len, elements);
}

/* Nested block formatting contexts, each with a float beside the level below it (W3b). Every level
 * is a BFC root beside the floats of its parent, so its layout must not repeat the work of the levels
 * inside it: the cost stays linear in the depth. */
#define BENCH_DEPTH 48u

static const char *const BENCH_DEPTH_CSS =
    "body { margin: 0; font: 16px/1.4 sans-serif }\n"
    ".lvl { overflow: hidden; padding: 1px }\n"
    ".fl { float: left; width: 20px; height: 10px; margin-right: 4px }\n";

static char *build_depth_html(size_t *out_len, uint32_t *elements, uint32_t depth)
{
    gen g = {{nullptr, 0, 0}, 0};
    put(&g, "<!DOCTYPE html><html><body>");
    for (uint32_t d = 0; d < depth; d++) {
        put(&g, "<div class=lvl><div class=fl>float</div>text of the level ");
    }
    put(&g, "<p>leaf</p>");
    for (uint32_t d = 0; d < depth; d++) {
        put(&g, "</div>");
    }
    put(&g, "</body></html>");
    return finish_html(&g, out_len, elements);
}

static const char *const BENCH_CSS =
    "body { margin: 0; font: 16px/1.4 sans-serif }\n"
    "section { margin: 4px 0; padding: 2px }\n"
    ".wrap { width: 760px; margin: 0 auto }\n"
    "p { margin: 6px 0 }\n"
    ".s { color: #444 }\n"
    ".s.hot { color: #c00 }\n"
    ".s.pad { padding: 0 3px }\n"
    "a { color: blue; text-decoration: underline }\n"
    ".fl { float: left; width: 120px; height: 40px; margin: 0 8px 4px 0 }\n"
    ".row { display: flex; gap: 8px; margin: 8px 0 }\n"
    ".card { flex: 1 1 0; padding: 6px; border: 1px solid #ccc }\n"
    ".card h4 { margin: 0 0 4px }\n"
    ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px }\n"
    ".cell { padding: 4px; background: #eee }\n"
    ".t { border-collapse: collapse }\n"
    ".t td { padding: 1px 3px; border: 1px solid #999 }\n"
    ".host { position: relative; padding: 4px 40px 4px 4px }\n"
    ".badge { position: absolute; top: 2px; right: 2px; padding: 0 4px; background: #c00; color: #fff }\n";

/* W6: one class toggle of one element, restyle plus relayout, best of BENCH_RUNS * 2 */
typedef struct class_run {
    double ms;
    uint32_t restyled;  /* elements restyled by the last toggle */
    uint32_t kind;      /* EMBI_LU_* of the last layout */
    uint32_t laid_out;  /* boxes the last layout laid out */
} class_run;

typedef struct bench_result {
    double ms;      /* best of BENCH_RUNS layouts of the unchanged document (the box tree is kept) */
    double cold_ms; /* the first layout of the document: the shaping caches and arenas are empty */
    double vp_ms;    /* best of BENCH_RUNS layouts that rebuild the box tree because the viewport width changed (as W3p measured the rebuild) */
    double build_ms; /* best of BENCH_RUNS layouts that rebuild the box tree (a DOM mutation, styles resolved first) */
    double setup_ms; /* creating the text system (outside the layouts) */
    embi_layout_cold_stats cold;
    uint64_t first_ticks; /* the first shaped run: font loading and face setup */
    uint32_t boxes, frags, frag_allocs, frag_reused;
    size_t arena_bytes, cache_bytes;
    uint32_t elements;
    double paint_ms;     /* W4c: best of BENCH_RUNS display list builds of the laid-out document (no target yet) */
    uint32_t paint_items;
    class_run cls_paint;      /* W6: toggling a class that changes paint only */
    class_run cls_layout;     /* W6: toggling a class that changes the padding of an inline box */
    size_t paint_bytes;
} bench_result;

static embi_element *find_class_s(embi_node *n)
{
    for (; n != nullptr; n = n->next) {
        if (embi_is_element(n)) {
            embi_element *e = embi_as_element(n);
            embi_str nm = embi_atom_str(e->base.name);
            if (nm.len == 4 && memcmp(nm.ptr, "span", 4) == 0) {
                return e;
            }
            embi_element *r = find_class_s(n->first_child);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return nullptr;
}

static class_run class_toggle(embi_layout_engine *eng, embi_style_engine *style, embi_document *doc, embi_element *el,
                              embi_str on, embi_str off)
{
    class_run out;
    memset(&out, 0, sizeof out);
    double freq = (double)SDL_GetPerformanceFrequency();
    double best = 1e300;
    for (int r = -3; r < BENCH_RUNS * 2; r++) {
        embi_el_set_attribute(el, EMBI_STR("class"), embi_dstr_from_utf8(doc->slab, (r & 1) != 0 ? on : off));
        uint64_t t0 = SDL_GetPerformanceCounter();
        embi_style_resolve_document(style, doc);
        (void)embi_layout_document(eng, doc);
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
        if (r >= 0 && ms < best) {
            best = ms;
        }
    }
    embi_style_restyle_stats rs;
    embi_style_restyle_stats_get(style, doc, &rs);
    embi_layout_incr_stats ls;
    embi_layout_incr_stats_get(eng, doc, &ls);
    out.ms = best;
    out.restyled = rs.last_restyled;
    out.kind = ls.last_kind;
    out.laid_out = ls.boxes_laid_out;
    return out;
}

static bench_result run_once(const char *html, size_t html_len, uint32_t elements, const char *css)
{
    bench_result out;
    memset(&out, 0, sizeof out);
    out.elements = elements;
    uint64_t s0 = SDL_GetPerformanceCounter();
    embi_tshim_reset_first();
    embi_text *text = embi_tshim_create();
    out.setup_ms = (double)(SDL_GetPerformanceCounter() - s0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    embi_style_engine *style = embi_style_engine_create(embi_tshim_fonts(text), nullptr, nullptr);
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, html_len);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);

    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){css, (uint32_t)strlen(css)}, &opts);
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(style, doc, sheets, 1);
    embi_style_resolve_document(style, doc); /* style is resolved once, before any timing */

    embi_layout_engine *eng = embi_layout_engine_create(text, style, nullptr);
    embi_layout_set_images_stable(eng, true); /* W6: the incremental layout path (this page has no images) */
    double freq = (double)SDL_GetPerformanceFrequency();
    /* The first layout fills the shaping caches and sizes the arenas: reported as the cold time. */
    uint64_t c0 = SDL_GetPerformanceCounter();
    (void)embi_layout_document(eng, doc);
    out.cold_ms = (double)(SDL_GetPerformanceCounter() - c0) * 1000.0 / freq;
    embi_layout_cold_stats_get(eng, doc, &out.cold);
    out.first_ticks = embi_tshim_first_ticks();

    /* A layout that has to rebuild the box tree because the viewport width changed (no restyle: the only key of the
     * kept tree that needs none; this is how W3p measured the rebuild path). The shaping caches and the paragraph
     * analyses are warm. */
    double best_vp = 1e300;
    for (int r = 0; r < BENCH_RUNS; r++) {
        embi_layout_set_viewport(eng, (r & 1) != 0 ? 800.0f : 801.0f, 600.0f);
        uint64_t t0 = SDL_GetPerformanceCounter();
        (void)embi_layout_document(eng, doc);
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
        if (ms < best_vp) {
            best_vp = ms;
        }
    }
    embi_layout_set_viewport(eng, 800.0f, 600.0f);
    out.vp_ms = best_vp;

    /* A layout that has to rebuild the box tree because of a DOM or style change. The document is mutated (an
     * attribute of the root element changes, which marks every style stale) and its styles are resolved before
     * the clock starts, so the layout is timed without the style engine's own full restyle. */
    embi_element *root_el = embi_document_element(doc);
    double best_build = 1e300;
    for (int r = 0; r < BENCH_RUNS; r++) {
        embi_el_set_attribute(root_el, EMBI_STR("data-bench"),
                              embi_dstr_from_utf8(doc->slab, (r & 1) != 0 ? EMBI_STR("odd") : EMBI_STR("even")));
        embi_style_resolve_document(style, doc);
        embi_layout_invalidate_all(eng, doc); /* W6b: the attribute changes nothing the incremental layout reads; force the rebuild */
        uint64_t t0 = SDL_GetPerformanceCounter();
        (void)embi_layout_document(eng, doc);
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
        if (ms < best_build) {
            best_build = ms;
        }
    }
    out.build_ms = best_build;

    (void)embi_layout_document(eng, doc); /* rebuilds once more; the runs below keep the tree */
    double best = 1e300;
    for (int r = 0; r < BENCH_RUNS; r++) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        (void)embi_layout_document(eng, doc);
        uint64_t t1 = SDL_GetPerformanceCounter();
        double ms = (double)(t1 - t0) * 1000.0 / freq;
        if (ms < best) {
            best = ms;
        }
    }
    out.ms = best;
    embi_layout_stats(eng, doc, &out.boxes, &out.frags, &out.arena_bytes);
    out.cache_bytes = embi_layout_cache_bytes(eng, doc);
    embi_layout_alloc_stats(eng, doc, &out.frag_allocs, &out.frag_reused);

    /* W4c, the first-paint path: the display list of the laid-out document, built BENCH_RUNS times into one pooled
     * list (the second and later builds reuse its arrays). An 800 x 600 viewport; no images on this page. */
    {
        embi_dl dl;
        embi_dl_init(&dl);
        embi_paint_cfg pcfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
        double best_paint = 1e300;
        for (int r = 0; r < BENCH_RUNS; r++) {
            uint64_t t0 = SDL_GetPerformanceCounter();
            (void)embi_paint_document(&dl, eng, doc, &pcfg);
            double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
            if (ms < best_paint) {
                best_paint = ms;
            }
        }
        out.paint_ms = best_paint;
        out.paint_items = dl.items.len;
        out.paint_bytes = embi_dl_bytes(&dl);
        embi_dl_free(&dl);
    }

    /* W6: one class change. The class of one span of the page is toggled; the restyle and the relayout are timed together
     * (the invalidation sets limit the restyle to the element and what depends on it). */
    {
        embi_element *el = find_class_s(embi_document_element(doc)->base.first_child);
        if (el != nullptr) {
            out.cls_paint = class_toggle(eng, style, doc, el, EMBI_STR("s hot"), EMBI_STR("s"));
            out.cls_layout = class_toggle(eng, style, doc, el, EMBI_STR("s pad"), EMBI_STR("s"));
        }
    }

    embi_layout_engine_destroy(eng);
    embi_style_forget_document(style, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(style);
    embi_tshim_destroy(text);
    return out;
}

int main(void)
{
    size_t html_len = 0;
    uint32_t elements = 0;
    char *html = build_html(&html_len, &elements);
    bench_result r = run_once(html, html_len, elements, BENCH_CSS);
    SDL_free(html);

    size_t depth_len = 0;
    uint32_t depth_elements = 0;
    char *depth_html = build_depth_html(&depth_len, &depth_elements, BENCH_DEPTH);
    bench_result rd = run_once(depth_html, depth_len, depth_elements, BENCH_DEPTH_CSS);
    SDL_free(depth_html);

    double bytes_per_box = r.boxes > 0 ? (double)r.arena_bytes / (double)r.boxes : 0;
    double cached_per_box = r.boxes > 0 ? (double)(r.arena_bytes + r.cache_bytes) / (double)r.boxes : 0;
    bool ok_ms = r.ms <= BENCH_TARGET_MS;
    bool ok_mem = bytes_per_box <= BENCH_TARGET_BYTES_PER_BOX;
    SDL_Log("document: %u elements, %u bytes of HTML", r.elements, (uint32_t)html_len);
    SDL_Log("created: %u boxes, %u fragments (%u asked for in the last layout, %u of them recycled)", r.boxes,
            r.frags, r.frag_allocs, r.frag_reused);
    bool ok_cold = r.cold_ms <= BENCH_TARGET_COLD_MS;
    bool ok_rebuild = r.build_ms <= BENCH_TARGET_REBUILD_MS;
    bool ok_vp = r.vp_ms <= BENCH_TARGET_REBUILD_MS;
    SDL_Log("first layout (cold: empty caches, fonts not loaded): %.2f ms (target <= %.0f ms) %s", r.cold_ms,
            BENCH_TARGET_COLD_MS, ok_cold ? "PASS" : "FAIL");
    {
        double tick_ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        double first = (double)r.first_ticks * tick_ms;
        double box = (double)r.cold.box_ticks * tick_ms, para = (double)r.cold.para_ticks * tick_ms;
        double text_other = box + para - first;
        double build = (double)r.cold.build_ticks * tick_ms, an = (double)r.cold.analysis_ticks * tick_ms;
        double own = r.cold_ms - box - para;
        SDL_Log("  text system created before the first layout (outside it): %.2f ms", r.setup_ms);
        SDL_Log("  text module, first shaped run (font loading and face setup): %.2f ms", first);
        SDL_Log("  text module, the other shaped runs: %.2f ms (%u text box runs %.2f ms, %u paragraph runs %.2f ms, less the first run)",
                text_other, r.cold.box_runs, box, r.cold.para_runs, para);
        SDL_Log("  layout's own code: %.2f ms = box tree build %.2f ms + %u paragraph analyses %.2f ms + the passes %.2f ms",
                own, build, r.cold.analyses, an, own - build - an);
    }
    SDL_Log("layout time, box tree rebuilt after a viewport width change: best of %d runs %.2f ms (target <= %.1f ms; %.1fx the kept-tree time) %s",
            BENCH_RUNS, r.vp_ms, BENCH_TARGET_REBUILD_MS, r.ms > 0 ? r.vp_ms / r.ms : 0.0, ok_vp ? "PASS" : "FAIL");
    SDL_Log("layout time, box tree rebuilt after a DOM change (styles resolved before): best of %d runs %.2f ms (target <= %.1f ms; %.1fx the kept-tree time) %s",
            BENCH_RUNS, r.build_ms, BENCH_TARGET_REBUILD_MS, r.ms > 0 ? r.build_ms / r.ms : 0.0, ok_rebuild ? "PASS" : "FAIL");
    SDL_Log("layout time, document unchanged (box tree kept): best of %d runs %.2f ms (target <= %.1f ms) %s",
            BENCH_RUNS, r.ms, BENCH_TARGET_MS, r.ms <= BENCH_TARGET_MS ? "PASS" : "FAIL");
    SDL_Log("live layout memory: %.1f bytes per box (%u bytes of box and fragment trees; target <= %.0f) %s",
            bytes_per_box, (uint32_t)r.arena_bytes, BENCH_TARGET_BYTES_PER_BOX, ok_mem ? "PASS" : "FAIL");
    SDL_Log("  with the derived caches the layout keeps (inline analysis, tables, scratch pool: %u bytes): %.1f bytes per box (no target)",
            (uint32_t)r.cache_bytes, cached_per_box);
    /* W4c: the display list of the bench page (best of BENCH_RUNS builds). */
    bool ok_paint = r.paint_ms <= BENCH_TARGET_PAINT_MS;
    SDL_Log("display list (W4c): best of %d builds %.2f ms (target <= %.1f ms) %s, %u items, %.1f bytes per box", BENCH_RUNS,
            r.paint_ms, BENCH_TARGET_PAINT_MS, ok_paint ? "PASS" : "FAIL", r.paint_items,
            r.boxes > 0 ? (double)r.paint_bytes / (double)r.boxes : 0.0);
    bool ok_class = r.cls_paint.ms <= BENCH_TARGET_CLASS_MS;
    SDL_Log("one class change (W6, a class that changes paint): restyle + relayout, best of %d runs %.3f ms (target <= %.1f ms) %s, %u of %u elements restyled, layout kind %u (1 = paint only, fragments kept)",
            BENCH_RUNS * 2, r.cls_paint.ms, BENCH_TARGET_CLASS_MS, ok_class ? "PASS" : "FAIL", r.cls_paint.restyled, r.elements,
            r.cls_paint.kind);
    SDL_Log("one class change (W6, a class that changes the padding of an inline box): %.3f ms (no target: each section holds floats and absolutely positioned badges, which the partial relayout cannot place, so the kept tree is laid out again), %u of %u elements restyled, layout kind %u, %u boxes laid out",
            r.cls_layout.ms, r.cls_layout.restyled, r.elements, r.cls_layout.kind, r.cls_layout.laid_out);
    /* Informational, no target: nested BFC roots beside floats, one float per level (W3b). */
    SDL_Log("nested float depth %u: %u elements, %u boxes, best of %d runs %.2f ms (no target)",
            BENCH_DEPTH, rd.elements, rd.boxes, BENCH_RUNS, rd.ms);
    return ok_ms && ok_mem && ok_cold && ok_rebuild && ok_vp && ok_paint && ok_class ? 0 : 1;
}
