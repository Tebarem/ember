/*
 * Events, AbortController and AbortSignal (W5b), and the EventTarget
 * operations that use them (addEventListener, removeEventListener, dispatchEvent, the event
 * handler attributes). The dispatch algorithm itself is src/dom/event.c; this file supplies the
 * tree (parents of the path) and the listeners of each item.
 *
 * Wrappers: an Event is a wrapper whose record ptr is the event block (embi_event, owned, freed by
 * the finalizer with its size). Its interface payload (detail, clientX, key, target, ...) is a plain
 * JS object kept in the record's ref slot (traced). AbortSignal and AbortController are wrappers
 * whose payload object holds their state; an AbortSignal's record ptr is the record itself, so that
 * the dictionary member signal of addEventListener can be read as a record.
 *
 * Listener store: each EventTarget record has bo->listeners, a flat JS array of quadruples
 *     [type (JS string), callback (JS value), flags (number: LF_*), aux (JS value)]
 * aux holds the source text of a content attribute handler (LF_CONTENT), undefined otherwise.
 * An event handler attribute (onclick) is a quadruple with LF_HANDLER; setting it to null keeps the
 * quadruple with a null callback, so that the position of the first registration is kept.
 *
 * Event path: each item is a node (KIND_NODE: the node's wrapper exists and is pinned for the
 * dispatch), a record (KIND_RECORD: the window and the other EventTarget wrappers) or an ancestor
 * element without wrapper and without a content attribute for the event (KIND_BARE: no listeners
 * possible, no wrapper made). A listener quadruple's flags number also carries the serial of the
 * registration (see LF_SERIAL_SHIFT), and aux of a plain listener holds its AbortSignal.
 *
 * Known limits: AbortSignal.any holds its dependents strongly until the source aborts (aborted
 * dependents are pruned as the deps grow).
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/events.h"
#include "bindings/gen/eventtarget.h"
#include "bindings/gen/window.h"
#include "bindings/gen/timers.h"
#include "base/clock.h"
#include "dom/event.h"
#include "bindings/activation.h"
#include "js/jobs.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/string.h"

#include <string.h>

enum { STRIDE = 4 };

enum {
    LF_CAPTURE = 1,
    LF_ONCE = 2,
    LF_PASSIVE = 4,
    LF_HANDLER = 8,
    LF_CONTENT = 16,
};

/* Path item kinds. KIND_BARE is an ancestor element that has no wrapper and no content handler for the
 * event: it can have no listeners, so no wrapper is made (nor pinned) for it. It is only touched while it
 * is still an ancestor of the target (it may be freed when a listener removes it). */
enum { KIND_NODE = 0, KIND_RECORD = 1, KIND_BARE = 2 };

/* The flags slot of a listener quadruple holds LF_* in the low 5 bits and the serial of the
 * registration above them. The serial identifies the registration: a listener removed and added
 * again during a dispatch is a different registration, and the dispatch skips the removed one. */
enum { LF_MASK = 31, LF_SERIAL_SHIFT = 32 };
static thread_local double g_serial = 0;

static bool item_alive(const struct embi_event *ev, embi_event_item it);

/* Abort dependencies (the deps array of an AbortSignal payload): quadruples
 * [kind, a, b, c]. DEP_REMOVE(_CAPTURE): remove the listener b (type) of target a with callback c.
 * DEP_PROPAGATE: abort the dependent signal a with the same reason. */
enum { DEP_REMOVE = 0, DEP_PROPAGATE = 1, DEP_REMOVE_CAPTURE = 2 };

/* ------------------------------------------------------------------ payload helpers */

static embi_js_value pget(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (embi_js_is_empty(obj) || !embi_js_is_object(obj)) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_UNDEFINED;
    }
    return v;
}

static void pset(embi_bind_ctx *c, embi_js_value obj, const char *k, embi_js_value v)
{
    if (embi_js_is_object(obj)) {
        embi_bind_object_set_c(c, obj, k, v);
    }
}

static void pset_num(embi_bind_ctx *c, embi_js_value obj, const char *k, double d)
{
    pset(c, obj, k, embi_js_number(d));
}

static double pnum(embi_bind_ctx *c, embi_js_value obj, const char *k, double def)
{
    embi_js_value v = pget(c, obj, k);
    double d = 0;
    if (embi_js_is_undefined(v)) {
        return def;
    }
    if (!embi_js_to_number(c->rt, v, &d)) {
        embi_js_exception_clear(c->rt);
        return def;
    }
    return d;
}

/* A payload value, or null when it is absent (nullable interface and any members). */
static embi_js_value pnull(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    embi_js_value v = pget(c, obj, k);
    return embi_js_is_undefined(v) ? EMBI_JS_NULL : v;
}

static embi_dstr pstr(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    embi_js_value v = pget(c, obj, k);
    embi_dstr d = embi_dstr_empty();
    if (embi_js_is_undefined(v)) {
        return d;
    }
    if (!embi_bind_to_dstr(c, v, &d)) {
        embi_js_exception_clear(c->rt);
        return embi_dstr_empty();
    }
    return d;
}

static embi_dstr lit(const char *s)
{
    return (embi_dstr){s, (uint32_t)SDL_strlen(s), false};
}

static bool str_eq(embi_js_value a, embi_js_value b)
{
    return embi_js_is_string(a) && embi_js_is_string(b) && embi_js_str_equal(embi_js_as_string(a), embi_js_as_string(b));
}

/* ------------------------------------------------------------------ event records */

static embi_event *event_of(embi_bind_ctx *c, embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    (void)c;
    if (!bo || !bo->ptr || !embi_bind_iface_is(bo->iface, EMBI_IF_Event)) {
        return nullptr;
    }
    return bo->ptr;
}

static uint32_t flags_of(const embi_dict_EventInit *e)
{
    uint32_t f = 0;
    if (e->bubbles) {
        f |= EMBI_EVF_BUBBLES;
    }
    if (e->cancelable) {
        f |= EMBI_EVF_CANCELABLE;
    }
    if (e->composed) {
        f |= EMBI_EVF_COMPOSED;
    }
    return f;
}

static embi_js_value payload_new(embi_bind_ctx *c)
{
    return embi_bind_pin(c, embi_bind_object_new(c));
}

/* A wrapper of an event of interface iface (trusted or not, per flags), with the payload object
 * (a fresh one when payload is not an object). Pending on the caller's scope. */
static embi_js_value event_make(embi_bind_ctx *c, uint16_t iface, embi_str type, uint32_t flags, embi_js_value payload)
{
    /* the time stamp is on the clock of performance.now() */
    embi_event *ev = embi_event_new(type, flags, embi_impl_Performance_now(c, nullptr));
    if (!ev) {
        return embi_js_throw_type_error(c->rt, "out of memory");
    }
    embi_js_value p = embi_js_is_object(payload) ? embi_bind_pin(c, payload) : payload_new(c);
    embi_js_value v = embi_bind_other_value(c, iface, ev, ev, (uint32_t)embi_event_size(ev));
    embi_bind_set_ref(embi_bind_obj_of(v), p);
    return v;
}

embi_js_value embi_impl_event_make(embi_bind_ctx *c, uint16_t iface, const char *type, uint32_t flags,
                                   embi_js_value payload)
{
    return event_make(c, iface, (embi_str){type, (uint32_t)SDL_strlen(type)}, flags, payload);
}

/* Sets the payload fields of the init dictionaries (the chain of the base members). */
static void set_ui(embi_bind_ctx *c, embi_js_value p, const embi_dict_UIEventInit *u)
{
    pset(c, p, "view", u->view);
    pset_num(c, p, "detail", u->detail);
    pset_num(c, p, "which", u->which);
}

static void set_mods(embi_bind_ctx *c, embi_js_value p, const embi_dict_EventModifierInit *m)
{
    set_ui(c, p, &m->base);
    pset(c, p, "ctrlKey", embi_js_from_bool(m->ctrlKey));
    pset(c, p, "shiftKey", embi_js_from_bool(m->shiftKey));
    pset(c, p, "altKey", embi_js_from_bool(m->altKey));
    pset(c, p, "metaKey", embi_js_from_bool(m->metaKey));
    pset(c, p, "modifierAltGraph", embi_js_from_bool(m->modifierAltGraph));
    pset(c, p, "modifierCapsLock", embi_js_from_bool(m->modifierCapsLock));
    pset(c, p, "modifierFn", embi_js_from_bool(m->modifierFn));
    pset(c, p, "modifierFnLock", embi_js_from_bool(m->modifierFnLock));
    pset(c, p, "modifierHyper", embi_js_from_bool(m->modifierHyper));
    pset(c, p, "modifierNumLock", embi_js_from_bool(m->modifierNumLock));
    pset(c, p, "modifierScrollLock", embi_js_from_bool(m->modifierScrollLock));
    pset(c, p, "modifierSuper", embi_js_from_bool(m->modifierSuper));
    pset(c, p, "modifierSymbol", embi_js_from_bool(m->modifierSymbol));
    pset(c, p, "modifierSymbolLock", embi_js_from_bool(m->modifierSymbolLock));
}

static void set_mouse(embi_bind_ctx *c, embi_js_value p, const embi_dict_MouseEventInit *m)
{
    set_mods(c, p, &m->base);
    pset_num(c, p, "screenX", m->screenX);
    pset_num(c, p, "screenY", m->screenY);
    pset_num(c, p, "clientX", m->clientX);
    pset_num(c, p, "clientY", m->clientY);
    pset_num(c, p, "button", m->button);
    pset_num(c, p, "buttons", m->buttons);
    pset(c, p, "relatedTarget", m->relatedTarget);
}

/* Finishes a constructed event: the prototype of new_target, and the wrapper value. */
static embi_js_value finish_ctor(embi_bind_ctx *c, embi_js_value v, embi_js_value new_target)
{
    if (v == EMBI_JS_EXCEPTION) {
        return v;
    }
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

/* ------------------------------------------------------------------ Event */

embi_js_value embi_impl_Event_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                          embi_dict_EventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = event_make(c, EMBI_IF_Event, embi_bind_utf8(c, a0), flags_of(&a1), EMBI_JS_UNDEFINED);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_Event_get_type(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_tmp_utf8(c, embi_event_type(self->ptr));
}

embi_js_value embi_impl_Event_get_target(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "target");
}

embi_js_value embi_impl_Event_get_srcElement(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "target");
}

embi_js_value embi_impl_Event_get_currentTarget(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "currentTarget");
}

embi_bind_seq embi_impl_Event_composedPath(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_event *ev = self->ptr;
    embi_bind_seq out = {nullptr, 0};
    if (!ev->path || ev->path_len == 0) {
        return out;
    }
    embi_js_value *vals = (embi_js_value *)embi_bind_alloc(c, ev->path_len * sizeof(embi_js_value));
    uint32_t w = 0;
    for (uint32_t i = 0; i < ev->path_len; i++) {
        embi_event_item it = ev->path[i];
        if (!item_alive(ev, it)) {
            continue; /* a bare ancestor that a listener removed from the target's tree */
        }
        vals[w++] = it.kind == KIND_RECORD ? embi_bind_obj_value(it.obj) : embi_bind_node_value(c, it.obj);
    }
    out.data = vals;
    out.len = w;
    return out;
}

