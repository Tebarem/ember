#include "dom/internal.h"

/* ------------------------------------------------------------------ namespaces */

static const char *const g_ns_uris[EMBI_NS_XMLNS + 1] = {
    "",
    "http://www.w3.org/1999/xhtml",
    "http://www.w3.org/2000/svg",
    "http://www.w3.org/1998/Math/MathML",
    "http://www.w3.org/1999/xlink",
    "http://www.w3.org/XML/1998/namespace",
    "http://www.w3.org/2000/xmlns/",
};

embi_str embi_dom_ns_uri(uint8_t ns, embi_atom ns_uri)
{
    if (ns == EMBI_NS_OTHER) {
        return embi_atom_str(ns_uri);
    }
    if (ns > EMBI_NS_XMLNS) {
        return EMBI_STR("");
    }
    embi_str s = {g_ns_uris[ns], (uint32_t)SDL_strlen(g_ns_uris[ns])};
    return s;
}

uint8_t embi_dom_ns_from_uri(embi_str uri)
{
    if (uri.len == 0) {
        return EMBI_NS_NULL;
    }
    for (uint8_t ns = EMBI_NS_HTML; ns <= EMBI_NS_XMLNS; ns++) {
        embi_str known = {g_ns_uris[ns], (uint32_t)SDL_strlen(g_ns_uris[ns])};
        if (embi_str_eq(uri, known)) {
            return ns;
        }
    }
    return EMBI_NS_OTHER;
}

uint8_t embi_dom_ns_arg(bool has_ns, embi_str uri, embi_atom *uri_atom)
{
    *uri_atom = 0;
    if (!has_ns || uri.len == 0) {
        return EMBI_NS_NULL;
    }
    uint8_t ns = embi_dom_ns_from_uri(uri);
    if (ns == EMBI_NS_OTHER) {
        *uri_atom = embi_atom_intern(uri.ptr, uri.len);
    }
    return ns;
}

bool embi_dom_ns_arg_find(bool has_ns, embi_str uri, uint8_t *ns, embi_atom *uri_atom)
{
    *uri_atom = 0;
    *ns = EMBI_NS_NULL;
    if (!has_ns || uri.len == 0) {
        return true;
    }
    *ns = embi_dom_ns_from_uri(uri);
    if (*ns == EMBI_NS_OTHER) {
        *uri_atom = embi_atom_find(uri.ptr, uri.len);
        return *uri_atom != 0;
    }
    return true;
}

embi_dom_err embi_dom_check_ns(uint8_t ns, embi_str qname, embi_str prefix)
{
    bool prefix_is_xmlns = embi_str_eq(prefix, EMBI_STR("xmlns"));
    bool qname_is_xmlns = embi_str_eq(qname, EMBI_STR("xmlns"));
    if (prefix.len && ns == EMBI_NS_NULL) {
        return EMBI_DOM_ERR_NAMESPACE;
    }
    if (embi_str_eq(prefix, EMBI_STR("xml")) && ns != EMBI_NS_XML) {
        return EMBI_DOM_ERR_NAMESPACE;
    }
    if ((prefix_is_xmlns || qname_is_xmlns) && ns != EMBI_NS_XMLNS) {
        return EMBI_DOM_ERR_NAMESPACE;
    }
    if (ns == EMBI_NS_XMLNS && !(prefix_is_xmlns || qname_is_xmlns)) {
        return EMBI_DOM_ERR_NAMESPACE;
    }
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ name productions */

static bool name_start_char(uint32_t c, bool colon)
{
    if (c == ':') {
        return colon;
    }
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xC0u && c <= 0xD6u) || (c >= 0xD8u && c <= 0xF6u) || (c >= 0xF8u && c <= 0x2FFu) ||
           (c >= 0x370u && c <= 0x37Du) || (c >= 0x37Fu && c <= 0x1FFFu) ||
           (c >= 0x200Cu && c <= 0x200Du) || (c >= 0x2070u && c <= 0x218Fu) ||
           (c >= 0x2C00u && c <= 0x2FEFu) || (c >= 0x3001u && c <= 0xD7FFu) ||
           (c >= 0xF900u && c <= 0xFDCFu) || (c >= 0xFDF0u && c <= 0xFFFDu) ||
           (c >= 0x10000u && c <= 0xEFFFFu);
}

