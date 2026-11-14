// Events, AbortController and AbortSignal (W5b).
// The core record (type, phase, flags, path) lives in src/dom/event.h. The interface payload
// (detail, clientX, key, ...) lives in a plain object kept by the wrapper (impl_events.c).

dictionary EventInit {
    boolean bubbles = false;
    boolean cancelable = false;
    boolean composed = false;
};

[Exposed=Window, BindObj]
interface Event {
    constructor(DOMString type, optional EventInit eventInitDict = {});
    readonly attribute DOMString type;
    readonly attribute EventTarget? target;
    readonly attribute EventTarget? srcElement;
    readonly attribute EventTarget? currentTarget;
    sequence<object> composedPath();
    const unsigned short NONE = 0;
    const unsigned short CAPTURING_PHASE = 1;
    const unsigned short AT_TARGET = 2;
    const unsigned short BUBBLING_PHASE = 3;
    readonly attribute unsigned short eventPhase;
    undefined stopPropagation();
    attribute boolean cancelBubble;
    undefined stopImmediatePropagation();
    readonly attribute boolean bubbles;
    readonly attribute boolean cancelable;
    attribute boolean returnValue;
    undefined preventDefault();
    readonly attribute boolean defaultPrevented;
    readonly attribute boolean composed;
    readonly attribute boolean isTrusted;
    readonly attribute double timeStamp;
    undefined initEvent(DOMString type, optional boolean bubbles = false,
                        optional boolean cancelable = false);
};

dictionary CustomEventInit : EventInit {
    any detail = null;
};

[Exposed=Window, BindObj]
interface CustomEvent : Event {
    constructor(DOMString type, optional CustomEventInit eventInitDict = {});
    readonly attribute any detail;
    undefined initCustomEvent(DOMString type, optional boolean bubbles = false,
                              optional boolean cancelable = false, optional any detail = null);
};

dictionary UIEventInit : EventInit {
    object? view = null;
    long detail = 0;
    unsigned long which = 0;
};

[Exposed=Window, BindObj]
interface UIEvent : Event {
    constructor(DOMString type, optional UIEventInit eventInitDict = {});
    readonly attribute object? view;
    readonly attribute long detail;
    readonly attribute unsigned long which;
    readonly attribute EventTarget? pseudoTarget;
    undefined initUIEvent(DOMString typeArg, optional boolean bubblesArg = false,
                          optional boolean cancelableArg = false, optional object? viewArg = null,
                          optional long detailArg = 0);
};

dictionary EventModifierInit : UIEventInit {
    boolean ctrlKey = false;
    boolean shiftKey = false;
    boolean altKey = false;
    boolean metaKey = false;
    boolean modifierAltGraph = false;
    boolean modifierCapsLock = false;
    boolean modifierFn = false;
    boolean modifierFnLock = false;
    boolean modifierHyper = false;
    boolean modifierNumLock = false;
    boolean modifierScrollLock = false;
    boolean modifierSuper = false;
    boolean modifierSymbol = false;
    boolean modifierSymbolLock = false;
};

dictionary MouseEventInit : EventModifierInit {
    long screenX = 0;
    long screenY = 0;
    long clientX = 0;
    long clientY = 0;
    short button = 0;
    unsigned short buttons = 0;
    object? relatedTarget = null;
};

[Exposed=Window, BindObj]
interface MouseEvent : UIEvent {
    constructor(DOMString type, optional MouseEventInit eventInitDict = {});
    readonly attribute long screenX;
    readonly attribute long screenY;
    readonly attribute long clientX;
    readonly attribute long clientY;
    readonly attribute double pageX;
    readonly attribute double pageY;
    readonly attribute double offsetX;
    readonly attribute double offsetY;
    readonly attribute double x;
    readonly attribute double y;
    readonly attribute boolean ctrlKey;
    readonly attribute boolean shiftKey;
    readonly attribute boolean altKey;
    readonly attribute boolean metaKey;
    readonly attribute short button;
    readonly attribute unsigned short buttons;
    readonly attribute EventTarget? relatedTarget;
    boolean getModifierState(DOMString keyArg);
    undefined initMouseEvent(DOMString typeArg, optional boolean bubblesArg = false,
                             optional boolean cancelableArg = false, optional object? viewArg = null,
                             optional long detailArg = 0, optional long screenXArg = 0,
                             optional long screenYArg = 0, optional long clientXArg = 0,
                             optional long clientYArg = 0, optional boolean ctrlKeyArg = false,
                             optional boolean altKeyArg = false, optional boolean shiftKeyArg = false,
                             optional boolean metaKeyArg = false, optional short buttonArg = 0,
                             optional object? relatedTargetArg = null);
};

dictionary PointerEventInit : MouseEventInit {
    long pointerId = 0;
    double width = 1;
    double height = 1;
    float pressure = 0;
    float tangentialPressure = 0;
    long tiltX = 0;
    long tiltY = 0;
    long twist = 0;
    DOMString pointerType = "";
    boolean isPrimary = false;
};

