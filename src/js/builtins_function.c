/*
 * Function: Function.prototype (itself a callable that returns undefined), its call, apply, bind,
 * toString and @@hasInstance, and the Function constructor.
 *
 * The Function constructor (CreateDynamicFunction) builds source text and runs it through the
 * realm's script path, so the new function's scope is the global scope. The parameter list and the
 * body are each checked on their own first, inside a wrapper function so the check creates no
 * global binding. Then the joined source is compiled as one function expression.
 */
#include "js/builtins.h"
#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/interp.h"
#include "js/js.h"

#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

static embi_js_value function_proto_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return EMBI_JS_UNDEFINED;
}

static embi_js_value fn_call(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_callable(this_v)) {
        return embi_js_throw_type_error(rt, "Function.prototype.call called on non-function");
    }
    embi_js_value this_arg = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (argc <= 1) {
        return embi_js_call(rt, this_v, this_arg, 0, nullptr);
    }
    return embi_js_call(rt, this_v, this_arg, argc - 1, argv + 1);
}

static embi_js_value fn_apply(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_callable(this_v)) {
        return embi_js_throw_type_error(rt, "Function.prototype.apply called on non-function");
    }
    embi_js_value this_arg = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_js_value arr = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    if (embi_js_is_nullish(arr)) {
        return embi_js_call(rt, this_v, this_arg, 0, nullptr);
    }
    uint32_t n = 0;
    embi_js_varray *list = embi_js_list_from_array_like(rt, arr, &n);
    if (!list) {
        return EXC;
    }
    /* argv points into the middle of the list cell. The conservative scan counts only cell starts,
     * so the list is kept alive by using it again after the call: the compiler must then keep the
     * list pointer itself on the C stack or in a callee-saved register, which the scan sees. */
    embi_js_value res = embi_js_call(rt, this_v, this_arg, (int)n, list->elems);
    EMBI_KEEP_ALIVE(list);
    return res;
}

/* Function.prototype.bind: BoundFunctionCreate, then the length and name properties. */
static embi_js_value fn_bind(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_callable(this_v)) {
        return embi_js_throw_type_error(rt, "Bind must be called on a function");
    }
    embi_js_object *target = embi_js_as_object(this_v);
    embi_js_value bound_this = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    int nargs = argc > 1 ? argc - 1 : 0;
    embi_js_object *bf = embi_js_bound_function_new(rt, target, bound_this, nargs,
                                                    nargs > 0 ? argv + 1 : nullptr);
    if (!bf) {
        return EXC;
    }

    double len = 0.0;
    embi_js_pdesc pd;
    int st = embi_js_obj_get_own_property(rt, target, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), &pd);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_YES) {
        embi_js_value tl = embi_js_get(rt, target, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH));
        if (tl == EXC) {
            return EXC;
        }
        if (embi_js_is_number(tl)) {
            double d = 0.0;
            if (!embi_js_to_integer_or_infinity(rt, tl, &d)) {
                return EXC;
            }
            d -= (double)nargs;
            len = d > 0.0 ? d : 0.0;
        }
    }
    embi_js_define_raw(rt, bf, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(len),
                       ATTR_C);

    embi_js_value tn = embi_js_get(rt, target, embi_js_name_val(rt, EMBI_JS_NAME_NAME));
    if (tn == EXC) {
        return EXC;
    }
    embi_js_string *base = embi_js_is_string(tn) ? embi_js_as_string(tn)
                                                 : embi_js_atom_intern_utf8(rt, "", 0);
    embi_js_string *prefix = embi_js_str_new_utf8(rt, "bound ", 6);
    if (!prefix) {
        return EXC;
    }
    embi_js_string *name = embi_js_str_concat(rt, prefix, base);
    if (!name) {
        return EXC;
    }
    embi_js_define_raw(rt, bf, embi_js_name_val(rt, EMBI_JS_NAME_NAME), embi_js_from_string(name),
                       ATTR_C);
    return embi_js_from_object(bf);
}

/* Compiled functions return their source text. Native functions and bound functions have the
 * NativeFunction-shaped string "function NAME() { [native code] }". */
