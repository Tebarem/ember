/* Superinstructions (J5i). Each fused op performs the work of its two
 * parts, so every case runs a pattern that the peephole fuses inside a function (its locals are
 * GET_LOC/SET_LOC operands) and checks the result against what the unfused pair gives: the int32,
 * double, string, BigInt and object slow paths, the exceptions of the field and store parts, the
 * side effects of conversions, and the jumps. The disassembly checks that each kept op really
 * fuses, and that a pair split by a jump target or a protected-range boundary doesn't. The mask
 * checks run the same programs with the peephole off (embi_jsc_fuse_mask = 0) and compare results,
 * stacks and error positions, which is what the pc advance in each handler relies on. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* The runtime copy of EMBI_JS_FUSE_MASK (compiler.c). 0 compiles the unfused code. */
extern uint32_t embi_jsc_fuse_mask;

/* Disassembles the CODE cells of the functions a compiled script creates (its constants) into sb. */
static void disasm_functions(embi_js_value top, embi_sb *sb)
{
    const embi_js_code *c = (const embi_js_code *)embi_js_as_cell(top);
    for (uint32_t i = 0; i < c->nconsts; i++) {
        embi_js_value v = c->consts[i];
        if (embi_js_is_cell(v) &&
            ((const embi_js_cell *)embi_js_as_cell(v))->type == EMBI_JS_CELL_CODE) {
            embi_js_disasm((const embi_js_code *)embi_js_as_cell(v), sb);
        }
    }
}

/* True when text has op name as a mnemonic: the name follows the two-space column gap of a
 * disassembly line and is followed by a space or the end of the line. A fused op prints its parts
 * in brackets, which doesn't match, and GET_LOC doesn't match GET_LOC_FIELD. */
static bool has_mnemonic(const char *text, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = text; (p = strstr(p, name)) != nullptr; p += n) {
        bool lead = p - text >= 2 && p[-1] == ' ' && p[-2] == ' ';
        bool tail = p[n] == ' ' || p[n] == '\n' || p[n] == '\0';
        if (lead && tail) {
            return true;
        }
    }
    return false;
}

/* Disassembles the functions of src, compiled with the current fuse mask, into sb. */
static bool disasm_src(es6_env e, const char *src, embi_sb *sb)
{
    embi_js_value code = embi_js_compile(e.r, (const uint8_t *)src, (uint32_t)strlen(src),
                                         "fused.js", false);
    if (code == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
        return false;
    }
    disasm_functions(code, sb);
    return true;
}

/* True when the disassembly of the functions in src has the mnemonic a, and b when given. */
static bool fuses(es6_env e, const char *src, const char *a, const char *b)
{
    embi_sb sb = {nullptr, 0, 0};
    bool ok = disasm_src(e, src, &sb);
    const char *text = embi_sb_cstr(&sb);
    ok = ok && text && has_mnemonic(text, a) && (!b || has_mnemonic(text, b));
    if (!ok) {
        SDL_Log("    no %s / %s in:\n%s", a, b ? b : "-", text ? text : "");
    }
    embi_sb_free(&sb);
    return ok;
}

/* True when the functions of src have no mnemonic a. */
static bool lacks(es6_env e, const char *src, const char *a)
{
    embi_sb sb = {nullptr, 0, 0};
    bool ok = disasm_src(e, src, &sb);
    const char *text = embi_sb_cstr(&sb);
    ok = ok && text && !has_mnemonic(text, a);
    if (!ok) {
        SDL_Log("    unexpected %s in:\n%s", a, text ? text : "");
    }
    embi_sb_free(&sb);
    return ok;
}

/* Appends to out the result of src under the fuse mask: "V " and the value's string, or "E " and
 * the stack of the exception it throws. The mask is restored before returning. */
static void result_text(es6_env e, const char *src, uint32_t mask, embi_sb *out)
{
    uint32_t saved = embi_jsc_fuse_mask;
    embi_jsc_fuse_mask = mask;
    embi_js_value v = es6_run(e, src);
    embi_jsc_fuse_mask = saved;
    embi_js_string *s = nullptr;
    if (v == EMBI_JS_EXCEPTION) {
        embi_sb_append(out, (embi_str){"E ", 2});
        embi_js_value ex = embi_js_exception_get(e.rt);
        embi_js_value st = EMBI_JS_UNDEFINED;
        if (embi_js_is_object(ex)) {
            st = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "stack"));
        }
        s = embi_js_to_string(e.rt, st);
        embi_js_exception_clear(e.rt);
    } else {
        embi_sb_append(out, (embi_str){"V ", 2});
        s = embi_js_to_string(e.rt, v);
    }
    if (s) {
        embi_js_str_append_utf8(s, out);
    }
}

