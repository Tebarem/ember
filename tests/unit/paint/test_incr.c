/*
 * The incremental display list (W6b, src/paint/incr.c): after a paint-only update the stacking contexts that hold the
 * changed elements are built again and spliced into the cached list. The result must equal a full build (embi_dl_equal).
 */
#include "paint_harness.h"

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static void set_class(ph *h, const char *id, const char *cls)
{
    embi_element *el = ph_id(h, id);
    EMB_REQUIRE(el != nullptr);
    EMB_REQUIRE(embi_el_set_attribute(el, S("class"), embi_dstr_from_utf8(h->doc->slab, S(cls))) == EMBI_DOM_OK);
}

static const char *k_page =
    "<style>"
    ".r{color:red} .g{color:green;background-color:#cfc} .b{background-color:#00f} .h{opacity:.5} .h2{opacity:.8}"
    ".t{transform:rotate(3deg)} .t2{transform:rotate(10deg)} .o{outline:2px solid #f0f}"
    "div{padding:4px} .z{position:relative;z-index:2;background-color:#eee} .n{position:relative;z-index:-1;background-color:#ddd}"
    "</style>"
    "<div id=root><div id=a>alpha<div id=a1 class=z>inner z <span id=sp>span</span></div></div>"
    "<div id=b class=h>half <div id=b1 class=t>rotated</div><div id=b2>plain</div></div>"
    "<div id=c><div id=c1 class=n>negative</div><div id=c2 class=z>top</div> text <b id=bold>bold</b></div></div>";

static void paint_both(ph *h, embi_paint_cache *c, embi_dl *out, bool allow)
{
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_paint_document_incr(c, out, h->eng, h->doc, &cfg, allow);
}

/* Prints the first item where two lists differ (called when they do). */
static void report_first_diff(const embi_dl *got, const embi_dl *fresh)
{
    SDL_Log("  items got=%u fresh=%u ctxs got=%u fresh=%u", got->items.len, fresh->items.len, got->ctxs.len, fresh->ctxs.len);
    uint32_t n = got->items.len > fresh->items.len ? got->items.len : fresh->items.len;
    for (uint32_t i = 0; i < n; i++) {
        const embi_ditem *x = i < got->items.len ? &got->items.data[i] : nullptr;
        const embi_ditem *y = i < fresh->items.len ? &fresh->items.data[i] : nullptr;
        bool same = x != nullptr && y != nullptr && x->kind == y->kind && memcmp(x->rect, y->rect, sizeof x->rect) == 0 &&
                    memcmp(x->colors, y->colors, sizeof x->colors) == 0 && x->clip == y->clip && x->xform == y->xform &&
                    x->aux == y->aux;
        if (!same) {
            SDL_Log("  first differing item %u: kind %d vs %d, rect0 %g vs %g", i, x ? (int)x->kind : -1, y ? (int)y->kind : -1,
                    x ? (double)x->rect[0] : 0.0, y ? (double)y->rect[0] : 0.0);
            return;
        }
    }
}

static bool equals_full(ph *h, const embi_dl *got)
{
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_dl fresh;
    embi_dl_init(&fresh);
    embi_paint_document(&fresh, h->eng, h->doc, &cfg);
    bool eq = embi_dl_equal(got, &fresh);
    if (!eq) {
        report_first_diff(got, &fresh);
    }
    embi_dl_free(&fresh);
    return eq;
}

EMB_TEST(paint_only_change_rebuilds_one_context)
{
    ph h = ph_make(k_page, "");
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    paint_both(&h, c, &out, false);
    EMB_CHECK(!embi_paint_cache_last_incremental(c));
    EMB_CHECK(equals_full(&h, &out));

    set_class(&h, "a1", "z g"); /* inside the context of #a1 (z-index 2) */
    (void)embi_layout_document(h.eng, h.doc);
    paint_both(&h, c, &out, true);
    EMB_CHECK(embi_paint_cache_last_incremental(c));
    EMB_CHECK(embi_paint_cache_rebuilt(c) == 1);
    EMB_CHECK(equals_full(&h, &out));

    set_class(&h, "b", "h2"); /* opacity .5 to .8: the layer of #b */
    (void)embi_layout_document(h.eng, h.doc);
    paint_both(&h, c, &out, true);
    EMB_CHECK(embi_paint_cache_last_incremental(c));
    EMB_CHECK(equals_full(&h, &out));

    set_class(&h, "b1", "t2"); /* a transform angle */
    (void)embi_layout_document(h.eng, h.doc);
    paint_both(&h, c, &out, true);
    EMB_CHECK(embi_paint_cache_last_incremental(c));
    EMB_CHECK(equals_full(&h, &out));

    /* a layout change: the cache builds the list again */
    set_class(&h, "b2", "z");
    (void)embi_layout_document(h.eng, h.doc);
    paint_both(&h, c, &out, true);
    EMB_CHECK(equals_full(&h, &out));
    embi_dl_free(&out);
    embi_paint_cache_free(c);
    ph_free(&h);
}

