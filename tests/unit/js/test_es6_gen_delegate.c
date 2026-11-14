/* yield* (J2c): delegation to generators, arrays, strings and iterables, the value of the delegate,
 * forwarding of next, throw and return, and the close when the inner iterator has no throw. */
#include "es6_util.h"
#include <stdio.h>

EMB_TEST(gen_delegate_values)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* inner() { var x = yield 1; yield x; return 'r'; }"
              "function* outer() { var r = yield* inner(); yield r; }"
              "var it = outer(); var a = it.next().value; var b = it.next(5).value; var c = it.next();"
              "a * 100 + b * 10 + (c.value === 'r' ? 1 : 0)", 151);
    E6_NUM(e, "function* arr() { yield* [1, 2, 3]; } var s = 0; for (var v of arr()) s += v; s", 6);
    E6_TEXT(e, "function* str() { yield* 'ab'; } var t = ''; for (var c of str()) t += c; t", "ab");
    E6_NUM(e, "function* ret() { var got = yield* (function* () { yield 'a'; return 7; })(); yield got; }"
              "var rt = ret(); rt.next(); rt.next().value", 7);
    E6_NUM(e, "var log = []; function* d() { yield* { [Symbol.iterator]() { return { i: 0, next() { return { done: this.i >= 2, value: this.i++ }; } }; } }; }"
              "var dit = d(); dit.next().value + dit.next().value * 10", 10);
    E6_NUM(e, "function* empty() { yield* []; yield 4; } empty().next().value", 4);
    E6_NUM(e, "function* n2() { var v = yield* []; yield v === undefined ? 3 : 0; } n2().next().value", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(gen_delegate_throw_return)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "function* inner() { try { yield 1; } catch (e) { yield 'caught ' + e; } }"
               "function* outer() { yield* inner(); }"
               "var it = outer(); it.next(); it.throw('x').value", "caught x");
    E6_THROWS(e, "function* plain() { yield 1; yield 2; } function* o2() { yield* plain(); }"
                 "var it2 = o2(); it2.next(); it2.throw(new RangeError('q'));", "RangeError");
    E6_NUM(e, "var log = ''; function* inner3() { try { yield 1; } finally { log += 'i'; } }"
              "function* outer3() { try { yield* inner3(); } finally { log += 'o'; } }"
              "var it3 = outer3(); it3.next(); var r3 = it3.return(9); (r3.value === 9 && r3.done ? 10 : 0) + log.length", 12);
    E6_NUM(e, "var log4 = 0; var iter4 = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; },"
              "  return() { log4 = 5; return { value: 3, done: true }; } };"
              "function* o4() { yield* iter4; }"
              "var it4 = o4(); it4.next(); var rr = it4.return(3); log4 * 10 + rr.value", 53);
    E6_THROWS(e, "var iter5 = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; } };"
                 "function* o5() { yield* iter5; }"
                 "var it5 = o5(); it5.next(); it5.throw(1);", "TypeError");
    E6_NUM(e, "function* o6() { yield* [1]; } var it6 = o6(); it6.next(); var r6 = it6.next(); r6.done ? 2 : 0", 2);
    E6_THROWS(e, "var bad = { [Symbol.iterator]() { return { next() { return 1; } }; } };"
                 "function* o7() { yield* bad; } o7().next();", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
