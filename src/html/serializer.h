#pragma once
/*
 * HTML fragment serialization (WHATWG 13.3), behind the innerHTML and outerHTML getters. Internal
 * module API.
 */
#include "dom/dom.h"

/* Appends the serialization of node's children (innerHTML). A template element serializes its
 * template contents. scripting: whether noscript content is raw text. */
void embi_html_serialize_children(const embi_node *node, bool scripting, embi_sb *out);
/* Appends the serialization of node itself (outerHTML). For a document or a fragment this is the
 * same as its children. */
void embi_html_serialize_node(const embi_node *node, bool scripting, embi_sb *out);
