/* Minimal Intl through the engine: locale lists, NumberFormat,
 * Collator, PluralRules, Intl.getCanonicalLocales and the toLocaleString family for numbers,
 * BigInts, arrays and strings. Each script returns the labels of the checks that failed, so an
 * empty string means every check passed. Date formatting is in test_builtins_intl_date.c. */
#include "es6_util.h"

#include <string.h>

static const char *intl_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "function throws(f, E) { try { f(); return false; } catch (e) { return e instanceof E; } }\n"
    "var nf = function (o) { return new Intl.NumberFormat('en-US', o); };\n"
    "c('nf_plain', nf().format(1234567.891) === '1,234,567.891' &&\n"
    "    (1234.5).toLocaleString() === '1,234.5' && (0).toLocaleString() === '0');\n"
    "c('nf_negative_zero', (-0).toLocaleString() === '-0' && nf().format(-1.5) === '-1.5');\n"
    "c('nf_nan_inf', NaN.toLocaleString() === 'NaN' && (Infinity).toLocaleString() === '\\u221e' &&\n"
    "    (-Infinity).toLocaleString() === '-\\u221e');\n"
    "c('nf_big_magnitude', (1e21).toLocaleString() === '1,000,000,000,000,000,000,000' &&\n"
    "    (1e-7).toLocaleString('en-US', { maximumFractionDigits: 8 }) === '0.0000001');\n"
    "c('nf_percent', (0.5).toLocaleString('en-US', { style: 'percent' }) === '50%' &&\n"
    "    nf({ style: 'percent', maximumFractionDigits: 1 }).format(0.1234) === '12.3%');\n"
    "c('nf_currency', nf({ style: 'currency', currency: 'USD' }).format(1234.56) === '$1,234.56' &&\n"
    "    nf({ style: 'currency', currency: 'USD' }).format(-5) === '-$5.00' &&\n"
    "    nf({ style: 'currency', currency: 'JPY' }).format(1234) === '\\u00a51,234' &&\n"
    "    nf({ style: 'currency', currency: 'EUR' }).format(9.5) === '\\u20ac9.50' &&\n"
    "    nf({ style: 'currency', currency: 'CHF' }).format(2) === 'CHF\\u00a02.00');\n"
    "c('nf_currency_display', nf({ style: 'currency', currency: 'USD', currencyDisplay: 'code' })\n"
    "    .format(1) === 'USD\\u00a01.00' &&\n"
    "    nf({ style: 'currency', currency: 'USD', currencyDisplay: 'name' }).format(2) ===\n"
    "    '2.00 US dollars' &&\n"
    "    nf({ style: 'currency', currency: 'USD', currencyDisplay: 'name' }).format(1) ===\n"
    "    '1.00 US dollars' &&\n"
    "    nf({ style: 'currency', currency: 'USD', currencyDisplay: 'name', minimumFractionDigits: 0,\n"
    "        maximumFractionDigits: 0 }).format(1) === '1 US dollar');\n"
    "c('nf_accounting', nf({ style: 'currency', currency: 'USD', currencySign: 'accounting' })\n"
    "    .format(-3) === '($3.00)');\n"
    "c('nf_currency_required', throws(function () { nf({ style: 'currency' }); }, TypeError) &&\n"
    "    throws(function () { nf({ style: 'currency', currency: 'US' }); }, RangeError));\n"
    "c('nf_digits', nf({ minimumFractionDigits: 2 }).format(5) === '5.00' &&\n"
    "    nf({ minimumIntegerDigits: 3 }).format(7) === '007' &&\n"
    "    nf({ maximumFractionDigits: 0 }).format(2.5) === '3' &&\n"
    "    nf({ maximumFractionDigits: 2 }).format(0.125) === '0.13');\n"
    "c('nf_significant', nf({ maximumSignificantDigits: 3 }).format(123456) === '123,000' &&\n"
    "    nf({ maximumSignificantDigits: 2 }).format(0.000123) === '0.00012' &&\n"
    "    nf({ minimumSignificantDigits: 3 }).format(5) === '5.00' &&\n"
    "    nf({ minimumSignificantDigits: 3 }).format(0) === '0.00');\n"
    "c('nf_digit_range', throws(function () { nf({ maximumFractionDigits: 101 }); }, RangeError) &&\n"
    "    throws(function () { nf({ minimumFractionDigits: 3, maximumFractionDigits: 2 }); },\n"
    "        RangeError) &&\n"
    "    throws(function () { nf({ minimumSignificantDigits: 5, maximumSignificantDigits: 2 }); },\n"
    "        RangeError));\n"
    "c('nf_grouping', nf({ useGrouping: false }).format(12345) === '12345' &&\n"
    "    nf({ useGrouping: 'always' }).format(123) === '123' &&\n"
    "    nf({ useGrouping: 'min2' }).format(1234) === '1234' &&\n"
    "    nf({ useGrouping: 'min2' }).format(12345) === '12,345' &&\n"
    "    nf({ useGrouping: 'always' }).format(1234) === '1,234');\n"
    "c('nf_sign_display', nf({ signDisplay: 'always' }).format(5) === '+5' &&\n"
    "    nf({ signDisplay: 'exceptZero' }).format(0) === '0' &&\n"
    "    nf({ signDisplay: 'never' }).format(-5) === '5' &&\n"
    "    nf({ signDisplay: 'negative' }).format(-0) === '0' &&\n"
    "    nf({ signDisplay: 'auto' }).format(-5) === '-5');\n"
    "c('nf_rounding_modes', nf({ maximumFractionDigits: 0, roundingMode: 'halfEven' }).format(2.5) ===\n"
    "    '2' && nf({ maximumFractionDigits: 0, roundingMode: 'halfEven' }).format(3.5) === '4' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'trunc' }).format(-2.7) === '-2' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'floor' }).format(-2.2) === '-3' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'ceil' }).format(2.2) === '3' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'expand' }).format(-2.2) === '-3' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'halfFloor' }).format(-2.5) === '-3' &&\n"
    "    nf({ maximumFractionDigits: 0, roundingMode: 'halfTrunc' }).format(2.5) === '2');\n"
    "c('nf_rounding_mode_invalid', throws(function () { nf({ roundingMode: 'up' }); }, RangeError));\n"
    "c('nf_unit', nf({ style: 'unit', unit: 'kilometer' }).format(5) === '5 km' &&\n"
    "    nf({ style: 'unit', unit: 'kilometer', unitDisplay: 'long' }).format(1) === '1 kilometer' &&\n"
    "    nf({ style: 'unit', unit: 'kilometer-per-hour', unitDisplay: 'long' }).format(2) ===\n"
    "    '2 kilometers per hour' &&\n"
    "    nf({ style: 'unit', unit: 'liter', unitDisplay: 'narrow' }).format(3) === '3L' &&\n"
    "    nf({ style: 'unit', unit: 'celsius' }).format(20) === '20\\u00b0C');\n"
    "c('nf_unit_errors', throws(function () { nf({ style: 'unit' }); }, TypeError) &&\n"
    "    throws(function () { nf({ style: 'unit', unit: 'parsec' }); }, RangeError));\n"
    "c('nf_bigint', nf().format(12345678901234567890n) === '12,345,678,901,234,567,890' &&\n"
    "    (-42n).toLocaleString() === '-42' && (1000n).toLocaleString('en-US') === '1,000' &&\n"
    "    nf({ style: 'percent' }).format(2n) === '200%');\n"
    "c('nf_format_parts', (function () {\n"
    "    var parts = new Intl.NumberFormat('en-US').formatToParts(1234.5);\n"
    "    return parts.map(function (p) { return p.type; }).join() ===\n"
    "        'integer,group,integer,decimal,fraction' && parts[0].value === '1' &&\n"
    "        parts[4].value === '5';\n"
    "})());\n"
    "c('nf_parts_currency', new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' })\n"
    "    .formatToParts(-1).map(function (p) { return p.type; }).join() ===\n"
    "    'minusSign,currency,integer,decimal,fraction');\n"
    "c('nf_resolved', (function () {\n"
    "    var r = nf({ style: 'currency', currency: 'eur' }).resolvedOptions();\n"
    "    return r.locale === 'en-US' && r.style === 'currency' && r.currency === 'EUR' &&\n"
    "        r.minimumFractionDigits === 2 && r.maximumFractionDigits === 2 &&\n"
    "        r.useGrouping === 'auto' && r.signDisplay === 'auto' && r.roundingMode === 'halfExpand';\n"
    "})());\n"
    "c('nf_format_bound', (function () {\n"
    "    var n = nf(); var f = n.format;\n"
    "    return f(1000) === '1,000' && n.format === n.format && f(2) === '2';\n"
    "})());\n"
    "c('nf_locale_resolves', nf().resolvedOptions().locale === 'en-US' &&\n"
    "    new Intl.NumberFormat('de-DE').resolvedOptions().locale === 'en-US' &&\n"
    "    new Intl.NumberFormat(['fr', 'en-GB']).resolvedOptions().locale === 'en-US');\n"
    "c('nf_call_without_new', Intl.NumberFormat().format(1) === '1' &&\n"
    "    Intl.NumberFormat() instanceof Intl.NumberFormat);\n"
    "c('coll_order', ['b', 'a', 'C', 'B'].sort(new Intl.Collator().compare).join('') === 'abBC');\n"
    "c('coll_localecompare', 'a'.localeCompare('b') === -1 && 'b'.localeCompare('a') === 1 &&\n"
    "    'a'.localeCompare('a') === 0 && 'a'.localeCompare('A') === -1);\n"
    "c('coll_sensitivity', new Intl.Collator('en', { sensitivity: 'base' }).compare('a', '\\u00e1') === 0 &&\n"
    "    new Intl.Collator('en', { sensitivity: 'base' }).compare('a', 'A') === 0 &&\n"
    "    new Intl.Collator('en', { sensitivity: 'accent' }).compare('a', '\\u00e1') === -1 &&\n"
    "    new Intl.Collator('en', { sensitivity: 'accent' }).compare('a', 'A') === 0 &&\n"
    "    new Intl.Collator('en', { sensitivity: 'case' }).compare('a', '\\u00e1') === 0 &&\n"
    "    new Intl.Collator('en', { sensitivity: 'case' }).compare('a', 'A') === -1);\n"
    "c('coll_numeric', new Intl.Collator(undefined, { numeric: true }).compare('2', '10') === -1 &&\n"
    "    new Intl.Collator().compare('2', '10') === 1 &&\n"
    "    ['item10', 'item2', 'item1'].sort(new Intl.Collator('en', { numeric: true }).compare)\n"
    "        .join() === 'item1,item2,item10');\n"
    "c('coll_resolved', (function () {\n"
    "    var r = new Intl.Collator('en-US', { sensitivity: 'base', numeric: true }).resolvedOptions();\n"
    "    return r.locale === 'en-US' && r.sensitivity === 'base' && r.numeric === true &&\n"
    "        r.usage === 'sort' && r.collation === 'default';\n"
    "})());\n"
    "c('coll_bad_sensitivity', throws(function () { new Intl.Collator('en', { sensitivity: 'x' }); },\n"
    "    RangeError));\n"
    "c('pr_cardinal', (function () {\n"
    "    var p = new Intl.PluralRules('en-US');\n"
    "    return p.select(1) === 'one' && p.select(0) === 'other' && p.select(2) === 'other' &&\n"
    "        p.select(1.5) === 'other' && p.select(-1) === 'one';\n"
    "})());\n"
    "c('pr_ordinal', (function () {\n"
    "    var p = new Intl.PluralRules('en-US', { type: 'ordinal' });\n"
    "    return p.select(1) === 'one' && p.select(2) === 'two' && p.select(3) === 'few' &&\n"
    "        p.select(4) === 'other' && p.select(11) === 'other' && p.select(12) === 'other' &&\n"
    "        p.select(13) === 'other' && p.select(21) === 'one' && p.select(22) === 'two' &&\n"
    "        p.select(101) === 'one' && p.select(111) === 'other';\n"
    "})());\n"
    "c('pr_requires_new', throws(function () { Intl.PluralRules(); }, TypeError));\n"
    "c('pr_resolved', new Intl.PluralRules('en').resolvedOptions().pluralCategories.join() ===\n"
    "    'one,other' &&\n"
    "    new Intl.PluralRules('en', { type: 'ordinal' }).resolvedOptions().pluralCategories.join() ===\n"
    "    'few,one,two,other');\n"
    "c('canonical_locales', Intl.getCanonicalLocales('EN-us').join() === 'en-US' &&\n"
    "    Intl.getCanonicalLocales(['de', 'en-gb', 'de']).join() === 'de,en-GB' &&\n"
    "    Intl.getCanonicalLocales('zh-hant-tw').join() === 'zh-Hant-TW' &&\n"
    "    Intl.getCanonicalLocales(undefined).length === 0 &&\n"
    "    Intl.getCanonicalLocales([]).length === 0);\n"
    "c('canonical_locales_errors', throws(function () { Intl.getCanonicalLocales('x'); }, RangeError) &&\n"
    "    throws(function () { Intl.getCanonicalLocales('en_US'); }, RangeError) &&\n"
    "    throws(function () { Intl.getCanonicalLocales('de-u-co-phonebk-u-co-pinyin'); }, RangeError) &&\n"
    "    throws(function () { Intl.getCanonicalLocales([1]); }, TypeError));\n"
    "c('supported_locales', Intl.NumberFormat.supportedLocalesOf(['en-US', 'de']).join() === 'en-US' &&\n"
    "    Intl.Collator.supportedLocalesOf('de').length === 0 &&\n"
    "    Intl.PluralRules.supportedLocalesOf(['en-GB']).length === 0 &&\n"
    "    Intl.PluralRules.supportedLocalesOf(['en-US-u-nu-latn']).join() === 'en-US-u-nu-latn');\n"
    "c('intl_shape', Object.prototype.toString.call(Intl) === '[object Intl]' &&\n"
    "    Intl.NumberFormat.name === 'NumberFormat' && Intl.NumberFormat.length === 0 &&\n"
    "    Intl.Collator.supportedLocalesOf.length === 1 &&\n"
    "    Object.prototype.toString.call(new Intl.NumberFormat()) === '[object Intl.NumberFormat]' &&\n"
    "    Object.prototype.toString.call(new Intl.PluralRules()) === '[object Intl.PluralRules]');\n"
    "c('brand_checks', throws(function () { Intl.NumberFormat.prototype.resolvedOptions.call({}); },\n"
    "    TypeError) && throws(function () { Object.getOwnPropertyDescriptor(\n"
    "    Intl.NumberFormat.prototype, 'format').get.call(1); }, TypeError));\n"
    "c('array_to_locale', [1234, 5678].toLocaleString() === '1,234,5,678' &&\n"
    "    [1, null, undefined, 2000].toLocaleString() === '1,,,2,000' &&\n"
    "    [].toLocaleString() === '');\n"
    "c('bigint_to_locale_shape', BigInt.prototype.toLocaleString.length === 0 &&\n"
    "    (10n ** 6n).toLocaleString() === '1,000,000');\n"
    "fails.join(',');\n";