[Exposed=Window, BindObj]
interface PointerEvent : MouseEvent {
    constructor(DOMString type, optional PointerEventInit eventInitDict = {});
    readonly attribute long pointerId;
    readonly attribute double width;
    readonly attribute double height;
    readonly attribute float pressure;
    readonly attribute float tangentialPressure;
    readonly attribute long tiltX;
    readonly attribute long tiltY;
    readonly attribute long twist;
    readonly attribute DOMString pointerType;
    readonly attribute boolean isPrimary;
};

dictionary WheelEventInit : MouseEventInit {
    double deltaX = 0;
    double deltaY = 0;
    double deltaZ = 0;
    unsigned long deltaMode = 0;
};

[Exposed=Window, BindObj]
interface WheelEvent : MouseEvent {
    constructor(DOMString type, optional WheelEventInit eventInitDict = {});
    const unsigned long DOM_DELTA_PIXEL = 0;
    const unsigned long DOM_DELTA_LINE = 1;
    const unsigned long DOM_DELTA_PAGE = 2;
    readonly attribute double deltaX;
    readonly attribute double deltaY;
    readonly attribute double deltaZ;
    readonly attribute unsigned long deltaMode;
};

dictionary KeyboardEventInit : EventModifierInit {
    DOMString key = "";
    DOMString code = "";
    unsigned long location = 0;
    boolean repeat = false;
    boolean isComposing = false;
    unsigned long charCode = 0;
    unsigned long keyCode = 0;
};

[Exposed=Window, BindObj]
interface KeyboardEvent : UIEvent {
    constructor(DOMString type, optional KeyboardEventInit eventInitDict = {});
    const unsigned long DOM_KEY_LOCATION_STANDARD = 0;
    const unsigned long DOM_KEY_LOCATION_LEFT = 1;
    const unsigned long DOM_KEY_LOCATION_RIGHT = 2;
    const unsigned long DOM_KEY_LOCATION_NUMPAD = 3;
    readonly attribute DOMString key;
    readonly attribute DOMString code;
    readonly attribute unsigned long location;
    readonly attribute boolean ctrlKey;
    readonly attribute boolean shiftKey;
    readonly attribute boolean altKey;
    readonly attribute boolean metaKey;
    readonly attribute boolean repeat;
    readonly attribute boolean isComposing;
    readonly attribute unsigned long charCode;
    readonly attribute unsigned long keyCode;
    boolean getModifierState(DOMString keyArg);
    undefined initKeyboardEvent(DOMString typeArg, optional boolean bubblesArg = false,
                                optional boolean cancelableArg = false, optional object? viewArg = null,
                                optional DOMString keyArg = "", optional unsigned long locationArg = 0,
                                optional boolean ctrlKey = false, optional boolean altKey = false,
                                optional boolean shiftKey = false, optional boolean metaKey = false);
};

dictionary DragEventInit : MouseEventInit {
    object? dataTransfer = null;
};

[Exposed=Window, BindObj]
interface DragEvent : MouseEvent {
    constructor(DOMString type, optional DragEventInit eventInitDict = {});
    readonly attribute DataTransfer? dataTransfer;
};

[Exposed=Window, BindObj]
interface TextEvent : UIEvent {
    readonly attribute DOMString data;
    undefined initTextEvent(DOMString type, optional boolean bubbles = false, optional boolean cancelable = false,
                            optional object? view = null, optional DOMString data = "undefined");
};

dictionary TouchEventInit : EventModifierInit {
    any touches;
    any targetTouches;
    any changedTouches;
};

[Exposed=Window, BindObj]
interface TouchEvent : UIEvent {
    constructor(DOMString type, optional TouchEventInit eventInitDict = {});
    readonly attribute any touches;
    readonly attribute any targetTouches;
    readonly attribute any changedTouches;
    readonly attribute boolean ctrlKey;
    readonly attribute boolean shiftKey;
    readonly attribute boolean altKey;
    readonly attribute boolean metaKey;
};

dictionary FocusEventInit : UIEventInit {
    object? relatedTarget = null;
};

[Exposed=Window, BindObj]
interface FocusEvent : UIEvent {
    constructor(DOMString type, optional FocusEventInit eventInitDict = {});
    readonly attribute EventTarget? relatedTarget;
};

dictionary InputEventInit : UIEventInit {
    DOMString? data = null;
    boolean isComposing = false;
    DOMString inputType = "";
};

[Exposed=Window, BindObj]
interface InputEvent : UIEvent {
    constructor(DOMString type, optional InputEventInit eventInitDict = {});
    readonly attribute DOMString? data;
    readonly attribute boolean isComposing;
    readonly attribute DOMString inputType;
};

dictionary CompositionEventInit : UIEventInit {
    DOMString data = "";
};

