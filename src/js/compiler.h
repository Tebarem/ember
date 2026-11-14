#pragma once
/*
 * Compiler internals shared by parser.c (statements and expressions), compiler.c (emission and
 * the CODE builder) and scope.c (the resolve pass).
 *
 * Shape of a compile. The parser emits bytecode straight into a cfunc while it reads the source.
 * Variable references are emitted as SYM_* ops that name a scope. When the whole unit is parsed,
 * the resolve pass (scope.c) runs for every function, children before parents. It decides the
 * storage of every binding, rewrites the SYM_* ops, and writes each function's init block.
 * The CODE cells are built only after the whole unit is resolved.
 *
 * Dynamic scopes. A with statement adds a scope whose only binding holds the with object. A
 * sloppy function that contains a direct eval has a binding holding its variable environment
 * (eval-declared vars). A reference that crosses such a scope gets a dynamic site: runtime
 * checks of those entries, then the static code (the fallback). A direct eval gets an eval site
 * that lists the scopes visible at the call; the eval code is compiled later and reads that
 * context through its upvalue 0.
 *
 * Compile-time state lives in an arena and in vectors owned by the unit, and the unit roots its
 * constants (see compiler.c).
 */
#include "base/base.h"
#include "js/bytecode.h"
#include "js/js.h"
#include "js/value.h"
#include "jslex/jslex.h"

#include <stdbool.h>

typedef struct cfunc cfunc;
typedef struct cscope cscope;
typedef struct cbind cbind;
typedef struct ctl ctl;
typedef struct cunit cunit;
typedef struct cmod cmod;

/* Module metadata of a module unit (J3c, module.c owns the record that holds it). Atoms are
 * permanent, so the values are plain. A request is one distinct (specifier, type) pair. type is
 * EMPTY or the atom "json". An import is one imported name: name is the import name, EMPTY for a
 * namespace import; local is the local binding's name. An export is a local one (kind CMEX_LOCAL,
 * local is the binding's name, and slot is its box after the build), an indirect one (CMEX_INDIRECT:
 * req and local, the import name, EMPTY for a namespace re-export), or a star one (CMEX_STAR). */
enum cmexp_kind { CMEX_LOCAL, CMEX_INDIRECT, CMEX_STAR };
typedef struct cmreq {
    embi_js_value spec;
    embi_js_value type;
    bool unsupported; /* the attribute list has a key other than "type": the host rejects the request */
} cmreq;
typedef struct cmimp {
    embi_js_value name;
    embi_js_value local;
    uint32_t req;
} cmimp;
typedef struct cmexp {
    uint8_t kind;      /* enum cmexp_kind */
    embi_js_value name; /* exported name (EMPTY for a star export) */
    embi_js_value local; /* CMEX_LOCAL: local binding name; CMEX_INDIRECT: import name or EMPTY */
    uint32_t req;      /* CMEX_INDIRECT and CMEX_STAR */
    int32_t slot;      /* CMEX_LOCAL: slot of the binding in the module's frame (set by the build) */
} cmexp;
struct cmod {
    EMBI_VEC(cmreq) reqs;
    EMBI_VEC(cmimp) imps;
    EMBI_VEC(cmexp) exps;
    bool has_tla;      /* the module uses await at its top level */
};

/* BK_LET and BK_CONST are lexical (TDZ until initialized). BK_IMPLICIT holds this or new.target
 * for arrow functions (its name is "%this" or "%nt"). */
enum cbind_kind { BK_VAR, BK_PARAM, BK_FUNC, BK_CATCH, BK_CALLEE, BK_ARGUMENTS, BK_TEMP, BK_LET,
                  BK_CONST, BK_IMPLICIT, BK_IMPORT /* J3c: an import of a module (no storage) */ };

