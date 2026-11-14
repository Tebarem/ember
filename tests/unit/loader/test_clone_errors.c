/*
 * Structured clone, failures: values that cannot be cloned raise a DataCloneError and leave the
 * output as it was, a bad transfer list is refused before anything is detached, deep nesting is
 * refused rather than overflowing the native stack, and malformed bytes give an Error. The
 * deserializer is also run over every truncation of a valid stream, and over pseudo-random bodies,
 * which must neither crash nor leak.
 */
#include "loader_test.h"

#include "js/js.h"
#include "loader/clone.h"

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_thread.h>

#include <stdio.h>
#include <string.h>

static embi_js_runtime *g_rt;
static embi_js_realm *g_r;

static void clear_exception(void)
{
    embi_js_exception_clear(g_rt);
}

static embi_js_value js_eval(const char *src)
{
    return embi_js_eval(g_r, src, strlen(src), "clone_errors.js", 0);
}

static bool js_true(const char *src)
{
    embi_js_value v = js_eval(src);
    if (v == EMBI_JS_EXCEPTION) {
        clear_exception();
        return false;
    }
    return v == EMBI_JS_TRUE;
}

static void set_global(const char *name, embi_js_value v)
{
    (void)embi_js_obj_set(g_rt, g_r->global, embi_js_key_c(g_rt, name), v,
                          embi_js_from_object(g_r->global));
}

static embi_js_value get_global(const char *name)
{
    return embi_js_obj_get(g_rt, g_r->global, embi_js_key_c(g_rt, name),
                           embi_js_from_object(g_r->global));
}

static bool setup(void)
{
    g_rt = embi_js_runtime_new();
    if (g_rt == nullptr) {
        return false;
    }
    g_r = embi_js_realm_new(g_rt);
    return g_r != nullptr;
}

static void teardown(void)
{
    embi_js_realm_free(g_r);
    embi_js_runtime_free(g_rt);
    g_r = nullptr;
    g_rt = nullptr;
}

/* Serializes expr (evaluated as globalThis.src) and expects a DataCloneError. */
static bool expect_data_clone_error(const char *expr)
{
    char src[1024];
    snprintf(src, sizeof src, "globalThis.src = (%s);", expr);
    (void)js_eval(src);
    embi_sb sb = {nullptr, 0, 0};
    bool ok = embi_clone_serialize(g_r, get_global("src"), nullptr, 0, &sb);
    embi_sb_free(&sb);
    if (ok) {
        fprintf(stderr, "    expected a DataCloneError for: %s\n", expr);
        return false;
    }
    embi_js_value exc = embi_js_exception_get(g_rt);
    set_global("__e", exc);
    clear_exception();
    return js_true("__e instanceof Error && __e.name === 'DataCloneError'");
}

EMB_TEST(clone_refuses_uncloneable_values)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(expect_data_clone_error("function () {}"));
    EMB_CHECK(expect_data_clone_error("Symbol('s')"));
    EMB_CHECK(expect_data_clone_error("({inner: [{f: function () {}}]})"));
    EMB_CHECK(expect_data_clone_error("new Proxy({}, {})"));
    EMB_CHECK(expect_data_clone_error("new WeakMap()"));
    EMB_CHECK(expect_data_clone_error("new WeakSet()"));
    EMB_CHECK(expect_data_clone_error("new WeakRef({})"));
    EMB_CHECK(expect_data_clone_error("Promise.resolve(1)"));
    EMB_CHECK(expect_data_clone_error("(function () { return arguments; })()"));
    EMB_CHECK(expect_data_clone_error("(function* () {})()"));
    EMB_CHECK(expect_data_clone_error("new SharedArrayBuffer(4)"));
    EMB_CHECK(expect_data_clone_error("(function () { var b = new ArrayBuffer(4); b.transfer(); "
                                      "return b; })()"));
    teardown();
}

EMB_TEST(clone_failure_restores_output_length)
{
    EMB_REQUIRE(setup());
    (void)js_eval("globalThis.src = {ok: 1, bad: function () {}};");
    embi_sb sb = {nullptr, 0, 0};
    embi_sb_append(&sb, (embi_str){"XYZ", 3});
    EMB_CHECK(!embi_clone_serialize(g_r, get_global("src"), nullptr, 0, &sb));
    clear_exception();
    EMB_CHECK_EQ_INT(sb.len, 3);
    EMB_CHECK(memcmp(sb.data, "XYZ", 3) == 0);
    embi_sb_free(&sb);
    teardown();
}

