// HTMLCanvasElement (R4b; split from canvas.webidl by SZ3b). It is always built: with EMBER_CANVAS=OFF the element
// is the same, but getContext() returns null for every id, toDataURL() gives "data:," and toBlob() calls back with
// null (impl_canvas_off.c); with EMBER_CANVAS=ON impl_canvas.c implements it. Context and the other canvas
// interfaces: canvas.webidl.

[Exposed=Window, HTMLTag="canvas"] interface HTMLCanvasElement : HTMLElement {
    // The attributes are stored as content attributes (default 300 x 150). Setting either one, by this setter or by
    // setAttribute, resets the bitmap and the context state.
    attribute unsigned long width;
    attribute unsigned long height;
    // contextId 2d gives the CanvasRenderingContext2D (the same object every time); anything else gives null.
    any getContext(DOMString contextId, optional any options = null);
    [Throws] DOMString toDataURL(optional DOMString type = "image/png", optional any quality);
    [Throws] undefined toBlob(any callback, optional DOMString type = "image/png", optional any quality);
};

// SZ3b: here, not in canvas.webidl, for the same reason as SVGBoundingBoxOptions in geometry.webidl.
dictionary ImageDataSettings {
    DOMString colorSpace = "srgb";
    DOMString pixelFormat = "rgba-unorm8";
};

