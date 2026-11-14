#pragma once
/*
 * Shared helpers for the ES2015+ language tests (test_es6_*.c). Not a test itself: the name does
 * not match test_*.c, so the build does not register it.
 */
#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
#include <string.h>

typedef struct es6_env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} es6_env;

static inline es6_env es6_env_new(void)
{
    es6_env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static inline void es6_env_free(es6_env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static inline embi_js_value es6_run(es6_env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
}

static inline double es6_num(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

static inline bool es6_text_is(embi_js_runtime *rt, embi_js_value v, const char *want)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

static inline bool es6_exc_name_is(embi_js_runtime *rt, const char *want)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
    if (!embi_js_is_string(n)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(n), &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

static inline void es6_note(const char *src)
{
    SDL_Log("    in: %s", src);
}

/* Logs the message of the pending exception (for a failed check). */
static inline void es6_note_exc(embi_js_runtime *rt)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "message"));
    if (!embi_js_is_string(m)) {
        return;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(m), &sb);
    SDL_Log("    threw: %s", embi_sb_cstr(&sb));
    embi_sb_free(&sb);
}

#define E6_NUM(e_, src_, want_)                                                                \
    do {                                                                                       \
        embi_js_value v_ = es6_run((e_), (src_));                                              \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && embi_js_is_number(v_) &&                         \
                   es6_num(v_) == (double)(want_);                                             \
        if (!ok_) {                                                                            \
            if (v_ == EMBI_JS_EXCEPTION) {                                                     \
                es6_note_exc((e_).rt);                                         \
                embi_js_exception_clear((e_).rt);                                              \
            }                                                                                  \
            es6_note(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
    } while (0)

#define E6_TEXT(e_, src_, want_)                                                               \
    do {                                                                                       \
        embi_js_value v_ = es6_run((e_), (src_));                                              \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && es6_text_is((e_).rt, v_, (want_));               \
        if (!ok_) {                                                                            \
            if (v_ == EMBI_JS_EXCEPTION) {                                                     \
                embi_js_exception_clear((e_).rt);                                              \
            }                                                                                  \
            es6_note(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
    } while (0)

/* A value that is the boolean true (or false, per want_). */
#define E6_BOOL(e_, src_, want_)                                                               \
    do {                                                                                       \
        embi_js_value v_ = es6_run((e_), (src_));                                              \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && embi_js_is_bool(v_) &&                           \
                   embi_js_as_bool(v_) == (bool)(want_);                                       \
        if (!ok_) {                                                                            \
            if (v_ == EMBI_JS_EXCEPTION) {                                                     \
                embi_js_exception_clear((e_).rt);                                              \
            }                                                                                  \
            es6_note(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
    } while (0)

/* The source throws, and the thrown error has the given constructor name. */
#define E6_THROWS(e_, src_, name_)                                                             \
    do {                                                                                       \
        embi_js_value v_ = es6_run((e_), (src_));                                              \
        bool ok_ = v_ == EMBI_JS_EXCEPTION && es6_exc_name_is((e_).rt, (name_));               \
        if (!ok_) {                                                                            \
            es6_note(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

/* The source is a SyntaxError at parse time (or throws one). */
#define E6_SYNTAX(e_, src_) E6_THROWS((e_), (src_), "SyntaxError")
