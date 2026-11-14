#pragma once
/*
 * Loader core (W7a): WHATWG URL, the Encoding Standard, MIME Sniffing, data: URLs, the HTML
 * byte-stream decoding and forgiving base64 (in src/base). Internal module API: embi_ prefix.
 *
 * This header and every file in src/loader/ are C only. They include no JS header, so the loader
 * stays usable from the embedder and from the C-only unit tests.
 *
 * Ownership: every struct with an owned buffer has a *_free function, and a zeroed struct is an
 * empty value. embi_sb holds byte strings here too (URL and form data are byte-clean UTF-8, and
 * decoded bodies are arbitrary bytes).
 */
#include "base/base.h"

/* One past the end of s. A string with no data gives nullptr, so no pointer arithmetic is done on
 * a null base (UBSan in CI rejects that even with a zero offset). */
static inline const char *embi_str_end(embi_str s)
{
    return s.ptr != nullptr ? s.ptr + s.len : nullptr;
}

typedef struct embi_encoding embi_encoding;

/* ------------------------------------------------------------------ URL (url.c, idna.c) */

typedef enum embi_url_attr {
    EMBI_URL_HREF,
    EMBI_URL_PROTOCOL,
    EMBI_URL_USERNAME,
    EMBI_URL_PASSWORD,
    EMBI_URL_HOST,
    EMBI_URL_HOSTNAME,
    EMBI_URL_PORT,
    EMBI_URL_PATHNAME,
    EMBI_URL_SEARCH,
    EMBI_URL_HASH,
} embi_url_attr;

/* A parsed URL record packed into one string. The offsets index href. The scheme is at offset 0
 * and is scheme_len bytes long, without the ':'. A component that is null in the record has its
 * flag false and length 0. A null port is -1. The host serialization includes [] for IPv6 and
 * never includes the port. */
typedef struct embi_url {
    char *href; /* owned, NUL-terminated; nullptr only in a zeroed value */
    uint32_t href_len;
    uint32_t scheme_len;
    uint32_t user_off, user_len;
    uint32_t pass_off, pass_len;
    uint32_t host_off, host_len;
    uint32_t path_off, path_len; /* the path as the URL record stores it; excludes a "/." prefix */
    uint32_t query_off, query_len;
    uint32_t frag_off, frag_len;
    int32_t port;
    bool has_host, has_query, has_fragment, opaque_path;
} embi_url;

/* Basic URL parser (WHATWG URL 4.4) with no encoding override (queries are UTF-8). base may be
 * nullptr. Returns false on failure; out is then left zeroed. The input is a UTF-8 string. */
bool embi_url_parse(embi_str input, const embi_url *base, embi_url *out);
/* The same with an encoding override for the query (the document encoding). A UTF-8, UTF-16,
 * replacement, unsupported or null encoding means UTF-8. A single-byte encoding or x-user-defined
 * encodes each non-ASCII code point of the query as one percent-encoded byte, and an
 * unrepresentable one as "%26%23N%3B" (the URL standard's percent-encode-after-encoding). */
bool embi_url_parse_enc(embi_str input, const embi_url *base, const embi_encoding *query_enc, embi_url *out);
/* Frees the owned buffer and zeroes the value. */
void embi_url_free(embi_url *u);
/* Deep copy of a parsed URL. */
bool embi_url_copy(const embi_url *src, embi_url *dst);
/* Appends the attribute's getter value to out (href, protocol, username, password, host,
 * hostname, port, pathname, search, hash), per the URL standard's API getters. */
void embi_url_get(const embi_url *u, embi_url_attr attr, embi_sb *out);
/* Applies a URL API setter with the state-override parser. Returns false only when a href value
 * fails to parse (the URL is then unchanged). Other setters return true, whether or not they
 * changed anything, as the standard requires. */
bool embi_url_set(embi_url *u, embi_url_attr attr, embi_str value);
/* Origin serialization: "scheme://host[:port]" for tuple origins, "null" for opaque ones. */
void embi_url_origin(const embi_url *u, embi_sb *out);
bool embi_url_is_special(const embi_url *u);
/* Whether input parses (with base) without keeping the result. */
bool embi_url_can_parse(embi_str input, const embi_url *base);

/* Percent-decoding (bytes). Appends the decoded bytes to out. */
void embi_pct_decode(embi_str in, embi_sb *out);

