/*
 * Resolve pass. Runs once every function of the unit is parsed, children before parents:
 *
 *   0. Implicit bindings. A function with a direct eval gets its callee name bound.
 *   1. Child requests. Each child upvalue that is not declared in the child is looked up here,
 *      from the scope where the child is defined. A local hit marks the binding captured (it
 *      lives in a box). A miss becomes this function's own upvalue, which the parent resolves
 *      when this function finishes. At the top of a unit a miss is a global.
 *   2. Own references. Every SYM op is resolved to a binding, an upvalue, or a global. A name
 *      that crosses a with scope, or a sloppy function's variable environment, also gets a
 *      dynamic site: its runtime entries are checked first and the static code is the fallback.
 *      Each direct eval gets an eval site that lists the scopes visible at the call. Bindings
 *      listed there are captured, so the eval context can hold their boxes.
 *   3. Storage. Parameters keep their argument slot. Other local bindings get slots after them.
 *      A sloppy function with a mapped arguments object boxes its parameters.
 *   4. Rewrite. Each SYM op becomes a final op (GET_LOC, GET_BOX, GET_UPVAL, GET_GLOBAL, ...), or
 *      a DYN op whose fallback is appended after the body. Upvalue sites are recorded so the
 *      CODE builder can turn them into global ops when the chain of upvalues ends at a global.
 *   5. Init block. Code appended after the body: the eval context and variable environment,
 *      callee and captured bindings get their boxes, script and eval declarations are made, and
 *      function declarations are instantiated. Then a jump runs the body. Pc 0 holds a jump to
 *      this block (the parser emitted it).
 */
#include "js/compiler.h"
#include "js/interp.h"
#include "js/ops.h"

#include <string.h>

/* Pushes a dynamic entry of the given type for the binding b (in the function f's list). */
static void push_dent(cfunc *f, uint8_t type, cbind *b)
{
    cdynent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.bind = b;
    e.upidx = -1;
    CF_PUSH(f, dents, e);
}

static cbind *ensure_callee(cunit *p, cfunc *f)
{
    if (!f->callee) {
        f->callee = embi_jsc_new_bind(p, f, f->root, f->self_name, BK_CALLEE);
    }
    return f->callee;
}

cbind *embi_jsc_ensure_arguments(cunit *p, cfunc *f)
{
    if (!f->arguments) {
        f->arguments = embi_jsc_new_bind(p, f, f->root, p->atom_arguments, BK_ARGUMENTS);
    }
    return f->arguments;
}

/* The implicit binding holding this or new.target of a non-arrow function, created on demand
 * and initialized at entry (emit_init_block). */
static cbind *ensure_implicit(cunit *p, cfunc *g, embi_js_value name)
{
    cbind *b = embi_jsc_find_in_scope(g->root, name);
    if (!b) {
        b = embi_jsc_new_bind(p, g, g->root, name, BK_IMPLICIT);
    }
    return b;
}

/* Bindings that start uninitialized: let, const, and the parameters of a function with default
 * values (initialized in order). */
static bool is_lexical(const cbind *b)
{
    return b->kind == BK_LET || b->kind == BK_CONST || b->ptdz;
}

/* The binding an upvalue of f refers to, when its parent chain ends at a local binding. */
static const cbind *upval_bind_of(const cfunc *f, uint32_t idx)
{
    for (;;) {
        const cupval *e = &f->upvals.data[idx];
        if (e->kind == UPV_LOCAL) {
            return e->bind;
        }
        if (e->kind != UPV_PARENT || !f->parent) {
            return nullptr;
        }
        idx = e->index;
        f = f->parent;
    }
}

/* Whether a reference to a lexical binding must check that the binding is initialized. A
 * reference from another function always checks (the closure may run early). Within the owner,
 * a reference textually after the initializer runs after it, except in a switch, whose case
 * bodies can be entered past a declaration. */
static bool needs_tdz(const cfunc *f, const csym *s, const cbind *b)
{
    if (!is_lexical(b)) {
        return false;
    }
    if (b->owner != f) {
        return true;
    }
    if (b->scope && b->scope->kind == SC_SWITCH) {
        return true;
    }
    return s->pc <= b->init_pc;
}

/* The op and operand that access a local binding (kind is a csym_kind). */
static void local_op(const cfunc *f, const cbind *b, uint8_t kind, uint32_t at, uint8_t *op,
                     uint32_t *operand)
{
    *operand = (uint32_t)b->slot;
    switch (kind) {
    case SYM_KIND_GET:
    case SYM_KIND_TYPEOF:
    case SYM_KIND_CALL:
        *op = b->captured ? EMBI_JSOP_GET_BOX : EMBI_JSOP_GET_LOC;
        break;
    case SYM_KIND_SET:
        if (b->kind == BK_CONST) {
            /* Assignment to a constant: a TypeError, after the TDZ check of the caller. */
            *op = EMBI_JSOP_CONST_ASSIGN;
            *operand = 0;
        } else {
            *op = b->captured ? EMBI_JSOP_SET_BOX : EMBI_JSOP_SET_LOC;
        }
        break;
    case SYM_KIND_INIT:
        if (!b->captured) {
            *op = EMBI_JSOP_PUT_LOC;
        } else if (b->scope == f->root || b->scope == f->vroot ||
                   (b->prebox && at != b->stub_pc)) {
            /* The init block (or the block's entry stub) made this box already, so the
             * declaration stores into it: closures created before it share the box. */
            *op = EMBI_JSOP_PUT_BOX;
        } else {
            *op = EMBI_JSOP_INIT_BOX;
        }
        break;
    default:
        /* delete of a declared name: not deletable */
        *op = EMBI_JSOP_CONST_FALSE;
        *operand = 0;
        break;
    }
}

