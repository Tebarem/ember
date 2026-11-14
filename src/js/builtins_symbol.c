/*
 * Symbol (ECMA-262 20.4): the constructor (callable, never constructible), Symbol.for and
 * Symbol.keyFor, the well-known symbol properties, and Symbol.prototype (description, toString,
 * valueOf, @@toPrimitive, @@toStringTag). Symbol wrapper objects use embi_js_class_symbol from
 * object.c, and the hook points for the well-known symbols live in ops.c and object.c.
 *
 * The Symbol.for registry is an ordinary object with a null prototype, from key string to symbol.
 * It belongs to the runtime (rt->sym_registry, created by the first Symbol.for call), so every
 * realm of a runtime shares it, as the spec requires. Registered symbols carry
 * EMBI_JS_FLAG_SYM_REGISTERED.
 */
#include "js/builtins.h"
#include "js/js.h"
#include "js/string.h"

#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (ATTR_W | ATTR_C)

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value atom_val(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* thisSymbolValue: the symbol itself, or the symbol of a Symbol wrapper. Returns EXC when this is
 * neither, with a TypeError pending. */
static embi_js_value this_symbol(embi_js_runtime *rt, embi_js_value v, const char *method)
{
    if (embi_js_is_symbol(v)) {
        return v;
    }
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_symbol) {
        return embi_js_wrapper_value(embi_js_as_object(v));
    }
    return embi_js_throw_type_error(rt, "Symbol.prototype.%s requires that 'this' be a Symbol",
                                    method);
}

/* SymbolDescriptiveString: "Symbol(" + description + ")", the description empty when undefined. */
static embi_js_value descriptive_string(embi_js_runtime *rt, embi_js_value sym)
{
    embi_js_value d = embi_js_as_symbol(sym)->description;
    embi_js_string *open = embi_js_str_new_latin1(rt, (const uint8_t *)"Symbol(", 7);
    embi_js_string *s = open;
    if (embi_js_is_string(d)) {
        s = embi_js_str_concat(rt, open, embi_js_as_string(d));
    }
    embi_js_string *close = embi_js_str_new_latin1(rt, (const uint8_t *)")", 1);
    s = embi_js_str_concat(rt, s, close);
    return embi_js_from_string(s);
}

/* ------------------------------------------------------------------ constructor */

/* Symbol(description). Not a constructor in practice: [[Construct]] throws, which the constructor
 * class below provides so that IsConstructor(Symbol) is true as the spec requires. */
static embi_js_value symbol_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    embi_js_value desc = arg_at(argc, argv, 0);
    if (!embi_js_is_undefined(desc)) {
        embi_js_string *s = embi_js_to_string(rt, desc);
        if (!s) {
            return EXC;
        }
        desc = embi_js_from_string(s);
    }
    return embi_js_from_symbol(embi_js_symbol_new(rt, desc));
}

static embi_js_value symbol_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    (void)argc;
    (void)argv;
    (void)new_target;
    return embi_js_throw_type_error(rt, "Symbol is not a constructor");
}

static const embi_js_class symbol_ctor_class = {
    .name = "Function",
    .call = symbol_call,
    .construct = symbol_construct,
};

/* Symbol.for(key). */
static embi_js_value symbol_for(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *key_s = embi_js_to_string(rt, arg_at(argc, argv, 0));
    if (!key_s) {
        return EXC;
    }
    if (rt->sym_registry == 0) {
        /* Created on first use, not in embi_js_runtime_new: an object made there would give the
         * heap a shape cell before a host installs its own cell ops. */
        rt->sym_registry =
            embi_js_persistent_new(rt, embi_js_from_object(embi_js_object_new(rt, EMBI_JS_NULL)));
    }
    embi_js_value reg_v = embi_js_persistent_get(rt, rt->sym_registry);
    embi_js_object *reg = embi_js_as_object(reg_v);
    embi_js_key k = embi_js_key_from_str(rt, key_s);
    embi_js_value found = embi_js_obj_get(rt, reg, k, reg_v);
    if (found == EXC) {
        return EXC;
    }
    if (!embi_js_is_undefined(found)) {
        return found; /* only symbols are stored, so undefined means absent */
    }
    embi_js_symbol *sym = embi_js_symbol_new(rt, embi_js_from_string(key_s));
    sym->hdr.flags |= EMBI_JS_FLAG_SYM_REGISTERED;
    embi_js_value sv = embi_js_from_symbol(sym);
    embi_js_define_raw(rt, reg, k, sv, EMBI_JS_ATTR_DEFAULT);
    return sv;
}

