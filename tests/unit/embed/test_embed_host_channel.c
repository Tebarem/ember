/*
 * HX1: the host channel of a view (emb_view_set_host_channel, emb_view_post_host_message) and the SDL events of an
 * offscreen host (emb_view_sdl_event). The views need a GPU device: without one the tests skip.
 */
#include "emb_test.h"

/* The SDL event header first, then the public headers: ember_input.h must accept either order. */
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

#include <ember/ember_input.h>
#include <ember/ember_script.h>

#include "embed/embed_int.h"
#include "embed/script_int.h"

#include <math.h>
#include <string.h>

/* The messages a host channel received, copied (up to 24). */
typedef struct rec {
    char *msg[24];
    size_t len[24];
    int n;
} rec;

static void rec_add(void *user, const char *m, size_t len)
{
    rec *r = user;
    if (r->n < 24) {
        r->msg[r->n] = SDL_malloc(len + 1);
        memcpy(r->msg[r->n], m, len);
        r->msg[r->n][len] = '\0';
        r->len[r->n] = len;
        r->n++;
    }
}

static void rec_clear(rec *r)
{
    for (int i = 0; i < r->n; i++) {
        SDL_free(r->msg[i]);
    }
    r->n = 0;
}

/* Whether message i of r is exactly the len bytes at exp. */
static bool rec_is(const rec *r, int i, const char *exp, size_t len)
{
    return i < r->n && r->len[i] == len && memcmp(r->msg[i], exp, len) == 0;
}

/* The result text of the last run() call. */
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

/* Runs js in the view and returns its completion text (or "!throw"). */
static const char *run(emb_view *v, const char *js)
{
    g_res[0] = '\0';
    g_ok = false;
    (void)emb_view_eval(v, js, strlen(js), on_result, nullptr);
    return g_ok ? g_res : "!throw";
}

/* The first error line the script layer logged (an uncaught exception). */
static char g_log[512];

static void log_cb(void *user, emb_log_level level, const char *text)
{
    (void)user;
    if (level == EMB_LOG_ERROR && g_log[0] == '\0') {
        SDL_strlcpy(g_log, text, sizeof g_log);
    }
}

/* A 300x200 offscreen view on the device. Returns nullptr when the device is missing. */
static emb_view *open_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = 300;
    d.height = 200;
    d.scale = 1.0f;
    d.device = dev;
    return emb_view_create(rt, &d);
}

static void load(emb_view *v, const char *html)
{
    EMB_REQUIRE(emb_view_load_html(v, html, strlen(html), "http://127.0.0.1/hx1.html"));
    (void)embi_view_update(v); /* the load's tasks and the layout */
}

static const char k_blank[] = "<!doctype html><body style=\"margin:0\">";
static const char k_script_page[] = "<!doctype html><body><script>window.first = typeof host;</script>";

/* Opens the view and the runtime for a test; skips the test when there is no device. Leaves g and rt set. */
#define OPEN_OR_SKIP(v, rt, g)                                                                             \
    do {                                                                                                   \
        (rt) = emb_runtime_create(nullptr);                                                                \
        EMB_REQUIRE((rt) != nullptr);                                                                      \
        (g) = embi_gpu_create(nullptr, false);                                                             \
        if ((g) == nullptr) {                                                                              \
            emb_test_cur_skip = "no GPU device";                                                           \
            emb_runtime_destroy(rt);                                                                       \
            return;                                                                                        \
        }                                                                                                  \
        (v) = open_view((rt), embi_gpu_device(g));                                                         \
        EMB_REQUIRE((v) != nullptr);                                                                       \
    } while (0)

static void close_all(emb_view *v, embi_gpu *g, emb_runtime *rt)
{
    emb_view_destroy(v);
    embi_gpu_destroy(g);
    emb_runtime_destroy(rt);
}

