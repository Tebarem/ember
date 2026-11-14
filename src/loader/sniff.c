/*
 * Document encoding sniffing (WHATWG HTML 13.2.3.2) for the parser's byte input: BOM, transport
 * layer charset, prescan of the first 1024 bytes, then windows-1252. This is the input decoding
 * step only. The HTML tokenizer still receives UTF-8.
 */
#include "loader/loader.h"

#include <string.h>

#define PRESCAN_LIMIT 1024u

static bool pre_ws(unsigned char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static unsigned char pre_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool pre_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ASCII case-insensitive match of lit at p[pos] within lim bytes. */
static bool ci_at(const unsigned char *p, size_t lim, size_t pos, const char *lit)
{
    size_t n = strlen(lit);
    if (pos + n > lim) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (pre_lower(p[pos + i]) != pre_lower((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

/* An attribute as the prescan sees it: name and value slices of the input. */
typedef struct pre_attr {
    const unsigned char *name;
    size_t name_len;
    const unsigned char *value;
    size_t value_len;
} pre_attr;

/* WHATWG "get an attribute" for the prescan. Returns false at the end of the tag. */
static bool pre_get_attribute(const unsigned char *p, size_t lim, size_t *pos, pre_attr *a)
{
    size_t i = *pos;
    while (i < lim && (pre_ws(p[i]) || p[i] == '/')) {
        i++;
    }
    if (i >= lim || p[i] == '>') {
        *pos = i;
        return false;
    }
    size_t ns = i;
    /* The first character of a name may be '='. */
    i++;
    while (i < lim && !pre_ws(p[i]) && p[i] != '/' && p[i] != '=' && p[i] != '>') {
        i++;
    }
    a->name = p + ns;
    a->name_len = i - ns;
    while (i < lim && pre_ws(p[i])) {
        i++;
    }
    a->value = p + i;
    a->value_len = 0;
    if (i < lim && p[i] == '=') {
        i++;
        while (i < lim && pre_ws(p[i])) {
            i++;
        }
        if (i < lim && (p[i] == '"' || p[i] == '\'')) {
            unsigned char q = p[i];
            size_t vs = ++i;
            while (i < lim && p[i] != q) {
                i++;
            }
            a->value = p + vs;
            a->value_len = i - vs;
            if (i < lim) {
                i++;
            }
        } else if (i < lim && p[i] != '>') {
            size_t vs = i;
            while (i < lim && !pre_ws(p[i]) && p[i] != '>') {
                i++;
            }
            a->value = p + vs;
            a->value_len = i - vs;
        }
    }
    *pos = i;
    return true;
}

/* WHATWG "extract a character encoding from a meta element": finds charset= in a content value.
 * Returns the label slice in *out, or false. */
static bool pre_extract_charset(const unsigned char *v, size_t n, const unsigned char **out, size_t *outn)
{
    for (size_t i = 0; i + 7 <= n; i++) {
        if (!ci_at(v, n, i, "charset")) {
            continue;
        }
        size_t j = i + 7;
        while (j < n && pre_ws(v[j])) {
            j++;
        }
        if (j >= n || v[j] != '=') {
            continue;
        }
        j++;
        while (j < n && pre_ws(v[j])) {
            j++;
        }
        if (j >= n) {
            return false;
        }
        if (v[j] == '"' || v[j] == '\'') {
            unsigned char q = v[j];
            size_t s = ++j;
            while (j < n && v[j] != q) {
                j++;
            }
            if (j >= n) {
                return false;
            }
            *out = v + s;
            *outn = j - s;
            return *outn > 0;
        }
        size_t s = j;
        while (j < n && !pre_ws(v[j]) && v[j] != ';') {
            j++;
        }
        *out = v + s;
        *outn = j - s;
        return *outn > 0;
    }
    return false;
}

/* WHATWG prescan a byte stream: the encoding named by a meta element in the first 1024 bytes, or
 * nullptr. The UTF-16 labels become UTF-8, and x-user-defined becomes windows-1252. Labels the
 * decoder does not support return windows-1252 too, so the caller never sees them. */
static const embi_encoding *prescan(const unsigned char *p, size_t n)
{
    size_t lim = n < PRESCAN_LIMIT ? n : PRESCAN_LIMIT;
    size_t pos = 0;
    while (pos < lim) {
        if (ci_at(p, lim, pos, "<!--")) {
            /* The dashes of "<!--" can be reused: "<!-->" is a whole comment. */
            pos += 2;
            while (pos < lim && !(pos + 3 <= lim && p[pos] == '-' && p[pos + 1] == '-' && p[pos + 2] == '>')) {
                pos++;
            }
            pos = pos + 3 <= lim ? pos + 3 : lim;
            continue;
        }
        if (ci_at(p, lim, pos, "<meta") && pos + 5 < lim && (pre_ws(p[pos + 5]) || p[pos + 5] == '/')) {
            pos += 5;
            bool got_pragma = false;
            int need_pragma = -1; /* -1 no charset yet, 0 from charset=, 1 from content= */
            const embi_encoding *e = nullptr; /* resolved when read, null while none or unknown */
            bool seen_equiv = false;
            bool seen_content = false;
            bool seen_charset = false;
            pre_attr a = {nullptr, 0, nullptr, 0};
            while (pre_get_attribute(p, lim, &pos, &a)) {
                embi_str name = {(const char *)a.name, (uint32_t)a.name_len};
                embi_str value = {(const char *)a.value, (uint32_t)a.value_len};
                /* A repeated attribute name is ignored. */
                if (embi_str_ieq(name, EMBI_STR("http-equiv")) && !seen_equiv) {
                    seen_equiv = true;
                    if (embi_str_ieq(value, EMBI_STR("content-type"))) {
                        got_pragma = true;
                    }
                } else if (embi_str_ieq(name, EMBI_STR("content")) && !seen_content) {
                    seen_content = true;
                    const unsigned char *cs = nullptr;
                    size_t cs_len = 0;
                    if (e == nullptr && !seen_charset && pre_extract_charset(a.value, a.value_len, &cs, &cs_len)) {
                        /* A label that names no encoding does not count as found. */
                        const embi_encoding *found = embi_encoding_for_label((embi_str){(const char *)cs, (uint32_t)cs_len});
                        if (found != nullptr) {
                            e = found;
                            need_pragma = 1;
                        }
                    }
                } else if (embi_str_ieq(name, EMBI_STR("charset")) && !seen_charset) {
                    /* A charset attribute overrides one found in content, even when its label
                     * names no encoding (then the element is skipped). */
                    seen_charset = true;
                    e = embi_encoding_for_label(value);
                    need_pragma = 0;
                }
            }
            if (need_pragma < 0 || e == nullptr) {
                continue;
            }
            if (need_pragma == 1 && !got_pragma) {
                continue;
            }
            if (e->kind == EMBI_ENC_UTF16LE || e->kind == EMBI_ENC_UTF16BE) {
                return embi_encoding_named("utf-8");
            }
            if (e->kind == EMBI_ENC_USER_DEFINED) {
                return embi_encoding_named("windows-1252");
            }
            if (e->kind == EMBI_ENC_UNSUPPORTED) {
                return embi_encoding_named("windows-1252");
            }
            return e;
        }
        if (p[pos] == '<' && pos + 1 < lim && (pre_alpha(p[pos + 1]) || (p[pos + 1] == '/' && pos + 2 < lim && pre_alpha(p[pos + 2])))) {
            /* Any other tag: its attributes are consumed so a quoted "<meta" is not seen. */
            pos += p[pos + 1] == '/' ? 2 : 1;
            while (pos < lim && !pre_ws(p[pos]) && p[pos] != '/' && p[pos] != '>') {
                pos++;
            }
            pre_attr skip = {nullptr, 0, nullptr, 0};
            while (pre_get_attribute(p, lim, &pos, &skip)) {
            }
            pos++;
            continue;
        }
        if (p[pos] == '<' && pos + 1 < lim && (p[pos + 1] == '!' || p[pos + 1] == '?' || p[pos + 1] == '/')) {
            /* Comments, declarations and end tags: skip to the closing '>'. */
            pos += 2;
            while (pos < lim && p[pos] != '>') {
                pos++;
            }
            pos++;
            continue;
        }
        pos++;
    }
    return nullptr;
}

/* A transport label is used only when the decoder supports it. *unsupported is set when the label
 * is recognized but has no decoder (the CJK multi-byte encodings). */
static const embi_encoding *usable_label(embi_str label, bool *unsupported)
{
    *unsupported = false;
    if (label.len == 0) {
        return nullptr;
    }
    const embi_encoding *e = embi_encoding_for_label(label);
    if (e == nullptr) {
        return nullptr;
    }
    if (e->kind == EMBI_ENC_UNSUPPORTED) {
        *unsupported = true;
        return nullptr;
    }
    return e;
}

const embi_encoding *embi_sniff_encoding_fb(const unsigned char *p, size_t n, embi_str transport_label,
                                            const embi_encoding *fallback)
{
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        return embi_encoding_named("utf-8");
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        return embi_encoding_named("utf-16le");
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        return embi_encoding_named("utf-16be");
    }
    bool unsupported;
    const embi_encoding *e = usable_label(transport_label, &unsupported);
    if (e != nullptr) {
        return e;
    }
    if (unsupported) {
        /* A recognized but unsupported (CJK) transport charset falls back to windows-1252. */
        return embi_encoding_named("windows-1252");
    }
    e = prescan(p, n);
    if (e != nullptr) {
        return e;
    }
    return fallback != nullptr ? fallback : embi_encoding_named("windows-1252");
}

const embi_encoding *embi_sniff_encoding(const unsigned char *p, size_t n, embi_str transport_label)
{
    return embi_sniff_encoding_fb(p, n, transport_label, nullptr);
}

void embi_decode_document_fb(const unsigned char *p, size_t n, embi_str transport_label,
                             const embi_encoding *fallback, embi_sb *out, const embi_encoding **used)
{
    const embi_encoding *e = embi_sniff_encoding_fb(p, n, transport_label, fallback);
    if (used != nullptr) {
        *used = e;
    }
    embi_decoder d;
    embi_decoder_init(&d, e, false, false);
    (void)embi_decode(&d, p, n, true, out);
}

void embi_decode_document(const unsigned char *p, size_t n, embi_str transport_label, embi_sb *out,
                          const embi_encoding **used)
{
    const embi_encoding *e = embi_sniff_encoding(p, n, transport_label);
    if (used != nullptr) {
        *used = e;
    }
    embi_decoder d;
    embi_decoder_init(&d, e, false, false);
    /* A non-fatal decoder never fails. */
    (void)embi_decode(&d, p, n, true, out);
}
