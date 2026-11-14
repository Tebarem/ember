#pragma once
/*
 * JS engine umbrella header and embedder entry points.
 *
 * Conventions: every value is embi_js_value (NaN-boxed, see value.h). A function that can
 * allocate may run a collection, so every live value a C function holds must be in a local or
 * argument (conservatively scanned), in a persistent handle, or reachable from a root. Native code
 * that keeps values in its own heap memory (embi_alloc) must register them as roots or handles.
 *
 * Errors: a function that throws returns EMBI_JS_EXCEPTION with the exception pending on the
 * runtime (embi_js_exception_get). See ops.h for the per-function conventions.
 */
#include "js/builtins.h"
#include "js/heap.h"
#include "js/jobs.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/promise.h"
#include "js/runtime.h"
#include "js/string.h"
#include "js/value.h"

#include <stddef.h>
#include <stdint.h>

/* A realm: the global object and the intrinsics its builtins installed. The intrinsic prototypes
 * live in the runtime (rt->protos), so one runtime supports one realm for now. */
struct embi_js_realm {
    embi_js_runtime *rt;
    embi_js_object *global;
    embi_js_object *eval_fn; /* the realm eval function: a direct call runs its source in the caller scope */
    embi_js_object *thrower; /* %ThrowTypeError%, for the strict arguments.callee accessor */
    embi_js_object *obj_to_string; /* %Object.prototype.toString%, the fallback of Array.prototype.toString */
    embi_js_object *object_ctor; /* %Object%: its constructor tells NewTarget apart from the active function */
    embi_js_object *lex;     /* global declarative record: name -> box (EMPTY while uninitialized),
                              * writable attribute for let and none for const; null prototype */
    embi_js_object *vars;    /* [[VarNames]]: names declared by scripts' var and function
                              * declarations and by sloppy eval at global level; null prototype */
    uint32_t nlex;           /* entries in lex (0 skips the lookup) */
    void *cur_data;          /* W5a0: native data of the native function being called (see
                              * embi_js_native_data_current); saved and restored around each call */
};

/* Native function: this_v and argv come from the caller, magic is the value given at creation.
 * Returns the result or EMBI_JS_EXCEPTION. */
typedef embi_js_value (*embi_js_native)(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic);

/* Native constructor (J2d): like embi_js_native, plus new_target, which is undefined for a call.
 * The constructor creates its object from new_target's prototype (GetPrototypeFromConstructor),
 * so a subclass instance gets the subclass prototype. this_v is undefined for a construct. */
typedef embi_js_value (*embi_js_native_ctor)(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic,
                                             embi_js_value new_target);

/* Interrupt callback: return true to stop the running script. The stop cannot be caught by
 * script code: every frame unwinds without running handlers or finally blocks. */
typedef bool (*embi_js_interrupt_fn)(embi_js_runtime *rt, void *user);

/* embi_js_eval flags. */
#define EMBI_JS_EVAL_STRICT 0x1u /* run the code in strict mode even without a directive */
#define EMBI_JS_EVAL_DYNFN 0x2u  /* Function constructor source: "(function anonymous(" binds no name */

/* ------------------------------------------------------------------ realms and evaluation */

/* Creates a realm with all intrinsics and the global object. The runtime must outlive it. */
[[nodiscard]] embi_js_realm *embi_js_realm_new(embi_js_runtime *rt);
/* Same, with a global object of class global_class (nullptr: an ordinary object) whose internal
 * slot is internal. The global's prototype is Object.prototype until embi_js_realm_set_global_proto.
 * The class's trace and finalize hooks run for the global like for any other object. */
[[nodiscard]] embi_js_realm *embi_js_realm_new_ex(embi_js_runtime *rt,
                                                  const embi_js_class *global_class,
                                                  void *internal);
/* Replaces the prototype of the realm's global object (for a Window: Window.prototype). */
void embi_js_realm_set_global_proto(embi_js_realm *r, embi_js_object *proto);
void embi_js_realm_free(embi_js_realm *r);

/* Compiles and runs src as a script and returns its completion value, or EMBI_JS_EXCEPTION with
 * the exception pending. filename names the source in error locations (copied). src need not be
 * NUL-terminated. */
embi_js_value embi_js_eval(embi_js_realm *r, const char *src, size_t len, const char *filename,
                           unsigned flags);

/* Native functions. A constructor is callable with new: it gets a fresh object as this_v (whose
 * prototype comes from new.target), and its result is used when it is an object. */
[[nodiscard]] embi_js_object *embi_js_native_function_new(embi_js_realm *r, const char *name,
                                                          int length, embi_js_native fn,
                                                          int magic);
[[nodiscard]] embi_js_object *embi_js_native_constructor_new(embi_js_realm *r, const char *name,
                                                             int length, embi_js_native_ctor fn,
                                                             int magic);
