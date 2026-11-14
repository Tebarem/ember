// Canvas 2D (R4b): HTMLCanvasElement, CanvasRenderingContext2D, Path2D, CanvasGradient, CanvasPattern,
// ImageData, ImageBitmap, TextMetrics and createImageBitmap. Implementations: impl_canvas.c (element, context, state),
// impl_canvas_path.c (Path2D and the path methods), impl_canvas_data.c (ImageData, ImageBitmap, TextMetrics, gradients and
// patterns). The drawing itself is src/canvas.
//
// Out of scope (labelled): OffscreenCanvas, WebGL contexts (getContext returns null for them), the context filter
// (stored and read back, never applied), drawFocusIfNeeded (a no-op), Display P3 (colorSpace is always srgb).
//
// Conventions. The attributes whose values the specification validates by hand are DOMString here, so that an invalid value
// is ignored by the setter instead of throwing (the WebIDL enum rule of an operation argument would throw). Numeric
// arguments are unrestricted double: the context ignores non-finite values by itself. Arguments that take several types
// (a style, an image source, radii) are any and are classified by the implementation.


[Exposed=Window, BindObj, Feature=Canvas]
interface CanvasGradient {
    [Throws] undefined addColorStop(unrestricted double offset, DOMString color);
};

[Exposed=Window, BindObj, Feature=Canvas]
interface CanvasPattern {
    [Throws] undefined setTransform(optional DOMMatrix2DInit transform = {});
};

[Exposed=Window, BindObj, Feature=Canvas]
interface TextMetrics {
    readonly attribute double width;
    readonly attribute double actualBoundingBoxLeft;
    readonly attribute double actualBoundingBoxRight;
    readonly attribute double fontBoundingBoxAscent;
    readonly attribute double fontBoundingBoxDescent;
    readonly attribute double actualBoundingBoxAscent;
    readonly attribute double actualBoundingBoxDescent;
    readonly attribute double emHeightAscent;
    readonly attribute double emHeightDescent;
    readonly attribute double hangingBaseline;
    readonly attribute double alphabeticBaseline;
    readonly attribute double ideographicBaseline;
};

[Exposed=Window, BindObj, Feature=Canvas]
interface ImageData {
    [Throws] constructor(any a, any b, optional any c, optional any d);
    readonly attribute unsigned long width;
    readonly attribute unsigned long height;
    readonly attribute any data;
    readonly attribute DOMString colorSpace;
    readonly attribute DOMString pixelFormat;
};

[Exposed=Window, BindObj, Feature=Canvas]
interface ImageBitmap {
    readonly attribute unsigned long width;
    readonly attribute unsigned long height;
    undefined close();
};

[Exposed=Window]
partial interface Window {
    // createImageBitmap(image, [sx, sy, sw, sh], [options]): the image is an img, a canvas, an ImageBitmap or an ImageData.
    [Feature=Canvas] any createImageBitmap(any image, optional any a, optional any b, optional any c, optional any d, optional any e);
};

[Exposed=Window, BindObj, Feature=Canvas]
interface Path2D {
    constructor(optional any path);
    [Throws] undefined addPath(Path2D path, optional DOMMatrix2DInit transform = {});
    undefined closePath();
    undefined moveTo(unrestricted double x, unrestricted double y);
    undefined lineTo(unrestricted double x, unrestricted double y);
    undefined quadraticCurveTo(unrestricted double cpx, unrestricted double cpy, unrestricted double x, unrestricted double y);
    undefined bezierCurveTo(unrestricted double cp1x, unrestricted double cp1y, unrestricted double cp2x,
                            unrestricted double cp2y, unrestricted double x, unrestricted double y);
    [Throws] undefined arcTo(unrestricted double x1, unrestricted double y1, unrestricted double x2, unrestricted double y2,
                             unrestricted double radius);
    undefined rect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h);
    [Throws] undefined roundRect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h,
                                 optional any radii = 0);
    [Throws] undefined arc(unrestricted double x, unrestricted double y, unrestricted double radius,
                           unrestricted double startAngle, unrestricted double endAngle, optional boolean counterclockwise = false);
    [Throws] undefined ellipse(unrestricted double x, unrestricted double y, unrestricted double radiusX,
                               unrestricted double radiusY, unrestricted double rotation, unrestricted double startAngle,
                               unrestricted double endAngle, optional boolean counterclockwise = false);
};

[Exposed=Window, BindObj, Feature=Canvas]
interface CanvasRenderingContext2D {
    readonly attribute HTMLCanvasElement canvas;
    any getContextAttributes();
    boolean isContextLost();

    // state
    undefined save();
    [Throws] undefined restore();
    undefined reset();
    // Layers: the state semantics only (the layer is not composed as a group; not-yet).
    undefined beginLayer(optional any options);
    [Throws] undefined endLayer();

    // transforms
    undefined scale(unrestricted double x, unrestricted double y);
    undefined rotate(unrestricted double angle);
    undefined translate(unrestricted double x, unrestricted double y);
    undefined transform(unrestricted double a, unrestricted double b, unrestricted double c, unrestricted double d,
                        unrestricted double e, unrestricted double f);
    any getTransform();
    undefined setTransform(unrestricted double a, unrestricted double b, unrestricted double c, unrestricted double d,
                           unrestricted double e, unrestricted double f);
    [Throws] undefined setTransform(optional DOMMatrix2DInit transform = {});
    undefined resetTransform();

