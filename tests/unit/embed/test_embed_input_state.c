/*
 * The selector state hook and what it paints (W5d2): :hover, :active, :focus, :focus-visible
 * (the user agent's outline, painted through the W4 outline path), :focus-within, :checked and :disabled are checked in
 * rendered pixels of an offscreen view (the frame read back from the GPU). The CSS cursor of the element under a point,
 * and Tab skipping visibility: hidden elements, are checked through their queries. Test-only; skipped without a device.
 */
#include "input_rig.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>

#define SETUP(html)                                                                                            emb_runtime *rt = emb_runtime_create(nullptr);                                                             EMB_REQUIRE(rt != nullptr);                                                                                embi_gpu *g = nullptr;                                                                                     emb_view *v = nullptr;                                                                                     OPEN_HTML_OR_SKIP(v, rt, g, html)

#define FRAME_W 300
#define FRAME_H 200

/* A frame of the view read back from the GPU (RGBA8, row-major, FRAME_W x FRAME_H), or nullptr. */
static uint8_t *frame(emb_view *v, embi_gpu *g)
{
    SDL_GPUDevice *dev = embi_gpu_device(g);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return nullptr;
    }
    /* The rig's first update already took the damage, so the frame is repainted whole (the picture is not drawn otherwise). */
    v->paint_dirty = true;
    v->force_full = true;
    (void)emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    size_t bytes = (size_t)FRAME_W * FRAME_H * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    if (tb == nullptr) {
        return nullptr;
    }
    SDL_GPUCommandBuffer *rc = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(rc);
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = emb_view_texture(v);
    region.w = FRAME_W;
    region.h = FRAME_H;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    SDL_zero(info);
    info.transfer_buffer = tb;
    info.pixels_per_row = FRAME_W;
    info.rows_per_layer = FRAME_H;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *rf = SDL_SubmitGPUCommandBufferAndAcquireFence(rc);
    uint8_t *out = nullptr;
    if (rf != nullptr) {
        SDL_WaitForGPUFences(dev, true, &rf, 1);
        SDL_ReleaseGPUFence(dev, rf);
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

typedef struct rgb {
    int r, g, b;
} rgb;

static rgb px(const uint8_t *p, int x, int y)
{
    const uint8_t *q = p + ((size_t)y * FRAME_W + (size_t)x) * 4u;
    rgb c = {q[0], q[1], q[2]};
    return c;
}

static bool is_rgb(rgb c, int r, int g, int b)
{
    return c.r >= r - 6 && c.r <= r + 6 && c.g >= g - 6 && c.g <= g + 6 && c.b >= b - 6 && c.b <= b + 6;
}

static void click_at(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

EMB_TEST(hover_rule_paints_the_hovered_box)
{
    SETUP("<!doctype html><style>#h{width:100px;height:100px;background:rgb(0,0,255)}"
          "#h:hover{background:rgb(255,0,0)}</style><body style=\"margin:0\"><div id=h></div>");
    uint8_t *before = frame(v, g);
    EMB_REQUIRE(before != nullptr);
    EMB_CHECK(is_rgb(px(before, 50, 50), 0, 0, 255));
    SDL_free(before);
    emb_view_mouse_move(v, 50.0f, 50.0f, 0);
    uint8_t *on = frame(v, g);
    EMB_REQUIRE(on != nullptr);
    EMB_CHECK(is_rgb(px(on, 50, 50), 255, 0, 0));
    SDL_free(on);
    emb_view_mouse_move(v, 250.0f, 150.0f, 0);
    uint8_t *off = frame(v, g);
    EMB_REQUIRE(off != nullptr);
    EMB_CHECK(is_rgb(px(off, 50, 50), 0, 0, 255));
    SDL_free(off);
    rig_close(v, g, rt);
}

EMB_TEST(active_rule_paints_while_the_button_is_down)
{
    SETUP("<!doctype html><style>#a{width:100px;height:100px;background:rgb(0,0,255)}"
          "#a:active{background:rgb(0,255,0)}</style><body style=\"margin:0\"><div id=a></div>");
    emb_view_mouse_move(v, 50.0f, 50.0f, 0);
    emb_view_mouse_button(v, 50.0f, 50.0f, EMB_BUTTON_PRIMARY, true, 0);
    uint8_t *down = frame(v, g);
    EMB_REQUIRE(down != nullptr);
    EMB_CHECK(is_rgb(px(down, 50, 50), 0, 255, 0));
    SDL_free(down);
    emb_view_mouse_button(v, 50.0f, 50.0f, EMB_BUTTON_PRIMARY, false, 0);
    uint8_t *up = frame(v, g);
    EMB_REQUIRE(up != nullptr);
    EMB_CHECK(is_rgb(px(up, 50, 50), 0, 0, 255));
    SDL_free(up);
    rig_close(v, g, rt);
}

EMB_TEST(focus_rule_paints_the_focused_element_after_a_click)
{
    SETUP("<!doctype html><style>#f{width:100px;height:100px;background:rgb(0,0,255)}"
          "#f:focus{background:rgb(255,255,0)}</style><body style=\"margin:0\"><div id=f tabindex=0></div>");
    click_at(v, 50.0f, 50.0f);
    uint8_t *p = frame(v, g);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(is_rgb(px(p, 50, 50), 255, 255, 0));
    SDL_free(p);
    rig_close(v, g, rt);
}

EMB_TEST(script_focus_before_any_input_applies_the_focus_rule)
{
    /* No pointer or key call has happened: the focus moved by a script must still reach the selector hook before the
     * first frame. */
    SETUP("<!doctype html><style>#f{width:100px;height:100px;background:rgb(0,0,255)}"
          "#f:focus{background:rgb(255,255,0)}</style><body style=\"margin:0\"><div id=f tabindex=0></div>");
    (void)run(v, "document.getElementById('f').focus()");
    uint8_t *p = frame(v, g);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(is_rgb(px(p, 50, 50), 255, 255, 0));
    SDL_free(p);
    rig_close(v, g, rt);
}

EMB_TEST(focus_visible_ring_follows_the_keyboard_and_not_the_mouse)
{
    /* The user agent's :focus-visible outline (auto: a 2 px ring of the focus color) is painted outside the button's box
     * after a Tab, and not after a click (focus-visible stays false for a mouse focus). */
    SETUP("<!doctype html><body style=\"margin:0\"><button id=b style=\"position:absolute;left:40px;top:40px;"
          "width:80px;height:40px;margin:0;padding:0;border:0;background:rgb(255,255,255)\">b</button>");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    uint8_t *k = frame(v, g);
    EMB_REQUIRE(k != nullptr);
    bool ring = is_rgb(px(k, 38, 60), 26, 115, 232) || is_rgb(px(k, 39, 60), 26, 115, 232);
    EMB_CHECK(ring);
    SDL_free(k);
    emb_view_mouse_move(v, 80.0f, 60.0f, 0);
    emb_view_mouse_button(v, 80.0f, 60.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, 80.0f, 60.0f, EMB_BUTTON_PRIMARY, false, 0);
    uint8_t *m = frame(v, g);
    EMB_REQUIRE(m != nullptr);
    EMB_CHECK(!is_rgb(px(m, 38, 60), 26, 115, 232) && !is_rgb(px(m, 39, 60), 26, 115, 232));
    SDL_free(m);
    rig_close(v, g, rt);
}

EMB_TEST(an_author_outline_replaces_the_focus_ring)
{
    /* The page's own outline (here none) wins over the user agent's :focus-visible outline, which sits below author rules. */
    SETUP("<!doctype html><style>#b{outline:none}</style><body style=\"margin:0\">"
          "<button id=b style=\"position:absolute;left:40px;top:40px;width:80px;height:40px;margin:0;padding:0;border:0;"
          "background:rgb(255,255,255)\">b</button>");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    uint8_t *k = frame(v, g);
    EMB_REQUIRE(k != nullptr);
    EMB_CHECK(!is_rgb(px(k, 38, 60), 26, 115, 232) && !is_rgb(px(k, 39, 60), 26, 115, 232));
    SDL_free(k);
    rig_close(v, g, rt);
}

EMB_TEST(focus_within_rule_paints_the_ancestor_of_the_focused_input)
{
    SETUP("<!doctype html><style>#w{width:100px;height:100px;background:rgb(0,0,255)}"
          "#w:focus-within{background:rgb(0,255,0)}</style><body style=\"margin:0\">"
          "<div id=w><input id=i style=\"display:block;margin:0;width:50px;height:20px\"></div>"
          "<div style=\"height:100px\"></div>");
    click_at(v, 25.0f, 10.0f);
    uint8_t *in = frame(v, g);
    EMB_REQUIRE(in != nullptr);
    EMB_CHECK(is_rgb(px(in, 80, 80), 0, 255, 0));
    SDL_free(in);
    click_at(v, 250.0f, 150.0f);
    uint8_t *out = frame(v, g);
    EMB_REQUIRE(out != nullptr);
    EMB_CHECK(is_rgb(px(out, 80, 80), 0, 0, 255));
    SDL_free(out);
    rig_close(v, g, rt);
}

EMB_TEST(checked_and_disabled_rules_paint_their_controls)
{
    SETUP("<!doctype html><style>#c{display:block;margin:0;width:100px;height:100px;background:rgb(255,255,255)}"
          "#c:checked{background:rgb(0,0,255)}"
          "#d{display:block;margin:0;width:100px;height:100px;background:rgb(255,255,255)}"
          "#d:disabled{background:rgb(255,0,255)}</style><body style=\"margin:0\">"
          "<input id=c type=checkbox checked><input id=d disabled></body>");
    uint8_t *p = frame(v, g);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(is_rgb(px(p, 50, 50), 0, 0, 255));
    EMB_CHECK(is_rgb(px(p, 50, 150), 255, 0, 255));
    SDL_free(p);
    rig_close(v, g, rt);
}

/* The state hook is installed on every view; the pseudo-classes it does not own (:defined, :required, :optional, :default,
 * :disabled inside a disabled fieldset ...) must still be decided by the matcher's attribute rules. */
EMB_TEST(attribute_state_classes_still_match_with_the_hook_installed)
{
    SETUP("<!doctype html><style>.b{display:block;margin:0;padding:0;border:0;width:100px;height:20px;"
          "background:rgb(255,255,255)}fieldset{margin:0;padding:0;border:0}"
          "#u:not(:defined){background:rgb(255,0,0)}"
          "#dv:defined{background:rgb(0,255,0)}"
          "#rq:required{background:rgb(0,0,255)}"
          "#op:optional{background:rgb(255,0,255)}"
          "#df:default{background:rgb(0,255,255)}"
          "#fd:disabled{background:rgb(255,255,0)}</style><body style=\"margin:0\">"
          "<x-foo id=u class=b></x-foo>"
          "<div id=dv class=b></div>"
          "<input id=rq class=b required>"
          "<input id=op class=b>"
          "<input id=df class=b type=checkbox checked>"
          "<fieldset disabled><input id=fd class=b></fieldset></body>");
    uint8_t *p = frame(v, g);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(is_rgb(px(p, 50, 10), 255, 0, 0));
    EMB_CHECK(is_rgb(px(p, 50, 30), 0, 255, 0));
    EMB_CHECK(is_rgb(px(p, 50, 50), 0, 0, 255));
    EMB_CHECK(is_rgb(px(p, 50, 70), 255, 0, 255));
    EMB_CHECK(is_rgb(px(p, 50, 90), 0, 255, 255));
    EMB_CHECK(is_rgb(px(p, 50, 110), 255, 255, 0));
    SDL_free(p);
    rig_close(v, g, rt);
}

/* :read-write / :read-only follow the contenteditable and readonly rules (the matcher decides them even with the hook
 * installed), :open and :indeterminate their attributes. */
EMB_TEST(editability_open_and_indeterminate_match_with_the_hook_installed)
{
    SETUP("<!doctype html><body><div id=ce contenteditable></div>"
          "<div id=ce2 contenteditable=true><span id=inh></span></div>"
          "<div id=off contenteditable=false></div>"
          "<p id=p>x</p><textarea id=ro readonly></textarea><textarea id=rw></textarea>"
          "<details id=do open></details><details id=dc></details>"
          "<progress id=pi></progress><progress id=pd value=0.5></progress>");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('ce').matches(':read-write') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('ce').matches(':read-only') ? 'y' : 'n'"), "n");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('inh').matches(':read-write') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('off').matches(':read-only') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('p').matches(':read-only') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('p').matches(':read-write') ? 'y' : 'n'"), "n");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('ro').matches(':read-only') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('rw').matches(':read-write') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('do').matches(':open') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('dc').matches(':open') ? 'y' : 'n'"), "n");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('pi').matches(':indeterminate') ? 'y' : 'n'"), "y");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('pd').matches(':indeterminate') ? 'y' : 'n'"), "n");
    rig_close(v, g, rt);
}