EMB_TEST(host_post_message_bytes_and_type_error)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    rec r = {0};
    EMB_REQUIRE(emb_view_set_host_channel(v, "host", rec_add, &r));
    load(v, k_blank);
    EMB_CHECK_EQ_STR(run(v, "typeof host"), "object");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('ASCII'); 'ok'"), "ok");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('h\\u00e9llo \\u2713'); 'ok'"), "ok");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage(42); 'ok'"), "ok");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage(JSON.stringify({a: 'q\"uote', b: 'back\\\\slash'})); 'ok'"), "ok");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('x'.repeat(2 * 1024 * 1024)); 'ok'"), "ok");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('r') === undefined"), "true");
    EMB_REQUIRE(r.n == 6);
    EMB_CHECK(rec_is(&r, 0, "ASCII", 5));
    EMB_CHECK(rec_is(&r, 1, "h\xc3\xa9llo \xe2\x9c\x93", 10));
    EMB_CHECK(rec_is(&r, 2, "42", 2));
    const char json[] = "{\"a\":\"q\\\"uote\",\"b\":\"back\\\\slash\"}";
    EMB_CHECK(rec_is(&r, 3, json, strlen(json)));
    EMB_CHECK_EQ_INT((int)r.len[4], 2 * 1024 * 1024);
    bool all_x = r.n > 4 && r.len[4] == 2u * 1024u * 1024u;
    for (size_t i = 0; all_x && i < r.len[4]; i++) {
        all_x = r.msg[4][i] == 'x';
    }
    EMB_CHECK(all_x);
    EMB_CHECK(rec_is(&r, 5, "r", 1));
    EMB_CHECK_EQ_STR(run(v, "try { host.postMessage(); 'no' } catch (e) { e instanceof TypeError ? 'TypeError' : 'other' }"),
                     "TypeError");

    /* A null receiver keeps the object but drops the messages. */
    EMB_CHECK(emb_view_set_host_channel(v, "host", nullptr, nullptr));
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('dropped'); 'ok'"), "ok");
    EMB_CHECK_EQ_INT(r.n, 6);

    rec_clear(&r);
    close_all(v, g, rt);
}

EMB_TEST(host_on_message_order_drop_and_exception)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    EMB_REQUIRE(emb_view_set_host_channel(v, "host", nullptr, nullptr));
    load(v, k_blank);
    embi_script_set_log(embi_doc_script(v->doc), log_cb, nullptr);

    /* A message posted before onMessage is set is dropped. */
    EMB_CHECK(emb_view_post_host_message(v, "early", 5));
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "window.got = []; host.onMessage = function (m) { got.push(m); }; typeof host.onMessage"),
                     "function");
    EMB_CHECK(emb_view_post_host_message(v, "a", 1));
    EMB_CHECK(emb_view_post_host_message(v, "b", 1));
    EMB_CHECK(emb_view_post_host_message(v, "c", 1));
    EMB_CHECK_EQ_STR(run(v, "got.length"), "0"); /* nothing before the next update */
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "got.join(',')"), "a,b,c");

    /* An exception in onMessage is logged, and the next message is still delivered. */
    EMB_CHECK_EQ_STR(run(v, "host.onMessage = function (m) { if (m === 'boom') throw new Error('bad'); got.push(m); }; 'ok'"),
                     "ok");
    EMB_CHECK(emb_view_post_host_message(v, "boom", 4));
    EMB_CHECK(emb_view_post_host_message(v, "after", 5));
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "got.join(',')"), "a,b,c,after");
    EMB_CHECK(SDL_strstr(g_log, "uncaught exception:") != nullptr);

    EMB_CHECK(!emb_view_post_host_message(nullptr, "x", 1));
    EMB_CHECK(!emb_view_post_host_message(v, nullptr, 0));
    close_all(v, g, rt);
}

static int post_from_thread(void *arg)
{
    emb_view *v = arg;
    for (int i = 0; i < 5; i++) {
        char b[8];
        int n = SDL_snprintf(b, sizeof b, "t%d", i);
        (void)emb_view_post_host_message(v, b, (size_t)n);
    }
    return 0;
}