/* Gives every exception entry that covers src_pc a copy covering [from, to), so that a tail or a
 * fallback placed past the body still reaches the handlers of its source. */
static void mirror_exc(cfunc *f, uint32_t src_pc, uint32_t from, uint32_t to)
{
    uint32_t n = (uint32_t)f->exc.len;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_exc_entry e = f->exc.data[i];
        if (src_pc >= e.start && src_pc < e.end) {
            e.start = from;
            e.end = to;
            CF_PUSH(f, exc, e);
        }
    }
}

/* Gives the code at `to` the source position of src_pc. Positions stay sorted by pc, because
 * `to` lies past every entry. */
static void mirror_pos(cfunc *f, uint32_t src_pc, uint32_t to)
{
    bool have = false;
    embi_js_pos_entry last = {0, 0, 0};
    for (uint32_t i = 0; i < f->pos.len; i++) {
        if (f->pos.data[i].pc > src_pc) {
            break;
        }
        last = f->pos.data[i];
        have = true;
    }
    if (have) {
        last.pc = to;
        CF_PUSH(f, pos, last);
    }
}

/* Replaces the op at pc with a jump to a tail appended to the code. The tail checks that the
 * lexical binding is initialized (the check op with its slot and the name), runs the body op,
 * and jumps back to ret. The exception entries and the position of src_pc are copied to it. */
/* Appends a constant to f (the function being resolved, which need not be the function being
 * parsed) and returns its index in f. */
static uint32_t const_in(cfunc *f, embi_js_value v)
{
    for (size_t i = 0; i < f->consts.len; i++) {
        if (f->consts.data[i] == v) {
            return (uint32_t)i;
        }
    }
    uint32_t k = (uint32_t)f->consts.len;
    CF_PUSH(f, consts, v);
    return k;
}

static void write_op(cfunc *f, uint32_t pc, uint8_t op, uint32_t operand)
{
    f->code.data[pc] = op;
    embi_js_wr32(&f->code.data[pc + 1], operand);
}

static void write_tail(cfunc *f, uint32_t pc, uint32_t src_pc, uint32_t ret,
                       uint8_t check, uint32_t slot, embi_js_value name, uint8_t body,
                       uint32_t body_operand)
{
    uint32_t name_k = const_in(f, name);
    uint32_t tail = (uint32_t)f->code.len;
    for (uint32_t i = 0; i < 19u; i++) {
        CF_PUSH(f, code, 0);
    }
    f->code.data[tail] = check;
    embi_js_wr32(&f->code.data[tail + 1], slot);
    embi_js_wr32(&f->code.data[tail + 5], name_k);
    write_op(f, tail + 9u, body, body_operand);
    write_op(f, tail + 14u, EMBI_JSOP_JMP, ret);
    mirror_exc(f, src_pc, tail, tail + 19u);
    mirror_pos(f, src_pc, tail);
    write_op(f, pc, EMBI_JSOP_JMP, tail);
}

/* The binding a name refers to, looking outward from scope s. With cross set, the walk continues
 * through the defining scopes of enclosing functions. When dyn is set, the with scopes and
 * variable environments that are crossed before the binding are pushed as entries of f's dynamic
 * list. Without cross, the walk stops at f (a child's upvalue request is resolved this way; the
 * outer part is the parent's business). Returns nullptr for a name that no scope declares. */
static cbind *walk_ref(cunit *p, cfunc *f, cscope *s, embi_js_value name, bool dyn, bool cross)
{
    cfunc *g = f;
    cscope *sc = s;
    for (;;) {
        for (; sc; sc = sc->parent) {
            if (dyn && sc->kind == SC_WITH) {
                push_dent(f, EMBI_JS_DYN_WITH, sc->first);
            }
            cbind *b = embi_jsc_find_in_scope(sc, name);
            if (b) {
                return b;
            }
        }
        /* The root of g has been searched. An arrow function has no this, new.target or
         * arguments of its own: they are the enclosing function's. */
        if (!g->is_arrow && (name == p->atom_this || name == p->atom_nt || name == p->atom_home ||
                             name == p->atom_fn)) {
            return ensure_implicit(p, g, name);
        }
        if (!embi_jsc_is_top(g) && name != EMBI_JS_EMPTY) {
            if (name == g->self_name) {
                return ensure_callee(p, g);
            }
            if (name == p->atom_arguments && !g->is_arrow) {
                return embi_jsc_ensure_arguments(p, g);
            }
        }
        if (!cross) {
            return nullptr;
        }
        if (dyn && g->ve_bind) {
            push_dent(f, EMBI_JS_DYN_VE, g->ve_bind);
        }
        if (embi_jsc_is_top(g)) {
            if (dyn && g->is_eval && g->chain_bind) {
                push_dent(f, EMBI_JS_DYN_CHAIN, g->chain_bind);
            }
            return nullptr;
        }
        sc = g->def;
        g = g->parent;
    }
}

