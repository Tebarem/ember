#!/usr/bin/env python3
"""Select the WPT testharness.js tests W5f runs and every local file they need.

Usage: select_testharness.py WPT_ROOT OUT_LIST OUT_FILES [--why]
  OUT_LIST   one test path per line (relative to WPT_ROOT), sorted
  OUT_FILES  every file to vendor (tests and their local resources), sorted. /resources/testharnessreport.js is not
             listed: tests/wpt-th/testharnessreport.js replaces it.
  --why      print how many candidate tests each exclusion rule removed
Rules: the file is .html/.htm and loads /resources/testharness.js, or is a .any.js whose META global includes window (it
runs as the foo.any.html document WPT serves for it, with its META scripts); it is not a manual test and not a generated
or server-side file (.sub., .https., .h2., .worker., .window., .serviceworker., .www.); its content (comments and
rel=help/author/license/reviewer/details links removed) uses no iframe, object, embed, worker, service worker, shadow
DOM, custom element, window.open, testdriver, server handler (.py), templating ({{...}}), network request, or fetched
absolute http(s) URL (a src, href, action, poster, data or url() value; namespace URIs and other literals are allowed), and,
for reflection-*.html, no element Ember does not implement (video, audio, canvas, picture, svg, math). Every local
resource it references (script, stylesheet, image, url(), @import) must exist under WPT_ROOT. It also uses nothing Ember
does not have and the spec leaves out: Range, Selection, XPath, any iframe (also made from script), postMessage and
cross-document navigation; under html/dom only the reflection-*.html files are taken; the dom/events/scrolling and
dom/nodes/moveBefore tests that need rendering, scrolling, animation, focus movement or style invalidation are left out,
as are the dom/events tests of the legacy webkit-prefixed animation/transition events, the .tentative tests, the declarative-partial-updates proposal and the webkit-prefixed animation/transition event
tests. The local scripts a test loads are scanned with the same rules (the structured-clone transferables helper is kept,
see HELPER_ALLOWED). The W7 directories (encoding, fetch/api, html/webappapis/structured-clone, webstorage) also take
foo.window.js tests, listed as foo.window.html, and fetch/api vendors the static files of fetch/api/resources; the CJK-only
encoding files are left out. Stdlib only.
"""
import os, re, sys

ROOT, OUT_LIST, OUT_FILES = sys.argv[1], sys.argv[2], sys.argv[3]
SHOW_WHY = "--why" in sys.argv[4:]
DIRS = ["dom/nodes", "dom/events", "dom/collections", "dom/lists", "dom/traversal", "dom/abort", "uievents",
        "html/dom", "html/semantics/forms", "html/webappapis/timers", "html/webappapis/microtask-queuing", "console",
        "encoding", "fetch/api", "html/webappapis/structured-clone", "webstorage", "svg/types", "svg/geometry", "html/canvas/element",
        "web-animations", "css/css-animations", "css/css-transitions", "resize-observer", "intersection-observer"]
# The directories that take foo.window.js tests (the W7 directories; the W5 directories keep their selection).
NEW_DIRS = ("encoding", "fetch/api", "html/webappapis/structured-clone", "webstorage")
RENDER_LATER = {
    "html/semantics/forms/the-fieldset-element/disabled-003.html",
    "html/semantics/forms/the-select-element/select-insertion-inherited-style.html",
    "html/dom/reflection-sections.html",
}
# File names of the encoding tests that only test CJK decoders (W7 spec: out of scope).
CJK_ONLY = re.compile(r"legacy-mb-|big5|iso-2022-jp|eof-shift_jis", re.I)
# W7f: encoding/streams/ is selected again (TextEncoderStream and TextDecoderStream exist); only a MediaSource blob: URL and the
# bad-port test (a proxy to a server) stay out, both not applicable by design.
OUT_OF_SCOPE_TESTS = re.compile(r"^fetch/api/basic/mediasource|^fetch/api/request/request-bad-port")
# W7f: tests that run a loop of tens of thousands of assert_equals calls with template strings (several seconds under Debug,
# over the 10 s testharness timeout under ASan): the result would differ between build types, so they stay out of the selection.
TOO_SLOW_TESTS = ("encoding/encodeInto-tail-unmodified.any.js",)  # the source file of foo.any.html
# Helpers scanned by the same rules as tests, but kept on purpose: the structured-clone battery with transferables uses
# MessageChannel/postMessage in some of its subtests only; those subtests are expectations (not-yet), the rest is the subject.
HELPER_ALLOWED = ("/html/webappapis/structured-clone/structured-clone-battery-of-tests-with-transferables.js",
                  "/html/canvas/resources/canvas-tests.js",
                  "/web-animations/testcommon.js")  # W6e: its iframe helper is only called by tests the selector leaves out  # its cross-origin helpers are only used by tests CANVAS_BAD leaves out
