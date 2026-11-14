/*
 * Cached-layer decisions and the layer table of W4d (layer.c): will-change, fixed positioning and scroll overflow on
 * real styles, the record and composite decisions of the table, its budget, and the content hash that a scroll
 * leaves unchanged.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/layer.h"
#include "paint/paint_int.h"

static const embi_style *style_of(ph *h, const char *id)
{
    embi_element *el = ph_id(h, id);
    return el != nullptr && el->box != nullptr ? ((const embi_box *)el->box)->style : nullptr;
}

EMB_TEST(will_change_transform_and_opacity_get_a_cached_layer)
{
    ph h = ph_make("<div id=a style=\"will-change:transform;height:10px\"></div>"
                   "<div id=b style=\"will-change:opacity;height:10px\"></div>"
                   "<div id=c style=\"will-change:top;height:10px\"></div>"
                   "<div id=d style=\"height:10px\"></div>"
                   "<div id=e style=\"will-change:auto;height:10px\"></div>",
                   "");
    EMB_CHECK(embi_layer_wants_cache(style_of(&h, "a"), false, 0.0f));
    EMB_CHECK(embi_layer_wants_cache(style_of(&h, "b"), false, 0.0f));
    EMB_CHECK(!embi_layer_wants_cache(style_of(&h, "c"), false, 0.0f));
    EMB_CHECK(!embi_layer_wants_cache(style_of(&h, "d"), false, 0.0f));
    EMB_CHECK(!embi_layer_wants_cache(style_of(&h, "e"), false, 0.0f));
    ph_free(&h);
}

EMB_TEST(fixed_position_boxes_get_a_cached_layer)
{
    ph h = ph_make("<div id=f style=\"position:fixed;top:0;height:10px\"></div>"
                   "<div id=g style=\"position:absolute;top:0;height:10px\"></div>",
                   "");
    EMB_CHECK(embi_layer_wants_cache(style_of(&h, "f"), false, 0.0f));
    EMB_CHECK(!embi_layer_wants_cache(style_of(&h, "g"), false, 0.0f));
    ph_free(&h);
}

EMB_TEST(scroll_containers_need_overflow_beyond_the_threshold)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\"><div style=\"height:900px\"></div></div>",
                   "");
    const embi_style *s = style_of(&h, "s");
    EMB_CHECK(!embi_layer_wants_cache(s, true, 0.0f));
    EMB_CHECK(!embi_layer_wants_cache(s, true, EMBI_LAYER_SCROLL_THRESHOLD));
    EMB_CHECK(embi_layer_wants_cache(s, true, EMBI_LAYER_SCROLL_THRESHOLD + 1.0f));
    /* Not a scroll container: the overflow does not count. */
    EMB_CHECK(!embi_layer_wants_cache(s, false, 5000.0f));
    ph_free(&h);
}

EMB_TEST(cache_records_new_resized_and_changed_layers_and_composites_the_rest)
{
    embi_layer_cache c;
    embi_layer_cache_init(&c, 1u << 20);
    embi_layer_cache_begin(&c);
    EMB_CHECK(embi_layer_cache_touch(&c, 1, 100, 100, 7));
    embi_layer_cache_end(&c);
    EMB_CHECK_EQ_INT((int)c.bytes, 100 * 100 * 4);

    embi_layer_cache_begin(&c);
    EMB_CHECK(!embi_layer_cache_touch(&c, 1, 100, 100, 7));
    embi_layer_cache_end(&c);

    embi_layer_cache_begin(&c);
    EMB_CHECK(embi_layer_cache_touch(&c, 1, 100, 100, 8));
    embi_layer_cache_end(&c);

    embi_layer_cache_begin(&c);
    EMB_CHECK(embi_layer_cache_touch(&c, 1, 200, 100, 8));
    embi_layer_cache_end(&c);
    EMB_CHECK_EQ_INT((int)c.bytes, 200 * 100 * 4);
    EMB_CHECK_EQ_INT((int)c.ents.len, 1);

    /* A layer that no frame paints is dropped, with its texture. */
    embi_layer_cache_begin(&c);
    embi_layer_cache_end(&c);
    EMB_CHECK(embi_layer_cache_find(&c, 1) == nullptr);
    EMB_CHECK_EQ_INT((int)c.bytes, 0);
    EMB_CHECK_EQ_INT((int)c.evicted_last, 1);
    embi_layer_cache_free(&c);
}

