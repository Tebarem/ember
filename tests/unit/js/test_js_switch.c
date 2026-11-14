/* CF1: SWITCH_JMP, the entry jump of a switch statement, runs its chain of constant case tests (DUP, PUSH_CONST or
 * PUSH_I32, SEQ, JMP_TRUE, JMP) in one step and goes on at the first body that matches or at the first test of another
 * form. Every case here checks what the statement gives against the language: strict equality (numbers, -0, NaN,
 * strings made at run time, objects), the order of the tests (a test with side effects stops the chain, the constant
 * tests after it run in their turn), the default clause anywhere, fall through, lexical declarations in the cases,
 * nested and empty switches, and a switch inside a generator. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"

#include <string.h>

static const char k_fn[] =
    "function f(x) {"
    "  switch (x) {"
    "    case 'a': return 1;"
    "    case 'bb': return 2;"
    "    case 3: return 3;"
    "    case 'c' + 'd': return 4;"
    "    default: return 5;"
    "    case 'e': return 6;"
    "  }"
    "}";

EMB_TEST(constant_cases_match_by_strict_equality)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    char src[1024];
    SDL_snprintf(src, sizeof src,
                 "%s [f('a'), f('bb'), f(3), f(3.0), f('cd'), f('e'), f('zz'), f(-0), f(NaN), f(['a'].join('')),"
                 " f('b' + 'b'), f('3'), f(new String('a')), f(undefined), f(1.5 + 1.5)].join()",
                 k_fn);
    E6_TEXT(e, src, "1,2,3,3,4,6,5,5,5,1,2,5,5,5,3");
    /* numbers: -0 equals 0, NaN equals nothing, a double equals the int32 case */
    E6_TEXT(e,
            "(function (x) { switch (x) { case 0: return 'zero'; case NaN: return 'nan'; case 2: return 'two';"
            " default: return 'd'; } })(-0) + (function (x) { switch (x) { case NaN: return 'nan';"
            " default: return 'd'; } })(NaN) + (function (x) { switch (x) { case 2: return 'two'; } return 'none'; })(2.0)",
            "zerodtwo");
    /* objects are equal only to themselves; null and undefined are distinct */
    E6_TEXT(e,
            "(function () { const o = {}; const g = (x) => { switch (x) { case null: return 'n'; case undefined:"
            " return 'u'; case o: return 'o'; case true: return 't'; default: return 'd'; } };"
            " return g(null) + g(undefined) + g(o) + g({}) + g(true) + g(1); })()",
            "nuodtd");
    es6_env_free(e);
}

EMB_TEST(tests_run_in_order_and_stop_at_side_effects)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* the call in the second test runs only when the first constant test fails, and before the third */
    E6_TEXT(e,
            "(function () { let log = ''; const t = (v) => { log += v; return v; };"
            " const g = (x) => { log += '|'; switch (x) { case 1: return 'a'; case t(2): return 'b'; case 3:"
            " return 'c'; case t(4): return 'd'; default: return 'e'; } };"
            " return [g(1), g(2), g(3), g(4), g(5), log].join(); })()",
            "a,b,c,d,e,||2|2|24|24");
    /* a constant case after a matching non-constant one is not reached */
    E6_NUM(e,
           "(function () { let n = 0; const one = () => { n++; return 1; };"
           " switch (1) { case one(): n += 10; break; case 1: n += 100; break; } return n; })()",
           11);
    es6_env_free(e);
}

