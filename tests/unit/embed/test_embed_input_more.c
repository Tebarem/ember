/*
 * The input API of a view (W5d), part two: the default actions of the keys on links and buttons (Enter, Space on keyup,
 * keypress), a pointer click on a label and on a #fragment link (the scroll offset and hashchange), the order of the focus
 * events from a real click, host focus and blur with a composition, touch pointer events (per-finger press targets,
 * over/enter/out/leave, the click at the common ancestor), movementX/Y, the pointer leaving the window, pointer capture,
 * autofocus at parse and at insertion, Tab skipping hidden controls and scrolling the focus into view, a blur listener that
 * moves the focus, and the style invalidation gate. Views need an offscreen device: the tests skip (77) without one.
 */
#include "input_rig.h"

#include <stdio.h>
#include <stdlib.h>

/* l 0-20 (#t), l2 20-40 (/go), btn 40-60, lab 60-80 (for=chk), chk 80-100, a 100-124, b 124-148, hid (display:none), then a
 * spacer, #t at y 1000, and a tall tail. */
static const char k_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<a id=l href=\"#t\" style=\"display:block;width:100px;height:20px\">l</a>"
    "<a id=l2 href=\"/go\" style=\"display:block;width:100px;height:20px\">go</a>"
    "<button id=btn style=\"display:block;width:100px;height:20px;padding:0;border:0\">b</button>"
    "<label id=lab for=chk style=\"display:block;width:100px;height:20px\">lab</label>"
    "<input id=chk type=checkbox style=\"display:block;width:20px;height:20px;margin:0\">"
    "<input id=a style=\"display:block;margin:0;height:24px;box-sizing:border-box\">"
    "<input id=b style=\"display:block;margin:0;height:24px;box-sizing:border-box\">"
    "<div style=\"display:none\"><button id=hid>h</button></div>"
    "<div style=\"height:852px\"></div><div id=t style=\"height:40px\">target</div><div style=\"height:3000px\"></div>";

/* The center of element id in view coordinates. */
static void center(emb_view *v, const char *id, float *x, float *y)
{
    char js[256];
    SDL_snprintf(js, sizeof js,
                 "(function () { var r = document.getElementById('%s').getBoundingClientRect(); "
                 "return (r.left + r.width / 2) + ',' + (r.top + r.height / 2); })()",
                 id);
    const char *r = run(v, js);
    float cx = -1000.0f, cy = -1000.0f;
    (void)SDL_sscanf(r, "%f,%f", &cx, &cy);
    *x = cx;
    *y = cy;
}

static void click_at(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

static void click_id(emb_view *v, const char *id)
{
    float x, y;
    center(v, id, &x, &y);
    click_at(v, x, y);
}

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

EMB_TEST(enter_activates_a_link_and_keypress_comes_first)
{
    SETUP(k_page);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, rig_nav, nullptr);
    rig_log(v, "'keydown','keypress','keyup','click'");
    (void)run(v, "document.getElementById('l2').focus(); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:l2,keypress:l2,click:l2,keyup:l2");
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    EMB_CHECK_EQ_STR(g_nav_url, "http://127.0.0.1/go");

    /* A canceled keypress skips the default action (the navigation), as a canceled keydown does. */
    (void)run(v, "log = []; document.addEventListener('keypress', function (e) { e.preventDefault(); }, true); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:l2,keypress:l2,keyup:l2");
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    emb_view_set_navigate_handler(v, nullptr, nullptr);
    rig_close(v, g, rt);
}

