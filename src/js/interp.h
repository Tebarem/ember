#pragma once
/*
 * Interpreter internals: the VM state kept per runtime, call frames, the value stack, and the
 * closure and native function classes. The public entry points are in js.h.
 *
 * Value stack. Each frame occupies one contiguous run of slots: parameters, then locals, then the
 * operand stack. Frames never span chunks. A new chunk is started when the current one cannot
 * hold the next frame. Every slot of a live frame is initialized at push time, so the collector
 * can mark [base, top) of every chunk up to the current one without looking at stale data.
 */
#include "js/bytecode.h"
#include "js/js.h"
#include "js/object.h"
#include "js/regexp.h"
#include "js/value.h"

#include <stdbool.h>

#define EMBI_JS_FRAME_ENTRY 0x01u     /* returning from this frame leaves the interpreter loop */
#define EMBI_JS_FRAME_CONSTRUCT 0x02u /* [[Construct]] frame: a non-object result yields this */
#define EMBI_JS_FRAME_GEN 0x04u       /* a generator body: its frame->gen is the generator object */
#define EMBI_JS_FRAME_ASYNC 0x08u     /* an async function body (not a generator): frame->gen holds its promise once started */

#define EMBI_JS_STACK_FIRST_VALUES (16u * 1024u)
#define EMBI_JS_STACK_NEXT_VALUES (64u * 1024u)
#define EMBI_JS_DEFAULT_MAX_DEPTH 10000u
#define EMBI_JS_DEFAULT_NATIVE_BYTES (768u * 1024u)

typedef struct embi_js_stack_chunk {
    embi_js_value *base;
    embi_js_value *top;
    embi_js_value *end;
    size_t cap; /* values */
} embi_js_stack_chunk;

typedef struct embi_js_frame {
    embi_js_code *code;
    embi_js_object *callee; /* nullptr for code run without a closure */
    embi_js_value this_v;
    embi_js_value new_target;
    embi_js_value *base;
    uint32_t nslots;
    uint32_t chunk;      /* chunk holding base */
    uint32_t prev_chunk; /* chunk that was current before this frame */
    const uint8_t *pc;   /* resume point while the frame is not the running one */
    embi_js_value *sp;   /* operand stack top while the frame is not the running one */
    const uint8_t *cur;  /* while a native or accessor re-entered from this frame: the op that did */
    uint8_t flags;       /* EMBI_JS_FRAME_* */
    embi_js_object *gen; /* EMBI_JS_FRAME_GEN: the generator whose body this is */
} embi_js_frame;

#ifdef EMBI_JS_OPSTATS
/* Dynamic op statistics (compile definition EMBI_JS_OPSTATS): counts since the last reset, as
 * TSV lines appended to out. See interp.c. */
void embi_js_opstats_reset(void);
void embi_js_opstats_dump(embi_sb *out);
#endif

typedef struct embi_js_vm {
    embi_js_realm *realm;
    EMBI_VEC(embi_js_stack_chunk) chunks;
    uint32_t cur;
    EMBI_VEC(embi_js_frame) frames;
    uint32_t max_depth;
    size_t native_limit;
    embi_js_interrupt_fn interrupt;
    void *interrupt_user;
    const uint8_t *pc_cur; /* the op the running frame is executing (set at every dispatch) */
    bool terminating; /* an interrupt is unwinding: handlers and finally blocks are skipped */
    bool loc_valid;
    embi_js_value loc_value; /* the exception the location belongs to (rooted) */
    char *loc_file;
    uint32_t loc_line;
    uint32_t loc_col;
    bool sav_valid; /* location saved when a finally entry is taken (restored at RETHROW) */
    embi_js_value sav_value;
    char *sav_file;
    uint32_t sav_line;
    uint32_t sav_col;
    /* Interned keys of the iterator protocol (atoms are permanent, so they need no marking). */
    embi_js_key k_next, k_done, k_value, k_return, k_throw;
    bool iter_keys;
    /* One-unit strings below 256, interned on first use (see embi_js_unit_string). */
    embi_js_string *unit_str[256];
} embi_js_vm;

