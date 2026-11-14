// document.fonts (split from canvas.webidl by SZ3b; it is not part of the canvas feature).
// document.fonts (minimal, added with the canvas text tests): the @font-face sources of the document load synchronously
// at style time, so ready is a resolved promise and check() is true. FontFace, add/delete and the load events are not
// implemented (not-yet in the WPT expectations).
[Exposed=Window, BindObj]
interface FontFaceSet {
    readonly attribute any ready;
    readonly attribute DOMString status;
    boolean check(DOMString font, optional DOMString text = "");
    any load(DOMString font, optional DOMString text = "");
};

[Exposed=Window]
partial interface Document {
    [SameObject] readonly attribute FontFaceSet fonts;
};
