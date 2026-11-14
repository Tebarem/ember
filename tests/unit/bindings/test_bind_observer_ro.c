/*
 * ResizeObserver (W6d) against a laid-out document: the entries and their sizes, the target that is not reported (no box,
 * or 0x0), the depth-ordered loop (a callback that resizes an intermediate target), the loop-limit error event, and the
 * allocation count at the end of each test.
 */
#include "observer_harness.h"

static const char ro_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=s><div id=m><div id=d>x</div></div></div>"
    "<div id=a></div><div id=b></div><div id=c></div><div id=e></div>"
    "</body></html>";

static const char ro_css[] =
    "#s{width:300px;height:200px}"
    "#m{width:200px;height:100px}"
    "#d{width:100px;height:50px}"
    "#a{width:100px;height:50px}"
    "#b{display:none}"
    "#c{width:0px;height:0px}"
    "#e{width:200px;height:20px;padding:5px;border:2px solid}";

/* Records errors reported at the window (the ResizeObserver loop error is one of them). */
static const char errors_js[] =
    "var errs = []; window.addEventListener('error', function (ev) { errs.push(ev.message); }); true";

EMB_TEST(ro_reports_the_first_observation_of_boxless_and_zero_size_targets)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var ro = new ResizeObserver(function () { calls++; });"
                             "ro.observe(document.getElementById('b')); ro.observe(document.getElementById('c')); true"));
    ob_frame(&e);
    ob_frame(&e);
    /* the first observation is always reported, for a target without a box and for a 0 x 0 target too (WPT notify.html) */
    EMB_CHECK(ob_js_true(&e, "calls === 1"));
    /* a change of size after the observation is reported */
    EMB_CHECK(ob_js_true(&e, "document.getElementById('c').style.width = '10px'; true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 2"));
    EMB_CHECK(ob_js_true(&e, "ro.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_reports_a_sized_target_once_per_change)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, "var log = []; var a = document.getElementById('a');"
                             "var ro = new ResizeObserver(function (es) { for (var i = 0; i < es.length; i++) log.push(es[i]); });"
                             "ro.observe(a); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 1 && log[0].target === a && log[0].contentRect.width === 100 &&"
                             "log[0].contentRect.height === 50 && log[0].contentRect.x === 0 &&"
                             "log[0].borderBoxSize[0].inlineSize === 100 && log[0].borderBoxSize[0].blockSize === 50 &&"
                             "log[0].contentBoxSize[0].inlineSize === 100 && log[0].contentBoxSize[0].blockSize === 50 &&"
                             "log[0].devicePixelContentBoxSize[0].inlineSize === 100"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 1"));
    EMB_CHECK(ob_js_true(&e, "a.style.width = '60px'; true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 2 && log[1].contentRect.width === 60"));
    EMB_CHECK(ob_js_true(&e, "ro.unobserve(a); true"));
    EMB_CHECK(ob_js_true(&e, "a.style.width = '70px'; true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.length === 2"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_content_and_border_box_sizes_and_the_box_option)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    /* #e is 200x20 with 5px padding and 2px border: the border box is 214x34, the content box 200x20. */
    EMB_CHECK(ob_js_true(&e, "var r1 = null; var r2 = null; var el = document.getElementById('e');"
                             "var ro1 = new ResizeObserver(function (es) { r1 = es[0]; });"
                             "var ro2 = new ResizeObserver(function (es) { r2 = es[0]; });"
                             "ro1.observe(el, {box: 'content-box'}); ro2.observe(el, {box: 'border-box'}); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "r1.contentBoxSize[0].inlineSize === 200 && r1.contentBoxSize[0].blockSize === 20 &&"
                             "r1.borderBoxSize[0].inlineSize === 214 && r1.borderBoxSize[0].blockSize === 34 &&"
                             "r1.contentRect.x === 5 && r1.contentRect.y === 5"));
    EMB_CHECK(ob_js_true(&e, "r2.borderBoxSize[0].inlineSize === 214 && r2.borderBoxSize[0].blockSize === 34"));
    /* box is a stored option: re-observing with another box resets the last reported size, so the next frame reports. */
    EMB_CHECK(ob_js_true(&e, "r1 = null; ro1.observe(el, {box: 'border-box'}); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "r1 !== null && r1.borderBoxSize[0].inlineSize === 214"));
    EMB_CHECK(ob_js_true(&e, "ro1.disconnect(); ro2.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_loop_delivers_a_shallower_than_deeper_target_changed_by_a_callback)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    /* s (shallow), m (between) and d (deep) are observed. The first pass delivers all three; the callback resizes m,
     * which lies deeper than the shallowest delivered target, so the second pass delivers it. */
    EMB_CHECK(ob_js_true(&e, errors_js));
    EMB_CHECK(ob_js_true(&e, "var sizes = []; var m = document.getElementById('m');"
                             "var ro = new ResizeObserver(function (es) {"
                             "  sizes.push(es.length);"
                             "  if (sizes.length === 1) { m.style.width = '50px'; } });"
                             "ro.observe(document.getElementById('s')); ro.observe(m);"
                             "ro.observe(document.getElementById('d')); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "sizes.length === 2 && sizes[0] === 3 && sizes[1] === 1 && errs.length === 0"));
    EMB_CHECK(ob_js_true(&e, "ro.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_change_to_a_shallower_target_reports_the_loop_error)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, errors_js));
    /* The callback resizes s, which is no deeper than the shallowest target of the first pass (s itself), so the loop
     * cannot deliver it and reports the undelivered notification as an error event. */
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var s = document.getElementById('s');"
                             "var ro = new ResizeObserver(function () {"
                             "  calls++; if (calls === 1) { s.style.width = '250px'; } });"
                             "ro.observe(s); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 1 && errs.length === 1 &&"
                             "errs[0] === 'ResizeObserver loop completed with undelivered notifications.'"));
    EMB_CHECK(ob_js_true(&e, "ro.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_disconnect_stops_delivery_and_releases_the_targets)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, "var calls = 0; var a = document.getElementById('a');"
                             "var ro = new ResizeObserver(function () { calls++; }); ro.observe(a); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 1 && (ro.disconnect(), true) && (a.style.width = '80px', true)"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "calls === 1"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_reobserve_with_the_same_box_reports_again_and_device_pixels_are_whole)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, "var n = 0; var a = document.getElementById('a');"
                             "var ro = new ResizeObserver(function (es) { n++; });"
                             "ro.observe(a); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "n === 1"));
    EMB_CHECK(ob_js_true(&e, "ro.observe(a); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "n === 2"));
    EMB_CHECK(ob_js_true(&e, "ro.disconnect(); true"));
    /* device-pixel-content-box at dpr 2: the 100x50 content box is 200x100 device pixels (whole numbers) */
    e.dpr = 2.0;
    EMB_CHECK(ob_js_true(&e, "var dp = null; var ro2 = new ResizeObserver(function (es) { dp = es[0]; });"
                             "ro2.observe(a, {box: 'device-pixel-content-box'}); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "dp !== null && dp.devicePixelContentBoxSize[0].inlineSize === 200 &&"
                             "dp.devicePixelContentBoxSize[0].blockSize === 100"));
    EMB_CHECK(ob_js_true(&e, "ro2.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST(ro_unobserve_keeps_the_observation_order)
{
    ob_env e;
    ob_env_init(&e, ro_html, ro_css);
    EMB_CHECK(ob_js_true(&e, "var log = []; var ids = ['a', 'c', 'e', 's'].map(function (i) { return document.getElementById(i); });"
                             "var ro = new ResizeObserver(function (es) { for (var i = 0; i < es.length; i++) log.push(es[i].target.id); });"
                             "ids[0].style.width = '10px'; ids[1].style.width = '10px'; ids[1].style.height = '10px';"
                             "ids.forEach(function (n) { ro.observe(n); }); ro.unobserve(ids[0]); true"));
    ob_frame(&e);
    EMB_CHECK(ob_js_true(&e, "log.join(',') === 'c,e,s'"));
    EMB_CHECK(ob_js_true(&e, "ro.disconnect(); true"));
    EMB_CHECK_EQ_INT(ob_env_done(&e), 0);
}

EMB_TEST_MAIN()
