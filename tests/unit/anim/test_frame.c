/*
 * The animations in the frame (W6c): requestAnimationFrame in the HTML event loop order, CSS transitions and animations
 * wired into style (values at a time, events, the incremental path), the document timeline, and the idle state. The
 * document has no view: frame_rig.h runs the same steps as embi_view_update on a fake clock.
 */
#include "frame_rig.h"

#include "layout/layout.h"

#define PAGE_TRANSITION                                                                                              \
    "<style>#a{width:100px;height:50px;opacity:1;transition:opacity 100ms linear}</style><div id=a></div>"

static double num(rig *r, const char *expr)
{
    char src[256];
    SDL_snprintf(src, sizeof src, "String(%s)", expr);
    if (!rig_js(r, src)) {
        return -1e9;
    }
    return SDL_atof(r->res);
}

/* ------------------------------------------------------------------ requestAnimationFrame */

EMB_TEST(raf_runs_after_timers_and_before_style_update)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a style='width:10px'></div>");
    EMB_CHECK(rig_js(&r, "setTimeout(()=>console.log('timer'),0);"
                         "requestAnimationFrame(t=>{console.log('raf');"
                         "document.getElementById('a').style.width='80px';"
                         "console.log(document.getElementById('a').getBoundingClientRect().width)});"
                         "Promise.resolve().then(()=>console.log('micro'))"));
    rig_frames(&r, 1, 20);
    EMB_CHECK_EQ_STR(r.out.text, "micro;timer;raf;80;");
    rig_teardown(&r);
}

EMB_TEST(raf_timestamp_is_the_frame_time_and_ids_increase)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    EMB_CHECK(rig_js(&r, "var ts=[]; var i1=requestAnimationFrame(t=>ts.push(t)); var i2=requestAnimationFrame(t=>ts.push(t));"
                         "i2>i1"));
    EMB_CHECK_EQ_STR(r.res, "true");
    rig_ms(16);
    (void)rig_frame(&r);
    EMB_CHECK(rig_js(&r, "ts.length+':'+(ts[0]===ts[1])"));
    EMB_CHECK_EQ_STR(r.res, "2:true");
    EMB_CHECK(num(&r, "ts[0]") >= 16.0);
    rig_teardown(&r);
}

EMB_TEST(raf_scheduled_in_a_callback_runs_next_frame_and_cancel_works)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    EMB_CHECK(rig_js(&r, "var n=0; function f(){ n++; requestAnimationFrame(f); } requestAnimationFrame(f);"
                         "var c=requestAnimationFrame(()=>console.log('cancelled')); cancelAnimationFrame(c);"));
    rig_frames(&r, 3, 16);
    EMB_CHECK_EQ_INT((int)num(&r, "n"), 3);
    EMB_CHECK_EQ_STR(r.out.text, "");
    EMB_CHECK(embi_doc_animating(r.d));
    rig_teardown(&r);
}

EMB_TEST(raf_callback_can_cancel_a_later_one_and_errors_are_reported)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    EMB_CHECK(rig_js(&r, "var b; requestAnimationFrame(()=>{ cancelAnimationFrame(b); throw new Error('boom') });"
                         "b=requestAnimationFrame(()=>console.log('b'));"
                         "requestAnimationFrame(()=>console.log('c'))"));
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_STR(r.out.text, "c;");
    EMB_CHECK(SDL_strstr(r.err.text, "boom") != nullptr);
    rig_teardown(&r);
}

EMB_TEST(idle_page_does_not_ask_for_frames)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    rig_frames(&r, 2, 16);
    EMB_CHECK(!embi_doc_animating(r.d));
    EMB_CHECK(rig_js(&r, "requestAnimationFrame(()=>{})"));
    EMB_CHECK(embi_doc_animating(r.d));
    rig_frames(&r, 1, 16);
    EMB_CHECK(!embi_doc_animating(r.d));
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ CSS transitions */

EMB_TEST(css_transition_runs_with_events_and_no_relayout)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, PAGE_TRANSITION);
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); ['transitionrun','transitionstart','transitionend','transitioncancel']"
                         ".forEach(t=>a.addEventListener(t,e=>console.log(t+':'+e.propertyName)));"
                         "getComputedStyle(a).opacity"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "a.style.opacity='0'; getComputedStyle(a).opacity"));
    rig_frames(&r, 1, 16); /* the transition starts */
    rig_ms(50);
    bool rebuilt = rig_frame(&r);
    EMB_CHECK(!rebuilt); /* an opacity animation changes paint only */
    double mid = num(&r, "getComputedStyle(a).opacity");
    EMB_CHECK(mid > 0.2 && mid < 0.8);
    EMB_CHECK(embi_doc_animating(r.d));
    rig_frames(&r, 1, 100);
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 0);
    EMB_CHECK_EQ_STR(r.out.text, "transitionrun:opacity;transitionstart:opacity;transitionend:opacity;");
    rig_frames(&r, 1, 16);
    EMB_CHECK(!embi_doc_animating(r.d));
    rig_teardown(&r);
}

EMB_TEST(css_transition_default_timing_function_is_ease)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>#a{width:100px;height:50px;opacity:1;transition:opacity 100ms}</style><div id=a></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); getComputedStyle(a).opacity"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "a.style.opacity='0'; getComputedStyle(a).opacity"));
    rig_frames(&r, 1, 16);
    rig_ms(25);
    (void)rig_frame(&r);
    double v = num(&r, "getComputedStyle(a).opacity");
    /* 41 ms of 100 (the transition started at the previous frame time): ease is about 0.68 of the way, linear 0.41 */
    EMB_CHECK(v > 0.2 && v < 0.45);
    rig_teardown(&r);
}

EMB_TEST(css_transition_of_width_relayouts)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>#a{width:100px;height:50px;transition:width 100ms linear}</style><div id=a></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); getComputedStyle(a).width"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "a.style.width='200px'; getComputedStyle(a).width"));
    rig_frames(&r, 1, 16);
    rig_ms(50);
    (void)rig_frame(&r);
    double w = num(&r, "a.getBoundingClientRect().width");
    EMB_CHECK(w > 120.0 && w < 180.0);
    rig_teardown(&r);
}

/* Changes a child only inherits from an ancestor's animation start no transition (CSS Transitions 1, before-change style); a
 * change of what the child itself declares does, whatever the ancestor is doing. The child #c is 10em wide and transitions its
 * width over 2 s; its parent #p has a font-size of 20px. Events on #c and the #p transitions are logged in order. The engine has
 * no shadow roots or slots, so there is no shadow-tree case: the flat tree is the DOM tree. */
#define EM_FONT_TRANS "#p{font-size:20px;transition:font-size 2s linear}#p.to{font-size:30px}"
#define EM_FONT_ANIM \
    "@keyframes grow{from{font-size:20px}to{font-size:30px}}" \
    "#p{font-size:20px;animation:grow 1s linear infinite alternate}"
#define EM_FONT_ANIM_M \
    "@keyframes grow{from{font-size:20px}to{font-size:30px}}" \
    "#m{font-size:20px;animation:grow 1s linear infinite alternate}"
#define EM_CHILD "#c{width:10em;height:10px;transition:width 2s linear}#c.w{width:50px}#p.hot #c{width:70px}"
#define EM_BODY "</style><div id=p><div id=c></div></div>"

