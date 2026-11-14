/*
 * DOMParser (W5f): parseFromString for text/html (the HTML parser, scripting off) and for the XML types (a small
 * non-validating XML parser: elements, attributes, namespaces, text, character references and the five predefined
 * entities, comments, CDATA sections, processing instructions and a DOCTYPE). A document that is not well-formed becomes a
 * document with a <parsererror> root, like Firefox's.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "html/parser.h"

typedef struct xml_ns_decl {
    embi_atom prefix; /* 0: the default namespace */
    embi_atom uri;    /* 0: undeclared (xmlns="") */
} xml_ns_decl;

typedef struct xml_p {
    embi_document *doc;
    const char *s, *end, *p;
    EMBI_VEC(xml_ns_decl) ns;
    bool ok;
} xml_p;

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(xml_p *x)
{
    while (x->p < x->end && is_ws(*x->p)) {
        x->p++;
    }
}

static bool starts(xml_p *x, const char *lit)
{
    size_t n = SDL_strlen(lit);
    return (size_t)(x->end - x->p) >= n && SDL_memcmp(x->p, lit, n) == 0;
}

static bool name_start(unsigned char c)
{
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
}

static bool name_char(unsigned char c)
{
    return name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9');
}

static embi_str read_name(xml_p *x)
{
    const char *b = x->p;
    if (x->p < x->end && name_start((unsigned char)*x->p)) {
        x->p++;
        while (x->p < x->end && name_char((unsigned char)*x->p)) {
            x->p++;
        }
    }
    return (embi_str){b, (uint32_t)(x->p - b)};
}

/* Appends the text of [b, e) with references resolved; false for an unknown reference. */
static bool decode_text(const char *b, const char *e, embi_sb *out)
{
    while (b < e) {
        if (*b != '&') {
            embi_sb_append_char(out, *b++);
            continue;
        }
        const char *semi = b;
        while (semi < e && *semi != ';') {
            semi++;
        }
        if (semi == e) {
            return false;
        }
        embi_str ref = {b + 1, (uint32_t)(semi - b - 1)};
        if (embi_str_eq(ref, EMBI_STR("lt"))) {
            embi_sb_append_char(out, '<');
        } else if (embi_str_eq(ref, EMBI_STR("gt"))) {
            embi_sb_append_char(out, '>');
        } else if (embi_str_eq(ref, EMBI_STR("amp"))) {
            embi_sb_append_char(out, '&');
        } else if (embi_str_eq(ref, EMBI_STR("quot"))) {
            embi_sb_append_char(out, '"');
        } else if (embi_str_eq(ref, EMBI_STR("apos"))) {
            embi_sb_append_char(out, '\'');
        } else if (ref.len > 1 && ref.ptr[0] == '#') {
            uint32_t cp = 0;
            bool hex = ref.ptr[1] == 'x';
            for (uint32_t i = hex ? 2u : 1u; i < ref.len; i++) {
                char ch = ref.ptr[i];
                int d = ch >= '0' && ch <= '9' ? ch - '0' : hex && ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                        : hex && ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (d < 0 || cp > 0x10FFFF) {
                    return false;
                }
                cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
            }
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            embi_sb_append_cp(out, cp);
        } else {
            return false;
        }
        b = semi + 1;
    }
    return true;
}

static embi_atom lookup_ns(xml_p *x, embi_atom prefix)
{
    for (uint32_t i = x->ns.len; i > 0; i--) {
        if (x->ns.data[i - 1].prefix == prefix) {
            return x->ns.data[i - 1].uri;
        }
    }
    return 0;
}

static void add_text(xml_p *x, embi_node *parent, const char *b, const char *e, bool decode)
{
    if (b == e) {
        return;
    }
    embi_sb sb = {nullptr, 0, 0};
    if (decode) {
        if (!decode_text(b, e, &sb)) {
            x->ok = false;
        }
    } else {
        embi_sb_append(&sb, (embi_str){b, (uint32_t)(e - b)});
    }
    if (x->ok && sb.len) {
        embi_chardata *t = embi_new_text_utf8(x->doc, (embi_str){sb.data, sb.len});
        (void)embi_append_child(parent, &t->base);
    }
    embi_sb_free(&sb);
}

