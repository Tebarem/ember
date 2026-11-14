/*
 * WeakRef and FinalizationRegistry (ES2026).
 *
 * The target of a WeakRef and the targets, unregister tokens of a FinalizationRegistry are held
 * through weak handles (embi_js_weak_new). The collector clears a weak handle whose target is not
 * marked, so none of these keep their target alive. The held values of a registry are strong: the
 * registry's trace marks them, and its cleanup callback.
 *
 * WeakRef.deref and the WeakRef constructor add the target to the kept set (KeepDuringJob, jobs.c),
 * which the job queue clears once it drains. A collection can find that a registry's target has
 * died. Its cleanup is then queued as a host job (embi_js_weakref_after_gc, called after the
 * collection, where allocating is allowed). The callback never runs inside the collection.
 *
 * A record (a value array, rooted through the runtime's root list) keeps the two prototypes and
 * the list of live registries. The list holds weak handles to the registry objects, so a dead
 * registry drops out of it after the collection that killed it. Each registry is one list entry.
 */
#include "js/builtins.h"
#include "js/jobs.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/value.h"

#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* Record slots. */
enum {
    WR_WEAKREF_PROTO,
    WR_FR_PROTO,
    WR_REGISTRIES, /* value array of weak handle ids, as int32 values */
    WR_REG_COUNT,  /* int32: entries in use in WR_REGISTRIES */
    WR_BUSY,       /* int32: 1 while embi_js_weakref_after_gc runs (it may allocate) */
    WR_N
};

/* A WeakRef's state: its runtime and the weak handle of its target. */
typedef struct wr_state {
    embi_js_runtime *rt;
    uint32_t target;
} wr_state;

/* One registration: the weak handles of the target and the unregister token (0 when none), and
 * the held value (strong). */
typedef struct fr_cell {
    uint32_t target;
    uint32_t token;
    embi_js_value held;
} fr_cell;

/* A FinalizationRegistry's state. */
typedef struct fr_state {
    embi_js_runtime *rt;
    embi_js_value cleanup;
    EMBI_VEC(fr_cell) cells;
} fr_state;

static void weakref_root_mark(embi_js_tracer *t, void *user);
static embi_js_value proto_from_ctor(embi_js_runtime *rt, embi_js_value new_target,
                                     embi_js_value dflt);

/* ------------------------------------------------------------------ record and helpers */

/* The record of the runtime's realm, or nullptr when no realm was created (a bare runtime that
 * collects still gets the post-collection hook). */
static embi_js_varray *wr_record_find(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == weakref_root_mark) {
            return rt->roots.data[i].user;
        }
    }
    return nullptr;
}

/* The record, for code that runs only where a realm exists (the constructors and methods). */
static embi_js_varray *wr_record(embi_js_runtime *rt)
{
    embi_js_varray *rec = wr_record_find(rt);
    EMBI_ASSERT(rec != nullptr);
    return rec;
}

static void weakref_root_mark(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, user);
}

/* CanBeHeldWeakly: an object, or a symbol that is not in the Symbol.for registry. */
static bool can_hold_weakly(embi_js_value v)
{
    if (embi_js_is_object(v)) {
        return true;
    }
    return embi_js_is_symbol(v) && !embi_js_symbol_is_registered(v);
}

static bool is_obj_of(embi_js_value v, const embi_js_class *k)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == k;
}

/* Appends a weak handle id to the registry list, growing it when full. Growing allocates, and
 * an allocation may collect: the post-collection hook then compacts this list and lowers the
 * count. So the count is read again after the grow, and nothing allocates between that read and
 * the store. */
static void registry_push(embi_js_runtime *rt, uint32_t id)
{
    embi_js_varray *rec = wr_record(rt);
    embi_js_varray *list = (embi_js_varray *)embi_js_as_cell(rec->elems[WR_REGISTRIES]);
    uint32_t n = (uint32_t)embi_js_as_int32(rec->elems[WR_REG_COUNT]);
    if (n == list->hdr.aux) {
        list = embi_js_varray_grow(rt, list, n * 2u + 4u);
        EMBI_JS_STORE(rec, rec->elems[WR_REGISTRIES], embi_js_from_cell(list));
        n = (uint32_t)embi_js_as_int32(rec->elems[WR_REG_COUNT]);
    }
    EMBI_ASSERT(n < list->hdr.aux);
    EMBI_JS_STORE(list, list->elems[n], embi_js_from_int32((int32_t)id));
    EMBI_JS_STORE(rec, rec->elems[WR_REG_COUNT], embi_js_from_int32((int32_t)(n + 1u)));
}

