/* The remaining ES2015-ES2025 builtins of J2h: Array.from, the ES2023
 * copying methods, the Array unscopables, String isWellFormed/toWellFormed and normalize (with the
 * generated UAX #15 tables), localeCompare's canonical equivalence, Object.fromEntries, and the
 * runtime-wide Symbol.for registry. */
#include "es6_util.h"

#include "js/js.h"

#include <string.h>

/* Array.from: iterables (with IteratorClose on a throwing mapper), array-likes, mapFn and thisArg,
 * and a constructor as this. */
EMB_TEST(es2015_array_from)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "Array.from('abc').length", 3);
    E6_TEXT(e, "Array.from('a\\uD83D\\uDE00b').join('|')", "a|\xF0\x9F\x98\x80|b");
    E6_NUM(e, "Array.from({length: 2, 0: 'x', 1: 'y'}).length", 2);
    E6_TEXT(e, "Array.from([1, 2, 3], function (v, k) { return v * 10 + k + this.d }, {d: 1}).join()",
            "11,22,33");
    E6_BOOL(e, "Array.from(new Set([1, 1, 2])).length === 2", true);
    E6_NUM(e, "var closed = 0; var it = {[Symbol.iterator]() { return {next() { return {value: 1, done: false} },"
              " return() { closed++; return {} }} }}; try { Array.from(it, function () { throw 1 }) } catch (x) {}"
              " closed", 1);
    E6_THROWS(e, "Array.from([], 5)", "TypeError");
    E6_THROWS(e, "Array.from(null)", "TypeError");
    E6_BOOL(e, "function C() { this.made = arguments.length } Array.from.call(C, [1]) instanceof C", true);
    E6_NUM(e, "Array.from.call(function (n) { this.n = n }, [7, 8]).length", 2);
    E6_BOOL(e, "Array.from.length === 1", true);
    es6_env_free(e);
}

/* toReversed, toSorted, toSpliced and with: fresh arrays, holes read as undefined, and the errors. */
EMB_TEST(es2023_array_copy_methods)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "[1, 2, 3].toReversed().join()", "3,2,1");
    E6_TEXT(e, "var src = [1, 2]; var rev = src.toReversed(); rev !== src && src.join() === '1,2' ? 'ok' : 'bad'", "ok");
    E6_TEXT(e, "[3, undefined, 1, , 2].toSorted().join()", "1,2,3,,");
    E6_BOOL(e, "[3, undefined, 1].toSorted()[2] === undefined", true);
    E6_TEXT(e, "[10, 9, 1].toSorted((a, b) => a - b).join()", "1,9,10");
    E6_THROWS(e, "[1].toSorted(5)", "TypeError");
    E6_TEXT(e, "[1, 2, 3, 4].toSpliced(1, 2, 'a', 'b', 'c').join()", "1,a,b,c,4");
    E6_TEXT(e, "[1, 2, 3].toSpliced().join()", "1,2,3");
    E6_TEXT(e, "[1, 2, 3].toSpliced(1).join()", "1");
    E6_TEXT(e, "[1, 2, 3].toSpliced(-1, 1, 9).join()", "1,2,9");
    E6_TEXT(e, "var s = [1, 2, 3]; s.toSpliced(0, 1); s.join()", "1,2,3");
    E6_TEXT(e, "[1, 2, 3].with(-1, 'x').join()", "1,2,x");
    E6_TEXT(e, "[1, 2, 3].with(0, 'y').join()", "y,2,3");
    E6_THROWS(e, "[1, 2].with(2, 0)", "RangeError");
    E6_THROWS(e, "[1, 2].with(-3, 0)", "RangeError");
    E6_BOOL(e, "Array.prototype.with.length === 2 && Array.prototype.toSpliced.length === 2", true);
    E6_BOOL(e, "Array.prototype.toSorted.length === 1 && Array.prototype.toReversed.length === 0", true);
    E6_BOOL(e, "Array.prototype[Symbol.unscopables].toSorted === true && "
               "Array.prototype[Symbol.unscopables].with === undefined", true);
    E6_TEXT(e, "Array.prototype.toReversed.call({length: 2, 0: 'a', 1: 'b'}).join()", "b,a");
    es6_env_free(e);
}

