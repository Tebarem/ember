#include "emb_test.h"

#include "js/heap.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/object.h"
#include "js/runtime.h"

#include <string.h>

/* W5a0: realm_new_ex, realm_set_global_proto, native data slot and define_getset. */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static const embi_js_class host_global_class = {
    .name = "Window",
    .flags = EMBI_JS_CLASS_IC,
};

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static env env_new_ex(const embi_js_class *klass, void *internal)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new_ex(e.rt, klass, internal) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = v == embi_js_from_bool(true);
    if (!ok) {
        SDL_Log("    js not true: %s", src);
    }
    return ok;
}

static void def_global(env e, const char *name, embi_js_object *f)
{
    embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, name), embi_js_from_object(f),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

/* ------------------------------------------------------------------ realm_new_ex */

static int marker_a;

EMB_TEST(host_realm_new_ex_global_class_and_internal)
{
    size_t mem0 = embi_mem_live();
    env e = env_new_ex(&host_global_class, &marker_a);
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(e.r->global->klass == &host_global_class);
    EMB_CHECK(e.r->global->internal == &marker_a);
    EMB_CHECK(js_true(e, "this === globalThis && typeof Array === 'function' && "
                         "typeof eval === 'function' && Math.max(1, 2) === 2"));
    /* The global's prototype stays Object.prototype until a host changes it. */
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(globalThis) === Object.prototype"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(host_realm_new_ex_null_class_is_plain)
{
    size_t mem0 = embi_mem_live();
    env e = env_new_ex(nullptr, nullptr);
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(e.r->global->klass == &embi_js_class_object);
    EMB_CHECK(e.r->global->internal == nullptr);
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(globalThis) === Object.prototype"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* ------------------------------------------------------------------ realm_set_global_proto */

EMB_TEST(host_realm_set_global_proto_chain)
{
    size_t mem0 = embi_mem_live();
    env e = env_new_ex(&host_global_class, nullptr);
    EMB_REQUIRE(e.r != nullptr);
    /* Window.prototype -> WindowProperties -> EventTarget.prototype -> Object.prototype, built
     * from plain objects here. */
    embi_js_object *ev = embi_js_object_new_plain(e.rt);
    embi_js_define_raw(e.rt, ev, embi_js_key_c(e.rt, "addEventListener"), embi_js_number(1),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_object *wp = embi_js_object_new(e.rt, embi_js_from_object(ev));
    embi_js_define_raw(e.rt, wp, embi_js_key_c(e.rt, "hostWindowProp"), embi_js_number(42),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_realm_set_global_proto(e.r, wp);
    embi_js_gc(e.rt); /* the new prototype is kept alive by the global alone */
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(globalThis) !== Object.prototype"));
    EMB_CHECK(js_true(e, "hostWindowProp === 42 && typeof addEventListener === 'number'"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(Object.getPrototypeOf(globalThis)).addEventListener === 1"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf(globalThis))) === Object.prototype"));
    EMB_CHECK(js_true(e, "globalThis.toString === Object.prototype.toString"));
    EMB_CHECK(js_true(e, "'eval' in globalThis && Array.isArray([])"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* ------------------------------------------------------------------ native data slot */

static embi_js_value read_data_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    const int *d = embi_js_native_data_current(r);
    return embi_js_number(d ? *d + magic : -1);
}

static int data_b = 7;
static int data_a = 40;

/* Outer native: reads its own data, runs JS that calls the inner function (with other data),
 * then reads its data again. Returns true when the data was restored. */
static embi_js_value nest_outer_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    void *before = embi_js_native_data_current(r);
    embi_js_value v = embi_js_eval(r, "inner()", 7, "nest.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_JS_EXCEPTION;
    }
    void *after = embi_js_native_data_current(r);
    return embi_js_from_bool(before == &data_a && after == &data_a);
}

static embi_js_value plain_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_from_bool(embi_js_native_data_current(r) == nullptr);
}

EMB_TEST(host_native_data_read_and_restore)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_object *inner = embi_js_native_function_new_data(e.r, "inner", 0, read_data_fn, 1,
                                                              &data_b);
    embi_js_object *outer = embi_js_native_function_new_data(e.r, "outer", 0, nest_outer_fn, 0,
                                                              &data_a);
    embi_js_object *read = embi_js_native_function_new_data(e.r, "read", 0, read_data_fn, 3,
                                                             &data_a);
    embi_js_object *plain = embi_js_native_function_new(e.r, "plain", 0, plain_fn, 0);
    def_global(e, "inner", inner);
    def_global(e, "outer", outer);
    def_global(e, "read", read);
    def_global(e, "plain", plain);
    EMB_CHECK(js_true(e, "read() === 43"));
    EMB_CHECK(js_true(e, "plain()"));
    EMB_CHECK(js_true(e, "inner() === 8"));
    EMB_CHECK(js_true(e, "outer()"));
    /* Outside any call there is no data. */
    EMB_CHECK(embi_js_native_data_current(e.r) == nullptr);
    EMB_CHECK(js_true(e, "read() === 43"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

static embi_js_value ctor_probe(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)new_target;
    const int *d = embi_js_native_data_current(r);
    embi_js_object *o = embi_js_object_new_plain(r->rt);
    embi_js_define_raw(r->rt, o, embi_js_key_c(r->rt, "seen"), embi_js_number(d ? *d : -1),
                       EMBI_JS_ATTR_DEFAULT);
    return embi_js_from_object(o);
}

EMB_TEST(host_native_data_constructor_path)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_object *ctor = embi_js_native_constructor_new(e.r, "Probe", 0, ctor_probe, 0);
    /* The native constructor path takes data from the same slot as a call; set it directly. */
    ((embi_js_native_data *)ctor->internal)->data = &data_b;
    def_global(e, "Probe", ctor);
    EMB_CHECK(js_true(e, "new Probe().seen === 7"));
    EMB_CHECK(js_true(e, "Probe.name === 'Probe'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* ------------------------------------------------------------------ define_getset */

static int last_set = -1;

static embi_js_value answer_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_number(magic);
}

static embi_js_value answer_set(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    double d = 0;
    if (argc > 0 && embi_js_is_number(argv[0]) && embi_js_to_number(r->rt, argv[0], &d)) {
        last_set = (int)d;
    }
    return EMBI_JS_UNDEFINED;
}

EMB_TEST(host_define_getset_get_set_and_names)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_object *o = embi_js_object_new_plain(e.rt);
    embi_js_define_getset(e.r, o, "answer", answer_get, answer_set, 42,
                          EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_object *ro = embi_js_object_new_plain(e.rt);
    embi_js_define_getset(e.r, ro, "fixed", answer_get, nullptr, 5, 0);
    def_global(e, "o", o);
    def_global(e, "ro", ro);
    EMB_CHECK(js_true(e, "o.answer === 42"));
    EMB_CHECK(js_true(e, "(o.answer = 9, true)"));
    EMB_CHECK(last_set == 9);
    EMB_CHECK(js_true(e, "Object.keys(o).indexOf('answer') >= 0"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(o, 'answer').get.name === 'get answer'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(o, 'answer').set.name === 'set answer'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(o, 'answer').set.length === 1"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(o, 'answer').get.length === 0"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(o, 'answer').configurable === true"));
    /* No setter: assignment is ignored in sloppy code and throws in strict code. */
    EMB_CHECK(js_true(e, "ro.fixed === 5 && (ro.fixed = 1, ro.fixed === 5)"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(ro, 'fixed').set === undefined"));
    EMB_CHECK(js_true(e, "Object.keys(ro).indexOf('fixed') < 0"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(ro, 'fixed').enumerable === false"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(ro, 'fixed').configurable === false"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(host_define_getset_survives_gc)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_object *o = embi_js_object_new_plain(e.rt);
    def_global(e, "o", o);
    embi_js_define_getset(e.r, o, "answer", answer_get, nullptr, 3, EMBI_JS_ATTR_ENUMERABLE);
    embi_js_gc(e.rt);
    EMB_CHECK(js_true(e, "o.answer === 3"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
