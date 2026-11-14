/*
 * Timers and microtasks on the Window (timers.webidl, W5c). The event loop is the
 * embedder's: the timer hooks of bind.h add and clear timers, and the embedder fires them through
 * embi_bind_fire_timer. A timer's record is a JS array [handler, arguments...] kept alive by a persistent handle; the
 * payload the loop carries is that handle plus one (so it is never null).
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/timers.h"
#include "bindings/gen/window.h"

#include <stdint.h>

#include <SDL3/SDL_time.h>

/* The payload of a timer: its persistent handle plus one. */
static void *payload_of(uint32_t pid)
{
    return (void *)(uintptr_t)(pid + 1u);
}

static uint32_t pid_of(const void *payload)
{
    return (uint32_t)((uintptr_t)payload - 1u);
}

static void release_payload(embi_bind_ctx *c, void *payload)
{
    if (payload != nullptr) {
        embi_js_persistent_free(c->rt, pid_of(payload));
    }
}

/* Schedules a timer for handler with its arguments. Returns the timer id as a number (0 when the loop cannot take it), or EMBI_JS_EXCEPTION with the exception pending. */
static embi_js_value timer_create(embi_bind_ctx *c, embi_js_value handler, int32_t timeout, bool repeat, int argc,
                            const embi_js_value *argv)
{
    if (c->timers.add == nullptr) {
        return embi_js_number(0);
    }
    embi_js_runtime *rt = c->rt;
    if (!embi_js_is_callable(handler)) {
        /* HTML: a handler that is not a function is converted to a string now (its toString may run script and set
         * timers of its own, which come first), and evaluated when the timer fires. */
        embi_js_string *hs = embi_js_to_string(rt, handler);
        if (hs == nullptr) {
            return EMBI_JS_EXCEPTION;
        }
        handler = embi_js_from_string(hs);
    }
    embi_js_value rec = embi_bind_array_new(c, 1u + (uint32_t)(argc > 0 ? argc : 0));
    embi_bind_array_set(c, rec, 0, handler);
    for (int i = 0; i < argc; i++) {
        embi_bind_array_set(c, rec, (uint32_t)i + 1u, argv[i]);
    }
    uint32_t pid = embi_js_persistent_new(rt, rec);
    /* HTML: a negative timeout is 0. */
    double ms = timeout < 0 ? 0.0 : (double)timeout;
    uint32_t id = c->timers.add(c->timers.user, ms, repeat, payload_of(pid));
    if (id == 0) {
        embi_js_persistent_free(rt, pid);
        return embi_js_number(0);
    }
    return embi_js_number((double)id);
}

static void timer_clear(embi_bind_ctx *c, int32_t id)
{
    if (id <= 0 || c->timers.clear == nullptr) {
        return;
    }
    release_payload(c, c->timers.clear(c->timers.user, (uint32_t)id));
}

embi_js_value embi_impl_Window_setTimeout(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, int32_t a1, int a2n,
                                    const embi_js_value *a2p)
{
    (void)self;
    return timer_create(c, a0, a1, false, a2n, a2p);
}

embi_js_value embi_impl_Window_setInterval(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, int32_t a1, int a2n,
                                     const embi_js_value *a2p)
{
    (void)self;
    return timer_create(c, a0, a1, true, a2n, a2p);
}

void embi_impl_Window_clearTimeout(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    timer_clear(c, a0);
}

void embi_impl_Window_clearInterval(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    timer_clear(c, a0);
}

/* The job of a queued microtask: calls the callback (its one argument) with no arguments. */
static embi_js_value microtask_job(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (argc < 1 || !embi_js_is_callable(argv[0])) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_js_call(r->rt, argv[0], EMBI_JS_UNDEFINED, 0, nullptr);
}

/* queueMicrotask(callback): a callback that is not callable is a TypeError now (the WebIDL callback conversion); a
 * callable runs at the next microtask checkpoint with no argument. */
