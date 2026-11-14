/*
 * The events of the form controls bubble as HTML says (W8b, src/bindings/impl_forms.c): input (bubbles, composed) and
 * change (bubbles) of a checkbox clicked directly or through its label, input and change of a text field the user typed
 * into and left, and the select event of select(). They are trusted. Svelte 5 delegates change and input to the root of
 * the app, so a change that does not bubble never reaches its handler (the toggle-all of the Svelte TodoMVC). Also :checked,
 * which follows the checkedness rather than the checked attribute. Views need an offscreen device: the tests skip (77)
 * without one.
 */
#include "input_rig.h"

static const char k_page[] = "<!doctype html><body style=\"margin:0\">"
                             "<div id=root>"
                             "<input id=cb type=checkbox style=\"display:block;width:20px;height:20px;margin:0\">"
                             "<label id=lab for=cb style=\"display:block;width:100px;height:20px\">lab</label>"
                             "<input id=t style=\"display:block;margin:0;height:24px;box-sizing:border-box\">"
                             "<input id=u style=\"display:block;margin:0;height:24px;box-sizing:border-box\">"
                             "</div>";

/* Logs at #root in the bubble phase only: "type:target:bubbles:composed:trusted". */
static const char k_log[] =
    "window.log = []; ['input','change','select'].forEach(function (t) { document.getElementById('root')"
    ".addEventListener(t, function (e) { if (e.eventPhase === 3) log.push(t + ':' + e.target.id + ':' + e.bubbles + ':' +"
    " e.composed + ':' + e.isTrusted); }); }); true";

static void center(emb_view *v, const char *id, float *x, float *y)
{
    char js[256];
    SDL_snprintf(js, sizeof js,
                 "(function () { var r = document.getElementById('%s').getBoundingClientRect(); "
                 "return (r.left + r.width / 2) + ',' + (r.top + r.height / 2); })()",
                 id);
    float cx = -1000.0f, cy = -1000.0f;
    (void)SDL_sscanf(run(v, js), "%f,%f", &cx, &cy);
    *x = cx;
    *y = cy;
}

static void click_id(emb_view *v, const char *id)
{
    float x, y;
    center(v, id, &x, &y);
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

EMB_TEST(checkbox_input_and_change_bubble)
{
    SETUP(k_page);
    (void)run(v, k_log);
    click_id(v, "cb");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "input:cb:true:true:true,change:cb:true:false:true");
    EMB_CHECK_EQ_STR(run(v, "String(document.getElementById('cb').checked)"), "true");
    /* Through the label: the same two events at the checkbox, which reach the root. */
    (void)run(v, "log = []; true");
    click_id(v, "lab");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "input:cb:true:true:true,change:cb:true:false:true");
    EMB_CHECK_EQ_STR(run(v, "String(document.getElementById('cb').checked)"), "false");
    /* A script's click() runs the same activation; its events are trusted too, as in browsers. */
    (void)run(v, "log = []; document.getElementById('cb').click(); true");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "input:cb:true:true:true,change:cb:true:false:true");
    rig_close(v, g, rt);
}

EMB_TEST(text_input_and_commit_change_bubble)
{
    SETUP(k_page);
    (void)run(v, k_log);
    click_id(v, "t");
    emb_view_text_input(v, "ab", 2); /* one insertion, one input event */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "input:t:true:true:true");
    (void)run(v, "log = []; true");
    click_id(v, "u"); /* the blur commits the edit */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "change:t:true:false:true");
    rig_close(v, g, rt);
}

EMB_TEST(select_event_bubbles)
{
    SETUP(k_page);
    (void)run(v, k_log);
    (void)run(v, "var t = document.getElementById('t'); t.value = 'xyz'; t.select(); true");
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "select:t:true:false:true");
    rig_close(v, g, rt);
}

/* :checked follows the checkedness (W8b): a checkbox checked through the IDL property or a click, with no checked attribute,
 * matches :checked in matches() and querySelector and in the cascade (`:checked + label`, the TodoMVC toggle), and so does a
 * radio unchecked by its group and an option picked through select.value. */
static const char k_checked_page[] =
    "<!doctype html><style>#cb:checked + #lab { color: rgb(255, 0, 0) } #r2:checked ~ #rl { color: rgb(0, 128, 0) }"
    " option:checked { color: rgb(0, 0, 255) }</style><body style=\"margin:0\">"
    "<input id=cb type=checkbox style=\"display:block;width:20px;height:20px;margin:0\"><label id=lab for=cb>lab</label>"
    "<input id=r1 type=radio name=g checked><input id=r2 type=radio name=g><span id=rl>radio</span>"
    "<select id=s><option id=o1 value=a>a</option><option id=o2 value=b>b</option></select>";

EMB_TEST(checked_follows_checkedness)
{
    SETUP(k_checked_page);
    const char *lab_color = "String(getComputedStyle(document.getElementById('lab')).color === 'rgb(255, 0, 0)')";
    EMB_CHECK_EQ_STR(run(v, lab_color), "false");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('cb').checked = true;"
                            "String(document.getElementById('cb').matches(':checked')) + ',' +"
                            "document.getElementById('cb').hasAttribute('checked')"),
                     "true,false");
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, lab_color), "true");
    EMB_CHECK_EQ_STR(run(v, "Array.from(document.querySelectorAll(':checked')).map(function (e) { return e.id; }).join()"),
                     "cb,r1,o1");
    /* A click unchecks it again; the next frame's style has the label back to the default color. */
    click_id(v, "cb");
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "String(document.getElementById('cb').checked)"), "false");
    EMB_CHECK_EQ_STR(run(v, lab_color), "false");
    /* The radio group: checking r2 unchecks r1. */
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('r2').checked = true;"
                            "document.getElementById('r1').matches(':checked') + ',' + getComputedStyle("
                            "document.getElementById('rl')).color"),
                     "false,rgb(0, 128, 0)");
    /* Options: selectedness, not the selected attribute. */
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('s').value = 'b';"
                            "document.getElementById('o2').matches(':checked') + ',' +"
                            "document.getElementById('o1').matches(':checked') + ',' +"
                            "getComputedStyle(document.getElementById('o2')).color"),
                     "true,false,rgb(0, 0, 255)");
    rig_close(v, g, rt);
}

RIG_MAIN()
