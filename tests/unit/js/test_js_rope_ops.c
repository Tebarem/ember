#include "emb_test.h"

#include "es6_util.h"
#include "js/js.h"

/* J5g rope producers and the flatten audit. Each script check runs
 * with rope_min 1 (every concatenation is a rope) and with the default, and must give true both
 * times: a char-reading site that misses a flatten fails here. The C-level checks pin the
 * producers (the result really is a rope) and the flat forms of the conversions. */

static const char *const script_checks[] = {
    /* ADD and += */
    "var s = ''; for (var i = 0; i < 300; i++) { s += 'ab'; } s.length === 600 && "
    "s.charCodeAt(599) === 98 && s.charAt(0) === 'a'",
    "var a = 'x' + 'y'; var b = a + 1 + null + undefined + true; b === 'xy1nullundefinedtrue'",
    "var u = '\\u00e9' + ('a' + 'b'); u.length === 3 && u.charCodeAt(0) === 233 && u === '\\u00e9ab'",
    "var w = '\\u4e2d' + 'z'; w.length === 2 && w.charCodeAt(0) === 0x4e2d && w.charCodeAt(1) === 122",
    "('a' + 'b') === 'ab' && ('a' + 'b') < ('a' + 'c') && ('ab' + 'c') == 'abc'",
    "var o = { valueOf: function () { return 'p' + 'q'; } }; (o + 'r') === 'pqr'",
    "var k = {}; k['a' + 'b'] = 7; k.ab === 7 && Object.keys(k)[0] === 'ab'",
    "var n = {}; n['1' + '0'] = 'idx'; n[10] === 'idx' && n['10'] === 'idx'",
    "var big = ''; for (var i = 0; i < 300; i++) { big = big + 'ab' + 'c'; } big.length === 900 && "
    "big.indexOf('cab') === 2",
    /* Template literals */
    "var x = 'q' + 'r'; var t = `a${x}b${'c' + 'd'}e${1 + 1}`; t === 'aqrbcde2'",
    "var o2 = { toString: function () { return 'T' + 'S'; } }; `${o2}` === 'TS'",
    "var tl = ''; for (var i = 0; i < 200; i++) { tl = `${tl}${'z' + 'y'}`; } tl.length === 400 && "
    "tl.charAt(399) === 'y'",
    /* String.prototype.concat */
    "'ab'.concat('cd', 1, null) === 'abcd1null'",
    "('x' + 'y').concat('z' + 'w').concat(3) === 'xyzw3'",
    "var cc = ''; for (var i = 0; i < 300; i++) { cc = cc.concat('ab'); } cc.length === 600 && "
    "cc.indexOf('ba') === 1",
    /* Array.prototype.join, short and chunked (over 4096 units) */
    "[1, 'b' + 'c', null, undefined, true].join('-') === '1-bc---true'",
    "[,'a',,'b'].join('+') === '+a++b'",
    "var a2 = []; for (var i = 0; i < 2000; i++) { a2.push('xyz'); } var j = a2.join(''); "
    "j.length === 6000 && j.charAt(5999) === 'z' && j.slice(2998, 3001) === 'yzx'",
    "var a5 = []; for (var i = 0; i < 1500; i++) { a5.push(i % 2 ? '\\u4e2d' : 'a'); } "
    "var j5 = a5.join(','); j5.length === 2999 && j5.charCodeAt(0) === 97 && "
    "j5.charCodeAt(2) === 0x4e2d && j5.indexOf(',') === 1",
    /* JSON */
    "var js = ''; for (var i = 0; i < 300; i++) { js += '\\u00e9'; } JSON.stringify(js).length === 302",
    "JSON.stringify({ ke: 'v' + 'w' }) === '{\"ke\":\"vw\"}'",
    "JSON.parse('[' + '1,2' + ']').length === 2 && JSON.parse('\"' + 'ab' + '\"') === 'ab'",
    "JSON.stringify({ a: 1 }, null, '' + '  ') === '{\\n  \"a\": 1\\n}'",
    "typeof JSON.rawJSON !== 'function' || JSON.stringify({ x: JSON.rawJSON('1' + '2') }) === "
    "'{\"x\":12}'",
    /* Date parsing through ToPrimitive and the string constructor path */
    "new Date('2020-01-' + '01T00:00:00Z').getTime() === Date.UTC(2020, 0, 1)",
    "var tv = { valueOf: function () { return '2020-01-' + '03T00:00:00Z'; } }; "
    "new Date(tv).getTime() === Date.UTC(2020, 0, 3)",
    "typeof Symbol !== 'function' || new Date(0)[Symbol.toPrimitive]('str' + 'ing') === "
    "new Date(0).toString()",
    /* RegExp, replace, split, matching */
    "'abc'.replace('b', function () { return '1' + '2'; }) === 'a12c'",
    "'xaay'.replace(/a+/, '[' + ']') === 'x[]y'",
    "'abc'.replace(/(b)/, '<' + '$1' + '>') === 'a<b>c'",
    "/^a+$/.test('a' + 'aa')",
    "var sep = ',' + 'x'; var parts = 'a,xb'.split(sep); parts.length === 2 && parts[1] === 'b'",
    "var rr = 'ab' + 'ab'; /b/g.exec(rr).index === 1",
    "new RegExp('a' + 'b').source === 'ab' && String(new RegExp('/' + 'x')) === '/\\\\/x/'",
    "'AB'.search(/B/) === 1 && 'xAB'.match('A' + 'B').index === 1",
    "'a' + 'b' === 'ab' && 'xaby'.replace(/(?<n>ab)/, '[$<n>]') === 'x[ab]y'",
    /* String iteration and the string methods on rope receivers */
    "[...('ab' + 'cd')].join('') === 'abcd'",
    "var sc = 0; for (var c of 'a' + 'b') { sc += c.length; } sc === 2",
    "('a' + '\\uD83D\\uDE00') + ('\\uDE00' + 'b') === 'a\\uD83D\\uDE00\\uDE00b'",
    "('x' + '\\uD83D\\uDE00').codePointAt(1) === 0x1F600",
    "('ab' + 'cd').toUpperCase() === 'ABCD' && ('ab' + 'cd').slice(1, 3) === 'bc' && "
    "('ab' + 'cd').indexOf('cd') === 2 && ('a' + 'b').at(-1) === 'b'",
    "('ab' + 'cd').padStart(6, '.' + '') === '..abcd' && ('ab' + 'cd').normalize('NFC') === 'abcd'",
    "('x' + 'y').length === 2 && ('x' + 'y')[1] === 'y'",
    /* String wrappers read their characters through the own-property hooks */
    "var w = Object('x' + 'yz'); w.length === 3 && w[1] === 'y' && Object.keys(w).length === 3",
    "new String('p' + 'q').toString() === 'pq' && new String('p' + 'q') == 'pq'",
    /* Numbers, BigInt, booleans */
    "123n == ('1' + '23') && BigInt('1' + '23') === 123n && 1n < ('2' + '0')",
    "Number('1' + '2') === 12 && +('1' + '.5') === 1.5 && !!('0' + '') && parseInt('1' + '2') === 12",
    "parseFloat('3' + '.5') === 3.5 && isNaN('x' + 'y')",
    /* Intl locale tags */
    "typeof Intl !== 'object' || Intl.NumberFormat.supportedLocalesOf(['en-' + 'US']).length === 1",
    /* Typed arrays: base64 and hex decoding, alphabet options */
    "typeof Uint8Array.fromHex !== 'function' || Uint8Array.fromHex('0' + '1ff')[1] === 255",
    "typeof Uint8Array.fromBase64 !== 'function' || Uint8Array.fromBase64('AQ' + 'I=')[1] === 2",
    "typeof Uint8Array.fromBase64 !== 'function' || "
    "Uint8Array.fromBase64('AQ' + 'I', { alphabet: 'base64' + 'url' })[1] === 2",
    "typeof Uint8Array.fromBase64 !== 'function' || "
    "Uint8Array.fromBase64('AQ' + 'I=', { alphabet: 'base' + '64' })[1] === 2",
    /* Symbols with rope descriptions */
    "typeof Symbol !== 'function' || String(Symbol('a' + 'b')) === 'Symbol(ab)'",
    "typeof Symbol !== 'function' || Symbol('a' + 'b').description === 'ab'",
    "typeof Symbol !== 'function' || (function () { var tg = {}; tg[Symbol.toStringTag] = 'A' + 'B'; "
    "return Object.prototype.toString.call(tg) === '[object AB]'; })()",
    /* Errors, collections, equality of long ropes */
    "var msg = 'bad' + ' thing'; try { throw new Error(msg); } catch (e) { e.message === 'bad thing' }",
    "var m = new Map(); m.set('a' + 'b', 1); m.get('ab') === 1 && new Set(['x' + 'y']).has('xy')",
    "var hk = ''; for (var i = 0; i < 300; i++) { hk += 'k'; } var mm = new Map(); mm.set(hk, 'v'); "
    "mm.get('k'.repeat(300)) === 'v' && hk === 'k'.repeat(300)",
    "typeof ('a' + 'b') === 'string' && ('a' + 'b') === 'ab'",
    /* Sites the source audit names only by reach: eval, Function, String.raw, URI decoding, parseInt */
    "eval('1+' + '2') === 3",
    "new Function('a', 'return a' + '+1')(1) === 2",
    "String.raw({ raw: ['a' + 'b', 'c'] }, 1) === 'ab1c'",
    "decodeURIComponent('%41' + 'B') === 'AB' && decodeURI('%41' + '%42') === 'AB'",
    "parseInt(' ' + '  4' + '2x') === 42 && parseInt('0x' + '1f') === 31",
    /* Join: hole-heavy input, locale input and the 4096-unit chunk boundary */
    "var h = []; h[5000] = 'x'; var hj = h.join('-'); hj.length === 5001 && hj.charAt(5000) === 'x'",
    "var lo = { toLocaleString: function () { return 'p' + 'q'; } }; "
    "[lo, 'r' + 's'].toLocaleString() === 'pq,rs'",
    "var la = []; for (var i = 0; i < 1500; i++) { la.push({ toLocaleString: function () "
    "{ return 'p' + 'q'; } }); } la.toLocaleString().length === 4499",
    "var a6 = []; for (var i = 0; i < 1365; i++) { a6.push('abc'); } var j6 = a6.join('-'); "
    "j6.length === 5459 && j6.slice(4093, 4097) === 'bc-a'",
};