/* Review fixes: shortest-digit rounding, NaN signs, bound function shape, cyclic and large arrays,
 * unsupported options, collation by code point, the option order and validation. */
static const char *intl_round2_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "function throws(f, E) { try { f(); return false; } catch (e) { return e instanceof E; } }\n"
    "var nf = function (o) { return new Intl.NumberFormat('en-US', o); };\n"
    "c('round_shortest', nf({ maximumFractionDigits: 2 }).format(1.005) === '1.01' &&\n"
    "    nf({ maximumFractionDigits: 1 }).format(1.15) === '1.2' &&\n"
    "    (0.1).toLocaleString('en', { maximumSignificantDigits: 21 }) === '0.1' &&\n"
    "    nf({ maximumFractionDigits: 2 }).format(8.345) === '8.35' &&\n"
    "    nf({ style: 'currency', currency: 'USD' }).format(1.255) === '$1.26');\n"
    "var names = ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand',\n"
    "    'halfTrunc', 'halfEven'];\n"
    "var pos = ['1.2', '1.1', '1.2', '1.1', '1.2', '1.1', '1.2', '1.1', '1.2'];\n"
    "var neg = ['-1.1', '-1.2', '-1.2', '-1.1', '-1.1', '-1.2', '-1.2', '-1.1', '-1.2'];\n"
    "c('rounding_ties', names.every(function (m, i) {\n"
    "    var o = { maximumFractionDigits: 1, roundingMode: m };\n"
    "    return nf(o).format(1.15) === pos[i] && nf(o).format(-1.15) === neg[i];\n"
    "}));\n"
    "c('nan_sign', nf({ signDisplay: 'always' }).format(NaN) === '+NaN' &&\n"
    "    nf().format(NaN) === 'NaN' && nf({ signDisplay: 'always' }).format(-0) === '-0' &&\n"
    "    nf({ signDisplay: 'exceptZero' }).format(0) === '0');\n"
    "c('use_grouping_strings', nf({ useGrouping: 'true' }).format(1234) === '1,234' &&\n"
    "    nf({ useGrouping: 'false' }).format(1234) === '1,234' &&\n"
    "    nf({ useGrouping: false }).format(1234) === '1234');\n"
    "c('bound_function_shape', (function () {\n"
    "    var n = nf(), g = new Intl.Collator().compare;\n"
    "    var d = Object.getOwnPropertyDescriptor(n.format, 'name');\n"
    "    return Object.getOwnPropertyNames(n.format).join() === 'length,name' &&\n"
    "        n.format.length === 1 && n.format.name === '' && !d.writable && !d.enumerable &&\n"
    "        d.configurable && Object.getOwnPropertyNames(g).join() === 'length,name' &&\n"
    "        g.length === 2 && g.name === '' && n.format === n.format &&\n"
    "        throws(function () { new n.format(); }, TypeError);\n"
    "})());\n"
    "c('array_cycle', throws(function () { var a = [1]; a.push(a); a.toLocaleString(); }, RangeError));\n"
    "c('array_large', (function () {\n"
    "    var big = new Array(30000);\n"
    "    big.fill(7);\n"
    "    return big.toLocaleString() === new Array(30000).fill('7').join(',');\n"
    "})());\n"
    "c('notation_rejected', throws(function () { nf({ notation: 'compact' }); }, RangeError) &&\n"
    "    throws(function () { nf({ notation: 'scientific' }); }, RangeError) &&\n"
    "    nf({ notation: 'standard' }).format(1) === '1');\n"
    "c('unsupported_digit_options', throws(function () { nf({ roundingIncrement: 5 }); }, RangeError) &&\n"
    "    throws(function () { nf({ roundingPriority: 'morePrecision' }); }, RangeError) &&\n"
    "    nf({ roundingIncrement: 1, roundingPriority: 'auto' }).format(1) === '1');\n"
    "c('numbering_system', throws(function () { nf({ numberingSystem: 'a' }); }, RangeError) &&\n"
    "    nf({ numberingSystem: 'arab' }).format(1) === '1' &&\n"
    "    nf({ numberingSystem: 'arab' }).resolvedOptions().numberingSystem === 'latn');\n"
    "c('strip_if_integer', nf({ trailingZeroDisplay: 'stripIfInteger', minimumFractionDigits: 2 })\n"
    "    .format(5) === '5' &&\n"
    "    nf({ trailingZeroDisplay: 'stripIfInteger', minimumFractionDigits: 2 }).format(5.5) === '5.50' &&\n"
    "    throws(function () { nf({ trailingZeroDisplay: 'x' }); }, RangeError));\n"
    "c('plural_fraction_digits', new Intl.PluralRules('en', { minimumFractionDigits: 1 }).select(1) ===\n"
    "    'other' && new Intl.PluralRules('en').select(1) === 'one' &&\n"
    "    new Intl.PluralRules('en', { type: 'ordinal', minimumFractionDigits: 1 }).select(1) === 'other');\n"
    "c('canonical_singletons', throws(function () { Intl.getCanonicalLocales('en-a-b-cc'); }, RangeError) &&\n"
    "    throws(function () { Intl.getCanonicalLocales('en-x'); }, RangeError) &&\n"
    "    Intl.getCanonicalLocales('en-a-cc').join() === 'en-a-cc' &&\n"
    "    Intl.getCanonicalLocales('en-x-abc').join() === 'en-x-abc');\n"
    "var coll = function (o) { return new Intl.Collator('en', o); };\n"
    "c('collation_base_case', coll({ sensitivity: 'base' }).compare('\\u0141', '\\u0142') === 0 &&\n"
    "    coll({ sensitivity: 'base' }).compare('\\u0391', '\\u03b1') === 0);\n"
    "c('collation_cyrillic', ['\\u0431', '\\u0410', '\\u0430', '\\u0411'].sort(coll().compare).join() ===\n"
    "    '\\u0430,\\u0410,\\u0431,\\u0411');\n"
    "c('collation_greek', ['\\u03b2', '\\u0392', '\\u03b1', '\\u0391'].sort(coll().compare).join() ===\n"
    "    '\\u03b1,\\u0391,\\u03b2,\\u0392');\n"
    "c('collation_code_points', coll().compare('\\ud83d\\ude00', '\\uff21') === 1 &&\n"
    "    coll().compare('\\uff21', '\\ud83d\\ude00') === -1);\n"
    "c('collation_ignore_punctuation', coll({ ignorePunctuation: true }).compare('a b', 'ab') === 0 &&\n"
    "    coll().compare('a b', 'ab') !== 0);\n"
    "c('collation_case_first', ['a', 'A'].sort(coll({ caseFirst: 'upper' }).compare).join() === 'A,a' &&\n"
    "    ['a', 'A'].sort(coll().compare).join() === 'a,A');\n"
    "c('option_validation', throws(function () { new Intl.Collator('en', { usage: 'x' }); }, RangeError) &&\n"
    "    throws(function () { new Intl.PluralRules('en', { type: 'x' }); }, RangeError));\n"
    "fails.join(',');\n";