embi_js_value embi_impl_Window_queueMicrotask(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    (void)self;
    if (!embi_js_is_callable(a0)) {
        return embi_js_throw_type_error(c->rt, "Failed to execute 'queueMicrotask' on 'Window': parameter 1 is not of type 'Function'.");
    }
    embi_js_object *job = embi_js_native_function_new_data(c->realm, "queueMicrotask", 0, microtask_job, 0, c);
    if (job == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_enqueue_microtask(c->rt, embi_js_from_object(job), a0);
    return EMBI_JS_UNDEFINED;
}

/* A string handler is evaluated as a script in the global scope when its timer fires. */
static embi_js_value eval_handler(embi_bind_ctx *c, embi_js_value h)
{
    embi_js_string *s = embi_js_to_string(c->rt, h);
    if (s == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    embi_sb sb = {0};
    embi_js_str_append_utf8(s, &sb);
    embi_js_value r = embi_js_eval(c->realm, sb.data, sb.len, "timer", 0);
    embi_sb_free(&sb);
    return r;
}

embi_js_value embi_bind_fire_timer(embi_bind_ctx *c, void *payload)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_runtime *rt = c->rt;
    embi_js_value rec = embi_js_persistent_get(rt, pid_of(payload));
    if (!embi_js_is_object(rec)) {
        return embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    uint32_t n = embi_bind_array_len(c, rec);
    if (n == 0) {
        return embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    embi_js_value h = embi_bind_array_get(c, rec, 0);
    if (!embi_js_is_callable(h)) {
        return embi_bind_leave(sc, eval_handler(c, h));
    }
    uint32_t argc = n - 1u;
    embi_js_value *argv = nullptr;
    if (argc > 0) {
        argv = embi_alloc(argc * sizeof *argv);
        for (uint32_t i = 0; i < argc; i++) {
            argv[i] = embi_bind_array_get(c, rec, i + 1u);
        }
    }
    /* The arguments stay reachable through rec, which the persistent handle keeps alive during the call. */
    embi_js_value r = embi_js_call(rt, h, EMBI_JS_UNDEFINED, (int)argc, argv);
    if (argv != nullptr) {
        embi_free(argv, argc * sizeof *argv);
    }
    return embi_bind_leave(sc, r);
}

void embi_bind_release_timer(embi_bind_ctx *c, void *payload)
{
    release_payload(c, payload);
}

void embi_bind_fire_lifecycle(embi_bind_ctx *c, const char *type)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool at_document = SDL_strcmp(type, "load") != 0;
    embi_bind_obj *target = at_document ? embi_impl_node_rec(c, (embi_node *)c->doc) : c->gbo;
    if (!at_document) {
        embi_impl_sync_body_window_handler(c, type);
    }
    if (target != nullptr) {
        /* DOMContentLoaded bubbles (to the window); readystatechange and load do not. */
        uint32_t flags = EMBI_EVF_TRUSTED | (SDL_strcmp(type, "DOMContentLoaded") == 0 ? EMBI_EVF_BUBBLES : 0u);
        embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, type, flags, EMBI_JS_UNDEFINED);
        if (ev == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            ev = EMBI_JS_UNDEFINED;
        }
        embi_impl_fire(c, target, type, ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_bind_fire_node_event(embi_bind_ctx *c, embi_node *node, const char *type)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *target = embi_impl_node_rec(c, node);
    if (target != nullptr) {
        embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, type, EMBI_EVF_TRUSTED, EMBI_JS_UNDEFINED);
        if (ev == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            ev = EMBI_JS_UNDEFINED;
        }
        embi_impl_fire(c, target, type, ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_bind_set_timers(embi_bind_ctx *c, const embi_bind_timers *t)
{
    if (t != nullptr) {
        c->timers = *t;
    } else {
        SDL_memset(&c->timers, 0, sizeof c->timers);
    }
}

/* ------------------------------------------------------------------ document.currentScript */

/* Pins the wrapper of node, so the element outlives its removal from the tree while the pin lives. The token is the
 * persistent handle plus one (never 0), or 0 when the wrapper could not be made. */
uint32_t embi_bind_pin_node(embi_bind_ctx *c, embi_node *node)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *bo = embi_impl_node_rec(c, node);
    uint32_t tok = 0;
    if (bo != nullptr) {
        tok = embi_js_persistent_new(c->rt, embi_bind_obj_value(bo)) + 1u;
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return tok;
}

void embi_bind_unpin_node(embi_bind_ctx *c, uint32_t token)
{
    if (token != 0) {
        embi_js_persistent_free(c->rt, token - 1u);
    }
}

/* The current script borrows a pin that its owner frees: the pin of a queued script, or one made for a running one. */
void embi_bind_set_current_script_token(embi_bind_ctx *c, uint32_t token)
{
    c->current_script = token;
}

void embi_bind_set_current_script(embi_bind_ctx *c, embi_node *node)
{
    embi_bind_unpin_node(c, c->current_script_own);
    c->current_script_own = 0;
    c->current_script = 0;
    if (node != nullptr) {
        c->current_script_own = embi_bind_pin_node(c, node);
        c->current_script = c->current_script_own;
    }
}

embi_js_value embi_impl_Document_get_currentScript(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    if (c->current_script == 0) {
        return EMBI_JS_NULL;
    }
    return embi_js_persistent_get(c->rt, c->current_script - 1u);
}

/* ------------------------------------------------------------------ reportError */

void embi_bind_report_error(embi_bind_ctx *c, embi_js_value error, const char *file, uint32_t line, uint32_t col)
{
    static thread_local int depth = 0;
    if (depth > 4) {
        embi_bind_log(c, EMBI_BIND_LOG_ERROR, EMBI_STR("uncaught exception: an error while reporting an error"));
        return;
    }
    depth++;
    embi_bind_scope sc = embi_bind_enter(c);
    (void)embi_bind_pin(c, error);
    embi_dstr msg = embi_dstr_empty();
    if (!embi_bind_to_dstr(c, error, &msg)) {
        embi_js_exception_clear(c->rt);
        msg = embi_impl_lit("exception");
    }
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, p, "message", embi_bind_dstr_value(c, msg));
    embi_bind_object_set_c(c, p, "error", error);
    if (file != nullptr) {
        embi_bind_object_set_c(c, p, "filename", embi_bind_utf8_value(c, (embi_str){file, (uint32_t)SDL_strlen(file)}));
        embi_bind_object_set_c(c, p, "lineno", embi_js_number((double)line));
        embi_bind_object_set_c(c, p, "colno", embi_js_number((double)col));
    }
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_ErrorEvent, "error", EMBI_EVF_CANCELABLE | EMBI_EVF_TRUSTED, p);
    bool canceled = false;
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        (void)embi_bind_pin(c, ev);
        canceled = !embi_impl_dispatch_value(c, c->gbo, ev);
    }
    if (!canceled) {
        embi_sb text = {0};
        embi_sb_append(&text, EMBI_STR("uncaught exception: "));
        embi_sb_append(&text, embi_bind_utf8(c, msg));
        if (file != nullptr) {
            char loc[32];
            SDL_snprintf(loc, sizeof loc, ":%u:%u)", line, col);
            embi_sb_append(&text, EMBI_STR(" ("));
            embi_sb_append(&text, (embi_str){file, (uint32_t)SDL_strlen(file)});
            embi_sb_append(&text, (embi_str){loc, (uint32_t)SDL_strlen(loc)});
        }
        embi_str line_text = {embi_sb_cstr(&text), text.len};
        if (c->host.uncaught != nullptr) {
            c->host.uncaught(c->host.user, line_text);
        } else {
            embi_bind_log(c, EMBI_BIND_LOG_ERROR, line_text);
        }
        embi_sb_free(&text);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    depth--;
}

void embi_impl_Window_reportError(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    (void)self;
    embi_bind_report_error(c, a0, nullptr, 0, 0);
}

/* ------------------------------------------------------------------ performance */

static double time_origin(embi_bind_ctx *c)
{
    return c->host.time_origin_ms != nullptr ? c->host.time_origin_ms(c->host.user) : 0.0;
}

embi_js_value embi_impl_Window_get_performance(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    if (c->perf_obj != 0) {
        return embi_js_persistent_get(c->rt, c->perf_obj - 1u);
    }
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_Performance, c, nullptr, 0);
    c->perf_obj = embi_js_persistent_new(c->rt, v) + 1u;
    return v;
}

double embi_impl_Performance_now(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    double t = embi_bind_now_ms(c) - time_origin(c);
    /* coarsened to 5 microseconds (the resolution limit of a safe high resolution clock) */
    return t < 0 ? 0 : SDL_floor(t * 200.0) / 200.0;
}

double embi_impl_Performance_get_timeOrigin(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    /* The Unix time in ms at the time origin: the wall clock now, minus the time since the origin. */
    SDL_Time ns = 0;
    double wall_ms = SDL_GetCurrentTime(&ns) ? (double)ns / 1e6 : 0.0;
    return wall_ms - (embi_bind_now_ms(c) - time_origin(c));
}

/* The array of entries (marks and measures) of the context, made on first use. */
static embi_js_value perf_entries(embi_bind_ctx *c)
{
    if (c->perf_entries == 0) {
        c->perf_entries = embi_js_persistent_new(c->rt, embi_bind_array_new(c, 0)) + 1u;
    }
    return embi_js_persistent_get(c->rt, c->perf_entries - 1u);
}

static embi_js_value entry_get(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_UNDEFINED;
    }
    return v;
}

static bool js_str_is(embi_js_value a, embi_js_value b)
{
    return embi_js_is_string(a) && embi_js_is_string(b) && embi_js_str_equal(embi_js_as_string(a), embi_js_as_string(b));
}

static embi_js_value type_value(embi_bind_ctx *c, const char *type)
{
    return embi_bind_utf8_value(c, (embi_str){type, (uint32_t)SDL_strlen(type)});
}

static embi_js_value entry_add(embi_bind_ctx *c, embi_js_value name, const char *type, double start, double duration)
{
    embi_js_value e = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, e, "name", name);
    embi_bind_object_set_c(c, e, "entryType", type_value(c, type));
    embi_bind_object_set_c(c, e, "startTime", embi_js_number(start));
    embi_bind_object_set_c(c, e, "duration", embi_js_number(duration));
    embi_js_value arr = perf_entries(c);
    embi_bind_array_set(c, arr, embi_bind_array_len(c, arr), e);
    return e;
}

