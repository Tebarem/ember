/*
 * Chromium-style sliders and scrollbars in a view (the acceptance CSS of a game editor): a drag on a range whose thumb is
 * 12px puts the thumb's centre under the pointer, with `input` events while it moves, `change` on release and :active while
 * the button is down; changes of --pct, of the value and of the hover repaint the slider; a scroll container with the 10px
 * ::-webkit-scrollbar takes 10px of gutter, shows a bar that a drag on its thumb, a click on its track and the wheel scroll,
 * and display: none or no width removes it. The views are 300 x 200 CSS px offscreen views; without a GPU device the tests skip.
 */
#include "input_rig.h"

#include <math.h>
#include <stdlib.h>

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html);                                                                     \
    (void)run(v, "1");                                                                                     \
    v->force_full = true; /* rig_open updated the view without drawing it: the first draw is a whole one */  \
    v->paint_dirty = true;                                                                                 \
    (void)render_now(v);                                                                                   \
    (void)render_now(v)

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

static uint32_t pixel(const uint8_t *px, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * 300u + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* The picture's pixel (x, y) after a render. */
static uint32_t sample(emb_view *v, int x, int y)
{
    bool ch = render_now(v);
    uint8_t *px = picture(v);
    if (SDL_getenv("RS_DEBUG") != nullptr) {
        SDL_Log("render changed=%d", (int)ch);
    }
    uint32_t c = px != nullptr ? pixel(px, x, y) : 0xDEADBEu;
    if (SDL_getenv("RS_DEBUG") != nullptr) {
        SDL_Log("sample %d,%d = %06x", x, y, c);
    }
    SDL_free(px);
    return c;
}

static bool near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s <= 16; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > tol || d < -tol) {
            return false;
        }
    }
    return true;
}

#define SLIDER_CSS                                                                                                         \
    ":root{--accent:#3a79bb}body{margin:0;background:#202020}"                                                              \
    "input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:18px;margin:0;background:transparent;"    \
    "cursor:pointer;--pct:50%}"                                                                                             \
    "input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;"                                        \
    "background:linear-gradient(to right,var(--accent) 0 var(--pct),#4a4a4a var(--pct) 100%)}"                              \
    "input[type=range]:hover::-webkit-slider-runnable-track{"                                                               \
    "background:linear-gradient(to right,#4b8ad0 0 var(--pct),#565656 var(--pct) 100%)}"                                    \
    "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:12px;height:12px;margin-top:-4px;" \
    "border:0;border-radius:50%;background:#e6e6e6}"                                                                        \
    "input[type=range]:hover::-webkit-slider-thumb{background:#fff}"                                                         \
    "input[type=range]:active::-webkit-slider-thumb{background:#ff0000}"                                                     \
    ".row{position:absolute;left:0;top:0;width:200px;display:flex}"                                                         \
    ".row input[type=range]{flex:1;min-width:0}"

static const char k_slider_page[] =
    "<!doctype html><style>" SLIDER_CSS "</style><div class=row><input id=r type=range min=0 max=100 value=50></div>"
    "<script>var r=document.getElementById('r');window.n={input:0,change:0};"
    "r.addEventListener('input',function(){n.input++;r.style.setProperty('--pct',r.value+'%')});"
    "r.addEventListener('change',function(){n.change++});</script>";