EMB_TEST(cache_evicts_least_recently_used_over_budget)
{
    embi_layer_cache c;
    embi_layer_cache_init(&c, 100 * 100 * 4 + 10);
    embi_layer_cache_begin(&c);
    EMB_CHECK(embi_layer_cache_touch(&c, 1, 100, 100, 1));
    EMB_CHECK(embi_layer_cache_touch(&c, 2, 100, 100, 1));
    embi_layer_cache_end(&c);
    EMB_CHECK(c.bytes <= c.budget);
    EMB_CHECK_EQ_INT((int)c.ents.len, 1);
    EMB_CHECK(embi_layer_cache_find(&c, 1) == nullptr);
    EMB_CHECK(embi_layer_cache_find(&c, 2) != nullptr);

    /* Touching the survivor again in a frame keeps it. */
    embi_layer_cache_begin(&c);
    EMB_CHECK(!embi_layer_cache_touch(&c, 2, 100, 100, 1));
    embi_layer_cache_end(&c);
    EMB_CHECK(embi_layer_cache_find(&c, 2) != nullptr);
    embi_layer_cache_free(&c);
    EMB_CHECK_EQ_INT((int)c.bytes, 0);
}

static void add_fill(embi_pst *p, float x, float y, uint32_t rgba)
{
    float r[4] = {x, y, 10, 10};
    float radii[4] = {0, 0, 0, 0};
    EMB_CHECK(embi_pb_fill(p, r, radii, rgba, 0, 0));
}

EMB_TEST(content_hash_ignores_a_translation_and_sees_a_change)
{
    embi_dl a, b, c, d;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&c, 0, sizeof c);
    memset(&d, 0, sizeof d);
    embi_dl_init(&a);
    embi_dl_init(&b);
    embi_dl_init(&c);
    embi_dl_init(&d);
    embi_pst pa, pb, pc, pd;
    memset(&pa, 0, sizeof pa);
    memset(&pb, 0, sizeof pb);
    memset(&pc, 0, sizeof pc);
    memset(&pd, 0, sizeof pd);
    pa.d = &a;
    pb.d = &b;
    pc.d = &c;
    pd.d = &d;
    add_fill(&pa, 10, 10, PH_RED);
    add_fill(&pa, 40, 10, PH_BLUE);
    /* The same content moved by (100, 50), as a scroll moves a container's children. */
    add_fill(&pb, 110, 60, PH_RED);
    add_fill(&pb, 140, 60, PH_BLUE);
    /* One color changed. */
    add_fill(&pc, 10, 10, PH_RED);
    add_fill(&pc, 40, 10, PH_GREEN);
    /* One item moved relative to the other. */
    add_fill(&pd, 10, 10, PH_RED);
    add_fill(&pd, 45, 10, PH_BLUE);
    uint64_t ha = embi_layer_content_hash(&a, 0, a.items.len);
    EMB_CHECK(ha == embi_layer_content_hash(&b, 0, b.items.len));
    EMB_CHECK(ha != embi_layer_content_hash(&c, 0, c.items.len));
    EMB_CHECK(ha != embi_layer_content_hash(&d, 0, d.items.len));
    embi_dl_free(&a);
    embi_dl_free(&b);
    embi_dl_free(&c);
    embi_dl_free(&d);
}

/* Item 0 of d hashed at origin (0, 0), then moved by (dx, dy) in the positions kind says it holds, and hashed with the
 * origin (dx, dy): equal when the hash is translation-invariant. */
static void move_item(embi_ditem *it, float dx, float dy)
{
    it->rect[0] += dx;
    it->rect[1] += dy;
    switch (it->kind) {
    case EMBI_DI_TEXT:
        it->f[0] += dx;
        it->f[1] += dy;
        break;
    case EMBI_DI_GRADIENT:
        it->f[0] += dx;
        it->f[1] += dy;
        if (it->sub == EMBI_GRAD_LINEAR_DL) {
            it->f[2] += dx;
            it->f[3] += dy;
        }
        break;
    case EMBI_DI_IMAGE:
        if (it->flags & EMBI_DF_TILE) {
            it->f[0] += dx;
            it->f[1] += dy;
        }
        break;
    default:
        break;
    }
}

static void check_translation_invariant(embi_dl *d, uint32_t i)
{
    uint64_t h0 = embi_layer_item_hash(d, i, 0.0f, 0.0f);
    move_item(&d->items.data[i], 37.0f, -12.5f);
    EMB_CHECK(embi_layer_item_hash(d, i, 37.0f, -12.5f) == h0);
    EMB_CHECK(embi_layer_item_hash(d, i, 0.0f, 0.0f) != h0);
}

