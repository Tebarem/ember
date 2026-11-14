/*
 * ES modules (J3c): the module registry, the graph loader (through the host's load hook), linking
 * (ResolveExport with cycles and ambiguous star exports, live import bindings that share the
 * exporter's boxes), evaluation with the async module algorithm (top-level await, cycle roots,
 * pending async dependencies), module namespace objects, import.meta and dynamic import.
 *
 * The engine never reads files. The host gets each request through the load hook and completes it
 * with embi_js_module_loaded (or embi_js_module_load_failed), synchronously or later. Strings the
 * host receives through the hook are valid only during the call; the host must copy what it keeps.
 * See ECMA-262 section 16.2.
 *
 * A module whose dependency failed to load keeps that failure for the runtime's lifetime (a later
 * import of it fails the same way). A job finishes when its own requests are complete and every
 * module reachable from its root is loaded, so it waits for loads that other jobs have in flight.
 */
#include "js/module.h"

#include "js/interp.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/promise.h"
#include "js/string.h"

#include <string.h>

typedef struct mpair {
    embi_js_value holder;
    embi_js_value name;
} mpair;
typedef EMBI_VEC(embi_js_value) value_vec;
typedef EMBI_VEC(mpair) pair_vec;

/* ------------------------------------------------------------------ registry state */

/* A host request in flight (it is removed when it completes). parent is the requesting module's
 * holder (EMPTY for an entry or an import()). type is EMPTY or the type atom. job is the id of the
 * job that issued it: a job cannot finish while one of its requests is in flight. */
typedef struct mreq_entry {
    uint32_t id;
    uint32_t job;
    embi_js_value parent;
    uint32_t index;
    embi_js_value type;
} mreq_entry;

/* A load job: one entry request (embi_js_module_eval or import()) and the graph it loads. The job
 * finishes (links, evaluates and settles its promise) and is freed once its own requests have
 * completed and every module reachable from its root is loaded, including modules that another job
 * is still loading. The guard holds the job open while its first request is issued, so that
 * synchronous completions cannot finish it early. */
typedef struct mjob {
    uint32_t id;
    embi_js_value cap;   /* the promise the caller got */
    embi_js_value root;  /* the entry module's holder, once loaded */
    embi_js_value error; /* the first failure */
    embi_js_realm *realm;
    uint32_t pending;    /* requests issued and not completed */
    uint64_t walked_at;  /* mstate.completions at the last graph walk that found a load in flight */
    bool walked;
    bool guard;
    bool failed;
    bool dynamic;        /* import(): the promise resolves with the namespace after evaluation */
    bool deferred;       /* import(): finishing waits for a job (the import never evaluates inline) */
} mjob;

typedef struct mstate {
    EMBI_VEC(embi_js_value) mods; /* every module holder (rooted), kept for the runtime's lifetime */
    embi_hashmap map;             /* hash of (key, type) -> 1 + index in mods of the chain's head */
    uint32_t epoch;               /* last visit mark handed out to a graph walk */
    uint64_t completions;         /* host requests completed so far */
    EMBI_VEC(mjob *) jobs;        /* jobs that have not finished */
    EMBI_VEC(mreq_entry) reqs;    /* requests in flight */
    uint32_t next_id;
    uint32_t next_job;
    uint64_t async_counter;
    embi_js_module_load_fn load;
    void *load_user;
    embi_js_import_meta_fn meta_fn;
    void *meta_user;
} mstate;

static void state_mark(embi_js_tracer *t, void *user);

static mstate *state_of(embi_js_runtime *rt)
{
    if (rt->modules) {
        return rt->modules;
    }
    mstate *s = embi_calloc(1, sizeof *s);
    rt->modules = s;
    embi_js_root_add(rt, state_mark, rt);
    return s;
}

static embi_js_module *rec_of(embi_js_value holder)
{
    return (embi_js_module *)((embi_js_object *)embi_js_as_cell(holder))->internal;
}

embi_js_module *embi_jsm_of(embi_js_value holder)
{
    return rec_of(holder);
}

static void module_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_js_module *m = o->internal;
    if (!m) {
        return;
    }
    embi_js_mark_value(t, m->holder);
    embi_js_mark_value(t, m->code);
    embi_js_mark_value(t, m->gen);
    embi_js_mark_value(t, m->key);
    embi_js_mark_value(t, m->type);
    embi_js_mark_value(t, m->meta);
    embi_js_mark_value(t, m->ns);
    embi_js_mark_value(t, m->top_cap);
    embi_js_mark_value(t, m->eval_error);
    embi_js_mark_value(t, m->cycle_root);
    embi_js_mark_value(t, m->load_error);
    if (m->imports) {
        for (uint32_t i = 0; i < m->mod.imps.len; i++) {
            embi_js_mark_value(t, m->imports[i]);
        }
    }
    if (m->reqs) {
        for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
            embi_js_mark_value(t, m->reqs[i]);
        }
    }
    if (m->locals) {
        for (uint32_t i = 0; i < m->mod.exps.len; i++) {
            embi_js_mark_value(t, m->locals[i]);
        }
    }
    for (uint32_t i = 0; i < m->async_parents.len; i++) {
        embi_js_mark_value(t, m->async_parents.data[i]);
    }
}

static const embi_js_class class_module = {
    .name = "Module",
    .trace = module_trace,
};

static void state_mark(embi_js_tracer *t, void *user)
{
    const embi_js_runtime *rt = user;
    const mstate *s = rt->modules;
    if (!s) {
        return;
    }
    for (uint32_t i = 0; i < s->mods.len; i++) {
        embi_js_mark_value(t, s->mods.data[i]);
    }
    for (uint32_t i = 0; i < s->jobs.len; i++) {
        const mjob *j = s->jobs.data[i];
        embi_js_mark_value(t, j->cap);
        embi_js_mark_value(t, j->root);
        embi_js_mark_value(t, j->error);
    }
    for (uint32_t i = 0; i < s->reqs.len; i++) {
        embi_js_mark_value(t, s->reqs.data[i].parent);
        embi_js_mark_value(t, s->reqs.data[i].type);
    }
}

/* ------------------------------------------------------------------ helpers */

static embi_js_value take_exception(embi_js_runtime *rt)
{
    embi_js_value e = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    return e;
}

/* UTF-8 copy of a string value: nul-terminated, owned by the caller (free with embi_free(p, n+1)). */
static char *string_utf8(const embi_js_value v, size_t *len)
{
    embi_sb sb;
    memset(&sb, 0, sizeof sb);
    embi_js_str_append_utf8((const embi_js_string *)embi_js_as_cell(v), &sb);
    size_t n = strlen(embi_sb_cstr(&sb));
    char *out = embi_alloc(n + 1u);
    memcpy(out, embi_sb_cstr(&sb), n + 1u);
    embi_sb_free(&sb);
    *len = n;
    return out;
}

static char *cstr_copy(const char *s, size_t n)
{
    char *out = embi_alloc(n + 1u);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static embi_js_value atom_value(embi_js_runtime *rt, const char *s, size_t n)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, n));
}

static void promise_resolve_with(embi_js_runtime *rt, embi_js_value cap, embi_js_value v)
{
    (void)embi_js_promise_resolve_with(rt, embi_js_as_object(cap), v);
}

static void promise_reject_with(embi_js_runtime *rt, embi_js_value cap, embi_js_value e)
{
    (void)embi_js_promise_reject_with(rt, embi_js_as_object(cap), e);
}

static bool contains_value(const value_vec *v, embi_js_value x)
{
    for (uint32_t i = 0; i < v->len; i++) {
        if (v->data[i] == x) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ module records */

/* A new holder object with an empty record (kept in the registry for the runtime's lifetime). */
static embi_js_value holder_new(embi_js_runtime *rt, embi_js_realm *r)
{
    embi_js_object *ho = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_module);
    embi_js_module *m = embi_calloc(1, sizeof *m);
    ho->internal = m;
    EMBI_JS_WB_STRUCT(ho);
    m->holder = embi_js_from_object(ho);
    m->realm = r;
    m->code = EMBI_JS_EMPTY;
    m->gen = EMBI_JS_EMPTY;
    m->key = EMBI_JS_EMPTY;
    m->type = EMBI_JS_EMPTY;
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->meta, EMBI_JS_EMPTY);
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->ns, EMBI_JS_EMPTY);
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->top_cap, EMBI_JS_EMPTY);
    m->eval_error = EMBI_JS_EMPTY;
    m->cycle_root = EMBI_JS_EMPTY;
    m->load_error = EMBI_JS_EMPTY;
    return m->holder;
}

