/*
 * document.fonts (fonts.webidl): the FontFaceSet of a document. Split from impl_canvas_data.c by SZ3b: it is not part of the
 * canvas feature, so it is built whether EMBER_CANVAS is on or off.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/fonts.h"

embi_js_value embi_impl_Document_get_fonts(embi_bind_ctx *c, embi_node *self)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_FontFaceSet, self, nullptr, 0);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo != nullptr) {
        embi_bind_set_ref(bo, embi_bind_node_value(c, self)); /* keeps the document alive */
    }
    return v;
}

/* A promise that settles one task later: the @font-face sources of the document are registered when its style sheets
 * are collected (the embedder does that between a script and the next frame), so a script that awaits fonts.ready sees
 * the faces. */
static embi_js_value fonts_later(embi_bind_ctx *c)
{
    static const char src[] = "(function(){return new Promise(function(r){setTimeout(function(){r(undefined);},0);});})()";
    embi_js_value v = embi_js_eval(c->realm, src, sizeof src - 1, "fonts.ready", 0);
    return v;
}

embi_js_value embi_impl_FontFaceSet_get_ready(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return fonts_later(c);
}

embi_dstr embi_impl_FontFaceSet_get_status(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    (void)c;
    return embi_impl_lit("loaded");
}

bool embi_impl_FontFaceSet_check(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    (void)c;
    (void)self;
    (void)a0;
    (void)a1;
    return true;
}

embi_js_value embi_impl_FontFaceSet_load(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    (void)self;
    (void)a0;
    (void)a1;
    return fonts_later(c);
}
