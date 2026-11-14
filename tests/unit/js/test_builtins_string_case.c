#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* String.prototype.toLowerCase and toUpperCase: full default case mapping from the generated
 * tables (SpecialCasing multi-code-point results, simple mappings across the planes, Final_Sigma)
 * and the non-locale rules. */

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

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "case.js", 0);
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

EMB_TEST(case_ascii_and_latin1)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'abcXYZ'.toUpperCase() === 'ABCXYZ'"));
    EMB_CHECK(js_true(e, "'ABCxyz 123!'.toLowerCase() === 'abcxyz 123!'"));
    EMB_CHECK(js_true(e, "'\\u00DF'.toUpperCase() === 'SS'"));
    EMB_CHECK(js_true(e, "'\\u00DF'.toUpperCase().length === 2"));
    EMB_CHECK(js_true(e, "'\\u00FF'.toUpperCase() === '\\u0178'"));
    EMB_CHECK(js_true(e, "'\\u00B5'.toUpperCase() === '\\u039C'"));
    EMB_CHECK(js_true(e, "'\\u00E9'.toUpperCase() === '\\u00C9'"));
    EMB_CHECK(js_true(e, "'\\u00C9'.toLowerCase() === '\\u00E9'"));
    EMB_CHECK(js_true(e, "'\\u00D7'.toLowerCase() === '\\u00D7'"));
    EMB_CHECK(js_true(e, "'\\u00F7'.toUpperCase() === '\\u00F7'"));
    env_free(e);
}

EMB_TEST(case_special_casing_multi_code_point)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'\\u0130'.toLowerCase() === 'i\\u0307'"));
    EMB_CHECK(js_true(e, "'\\u0130'.toLowerCase().length === 2"));
    EMB_CHECK(js_true(e, "'\\u0131'.toUpperCase() === 'I'"));
    EMB_CHECK(js_true(e, "'\\u0149'.toUpperCase() === '\\u02BCN'"));
    EMB_CHECK(js_true(e, "'\\u01F0'.toUpperCase() === 'J\\u030C'"));
    EMB_CHECK(js_true(e, "'\\u0390'.toUpperCase().length === 3"));
    EMB_CHECK(js_true(e, "'\\uFB03'.toUpperCase() === 'FFI'"));
    EMB_CHECK(js_true(e, "'\\uFB00'.toUpperCase() === 'FF'"));
    EMB_CHECK(js_true(e, "'\\u1E96'.toUpperCase() === 'H\\u0331'"));
    EMB_CHECK(js_true(e, "'\\u1E9E'.toLowerCase() === '\\u00DF'"));
    EMB_CHECK(js_true(e, "'stra\\u00DFe'.toUpperCase() === 'STRASSE'"));
    EMB_CHECK(js_true(e, "'\\u00DF'.toLocaleUpperCase() === 'SS'"));
    env_free(e);
}

EMB_TEST(case_titlecase_digraphs_and_special_letters)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'\\u01C5'.toUpperCase() === '\\u01C4'"));
    EMB_CHECK(js_true(e, "'\\u01C5'.toLowerCase() === '\\u01C6'"));
    EMB_CHECK(js_true(e, "'\\u01C4'.toLowerCase() === '\\u01C6'"));
    EMB_CHECK(js_true(e, "'\\u017F'.toUpperCase() === 'S'"));
    EMB_CHECK(js_true(e, "'\\u212A'.toLowerCase() === 'k'"));
    EMB_CHECK(js_true(e, "'\\u2160'.toLowerCase() === '\\u2170'"));
    EMB_CHECK(js_true(e, "'\\u2170'.toUpperCase() === '\\u2160'"));
    EMB_CHECK(js_true(e, "'\\u13A0'.toLowerCase() === '\\uAB70'"));
    EMB_CHECK(js_true(e, "'\\uAB70'.toUpperCase() === '\\u13A0'"));
    EMB_CHECK(js_true(e, "'\\u0412'.toLowerCase() === '\\u0432'"));
    EMB_CHECK(js_true(e, "'\\u0430'.toUpperCase() === '\\u0410'"));
    EMB_CHECK(js_true(e, "'\\u0561'.toUpperCase() === '\\u0531'"));
    env_free(e);
}

