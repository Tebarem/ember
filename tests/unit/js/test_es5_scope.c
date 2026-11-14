#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
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

/* An environment that runs the collector at every allocation check (GC stress). */
static env env_new_gc_stress(void)
{
    env e = env_new();
    if (e.rt) {
        embi_js_set_gc_params(e.rt, 0, 0.0);
    }
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

static double num_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
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

static bool exc_name_is(embi_js_runtime *rt, const char *want)
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
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

/* Prints the source of a failed check, so the failing case is named in the log. */
static void note_src(const char *src)
{
    SDL_Log("    in: %s", src);
}

#define CHECK_NUM(e_, src_, want_)                                                             \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && embi_js_is_number(v_) &&                         \
                   num_of(v_) == (double)(want_);                                              \
        if (!ok_) {                                                                            \
            note_src(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

#define CHECK_TEXT(e_, src_, want_)                                                            \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && text_is((e_).rt, v_, (want_));                   \
        if (!ok_) {                                                                            \
            note_src(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

#define CHECK_BOOL(e_, src_, want_)                                                            \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        bool ok_ = v_ != EMBI_JS_EXCEPTION && embi_js_is_bool(v_) &&                           \
                   embi_js_as_bool(v_) == (bool)(want_);                                       \
        if (!ok_) {                                                                            \
            note_src(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

#define CHECK_THROWS(e_, src_, name_)                                                          \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ == EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(exc_name_is((e_).rt, (name_)));                                              \
        embi_js_exception_clear((e_).rt);                                                      \
    } while (0)

/* with statements: names resolve through the object first, then the scopes outside it. */
static void with_cases(env e)
{
    CHECK_NUM(e, "var o = {x: 1}; with (o) { x }", 1);
    CHECK_NUM(e, "var o2 = {x: 1}; with (o2) { x = 5 } o2.x", 5);
    CHECK_NUM(e, "var y = 2; with ({}) { y }", 2);
    CHECK_NUM(e, "function g() { var z = 3; with ({a: 1}) { return a + z } } g()", 4);
    CHECK_NUM(e, "var o3 = {v: 7, f: function () { return this.v }}; with (o3) { f() }", 7);
    CHECK_BOOL(e, "var o9 = {v: 4, g: function () { return this }}; with (o9) { g() === o9 }", true);
    CHECK_TEXT(e, "with ({}) typeof nope", "undefined");
    CHECK_BOOL(e, "var o4 = {p: 1}; with (o4) { delete p } !('p' in o4)", true);
    CHECK_NUM(e, "var n = 0; with ({k: 1}) { var m = k + 1; n = m } n", 2);
    /* Blocks whose scan for a function declaration is ambiguous or deep keep the entry jump. */
    CHECK_NUM(e, "var bq1; { bq1 = bq1h(); var d1 = 8 / 2; function bq1h() { return 4 } } bq1 + d1", 8);
    CHECK_NUM(e, "var bq2; { var r2 = '}'.length / 1; { bq2 = bq2h(); } function bq2h() { return 3 } } bq2", 3);
    CHECK_NUM(e, "var bq3 = 0; for (var i3 = 0; i3 < 3; i3++) { bq3 += i3 } bq3", 3);
    /* The reference is resolved before the right-hand side runs (ES5 8.7.2, 11.13.1). */
    CHECK_TEXT(e, "var wo1={xa1:1}; with(wo1){ xa1 = (delete wo1.xa1, 5) } wo1.xa1 + ',' + typeof xa1", "5,undefined");
    CHECK_TEXT(e, "var wo2={}; function wg2(){wo2.xa2=1; return 7} with(wo2){ xa2 = wg2() } wo2.xa2+','+typeof xa2", "1,number");
    CHECK_TEXT(e, "var wo3={xa3:1}; with(wo3){ xa3 += (delete wo3.xa3, 1) } wo3.xa3 + ',' + typeof xa3", "2,undefined");
    CHECK_TEXT(e, "var wo4={xa4:1}; with(wo4){ xa4 -= (delete wo4.xa4, 1) } wo4.xa4", "0");
    CHECK_TEXT(e, "var wo5={xa5:1}; with(wo5){ var xa5 = (delete wo5.xa5, 9) } wo5.xa5", "9");
    CHECK_TEXT(e, "var wo6={xa6:{valueOf:function(){delete wo6.xa6; return 1}}}; with(wo6){ xa6++ } typeof wo6.xa6 + ',' + wo6.xa6", "number,2");
    CHECK_TEXT(e, "var wo7={xa7:1}; var wr7; with(wo7){ wr7 = xa7++ } wr7 + ',' + wo7.xa7", "1,2");
    CHECK_TEXT(e, "var wo8={xa8:1}; var wr8; with(wo8){ wr8 = --xa8 } wr8 + ',' + wo8.xa8", "0,0");
    CHECK_TEXT(e, "var wo9={}; with(wo9){ xa9y = (wo9.xa9y = 3, 4) } wo9.xa9y + ',' + xa9y", "3,4");
    CHECK_TEXT(e, "var wp={xp:1}; function wq1(){ with(wp){ return function(){ xp = (delete wp.xp, 8); return typeof xp } } } wq1()() + ',' + wp.xp", "number,8");
    /* Function declarations in sloppy eval replace function-level bindings (ES5 10.5). */
    CHECK_TEXT(e, "function ef1(a){ eval('function a(){}'); return typeof a } ef1(1)", "function");
    CHECK_TEXT(e, "function ef2(){ var a=1; eval('function a(){return 2}'); return typeof a } ef2()", "function");
    CHECK_NUM(e, "function ef3(){ function a(){return 1}; eval('function a(){return 2}'); return a() } ef3()", 2);
    /* Eval code assigning through box holders. */
    CHECK_NUM(e, "function eb1(){ var a=1; eval('a = 5'); return a } eb1()", 5);
    CHECK_NUM(e, "function eb2(){ var a=1; eval('a += 1; a++'); return a } eb2()", 3);
    CHECK_NUM(e, "function eb3(){ var a=1; eval('--a; a -= 5'); return a } eb3()", -5);
    CHECK_NUM(e, "function eb4(){ eval('var v=1'); eval('v += 4; v++'); return v } eb4()", 6);
    CHECK_TEXT(e, "var eo={x:1}; function eb5(){ with(eo){ eval('x = (delete eo.x, 5)') } return eo.x + ',' + typeof x } eb5()", "5,undefined");
    CHECK_NUM(e, "var eo6={x:1}; function eb6(){ with(eo6){ eval('x += 2; x++') } return eo6.x } eb6()", 4);
    /* Eval-declared names target the function's variable environment, not catch or block scopes. */
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(e){ eval('function e(){}'); return typeof e } } f()", "number");
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(e){ eval('function e(){}'); } return typeof e } f()", "function");
    CHECK_TEXT(e, "function f(){ var r; try{throw 1}catch(e){ eval('var e = 5'); r = e } return r + ',' + e } f()", "5,undefined");
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(e){ eval('var e'); return e + ',' + typeof e } } f()", "1,number");
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(e){ eval('var e') } return typeof e } f()", "undefined");
    CHECK_TEXT(e, "function f(){ var o={e:7}; with(o){ eval('var e'); } return o.e + ',' + typeof e } f()", "7,undefined");
    CHECK_TEXT(e, "function f(){ var o={e:7}; with(o){ eval('var e = 2'); } return o.e + ',' + e } f()", "2,undefined");
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(q){ { eval('var e2 = q + 1'); } } return typeof q + ',' + e2 } f()", "undefined,2");
    CHECK_TEXT(e, "function f(){ try{throw 1}catch(e){ { eval('var e = 9'); } return e } } f()", "9");
    CHECK_TEXT(e, "function f(a){ try{throw 1}catch(e){ eval('var a = 3'); } return a } f(1)", "3");
    /* The for-in target is resolved after the object expression runs, per iteration: deleting
     * gw.k there makes k resolve to the global, so gw.k stays deleted. */
    CHECK_TEXT(e, "var gw={k:0}; (function(){ with(gw){ for (k in (delete gw.k, {z:1})) {} } })(); typeof gw.k", "undefined");
    CHECK_TEXT(e, "var gw2={k:0}; (function(){ with(gw2){ for (k in {z:1}) {} } })(); typeof gw2.k", "string");
    /* Control flow leaving a with body. */
    CHECK_NUM(e, "function wr() { var o = {a: 1}; with (o) { return a } } wr()", 1);
    CHECK_NUM(e, "var io = {}; var i = 0; while (i < 3) { with (io) { i++; if (i < 2) continue; break; } } i",
              2);
    CHECK_NUM(e, "var o8 = {n: 2}; var t8 = 0; for (var q8 = 0; q8 < 3; q8++) { with (o8) { if (q8 == 1) break; t8 = t8 + n } } t8",
              2);
    CHECK_TEXT(e, "var log = ''; function wf2() { var o = {x: 1}; try { with (o) { return x } } finally { log = 'f' } } wf2() + log",
               "1f");
    /* try and catch inside a with body. */
    CHECK_NUM(e, "var o6 = {v: 3}; with (o6) { try { throw v } catch (e) { var c6 = e } } c6", 3);
    CHECK_NUM(e, "var o7 = {v: 1}; var r7 = 0; with (o7) { try { r7 = v; throw 2 } catch (e) { r7 = r7 + e } } r7",
              3);
}

/* Direct and indirect eval. */
static void eval_cases(env e)
{
    CHECK_NUM(e, "eval('1 + 2')", 3);
    CHECK_NUM(e, "var a = 5; eval('a * 2')", 10);
    CHECK_NUM(e, "function f() { var b = 4; return eval('b + 1'); } f()", 5);
    CHECK_NUM(e, "function fv() { eval('var v = 8'); return v; } fv()", 8);
    CHECK_NUM(e, "eval('var gv = 3'); gv", 3);
    CHECK_NUM(e, "var iv = 1; (0, eval)('iv + 1')", 2);
    CHECK_NUM(e, "eval('1; 2')", 2);
    CHECK_NUM(e, "eval(42)", 42);
    CHECK_BOOL(e, "var o = {m: function () { return eval('this') }}; o.m() === o", true);
    CHECK_TEXT(e, "function s() { 'use strict'; eval('var sv = 1'); return typeof sv } s()", "undefined");
    CHECK_NUM(e, "eval('function ef() { return 11 }'); ef()", 11);
    CHECK_NUM(e, "function outer() { var c = 6; function inner() { return eval('c') } return inner() } outer()",
              6);
    CHECK_NUM(e, "with ({wq: 12}) { eval('wq') }", 12);
    CHECK_NUM(e, "var w = 1; function fw() { eval('w = 9') } fw(); w", 9);
    CHECK_NUM(e, "function nn() { var d = 2; return eval(\"eval('d * 3')\") } nn()", 6);
    CHECK_BOOL(e, "function dv() { eval('var z1 = 1'); return delete z1 } dv()", true);
    /* Eval-declared variables are deletable from a closure of the declaring function too. */
    CHECK_BOOL(e, "function ed() { eval('var z9 = 1'); return function () { return delete z9 } } ed()()", true);
    CHECK_NUM(e, "function ev1() { eval('var q1 = 4'); return function () { return q1 } } ev1()()", 4);
    CHECK_THROWS(e, "eval('(')", "SyntaxError");
    /* Nested with and eval. */
    CHECK_NUM(e, "var q = 1; function ff() { with ({q: 2}) { return function () { return q } } } ff()()", 2);
    CHECK_NUM(e, "function h() { var k = 4; with ({}) { return eval('k') } } h()", 4);
}

/* Mapped and unmapped arguments objects. */
static void arguments_cases(env e)
{
    CHECK_NUM(e, "function m(a) { arguments[0] = 9; return a } m(1)", 9);
    /* var arguments keeps the arguments object (ES5 10.5 step 7). */
    CHECK_TEXT(e, "function va1(a) { var arguments; return typeof arguments } va1(1)", "object");
    CHECK_NUM(e, "function va2(a) { var arguments; return arguments[0] } va2(7)", 7);
    CHECK_NUM(e, "function va3(a) { var arguments = 5; return arguments } va3(1)", 5);
    CHECK_NUM(e, "function va4(a) { var arguments; arguments[0] = 4; return a } va4(1)", 4);
    CHECK_TEXT(e, "function va5() { var arguments; return typeof arguments + arguments.length } va5(1, 2)", "object2");
    CHECK_TEXT(e, "function va6() { var arguments; function arguments() {} return typeof arguments } va6(1)", "function");
    CHECK_TEXT(e, "function va7() { function arguments() {} var arguments; return typeof arguments } va7(1)", "function");
    CHECK_TEXT(e, "function va8(arguments) { var arguments; return typeof arguments } va8(1)", "number");
    CHECK_TEXT(e, "function va9() { return eval('var arguments; typeof arguments') } va9(1)", "object");
    CHECK_TEXT(e, "function va10() { return function () { var arguments; return typeof arguments }() } va10()", "object");
    CHECK_NUM(e, "function m2(a) { a = 7; return arguments[0] } m2(1)", 7);
    CHECK_NUM(e, "function m3(a) { 'use strict'; arguments[0] = 9; return a } m3(1)", 1);
    CHECK_NUM(e, "function m4(a) { delete arguments[0]; arguments[0] = 4; return a } m4(2)", 2);
    CHECK_NUM(e, "function m5(a) { a = 3; arguments[0] = 8; return a } m5()", 3);
    CHECK_BOOL(e, "function cf() { return arguments.callee } cf() === cf", true);
    CHECK_THROWS(e, "function sc() { 'use strict'; return arguments.callee } sc()", "TypeError");
    /* After delete the index is no longer mapped. */
    CHECK_TEXT(e, "function d1(a) { delete arguments[0]; a = 5; return arguments[0] } d1(1)", "undefined");
    CHECK_NUM(e, "function d2(a) { a = 5; delete arguments[0]; arguments[0] = 6; return a } d2(1)", 5);
    CHECK_NUM(e, "function d3(a) { arguments[0] = 2; return a } d3(1)", 2);
    CHECK_NUM(e, "function d4(a) { var o = arguments; a = 3; return o[0] } d4(1)", 3);
    CHECK_NUM(e, "function d5(a) { arguments.x = 1; arguments.y = 2; delete arguments.x; a = 8; return arguments[0] + arguments.y } d5(1)",
              10);
}

/* Annex B.3.3: function declarations in blocks of sloppy code. */
static void annexb_cases(env e)
{
    CHECK_TEXT(e, "function f() { var r = typeof g; { function g() {} } return r } f()", "undefined");
    CHECK_TEXT(e, "function f() { if (false) { function g() {} } return typeof g } f()", "undefined");
    CHECK_TEXT(e, "function f() { var g = 1; { function g() {} } return typeof g } f()", "function");
    CHECK_TEXT(e, "function f() { g = 1; { function g() {} } return typeof g } f()", "function");
    /* Parameter names are not replaced by the block function. */
    CHECK_TEXT(e, "function f(g) { { function g() {} } return typeof g } f(1)", "number");
    CHECK_NUM(e, "{ function g() { return 1 } } { function g() { return 2 } } g()", 2);
    /* Hoisted within the block: callable before the declaration. */
    CHECK_TEXT(e, "function f() { var k; { k = typeof h; function h() {} } return k } f()", "function");
    CHECK_NUM(e, "function f() { var r; { r = h(9); function h(n) { return n } } return r } f()", 9);
    CHECK_NUM(e, "function f() { var r; { function h(n) { return n ? h(n - 1) + 1 : 0 } r = h(3) } return r } f()",
              3);
    /* Inside the block the block function shadows the var. */
    CHECK_TEXT(e, "function f() { var g = 1; var t; { t = typeof g; function g() {} } return t } f()", "function");
    CHECK_TEXT(e, "var seen = typeof zq; { function zq() {} } seen + ' ' + typeof zq", "undefined function");
    CHECK_NUM(e, "function c() { try { throw 1 } catch (e) { function q() { return e } return q() } } c()", 1);
    /* The declaration in a case that is not run copies nothing. */
    CHECK_TEXT(e, "function s(x) { switch (x) { case 1: break; default: function sf() { return 4 } } return typeof sf } s(1)",
               "undefined");
    CHECK_TEXT(e, "function s2(x) { switch (x) { case 1: return typeof sf2; default: function sf2() {} } } s2(1)",
               "function");
    CHECK_TEXT(e, "function ie() { if (true) function a1() { return 2 } return typeof a1 } ie()", "function");
    CHECK_TEXT(e, "function wf() { var wo = {}; with (wo) { function wq() { return 1 } } return typeof wq } wf()",
               "function");
    CHECK_NUM(e, "eval('{ function ev() { return 6 } } ev()')", 6);
    CHECK_TEXT(e, "eval('{ function ev2() { return 1 } }'); typeof ev2", "function");
    /* Block functions in loops, finally blocks, labelled blocks, and blocks that refer to each other. */
    CHECK_NUM(e, "function lp() { var n = 0; for (var i = 0; i < 3; i++) { function h() { return i } n += h() } return n } lp()", 3);
    CHECK_NUM(e, "function tf() { try { return 1 } finally { function fi() {} } } tf()", 1);
    CHECK_NUM(e, "function lb() { var r = 0; l: { function lbf() { return 2 } r = lbf(); break l; } return r } lb()", 2);
    CHECK_TEXT(e, "function cb() { var out = ''; { function a() { return 'a' } function b() { return a() + 'b' } out = b() } return out } cb()", "ab");
}

/* Strict code: block functions are block-scoped and do not reach the function. */
EMB_TEST(es5_strict_block_functions_are_block_scoped)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e, "'use strict'; { function sb() { return 1 } } typeof sb", "undefined");
    CHECK_NUM(e, "function s() { 'use strict'; var r = 0; { function h() { return 5 } r = h() } return r } s()", 5);
    CHECK_TEXT(e, "function s2() { 'use strict'; { function h2() {} } return typeof h2 } s2()", "undefined");
    env_free(e);
}