/* Runs src with the default fuse mask and with it off. The two texts must be equal, and must
 * contain want when it isn't nullptr (the line of a throw, for instance). */
static bool same_unfused(es6_env e, const char *src, const char *want)
{
    uint32_t on = embi_jsc_fuse_mask;
    embi_sb a_sb = {nullptr, 0, 0};
    embi_sb b_sb = {nullptr, 0, 0};
    result_text(e, src, on, &a_sb);
    result_text(e, src, 0, &b_sb);
    const char *a = embi_sb_cstr(&a_sb);
    const char *b = embi_sb_cstr(&b_sb);
    bool ok = a && b && strcmp(a, b) == 0 && (!want || strstr(a, want) != nullptr);
    if (!ok) {
        SDL_Log("    fused: %s\n    unfused: %s", a ? a : "", b ? b : "");
    }
    embi_sb_free(&a_sb);
    embi_sb_free(&b_sb);
    return ok;
}

/* Each kept op fuses where its source pattern says it does. */
EMB_TEST(js_fused_disasm_shows_each_kept_op)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(fuses(e, "(function (a) { return a + 5; })", "GET_LOC_I32", nullptr));
    EMB_CHECK(fuses(e, "(function (a) { return a < 5 ? 1 : 2; })", "GET_LOC_I32", nullptr));
    EMB_CHECK(fuses(e, "(function (a) { ++a; return a; })", "INC_SET_LOC", nullptr));
    /* The pairs cut by the keep rule stay unfused: a field read, a store pop and a compare. */
    EMB_CHECK(lacks(e, "(function (o) { return o.x; })", "GET_LOC_FIELD"));
    EMB_CHECK(lacks(e, "(function (o) { o.x = 1; return o; })", "SET_FIELD_POP"));
    EMB_CHECK(lacks(e, "(function (a) { return a + 1 < 5; })", "PUSH_I32_LT"));
    es6_env_free(e);
}

/* A pair is fused when no jump lands on its second part, and the positive twins show the kept
 * ops still fuse inside loops, where their first part is the target of the back jump. */
EMB_TEST(js_fused_loop_heads_keep_pairs)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* The loop head is the first part of GET_LOC_I32: the pair still fuses. */
    EMB_CHECK(fuses(e, "(function (a) { var s = 0; while (s < 3) { s = s + 1; } return a; })",
                    "GET_LOC_I32", nullptr));
    /* Each side of a conditional stores into the same slot: the store pair after it fuses. */
    EMB_CHECK(fuses(e, "(function (a) { var s = 0; a ? (s = 1) : (s = 2); ++s; return s; })",
                    "INC_SET_LOC", nullptr));
    es6_env_free(e);
}

/* A jump target must not land inside a fused sequence. In (a || b) + 5 the join point of the ||
 * is the PUSH_I32 right after GET_LOC b, so that pair must stay unfused; the same shape with no
 * jump into it still fuses. INC_SET_LOC has no source shape with a jump landing on its second
 * part (its parts are emitted together by one ++/-- on a local), so only GET_LOC_I32 is probed. */
EMB_TEST(js_fused_jump_target_blocks_pair)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(lacks(e, "(function (a, b) { return (a || b) + 5; })", "GET_LOC_I32"));
    EMB_CHECK(lacks(e, "(function (a, b) { return (a && b) < 5; })", "GET_LOC_I32"));
    EMB_CHECK(fuses(e, "(function (a, b) { return b + 5; })", "GET_LOC_I32", nullptr));
    EMB_CHECK(fuses(e, "(function (a, b) { var t = a || b; return t + 5; })", "GET_LOC_I32",
                    nullptr));
    /* Both branches of the || (and of the &&) agree with the unfused code. */
    EMB_CHECK(same_unfused(e, "(function (a, b) { return (a || b) + 5; })(1, 2)", "V 6"));
    EMB_CHECK(same_unfused(e, "(function (a, b) { return (a || b) + 5; })(0, 2)", "V 7"));
    EMB_CHECK(same_unfused(e, "(function (a, b) { return (a && b) < 5; })(1, 2)", "V true"));
    EMB_CHECK(same_unfused(e, "(function (a, b) { return (a && b) < 5; })(0, 9)", "V true"));
    EMB_CHECK(same_unfused(e, "(function (a, b) { return (a && b) < 5; })(1, 9)", "V false"));
    es6_env_free(e);
}

