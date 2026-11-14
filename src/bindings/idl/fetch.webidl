// Fetch, Blob, File, FormData and ReadableStream (Fetch Standard, File API, XHR FormData, Streams).
// The C core is src/loader/{fetch,blob,stream}.c; the JS side is impl_loader_{blob,headers,
// body,fetch,stream,form}.c. Init dictionaries are read by hand from `any` arguments (idlgen's dictionaries do not tell an
// absent member from a default). BufferSource arguments are `any` too: the implementations read the bytes themselves.

// ------------------------------------------------------------------ Blob and File

[Exposed=Window, BindObj]
interface Blob {
    constructor(optional any blobParts, optional any options);
    readonly attribute unsigned long long size;
    readonly attribute DOMString type;
    [Throws] Blob slice(optional any start, optional any end, optional any contentType);
    Promise<any> text();
    Promise<any> arrayBuffer();
    Promise<any> bytes();
    [Throws] any stream();
};

[Exposed=Window, BindObj]
interface File : Blob {
    constructor(any fileBits, USVString fileName, optional any options);
    readonly attribute DOMString name;
    readonly attribute long long lastModified;
};

// ------------------------------------------------------------------ FormData

[Exposed=Window, BindObj]
interface FormData {
    constructor(optional any form);
    [Throws] undefined append(USVString name, any value, optional any filename);
    undefined delete(USVString name);
    any get(USVString name);
    sequence<any> getAll(USVString name);
    boolean has(USVString name);
    [Throws] undefined set(USVString name, any value, optional any filename);
    iterable<USVString, any>;
};

// ------------------------------------------------------------------ Headers

[Exposed=Window, BindObj]
interface Headers {
    constructor(optional any init);
    [Throws] undefined append(ByteString name, ByteString value);
    [Throws] undefined delete(ByteString name);
    [Throws] any get(ByteString name);
    sequence<ByteString> getSetCookie();
    [Throws] boolean has(ByteString name);
    [Throws] undefined set(ByteString name, ByteString value);
    iterable<ByteString, ByteString>;
};

// ------------------------------------------------------------------ Request and Response

[Exposed=Window, BindObj]
interface Request {
    constructor(any input, optional any init);
    readonly attribute ByteString method;
    readonly attribute USVString url;
    [SameObject] readonly attribute Headers headers;
    readonly attribute DOMString destination;
    readonly attribute USVString referrer;
    readonly attribute DOMString referrerPolicy;
    readonly attribute DOMString mode;
    readonly attribute DOMString credentials;
    readonly attribute DOMString cache;
    readonly attribute DOMString redirect;
    readonly attribute DOMString integrity;
    readonly attribute boolean keepalive;
    readonly attribute any signal;
    readonly attribute DOMString duplex;
    [Throws] Request clone();
    readonly attribute any body;
    readonly attribute boolean bodyUsed;
    Promise<any> arrayBuffer();
    Promise<any> blob();
    Promise<any> bytes();
    Promise<any> formData();
    Promise<any> json();
    Promise<any> text();
};

[Exposed=Window, BindObj]
interface Response {
    constructor(optional any body, optional any init);
    static Response error();
    [Throws] static Response redirect(USVString url, optional unsigned short status = 302);
    [Throws] static Response json(any data, optional any init);
    readonly attribute DOMString type;
    readonly attribute USVString url;
    readonly attribute boolean redirected;
    readonly attribute unsigned short status;
    readonly attribute boolean ok;
    readonly attribute ByteString statusText;
    [SameObject] readonly attribute Headers headers;
    [Throws] Response clone();
    readonly attribute any body;
    readonly attribute boolean bodyUsed;
    Promise<any> arrayBuffer();
    Promise<any> blob();
    Promise<any> bytes();
    Promise<any> formData();
    Promise<any> json();
    Promise<any> text();
};

[Exposed=Window]
partial interface Window {
    Promise<any> fetch(any input, optional any init);
};

// ------------------------------------------------------------------ ReadableStream

[Exposed=Window, BindObj]
interface ReadableStream {
    [Throws] constructor(optional any underlyingSource, optional any strategy);
    readonly attribute boolean locked;
    Promise<any> cancel(optional any reason);
    [Throws] any getReader(optional any options);
    [Throws] any values(optional any options);
    Promise<any> pipeTo(any destination, optional any options);
    [Throws] any pipeThrough(any transform, optional any options);
};

[Exposed=Window, BindObj]
interface ReadableStreamDefaultReader {
    [Throws] constructor(any stream);
    Promise<any> read();
    [Throws] undefined releaseLock();
    Promise<any> cancel(optional any reason);
    readonly attribute any closed;
};

[Exposed=Window, BindObj]
interface ReadableStreamDefaultController {
    readonly attribute any desiredSize;
    [Throws] undefined close();
    [Throws] undefined enqueue(optional any chunk);
    undefined error(optional any e);
};

[Exposed=Window, BindObj]
interface ReadableStreamAsyncIterator {
    Promise<any> next();
    Promise<any> return(optional any value);
};