static void module_free(embi_js_module *m)
{
    if (!m) {
        return;
    }
    if (m->imports) {
        embi_free(m->imports, m->mod.imps.len * sizeof(embi_js_value));
    }
    if (m->reqs) {
        embi_free(m->reqs, m->mod.reqs.len * sizeof(embi_js_value));
    }
    if (m->locals) {
        embi_free(m->locals, m->mod.exps.len * sizeof(embi_js_value));
    }
    embi_free(m->key_c, m->key_len + 1u);
    embi_vec_free(&m->async_parents);
    embi_vec_free(&m->mod.reqs);
    embi_vec_free(&m->mod.imps);
    embi_vec_free(&m->mod.exps);
    embi_free(m, sizeof *m);
}

static uint64_t key_hash(const char *key, size_t klen, embi_js_value type)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < klen; i++) {
        h = (h ^ (uint8_t)key[i]) * 1099511628211ull;
    }
    return (h ^ (uint64_t)type) * 1099511628211ull;
}

static embi_js_value find_module(embi_js_runtime *rt, const char *key, size_t klen,
                                 embi_js_value type)
{
    const mstate *s = state_of(rt);
    uint64_t idx;
    if (!embi_hashmap_get(&s->map, key_hash(key, klen, type), &idx)) {
        return EMBI_JS_EMPTY;
    }
    while (idx != 0) {
        embi_js_value hv = s->mods.data[idx - 1u];
        const embi_js_module *m = rec_of(hv);
        if (m->key_len == klen && memcmp(m->key_c, key, klen) == 0 && m->type == type) {
            return hv;
        }
        idx = m->hnext;
    }
    return EMBI_JS_EMPTY;
}

/* Adds a fully built record to the registry (the module map). */
static void register_module(mstate *s, embi_js_value hv)
{
    embi_js_module *m = rec_of(hv);
    embi_vec_push(&s->mods, hv);
    uint64_t h = key_hash(m->key_c, m->key_len, m->type);
    uint64_t head = 0;
    (void)embi_hashmap_get(&s->map, h, &head);
    m->hnext = (uint32_t)head;
    (void)embi_hashmap_put(&s->map, h, s->mods.len);
}

/* Parses and compiles a source text module under key. Returns its holder, or EMBI_JS_EXCEPTION
 * with the SyntaxError pending (the record is then dropped). The record joins the registry only once
 * it is complete, so a failed compile leaves nothing behind. */
static embi_js_value module_new(embi_js_runtime *rt, embi_js_realm *r, const char *key, size_t klen,
                                embi_js_value type, const char *src, size_t slen)
{
    mstate *s = state_of(rt);
    embi_js_value hv = holder_new(rt, r);
    embi_js_module *m = rec_of(hv);
    m->key_c = cstr_copy(key, klen);
    m->key_len = (uint32_t)klen;
    m->key = atom_value(rt, key, klen);
    m->type = type;
    const size_t gc_mark = embi_js_gc_alloc_mark(rt);
    embi_js_value code = embi_jsc_compile_module(r, (const uint8_t *)src, (uint32_t)slen, m->key_c,
                                                 hv, &m->mod);
    if (code != EMBI_JS_EXCEPTION) {
        /* A module's code lives as long as the runtime (the registry keeps every module): it is not garbage in the making. */
        embi_js_gc_forgive_since(rt, gc_mark);
    }
    if (code == EMBI_JS_EXCEPTION) {
        embi_js_as_object(hv)->internal = nullptr;
        module_free(m);
        return EMBI_JS_EXCEPTION;
    }
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->code, code);
    m->has_tla = m->mod.has_tla;
    m->imports = m->mod.imps.len > 0 ? embi_calloc(m->mod.imps.len, sizeof(embi_js_value)) : nullptr;
    m->reqs = m->mod.reqs.len > 0 ? embi_calloc(m->mod.reqs.len, sizeof(embi_js_value)) : nullptr;
    m->locals = m->mod.exps.len > 0 ? embi_calloc(m->mod.exps.len, sizeof(embi_js_value)) : nullptr;
    for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
        m->reqs[i] = EMBI_JS_EMPTY;
    }
    register_module(s, hv);
    return hv;
}


/* A JSON module (a synthetic record, ECMA-262 JSON module records). Its source is JSON text, parsed
 * with the intrinsic JSON parser (not the realm's, which user code may have replaced); its only
 * export is "default", bound to the parsed value. */
static bool is_json_type(embi_js_runtime *rt, embi_js_value type)
{
    return type != EMBI_JS_EMPTY && type == atom_value(rt, "json", 4);
}

/* A JSON module record. A parse error is a SyntaxError (nothing is registered). */
static embi_js_value json_module_new(embi_js_runtime *rt, embi_js_realm *r, const char *key, size_t klen,
                                     embi_js_value type, const char *src, size_t slen)
{
    mstate *s = state_of(rt);
    embi_js_string *ts = embi_js_str_new_utf8(rt, src, slen);
    if (ts == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value v = embi_js_json_parse_text(rt, ts);
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_box *b = embi_js_box_new(rt, v);
    embi_js_value hv = holder_new(rt, r);
    embi_js_module *m = rec_of(hv);
    m->key_c = cstr_copy(key, klen);
    m->key_len = (uint32_t)klen;
    m->key = atom_value(rt, key, klen);
    m->type = type;
    m->synthetic = true;
    m->locals = embi_calloc(1, sizeof(embi_js_value));
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->locals[0], embi_js_from_cell(b));
    cmexp e;
    memset(&e, 0, sizeof e);
    e.kind = CMEX_LOCAL;
    e.name = atom_value(rt, "default", 7);
    e.local = e.name;
    e.slot = -1;
    embi_vec_push(&m->mod.exps, e);
    m->status = EMBJ_UNLINKED;
    register_module(s, hv);
    return hv;
}

/* ------------------------------------------------------------------ host requests and jobs */

static void check_jobs(embi_js_runtime *rt);
static void finish(embi_js_runtime *rt, mjob *job);
static bool link(embi_js_runtime *rt, embi_js_value root);

static mjob *job_find(const mstate *s, uint32_t id)
{
    for (uint32_t i = 0; i < s->jobs.len; i++) {
        if (s->jobs.data[i]->id == id) {
            return s->jobs.data[i];
        }
    }
    return nullptr;
}

static uint32_t job_new(embi_js_runtime *rt, embi_js_realm *r, embi_js_value cap, bool dynamic)
{
    mstate *s = state_of(rt);
    mjob *j = embi_calloc(1, sizeof *j);
    j->id = ++s->next_job;
    j->realm = r;
    j->cap = cap;
    j->root = EMBI_JS_EMPTY;
    j->error = EMBI_JS_UNDEFINED;
    j->guard = true;
    j->dynamic = dynamic;
    embi_vec_push(&s->jobs, j);
    return j->id;
}

/* Takes the in-flight request with this id out of the list. False when there is none. */
static bool req_take(mstate *s, uint32_t id, mreq_entry *out)
{
    for (uint32_t i = 0; i < s->reqs.len; i++) {
        if (s->reqs.data[i].id == id) {
            *out = s->reqs.data[i];
            s->reqs.data[i] = s->reqs.data[--s->reqs.len];
            return true;
        }
    }
    return false;
}

/* A request of job_id failed with err. parent is the requesting module (EMPTY for an entry or an
 * import()). The module keeps the failure: its slot holds UNDEFINED and load_error the first error,
 * so every job that reaches it fails with that error. */