EMB_TEST(host_post_from_another_thread_is_delivered_in_order)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    EMB_REQUIRE(emb_view_set_host_channel(v, "host", nullptr, nullptr));
    load(v, k_blank);
    EMB_CHECK_EQ_STR(run(v, "window.got = []; host.onMessage = function (m) { got.push(m); }; 'ok'"), "ok");
    SDL_Thread *th = SDL_CreateThread(post_from_thread, "hx1-post", v);
    EMB_REQUIRE(th != nullptr);
    SDL_WaitThread(th, nullptr);
    (void)embi_view_update(v);
    EMB_CHECK_EQ_STR(run(v, "got.join(',')"), "t0,t1,t2,t3,t4");
    close_all(v, g, rt);
}

EMB_TEST(host_channel_per_document_and_global_property)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    rec r = {0};
    EMB_REQUIRE(emb_view_set_host_channel(v, "host", rec_add, &r));
    load(v, k_script_page);
    /* The channel exists in the document whose script already ran. */
    EMB_CHECK_EQ_STR(run(v, "window.first"), "object");
    EMB_CHECK_EQ_STR(run(v, "var d = Object.getOwnPropertyDescriptor(globalThis, 'host'); "
                            "[d.writable, d.enumerable, d.configurable].join()"),
                     "false,false,false");
    EMB_CHECK_EQ_STR(run(v, "delete globalThis.host"), "false");
    EMB_CHECK_EQ_STR(run(v, "host = 5; typeof host"), "object");
    EMB_CHECK_EQ_STR(run(v, "typeof host.postMessage + ' ' + host.onMessage"), "function null");

    /* A document loaded later gets the channel again. */
    load(v, k_blank);
    EMB_CHECK_EQ_STR(run(v, "typeof host"), "object");
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('second'); 'ok'"), "ok");
    EMB_REQUIRE(r.n == 1);
    EMB_CHECK(rec_is(&r, 0, "second", 6));

    /* A bad name returns false and changes nothing: the channel still works. */
    EMB_CHECK(!emb_view_set_host_channel(v, "1abc", rec_add, &r));
    EMB_CHECK(!emb_view_set_host_channel(v, "a-b", rec_add, &r));
    EMB_CHECK(!emb_view_set_host_channel(v, "", rec_add, &r));
    EMB_CHECK(!emb_view_set_host_channel(v, nullptr, rec_add, &r));
    EMB_CHECK(!emb_view_set_host_channel(nullptr, "host", rec_add, &r));
    char longname[65];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    EMB_CHECK(!emb_view_set_host_channel(v, longname, rec_add, &r));
    EMB_CHECK_EQ_STR(run(v, "host.postMessage('still'); 'ok'"), "ok");
    EMB_CHECK_EQ_INT(r.n, 2);
    rec_clear(&r);

    /* A view whose realm exists and that has no channel: none before the set, and the set installs it at once. */
    emb_view *w = open_view(rt, embi_gpu_device(g));
    EMB_REQUIRE(w != nullptr);
    load(w, k_script_page);
    EMB_CHECK_EQ_STR(run(w, "typeof host"), "undefined");
    rec r2 = {0};
    char name63[64];
    memset(name63, 'L', sizeof name63 - 1);
    name63[sizeof name63 - 1] = '\0';
    EMB_CHECK(emb_view_set_host_channel(w, "late", rec_add, &r2));
    EMB_CHECK_EQ_STR(run(w, "typeof late"), "object");
    EMB_CHECK_EQ_STR(run(w, "late.postMessage('now'); 'ok'"), "ok");
    EMB_REQUIRE(r2.n == 1);
    EMB_CHECK(rec_is(&r2, 0, "now", 3));
    EMB_CHECK(emb_view_set_host_channel(w, name63, rec_add, &r2)); /* 63 bytes is the longest name */
    rec_clear(&r2);
    emb_view_destroy(w);

    rec_clear(&r);
    close_all(v, g, rt);
}

