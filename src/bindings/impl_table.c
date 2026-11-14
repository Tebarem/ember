/*
 * The table structure API of HTML (W5f): HTMLTableElement caption/tHead/tFoot/tBodies/rows and their create/delete/insertRow
 * members, HTMLTableSectionElement.rows, HTMLTableRowElement cells/rowIndex/sectionRowIndex/insertCell/deleteCell and
 * HTMLTableCellElement.cellIndex. The collections are live (impl_coll.c, EMBI_COLL_TBODIES .. EMBI_COLL_CELLS).
 */
#include "bindings/impl.h"
#include "bindings/gen/html.h"
#include "dom/internal.h"

static bool html_el(const embi_node *n, embi_atom name)
{
    return n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_HTML && n->name == name;
}

static bool is_section(const embi_node *n)
{
    return html_el(n, EMBI_ATOM_thead) || html_el(n, EMBI_ATOM_tbody) || html_el(n, EMBI_ATOM_tfoot);
}

static embi_node *new_html(embi_node *like, embi_atom name)
{
    return &embi_new_element(like->doc, EMBI_NS_HTML, 0, 0, name)->base;
}

static embi_node *first_child_named(embi_node *parent, embi_atom name)
{
    for (embi_node *k = parent->first_child; k; k = k->next) {
        if (html_el(k, name)) {
            return k;
        }
    }
    return nullptr;
}

static void remove_child_named(embi_node *parent, embi_atom name)
{
    embi_node *k = first_child_named(parent, name);
    if (k) {
        embi_detach(k);
        embi_destroy_if_unowned(k);
    }
}

/* ------------------------------------------------------------------ HTMLTableElement */

embi_node *embi_impl_HTMLTableElement_get_caption(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return first_child_named(self, EMBI_ATOM_caption);
}

embi_node *embi_impl_HTMLTableElement_get_tHead(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return first_child_named(self, EMBI_ATOM_thead);
}

embi_node *embi_impl_HTMLTableElement_get_tFoot(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return first_child_named(self, EMBI_ATOM_tfoot);
}

embi_js_value embi_impl_HTMLTableElement_get_tBodies(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_coll_new(c, EMBI_COLL_TBODIES, self, (embi_str){"", 0}, (embi_str){"", 0}, false);
}

embi_js_value embi_impl_HTMLTableElement_get_rows(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_coll_new(c, EMBI_COLL_TABLE_ROWS, self, (embi_str){"", 0}, (embi_str){"", 0}, false);
}

embi_node *embi_impl_HTMLTableElement_createCaption(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *cap = first_child_named(self, EMBI_ATOM_caption);
    if (!cap) {
        cap = new_html(self, EMBI_ATOM_caption);
        (void)embi_pre_insert(self, cap, self->first_child);
    }
    return cap;
}

void embi_impl_HTMLTableElement_deleteCaption(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    remove_child_named(self, EMBI_ATOM_caption);
}

embi_node *embi_impl_HTMLTableElement_createTHead(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *h = first_child_named(self, EMBI_ATOM_thead);
    if (!h) {
        h = new_html(self, EMBI_ATOM_thead);
        /* before the first element child that is not a caption or a colgroup (other nodes are skipped) */
        embi_node *ref = self->first_child;
        while (ref && (ref->type != EMBI_NODE_ELEMENT || html_el(ref, EMBI_ATOM_caption) ||
                       html_el(ref, EMBI_ATOM_colgroup))) {
            ref = ref->next;
        }
        (void)embi_pre_insert(self, h, ref);
    }
    return h;
}

void embi_impl_HTMLTableElement_deleteTHead(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    remove_child_named(self, EMBI_ATOM_thead);
}

embi_node *embi_impl_HTMLTableElement_createTFoot(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *f = first_child_named(self, EMBI_ATOM_tfoot);
    if (!f) {
        f = new_html(self, EMBI_ATOM_tfoot);
        (void)embi_pre_insert(self, f, nullptr);
    }
    return f;
}

void embi_impl_HTMLTableElement_deleteTFoot(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    remove_child_named(self, EMBI_ATOM_tfoot);
}

/* After the last tbody child, or at the end when there is none. */
static embi_node *append_tbody(embi_node *table)
{
    embi_node *last = nullptr;
    for (embi_node *k = table->first_child; k; k = k->next) {
        if (html_el(k, EMBI_ATOM_tbody)) {
            last = k;
        }
    }
    embi_node *tb = new_html(table, EMBI_ATOM_tbody);
    (void)embi_pre_insert(table, tb, last ? last->next : nullptr);
    return tb;
}

