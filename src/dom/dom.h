#pragma once
/*
 * DOM core: nodes, elements, attributes, documents and tree operations, following the DOM
 * Standard. Internal cross-module header (include "dom/dom.h").
 *
 * Memory: every node, payload and attribute block lives in a slab (slab.h). New allocations come
 * from the slab of the node's document (node->doc->slab). Frees go back to the block's own owner
 * slab, so nodes adopted from another document stay valid while they are reachable.
 *
 * Strings: embi_dstr (dstr.h). Creation functions copy their dstr and UTF-8 inputs. Pointers to
 * nodes and attributes stay valid until the node is destroyed. A const embi_attr * is valid until
 * the next change to that element's attributes.
 *
 * Mutation hooks: see embi_observer. Hooks run synchronously on each tree or attribute change.
 */
#include "base/base.h"
#include "dom/atoms.inc"
#include "dom/dstr.h"
#include "dom/slab.h"

/* ------------------------------------------------------------------ constants */

typedef enum embi_node_type {
    EMBI_NODE_ELEMENT = 1,
    EMBI_NODE_ATTR = 2,
    EMBI_NODE_TEXT = 3,
    EMBI_NODE_PI = 7,
    EMBI_NODE_COMMENT = 8,
    EMBI_NODE_DOCUMENT = 9,
    EMBI_NODE_DOCTYPE = 10,
    EMBI_NODE_FRAGMENT = 11,
} embi_node_type;

/* Namespaces. The element's namespace is stored in node.extra; the URI of EMBI_NS_OTHER is an
 * atom in the attribute block (embi_attrs.ns_uri) or in embi_attr.ns_uri. */
typedef enum embi_ns {
    EMBI_NS_NULL = 0,
    EMBI_NS_HTML,
    EMBI_NS_SVG,
    EMBI_NS_MATHML,
    EMBI_NS_XLINK,
    EMBI_NS_XML,
    EMBI_NS_XMLNS,
    EMBI_NS_OTHER,
} embi_ns;

typedef enum embi_quirks {
    EMBI_QUIRKS_NONE = 0,
    EMBI_QUIRKS_LIMITED,
    EMBI_QUIRKS_FULL,
} embi_quirks;

typedef enum embi_dom_err {
    EMBI_DOM_OK = 0,
    EMBI_DOM_ERR_HIERARCHY,    /* HierarchyRequestError */
    EMBI_DOM_ERR_NOT_FOUND,    /* NotFoundError */
    EMBI_DOM_ERR_INVALID_CHAR, /* InvalidCharacterError */
    EMBI_DOM_ERR_NAMESPACE,    /* NamespaceError */
    EMBI_DOM_ERR_NOT_SUPPORTED /* NotSupportedError */
} embi_dom_err;

const char *embi_dom_err_name(embi_dom_err e);

/* compareDocumentPosition result bits. */
#define EMBI_POS_DISCONNECTED 0x01u
#define EMBI_POS_PRECEDING 0x02u
#define EMBI_POS_FOLLOWING 0x04u
#define EMBI_POS_CONTAINS 0x08u
#define EMBI_POS_CONTAINED_BY 0x10u
#define EMBI_POS_IMPL_SPECIFIC 0x20u

/* Node flags. */
#define EMBI_NF_CONNECTED 0x01u /* the root of the node is a document */
/* embi_element.id before the id attribute was interned. */
#define EMBI_ID_STALE 0xFFFFFFFFu
#define EMBI_NF_INLINE_DATA 0x02u /* character data: the payload is in the node's own block */
#define EMBI_NF_CDATA 0x08u /* a Text node that is a CDATASection (nodeType 4) */
#define EMBI_NF_HELD 0x04u /* a MutationObserver record or observation refers to the node: never freed (dom/mutation.h) */

/* ------------------------------------------------------------------ nodes */

typedef struct embi_document embi_document;

