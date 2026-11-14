#include "emb_test.h"

#include "base/base.h"
#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/js.h"

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

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

/* True when the pending exception is an object whose name property equals want. */
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

static embi_js_value compile(env e, const char *src, const char *file)
{
    return embi_js_compile(e.r, (const uint8_t *)src, (uint32_t)strlen(src), file, false);
}

EMB_TEST(compile_accepts_functions_and_closures)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var x = 1; function f(a) { return a + x; } f(2);";
    embi_js_value code = compile(e, src, "t.js");
    EMB_CHECK(code != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_cell(code));
    env_free(e);
}

EMB_TEST(compile_accepts_strict_directives)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(compile(e, "'use strict'; var q = 1;", "s.js") != EMBI_JS_EXCEPTION);
    EMB_CHECK(compile(e, "function g() { 'use strict'; return 2; }", "s.js") != EMBI_JS_EXCEPTION);
    env_free(e);
}

EMB_TEST(compile_reports_syntax_error_with_location)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_value code = compile(e, "var a = 1;\nvar = 2;\n", "bad.js");
    EMB_CHECK(code == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    EMB_REQUIRE(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 2);
    EMB_REQUIRE(file != nullptr);
    EMB_CHECK_EQ_STR(file, "bad.js");
    embi_js_exception_clear(e.rt);
    env_free(e);
}

/* ------------------------------------------------------------------ operand stack verifier */

/* Records pc at depth d unless it already has one. Returns an error message on a mismatch. */
static const char *visit(uint32_t pc, int32_t d, uint32_t n, int32_t *dep, uint32_t *work,
                         uint32_t *nwork)
{
    if (pc >= n) {
        return "jump target outside the bytecode";
    }
    if (dep[pc] < 0) {
        dep[pc] = d;
        work[(*nwork)++] = pc;
        return nullptr;
    }
    return dep[pc] == d ? nullptr : "operand stack depth differs between paths";
}

/* Walks every path of one CODE cell with the stack effects from opcodes.def and checks the
 * compiler's depth model: every join point has one depth, nothing underflows, a handler is
 * entered at its recorded depth plus the exception, and the peak depth fits in max_stack. Returns
 * nullptr when the code is consistent. */
