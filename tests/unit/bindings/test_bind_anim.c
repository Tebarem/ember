/*
 * The Web Animations API in the realm (W6c): the classes and their surface, the keyframe forms, effect timing and its
 * validation, the ready and finished promises, the finish, cancel and remove events, getAnimations, the replace state and
 * the lifetime of animations the page does not hold. The frames run on a fake clock without a view (frame_rig.h).
 */
#include "../anim/frame_rig.h"

#define PAGE "<div id=a style='width:100px;height:50px'><div id=b style='width:10px;height:10px'></div></div>"

static void setup_page(rig *r)
{
    rig_setup(r);
    rig_load(r, PAGE);
    (void)rig_js(r, "var a=document.getElementById('a'), b=document.getElementById('b');");
}

EMB_TEST(classes_and_members_exist)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "['Animation','KeyframeEffect','AnimationEffect','AnimationTimeline','DocumentTimeline',"
                         "'AnimationPlaybackEvent','AnimationEvent','TransitionEvent','CSSAnimation','CSSTransition']"
                         ".map(n=>typeof globalThis[n]).join()"));
    EMB_CHECK_EQ_STR(r.res, "function,function,function,function,function,function,function,function,function,function");
    EMB_CHECK(rig_js(&r, "[typeof a.animate, typeof a.getAnimations, typeof document.getAnimations, typeof requestAnimationFrame,"
                         "typeof cancelAnimationFrame, document.timeline instanceof DocumentTimeline,"
                         "document.timeline instanceof AnimationTimeline, document.timeline === document.timeline,"
                         "Object.getPrototypeOf(KeyframeEffect.prototype) === AnimationEffect.prototype,"
                         "Object.getPrototypeOf(Animation.prototype) === EventTarget.prototype].join()"));
    EMB_CHECK_EQ_STR(r.res, "function,function,function,function,function,true,true,true,true,true");
    EMB_CHECK(rig_js(&r, "var o=[]; try{ new AnimationEffect() }catch(e){o.push(e.name)}"
                         "try{ new KeyframeEffect(5, []) }catch(e){o.push(e.name)} o.join()"));
    EMB_CHECK_EQ_STR(r.res, "TypeError,TypeError");
    rig_teardown(&r);
}

EMB_TEST(keyframe_forms_and_get_keyframes)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var k1=a.animate([{opacity:0},{opacity:0.5},{opacity:1}],100).effect.getKeyframes();"
                         "k1.map(k=>k.computedOffset+':'+k.offset+':'+k.opacity).join()"));
    EMB_CHECK_EQ_STR(r.res, "0:null:0,0.5:null:0.5,1:null:1");
    EMB_CHECK(rig_js(&r, "var k2=a.animate({opacity:[0,1,0], offset:[0,0.25,1], easing:'ease-in'},100).effect.getKeyframes();"
                         "k2.map(k=>k.offset+':'+k.easing).join()"));
    EMB_CHECK_EQ_STR(r.res, "0:ease-in,0.25:ease-in,1:ease-in");
    EMB_CHECK(rig_js(&r, "var e=new KeyframeEffect(a,{marginLeft:['0px','10px'], cssFloat:['left','right']},{duration:10});"
                         "var k3=e.getKeyframes()[1]; k3.marginLeft+':'+k3.cssFloat"));
    EMB_CHECK_EQ_STR(r.res, "10px:right");
    EMB_CHECK(rig_js(&r, "e.setKeyframes([{opacity:1},{opacity:0}]); e.getKeyframes().length+':'+e.getKeyframes()[0].opacity"));
    EMB_CHECK_EQ_STR(r.res, "2:1");
    EMB_CHECK(rig_js(&r, "a.animate(null).effect.getKeyframes().length+':'+a.animate([]).effect.getKeyframes().length"));
    EMB_CHECK_EQ_STR(r.res, "0:0");
    rig_teardown(&r);
}

