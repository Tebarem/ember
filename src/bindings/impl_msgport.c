/*
 * MessageChannel, MessagePort, MessageEvent and Window.postMessage (messaging.webidl, W7f, HTML 9.3 and 9.4).
 *
 * The port core is src/loader/msgport.c: entangled pairs with a queue of structured-clone byte strings. A port wrapper is
 * a small record (the port id; zero once the port was transferred away) and is kept alive by a persistent handle until the
 * port is closed or transferred (the standard keeps an entangled port that can still receive alive). A message becomes one
 * task of the view's loop (the loader environment's post hook) once the receiving port is started; the task takes the
 * oldest message off the queue, deserializes it in the realm and fires a MessageEvent at the port ("messageerror" when the
 * bytes cannot be read). The first task of a port is queued by start() or by assigning onmessage. React's scheduler relies
 * on exactly this: a task per message, in order, after the microtasks.
 *
 * A context without a post hook (a bare test context) queues messages and never delivers them.
 *
 * Transfer: MessagePort and ReadableStream are transferable (ldr_xfer_*, called by the serializer through the hooks in
 * impl_loader_clone.c). A port in a transfer list is detached from its wrapper at commit and rebuilt in the receiving
 * realm with a new wrapper (its queue and its entanglement move with the record). A ReadableStream becomes a message
 * channel whose far end is the transferred port; the pump on each side is in streams.js (transferReadable and
 * receiveReadable).
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"

#include "bindings/gen/eventtarget.h"
#include "bindings/gen/events.h"
#include "bindings/gen/fetch.h"
#include "bindings/gen/messaging.h"
#include "bindings/gen/window.h"

#include "loader/clone.h"

/* ------------------------------------------------------------------ wrappers */

/* The block behind a MessagePort wrapper. id is zero once the port was transferred away (a detached wrapper). */
typedef struct pw {
    uint32_t id;
    embi_bind_obj *bo;
} pw;

static pw *pw_of(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || bo->iface != EMBI_IF_MessagePort || embi_bind_stale(bo)) {
        return nullptr;
    }
    return bo->ptr;
}

static embi_mport *port_of(embi_bind_ctx *c, embi_bind_obj *bo)
{
    if (embi_bind_stale(bo)) {
        return nullptr;
    }
    pw *w = bo->ptr;
    return w->id != 0 ? embi_mp_find(ldr_ports(c), w->id) : nullptr;
}

/* Releases the handle that keeps the wrapper of a port alive. */
static void unpin(embi_bind_ctx *c, embi_mport *p)
{
    uint32_t slot = embi_mp_slot(p);
    if (slot != 0) {
        embi_js_persistent_free(c->rt, slot - 1u);
        embi_mp_set_slot(p, 0);
    }
}

/* A new wrapper for the port, pinned until the port closes or moves. */
static embi_js_value port_wrap(embi_bind_ctx *c, embi_mport *p)
{
    pw *w = embi_alloc(sizeof *w);
    w->id = embi_mp_id(p);
    w->bo = nullptr;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_MessagePort, w, w, sizeof *w);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    w->bo = bo;
    unpin(c, p);
    embi_mp_set_slot(p, embi_js_persistent_new(c->rt, v) + 1u);
    return v;
}

/* The wrapper of a port that is alive (undefined: none). */
static embi_js_value port_value(embi_bind_ctx *c, embi_mport *p)
{
    uint32_t slot = embi_mp_slot(p);
    return slot != 0 ? embi_js_persistent_get(c->rt, slot - 1u) : EMBI_JS_UNDEFINED;
}

static void release_each(embi_mport *p, void *arg)
{
    embi_bind_ctx *c = arg;
    unpin(c, p);
}

void ldr_msg_release(embi_bind_ctx *c, embi_mports *ports)
{
    if (ports == nullptr) {
        return;
    }
    embi_mports_each(ports, release_each, c);
    embi_mports_free(ports);
}

/* ------------------------------------------------------------------ delivery */