/* The index of this function's upvalue for name, created when missing. Its scope is where the
 * function is defined. */
static uint32_t upval_index(cunit *p, cfunc *f, embi_js_value name)
{
    (void)p;
    for (uint32_t i = 0; i < f->upvals.len; i++) {
        if (f->upvals.data[i].name == name) {
            return i;
        }
    }
    cupval e = {name, f->def, UPV_UNRESOLVED, nullptr, 0};
    CF_PUSH(f, upvals, e);
    return (uint32_t)f->upvals.len - 1u;
}

/* Gives the entries [first, end) of f's dynamic list their upvalue numbers when their bindings
 * belong to an enclosing function. */
static void fix_dents(cunit *p, cfunc *f, uint32_t first)
{
    for (uint32_t i = first; i < f->dents.len; i++) {
        cdynent *e = &f->dents.data[i];
        if (e->bind && e->bind->owner != f) {
            e->upidx = (int32_t)upval_index(p, f, e->bind->name);
        }
    }
}

static void write_tail(cfunc *f, uint32_t pc, uint32_t src_pc, uint32_t ret,
                       uint8_t check, uint32_t slot, embi_js_value name, uint8_t body,
                       uint32_t body_operand);

static void write_local_op(cfunc *f, uint32_t pc, uint8_t kind, const cbind *b, uint32_t at)
{
    if (b->kind == BK_CALLEE && kind == SYM_KIND_SET) {
        /* The name of a named function expression is an immutable binding. */
        embi_jsc_write_callee_store(f, pc);
        return;
    }
    uint8_t op;
    uint32_t operand;
    local_op(f, b, kind, at, &op, &operand);
    write_op(f, pc, op, operand);
}

static void write_global_op(cfunc *f, uint32_t pc, uint8_t kind, embi_js_value name,
                            const cbind *b)
{
    uint32_t k = const_in(f, name);
    if (kind == SYM_KIND_INIT && b && b->glex) {
        write_op(f, pc, EMBI_JSOP_INIT_GLEX, k);
        return;
    }
    switch (kind) {
    case SYM_KIND_GET:
    case SYM_KIND_CALL:
        write_op(f, pc, EMBI_JSOP_GET_GLOBAL, embi_jsc_global_ic(f, k));
        break;
    case SYM_KIND_TYPEOF:
        write_op(f, pc, EMBI_JSOP_GET_GLOBAL_OPT, embi_jsc_global_ic(f, k));
        break;
    case SYM_KIND_SET:
        write_op(f, pc, EMBI_JSOP_SET_GLOBAL, embi_jsc_global_ic(f, k));
        break;
    case SYM_KIND_DELETE:
        write_op(f, pc, EMBI_JSOP_DELETE_GLOBAL, k);
        break;
    default:
        /* INIT of a global never happens: catch parameters are local. Keep the write observable. */
        write_op(f, pc, EMBI_JSOP_SET_GLOBAL, embi_jsc_global_ic(f, k));
        break;
    }
}

/* Writes the static code of a reference at pc (the op that a dynamic site falls back to). ret is
 * the pc that follows the code (a lexical reference that needs a TDZ check jumps back there). */
static void write_static_sym(cfunc *f, uint32_t pc, const csym *s, uint32_t ret)
{
    if (s->res && s->res->kind == BK_IMPORT) {
        /* J3c: an import reads the exporting module's box through the module holder. A store
         * to an import is a TypeError (the binding is immutable). */
        const cbind *b = s->res;
        switch (s->kind) {
        case SYM_KIND_GET:
        case SYM_KIND_TYPEOF:
        case SYM_KIND_CALL:
        case SYM_KIND_GETREF:
            /* The operand is the constant of the import's token (the holder and the index). */
            write_op(f, pc, EMBI_JSOP_IMPORT_GET, const_in(f, b->import_token));
            break;
        default:
            write_op(f, pc, EMBI_JSOP_CONST_ASSIGN, 0);
            break;
        }
        return;
    }
    if (s->res && !s->res->global) {
        const cbind *b = s->res;
        if (is_lexical(b) && s->kind != SYM_KIND_INIT && s->kind != SYM_KIND_DELETE &&
            needs_tdz(f, s, b)) {
            uint8_t body;
            uint32_t operand;
            local_op(f, b, s->kind, s->pc, &body, &operand);
            write_tail(f, pc, s->pc, ret, b->captured ? EMBI_JSOP_TDZ_BOX : EMBI_JSOP_TDZ_LOC,
                       (uint32_t)b->slot, b->name, body, operand);
            return;
        }
        write_local_op(f, pc, s->kind, b, s->pc);
    } else if (s->res && s->res->global) {
        write_global_op(f, pc, s->kind, s->name, s->res);
    } else if (s->upidx >= 0) {
        uint8_t op;
        switch (s->kind) {
        case SYM_KIND_GET:
        case SYM_KIND_TYPEOF:
        case SYM_KIND_CALL:
            op = EMBI_JSOP_GET_UPVAL;
            break;
        case SYM_KIND_SET:
            op = EMBI_JSOP_SET_UPVAL;
            break;
        case SYM_KIND_DELETE:
            op = EMBI_JSOP_CONST_FALSE;
            break;
        default:
            EMBI_UNREACHABLE();
        }
        write_op(f, pc, op, (uint32_t)s->upidx);
        cupsite site = {pc, (uint32_t)s->upidx, s->name, s->kind};
        CF_PUSH(f, upsites, site);
    } else {
        write_global_op(f, pc, s->kind, s->name, nullptr);
    }
}

