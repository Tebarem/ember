/*
 * The per-context state of the loader's web APIs (W7c): the embedder's environment, the fallbacks for a context without
 * one, the fetches in flight, and the small JS helpers the impl_loader_*.c files share.
 *
 * The binding context (bind.h, which this task may not edit) has no slot for loader state, so the states live in a
 * process-wide list keyed by context (and its realm, so a context allocated at a freed one's address is not mistaken for
 * it), with a per-thread cache of the last lookup. embi_loader_detach removes the state; the embedding layer calls it
 * before embi_bind_free. A context freed without it leaks its state (a slot in embi_bind_ctx with a finalizer called from
 * embi_bind_free would remove that: a small bind.h change for a later task).
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"

#include <stdatomic.h>

#include <SDL3/SDL_atomic.h>

struct ldr_state {
    ldr_state *next;
    embi_bind_ctx *c;
    embi_js_realm *realm;
    embi_loader_env env;
    embi_blob_urls own_blobs;
    embi_storage *own_storage;
    embi_storage_area *session;
    ldr_job *jobs;
    uint32_t next_job;
    bool prepared;
    uint32_t prelude;    /* persistent handle of the streams prelude exports, plus one */
    uint32_t xhr;        /* persistent handle of the XMLHttpRequest source's exports, plus one (impl_xhr.c) */
    uint32_t xhr_host;   /* persistent handle of the globals captured for that source, plus one (impl_xhr.c) */
    embi_mports *ports;  /* MessagePort registry (impl_msgport.c) */
    bool ports_shared;   /* IF1: ports is the registry of the view's frames (a reference of this context) */
    void *net_reg;       /* WebSocket and EventSource registry (impl_websocket.c) */
};

static SDL_SpinLock g_lock;
static ldr_state *g_states;
/* Bumped whenever a state is freed. A thread's last lookup stays valid while the count has not moved, so the common case
 * (many operations on one context) skips the lock and the list walk. */
static atomic_uint g_free_gen;
static thread_local struct {
    ldr_state *s;
    unsigned gen;
} g_last;

static thread_local embi_rng g_rng;

embi_rng *ldr_rng(void)
{
    return &g_rng;
}

static ldr_state *find_locked(embi_bind_ctx *c)
{
    for (ldr_state *s = g_states; s != nullptr; s = s->next) {
        if (s->c == c && s->realm == c->realm) {
            return s;
        }
    }
    return nullptr;
}

ldr_state *ldr_state_of(embi_bind_ctx *c)
{
    /* The generation is compared first: after a free the cached pointer must not be read. */
    if (g_last.s != nullptr && g_last.gen == atomic_load_explicit(&g_free_gen, memory_order_acquire) &&
        g_last.s->c == c && g_last.s->realm == c->realm) {
        return g_last.s;
    }
    SDL_LockSpinlock(&g_lock);
    ldr_state *s = find_locked(c);
    if (s == nullptr) {
        s = embi_alloc(sizeof *s);
        *s = (ldr_state){0};
        s->c = c;
        s->realm = c->realm;
        s->next = g_states;
        g_states = s;
    }
    unsigned gen = atomic_load_explicit(&g_free_gen, memory_order_acquire);
    SDL_UnlockSpinlock(&g_lock);
    g_last.s = s;
    g_last.gen = gen;
    if (!s->prepared) {
        s->prepared = true;
        ldr_prepare(c);
    }
    return s;
}

uint32_t *ldr_xhr_slot(embi_bind_ctx *c)
{
    return &ldr_state_of(c)->xhr;
}

uint32_t *ldr_xhr_host_slot(embi_bind_ctx *c)
{
    return &ldr_state_of(c)->xhr_host;
}

uint32_t *ldr_prelude_slot(embi_bind_ctx *c)
{
    return &ldr_state_of(c)->prelude;
}

void embi_loader_share_ports(embi_bind_ctx *c, embi_mports *shared)
{
    ldr_state *s = ldr_state_of(c);
    if (s->ports != nullptr || shared == nullptr) {
        return; /* a context that made ports already keeps its own registry */
    }
    embi_mports_retain(shared);
    s->ports = shared;
    s->ports_shared = true;
}

embi_mports *ldr_ports(embi_bind_ctx *c)
{
    ldr_state *s = ldr_state_of(c);
    if (s->ports == nullptr) {
        s->ports = embi_mports_new();
    }
    return s->ports;
}

