/*
 * Document.elementFromPoint and Document.elementsFromPoint (CSSOM View section 6.1), W8b.
 *
 * Both sit on the hit test of the layout (embi_layout_hit_test, W5d2), the one the input of the view uses, so a script
 * sees the element a click at that point would target. The point is in viewport coordinates (CSS px). A point outside
 * the viewport (negative, or past innerWidth/innerHeight) gives null and an empty list, as the spec says.
 *
 * elementFromPoint: the topmost element whose box contains the point in paint order, skipping pointer-events: none and
 * visibility: hidden targets (as browsers do); the document element when the point hits no box (the canvas). A
 * Document other than the realm's own (one made by DOMParser or createHTMLDocument) has no layout: null.
 *
 * elementsFromPoint: every element whose box contains the point (the layout's collect-mode hit test, embi_layout_hit_test_all,
 * which applies the same target rules as the single hit test), topmost first in paint order, each once, and the document
 * element last. The first entry is elementFromPoint's answer; no entries outside the viewport.
 *
 * The methods are defined on Document.prototype when the realm is set up (embi_impl_hit_install, from impl_window.c),
 * like Element.animate in impl_anim.c, so the generated binding tables are not touched.
 */
#include "bindings/impl.h"

#include "dom/dom.h"
#include "layout/layout.h"

enum { M_FROM_POINT, M_ALL_FROM_POINT };

/* The realm's own document behind this_v, or nullptr (another document, or an exception is pending when *threw). */
static embi_document *own_document(embi_bind_ctx *c, embi_js_value this_v, bool *threw)
{
    embi_bind_obj *bo = nullptr;
    *threw = false;
    if (!embi_bind_this(c, this_v, EMBI_IF_Document, &bo)) {
        *threw = true;
        return nullptr;
    }
    embi_document *doc = embi_bind_document(c);
    return bo != nullptr && bo->ptr == (void *)doc ? doc : nullptr;
}

/* The element at (x, y) of the viewport, or nullptr when the point is outside it or nothing can be hit. */
static embi_element *element_at(embi_bind_ctx *c, embi_document *doc, double x, double y)
{
    double vw = 0, vh = 0, dpr = 1;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    if (!(x >= 0 && y >= 0 && x <= vw && y <= vh)) {
        return nullptr; /* also NaN */
    }
    embi_layout_engine *le = embi_bind_layout(c); /* flushes style and layout */
    embi_element *hit = le != nullptr ? embi_layout_hit_test(le, doc, (float)x, (float)y) : nullptr;
    if (hit == nullptr) {
        hit = embi_document_element(doc); /* the canvas: the root element is the answer */
    }
    return hit;
}

/* The elements at (x, y) for elementsFromPoint: every element whose box is hit, topmost first, each once, and the document
 * element last (it is the canvas under the content, as element_at says, so the first entry is element_at's answer). Empty
 * outside the viewport. *n is the length; the array (of *cap entries) is embi_alloc'd, free it with embi_free(list,
 * *cap * sizeof *list). It is nullptr when the point is outside the viewport. */
static embi_element **elements_at(embi_bind_ctx *c, embi_document *doc, double x, double y, uint32_t *n, uint32_t *cap)
{
    *n = 0;
    *cap = 0;
    double vw = 0, vh = 0, dpr = 1;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    if (!(x >= 0 && y >= 0 && x <= vw && y <= vh)) {
        return nullptr; /* also NaN */
    }
    embi_layout_engine *le = embi_bind_layout(c); /* flushes style and layout */
    embi_element **hits = nullptr;
    uint32_t hn = 0;
    if (le != nullptr) {
        embi_layout_hit_test_all(le, doc, (float)x, (float)y, &hits, &hn);
    }
    embi_element *root = embi_document_element(doc);
    embi_element **list = embi_alloc((hn + 1u) * sizeof *list);
    uint32_t m = 0;
    for (uint32_t i = 0; i < hn; i++) {
        if (hits[i] != root) {
            list[m++] = hits[i];
        }
    }
    if (hits != nullptr) {
        embi_free(hits, hn * sizeof *hits);
    }
    if (root != nullptr) {
        list[m++] = root;
    }
    *n = m;
    *cap = hn + 1u;
    return list;
}

static embi_js_value hit_native(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (c == nullptr) {
        return embi_js_throw_type_error(r->rt, "Illegal invocation");
    }
    const char *what = magic == M_FROM_POINT ? "elementFromPoint" : "elementsFromPoint";
    if (argc < 2) {
        char msg[160];
        SDL_snprintf(msg, sizeof msg, "Failed to execute '%s' on 'Document': 2 arguments required, but only %d present.",
                     what, argc);
        return embi_js_throw_type_error(r->rt, "%s", msg);
    }
    bool threw = false;
    embi_document *doc = own_document(c, this_v, &threw);
    if (threw) {
        return EMBI_JS_EXCEPTION;
    }
    /* double x, double y: ToNumber, then non-finite values throw (WebIDL "double", not "unrestricted double"). */
    double xy[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (!embi_js_to_number(c->rt, argv[i], &xy[i])) {
            return EMBI_JS_EXCEPTION;
        }
        if (!SDL_isinf(xy[i]) && !SDL_isnan(xy[i])) {
            continue;
        }
        char msg[160];
        SDL_snprintf(msg, sizeof msg, "Failed to execute '%s' on 'Document': The provided double value is non-finite.",
                     what);
        return embi_js_throw_type_error(r->rt, "%s", msg);
    }
    if (magic == M_FROM_POINT) {
        embi_element *hit = doc != nullptr ? element_at(c, doc, xy[0], xy[1]) : nullptr;
        return hit != nullptr ? embi_bind_node_value(c, (embi_node *)hit) : EMBI_JS_NULL;
    }
    uint32_t n = 0, cap = 0;
    embi_element **list = doc != nullptr ? elements_at(c, doc, xy[0], xy[1], &n, &cap) : nullptr;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value arr = embi_bind_pin(c, embi_bind_array_new(c, n)); /* pinned while the wrappers are made */
    for (uint32_t i = 0; i < n; i++) {
        embi_bind_array_set(c, arr, i, embi_bind_node_value(c, (embi_node *)list[i]));
    }
    if (list != nullptr) {
        embi_free(list, cap * sizeof *list);
    }
    return embi_bind_leave(sc, arr);
}

void embi_impl_hit_install(embi_bind_ctx *c)
{
    if (!embi_js_is_object(c->protos[EMBI_IF_Document])) {
        return;
    }
    embi_js_object *dp = embi_js_as_object(c->protos[EMBI_IF_Document]);
    embi_js_define_method(c->realm, dp, "elementFromPoint", hit_native, 2, M_FROM_POINT);
    embi_js_define_method(c->realm, dp, "elementsFromPoint", hit_native, 2, M_ALL_FROM_POINT);
}