static uint8_t dyn_op_for(uint8_t kind)
{
    switch (kind) {
    case SYM_KIND_GET:
        return EMBI_JSOP_DYN_GET;
    case SYM_KIND_CALL:
        return EMBI_JSOP_DYN_CALL;
    case SYM_KIND_TYPEOF:
        return EMBI_JSOP_DYN_TYPEOF;
    case SYM_KIND_SET:
        return EMBI_JSOP_DYN_SET;
    case SYM_KIND_REF:
        return EMBI_JSOP_DYN_REF;
    case SYM_KIND_GETREF:
        return EMBI_JSOP_DYN_GET_REF;
    case SYM_KIND_SETREF:
        return EMBI_JSOP_DYN_SET_REF;
    case SYM_KIND_DELETE:
        return EMBI_JSOP_DYN_DELETE;
    default:
        EMBI_UNREACHABLE();
    }
}

/* Whether a binding of a function is listed in an eval context: declared names, not the
 * compiler's temporaries and not script globals. */
static bool decl_visible(const cbind *b)
{
    if (b->global || b->name == EMBI_JS_EMPTY) {
        return false;
    }
    switch (b->kind) {
    case BK_VAR:
    case BK_PARAM:
    case BK_FUNC:
    case BK_CATCH:
    case BK_CALLEE:
    case BK_ARGUMENTS:
    case BK_LET:
    case BK_CONST:
        return true;
    default:
        return false;
    }
}

/* Lists, for a direct eval at scope s of f, the entries an eval context needs: each with scope
 * (its binding), each declared name of each scope as a DECL group (captured boxes), each sloppy
 * variable environment, and the caller's context when the walk reaches an eval unit's top. */
static void resolve_eval(cunit *p, cfunc *f, ceval *e)
{
    cfunc *g = f;
    cscope *sc = e->scope;
    EMBI_VEC(embi_js_value) seen = {nullptr, 0, 0};
    e->first = f->eents.len;
    /* The lexical names of this function that an eval-declared var may not repeat. */
    {
        uint32_t first_name = f->enames.len;
        uint32_t cnt = 0;
        for (const cscope *s2 = e->scope; s2; s2 = s2->parent) {
            for (const cbind *b = s2->first; b; b = b->next_in_scope) {
                bool lexical = b->kind == BK_LET || b->kind == BK_CONST ||
                               (b->kind == BK_FUNC && s2 != f->root && s2 != f->vroot) ||
                               (e->in_params && b->kind == BK_PARAM);
                if (lexical && b->name != EMBI_JS_EMPTY) {
                    cdynname n;
                    memset(&n, 0, sizeof n);
                    n.name = b->name;
                    n.upidx = -1;
                    CF_PUSH(f, enames, n);
                    cnt++;
                }
            }
        }
        if (cnt > 0) {
            cdynent d;
            memset(&d, 0, sizeof d);
            d.type = EMBI_JS_DYN_DECL_LEX;
            d.upidx = -1;
            d.first_name = first_name;
            d.nnames = cnt;
            CF_PUSH(f, eents, d);
        }
    }
    for (;;) {
        for (; sc; sc = sc->parent) {
            if (sc->kind == SC_WITH) {
                cdynent d;
                memset(&d, 0, sizeof d);
                d.type = EMBI_JS_DYN_WITH;
                d.bind = sc->first;
                d.upidx = -1;
                CF_PUSH(f, eents, d);
                continue;
            }
            uint32_t first_name = f->enames.len;
            uint32_t cnt = 0;
            for (cbind *b = sc->first; b; b = b->next_in_scope) {
                if (!decl_visible(b) && b->kind != BK_IMPORT) {
                    continue;
                }
                bool dup = false;
                for (uint32_t k = 0; k < seen.len; k++) {
                    dup = dup || seen.data[k] == b->name;
                }
                if (dup) {
                    continue;
                }
                embi_vec_push(&seen, b->name);
                if (b->kind == BK_IMPORT) {
                    /* J3c: an import has no storage. The eval context names it, and build_context
                     * reads its box through the module (the token constant). Read-only. */
                    cdynname imp;
                    memset(&imp, 0, sizeof imp);
                    imp.name = b->name;
                    imp.upidx = -1;
                    imp.src = EMBI_JS_SRC_IMPORT;
                    imp.tok = const_in(f, b->import_token);
                    imp.ro = 1;
                    CF_PUSH(f, enames, imp);
                    cnt++;
                    continue;
                }
                cdynname n;
                memset(&n, 0, sizeof n);
                n.name = b->name;
                n.upidx = -1;
                if (b->owner == f) {
                    b->captured = true;
                    n.bind = b;
                } else {
                    n.upidx = (int32_t)upval_index(p, f, b->name);
                }
                n.ro = b->kind == BK_CALLEE;
                CF_PUSH(f, enames, n);
                cnt++;
            }
            if (cnt > 0) {
                cdynent d;
                memset(&d, 0, sizeof d);
                d.type = sc->parent ? EMBI_JS_DYN_DECL : EMBI_JS_DYN_DECL_FN;
                d.upidx = -1;
                d.first_name = first_name;
                d.nnames = cnt;
                CF_PUSH(f, eents, d);
            }
        }
        if (g->ve_bind) {
            cdynent d;
            memset(&d, 0, sizeof d);
            d.type = EMBI_JS_DYN_VE;
            d.bind = g->ve_bind;
            d.upidx = -1;
            CF_PUSH(f, eents, d);
        }
        if (embi_jsc_is_top(g)) {
            if (g->is_eval && g->chain_bind) {
                cdynent d;
                memset(&d, 0, sizeof d);
                d.type = EMBI_JS_DYN_CHAIN;
                d.bind = g->chain_bind;
                d.upidx = -1;
                CF_PUSH(f, eents, d);
            }
            break;
        }
        sc = g->def;
        g = g->parent;
    }
    e->n = f->eents.len - e->first;
    if (f->is_arrow) {
        /* This and new.target of an arrow function are the enclosing function's: the call passes
         * them to the eval code. */
        e->this_up = (int32_t)upval_index(p, f, p->atom_this);
        e->nt_up = (int32_t)upval_index(p, f, p->atom_nt);
        if (e->super_ok) {
            e->home_up = (int32_t)upval_index(p, f, p->atom_home);
        }
    }
    embi_vec_free(&seen);
    for (uint32_t i = e->first; i < f->eents.len; i++) {
        cdynent *d = &f->eents.data[i];
        if (d->bind && d->bind->owner != f) {
            d->upidx = (int32_t)upval_index(p, f, d->bind->name);
        }
    }
}

