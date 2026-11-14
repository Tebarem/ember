#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/heap.h"
#include "js/runtime.h"
#include "js/string.h"

/* FP1a: the compile-speed work (lookahead memo and peek reuse in the parser, the single-allocation
 * CODE tables, arena-backed compile vectors, the atom fast path, the collection hold). Each test
 * checks behaviour that a mistake in those would change. */

static bool eval_is(es6_env e, const char *src, const char *want)
{
    embi_js_value v = es6_run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        es6_note(src);
        es6_note_exc(e.rt);
        return false;
    }
    bool ok = es6_text_is(e.rt, v, want);
    if (!ok) {
        es6_note(src);
    }
    return ok;
}

static bool syntax_error(es6_env e, const char *src)
{
    embi_js_value v = es6_run(e, src);
    bool ok = v == EMBI_JS_EXCEPTION && es6_exc_name_is(e.rt, "SyntaxError");
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
    } else {
        es6_note(src);
    }
    return ok;
}

EMB_TEST(arrow_lookahead_tells_arrows_from_parenthesised_expressions)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(eval_is(e, "((a, b) => a + b)(1, 2)", "3"));
    EMB_CHECK(eval_is(e, "(() => 7)()", "7"));
    EMB_CHECK(eval_is(e, "((a,) => a)(5)", "5"));
    EMB_CHECK(eval_is(e, "((...r) => r.length)(1, 2, 3)", "3"));
    EMB_CHECK(eval_is(e, "(({a, b}) => a * b)({a: 3, b: 4})", "12"));
    EMB_CHECK(eval_is(e, "(([a, b]) => a - b)([9, 4])", "5"));
    EMB_CHECK(eval_is(e, "((a = (b) => b + 1, c = a(1)) => c)()", "2"));
    EMB_CHECK(eval_is(e, "((a = function () { return 4; }) => a())()", "4"));
    EMB_CHECK(eval_is(e, "(function () { return 8; })()", "8"));
    EMB_CHECK(eval_is(e, "(1, 2)", "2"));
    EMB_CHECK(eval_is(e, "var f = 3, g = 4; (f, g)", "4"));
    EMB_CHECK(eval_is(e, "((a) => (b) => (c) => a + b + c)(1)(2)(3)", "6"));
    EMB_CHECK(eval_is(e, "(async (x) => x)(1) instanceof Promise", "true"));
    EMB_CHECK(eval_is(e, "var async = (x) => x * 2; async(4)", "8"));
    EMB_CHECK(eval_is(e, "var o = { m(a) { return ((b) => a + b)(1); } }; o.m(2)", "3"));
    /* nested parentheses reached again by the parser after the outer scan memoized them */
    EMB_CHECK(eval_is(e, "(((((1))))) + ((2, 3)) + (((x) => x)(4))", "8"));
    EMB_CHECK(eval_is(e, "((a, b) => ((c, d) => a + b + c + d))(1, 2)(3, 4)", "10"));
    /* a line break before the arrow is not an arrow */
    EMB_CHECK(syntax_error(e, "(a, b)\n=> a"));
    /* not parameter lists */
    EMB_CHECK(syntax_error(e, "(a + b) => 1"));
    EMB_CHECK(syntax_error(e, "(1) => 1"));
    EMB_CHECK(syntax_error(e, "(a, 1) => 1"));
    EMB_CHECK(syntax_error(e, "(a b) => 1"));
    EMB_CHECK(syntax_error(e, "(new) => 1"));
    EMB_CHECK(syntax_error(e, "((a)) => 1"));
    EMB_CHECK(syntax_error(e, "(a.b) => 1"));
    EMB_CHECK(syntax_error(e, "(function () {}) => 1"));
    EMB_CHECK(syntax_error(e, "(...a, b) => 1"));
    es6_env_free(e);
}

