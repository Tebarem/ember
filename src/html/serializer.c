/*
 * HTML fragment serialization, WHATWG 13.3. Escapes text (&, nbsp, <, >) and attribute values
 * (&, nbsp, "), omits end tags of void elements, and writes raw text for the raw text parents.
 *
 * The walk keeps its own stack of open elements, so the depth of the tree costs heap memory, not
 * C stack: a document nested 100k deep serializes as easily as a flat one.
 */
#include "html/serializer.h"

#include "base/base.h"

#include <string.h>

/* An open element whose end tag is still to be written, with the container its siblings sit
 * in (the parent, or the template contents of a template). */
typedef struct ser_frame {
    const embi_node *node;
    const embi_node *container;
} ser_frame;

static bool is_html_el(const embi_node *n, embi_atom name)
{
    return n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_HTML && n->name == name;
}

/* The template contents of a template element (nullptr for other elements, or none yet). */
static const embi_node *template_content(const embi_node *n)
{
    const embi_element *el = (const embi_element *)n;
    return el->attrs ? el->attrs->content : nullptr;
}

/* The node whose children are serialized for n: the template contents for a template. */
static const embi_node *children_container(const embi_node *n)
{
    if (is_html_el(n, EMBI_ATOM_template)) {
        const embi_node *c = template_content(n);
        if (c) {
            return c;
        }
    }
    return n;
}

static bool is_void(const embi_node *n)
{
    if (n->extra != EMBI_NS_HTML) {
        return false;
    }
    switch (n->name) {
    case EMBI_ATOM_area:
    case EMBI_ATOM_base:
    case EMBI_ATOM_basefont:
    case EMBI_ATOM_bgsound:
    case EMBI_ATOM_br:
    case EMBI_ATOM_col:
    case EMBI_ATOM_embed:
    case EMBI_ATOM_frame:
    case EMBI_ATOM_hr:
    case EMBI_ATOM_img:
    case EMBI_ATOM_input:
    case EMBI_ATOM_keygen:
    case EMBI_ATOM_link:
    case EMBI_ATOM_meta:
    case EMBI_ATOM_param:
    case EMBI_ATOM_source:
    case EMBI_ATOM_track:
    case EMBI_ATOM_wbr:
        return true;
    default:
        return false;
    }
}

/* Text of these parents is written without escaping. */
static bool is_raw_parent(const embi_node *parent, bool scripting)
{
    if (parent->type != EMBI_NODE_ELEMENT) {
        return false;
    }
    if (parent->extra != EMBI_NS_HTML) {
        return false;
    }
    switch (parent->name) {
    case EMBI_ATOM_style:
    case EMBI_ATOM_script:
    case EMBI_ATOM_xmp:
    case EMBI_ATOM_iframe:
    case EMBI_ATOM_noembed:
    case EMBI_ATOM_noframes:
    case EMBI_ATOM_plaintext:
        return true;
    case EMBI_ATOM_noscript:
        return scripting;
    default:
        return false;
    }
}

/* Appends the code point (or U+FFFD for a lone surrogate) of the next unit(s) of d. */
static uint32_t next_cp(embi_dstr d, uint32_t *i)
{
    uint32_t u = embi_dstr_unit(d, *i);
    if (u >= 0xD800u && u <= 0xDBFFu && *i + 1 < d.len) {
        uint32_t lo = embi_dstr_unit(d, *i + 1);
        if (lo >= 0xDC00u && lo <= 0xDFFFu) {
            *i += 1;
            return 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
        }
    }
    if (u >= 0xD800u && u <= 0xDFFFu) {
        return 0xFFFDu;
    }
    return u;
}

/* Escapes text for a text node (attribute == false) or an attribute value (attribute == true). */
static void put_escaped(embi_sb *out, embi_dstr d, bool attribute)
{
    for (uint32_t i = 0; i < d.len; i++) {
        uint32_t cp = next_cp(d, &i);
        switch (cp) {
        case '&':
            embi_sb_append(out, EMBI_STR("&amp;"));
            break;
        case 0xA0:
            embi_sb_append(out, EMBI_STR("&nbsp;"));
            break;
        case '"':
            if (attribute) {
                embi_sb_append(out, EMBI_STR("&quot;"));
            } else {
                embi_sb_append_cp(out, cp);
            }
            break;
        case '<':
            embi_sb_append(out, EMBI_STR("&lt;"));
            break;
        case '>':
            embi_sb_append(out, EMBI_STR("&gt;"));
            break;
        default:
            embi_sb_append_cp(out, cp);
            break;
        }
    }
}

/* The serialized name of an attribute (the namespace designators of the spec). */
static void put_attr_name(embi_sb *out, const embi_attr *a)
{
    embi_str local = embi_atom_str(a->name);
    switch (a->ns) {
    case EMBI_NS_XML:
        embi_sb_append(out, EMBI_STR("xml:"));
        embi_sb_append(out, local);
        return;
    case EMBI_NS_XMLNS:
        if (embi_str_eq(local, EMBI_STR("xmlns"))) {
            embi_sb_append(out, local);
        } else {
            embi_sb_append(out, EMBI_STR("xmlns:"));
            embi_sb_append(out, local);
        }
        return;
    case EMBI_NS_XLINK:
        embi_sb_append(out, EMBI_STR("xlink:"));
        embi_sb_append(out, local);
        return;
    default:
        if (a->prefix != 0) {
            embi_sb_append(out, embi_atom_str(a->prefix));
            embi_sb_append_char(out, ':');
        }
        embi_sb_append(out, local);
        return;
    }
}

