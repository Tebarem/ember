/* J5n2 memory diet: inline caches are allocated by the first
 * run of their code, source positions are stored delta-coded, a code cell fits the 256-byte size
 * class, and the first out-of-line slot block and element block are sized to the need. These tests
 * check that each of them still gives the same answers. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/ic.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void sbf(embi_sb *sb, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    embi_sb_append(sb, (embi_str){buf, (uint32_t)n});
}

static const embi_js_code *code_of_global_fn(es6_env e, const char *name)
{
    embi_js_value f = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    if (!embi_js_is_object(f) || embi_js_as_object(f)->klass != &embi_js_class_closure) {
        return nullptr;
    }
    const embi_js_closure *cl = embi_js_as_object(f)->internal;
    return (const embi_js_code *)embi_js_as_cell(cl->code);
}

EMB_TEST(js_memdiet_code_cell_fits_256)
{
    EMB_CHECK(sizeof(embi_js_code) <= 256);
    EMB_CHECK(sizeof(embi_js_ic) <= 64);
}

/* A function that has not run owns no site array; the first call allocates it, and a second
 * function is unaffected. */
EMB_TEST(js_memdiet_ics_allocated_on_first_run)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function f(o) { return o.a + o.b; } function g(o) { return o.c; } 1", 1);
    const embi_js_code *f = code_of_global_fn(e, "f");
    const embi_js_code *g = code_of_global_fn(e, "g");
    EMB_REQUIRE(f != nullptr && g != nullptr);
    EMB_CHECK(f->nics >= 2 && g->nics >= 1);
    EMB_CHECK(f->ics == nullptr && g->ics == nullptr);
    EMB_CHECK(f->ic_keys != nullptr);
    E6_NUM(e, "f({a: 1, b: 2})", 3);
    EMB_CHECK(f->ics != nullptr);
    EMB_CHECK(g->ics == nullptr);
    /* The sites work: a second call hits, a different shape misses and still answers. */
    E6_NUM(e, "f({a: 4, b: 5}) + f({b: 1, a: 2, z: 0})", 12);
    /* A collection frees nothing that is still in use, then the unused code is collected whole. */
    embi_js_gc(e.rt);
    E6_NUM(e, "f({a: 10, b: 20})", 30);
    E6_NUM(e, "g({c: 7})", 7);
    EMB_CHECK(g->ics != nullptr);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Global sites share storage with the field-entry record (isglob says which is live). Globals read,
 * written and shadowed across collections must keep working. */
EMB_TEST(js_memdiet_global_sites_share_the_record)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var gv = 5; function rd() { return gv + Math.abs(-1); }"
              "function wr(x) { gv = x; }"
              "var s = 0; for (var i = 0; i < 50; i++) { wr(i); s += rd(); } s", 1225 + 50);
    for (int i = 0; i < 3; i++) {
        embi_js_gc(e.rt);
    }
    E6_NUM(e, "let gv2 = 3; function rd2() { return gv2; } var t = 0;"
              "for (var i = 0; i < 20; i++) { t += rd2(); } gv2 = 4; t + rd2()", 64);
    /* A later `let` shadows a global property that a site cached. */
    E6_NUM(e, "globalThis.sh = 1; function rs() { return sh; } rs(); rs();", 1);
    E6_NUM(e, "let sh = 2; rs()", 2);
    es6_env_free(e);
}

/* Source positions: the stack of an error names the right line for statements far down a long
 * script and for code the compiler emits out of source order (a for-loop update comes after the
 * body). */
EMB_TEST(js_memdiet_positions_survive_the_delta_code)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_sb sb = {nullptr, 0, 0};
    sbf(&sb, "function g() { try { throw new Error('x'); } catch (e) { return e.stack; } }\n");
    sbf(&sb, "var st = [];\n");
    sbf(&sb, "for (var i = 0; i < 2; i++,\n");
    sbf(&sb, "     st.push(g())) {\n");
    sbf(&sb, "  st.push(g());\n");
    sbf(&sb, "}\n");
    for (int i = 0; i < 400; i++) {
        sbf(&sb, "var pad%d = %d;\n", i, i);
    }
    sbf(&sb, "st.push(g());\n");
    sbf(&sb, "st.join('|')\n");
    embi_js_value v = embi_js_eval(e.r, embi_sb_cstr(&sb), sb.len, "pos.js", 0);
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    embi_sb out = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &out);
    const char *text = embi_sb_cstr(&out);
    /* Body on line 5, update on line 4 (twice each: the body runs, then the update), pad on 409. */
    EMB_CHECK(strstr(text, "pos.js:5:") != nullptr);
    EMB_CHECK(strstr(text, "pos.js:4:") != nullptr);
    EMB_CHECK(strstr(text, "pos.js:407:") != nullptr);
    /* The function g itself is on line 1. */
    EMB_CHECK(strstr(text, "pos.js:1:") != nullptr);
    embi_sb_free(&out);
    embi_sb_free(&sb);
    es6_env_free(e);
}

/* Object slot blocks (first block sized to the need, then doubling) and element blocks (first block
 * the exact need) keep their values through growth, a switch to dictionary mode and a collection. */
EMB_TEST(js_memdiet_storage_growth_keeps_values)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var ok = 1;"
              "for (var n = 1; n <= 80; n++) { var o = {}; for (var k = 0; k < n; k++) { o['p' + k] = k * 3; }"
              "  for (var k = 0; k < n; k++) { if (o['p' + k] !== k * 3) { ok = 0; } } }"
              "ok", 1);
    E6_NUM(e, "var ok2 = 1;"
              "for (var n = 0; n <= 120; n++) { var a = [1, 2, 3, 4, 5].slice(0, Math.min(n, 5));"
              "  for (var k = a.length; k < n; k++) { a.push(k + 1); }"
              "  if (a.length !== n) { ok2 = 0; }"
              "  for (var k = 0; k < n; k++) { if (a[k] !== k + 1) { ok2 = 0; } } }"
              "ok2", 1);
    E6_NUM(e, "var h = []; h[9] = 1; h[2] = 2; h.length === 10 && h[9] === 1 && h[2] === 2 && !(5 in h) ? 1 : 0", 1);
    for (int i = 0; i < 3; i++) {
        embi_js_gc(e.rt);
    }
    E6_NUM(e, "var lit = [10, 20, 30]; lit.push(40, 50, 60); lit.length * 1000 + lit[5]", 6060);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A shape table belongs to one node: a child takes its parent's. Objects at every intermediate shape
 * of a 40-property chain keep answering their own lookups, in any order, across collections. */
EMB_TEST(js_memdiet_shape_tables_move_down_the_chain)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var objs = []; var o = {};"
              "for (var k = 0; k < 40; k++) { o['k' + k] = k; objs.push(Object.assign({}, o)); }"
              "var bad = 0;"
              "function chk(i) { var x = objs[i];"
              "  for (var j = 0; j < 40; j++) { var v = x['k' + j];"
              "    if (j <= i ? v !== j : v !== undefined) { bad++; } } }"
              "for (var i = 39; i >= 0; i -= 3) { chk(i); }"
              "for (var i = 0; i < 40; i += 2) { chk(i); }"
              "for (var i = 0; i < 40; i++) { chk(i); chk(39 - i); }"
              "bad", 0);
    for (int i = 0; i < 3; i++) {
        embi_js_gc(e.rt);
    }
    E6_NUM(e, "bad = 0; for (var i = 0; i < 40; i++) { chk(i); } bad", 0);
    E6_NUM(e, "objs[10].k10 = 99; objs[30].k30 = 77; objs[10].k10 + objs[30].k30 + objs[35].k35", 211);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
