/*
 * Error and its native subtypes: their prototypes (name, message and toString) and the global
 * constructors. A constructor called with new or without it returns a new error object whose
 * message is the first argument when that is given, and whose cause is the `cause` option when
 * the options object has one. Each NativeError constructor inherits from Error. AggregateError
 * (J3a) takes an iterable of errors first and stores it as its errors array.
 */
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/promise.h"

#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* InstallErrorCause: the cause own property when the options object has one. */
static embi_js_value install_cause(embi_js_runtime *rt, embi_js_object *e, embi_js_value options)
{
    if (!embi_js_is_object(options)) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_object *opts = embi_js_as_object(options);
    embi_js_key cause_key = embi_js_key_c(rt, "cause");
    int has = embi_js_obj_has_property(rt, opts, cause_key);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    if (has == EMBI_JS_YES) {
        embi_js_value cause = embi_js_get(rt, opts, cause_key);
        if (cause == EXC) {
            return EXC;
        }
        embi_js_define_raw(rt, e, cause_key, cause, ATTR_WC);
    }
    return EMBI_JS_UNDEFINED;
}

/* The message (when not undefined) and the cause, shared by every error constructor. */
static embi_js_value init_message_and_cause(embi_js_runtime *rt, embi_js_object *e,
                                            embi_js_value message, embi_js_value options)
{
    if (!embi_js_is_undefined(message)) {
        embi_js_string *msg = embi_js_to_string(rt, message);
        if (!msg) {
            return EXC;
        }
        embi_js_define_raw(rt, e, embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE),
                           embi_js_from_string(msg), ATTR_WC);
    }
    return install_cause(rt, e, options);
}

static embi_js_value error_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic,
                                embi_js_value new_target)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, (enum embi_js_proto)magic);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *e = embi_js_object_new_class(rt, proto, &embi_js_class_error);
    embi_js_value message = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_js_value options = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    if (init_message_and_cause(rt, e, message, options) == EXC) {
        return EXC;
    }
    return embi_js_from_object(e);
}

/* AggregateError(errors, message, options) (ECMA-262 20.5.7.1). errors is read with the iterator
 * protocol (IterableToList) and becomes the non-enumerable errors array. */
static embi_js_value aggregate_error_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic,
                                          embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_AGGREGATE_ERROR);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *e = embi_js_object_new_class(rt, proto, &embi_js_class_error);
    embi_js_value message = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    embi_js_value options = argc > 2 ? argv[2] : EMBI_JS_UNDEFINED;
    if (init_message_and_cause(rt, e, message, options) == EXC) {
        return EXC;
    }
    embi_js_value rec = embi_js_ir_open(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (rec == EXC) {
        return EXC;
    }
    embi_js_object *list = embi_js_array_create_default(rt, 0.0);
    for (uint32_t i = 0;; i++) {
        embi_js_value v = EMBI_JS_UNDEFINED;
        int st = embi_js_ir_next(rt, rec, &v);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        if (embi_js_create_data_property_or_throw(rt, list, embi_js_from_int32((int32_t)i), v) ==
            EMBI_JS_THREW) {
            return EXC;
        }
    }
    embi_js_define_raw(rt, e, embi_js_key_c(rt, "errors"), embi_js_from_object(list), ATTR_WC);
    return embi_js_from_object(e);
}

/* The AggregateError of Promise.any: errors is the array of rejection reasons. */
embi_js_object *embi_js_aggregate_error_new(embi_js_runtime *rt, embi_js_value errors)
{
    embi_js_object *e = embi_js_object_new_class(
        rt, rt->protos[EMBI_JS_PROTO_AGGREGATE_ERROR], &embi_js_class_error);
    embi_js_define_raw(rt, e, embi_js_key_c(rt, "errors"), errors, ATTR_WC);
    return e;
}

/* Error.prototype.toString: "name: message", with the defaults and the empty-part rules of
 * ECMA-262 20.5.3.4. */
static embi_js_value error_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Error.prototype.toString called on non-object");
    }
    embi_js_object *o = embi_js_as_object(this_v);
    embi_js_value nv = embi_js_get(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_NAME));
    if (nv == EXC) {
        return EXC;
    }
    embi_js_string *name;
    if (embi_js_is_undefined(nv)) {
        name = embi_js_atom_intern_utf8(rt, "Error", 5);
    } else {
        name = embi_js_to_string(rt, nv);
        if (!name) {
            return EXC;
        }
    }
    embi_js_value mv = embi_js_get(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE));
    if (mv == EXC) {
        return EXC;
    }
    embi_js_string *msg;
    if (embi_js_is_undefined(mv)) {
        msg = embi_js_atom_intern_utf8(rt, "", 0);
    } else {
        msg = embi_js_to_string(rt, mv);
        if (!msg) {
            return EXC;
        }
    }
    if (embi_js_str_len(name) == 0) {
        return embi_js_from_string(msg);
    }
    if (embi_js_str_len(msg) == 0) {
        return embi_js_from_string(name);
    }
    embi_js_string *colon = embi_js_str_new_utf8(rt, ": ", 2);
    if (!colon) {
        return EXC;
    }
    embi_js_string *head = embi_js_str_concat(rt, name, colon);
    if (!head) {
        return EXC;
    }
    embi_js_string *res = embi_js_str_concat(rt, head, msg);
    return res ? embi_js_from_string(res) : EXC;
}