/* The thumb of a 200px slider with a 12px thumb runs over 188px: the pointer's x puts the thumb's centre under it. */
EMB_TEST(range_drag_puts_the_thumb_centre_under_the_pointer)
{
    SETUP(k_slider_page);
    EMB_CHECK_EQ_STR(run(v, "r.value"), "50");
    emb_view_mouse_move(v, 56.0f, 9.0f, 0);
    emb_view_mouse_button(v, 56.0f, 9.0f, EMB_BUTTON_PRIMARY, true, 0);
    /* (56 - 6) / 188 of the range */
    EMB_CHECK(fabs(atof(run(v, "r.value")) - 100.0 * 50.0 / 188.0) <= 1.0);
    EMB_CHECK(near(sample(v, 56, 9), 0xff0000, 3)); /* :active restyles the thumb (under the pointer) while the button is down */
    emb_view_mouse_move(v, 150.0f, 9.0f, 0);
    EMB_CHECK(fabs(atof(run(v, "r.value")) - 100.0 * 144.0 / 188.0) <= 1.0);
    EMB_CHECK_EQ_STR(run(v, "n.change"), "0");
    emb_view_mouse_move(v, 400.0f, 9.0f, 0); /* past the end: the thumb stops at its end */
    EMB_CHECK_EQ_STR(run(v, "r.value"), "100");
    emb_view_mouse_move(v, -50.0f, 9.0f, 0);
    EMB_CHECK_EQ_STR(run(v, "r.value"), "0");
    emb_view_mouse_button(v, -50.0f, 9.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "n.change"), "1");
    EMB_CHECK(atoi(run(v, "n.input")) >= 3);
    rig_close(v, g, rt);
}

/* A press on the track puts the thumb's centre at the press, the rest of the range is the same fraction. */
EMB_TEST(range_click_on_the_track_sets_the_value)
{
    SETUP(k_slider_page);
    emb_view_mouse_button(v, 194.0f, 9.0f, EMB_BUTTON_PRIMARY, true, 0); /* the centre of the thumb at its end: 194 = 200 - 6 */
    EMB_CHECK_EQ_STR(run(v, "r.value"), "100");
    emb_view_mouse_button(v, 194.0f, 9.0f, EMB_BUTTON_PRIMARY, false, 0);
    emb_view_mouse_button(v, 6.0f, 9.0f, EMB_BUTTON_PRIMARY, true, 0);
    EMB_CHECK_EQ_STR(run(v, "r.value"), "0");
    emb_view_mouse_button(v, 6.0f, 9.0f, EMB_BUTTON_PRIMARY, false, 0);
    rig_close(v, g, rt);
}

/* The slider repaints when --pct changes, when its value changes, and with the hover and the press. The track is 4px at
 * y 7..10 of the 18px input; the thumb is a 12px disc at y 3..14. */
EMB_TEST(range_repaints_for_pct_value_hover_and_press)
{
    SETUP(k_slider_page);
    /* the value 50: thumb at 94..106, the accent to its centre, the grey after it */
    EMB_CHECK(near(sample(v, 20, 9), 0x3a79bb, 3));
    EMB_CHECK(near(sample(v, 150, 9), 0x4a4a4a, 3));
    EMB_CHECK(near(sample(v, 100, 9), 0xe6e6e6, 3));
    /* --pct alone: the gradient moves, the thumb stays */
    (void)run(v, "r.style.setProperty('--pct','80%')");
    EMB_CHECK(near(sample(v, 150, 9), 0x3a79bb, 3));
    EMB_CHECK(near(sample(v, 190, 9), 0x4a4a4a, 3));
    EMB_CHECK(near(sample(v, 100, 9), 0xe6e6e6, 3));
    /* the value: the thumb moves (the 12px thumb of 0 sits at 0..12) */
    (void)run(v, "r.value=0");
    EMB_CHECK(near(sample(v, 4, 9), 0xe6e6e6, 3));
    EMB_CHECK(near(sample(v, 100, 9), 0x3a79bb, 3));
    /* the hover: the track takes the hover colors and the thumb is white */
    emb_view_mouse_move(v, 120.0f, 12.0f, 0);
    EMB_CHECK(near(sample(v, 150, 9), 0x4b8ad0, 3));
    EMB_CHECK(near(sample(v, 4, 9), 0xffffff, 3));
    emb_view_mouse_move(v, 250.0f, 100.0f, 0);
    EMB_CHECK(near(sample(v, 150, 9), 0x3a79bb, 3));
    EMB_CHECK(near(sample(v, 4, 9), 0xe6e6e6, 3));
    /* the press: :active restyles the thumb */
    emb_view_mouse_button(v, 4.0f, 9.0f, EMB_BUTTON_PRIMARY, true, 0);
    EMB_CHECK(near(sample(v, 4, 9), 0xff0000, 3));
    emb_view_mouse_button(v, 4.0f, 9.0f, EMB_BUTTON_PRIMARY, false, 0);
    rig_close(v, g, rt);
}

