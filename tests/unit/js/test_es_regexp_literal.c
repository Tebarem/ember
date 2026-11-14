#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"

#include <string.h>

/* RegExp literals (J4c): the parser's regex token, early errors at parse time, and one fresh
 * RegExp object per evaluation that shares the compiled program. */

typedef struct lit_env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} lit_env;

static lit_env lit_new(void)
{
    lit_env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void lit_free(lit_env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool lit_true(lit_env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "literal.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

/* The source must fail to parse or run with a SyntaxError (an early error). */
static bool lit_syntax_error(lit_env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "literal.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected SyntaxError: %s", src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "name"));
        ok = embi_js_is_string(n) && embi_js_str_len(embi_js_as_string(n)) == 11 &&
             embi_js_str_at(embi_js_as_string(n), 0) == 'S';
    }
    embi_js_exception_clear(e.rt);
    if (!ok) {
        SDL_Log("    wrong exception for: %s", src);
    }
    return ok;
}

EMB_TEST(regexp_literal_basic_values)
{
    size_t mem0 = embi_mem_live();
    lit_env e = lit_new();
    EMB_CHECK(lit_true(e, "/a\\/b/.source === 'a\\\\/b'"));
    EMB_CHECK(lit_true(e, "/ab+c/gi.flags === 'gi'"));
    EMB_CHECK(lit_true(e, "/x/.test('axb')"));
    EMB_CHECK(lit_true(e, "var a = 6 / 2 / 3; a === 1"));
    EMB_CHECK(lit_true(e, "var b = [1, 2].map(function (x) { return /\\d/.test(String(x)); }); b[0] && b[1]"));
    EMB_CHECK(lit_true(e, "/[/]/.source === '[/]' && /[/]/.test('/')"));
    EMB_CHECK(lit_true(e, "/a/ instanceof RegExp"));
    EMB_CHECK(lit_true(e, "/\\u{61}/u.test('a') && /\\//.test('/')"));
    EMB_CHECK(lit_true(e, "(/a/).source === 'a' && typeof /a/ === 'object'"));
    EMB_CHECK(lit_true(e, "var q = 1; q /2/ 1 === 0.5"));
    lit_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_literal_fresh_object_per_evaluation)
{
    size_t mem0 = embi_mem_live();
    lit_env e = lit_new();
    EMB_CHECK(lit_true(e,
        "function mk() { return /x/g; }"
        "var r1 = mk(), r2 = mk();"
        "r1 !== r2 && r1.source === r2.source && r1.lastIndex === 0"));
    EMB_CHECK(lit_true(e,
        "var r3 = mk(); r3.lastIndex = 5; mk().lastIndex === 0"));
    EMB_CHECK(lit_true(e,
        "var out = []; for (var i = 0; i < 3; i++) { var z = /q/; z.marker = i; out.push(z); }"
        "out[0] !== out[1] && out[1].marker === 1 && out[0].marker === 0"));
    lit_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_literal_early_errors)
{
    size_t mem0 = embi_mem_live();
    lit_env e = lit_new();
    EMB_CHECK(lit_syntax_error(e, "var x = /(/;"));
    EMB_CHECK(lit_syntax_error(e, "/a/gg;"));
    EMB_CHECK(lit_syntax_error(e, "/a/uv;"));
    EMB_CHECK(lit_syntax_error(e, "var y = /[z-a]/;"));
    EMB_CHECK(lit_syntax_error(e, "/(?<n>a)(?<n>b)/;"));
    EMB_CHECK(lit_syntax_error(e, "/\\p{NotAProp}/u;"));
    EMB_CHECK(lit_syntax_error(e, "x = /unterminated"));
    lit_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_literal_in_operand_position_only)
{
    size_t mem0 = embi_mem_live();
    lit_env e = lit_new();
    EMB_CHECK(lit_true(e, "var n = 10; var d = n / 5 / 2; d === 1"));
    EMB_CHECK(lit_true(e, "var t = [1,2,3]; t[1] / 2 === 1"));
    EMB_CHECK(lit_true(e, "var arr = [/a/, /b/g]; arr[1].global === true"));
    EMB_CHECK(lit_true(e, "var f = function () { return /z/i; }; f().flags === 'i'"));
    lit_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
