// DOM Standard (with the CSSOM View and HTML pieces frameworks use), trimmed to what Ember implements.

dictionary GetRootNodeOptions {
    boolean composed = false;
};

// CSSOM View: Element.scrollIntoView.
enum ScrollBehavior { "auto", "instant", "smooth" };
enum ScrollLogicalPosition { "start", "center", "end", "nearest" };

dictionary ScrollOptions {
    ScrollBehavior behavior = "auto";
};

dictionary ScrollIntoViewOptions : ScrollOptions {
    ScrollLogicalPosition block = "start";
    ScrollLogicalPosition inline = "nearest";
};

interface mixin NonElementParentNode {
    Element? getElementById(DOMString elementId);
};

interface mixin ParentNode {
    [SameObject] readonly attribute HTMLCollection children;
    readonly attribute Element? firstElementChild;
    readonly attribute Element? lastElementChild;
    readonly attribute unsigned long childElementCount;
    [CEReactions, Throws] undefined prepend((Node or DOMString)... nodes);
    [CEReactions, Throws] undefined append((Node or DOMString)... nodes);
    [CEReactions, Throws] undefined replaceChildren((Node or DOMString)... nodes);
    [CEReactions, Throws] undefined moveBefore(Node node, Node? child);
    [Throws] Element? querySelector(DOMString selectors);
    [Throws, NewObject] NodeList querySelectorAll(DOMString selectors);
};

interface mixin NonDocumentTypeChildNode {
    readonly attribute Element? previousElementSibling;
    readonly attribute Element? nextElementSibling;
};

interface mixin ChildNode {
    [CEReactions, Throws] undefined before((Node or DOMString)... nodes);
    [CEReactions, Throws] undefined after((Node or DOMString)... nodes);
    [CEReactions, Throws] undefined replaceWith((Node or DOMString)... nodes);
    [CEReactions] undefined remove();
};

interface mixin ElementCSSInlineStyle {
    [SameObject, PutForwards=cssText] readonly attribute CSSStyleDeclaration style;
};

[Exposed=Window]
interface Node : EventTarget {
    const unsigned short ELEMENT_NODE = 1;
    const unsigned short ATTRIBUTE_NODE = 2;
    const unsigned short TEXT_NODE = 3;
    const unsigned short CDATA_SECTION_NODE = 4;
    const unsigned short ENTITY_REFERENCE_NODE = 5;
    const unsigned short ENTITY_NODE = 6;
    const unsigned short PROCESSING_INSTRUCTION_NODE = 7;
    const unsigned short COMMENT_NODE = 8;
    const unsigned short DOCUMENT_NODE = 9;
    const unsigned short DOCUMENT_TYPE_NODE = 10;
    const unsigned short DOCUMENT_FRAGMENT_NODE = 11;
    const unsigned short NOTATION_NODE = 12;
    const unsigned short DOCUMENT_POSITION_DISCONNECTED = 0x01;
    const unsigned short DOCUMENT_POSITION_PRECEDING = 0x02;
    const unsigned short DOCUMENT_POSITION_FOLLOWING = 0x04;
    const unsigned short DOCUMENT_POSITION_CONTAINS = 0x08;
    const unsigned short DOCUMENT_POSITION_CONTAINED_BY = 0x10;
    const unsigned short DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC = 0x20;