static void em_start(rig *r, const char *page)
{
    rig_setup(r);
    rig_load(r, page);
    EMB_CHECK(rig_js(r, "var p=document.getElementById('p'),c=document.getElementById('c');"
                        "['transitionrun','transitionend'].forEach(t=>{"
                        "c.addEventListener(t,e=>console.log('c:'+t+':'+e.propertyName));"
                        "p.addEventListener(t,e=>{if(e.target===p)console.log('p:'+t+':'+e.propertyName)})});"
                        "getComputedStyle(c).width"));
    rig_frames(r, 1, 16);
}

static double cw(rig *r)
{
    return num(r, "parseFloat(getComputedStyle(c).width)");
}

EMB_TEST(em_length_following_a_transitioning_font_size_starts_no_transition)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_TRANS EM_CHILD EM_BODY);
    EMB_CHECK(rig_js(&r, "p.classList.add('to'); getComputedStyle(c).width"));
    rig_frames(&r, 1, 16);
    rig_frames(&r, 1, 400);
    double mid = cw(&r);
    EMB_CHECK(mid > 200.0 && mid < 300.0); /* the child follows the font-size */
    rig_frames(&r, 1, 5000); /* one slow frame past the end of the font-size transition */
    rig_frames(&r, 1, 16);
    rig_frames(&r, 1, 3000); /* the child's would-be transition would have ended by now */
    EMB_CHECK_EQ_INT((int)cw(&r), 300);
    EMB_CHECK_EQ_STR(r.out.text, "p:transitionrun:font-size;p:transitionend:font-size;");
    rig_teardown(&r);
}

EMB_TEST(em_length_with_a_static_parent_change_still_transitions)
{
    rig r;
    em_start(&r, "<style>#p{font-size:20px}#p.to{font-size:30px}" EM_CHILD EM_BODY);
    EMB_CHECK(rig_js(&r, "p.classList.add('to'); getComputedStyle(c).width"));
    rig_frames(&r, 1, 16);
    rig_frames(&r, 1, 3000);
    EMB_CHECK_EQ_STR(r.out.text, "c:transitionrun:width;c:transitionend:width;");
    rig_teardown(&r);
}

EMB_TEST(an_ancestor_state_rule_for_the_child_transitions_while_the_ancestor_transitions)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_TRANS EM_CHILD EM_BODY);
    EMB_CHECK(rig_js(&r, "p.classList.add('to'); getComputedStyle(c).width"));
    rig_frames(&r, 2, 100);
    /* a descendant rule keyed on a class of the ancestor (what #p:hover #c is) changes the child's declared width */
    EMB_CHECK(rig_js(&r, "p.classList.add('hot'); getComputedStyle(c).width"));
    rig_frames(&r, 40, 100); /* the two transitions end in different frames, so the event order is fixed */
    EMB_CHECK_EQ_STR(r.out.text,
                     "p:transitionrun:font-size;c:transitionrun:width;p:transitionend:font-size;c:transitionend:width;");
    EMB_CHECK_EQ_INT((int)cw(&r), 70);
    rig_teardown(&r);
}

EMB_TEST(a_stylesheet_inserted_under_an_infinitely_animating_ancestor_transitions)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_ANIM EM_CHILD EM_BODY);
    rig_frames(&r, 3, 100); /* the child follows the animated font-size without a transition */
    EMB_CHECK_EQ_STR(r.out.text, "");
    EMB_CHECK(rig_js(&r, "var st=document.createElement('style'); st.textContent='#c{width:50px}';"
                         "document.head.appendChild(st); getComputedStyle(c).width"));
    rig_frames(&r, 1, 16);
    rig_frames(&r, 1, 2500);
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_STR(r.out.text, "c:transitionrun:width;c:transitionend:width;");
    EMB_CHECK_EQ_INT((int)cw(&r), 50);
    rig_teardown(&r);
}

EMB_TEST(an_ancestor_css_animation_of_font_size_starts_no_transition_on_the_child)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_ANIM EM_CHILD EM_BODY);
    rig_frames(&r, 3, 400);
    rig_frames(&r, 1, 5000);
    rig_frames(&r, 2, 700);
    double w = cw(&r);
    EMB_CHECK(w >= 200.0 && w <= 300.0);
    EMB_CHECK_EQ_STR(r.out.text, "");
    rig_teardown(&r);
}

EMB_TEST(an_unrelated_ancestor_attribute_or_class_change_mid_motion_keeps_the_em_follow_quiet)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_TRANS EM_CHILD "#p.dark{color:red}" EM_BODY);
    EMB_CHECK(rig_js(&r, "p.classList.add('to'); getComputedStyle(c).width"));
    rig_frames(&r, 2, 200);
    EMB_CHECK(rig_js(&r, "p.setAttribute('data-x','1'); p.classList.add('dark'); getComputedStyle(c).width"));
    rig_frames(&r, 1, 300);
    EMB_CHECK(rig_js(&r, "p.setAttribute('data-x','2'); getComputedStyle(c).width"));
    rig_frames(&r, 1, 5000);
    rig_frames(&r, 1, 3000);
    EMB_CHECK_EQ_STR(r.out.text, "p:transitionrun:font-size;p:transitionend:font-size;");
    EMB_CHECK_EQ_INT((int)cw(&r), 300);
    rig_teardown(&r);
}

EMB_TEST(a_running_child_transition_is_interrupted_by_its_own_change_while_the_ancestor_animates)
{
    rig r;
    em_start(&r, "<style>" EM_FONT_TRANS EM_CHILD EM_BODY);
    EMB_CHECK(rig_js(&r, "p.classList.add('to'); c.classList.add('w'); getComputedStyle(c).width"));
    rig_frames(&r, 2, 200); /* the child's width transition (to 50px) runs next to the font-size one */
    EMB_CHECK(rig_js(&r, "c.classList.remove('w'); getComputedStyle(c).width")); /* back to 10em: a reversal */
    rig_frames(&r, 50, 100);
    EMB_CHECK_EQ_STR(r.out.text, "p:transitionrun:font-size;c:transitionrun:width;c:transitionrun:width;"
                                 "p:transitionend:font-size;c:transitionend:width;");
    EMB_CHECK_EQ_INT((int)cw(&r), 300);
    rig_teardown(&r);
}

/* The WPT composition tests animate one target after another while others keep the frames going (the run that hit the use after
 * free in same_content under ASan, see hook_apply where the kept base style is replaced): an element that is animated,
 * sampled at a few times, left alone for some frames while others keep the frames swapping, and animated again (what the WPT
 * composition tests do with one target after another) must not read the values of an arena that was reset in between. With
 * the poison mode of the arenas, rig_poison counts what it would read. */
EMB_TEST(the_kept_styles_of_animated_elements_do_not_hold_values_of_a_reset_arena)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes pk{from{color:rgb(10,20,30)}to{color:rgb(10,20,30)}}"
                 "#bg{width:10px;height:10px;animation:pk 100ms linear infinite}"
                 ".t{vertical-align:50px;display:inline-block;width:10px;height:10px}</style><div id=bg></div><div id=host></div>");
    for (int i = 0; i < 6; i++) {
        EMB_CHECK(rig_js(&r, "var t=document.createElement('div'); t.className='t'; host.appendChild(t);"
                             "var an=t.animate({verticalAlign:['100px','200px']},{duration:1000,fill:'both',composite:'add'});"
                             "an.pause(); an.currentTime=500; getComputedStyle(t).verticalAlign"));
        rig_frames(&r, 3, 16);
        EMB_CHECK(rig_js(&r, "an.currentTime=250; getComputedStyle(t).verticalAlign"));
        rig_frames(&r, 2, 16);
        EMB_CHECK_EQ_STR(r.res, "175px");
        EMB_CHECK(rig_js(&r, "an.cancel(); getComputedStyle(t).verticalAlign"));
        rig_frames(&r, 2, 16);
        EMB_CHECK(rig_js(&r, "t.remove(); 1"));
    }
    EMB_CHECK_EQ_INT((int)num(&r, "host.childNodes.length"), 0);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ the lifetime of animated values (WPTASAN)
 *
 * These run with the poison mode of the arenas (rig_setup_poisoned): memory an arena gives back is poisoned and never
 * reused, and rig_poison counts the values in poisoned memory that the document's styles, the animator's kept styles and the
 * restyles so far have reached. Without the lifetime rule (animator.c, top; style.h, value blocks) they fail deterministically,
 * without ASan. */