struct cbind {
    embi_js_value name;   /* atom, or EMPTY for temporaries */
    cscope *scope;        /* scope that declares it (nullptr for temporaries) */
    cbind *next_in_scope; /* next binding of the same scope */
    cfunc *owner;         /* the function whose storage holds the binding */
    uint8_t kind;         /* enum cbind_kind */
    bool captured;        /* referenced by an inner function (or an eval): lives in a box */
    bool global;          /* script-level var or function: a property of the global object */
    bool glex;            /* script-level let or const: lives in the realm's lexical record (global) */
    bool catch_pat;       /* a name of a destructuring catch parameter: a var of the same name in the block is an error */
    bool ptdz;            /* parameter of a function with default values: initialized in order */
    bool plain_fn;        /* BK_FUNC of a plain function declaration (not a generator or async): a block may repeat it */
    bool prebox;          /* block let/const: the block entry stub made the box (stub_pc) */
    uint32_t stub_pc;     /* the SYM_INIT of that stub */
    int32_t slot;         /* local slot, assigned by the resolve pass (-1 before, or global) */
    uint32_t init_pc;     /* lexical: pc of the initializing SYM_INIT (0 before it is emitted) */
    embi_js_value import_token; /* BK_IMPORT: the token [module holder, import index] (a constant) */
};

/* Scope kinds. A with scope holds exactly one binding: the with object. */
#define SC_NORMAL 0u
#define SC_WITH 1u
#define SC_SWITCH 2u /* the case blocks of a switch: lexical accesses are always checked */
#define SC_CATCH 3u  /* the scope holding a catch parameter (its block is the scope inside) */

/* A name declared with var inside a block scope (it hoists through the scope): a lexical
 * declaration of the same name in that scope is an early error. */
typedef struct cvname {
    embi_js_value name;
    struct cvname *next;
} cvname;

struct cscope {
    cscope *parent;
    cvname *vars;    /* var names that were declared in or below this block scope */
    cbind *first;
    cbind *last;
    uint32_t count;  /* bindings in the list */
    uint32_t cap;    /* hash table capacity (a power of two), 0 while the scope is small */
    cbind **table;   /* name -> first binding of that name, built past a small size */
    uint64_t bloom;  /* FP2a: one bit per binding name (bloom_bit): a clear bit means the name is not in this scope */
    uint8_t kind;    /* SC_NORMAL or SC_WITH */
};

enum csym_kind { SYM_KIND_GET, SYM_KIND_SET, SYM_KIND_INIT, SYM_KIND_TYPEOF, SYM_KIND_DELETE,
                 SYM_KIND_CALL, SYM_KIND_REF, SYM_KIND_GETREF, SYM_KIND_SETREF };

typedef struct csym {
    embi_js_value name;
    cscope *scope;   /* where the reference appears */
    cbind *direct;   /* set for compiler temporaries: no scope walk */
    cbind *res;      /* resolved binding (step 2 of the resolve pass) */
    int32_t upidx;   /* resolved to upvalue number upidx, or -1 */
    int32_t dyn;     /* dynamic site index, or -1 when the reference is static */
    uint32_t pc;     /* offset of the SYM op */
    uint8_t kind;    /* enum csym_kind */
} csym;

/* One scope entry of a dynamic site or an eval site. The binding is in this function (bind) or
 * reached through an upvalue (upidx). src and index are set at the storage step. */
typedef struct cdynent {
    uint8_t type;        /* EMBI_JS_DYN_* */
    cbind *bind;
    int32_t upidx;       /* >= 0: reached through this upvalue of the function */
    uint8_t src;         /* EMBI_JS_SRC_* */
    uint32_t index;      /* slot or upvalue index */
    uint32_t first_name; /* DECL entries: names [first_name, first_name + nnames) in enames */
    uint32_t nnames;
} cdynent;

