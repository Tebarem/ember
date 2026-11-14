// Input additions (W5d1): navigator.clipboard and pointer capture. The events themselves (PointerEvent, WheelEvent,
// KeyboardEvent, ClipboardEvent, ...) are in events.webidl.

// The async clipboard API, text only. The promises settle at once: the clipboard is SDL's (or the process-local buffer of
// a headless embedder), and permission is always granted in an embedder context.
[Exposed=Window, BindObj]
interface Clipboard {
    Promise<any> readText();
    Promise<any> writeText(DOMString data);
};

[Exposed=Window]
partial interface Navigator {
    [SameObject] readonly attribute Clipboard clipboard;
};

// The movement of the pointer since the previous move event, in CSS px (set on the events the input layer dispatches; 0 on
// events made by script).
[Exposed=Window]
partial interface MouseEvent {
    readonly attribute double movementX;
    readonly attribute double movementY;
};

// Pointer capture: while an element has the capture of a pointer, the pointer's events are dispatched at it and no
// over/out/enter/leave events are sent for the elements under the pointer.
[Exposed=Window]
partial interface Element {
    [Throws] undefined setPointerCapture(long pointerId);
    [Throws] undefined releasePointerCapture(long pointerId);
    boolean hasPointerCapture(long pointerId);
};
