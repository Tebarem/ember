#include "emb_test.h"

#include "js/js.h"
#include "js/object.h"

#include <string.h>

/* WeakRef and FinalizationRegistry through script (ES2026 26.1, 26.2): constructors, argument
 * checks, deref, register and unregister. The collector behavior is in test_js_gc_weakref.c. */

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

/* Runs a script that evaluates to true. Logs and clears any exception. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "weakref.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(weakref_constructor_and_prototype)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "typeof WeakRef === 'function' && WeakRef.length === 1 && "
                         "WeakRef.name === 'WeakRef'"));
    EMB_CHECK(js_true(e, "WeakRef.prototype.deref.length === 0 && "
                         "Object.prototype.toString.call(new WeakRef({})) === '[object WeakRef]'"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(new WeakRef({})) === WeakRef.prototype"));
    EMB_CHECK(js_true(e, "(function(){ try { WeakRef({}); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new WeakRef(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new WeakRef(undefined); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new WeakRef(Symbol.for('reg')); return false; } "
                         "catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "typeof new WeakRef(Symbol('unregistered')).deref() === 'symbol'"));
    EMB_CHECK(js_true(e, "(function(){ try { WeakRef.prototype.deref.call({}); return false; } "
                         "catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var t = {}; var w = new WeakRef(t); return w.deref() === t; })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A subclass gets its prototype from new.target. */
EMB_TEST(weakref_subclass_uses_new_target_prototype)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "class W extends WeakRef { m() { return 7; } }; "
                         "var w = new W({}); w.m() === 7 && w instanceof WeakRef"));
    env_free(e);
}

EMB_TEST(finalization_registry_constructor_and_methods)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "typeof FinalizationRegistry === 'function' && "
                         "FinalizationRegistry.length === 1"));
    EMB_CHECK(js_true(e, "FinalizationRegistry.prototype.register.length === 2 && "
                         "FinalizationRegistry.prototype.unregister.length === 1"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new FinalizationRegistry(function(){})) === "
                         "'[object FinalizationRegistry]'"));
    EMB_CHECK(js_true(e, "(function(){ try { FinalizationRegistry(function(){}); return false; } "
                         "catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new FinalizationRegistry(1); return false; } "
                         "catch (x) { return x instanceof TypeError; } })()"));
    /* The target must be holdable weakly, and it must not be the held value. */
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "try { f.register(1, 'x'); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "var o = {}; try { f.register(o, o); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "try { f.register({}, 1, 2); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "return f.register({}, 'held') === undefined; })()"));
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "var tok = {}; f.register({}, 'held', tok); "
                         "return f.unregister(tok) === true && f.unregister(tok) === false; })()"));
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "try { f.unregister(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { FinalizationRegistry.prototype.register.call({}, {}, 1); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A symbol that is not registered is a valid target and a valid token. */
EMB_TEST(symbol_targets_and_tokens)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "(function(){ var f = new FinalizationRegistry(function(){}); "
                         "var tok = Symbol('tok'); f.register(Symbol('t'), 1, tok); "
                         "return f.unregister(tok) === true; })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new FinalizationRegistry(function(){}).register("
                         "Symbol.for('x'), 1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
}

EMB_TEST_MAIN()