EMB_TEST(item_hash_ignores_a_translation_of_text)
{
    ph h = ph_make("<p style=\"margin:0;font:20px sans-serif\">Hello world</p>", "");
    uint32_t t = UINT32_MAX;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_TEXT) {
            t = i;
            break;
        }
    }
    EMB_CHECK(t != UINT32_MAX);
    if (t != UINT32_MAX) {
        check_translation_invariant(&h.dl, t);
        /* The whole range translates alike, text included. */
        uint64_t before = embi_layer_content_hash(&h.dl, 0, h.dl.items.len);
        for (uint32_t i = 0; i < h.dl.items.len; i++) {
            move_item(&h.dl.items.data[i], 100.0f, 50.0f);
        }
        EMB_CHECK(embi_layer_content_hash(&h.dl, 0, h.dl.items.len) == before);
    }
    ph_free(&h);
}

EMB_TEST(item_hash_sees_a_change_of_text_content)
{
    ph a = ph_make("<p style=\"margin:0;font:20px sans-serif\">Hello world</p>", "");
    ph b = ph_make("<p style=\"margin:0;font:20px sans-serif\">Hello there</p>", "");
    uint64_t ha = 0, hb = 0;
    bool fa = false, fb = false;
    for (uint32_t i = 0; i < a.dl.items.len && !fa; i++) {
        if (a.dl.items.data[i].kind == EMBI_DI_TEXT) {
            ha = embi_layer_item_hash(&a.dl, i, 0.0f, 0.0f);
            fa = true;
        }
    }
    for (uint32_t i = 0; i < b.dl.items.len && !fb; i++) {
        if (b.dl.items.data[i].kind == EMBI_DI_TEXT) {
            hb = embi_layer_item_hash(&b.dl, i, 0.0f, 0.0f);
            fb = true;
        }
    }
    EMB_CHECK(fa && fb);
    EMB_CHECK(ha != hb);
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(item_hash_ignores_a_translation_of_gradients_and_tiled_images)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    float rect[4] = {20, 30, 100, 60};
    float radii[4] = {0, 0, 0, 0};
    embi_dstop stops[2] = {{0.0f, PH_RED}, {1.0f, PH_BLUE}};
    float lin[6] = {20, 30, 120, 30, 0, 0};
    embi_pb_gradient(&p, EMBI_GRAD_LINEAR_DL, rect, radii, lin, stops, 2, false, 0, 0);
    float rad[6] = {70, 60, 50, 30, 0, 0};
    embi_pb_gradient(&p, EMBI_GRAD_RADIAL_DL, rect, radii, rad, stops, 2, false, 0, 0);
    float con[6] = {70, 60, 0.5f, 0, 0, 0};
    embi_pb_gradient(&p, EMBI_GRAD_CONIC_DL, rect, radii, con, stops, 2, false, 0, 0);
    embi_ditem *img = embi_pb_item(&p, EMBI_DI_IMAGE, 0, 0);
    EMB_CHECK(img != nullptr);
    uint32_t n = d.items.len;
    if (img != nullptr) {
        img = &d.items.data[n - 1];
        memcpy(img->rect, rect, sizeof rect);
        img->flags = EMBI_DF_TILE;
        img->colors[0] = 0xFFFFFFFFu;
        float f[6] = {20, 30, 16, 16, 16, 16};
        memcpy(img->f, f, sizeof f);
    }
    for (uint32_t i = 0; i < d.items.len; i++) {
        check_translation_invariant(&d, i);
    }
    /* A different gradient geometry (same rect) is a different item. */
    uint64_t h0 = embi_layer_item_hash(&d, 0, 0.0f, 0.0f);
    d.items.data[0].f[2] += 5.0f;
    EMB_CHECK(embi_layer_item_hash(&d, 0, 0.0f, 0.0f) != h0);
    embi_dl_free(&d);
}

EMB_TEST(cache_and_hash_leave_no_live_memory)
{
    size_t base = embi_mem_live();
    embi_layer_cache c;
    embi_layer_cache_init(&c, 1u << 20);
    embi_layer_cache_begin(&c);
    EMB_CHECK(embi_layer_cache_touch(&c, 1, 10, 10, 1));
    EMB_CHECK(embi_layer_cache_touch(&c, 2, 10, 10, 1));
    embi_layer_cache_end(&c);
    embi_layer_cache_free(&c);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
