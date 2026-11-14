// Form controls and forms (HTML Standard sections 4.10 and 4.16, trimmed; W5e). The value and
// checkedness of a control are kept by impl_forms.c (dirty flags, the edit model of platform/edit.c); the reflected
// attributes are generated. Interfaces here replace the placeholders that used to be in html.webidl.
//
// Deviations: on textarea, selectionStart and selectionEnd are unsigned long (never null there) and selectionDirection a
// DOMString; on input, selectionStart and selectionEnd are any (unsigned long? converted by impl_forms.c: null on the types
// without the selection API, such as email and number, whose setters throw InvalidStateError); options and elements are
// NodeLists (not HTMLOptionsCollection or HTMLFormControlsCollection); setRangeText, the list and labels attributes, and the
// named getters of options and elements are not implemented. valueAsNumber, stepUp and stepDown
// cover the number and range types only (the date types are not implemented).

[Exposed=Window] interface ValidityState {
    readonly attribute boolean valueMissing;
    readonly attribute boolean typeMismatch;
    readonly attribute boolean patternMismatch;
    readonly attribute boolean tooLong;
    readonly attribute boolean tooShort;
    readonly attribute boolean rangeUnderflow;
    readonly attribute boolean rangeOverflow;
    readonly attribute boolean stepMismatch;
    readonly attribute boolean badInput;
    readonly attribute boolean customError;
    readonly attribute boolean valid;
};

// The files of a file input. Files are never chosen by the engine itself, so this is always empty for now.
[Exposed=Window] interface FileList {
    readonly attribute unsigned long length;
    File? item(unsigned long index);
};

[Exposed=Window, HTMLTag="form"] interface HTMLFormElement : HTMLElement {
    [CEReactions, Reflect="accept-charset"] attribute DOMString acceptCharset;
    [CEReactions, Reflect] attribute USVString action;
    [CEReactions, Reflect] attribute DOMString autocomplete;
    [CEReactions, Reflect] attribute DOMString enctype;
    [CEReactions, Reflect] attribute DOMString encoding;
    [CEReactions, Reflect] attribute DOMString method;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute boolean noValidate;
    [CEReactions, Reflect] attribute DOMString target;
    [CEReactions, Reflect] attribute DOMString rel;
    readonly attribute NodeList elements;
    readonly attribute unsigned long length;
    [CEReactions, Throws] undefined submit();
    [CEReactions, Throws] undefined requestSubmit(optional HTMLElement? submitter = null);
    [CEReactions] undefined reset();
    boolean checkValidity();
    boolean reportValidity();
};

[Exposed=Window, HTMLTag="input"] interface HTMLInputElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString accept;
    [CEReactions, Reflect] attribute DOMString alt;
    [CEReactions, Reflect] attribute DOMString autocomplete;
    [CEReactions, Reflect] attribute boolean autofocus;
    [CEReactions, Reflect=checked] attribute boolean defaultChecked;
    [CEReactions, Reflect=value] attribute DOMString defaultValue;
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute USVString formAction;
    [CEReactions, Reflect] attribute DOMString formEnctype;
    [CEReactions, Reflect] attribute DOMString formMethod;
    [CEReactions, Reflect] attribute boolean formNoValidate;
    [CEReactions, Reflect] attribute DOMString formTarget;
    [CEReactions, Reflect] attribute DOMString dirName;
    [CEReactions, Reflect] attribute DOMString max;
    [CEReactions, Reflect] attribute long maxLength;
    [CEReactions, Reflect] attribute DOMString min;
    [CEReactions, Reflect] attribute long minLength;
    [CEReactions, Reflect] attribute boolean multiple;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString pattern;
    [CEReactions, Reflect] attribute DOMString placeholder;
    [CEReactions, Reflect] attribute boolean readOnly;
    [CEReactions, Reflect] attribute boolean required;
    [CEReactions, Reflect] attribute unsigned long size;
    [CEReactions, Reflect] attribute USVString src;
    [CEReactions, Reflect] attribute DOMString step;
    [CEReactions] attribute DOMString type;
    [CEReactions] attribute DOMString value;
    [CEReactions] attribute boolean checked;
    [CEReactions] attribute boolean indeterminate;
    [CEReactions] attribute unrestricted double valueAsNumber;
    // unsigned long? in HTML: idlgen takes no nullable number as a value, so impl_forms.c converts it.
    [CEReactions, Throws] attribute any selectionStart;
    [CEReactions, Throws] attribute any selectionEnd;
    [CEReactions, Throws] attribute DOMString? selectionDirection;
    readonly attribute HTMLFormElement? form;
    readonly attribute FileList? files;
    readonly attribute boolean willValidate;
    readonly attribute ValidityState validity;
    readonly attribute DOMString validationMessage;
    boolean checkValidity();
    boolean reportValidity();
    undefined setCustomValidity(DOMString error);
    [CEReactions, Throws] undefined setSelectionRange(unsigned long start, unsigned long end, optional DOMString direction);
    [CEReactions, Throws] undefined stepUp(optional long n = 1);
    [CEReactions, Throws] undefined stepDown(optional long n = 1);
    undefined select();
};