uint16_t embi_impl_Event_get_eventPhase(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((embi_event *)self->ptr)->phase;
}

void embi_impl_Event_stopPropagation(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_event_stop_propagation(self->ptr);
}

bool embi_impl_Event_get_cancelBubble(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_STOP_PROPAGATION) != 0;
}

void embi_impl_Event_set_cancelBubble(embi_bind_ctx *c, embi_bind_obj *self, bool v)
{
    (void)c;
    if (v) {
        embi_event_stop_propagation(self->ptr);
    }
}

void embi_impl_Event_stopImmediatePropagation(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_event_stop_immediate(self->ptr);
}

bool embi_impl_Event_get_bubbles(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_BUBBLES) != 0;
}

bool embi_impl_Event_get_cancelable(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_CANCELABLE) != 0;
}

bool embi_impl_Event_get_returnValue(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_CANCELED) == 0;
}

void embi_impl_Event_set_returnValue(embi_bind_ctx *c, embi_bind_obj *self, bool v)
{
    (void)c;
    if (!v) {
        embi_event_prevent_default(self->ptr);
    }
}

void embi_impl_Event_preventDefault(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_event_prevent_default(self->ptr);
}

bool embi_impl_Event_get_defaultPrevented(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_CANCELED) != 0;
}

bool embi_impl_Event_get_composed(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_COMPOSED) != 0;
}

bool embi_impl_Event_get_isTrusted(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (((embi_event *)self->ptr)->flags & EMBI_EVF_TRUSTED) != 0;
}

double embi_impl_Event_get_timeStamp(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((embi_event *)self->ptr)->time_stamp;
}

/* initEvent: a new block with the type and the flags, in place of the old one (the wrapper's record
 * follows it). Does nothing while the event is being dispatched. */
static void event_reinit(embi_bind_ctx *c, embi_bind_obj *self, embi_str type, bool bubbles, bool cancelable)
{
    embi_event *old = self->ptr;
    if (old->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    /* The DOM Standard's initialize clears isTrusted. */
    uint32_t keep = old->flags & EMBI_EVF_COMPOSED;
    uint32_t flags = keep | (bubbles ? EMBI_EVF_BUBBLES : 0) | (cancelable ? EMBI_EVF_CANCELABLE : 0);
    embi_event *n = embi_event_new(type, flags, old->time_stamp);
    if (!n) {
        return;
    }
    size_t old_size = embi_event_size(old);
    embi_free(old, old_size);
    self->ptr = n;
    self->owned = n;
    self->owned_size = (uint32_t)embi_event_size(n);
    /* The initialize steps set the target to null. */
    pset(c, self->ref, "target", EMBI_JS_NULL);
    pset(c, self->ref, "currentTarget", EMBI_JS_NULL);
}

void embi_impl_Event_initEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2)
{
    event_reinit(c, self, embi_bind_utf8(c, a0), a1, a2);
}

/* ------------------------------------------------------------------ the subclasses */

embi_js_value embi_impl_CustomEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                embi_dict_CustomEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "detail", a1.detail);
    embi_js_value v = event_make(c, EMBI_IF_CustomEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_CustomEvent_get_detail(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "detail");
    return embi_js_is_undefined(v) ? EMBI_JS_NULL : v;
}

void embi_impl_CustomEvent_initCustomEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                           embi_js_value a3)
{
    if (((embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    event_reinit(c, self, embi_bind_utf8(c, a0), a1, a2);
    pset(c, self->ref, "detail", a3);
}

/* initUIEvent, initMouseEvent, initKeyboardEvent: the legacy initializers of createEvent('UIEvents'),
 * ('MouseEvents'), ('KeyboardEvent'). They reinitialize the event and set the payload fields. */
void embi_impl_UIEvent_initUIEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                   embi_js_value a3, int32_t a4)
{
    if (((embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    event_reinit(c, self, embi_bind_utf8(c, a0), a1, a2);
    pset(c, self->ref, "view", a3);
    pset_num(c, self->ref, "detail", a4);
}

void embi_impl_MouseEvent_initMouseEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                         embi_js_value a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7,
                                         int32_t a8, bool a9, bool a10, bool a11, bool a12, int16_t a13,
                                         embi_js_value a14)
{
    if (((embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    embi_impl_UIEvent_initUIEvent(c, self, a0, a1, a2, a3, a4);
    embi_js_value p = self->ref;
    pset_num(c, p, "screenX", a5);
    pset_num(c, p, "screenY", a6);
    pset_num(c, p, "clientX", a7);
    pset_num(c, p, "clientY", a8);
    pset(c, p, "ctrlKey", embi_js_from_bool(a9));
    pset(c, p, "altKey", embi_js_from_bool(a10));
    pset(c, p, "shiftKey", embi_js_from_bool(a11));
    pset(c, p, "metaKey", embi_js_from_bool(a12));
    pset_num(c, p, "button", a13);
    pset(c, p, "relatedTarget", a14);
}

void embi_impl_KeyboardEvent_initKeyboardEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                               embi_js_value a3, embi_dstr a4, uint32_t a5, bool a6, bool a7,
                                               bool a8, bool a9)
{
    if (((embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    embi_impl_UIEvent_initUIEvent(c, self, a0, a1, a2, a3, 0);
    embi_js_value p = self->ref;
    pset(c, p, "key", embi_bind_dstr_value(c, a4));
    pset_num(c, p, "location", a5);
    pset(c, p, "ctrlKey", embi_js_from_bool(a6));
    pset(c, p, "altKey", embi_js_from_bool(a7));
    pset(c, p, "shiftKey", embi_js_from_bool(a8));
    pset(c, p, "metaKey", embi_js_from_bool(a9));
}

embi_js_value embi_impl_UIEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                            embi_dict_UIEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_ui(c, p, &a1);
    embi_js_value v = event_make(c, EMBI_IF_UIEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_UIEvent_get_view(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "view");
}

embi_js_value embi_impl_UIEvent_get_pseudoTarget(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return EMBI_JS_NULL;
}

uint32_t embi_impl_UIEvent_get_which(embi_bind_ctx *c, embi_bind_obj *self)
{
    double w = pnum(c, self->ref, "which", 0);
    if (w == 0 && embi_bind_iface_is(self->iface, EMBI_IF_KeyboardEvent)) {
        w = pnum(c, self->ref, "keyCode", 0); /* legacy: the key code of a keyboard event */
    }
    return (uint32_t)w;
}

int32_t embi_impl_UIEvent_get_detail(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "detail", 0);
}

embi_js_value embi_impl_MouseEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_MouseEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mouse(c, p, &a1);
    embi_js_value v = event_make(c, EMBI_IF_MouseEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

int32_t embi_impl_MouseEvent_get_screenX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "screenX", 0);
}

int32_t embi_impl_MouseEvent_get_screenY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "screenY", 0);
}

int32_t embi_impl_MouseEvent_get_clientX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "clientX", 0);
}

int32_t embi_impl_MouseEvent_get_clientY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "clientY", 0);
}

/* pageX, offsetX: the page and element coordinates the platform layer stores; the client
 * coordinates when none were stored (the view has no scroll offset of its own here). */
double embi_impl_MouseEvent_get_pageX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "pageX", pnum(c, self->ref, "clientX", 0));
}

double embi_impl_MouseEvent_get_pageY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "pageY", pnum(c, self->ref, "clientY", 0));
}

double embi_impl_MouseEvent_get_offsetX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "offsetX", pnum(c, self->ref, "clientX", 0));
}

double embi_impl_MouseEvent_get_offsetY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "offsetY", pnum(c, self->ref, "clientY", 0));
}

double embi_impl_MouseEvent_get_x(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "clientX", 0);
}

double embi_impl_MouseEvent_get_y(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "clientY", 0);
}

bool embi_impl_MouseEvent_get_ctrlKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "ctrlKey"));
}

bool embi_impl_MouseEvent_get_shiftKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "shiftKey"));
}

bool embi_impl_MouseEvent_get_altKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "altKey"));
}

bool embi_impl_MouseEvent_get_metaKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "metaKey"));
}

int16_t embi_impl_MouseEvent_get_button(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int16_t)pnum(c, self->ref, "button", 0);
}

uint16_t embi_impl_MouseEvent_get_buttons(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint16_t)pnum(c, self->ref, "buttons", 0);
}

embi_js_value embi_impl_MouseEvent_get_relatedTarget(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "relatedTarget");
}

/* The modifier names of getModifierState and their keys in the payload. */
static bool modifier_state(embi_bind_ctx *c, embi_js_value p, embi_dstr key)
{
    static const struct {
        const char *name;
        const char *field;
    } names[] = {
        {"Control", "ctrlKey"},
        {"Shift", "shiftKey"},
        {"Alt", "altKey"},
        {"Meta", "metaKey"},
        {"OS", "metaKey"},
        {"AltGraph", "modifierAltGraph"},
        {"CapsLock", "modifierCapsLock"},
        {"Fn", "modifierFn"},
        {"FnLock", "modifierFnLock"},
        {"Hyper", "modifierHyper"},
        {"NumLock", "modifierNumLock"},
        {"ScrollLock", "modifierScrollLock"},
        {"Super", "modifierSuper"},
        {"Symbol", "modifierSymbol"},
        {"SymbolLock", "modifierSymbolLock"},
    };
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (embi_dstr_eq_utf8(key, (embi_str){names[i].name, (uint32_t)SDL_strlen(names[i].name)})) {
            return embi_js_to_boolean(pget(c, p, names[i].field));
        }
    }
    return false;
}

bool embi_impl_MouseEvent_getModifierState(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    return modifier_state(c, self->ref, a0);
}

embi_js_value embi_impl_PointerEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                 embi_dict_PointerEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mouse(c, p, &a1.base);
    pset_num(c, p, "pointerId", a1.pointerId);
    pset_num(c, p, "width", a1.width);
    pset_num(c, p, "height", a1.height);
    pset_num(c, p, "pressure", a1.pressure);
    pset_num(c, p, "tangentialPressure", a1.tangentialPressure);
    pset_num(c, p, "tiltX", a1.tiltX);
    pset_num(c, p, "tiltY", a1.tiltY);
    pset_num(c, p, "twist", a1.twist);
    pset(c, p, "pointerType", embi_bind_dstr_value(c, a1.pointerType));
    pset(c, p, "isPrimary", embi_js_from_bool(a1.isPrimary));
    embi_js_value v = event_make(c, EMBI_IF_PointerEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

int32_t embi_impl_PointerEvent_get_pointerId(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "pointerId", 0);
}

double embi_impl_PointerEvent_get_width(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "width", 1);
}

double embi_impl_PointerEvent_get_height(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "height", 1);
}

double embi_impl_PointerEvent_get_pressure(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "pressure", 0);
}

double embi_impl_PointerEvent_get_tangentialPressure(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "tangentialPressure", 0);
}

int32_t embi_impl_PointerEvent_get_tiltX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "tiltX", 0);
}

int32_t embi_impl_PointerEvent_get_tiltY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "tiltY", 0);
}

int32_t embi_impl_PointerEvent_get_twist(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (int32_t)pnum(c, self->ref, "twist", 0);
}

embi_dstr embi_impl_PointerEvent_get_pointerType(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "pointerType");
}