/* A closure: the callee's CODE and its boxes (upvalues). */
typedef struct embi_js_closure {
    embi_js_value code;
    embi_js_value *upvals;
    uint32_t nupvals;
    uint32_t has_ext; /* J5n2: nonzero when an embi_js_closure_ext sits between this record and the upvalues */
} embi_js_closure;

/* J5n2: the fields only methods and class constructors use. Most closures have none, which makes their
 * block 24 bytes plus the upvalues instead of 48. The block is [record][ext when has_ext][upvalues];
 * interp.c is the only reader (cl_home, cl_pinit, cl_finit) and the only writer (cl_ext_ensure). */
typedef struct embi_js_closure_ext {
    embi_js_value home;  /* [[HomeObject]]: the object a method was defined on, or undefined */
    embi_js_value pinit; /* class constructor: the private methods installer, or undefined */
    embi_js_value finit; /* class constructor: the field initializer, or undefined */
} embi_js_closure_ext;

/* A generator object's internal state (J2c). The saved frame is the callee, this, the live slots
 * [0, nsaved) of the frame (parameters, locals, operand stack), and the byte offset to resume at.
 * saved is sized to the live slice [0, nsaved) and grown by gen_reserve when a suspension needs more.
 * Generators are created by GEN_START and driven by embi_js_generator_resume. */
enum embi_js_gen_state {
    EMBI_JS_GEN_SUSPENDED_START,
    EMBI_JS_GEN_SUSPENDED_YIELD,
    EMBI_JS_GEN_EXECUTING,
    EMBI_JS_GEN_COMPLETED,
    EMBI_JS_GEN_AWAIT_RETURN, /* async generator: awaiting the value of a return request */
};

/* Why a frame is suspended (embi_js_gen.susp). A resumption pushes [received, mode] for YIELD and
 * AWAIT, and nothing for a body that has not started. */
#define EMBI_JS_SUSP_NONE 0u
#define EMBI_JS_SUSP_YIELD 1u
#define EMBI_JS_SUSP_AWAIT 2u

typedef struct embi_js_gen {
    embi_js_value callee;
    embi_js_value this_v;
    embi_js_value *saved;
    uint32_t nsaved;
    uint32_t cap;
    uint32_t pc;     /* resume offset into the callee's bytecode */
    uint8_t state;   /* enum embi_js_gen_state */
    bool raw;        /* the last suspension was YIELD_RAW: next() returns the result object itself */
    uint8_t susp;    /* EMBI_JS_SUSP_*: the last suspension (0 while running or not started) */
    /* J3b. An async function's promise (its holder is an object of class embi_js_class_async);
     * an async generator's request queue: qhead and qtail are records [value, mode, promise,
     * next] (EMPTY when the queue is empty). Undefined for other generators. */
    embi_js_value promise;
    embi_js_value qhead;
    embi_js_value qtail;
} embi_js_gen;

typedef struct embi_js_native_data {
    embi_js_native fn;
    embi_js_native_ctor cfn; /* constructors (J2d): fn is unused */
    int magic;
    embi_js_realm *realm;
    bool ctor;
    /* W5a0: opaque host data, set by embi_js_native_function_new_data and read by the function
     * body through embi_js_native_data_current. The engine neither traces nor frees it. */
    void *data;
} embi_js_native_data;

extern const embi_js_class embi_js_class_closure;
extern const embi_js_class embi_js_class_native;
extern const embi_js_class embi_js_class_arguments;
extern const embi_js_class embi_js_class_generator;
extern const embi_js_class embi_js_class_async;
extern const embi_js_class embi_js_class_async_gen;

/* Generator resumption for %GeneratorPrototype% next (mode 0), throw (1) and return (2). Returns
 * the iterator result object, or EMBI_JS_EXCEPTION with the exception pending. o must be a
 * generator (see embi_js_is_generator). */