EMB_TEST(timing_getters_computed_and_validation)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var an=a.animate([{opacity:0},{opacity:1}],{duration:200,delay:50,endDelay:10,iterations:2,"
                         "direction:'alternate',fill:'both',easing:'ease-out',iterationStart:0.5});"
                         "var t=an.effect.getTiming(); [t.duration,t.delay,t.endDelay,t.iterations,t.direction,t.fill,t.easing,"
                         "t.iterationStart].join()"));
    EMB_CHECK_EQ_STR(r.res, "200,50,10,2,alternate,both,ease-out,0.5");
    EMB_CHECK(rig_js(&r, "var c=an.effect.getComputedTiming(); [c.endTime,c.activeDuration,c.fill].join()"));
    EMB_CHECK_EQ_STR(r.res, "460,400,both");
    EMB_CHECK(rig_js(&r, "an.effect.updateTiming({duration:100,iterations:1}); var c2=an.effect.getComputedTiming();"
                         "[c2.endTime,c2.activeDuration,an.effect.getTiming().delay].join()"));
    EMB_CHECK_EQ_STR(r.res, "160,100,50");
    EMB_CHECK(rig_js(&r, "var errs=[]; function bad(o){ try{ a.animate([{opacity:0}],o); errs.push('ok') }catch(e){ errs.push(e.name) } }"
                         "bad({iterations:-1}); bad({iterations:NaN}); bad({iterationStart:-1}); bad({direction:'sideways'});"
                         "bad({fill:'x'}); bad({duration:'slow'}); bad({duration:NaN}); bad({easing:'cubic-bezier(2,0,1,1)'});"
                         "bad({duration:'auto'}); bad(Infinity); errs.join()"));
    EMB_CHECK_EQ_STR(r.res, "TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,ok,ok");
    rig_teardown(&r);
}

EMB_TEST(promises_events_finish_and_cancel)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var an=a.animate([{opacity:0},{opacity:1}],{duration:100});"
                         "an.addEventListener('finish',e=>console.log('finish:'+(e instanceof AnimationPlaybackEvent)+':'+e.currentTime));"
                         "an.addEventListener('cancel',e=>console.log('cancel:'+e.currentTime));"
                         "an.finished.then(()=>console.log('fin-ok'),e=>console.log('fin-err:'+e.name));"
                         "an.ready.then(()=>console.log('ready-ok'),e=>console.log('ready-err:'+e.name)); 0"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "an.finish(); an.playState"));
    EMB_CHECK_EQ_STR(r.res, "finished");
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_STR(r.out.text, "ready-ok;fin-ok;finish:true:100;");
    EMB_CHECK(rig_js(&r, "an.cancel(); an.playState+':'+an.currentTime+':'+an.pending"));
    EMB_CHECK_EQ_STR(r.res, "idle:null:false");
    rig_frames(&r, 1, 16);
    EMB_CHECK_EQ_STR(r.out.text, "ready-ok;fin-ok;finish:true:100;cancel:null;");
    /* Cancelling while the play is pending rejects the ready promise, and the finished promise too. */
    r.out.len = 0;
    r.out.text[0] = '\0';
    EMB_CHECK(rig_js(&r, "var an2=a.animate([{opacity:0},{opacity:1}],100);"
                         "an2.ready.then(()=>console.log('r2-ok'),e=>console.log('r2-err:'+e.name));"
                         "an2.finished.then(()=>console.log('f2-ok'),e=>console.log('f2-err:'+e.name)); an2.cancel(); 0"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(SDL_strstr(r.out.text, "r2-err:AbortError;") != nullptr);
    EMB_CHECK(SDL_strstr(r.out.text, "f2-err:AbortError;") != nullptr);
    EMB_CHECK(rig_js(&r, "an2.finished === an2.finished"));
    EMB_CHECK_EQ_STR(r.res, "true");
    rig_teardown(&r);
}

EMB_TEST(unreferenced_animation_still_finishes_and_fires)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "a.animate([{opacity:0},{opacity:1}],{duration:40}).onfinish=()=>console.log('fin'); 0"));
    rig_frames(&r, 6, 16);
    EMB_CHECK_EQ_STR(r.out.text, "fin;");
    rig_teardown(&r);
}

EMB_TEST(get_animations_element_subtree_and_relevance)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var x=a.animate([{opacity:0},{opacity:1}],100), y=b.animate([{opacity:0},{opacity:1}],100);"
                         "var z=b.animate([{opacity:1},{opacity:0}],{duration:100,delay:1000});"
                         "[a.getAnimations().length,a.getAnimations({subtree:true}).length,b.getAnimations().length,"
                         "document.getAnimations().length,a.getAnimations({subtree:true})[0]===x].join()"));
    EMB_CHECK_EQ_STR(r.res, "1,3,2,3,true");
    /* A finished animation without fill is no longer relevant. */
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "x.finish(); y.finish(); a.getAnimations({subtree:true}).length"));
    EMB_CHECK_EQ_STR(r.res, "1");
    rig_teardown(&r);
}