bool embi_impl_PointerEvent_get_isPrimary(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "isPrimary"));
}

embi_js_value embi_impl_WheelEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_WheelEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mouse(c, p, &a1.base);
    pset_num(c, p, "deltaX", a1.deltaX);
    pset_num(c, p, "deltaY", a1.deltaY);
    pset_num(c, p, "deltaZ", a1.deltaZ);
    pset_num(c, p, "deltaMode", a1.deltaMode);
    embi_js_value v = event_make(c, EMBI_IF_WheelEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

double embi_impl_WheelEvent_get_deltaX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "deltaX", 0);
}

double embi_impl_WheelEvent_get_deltaY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "deltaY", 0);
}

double embi_impl_WheelEvent_get_deltaZ(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnum(c, self->ref, "deltaZ", 0);
}

uint32_t embi_impl_WheelEvent_get_deltaMode(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "deltaMode", 0);
}

embi_js_value embi_impl_KeyboardEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                  embi_dict_KeyboardEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mods(c, p, &a1.base);
    pset(c, p, "key", embi_bind_dstr_value(c, a1.key));
    pset(c, p, "code", embi_bind_dstr_value(c, a1.code));
    pset_num(c, p, "location", a1.location);
    pset(c, p, "repeat", embi_js_from_bool(a1.repeat));
    pset(c, p, "isComposing", embi_js_from_bool(a1.isComposing));
    pset_num(c, p, "charCode", a1.charCode);
    pset_num(c, p, "keyCode", a1.keyCode);
    embi_js_value v = event_make(c, EMBI_IF_KeyboardEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_KeyboardEvent_get_key(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "key");
}

embi_dstr embi_impl_KeyboardEvent_get_code(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "code");
}

uint32_t embi_impl_KeyboardEvent_get_location(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "location", 0);
}

bool embi_impl_KeyboardEvent_get_ctrlKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "ctrlKey"));
}

bool embi_impl_KeyboardEvent_get_shiftKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "shiftKey"));
}

bool embi_impl_KeyboardEvent_get_altKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "altKey"));
}

bool embi_impl_KeyboardEvent_get_metaKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "metaKey"));
}

bool embi_impl_KeyboardEvent_get_repeat(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "repeat"));
}

bool embi_impl_KeyboardEvent_get_isComposing(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "isComposing"));
}

uint32_t embi_impl_KeyboardEvent_get_charCode(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "charCode", 0);
}

uint32_t embi_impl_KeyboardEvent_get_keyCode(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "keyCode", 0);
}

bool embi_impl_KeyboardEvent_getModifierState(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    return modifier_state(c, self->ref, a0);
}

embi_js_value embi_impl_DragEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                              embi_dict_DragEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mouse(c, p, &a1.base);
    pset(c, p, "dataTransfer", a1.dataTransfer);
    embi_js_value v = event_make(c, EMBI_IF_DragEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_DragEvent_get_dataTransfer(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "dataTransfer");
}

embi_dstr embi_impl_TextEvent_get_data(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "data");
}

void embi_impl_TextEvent_initTextEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                       embi_js_value a3, embi_dstr a4)
{
    if (((embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) {
        return;
    }
    embi_impl_UIEvent_initUIEvent(c, self, a0, a1, a2, a3, 0);
    pset(c, self->ref, "data", embi_bind_dstr_value(c, a4));
}

embi_js_value embi_impl_TouchEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_TouchEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_mods(c, p, &a1.base);
    pset(c, p, "touches", a1.touches);
    pset(c, p, "targetTouches", a1.targetTouches);
    pset(c, p, "changedTouches", a1.changedTouches);
    embi_js_value v = event_make(c, EMBI_IF_TouchEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_TouchEvent_get_touches(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "touches");
}

embi_js_value embi_impl_TouchEvent_get_targetTouches(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "targetTouches");
}

embi_js_value embi_impl_TouchEvent_get_changedTouches(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "changedTouches");
}

bool embi_impl_TouchEvent_get_ctrlKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "ctrlKey"));
}

bool embi_impl_TouchEvent_get_shiftKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "shiftKey"));
}

bool embi_impl_TouchEvent_get_altKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "altKey"));
}

bool embi_impl_TouchEvent_get_metaKey(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "metaKey"));
}

embi_js_value embi_impl_FocusEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_FocusEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_ui(c, p, &a1.base);
    pset(c, p, "relatedTarget", a1.relatedTarget);
    embi_js_value v = event_make(c, EMBI_IF_FocusEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_FocusEvent_get_relatedTarget(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "relatedTarget");
}

embi_js_value embi_impl_InputEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_InputEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_ui(c, p, &a1.base);
    pset(c, p, "data", a1.data.is_null ? EMBI_JS_NULL : embi_bind_dstr_value(c, a1.data.s));
    pset(c, p, "isComposing", embi_js_from_bool(a1.isComposing));
    pset(c, p, "inputType", embi_bind_dstr_value(c, a1.inputType));
    embi_js_value v = event_make(c, EMBI_IF_InputEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_InputEvent_get_data(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "data");
}

bool embi_impl_InputEvent_get_isComposing(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "isComposing"));
}

embi_dstr embi_impl_InputEvent_get_inputType(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "inputType");
}

embi_js_value embi_impl_CompositionEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                     embi_dict_CompositionEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    set_ui(c, p, &a1.base);
    pset(c, p, "data", embi_bind_dstr_value(c, a1.data));
    embi_js_value v = event_make(c, EMBI_IF_CompositionEvent, embi_bind_utf8(c, a0), flags_of(&a1.base.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_CompositionEvent_get_data(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "data");
}

embi_js_value embi_impl_ClipboardEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                   embi_dict_ClipboardEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "clipboardData", a1.clipboardData);
    embi_js_value v = event_make(c, EMBI_IF_ClipboardEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_ClipboardEvent_get_clipboardData(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "clipboardData");
}

embi_js_value embi_impl_SubmitEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                embi_dict_SubmitEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "submitter", a1.submitter);
    embi_js_value v = event_make(c, EMBI_IF_SubmitEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_SubmitEvent_get_submitter(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pnull(c, self->ref, "submitter");
}

embi_js_value embi_impl_ErrorEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                               embi_dict_ErrorEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "message", embi_bind_dstr_value(c, a1.message));
    pset(c, p, "filename", embi_bind_dstr_value(c, a1.filename));
    pset_num(c, p, "lineno", a1.lineno);
    pset_num(c, p, "colno", a1.colno);
    pset(c, p, "error", a1.error);
    embi_js_value v = event_make(c, EMBI_IF_ErrorEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_ErrorEvent_get_message(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "message");
}

embi_dstr embi_impl_ErrorEvent_get_filename(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "filename");
}

uint32_t embi_impl_ErrorEvent_get_lineno(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "lineno", 0);
}

uint32_t embi_impl_ErrorEvent_get_colno(embi_bind_ctx *c, embi_bind_obj *self)
{
    return (uint32_t)pnum(c, self->ref, "colno", 0);
}

embi_js_value embi_impl_ErrorEvent_get_error(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "error");
    return embi_js_is_undefined(v) ? EMBI_JS_NULL : v;
}

embi_js_value embi_impl_PromiseRejectionEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                          embi_dict_PromiseRejectionEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "promise", a1.promise);
    pset(c, p, "reason", a1.reason);
    embi_js_value v = event_make(c, EMBI_IF_PromiseRejectionEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_PromiseRejectionEvent_get_promise(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "promise");
}

embi_js_value embi_impl_PromiseRejectionEvent_get_reason(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "reason");
}

embi_js_value embi_impl_PopStateEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                  embi_dict_PopStateEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "state", a1.state);
    embi_js_value v = event_make(c, EMBI_IF_PopStateEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_js_value embi_impl_PopStateEvent_get_state(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "state");
    return embi_js_is_undefined(v) ? EMBI_JS_NULL : v;
}

embi_js_value embi_impl_MediaQueryListEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                         embi_dict_MediaQueryListEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "media", embi_bind_dstr_value(c, a1.media));
    pset(c, p, "matches", embi_js_from_bool(a1.matches));
    embi_js_value v = event_make(c, EMBI_IF_MediaQueryListEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_MediaQueryListEvent_get_media(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "media");
}

bool embi_impl_MediaQueryListEvent_get_matches(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "matches"));
}

embi_js_value embi_impl_HashChangeEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                    embi_dict_HashChangeEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "oldURL", embi_bind_dstr_value(c, a1.oldURL));
    pset(c, p, "newURL", embi_bind_dstr_value(c, a1.newURL));
    embi_js_value v = event_make(c, EMBI_IF_HashChangeEvent, embi_bind_utf8(c, a0), flags_of(&a1.base), p);
    return embi_bind_leave(sc, finish_ctor(c, v, new_target));
}

embi_dstr embi_impl_HashChangeEvent_get_oldURL(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "oldURL");
}

embi_dstr embi_impl_HashChangeEvent_get_newURL(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "newURL");
}

/* ------------------------------------------------------------------ createEvent */