/* Logs the transition events of every element as id:type:property. */
#define LOG_TRANSITIONS                                                                                                     \
    "['transitionrun','transitionend','transitioncancel'].forEach(t=>document.addEventListener(t,"                        \
    "e=>console.log(e.target.id+':'+t+':'+e.propertyName),true));"

/* The values of an overlay kept in a slot outlive the arenas of the frames that made them. #a's opacity transition ends
 * while #b's animation keeps the frames styling; #a's next restyle reads its old slot. (The round 2 review's scenario: the
 * state freed in the same restyle that leaves an overlay in the slot cannot happen, st_free runs only when the result is the
 * base; the test stays as a check of the slot's values.) */
EMB_TEST(an_overlay_kept_in_a_slot_after_its_transition_finished_survives_the_arena_swaps)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:.2}to{opacity:.8}}"
                 "#a{width:100px;height:50px;opacity:1;transition:opacity 100ms linear}"
                 "#b{width:10px;height:10px;animation:k 1s linear infinite alternate}</style><div id=a></div><div id=b></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); ['transitionrun','transitionend'].forEach(t=>"
                         "a.addEventListener(t,e=>console.log(t+':'+e.propertyName))); getComputedStyle(a).opacity"));
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "a.style.opacity='0'; getComputedStyle(a).opacity"));
    rig_frames(&r, 12, 16); /* the transition ends; b keeps the frames styling */
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 0);
    rig_frames(&r, 6, 16);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    EMB_CHECK(rig_js(&r, "a.style.opacity='1'; getComputedStyle(a).opacity"));
    rig_frames(&r, 12, 16);
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 100);
    EMB_CHECK_EQ_STR(r.out.text, "transitionrun:opacity;transitionend:opacity;transitionrun:opacity;transitionend:opacity;");
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

/* An element taken out of the document keeps its style (the overlay of its running animation) while other animations keep
 * the frames going; when it comes back, its restyle reads that old style. */
EMB_TEST(a_detached_animated_element_keeps_the_values_of_its_style_until_it_comes_back)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:.2;transform:translateX(10px)}to{opacity:.8;transform:translateX(90px)}}"
                 "@keyframes bk{from{color:rgb(0,0,0)}to{color:rgb(200,0,0)}}"
                 "#x{width:10px;height:10px;animation:k 1s linear infinite;transition:width 1s linear}"
                 "#b{width:10px;height:10px;animation:bk 1s linear infinite}</style>"
                 "<div id=host><div id=x></div></div><div id=b></div>");
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var x=document.getElementById('x'); getComputedStyle(x).opacity"));
    rig_frames(&r, 3, 16);
    EMB_CHECK(rig_js(&r, "x.remove(); 1"));
    rig_frames(&r, 6, 16); /* #b keeps the frames styling */
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    EMB_CHECK(rig_js(&r, "host.appendChild(x); getComputedStyle(x).width"));
    EMB_CHECK_EQ_STR(r.res, "10px");
    rig_frames(&r, 3, 16);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    EMB_CHECK_EQ_STR(r.out.text, "");
    rig_teardown(&r);
}

/* A transition keeps its before-change value. Here it is the value the element inherits from its parent's animation (a
 * text-shadow, whose layers live apart from the value): the transition keeps a copy of it, so it runs on after the parent's
 * overlay of that frame is gone. */
EMB_TEST(a_transition_from_an_animated_inherited_value_keeps_its_own_copy)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes sh{from{text-shadow:1px 1px 1px rgb(0,0,255)}to{text-shadow:9px 9px 9px rgb(0,0,255)}}"
                 "#p{animation:sh 1s linear infinite alternate}"
                 "#c{transition:text-shadow 1s linear}#c.on{text-shadow:0px 0px 5px rgb(255,0,0)}</style>"
                 "<div id=p><div id=c>x</div></div>");
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var c=document.getElementById('c'); getComputedStyle(c).textShadow"));
    rig_frames(&r, 3, 50);
    EMB_CHECK(rig_js(&r, "c.classList.add('on'); getComputedStyle(c).textShadow"));
    rig_frames(&r, 10, 50); /* half way; the parent's overlays of these frames come and go */
    EMB_CHECK(rig_js(&r, "getComputedStyle(c).textShadow"));
    EMB_CHECK(SDL_strstr(r.res, "rgb(") != nullptr && SDL_strstr(r.res, "px") != nullptr);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_frames(&r, 12, 50);
    EMB_CHECK(rig_js(&r, "getComputedStyle(c).textShadow"));
    EMB_CHECK_EQ_STR(r.res, "0px 0px 5px rgb(255, 0, 0)");
    EMB_CHECK_EQ_STR(r.out.text, "c:transitionrun:text-shadow;c:transitionend:text-shadow;");
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

/* A keyframe that says inherit takes the parent's value when the keyframes are resolved; the parent's value is an animated
 * one here, and the resolved keyframes outlive the parent's overlay of that frame: they keep a copy. */
/* The same with a value whose parts are not rebuilt on the way into the transition (a calc() with a percentage keeps its tree,
 * where a shadow list with colors is resolved into a new list): the transition's own deep copy is what keeps it alive. */
EMB_TEST(a_transition_from_an_animated_inherited_calc_keeps_its_own_copy)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes ws{from{word-spacing:calc(1px + 10%)}to{word-spacing:calc(9px + 20%)}}"
                 "#p{animation:ws 1s linear infinite alternate}"
                 "#c{transition:word-spacing 1s linear}#c.on{word-spacing:3px}</style>"
                 "<div id=p><div id=c>x y</div></div>");
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var c=document.getElementById('c'); getComputedStyle(c).wordSpacing"));
    rig_frames(&r, 3, 50);
    EMB_CHECK(rig_js(&r, "c.classList.add('on'); getComputedStyle(c).wordSpacing"));
    rig_frames(&r, 10, 50);
    EMB_CHECK(rig_js(&r, "getComputedStyle(c).wordSpacing"));
    EMB_CHECK(SDL_strstr(r.res, "calc(") != nullptr);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_frames(&r, 12, 50);
    EMB_CHECK(rig_js(&r, "getComputedStyle(c).wordSpacing"));
    EMB_CHECK_EQ_STR(r.res, "3px");
    EMB_CHECK_EQ_STR(r.out.text, "c:transitionrun:word-spacing;c:transitionend:word-spacing;");
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