EMB_TEST(cursor_of_the_element_under_the_point_follows_css)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div id=p style=\"width:100px;height:100px;cursor:pointer\"></div>"
          "<div id=t style=\"width:100px;height:100px;cursor:text\"></div>"
          "<div id=q style=\"width:100px;height:100px;cursor:url(a.png) 4 4, crosshair\"></div>"
          "<div id=n style=\"width:100px;height:100px\"></div>"
          "<input id=i style=\"display:block;margin:0;width:100px;height:20px;cursor:auto\">");
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 50.0f) == (int32_t)SDL_SYSTEM_CURSOR_POINTER);
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 150.0f) == (int32_t)SDL_SYSTEM_CURSOR_TEXT);
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 250.0f) == (int32_t)SDL_SYSTEM_CURSOR_CROSSHAIR);
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 350.0f) == -1);
    /* auto over a text control is the text cursor. */
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 410.0f) == (int32_t)SDL_SYSTEM_CURSOR_TEXT);
    rig_close(v, g, rt);
}

EMB_TEST(a_comma_inside_url_does_not_split_the_cursor_list)
{
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<div style=\"width:100px;height:100px;cursor:url(a,b.png) 4 4, move\"></div>");
    EMB_CHECK(embi_input_cursor_at(v, 50.0f, 50.0f) == (int32_t)SDL_SYSTEM_CURSOR_MOVE);
    rig_close(v, g, rt);
}

