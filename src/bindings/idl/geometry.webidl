// Geometry interfaces (SZ3b: split from svg.webidl): DOMPoint(ReadOnly) and DOMMatrix(ReadOnly), with the dictionaries
// they take. They are always built: canvas, SVG and scripts of any page use them. The SVG value types that derive from
// them (SVGPoint, SVGMatrix, SVGRect) are in svg.webidl. Implementation: impl_svg_val.c (the payload is shared with them).

dictionary DOMPointInit {
    unrestricted double x = 0;
    unrestricted double y = 0;
    unrestricted double z = 0;
    unrestricted double w = 1;
};

// KNOWN LIMITATION: this 2D-only init (no m13..m44, is2D) is also used by DOMMatrix.multiply/multiplySelf/preMultiplySelf and
// DOMPoint.matrixTransform, so the 3D part of a 3D DOMMatrix argument is dropped there.
// Absent members are NaN (the dictionary has no presence flags); an alias and its element must agree.
dictionary DOMMatrix2DInit {
    unrestricted double a = NaN;
    unrestricted double b = NaN;
    unrestricted double c = NaN;
    unrestricted double d = NaN;
    unrestricted double e = NaN;
    unrestricted double f = NaN;
    unrestricted double m11 = NaN;
    unrestricted double m12 = NaN;
    unrestricted double m21 = NaN;
    unrestricted double m22 = NaN;
    unrestricted double m41 = NaN;
    unrestricted double m42 = NaN;
};

// ------------------------------------------------------------------ geometry values

[Exposed=Window, BindObj]
interface DOMPointReadOnly {
    constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
                optional unrestricted double z = 0, optional unrestricted double w = 1);
    static DOMPointReadOnly fromPoint(optional DOMPointInit other = {});
    readonly attribute unrestricted double x;
    readonly attribute unrestricted double y;
    readonly attribute unrestricted double z;
    readonly attribute unrestricted double w;
    DOMPoint matrixTransform(optional DOMMatrix2DInit matrix = {});
    object toJSON();
};

[Exposed=Window, BindObj]
interface DOMPoint : DOMPointReadOnly {
    constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
                optional unrestricted double z = 0, optional unrestricted double w = 1);
    static DOMPoint fromPoint(optional DOMPointInit other = {});
    [Throws] attribute unrestricted double x;
    [Throws] attribute unrestricted double y;
    [Throws] attribute unrestricted double z;
    [Throws] attribute unrestricted double w;
};

[Exposed=Window, BindObj]
interface DOMMatrixReadOnly {
    constructor(optional any init);
    readonly attribute unrestricted double a;
    readonly attribute unrestricted double b;
    readonly attribute unrestricted double c;
    readonly attribute unrestricted double d;
    readonly attribute unrestricted double e;
    readonly attribute unrestricted double f;
    readonly attribute unrestricted double m11;
    readonly attribute unrestricted double m12;
    readonly attribute unrestricted double m13;
    readonly attribute unrestricted double m14;
    readonly attribute unrestricted double m21;
    readonly attribute unrestricted double m22;
    readonly attribute unrestricted double m23;
    readonly attribute unrestricted double m24;
    readonly attribute unrestricted double m31;
    readonly attribute unrestricted double m32;
    readonly attribute unrestricted double m33;
    readonly attribute unrestricted double m34;
    readonly attribute unrestricted double m41;
    readonly attribute unrestricted double m42;
    readonly attribute unrestricted double m43;
    readonly attribute unrestricted double m44;
    readonly attribute boolean is2D;
    readonly attribute boolean isIdentity;
    DOMMatrix translate(optional unrestricted double tx = 0, optional unrestricted double ty = 0,
                        optional unrestricted double tz = 0);
    DOMMatrix scale(optional unrestricted double scaleX = 1, optional unrestricted double scaleY = NaN,
                    optional unrestricted double scaleZ = 1, optional unrestricted double originX = 0,
                    optional unrestricted double originY = 0, optional unrestricted double originZ = 0);
    DOMMatrix scaleNonUniform(optional unrestricted double scaleX = 1, optional unrestricted double scaleY = 1);
    DOMMatrix rotate(optional unrestricted double rotX = 0, optional unrestricted double rotY = NaN,
                     optional unrestricted double rotZ = NaN);
    DOMMatrix rotateFromVector(optional unrestricted double x = 0, optional unrestricted double y = 0);
    DOMMatrix skewX(optional unrestricted double sx = 0);
    DOMMatrix skewY(optional unrestricted double sy = 0);
    DOMMatrix multiply(optional DOMMatrix2DInit other = {});
    DOMMatrix flipX();
    DOMMatrix flipY();
    DOMMatrix inverse();
    DOMPoint transformPoint(optional DOMPointInit point = {});
    object toJSON();
};

[Exposed=Window, BindObj]
interface DOMMatrix : DOMMatrixReadOnly {
    constructor(optional any init);
    [Throws] attribute unrestricted double a;
    [Throws] attribute unrestricted double b;
    [Throws] attribute unrestricted double c;
    [Throws] attribute unrestricted double d;
    [Throws] attribute unrestricted double e;
    [Throws] attribute unrestricted double f;
    [Throws] attribute unrestricted double m11;
    [Throws] attribute unrestricted double m12;
    [Throws] attribute unrestricted double m13;
    [Throws] attribute unrestricted double m14;
    [Throws] attribute unrestricted double m21;
    [Throws] attribute unrestricted double m22;
    [Throws] attribute unrestricted double m23;
    [Throws] attribute unrestricted double m24;
    [Throws] attribute unrestricted double m31;
    [Throws] attribute unrestricted double m32;
    [Throws] attribute unrestricted double m33;
    [Throws] attribute unrestricted double m34;
    [Throws] attribute unrestricted double m41;
    [Throws] attribute unrestricted double m42;
    [Throws] attribute unrestricted double m43;
    [Throws] attribute unrestricted double m44;
    [Throws] DOMMatrix multiplySelf(optional DOMMatrix2DInit other = {});
    [Throws] DOMMatrix preMultiplySelf(optional DOMMatrix2DInit other = {});
    [Throws] DOMMatrix translateSelf(optional unrestricted double tx = 0, optional unrestricted double ty = 0,
                            optional unrestricted double tz = 0);
    [Throws] DOMMatrix scaleSelf(optional unrestricted double scaleX = 1, optional unrestricted double scaleY = NaN,
                        optional unrestricted double scaleZ = 1, optional unrestricted double originX = 0,
                        optional unrestricted double originY = 0, optional unrestricted double originZ = 0);
    [Throws] DOMMatrix rotateSelf(optional unrestricted double rotX = 0, optional unrestricted double rotY = NaN,
                         optional unrestricted double rotZ = NaN);
    [Throws] DOMMatrix rotateFromVectorSelf(optional unrestricted double x = 0, optional unrestricted double y = 0);
    [Throws] DOMMatrix skewXSelf(optional unrestricted double sx = 0);
    [Throws] DOMMatrix skewYSelf(optional unrestricted double sy = 0);
    [Throws] DOMMatrix invertSelf();
};

// SZ3b: the dictionary of getBBox() lives here, not in svg.webidl: shared native code converts it (a converter is built whether or not
// the interface that takes it is).
dictionary SVGBoundingBoxOptions {
    boolean fill = true;
    boolean stroke = false;
    boolean markers = false;
    boolean clipped = false;
};

