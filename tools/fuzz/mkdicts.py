#!/usr/bin/env python3
"""Writes the libFuzzer dictionaries tests/fuzz/dict/<target>.dict.

    python tools/fuzz/mkdicts.py [--out DIR]

Tokens are Python str with latin-1 escapes for raw bytes (so "\\xc3\\xa9" is the two bytes of a UTF-8 e-acute).
Deterministic: rerunning it changes nothing.
"""
import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--out", default=os.path.normpath(os.path.join(HERE, "..", "..", "tests", "fuzz", "dict")))
args = ap.parse_args()

HTML = [
    "<!DOCTYPE html>", "<!doctype", "<html>", "</html>", "<head>", "<body>", "<!--", "-->", "<![CDATA[", "]]>", "<?", "?>",
    "</", "/>", "<script>", "</script>", "<style>", "</style>", "<template>", "<table>", "<tr>", "<td>", "<th>", "<tbody>",
    "<caption>", "<colgroup>", "<select>", "<option>", "<optgroup>", "<selectedcontent>", "<textarea>", "<title>",
    "<plaintext>", "<xmp>", "<iframe>", "<noembed>", "<noframes>", "<noscript>", "<svg>", "<math>", "<foreignObject>",
    "<annotation-xml encoding=text/html>", "<mi>", "<mtext>", "<desc>", "<form>", "<input>", "<button>", "<a>", "<b>", "<i>",
    "<em>", "<nobr>", "<p>", "<li>", "<dd>", "<dt>", "<ul>", "<ol>", "<h1>", "<frameset>", "<frame>", "<br>", "<hr>",
    "<img src=x onerror=1>", "<ruby>", "<rt>", "<rp>", "<details>", "<dialog>", "<menu>", "<object>", "<embed>",
    "<base href=", "<meta charset=", "<link rel=", "<meta http-equiv=content-type content=", "charset=", "utf-8",
    "windows-1252", "shift_jis", "class=", "id=", "style=", "href=", "src=", "type=", "name=", "value=", "=\"", "='",
    "&amp;", "&lt;", "&gt;", "&quot;", "&apos;", "&nbsp;", "&#x", "&#", "&#0;", "&#x80;", "&#xD800;", "&#x10FFFF;",
    "&notin;", "&notit;", "&AMP", "&amp", "&copy", "&NotEqualTilde;", "&fjlig;", "\x00", "\r\n", "\r", "\xef\xbb\xbf",
    "<![endif]-->", "<!-->", "<!--->", "--!>", "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\">",
    "<a href=\"x\" href=\"y\">", "</p>", "</br>", "</body>", "</table>", "</select>", "</template>", "</svg>",
    "<b><i></b></i>", "<table><td>", "<select><template>", "<svg><p>", "<math><p>", "<p><table>", "<a><table><a>",
]

CSS = [
    "@media", "@import", "@charset \"utf-8\";", "@font-face", "@keyframes", "@supports", "@layer", "@container",
    "@property", "@namespace", "@scope", "@page", "@starting-style", "@counter-style", "@view-transition",
    "@font-feature-values", "!important", "url(", "url(\"", "src(", "calc(", "min(", "max(", "clamp(", "var(--", "env(",
    "attr(", "if(", "image-set(", "linear-gradient(", "radial-gradient(", "conic-gradient(", "repeating-linear-gradient(",
    "rgb(", "rgba(", "hsl(", "hwb(", "lab(", "lch(", "oklab(", "oklch(", "color(", "color-mix(in srgb, ", "light-dark(",
    "device-cmyk(", "from ", " / ", "translate(", "rotate(", "scale(", "matrix(", "matrix3d(", "perspective(", "skew(",
    "minmax(", "repeat(", "fit-content(", "auto-fill", "auto-fit", "span ", "subgrid", "masonry", "anchor(", "anchor-size(",
    "counter(", "counters(", "format(", "supports(", "layer(", "selector(", "font-tech(", "px", "em", "rem", "ex", "ch",
    "vw", "vh", "vmin", "vmax", "cqw", "dvh", "svh", "lvh", "lh", "cap", "ic", "deg", "rad", "turn", "grad", "ms", "s", "dpi",
    "dppx", "x", "fr", "%", ":is(", ":where(", ":not(", ":has(", ":nth-child(", ":nth-of-type(", " of ", ":lang(", ":dir(",
    ":host(", ":host-context(", ":slotted(", "::part(", "::before", "::after", "::first-line", "::first-letter", "::marker",
    "::selection", "::placeholder", "::backdrop", "::cue", "::highlight(", "::view-transition", ":hover", ":focus-visible",
    ":checked", ":root", ":scope", ":is(a, b)", "[a=b i]", "[a|=b]", "[a~=b]", "[a^=b]", "[a$=b]", "[a*=b]", "ns|a", "*|*",
    "|a", "&", ">", "+", "~", "||", "<!--", "-->", "/*", "*/", "\\", "\\0", "\\a ", "\\\n", "U+0025-00FF", "U+4??", "#fff",
    "#ffff", "#ffffffff", "1e3", "1e-3", "+.5", "-.5e+2", "\xef\xbb\xbf", "inherit", "initial", "unset", "revert",
    "revert-layer", "none", "auto", "important", "and", "or", "not", "only", "all", "screen", "print", "(min-width: ",
    "(width >= ", "(prefers-color-scheme: dark)", "display", "position", "grid-template-areas", "grid-area", "font",
    "font-family", "background", "border", "margin", "padding", "transition", "animation", "transform", "content", "--x:",
    "--x: ", "syntax", "inherits", "initial-value",
]