static embi_js_value fn_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_callable(this_v)) {
        return embi_js_throw_type_error(
            rt, "Function.prototype.toString requires that 'this' be a Function");
    }
    embi_js_object *f = embi_js_as_object(this_v);
    /* A compiled function keeps the source text of its span (ES2015 19.2.3.5): the exact slice. */
    if (f->klass == &embi_js_class_closure && f->internal) {
        const embi_js_closure *cl = (const embi_js_closure *)f->internal;
        const embi_js_code *code = (const embi_js_code *)embi_js_as_cell(cl->code);
        if (code && code->src && code->src_len > 0) {
            embi_js_string *src = embi_js_str_new_utf8(rt, code->src, code->src_len);
            return src ? embi_js_from_string(src) : EXC;
        }
    }
    /* A callable proxy is not observable here: no trap runs, and a revoked one still has a
     * NativeFunction string (ES2025 20.2.3.5). */
    if (embi_js_obj_is_proxy(f)) {
        embi_js_string *ps = embi_js_str_new_utf8(rt, "function () { [native code] }", 29);
        return ps ? embi_js_from_string(ps) : EXC;
    }
    embi_js_pdesc pd;
    int st = embi_js_obj_get_own_property(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_NAME), &pd);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    embi_sb sb = {nullptr, 0, 0};
    const char *head = "function ";
    for (const char *p = head; *p; p++) {
        embi_sb_append_char(&sb, *p);
    }
    if (st == EMBI_JS_YES && (pd.has & EMBI_JS_PD_VALUE) && embi_js_is_string(pd.value)) {
        /* NativeFunction takes only a PropertyName: a name such as "get $&" or "bound f g" that
         * is not one (accessor prefix and identifier characters, or a [computed] name) is left out. */
        embi_sb nm = {nullptr, 0, 0};
        embi_js_str_append_utf8(embi_js_as_string(pd.value), &nm);
        size_t i = 0;
        if (nm.len > 4 && (memcmp(nm.data, "get ", 4) == 0 || memcmp(nm.data, "set ", 4) == 0)) {
            i = 4;
        }
        bool ok = i < nm.len || nm.len == 0;
        if (nm.len > i && nm.data[i] == '[' && nm.data[nm.len - 1] == ']') {
            i = nm.len;
        }
        for (; i < nm.len && ok; i++) {
            unsigned char c = (unsigned char)nm.data[i];
            ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
                 c >= 0x80 || (i > 0 && c >= '0' && c <= '9');
        }
        if (ok) {
            embi_sb_append(&sb, (embi_str){nm.data, (uint32_t)nm.len});
        }
        embi_sb_free(&nm);
    }
    const char *tail = "() { [native code] }";
    for (const char *p = tail; *p; p++) {
        embi_sb_append_char(&sb, *p);
    }
    embi_js_string *s = embi_js_str_new_utf8(rt, sb.data, sb.len);
    embi_sb_free(&sb);
    return s ? embi_js_from_string(s) : EXC;
}

static embi_js_value fn_has_instance(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    int st = embi_js_ordinary_has_instance(r->rt, this_v, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

/* Appends ASCII text to a string builder. */
static void sb_text_local(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

/* Appends the bytes of another builder. */
static void sb_bytes_local(embi_sb *sb, const embi_sb *piece)
{
    embi_sb_append(sb, (embi_str){piece->data, piece->len});
}

/* Appends the UTF-8 of a ToString'd argument to sb. Returns false when the conversion throws. */
static bool append_arg_text(embi_js_runtime *rt, embi_js_value v, embi_sb *sb)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_js_str_append_wtf8(s, sb);
    return true;
}

/* Compiles the joined source as a script and returns its value (the function), or EXC. */
static embi_js_value eval_source(embi_js_realm *r, const embi_sb *sb)
{
    return embi_js_eval(r, sb->data, sb->len, "anonymous", EMBI_JS_EVAL_DYNFN);
}

/* Compile-only structural check of "pre piece post": it must be exactly one parenthesised
 * function expression (embi_js_compile_function_only), so injected text can neither close the
 * wrapper early nor add statements. Nothing is executed; the compiled code is discarded. */
static bool check_piece(embi_js_realm *r, const char *pre, const embi_sb *piece, const char *post)
{
    embi_sb sb = {nullptr, 0, 0};
    sb_text_local(&sb, pre);
    sb_bytes_local(&sb, piece);
    sb_text_local(&sb, post);
    embi_js_value v = embi_js_compile_function_only(r, (const uint8_t *)sb.data, sb.len);
    embi_sb_free(&sb);
    return v != EXC;
}

/* GetPrototypeFromConstructor for a function made by a constructor: f's [[Prototype]] becomes
 * new_target's prototype (or the intrinsic `which` when new_target is not an object). */
static bool set_proto_from_ctor(embi_js_runtime *rt, embi_js_value f, embi_js_value new_target,
                                enum embi_js_proto which)
{
    if (!embi_js_is_object(new_target)) {
        return true;
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, which);
    if (proto == EXC) {
        return false;
    }
    if (proto == rt->protos[which]) {
        return true;
    }
    return embi_js_obj_set_prototype_of(rt, embi_js_as_object(f), proto) != EMBI_JS_THREW;
}

static embi_js_value function_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic,
                                   embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_sb params = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    embi_sb src = {nullptr, 0, 0};
    embi_js_value result = EXC;

    for (int i = 0; i < argc - 1; i++) {
        if (i > 0) {
            embi_sb_append_char(&params, ',');
        }
        if (!append_arg_text(rt, argv[i], &params)) {
            goto done;
        }
    }
    if (argc > 0 && !append_arg_text(rt, argv[argc - 1], &body)) {
        goto done;
    }

    if (!check_piece(r, "(function anonymous(", &params, "\n) {\n})") ||
        !check_piece(r, "(function anonymous(\n) {\n", &body, "\n})")) {
        goto done;
    }

    /* The source is a function expression with no name, so the new function binds nothing. */
    sb_text_local(&src, "(function anonymous(");
    sb_bytes_local(&src, &params);
    sb_text_local(&src, "\n) {\n");
    sb_bytes_local(&src, &body);
    sb_text_local(&src, "\n})");

    /* The whole source is proven to be a single function expression, so running it only creates
     * the closure. */
    if (!check_piece(r, "", &src, "")) {
        goto done;
    }
    embi_js_value f = eval_source(r, &src);
    if (f == EXC) {
        goto done;
    }
    embi_js_define_raw(rt, embi_js_as_object(f), embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "anonymous", 9)), ATTR_C);
    if (!set_proto_from_ctor(rt, f, new_target, EMBI_JS_PROTO_FUNCTION)) {
        goto done;
    }
    result = f;