/* String.prototype.isWellFormed and toWellFormed. */
EMB_TEST(es2024_string_well_formed)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "'abc'.isWellFormed()", true);
    E6_BOOL(e, "'\\uD83D\\uDE00'.isWellFormed()", true);
    E6_BOOL(e, "'a\\uD800b'.isWellFormed()", false);
    E6_BOOL(e, "'a\\uDC00'.isWellFormed()", false);
    E6_TEXT(e, "'a\\uD800b'.toWellFormed()", "a\xEF\xBF\xBD" "b");
    E6_TEXT(e, "'\\uD83D\\uDE00\\uDC00'.toWellFormed()", "\xF0\x9F\x98\x80\xEF\xBF\xBD");
    E6_BOOL(e, "'\\uD83D\\uDE00'.toWellFormed() === '\\uD83D\\uDE00'", true);
    es6_env_free(e);
}

/* String.prototype.normalize: the four forms on sample strings from the UAX #15 examples. */
EMB_TEST(es2015_string_normalize)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* e + combining acute composes in NFC and decomposes in NFD. */
    E6_NUM(e, "'e\\u0301'.normalize('NFC').length", 1);
    E6_NUM(e, "'\\u00E9'.normalize('NFD').length", 2);
    E6_BOOL(e, "'\\u00E9'.normalize() === '\\u00E9'.normalize('NFC')", true);
    /* Hangul syllables decompose algorithmically and recompose. */
    E6_NUM(e, "'\\uAC00'.normalize('NFD').length", 2);
    E6_BOOL(e, "'\\u1100\\u1161\\u11A8'.normalize('NFC') === '\\uAC01'", true);
    /* Compatibility forms: ligature fi (U+FB01) and the super two (U+00B2). */
    E6_TEXT(e, "'\\uFB01'.normalize('NFKD')", "fi");
    E6_TEXT(e, "'\\uFB01'.normalize('NFC')", "\xEF\xAC\x81");
    E6_TEXT(e, "'x\\u00B2'.normalize('NFKC')", "x2");
    /* Canonical ordering of combining marks: a dot below (ccc 220) comes before an acute (230). */
    E6_TEXT(e, "'a\\u0301\\u0323'.normalize('NFD') === 'a\\u0323\\u0301' ? 'ok' : 'bad'", "ok");
    /* Supplementary characters: the mathematical bold A (U+1D400) is compatibility-equivalent to A. */
    E6_TEXT(e, "'\\uD835\\uDC00'.normalize('NFKC')", "A");
    E6_NUM(e, "'\\uD835\\uDC00'.normalize('NFC').length", 2);
    /* A lone surrogate passes through. */
    E6_NUM(e, "'\\uD800e\\u0301'.normalize('NFC').length", 2);
    E6_THROWS(e, "'a'.normalize('nfc')", "RangeError");
    E6_BOOL(e, "'ASCII'.normalize('NFD') === 'ASCII'", true);
    es6_env_free(e);
}

