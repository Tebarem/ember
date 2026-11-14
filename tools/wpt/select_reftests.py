#!/usr/bin/env python3
"""Select the WPT reftests W4f runs and every local file they need.

Usage: select_reftests.py WPT_ROOT OUT_LIST OUT_FILES [svg]
With a fourth argument "svg" (R4c) the directories svg/painting, shapes, pservers, struct, coordinate-systems and render are
selected instead: SVG is what they test, so inline svg, .svg files and svg images are allowed; scripts, SMIL animation, filter
elements (R3a does not cover SVG filters), SVG fonts, iframes, objects and embeds still exclude a test.
  OUT_LIST  one test path per line (relative to WPT_ROOT), sorted
  OUT_FILES every file to vendor (tests, references, resources), sorted
Rules: a test has <link rel=match|mismatch>; no file in its reference chain has a <script>, reftest-wait,
vertical or sideways writing modes, SVG (inline, files or data:), non-PNG images, iframes, objects, embeds,
video, audio, or 3D transforms (R3c: 3D transforms are allowed in css/css-transforms, where the 3D renderer is
tested); no print/paged/animated/interact flags; every local resource exists.
"""
import os, re, sys

ROOT, OUT_LIST, OUT_FILES = sys.argv[1], sys.argv[2], sys.argv[3]
SVG_MODE = len(sys.argv) > 4 and sys.argv[4] == "svg"
SVG_DIRS = ["painting", "shapes", "pservers", "struct", "coordinate-systems", "render"]
DIRS = ["CSS2", "css-backgrounds", "css-borders", "css-box", "css-display", "css-flexbox", "css-grid",
        "css-position", "css-tables", "css-text", "css-text-decor", "css-overflow", "css-sizing",
        "css-transforms", "css-values", "css-color", "css-images", "css-lists", "css-content", "css-ui",
        "css-cascade", "filter-effects", "compositing", "css-masking", "css-animations", "css-transitions"]
TEST_EXT = (".html", ".htm", ".xht", ".xhtml") + ((".svg",) if SVG_MODE else ())
BAD_IMG = (".jpg", ".jpeg", ".gif", ".svgz", ".webp", ".bmp", ".ico", ".avif", ".apng") + (() if SVG_MODE else (".svg",))
BAD_TEXT = re.compile(r"<script|shadowroot|reftest-wait|vertical-(?:lr|rl)|sideways-(?:lr|rl)|writing-mode\s*:\s*tb|"
                      r"<svg|image/svg|<iframe|<object|<embed|<video|<audio|<canvas|"
                      r"@page|@media\s+print", re.I)
if SVG_MODE:
    BAD_TEXT = re.compile(r"<script|shadowroot|reftest-wait|vertical-(?:lr|rl)|sideways-(?:lr|rl)|<iframe|<object|<embed|<video|<audio|<canvas|"
                          r"@page|@media\s+print|<(?:svg:)?(?:animate|set|animateTransform|animateMotion|mpath|filter|font|font-face)\b|"
                          r"requiredFeatures|data:image/(?!png|svg)", re.I)
BAD_3D = re.compile(r"perspective|preserve-3d|matrix3d|rotate3d|rotatex|rotatey|translate3d|translatez|"
                    r"scale3d|scalez|backface-visibility", re.I)
ALLOW_3D = ("css-transforms",)
allow3d = False
BAD_FLAGS = re.compile(r'<meta[^>]+name=["\']?flags["\']?[^>]+content=["\'][^"\']*\b(?:paged|animated|interact|'
                       r'userstyle|may|should|dom|font|speech|scroll)\b', re.I)
LINK = re.compile(r"<(?:html:)?link\b[^>]*>", re.I)
ATTR = re.compile(r'''\b(href|src)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))''', re.I)
REL = re.compile(r'''\brel\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))''', re.I)
URL = re.compile(r'''url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)|@import\s+(?:"([^"]*)"|'([^']*)')''', re.I)
cache = {}

def read(p):
    if p not in cache:
        try:
            with open(p, "rb") as f:
                cache[p] = f.read().decode("utf-8", "replace")
        except OSError:
            cache[p] = None
    return cache[p]