/* Creates a constructor and its prototype for one error type, and binds the constructor globally.
 * base_ctor is the parent constructor (nullptr for Error itself). Returns the constructor. */
static embi_js_object *define_error(embi_js_realm *r, enum embi_js_proto which,
                                    enum embi_js_proto parent, const char *name,
                                    embi_js_object *base_ctor, embi_js_native_ctor ctor_fn,
                                    int length)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[parent]);
    rt->protos[which] = embi_js_from_object(proto);
    embi_js_value name_v = embi_js_from_string(embi_js_atom_intern_utf8(rt, name, strlen(name)));
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_NAME), name_v, ATTR_WC);
    embi_js_value empty = embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE), empty, ATTR_WC);

    embi_js_object *ctor = embi_js_native_constructor_new(r, name, length, ctor_fn, which);
    if (base_ctor) {
        (void)embi_js_obj_set_prototype_of(rt, ctor, embi_js_from_object(base_ctor));
    }
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor), ATTR_WC);
    return ctor;
}

/* Error.isError (ES2026): true for an object that has the [[ErrorData]] slot, which is the error
 * class. Proxies and other objects are false. */
static embi_js_value error_is_error(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)magic;
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    return embi_js_from_bool(embi_js_is_object(v) &&
                             embi_js_as_object(v)->klass == &embi_js_class_error);
}

/* get Error.prototype.stack (the error-stack-accessor proposal): the stack of an object that has
 * the [[ErrorData]] slot, undefined for any other object. The string is the error's header line
 * (Error.prototype.toString's result, "" when that throws), with no frames. */
static embi_js_value error_stack_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Error.prototype.stack getter called on a non-object");
    }
    if (embi_js_as_object(this_v)->klass != &embi_js_class_error) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value head = error_to_string(r, this_v, 0, nullptr, 0);
    if (head == EXC) {
        embi_js_exception_clear(rt);
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0));
    }
    return head;
}

/* set Error.prototype.stack: SetterThatIgnoresPrototypeProperties(this, %Error.prototype%,
 * "stack", v). A non-string v is a TypeError. */
static embi_js_value error_stack_set(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Error.prototype.stack setter called on a non-object");
    }
    if (!embi_js_is_string(v)) {
        return embi_js_throw_type_error(rt, "Error.prototype.stack must be set to a string");
    }
    embi_js_object *o = embi_js_as_object(this_v);
    if (this_v == rt->protos[EMBI_JS_PROTO_ERROR]) {
        return embi_js_throw_type_error(rt, "Cannot assign to read only property 'stack' of Error.prototype");
    }
    embi_js_key k = embi_js_key_c(rt, "stack");
    embi_js_pdesc d;
    int has = embi_js_obj_get_own_property(rt, o, k, &d);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    if (has == EMBI_JS_NO) {
        embi_js_pdesc nd = {v, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                            EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                                EMBI_JS_PD_CONFIGURABLE,
                            EMBI_JS_ATTR_DEFAULT};
        if (embi_js_define_property_or_throw(rt, o, k, &nd) == EMBI_JS_THREW) {
            return EXC;
        }
        return EMBI_JS_UNDEFINED;
    }
    if (embi_js_put(rt, o, k, v, true) == EMBI_JS_THREW) {
        return EXC;
    }
    return EMBI_JS_UNDEFINED;
}

void embi_js_init_builtins_error(embi_js_realm *r)
{
    embi_js_object *err = define_error(r, EMBI_JS_PROTO_ERROR, EMBI_JS_PROTO_OBJECT, "Error",
                                       nullptr, error_ctor, 1);
    embi_js_define_method(r, err, "isError", error_is_error, 1, 0);
    embi_js_define_method(r, embi_js_as_object(r->rt->protos[EMBI_JS_PROTO_ERROR]), "toString",
                          error_to_string, 0, 0);
    embi_js_define_getset(r, embi_js_as_object(r->rt->protos[EMBI_JS_PROTO_ERROR]), "stack",
                          error_stack_get, error_stack_set, 0, EMBI_JS_ATTR_CONFIGURABLE);
    define_error(r, EMBI_JS_PROTO_TYPE_ERROR, EMBI_JS_PROTO_ERROR, "TypeError", err, error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_RANGE_ERROR, EMBI_JS_PROTO_ERROR, "RangeError", err,
                 error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_REFERENCE_ERROR, EMBI_JS_PROTO_ERROR, "ReferenceError", err,
                 error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_SYNTAX_ERROR, EMBI_JS_PROTO_ERROR, "SyntaxError", err,
                 error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_EVAL_ERROR, EMBI_JS_PROTO_ERROR, "EvalError", err, error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_URI_ERROR, EMBI_JS_PROTO_ERROR, "URIError", err, error_ctor, 1);
    define_error(r, EMBI_JS_PROTO_AGGREGATE_ERROR, EMBI_JS_PROTO_ERROR, "AggregateError", err,
                 aggregate_error_ctor, 2);
}
