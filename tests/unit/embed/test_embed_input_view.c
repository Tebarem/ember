/*
 * The input API of a view (W5d) with a script present: the calls of ember_input.h drive the DOM events a page's listeners
 * log (the hover order and the related targets, the button bits and the click counts, the default actions of the keys
 * and their cancellation, Tab wrapping and focus fixup on removal, the composition sequence and beforeinput gating, a
 * tap that focuses), and the view's state is freed with the view (the memory baseline across create and destroy).
 * Views need an offscreen device: the tests skip (77) without one.
 */
#include "emb_test.h"

#include "base/base.h"
#include "embed/embed_int.h"
#include "embed/doc.h"
#include "embed/input_int.h"

#include <ember/ember_input.h>
#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

/* A 300x200 page: two 100 px boxes (#o holding #i, and #p) on top, two inputs below them, then a tall box to scroll. */
static const char k_page[] =
    "<!doctype html><body style=\"margin:0\">"
    "<div id=o style=\"width:100px;height:100px\"><span id=i style=\"display:block;width:50px;height:50px\">x</span></div>"
    "<div id=p style=\"width:100px;height:100px\"></div>"
    "<input id=a style=\"display:block\"><input id=b style=\"display:block\">"
    "<div style=\"height:3000px\"></div>";

/* Opens a 300x200 offscreen view on the device and loads the page, laid out. Returns nullptr when there is no device. */
static emb_view *open_page(emb_runtime *rt, embi_gpu **gout)
{
    *gout = embi_gpu_create(nullptr, false);
    if (*gout == nullptr) {
        return nullptr;
    }
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = 300;
    d.height = 200;
    d.scale = 1.0f;
    d.device = embi_gpu_device(*gout);
    emb_view *v = emb_view_create(rt, &d);
    if (v == nullptr) {
        return nullptr;
    }
    (void)emb_view_load_html(v, k_page, strlen(k_page), "http://127.0.0.1/page.html");
    (void)embi_view_update(v);
    return v;
}

static void close_page(emb_view *v, embi_gpu *g, emb_runtime *rt)
{
    emb_view_destroy(v);
    embi_gpu_destroy(g);
    emb_runtime_destroy(rt);
}

/* The result text of the last emb_view_eval. */
static char g_res[4096];
static bool g_ok;

static void on_result(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    g_ok = ok;
    size_t n = len < sizeof g_res - 1 ? len : sizeof g_res - 1;
    memcpy(g_res, text, n);
    g_res[n] = '\0';
}

/* Runs js in the view and returns its result text (or "!throw"). */
static const char *run(emb_view *v, const char *js)
{
    g_res[0] = '\0';
    g_ok = false;
    (void)emb_view_eval(v, js, strlen(js), on_result, nullptr);
    return g_ok ? g_res : "!throw";
}

/* Installs the logger: every event of the listed types at the document (capture phase, so each dispatch logs once) appends
 * "type:target" to window.log, the target being its id, or its tag name. */
static const char k_logger[] =
    "window.log = []; "
    "['pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'pointermove', 'pointerdown', 'pointerup', "
    " 'mouseover', 'mouseout', 'mouseenter', 'mouseleave', 'mousemove', 'mousedown', 'mouseup', 'click', 'dblclick', "
    " 'contextmenu', 'auxclick', 'keydown', 'keyup', 'beforeinput', 'compositionstart', 'compositionupdate', "
    " 'compositionend', 'focus', 'blur', 'focusin', 'focusout'].forEach(function (t) { "
    "  document.addEventListener(t, function (e) { "
    "    log.push(t + ':' + (e.target.id || e.target.tagName)); }, true); }); "
    "true";

/* The root viewport's scroll offset. */
static float root_y(emb_view *v)
{
    float x = 0, y = 0;
    embi_doc_root_offset(v->doc, &x, &y);
    return y;
}

static emb_key_event key(const char *name, const char *code, bool down, uint32_t mods)
{
    emb_key_event k;
    memset(&k, 0, sizeof k);
    k.size = sizeof k;
    k.down = down;
    k.mods = mods;
    k.key = name;
    k.code = code;
    return k;
}