typedef struct xml_attr {
    embi_str name;
    embi_sb value;
} xml_attr;

/* A comment may not contain "--" and may not end with "-" (XML 1.0, section 2.5). */
static bool comment_ok(const char *b, const char *e)
{
    for (const char *q = b; q < e; q++) {
        if (q[0] == '-' && (q + 1 == e || q[1] == '-')) {
            return false;
        }
    }
    return true;
}

/* The start tag: name, attributes (kept on the heap), namespace declarations, and the new element appended to parent.
 * Kept out of line so that the recursive parse_element has a small C frame. Returns nullptr with x->ok cleared on
 * error. */
[[gnu::noinline]] static embi_element *parse_start_tag(xml_p *x, embi_node *parent, embi_str *qn_out, bool *self_close_out)
{
    x->p++; /* '<' */
    embi_str qn = read_name(x);
    *qn_out = qn;
    if (qn.len == 0) {
        x->ok = false;
        return nullptr;
    }
    /* attributes live on the heap so a deep element chain costs only a small C frame per level */
    EMBI_VEC(xml_attr) attrs = {};
    uint32_t na = 0;
    bool self_close = false;
    for (;;) {
        bool had_ws = x->p < x->end && is_ws(*x->p);
        skip_ws(x);
        if (x->p >= x->end) {
            x->ok = false;
            break;
        }
        if (*x->p == '>') {
            x->p++;
            break;
        }
        if (*x->p == '/' && x->p + 1 < x->end && x->p[1] == '>') {
            x->p += 2;
            self_close = true;
            break;
        }
        if (!had_ws) {
            x->ok = false;
            break;
        }
        embi_str an = read_name(x);
        skip_ws(x);
        if (an.len == 0 || x->p >= x->end || *x->p != '=') {
            x->ok = false;
            break;
        }
        x->p++;
        skip_ws(x);
        if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) {
            x->ok = false;
            break;
        }
        char q = *x->p++;
        const char *vb = x->p;
        while (x->p < x->end && *x->p != q && *x->p != '<') {
            x->p++;
        }
        if (x->p >= x->end || *x->p != q) {
            x->ok = false;
            break;
        }
        xml_attr na_ = {an, {nullptr, 0, 0}};
        if (!decode_text(vb, x->p, &na_.value)) {
            x->ok = false;
        }
        for (uint32_t i = 0; i < na; i++) {
            if (embi_str_eq(attrs.data[i].name, an)) {
                x->ok = false;
            }
        }
        embi_vec_push(&attrs, na_);
        na++;
        x->p++;
    }
    embi_element *el = nullptr;
    if (x->ok) {
        /* namespace declarations first */
        for (uint32_t i = 0; i < na; i++) {
            embi_str an = attrs.data[i].name;
            if (embi_str_eq(an, EMBI_STR("xmlns"))) {
                xml_ns_decl d = {0, attrs.data[i].value.len ? embi_atom_intern(attrs.data[i].value.data, attrs.data[i].value.len) : 0};
                embi_vec_push(&x->ns, d);
            } else if (an.len > 6 && SDL_memcmp(an.ptr, "xmlns:", 6) == 0) {
                xml_ns_decl d = {embi_atom_intern(an.ptr + 6, an.len - 6),
                                 attrs.data[i].value.len ? embi_atom_intern(attrs.data[i].value.data, attrs.data[i].value.len) : 0};
                embi_vec_push(&x->ns, d);
            }
        }
        embi_str prefix = {nullptr, 0};
        embi_str local = qn;
        for (uint32_t i = 0; i < qn.len; i++) {
            if (qn.ptr[i] == ':') {
                prefix = (embi_str){qn.ptr, i};
                local = (embi_str){qn.ptr + i + 1, qn.len - i - 1};
                break;
            }
        }
        embi_atom pa = prefix.len ? embi_atom_intern(prefix.ptr, prefix.len) : 0;
        embi_atom uri = lookup_ns(x, pa);
        if (prefix.len && uri == 0) {
            x->ok = false;
        } else if (local.len == 0) {
            x->ok = false;
        } else {
            embi_str us = uri ? embi_atom_str(uri) : EMBI_STR("");
            uint8_t ns = embi_dom_ns_from_uri(us);
            el = embi_new_element(x->doc, ns, ns == EMBI_NS_OTHER ? uri : 0, pa, embi_atom_intern(local.ptr, local.len));
            (void)embi_append_child(parent, &el->base);
            for (uint32_t i = 0; i < na; i++) {
                embi_str an = attrs.data[i].name;
                embi_str ap = {nullptr, 0};
                for (uint32_t k = 0; k < an.len; k++) {
                    if (an.ptr[k] == ':') {
                        ap = (embi_str){an.ptr, k};
                        break;
                    }
                }
                bool is_xmlns = embi_str_eq(an, EMBI_STR("xmlns")) || embi_str_eq(ap, EMBI_STR("xmlns"));
                embi_str auri = EMBI_STR("");
                if (is_xmlns) {
                    auri = EMBI_STR("http://www.w3.org/2000/xmlns/");
                } else if (embi_str_eq(ap, EMBI_STR("xml"))) {
                    auri = EMBI_STR("http://www.w3.org/XML/1998/namespace");
                } else if (ap.len) {
                    embi_atom u = lookup_ns(x, embi_atom_intern(ap.ptr, ap.len));
                    if (!u) {
                        x->ok = false;
                        break;
                    }
                    auri = embi_atom_str(u);
                }
                embi_dstr v = embi_dstr_from_utf8(x->doc->slab, (embi_str){attrs.data[i].value.data, attrs.data[i].value.len});
                embi_dom_err e = embi_el_set_attribute_ns(el, auri.len != 0, auri, an, v);
                embi_dstr_free(v);
                if (e != EMBI_DOM_OK) {
                    x->ok = false;
                    break;
                }
            }
        }
    }
    for (uint32_t i = 0; i < na; i++) {
        embi_sb_free(&attrs.data[i].value);
    }
    embi_vec_free(&attrs);
    *self_close_out = self_close;
    return x->ok ? el : nullptr;
}

