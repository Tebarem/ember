// HTML elements (HTML Standard, trimmed). [HTMLTag] maps local names in the HTML namespace to the
// interface; reflected attributes need no hand-written code. The form controls (form, input, textarea, select, option,
// button) are in forms.webidl (W5e).

[Exposed=Window]
interface HTMLElement : Element {
    [CEReactions, Reflect] attribute DOMString title;
    [CEReactions, Reflect] attribute DOMString lang;
    [CEReactions, Reflect] attribute boolean translate;
    [CEReactions, Reflect] attribute DOMString dir;
    [CEReactions, Reflect] attribute boolean hidden;
    [CEReactions, Reflect] attribute DOMString accessKey;
    [CEReactions, Reflect] attribute boolean draggable;
    [CEReactions, Reflect] attribute boolean spellcheck;
    [CEReactions, Reflect] attribute DOMString contentEditable;
    [CEReactions, Reflect] attribute boolean inert;
    [CEReactions, Reflect] attribute DOMString autocapitalize;
    [CEReactions, Reflect] attribute DOMString nonce;
    [CEReactions] attribute long tabIndex;
    [CEReactions, Throws] attribute DOMString innerText;
    [CEReactions, Throws] attribute DOMString outerText;
    undefined click();
    undefined focus();
    undefined blur();
    readonly attribute Element? offsetParent;
    readonly attribute long offsetTop;
    readonly attribute long offsetLeft;
    readonly attribute long offsetWidth;
    readonly attribute long offsetHeight;
    [SameObject] readonly attribute DOMStringMap dataset;
};
HTMLElement includes ElementCSSInlineStyle;

