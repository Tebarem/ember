/*
 * IntersectionObserver (W6d) against a laid-out document: entries before and after a scroll (the frame's scroll offset),
 * thresholds, rootMargin, an element root, a root removed while the observer lives, the queued entries dropped by
 * unobserve, a lost delivery task that is scheduled again, and the allocation count at the end of each test.
 */
#include "observer_harness.h"

#include "layout/box.h"
#include "layout/fragment.h"

/* The target sits at y 700..800 under a 700px spacer, in a 1024x768 viewport: 68 of its 100 rows are visible at scroll 0. */
static const char io_spacer_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=spacer></div><div id=t></div>"
    "</body></html>";

static const char io_spacer_css[] = "#spacer{height:700px} #t{height:100px;width:200px}";

/* Entries as numbers: [isIntersecting, intersectionRatio, boundingClientRect.y, intersectionRect.height]. */
static const char io_log_js[] =
    "var log = []; var io = new IntersectionObserver(function (es) {"
    "  for (var i = 0; i < es.length; i++) {"
    "    log.push([es[i].isIntersecting, es[i].intersectionRatio, es[i].boundingClientRect.y, es[i].intersectionRect.height]);"
    "  } }, {threshold: [0, 0.5, 1]});"
    "io.observe(document.getElementById('t')); true";

/* Fake timers: the scheduled delivery tasks are kept and fired by the test. */
typedef struct io_timers {
    void *payloads[8];
    uint32_t n;
} io_timers;

static uint32_t io_timer_add(void *user, double timeout_ms, bool repeat, void *payload)
{
    io_timers *t = user;
    (void)timeout_ms;
    (void)repeat;
    if (t->n >= 8) {
        return 0;
    }
    t->payloads[t->n] = payload;
    return ++t->n;
}

static void *io_timer_clear(void *user, uint32_t id)
{
    (void)user;
    (void)id;
    return nullptr;
}

EMB_TEST(io_entries_follow_layout_and_scroll)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    EMB_CHECK(ob_js_true(&e, io_log_js));
    ob_frame(&e);
    /* the first observation is always reported: 68 rows of 100 are visible, between thresholds 0 and 0.5 */
    EMB_CHECK(ob_js_true(&e, "log.length === 1 && log[0][0] === true && Math.abs(log[0][1] - 0.68) < 1e-9 &&"
                             "log[0][2] === 700 && log[0][3] === 68"));
    /* an unchanged frame reports nothing */
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 1"));
    /* scrolled to 100: the target is fully visible, so the ratio crosses the last threshold */
    e.c->scroll_y = 100.0;
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 2 && log[1][0] === true && log[1][1] === 1 && log[1][2] === 600 &&"
                             "log[1][3] === 100"));
    /* scrolled past the target: not intersecting, ratio 0 */
    e.c->scroll_y = 900.0;
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 3 && log[2][0] === false && log[2][1] === 0 && log[2][3] === 0"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_top_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=t></div>"
    "</body></html>";

static const char io_top_css[] = "#t{height:100px;width:200px}";