static void request_failed(embi_js_runtime *rt, uint32_t job_id, embi_js_value parent, uint32_t index,
                           embi_js_value err)
{
    mstate *s = state_of(rt);
    if (parent != EMBI_JS_EMPTY) {
        embi_js_module *pm = rec_of(parent);
        EMBI_JS_STORE(embi_js_as_cell(pm->holder), pm->reqs[index], EMBI_JS_UNDEFINED);
        if (pm->load_error == EMBI_JS_EMPTY) {
            EMBI_JS_STORE(embi_js_as_cell(pm->holder), pm->load_error, err);
        }
    }
    mjob *job = job_find(s, job_id);
    if (job != nullptr) {
        if (parent == EMBI_JS_EMPTY && !job->failed) {
            job->failed = true;
            job->error = err;
        }
        job->pending--;
    }
}

/* Issues one host request for spec (UTF-8, valid for the call) on behalf of job_id. parent is the
 * requesting module (EMPTY for an entry or import(), whose referrer is then ref). */
static void issue(embi_js_runtime *rt, uint32_t job_id, embi_js_value parent, uint32_t index,
                  const char *spec, size_t slen, embi_js_value type, const char *ref, size_t rlen)
{
    mstate *s = state_of(rt);
    mjob *job = job_find(s, job_id);
    job->pending++;
    if (parent != EMBI_JS_EMPTY) {
        ref = rec_of(parent)->key_c;
        rlen = rec_of(parent)->key_len;
    }
    if (s->load == nullptr) {
        embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "no module loader is installed");
        request_failed(rt, job_id, parent, index, take_exception(rt));
        return;
    }
    uint32_t id = ++s->next_id;
    mreq_entry e = {id, job_id, parent, index, type};
    embi_vec_push(&s->reqs, e);
    char *tname = nullptr;
    size_t tlen = 0;
    if (type != EMBI_JS_EMPTY) {
        tname = string_utf8(type, &tlen);
    }
    s->load(rt, s->load_user, id, ref, rlen, spec, slen, tname);
    if (tname != nullptr) {
        embi_free(tname, tlen + 1u);
    }
}

/* Requests every import of a freshly created module. */
static void issue_module(embi_js_runtime *rt, uint32_t job_id, embi_js_value hv)
{
    const embi_js_module *m = rec_of(hv);
    for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
        const cmreq *q = &m->mod.reqs.data[i];
        if (q->unsupported) {
            /* HostGetSupportedImportAttributes is only "type": the request fails at link time. */
            embi_js_throw_error(rt, EMBI_JS_PROTO_SYNTAX_ERROR, "unsupported import attribute");
            mstate *st = state_of(rt);
            mjob *jb = job_find(st, job_id);
            jb->pending++;
            request_failed(rt, job_id, hv, i, take_exception(rt));
            continue;
        }
        size_t slen;
        char *spec = string_utf8(q->spec, &slen);
        issue(rt, job_id, hv, i, spec, slen, q->type, nullptr, 0);
        embi_free(spec, slen + 1u);
    }
}

/* Walks the modules reachable from the job's root. A module whose load failed fails the job (the
 * error is kept). Returns false while a reachable request is still in flight (another job's, or
 * this one's). Each module is visited once (an epoch mark on the record), so a walk is linear. */
static bool graph_settled(embi_js_runtime *rt, mjob *job)
{
    mstate *s = state_of(rt);
    uint32_t ep = ++s->epoch;
    value_vec stack = {nullptr, 0, 0};
    bool waiting = false;
    rec_of(job->root)->epoch = ep;
    embi_vec_push(&stack, job->root);
    while (stack.len > 0) {
        embi_js_value hv = stack.data[--stack.len];
        const embi_js_module *m = rec_of(hv);
        if (m->load_error != EMBI_JS_EMPTY) {
            if (!job->failed) {
                job->failed = true;
                job->error = m->load_error;
            }
            break;
        }
        for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
            if (m->reqs[i] == EMBI_JS_EMPTY) {
                waiting = true;
            } else if (m->reqs[i] != EMBI_JS_UNDEFINED) {
                embi_js_module *c = rec_of(m->reqs[i]);
                if (c->epoch != ep) {
                    c->epoch = ep;
                    embi_vec_push(&stack, m->reqs[i]);
                }
            }
        }
    }
    embi_vec_free(&stack);
    return job->failed || !waiting;
}

/* Whether a job can finish now (see mjob). A failed job waits for its own requests only. A job
 * whose graph walk found a load in flight is walked again only after another request completes. */
static bool job_ready(embi_js_runtime *rt, mjob *job)
{
    if (job->guard || job->deferred || job->pending > 0) {
        return false;
    }
    if (job->failed) {
        return true;
    }
    if (job->root == EMBI_JS_EMPTY) {
        return false;
    }
    mstate *s = state_of(rt);
    if (job->walked && job->walked_at == s->completions) {
        return false;
    }
    if (graph_settled(rt, job)) {
        return true;
    }
    job->walked = true;
    job->walked_at = s->completions;
    return false;
}

/* Finishes every job that is ready. A finished job is removed from the list and freed before it
 * runs, so the user code it runs (module bodies) may start further jobs. */
static void check_jobs(embi_js_runtime *rt)
{
    mstate *s = state_of(rt);
    bool again = true;
    while (again) {
        again = false;
        for (uint32_t i = 0; i < s->jobs.len; i++) {
            mjob *j = s->jobs.data[i];
            if (!job_ready(rt, j)) {
                continue;
            }
            mjob fin = *j;
            s->jobs.data[i] = s->jobs.data[--s->jobs.len];
            embi_free(j, sizeof *j);
            finish(rt, &fin);
            again = true;
            break;
        }
    }
}

/* The deferral of an import() job ends (a job of the queue): the job may finish now. */
static void job_undefer(embi_js_runtime *rt, uint32_t id)
{
    mjob *job = job_find(state_of(rt), id);
    if (job != nullptr) {
        job->deferred = false;
        check_jobs(rt);
    }
}

/* A host request completed with a module source (key is its key, source its text). */
void embi_js_module_loaded(embi_js_runtime *rt, uint32_t request, const char *key, size_t key_len,
                           const char *source, size_t source_len)
{
    mstate *s = state_of(rt);
    mreq_entry e;
    if (!req_take(s, request, &e)) {
        return;
    }
    s->completions++;
    mjob *job = job_find(s, e.job);
    if (job == nullptr) {
        return;
    }
    embi_js_realm *realm = job->realm;
    if (e.type != EMBI_JS_EMPTY && !is_json_type(rt, e.type)) {
        (void)embi_js_throw_type_error(rt, "unsupported module type");
        request_failed(rt, e.job, e.parent, e.index, take_exception(rt));
        check_jobs(rt);
        return;
    }
    bool fresh = false;
    embi_js_value mv = find_module(rt, key, key_len, e.type);
    if (mv == EMBI_JS_EMPTY) {
        mv = is_json_type(rt, e.type)
                 ? json_module_new(rt, realm, key, key_len, e.type, source, source_len)
                 : module_new(rt, realm, key, key_len, e.type, source, source_len);
        if (mv == EMBI_JS_EXCEPTION) {
            request_failed(rt, e.job, e.parent, e.index, take_exception(rt));
            check_jobs(rt);
            return;
        }
        fresh = true;
    }
    if (e.parent != EMBI_JS_EMPTY) {
        embi_js_module *pm = rec_of(e.parent);
        EMBI_JS_STORE(embi_js_as_cell(pm->holder), pm->reqs[e.index], mv);
    } else {
        job_find(s, e.job)->root = mv;
    }
    if (fresh) {
        issue_module(rt, e.job, mv);
    }
    job_find(s, e.job)->pending--;
    check_jobs(rt);
}

/* A host request failed (message is a nul-terminated UTF-8 string). */
void embi_js_module_load_failed(embi_js_runtime *rt, uint32_t request, const char *message)
{
    mstate *s = state_of(rt);
    mreq_entry e;
    if (!req_take(s, request, &e)) {
        return;
    }
    s->completions++;
    embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "%s",
                        message != nullptr ? message : "module load failed");
    request_failed(rt, e.job, e.parent, e.index, take_exception(rt));
    check_jobs(rt);
}


/* ------------------------------------------------------------------ resolution */

enum { RES_NULL, RES_AMBIGUOUS, RES_BINDING, RES_NAMESPACE };

