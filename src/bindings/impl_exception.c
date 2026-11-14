/*
 * DOMException (exception.webidl). The payload (code, name, message) lives in the wrapper's
 * owned block, see bind.h. DOMException.prototype inherits from Error.prototype (the [Error]
 * extended attribute of the IDL), so instanceof Error holds.
 */
#include "bindings/impl.h"
#include "bindings/gen/exception.h"

embi_js_value embi_impl_DOMException_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                 embi_dstr a1)
{
    /* a0 is the message, a1 the name (defaults "" and "Error") */
    embi_js_value v = embi_bind_dom_exception(c, embi_bind_legacy_code(a1), a1, a0);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_dstr embi_impl_DOMException_get_name(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((embi_bind_dom_exc *)self->ptr)->name;
}

embi_dstr embi_impl_DOMException_get_message(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((embi_bind_dom_exc *)self->ptr)->message;
}

uint16_t embi_impl_DOMException_get_code(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((embi_bind_dom_exc *)self->ptr)->code;
}
