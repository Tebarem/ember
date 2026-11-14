[Exposed=Window, BindObj, CSSProperties]
interface CSSStyleDeclaration {
    [CEReactions] attribute DOMString cssText;
    readonly attribute unsigned long length;
    getter DOMString item(unsigned long index);
    DOMString getPropertyValue(DOMString property);
    DOMString getPropertyPriority(DOMString property);
    [CEReactions] undefined setProperty(DOMString property, [LegacyNullToEmptyString] DOMString value,
                                        [LegacyNullToEmptyString] optional DOMString priority = "");
    [CEReactions] DOMString removeProperty(DOMString property);
};

// Constructable stylesheets (W7d): CSSStyleSheet and document.adoptedStyleSheets. The
// implementation is impl_cssom_sheet.c: a CSSStyleSheet wrapper holds a parsed sheet, and document.adoptedStyleSheets feeds
// the adopted sheets into the document's cascade after its own sheets.

[Exposed=Window, BindObj]
interface CSSStyleSheet {
    constructor(optional any options);
    // The top-level rules as a new array of objects with the CSSRule type number (cssText is not kept).
    readonly attribute any cssRules;
    [Throws] undefined replaceSync(USVString text);
    Promise<any> replace(USVString text);
};

partial interface Document {
    [Throws] attribute any adoptedStyleSheets;
};