/* Closures over outer and counter state. */
static void nested_cases(env e)
{
    CHECK_NUM(e, "var o = {a: 1}; var fns = []; function mk() { return function () { return o.a } } mk()()", 1);
    /* for-in with a member target: the target is evaluated for every key (ES5 12.6.4). */
    CHECK_TEXT(e, "var i = 0, a = {}; for (a[i++] in {p: 1, q: 2}) {} a[0] + a[1] + i", "pq2");
    CHECK_TEXT(e, "var o = {}; for (o.k in {a: 1, b: 2}) {} o.k", "b");
    CHECK_NUM(e, "var n = 0, t = {}; for (t[n++] in {x: 1, y: 2, z: 3}) { if (n == 2) break; } n", 2);
    CHECK_TEXT(e, "var i2 = 0, b2 = {}; for (b2[i2++] in {p: 1, q: 2, r: 3}) { if (i2 == 2) continue; } b2[0] + b2[1] + b2[2] + i2",
               "pqr3");
    CHECK_TEXT(e, "function fs() { var a = {}; var i = 0; for (a[i++] in {p: 1, q: 2}) {} return a[0] + a[1] + i } fs()",
               "pq2");
    CHECK_TEXT(e, "function fo() { var a = {}; var i = 0; var s = 0; for (a[i++ * 2] in {p: 1}) { s = i } return s + ':' + a[0] } fo()",
               "1:p");
    CHECK_NUM(e, "function cl() { var n = 0; var inc = function () { n++; return n }; inc(); inc(); return inc() } cl()",
              3);
}

