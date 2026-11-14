#include "dom/internal.h"

embi_attr_node *embi_new_attr_node(embi_document *doc, uint8_t ns, embi_atom ns_uri, embi_atom prefix, embi_atom local,
                                   embi_dstr value)
{
    embi_attr_node *a = embi_slab_alloc(doc->slab, sizeof *a);
    SDL_memset(a, 0, sizeof *a);
    a->base.type = EMBI_NODE_ATTR;
    a->base.doc = doc;
    a->ns = ns;
    a->ns_uri = ns == EMBI_NS_OTHER ? ns_uri : 0;
    a->prefix = prefix;
    a->local = local;
    a->value = embi_dstr_copy(doc->slab, value);
    return a;
}

embi_dstr embi_attr_node_value(const embi_attr_node *a)
{
    if (a->owner) {
        const embi_attr *at = embi_el_find_attr(a->owner, a->ns, a->ns_uri, a->local);
        if (at) {
            return at->value;
        }
    }
    return a->value;
}

embi_document *embi_attr_node_doc(const embi_attr_node *a)
{
    return a->owner ? a->owner->base.doc : a->base.doc;
}

void embi_attr_node_set_value(embi_attr_node *a, embi_dstr v)
{
    if (a->owner && embi_el_find_attr(a->owner, a->ns, a->ns_uri, a->local)) {
        embi_el_set_attr_ns_atom(a->owner, a->ns, a->ns_uri, a->prefix, a->local, v);
        return;
    }
    embi_dstr fresh = embi_dstr_copy(a->base.doc->slab, v);
    embi_dstr_free(a->value);
    a->value = fresh;
}

void embi_attr_node_detach(embi_attr_node *a, embi_dstr value)
{
    if (a->owner) {
        a->base.doc = a->owner->base.doc;
    }
    embi_dstr fresh = embi_dstr_copy(a->base.doc->slab, value);
    embi_dstr_free(a->value);
    a->value = fresh;
    a->owner = nullptr;
}

void embi_attr_node_attach(embi_attr_node *a, embi_element *el)
{
    embi_dstr v = a->value;
    a->value = embi_dstr_empty();
    a->owner = el;
    embi_el_put_attr(el, a->ns, a->ns_uri, a->prefix, a->local, v);
    embi_dstr_free(v);
}