/* Defines a writable, configurable, non-enumerable native method on o. */
void embi_js_define_method(embi_js_realm *r, embi_js_object *o, const char *name,
                           embi_js_native fn, int length, int magic);
/* A native function (not a constructor) that carries data. The function body reads data with
 * embi_js_native_data_current while it runs. data is not traced or freed by the engine: it must
 * outlive the function (a static descriptor, or memory the host frees when the runtime ends). */
[[nodiscard]] embi_js_object *embi_js_native_function_new_data(embi_js_realm *r, const char *name,
                                                               int length, embi_js_native fn,
                                                               int magic, void *data);
/* The data of the native function whose body is running (nullptr when it has none, or outside
 * a native call). Nested calls restore the outer value when they return. */
void *embi_js_native_data_current(embi_js_realm *r);
/* Defines an accessor property on o named name: a getter named "get <name>" and, when setter is
 * not nullptr, a setter named "set <name>". Both use magic. attrs takes EMBI_JS_ATTR_ENUMERABLE
 * and EMBI_JS_ATTR_CONFIGURABLE (the accessor bit is implied). A missing setter is undefined.
 * name should be shorter than 250 bytes, which is the limit for the function names. */
void embi_js_define_getset(embi_js_realm *r, embi_js_object *o, const char *name,
                           embi_js_native getter, embi_js_native setter, int magic, uint8_t attrs);
/* The same, with both functions carrying data (see embi_js_native_function_new_data). */
void embi_js_define_getset_data(embi_js_realm *r, embi_js_object *o, const char *name,
                                embi_js_native getter, embi_js_native setter, int magic, uint8_t attrs,
                                void *data);

/* ------------------------------------------------------------------ limits and diagnostics */

/* The interrupt callback runs at every call and loop back-edge while set. */
void embi_js_set_interrupt(embi_js_runtime *rt, embi_js_interrupt_fn fn, void *user);
/* Call depth limit (default 10000) and native stack budget in bytes (default 768 KB). */
void embi_js_set_limits(embi_js_runtime *rt, uint32_t max_depth, size_t native_bytes);

/* Source position of the most recent exception that the engine recorded (the frame that threw
 * it, or the parser for syntax errors). file stays valid until the next recorded exception. */
bool embi_js_error_location(embi_js_runtime *rt, const char **file, uint32_t *line,
                            uint32_t *col);

/* ES modules (J3c, module.c). The host loads module sources; the engine never reads files.
 *
 * embi_js_module_load_fn is called once per request: request is the id to complete with
 * embi_js_module_loaded or embi_js_module_load_failed (synchronously from the hook or later). referrer
 * is the key of the requesting module (nullptr for an entry module or import() from a script).
 * specifier is the UTF-8 module specifier, type is "json" for an import attribute or nullptr.
 *
 * Lifetime: referrer, specifier and type are valid only during the call. A host that completes the
 * request later must copy them first (the engine keeps only the request id). Each request id must
 * be completed exactly once, with embi_js_module_loaded (source text, or JSON text for type "json")
 * or embi_js_module_load_failed. The key given to embi_js_module_loaded is the module's identity:
 * the same (key, type) pair always names the same module record. */
typedef void (*embi_js_module_load_fn)(embi_js_runtime *rt, void *user, uint32_t request,
                                       const char *referrer, size_t referrer_len,
                                       const char *specifier, size_t specifier_len,
                                       const char *type);

/* The import.meta hook: called once per module, with its meta object and its key. This single
 * hook stands for both HostGetImportMetaProperties and HostFinalizeImportMeta: the engine creates
 * the null-prototype object and the host fills it in.
 * Import attributes: only "type" is supported (value "json"); the load of a request with any other
 * attribute key fails (static imports: a SyntaxError at load, import(): a TypeError). import()
 * called from direct eval code inside a module has that module as its referrer. */
typedef void (*embi_js_import_meta_fn)(embi_js_runtime *rt, embi_js_value meta, const char *key,
                                       size_t key_len, void *user);

void embi_js_set_module_loader(embi_js_runtime *rt, embi_js_module_load_fn load, void *user);
void embi_js_set_import_meta_hook(embi_js_runtime *rt, embi_js_import_meta_fn fn, void *user);
void embi_js_module_loaded(embi_js_runtime *rt, uint32_t request, const char *key, size_t key_len,
                           const char *source, size_t source_len);
void embi_js_module_load_failed(embi_js_runtime *rt, uint32_t request, const char *message);

/* Loads and evaluates the module named specifier (UTF-8) in realm r. Returns a promise that
 * resolves when the module graph has been evaluated (or rejects with its error). */
embi_js_value embi_js_module_eval(embi_js_realm *r, const char *specifier, size_t len);

/* The namespace object of a loaded module (key is its host key), or a ReferenceError. */
embi_js_value embi_js_module_namespace(embi_js_runtime *rt, const char *key, size_t key_len);
