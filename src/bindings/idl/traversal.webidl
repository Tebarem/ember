// DOM Standard traversal: NodeFilter, NodeIterator and TreeWalker (W5f). The filter is an `any`: null, a function, or an
// object with acceptNode (the callback interface NodeFilter).

[Exposed=Window]
interface NodeFilter {
    const unsigned short FILTER_ACCEPT = 1;
    const unsigned short FILTER_REJECT = 2;
    const unsigned short FILTER_SKIP = 3;
    const unsigned long SHOW_ALL = 0xFFFFFFFF;
    const unsigned long SHOW_ELEMENT = 0x1;
    const unsigned long SHOW_ATTRIBUTE = 0x2;
    const unsigned long SHOW_TEXT = 0x4;
    const unsigned long SHOW_CDATA_SECTION = 0x8;
    const unsigned long SHOW_ENTITY_REFERENCE = 0x10;
    const unsigned long SHOW_ENTITY = 0x20;
    const unsigned long SHOW_PROCESSING_INSTRUCTION = 0x40;
    const unsigned long SHOW_COMMENT = 0x80;
    const unsigned long SHOW_DOCUMENT = 0x100;
    const unsigned long SHOW_DOCUMENT_TYPE = 0x200;
    const unsigned long SHOW_DOCUMENT_FRAGMENT = 0x400;
    const unsigned long SHOW_NOTATION = 0x800;
};

[Exposed=Window, BindObj]
interface NodeIterator {
    [SameObject] readonly attribute Node root;
    readonly attribute Node referenceNode;
    readonly attribute boolean pointerBeforeReferenceNode;
    readonly attribute unsigned long whatToShow;
    readonly attribute any filter;
    [Throws] Node? nextNode();
    [Throws] Node? previousNode();
    undefined detach();
};

[Exposed=Window, BindObj]
interface TreeWalker {
    [SameObject] readonly attribute Node root;
    readonly attribute unsigned long whatToShow;
    readonly attribute any filter;
    attribute Node currentNode;
    [Throws] Node? parentNode();
    [Throws] Node? firstChild();
    [Throws] Node? lastChild();
    [Throws] Node? previousSibling();
    [Throws] Node? nextSibling();
    [Throws] Node? previousNode();
    [Throws] Node? nextNode();
};