static void all_scope_cases(env e)
{
    with_cases(e);
    eval_cases(e);
    arguments_cases(e);
    annexb_cases(e);
    nested_cases(e);
}

EMB_TEST(es5_with_eval_arguments_annexb)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    all_scope_cases(e);
    env_free(e);
}

/* The same cases with the collector running at every allocation check. */
EMB_TEST(es5_gc_stress_scope_eval_arguments_annexb)
{
    env e = env_new_gc_stress();
    EMB_REQUIRE(e.r != nullptr);
    all_scope_cases(e);
    env_free(e);
}

/* Arguments objects that escape their call are traced and finalized when collections run while
 * they are being changed: new properties, delete, and the define and delete internal methods. */
static embi_js_object *global_object_value(env e, const char *name)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    return embi_js_is_object(v) ? embi_js_as_object(v) : nullptr;
}

EMB_TEST(es5_gc_stress_escaped_arguments)
{
    env e = env_new_gc_stress();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var A = (function (a, b) { return arguments })(1, 2); A.x = 1; A[0]", 1);
    CHECK_NUM(e, "A.y = 5; A.z = 6; delete A.y; A[1] + A.z", 8);
    CHECK_NUM(e, "var g = {}; for (var i = 0; i < 50; i++) { g = {k: i} } A[0] + A.x", 2);
    /* Define with a value: the parameter box and the stored property both follow. */
    CHECK_NUM(e, "var B = (function (a) { return arguments })(1); B.length", 1);
    embi_js_object *b = global_object_value(e, "B");
    EMB_REQUIRE(b != nullptr);
    embi_js_pdesc pd = {embi_js_number(7), EMBI_JS_EMPTY, EMBI_JS_EMPTY, EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(e.rt, b, embi_js_key_from_index(0), &pd) == EMBI_JS_YES);
    CHECK_NUM(e, "B[0]", 7);
    /* Making the index non-writable unmaps it: the stored value (the last write) is kept. */
    CHECK_NUM(e, "var C = (function (a) { a = 7; return arguments })(1); C[0]", 7);
    embi_js_object *c = global_object_value(e, "C");
    EMB_REQUIRE(c != nullptr);
    embi_js_pdesc ro = {EMBI_JS_EMPTY, EMBI_JS_EMPTY, EMBI_JS_EMPTY, EMBI_JS_PD_WRITABLE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(e.rt, c, embi_js_key_from_index(0), &ro) == EMBI_JS_YES);
    CHECK_NUM(e, "C[0]", 7);
    /* Deleting an index unmaps it; the object keeps its other properties. */
    CHECK_NUM(e, "var D = (function (a) { return arguments })(1); D.q = 3; D.length", 1);
    embi_js_object *d = global_object_value(e, "D");
    EMB_REQUIRE(d != nullptr);
    EMB_CHECK(embi_js_obj_delete(e.rt, d, embi_js_key_from_index(0)) == EMBI_JS_YES);
    CHECK_TEXT(e, "typeof D[0] + ' ' + D.q + ' ' + D.length", "undefined 3 1");
    env_free(e);
}

