/* Global lexical declarations (J2a section 1): script-level let and const live in a declarative
 * record shared by every script of the realm, with the GlobalDeclarationInstantiation collision
 * checks. Every test runs several scripts through embi_js_eval on one realm. */
#include "es6_util.h"

/* A binding made by one script is visible to the next, and it is not a property of the global. */
EMB_TEST(es6_global_lex_shared_across_scripts)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "let gl_a = 1; gl_a", 1);
    E6_NUM(e, "gl_a + 1", 2);
    E6_NUM(e, "gl_a = 5; gl_a", 5);
    E6_NUM(e, "function gl_get() { return gl_a } gl_get()", 5);
    E6_NUM(e, "gl_a = 7; gl_get()", 7);
    E6_BOOL(e, "typeof globalThis.gl_a === 'undefined'", true);
    E6_BOOL(e, "Object.getOwnPropertyDescriptor(globalThis, 'gl_a') === undefined", true);
    E6_BOOL(e, "this.gl_a === undefined && 'gl_a' in this === false", true);
    /* A closure created by an earlier script keeps working in a later one. */
    E6_NUM(e, "var gl_f = () => gl_a * 2; 0", 0);
    E6_NUM(e, "gl_a = 4; gl_f()", 8);
    es6_env_free(e);
}

/* const: a later script cannot assign to it, and the failed assignment keeps the value. */
EMB_TEST(es6_global_const_is_immutable)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "const gc_k = 1; gc_k", 1);
    E6_THROWS(e, "gc_k = 2", "TypeError");
    E6_THROWS(e, "gc_k++", "TypeError");
    E6_THROWS(e, "gc_k += 1", "TypeError");
    E6_THROWS(e, "(function () { 'use strict'; gc_k = 3 })()", "TypeError");
    E6_THROWS(e, "(function () { gc_k = 3 })()", "TypeError");
    E6_NUM(e, "gc_k", 1);
    E6_BOOL(e, "delete globalThis.gc_k", true);
    E6_NUM(e, "gc_k", 1);
    es6_env_free(e);
}

/* Redeclaration across scripts is a SyntaxError, and a failed script declares nothing. */
EMB_TEST(es6_global_lex_collisions)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "let rc_a = 1; rc_a", 1);
    E6_SYNTAX(e, "let rc_a = 2;");
    E6_SYNTAX(e, "const rc_a = 2;");
    E6_SYNTAX(e, "var rc_a;");
    E6_SYNTAX(e, "function rc_a() {}");
    E6_NUM(e, "rc_a", 1);
    /* var and function first, lexical second. */
    E6_NUM(e, "var rc_v = 1; function rc_f() {} 0", 0);
    E6_SYNTAX(e, "let rc_v;");
    E6_SYNTAX(e, "const rc_f = 1;");
    /* The whole script is checked before it declares anything. */
    E6_SYNTAX(e, "let rc_fresh = 1; let rc_a;");
    E6_THROWS(e, "rc_fresh", "ReferenceError");
    E6_NUM(e, "let rc_fresh = 3; rc_fresh", 3);
    /* A non-configurable property of the global object, and a configurable one. */
    E6_NUM(e, "Object.defineProperty(globalThis, 'rc_nc', {value: 1, configurable: false}); 0", 0);
    E6_SYNTAX(e, "let rc_nc;");
    E6_NUM(e, "globalThis.rc_cfg = 5; 0", 0);
    E6_NUM(e, "let rc_cfg = 6; rc_cfg", 6);
    E6_NUM(e, "globalThis.rc_cfg", 5);
    E6_SYNTAX(e, "let undefined;");
    E6_SYNTAX(e, "let NaN;");
    es6_env_free(e);
}

/* Reading a global lexical binding before its declaration has run is a TDZ ReferenceError. */
EMB_TEST(es6_global_lex_tdz)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "gt_early(); let gt_a = 1; function gt_early() { return gt_a }", "ReferenceError");
    /* The binding exists after the failed script, still uninitialized. */
    E6_THROWS(e, "gt_a", "ReferenceError");
    E6_THROWS(e, "typeof gt_a", "ReferenceError");
    E6_THROWS(e, "gt_a = 3", "ReferenceError");
    E6_SYNTAX(e, "let gt_a;");
    E6_THROWS(e, "gt_x; let gt_x;", "ReferenceError");
    E6_THROWS(e, "let gt_y = gt_y;", "ReferenceError");
    E6_THROWS(e, "const gt_z = (gt_z = 1);", "ReferenceError");
    E6_TEXT(e, "typeof gt_undeclared", "undefined");
    es6_env_free(e);
}

/* var and lexical names of one realm: the order of scripts decides which is an error. */
EMB_TEST(es6_global_lex_var_order)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "let vo_l = 1; 0", 0);
    E6_SYNTAX(e, "var vo_l = 2;");
    E6_NUM(e, "eval('1')", 1);
    /* Direct eval declares in its own lexical environment; its var conflicts with the record. */
    E6_SYNTAX(e, "eval('var vo_l')");
    E6_NUM(e, "eval('let vo_l = 9; vo_l')", 9);
    E6_NUM(e, "vo_l", 1);
    E6_SYNTAX(e, "(0, eval)('var vo_l')");
    /* Indirect eval makes a deletable global var: a later let of the same name is allowed. */
    E6_BOOL(e, "(0, eval)('var vo_ev = 3'); delete globalThis.vo_ev", true);
    E6_NUM(e, "let vo_ev = 4; vo_ev", 4);
    es6_env_free(e);
}

/* Functions declared in a block and lexical names of blocks do not enter the global record. */
EMB_TEST(es6_global_block_scopes_stay_local)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "{ let bl_a = 1; const bl_b = 2; } 0", 0);
    E6_THROWS(e, "bl_a", "ReferenceError");
    E6_NUM(e, "{ let bl_a = 5; } let bl_a = 6; bl_a", 6);
    E6_NUM(e, "for (let bl_i = 0; bl_i < 3; bl_i++) {} typeof bl_i === 'undefined' ? 1 : 0", 1);
    E6_NUM(e, "let bl_ok = 1; { let bl_ok = 2; } bl_ok", 1);
    es6_env_free(e);
}

EMB_TEST_MAIN()
