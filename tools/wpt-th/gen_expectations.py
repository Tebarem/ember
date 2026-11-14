#!/usr/bin/env python3
"""Writes tests/wpt-th/expectations/*.txt from the worker records of one full runner run (shard-*.out files).

Usage: gen_expectations.py OUT_DIR SHARD_DIR [SHARD_DIR ...]
  OUT_DIR    tests/wpt-th/expectations (overwritten: review the diff before committing)
  SHARD_DIR  the output directory of a `test_bindings_wpt_th --all` run (its shard-*.out files)

Each test that does not pass gets expectation lines (see tests/wpt-th/README.md): one line per failing subtest when the
test completed and the subtest names can be expressed (the first occurrence of a name decides, as in the runner), and one
whole-test line otherwise. The reason is `bug` unless a rule in RULES attributes the failure to another task or to a missing feature. Stdlib only.
"""
import glob, os, re, sys

OUT_DIR = sys.argv[1]
MANY = 25  # more failing subtests than this: one whole-test line
SHARDS = []
for d in sys.argv[2:]:
    SHARDS += glob.glob(os.path.join(d, "shard-*.out"))
    SHARDS += glob.glob(os.path.join(d, "inline.out"))  # a run of one test is not sharded

tests = {}  # id -> {"state", "hstatus", "subs": [(status, name, msg)]}
order = []
cur = None
for path in SHARDS:
    with open(path, "rb") as f:
        data = f.read().decode("utf-8", "replace")
    lines = data.split("\n")
    for ln in lines[:-1]:  # the last piece is a partial line when a worker was killed
        ln = ln.rstrip("\r")
        if ln.startswith("B\t"):
            cur = ln[2:]
            if cur not in tests:
                tests[cur] = {"state": None, "hstatus": 0, "subs": []}
                order.append(cur)
            else:
                tests[cur] = {"state": None, "hstatus": 0, "subs": []}
        elif cur is None:
            continue
        elif ln.startswith("S\t"):
            parts = ln[2:].split("\t")
            if len(parts) >= 2:
                try:
                    st = int(parts[0])
                except ValueError:
                    continue
                tests[cur]["subs"].append((st, parts[1], parts[2] if len(parts) > 2 else ""))
        elif ln.startswith("T\t"):
            parts = ln[2:].split("\t")
            tests[cur]["state"] = parts[0]
            try:
                tests[cur]["hstatus"] = int(parts[1]) if len(parts) > 1 else 0
            except ValueError:
                tests[cur]["hstatus"] = 0
            cur = None
# A test with no T record is a crash or a hang (the runner says so in its verdict).
for t in tests.values():
    if t["state"] is None:
        t["state"] = "crash"