# R4b: canvas tests that need a second origin, a server handler, a worker, video or an OffscreenCanvas (out of scope).
CANVAS_BAD = re.compile(r"get_host_info|forEachCanvasSource|addCrossOrigin|OffscreenCanvas|<video|<audio|HTMLVideoElement|"
                        r"getVideoURI|WebGL|webgl|requestVideoFrame|captureStream|drawElement|ImageCapture|VideoFrame|"
                        r"navigator\.|\.pdf", re.I)
# W6e: the animation, transition and observer directories. Out of scope: scroll-driven animations,
# View Transitions, @starting-style, animation of d/SVG properties, 3D transforms and registered custom properties' syntax
# list are kept (they are expectations); what the engine cannot host at all (other browsing contexts, workers) is already
# excluded by CONTENT_BAD and UNSUPPORTED.
W6_PREFIXES = ("web-animations/", "css/css-animations/", "css/css-transitions/", "resize-observer/", "intersection-observer/")
W6_OUT = re.compile(r"ScrollTimeline|ViewTimeline|scroll-timeline|view-timeline|animation-timeline|animation-range|"
                    r"view-transition|ViewTransition|@starting-style|starting-style|scroll-driven|"
                    r"path\(|d\s*:|perspective|preserve-3d|rotate3d|translate3d|matrix3d|SVGAnimat|<animate|<set|"
                    r"CSS\.registerProperty|@property|@scope|anchor-name|position-anchor|ElementInternals|"
                    r"requestFullscreen|popover|showModal|PointerEvent|Touch|print|@page|text-box|"
                    r"scrollTop\s*=|scrollTo\(|scrollIntoView|\.scroll\(|overflow\s*:\s*(?:scroll|auto)|"
                    r"setPointerCapture|focus\(|navigator\.|window\.top|window\.parent|opener|devicePixelRatio|"
                    r"internals\.|getWindowSelection", re.I)
PROVIDED = {"/resources/testharnessreport.js"}
HARNESS = "/resources/testharness.js"
NAME_BAD = re.compile(r"\.(sub|https|h2|worker|window|serviceworker|www)\.|manual|tentative|webkit-(?:animation|transition)", re.I)
META = re.compile(r"^//\s*META:\s*(\w+)\s*=\s*(.*?)\s*$", re.M)
CONTENT_BAD = re.compile(
    r"<iframe|<frame\b|<object|<embed|<portal|Worker\b|importScripts|serviceWorker|ServiceWorker|"
    r"attachShadow|shadowRoot|shadowroot|ShadowRoot|customElements|CustomElement|"
    r"window\.open|testdriver|\.py\b|\{\{|"
    r"(?:\bsrc|\bhref|\baction|\bposter|\bdata)\s*=\s*[\"']?\s*https?:|url\(\s*[\"']?\s*https?:|"
    r"@import\s+[\"']?https?:|(?:fetch|importScripts)\s*\(\s*[\"']https?:|new\s+Request\s*\(\s*[\"']https?:|"
    r"new\s+(?:EventSource|WebSocket)\b|XMLHttpRequest|sendBeacon|RTCPeerConnection", re.I)
# Features Ember does not have and the spec leaves out: Range, Selection, XPath, and iframes of any kind (also created from script).
UNSUPPORTED = re.compile(
    r"\bcreateRange\b|\bnew\s+Range\b|\bStaticRange\b|\bgetSelection\b|\bXPath\w*|document\.evaluate|\bcreateExpression\b|"
    r"\bcreateNSResolver\b|iframe|<frameset|<frame\b|\bFrameSet\b|contentWindow|contentDocument|window\.frames|"
    r"\bpostMessage\b|\bBroadcastChannel\b|\bMessageChannel\b|\bnavigation\.|\blocation\.(?:href|assign|replace)\s*=", re.I)