/* Results with the fuse mask on and off agree, for the same programs as the fast-path tests. */
EMB_TEST(js_fused_mask_off_matches_on)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    static const char *const progs[] = {
        "(function (o) { return o.x; })({x: 3})",
        "(function (o) { var t = 5; return o.x + t; })({get x() { return 7; }})",
        "(function (a, b) { return a + b; })('a', 'b')",
        "(function (a) { return a + 5; })(2147483647)",
        "(function (a) { a = a + 1; a = a * 2; return a; })(3)",
        "(function (a) { var b = a++; return b * 10 + a; })(5)",
        "(function (a) { ++a; return a; })('5')",
        "(function (a, b) { if (a < b) return 1; return 2; })('10', '9')",
        "(function (a) { return a + 1 < 5; })(2)",
        "(function (v) { return v === 'abc'; })('ab' + 'c')",
        "(function () { var s = 0; for (var i = 0; i < 10; i++) { if (i % 2) continue; s += i; }"
        " return s; })()",
        "var log = 0; var P = {set x(v) { log = v; }}; (function (o) { o.x = 7; })(Object.create(P));"
        " log",
    };
    for (uint32_t i = 0; i < sizeof progs / sizeof progs[0]; i++) {
        EMB_CHECK(same_unfused(e, progs[i], nullptr));
    }
    es6_env_free(e);
}

/* A throw from the second part of each fused op reports the same stack, with the same positions,
 * as the unfused pair, and the position is the line of the throwing expression. */
EMB_TEST(js_fused_throw_positions_match_unfused)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* GET_LOC_I32 feeds an ADD that throws on a Symbol, on line 3: the pc of the second part
     * is the ADD's own, so the position is the line of the expression. */
    EMB_CHECK(same_unfused(e,
                           "var f = function (a) {\n  var t = a;\n  return t + 5;\n};\n"
                           "var r = ''; try { f(Symbol()); } catch (err) { r = err.stack; } r;",
                           "test.js:3:"));
    /* INC_SET_LOC: ++ on a Symbol, on line 3. */
    EMB_CHECK(same_unfused(e,
                           "var i = function (a) {\n  var t = a;\n  ++t;\n  return t;\n};\n"
                           "var r = ''; try { i(Symbol()); } catch (err) { r = err.stack; } r;",
                           "test.js:3:"));
    /* INC_SET_LOC on a value that needs ToNumeric: a throwing valueOf, on line 3. */
    EMB_CHECK(same_unfused(e,
                           "var k = function (a) {\n  var t = a;\n  ++t;\n  return t;\n};\n"
                           "var r = ''; try { k({valueOf: function () { null.z; }}); }"
                           " catch (err) { r = err.stack; } r;",
                           "test.js:3:"));
    es6_env_free(e);
}

/* Field reads and method calls on locals. No field op is fused (cut by the keep rule); these are
 * semantic checks of the unfused fast paths over local operands. */
