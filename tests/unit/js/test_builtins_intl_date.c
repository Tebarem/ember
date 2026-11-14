/* Intl.DateTimeFormat and the Date toLocale*String methods. The
 * checks use the UTC zone, plus one pinned local offset through the Date test hook, so they do not
 * depend on the machine's time zone. Each script returns the labels of the checks that failed. */
#include "es6_util.h"

#include <stdint.h>

void embi_js_date_test_pin_offset(int32_t minutes);
void embi_js_date_test_unpin(void);

static const char *intl_date_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "function throws(f, E) { try { f(); return false; } catch (e) { return e instanceof E; } }\n"
    "var d = new Date(Date.UTC(2020, 0, 15, 15, 4, 5));\n"
    "var U = { timeZone: 'UTC' };\n"
    "function dtf(o) { return new Intl.DateTimeFormat('en-US', o); }\n"
    "c('default_strings', d.toLocaleString('en-US', U) === '1/15/2020, 3:04:05 PM' &&\n"
    "    d.toLocaleDateString('en-US', U) === '1/15/2020' &&\n"
    "    d.toLocaleTimeString('en-US', U) === '3:04:05 PM');\n"
    "c('date_styles', dtf({ timeZone: 'UTC', dateStyle: 'full' }).format(d) ===\n"
    "    'Wednesday, January 15, 2020' &&\n"
    "    dtf({ timeZone: 'UTC', dateStyle: 'long' }).format(d) === 'January 15, 2020' &&\n"
    "    dtf({ timeZone: 'UTC', dateStyle: 'medium' }).format(d) === 'Jan 15, 2020' &&\n"
    "    dtf({ timeZone: 'UTC', dateStyle: 'short' }).format(d) === '1/15/20');\n"
    "c('time_styles', dtf({ timeZone: 'UTC', timeStyle: 'full' }).format(d) ===\n"
    "    '3:04:05 PM Coordinated Universal Time' &&\n"
    "    dtf({ timeZone: 'UTC', timeStyle: 'long' }).format(d) === '3:04:05 PM UTC' &&\n"
    "    dtf({ timeZone: 'UTC', timeStyle: 'medium' }).format(d) === '3:04:05 PM' &&\n"
    "    dtf({ timeZone: 'UTC', timeStyle: 'short' }).format(d) === '3:04 PM');\n"
    "c('combined_styles', dtf({ timeZone: 'UTC', dateStyle: 'full', timeStyle: 'short' }).format(d) ===\n"
    "    'Wednesday, January 15, 2020 at 3:04 PM' &&\n"
    "    dtf({ timeZone: 'UTC', dateStyle: 'medium', timeStyle: 'short' }).format(d) ===\n"
    "    'Jan 15, 2020, 3:04 PM');\n"
    "c('style_conflicts', throws(function () {\n"
    "    dtf({ timeZone: 'UTC', dateStyle: 'short', month: 'long' }); }, TypeError));\n"
    "c('text_month', dtf({ month: 'long', day: 'numeric', year: 'numeric', timeZone: 'UTC' })\n"
    "    .format(d) === 'January 15, 2020' &&\n"
    "    dtf({ weekday: 'short', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(d) ===\n"
    "    'Wed, Jan 15' &&\n"
    "    dtf({ month: 'long', year: 'numeric', timeZone: 'UTC' }).format(d) === 'January 2020' &&\n"
    "    dtf({ month: 'long', timeZone: 'UTC' }).format(d) === 'January');\n"
    "c('numeric_fields', dtf({ year: 'numeric', month: '2-digit', day: '2-digit', timeZone: 'UTC' })\n"
    "    .format(d) === '01/15/2020' &&\n"
    "    dtf({ month: 'numeric', day: 'numeric', timeZone: 'UTC' }).format(d) === '1/15' &&\n"
    "    dtf({ year: '2-digit', timeZone: 'UTC' }).format(d) === '20' &&\n"
    "    dtf({ weekday: 'long', timeZone: 'UTC' }).format(d) === 'Wednesday' &&\n"
    "    dtf({ weekday: 'narrow', month: 'long', timeZone: 'UTC' }).format(d) === 'W, January');\n"
    "c('time_fields', dtf({ hour: 'numeric', timeZone: 'UTC' }).format(d) === '3 PM' &&\n"
    "    dtf({ hour: '2-digit', minute: '2-digit', hour12: false, timeZone: 'UTC' }).format(d) ===\n"
    "    '15:04' &&\n"
    "    dtf({ hour: 'numeric', minute: '2-digit', hourCycle: 'h23', timeZone: 'UTC' }).format(d) ===\n"
    "    '15:04' &&\n"
    "    dtf({ hour: 'numeric', hour12: true, timeZone: 'UTC' })\n"
    "        .format(new Date(Date.UTC(2020, 0, 1, 0, 0, 0))) === '12 AM' &&\n"
    "    dtf({ hour: 'numeric', hourCycle: 'h11', timeZone: 'UTC' })\n"
    "        .format(new Date(Date.UTC(2020, 0, 1, 0, 0, 0))) === '0 AM');\n"
    "c('fractional_seconds', dtf({ hour: 'numeric', minute: '2-digit', second: '2-digit',\n"
    "    fractionalSecondDigits: 3, timeZone: 'UTC' })\n"
    "    .format(new Date(Date.UTC(2020, 0, 1, 0, 0, 0, 123))) === '12:00:00.123 AM');\n"
    "c('time_zone_name', d.toLocaleString('en-US', { timeZone: 'UTC', timeZoneName: 'short' }) ===\n"
    "    '1/15/2020, 3:04:05 PM UTC' &&\n"
    "    dtf({ timeZone: 'UTC', timeZoneName: 'long', hour: 'numeric' }).format(d) ===\n"
    "    '3 PM Coordinated Universal Time');\n"
    "c('time_zone_names', dtf({ timeZone: 'utc' }).resolvedOptions().timeZone === 'UTC' &&\n"
    "    dtf({ timeZone: 'Etc/UTC' }).resolvedOptions().timeZone === 'UTC' &&\n"
    "    throws(function () { dtf({ timeZone: 'Mars/Olympus' }); }, RangeError));\n"
    "c('parts', dtf({ timeZone: 'UTC', year: 'numeric', month: 'numeric', day: 'numeric' })\n"
    "    .formatToParts(d).map(function (p) { return p.type; }).join() ===\n"
    "    'month,literal,day,literal,year' &&\n"
    "    dtf({ timeZone: 'UTC', hour: 'numeric', minute: '2-digit' }).formatToParts(d)\n"
    "        .map(function (p) { return p.type; }).join() === 'hour,literal,minute,literal,dayPeriod');\n"
    "c('resolved', (function () {\n"
    "    var r = dtf({ timeZone: 'UTC' }).resolvedOptions();\n"
    "    return r.locale === 'en-US' && r.calendar === 'gregory' && r.numberingSystem === 'latn' &&\n"
    "        r.year === 'numeric' && r.month === 'numeric' && r.day === 'numeric' &&\n"
    "        r.hour === undefined;\n"
    "})());\n"
    "c('resolved_hour', (function () {\n"
    "    var r = dtf({ hour: 'numeric', timeZone: 'UTC' }).resolvedOptions();\n"
    "    return r.hour === 'numeric' && r.hourCycle === 'h12' && r.hour12 === true &&\n"
    "        r.year === undefined;\n"
    "})());\n"
    "c('resolved_style', dtf({ dateStyle: 'medium', timeZone: 'UTC' }).resolvedOptions().dateStyle ===\n"
    "    'medium' && dtf({ dateStyle: 'medium', timeZone: 'UTC' }).resolvedOptions().year === undefined);\n"
    "c('number_argument', dtf({ timeZone: 'UTC' }).format(0) === '1/1/1970' &&\n"
    "    dtf({ timeZone: 'UTC' }).format(Date.UTC(2020, 0, 15)) === '1/15/2020' &&\n"
    "    typeof dtf().format() === 'string' && dtf().format() !== '');\n"
    "c('format_errors', throws(function () { dtf().format(NaN); }, RangeError) &&\n"
    "    throws(function () { dtf().format(8.64e15 + 1); }, RangeError));\n"
    "c('invalid_date', new Date(NaN).toLocaleString() === 'Invalid Date' &&\n"
    "    new Date(NaN).toLocaleDateString('en-US', U) === 'Invalid Date');\n"
    "c('bound_format', (function () { var o = dtf(U); var f = o.format;\n"
    "    return f(0) === '1/1/1970' && o.format === o.format; })());\n"
    "c('date_method_receiver', throws(function () { Date.prototype.toLocaleString.call(1); },\n"
    "    TypeError));\n"
    "c('date_method_shapes', Date.prototype.toLocaleString.length === 0 &&\n"
    "    Date.prototype.toLocaleDateString.name === 'toLocaleDateString' &&\n"
    "    Date.prototype.toLocaleTimeString.length === 0);\n"
    "fails.join(',');\n";