def resolve(base, ref):
    ref = ref.strip().split("#")[0].split("?")[0]
    if not ref or re.match(r"^[a-z][a-z0-9+.-]*:", ref, re.I) or ref.startswith("//"):
        return None  # data:, http:, about: and so on are not local files
    if ref.startswith("/"):
        p = os.path.normpath(os.path.join(ROOT, ref.lstrip("/")))
    else:
        p = os.path.normpath(os.path.join(os.path.dirname(base), ref))
    return p if p.startswith(os.path.abspath(ROOT)) else ""

def resources(p, text, out, seen):
    """Add every local resource of a test, reference or stylesheet to out; False if one is unusable."""
    refs = [m.group(2) or m.group(3) or m.group(4) for m in ATTR.finditer(text)]
    refs += [next(g for g in m.groups() if g is not None) for m in URL.finditer(text) if any(m.groups())]
    for r in refs:
        if r is None:
            continue
        q = resolve(p, r)
        if q is None:
            if r.strip().lower().startswith("data:image/") and not r.strip().lower().startswith("data:image/png"):
                return False
            continue
        if q == "" or not os.path.isfile(q):
            return False
        low = q.lower()
        if low.endswith(BAD_IMG):
            return False
        if q in seen:
            continue
        seen.add(q)
        out.add(q)
        if low.endswith(".css"):
            css = read(q)
            if css is None or BAD_TEXT.search(css) or (not allow3d and BAD_3D.search(css)) or not resources(q, css, out, seen):
                return False
    return True

def refs_of(p, text):
    m, mm = [], []
    for l in LINK.finditer(text):
        tag = l.group(0)
        rel = REL.search(tag)
        if not rel:
            continue
        kind = (rel.group(1) or rel.group(2) or rel.group(3) or "").lower().split()
        href = next((a.group(2) or a.group(3) or a.group(4) for a in ATTR.finditer(tag) if a.group(1).lower() == "href"), None)
        if href is None:
            continue
        if "match" in kind:
            m.append(resolve(p, href))
        elif "mismatch" in kind:
            mm.append(resolve(p, href))
    return m, mm

def usable(p, out, depth=0):
    """p and its reference chain are usable; collects files into out."""
    if depth > 8 or not p:
        return False
    text = read(p)
    if text is None or BAD_TEXT.search(text) or BAD_FLAGS.search(text) or (not allow3d and BAD_3D.search(text)):
        return False
    out.add(p)
    if not resources(p, text, out, set()):
        return False
    m, mm = refs_of(p, text)
    for r in m + mm:
        if not r or not os.path.isfile(r) or not usable(r, out, depth + 1):
            return False
    return True

tests, files = [], set()
for d in (SVG_DIRS if SVG_MODE else DIRS):
    allow3d = d in ALLOW_3D
    for dp, dn, fn in os.walk(os.path.join(ROOT, "svg" if SVG_MODE else "css", d)):
        dn[:] = [x for x in dn if x not in ("support", "reference", "resources", "tools") and "print" not in x]
        for f in fn:
            if not f.lower().endswith(TEST_EXT) or re.search(r"(-ref|-notref|-print)\d*\.", f) or "-manual" in f:
                continue
            p = os.path.join(dp, f)
            text = read(p)
            if text is None:
                continue
            m, mm = refs_of(p, text)
            if not m and not mm:
                continue
            need = set()
            if usable(p, need):
                tests.append(os.path.relpath(p, ROOT).replace(os.sep, "/"))
                files |= need
files.add(os.path.join(ROOT, "LICENSE.md"))
with open(OUT_LIST, "w") as f:
    f.write("\n".join(sorted(tests)) + "\n")
with open(OUT_FILES, "w") as f:
    f.write("\n".join(sorted(os.path.relpath(x, ROOT).replace(os.sep, "/") for x in files)) + "\n")
size = sum(os.path.getsize(x) for x in files)
per = {}
for t in tests:
    k = ("svg/" + t.split("/")[1]) if SVG_MODE else t.split("/")[1]
    per[k] = per.get(k, 0) + 1
print(len(tests), "tests,", len(files), "files,", round(size / 1e6, 1), "MB")
for k in sorted(per):
    print(" ", k, per[k])