typedef struct mtask {
    embi_bind_ctx *c;
    embi_js_realm *realm;
    uint32_t port;   /* the receiving port (0: the window) */
    embi_sb bytes;   /* window messages carry their bytes; port messages are taken off the port's queue */
    embi_sb origin;  /* window messages: the sender's origin */
} mtask;

static void mtask_free(mtask *t)
{
    embi_sb_free(&t->bytes);
    embi_sb_free(&t->origin);
    embi_free(t, sizeof *t);
}

static void mtask_drop(void *arg)
{
    mtask_free(arg);
}

/* Fires the message (or messageerror) event for already-read bytes at target. */
static void fire_message(embi_bind_ctx *c, embi_js_realm *realm, embi_bind_obj *target, embi_str bytes, embi_str origin,
                         embi_js_value source)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value data = EMBI_JS_UNDEFINED, xfer = EMBI_JS_UNDEFINED;
    bool ok = embi_clone_deserialize_ex(realm, bytes, &data, &xfer);
    const char *type = ok ? "message" : "messageerror";
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    if (!ok) {
        embi_js_exception_clear(c->rt);
        data = EMBI_JS_NULL;
        xfer = EMBI_JS_UNDEFINED;
    }
    embi_bind_object_set_c(c, p, "data", data);
    embi_bind_object_set_c(c, p, "origin", embi_bind_utf8_value(c, origin));
    embi_bind_object_set_c(c, p, "lastEventId", embi_bind_utf8_value(c, EMBI_STR("")));
    embi_bind_object_set_c(c, p, "source", ok ? source : EMBI_JS_NULL);
    /* event.ports: the MessagePorts among the transferred objects, as a frozen array. */
    embi_js_value ports = embi_bind_array_new(c, 0);
    uint32_t np = 0;
    if (embi_js_is_object(xfer)) {
        uint32_t n = embi_bind_array_len(c, xfer);
        for (uint32_t i = 0; i < n; i++) {
            embi_js_value x = embi_bind_array_get(c, xfer, i);
            if (pw_of(x) != nullptr) {
                embi_bind_array_set(c, ports, np++, x);
            }
        }
    }
    (void)embi_js_set_integrity(c->rt, embi_js_as_object(ports), true);
    embi_bind_object_set_c(c, p, "ports", ports);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_MessageEvent, type, EMBI_EVF_TRUSTED, p);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        embi_impl_fire(c, target, type, ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void mtask_run(void *arg)
{
    mtask *t = arg;
    embi_bind_ctx *c = t->c;
    if (!ldr_ctx_live(c, t->realm)) {
        mtask_free(t);
        return;
    }
    if (t->port == 0) {
        /* Window.postMessage: the event goes to the global object. */
        if (c->gbo != nullptr) {
            fire_message(c, t->realm, c->gbo, (embi_str){t->bytes.data, t->bytes.len}, (embi_str){t->origin.data, t->origin.len},
                         embi_js_from_object(c->realm->global));
        }
        mtask_free(t);
        return;
    }
    embi_mport *p = embi_mp_find(ldr_ports(c), t->port);
    embi_sb msg = {0};
    if (p != nullptr && embi_mp_take(p, &msg)) {
        embi_js_value wv = port_value(c, p);
        embi_bind_obj *bo = embi_bind_obj_of(wv);
        if (bo != nullptr) {
            fire_message(c, t->realm, bo, (embi_str){msg.data, msg.len}, EMBI_STR(""), EMBI_JS_NULL);
        }
        embi_sb_free(&msg);
    }
    mtask_free(t);
}

/* Queues one delivery task for the port. */
static void schedule(embi_bind_ctx *c, embi_mport *p)
{
    const embi_loader_env *env = ldr_env(c);
    if (env->post == nullptr) {
        return;
    }
    mtask *t = embi_alloc(sizeof *t);
    *t = (mtask){0};
    t->c = c;
    t->realm = c->realm;
    t->port = embi_mp_id(p);
    if (!env->post(env->user, mtask_run, mtask_drop, t)) {
        mtask_free(t);
    }
}

static void start_port(embi_bind_ctx *c, embi_mport *p)
{
    uint32_t n = embi_mp_start(p);
    for (uint32_t i = 0; i < n; i++) {
        schedule(c, p);
    }
}

/* ------------------------------------------------------------------ MessageChannel, MessagePort */

enum { MC_PORT1, MC_PORT2, MC_N };

embi_js_value embi_impl_MessageChannel_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_mport *b = nullptr;
    embi_mport *a = embi_mp_channel(ldr_ports(c), &b);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_MessageChannel, c, nullptr, 0);
    embi_bind_adopt_new_target(c, v, new_target);
    embi_js_value arr = embi_bind_array_new(c, MC_N);
    embi_bind_set_ref(embi_bind_obj_of(v), arr);
    embi_bind_array_set(c, arr, MC_PORT1, port_wrap(c, a));
    embi_bind_array_set(c, arr, MC_PORT2, port_wrap(c, b));
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_MessageChannel_get_port1(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_array_get(c, self->ref, MC_PORT1);
}

