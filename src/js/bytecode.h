#pragma once
/*
 * Bytecode layout: opcodes (opcodes.def), the CODE cell that holds a compiled function, and the
 * side tables it carries (exception table, position table, upvalue descriptors).
 *
 * A CODE cell is immutable once built. Its bytecode, constant, table and file-name buffers live
 * outside the heap and are released by the cell's finalizer.
 */
#include "base/base.h"
#include "js/heap.h"
#include "js/value.h"

#include <stdint.h>

enum embi_js_op : uint8_t {
#define OP(name, size, pops, pushes) EMBI_JSOP_##name,
#include "js/opcodes.def"
#undef OP
    EMBI_JSOP_COUNT
};

typedef struct embi_js_op_info {
    const char *name;
    uint8_t size;
    int8_t pops;
    int8_t pushes;
} embi_js_op_info;

extern const embi_js_op_info embi_js_op_table[EMBI_JSOP_COUNT];

/* J5i superinstructions (opcodes.def, compiler.c): the two ops a fused op replaces, in the order
 * they execute. Index i is the bit that EMBI_JS_FUSE_MASK tests for the pair. */
typedef struct embi_js_fuse_def {
    uint8_t first;
    uint8_t second;
    uint8_t fused;
} embi_js_fuse_def;
#define EMBI_JS_NFUSE 2
extern const embi_js_fuse_def embi_js_fuse_defs[EMBI_JS_NFUSE];

static inline uint32_t embi_js_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void embi_js_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define EMBI_JS_NO_SLOT 0xFFFFFFFFu

#define EMBI_JS_CODE_STRICT 0x01u
#define EMBI_JS_CODE_SCRIPT 0x02u      /* top-level code of a script */
#define EMBI_JS_CODE_CONSTRUCTOR 0x04u /* ordinary function: may be used with new */
#define EMBI_JS_CODE_MAPPED_ARGS 0x08u /* sloppy arguments object aliases the parameters */
#define EMBI_JS_CODE_EVAL 0x10u        /* eval code: upvalue 0 holds the caller's context */
#define EMBI_JS_CODE_REST 0x20u        /* the parameter at rest_slot takes the trailing arguments */
#define EMBI_JS_CODE_NO_SELF 0x40u     /* arrow function or method: no prototype, not a constructor */
#define EMBI_JS_CODE_GENERATOR 0x80u   /* generator function: its body starts with GEN_START */
#define EMBI_JS_CODE_CLASS_CTOR 0x100u /* class constructor: [[Call]] throws, [[Construct]] only */
#define EMBI_JS_CODE_DERIVED 0x200u    /* derived class constructor: this starts uninitialized */
#define EMBI_JS_CODE_HOMED 0x800u      /* J5n2: method, field initializer, static block or class constructor: its closures get [[HomeObject]] storage up front */
#define EMBI_JS_CODE_ASYNC 0x400u      /* async function or generator: its body starts with ASYNC_START or ASYNC_GEN_START */

/* Runtime scope entries of dynamic references and eval contexts. WITH: the value is a with
 * object. REC: the value is an environment record whose own properties hold boxes. VE: a record
 * of a sloppy function's eval-declared vars (like REC). CHAIN: the value is an eval context.
 * DECL: an eval site's group of visible bindings, turned into a REC record when the eval runs. */
#define EMBI_JS_DYN_WITH 1u
#define EMBI_JS_DYN_REC 2u
#define EMBI_JS_DYN_CHAIN 3u
#define EMBI_JS_DYN_DECL 4u
#define EMBI_JS_DYN_VE 5u
/* FN_DECL / FN_REC: like DECL / REC, but for the outermost scope of a function (its params,
 * locals, functions, callee and arguments). Eval-declared names that already exist there bind to
 * those boxes; names in catch or block scopes do not count. */
#define EMBI_JS_DYN_DECL_FN 6u
#define EMBI_JS_DYN_FN_REC 7u
/* DECL_LEX (site entry) / LEXREC (runtime): the names of the let, const and block functions
 * between a sloppy direct eval and its function's top scope; an eval var of such a name is a
 * SyntaxError. */