EMB_TEST(an_inherit_keyframe_keeps_its_own_copy_of_the_animated_parent_value)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes sh{from{text-shadow:1px 1px 1px rgb(0,0,255)}to{text-shadow:9px 9px 9px rgb(0,0,255)}}"
                 "@keyframes ik{from{text-shadow:inherit}to{text-shadow:0px 0px 5px rgb(255,0,0)}}"
                 "#p{animation:sh 1s linear infinite alternate}"
                 "#c{text-shadow:none;animation:ik 10s linear}</style><div id=p><div id=c>x</div></div>");
    EMB_CHECK(rig_js(&r, "var c=document.getElementById('c'); getComputedStyle(c).textShadow"));
    for (int i = 0; i < 4; i++) {
        rig_frames(&r, 5, 50);
        EMB_CHECK(rig_js(&r, "getComputedStyle(c).textShadow"));
        EMB_CHECK(SDL_strstr(r.res, "rgb(") != nullptr && SDL_strstr(r.res, "px") != nullptr);
        EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    }
    rig_teardown(&r);
}

/* The parent style a child was last restyled under (kept for the before-change check) is the parent's overlay while the
 * parent animates opacity; it stays alive however long the child keeps it, and a declared change of the child still
 * transitions. */
EMB_TEST(the_kept_parent_style_of_a_child_under_an_opacity_animation_stays_alive)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes op{from{opacity:.1}to{opacity:.9}}#p{animation:op 1s linear infinite alternate}"
                 "#c{color:rgb(0,0,0);transition:color 200ms linear}#c.on{color:rgb(0,0,200)}</style>"
                 "<div id=p><div id=c>x</div></div>");
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var c=document.getElementById('c'); getComputedStyle(c).color"));
    for (int i = 0; i < 5; i++) {
        rig_frames(&r, 1, 16);
        EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    }
    EMB_CHECK(rig_js(&r, "c.classList.add('on'); getComputedStyle(c).color"));
    rig_frames(&r, 20, 16);
    EMB_CHECK(rig_js(&r, "getComputedStyle(c).color"));
    EMB_CHECK_EQ_STR(r.res, "rgb(0, 0, 200)");
    EMB_CHECK_EQ_STR(r.out.text, "c:transitionrun:color;c:transitionend:color;");
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

/* A child that keeps a parent style moves to another parent, then leaves the document; a new document replaces the old one
 * while children still keep parent styles (embi_animator_reset lets them go), and the rig's teardown frees a document whose
 * animator still keeps some. */
EMB_TEST(kept_parent_styles_survive_reparenting_removal_a_new_document_and_teardown)
{
    rig r;
    rig_setup_poisoned(&r);
    const char *page = "<style>@keyframes fs{from{font-size:20px}to{font-size:30px}}#p{animation:fs 1s linear infinite alternate}"
                       "#q{font-size:16px}#c{width:10em;height:5px;transition:width 1s linear}</style>"
                       "<div id=p><div id=c></div></div><div id=q></div>";
    rig_load(&r, page);
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var c=document.getElementById('c'); getComputedStyle(c).width"));
    rig_frames(&r, 4, 50);
    EMB_CHECK(rig_js(&r, "q.appendChild(c); getComputedStyle(c).width"));
    EMB_CHECK_EQ_STR(r.res, "160px");
    rig_frames(&r, 4, 50);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    EMB_CHECK(rig_js(&r, "p.appendChild(c); getComputedStyle(c).width"));
    rig_frames(&r, 4, 50);
    EMB_CHECK(rig_js(&r, "c.remove(); 1"));
    rig_frames(&r, 4, 50);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    EMB_CHECK_EQ_STR(r.out.text, "");
    rig_load(&r, page); /* a new document while the old one's children keep parent styles */
    EMB_CHECK(rig_js(&r, "var c=document.getElementById('c'); getComputedStyle(c).width"));
    rig_frames(&r, 4, 50);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r); /* #c keeps a parent style */
}

/* The shape of css-transitions/properties-value-inherit-001 (the WPT test where the poison mode sees the restyles of main's
 * animator read freed animated values: z-index): children that inherit, explicitly, non-inherited properties that their
 * parent transitions, and declare transitions of them too (which do not start: the change is the parent's transition). An
 * integer steps, so most frames give the child a base equal in content to the kept one. Another animation keeps the frames
 * going after the transitions end. */
EMB_TEST(children_inheriting_transitioned_values_explicitly_never_read_freed_values)
{
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:.2}to{opacity:.8}}#b{width:5px;height:5px;animation:k 1s linear infinite}"
                 ".p{position:absolute;z-index:1;width:100px;padding-left:1px;border-left:1px solid rgb(0,0,0);"
                 "transform:translate(1px,2px);box-shadow:1px 1px 2px rgb(0,0,0)}.p.how{transition:all 1s linear}"
                 ".p.to{z-index:3;width:200px;padding-left:9px;border-left:5px solid rgb(0,0,255);"
                 "transform:translate(9px,8px) rotate(5deg);box-shadow:4px 4px 8px rgb(0,0,255)}"
                 ".c{position:absolute;z-index:inherit;width:inherit;padding-left:inherit;border-left:inherit;transform:inherit;"
                 "box-shadow:inherit}.c.how{transition:all 1s linear}</style><div id=b></div>"
                 "<div id=f><div class=p id=p1><div class=c id=c1>x</div></div><div class=p id=p2><div class=c id=c2>y</div></div>"
                 "</div>");
    EMB_CHECK(rig_js(&r, LOG_TRANSITIONS "var ps=document.querySelectorAll('.p'), cs=document.querySelectorAll('.c');"
                         "getComputedStyle(cs[0]).width"));
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "ps.forEach(p=>p.classList.add('how')); cs.forEach(c=>c.classList.add('how'));"
                         "getComputedStyle(cs[0]).width"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "ps.forEach(p=>p.classList.add('to')); getComputedStyle(cs[0]).width"));
    for (int i = 0; i < 8; i++) {
        rig_frames(&r, 10, 16);
        EMB_CHECK(rig_js(&r, "getComputedStyle(cs[0]).zIndex + getComputedStyle(cs[1]).width")); /* the test's reads */
    }
    EMB_CHECK(rig_js(&r, "cs.forEach(c=>c.setAttribute('data-x','1')); getComputedStyle(cs[0]).transform"));
    EMB_CHECK_EQ_STR(r.res, "translate(9px, 8px) rotate(5deg)");
    EMB_CHECK(rig_js(&r, "getComputedStyle(cs[1]).boxShadow"));
    EMB_CHECK_EQ_STR(r.res, "4px 4px 8px rgb(0, 0, 255)");
    EMB_CHECK(rig_js(&r, "getComputedStyle(cs[0]).zIndex + ' ' + getComputedStyle(cs[1]).zIndex"));
    EMB_CHECK_EQ_STR(r.res, "3 3");
    EMB_CHECK(SDL_strstr(r.out.text, "c1:") == nullptr && SDL_strstr(r.out.text, "c2:") == nullptr);
    EMB_CHECK(rig_js(&r, "ps.forEach(p=>p.classList.remove('how','to')); cs.forEach(c=>c.classList.remove('how'));"
                         "getComputedStyle(cs[0]).zIndex"));
    rig_frames(&r, 4, 16);
    EMB_CHECK(rig_js(&r, "getComputedStyle(cs[0]).zIndex + ' ' + getComputedStyle(cs[1]).width"));
    EMB_CHECK_EQ_STR(r.res, "1 100px");
    EMB_CHECK(rig_js(&r, "document.getElementById('f').remove(); 1"));
    rig_frames(&r, 4, 16);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