EMB_TEST(constructor_effect_timeline_and_idle_state)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var n=new Animation(); [n.playState,n.effect,n.timeline===document.timeline,n.currentTime,n.startTime,"
                         "n.pending,n.replaceState,n.playbackRate,n.id].join()"));
    EMB_CHECK_EQ_STR(r.res, "idle,,true,,,false,active,1,");
    EMB_CHECK(rig_js(&r, "var nt=new Animation(null,null); nt.timeline"));
    EMB_CHECK_EQ_STR(r.res, "null");
    EMB_CHECK(rig_js(&r, "var e1=new KeyframeEffect(a,[{opacity:0},{opacity:1}],100), e2=new KeyframeEffect(e1);"
                         "var p1=new Animation(e1), p2=new Animation(e2); p1.effect=e2;"
                         "[p1.effect===e2,p2.effect===null,e2.target===a,e1.target===a].join()"));
    EMB_CHECK_EQ_STR(r.res, "true,true,true,true");
    EMB_CHECK(rig_js(&r, "var id=a.animate([{opacity:0},{opacity:1}],{duration:100,id:'hello'}); id.id"));
    EMB_CHECK_EQ_STR(r.res, "hello");
    rig_teardown(&r);
}

EMB_TEST(update_playback_rate_persist_and_replace_state)
{
    rig r;
    setup_page(&r);
    EMB_CHECK(rig_js(&r, "var an=a.animate([{opacity:0},{opacity:1}],{duration:1000,fill:'forwards'});"));
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "an.updatePlaybackRate(4); an.playbackRate"));
    EMB_CHECK_EQ_STR(r.res, "1"); /* applies when the animation is ready */
    rig_frames(&r, 2, 16);
    EMB_CHECK(rig_js(&r, "an.playbackRate"));
    EMB_CHECK_EQ_STR(r.res, "4");
    /* Two forwards-filling finished animations on one property: the older one is replaced. */
    EMB_CHECK(rig_js(&r, "var w1=b.animate([{opacity:0.2},{opacity:0.3}],{duration:10,fill:'forwards'});"
                         "var w2=b.animate([{opacity:0.4},{opacity:0.5}],{duration:10,fill:'forwards'});"));
    rig_frames(&r, 6, 16);
    EMB_CHECK(rig_js(&r, "[w1.playState,w2.playState,w1.replaceState,w2.replaceState].join()"));
    EMB_CHECK_EQ_STR(r.res, "finished,finished,removed,active");
    EMB_CHECK(rig_js(&r, "w1.persist(); w1.replaceState"));
    EMB_CHECK_EQ_STR(r.res, "persisted");
    rig_teardown(&r);
}

EMB_TEST(css_events_have_names_and_bubble)
{
    rig r;
    rig_setup(&r);
    rig_load(&r, "<style>@keyframes k{from{width:10px}to{width:50px}}#a{width:10px;height:10px;animation:k 50ms linear 2}</style>"
                 "<div id=wrap><div id=a></div></div>");
    EMB_CHECK(rig_js(&r, "var wrap=document.getElementById('wrap');"
                         "['animationstart','animationiteration','animationend'].forEach(t=>wrap.addEventListener(t,e=>"
                         "console.log(t+':'+e.animationName+':'+(e instanceof AnimationEvent)+':'+e.target.id)));0"));
    rig_frames(&r, 12, 16);
    EMB_CHECK_EQ_STR(r.out.text, "animationstart:k:true:a;animationiteration:k:true:a;animationend:k:true:a;");
    rig_teardown(&r);
}

EMB_TEST(document_timeline_current_time_follows_the_frames)
{
    rig r;
    setup_page(&r);
    rig_frames(&r, 1, 16);
    EMB_CHECK(rig_js(&r, "var t0=document.timeline.currentTime; 0"));
    rig_frames(&r, 2, 10);
    double d = SDL_atof((rig_js(&r, "String(document.timeline.currentTime-t0)"), r.res));
    EMB_CHECK(d >= 19.0 && d <= 21.0);
    rig_teardown(&r);
}

EMB_TEST_MAIN()