#define EMBI_JS_DYN_DECL_LEX 8u
#define EMBI_JS_DYN_LEXREC 9u

/* Where a scope entry's value is read from: a local slot (LOC), a box held in a local slot
 * (BOX), or a box held in an upvalue (UPV). */
#define EMBI_JS_SRC_LOC 0u
#define EMBI_JS_SRC_BOX 1u
#define EMBI_JS_SRC_UPV 2u
#define EMBI_JS_SRC_IMPORT 3u /* an eval context name that is a module import (no storage) */

typedef struct embi_js_dyn_ent {
    uint8_t type; /* EMBI_JS_DYN_WITH, REC, VE or CHAIN */
    uint8_t src;  /* EMBI_JS_SRC_* */
    uint32_t index;
} embi_js_dyn_ent;

/* A dynamic reference: entries [first, first + n) are checked in order, then the fallback. */
typedef struct embi_js_dyn_site {
    uint32_t name;     /* constant index of the name */
    uint32_t fallback; /* pc of the static code for the reference */
    uint32_t first;
    uint32_t n;
    bool unres;        /* strict ref form of a name that no scope declares: records an unresolvable reference */
} embi_js_dyn_site;

/* A name captured by an eval site's DECL group. The source holds a box (BOX or UPV). */
typedef struct embi_js_eval_name {
    uint32_t name;
    uint8_t src;
    uint32_t index;
    uint8_t ro; /* immutable binding: stores are ignored (sloppy) or throw (strict) */
    uint32_t tok; /* EMBI_JS_SRC_IMPORT: constant index of the import's token */
} embi_js_eval_name;

typedef struct embi_js_eval_ent {
    uint8_t type; /* EMBI_JS_DYN_* (WITH, VE, CHAIN or DECL) */
    uint8_t src;
    uint32_t index;
    uint32_t first_name; /* DECL: names [first_name, first_name + nnames) */
    uint32_t nnames;
} embi_js_eval_ent;

/* A direct eval site: the call's argc and callee text, and the scope entries of the caller. */
typedef struct embi_js_eval_site {
    uint32_t argc;
    uint32_t name_k; /* 1 + constant index of the callee text, or 0 */
    uint32_t first;  /* entries [first, first + n) */
    uint32_t n;
    uint8_t strict;
    uint8_t nt_ok;   /* new.target is allowed in the eval code (the caller is inside a function) */
    uint8_t in_params; /* the call is in a parameter default */
    uint32_t this_up; /* arrow caller: upvalues holding the enclosing this and new.target, or */
    uint32_t nt_up;   /* EMBI_JS_NO_SLOT when the frame's own values are used */
    uint8_t super_ok; /* the eval code may use super.x: the caller is a method (J2d) */
    uint8_t priv_ok;  /* the caller is inside a class body: the eval code may name its private names */
    uint32_t home_up; /* arrow caller: upvalue holding the enclosing [[HomeObject]], else NO_SLOT */
    uint8_t noargs;   /* the eval code may not reference arguments (field initializer, static block) */
    uint32_t mod_k;   /* 1 + constant index of the caller's module holder (import() referrer), or 0 */
} embi_js_eval_site;

/* Where a closure's upvalue comes from when the closure is created. */
#define EMBI_JS_UPV_PARENT 0u /* parent's upvalue number `index` */
#define EMBI_JS_UPV_LOCAL 1u  /* parent's local slot `index`, which holds a box */
#define EMBI_JS_UPV_NONE 2u   /* unused (the name is a global) */

/* A protected range [start, end) whose exceptions go to handler, with the operand stack
 * reset to depth values above the frame's locals. Inner ranges come first. */
typedef struct embi_js_exc_entry {
    uint32_t start;
    uint32_t end;
    uint32_t handler;
    uint32_t depth;
    uint32_t finally_entry; /* nonzero: a finally entry, which keeps the recorded error location */
} embi_js_exc_entry;

/* From pc onward (until the next entry) the source position is line:col. */
typedef struct embi_js_pos_entry {
    uint32_t pc;
    uint32_t line;
    uint32_t col;
} embi_js_pos_entry;

typedef struct embi_js_upval_desc {
    uint32_t index;
    uint32_t from; /* EMBI_JS_UPV_* */
} embi_js_upval_desc;