JS = [
    "function", "function*", "async function", "async ", "await ", "yield", "yield*", "class ", "extends ", "static ",
    "static {", "get ", "set ", "constructor", "super(", "super.", "new.target", "import(", "import.meta", "import ",
    "export ", "export default ", "export * from ", "export {", "from ", " as ", "with {", "type: \"json\"", "let ",
    "const ", "var ", "using ", "await using ", "if (", "else", "for (", "for await (", "of ", " in ", "while (", "do {",
    "switch (", "case ", "default:", "break", "continue", "return", "throw ", "try {", "catch (", "finally", "debugger",
    "delete ", "typeof ", "void ", "instanceof ", "new ", "this", "null", "undefined", "true", "false", "NaN", "Infinity",
    "arguments", "eval(", "with (", "\"use strict\"", "=>", "...", "?.", "??", "??=", "||=", "&&=", "**", "**=", ">>>=",
    "<<=", "++", "--", "===", "!==", "<=", ">=", "&&", "||", "?", ":", ";", ",", "(", ")", "[", "]", "{", "}", "${", "`",
    "/*", "*/", "//", "<!--", "-->", "#!", "#priv", "this.#priv", "#priv in x", "@", "\\u0061", "\\u{61}", "\\x61", "\\0",
    "\\07", "\\8", "0b101", "0o17", "0xFF", "017", "09.5", "1_000", "1e3", "1e-3", ".5", "5.", "10n", "0x1Fn", "/./",
    "/[/]/", "/(?<n>a)\\k<n>/u", "/\\p{L}/u", "/[\\p{L}--[a-z]]/v", "/a/dgimsuvy", "Object", "Array", "String", "Number",
    "Boolean", "Symbol", "BigInt", "Math", "JSON", "Date", "RegExp", "Map", "Set", "WeakMap", "WeakSet", "WeakRef",
    "FinalizationRegistry", "Promise", "Proxy", "Reflect", "Intl", "ArrayBuffer", "SharedArrayBuffer", "Uint8Array",
    "Float64Array", "BigInt64Array", "DataView", "Atomics", "Error", "TypeError", "RangeError", "AggregateError",
    "globalThis", "prototype", "__proto__", "toString", "valueOf", "Symbol.iterator", "Symbol.asyncIterator",
    "Symbol.toPrimitive", "Symbol.species", "Symbol.hasInstance", "Symbol.unscopables", "Object.defineProperty(",
    "Object.freeze(", "Object.getOwnPropertyDescriptors(", "Reflect.construct(", "Array.from(", "Array.prototype.",
    "String.prototype.", "JSON.stringify(", "JSON.parse(", "Promise.all(", "Promise.withResolvers(", "queueMicrotask(",
    "Function(", "new Function(", ".call(", ".apply(", ".bind(", ".then(", ".catch(", ".finally(", ".map(", ".flatMap(",
    ".reduce(", ".sort(", ".toSorted(", ".at(", ".replaceAll(", ".matchAll(", ".padStart(", ".normalize(",
    ".localeCompare(", ".toLocaleString(", ".toFixed(", ".toString(36)", "Intl.DateTimeFormat(",
]

REGEXP = [
    "(?:", "(?=", "(?!", "(?<=", "(?<!", "(?<n>", "\\k<n>", "(?i:", "(?-i:", "(?i-m:", "\\p{", "\\P{", "\\p{L}", "\\p{Lu}",
    "\\p{Script=Greek}", "\\p{Emoji}", "\\p{RGI_Emoji}", "\\q{", "[\\p{L}--[a-z]]", "[\\w&&\\d]", "[^", "[]", "[^]", "\\d",
    "\\D", "\\w", "\\W", "\\s", "\\S", "\\b", "\\B", "\\u{", "\\u", "\\x", "\\c", "\\0", "\\1", "\\10", "{1,}", "{,2}",
    "{2,1}", "{1,2}?", "*?", "+?", "??", ".", "^", "$", "|", "-", "a-z", "\\-", "\\/", "\xc3\xa9", "\xf0\x9f\x98\x80",
    "\xed\xa0\x80", "dgimsuvy", "gi", "uv",
]

