#!/usr/bin/env python3
"""Builds the committed seed corpora in tests/fuzz/corpus/<target>/ from the repository's own fixtures.

    python tools/fuzz/mkseeds.py [--root DIR] [--test262 DIR] [--out DIR]

--root     the repository (default: the checkout this script is in)
--test262  a test262 checkout (default: ROOT/third_party/test262); the JavaScript seeds use a sample of it
--out      where the corpora go (default: ROOT/tests/fuzz/corpus)

The result is deterministic (fixed seed, sorted inputs), so rerunning it changes nothing. Each seed is a
whole fuzz input: for the harnesses that take a mode byte first, the script writes the mode byte(s)
too. The harnesses for binary formats (inflate, brotli, font, woff, image) read a file as it is
(their options are in the last byte), so their seeds are the fixtures themselves.

A target's directory is cleared before it is written. The total stays under 2 MB (the script checks).
"""
import argparse
import hashlib
import json
import os
import random
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--root", default=os.path.normpath(os.path.join(HERE, "..", "..")))
ap.add_argument("--test262", default=None)
ap.add_argument("--out", default=None)
args = ap.parse_args()
ROOT = args.root
T262 = args.test262 or os.path.join(ROOT, "third_party", "test262")
OUT = args.out or os.path.join(ROOT, "tests", "fuzz", "corpus")
rng = random.Random(0x4D39)  # "M9"

BUDGET = {  # bytes per target
    "html_doc": 260_000, "css_sheet": 200_000, "css_selector": 50_000, "css_value": 90_000,
    "js_parse": 250_000, "js_exec": 160_000, "regexp": 40_000, "json_num": 40_000, "url": 100_000,
    "mime_headers": 40_000, "encoding": 60_000, "importmap": 20_000, "inflate": 60_000,
    "brotli": 90_000, "image": 150_000,
}
out_sizes = {}


def rd(path, limit=None):
    try:
        with open(path, "rb") as f:
            b = f.read() if limit is None else f.read(limit + 1)
    except OSError:
        return None
    if limit is not None and len(b) > limit:
        return None
    return b


def files(root, exts, max_size, sub=None):
    base = os.path.join(ROOT, root) if not os.path.isabs(root) else root
    res = []
    for d, dirs, fs in os.walk(base):
        dirs.sort()
        for f in sorted(fs):
            if os.path.splitext(f)[1].lower() in exts:
                p = os.path.join(d, f)
                try:
                    if os.path.getsize(p) <= max_size:
                        res.append(p)
                except OSError:
                    pass
    return res


class Corpus:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(OUT, name)
        if os.path.isdir(self.dir):
            shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        self.seen = set()
        self.total = 0
        self.budget = BUDGET[name]

    def add(self, data, tag=""):
        if not isinstance(data, bytes):
            data = data.encode("utf-8", "surrogatepass")
        if len(data) == 0 or len(data) > 4096 * 2:
            return False
        h = hashlib.sha1(data).hexdigest()[:16]
        if h in self.seen or self.total + len(data) > self.budget:
            return False
        self.seen.add(h)
        with open(os.path.join(self.dir, h), "wb") as f:
            f.write(data)
        self.total += len(data)
        return True

    def done(self):
        out_sizes[self.name] = (len(self.seen), self.total)


def sample(items, n):
    items = list(items)
    rng.shuffle(items)
    return items[:n]


# ---------------------------------------------------------------- shared sources

def html_files():
    return files("tests/wpt/html", {".html", ".htm"}, 2600) + files("tests/wpt/dom", {".html"}, 2600) + \
        files("tests/pages", {".html"}, 4000) + files("tests/wpt", {".xht", ".xhtml"}, 1800)


def css_files():
    return files("tests/wpt/css", {".css"}, 3000)


STYLE_RE = re.compile(rb"<style[^>]*>(.*?)</style>", re.S | re.I)
STYLE_ATTR_RE = re.compile(rb'style="([^"]{3,200})"', re.I)


def style_blocks(paths, limit):
    res = []
    for p in paths:
        b = rd(p)
        if b is None:
            continue
        for m in STYLE_RE.finditer(b):
            t = m.group(1).strip()
            if 8 < len(t) < 2500:
                res.append(t)
        if len(res) > limit:
            break
    return res