/* Runs one script on a fresh realm with the given rope threshold, expecting it to be true. */
static bool script_is_true(const char *src, uint32_t rope_min)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    if (!e.rt || !e.r) {
        return false;
    }
    if (rope_min != 0) {
        embi_js_set_rope_min(e.rt, rope_min);
    }
    embi_js_value v = es6_run(e, src);
    bool ok = v == EMBI_JS_TRUE;
    if (!ok) {
        es6_note(src);
        es6_note_exc(e.rt);
    }
    es6_env_free(e);
    /* Windows ASan has no leak detector, so the live-allocation count stands in for one. */
    if (embi_mem_live() != mem0) {
        SDL_Log("    leak: live allocations changed by %lld", (long long)embi_mem_live() - (long long)mem0);
        ok = false;
    }
    return ok;
}

EMB_TEST(rope_ops_script_checks_hold_with_every_concat_a_rope)
{
    for (size_t i = 0; i < sizeof script_checks / sizeof script_checks[0]; i++) {
        EMB_CHECK(script_is_true(script_checks[i], 1));
    }
}

EMB_TEST(rope_ops_script_checks_hold_with_default_threshold)
{
    for (size_t i = 0; i < sizeof script_checks / sizeof script_checks[0]; i++) {
        EMB_CHECK(script_is_true(script_checks[i], 0));
    }
}

