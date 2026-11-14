#!/usr/bin/env python3
"""Writes the documents WPT serves for foo.window.js tests (foo.window.html), next to the vendored scripts.

Usage: gen_window_wrappers.py WPT_TREE LIST
  WPT_TREE  tests/wpt (the vendored tree: foo.window.js is there, foo.window.html is written here)
  LIST      tests/wpt/testharness.txt (every line ending in .window.html gets its document)

The document is what WPT's server builds for a .window.js test: the harness, our report (tests/wpt-th/testharnessreport.js,
served for /resources/testharnessreport.js), the META script lines of the test in order, and the test itself. The runner
serves the file as it is. Stdlib only.
"""
import os, re, sys

TREE, LIST = sys.argv[1], sys.argv[2]
META = re.compile(r"^//\s*META:\s*script\s*=\s*(.*?)\s*$", re.M)

with open(LIST, encoding="utf-8") as f:
    ids = [ln.strip() for ln in f if ln.strip().endswith(".window.html")]
written = 0
for rel in ids:
    js = os.path.join(TREE, rel[: -len(".html")] + ".js")
    with open(js, encoding="utf-8", errors="replace") as f:
        text = f.read()
    doc = ["<!doctype html>", "<meta charset=utf-8>",
           '<script src="/resources/testharness.js"></script>',
           '<script src="/resources/testharnessreport.js"></script>']
    doc += ['<script src="%s"></script>' % m for m in META.findall(text)]
    doc.append('<script src="%s"></script>' % os.path.basename(js))
    with open(os.path.join(TREE, rel), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(doc) + "\n")
    written += 1
print("wrote", written, "window documents")