/* Resolves one SYM op: its binding, or its upvalue, or a global, plus a dynamic site when a
 * with scope or a variable environment is crossed on the way. */
static void resolve_sym(cunit *p, cfunc *f, csym *s)
{
    if (s->direct) {
        s->res = s->direct;
        s->dyn = -1;
        return;
    }
    bool dyn = s->kind != SYM_KIND_INIT;
    uint32_t first = f->dents.len;
    cbind *b = walk_ref(p, f, s->scope, s->name, dyn, true);
    uint32_t n = f->dents.len - first;
    fix_dents(p, f, first);
    if (b) {
        if (b->owner == f || b->kind == BK_IMPORT) {
            /* An import has no storage of its own: every function reads it through the module. */
            s->res = b;
        } else {
            s->upidx = (int32_t)upval_index(p, f, s->name);
        }
    } else if (!embi_jsc_is_top(f)) {
        s->upidx = (int32_t)upval_index(p, f, s->name);
    }
    bool is_ref = s->kind == SYM_KIND_REF || s->kind == SYM_KIND_GETREF ||
                  s->kind == SYM_KIND_SETREF;
    if (n > 0 || is_ref) {
        /* The ref forms always get a site: their stack shape does not depend on whether any
         * dynamic scope is crossed (a site with no entries yields the undefined holder). */
        cdynsite site;
        site.name = const_in(f, s->name);
        site.first = first;
        site.n = n;
        site.fallback = 0;
        site.unres = s->kind == SYM_KIND_REF && f->strict && !b;
        s->dyn = (int32_t)f->dsites.len;
        CF_PUSH(f, dsites, site);
    } else {
        s->dyn = -1;
    }
}

