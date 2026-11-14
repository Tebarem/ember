#!/usr/bin/env python3
"""Pass rates of the W7 conformance directories from a runner output directory and the expectations.

Usage: score.py EXPECTATIONS_DIR RUN_DIR [RUN_DIR ...]
  EXPECTATIONS_DIR  tests/wpt-th/expectations
  RUN_DIR           the --out directory of a test_bindings_wpt_th run (results.txt and failures.txt)

A failing test or subtest is excluded from the denominator when its expectation has the reason not-applicable (by design: no
network, no WebAssembly, the CJK decoders ...), out-of-scope, needs-http or platform (a feature the W7 spec leaves out, a server
the runner does not have, or a platform difference), or is `not-yet W5e` / `not-yet R4` (a later task takes the feature over:
FileList, the canvas objects). Other not-yet entries and bug count against the rate. The script prints, per first path component pair, the tests passing / tests, the same without the
excluded tests, and the subtests the same way. Stdlib only.
"""
import os, re, sys

EXCLUDED = {"not-applicable", "out-of-scope", "needs-http", "platform", "not-yet W5e", "not-yet R4"}


def label(words):
    """The reason of an expectation line (its words after the path): `not-yet` is qualified by the task that follows it."""
    if words and words[0] == "not-yet" and len(words) > 1:
        return words[0] + " " + words[1]
    return words[0] if words else ""


def load_expectations(d):
    whole, sub = {}, {}
    for fn in os.listdir(d):
        if not fn.endswith(".txt"):
            continue
        with open(os.path.join(d, fn), encoding="utf-8") as f:
            for ln in f:
                ln = ln.rstrip("\n")
                if not ln or ln.startswith("#"):
                    continue
                if " :: " in ln:
                    parts = ln.split(" :: ")
                    tid, name, rest = parts[0], " :: ".join(parts[1:-1]), parts[-1]
                    sub[(tid, name)] = label(rest.split(" "))
                else:
                    tid, *words = ln.split(" ")
                    whole[tid] = label(words)
    return whole, sub


def key_of(tid):
    p = tid.split("/")
    return "/".join(p[:2]) if p[0] in ("html", "fetch", "dom") else p[0]


def main():
    whole, sub = load_expectations(sys.argv[1])
    stats = {}
    for run in sys.argv[2:]:
        fails = {}
        with open(os.path.join(run, "failures.txt"), encoding="utf-8") as f:
            for ln in f:
                p = ln.rstrip("\n").split("\t")
                if len(p) >= 2 and p[1]:
                    fails.setdefault(p[0], []).append(p[1])
        with open(os.path.join(run, "results.txt"), encoding="utf-8") as f:
            for ln in f:
                p = ln.rstrip("\n").split("\t")
                if len(p) < 4:
                    continue
                tid, outcome, npass, ntotal = p[0], p[1], int(p[2]), int(p[3])
                s = stats.setdefault(key_of(tid), dict(t=0, tp=0, tx=0, s=0, sp=0, sx=0))
                s["t"] += 1
                s["s"] += ntotal
                s["sp"] += npass
                if outcome == "pass":
                    s["tp"] += 1
                    continue
                failing = ntotal - npass
                excl = 0
                all_excl = True
                if tid in whole:
                    if whole[tid] in EXCLUDED:
                        excl = failing
                    else:
                        all_excl = False
                else:
                    names = fails.get(tid, [])
                    for n in names:
                        if sub.get((tid, n), "bug") in EXCLUDED:
                            excl += 1
                        else:
                            all_excl = False
                    if failing > len(names):
                        all_excl = False
                s["sx"] += excl
                if all_excl:
                    s["tx"] += 1
    print("%-26s %9s %9s %9s   %13s %9s %9s" % ("directory", "tests", "passing", "rate*", "subtests", "passing", "rate*"))
    for k in sorted(stats):
        s = stats[k]
        tden = s["t"] - s["tx"]
        sden = s["s"] - s["sx"]
        print("%-26s %9d %9d %8.1f%%   %13d %9d %8.1f%%" % (k, s["t"], s["tp"], 100.0 * s["tp"] / tden if tden else 100.0,
                                                           s["s"], s["sp"], 100.0 * s["sp"] / sden if sden else 100.0))
    print("* without the tests and subtests whose failures are all out-of-scope, needs-http or platform")


main()
