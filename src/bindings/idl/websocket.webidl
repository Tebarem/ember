// WebSocket and CloseEvent (N1c, HTML Standard 9.3, RFC 6455). The protocol is src/net/websocket.c; the glue and the
// bridge to the network service are impl_websocket.c. The event handlers onopen and onclose are on EventTarget (below),
// like the other handlers; onerror and onmessage are declared there already.

dictionary CloseEventInit : EventInit {
    boolean wasClean = false;
    unsigned short code = 0;
    USVString reason = "";
};

[Exposed=Window, BindObj]
interface CloseEvent : Event {
    constructor(DOMString type, optional CloseEventInit eventInitDict = {});
    readonly attribute boolean wasClean;
    readonly attribute unsigned short code;
    readonly attribute USVString reason;
};

// protocols is a string or a sequence of strings; send takes a string, a Blob, an ArrayBuffer or an ArrayBufferView;
// close takes the optional status code (1000 or 3000-4999) and reason (at most 123 bytes of UTF-8). All are read by hand.
[Exposed=Window, BindObj]
interface WebSocket : EventTarget {
    [Throws] constructor(USVString url, optional any protocols);
    readonly attribute USVString url;
    const unsigned short CONNECTING = 0;
    const unsigned short OPEN = 1;
    const unsigned short CLOSING = 2;
    const unsigned short CLOSED = 3;
    readonly attribute unsigned short readyState;
    readonly attribute unsigned long long bufferedAmount;
    readonly attribute DOMString extensions;
    readonly attribute DOMString protocol;
    [Throws] undefined close(optional any code, optional any reason);
    attribute DOMString binaryType;
    [Throws] undefined send(any data);
};

partial interface EventTarget {
    attribute any onopen;
    attribute any onclose;
};
