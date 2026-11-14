/*
 * Web Storage (storage.webidl, W7c): Storage over the area of the backend (src/loader/storage.c), Window.localStorage and
 * Window.sessionStorage, and StorageEvent. The keys and values are UTF-16 code unit strings. localStorage belongs to the
 * document's origin (an opaque origin throws SecurityError); sessionStorage is a memory-only area of the context. A change
 * to localStorage is announced as a storage event to the other contexts that share the backend, through the environment's
 * post hook, so the event runs on the other context's own thread. The backend flushes write-behind: each mutation ticks it,
 * and the runtime flushes it when it is destroyed.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/events.h"
#include "bindings/gen/storage.h"
#include "bindings/gen/window.h"

/* UTF-16 text of a dstr: a view over it when wide, else a widened copy (free with u16_release). */
typedef struct u16buf {
    embi_u16view v;
    uint16_t *own;
} u16buf;

static u16buf u16_of(embi_dstr d)
{
    u16buf b = {{nullptr, d.len}, nullptr};
    if (d.len == 0) {
        return b;
    }
    if (d.wide) {
        b.v.p = d.ptr;
        return b;
    }
    b.own = embi_alloc((size_t)d.len * sizeof(uint16_t));
    const uint8_t *s = d.ptr;
    for (uint32_t i = 0; i < d.len; i++) {
        b.own[i] = s[i];
    }
    b.v.p = b.own;
    return b;
}

static void u16_release(u16buf *b)
{
    if (b->own != nullptr) {
        embi_free(b->own, (size_t)b->v.len * sizeof(uint16_t));
        b->own = nullptr;
    }
}

/* A named-property key of Storage: a string, or an array index (localStorage[0] is the item named "0"). */
static bool key_is_name(embi_js_key k)
{
    return embi_js_is_string(k) || embi_js_key_is_index(k);
}

/* The UTF-16 units of a string property key, copied (lone surrogates preserved). The key is an atom, so it is flat.
 * An index key is its decimal digits. */
static u16buf u16_of_key(embi_js_key k)
{
    if (embi_js_key_is_index(k)) {
        char digits[16];
        int n = SDL_snprintf(digits, sizeof digits, "%u", (unsigned)embi_js_key_index(k));
        u16buf d = {{nullptr, (uint32_t)n}, nullptr};
        d.own = embi_alloc((size_t)n * sizeof(uint16_t));
        if (!d.own) {
            d.v.len = 0;
            return d;
        }
        for (int i = 0; i < n; i++) {
            d.own[i] = (uint16_t)digits[i];
        }
        d.v.p = d.own;
        return d;
    }
    const embi_js_string *str = embi_js_as_string(k);
    uint32_t n = embi_js_str_len(str);
    u16buf b = {{nullptr, n}, nullptr};
    if (n > 0) {
        b.own = embi_alloc((size_t)n * sizeof(uint16_t));
        for (uint32_t i = 0; i < n; i++) {
            b.own[i] = embi_js_str_at(str, i);
        }
        b.v.p = b.own;
    }
    return b;
}

static embi_js_value u16_value(embi_bind_ctx *c, embi_u16view v)
{
    return embi_bind_dstr_value(c, embi_bind_tmp_units(c, v.p, v.len));
}

static embi_storage_area *area_of(embi_bind_obj *self)
{
    return embi_bind_stale(self) ? nullptr : self->ptr;
}

static bool is_session(embi_bind_ctx *c, embi_storage_area *a)
{
    return a == ldr_session(c);
}

/* ------------------------------------------------------------------ operations */

uint32_t embi_impl_Storage_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_storage_area *a = area_of(self);
    return a != nullptr ? embi_storage_area_len(a) : 0;
}

embi_js_value embi_impl_Storage_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    embi_storage_area *a = area_of(self);
    embi_u16view k;
    if (a == nullptr || !embi_storage_area_key(a, a0, &k)) {
        return EMBI_JS_NULL;
    }
    return u16_value(c, k);
}