def decls_from(paths, limit):
    res = []
    for p in paths:
        b = rd(p)
        if b is None:
            continue
        for m in re.finditer(rb"([a-z-]+)\s*:\s*([^;{}]{1,120})[;}]", b):
            res.append((m.group(1).decode("latin-1").lower(), m.group(2).strip()))
        for m in STYLE_ATTR_RE.finditer(b):
            for part in m.group(1).split(b";"):
                if b":" in part:
                    k, v = part.split(b":", 1)
                    res.append((k.strip().decode("latin-1").lower(), v.strip()))
        if len(res) > limit:
            break
    return res


def css_parsing_inputs():
    res = []
    d = os.path.join(ROOT, "tests/data/css-parsing-tests")
    if not os.path.isdir(d):
        return res
    for f in sorted(os.listdir(d)):
        if not f.endswith(".json") or f.startswith("color"):
            continue
        try:
            data = json.load(open(os.path.join(d, f), encoding="utf-8"))
        except Exception:
            continue
        for i in range(0, len(data) - 1, 2):
            if isinstance(data[i], str) and 0 < len(data[i]) < 300:
                res.append(data[i])
    return res


def color_inputs():
    res = []
    d = os.path.join(ROOT, "tests/data/css-parsing-tests")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if f.startswith("color") and f.endswith(".json"):
                try:
                    data = json.load(open(os.path.join(d, f), encoding="utf-8"))
                except Exception:
                    continue
                for i in range(0, len(data) - 1, 2):
                    if isinstance(data[i], str) and len(data[i]) < 200:
                        res.append(data[i])
    return res


# ---------------------------------------------------------------- property ids

def prop_ids():
    ids = {}
    p = os.path.join(ROOT, "src/css/props.inc")
    try:
        txt = open(p, encoding="utf-8").read()
    except OSError:
        return ids
    for m in re.finditer(r"EMBI_PROP_([A-Za-z0-9_]+)\s*=\s*(\d+)", txt):
        ids[m.group(1).replace("_", "-")] = int(m.group(2))
    ids.pop("COUNT", None)
    ids.pop("NONE", None)
    return ids


# ---------------------------------------------------------------- targets

def make_html():
    c = Corpus("html_doc")
    prefixes = [b"\x00\x00", b"\x01\x05", b"\x04\x00", b"\x01\x11", b"\x0c\x07", b"\x03\x00", b"\x10\x00", b"\x20\x00"]
    paths = html_files()
    for i, p in enumerate(sample(paths, 100)):
        b = rd(p)
        if b:
            c.add(prefixes[i % len(prefixes)] + b)
    # tree-construction .dat files (#data blocks), as documents and as fragments
    dats = files("tests/wpt", {".dat"}, 200000) + files("third_party/html5lib-tests", {".dat"}, 200000)
    blocks = []
    for p in dats:
        b = rd(p)
        if not b:
            continue
        for m in re.finditer(rb"#data\n(.*?)\n#(?:errors|document)", b, re.S):
            t = m.group(1)
            if 0 < len(t) < 400:
                blocks.append(t)
    for i, t in enumerate(sample(blocks, 160)):
        if i % 2:
            c.add(bytes([2, i % 31]) + t)
        else:
            c.add(bytes([0 if i % 4 == 0 else 1, i % 17 + 1]) + t)
    # tokenizer tests: the "input" strings
    tok_inputs = []
    for p in files("third_party/html5lib-tests/tokenizer", {".test"}, 400000):
        try:
            data = json.load(open(p, encoding="utf-8"))
        except Exception:
            continue
        for t in data.get("tests", []):
            s = t.get("input", "")
            if t.get("doubleEscaped"):
                s = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
            if 0 < len(s) < 200:
                tok_inputs.append(s.encode("utf-8", "surrogatepass"))
    for t in sample(tok_inputs, 450):
        c.add(bytes([0x40 if len(c.seen) % 3 else 0x20, (len(c.seen) * 37) & 0xFF]) + t)
    c.done()


