/*
 * Wheel and clipboard through the input API of a view (W5d): the cancelable wheel event (a canceled one does not scroll, an
 * uncanceled one does), the copy, cut and paste events with a text/plain clipboardData and their default actions on text
 * controls, Ctrl and Cmd shortcuts, navigator.clipboard.readText and writeText, and a round trip through the system clipboard
 * (SDL's when the video subsystem is up, else the process-local buffer; the test saves and restores the text it found).
 * Views need an offscreen device: the tests skip (77) without one, and without a working clipboard.
 */
#include "input_rig.h"

#include "platform/platform.h"

#include <stdio.h>

static const char k_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<input id=a style=\"display:block;margin:0;height:24px;box-sizing:border-box\">"
    "<div id=pad style=\"height:60px\"></div>"
    "<div style=\"height:3000px\"></div>";

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

/* The clipboard text now. */
static char g_clip[512];

static const char *clip_get(void)
{
    embi_sb sb = {0};
    g_clip[0] = '\0';
    if (embi_pf_clipboard_get(&sb) && sb.len < sizeof g_clip) {
        if (sb.len > 0) {
            memcpy(g_clip, sb.data, sb.len);
        }
        g_clip[sb.len] = '\0';
    }
    embi_sb_free(&sb);
    return g_clip;
}

static bool clip_set(const char *s)
{
    return embi_pf_clipboard_set((embi_str){s, (uint32_t)strlen(s)});
}

/* The text the user had on the clipboard: put back at the end of the test. */
typedef struct saved {
    char text[512];
} saved;

static bool clip_save(saved *s)
{
    snprintf(s->text, sizeof s->text, "%s", clip_get());
    return clip_set("probe");
}

static const char *sel_text = "chosen";

static bool selection(void *user, embi_node *target, embi_sb *out)
{
    (void)user;
    (void)target;
    embi_sb_append(out, (embi_str){sel_text, (uint32_t)strlen(sel_text)});
    return true;
}

EMB_TEST(wheel_event_cancel_stops_the_scroll)
{
    SETUP(k_page);
    (void)run(v, "window.w = []; document.addEventListener('wheel', function (e) { "
                 "w.push(e.deltaX + '/' + e.deltaY + '/' + e.deltaMode + '/' + (e instanceof WheelEvent) + '/' + "
                 "e.cancelable + '/' + e.bubbles + '/' + e.isTrusted + '/' + e.target.id); if (window.stop_it) "
                 "e.preventDefault(); }, { passive: false }); true");
    EMB_CHECK_EQ_INT((int)root_y(v), 0);
    emb_view_wheel(v, 50.0f, 10.0f, 0.0f, 60.0f); /* an uncanceled wheel scrolls */
    EMB_CHECK_EQ_STR(run(v, "w.join(',')"), "0/60/0/true/true/true/true/a");
    EMB_CHECK_EQ_INT((int)root_y(v), 60);
    (void)run(v, "window.stop_it = true; true");
    emb_view_wheel(v, 50.0f, 10.0f, 0.0f, 60.0f); /* a canceled one does not */
    EMB_CHECK_EQ_INT((int)root_y(v), 60);
    EMB_CHECK_EQ_STR(run(v, "w.length"), "2");
    rig_close(v, g, rt);
}

EMB_TEST(wheel_with_no_listener_scrolls)
{
    SETUP(k_page);
    emb_view_wheel(v, 50.0f, 100.0f, 0.0f, 40.0f);
    EMB_CHECK_EQ_INT((int)root_y(v), 40);
    rig_close(v, g, rt);
}