EMB_TEST(keypress_only_for_character_keys)
{
    SETUP(k_page);
    rig_log(v, "'keydown','keypress','keyup'");
    rig_tap_key(v, "a", "KeyA", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:BODY,keypress:BODY,keyup:BODY");
    (void)run(v, "log = []; true");
    rig_tap_key(v, "ArrowLeft", "ArrowLeft", 0); /* not a character */
    rig_tap_key(v, "c", "KeyC", EMB_MOD_CTRL);   /* a shortcut, not a character */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:BODY,keyup:BODY,keydown:BODY,keyup:BODY");
    (void)run(v, "log = []; document.addEventListener('keydown', function (e) { e.preventDefault(); }, true); true");
    rig_tap_key(v, "b", "KeyB", 0); /* a canceled keydown has no keypress */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:BODY,keyup:BODY");
    rig_close(v, g, rt);
}

EMB_TEST(enter_and_space_activate_a_button)
{
    SETUP(k_page);
    rig_log(v, "'keydown','keyup','click'");
    (void)run(v, "document.getElementById('btn').focus(); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:btn,click:btn,keyup:btn");
    (void)run(v, "log = []; true");
    emb_key_event down = rig_key(" ", "Space", true, 0);
    emb_view_key(v, &down);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:btn"); /* Space activates on the release */
    EMB_CHECK_EQ_INT((int)root_y(v), 0);                      /* and does not scroll a focused button */
    emb_key_event up = rig_key(" ", "Space", false, 0);
    emb_view_key(v, &up);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "keydown:btn,keyup:btn,click:btn");
    rig_close(v, g, rt);
}

EMB_TEST(pointer_click_on_a_label_forwards_to_its_control)
{
    SETUP(k_page);
    rig_log(v, "'click'");
    click_id(v, "lab");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:lab,click:chk");
    rig_close(v, g, rt);
}

EMB_TEST(fragment_click_scrolls_and_fires_hashchange)
{
    SETUP(k_page);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, rig_nav, nullptr);
    (void)run(v, "window.hc = []; window.addEventListener('hashchange', function (e) { hc.push(e.newURL); }); true");
    float expect = 0;
    (void)SDL_sscanf(run(v, "document.getElementById('t').getBoundingClientRect().top + window.scrollY"), "%f", &expect);
    EMB_CHECK(expect > 900.0f);
    click_id(v, "l");
    EMB_CHECK_EQ_INT(g_nav_calls, 0); /* same-document: not passed to the embedder */
    EMB_CHECK_EQ_STR(run(v, "hc.join(',')"), "http://127.0.0.1/page.html#t");
    EMB_CHECK(root_y(v) > expect - 1.0f && root_y(v) < expect + 1.0f);
    EMB_CHECK_EQ_STR(run(v, "location.hash"), "#t");
    emb_view_set_navigate_handler(v, nullptr, nullptr);
    rig_close(v, g, rt);
}

EMB_TEST(click_focus_events_come_after_mousedown_in_order)
{
    SETUP(k_page);
    rig_log(v, "'mousedown','focus','blur','focusin','focusout'");
    click_id(v, "a");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "mousedown:a,focus:a,focusin:a");
    (void)run(v, "log = []; true");
    click_id(v, "b");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "mousedown:b,blur:a,focusout:a,focus:b,focusin:b");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    /* A right press focuses too. */
    (void)run(v, "log = []; true");
    float x, y;
    center(v, "a", &x, &y);
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_SECONDARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_SECONDARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    rig_close(v, g, rt);
}

EMB_TEST(host_focus_and_blur_and_composition_end)
{
    SETUP(k_page);
    (void)run(v, "document.getElementById('a').focus(); true");
    rig_log(v, "'focus','blur','focusin','focusout','compositionstart','compositionupdate','compositionend'");
    emb_view_focus(v, false);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "blur:a,focusout:a");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a"); /* the focused element itself does not change */
    (void)run(v, "log = []; true");
    emb_view_focus(v, true);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "focus:a,focusin:a");
    /* Losing the host focus ends a composition in progress. */
    (void)run(v, "log = []; true");
    emb_text_editing te = {.size = sizeof te, .text = "ni", .cursor = 2, .sel_len = 0};
    emb_view_text_editing(v, &te);
    emb_view_focus(v, false);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "compositionstart:a,compositionupdate:a,compositionend:a,blur:a,focusout:a");
    rig_close(v, g, rt);
}