static emb_key_event tab(bool shift)
{
    return key("Tab", "Tab", true, shift ? EMB_MOD_SHIFT : 0);
}

/* The skip path of the tests: no device means no view, so the test is not run. */
#define OPEN_OR_SKIP(v, rt, g)                                                                             \
    do {                                                                                                   \
        (v) = open_page((rt), &(g));                                                                       \
        if ((v) == nullptr) {                                                                              \
            emb_test_cur_skip = "no GPU device";                                                           \
            if ((g) != nullptr) {                                                                          \
                embi_gpu_destroy(g);                                                                       \
            }                                                                                              \
            emb_runtime_destroy(rt);                                                                       \
            return;                                                                                        \
        }                                                                                                  \
    } while (0)

EMB_TEST(view_hover_order_and_related_targets)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);

    emb_view_mouse_move(v, 10.0f, 10.0f, 0); /* into #i, through #o */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "pointerover:i,pointerenter:HTML,pointerenter:BODY,pointerenter:o,pointerenter:i,pointermove:i,"
                     "mouseover:i,mouseenter:HTML,mouseenter:BODY,mouseenter:o,mouseenter:i,mousemove:i");

    (void)run(v, "log = []");
    emb_view_mouse_move(v, 10.0f, 120.0f, 0); /* into #p: leave #i then #o, enter #p */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "pointerout:i,pointerleave:i,pointerleave:o,pointerover:p,pointerenter:p,pointermove:p,"
                     "mouseout:i,mouseleave:i,mouseleave:o,mouseover:p,mouseenter:p,mousemove:p");

    /* relatedTarget of the over and out events: the element left (over) and the element entered (out). */
    (void)run(v, "window.rel = []; document.addEventListener('pointerover', function (e) { "
                 "rel.push('over:' + (e.relatedTarget && e.relatedTarget.id)); }, true); "
                 "document.addEventListener('pointerout', function (e) { "
                 "rel.push('out:' + (e.relatedTarget && e.relatedTarget.id)); }, true);");
    emb_view_mouse_move(v, 10.0f, 10.0f, 0);
    EMB_CHECK_EQ_STR(run(v, "rel.join(',')"), "out:i,over:p"); /* the out of #p goes to #i, the over of #i comes from #p */
    close_page(v, g, rt);
}

EMB_TEST(view_mouse_buttons_and_click_counts)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);
    (void)run(v, "window.bt = []; "
                 "document.addEventListener('pointerdown', function (e) { bt.push('p' + e.button + '/' + e.buttons); }, "
                 "true); "
                 "document.addEventListener('mousedown', function (e) { bt.push('md' + e.button + '/' + e.buttons); }, "
                 "true); "
                 "document.addEventListener('mouseup', function (e) { bt.push('mu' + e.button + '/' + e.buttons); }, "
                 "true);");

    emb_view_mouse_move(v, 10.0f, 10.0f, 0);
    /* Middle button: the DOM button is 1 and its bit is 4. */
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_AUXILIARY, true, 0);
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_AUXILIARY, false, 0);
    /* Right button: DOM button 2, bit 2, and a contextmenu. */
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_SECONDARY, true, 0);
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_SECONDARY, false, 0);
    /* Primary: DOM button 0, bit 1. */
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK_EQ_STR(run(v, "bt.join(',')"),
                     "p1/4,md1/4,mu1/0,p2/2,md2/2,mu2/0,p0/1,md0/1,mu0/0");
    EMB_CHECK(strstr(run(v, "log.join(',')"), "contextmenu:i") != nullptr);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "auxclick:i") != nullptr);
    close_page(v, g, rt);
}

static double fixed_clock(void)
{
    return 1000.0;
}

EMB_TEST(view_double_click_counts_two)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);
    (void)run(v, "window.cd = []; ['click', 'dblclick'].forEach(function (t) { "
                 "document.addEventListener(t, function (e) { cd.push(t + ':' + e.detail); }, true); });");
    emb_view_mouse_move(v, 10.0f, 10.0f, 0);
    embi_input_set_clock(fixed_clock); /* the real clock could pass the 500 ms window under a sanitizer */
    for (int i = 0; i < 2; i++) {
        emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, false, 0);
    }
    embi_input_set_clock(nullptr);
    EMB_CHECK_EQ_STR(run(v, "cd.join(',')"), "click:1,click:2,dblclick:2");
    close_page(v, g, rt);
}