static const char *intl_local_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "var d = new Date(Date.UTC(2020, 0, 15, 15, 4, 5));\n"
    "c('local_offset', d.toLocaleString('en-US') === '1/15/2020, 4:04:05 PM' &&\n"
    "    d.toLocaleTimeString('en-US') === '4:04:05 PM' &&\n"
    "    new Intl.DateTimeFormat('en-US', { hour: 'numeric', hour12: false }).format(d) === '16');\n"
    "c('local_zone_name', d.toLocaleString('en-US', { timeZoneName: 'short' }) ===\n"
    "    '1/15/2020, 4:04:05 PM GMT+1');\n"
    "c('utc_still_utc', d.toLocaleString('en-US', { timeZone: 'UTC' }) === '1/15/2020, 3:04:05 PM');\n"
    "c('local_resolved', new Intl.DateTimeFormat('en-US').resolvedOptions().timeZone === 'Etc/GMT-1');\n"
    "c('local_round_trip', (function () {\n"
    "    var z = new Intl.DateTimeFormat('en-US').resolvedOptions().timeZone;\n"
    "    return new Intl.DateTimeFormat('en-US', { timeZone: z, hour: 'numeric', timeZoneName: 'short' })\n"
    "        .format(d) === '4 PM GMT+1';\n"
    "})());\n"
    "c('local_getter_patch', (function () {\n"
    "    var p = Date.prototype, saved = p.getTimezoneOffset, r;\n"
    "    p.getTimezoneOffset = function () { return 0; };\n"
    "    r = d.toLocaleTimeString('en-US') === '4:04:05 PM';\n"
    "    p.getTimezoneOffset = saved;\n"
    "    return r;\n"
    "})());\n"
    "fails.join(',');\n";