EMB_TEST(js_local_field_reads_semantics)
{
    size_t before = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "(function (o) { return o.x; })({x: 3})", 3);
    E6_NUM(e, "(function (o) { var t = 5; return o.x + t; })({get x() { return 7; }})", 12);
    E6_NUM(e, "(function (o) { return o.x; })(Object.create({x: 4}))", 4);
    E6_NUM(e, "(function (s) { return s.length; })('abc')", 3);
    E6_BOOL(e, "(function (n) { return n.toFixed === Number.prototype.toFixed; })(5)", true);
    E6_THROWS(e, "(function (o) { return o.x; })(undefined)", "TypeError");
    E6_THROWS(e, "(function (o) { return o.x; })(null)", "TypeError");
    E6_NUM(e, "(function (o) { try { return o.x; } catch (err) { return -1; } })(undefined)", -1);
    /* The method form: a call on a local receiver keeps the function and the receiver. */
    E6_NUM(e, "(function (o) { return o.m(); })({v: 9, m: function () { return this.v; }})", 9);
    E6_TEXT(e, "(function (s) { return s.toUpperCase(); })('ab')", "AB");
    E6_THROWS(e, "(function (o) { return o.m(); })(null)", "TypeError");
    /* A cache filled by reads sees the change made to the prototype afterwards. */
    E6_BOOL(e,
            "var P = {x: 1}; var O = Object.create(P); function g(o) { return o.x; } var s = 0;"
            "for (var i = 0; i < 50; i++) s += g(O);"
            "Object.defineProperty(P, 'x', {get: function () { return 2; }}); s + g(O) === 52",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(js_fused_locals_and_int32)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "(function (a, b) { return a + b; })(1, 2)", 3);
    E6_TEXT(e, "(function (a, b) { return a + b; })('a', 'b')", "ab");
    E6_NUM(e, "(function (a, b) { return a - b; })(10, 0.5)", 9.5);
    E6_NUM(e, "(function (a) { return a + 5; })(10)", 15);
    E6_NUM(e, "(function (a) { return a + 5; })(1.5)", 6.5);
    E6_TEXT(e, "(function (a) { return a + 5; })('x')", "x5");
    E6_NUM(e, "(function (a) { return a + 5; })(2147483647)", 2147483652.0);
    E6_NUM(e, "(function (a) { return a - 5; })(-2147483648)", -2147483653.0);
    E6_NUM(e, "(function (a) { return a * 2; })(-3)", -6);
    E6_NUM(e, "(function (a) { return a + 1; })(true)", 2);
    E6_NUM(e, "(function (a) { return a < 5 ? 1 : 2; })(4)", 1);
    es6_env_free(e);
}

/* Stores, pops and ++/-- on locals. Only INC_SET_LOC is fused here; the store and pop pairs were
 * cut, so these are semantic checks of the unfused paths. */
EMB_TEST(js_local_stores_pops_incdec_semantics)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "(function () { var a; a = 5; return a; })()", 5);
    E6_NUM(e, "(function (a) { a = a + 1; a = a * 2; return a; })(3)", 8);
    E6_NUM(e, "(function () { var a = 1, b = 2; a; b; 1; 2; return a + b; })()", 3);
    E6_NUM(e, "(function () { var a = 1; a; a; return a; })()", 1);
    E6_NUM(e, "(function (o) { o.x = 5; return o.x; })({})", 5);
    E6_THROWS(e, "(function (o) { 'use strict'; o.x = 5; })(Object.freeze({}))", "TypeError");
    E6_BOOL(e,
            "var log = 0; var P = {set x(v) { log = v; }}; var O = Object.create(P);"
            "(function (o) { o.x = 7; })(O); log === 7",
            true);
    E6_NUM(e, "(function () { var i = 0; i++; i++; ++i; return i; })()", 3);
    E6_NUM(e, "(function (a) { a++; return a; })(2147483647)", 2147483648.0);
    E6_NUM(e, "(function (a) { a++; return a; })('5')", 6);
    E6_NUM(e, "(function (a) { a--; return a; })(-2147483648)", -2147483649.0);
    E6_BOOL(e, "(function (a) { a++; return a; })(5n) === 6n", true);
    E6_BOOL(e, "Number.isNaN((function (a) { a++; return a; })(undefined))", true);
    E6_THROWS(e, "(function (a) { a++; return a; })(Symbol())", "TypeError");
    E6_NUM(e, "(function (a) { var b = a++; return b * 10 + a; })(5)", 56);
    E6_NUM(e, "(function (a) { var b = a++; return b; })('5')", 5);
    E6_NUM(e, "(function (a) { return a++; })(2147483647)", 2147483647.0);
    E6_BOOL(e, "(function (a) { var b = a++; return b; })(5n) === 5n", true);
    E6_BOOL(e, "Number.isNaN((function (a) { var b = a++; return b; })(undefined))", true);
    es6_env_free(e);
}