EMB_TEST(tab_skips_visibility_hidden_elements)
{
    SETUP("<!doctype html><body style=\"margin:0\"><button id=a>a</button>"
          "<button id=b style=\"visibility:hidden\">b</button><button id=c>c</button>");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "c");
    rig_close(v, g, rt);
}

/* Whether the 2 px focus ring of a button whose top edge is at y is painted at its left edge (x 38 or 39). */
static bool ring_at(const uint8_t *p, int y)
{
    return is_rgb(px(p, 38, y), 26, 115, 232) || is_rgb(px(p, 39, y), 26, 115, 232);
}

EMB_TEST(focus_ring_moves_with_the_focus_between_two_buttons)
{
    /* Tab, Tab: the ring leaves the first button and goes to the second (focus-visible stays true, so the rule is
     * restyled on the move). Then a script focus() back to the first moves it again. Button a's centre is y 60, b's y 140. */
    SETUP("<!doctype html><body style=\"margin:0\">"
          "<button id=a style=\"position:absolute;left:40px;top:40px;width:80px;height:40px;margin:0;padding:0;border:0;"
          "background:rgb(255,255,255)\">a</button>"
          "<button id=b style=\"position:absolute;left:40px;top:120px;width:80px;height:40px;margin:0;padding:0;border:0;"
          "background:rgb(255,255,255)\">b</button>");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    uint8_t *k = frame(v, g);
    EMB_REQUIRE(k != nullptr);
    EMB_CHECK(ring_at(k, 140));
    EMB_CHECK(!ring_at(k, 60));
    SDL_free(k);
    (void)run(v, "document.getElementById('a').focus()");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    uint8_t *s = frame(v, g);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(ring_at(s, 60));
    EMB_CHECK(!ring_at(s, 140));
    SDL_free(s);
    rig_close(v, g, rt);
}