EMB_TEST(io_root_margin_shrinks_the_root_rectangle)
{
    ob_env e;
    ob_env_init(&e, io_top_html, io_top_css);
    /* the root's top edge moves 200px down: the target at 0..100 is outside it */
    EMB_CHECK(ob_js_true(&e, "var got = null; var io = new IntersectionObserver(function (es) { got = es[0]; },"
                             "{rootMargin: '-200px 0px 0px 0px'});"
                             "io.rootMargin === '-200px 0px 0px 0px' && io.observe(document.getElementById('t')) === undefined"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "got !== null && got.isIntersecting === false && got.intersectionRatio === 0 &&"
                             "got.rootBounds.y === 200 && got.rootBounds.height === 568 && got.rootBounds.width === 1024"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_percent_root_margin_scales_with_the_root_rectangle)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    /* 10% of the 768px root height is 76.8px: the bottom edge moves to 844.8, so the target (700..800) is fully inside */
    EMB_CHECK(ob_js_true(&e, "var got = null; var io = new IntersectionObserver(function (es) { got = es[0]; },"
                             "{rootMargin: '0px 0px 10% 0px', threshold: 1});"
                             "io.rootMargin === '0px 0px 10% 0px' && io.observe(document.getElementById('t')) === undefined"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "got !== null && got.isIntersecting === true && got.intersectionRatio === 1 &&"
                             "got.rootBounds.y === 0 && Math.abs(got.rootBounds.height - 844.8) < 1e-6 &&"
                             "got.rootBounds.width === 1024"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_unobserve_keeps_the_order_of_the_other_entries)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    io_timers tm = {{0}, 0};
    embi_bind_timers hooks = {&tm, io_timer_add, io_timer_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(ob_js_true(&e, "var io = new IntersectionObserver(function () {});"
                             "var t = document.getElementById('t'), sp = document.getElementById('spacer');"
                             "io.observe(t); io.observe(document.body); io.observe(sp); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "io.unobserve(document.body); var r = io.takeRecords();"
                             "r.length === 2 && r[0].target === t && r[1].target === sp"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    for (uint32_t i = 0; i < tm.n; i++) {
        embi_bind_release_timer(e.c, tm.payloads[i]);
    }
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_root_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=rt></div><div id=far></div>"
    "</body></html>";

static const char io_root_css[] = "#rt{width:300px;height:300px} #far{width:200px;height:100px;margin-top:100px}";

EMB_TEST(io_element_root_clips_to_the_root_box)
{
    ob_env e;
    ob_env_init(&e, io_root_html, io_root_css);
    /* the target starts 100px below the root's bottom edge (the root is 0..300, the target 400..500) */
    EMB_CHECK(ob_js_true(&e, "var got = null; var rt = document.getElementById('rt');"
                             "var io = new IntersectionObserver(function (es) { got = es[0]; }, {root: rt});"
                             "io.root === rt && io.observe(document.getElementById('far')) === undefined"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "got !== null && got.isIntersecting === false && got.rootBounds.width === 300 &&"
                             "got.rootBounds.height === 300 && got.target === document.getElementById('far')"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_root_removed_while_observing_survives_a_collection)
{
    ob_env e;
    ob_env_init(&e, io_root_html, io_root_css);
    /* The observer keeps the root's wrapper (its ref array holds the root), so the root node stays valid after the script
     * drops its own references, the root is removed from the document and a collection runs. */
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var got = null;"
                             "(function () {"
                             "  var rt = document.getElementById('rt');"
                             "  var io = new IntersectionObserver(function (es) { calls++; got = es[0]; }, {root: rt});"
                             "  io.observe(document.getElementById('far'));"
                             "})();"
                             "document.body.removeChild(document.getElementById('rt')); true"));
    embi_js_gc(e.rt);
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 1"));
    EMB_CHECK(ob_js_true(&e, "got !== null && got.isIntersecting === false"));
    EMB_CHECK(ob_js_true(&e, "got.target === document.getElementById('far')"));
    embi_js_gc(e.rt);
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 1"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_threshold_list_is_kept_in_order)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    EMB_CHECK(ob_js_true(&e, "var io = new IntersectionObserver(function () {}, {threshold: [1, 0.25, 0.75, 0]});"
                             "io.thresholds.length === 4 && io.thresholds[0] === 0 && io.thresholds[1] === 0.25 &&"
                             "io.thresholds[2] === 0.75 && io.thresholds[3] === 1"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_unobserve_drops_the_queued_entries_of_the_target)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    io_timers tm = {{0}, 0};
    embi_bind_timers hooks = {&tm, io_timer_add, io_timer_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var io = new IntersectionObserver(function () { calls++; });"
                             "io.observe(document.getElementById('t')); true"));
    ob_frame(&e);
    /* delivery is a task: nothing is called yet, and one task is scheduled */
    EMB_CHECK(ob_js_true(&e, "calls === 0"));
    EMB_CHECK_EQ_INT(tm.n, 1);
    EMB_CHECK(ob_js_true(&e, "io.unobserve(document.getElementById('t')); true"));
    embi_bind_fire_timer(e.c, tm.payloads[0]);
    embi_bind_release_timer(e.c, tm.payloads[0]);
    EMB_CHECK(ob_js_true(&e, "calls === 0"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_lost_delivery_task_is_scheduled_again_by_the_next_frame)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    io_timers tm = {{0}, 0};
    embi_bind_timers hooks = {&tm, io_timer_add, io_timer_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var got = 0; var io = new IntersectionObserver(function (es) {"
                             "  calls++; got += es.length; }); io.observe(document.getElementById('t')); true"));
    ob_frame(&e);
    EMB_CHECK_EQ_INT(tm.n, 1);
    /* the loop drops the first task without running it: the next frame sees it still marked and schedules another */
    ob_frame(&e);
    EMB_CHECK_EQ_INT(tm.n, 2);
    embi_bind_fire_timer(e.c, tm.payloads[1]);
    embi_bind_release_timer(e.c, tm.payloads[1]);
    embi_bind_release_timer(e.c, tm.payloads[0]);
    EMB_CHECK(ob_js_true(&e, "calls === 1 && got === 1"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_clip_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=box><div id=in></div><div id=out></div></div>"
    "</body></html>";

static const char io_clip_css[] =
    "#box{width:200px;height:100px;overflow:hidden}"
    "#in{height:50px;width:100px} #out{height:50px;width:100px;margin-top:150px}";

EMB_TEST(io_ancestor_overflow_clips_the_intersection)
{
    ob_env e;
    ob_env_init(&e, io_clip_html, io_clip_css);
    /* #out lies at y 200..250, inside the viewport but below the 100px high overflow:hidden box: it is clipped away */
    EMB_CHECK(ob_js_true(&e, "var log = {}; var io = new IntersectionObserver(function (es) {"
                             "  for (var i = 0; i < es.length; i++) log[es[i].target.id] = es[i]; });"
                             "io.observe(document.getElementById('in')); io.observe(document.getElementById('out')); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log['in'].isIntersecting === true && log['in'].intersectionRatio === 1 &&"
                             "log['out'].isIntersecting === false && log['out'].intersectionRatio === 0 &&"
                             "log['out'].intersectionRect.height === 0"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_part_css[] =
    "#box{width:200px;height:100px;overflow:hidden}"
    "#in{height:50px;width:100px} #out{height:50px;width:100px;margin-top:25px}";

EMB_TEST(io_ancestor_overflow_clips_part_of_the_target)
{
    ob_env e;
    ob_env_init(&e, io_clip_html, io_part_css);
    /* #out is at y 75..125; the box ends at 100: half of it is visible */
    EMB_CHECK(ob_js_true(&e, "var got = null; var io = new IntersectionObserver(function (es) { got = es[0]; });"
                             "io.observe(document.getElementById('out')); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "got.isIntersecting === true && Math.abs(got.intersectionRatio - 0.5) < 1e-9 &&"
                             "got.intersectionRect.height === 25 && got.boundingClientRect.height === 50"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_scroll_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=sc><div id=spacer></div><div id=t></div></div>"
    "</body></html>";

static const char io_scroll_css[] =
    "#sc{width:200px;height:100px;overflow:auto}"
    "#spacer{height:300px} #t{height:50px;width:100px}";

EMB_TEST(io_element_root_scroll_offset_changes_the_result)
{
    ob_env e;
    ob_env_init(&e, io_scroll_html, io_scroll_css);
    /* #t is at y 300..350 inside a 100px high scroll container used as the root. */
    EMB_CHECK(ob_js_true(&e, "var log = []; var sc = document.getElementById('sc');"
                             "var io = new IntersectionObserver(function (es) {"
                             "  for (var i = 0; i < es.length; i++) log.push(es[i].isIntersecting); }, {root: sc});"
                             "io.observe(document.getElementById('t')); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 1 && log[0] === false"));
    /* scroll the root by 280: the target is at 20..70 in the visible area */
    embi_element *sc = embi_get_element_by_id(e.doc, EMBI_STR("sc"));
    EMB_CHECK(sc != nullptr && sc->box != nullptr);
    embi_frag *f = ((embi_box *)sc->box)->frag;
    EMB_CHECK(f != nullptr);
    embi_layout_frag_set_scroll(e.layout, e.doc, f, 0.0f, 280.0f);
    embi_observers_after_layout(e.c);
    EMB_CHECK(ob_js_true(&e, "log.length === 2 && log[1] === true"));
    embi_layout_frag_set_scroll(e.layout, e.doc, f, 0.0f, 0.0f);
    embi_observers_after_layout(e.c);
    EMB_CHECK(ob_js_true(&e, "log.length === 3 && log[2] === false"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

static const char io_fixed_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=f></div><div id=spacer></div><div id=t></div>"
    "</body></html>";

static const char io_fixed_css[] = "body{margin:0} #f{position:fixed;top:20px;left:0;width:100px;height:50px}"
                                   "#spacer{height:3000px} #t{height:50px;width:100px}";

EMB_TEST(io_fixed_target_stays_in_the_scrolled_viewport)
{
    ob_env e;
    ob_env_init(&e, io_fixed_html, io_fixed_css);
    /* the viewport scrolled to 1000 (its offset is the root fragment's, as the view keeps it) */
    embi_frag *root = (embi_frag *)(uintptr_t)embi_layout_root(e.layout, e.doc);
    EMB_REQUIRE(root != nullptr);
    embi_layout_frag_set_scroll(e.layout, e.doc, root, 0.0f, 1000.0f);
    e.c->scroll_y = 1000.0;
    EMB_CHECK(ob_js_true(&e, "var log = []; var io = new IntersectionObserver(function (es) {"
                             "  for (var i = 0; i < es.length; i++) log.push(es[i].target.id + ':' + es[i].isIntersecting +"
                             "  ':' + es[i].boundingClientRect.y + ':' + es[i].intersectionRect.height); });"
                             "io.observe(document.getElementById('f')); io.observe(document.getElementById('t')); true"));
    embi_observers_after_layout(e.c);
    /* the fixed box is where it is painted, at 20 in the viewport; the target below the spacer is far below it */
    EMB_CHECK(ob_js_true(&e, "log.join(' ') === 'f:true:20:50 t:false:2000:0'"));
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(io_disconnect_drops_the_queued_entries)
{
    ob_env e;
    ob_env_init(&e, io_spacer_html, io_spacer_css);
    io_timers tm = {{0}, 0};
    embi_bind_timers hooks = {&tm, io_timer_add, io_timer_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var io = new IntersectionObserver(function () { calls++; });"
                             "io.observe(document.getElementById('t')); true"));
    ob_frame(&e);
    EMB_CHECK_EQ_INT(tm.n, 1);
    EMB_CHECK(ob_js_true(&e, "io.disconnect(); io.takeRecords().length === 0"));
    embi_bind_fire_timer(e.c, tm.payloads[0]);
    embi_bind_release_timer(e.c, tm.payloads[0]);
    EMB_CHECK(ob_js_true(&e, "calls === 0"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST_MAIN()
