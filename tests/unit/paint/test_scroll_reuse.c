/*
 * A scroll that changes nothing but scroll offsets does not build the display list again (scroll60): the offsets are
 * translations in the list's transforms, and embi_paint_document_scroll moves them in place. The moved list must equal a
 * list built from scratch at the new offsets, whatever scrolls (the viewport, an element, nested elements), and the cases
 * that depend on the offset in another way (sticky boxes, fixed backgrounds) must build the list again.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/damage.h"
#include "paint/paint_int.h"
#include "paint/scroll.h"

#include <math.h>

static embi_paint_cfg cfg800(void)
{
    embi_paint_cfg c = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    return c;
}

static embi_frag *frag_of(ph *h, const char *id)
{
    embi_element *el = ph_id(h, id);
    return el != nullptr && el->box != nullptr ? ((embi_box *)el->box)->frag : nullptr;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* A tall page: 400 rows of 20 px in the document, so the viewport scrolls. */
static char *rows_page(int n, const char *wrap_open, const char *wrap_close)
{
    size_t cap = (size_t)n * 80 + 512;
    char *s = SDL_malloc(cap);
    size_t len = (size_t)SDL_snprintf(s, cap, "%s", wrap_open);
    for (int i = 0; i < n; i++) {
        len += (size_t)SDL_snprintf(s + len, cap - len, "<div style=\"height:20px;background:#%02x%02x%02x\">row %d</div>",
                                    (i * 7) & 0xFF, (i * 13) & 0xFF, (i * 29) & 0xFF, i);
    }
    SDL_snprintf(s + len, cap - len, "%s", wrap_close);
    return s;
}

/* The moved list against one built from scratch at the offsets now in the layout. */
static bool equals_fresh(ph *h, const embi_dl *got)
{
    embi_paint_cfg cfg = cfg800();
    embi_dl fresh;
    embi_dl_init(&fresh);
    embi_paint_document(&fresh, h->eng, h->doc, &cfg);
    bool eq = embi_dl_equal(got, &fresh);
    if (!eq) {
        SDL_Log("  items got=%u fresh=%u xforms got=%u fresh=%u", got->items.len, fresh.items.len, got->xforms.len,
                fresh.xforms.len);
    }
    embi_dl_free(&fresh);
    return eq;
}

/* The page's effective top of the first item with color rgba: its rect mapped by its transform. */
static float eff_y(const embi_dl *d, uint32_t rgba)
{
    int i = ph_fill_of(d, rgba);
    if (i < 0) {
        return NAN;
    }
    const embi_ditem *it = &d->items.data[i];
    return it->rect[1] + (it->xform != 0 ? d->xforms.data[it->xform].m[5] : 0.0f);
}

EMB_TEST(scroll_reuse_viewport_scroll_moves_the_list_without_building)
{
    char *html = rows_page(400, "", "");
    ph h = ph_make(html, "");
    SDL_free(html);
    embi_paint_cfg cfg = cfg800();
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
    EMB_CHECK(embi_paint_cache_count(c, false) == 1);
    EMB_CHECK(out.scrolls.len == 1); /* the viewport scrolls */
    uint32_t content = out.items.len;
    EMB_CHECK(equals_fresh(&h, &out));

    embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_REQUIRE(root != nullptr);
    const float steps[4] = {300.0f, -120.0f, 1000.0f, -5000.0f};
    for (int i = 0; i < 4; i++) {
        EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, steps[i], true, 800.0f, 600.0f));
        embi_scroll_damage dmg;
        EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 2);
        EMB_CHECK(dmg.full); /* the viewport scrolled */
        EMB_CHECK(embi_paint_cache_count(c, false) == 1); /* nothing was built */
        EMB_CHECK(embi_paint_cache_count(c, true) == 0);
        EMB_CHECK(out.items.len == content);
        EMB_CHECK(equals_fresh(&h, &out));
    }
    /* The same offsets again: nothing moved. */
    embi_scroll_damage dmg;
    EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 1);
    embi_paint_cache_free(c);
    embi_dl_free(&out);
    ph_free(&h);
}

EMB_TEST(scroll_reuse_element_scroll_damages_only_the_container)
{
    char *html = rows_page(100, "<div id=s style=\"overflow:auto;width:200px;height:100px;margin:30px 0 0 40px\">", "</div><p>after</p>");
    ph h = ph_make(html, "");
    SDL_free(html);
    embi_paint_cfg cfg = cfg800();
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
    uint32_t content = out.items.len;
    EMB_CHECK(out.scrolls.len >= 1);

    embi_frag *s = frag_of(&h, "s");
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 70.0f, false, 800.0f, 600.0f));
    embi_scroll_damage dmg;
    EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 2);
    EMB_CHECK(!dmg.full && dmg.n == 1);
    EMB_CHECK(near(dmg.r[0][0], 40.0f) && near(dmg.r[0][1], 30.0f) && near(dmg.r[0][2], 240.0f) && near(dmg.r[0][3], 130.0f));
    EMB_CHECK(embi_paint_cache_count(c, false) == 1);
    EMB_CHECK(equals_fresh(&h, &out));
    /* Row 4 (index 3) is at 60 px in the container: its fill is now at 30 + 60 - 70 = 20 px. */
    EMB_CHECK(near(eff_y(&out, 0xFF000000u | (87u << 16) | (39u << 8) | 21u), 30.0f + 60.0f - 70.0f));
    embi_paint_cache_free(c);
    embi_dl_free(&out);
    ph_free(&h);
}