/* The start time of the newest mark called name, or false. */
static bool mark_time(embi_bind_ctx *c, embi_js_value name, double *out)
{
    embi_js_value arr = perf_entries(c);
    embi_js_value mark_v = embi_bind_pin(c, type_value(c, "mark"));
    for (uint32_t i = embi_bind_array_len(c, arr); i-- > 0;) {
        embi_js_value e = embi_bind_array_get(c, arr, i);
        if (!embi_js_is_object(e) || !js_str_is(entry_get(c, e, "entryType"), mark_v) ||
            !js_str_is(entry_get(c, e, "name"), name)) {
            continue;
        }
        double d = 0;
        if (embi_js_to_number(c->rt, entry_get(c, e, "startTime"), &d)) {
            *out = d;
            return true;
        }
        embi_js_exception_clear(c->rt);
    }
    return false;
}

embi_js_value embi_impl_Performance_mark(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    double t = embi_impl_Performance_now(c, nullptr);
    return entry_add(c, embi_bind_dstr_value(c, a0), "mark", t, 0.0);
}

/* A point of a measure: the mark's time, the default when no name is given, or false with a SyntaxError pending. */
static bool point(embi_bind_ctx *c, embi_bind_nstr n, double dflt, double *out, embi_js_value *err)
{
    if (n.is_null) {
        *out = dflt;
        return true;
    }
    if (!mark_time(c, embi_bind_dstr_value(c, n.s), out)) {
        *err = embi_bind_throw_named(c, "SyntaxError", "The mark does not exist");
        return false;
    }
    return true;
}