    readonly attribute unsigned short nodeType;
    readonly attribute DOMString nodeName;
    readonly attribute USVString baseURI;
    readonly attribute boolean isConnected;
    readonly attribute Document? ownerDocument;
    Node getRootNode(optional GetRootNodeOptions options = {});
    readonly attribute Node? parentNode;
    readonly attribute Element? parentElement;
    boolean hasChildNodes();
    [SameObject] readonly attribute NodeList childNodes;
    readonly attribute Node? firstChild;
    readonly attribute Node? lastChild;
    readonly attribute Node? previousSibling;
    readonly attribute Node? nextSibling;
    [CEReactions] attribute DOMString? nodeValue;
    [CEReactions] attribute DOMString? textContent;
    [CEReactions] undefined normalize();
    [CEReactions, NewObject, Throws] Node cloneNode(optional boolean subtree = false);
    boolean isEqualNode(Node? otherNode);
    boolean isSameNode(Node? otherNode);
    unsigned short compareDocumentPosition(Node other);
    boolean contains(Node? other);
    DOMString? lookupNamespaceURI(DOMString? prefix);
    DOMString? lookupPrefix(DOMString? namespace);
    boolean isDefaultNamespace(DOMString? namespace);
    [CEReactions, Throws] Node insertBefore(Node node, Node? child);
    [CEReactions, Throws] Node appendChild(Node node);
    [CEReactions, Throws] Node replaceChild(Node node, Node child);
    [CEReactions, Throws] Node removeChild(Node child);
};

[Exposed=Window]
interface Attr : Node {
    readonly attribute DOMString? namespaceURI;
    readonly attribute DOMString? prefix;
    readonly attribute DOMString localName;
    readonly attribute DOMString name;
    attribute DOMString value;
    readonly attribute Element? ownerElement;
    readonly attribute boolean specified;
};

[Exposed=Window]
interface CharacterData : Node {
    [LegacyNullToEmptyString] attribute DOMString data;
    readonly attribute unsigned long length;
    [Throws] DOMString substringData(unsigned long offset, unsigned long count);
    undefined appendData(DOMString data);
    [Throws] undefined insertData(unsigned long offset, DOMString data);
    [Throws] undefined deleteData(unsigned long offset, unsigned long count);
    [Throws] undefined replaceData(unsigned long offset, unsigned long count, DOMString data);
};
CharacterData includes NonDocumentTypeChildNode;
CharacterData includes ChildNode;

[Exposed=Window]
interface Text : CharacterData {
    constructor(optional DOMString data = "");
    [Throws, NewObject] Text splitText(unsigned long offset);
    readonly attribute DOMString wholeText;
};

[Exposed=Window]
interface Comment : CharacterData {
    constructor(optional DOMString data = "");
};

[Exposed=Window]
interface CDATASection : Text {
};

[Exposed=Window]
interface ProcessingInstruction : CharacterData {
    readonly attribute DOMString target;
};

[Exposed=Window]
interface DocumentType : Node {
    readonly attribute DOMString name;
    readonly attribute DOMString publicId;
    readonly attribute DOMString systemId;
};
DocumentType includes ChildNode;

[Exposed=Window]
interface DocumentFragment : Node {
    constructor();
};
DocumentFragment includes NonElementParentNode;
DocumentFragment includes ParentNode;

[Exposed=Window]
interface Document : Node {
    constructor();
    [SameObject] readonly attribute DOMImplementation implementation;
    readonly attribute DOMString URL;
    readonly attribute DOMString documentURI;
    [SameObject, PutForwards=href] readonly attribute Location? location;
    readonly attribute DOMString compatMode;
    readonly attribute DOMString characterSet;
    readonly attribute DOMString charset;
    readonly attribute DOMString inputEncoding;
    readonly attribute DOMString contentType;
    readonly attribute DocumentType? doctype;
    readonly attribute Element? documentElement;
    readonly attribute Element? scrollingElement;
    HTMLCollection getElementsByTagName(DOMString qualifiedName);
    HTMLCollection getElementsByTagNameNS(DOMString? namespace, DOMString localName);
    HTMLCollection getElementsByClassName(DOMString classNames);
    [CEReactions, Throws, NewObject] Element createElement(DOMString localName);
    [CEReactions, Throws, NewObject] Element createElementNS(DOMString? namespace, DOMString qualifiedName);
    [CEReactions, Throws, NewObject] Attr createAttribute(DOMString localName);
    [CEReactions, Throws, NewObject] Attr createAttributeNS(DOMString? namespace, DOMString qualifiedName);
    [NewObject] DocumentFragment createDocumentFragment();
    [NewObject] Text createTextNode(DOMString data);
    [NewObject] Comment createComment(DOMString data);
    [Throws, NewObject] CDATASection createCDATASection(DOMString data);
    [Throws, NewObject] ProcessingInstruction createProcessingInstruction(DOMString target, DOMString data);
    [CEReactions, Throws] Node importNode(Node node, optional boolean subtree = false);
    [CEReactions, Throws] Node adoptNode(Node node);
    [Throws] object createEvent(DOMString interface);
    [NewObject] NodeIterator createNodeIterator(Node root, optional unsigned long whatToShow = 0xFFFFFFFF, optional any filter = null);
    [NewObject] TreeWalker createTreeWalker(Node root, optional unsigned long whatToShow = 0xFFFFFFFF, optional any filter = null);

