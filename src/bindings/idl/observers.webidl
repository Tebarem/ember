// MutationObserver, ResizeObserver and IntersectionObserver (W6d). The observer objects keep their
// state in C (src/bindings/impl_observers.c over src/dom/mutation.h). The records and entries are objects whose fields are read
// through their getters, so their attributes are typed any. The option bags are typed any too, because the implementation reads
// them (presence matters for MutationObserverInit: an absent attributes member defaults from the other options).

callback MutationCallback = undefined (any mutations, any observer);
callback ResizeObserverCallback = undefined (any entries, any observer);

[Exposed=Window, BindObj]
interface MutationObserver {
    constructor(MutationCallback callback);
    any observe(Node target, optional any options);
    undefined disconnect();
    any takeRecords();
};

[Exposed=Window, BindObj]
interface MutationRecord {
    readonly attribute any type;
    readonly attribute any target;
    readonly attribute any addedNodes;
    readonly attribute any removedNodes;
    readonly attribute any previousSibling;
    readonly attribute any nextSibling;
    readonly attribute any attributeName;
    readonly attribute any attributeNamespace;
    readonly attribute any oldValue;
};

enum ResizeObserverBoxOptions { "content-box", "border-box", "device-pixel-content-box" };

dictionary ResizeObserverOptions {
    ResizeObserverBoxOptions box = "content-box";
};

[Exposed=Window, BindObj]
interface ResizeObserver {
    constructor(ResizeObserverCallback callback);
    undefined observe(Element target, optional ResizeObserverOptions options = {});
    undefined unobserve(Element target);
    undefined disconnect();
};

[Exposed=Window, BindObj]
interface ResizeObserverEntry {
    readonly attribute any target;
    readonly attribute any contentRect;
    readonly attribute any borderBoxSize;
    readonly attribute any contentBoxSize;
    readonly attribute any devicePixelContentBoxSize;
};

[Exposed=Window, BindObj]
interface IntersectionObserver {
    constructor(any callback, optional any options);
    undefined observe(Element target);
    undefined unobserve(Element target);
    undefined disconnect();
    any takeRecords();
    readonly attribute any root;
    readonly attribute DOMString rootMargin;
    readonly attribute DOMString scrollMargin;
    readonly attribute any thresholds;
};

[Exposed=Window, BindObj]
interface IntersectionObserverEntry {
    readonly attribute any time;
    readonly attribute any rootBounds;
    readonly attribute any boundingClientRect;
    readonly attribute any intersectionRect;
    readonly attribute any isIntersecting;
    readonly attribute any intersectionRatio;
    readonly attribute any target;
};
