#pragma once
/*
 * ES modules (J3c): module records, the module graph loader, linking,
 * evaluation (with top-level await), namespace objects, import.meta and dynamic import. Internal:
 * the public API is in js.h. The interpreter calls the embi_jsm_* entry points for IMPORT_GET,
 * IMPORT_META and DYN_IMPORT.
 *
 * A module record is a Source Text Module Record. Its holder object (class embi_js_class_module)
 * is the value the module's code refers to through constants. The engine keeps every record in
 * the runtime's registry for the runtime's lifetime, so records are never released before the
 * runtime is freed.
 *
 * Known limits (for W7): the registry is per runtime, keyed by (key, type) only, and a record
 * keeps a raw realm pointer: a runtime hosting several realms must treat a module key as
 * belonging to ONE realm (use realm-distinct keys, e.g. prefix with the frame id). Graph loading
 * and evaluation recurse on the native stack (one frame set per dependency level), so a host
 * loader that answers synchronously should not be fed chains of several thousand modules.
 */
#include "js/compiler.h"
#include "js/js.h"
#include "js/object.h"

typedef struct embi_js_module embi_js_module;

enum embi_jsm_status {
    EMBJ_UNLINKED,
    EMBJ_LINKED,
    EMBJ_EVALUATING,
    EMBJ_EVALUATING_ASYNC,
    EMBJ_EVALUATED,
};

struct embi_js_module {
    embi_js_value holder;     /* the holder object (its internal is this record) */
    embi_js_realm *realm;
    embi_js_value code;       /* the module's CODE (rooted) */
    embi_js_value gen;        /* the suspended instantiation frame (after linking) */
    embi_js_value key;        /* the host's key string (EMPTY for a module without one) */
    embi_js_value type;       /* EMPTY, or the atom "json" */
    embi_js_value meta;       /* import.meta, or EMPTY until created */
    embi_js_value ns;         /* the namespace object, or EMPTY until created */
    embi_js_value top_cap;    /* the evaluation promise (EMPTY until evaluated) */
    embi_js_value eval_error; /* the evaluation error, EMPTY when none */
    embi_js_value cycle_root; /* the cycle root holder, after evaluation */
    embi_js_value load_error; /* the failure of a request of this module (EMPTY when none) */
    embi_js_value *imports;   /* per import: the box that the binding reads (rooted) */
    embi_js_value *reqs;      /* per request: the requested module's holder, EMPTY until loaded */
    embi_js_value *locals;    /* per export: the box of a local export (rooted) */
    EMBI_VEC(embi_js_value) async_parents; /* holders waiting for this module (rooted) */
    cmod mod;                 /* requests, imports and exports */
    char *key_c;              /* the key as UTF-8 (owned), for the host hooks */
    uint32_t key_len;
    uint8_t status;           /* enum embi_jsm_status */
    bool has_tla;
    bool synthetic;           /* a JSON module: no code, its only export is the box locals[0] */
    bool async_eval;          /* [[AsyncEvaluation]] */
    uint32_t hnext;           /* the registry's hash chain: 1 + index of the next module in mods, or 0 */
    uint32_t epoch;           /* visit mark of the graph walks (mstate.epoch) */
    uint32_t dfs_index;
    uint32_t dfs_ancestor;
    uint32_t pending_async;   /* [[PendingAsyncDependencies]] */
    uint64_t async_order;     /* [[AsyncEvaluationOrder]] */
};

/* The interpreter's entry points. token is the constant of an IMPORT_GET (module holder, index).
 * meta_holder is the holder of the module whose import.meta is read. referrer is the holder of
 * the module that calls import(), or undefined in a script. Each returns the value or
 * EMBI_JS_EXCEPTION. */
embi_js_value embi_jsm_import_get(embi_js_runtime *rt, embi_js_value token);
/* The box of an import named by its token (EMPTY while the module is not linked). */
embi_js_value embi_jsm_import_box(embi_js_value token);
embi_js_value embi_jsm_import_meta(embi_js_runtime *rt, embi_js_value holder);
embi_js_value embi_jsm_dynamic_import(embi_js_runtime *rt, embi_js_value referrer,
                                      embi_js_value spec, embi_js_value options);

/* Releases the module state of rt (called by the runtime's free). */
void embi_jsm_runtime_free(embi_js_runtime *rt);

/* The module record of a holder. */
embi_js_module *embi_jsm_of(embi_js_value holder);

/* Interpreter support (interp.c): the closure of a module CODE, the instantiation that runs it up to
 * GEN_START (returns the suspended frame, a generator object), the slot of a suspended frame, and
 * the resumption of the body (returns the body's completion, or the awaited promise). */
embi_js_object *embi_jsi_module_closure(embi_js_runtime *rt, embi_js_value code);
embi_js_value embi_jsi_module_instantiate(embi_js_runtime *rt, embi_js_object *closure);
embi_js_value embi_jsi_module_slot(embi_js_value gen, uint32_t slot);
embi_js_value embi_jsi_module_resume(embi_js_runtime *rt, embi_js_value gen);