embi_node *embi_impl_HTMLTableElement_createTBody(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return append_tbody(self);
}

/* The rows of a table in the order of table.rows, in the scratch memory of the scope. */
static uint32_t table_rows_list(embi_bind_ctx *c, embi_node *table, embi_node ***out)
{
    uint32_t cap = 0;
    for (embi_node *k = table->first_child; k; k = k->next) {
        cap++;
        for (embi_node *r = k->first_child; r; r = r->next) {
            cap++;
        }
    }
    embi_node **v = (embi_node **)embi_bind_alloc(c, ((size_t)cap + 1) * sizeof *v);
    uint32_t n = 0;
    embi_node *thead = first_child_named(table, EMBI_ATOM_thead);
    embi_node *tfoot = first_child_named(table, EMBI_ATOM_tfoot);
    for (embi_node *r = thead ? thead->first_child : nullptr; r; r = r->next) {
        if (html_el(r, EMBI_ATOM_tr)) {
            v[n++] = r;
        }
    }
    for (embi_node *k = table->first_child; k; k = k->next) {
        if (html_el(k, EMBI_ATOM_tr)) {
            v[n++] = k;
        } else if (html_el(k, EMBI_ATOM_tbody)) {
            for (embi_node *r = k->first_child; r; r = r->next) {
                if (html_el(r, EMBI_ATOM_tr)) {
                    v[n++] = r;
                }
            }
        }
    }
    for (embi_node *r = tfoot ? tfoot->first_child : nullptr; r; r = r->next) {
        if (html_el(r, EMBI_ATOM_tr)) {
            v[n++] = r;
        }
    }
    *out = v;
    return n;
}

static embi_dom_err index_error(embi_bind_ctx *c)
{
    return embi_impl_throw(c, "IndexSizeError", "The index is not in the allowed range.");
}

