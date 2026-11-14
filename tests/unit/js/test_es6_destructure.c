/* ES2015 destructuring (J2b): binding and assignment patterns, parameter and catch patterns, for-in
 * heads, the iterator protocol with IteratorClose, evaluation order, TDZ, and early errors. */
#include "es6_util.h"
#include <stdio.h>

/* A binding pattern of each declaration kind, with holes, rest, defaults and nesting. */
EMB_TEST(destr_binding_array)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var [a, b] = [1, 2]; a + b", 3);
    E6_NUM(e, "let [x, , z = 9, ...r] = [1, 2, undefined, 4, 5]; x + z + r.length", 12);
    E6_NUM(e, "const [p = 5] = []; p", 5);
    E6_NUM(e, "var [[n1, n2], [n3]] = [[1, 2], [3]]; n1 + n2 + n3", 6);
    E6_NUM(e, "var [, , third] = [1, 2, 3]; third", 3);
    E6_NUM(e, "var [...all] = [7, 8]; all.length", 2);
    E6_TEXT(e, "var [q = function () {}] = []; q.name", "q");
    E6_NUM(e, "var [hole1, , hole3] = [1]; hole3 === undefined ? 4 : 0", 4);
    E6_NUM(e, "var [, ] = [1]; 6", 6);
    E6_NUM(e, "var [rr = 2] = [undefined]; rr", 2);
    E6_NUM(e, "var [nn = 2] = [null]; nn === null ? 7 : 0", 7);
    E6_NUM(e, "var rv = ([qa, qb] = [1, 2]); rv.length", 2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(destr_binding_object)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var {a, b: c, d = 4} = {a: 1, b: 2}; a + c + d", 7);
    E6_NUM(e, "var {['x' + 1]: v} = {x1: 3}; v", 3);
    E6_NUM(e, "var {...rest} = {p: 1, q: 2}; Object.keys(rest).length", 2);
    E6_NUM(e, "var {p: pp, ...r2} = {p: 1, q: 2, z: 3}; Object.keys(r2).join('') === 'qz' ? pp + 10 : 0", 11);
    E6_NUM(e, "var {0: z0, 1: z1} = ['a', 'b']; z0 === 'a' && z1 === 'b' ? 2 : 0", 2);
    E6_NUM(e, "var k = 'key'; var {[k]: kv, ...kr} = {key: 1, other: 2}; kv * 10 + Object.keys(kr).length", 11);
    E6_NUM(e, "var {length} = 'abc'; length", 3);
    E6_TEXT(e, "var {toFixed} = 5; typeof toFixed", "function");
    E6_NUM(e, "var {} = {}; 5", 5);
    E6_NUM(e, "var {n1: {deep: [dd]}} = {n1: {deep: [8]}}; dd", 8);
    E6_THROWS(e, "var {x} = null;", "TypeError");
    E6_THROWS(e, "var {} = undefined;", "TypeError");
    E6_THROWS(e, "var {y: {z}} = {};", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Assignment patterns: targets are references, and the value of the expression is the right side. */
EMB_TEST(destr_assign)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var a1 = 1, b1 = 2; [a1, b1] = [b1, a1]; a1 * 10 + b1", 21);
    E6_NUM(e, "var o1 = {}; ({x: o1.p, y: o1['q']} = {x: 4, y: 5}); o1.p + o1.q", 9);
    E6_NUM(e, "var t1; ({t1 = 8} = {}); t1", 8);
    E6_NUM(e, "var t2, t3; [t2, [t3] = [6]] = [1]; t2 + t3", 7);
    E6_NUM(e, "var r1, r2; ({a: r1, ...r2} = {a: 1, b: 2, c: 3}); r1 + Object.keys(r2).length", 3);
    E6_NUM(e, "var h1, h2; [h1, , h2] = [1, 2, 3]; h1 + h2", 4);
    E6_NUM(e, "var z; var res = ([z] = [5]); res[0] + z", 10);
    E6_NUM(e, "var ar = []; [ar[0], ar[1] = 3] = [1]; ar[0] + ar[1]", 4);
    E6_TEXT(e, "var fb; ({fb = () => 1} = {}); fb.name", "fb");
    E6_NUM(e, "var ob = {}; [...ob.rest] = [1, 2, 3]; ob.rest.length", 3);
    E6_NUM(e, "var ok1 = 0; ({ok1} = {ok1: 6}); ok1", 6);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Evaluation order: a target's base and key before the value; defaults only when undefined. */
EMB_TEST(destr_order)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var log = []; var o = {}; [o[(log.push('k'), 'x')]] = [(log.push('v'), 1)]; log.join('')", "vk");
    E6_TEXT(e, "var log2 = []; var src = {get p() { log2.push('get'); return 1; }}; var t = {}; ({p: t.x} = src); log2.join('')", "get");
    E6_NUM(e, "var cnt = 0; function d() { cnt++; return 2; } var [l1 = d(), l2 = d()] = [1]; cnt", 1);
    E6_NUM(e, "var cnt2 = 0; function d2() { cnt2++; return 2; } var {u = d2()} = {u: 0}; cnt2", 0);
    E6_NUM(e, "var seen = 0; var [w = seen] = [undefined]; w", 0);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Iterator protocol: exhaustion, IteratorClose on early exit and throws, and the errors it drops. */
static const char *const k_mk =
    "function mk(n) { var log = []; var i = 0; var it = {}; it[Symbol.iterator] = function () {"
    "  return { next: function () { log.push('n'); return { value: i, done: i++ >= n }; },"
    "           return: function () { log.push('r'); return {}; } }; };"
    "  return { iter: it, log: log }; }"
    "function mku() { var log = []; var it = {}; it[Symbol.iterator] = function () {"
    "  return { next: function () { log.push('n'); return { value: undefined, done: false }; },"
    "           return: function () { log.push('r'); return {}; } }; };"
    "  return { iter: it, log: log }; }";

EMB_TEST(destr_iterator_close)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, k_mk) != EMBI_JS_EXCEPTION);
    E6_TEXT(e, "var m = mk(5); var [p1, p2] = m.iter; m.log.join('')", "nnr");
    E6_TEXT(e, "var m2 = mk(2); var [q1, q2, q3] = m2.iter; m2.log.join('')", "nnn");
    E6_TEXT(e, "var m3 = mk(5); var [...rr] = m3.iter; m3.log.join('') + rr.length", "nnnnnn5");
    E6_TEXT(e, "var m4 = mk(2); var [, ] = m4.iter; m4.log.join('')", "nr");
    E6_TEXT(e, "var mu = mku(); var caught = ''; try { var [w = (function () { throw new RangeError('b'); })()] = mu.iter; } catch (err) { caught = err.name; } caught + mu.log.join('')", "RangeErrornr");
    E6_NUM(e, "var rlog = 0; var nt = {}; nt[Symbol.iterator] = function () { return { next: function () { throw new TypeError('nx'); }, return: function () { rlog++; return {}; } }; }; try { var [ee] = nt; } catch (err) {} rlog", 0);
    E6_THROWS(e, "var rt2 = {}; rt2[Symbol.iterator] = function () { return { next: function () { return {value: 1, done: false}; }, return: function () { throw new RangeError('rr'); } }; }; var [o1] = rt2;", "RangeError");
    E6_TEXT(e, "var tt = {}; tt[Symbol.iterator] = function () { return { next: function () { return {value: undefined, done: false}; }, return: function () { throw new TypeError('ignored'); } }; }; var caught2 = ''; try { var [z = (function () { throw new RangeError('orig'); })()] = tt; } catch (err) { caught2 = err.name; } caught2", "RangeError");
    E6_THROWS(e, "var bad = {}; bad[Symbol.iterator] = function () { return { next: function () { return 1; } }; }; var [bz] = bad;", "TypeError");
    E6_THROWS(e, "var [nz] = 5;", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Parameter patterns: defaults and rest on patterns, length, and the parameter scope. */
EMB_TEST(destr_params)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function pa({a, b: [c] = [3]}, ...[d, e2]) { return a + c + d + e2 } pa({a: 1}, 2, 4)", 10);
    E6_NUM(e, "var af = ({x}, [y] = [5]) => x + y; af({x: 1})", 6);
    E6_NUM(e, "function pl({length}) { return length } pl('hey')", 3);
    E6_NUM(e, "function plen(a, [b], {c}) {} plen.length", 3);
    E6_NUM(e, "function pdef([a] = [4]) { return a } pdef()", 4);
    E6_NUM(e, "function pe(a, b = a, {c = b} = {}) { return c } pe(7)", 7);
    E6_NUM(e, "function pr(...[x, ...ys]) { return x + ys.length } pr(1, 2, 3)", 3);
    E6_NUM(e, "function pv([v]) { var v; return v } pv([9])", 9);
    E6_NUM(e, "function pw({a} = {a: 2}) { a = a * 2; return a } pw()", 4);
    E6_THROWS(e, "function pn({n}) {} pn();", "TypeError");
    E6_TEXT(e, "function pf([f1] = [], {f2} = {f2: 'x'}) { return f1 + f2 } pf(['a'])", "ax");
    E6_NUM(e, "function pdd(x = 1, [y = x] = []) { return y } pdd(5)", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Catch parameters, for-in heads, and per-iteration bindings of a loop body. */
EMB_TEST(destr_catch_forin)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var cm; try { throw {m: 5}; } catch ({m}) { cm = m; } cm", 5);
    E6_NUM(e, "var ca; try { throw [1, 2]; } catch ([p, q]) { ca = p + q; } ca", 3);
    E6_TEXT(e, "var fk4 = ''; for (var {length: ln} in {abc: 1}) fk4 += ln; fk4", "3");
    E6_NUM(e, "var n1 = 0; for (var {length: z} in {aa: 1, bb: 2}) { n1++; break; } n1", 1);
    E6_NUM(e, "var fs = []; for (let i = 0; i < 2; i++) { let [v] = [i]; fs.push(() => v); } fs[0]() + fs[1]()", 1);
    E6_NUM(e, "var fs2 = []; for (var key in {a: 1, b: 2}) { let [v2] = [key]; fs2.push(() => v2); } fs2[0]() === 'a' && fs2[1]() === 'b' ? 3 : 0", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A catch pattern's names are boxed before the pattern stores them: a default may capture them in a
 * closure, or an eval may refer to them. */
EMB_TEST(destr_catch_boxes)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var cq2; try { throw [undefined, 7]; } catch ([cw2 = () => cw2, cv2]) { cq2 = cw2(); } typeof cq2", "function");
    E6_NUM(e, "var ce3 = 0; try { throw [undefined]; } catch ([cw3 = eval('ce3 = 9; 4')]) { ce3 += cw3; } ce3", 13);
    E6_NUM(e, "var cz = 0; try { throw {p: 2}; } catch ({p: cp, q: cq = cp * 3}) { cz = cq; } cz", 6);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Lexical bindings in a pattern: TDZ for a name used before its own initialization. */
EMB_TEST(destr_tdz)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "let [tz1 = tz2, tz2] = [];", "ReferenceError");
    E6_THROWS(e, "let {tz3 = tz3} = {};", "ReferenceError");
    E6_NUM(e, "let [tzA, tzB = tzA] = [4]; tzB", 4);
    E6_NUM(e, "let {ta, tb = ta + 1} = {ta: 2}; tb", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Early errors: invalid patterns, targets, rest elements and redeclarations. */
EMB_TEST(destr_early_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "let [a, a] = [1];");
    E6_SYNTAX(e, "let [b];");
    E6_SYNTAX(e, "const {c};");
    E6_SYNTAX(e, "var [d];");
    E6_SYNTAX(e, "var [...f, g] = [];");
    E6_SYNTAX(e, "[h, ...i,] = [];");
    E6_SYNTAX(e, "[...j = 1] = [];");
    E6_SYNTAX(e, "({...{k}} = {});");
    E6_SYNTAX(e, "var {...m = 1} = {};");
    E6_SYNTAX(e, "[n + 1] = [];");
    E6_SYNTAX(e, "({o: 1} = {});");
    E6_SYNTAX(e, "({p}) = {};");
    E6_SYNTAX(e, "'use strict'; [eval] = [1];");
    E6_SYNTAX(e, "function ff([q, q]) {}");
    E6_SYNTAX(e, "try {} catch ([r, r]) {}");
    E6_SYNTAX(e, "let [s] = [1], [s] = [2];");
    E6_SYNTAX(e, "let [let] = [1];");
    E6_SYNTAX(e, "var [t] += 1;");
    E6_SYNTAX(e, "function fg(...[w] = []) {}");
    E6_SYNTAX(e, "[x] => 1;");
    /* B.3.5: the names of a destructured catch parameter may not be var-declared in its block. */
    E6_SYNTAX(e, "try {} catch ([ca]) { var ca; }");
    E6_SYNTAX(e, "try {} catch ({cb}) { var cb; }");
    E6_SYNTAX(e, "try {} catch ([cc]) { for (var cc in {}) ; }");
    E6_SYNTAX(e, "try {} catch ([cd]) { { var cd; } }");
    E6_SYNTAX(e, "try {} catch ([ce]) { let ce; }");
    E6_SYNTAX(e, "try {} catch ({cf: [cg]}) { var cg = 1; }");
    E6_NUM(e, "var ann = 0; try { throw 4; } catch (ce2) { var ce2 = 5; ann = ce2; } ann", 5);
    E6_NUM(e, "var [y1] = [1], {y2} = {y2: 2}; y1 + y2", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A syntax error whose message is want (the message of the declaration that failed). */
static bool syntax_msg_is(es6_env e, const char *src, const char *want)
{
    embi_js_value v = es6_run(e, src);
    bool ok = false;
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(e.rt);
        if (embi_js_is_object(ex) && es6_exc_name_is(e.rt, "SyntaxError")) {
            embi_js_value m = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "message"));
            ok = es6_text_is(e.rt, m, want);
        }
        embi_js_exception_clear(e.rt);
    }
    if (!ok) {
        es6_note(src);
    }
    return ok;
}

/* The message of a redeclaration is kept when the pattern is scanned for its names. */
EMB_TEST(destr_early_messages)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *dup = "Identifier has already been declared";
    EMB_CHECK(syntax_msg_is(e, "let [a1] = [1]; let [a1] = [2];", dup));
    EMB_CHECK(syntax_msg_is(e, "let {a2, a2} = {};", dup));
    EMB_CHECK(syntax_msg_is(e, "for (let [a3, a3] in {}) ;", dup));
    EMB_CHECK(syntax_msg_is(e, "const [a4, {a4}] = [1, {}];", dup));
    EMB_CHECK(syntax_msg_is(e, "try {} catch ([a5]) { var a5; }", dup));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Nesting: a deeply nested pattern is a SyntaxError or RangeError in every form (never a native
 * stack overflow), and a moderately nested one works. The sources are built in static buffers,
 * since 100000 levels is far too much for the stack. */
#define DEEP_N 100000
static char g_deep[4 * DEEP_N + 512];

/* Copies s with its terminator, so the built source ends where it should (a reused buffer). */
static char *put(char *w, const char *s)
{
    size_t l = strlen(s);
    memcpy(w, s, l + 1);
    return w + l;
}

static char *put_rep(char *w, const char *s, int n)
{
    size_t l = strlen(s);
    for (int i = 0; i < n; i++) {
        memcpy(w, s, l);
        w += l;
    }
    return w;
}

/* pre, then n copies of open, a name, n copies of close, then post. */
static const char *deep_src(const char *pre, const char *open, const char *close, int n, const char *post)
{
    char *w = put(g_deep, pre);
    w = put_rep(w, open, n);
    w = put(w, "x");
    w = put_rep(w, close, n);
    put(w, post);
    return g_deep;
}

static bool deep_rejected(es6_env e, const char *src)
{
    embi_js_value v = es6_run(e, src);
    bool ok = v == EMBI_JS_EXCEPTION &&
              (es6_exc_name_is(e.rt, "SyntaxError") || es6_exc_name_is(e.rt, "RangeError"));
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
    }
    if (!ok) {
        es6_note("(deep nesting)");
    }
    return ok;
}