[Exposed=Window, HTMLTag="textarea"] interface HTMLTextAreaElement : HTMLElement {
    [CEReactions, Reflect] attribute DOMString autocomplete;
    [CEReactions, Reflect] attribute boolean autofocus;
    [CEReactions, Reflect] attribute unsigned long cols;
    [CEReactions, Reflect] attribute DOMString dirName;
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute DOMString placeholder;
    [CEReactions, Reflect] attribute long maxLength;
    [CEReactions, Reflect] attribute long minLength;
    [CEReactions, Reflect] attribute boolean readOnly;
    [CEReactions, Reflect] attribute boolean required;
    [CEReactions, Reflect] attribute unsigned long rows;
    [CEReactions, Reflect] attribute DOMString wrap;
    readonly attribute DOMString type;
    [CEReactions] attribute DOMString defaultValue;
    [CEReactions] attribute DOMString value;
    readonly attribute unsigned long textLength;
    [CEReactions] attribute unsigned long selectionStart;
    [CEReactions] attribute unsigned long selectionEnd;
    [CEReactions] attribute DOMString selectionDirection;
    readonly attribute HTMLFormElement? form;
    readonly attribute boolean willValidate;
    readonly attribute ValidityState validity;
    readonly attribute DOMString validationMessage;
    boolean checkValidity();
    boolean reportValidity();
    undefined setCustomValidity(DOMString error);
    [CEReactions, Throws] undefined setSelectionRange(unsigned long start, unsigned long end, optional DOMString direction);
    undefined select();
};

[Exposed=Window, HTMLTag="select"] interface HTMLSelectElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean autofocus;
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute boolean multiple;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions, Reflect] attribute boolean required;
    [CEReactions, Reflect] attribute unsigned long size;
    readonly attribute DOMString type;
    readonly attribute NodeList options;
    readonly attribute unsigned long length;
    [CEReactions] attribute long selectedIndex;
    [CEReactions] attribute DOMString value;
    readonly attribute HTMLFormElement? form;
    readonly attribute boolean willValidate;
    readonly attribute ValidityState validity;
    readonly attribute DOMString validationMessage;
    boolean checkValidity();
    boolean reportValidity();
    undefined setCustomValidity(DOMString error);
};

[Exposed=Window, HTMLTag="option"] interface HTMLOptionElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions] attribute DOMString label;
    [CEReactions, Reflect=selected] attribute boolean defaultSelected;
    [CEReactions] attribute boolean selected;
    [CEReactions] attribute DOMString value;
    readonly attribute DOMString text;
    readonly attribute long index;
    readonly attribute HTMLFormElement? form;
};

[Exposed=Window, HTMLTag="button"] interface HTMLButtonElement : HTMLElement {
    [CEReactions, Reflect] attribute boolean autofocus;
    [CEReactions, Reflect] attribute boolean disabled;
    [CEReactions, Reflect] attribute USVString formAction;
    [CEReactions, Reflect] attribute DOMString formEnctype;
    [CEReactions, Reflect] attribute DOMString formMethod;
    [CEReactions, Reflect] attribute boolean formNoValidate;
    [CEReactions, Reflect] attribute DOMString formTarget;
    [CEReactions, Reflect] attribute DOMString name;
    [CEReactions] attribute DOMString type;
    [CEReactions, Reflect] attribute DOMString value;
    readonly attribute HTMLFormElement? form;
    readonly attribute boolean willValidate;
    readonly attribute ValidityState validity;
    readonly attribute DOMString validationMessage;
    boolean checkValidity();
    boolean reportValidity();
    undefined setCustomValidity(DOMString error);
};