embi_dom_err embi_impl_Storage_getItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value *out)
{
    embi_storage_area *a = area_of(self);
    u16buf k = u16_of(a0);
    embi_u16view v;
    *out = (a != nullptr && embi_storage_area_get(a, k.v, &v)) ? u16_value(c, v) : EMBI_JS_NULL;
    u16_release(&k);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ QuotaExceededError */

/* QuotaExceededError (WebIDL 3.14.2.2) is a DOMException with the legacy code 22 and two read-only members, quota and
 * requested (numbers or null). The wrapper's payload extends embi_bind_dom_exc, which the DOMException accessors read,
 * so name, message and code come from the base. */
typedef struct quota_exc {
    embi_bind_dom_exc base;
    double quota, requested;
    bool has_quota, has_requested;
} quota_exc;

static embi_js_value quota_exc_new(embi_bind_ctx *c, embi_dstr message, bool has_quota, double quota, bool has_requested,
                                   double requested)
{
    static const char name[] = "QuotaExceededError";
    const size_t name_len = sizeof name - 1;
    size_t name_off = (sizeof(quota_exc) + 7u) & ~(size_t)7u;
    size_t msg_off = (name_off + name_len + 7u) & ~(size_t)7u;
    size_t msg_bytes = (size_t)message.len * (message.wide ? 2u : 1u);
    size_t total = msg_off + msg_bytes;
    uint8_t *block = embi_calloc(1, total);
    quota_exc *q = (quota_exc *)block;
    memcpy(block + name_off, name, name_len);
    if (msg_bytes > 0) {
        memcpy(block + msg_off, message.ptr, msg_bytes);
    }
    q->base.code = 22;
    q->base.name = (embi_dstr){block + name_off, (uint32_t)name_len, false};
    q->base.message = (embi_dstr){msg_bytes > 0 ? block + msg_off : nullptr, message.len, message.wide};
    q->has_quota = has_quota;
    q->quota = quota;
    q->has_requested = has_requested;
    q->requested = requested;
    return embi_bind_other_value(c, EMBI_IF_QuotaExceededError, q, block, (uint32_t)total);
}

/* One member of QuotaExceededErrorOptions: a double that is not NaN, infinite or negative. False with an exception pending. */
static bool quota_option(embi_bind_ctx *c, embi_js_value options, const char *name, bool *present, double *out)
{
    *present = false;
    embi_js_value v = ldr_get(c, options, name);
    if (v == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (embi_js_is_undefined(v)) {
        return true;
    }
    double d;
    if (!embi_js_to_number(c->rt, v, &d)) {
        return false;
    }
    if (d != d || d - d != 0) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'QuotaExceededError': The %s member is not a finite number.",
                                       name);
        return false;
    }
    if (d < 0) {
        (void)embi_js_throw_range_error(c->rt, "Failed to construct 'QuotaExceededError': The %s member must not be negative.",
                                        name);
        return false;
    }
    *present = true;
    *out = d;
    return true;
}

