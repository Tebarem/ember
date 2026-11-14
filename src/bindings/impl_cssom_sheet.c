/*
 * Constructable stylesheets (W7d, the CSSStyleSheet and Document partial in src/bindings/idl/cssom.webidl).
 *
 * A CSSStyleSheet wrapper's record holds one reference to a parsed sheet (src/css/css.h). replaceSync and replace parse
 * the text again and swap the reference. The document's adopted list is a JS array kept in a persistent handle of the
 * context (c->adopted); every change hands the sheets to the embedder (host.adopt), which restyles the document with them
 * after its own sheets. A sheet is parsed without an @import loader (its imports have no sheet), with the same declaration
 * hook as a <style> element.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/cssom.h"
#include "bindings/gen/dom.h"
#include "css/css.h"
#include "css/props.h"

#include <string.h>

/* The record of a CSSStyleSheet wrapper: one reference to its sheet. The finalizer releases the reference (bo->release). */
typedef struct sheet_rec {
    embi_css_sheet *sheet;
} sheet_rec;

static void sheet_rec_release(void *owned)
{
    sheet_rec *r = owned;
    if (r->sheet != nullptr) {
        embi_css_sheet_unref(r->sheet);
        r->sheet = nullptr;
    }
}

/* The record of a CSSStyleSheet wrapper, or nullptr for another object or a stale wrapper. */
static sheet_rec *sheet_of(embi_bind_obj *bo)
{
    if (bo == nullptr || !embi_bind_iface_is(bo->iface, EMBI_IF_CSSStyleSheet) || embi_bind_stale(bo)) {
        return nullptr;
    }
    return bo->ptr;
}

/* A wrapper that takes the reference to sh. */
static embi_js_value sheet_wrap(embi_bind_ctx *c, embi_css_sheet *sh)
{
    sheet_rec *r = embi_alloc(sizeof *r);
    r->sheet = sh;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_CSSStyleSheet, r, r, (uint32_t)sizeof *r);
    if (v == EMBI_JS_EXCEPTION) {
        sheet_rec_release(r);
        embi_free(r, sizeof *r);
        return v;
    }
    embi_bind_obj_of(v)->release = sheet_rec_release;
    return v;
}

/* Parses text as the sheet of a constructed or replaced stylesheet (no loader: @import rules have no sheet). */
static embi_css_sheet *parse_sheet(embi_bind_ctx *c, embi_str text)
{
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, embi_bind_url(c)};
    return embi_css_sheet_parse(text, &opts);
}

/* Hands the adopted sheets of the document to the embedder again (after a change of the array or of one of its sheets). */
static void sync_adopted(embi_bind_ctx *c)
{
    if (c->adopted == 0 || c->host.adopt == nullptr) {
        return;
    }
    embi_js_value arr = embi_js_persistent_get(c->rt, c->adopted - 1u);
    uint32_t n = embi_bind_array_len(c, arr);
    void **v = n > 0 ? embi_alloc((size_t)n * sizeof *v) : nullptr;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        sheet_rec *r = sheet_of(embi_bind_obj_of(embi_bind_array_get(c, arr, i)));
        if (r != nullptr && r->sheet != nullptr) {
            v[k++] = r->sheet;
        }
    }
    c->host.adopt(c->host.user, v, k);
    if (v != nullptr) {
        embi_free(v, (size_t)n * sizeof *v);
    }
}

/* ------------------------------------------------------------------ CSSStyleSheet */