    readonly attribute DOMString domain;
    readonly attribute DOMString referrer;
    attribute DOMString cookie;
    readonly attribute DOMString lastModified;
    readonly attribute DOMString readyState;
    readonly attribute Window? defaultView;
    readonly attribute Element? activeElement;
    boolean hasFocus();
    readonly attribute boolean hidden;
    readonly attribute DOMString visibilityState;
    [CEReactions] attribute DOMString title;
    readonly attribute HTMLHeadElement? head;
    [CEReactions, Throws] attribute HTMLElement? body;
    [SameObject] readonly attribute HTMLCollection images;
    [SameObject] readonly attribute HTMLCollection links;
    [SameObject] readonly attribute HTMLCollection forms;
    [SameObject] readonly attribute HTMLCollection scripts;
    [SameObject] readonly attribute HTMLCollection embeds;
    [SameObject] readonly attribute HTMLCollection all;
    [SameObject] readonly attribute HTMLCollection anchors;
};
Document includes NonElementParentNode;
Document includes ParentNode;

[Exposed=Window]
interface Element : Node {
    readonly attribute DOMString? namespaceURI;
    readonly attribute DOMString? prefix;
    readonly attribute DOMString localName;
    readonly attribute DOMString tagName;

    [CEReactions, Reflect] attribute DOMString id;
    [CEReactions, Reflect=class] attribute DOMString className;
    [SameObject, PutForwards=value] readonly attribute DOMTokenList classList;
    [CEReactions, Reflect] attribute DOMString slot;

    boolean hasAttributes();
    [SameObject] readonly attribute NamedNodeMap attributes;
    sequence<DOMString> getAttributeNames();
    DOMString? getAttribute(DOMString qualifiedName);
    DOMString? getAttributeNS(DOMString? namespace, DOMString localName);
    [CEReactions, Throws] undefined setAttribute(DOMString qualifiedName, DOMString value);
    [CEReactions, Throws] undefined setAttributeNS(DOMString? namespace, DOMString qualifiedName, DOMString value);
    [CEReactions] undefined removeAttribute(DOMString qualifiedName);
    [CEReactions] undefined removeAttributeNS(DOMString? namespace, DOMString localName);
    [CEReactions, Throws] boolean toggleAttribute(DOMString qualifiedName);
    [CEReactions, Throws] boolean toggleAttribute(DOMString qualifiedName, boolean force);
    boolean hasAttribute(DOMString qualifiedName);
    boolean hasAttributeNS(DOMString? namespace, DOMString localName);
    Attr? getAttributeNode(DOMString qualifiedName);
    Attr? getAttributeNodeNS(DOMString? namespace, DOMString localName);
    [CEReactions, Throws] Attr? setAttributeNode(Attr attr);
    [CEReactions, Throws] Attr? setAttributeNodeNS(Attr attr);
    [CEReactions, Throws] Attr removeAttributeNode(Attr attr);

    [Throws] Element? closest(DOMString selectors);
    [Throws] boolean matches(DOMString selectors);
    [Throws] boolean webkitMatchesSelector(DOMString selectors);

    HTMLCollection getElementsByTagName(DOMString qualifiedName);
    HTMLCollection getElementsByTagNameNS(DOMString? namespace, DOMString localName);
    HTMLCollection getElementsByClassName(DOMString classNames);

