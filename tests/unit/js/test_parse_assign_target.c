#include "emb_test.h"

#include "base/base.h"
#include "js/compiler.h"
#include "js/js.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* Regression: the parser used to read an uninitialised cover-grammar flag (expr.literal) for
 * assignment targets that are not references, and sometimes reported "not yet supported:
 * destructuring assignment" instead of the SyntaxError of an invalid target. */

static bool exc_msg_has(embi_js_runtime *rt, const char *want)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "message"));
    if (!embi_js_is_string(m)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(m), &sb);
    bool ok = strstr(embi_sb_cstr(&sb), want) != nullptr;
    embi_sb_free(&sb);
    return ok;
}

static bool exc_is_syntax(embi_js_runtime *rt)
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
    bool ok = strcmp(embi_sb_cstr(&sb), "SyntaxError") == 0;
    embi_sb_free(&sb);
    return ok;
}

/* Fills the stack below the caller and a freed heap block with 0xA5 so that any uninitialised
 * read sees a non-zero value. */
static void poison(unsigned round)
{
    volatile unsigned char stack[16384];
    for (size_t i = 0; i < sizeof stack; i++) {
        stack[i] = 0xA5;
    }
    size_t n = 4096u + (round % 7u) * 512u;
    unsigned char *h = SDL_malloc(n);
    if (h) {
        memset(h, 0xA5, n);
        SDL_free(h);
    }
}

static const char *const shapes[] = {
    "(x, y = z) = 1;",
    "((x, y = z)) = 1;",
    "(true ? false : true) = 1;",
    "((true ? false : true)) = 1;",
    "(x, y) = 1;",
    "(a + b) = 1;",
    "(a || b) = 1;",
    "(typeof a) = 1;",
    "(new X) = 1;",
};

EMB_TEST(assign_target_invalid_is_syntax_error)
{
    for (int strict = 0; strict < 2; strict++) {
        for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
            char src[128];
            SDL_snprintf(src, sizeof src, "%s%s", strict ? "'use strict'; " : "", shapes[s]);
            for (unsigned round = 0; round < 200; round++) {
                poison(round);
                embi_js_runtime *rt = embi_js_runtime_new();
                EMB_REQUIRE(rt != nullptr);
                embi_js_realm *r = embi_js_realm_new(rt);
                EMB_REQUIRE(r != nullptr);
                poison(round + 1);
                embi_js_value v = embi_js_eval(r, src, strlen(src), "t.js", 0);
                EMB_CHECK(v == EMBI_JS_EXCEPTION);
                EMB_CHECK(exc_is_syntax(rt));
                EMB_CHECK(!exc_msg_has(rt, "not yet supported"));
                EMB_CHECK(exc_msg_has(rt, "Invalid left-hand side"));
                embi_js_exception_clear(rt);
                embi_js_realm_free(r);
                embi_js_runtime_free(rt);
            }
        }
    }
}

/* Array and object literal targets are destructuring (J2b). A valid pattern assigns; the iterable
 * check of an array pattern throws a TypeError at run time; a parenthesized pattern is an invalid
 * target and a SyntaxError. */
EMB_TEST(assign_target_literal_is_destructuring)
{
    static const struct {
        const char *src;
        int outcome; /* 0: runs normally, 1: TypeError at run time, 2: SyntaxError */
    } cases[] = {
        {"[a, b] = [1, 2];", 0},
        {"[a] = 1;", 1},
        {"({a: b}) = 1;", 2},
    };
    for (size_t s = 0; s < sizeof cases / sizeof cases[0]; s++) {
        for (unsigned round = 0; round < 50; round++) {
            poison(round);
            embi_js_runtime *rt = embi_js_runtime_new();
            EMB_REQUIRE(rt != nullptr);
            embi_js_realm *r = embi_js_realm_new(rt);
            EMB_REQUIRE(r != nullptr);
            embi_js_value v = embi_js_eval(r, cases[s].src, strlen(cases[s].src), "t.js", 0);
            if (cases[s].outcome == 0) {
                EMB_CHECK(v != EMBI_JS_EXCEPTION);
            } else {
                EMB_CHECK(v == EMBI_JS_EXCEPTION);
                EMB_CHECK(exc_is_syntax(rt) == (cases[s].outcome == 2));
                embi_js_exception_clear(rt);
            }
            embi_js_realm_free(r);
            embi_js_runtime_free(rt);
        }
    }
}

EMB_TEST_MAIN()