/* Writes the init block and jumps to the body from pc 0. */
static void emit_init_block(cunit *p, cfunc *f)
{
    /* Eval code takes the caller's context from upvalue 0. */
    if (f->chain_bind) {
        embi_jsc_emit_op32(p, EMBI_JSOP_GET_UPVAL, 0);
        embi_jsc_emit_op32(p, EMBI_JSOP_PUT_LOC, (uint32_t)f->chain_bind->slot);
    }
    /* A sloppy function with a direct eval gets its variable environment. */
    if (f->ve_bind) {
        embi_jsc_emit_op(p, EMBI_JSOP_NEW_ENV);
        embi_jsc_emit_op32(p, EMBI_JSOP_PUT_LOC, (uint32_t)f->ve_bind->slot);
    }
    /* The callee name is stored first so that a captured callee can be boxed below. */
    if (f->callee) {
        embi_jsc_emit_op(p, EMBI_JSOP_PUSH_CALLEE);
        embi_jsc_emit_op32(p, EMBI_JSOP_PUT_LOC, (uint32_t)f->callee->slot);
    }
    /* Lexical bindings of the top scopes start uninitialized (boxes are made around EMPTY). */
    for (int sci = 0; sci < 2; sci++) {
        const cscope *sc = sci == 0 ? f->root : f->vroot;
        for (const cbind *b = sc ? sc->first : nullptr; b; b = b->next_in_scope) {
            if (is_lexical(b) && !b->global) {
                embi_jsc_emit_op(p, EMBI_JSOP_PUSH_EMPTY);
                embi_jsc_emit_op32(p, EMBI_JSOP_PUT_LOC, (uint32_t)b->slot);
            }
        }
    }
    for (int sci = 0; sci < 2; sci++) {
        const cscope *sc = sci == 0 ? f->root : f->vroot;
        for (const cbind *b = sc ? sc->first : nullptr; b; b = b->next_in_scope) {
            if (b->captured && !b->global) {
                embi_jsc_emit_op32(p, EMBI_JSOP_MAKE_BOX, (uint32_t)b->slot);
            }
        }
    }
    for (cbind *b = f->root->first; b; b = b->next_in_scope) {
        if (b->kind == BK_IMPLICIT) {
            uint8_t src_op = b->name == p->atom_this   ? EMBI_JSOP_PUSH_THIS
                             : b->name == p->atom_nt   ? EMBI_JSOP_PUSH_NEW_TARGET
                             : b->name == p->atom_home ? EMBI_JSOP_PUSH_HOME
                                                       : EMBI_JSOP_PUSH_CALLEE;
            embi_jsc_emit_op(p, src_op);
            embi_jsc_emit_op32(p, b->captured ? EMBI_JSOP_PUT_BOX : EMBI_JSOP_PUT_LOC,
                               (uint32_t)b->slot);
        }
    }
    /* Every script-level name exists before the body runs. Function names are declared too, so
     * a function declared inside a block (its store happens at the declaration point) has a
     * property to write to. */
    if (f->is_script) {
        /* GlobalDeclarationInstantiation: every collision is reported before anything is declared. */
        for (cbind *b = f->root->first; b; b = b->next_in_scope) {
            if (b->glex) {
                embi_jsc_emit_op2(p, EMBI_JSOP_GLOBAL_CHECK, const_in(f, b->name), 0);
            }
        }
        for (cbind *b = f->root->first; b; b = b->next_in_scope) {
            if (b->global && !b->glex) {
                bool is_fn = false;
                for (uint32_t i = 0; i < f->decls.len; i++) {
                    is_fn = is_fn || f->decls.data[i].bind == b;
                }
                embi_jsc_emit_op2(p, EMBI_JSOP_GLOBAL_CHECK, const_in(f, b->name), is_fn ? 2u : 1u);
            }
        }
        for (cbind *b = f->root->first; b; b = b->next_in_scope) {
            if (b->glex) {
                embi_jsc_emit_op2(p, EMBI_JSOP_DECLARE_GLEX, const_in(f, b->name),
                                  b->kind == BK_CONST ? 1u : 0u);
            }
        }
    }
    for (cbind *b = f->root->first; b; b = b->next_in_scope) {
        if (b->global && !b->glex) {
            uint32_t k = const_in(f, b->name);
            embi_jsc_emit_op32(p, EMBI_JSOP_DECLARE_GLOBAL, k);
        }
    }
    for (uint32_t i = 0; i < f->evalvars.len; i++) {
        bool is_fn = false;
        for (uint32_t j = 0; j < f->decls.len; j++) {
            is_fn = is_fn || (!f->decls.data[j].bind && f->decls.data[j].name == f->evalvars.data[i]);
        }
        embi_jsc_emit_op2(p, EMBI_JSOP_EVAL_CHECK, const_in(f, f->evalvars.data[i]), is_fn ? 2u : 1u);
    }
    for (uint32_t i = 0; i < f->evalvars.len; i++) {
        uint32_t k = const_in(f, f->evalvars.data[i]);
        embi_jsc_emit_op32(p, EMBI_JSOP_EVAL_DECL, k);
    }
    for (uint32_t i = 0; i < f->decls.len; i++) {
        const cdecl *d = &f->decls.data[i];
        embi_jsc_emit_op32(p, EMBI_JSOP_NEW_CLOSURE, d->child->const_idx);
        if (!d->bind) {
            uint32_t k = const_in(f, d->name);
            embi_jsc_emit_op32(p, EMBI_JSOP_EVAL_DEFINE, k);
        } else if (d->bind->global) {
            uint32_t k = const_in(f, d->bind->name);
            embi_jsc_emit_op32(p, EMBI_JSOP_DEFINE_GLOBAL_FN, k);
        } else if (d->bind->captured) {
            embi_jsc_emit_op32(p, EMBI_JSOP_SET_BOX, (uint32_t)d->bind->slot);
            embi_jsc_emit_op(p, EMBI_JSOP_POP);
        } else {
            embi_jsc_emit_op32(p, EMBI_JSOP_PUT_LOC, (uint32_t)d->bind->slot);
        }
    }
    if (f->is_module) {
        /* J3c: the instantiation is done. The module frame is saved and returned to the linker;
         * the evaluation resumes at the JMP_ENTRY that follows (the body starts at pc 5). */
        embi_jsc_emit_op(p, EMBI_JSOP_GEN_START);
    }
    embi_jsc_emit_op32(p, EMBI_JSOP_JMP_ENTRY, 5u);
}