/* The tag name of an element: the local name in the HTML, MathML and SVG namespaces, and the
 * qualified name (prefix:local) in any other namespace. */
static void put_tagname(embi_sb *out, const embi_element *el)
{
    const embi_node *n = &el->base;
    bool local_only = n->extra == EMBI_NS_HTML || n->extra == EMBI_NS_SVG ||
                      n->extra == EMBI_NS_MATHML;
    if (!local_only && el->prefix != 0) {
        embi_sb_append(out, embi_atom_str(el->prefix));
        embi_sb_append_char(out, ':');
    }
    embi_sb_append(out, embi_atom_str(n->name));
}

static void put_start_tag(embi_sb *out, const embi_element *el)
{
    embi_sb_append_char(out, '<');
    put_tagname(out, el);
    uint32_t count = embi_el_attr_count(el);
    for (uint32_t i = 0; i < count; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        embi_sb_append_char(out, ' ');
        put_attr_name(out, a);
        embi_sb_append(out, EMBI_STR("=\""));
        put_escaped(out, a->value, true);
        embi_sb_append_char(out, '"');
    }
    embi_sb_append_char(out, '>');
}

static void put_end_tag(embi_sb *out, const embi_element *el)
{
    embi_sb_append(out, EMBI_STR("</"));
    put_tagname(out, el);
    embi_sb_append_char(out, '>');
}

static void put_leaf(embi_sb *out, const embi_node *n, const embi_node *parent, bool scripting)
{
    switch (n->type) {
    case EMBI_NODE_TEXT: {
        const embi_chardata *cd = (const embi_chardata *)n;
        if (is_raw_parent(parent, scripting)) {
            embi_dstr_append_utf8(out, cd->data);
        } else {
            put_escaped(out, cd->data, false);
        }
        return;
    }
    case EMBI_NODE_COMMENT: {
        const embi_chardata *cd = (const embi_chardata *)n;
        embi_sb_append(out, EMBI_STR("<!--"));
        embi_dstr_append_utf8(out, cd->data);
        embi_sb_append(out, EMBI_STR("-->"));
        return;
    }
    case EMBI_NODE_PI: {
        const embi_chardata *cd = (const embi_chardata *)n;
        embi_sb_append(out, EMBI_STR("<?"));
        embi_sb_append(out, embi_atom_str(n->name));
        embi_sb_append_char(out, ' ');
        embi_dstr_append_utf8(out, cd->data);
        embi_sb_append(out, EMBI_STR("?>"));
        return;
    }
    case EMBI_NODE_DOCTYPE: {
        const embi_doctype *dt = (const embi_doctype *)n;
        embi_sb_append(out, EMBI_STR("<!DOCTYPE "));
        embi_dstr_append_utf8(out, dt->name);
        embi_sb_append_char(out, '>');
        return;
    }
    default:
        return;
    }
}

/* Serializes first and its following siblings (children of first_parent), with their
 * descendants. In node mode only first itself is written (outerHTML). */
static void walk(const embi_node *first, const embi_node *first_parent, bool node_mode,
                 bool scripting, embi_sb *out)
{
    EMBI_VEC(ser_frame) open = {nullptr, 0, 0};
    const embi_node *n = first;
    const embi_node *container = first_parent;
    for (;;) {
        if (!n) {
            if (open.len == 0) {
                break;
            }
            ser_frame f = embi_vec_pop(&open);
            put_end_tag(out, (const embi_element *)f.node);
            if (node_mode && open.len == 0) {
                break;
            }
            n = f.node->next;
            container = f.container;
            continue;
        }
        if (n->type == EMBI_NODE_ELEMENT) {
            const embi_element *el = (const embi_element *)n;
            put_start_tag(out, el);
            if (is_void(n)) {
                if (node_mode && open.len == 0) {
                    break;
                }
                n = n->next;
                continue;
            }
            ser_frame f = {n, container};
            embi_vec_push(&open, f);
            container = children_container(n);
            n = container->first_child;
            continue;
        }
        put_leaf(out, n, container, scripting);
        if (node_mode && open.len == 0) {
            break;
        }
        n = n->next;
    }
    embi_vec_free(&open);
}

void embi_html_serialize_children(const embi_node *node, bool scripting, embi_sb *out)
{
    const embi_node *src = children_container(node);
    walk(src->first_child, src, false, scripting, out);
}

void embi_html_serialize_node(const embi_node *node, bool scripting, embi_sb *out)
{
    if (node->type == EMBI_NODE_ELEMENT) {
        walk(node, node->parent ? node->parent : node, true, scripting, out);
    } else if (node->type == EMBI_NODE_DOCUMENT || node->type == EMBI_NODE_FRAGMENT) {
        embi_html_serialize_children(node, scripting, out);
    } else {
        const embi_node *parent = node->parent ? node->parent : node;
        walk(node, parent, true, scripting, out);
    }
}
