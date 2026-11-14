// structuredClone (HTML 2.7.4) and Web Storage (HTML 12.2; W7c). The serializer is
// src/loader/clone.c, the storage backend src/loader/storage.c. Implementations: impl_loader_clone.c (structuredClone) and
// impl_loader_storage.c (Storage, StorageEvent).

dictionary StructuredSerializeOptions {
    sequence<object> transfer = [];
};

[Exposed=Window]
partial interface Window {
    [Throws] any structuredClone(any value, optional StructuredSerializeOptions options = {});
};

// Storage has named properties: localStorage.foo reads the item "foo", and an assignment writes it.
[Exposed=Window, BindObj, CustomNamed, CustomNamedSet]
interface Storage {
    readonly attribute unsigned long length;
    any key(unsigned long index);
    [Throws] any getItem(DOMString key);
    [Throws] undefined setItem(DOMString key, DOMString value);
    undefined removeItem(DOMString key);
    undefined clear();
};

[Exposed=Window]
partial interface Window {
    [Throws, SameObject] readonly attribute Storage localStorage;
    [Throws, SameObject] readonly attribute Storage sessionStorage;
};

dictionary StorageEventInit : EventInit {
    DOMString? key = null;
    DOMString? oldValue = null;
    DOMString? newValue = null;
    USVString url = "";
    any storageArea = null;
};

[Exposed=Window, BindObj]
interface StorageEvent : Event {
    constructor(DOMString type, optional StorageEventInit eventInitDict = {});
    readonly attribute DOMString? key;
    readonly attribute DOMString? oldValue;
    readonly attribute DOMString? newValue;
    readonly attribute USVString url;
    readonly attribute any storageArea;
    undefined initStorageEvent(DOMString type, optional boolean bubbles = false, optional boolean cancelable = false,
                               optional DOMString? key = null, optional DOMString? oldValue = null,
                               optional DOMString? newValue = null, optional USVString url = "",
                               optional any storageArea = null);
};

// QuotaExceededError (WebIDL 3.14.2.2): a DOMException subclass with the DOMException legacy code 22. quota and requested
// are numbers or null; Storage throws it with both null. The options are read by hand (an `any` argument).
[Exposed=Window, BindObj]
interface QuotaExceededError : DOMException {
    constructor(optional DOMString message = "", optional any options);
    readonly attribute double? quota;
    readonly attribute double? requested;
};