/* Errors on the construction paths, which must free the state they own. The script returns the
 * names of the errors. */
static const char *intl_error_paths_script =
    "var out = [];\n"
    "function t(f) { try { f(); out.push('ok'); } catch (x) { out.push(x.name); } }\n"
    "var nt = new Proxy(function () {}, { get: function (t, k) {\n"
    "    if (k === 'prototype') { throw new Error('p'); }\n"
    "    return t[k];\n"
    "} });\n"
    "t(function () { Reflect.construct(Intl.NumberFormat, [], nt); });\n"
    "t(function () { Reflect.construct(Intl.DateTimeFormat, [], nt); });\n"
    "t(function () { Reflect.construct(Intl.Collator, [], nt); });\n"
    "t(function () { Reflect.construct(Intl.PluralRules, [], nt); });\n"
    "t(function () { new Intl.DateTimeFormat('en', { dateStyle: 'short', month: 'long' }); });\n"
    "t(function () { new Intl.NumberFormat('en', { notation: 'compact' }); });\n"
    "t(function () { new Intl.Collator('en', { usage: 'x' }); });\n"
    "t(function () { Intl.getCanonicalLocales('en-a-b'); });\n"
    "t(function () { new Intl.NumberFormat('en', { maximumFractionDigits: 101 }); });\n"
    "out.join();\n";

EMB_TEST(intl_round2_checks_pass)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, intl_round2_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(intl_error_paths_free_state)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, intl_error_paths_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    EMB_CHECK(es6_text_is(e.rt, v, "Error,Error,Error,Error,TypeError,RangeError,RangeError,"
                                   "RangeError,RangeError"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(intl_js_checks_pass)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, intl_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(intl_error_messages)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, "try { new Intl.NumberFormat('en', { style: 'currency' }); } "
                                 "catch (x) { x.message }");
    EMB_CHECK(es6_text_is(e.rt, v, "Currency code is required with currency style."));
    v = es6_run(e, "try { Intl.getCanonicalLocales('x'); } catch (x) { x.name }");
    EMB_CHECK(es6_text_is(e.rt, v, "RangeError"));
    v = es6_run(e, "typeof Intl.DateTimeFormat.prototype.formatToParts");
    EMB_CHECK(es6_text_is(e.rt, v, "function"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