# Reasons (tests/wpt-th/README.md). A failure is a `bug` (a defect of code that exists today, counted by the runner against the
# 3% cap of its directory) unless a rule below says that it belongs to another task or to a feature that does not exist yet.
# RULES: (test path regex, text regex over the test path, subtest name and message, reason); the first match wins.
RULES = [
    # W6e (web-animations, css/css-animations, css/css-transitions, resize-observer, intersection-observer). The causes are
    # written in tests/wpt-th/README.md ("W6e conformance"). out-of-scope: features that are not part of the engine's scope;
    # not-yet: a defect or a gap whose fix is a follow-up task, named in the note.
    (r"^(web-animations|css/css-(animations|transitions))/", r"animation-range|animation-timeline|ScrollTimeline|ViewTimeline|scroll-timeline|view-timeline|timeline-scope", "out-of-scope:scroll-driven animations (only DocumentTimeline exists)"),
    (r"^intersection-observer/v2/", r".", "out-of-scope:IntersectionObserver v2 (trackVisibility, occlusion and filter visibility are not implemented)"),
    (r"^(web-animations|css/css-(animations|transitions)|intersection-observer|resize-observer)/", r"CSSNumericValue|CSSUnitValue|CSSMathSum|CSS\.(em|px|percent|number)|attributeStyleMap|computedStyleMap|CSSStyleValue|CSSKeywordValue", "not-yet:CSS Typed OM objects"),
    (r"^(intersection|resize)-observer/", r"svg", "not-yet:SVG element boxes in the observers (no bounding box of SVG graphics elements)"),
    (r"^intersection-observer/zoom-", r".", "out-of-scope:CSS zoom (the property is not implemented)"),
    (r"^intersection-observer/root-vertical-rl", r".", "not-yet:IntersectionObserver geometry of a root in a vertical writing mode"),
    (r"^intersection-observer/(clip-path|scroll-margin-clip-path)", r".", "not-yet:IntersectionObserver clipping by clip-path (the clip is not part of the observer geometry)"),
    (r"^intersection-observer/svg-", r".", "not-yet:IntersectionObserver SVG viewport clipping and units of SVG boxes"),
    (r"^intersection-observer/", r".", "not-yet:IntersectionObserver corner cases (root margin rounding, root and scroll margin together, lazy images, bounding box of fragmented boxes)"),
    (r"^resize-observer/", r".", "not-yet:ResizeObserver boxes of vertical writing modes and fragmented (multicol) boxes"),
    (r"^web-animations/interfaces/KeyframeEffect/target", r".", "not-yet:animation targets of pseudo-elements (::before, ::after) are not implemented"),
    (r"^web-animations/responsive/", r".", "not-yet:animated computed value of a property not serialized or not implemented (calc(), matrix(), translate, rotate, clamping to 0px, motion path, shape-outside, SVG presentation properties)"),
    (r"^web-animations/timing-model/", r".", "not-yet:timing model corner cases (reverse() from outside the active interval, null timeline, pending tasks and rates, step easing limits, finished promise)"),
    (r"^web-animations/animation-model/", r".", "not-yet:animation model gaps (accumulate composite, background shorthands, step easing left limits, interpolated transform serialization)"),
    (r"^web-animations/interfaces/", r".", "not-yet:interface details (overallProgress, property-indexed keyframes details, event timestamps, easing escapes)"),
    (r"^css/css-animations/parsing/", r".", "not-yet:CSSOM parsing details (keyframe selectors through insertRule, canonical shorthand serialization, quoted animation names)"),
    (r"^css/css-transitions/parsing/", r".", "not-yet:CSSOM parsing details (longhands read back from a shorthand, canonical shorthand serialization, validity of transition-property lists)"),
    (r"^css/css-transitions/animations/", r".", "not-yet:interpolation details (text-shadow, vertical-align and z-index tests: serialization and discrete behavior)"),
    (r"^css/css-(animations|transitions)/", r".", "not-yet:style and event details of CSS animations and transitions (pseudo-elements, events of removed elements, shorthand values, color-mix and relative colors, CSS-OM keyframes rules)"),
    # W7e and W7f (encoding/, fetch/api/, webstorage/, html/webappapis/structured-clone/): the causes are written in
    # tests/wpt-th/README.md (W7f, "Scope of the W7 pass rates"). A reason is `not-applicable` (by design: the cause is the
    # note), `not-yet:TASK` (a later task takes the feature over: written as `not-yet TASK ...`), `needs-http` or `bug`.
    # tools/wpt-th/score.py leaves not-applicable, needs-http and the not-yet W5e/R4 entries out of the denominator.
    # CJK decoders are out of scope (W7 spec: the tables are hundreds of KB) and their labels fail in mixed tests.
    (r"^encoding/", r"gbk|gb18030|big5|shift_jis|shift-jis|sjis|euc-jp|euc-kr|iso-2022|ISO-2022|csiso2022|csiso58|"
                    r"hz-gb|iso-ir-|csksc|ksc|korean|windows-949|windows-31j|ms932|ms_kanji|x-sjis|x-gbk|x-euc|x-x-big5|"
                    r"gb_2312|gb2312|csshiftjis|csiso2022jp|csshift|Big5|GBK|EUC-|Shift_JIS", "not-applicable:CJK decoders (size budget)"),
    # WPT's common/sab.js makes a SharedArrayBuffer through new WebAssembly.Memory({shared: true}); WebAssembly is outside the project's scope
    (r"^(encoding|fetch/api|html/webappapis/structured-clone)/", r"SharedArrayBuffer", "not-applicable:SharedArrayBuffer needs WebAssembly (common/sab.js)"),
    # a document without a charset is decoded as UTF-8 (src/embed/view.c), not as windows-1252 as the HTML sniffing says
    (r"^encoding/sniffing", r".", "not-applicable:the UTF-8 default is the user's decision"),
    # decode-utf8 builds a SharedArrayBuffer at the top level of the script for its second pass: the harness reports an error
    (r"^encoding/streams/decode-utf8", r".", "not-applicable:SharedArrayBuffer needs WebAssembly (common/sab.js)"),
    # the two consume-empty tests expect "" for an empty FormData body; response-form-data.html "Empty form data" expects the
    # closing delimiter for the same input, which is what the multipart/form-data encoding algorithm produces (the test's own
    # FIXME doubts the empty string)
    (r"consume-empty", r"FormData", "not-applicable:contradicts response-form-data.html 'Empty form data' and the Fetch standard"),
    (r"^fetch/api/basic/header-value-combining", r".", "needs-http"),
    # structured-clone: FileList exists since W5e (<input type=file>); cloning one is W7f's (src/loader/clone.c); the canvas objects come with Canvas 2D (R4)
    (r"^html/webappapis/structured-clone/", r"FileList", "not-yet:W7f structured clone of a FileList (src/loader/clone.c); the type exists since W5e"),
    (r"^html/webappapis/structured-clone/", r"OffscreenCanvas|ImageData|ImageBitmap|canvas|createImageBitmap", "not-yet:R4"),
    # the test needs a response header (a CSP header that hides the nonce content attribute)
    (r"moveBefore/nonce", r".", "needs-http"),
    # features that do not exist: image loading and load events, observers on the nodes of documents made by script,
    # lone surrogates in strings (DOM strings are UTF-8), a [[Set]] that reaches a legacy platform object through a prototype
    # (the engine, src/js/object.c, has no "ignore named properties" mode)
    (r"moveBefore/relevant-mutations|MutationObserver-textContent|HTMLCollection-as-prototype", r".", "not-yet"),
    (r"ParentNode-querySelector-escapes", r"surrogate", "not-yet"),
    # dynamic <script>/<style>/<link> elements: the insertion steps run them (W7d, src/embed/script.c and src/loader)
    (r"insertion-removing-steps/|MutationObserver-document|moveBefore/mutation-observer", r".", "scripts"),
    # the event loop: a timer set from a microtask of a timer task inherits its nesting level (src/embed/loop.c, W7d)
    (r"timer-nesting-not-inherited-in-microtask", r".", "scripts"),
    # controls, form association, validation and activation behavior of controls (W5e)
    (r".", r":invalid|:valid|form association|form owner|\.form\b|validity|checkbox|radio|activation behavior|"
           r"<input|INPUT|\bselect\b|<select|optgroup|\boption\b|selectedness|namespaced ID", "forms"),
    (r"Event-dispatch-click|Event-dispatch-single-activation|preventDefault-during-activation|dispatchEvent\.click",
     r".", "forms"),
    # rendering, style and layout state (W5d2)
    (r"moveBefore/", r".", "layout"),
    (r".", r"display: none|getComputedStyle|offsetX|child-style|computed", "layout"),
    # features Ember does not have yet
    (r".", r"FileReader|Performance|BeforeUnloadEvent|DeviceMotionEvent|DeviceOrientationEvent|MessageEvent|StorageEvent|"
           r"SVGEvents|PointerEvent|execCommand|createRange|Range is not|XPath", "not-yet"),
    (r"getElementsByClassName-(20|21|22|25)|html/dom/", r".", "not-yet"),
    (r"html/semantics/", r".", "forms"),
]
DEFAULT_REASON = "bug"