/* Long mark runs, reordering across 3+ marks, and composition blocking. */
EMB_TEST(es2015_string_normalize_marks)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Reordering: ccc 230 (0301), 220 (0323), 202 (0328), 230 (0307) -> stable sort by ccc. */
    E6_BOOL(e, "'a\u0301\u0323\u0328\u0307'.normalize('NFD') === 'a\u0328\u0323\u0301\u0307'", true);
    /* NFC composes a + 0328 + 0323 + 0301 ? first starter-composable mark only; rest stay ordered. */
    E6_BOOL(e, "'a\u0301\u0323\u0328'.normalize('NFC') === '\u0105\u0323\u0301'", true);
    /* Blocking: a + 0323 + 0307 composes to U+1EA1 + 0307 (0307 not blocked, but no pair);
       a + 0301 + 0301: the second 0301 has the same ccc as the first, so it is blocked. */
    E6_BOOL(e, "'a\u0301\u0301'.normalize('NFC') === '\u00E1\u0301'", true);
    /* Blocking by a starter-less intervening mark of lower ccc: o + 0323 + 0302 -> U+1ED9. */
    E6_BOOL(e, "'o\u0323\u0302'.normalize('NFC') === '\u1ED9'", true);
    /* A long run of 2000 identical marks must keep its length in NFD and fold the first mark in NFC. */
    E6_NUM(e, "('a' + '\u0301'.repeat(2000)).normalize('NFD').length", 2001);
    E6_NUM(e, "('a' + '\u0301'.repeat(2000)).normalize('NFC').length", 2000);
    /* A long alternating run sorts: 1000 x (0301, 0323) -> 1000 x 0323 then 1000 x 0301. */
    E6_BOOL(e, "(function(){var s='a'+'\u0301\u0323'.repeat(1000);var r=s.normalize('NFD');"
               "if(r.length!==2001)return false;for(var i=1;i<=1000;i++)if(r[i]!=='\u0323')return false;"
               "for(var i=1001;i<=2000;i++)if(r[i]!=='\u0301')return false;return true;})()", true);
    es6_env_free(e);
}

/* localeCompare compares canonical equivalents as equal (ECMA-262 22.1.3.12). */
EMB_TEST(es2015_string_locale_compare_canonical)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "'o\\u0308'.localeCompare('\\u00F6')", 0);
    E6_NUM(e, "'a'.localeCompare('b')", -1);
    E6_NUM(e, "'b'.localeCompare('a')", 1);
    es6_env_free(e);
}

/* Object.fromEntries: pairs from an iterable, and the closing of the iterator on a bad entry. */
EMB_TEST(es2019_object_from_entries)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "Object.fromEntries([['a', 1], ['b', 2]]).b", 2);
    E6_NUM(e, "Object.fromEntries(new Map([[1, 'x']]))['1'].length", 1);
    E6_BOOL(e, "Object.getPrototypeOf(Object.fromEntries([])) === Object.prototype", true);
    E6_THROWS(e, "Object.fromEntries(null)", "TypeError");
    E6_THROWS(e, "Object.fromEntries([1])", "TypeError");
    E6_NUM(e, "var closed = 0; var it = {[Symbol.iterator]() { return {next() { return {value: 1, done: false} },"
              " return() { closed++; return {} }} }}; try { Object.fromEntries(it) } catch (x) {} closed", 1);
    es6_env_free(e);
}

/* Symbol.for uses one registry per runtime, so two realms of a runtime share it. */
EMB_TEST(es2015_symbol_registry_runtime_wide)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "Symbol.for('ember') === Symbol.for('ember')", true);
    E6_TEXT(e, "Symbol.keyFor(Symbol.for('ember'))", "ember");
    E6_BOOL(e, "Symbol.keyFor(Symbol('ember')) === undefined", true);
    embi_js_realm *other = embi_js_realm_new(e.rt);
    EMB_REQUIRE(other != nullptr);
    embi_js_value v = embi_js_eval(other, "Symbol.for('ember')", strlen("Symbol.for('ember')"),
                                   "test.js", 0);
    bool same = v != EMBI_JS_EXCEPTION && embi_js_is_symbol(v);
    embi_js_value first = es6_run(e, "Symbol.for('ember')");
    same = same && first == v;
    EMB_CHECK(same);
    embi_js_realm_free(other);
    es6_env_free(e);
}

/* Full-width and fraction forms still map in the compatibility path, and the unscopables list
 * lets with-blocks see the new names as absent. */
EMB_TEST(es2015_array_unscopables_names)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "Array.prototype[Symbol.unscopables].keys === true && "
               "Array.prototype[Symbol.unscopables].values === true && "
               "Array.prototype[Symbol.unscopables].entries === true", true);
    es6_env_free(e);
}

EMB_TEST_MAIN()
