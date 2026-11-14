/*
 * Ember's testharnessreport.js (W5f). It replaces the WPT file of the same name: the runner
 * (tests/unit/bindings/test_wpt_th.c) serves this file for /resources/testharnessreport.js.
 *
 * Every subtest result and the harness status go to the host as one console line through the
 * public console callback (emb_view_set_console_callback). A line is the marker "\x01WPT" and
 * the kinds and fields, separated by \x1f:
 *   R <status> <name> <message>     one subtest (status: 0 PASS, 1 FAIL, 2 TIMEOUT, 3 NOTRUN, 4 PRECONDITION_FAILED)
 *   H <status> <message>            the harness status when the tests complete (0 OK, 1 ERROR, 2 TIMEOUT, 3 PRECONDITION_FAILED)
 *   D                               the completion; nothing follows it
 * Fields are cleaned of control characters, so a line is always one record.
 */
(function () {
    var SEP = "\u001f";
    var MARK = "\u0001WPT" + SEP;

    function clean(s) {
        s = String(s);
        var out = "";
        for (var i = 0; i < s.length; i++) {
            var c = s.charCodeAt(i);
            out += c < 32 ? " " : s.charAt(i);
        }
        return out.length > 400 ? out.slice(0, 400) : out;
    }

    function send(kind, fields) {
        var line = MARK + kind;
        for (var i = 0; i < fields.length; i++) {
            line += SEP + clean(fields[i]);
        }
        console.log("%s", line);
    }

    /* The results reach the host through the callbacks below, so the harness does not build its HTML results table (laying out
     * and painting thousands of rows costs more than the test). Every test gets six times the default 10 s harness timeout:
     * the CPU-heavy tests (thousands of subtests) need more than 10 s when the full run loads every core. */
    if (typeof setup === "function") {
        setup({ output: false, timeout_multiplier: 6 });
    }

    add_result_callback(function (test) {
        send("R", [test.status, test.name, test.message || ""]);
    });

    add_completion_callback(function (tests, harness_status) {
        send("H", [harness_status.status, harness_status.message || ""]);
        send("D", []);
    });
})();