/* Every node kind starts with this 64-byte header. For elements, node.extra holds the namespace. */
typedef struct embi_node {
    uint8_t type;      /* embi_node_type */
    uint8_t flags;     /* EMBI_NF_* */
    uint16_t extra;    /* element: embi_ns; otherwise 0 */
    embi_atom name;    /* element local name; PI target; 0 otherwise */
    struct embi_node *parent;
    struct embi_node *first_child;
    struct embi_node *last_child;
    struct embi_node *prev;
    struct embi_node *next;
    struct embi_document *doc; /* node document */
    void *wrapper;             /* JS wrapper (W5) */
} embi_node;
static_assert(sizeof(embi_node) == 64, "embi_node is 64 bytes");

/* Attribute (in an element's attribute block). */
typedef struct embi_attr {
    embi_atom name;   /* local name */
    embi_atom prefix; /* 0 when none */
    embi_atom ns_uri; /* namespace URI atom when ns == EMBI_NS_OTHER, else 0 */
    uint8_t ns;       /* embi_ns */
    embi_dstr value;
} embi_attr;
static_assert(sizeof(embi_attr) == 32, "embi_attr is 32 bytes");

/* Attribute block: header plus entries, allocated from the element document's slab. */
typedef struct embi_attrs {
    embi_node *content;  /* template contents fragment (template elements only) */
    embi_atom ns_uri;    /* element namespace URI atom when ns == EMBI_NS_OTHER */
    uint32_t len;
    uint32_t cap;
    uint32_t nclass;     /* cached class tokens */
    uint32_t flags;      /* EMBI_ATTRS_CLASSES_VALID */
    embi_atom *classes;  /* cached class atoms, nullptr when none */
    embi_attr entries[];
} embi_attrs;

#define EMBI_ATTRS_CLASSES_VALID 0x1u
/* Some values of the entries live in the tail of the attribute block itself (see
 * embi_new_element_attrs). They are never freed on their own. */
#define EMBI_ATTRS_INLINE_VALUES 0x2u

typedef struct embi_element {
    embi_node base;
    embi_atom prefix; /* 0 when none */
    embi_atom id;     /* cached id atom, 0 when none, EMBI_ID_STALE until first use: read it with
                         embi_el_id */
    embi_attrs *attrs; /* nullptr when there are no attributes and no extra data */
    void *style;      /* W2 slot */
    void *box;        /* W3 slot */
} embi_element;
static_assert(sizeof(embi_element) <= 96, "embi_element fits in 96 bytes");

/* An Attr node (DOM Standard section 4.9.2), made only for script: the attributes of an element live in its attribute
 * block, and an Attr is a handle with the identity (namespace, prefix, local name) of one of them. While owner is set the
 * value is the element's; after the attribute is removed (or before it is set on an element) the node keeps value
 * itself. base.doc is the node document while detached; embi_attr_node_doc gives the current one. */
typedef struct embi_attr_node {
    embi_node base;
    embi_element *owner; /* nullptr when detached */
    uint8_t ns;          /* embi_ns */
    embi_atom ns_uri;    /* only for EMBI_NS_OTHER */
    embi_atom prefix;
    embi_atom local;
    embi_dstr value;     /* the value while detached */
} embi_attr_node;

/* DocumentFragment. A template contents fragment sets host to its template element, so that
 * insertion checks can treat the template as the host of its contents (host-including ancestors).
 * Every fragment is allocated with this size. */
typedef struct embi_fragment {
    embi_node base;
    embi_node *host; /* template element for template contents, nullptr otherwise */
} embi_fragment;

/* Text, Comment and ProcessingInstruction. The PI target is base.name. A short payload is stored
 * after the struct, in the same block (EMBI_NF_INLINE_DATA): such a payload is not freed on its
 * own, and data.ptr is not a block start. Replace data with embi_chardata_adopt. */
typedef struct embi_chardata {
    embi_node base;
    embi_dstr data;
} embi_chardata;

typedef struct embi_doctype {
    embi_node base;
    embi_dstr name;
    embi_dstr public_id;
    embi_dstr system_id;
} embi_doctype;

/* ------------------------------------------------------------------ documents and observers */

/* Observers run synchronously, in registration order. They must not add or remove observers or
 * mutate the tree from inside a callback. */
