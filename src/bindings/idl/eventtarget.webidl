// EventTarget (W5a declares it; W5b implements the dispatch in impl_events.c and adds the signal option).
callback interface EventListener {
    undefined handleEvent(any event);
};

dictionary EventListenerOptions {
    boolean capture = false;
};

dictionary AddEventListenerOptions : EventListenerOptions {
    any passive;
    boolean once = false;
    AbortSignal signal;
};

[Exposed=Window, BindObj]
interface EventTarget {
    constructor();
    undefined addEventListener(DOMString type, EventListener? callback,
                               optional (AddEventListenerOptions or boolean) options = {});
    undefined removeEventListener(DOMString type, EventListener? callback,
                                  optional (EventListenerOptions or boolean) options = {});
    any dispatchEvent(any event); // a boolean: false when canceled (the InvalidStateError path needs a JS value)
};