/* The source text of a unit, shared by every function CODE cell compiled from it (each cell keeps
 * a pointer into data and a reference). Freed with the last cell. */
typedef struct embi_js_srcbuf {
    uint32_t refs;
    uint32_t len;
    char data[];
} embi_js_srcbuf;

typedef struct embi_js_code {
    embi_js_cell hdr;
    /* J5n2: the fields are grouped by size (pointers, then 32-bit counts, then 16-bit flags), which
     * removes about 100 bytes of padding from the old declaration order; the cell was 320 bytes. */
    embi_js_value name;     /* string, or EMPTY for anonymous code */
    char *file;             /* copy of the source name (NUL-terminated), inside blk */
    uint8_t *bc;
    embi_js_value *consts;
    embi_js_exc_entry *exc;
    uint8_t *pos;            /* J5n2: source positions, delta-coded (embi_js_code_position), pos_bytes bytes (0: none) */
    embi_js_upval_desc *upv;
    const char *src;         /* source text of the function (Function.prototype.toString), or nullptr; points into src_buf */
    embi_js_srcbuf *src_buf; /* shared source of the unit (one reference per cell) */
    embi_js_dyn_ent *dents;  /* dynamic reference entries, all sites */
    embi_js_dyn_site *dsites;
    embi_js_eval_ent *eents; /* eval site entries */
    embi_js_eval_name *enames;
    embi_js_eval_site *esites;
    uint8_t *pmap;           /* MAPPED_ARGS: pmap[i] != 0 when parameter i is aliased */
    /* J5c: one inline cache per GET_FIELD/SET_FIELD/GET_FIELD_THIS/DEFINE_FIELD/SUPER_GET site (and the
     * global sites); the op operand is its index (ic.h). J5n2: the array is allocated by the first
     * execution of the code (embi_js_code_ics_ensure), so code that never runs costs only ic_keys:
     * the canonical key of each site, resolved at compile time. */
    struct embi_js_ic *ics;
    embi_js_value *ic_keys;
    /* J5n1: the shapes a new closure of this code reaches by appending its own properties (length, name,
     * prototype) from fn_from. nullptr until the first closure is made. Held strongly (embi_js_code_trace). */
    struct embi_js_shape *fn_from, *fn_to;
    void *blk;               /* FP1a: the one allocation that holds file, bc, consts, exc, pos, upv, ic_keys and the tables below */
    uint32_t blk_len;
    uint32_t bc_len;
    uint32_t nconsts;
    uint32_t nexc;
    uint32_t pos_bytes;
    uint32_t nupv;
    uint32_t nparams;
    uint32_t nlocals;
    uint32_t max_stack;
    uint32_t arguments_slot; /* EMBI_JS_NO_SLOT when the function does not use arguments */
    uint32_t fn_length;      /* the length property */
    uint32_t rest_slot;      /* EMBI_JS_CODE_REST: the parameter slot of the rest parameter */
    uint32_t src_len;
    uint32_t ndents;
    uint32_t ndsites;
    uint32_t neents;
    uint32_t nenames;
    uint32_t nesites;
    uint32_t npmap;
    uint32_t nics;
    uint16_t flags;          /* EMBI_JS_CODE_* */
} embi_js_code;

/* J5n2: allocates code->ics on the code's first execution (the interpreter calls it when it loads a
 * frame whose code has sites and no array yet). Every site starts empty with its key from ic_keys. */
void embi_js_code_ics_ensure(embi_js_runtime *rt, embi_js_code *c);

/* Position of pc in code: the last entry at or before pc. Returns false when the table is
 * empty. */
bool embi_js_code_position(const embi_js_code *c, uint32_t pc, uint32_t *line, uint32_t *col);

/* The CODE cell type ops (trace and finalize). Registered by the VM. */
void embi_js_code_trace(embi_js_cell *cell, embi_js_tracer *t);
void embi_js_code_finalize(embi_js_cell *cell);

/* Appends a listing of the bytecode (one op per line: offset, name, operand) to out. */
void embi_js_disasm(const embi_js_code *c, embi_sb *out);