/* ------------------------------------------------------------------ WeakRef */

static void weakref_finalize(embi_js_object *o)
{
    wr_state *st = o->internal;
    if (!st) {
        return;
    }
    embi_js_weak_free(st->rt, st->target);
    embi_free(st, sizeof *st);
    o->internal = nullptr;
}

static const embi_js_class class_weakref = {.name = "WeakRef", .finalize = weakref_finalize};

/* new WeakRef(target) (ES2026 26.1.1.1). */
static embi_js_value weakref_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(new_target)) {
        return embi_js_throw_type_error(rt, "Constructor WeakRef requires 'new'");
    }
    embi_js_value target = argc > 0 ? argv[0] : UNDEF;
    if (!can_hold_weakly(target)) {
        return embi_js_throw_type_error(rt, "WeakRef: target must be an object or a symbol");
    }
    embi_js_value dflt = wr_record(rt)->elems[WR_WEAKREF_PROTO];
    embi_js_value proto = proto_from_ctor(rt, new_target, dflt);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, &class_weakref);
    wr_state *st = embi_alloc(sizeof *st);
    st->rt = rt;
    st->target = embi_js_weak_new(rt, target);
    o->internal = st;
    EMBI_JS_WB_STRUCT(o);
    embi_js_keep_during_job(rt, target);
    return embi_js_from_object(o);
}

/* WeakRef.prototype.deref (ES2026 26.1.3.2). */
static embi_js_value weakref_deref(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_obj_of(this_v, &class_weakref)) {
        return embi_js_throw_type_error(rt, "WeakRef.prototype.deref: receiver is not a WeakRef");
    }
    wr_state *st = embi_js_as_object(this_v)->internal;
    embi_js_value t = embi_js_weak_get(rt, st->target);
    if (embi_js_is_empty(t)) {
        return UNDEF;
    }
    embi_js_keep_during_job(rt, t);
    return t;
}

/* ------------------------------------------------------------------ FinalizationRegistry */

static void fr_trace(embi_js_object *o, embi_js_tracer *t)
{
    fr_state *st = o->internal;
    if (!st) {
        return;
    }
    embi_js_mark_value(t, st->cleanup);
    for (uint32_t i = 0; i < st->cells.len; i++) {
        embi_js_mark_value(t, st->cells.data[i].held);
    }
    t->external += sizeof *st + st->cells.cap * sizeof(fr_cell);
}

static void fr_finalize(embi_js_object *o)
{
    fr_state *st = o->internal;
    if (!st) {
        return;
    }
    for (uint32_t i = 0; i < st->cells.len; i++) {
        embi_js_weak_free(st->rt, st->cells.data[i].target);
        if (st->cells.data[i].token) {
            embi_js_weak_free(st->rt, st->cells.data[i].token);
        }
    }
    embi_vec_free(&st->cells);
    embi_free(st, sizeof *st);
    o->internal = nullptr;
}

static const embi_js_class class_fr = {
    .name = "FinalizationRegistry", .trace = fr_trace, .finalize = fr_finalize};

/* new FinalizationRegistry(cleanupCallback) (ES2026 26.2.1.1). */
static embi_js_value fr_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(new_target)) {
        return embi_js_throw_type_error(rt, "Constructor FinalizationRegistry requires 'new'");
    }
    embi_js_value cleanup = argc > 0 ? argv[0] : UNDEF;
    if (!embi_js_is_callable(cleanup)) {
        return embi_js_throw_type_error(rt, "FinalizationRegistry: cleanup must be callable");
    }
    embi_js_value dflt = wr_record(rt)->elems[WR_FR_PROTO];
    embi_js_value proto = proto_from_ctor(rt, new_target, dflt);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, &class_fr);
    fr_state *st = embi_alloc(sizeof *st);
    memset(st, 0, sizeof *st);
    st->rt = rt;
    EMBI_JS_STORE(o, st->cleanup, cleanup);
    o->internal = st;
    EMBI_JS_WB_STRUCT(o);
    registry_push(rt, embi_js_weak_new(rt, embi_js_from_object(o)));
    return embi_js_from_object(o);
}