embi_js_value embi_impl_QuotaExceededError_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                       embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool hq = false, hr = false;
    double q = 0, r = 0;
    if (!embi_js_is_nullish(a1) && !embi_js_is_object(a1)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'QuotaExceededError': The options are not an object.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (embi_js_is_object(a1) && (!quota_option(c, a1, "quota", &hq, &q) || !quota_option(c, a1, "requested", &hr, &r))) {
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (hq && hr && r < q) {
        (void)embi_js_throw_range_error(c->rt, "Failed to construct 'QuotaExceededError': requested is less than quota.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value v = quota_exc_new(c, a0, hq, q, hr, r);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_QuotaExceededError_get_quota(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const quota_exc *q = self->ptr;
    return q->has_quota ? embi_js_number(q->quota) : EMBI_JS_NULL;
}

embi_js_value embi_impl_QuotaExceededError_get_requested(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const quota_exc *q = self->ptr;
    return q->has_requested ? embi_js_number(q->requested) : EMBI_JS_NULL;
}

/* What Storage throws when the quota is exceeded (HTML 12.2.4.2): quota and requested are null. */
static void throw_quota(embi_bind_ctx *c)
{
    static const char msg[] = "Setting the value exceeded the quota.";
    embi_dstr md = {msg, (uint32_t)sizeof msg - 1, false};
    (void)embi_js_throw_value(c->rt, quota_exc_new(c, md, false, 0, false, 0));
}

/* Store, announce, tick. Returns false (QuotaExceededError thrown) when over quota. */
static bool store_item(embi_bind_ctx *c, embi_storage_area *a, embi_u16view k, embi_u16view v)
{
    embi_u16view old = {nullptr, 0};
    bool had = embi_storage_area_get(a, k, &old);
    /* old is invalidated by the mutation: keep a copy for the event. */
    uint16_t *oldcopy = nullptr;
    uint32_t oldlen = had ? old.len : 0;
    if (oldlen > 0) {
        oldcopy = embi_alloc((size_t)oldlen * sizeof(uint16_t));
        memcpy(oldcopy, old.p, (size_t)oldlen * sizeof(uint16_t));
    }
    embi_u16view oldv = {oldcopy, oldlen};
    bool ok = true;
    if (had && oldlen == v.len && (v.len == 0 || memcmp(oldcopy, v.p, (size_t)v.len * sizeof(uint16_t)) == 0)) {
        /* Setting the same value: no change, no event. */
    } else {
        ok = embi_storage_area_set(a, k, v) == EMBI_STORAGE_OK;
        if (ok && !is_session(c, a)) {
            ldr_storage_changed(c, a, &k, had ? &oldv : nullptr, &v);
        }
    }
    if (oldcopy != nullptr) {
        embi_free(oldcopy, (size_t)oldlen * sizeof(uint16_t));
    }
    if (!ok) {
        throw_quota(c);
    }
    embi_storage_tick(ldr_storage(c));
    return ok;
}

embi_dom_err embi_impl_Storage_setItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    embi_storage_area *a = area_of(self);
    if (a == nullptr) {
        return EMBI_DOM_OK;
    }
    u16buf k = u16_of(a0), v = u16_of(a1);
    bool ok = store_item(c, a, k.v, v.v);
    u16_release(&k);
    u16_release(&v);
    return ok ? EMBI_DOM_OK : EMBI_BIND_THREW;
}

static void remove_item(embi_bind_ctx *c, embi_storage_area *a, embi_u16view k)
{
    embi_u16view old;
    if (!embi_storage_area_get(a, k, &old)) {
        return;
    }
    uint16_t *oldcopy = nullptr;
    uint32_t oldlen = old.len;
    if (oldlen > 0) {
        oldcopy = embi_alloc((size_t)oldlen * sizeof(uint16_t));
        memcpy(oldcopy, old.p, (size_t)oldlen * sizeof(uint16_t));
    }
    embi_u16view oldv = {oldcopy, oldlen};
    embi_storage_area_remove(a, k);
    if (!is_session(c, a)) {
        ldr_storage_changed(c, a, &k, &oldv, nullptr);
    }
    if (oldcopy != nullptr) {
        embi_free(oldcopy, (size_t)oldlen * sizeof(uint16_t));
    }
    embi_storage_tick(ldr_storage(c));
}

void embi_impl_Storage_removeItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_storage_area *a = area_of(self);
    if (a == nullptr) {
        return;
    }
    u16buf k = u16_of(a0);
    remove_item(c, a, k.v);
    u16_release(&k);
}

void embi_impl_Storage_clear(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_storage_area *a = area_of(self);
    if (a == nullptr || embi_storage_area_len(a) == 0) {
        return;
    }
    embi_storage_area_clear(a);
    if (!is_session(c, a)) {
        ldr_storage_changed(c, a, nullptr, nullptr, nullptr);
    }
    embi_storage_tick(ldr_storage(c));
}

/* ------------------------------------------------------------------ named properties */

/* Storage is a legacy platform object with a named getter, setter and deleter and no [LegacyOverrideBuiltIns]
 * (WebIDL 3.9): the item named P is the named property P unless an object of the prototype chain has a property P (the
 * named property visibility algorithm), so storage.key and storage.getItem stay the methods. A string key (an index is
 * its digits) goes to the setter and deleter; a symbol key is an ordinary property. */

/* The named property of Storage for k: visible, present. Sets *found and fills out. */
static bool named_lookup(embi_js_runtime *rt, embi_js_object *o, embi_bind_obj *bo, embi_js_key k, embi_js_pdesc *out)
{
    if (bo == nullptr || bo->ctx == nullptr || embi_bind_stale(bo) || !key_is_name(k) || embi_bind_proto_has(rt, o, k)) {
        return false;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_bind_scope sc = embi_bind_enter(c);
    u16buf kb = u16_of_key(k);
    embi_u16view v;
    bool found = embi_storage_area_get(bo->ptr, kb.v, &v);
    u16_release(&kb);
    if (found) {
        out->value = u16_value(c, v);
        out->get = EMBI_JS_UNDEFINED;
        out->set = EMBI_JS_UNDEFINED;
        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
        out->attrs = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return found;
}

int embi_impl_Storage_named_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    if (named_lookup(rt, o, o->internal, k, out)) {
        return EMBI_JS_YES;
    }
    return embi_bind_own_named(rt, o, k, out);
}

/* The named property setter: ToString(value), then the backend. YES, or THREW (QuotaExceededError, or ToString threw). */
static int named_store(embi_js_object *o, embi_js_key k, embi_js_value value)
{
    embi_bind_obj *bo = o->internal;
    if (bo == nullptr || bo->ctx == nullptr || embi_bind_stale(bo)) {
        return EMBI_JS_NO;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_dstr text;
    if (!embi_bind_to_dstr(c, value, &text)) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_JS_THREW;
    }
    u16buf kb = u16_of_key(k);
    u16buf vb = u16_of(text);
    bool ok = store_item(c, bo->ptr, kb.v, vb.v);
    u16_release(&kb);
    u16_release(&vb);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok ? EMBI_JS_YES : EMBI_JS_THREW;
}

/* The ordinary [[DefineOwnProperty]] of o: the class hook that called us replaces it. */
static int define_ordinary(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, const embi_js_pdesc *d)
{
    const embi_js_class *saved = o->klass;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_define_own_property(rt, o, k, d);
    o->klass = saved;
    return r;
}

int embi_impl_Storage_named_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, const embi_js_pdesc *d)
{
    embi_js_pdesc own;
    if (!key_is_name(k) || embi_bind_own_named(rt, o, k, &own) == EMBI_JS_YES) {
        return define_ordinary(rt, o, k, d);
    }
    /* WebIDL [[DefineOwnProperty]]: a data descriptor goes to the named property setter; any other is refused. */
    if ((d->has & (EMBI_JS_PD_GET | EMBI_JS_PD_SET)) || !(d->has & (EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE))) {
        return EMBI_JS_NO;
    }
    return named_store(o, k, (d->has & EMBI_JS_PD_VALUE) ? d->value : EMBI_JS_UNDEFINED);
}

/* [[Set]]: an assignment to a string key of the object itself is the named setter, whatever the prototype chain holds
 * (an accessor there is not called); every other case is OrdinarySetWithOwnDescriptor. */
int embi_impl_Storage_named_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                                embi_js_value receiver)
{
    if (receiver == embi_js_from_object(o) && key_is_name(k)) {
        return named_store(o, k, v);
    }
    embi_js_pdesc own;
    int r = embi_js_obj_get_own_property(rt, o, k, &own);
    if (r == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (r != EMBI_JS_YES) {
        embi_js_value p = embi_js_obj_proto(o);
        if (embi_js_is_object(p)) {
            return embi_js_obj_set(rt, embi_js_as_object(p), k, v, receiver);
        }
        own = (embi_js_pdesc){EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                              EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE,
                              EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE};
    }
    if (embi_js_pd_is_accessor(&own)) {
        if (embi_js_is_undefined(own.set)) {
            return EMBI_JS_NO;
        }
        return embi_js_call(rt, own.set, receiver, 1, &v) == EMBI_JS_EXCEPTION ? EMBI_JS_THREW : EMBI_JS_YES;
    }
    if (!(own.attrs & EMBI_JS_ATTR_WRITABLE) || !embi_js_is_object(receiver)) {
        return EMBI_JS_NO;
    }
    embi_js_object *rcv = embi_js_as_object(receiver);
    embi_js_pdesc ex;
    r = embi_js_obj_get_own_property(rt, rcv, k, &ex);
    if (r == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (r == EMBI_JS_YES) {
        if (embi_js_pd_is_accessor(&ex) || !(ex.attrs & EMBI_JS_ATTR_WRITABLE)) {
            return EMBI_JS_NO;
        }
        embi_js_pdesc vd = {v, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_PD_VALUE, 0};
        return embi_js_obj_define_own_property(rt, rcv, k, &vd);
    }
    embi_js_pdesc nd = {v,
                        EMBI_JS_UNDEFINED,
                        EMBI_JS_UNDEFINED,
                        EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE,
                        EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE};
    return embi_js_obj_define_own_property(rt, rcv, k, &nd);
}

int embi_impl_Storage_named_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_bind_obj *bo = o->internal;
    embi_js_pdesc ignore;
    if (named_lookup(rt, o, bo, k, &ignore)) {
        embi_bind_ctx *c = bo->ctx;
        embi_bind_scope sc = embi_bind_enter(c);
        u16buf kb = u16_of_key(k);
        remove_item(c, bo->ptr, kb.v);
        u16_release(&kb);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_JS_YES;
    }
    const embi_js_class *saved = o->klass;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_delete(rt, o, k);
    o->klass = saved;
    return r;
}

int embi_impl_Storage_named_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    embi_bind_obj *bo = o->internal;
    if (bo != nullptr && bo->ctx != nullptr && !embi_bind_stale(bo)) {
        embi_bind_ctx *c = bo->ctx;
        embi_bind_scope sc = embi_bind_enter(c);
        uint32_t n = embi_storage_area_len(bo->ptr);
        for (uint32_t i = 0; i < n; i++) {
            embi_u16view kv;
            if (!embi_storage_area_key(bo->ptr, i, &kv)) {
                continue;
            }
            embi_js_key k = embi_js_key_from_str(c->rt, embi_js_str_new_utf16(c->rt, kv.p, kv.len));
            if (!embi_bind_proto_has(rt, o, k)) {
                embi_vec_push(out, k);
            }
        }
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return embi_bind_own_keys_ordinary(rt, o, out);
}

/* ------------------------------------------------------------------ Window.localStorage / sessionStorage */

/* The localStorage area of the context's document origin, or nullptr for an opaque origin. */
static embi_storage_area *local_area_of(embi_bind_ctx *c)
{
    embi_url u = {0};
    embi_sb origin = {nullptr, 0, 0};
    bool ok = embi_url_parse(embi_bind_url(c), nullptr, &u);
    if (ok) {
        embi_url_origin(&u, &origin);
        embi_url_free(&u);
    }
    bool opaque = !ok || origin.len == 0 || (origin.len == 4 && memcmp(origin.data, "null", 4) == 0);
    embi_storage_area *a = nullptr;
    if (!opaque) {
        a = embi_storage_local(ldr_storage(c), (embi_str){origin.data, origin.len});
    }
    embi_sb_free(&origin);
    return a;
}

embi_dom_err embi_impl_Window_get_localStorage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    (void)self;
    embi_storage_area *a = local_area_of(c);
    if (a == nullptr) {
        (void)embi_bind_throw_named(c, "SecurityError",
                                    "Failed to read the 'localStorage' property from 'Window': Access is denied for this document.");
        return EMBI_BIND_THREW;
    }
    *out = embi_bind_other_value(c, EMBI_IF_Storage, a, nullptr, 0);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Window_get_sessionStorage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    (void)self;
    *out = embi_bind_other_value(c, EMBI_IF_Storage, ldr_session(c), nullptr, 0);
    return EMBI_DOM_OK;
}

