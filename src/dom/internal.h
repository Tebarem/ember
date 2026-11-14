#pragma once
/*
 * Helpers shared by the DOM sources (node.c, tree.c, attr.c, document.c, names.c, idmap.c). Not
 * part of the module API; other modules use dom/dom.h.
 */
#include "dom/dom.h"

/* Raw tree links: no validity checks, no hooks. Linking sets EMBI_NF_CONNECTED on the moved
 * nodes when the parent is connected; unlinking leaves the flag alone (embi_detach clears it). */
void embi_tree_unlink(embi_node *node);
/* Links the chain first..last (linked through next) before `before` (nullptr appends). */
void embi_tree_link_chain(embi_node *parent, embi_node *first, embi_node *last, embi_node *before);
void embi_tree_append_raw(embi_node *parent, embi_node *child);

/* Deep or shallow copy into doc with no hooks. Template contents are cloned into the template
 * owner of doc. Does not handle Document nodes. */
embi_node *embi_clone_tree(embi_document *doc, const embi_node *src, bool deep);

/* Sets or clears EMBI_NF_CONNECTED on root and its descendants. */
void embi_set_connected_subtree(embi_node *root, bool on);

/* Attribute block. Created or grown in the element document's slab. */
embi_attrs *embi_el_attrs_ensure(embi_element *el);
/* Like embi_el_attrs_ensure, with room for at least `need` entries. Entries may move when the
 * block grows, so do not keep entry pointers across this call. */
embi_attrs *embi_el_attrs_reserve(embi_element *el, uint32_t need);
/* Releases an attribute value of block a: nothing for a value stored inside the block. */
void embi_attr_value_free(const embi_attrs *a, embi_dstr v);
/* True when the non-empty value v is stored inside the attribute block a itself. */
bool embi_attr_value_inline(const embi_attrs *a, embi_dstr v);
/* Recomputes the cached id atom from the attribute block. */
void embi_el_recompute_id(embi_element *el);

/* Namespace argument of a DOM call (null when has_ns is false or the URI is empty). For
 * EMBI_NS_OTHER, *uri_atom receives the interned URI. */
uint8_t embi_dom_ns_arg(bool has_ns, embi_str uri, embi_atom *uri_atom);
/* Lookup variant of embi_dom_ns_arg: never interns. Returns false when the URI is an unknown
 * namespace that was never interned, which means no element or attribute can have it. */
bool embi_dom_ns_arg_find(bool has_ns, embi_str uri, uint8_t *ns, embi_atom *uri_atom);
/* Frees a detached subtree unless the JS side owns a wrapper somewhere in it (then the wrapper's
 * finalizer owns the nodes). Returns true when freed. */
bool embi_destroy_if_unowned(embi_node *root);
/* The namespace/prefix/qualified name rules shared by createElementNS and setAttributeNS. */
embi_dom_err embi_dom_check_ns(uint8_t ns, embi_str qname, embi_str prefix);

/* Observer dispatch. A no-op when nothing is registered. */
void embi_notify_children(embi_node *parent, embi_node *first_added, embi_node *last_added,
                          embi_node *first_removed, embi_node *last_removed);
void embi_notify_attribute(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom name,
                           embi_dstr old, bool had_old);
void embi_notify_char_data(embi_node *node);
void embi_notify_inserted(embi_node *node);
void embi_notify_removed(embi_node *node);
void embi_notify_pre_remove(embi_node *node);