/* FinalizationRegistry.prototype.register (magic 0) and unregister (magic 1). */
static embi_js_value fr_method(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value a0 = argc > 0 ? argv[0] : UNDEF;
    embi_js_value a1 = argc > 1 ? argv[1] : UNDEF;
    embi_js_value a2 = argc > 2 ? argv[2] : UNDEF;
    if (!is_obj_of(this_v, &class_fr)) {
        return embi_js_throw_type_error(rt, "FinalizationRegistry method called on incompatible receiver");
    }
    fr_state *st = embi_js_as_object(this_v)->internal;
    if (magic == 0) {
        if (!can_hold_weakly(a0)) {
            return embi_js_throw_type_error(rt, "FinalizationRegistry.prototype.register: invalid target");
        }
        if (embi_js_same_value(a0, a1)) {
            return embi_js_throw_type_error(rt, "FinalizationRegistry.prototype.register: target and holdings must not be same");
        }
        if (!embi_js_is_undefined(a2) && !can_hold_weakly(a2)) {
            return embi_js_throw_type_error(rt, "FinalizationRegistry.prototype.register: invalid unregister token");
        }
        fr_cell c;
        c.target = embi_js_weak_new(rt, a0);
        c.token = embi_js_is_undefined(a2) ? 0u : embi_js_weak_new(rt, a2);
        c.held = a1;
        EMBI_JS_WB(embi_js_as_object(this_v), c.held);
        embi_vec_push(&st->cells, c);
        return UNDEF;
    }
    if (!can_hold_weakly(a0)) {
        return embi_js_throw_type_error(rt, "FinalizationRegistry.prototype.unregister: invalid unregister token");
    }
    bool removed = false;
    for (uint32_t i = st->cells.len; i-- > 0;) {
        fr_cell *c = &st->cells.data[i];
        if (c->token == 0 || !embi_js_same_value(embi_js_weak_get(rt, c->token), a0)) {
            continue;
        }
        embi_js_weak_free(rt, c->target);
        embi_js_weak_free(rt, c->token);
        memmove(c, c + 1, (size_t)(st->cells.len - i - 1u) * sizeof *c);
        st->cells.len--;
        removed = true;
    }
    return embi_js_from_bool(removed);
}

/* ------------------------------------------------------------------ after a collection */

/* Queues the cleanup job of each registration of st whose target died, and drops it. A token
 * that died is forgotten, and its registration stays (it can still clean up). */
static void fr_sweep(embi_js_runtime *rt, fr_state *st)
{
    for (uint32_t i = 0; i < st->cells.len;) {
        fr_cell *c = &st->cells.data[i];
        if (c->token && embi_js_is_empty(embi_js_weak_get(rt, c->token))) {
            embi_js_weak_free(rt, c->token);
            c->token = 0;
        }
        if (!embi_js_is_empty(embi_js_weak_get(rt, c->target))) {
            i++;
            continue;
        }
        /* The registration is gone, so its unregister token (still alive, or it would have been
         * dropped above) is released too; otherwise a long-lived token would keep one handle per
         * registration for the life of the runtime. */
        embi_js_weak_free(rt, c->target);
        if (c->token) {
            embi_js_weak_free(rt, c->token);
            c->token = 0;
        }
        embi_js_value held = c->held;
        memmove(c, c + 1, (size_t)(st->cells.len - i - 1u) * sizeof *c);
        st->cells.len--;
        embi_js_enqueue_job(rt, EMBI_JS_JOB_CALL, st->cleanup, held, UNDEF);
    }
}