/* A comment, CDATA section or processing instruction inside an element (x->p is at its "<"). */
[[gnu::noinline]] static void parse_misc(xml_p *x, embi_element *el)
{
    if (starts(x, "<!--")) {
        const char *b = x->p + 4;
        const char *e = b;
        while (e + 2 < x->end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) {
            e++;
        }
        if (e + 2 >= x->end || !comment_ok(b, e)) {
            x->ok = false;
        } else {
            embi_dstr d = embi_dstr_from_utf8(x->doc->slab, (embi_str){b, (uint32_t)(e - b)});
            (void)embi_append_child(&el->base, &embi_new_comment(x->doc, d)->base);
            embi_dstr_free(d);
            x->p = e + 3;
        }
    } else if (starts(x, "<![CDATA[")) {
        const char *b = x->p + 9;
        const char *e = b;
        while (e + 2 < x->end && !(e[0] == ']' && e[1] == ']' && e[2] == '>')) {
            e++;
        }
        if (e + 2 >= x->end) {
            x->ok = false;
        } else {
            embi_chardata *cd = embi_new_text_utf8(x->doc, (embi_str){b, (uint32_t)(e - b)});
            cd->base.flags |= EMBI_NF_CDATA;
            (void)embi_append_child(&el->base, &cd->base);
            x->p = e + 3;
        }
    } else if (starts(x, "<?")) {
        x->p += 2;
        embi_str tg = read_name(x);
        const char *b = x->p;
        const char *e = b;
        while (e + 1 < x->end && !(e[0] == '?' && e[1] == '>')) {
            e++;
        }
        if (e + 1 >= x->end || tg.len == 0) {
            x->ok = false;
        } else {
            while (b < e && is_ws(*b)) {
                b++;
            }
            embi_dstr d = embi_dstr_from_utf8(x->doc->slab, (embi_str){b, (uint32_t)(e - b)});
            (void)embi_append_child(&el->base,
                                    &embi_new_pi(x->doc, embi_atom_intern(tg.ptr, tg.len), d)->base);
            embi_dstr_free(d);
            x->p = e + 2;
        }
    }
}