EMB_TEST(clone_bad_transfer_lists_are_refused)
{
    EMB_REQUIRE(setup());
    (void)js_eval("globalThis.src = {b: new ArrayBuffer(4), n: 1};"
                  "globalThis.gone = new ArrayBuffer(2); gone.transfer();");
    embi_js_value obj = get_global("src");
    embi_js_object *o = embi_js_as_object(obj);
    embi_js_value b = embi_js_obj_get(g_rt, o, embi_js_key_c(g_rt, "b"), obj);
    embi_js_value gone = get_global("gone");
    embi_js_value notbuf = embi_js_obj_get(g_rt, o, embi_js_key_c(g_rt, "n"), obj);
    embi_sb sb = {nullptr, 0, 0};

    embi_js_value dup[2] = {b, b};
    EMB_CHECK(!embi_clone_serialize(g_r, obj, dup, 2, &sb));
    clear_exception();
    EMB_CHECK(!embi_clone_serialize(g_r, obj, &notbuf, 1, &sb));
    clear_exception();
    EMB_CHECK(!embi_clone_serialize(g_r, obj, &gone, 1, &sb));
    clear_exception();
    /* None of the refused transfers detached the buffer. */
    EMB_CHECK(js_true("globalThis.src.b.byteLength === 4"));
    EMB_CHECK_EQ_INT(sb.len, 0);
    embi_sb_free(&sb);
    teardown();
}

EMB_TEST(clone_deep_nesting_is_refused)
{
    EMB_REQUIRE(setup());
    (void)js_eval("globalThis.deep = (function () { var a = []; var cur = a; "
                  "for (var i = 0; i < 5000; i++) { var n = []; cur.push(n); cur = n; } "
                  "return a; })();"
                  "globalThis.shallow = (function () { var a = []; var cur = a; "
                  "for (var i = 0; i < 100; i++) { var n = []; cur.push(n); cur = n; } "
                  "return a; })();");
    embi_sb sb = {nullptr, 0, 0};
    EMB_CHECK(!embi_clone_serialize(g_r, get_global("deep"), nullptr, 0, &sb));
    clear_exception();
    embi_sb_free(&sb);
    sb = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(embi_clone_serialize(g_r, get_global("shallow"), nullptr, 0, &sb));
    embi_js_value copy = EMBI_JS_UNDEFINED;
    EMB_CHECK(embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &copy));
    set_global("c", copy);
    EMB_CHECK(js_true("Array.isArray(c) && c.length === 1 && Array.isArray(c[0])"));
    embi_sb_free(&sb);
    teardown();
}

/* Appends a u32 in the stream's little-endian order. */
static void sb_u32(embi_sb *sb, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        embi_sb_append_char(sb, (char)((v >> (8 * i)) & 0xFFu));
    }
}

/* Builds a stream of `levels` nested arrays, each holding one element at key "0", with an
 * undefined leaf inside the innermost one. The tag values are the clone.c tags: 0x15 is an array
 * (CL_ARRAY), 0x01 is undefined (CL_UNDEFINED). The header is "EMCL" and version 1. */
static void build_nested_stream(embi_sb *sb, uint32_t levels)
{
    embi_sb_append(sb, (embi_str){"EMCL\x01", 5});
    for (uint32_t i = 0; i < levels; i++) {
        embi_sb_append_char(sb, 0x15);
        sb_u32(sb, 1);           /* length */
        sb_u32(sb, 1);           /* one key */
        sb_u32(sb, 1);           /* key "0" */
        embi_sb_append(sb, (embi_str){"0\x00", 2});
    }
    embi_sb_append_char(sb, 0x01);
}

/* Nesting at the limit: EMBI_CLONE_MAX_DEPTH arrays with a primitive leaf must serialize and read
 * back, and one more level must be refused by the serializer. The reader is checked directly with
 * hand-built streams too, so the two sides agree on the boundary. */
static void nesting_limit_body(void)
{
    EMB_REQUIRE(setup());
    embi_js_value copy = EMBI_JS_UNDEFINED;

    /* Written by the serializer: 512 levels, leaf 7 at the bottom. */
    (void)js_eval("globalThis.nest = function (n) { var cur = [7]; "
                  "for (var i = 1; i < n; i++) { cur = [cur]; } return cur; };");
    (void)js_eval("globalThis.at_limit = nest(512);");
    embi_sb sb = {nullptr, 0, 0};
    EMB_CHECK(embi_clone_serialize(g_r, get_global("at_limit"), nullptr, 0, &sb));
    EMB_CHECK(embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &copy));
    set_global("c", copy);
    EMB_CHECK(js_true("(function () { var a = c; for (var i = 1; i < 512; i++) { a = a[0]; } "
                      "return Array.isArray(a) && a.length === 1 && a[0] === 7; })()"));
    embi_sb_free(&sb);

    (void)js_eval("globalThis.past_limit = nest(513);");
    sb = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!embi_clone_serialize(g_r, get_global("past_limit"), nullptr, 0, &sb));
    clear_exception();
    EMB_CHECK_EQ_INT(sb.len, 0);
    embi_sb_free(&sb);

    /* Hand-built streams: 512 levels read, 513 are refused. */
    sb = (embi_sb){nullptr, 0, 0};
    build_nested_stream(&sb, 512);
    EMB_CHECK(embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &copy));
    embi_sb_free(&sb);

    sb = (embi_sb){nullptr, 0, 0};
    build_nested_stream(&sb, 513);
    EMB_CHECK(!embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &copy));
    clear_exception();
    embi_sb_free(&sb);
    teardown();
}