typedef struct mres {
    int kind;
    embi_js_value box;    /* RES_BINDING: the binding's box */
    embi_js_value holder; /* RES_NAMESPACE: the module whose namespace it is */
} mres;

static mres mres_of(int kind)
{
    mres r = {kind, EMBI_JS_EMPTY, EMBI_JS_EMPTY};
    return r;
}

/* ResolveExport (ECMA-262 16.2.1.6.3). set holds the (module, name) pairs already visited. */
static mres resolve_export(embi_js_runtime *rt, embi_js_value holder, embi_js_value name,
                           pair_vec *set)
{
    const embi_js_module *m = rec_of(holder);
    for (uint32_t i = 0; i < set->len; i++) {
        if (set->data[i].holder == holder && set->data[i].name == name) {
            return mres_of(RES_NULL);
        }
    }
    mpair pr = {holder, name};
    embi_vec_push(set, pr);
    for (uint32_t i = 0; i < m->mod.exps.len; i++) {
        const cmexp *e = &m->mod.exps.data[i];
        if (e->kind == CMEX_LOCAL && e->name == name) {
            mres r = mres_of(RES_BINDING);
            r.box = m->locals[i];
            return r;
        }
    }
    for (uint32_t i = 0; i < m->mod.exps.len; i++) {
        const cmexp *e = &m->mod.exps.data[i];
        if (e->kind != CMEX_INDIRECT || e->name != name) {
            continue;
        }
        embi_js_value target = m->reqs[e->req];
        if (e->local == EMBI_JS_EMPTY) {
            mres r = mres_of(RES_NAMESPACE);
            r.holder = target;
            return r;
        }
        return resolve_export(rt, target, e->local, set);
    }
    if (name == atom_value(rt, "default", 7)) {
        return mres_of(RES_NULL);
    }
    mres star = mres_of(RES_NULL);
    for (uint32_t i = 0; i < m->mod.exps.len; i++) {
        const cmexp *e = &m->mod.exps.data[i];
        if (e->kind != CMEX_STAR) {
            continue;
        }
        mres r = resolve_export(rt, m->reqs[e->req], name, set);
        if (r.kind == RES_AMBIGUOUS) {
            return r;
        }
        if (r.kind == RES_NULL) {
            continue;
        }
        if (star.kind == RES_NULL) {
            star = r;
        } else if (r.kind != star.kind || r.box != star.box || r.holder != star.holder) {
            return mres_of(RES_AMBIGUOUS);
        }
    }
    return star;
}

/* GetExportedNames (ECMA-262 16.2.1.6.2), appending to out (names are atoms). */
static void exported_names(embi_js_runtime *rt, embi_js_value holder, value_vec *out, uint32_t ep)
{
    embi_js_module *m = rec_of(holder);
    if (m->epoch == ep) {
        return;
    }
    m->epoch = ep;
    for (uint32_t i = 0; i < m->mod.exps.len; i++) {
        const cmexp *e = &m->mod.exps.data[i];
        if (e->kind != CMEX_STAR && !contains_value(out, e->name)) {
            embi_vec_push(out, e->name);
        }
    }
    embi_js_value dflt = atom_value(rt, "default", 7);
    for (uint32_t i = 0; i < m->mod.exps.len; i++) {
        const cmexp *e = &m->mod.exps.data[i];
        if (e->kind != CMEX_STAR) {
            continue;
        }
        value_vec star = {nullptr, 0, 0};
        exported_names(rt, m->reqs[e->req], &star, ep);
        for (uint32_t j = 0; j < star.len; j++) {
            if (star.data[j] != dflt && !contains_value(out, star.data[j])) {
                embi_vec_push(out, star.data[j]);
            }
        }
        embi_vec_free(&star);
    }
}

/* ------------------------------------------------------------------ namespace objects */

/* A namespace object's internal is a varray: [holder, name0, box0, name1, box1, ...] with the
 * names sorted by code units. */
static embi_js_varray *ns_entries(const embi_js_object *o)
{
    return (embi_js_varray *)o->internal;
}

static uint32_t ns_count(const embi_js_varray *a)
{
    return (a->hdr.aux - 1u) / 2u;
}

/* The index of the export named by key k (a string key; an index key through its string form), or
 * -1. The name table is sorted by code units, so this is a binary search. */
static int64_t ns_find(embi_js_runtime *rt, const embi_js_varray *a, embi_js_key k)
{
    if (embi_js_is_symbol(k)) {
        return -1;
    }
    embi_js_value kv = embi_js_key_to_value(rt, k);
    if (!embi_js_is_string(kv)) {
        return -1;
    }
    const embi_js_string *ks = (const embi_js_string *)embi_js_as_cell(kv);
    int64_t lo = 0;
    int64_t hi = (int64_t)ns_count(a) - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        const embi_js_string *ns = (const embi_js_string *)embi_js_as_cell(a->elems[1 + 2 * mid]);
        int c = embi_js_str_compare(ks, ns);
        if (c == 0) {
            return mid;
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

static bool ns_is_tag(embi_js_runtime *rt, embi_js_key k)
{
    return embi_js_is_symbol(k) && k == embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG);
}

static int ns_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    if (ns_is_tag(rt, k)) {
        out->value = atom_value(rt, "Module", 6);
        out->get = EMBI_JS_UNDEFINED;
        out->set = EMBI_JS_UNDEFINED;
        out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                   EMBI_JS_PD_CONFIGURABLE;
        out->attrs = 0;
        return EMBI_JS_YES;
    }
    const embi_js_varray *a = ns_entries(o);
    int64_t i = ns_find(rt, a, k);
    if (i < 0) {
        return EMBI_JS_NO;
    }
    embi_js_value v = ((embi_js_box *)embi_js_as_cell(a->elems[2 + 2 * i]))->value;
    if (embi_js_is_empty(v)) {
        embi_js_throw_reference_error(rt, "Cannot access an export before initialization");
        return EMBI_JS_THREW;
    }
    out->value = v;
    out->get = EMBI_JS_UNDEFINED;
    out->set = EMBI_JS_UNDEFINED;
    out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
               EMBI_JS_PD_CONFIGURABLE;
    out->attrs = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE;
    return EMBI_JS_YES;
}

static int ns_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                         const embi_js_pdesc *d)
{
    if (ns_is_tag(rt, k)) {
        /* @@toStringTag is {value: "Module", writable: false, enumerable: false, configurable:
         * false}: OrdinaryDefineOwnProperty accepts a descriptor that changes none of it. */
        if (embi_js_pd_is_accessor(d)) {
            return EMBI_JS_NO;
        }
        if (((d->has & EMBI_JS_PD_CONFIGURABLE) && (d->attrs & EMBI_JS_ATTR_CONFIGURABLE)) ||
            ((d->has & EMBI_JS_PD_ENUMERABLE) && (d->attrs & EMBI_JS_ATTR_ENUMERABLE)) ||
            ((d->has & EMBI_JS_PD_WRITABLE) && (d->attrs & EMBI_JS_ATTR_WRITABLE))) {
            return EMBI_JS_NO;
        }
        if ((d->has & EMBI_JS_PD_VALUE) &&
            !embi_js_same_value(d->value, atom_value(rt, "Module", 6))) {
            return EMBI_JS_NO;
        }
        return EMBI_JS_YES;
    }
    embi_js_pdesc cur;
    int g = ns_get_own(rt, o, k, &cur);
    if (g != EMBI_JS_YES) {
        return g; /* NO for a missing name; THREW for an uninitialized export (ReferenceError) */
    }
    if (embi_js_pd_is_accessor(d)) {
        return EMBI_JS_NO;
    }
    if ((d->has & EMBI_JS_PD_CONFIGURABLE) && (d->attrs & EMBI_JS_ATTR_CONFIGURABLE)) {
        return EMBI_JS_NO;
    }
    if ((d->has & EMBI_JS_PD_ENUMERABLE) && !(d->attrs & EMBI_JS_ATTR_ENUMERABLE)) {
        return EMBI_JS_NO;
    }
    if ((d->has & EMBI_JS_PD_WRITABLE) && !(d->attrs & EMBI_JS_ATTR_WRITABLE)) {
        return EMBI_JS_NO;
    }
    if ((d->has & EMBI_JS_PD_VALUE) && !embi_js_same_value(d->value, cur.value)) {
        return EMBI_JS_NO;
    }
    return EMBI_JS_YES;
}