static void parse_element(xml_p *x, embi_node *parent, int depth)
{
    if (depth > 256) {
        x->ok = false;
        return;
    }
    uint32_t ns_mark = x->ns.len;
    embi_str qn = {nullptr, 0};
    bool self_close = false;
    embi_element *el = parse_start_tag(x, parent, &qn, &self_close);
    if (el && !self_close) {
        for (;;) {
            if (x->p >= x->end) {
                x->ok = false;
                break;
            }
            if (*x->p != '<') {
                const char *b = x->p;
                while (x->p < x->end && *x->p != '<') {
                    x->p++;
                }
                add_text(x, &el->base, b, x->p, true);
            } else if (starts(x, "</")) {
                x->p += 2;
                embi_str en = read_name(x);
                skip_ws(x);
                if (!embi_str_eq(en, qn) || x->p >= x->end || *x->p != '>') {
                    x->ok = false;
                } else {
                    x->p++;
                }
                break;
            } else if (starts(x, "<!--") || starts(x, "<![CDATA[") || starts(x, "<?")) {
                parse_misc(x, el);
            } else {
                parse_element(x, &el->base, depth + 1);
            }
            if (!x->ok) {
                break;
            }
        }
    }
    x->ns.len = ns_mark;
}

static bool xml_parse(embi_document *doc, embi_str text)
{
    xml_p x = {.doc = doc, .s = text.ptr, .end = text.ptr + text.len, .p = text.ptr, .ok = true};
    if (text.len >= 3 && (unsigned char)text.ptr[0] == 0xEF && (unsigned char)text.ptr[1] == 0xBB &&
        (unsigned char)text.ptr[2] == 0xBF) {
        x.p += 3;
    }
    bool have_root = false;
    if (starts(&x, "<?xml") && x.p + 5 < x.end && is_ws(x.p[5])) {
        while (x.p + 1 < x.end && !(x.p[0] == '?' && x.p[1] == '>')) {
            x.p++;
        }
        x.p += 2;
    }
    while (x.ok) {
        skip_ws(&x);
        if (x.p >= x.end) {
            break;
        }
        if (starts(&x, "<!--")) {
            const char *b = x.p + 4;
            const char *e = b;
            while (e + 2 < x.end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) {
                e++;
            }
            if (e + 2 >= x.end || !comment_ok(b, e)) {
                x.ok = false;
                break;
            }
            embi_dstr d = embi_dstr_from_utf8(doc->slab, (embi_str){b, (uint32_t)(e - b)});
            (void)embi_append_child(&doc->base, &embi_new_comment(doc, d)->base);
            embi_dstr_free(d);
            x.p = e + 3;
        } else if (starts(&x, "<?")) {
            x.p += 2;
            embi_str tg = read_name(&x);
            const char *b = x.p;
            const char *e = b;
            while (e + 1 < x.end && !(e[0] == '?' && e[1] == '>')) {
                e++;
            }
            if (e + 1 >= x.end || tg.len == 0) {
                x.ok = false;
                break;
            }
            while (b < e && is_ws(*b)) {
                b++;
            }
            embi_dstr d = embi_dstr_from_utf8(doc->slab, (embi_str){b, (uint32_t)(e - b)});
            (void)embi_append_child(&doc->base, &embi_new_pi(doc, embi_atom_intern(tg.ptr, tg.len), d)->base);
            embi_dstr_free(d);
            x.p = e + 2;
        } else if (starts(&x, "<!DOCTYPE")) {
            x.p += 9;
            skip_ws(&x);
            embi_str nm = read_name(&x);
            embi_str pub = EMBI_STR("");
            embi_str sys = EMBI_STR("");
            skip_ws(&x);
            bool is_pub = starts(&x, "PUBLIC");
            if (is_pub || starts(&x, "SYSTEM")) {
                x.p += 6;
                for (int lit = is_pub ? 0 : 1; lit < 2; lit++) {
                    skip_ws(&x);
                    if (x.p < x.end && (*x.p == '"' || *x.p == 0x27)) {
                        char q = *x.p++;
                        const char *b = x.p;
                        while (x.p < x.end && *x.p != q) {
                            x.p++;
                        }
                        if (x.p >= x.end) {
                            break;
                        }
                        embi_str v = {b, (uint32_t)(x.p - b)};
                        x.p++;
                        if (lit == 0) {
                            pub = v;
                        } else {
                            sys = v;
                        }
                    } else {
                        break;
                    }
                }
            }
            int depth = 0;
            while (x.p < x.end && !(depth == 0 && *x.p == '>')) {
                if (*x.p == '[') {
                    depth++;
                } else if (*x.p == ']') {
                    depth--;
                }
                x.p++;
            }
            if (x.p >= x.end || nm.len == 0) {
                x.ok = false;
                break;
            }
            x.p++;
            embi_doctype *dt = nullptr;
            if (embi_dom_create_doctype(doc, nm, pub, sys, &dt) == EMBI_DOM_OK) {
                (void)embi_append_child(&doc->base, &dt->base);
            }
        } else if (*x.p == '<' && !have_root) {
            parse_element(&x, &doc->base, 0);
            have_root = true;
        } else {
            x.ok = false;
        }
    }
    if (!have_root) {
        x.ok = false;
    }
    embi_vec_free(&x.ns);
    return x.ok;
}