[Exposed=Window, HTMLTag="div"] interface HTMLDivElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; };
[Exposed=Window, HTMLTag="span"] interface HTMLSpanElement : HTMLElement {};
[Exposed=Window, HTMLTag="p"] interface HTMLParagraphElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; };
[Exposed=Window, HTMLTag="br"] interface HTMLBRElement : HTMLElement { [CEReactions, Reflect] attribute DOMString clear; };
[Exposed=Window, HTMLTag="hr"] interface HTMLHRElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; [CEReactions, Reflect] attribute DOMString width; };
[Exposed=Window, HTMLTags="h1 h2 h3 h4 h5 h6"] interface HTMLHeadingElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; };
[Exposed=Window, HTMLTag="pre"] interface HTMLPreElement : HTMLElement {};
[Exposed=Window, HTMLTags="q blockquote"] interface HTMLQuoteElement : HTMLElement { [CEReactions, Reflect] attribute USVString cite; };
[Exposed=Window, HTMLTag="ul"] interface HTMLUListElement : HTMLElement { [CEReactions, Reflect] attribute boolean compact; [CEReactions, Reflect] attribute DOMString type; };
[Exposed=Window, HTMLTag="ol"] interface HTMLOListElement : HTMLElement { [CEReactions, Reflect] attribute boolean reversed; [CEReactions, Reflect] attribute long start; [CEReactions, Reflect] attribute DOMString type; };
[Exposed=Window, HTMLTag="li"] interface HTMLLIElement : HTMLElement { [CEReactions, Reflect] attribute long value; [CEReactions, Reflect] attribute DOMString type; };
[Exposed=Window, HTMLTag="dl"] interface HTMLDListElement : HTMLElement { [CEReactions, Reflect] attribute boolean compact; };
[Exposed=Window, HTMLTag="html"] interface HTMLHtmlElement : HTMLElement { [CEReactions, Reflect] attribute DOMString version; };
[Exposed=Window, HTMLTag="head"] interface HTMLHeadElement : HTMLElement {};
[Exposed=Window, HTMLTag="title"] interface HTMLTitleElement : HTMLElement { [CEReactions, Throws] attribute DOMString text; };
[Exposed=Window, HTMLTag="body"] interface HTMLBodyElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString text;
    [CEReactions, Reflect] attribute DOMString link;
    [CEReactions, Reflect] attribute DOMString vLink;
    [CEReactions, Reflect] attribute DOMString aLink;
    [CEReactions, Reflect] attribute DOMString bgColor;
    [CEReactions, Reflect] attribute DOMString background;
};
[Exposed=Window, HTMLTag="base"] interface HTMLBaseElement : HTMLElement { [CEReactions, Reflect] attribute USVString href; [CEReactions, Reflect] attribute DOMString target; };
[Exposed=Window, HTMLTag="meta"] interface HTMLMetaElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect="http-equiv"] attribute DOMString httpEquiv;
    [CEReactions, Reflect] attribute DOMString content;
    [CEReactions, Reflect] attribute DOMString media;
};
[Exposed=Window, HTMLTag="link"] interface HTMLLinkElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString href;
    [CEReactions, Reflect] attribute DOMString? crossOrigin;
    [CEReactions, Reflect] attribute DOMString rel;
    [CEReactions, Reflect] attribute DOMString as;
    [CEReactions, Reflect] attribute DOMString media;
    [CEReactions, Reflect] attribute DOMString hreflang;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute boolean disabled;
    [SameObject, PutForwards=value] readonly attribute DOMTokenList relList;
};
[Exposed=Window, HTMLTag="style"] interface HTMLStyleElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute DOMString media;
    [CEReactions, Reflect] attribute DOMString type;
};
[Exposed=Window, HTMLTag="script"] interface HTMLScriptElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute boolean noModule;
    [CEReactions, Reflect] attribute boolean async;
    [CEReactions, Reflect] attribute boolean defer;
    [CEReactions, Reflect] attribute DOMString? crossOrigin;
    [CEReactions, Reflect] attribute DOMString integrity;
    [CEReactions, Reflect] attribute DOMString charset;
    [CEReactions, Throws] attribute DOMString text;
};
[Exposed=Window, HTMLTag="template"] interface HTMLTemplateElement : HTMLElement {
    [SameObject] readonly attribute DocumentFragment content;
};
[Exposed=Window, HTMLTag="slot"] interface HTMLSlotElement : HTMLElement { [CEReactions, Reflect] attribute DOMString name; };
[Exposed=Window, HTMLTag="a"] interface HTMLAnchorElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString target;
    [CEReactions, Reflect] attribute DOMString download;
    [CEReactions, Reflect] attribute USVString ping;
    [CEReactions, Reflect] attribute DOMString rel;
    [CEReactions, Reflect] attribute DOMString hreflang;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute USVString href;
    [CEReactions, Reflect] attribute DOMString coords;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString shape;
    [SameObject, PutForwards=value] readonly attribute DOMTokenList relList;
    [CEReactions, Throws] attribute DOMString text;
};
[Exposed=Window, HTMLTag="area"] interface HTMLAreaElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString alt;
    [CEReactions, Reflect] attribute DOMString coords;
    [CEReactions, Reflect] attribute DOMString shape;
    [CEReactions, Reflect] attribute DOMString target;
    [CEReactions, Reflect] attribute USVString href;
};
[Exposed=Window, HTMLTag="map"] interface HTMLMapElement : HTMLElement { [CEReactions, Reflect] attribute DOMString name; };
[Exposed=Window, HTMLTag="img"] interface HTMLImageElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString alt;
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute USVString srcset;
    [CEReactions, Reflect] attribute DOMString sizes;
    [CEReactions, Reflect] attribute DOMString? crossOrigin;
    [CEReactions, Reflect] attribute DOMString useMap;
    [CEReactions, Reflect] attribute boolean isMap;
    [CEReactions, Reflect] attribute unsigned long width;
    [CEReactions, Reflect] attribute unsigned long height;
    [CEReactions, Reflect] attribute DOMString loading;
    [CEReactions, Reflect] attribute DOMString decoding;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString align;
    readonly attribute unsigned long naturalWidth;
    readonly attribute unsigned long naturalHeight;
    readonly attribute boolean complete;
    readonly attribute USVString currentSrc;
};
[Exposed=Window, HTMLTag="picture"] interface HTMLPictureElement : HTMLElement {};
[Exposed=Window, HTMLTag="source"] interface HTMLSourceElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute USVString srcset;
    [CEReactions, Reflect] attribute DOMString sizes;
    [CEReactions, Reflect] attribute DOMString media;
};
[Exposed=Window, HTMLTag="track"] interface HTMLTrackElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString kind;
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString srclang;
    [CEReactions, Reflect] attribute DOMString label;
    [CEReactions, Reflect] attribute boolean default;
};
[Exposed=Window] interface HTMLMediaElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute boolean autoplay;
    [CEReactions, Reflect] attribute boolean loop;
    [CEReactions, Reflect] attribute boolean controls;
    [CEReactions, Reflect] attribute boolean muted;
    [CEReactions, Reflect] attribute DOMString preload;
};
[Exposed=Window, HTMLTag="audio"] interface HTMLAudioElement : HTMLMediaElement {};
[Exposed=Window, HTMLTag="video"] interface HTMLVideoElement : HTMLMediaElement {
    [CEReactions, Reflect] attribute unsigned long width;
    [CEReactions, Reflect] attribute unsigned long height;
    [CEReactions, Reflect] attribute USVString poster;
};
// HTMLCanvasElement is in canvas.webidl (R4b).
[Exposed=Window, HTMLTag="iframe"] interface HTMLIFrameElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString srcdoc;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString width;
    [CEReactions, Reflect] attribute DOMString height;
    [CEReactions, Reflect] attribute DOMString allow;
    [CEReactions, Reflect] attribute DOMString loading;
};
[Exposed=Window, HTMLTag="embed"] interface HTMLEmbedElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute DOMString width;
    [CEReactions, Reflect] attribute DOMString height;
};
[Exposed=Window, HTMLTag="object"] interface HTMLObjectElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString data;
    [CEReactions, Reflect] attribute DOMString type;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString width;
    [CEReactions, Reflect] attribute DOMString height;
};
[Exposed=Window, HTMLTags="ins del"] interface HTMLModElement : HTMLElement {
    [CEReactions, Reflect] attribute USVString cite;
    [CEReactions, Reflect] attribute DOMString dateTime;
};
[Exposed=Window, HTMLTag="time"] interface HTMLTimeElement : HTMLElement { [CEReactions, Reflect] attribute DOMString dateTime; };
[Exposed=Window, HTMLTag="data"] interface HTMLDataElement : HTMLElement { [CEReactions, Reflect] attribute DOMString value; };
[Exposed=Window, HTMLTag="details"] interface HTMLDetailsElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean open;
    [CEReactions, Reflect] attribute DOMString name;
};
[Exposed=Window, HTMLTag="dialog"] interface HTMLDialogElement : HTMLElement { [CEReactions, Reflect] attribute boolean open; };
[Exposed=Window, HTMLTag="datalist"] interface HTMLDataListElement : HTMLElement {};
[Exposed=Window, HTMLTag="menu"] interface HTMLMenuElement : HTMLElement { [CEReactions, Reflect] attribute boolean compact; };