/* Directed cases: each step is a class change on one element; the list after it must equal a full build, and the steps
 * that only change paint must have been incremental. */
typedef struct pstep {
    const char *id, *cls;
    bool incremental; /* the cache must have taken the incremental path */
} pstep;

static void run_steps(const char *page, const pstep *steps, size_t n)
{
    ph h = ph_make(page, "");
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    paint_both(&h, c, &out, false);
    EMB_CHECK(equals_full(&h, &out));
    for (size_t i = 0; i < n; i++) {
        set_class(&h, steps[i].id, steps[i].cls);
        (void)embi_layout_document(h.eng, h.doc);
        paint_both(&h, c, &out, true);
        if (steps[i].incremental && !embi_paint_cache_last_incremental(c)) {
            SDL_Log("  step %u (#%s class=[%s]) was expected to be incremental", (unsigned)i, steps[i].id, steps[i].cls);
            EMB_CHECK(false);
        }
        if (!equals_full(&h, &out)) {
            SDL_Log("  step %u (#%s class=[%s]) differs from a full build", (unsigned)i, steps[i].id, steps[i].cls);
            EMB_CHECK(false);
            break;
        }
    }
    embi_dl_free(&out);
    embi_paint_cache_free(c);
    ph_free(&h);
}

EMB_TEST(nested_stacking_contexts)
{
    static const char *page =
        "<style>div{padding:4px} .z{position:relative;z-index:1;background-color:#eee} .y{position:relative;z-index:3;background-color:#ccd}"
        ".r{color:red;background-color:#fcc} .g{background-color:#cfc} .h{opacity:.6}</style>"
        "<div id=o class=z>outer<div id=m class=y>middle<div id=i class=z>inner <span id=s>s</span></div></div>"
        "<div id=sib class=y>sibling</div></div><div id=after>after</div>";
    static const pstep steps[] = {
        {"i", "z r", true}, {"m", "y g", true}, {"o", "z r", true}, {"s", "r", true}, {"i", "z", true},
        {"m", "y h", false}, {"i", "z g", true}, {"sib", "y r", true}, {"o", "z h", false}, {"after", "g", true},
    };
    run_steps(page, steps, sizeof steps / sizeof steps[0]);
}

EMB_TEST(opacity_crossing_one)
{
    static const char *page =
        "<style>div{padding:6px} .h{opacity:.5} .f{opacity:1} .o0{opacity:0} .g{background-color:#cfc} .r{color:red}</style>"
        "<div id=a class=g>plain<div id=b class=r>child<div id=c>grand</div></div></div><div id=d>tail</div>";
    /* Layers (opacity below 1, transforms) and the root context make the cache build the whole list: only equality is asserted.
     * the layer appears when opacity drops below 1 and disappears when it comes back to 1 (a stacking context either way) */
    static const pstep steps[] = {
        {"a", "g h", false}, {"a", "g", false}, {"b", "r h", false}, {"b", "r f", false}, {"b", "r o0", false},
        {"a", "g h", false}, {"b", "r", false}, {"a", "g f", false}, {"c", "h", false}, {"a", "h", false}, {"c", "", false}, {"a", "", false},
    };
    run_steps(page, steps, sizeof steps / sizeof steps[0]);
}

