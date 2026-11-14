// TextEncoder, TextDecoder and atob/btoa (W7c, Encoding and HTML standards). The decoder
// is src/loader/encoding.c. Implementations: impl_loader_encoding.c. encode returns a Uint8Array and decode takes any
// BufferSource (idlgen has no BufferSource type: the implementation reads the bytes of a typed array, a DataView or an
// ArrayBuffer itself).

dictionary TextDecoderOptions {
    boolean fatal = false;
    boolean ignoreBOM = false;
};

dictionary TextDecodeOptions {
    boolean stream = false;
};

[Exposed=Window, BindObj]
interface TextEncoder {
    constructor();
    readonly attribute DOMString encoding;
    any encode(optional USVString input = "");
    any encodeInto(USVString source, any dest);
};

[Exposed=Window, BindObj]
interface TextDecoder {
    constructor(optional DOMString label = "utf-8", optional TextDecoderOptions options = {});
    readonly attribute DOMString encoding;
    readonly attribute boolean fatal;
    readonly attribute boolean ignoreBOM;
    [Throws] DOMString decode(optional any input, optional TextDecodeOptions options = {});
};

[Exposed=Window]
partial interface Window {
    [Throws] DOMString atob(DOMString data);
    [Throws] DOMString btoa(DOMString data);
};
