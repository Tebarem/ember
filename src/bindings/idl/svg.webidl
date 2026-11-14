// SVG DOM (SVG 2, trimmed; R4d). The elements are nodes in the SVG namespace: [SVGTag] (the SVG table of idlgen,
// case-sensitive) picks the interface, and the default is SVGElement.
//
// The values are live. An SVGAnimated* is a view of its element: a read or write goes to the attribute at that moment.
// baseVal of an SVGAnimatedLength, SVGAnimatedAngle, SVGAnimatedRect and SVGAnimatedPreserveAspectRatio is an object bound
// to the attribute (a read parses it, a write serializes it back). The lists (SVGLengthList, SVGNumberList, SVGPointList,
// SVGTransformList, SVGStringList) keep their items as objects with identity and write through to the attribute; an
// attribute change re-parses the list and updates the held items in place (impl_svg_val.c). animVal is the base value
// (no SMIL: out of scope), a read-only copy for the single values, the same list for the lists.
//
// The geometry methods (getBBox, getCTM, getScreenCTM, getTotalLength, getPointAtLength, isPointIn*) call the hook in
// bindings/svg_geometry.h, which R4c installs.

// SVGPoint, SVGMatrix and SVGRect (the types of createSVGPoint(), transform.matrix, viewBox.baseVal and the items of
// `points`) are DOMPoint, DOMMatrix and DOMRect (SVG 2, R4.md): an SVGMatrix is a DOMMatrix, so everything that takes
// a DOMMatrix or a DOMMatrix2DInit takes one, and the other way round. They only restrict what the WPT svg/types tests
// (SVGPoint.html, SVGMatrix.html, SVGRect.html: svgwg#706 reverted the plain alias) require: their members are floats, so
// NaN and Infinity throw a TypeError. The members of the three base interfaces are unrestricted doubles.
//
// All six types are live where they belong to an attribute or a list item: an accessor reads the attribute first and a
// write goes back to it, and the accessors of an animVal throw NoModificationAllowedError (impl_svg_val.c).
[Exposed=Window, BindObj, Feature=SVG]
interface SVGPoint : DOMPoint {
    [Throws] attribute float x;
    [Throws] attribute float y;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGMatrix : DOMMatrix {
    [Throws] attribute float a;
    [Throws] attribute float b;
    [Throws] attribute float c;
    [Throws] attribute float d;
    [Throws] attribute float e;
    [Throws] attribute float f;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGRect : DOMRect {
    [Throws] attribute float x;
    [Throws] attribute float y;
    [Throws] attribute float width;
    [Throws] attribute float height;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGNumber {
    [Throws] attribute float value;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGLength {
    const unsigned short SVG_LENGTHTYPE_UNKNOWN = 0;
    const unsigned short SVG_LENGTHTYPE_NUMBER = 1;
    const unsigned short SVG_LENGTHTYPE_PERCENTAGE = 2;
    const unsigned short SVG_LENGTHTYPE_EMS = 3;
    const unsigned short SVG_LENGTHTYPE_EXS = 4;
    const unsigned short SVG_LENGTHTYPE_PX = 5;
    const unsigned short SVG_LENGTHTYPE_CM = 6;
    const unsigned short SVG_LENGTHTYPE_MM = 7;
    const unsigned short SVG_LENGTHTYPE_IN = 8;
    const unsigned short SVG_LENGTHTYPE_PT = 9;
    const unsigned short SVG_LENGTHTYPE_PC = 10;
    readonly attribute unsigned short unitType;
    [Throws] attribute float value;
    [Throws] attribute float valueInSpecifiedUnits;
    [Throws] attribute DOMString valueAsString;
    [Throws] undefined newValueSpecifiedUnits(unsigned short unitType, float valueInSpecifiedUnits);
    [Throws] undefined convertToSpecifiedUnits(unsigned short unitType);
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAngle {
    const unsigned short SVG_ANGLETYPE_UNKNOWN = 0;
    const unsigned short SVG_ANGLETYPE_UNSPECIFIED = 1;
    const unsigned short SVG_ANGLETYPE_DEG = 2;
    const unsigned short SVG_ANGLETYPE_RAD = 3;
    const unsigned short SVG_ANGLETYPE_GRAD = 4;
    readonly attribute unsigned short unitType;
    [Throws] attribute float value;
    [Throws] attribute float valueInSpecifiedUnits;
    [Throws] attribute DOMString valueAsString;
    [Throws] undefined newValueSpecifiedUnits(unsigned short unitType, float valueInSpecifiedUnits);
    [Throws] undefined convertToSpecifiedUnits(unsigned short unitType);
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGPreserveAspectRatio {
    const unsigned short SVG_PRESERVEASPECTRATIO_UNKNOWN = 0;
    const unsigned short SVG_PRESERVEASPECTRATIO_NONE = 1;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMINYMIN = 2;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMIDYMIN = 3;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMAXYMIN = 4;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMINYMID = 5;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMIDYMID = 6;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMAXYMID = 7;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMINYMAX = 8;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMIDYMAX = 9;
    const unsigned short SVG_PRESERVEASPECTRATIO_XMAXYMAX = 10;
    const unsigned short SVG_MEETORSLICE_UNKNOWN = 0;
    const unsigned short SVG_MEETORSLICE_MEET = 1;
    const unsigned short SVG_MEETORSLICE_SLICE = 2;
    [Throws] attribute unsigned short align;
    [Throws] attribute unsigned short meetOrSlice;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGTransform {
    const unsigned short SVG_TRANSFORM_UNKNOWN = 0;
    const unsigned short SVG_TRANSFORM_MATRIX = 1;
    const unsigned short SVG_TRANSFORM_TRANSLATE = 2;
    const unsigned short SVG_TRANSFORM_SCALE = 3;
    const unsigned short SVG_TRANSFORM_ROTATE = 4;
    const unsigned short SVG_TRANSFORM_SKEWX = 5;
    const unsigned short SVG_TRANSFORM_SKEWY = 6;
    readonly attribute unsigned short type;
    [SameObject] readonly attribute SVGMatrix matrix;
    readonly attribute float angle;
    [Throws] undefined setMatrix(optional DOMMatrix2DInit matrix = {});
    [Throws] undefined setTranslate(float tx, float ty);
    [Throws] undefined setScale(float sx, float sy);
    [Throws] undefined setRotate(float angle, float cx, float cy);
    [Throws] undefined setSkewX(float angle);
    [Throws] undefined setSkewY(float angle);
};

// ------------------------------------------------------------------ lists

// has the indexed property setter of the SVG 2 IDL: list[i] = item is replaceItem(item, i) ([IndexedSetter], idlgen)
[Exposed=Window, BindObj, IndexedSetter, Feature=SVG]
interface SVGLengthList {
    readonly attribute unsigned long length;
    readonly attribute unsigned long numberOfItems;
    getter SVGLength (unsigned long index);
    [Throws] undefined clear();
    [Throws] SVGLength initialize(SVGLength newItem);
    [Throws] SVGLength getItem(unsigned long index);
    [Throws] SVGLength insertItemBefore(SVGLength newItem, unsigned long index);
    [Throws] SVGLength replaceItem(SVGLength newItem, unsigned long index);
    [Throws] SVGLength removeItem(unsigned long index);
    [Throws] SVGLength appendItem(SVGLength newItem);
};

// has the indexed property setter of the SVG 2 IDL: list[i] = item is replaceItem(item, i) ([IndexedSetter], idlgen)
[Exposed=Window, BindObj, IndexedSetter, Feature=SVG]
interface SVGNumberList {
    readonly attribute unsigned long length;
    readonly attribute unsigned long numberOfItems;
    getter SVGNumber (unsigned long index);
    [Throws] undefined clear();
    [Throws] SVGNumber initialize(SVGNumber newItem);
    [Throws] SVGNumber getItem(unsigned long index);
    [Throws] SVGNumber insertItemBefore(SVGNumber newItem, unsigned long index);
    [Throws] SVGNumber replaceItem(SVGNumber newItem, unsigned long index);
    [Throws] SVGNumber removeItem(unsigned long index);
    [Throws] SVGNumber appendItem(SVGNumber newItem);
};

// has the indexed property setter of the SVG 2 IDL: list[i] = item is replaceItem(item, i) ([IndexedSetter], idlgen)
[Exposed=Window, BindObj, IndexedSetter, Feature=SVG]
interface SVGPointList {
    readonly attribute unsigned long length;
    readonly attribute unsigned long numberOfItems;
    getter SVGPoint (unsigned long index);
    [Throws] undefined clear();
    [Throws] SVGPoint initialize(DOMPoint newItem);
    [Throws] SVGPoint getItem(unsigned long index);
    [Throws] SVGPoint insertItemBefore(DOMPoint newItem, unsigned long index);
    [Throws] SVGPoint replaceItem(DOMPoint newItem, unsigned long index);
    [Throws] SVGPoint removeItem(unsigned long index);
    [Throws] SVGPoint appendItem(DOMPoint newItem);
};

// has the indexed property setter of the SVG 2 IDL: list[i] = item is replaceItem(item, i) ([IndexedSetter], idlgen)
[Exposed=Window, BindObj, IndexedSetter, Feature=SVG]
interface SVGTransformList {
    readonly attribute unsigned long length;
    readonly attribute unsigned long numberOfItems;
    getter SVGTransform (unsigned long index);
    [Throws] undefined clear();
    [Throws] SVGTransform initialize(SVGTransform newItem);
    [Throws] SVGTransform getItem(unsigned long index);
    [Throws] SVGTransform insertItemBefore(SVGTransform newItem, unsigned long index);
    [Throws] SVGTransform replaceItem(SVGTransform newItem, unsigned long index);
    [Throws] SVGTransform removeItem(unsigned long index);
    [Throws] SVGTransform appendItem(SVGTransform newItem);
    SVGTransform createSVGTransformFromMatrix(optional DOMMatrix2DInit matrix = {});
    SVGTransform? consolidate();
};

// has the indexed property setter of the SVG 2 IDL: list[i] = item is replaceItem(item, i) ([IndexedSetter], idlgen)
[Exposed=Window, BindObj, IndexedSetter, Feature=SVG]
interface SVGStringList {
    readonly attribute unsigned long length;
    readonly attribute unsigned long numberOfItems;
    getter DOMString (unsigned long index);
    [Throws] undefined clear();
    [Throws] DOMString initialize(DOMString newItem);
    [Throws] DOMString getItem(unsigned long index);
    [Throws] DOMString insertItemBefore(DOMString newItem, unsigned long index);
    [Throws] DOMString replaceItem(DOMString newItem, unsigned long index);
    [Throws] DOMString removeItem(unsigned long index);
    [Throws] DOMString appendItem(DOMString newItem);
};

// ------------------------------------------------------------------ animated values

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedString {
    attribute DOMString baseVal;
    readonly attribute DOMString animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedNumber {
    [Throws] attribute float baseVal;
    readonly attribute float animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedEnumeration {
    [Throws] attribute unsigned short baseVal;
    readonly attribute unsigned short animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedLength {
    [SameObject] readonly attribute SVGLength baseVal;
    [SameObject] readonly attribute SVGLength animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedAngle {
    [SameObject] readonly attribute SVGAngle baseVal;
    [SameObject] readonly attribute SVGAngle animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedRect {
    [SameObject] readonly attribute SVGRect baseVal;
    [SameObject] readonly attribute SVGRect animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedPreserveAspectRatio {
    [SameObject] readonly attribute SVGPreserveAspectRatio baseVal;
    [SameObject] readonly attribute SVGPreserveAspectRatio animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedLengthList {
    [SameObject] readonly attribute SVGLengthList baseVal;
    [SameObject] readonly attribute SVGLengthList animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedNumberList {
    [SameObject] readonly attribute SVGNumberList baseVal;
    [SameObject] readonly attribute SVGNumberList animVal;
};

[Exposed=Window, BindObj, Feature=SVG]
interface SVGAnimatedTransformList {
    [SameObject] readonly attribute SVGTransformList baseVal;
    [SameObject] readonly attribute SVGTransformList animVal;
};

[Exposed=Window, Feature=SVG]
interface SVGUnitTypes {
    const unsigned short SVG_UNIT_TYPE_UNKNOWN = 0;
    const unsigned short SVG_UNIT_TYPE_USERSPACEONUSE = 1;
    const unsigned short SVG_UNIT_TYPE_OBJECTBOUNDINGBOX = 2;
};

// ------------------------------------------------------------------ elements

// SVGElement itself (dataset, tabIndex, focus, blur and style) is in svgelement.webidl and always built: frameworks test
// `x instanceof SVGElement`. These members need the SVG value machinery.
[Exposed=Window]
partial interface SVGElement {
    [Feature=SVG, SameObject] readonly attribute SVGAnimatedString className;
    [Feature=SVG] readonly attribute SVGSVGElement? ownerSVGElement;
    [Feature=SVG] readonly attribute SVGElement? viewportElement;
};

[Exposed=Window, Feature=SVG] interface SVGGraphicsElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedTransformList transform;
    [SameObject] readonly attribute SVGStringList requiredExtensions;
    [SameObject] readonly attribute SVGStringList systemLanguage;
    DOMRect getBBox(optional SVGBoundingBoxOptions options = {});
    DOMMatrix? getCTM();
    DOMMatrix? getScreenCTM();
};

[Exposed=Window, Feature=SVG] interface SVGGeometryElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedNumber pathLength;
    float getTotalLength();
    DOMPoint getPointAtLength(float distance);
    boolean isPointInFill(optional DOMPointInit point = {});
    boolean isPointInStroke(optional DOMPointInit point = {});
};

[Exposed=Window, SVGTag="svg", Feature=SVG] interface SVGSVGElement : SVGGraphicsElement {
    const unsigned short SVG_ZOOMANDPAN_UNKNOWN = 0;
    const unsigned short SVG_ZOOMANDPAN_DISABLE = 1;
    const unsigned short SVG_ZOOMANDPAN_MAGNIFY = 2;
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
    [SameObject] readonly attribute SVGAnimatedRect viewBox;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
    [SameObject] readonly attribute DOMPointReadOnly currentTranslate;
    attribute float currentScale;
    [Throws] attribute unsigned short zoomAndPan;
    Element? getElementById(DOMString elementId);
    SVGNumber createSVGNumber();
    SVGLength createSVGLength();
    SVGAngle createSVGAngle();
    SVGRect createSVGRect();
    SVGMatrix createSVGMatrix();
    SVGPoint createSVGPoint();
    SVGTransform createSVGTransform();
    SVGTransform createSVGTransformFromMatrix(optional DOMMatrix2DInit matrix = {});
};

[Exposed=Window, SVGTag="g", Feature=SVG] interface SVGGElement : SVGGraphicsElement {};
[Exposed=Window, SVGTag="defs", Feature=SVG] interface SVGDefsElement : SVGGraphicsElement {};
[Exposed=Window, SVGTag="symbol", Feature=SVG] interface SVGSymbolElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedRect viewBox;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
};
[Exposed=Window, SVGTag="switch", Feature=SVG] interface SVGSwitchElement : SVGGraphicsElement {};
[Exposed=Window, SVGTag="foreignObject", Feature=SVG] interface SVGForeignObjectElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
};
[Exposed=Window, SVGTag="clipPath", Feature=SVG] interface SVGClipPathElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedEnumeration clipPathUnits;
    [SameObject] readonly attribute SVGAnimatedTransformList transform;
};
[Exposed=Window, SVGTag="a", Feature=SVG] interface SVGAElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedString href;
    [SameObject] readonly attribute SVGAnimatedString target;
};

[Exposed=Window, SVGTag="use", Feature=SVG] interface SVGUseElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
    [SameObject] readonly attribute SVGAnimatedString href;
};

[Exposed=Window, SVGTag="image", Feature=SVG] interface SVGImageElement : SVGGraphicsElement {
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
    [SameObject] readonly attribute SVGAnimatedString href;
};

[Exposed=Window, Feature=SVG] interface SVGTextContentElement : SVGGraphicsElement {
    const unsigned short LENGTHADJUST_UNKNOWN = 0;
    const unsigned short LENGTHADJUST_SPACING = 1;
    const unsigned short LENGTHADJUST_SPACINGANDGLYPHS = 2;
    [SameObject] readonly attribute SVGAnimatedLength textLength;
    [SameObject] readonly attribute SVGAnimatedEnumeration lengthAdjust;
};
[Exposed=Window, Feature=SVG] interface SVGTextPositioningElement : SVGTextContentElement {
    [SameObject] readonly attribute SVGAnimatedLengthList x;
    [SameObject] readonly attribute SVGAnimatedLengthList y;
    [SameObject] readonly attribute SVGAnimatedLengthList dx;
    [SameObject] readonly attribute SVGAnimatedLengthList dy;
    [SameObject] readonly attribute SVGAnimatedNumberList rotate;
};
[Exposed=Window, SVGTag="text", Feature=SVG] interface SVGTextElement : SVGTextPositioningElement {};
[Exposed=Window, SVGTag="tspan", Feature=SVG] interface SVGTSpanElement : SVGTextPositioningElement {};
[Exposed=Window, SVGTag="textPath", Feature=SVG] interface SVGTextPathElement : SVGTextContentElement {
    const unsigned short TEXTPATH_METHODTYPE_UNKNOWN = 0;
    const unsigned short TEXTPATH_METHODTYPE_ALIGN = 1;
    const unsigned short TEXTPATH_METHODTYPE_STRETCH = 2;
    const unsigned short TEXTPATH_SPACINGTYPE_UNKNOWN = 0;
    const unsigned short TEXTPATH_SPACINGTYPE_AUTO = 1;
    const unsigned short TEXTPATH_SPACINGTYPE_EXACT = 2;
    const unsigned short TEXTPATH_SIDETYPE_UNKNOWN = 0;
    const unsigned short TEXTPATH_SIDETYPE_LEFT = 1;
    const unsigned short TEXTPATH_SIDETYPE_RIGHT = 2;
    [SameObject] readonly attribute SVGAnimatedString href;
    [SameObject] readonly attribute SVGAnimatedLength startOffset;
    [SameObject] readonly attribute SVGAnimatedEnumeration method;
    [SameObject] readonly attribute SVGAnimatedEnumeration spacing;
    [SameObject] readonly attribute SVGAnimatedEnumeration side;
};

[Exposed=Window, SVGTag="path", Feature=SVG] interface SVGPathElement : SVGGeometryElement {};
[Exposed=Window, SVGTag="rect", Feature=SVG] interface SVGRectElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
    [SameObject] readonly attribute SVGAnimatedLength rx;
    [SameObject] readonly attribute SVGAnimatedLength ry;
};
[Exposed=Window, SVGTag="circle", Feature=SVG] interface SVGCircleElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGAnimatedLength cx;
    [SameObject] readonly attribute SVGAnimatedLength cy;
    [SameObject] readonly attribute SVGAnimatedLength r;
};
[Exposed=Window, SVGTag="ellipse", Feature=SVG] interface SVGEllipseElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGAnimatedLength cx;
    [SameObject] readonly attribute SVGAnimatedLength cy;
    [SameObject] readonly attribute SVGAnimatedLength rx;
    [SameObject] readonly attribute SVGAnimatedLength ry;
};
[Exposed=Window, SVGTag="line", Feature=SVG] interface SVGLineElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGAnimatedLength x1;
    [SameObject] readonly attribute SVGAnimatedLength y1;
    [SameObject] readonly attribute SVGAnimatedLength x2;
    [SameObject] readonly attribute SVGAnimatedLength y2;
};
[Exposed=Window, SVGTag="polyline", Feature=SVG] interface SVGPolylineElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGPointList points;
    [SameObject] readonly attribute SVGPointList animatedPoints;
};
[Exposed=Window, SVGTag="polygon", Feature=SVG] interface SVGPolygonElement : SVGGeometryElement {
    [SameObject] readonly attribute SVGPointList points;
    [SameObject] readonly attribute SVGPointList animatedPoints;
};

[Exposed=Window, Feature=SVG] interface SVGGradientElement : SVGElement {
    const unsigned short SVG_SPREADMETHOD_UNKNOWN = 0;
    const unsigned short SVG_SPREADMETHOD_PAD = 1;
    const unsigned short SVG_SPREADMETHOD_REFLECT = 2;
    const unsigned short SVG_SPREADMETHOD_REPEAT = 3;
    [SameObject] readonly attribute SVGAnimatedEnumeration gradientUnits;
    [SameObject] readonly attribute SVGAnimatedTransformList gradientTransform;
    [SameObject] readonly attribute SVGAnimatedEnumeration spreadMethod;
    [SameObject] readonly attribute SVGAnimatedString href;
};
[Exposed=Window, SVGTag="linearGradient", Feature=SVG] interface SVGLinearGradientElement : SVGGradientElement {
    [SameObject] readonly attribute SVGAnimatedLength x1;
    [SameObject] readonly attribute SVGAnimatedLength y1;
    [SameObject] readonly attribute SVGAnimatedLength x2;
    [SameObject] readonly attribute SVGAnimatedLength y2;
};
[Exposed=Window, SVGTag="radialGradient", Feature=SVG] interface SVGRadialGradientElement : SVGGradientElement {
    [SameObject] readonly attribute SVGAnimatedLength cx;
    [SameObject] readonly attribute SVGAnimatedLength cy;
    [SameObject] readonly attribute SVGAnimatedLength r;
    [SameObject] readonly attribute SVGAnimatedLength fx;
    [SameObject] readonly attribute SVGAnimatedLength fy;
    [SameObject] readonly attribute SVGAnimatedLength fr;
};
[Exposed=Window, SVGTag="stop", Feature=SVG] interface SVGStopElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedNumber offset;
};
[Exposed=Window, SVGTag="pattern", Feature=SVG] interface SVGPatternElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedEnumeration patternUnits;
    [SameObject] readonly attribute SVGAnimatedEnumeration patternContentUnits;
    [SameObject] readonly attribute SVGAnimatedTransformList patternTransform;
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
    [SameObject] readonly attribute SVGAnimatedRect viewBox;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
    [SameObject] readonly attribute SVGAnimatedString href;
};
[Exposed=Window, SVGTag="mask", Feature=SVG] interface SVGMaskElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedEnumeration maskUnits;
    [SameObject] readonly attribute SVGAnimatedEnumeration maskContentUnits;
    [SameObject] readonly attribute SVGAnimatedLength x;
    [SameObject] readonly attribute SVGAnimatedLength y;
    [SameObject] readonly attribute SVGAnimatedLength width;
    [SameObject] readonly attribute SVGAnimatedLength height;
};
[Exposed=Window, SVGTag="marker", Feature=SVG] interface SVGMarkerElement : SVGElement {
    const unsigned short SVG_MARKERUNITS_UNKNOWN = 0;
    const unsigned short SVG_MARKERUNITS_USERSPACEONUSE = 1;
    const unsigned short SVG_MARKERUNITS_STROKEWIDTH = 2;
    const unsigned short SVG_MARKER_ORIENT_UNKNOWN = 0;
    const unsigned short SVG_MARKER_ORIENT_AUTO = 1;
    const unsigned short SVG_MARKER_ORIENT_ANGLE = 2;
    const unsigned short SVG_MARKER_ORIENT_AUTO_START_REVERSE = 3;
    [SameObject] readonly attribute SVGAnimatedLength refX;
    [SameObject] readonly attribute SVGAnimatedLength refY;
    [SameObject] readonly attribute SVGAnimatedEnumeration markerUnits;
    [SameObject] readonly attribute SVGAnimatedLength markerWidth;
    [SameObject] readonly attribute SVGAnimatedLength markerHeight;
    [SameObject] readonly attribute SVGAnimatedEnumeration orientType;
    [SameObject] readonly attribute SVGAnimatedAngle orientAngle;
    [SameObject] readonly attribute SVGAnimatedRect viewBox;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
    undefined setOrientToAuto();
    [Throws] undefined setOrientToAngle(SVGAngle angle);
};
[Exposed=Window, SVGTag="title", Feature=SVG] interface SVGTitleElement : SVGElement {};
[Exposed=Window, SVGTag="desc", Feature=SVG] interface SVGDescElement : SVGElement {};
[Exposed=Window, SVGTag="metadata", Feature=SVG] interface SVGMetadataElement : SVGElement {};
[Exposed=Window, SVGTag="view", Feature=SVG] interface SVGViewElement : SVGElement {
    [SameObject] readonly attribute SVGAnimatedRect viewBox;
    [SameObject] readonly attribute SVGAnimatedPreserveAspectRatio preserveAspectRatio;
};
[Exposed=Window, SVGTag="script", Feature=SVG] interface SVGScriptElement : SVGElement {
    [CEReactions, Reflect] attribute DOMString type;
};
[Exposed=Window, SVGTag="style", Feature=SVG] interface SVGStyleElement : SVGElement {
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute DOMString media;
};