EMB_TEST(hover_cursor_rule_is_read_after_the_pointer_move)
{
    /* The cursor is read from the style the hover produced, not the one before the move. The window is not present here,
     * so the tracked kind (embi_input_cursor_kind) stands for what the window would show. */
    SETUP("<!doctype html><style>#l{width:100px;height:100px;cursor:text}#l:hover{cursor:pointer}</style>"
          "<body style=\"margin:0\"><div id=l></div>");
    emb_view_mouse_move(v, 50.0f, 50.0f, 0);
    EMB_CHECK(embi_input_cursor_kind(v) == (int32_t)SDL_SYSTEM_CURSOR_POINTER);
    emb_view_mouse_move(v, 250.0f, 150.0f, 0);
    EMB_CHECK(embi_input_cursor_kind(v) == -1);
    emb_view_mouse_move(v, 50.0f, 50.0f, 0);
    EMB_CHECK(embi_input_cursor_kind(v) == (int32_t)SDL_SYSTEM_CURSOR_POINTER);
    rig_close(v, g, rt);
}

EMB_TEST(cursor_follows_a_style_change_under_a_still_pointer)
{
    /* A script changes the class of the element under the pointer: the frame's restyle moves the cursor with it. */
    SETUP("<!doctype html><style>#l{width:100px;height:100px;cursor:text}#l.p{cursor:pointer}</style>"
          "<body style=\"margin:0\"><div id=l></div>");
    emb_view_mouse_move(v, 50.0f, 50.0f, 0);
    EMB_CHECK(embi_input_cursor_kind(v) == (int32_t)SDL_SYSTEM_CURSOR_TEXT);
    (void)run(v, "document.getElementById('l').className = 'p'");
    uint8_t *p = frame(v, g);
    EMB_REQUIRE(p != nullptr);
    SDL_free(p);
    EMB_CHECK(embi_input_cursor_kind(v) == (int32_t)SDL_SYSTEM_CURSOR_POINTER);
    rig_close(v, g, rt);
}

EMB_TEST(click_after_a_viewport_scroll_reaches_the_element_at_the_scrolled_point)
{
    /* far is at document y 1000..1100. Scrolled to y 900 it is at viewport y 100..200, so a click at viewport (50, 150)
     * reaches it. */
    SETUP("<!doctype html><body style=\"margin:0\"><div style=\"height:1000px\"></div>"
          "<div id=far style=\"width:100px;height:100px;background:rgb(0,0,255)\"></div>");
    (void)run(v, "window.clicked = ''; document.addEventListener('click', e => { window.clicked = e.target.id; })");
    emb_view_scroll_to(v, 0.0f, 900.0f);
    click_at(v, 50.0f, 150.0f);
    EMB_CHECK_EQ_STR(run(v, "window.clicked"), "far");
    rig_close(v, g, rt);
}

RIG_MAIN()