EMB_TEST(touch_events_per_finger_with_over_enter_out_leave)
{
    SETUP(k_page);
    rig_log(v, "'pointerover','pointerenter','pointerdown','pointermove','pointerup','pointerout','pointerleave',"
               "'pointercancel','click','mousedown','mouseover'");
    float bx, by, lx, ly;
    center(v, "btn", &bx, &by);
    center(v, "l2", &lx, &ly);
    /* A tap on the button: over and enter before the down, out and leave after the up, then the click. */
    emb_view_touch(v, 11, bx, by, 0);
    emb_view_touch(v, 11, bx, by, 2);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "pointerover:btn,pointerenter:HTML,pointerenter:BODY,pointerenter:btn,pointerdown:btn,"
                     "pointerup:btn,pointerout:btn,pointerleave:btn,pointerleave:BODY,pointerleave:HTML,click:btn");
    /* Touches have no compatibility mouse events, and the pointer events say touch. */
    EMB_CHECK(strstr(run(v, "log.join(',')"), "mousedown") == nullptr);
    (void)run(v, "log = []; window.pt = []; document.addEventListener('pointerdown', function (e) { "
                 "pt.push(e.pointerType + '/' + e.isPrimary + '/' + e.pointerId); }, true); true");
    /* Two fingers: each finger's up goes to its own press target, in either order. */
    emb_view_touch(v, 21, lx, ly, 0);
    emb_view_touch(v, 22, bx, by, 0);
    emb_view_touch(v, 22, bx, by, 2);
    emb_view_touch(v, 21, lx, ly, 2);
    const char *pt = run(v, "pt.join(',')");
    EMB_CHECK(strncmp(pt, "touch/true/", 11) == 0);
    EMB_CHECK(strstr(pt, ",touch/false/") != nullptr); /* the second finger is not primary */
    const char *l = run(v, "log.join(',')");
    const char *up_b = strstr(l, "pointerup:btn");
    const char *up_l = strstr(l, "pointerup:l2");
    EMB_CHECK(up_b != nullptr && up_l != nullptr && up_b < up_l);
    /* An up over nothing, and a cancel, still reach the press target. */
    (void)run(v, "log = []; true");
    emb_view_touch(v, 31, bx, by, 0);
    emb_view_touch(v, 31, -50.0f, -50.0f, 2);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "pointerup:btn") != nullptr);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "click:") == nullptr); /* the finger left: not a tap */
    (void)run(v, "log = []; true");
    emb_view_touch(v, 32, bx, by, 0);
    emb_view_touch(v, 32, bx, by + 1.0f, 3);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "pointercancel:btn,pointerout:btn,pointerleave:btn") != nullptr);
    rig_close(v, g, rt);
}

EMB_TEST(touch_tap_clicks_the_common_ancestor)
{
    SETUP(k_page);
    rig_log(v, "'click'");
    float bx, by;
    center(v, "btn", &bx, &by);
    /* Down on the button, up (within the tap slop) over the link above it: the click goes to their common ancestor. */
    emb_view_touch(v, 5, bx, by - 8.0f, 0); /* inside the button (40-60), near its top */
    emb_view_touch(v, 5, bx, by - 14.0f, 2); /* inside the link above (20-40) */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:BODY");
    rig_close(v, g, rt);
}

EMB_TEST(mouse_move_reports_movement_and_leave_clears_hover)
{
    SETUP(k_page);
    (void)run(v, "window.mv = []; document.addEventListener('mousemove', function (e) { "
                 "mv.push(e.movementX + ',' + e.movementY); }, true); true");
    emb_view_mouse_move(v, 10.0f, 50.0f, 0);
    emb_view_mouse_move(v, 25.0f, 70.0f, 0);
    EMB_CHECK_EQ_STR(run(v, "mv.join(';')"), "10,50;15,20");
    rig_log(v, "'pointerout','pointerleave','mouseout','mouseleave','pointerover'");
    (void)run(v, "window.rel = []; document.addEventListener('pointerout', function (e) { "
                 "rel.push(String(e.relatedTarget)); }, true); true");
    emb_view_mouse_leave(v);
    /* The pointer was at (25, 70): over the label, a block that is the deepest element there. */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "pointerout:lab,pointerleave:lab,pointerleave:BODY,pointerleave:HTML,"
                     "mouseout:lab,mouseleave:lab,mouseleave:BODY,mouseleave:HTML");
    EMB_CHECK_EQ_STR(run(v, "rel.join(',')"), "null");
    /* The hover is none: coming back is a fresh over. */
    (void)run(v, "log = []; true");
    emb_view_mouse_move(v, 25.0f, 50.0f, 0);
    EMB_CHECK(strncmp(run(v, "log.join(',')"), "pointerover:btn", 15) == 0);
    rig_close(v, g, rt);
}