void **ldr_net_registry(embi_bind_ctx *c)
{
    return &ldr_state_of(c)->net_reg;
}

const embi_loader_env *ldr_env(embi_bind_ctx *c)
{
    return &ldr_state_of(c)->env;
}

embi_blob_urls *ldr_blobs(embi_bind_ctx *c)
{
    ldr_state *s = ldr_state_of(c);
    return s->env.blobs != nullptr ? s->env.blobs : &s->own_blobs;
}

embi_storage *ldr_storage(embi_bind_ctx *c)
{
    ldr_state *s = ldr_state_of(c);
    if (s->env.storage != nullptr) {
        return s->env.storage;
    }
    if (s->own_storage == nullptr) {
        s->own_storage = embi_storage_open(nullptr, nullptr);
    }
    return s->own_storage;
}

embi_storage_area *ldr_session(embi_bind_ctx *c)
{
    ldr_state *s = ldr_state_of(c);
    if (s->session == nullptr) {
        s->session = embi_storage_session_new();
    }
    return s->session;
}

void ldr_for_peers(embi_bind_ctx *c,
                   void (*fn)(embi_bind_ctx *peer, embi_js_realm *realm, const embi_loader_env *env, void *arg),
                   void *arg)
{
    /* The peers are collected under the lock and called after it is released: fn posts to other threads and
     * allocates, which a spinlock must not cover. What a peer's post hook dereferences is retained under the lock and
     * unretained after fn, so a peer view that detaches meanwhile cannot free it. The posted tasks check ldr_ctx_live
     * before touching a peer. The list is sized first (counted under the lock), allocated outside it, then filled; if
     * peers were added meanwhile the pass retries. */
    typedef struct peer {
        embi_bind_ctx *c;
        embi_js_realm *realm;
        embi_loader_env env;
    } peer;
    peer stackbuf[8];
    peer *list = stackbuf;
    uint32_t n = 0, cap = 8;
    for (;;) {
        n = 0;
        uint32_t want = 0;
        bool full = false;
        SDL_LockSpinlock(&g_lock);
        ldr_state *me = find_locked(c);
        if (me != nullptr && me->env.storage != nullptr && me->env.post != nullptr) {
            for (ldr_state *s = g_states; s != nullptr; s = s->next) {
                if (s == me || s->env.storage != me->env.storage || s->env.post == nullptr) {
                    continue;
                }
                want++;
                if (n < cap) {
                    list[n++] = (peer){s->c, s->realm, s->env};
                    if (s->env.retain != nullptr) {
                        s->env.retain(s->env.user);
                    }
                } else {
                    full = true;
                }
            }
        }
        SDL_UnlockSpinlock(&g_lock);
        if (!full) {
            break;
        }
        /* Too many peers for the buffer: undo, grow outside the lock, retry. */
        for (uint32_t i = 0; i < n; i++) {
            if (list[i].env.unretain != nullptr) {
                list[i].env.unretain(list[i].env.user);
            }
        }
        if (list != stackbuf) {
            SDL_free(list);
        }
        cap = want + 4;
        list = SDL_malloc((size_t)cap * sizeof *list);
        if (list == nullptr) {
            return;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        fn(list[i].c, list[i].realm, &list[i].env, arg);
        if (list[i].env.unretain != nullptr) {
            list[i].env.unretain(list[i].env.user);
        }
    }
    if (list != stackbuf) {
        SDL_free(list);
    }
}

bool ldr_ctx_live(embi_bind_ctx *c, embi_js_realm *realm)
{
    SDL_LockSpinlock(&g_lock);
    ldr_state *s = find_locked(c);
    bool ok = s != nullptr && s->realm == realm;
    SDL_UnlockSpinlock(&g_lock);
    return ok;
}

ldr_job **ldr_jobs_head(ldr_state *st)
{
    return &st->jobs;
}

uint32_t ldr_next_job_id(ldr_state *st)
{
    return ++st->next_job;
}

void embi_loader_attach(embi_bind_ctx *c, const embi_loader_env *env)
{
    ldr_state *s = ldr_state_of(c);
    if (s->jobs != nullptr) {
        ldr_jobs_abort_all(s);
    }
    /* Swapped under the lock: ldr_for_peers on another thread copies the environment. */
    embi_loader_env old = s->env;
    SDL_LockSpinlock(&g_lock);
    s->env = env != nullptr ? *env : (embi_loader_env){0};
    SDL_UnlockSpinlock(&g_lock);
    if (old.release != nullptr) {
        old.release(old.user);
    }
}

void embi_loader_detach(embi_bind_ctx *c)
{
    SDL_LockSpinlock(&g_lock);
    ldr_state *s = find_locked(c);
    if (s != nullptr) {
        ldr_state **pp = &g_states;
        while (*pp != s) {
            pp = &(*pp)->next;
        }
        *pp = s->next;
        atomic_fetch_add_explicit(&g_free_gen, 1, memory_order_acq_rel);
    }
    SDL_UnlockSpinlock(&g_lock);
    if (s == nullptr) {
        return;
    }
    ldr_jobs_abort_all(s);
    if (s->ports_shared) {
        ldr_msg_release_owned(c, s->ports);
    } else {
        ldr_msg_release(c, s->ports);
    }
    embi_impl_net_release(c, s->net_reg);
    ldr_prelude_release(c, s->prelude);
    ldr_xhr_release(c, s->xhr);
    ldr_xhr_release(c, s->xhr_host);
    ldr_storage_release(s);
    if (s->env.release != nullptr) {
        s->env.release(s->env.user);
    }
    if (s->session != nullptr) {
        embi_storage_session_free(s->session);
    }
    if (s->own_storage != nullptr) {
        embi_storage_close(s->own_storage);
    }
    embi_blob_urls_free(&s->own_blobs);
    embi_free(s, sizeof *s);
}

/* ------------------------------------------------------------------ JS helpers */

embi_js_value ldr_new_uint8(embi_bind_ctx *c, const uint8_t *p, size_t n)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value ctor = embi_js_binary_intrinsic(rt, (enum embi_js_bin_intr)(EMBI_JS_BI_TA_CTOR_BASE + EMBI_JS_TA_UINT8));
    embi_js_value len = embi_js_from_double((double)n);
    embi_js_value arr = embi_js_construct(rt, ctor, 1, &len, ctor);
    if (arr == EMBI_JS_EXCEPTION || n == 0 || p == nullptr) {
        return arr;
    }
    embi_js_varray *v = embi_js_view_of(embi_js_as_object(arr));
    memcpy(embi_js_view_base(v), p, n);
    return arr;
}

