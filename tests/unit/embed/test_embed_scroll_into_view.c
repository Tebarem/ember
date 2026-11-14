/*
 * Element.scrollIntoView and the viewport's scroll range (src/bindings/impl_scroll.c), and position: fixed boxes in a
 * scrolled page: nearest leaves a visible element alone; start, center, end and nearest scroll the element's overflow
 * container by the right amount and not the viewport; a fixed overflow menu scrolls itself and never the viewport (the
 * case Falcon's editor met: its menu's rectangle jumped to the top while it stayed painted where it was); the viewport
 * scrolls only within its overflow (scrollIntoView, scrollTo, scrollBy, the root's scrollTop); the boolean form; and a
 * fixed box stays where it is painted, reported (getBoundingClientRect) and hit (elementFromPoint, clicks) when the viewport
 * scrolls. The views are 300 x 200 CSS px offscreen views; without a GPU device the tests skip.
 */
#include "input_rig.h"

#include <SDL3/SDL_timer.h>

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

static bool render_now(emb_view *v)
{
    SDL_GPUDevice *dev = v->dev;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    return changed;
}

/* The view's picture (300 x 200) read back as RGBA8; nullptr on failure. */
static uint8_t *picture(emb_view *v)
{
    SDL_GPUDevice *dev = v->dev;
    const uint32_t w = 300, h = 200;
    size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (tb == nullptr || cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = emb_view_texture(v);
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = w;
    info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    uint8_t *out = nullptr;
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            SDL_memcpy(out, mapped, bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

/* The RGB of pixel (x, y) of a picture, as 0xRRGGBB. */
static uint32_t pixel(const uint8_t *px, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * 300u + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* "scrollTop of #box, window.scrollY" after running js (which may be empty). */
static const char *offsets_after(emb_view *v, const char *js)
{
    char src[1024];
    SDL_snprintf(src, sizeof src, "%s; document.getElementById('box').scrollTop + ',' + window.scrollY", js);
    return run(v, src);
}

/* A tall page (the viewport can scroll 800 px) with a 100 x 100 scroll container at the top left whose content is 620 px
 * tall: #t (20 px) at 300 px into it. */
static const char k_box_page[] =
    "<!doctype html><body style='margin:0;height:1000px'>"
    "<div id=box style='position:absolute;left:0;top:0;width:100px;height:100px;overflow:auto;scrollbar-width:none'>"
    "<div style='height:300px'></div><div id=t style='height:20px'></div><div style='height:300px'></div></div>"
    "<div id=vis style='position:absolute;left:150px;top:20px;width:50px;height:24px'></div>";

EMB_TEST(scroll_into_view_nearest_leaves_a_visible_element_alone)
{
    SETUP(k_box_page);
    (void)run(v, "var box = document.getElementById('box'), t = document.getElementById('t'); true");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('vis').scrollIntoView({block: 'nearest'}); String(window.scrollY)"),
                     "0");
    EMB_CHECK_EQ_STR(run(v, "String(document.getElementById('vis').getBoundingClientRect().top)"), "20");
    /* the same inside the container: #t shown, then asked for again */
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 290; t.scrollIntoView({block: 'nearest', inline: 'nearest'})"),
                     "290,0");
    EMB_CHECK_EQ_STR(run(v, "String(t.getBoundingClientRect().top)"), "10");
    rig_close(v, g, rt);
}

EMB_TEST(scroll_into_view_aligns_in_the_overflow_container_only)
{
    SETUP(k_box_page);
    (void)run(v, "var box = document.getElementById('box'), t = document.getElementById('t'); true");
    /* start: #t's top at the scrollport's top; the container is fully in the viewport, so the viewport stays */
    EMB_CHECK_EQ_STR(offsets_after(v, "t.scrollIntoView({block: 'start'})"), "300,0");
    EMB_CHECK_EQ_STR(run(v, "String(t.getBoundingClientRect().top)"), "0");
    /* end: its bottom (320) at the scrollport's bottom (100) */
    EMB_CHECK_EQ_STR(offsets_after(v, "t.scrollIntoView({block: 'end'})"), "220,0");
    EMB_CHECK_EQ_STR(run(v, "String(t.getBoundingClientRect().bottom)"), "100");
    /* center: its middle (310) at the scrollport's middle (50) */
    EMB_CHECK_EQ_STR(offsets_after(v, "t.scrollIntoView({block: 'center'})"), "260,0");
    EMB_CHECK_EQ_STR(run(v, "String(t.getBoundingClientRect().top)"), "40");
    /* nearest from above: the end edge comes in; from below: the start edge */
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 0; t.scrollIntoView({block: 'nearest'})"), "220,0");
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 1000; t.scrollIntoView({block: 'nearest'})"), "300,0");
    /* the container's own range bounds it: start of the last row stops at 520 (620 - 100) */
    EMB_CHECK_EQ_STR(offsets_after(v, "box.lastElementChild.scrollIntoView()"), "320,0");
    EMB_CHECK_EQ_STR(offsets_after(v, "var z = document.createElement('div'); z.style.height = '10px'; box.appendChild(z);"
                                      " z.scrollIntoView()"),
                     "530,90"); /* 620 is past the end of the range (630 - 100); the viewport then brings z (at 90)
                                  * to its top, as the page can scroll */
    EMB_CHECK_EQ_STR(run(v, "String(z.getBoundingClientRect().top)"), "0");
    /* an element hit at its new place */
    EMB_CHECK_EQ_STR(run(v, "t.scrollIntoView(); String(document.elementFromPoint(50, 10) === t)"), "true");
    rig_close(v, g, rt);
}

EMB_TEST(scroll_into_view_boolean_and_default_arguments)
{
    SETUP(k_box_page);
    (void)run(v, "var box = document.getElementById('box'), t = document.getElementById('t'); true");
    EMB_CHECK_EQ_STR(offsets_after(v, "t.scrollIntoView(false)"), "220,0"); /* block end */
    EMB_CHECK_EQ_STR(offsets_after(v, "t.scrollIntoView(true)"), "300,0");  /* block start */
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 0; t.scrollIntoView()"), "300,0");
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 0; t.scrollIntoView(undefined)"), "300,0");
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 0; t.scrollIntoView({})"), "300,0");
    EMB_CHECK_EQ_STR(offsets_after(v, "box.scrollTop = 0; t.scrollIntoView({behavior: 'smooth', block: 'end'})"), "220,0");
    EMB_CHECK_EQ_STR(run(v, "try { t.scrollIntoView({block: 'top'}); 'no' } catch (e) { e.name }"), "TypeError");
    rig_close(v, g, rt);
}

/* Falcon's menu: a fixed overflow box appended to body, 10 rows of 24 px in at most 100 px, over a page that can scroll. */
static const char k_menu_page[] =
    "<!doctype html><body style='margin:0;height:1000px'>"
    "<div id=menu style='position:fixed;left:10px;top:50px;width:100px;max-height:100px;overflow-y:auto;z-index:100;"
    "scrollbar-width:none'></div>"
    "<script>for (var i = 0; i < 10; i++) { var r = document.createElement('div'); r.id = 'r' + i;"
    " r.style.height = '24px'; document.getElementById('menu').appendChild(r); }</script>";

EMB_TEST(scroll_into_view_in_a_fixed_menu_scrolls_the_menu_not_the_viewport)
{
    SETUP(k_menu_page);
    (void)run(v, "var menu = document.getElementById('menu'), r1 = document.getElementById('r1'),"
                 " r9 = document.getElementById('r9'); true");
    /* a visible row: nothing moves, and its rectangle stays where it is painted */
    EMB_CHECK_EQ_STR(run(v, "r1.scrollIntoView({block: 'nearest'}); menu.scrollTop + ',' + window.scrollY + ',' +"
                            " document.scrollingElement.scrollTop + ',' + r1.getBoundingClientRect().top + ',' +"
                            " menu.getBoundingClientRect().top"),
                     "0,0,0,74,50");
    /* the last row (216 to 240): the menu scrolls by 140 to show it at its bottom; the viewport does not move */
    EMB_CHECK_EQ_STR(run(v, "r9.scrollIntoView({block: 'nearest'}); menu.scrollTop + ',' + window.scrollY + ',' +"
                            " r9.getBoundingClientRect().bottom + ',' + menu.getBoundingClientRect().top"),
                     "140,0,150,50");
    /* block start is bounded by the menu's range; the viewport still does not move for a fixed box */
    EMB_CHECK_EQ_STR(run(v, "menu.scrollTop = 0; r9.scrollIntoView(); menu.scrollTop + ',' + window.scrollY"), "140,0");
    /* with the page scrolled, the menu and its rows keep their place */
    EMB_CHECK_EQ_STR(run(v, "window.scrollTo(0, 300); menu.scrollTop = 0; r9.scrollIntoView({block: 'nearest'});"
                            " menu.scrollTop + ',' + window.scrollY + ',' + menu.getBoundingClientRect().top + ',' +"
                            " r9.getBoundingClientRect().bottom"),
                     "140,300,50,150");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(50, 140) === r9)"), "true");
    /* the wheel over the menu in the scrolled page scrolls the menu (its scroller is found where it is painted) */
    EMB_CHECK_EQ_STR(run(v, "menu.scrollTop = 0; String(menu.scrollTop)"), "0");
    emb_view_wheel(v, 50, 100, 0, 40);
    EMB_CHECK_EQ_STR(run(v, "(menu.scrollTop > 0) + ',' + window.scrollY"), "true,300");
    rig_close(v, g, rt);
}

EMB_TEST(viewport_scrolls_stay_within_its_overflow)
{
    {
        /* a page that does not overflow the viewport (html and body fill it, overflow hidden): nothing scrolls it */
        SETUP("<!doctype html><html style='height:100%;overflow:hidden'><body style='margin:0;height:100%;overflow:hidden'>"
              "<div id=t style='position:absolute;top:150px;height:24px;width:50px'></div>");
        (void)run(v, "var t = document.getElementById('t'); true");
        EMB_CHECK_EQ_STR(run(v, "t.scrollIntoView(); String(window.scrollY)"), "0");
        EMB_CHECK_EQ_STR(run(v, "String(t.getBoundingClientRect().top)"), "150");
        EMB_CHECK_EQ_STR(run(v, "window.scrollTo(0, 500); String(window.scrollY)"), "0");
        EMB_CHECK_EQ_STR(run(v, "document.documentElement.scrollTop = 500; String(document.documentElement.scrollTop)"),
                         "0");
        EMB_CHECK(root_y(v) == 0.0f);
        rig_close(v, g, rt);
    }
    {
        /* a 1000 px page in a 200 px viewport scrolls 0 to 800 */
        SETUP("<!doctype html><body style='margin:0;height:1000px'>"
              "<div id=t style='position:absolute;top:950px;height:20px;width:50px'></div>");
        (void)run(v, "var t = document.getElementById('t'); true");
        EMB_CHECK_EQ_STR(run(v, "window.scrollTo(0, 5000); String(window.scrollY)"), "800");
        EMB_CHECK_EQ_STR(run(v, "window.scrollBy(0, -100); String(window.scrollY)"), "700");
        EMB_CHECK_EQ_STR(run(v, "document.documentElement.scrollTop = -5; String(window.scrollY)"), "0");
        EMB_CHECK_EQ_STR(run(v, "document.documentElement.scrollTop = 900; String(document.documentElement.scrollTop)"),
                         "800");
        EMB_CHECK_EQ_STR(run(v, "window.scrollTo(0, 0); t.scrollIntoView(); window.scrollY + ',' +"
                                " t.getBoundingClientRect().top"),
                         "800,150");
        EMB_CHECK(root_y(v) == 800.0f);
        /* a wheel scroll is what scripts read next */
        emb_view_wheel(v, 10, 10, 0, -300);
        EMB_CHECK_EQ_STR(run(v, "String(window.scrollY === document.documentElement.scrollTop)"), "true");
        EMB_CHECK(root_y(v) < 800.0f);
        char expect[32];
        SDL_snprintf(expect, sizeof expect, "%g", (double)root_y(v));
        EMB_CHECK_EQ_STR(run(v, "String(window.scrollY)"), expect);
        rig_close(v, g, rt);
    }
}

EMB_TEST(fixed_box_keeps_its_place_when_the_viewport_scrolls)
{
    SETUP("<!doctype html><body style='margin:0;height:1000px;background:#fff'>"
          "<div id=hdr style='position:fixed;left:0;top:20px;width:300px;height:30px;background:#0f0'>"
          "<span id=lbl style='display:block;width:20px;height:10px'></span></div>"
          "<div id=flow style='position:absolute;left:0;top:200px;width:300px;height:30px;background:#00f'></div>");
    (void)run(v, "var hdr = document.getElementById('hdr'), lbl = document.getElementById('lbl'),"
                 " flow = document.getElementById('flow'); true");
    (void)render_now(v);
    (void)run(v, "window.hits = []; document.addEventListener('mousedown', function (e) { hits.push(e.target.id); }); true");
    (void)run(v, "window.scrollTo(0, 100); true");
    (void)render_now(v); /* a scroll only: the list on screen is patched, not built again */
    EMB_CHECK_EQ_STR(run(v, "String(window.scrollY)"), "100");
    EMB_CHECK_EQ_STR(run(v, "var r = hdr.getBoundingClientRect(); r.top + ',' + r.bottom"), "20,50");
    EMB_CHECK_EQ_STR(run(v, "String(hdr.getClientRects()[0].top)"), "20");
    EMB_CHECK_EQ_STR(run(v, "String(lbl.getBoundingClientRect().top)"), "20"); /* the content of the fixed box too */
    EMB_CHECK_EQ_STR(run(v, "String(hdr.offsetTop)"), "20");
    EMB_CHECK_EQ_STR(run(v, "String(flow.getBoundingClientRect().top)"), "100"); /* an absolute box moves */
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(150, 35) === hdr)"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(150, 110) === flow)"), "true");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((long long)pixel(px, 150, 35), 0x00FF00);
    EMB_CHECK_EQ_INT((long long)pixel(px, 150, 110), 0x0000FF);
    EMB_CHECK_EQ_INT((long long)pixel(px, 150, 140), 0xFFFFFF);
    SDL_free(px);
    emb_view_mouse_button(v, 150, 35, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, 150, 35, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "hits.join(',')"), "hdr");
    /* nearest on the fixed header in a scrolled page moves nothing */
    EMB_CHECK_EQ_STR(run(v, "hdr.scrollIntoView({block: 'nearest'}); String(window.scrollY)"), "100");
    EMB_CHECK_EQ_STR(run(v, "hdr.scrollIntoView(); String(window.scrollY)"), "100");
    rig_close(v, g, rt);
}