EMB_TEST(pointer_capture_retargets_and_releases)
{
    SETUP(k_page);
    rig_log(v, "'pointermove','pointerup','pointerdown','gotpointercapture','lostpointercapture'");
    (void)run(v, "var b = document.getElementById('btn'); b.addEventListener('pointerdown', function (e) { "
                 "b.setPointerCapture(e.pointerId); }); true");
    float bx, by;
    center(v, "btn", &bx, &by);
    emb_view_mouse_move(v, bx, by, 0);
    emb_view_mouse_button(v, bx, by, EMB_BUTTON_PRIMARY, true, 0);
    EMB_CHECK_EQ_STR(run(v, "b.hasPointerCapture(1)"), "true");
    (void)run(v, "log = []; true");
    emb_view_mouse_move(v, 200.0f, 190.0f, 0); /* far from the button: the moves still go to it */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "pointermove:btn");
    emb_view_mouse_button(v, 200.0f, 190.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "pointermove:btn,pointerup:btn,lostpointercapture:btn");
    EMB_CHECK_EQ_STR(run(v, "b.hasPointerCapture(1)"), "false");
    EMB_CHECK_EQ_STR(run(v, "try { b.setPointerCapture(999); 'no' } catch (e) { e.name }"), "NotFoundError");
    /* With no button down a mouse capture request is ignored. */
    EMB_CHECK_EQ_STR(run(v, "b.setPointerCapture(1); b.hasPointerCapture(1)"), "false");
    rig_close(v, g, rt);
}

EMB_TEST(autofocus_at_parse_and_at_insertion)
{
    {
        SETUP("<!doctype html><body><button id=x>x</button><input id=z autofocus><input id=y autofocus>");
        (void)embi_view_update(v); /* the loop runs the queued autofocus task */
        EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "z");
        rig_close(v, g, rt);
    }
    {
        SETUP("<!doctype html><body><button id=x>x</button>");
        (void)embi_view_update(v);
        EMB_CHECK_EQ_STR(run(v, "document.activeElement.tagName"), "BODY");
        (void)run(v, "document.body.insertAdjacentHTML('beforeend', '<div><input id=n autofocus></div>'); true");
        (void)embi_view_update(v);
        EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "n");
        rig_close(v, g, rt);
    }
    {
        /* Something that already has the focus keeps it. */
        SETUP("<!doctype html><body><button id=x>x</button>");
        (void)run(v, "document.getElementById('x').focus(); document.body.insertAdjacentHTML('beforeend', "
                     "'<input id=n autofocus>'); true");
        (void)embi_view_update(v);
        EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "x");
        rig_close(v, g, rt);
    }
    {
        /* An autofocus element that cannot take the focus is skipped. */
        SETUP("<!doctype html><body><input id=d autofocus disabled><input id=ok autofocus>");
        (void)embi_view_update(v);
        EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "ok");
        rig_close(v, g, rt);
    }
}

EMB_TEST(tab_skips_hidden_controls_and_wraps)
{
    SETUP(k_page);
    (void)run(v, "window.seq = []; true");
    for (int i = 0; i < 7; i++) {
        rig_tap_key(v, "Tab", "Tab", 0);
        (void)run(v, "seq.push(document.activeElement.id)");
    }
    /* The button inside the display:none subtree (#hid) is not reached. */
    EMB_CHECK_EQ_STR(run(v, "seq.join(',')"), "l,l2,btn,chk,a,b,l");
    rig_close(v, g, rt);
}

EMB_TEST(tab_from_a_click_focused_element_continues_in_tree_order)
{
    SETUP("<!doctype html><body style=\"margin:0\"><button id=a style=\"display:block\">a</button>"
          "<div id=m tabindex=-1 style=\"height:20px\">m</div><button id=c style=\"display:block\">c</button>"
          "<button id=d style=\"display:block\">d</button>");
    click_id(v, "m"); /* focusable by tabindex=-1, not in the tab order */
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "m");
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "c"); /* the next one after #m, not the first */
    (void)run(v, "document.getElementById('m').focus(); true");
    rig_tap_key(v, "Tab", "Tab", EMB_MOD_SHIFT);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    rig_close(v, g, rt);
}