EMB_TEST(lookahead_inside_templates_regexps_and_strings)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(eval_is(e, "((a = `x${(1)}y`) => a)()", "x1y"));
    EMB_CHECK(eval_is(e, "((a = /\\)/.source) => a)()", "\\)"));
    EMB_CHECK(eval_is(e, "((a = \")\") => a)()", ")"));
    EMB_CHECK(eval_is(e, "((a = { b: (1, 2) }) => a.b)()", "2"));
    EMB_CHECK(eval_is(e, "((a = [(1), (2)]) => a.length)()", "2"));
    EMB_CHECK(eval_is(e, "var x = 6; (x / 2) + (x / 3)", "5"));
    EMB_CHECK(eval_is(e, "var y = 8; (y) / 2 / (2)", "2"));
    es6_env_free(e);
}

EMB_TEST(blocks_hoist_function_declarations_after_the_scan_shortcut)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(eval_is(e, "var r = []; { r.push(typeof f); function f() {} } r[0]", "function"));
    EMB_CHECK(eval_is(e, "var s = 0; for (var i = 0; i < 3; i++) { s += i; } s", "3"));
    EMB_CHECK(eval_is(e, "var t = ''; { { t += typeof g; } function g() {} } t", "function"));
    EMB_CHECK(eval_is(e, "var u = 0; { let a = 1; { let b = 2; u = a + b; } } u", "3"));
    es6_env_free(e);
}

EMB_TEST(an_identifier_followed_by_a_token_is_taken_from_the_peek)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(eval_is(e, "var a = 1, b = 2; a\n+ b", "3"));
    EMB_CHECK(eval_is(e, "var k = x => x + 1; k(1)", "2"));
    EMB_CHECK(eval_is(e, "var m = async x => x; typeof m", "function"));
    EMB_CHECK(eval_is(e, "var n = 4; n /= 2; n", "2"));
    EMB_CHECK(eval_is(e, "var q = 'a'; q\n'b'", "b"));
    EMB_CHECK(eval_is(e, "var p = { get a() { return 1; }, set a(v) {}, async b() {}, *c() {} }; p.a", "1"));
    EMB_CHECK(eval_is(e, "var w = 3; w\n/ 3 / 1", "1"));
    EMB_CHECK(eval_is(e, "var z = 2; z\n++\nz", "3"));
    EMB_CHECK(syntax_error(e, "var v = a b"));
    es6_env_free(e);
}

EMB_TEST(error_positions_survive_the_single_block_tables)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A runtime error at a known line and column comes through the delta-coded position table. */
    embi_js_value v = es6_run(e, "function f() {\n  var o = null;\n  return o.x;\n}\nf();");
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    const char *file = "";
    uint32_t line = 0;
    uint32_t col = 0;
    EMB_CHECK(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 3);
    EMB_CHECK_EQ_INT(strcmp(file, "test.js"), 0);
    embi_js_exception_clear(e.rt);
    /* A syntax error reports its position too. */
    v = es6_run(e, "var a = 1;\nvar b = ;\n");
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 2);
    embi_js_exception_clear(e.rt);
    es6_env_free(e);
}

EMB_TEST(functions_with_every_table_keep_them)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* exception table (try), upvalues, a sloppy direct eval and a with (dynamic scope tables),
     * mapped arguments, a rest parameter, inline caches and a long constant list. */
    const char *src =
        "var out = [];\n"
        "function mk(a, b) {\n"
        "  var c = 10;\n"
        "  function inner() { return a + b + c + arguments.length; }\n"
        "  try { out.push(inner(1, 2, 3)); } catch (x) { out.push('no'); } finally { out.push('f'); }\n"
        "  out.push(eval('c + a'));\n"
        "  with ({ w: 5 }) { out.push(w + c); }\n"
        "  arguments[0] = 100; out.push(a);\n"
        "  return (...r) => r.length + c;\n"
        "}\n"
        "var g = mk(1, 2);\n"
        "out.push(g(1, 2, 3));\n"
        "out.join(',')";
    EMB_CHECK(eval_is(e, src, "16,f,11,15,100,13"));
    es6_env_free(e);
}

EMB_TEST(source_text_of_functions_stays_available)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(eval_is(e, "function  f ( a ) { return a; }\nf.toString()", "function  f ( a ) { return a; }"));
    EMB_CHECK(eval_is(e, "var o = { m( x ) { return 1 } }; o.m.toString()", "m( x ) { return 1 }"));
    EMB_CHECK(eval_is(e, "(( a ) =>  a).toString()", "( a ) =>  a"));
    es6_env_free(e);
}