embi_dom_err embi_impl_HTMLTableElement_insertRow(embi_bind_ctx *c, embi_node *self, int32_t a0, embi_node **out)
{
    embi_node **rows;
    uint32_t n = table_rows_list(c, self, &rows);
    if (a0 < -1 || (int64_t)a0 > (int64_t)n) {
        return index_error(c);
    }
    embi_node *tr = new_html(self, EMBI_ATOM_tr);
    if (n == 0) {
        embi_node *tb = nullptr;
        for (embi_node *k = self->first_child; k; k = k->next) {
            if (html_el(k, EMBI_ATOM_tbody)) {
                tb = k; /* the last tbody child */
            }
        }
        if (!tb) {
            tb = new_html(self, EMBI_ATOM_tbody);
            (void)embi_pre_insert(self, tb, nullptr);
        }
        (void)embi_pre_insert(tb, tr, nullptr);
    } else if (a0 == -1 || (uint32_t)a0 == n) {
        embi_node *last = rows[n - 1];
        (void)embi_pre_insert(last->parent, tr, nullptr);
    } else {
        embi_node *ref = rows[a0];
        (void)embi_pre_insert(ref->parent, tr, ref);
    }
    *out = tr;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLTableElement_deleteRow(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_node **rows;
    uint32_t n = table_rows_list(c, self, &rows);
    if (a0 < -1 || (int64_t)a0 >= (int64_t)n) {
        if (a0 == -1 && n == 0) {
            return EMBI_DOM_OK;
        }
        return index_error(c);
    }
    embi_node *victim = rows[a0 == -1 ? n - 1 : (uint32_t)a0];
    embi_detach(victim);
    embi_destroy_if_unowned(victim);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ HTMLTableSectionElement */

embi_js_value embi_impl_HTMLTableSectionElement_get_rows(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_coll_new(c, EMBI_COLL_SECTION_ROWS, self, (embi_str){"", 0}, (embi_str){"", 0}, false);
}

static uint32_t children_named(embi_node *parent, embi_atom a, embi_atom b, embi_node **at, int64_t want)
{
    uint32_t n = 0;
    for (embi_node *k = parent->first_child; k; k = k->next) {
        if (html_el(k, a) || (b && html_el(k, b))) {
            if ((int64_t)n == want && at) {
                *at = k;
            }
            n++;
        }
    }
    return n;
}

embi_dom_err embi_impl_HTMLTableSectionElement_insertRow(embi_bind_ctx *c, embi_node *self, int32_t a0, embi_node **out)
{
    embi_node *at = nullptr;
    uint32_t n = children_named(self, EMBI_ATOM_tr, 0, &at, a0);
    if (a0 < -1 || (int64_t)a0 > (int64_t)n) {
        return index_error(c);
    }
    embi_node *tr = new_html(self, EMBI_ATOM_tr);
    (void)embi_pre_insert(self, tr, (a0 == -1 || (uint32_t)a0 == n) ? nullptr : at);
    *out = tr;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLTableSectionElement_deleteRow(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_node *at = nullptr;
    uint32_t n = children_named(self, EMBI_ATOM_tr, 0, &at, a0 == -1 ? -2 : a0);
    if (a0 < -1 || (int64_t)a0 >= (int64_t)n) {
        if (a0 == -1 && n == 0) {
            return EMBI_DOM_OK;
        }
        return index_error(c);
    }
    if (a0 == -1) {
        (void)children_named(self, EMBI_ATOM_tr, 0, &at, (int64_t)n - 1);
    }
    embi_detach(at);
    embi_destroy_if_unowned(at);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ HTMLTableRowElement */

static int32_t index_in(embi_node **v, uint32_t n, const embi_node *x)
{
    for (uint32_t i = 0; i < n; i++) {
        if (v[i] == x) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t embi_impl_HTMLTableRowElement_get_rowIndex(embi_bind_ctx *c, embi_node *self)
{
    embi_node *t = self->parent;
    while (t && !html_el(t, EMBI_ATOM_table)) {
        t = t->parent;
    }
    if (!t) {
        return -1;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_node **rows;
    uint32_t n = table_rows_list(c, t, &rows);
    int32_t r = index_in(rows, n, self);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return r;
}

int32_t embi_impl_HTMLTableRowElement_get_sectionRowIndex(embi_bind_ctx *c, embi_node *self)
{
    embi_node *p = self->parent;
    if (!p) {
        return -1;
    }
    if (is_section(p)) {
        int32_t i = 0;
        for (embi_node *k = p->first_child; k; k = k->next) {
            if (k == self) {
                return i;
            }
            i += html_el(k, EMBI_ATOM_tr);
        }
        return -1;
    }
    if (html_el(p, EMBI_ATOM_table)) {
        embi_bind_scope sc = embi_bind_enter(c);
        embi_node **rows;
        uint32_t n = table_rows_list(c, p, &rows);
        int32_t r = index_in(rows, n, self);
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return r;
    }
    return -1;
}

embi_js_value embi_impl_HTMLTableRowElement_get_cells(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_coll_new(c, EMBI_COLL_CELLS, self, (embi_str){"", 0}, (embi_str){"", 0}, false);
}

embi_dom_err embi_impl_HTMLTableRowElement_insertCell(embi_bind_ctx *c, embi_node *self, int32_t a0, embi_node **out)
{
    embi_node *at = nullptr;
    uint32_t n = children_named(self, EMBI_ATOM_td, EMBI_ATOM_th, &at, a0);
    if (a0 < -1 || (int64_t)a0 > (int64_t)n) {
        return index_error(c);
    }
    embi_node *td = new_html(self, EMBI_ATOM_td);
    (void)embi_pre_insert(self, td, (a0 == -1 || (uint32_t)a0 == n) ? nullptr : at);
    *out = td;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLTableRowElement_deleteCell(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_node *at = nullptr;
    uint32_t n = children_named(self, EMBI_ATOM_td, EMBI_ATOM_th, &at, a0 == -1 ? -2 : a0);
    if (a0 < -1 || (int64_t)a0 >= (int64_t)n) {
        if (a0 == -1 && n == 0) {
            return EMBI_DOM_OK;
        }
        return index_error(c);
    }
    if (a0 == -1) {
        (void)children_named(self, EMBI_ATOM_td, EMBI_ATOM_th, &at, (int64_t)n - 1);
    }
    embi_detach(at);
    embi_destroy_if_unowned(at);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ HTMLTableCellElement */

int32_t embi_impl_HTMLTableCellElement_get_cellIndex(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *p = self->parent;
    if (!p || !html_el(p, EMBI_ATOM_tr)) {
        return -1;
    }
    int32_t i = 0;
    for (embi_node *k = p->first_child; k; k = k->next) {
        if (k == self) {
            return i;
        }
        i += html_el(k, EMBI_ATOM_td) || html_el(k, EMBI_ATOM_th);
    }
    return -1;
}