/* A steady animation does not grow the memory of its values: value blocks go with the overlays that pinned them. */
EMB_TEST(value_blocks_of_a_steady_animation_do_not_pile_up)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>@keyframes k{from{transform:translateX(0px);opacity:.1}to{transform:translateX(90px);opacity:.9}}"
                 ".a{width:10px;height:10px;animation:k 1s linear infinite}</style>"
                 "<div class=a></div><div class=a><div class=a></div></div>");
    rig_frames(&r, 10, 16);
    embi_style_engine *se = embi_doc_style_engine(r.d);
    uint32_t n0 = embi_style_vblock_count(se);
    size_t b0 = embi_animator_arena_bytes(embi_doc_animator(r.d));
    rig_frames(&r, 200, 16);
    EMB_CHECK(embi_style_vblock_count(se) <= n0 + 2u);
    EMB_CHECK(embi_animator_arena_bytes(embi_doc_animator(r.d)) <= b0 + 4096u);
    EMB_CHECK(embi_style_vblock_count(se) <= 8u); /* three animated elements: about one block each */
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ CSS animations */

/* ---- inherited changes under a moving ancestor that a declaration made (round 2 review, finding 3): they transition. #m
 * animates (opacity, which nothing inherits, unless a test says otherwise) for the whole test; the changes come from a
 * declaration somewhere on the chain, the environment, the sheets, or the element itself. */
#define MOVER "@keyframes op{from{opacity:.2}to{opacity:.8}}#m{animation:op 1s linear infinite alternate}"

static void mv_start(rig *r, const char *page)
{
    rig_setup_poisoned(r);
    embi_doc_set_viewport(r->d, 600.0f, 400.0f, 1.0f, false);
    rig_load(r, page);
    EMB_CHECK(rig_js(r, LOG_TRANSITIONS "var c=document.getElementById('c'),app=document.getElementById('app');"
                        "getComputedStyle(c).color"));
    rig_frames(r, 3, 16);
}

static void mv_end(rig *r, const char *events)
{
    EMB_CHECK_EQ_STR(r->out.text, events);
    EMB_CHECK_EQ_INT((int)rig_poison(r), 0);
    rig_teardown(r);
}

static const char *cstyle(rig *r, const char *prop)
{
    char src[160];
    SDL_snprintf(src, sizeof src, "getComputedStyle(c).%s", prop);
    EMB_CHECK(rig_js(r, src));
    return r->res;
}

EMB_TEST(a_declared_color_change_above_an_infinitely_animating_ancestor_transitions)
{
    rig r;
    mv_start(&r, "<style>" MOVER "#app{color:rgb(0,0,0)}#app.dark{color:rgb(255,0,0)}#c{transition:color 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c>x</div></div></div>");
    EMB_CHECK(rig_js(&r, "app.classList.add('dark'); getComputedStyle(c).color"));
    rig_frames(&r, 6, 16);
    EMB_CHECK(SDL_strcmp(cstyle(&r, "color"), "rgb(255, 0, 0)") != 0); /* mid-way */
    rig_frames(&r, 20, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(255, 0, 0)");
    mv_end(&r, "c:transitionrun:color;c:transitionend:color;");
}

EMB_TEST(a_declared_color_change_between_an_animating_ancestor_and_the_element_transitions)
{
    rig r;
    mv_start(&r, "<style>" MOVER "#app{color:rgb(0,0,0)}#app.dark{color:rgb(255,0,0)}#c{transition:color 200ms linear}</style>"
                 "<div id=m><div id=app><div id=c>x</div></div></div>");
    EMB_CHECK(rig_js(&r, "app.classList.add('dark'); getComputedStyle(c).color"));
    rig_frames(&r, 26, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(255, 0, 0)");
    mv_end(&r, "c:transitionrun:color;c:transitionend:color;");
}

/* The frame right after an ancestor's transition ended (the frames it still counts as moving): a declared change of an
 * inherited property transitions. */
EMB_TEST(a_declared_change_in_the_frame_after_an_ancestor_transition_ended_transitions)
{
    rig r;
    mv_start(&r, "<style>#app{color:rgb(0,0,0)}#app.dark{color:rgb(255,0,0)}#m{font-size:16px;transition:font-size 100ms linear}"
                 "#m.big{font-size:30px}#c{transition:color 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c>x</div></div></div>");
    EMB_CHECK(rig_js(&r, "m.classList.add('big'); getComputedStyle(c).color"));
    for (int i = 0; i < 30 && SDL_strstr(r.out.text, "m:transitionend") == nullptr; i++) {
        rig_frames(&r, 1, 16);
    }
    EMB_CHECK_EQ_STR(r.out.text, "m:transitionrun:font-size;m:transitionend:font-size;");
    EMB_CHECK(rig_js(&r, "app.classList.add('dark'); getComputedStyle(c).color"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "document.body.classList.add('x'); getComputedStyle(c).color")); /* and the frame after it */
    rig_frames(&r, 25, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(255, 0, 0)");
    mv_end(&r, "m:transitionrun:font-size;m:transitionend:font-size;c:transitionrun:color;c:transitionend:color;");
}

EMB_TEST(a_media_query_change_under_an_animating_ancestor_transitions)
{
    rig r;
    mv_start(&r, "<style>" MOVER "#app{color:rgb(0,0,0)}@media (min-width:700px){#app{color:rgb(255,0,0)}}"
                 "#c{transition:color 200ms linear}</style><div id=app><div id=m><div id=c>x</div></div></div>");
    embi_doc_set_viewport(r.d, 800.0f, 400.0f, 1.0f, false);
    rig_frames(&r, 26, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(255, 0, 0)");
    mv_end(&r, "c:transitionrun:color;c:transitionend:color;");
}

EMB_TEST(a_viewport_unit_change_under_an_animating_ancestor_transitions)
{
    rig r;
    mv_start(&r, "<style>" MOVER "#c{width:10vw;height:5px;transition:width 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c></div></div></div>");
    EMB_CHECK_EQ_STR(cstyle(&r, "width"), "60px");
    embi_doc_set_viewport(r.d, 800.0f, 400.0f, 1.0f, false);
    rig_frames(&r, 26, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "width"), "80px");
    mv_end(&r, "c:transitionrun:width;c:transitionend:width;");
}

/* A style element added, its text changed, and removed: each changes what #app declares. (The engine's CSSOM has no
 * insertRule or deleteRule; a sheet's text change is the same restyle, a full one.) */
EMB_TEST(sheet_changes_and_sheet_removal_under_an_animating_ancestor_transition)
{
    rig r;
    mv_start(&r, "<style>" MOVER "#app{color:rgb(0,0,0)}#c{transition:color 100ms linear}</style>"
                 "<div id=app><div id=m><div id=c>x</div></div></div>");
    EMB_CHECK(rig_js(&r, "var st=document.createElement('style'); st.textContent='#app{color:rgb(0,0,255)}';"
                         "document.head.appendChild(st); getComputedStyle(c).color"));
    rig_frames(&r, 12, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(0, 0, 255)");
    EMB_CHECK(rig_js(&r, "st.textContent='#app{color:rgb(0,128,0)}'; getComputedStyle(c).color"));
    rig_frames(&r, 12, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(0, 128, 0)");
    EMB_CHECK(rig_js(&r, "st.remove(); getComputedStyle(c).color"));
    rig_frames(&r, 12, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(0, 0, 0)");
    mv_end(&r, "c:transitionrun:color;c:transitionend:color;c:transitionrun:color;c:transitionend:color;"
               "c:transitionrun:color;c:transitionend:color;");
}

/* The element's own inline style changes while its parent's font-size animates (its width is in em). */
EMB_TEST(the_elements_own_inline_style_change_under_an_animating_parent_transitions)
{
    rig r;
    mv_start(&r, "<style>" EM_FONT_ANIM_M "#c{width:10em;height:5px;transition:width 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c></div></div></div>");
    rig_frames(&r, 5, 50);
    EMB_CHECK_EQ_STR(r.out.text, "");
    EMB_CHECK(rig_js(&r, "c.style.width='50px'; getComputedStyle(c).width"));
    rig_frames(&r, 12, 50);
    EMB_CHECK_EQ_STR(cstyle(&r, "width"), "50px");
    mv_end(&r, "c:transitionrun:width;c:transitionend:width;");
}

/* Values that depend on an animated inherited input in other ways: font-size: inherit, a calc() with em, a percentage, and
 * currentcolor under an animated color. None transitions; a declared change of a custom property above does. */
EMB_TEST(inherit_calc_percent_and_currentcolor_follow_an_animating_parent_without_transitions)
{
    rig r;
    mv_start(&r, "<style>@keyframes fc{from{font-size:20px;color:rgb(0,0,0);width:100px}to{font-size:30px;color:rgb(0,0,250);"
                 "width:300px}}#m{animation:fc 1s linear infinite alternate}"
                 "#c{font-size:inherit;width:calc(2em + 10px);height:50%;border:1px solid currentcolor;"
                 "transition:font-size 200ms linear,width 200ms linear,height 200ms linear,border-top-color 200ms linear}"
                 "</style><div id=app><div id=m><div id=c>x</div></div></div>");
    rig_frames(&r, 10, 50);
    EMB_CHECK_EQ_STR(r.out.text, "");
    mv_end(&r, "");
}

EMB_TEST(a_declared_custom_property_change_above_an_animating_parent_transitions)
{
    rig r;
    mv_start(&r, "<style>" EM_FONT_ANIM_M "#app{--w:20px}#app.wide{--w:60px}"
                 "#c{width:calc(var(--w) + 1em);height:5px;transition:width 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c></div></div></div>");
    rig_frames(&r, 5, 50);
    EMB_CHECK_EQ_STR(r.out.text, "");
    EMB_CHECK(rig_js(&r, "app.classList.add('wide'); getComputedStyle(c).width"));
    rig_frames(&r, 2, 50);
    mv_end(&r, "c:transitionrun:width;");
}

/* The parent changes a declaration and starts an animation of its font-size in the same pass (one class does both): the
 * child's color transitions, its em width follows the animation without one. */
EMB_TEST(a_parent_declared_change_and_animation_start_in_the_same_pass)
{
    rig r;
    mv_start(&r, "<style>@keyframes fs{from{font-size:20px}to{font-size:30px}}#m{font-size:16px;color:rgb(0,0,0)}"
                 "#m.go{color:rgb(255,0,0);animation:fs 1s linear infinite alternate}"
                 "#c{width:10em;height:5px;transition:color 200ms linear,width 200ms linear}</style>"
                 "<div id=app><div id=m><div id=c>x</div></div></div>");
    EMB_CHECK_EQ_STR(cstyle(&r, "width"), "160px");
    EMB_CHECK(rig_js(&r, "m.classList.add('go'); getComputedStyle(c).width"));
    EMB_CHECK_EQ_STR(r.res, "200px");
    rig_frames(&r, 20, 16);
    EMB_CHECK_EQ_STR(cstyle(&r, "color"), "rgb(255, 0, 0)");
    mv_end(&r, "c:transitionrun:color;c:transitionend:color;");
}

EMB_TEST(css_animation_values_and_events)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:0}to{opacity:1}}"
                 "#a{width:100px;height:50px;animation:k 100ms linear 1 forwards}</style><div id=a></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); ['animationstart','animationiteration','animationend']"
                         ".forEach(t=>a.addEventListener(t,e=>console.log(t+':'+e.animationName+':'+e.elapsedTime)));0"));
    rig_frames(&r, 1, 16);
    rig_ms(50);
    (void)rig_frame(&r);
    double v = num(&r, "getComputedStyle(a).opacity");
    EMB_CHECK(v > 0.2 && v < 0.8);
    rig_frames(&r, 1, 100);
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 100);
    EMB_CHECK_EQ_STR(r.out.text, "animationstart:k:0;animationend:k:0.1;");
    rig_teardown(&r);
}