EMB_TEST(host_sdl_events_dispatch_to_the_view)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    /* #o at the top left (0..100), the scroll box #s below it (100..200), the input below both. */
    static const char k_events_page[] =
        "<!doctype html><body style=\"margin:0\">"
        "<div id=o style=\"width:100px;height:100px;background:#eee\"></div>"
        "<div id=s style=\"width:100px;height:100px;overflow:auto\"><div style=\"height:1000px\"></div></div>"
        "<input id=a style=\"display:block;width:100px\">";
    load(v, k_events_page);
    EMB_CHECK_EQ_STR(run(v, "window.log = []; "
                            "['keydown', 'keyup'].forEach(function (t) { document.addEventListener(t, function (e) { "
                            "  log.push(t + ' ' + e.key + ' ' + e.code + ' ' + e.shiftKey); }, true); }); "
                            "document.addEventListener('mousemove', function (e) { "
                            "  log.push('move ' + e.clientX + ',' + e.clientY); }, true); "
                            "document.addEventListener('mousedown', function (e) { "
                            "  log.push('down ' + e.clientX + ',' + e.clientY + ' ' + e.button); }, true); 'ok'"),
                     "ok");

    SDL_Event e;
    memset(&e, 0, sizeof e);

    /* Key down and up with Shift: the DOM key on the layout, the code and shiftKey. */
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.down = true;
    e.key.scancode = SDL_SCANCODE_A;
    e.key.key = SDLK_A;
    e.key.mod = SDL_KMOD_SHIFT;
    EMB_CHECK(emb_view_sdl_event(v, &e, 0.0f, 0.0f, 1.0f));
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    EMB_CHECK(emb_view_sdl_event(v, &e, 0.0f, 0.0f, 1.0f));
    EMB_CHECK_EQ_STR(run(v, "log.join('|')"), "keydown A KeyA true|keyup A KeyA true");

    /* Pointer positions map with the origin (100, 50) and scale 0.5: event (180, 110) is CSS (40, 30). */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 180.0f;
    e.motion.y = 110.0f;
    EMB_CHECK(emb_view_sdl_event(v, &e, 100.0f, 50.0f, 0.5f));
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.clicks = 1;
    e.button.x = 180.0f;
    e.button.y = 110.0f;
    EMB_CHECK(emb_view_sdl_event(v, &e, 100.0f, 50.0f, 0.5f));
    EMB_CHECK_EQ_STR(run(v, "log.slice(2).join('|')"), "move 40,30|down 40,30 0");

    /* The wheel over the scroll box (CSS (50, 150)) scrolls it by one notch. */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = -1.0f;
    e.wheel.mouse_x = 200.0f;
    e.wheel.mouse_y = 350.0f;
    EMB_CHECK(emb_view_sdl_event(v, &e, 100.0f, 50.0f, 0.5f));
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('s').scrollTop"), "53");

    /* Text input inserts into the focused input. */
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('a').focus(); 'ok'"), "ok");
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = "hi";
    EMB_CHECK(emb_view_sdl_event(v, &e, 0.0f, 0.0f, 1.0f));
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('a').value"), "hi");

    /* Events that are not of these kinds, and null arguments, are not dispatched. */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_JOYSTICK_ADDED;
    EMB_CHECK(!emb_view_sdl_event(v, &e, 0.0f, 0.0f, 1.0f));
    EMB_CHECK(!emb_view_sdl_event(nullptr, &e, 0.0f, 0.0f, 1.0f));
    EMB_CHECK(!emb_view_sdl_event(v, nullptr, 0.0f, 0.0f, 1.0f));
    EMB_CHECK(!emb_view_sdl_event(v, &e, 0.0f, 0.0f, 0.0f));
    EMB_CHECK(!emb_view_sdl_event(v, &e, 0.0f, 0.0f, -1.0f));
    EMB_CHECK(!emb_view_sdl_event(v, &e, 0.0f, 0.0f, NAN));

    close_all(v, g, rt);
}

/* Host messages that rebuild text between frames, as the Falcon editor's do (rename rows, remove and re-create them,
 * toggle hidden, add a style sheet), and then read geometry (to place a menu). The read lays the document out from the
 * script, inside the view's update, where embi_view_layout does not see it. The change is still painted. When that layout
 * freed shaped runs (the tree built again: the rows were cleared, a style sheet came) the list on screen names freed runs:
 * the frame must not compare it (it crashed in embi_tshim_content_hash) and redraws everything. A layout that freed nothing
 * (a renamed row) is compared as usual. After every frame the list on screen belongs to the current runs. The big row is
 * over 1 MB of text on one line, so a read of its freed bytes faults as well. */