/* UTS #46 ToASCII as the URL standard's domain to ASCII (non-transitional, UseSTD3ASCIIRules off,
 * CheckBidi and CheckJoiners on). Accepts a UTF-8 domain and appends the ASCII form to out. Returns
 * false when a label is invalid. A-labels (xn--) are checked for ASCII only: they are not decoded and
 * neither make a domain Bidi nor get the bidi rule (the WPT data expects that). When any other label
 * has an R, AL or AN character, the bidi rule applies to every non-A-label, ASCII ones included. */
bool embi_idna_to_ascii(embi_str domain, embi_sb *out); /* out may hold partial output after false */
/* RFC 3492 Punycode over code points. Encode appends the ASCII form. Decode appends UTF-8 and
 * returns false on malformed input. */
bool embi_punycode_encode(const uint32_t *cps, uint32_t n, embi_sb *out);
bool embi_punycode_decode(embi_str in, embi_sb *out);

/* ------------------------------------------------------------------ application/x-www-form-urlencoded */

typedef struct embi_form_pair {
    embi_sb name;
    embi_sb value;
} embi_form_pair;

typedef EMBI_VEC(embi_form_pair) embi_form;

/* Parses an urlencoded string and appends its pairs to out. Names and values are decoded to
 * UTF-8 (invalid sequences become U+FFFD). */
void embi_form_parse(embi_str input, embi_form *out);
/* Serializes pairs as name=value joined by '&' (the URL standard's serializer). Appends to out. */
void embi_form_serialize(const embi_form *f, embi_sb *out);
void embi_form_free(embi_form *f);

/* ------------------------------------------------------------------ Encoding Standard (encoding.c, sniff.c) */

typedef enum embi_enc_kind {
    EMBI_ENC_UTF8,
    EMBI_ENC_UTF16LE,
    EMBI_ENC_UTF16BE,
    EMBI_ENC_SINGLE,       /* a single-byte table: index holds the code points for 0x80..0xFF */
    EMBI_ENC_USER_DEFINED, /* x-user-defined */
    EMBI_ENC_REPLACEMENT,
    EMBI_ENC_UNSUPPORTED,  /* a label that is recognized but whose decoder is out of scope */
} embi_enc_kind;

typedef struct embi_encoding {
    const char *name;   /* canonical lowercase name, e.g. "windows-1252" */
    const char *labels; /* space-separated labels */
    embi_enc_kind kind;
    const uint16_t *index;
} embi_encoding;

/* The encoding for a label (ASCII whitespace trimmed, case-insensitive), or nullptr when the
 * label is not a WHATWG label. An unsupported encoding is returned with kind
 * EMBI_ENC_UNSUPPORTED, so the caller can report it. */
const embi_encoding *embi_encoding_for_label(embi_str label);
/* Canonical name for a label given as bytes (the css charset path), or nullptr. */
const char *embi_encoding_label_name(const unsigned char *p, size_t n);
/* Encoding by canonical name, or nullptr. */
const embi_encoding *embi_encoding_named(const char *name);

/* Streaming decoder. A zeroed value is not valid: call embi_decoder_init first. */
typedef struct embi_decoder {
    const embi_encoding *enc;
    bool fatal;
    bool ignore_bom;
    bool started;      /* the BOM check is finished */
    bool failed;       /* a fatal error happened: every later call fails */
    uint8_t head[3];   /* bytes held back while the BOM is checked */
    uint8_t head_len;
    /* UTF-8 state */
    uint32_t cp;
    uint8_t needed, seen;
    uint8_t lower, upper;
    /* UTF-16 state */
    int32_t lead_byte;     /* -1 when no byte is pending */
    uint32_t lead_surrogate; /* 0 when none */
    bool replacement_done;
} embi_decoder;

void embi_decoder_init(embi_decoder *d, const embi_encoding *enc, bool fatal, bool ignore_bom);
/* Decodes bytes and appends UTF-8 to out. flush is true for the last call, so an incomplete
 * sequence at the end is reported. Returns false on a fatal error (out then holds what was
 * decoded before it). Without fatal, errors become U+FFFD and the result is always true. */
bool embi_decode(embi_decoder *d, const unsigned char *p, size_t n, bool flush, embi_sb *out);

/* A call with flush true ends the stream whatever it returns: the decoder is then reset for a new
 * stream (BOM state, pending sequences, failed flag), keeping enc, fatal and ignore_bom. After a
 * fatal failure from a call without flush, every call fails until one with flush true. */