[Exposed=Window, HTMLTag="table"] interface HTMLTableElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString align;
    [CEReactions, Reflect] attribute DOMString border;
    [CEReactions, Reflect] attribute DOMString frame;
    [CEReactions, Reflect] attribute DOMString rules;
    [CEReactions, Reflect] attribute DOMString summary;
    [CEReactions, Reflect] attribute DOMString width;
    [CEReactions, Reflect] attribute DOMString cellPadding;
    [CEReactions, Reflect] attribute DOMString cellSpacing;
    [CEReactions, Reflect] attribute DOMString bgColor;
    readonly attribute HTMLTableCaptionElement? caption;
    readonly attribute HTMLTableSectionElement? tHead;
    readonly attribute HTMLTableSectionElement? tFoot;
    [SameObject] readonly attribute HTMLCollection tBodies;
    [SameObject] readonly attribute HTMLCollection rows;
    [CEReactions, NewObject] HTMLTableCaptionElement createCaption();
    [CEReactions] undefined deleteCaption();
    [CEReactions, NewObject] HTMLTableSectionElement createTHead();
    [CEReactions] undefined deleteTHead();
    [CEReactions, NewObject] HTMLTableSectionElement createTFoot();
    [CEReactions] undefined deleteTFoot();
    [CEReactions, NewObject] HTMLTableSectionElement createTBody();
    [CEReactions, Throws, NewObject] HTMLTableRowElement insertRow(optional long index = -1);
    [CEReactions, Throws] undefined deleteRow(long index);
};
[Exposed=Window, HTMLTag="caption"] interface HTMLTableCaptionElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; };
[Exposed=Window, HTMLTags="col colgroup"] interface HTMLTableColElement : HTMLElement {
    [CEReactions, Reflect] attribute unsigned long span;
    [CEReactions, Reflect] attribute DOMString align;
    [CEReactions, Reflect] attribute DOMString width;
};
[Exposed=Window, HTMLTags="thead tbody tfoot"] interface HTMLTableSectionElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString align;
    [CEReactions, Reflect] attribute DOMString vAlign;
    [SameObject] readonly attribute HTMLCollection rows;
    [CEReactions, Throws, NewObject] HTMLTableRowElement insertRow(optional long index = -1);
    [CEReactions, Throws] undefined deleteRow(long index);
};
[Exposed=Window, HTMLTag="tr"] interface HTMLTableRowElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString align;
    [CEReactions, Reflect] attribute DOMString vAlign;
    [CEReactions, Reflect] attribute DOMString bgColor;
    readonly attribute long rowIndex;
    readonly attribute long sectionRowIndex;
    [SameObject] readonly attribute HTMLCollection cells;
    [CEReactions, Throws, NewObject] HTMLElement insertCell(optional long index = -1);
    [CEReactions, Throws] undefined deleteCell(long index);
};
[Exposed=Window, HTMLTags="td th"] interface HTMLTableCellElement : HTMLElement {
    readonly attribute long cellIndex;
    [CEReactions, Reflect] attribute unsigned long colSpan;
    [CEReactions, Reflect] attribute unsigned long rowSpan;
    [CEReactions, Reflect] attribute DOMString headers;
    [CEReactions, Reflect] attribute DOMString align;
    [CEReactions, Reflect] attribute DOMString width;
    [CEReactions, Reflect] attribute DOMString height;
    [CEReactions, Reflect] attribute DOMString vAlign;
    [CEReactions, Reflect] attribute DOMString bgColor;
    [CEReactions, Reflect] attribute DOMString scope;
};