# W7f: the W7 directories have MessageChannel, MessagePort and window.postMessage in one document (no other browsing context),
# so they are not in this list there.
UNSUPPORTED_W7 = re.compile(
    r"\bcreateRange\b|\bnew\s+Range\b|\bStaticRange\b|\bgetSelection\b|\bXPath\w*|document\.evaluate|\bcreateExpression\b|"
    r"\bcreateNSResolver\b|iframe|<frameset|<frame\b|\bFrameSet\b|contentWindow|contentDocument|window\.frames|"
    r"\bBroadcastChannel\b|\bnavigation\.|\blocation\.(?:href|assign|replace)\s*=", re.I)
W7_PREFIXES = ("encoding/", "fetch/api/", "html/webappapis/structured-clone/", "webstorage/")
# Tests that need rendering, scrolling, animations, focus movement or style invalidation across a move (dom/events and moveBefore).
RENDER_DEP = re.compile(
    r"@keyframes|\banimation(?:-name|\s*:)|\btransition(?:-property|-duration|\s*:)|getAnimations|scrollend|\bscrollTo\b|"
    r"scrollIntoView|ScrollTimeline|\binert\b|\bpopover\b|showModal|\.focus\(|:focus|rel\s*=\s*[\"']?stylesheet|"
    r"\blang\b|\bdir\b|container-type|getComputedStyle|requestAnimationFrame|overflow\s*:\s*(?:scroll|auto)", re.I)
RENDER_DIRS = ("dom/events/scrolling/", "dom/nodes/moveBefore/")
REFLECTION_BAD = re.compile(r"<(?:video|audio|canvas|picture|svg|math)\b|['\"](?:video|audio|canvas|picture|svg|math)['\"]", re.I)
COMMENT = re.compile(r"<!--.*?-->", re.S)
JS_COMMENT = re.compile(r"/\*.*?\*/|^\s*//[^\n]*", re.S | re.M)  # block and line comments of a script
HELP_LINK = re.compile(r"<link\b[^>]*\brel\s*=\s*[\"']?(?:help|author|license|reviewer|details)\b[^>]*>", re.I)
ANCHOR = re.compile(r"<a\b[^>]*>", re.I)
SCRIPT_SRC = re.compile(r"<script\b[^>]*?\bsrc\s*=\s*[\"']?([^\"'\s>]+)", re.I)
LINK_HREF = re.compile(r"<link\b[^>]*\bhref\s*=\s*[\"']?([^\"'\s>]+)", re.I)
IMG_SRC = re.compile(r"<(?:img|source|track)\b[^>]*?\bsrc\s*=\s*[\"']?([^\"'\s>]+)", re.I)
URL_FN = re.compile(r"url\(\s*[\"']?([^\"')\s]+)", re.I)
IMPORT = re.compile(r"@import\s+(?:url\(\s*)?[\"']([^\"']+)[\"']", re.I)
cache = {}
why = {}


def read(p):
    if p not in cache:
        try:
            with open(p, "rb") as f:
                cache[p] = f.read().decode("utf-8", "replace")
        except OSError:
            cache[p] = None
    return cache[p]


def resolve(base, ref):
    """Local path of ref (relative to the file base, or root-relative); None when ref is not a local URL."""
    ref = ref.split("#")[0].split("?")[0].strip()
    if not ref or ref.startswith("data:") or re.match(r"^[a-z][a-z0-9+.-]*:", ref, re.I) or ref.startswith("//"):
        return None
    if ref.startswith("/"):
        p = os.path.normpath(os.path.join(ROOT, ref.lstrip("/")))
    else:
        p = os.path.normpath(os.path.join(os.path.dirname(base), ref))
    return p if p.startswith(os.path.abspath(ROOT)) else ""


def collect(p, text, out, seen, depth=0):
    """Adds the local resources of a document or stylesheet to out. False when one is missing or unusable."""
    if depth > 6:
        return False
    refs = [m.group(1) for m in SCRIPT_SRC.finditer(text)]
    refs += [m.group(1) for m in LINK_HREF.finditer(text)]
    refs += [m.group(1) for m in IMG_SRC.finditer(text)]
    is_css = p.lower().endswith(".css")
    if is_css:
        refs += URL_FN.findall(text) + IMPORT.findall(text)
    for r in refs:
        if r.split("?")[0] in PROVIDED:
            continue
        q = resolve(p, r)
        if q is None:
            continue
        if q.replace(os.sep, "/").endswith("/html/canvas/images/background.png"):
            continue  # canvas-tests.css names a decoration image that WPT itself does not have
        if q == "" or not os.path.isfile(q):
            return False
        if q in seen:
            continue
        seen.add(q)
        out.add(q)
        if q.lower().endswith(".css"):
            css = read(q)
            if css is None or not collect(q, css, out, seen, depth + 1):
                return False
    return True