/* A name of an eval site's DECL group: its box is copied into the eval context. */
typedef struct cdynname {
    embi_js_value name;
    uint32_t name_k;    /* constant index of the name, set when the CODE is built */
    cbind *bind;
    int32_t upidx;
    uint8_t src;
    uint32_t index;
    uint8_t ro; /* the immutable self-name of a function expression */
    uint32_t tok; /* EMBI_JS_SRC_IMPORT: constant index of the import's token (set at resolve) */
} cdynname;

typedef struct cdynsite {
    uint32_t name;      /* constant index of the name */
    uint32_t first;     /* entries [first, first + n) in dents */
    uint32_t n;
    uint32_t fallback;  /* pc of the static code */
    bool unres;         /* see embi_js_dyn_site */
} cdynsite;

typedef bool (*embi_jsc_priv_known_fn)(void *user, embi_js_value name);

/* A direct eval call. */
typedef struct ceval {
    cscope *scope;      /* the scope of the call */
    uint32_t argc;
    uint32_t name_k;
    bool strict;
    bool nt_ok;         /* new.target is allowed in the eval code */
    bool in_params;     /* the call is in a parameter default: its var names may not repeat a parameter */
    bool args_bad;      /* ... and the eval code may not declare arguments */
    bool super_ok;      /* the caller is a method, field initializer or static block (super.x) */
    bool priv_ok;       /* the caller is inside a class body (or in eval code that may name private names) */
    bool noargs;        /* the eval code may not reference arguments (field initializer, static block) */
    int32_t home_up;    /* arrow caller with super_ok: upvalue of the enclosing %home, else -1 */
    int32_t this_up;    /* arrow caller: upvalues of the enclosing this and new.target, else -1 */
    int32_t nt_up;
    uint32_t mod_k;     /* 1 + constant index of the enclosing module holder (the import() referrer), or 0 */
    uint32_t first;     /* entries [first, first + n) in eents */
    uint32_t n;
} ceval;

enum cupval_kind { UPV_UNRESOLVED, UPV_LOCAL, UPV_PARENT, UPV_GLOBAL };

typedef struct cupval {
    embi_js_value name;
    cscope *def;     /* scope in the parent where the function appears */
    uint8_t kind;    /* enum cupval_kind */
    cbind *bind;     /* UPV_LOCAL: the parent's binding */
    uint32_t index;  /* UPV_PARENT: the parent's upvalue number */
} cupval;

/* A rewritable upvalue reference: the op at pc refers to upvalue upidx. */
typedef struct cupsite {
    uint32_t pc;
    uint32_t upidx;
    embi_js_value name;
    uint8_t kind;    /* enum csym_kind */
} cupsite;

/* A function declaration that the init block instantiates. A sloppy eval's function has no
 * binding (bind is nullptr): it is stored in the variable environment under name. */
typedef EMBI_VEC(embi_js_value) cvalvec;

typedef struct cdecl {
    cfunc *child;
    cbind *bind;
    embi_js_value name;
} cdecl;

#define EMBI_JSC_CALLEE_CACHE 16u

