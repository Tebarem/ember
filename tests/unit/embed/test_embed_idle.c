/*
 * CF2: the work a view does in a frame that has nothing to do (embi_doc_idle, from embi_view_update). Once the document
 * is complete and nothing animates, the realm evaluates the animation prelude when a resolved style declared a CSS
 * transition or animation (so the first transition event does not compile it inside its frame), and it collects when
 * half its allocation budget is spent. A page without such styles never loads the prelude. The events sent after the
 * prelude was made ahead are the same as before. Test-only; skipped without a device.
 */
#include "input_rig.h"

#include "anim/frame.h"
#include "bindings/bind.h"
#include "embed/frame.h"
#include "js/runtime.h"

static embi_bind_ctx *ctx_of(emb_view *v)
{
    embi_script *s = embi_doc_script(v->doc);
    return s != nullptr ? embi_script_bind_if(s) : nullptr;
}

/* n frames with nothing to paint (each may take one idle step). */
static void idle_frames(emb_view *v, int n)
{
    for (int i = 0; i < n; i++) {
        (void)embi_view_update(v);
    }
}

static bool prelude_ready(emb_view *v)
{
    embi_bind_ctx *c = ctx_of(v);
    return c != nullptr && c->anim_exports != 0;
}

/* A transition declared on the page's elements: idle frames make the prelude before any event, and the events of a
 * transition that starts afterwards are the right TransitionEvents. */
EMB_TEST(idle_frames_make_the_animation_prelude_for_a_page_with_transitions)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g,
                      "<!doctype html><style>.t{color:rgb(0,0,0);transition:color 1s} .t.on{color:rgb(255,0,0)}</style>"
                      "<div class=t id=a>a</div><script>window.seen = [];</script>");
    EMB_CHECK(embi_animator_declared(embi_doc_animator(v->doc)));
    idle_frames(v, 4);
    EMB_CHECK(prelude_ready(v));
    (void)run(v, "for (const t of ['transitionrun', 'transitionstart']) document.addEventListener(t, function (e) {"
                 " seen.push([e.type, e.propertyName, e.elapsedTime, e instanceof TransitionEvent, e.bubbles,"
                 " e.target.id].join(':')); }); document.getElementById('a').classList.add('on'); true");
    idle_frames(v, 3);
    EMB_CHECK_EQ_STR(run(v, "seen.join(' ')"), "transitionrun:color:0:true:true:a transitionstart:color:0:true:true:a");
    rig_close(v, g, rt);
}

/* No transition or animation in the styles: the prelude is never made, however long the page idles. */
EMB_TEST(idle_frames_leave_the_prelude_alone_without_transitions)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g,
                      "<!doctype html><style>.t{color:rgb(0,0,0);transition:color 0s} .t:hover{color:red}"
                      " p{animation-name:none}</style><div class=t id=a>a</div><p>p</p>"
                      "<script>window.x = 1;</script>");
    EMB_CHECK(ctx_of(v) != nullptr);
    idle_frames(v, 10);
    (void)run(v, "document.getElementById('a').className = 't u'; true");
    idle_frames(v, 10);
    EMB_CHECK(!embi_animator_declared(embi_doc_animator(v->doc)));
    EMB_CHECK(!prelude_ready(v));
    rig_close(v, g, rt);
}

/* The declaration shows up only in a later style (a class added by a script, and a CSS animation on an element added
 * later): the animator notices it although the animation group of the first styles was already seen without one. */