EMB_TEST(destr_deep_nesting)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(deep_rejected(e, deep_src("var ", "[", "]", DEEP_N, " = 1;")));
    EMB_CHECK(deep_rejected(e, deep_src("let ", "[", "]", DEEP_N, " = 1;")));
    EMB_CHECK(deep_rejected(e, deep_src("function fdn(", "[", "]", DEEP_N, ") {}")));
    EMB_CHECK(deep_rejected(e, deep_src("try {} catch (", "[", "]", DEEP_N, ") {}")));
    EMB_CHECK(deep_rejected(e, deep_src("", "[", "]", DEEP_N, " = 1;")));
    EMB_CHECK(deep_rejected(e, deep_src("var ", "{a:", "}", DEEP_N, " = {};")));
    EMB_CHECK(deep_rejected(e, deep_src("function fdo(", "{a:", "}", DEEP_N, ") {}")));
    EMB_CHECK(deep_rejected(e, deep_src("(", "[", "]", DEEP_N, ") => 1;")));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Moderate nesting in each binding form and in the assignment form. The release depth is past the
 * old 128-level limit of the lookahead. Clang Debug frames are much larger than gcc's (300 levels exceed the
 * 768 KB native budget there), and under ASan the frames take even more of it (Linux clang ASan fails at 110)
 * (a deeper source is a RangeError there, which is also correct), so the depth is smaller. */