EMB_TEST(case_supplementary_planes)
{
    env e = env_new();
    /* U+10428 DESERET SMALL LETTER LONG I, U+10400 DESERET CAPITAL LETTER LONG I. */
    EMB_CHECK(js_true(e, "'\\uD801\\uDC28'.toUpperCase() === '\\uD801\\uDC00'"));
    EMB_CHECK(js_true(e, "'\\uD801\\uDC00'.toLowerCase().codePointAt(0) === 0x10428"));
    EMB_CHECK(js_true(e, "'\\uD801\\uDC28'.toUpperCase().length === 2"));
    /* Lone surrogates map to themselves. */
    EMB_CHECK(js_true(e, "'\\uD801'.toUpperCase() === '\\uD801'"));
    EMB_CHECK(js_true(e, "'\\uDC28'.toLowerCase() === '\\uDC28'"));
    EMB_CHECK(js_true(e, "'\\uDC28\\uD801'.toLowerCase() === '\\uDC28\\uD801'"));
    env_free(e);
}

EMB_TEST(case_final_sigma)
{
    env e = env_new();
    /* Final sigma: preceded by a cased letter and not followed by a cased letter. */
    EMB_CHECK(js_true(e, "'\\u0391\\u03A3'.toLowerCase() === '\\u03B1\\u03C2'"));
    EMB_CHECK(js_true(e, "'\\u0391\\u03A3\\u0391'.toLowerCase() === '\\u03B1\\u03C3\\u03B1'"));
    EMB_CHECK(js_true(e, "'\\u0391\\u03A3.'.toLowerCase() === '\\u03B1\\u03C2.'"));
    /* A case-ignorable apostrophe between the letters is skipped. */
    EMB_CHECK(js_true(e, "'\\u0391\\u03A3\\'\\u0392'.toLowerCase() === '\\u03B1\\u03C3\\'\\u03B2'"));
    EMB_CHECK(js_true(e, "'\\u0391\\u03A3\\''.toLowerCase() === '\\u03B1\\u03C2\\''"));
    /* No cased letter before: medial form. */
    EMB_CHECK(js_true(e, "'\\u03A3'.toLowerCase() === '\\u03C3'"));
    EMB_CHECK(js_true(e, "' \\u03A3'.toLowerCase() === ' \\u03C3'"));
    /* Upper case has no context rules. */
    EMB_CHECK(js_true(e, "'\\u03C2'.toUpperCase() === '\\u03A3'"));
    env_free(e);
}

EMB_TEST(case_empty_and_wide_results)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "''.toUpperCase() === '' && ''.toLowerCase() === ''"));
    EMB_CHECK(js_true(e, "'\\u0100\\u0101'.toUpperCase() === '\\u0100\\u0100'"));
    EMB_CHECK(js_true(e, "'\\u0100\\u0101'.toLowerCase() === '\\u0101\\u0101'"));
    EMB_CHECK(js_true(e, "'\\u4E2D\\u6587'.toUpperCase() === '\\u4E2D\\u6587'"));
    EMB_CHECK(js_true(e, "'\\u4E2D'.toLowerCase() === '\\u4E2D'"));
    env_free(e);
}

EMB_TEST(case_full_bmp_sweep_does_not_throw)
{
    env e = env_new();
    /* Every BMP code unit except the surrogates, through both mappings. */
    EMB_CHECK(js_true(e,
        "var a = []; for (var i = 0; i < 0x10000; i++) {"
        "  if (i < 0xD800 || i > 0xDFFF) a.push(String.fromCharCode(i)); }"
        "var s = a.join('');"
        "var u = s.toUpperCase(), l = s.toLowerCase();"
        "u.length >= s.length && l.length >= s.length && u.indexOf('\\u0178') >= 0 &&"
        "l.indexOf('\\u00E9') >= 0"));
    env_free(e);
}

EMB_TEST_MAIN()