def make_css_sheet():
    c = Corpus("css_sheet")
    paths = css_files()
    modes = [0, 1, 4, 3, 5, 1]
    for i, p in enumerate(sample(paths, 90)):
        b = rd(p)
        if b:
            c.add(bytes([modes[i % len(modes)]]) + b)
    for i, t in enumerate(sample(style_blocks(sample(html_files(), 600), 200), 120)):
        c.add(bytes([modes[i % len(modes)]]) + t)
    for i, t in enumerate(sample(css_parsing_inputs(), 400)):
        c.add(bytes([[0, 3, 1, 4][i % 4]]) + t.encode("utf-8", "surrogatepass"))
    # byte-level inputs for parse_bytes (BOM, @charset, UTF-16)
    for i, t in enumerate([b"\xef\xbb\xbfa{b:c}", b"\xff\xfea\x00{\x00}\x00", b"\xfe\xff\x00a\x00{\x00}",
                           b'@charset "windows-1252"; a{content:"\xe9"}', b'@charset "utf-8";a{b:c}',
                           b"a{b:c}\xff\xfe"]):
        c.add(bytes([2, i * 9]) + t)
    c.done()


def selector_strings():
    res = []
    for p in sample(css_files(), 100):
        b = rd(p)
        if not b:
            continue
        for m in re.finditer(rb"(?:^|})\s*([^{}@/][^{}]{0,150})\{", b):
            t = m.group(1).strip()
            if t:
                res.append(t)
    for p in sample(files("tests/wpt/dom", {".js", ".html"}, 60000), 40):
        b = rd(p)
        if not b:
            continue
        for m in re.finditer(rb"""(?:querySelector(?:All)?|matches|closest)\(\s*(["'])(.{1,150}?)\1\s*\)""", b):
            res.append(m.group(2))
    return res


def make_css_selector():
    c = Corpus("css_selector")
    sels = selector_strings()
    extra = [b"a:is(b, .c) > d", b":has(> img, + p)", b"&:hover", b"a:nth-child(2n+1 of .x)", b"*|*", b"svg|circle",
             b"[foo=bar i]", b":not(:where(a, b))", b"::before", b":lang(en, \"fr\")", b":dir(rtl)",
             b"a b c d e f g", b"input:checked:enabled", b":scope > p", b"p:nth-last-of-type(-n+3)",
             b"tr:nth-child(even) td:first-child"]
    sels += extra
    for i, t in enumerate(sample(sels, 700)):
        mode = [0, 0, 1, 4, 3, 0, 2][i % 7]
        if mode == 3:
            body = b"div.a\x00" + t
        else:
            body = t
        c.add(bytes([mode | (8 if i % 5 == 0 else 0) | (16 if i % 3 == 0 else 0)]) + body)
    for t in [b"2n+1", b"-n+3", b"odd", b"even", b"+5", b"n", b"- n + 2", b"2n + 1 of", b"3n-2"]:
        c.add(bytes([2]) + t)
    c.done()


def make_css_value():
    c = Corpus("css_value")
    ids = prop_ids()
    decls = decls_from(sample(css_files() + html_files(), 500), 6000)
    decls = [d for d in decls if d[0] in ids]
    seen = set()
    n = 0
    for k, v in sample(decls, 1000):
        key = (k, v)
        if key in seen:
            continue
        seen.add(key)
        raw = ids[k] - 1
        mode = [0, 0, 1, 0, 3, 5, 0][n % 7]
        b0 = raw & 255
        b1 = ((raw >> 8) & 1) | (mode << 1)
        body = v
        if mode == 5:
            body = v + b"\x00" + b"10px"
        if c.add(bytes([b0, b1]) + body):
            n += 1
    # one spelled-out value per property: the initial-ish forms keep every property reachable
    for name, pid in sorted(ids.items()):
        raw = pid - 1
        for val in (b"inherit", b"10px 2", b"var(--x, 5px)"):
            c.add(bytes([raw & 255, (raw >> 8) & 1]) + val)
    for t in sample(color_inputs(), 500):
        c.add(bytes([0, 2 << 1]) + t.encode("utf-8", "surrogatepass"))
    for t in [b"calc(1px + 2%)", b"min(1px, 2em)", b"clamp(1px, 50%, 3rem)", b"round(up, 5.3px, 1px)",
              b"atan2(1, 2)", b"calc(sin(30deg) * 1px)", b"hypot(3px, 4px)", b"calc(1/0)", b"calc(infinity * 1px)"]:
        c.add(bytes([0, 3 << 1]) + t)
    for t in [b"<length> | <color>#", b"<length>+\x00 1px 2px", b"<transform-list>\x00translate(1px)",
              b"foo | bar\x00bar", b"*\x00", b"<custom-ident>\x00x", b"<image>\x00url(a.png)",
              b"<length-percentage>#\x00 1px, 2%"]:
        c.add(bytes([0, 4 << 1]) + t)
    for t in [b"var(--a, var(--b, 1px))\x00 2px", b"var(--x) var(--x)\x00 1px", b"var(--ab)\x00var(--cd)",
              b"calc(var(--ab) * 2)\x00 3px"]:
        c.add(bytes([4, 5 << 1]) + t)
    c.done()