/* Document.createEvent: an event of the interface named by name, not yet initialized. */
embi_dom_err embi_impl_create_event(embi_bind_ctx *c, embi_dstr name, embi_js_value *out)
{
    static const struct {
        const char *name;
        uint16_t iface;
    } table[] = {
        {"Event", EMBI_IF_Event},
        {"Events", EMBI_IF_Event},
        {"HTMLEvents", EMBI_IF_Event},
        {"CustomEvent", EMBI_IF_CustomEvent},
        {"UIEvent", EMBI_IF_UIEvent},
        {"UIEvents", EMBI_IF_UIEvent},
        {"MouseEvent", EMBI_IF_MouseEvent},
        {"MouseEvents", EMBI_IF_MouseEvent},
        {"PointerEvent", EMBI_IF_PointerEvent},
        {"DragEvent", EMBI_IF_DragEvent},
        {"TextEvent", EMBI_IF_TextEvent},
        {"TouchEvent", EMBI_IF_TouchEvent},
        {"WheelEvent", EMBI_IF_WheelEvent},
        {"KeyboardEvent", EMBI_IF_KeyboardEvent},
        {"KeyboardEvents", EMBI_IF_KeyboardEvent},
        {"FocusEvent", EMBI_IF_FocusEvent},
        {"FocusEvents", EMBI_IF_FocusEvent},
        {"InputEvent", EMBI_IF_InputEvent},
        {"CompositionEvent", EMBI_IF_CompositionEvent},
        {"ClipboardEvent", EMBI_IF_ClipboardEvent},
        {"ErrorEvent", EMBI_IF_ErrorEvent},
        {"HashChangeEvent", EMBI_IF_HashChangeEvent},
        {"PopStateEvent", EMBI_IF_PopStateEvent},
        {"SubmitEvent", EMBI_IF_SubmitEvent},
        /* Interfaces Ember does not have (platform events): an Event, which the standard also does for "SVGEvents". */
        {"BeforeUnloadEvent", EMBI_IF_Event},
        {"DeviceMotionEvent", EMBI_IF_Event},
        {"DeviceOrientationEvent", EMBI_IF_Event},
        {"MessageEvent", EMBI_IF_MessageEvent},
        {"StorageEvent", EMBI_IF_Event},
        {"SVGEvents", EMBI_IF_Event},
    };
    for (uint32_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (embi_dstr_ieq_utf8(name, (embi_str){table[i].name, (uint32_t)SDL_strlen(table[i].name)})) {
            embi_js_value p = payload_new(c);
            embi_js_value v = event_make(c, table[i].iface, (embi_str){nullptr, 0}, 0, p);
            if (v == EMBI_JS_EXCEPTION) {
                return EMBI_BIND_THREW;
            }
            /* created but not initialized, and not trusted */
            embi_event *ev = embi_bind_obj_of(v)->ptr;
            ev->flags &= ~EMBI_EVF_INITIALIZED;
            *out = v;
            return EMBI_DOM_OK;
        }
    }
    return EMBI_DOM_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ listeners */

/* The listener list of a record, created on demand. */
static embi_js_value listeners_of(embi_bind_ctx *c, embi_bind_obj *bo, bool create)
{
    if (embi_js_is_empty(bo->listeners) && create) {
        embi_bind_set_listeners(bo, embi_bind_array_new(c, 0));
    }
    return bo->listeners;
}

static double lraw(embi_bind_ctx *c, embi_js_value list, uint32_t i)
{
    double d = 0;
    if (!embi_js_to_number(c->rt, embi_bind_array_get(c, list, i + 2), &d)) {
        embi_js_exception_clear(c->rt);
    }
    return d;
}

static int32_t lflags(embi_bind_ctx *c, embi_js_value list, uint32_t i)
{
    return (int32_t)((uint64_t)lraw(c, list, i) % LF_SERIAL_SHIFT);
}

static double lserial(embi_bind_ctx *c, embi_js_value list, uint32_t i)
{
    return (double)((uint64_t)lraw(c, list, i) / LF_SERIAL_SHIFT);
}

/* Index of the registration with this serial, or -1. */
static int32_t find_serial(embi_bind_ctx *c, embi_js_value list, double serial)
{
    if (!embi_js_is_object(list)) {
        return -1;
    }
    uint32_t n = embi_bind_array_len(c, list);
    for (uint32_t i = 0; i + STRIDE <= n; i += STRIDE) {
        if (lserial(c, list, i) == serial) {
            return (int32_t)i;
        }
    }
    return -1;
}

static double flags_with_serial(int32_t flags, double serial)
{
    return (double)(uint32_t)flags + serial * LF_SERIAL_SHIFT;
}

/* Index of the quadruple with this type, callback and capture/handler kind, or -1. */
static int32_t find_listener(embi_bind_ctx *c, embi_js_value list, embi_js_value type, embi_js_value cb, bool capture,
                             bool handler)
{
    if (!embi_js_is_object(list)) {
        return -1;
    }
    uint32_t n = embi_bind_array_len(c, list);
    for (uint32_t i = 0; i + STRIDE <= n; i += STRIDE) {
        if (!str_eq(embi_bind_array_get(c, list, i), type) || embi_bind_array_get(c, list, i + 1) != cb) {
            continue;
        }
        int32_t fl = lflags(c, list, i);
        if (((fl & LF_CAPTURE) != 0) == capture && ((fl & LF_HANDLER) != 0) == handler) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_handler(embi_bind_ctx *c, embi_js_value list, embi_js_value type)
{
    if (!embi_js_is_object(list)) {
        return -1;
    }
    uint32_t n = embi_bind_array_len(c, list);
    for (uint32_t i = 0; i + STRIDE <= n; i += STRIDE) {
        if (str_eq(embi_bind_array_get(c, list, i), type) && (lflags(c, list, i) & LF_HANDLER)) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Removes the quadruple at idx. Works on a copy, so that a dispatch running over the old list is
 * not disturbed. */
static void list_remove_at(embi_bind_ctx *c, embi_bind_obj *bo, uint32_t idx)
{
    embi_js_value old = bo->listeners;
    uint32_t n = embi_bind_array_len(c, old);
    embi_js_value nl = embi_bind_pin(c, embi_bind_array_new(c, 0));
    uint32_t w = 0;
    for (uint32_t k = 0; k + STRIDE <= n; k += STRIDE) {
        if (k == idx) {
            continue;
        }
        for (uint32_t q = 0; q < STRIDE; q++) {
            embi_bind_array_set(c, nl, w++, embi_bind_array_get(c, old, k + q));
        }
    }
    embi_bind_set_listeners(bo, nl);
}

/* Abort dependencies, see DEP_*. */
static void deps_forget(embi_bind_ctx *c, embi_js_value sig_v, embi_js_value target_v, embi_js_value type,
                        embi_js_value cb, bool capture);

/* aux of a listener quadruple is the AbortSignal that removes it (addEventListener's signal), if any. */
static bool listener_add(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value type, embi_js_value cb, int32_t flags,
                         embi_js_value aux)
{
    embi_js_value list = listeners_of(c, bo, true);
    if (find_listener(c, list, type, cb, (flags & LF_CAPTURE) != 0, (flags & LF_HANDLER) != 0) >= 0) {
        return false;
    }
    uint32_t n = embi_bind_array_len(c, list);
    embi_bind_array_set(c, list, n, type);
    embi_bind_array_set(c, list, n + 1, cb);
    embi_bind_array_set(c, list, n + 2, embi_js_number(flags_with_serial(flags, ++g_serial)));
    embi_bind_array_set(c, list, n + 3, aux);
    return true;
}

/* Removes the registration at idx, and its entry in the deps of the signal that would remove it. */
static void listener_drop(embi_bind_ctx *c, embi_bind_obj *bo, uint32_t idx)
{
    embi_js_value list = bo->listeners;
    int32_t fl = lflags(c, list, idx);
    embi_js_value aux = embi_bind_array_get(c, list, idx + 3);
    if (!(fl & LF_HANDLER) && embi_js_is_object(aux)) {
        embi_js_value target_v = embi_bind_pin(c, embi_bind_obj_value(bo));
        deps_forget(c, aux, target_v, embi_bind_array_get(c, list, idx), embi_bind_array_get(c, list, idx + 1),
                    (fl & LF_CAPTURE) != 0);
    }
    list_remove_at(c, bo, idx);
}

static void listener_remove(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value type, embi_js_value cb, bool capture)
{
    if (!embi_js_is_object(bo->listeners)) {
        return;
    }
    int32_t idx = find_listener(c, bo->listeners, type, cb, capture, false);
    if (idx >= 0) {
        listener_drop(c, bo, (uint32_t)idx);
    }
}

/* Stores a handler (an event handler attribute or a content attribute), keeping the position of
 * its first registration. */
static void handler_store(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value type, embi_js_value cb, int32_t flags,
                          embi_js_value aux)
{
    embi_js_value list = listeners_of(c, bo, true);
    int32_t idx = find_handler(c, list, type);
    if (idx >= 0) {
        embi_bind_array_set(c, list, (uint32_t)idx + 1, cb);
        embi_bind_array_set(c, list, (uint32_t)idx + 2,
                            embi_js_number(flags_with_serial(flags, lserial(c, list, (uint32_t)idx))));
        embi_bind_array_set(c, list, (uint32_t)idx + 3, aux);
        return;
    }
    uint32_t n = embi_bind_array_len(c, list);
    embi_bind_array_set(c, list, n, type);
    embi_bind_array_set(c, list, n + 1, cb);
    embi_bind_array_set(c, list, n + 2, embi_js_number(flags_with_serial(flags, ++g_serial)));
    embi_bind_array_set(c, list, n + 3, aux);
}

/* ------------------------------------------------------------------ dispatch */

static void report_exception(embi_bind_ctx *c);
static embi_js_value compile_content_handler(embi_bind_ctx *c, embi_node *el, embi_str body);
static embi_str on_name(embi_bind_ctx *c, embi_str type);

bool embi_impl_dispatch_value(embi_bind_ctx *c, embi_bind_obj *target, embi_js_value ev_v);

/* The event handler of a content attribute: function (event) { with (document) with (form)
 * with (element) { body } }, called with this = element. */
static embi_js_value compile_content_handler(embi_bind_ctx *c, embi_node *el, embi_str body)
{
    static const char head[] = "(function (element) { return function (event) { "
                               "with (document) { with (element.form || {}) { with (element) {\n";
    static const char tail[] = "\n} } } }; })";
    size_t hn = sizeof head - 1, tn = sizeof tail - 1;
    char *src = embi_bind_alloc(c, hn + body.len + tn);
    SDL_memcpy(src, head, hn);
    if (body.len) {
        SDL_memcpy(src + hn, body.ptr, body.len);
    }
    SDL_memcpy(src + hn + body.len, tail, tn);
    embi_js_value outer = embi_js_eval(c->realm, src, hn + body.len + tn, "event handler", 0);
    if (outer == EMBI_JS_EXCEPTION) {
        report_exception(c);
        return EMBI_JS_NULL;
    }
    embi_js_value el_v = embi_bind_pin(c, embi_bind_node_value(c, el));
    embi_js_value fn = embi_js_call(c->rt, outer, EMBI_JS_UNDEFINED, 1, &el_v);
    if (fn == EMBI_JS_EXCEPTION) {
        report_exception(c);
        return EMBI_JS_NULL;
    }
    return fn;
}

/* The attribute name "on" + type, in scratch memory of the current scope. */
static embi_str on_name(embi_bind_ctx *c, embi_str type)
{
    char *name = embi_bind_alloc(c, (size_t)type.len + 3);
    name[0] = 'o';
    name[1] = 'n';
    if (type.len) {
        SDL_memcpy(name + 2, type.ptr, type.len);
    }
    name[type.len + 2] = '\0';
    return (embi_str){name, type.len + 2};
}

/* Brings the content attribute handler of an element record up to date with its attribute: the
 * handler is compiled when the attribute appears or changes, and dropped when it goes away. A
 * handler set from script remembers the attribute source it saw (aux); when the attribute differs
 * from that, the attribute wins again. name is "on" + the event type. */
static void sync_content_attr(embi_bind_ctx *c, embi_bind_obj *bo, const embi_attr *a, embi_js_value type_v)
{
    embi_element *el = bo->ptr;
    int32_t idx = find_handler(c, bo->listeners, type_v);
    if (!a) {
        if (idx >= 0 && ((lflags(c, bo->listeners, (uint32_t)idx) & LF_CONTENT) ||
                         !embi_js_is_undefined(embi_bind_array_get(c, bo->listeners, (uint32_t)idx + 3)))) {
            list_remove_at(c, bo, (uint32_t)idx); /* the attribute went away */
        }
        return;
    }
    embi_str src = embi_bind_utf8(c, a->value);
    embi_js_value src_v = embi_bind_pin(c, embi_bind_utf8_value(c, src));
    if (idx >= 0) {
        int32_t fl = lflags(c, bo->listeners, (uint32_t)idx);
        embi_js_value seen = embi_bind_array_get(c, bo->listeners, (uint32_t)idx + 3);
        if (str_eq(seen, src_v)) {
            return;
        }
        (void)fl; /* a script-set handler that saw another attribute value (or none) is replaced as well */
    }
    embi_js_value fn = compile_content_handler(c, &el->base, src);
    handler_store(c, bo, type_v, fn, LF_HANDLER | LF_CONTENT, src_v);
}

static void sync_content(embi_bind_ctx *c, embi_bind_obj *bo, embi_str name, embi_js_value type_v)
{
    if (!bo->is_node || !bo->ptr || ((embi_node *)bo->ptr)->type != EMBI_NODE_ELEMENT) {
        return;
    }
    embi_element *el = bo->ptr;
    if (!el->attrs && !embi_js_is_object(bo->listeners)) {
        return; /* no attributes, no listeners: nothing to compile or drop */
    }
    sync_content_attr(c, bo, el->attrs ? embi_el_get_attribute(el, name) : nullptr, type_v);
}

/* The body's content attribute handlers of the window events (HTML section 8.1.8.2: onload, onerror, onresize and the like on the body
 * element set the window's handler). Synced before the event fires at the window; the handler runs with the body as its element. */
void embi_impl_sync_body_window_handler(embi_bind_ctx *c, const char *type)
{
    embi_element *html = embi_document_element(c->doc);
    embi_element *body = nullptr;
    for (embi_node *k = html != nullptr ? html->base.first_child : nullptr; k != nullptr && body == nullptr; k = k->next) {
        if (k->type == EMBI_NODE_ELEMENT && embi_str_ieq(embi_atom_str(k->name), EMBI_STR("body"))) {
            body = embi_as_element(k);
        }
    }
    embi_bind_obj *w = c->gbo;
    if (body == nullptr || w == nullptr || body->attrs == nullptr) {
        return;
    }
    embi_str ts = {type, (uint32_t)SDL_strlen(type)};
    embi_js_value type_v = embi_bind_pin(c, embi_bind_utf8_value(c, ts));
    const embi_attr *a = embi_el_get_attribute(body, on_name(c, ts));
    if (a == nullptr || find_handler(c, w->listeners, type_v) >= 0) {
        return;
    }
    embi_str src = embi_bind_utf8(c, a->value);
    embi_js_value src_v = embi_bind_pin(c, embi_bind_utf8_value(c, src));
    embi_js_value fn = compile_content_handler(c, &body->base, src);
    handler_store(c, w, type_v, fn, LF_HANDLER | LF_CONTENT, src_v);
}

static embi_js_value handler_get(embi_bind_ctx *c, embi_bind_obj *bo, embi_str type)
{
    if (embi_bind_stale(bo)) {
        return EMBI_JS_NULL;
    }
    embi_js_value type_v = embi_bind_pin(c, embi_bind_utf8_value(c, type));
    sync_content(c, bo, on_name(c, type), type_v);
    int32_t idx = find_handler(c, bo->listeners, type_v);
    if (idx < 0) {
        return EMBI_JS_NULL;
    }
    embi_js_value cb = embi_bind_array_get(c, bo->listeners, (uint32_t)idx + 1);
    return embi_js_is_callable(cb) ? cb : EMBI_JS_NULL;
}

static void handler_set(embi_bind_ctx *c, embi_bind_obj *bo, embi_str type, embi_js_value v)
{
    if (embi_bind_stale(bo)) {
        return;
    }
    embi_js_value type_v = embi_bind_pin(c, embi_bind_utf8_value(c, type));
    embi_js_value cb = embi_js_is_callable(v) ? v : EMBI_JS_NULL;
    /* Remember the attribute source this handler overrides, so that a later change of the attribute wins. */
    embi_js_value seen = EMBI_JS_UNDEFINED;
    if (bo->is_node && bo->ptr && ((embi_node *)bo->ptr)->type == EMBI_NODE_ELEMENT) {
        embi_element *el = bo->ptr;
        const embi_attr *a = el->attrs ? embi_el_get_attribute(el, on_name(c, type)) : nullptr;
        if (a) {
            seen = embi_bind_pin(c, embi_bind_utf8_value(c, embi_bind_utf8(c, a->value)));
        }
    }
    handler_store(c, bo, type_v, cb, LF_HANDLER, seen);
}

#define EVENT_HANDLER(prop, type_name)                                                                          \
    embi_js_value embi_impl_EventTarget_get_##prop(embi_bind_ctx *c, embi_bind_obj *self)                      \
    {                                                                                                          \
        return handler_get(c, self, EMBI_STR(type_name));                                                      \
    }                                                                                                          \
    void embi_impl_EventTarget_set_##prop(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value v)              \
    {                                                                                                          \
        handler_set(c, self, EMBI_STR(type_name), v);                                                          \
    }

EVENT_HANDLER(onabort, "abort")
EVENT_HANDLER(onauxclick, "auxclick")
EVENT_HANDLER(onbeforeinput, "beforeinput")
EVENT_HANDLER(onblur, "blur")
EVENT_HANDLER(onchange, "change")
EVENT_HANDLER(onclick, "click")
EVENT_HANDLER(oncompositionend, "compositionend")
EVENT_HANDLER(oncompositionstart, "compositionstart")
EVENT_HANDLER(oncompositionupdate, "compositionupdate")
EVENT_HANDLER(oncontextmenu, "contextmenu")
EVENT_HANDLER(oncopy, "copy")
EVENT_HANDLER(oncut, "cut")
EVENT_HANDLER(ondblclick, "dblclick")
EVENT_HANDLER(onerror, "error")
EVENT_HANDLER(onfocus, "focus")
EVENT_HANDLER(onfocusin, "focusin")
EVENT_HANDLER(onfocusout, "focusout")
EVENT_HANDLER(onhashchange, "hashchange")
EVENT_HANDLER(oninput, "input")
EVENT_HANDLER(onkeydown, "keydown")
EVENT_HANDLER(onkeypress, "keypress")
EVENT_HANDLER(onkeyup, "keyup")
EVENT_HANDLER(onload, "load")
EVENT_HANDLER(onmessage, "message")
EVENT_HANDLER(onmessageerror, "messageerror")
EVENT_HANDLER(onopen, "open")
EVENT_HANDLER(onclose, "close")
EVENT_HANDLER(onmousedown, "mousedown")
EVENT_HANDLER(onmouseenter, "mouseenter")
EVENT_HANDLER(onmouseleave, "mouseleave")
EVENT_HANDLER(onmousemove, "mousemove")
EVENT_HANDLER(onmouseout, "mouseout")
EVENT_HANDLER(onmouseover, "mouseover")
EVENT_HANDLER(onmouseup, "mouseup")
EVENT_HANDLER(onpaste, "paste")
EVENT_HANDLER(onpointercancel, "pointercancel")
EVENT_HANDLER(onpointerdown, "pointerdown")
EVENT_HANDLER(onpointerenter, "pointerenter")
EVENT_HANDLER(onpointerleave, "pointerleave")
EVENT_HANDLER(onpointermove, "pointermove")
EVENT_HANDLER(onpointerout, "pointerout")
EVENT_HANDLER(onpointerover, "pointerover")
EVENT_HANDLER(onpointerup, "pointerup")
EVENT_HANDLER(onpopstate, "popstate")
EVENT_HANDLER(onreset, "reset")
EVENT_HANDLER(onresize, "resize")
EVENT_HANDLER(onscroll, "scroll")
EVENT_HANDLER(onsubmit, "submit")
EVENT_HANDLER(onunhandledrejection, "unhandledrejection")
EVENT_HANDLER(onwheel, "wheel")

/* Reports an exception as an uncaught error: an ErrorEvent at the window (cancelable); when no
 * listener cancels it, the console gets the message. */
static void report_error_value(embi_bind_ctx *c, embi_js_value err)
{
    static thread_local int depth = 0;
    if (depth > 4) {
        embi_bind_log(c, EMBI_BIND_LOG_ERROR, EMBI_STR("Uncaught exception while reporting an error"));
        return;
    }
    depth++;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_dstr msg = embi_dstr_empty();
    if (!embi_bind_to_dstr(c, err, &msg)) {
        embi_js_exception_clear(c->rt);
        msg = lit("exception");
    }
    embi_js_value p = payload_new(c);
    pset(c, p, "message", embi_bind_dstr_value(c, msg));
    pset(c, p, "error", err);
    embi_js_value ev = event_make(c, EMBI_IF_ErrorEvent, EMBI_STR("error"), EMBI_EVF_CANCELABLE | EMBI_EVF_TRUSTED, p);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        depth--;
        return;
    }
    bool ok = embi_impl_dispatch_value(c, c->gbo, ev);
    if (ok) {
        embi_bind_log(c, EMBI_BIND_LOG_ERROR, embi_bind_utf8(c, msg));
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    depth--;
}

static void report_exception(embi_bind_ctx *c)
{
    embi_js_value err = embi_js_exception_get(c->rt);
    embi_js_exception_clear(c->rt);
    report_error_value(c, err);
}

/* Context of one dispatch. */
typedef struct dctx {
    embi_bind_ctx *c;
    embi_bind_obj *ev_bo;   /* the Event wrapper */
    embi_js_value type_v;   /* the event type as a JS string */
    embi_str on_name;       /* "on" + the event type, for content attributes (scratch memory of the dispatch) */
    bool activation_event;  /* a MouseEvent of type click: activation behaviors apply */
    uint8_t on_kind;        /* CF1: 0 not looked at yet, 1 on_name has no colon and no ASCII upper case, 2 other */
    embi_atom on_atom;      /* CF1: the atom of on_name (0 when none exists: no attribute has that name) */
} dctx;

/* The content attribute on<type> of el, as embi_el_get_attribute(el, d->on_name) finds it. CF1: a name without a colon
 * and without upper case is neither folded nor split, so the attribute is the one without a prefix whose local name is
 * its atom, looked up once per dispatch. */
static const embi_attr *on_attr(dctx *d, embi_element *el)
{
    if (el->attrs == nullptr) {
        return nullptr;
    }
    if (d->on_kind == 0) {
        bool simple = true;
        for (uint32_t i = 0; i < d->on_name.len && simple; i++) {
            char ch = d->on_name.ptr[i];
            simple = ch != ':' && !(ch >= 'A' && ch <= 'Z');
        }
        d->on_kind = simple ? 1 : 2;
        d->on_atom = simple ? embi_atom_find(d->on_name.ptr, d->on_name.len) : 0;
    }
    if (d->on_kind == 2) {
        return embi_el_get_attribute(el, d->on_name);
    }
    if (d->on_atom == 0) {
        return nullptr;
    }
    uint32_t n = embi_el_attr_count(el);
    for (uint32_t i = 0; i < n; i++) {
        const embi_attr *at = embi_el_attr_at(el, i);
        if (at->prefix == 0 && at->name == d->on_atom) {
            return at;
        }
    }
    return nullptr;
}

static embi_event_item item_of(embi_bind_obj *bo)
{
    if (bo->is_node) {
        return (embi_event_item){bo->ptr, KIND_NODE};
    }
    return (embi_event_item){bo, KIND_RECORD};
}

static embi_bind_obj *record_of(embi_event_item it)
{
    if (it.kind == KIND_RECORD) {
        return it.obj;
    }
    return ((embi_node *)it.obj)->wrapper;
}

/* True when n is from or one of its ancestors. Only valid for a from that is alive (the target of a
 * dispatch is pinned), whose ancestors are alive as well. */
static bool in_chain(const embi_node *from, const embi_node *n)
{
    for (; from; from = from->parent) {
        if (from == n) {
            return true;
        }
    }
    return false;
}

/* Is the item still safe to touch? KIND_BARE items are, while the target is below them. */
static bool item_alive(const embi_event *ev, embi_event_item it)
{
    if (it.kind != KIND_BARE) {
        return true;
    }
    return ev->target.kind != KIND_RECORD && in_chain(ev->target.obj, it.obj);
}

static embi_event_item op_parent(void *ctxp, embi_event_item item, const embi_event *ev)
{
    dctx *d = ctxp;
    embi_bind_ctx *c = d->c;
    if (item.kind == KIND_RECORD) {
        return (embi_event_item){nullptr, 0};
    }
    embi_node *n = item.obj;
    if (n->type == EMBI_NODE_DOCUMENT) {
        /* The document's parent is the window, except for load. */
        bool load = ev->type_len == 4 && SDL_memcmp(ev->type, "load", 4) == 0;
        if (n == &c->doc->base && !load) {
            return (embi_event_item){c->gbo, KIND_RECORD};
        }
        return (embi_event_item){nullptr, 0};
    }
    embi_node *p = n->parent;
    if (!p) {
        return (embi_event_item){nullptr, 0};
    }
    /* An ancestor with a wrapper, or with a content attribute for this event, gets its wrapper pinned by
     * the dispatch scope, so that a listener removing it cannot free it under the dispatch. Any other
     * ancestor has no listeners and gets no wrapper. */
    bool bare = false;
    if (!p->wrapper) {
        if (p->type == EMBI_NODE_ELEMENT) {
            embi_element *pe = (embi_element *)p;
            bare = on_attr(d, pe) == nullptr;
        } else {
            bare = true;
        }
    }
    if (bare) {
        return (embi_event_item){p, KIND_BARE};
    }
    (void)embi_bind_pin(c, embi_bind_node_value(c, p));
    return (embi_event_item){p, KIND_NODE};
}

static bool is_window_error(const dctx *d, const embi_bind_obj *rec)
{
    return rec == d->c->gbo && embi_bind_iface_is(d->ev_bo->iface, EMBI_IF_ErrorEvent) &&
           embi_dstr_eq_utf8(lit("error"), embi_event_type(d->ev_bo->ptr));
}

/* Calls one listener of rec. Returns the result of the call (EMBI_JS_EXCEPTION when it threw). */
static embi_js_value call_listener(dctx *d, embi_bind_obj *rec, embi_js_value cb, int32_t fl, embi_js_value this_v,
                                   embi_js_value ev_v)
{
    embi_bind_ctx *c = d->c;
    if (!embi_js_is_callable(cb)) {
        if (fl & LF_HANDLER) {
            return EMBI_JS_UNDEFINED;
        }
        embi_js_value h = embi_js_obj_get(c->rt, embi_js_as_object(cb), embi_js_key_c(c->rt, "handleEvent"), cb);
        if (h == EMBI_JS_EXCEPTION) {
            return h;
        }
        if (!embi_js_is_callable(h)) {
            return embi_js_throw_type_error(c->rt, "The event listener's handleEvent is not a function.");
        }
        return embi_js_call(c->rt, h, cb, 1, &ev_v);
    }
    if ((fl & LF_HANDLER) && is_window_error(d, rec)) {
        /* window.onerror(message, source, lineno, colno, error) */
        embi_js_value p = d->ev_bo->ref;
        embi_js_value args[5] = {pget(c, p, "message"), pget(c, p, "filename"), embi_js_number(pnum(c, p, "lineno", 0)),
                                 embi_js_number(pnum(c, p, "colno", 0)), pnull(c, p, "error")};
        if (embi_js_is_undefined(args[0])) {
            args[0] = embi_bind_utf8_value(c, EMBI_STR(""));
        }
        if (embi_js_is_undefined(args[1])) {
            args[1] = embi_bind_utf8_value(c, EMBI_STR(""));
        }
        return embi_js_call(c->rt, cb, this_v, 5, args);
    }
    return embi_js_call(c->rt, cb, this_v, 1, &ev_v);
}

/* Runs the quadruples of rec that match the event type and the pass (capturing or bubbling). */
static void op_invoke(void *ctxp, embi_event_item item, embi_event *ev, bool capture)
{
    dctx *d = ctxp;
    embi_bind_ctx *c = d->c;
    if (!item_alive(ev, item)) {
        return;
    }
    embi_bind_obj *rec = record_of(item);
    if (!rec || embi_bind_stale(rec)) {
        return; /* a bare ancestor still has no wrapper: no listeners */
    }
    embi_bind_scope sc = embi_bind_enter(c);
    bool el_rec = rec->is_node && ((embi_node *)rec->ptr)->type == EMBI_NODE_ELEMENT;
    const embi_attr *on = el_rec ? on_attr(d, (embi_element *)rec->ptr) : nullptr;
    embi_js_value list = rec->listeners;
    /* CF1: one pass over the list finds the matching registrations and the content handler of the type. Snapshot of the
     * matching registrations (flags and serial, and the index they had); the list may change while they run. */
    uint32_t m = 0;
    embi_js_value snap = EMBI_JS_UNDEFINED;
    for (int pass = 0; pass < 2; pass++) {
        m = 0;
        snap = EMBI_JS_UNDEFINED;
        int32_t hidx = -1;
        uint32_t n = embi_js_is_object(list) ? embi_bind_array_len(c, list) : 0;
        for (uint32_t i = 0; i + STRIDE <= n; i += STRIDE) {
            if (!str_eq(embi_bind_array_get(c, list, i), d->type_v)) {
                continue;
            }
            int32_t fl = lflags(c, list, i);
            if ((fl & LF_HANDLER) != 0 && hidx < 0) {
                hidx = (int32_t)i;
            }
            if (((fl & LF_CAPTURE) != 0) == capture) {
                if (embi_js_is_undefined(snap)) { /* no matching registration: no snapshot */
                    snap = embi_bind_pin(c, embi_bind_array_new(c, 0));
                }
                embi_bind_array_set(c, snap, 2 * m, embi_bind_array_get(c, list, i + 2));
                embi_bind_array_set(c, snap, 2 * m + 1, embi_js_number(i));
                m++;
            }
        }
        /* The content attribute handler is brought up to date first (sync_content), which changes the list only when
         * there is an attribute or a handler that came from one (or that saw one): then the list is looked at again. */
        if (pass == 1 || !el_rec ||
            (on == nullptr && (hidx < 0 || ((lflags(c, list, (uint32_t)hidx) & LF_CONTENT) == 0 &&
                                            embi_js_is_undefined(embi_bind_array_get(c, list, (uint32_t)hidx + 3)))))) {
            break;
        }
        sync_content_attr(c, rec, on, d->type_v);
        list = rec->listeners;
    }
    if (m > 0) {
        embi_js_value this_v = embi_bind_obj_value(rec);
        for (uint32_t k = 0; k < m; k++) {
            if (ev->flags & EMBI_EVF_STOP_IMMEDIATE) {
                break;
            }
            double raw = 0, at = 0;
            (void)embi_js_to_number(c->rt, embi_bind_array_get(c, snap, 2 * k), &raw);
            (void)embi_js_to_number(c->rt, embi_bind_array_get(c, snap, 2 * k + 1), &at);
            int32_t fl = (int32_t)((uint64_t)raw % LF_SERIAL_SHIFT);
            /* Still registered? The registration is identified by its serial, so that a listener removed and
             * added again during the dispatch (a new registration) does not run for this snapshot. CF1: the slot it
             * had is looked at first (the same flags and serial there are the same registration). */
            embi_js_value cur = rec->listeners;
            int32_t idx = -1;
            if (embi_js_is_object(cur) && (uint32_t)at + STRIDE <= embi_bind_array_len(c, cur) &&
                lraw(c, cur, (uint32_t)at) == raw) {
                idx = (int32_t)at;
            } else {
                idx = find_serial(c, cur, (double)((uint64_t)raw / LF_SERIAL_SHIFT));
            }
            if (idx < 0) {
                continue;
            }
            /* A handler attribute reads its current value; the others cannot change. */
            embi_js_value cb = embi_bind_array_get(c, rec->listeners, (uint32_t)idx + 1);
            if (!embi_js_is_object(cb)) {
                continue;
            }
            if (fl & LF_ONCE) {
                listener_drop(c, rec, (uint32_t)idx);
            }
            pset(c, d->ev_bo->ref, "currentTarget", this_v);
            if (fl & LF_PASSIVE) {
                ev->flags |= EMBI_EVF_IN_PASSIVE;
            }
            embi_js_value ev_v = embi_bind_obj_value(d->ev_bo);
            /* window.event is the event while a listener runs and what it was before afterwards */
            bool had_event = c->has_win_event;
            embi_js_value prev_event = c->win_event;
            c->win_event = ev_v;
            c->has_win_event = true;
            embi_js_value r = call_listener(d, rec, cb, fl, this_v, ev_v);
            c->win_event = prev_event;
            c->has_win_event = had_event;
            ev->flags &= ~EMBI_EVF_IN_PASSIVE;
            if (r == EMBI_JS_EXCEPTION) {
                report_exception(c);
            } else if ((fl & LF_HANDLER) && r == embi_js_from_bool(is_window_error(d, rec))) {
                /* An event handler returning false cancels the event; for window.onerror it is true. */
                embi_event_prevent_default(ev);
            }
            /* A microtask checkpoint after each callback when no script is running (the embedder
             * entry depth is zero): the dispatch was started by the host, not by a script. */
            if (c->rt->nest == 0) {
                embi_js_run_jobs(c->rt);
            }
        }
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ activation behaviors */

#define MAX_ACTIVATIONS 32
static struct {
    const char *tag;
    const embi_activation *a;
} g_activations[MAX_ACTIVATIONS];
static uint32_t g_nactivations;

bool embi_bind_register_activation(const char *tag, const embi_activation *a)
{
    for (uint32_t i = 0; i < g_nactivations; i++) {
        if (SDL_strcmp(g_activations[i].tag, tag) == 0) {
            g_activations[i].a = a;
            return true;
        }
    }
    if (g_nactivations == MAX_ACTIVATIONS) {
        return false;
    }
    g_activations[g_nactivations].tag = tag;
    g_activations[g_nactivations++].a = a;
    return true;
}

void embi_bind_clear_activations(void)
{
    g_nactivations = 0;
}

static const embi_activation *activation_of(embi_bind_ctx *c, embi_event_item it)
{
    if (it.kind == KIND_RECORD || !g_nactivations) {
        return nullptr;
    }
    embi_node *n = it.obj;
    if (n->type != EMBI_NODE_ELEMENT) {
        return nullptr;
    }
    embi_str name = embi_atom_str(n->name);
    for (uint32_t i = 0; i < g_nactivations; i++) {
        size_t tl = SDL_strlen(g_activations[i].tag);
        if (tl == name.len && SDL_memcmp(g_activations[i].tag, name.ptr, tl) == 0) {
            const embi_activation *a = g_activations[i].a;
            return (!a->applies || a->applies(c, n)) ? a : nullptr;
        }
    }
    return nullptr;
}

static bool op_has_activation(void *ctxp, embi_event_item item, const embi_event *ev)
{
    dctx *d = ctxp;
    (void)ev;
    if (!d->activation_event || !activation_of(d->c, item)) {
        return false;
    }
    /* Keep the element (and its wrapper) alive until the activation has run. */
    (void)embi_bind_pin(d->c, embi_bind_node_value(d->c, item.obj));
    return true;
}

static void op_activation(void *ctxp, embi_event_item item, embi_event *ev, int phase)
{
    dctx *d = ctxp;
    (void)ev;
    const embi_activation *a = activation_of(d->c, item);
    if (!a) {
        return;
    }
    embi_bind_obj *eb = d->ev_bo;
    if (phase == EMBI_ACT_PRE) {
        if (a->pre) {
            a->pre(d->c, item.obj, eb);
        }
    } else if (phase == EMBI_ACT_RUN) {
        a->run(d->c, item.obj, eb);
    } else if (a->canceled) {
        a->canceled(d->c, item.obj, eb);
    }
}

/* Dispatches the Event wrapper ev_v at target (the DOM dispatch, see event.h). Returns true when
 * the event was not canceled. The caller checks the state of the event. */
bool embi_impl_dispatch_value(embi_bind_ctx *c, embi_bind_obj *target, embi_js_value ev_v)
{
    embi_bind_obj *eb = embi_bind_obj_of(ev_v);
    embi_event *ev = eb->ptr;
    embi_bind_scope sc = embi_bind_enter(c);
    dctx d = {c, eb, embi_bind_pin(c, embi_bind_utf8_value(c, embi_event_type(ev))), on_name(c, embi_event_type(ev)),
              embi_bind_iface_is(eb->iface, EMBI_IF_MouseEvent) && embi_event_type(ev).len == 5 &&
                  SDL_memcmp(ev->type, "click", 5) == 0,
              0, 0};
    embi_event_ops ops = {&d, op_parent, op_invoke, op_has_activation, op_activation};
    embi_js_value target_v = embi_bind_pin(c, embi_bind_obj_value(target));
    pset(c, eb->ref, "target", target_v);
    pset(c, eb->ref, "currentTarget", EMBI_JS_NULL);
    bool ok = embi_event_dispatch(&ops, ev, item_of(target));
    pset(c, eb->ref, "currentTarget", EMBI_JS_NULL);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}

void embi_impl_fire(embi_bind_ctx *c, embi_bind_obj *target, const char *type, embi_js_value event)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value ev_v = event;
    if (!event_of(c, event)) {
        ev_v = event_make(c, EMBI_IF_Event, (embi_str){type, (uint32_t)SDL_strlen(type)}, EMBI_EVF_TRUSTED,
                          EMBI_JS_UNDEFINED);
        if (ev_v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return;
        }
    }
    (void)embi_bind_pin(c, ev_v);
    (void)embi_impl_dispatch_value(c, target, ev_v);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ EventTarget */

embi_js_value embi_impl_EventTarget_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_EventTarget, c, nullptr, 0);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

/* DOM "default passive value": touchstart, touchmove, wheel and mousewheel listeners on the window, the document, its
 * document element and its body are passive unless the options say otherwise. */
static bool default_passive(embi_bind_ctx *c, const embi_bind_obj *self, embi_dstr type)
{
    if (!embi_dstr_eq_utf8(type, EMBI_STR("touchstart")) && !embi_dstr_eq_utf8(type, EMBI_STR("touchmove")) &&
        !embi_dstr_eq_utf8(type, EMBI_STR("wheel")) && !embi_dstr_eq_utf8(type, EMBI_STR("mousewheel"))) {
        return false;
    }
    if (self == c->gbo) {
        return true;
    }
    if (!self->is_node || self->ptr == nullptr) {
        return false;
    }
    const embi_node *n = self->ptr;
    if (n->type == EMBI_NODE_DOCUMENT) {
        return true;
    }
    if (n->type != EMBI_NODE_ELEMENT || n->extra != EMBI_NS_HTML || n->parent == nullptr) {
        return false;
    }
    if (n->parent->type == EMBI_NODE_DOCUMENT) {
        return true; /* the document element */
    }
    if (n->name == EMBI_ATOM_body && n->parent->type == EMBI_NODE_ELEMENT && n->parent->parent != nullptr &&
        n->parent->parent->type == EMBI_NODE_DOCUMENT) {
        return true;
    }
    return false;
}

static bool options_of(embi_bind_ctx *c, const embi_bind_obj *self, embi_dstr type, embi_bind_union o, bool *capture,
                       bool *once, bool *passive, embi_bind_obj **signal)
{
    *capture = false;
    *once = false;
    *signal = nullptr;
    bool passive_set = false;
    *passive = false;
    if (o.kind == 2) {
        *capture = o.u.b;
    } else if (o.kind == 1 && o.u.ptr) {
        const embi_dict_AddEventListenerOptions *d = o.u.ptr;
        *capture = d->base.capture;
        *once = d->once;
        if (!embi_js_is_undefined(d->passive)) {
            passive_set = true;
            *passive = embi_js_to_boolean(d->passive);
        }
        *signal = d->signal;
    }
    if (!passive_set) {
        *passive = default_passive(c, self, type);
    }
    return true;
}

static bool signal_aborted(embi_bind_ctx *c, embi_bind_obj *sig);
static void deps_add(embi_bind_ctx *c, embi_bind_obj *sig, uint32_t kind, embi_js_value a, embi_js_value b,
                     embi_js_value cb);

void embi_impl_EventTarget_addEventListener(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1,
                                            embi_bind_union a2)
{
    if (embi_js_is_nullish(a1) || embi_bind_stale(self)) {
        return;
    }
    bool capture, once, passive;
    embi_bind_obj *signal;
    options_of(c, self, a0, a2, &capture, &once, &passive, &signal);
    if (signal && signal_aborted(c, signal)) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value type = embi_bind_pin(c, embi_bind_dstr_value(c, a0));
    int32_t flags = (capture ? LF_CAPTURE : 0) | (once ? LF_ONCE : 0) | (passive ? LF_PASSIVE : 0);
    embi_js_value sig_v = signal ? embi_bind_obj_value(signal) : EMBI_JS_UNDEFINED;
    if (listener_add(c, self, type, a1, flags, sig_v) && signal) {
        deps_add(c, signal, capture ? DEP_REMOVE_CAPTURE : DEP_REMOVE, embi_bind_obj_value(self), type, a1);
    }
    if (self->iface == EMBI_IF_MediaQueryList && embi_dstr_eq_utf8(a0, EMBI_STR("change"))) {
        embi_impl_mql_watch(c, self);
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_impl_EventTarget_removeEventListener(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1,
                                               embi_bind_union a2)
{
    if (embi_js_is_nullish(a1) || embi_bind_stale(self)) {
        return;
    }
    /* the options are EventListenerOptions here (only capture), a smaller dictionary than AddEventListenerOptions */
    bool capture = false;
    if (a2.kind == 2) {
        capture = a2.u.b;
    } else if (a2.kind == 1 && a2.u.ptr) {
        capture = ((const embi_dict_EventListenerOptions *)a2.u.ptr)->capture;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value type = embi_bind_pin(c, embi_bind_dstr_value(c, a0));
    listener_remove(c, self, type, a1, capture);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_impl_EventTarget_dispatchEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_event *ev = event_of(c, a0);
    if (!ev) {
        return embi_js_throw_type_error(c->rt, "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    }
    if (ev->flags & EMBI_EVF_DISPATCHING) {
        return embi_bind_throw_named(c, "InvalidStateError", "The event is already being dispatched.");
    }
    if (!(ev->flags & EMBI_EVF_INITIALIZED)) {
        return embi_bind_throw_named(c, "InvalidStateError", "The event is not initialized.");
    }
    embi_bind_scope sc = embi_bind_enter(c);
    bool ok = embi_impl_dispatch_value(c, self, a0);
    return embi_bind_leave(sc, embi_js_from_bool(ok));
}

/* ------------------------------------------------------------------ AbortSignal and AbortController */

/* A signal with a fresh payload: aborted false, reason undefined, deps []. */
static embi_js_value signal_new(embi_bind_ctx *c)
{
    embi_js_value p = payload_new(c);
    embi_js_value deps = embi_bind_pin(c, embi_bind_array_new(c, 0));
    pset(c, p, "deps", deps);
    pset(c, p, "aborted", embi_js_from_bool(false));
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_AbortSignal, nullptr, nullptr, 0);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    bo->ptr = bo;
    embi_bind_set_ref(bo, p);
    return v;
}

/* A stale signal counts as aborted: nothing can be added to it. */
static bool signal_aborted(embi_bind_ctx *c, embi_bind_obj *sig)
{
    if (embi_bind_stale(sig)) {
        return true;
    }
    return embi_js_to_boolean(pget(c, sig->ref, "aborted"));
}

/* Appends an action to the signal's deps. Dependents that are already aborted are pruned on the way
 * (an AbortSignal.any signal that was aborted by another source is of no use to this one any more). */
static void deps_add(embi_bind_ctx *c, embi_bind_obj *sig, uint32_t kind, embi_js_value a, embi_js_value b,
                     embi_js_value cb)
{
    embi_js_value deps = pget(c, sig->ref, "deps");
    if (!embi_js_is_object(deps)) {
        return;
    }
    uint32_t n = embi_bind_array_len(c, deps);
    if (kind == DEP_PROPAGATE && n >= 64 * STRIDE && ((n / STRIDE) & (n / STRIDE - 1)) == 0) {
        /* Sweep when the entry count is a power of two, so that the cost stays amortized. */
        embi_js_value nd = embi_bind_pin(c, embi_bind_array_new(c, 0));
        uint32_t w = 0;
        for (uint32_t k = 0; k + STRIDE <= n; k += STRIDE) {
            double kd = 0;
            (void)embi_js_to_number(c->rt, embi_bind_array_get(c, deps, k), &kd);
            if ((uint32_t)kd == DEP_PROPAGATE) {
                embi_bind_obj *dep = embi_bind_obj_of(embi_bind_array_get(c, deps, k + 1));
                if (!dep || signal_aborted(c, dep)) {
                    continue;
                }
            }
            for (uint32_t q = 0; q < STRIDE; q++) {
                embi_bind_array_set(c, nd, w++, embi_bind_array_get(c, deps, k + q));
            }
        }
        pset(c, sig->ref, "deps", nd);
        deps = nd;
        n = w;
    }
    embi_bind_array_set(c, deps, n, embi_js_number((double)kind));
    embi_bind_array_set(c, deps, n + 1, a);
    embi_bind_array_set(c, deps, n + 2, b);
    embi_bind_array_set(c, deps, n + 3, cb);
}

/* Drops the DEP_REMOVE entry of a listener that was removed some other way (removeEventListener, once),
 * so that a long-lived signal does not keep removed targets and callbacks alive. */
static void deps_forget(embi_bind_ctx *c, embi_js_value sig_v, embi_js_value target_v, embi_js_value type,
                        embi_js_value cb, bool capture)
{
    embi_bind_obj *sig = embi_bind_obj_of(sig_v);
    if (!sig || embi_bind_stale(sig)) {
        return;
    }
    embi_js_value deps = pget(c, sig->ref, "deps");
    if (!embi_js_is_object(deps)) {
        return;
    }
    uint32_t n = embi_bind_array_len(c, deps);
    uint32_t want = capture ? DEP_REMOVE_CAPTURE : DEP_REMOVE;
    for (uint32_t k = 0; k + STRIDE <= n; k += STRIDE) {
        double kd = 0;
        (void)embi_js_to_number(c->rt, embi_bind_array_get(c, deps, k), &kd);
        if ((uint32_t)kd != want || embi_bind_array_get(c, deps, k + 1) != target_v ||
            embi_bind_array_get(c, deps, k + 3) != cb || !str_eq(embi_bind_array_get(c, deps, k + 2), type)) {
            continue;
        }
        /* A copy without the entry: an abort in progress may be walking the old array. */
        embi_js_value nd = embi_bind_pin(c, embi_bind_array_new(c, 0));
        uint32_t w = 0;
        for (uint32_t j = 0; j + STRIDE <= n; j += STRIDE) {
            if (j == k) {
                continue;
            }
            for (uint32_t q = 0; q < STRIDE; q++) {
                embi_bind_array_set(c, nd, w++, embi_bind_array_get(c, deps, j + q));
            }
        }
        pset(c, sig->ref, "deps", nd);
        return;
    }
}

static embi_js_value default_abort_reason(embi_bind_ctx *c)
{
    return embi_bind_dom_exception(c, 20, lit("AbortError"), lit("signal is aborted without reason"));
}

/* First half of the abort: sets aborted and reason on sig, removes the listeners that were bound to it,
 * and does the same for the dependent signals (recursively), appending each one to out. No event is
 * fired here. */
static void abort_mark(embi_bind_ctx *c, embi_bind_obj *sig, embi_js_value reason, embi_js_value out)
{
    pset(c, sig->ref, "aborted", embi_js_from_bool(true));
    pset(c, sig->ref, "reason", reason);
    embi_js_value taken = embi_bind_pin(c, pget(c, sig->ref, "deps"));
    pset(c, sig->ref, "deps", embi_bind_pin(c, embi_bind_array_new(c, 0)));
    uint32_t n = embi_js_is_object(taken) ? embi_bind_array_len(c, taken) : 0;
    for (uint32_t k = 0; k + STRIDE <= n; k += STRIDE) {
        double kd = 0;
        (void)embi_js_to_number(c->rt, embi_bind_array_get(c, taken, k), &kd);
        embi_js_value a = embi_bind_array_get(c, taken, k + 1);
        embi_js_value b = embi_bind_array_get(c, taken, k + 2);
        embi_js_value cb = embi_bind_array_get(c, taken, k + 3);
        if ((uint32_t)kd == DEP_PROPAGATE) {
            embi_bind_obj *dep = embi_bind_obj_of(a);
            if (dep && !signal_aborted(c, dep)) {
                embi_bind_array_set(c, out, embi_bind_array_len(c, out), a);
                abort_mark(c, dep, reason, out);
            }
        } else {
            embi_bind_obj *target = embi_bind_obj_of(a);
            if (target && !embi_bind_stale(target)) {
                listener_remove(c, target, b, cb, (uint32_t)kd == DEP_REMOVE_CAPTURE);
            }
        }
    }
}

static void fire_abort(embi_bind_ctx *c, embi_bind_obj *sig)
{
    embi_js_value ev = event_make(c, EMBI_IF_Event, EMBI_STR("abort"), EMBI_EVF_TRUSTED, EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return;
    }
    (void)embi_bind_pin(c, ev);
    (void)embi_impl_dispatch_value(c, sig, ev);
}

/* The DOM Standard's "signal abort": the state of the signal and of its dependents changes first, then the
 * abort event fires at the signal, then at each dependent in order. */
static void signal_abort(embi_bind_ctx *c, embi_bind_obj *sig, embi_js_value reason)
{
    if (embi_bind_stale(sig) || embi_js_to_boolean(pget(c, sig->ref, "aborted"))) {
        return;
    }
    embi_bind_scope scope = embi_bind_enter(c);
    if (embi_js_is_undefined(reason)) {
        reason = embi_bind_pin(c, default_abort_reason(c));
    } else {
        reason = embi_bind_pin(c, reason);
    }
    embi_js_value dependents = embi_bind_pin(c, embi_bind_array_new(c, 0));
    abort_mark(c, sig, reason, dependents);
    fire_abort(c, sig);
    uint32_t n = embi_bind_array_len(c, dependents);
    for (uint32_t i = 0; i < n; i++) {
        embi_bind_obj *dep = embi_bind_obj_of(embi_bind_array_get(c, dependents, i));
        if (dep && !embi_bind_stale(dep)) {
            fire_abort(c, dep);
        }
    }
    embi_bind_leave(scope, EMBI_JS_UNDEFINED);
}

embi_js_value embi_impl_AbortSignal_abort(embi_bind_ctx *c, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value s = embi_bind_pin(c, signal_new(c));
    signal_abort(c, embi_bind_obj_of(s), a0);
    return embi_bind_leave(sc, s);
}

/* The native handler of the timer behind AbortSignal.timeout: the timer is given [handler, signal]. */
static embi_js_value timeout_fire(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                  int magic)
{
    (void)this_v;
    (void)magic;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_bind_obj *sig = c && argc > 0 ? embi_bind_obj_of(argv[0]) : nullptr;
    if (sig) {
        embi_bind_scope sc = embi_bind_enter(c);
        signal_abort(c, sig, embi_bind_dom_exception(c, 23, lit("TimeoutError"), lit("signal timed out")));
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return EMBI_JS_UNDEFINED;
}

/* AbortSignal.timeout(ms): aborts with a TimeoutError DOMException through the embedder's timer hooks;
 * without hooks the signal never aborts. */
embi_js_value embi_impl_AbortSignal_timeout(embi_bind_ctx *c, uint32_t a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value s = embi_bind_pin(c, signal_new(c));
    if (c->timers.add) {
        embi_js_object *fn = embi_js_native_function_new(c->realm, "", 0, timeout_fire, 0);
        if (fn) {
            embi_js_value fv = embi_bind_pin(c, embi_js_from_object(fn));
            (void)embi_impl_Window_setTimeout(c, c->gbo, fv, (int32_t)(a0 > 0x7fffffffu ? 0x7fffffffu : a0), 1, &s);
        }
    }
    return embi_bind_leave(sc, s);
}

embi_js_value embi_impl_AbortSignal_any(embi_bind_ctx *c, embi_bind_seq a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value s = embi_bind_pin(c, signal_new(c));
    embi_bind_obj *out = embi_bind_obj_of(s);
    embi_bind_obj **items = a0.data;
    /* "source signals": the dependent signals of a dependent signal are those of its sources (DOM, any()), so that the
     * abort events fire in the order the signals were made. */
    embi_js_value srcs = embi_bind_pin(c, embi_bind_array_new(c, 0));
    pset(c, out->ref, "sources", srcs);
    for (uint32_t i = 0; i < a0.len; i++) {
        embi_bind_obj *src = items[i];
        if (!src || embi_bind_stale(src)) {
            continue;
        }
        if (embi_js_to_boolean(pget(c, src->ref, "aborted"))) {
            signal_abort(c, out, pget(c, src->ref, "reason"));
            break;
        }
        embi_js_value inherited = pget(c, src->ref, "sources");
        uint32_t ni = embi_js_is_object(inherited) ? embi_bind_array_len(c, inherited) : 0;
        for (uint32_t j = 0; j < (ni ? ni : 1); j++) {
            embi_js_value sv = ni ? embi_bind_array_get(c, inherited, j) : embi_bind_obj_value(src);
            bool dup = false;
            uint32_t have = embi_bind_array_len(c, srcs);
            for (uint32_t q = 0; q < have && !dup; q++) {
                dup = embi_bind_array_get(c, srcs, q) == sv;
            }
            embi_bind_obj *sb = embi_bind_obj_of(sv);
            if (dup || !sb || embi_bind_stale(sb)) {
                continue;
            }
            embi_bind_array_set(c, srcs, have, sv);
            deps_add(c, sb, DEP_PROPAGATE, s, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
        }
    }
    return embi_bind_leave(sc, s);
}

bool embi_impl_AbortSignal_get_aborted(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_to_boolean(pget(c, self->ref, "aborted"));
}

embi_js_value embi_impl_AbortSignal_get_reason(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "reason");
}

embi_dom_err embi_impl_AbortSignal_throwIfAborted(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_js_to_boolean(pget(c, self->ref, "aborted"))) {
        embi_js_exception_set(c->rt, pget(c, self->ref, "reason"));
        return EMBI_BIND_THREW;
    }
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_AbortController_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    embi_js_value sig = signal_new(c);
    pset(c, p, "signal", sig);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_AbortController, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), p);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_AbortController_get_signal(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "signal");
}

void embi_impl_AbortController_abort(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_bind_obj *sig = embi_bind_obj_of(pget(c, self->ref, "signal"));
    if (sig) {
        signal_abort(c, sig, a0);
    }
}

/* ------------------------------------------------------------------ DataTransfer */

embi_js_value embi_impl_DataTransfer_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = payload_new(c);
    pset(c, p, "dropEffect", embi_bind_utf8_value(c, EMBI_STR("none")));
    pset(c, p, "data", embi_bind_object_new(c));
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_DataTransfer, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), p);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_dstr embi_impl_DataTransfer_get_dropEffect(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pstr(c, self->ref, "dropEffect");
}

void embi_impl_DataTransfer_set_dropEffect(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    pset(c, self->ref, "dropEffect", embi_bind_dstr_value(c, a0));
}

/* The format names of getData and setData: "text" is "text/plain", case is folded. */
static embi_js_key format_key(embi_bind_ctx *c, embi_dstr format)
{
    embi_str f = embi_bind_utf8(c, format);
    char *buf = embi_bind_alloc(c, f.len + 1);
    for (uint32_t i = 0; i < f.len; i++) {
        char ch = (char)f.ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    buf[f.len] = '\0';
    if (SDL_strcmp(buf, "text") == 0) {
        return embi_js_key_c(c->rt, "text/plain");
    }
    return embi_js_key_c(c->rt, buf);
}

embi_dstr embi_impl_DataTransfer_getData(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_js_value data = pget(c, self->ref, "data");
    if (!embi_js_is_object(data)) {
        return embi_dstr_empty();
    }
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(data), format_key(c, a0), data);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return embi_dstr_empty();
    }
    embi_dstr out = embi_dstr_empty();
    if (!embi_js_is_undefined(v) && embi_bind_to_dstr(c, v, &out)) {
        return out;
    }
    return embi_dstr_empty();
}

void embi_impl_DataTransfer_setData(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    embi_js_value data = pget(c, self->ref, "data");
    if (embi_js_is_object(data)) {
        (void)embi_js_obj_set(c->rt, embi_js_as_object(data), format_key(c, a0), embi_bind_dstr_value(c, a1), data);
    }
}

void embi_impl_DataTransfer_clearData(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_js_value data = pget(c, self->ref, "data");
    if (!embi_js_is_object(data)) {
        return;
    }
    if (a0.len == 0) {
        pset(c, self->ref, "data", embi_bind_object_new(c));
        return;
    }
    (void)embi_js_obj_delete(c->rt, embi_js_as_object(data), format_key(c, a0));
}
