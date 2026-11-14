#include "emb_test.h"

#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
}

static bool text_is(embi_js_runtime *rt, embi_js_value v, const char *want)
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

static bool exc_name_is(env e, const char *want)
{
    embi_js_runtime *rt = e.rt;
    embi_js_value ex = embi_js_exception_get(rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
        ok = embi_js_is_string(n) && text_is(rt, n, want);
    }
    embi_js_exception_clear(rt);
    return ok;
}

static bool check_text(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw (wanted \"%s\"): %s", want, src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = text_is(e.rt, v, want);
    if (!ok) {
        SDL_Log("    js: %s", src);
    }
    return ok;
}

static bool check_throws(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    js did not throw (wanted %s): %s", want, src);
        return false;
    }
    bool ok = exc_name_is(e, want);
    if (!ok) {
        SDL_Log("    js: %s", src);
    }
    return ok;
}

#define T_TEXT(src_, want_) EMB_CHECK(check_text(e, (src_), (want_)))
#define T_THROWS(src_, want_) EMB_CHECK(check_throws(e, (src_), (want_)))

/* A symbol S for the tests that need symbol keys. */
static void install_symbol(env e)
{
    embi_js_runtime *rt = e.rt;
    embi_js_symbol *s = embi_js_symbol_new(rt, embi_js_from_string(embi_js_atom_intern_utf8(rt, "s", 1)));
    embi_js_define_raw(rt, e.r->global, embi_js_key_c(rt, "S"), embi_js_from_symbol(s), 0);
}

EMB_TEST(reflect_shape_and_lengths)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("typeof Reflect", "object");
    T_TEXT("Object.prototype.toString.call(Reflect)", "[object Reflect]");
    T_TEXT("Object.getOwnPropertyNames(Reflect).length", "13");
    T_TEXT("[Reflect.apply.length, Reflect.construct.length, Reflect.defineProperty.length,"
           " Reflect.deleteProperty.length, Reflect.get.length, Reflect.getOwnPropertyDescriptor.length,"
           " Reflect.getPrototypeOf.length, Reflect.has.length, Reflect.isExtensible.length,"
           " Reflect.ownKeys.length, Reflect.preventExtensions.length, Reflect.set.length,"
           " Reflect.setPrototypeOf.length].join()",
           "3,2,3,2,2,2,1,2,1,1,1,3,2");
    T_TEXT("Reflect.apply.name + ':' + Reflect.ownKeys.name", "apply:ownKeys");
    T_THROWS("new Reflect.get({}, 'x')", "TypeError");
    env_free(e);
}

EMB_TEST(reflect_apply_and_construct)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Reflect.apply(Math.max, null, [1, 5, 3])", "5");
    T_TEXT("Reflect.apply(function () { return this.v; }, { v: 4 }, [])", "4");
    T_THROWS("Reflect.apply(1, null, [])", "TypeError");
    T_THROWS("Reflect.apply(Math.max, null, 1)", "TypeError");
    T_TEXT("Object.getPrototypeOf(Reflect.construct(Boolean, [1], Object)) === Object.prototype", "true");
    T_TEXT("Reflect.construct(Boolean, [0]) instanceof Boolean", "true");
    T_THROWS("Reflect.construct(function () {}, [], 1)", "TypeError");
    T_THROWS("Reflect.construct(() => 1, [])", "TypeError");
    env_free(e);
}

EMB_TEST(reflect_define_delete_get_has)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Reflect.defineProperty(Object.freeze({}), 'x', { value: 1 })", "false");
    T_TEXT("Reflect.defineProperty({}, 'x', { value: 1 })", "true");
    T_THROWS("Reflect.defineProperty(1, 'x', {})", "TypeError");
    T_THROWS("Reflect.defineProperty({}, 'x', 1)", "TypeError");
    T_TEXT("Reflect.deleteProperty(Object.freeze({ a: 1 }), 'a')", "false");
    T_TEXT("Reflect.deleteProperty({ a: 1 }, 'a')", "true");
    T_TEXT("Reflect.get({ get x() { return this.y; } }, 'x', { y: 7 })", "7");
    T_TEXT("Reflect.get(new Proxy({}, { get() { return 9; } }), 'x')", "9");
    T_TEXT("Reflect.has({ x: 1 }, 'x') + ':' + Reflect.has({}, 'toString')", "true:true");
    T_TEXT("Reflect.has(new Proxy({}, { has() { return true; } }), 'anything')", "true");
    env_free(e);
}

EMB_TEST(reflect_descriptors_prototypes_keys)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    install_symbol(e);
    T_TEXT("JSON.stringify(Reflect.getOwnPropertyDescriptor({ a: 1 }, 'a'))",
           "{\"value\":1,\"writable\":true,\"enumerable\":true,\"configurable\":true}");
    T_TEXT("typeof Reflect.getOwnPropertyDescriptor({}, 'x')", "undefined");
    T_TEXT("Reflect.getPrototypeOf([]) === Array.prototype", "true");
    T_THROWS("Reflect.getPrototypeOf(1)", "TypeError");
    T_TEXT("Reflect.isExtensible(Object.preventExtensions({}))", "false");
    T_TEXT("Reflect.isExtensible({})", "true");
    T_TEXT("Reflect.preventExtensions({}) ", "true");
    T_TEXT("Reflect.ownKeys({ b: 1, 1: 0 }).join()", "1,b");
    T_TEXT("typeof Reflect.ownKeys({ [S]: 1 })[0]", "symbol");
    T_TEXT("Reflect.ownKeys(new Proxy({}, { ownKeys() { return ['z', S]; } })).length", "2");
    T_TEXT("var o = {}; Reflect.set(o, 'a', 3) + ':' + o.a", "true:3");
    T_TEXT("Reflect.set(Object.freeze({ a: 1 }), 'a', 2)", "false");
    T_TEXT("var sr = {}; Reflect.set({ set x(v) { sr.v = v; } }, 'x', 5, sr) + ':' + sr.v", "true:5");
    T_TEXT("Reflect.setPrototypeOf({}, null)", "true");
    T_TEXT("Reflect.setPrototypeOf(Object.preventExtensions({}), {})", "false");
    T_THROWS("Reflect.setPrototypeOf({}, 1)", "TypeError");
    env_free(e);
}

EMB_TEST_MAIN()