/* [[HasProperty]]: an uninitialized export still exists (no TDZ check here). */
static int ns_has(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    if (ns_is_tag(rt, k)) {
        return EMBI_JS_YES;
    }
    return ns_find(rt, ns_entries(o), k) >= 0 ? EMBI_JS_YES : EMBI_JS_NO;
}

static int ns_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    if (ns_is_tag(rt, k)) {
        return EMBI_JS_NO;
    }
    return ns_find(rt, ns_entries(o), k) >= 0 ? EMBI_JS_NO : EMBI_JS_YES;
}

static int ns_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    const embi_js_varray *a = ns_entries(o);
    uint32_t n = ns_count(a);
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(out, embi_js_to_property_key(rt, a->elems[1 + 2 * i]));
    }
    embi_vec_push(out, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG));
    return EMBI_JS_YES;
}

static embi_js_value ns_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                            embi_js_value receiver)
{
    (void)receiver;
    embi_js_pdesc pd;
    int r = ns_get_own(rt, o, k, &pd);
    if (r == EMBI_JS_THREW) {
        return EMBI_JS_EXCEPTION;
    }
    return r == EMBI_JS_YES ? pd.value : EMBI_JS_UNDEFINED;
}

static int ns_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                  embi_js_value receiver)
{
    (void)rt;
    (void)o;
    (void)k;
    (void)v;
    (void)receiver;
    return EMBI_JS_NO;
}

static int ns_get_proto(embi_js_runtime *rt, embi_js_object *o, embi_js_value *out)
{
    (void)rt;
    (void)o;
    *out = EMBI_JS_NULL;
    return EMBI_JS_YES;
}

static int ns_set_proto(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto)
{
    (void)rt;
    (void)o;
    return proto == EMBI_JS_NULL ? EMBI_JS_YES : EMBI_JS_NO;
}

static int ns_is_extensible(embi_js_runtime *rt, embi_js_object *o)
{
    (void)rt;
    (void)o;
    return EMBI_JS_NO;
}

static int ns_prevent_extensions(embi_js_runtime *rt, embi_js_object *o)
{
    (void)rt;
    (void)o;
    return EMBI_JS_YES;
}

static void ns_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_module_ns = {
    .name = "Module",
    .get_own = ns_get_own,
    .define_own = ns_define_own,
    .has = ns_has,
    .delete = ns_delete,
    .own_keys = ns_own_keys,
    .get = ns_get,
    .set = ns_set,
    .get_proto = ns_get_proto,
    .set_proto = ns_set_proto,
    .is_extensible = ns_is_extensible,
    .prevent_extensions = ns_prevent_extensions,
    .trace = ns_trace,
};

typedef struct nsent {
    embi_js_value name; /* an atom (atoms are permanent) */
    mres res;
} nsent;

/* Sorts entries by name (code units): a stable merge sort over a scratch buffer. */
static void nsent_sort(nsent *v, nsent *tmp, uint32_t n)
{
    for (uint32_t w = 1; w < n; w *= 2u) {
        for (uint32_t lo = 0; lo < n; lo += 2u * w) {
            uint32_t mid = lo + w < n ? lo + w : n;
            uint32_t hi = lo + 2u * w < n ? lo + 2u * w : n;
            uint32_t i = lo;
            uint32_t j = mid;
            uint32_t k = lo;
            while (i < mid && j < hi) {
                const embi_js_string *x = (const embi_js_string *)embi_js_as_cell(v[i].name);
                const embi_js_string *y = (const embi_js_string *)embi_js_as_cell(v[j].name);
                tmp[k++] = embi_js_str_compare(x, y) <= 0 ? v[i++] : v[j++];
            }
            while (i < mid) {
                tmp[k++] = v[i++];
            }
            while (j < hi) {
                tmp[k++] = v[j++];
            }
        }
        memcpy(v, tmp, (size_t)n * sizeof *v);
    }
}

/* GetModuleNamespace: the namespace object of holder, created on first use. Every cell created here
 * is stored at once into a cell that is reachable (the namespace object is kept by the module
 * record and its varray by the object), never only into malloc memory that the collector does not
 * scan. */
static embi_js_value namespace_of(embi_js_runtime *rt, embi_js_value holder)
{
    embi_js_module *m = rec_of(holder);
    if (m->ns != EMBI_JS_EMPTY) {
        return m->ns;
    }
    embi_js_object *ns = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_module_ns);
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->ns, embi_js_from_object(ns));
    value_vec names = {nullptr, 0, 0};
    exported_names(rt, holder, &names, ++state_of(rt)->epoch);
    /* The names that resolve to a binding or a namespace; ambiguous names are left out. */
    EMBI_VEC(nsent) ents = {nullptr, 0, 0};
    for (uint32_t i = 0; i < names.len; i++) {
        pair_vec set = {nullptr, 0, 0};
        mres r = resolve_export(rt, holder, names.data[i], &set);
        embi_vec_free(&set);
        if (r.kind == RES_NULL || r.kind == RES_AMBIGUOUS) {
            continue;
        }
        nsent en = {names.data[i], r};
        embi_vec_push(&ents, en);
    }
    embi_vec_free(&names);
    uint32_t n = (uint32_t)ents.len;
    if (n > 1) {
        nsent *tmp = embi_alloc((size_t)n * sizeof *tmp);
        nsent_sort(ents.data, tmp, n);
        embi_free(tmp, (size_t)n * sizeof *tmp);
    }
    embi_js_varray *a = embi_js_varray_new(rt, 1u + 2u * n);
    EMBI_JS_STORE(a, a->elems[0], holder);
    ns->internal = a;
    EMBI_JS_WB(ns, a);
    for (uint32_t i = 0; i < n; i++) {
        EMBI_JS_STORE(a, a->elems[1 + 2 * i], ents.data[i].name);
    }
    for (uint32_t i = 0; i < n; i++) {
        const mres *r = &ents.data[i].res;
        if (r->kind == RES_BINDING) {
            EMBI_JS_STORE(a, a->elems[2 + 2 * i], r->box);
            continue;
        }
        embi_js_value nv = namespace_of(rt, r->holder);
        EMBI_JS_STORE(a, a->elems[2 + 2 * i], embi_js_from_cell(embi_js_box_new(rt, nv)));
    }
    embi_vec_free(&ents);
    return m->ns;
}

/* ------------------------------------------------------------------ linking */
/* Link (ECMA-262 16.2.1.5.1): instantiate every unlinked module reachable from root, then resolve
 * the imports. False with the SyntaxError pending. */