/* Evaluates src and returns the result value (the realm stays alive for the caller to free). */
static bool result_is_rope(const char *src, uint32_t rope_min, bool want_rope)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    if (!e.rt || !e.r) {
        return false;
    }
    if (rope_min != 0) {
        embi_js_set_rope_min(e.rt, rope_min);
    }
    embi_js_value v = es6_run(e, src);
    bool ok = embi_js_is_string(v) && embi_js_str_is_rope(embi_js_as_string(v)) == want_rope;
    if (!ok) {
        es6_note(src);
    }
    es6_env_free(e);
    if (embi_mem_live() != mem0) {
        SDL_Log("    leak: live allocations changed");
        ok = false;
    }
    return ok;
}

EMB_TEST(rope_ops_producers_build_ropes)
{
    EMB_CHECK(result_is_rope("'ab' + 'cd'", 1, true));
    EMB_CHECK(result_is_rope("var s = 'a'; s += 'b'; s", 1, true));
    EMB_CHECK(result_is_rope("var q = 'x'; `a${q}b`", 1, true));
    EMB_CHECK(result_is_rope("'ab'.concat('cd')", 1, true));
    EMB_CHECK(result_is_rope("'ab' + 'cd' + 'ef'", 0, false)); /* short: flat under the default */
    EMB_CHECK(result_is_rope("var a = []; for (var i = 0; i < 2000; i++) a.push('xyz'); a.join('')",
                             0, true));
    EMB_CHECK(result_is_rope("[1, 2].join('-')", 0, false));
}

EMB_TEST(rope_ops_to_string_and_key_are_flat)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    embi_js_set_rope_min(e.rt, 1);
    embi_js_string *a = embi_js_str_new_utf8(e.rt, "ab", 2);
    embi_js_string *b = embi_js_str_new_utf8(e.rt, "cd", 2);
    embi_js_value r = embi_js_str_concat_v(e.rt, embi_js_from_string(a), embi_js_from_string(b));
    EMB_CHECK(embi_js_str_is_rope(embi_js_as_string(r)));

    embi_js_string *flat = embi_js_to_string(e.rt, r);
    EMB_REQUIRE(flat != nullptr);
    EMB_CHECK(!embi_js_str_is_rope(flat));
    EMB_CHECK(embi_js_str_len(flat) == 4);
    EMB_CHECK(embi_js_str_at(flat, 3) == 'd');

    /* A property key made from the rope is the interned atom for the same text. */
    embi_js_key key = embi_js_to_property_key(e.rt, r);
    embi_js_key same = embi_js_key_from_str(e.rt, embi_js_str_new_utf8(e.rt, "abcd", 4));
    EMB_CHECK(key == same);

    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