embi_js_value ldr_new_arraybuffer(embi_bind_ctx *c, const uint8_t *p, size_t n)
{
    embi_js_runtime *rt = c->rt;
    embi_js_object *o =
        embi_js_arraybuffer_alloc(rt, embi_js_binary_intrinsic(rt, EMBI_JS_BI_ARRAYBUFFER_PROTO), n, n, 0);
    if (o == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    if (n > 0) {
        memcpy(embi_js_abuf_of(o)->data, p, n);
    }
    return embi_js_from_object(o);
}

bool ldr_buffer_source(embi_js_value v, const uint8_t **p, size_t *n, bool *detached)
{
    *p = nullptr;
    *n = 0;
    if (detached != nullptr) {
        *detached = false;
    }
    if (!embi_js_is_object(v)) {
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    if (embi_js_is_buffer_obj(o)) {
        embi_js_abuf *b = embi_js_abuf_of(o);
        if ((b->flags & EMBI_JS_ABUF_DETACHED) != 0) {
            if (detached != nullptr) {
                *detached = true;
            }
        } else if (b->len > 0) {
            *p = b->data;
            *n = b->len;
        }
        return true;
    }
    if (embi_js_is_typedarray_obj(o) || embi_js_is_dataview_obj(o)) {
        embi_js_varray *vw = embi_js_view_of(o);
        const embi_js_abuf *b = embi_js_view_abuf(vw);
        size_t esz = 1;
        if (embi_js_is_typedarray_obj(o)) {
            esz = embi_js_bin_size((enum embi_js_ta_kind)embi_js_bin_num(vw->elems[EMBI_JS_VIEW_KIND]));
        }
        if (b != nullptr && (b->flags & EMBI_JS_ABUF_DETACHED) != 0) {
            if (detached != nullptr) {
                *detached = true;
            }
            return true;
        }
        size_t count = 0;
        if (embi_js_view_len(vw, esz, &count) && count > 0) {
            *n = count * esz;
            *p = embi_js_view_base(vw);
        }
        return true;
    }
    return false;
}

embi_js_value ldr_iter_result(embi_bind_ctx *c, embi_js_value value, bool done)
{
    embi_js_value o = embi_bind_object_new(c);
    embi_bind_object_set_c(c, o, "value", value);
    embi_bind_object_set_c(c, o, "done", embi_js_from_bool(done));
    return o;
}

embi_js_value ldr_native(embi_bind_ctx *c, const char *name, int length, embi_js_native fn, int magic)
{
    embi_js_object *f = embi_js_native_function_new(c->realm, name, length, fn, magic);
    return f != nullptr ? embi_js_from_object(f) : EMBI_JS_EXCEPTION;
}

embi_js_value ldr_promise_reject(embi_bind_ctx *c, embi_js_value reason)
{
    embi_js_object *p = embi_js_promise_new(c->rt);
    embi_js_value pv = embi_js_from_object(p);
    (void)embi_js_promise_reject_with(c->rt, p, reason);
    return pv;
}

embi_js_value ldr_promise_resolve(embi_bind_ctx *c, embi_js_value v)
{
    embi_js_object *p = embi_js_promise_new(c->rt);
    embi_js_value pv = embi_js_from_object(p);
    (void)embi_js_promise_resolve_with(c->rt, p, v);
    return pv;
}

embi_js_value ldr_promise_reject_pending(embi_bind_ctx *c)
{
    embi_js_value ex = embi_js_exception_get(c->rt);
    embi_js_exception_clear(c->rt);
    return ldr_promise_reject(c, ex);
}

embi_js_value ldr_make_type_error(embi_bind_ctx *c, const char *msg)
{
    (void)embi_js_throw_type_error(c->rt, "%s", msg);
    embi_js_value ex = embi_js_exception_get(c->rt);
    embi_js_exception_clear(c->rt);
    return ex;
}

embi_js_value ldr_promise_type_error(embi_bind_ctx *c, const char *msg)
{
    return ldr_promise_reject(c, ldr_make_type_error(c, msg));
}

bool ldr_to_utf8(embi_bind_ctx *c, embi_js_value v, embi_sb *out)
{
    embi_js_string *s = embi_js_to_string(c->rt, v);
    if (s == nullptr) {
        return false;
    }
    embi_js_str_append_utf8(s, out);
    return true;
}

embi_js_value ldr_get(embi_bind_ctx *c, embi_js_value obj, const char *name)
{
    if (!embi_js_is_object(obj)) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_js_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, name));
}