#if EMB_TEST_ASAN
#define MOD_N 48
#else
#define MOD_N 150
#endif
EMB_TEST(destr_moderate_nesting)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    static char mod[8192];
    char *w = put(mod, "var ");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "x");
    w = put_rep(w, "]", MOD_N);
    w = put(w, " = ");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "1");
    w = put_rep(w, "]", MOD_N);
    put(w, "; x");
    E6_NUM(e, mod, 1);
    w = put(mod, "function fmn(");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "x");
    w = put_rep(w, "]", MOD_N);
    w = put(w, ") { return x; } fmn(");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "2");
    w = put_rep(w, "]", MOD_N);
    put(w, ")");
    E6_NUM(e, mod, 2);
    w = put(mod, "var nx; ");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "nx");
    w = put_rep(w, "]", MOD_N);
    w = put(w, " = ");
    w = put_rep(w, "[", MOD_N);
    w = put(w, "3");
    w = put_rep(w, "]", MOD_N);
    put(w, "; nx");
    E6_NUM(e, mod, 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Assignment patterns in for-in heads, arrow parameter patterns, and parenthesized targets. */
EMB_TEST(destr_assign_more)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var fa1 = {}; for ({length: fa1.n} in {xyz: 1}) ; fa1.n", 3);
    E6_TEXT(e, "var fa2 = {}; for ({0: fa2.c} in {qq: 1}) ; fa2.c", "q");
    E6_TEXT(e, "var fa3 = {}; for ({length: fa3.n} in {}) ; typeof fa3.n", "undefined");
    E6_NUM(e, "var pa1 = ([a], {b}) => a + b; pa1([1], {b: 2})", 3);
    E6_NUM(e, "var pa2 = ([a] = [1]) => a; pa2()", 1);
    E6_NUM(e, "var pa3 = ({x: [y]} = {x: [7]}) => y; pa3()", 7);
    E6_NUM(e, "var pa4 = ([p1], ...[p2]) => p1 + p2; pa4([4], 5, 6)", 9);
    E6_NUM(e, "var pp1; [(pp1)] = [1]; pp1", 1);
    E6_NUM(e, "var pp2; ({x: (pp2)} = {x: 2}); pp2", 2);
    E6_SYNTAX(e, "var pp3; [(pp3 = 1)] = [2];");
    E6_SYNTAX(e, "var pp4; ([pp4]) = [1];");
    E6_SYNTAX(e, "var pp5; [([pp5])] = [[1]];");
    E6_SYNTAX(e, "var pp6; ({x: ({pp6})} = {x: {}});");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* IteratorClose for assignment patterns: a throwing target reference or default, and a strict
 * failed store, each close the iterator (once, with the error of return dropped). */
EMB_TEST(destr_assign_close)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, k_mk) != EMBI_JS_EXCEPTION);
    E6_TEXT(e, "var sm = mk(5); var tgt1 = {}; Object.defineProperty(tgt1, 'x', {set: function (v) { throw new RangeError('s'); }}); var sc1 = ''; try { [tgt1.x] = sm.iter; } catch (err) { sc1 = err.name; } sc1 + sm.log.join('')", "RangeErrornr");
    E6_TEXT(e, "function sf() { 'use strict'; var fr = Object.freeze({v: 0}); var sm2 = mk(5); var c2 = ''; try { [fr.v] = sm2.iter; } catch (err) { c2 = err.name; } return c2 + sm2.log.join(''); } sf()", "TypeErrornr");
    E6_TEXT(e, "var sm3 = mk(1); var t3 = {}; [t3.y] = sm3.iter; sm3.log.join('') + t3.y", "nr0");
    E6_TEXT(e, "var mu2 = mku(); var t4 = {}; var sc4 = ''; try { [t4.z = (function () { throw new RangeError('d'); })()] = mu2.iter; } catch (err) { sc4 = err.name; } sc4 + mu2.log.join('')", "RangeErrornr");
    E6_TEXT(e, "var mu3 = mku(); var t5 = {}; [t5.q, t5.r] = mu3.iter; mu3.log.join('')", "nnr");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Object rest: the excluded keys are skipped before their descriptors are read, the order of
 * traps follows CopyDataProperties, and symbol keys are excluded by identity. */