static bool link(embi_js_runtime *rt, embi_js_value root)
{
    value_vec list = {nullptr, 0, 0};
    value_vec stack = {nullptr, 0, 0};
    embi_vec_push(&stack, root);
    bool ok = true;
    while (ok && stack.len > 0) {
        embi_js_value hv = stack.data[--stack.len];
        embi_js_module *m = rec_of(hv);
        if (m->status != EMBJ_UNLINKED || m->dfs_index == 1u) {
            continue;
        }
        m->dfs_index = 1u; /* marks the walk's visits; reset below */
        embi_vec_push(&list, hv);
        for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
            if (m->reqs[i] == EMBI_JS_EMPTY || m->reqs[i] == EMBI_JS_UNDEFINED) {
                embi_js_throw_error(rt, EMBI_JS_PROTO_SYNTAX_ERROR,
                                    "a requested module was not loaded");
                ok = false;
                break;
            }
            embi_vec_push(&stack, m->reqs[i]);
        }
    }
    embi_vec_free(&stack);
    /* Instantiate first, so every box and function declaration exists before any import is read. */
    for (uint32_t li = 0; ok && li < list.len; li++) {
        embi_js_module *m = rec_of(list.data[li]);
        if (m->synthetic) {
            continue; /* a JSON module has no code: its export box was made when it was loaded */
        }
        embi_js_object *closure = embi_jsi_module_closure(rt, m->code);
        embi_js_value gen = embi_jsi_module_instantiate(rt, closure);
        if (gen == EMBI_JS_EXCEPTION) {
            ok = false;
            break;
        }
        EMBI_JS_STORE(embi_js_as_cell(m->holder), m->gen, gen);
        for (uint32_t i = 0; i < m->mod.exps.len; i++) {
            const cmexp *e = &m->mod.exps.data[i];
            if (e->kind == CMEX_LOCAL) {
                EMBI_JS_STORE(embi_js_as_cell(m->holder), m->locals[i], embi_jsi_module_slot(gen, (uint32_t)e->slot));
            }
        }
    }
    /* Imports: each reads the exporter's box, or a fresh box holding a namespace object. */
    for (uint32_t li = 0; ok && li < list.len; li++) {
        embi_js_module *m = rec_of(list.data[li]);
        for (uint32_t i = 0; ok && i < m->mod.imps.len; i++) {
            const cmimp *imp = &m->mod.imps.data[i];
            embi_js_value target = m->reqs[imp->req];
            if (imp->name == EMBI_JS_EMPTY) {
                EMBI_JS_STORE(embi_js_as_cell(m->holder), m->imports[i], embi_js_from_cell(embi_js_box_new(rt, namespace_of(rt, target))));
                continue;
            }
            pair_vec set = {nullptr, 0, 0};
            mres r = resolve_export(rt, target, imp->name, &set);
            embi_vec_free(&set);
            if (r.kind == RES_BINDING) {
                EMBI_JS_STORE(embi_js_as_cell(m->holder), m->imports[i], r.box);
            } else if (r.kind == RES_NAMESPACE) {
                EMBI_JS_STORE(embi_js_as_cell(m->holder), m->imports[i], embi_js_from_cell(embi_js_box_new(rt, namespace_of(rt, r.holder))));
            } else {
                embi_js_throw_error(rt, EMBI_JS_PROTO_SYNTAX_ERROR,
                                    "the requested module does not provide an export that this "
                                    "module imports");
                ok = false;
            }
        }
    }
    /* Indirect exports (ECMA-262 InitializeEnvironment step 9): each must resolve, not to null or an
     * ambiguous name. */
    for (uint32_t li = 0; ok && li < list.len; li++) {
        embi_js_value hv = list.data[li];
        const embi_js_module *m = rec_of(hv);
        for (uint32_t i = 0; ok && i < m->mod.exps.len; i++) {
            const cmexp *e = &m->mod.exps.data[i];
            if (e->kind != CMEX_INDIRECT) {
                continue;
            }
            pair_vec set = {nullptr, 0, 0};
            mres r = resolve_export(rt, hv, e->name, &set);
            embi_vec_free(&set);
            if (r.kind == RES_NULL || r.kind == RES_AMBIGUOUS) {
                embi_js_throw_error(rt, EMBI_JS_PROTO_SYNTAX_ERROR,
                                    "an indirect export does not resolve to a binding");
                ok = false;
            }
        }
    }
    for (uint32_t li = 0; li < list.len; li++) {
        embi_js_module *m = rec_of(list.data[li]);
        m->dfs_index = 0;
        if (ok) {
            m->status = EMBJ_LINKED;
        }
    }
    embi_vec_free(&list);
    return ok;
}

/* ------------------------------------------------------------------ evaluation */

static void async_fulfilled(embi_js_runtime *rt, embi_js_value hv);
static void async_rejected(embi_js_runtime *rt, embi_js_value hv, embi_js_value err);

/* Reaction callables: a varray [kind, a, b] in the object's internal. */
enum { MCB_ASYNC_OK, MCB_ASYNC_ERR, MCB_DYN_OK, MCB_DYN_ERR, MCB_UNDEFER };

static void job_undefer(embi_js_runtime *rt, uint32_t id);

static embi_js_value mcb_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v, int argc,
                              const embi_js_value *argv)
{
    (void)this_v;
    const embi_js_varray *d = (const embi_js_varray *)f->internal;
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    switch (embi_js_as_int32(d->elems[0])) {
    case MCB_ASYNC_OK:
        async_fulfilled(rt, d->elems[1]);
        break;
    case MCB_ASYNC_ERR:
        async_rejected(rt, d->elems[1], v);
        break;
    case MCB_DYN_OK:
        promise_resolve_with(rt, d->elems[1], namespace_of(rt, d->elems[2]));
        break;
    case MCB_DYN_ERR:
        promise_reject_with(rt, d->elems[1], v);
        break;
    case MCB_UNDEFER:
        job_undefer(rt, (uint32_t)embi_js_as_int32(d->elems[1]));
        break;
    default:
        break;
    }
    return EMBI_JS_UNDEFINED;
}

static void mcb_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_mcb = {
    .name = "Function",
    .call = mcb_call,
    .trace = mcb_trace,
};

static embi_js_value mcb_new(embi_js_runtime *rt, int kind, embi_js_value a, embi_js_value b)
{
    embi_js_varray *d = embi_js_varray_new(rt, 3);
    EMBI_JS_STORE(d, d->elems[0], embi_js_from_int32(kind));
    EMBI_JS_STORE(d, d->elems[1], a);
    EMBI_JS_STORE(d, d->elems[2], b);
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_mcb);
    f->internal = d;
    EMBI_JS_WB_STRUCT(f);
    return embi_js_from_object(f);
}

/* ExecuteModule for a synchronous module. False with the exception pending when the body throws. A
 * JSON module has no body. */
static bool execute_module(embi_js_runtime *rt, embi_js_value hv)
{
    if (rec_of(hv)->synthetic) {
        return true;
    }
    return embi_jsi_module_resume(rt, rec_of(hv)->gen) != EMBI_JS_EXCEPTION;
}

/* A promise already rejected with reason. */
static embi_js_value rejected_promise(embi_js_runtime *rt, embi_js_value reason)
{
    embi_js_value pv = embi_js_from_object(embi_js_promise_new(rt));
    promise_reject_with(rt, pv, reason);
    return pv;
}

/* ExecuteAsyncModule: resumes the body and reacts to its completion promise, which settles the
 * module through a job. An error thrown before the first await rejects that promise (as an async
 * function's body does), so it reaches AsyncModuleExecutionRejected the same way a later one does. */
static bool execute_async(embi_js_runtime *rt, embi_js_value hv)
{
    embi_js_value p = embi_jsi_module_resume(rt, rec_of(hv)->gen);
    embi_js_value pv;
    if (p == EMBI_JS_EXCEPTION) {
        pv = rejected_promise(rt, take_exception(rt));
    } else {
        pv = embi_js_promise_resolve(rt, rt->protos[EMBI_JS_PROTO_PROMISE_CTOR], p);
        if (pv == EMBI_JS_EXCEPTION) {
            pv = rejected_promise(rt, take_exception(rt));
        }
    }
    embi_js_value ok = mcb_new(rt, MCB_ASYNC_OK, hv, EMBI_JS_UNDEFINED);
    embi_js_value err = mcb_new(rt, MCB_ASYNC_ERR, hv, EMBI_JS_UNDEFINED);
    (void)embi_js_promise_then(rt, embi_js_as_object(pv), ok, err, nullptr);
    return true;
}

static void settle_top(embi_js_runtime *rt, embi_js_value hv, bool rejected, embi_js_value v)
{
    embi_js_module *m = rec_of(hv);
    if (m->top_cap == EMBI_JS_EMPTY) {
        return;
    }
    if (rejected) {
        promise_reject_with(rt, m->top_cap, v);
    } else {
        promise_resolve_with(rt, m->top_cap, EMBI_JS_UNDEFINED);
    }
}

/* GatherAvailableAncestors (ECMA-262 16.2.1.5.3.4). */
static void gather_ancestors(embi_js_runtime *rt, embi_js_value hv, value_vec *exec)
{
    const embi_js_module *m = rec_of(hv);
    for (uint32_t i = 0; i < m->async_parents.len; i++) {
        embi_js_value pv = m->async_parents.data[i];
        embi_js_module *pm = rec_of(pv);
        embi_js_value root = pm->cycle_root != EMBI_JS_EMPTY ? pm->cycle_root : pv;
        if (contains_value(exec, pv) || rec_of(root)->eval_error != EMBI_JS_EMPTY) {
            continue;
        }
        pm->pending_async--;
        if (pm->pending_async == 0) {
            embi_vec_push(exec, pv);
            if (!pm->has_tla) {
                gather_ancestors(rt, pv, exec);
            }
        }
    }
}

