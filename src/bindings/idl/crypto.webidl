// The Web Crypto subset of W7c (getRandomValues and randomUUID): a ChaCha20 random
// source, documented as not for key generation. Implementation: impl_loader_crypto.c.

[Exposed=Window, BindObj]
interface Crypto {
    [Throws] any getRandomValues(any array);
    DOMString randomUUID();
};

[Exposed=Window]
partial interface Window {
    [SameObject] readonly attribute Crypto crypto;
};