JSON = [
    "{", "}", "[", "]", ":", ",", "\"", "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t", "\\u", "\\ud800",
    "\\udc00", "\\u0000", "true", "false", "null", "-", "0", "1e", "1E+", "1e-", ".5", "0.", "__proto__", "\"__proto__\":",
    "1e999", "-0", "NaN", "Infinity", "\t", "\n", " ", "\xef\xbb\xbf", "//", "/*", "'",
]

URL = [
    "http://", "https://", "ftp://", "file://", "file:///", "ws://", "wss://", "blob:", "data:", "javascript:",
    "about:blank", "mailto:", "view-source:", "//", "/", "\\", "..", "/..", "/.", "%2e", "%2E%2e", ".%2e", "?", "#", "@",
    ":", ":80", ":0", ":65536", ":99999", "[::1]", "[::]", "[1:2:3:4:5:6:7:8]", "[::ffff:1.2.3.4]", "[::1.2.3.4]",
    "1.2.3.4", "0x7f.1", "0300.0250.0.1", "1.2.3", "4294967295", "4294967296", "xn--", "xn--a", "xn--nxasmq6b",
    "\xc3\xa9", "\xe2\x80\x8d", "\xe2\x80\x8c", "\xdf\x80", "\xef\xbc\x8e", "\xe3\x80\x82", "\xc2\xad", "%", "%zz", "%41",
    "%00", "%ff", "%C0%80", "+", " ", "\t", "\n", "&", "=", "a=b", ";", "'", "\"", "<", ">", "|", "^", "`", "{", "}",
    "localhost", "example.com", "EXAMPLE.COM", "a.b.c", "\x00", "text/plain", ";base64,", ";charset=", "SGVsbG8=",
    "\xc7\x85", "\xc3\x9f", "\xcf\x82",
]

MIME = [
    "text/html", "text/plain", "application/json", "application/javascript", "image/svg+xml", "*/*", "multipart/form-data",
    "application/x-www-form-urlencoded", "; ", ";", "=", "\"", "\\\"", "charset=", "boundary=", "utf-8", "q=0.5", ",", " ",
    "\t", "\r\n", "\n", "\x00", "\x7f", "\xff", "content-type", "content-length", "set-cookie", "host", "cookie", "origin",
    "sec-", "proxy-", "accept", "GET", "POST", "CONNECT", "TRACE", "TRACK", "options", "patch", "PUT", "HEAD", "DELETE",
    "foo-bar", "x-a", "200 OK", "!#$%&'*+-.^_`|~",
]

IMPORTMAP = [
    "{\"imports\":{", "{\"scopes\":{", "\"integrity\":{", "}}", "\":\"", "\":null", "\":[", "\"./", "\"../", "\"/",
    "\"https://", "\"//", "\"data:", "\"blob:", "\"a/\"", "\"a\"", "\"\"", "/", "./", "../", "%2e", "\\", "://", "?", "#", ",",
]

ENCODING = [
    "\xef\xbb\xbf", "\xff\xfe", "\xfe\xff", "\x1b$B", "\x1b(B", "\x1b$@", "\x1b(J", "\x1b$(D", "\x0e", "\x0f", "\x81\x30",
    "\x80", "\xa1", "\xfe", "\xff", "\x8e", "\x8f", "<meta charset=", "<meta http-equiv=", "content-type", "charset=",
    "utf-8", "shift_jis", "gbk", "big5", "euc-kr", "windows-1252", "x-user-defined", "replacement", "utf-16le",
    "\x00\xd8", "\x00\xdc",
]

SETS = {
    "html_doc": HTML, "css_sheet": CSS, "css_selector": CSS, "css_value": CSS, "js_parse": JS, "js_exec": JS,
    "regexp": REGEXP, "json_num": JSON, "url": URL, "mime_headers": MIME, "importmap": IMPORTMAP, "encoding": ENCODING,
}


def esc(tok):
    b = tok.encode("latin-1")
    return "".join(chr(c) if 32 <= c < 127 and chr(c) not in '"\\' else "\\x%02x" % c for c in b)


os.makedirs(args.out, exist_ok=True)
for name, toks in sorted(SETS.items()):
    seen = []
    for t in toks:
        if t and t not in seen:
            seen.append(t)
    with open(os.path.join(args.out, name + ".dict"), "w", newline="\n") as f:
        f.write("# libFuzzer dictionary for fuzz_%s (generated by tools/fuzz/mkdicts.py)\n" % name)
        for t in seen:
            f.write('"%s"\n' % esc(t))
print("wrote %d dictionaries to %s" % (len(SETS), args.out))