done:
    embi_sb_free(&params);
    embi_sb_free(&body);
    embi_sb_free(&src);
    return result;
}

/* %ThrowTypeError%: the poison-pill getter and setter of the restricted caller and arguments
 * properties (ECMA-262 10.2.4.1). It always throws. */
static embi_js_value throw_type_error_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_throw_type_error(r->rt, "'caller', 'callee' and 'arguments' properties may not be "
                                           "accessed on strict mode functions or the arguments "
                                           "objects for calls to them");
}

void embi_js_init_builtins_function(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;

    /* Function.prototype is itself a function object, so it is created as a native function. Its
     * prototype is Object.prototype; FUNCTION is set before any other function object is made. */
    embi_js_object *fp = embi_js_native_function_new(r, "", 0, function_proto_fn, 0);
    (void)embi_js_obj_set_prototype_of(rt, fp, rt->protos[EMBI_JS_PROTO_OBJECT]);
    rt->protos[EMBI_JS_PROTO_FUNCTION] = embi_js_from_object(fp);

    /* AddRestrictedFunctionProperties: caller and arguments throw on read and write. The thrower
     * is the realm's one %ThrowTypeError%, shared by Function.prototype caller/arguments and the
     * callee accessor of unmapped arguments objects. Its length and name are fixed, and it is not
     * extensible. */
    embi_js_object *thrower = embi_js_native_function_new(r, "", 0, throw_type_error_fn, 0);
    r->thrower = thrower; /* one per realm: also the callee accessor of unmapped arguments objects */
    embi_js_define_raw(rt, thrower, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(0),
                       0);
    embi_js_define_raw(rt, thrower, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0)), 0);
    (void)embi_js_obj_prevent_extensions(rt, thrower);
    embi_js_define_accessor_raw(rt, fp, embi_js_key_c(rt, "caller"), embi_js_from_object(thrower),
                                embi_js_from_object(thrower), ATTR_C);
    embi_js_define_accessor_raw(rt, fp, embi_js_key_c(rt, "arguments"),
                                embi_js_from_object(thrower), embi_js_from_object(thrower), ATTR_C);

    embi_js_define_method(r, fp, "apply", fn_apply, 2, 0);
    embi_js_define_method(r, fp, "bind", fn_bind, 1, 0);
    embi_js_define_method(r, fp, "call", fn_call, 1, 0);
    embi_js_define_method(r, fp, "toString", fn_to_string, 0, 0);

    embi_js_object *hi = embi_js_native_function_new(r, "[Symbol.hasInstance]", 1, fn_has_instance,
                                                     0);
    embi_js_define_raw(rt, fp, embi_js_wk_sym(rt, EMBI_JS_WK_HAS_INSTANCE), embi_js_from_object(hi),
                       0);

    embi_js_object *ctor = embi_js_native_constructor_new(r, "Function", 1, function_ctor, 0);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(fp), 0);
    embi_js_define_raw(rt, fp, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Function"), embi_js_from_object(ctor),
                       ATTR_WC);
}