#define SB_CSS                                                                                                             \
    "body{margin:0;background:#202020}::-webkit-scrollbar{width:10px;height:10px}"                                          \
    "::-webkit-scrollbar-thumb{background:#f00;border:2px solid transparent;background-clip:padding-box;border-radius:6px}" \
    "::-webkit-scrollbar-thumb:hover{background-color:#0f0}"                                                                \
    "::-webkit-scrollbar-track,::-webkit-scrollbar-corner{background:transparent}"                                          \
    "#box{position:absolute;left:0;top:0;width:100px;height:100px;overflow:auto;background:#000}"                           \
    ".tall{height:400px}"

static const char k_sb_page[] = "<!doctype html><style>" SB_CSS "</style><div id=box><div class=tall></div></div>";

static double scroll_top(emb_view *v)
{
    return atof(run(v, "document.getElementById('box').scrollTop"));
}

/* The container's gutter is the bar's 10px (the content box is 90px wide), and the page lays the bar out whether or not the
 * pointer ever scrolled it. */
EMB_TEST(styled_scrollbar_takes_its_thickness_from_the_container)
{
    SETUP(k_sb_page);
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('box').clientWidth"), "90");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('box').clientHeight"), "100");
    /* the bar: the thumb is 25px of the 100px track, inset 2px by its border: red at (95, 12), the track clear (black) at (91, 60) */
    EMB_CHECK(near(sample(v, 95, 12), 0xff0000, 6));
    EMB_CHECK(near(sample(v, 95, 60), 0x000000, 2));
    EMB_CHECK(near(sample(v, 90, 12), 0x000000, 2)); /* the thumb's border is transparent: the box's background */
    rig_close(v, g, rt);
}

EMB_TEST(styled_scrollbar_thumb_drag_scrolls)
{
    SETUP(k_sb_page);
    EMB_CHECK(scroll_top(v) == 0.0);
    emb_view_mouse_move(v, 95.0f, 10.0f, 0);
    EMB_CHECK(near(sample(v, 95, 12), 0x00ff00, 6)); /* the pointer on the thumb: the hover color */
    emb_view_mouse_button(v, 95.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_move(v, 95.0f, 60.0f, 0);
    /* the thumb moved 50px of its 75px travel: 2/3 of the 300px range */
    EMB_CHECK(fabs(scroll_top(v) - 200.0) <= 1.5);
    EMB_CHECK(near(sample(v, 95, 62), 0x00ff00, 6)); /* the thumb follows the pointer */
    emb_view_mouse_move(v, 5.0f, 400.0f, 0);          /* far off the bar: the thumb stops at the end */
    EMB_CHECK(fabs(scroll_top(v) - 300.0) <= 1.0);
    emb_view_mouse_button(v, 5.0f, 400.0f, EMB_BUTTON_PRIMARY, false, 0);
    emb_view_mouse_move(v, 5.0f, 150.0f, 0);
    EMB_CHECK(near(sample(v, 95, 90), 0xff0000, 6)); /* off the thumb again: the normal color */
    rig_close(v, g, rt);
}

EMB_TEST(styled_scrollbar_track_click_pages_and_wheel_scrolls)
{
    SETUP(k_sb_page);
    emb_view_mouse_button(v, 95.0f, 90.0f, EMB_BUTTON_PRIMARY, true, 0); /* the track below the thumb */
    emb_view_mouse_button(v, 95.0f, 90.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK(fabs(scroll_top(v) - 87.5) <= 1.0);
    emb_view_mouse_button(v, 95.0f, 3.0f, EMB_BUTTON_PRIMARY, true, 0); /* above the thumb now */
    emb_view_mouse_button(v, 95.0f, 3.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK(fabs(scroll_top(v)) <= 1.0);
    emb_view_wheel(v, 40.0f, 40.0f, 0.0f, 60.0f);
    EMB_CHECK(fabs(scroll_top(v) - 60.0) <= 1.0);
    rig_close(v, g, rt);
}

/* A press on the bar is the bar's: the page sees no mousedown, and a press on the content still does. */
EMB_TEST(styled_scrollbar_press_is_not_a_page_click)
{
    SETUP(k_sb_page);
    (void)run(v, "window.d=0;document.addEventListener('mousedown',function(){d++})");
    emb_view_mouse_button(v, 95.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, 95.0f, 10.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "d"), "0");
    emb_view_mouse_button(v, 40.0f, 40.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, 40.0f, 40.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "d"), "1");
    rig_close(v, g, rt);
}

EMB_TEST(scrollbar_display_none_shows_no_bar)
{
    SETUP("<!doctype html><style>" SB_CSS "::-webkit-scrollbar{display:none}</style><div id=box><div class=tall></div></div>");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('box').clientWidth"), "100");
    EMB_CHECK(near(sample(v, 95, 12), 0x000000, 2)); /* no thumb, no overlay bar after a wheel either */
    emb_view_wheel(v, 40.0f, 40.0f, 0.0f, 60.0f);
    EMB_CHECK(fabs(scroll_top(v) - 60.0) <= 1.0);
    EMB_CHECK(near(sample(v, 95, 40), 0x000000, 2));
    rig_close(v, g, rt);
}

EMB_TEST(scrollbar_with_no_width_shows_no_bar)
{
    SETUP("<!doctype html><style>" SB_CSS "::-webkit-scrollbar{width:0;height:0}</style><div id=box><div class=tall></div></div>");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('box').clientWidth"), "100");
    rig_close(v, g, rt);
}

/* Containers that do not overflow keep their size, and a box with overflow: hidden never gets a bar. */
EMB_TEST(scrollbar_gutter_only_where_the_content_overflows)
{
    SETUP("<!doctype html><style>" SB_CSS "#h{position:absolute;left:150px;top:0;width:100px;height:100px;overflow:hidden}"
          "#s{position:absolute;left:150px;top:110px;width:100px;height:50px;overflow:auto}</style>"
          "<div id=box><div style='height:10px'></div></div><div id=h><div class=tall></div></div>"
          "<div id=s><div style='height:20px'></div></div>");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('box').clientWidth"), "100");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('h').clientWidth"), "100");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('s').clientWidth"), "100");
    rig_close(v, g, rt);
}

