/*
 * HTMLCanvasElement without the canvas feature (EMBER_CANVAS=OFF, SZ3b). Built instead of impl_canvas.c,
 * impl_canvas_path.c and impl_canvas_data.c, whose interfaces (CanvasRenderingContext2D, Path2D, CanvasGradient,
 * CanvasPattern, ImageData, ImageBitmap, TextMetrics, createImageBitmap) are not bound.
 *
 * The element itself stays, so that the common probes work: getContext() returns null for every context id (what a browser
 * says for a context type it does not support), toDataURL() gives the empty data URL and toBlob() calls back with null.
 * Layout is unchanged: the element stays an empty replaced box of its width and height (the fallback content is not shown).
 */
#include "bindings/gen/htmlcanvas.h"
#include "bindings/impl.h"

uint32_t embi_impl_HTMLCanvasElement_get_width(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    uint32_t v = embi_bind_reflect_get_ulong((embi_element *)self, EMBI_STR("width"));
    return v != 0 ? v : 300u;
}

uint32_t embi_impl_HTMLCanvasElement_get_height(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    uint32_t v = embi_bind_reflect_get_ulong((embi_element *)self, EMBI_STR("height"));
    return v != 0 ? v : 150u;
}

void embi_impl_HTMLCanvasElement_set_width(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    (void)c;
    embi_bind_reflect_set_ulong((embi_element *)self, EMBI_STR("width"), a0 > 2147483647u ? 300u : a0);
}

void embi_impl_HTMLCanvasElement_set_height(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    (void)c;
    embi_bind_reflect_set_ulong((embi_element *)self, EMBI_STR("height"), a0 > 2147483647u ? 150u : a0);
}

embi_js_value embi_impl_HTMLCanvasElement_getContext(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_js_value a1)
{
    (void)c;
    (void)self;
    (void)a0;
    (void)a1;
    return EMBI_JS_NULL;
}

embi_dom_err embi_impl_HTMLCanvasElement_toDataURL(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_js_value a1,
                                                   embi_dstr *out)
{
    (void)c;
    (void)self;
    (void)a0;
    (void)a1;
    *out = embi_impl_lit("data:,");
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLCanvasElement_toBlob(embi_bind_ctx *c, embi_node *self, embi_js_value a0, embi_dstr a1,
                                                embi_js_value a2)
{
    (void)self;
    (void)a1;
    (void)a2;
    if (!embi_js_is_callable(a0)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'toBlob' on 'HTMLCanvasElement': The callback provided as parameter 1 is not a function.");
        return EMBI_BIND_THREW;
    }
    embi_js_value arg = EMBI_JS_NULL;
    embi_js_value r = embi_js_call(c->rt, a0, EMBI_JS_UNDEFINED, 1, &arg);
    if (r == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    }
    return EMBI_DOM_OK;
}

/* The hooks of bind.c: there is no canvas state to keep. */
void embi_impl_canvas_attr_changed(embi_bind_ctx *c, embi_element *el, embi_atom local)
{
    (void)c;
    (void)el;
    (void)local;
}

void embi_impl_canvas_release(embi_bind_ctx *c)
{
    (void)c;
}