[Exposed=Window, BindObj]
interface CompositionEvent : UIEvent {
    constructor(DOMString type, optional CompositionEventInit eventInitDict = {});
    readonly attribute DOMString data;
};

[Exposed=Window, BindObj]
interface DataTransfer {
    constructor();
    attribute DOMString dropEffect;
    DOMString getData(DOMString format);
    undefined setData(DOMString format, DOMString data);
    undefined clearData(optional DOMString format);
};

dictionary ClipboardEventInit : EventInit {
    object? clipboardData = null;
};

[Exposed=Window, BindObj]
interface ClipboardEvent : Event {
    constructor(DOMString type, optional ClipboardEventInit eventInitDict = {});
    readonly attribute DataTransfer? clipboardData;
};

dictionary SubmitEventInit : EventInit {
    object? submitter = null;
};

[Exposed=Window, BindObj]
interface SubmitEvent : Event {
    constructor(DOMString type, optional SubmitEventInit eventInitDict = {});
    readonly attribute EventTarget? submitter;
};

dictionary ErrorEventInit : EventInit {
    DOMString message = "";
    DOMString filename = "";
    unsigned long lineno = 0;
    unsigned long colno = 0;
    any error;
};

[Exposed=Window, BindObj]
interface ErrorEvent : Event {
    constructor(DOMString type, optional ErrorEventInit eventInitDict = {});
    readonly attribute DOMString message;
    readonly attribute DOMString filename;
    readonly attribute unsigned long lineno;
    readonly attribute unsigned long colno;
    readonly attribute any error;
};

dictionary PromiseRejectionEventInit : EventInit {
    required object promise;
    any reason;
};

[Exposed=Window, BindObj]
interface PromiseRejectionEvent : Event {
    constructor(DOMString type, PromiseRejectionEventInit eventInitDict);
    readonly attribute object promise;
    readonly attribute any reason;
};

dictionary PopStateEventInit : EventInit {
    any state = null;
};

[Exposed=Window, BindObj]
interface PopStateEvent : Event {
    constructor(DOMString type, optional PopStateEventInit eventInitDict = {});
    readonly attribute any state;
};

dictionary MediaQueryListEventInit : EventInit {
    DOMString media = "";
    boolean matches = false;
};

[Exposed=Window, BindObj]
interface MediaQueryListEvent : Event {
    constructor(DOMString type, optional MediaQueryListEventInit eventInitDict = {});
    readonly attribute DOMString media;
    readonly attribute boolean matches;
};

dictionary HashChangeEventInit : EventInit {
    USVString oldURL = "";
    USVString newURL = "";
};

[Exposed=Window, BindObj]
interface HashChangeEvent : Event {
    constructor(DOMString type, optional HashChangeEventInit eventInitDict = {});
    readonly attribute USVString oldURL;
    readonly attribute USVString newURL;
};

[Exposed=Window, BindObj]
interface AbortSignal : EventTarget {
    [NewObject] static AbortSignal abort(optional any reason);
    [NewObject] static AbortSignal timeout(unsigned long milliseconds);
    [NewObject] static AbortSignal any(sequence<AbortSignal> signals);
    readonly attribute boolean aborted;
    readonly attribute any reason;
    [Throws] undefined throwIfAborted();
};

[Exposed=Window, BindObj]
interface AbortController {
    constructor();
    [SameObject] readonly attribute AbortSignal signal;
    undefined abort(optional any reason);
};

// Event handler IDL attributes. A handler is stored in the target's listener list (flag handler)
// in the position of its first registration. A content attribute (onclick="...") is compiled when
// the handler is read or dispatched, and again when the attribute value changes.
partial interface EventTarget {
    attribute any onabort;
    attribute any onauxclick;
    attribute any onbeforeinput;
    attribute any onblur;
    attribute any onchange;
    attribute any onclick;
    attribute any oncompositionend;
    attribute any oncompositionstart;
    attribute any oncompositionupdate;
    attribute any oncontextmenu;
    attribute any oncopy;
    attribute any oncut;
    attribute any ondblclick;
    attribute any onerror;
    attribute any onfocus;
    attribute any onfocusin;
    attribute any onfocusout;
    attribute any onhashchange;
    attribute any oninput;
    attribute any onkeydown;
    attribute any onkeypress;
    attribute any onkeyup;
    attribute any onload;
    attribute any onmousedown;
    attribute any onmouseenter;
    attribute any onmouseleave;
    attribute any onmousemove;
    attribute any onmouseout;
    attribute any onmouseover;
    attribute any onmouseup;
    attribute any onpaste;
    attribute any onpointercancel;
    attribute any onpointerdown;
    attribute any onpointerenter;
    attribute any onpointerleave;
    attribute any onpointermove;
    attribute any onpointerout;
    attribute any onpointerover;
    attribute any onpointerup;
    attribute any onpopstate;
    attribute any onreset;
    attribute any onresize;
    attribute any onscroll;
    attribute any onsubmit;
    attribute any onunhandledrejection;
    attribute any onwheel;
};
