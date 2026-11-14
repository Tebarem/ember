/* Date (J4d): the constructor forms, the statics, the UTC and local
 * getters and setters, toJSON, @@toPrimitive, the Annex B methods, and the local time rules
 * (the offset is pinned through the test hook, so the results do not depend on the machine's time
 * zone). Each test checks that the runtime's live memory returns to its baseline. */
#include "es6_util.h"

#include "js/builtins.h"

/* The test hook of builtins_date.c. */
void embi_js_date_test_pin_offset(int32_t minutes);
void embi_js_date_test_pin_zone(int32_t before_min, int32_t after_min, double transition_ms);
void embi_js_date_test_unpin(void);

/* An environment with the well-known symbol @@toPrimitive as the global S_toPrimitive. */
static es6_env date_env_new(void)
{
    es6_env e = es6_env_new();
    if (e.r) {
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_toPrimitive"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_TO_PRIMITIVE), 0);
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_iterator"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_ITERATOR), 0);
    }
    return e;
}

/* 2026-11-09T00:00:00Z and 2000-01-01T00:00:00Z. */
#define NOV9_2026_MS 1794182400000
#define Y2000_MS 946684800000

EMB_TEST(date_shape_and_statics)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "typeof Date === 'function' && Date.length === 7 && Date.name === 'Date'", true);
    E6_BOOL(e, "Date.UTC.length === 7 && Date.parse.length === 1 && Date.now.length === 0", true);
    E6_BOOL(e, "Date.prototype.constructor === Date", true);
    E6_BOOL(e, "Date.prototype.toGMTString === Date.prototype.toUTCString", true);
    E6_BOOL(e, "Object.getOwnPropertyDescriptor(Date, 'prototype').writable === false", true);
    E6_BOOL(e, "Object.getPrototypeOf(new Date(0)) === Date.prototype", true);
    E6_BOOL(e, "typeof Date() === 'string' && typeof Date.now() === 'number'", true);
    E6_BOOL(e, "Date.now() > 1600000000000 && Date.now() % 1 === 0", true);
    E6_BOOL(e, "Date.prototype.getYear.length === 0 && Date.prototype.setHours.length === 4", true);
    E6_BOOL(e, "Date.prototype.setMonth.length === 2 && Date.prototype.toJSON.length === 1", true);
    E6_BOOL(e, "Date.prototype.setFullYear.length === 3 && Date.prototype.setYear.length === 1",
            true);
    E6_BOOL(e, "Date.prototype[S_toPrimitive].length === 1", true);
    E6_BOOL(e, "Object.getOwnPropertyDescriptor(Date.prototype, S_toPrimitive).writable === false",
            true);
    E6_THROWS(e, "Date.prototype.getTime.call({})", "TypeError");
    E6_THROWS(e, "Date.prototype.getTime.call(Date.prototype)", "TypeError");
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_constructor_forms)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "new Date(0).getTime() === 0 && 1 / new Date(-0).getTime() === Infinity", true);
    E6_BOOL(e, "new Date(8.64e15).getTime() === 8.64e15", true);
    E6_BOOL(e, "isNaN(new Date(8.64e15 + 1).getTime())", true);
    E6_BOOL(e, "isNaN(new Date(-8.64e15 - 1).getTime())", true);
    E6_BOOL(e, "new Date(12.9).getTime() === 12", true);
    E6_BOOL(e, "new Date(-12.9).getTime() === -12", true);
    E6_BOOL(e, "new Date(null).getTime() === 0 && isNaN(new Date(undefined).getTime())", true);
    E6_BOOL(e, "isNaN(new Date('x').getTime()) && isNaN(new Date(NaN).getTime())", true);
    /* Local midnight of 2000-01-01 at +05:30 is 5.5 hours before the UTC midnight. */
    E6_BOOL(e, "new Date(2000, 0).getTime() === 946665000000", true);
    E6_BOOL(e, "new Date(2000, 0, 1, 0, 0, 0, 0).getTime() === 946665000000", true);
    E6_BOOL(e, "new Date(2026, 10, 9, 10).getTime() === 1794198600000", true);
    E6_TEXT(e, "new Date(2026, 10, 9, 10).toISOString()", "2026-11-09T04:30:00.000Z");
    /* Two-digit years are 19xx, and month overflow carries into the year. */
    E6_BOOL(e, "new Date(99, 0).getFullYear() === 1999 && new Date(100, 0).getFullYear() === 100",
            true);
    E6_BOOL(e, "new Date(2026, 12, 1).getFullYear() === 2027", true);
    E6_BOOL(e, "new Date(2026, 12, 1).getMonth() === 0", true);
    E6_BOOL(e, "new Date(2026, 0, 0).getDate() === 31 && new Date(2026, 0, 0).getMonth() === 11",
            true);
    E6_BOOL(e, "isNaN(new Date(2026, NaN).getTime()) && isNaN(new Date(2026, 0, Infinity))", true);
    /* A Date argument copies its time value; other objects go through ToPrimitive (default). */
    E6_BOOL(e, "new Date(new Date(5)).getTime() === 5", true);
    E6_BOOL(e, "new Date({ valueOf: function () { return 7; } }).getTime() === 7", true);
    E6_BOOL(e, "new Date({ toString: function () { return '1970-01-01T00:00:01Z'; },"
               "  valueOf: function () { return 9; } }).getTime() === 9", true);
    E6_THROWS(e, "new Date(S_iterator)", "TypeError");
    /* Called without new, Date returns a string, whatever the arguments are. */
    E6_BOOL(e, "typeof Date(2000, 1, 1) === 'string' && Date(0).length > 20", true);
    /* The prototype comes from new.target. */
    E6_BOOL(e, "function Sub() {} Sub.prototype = Object.create(Date.prototype);"
               "Object.getPrototypeOf(Reflect.construct(Date, [0], Sub)) === Sub.prototype", true);
    E6_BOOL(e, "Object.getPrototypeOf(Reflect.construct(Date, [0], Object)) === Object.prototype"
               " || Object.getPrototypeOf(Reflect.construct(Date, [0], Object)) === Date.prototype",
            true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_statics)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "Date.UTC(2000) === 946684800000", true);
    E6_BOOL(e, "Date.UTC(2000, 0, 1, 0, 0, 0, 0, 999) === 946684800000", true);
    E6_BOOL(e, "Date.UTC(99, 0) === Date.UTC(1999, 0)", true);
    E6_BOOL(e, "isNaN(Date.UTC()) && isNaN(Date.UTC(2000, NaN))", true);
    E6_BOOL(e, "Date.UTC(1970, 0, 1, 0, 0, 0, 0) === 0", true);
    E6_BOOL(e, "Date.UTC(2026, 10, 9, 10) === 1794218400000", true);
    E6_BOOL(e, "Date.UTC(2026, 10, 9, 10) === new Date(Date.UTC(2026, 10, 9, 10)).getTime()", true);
    E6_BOOL(e, "Date.parse('1970-01-01T00:00:00Z') === 0", true);
    E6_BOOL(e, "Date.now() - new Date().getTime() < 60000", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_utc_and_local_getters)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* 2026-11-09T10:20:30.456Z is 15:50:30.456 at +05:30. */
    const char *setup = "var d = new Date(Date.UTC(2026, 10, 9, 10, 20, 30, 456));";
    embi_js_eval(e.r, setup, strlen(setup), "date.js", 0);
    E6_BOOL(e, "d.getUTCHours() === 10 && d.getUTCMinutes() === 20 && d.getUTCSeconds() === 30",
            true);
    E6_BOOL(e, "d.getUTCMilliseconds() === 456 && d.getUTCDate() === 9", true);
    E6_BOOL(e, "d.getUTCMonth() === 10 && d.getUTCFullYear() === 2026 && d.getUTCDay() === 1", true);
    E6_BOOL(e, "d.getHours() === 15 && d.getMinutes() === 50 && d.getSeconds() === 30", true);
    E6_BOOL(e, "d.getMilliseconds() === 456 && d.getDate() === 9 && d.getDay() === 1", true);
    E6_BOOL(e, "d.getMonth() === 10 && d.getFullYear() === 2026", true);
    E6_BOOL(e, "d.getTimezoneOffset() === -330 && d.getYear() === 126", true);
    E6_BOOL(e, "d.valueOf() === d.getTime() && d.getTime() === 1794219630456", true);
    /* A local time just after midnight is still on the previous UTC day. */
    E6_BOOL(e, "new Date(2026, 10, 9, 0, 30).getUTCDate() === 8", true);
    E6_BOOL(e, "isNaN(new Date(NaN).getHours()) && isNaN(new Date(NaN).getTimezoneOffset())", true);
    E6_BOOL(e, "isNaN(new Date(NaN).getUTCFullYear()) && isNaN(new Date(NaN).getYear())", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_setters)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var d = new Date(0); d.setUTCHours(1); d.getTime() === 3600000", true);
    /* Local midnight of 1970-01-01 at +05:30 is -19800000. */
    E6_BOOL(e, "var d = new Date(0); d.setHours(0, 0, 0, 0); d.getTime() === -19800000", true);
    E6_BOOL(e, "var d = new Date(0); d.setMonth(1); d.getMonth() === 1", true);
    E6_BOOL(e, "var d = new Date(0); d.setMonth(1, 15); d.getDate() === 15", true);
    E6_BOOL(e, "var d = new Date(0); d.setFullYear(2000, 1, 29);"
               " d.getFullYear() === 2000 && d.getMonth() === 1 && d.getDate() === 29", true);
    E6_BOOL(e, "var d = new Date(0); d.setDate(0); d.getDate() === 31 && d.getMonth() === 11 &&"
               " d.getFullYear() === 1969", true);
    E6_BOOL(e, "var d = new Date(0); d.setMilliseconds(1500); d.getSeconds() === 1 &&"
               " d.getMilliseconds() === 500", true);
    E6_BOOL(e, "var d = new Date(0); d.setSeconds(30, 250);"
               " d.getSeconds() === 30 && d.getMilliseconds() === 250", true);
    /* Local 05:30 plus 90 minutes is 07:00 at +05:30 minus... i.e. hour 6, minute 30 */
    E6_BOOL(e, "var d = new Date(0); d.setMinutes(90); d.getHours() === 6 && d.getMinutes() === 30",
            true);
    E6_BOOL(e, "var d = new Date(0); d.setTime(12345) === 12345 && d.getTime() === 12345", true);
    E6_BOOL(e, "var d = new Date(0); isNaN(d.setTime(8.64e15 + 1)) && isNaN(d.getTime())", true);
    E6_BOOL(e, "var d = new Date(0); d.setYear(99); d.getFullYear() === 1999", true);
    E6_BOOL(e, "var d = new Date(0); d.setYear(2005); d.getFullYear() === 2005", true);
    E6_BOOL(e, "var d = new Date(0); isNaN(d.setYear(NaN)) && isNaN(d.getTime())", true);
    /* A setter on an invalid date gives NaN, except the year setters, which start from +0 (the
     * UTC epoch, read as a local midnight of 2000-01-01 here). */
    E6_BOOL(e, "var d = new Date(NaN); isNaN(d.setHours(1)) && isNaN(d.getTime())", true);
    E6_BOOL(e, "var d = new Date(NaN); d.setFullYear(2000); d.getFullYear() === 2000 &&"
               " d.getMonth() === 0 && d.getDate() === 1 && d.getHours() === 0", true);
    E6_BOOL(e, "var d = new Date(NaN); d.setUTCFullYear(2001, 1, 1) === Date.UTC(2001, 1, 1)", true);
    E6_BOOL(e, "var d = new Date(0); d.setUTCFullYear(2001, 1, 1) === Date.UTC(2001, 1, 1)", true);
    /* The time value is read before the arguments convert: a valueOf that invalidates the date
     * does not change the result of a setter that started from a valid one. */
    E6_BOOL(e, "var d = new Date(0); var r = d.setHours({ valueOf: function () {"
               " d.setTime(NaN); return 1; } }); !isNaN(r) && d.getTime() === r", true);
    /* Arguments are converted even when the date is invalid, and extra ones are ignored. */
    E6_THROWS(e, "new Date(NaN).setHours({ valueOf: function () { throw new TypeError('x'); } })",
              "TypeError");
    E6_BOOL(e, "var n = 0; var d = new Date(NaN);"
               " var c = { valueOf: function () { n++; return 1; } };"
               " d.setMinutes(1, c, 1, c) !== d.setMinutes(1) && n === 1", true);
    E6_BOOL(e, "var d = new Date(0); d.setHours(1, 2, 3, 4, 5, 6); d.getHours() === 1 &&"
               " d.getMinutes() === 2 && d.getMilliseconds() === 4", true);
    /* The setter's result is the new time value. */
    E6_BOOL(e, "var d = new Date(0); d.setHours(2) === d.getTime()", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_string_formats_utc)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(0);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "new Date(0).toString()",
            "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)");
    E6_TEXT(e, "new Date(0).toDateString()", "Thu Jan 01 1970");
    E6_TEXT(e, "new Date(0).toTimeString()", "00:00:00 GMT+0000 (Coordinated Universal Time)");
    E6_TEXT(e, "new Date(0).toUTCString()", "Thu, 01 Jan 1970 00:00:00 GMT");
    E6_TEXT(e, "new Date(0).toGMTString()", "Thu, 01 Jan 1970 00:00:00 GMT");
    E6_TEXT(e, "new Date(0).toISOString()", "1970-01-01T00:00:00.000Z");
    E6_TEXT(e, "new Date(0).toLocaleString()", "1/1/1970, 12:00:00 AM");
    E6_TEXT(e, "new Date(0).toLocaleDateString()", "1/1/1970");
    E6_TEXT(e, "new Date(0).toLocaleTimeString()", "12:00:00 AM");
    E6_TEXT(e, "new Date(Date.UTC(2026, 10, 9, 13, 5, 7)).toLocaleString()",
            "11/9/2026, 1:05:07 PM");
    E6_TEXT(e, "new Date(Date.UTC(2026, 10, 9, 12, 0, 0)).toLocaleTimeString()", "12:00:00 PM");
    /* The extremes of the time value range. */
    E6_TEXT(e, "new Date(8.64e15).toISOString()", "+275760-09-13T00:00:00.000Z");
    E6_TEXT(e, "new Date(-8.64e15).toISOString()", "-271821-04-20T00:00:00.000Z");
    E6_TEXT(e, "new Date(Date.UTC(-1, 0, 1)).toISOString()", "-000001-01-01T00:00:00.000Z");
    E6_TEXT(e, "new Date(Date.UTC(9999, 11, 31, 23, 59, 59, 999)).toISOString()",
            "9999-12-31T23:59:59.999Z");
    E6_BOOL(e, "new Date(Date.UTC(-1, 0, 1)).toString().indexOf(' -0001 ') > 0", true);
    E6_TEXT(e, "new Date(Date.UTC(2026, 10, 9)).toJSON()", "2026-11-09T00:00:00.000Z");
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_string_formats_local)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "new Date(0).toString()",
            "Thu Jan 01 1970 05:30:00 GMT+0530 (UTC+05:30)");
    E6_TEXT(e, "new Date(0).toTimeString()", "05:30:00 GMT+0530 (UTC+05:30)");
    E6_TEXT(e, "new Date(0).toLocaleString()", "1/1/1970, 5:30:00 AM");
    embi_js_date_test_pin_offset(-300);
    E6_TEXT(e, "new Date(0).toTimeString()", "19:00:00 GMT-0500 (UTC-05:00)");
    embi_js_date_test_pin_offset(-570);
    E6_TEXT(e, "new Date(0).toString()", "Wed Dec 31 1969 14:30:00 GMT-0930 (UTC-09:30)");
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_invalid_values)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "new Date(NaN).toString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toDateString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toTimeString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toUTCString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toLocaleString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toLocaleDateString()", "Invalid Date");
    E6_TEXT(e, "new Date(NaN).toLocaleTimeString()", "Invalid Date");
    E6_THROWS(e, "new Date(NaN).toISOString()", "RangeError");
    E6_THROWS(e, "Date.prototype.toISOString.call(new Date(8.64e15 + 1))", "RangeError");
    E6_TEXT(e, "JSON.stringify(new Date(NaN))", "null");
    E6_TEXT(e, "JSON.stringify({ d: new Date(0) })", "{\"d\":\"1970-01-01T00:00:00.000Z\"}");
    /* toJSON calls toISOString on any object, after ToPrimitive with the number hint. */
    E6_TEXT(e, "Date.prototype.toJSON.call({ toISOString: function () { return 'x'; },"
               " valueOf: function () { return 1; } })", "x");
    E6_BOOL(e, "Date.prototype.toJSON.call({ valueOf: function () { return NaN; } }) === null",
            true);
    E6_THROWS(e, "Date.prototype.toJSON.call({ valueOf: function () { return 1; } })", "TypeError");
    E6_THROWS(e, "Date.prototype.toJSON.call(null)", "TypeError");
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_to_primitive)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(0);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "new Date(5)[S_toPrimitive]('number') === 5", true);
    E6_TEXT(e, "new Date(0)[S_toPrimitive]('string')",
            "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)");
    E6_TEXT(e, "new Date(0)[S_toPrimitive]('default')",
            "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)");
    E6_BOOL(e, "(new Date(0) + '') === new Date(0).toString()", true);
    E6_BOOL(e, "new Date(5) - 0 === 5 && +new Date(7) === 7", true);
    E6_BOOL(e, "Date.prototype[S_toPrimitive].call({ toString: function () { return 1; },"
               " valueOf: function () { return 2; } }, 'number') === 2", true);
    E6_BOOL(e, "Date.prototype[S_toPrimitive].call({ toString: function () { return 'a'; },"
               " valueOf: function () { return {}; } }, 'number') === 'a'", true);
    E6_THROWS(e, "new Date(0)[S_toPrimitive]('bogus')", "TypeError");
    E6_THROWS(e, "new Date(0)[S_toPrimitive]()", "TypeError");
    E6_THROWS(e, "Date.prototype[S_toPrimitive].call(1, 'string')", "TypeError");
    E6_THROWS(e, "Date.prototype[S_toPrimitive].call({ toString: function () { return {}; },"
                 " valueOf: function () { return {}; } }, 'default')", "TypeError");
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_annex_b)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "new Date(0).getYear() === 70 && new Date(Date.UTC(2026, 0, 1)).getYear() === 126",
            true);
    E6_BOOL(e, "var d = new Date(0); d.setYear(2000); d.getFullYear() === 2000", true);
    E6_BOOL(e, "var d = new Date(0); d.setYear(0); d.getFullYear() === 1900", true);
    E6_BOOL(e, "Date.prototype.toGMTString.name === 'toUTCString'", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The DST rules of a zone with one change at a time: PST (-480) to PDT (-420) at 2026-03-08 10:00Z
 * (spring), and PDT to PST at 2026-11-01 09:00Z (fall). A local time in the gap is read with the
 * offset before the change; a local time in the overlap takes the earlier instant. */
EMB_TEST(date_transitions)
{
    size_t mem0 = embi_mem_live();
    es6_env e = date_env_new();
    EMB_REQUIRE(e.r != nullptr);

    embi_js_date_test_pin_zone(-480, -420, 1772964000000.0); /* spring forward */
    E6_BOOL(e, "new Date(2026, 2, 8, 1, 30).getTime() === Date.UTC(2026, 2, 8, 9, 30)", true);
    /* 02:30 does not exist on 2026-03-08: the offset before the change gives 03:30 PDT. */
    E6_TEXT(e, "new Date(2026, 2, 8, 2, 30).toISOString()", "2026-03-08T10:30:00.000Z");
    E6_BOOL(e, "new Date(2026, 2, 8, 2, 30).getHours() === 3 &&"
               " new Date(2026, 2, 8, 2, 30).getTimezoneOffset() === 420", true);
    E6_TEXT(e, "new Date(Date.UTC(2026, 2, 8, 9, 59)).toString().slice(16, 21)", "01:59");
    E6_TEXT(e, "new Date(Date.UTC(2026, 2, 8, 10, 0)).toString().slice(16, 21)", "03:00");
    E6_BOOL(e, "new Date(2026, 6, 1).getTimezoneOffset() === 420", true);
    E6_BOOL(e, "new Date(2026, 0, 1).getTimezoneOffset() === 480", true);

    embi_js_date_test_pin_zone(-420, -480, 1793523600000.0); /* fall back */
    /* 01:30 on 2026-11-01 happens twice: the earlier instant is the PDT one. */
    E6_TEXT(e, "new Date(2026, 10, 1, 1, 30).toISOString()", "2026-11-01T08:30:00.000Z");
    E6_BOOL(e, "new Date(2026, 10, 1, 1, 30).getTimezoneOffset() === 420", true);
    E6_TEXT(e, "new Date(2026, 10, 1, 2, 30).toISOString()", "2026-11-01T10:30:00.000Z");
    E6_BOOL(e, "new Date(2026, 10, 1, 2, 30).getTimezoneOffset() === 480", true);
    E6_BOOL(e, "new Date(Date.UTC(2026, 10, 1, 8, 59)).getHours() === 1", true);

    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
