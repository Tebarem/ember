/*
 * The frame step of a document (W6c, frame.h): the glue between the animator (src/anim/frame.h), the script layer and the
 * binding context. The animator has no JS; the host hooks here forward its notifications to the realm when there is one (a
 * page that never ran a script has no animation objects and nobody to hear the events).
 */
#include "embed/frame.h"

#include "bindings/bind.h"
#include "embed/script_int.h"

static void host_invalidate(void *user)
{
    embi_doc_invalidate(user);
}

static embi_bind_ctx *realm_of(embi_doc *d)
{
    embi_script *s = embi_doc_script(d);
    return s != nullptr ? embi_script_bind_if(s) : nullptr;
}

static void host_notify(void *user, embi_an *a, int what, double time_ms)
{
    embi_bind_ctx *c = realm_of(user);
    if (c != nullptr) {
        embi_bind_anim_notify(c, embi_an_id(a), what, time_ms);
    }
}

static void host_css_event(void *user, embi_element *el, int kind, embi_str name, double elapsed)
{
    embi_bind_ctx *c = realm_of(user);
    if (c != nullptr) {
        embi_bind_anim_css_event(c, el, kind, name, elapsed);
    }
}

static void host_checkpoint(void *user)
{
    embi_bind_ctx *c = realm_of(user);
    if (c != nullptr) {
        embi_bind_anim_checkpoint(c);
    }
}

embi_animator *embi_frame_animator_new(embi_doc *d)
{
    embi_animator_host h = {.user = d, .invalidate = host_invalidate, .notify = host_notify, .css_event = host_css_event,
                           .checkpoint = host_checkpoint};
    return embi_animator_new(embi_doc_style_engine(d), &h);
}

void embi_doc_frame(embi_doc *d)
{
    embi_script *s = embi_doc_script(d);
    if (embi_doc_dom(d) == nullptr || s == nullptr) {
        return;
    }
    /* The tasks and timers that are due run first; their microtasks have run when they return. */
    (void)embi_script_pump(s);
    if (embi_doc_dom(d) == nullptr) {
        return;
    }
    double now = embi_script_time_ms(s);
    embi_animator *an = embi_doc_animator(d);
    embi_animator_frame(an, now);
    embi_bind_ctx *c = embi_script_bind_if(s);
    if (c != nullptr) {
        embi_bind_run_animation_frame(c, now);
    }
}

void embi_doc_frame_end(embi_doc *d)
{
    if (embi_doc_dom(d) != nullptr) {
        embi_animator_frame_end(embi_doc_animator(d));
    }
}

bool embi_doc_idle(embi_doc *d)
{
    embi_script *s = embi_doc_script(d);
    if (embi_doc_dom(d) == nullptr || s == nullptr) {
        return false;
    }
    return embi_script_idle(s, embi_animator_declared(embi_doc_animator(d)));
}

bool embi_doc_animating(embi_doc *d)
{
    if (embi_doc_dom(d) == nullptr) {
        return false;
    }
    if (embi_animator_wants_frame(embi_doc_animator(d))) {
        return true;
    }
    embi_script *s = embi_doc_script(d);
    embi_bind_ctx *c = s != nullptr ? embi_script_bind_if(s) : nullptr;
    return c != nullptr && embi_bind_raf_pending(c);
}