/* AsyncModuleExecutionFulfilled (ECMA-262 16.2.1.5.3.3). */
static void async_fulfilled(embi_js_runtime *rt, embi_js_value hv)
{
    embi_js_module *m = rec_of(hv);
    if (m->status == EMBJ_EVALUATED) {
        return;
    }
    m->async_eval = false;
    m->status = EMBJ_EVALUATED;
    settle_top(rt, hv, false, EMBI_JS_UNDEFINED);
    value_vec exec = {nullptr, 0, 0};
    gather_ancestors(rt, hv, &exec);
    /* Run in [[AsyncEvaluationOrder]] order (insertion sort, the lists are short). */
    for (uint32_t i = 1; i < exec.len; i++) {
        for (uint32_t j = i; j > 0; j--) {
            if (rec_of(exec.data[j - 1])->async_order <= rec_of(exec.data[j])->async_order) {
                break;
            }
            embi_js_value t = exec.data[j - 1];
            exec.data[j - 1] = exec.data[j];
            exec.data[j] = t;
        }
    }
    for (uint32_t i = 0; i < exec.len; i++) {
        embi_js_value ev = exec.data[i];
        embi_js_module *em = rec_of(ev);
        if (em->has_tla) {
            if (!execute_async(rt, ev)) {
                async_rejected(rt, ev, take_exception(rt));
            }
            continue;
        }
        if (!execute_module(rt, ev)) {
            async_rejected(rt, ev, take_exception(rt));
        } else {
            em->async_eval = false;
            em->status = EMBJ_EVALUATED;
            settle_top(rt, ev, false, EMBI_JS_UNDEFINED);
        }
    }
    embi_vec_free(&exec);
}

/* AsyncModuleExecutionRejected (ECMA-262 16.2.1.5.3.2). */
static void async_rejected(embi_js_runtime *rt, embi_js_value hv, embi_js_value err)
{
    embi_js_module *m = rec_of(hv);
    if (m->status == EMBJ_EVALUATED) {
        return;
    }
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->eval_error, err);
    m->status = EMBJ_EVALUATED;
    m->async_eval = false;
    /* The module's own capability settles before its parents': a module imported directly rejects
     * before the modules that depend on it (test262 top-level-await/rejection-order). */
    settle_top(rt, hv, true, err);
    for (uint32_t i = 0; i < m->async_parents.len; i++) {
        async_rejected(rt, m->async_parents.data[i], err);
    }
}

/* InnerModuleEvaluation (ECMA-262 16.2.1.5.3.1). Returns the next index, or -1 with the exception
 * pending. */
static int64_t inner_eval(embi_js_runtime *rt, embi_js_value hv, value_vec *stack,
                          int64_t index)
{
    embi_js_module *m = rec_of(hv);
    if (m->status == EMBJ_EVALUATING_ASYNC || m->status == EMBJ_EVALUATED) {
        if (m->eval_error != EMBI_JS_EMPTY) {
            embi_js_exception_set(rt, m->eval_error);
            return -1;
        }
        return index;
    }
    if (m->status == EMBJ_EVALUATING) {
        return index;
    }
    if (m->status != EMBJ_LINKED) {
        embi_js_throw_error(rt, EMBI_JS_PROTO_SYNTAX_ERROR, "module is not linked");
        return -1;
    }
    m->status = EMBJ_EVALUATING;
    m->dfs_index = (uint32_t)index;
    m->dfs_ancestor = (uint32_t)index;
    m->pending_async = 0;
    index++;
    embi_vec_push(stack, hv);
    for (uint32_t i = 0; i < m->mod.reqs.len; i++) {
        embi_js_value rv = m->reqs[i];
        int64_t r = inner_eval(rt, rv, stack, index);
        if (r < 0) {
            return -1;
        }
        index = r;
        embi_js_module *rm = rec_of(rv);
        if (rm->status == EMBJ_EVALUATING) {
            if (rm->dfs_ancestor < m->dfs_ancestor) {
                m->dfs_ancestor = rm->dfs_ancestor;
            }
        } else {
            rv = rm->cycle_root != EMBI_JS_EMPTY ? rm->cycle_root : rv;
            rm = rec_of(rv);
            if (rm->eval_error != EMBI_JS_EMPTY) {
                embi_js_exception_set(rt, rm->eval_error);
                return -1;
            }
        }
        if (rm->async_eval) {
            m->pending_async++;
            EMBI_JS_WB(embi_js_as_cell(rm->holder), hv);
            embi_vec_push(&rm->async_parents, hv);
        }
    }
    if (m->pending_async > 0 || m->has_tla) {
        m->async_eval = true;
        m->async_order = ++state_of(rt)->async_counter;
        if (m->pending_async == 0 && !execute_async(rt, hv)) {
            return -1;
        }
    } else if (!execute_module(rt, hv)) {
        return -1;
    }
    if (m->dfs_ancestor == m->dfs_index) {
        for (;;) {
            embi_js_value n = stack->data[--stack->len];
            embi_js_module *nm = rec_of(n);
            nm->status = nm->async_eval ? EMBJ_EVALUATING_ASYNC : EMBJ_EVALUATED;
            EMBI_JS_STORE(embi_js_as_cell(nm->holder), nm->cycle_root, hv);
            if (n == hv) {
                break;
            }
        }
    }
    return index;
}

/* Module.Evaluate: returns the module's evaluation promise (created on first use). A module that
 * has started or finished evaluating answers with its cycle root's promise, so every member of a
 * cycle settles together with the root. */
static embi_js_value evaluate(embi_js_runtime *rt, embi_js_value hv)
{
    embi_js_module *m = rec_of(hv);
    if ((m->status == EMBJ_EVALUATING_ASYNC || m->status == EMBJ_EVALUATED) &&
        m->cycle_root != EMBI_JS_EMPTY) {
        hv = m->cycle_root;
        m = rec_of(hv);
    }
    if (m->top_cap != EMBI_JS_EMPTY) {
        return m->top_cap;
    }
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->top_cap, embi_js_from_object(embi_js_promise_new(rt)));
    value_vec stack = {nullptr, 0, 0};
    int64_t r = inner_eval(rt, hv, &stack, 0);
    if (r < 0) {
        embi_js_value err = take_exception(rt);
        for (uint32_t i = 0; i < stack.len; i++) {
            embi_js_module *sm = rec_of(stack.data[i]);
            sm->status = EMBJ_EVALUATED;
            EMBI_JS_STORE(embi_js_as_cell(sm->holder), sm->eval_error, err);
        }
        embi_vec_free(&stack);
        promise_reject_with(rt, m->top_cap, err);
        return m->top_cap;
    }
    embi_vec_free(&stack);
    if (!m->async_eval) {
        promise_resolve_with(rt, m->top_cap, EMBI_JS_UNDEFINED);
    }
    return m->top_cap;
}

/* Finishes a job whose requests are complete and whose graph is loaded: links, evaluates and settles
 * its promise. The job is already removed from the registry (check_jobs). */
static void finish(embi_js_runtime *rt, mjob *job)
{
    if (job->failed) {
        promise_reject_with(rt, job->cap, job->error);
        return;
    }
    if (!link(rt, job->root)) {
        promise_reject_with(rt, job->cap, take_exception(rt));
        return;
    }
    embi_js_value ev = evaluate(rt, job->root);
    if (job->dynamic) {
        embi_js_value ok = mcb_new(rt, MCB_DYN_OK, job->cap, job->root);
        embi_js_value err = mcb_new(rt, MCB_DYN_ERR, job->cap, EMBI_JS_UNDEFINED);
        (void)embi_js_promise_then(rt, embi_js_as_object(ev), ok, err, nullptr);
        return;
    }
    promise_resolve_with(rt, job->cap, ev);
}

/* ------------------------------------------------------------------ public entry points */

embi_js_value embi_js_module_eval(embi_js_realm *r, const char *specifier, size_t len)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value cap = embi_js_from_object(embi_js_promise_new(rt));
    uint32_t jid = job_new(rt, r, cap, false);
    issue(rt, jid, EMBI_JS_EMPTY, 0, specifier, len, EMBI_JS_EMPTY, nullptr, 0);
    job_find(state_of(rt), jid)->guard = false;
    check_jobs(rt);
    return cap;
}