/* A box on top of the scroller (a menu, a popover: position fixed or absolute, over the bar) takes the press and the hover from the
 * bar below it: the page's pop-up gets the click, the scroller does not scroll and its thumb stays in the normal color. */
EMB_TEST(box_over_a_styled_bar_takes_the_press_and_the_hover)
{
    SETUP("<!doctype html><style>" SB_CSS "#pop{position:fixed;left:70px;top:0;width:60px;height:60px;background:#00f}"
          "#abs{position:absolute;left:70px;top:70px;width:60px;height:30px;background:#00f}</style>"
          "<div id=box><div class=tall></div></div><div id=pop></div><div id=abs></div>");
    (void)run(v, "window.p=0;window.a=0;document.getElementById('pop').addEventListener('mousedown',function(){p++});document.getElementById('abs').addEventListener('mousedown',function(){a++})");
    emb_view_mouse_move(v, 95.0f, 10.0f, 0);
    emb_view_mouse_button(v, 95.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_move(v, 95.0f, 50.0f, 0); /* no drag of the bar below */
    emb_view_mouse_button(v, 95.0f, 50.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "p"), "1");
    EMB_CHECK(scroll_top(v) == 0.0);
    emb_view_mouse_button(v, 95.0f, 90.0f, EMB_BUTTON_PRIMARY, true, 0); /* the track part under the absolute box */
    emb_view_mouse_button(v, 95.0f, 90.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "a"), "1");
    EMB_CHECK(scroll_top(v) == 0.0);
    /* beside the boxes the bar still works: the thumb (y 0..25) is under the pointer at y 20, but the fixed box covers x 70..130 */
    emb_view_mouse_button(v, 95.0f, 65.0f, EMB_BUTTON_PRIMARY, true, 0); /* between the two boxes: the track */
    emb_view_mouse_button(v, 95.0f, 65.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK(fabs(scroll_top(v) - 87.5) <= 1.0);
    rig_close(v, g, rt);
}

RIG_MAIN()