typedef struct embi_observer {
    void *ctx;
    /* A single tree mutation. parent is the node whose children changed. The added and removed
     * ranges are contiguous runs of siblings, or nullptr when empty. A replace produces two calls. */
    void (*children_changed)(void *ctx, embi_node *parent, embi_node *first_added,
                             embi_node *last_added, embi_node *first_removed,
                             embi_node *last_removed);
    /* An attribute was added, changed or removed. The attribute is identified by its namespace
     * (embi_ns; ns_uri is the URI atom only for EMBI_NS_OTHER, else 0, and embi_dom_ns_uri maps
     * the pair to a URI string) and local name, which stay valid after a removal. old_value is
     * valid only during the call. */
    void (*attribute_changed)(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri,
                              embi_atom local_name, embi_dstr old_value, bool had_old);
    void (*character_data_changed)(void *ctx, embi_node *node);
    /* Called for each root of an inserted run, after it is linked. */
    void (*node_inserted)(void *ctx, embi_node *node);
    /* Called for each detached root, after it is unlinked. */
    void (*node_removed)(void *ctx, embi_node *node);
    /* Called for each node that is about to be removed from its parent, before it is unlinked (the "removing steps"
     * of NodeIterator). Optional. */
    void (*pre_remove)(void *ctx, embi_node *node);
} embi_observer;

/* The content type of a document (Document.contentType). */
typedef enum embi_ctype {
    EMBI_CT_HTML = 0, /* text/html */
    EMBI_CT_XML,      /* application/xml */
    EMBI_CT_XHTML,    /* application/xhtml+xml */
    EMBI_CT_SVG,      /* image/svg+xml */
} embi_ctype;

typedef struct embi_document {
    embi_node base; /* type EMBI_NODE_DOCUMENT; base.doc == this */
    embi_slab *slab;
    bool is_html;
    bool inert;                     /* template contents owner: no browsing context */
    uint8_t ctype;                  /* embi_ctype; EMBI_CT_HTML when is_html */
    bool xml_class;                 /* created by DOMImplementation.createDocument: an XMLDocument */
    embi_quirks quirks;             /* set by the tree builder from the doctype */
    embi_document *template_owner;  /* lazily created inert owner, freed with this document */
    embi_hashmap idmap;             /* id atom -> element (first in tree order) */
    bool idmap_dirty;
    EMBI_VEC(embi_observer) observers;
    void *mut; /* MutationObserver state (dom/mutation.h), nullptr until the first observer */
    /* W5f: owned by the bindings, for documents made by script (DOMParser, new Document(), ...). script_made marks the
     * document, xfresh is set until a wrapper of one of its nodes exists, and xseen is the GC epoch of the last
     * collection that found one of its wrappers reachable. */
    bool script_made;
    bool xfresh;
    uint32_t xseen;
} embi_document;

/* Optional. The static atoms (EMBI_ATOM_*) are interned by base before anything else, so the ids
 * hold whichever module touches the atom table first. This only asserts them (idempotent,
 * thread-safe, cheap) and aborts if atoms.inc and base's static_atoms.inc disagree. Called by
 * embi_document_new. */
void embi_dom_init(void);

embi_document *embi_document_new(bool is_html);
/* The inert document that owns template contents for `doc`. Returns doc itself when doc is inert.
 * Created on first use and owned by doc. */
embi_document *embi_document_template_owner(embi_document *doc);
/* Frees the document, its template owner, and all of its slab storage (unless another document
 * still holds nodes adopted from it). Nodes must not be used afterwards. */
void embi_document_free(embi_document *doc);
void embi_document_observe(embi_document *doc, const embi_observer *obs);
/* Removes the first observer registered with this ctx. */
void embi_document_unobserve(embi_document *doc, void *ctx);
embi_element *embi_document_element(embi_document *doc);

/* ------------------------------------------------------------------ type helpers */

static inline bool embi_is_element(const embi_node *n)
{
    return n->type == EMBI_NODE_ELEMENT;
}

static inline embi_element *embi_as_element(embi_node *n)
{
    EMBI_ASSERT(n->type == EMBI_NODE_ELEMENT);
    return (embi_element *)n;
}

static inline embi_chardata *embi_as_chardata(embi_node *n)
{
    EMBI_ASSERT(n->type == EMBI_NODE_TEXT || n->type == EMBI_NODE_COMMENT ||
                n->type == EMBI_NODE_PI);
    return (embi_chardata *)n;
}

