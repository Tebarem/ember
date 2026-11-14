/*
 * The scripts of a document (W5c): classic inline and external scripts in document order, defer and async (order, and
 * their run when the element is gone), errors that do not stop later scripts, timers on a fake clock (setInterval,
 * clearInterval from inside the callback, extra arguments, string handlers, the nesting clamp), microtasks before timers,
 * eval, the readyState / DOMContentLoaded / load order and event objects, document.currentScript, performance, reportError,
 * and the type / language / nomodule filtering of script elements. The document is made without a view, so no GPU is
 * needed: the console and log output is captured through the script layer's own hooks.
 */
#include "emb_test.h"

#include "base/clock.h"
#include "embed/embed_int.h"
#include "embed/script_int.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct buf {
    char text[2048];
    size_t len;
} buf;

/* Appends s followed by ';' so tests can compare whole sequences. */
static void buf_add(buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 < sizeof b->text) {
        memcpy(b->text + b->len, s, n);
        b->len += n;
        b->text[b->len++] = ';';
        b->text[b->len] = '\0';
    }
}

typedef struct env {
    emb_runtime *rt;
    embi_doc *d;
    buf out;   /* console output */
    buf err;   /* script log lines (errors, console.error) */
    buf eval;  /* the last emb_view_eval-style result */
    bool eval_ok;
} env;

static void console_cb(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    buf_add(&((env *)user)->out, text, len);
}

static void log_cb(void *user, emb_log_level level, const char *text)
{
    (void)level;
    buf_add(&((env *)user)->err, text, strlen(text));
}

static bool ends_with(embi_str s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.len >= n && memcmp(s.ptr + s.len - n, suffix, n) == 0;
}

/* The loader of the test document: the scripts below, by the end of their URL. */
static const struct {
    const char *name;
    const char *body;
} k_files[] = {
    {"/ext.js", "console.log('ext')"},
    {"/defer.js", "console.log('defer')"},
    {"/async.js", "console.log('async')"},
    {"/d1.js", "console.log('d1')"},
    {"/d2.js", "console.log('d2')"},
    {"/a1.js", "console.log('a1')"},
    {"/a2.js", "console.log('a2')"},
    {"/cur.js", "console.log(document.currentScript === null ? 'null' : document.currentScript.tagName)"},
    {"/rs.js", "console.log('defer:' + document.readyState)"},
    {"/late.js", "console.log('defer:' + document.readyState + ':' + (document.getElementById('late') !== null))"},
};

static bool fetch_cb(void *user, embi_str url, uint8_t **data, size_t *len)
{
    (void)user;
    const char *body = nullptr;
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; i++) {
        if (ends_with(url, k_files[i].name)) {
            body = k_files[i].body;
        }
    }
    if (body == nullptr) {
        return false;
    }
    size_t n = strlen(body);
    uint8_t *copy = SDL_malloc(n);
    SDL_memcpy(copy, body, n);
    *data = copy;
    *len = n;
    return true;
}

static void setup(env *e)
{
    memset(e, 0, sizeof *e);
    e->rt = emb_runtime_create(nullptr);
    embi_doc_cb cb = {e, fetch_cb, nullptr, nullptr};
    e->d = embi_doc_create(e->rt->text, e->rt->fonts, &cb);
    embi_script_set_console(embi_doc_script(e->d), console_cb, e);
    embi_script_set_log(embi_doc_script(e->d), log_cb, e);
}

static void teardown(env *e)
{
    embi_doc_destroy(e->d);
    emb_runtime_destroy(e->rt);
}

static void load(env *e, const char *html)
{
    (void)embi_doc_load(e->d, (embi_str){html, (uint32_t)strlen(html)}, (embi_str){"http://t.test/page.html", 23},
                        nullptr);
}

static bool pump(env *e)
{
    return embi_script_pump(embi_doc_script(e->d));
}

static void eval_result(void *user, bool ok, const char *text, size_t len)
{
    env *e = user;
    e->eval_ok = ok;
    e->eval.len = 0;
    if (len < sizeof e->eval.text) {
        memcpy(e->eval.text, text, len);
        e->eval.len = len;
        e->eval.text[len] = '\0';
    }
}