EMB_TEST(css_animation_shows_in_get_animations)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:0}to{opacity:1}}"
                 "#a{width:100px;height:50px;animation:k 1s linear infinite}</style><div id=a></div>");
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); var l=a.getAnimations();"
                         "l.length+':'+(l[0] instanceof CSSAnimation)+':'+l[0].animationName+':'+l[0].playState"));
    EMB_CHECK_EQ_STR(r.res, "1:true:k:running");
    EMB_CHECK(rig_js(&r, "document.getAnimations().length+':'+(document.getAnimations()[0]===l[0])"));
    EMB_CHECK_EQ_STR(r.res, "1:true");
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ Web Animations */

EMB_TEST(element_animate_runs_and_finishes)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a style='width:100px;height:50px'></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                         "var an=a.animate([{opacity:0},{opacity:1}],{duration:100,fill:'both'});"
                         "an.onfinish=e=>console.log('finish:'+e.type);"
                         "an.finished.then(x=>console.log('finished:'+(x===an)));"
                         "an.ready.then(x=>console.log('ready'));"
                         "an.playState+':'+an.pending"));
    EMB_CHECK_EQ_STR(r.res, "running:true");
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "an.playState+':'+an.pending"));
    EMB_CHECK_EQ_STR(r.res, "running:false");
    rig_ms(50);
    (void)rig_frame(&r);
    double v = num(&r, "getComputedStyle(a).opacity");
    EMB_CHECK(v > 0.2 && v < 0.8);
    rig_frames(&r, 1, 100);
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 100);
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_STR(r.out.text, "ready;finished:true;finish:finish;");
    EMB_CHECK(rig_js(&r, "an.playState"));
    EMB_CHECK_EQ_STR(r.res, "finished");
    rig_teardown(&r);
}

EMB_TEST(animation_controls_pause_reverse_rate_cancel)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a style='width:100px;height:50px'></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                         "var an=a.animate([{opacity:0},{opacity:1}],{duration:1000,fill:'both'}); an.pause();"
                         "an.playState+':'+an.pending"));
    EMB_CHECK_EQ_STR(r.res, "paused:true");
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "an.playState+':'+an.pending+':'+an.currentTime"));
    EMB_CHECK_EQ_STR(r.res, "paused:false:0");
    EMB_CHECK(rig_js(&r, "an.currentTime=250; an.currentTime"));
    EMB_CHECK_EQ_STR(r.res, "250");
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_INT((int)(num(&r, "getComputedStyle(a).opacity") * 100), 25);
    EMB_CHECK(rig_js(&r, "an.playbackRate=2; an.play(); an.playbackRate"));
    EMB_CHECK_EQ_STR(r.res, "2");
    rig_frames(&r, 3, 50);
    EMB_CHECK(num(&r, "an.currentTime") > 400.0);
    EMB_CHECK(rig_js(&r, "an.reverse(); an.playbackRate")); /* the new rate applies when the pending play is ready */
    EMB_CHECK_EQ_STR(r.res, "2");
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "an.playbackRate"));
    EMB_CHECK_EQ_STR(r.res, "-2");
    EMB_CHECK(rig_js(&r, "an.cancel(); an.playState+':'+an.currentTime+':'+getComputedStyle(a).opacity"));
    EMB_CHECK_EQ_STR(r.res, "idle:null:1");
    rig_teardown(&r);
}