EMB_TEST(compile_does_not_collect_and_keeps_the_budget)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_gc(e.rt);
    embi_js_mem_stats a;
    embi_js_get_mem_stats(e.rt, &a);
    /* A script with a few thousand small functions allocates well past the collection floor. */
    embi_sb sb = {nullptr, 0, 0};
    for (int i = 0; i < 3000; i++) {
        char line[96];
        SDL_snprintf(line, sizeof line, "function fn%d(a, b) { return a + b * %d; }\n", i, i);
        embi_sb_append(&sb, (embi_str){line, (uint32_t)strlen(line)});
    }
    const char *text = embi_sb_cstr(&sb);
    embi_js_value code = embi_js_compile(e.r, (const uint8_t *)text, (uint32_t)strlen(text), "big.js", false);
    EMB_CHECK(code != EMBI_JS_EXCEPTION);
    embi_js_mem_stats b;
    embi_js_get_mem_stats(e.rt, &b);
    EMB_CHECK_EQ_INT(b.gc_count, a.gc_count);
    /* The budget is deferred, not forgiven: what the compile allocated stays counted, so the
     * first allocation after the hold collects. */
    EMB_CHECK(b.heap_bytes_since_gc > 256u * 1024u);
    embi_js_mem_stats c;
    embi_js_value v = es6_run(e, "1 + 1");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    embi_js_get_mem_stats(e.rt, &c);
    EMB_CHECK_EQ_INT(c.gc_count, b.gc_count + 1u);
    embi_sb_free(&sb);
    es6_env_free(e);
}

/* FP2a: what a module compile allocated can be forgiven by its caller (module code is kept for the runtime's life), and
 * only that: the mark is a floor, a collection that reset the counter in between leaves it alone. */
EMB_TEST(forgiving_an_allocation_lowers_the_count_to_the_mark_and_no_further)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_gc(e.rt);
    size_t mark = embi_js_gc_alloc_mark(e.rt);
    embi_js_mem_stats a;
    embi_js_get_mem_stats(e.rt, &a);
    EMB_CHECK_EQ_INT(a.heap_bytes_since_gc, mark);
    embi_js_value v = es6_run(e, "var big = []; for (var i = 0; i < 400; i++) big.push({a: i, b: 'x' + i}); big.length");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    embi_js_get_mem_stats(e.rt, &a);
    EMB_CHECK(a.heap_bytes_since_gc > mark + 8u * 1024u);
    embi_js_gc_forgive_since(e.rt, mark);
    embi_js_get_mem_stats(e.rt, &a);
    EMB_CHECK_EQ_INT(a.heap_bytes_since_gc, mark);
    /* A mark above the count (a collection reset it) changes nothing. */
    embi_js_gc(e.rt);
    embi_js_get_mem_stats(e.rt, &a);
    EMB_CHECK_EQ_INT(a.heap_bytes_since_gc, 0);
    embi_js_gc_forgive_since(e.rt, 100000);
    embi_js_get_mem_stats(e.rt, &a);
    EMB_CHECK_EQ_INT(a.heap_bytes_since_gc, 0);
    es6_env_free(e);
}

/* Compiling in a loop (eval, new Function) must not outgrow the collector: the garbage code the
 * loop leaves behind has to count toward the next collection. */
static void check_bounded_loop(const char *loop_1k, const char *loop_99k)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, loop_1k) != EMBI_JS_EXCEPTION);
    embi_js_mem_stats a;
    embi_js_get_mem_stats(e.rt, &a);
    size_t c_heap_a = embi_mem_live();
    EMB_CHECK(es6_run(e, loop_99k) != EMBI_JS_EXCEPTION);
    embi_js_mem_stats b;
    embi_js_get_mem_stats(e.rt, &b);
    size_t c_heap_b = embi_mem_live();
    EMB_CHECK(b.gc_count > a.gc_count);
    EMB_CHECK(b.heap_live_bytes < a.heap_live_bytes + 1024u * 1024u);
    EMB_CHECK(c_heap_b < c_heap_a + 2u * 1024u * 1024u);
    es6_env_free(e);
}