embi_js_value embi_impl_Performance_measure(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_bind_nstr a1,
                                            embi_bind_nstr a2)
{
    (void)self;
    double now = embi_impl_Performance_now(c, nullptr);
    double start = 0, end = now;
    embi_js_value err = EMBI_JS_UNDEFINED;
    if (!point(c, a1, 0.0, &start, &err) || !point(c, a2, now, &end, &err)) {
        return err;
    }
    return entry_add(c, embi_bind_dstr_value(c, a0), "measure", start, end - start);
}

/* Removes the entries of a type (and of a name, unless the name is null) by rebuilding the array. */
static void clear_entries(embi_bind_ctx *c, const char *type, embi_bind_nstr name)
{
    if (c->perf_entries == 0) {
        return;
    }
    embi_js_value old = perf_entries(c);
    embi_js_value fresh = embi_bind_pin(c, embi_bind_array_new(c, 0));
    embi_js_value type_v = embi_bind_pin(c, type_value(c, type));
    embi_js_value name_v = name.is_null ? EMBI_JS_UNDEFINED : embi_bind_pin(c, embi_bind_dstr_value(c, name.s));
    uint32_t n = embi_bind_array_len(c, old), w = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value e = embi_bind_array_get(c, old, i);
        bool drop = embi_js_is_object(e) && js_str_is(entry_get(c, e, "entryType"), type_v) &&
                    (name.is_null || js_str_is(entry_get(c, e, "name"), name_v));
        if (!drop) {
            embi_bind_array_set(c, fresh, w++, e);
        }
    }
    embi_js_persistent_set(c->rt, c->perf_entries - 1u, fresh);
}

void embi_impl_Performance_clearMarks(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0)
{
    (void)self;
    clear_entries(c, "mark", a0);
}

void embi_impl_Performance_clearMeasures(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0)
{
    (void)self;
    clear_entries(c, "measure", a0);
}
