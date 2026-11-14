/*
 * FP2a: the realm is built on a thread from the creation of the document, ahead of the first script. These tests cover what
 * the thread must not change: a script that runs first on that realm (including one that allocates enough to collect, so
 * the write barrier of the new owner thread matters), a document that never uses it (static markup, scripting off, a
 * destroy at once) frees it, and a document whose realm was dropped still gets one when a script comes later.
 */
#include "emb_test.h"

#include "embed/embed_int.h"
#include "embed/script_int.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

typedef struct env {
    emb_runtime *rt;
    embi_doc *d;
    char out[512];
    bool ok;
} env;

static void console_cb(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    env *e = user;
    size_t n = strlen(e->out);
    if (n + len + 2 < sizeof e->out) {
        memcpy(e->out + n, text, len);
        e->out[n + len] = ';';
        e->out[n + len + 1] = '\0';
    }
}

static bool fetch_cb(void *user, embi_str url, uint8_t **data, size_t *len)
{
    (void)user;
    (void)url;
    (void)data;
    (void)len;
    return false;
}

/* A runtime whose UA prefetch thread (FP2b) has finished: the shared UA sheet is allocated and held before any memory
 * baseline, so that the counters below only see the document and its realm. emb_runtime_destroy skips a joined thread. */
static emb_runtime *runtime_new(void)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    if (rt != nullptr && rt->ua_thread != nullptr) {
        SDL_WaitThread(rt->ua_thread, nullptr);
        rt->ua_thread = nullptr;
    }
    return rt;
}

static void make(env *e, bool scripting)
{
    memset(e, 0, sizeof *e);
    e->rt = runtime_new();
    embi_doc_cb cb = {e, fetch_cb, nullptr, nullptr};
    e->d = embi_doc_create(e->rt->text, e->rt->fonts, &cb);
    if (!scripting) {
        embi_doc_set_scripting(e->d, false);
    }
    embi_script_set_console(embi_doc_script(e->d), console_cb, e);
}

static void drop(env *e)
{
    embi_doc_destroy(e->d);
    emb_runtime_destroy(e->rt);
}

static void load(env *e, const char *html)
{
    (void)embi_doc_load(e->d, (embi_str){html, (uint32_t)strlen(html)}, (embi_str){"http://t.test/page.html", 23}, nullptr);
}

/* Gives a thread that was started and then dropped time to finish, and the document the chance to reap it. */
static void settle(env *e)
{
    /* Until the pump has collected the thread and the realm is gone (bounded: 10 s), so that memory counters are stable. */
    for (int i = 0; i < 10000 && embi_script_prewarm_held(embi_doc_script(e->d)); i++) {
        (void)embi_script_pump(embi_doc_script(e->d));
        SDL_Delay(1);
    }
    EMB_CHECK(!embi_script_prewarm_held(embi_doc_script(e->d)));
}

static void eval_result(void *user, bool ok, const char *text, size_t len)
{
    (void)text;
    (void)len;
    ((env *)user)->ok = ok;
}

EMB_TEST(first_script_uses_the_prebuilt_realm)
{
    env e;
    make(&e, true);
    load(&e, "<script>console.log(typeof document.createElement + ':' + typeof Image + ':' + (window === globalThis))</script>");
    EMB_CHECK_EQ_STR(e.out, "function:function:true;");
    EMB_CHECK(embi_script_bind_if(embi_doc_script(e.d)) != nullptr);
    drop(&e);
}

EMB_TEST(prebuilt_realm_collects_correctly_on_the_main_thread)
{
    env e;
    make(&e, true);
    /* Many cells that point at younger cells, across collections: a missing write barrier corrupts or crashes this. */
    load(&e,
         "<body><script>var keep = []; for (var i = 0; i < 60000; i++) { var o = {a: i, b: [i, {c: i}]}; if (i % 7 == 0) keep.push(o); }"
         "var s = 0; for (var j = 0; j < keep.length; j++) s += keep[j].b[1].c - keep[j].a; "
         "document.body.appendChild(document.createElement('div')); console.log(keep.length + ':' + s)</script>");
    EMB_CHECK_EQ_STR(e.out, "8572:0;");
    drop(&e);
}

EMB_TEST(realm_survives_a_reload_and_a_later_script)
{
    env e;
    make(&e, true);
    load(&e, "<script>console.log('one')</script>");
    load(&e, "<script>console.log('two')</script>");
    EMB_CHECK_EQ_STR(e.out, "one;two;");
    drop(&e);
}