static void make_parsererror(embi_document *doc)
{
    while (doc->base.first_child) {
        embi_node *k = doc->base.first_child;
        embi_detach(k);
        embi_destroy_detached(k);
    }
    embi_atom uri = embi_atom_intern("http://www.mozilla.org/newlayout/xml/parsererror.xml", 52);
    embi_element *el = embi_new_element(doc, EMBI_NS_OTHER, uri, 0, embi_atom_intern("parsererror", 11));
    (void)embi_append_child(&doc->base, &el->base);
    (void)embi_append_child(&el->base, &embi_new_text_utf8(doc, EMBI_STR("XML Parsing Error: not well-formed"))->base);
}

embi_js_value embi_impl_DOMParser_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_DOMParser, c, nullptr, 0);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_node *embi_impl_DOMParser_parseFromString(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, int32_t a1)
{
    (void)self;
    embi_str text = embi_bind_utf8(c, a0);
    embi_document *d;
    if (a1 == 0) {
        d = embi_document_new(true);
        embi_bind_add_xdoc(c, d);
        embi_html_parser *p = embi_html_parser_new(d, nullptr);
        embi_html_parser_write(p, text.ptr, text.len);
        embi_html_parser_end(p);
        embi_html_parser_destroy(p);
        return &d->base;
    }
    static const uint8_t cts[] = {EMBI_CT_HTML, EMBI_CT_XML, EMBI_CT_XML, EMBI_CT_XHTML, EMBI_CT_SVG};
    d = embi_document_new(false);
    d->ctype = cts[a1];
    embi_bind_add_xdoc(c, d);
    if (!xml_parse(d, text)) {
        make_parsererror(d);
    }
    return &d->base;
}