EMB_TEST(view_removed_hover_target_is_dropped)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);
    emb_view_mouse_move(v, 10.0f, 10.0f, 0);
    EMB_CHECK(strcmp(run(v, "document.getElementById('i').remove(); log = []; 'ok'"), "ok") == 0);
    emb_view_mouse_move(v, 10.0f, 120.0f, 0); /* the old target is gone: no leave events for it */
    EMB_CHECK(strstr(run(v, "log.join(',')"), "pointerleave:i") == nullptr);
    EMB_CHECK(strstr(run(v, "log.join(',')"), "pointerover:p") != nullptr);
    close_page(v, g, rt);
}

EMB_TEST(view_tab_wraps_and_fixes_up_removed_focus)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    emb_view_key(v, &(emb_key_event){.size = sizeof(emb_key_event), .down = true, .key = "Tab", .code = "Tab"});
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    emb_view_key(v, &(emb_key_event){.size = sizeof(emb_key_event), .down = true, .key = "Tab", .code = "Tab"});
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b");
    emb_view_key(v, &(emb_key_event){.size = sizeof(emb_key_event), .down = true, .key = "Tab", .code = "Tab"});
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a"); /* wraps forward */
    emb_view_key(v, &(emb_key_event){.size = sizeof(emb_key_event), .down = true, .key = "Tab", .code = "Tab",
                                     .mods = EMB_MOD_SHIFT});
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "b"); /* wraps backward */

    /* The focused element is removed: the focus goes to the body, and the next Tab starts from the first element. */
    (void)run(v, "document.getElementById('b').remove()");
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.tagName"), "BODY");
    emb_view_key(v, &(emb_key_event){.size = sizeof(emb_key_event), .down = true, .key = "Tab", .code = "Tab"});
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    close_page(v, g, rt);
}

EMB_TEST(view_canceled_keydown_skips_tab_and_scrolling)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    (void)run(v, "document.addEventListener('keydown', function (e) { e.preventDefault(); }, false); true");
    emb_key_event t = tab(false);
    emb_view_key(v, &t);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.tagName"), "BODY"); /* Tab did not move the focus */
    emb_key_event down = key("ArrowDown", "ArrowDown", true, 0);
    emb_view_key(v, &down);
    EMB_CHECK(root_y(v) == 0.0f); /* the arrow did not scroll */
    close_page(v, g, rt);
}

EMB_TEST(view_scroll_keys_are_the_default_action)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(root_y(v) == 0.0f);
    emb_key_event down = key("ArrowDown", "ArrowDown", true, 0);
    emb_view_key(v, &down);
    EMB_CHECK(root_y(v) == 40.0f); /* EMBI_VIEW_KEY_PX */
    emb_key_event up_release = key("ArrowDown", "ArrowDown", false, 0);
    emb_view_key(v, &up_release); /* a keyup scrolls nothing */
    EMB_CHECK(root_y(v) == 40.0f);
    emb_key_event end = key("End", "End", true, 0);
    emb_view_key(v, &end);
    EMB_CHECK(root_y(v) > 40.0f);
    emb_key_event home = key("Home", "Home", true, 0);
    emb_view_key(v, &home);
    EMB_CHECK(root_y(v) == 0.0f);
    close_page(v, g, rt);
}

static int g_nav_calls;
static char g_nav_url[256];

static void count_nav(void *user, const char *url, size_t len)
{
    (void)user;
    g_nav_calls++;
    size_t n = len < sizeof g_nav_url - 1 ? len : sizeof g_nav_url - 1;
    memcpy(g_nav_url, url, n);
    g_nav_url[n] = '\0';
}

