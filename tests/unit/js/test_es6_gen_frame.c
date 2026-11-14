/* Generator frames (J2c, interp.c design note): the saved slots grow from the live part at the
 * first yield that needs more room, so a yield with a deep operand stack keeps every value, and
 * repeated suspensions keep working. The live memory returns to its baseline after the generators
 * are dropped. */
#include "es6_util.h"

EMB_TEST(gen_frame_deep_operand_stack)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* The array literal holds ten values on the operand stack when the first yield saves the
     * frame, which is more than a parameterless generator starts with. */
    EMB_CHECK(es6_run(e, "function* g() { return [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, yield 'a', yield 'b']; }") !=
              EMBI_JS_EXCEPTION);
    E6_NUM(e, "var it = g(); it.next(); it.next(10); var rr = it.next(11); var s = 0;"
              "for (var q of rr.value) s += q; s + (rr.done ? 1000 : 0)", 1066);
    E6_NUM(e, "var it2 = g(); var first = it2.next().value; first === 'a' ? 1 : 0", 1);
    /* A parameter and a local survive a yield, and the body runs on after it. */
    E6_NUM(e, "function* p(a, b) { var c = a + b; yield c; return c * 2; } var pi = p(3, 4);"
              "pi.next().value * 10 + pi.next().value", 84);
    /* A generator that is never resumed drops its frame without a leak. */
    E6_NUM(e, "var dropped = g(); dropped.next(); 1", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(gen_frame_many_generators)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, "function* c(n) { var i = 0; while (i < n) { yield i * 2; i++; } }") !=
              EMBI_JS_EXCEPTION);
    /* Many suspended generators at once, each with its own saved frame. */
    E6_NUM(e, "var gs = []; for (var k = 0; k < 200; k++) { var gg = c(3); gg.next(); gs.push(gg); }"
              "var tot = 0; for (var j = 0; j < gs.length; j++) tot += gs[j].next().value; tot", 400);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