EMB_TEST(scroll_reuse_nested_containers_move_together)
{
    char *inner = rows_page(40, "<div id=in style=\"overflow:auto;width:150px;height:80px\">", "</div>");
    char *html = rows_page(30, "<div id=out style=\"overflow:auto;width:300px;height:200px;margin-left:10px\"><div style=\"height:50px\"></div>", "");
    size_t n = strlen(html) + strlen(inner) + 64;
    char *all = SDL_malloc(n);
    SDL_snprintf(all, n, "%s%s</div>", html, inner);
    SDL_free(html);
    SDL_free(inner);
    ph h = ph_make(all, "");
    SDL_free(all);
    embi_paint_cfg cfg = cfg800();
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
    uint32_t content = out.items.len;
    EMB_CHECK(out.scrolls.len >= 2);
    embi_frag *o = frag_of(&h, "out"), *in = frag_of(&h, "in");
    EMB_REQUIRE(o != nullptr && in != nullptr);

    const struct {
        embi_frag *f;
        float dy;
    } steps[] = {{in, 33.0f}, {o, 41.0f}, {in, -10.0f}, {o, 17.0f}, {in, 200.0f}};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, steps[i].f, 0.0f, steps[i].dy, false, 800.0f, 600.0f));
        embi_scroll_damage dmg;
        EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 2);
        EMB_CHECK(!dmg.full && dmg.n >= 1);
        EMB_CHECK(equals_fresh(&h, &out));
    }
    EMB_CHECK(embi_paint_cache_count(c, false) == 1);
    embi_paint_cache_free(c);
    embi_dl_free(&out);
    ph_free(&h);
}

EMB_TEST(scroll_reuse_fractional_offsets_snap_to_device_pixels)
{
    char *html = rows_page(100, "", "");
    ph h = ph_make(html, "");
    SDL_free(html);
    embi_paint_cfg cfg = cfg800();
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
    uint32_t content = out.items.len;
    embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, 10.4f, true, 800.0f, 600.0f));
    embi_scroll_damage dmg;
    EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 2);
    EMB_CHECK(equals_fresh(&h, &out));
    /* the content is 10 px up, not 10.4 */
    EMB_CHECK(near(eff_y(&out, 0xFF000000u | (29u << 16) | (13u << 8) | 7u), 20.0f - 10.0f)); /* row 1 */
    embi_paint_cache_free(c);
    embi_dl_free(&out);
    ph_free(&h);
}

EMB_TEST(scroll_reuse_sticky_and_fixed_background_build_the_list_again)
{
    /* A sticky header follows the offset by its own rule, and a fixed background is placed in the viewport: neither is a
     * translation, so the list is built for each offset. */
    static const char *pages[2] = {
        "<div style=\"position:sticky;top:0;height:30px;background:#f00\">head</div>",
        "<div style=\"height:100px;background:#0f0 url(data:image/gif;base64,R0lGODlhAQABAAAAACw=) fixed\"></div>",
    };
    for (int p = 0; p < 2; p++) {
        char *html = rows_page(100, pages[p], "");
        ph h = ph_make(html, "");
        SDL_free(html);
        embi_paint_cfg cfg = cfg800();
        embi_paint_cache *c = embi_paint_cache_create();
        embi_dl out;
        embi_dl_init(&out);
        embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
        uint32_t content = out.items.len;
        if (p == 0) {
            EMB_CHECK(out.scroll_rigid);
        }
        embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
        EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, 250.0f, true, 800.0f, 600.0f));
        embi_scroll_damage dmg;
        if (p == 0) {
            EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 0);
        }
        /* The ordinary path builds it and it equals a fresh list. */
        embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
        EMB_CHECK(embi_paint_cache_count(c, false) == 2);
        EMB_CHECK(equals_fresh(&h, &out));
        embi_paint_cache_free(c);
        embi_dl_free(&out);
        ph_free(&h);
    }
}

EMB_TEST(scroll_reuse_refused_when_the_page_changed)
{
    char *html = rows_page(100, "<div id=x>text</div>", "");
    ph h = ph_make(html, "");
    SDL_free(html);
    embi_paint_cfg cfg = cfg800();
    embi_paint_cache *c = embi_paint_cache_create();
    embi_dl out;
    embi_dl_init(&out);
    embi_paint_document_incr(c, &out, h.eng, h.doc, &cfg, false);
    uint32_t content = out.items.len;
    embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, 100.0f, true, 800.0f, 600.0f));
    /* another viewport: the list was made for 800 x 600 */
    embi_paint_cfg other = cfg;
    other.viewport_h = 500.0f;
    embi_scroll_damage dmg;
    EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &other, &dmg) == 0);
    /* a content that cannot be cut back to the page */
    EMB_CHECK(embi_paint_document_scroll(c, &out, out.items.len + 1, h.eng, h.doc, &cfg, &dmg) == 0);
    EMB_CHECK(embi_paint_document_scroll(c, &out, content, h.eng, h.doc, &cfg, &dmg) == 2);
    embi_paint_cache_free(c);
    embi_dl_free(&out);
    ph_free(&h);
}

EMB_TEST_MAIN()