def any_wrapper(text):
    """The document WPT serves for foo.any.js as foo.any.html: the harness, our report, and the META scripts in order.
    None when the test is not for the window global (a worker-only test)."""
    globals_ = [v for k, v in META.findall(text) if k == "global"]
    if globals_ and not any("window" in g.split(",") for g in globals_):
        return None
    scripts = [v for k, v in META.findall(text) if k == "script"]
    html = '<!doctype html><script src="/resources/testharness.js"></script>'
    html += '<script src="/resources/testharnessreport.js"></script>'
    for s in scripts:
        html += '<script src="%s"></script>' % s
    return html


def exclusion(p, text):
    """None when the test may run, else the name of the rule that removes it."""
    body = ANCHOR.sub("", HELP_LINK.sub("", COMMENT.sub("", META.sub("", text))))
    if p.endswith((".any.js", ".window.js")):
        if any_wrapper(text) is None:
            return "global"
    elif HARNESS not in body:
        return "not-testharness"
    m = CONTENT_BAD.search(body)
    if m:
        return "content"
    rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
    # The CJK legacy decoders and ISO-2022-JP are out of scope (W7 spec): tests that are only about them are left out.
    if rel.startswith("encoding/") and CJK_ONLY.search(rel):
        return "out-of-scope"
    # Whole tests of features the W7 spec leaves out: TextDecoderStream/TextEncoderStream (TransformStream, pipes), a
    # MediaSource blob: URL, and the bad-port test that needs a proxy to a server (no network).
    if OUT_OF_SCOPE_TESTS.search(rel):
        return "out-of-scope"
    if rel in TOO_SLOW_TESTS:
        return "too-slow"
    # Tests that became vendorable with /common (W7e) and need a rendering pass (requestAnimationFrame, computed custom
    # properties) or a reflection of every element; they stay out of the W5 selection.
    if rel in RENDER_LATER:
        return "render-dependent"
    if "declarative-partial-updates" in text:
        return "proposal"
    if (UNSUPPORTED_W7 if rel.startswith(W7_PREFIXES) else UNSUPPORTED).search(body):
        return "unsupported"
    if rel.startswith("dom/events/") and re.search(r"webkit(?:Animation|Transition)", body):
        return "render-dependent"  # the legacy prefixed events need a running CSS animation or transition
    if rel.startswith(RENDER_DIRS) or (rel.startswith("dom/events/") and re.search(r"scroll|(?:transition|animation)-?(?:end|start|iteration)", rel)):
        if rel.startswith("dom/events/scrolling/") or RENDER_DEP.search(body):
            return "render-dependent"
    if rel.startswith("html/canvas/") and CANVAS_BAD.search(body):
        return "canvas-out-of-scope"
    if rel.startswith(W6_PREFIXES) and W6_OUT.search(body):
        return "w6-out-of-scope"
    if rel.startswith("html/dom/") and not os.path.basename(p).startswith("reflection-"):
        return "not-reflection"
    if os.path.basename(p).startswith("reflection-") and REFLECTION_BAD.search(body):
        return "reflection-element"
    return None