struct cfunc {
    embi_arena *ar;     /* the unit's arena: the vectors below live in it (CF_PUSH), freed with it */
    cfunc *parent;
    cscope *def;        /* scope in the parent where this function appears */
    cscope *root;      /* the function's own top scope: params, vars, functions */
    cscope *cur;       /* the scope that is current while the body is parsed */
    embi_js_value cc_val[EMBI_JSC_CALLEE_CACHE]; /* callee-text constants already added */
    uint32_t cc_idx[EMBI_JSC_CALLEE_CACHE];
    bool cc_used[EMBI_JSC_CALLEE_CACHE];
    embi_js_value name;      /* the name property (may be EMPTY) */
    embi_js_value self_name; /* named function expression: name bound inside it, else EMPTY */
    uint32_t const_idx;      /* the parent's constant that receives this function's CODE */
    uint32_t nparams;
    bool is_script;
    bool is_eval;            /* eval code: upvalue 0 is the caller's context */
    bool is_arrow;           /* arrow function: this, new.target and arguments are the enclosing ones */
    bool is_method;          /* object literal method or accessor: no prototype, not a constructor */
    bool is_gen;             /* generator function (function*): its body starts with GEN_START */
    bool is_async;           /* async function or arrow (async function*: is_gen too): its body starts with ASYNC_START (ASYNC_GEN_START) */
    bool is_setter;          /* property setter: exactly one plain parameter */
    bool is_module;          /* module code (J3c): the top of a module unit; not a script */
    bool uses_await;         /* module code: an await at its top level (sets is_async at the end) */
    bool is_class_ctor;      /* class constructor (its CODE is a constructor that cannot be called) */
    bool is_derived;         /* derived class constructor: this is set by super() */
    bool is_field_init;      /* class field initializer function (arguments is an early error) */
    bool is_static_block;    /* class static block (return and arguments are early errors) */
    bool is_init;            /* class initializer function (fields, private methods, static elements) */
    bool in_params;          /* the parameter list is being parsed */
    bool simple;             /* simple parameter list (no defaults, rest or patterns) */
    bool has_rest;
    uint32_t rest_slot;
    uint32_t length;         /* the length property: parameters before the first default or rest */
    uint32_t src_start;      /* source span of the function text (Function.prototype.toString) */
    uint32_t src_end;
    uint32_t new_pc;         /* pc of the NEW_CLOSURE that creates it in the parent, 0 when hoisted */
    cscope *vroot;           /* parameter expressions: the scope of the body's vars (child of root) */
    bool ptdz;               /* parameter bindings are initialized in order (default values) */
    uint32_t pcopy_at;       /* operand pc of the body-start jump to the var copy stub (vroot) */
    EMBI_VEC(cbind *) pcopies; /* body vars that start with the value of the same-named parameter */
    bool dynref;             /* a with object or an eval context may be on the scope chain */
    bool has_eval;           /* contains a direct eval call (not counting nested functions) */
    bool mapped;             /* sloppy arguments object aliases the parameters */
    bool strict;
    bool done;
    cbind *callee;      /* the self-name binding, created on demand */
    cbind *arguments;   /* the arguments binding, created on demand */
    cbind *completion;  /* script and eval: the completion value of the last statement */
    struct cpatch *epi; /* derived constructor: JMP sites to the result-check epilogue (value on the stack) */
    cbind *ve_bind;     /* sloppy function with a direct eval: its variable environment */
    cbind *chain_bind;  /* eval code: the binding holding the caller's context */
    EMBI_VEC(uint8_t) code;
    EMBI_VEC(embi_js_value) consts; /* rooted while the unit lives */
    EMBI_VEC(uint32_t) ics;         /* J5c: constant index of the key of each IC site (the site's operand is its position here) */
    EMBI_VEC(cfunc *) children;
    EMBI_VEC(cupval) upvals;
    EMBI_VEC(csym) syms;
    EMBI_VEC(cupsite) upsites;
    EMBI_VEC(cbind *) binds;   /* every binding, in creation order */
    EMBI_VEC(cdecl) decls;
    EMBI_VEC(embi_js_exc_entry) exc;
    EMBI_VEC(embi_js_pos_entry) pos;
    EMBI_VEC(embi_js_value) evalvars; /* sloppy eval: var names (in the variable env) */
    EMBI_VEC(embi_js_value) pnames;   /* parameter names by index */
    EMBI_VEC(cdynent) dents;
    EMBI_VEC(cdynsite) dsites;
    EMBI_VEC(cdynent) eents;
    EMBI_VEC(cdynname) enames;
    EMBI_VEC(ceval) evals;
    int32_t depth;      /* operand stack depth tracked during emission */
    int32_t max_depth;
    uint32_t nlocals;
    uint32_t max_stack;
    uint32_t arguments_slot;
    embi_js_value code_value; /* the built CODE cell, or EMPTY */
};