void ldr_storage_release(ldr_state *st)
{
    (void)st;
}

/* ------------------------------------------------------------------ StorageEvent */

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

static embi_js_value nstr_value(embi_bind_ctx *c, embi_bind_nstr s)
{
    return s.is_null ? EMBI_JS_NULL : embi_bind_dstr_value(c, s.s);
}

static uint32_t flags_of(const embi_dict_EventInit *d)
{
    return (d->bubbles ? EMBI_EVF_BUBBLES : 0u) | (d->cancelable ? EMBI_EVF_CANCELABLE : 0u) |
           (d->composed ? EMBI_EVF_COMPOSED : 0u);
}

embi_js_value embi_impl_StorageEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                 embi_dict_StorageEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    pset(c, p, "key", nstr_value(c, a1.key));
    pset(c, p, "oldValue", nstr_value(c, a1.oldValue));
    pset(c, p, "newValue", nstr_value(c, a1.newValue));
    pset(c, p, "url", embi_bind_dstr_value(c, a1.url));
    pset(c, p, "storageArea", embi_js_is_undefined(a1.storageArea) ? EMBI_JS_NULL : a1.storageArea);
    embi_str type = embi_bind_utf8(c, a0);
    char *t = embi_bind_alloc(c, (size_t)type.len + 1u);
    memcpy(t, type.ptr, type.len);
    t[type.len] = 0;
    embi_js_value v = embi_impl_event_make(c, EMBI_IF_StorageEvent, t, flags_of(&a1.base), p);
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_StorageEvent_get_key(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "key");
}