EMB_TEST(animation_api_validation)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'); var out=[];"
                         "try{a.animate([{opacity:0}],{duration:-1})}catch(e){out.push(e.name)}"
                         "try{a.animate([{opacity:0}],{easing:'nope'})}catch(e){out.push(e.name)}"
                         "try{a.animate([{offset:1,opacity:0},{offset:0,opacity:1}])}catch(e){out.push(e.name)}"
                         "var an=a.animate({opacity:[0,1]},500); out.push(an.effect.getKeyframes().length);"
                         "out.push(an.effect.getTiming().duration, an.effect.getComputedTiming().endTime);"
                         "out.push(typeof document.timeline.currentTime, a.getAnimations().length);"
                         "out.join(',')"));
    EMB_CHECK_EQ_STR(r.res, "TypeError,TypeError,TypeError,2,500,500,number,1");
    rig_teardown(&r);
}

EMB_TEST(new_animation_with_keyframe_effect_and_document_timeline)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a style='width:100px;height:50px'></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                         "var e=new KeyframeEffect(a,[{opacity:0},{opacity:1}],{duration:100,fill:'forwards'});"
                         "var an=new Animation(e, document.timeline);"
                         "var s0=an.playState; an.play(); var ok=(an.effect===e)&&(e.target===a);"
                         "s0+':'+an.playState+':'+ok+':'+(an.timeline===document.timeline)"));
    EMB_CHECK_EQ_STR(r.res, "idle:running:true:true");
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ the benchmark page */

EMB_TEST(anim_60fps_page_animates_and_keeps_going)
{
    size_t len = 0;
    char *html = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/pages/anim-60fps.html", &len);
    EMB_CHECK(html != nullptr);
    if (html == nullptr) {
        return;
    }
    rig r;
    rig_setup(&r);
    (void)embi_doc_load(r.d, (embi_str){html, (uint32_t)len}, (embi_str){"http://t.test/anim.html", 23}, nullptr);
    SDL_free(html);
    uint32_t rebuilt = 0, width_changes = 0;
    double most = 0, prev_w = -1.0;
    for (uint32_t i = 0; i < 120; i++) {
        rig_ms(16);
        rebuilt += rig_frame(&r) ? 1u : 0u;
        double n = num(&r, "document.getAnimations().length");
        most = n > most ? n : most;
        double w = num(&r, "document.getElementById('bar').getBoundingClientRect().width");
        width_changes += (prev_w >= 0.0 && w != prev_w) ? 1u : 0u;
        prev_w = w;
    }
    double flips = num(&r, "window.__flips");
    EMB_CHECK_EQ_INT((int)num(&r, "window.__frames"), 120);
    EMB_CHECK(flips >= 3.0);
    EMB_CHECK(embi_doc_animating(r.d));
    /* The bar's width animates every frame (layout); the 200 items' transform and opacity transitions are paint only. The
     * fragments are rebuilt in the frames where the bar moved (the first frame may also rebuild), plus at most one rebuild
     * per flip: not once per item and not in the frames where only the items move. */
    EMB_CHECK(width_changes >= 100u);
    EMB_CHECK(rebuilt >= width_changes);
    EMB_CHECK(rebuilt <= width_changes + (uint32_t)flips + 1u);
    EMB_CHECK(most >= 200.0); /* every item transitions on each flip */
    rig_teardown(&r);
}

/* ------------------------------------------------------------------ memory */

static void anim_cycle(void)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>@keyframes k{from{opacity:0}to{opacity:1}}#b{animation:k 50ms linear}</style>"
                 "<div id=a style='width:10px'></div><div id=b></div>");
    (void)rig_js(&r, "var a=document.getElementById('a'); var an=a.animate([{opacity:0},{opacity:1}],{duration:40});"
                     "requestAnimationFrame(()=>{}); an.finished.then(()=>0)");
    rig_frames(&r, 12, 16);
    (void)rig_js(&r, "an=null; for(let i=0;i<20;i++) a.animate([{opacity:0},{opacity:1}],{duration:30});");
    rig_frames(&r, 12, 16);
    rig_teardown(&r);
}

EMB_TEST(easing_memo_survives_a_document_reload)
{
    rig r;
    rig_setup(&r);
    for (int i = 0; i < 3; i++) {
        rig_load(&r, "<div id=a style='width:10px'></div>");
        EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                             "var an=a.animate([{opacity:0},{opacity:1}],{duration:100,easing:'cubic-bezier(0.1,0.2,0.3,0.4)'});"
                             "a.animate([{opacity:0},{opacity:1}],{duration:100,easing:'steps(3, jump-end)'});"
                             "an.effect.getTiming().easing"));
        EMB_CHECK_EQ_STR(r.res, "cubic-bezier(0.1,0.2,0.3,0.4)");
        rig_frames(&r, 3, 16);
    }
    rig_teardown(&r);
}

/* More distinct easings than the memo holds (EMBI_EASING_MEMO_MAX): the rest are parsed into the animation's own arena
 * (keyframe easings: the keyframes arena, timing easings: the animation's `ear`) and must stay valid and be released. */
static void easing_flood(void)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=h></div>");
    EMB_CHECK(rig_js(&r,
        "var host=document.getElementById('h'); var A=[], B=[], T=[], bad=[];"
        "for(let i=0;i<320;i++){"
        "  const t=(i%2===0)?'cubic-bezier(0.1,0.2,0.3,0.'+(100+i)+')':'steps('+(i+1)+')';"
        "  T.push(t);"
        "  const ea=document.createElement('div'), eb=document.createElement('div');"
        "  host.appendChild(ea); host.appendChild(eb);"
        "  const x=ea.animate([{opacity:0},{opacity:1}],{duration:1000,fill:'both',easing:t});"
        "  const y=eb.animate([{opacity:0,easing:t},{opacity:1}],{duration:1000,fill:'both'});"
        "  if(x.effect.getTiming().easing.indexOf(i%2===0?'cubic-bezier(':'steps(')!==0) bad.push('rb'+i);"
        "  x.currentTime=500; x.pause(); y.currentTime=500; y.pause(); A.push(ea); B.push(eb);"
        "}"
        "bad.length"));
    EMB_CHECK_EQ_STR(r.res, "0");
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r,
        "var diff=0, stepbad=0, cubicflat=0;"
        "for(let i=0;i<320;i++){"
        "  const oa=parseFloat(getComputedStyle(A[i]).opacity), ob=parseFloat(getComputedStyle(B[i]).opacity);"
        "  if(Math.abs(oa-ob)>1e-6) diff++;"
        "  if(i%2===1){ const n=i+1; if(Math.abs(oa-Math.floor(0.5*n)/n)>1e-4) stepbad++; }"
        "  else if(!(oa>0&&oa<1)) cubicflat++;"
        "}"
        "diff+':'+stepbad+':'+cubicflat"));
    EMB_CHECK_EQ_STR(r.res, "0:0:0");
    /* updateTiming past the cap: a new easing works, an invalid one throws and leaves the old one. */
    EMB_CHECK(rig_js(&r,
        "var an=A[319].getAnimations()[0]; an.effect.updateTiming({easing:'steps(4)'});"
        "var e1=an.effect.getTiming().easing; var thrown='no';"
        "try{an.effect.updateTiming({easing:'nope(1)'})}catch(e){thrown=e.name}"
        "an.effect.updateTiming({easing:'cubic-bezier(0.25,0.1,0.25,1)'});"
        "e1+'|'+thrown+'|'+an.effect.getTiming().easing"));
    EMB_CHECK_EQ_STR(r.res, "steps(4)|TypeError|cubic-bezier(0.25,0.1,0.25,1)");
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "var o=parseFloat(getComputedStyle(A[319]).opacity); (o>0.4 && o<0.95)"));
    EMB_CHECK_EQ_STR(r.res, "true");
    rig_frames(&r, 3, 16);
    EMB_CHECK(rig_js(&r, "document.getAnimations().forEach(a=>a.cancel()); host.textContent=''; A=B=null; document.getAnimations().length"));
    EMB_CHECK_EQ_STR(r.res, "0");
    rig_frames(&r, 3, 16);
    rig_teardown(&r);
}

