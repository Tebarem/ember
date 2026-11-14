#include "dom/internal.h"
#include "dom/mutation.h"

static const char *const g_err_names[] = {
    "OK",
    "HierarchyRequestError",
    "NotFoundError",
    "InvalidCharacterError",
    "NamespaceError",
    "NotSupportedError",
};

const char *embi_dom_err_name(embi_dom_err e)
{
    return g_err_names[e];
}

embi_document *embi_document_new(bool is_html)
{
    embi_dom_init();
    embi_document *doc = embi_calloc(1, sizeof *doc);
    doc->base.type = EMBI_NODE_DOCUMENT;
    doc->base.flags = EMBI_NF_CONNECTED;
    doc->base.doc = doc;
    doc->slab = embi_slab_new();
    doc->is_html = is_html;
    doc->quirks = EMBI_QUIRKS_NONE;
    embi_hashmap_init(&doc->idmap);
    doc->idmap_dirty = true;
    return doc;
}

embi_document *embi_document_template_owner(embi_document *doc)
{
    if (doc->inert) {
        return doc;
    }
    if (!doc->template_owner) {
        embi_document *owner = embi_document_new(doc->is_html);
        owner->inert = true;
        doc->template_owner = owner;
    }
    return doc->template_owner;
}

void embi_document_free(embi_document *doc)
{
    if (!doc) {
        return;
    }
    if (doc->template_owner) {
        embi_document_free(doc->template_owner);
        doc->template_owner = nullptr;
    }
    embi_hashmap_free(&doc->idmap);
    embi_mut_free(doc);
    embi_vec_free(&doc->observers);
    /* Every node that belongs to this document dies with it. Slabs it keeps alive are held only for
     * memory that other documents still reach, and those documents keep their own references. Drop
     * ours now so that a cycle of documents that hold each other's nodes can be freed. */
    embi_slab_drop_kept(doc->slab);
    embi_slab_release(doc->slab);
    embi_free(doc, sizeof *doc);
}

void embi_document_observe(embi_document *doc, const embi_observer *obs)
{
    embi_vec_push(&doc->observers, *obs);
}

void embi_document_unobserve(embi_document *doc, void *ctx)
{
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        if (doc->observers.data[i].ctx == ctx) {
            for (uint32_t j = i + 1; j < doc->observers.len; j++) {
                doc->observers.data[j - 1] = doc->observers.data[j];
            }
            doc->observers.len--;
            return;
        }
    }
}

embi_element *embi_document_element(embi_document *doc)
{
    for (embi_node *n = doc->base.first_child; n; n = n->next) {
        if (n->type == EMBI_NODE_ELEMENT) {
            return (embi_element *)n;
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ observer dispatch */

void embi_notify_children(embi_node *parent, embi_node *first_added, embi_node *last_added,
                          embi_node *first_removed, embi_node *last_removed)
{
    embi_document *doc = parent->doc;
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->children_changed) {
            o->children_changed(o->ctx, parent, first_added, last_added, first_removed,
                                last_removed);
        }
    }
}

void embi_notify_attribute(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom name,
                           embi_dstr old, bool had_old)
{
    embi_document *doc = el->base.doc;
    embi_mut_attribute(el, ns, ns_uri, name, old, had_old);
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->attribute_changed) {
            o->attribute_changed(o->ctx, el, ns, ns_uri, name, old, had_old);
        }
    }
}

void embi_notify_char_data(embi_node *node)
{
    embi_document *doc = node->doc;
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->character_data_changed) {
            o->character_data_changed(o->ctx, node);
        }
    }
}

void embi_notify_inserted(embi_node *node)
{
    embi_document *doc = node->doc;
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->node_inserted) {
            o->node_inserted(o->ctx, node);
        }
    }
}

void embi_notify_removed(embi_node *node)
{
    embi_document *doc = node->doc;
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->node_removed) {
            o->node_removed(o->ctx, node);
        }
    }
}

void embi_notify_pre_remove(embi_node *node)
{
    embi_document *doc = node->doc;
    for (uint32_t i = 0; i < doc->observers.len; i++) {
        embi_observer *o = &doc->observers.data[i];
        if (o->pre_remove) {
            o->pre_remove(o->ctx, node);
        }
    }
}