[Exposed=Window, HTMLTag="label"] interface HTMLLabelElement : HTMLElement { [CEReactions, Reflect=for] attribute DOMString htmlFor; };
[Exposed=Window, HTMLTag="legend"] interface HTMLLegendElement : HTMLElement { [CEReactions, Reflect] attribute DOMString align; };
[Exposed=Window, HTMLTag="fieldset"] interface HTMLFieldSetElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute DOMString name;
};
[Exposed=Window, HTMLTag="optgroup"] interface HTMLOptGroupElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute DOMString label;
};
[Exposed=Window, HTMLTag="output"] interface HTMLOutputElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect=for] attribute DOMString htmlFor;
};
[Exposed=Window, HTMLTag="progress"] interface HTMLProgressElement : HTMLElement {
    [CEReactions, Reflect] attribute double value;
    [CEReactions, Reflect] attribute double max;
};
[Exposed=Window, HTMLTag="meter"] interface HTMLMeterElement : HTMLElement {
    [CEReactions, Reflect] attribute double value;
    [CEReactions, Reflect] attribute double min;
    [CEReactions, Reflect] attribute double max;
    [CEReactions, Reflect] attribute double low;
    [CEReactions, Reflect] attribute double high;
    [CEReactions, Reflect] attribute double optimum;
};
[Exposed=Window] interface HTMLUnknownElement : HTMLElement {};