void embi_js_weakref_after_gc(embi_js_runtime *rt)
{
    embi_js_varray *rec = wr_record_find(rt);
    if (!rec || embi_js_as_int32(rec->elems[WR_BUSY]) != 0) {
        /* No realm, or a collection that the cleanup queueing itself started: nothing more to do
         * on this pass. */
        return;
    }
    EMBI_JS_STORE(rec, rec->elems[WR_BUSY], embi_js_from_int32(1));
    embi_js_varray *list = (embi_js_varray *)embi_js_as_cell(rec->elems[WR_REGISTRIES]);
    uint32_t n = (uint32_t)embi_js_as_int32(rec->elems[WR_REG_COUNT]);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = (uint32_t)embi_js_as_int32(list->elems[i]);
        embi_js_value reg = embi_js_weak_get(rt, id);
        if (embi_js_is_empty(reg)) {
            /* The registry died: its finalizer freed its cells, and its entry goes now. */
            embi_js_weak_free(rt, id);
            continue;
        }
        EMBI_JS_STORE(list, list->elems[keep++], list->elems[i]);
        fr_state *st = embi_js_as_object(reg)->internal;
        if (st) {
            fr_sweep(rt, st);
        }
    }
    for (uint32_t i = keep; i < n; i++) {
        EMBI_JS_STORE(list, list->elems[i], EMBI_JS_EMPTY);
    }
    EMBI_JS_STORE(rec, rec->elems[WR_REG_COUNT], embi_js_from_int32((int32_t)keep));
    EMBI_JS_STORE(rec, rec->elems[WR_BUSY], embi_js_from_int32(0));
}

/* ------------------------------------------------------------------ setup */

/* GetPrototypeFromConstructor for the realm's own constructors: the new target's prototype when
 * it is an object, the intrinsic otherwise. */
static embi_js_value proto_from_ctor(embi_js_runtime *rt, embi_js_value new_target,
                                     embi_js_value dflt)
{
    embi_js_value p = embi_js_get(rt, embi_js_as_object(new_target),
                                  embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
    if (p == EXC) {
        return EXC;
    }
    return embi_js_is_object(p) ? p : dflt;
}

static void define_tag(embi_js_runtime *rt, embi_js_object *o, const char *tag)
{
    embi_js_value s = embi_js_from_string(embi_js_atom_intern_utf8(rt, tag, strlen(tag)));
    embi_js_define_raw(rt, o, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), s, ATTR_C);
}

/* The constructor object for a prototype: length 1, a non-writable prototype property, and the
 * global binding. */
static embi_js_object *make_ctor(embi_js_realm *r, const char *name, embi_js_native_ctor fn,
                                 embi_js_object *proto)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *ctor = embi_js_native_constructor_new(r, name, 1, fn, 0);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor), ATTR_WC);
    return ctor;
}

void embi_js_init_builtins_weakref(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];

    embi_js_varray *rec = embi_js_varray_new(rt, WR_N);
    EMBI_JS_STORE(rec, rec->elems[WR_REGISTRIES], embi_js_from_cell(embi_js_varray_new(rt, 8)));
    EMBI_JS_STORE(rec, rec->elems[WR_REG_COUNT], embi_js_from_int32(0));
    EMBI_JS_STORE(rec, rec->elems[WR_BUSY], embi_js_from_int32(0));
    embi_js_root_add(rt, weakref_root_mark, rec);

    embi_js_object *wr_proto = embi_js_object_new(rt, obj_proto);
    EMBI_JS_STORE(rec, rec->elems[WR_WEAKREF_PROTO], embi_js_from_object(wr_proto));
    embi_js_define_method(r, wr_proto, "deref", weakref_deref, 0, 0);
    define_tag(rt, wr_proto, "WeakRef");
    make_ctor(r, "WeakRef", weakref_ctor, wr_proto);

    embi_js_object *fr_proto = embi_js_object_new(rt, obj_proto);
    EMBI_JS_STORE(rec, rec->elems[WR_FR_PROTO], embi_js_from_object(fr_proto));
    embi_js_define_method(r, fr_proto, "register", fr_method, 2, 0);
    embi_js_define_method(r, fr_proto, "unregister", fr_method, 1, 1);
    define_tag(rt, fr_proto, "FinalizationRegistry");
    make_ctor(r, "FinalizationRegistry", fr_ctor, fr_proto);
}