/* ------------------------------------------------------------------ creation (no validation) */

/* These copy their inputs into the document's slab. Element and namespace arguments are not
 * checked; use the validated functions below for script-visible creation. */
embi_element *embi_new_element(embi_document *doc, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                               embi_atom local);
embi_chardata *embi_new_text(embi_document *doc, embi_dstr data);
/* A text node from UTF-8 text (WTF-8 surrogates are kept), converted once into the node's block. */
embi_chardata *embi_new_text_utf8(embi_document *doc, embi_str text);
embi_chardata *embi_new_comment(embi_document *doc, embi_dstr data);
embi_chardata *embi_new_pi(embi_document *doc, embi_atom target, embi_dstr data);
embi_doctype *embi_new_doctype(embi_document *doc, embi_dstr name, embi_dstr public_id,
                               embi_dstr system_id);
embi_node *embi_new_fragment(embi_document *doc);
/* A detached Attr node (value is copied). */
embi_attr_node *embi_new_attr_node(embi_document *doc, uint8_t ns, embi_atom ns_uri, embi_atom prefix, embi_atom local,
                                   embi_dstr value);
/* The current value: the owner's attribute value while attached, else the node's own. */
embi_dstr embi_attr_node_value(const embi_attr_node *a);
/* Sets the value (an attached Attr changes the element's attribute, with its observers). */
void embi_attr_node_set_value(embi_attr_node *a, embi_dstr v);
/* The node document (the owner's while attached). */
embi_document *embi_attr_node_doc(const embi_attr_node *a);
/* Makes the Attr detached, keeping value (copied) as its own. */
void embi_attr_node_detach(embi_attr_node *a, embi_dstr value);
/* Attaches the Attr to el: the element's attribute (namespace, local name) takes the Attr's prefix and value, an attribute
 * is added when there is none. The caller has dealt with the Attr that was there before. */
void embi_attr_node_attach(embi_attr_node *a, embi_element *el);
/* Puts an attribute with the given identity on el: replaces the entry with the same namespace and local name (taking the
 * prefix too), or appends one. */
void embi_el_put_attr(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix, embi_atom local, embi_dstr value);
/* The template contents fragment (created on first use in the template owner document). */
embi_node *embi_template_contents(embi_element *el);

/* ------------------------------------------------------------------ creation (validated) */

/* Limitation: non-HTML documents (is_html == false) create createElement elements in the null
 * namespace; the XHTML content-type rule (XML documents creating HTML-namespace elements) is not
 * implemented. Names are checked against the XML Name production; malformed UTF-8 is rejected.
 */
/* createElement: lowercases the name for HTML elements in HTML documents. */
embi_dom_err embi_dom_create_element(embi_document *doc, embi_str local_name, embi_element **out);
/* createElementNS. has_ns == false means a null namespace. */
embi_dom_err embi_dom_create_element_ns(embi_document *doc, bool has_ns, embi_str ns_uri,
                                        embi_str qualified_name, embi_element **out);
/* createAttribute / createAttributeNS: a detached Attr node (lowercased in HTML documents for createAttribute). */
embi_dom_err embi_dom_create_attr(embi_document *doc, embi_str local_name, embi_attr_node **out);
embi_dom_err embi_dom_create_attr_ns(embi_document *doc, bool has_ns, embi_str ns_uri, embi_str qualified_name,
                                     embi_attr_node **out);
embi_dom_err embi_dom_create_pi(embi_document *doc, embi_str target, embi_dstr data,
                                embi_chardata **out);
embi_dom_err embi_dom_create_doctype(embi_document *doc, embi_str name, embi_str public_id,
                                     embi_str system_id, embi_doctype **out);

/* ------------------------------------------------------------------ tree operations */