bool embi_jsc_finish_function(cunit *p, cfunc *f)
{
    cfunc *saved = p->fn;
    p->fn = f;
    f->depth = 0;

    /* A body var named like a parameter (separate parameter and body scopes) starts with the
     * parameter's value. A stub past the body copies the values and jumps back to the body start
     * (the parser left a jump there). A function declaration of the same name overrides it. */
    if (f->vroot && f->pcopies.len > 0) {
        uint32_t stub = embi_jsc_here(p);
        for (uint32_t i = 0; i < f->pcopies.len; i++) {
            cbind *vb = f->pcopies.data[i];
            bool overridden = false;
            for (uint32_t j = 0; j < f->decls.len; j++) {
                overridden = overridden || f->decls.data[j].bind == vb;
            }
            cbind *pb = embi_jsc_find_in_scope(f->root, vb->name);
            if (overridden || !pb) {
                continue;
            }
            embi_jsc_emit_sym(p, SYM_KIND_GET, vb->name, f->root, pb);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, vb->name, f->vroot, vb);
        }
        embi_jsc_emit_op32(p, EMBI_JSOP_JMP, f->pcopy_at + 4u);
        embi_jsc_patch(p, f->pcopy_at, stub);
    }

    /* 0. Implicit bindings. */
    if (f->has_eval && !embi_jsc_is_top(f) && f->self_name != EMBI_JS_EMPTY &&
        !embi_jsc_find_in_scope(f->root, f->self_name)) {
        ensure_callee(p, f);
    }

    /* 1. Child requests. */
    for (uint32_t i = 0; i < f->children.len; i++) {
        cfunc *c = f->children.data[i];
        for (uint32_t j = 0; j < c->upvals.len; j++) {
            cupval *e = &c->upvals.data[j];
            if (e->kind != UPV_UNRESOLVED) {
                continue;
            }
            cbind *b = walk_ref(p, f, e->def, e->name, false, false);
            /* A binding that belongs to an enclosing function (a class scope's inner name, for one,
             * is reached from a static block's scope) is the parent's upvalue, not a local of f. */
            if (b && !b->global && b->owner != f) {
                b = nullptr;
            }
            if (b) {
                if (b->global) {
                    e->kind = UPV_GLOBAL;
                } else {
                    e->kind = UPV_LOCAL;
                    e->bind = b;
                    b->captured = true;
                }
            } else if (embi_jsc_is_top(f)) {
                e->kind = UPV_GLOBAL;
            } else {
                e->kind = UPV_PARENT;
                e->index = upval_index(p, f, e->name);
            }
        }
    }

    /* 2. Own references and eval sites. The upvalue list may grow here. */
    for (uint32_t i = 0; i < f->syms.len; i++) {
        resolve_sym(p, f, &f->syms.data[i]);
    }
    for (uint32_t i = 0; i < f->evals.len; i++) {
        resolve_eval(p, f, &f->evals.data[i]);
    }

    /* A sloppy function whose arguments object aliases its parameters boxes each aliased
     * parameter (the object holds the box). */
    f->mapped = !f->strict && !embi_jsc_is_top(f) && f->arguments != nullptr && f->simple;
    if (f->mapped) {
        for (uint32_t i = 0; i < f->pnames.len && i < f->nparams; i++) {
            cbind *b = embi_jsc_find_in_scope(f->root, f->pnames.data[i]);
            if (b && b->kind == BK_PARAM && b->slot == (int32_t)i) {
                b->captured = true;
            }
        }
    }

    /* 3. Storage. */
    uint32_t nlocals = 0;
    if (f->is_module) {
        /* Every named binding of a module's top level is a box, so that an export can share it. */
        for (uint32_t i = 0; i < f->binds.len; i++) {
            cbind *b = f->binds.data[i];
            if (b->owner == f && b->name != EMBI_JS_EMPTY && b->kind != BK_IMPORT) {
                b->captured = true;
            }
        }
    }
    for (uint32_t i = 0; i < f->binds.len; i++) {
        cbind *b = f->binds.data[i];
        if (b->kind == BK_PARAM && !b->ptdz) {
            continue;
        }
        if (b->global || b->kind == BK_IMPORT) {
            b->slot = -1;
            continue;
        }
        b->slot = (int32_t)(f->nparams + nlocals);
        nlocals++;
    }
    f->nlocals = nlocals;
    f->arguments_slot = f->arguments ? (uint32_t)f->arguments->slot : EMBI_JS_NO_SLOT;

    /* 3b. Where each scope entry is read from. */
    for (uint32_t i = 0; i < f->dents.len; i++) {
        cdynent *d = &f->dents.data[i];
        if (d->upidx >= 0) {
            d->src = EMBI_JS_SRC_UPV;
            d->index = (uint32_t)d->upidx;
        } else {
            d->src = d->bind->captured ? EMBI_JS_SRC_BOX : EMBI_JS_SRC_LOC;
            d->index = (uint32_t)d->bind->slot;
        }
    }
    for (uint32_t i = 0; i < f->eents.len; i++) {
        cdynent *d = &f->eents.data[i];
        if (d->type == EMBI_JS_DYN_DECL || d->type == EMBI_JS_DYN_DECL_FN ||
            d->type == EMBI_JS_DYN_DECL_LEX) {
            continue;
        }
        if (d->upidx >= 0) {
            d->src = EMBI_JS_SRC_UPV;
            d->index = (uint32_t)d->upidx;
        } else {
            d->src = d->bind->captured ? EMBI_JS_SRC_BOX : EMBI_JS_SRC_LOC;
            d->index = (uint32_t)d->bind->slot;
        }
    }
    for (uint32_t i = 0; i < f->enames.len; i++) {
        cdynname *n = &f->enames.data[i];
        if (!n->bind && n->upidx < 0) {
            continue; /* a name of a DECL_LEX group: only its text matters */
        }
        if (n->upidx >= 0) {
            n->src = EMBI_JS_SRC_UPV;
            n->index = (uint32_t)n->upidx;
        } else {
            n->src = EMBI_JS_SRC_BOX;
            n->index = (uint32_t)n->bind->slot;
        }
    }

    /* 4. Rewrite the symbolic ops. A dynamic reference keeps its static code after the body. */
    for (uint32_t i = 0; i < f->syms.len; i++) {
        csym *s = &f->syms.data[i];
        if (s->kind == SYM_KIND_REF) {
            write_op(f, s->pc, EMBI_JSOP_DYN_REF, (uint32_t)s->dyn);
            continue;
        }
        csym m = *s;
        if (s->kind == SYM_KIND_GETREF) {
            m.kind = SYM_KIND_GET;
        } else if (s->kind == SYM_KIND_SETREF) {
            m.kind = SYM_KIND_SET;
        }
        if (s->dyn < 0) {
            write_static_sym(f, s->pc, &m, s->pc + 5u);
            continue;
        }
        uint32_t fb = (uint32_t)f->code.len;
        for (uint32_t k = 0; k < 10u; k++) {
            CF_PUSH(f, code, 0);
        }
        /* The fallback sits past the body: it keeps the handlers that cover the reference. */
        mirror_exc(f, s->pc, fb, fb + 10u);
        write_static_sym(f, fb, &m, fb + 5u);
        write_op(f, fb + 5u, EMBI_JSOP_JMP, s->pc + 5u);
        f->dsites.data[s->dyn].fallback = fb;
        write_op(f, s->pc, dyn_op_for(s->kind), (uint32_t)s->dyn);
    }

    /* 5. Init block. Pc 0 jumps here (the parser emitted that jump), and the block ends with a
     * jump back to the body at pc 5. */
    uint32_t init_pc = embi_jsc_here(p);
    embi_jsc_patch(p, 1, init_pc);
    emit_init_block(p, f);
    f->done = true;
    p->fn = saved;
    return true;
}