EMB_TEST(transform_on_a_layer)
{
    static const char *page =
        "<style>div{padding:6px} .t{transform:rotate(5deg)} .s{transform:scale(1.2)} .h{opacity:.7} .g{background-color:#cfc}"
        ".z{position:relative;z-index:1;background-color:#eee} .o{overflow:hidden;height:30px}</style>"
        "<div id=a>a<div id=b class=z>b<div id=c>c</div></div><div id=d class=o>clipped text that is long<div id=e>e</div></div></div>";
    static const pstep steps[] = {
        {"b", "z t", false}, {"b", "z t h", false}, {"c", "t", false}, {"b", "z s", false}, {"d", "o t", false},
        {"e", "g", true}, {"d", "o", false}, {"b", "z", false}, {"c", "", false}, {"a", "t h", false}, {"a", "", false},
    };
    run_steps(page, steps, sizeof steps / sizeof steps[0]);
}

EMB_TEST(fuzz_incremental_paint_equals_full)
{
    /* the library's own verification is off: the comparison with a full build here is the independent one */
    embi_incr_set_verify(0);
    ph h = ph_make(k_page, "");
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    paint_both(&h, c, &out, false);
    static const char *ids[] = {"root", "a", "a1", "sp", "b", "b1", "b2", "c", "c1", "c2", "bold"};
    static const char *cls[] = {"", "r", "g", "b", "h", "h2", "t", "t2", "o", "z", "n", "z r", "n g", "t o", "h r b", "z h2"};
    uint32_t rng = 99;
    uint32_t incr = 0;
    for (int it = 0; it < 1000; it++) {
        rng = rng * 1664525u + 1013904223u;
        const char *id = ids[(rng >> 8) % 11u];
        rng = rng * 1664525u + 1013904223u;
        const char *cl = cls[(rng >> 8) % 16u];
        set_class(&h, id, cl);
        (void)embi_layout_document(h.eng, h.doc);
        paint_both(&h, c, &out, true);
        incr += embi_paint_cache_last_incremental(c) ? 1u : 0u;
        if (!equals_full(&h, &out)) {
            embi_layout_incr_stats st;
            embi_layout_incr_stats_get(h.eng, h.doc, &st);
            SDL_Log("  paint differs after step %d: #%s class=[%s] incremental=%d layout kind=%u partial=%d rebuilt ctxs=%u", it,
                    id, cl, (int)embi_paint_cache_last_incremental(c), st.last_kind, (int)st.partial,
                    embi_paint_cache_rebuilt(c));
            EMB_REQUIRE(false);
        }
    }
    EMB_CHECK(embi_incr_verify_failures() == 0);
    EMB_CHECK(incr > 100);
    embi_dl_free(&out);
    embi_paint_cache_free(c);
    ph_free(&h);
}

/* R4c: a page with an inline SVG icon keeps incremental paint; the copy of the list shares the path references. */
#if EMBER_SVG
EMB_TEST(svg_paths_stay_incremental)
{
    const char *page =
        "<style>.h{opacity:.5} .h2{opacity:.8} .z{position:relative;z-index:2}</style>"
        "<div id=a><svg width=24 height=24 viewBox=\"0 0 24 24\"><path d=\"M2 2 L22 2 L12 22 Z\" fill=\"#f00\" stroke=\"#00f\" stroke-width=\"2\"/></svg></div>"
        "<div id=b class=\"h z\">half</div>";
    {
        ph warm = ph_make(page, "");
        ph_free(&warm);
    }
    size_t live0 = embi_mem_live();
    ph h = ph_make(page, "");
    {
        embi_paint_cache *c = embi_paint_cache_create();
        embi_dl out;
        embi_dl_init(&out);
        paint_both(&h, c, &out, false);
        EMB_CHECK(out.paths.len == 2); /* the fill and the stroke item: one path entry each, one shared path */
        EMB_CHECK(equals_full(&h, &out));
        set_class(&h, "b", "h2 z");
        (void)embi_layout_document(h.eng, h.doc);
        paint_both(&h, c, &out, true);
        EMB_CHECK(embi_paint_cache_last_incremental(c));
        EMB_CHECK(out.paths.len >= 2);
        EMB_CHECK(equals_full(&h, &out));
        embi_dl_free(&out);
        embi_paint_cache_free(c);
    }
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == live0); /* no path leaked by the shared references */
}
#endif

EMB_TEST_MAIN()