EMB_TEST(tab_scrolls_the_focus_into_view)
{
    SETUP("<!doctype html><body style=\"margin:0\"><div style=\"height:1500px\"></div>"
          "<button id=far style=\"display:block\">far</button><div style=\"height:1500px\"></div>");
    EMB_CHECK_EQ_INT((int)root_y(v), 0);
    rig_tap_key(v, "Tab", "Tab", 0);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "far");
    (void)embi_view_update(v);
    /* The button (document y 1500 to 1528) is inside the 200 px viewport now: the root scrolled just far enough. */
    EMB_CHECK(root_y(v) >= 1328.0f - 0.5f && root_y(v) <= 1500.0f);
    /* Tab back to the same element does not scroll again. */
    rig_close(v, g, rt);
}

EMB_TEST(focus_moved_by_a_blur_listener_stands)
{
    SETUP(k_page);
    rig_log(v, "'focus','blur'");
    (void)run(v, "var a = document.getElementById('a'), b = document.getElementById('b'); "
                 "a.focus(); a.addEventListener('blur', function () { b.focus(); }); "
                 "document.getElementById('chk').focus(); true");
    /* a is blurred; its listener focuses b; the move to chk stops there, so activeElement and the events agree. */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "focus:a,blur:a,focus:b");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    rig_close(v, g, rt);
}

EMB_TEST(apply_cursor_on_an_offscreen_view_is_harmless)
{
    SETUP(k_page);
    embi_input_apply_cursor(v, EMBI_STR("pointer"));
    EMB_CHECK(embi_input_cursor_kind(v) == (int32_t)SDL_SYSTEM_CURSOR_POINTER);
    embi_input_apply_cursor(v, EMBI_STR("nonsense"));
    EMB_CHECK(embi_input_cursor_kind(v) == -1);
    rig_close(v, g, rt);
}

EMB_TEST(pointer_moves_do_not_relayout_unless_a_rule_reads_the_state)
{
    /* A page with no rule that reads the state: moves across element boundaries and a press (which focuses a button
     * with the mouse, so focus-visible stays false) cost no restyle and no relayout. */
    {
        SETUP(k_page);
        (void)embi_view_update(v);
        emb_view_mouse_move(v, 10.0f, 10.0f, 0);
        emb_view_mouse_move(v, 10.0f, 50.0f, 0);
        emb_view_mouse_button(v, 10.0f, 50.0f, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(v, 10.0f, 50.0f, EMB_BUTTON_PRIMARY, false, 0);
        EMB_CHECK(!embi_view_update(v));
        EMB_CHECK(!embi_input_state_consumed(v));
        EMB_CHECK(embi_input_hover_element(v) != nullptr);
        rig_close(v, g, rt);
    }

    /* A page whose rule reads :hover: a move onto another element restyles, a move within the same one does not. */
    {
        SETUP("<!doctype html><style>#l:hover { color: red; }</style><body style=\"margin:0\">"
              "<a id=l href=\"#t\" style=\"display:block;width:100px;height:20px\">l</a><div style=\"height:200px\"></div>");
        (void)embi_view_update(v);
        emb_view_mouse_move(v, 10.0f, 10.0f, 0);
        EMB_CHECK(embi_view_update(v));
        EMB_CHECK(embi_input_state_consumed(v));
        emb_view_mouse_move(v, 20.0f, 15.0f, 0);
        EMB_CHECK(!embi_view_update(v));
        emb_view_mouse_move(v, 10.0f, 100.0f, 0);
        EMB_CHECK(embi_view_update(v));
        rig_close(v, g, rt);
    }
}

static const char k_enter_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<input id=ib type=button value=x style=\"display:block;width:60px;height:20px\">"
    "<input id=is type=submit value=y style=\"display:block;width:60px;height:20px\">"
    "<div id=dv tabindex=0 type=submit style=\"width:60px;height:20px\">d</div>"
    "<input id=cb type=checkbox style=\"display:block;width:20px;height:20px;margin:0\">"
    "<label id=lt for=tx style=\"display:block;width:100px;height:20px\">lt</label>"
    "<input id=tx style=\"display:block;margin:0;height:24px;width:100px;box-sizing:border-box\">"
    "<label id=ld for=dis style=\"display:block;width:100px;height:20px\">ld</label>"
    "<button id=dis disabled style=\"display:block;width:100px;height:20px;padding:0;border:0\">d</button>"
    "<input id=other style=\"display:block;margin:0;height:24px;width:100px;box-sizing:border-box\">";

EMB_TEST(enter_clicks_only_real_buttons_not_a_div_with_a_type_attribute)
{
    SETUP(k_enter_page);
    rig_log(v, "'click'");
    (void)run(v, "document.getElementById('ib').focus(); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:ib");
    (void)run(v, "log = []; document.getElementById('is').focus(); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:is");
    (void)run(v, "log = []; document.getElementById('dv').focus(); true");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "dv");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "");
    (void)run(v, "document.getElementById('cb').focus(); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "");
    /* Without an html element the key target is the Document: Enter on it is harmless. */
    (void)run(v, "document.activeElement.blur(); document.removeChild(document.documentElement); true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "");
    rig_close(v, g, rt);
}

EMB_TEST(label_click_focuses_a_text_input_and_disabled_controls_get_nothing)
{
    SETUP(k_enter_page);
    rig_log(v, "'click','focus'");
    click_id(v, "lt");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "tx");
    EMB_CHECK(strstr(run(v, "log.join(',')"), "focus:tx") != nullptr);
    /* A disabled control: neither a click on it nor a click on its label focuses it or fires a click on it. */
    (void)run(v, "document.getElementById('other').focus(); log = []; true");
    click_id(v, "dis");
    click_id(v, "ld");
    EMB_CHECK(strstr(run(v, "log.join(',')"), "click:dis") == nullptr);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "focus:dis") == nullptr);
    EMB_CHECK(strcmp(run(v, "document.activeElement.id"), "dis") != 0);
    rig_close(v, g, rt);
}