embi_js_value embi_impl_MessageChannel_get_port2(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_array_get(c, self->ref, MC_PORT2);
}

/* Reads the transfer list of postMessage(message, options): a sequence of objects, or a dictionary with a transfer member.
 * On success *items points to arena memory. False with an exception pending. */
static bool read_transfer(embi_bind_ctx *c, embi_js_value arg, const char *what, embi_js_value **items, uint32_t *n)
{
    *items = nullptr;
    *n = 0;
    if (embi_js_is_nullish(arg)) {
        return true;
    }
    if (!embi_js_is_object(arg)) {
        (void)embi_js_throw_type_error(c->rt, "%s: the second argument is not a sequence or an options object.", what);
        return false;
    }
    embi_js_value iter = embi_js_get(c->rt, embi_js_as_object(arg), ldr_wk_key(c, EMBI_JS_WK_ITERATOR));
    if (iter == EMBI_JS_EXCEPTION) {
        return false;
    }
    embi_js_value seq = arg;
    if (embi_js_is_undefined(iter) || iter == EMBI_JS_NULL) {
        seq = ldr_get(c, arg, "transfer");
        if (seq == EMBI_JS_EXCEPTION) {
            return false;
        }
        if (embi_js_is_undefined(seq)) {
            return true;
        }
    }
    if (!embi_bind_seq_open(c, seq, what, items, n)) {
        return false;
    }
    for (uint32_t i = 0; i < *n; i++) {
        if (!embi_js_is_object((*items)[i])) {
            (void)embi_js_throw_type_error(c->rt, "%s: a transfer list entry is not an object.", what);
            return false;
        }
    }
    return true;
}

/* Serializes the message with its transfer list. False with the exception (a DOMException for DataCloneError) pending. */
static bool serialize_message(embi_bind_ctx *c, embi_js_value message, const embi_js_value *items, uint32_t n, embi_sb *bytes)
{
    ldr_clone_install();
    if (!embi_clone_serialize(embi_bind_realm(c), message, items, n, bytes)) {
        embi_sb_free(bytes);
        ldr_clone_rethrow_dom(c);
        return false;
    }
    return true;
}

embi_dom_err embi_impl_MessagePort_postMessage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value *items = nullptr;
    uint32_t n = 0;
    if (!read_transfer(c, a1, "Failed to execute 'postMessage' on 'MessagePort'", &items, &n)) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    embi_js_value self_v = embi_bind_obj_value(self);
    for (uint32_t i = 0; i < n; i++) {
        if (items[i] == self_v) {
            embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return embi_impl_throw(c, "DataCloneError", "Failed to execute 'postMessage' on 'MessagePort': Port at index 0 contains the source port.");
        }
    }
    embi_sb bytes = {0};
    if (!serialize_message(c, a0, items, n, &bytes)) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    embi_mport *p = port_of(c, self);
    if (p == nullptr) {
        embi_sb_free(&bytes);
    } else {
        embi_mport *to = nullptr;
        if (embi_mp_post(p, &bytes, &to) == EMBI_MP_SCHEDULE) {
            schedule(c, to);
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return EMBI_DOM_OK;
}

void embi_impl_MessagePort_start(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_mport *p = port_of(c, self);
    if (p != nullptr) {
        start_port(c, p);
    }
}

void embi_impl_MessagePort_close(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_mport *p = port_of(c, self);
    if (p != nullptr) {
        embi_mp_close(p);
        unpin(c, p);
        embi_mp_release(ldr_ports(c), p);
    }
}

embi_js_value embi_impl_MessagePort_get_onmessage(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_EventTarget_get_onmessage(c, self);
}

/* Setting onmessage starts the port (HTML: the onmessage setter enables the port message queue). */
void embi_impl_MessagePort_set_onmessage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_impl_EventTarget_set_onmessage(c, self, a0);
    embi_impl_MessagePort_start(c, self);
}