static const char *verify_code(const embi_js_code *c, int32_t *peak_out)
{
    const uint8_t *bc = c->bc;
    uint32_t n = c->bc_len;
    int32_t *dep = embi_alloc(sizeof(int32_t) * (n ? n : 1u));
    uint32_t *work = embi_alloc(sizeof(uint32_t) * (n ? n : 1u));
    for (uint32_t i = 0; i < n; i++) {
        dep[i] = -1;
    }
    uint32_t nwork = 0;
    int32_t peak = 0;
    const char *err = nullptr;

    err = visit(0, 0, n, dep, work, &nwork);
    for (uint32_t i = 0; !err && i < c->nexc; i++) {
        err = visit(c->exc[i].handler, (int32_t)c->exc[i].depth + 1, n, dep, work, &nwork);
    }
    while (!err && nwork > 0) {
        uint32_t pc = work[--nwork];
        int32_t d = dep[pc];
        uint8_t op = bc[pc];
        if (op >= EMBI_JSOP_COUNT) {
            err = "unknown opcode";
            break;
        }
        if (op == EMBI_JSOP_SYM_GET || op == EMBI_JSOP_SYM_SET || op == EMBI_JSOP_SYM_INIT ||
            op == EMBI_JSOP_SYM_TYPEOF || op == EMBI_JSOP_SYM_DELETE) {
            err = "symbolic op survived the resolve pass";
            break;
        }
        const embi_js_op_info *info = &embi_js_op_table[op];
        if (pc + info->size > n) {
            err = "truncated operand";
            break;
        }
        int32_t pops = info->pops;
        int32_t pushes = info->pushes;
        if (op == EMBI_JSOP_CALL) {
            pops = (int32_t)EMBI_JS_CALL_ARGC(embi_js_rd32(bc + pc + 1)) + 2;
            pushes = 1;
        } else if (op == EMBI_JSOP_NEW) {
            pops = (int32_t)embi_js_rd32(bc + pc + 1) + 1;
            pushes = 1;
        }
        if (d < pops) {
            err = "operand stack underflow";
            break;
        }
        int32_t after = d - pops + pushes;
        if (d > peak) {
            peak = d;
        }
        if (after > peak) {
            peak = after;
        }
        for (uint32_t i = 0; i < c->nexc; i++) {
            const embi_js_exc_entry *e = &c->exc[i];
            if (pc >= e->start && pc < e->end && d < (int32_t)e->depth) {
                err = "stack below the base of an enclosing try";
            }
        }
        if (err) {
            break;
        }
        uint32_t next = pc + info->size;
        uint32_t target = (op == EMBI_JSOP_JMP || op == EMBI_JSOP_JMP_ENTRY || op == EMBI_JSOP_SWITCH_JMP ||
                           op == EMBI_JSOP_JMP_TRUE || op == EMBI_JSOP_JMP_FALSE ||
                           op == EMBI_JSOP_JMP_TRUE_KEEP || op == EMBI_JSOP_JMP_FALSE_KEEP ||
                           op == EMBI_JSOP_FOR_IN_NEXT)
                              ? embi_js_rd32(bc + pc + 1)
                              : 0;
        switch (op) {
        case EMBI_JSOP_RETURN:
        case EMBI_JSOP_THROW:
        case EMBI_JSOP_RETHROW:
            break;
        case EMBI_JSOP_JMP:
        case EMBI_JSOP_JMP_ENTRY:
        case EMBI_JSOP_SWITCH_JMP:
            /* SWITCH_JMP (CF1) is a JMP to the first case test: skipping tests only takes edges those tests have */
            err = visit(target, after, n, dep, work, &nwork);
            break;
        case EMBI_JSOP_JMP_TRUE:
        case EMBI_JSOP_JMP_FALSE:
            err = visit(target, after, n, dep, work, &nwork);
            if (!err) {
                err = visit(next, after, n, dep, work, &nwork);
            }
            break;
        case EMBI_JSOP_JMP_TRUE_KEEP:
        case EMBI_JSOP_JMP_FALSE_KEEP:
            /* Taken: the operand stays. Not taken: it is popped. */
            err = visit(target, d, n, dep, work, &nwork);
            if (!err) {
                if (d < 1) {
                    err = "operand stack underflow on a logical fall-through";
                } else {
                    err = visit(next, d - 1, n, dep, work, &nwork);
                }
            }
            break;
        case EMBI_JSOP_FOR_IN_NEXT:
            /* Exhausted: no key is pushed. Otherwise the key is pushed. */
            err = visit(target, d, n, dep, work, &nwork);
            if (!err) {
                err = visit(next, d + 1, n, dep, work, &nwork);
            }
            break;
        default:
            err = visit(next, after, n, dep, work, &nwork);
            break;
        }
        if (peak > peak_out[0]) {
            peak_out[0] = peak;
        }
    }
    if (!err && peak > (int32_t)c->max_stack - 2) {
        err = "peak operand depth exceeds max_stack";
    }
    embi_free(dep, sizeof(int32_t) * (n ? n : 1u));
    embi_free(work, sizeof(uint32_t) * (n ? n : 1u));
    return err;
}

/* Verifies a CODE cell and every function nested in it. */
static const char *verify_tree(const embi_js_code *c, int32_t *peak)
{
    const char *err = verify_code(c, peak);
    for (uint32_t i = 0; !err && i < c->nconsts; i++) {
        embi_js_value v = c->consts[i];
        if (embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_CODE) {
            err = verify_tree((const embi_js_code *)embi_js_as_cell(v), peak);
        }
    }
    return err;
}

static const char *const verify_sources[] = {
    /* try inside for-in and switch, with finally exits */
    "var t = 0; for (var k in {a: 1, b: 2}) { try { t += 1; throw k; } catch (e) { t += 2; }"
    " finally { t += 3; } } t",
    "var n = 0; for (var k in {q: 1}) { switch (k) { case 'q': try { n++; throw 1; }"
    " catch (x) { n += 2; } break; default: n = 9; } } n",
    "var hits = 0; outer: for (var i = 0; i < 3; i++) { for (var j in {x: 1}) { try {"
    " if (i == 1) continue outer; hits++; } finally { hits += 10; } } } hits",
    "function f(a) { try { return a; } finally { var z = 2; } } var r = f(3) || 0; r ? 1 : 2",
    "var c = 0; do { try { c++; if (c > 2) break; } finally { c += 0; } } while (c < 9); c",
    "function mk() { var n = 0; return function() { return ++n + (n && 1); }; } mk()()",
    "var fn = function f() { f = 1; return typeof f; }; fn()",
    "try { throw 1; } catch (e) { function g() { return e; } } g()",
    "'use strict'; function f() { return 1; } f()",
    "var o = {a: 1, b: 2}; var s = 0; for (var k in o) { switch (o[k]) { case 1: s += 1;"
    " break; default: s = 2; } } s",
    "var o = {}; var ks = 'a'; for (o.x in {p: 1, q: 2}) { o[ks] = o.x; } for (o[ks] in {r: 1}) {}"
    " o.a",
    "function g(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17,"
    " p18, p19) { return p0 + p19; } function f() { for (var a in {x: 1}) for (var b in {x: 1})"
    " for (var c in {x: 1}) for (var d in {x: 1}) { try {} catch (e) {} try {} catch (e) {}"
    " try {} catch (e) {} return g(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,"
    " 18, 19); } } f()",
};

