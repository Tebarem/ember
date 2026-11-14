/*
 * Boolean: the Boolean constructor (called as a function it converts with ToBoolean, with new it
 * makes a Boolean wrapper) and Boolean.prototype, itself a Boolean wrapper of false, with
 * toString and valueOf.
 */
#include "js/builtins.h"
#include "js/js.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* thisBooleanValue: a boolean, or a Boolean wrapper. */
static bool this_boolean(embi_js_value v, bool *out)
{
    if (embi_js_is_bool(v)) {
        *out = embi_js_as_bool(v);
        return true;
    }
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_boolean) {
        *out = embi_js_as_bool(embi_js_wrapper_value(embi_js_as_object(v)));
        return true;
    }
    return false;
}

static embi_js_value boolean_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    return embi_js_from_bool(argc > 0 && embi_js_to_boolean(argv[0]));
}

static embi_js_value boolean_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                       const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    bool b = argc > 0 && embi_js_to_boolean(argv[0]);
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_BOOLEAN);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, &embi_js_class_boolean);
    o->internal = (void *)(uintptr_t)embi_js_from_bool(b);
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

static const embi_js_class boolean_ctor_class = {
    .name = "Function",
    .call = boolean_call,
    .construct = boolean_construct,
};

/* toString (magic 0) and valueOf (magic 1). */
static embi_js_value boolean_proto_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    bool b;
    if (!this_boolean(this_v, &b)) {
        return embi_js_throw_type_error(rt, "Boolean.prototype.%s requires that 'this' be a Boolean",
                                        magic ? "valueOf" : "toString");
    }
    if (magic) {
        return embi_js_from_bool(b);
    }
    const char *text = b ? "true" : "false";
    embi_js_string *s = embi_js_str_new_latin1(rt, (const uint8_t *)text, strlen(text));
    return s ? embi_js_from_string(s) : EXC;
}

void embi_js_init_builtins_boolean(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];

    /* Boolean.prototype is a Boolean wrapper whose value is false. */
    embi_js_object *proto = embi_js_object_new_class(rt, obj_proto, &embi_js_class_boolean);
    proto->internal = (void *)(uintptr_t)EMBI_JS_FALSE;
    EMBI_JS_WB_STRUCT(proto);
    rt->protos[EMBI_JS_PROTO_BOOLEAN] = embi_js_from_object(proto);

    embi_js_object *ctor = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION],
                                                    &boolean_ctor_class);
    ctor->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(1.0),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "Boolean", 7)), ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Boolean"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, proto, "toString", boolean_proto_method, 0, 0);
    embi_js_define_method(r, proto, "valueOf", boolean_proto_method, 0, 1);
}