/* Review fixes: style and option requirements, zone names and fixed offsets, patched Date
 * prototype methods (the output must not change), formatToParts literals and option validation. */
static const char *intl_date_round2_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "function throws(f, E) { try { f(); return false; } catch (e) { return e instanceof E; } }\n"
    "var d = new Date(Date.UTC(2020, 0, 15, 15, 4, 5));\n"
    "var U = { timeZone: 'UTC' };\n"
    "function dtf(o) { return new Intl.DateTimeFormat('en-US', o); }\n"
    "c('style_requirements', throws(function () {\n"
    "    d.toLocaleDateString('en-US', { timeStyle: 'short', timeZone: 'UTC' }); }, TypeError) &&\n"
    "    throws(function () { d.toLocaleTimeString('en-US', { dateStyle: 'short', timeZone: 'UTC' }); },\n"
    "        TypeError) && new Date(0).toLocaleDateString('en-US', U) === '1/1/1970');\n"
    "c('zone_names', dtf({ timeZone: 'Etc/GMT-5' }).resolvedOptions().timeZone === 'Etc/GMT-5' &&\n"
    "    dtf({ timeZone: '+05:30' }).resolvedOptions().timeZone === '+05:30' &&\n"
    "    dtf({ timeZone: 'GMT' }).resolvedOptions().timeZone === 'UTC' &&\n"
    "    throws(function () { dtf({ timeZone: '+25:00' }); }, RangeError) &&\n"
    "    throws(function () { dtf({ timeZone: 'Etc/GMT-15' }); }, RangeError));\n"
    "c('fixed_offsets', dtf({ timeZone: 'Etc/GMT-5', hour: 'numeric', timeZoneName: 'short' })\n"
    "    .format(d) === '8 PM GMT+5' &&\n"
    "    dtf({ timeZone: '-03:30', hour: 'numeric', minute: '2-digit', timeZoneName: 'short' })\n"
    "        .format(d) === '11:34 AM GMT-3:30');\n"
    "c('zero_offset_names', dtf({ timeZone: 'UTC', hour: 'numeric', timeZoneName: 'shortOffset' })\n"
    "    .format(d) === '3 PM GMT' && dtf({ timeZone: 'UTC', hour: 'numeric', timeZoneName: 'short' })\n"
    "    .format(d) === '3 PM UTC');\n"
    "c('getter_patches', (function () {\n"
    "    var p = Date.prototype, saved = p.getUTCMonth, hours = Object.getOwnPropertyDescriptor(p, 'getUTCHours'), r;\n"
    "    p.getUTCMonth = function () { return 5; };\n"
    "    delete p.getUTCHours;\n"
    "    r = d.toLocaleDateString('en-US', U) === '1/15/2020' &&\n"
    "        d.toLocaleTimeString('en-US', U) === '3:04:05 PM' && dtf(U).format(0) === '1/1/1970';\n"
    "    p.getUTCMonth = saved;\n"
    "    Object.defineProperty(p, 'getUTCHours', hours);\n"
    "    return r;\n"
    "})());\n"
    "c('getter_restored', d.toLocaleTimeString('en-US', U) === '3:04:05 PM');\n"
    "c('fraction_alone', dtf({ fractionalSecondDigits: 3, timeZone: 'UTC' }).format(0) === '000' &&\n"
    "    dtf({ fractionalSecondDigits: 3, timeZone: 'UTC' }).formatToParts(0).length === 1 &&\n"
    "    dtf({ minute: '2-digit', fractionalSecondDigits: 2, timeZone: 'UTC' })\n"
    "        .format(new Date(Date.UTC(2020, 0, 1, 0, 5, 0, 120))) === '05.12' &&\n"
    "    dtf({ timeZone: 'UTC', timeZoneName: 'shortGeneric', hour: 'numeric' }).format(d) === '3 PM GMT' &&\n"
    "    throws(function () { dtf({ timeZone: 'Etc/GMT-01' }); }, RangeError));\n"
    "c('fraction_parts', dtf({ timeZone: 'UTC', hour: 'numeric', minute: '2-digit', second: '2-digit',\n"
    "    fractionalSecondDigits: 3 }).formatToParts(new Date(Date.UTC(2020, 0, 1, 0, 0, 0, 123)))\n"
    "    .map(function (p) { return p.type; }).join() ===\n"
    "    'hour,literal,minute,literal,second,literal,fractionalSecond,literal,dayPeriod');\n"
    "c('option_validation', throws(function () { dtf({ calendar: 'x' }); }, RangeError) &&\n"
    "    throws(function () { dtf({ era: 'long' }); }, RangeError) &&\n"
    "    throws(function () { dtf({ dayPeriod: 'short' }); }, RangeError) &&\n"
    "    throws(function () { dtf({ formatMatcher: 'x' }); }, RangeError) &&\n"
    "    dtf({ calendar: 'islamic', formatMatcher: 'basic', timeZone: 'UTC' }).format(d) === '1/15/2020');\n"
    "fails.join(',');\n";

EMB_TEST(intl_date_round2_checks_pass)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, intl_date_round2_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(intl_date_js_checks_pass)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, intl_date_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(intl_date_local_zone_checks_pass)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_date_test_pin_offset(60);
    embi_js_value v = es6_run(e, intl_local_script);
    embi_js_date_test_unpin();
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
