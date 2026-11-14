// iframes (IF1): every frame is its own realm, as a cross-origin frame is in a browser. Another frame's
// window is a RemoteWindow: postMessage, closed, window and self, nothing else. The implementation is impl_frames.c; the
// frames themselves are the embedder's (src/embed/frames.c).

[Exposed=Window, BindObj]
interface RemoteWindow {
    readonly attribute any window;
    readonly attribute any self;
    readonly attribute boolean closed;
    // postMessage(message, targetOrigin, transfer) or postMessage(message, { targetOrigin, transfer }).
    [Throws] undefined postMessage(any message, optional any targetOriginOrOptions, optional any transfer);
};

partial interface HTMLIFrameElement {
    readonly attribute any contentWindow;
    readonly attribute any contentDocument;
};

[Exposed=Window]
partial interface Window {
    [Replaceable] readonly attribute any frameElement;
};
