// SVGElement without the SVG feature (SZ3b): the base interface of every element in the SVG namespace. It is always built, so that
// `x instanceof SVGElement` (Vue does it) works and the elements keep `style`, `dataset`, `tabIndex`, `focus()` and `blur()` even in
// a build without EMBER_SVG. className, ownerSVGElement and viewportElement (they need SVGAnimatedString and the SVG tree) are
// added by svg.webidl with [Feature=SVG]. Implementation: impl_svgelement.c.
[Exposed=Window] interface SVGElement : Element {
    [SameObject] readonly attribute DOMStringMap dataset;
    [CEReactions] attribute long tabIndex;
    undefined focus();
    undefined blur();
};
SVGElement includes ElementCSSInlineStyle;