EMB_TEST(eval_in_a_loop_keeps_the_heap_bounded)
{
    check_bounded_loop("for (var i = 0; i < 1000; i++) eval('({a:1})'); 0",
                       "for (var j = 0; j < 99000; j++) eval('({a:1})'); 0");
}

EMB_TEST(new_function_in_a_loop_keeps_the_heap_bounded)
{
    check_bounded_loop("for (var i = 0; i < 1000; i++) new Function('return 1'); 0",
                       "for (var j = 0; j < 99000; j++) new Function('return 1'); 0");
}

EMB_TEST(unbalanced_hold_end_clamps_at_zero)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
#ifdef NDEBUG
    embi_js_gc_hold_end(e.rt);
    embi_js_gc_hold_end(e.rt);
    EMB_CHECK_EQ_INT(e.rt->heap.gc_hold, 0);
    /* a later hold still nests normally */
    embi_js_gc_hold_begin(e.rt);
    EMB_CHECK_EQ_INT(e.rt->heap.gc_hold, 1);
    embi_js_gc_hold_end(e.rt);
    EMB_CHECK_EQ_INT(e.rt->heap.gc_hold, 0);
#else
    embi_js_gc_hold_begin(e.rt);
    embi_js_gc_hold_begin(e.rt);
    embi_js_gc_hold_end(e.rt);
    EMB_CHECK_EQ_INT(e.rt->heap.gc_hold, 1);
    embi_js_gc_hold_end(e.rt);
    EMB_CHECK_EQ_INT(e.rt->heap.gc_hold, 0);
#endif
    es6_env_free(e);
}

EMB_TEST(compile_is_safe_under_collection_stress)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_gc_set_stress(e.rt, 1);
    EMB_CHECK(eval_is(e, "var s = 0; function add(a) { s += a; return add; } add(1)(2)(3); s", "6"));
    EMB_CHECK(eval_is(e, "[1, 2, 3].map((x) => x * x).join('-')", "1-4-9"));
    embi_js_gc_set_stress(e.rt, 0);
    es6_env_free(e);
}

EMB_TEST(atom_lookup_by_text_matches_interning_a_string)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    const char *names[] = {"a", "name", "constructor", "x_1$", "", "caf\xC3\xA9", "\xF0\x9F\x98\x80z",
                           "a-longer-property-name-with-dashes"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n = strlen(names[i]);
        embi_js_string *first = embi_js_atom_intern_utf8(rt, names[i], n);
        embi_js_string *again = embi_js_atom_intern_utf8(rt, names[i], n);
        EMB_CHECK(first == again);
        embi_js_string *viastr = embi_js_atom_intern(rt, embi_js_str_new_utf8(rt, names[i], n));
        EMB_CHECK(first == viastr);
        EMB_CHECK(embi_js_str_is_atom(first));
    }
    /* A prefix of an interned name is a different atom. */
    embi_js_string *whole = embi_js_atom_intern_utf8(rt, "propertyName", 12);
    embi_js_string *prefix = embi_js_atom_intern_utf8(rt, "property", 8);
    EMB_CHECK(whole != prefix);
    EMB_CHECK_EQ_INT(embi_js_str_len(prefix), 8);
    embi_js_runtime_free(rt);
}

EMB_TEST(code_cells_free_their_block)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    size_t before = embi_mem_live();
    for (int i = 0; i < 20; i++) {
        embi_js_value v = es6_run(e, "(function () { var a = 1; return function () { return a; }; })()()");
        EMB_CHECK(v != EMBI_JS_EXCEPTION);
        embi_js_gc(e.rt);
    }
    size_t after = embi_mem_live();
    /* Twenty rounds of compile and collect do not grow the C heap by more than the atoms they add. */
    EMB_CHECK(after < before + 64u * 1024u);
    es6_env_free(e);
}

EMB_TEST_MAIN()