def reason_for(tid, text):
    for path_re, text_re, reason in RULES:
        if re.search(path_re, tid) and re.search(text_re, tid + " " + text):
            return reason
    return DEFAULT_REASON


def fmt(reason, msg):
    """'reason' or 'reason:prefix' (a rule's way to put the task or the cause at the head of the note) plus the message."""
    if ":" in reason:
        reason, prefix = reason.split(":", 1)
        return (reason + " " + prefix + " " + msg).rstrip()
    return (reason + " " + msg).rstrip()


def note_for(tid, state, hstatus, first_msg):
    if state != "done":
        return "did not complete (%s)" % state
    m = first_msg.strip()
    m = m[:90]
    return m


def clean(s):
    return re.sub(r"\s+", " ", s).strip()


entries = {}  # file key -> list of lines
DIR_KEYS = [
    ("dom/nodes/", "dom-nodes"), ("dom/events/", "dom-events"), ("dom/collections/", "dom-collections"),
    ("dom/lists/", "dom-lists"), ("dom/traversal/", "dom-traversal"), ("dom/abort/", "dom-abort"),
    ("html/dom/", "html-dom"), ("html/semantics/", "html-semantics"), ("html/webappapis/", "html-webappapis"),
    ("uievents/", "uievents"), ("console/", "console"),
    ("encoding/", "encoding"), ("fetch/api/", "fetch"), ("webstorage/", "webstorage"),
    ("css/css-animations/", "css-animations"), ("css/css-transitions/", "css-transitions"), ("web-animations/", "web-animations"),
    ("resize-observer/", "resize-observer"), ("intersection-observer/", "intersection-observer"),
]