EMB_TEST(a_transition_declared_later_is_noticed)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g,
                      "<!doctype html><style>.late{transition-delay:50ms} @keyframes k{to{opacity:0}}"
                      " .anim{animation:k 1s}</style><div id=a>a</div><div id=b>b</div><script>window.x = 1;</script>");
    idle_frames(v, 5);
    EMB_CHECK(!embi_animator_declared(embi_doc_animator(v->doc)));
    EMB_CHECK(!prelude_ready(v));
    (void)run(v, "document.getElementById('b').className = 'late'; true");
    idle_frames(v, 5);
    EMB_CHECK(embi_animator_declared(embi_doc_animator(v->doc)));
    EMB_CHECK(prelude_ready(v));
    rig_close(v, g, rt);

    rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    OPEN_HTML_OR_SKIP(v, rt, g,
                      "<!doctype html><style>@keyframes k{to{opacity:0}} .anim{animation:k 1s}</style><div id=a>a</div>"
                      "<script>window.x = 1;</script>");
    idle_frames(v, 5);
    EMB_CHECK(!embi_animator_declared(embi_doc_animator(v->doc)));
    (void)run(v, "const d = document.createElement('div'); d.className = 'anim'; document.body.append(d); true");
    (void)embi_view_update(v);
    EMB_CHECK(embi_animator_declared(embi_doc_animator(v->doc)));
    rig_close(v, g, rt);
}

/* Garbage made by a script is collected in idle frames once half the budget is spent, not by the next script. */
EMB_TEST(idle_frames_collect_when_half_the_budget_is_spent)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g, "<!doctype html><div id=a>a</div><script>window.x = 1;</script>");
    idle_frames(v, 5);
    embi_js_runtime *jrt = ctx_of(v)->rt;
    /* Allocate a little over half the budget (strings that die at once), without reaching it. */
    size_t want = jrt->heap.threshold / 2u + 4096u;
    while (jrt->heap.bytes_since_gc < want) {
        (void)run(v, "(function () { const s = []; for (let i = 0; i < 200; i++) s.push({i: i, t: 'x' + i}); return s.length; })()");
    }
    EMB_REQUIRE(jrt->heap.bytes_since_gc < jrt->heap.threshold);
    size_t gc0 = jrt->heap.gc_count;
    idle_frames(v, 1);
    EMB_CHECK(jrt->heap.gc_count == gc0 + 1u);
    EMB_CHECK(jrt->heap.bytes_since_gc < want);
    /* Nothing new: at most the major collection the promotions may have made due, then nothing. */
    idle_frames(v, 3);
    size_t gc1 = jrt->heap.gc_count;
    EMB_CHECK(gc1 <= gc0 + 2u);
    idle_frames(v, 10);
    EMB_CHECK(jrt->heap.gc_count == gc1);
    rig_close(v, g, rt);
}

/* A page that keeps asking for animation frames is not idle, even when its frames paint nothing: no collection runs in
 * them. Once the loop stops, the next frame with nothing to do collects. */
EMB_TEST(no_idle_work_while_animation_frames_are_requested)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g,
                      "<!doctype html><div id=a>a</div><script>window.go = true; window.n = 0;"
                      " function f() { n++; if (go) requestAnimationFrame(f); } requestAnimationFrame(f);</script>");
    idle_frames(v, 3);
    EMB_REQUIRE(embi_view_animating(v));
    embi_js_runtime *jrt = ctx_of(v)->rt;
    size_t want = jrt->heap.threshold / 2u + 4096u;
    while (jrt->heap.bytes_since_gc < want) {
        (void)run(v, "(function () { const s = []; for (let i = 0; i < 200; i++) s.push({i: i, t: 'x' + i}); return s.length; })()");
    }
    EMB_REQUIRE(jrt->heap.bytes_since_gc < jrt->heap.threshold * 3u / 4u);
    size_t gc0 = jrt->heap.gc_count;
    idle_frames(v, 3);
    EMB_CHECK(embi_view_animating(v));
    EMB_CHECK(jrt->heap.gc_count == gc0);
    (void)run(v, "go = false; true");
    idle_frames(v, 3);
    EMB_CHECK(!embi_view_animating(v));
    EMB_CHECK(jrt->heap.gc_count > gc0);
    EMB_CHECK(jrt->heap.bytes_since_gc < want);
    /* and then nothing more: the page allocates nothing */
    size_t gc1 = jrt->heap.gc_count;
    idle_frames(v, 5);
    EMB_CHECK(jrt->heap.gc_count == gc1);
    rig_close(v, g, rt);
}

RIG_MAIN()