/* Whether the binding b is provably initialized whenever f runs: the outermost closure of the
 * chain from b's function down to f is created by an expression that follows b's initializer, in
 * a scope that cannot be entered past it (a switch can). A hoisted function (created at entry of
 * a function or block, new_pc 0) can run before the initializer, so it keeps the check. */
static bool tdz_done_at_creation(const cfunc *f, const cbind *b)
{
    const cfunc *c = f;
    while (c->parent && c->parent != b->owner) {
        c = c->parent;
    }
    if (c->parent != b->owner || c->new_pc == 0 || b->init_pc == 0 || b->init_pc == 0xFFFFFFFFu) {
        return false;
    }
    if (b->scope && b->scope->kind == SC_SWITCH) {
        return false;
    }
    return c->new_pc > b->init_pc;
}

/* Makes the upvalue references of f that reach a lexical binding check its initialization (they
 * run in another function, so the check is always needed). Runs at build time, after the
 * resolve pass has linked every upvalue. */
void embi_jsc_tdz_upsites(cfunc *f)
{
    uint32_t n = (uint32_t)f->upsites.len;
    for (uint32_t i = 0; i < n; i++) {
        const cupsite *s = &f->upsites.data[i];
        if (s->kind == SYM_KIND_DELETE) {
            continue;
        }
        const cbind *b = upval_bind_of(f, s->upidx);
        if (!b || !is_lexical(b)) {
            continue;
        }
        if (tdz_done_at_creation(f, b)) {
            if (s->kind == SYM_KIND_SET && b->kind == BK_CONST) {
                /* Initialized for sure, but still a constant: the store throws. */
                write_op(f, s->pc, EMBI_JSOP_CONST_ASSIGN, 0);
            }
            continue;
        }
        uint8_t body;
        uint32_t operand;
        if (s->kind == SYM_KIND_SET) {
            if (b->kind == BK_CONST) {
                body = EMBI_JSOP_CONST_ASSIGN;
                operand = 0;
            } else {
                body = EMBI_JSOP_SET_UPVAL;
                operand = s->upidx;
            }
        } else {
            body = EMBI_JSOP_GET_UPVAL;
            operand = s->upidx;
        }
        write_tail(f, s->pc, s->pc, s->pc + 5u, EMBI_JSOP_TDZ_UPVAL, s->upidx, s->name, body,
                   operand);
    }
}
