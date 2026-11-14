// Timers, microtasks, time and script state on the Window and Document (HTML 8.6 and 8.1.7, High Resolution Time, W5c).
// The handler is any value: a callable is called, a string is evaluated as a script when the
// timer fires; the result is the id as a number, or the exception of the handler's string conversion (the TimerHandler union of the spec is reduced to any). The event loop itself is the embedder's
// (src/embed/loop.c); the timer hooks are in bind.h.

[Exposed=Window]
partial interface Window {
    any setTimeout(any handler, optional long timeout = 0, any... arguments);
    undefined clearTimeout(optional long id = 0);
    any setInterval(any handler, optional long timeout = 0, any... arguments);
    undefined clearInterval(optional long id = 0);
    any queueMicrotask(any callback); // returns undefined; a TypeError for a callback that is not callable
    undefined reportError(any e);
    [Replaceable, SameObject] readonly attribute Performance performance;
};

// The marks and measures are plain objects with name, entryType, startTime and duration (the PerformanceEntry members).
[Exposed=Window, BindObj]
interface Performance : EventTarget {
    double now();
    readonly attribute double timeOrigin;
    any mark(DOMString markName);
    any measure(DOMString measureName, optional DOMString? startMark = null, optional DOMString? endMark = null);
    undefined clearMarks(optional DOMString? markName = null);
    undefined clearMeasures(optional DOMString? measureName = null);
};

[Exposed=Window]
partial interface Document {
    // The script element whose classic script is running, or null (an element of a shadow tree and module scripts: null).
    readonly attribute any currentScript;
};