    [CEReactions, Throws] Element? insertAdjacentElement(DOMString where, Element element);
    [Throws] undefined insertAdjacentText(DOMString where, DOMString data);
    [CEReactions, Throws] undefined insertAdjacentHTML(DOMString position, DOMString text);
    [CEReactions, Throws, LegacyNullToEmptyString] attribute DOMString innerHTML;
    [CEReactions, Throws, LegacyNullToEmptyString] attribute DOMString outerHTML;

    DOMRectList getClientRects();
    [NewObject] DOMRect getBoundingClientRect();
    undefined scrollIntoView(optional (boolean or ScrollIntoViewOptions) arg);
    attribute unrestricted double scrollTop;
    attribute unrestricted double scrollLeft;
    readonly attribute long scrollWidth;
    readonly attribute long scrollHeight;
    readonly attribute long clientTop;
    readonly attribute long clientLeft;
    readonly attribute long clientWidth;
    readonly attribute long clientHeight;
};
Element includes ParentNode;
Element includes NonDocumentTypeChildNode;
Element includes ChildNode;

[Exposed=Window, BindObj]
interface NodeList {
    getter Node? item(unsigned long index);
    readonly attribute unsigned long length;
    iterable<Node>;
};

[Exposed=Window, BindObj, NamedLookup, LegacyUnenumerableNamedProperties]
interface HTMLCollection {
    readonly attribute unsigned long length;
    getter Element? item(unsigned long index);
    Element? namedItem(DOMString name);
    [SymbolIteratorOnly] iterable<Element>;
};

[Exposed=Window, BindObj]
interface DOMTokenList {
    readonly attribute unsigned long length;
    getter DOMString? item(unsigned long index);
    boolean contains(DOMString token);
    [CEReactions, Throws] undefined add(DOMString... tokens);
    [CEReactions, Throws] undefined remove(DOMString... tokens);
    [CEReactions, Throws] boolean toggle(DOMString token);
    [CEReactions, Throws] boolean toggle(DOMString token, boolean force);
    [CEReactions, Throws] boolean replace(DOMString token, DOMString newToken);
    [Throws] boolean supports(DOMString token);
    [CEReactions] attribute DOMString value;
    stringifier;
    iterable<DOMString>;
};

[Exposed=Window, BindObj, NamedLookup, LegacyUnenumerableNamedProperties]
interface NamedNodeMap {
    readonly attribute unsigned long length;
    getter Attr? item(unsigned long index);
    Attr? getNamedItem(DOMString qualifiedName);
    Attr? getNamedItemNS(DOMString? namespace, DOMString localName);
    [CEReactions, Throws] Attr? setNamedItem(Attr attr);
    [CEReactions, Throws] Attr? setNamedItemNS(Attr attr);
    [CEReactions, Throws] Attr removeNamedItem(DOMString qualifiedName);
    [CEReactions, Throws] Attr removeNamedItemNS(DOMString? namespace, DOMString localName);
    iterable<Attr>;
};

[Exposed=Window, BindObj, CustomNamed, LegacyUnenumerableNamedProperties]
interface DOMStringMap {
};

[Exposed=Window]
interface XMLDocument : Document {
};

[Exposed=Window, BindObj]
interface DOMImplementation {
    [Throws, NewObject] DocumentType createDocumentType(DOMString qualifiedName, DOMString publicId, DOMString systemId);
    [Throws, NewObject] XMLDocument createDocument(DOMString? namespace, [LegacyNullToEmptyString] DOMString qualifiedName, optional DocumentType? doctype = null);
    [NewObject] Document createHTMLDocument();
    [NewObject] Document createHTMLDocument(DOMString title);
    boolean hasFeature();
};

enum DOMParserSupportedType { "text/html", "text/xml", "application/xml", "application/xhtml+xml", "image/svg+xml" };

[Exposed=Window, BindObj]
interface DOMParser {
    constructor();
    [NewObject] Document parseFromString(DOMString string, DOMParserSupportedType type);
};
