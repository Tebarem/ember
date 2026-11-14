/*
 * The CSS namespace (W6e): CSS.supports and CSS.escape, written in JavaScript (js/css_ns.js, embedded here) and made the first
 * time a page reads the global CSS. The global is an accessor on the window until then, as the animation classes are
 * (impl_anim.c); reading it replaces the accessor with the data property.
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"
#include "bindings/gen/types.h"

#ifdef EMBER_LZ_ASSETS
#include "ember_lz/js/css_ns.js.lz.h"
#define k_css_ns_js embi_lz_js_css_ns_js
#else
static const unsigned char k_css_ns_js[] = {
#embed "js/css_ns.js"
    , 0
};
#endif

#ifdef EMBER_LZ_ASSETS
#include "ember_lz/js/style_sheet.js.lz.h"
#define k_style_sheet_js embi_lz_js_style_sheet_js
#else
static const unsigned char k_style_sheet_js[] = {
#embed "js/style_sheet.js"
    , 0
};
#endif

static void css_define(embi_bind_ctx *c, embi_js_value v)
{
    embi_js_key k = embi_js_key_c(c->rt, "CSS");
    (void)embi_js_obj_delete(c->rt, c->realm->global, k);
    embi_js_define_raw(c->rt, c->realm->global, k, v, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

static embi_js_value css_get(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_value ns = embi_bind_eval_asset(c, k_css_ns_js, sizeof k_css_ns_js, "css_ns.js");
    if (ns == EMBI_JS_EXCEPTION) {
        return ns;
    }
    css_define(c, ns);
    return ns;
}

static embi_js_value css_set(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    css_define(embi_bind_ctx_of(r), argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

/* HTMLStyleElement.sheet: calls the function of js/style_sheet.js (made once per realm) with the element. */
static embi_js_value style_sheet_get(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (c == nullptr || c->freed) {
        return EMBI_JS_NULL;
    }
    embi_bind_obj *bo = nullptr;
    if (!embi_bind_this(c, this_v, EMBI_IF_HTMLStyleElement, &bo)) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value fn;
    if (c->css_sheet_fn != 0) {
        fn = embi_js_persistent_get(c->rt, c->css_sheet_fn - 1u);
    } else {
        fn = embi_bind_eval_asset(c, k_style_sheet_js, sizeof k_style_sheet_js, "style_sheet.js");
        if (fn == EMBI_JS_EXCEPTION) {
            return fn;
        }
        c->css_sheet_fn = embi_js_persistent_new(c->rt, fn) + 1u;
    }
    return embi_js_call(c->rt, fn, EMBI_JS_UNDEFINED, 1, &this_v);
}

void embi_impl_css_ns_install(embi_bind_ctx *c)
{
    if (embi_js_is_object(c->protos[EMBI_IF_HTMLStyleElement])) {
        embi_js_define_getset(c->realm, embi_js_as_object(c->protos[EMBI_IF_HTMLStyleElement]), "sheet", style_sheet_get,
                              nullptr, 0, EMBI_JS_ATTR_CONFIGURABLE | EMBI_JS_ATTR_ENUMERABLE);
    }
    embi_js_define_getset(c->realm, c->realm->global, "CSS", css_get, css_set, 0, EMBI_JS_ATTR_CONFIGURABLE);
}