typedef struct cpatch {
    uint32_t at;
    struct cpatch *next;
} cpatch;

typedef struct cjumprec {
    uint32_t id;
    ctl *target;      /* nullptr with is_return: leave the function */
    bool cont;
    bool is_return;
    struct cjumprec *next;
} cjumprec;

enum ctl_kind { CTL_LOOP, CTL_SWITCH, CTL_LABEL, CTL_TRY_CATCH, CTL_TRY_FIN, CTL_PATTERN };

/* A breakable or try construct on the control stack (a C-stack linked list). */
struct ctl {
    ctl *parent;
    uint8_t kind;      /* enum ctl_kind */
    uint8_t items;     /* operand stack values the construct keeps (for-in 1, switch 1) */
    uint8_t nlabels;
    embi_js_value labels[8];
    cpatch *brk;       /* JMP sites that go to the construct's exit */
    cpatch *cont;      /* JMP sites that go to its continue point */
    cpatch *fin_sites; /* TRY_FIN: JMP sites that go to the finally block */
    cjumprec *jumps;   /* TRY_FIN: exits routed through the finally block */
    uint32_t next_id;  /* TRY_FIN: next dispatch id */
    cbind *fk;         /* TRY_FIN: completion kind */
    cbind *fv;         /* TRY_FIN: completion value */
    bool is_iter;      /* for-of: the construct keeps an iterator record (items == 1); leaving it closes the iterator */
    bool is_async_iter; /* for-await (J3b): the iterator is async and its close awaits */
    cbind *pat_rec;    /* array pattern (J3e): its iterator record temporary; a return completion closes it */
};

/* A function declaration of a sloppy block: its closure is created by the block's hoisting stub. */
typedef struct cblkfn {
    cfunc *child;
    cbind *bind;   /* the block-scoped binding that receives the closure */
    embi_js_value name;
} cblkfn;

/* A block (a Block statement, a catch or finally body, a switch body, or the body of an if
 * statement that is a function declaration). Its function declarations are bound in the block's
 * scope and instantiated when the block is entered: a jump at the block's start reaches a hoisting
 * stub at its end, which creates the closures and jumps back to the body. In sloppy code
 * (Annex B.3.3) each declaration also copies its closure to the function's var of the same name
 * when the declaration is evaluated, unless the name is a parameter. Strict code keeps the
 * declarations block-scoped only (ES2015 semantics). */
typedef struct cblk {
    cunit *p;
    cfunc *fn;
    cscope *scope;       /* the block's scope (the current scope while the block is parsed) */
    cscope *saved_cur;   /* the function's current scope before the block */
    struct cblk *outer;  /* the enclosing block of the same function */
    uint32_t hoist_at;   /* operand of the entry jump, patched by block_leave */
    bool annexb;         /* sloppy code: Annex B copies to the function's var */
    EMBI_VEC(cblkfn) fns;
} cblk;

/* One open bracket level of a parser lookahead (see group_follow in parser.c). */
typedef struct cgframe {
    uint32_t start; /* offset of the '[' or '{' (UINT32_MAX for any other level) */
    bool rest;      /* an ellipsis at that level */
} cgframe;

/* FP1a: the answer of block_may_have_fn for the block whose '{' ends at key: 0 unknown, 1 no function
 * declaration or lexical binding can be in it, 2 one may be. */
typedef struct cbmemo {
    uint32_t key;
    uint8_t state;
} cbmemo;

