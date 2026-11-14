/*
 * AggregateError (builtins_error.c, J3a): the Error family member that Promise.any rejects with.
 * Its constructor takes an iterable of errors, the message and the cause option, and stores the
 * errors as a non-enumerable array.
 */
#include "emb_test.h"

#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
    size_t mem0; /* live bytes before the runtime existed: must return here */
} env;

static env env_new(void)
{
    env e;
    e.mem0 = embi_mem_live();
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
    EMB_CHECK(embi_mem_live() == e.mem0);
}

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "aggregate.js", 0);
}

/* Evaluates src and checks that its string value is want. */
static void expect_text(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    script threw: %s", src);
        EMB_CHECK(false);
        embi_js_exception_clear(e.rt);
        return;
    }
    embi_js_string *s = embi_js_to_string(e.rt, v);
    bool ok = false;
    if (s != nullptr) {
        embi_sb sb = {nullptr, 0, 0};
        embi_js_str_append_utf8(s, &sb);
        ok = strcmp(embi_sb_cstr(&sb), want) == 0;
        if (!ok) {
            SDL_Log("    script: %s got \"%s\" wanted \"%s\"", src, embi_sb_cstr(&sb), want);
        }
        embi_sb_free(&sb);
    }
    EMB_CHECK(ok);
}

EMB_TEST(aggregate_error_constructor_shape)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_text(e, "AggregateError.length + ',' + AggregateError.name", "2,AggregateError");
    expect_text(e, "Object.getPrototypeOf(AggregateError) === Error ? 'yes' : 'no'", "yes");
    expect_text(e, "Object.getPrototypeOf(AggregateError.prototype) === Error.prototype ? 'yes' : 'no'",
                "yes");
    expect_text(e, "AggregateError.prototype.name + '|' + JSON.stringify(AggregateError.prototype.message)",
                "AggregateError|\"\"");
    env_free(e);
}

EMB_TEST(aggregate_error_instances)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_text(e, "var a = new AggregateError([1, 2], 'msg', { cause: 3 });"
                   "[a.errors.join(','), a.message, a.cause, a.name, a instanceof Error,"
                   " a instanceof AggregateError].join('|')",
                "1,2|msg|3|AggregateError|true|true");
    expect_text(e, "var d = Object.getOwnPropertyDescriptor(new AggregateError([]), 'errors');"
                   "[d.writable, d.enumerable, d.configurable, Array.isArray(d.value)].join(',')",
                   "true,false,true,true");
    expect_text(e, "var n = new AggregateError(new Set(['x', 'y']));"
                   "n.errors.length + ':' + ('message' in n) + ':' + ('cause' in n)",
                   "2:true:false");
    expect_text(e, "AggregateError([7]).errors[0]", "7");
    env_free(e);
}

EMB_TEST(aggregate_error_rejects_non_iterables)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_text(e, "var t = ''; try { new AggregateError(); } catch (x) { t = x.name; } t", "TypeError");
    expect_text(e, "var u = ''; try { new AggregateError(5); } catch (x) { u = x.name; } u",
                "TypeError");
    env_free(e);
}

EMB_TEST(aggregate_error_iterator_errors_propagate)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_text(e, "var bad = { [Symbol.iterator]() { return { next() { throw 'it'; } }; } };"
                   "var v = ''; try { new AggregateError(bad); } catch (x) { v = x; } v",
                "it");
    env_free(e);
}

EMB_TEST_MAIN()
