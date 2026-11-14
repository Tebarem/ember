/*
 * <input type=number> is an editable text field of the view (src/bindings/impl_forms.c), as in Chromium: a click focuses it,
 * SDL text input types into it (only the number characters), Ctrl+A selects its text and the typing replaces it, Enter and the
 * blur fire change with the new value, ArrowUp and ArrowDown step it (input and change each), and select() works on it and
 * on text fields (Falcon's editor selects a field's text on dblclick). Falcon's editor has fields like
 * <input class="field" type="number" step="any" value="16" onchange=...> that are edited this way. Views need an offscreen
 * device: the tests skip (77) without one.
 */
#include "input_rig.h"

#include <SDL3/SDL_events.h>

static const char k_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<input id=n class=field type=number step=any value=16 style=\"display:block;margin:0;height:24px\""
    " onchange=\"log.push('change:' + this.value)\">"
    "<input id=s type=number min=0 max=4 step=2 value=2 style=\"display:block;margin:0;height:24px\""
    " onchange=\"log.push('change:' + this.value)\">"
    "<input id=t value=abc style=\"display:block;margin:0;height:24px\">"
    "<script>window.log = []; ['n', 's'].forEach(function (id) { var x = document.getElementById(id);"
    " x.addEventListener('input', function () { log.push('input:' + x.value); }); });</script>";

static void click_id(emb_view *v, const char *id)
{
    char js[256];
    SDL_snprintf(js, sizeof js,
                 "(function () { var r = document.getElementById('%s').getBoundingClientRect(); "
                 "return (r.left + r.width / 2) + ',' + (r.top + r.height / 2); })()",
                 id);
    float x = -1000.0f, y = -1000.0f;
    (void)SDL_sscanf(run(v, js), "%f,%f", &x, &y);
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

/* Text as the window's SDL_EVENT_TEXT_INPUT brings it. */
static void sdl_text(emb_view *v, const char *text)
{
    SDL_Event e;
    SDL_memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    (void)emb_view_sdl_event(v, &e, 0.0f, 0.0f, 1.0f);
}

#define SETUP(html)                                                                                        \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                         \
    EMB_REQUIRE(rt != nullptr);                                                                            \
    embi_gpu *g = nullptr;                                                                                 \
    emb_view *v = nullptr;                                                                                 \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

/* Falcon's editor: click the field, Ctrl+A, type 12, Enter: change with "12". */
EMB_TEST(click_select_all_type_and_enter_commit)
{
    SETUP(k_page);
    click_id(v, "n");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "n");
    rig_tap_key(v, "a", "KeyA", EMB_MOD_CTRL);
    sdl_text(v, "1");
    sdl_text(v, "2");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('n').value"), "12");
    EMB_CHECK_EQ_STR(run(v, "log.join()"), "input:1,input:12");
    (void)run(v, "log = []; true");
    rig_tap_key(v, "Enter", "Enter", 0);
    EMB_CHECK_EQ_STR(run(v, "log.join()"), "change:12");
    /* nothing changed since: the blur fires no second change */
    (void)run(v, "log = []; true");
    click_id(v, "t");
    EMB_CHECK_EQ_STR(run(v, "log.join()"), "");
    rig_close(v, g, rt);
}

/* Only digits, '.', ',', '-', '+', 'e' and 'E' get in; text that is not a number yet leaves the value empty while the field
 * keeps it; Backspace edits; the blur commits. */
EMB_TEST(typing_is_filtered_and_invalid_text_reads_empty)
{
    SETUP(k_page);
    click_id(v, "n");
    rig_tap_key(v, "a", "KeyA", EMB_MOD_CTRL);
    sdl_text(v, "x");
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('n').value + '|' + log.join()"), "16|");
    sdl_text(v, "3a");
    sdl_text(v, "e");
    EMB_CHECK_EQ_STR(run(v, "var n = document.getElementById('n'); n.value + '|' + n.validity.badInput + '|' + "
                            "isNaN(n.valueAsNumber)"),
                     "|true|true");
    rig_tap_key(v, "Backspace", "Backspace", 0);
    EMB_CHECK_EQ_STR(run(v, "n.value + '|' + n.validity.badInput"), "3|false");
    EMB_CHECK_EQ_STR(run(v, "log.join()"), "input:3,input:,input:3");
    (void)run(v, "log = []; true");
    click_id(v, "t");
    EMB_CHECK_EQ_STR(run(v, "log.join()"), "change:3");
    rig_close(v, g, rt);
}

/* ArrowUp and ArrowDown step by the step within min and max, firing input and change on each step. */
EMB_TEST(arrow_keys_step_with_input_and_change)
{
    SETUP(k_page);
    click_id(v, "s");
    rig_tap_key(v, "ArrowUp", "ArrowUp", 0);
    rig_tap_key(v, "ArrowUp", "ArrowUp", 0); /* at max 4 */
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('s').value + '|' + log.join()"), "4|input:4,change:4");
    (void)run(v, "log = []; true");
    rig_tap_key(v, "ArrowDown", "ArrowDown", 0);
    rig_tap_key(v, "ArrowDown", "ArrowDown", 0);
    rig_tap_key(v, "ArrowDown", "ArrowDown", 0); /* at min 0 */
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('s').value + '|' + log.join()"),
                     "0|input:2,change:2,input:0,change:0");
    /* step=any steps by 1 */
    (void)run(v, "log = []; document.getElementById('n').focus(); true");
    rig_tap_key(v, "ArrowUp", "ArrowUp", 0);
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('n').value + '|' + log.join()"), "17|input:17,change:17");
    /* a page that cancels the keydown keeps the value */
    (void)run(v, "document.getElementById('n').addEventListener('keydown', function (e) { e.preventDefault(); }); true");
    rig_tap_key(v, "ArrowUp", "ArrowUp", 0);
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('n').value"), "17");
    rig_close(v, g, rt);
}

/* selectionStart is null and setSelectionRange throws for number; select() selects its text, and still a text field's. */
EMB_TEST(selection_api_is_null_and_select_works)
{
    SETUP(k_page);
    EMB_CHECK_EQ_STR(run(v, "var n = document.getElementById('n'); String(n.selectionStart) + ',' + "
                            "String(n.selectionEnd) + ',' + String(n.selectionDirection)"),
                     "null,null,null");
    EMB_CHECK_EQ_STR(run(v, "var r = 'none'; try { n.setSelectionRange(0, 1); } catch (e) { r = e.name; } r"),
                     "InvalidStateError");
    (void)run(v, "n.focus(); n.select(); true");
    sdl_text(v, "8");
    EMB_CHECK_EQ_STR(run(v, "n.value"), "8");
    /* a text field: dblclick-style select() then typing replaces the whole text */
    (void)run(v, "var t = document.getElementById('t'); t.focus(); t.select(); true");
    EMB_CHECK_EQ_STR(run(v, "t.selectionStart + ',' + t.selectionEnd"), "0,3");
    sdl_text(v, "xy");
    EMB_CHECK_EQ_STR(run(v, "t.value"), "xy");
    rig_close(v, g, rt);
}

RIG_MAIN()