EMB_TEST(host_messages_that_rebuild_text_and_read_geometry_between_frames)
{
    emb_view *v = nullptr;
    embi_gpu *g = nullptr;
    emb_runtime *rt = nullptr;
    OPEN_OR_SKIP(v, rt, g);

    EMB_REQUIRE(emb_view_set_host_channel(v, "host", nullptr, nullptr));
    static const char k_rows_page[] =
        "<!doctype html><body style=\"margin:0;font:13px monospace\">"
        "<div id=big style=\"white-space:pre\"></div><ul id=list></ul><div id=menu hidden>Menu</div>"
        "<script>"
        "var list = document.getElementById('list'), big = document.getElementById('big');"
        "var menu = document.getElementById('menu');"
        "function rows(tag, n) {"
        "  while (list.firstChild) list.removeChild(list.firstChild);"
        "  for (var i = 0; i < n; i++) { var li = document.createElement('li'); li.textContent = tag + ' ' + i;"
        "    list.appendChild(li); } }"
        "host.onMessage = function (m) {"
        "  if (m === 'big') { big.textContent = 'abcdefghijklmnopqrstuvwxyz'.repeat(60000); rows('entity', 150); }"
        "  else if (m === 'rename') { big.textContent = 'small'; for (var c = list.firstChild; c; c = c.nextSibling)"
        "    c.firstChild.data = 'renamed ' + c.firstChild.data; }"
        "  else if (m === 'rebuild') { rows('row', 7); menu.hidden = !menu.hidden; }"
        "  else if (m === 'sheet') { big.textContent = 'sheet'; var st = document.createElement('style');"
        "    st.textContent = 'li { color: #333 }'; document.head.appendChild(st); }"
        "  else if (m === 'clear') { big.textContent = ''; rows('x', 0); menu.textContent = 'Closed'; }"
        "  else if (m === 'one') { list.lastChild.firstChild.data = 'one'; }"
        "  else if (m === 'tint') { document.body.style.color = document.body.style.color === 'red' ? 'blue' : 'red'; return; }"
        "  window.placed = menu.getBoundingClientRect().top + list.offsetHeight; };"
        "</script>";
    load(v, k_rows_page);
    (void)embi_view_update(v);
    /* tint changes only a color and reads nothing: its frame compares the list on screen with the next one (and damages
     * nothing when no text is left). */
    static const char *const k_msgs[] = {"big", "tint", "clear", "tint", "big", "tint", "sheet", "tint", "rename", "rebuild",
                                         "tint", "rename", "one"};
    uint32_t swept = 0, compared = 0;
    for (size_t i = 0; i < sizeof k_msgs / sizeof k_msgs[0]; i++) {
        const uint64_t runs0 = embi_doc_run_epoch(v->doc);
        EMB_CHECK(emb_view_post_host_message(v, k_msgs[i], strlen(k_msgs[i])));
        bool changed = embi_view_update(v);
        const bool sweep = embi_doc_run_epoch(v->doc) != runs0;
        if (strcmp(k_msgs[i], "tint") != 0) {
            EMB_CHECK(changed); /* the change is painted */
        }
        if (sweep) {
            EMB_CHECK(changed && v->damage.full); /* runs were freed: the list on screen was not compared */
            swept++;
        } else if (changed && !v->damage.full) {
            compared++;
        }
        /* The list on screen is the one built at the document's current layout and runs. */
        EMB_CHECK(v->have_prev);
        EMB_CHECK(v->prev_runs == embi_doc_run_epoch(v->doc));
        EMB_CHECK(v->prev_serial == embi_doc_layout_serial(v->doc));
    }
    EMB_CHECK(swept >= 2);
    EMB_CHECK(compared >= 2);
    EMB_CHECK_EQ_STR(run(v, "list.children.length + ' ' + list.firstChild.textContent + ' ' + list.lastChild.textContent"),
                     "7 renamed row 0 one");
    close_all(v, g, rt);
}

/* The GPU device needs the video subsystem, initialized once around the run (as the other GPU tests do). */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
