// Cross-document messaging in one document (HTML 9.3 and 9.4; W7f): MessageChannel, MessagePort,
// MessageEvent and Window.postMessage. The port core is src/loader/msgport.c, the glue impl_msgport.c. There are no other
// browsing contexts or workers, so every port belongs to the document's own realm; a message is a structured-clone byte
// string (src/loader/clone.c) that becomes one task on the posted-message task source per delivery.

dictionary MessageEventInit : EventInit {
    any data = null;
    USVString origin = "";
    DOMString lastEventId = "";
    any source = null;
    sequence<MessagePort> ports = [];
};

[Exposed=Window, BindObj]
interface MessageEvent : Event {
    constructor(DOMString type, optional MessageEventInit eventInitDict = {});
    readonly attribute any data;
    readonly attribute USVString origin;
    readonly attribute DOMString lastEventId;
    readonly attribute any source;
    readonly attribute any ports;
    undefined initMessageEvent(DOMString type, optional boolean bubbles = false, optional boolean cancelable = false,
                               optional any data = null, optional USVString origin = "", optional DOMString lastEventId = "",
                               optional any source = null, optional sequence<MessagePort> ports = []);
};

[Exposed=Window, BindObj]
interface MessageChannel {
    constructor();
    readonly attribute any port1;
    readonly attribute any port2;
};

// The second argument of postMessage is the transfer list (a sequence) or an options dictionary { transfer }; it is read by hand.
[Exposed=Window, BindObj]
interface MessagePort : EventTarget {
    [Throws] undefined postMessage(any message, optional any options);
    undefined start();
    undefined close();
    attribute any onmessage;
    attribute any onmessageerror;
};

// Event handler attributes of the Window (and, as for the other handlers, of every EventTarget).
partial interface EventTarget {
    attribute any onmessage;
    attribute any onmessageerror;
};

// postMessage(message, targetOrigin, transfer) or postMessage(message, { targetOrigin, transfer }).
[Exposed=Window]
partial interface Window {
    [Throws] undefined postMessage(any message, optional any targetOriginOrOptions, optional any transfer);
};