def t262_sample(sub, n, max_size=1500):
    base = os.path.join(T262, "test", sub)
    paths = []
    for d, dirs, fs in os.walk(base):
        dirs.sort()
        for f in sorted(fs):
            if f.endswith(".js") and "_FIXTURE" not in f:
                p = os.path.join(d, f)
                try:
                    if os.path.getsize(p) <= max_size:
                        paths.append(p)
                except OSError:
                    pass
    return sample(paths, n)


PRELUDE = (b"var Test262Error=function(m){this.message=m};Test262Error.prototype.toString=function(){return 'T'};"
           b"var assert=function(c,m){if(c!==true)throw new Test262Error(m)};"
           b"assert.sameValue=function(a,b,m){if(a!==b&&(a===a||b===b))throw new Test262Error(m)};"
           b"assert.notSameValue=function(a,b,m){if(a===b)throw new Test262Error(m)};"
           b"assert.throws=function(E,f,m){try{f()}catch(e){return}throw new Test262Error(m)};"
           b"var $DONOTEVALUATE=function(){throw 'x'};var print=function(){};\n")
LICENSE_RE = re.compile(rb"/\*---.*?---\*/", re.S)


def js_sources(n, extra_wpt=True):
    """(text, is_module, is_strict) tuples."""
    res = []
    subs = [("language/expressions", n // 5), ("language/statements", n // 5), ("built-ins", n // 3),
            ("language/literals", n // 25), ("language/module-code", n // 25), ("annexB", n // 25),
            ("language/eval-code", n // 25), ("language/block-scope", n // 25), ("intl402", n // 25)]
    for sub, k in subs:
        for p in t262_sample(sub, k):
            b = rd(p)
            if not b:
                continue
            m = LICENSE_RE.search(b)
            meta = m.group(0) if m else b""
            body = LICENSE_RE.sub(b"", b, count=1).strip()
            body = re.sub(rb"/\*.*?\*/", b"", body, flags=re.S) if len(body) > 200 else body
            is_mod = b"flags: [module" in meta or b", module" in meta
            res.append((body, is_mod, b"onlyStrict" in meta))
    if extra_wpt:
        for p in sample(files("tests/wpt", {".js"}, 3500), 60):
            b = rd(p)
            if b:
                res.append((b, False, False))
    return res


def make_js_parse():
    c = Corpus("js_parse")
    srcs = js_sources(520)
    for i, (body, is_mod, strict) in enumerate(srcs):
        mode = [0, 1, 2, 4, 0, 5, 8, 3][i % 8]
        if strict:
            mode |= 1
        c.add(bytes([mode]) + body)
    for t in [b"`a${b}c${`d${e}`}`", b"/(?<n>a)\\k<n>/u.test('a')", b"class A { static #x = 1; get [y](){} }",
              b"async function* f(){ for await (const x of y) yield* x }", b"a?.b?.[c]?.(d) ?? e",
              b"({a, ...b} = c); [d, ,...e] = f", b"import('x'); import.meta", b"label: for(;;) { break label }",
              b"0b101n + 0o7 + 0xFFn + 1_000", b"\\u0061bc = 1", b"var \\u{61}b", b"x = /[/]/g / 2",
              b"if (a) function f(){}", b"with(a) b", b"'use strict'; 010", b"({ __proto__: 1, __proto__: 2 })",
              b"function f(a = 1, {b}, ...c) { 'use strict' }", b"a => b => c", b"new.target", b"super.x"]:
        c.add(bytes([0b0111]) + t)
    c.done()


def make_js_exec():
    c = Corpus("js_exec")
    srcs = js_sources(420, extra_wpt=False)
    for i, (body, is_mod, strict) in enumerate(srcs):
        mode = 2 if is_mod else (1 if strict else (3 if i % 3 == 0 else 0))
        if i % 11 == 0:
            mode |= 8
        pre = PRELUDE if (not is_mod and i % 2 == 0) else b""
        if is_mod:
            pre = PRELUDE.replace(b"var ", b"globalThis.", 1) if False else b""
        c.add(bytes([mode]) + pre + body)
    progs = [
        b"var a=[]; for (var i=0;i<1000;i++) a.push({i, s:'x'+i}); JSON.stringify(a).length",
        b"var m=new Map; for (var i=0;i<500;i++) m.set('k'+i,i); [...m.keys()].join()",
        b"Promise.resolve(1).then(x=>{throw x}).catch(e=>e).finally(()=>0); (async()=>{await 1; await 2})()",
        b"function* g(){ yield* [1,2,3]; return 4 } [...g()]",
        b"'abc'.replace(/(?<x>b)/g, '[$<x>]') + 'aXbX'.split(/x/i) + /\\p{L}+/u.exec('h\xe9llo')",
        b"class A { #p=1; static s = 2; get p(){return this.#p} } class B extends A {} new B().p",
        b"new Proxy({}, {get(t,k){return k}}).foo + Reflect.ownKeys(Object)",
        b"var o={}; Object.defineProperty(o,'x',{get(){return 1}}); Object.freeze(o); o.x",
        b"new Uint8Array(new ArrayBuffer(8)).fill(7).map(x=>x*2).join() + new Date(0).toISOString()",
        b"1n << 64n | 5n ** 30n % 17n",
        b"[3,1,2].sort((a,b)=>a-b).toString() + [1,[2,[3]]].flat(Infinity)",
        b"label: { try { throw 1 } catch ({message}) { break label } finally { 1 } }",
        b"(function f(n){ return n ? n * f(n-1) : 1 })(20)",
        b"Symbol.iterator in [] && Symbol('x').description + String(Symbol.for('y'))",
        b"new WeakRef({}); new FinalizationRegistry(()=>{}); queueMicrotask?.(()=>{})",
        b"eval('var q = 1; q + 1') + new Function('a','b','return a+b')(1,2)",
        b"Intl.NumberFormat('en').format(1234.5) + new Date(0).toLocaleDateString('en')",
        b"JSON.parse('[1,{\"a\":null}]', function(k,v){ return v }) ",
        b"export default 1; export const a = 2; import * as self from 'entry';",
        b"import x, {y as z} from 'a'; import j from 'b' with { type: 'json' }; export {x, z};",
        b"await 1; export let t = await Promise.resolve(2);",
    ]
    for i, p in enumerate(progs):
        mode = 2 if p.startswith((b"export", b"import", b"await")) else (i % 4 if i % 4 != 2 else 0)
        c.add(bytes([mode]) + p)
    c.done()


def make_regexp():
    c = Corpus("regexp")
    pats = []
    for p in sample(files(os.path.join(T262, "test", "built-ins", "RegExp"), {".js"}, 4000), 500) + \
            sample(t262_sample("language/literals/regexp", 120), 120):
        b = rd(p)
        if not b:
            continue
        for m in re.finditer(rb"(?<![\w)\]/])/((?:[^/\n\\\[]|\\.|\[(?:[^\]\\\n]|\\.)*\])+)/([dgimsuvy]*)", b):
            pats.append((m.group(1), m.group(2)))
    pats += [(b"(a+)+b", b""), (b"(?<n>x)\\k<n>", b"u"), (b"\\p{Lu}+", b"u"), (b"[\\p{L}--[a-z]]", b"v"),
             (b"(?:a|b)*?c", b"i"), (b"^(?=a)(?!b).", b"m"), (b"(.)\\1", b""), (b"\\u{1F600}", b"u"),
             (b"[^]*", b"s"), (b"(?i:a)b", b""), (b"\\b\\w+\\b", b"g"), (b"a{2,5}?", b""), (b"(?<=\\$)\\d+", b""),
             (b"(a*)*", b""), (b"(a|aa)+$", b""), (b"[\\d-x]", b"")]
    subjects = [b"aaaaaaaaaaaaaaaaaaaaaaaaaaaab", b"Hello World 123", b"xx\n\nyy", b"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80",
                b"abcabcabc", b"$12 \xe2\x82\xac12", b""]
    flagbits = {b"d": 1, b"g": 2, b"i": 4, b"m": 8, b"s": 16, b"u": 32, b"v": 64, b"y": 128}
    for i, (pat, fl) in enumerate(sample(pats, 900)):
        if len(pat) > 120:
            continue
        flags = 0
        for ch in fl:
            flags |= flagbits.get(bytes([ch]), 0)
        if flags & 32 and flags & 64:
            flags &= ~64
        subj = subjects[i % len(subjects)]
        c.add(bytes([flags, (i % 2) | ((i % 5) << 1), len(pat)]) + pat + subj)
    c.done()


def make_json_num():
    c = Corpus("json_num")
    jsons = files("tests/data", {".json"}, 3500) + files("tests/wpt", {".json"}, 3500) + \
        files("tests/apps", {".json"}, 3500)
    for i, p in enumerate(jsons):
        b = rd(p)
        if b:
            c.add(bytes([0 if i % 2 else 3]) + b)
    for t in [b'{"a":[1,2.5e-3,-0,true,false,null,"\\u00e9\\ud83d\\ude00"],"b":{"c":{}}}', b"[]", b"{}", b'"x"',
              b"[1,", b"[1e999, -1e999, 1e-999]", b'{"__proto__":1,"a":{"__proto__":[]}}', b"\x00",
              b'"\\ud800"', b"[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]", b"-", b"01", b"1.", b'{"a" 1}',
              b"\xef\xbb\xbf[1]", b'{"a":1,"a":2}', b"123456789012345678901234567890"]:
        c.add(bytes([i for i in (0,)]) + t)
        c.add(bytes([3]) + t)
    nums = [b"0", b"-0", b"1e308", b"1e309", b"5e-324", b"2e-324", b"1.7976931348623157e308",
            b"4.9406564584124654e-324", b"0.1", b"0.3", b"9007199254740993", b"9007199254740992.5",
            b"123456789012345678901234567890", b"0.000001", b"1e21", b"1e-7", b".5", b"5.", b"+7", b"1e+", b"1_0",
            b"2.2250738585072011e-308", b"2.2250738585072014e-308", b"1." + b"0" * 400 + b"1",
            b"8.41e21", b"0.5e-323", b"1.00000000000000011102230246251565404236316680908203125",
            b"1.00000000000000011102230246251565404236316680908203126", b"4503599627370497.5",
            b"-1e-400", b"99999999999999999999999999999999999999e-100"]
    for t in nums:
        c.add(bytes([1]) + t)
    for d in [0.0, 1.0, -2.5, 3.141592653589793, 1e21, 1e-7, 5e-324, 1.7976931348623157e308,
              float("inf"), float("nan"), 123456789.125, 0.1 + 0.2]:
        import struct
        for tail in (b"\x0a\x14", b"\x24\x05", b"\x10\x00ff", b"\x02\x00101"):
            c.add(bytes([2]) + struct.pack("<d", d) + tail)
    c.done()


def make_url():
    c = Corpus("url")
    d = os.path.join(ROOT, "tests/wpt/url/resources")
    try:
        data = json.load(open(os.path.join(d, "urltestdata.json"), encoding="utf-8"))
    except Exception:
        data = []
    ents = [e for e in data if isinstance(e, dict)]
    for i, e in enumerate(sample(ents, 700)):
        inp = e.get("input", "")
        base = e.get("base") or ""
        if len(inp) > 200 or len(base) > 200:
            continue
        val = ["x", "8080", "example.org", "/p/a/t/h", "?q=1", "#f", "https:", "user", "pass"][i % 9]
        c.add(bytes([[0, 4, 0][i % 3]]) + inp.encode("utf-8", "surrogatepass") + b"\x00" +
              base.encode("utf-8", "surrogatepass") + b"\x00" + val.encode())
    try:
        setters = json.load(open(os.path.join(d, "setters_tests.json"), encoding="utf-8"))
    except Exception:
        setters = {}
    n = 0
    for key, lst in setters.items():
        if not isinstance(lst, list):
            continue
        for e in [x for x in lst if isinstance(x, dict)][:12]:
            href = e.get("href", "")
            val = e.get("new_value", "")
            if len(href) < 200 and len(val) < 100:
                c.add(bytes([0]) + href.encode("utf-8", "surrogatepass") + b"\x00\x00" + val.encode("utf-8", "surrogatepass"))
                n += 1
    try:
        idna = json.load(open(os.path.join(d, "toascii.json"), encoding="utf-8"))
    except Exception:
        idna = []
    for e in idna:
        if isinstance(e, dict) and isinstance(e.get("input"), str) and len(e["input"]) < 100:
            c.add(bytes([1]) + e["input"].encode("utf-8", "surrogatepass"))
    try:
        idna2 = json.load(open(os.path.join(d, "IdnaTestV2.json"), encoding="utf-8"))
    except Exception:
        idna2 = []
    for e in sample([x for x in idna2 if isinstance(x, dict)], 150):
        s = e.get("source")
        if isinstance(s, str) and len(s) < 100:
            c.add(bytes([1]) + s.encode("utf-8", "surrogatepass"))
    for t in ["xn--nxasmq6b", "xn--", "xn--a-ecp.ru", "xn--bcher-kva", "xn--ls8h", "xn--zca"]:
        c.add(bytes([1]) + t.encode())
    try:
        pe = json.load(open(os.path.join(d, "percent-encoding.json"), encoding="utf-8"))
    except Exception:
        pe = []
    for e in pe:
        if isinstance(e, dict) and isinstance(e.get("input"), str):
            c.add(bytes([3]) + e["input"].encode("utf-8", "surrogatepass"))
    for t in [b"a=1&b=2&a=3", b"%E2%82%AC=%f0%9f%98%80&x", b"+a+=+b+&&=&=", b"a=%zz&b=%", b"%ff%fe=%c0%80", b"=&=",
              b"data:text/html;base64,PGI+aGk8L2I+", b"data:,hello%20world", b"data:text/plain;charset=x;base64,AA=="]:
        c.add(bytes([2]) + t)
        c.add(bytes([3]) + t)
    c.done()


def make_mime_headers():
    c = Corpus("mime_headers")
    p = os.path.join(ROOT, "tests/wpt/mimesniff/mime-types/resources/mime-types.json")
    try:
        data = json.load(open(p, encoding="utf-8"))
    except Exception:
        data = []
    for e in data:
        if isinstance(e, dict) and isinstance(e.get("input"), str) and len(e["input"]) < 200:
            c.add(bytes([0]) + e["input"].encode("utf-8", "surrogatepass"))
    for t in [b"text/html;charset=utf-8", b'text/plain;a="b\\"c";d=e', b"application/json", b"image/svg+xml;x=y;x=z",
              b"*/*", b"text/html;;;a=b", b"a/b;c=\"d", b'a/b;c="d\xff"', b"TEXT/HTML ; CHARSET = UTF-8"]:
        c.add(bytes([0]) + t)
    seqs = [b"content-type\x00text/html", b"Set-Cookie\x00a=b", b"x-a\x00 v \x00", b"a b\x00c", b"x\x00\x00",
            b"Host\x00evil", b"GET", b"get", b"CONNECT", b"patch", b"FOO BAR", b"200 OK", b"\xff\xfe\x00v"]
    for i in range(160):
        parts = []
        for _ in range(rng.randint(1, 8)):
            parts.append(bytes([rng.randrange(8)]) + rng.choice(seqs) + b"\x00" + rng.choice(seqs))
        c.add(bytes([1 | ((i % 5) << 1)]) + b"".join(parts))
    c.done()


def make_encoding():
    c = Corpus("encoding")
    samples = [b"hello, world\n", bytes(range(0x80, 0x100)), "café € あい 中文 \U0001f600".encode("utf-8"),
               b"\xef\xbb\xbfBOM", b"\xff\xfeA\x00B\x00", b"\xfe\xff\x00A\x00B", b"\x1b$B$\"$$\x1b(B", b"\x81\x40\x82\xa0\xb1\xdf",
               b"\xa4\xa4\xa4\xe5\xb0\xa1", b"\x8e\xa1\xa4\xa1", b"\xb0\xa1\xc7\xd1", b"\x81\x30\x81\x30\x84\x31\xa4\x39",
               b"\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82", b"<meta charset=windows-1251>\xcf\xf0\xe8\xe2\xe5\xf2",
               b"<meta http-equiv=content-type content='text/html; charset=shift_jis'>", b"\x00\xd8\x00\xdc\x3d\xd8"]
    for i in range(40):
        for j, s in enumerate(samples):
            if (i + j) % 3 == 0 or j < 2:
                c.add(bytes([i, ((i * 7 + j) % 8) | (((j * 5) % 32) << 3)]) + s[:400])
    c.done()


def make_importmap():
    c = Corpus("importmap")
    maps = [b'{"imports":{"a":"./a.js","b/":"/b/","https://x/c":"./c.js"},"scopes":{"/s/":{"a":"./sa.js"}}}',
            b'{"imports":{"":"x"}}', b'{"imports":{"a":null,"b":[],"c":"//evil/x"},"integrity":{"./a.js":"sha384-x"}}',
            b'{"scopes":{"https://example.com/":{"x":"./y"}},"imports":{}}', b'{"imports":[]}', b"[]", b"{",
            b'{"imports":{"a/b/":"./","a/":"./x/"}}', b'{"imports":{"a":"data:text/javascript,1"}}']
    specs = [b"a", b"b/x.js", b"./rel.js", b"../up.js", b"/abs.js", b"https://x/c", b"bare", b"a/b/c", b"%2e/x",
             b"data:text/javascript,1", b"blob:x", b"//host/p", b"./", b"", b"a\\b"]
    for i in range(120):
        m1 = rng.choice(maps)
        m2 = rng.choice(maps)
        body = m1 + b"\x00" + m2
        for _ in range(rng.randint(1, 5)):
            body += b"\x00" + rng.choice(specs) + b"\x00" + rng.choice(specs)
        c.add(bytes([i % 4]) + body)
    c.done()


def make_inflate():
    c = Corpus("inflate")
    d = os.path.join(ROOT, "tests/data/deflate")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            b = rd(os.path.join(d, f), 3000)
            if b and f.endswith((".zlib", ".raw")):
                zl = f.endswith(".zlib")
                for stream_mode in (0, 2):
                    c.add(b + bytes([(1 if zl else 0) | stream_mode | ((len(b) % 32) << 2)]))
    c.done()


def make_brotli():
    c = Corpus("brotli")
    d = os.path.join(ROOT, "tests/data/brotli")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if ".compressed" in f:
                b = rd(os.path.join(d, f), 2500)
                if b:
                    c.add(b)
    c.done()


def make_image():
    c = Corpus("image")
    for p in files("tests/image", {".png", ".jpg", ".gif"}, 7000):
        b = rd(p)
        if b:
            for m in (0, 1, 2, 3):
                c.add(b + bytes([m]))
    pngs = files("tests/data/pngsuite", {".png"}, 1500)
    for i, p in enumerate(pngs):
        b = rd(p)
        if b:
            c.add(b + bytes([i % 8]))
    c.done()


def main():
    os.makedirs(OUT, exist_ok=True)
    for fn in (make_html, make_css_sheet, make_css_selector, make_css_value, make_js_parse, make_js_exec,
               make_regexp, make_json_num, make_url, make_mime_headers, make_encoding, make_importmap,
               make_inflate, make_brotli, make_image):
        fn()
    total = 0
    for k in sorted(out_sizes):
        n, sz = out_sizes[k]
        total += sz
        print("%-14s %5d seeds %8d bytes" % (k, n, sz))
    print("total %d bytes" % total)
    # clone seeds come from the harness itself (EMBER_FUZZ_DUMP_SEEDS), not from here
    if total > 2_000_000:
        print("ERROR: seed corpora exceed 2 MB", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