tests, files = [], set()
for d in DIRS:
    for dp, dn, fn in os.walk(os.path.join(ROOT, d)):
        dn[:] = [x for x in dn if x not in ("resources", "support", "tools", "manual", "offscreen")]
        for f in sorted(fn):
            if not f.lower().endswith((".html", ".htm", ".any.js")) and not (f.endswith(".window.js") and d.startswith(NEW_DIRS)):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
            # foo.window.js is served as foo.window.html (tools/wpt/gen_window_wrappers.py writes that document).
            base = f[: -len(".window.js")] if f.endswith(".window.js") else f
            if NAME_BAD.search(base):
                why["name"] = why.get("name", 0) + 1
                continue
            text = read(p)
            if text is None:
                continue
            r = exclusion(p, text)
            if r is not None:
                why[r] = why.get(r, 0) + 1
                continue
            # A .any.js test runs as the document WPT serves for it (foo.any.html), with its META scripts.
            scan = text
            if p.endswith(".any.js"):
                scan = any_wrapper(text)
                rel = rel[: -len(".any.js")] + ".any.html"
            elif p.endswith(".window.js"):
                scan = any_wrapper(text)
                rel = rel[: -len(".window.js")] + ".window.html"
            need = set()
            if not collect(p, scan, need, set()):
                why["missing-resource"] = why.get("missing-resource", 0) + 1
                continue
            # The scripts a test loads are part of it: a helper that makes an iframe or a Range counts too.
            helper_bad = False
            for q in sorted(need):
                q = q.replace(os.sep, "/")
                if q.lower().endswith(".js") and not q.endswith("/testharness.js"):
                    js = read(q)
                    if q.endswith(HELPER_ALLOWED):
                        continue
                    # Comments are ignored in the W7 directories only; the W5 selection keeps its scan (its helpers are unchanged).
                    if js is not None and d.startswith(NEW_DIRS):
                        js = JS_COMMENT.sub("", js)
                    if js is not None and ((UNSUPPORTED_W7 if d.startswith(NEW_DIRS) else UNSUPPORTED).search(js) or CONTENT_BAD.search(js)):
                        helper_bad = True
                        if "--helpers" in sys.argv: print("HELPER", rel, q)
                        break
            if helper_bad:
                why["unsupported-helper"] = why.get("unsupported-helper", 0) + 1
                continue
            if rel.startswith("html/canvas/"):
                # The images a canvas test fetches or loads from script by an absolute path.
                for ref in re.findall(r"[\"'](/images/[^\"'?#]+\.(?:png|svg|gif|jpg|jpeg))[\"']", text):
                    q = os.path.join(ROOT, ref.lstrip("/"))
                    if os.path.isfile(q):
                        need.add(q)
            if rel.startswith("html/canvas/"):
                # @font-face sources and fonts named from script (url(...) or a FontFace source).
                for ref in re.findall(r"[(\"'](/fonts/[^\"')\s?#]+\.(?:ttf|otf|woff2?))", text):
                    q = os.path.join(ROOT, ref.lstrip("/"))
                    if os.path.isfile(q):
                        need.add(q)
            need.add(p)
            tests.append(rel)
            files |= need
# The fetch tests load resources at run time (fetch("../resources/x.json")): the static files of the shared resources
# directory are vendored with them. Server-side files (.py, .h2., .sub., .headers, .asis) are not: they need HTTP.
FETCH_RES = os.path.join(ROOT, "fetch", "api", "resources")
if any(t.startswith("fetch/api/") for t in tests) and os.path.isdir(FETCH_RES):
    for f in sorted(os.listdir(FETCH_RES)):
        q = os.path.join(FETCH_RES, f)
        if os.path.isfile(q) and not re.search(r"\.(py|h2|sub|headers|asis)$|\.(sub|h2)\.|-worker\.js$", f, re.I):
            files.add(q)
# Raw responses and data files that fetch tests request by a relative URL: the runner serves the .asis files as the HTTP
# response they hold (tests/unit/bindings/test_wpt_th.c).
FETCH_EXTRA = ["xhr/resources/header-content-length.asis", "xhr/resources/header-content-length-twice.asis",
               "xhr/resources/headers-basic.asis", "xhr/resources/headers-double-empty.asis",
               "xhr/resources/headers-some-are-empty.asis", "xhr/resources/headers-www-authenticate.asis",
               "xhr/resources/utf16-bom.json", "fetch/api/cors/resources/not-cors-safelisted.json"]
if any(t.startswith("fetch/api/") for t in tests):
    for f in FETCH_EXTRA:
        q = os.path.join(ROOT, *f.split("/"))
        if os.path.isfile(q):
            files.add(q)
if tests:
    files.add(os.path.join(ROOT, HARNESS.lstrip("/")))
tests = sorted(set(tests))
with open(OUT_LIST, "w") as f:
    f.write("\n".join(tests) + "\n")
with open(OUT_FILES, "w") as f:
    f.write("\n".join(sorted(os.path.relpath(x, ROOT).replace(os.sep, "/") for x in files)) + "\n")
size = sum(os.path.getsize(x) for x in files)
per = {}
for t in tests:
    k = "/".join(t.split("/")[:2]) if t.startswith(("dom/", "html/")) else t.split("/")[0]
    per[k] = per.get(k, 0) + 1
print(len(tests), "tests,", len(files), "files,", round(size / 1e6, 2), "MB")
for k in sorted(per):
    print(" ", k, per[k])
if SHOW_WHY:
    for k in sorted(why):
        print("  excluded", k, why[k])