EMB_TEST(js_fused_compare_and_jumps)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "(function (a, b) { if (a === b) return 1; return 2; })(1, 1)", 1);
    E6_NUM(e, "(function (a, b) { if (a === b) return 1; return 2; })('1', 1)", 2);
    E6_NUM(e, "(function (a, b) { if (a === b) return 1; return 2; })(NaN, NaN)", 2);
    E6_NUM(e, "(function (a) { if (a === 0) return 1; return 2; })(-0)", 1);
    E6_NUM(e, "(function (a) { if (a === 'abc') return 1; return 2; })('ab' + 'c')", 1);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })(1, 2)", 1);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })('10', '9')", 1);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })('9', '10')", 2);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })(2n, 3)", 1);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })(NaN, 1)", 2);
    E6_NUM(e, "(function (a, b) { if (a < b) return 1; return 2; })(1.5, 2.5)", 1);
    E6_NUM(e, "(function (a) { if (a < 5) return 1; return 2; })(4)", 1);
    E6_NUM(e, "(function (a) { if (a < 5) return 1; return 2; })(1.5)", 1);
    E6_NUM(e, "(function (a) { if (a < 5) return 1; return 2; })('3')", 1);
    E6_NUM(e, "(function (a) { if (a < 5) return 1; return 2; })(undefined)", 2);
    E6_NUM(e, "(function (a) { if (a < 5) return 1; return 2; })(7)", 2);
    E6_BOOL(e, "(function (a) { return a < 5; })(NaN) === false", true);
    E6_BOOL(e, "(function (a) { return a < 5; })(2n) === true", true);
    /* valueOf runs once, in the slow path of the compare. */
    E6_NUM(e,
           "var n = 0; (function (a, b) { if (a < b) return 1; return 2; })"
           "({valueOf: function () { n++; return 1; }}, 2) + n * 10",
           11);
    E6_THROWS(e, "(function (a, b) { if (a < b) return 1; return 2; })(Symbol(), 1)", "TypeError");
    E6_BOOL(e, "(function (v) { return v === 'abc'; })('abc')", true);
    E6_BOOL(e, "(function (v) { return v === 1.5; })(1.5)", true);
    E6_BOOL(e, "(function (v) { return v === 1; })('1')", false);
    E6_BOOL(e, "(function (v) { return v === 0; })(-0)", true);
    E6_BOOL(e, "(function (v) { return v === 'x'; })(undefined)", false);
    E6_BOOL(e, "(function (v) { return typeof v === 'string'; })('a')", true);
    E6_NUM(e, "(function () { var i = 0; while (true) { i++; if (i > 3) break; } return i; })()", 4);
    E6_NUM(e,
           "(function () { var s = 0; for (var i = 0; i < 10; i++) { if (i % 2) continue; s += i; }"
           " return s; })()",
           20);
    E6_NUM(e, "(function () { var s = 0; var i = 0; do { s = s + i; } while (i++ < 3); return s; })()",
           6);
    E6_NUM(e, "(function () { var s = 0; for (var i = 0; i < 100000; i++) s = s + 1; return s; })()",
           100000);
    es6_env_free(e);
}

/* FP1a: the peephole's candidate list grows; a function with far more than 64 fusable pairs fuses
 * every one of them. */
static uint32_t count_mnemonic(const char *text, const char *name)
{
    size_t n = strlen(name);
    uint32_t count = 0;
    for (const char *p = text; (p = strstr(p, name)) != nullptr; p += n) {
        bool lead = p - text >= 2 && p[-1] == ' ' && p[-2] == ' ';
        bool tail = p[n] == ' ' || p[n] == '\n' || p[n] == '\0';
        if (lead && tail) {
            count++;
        }
    }
    return count;
}

EMB_TEST(every_pair_of_a_large_function_is_fused)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_sb src = {nullptr, 0, 0};
    const char *head = "(function (a) { var s = 0;\n";
    embi_sb_append(&src, (embi_str){head, (uint32_t)strlen(head)});
    for (int i = 0; i < 300; i++) {
        const char *line = "s = a + 5;\n";
        embi_sb_append(&src, (embi_str){line, (uint32_t)strlen(line)});
    }
    const char *tail = "return s; })";
    embi_sb_append(&src, (embi_str){tail, (uint32_t)strlen(tail)});
    embi_sb sb = {nullptr, 0, 0};
    EMB_CHECK(disasm_src(e, embi_sb_cstr(&src), &sb));
    const char *text = embi_sb_cstr(&sb);
    EMB_REQUIRE(text != nullptr);
    EMB_CHECK_EQ_INT(count_mnemonic(text, "GET_LOC_I32"), 300);
    EMB_CHECK_EQ_INT(count_mnemonic(text, "PUSH_I32"), 1); /* the initializer of s */
    embi_sb_free(&sb);
    embi_sb_free(&src);
    es6_env_free(e);
}

EMB_TEST_MAIN()