    // compositing and smoothing
    attribute unrestricted double globalAlpha;
    attribute DOMString globalCompositeOperation;
    attribute boolean imageSmoothingEnabled;
    attribute DOMString imageSmoothingQuality;

    // styles
    [Throws] attribute any strokeStyle;
    [Throws] attribute any fillStyle;
    [Throws] any createLinearGradient(unrestricted double x0, unrestricted double y0, unrestricted double x1, unrestricted double y1);
    [Throws] any createRadialGradient(unrestricted double x0, unrestricted double y0, unrestricted double r0,
                                      unrestricted double x1, unrestricted double y1, unrestricted double r1);
    [Throws] any createConicGradient(unrestricted double startAngle, unrestricted double x, unrestricted double y);
    [Throws] any createPattern(any image, any repetition);

    // shadows
    attribute unrestricted double shadowOffsetX;
    attribute unrestricted double shadowOffsetY;
    attribute unrestricted double shadowBlur;
    attribute DOMString shadowColor;

    // filters (stored, not applied)
    attribute DOMString filter;

    // rects
    undefined clearRect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h);
    undefined fillRect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h);
    undefined strokeRect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h);

    // paths
    undefined beginPath();
    undefined fill(optional DOMString fillRule = "nonzero");
    undefined fill(Path2D path, optional DOMString fillRule = "nonzero");
    undefined stroke();
    undefined stroke(Path2D path);
    undefined clip(optional DOMString fillRule = "nonzero");
    undefined clip(Path2D path, optional DOMString fillRule = "nonzero");
    [Throws] boolean isPointInPath(unrestricted double x, unrestricted double y, optional DOMString fillRule = "nonzero");
    [Throws] boolean isPointInPath(Path2D path, unrestricted double x, unrestricted double y, optional DOMString fillRule = "nonzero");
    boolean isPointInStroke(unrestricted double x, unrestricted double y);
    boolean isPointInStroke(Path2D path, unrestricted double x, unrestricted double y);
    undefined drawFocusIfNeeded(any a, optional any b);

    // text
    undefined fillText(DOMString text, unrestricted double x, unrestricted double y, optional unrestricted double maxWidth = Infinity);
    undefined strokeText(DOMString text, unrestricted double x, unrestricted double y, optional unrestricted double maxWidth = Infinity);
    any measureText(DOMString text);

    // images
    [Throws] undefined drawImage(any image, unrestricted double dx, unrestricted double dy);
    [Throws] undefined drawImage(any image, unrestricted double dx, unrestricted double dy, unrestricted double dw, unrestricted double dh);
    [Throws] undefined drawImage(any image, unrestricted double sx, unrestricted double sy, unrestricted double sw,
                                 unrestricted double sh, unrestricted double dx, unrestricted double dy, unrestricted double dw,
                                 unrestricted double dh);

    // pixels
    [Throws] any createImageData([EnforceRange] long sw, [EnforceRange] long sh, optional ImageDataSettings settings = {});
    [Throws] any createImageData(ImageData imagedata);
    [Throws] any getImageData([EnforceRange] long sx, [EnforceRange] long sy, [EnforceRange] long sw, [EnforceRange] long sh, optional ImageDataSettings settings = {});
    [Throws] undefined putImageData(ImageData imagedata, [EnforceRange] long dx, [EnforceRange] long dy);
    [Throws] undefined putImageData(ImageData imagedata, [EnforceRange] long dx, [EnforceRange] long dy, [EnforceRange] long dirtyX, [EnforceRange] long dirtyY, [EnforceRange] long dirtyWidth, [EnforceRange] long dirtyHeight);

    // line styles
    attribute unrestricted double lineWidth;
    attribute DOMString lineCap;
    attribute DOMString lineJoin;
    attribute unrestricted double miterLimit;
    undefined setLineDash(sequence<unrestricted double> segments);
    sequence<double> getLineDash();
    attribute unrestricted double lineDashOffset;

    // text styles
    attribute DOMString font;
    attribute DOMString textAlign;
    attribute DOMString textBaseline;
    attribute DOMString direction;
    attribute DOMString letterSpacing;
    attribute DOMString wordSpacing;
    attribute DOMString fontKerning;
    attribute DOMString fontStretch;
    attribute DOMString fontVariantCaps;
    attribute DOMString textRendering;

    // path building
    undefined closePath();
    undefined moveTo(unrestricted double x, unrestricted double y);
    undefined lineTo(unrestricted double x, unrestricted double y);
    undefined quadraticCurveTo(unrestricted double cpx, unrestricted double cpy, unrestricted double x, unrestricted double y);
    undefined bezierCurveTo(unrestricted double cp1x, unrestricted double cp1y, unrestricted double cp2x,
                            unrestricted double cp2y, unrestricted double x, unrestricted double y);
    [Throws] undefined arcTo(unrestricted double x1, unrestricted double y1, unrestricted double x2, unrestricted double y2,
                             unrestricted double radius);
    undefined rect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h);
    [Throws] undefined roundRect(unrestricted double x, unrestricted double y, unrestricted double w, unrestricted double h,
                                 optional any radii = 0);
    [Throws] undefined arc(unrestricted double x, unrestricted double y, unrestricted double radius,
                           unrestricted double startAngle, unrestricted double endAngle, optional boolean counterclockwise = false);
    [Throws] undefined ellipse(unrestricted double x, unrestricted double y, unrestricted double radiusX,
                               unrestricted double radiusY, unrestricted double rotation, unrestricted double startAngle,
                               unrestricted double endAngle, optional boolean counterclockwise = false);
};
