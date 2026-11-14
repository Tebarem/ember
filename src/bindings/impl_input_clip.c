/*
 * The clipboard of the bindings (W5d): navigator.clipboard.readText and writeText, and the
 * copy, cut and paste events with a text/plain clipboardData. The text goes through src/platform/clipboard.c, which is
 * SDL's clipboard (SDL_GetClipboardText, SDL_SetClipboardText), or a process-local buffer when SDL's video is not up.
 * Permission is always granted in an embedder context, so the promises settle at once.
 */
#include "bindings/impl_input.h"

#include "bindings/gen/events.h"
#include "bindings/gen/uievents.h"
#include "bindings/gen/window.h"
#include "dom/dstr.h"
#include "dom/event.h"
#include "platform/platform.h"

#include <string.h>

/* ------------------------------------------------------------------ navigator.clipboard */

embi_js_value embi_impl_Navigator_get_clipboard(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_other_value(c, EMBI_IF_Clipboard, c, nullptr, 0);
}

embi_js_value embi_impl_Clipboard_readText(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_sb text = {0};
    embi_js_value p;
    if (embi_pf_clipboard_get(&text)) {
        p = embi_bind_promise_resolved(c, embi_bind_utf8_value(c, text.len > 0 ? (embi_str){text.data, text.len} : EMBI_STR("")));
    } else {
        embi_js_value e = embi_bind_throw_named(c, "NotAllowedError", "the clipboard cannot be read");
        (void)e;
        p = embi_bind_promise_from_pending(c);
    }
    embi_sb_free(&text);
    return embi_bind_leave(sc, p);
}

embi_js_value embi_impl_Clipboard_writeText(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p;
    if (embi_pf_clipboard_set(embi_bind_utf8(c, a0))) {
        p = embi_bind_promise_resolved(c, EMBI_JS_UNDEFINED);
    } else {
        (void)embi_bind_throw_named(c, "NotAllowedError", "the clipboard cannot be written");
        p = embi_bind_promise_from_pending(c);
    }
    return embi_bind_leave(sc, p);
}

/* ------------------------------------------------------------------ copy, cut and paste events */

bool embi_impl_input_clipboard_event(embi_bind_ctx *c, embi_node *target, const char *type, const char *in_text,
                                     embi_sb *out, bool *has_text)
{
    embi_bind_scope sc = embi_bind_enter(c);
    if (has_text != nullptr) {
        *has_text = false;
    }
    bool ok = true;
    embi_js_value dt = embi_impl_DataTransfer_constructor(c, EMBI_JS_UNDEFINED);
    embi_bind_obj *dto = dt == EMBI_JS_EXCEPTION ? nullptr : embi_bind_obj_of(dt);
    if (dto == nullptr) {
        embi_js_exception_clear(c->rt);
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return true;
    }
    (void)embi_bind_pin(c, dt);
    embi_dstr fmt = embi_dstr_empty();
    embi_js_value fmt_v = embi_bind_pin(c, embi_bind_utf8_value(c, EMBI_STR("text/plain")));
    (void)embi_bind_to_dstr(c, fmt_v, &fmt);
    if (in_text != nullptr && in_text[0] != '\0') {
        embi_dstr text = embi_dstr_empty();
        embi_js_value tv = embi_bind_pin(c, embi_bind_utf8_value(c, (embi_str){in_text, (uint32_t)strlen(in_text)}));
        if (embi_bind_to_dstr(c, tv, &text)) {
            embi_impl_DataTransfer_setData(c, dto, fmt, text);
        }
    }
    embi_js_value pl = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, pl, "clipboardData", dt);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_ClipboardEvent, type,
                                            EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED | EMBI_EVF_TRUSTED, pl);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        (void)embi_bind_pin(c, ev);
        embi_bind_obj *rec = embi_impl_node_rec(c, target);
        ok = embi_impl_dispatch_value(c, rec, ev);
    }
    if (out != nullptr) {
        embi_dstr got = embi_impl_DataTransfer_getData(c, dto, fmt);
        if (got.len > 0) {
            embi_dstr_append_utf8(out, got);
            if (has_text != nullptr) {
                *has_text = true;
            }
        }
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}