embi_js_value embi_impl_CSSStyleSheet_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    /* The options (media, disabled, baseURL) are not kept: the sheet applies, and its base is the document's. */
    (void)a0;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = sheet_wrap(c, parse_sheet(c, EMBI_STR("")));
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_CSSStyleSheet_get_cssRules(embi_bind_ctx *c, embi_bind_obj *self)
{
    sheet_rec *r = sheet_of(self);
    uint32_t n = r != nullptr && r->sheet != nullptr ? r->sheet->nrules : 0;
    embi_js_value arr = embi_bind_array_new(c, n);
    for (uint32_t i = 0; i < n; i++) {
        /* The CSSRule type numbers of the CSSOM (0 for the rules that have none). */
        static const uint8_t types[] = {
            [EMBI_CSS_RULE_STYLE] = 1,  [EMBI_CSS_RULE_MEDIA] = 4,    [EMBI_CSS_RULE_SUPPORTS] = 12,
            [EMBI_CSS_RULE_IMPORT] = 3, [EMBI_CSS_RULE_FONT_FACE] = 5, [EMBI_CSS_RULE_KEYFRAMES] = 7,
            [EMBI_CSS_RULE_NAMESPACE] = 10,
        };
        uint8_t kind = r->sheet->rules[i].kind;
        uint8_t type = kind < sizeof types ? types[kind] : 0;
        embi_js_value rule = embi_bind_object_new(c);
        embi_bind_object_set_c(c, rule, "type", embi_js_number(type));
        embi_bind_array_set(c, arr, i, rule);
    }
    return arr;
}

embi_dom_err embi_impl_CSSStyleSheet_replaceSync(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    sheet_rec *r = sheet_of(self);
    if (r == nullptr) {
        return EMBI_DOM_OK;
    }
    embi_css_sheet *sh = parse_sheet(c, embi_bind_utf8(c, a0));
    if (r->sheet != nullptr) {
        embi_css_sheet_unref(r->sheet);
    }
    r->sheet = sh;
    /* An adopted sheet has changed: the cascade reads the new rules. */
    sync_adopted(c);
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_CSSStyleSheet_replace(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (sheet_of(self) == nullptr) {
        return ldr_promise_type_error(c, "replace: this is not a CSSStyleSheet");
    }
    (void)embi_impl_CSSStyleSheet_replaceSync(c, self, a0);
    return ldr_promise_resolve(c, embi_js_from_object(self->obj));
}

/* ------------------------------------------------------------------ Document.adoptedStyleSheets */

embi_dom_err embi_impl_Document_get_adoptedStyleSheets(embi_bind_ctx *c, embi_node *self, embi_js_value *out)
{
    (void)self;
    if (c->adopted == 0) {
        embi_js_value arr = embi_bind_array_new(c, 0);
        c->adopted = embi_js_persistent_new(c->rt, arr) + 1u;
    }
    *out = embi_js_persistent_get(c->rt, c->adopted - 1u);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Document_set_adoptedStyleSheets(embi_bind_ctx *c, embi_node *self, embi_js_value v)
{
    (void)self;
    const char *what = "Failed to set 'adoptedStyleSheets' on 'Document'";
    if (!embi_js_is_object(v)) {
        (void)embi_js_throw_type_error(c->rt, "%s: The provided value cannot be converted to a sequence.", what);
        return EMBI_BIND_THREW;
    }
    embi_js_value *items = nullptr;
    uint32_t n = 0;
    if (!embi_bind_seq_open(c, v, what, &items, &n)) {
        return EMBI_BIND_THREW;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (sheet_of(embi_bind_obj_of(items[i])) == nullptr) {
            (void)embi_js_throw_type_error(c->rt, "%s: Failed to convert value to 'CSSStyleSheet'.", what);
            return EMBI_BIND_THREW;
        }
    }
    /* The array is a copy of the values (the setter stores what it was given, not the array itself). */
    embi_js_value arr = embi_bind_array_new(c, n);
    for (uint32_t i = 0; i < n; i++) {
        embi_bind_array_set(c, arr, i, items[i]);
    }
    if (c->adopted == 0) {
        c->adopted = embi_js_persistent_new(c->rt, arr) + 1u;
    } else {
        embi_js_persistent_set(c->rt, c->adopted - 1u, arr);
    }
    sync_adopted(c);
    return EMBI_DOM_OK;
}
