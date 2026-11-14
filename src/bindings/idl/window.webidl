dictionary ScrollToOptions {
    unrestricted double left = NaN;
    unrestricted double top = NaN;
    DOMString behavior = "auto";
};

[Exposed=Window, BindObj, Global]
interface Window : EventTarget {
    [LegacyUnforgeable] readonly attribute Window window;
    [Replaceable] readonly attribute Window self;
    [Replaceable] readonly attribute any event;
    [LegacyUnforgeable] readonly attribute Document document;
    [LegacyUnforgeable, SameObject, PutForwards=href] readonly attribute Location location;
    [SameObject] readonly attribute History history;
    [SameObject] readonly attribute Screen screen;
    [SameObject] readonly attribute Navigator navigator;
    [Replaceable] readonly attribute Window frames;
    [Replaceable] readonly attribute Window top;
    [Replaceable] readonly attribute Window parent;
    [Replaceable] readonly attribute long length;
    readonly attribute boolean closed;
    attribute DOMString name;

    [Replaceable] readonly attribute long innerWidth;
    [Replaceable] readonly attribute long innerHeight;
    [Replaceable] readonly attribute long outerWidth;
    [Replaceable] readonly attribute long outerHeight;
    [Replaceable] readonly attribute double devicePixelRatio;
    [Replaceable] readonly attribute double scrollX;
    [Replaceable] readonly attribute double scrollY;
    [Replaceable] readonly attribute double pageXOffset;
    [Replaceable] readonly attribute double pageYOffset;
    [Replaceable] readonly attribute long screenX;
    [Replaceable] readonly attribute long screenY;

    undefined scroll(optional ScrollToOptions options = {});
    undefined scroll(unrestricted double x, unrestricted double y);
    undefined scrollTo(optional ScrollToOptions options = {});
    undefined scrollTo(unrestricted double x, unrestricted double y);
    undefined scrollBy(optional ScrollToOptions options = {});
    undefined scrollBy(unrestricted double x, unrestricted double y);

    CSSStyleDeclaration getComputedStyle(Element elt, optional DOMString? pseudoElt = null);
    MediaQueryList matchMedia(DOMString query);
    undefined focus();
    undefined blur();
    undefined close();
    undefined stop();
};

[Exposed=Window, BindObj]
interface Location {
    [LegacyUnforgeable] stringifier attribute USVString href;
    [LegacyUnforgeable] readonly attribute USVString origin;
    [LegacyUnforgeable] attribute USVString protocol;
    [LegacyUnforgeable] attribute USVString host;
    [LegacyUnforgeable] attribute USVString hostname;
    [LegacyUnforgeable] attribute USVString port;
    [LegacyUnforgeable] attribute USVString pathname;
    [LegacyUnforgeable] attribute USVString search;
    [LegacyUnforgeable] attribute USVString hash;
    [LegacyUnforgeable] undefined assign(USVString url);
    [LegacyUnforgeable] undefined replace(USVString url);
    [LegacyUnforgeable] undefined reload();
};

enum ScrollRestoration { "auto", "manual" };

[Exposed=Window, BindObj]
interface History {
    readonly attribute unsigned long length;
    attribute ScrollRestoration scrollRestoration;
    readonly attribute any state;
    undefined go(optional long delta = 0);
    undefined back();
    undefined forward();
    [Throws] undefined pushState(any data, DOMString unused, optional USVString? url = null);
    [Throws] undefined replaceState(any data, DOMString unused, optional USVString? url = null);
};

[Exposed=Window, BindObj]
interface Screen {
    readonly attribute long availWidth;
    readonly attribute long availHeight;
    readonly attribute long width;
    readonly attribute long height;
    readonly attribute unsigned long colorDepth;
    readonly attribute unsigned long pixelDepth;
};

[Exposed=Window, BindObj]
interface Navigator {
    readonly attribute DOMString userAgent;
    readonly attribute DOMString appCodeName;
    readonly attribute DOMString appName;
    readonly attribute DOMString appVersion;
    readonly attribute DOMString platform;
    readonly attribute DOMString product;
    readonly attribute DOMString vendor;
    readonly attribute DOMString language;
    readonly attribute boolean onLine;
    readonly attribute boolean cookieEnabled;
    readonly attribute unsigned long long hardwareConcurrency;
    readonly attribute boolean webdriver;
};

[Exposed=Window, BindObj]
interface MediaQueryList : EventTarget {
    readonly attribute DOMString media;
    readonly attribute boolean matches;
    undefined addListener(EventListener? callback);
    undefined removeListener(EventListener? callback);
};

dictionary DOMRectInit {
    unrestricted double x = 0;
    unrestricted double y = 0;
    unrestricted double width = 0;
    unrestricted double height = 0;
};

[Exposed=Window, BindObj]
interface DOMRectReadOnly {
    constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
                optional unrestricted double width = 0, optional unrestricted double height = 0);
    readonly attribute unrestricted double x;
    readonly attribute unrestricted double y;
    readonly attribute unrestricted double width;
    readonly attribute unrestricted double height;
    readonly attribute unrestricted double top;
    readonly attribute unrestricted double right;
    readonly attribute unrestricted double bottom;
    readonly attribute unrestricted double left;
    object toJSON();
};

[Exposed=Window, BindObj]
interface DOMRect : DOMRectReadOnly {
    constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
                optional unrestricted double width = 0, optional unrestricted double height = 0);
    [Throws] attribute unrestricted double x;
    [Throws] attribute unrestricted double y;
    [Throws] attribute unrestricted double width;
    [Throws] attribute unrestricted double height;
};

[Exposed=Window, BindObj]
interface DOMRectList {
    readonly attribute unsigned long length;
    getter DOMRect? item(unsigned long index);
    iterable<DOMRect>;
};