embi_js_value embi_js_module_namespace(embi_js_runtime *rt, const char *key, size_t key_len)
{
    embi_js_value hv = find_module(rt, key, key_len, EMBI_JS_EMPTY);
    if (hv == EMBI_JS_EMPTY) {
        return embi_js_throw_reference_error(rt, "module is not loaded");
    }
    return namespace_of(rt, hv);
}

void embi_js_set_module_loader(embi_js_runtime *rt, embi_js_module_load_fn load, void *user)
{
    mstate *s = state_of(rt);
    s->load = load;
    s->load_user = user;
}

void embi_js_set_import_meta_hook(embi_js_runtime *rt, embi_js_import_meta_fn fn, void *user)
{
    mstate *s = state_of(rt);
    s->meta_fn = fn;
    s->meta_user = user;
}

/* Reads the binding of an IMPORT_GET token (a varray [holder, index]). */
embi_js_value embi_jsm_import_get(embi_js_runtime *rt, embi_js_value token)
{
    const embi_js_varray *t = (const embi_js_varray *)embi_js_as_cell(token);
    const embi_js_module *m = rec_of(t->elems[0]);
    uint32_t idx = (uint32_t)embi_js_as_int32(t->elems[1]);
    embi_js_value box = m->imports ? m->imports[idx] : EMBI_JS_EMPTY;
    if (embi_js_is_empty(box)) {
        return embi_js_throw_reference_error(rt, "import is not linked");
    }
    embi_js_value v = ((embi_js_box *)embi_js_as_cell(box))->value;
    if (embi_js_is_empty(v)) {
        return embi_js_throw_reference_error(rt, "Cannot access an import before initialization");
    }
    return v;
}

embi_js_value embi_jsm_import_meta(embi_js_runtime *rt, embi_js_value holder)
{
    embi_js_module *m = rec_of(holder);
    if (m->meta != EMBI_JS_EMPTY) {
        return m->meta;
    }
    EMBI_JS_STORE(embi_js_as_cell(m->holder), m->meta, embi_js_from_object(embi_js_object_new_class(rt, EMBI_JS_NULL, &embi_js_class_object)));
    mstate *s = state_of(rt);
    if (s->meta_fn) {
        s->meta_fn(rt, m->meta, m->key_c, m->key_len, s->meta_user);
    }
    return m->meta;
}

/* The import attributes of import(specifier, options) (ECMA-262 EvaluateImportCall). Returns the
 * type atom, EMBI_JS_EMPTY when there is none, or EMBI_JS_EXCEPTION with the error pending. Every
 * enumerable own string key of options.with is read, in order, and its value must be a string; a key
 * other than "type" is not supported by this host and is a TypeError. */
static embi_js_value import_type(embi_js_runtime *rt, embi_js_value options)
{
    if (embi_js_is_undefined(options)) {
        return EMBI_JS_EMPTY;
    }
    if (!embi_js_is_object(options)) {
        embi_js_throw_type_error(rt, "The second argument of import() must be an object");
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value with = embi_js_get(rt, embi_js_as_object(options), embi_js_key_c(rt, "with"));
    if (with == EMBI_JS_EXCEPTION) {
        return EMBI_JS_EXCEPTION;
    }
    if (embi_js_is_undefined(with)) {
        return EMBI_JS_EMPTY;
    }
    if (!embi_js_is_object(with)) {
        embi_js_throw_type_error(rt, "The 'with' option must be an object");
        return EMBI_JS_EXCEPTION;
    }
    embi_js_object *wo = embi_js_as_object(with);
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_enumerable_own_keys(rt, wo, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return EMBI_JS_EXCEPTION;
    }
    embi_js_key type_key = embi_js_key_c(rt, "type");
    embi_js_value result = EMBI_JS_EMPTY;
    bool unsupported = false;
    for (uint32_t i = 0; i < keys.len; i++) {
        embi_js_value v = embi_js_get(rt, wo, keys.data[i]);
        if (v == EMBI_JS_EXCEPTION) {
            result = EMBI_JS_EXCEPTION;
            break;
        }
        if (!embi_js_is_string(v)) {
            embi_js_throw_type_error(rt, "Import attribute value must be a string");
            result = EMBI_JS_EXCEPTION;
            break;
        }
        if (keys.data[i] == type_key) {
            size_t n;
            char *u = string_utf8(v, &n);
            result = atom_value(rt, u, n);
            embi_free(u, n + 1u);
        } else {
            unsupported = true;
        }
    }
    embi_vec_free(&keys);
    if (result != EMBI_JS_EXCEPTION && unsupported) {
        embi_js_throw_type_error(rt, "Unsupported import attribute");
        return EMBI_JS_EXCEPTION;
    }
    return result;
}

embi_js_value embi_jsm_dynamic_import(embi_js_runtime *rt, embi_js_value referrer, embi_js_value spec,
                                      embi_js_value options)
{
    embi_js_value capv = embi_js_from_object(embi_js_promise_new(rt));
    embi_js_string *ss = embi_js_to_string(rt, spec);
    if (ss == nullptr) {
        promise_reject_with(rt, capv, take_exception(rt));
        return capv;
    }
    embi_js_value sv = embi_js_from_string(ss);
    embi_js_value type = import_type(rt, options);
    if (type == EMBI_JS_EXCEPTION) {
        promise_reject_with(rt, capv, take_exception(rt));
        return capv;
    }
    embi_js_realm *realm = rt->vm != nullptr ? rt->vm->realm : nullptr;
    const char *ref = nullptr;
    size_t rlen = 0;
    if (embi_js_is_object(referrer)) {
        const embi_js_module *rm = rec_of(referrer);
        ref = rm->key_c;
        rlen = rm->key_len;
        realm = rm->realm;
    }
    size_t slen;
    char *spec_c = string_utf8(sv, &slen);
    uint32_t jid = job_new(rt, realm, capv, true);
    /* import() never evaluates inline: even when the host completes every request synchronously,
     * the job finishes in a job of the queue, so the importing module's evaluation is not
     * preempted (ECMA-262 ContinueDynamicImport runs from a job; test262 verify-dfs checks it). */
    job_find(state_of(rt), jid)->deferred = true;
    issue(rt, jid, EMBI_JS_EMPTY, 0, spec_c, slen, type, ref, rlen);
    embi_free(spec_c, slen + 1u);
    job_find(state_of(rt), jid)->guard = false;
    embi_js_enqueue_microtask(rt, mcb_new(rt, MCB_UNDEFER, embi_js_from_int32((int32_t)jid),
                                          EMBI_JS_UNDEFINED),
                              EMBI_JS_UNDEFINED);
    check_jobs(rt);
    return capv;
}

/* The box of an import, named by its IMPORT_GET token (the varray [holder, index]). EMPTY while the
 * module is not linked. Eval contexts read the boxes of the imports of their module. */
embi_js_value embi_jsm_import_box(embi_js_value token)
{
    const embi_js_varray *t = (const embi_js_varray *)embi_js_as_cell(token);
    const embi_js_module *m = rec_of(t->elems[0]);
    uint32_t idx = (uint32_t)embi_js_as_int32(t->elems[1]);
    return m->imports ? m->imports[idx] : EMBI_JS_EMPTY;
}

void embi_jsm_runtime_free(embi_js_runtime *rt)
{
    mstate *s = rt->modules;
    if (!s) {
        return;
    }
    for (uint32_t i = 0; i < s->mods.len; i++) {
        embi_js_object *ho = embi_js_as_object(s->mods.data[i]);
        module_free(ho->internal);
        ho->internal = nullptr;
    }
    embi_vec_free(&s->mods);
    embi_hashmap_free(&s->map);
    for (uint32_t i = 0; i < s->jobs.len; i++) {
        embi_free(s->jobs.data[i], sizeof(mjob));
    }
    embi_vec_free(&s->jobs);
    embi_vec_free(&s->reqs);
    embi_free(s, sizeof *s);
    rt->modules = nullptr;
}