embi_js_key ldr_wk_key(embi_bind_ctx *c, enum embi_js_wk which)
{
    return embi_js_to_property_key(c->rt, embi_js_wk_sym(c->rt, which));
}

/* ------------------------------------------------------------------ one-time preparation */

/* The method of the async iterator object that returns the iterator itself (@@asyncIterator). */
static embi_js_value iter_self(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

/* Defines the symbol-keyed members idlgen cannot: ReadableStream.prototype[Symbol.asyncIterator] (the same function as
 * values) and the self-returning @@asyncIterator of the iterator objects. */
void ldr_prepare(embi_bind_ctx *c)
{
    ldr_clone_install();
    embi_js_runtime *rt = c->rt;
    embi_js_key sym = ldr_wk_key(c, EMBI_JS_WK_ASYNC_ITERATOR);
    if (sym == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        return;
    }
    embi_js_value proto = c->protos[EMBI_IF_ReadableStream];
    if (embi_js_is_object(proto)) {
        embi_js_value vals = embi_js_get(rt, embi_js_as_object(proto), embi_js_key_c(rt, "values"));
        if (vals != EMBI_JS_EXCEPTION) {
            embi_js_define_raw(rt, embi_js_as_object(proto), sym, vals, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
        } else {
            embi_js_exception_clear(rt);
        }
    }
    embi_js_value iproto = c->protos[EMBI_IF_ReadableStreamAsyncIterator];
    if (embi_js_is_object(iproto)) {
        embi_js_value self = ldr_native(c, "[Symbol.asyncIterator]", 0, iter_self, 0);
        if (self != EMBI_JS_EXCEPTION) {
            embi_js_define_raw(rt, embi_js_as_object(iproto), sym, self, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
        }
    }
}