/* The boundary test recurses EMBI_CLONE_MAX_DEPTH levels deep in both directions. Sanitizer frames
 * are large, so under ASan the 1 MB stack of the main thread overflows. The body therefore runs on
 * a worker thread with a 32 MB stack, from setup to teardown, so the engine's recorded stack top
 * (taken at the outermost embi_js_enter) belongs to that thread throughout. A failed thread start
 * falls back to a direct call. */
static void (*g_nesting_body)(void);

static int nesting_thread_main(void *unused)
{
    (void)unused;
    g_nesting_body();
    return 0;
}

static void run_with_big_stack(void (*body)(void))
{
    g_nesting_body = body;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           nesting_thread_main);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 32 * 1024 * 1024);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (t == nullptr) {
        body();
        return;
    }
    SDL_WaitThread(t, nullptr);
}

EMB_TEST(clone_nesting_limit_matches_on_both_sides)
{
    run_with_big_stack(nesting_limit_body);
}

/* Serializes a small structure that uses every tag family, for the truncation and fuzz tests. */
static bool serialize_sample(embi_sb *sb)
{
    (void)js_eval("globalThis.sample = {a: [1, 'two', 3.5, 4n, null, undefined, true], "
                  "d: new Date(5), r: /x/g, m: new Map([['k', 1]]), s: new Set([2]), "
                  "e: new TypeError('m', {cause: 'c'}), b: new Uint16Array(new ArrayBuffer(6), 2)};");
    return embi_clone_serialize(g_r, get_global("sample"), nullptr, 0, sb);
}

EMB_TEST(clone_malformed_bytes_fail_with_error)
{
    EMB_REQUIRE(setup());
    embi_js_value out = EMBI_JS_UNDEFINED;
    static const char wrong_magic[] = "XXXX\x01\x01";
    static const char wrong_version[] = "EMCL\x7f\x01";
    EMB_CHECK(!embi_clone_deserialize(g_r, (embi_str){wrong_magic, 6}, &out));
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(g_rt)));
    clear_exception();
    EMB_CHECK(!embi_clone_deserialize(g_r, (embi_str){wrong_version, 6}, &out));
    clear_exception();
    /* An unknown tag. */
    static const char bad_tag[] = "EMCL\x01\xee";
    EMB_CHECK(!embi_clone_deserialize(g_r, (embi_str){bad_tag, 6}, &out));
    clear_exception();
    teardown();
}

EMB_TEST(clone_every_truncation_fails_and_trailing_bytes_fail)
{
    EMB_REQUIRE(setup());
    embi_sb sb = {nullptr, 0, 0};
    EMB_REQUIRE(serialize_sample(&sb));
    embi_js_value out = EMBI_JS_UNDEFINED;
    EMB_CHECK(embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &out));
    clear_exception();
    for (uint32_t n = 0; n < sb.len; n++) {
        if (embi_clone_deserialize(g_r, (embi_str){sb.data, n}, &out)) {
            fprintf(stderr, "    a %u-byte prefix of %u bytes deserialized\n", n, sb.len);
            EMB_CHECK(false);
            break;
        }
        clear_exception();
    }
    /* One extra byte after a complete value is malformed. */
    embi_sb_append(&sb, (embi_str){"\x00", 1});
    EMB_CHECK(!embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &out));
    clear_exception();
    embi_sb_free(&sb);
    teardown();
}

EMB_TEST(clone_pseudo_random_bodies_do_not_crash)
{
    EMB_REQUIRE(setup());
    embi_sb sb = {nullptr, 0, 0};
    EMB_REQUIRE(serialize_sample(&sb));
    uint32_t seed = 0x2468ace1u;
    for (int iter = 0; iter < 400; iter++) {
        /* Start from the valid stream and flip some bytes, so the tags stay plausible. */
        char *buf = embi_alloc(sb.len);
        memcpy(buf, sb.data, sb.len);
        for (int flips = 0; flips < 3; flips++) {
            seed = seed * 1103515245u + 12345u;
            uint32_t pos = 5 + (seed >> 8) % (sb.len - 5);
            seed = seed * 1103515245u + 12345u;
            buf[pos] = (char)(seed >> 16);
        }
        embi_js_value out = EMBI_JS_UNDEFINED;
        if (!embi_clone_deserialize(g_r, (embi_str){buf, sb.len}, &out)) {
            clear_exception();
        }
        embi_free(buf, sb.len);
    }
    embi_sb_free(&sb);
    teardown();
}

LOADER_TEST_MAIN()