EMB_TEST(destr_object_rest_proxy)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var plog = []; var px = new Proxy({a: 1, b: 2}, {ownKeys: function (t) { plog.push('ownKeys'); return Reflect.ownKeys(t); }, getOwnPropertyDescriptor: function (t, k) { plog.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); }, get: function (t, k, r) { plog.push('get:' + String(k)); return Reflect.get(t, k, r); }}); var {a: pa1, ...prest} = px; plog.join(',') + '|' + prest.b", "get:a,ownKeys,gopd:b,get:b|2");
    E6_NUM(e, "var s3 = Symbol('s'); var {[s3]: sv, ...sr} = {[s3]: 1, t: 2}; sv + Object.getOwnPropertySymbols(sr).length", 1);
    E6_NUM(e, "var s4 = Symbol('t'); var s5 = Symbol('u'); var {[s5]: sv2, ...sr2} = {[s5]: 1, [s4]: 5}; sr2[s4] + sv2 * 10", 15);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Catch patterns are in their temporal dead zone until the pattern stores them. */
EMB_TEST(destr_catch_tdz)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "try { throw {}; } catch ({a = b, b}) {}", "ReferenceError");
    E6_THROWS(e, "try { throw []; } catch ([a = a]) {}", "ReferenceError");
    E6_NUM(e, "var r; try { throw {b: 1}; } catch ({b, a = b}) { r = a; } r", 1);
    E6_NUM(e, "var r2; try { throw {b: 4}; } catch ({a = () => b, b}) { r2 = a(); } r2", 4);
    E6_NUM(e, "var r3; try { throw {b: 5}; } catch ({b}) { r3 = b; } r3", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A parameter pattern is in its temporal dead zone for earlier defaults; a nested array pattern
 * closes the outer iterator when the inner one throws, and closes both in order on a normal exit. */
EMB_TEST(destr_param_tdz_nested_close)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "function f(a = b, [b]) {} f(undefined, [1]);", "ReferenceError");
    E6_NUM(e, "function g(a, [b] = [a]) { return b; } g(3)", 3);
    const char *mkit =
        "var log = []; function mkn(throws) { var inner = {}; inner[Symbol.iterator] = function () {"
        " return { next: function () { log.push('in'); if (throws) { throw new RangeError('x'); }"
        " return { value: 1, done: false }; }, return: function () { log.push('ir'); return {}; } }; };"
        " var outer = {}; outer[Symbol.iterator] = function () {"
        " return { next: function () { log.push('on'); return { value: inner, done: false }; },"
        " return: function () { log.push('or'); return {}; } }; }; return outer; }";
    EMB_CHECK(es6_run(e, mkit) != EMBI_JS_EXCEPTION);
    E6_TEXT(e, "var c = ''; try { var [[x]] = mkn(true); } catch (err) { c = err.name; } c + log.join(',')",
            "RangeErroron,in,or");
    E6_TEXT(e, "log.length = 0; var [[y]] = mkn(false); log.join(',')", "on,in,ir,or");
    E6_TEXT(e, "log.length = 0; var yy; [[yy]] = mkn(false); log.join(',')", "on,in,ir,or");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A let/const declaration with a pattern is in its temporal dead zone for its own initializer, at
 * script level, in a function and in a block; later code reads the bindings unchecked. */
EMB_TEST(destr_own_init_tdz)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    static const char *const bodies[] = {
        "let [x] = [x];",
        "const {a} = {a: a};",
        "let [y] = (function(){ return [y]; })();",
        "let [z] = [()=>z][0]();",
        "let [w] = [typeof w];",
        "let [v = 1] = [v];",
    };
    char buf[256];
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        snprintf(buf, sizeof buf, "%s", bodies[i]);
        E6_THROWS(e, buf, "ReferenceError");
        snprintf(buf, sizeof buf, "(function () { %s })()", bodies[i]);
        E6_THROWS(e, buf, "ReferenceError");
        snprintf(buf, sizeof buf, "(function () { { %s } })()", bodies[i]);
        E6_THROWS(e, buf, "ReferenceError");
    }
    E6_NUM(e, "(function () { let [p, q] = [1, 2]; const {r} = {r: p + q}; return r; })()", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
