// EventSource (N1c, HTML Standard 9.2). The connection and the event stream parser are src/net/sse.c; the glue and the
// bridge to the network service are impl_eventsource.c. Its events are MessageEvent (messaging.webidl) and Event.

dictionary EventSourceInit {
    boolean withCredentials = false;
};

[Exposed=Window, BindObj]
interface EventSource : EventTarget {
    [Throws] constructor(USVString url, optional EventSourceInit eventSourceInitDict = {});
    readonly attribute USVString url;
    readonly attribute boolean withCredentials;
    const unsigned short CONNECTING = 0;
    const unsigned short OPEN = 1;
    const unsigned short CLOSED = 2;
    readonly attribute unsigned short readyState;
    undefined close();
};