embi_js_value embi_impl_MessagePort_get_onmessageerror(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_EventTarget_get_onmessageerror(c, self);
}

void embi_impl_MessagePort_set_onmessageerror(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_impl_EventTarget_set_onmessageerror(c, self, a0);
}

/* ------------------------------------------------------------------ MessageEvent */

static void pset(embi_bind_ctx *c, embi_js_value obj, const char *k, embi_js_value v)
{
    if (embi_js_is_object(obj)) {
        embi_bind_object_set_c(c, obj, k, v);
    }
}

static embi_js_value pget(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (!embi_js_is_object(obj)) {
        return EMBI_JS_NULL;
    }
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_NULL;
    }
    return embi_js_is_undefined(v) ? EMBI_JS_NULL : v;
}

/* A frozen array of the MessagePort wrappers behind a sequence<MessagePort> (the conversion yields the wrappers' blocks). */
static embi_js_value ports_array(embi_bind_ctx *c, embi_bind_seq seq)
{
    embi_js_value arr = embi_bind_array_new(c, 0);
    void *const *items = seq.data;
    for (uint32_t i = 0; i < seq.len; i++) {
        const pw *w = items[i];
        embi_bind_array_set(c, arr, i, w != nullptr && w->bo != nullptr ? embi_bind_obj_value(w->bo) : EMBI_JS_NULL);
    }
    (void)embi_js_set_integrity(c->rt, embi_js_as_object(arr), true);
    return arr;
}

static uint32_t flags_of(const embi_dict_EventInit *d)
{
    return (d->bubbles ? EMBI_EVF_BUBBLES : 0u) | (d->cancelable ? EMBI_EVF_CANCELABLE : 0u) |
           (d->composed ? EMBI_EVF_COMPOSED : 0u);
}

embi_js_value embi_impl_MessageEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                 embi_dict_MessageEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    pset(c, p, "data", a1.data);
    pset(c, p, "origin", embi_bind_dstr_value(c, a1.origin));
    pset(c, p, "lastEventId", embi_bind_dstr_value(c, a1.lastEventId));
    pset(c, p, "source", embi_js_is_undefined(a1.source) ? EMBI_JS_NULL : a1.source);
    pset(c, p, "ports", ports_array(c, a1.ports));
    embi_str type = embi_bind_utf8(c, a0);
    char *t = embi_bind_alloc(c, (size_t)type.len + 1u);
    memcpy(t, type.ptr, type.len);
    t[type.len] = 0;
    embi_js_value v = embi_impl_event_make(c, EMBI_IF_MessageEvent, t, flags_of(&a1.base), p);
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_MessageEvent_get_data(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "data");
}

static embi_dstr str_member(embi_bind_ctx *c, embi_bind_obj *self, const char *name)
{
    embi_js_value v = pget(c, self->ref, name);
    embi_dstr d;
    if (v == EMBI_JS_NULL || !embi_bind_to_dstr(c, v, &d)) {
        if (v != EMBI_JS_NULL) {
            embi_js_exception_clear(c->rt);
        }
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    return d;
}

embi_dstr embi_impl_MessageEvent_get_origin(embi_bind_ctx *c, embi_bind_obj *self)
{
    return str_member(c, self, "origin");
}

embi_dstr embi_impl_MessageEvent_get_lastEventId(embi_bind_ctx *c, embi_bind_obj *self)
{
    return str_member(c, self, "lastEventId");
}

embi_js_value embi_impl_MessageEvent_get_source(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "source");
}

