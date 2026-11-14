/* Date.parse (J4d): the ISO format (date-only forms are UTC, date-time
 * forms without an offset are local), the legacy forms that browsers accept (toString, toUTCString
 * and the common spoken and numeric forms), rejection of malformed text, and round trips through
 * every string format. The offset is pinned, so the results do not depend on the machine's time
 * zone. Each test checks that the runtime's live memory returns to its baseline. */
#include "es6_util.h"

void embi_js_date_test_pin_offset(int32_t minutes);
void embi_js_date_test_unpin(void);

EMB_TEST(date_parse_iso)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Date-only forms are UTC, whatever the local offset. */
    E6_BOOL(e, "Date.parse('1970-01-01') === 0 && Date.parse('1970-01') === 0", true);
    E6_BOOL(e, "Date.parse('2000') === 946684800000", true);
    E6_BOOL(e, "Date.parse('2026-11-09') === 1794182400000", true);
    /* Date-time forms with a Z or an offset. */
    E6_BOOL(e, "Date.parse('1970-01-01T00:00:00Z') === 0", true);
    E6_BOOL(e, "Date.parse('1970-01-01T00:00:00.000+01:00') === -3600000", true);
    E6_BOOL(e, "Date.parse('1970-01-01T00:00:00-02:30') === 9000000", true);
    E6_BOOL(e, "Date.parse('2026-11-09T10:00Z') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('2026-11-09T10:00:00.500Z') === 1794218400500", true);
    /* Without an offset, a date-time is local time (+05:30 here). */
    E6_BOOL(e, "Date.parse('1970-01-01T00:00:00') === -19800000", true);
    E6_BOOL(e, "Date.parse('2026-11-09T10:00') === 1794198600000", true);
    /* Extended years. */
    E6_BOOL(e, "Date.parse('+002000-01-01T00:00:00Z') === 946684800000", true);
    E6_BOOL(e, "Date.parse('-000001-01-01T00:00:00Z') === -62198755200000", true);
    E6_BOOL(e, "Date.parse('+275760-09-13T00:00:00.000Z') === 8.64e15", true);
    E6_BOOL(e, "isNaN(Date.parse('+275760-09-13T00:00:00.001Z'))", true);
    E6_BOOL(e, "isNaN(Date.parse('-000000-01-01T00:00:00Z'))", true);
    E6_BOOL(e, "isNaN(Date.parse('-271821-04-19T23:59:59.999Z'))", true);
    /* The end of a day can be written as 24:00. */
    E6_BOOL(e, "Date.parse('2026-11-09T24:00:00Z') === Date.parse('2026-11-10T00:00:00Z')", true);
    E6_BOOL(e, "isNaN(Date.parse('2026-11-09T24:01Z'))", true);
    /* Out-of-range fields and malformed text. */
    E6_BOOL(e, "isNaN(Date.parse('2026-13-01')) && isNaN(Date.parse('2026-00-01'))", true);
    E6_BOOL(e, "isNaN(Date.parse('2026-11-09T25:00Z')) && isNaN(Date.parse('2026-11-09T10:60Z'))",
            true);
    E6_BOOL(e, "isNaN(Date.parse('2026-11-09T10:00:00+24:00'))", true);
    E6_BOOL(e, "isNaN(Date.parse('26-11-09'))", true);
    E6_BOOL(e, "isNaN(Date.parse('')) && isNaN(Date.parse('garbage')) && isNaN(Date.parse('x'))",
            true);
    E6_BOOL(e, "isNaN(Date.parse('2026-11-09T10:00:00.5éZ'))", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(date_parse_legacy)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* The spoken and the US numeric forms, with and without a time, GMT and AM/PM. */
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 GMT') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('Nov 9, 2026 10:00:00 UTC') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('9 November 2026 10:00 Z') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('Mon, 09 Nov 2026 10:00:00 GMT') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('2026/11/09 10:00 GMT+0000') === 1794218400000", true);
    E6_BOOL(e, "Date.parse('11/09/2026 10:00 PM GMT') === 1794261600000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 12:00 AM GMT') === 1794182400000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 12:30 PM GMT') === 1794182400000 + 45000000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 GMT-0500') === 1794218400000 + 18000000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 GMT+05:30') === 1794218400000 - 19800000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 EST') === 1794218400000 + 18000000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 PDT') === 1794218400000 + 25200000", true);
    /* Without an offset the time is local (+05:30 here). */
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00') === 1794198600000", true);
    E6_BOOL(e, "Date.parse('Nov 9 2026') === 1794182400000 - 19800000", true);
    /* Comments in parentheses are skipped. */
    E6_BOOL(e, "Date.parse('Nov 9 2026 10:00 GMT (anything at all)') === 1794218400000", true);
    /* Two-digit years: below 50 is 20xx, from 50 is 19xx. */
    E6_BOOL(e, "Date.parse('Jan 1 49 GMT') === Date.UTC(2049, 0, 1)", true);
    E6_BOOL(e, "Date.parse('Jan 1 50 GMT') === Date.UTC(1950, 0, 1)", true);
    /* Unknown words, bad months and bad times are NaN. */
    E6_BOOL(e, "isNaN(Date.parse('Foo 1 2026'))", true);
    E6_BOOL(e, "isNaN(Date.parse('13/01/2026'))", true);
    E6_BOOL(e, "isNaN(Date.parse('Nov 9 2026 25:00 GMT'))", true);
    E6_BOOL(e, "isNaN(Date.parse('Nov 9 2026 13:00 PM GMT'))", true);
    E6_BOOL(e, "isNaN(Date.parse('Nov 2026 GMT')) || Date.parse('Nov 2026 GMT') === Date.UTC(2026, 10, 1)",
            true);
    E6_BOOL(e, "isNaN(Date.parse('11/09 GMT'))", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Every string format reads back the time value it was made from (to the second, or the
 * millisecond for ISO). The values cover negative and large times. */
EMB_TEST(date_parse_round_trip)
{
    size_t mem0 = embi_mem_live();
    embi_js_date_test_pin_offset(330);
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var bad = 0; for (var i = -1500; i < 1500; i++) {"
               "  var t = i * 1234567000 + 987; var s = Math.floor(t / 1000) * 1000;"
               "  if (Date.parse(new Date(t).toISOString()) !== t) bad++;"
               "  if (Date.parse(new Date(s).toString()) !== s) bad++;"
               "  if (Date.parse(new Date(s).toUTCString()) !== s) bad++;"
               "  if (Date.parse(new Date(s).toDateString() + ' 00:00:00 GMT') !== Date.UTC("
               "    new Date(s).getFullYear(), new Date(s).getMonth(), new Date(s).getDate())) bad++;"
               "} bad === 0", true);
    /* Times far from 1970, where the local offset is taken from another year of the cycle. */
    E6_BOOL(e, "var t2 = [8.64e15, -8.64e15, 8.64e15 - 86400000, -8.64e15 + 86400000, -62198755200000];"
               "var bad2 = 0; for (var j = 0; j < t2.length; j++) {"
               "  if (Date.parse(new Date(t2[j]).toISOString()) !== t2[j]) bad2++;"
               "  if (Date.parse(new Date(t2[j]).toString()) !== Math.floor(t2[j] / 1000) * 1000) bad2++;"
               "} bad2 === 0", true);
    E6_BOOL(e, "Date.parse(new Date(0).toString()) === 0 && Date.parse(new Date(0).toUTCString()) === 0",
            true);
    E6_BOOL(e, "Date.parse(new Date(1234567890123).toString()) === 1234567890000", true);
    E6_BOOL(e, "Date.parse(new Date(-62198755200000).toString()) === -62198755200000", true);
    E6_BOOL(e, "Date.parse(new Date(-62198755200000).toUTCString()) === -62198755200000", true);
    es6_env_free(e);
    embi_js_date_test_unpin();
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
