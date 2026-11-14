#!/usr/bin/env python3
"""Writes the reftest expectations (tests/reftest/expectations/<dir>.txt) from a run's failures.

    test_paint_reftest --all            # writes <out>/failures.txt (build/tests/reftest-out by default)
    python tools/wpt/classify_reftests.py build/tests/reftest-out/failures.txt

Every failing test gets a reason tag, chosen by the first rule that matches (the path of the test, then the
content of the test and of its references; see RULES). A test that is already listed keeps its tag and note, so
a hand-made classification survives a rerun. Tests that match no rule are tagged `bug`: the runner caps those at 3%
of a directory, so every `bug` line is a defect to fix or to classify, not a place to park failures.

The reason tags (the runner checks them): not-yet, writing-mode, multicol, r3-effects, svg, jpeg, xhtml, font-metrics,
needs-js, needs-http, out-of-scope, not-applicable, layout, paint, bug.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
WPT = os.path.join(ROOT, "tests", "wpt")
EXP = os.path.join(ROOT, "tests", "reftest", "expectations")


def read(path):
    try:
        with open(os.path.join(WPT, path), encoding="utf8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def refs_of(test, text):
    out = []
    for m in re.finditer(r"<link\b[^>]*>", text, re.I):
        tag = m.group(0)
        rel = re.search(r"rel\s*=\s*[\"']?(match|mismatch)", tag, re.I)
        href = re.search(r"href\s*=\s*[\"']([^\"']+)[\"']", tag, re.I)
        if rel and href:
            h = href.group(1).split("#")[0].split("?")[0]
            if h.startswith("/"):
                p = h[1:]
            else:
                p = os.path.normpath(os.path.join(os.path.dirname(test), h)).replace("\\", "/")
            out.append(p)
    return out


def chain_text(test, depth=0, seen=None):
    seen = seen if seen is not None else set()
    if test in seen or depth > 6:
        return ""
    seen.add(test)
    text = read(test)
    for r in refs_of(test, text):
        text += "\n" + chain_text(r, depth + 1, seen)
    return text


# (tag, note, path regex or None, content regex or None). The first matching rule wins.
RULES = [
    ("not-yet", "CSS animations and transitions are not applied to the style (only the animation timing model exists)", r"filter-effects/animation/", None),
    ("not-yet", "the filter() image function (CSS Images 4 section 2.2.1)", r"filter-effects/filter-function/", None),
    ("not-yet", "svg-filters: filter: url(#id) references an SVG filter (R4)", r"filter-effects/.*(svg|url)", None),
    ("r3-effects", "backdrop root: a will-change, mix-blend-mode, clip-path (R3b) or nested backdrop-filter parent isolates the backdrop (Filter Effects 2 section 2.1); only opacity and filter groups are roots here", r"filter-effects/backdrop-filter-backdrop-root", None),
    ("layout", "a filtered inline with block children, or an inline containing block, has no fragment of its own, so it forms no stacking context or containing block", r"filter-effects/(filtered-inline|filter-cb-abspos-inline)", None),
    ("jpeg", "the test's background is a PNG data URL: PNG decoding is not part of the allowed image codecs (JPEG and GIF)", r"filter-effects/hidpi-invert", None),
    ("not-yet", "grid-lanes (CSS masonry layout)", r"/grid-lanes/", None),
    ("not-yet", "line-clamp and block-ellipsis", r"/line-clamp/", None),
    ("not-yet", "scroll markers (::scroll-marker, scroll-marker-group)", r"/scroll-markers/", None),
    ("not-yet", "corner-shape", r"/corner-shape/", None),
    ("not-yet", "border-shape", r"/border-shape/", None),
    ("out-of-scope", "display: run-in (removed from CSS, no engine ships it)", r"/run-in/", None),
    ("not-yet", "hyphens: auto needs hyphenation dictionaries", r"/hyphens/", None),
    ("not-yet", "image-set()", r"/image-set/", None),
    ("not-yet", "text-autospace", r"/text-autospace/", None),
    ("not-yet", "text-justify", r"/text-justify/", None),
    ("not-yet", "text-wrap: balance and pretty", r"/(wrap-inside|balance)/|text-wrap", None),
    ("not-yet", "hanging-punctuation", r"/hanging-punctuation/", None),
    ("not-yet", "contain-intrinsic-size", r"/contain-intrinsic-size/", None),
    ("not-yet", "margin-trim", r"/margin-trim/", None),
    ("not-yet", "text-fit", r"/text-fit/", None),
    ("not-yet", "subgrid", r"/subgrid/", None),
    ("not-yet", "tab-size", r"/tab-size/", None),
    ("not-yet", "word-space-transform", r"/word-space-transform/", None),
    ("not-yet", "::first-line: not implemented (a spec deferral of W4f; it needs the first formatted line's text before line breaking)", None, r"::?first-line"),
    ("not-yet", "ol reversed: the start value must account for li value attributes, and reversed list-item counters (HTML ordinal value)", r"li-value-reversed|counter-reset-reversed|counter-reversed|foo-counter-reversed", None),
    ("not-yet", "text-decoration-inset and text-decoration-skip-spaces are not implemented", r"text-decoration-(inset|skip-spaces)", None),
    ("not-yet", "text-overflow with a string value (and its interaction with line-clamp)", r"text-overflow-string", None),
    ("not-yet", "cross-fade(), image(), element(), image-orientation: from-image, color images", r"cross-fade|css-images/image-color|css-images/image-light-dark|image-orientation|element-replacement|css-image-fallbacks|image-rendering-", None),
    ("not-yet", "sibling-index() and sibling-count()", r"sibling-index|sibling-count", None),
    ("not-yet", "font-variant: small-caps is not synthesized (the default face has no smcp)", r"CSS2/fonts/font-variant", None),
    ("not-yet", "attr() with types, calc-size(), the lh and rlh units", r"css-values/(attr-|calc-size|lh-unit|rlh-unit)", None),
    ("not-yet", "revert-layer and the all property details", r"css-cascade/(revert-layer|all-prop)", None),
    ("not-yet", "::marker details", None, r"::marker"),
    ("not-yet", "::selection, ::highlight and other highlight pseudo-elements", None, r"::(selection|highlight|target-text|spelling-error|grammar-error)"),
    ("not-yet", "anchor positioning", None, r"anchor-name|position-anchor|anchor\("),
    ("not-yet", "@scope", None, r"@scope"),
    ("not-yet", "initial-letter", None, r"initial-letter"),
    ("not-yet", "shape-outside", None, r"shape-outside"),
    ("not-yet", "ruby layout", None, r"<ruby|display:\s*ruby"),
    ("not-yet", "form controls", None, r"<(input|select|textarea|button|meter|progress)\b"),
    ("writing-mode", "", None, r"writing-mode|vertical-(rl|lr)|sideways-|text-orientation|text-combine-upright"),
    ("multicol", "", None, r"column-count|column-width|columns\s*:|column-rule|column-span|column-fill"),
    ("r3-effects", "box-shadow, text-shadow, filters, masks, clip-path, blend modes, border-image (R3)", None,
     r"box-shadow|text-shadow|filter\s*:|clip-path|mask(-[a-z]+)?\s*:|mix-blend-mode|backdrop-filter|border-image|"
     r"background-blend-mode|background-clip\s*:\s*text|drop-shadow\("),
    ("needs-http", "the test needs HTTP response headers", None, r"<meta[^>]+name=[\"']?flags[\"']?[^>]+content=[\"'][^\"']*\bhttp\b"),
    ("needs-js", "", None, r"<script"),
]


def classify(test, detail):
    for tag, note, pathre, contre in RULES:
        if pathre and re.search(pathre, test):
            return tag, note
    text = None
    for tag, note, pathre, contre in RULES:
        if contre:
            if text is None:
                text = chain_text(test)
            if re.search(contre, text, re.I):
                return tag, note
    m = re.search(r"(\d+) px differ, max (\d+)", detail)
    if m and "mismatch" not in detail:
        if int(m.group(1)) <= 8:
            return "font-metrics", "a few antialiasing pixels: glyph or line metrics of the default font differ from the reference's"
        if int(m.group(2)) <= 64 and int(m.group(1)) <= 700:
            return "font-metrics", "partial-coverage pixels only (max difference <= 64): sub-pixel glyph or edge positions differ from the reference's"
    return "bug", ""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    old = {}
    if os.path.isdir(EXP):
        for name in os.listdir(EXP):
            if name.endswith(".txt"):
                for line in open(os.path.join(EXP, name), encoding="utf8"):
                    parts = line.rstrip("\n").split(" ", 2)
                    if len(parts) >= 2 and not line.startswith("#"):
                        old[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "")
    by_dir = {}
    for line in open(sys.argv[1], encoding="utf8"):
        test, _, detail = line.rstrip("\n").partition("\t")
        if test.startswith("ours/"):
            print("ours test fails: " + test + " " + detail)
            continue
        d = test.split("/")[1]
        tag, note = old.get(test) or classify(test, detail)
        by_dir.setdefault(d, []).append((test, tag, note))
    os.makedirs(EXP, exist_ok=True)
    for name in os.listdir(EXP):
        if name.endswith(".txt"):
            os.remove(os.path.join(EXP, name))
    counts = {}
    for d, items in sorted(by_dir.items()):
        with open(os.path.join(EXP, d + ".txt"), "w", encoding="utf8", newline="\n") as f:
            for test, tag, note in sorted(items):
                f.write(test + " " + tag + ((" " + note) if note else "") + "\n")
                counts[tag] = counts.get(tag, 0) + 1
    for tag, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("%-14s %5d" % (tag, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