EMB_TEST(paste_event_carries_the_clipboard_text)
{
    SETUP(k_page);
    saved s;
    if (!clip_save(&s)) {
        emb_view_destroy(v);
        embi_gpu_destroy(g);
        emb_runtime_destroy(rt);
        EMB_SKIP("the clipboard is not available");
    }
    EMB_CHECK(clip_set("pasted \xC3\xA9 text"));
    (void)run(v, "document.getElementById('a').focus(); window.ev = []; "
                 "document.addEventListener('paste', function (e) { ev.push('paste:' + e.target.id + ':' + "
                 "e.clipboardData.getData('text/plain') + ':' + e.cancelable + ':' + e.isTrusted); }, true); "
                 "document.addEventListener('beforeinput', function (e) { ev.push(e.inputType + ':' + e.data); }, true); "
                 "true");
    rig_tap_key(v, "v", "KeyV", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(run(v, "ev.join(',')"), "paste:a:pasted \xC3\xA9 text:true:true,insertFromPaste:pasted \xC3\xA9 text");
    /* Cmd+V on a Mac, Shift+Insert elsewhere. */
    (void)run(v, "ev = []; true");
    rig_tap_key(v, "V", "KeyV", EMB_MOD_META);
    EMB_CHECK_EQ_STR(run(v, "ev.length"), "2");
    (void)run(v, "ev = []; true");
    rig_tap_key(v, "Insert", "Insert", EMB_MOD_SHIFT);
    EMB_CHECK_EQ_STR(run(v, "ev.length"), "2");
    /* A canceled paste inserts nothing. */
    (void)run(v, "ev = []; document.addEventListener('paste', function (e) { e.preventDefault(); }, true); true");
    rig_tap_key(v, "v", "KeyV", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(run(v, "ev.length"), "1");
    /* A canceled keydown never reaches the clipboard. */
    (void)run(v, "ev = []; document.addEventListener('keydown', function (e) { e.preventDefault(); }, true); true");
    rig_tap_key(v, "v", "KeyV", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(run(v, "ev.length"), "0");
    (void)clip_set(s.text);
    rig_close(v, g, rt);
}

EMB_TEST(paste_into_a_non_text_target_only_fires_the_event)
{
    SETUP(k_page);
    saved s;
    if (!clip_save(&s)) {
        emb_view_destroy(v);
        embi_gpu_destroy(g);
        emb_runtime_destroy(rt);
        EMB_SKIP("the clipboard is not available");
    }
    EMB_CHECK(clip_set("text"));
    (void)run(v, "window.ev = []; ['paste', 'beforeinput'].forEach(function (t) { document.addEventListener(t, "
                 "function (e) { ev.push(t + ':' + e.target.tagName); }, true); }); true");
    emb_view_clipboard(v, EMB_CLIPBOARD_PASTE); /* nothing is focused: the body gets the event, and no insertion */
    EMB_CHECK_EQ_STR(run(v, "ev.join(',')"), "paste:BODY");
    (void)clip_set(s.text);
    rig_close(v, g, rt);
}

EMB_TEST(copy_and_cut_listener_data_goes_to_the_clipboard)
{
    SETUP(k_page);
    saved s;
    if (!clip_save(&s)) {
        emb_view_destroy(v);
        embi_gpu_destroy(g);
        emb_runtime_destroy(rt);
        EMB_SKIP("the clipboard is not available");
    }
    (void)run(v, "document.getElementById('a').focus(); window.ev = []; "
                 "document.addEventListener('copy', function (e) { e.clipboardData.setData('text/plain', 'copied by page'); "
                 "e.preventDefault(); ev.push('copy'); }, true); "
                 "document.addEventListener('cut', function (e) { e.clipboardData.setData('text/plain', 'cut by page'); "
                 "e.preventDefault(); ev.push('cut'); }, true); true");
    rig_tap_key(v, "c", "KeyC", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "copied by page");
    rig_tap_key(v, "x", "KeyX", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "cut by page");
    EMB_CHECK_EQ_STR(run(v, "ev.join(',')"), "copy,cut");
    /* Ctrl+Insert copies and Shift+Delete cuts. */
    EMB_CHECK(clip_set("probe"));
    rig_tap_key(v, "Insert", "Insert", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "copied by page");
    EMB_CHECK(clip_set("probe"));
    rig_tap_key(v, "Delete", "Delete", EMB_MOD_SHIFT);
    EMB_CHECK_EQ_STR(clip_get(), "cut by page");
    (void)clip_set(s.text);
    rig_close(v, g, rt);
}

EMB_TEST(copy_default_action_uses_the_selection_and_cut_removes_it)
{
    SETUP(k_page);
    saved s;
    if (!clip_save(&s)) {
        emb_view_destroy(v);
        embi_gpu_destroy(g);
        emb_runtime_destroy(rt);
        EMB_SKIP("the clipboard is not available");
    }
    (void)run(v, "document.getElementById('a').focus(); window.ev = []; ['copy', 'cut', 'beforeinput'].forEach(function "
                 "(t) { document.addEventListener(t, function (e) { ev.push(t + (e.inputType ? ':' + e.inputType : '')); }, "
                 "true); }); true");
    /* No selection (no provider, or an empty one): nothing is written. */
    rig_tap_key(v, "c", "KeyC", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "probe");
    embi_input_set_selection_provider(v, selection, nullptr);
    rig_tap_key(v, "c", "KeyC", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "chosen");
    EMB_CHECK(clip_set("probe"));
    (void)run(v, "ev = []; true");
    rig_tap_key(v, "x", "KeyX", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "chosen");
    EMB_CHECK_EQ_STR(run(v, "ev.join(',')"), "cut,beforeinput:deleteByCut"); /* the removal is a beforeinput for W5e */
    /* A canceled copy event writes nothing of the selection. */
    EMB_CHECK(clip_set("probe"));
    (void)run(v, "document.addEventListener('copy', function (e) { e.preventDefault(); }, true); true");
    rig_tap_key(v, "c", "KeyC", EMB_MOD_CTRL);
    EMB_CHECK_EQ_STR(clip_get(), "probe");
    (void)clip_set(s.text);
    rig_close(v, g, rt);
}

EMB_TEST(navigator_clipboard_round_trip)
{
    SETUP(k_page);
    saved s;
    if (!clip_save(&s)) {
        emb_view_destroy(v);
        embi_gpu_destroy(g);
        emb_runtime_destroy(rt);
        EMB_SKIP("the clipboard is not available");
    }
    (void)run(v, "window.got = 'pending'; navigator.clipboard.writeText('h\\u00e9llo w\\u00f6rld')"
                 ".then(function () { return navigator.clipboard.readText(); })"
                 ".then(function (t) { window.got = t; }); true");
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(clip_get(), "h\xC3\xA9llo w\xC3\xB6rld"); /* it reached the system clipboard */
    EMB_CHECK_EQ_STR(run(v, "got"), "h\xC3\xA9llo w\xC3\xB6rld");
    /* readText sees text put there from outside. */
    EMB_CHECK(clip_set("from outside"));
    (void)run(v, "navigator.clipboard.readText().then(function (t) { window.got = t; }); true");
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "got"), "from outside");
    EMB_CHECK_EQ_STR(run(v, "navigator.clipboard === navigator.clipboard"), "true");
    (void)clip_set(s.text);
    rig_close(v, g, rt);
}

RIG_MAIN()