embi_js_value embi_impl_StorageEvent_get_oldValue(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "oldValue");
}

embi_js_value embi_impl_StorageEvent_get_newValue(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "newValue");
}

embi_dstr embi_impl_StorageEvent_get_url(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "url");
    embi_dstr d;
    if (v == EMBI_JS_NULL || !embi_bind_to_dstr(c, v, &d)) {
        if (v != EMBI_JS_NULL) {
            embi_js_exception_clear(c->rt);
        }
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    return d;
}

embi_js_value embi_impl_StorageEvent_get_storageArea(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "storageArea");
}

/* initStorageEvent (legacy): the initEvent steps, then the members. Nothing happens while the event is dispatched. */
void embi_impl_StorageEvent_initStorageEvent(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1, bool a2,
                                             embi_bind_nstr a3, embi_bind_nstr a4, embi_bind_nstr a5, embi_dstr a6,
                                             embi_js_value a7)
{
    if (embi_bind_stale(self) || (((const embi_event *)self->ptr)->flags & EMBI_EVF_DISPATCHING) != 0) {
        return;
    }
    embi_impl_Event_initEvent(c, self, a0, a1, a2);
    pset(c, self->ref, "key", nstr_value(c, a3));
    pset(c, self->ref, "oldValue", nstr_value(c, a4));
    pset(c, self->ref, "newValue", nstr_value(c, a5));
    pset(c, self->ref, "url", embi_bind_dstr_value(c, a6));
    pset(c, self->ref, "storageArea", embi_js_is_nullish(a7) ? EMBI_JS_NULL : a7);
}

