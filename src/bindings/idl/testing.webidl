// Interfaces for the unit tests of the generator and the runtime (never exposed to pages: they
// are defined on the global by embi_bind_expose_testing).
dictionary TestOptions {
    long count = 3;
    DOMString label = "x";
    boolean flag = false;
    double ratio = 0.5;
};

dictionary TestDerivedOptions : TestOptions {
    required DOMString must;
    sequence<DOMString> names;
};

dictionary TestResult {
    DOMString label;
    long count;
    boolean flag;
    sequence<long> numbers;
};

enum TestMode { "alpha", "beta", "gamma" };

callback TestCallback = undefined (long x);

[Exposed=Window, Testing, BindObj]
interface TestBase {
    constructor();
    readonly attribute DOMString baseName;
    DOMString describe();
};

[Exposed=Window, Testing, BindObj]
interface TestWidget : TestBase {
    constructor(optional TestOptions options = {});
    attribute long count;
    [Clamp] attribute octet small;
    [Clamp] attribute short sclamp;
    [Clamp] attribute long long lclamp;
    [EnforceRange] attribute unsigned long long big;
    attribute unsigned long long wrapped;
    attribute TestMode? mode;
    attribute DOMString? note;
    [LegacyNullToEmptyString] attribute DOMString text;
    readonly attribute DOMString constant;
    [SameObject] readonly attribute TestBase same;
    [PutForwards=count] readonly attribute TestWidget self;
    [Replaceable] readonly attribute long replaceable;
    [LegacyUnforgeable] readonly attribute long forged;
    TestResult result(optional TestOptions options = {});
    sequence<long> sequenceOf(sequence<long> values);
    sequence<DOMString> names();
    record<DOMString, long> recordOf(record<DOMString, long> values);
    Promise<long> promised(long value);
    Promise<long> promisedFail(long value);
    (DOMString or long) union((DOMString or long or boolean) value);
    DOMString kind((Node or DOMString or long or sequence<DOMString> or TestOptions) value);
    DOMString overload(DOMString text);
    DOMString overload(long number);
    DOMString overload(Node node);
    DOMString overload(boolean flag, DOMString text);
    DOMString overload(TestCallback cb);
    DOMString variadic(DOMString first, long... rest);
    [Throws] undefined fail(unsigned short code);
    long sum(sequence<long> values);
    static DOMString make();
    const unsigned short KIND = 7;
    iterable<long, DOMString>;
    stringifier;
};

[Exposed=Window, Testing, BindObj]
interface TestList {
    readonly attribute unsigned long length;
    getter DOMString? item(unsigned long index);
    iterable<DOMString>;
};