embi_js_value embi_js_generator_resume(embi_js_runtime *rt, embi_js_object *o, int mode,
                                       embi_js_value v);
bool embi_js_is_generator(embi_js_value v);
/* J3b: AsyncGenerator.prototype next (mode 0), throw (1) and return (2): the request's promise, or
 * EMBI_JS_EXCEPTION when the interrupt unwinds. self is checked for the async generator brand. */
embi_js_value embi_js_async_gen_request(embi_js_runtime *rt, embi_js_value self, int mode,
                                        embi_js_value v);
/* J3b: CreateAsyncFromSyncIterator over the sync iterator record sync_rec (builtins_iterator.c).
 * Returns the wrapper object. */
embi_js_value embi_js_async_from_sync_create(embi_js_runtime *rt, embi_js_value sync_rec);
/* CreateIterResultObject. */
embi_js_value embi_js_iter_result_new(embi_js_runtime *rt, embi_js_value v, bool done);
/* Iterator records of the iterator protocol, for the builtins (interp.c). ir_open is
 * GetIterator(v, sync) and returns the record (a varray) or EMBI_JS_EXCEPTION. ir_next is
 * IteratorStepValue: 1 with the value, 0 when done, -1 when it threw. ir_close is
 * IteratorClose on a normal completion (false when it threw); ir_close_throw ignores its errors. */
embi_js_value embi_js_ir_open(embi_js_runtime *rt, embi_js_value v);
int embi_js_ir_next(embi_js_runtime *rt, embi_js_value rec, embi_js_value *out);
bool embi_js_ir_close(embi_js_runtime *rt, embi_js_value rec);
void embi_js_ir_close_throw(embi_js_runtime *rt, embi_js_value rec);

/* The interned keys of the iterator protocol (permanent atoms, so callers may keep them). */
typedef enum embi_js_ikey {
    EMBI_JS_IK_NEXT,
    EMBI_JS_IK_DONE,
    EMBI_JS_IK_VALUE,
    EMBI_JS_IK_RETURN,
} embi_js_ikey;
embi_js_key embi_js_iter_key(embi_js_runtime *rt, embi_js_ikey k);

/* The one-unit string for u. Units below 256 are atoms kept in the VM, so the string iterator
 * does not allocate for them. */
embi_js_string *embi_js_unit_string(embi_js_runtime *rt, uint16_t u);

/* Creates the VM for rt when it does not exist yet (sets the CODE cell ops and the roots). */
embi_js_vm *embi_js_vm_ensure(embi_js_runtime *rt);
void embi_js_vm_free(embi_js_runtime *rt);

/* Creates a closure of code (no upvalues). Returns the object or nullptr when it throws. */
embi_js_object *embi_js_closure_from_code(embi_js_runtime *rt, embi_js_value code);

/* Records the source position of an exception created by the compiler (no frames). */
void embi_js_note_location(embi_js_runtime *rt, const char *file, uint32_t line, uint32_t col);

/* True while the native stack used since the outermost embedder entry is within the budget. The
 * parser checks it on every nested construct. */
bool embi_js_native_stack_ok(embi_js_runtime *rt);

/* ------------------------------------------------------------------ RegExp literals and objects (J4c) */

/* The template of a RegExp literal (builtins_regexp.c): a null-prototype RegExp object that holds
 * the compiled program (prog, which the call takes over), the flags bits and the pattern units.
 * The parser checks the pattern with embi_re_compile before calling this, so it cannot fail except
 * for allocation. Returns the template or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_regexp_template_new(embi_js_runtime *rt, embi_re_prog *prog, uint32_t flags,
                                          const uint16_t *src, uint32_t len);
/* NEW_REGEXP: a fresh RegExp object (own lastIndex) sharing the template's compiled program. */
embi_js_value embi_js_regexp_from_template(embi_js_runtime *rt, embi_js_value tmpl);
/* True for RegExp objects (the [[RegExpMatcher]] internal slot). */
bool embi_js_is_regexp(embi_js_value v);