/* Encoder (Encoding Standard "encode or fail"). Appends the bytes of the UTF-8 string utf8 in enc
 * to out. Supported: UTF-8 (and UTF-16 and replacement, whose encoder is UTF-8), every
 * single-byte encoding and x-user-defined. Returns false for EMBI_ENC_UNSUPPORTED (nothing
 * appended) and, when html_errors is false, at the first code point enc cannot represent (out
 * then holds the bytes before it). With html_errors true, such a code point becomes "&#N;" in
 * decimal (the HTML form-submission error mode) and the result is true. */
bool embi_encode(const embi_encoding *enc, embi_str utf8, bool html_errors, embi_sb *out);
/* One code point to one byte for a single-byte encoding or x-user-defined (the encoder of the
 * Encoding Standard). Returns false when the code point has no byte. */
bool embi_encode_single(const embi_encoding *enc, uint32_t cp, uint8_t *byte);

/* The encoding sniffing algorithm for a document (WHATWG HTML 13.2.3.2): BOM, then the transport
 * layer label (a Content-Type charset), then a prescan of the first 1024 bytes for a meta
 * charset, then windows-1252. A UTF-16 meta charset becomes UTF-8. Unsupported encodings are
 * never returned: they fall back to windows-1252. transport_label may be empty. */
const embi_encoding *embi_sniff_encoding(const unsigned char *p, size_t n, embi_str transport_label);
/* As embi_sniff_encoding / embi_decode_document, but the last step (no BOM, no usable transport label, no meta
 * charset) yields fallback instead of windows-1252 (nullptr keeps windows-1252). The embedder's document loads pass
 * UTF-8 so a plain UTF-8 file without a meta tag is not read as mojibake. */
const embi_encoding *embi_sniff_encoding_fb(const unsigned char *p, size_t n, embi_str transport_label,
                                            const embi_encoding *fallback);
void embi_decode_document_fb(const unsigned char *p, size_t n, embi_str transport_label,
                             const embi_encoding *fallback, embi_sb *out, const embi_encoding **used);
/* Decodes a whole document to UTF-8 (the BOM is dropped). used receives the encoding chosen. */
void embi_decode_document(const unsigned char *p, size_t n, embi_str transport_label, embi_sb *out,
                          const embi_encoding **used);

/* ------------------------------------------------------------------ MIME Sniffing (mime.c) */

typedef struct embi_mime_param {
    embi_sb name;  /* lowercase */
    embi_sb value; /* as parsed (quotes removed) */
} embi_mime_param;

typedef struct embi_mime {
    embi_sb type;    /* lowercase */
    embi_sb subtype; /* lowercase */
    EMBI_VEC(embi_mime_param) params;
} embi_mime;

/* Parses a MIME type (MIME Sniffing 4.2). Returns false on failure. out must be zeroed, and it is
 * freed by embi_mime_free. */
bool embi_mime_parse(embi_str input, embi_mime *out);
void embi_mime_free(embi_mime *m);
/* Serializes (type/subtype plus parameters, quoted where needed). Appends to out. */
void embi_mime_serialize(const embi_mime *m, embi_sb *out);
/* Essence: "type/subtype", lowercase, without parameters. Appends to out. */
void embi_mime_essence(const embi_mime *m, embi_sb *out);
/* The value of a parameter, or {nullptr, 0} when there is none. */
embi_str embi_mime_param_value(const embi_mime *m, embi_str name);
/* Whether an essence is a JavaScript MIME type (the HTML script essence list). */
bool embi_mime_essence_is_javascript(embi_str essence);
/* Whether an essence is a JSON MIME type (MIME Sniffing): application/json, text/json, or any
 * type/subtype whose subtype ends in "+json" (text/+json too). ASCII case-insensitive. */
bool embi_mime_essence_is_json(embi_str essence);
/* The MIME type for a path's extension (case-insensitive) from the small table in mime.c, or
 * {nullptr, 0} when the extension is not in the table. The path's query and fragment are ignored. */
embi_str embi_mime_from_path(embi_str path);

/* ------------------------------------------------------------------ data: URLs (data.c) */

/* The Fetch standard's data: URL processor. url is an absolute URL string. The serialized MIME
 * type (default text/plain;charset=US-ASCII) is appended to mime_out and the decoded body to
 * body_out. Returns false when the URL is not a data: URL or the body cannot be decoded. */
bool embi_data_url(embi_str url, embi_sb *mime_out, embi_sb *body_out);
