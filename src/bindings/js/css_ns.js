// The CSS namespace (W6e): CSS.supports(property, value), CSS.supports(conditionText) and CSS.escape(ident). Embedded by
// impl_css_ns.c and evaluated the first time a page reads the global CSS; the completion value is the namespace object.
// A declaration is supported when a style declaration keeps it (the same check that cssText and setProperty make).
(function () {
'use strict';
const createElement = Document.prototype.createElement;
const setProperty = CSSStyleDeclaration.prototype.setProperty;
const getPropertyValue = CSSStyleDeclaration.prototype.getPropertyValue;
const styleOf = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'style').get;

function declSupported(name, value) {
  name = String(name);
  value = String(value);
  if (name.startsWith('--')) return true;
  if (/!\s*important\s*$/i.test(value)) return false;
  const s = styleOf.call(createElement.call(document, 'div'));
  setProperty.call(s, name, value);
  return getPropertyValue.call(s, name) !== '';
}

// <supports-condition> = not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]*
// Returns true, false, or null when the text is not a condition.
function parseCondition(text) {
  let i = 0;
  const n = text.length;
  const ws = () => { while (i < n && /\s/.test(text[i])) i++; };
  const word = () => {
    const m = /^(not|and|or)(?=[\s(])/i.exec(text.slice(i));
    return m ? m[1].toLowerCase() : null;
  };
  function inParens() {
    ws();
    if (text[i] !== '(') {
      const m = /^(selector|font-tech|font-format)\(/i.exec(text.slice(i));
      if (!m) return null;
      let depth = 0, j = i + m[1].length;
      for (; j < n; j++) { if (text[j] === '(') depth++; else if (text[j] === ')' && --depth === 0) break; }
      if (j >= n) return null;
      i = j + 1;
      return false;
    }
    let depth = 0, j = i;
    for (; j < n; j++) {
      if (text[j] === '(') depth++;
      else if (text[j] === ')' && --depth === 0) break;
    }
    if (j >= n) return null;
    const inner = text.slice(i + 1, j).trim();
    i = j + 1;
    const colon = inner.indexOf(':');
    const head = colon > 0 ? inner.slice(0, colon).trim() : '';
    if (colon > 0 && /^[-\w]+$/.test(head) && !/^(not|and|or)$/i.test(head)) {
      return declSupported(head, inner.slice(colon + 1).trim());
    }
    return parseCondition(inner);
  }
  function condition() {
    ws();
    if (word() === 'not') {
      i += 3;
      const v = inParens();
      ws();
      return v === null || i !== n ? null : !v;
    }
    let v = inParens();
    if (v === null) return null;
    ws();
    const w = word();
    if (w === 'and' || w === 'or') {
      for (;;) {
        ws();
        const w2 = word();
        if (w2 === null) break;
        if (w2 !== w) return null;
        i += w2.length;
        const r = inParens();
        if (r === null) return null;
        v = w === 'and' ? (v && r) : (v || r);
      }
    }
    ws();
    return i === n ? v : null;
  }
  return condition();
}

const CSS = {};
Object.defineProperty(CSS, Symbol.toStringTag, { value: 'CSS', configurable: true });
Object.defineProperties(CSS, {
  supports: {
    value: function supports(a, b) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'supports' on 'CSS': 1 argument required, but only 0 present.");
      if (arguments.length >= 2) return declSupported(a, b);
      const text = String(a).trim();
      let r = parseCondition(text);
      if (r === null && text.indexOf(':') > 0 && text[0] !== '(') r = parseCondition('(' + text + ')');
      return r === true;
    },
    writable: true, enumerable: true, configurable: true,
  },
  escape: {
    value: function escape(value) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'escape' on 'CSS': 1 argument required, but only 0 present.");
      const s = String(value);
      let out = '';
      const len = s.length;
      const first = s.charCodeAt(0);
      for (let i = 0; i < len; i++) {
        const c = s.charCodeAt(i);
        if (c === 0) { out += '�'; continue; }
        if ((c >= 0x1 && c <= 0x1f) || c === 0x7f ||
            (i === 0 && c >= 0x30 && c <= 0x39) ||
            (i === 1 && c >= 0x30 && c <= 0x39 && first === 0x2d)) {
          out += '\\' + c.toString(16) + ' ';
          continue;
        }
        if (i === 0 && len === 1 && c === 0x2d) { out += '\\' + s.charAt(i); continue; }
        if (c >= 0x80 || c === 0x2d || c === 0x5f || (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a)) {
          out += s.charAt(i);
          continue;
        }
        out += '\\' + s.charAt(i);
      }
      return out;
    },
    writable: true, enumerable: true, configurable: true,
  },
});
return CSS;
})()