/* let and const (block scope, TDZ, per-iteration bindings), default and rest parameters. */
EMB_TEST(es6_lexical_and_params)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var a = 1; { let a = 2; } a", 1);
    CHECK_NUM(e, "{ let b = 3; { let b = 4; } b }", 3);
    CHECK_NUM(e, "{ const c = 5; c }", 5);
    CHECK_THROWS(e, "x; let x = 1;", "ReferenceError");
    CHECK_THROWS(e, "const k = 1; k = 2;", "TypeError");
    CHECK_NUM(e, "var fs = []; for (let i = 0; i < 3; i++) { fs.push(function () { return i }) } fs[0]() + fs[2]()", 2);
    CHECK_NUM(e, "function d(a, b = a + 1) { return b } d(1)", 2);
    CHECK_NUM(e, "function d2(a, b = a + 1) { return b } d2(1, 5)", 5);
    CHECK_NUM(e, "function l(a, b = 1, c) {} l.length", 1);
    CHECK_NUM(e, "function u(a, b = 0) { arguments[0] = 9; return a } u(1)", 1);
    CHECK_NUM(e, "function r(a, ...b) { return b.length } r(1, 2, 3)", 2);
    CHECK_NUM(e, "function r2(...b) { return b[0] } r2(7)", 7);
    CHECK_NUM(e, "(function (...b) { return b.length })()", 0);
    CHECK_NUM(e, "function q(a, ...b) {} q.length", 1);
    env_free(e);
}

EMB_TEST_MAIN()
