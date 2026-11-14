#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* Uint8Array base64 and hex methods (ES2026). */

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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "base64.js", 0);
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

EMB_TEST(base64_round_trip_and_padding)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('SGVsbG8=').join() === '72,101,108,108,111'"));
    EMB_CHECK(js_true(e, "new Uint8Array([72, 101, 108, 108, 111]).toBase64() === 'SGVsbG8='"));
    EMB_CHECK(js_true(e, "new Uint8Array([72, 101, 108, 108, 111]).toBase64({omitPadding: true}) === 'SGVsbG8'"));
    EMB_CHECK(js_true(e, "new Uint8Array([]).toBase64() === '' && new Uint8Array([0]).toBase64() === 'AA=='"));
    EMB_CHECK(js_true(e, "new Uint8Array([251, 255]).toBase64() === '+/8='"));
    EMB_CHECK(js_true(e, "new Uint8Array([251, 255]).toBase64({alphabet: 'base64url'}) === '-_8='"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('-_8', {alphabet: 'base64url'}).join() === '251,255'"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64(' S G V s\\n bG8= ').length === 5"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('SGVsbG8').length === 5"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('SGVsbG8', {lastChunkHandling: 'strict'});"
                         " return false; } catch (x) { return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('SGVsbG8=', {lastChunkHandling: 'strict'}).length === 5"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('SGVsbG8', {lastChunkHandling: 'stop-before-partial'}).length === 3"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('SGV*'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('SGVsbG8==='); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('S', {}); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64(5); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('', {alphabet: 'base32'}); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('', 3); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(base64_set_from_returns_read_and_written)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = new Uint8Array(4); var r = t.setFromBase64('SGVsbG8=');"
                         "r.read === 4 && r.written === 3 && t.join() === '72,101,108,0'"));
    EMB_CHECK(js_true(e, "var u = new Uint8Array(8); var q = u.setFromBase64('SGVs');"
                         "q.read === 4 && q.written === 3 && u[2] === 108"));
    EMB_CHECK(js_true(e, "var w = new Uint8Array(2); w.setFromBase64('AAAA').written === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { new Int8Array(2).setFromBase64('AA=='); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(hex_round_trip_and_errors)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "Uint8Array.fromHex('48656c6C6f').join() === '72,101,108,108,111'"));
    EMB_CHECK(js_true(e, "new Uint8Array([255, 0, 16]).toHex() === 'ff0010'"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromHex('abc'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromHex('zz'); return false; } catch (x) { "
                         "return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "var h = new Uint8Array(3); var hr = h.setFromHex('0102'); "
                         "hr.read === 4 && hr.written === 2 && h.join() === '1,2,0'"));
    EMB_CHECK(js_true(e, "var h2 = new Uint8Array(1); var hr2 = h2.setFromHex('0102zz');"
                         "hr2.read === 2 && hr2.written === 1 && h2[0] === 1"));
    EMB_CHECK(js_true(e, "(function(){ var t = new Uint8Array(2); try { t.setFromHex('0g'); return false; }"
                         " catch (x) { return x instanceof SyntaxError && t[0] === 0; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A syntax error still writes the bytes decoded before it, then throws (FromHex / FromBase64). */
EMB_TEST(hex_and_base64_write_partial_bytes_before_error)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "(function(){ var t = new Uint8Array(4); try { t.setFromHex('0102zz');"
                         " return false; } catch (x) { return x instanceof SyntaxError &&"
                         " t.join() === '1,2,0,0'; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var t = new Uint8Array(4); try { t.setFromBase64('SGVs!!');"
                         " return false; } catch (x) { return x instanceof SyntaxError &&"
                         " t.join() === '72,101,108,0'; } })()"));
    /* An odd-length hex string is rejected up front, so nothing is written. */
    EMB_CHECK(js_true(e, "(function(){ var t = new Uint8Array(1); try { t.setFromHex('abc'); return false; }"
                         " catch (x) { return x instanceof SyntaxError && t[0] === 0; } })()"));
    /* Writes stop at the target's length even when the decoding would produce more bytes. */
    EMB_CHECK(js_true(e, "var lh = new Uint8Array(1); var lr = lh.setFromHex('0102zz');"
                         "lr.written === 1 && lh[0] === 1"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Option reads: toBase64 reads only alphabet and omitPadding; fromBase64 reads only alphabet and
 * lastChunkHandling. An unrelated option is never read, so its getter and bad value are ignored. */
EMB_TEST(base64_options_read_only_their_own_keys)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "(function(){ var hit = false; var o = new Uint8Array(1).toBase64({"
                         " get lastChunkHandling() { hit = true; return 'x'; } }); return !hit && o === 'AA=='; })()"));
    EMB_CHECK(js_true(e, "new Uint8Array(1).toBase64({lastChunkHandling: 'bogus'}) === 'AA=='"));
    EMB_CHECK(js_true(e, "(function(){ var hit = false; Uint8Array.fromBase64('AA', {"
                         " get omitPadding() { hit = true; return true; } }); return !hit; })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('AA', {omitPadding: true, lastChunkHandling: 'x'});"
                         " return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var t = new Uint8Array(2); try { t.setFromBase64('AA', {get omitPadding()"
                         " { throw new RangeError(); }}); return true; } catch (x) { return false; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* lastChunkHandling stop-before-partial: a lone '=' after a 2-character chunk stops before it. */
EMB_TEST(base64_stop_before_partial_lone_padding)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('ZXhhZg=', {lastChunkHandling: 'stop-before-partial'}).join() === '101,120,97'"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('AA=', {lastChunkHandling: 'stop-before-partial'}).length === 0"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('aQ=', {lastChunkHandling: 'stop-before-partial'}).length === 0"));
    EMB_CHECK(js_true(e, "var sb = new Uint8Array(8); var sr = sb.setFromBase64('ZXhhZg=', {lastChunkHandling: 'stop-before-partial'});"
                         "sr.read === 4 && sr.written === 3"));
    EMB_CHECK(js_true(e, "Uint8Array.fromBase64('AA==', {lastChunkHandling: 'stop-before-partial'}).join() === '0'"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('AA=', {lastChunkHandling: 'loose'}); return false; }"
                         " catch (x) { return x instanceof SyntaxError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array.fromBase64('AA=x', {lastChunkHandling: 'stop-before-partial'}); return false; }"
                         " catch (x) { return x instanceof SyntaxError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A full output stops the decoding before any trailing garbage is examined. */
EMB_TEST(base64_full_output_ignores_trailing_garbage)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var g = new Uint8Array(3); var gr = g.setFromBase64('aaaaaa#'); gr.read === 4 && gr.written === 3"));
    EMB_CHECK(js_true(e, "var g0 = new Uint8Array(0); var gr0 = g0.setFromBase64('#!'); gr0.read === 0 && gr0.written === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { new Uint8Array(3).setFromBase64('aaa#'); return false; }"
                         " catch (x) { return x instanceof SyntaxError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* FromBase64 with a limited target: the partial-chunk stop rule and the read index. */
EMB_TEST(base64_limited_target_stop_rule)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var s1 = new Uint8Array(4).setFromBase64('AAAAAAA', {lastChunkHandling: 'strict'});"
                         "s1.read === 4 && s1.written === 3"));
    EMB_CHECK(js_true(e, "var s2 = new Uint8Array(5).setFromBase64('AAAA   Zm9v'); s2.read === 4 && s2.written === 3"));
    EMB_CHECK(js_true(e, "var s3 = new Uint8Array(1).setFromBase64('AAA*'); s3.read === 0 && s3.written === 0"));
    EMB_CHECK(js_true(e, "var s4 = new Uint8Array(2).setFromBase64('AAAA'); s4.read === 0 && s4.written === 0"));
    EMB_CHECK(js_true(e, "var s5 = new Uint8Array(2).setFromBase64('AAA='); s5.read === 4 && s5.written === 2"));
    EMB_CHECK(js_true(e, "var s6 = new Uint8Array(4).setFromBase64('AAAA  AA=', {lastChunkHandling: 'stop-before-partial'});"
                         "s6.read === 4 && s6.written === 3"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The string builders throw a RangeError instead of aborting on an over-long result. */
EMB_TEST(base64_hex_result_too_long)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    /* A 1 GiB buffer's toHex would exceed the string limit; a plain ArrayBuffer allocation of that
     * size may fail first with a RangeError, which is also acceptable (never an abort). */
    EMB_CHECK(js_true(e, "(function(){ var u; try { u = new Uint8Array(2 ** 30); } catch (x) { return x instanceof RangeError; }"
                         " try { u.toHex(); return false; } catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var u; try { u = new Uint8Array(1.62e9); } catch (x) { return x instanceof RangeError; }"
                         " try { u.toBase64(); return false; } catch (x) { return x instanceof RangeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A fixed explicit length over a huge buffer is fine. */
EMB_TEST(typedarray_ctor_huge_buffer_fixed_length)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "(function(){ var b; try { b = new ArrayBuffer(2 ** 32); } catch (x) { return x instanceof RangeError; }"
                         " return new Uint8Array(b, 0, 10).length === 10; })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