static bool name_char(uint32_t c, bool colon)
{
    return name_start_char(c, colon) || c == '-' || c == '.' || (c >= '0' && c <= '9') ||
           c == 0xB7u || (c >= 0x300u && c <= 0x36Fu) || (c >= 0x203Fu && c <= 0x2040u);
}

/* Decodes one code point. Malformed input (which embi_utf8_decode reports as U+FFFD) yields
 * UINT32_MAX, which is in no name range. A genuine U+FFFD (EF BF BD) is kept. */
static uint32_t decode_strict(const char **pp, const char *end)
{
    const char *start = *pp;
    uint32_t c = embi_utf8_decode(pp, end);
    if (c == 0xFFFDu) {
        bool genuine = *pp - start == 3 && (unsigned char)start[0] == 0xEFu &&
                       (unsigned char)start[1] == 0xBFu && (unsigned char)start[2] == 0xBDu;
        if (!genuine) {
            return UINT32_MAX;
        }
    }
    return c;
}

bool embi_dom_is_name(embi_str s, bool allow_colon)
{
    if (s.len == 0) {
        return false;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    if (!name_start_char(decode_strict(&p, end), allow_colon)) {
        return false;
    }
    while (p < end) {
        if (!name_char(decode_strict(&p, end), allow_colon)) {
            return false;
        }
    }
    return true;
}

static bool is_ascii_ws_cp(uint32_t c)
{
    return c == 0x9 || c == 0xA || c == 0xC || c == 0xD || c == 0x20;
}

/* The DOM Standard's "valid element local name": [A-Za-z][^\0\t\n\f\r /> ]* or [:_\u0080-][A-Za-z0-9-.:_\u0080-]*. */
bool embi_dom_valid_element_name(embi_str s)
{
    if (s.len == 0) {
        return false;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    uint32_t c = decode_strict(&p, end);
    if (c == UINT32_MAX) {
        return false;
    }
    bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (alpha) {
        while (p < end) {
            c = decode_strict(&p, end);
            if (c == UINT32_MAX || c == 0 || is_ascii_ws_cp(c) || c == '/' || c == '>') {
                return false;
            }
        }
        return true;
    }
    if (c != ':' && c != '_' && c < 0x80) {
        return false;
    }
    while (p < end) {
        c = decode_strict(&p, end);
        if (c == UINT32_MAX) {
            return false;
        }
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '.' || c == ':' || c == '_' || c >= 0x80;
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* "valid attribute local name": not empty, none of NUL, ASCII whitespace, '/', '>', '='. */
bool embi_dom_valid_attr_name(embi_str s)
{
    if (s.len == 0) {
        return false;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        uint32_t c = decode_strict(&p, end);
        if (c == UINT32_MAX || c == 0 || is_ascii_ws_cp(c) || c == '/' || c == '>' || c == '=') {
            return false;
        }
    }
    return true;
}

/* "valid namespace prefix": not empty, none of NUL, ASCII whitespace, '/', '>'. */
static bool valid_prefix(embi_str s)
{
    if (s.len == 0) {
        return false;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        uint32_t c = decode_strict(&p, end);
        if (c == UINT32_MAX || c == 0 || is_ascii_ws_cp(c) || c == '/' || c == '>') {
            return false;
        }
    }
    return true;
}

/* "valid doctype name": no NUL, ASCII whitespace or '>' (the empty string is valid). */
bool embi_dom_valid_doctype_name(embi_str s)
{
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        uint32_t c = decode_strict(&p, end);
        if (c == UINT32_MAX || c == 0 || is_ascii_ws_cp(c) || c == '>') {
            return false;
        }
    }
    return true;
}

bool embi_dom_split_qname(embi_str q, bool attr, embi_str *prefix, embi_str *local)
{
    uint32_t pos = q.len;
    for (uint32_t i = 0; i < q.len; i++) {
        if (q.ptr[i] == ':') {
            pos = i;
            break;
        }
    }
    embi_str p = {nullptr, 0};
    embi_str l = q;
    if (pos < q.len) {
        p = (embi_str){q.ptr, pos};
        l = (embi_str){q.ptr + pos + 1, q.len - pos - 1};
        if (!valid_prefix(p)) {
            return false;
        }
    }
    if (attr ? !embi_dom_valid_attr_name(l) : !embi_dom_valid_element_name(l)) {
        return false;
    }
    *prefix = p;
    *local = l;
    return true;
}

/* ------------------------------------------------------------------ validated creation */

embi_dom_err embi_dom_create_element(embi_document *doc, embi_str local_name, embi_element **out)
{
    if (!embi_dom_valid_element_name(local_name)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_sb sb = {nullptr, 0, 0};
    for (uint32_t i = 0; i < local_name.len; i++) {
        char c = local_name.ptr[i];
        if (doc->is_html && c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        embi_sb_append_char(&sb, c);
    }
    embi_atom local = embi_atom_intern(sb.data, sb.len);
    embi_sb_free(&sb);
    uint8_t ns = (doc->is_html || doc->ctype == EMBI_CT_XHTML) ? EMBI_NS_HTML : EMBI_NS_NULL;
    *out = embi_new_element(doc, ns, 0, 0, local);
    return EMBI_DOM_OK;
}

embi_dom_err embi_dom_create_element_ns(embi_document *doc, bool has_ns, embi_str ns_uri,
                                        embi_str qualified_name, embi_element **out)
{
    embi_str prefix;
    embi_str local;
    if (!embi_dom_split_qname(qualified_name, false, &prefix, &local)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_atom uri_atom;
    uint8_t ns = embi_dom_ns_arg(has_ns, ns_uri, &uri_atom);
    embi_dom_err err = embi_dom_check_ns(ns, qualified_name, prefix);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    embi_atom prefix_atom = prefix.len ? embi_atom_intern(prefix.ptr, prefix.len) : 0;
    embi_atom local_atom = embi_atom_intern(local.ptr, local.len);
    *out = embi_new_element(doc, ns, uri_atom, prefix_atom, local_atom);
    return EMBI_DOM_OK;
}

embi_dom_err embi_dom_create_pi(embi_document *doc, embi_str target, embi_dstr data,
                                embi_chardata **out)
{
    if (!embi_dom_is_name(target, true)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    for (uint32_t i = 0; i + 1 < data.len; i++) {
        if (embi_dstr_unit(data, i) == '?' && embi_dstr_unit(data, i + 1) == '>') {
            return EMBI_DOM_ERR_INVALID_CHAR;
        }
    }
    embi_atom atom = embi_atom_intern(target.ptr, target.len);
    *out = embi_new_pi(doc, atom, data);
    return EMBI_DOM_OK;
}

embi_dom_err embi_dom_create_doctype(embi_document *doc, embi_str name, embi_str public_id,
                                     embi_str system_id, embi_doctype **out)
{
    if (!embi_dom_valid_doctype_name(name)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_dstr n = embi_dstr_from_utf8(doc->slab, name);
    embi_dstr p = embi_dstr_from_utf8(doc->slab, public_id);
    embi_dstr s = embi_dstr_from_utf8(doc->slab, system_id);
    *out = embi_new_doctype(doc, n, p, s);
    embi_dstr_free(n);
    embi_dstr_free(p);
    embi_dstr_free(s);
    return EMBI_DOM_OK;
}

embi_dom_err embi_dom_create_attr(embi_document *doc, embi_str local_name, embi_attr_node **out)
{
    if (!embi_dom_valid_attr_name(local_name)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_sb sb = {nullptr, 0, 0};
    for (uint32_t i = 0; i < local_name.len; i++) {
        char c = local_name.ptr[i];
        if (doc->is_html && c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        embi_sb_append_char(&sb, c);
    }
    embi_atom local = embi_atom_intern(sb.data, sb.len);
    embi_sb_free(&sb);
    *out = embi_new_attr_node(doc, EMBI_NS_NULL, 0, 0, local, embi_dstr_empty());
    return EMBI_DOM_OK;
}

embi_dom_err embi_dom_create_attr_ns(embi_document *doc, bool has_ns, embi_str ns_uri, embi_str qualified_name,
                                     embi_attr_node **out)
{
    embi_str prefix;
    embi_str local;
    if (!embi_dom_split_qname(qualified_name, true, &prefix, &local)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_atom uri_atom;
    uint8_t ns = embi_dom_ns_arg(has_ns, ns_uri, &uri_atom);
    embi_dom_err err = embi_dom_check_ns(ns, qualified_name, prefix);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    embi_atom prefix_atom = prefix.len ? embi_atom_intern(prefix.ptr, prefix.len) : 0;
    embi_atom local_atom = embi_atom_intern(local.ptr, local.len);
    *out = embi_new_attr_node(doc, ns, uri_atom, prefix_atom, local_atom, embi_dstr_empty());
    return EMBI_DOM_OK;
}