EMB_TEST(default_fall_through_and_declarations)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e,
            "(function () { const g = (x) => { let r = ''; switch (x) { case 0: r += 'z'; case 1: r += 'o'; break;"
            " case 2: { let q = 2; r += q; } default: r += 'd'; } return r; };"
            " return [g(0), g(1), g(2), g(5), g(-0), g('0')].join(); })()",
            "zo,o,2d,d,zo,d");
    /* default first, then constant cases: a match skips the default */
    E6_TEXT(e,
            "(function () { const g = (x) => { switch (x) { default: return 'd'; case 1: return 'one'; case 2:"
            " return 'two'; } }; return g(1) + g(2) + g(3); })()",
            "onetwod");
    /* empty switch, default only, nested switches */
    E6_TEXT(e,
            "(function () { const h = (x) => { switch (x) { } return 'h'; }; const k = (x) => { switch (x)"
            " { default: return 'k'; } }; const n = (a, b) => { switch (a) { case 1: switch (b) { case 'x':"
            " return 'ax'; case 'y': return 'ay'; } return 'a?'; case 2: return 'b'; } return '?'; };"
            " return h(1) + k(2) + n(1, 'x') + n(1, 'y') + n(1, 'z') + n(2) + n(3); })()",
            "hkaxaya?b?");
    /* the cases share one scope: a declaration in a later case is in its dead zone in an earlier one */
    E6_THROWS(e, "(function (x) { switch (x) { case 0: v = 1; break; case 1: let v = 2; } })(0)", "ReferenceError");
    E6_TEXT(e,
            "(function (x) { switch (x) { case 0: let v = 'zero'; return v; case 1: return 'one'; } })(0)",
            "zero");
    es6_env_free(e);
}

EMB_TEST(loops_generators_and_many_cases)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e,
           "(function () { let t = 0; for (let i = 0; i < 10; i++) { switch (i % 4) { case 0: t += 1; break;"
           " case 1: t += 10; break; case 2: t += 100; break; default: t += 1000; } } return t; })()",
           2233);
    /* continue and break of an enclosing loop from inside a case */
    E6_TEXT(e,
            "(function () { let r = ''; for (const c of 'abcxd') { switch (c) { case 'a': continue; case 'x':"
            " break; default: r += c; continue; } break; } return r; })()",
            "bc");
    E6_TEXT(e,
            "(function () { function* g(x) { switch (x) { case 'a': yield 1; case 'b': yield 2; break;"
            " default: yield 3; } } return [...g('a'), ...g('b'), ...g('c')].join(); })()",
            "1,2,2,3");
    /* a long chain of string cases (the shape of a reducer) */
    embi_sb src = {nullptr, 0, 0};
    const char *head = "(function () { const g = (x) => { switch (x) {";
    embi_sb_append(&src, (embi_str){head, (uint32_t)strlen(head)});
    for (int i = 0; i < 40; i++) {
        char line[64];
        SDL_snprintf(line, sizeof line, " case 'k%d': return %d;", i, i);
        embi_sb_append(&src, (embi_str){line, (uint32_t)strlen(line)});
    }
    const char *tail = " default: return -1; } }; let s = 0; for (let i = 0; i < 41; i++) s += g('k' + i); return s; })()";
    embi_sb_append(&src, (embi_str){tail, (uint32_t)strlen(tail)});
    E6_NUM(e, embi_sb_cstr(&src), 40 * 39 / 2 - 1);
    embi_sb_free(&src);
    es6_env_free(e);
}

/* The entry jump of a switch is SWITCH_JMP in the disassembly. */
EMB_TEST(switch_compiles_to_switch_jmp)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "(function (x) { switch (x) { case 1: return 'a'; default: return 'b'; } })";
    embi_js_value code = embi_js_compile(e.r, (const uint8_t *)src, (uint32_t)strlen(src), "sw.js", false);
    EMB_REQUIRE(code != EMBI_JS_EXCEPTION);
    const embi_js_code *c = (const embi_js_code *)embi_js_as_cell(code);
    embi_sb sb = {nullptr, 0, 0};
    for (uint32_t i = 0; i < c->nconsts; i++) {
        embi_js_value v = c->consts[i];
        if (embi_js_is_cell(v) && ((const embi_js_cell *)embi_js_as_cell(v))->type == EMBI_JS_CELL_CODE) {
            embi_js_disasm((const embi_js_code *)embi_js_as_cell(v), &sb);
        }
    }
    const char *text = embi_sb_cstr(&sb);
    EMB_REQUIRE(text != nullptr);
    EMB_CHECK(strstr(text, "SWITCH_JMP") != nullptr);
    embi_sb_free(&sb);
    es6_env_free(e);
}

EMB_TEST_MAIN()
