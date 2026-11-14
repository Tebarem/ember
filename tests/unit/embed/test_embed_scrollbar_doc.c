/*
 * The styled ::-webkit-scrollbar of a scroll container without a GPU or a view: the document alone (its layout and the doc.c
 * bar API). The gutter is in clientWidth and not in scrollWidth, a bar is found only where the page's own hit test lands on its
 * scroller or inside it (a box over it takes the point), and the scrollers the bar API reads follow a scroll and a new layout.
 */
#include "emb_test.h"

#include "embed/embed_int.h"
#include "embed/script_int.h"
#include "layout/layout.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

static const char k_page[] =
    "<!doctype html><style>body{margin:0}::-webkit-scrollbar{width:10px;height:10px}"
    "::-webkit-scrollbar-thumb{background:#f00}"
    "#box{position:absolute;left:0;top:0;width:100px;height:100px;overflow:auto}.tall{height:400px}"
    "#pop{position:fixed;left:70px;top:0;width:60px;height:40px}</style>"
    "<div id=box><div class=tall></div></div><div id=pop></div>";

typedef struct evalres {
    bool ok;
    char text[128];
} evalres;

static void eval_cb(void *user, bool ok, const char *text, size_t len)
{
    evalres *r = user;
    r->ok = ok;
    size_t n = len < sizeof r->text - 1 ? len : sizeof r->text - 1;
    SDL_memcpy(r->text, text, n);
    r->text[n] = '\0';
}

static const char *eval(embi_doc *d, const char *js)
{
    static evalres r;
    SDL_memset(&r, 0, sizeof r);
    (void)embi_script_eval(embi_doc_script(d), js, strlen(js), eval_cb, &r);
    return r.text;
}

static bool fetch_none(void *user, embi_str url, uint8_t **data, size_t *len)
{
    (void)user;
    (void)url;
    (void)data;
    (void)len;
    return false;
}

static const embi_element *hit_at(embi_doc *d, float x, float y)
{
    return embi_layout_hit_test(embi_doc_layout_engine(d), embi_doc_dom(d), x, y);
}

static bool bar_at(embi_doc *d, float x, float y, embi_doc_sbar *sb)
{
    return embi_doc_sbar_at(d, x, y, hit_at(d, x, y), sb);
}

static embi_doc *make(emb_runtime *rt)
{
    embi_doc_cb cb = {rt, fetch_none, nullptr, nullptr};
    embi_doc *d = embi_doc_create(rt->text, rt->fonts, &cb);
    EMB_CHECK(d != nullptr);
    EMB_CHECK(embi_doc_load(d, (embi_str){k_page, (uint32_t)sizeof k_page - 1}, EMBI_STR("app://local/index.html"), nullptr));
    embi_doc_set_viewport(d, 300.0f, 200.0f, 1.0f, false);
    (void)embi_doc_update(d, nullptr);
    return d;
}

/* The gutter is inside the client box and outside the scrollable overflow: no horizontal scrolling for a vertical bar. */
EMB_TEST(gutter_is_in_client_size_and_not_in_scroll_size)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_doc *d = make(rt);
    EMB_REQUIRE(d != nullptr);
    EMB_CHECK_EQ_STR(eval(d, "document.getElementById('box').clientWidth"), "90");
    EMB_CHECK_EQ_STR(eval(d, "document.getElementById('box').clientHeight"), "100");
    EMB_CHECK_EQ_STR(eval(d, "document.getElementById('box').scrollWidth"), "90");
    EMB_CHECK_EQ_STR(eval(d, "document.getElementById('box').scrollHeight"), "400");
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

/* The bar is found on the thumb and the track, not on the content, and not under a box on top of it. */
EMB_TEST(bar_is_found_only_where_the_scroller_is_the_hit)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_doc *d = make(rt);
    EMB_REQUIRE(d != nullptr);
    embi_doc_sbar sb;
    EMB_CHECK(bar_at(d, 95.0f, 60.0f, &sb)); /* below the fixed box (0..40) */
    EMB_CHECK(sb.vertical);
    EMB_CHECK_EQ_INT(sb.part, 3); /* the track after the 25px thumb */
    EMB_CHECK(sb.thumb_start == 0.0f && sb.thumb_len == 25.0f && sb.track_len == 100.0f);
    EMB_CHECK(!bar_at(d, 40.0f, 60.0f, &sb)); /* the content */
    EMB_CHECK(!bar_at(d, 95.0f, 10.0f, &sb)); /* the fixed box is on top: it takes the point */
    EMB_CHECK(!embi_doc_sbar_at(d, 95.0f, 60.0f, nullptr, &sb)); /* no hit element: no bar */
    embi_element *pop = embi_get_element_by_id(embi_doc_dom(d), EMBI_STR("pop"));
    EMB_REQUIRE(pop != nullptr);
    EMB_CHECK(!embi_doc_sbar_at(d, 95.0f, 60.0f, pop, &sb)); /* the hit is a box outside the scroller */
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

/* The scrollers the API reads are cached per layout: a scroll moves the thumb found, and a new layout is seen. */
EMB_TEST(scroller_cache_follows_scrolls_and_layouts)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_doc *d = make(rt);
    EMB_REQUIRE(d != nullptr);
    embi_doc_sbar sb;
    EMB_REQUIRE(bar_at(d, 95.0f, 60.0f, &sb));
    EMB_CHECK(embi_doc_sbar_page(d, sb.key, true, 1, 300.0f, 200.0f));
    EMB_REQUIRE(bar_at(d, 95.0f, 90.0f, &sb));
    EMB_CHECK(sb.thumb_start > 20.0f && sb.thumb_start < 25.0f); /* 87.5 of 300 scrolled: 87.5 / 300 * 75 = 21.9 */
    EMB_CHECK(embi_doc_sbar_drag(d, sb.key, true, 75.0f, 300.0f, 200.0f)); /* to the end of the track */
    EMB_CHECK_EQ_STR(eval(d, "document.getElementById('box').scrollTop"), "300");
    /* a new layout: the box moves 50px down and its bar goes with it */
    (void)eval(d, "document.getElementById('box').style.top='50px'");
    (void)embi_doc_update(d, nullptr);
    EMB_CHECK(!bar_at(d, 95.0f, 20.0f, &sb)); /* the old place is empty now (the box starts at y 50) */
    EMB_REQUIRE(bar_at(d, 95.0f, 60.0f, &sb));
    EMB_CHECK(sb.track_start == 50.0f);
    embi_doc_destroy(d);
    emb_runtime_destroy(rt);
}

EMB_TEST_MAIN()