/* The navigation hook is permanent: a link clicked from script (not inside an input call) reaches the embedder. */
EMB_TEST(view_navigation_hook_gets_script_clicks)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    g_nav_calls = 0;
    emb_view_set_navigate_handler(v, count_nav, nullptr);
    (void)run(v, "document.body.insertAdjacentHTML('beforeend', '<a id=l href=\"/next\">n</a>'); "
                 "document.getElementById('l').click(); true");
    EMB_CHECK_EQ_INT(g_nav_calls, 1);
    EMB_CHECK(strcmp(g_nav_url, "http://127.0.0.1/next") == 0);
    emb_view_set_navigate_handler(v, nullptr, nullptr);
    (void)run(v, "document.getElementById('l').click(); true");
    EMB_CHECK_EQ_INT(g_nav_calls, 1); /* removed hook: nothing is delivered */
    close_page(v, g, rt);
}

EMB_TEST(view_composition_sequence_and_beforeinput_gating)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);
    (void)run(v, "window.bi = []; document.addEventListener('beforeinput', function (e) { "
                 "var s = e.inputType + ':' + e.data + ':' + e.isComposing; bi.push(s); }, true); "
                 "document.getElementById('a').focus(); log = []; true");

    emb_text_editing te = {.size = sizeof te, .text = "ni", .cursor = 2, .sel_len = 0};
    emb_view_text_editing(v, &te);
    te.text = "nih";
    te.cursor = 3;
    emb_view_text_editing(v, &te);
    te.text = "";
    te.cursor = 0;
    emb_view_text_editing(v, &te); /* canceled: compositionend only */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"),
                     "compositionstart:a,beforeinput:a,compositionupdate:a,beforeinput:a,compositionupdate:a,compositionend:a");
    EMB_CHECK_EQ_STR(run(v, "bi.join('|')"), "insertCompositionText:ni:true|insertCompositionText:nih:true");

    /* A committed composition: insertCompositionText (isComposing true) before compositionend, no insertText. */
    (void)run(v, "bi = []; log = []; true");
    te.text = "ni";
    te.cursor = 2;
    emb_view_text_editing(v, &te);
    (void)run(v, "log = []; bi = []; true");
    emb_view_text_input(v, "你", 3); /* 你 */
    EMB_CHECK_EQ_STR(run(v, "log.join(',')"), "beforeinput:a,compositionend:a");
    EMB_CHECK_EQ_STR(run(v, "bi.join('|')"), "insertCompositionText:你:true");

    /* Text with no composition is an insertText with isComposing false. */
    (void)run(v, "bi = []; true");
    emb_view_text_input(v, "x", 1);
    EMB_CHECK_EQ_STR(run(v, "bi.join('|')"), "insertText:x:false");

    /* Without a text control focused, text input reaches no beforeinput (the target is the body). */
    (void)run(v, "document.activeElement.blur(); bi = []; true");
    emb_view_text_input(v, "x", 1);
    EMB_CHECK_EQ_STR(run(v, "bi.length"), "0");
    close_page(v, g, rt);
}

EMB_TEST(view_right_press_and_touch_tap_focus)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g);
    /* A touch on the input (below the two boxes) focuses it: touches have no compatibility mouse events, so the focus
     * comes from the tap itself. */
    emb_view_touch(v, 7, 10.0f, 205.0f, 0);
    emb_view_touch(v, 7, 10.0f, 205.0f, 2);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "a");
    close_page(v, g, rt);
}

EMB_TEST(view_state_is_freed_with_the_view)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    size_t baseline = 0;
    for (int round = 0; round < 3; round++) {
        embi_gpu *g = nullptr;
        emb_view *v = nullptr;
        OPEN_OR_SKIP(v, rt, g);
        EMB_CHECK(strcmp(run(v, k_logger), "true") == 0);
        emb_view_mouse_move(v, 10.0f, 10.0f, 0);
        emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(v, 10.0f, 10.0f, EMB_BUTTON_PRIMARY, false, 0);
        emb_view_set_navigate_handler(v, nullptr, nullptr);
        emb_view_destroy(v); /* the runtime lives on across the rounds: only the view and its device go */
        embi_gpu_destroy(g);
        if (round == 0) {
            baseline = embi_mem_live(); /* the first cycle warms up the process-wide tables */
        } else {
            EMB_CHECK(embi_mem_live() == baseline);
        }
    }
    emb_runtime_destroy(rt);
}

/* The GPU tests use the video subsystem, which is initialized once around the run (as the other GPU tests do). */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
