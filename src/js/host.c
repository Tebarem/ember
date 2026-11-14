/*
 * Host hooks for embedders and the DOM bindings (W5a0): realms with a host global object, the
 * prototype of that global, native functions that carry data, and accessor properties.
 *
 * Every public entry point here runs inside embi_js_enter/embi_js_leave, since each may allocate
 * and so may collect. The realm and native-function internals are in interp.c and interp.h.
 */
#include "base/base.h"
#include "js/heap.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/object.h"
#include "js/runtime.h"

#include <SDL3/SDL_stdinc.h>

void embi_js_realm_set_global_proto(embi_js_realm *r, embi_js_object *proto)
{
    embi_js_runtime *rt = r->rt;
    embi_js_enter(rt);
    (void)embi_js_obj_set_prototype_of(rt, r->global, embi_js_from_object(proto));
    embi_js_leave(rt);
}

embi_js_object *embi_js_native_function_new_data(embi_js_realm *r, const char *name, int length,
                                                 embi_js_native fn, int magic, void *data)
{
    embi_js_runtime *rt = r->rt;
    embi_js_enter(rt);
    embi_js_object *f = embi_js_native_function_new(r, name, length, fn, magic);
    ((embi_js_native_data *)f->internal)->data = data;
    embi_js_leave(rt);
    return f;
}

void *embi_js_native_data_current(embi_js_realm *r)
{
    return r->cur_data;
}

void embi_js_define_getset_data(embi_js_realm *r, embi_js_object *o, const char *name,
                                embi_js_native getter, embi_js_native setter, int magic, uint8_t attrs,
                                void *data)
{
    embi_js_runtime *rt = r->rt;
    embi_js_enter(rt);
    char buf[256];
    embi_js_value g = EMBI_JS_UNDEFINED;
    embi_js_value s = EMBI_JS_UNDEFINED;
    if (getter) {
        SDL_snprintf(buf, sizeof buf, "get %s", name);
        embi_js_object *f = embi_js_native_function_new(r, buf, 0, getter, magic);
        ((embi_js_native_data *)f->internal)->data = data;
        g = embi_js_from_object(f);
    }
    if (setter) {
        SDL_snprintf(buf, sizeof buf, "set %s", name);
        embi_js_object *f = embi_js_native_function_new(r, buf, 1, setter, magic);
        ((embi_js_native_data *)f->internal)->data = data;
        s = embi_js_from_object(f);
    }
    embi_js_define_accessor_raw(rt, o, embi_js_key_c(rt, name), g, s, attrs);
    embi_js_leave(rt);
}

void embi_js_define_getset(embi_js_realm *r, embi_js_object *o, const char *name,
                           embi_js_native getter, embi_js_native setter, int magic, uint8_t attrs)
{
    embi_js_define_getset_data(r, o, name, getter, setter, magic, attrs, nullptr);
}
