// HTMLStyleElement.sheet (W6e). Embedded by impl_css_ns.c; the completion value is a function (element) -> CSSStyleSheet|null.
// The sheet is a snapshot: a CSSStyleSheet made from the element's text, made again when the text has changed. It is not the
// sheet the cascade uses (the document keeps its own), and its rules are the ones CSSStyleSheet.cssRules reports.
(function () {
'use strict';
const cache = new WeakMap();
const text = Object.getOwnPropertyDescriptor(Node.prototype, 'textContent').get;
const connected = Object.getOwnPropertyDescriptor(Node.prototype, 'isConnected').get;
return function sheetOf(el) {
  if (!connected.call(el)) return null;
  const t = text.call(el);
  let e = cache.get(el);
  if (e === undefined || e.text !== t) {
    const s = new CSSStyleSheet();
    s.replaceSync(t);
    e = { text: t, sheet: s };
    cache.set(el, e);
  }
  return e.sheet;
};
})()