EMB_TEST(compile_stack_depth_matches_bytecode)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    for (uint32_t i = 0; i < sizeof verify_sources / sizeof verify_sources[0]; i++) {
        embi_js_value code = compile(e, verify_sources[i], "verify.js");
        EMB_REQUIRE(code != EMBI_JS_EXCEPTION);
        int32_t peak = 0;
        const char *err = verify_tree((const embi_js_code *)embi_js_as_cell(code), &peak);
        if (err) {
            SDL_Log("verify source %u: %s", i, err);
        }
        EMB_CHECK(err == nullptr);
        EMB_CHECK(peak > 0);
    }
    env_free(e);
}

/* ------------------------------------------------------------------ nesting limit */

static bool exc_is_one_of(embi_js_runtime *rt, const char *a, const char *b)
{
    return exc_name_is(rt, a) || exc_name_is(rt, b);
}

/* Builds "open" repeated n times, then tail, then close repeated n times. */
static void build_nest(char *out, size_t cap, const char *open, const char *mid, const char *close,
                       unsigned n)
{
    size_t len = 0;
    size_t ol = strlen(open);
    size_t cl = strlen(close);
    for (unsigned i = 0; i < n && len + ol + 1 < cap; i++) {
        memcpy(out + len, open, ol);
        len += ol;
    }
    size_t ml = strlen(mid);
    memcpy(out + len, mid, ml + 1u);
    len += ml;
    for (unsigned i = 0; i < n && len + cl + 1 < cap; i++) {
        memcpy(out + len, close, cl);
        len += cl;
    }
    out[len] = '\0';
}

EMB_TEST(compile_deep_nesting_is_a_syntax_or_range_error)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Each source nests 100000 levels: the parser must stop with an error, not overflow. */
    static char buf[1u << 20];
    struct {
        const char *open;
        const char *mid;
        const char *close;
    } shapes[] = {
        {"(", "1", ")"},
        {"[", "1", "]"},
        {"function f(){", "", "}"},
        {"!", "0", ""},
        {"{", "", "}"},
        {"new ", "x", ""},
    };
    for (uint32_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        build_nest(buf, sizeof buf, shapes[i].open, shapes[i].mid, shapes[i].close, 100000u);
        embi_js_value code = embi_js_compile(e.r, (const uint8_t *)buf,
                                             (uint32_t)strlen(buf), "deep.js", false);
        EMB_CHECK(code == EMBI_JS_EXCEPTION);
        EMB_CHECK(exc_is_one_of(e.rt, "SyntaxError", "RangeError"));
        embi_js_exception_clear(e.rt);
    }
    /* Modest nesting still compiles. */
    build_nest(buf, sizeof buf, "(", "1", ")", 60u);
    EMB_CHECK(embi_js_compile(e.r, (const uint8_t *)buf, (uint32_t)strlen(buf), "ok.js", false) !=
              EMBI_JS_EXCEPTION);
    env_free(e);
}

EMB_TEST(compile_disasm_lists_operations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_value code = compile(e, "var s = 0; s = s + 40;", "d.js");
    EMB_REQUIRE(code != EMBI_JS_EXCEPTION);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_disasm((const embi_js_code *)embi_js_as_cell(code), &sb);
    const char *text = embi_sb_cstr(&sb);
    EMB_CHECK(strstr(text, "ADD") != nullptr);
    EMB_CHECK(strstr(text, "RETURN") != nullptr);
    embi_sb_free(&sb);
    env_free(e);
}

EMB_TEST_MAIN()