/* Evaluates src in the document's realm; the result text is in e->eval.text. */
static bool run_js(env *e, const char *src)
{
    return embi_script_eval(embi_doc_script(e->d), src, strlen(src), eval_result, e);
}

static void use_fake_clock(void)
{
    embi_clock_set_fake(true, 1000000000ULL);
}

static void ms(uint64_t n)
{
    embi_clock_advance_fake(n * 1000000ULL);
}

/* ------------------------------------------------------------------ order, errors, loading */

EMB_TEST(scripts_inline_run_in_document_order)
{
    env e;
    setup(&e);
    load(&e, "<script>console.log('a')</script><p>x</p><script>console.log('b')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "a;b;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

EMB_TEST(exception_in_one_script_does_not_stop_the_next)
{
    env e;
    setup(&e);
    load(&e, "<script>throw new Error('boom')</script><script>console.log('after')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "after;");
    /* The error is logged with the contract's prefix, with its message and location. */
    EMB_CHECK(SDL_strstr(e.err.text, "uncaught exception:") != nullptr);
    EMB_CHECK(SDL_strstr(e.err.text, "boom") != nullptr);
    EMB_CHECK(SDL_strstr(e.err.text, "inline script") != nullptr);
    teardown(&e);
}

EMB_TEST(external_script_loads_through_the_loader_in_order)
{
    env e;
    setup(&e);
    load(&e, "<script src='ext.js'></script><script>console.log('inline')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "ext;inline;");
    teardown(&e);
}

EMB_TEST(missing_external_script_warns_and_the_page_goes_on)
{
    env e;
    setup(&e);
    load(&e, "<script src='nothere.js'></script><script>console.log('next')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "next;");
    teardown(&e);
}

EMB_TEST(defer_runs_after_the_parse_and_async_as_a_task)
{
    env e;
    setup(&e);
    load(&e, "<script defer src='defer.js'></script><script>console.log('inline')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "inline;defer;");
    teardown(&e);

    setup(&e);
    load(&e, "<script async src='async.js'></script><script>console.log('sync')</script>");
    /* The async script does not block the parser: the inline script after it ran first. */
    EMB_CHECK_EQ_STR(e.out.text, "sync;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "sync;async;");
    teardown(&e);
}

EMB_TEST(several_defer_scripts_run_in_order_before_dom_content_loaded)
{
    env e;
    setup(&e);
    load(&e,
         "<script>document.addEventListener('DOMContentLoaded', function (ev) {"
         "  console.log('dcl:' + ev.type + ':' + (ev.target === document) + ':' + document.readyState);"
         "});</script>"
         "<script defer src='d1.js'></script><script>console.log('inline')</script><script defer src='d2.js'></script>");
    /* Both deferred scripts ran at the end of the parse, in document order, and DOMContentLoaded waits for the loop. */
    EMB_CHECK_EQ_STR(e.out.text, "inline;d1;d2;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "inline;d1;d2;dcl:DOMContentLoaded:true:interactive;");
    teardown(&e);
}

EMB_TEST(several_async_scripts_run_in_queue_order_and_before_load)
{
    env e;
    setup(&e);
    load(&e,
         "<script async src='a1.js'></script><script>console.log('mid');"
         "window.addEventListener('load', function () { console.log('load'); });</script>"
         "<script async src='a2.js'></script>");
    EMB_CHECK_EQ_STR(e.out.text, "mid;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "mid;a1;a2;load;");
    teardown(&e);
}

/* The element of a queued script is gone when its turn comes: the run uses the URL copied at queue time. */
EMB_TEST(defer_and_async_scripts_run_after_their_elements_are_removed)
{
    env e;
    setup(&e);
    load(&e,
         "<body><script defer src='defer.js'></script><script async src='async.js'></script>"
         "<script>document.body.innerHTML = ''; console.log('cleared');</script></body>");
    /* The deferred script ran at the end of the parse; the async one waits for the loop. */
    EMB_CHECK_EQ_STR(e.out.text, "cleared;defer;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "cleared;defer;async;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

EMB_TEST(an_inline_script_clearing_the_body_before_dom_content_loaded)
{
    env e;
    setup(&e);
    load(&e,
         "<body><script defer src='cur.js'></script><script>"
         "document.addEventListener('DOMContentLoaded', function () { console.log('dcl'); });"
         "document.body.innerHTML = '';</script></body>");
    /* The deferred script still sees its (removed) element as document.currentScript. */
    EMB_CHECK_EQ_STR(e.out.text, "SCRIPT;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "SCRIPT;dcl;");
    teardown(&e);
}

EMB_TEST(a_script_that_replaces_its_own_document_content_is_safe)
{
    env e;
    setup(&e);
    load(&e, "<body><script id=s>document.body.innerHTML = ''; console.log(document.currentScript.id);</script></body>");
    EMB_CHECK_EQ_STR(e.out.text, "s;");
    teardown(&e);
}

/* ------------------------------------------------------------------ timers */

EMB_TEST(timers_use_the_fake_clock_and_run_in_due_order)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>setTimeout(()=>console.log('t10'),10); setTimeout(()=>console.log('t0'));</script>");
    EMB_CHECK_EQ_STR(e.out.text, "");
    /* The zero timeout is due now; the ten millisecond one is not. */
    EMB_CHECK(pump(&e));
    EMB_CHECK_EQ_STR(e.out.text, "t0;");
    EMB_CHECK(!pump(&e));
    ms(10);
    EMB_CHECK(pump(&e));
    EMB_CHECK_EQ_STR(e.out.text, "t0;t10;");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(microtasks_run_before_timers)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>setTimeout(()=>console.log('timer'),0); Promise.resolve().then(()=>console.log('micro'));</script>");
    /* The checkpoint after the script runs the microtask; the timer waits for the loop. */
    EMB_CHECK_EQ_STR(e.out.text, "micro;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "micro;timer;");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(clear_timeout_cancels_the_timer)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var id = setTimeout(()=>console.log('no'),0); clearTimeout(id); console.log('done');</script>");
    EMB_CHECK_EQ_STR(e.out.text, "done;");
    /* The first pump runs the lifecycle tasks the script queued (they have no listeners); the cleared timer does not run. */
    (void)pump(&e);
    EMB_CHECK(!pump(&e));
    EMB_CHECK_EQ_STR(e.out.text, "done;");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(set_timeout_passes_extra_arguments)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>setTimeout(function (a, b, c) { console.log(a + ':' + b + ':' + c); }, 0, 'a', 1);</script>");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "a:1:undefined;");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(set_timeout_evaluates_a_string_handler_in_the_global_scope)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var g = 'global'; setTimeout(\"console.log('str:' + g)\", 0);</script>");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "str:global;");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(set_interval_repeats_and_clear_interval_from_its_callback_stops_it)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e,
         "<script>var n = 0; var id = setInterval(function () {"
         "  n++; console.log('i' + n); if (n === 3) { clearInterval(id); }"
         "}, 5);</script>");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "");
    for (int i = 0; i < 6; i++) {
        ms(5);
        (void)pump(&e);
    }
    EMB_CHECK_EQ_STR(e.out.text, "i1;i2;i3;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(timer_ids_are_distinct_and_clear_of_a_stale_id_is_harmless)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var a = setTimeout(function () {}, 1), b = setTimeout(function () {}, 1);"
             "console.log((a !== b) + ':' + (a > 0) + ':' + (b > 0)); clearTimeout(99999); clearTimeout(a); clearTimeout(a);</script>");
    EMB_CHECK_EQ_STR(e.out.text, "true:true:true;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(nesting_clamp_through_javascript_timeouts)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var n = 1; function f() { n++; if (n < 20) { setTimeout(f, 0); } } setTimeout(f, 0);</script>");
    /* Levels 1 to 5 run one per pump with the clock standing still. */
    for (int i = 1; i <= 5; i++) {
        (void)pump(&e);
        EMB_CHECK(run_js(&e, "n"));
        char want[8];
        SDL_snprintf(want, sizeof want, "%d", 1 + i);
        EMB_CHECK_EQ_STR(e.eval.text, want);
    }
    /* The sixth is clamped to 4 ms. */
    (void)pump(&e);
    EMB_CHECK(run_js(&e, "n"));
    EMB_CHECK_EQ_STR(e.eval.text, "6");
    ms(3);
    (void)pump(&e);
    EMB_CHECK(run_js(&e, "n"));
    EMB_CHECK_EQ_STR(e.eval.text, "6");
    ms(1);
    (void)pump(&e);
    EMB_CHECK(run_js(&e, "n"));
    EMB_CHECK_EQ_STR(e.eval.text, "7");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(set_interval_zero_is_clamped_after_five_runs)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var c = 0; var id = setInterval(function () { c++; if (c === 8) { clearInterval(id); } }, 0);</script>");
    for (int i = 1; i <= 5; i++) {
        (void)pump(&e);
    }
    EMB_CHECK(run_js(&e, "c"));
    EMB_CHECK_EQ_STR(e.eval.text, "5");
    (void)pump(&e); /* the same instant: the sixth run is 4 ms away */
    EMB_CHECK(run_js(&e, "c"));
    EMB_CHECK_EQ_STR(e.eval.text, "5");
    ms(4);
    (void)pump(&e);
    ms(4);
    (void)pump(&e);
    ms(4);
    (void)pump(&e);
    EMB_CHECK(run_js(&e, "c"));
    EMB_CHECK_EQ_STR(e.eval.text, "8");
    ms(100);
    (void)pump(&e);
    EMB_CHECK(run_js(&e, "c"));
    EMB_CHECK_EQ_STR(e.eval.text, "8");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(queue_microtask_of_a_non_callable_throws_a_type_error_at_once)
{
    env e;
    setup(&e);
    load(&e, "<script>try { queueMicrotask(1); console.log('no throw'); } catch (x) { console.log(x.name); }"
             "queueMicrotask(function () { console.log('micro'); }); console.log('sync');</script>");
    EMB_CHECK_EQ_STR(e.out.text, "TypeError;sync;micro;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

/* ------------------------------------------------------------------ eval, static pages, events */

EMB_TEST(eval_returns_the_completion_value_or_the_error)
{
    env e;
    setup(&e);
    load(&e, "<p>no script here</p>");
    /* A static page has no realm until the first eval, and the eval makes it. */
    EMB_CHECK(run_js(&e, "1 + 2"));
    EMB_CHECK(e.eval_ok);
    EMB_CHECK_EQ_STR(e.eval.text, "3");
    EMB_CHECK(!run_js(&e, "throw new Error('e')"));
    EMB_CHECK(!e.eval_ok);
    EMB_CHECK(SDL_strstr(e.eval.text, "Error: e") != nullptr);
    EMB_CHECK_EQ_STR(e.out.text, "");
    teardown(&e);
}

EMB_TEST(noscript_content_is_not_built_when_scripts_are_on_and_is_when_off)
{
    env e;
    setup(&e);
    load(&e, "<body><noscript><p id=n>x</p></noscript><script>1</script>");
    EMB_CHECK(run_js(&e, "String(document.getElementById('n'))"));
    EMB_CHECK_EQ_STR(e.eval.text, "null");
    embi_doc_set_scripting(e.d, false);
    load(&e, "<body><noscript><p id=n>x</p></noscript>");
    EMB_CHECK(run_js(&e, "String(document.getElementById('n'))"));
    EMB_CHECK(strcmp(e.eval.text, "null") != 0);
    teardown(&e);
}

EMB_TEST(static_document_runs_no_script_and_queues_no_events)
{
    env e;
    setup(&e);
    load(&e, "<p>hello</p><style>p{color:red}</style>");
    EMB_CHECK(!pump(&e));
    EMB_CHECK_EQ_STR(e.out.text, "");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    /* A static page is complete at once. */
    EMB_CHECK(run_js(&e, "document.readyState"));
    EMB_CHECK_EQ_STR(e.eval.text, "complete");
    teardown(&e);
}

EMB_TEST(dom_content_loaded_then_load_with_their_event_objects)
{
    env e;
    setup(&e);
    load(&e,
         "<script>"
         "document.addEventListener('DOMContentLoaded', function (ev) {"
         "  console.log('dcl:' + ev.type + ':' + (ev.target === document) + ':' + ev.bubbles + ':' + ev.isTrusted);"
         "});"
         "window.addEventListener('DOMContentLoaded', function (ev) {"
         "  console.log('win-dcl:' + ev.type + ':' + (ev.currentTarget === window) + ':' + (ev.target === document));"
         "});"
         "window.addEventListener('load', function (ev) {"
         "  console.log('load:' + ev.type + ':' + (ev.currentTarget === window) + ':' + ev.bubbles + ':' + ev.isTrusted);"
         "});</script>");
    EMB_CHECK_EQ_STR(e.out.text, "");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text,
                     "dcl:DOMContentLoaded:true:true:true;win-dcl:DOMContentLoaded:true:true;load:load:true:false:true;");
    teardown(&e);
}

EMB_TEST(ready_state_goes_loading_interactive_complete_with_readystatechange)
{
    env e;
    setup(&e);
    load(&e,
         "<script>console.log(document.readyState);"
         "document.addEventListener('readystatechange', function (ev) {"
         "  console.log('rsc:' + ev.type + ':' + document.readyState); });"
         "document.addEventListener('DOMContentLoaded', function () { console.log('dcl:' + document.readyState); });"
         "window.addEventListener('load', function () { console.log('load:' + document.readyState); });</script>"
         "<script defer src='rs.js'></script>");
    /* The parse ended: interactive before the deferred script ran. */
    EMB_CHECK_EQ_STR(e.out.text, "loading;rsc:readystatechange:interactive;defer:interactive;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text,
                     "loading;rsc:readystatechange:interactive;defer:interactive;dcl:interactive;"
                     "rsc:readystatechange:complete;load:complete;");
    EMB_CHECK(run_js(&e, "document.readyState"));
    EMB_CHECK_EQ_STR(e.eval.text, "complete");
    teardown(&e);
}

/* ------------------------------------------------------------------ document.currentScript */

EMB_TEST(current_script_is_the_running_classic_script_and_null_otherwise)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e,
         "<script id=s1>console.log(document.currentScript.id + ':' + document.currentScript.tagName);</script>"
         "<script src='cur.js'></script>"
         "<script defer src='cur.js'></script>"
         "<script>setTimeout(function () { console.log('timer:' + document.currentScript); }, 0);</script>");
    EMB_CHECK_EQ_STR(e.out.text, "s1:SCRIPT;SCRIPT;SCRIPT;");
    (void)pump(&e);
    EMB_CHECK_EQ_STR(e.out.text, "s1:SCRIPT;SCRIPT;SCRIPT;timer:null;");
    EMB_CHECK(run_js(&e, "String(document.currentScript)"));
    EMB_CHECK_EQ_STR(e.eval.text, "null");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

/* ------------------------------------------------------------------ performance */

EMB_TEST(performance_now_follows_the_clock_from_the_document_origin)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var t0 = performance.now(); console.log(typeof t0 + ':' + t0);</script>");
    EMB_CHECK_EQ_STR(e.out.text, "number:0;");
    ms(10);
    EMB_CHECK(run_js(&e, "performance.now() - t0"));
    EMB_CHECK_EQ_STR(e.eval.text, "10");
    EMB_CHECK(run_js(&e, "performance === window.performance"));
    EMB_CHECK_EQ_STR(e.eval.text, "true");
    /* timeOrigin is a Unix time in ms: well after 2001. */
    EMB_CHECK(run_js(&e, "performance.timeOrigin > 1e12"));
    EMB_CHECK_EQ_STR(e.eval.text, "true");
    /* The origin moves with the document: a new document starts again at 0. */
    ms(5);
    load(&e, "<script>console.log(performance.now());</script>");
    EMB_CHECK(SDL_strstr(e.out.text, "0;") != nullptr);
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(performance_mark_and_measure)
{
    use_fake_clock();
    env e;
    setup(&e);
    load(&e, "<script>var m = performance.mark('a'); console.log(m.name + ':' + m.entryType + ':' + m.startTime);</script>");
    EMB_CHECK_EQ_STR(e.out.text, "a:mark:0;");
    ms(7);
    EMB_CHECK(run_js(&e, "performance.mark('b'); var d = performance.measure('ab', 'a', 'b'); d.entryType + ':' + d.duration"));
    EMB_CHECK_EQ_STR(e.eval.text, "measure:7");
    EMB_CHECK(run_js(&e, "var d2 = performance.measure('since-a', 'a'); d2.duration"));
    EMB_CHECK_EQ_STR(e.eval.text, "7");
    EMB_CHECK(!run_js(&e, "performance.measure('x', 'nope')"));
    EMB_CHECK(SDL_strstr(e.eval.text, "SyntaxError") != nullptr);
    /* clearMarks removes the mark, so a measure from it fails. */
    EMB_CHECK(run_js(&e, "performance.clearMarks('a'); try { performance.measure('y', 'a'); 'no'; } catch (x) { x.name; }"));
    EMB_CHECK_EQ_STR(e.eval.text, "SyntaxError");
    EMB_CHECK(run_js(&e, "performance.clearMarks(); performance.clearMeasures(); 'ok'"));
    EMB_CHECK_EQ_STR(e.eval.text, "ok");
    teardown(&e);
    embi_clock_set_fake(false, 0);
}

/* ------------------------------------------------------------------ error reporting */

EMB_TEST(report_error_dispatches_an_error_event_or_logs)
{
    env e;
    setup(&e);
    load(&e,
         "<script>window.addEventListener('error', function (ev) {"
         "  console.log('ev:' + ev.message + ':' + (ev.error instanceof Error) + ':' + ev.cancelable);"
         "  ev.preventDefault(); });"
         "reportError(new Error('boom'));</script>");
    EMB_CHECK_EQ_STR(e.out.text, "ev:Error: boom:true:true;");
    /* A listener cancelled the event, so nothing is logged. */
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);

    setup(&e);
    load(&e, "<script>reportError(new Error('plain'));</script><script>console.log('after')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "after;");
    EMB_CHECK(SDL_strstr(e.err.text, "uncaught exception: Error: plain") != nullptr);
    teardown(&e);
}

EMB_TEST(thrown_errors_reach_window_error_listeners_with_their_location)
{
    env e;
    setup(&e);
    load(&e,
         "<script>window.addEventListener('error', function (ev) {"
         "  console.log('caught:' + ev.message + ':' + ev.filename + ':' + ev.lineno); ev.preventDefault(); });</script>"
         "<script>throw new Error('x');</script>");
    EMB_CHECK_EQ_STR(e.out.text, "caught:Error: x:inline script:1;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

EMB_TEST(module_and_non_javascript_scripts_are_skipped)
{
    env e;
    setup(&e);
    load(&e, "<script type='module'>console.log('m')</script><script type='text/template'>console.log('t')</script>");
    /* W7d: the inline module is deferred (it runs once parsing is over); the template is data and is not run. */
    EMB_CHECK_EQ_STR(e.out.text, "m;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

EMB_TEST(inline_module_is_deferred_and_ordered_with_defer_scripts)
{
    env e;
    setup(&e);
    load(&e,
         "<script defer src='late.js'></script>"
         "<script type='module'>console.log('module:' + document.readyState + ':' + "
         "(document.getElementById('late') !== null));</script>"
         "<script>console.log('classic:' + (document.getElementById('late') !== null));</script>"
         "<div id='late'></div>");
    /* The classic script runs at parse time and does not see the later element; the deferred external script and the
     * inline module run in document order after parsing, in the interactive state, and both see it. */
    EMB_CHECK_EQ_STR(e.out.text, "classic:false;defer:interactive:true;module:interactive:true;");
    EMB_CHECK_EQ_INT((int)e.err.len, 0);
    teardown(&e);
}

EMB_TEST(unhandled_rejection_is_logged_with_its_prefix)
{
    env e;
    setup(&e);
    load(&e, "<script>Promise.reject(new Error('nobody'));</script><script>console.log('next')</script>");
    /* The rejection is reported when the job drain after the script ends; the next script still runs. */
    EMB_CHECK_EQ_STR(e.out.text, "next;");
    EMB_CHECK(SDL_strstr(e.err.text, "unhandled rejection:") != nullptr);
    teardown(&e);
}

EMB_TEST(console_error_goes_to_the_log_with_its_prefix)
{
    env e;
    setup(&e);
    load(&e, "<script>console.error('bad')</script>");
    EMB_CHECK_EQ_STR(e.out.text, "bad;");
    EMB_CHECK(SDL_strstr(e.err.text, "console.error: bad") != nullptr);
    teardown(&e);
}

/* ------------------------------------------------------------------ which script elements run */

EMB_TEST(script_type_and_language_follow_the_html_algorithm)
{
    static const struct {
        const char *attrs;
        bool runs;
    } cases[] = {
        {"", true},
        {"type='text/javascript'", true},
        {"type='TEXT/JavaScript'", true},
        {"type='application/javascript; charset=utf-8'", true},
        {"type=''", true},
        {"type='text/javascript' language='foo'", true}, /* a non-empty type wins over language */
        {"type='application/x-javascript' language='vbscript'", true},
        {"language=''", true}, /* present but empty */
        {"language='JavaScript'", true},
        {"language='JScript'", true},
        {"language='ecmascript'", true},
        {"language='LiveScript'", true},
        {"language='javascript1.2'", true},
        {"type='' language='JScript'", true},
        {"language='foo'", false},
        {"language='vbscript'", false},
        {"type='' language='foo'", false}, /* an empty type does not win */
        {"type='text/foo'", false},
        {"type='text/template'", false},
        {"type='application/json'", false},
        {"nomodule", false},
        {"type='text/javascript' nomodule", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        env e;
        setup(&e);
        char html[256];
        SDL_snprintf(html, sizeof html, "<script %s>console.log('ran')</script>", cases[i].attrs);
        load(&e, html);
        bool ran = SDL_strcmp(e.out.text, "ran;") == 0;
        if (ran != cases[i].runs) {
            char msg[200];
            SDL_snprintf(msg, sizeof msg, "script attrs [%s]: expected %s", cases[i].attrs,
                         cases[i].runs ? "run" : "skip");
            emb_test_report_fail(__FILE__, __LINE__, msg);
        }
        teardown(&e);
    }
}

EMB_TEST(external_script_filtering_uses_the_same_rules)
{
    env e;
    setup(&e);
    load(&e, "<script src='ext.js' language='JScript'></script><script src='ext.js' language='foo'></script>"
             "<script src='ext.js' type='text/foo'></script><script src='d1.js' type='text/javascript' language='foo'></script>");
    EMB_CHECK_EQ_STR(e.out.text, "ext;d1;");
    teardown(&e);
}

/* ------------------------------------------------------------------ memory */

static void script_cycle(void)
{
    env e;
    setup(&e);
    load(&e, "<body><script>setTimeout(()=>console.log('x'),0); Promise.resolve(1).then(()=>1);"
             "var id = setInterval(function () { clearInterval(id); }, 1);"
             "performance.mark('m'); performance.measure('n', 'm');"
             "window.addEventListener('error', function (ev) { ev.preventDefault(); }); reportError(new Error('r'));</script>"
             "<script src='ext.js'></script><script defer src='defer.js'></script>"
             "<script async src='async.js'></script><script defer src='d1.js'></script>"
             "<script>document.body.innerHTML = '';</script></body>");
    (void)pump(&e);
    (void)pump(&e);
    ms(5);
    (void)pump(&e);
    teardown(&e);
}

EMB_TEST(script_document_returns_to_baseline)
{
    use_fake_clock();
    /* One cycle first: the atom table and the text system's shared tables are created once and kept. */
    script_cycle();
    size_t base = embi_mem_live();
    script_cycle();
    script_cycle();
    EMB_CHECK(embi_mem_live() == base);
    embi_clock_set_fake(false, 0);
}

EMB_TEST_MAIN()
