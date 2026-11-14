#pragma once
/*
 * Builtins registration seam. embi_js_realm_new calls these in the order declared here, once
 * per realm. Each builtins_<name>.c installs its intrinsics on the realm. Object must come first
 * (it creates Object.prototype); the global object is created right after it.
 */

typedef struct embi_js_realm embi_js_realm;

void embi_js_init_builtins_object(embi_js_realm *r);
/* Object constructor, Object statics and Object.prototype methods, plus the global Object binding.
 * Needs Function.prototype, so builtins_global.c calls it after the function and error inits. */
void embi_js_builtins_object_install(embi_js_realm *r);
void embi_js_init_builtins_function(embi_js_realm *r);
void embi_js_init_builtins_error(embi_js_realm *r);
void embi_js_init_builtins_global(embi_js_realm *r);
void embi_js_init_builtins_array(embi_js_realm *r);
void embi_js_init_builtins_string(embi_js_realm *r);
void embi_js_init_builtins_number(embi_js_realm *r);
void embi_js_init_builtins_boolean(embi_js_realm *r);
void embi_js_init_builtins_math(embi_js_realm *r);
void embi_js_init_builtins_json(embi_js_realm *r);
/* J2. Stubs until their tasks land. */
void embi_js_init_builtins_symbol(embi_js_realm *r);
void embi_js_init_builtins_iterator(embi_js_realm *r);
void embi_js_init_builtins_collections(embi_js_realm *r);
void embi_js_init_builtins_proxy(embi_js_realm *r);
/* J3a: Promise. Needs Function, Object and Symbol intrinsics. */
void embi_js_init_builtins_promise(embi_js_realm *r);
/* J4. Stubs until their tasks land; called in this order, after Promise. */
/* J4c (section 2): RegExp, %RegExpStringIteratorPrototype%. Needs String and Symbol intrinsics. */
void embi_js_init_builtins_regexp(embi_js_realm *r);
/* J4b (section 3): BigInt. */
void embi_js_init_builtins_bigint(embi_js_realm *r);
/* J4d (section 4): Date. */
void embi_js_init_builtins_date(embi_js_realm *r);
/* J4e (section 5): ArrayBuffer and SharedArrayBuffer. */
void embi_js_init_builtins_arraybuffer(embi_js_realm *r);
/* J4e (section 5): %TypedArray% and the concrete TypedArray constructors. After arraybuffer. */
void embi_js_init_builtins_typedarray(embi_js_realm *r);
/* J4e (section 5): DataView. After arraybuffer. */
void embi_js_init_builtins_dataview(embi_js_realm *r);
/* J4e (section 5): Atomics. After typedarray. */
void embi_js_init_builtins_atomics(embi_js_realm *r);
/* J4f (section 6): WeakRef and FinalizationRegistry. */
void embi_js_init_builtins_weakref(embi_js_realm *r);
/* J4g (section 7): minimal Intl. Last, so it can wrap the toLocale* methods of the others. */
void embi_js_init_builtins_intl(embi_js_realm *r);