struct cunit {
    embi_js_runtime *rt;
    embi_js_realm *realm;
    embi_jslex lx;
    embi_jstok tok;
    /* FP1a: the token the last peek() read (no cooked value), its start offset and the lexer state
     * after it, so that the adv() that follows takes it instead of lexing the same text again. */
    embi_jstok pk_tok;
    embi_jslex pk_lx;
    uint32_t pk_from;
    bool pk_valid;
    uint32_t na_start;         /* name_atom cache: offset of the NAME token whose atom is na_atom (UINT32_MAX: none) */
    uint32_t na_end;           /* ... and its end offset */
    embi_js_value na_atom;     /* valid only because atoms are permanent (js_atom.c) */
    EMBI_VEC(cbmemo) bm;       /* block_may_have_fn answers, in source order, of the last scan */
    uint32_t bm_i;             /* cursor into bm (the parser asks in source order) */
    embi_hashmap arr;          /* offset of a '(' -> 1 when its matching ')' is followed by '=>' (arrow_ahead memo) */
    uint16_t *scratch;
    uint16_t *peek_scratch;
    embi_hashmap grp;          /* bracket group start offset -> follow kind | rest << 8 (group memo) */
    EMBI_VEC(cgframe) gstk;    /* open levels of the running lookahead */
    uint32_t scratch_cap;
    uint32_t peek_cap;
    const uint8_t *src;
    uint32_t src_len;
    uint32_t eval_last;    /* offset of the last "eval" text of the source (eval_in_rhs), or UINT32_MAX */
    bool eval_last_set;
    char *file;            /* owned copy, may be nullptr */
    embi_js_srcbuf *srcbuf; /* shared copy of the source for the CODE cells (made on demand) */
    embi_arena arena;
    cfunc *fn;             /* function being compiled */
    cfunc *top;            /* the script or the eval code */
    EMBI_VEC(cfunc *) all; /* every function of the unit, parents before children */
    ctl *ctl;              /* control stack of the current function */
    embi_js_value hint;    /* name for the next anonymous function expression, or EMPTY */
    embi_js_value pending_labels[8];
    uint8_t npending;
    uint32_t tmp_counter;
    uint32_t last_end;     /* byte offset after the last consumed token (callee text) */
    int32_t nest;          /* recursion depth of the parser (nesting limit) */
    bool eval_mode;        /* the unit is eval code */
    embi_js_value eval_module; /* eval code: the holder of the module of the caller, or EMPTY */
    bool nt_ok;            /* eval code: new.target is allowed (the caller is inside a function) */
    bool eval_params;      /* eval code called from a parameter default */
    bool eval_super;       /* eval code: super.x is allowed (the caller is a method) */
    bool eval_noargs;      /* eval code: arguments is an early error (field initializer, static block) */
    bool eval_priv;        /* eval code: private names of the caller's classes may be named */
    embi_jsc_priv_known_fn priv_known; /* eval code: whether the caller declares a private name */
    void *priv_user;
    bool dynfn;            /* Function constructor source */
    bool dynfn_seen;
    uint8_t next_kind;     /* kind of the function parsed next: 0 plain, 1 method, 2 getter, 3 setter */
    bool next_gen;         /* the function parsed next is a generator (function*) */
    bool next_async;       /* the function parsed next is async (J3b) */
    bool fn_only;          /* the unit must be exactly "(function ...)" and nothing else */
    bool keep_ref_once;    /* set by delete for its operand: the outer chain keeps its reference */
    uint32_t block_depth;  /* blocks entered in the current function */
    cblk *blk;             /* innermost block of the current function (nullptr at its top) */
    embi_js_value atom_eval;
    embi_js_value atom_arguments;
    embi_js_value atom_strict[9]; /* implements .. yield, the strict-mode reserved words */
    embi_js_value atom_this;      /* "%this": the implicit this binding of arrow functions */
    embi_js_value atom_let;       /* "let": not a lexical binding name */
    embi_js_value atom_proto;     /* "__proto__": the object literal prototype setter form */
    embi_js_value atom_nt;        /* "%nt": the implicit new.target binding of arrow functions */
    embi_js_value atom_home;      /* "%home": the implicit [[HomeObject]] binding (super references) */
    embi_js_value atom_fn;        /* "%fn": the implicit active function binding (super calls) */
    struct cclass *cls;           /* innermost class body being parsed (parser.c), or nullptr */
    cmod *mod;                    /* module unit: the requests, imports and exports (J3c), or nullptr */
    cvalvec *collect; /* export declarations: the names their declarations bind */
    bool late_err;                /* a module import name was redeclared by a var (reported after the statement) */
    embi_js_value holder;         /* module unit: the holder object of the module (constants of its code) */
};

