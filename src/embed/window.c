/*
 * The windowed mode of a view (W4e, ember.h "windowed"): the SDL window and Ember's own device, the event loop, and the
 * present of the view's texture into the swapchain. The SDL events are translated into the input API of the view
 * (ember_input.h, src/embed/input.c): the mouse, the wheel, the touch fingers, the keys and the text, the IME composition
 * and the host focus. The scroll keys are the default action of a key that the page did not cancel (input.c).
 *
 * The window cannot be opened on a headless host, so this file is exercised by hand on a desktop; the logic it drives
 * (the view update and the frame) is covered by the offscreen tests.
 */
#include "embed_int.h"
#include "embed/frames.h"
#include "embed/input_int.h"
#include "embed/script_int.h"

#include "base/clock.h"
#include "platform/platform.h"

#include <ember/ember_input.h>

#include <math.h>
#include <string.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>

#define EMBI_WIN_IDLE_MS 100u
#define EMBI_WIN_RETRY_MS 10u    /* the wait after a frame whose command buffer could not be acquired */
#define EMBI_WIN_MAX_FAILS 500u  /* consecutive acquire failures (about 5 s) after which the loop gives up */

static double window_ms(uint64_t ticks)
{
    return (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

bool embi_window_open(emb_view *v, const emb_view_desc *desc)
{
    /* The page sees pointer events only: SDL's touch-to-mouse and mouse-to-touch synthesis would send each tap twice. */
    (void)SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    (void)SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#ifdef SDL_PLATFORM_WINDOWS
    /* Most of the cost of a Direct3D 12 device is the driver starting up (300 ms and more). Our SDL (the Tebarem fork)
     * can start it on a background thread when video starts, so it overlaps the window creation; SDL_CreateGPUDevice then
     * gets the device that is already there (one per adapter). Only when the device will be Direct3D 12: a forced other
     * driver would keep an unused device. Set at normal priority, so the environment variable of the same name wins. The
     * hint is the fork's prototype and is not in SDL_hints.h yet; other SDL builds ignore it. */
    const char *drv = SDL_GetHint(SDL_HINT_GPU_DRIVER);
    if (drv == nullptr || *drv == '\0' || SDL_strcasecmp(drv, "direct3d12") == 0) {
        (void)SDL_SetHint("SDL_WINDOWS_GPU_WARMUP", "direct3d12");
    }
#endif
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        embi_rt_log(v->rt, EMB_LOG_ERROR, "SDL video: %s", SDL_GetError());
        return false;
    }
    v->window_ref = true;
    v->fallback_title = SDL_strdup(desc->title != nullptr ? desc->title : "Ember");
    v->window = SDL_CreateWindow(v->fallback_title, (int)v->css_w, (int)v->css_h,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (v->window == nullptr) {
        embi_rt_log(v->rt, EMB_LOG_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    /* The window stays hidden until the first frame is presented (emb_view_pump), so it appears with the page in it. The
     * GPU device comes later too (embi_window_gpu_open), at the first frame or the first image or canvas: the page loads
     * meanwhile, while the driver starts on SDL's warm-up thread. Text input (and the IME) is off until a text control
     * takes the focus (input.c, embi_window_text_input). */
    return true;
}

bool embi_window_gpu_open(emb_view *v)
{
    v->gpu = embi_gpu_create(nullptr, false);
    v->gpu_owned = true;
    if (v->gpu == nullptr) {
        embi_rt_log(v->rt, EMB_LOG_ERROR, "the GPU device could not be created");
        return false;
    }
    v->dev = embi_gpu_device(v->gpu);
    if (!SDL_ClaimWindowForGPUDevice(v->dev, v->window)) {
        embi_rt_log(v->rt, EMB_LOG_ERROR, "SDL_ClaimWindowForGPUDevice: %s", SDL_GetError());
        return false;
    }
    v->window_claimed = true;
    return true;
}

void embi_window_text_input(emb_view *v, bool on, float x, float y, float w, float h)
{
    if (v == nullptr || v->window == nullptr) {
        return;
    }
    if (on) {
        SDL_Rect rect = {(int)x, (int)y, (int)w > 0 ? (int)w : 1, (int)h > 0 ? (int)h : 1};
        (void)SDL_SetTextInputArea(v->window, &rect, 0); /* the caret rectangle, for the input method's candidate window */
        (void)SDL_StartTextInput(v->window);
    } else {
        (void)SDL_StopTextInput(v->window);
    }
}

void embi_window_release(emb_view *v)
{
    if (v->window_claimed && v->dev != nullptr && v->window != nullptr) {
        SDL_ReleaseWindowFromGPUDevice(v->dev, v->window);
    }
    v->window_claimed = false;
}

/* The system cursors, created on first use and kept per kind (SDL destroys them with the video subsystem, so they go
 * with the window). g_cursor_kind is the kind on the window: -1 the default arrow, -2 hidden. */
static SDL_Cursor *g_cursors[SDL_SYSTEM_CURSOR_COUNT];
static int32_t g_cursor_kind = -1;

void embi_window_set_cursor(emb_view *v, int32_t kind)
{
    if (v == nullptr || v->window == nullptr) {
        return;
    }
    if (kind == -2) {
        if (g_cursor_kind != -2) {
            (void)SDL_HideCursor();
            g_cursor_kind = -2;
        }
        return;
    }
    if (kind < 0 || kind >= (int32_t)SDL_SYSTEM_CURSOR_COUNT) {
        kind = -1;
    }
    if (kind == g_cursor_kind) {
        return;
    }
    SDL_Cursor *next = nullptr;
    if (kind >= 0) {
        if (g_cursors[kind] == nullptr) {
            g_cursors[kind] = SDL_CreateSystemCursor((SDL_SystemCursor)kind);
        }
        next = g_cursors[kind];
    } else {
        next = SDL_GetDefaultCursor();
    }
    if (next == nullptr) {
        return;
    }
    (void)SDL_ShowCursor();
    SDL_SetCursor(next);
    g_cursor_kind = kind;
}

/* Drops the cursors made for the window (SDL frees the default one itself). */
static void cursors_release(void)
{
    SDL_SetCursor(SDL_GetDefaultCursor());
    for (uint32_t i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++) {
        if (g_cursors[i] != nullptr) {
            SDL_DestroyCursor(g_cursors[i]);
            g_cursors[i] = nullptr;
        }
    }
    g_cursor_kind = -1;
}

void embi_window_close(emb_view *v)
{
    if (g_cursor_kind != -1) {
        (void)SDL_ShowCursor();
    }
    cursors_release();
    if (v->window != nullptr) {
        SDL_DestroyWindow(v->window);
        v->window = nullptr;
    }
    if (v->window_ref) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        v->window_ref = false;
    }
}

void embi_view_window_reflow(emb_view *v)
{
    if (v->window == nullptr) {
        return;
    }
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(v->window, &lw, &lh);
    SDL_GetWindowSizeInPixels(v->window, &pw, &ph);
    if (lw > 0 && lh > 0 && pw > 0 && ph > 0) {
        embi_view_set_window_size(v, (uint32_t)lw, (uint32_t)lh, (uint32_t)pw, (uint32_t)ph);
    }
    v->need_present = true;
}

/* One notch of a wheel, in CSS px (the same as Chromium's). The deltas are CSS px already; the position maps like a pointer's. */
static void wheel(emb_view *v, const SDL_MouseWheelEvent *w, float ox, float oy, float scale)
{
    if (w->which == SDL_TOUCH_MOUSEID) {
        return; /* a touch scroll is a pointer gesture of its own (not a wheel) */
    }
    /* SDL's x and y already include the user's natural-scrolling setting (direction only reports it), so it is not
     * undone here. Positive x is to the right: scrollLeft increases. Positive y is away from the user: the content
     * moves down, so the offset decreases. */
    float dx = w->x * EMBI_VIEW_WHEEL_PX;
    float dy = -w->y * EMBI_VIEW_WHEEL_PX;
    emb_view_wheel(v, (w->mouse_x - ox) * scale, (w->mouse_y - oy) * scale, dx, dy); /* a cancelable wheel event first (input.c) */
}

/* The EMB_MOD_* bits of the SDL modifier state. */
static uint32_t mods_of(SDL_Keymod m)
{
    uint32_t r = 0;
    if ((m & SDL_KMOD_SHIFT) != 0) {
        r |= EMB_MOD_SHIFT;
    }
    if ((m & SDL_KMOD_CTRL) != 0) {
        r |= EMB_MOD_CTRL;
    }
    if ((m & SDL_KMOD_ALT) != 0) {
        r |= EMB_MOD_ALT;
    }
    if ((m & SDL_KMOD_GUI) != 0) {
        r |= EMB_MOD_META;
    }
    return r;
}

/* An SDL mouse button as the DOM numbers it (the wheel and the touch buttons do not come here). */
static emb_button button_of(uint8_t b)
{
    switch (b) {
    case SDL_BUTTON_MIDDLE:
        return EMB_BUTTON_AUXILIARY;
    case SDL_BUTTON_RIGHT:
        return EMB_BUTTON_SECONDARY;
    case SDL_BUTTON_X1:
        return EMB_BUTTON_BACK;
    case SDL_BUTTON_X2:
        return EMB_BUTTON_FORWARD;
    default:
        return EMB_BUTTON_PRIMARY;
    }
}

static void key(emb_view *v, const SDL_KeyboardEvent *k)
{
    char key_buf[32];
    char code_buf[32];
    /* The key value on the user's layout: Shift, AltGr and CapsLock applied to the character (SDL's keymap), not the US
     * table. Keypad digits follow NumLock through k->mod. */
    embi_pf_key_name_layout(k->key, SDL_GetKeyFromScancode(k->scancode, k->mod, false), k->mod, key_buf, sizeof key_buf);
    embi_pf_code_name(k->scancode, code_buf, sizeof code_buf);
    emb_key_event ev = {
        .size = sizeof(emb_key_event),
        .down = k->down,
        .repeat = k->repeat,
        .mods = mods_of(k->mod),
        .location = (emb_key_location)embi_pf_key_location(k->scancode),
        .key = key_buf,
        .code = code_buf,
    };
    emb_view_key(v, &ev);
}

/* Translates an SDL event of the kinds the view takes (keys, text and IME, mouse motion, buttons and wheel, and the window's
 * mouse leave and focus) into the input API. win is the window whose events the view takes; 0 takes the events of any
 * window (an offscreen host passes the events it wants). Pointer positions map from event units to CSS px as
 * ((x - ox) * scale, (y - oy) * scale); a windowed view passes 0, 0 and 1. Returns true when ev is one of these kinds. */
static bool input_event(emb_view *v, const SDL_Event *ev, SDL_WindowID win, float ox, float oy, float scale)
{
    switch (ev->type) {
    case SDL_EVENT_MOUSE_WHEEL:
        /* Events of the host's other windows do not scroll this view. */
        if (win == 0 || ev->wheel.windowID == win) {
            wheel(v, &ev->wheel, ox, oy, scale);
        }
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (win == 0 || ev->key.windowID == win) {
            key(v, &ev->key);
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        /* Mouse events synthesized from touches are dropped: the touch is a pointer event of its own (below). */
        if ((win == 0 || ev->motion.windowID == win) && ev->motion.which != SDL_TOUCH_MOUSEID) {
            emb_view_mouse_move(v, (ev->motion.x - ox) * scale, (ev->motion.y - oy) * scale, mods_of(SDL_GetModState()));
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if ((win == 0 || ev->button.windowID == win) && ev->button.which != SDL_TOUCH_MOUSEID) {
            emb_view_mouse_button_clicks(v, (ev->button.x - ox) * scale, (ev->button.y - oy) * scale,
                                         button_of(ev->button.button), ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                         mods_of(SDL_GetModState()), ev->button.clicks);
        }
        return true;
    case SDL_EVENT_TEXT_INPUT:
        if ((win == 0 || ev->text.windowID == win) && ev->text.text != nullptr) {
            emb_view_text_input(v, ev->text.text, strlen(ev->text.text));
        }
        return true;
    case SDL_EVENT_TEXT_EDITING:
        if (win == 0 || ev->edit.windowID == win) {
            emb_text_editing te = {
                .size = sizeof(emb_text_editing),
                .text = ev->edit.text != nullptr ? ev->edit.text : "",
                .cursor = ev->edit.start,
                .sel_len = ev->edit.length,
            };
            emb_view_text_editing(v, &te);
        }
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (win == 0 || ev->window.windowID == win) {
            emb_view_mouse_leave(v); /* out/leave events, and :hover must not stay on the last element */
        }
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (win == 0 || ev->window.windowID == win) {
            emb_view_focus(v, true);
        }
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (win == 0 || ev->window.windowID == win) {
            emb_view_focus(v, false);
        }
        return true;
    default:
        return false;
    }
}

bool embi_window_event(emb_view *v, const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        return false;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (ev->window.windowID == SDL_GetWindowID(v->window)) {
            return false;
        }
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        embi_view_window_reflow(v);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESTORED:
        v->need_present = true;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        /* The finger coordinates are normalized to the window: scale them to the view's CSS px. */
        int phase = ev->type == SDL_EVENT_FINGER_DOWN     ? 0
                    : ev->type == SDL_EVENT_FINGER_MOTION ? 1
                    : ev->type == SDL_EVENT_FINGER_UP     ? 2
                                                          : 3;
        emb_view_touch(v, (uint64_t)ev->tfinger.fingerID, ev->tfinger.x * (float)v->css_w,
                       ev->tfinger.y * (float)v->css_h, phase);
        break;
    }
    default:
        (void)input_event(v, ev, SDL_GetWindowID(v->window), 0.0f, 0.0f, 1.0f);
        break;
    }
    return true;
}

bool emb_view_sdl_event(emb_view *view, const SDL_Event *event, float origin_x, float origin_y, float scale)
{
    if (view == nullptr || event == nullptr || !(scale > 0.0f) || !isfinite(scale)) {
        return false;
    }
    return input_event(view, event, 0, origin_x, origin_y, scale);
}

bool emb_view_pump(emb_view *v)
{
    if (v == nullptr || v->mode != EMB_VIEW_WINDOWED || v->closed || v->window == nullptr) {
        return false;
    }
    /* Nothing to draw and nothing animating: wait for an event (bounded, so the loop still polls the clock). */
    embi_script *script = v->doc != nullptr ? embi_doc_script(v->doc) : nullptr;
    if (script != nullptr) {
        embi_script_set_wake(script, true); /* a task posted from another thread wakes the wait below */
    }
    /* The wait ends at the next event, or when the next timer is due (at most EMBI_WIN_IDLE_MS). A queued task or a timer
     * that is due already means there is work: no wait. */
    int32_t wait_ms = (int32_t)EMBI_WIN_IDLE_MS;
    double due = 0;
    double fdue = 0;
    bool has_due = script != nullptr && embi_script_next_due(script, &due);
    if (embi_frames_next_due(v, &fdue) && (!has_due || fdue < due)) {
        due = fdue; /* IF1: the frames' timers */
        has_due = true;
    }
    if (has_due) {
        double left = due - embi_clock_ms();
        wait_ms = left <= 0 ? 0 : (left < (double)EMBI_WIN_IDLE_MS ? (int32_t)left + 1 : (int32_t)EMBI_WIN_IDLE_MS);
    }
    bool idle = !embi_view_animating(v) && !v->paint_dirty && !v->need_present && wait_ms > 0 &&
                (script == nullptr || !embi_script_has_tasks(script)) && !embi_frames_has_tasks(v);
    SDL_Event ev;
    bool keep = true;
    if (idle && SDL_WaitEventTimeout(&ev, wait_ms)) {
        keep = embi_window_event(v, &ev);
    }
    while (keep && SDL_PollEvent(&ev)) {
        keep = embi_window_event(v, &ev);
    }
    if (!keep) {
        v->closed = true;
        return false;
    }

    /* A minimized window has no swapchain: nothing is presented, and the restore or exposed event sets need_present. */
    bool minimized = (SDL_GetWindowFlags(v->window) & SDL_WINDOW_MINIMIZED) != 0;
    if (minimized) {
        v->need_present = false;
    }
    /* The first frame (unless an image or a canvas came first) creates the GPU device: see embi_window_open. */
    if (!embi_view_gfx_ready(v)) {
        v->closed = true;
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (cmd == nullptr) {
        /* The device could not give a command buffer: wait a little instead of spinning, and give up when it persists. */
        if (++v->gpu_fails >= EMBI_WIN_MAX_FAILS) {
            embi_rt_log(v->rt, EMB_LOG_ERROR, "SDL_AcquireGPUCommandBuffer keeps failing: %s", SDL_GetError());
            v->failed = true;
            v->closed = true;
            return false;
        }
        SDL_Delay(EMBI_WIN_RETRY_MS);
        return true;
    }
    v->gpu_fails = 0;
    bool changed = embi_view_frame(v, cmd);
    bool tried_present = changed || v->need_present;
    if (tried_present) {
        SDL_GPUTexture *sw = nullptr;
        uint32_t sw_w = 0, sw_h = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, v->window, &sw, &sw_w, &sw_h) && sw != nullptr) {
            embi_gfx_blit(v->gfx, cmd, v->tex, v->dev_w, v->dev_h, sw, sw_w, sw_h);
            v->need_present = false;
        } else {
            /* No swapchain this time (minimized, or the window is being resized): the picture is shown later, and
             * the loop waits a little instead of spinning. */
            v->need_present = !minimized;
            SDL_Delay(EMBI_WIN_RETRY_MS);
        }
    }
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        embi_rt_log(v->rt, EMB_LOG_WARN, "submit: %s", SDL_GetError());
    }
    v->st.submit_ms = window_ms(SDL_GetPerformanceCounter() - t0);
    if (tried_present && !v->shown) {
        /* The window was created hidden: it appears now, with the first frame in it. When the hidden window gave no
         * swapchain texture, it is shown anyway and the next pump presents. */
        SDL_ShowWindow(v->window);
        v->shown = true;
    }
    return true;
}

bool emb_view_run(emb_view *v)
{
    if (v == nullptr || v->mode != EMB_VIEW_WINDOWED) {
        return false;
    }
    while (emb_view_pump(v)) {
    }
    return !v->failed;
}