/* Pre-insert: validity checks, then insert node before child (nullptr appends). */
embi_dom_err embi_pre_insert(embi_node *parent, embi_node *node, embi_node *child);
embi_dom_err embi_append_child(embi_node *parent, embi_node *node);
embi_dom_err embi_replace_child(embi_node *parent, embi_node *node, embi_node *child);
embi_dom_err embi_remove_child(embi_node *parent, embi_node *child);
/* Detaches node from its parent. No-op when it has none. */
void embi_detach(embi_node *node);
/* replaceAll: removes every child of parent and inserts node when it is not nullptr.
 * Ownership: removed children that have no JS wrapper (node->wrapper == nullptr anywhere in the
 * subtree) are freed here, because the caller has no handle to them. A subtree with a wrapper
 * stays allocated and owned by the JS side, which frees it with embi_destroy_detached when the
 * wrapper dies (or it lives until embi_document_free). embi_normalize and
 * embi_text_content_set follow the same rule. embi_detach, embi_remove_child and
 * embi_replace_child never free: the caller holds the node and owns it. */
embi_dom_err embi_replace_all(embi_node *parent, embi_node *node);
/* adoptNode: detaches node and moves its subtree into doc. Documents are not supported. */
embi_dom_err embi_adopt_node(embi_document *doc, embi_node *node);
/* Adoption makes doc keep the source slab alive (embi_slab_keep) for as long as doc lives, so
 * moving nodes in from many short-lived documents pins all of them. Fragment parsing (innerHTML,
 * insertAdjacentHTML) must therefore build its nodes directly in the destination document, with
 * the context element's document as the target, and never in a temporary scratch document. */
/* cloneNode into the node's document. Returns the copy, or nullptr for a detached document clone
 * failure. For a Document, returns a new document (free with embi_document_free). */
embi_node *embi_clone_node(embi_node *node, bool deep);
/* importNode: clone into doc. Returns nullptr for documents. */
embi_node *embi_import_node(embi_document *doc, embi_node *node, bool deep);
/* Frees a detached subtree (parent must be nullptr; documents are not allowed). */
void embi_destroy_detached(embi_node *node);

/* textContent. The getter appends into out and returns false when the value is null (documents
 * and doctypes). The setter replaces children (elements and fragments) or data (character data). */
bool embi_text_content_get(embi_node *node, embi_dbuf *out);
embi_dom_err embi_text_content_set(embi_node *node, embi_dstr value);
/* normalize: removes empty Text nodes and merges adjacent ones, in tree order. Removed nodes
 * are freed unless they have a JS wrapper (see embi_replace_all). */
void embi_normalize(embi_node *root);

/* Character data setter with observer notification (data is copied). */
void embi_chardata_set(embi_chardata *cd, embi_dstr value);
/* Replaces the payload with fresh, which must be a slab allocation from the node's document (the
 * node takes ownership). Frees the old payload unless it was inline. Does not notify observers. */
void embi_chardata_adopt(embi_chardata *cd, embi_dstr fresh);

/* Queries. */
uint16_t embi_compare_position(const embi_node *a, const embi_node *b);
/* Inclusive descendant test. */
bool embi_contains(const embi_node *a, const embi_node *b);
bool embi_is_equal_node(const embi_node *a, const embi_node *b);
embi_node *embi_get_root(embi_node *node);
/* Next node in tree order under root (pre-order), or nullptr after the last descendant. */
embi_node *embi_next_in_tree(const embi_node *root, const embi_node *node);
/* The previous node in tree order within root's subtree (root included), or nullptr when node is
 * root. The inverse of embi_next_in_tree. */
embi_node *embi_prev_in_tree(const embi_node *root, const embi_node *node);
/* The next node in tree order after node's whole subtree (its descendants are skipped), or nullptr
 * when none is left under root. */
embi_node *embi_next_skipping_children(const embi_node *root, const embi_node *node);

/* ------------------------------------------------------------------ names and namespaces */

/* XML Name (allow_colon) or NCName production, over UTF-8 text. */
bool embi_dom_is_name(embi_str s, bool allow_colon);
/* "Validate and extract" (DOM Standard): splits a qualified name at its first colon. Returns false if the prefix is not a
 * valid namespace prefix or the local part is not a valid element (attr == false) or attribute (attr == true) local name.
 * prefix.len is 0 when there is none. */