embi_js_value embi_impl_MessageEvent_get_ports(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "ports");
    return v == EMBI_JS_NULL ? embi_bind_array_new(c, 0) : v;
}

void embi_impl_MessageEvent_initMessageEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                             embi_js_value a3, embi_dstr a4, embi_dstr a5, embi_js_value a6, embi_bind_seq a7)
{
    if (embi_bind_stale(self) || (((const embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) != 0) {
        return;
    }
    embi_impl_Event_initEvent(c, self, a0, a1, a2);
    pset(c, self->ref, "data", a3);
    pset(c, self->ref, "origin", embi_bind_dstr_value(c, a4));
    pset(c, self->ref, "lastEventId", embi_bind_dstr_value(c, a5));
    pset(c, self->ref, "source", embi_js_is_undefined(a6) ? EMBI_JS_NULL : a6);
    pset(c, self->ref, "ports", ports_array(c, a7));
}

/* ------------------------------------------------------------------ Window.postMessage */

/* The origin serialization of the document's URL ("null" for an opaque one). */
static void doc_origin(embi_bind_ctx *c, embi_sb *out)
{
    embi_url u = {0};
    if (embi_url_parse(embi_bind_url(c), nullptr, &u)) {
        embi_url_origin(&u, out);
        embi_url_free(&u);
    } else {
        embi_sb_append(out, EMBI_STR("null"));
    }
}

embi_dom_err embi_impl_Window_postMessage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1,
                                          embi_js_value a2)
{
    (void)self;
    const char *what = "Failed to execute 'postMessage' on 'Window'";
    embi_bind_scope sc = embi_bind_enter(c);
    embi_sb target = {0};
    embi_js_value tlist = a2;
    bool have_target = false;
    if (embi_js_is_object(a1)) {
        /* postMessage(message, { targetOrigin, transfer }) */
        embi_js_value to = ldr_get(c, a1, "targetOrigin");
        if (to == EMBI_JS_EXCEPTION || (!embi_js_is_undefined(to) && !ldr_to_utf8(c, to, &target))) {
            embi_sb_free(&target);
            (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return EMBI_BIND_THREW;
        }
        have_target = !embi_js_is_undefined(to);
        tlist = a1;
    } else if (!embi_js_is_undefined(a1)) {
        if (!ldr_to_utf8(c, a1, &target)) {
            embi_sb_free(&target);
            (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return EMBI_BIND_THREW;
        }
        have_target = true;
    }
    embi_js_value *items = nullptr;
    uint32_t n = 0;
    if (!read_transfer(c, tlist, what, &items, &n)) {
        embi_sb_free(&target);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    embi_sb origin = {0};
    doc_origin(c, &origin);
    /* targetOrigin: "*" always matches, "/" and the empty default mean this document's origin, anything else must parse and
     * match (an unparsable one is a SyntaxError). A mismatch drops the message silently. */
    bool match = true;
    if (have_target && !embi_str_eq((embi_str){target.data, target.len}, EMBI_STR("*")) &&
        !embi_str_eq((embi_str){target.data, target.len}, EMBI_STR("/"))) {
        embi_url tu = {0};
        if (!embi_url_parse((embi_str){target.data, target.len}, nullptr, &tu)) {
            embi_sb_free(&target);
            embi_sb_free(&origin);
            (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return embi_impl_throw(c, "SyntaxError", "Failed to execute 'postMessage' on 'Window': Invalid target origin.");
        }
        embi_sb to = {0};
        embi_url_origin(&tu, &to);
        match = to.len == origin.len && (to.len == 0 || memcmp(to.data, origin.data, to.len) == 0);
        embi_sb_free(&to);
        embi_url_free(&tu);
    }
    embi_sb_free(&target);
    embi_sb bytes = {0};
    if (!serialize_message(c, a0, items, n, &bytes)) {
        embi_sb_free(&origin);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    const embi_loader_env *env = ldr_env(c);
    if (!match || env->post == nullptr) {
        embi_sb_free(&bytes);
        embi_sb_free(&origin);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_DOM_OK;
    }
    mtask *t = embi_alloc(sizeof *t);
    *t = (mtask){0};
    t->c = c;
    t->realm = c->realm;
    t->bytes = bytes;
    t->origin = origin;
    if (!env->post(env->user, mtask_run, mtask_drop, t)) {
        mtask_free(t);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ transfer hooks */

enum { XF_PORT = 1, XF_STREAM = 2 };

static void put_payload(embi_sb *payload, uint8_t kind, uint32_t id)
{
    embi_sb_append(payload, (embi_str){(const char *)&kind, 1});
    embi_sb_append(payload, (embi_str){(const char *)&id, 4});
}

int ldr_xfer_prepare(embi_js_realm *r, embi_js_value v, embi_sb *payload)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    pw *w = pw_of(v);
    if (w != nullptr) {
        if (w->id == 0) {
            return 0; /* a detached port cannot be transferred again */
        }
        uint32_t id = w->id;
        if (embi_mp_find(ldr_ports(c), id) == nullptr) {
            /* The port was closed and released: transfer a fresh closed, disentangled record, never a dead id. */
            id = embi_mp_id(embi_mp_new_closed(ldr_ports(c)));
        }
        put_payload(payload, XF_PORT, id);
        return 1;
    }
    if (ldr_stream_core(v) != nullptr) {
        embi_stream *s = ldr_stream_core(v);
        if (s->locked) {
            return 0;
        }
        /* streams.js makes the channel the stream travels on and gives back its far port. */
        embi_js_value port2 = ldr_prelude_call(c, "prepareTransfer", 1, &v);
        if (port2 == EMBI_JS_EXCEPTION) {
            return -1;
        }
        pw *pw2 = pw_of(port2);
        if (pw2 == nullptr) {
            return 0;
        }
        put_payload(payload, XF_STREAM, pw2->id);
        return 1;
    }
    return 0;
}

/* Takes a port away from its wrapper: the wrapper is dead and the record is left to be rebuilt by the receiver. */
static void port_detach(embi_bind_ctx *c, embi_js_value v)
{
    pw *w = pw_of(v);
    if (w == nullptr || w->id == 0) {
        return;
    }
    embi_mport *p = embi_mp_find(ldr_ports(c), w->id);
    w->id = 0;
    if (p != nullptr) {
        embi_mp_stop(p);
        unpin(c, p);
    }
}

void ldr_xfer_commit(embi_js_realm *r, embi_js_value v)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (pw_of(v) != nullptr) {
        port_detach(c, v);
        return;
    }
    if (ldr_stream_core(v) != nullptr) {
        embi_js_value port2 = ldr_prelude_call(c, "transferReadable", 1, &v);
        if (port2 == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            return;
        }
        port_detach(c, port2);
    }
}

void ldr_xfer_rollback(embi_js_realm *r, embi_js_value v)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (ldr_stream_core(v) != nullptr) {
        if (ldr_prelude_call(c, "cancelTransfer", 1, &v) == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
        }
    }
}

bool ldr_xfer_receive(embi_js_realm *r, embi_str payload, embi_js_value *out)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (payload.len != 5) {
        return false;
    }
    uint8_t kind = (uint8_t)payload.ptr[0];
    uint32_t id = 0;
    memcpy(&id, payload.ptr + 1, 4);
    embi_mport *p = embi_mp_find(ldr_ports(c), id);
    if (p == nullptr || (kind != XF_PORT && kind != XF_STREAM)) {
        (void)embi_js_throw_error(c->rt, EMBI_JS_PROTO_ERROR, "invalid structured clone data");
        return false;
    }
    embi_js_value pv = port_wrap(c, p);
    if (kind == XF_PORT) {
        *out = pv;
        return true;
    }
    embi_js_value s = ldr_prelude_call(c, "receiveReadable", 1, &pv);
    if (s == EMBI_JS_EXCEPTION) {
        return false;
    }
    *out = s;
    return true;
}