EMB_TEST(static_markup_lets_the_realm_go_and_a_later_eval_makes_one)
{
    env e;
    make(&e, true);
    load(&e, "<p>no script here</p>");
    settle(&e);
    EMB_CHECK(embi_script_bind_if(embi_doc_script(e.d)) == nullptr);
    EMB_CHECK(embi_script_eval(embi_doc_script(e.d), "console.log(1 + 1)", 18, eval_result, &e));
    EMB_CHECK(e.ok);
    EMB_CHECK_EQ_STR(e.out, "2;");
    drop(&e);
}

EMB_TEST(script_tag_in_any_case_keeps_the_realm)
{
    env e;
    make(&e, true);
    load(&e, "<SCRIPT>console.log('upper')</SCRIPT>");
    EMB_CHECK_EQ_STR(e.out, "upper;");
    drop(&e);
}

EMB_TEST(unused_realm_is_freed_by_the_pump_and_by_destroy)
{
    /* The realm is a few MB: a static document that dropped it holds far less than one that runs a script. */
    env a;
    size_t base_a = 0;
    {
        emb_runtime *rt = runtime_new();
        base_a = embi_mem_live();
        memset(&a, 0, sizeof a);
        a.rt = rt;
        embi_doc_cb cb = {&a, fetch_cb, nullptr, nullptr};
        a.d = embi_doc_create(rt->text, rt->fonts, &cb);
        load(&a, "<p>static</p>");
        settle(&a);
    }
    size_t static_cost = embi_mem_live() - base_a;
    embi_doc_destroy(a.d);
    a.d = nullptr;
    emb_runtime_destroy(a.rt);

    env b;
    size_t base_b = 0;
    {
        emb_runtime *rt = runtime_new();
        base_b = embi_mem_live();
        memset(&b, 0, sizeof b);
        b.rt = rt;
        embi_doc_cb cb = {&b, fetch_cb, nullptr, nullptr};
        b.d = embi_doc_create(rt->text, rt->fonts, &cb);
        embi_script_set_console(embi_doc_script(b.d), console_cb, &b);
        load(&b, "<script>console.log('x')</script>");
    }
    size_t script_cost = embi_mem_live() - base_b;
    EMB_CHECK(script_cost > static_cost + 1024 * 1024);
    drop(&b);
}

static size_t doc_cost_after_wait(bool with_script)
{
    emb_runtime *rt = runtime_new();
    size_t base = embi_mem_live();
    env e;
    memset(&e, 0, sizeof e);
    e.rt = rt;
    embi_doc_cb cb = {&e, fetch_cb, nullptr, nullptr};
    e.d = embi_doc_create(rt->text, rt->fonts, &cb);
    embi_script_set_console(embi_doc_script(e.d), console_cb, &e);
    SDL_Delay(300); /* the realm thread has long finished when the markup arrives */
    load(&e, with_script ? "<script>console.log('x')</script>" : "<p>static</p>");
    settle(&e);
    size_t cost = embi_mem_live() - base;
    embi_doc_destroy(e.d);
    emb_runtime_destroy(rt);
    return cost;
}

EMB_TEST(realm_finished_before_the_markup_is_freed_off_the_main_thread)
{
    /* A free thread takes the finished realm, the pump only collects it: settle() returns once nothing is held. */
    size_t with = doc_cost_after_wait(true);
    size_t without = doc_cost_after_wait(false);
    EMB_CHECK(with > without + 1024 * 1024);
}

EMB_TEST(scripting_off_drops_the_realm_and_destroy_at_once_is_clean)
{
    env e;
    size_t base = 0;
    {
        emb_runtime *rt = runtime_new();
        base = embi_mem_live();
        emb_runtime_destroy(rt);
    }
    (void)base;
    make(&e, false);
    load(&e, "<script>console.log('never')</script><p>x</p>");
    settle(&e);
    EMB_CHECK_EQ_STR(e.out, "");
    EMB_CHECK(embi_script_bind_if(embi_doc_script(e.d)) == nullptr);
    drop(&e);

    /* Created and destroyed with the thread possibly still building: joined, nothing leaks. */
    emb_runtime *rt = runtime_new();
    size_t before = embi_mem_live();
    embi_doc_cb cb = {nullptr, fetch_cb, nullptr, nullptr};
    embi_doc *d = embi_doc_create(rt->text, rt->fonts, &cb);
    embi_doc_destroy(d);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
    emb_runtime_destroy(rt);
}

EMB_TEST_MAIN()