bool embi_dom_split_qname(embi_str q, bool attr, embi_str *prefix, embi_str *local);
/* The DOM Standard's name validity checks (also used by createElement, setAttribute and createDocumentType). */
bool embi_dom_valid_element_name(embi_str s);
bool embi_dom_valid_attr_name(embi_str s);
bool embi_dom_valid_doctype_name(embi_str s);
/* URI of a namespace. ns_uri is used for EMBI_NS_OTHER. Empty for EMBI_NS_NULL. */
embi_str embi_dom_ns_uri(uint8_t ns, embi_atom ns_uri);
/* Maps a namespace URI to an embi_ns (EMBI_NS_OTHER for unknown ones). */
uint8_t embi_dom_ns_from_uri(embi_str uri);

/* ------------------------------------------------------------------ attributes */

/* Element attribute access. Names are UTF-8. Qualified-name lookups compare prefix:local.
 * setAttribute and getAttribute lowercase the name for HTML elements in HTML documents. */
embi_dom_err embi_el_set_attribute(embi_element *el, embi_str qname, embi_dstr value);
embi_dom_err embi_el_set_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri,
                                      embi_str qname, embi_dstr value);
/* Parser fast path: an attribute with a UTF-8 value, for embi_new_element_attrs. */
typedef struct embi_attr_init {
    embi_atom name;
    embi_str value;
} embi_attr_init;
/* Creates an element with n null-namespace, unprefixed attributes (no duplicate names) in one
 * step: the attribute block and the values are a single allocation. Same result as embi_new_element
 * followed by embi_el_set_attr_atom for each entry, but observers are only told about the
 * attributes when the document has any. */
embi_element *embi_new_element_attrs(embi_document *doc, uint8_t ns, embi_atom local, uint32_t n,
                                     const embi_attr_init *attrs);
/* Parser fast path: makes room for need attributes in one block of exactly that size, so that
 * adding them does not regrow it. Entries move when the block grows. */
embi_attrs *embi_el_attrs_reserve(embi_element *el, uint32_t need);
/* Fast path for the parser: null namespace, no prefix, no validation. */
void embi_el_set_attr_atom(embi_element *el, embi_atom local, embi_dstr value);
/* Parser fast path for foreign attribute adjustment: the namespace, prefix and local name are
 * already atoms (ns_uri is used only for EMBI_NS_OTHER). No validation. */
void embi_el_set_attr_ns_atom(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                              embi_atom local, embi_dstr value);
/* Attribute iteration in insertion order. The pointer is valid until the next attribute change. */
uint32_t embi_el_attr_count(const embi_element *el);
const embi_attr *embi_el_attr_at(const embi_element *el, uint32_t index);
const embi_attr *embi_el_get_attribute(embi_element *el, embi_str qname);
const embi_attr *embi_el_get_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri,
                                          embi_str local);
/* Finds by namespace (embi_ns and URI atom) and local name atom. */
const embi_attr *embi_el_find_attr(const embi_element *el, uint8_t ns, embi_atom ns_uri,
                                   embi_atom local);
bool embi_el_has_attribute(embi_element *el, embi_str qname);
bool embi_el_has_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri, embi_str local);
bool embi_el_remove_attribute(embi_element *el, embi_str qname);
bool embi_el_remove_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri, embi_str local);
/* toggleAttribute. *out receives whether the attribute is present afterwards. */
embi_dom_err embi_el_toggle_attribute(embi_element *el, embi_str qname, bool has_force, bool force,
                                      bool *out);
/* Cached id atom (0 when none). The cache is filled lazily through a const pointer, so a DOM must
 * not be read from several threads at once (the DOM is single-threaded by design). */
embi_atom embi_el_id(const embi_element *el);
/* Cached class tokens. The pointer is valid until the next class change. */
const embi_atom *embi_el_classes(embi_element *el, uint32_t *count);
bool embi_el_has_class(embi_element *el, embi_atom cls);
/* Namespace URI of an element. */
embi_str embi_el_namespace_uri(const embi_element *el);

/* ------------------------------------------------------------------ ids */

/* getElementById: the first element in tree order under doc with this id. Rebuilds the id map
 * when it is stale. */
embi_element *embi_get_element_by_id(embi_document *doc, embi_str id);
/* Marks the id map stale. Called on every change that can move an id. */
void embi_document_idmap_invalidate(embi_document *doc);