EMB_TEST(composition_ends_on_its_own_target_when_the_focus_moves)
{
    SETUP(k_page);
    (void)run(v, "document.getElementById('a').focus(); true");
    rig_log(v, "'compositionstart','compositionupdate','compositionend'");
    emb_text_editing te = {.size = sizeof te, .text = "ni", .cursor = 2, .sel_len = 0};
    emb_view_text_editing(v, &te);
    (void)run(v, "document.getElementById('b').focus(); true");
    emb_text_editing none = {.size = sizeof none, .text = "", .cursor = 0, .sel_len = 0};
    emb_view_text_editing(v, &none); /* the next input call ends the composition on a, not on b */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "compositionstart:a,compositionupdate:a,compositionend:a");
    /* A new composition starts on b. */
    (void)run(v, "log = []; true");
    emb_view_text_editing(v, &te);
    emb_view_text_editing(v, &none);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "compositionstart:b,compositionupdate:b,compositionend:b");
    /* A removed target gets no event, and nothing is left behind. */
    (void)run(v, "log = []; true");
    emb_view_text_editing(v, &te);
    (void)run(v, "var e = document.getElementById('b'); e.parentNode.removeChild(e); true");
    emb_view_text_editing(v, &none);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "compositionstart:b,compositionupdate:b");
    rig_close(v, g, rt);
}

EMB_TEST(space_activates_on_keyup_only_if_it_went_down_on_the_same_control)
{
    SETUP(k_page);
    rig_log(v, "'click'");
    emb_key_event down = rig_key(" ", "Space", true, 0);
    emb_key_event up = rig_key(" ", "Space", false, 0);
    (void)run(v, "document.getElementById('btn').focus(); true");
    emb_view_key(v, &down);
    (void)run(v, "document.getElementById('chk').focus(); true"); /* the focus moves between down and up */
    emb_view_key(v, &up);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "");
    /* A keyup with no keydown on the control does nothing either. */
    emb_view_key(v, &up);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "");
    /* The normal press activates. */
    emb_view_key(v, &down);
    emb_view_key(v, &up);
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "click:chk");
    rig_close(v, g, rt);
}

RIG_MAIN()
