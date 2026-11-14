/* Array destructuring closes its iterator on a return completion (J3e): a yield inside an array
 * pattern that is resumed by return() calls the iterator's return method (IteratorClose), the
 * errors of that call propagate, a non-object result throws a TypeError, and the close runs once
 * (a throw completion closes it through the pattern's handler, a normal exit after the elements). */
#include "es6_util.h"
#include <stdio.h>

#define SRC_SETUP                                                                              \
    "var rc = 0; var itr = { next() { return { done: false, value: undefined }; },"           \
    "  return(v) { rc++; return {}; } };"                                                      \
    "var src = { [Symbol.iterator]() { return itr; } };"

EMB_TEST(dstr_return_close_calls_return)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, SRC_SETUP "function* g() { var [{} = yield] = src; return 'end'; }"
              "var it = g(); it.next(); var r = it.return(5); rc * 10 + (r.value === 5 && r.done ? 1 : 0)",
           11);
    E6_NUM(e, SRC_SETUP "function* g() { var [{} = yield] = src; }"
              "var it = g(); it.next(); it.return(1); rc", 1);
    E6_NUM(e, SRC_SETUP "function* g() { var r; [...{}[yield]] = src; }"
              "var it = g(); it.next(); it.return(1); rc", 1);
    E6_NUM(e, SRC_SETUP "function* g() { for ([{} = yield] of [src]) { } }"
              "var it = g(); it.next(); it.return(1); rc", 1);
    E6_TEXT(e, "var log = ''; var itr2 = { next() { return { done: false, value: undefined }; },"
               "  return() { log += 'r'; return {}; } }; var src2 = { [Symbol.iterator]() { return itr2; } };"
               "function* g() { try { var [{} = yield] = src2; } finally { log += 'f'; } }"
               "var it = g(); it.next(); it.return(1); log", "rf");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(dstr_return_close_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "var itr3 = { next() { return { done: false, value: undefined }; },"
                 "  return() { return 1; } }; var src3 = { [Symbol.iterator]() { return itr3; } };"
                 "function* g() { var [{} = yield] = src3; }"
                 "var it = g(); it.next(); it.return(1);", "TypeError");
    E6_NUM(e, "var rc4 = 0; var itr4 = { next() { return { done: false, value: undefined }; },"
              "  return() { rc4++; throw new RangeError('x'); } };"
              "var src4 = { [Symbol.iterator]() { return itr4; } };"
              "function* g() { var [{} = yield] = src4; }"
              "var it = g(); it.next(); var caught = 0;"
              "try { it.return(1); } catch (err) { caught = err instanceof RangeError ? 1 : 0; }"
              "rc4 * 10 + caught", 11);
    E6_NUM(e, "var itr5 = { next() { return { done: false, value: undefined }; } };"
              "var src5 = { [Symbol.iterator]() { return itr5; } };"
              "function* g() { var [{} = yield] = src5; }"
              "var it = g(); it.next(); it.return(7).value", 7);
    E6_NUM(e, SRC_SETUP "function* g() { var [{} = yield] = src; }"
              "var it = g(); it.next(); try { it.throw(new Error('t')); } catch (err) { } rc", 1);
    E6_NUM(e, SRC_SETUP "function* g() { var [a] = src; yield a; }"
              "var it = g(); it.next(); it.return(2); rc", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