/* ------------------------------------------------------------------ cross-context delivery */

typedef struct sevent {
    embi_bind_ctx *c;
    embi_js_realm *realm;
    const embi_storage_area *src; /* the changed area; compared only, never dereferenced */
    char *url;                    /* the writer's document URL */
    uint32_t nurl;
    bool has_key, has_old, has_new;
    uint16_t *key, *old_v, *new_v;
    uint32_t nkey, nold, nnew;
} sevent;

static uint16_t *dup16(const embi_u16view *v)
{
    if (v->len == 0) {
        return nullptr;
    }
    uint16_t *p = embi_alloc((size_t)v->len * sizeof *p);
    memcpy(p, v->p, (size_t)v->len * sizeof *p);
    return p;
}

static void sevent_free(sevent *e)
{
    if (e->url != nullptr) {
        embi_free(e->url, e->nurl);
    }
    if (e->key != nullptr) {
        embi_free(e->key, (size_t)e->nkey * sizeof(uint16_t));
    }
    if (e->old_v != nullptr) {
        embi_free(e->old_v, (size_t)e->nold * sizeof(uint16_t));
    }
    if (e->new_v != nullptr) {
        embi_free(e->new_v, (size_t)e->nnew * sizeof(uint16_t));
    }
    embi_free(e, sizeof *e);
}