/* FP1a: the vectors of a cfunc (code, consts, syms, ...) are grown inside the unit's arena, which a
 * compile frees in one go. A malloc and a free per vector dominated the compile on Windows. Use
 * CF_PUSH for every push to one of them. The grown block is extended in place when it is the last
 * thing the arena handed out. */
void *embi_jsc_vgrow(embi_arena *ar, void *data, uint32_t *cap, size_t elem);
#define CF_PUSH(f_, field_, x_)                                                                \
    do {                                                                                       \
        cfunc *cf_ = (f_);                                                                     \
        __typeof__(cf_->field_.data[0]) cv_ = (x_);                                            \
        if (cf_->field_.len == cf_->field_.cap) {                                              \
            cf_->field_.data = embi_jsc_vgrow(cf_->ar, cf_->field_.data, &cf_->field_.cap,     \
                                              sizeof cf_->field_.data[0]);                     \
        }                                                                                      \
        cf_->field_.data[cf_->field_.len++] = cv_;                                             \
    } while (0)

/* Constants and emission (compiler.c). */
uint32_t embi_jsc_here(const cunit *p);
uint32_t embi_jsc_add_const(cunit *p, embi_js_value v);
uint32_t embi_jsc_emit_op(cunit *p, uint8_t op);
uint32_t embi_jsc_emit_op32(cunit *p, uint8_t op, uint32_t v);
/* J5d: a GET_GLOBAL, GET_GLOBAL_OPT or SET_GLOBAL site for the name constant name_k. Returns the
 * site index, which is the op operand (ic.h); build_one resolves the site's key. */
uint32_t embi_jsc_global_ic(cfunc *f, uint32_t name_k);
uint32_t embi_jsc_emit_op_eff(cunit *p, uint8_t op, uint32_t v, int pops, int pushes);
/* An op of 9 bytes (two operands), stack effect from the op table. */
uint32_t embi_jsc_emit_op2(cunit *p, uint8_t op, uint32_t a, uint32_t b);
/* Appends bytes to the function's code (the caller writes them). Returns their offset. */
uint32_t embi_jsc_grow(cunit *p, uint32_t n);
void embi_jsc_patch(cunit *p, uint32_t operand_at, uint32_t target);
void embi_jsc_patch_list(cunit *p, cpatch *list, uint32_t target);
cpatch *embi_jsc_patch_add(cunit *p, cpatch *list, uint32_t at);
void embi_jsc_mark_pos(cunit *p, uint32_t line, uint32_t col);
void *embi_jsc_alloc(cunit *p, size_t n);
cfunc *embi_jsc_new_func(cunit *p, cfunc *parent, cscope *def);
cscope *embi_jsc_new_scope(cunit *p, cscope *parent);
cbind *embi_jsc_new_bind(cunit *p, cfunc *f, cscope *s, embi_js_value name, uint8_t kind);
cbind *embi_jsc_find_in_scope(cscope *s, embi_js_value name);
/* The implicit arguments binding of a non-top function, created on demand. */
cbind *embi_jsc_ensure_arguments(cunit *p, cfunc *f);
void embi_jsc_emit_sym(cunit *p, uint8_t kind, embi_js_value name, cscope *scope, cbind *direct);
void embi_jsc_unit_free(cunit *p);
/* Builds the CODE cells of every function (children first) and returns the top CODE value. */
embi_js_value embi_jsc_build(cunit *p);

/* Resolve pass (scope.c). Writes the init block and rewrites the symbolic ops. Runs once every
 * function of the unit is parsed. */
