#include "dom/internal.h"

void embi_document_idmap_invalidate(embi_document *doc)
{
    doc->idmap_dirty = true;
}

/* Rebuilds the map from the connected tree. The first element in tree order wins. */
static void idmap_rebuild(embi_document *doc)
{
    embi_hashmap_clear(&doc->idmap);
    for (embi_node *n = doc->base.first_child; n; n = embi_next_in_tree(&doc->base, n)) {
        if (n->type != EMBI_NODE_ELEMENT) {
            continue;
        }
        embi_atom id = embi_el_id((embi_element *)n);
        uint64_t existing = 0;
        if (id && !embi_hashmap_get(&doc->idmap, id, &existing)) {
            embi_hashmap_put(&doc->idmap, id, (uint64_t)(uintptr_t)n);
        }
    }
    doc->idmap_dirty = false;
}

embi_element *embi_get_element_by_id(embi_document *doc, embi_str id)
{
    if (id.len == 0) {
        return nullptr;
    }
    if (doc->idmap_dirty) {
        idmap_rebuild(doc);
    }
    /* A string that was never interned cannot be an id of any element. */
    embi_atom key = embi_atom_find(id.ptr, id.len);
    if (key == 0) {
        return nullptr;
    }
    uint64_t value = 0;
    if (embi_hashmap_get(&doc->idmap, key, &value)) {
        return (embi_element *)(uintptr_t)value;
    }
    return nullptr;
}
