#include "emb_test.h"

#include "js/js.h"
#include "js/object.h"

#include <string.h>

/* JSON.rawJSON, JSON.isRawJSON and the raw-JSON case of JSON.stringify, and the source text
 * context that JSON.parse passes to a reviver (json-parse-with-source, ES2026). */

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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "json_source.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(json_raw_json_objects)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "JSON.rawJSON.length === 1 && JSON.isRawJSON.length === 1"));
    EMB_CHECK(js_true(e, "(function () { var r = JSON.rawJSON('1e1000'); return r.rawJSON === "
                         "'1e1000' && JSON.isRawJSON(r) === true && Object.isFrozen(r) && "
                         "Object.getPrototypeOf(r) === null; })()"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(JSON.rawJSON('2'), 'rawJSON').enumerable === "
                         "true && Object.getOwnPropertyDescriptor(JSON.rawJSON('2'), 'rawJSON')"
                         ".writable === false"));
    EMB_CHECK(js_true(e, "JSON.isRawJSON({rawJSON: '1'}) === false && JSON.isRawJSON(1) === false"));
    EMB_CHECK(js_true(e, "JSON.isRawJSON(JSON.rawJSON('\"s\"')) === true && "
                         "JSON.rawJSON('null').rawJSON === 'null' && "
                         "JSON.rawJSON('true').rawJSON === 'true'"));
    /* Text that is not a JSON primitive, or that has white space at an end, is a SyntaxError. */
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON('{}'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON('[1]'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON(' 1'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON('1\\n'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON(''); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { JSON.rawJSON('1 2'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    env_free(e);
}

/* stringify writes the raw text as it is, so a big integer keeps all its digits. */
EMB_TEST(json_stringify_raw_json)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "JSON.stringify({a: JSON.rawJSON('12345678901234567890')}) === "
                         "'{\"a\":12345678901234567890}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([JSON.rawJSON('\"x\"'), JSON.rawJSON('null')]) === "
                         "'[\"x\",null]'"));
    EMB_CHECK(js_true(e, "JSON.stringify(JSON.rawJSON('1.50')) === '1.50'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a: JSON.rawJSON('1')}, null, 2) === "
                         "'{\\n  \"a\": 1\\n}'"));
    env_free(e);
}

/* The reviver gets context.source for primitives, and no source for objects or arrays. */
EMB_TEST(json_parse_reviver_source_context)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "(function () { var seen = {}; JSON.parse('[1.0, \"a\", {\"k\": 2e1}, "
                         "true, null]', function (k, v, c) { if (typeof v !== 'object' || v === null) "
                         "{ seen[k] = c.source; } else { seen[k] = 'obj:' + ('source' in c); } "
                         "return v; }); return seen['0'] === '1.0' && seen['1'] === '\"a\"' && "
                         "seen.k === '2e1' && seen['3'] === 'true' && seen['4'] === 'null' && "
                         "seen['2'] === 'obj:false'; })()"));
    /* The source is the text as written, including escapes. */
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\u0041\"', function (k, v, c) { return c.source; }) === "
                         "'\"\\\\u0041\"'"));
    /* The root value gets its source when it is a primitive. */
    EMB_CHECK(js_true(e, "JSON.parse(' -0 ', function (k, v, c) { return k === '' ? c.source : v; }) "
                         "=== '-0'"));
    /* A value replaced by the reviver before its visit has no source: SameValue fails. */
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":1,\"b\":2}', function (k, v, c) { if (k === 'a') { "
                         "this.b = 3; } if (k === 'b') { return [c.source === undefined ? 'none' : "
                         "c.source, v]; } return v; }).b[0] === 'none'"));
    /* Without a reviver the result is the plain parse. */
    EMB_CHECK(js_true(e, "JSON.parse('[1,{\"x\":[2]}]')[1].x[0] === 2"));
    /* A repeated key: the last entry gives the source. */
    EMB_CHECK(js_true(e, "(function () { var s; JSON.parse('{\"d\":1,\"d\":22}', function (k, v, c) { "
                         "if (k === 'd') { s = c.source; } return v; }); return s === '22'; })()"));
    env_free(e);
}

EMB_TEST_MAIN()