static void sevent_drop(void *arg)
{
    sevent_free(arg);
}

static void sevent_run(void *arg)
{
    sevent *e = arg;
    if (!ldr_ctx_live(e->c, e->realm)) {
        sevent_free(e);
        return;
    }
    embi_bind_ctx *c = e->c;
    /* Only documents of the same (non-opaque) origin share the area; this runs on the receiver's own thread, where its
     * URL may be read. */
    if (local_area_of(c) != e->src) {
        sevent_free(e);
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value area = EMBI_JS_NULL;
    /* Through the property, so the event carries the SameObject wrapper the page sees. */
    area = ldr_get(c, embi_js_from_object(c->realm->global), "localStorage");
    if (area == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        area = EMBI_JS_NULL;
    }
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    pset(c, p, "key", e->has_key ? u16_value(c, (embi_u16view){e->key, e->nkey}) : EMBI_JS_NULL);
    pset(c, p, "oldValue", e->has_old ? u16_value(c, (embi_u16view){e->old_v, e->nold}) : EMBI_JS_NULL);
    pset(c, p, "newValue", e->has_new ? u16_value(c, (embi_u16view){e->new_v, e->nnew}) : EMBI_JS_NULL);
    pset(c, p, "url", embi_bind_dstr_value(c, embi_bind_tmp_utf8(c, (embi_str){e->url, e->nurl})));
    pset(c, p, "storageArea", area);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_StorageEvent, "storage", EMBI_EVF_TRUSTED, p);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else if (c->gbo != nullptr) {
        embi_impl_fire(c, c->gbo, "storage", ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    sevent_free(e);
}

typedef struct bcast {
    const embi_storage_area *src;
    embi_str url;
    const embi_u16view *key, *old_v, *new_v;
} bcast;

static void peer_post(embi_bind_ctx *peer, embi_js_realm *realm, const embi_loader_env *env, void *arg)
{
    const bcast *b = arg;
    sevent *e = embi_alloc(sizeof *e);
    *e = (sevent){0};
    e->c = peer;
    e->realm = realm;
    e->src = b->src;
    if (b->url.len > 0) {
        e->nurl = b->url.len;
        e->url = embi_alloc(e->nurl);
        memcpy(e->url, b->url.ptr, e->nurl);
    }
    if (b->key != nullptr) {
        e->has_key = true;
        e->nkey = b->key->len;
        e->key = dup16(b->key);
    }
    if (b->old_v != nullptr) {
        e->has_old = true;
        e->nold = b->old_v->len;
        e->old_v = dup16(b->old_v);
    }
    if (b->new_v != nullptr) {
        e->has_new = true;
        e->nnew = b->new_v->len;
        e->new_v = dup16(b->new_v);
    }
    if (!env->post(env->user, sevent_run, sevent_drop, e)) {
        sevent_free(e);
    }
}

void ldr_storage_changed(embi_bind_ctx *c, embi_storage_area *area, const embi_u16view *key, const embi_u16view *old_v,
                         const embi_u16view *new_v)
{
    /* The writer's URL is read here, on the writer's own thread. */
    bcast b = {area, embi_bind_url(c), key, old_v, new_v};
    ldr_for_peers(c, peer_post, &b);
}
