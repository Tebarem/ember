/*
 * Runs one variant of a test262 test: a fresh runtime and realm, the host hooks ($262 and print),
 * the harness files the test asks for, then the test itself.
 */
#include "test262.h"

#include "js/js.h"
#include "js/typedarray.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define HOST_ATTRS (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

typedef struct run_ctx {
    uint64_t deadline_ms;
    uint32_t ticks;
    bool timed_out;
    bool async_done;
    bool async_failed;
    char async_msg[T262_REASON_MAX];
    bool job_failed;                    /* a job threw: the first such error is kept for reports */
    char job_msg[T262_REASON_MAX];
    int mod_state;                      /* module tests: 0 pending, 1 fulfilled, 2 rejected */
    char script_path[1024];             /* the test file: the referrer of a script's import() */
    char mod_name[T262_NAME_MAX];       /* module tests: the rejection's error name */
    char mod_reason[T262_REASON_MAX];   /* module tests: the rejection's text */
} run_ctx;

/* The context of the variant running on this thread. The print hook reports async results to it. */
static thread_local run_ctx *t_run;

static bool interrupt_cb(embi_js_runtime *rt, void *user)
{
    (void)rt;
    run_ctx *c = user;
    if (c->timed_out) {
        return true;
    }
    /* The clock is read every 1024 checks, which keeps the hook cheap on hot loops. */
    if ((++c->ticks & 1023u) != 0) {
        return false;
    }
    if (SDL_GetTicks() >= c->deadline_ms) {
        c->timed_out = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ string helpers */

/* s must be flat (embi_js_to_string returns flat strings). */
static bool str_starts_with(const embi_js_string *s, const char *prefix)
{
    size_t n = SDL_strlen(prefix);
    if (embi_js_str_len(s) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (unsigned char)prefix[i]) {
            return false;
        }
    }
    return true;
}

/* Copies s as ASCII, mapping control and non-ASCII units to safe characters. s must be flat. */
static void str_to_ascii(const embi_js_string *s, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    uint32_t n = embi_js_str_len(s);
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 1 < cap; i++) {
        uint16_t c = embi_js_str_at(s, i);
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[k] = '\0';
}

/* Encodes s as UTF-8 (lone surrogates as their 3-byte form). s must be flat. The caller frees
 * the result. */
static char *str_to_utf8(const embi_js_string *s, size_t *out_len)
{
    uint32_t n = embi_js_str_len(s);
    char *buf = SDL_malloc((size_t)n * 3u + 1u);
    if (buf == nullptr) {
        return nullptr;
    }
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = embi_js_str_at(s, i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t d = embi_js_str_at(s, i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u);
                i++;
            }
        }
        if (c < 0x80) {
            buf[k++] = (char)c;
        } else if (c < 0x800) {
            buf[k++] = (char)(0xC0 | (c >> 6));
            buf[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            buf[k++] = (char)(0xE0 | (c >> 12));
            buf[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            buf[k++] = (char)(0xF0 | (c >> 18));
            buf[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            buf[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    buf[k] = '\0';
    *out_len = k;
    return buf;
}

/* ------------------------------------------------------------------ exceptions */

/* Describes a thrown value: name gets the error's name (empty when there is none) and text gets
 * "Name: message" or a best-effort rendering. Errors raised while reading the value are cleared. */
static void describe_exception(embi_js_runtime *rt, embi_js_value ex, char *name, size_t name_cap,
                               char *text, size_t text_cap)
{
    char msg[T262_REASON_MAX] = "";
    name[0] = '\0';
    if (embi_js_is_object(ex)) {
        embi_js_object *o = embi_js_as_object(ex);
        embi_js_value nv = embi_js_get(rt, o, embi_js_key_c(rt, "name"));
        if (nv == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
            nv = EMBI_JS_UNDEFINED;
        }
        embi_js_value mv = embi_js_get(rt, o, embi_js_key_c(rt, "message"));
        if (mv == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
            mv = EMBI_JS_UNDEFINED;
        }
        if (!embi_js_is_string(nv)) {
            /* No name (Test262Error): the constructor's name identifies the error. */
            embi_js_value cv = embi_js_get(rt, o, embi_js_key_c(rt, "constructor"));
            if (cv == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
            } else if (embi_js_is_object(cv)) {
                nv = embi_js_get(rt, embi_js_as_object(cv), embi_js_key_c(rt, "name"));
                if (nv == EMBI_JS_EXCEPTION) {
                    embi_js_exception_clear(rt);
                    nv = EMBI_JS_UNDEFINED;
                }
            }
        }
        /* Char reads need the flat form: a long message may be a rope (J5g). */
        if (embi_js_is_string(nv)) {
            embi_js_string *fn = embi_js_str_flat(rt, nv);
            if (fn != nullptr) {
                str_to_ascii(fn, name, name_cap);
            }
        }
        if (embi_js_is_string(mv)) {
            embi_js_string *fm = embi_js_str_flat(rt, mv);
            if (fm != nullptr) {
                str_to_ascii(fm, msg, sizeof msg);
            }
        }
    } else {
        embi_js_string *s = embi_js_to_string(rt, ex);
        if (s != nullptr) {
            str_to_ascii(s, msg, sizeof msg);
        } else {
            embi_js_exception_clear(rt);
        }
    }
    if (name[0] != '\0' && msg[0] != '\0') {
        SDL_snprintf(text, text_cap, "%s: %s", name, msg);
    } else if (name[0] != '\0') {
        SDL_snprintf(text, text_cap, "%s", name);
    } else {
        SDL_snprintf(text, text_cap, "%s", msg[0] != '\0' ? msg : "thrown value");
    }
}

/* ------------------------------------------------------------------ host hooks */

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : EMBI_JS_UNDEFINED;
}

/* print(v): async tests report through it. The prefixes come from doneprintHandle.js. */
static embi_js_value native_print(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (argc < 1 || t_run == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_string *s = embi_js_to_string(r->rt, argv[0]);
    if (s == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    if (str_starts_with(s, "Test262:AsyncTestComplete")) {
        t_run->async_done = true;
    } else if (str_starts_with(s, "Test262:AsyncTestFailure")) {
        t_run->async_failed = true;
        str_to_ascii(s, t_run->async_msg, sizeof t_run->async_msg);
    }
    return EMBI_JS_UNDEFINED;
}

/* $262.evalScript(source): runs source as a script in the realm; its errors propagate. */
static embi_js_value native_eval_script(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_string *s = embi_js_to_string(r->rt, arg_at(argc, argv, 0));
    if (s == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    size_t n = 0;
    char *src = str_to_utf8(s, &n);
    if (src == nullptr) {
        return embi_js_throw_range_error(r->rt, "out of memory");
    }
    embi_js_value v = embi_js_eval(r, src, n, "evalScript", 0);
    SDL_free(src);
    return v;
}

/* $262.detachArrayBuffer(buffer): DetachArrayBuffer for the binary-data tests. */
static embi_js_value native_detach_buffer(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (!embi_js_arraybuffer_detach(arg_at(argc, argv, 0))) {
        return embi_js_throw_type_error(r->rt, "detachArrayBuffer needs an ArrayBuffer");
    }
    return EMBI_JS_NULL;
}

static embi_js_value native_gc(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_gc(r->rt);
    return EMBI_JS_UNDEFINED;
}

/* One realm per runtime is all the engine has, so cross-realm tests cannot run. They are skipped
 * through the cross-realm entry of unsupported-features.txt. */
static embi_js_value native_create_realm(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_throw_type_error(r->rt, "$262.createRealm is not supported: one realm per runtime");
}

/* Reactions of a module test's evaluation promise (magic 0: fulfilled, 1: rejected). */
static embi_js_value native_module_settled(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    (void)this_v;
    if (t_run == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    if (magic == 0) {
        t_run->mod_state = 1;
        return EMBI_JS_UNDEFINED;
    }
    t_run->mod_state = 2;
    describe_exception(r->rt, arg_at(argc, argv, 0), t_run->mod_name, sizeof t_run->mod_name,
                       t_run->mod_reason, sizeof t_run->mod_reason);
    return EMBI_JS_UNDEFINED;
}

static void install_host(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_define_method(r, r->global, "print", native_print, 1, 0);
    embi_js_object *host = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, host, embi_js_key_c(rt, "global"), embi_js_from_object(r->global),
                       HOST_ATTRS);
    embi_js_define_method(r, host, "evalScript", native_eval_script, 1, 0);
    embi_js_define_method(r, host, "gc", native_gc, 0, 0);
    embi_js_define_method(r, host, "detachArrayBuffer", native_detach_buffer, 1, 0);
    embi_js_define_method(r, host, "createRealm", native_create_realm, 0, 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "$262"), embi_js_from_object(host),
                       HOST_ATTRS);
}

/* ------------------------------------------------------------------ modules */

/* Copies in (n bytes) to out with "." and ".." segments resolved and empty segments dropped. A
 * leading "/" is kept. False when out is too small or the path is too deep. */
static bool normalize_path(const char *in, size_t n, char *out, size_t cap)
{
    size_t len = 0;
    size_t starts[64];
    int nseg = 0;
    if (cap == 0) {
        return false;
    }
    if (n > 0 && in[0] == '/') {
        out[len++] = '/';
    }
    size_t i = 0;
    while (i <= n) {
        size_t e = i;
        while (e < n && in[e] != '/') {
            e++;
        }
        size_t seg = e - i;
        if (seg == 0 || (seg == 1 && in[i] == '.')) {
            /* an empty or "." segment: nothing */
        } else if (seg == 2 && in[i] == '.' && in[i + 1] == '.') {
            if (nseg > 0) {
                size_t at = starts[--nseg];
                len = at > 1 ? at - 1 : at;
            }
        } else {
            if (nseg == 64 || len + seg + 2 > cap) {
                return false;
            }
            if (len > 0 && out[len - 1] != '/') {
                out[len++] = '/';
            }
            starts[nseg++] = len;
            SDL_memcpy(out + len, in + i, seg);
            len += seg;
        }
        i = e + 1;
    }
    out[len] = '\0';
    return true;
}

/* The key of a module: a relative specifier ("./x", "../x") resolves against the directory of the
 * referrer's key; an absolute path is used as it is. Bare specifiers are not supported. */
static bool module_key(const char *ref, size_t rlen, const char *spec, size_t slen, char *out,
                       size_t cap)
{
    char joined[1024];
    size_t n = 0;
    bool relative = slen >= 2 && spec[0] == '.' &&
                    (spec[1] == '/' || (spec[1] == '.' && slen >= 3 && spec[2] == '/'));
    if (relative) {
        if (ref == nullptr) {
            return false;
        }
        size_t d = rlen;
        while (d > 0 && ref[d - 1] != '/') {
            d--;
        }
        if (d + slen + 1 > sizeof joined) {
            return false;
        }
        SDL_memcpy(joined, ref, d);
        SDL_memcpy(joined + d, spec, slen);
        n = d + slen;
    } else {
        bool absolute = slen > 0 && (spec[0] == '/' || (slen > 2 && spec[1] == ':'));
        if (!absolute || slen + 1 > sizeof joined) {
            return false;
        }
        SDL_memcpy(joined, spec, slen);
        n = slen;
    }
    joined[n] = '\0';
    return normalize_path(joined, n, out, cap);
}

/* The module loader of the runner: reads the file a key names and completes the request with it.
 * The host reads synchronously here, so the request completes before the hook returns. */
static void module_load(embi_js_runtime *rt, void *user, uint32_t req, const char *ref, size_t rlen,
                        const char *spec, size_t slen, const char *type)
{
    (void)type;
    /* A script's import() has no referrer: its relative specifiers resolve against the test file. */
    const run_ctx *ctx = user;
    if (ref == nullptr && ctx != nullptr) {
        ref = ctx->script_path;
        rlen = SDL_strlen(ref);
    }
    char key[1024];
    if (!module_key(ref, rlen, spec, slen, key, sizeof key)) {
        embi_js_module_load_failed(rt, req, "unsupported module specifier");
        return;
    }
    size_t n = 0;
    char *src = SDL_LoadFile(key, &n);
    if (src == nullptr) {
        char msg[1200];
        SDL_snprintf(msg, sizeof msg, "cannot read module %s", key);
        embi_js_module_load_failed(rt, req, msg);
        return;
    }
    embi_js_module_loaded(rt, req, key, SDL_strlen(key), src, n);
    SDL_free(src);
}

/* A module test (flags: [module]): the test file is the entry module and the loader resolves its
 * imports from the files. The outcome is the evaluation promise's: a negative test expects the
 * rejection's error name. Module code is strict, so the test runs in the strict variant only. */
static t262_outcome run_module(embi_js_realm *r, embi_js_runtime *rt, run_ctx *ctx, const char *root,
                               const char *rel, const t262_meta *meta, char *reason, size_t cap)
{
    char path[1024];
    if (SDL_snprintf(path, sizeof path, "%s/%s", root, rel) >= (int)sizeof path) {
        SDL_snprintf(reason, cap, "module path too long");
        return T262_FAIL;
    }
    embi_js_value p = embi_js_module_eval(r, path, SDL_strlen(path));
    embi_js_object *on_ok = embi_js_native_function_new(r, "", 1, native_module_settled, 0);
    embi_js_object *on_err = embi_js_native_function_new(r, "", 1, native_module_settled, 1);
    if (on_ok == nullptr || on_err == nullptr) {
        SDL_snprintf(reason, cap, "engine allocation failed");
        return T262_FAIL;
    }
    (void)embi_js_promise_then(rt, embi_js_as_object(p), embi_js_from_object(on_ok),
                               embi_js_from_object(on_err), nullptr);
    embi_js_run_jobs(rt);
    if (ctx->timed_out) {
        SDL_snprintf(reason, cap, "timed out after %u s", t262_timeout_ms() / 1000u);
        return T262_TIMEOUT;
    }
    if (meta->has_negative) {
        if (ctx->mod_state == 1) {
            SDL_snprintf(reason, cap, "expected %s, no error was thrown", meta->negative_type);
            return T262_FAIL;
        }
        if (ctx->mod_state == 2) {
            if (SDL_strstr(ctx->mod_reason, "not yet supported") != nullptr) {
                SDL_snprintf(reason, cap, "unsupported syntax, expected %s: %s",
                             meta->negative_type, ctx->mod_reason);
                return T262_FAIL;
            }
            if (SDL_strcmp(ctx->mod_name, meta->negative_type) == 0) {
                return T262_PASS;
            }
            SDL_snprintf(reason, cap, "expected %s, got %s", meta->negative_type, ctx->mod_reason);
            return T262_FAIL;
        }
        SDL_snprintf(reason, cap, "module did not settle");
        return T262_FAIL;
    }
    if (ctx->mod_state == 2) {
        SDL_snprintf(reason, cap, "threw %s", ctx->mod_reason);
        return T262_FAIL;
    }
    if (ctx->mod_state != 1) {
        SDL_snprintf(reason, cap, "module evaluation did not complete");
        return T262_FAIL;
    }
    return T262_PASS;
}

/* ------------------------------------------------------------------ running */

/* HostReportErrors: a job that throws does not fail the test (test262 checks through $DONE and
 * the asserts), but the first error is kept so a test that never completes can say why. */
static void report_job_error(embi_js_runtime *rt, embi_js_value ex, void *user)
{
    run_ctx *ctx = user;
    if (ctx->job_failed) {
        return;
    }
    ctx->job_failed = true;
    char ename[T262_NAME_MAX];
    describe_exception(rt, ex, ename, sizeof ename, ctx->job_msg, sizeof ctx->job_msg);
}

/* Evaluates a harness or test source. Returns PASS when it completes without throwing. */
static t262_outcome run_source(embi_js_realm *r, embi_js_runtime *rt, run_ctx *ctx,
                               const char *name, const char *src, size_t len, unsigned flags,
                               char *reason, size_t cap)
{
    embi_js_value v = embi_js_eval(r, src, len, name, flags);
    if (ctx->timed_out) {
        SDL_snprintf(reason, cap, "timed out after %u s in %s", t262_timeout_ms() / 1000u, name);
        return T262_TIMEOUT;
    }
    if (v != EMBI_JS_EXCEPTION) {
        return T262_PASS;
    }
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    char ename[T262_NAME_MAX];
    char text[T262_REASON_MAX];
    describe_exception(rt, ex, ename, sizeof ename, text, sizeof text);
    SDL_snprintf(reason, cap, "%s threw %s", name, text);
    return T262_FAIL;
}

static t262_outcome run_harness_file(embi_js_realm *r, embi_js_runtime *rt, run_ctx *ctx,
                                     const char *root, const char *name, unsigned flags,
                                     char *reason, size_t cap)
{
    char path[1024];
    if (SDL_snprintf(path, sizeof path, "%s/harness/%s", root, name) >= (int)sizeof path) {
        SDL_snprintf(reason, cap, "harness path too long: %s", name);
        return T262_FAIL;
    }
    size_t len = 0;
    char *src = SDL_LoadFile(path, &len);
    if (src == nullptr) {
        SDL_snprintf(reason, cap, "missing harness file %s", name);
        return T262_FAIL;
    }
    t262_outcome out = run_source(r, rt, ctx, name, src, len, flags, reason, cap);
    SDL_free(src);
    return out;
}

/* Negative tests expect the named error from the test source itself, not from the harness. */
static t262_outcome check_negative(embi_js_runtime *rt, run_ctx *ctx, embi_js_value v,
                                   const t262_meta *meta, char *reason, size_t cap)
{
    if (ctx->timed_out) {
        SDL_snprintf(reason, cap, "timed out after %u s", t262_timeout_ms() / 1000u);
        return T262_TIMEOUT;
    }
    if (v != EMBI_JS_EXCEPTION) {
        SDL_snprintf(reason, cap, "expected %s, no error was thrown", meta->negative_type);
        return T262_FAIL;
    }
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    char ename[T262_NAME_MAX];
    char text[T262_REASON_MAX];
    describe_exception(rt, ex, ename, sizeof ename, text, sizeof text);
    /* The parser rejects unimplemented syntax with a SyntaxError too. That is not the error the
     * test asks for, so it must not pass a negative test. */
    if (SDL_strstr(text, "not yet supported") != nullptr) {
        SDL_snprintf(reason, cap, "unsupported syntax, expected %s: %s", meta->negative_type, text);
        return T262_FAIL;
    }
    if (SDL_strcmp(ename, meta->negative_type) == 0) {
        return T262_PASS;
    }
    SDL_snprintf(reason, cap, "expected %s, got %s", meta->negative_type, text);
    return T262_FAIL;
}

static t262_outcome run_variant(embi_js_realm *r, embi_js_runtime *rt, run_ctx *ctx,
                                const char *root, const char *rel, const char *src, size_t len,
                                const t262_meta *meta, bool strict, char *reason, size_t cap)
{
    unsigned flags = strict ? EMBI_JS_EVAL_STRICT : 0u;
    t262_outcome o;

    if ((meta->flags & T262_FLAG_RAW) == 0) {
        static const char *const base[] = {"assert.js", "sta.js"};
        for (size_t i = 0; i < sizeof base / sizeof base[0]; i++) {
            o = run_harness_file(r, rt, ctx, root, base[i], flags, reason, cap);
            if (o != T262_PASS) {
                return o;
            }
        }
        if ((meta->flags & T262_FLAG_ASYNC) != 0) {
            o = run_harness_file(r, rt, ctx, root, "doneprintHandle.js", flags, reason, cap);
            if (o != T262_PASS) {
                return o;
            }
        }
        for (int i = 0; i < meta->n_includes; i++) {
            o = run_harness_file(r, rt, ctx, root, meta->includes[i], flags, reason, cap);
            if (o != T262_PASS) {
                return o;
            }
        }
    }

    if ((meta->flags & T262_FLAG_MODULE) != 0) {
        t262_outcome mo = run_module(r, rt, ctx, root, rel, meta, reason, cap);
        if (mo != T262_PASS) {
            return mo;
        }
    } else {
        embi_js_value v = embi_js_eval(r, src, len, rel, flags);
        if (meta->has_negative) {
            return check_negative(rt, ctx, v, meta, reason, cap);
        }
        if (ctx->timed_out) {
            SDL_snprintf(reason, cap, "timed out after %u s", t262_timeout_ms() / 1000u);
            return T262_TIMEOUT;
        }
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_value ex = embi_js_exception_get(rt);
            embi_js_exception_clear(rt);
            char ename[T262_NAME_MAX];
            char text[T262_REASON_MAX];
            describe_exception(rt, ex, ename, sizeof ename, text, sizeof text);
            SDL_snprintf(reason, cap, "threw %s", text);
            return T262_FAIL;
        }
    }
    /* Jobs run after the script, for every test: promise reactions and $DONE come from them. */
    embi_js_run_jobs(rt);
    if (ctx->timed_out) {
        SDL_snprintf(reason, cap, "timed out after %u s", t262_timeout_ms() / 1000u);
        return T262_TIMEOUT;
    }
    if ((meta->flags & T262_FLAG_ASYNC) != 0) {
        if (ctx->async_failed) {
            SDL_snprintf(reason, cap, "async failure: %s", ctx->async_msg);
            return T262_FAIL;
        }
        if (!ctx->async_done) {
            if (ctx->job_failed) {
                SDL_snprintf(reason, cap, "no async completion signal; a job threw: %s",
                             ctx->job_msg);
            } else {
                SDL_snprintf(reason, cap, "no async completion signal");
            }
            return T262_FAIL;
        }
    }
    return T262_PASS;
}

static uint32_t g_gc_stress;
static bool g_gc_generational;
static uint32_t g_rope_min;

void t262_set_gc(uint32_t stress_every, bool generational)
{
    g_gc_stress = stress_every;
    g_gc_generational = generational;
}

uint32_t t262_timeout_ms(void)
{
    return T262_TIMEOUT_MS * (g_gc_stress != 0 ? T262_STRESS_SCALE : 1u);
}

void t262_set_rope_min(uint32_t rope_min)
{
    g_rope_min = rope_min;
}

t262_outcome t262_run(const char *root, const char *rel, const char *src, size_t len,
                      const t262_meta *meta, bool strict, char *reason, size_t reason_cap)
{
    reason[0] = '\0';
    /* The module keys are absolute paths: a relative root is resolved against the working directory. */
    char abs_root[1024];
    bool root_abs = root[0] == '/' || (root[0] != 0 && root[1] == ':');
    if (!root_abs) {
        char *cwd = SDL_GetCurrentDirectory();
        if (cwd == nullptr ||
            SDL_snprintf(abs_root, sizeof abs_root, "%s/%s", cwd, root) >= (int)sizeof abs_root) {
            SDL_free(cwd);
            SDL_snprintf(reason, reason_cap, "cannot resolve the test root");
            return T262_FAIL;
        }
        SDL_free(cwd);
        for (char *c = abs_root; *c; c++) {
            if (*c == 92) {
                *c = '/';
            }
        }
        root = abs_root;
    }
    run_ctx ctx;
    SDL_memset(&ctx, 0, sizeof ctx);
    ctx.deadline_ms = SDL_GetTicks() + t262_timeout_ms();

    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = rt != nullptr ? embi_js_realm_new(rt) : nullptr;
    if (r == nullptr) {
        if (rt != nullptr) {
            embi_js_runtime_free(rt);
        }
        SDL_snprintf(reason, reason_cap, "engine allocation failed");
        return T262_FAIL;
    }
    embi_js_set_rope_min(rt, g_rope_min);
    embi_js_gc_set_stress(rt, g_gc_stress);
    if (g_gc_generational) {
        embi_js_gc_set_generational(rt, true);
    }
    install_host(r);
    SDL_snprintf(ctx.script_path, sizeof ctx.script_path, "%s/%s", root, rel);
    embi_js_set_module_loader(rt, module_load, &ctx);
    embi_js_set_interrupt(rt, interrupt_cb, &ctx);
    embi_js_set_host_report_error(rt, report_job_error, &ctx);

    t_run = &ctx;
    t262_outcome out = run_variant(r, rt, &ctx, root, rel, src, len, meta, strict, reason,
                                   reason_cap);
    t_run = nullptr;

    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    return out;
}