EMB_TEST(easings_past_the_memo_cap_are_valid_and_released)
{
    easing_flood();
    size_t base = embi_mem_live();
    easing_flood();
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(promise_reactions_run_after_the_calling_script)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a style='width:10px'></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                         "var an=a.animate([{opacity:0},{opacity:1}],{duration:100});"
                         "an.finished.catch(()=>console.log('A')); an.cancel(); console.log('B')"));
    EMB_CHECK_EQ_STR(r.out.text, "B;A;");
    r.out.len = 0;
    r.out.text[0] = 0;
    EMB_CHECK(rig_js(&r, "var an2=a.animate([{opacity:0},{opacity:1}],{duration:100});"
                         "an2.finished.then(()=>console.log('C')); an2.finish(); console.log('D')"));
    EMB_CHECK_EQ_STR(r.out.text, "D;C;");
    rig_teardown(&r);
}

EMB_TEST(ready_callback_changing_another_animation_restyles_in_the_same_frame)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div><div id=b style='width:10px'></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a'), b=document.getElementById('b');"
                         "var y=b.animate([{width:'100px'},{width:'200px'}],{duration:1000,fill:'both'}); y.pause(); 0"));
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "var x=a.animate([{opacity:0},{opacity:1}],{duration:1000});"
                         "x.ready.then(()=>{ y.currentTime=500; }); 0"));
    rig_frames(&r, 1, 16); /* x becomes ready in this frame; its callback moves y before the style update */
    EMB_CHECK_EQ_INT((int)num(&r, "parseFloat(getComputedStyle(b).width)"), 150);
    rig_teardown(&r);
}

EMB_TEST(play_on_a_running_animation_is_a_no_op_and_the_rate_keeps_the_time)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<div id=a></div>");
    EMB_CHECK(rig_js(&r, "var a=document.getElementById('a');"
                         "var an=a.animate([{opacity:0},{opacity:1}],{duration:1000});"));
    rig_frames(&r, 3, 100);
    EMB_CHECK(rig_js(&r, "var r1=an.ready; an.play(); (r1===an.ready)+':'+an.pending"));
    EMB_CHECK_EQ_STR(r.res, "true:false");
    EMB_CHECK(rig_js(&r, "var t0=an.currentTime; an.pause(); an.playbackRate=2; (an.currentTime===t0)+':'+an.playbackRate"));
    EMB_CHECK_EQ_STR(r.res, "true:2");
    rig_teardown(&r);
}

EMB_TEST(animations_do_not_leak)
{
    anim_cycle();
    size_t base = embi_mem_live();
    anim_cycle();
    anim_cycle();
    EMB_CHECK(embi_mem_live() == base);
}

/* PROXYLEAK: the proxies of CSS transitions (made by getAnimations, held by JS wrappers) are freed when their element goes, when
 * the transition has finished or was cancelled, and when the document goes while a wrapper still holds one. */
static void proxy_cycle(void)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>.t{width:10px;height:10px;opacity:1;transition:opacity 50ms linear}</style>"
                 "<div id=a class=t></div><div id=b class=t></div><div id=c class=t></div>");
    (void)rig_js(&r, "var a=document.getElementById('a'),b=document.getElementById('b'),c=document.getElementById('c');"
                     "getComputedStyle(a).opacity; getComputedStyle(b).opacity; getComputedStyle(c).opacity");
    rig_frames(&r, 2, 16);
    (void)rig_js(&r, "a.style.opacity='0'; b.style.opacity='0'; c.style.opacity='0'; getComputedStyle(a).opacity;");
    rig_frames(&r, 2, 16);
    /* wrappers kept alive in globals: one on a transition that finishes, one on one that is cancelled, one on an element removed */
    (void)rig_js(&r, "var wa=a.getAnimations(), wb=b.getAnimations(), wc=c.getAnimations(), wd=document.getAnimations();"
                     "wb[0].cancel(); c.remove(); wa.length+':'+wb.length+':'+wc.length");
    rig_frames(&r, 12, 16); /* a's transition finishes */
    (void)rig_js(&r, "a.getAnimations().length");
    rig_frames(&r, 2, 16);
    rig_teardown(&r); /* wa, wb, wc, wd still hold wrappers here */
}

EMB_TEST(transition_proxies_do_not_leak)
{
    proxy_cycle();
    size_t base = embi_mem_live();
    proxy_cycle();
    proxy_cycle();
    EMB_CHECK(embi_mem_live() == base);
}

/* ANIMSCALE: thousands of elements animating paint-only properties at once (more than the grave of old styles that the layout
 * update held without a full build, 2048) take paint-only updates frame after frame: no frame rebuilds the fragments, every
 * element is at the same point of the animation, and nothing the styles reach is in freed memory. */
EMB_TEST(thousands_of_animated_elements_take_paint_only_updates)
{
    enum { N = 3000 };
    static char html[N * 32 + 1024];
    size_t len = (size_t)SDL_snprintf(html, sizeof html,
                                      "<style>.a{width:20px;height:10px;animation:k 1s linear infinite alternate}"
                                      "@keyframes k{from{opacity:1;transform:translateX(0);background-color:#f00}"
                                      "to{opacity:.2;transform:translateX(30px);background-color:#00f}}</style><body>");
    for (int i = 0; i < N; i++) {
        len += (size_t)SDL_snprintf(html + len, sizeof html - len, "<div class=a id=d%d></div>", i);
    }
    rig r;
    rig_setup_poisoned(&r);
    rig_load(&r, html);
    rig_frames(&r, 3, 16);
    for (int i = 0; i < 6; i++) {
        rig_ms(16);
        EMB_CHECK(!rig_frame(&r));
    }
    embi_layout_incr_stats st;
    embi_layout_incr_stats_get(embi_doc_layout_engine(r.d), embi_doc_dom(r.d), &st);
    EMB_CHECK_EQ_INT((int)st.n_full, 1); /* the first layout */
    EMB_CHECK(st.n_paint >= 6);
    EMB_CHECK(st.n_surgery == 0);
    double first = num(&r, "getComputedStyle(document.getElementById('d0')).opacity");
    double last = num(&r, "getComputedStyle(document.getElementById('d2999')).opacity");
    EMB_CHECK(first > 0.2 && first < 1.0);
    if (first != last) {
        SDL_Log("first %.9f last %.9f", first, last);
    }
    EMB_CHECK(first == last);
    EMB_CHECK_EQ_INT((int)rig_poison(&r), 0);
    rig_teardown(&r);
}

EMB_TEST_MAIN()