bool embi_jsc_finish_function(cunit *p, cfunc *f);

/* Adds TDZ checks to the upvalue references of f that reach a lexical binding (build time). */
void embi_jsc_tdz_upsites(cfunc *f);

/* Rewrites the store at pc (a SYM_SET or SET_UPVAL-like site) to a store to a function's own
 * name: a no-op in sloppy code, a TypeError in strict code (ES5 13: the binding is immutable). */
void embi_jsc_write_callee_store(cfunc *f, uint32_t pc);

/* Call operand: low 16 bits hold argc, high 16 bits hold 1 + the constant index of the callee's
 * source text for error messages (0 when none). */
#define EMBI_JS_CALL_ARGC(v) ((v) & 0xFFFFu)
#define EMBI_JS_CALL_NAME(v) ((v) >> 16)

/* Whether f is the top of a unit (a script, eval code or module code): no enclosing function. */
static inline bool embi_jsc_is_top(const cfunc *f)
{
    return f->is_script || f->is_eval || f->is_module;
}

/* Parser (parser.c). */
bool embi_jsc_parse_program(cunit *p, bool strict);
/* Module units (J3c, parser.c). embi_jsc_parse_module parses a module body into p (p->mod and
 * p->holder set). embi_jsc_compile_module compiles a module's source and returns its CODE, with the
 * requests, imports and exports in mod (the export slots are set). */
bool embi_jsc_parse_module(cunit *p);
embi_js_value embi_jsc_compile_module(embi_js_realm *r, const uint8_t *src, uint32_t len,
                                      const char *file, embi_js_value holder, cmod *mod);

/* Marks the units constants during a compile (compiler.c), for the collector. */
void embi_jsc_unit_root(embi_js_tracer *t, void *user);

/* Compiles a script and returns its CODE cell, or EMBI_JS_EXCEPTION with the error pending. */
embi_js_value embi_js_compile(embi_js_realm *r, const uint8_t *src, uint32_t len, const char *file,
                              bool strict);
/* As embi_js_compile; dynfn marks Function constructor source, whose outermost function expression
 * "function anonymous(" has the name anonymous but binds nothing. */
embi_js_value embi_js_compile_ex(embi_js_realm *r, const uint8_t *src, uint32_t len, const char *file,
                                 bool strict, bool dynfn);
/* Compiles a unit that must be exactly one parenthesised anonymous function expression, "(function
 * (...) {...})", with nothing before, after or around it. Compile only: nothing runs. Used by the
 * Function constructor to prove that injected parameter and body text cannot escape the wrapper. */
embi_js_value embi_js_compile_function_only(embi_js_realm *r, const uint8_t *src, uint32_t len);
/* Compiles eval code (strict: the caller is strict). Free names resolve through upvalue 0 at run
 * time. Returns the CODE cell, or EMBI_JS_EXCEPTION with a SyntaxError pending. */
#define EMBI_JSC_EVAL_NT 1u     /* new.target is allowed in the eval code */
#define EMBI_JSC_EVAL_PARAMS 2u /* the caller is a parameter default */
#define EMBI_JSC_EVAL_SUPER 4u  /* the caller is a method: super.x is allowed */
#define EMBI_JSC_EVAL_NOARGS 16u /* the caller is a field initializer or static block: no arguments */
#define EMBI_JSC_EVAL_PRIV 8u   /* the caller is inside a class body: private names resolve */
/* priv_known(user, name) tells whether the caller's scopes declare the private name `name` (an
 * atom such as "#x"); it is consulted only with EMBI_JSC_EVAL_PRIV, and a name it rejects is the
 * early error of AllPrivateNamesValid. */
embi_js_value embi_jsc_compile_eval(embi_js_realm *r, const uint8_t *src, uint32_t len,
                                    bool strict, unsigned eflags, embi_jsc_priv_known_fn priv_known,
                                    void *priv_user, embi_js_value module);
