// URL and URLSearchParams (WHATWG URL, W7c). The URL record is src/loader/url.c; a
// URLSearchParams is linked to its URL (the searchParams of the URL) or stands alone. Implementations: impl_loader_url.c.

[Exposed=Window, BindObj]
interface URL {
    constructor(USVString url, optional any base);
    static boolean canParse(USVString url, optional any base);
    static URL? parse(USVString url, optional any base);
    [Throws] static USVString createObjectURL(any obj);
    static undefined revokeObjectURL(USVString url);
    [Throws] attribute USVString href;
    readonly attribute USVString origin;
    attribute USVString protocol;
    attribute USVString username;
    attribute USVString password;
    attribute USVString host;
    attribute USVString hostname;
    attribute USVString port;
    attribute USVString pathname;
    attribute USVString search;
    [SameObject] readonly attribute URLSearchParams searchParams;
    attribute USVString hash;
    stringifier;
    USVString toJSON();
};

[Exposed=Window, BindObj]
interface URLSearchParams {
    constructor(optional any init);
    undefined append(USVString name, USVString value);
    undefined delete(USVString name, optional any value);
    USVString? get(USVString name);
    sequence<USVString> getAll(USVString name);
    boolean has(USVString name, optional any value);
    undefined set(USVString name, USVString value);
    undefined sort();
    stringifier;
    readonly attribute unsigned long size;
    iterable<USVString, USVString>;
};