/* A fixed box is viewport-fixed only when the viewport is its containing block: inside a transformed or contain:paint box it
 * scrolls with the page; with a z-index or a transform of its own, or inside another fixed box, it still stays. */
EMB_TEST(fixed_box_detection_follows_the_containing_block)
{
    SETUP("<!doctype html><body style='margin:0;height:1000px;background:#fff'>"
          "<div id=tf style='position:absolute;left:0;top:200px;width:100px;height:100px;transform:translateX(0)'>"
          "<div id=ft style='position:fixed;left:0;top:0;width:100px;height:20px;background:#00f'></div></div>"
          "<div id=cp style='position:absolute;left:100px;top:200px;width:100px;height:100px;contain:paint'>"
          "<div id=fc style='position:fixed;left:0;top:0;width:100px;height:20px;background:#00f'></div></div>"
          "<div id=fz style='position:fixed;left:200px;top:0;width:50px;height:20px;z-index:5;background:#0f0'></div>"
          "<div id=fx style='position:fixed;left:250px;top:0;width:50px;height:20px;transform:translateY(10px);"
          "background:#0f0'></div>"
          "<div id=fo style='position:fixed;left:0;top:160px;width:50px;height:30px;background:#0f0'>"
          "<div id=fi style='position:fixed;left:50px;top:160px;width:50px;height:50px;background:#0f0'></div></div>");
    (void)run(v, "var $ = function (i) { return document.getElementById(i); }; true");
    (void)render_now(v);
    (void)run(v, "window.scrollTo(0, 100); true");
    (void)render_now(v);
    EMB_CHECK_EQ_STR(run(v, "String(window.scrollY)"), "100");
    EMB_CHECK_EQ_STR(run(v, "String($('ft').getBoundingClientRect().top)"), "100");
    EMB_CHECK_EQ_STR(run(v, "String($('fc').getBoundingClientRect().top)"), "100");
    EMB_CHECK_EQ_STR(run(v, "String($('fz').getBoundingClientRect().top)"), "0");
    EMB_CHECK_EQ_STR(run(v, "String($('fx').getBoundingClientRect().top)"), "10");
    EMB_CHECK_EQ_STR(run(v, "String($('fo').getBoundingClientRect().top)"), "160");
    EMB_CHECK_EQ_STR(run(v, "String($('fi').getBoundingClientRect().top)"), "160");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(10, 110) === $('ft'))"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(110, 110) === $('fc'))"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(210, 5) === $('fz'))"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(260, 15) === $('fx'))"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(25, 175) === $('fo'))"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(document.elementFromPoint(75, 175) === $('fi'))"), "true");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((long long)pixel(px, 10, 110), 0x0000FF);  /* scrolled with the page */
    EMB_CHECK_EQ_INT((long long)pixel(px, 110, 110), 0x0000FF);
    EMB_CHECK_EQ_INT((long long)pixel(px, 10, 5), 0xFFFFFF);  /* not kept at the viewport top */
    EMB_CHECK_EQ_INT((long long)pixel(px, 210, 5), 0x00FF00);   /* z-index: kept still */
    EMB_CHECK_EQ_INT((long long)pixel(px, 260, 15), 0x00FF00);  /* transform: kept still */
    EMB_CHECK_EQ_INT((long long)pixel(px, 25, 175), 0x00FF00);
    EMB_CHECK_EQ_INT((long long)pixel(px, 75, 175), 0x00FF00);
    SDL_free(px);
    rig_close(v, g, rt);
}

RIG_MAIN()