/* Symbol.keyFor(sym). */
static embi_js_value symbol_key_for(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value s = arg_at(argc, argv, 0);
    if (!embi_js_is_symbol(s)) {
        return embi_js_throw_type_error(rt, "Symbol.keyFor: argument is not a symbol");
    }
    if (embi_js_symbol_is_registered(s)) {
        return embi_js_as_symbol(s)->description;
    }
    return UNDEF;
}

/* ------------------------------------------------------------------ Symbol.prototype */

static embi_js_value proto_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value s = this_symbol(rt, this_v, "toString");
    if (s == EXC) {
        return EXC;
    }
    return descriptive_string(rt, s);
}

static embi_js_value proto_value_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    return this_symbol(r->rt, this_v, "valueOf");
}

static embi_js_value proto_description(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_value s = this_symbol(r->rt, this_v, "description");
    if (s == EXC) {
        return EXC;
    }
    return embi_js_as_symbol(s)->description;
}

/* Symbol.prototype[@@toPrimitive]: the hint is ignored. */
static embi_js_value proto_to_primitive(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    return this_symbol(r->rt, this_v, "[Symbol.toPrimitive]");
}

/* ------------------------------------------------------------------ installation */

static const struct {
    const char *name;
    enum embi_js_wk wk;
} wk_props[] = {
    {"asyncIterator", EMBI_JS_WK_ASYNC_ITERATOR},
    {"hasInstance", EMBI_JS_WK_HAS_INSTANCE},
    {"isConcatSpreadable", EMBI_JS_WK_IS_CONCAT_SPREADABLE},
    {"iterator", EMBI_JS_WK_ITERATOR},
    {"match", EMBI_JS_WK_MATCH},
    {"matchAll", EMBI_JS_WK_MATCH_ALL},
    {"replace", EMBI_JS_WK_REPLACE},
    {"search", EMBI_JS_WK_SEARCH},
    {"species", EMBI_JS_WK_SPECIES},
    {"split", EMBI_JS_WK_SPLIT},
    {"toPrimitive", EMBI_JS_WK_TO_PRIMITIVE},
    {"toStringTag", EMBI_JS_WK_TO_STRING_TAG},
    {"unscopables", EMBI_JS_WK_UNSCOPABLES},
};

bool embi_js_symbol_is_registered(embi_js_value v)
{
    return embi_js_is_symbol(v) && (embi_js_as_cell(v)->flags & EMBI_JS_FLAG_SYM_REGISTERED) != 0;
}

void embi_js_init_builtins_symbol(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;

    embi_js_object *proto;
    if (embi_js_is_empty(rt->protos[EMBI_JS_PROTO_SYMBOL])) {
        proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
        rt->protos[EMBI_JS_PROTO_SYMBOL] = embi_js_from_object(proto);
    } else {
        proto = embi_js_as_object(rt->protos[EMBI_JS_PROTO_SYMBOL]);
    }

    embi_js_object *ctor =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], &symbol_ctor_class);
    ctor->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(0.0),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_NAME), atom_val(rt, "Symbol"),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Symbol"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, ctor, "for", symbol_for, 1, 0);
    embi_js_define_method(r, ctor, "keyFor", symbol_key_for, 1, 0);

    for (size_t i = 0; i < sizeof wk_props / sizeof wk_props[0]; i++) {
        embi_js_define_raw(rt, ctor, embi_js_key_c(rt, wk_props[i].name),
                           embi_js_wk_sym(rt, wk_props[i].wk), 0);
    }

    embi_js_define_method(r, proto, "toString", proto_to_string, 0, 0);
    embi_js_define_method(r, proto, "valueOf", proto_value_of, 0, 0);
    embi_js_object *getter =
        embi_js_native_function_new(r, "get description", 0, proto_description, 0);
    embi_js_define_accessor_raw(rt, proto, embi_js_key_c(rt, "description"),
                                embi_js_from_object(getter), UNDEF, ATTR_C);
    embi_js_object *to_prim =
        embi_js_native_function_new(r, "[Symbol.toPrimitive]", 1, proto_to_primitive, 0);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_PRIMITIVE),
                       embi_js_from_object(to_prim), ATTR_C);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       atom_val(rt, "Symbol"), ATTR_C);
}