def key_of(tid):
    for prefix, k in DIR_KEYS:
        if tid.startswith(prefix):
            return k
    return "other"


counts = {"tests_listed": 0, "whole": 0, "subtest": 0, "passing": 0}
for tid in order:
    t = tests[tid]
    counts["tests_listed"] += 1
    completed = t["state"] == "done" and t["hstatus"] == 0 and len(t["subs"]) > 0
    if completed and all(s[0] == 0 for s in t["subs"]):
        counts["passing"] += 1
        continue
    key = key_of(tid)
    lines = entries.setdefault(key, [])
    if not completed:
        r = reason_for(tid, tid)
        if t["state"] == "done":
            note = "harness status %d" % t["hstatus"]
        elif t["state"] == "done" or not t["subs"]:
            note = "did not complete (%s)" % t["state"]
        else:
            note = "did not complete (%s) after %d subtests" % (t["state"], len(t["subs"]))
        lines.append("%s %s" % (tid, fmt(r, note)))
        counts["whole"] += 1
        continue
    # the first occurrence of each name decides what the runner checks
    first = {}
    for st, name, msg in t["subs"]:
        if name not in first:
            first[name] = (st, msg)
    failing = [n for n, (st, msg) in first.items() if st != 0]
    expressible = True
    for n in failing:
        if n != n.strip() or " :: " in n or n == "" or len(n) > 480:
            expressible = False
    # a name whose first occurrence passes is not expressible when a later occurrence fails (the runner checks the first)
    if any(st != 0 and first[nm][0] == 0 for st, nm, _ in t["subs"]):
        expressible = False
    if not expressible or len(failing) > MANY:
        # whole test: not expressible per subtest, or so many failing subtests that one line per subtest hides the file
        msg = next((m for st, nm, m in t["subs"] if st != 0), "")
        r = reason_for(tid, " ".join(failing) + " " + msg)
        note = "%d of %d subtests fail: %s" % (len(failing), len(first), msg)
        lines.append("%s %s" % (tid, fmt(r, clean(note)[:150])))
        counts["whole"] += 1
        continue
    for n in failing:
        st, msg = first[n]
        r = reason_for(tid, n + " " + msg)
        lines.append("%s :: %s :: %s" % (tid, n, fmt(r, clean(msg)[:90])))
        counts["subtest"] += 1

os.makedirs(OUT_DIR, exist_ok=True)
for k, lines in sorted(entries.items()):
    if k == "other":
        continue
    with open(os.path.join(OUT_DIR, k + ".txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Expected failures of the testharness.js run in %s (generated; see tests/wpt-th/README.md).\n" % k)
        for ln in lines:
            f.write(ln + "\n")
print(counts)
for k, lines in sorted(entries.items()):
    print(k, len(lines))
